/* emit_wasm.c — RiboModule -> wasm binary, direct emission, no toolchain.

   Mapping (design decision D5, architecture.md):
   - Every register r0..r255 is an i64 wasm local (cells are raw 64-bit).
     I/F-ops wrap/reinterpret at use; L/D-ops use the cell directly.
   - Scratch locals: pc (i32, local 256), tmp (i32, local 257), an i64
     scratch (local 258) for L-shift guards.
   - Goto control flow becomes a dispatch loop: one nested block per
     instruction, br_table on pc selects the case; each case ends with
     pc := next and br to the break block. RET/TRAP leave the loop.
   - All internal funcs have signature (i64 x arity) -> i64. Exported funcs
     get thin wrappers with the declared result type (params stay i64 cells).
   - Runtime faults map to wasm traps: division and truncation trap natively,
     shifts get an explicit count guard, TRAP is `unreachable`. */
#include "ribo_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- byte buffer + LEB128 ---------- */

typedef struct { uint8_t *data; size_t len, cap; } Buf;

static int bput(Buf *b, uint8_t v) {
    if (b->len == b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        uint8_t *nd = realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd;
        b->cap = nc;
    }
    b->data[b->len++] = v;
    return 1;
}

static int bwrite(Buf *b, const void *p, size_t n) {
    const uint8_t *c = p;
    while (n--) if (!bput(b, *c++)) return 0;
    return 1;
}

static int bu32(Buf *b, uint32_t v) { /* unsigned LEB128 */
    do {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if (v) byte |= 0x80;
        if (!bput(b, byte)) return 0;
    } while (v);
    return 1;
}

static int bi64(Buf *b, int64_t v) { /* signed LEB128 */
    for (;;) {
        uint8_t byte = v & 0x7F;
        v >>= 7;
        if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
            return bput(b, byte);
        }
        if (!bput(b, byte | 0x80)) return 0;
    }
}

static int bstr(Buf *b, const char *s) {
    return bu32(b, (uint32_t)strlen(s)) && bwrite(b, s, strlen(s));
}

/* ---------- wasm opcodes we use ---------- */

#define W_UNREACHABLE 0x00
#define W_BLOCK  0x02
#define W_LOOP   0x03
#define W_IF     0x04
#define W_END    0x0B
#define W_BR     0x0C
#define W_BRTABLE 0x0E
#define W_RETURN 0x0F
#define W_CALL   0x10
#define W_CALLINDIRECT 0x11
#define W_DROP   0x1A
#define W_LOCALGET  0x20
#define W_LOCALSET  0x21
#define W_GLOBALGET 0x23
#define W_GLOBALSET 0x24
#define W_I32LOAD 0x28
#define W_I64LOAD 0x29
#define W_F32LOAD 0x2A
#define W_F64LOAD 0x2B
#define W_I32LOAD8S 0x2C
#define W_I32LOAD8U 0x2D
#define W_I32LOAD16S 0x2E
#define W_I32LOAD16U 0x2F
#define W_I64LOAD8S 0x30
#define W_I64LOAD8U 0x31
#define W_I64LOAD16S 0x32
#define W_I64LOAD16U 0x33
#define W_I64LOAD32S 0x34
#define W_I64LOAD32U 0x35
#define W_I32STORE 0x36
#define W_I64STORE 0x37
#define W_F32STORE 0x38
#define W_F64STORE 0x39
#define W_I32STORE8 0x3A
#define W_I32STORE16 0x3B
#define W_I64STORE8 0x3C
#define W_I64STORE16 0x3D
#define W_I64STORE32 0x3E
#define W_MEMSIZE 0x3F
#define W_MEMGROW 0x40
#define W_I32CONST 0x41
#define W_I64CONST 0x42

#define W_I32EQZ 0x45
#define W_I32EQ 0x46
#define W_I32NE 0x47
#define W_I32LTS 0x48
#define W_I32LTU 0x49
#define W_I32GTS 0x4A
#define W_I32GTU 0x4B
#define W_I32LES 0x4C
#define W_I32LEU 0x4D
#define W_I32GES 0x4E
#define W_I32GEU 0x4F
#define W_I64EQ 0x51
#define W_I64NE 0x52
#define W_I64LTS 0x53
#define W_I64LTU 0x54
#define W_I64GTS 0x55
#define W_I64GTU 0x56
#define W_I64LES 0x57
#define W_I64LEU 0x58
#define W_I64GES 0x59
#define W_I64GEU 0x5A
#define W_F32EQ 0x5B
#define W_F32NE 0x5C
#define W_F32LT 0x5D
#define W_F32GT 0x5E
#define W_F32LE 0x5F
#define W_F32GE 0x60
#define W_F64EQ 0x61
#define W_F64NE 0x62
#define W_F64LT 0x63
#define W_F64GT 0x64
#define W_F64LE 0x65
#define W_F64GE 0x66

#define W_I32ADD 0x6A
#define W_I32SUB 0x6B
#define W_I32MUL 0x6C
#define W_I32DIVS 0x6D
#define W_I32DIVU 0x6E
#define W_I32REMS 0x6F
#define W_I32REMU 0x70
#define W_I32AND 0x71
#define W_I32OR  0x72
#define W_I32XOR 0x73
#define W_I32CLZ 0x67
#define W_I32CTZ 0x68
#define W_I32POP 0x69
#define W_I32SHL 0x74
#define W_I32SHRS 0x75
#define W_I32SHRU 0x76
#define W_I32ROTL 0x77
#define W_I32ROTR 0x78
#define W_I64ADD 0x7C
#define W_I64SUB 0x7D
#define W_I64MUL 0x7E
#define W_I64DIVS 0x7F
#define W_I64DIVU 0x80
#define W_I64REMS 0x81
#define W_I64REMU 0x82
#define W_I64AND 0x83
#define W_I64OR  0x84
#define W_I64XOR 0x85
#define W_I64CLZ 0x79
#define W_I64CTZ 0x7A
#define W_I64POP 0x7B
#define W_I64SHL 0x86
#define W_I64SHRS 0x87
#define W_I64SHRU 0x88
#define W_I64ROTL 0x89
#define W_I64ROTR 0x8A

#define W_F32ABS 0x8B
#define W_F32NEG 0x8C
#define W_F32SQRT 0x91
#define W_F32ADD 0x92
#define W_F32SUB 0x93
#define W_F32MUL 0x94
#define W_F32DIV 0x95
#define W_F32MIN 0x96
#define W_F32MAX 0x97
#define W_F64ABS 0x99
#define W_F64NEG 0x9A
#define W_F64SQRT 0x9F
#define W_F64ADD 0xA0
#define W_F64SUB 0xA1
#define W_F64MUL 0xA2
#define W_F64DIV 0xA3
#define W_F64MIN 0xA4
#define W_F64MAX 0xA5

#define W_I32WRAP  0xA7
#define W_I32TRUNC_F32S 0xA8
#define W_I32TRUNC_F32U 0xA9
#define W_I32TRUNC_F64S 0xAA
#define W_I32TRUNC_F64U 0xAB
#define W_I64EXTS  0xAC
#define W_I64EXTU  0xAD
#define W_I64TRUNC_F32S 0xAE
#define W_I64TRUNC_F32U 0xAF
#define W_I64TRUNC_F64S 0xB0
#define W_I64TRUNC_F64U 0xB1
#define W_F32CONV_I32S 0xB2
#define W_F32CONV_I64S 0xB4
#define W_F32DEMOTE  0xB6
#define W_F64CONV_I32S 0xB8
#define W_F64CONV_I64S 0xBA
#define W_F64PROMOTE 0xBB
#define W_I32REINT_F32 0xBC
#define W_I64REINT_F64 0xBD
#define W_F32REINT_I32 0xBE
#define W_F64REINT_I64 0xBF

#define VT_F64 0x7C
#define VT_F32 0x7D
#define VT_I64 0x7E
#define VT_I32 0x7F

#define PC_LOCAL 256u
#define TMP_LOCAL 257u
#define TMP64_LOCAL 258u

/* ---------- signature table ---------- */

typedef struct {
    uint8_t params[RIBO_MAX_ARGS];
    uint8_t nparams;
    uint8_t result; /* 0 = none */
} Sig;

typedef struct {
    Sig *sigs;
    size_t count, cap;
} SigTab;

static int sig_add(SigTab *t, const Sig *s) {
    size_t i;
    for (i = 0; i < t->count; i++)
        if (t->sigs[i].nparams == s->nparams && t->sigs[i].result == s->result &&
            !memcmp(t->sigs[i].params, s->params, s->nparams))
            return (int)i;
    if (t->count == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 16;
        Sig *ns = realloc(t->sigs, nc * sizeof *ns);
        if (!ns) return -1;
        t->sigs = ns;
        t->cap = nc;
    }
    t->sigs[t->count] = *s;
    return (int)t->count++;
}

static uint8_t ret_valtype(int t) {
    switch (t) {
    case T_I32: return VT_I32;
    case T_I64: return VT_I64;
    case T_F32: return VT_F32;
    case T_F64: return VT_F64;
    default: return 0;
    }
}

/* ---------- cell helpers ---------- */

static void get_reg(Buf *b, uint32_t r) { bput(b, W_LOCALGET); bu32(b, r); }
static void set_reg(Buf *b, uint32_t r) { bput(b, W_LOCALSET); bu32(b, r); }
static void get_i32(Buf *b, uint32_t r) { get_reg(b, r); bput(b, W_I32WRAP); }
static void get_f32(Buf *b, uint32_t r) { get_i32(b, r); bput(b, W_F32REINT_I32); }
static void get_f64(Buf *b, uint32_t r) { get_reg(b, r); bput(b, W_F64REINT_I64); }
static void set_from_i32(Buf *b, uint32_t r) { bput(b, W_I64EXTS); set_reg(b, r); }
static void set_from_f32(Buf *b, uint32_t r) { bput(b, W_I32REINT_F32); bput(b, W_I64EXTU); set_reg(b, r); }
static void set_from_f64(Buf *b, uint32_t r) { bput(b, W_I64REINT_F64); set_reg(b, r); }

enum { C_N, C_I, C_L, C_F, C_D }; /* matches parse.c's classes */

static void push_src(Buf *b, const RiboInstr *in, int cls) {
    if (in->src_is_lit) {
        bput(b, W_I64CONST);
        bi64(b, in->imm);
    } else {
        get_reg(b, in->src);
    }
    switch (cls) {
    case C_I: bput(b, W_I32WRAP); break;
    case C_F: bput(b, W_I32WRAP); bput(b, W_F32REINT_I32); break;
    case C_D: bput(b, W_F64REINT_I64); break;
    default: break;
    }
}

static void push_dst(Buf *b, uint8_t reg, int cls) {
    switch (cls) {
    case C_I: get_i32(b, reg); break;
    case C_F: get_f32(b, reg); break;
    case C_D: get_f64(b, reg); break;
    default: get_reg(b, reg); break;
    }
}

static void store_result(Buf *b, uint8_t reg, int cls) {
    switch (cls) {
    case C_I: set_from_i32(b, reg); break;
    case C_F: set_from_f32(b, reg); break;
    case C_D: set_from_f64(b, reg); break;
    default: set_reg(b, reg); break;
    }
}

static int op_class(uint16_t op) {
    if (op >= OP_IADD && op <= OP_ISAR) return C_I;
    if (op >= OP_LADD && op <= OP_LSAR) return C_L;
    if (op >= OP_FADD && op <= OP_FNEG) return C_F;
    if (op >= OP_DADD && op <= OP_DNEG) return C_D;
    if (op >= OP_IEQ && op <= OP_IGEU) return C_I;
    if (op >= OP_LEQ && op <= OP_LGEU) return C_L;
    if (op >= OP_FEQ && op <= OP_FGE) return C_F;
    if (op >= OP_DEQ && op <= OP_DGE) return C_D;
    return C_N;
}

/* ---------- per-instruction emission ---------- */

static void emit_set_pc_br(Buf *b, uint32_t target, uint32_t depth) {
    bput(b, W_I32CONST);
    bi64(b, target);
    set_reg(b, PC_LOCAL);
    bput(b, W_BR);
    bu32(b, depth);
}

static uint8_t arith_opcode(uint16_t op) {
    switch (op) {
    case OP_IADD: return W_I32ADD; case OP_ISUB: return W_I32SUB;
    case OP_IMUL: return W_I32MUL; case OP_IDIV: return W_I32DIVS;
    case OP_IMOD: return W_I32REMS; case OP_IDIVU: return W_I32DIVU;
    case OP_IMODU: return W_I32REMU; case OP_IAND: return W_I32AND;
    case OP_IOR: return W_I32OR; case OP_IXOR: return W_I32XOR;
    case OP_LADD: return W_I64ADD; case OP_LSUB: return W_I64SUB;
    case OP_LMUL: return W_I64MUL; case OP_LDIV: return W_I64DIVS;
    case OP_LMOD: return W_I64REMS; case OP_LDIVU: return W_I64DIVU;
    case OP_LMODU: return W_I64REMU; case OP_LAND: return W_I64AND;
    case OP_LOR: return W_I64OR; case OP_LXOR: return W_I64XOR;
    case OP_FADD: return W_F32ADD; case OP_FSUB: return W_F32SUB;
    case OP_FMUL: return W_F32MUL; case OP_FDIV: return W_F32DIV;
    case OP_FMIN: return W_F32MIN; case OP_FMAX: return W_F32MAX;
    case OP_DADD: return W_F64ADD; case OP_DSUB: return W_F64SUB;
    case OP_DMUL: return W_F64MUL; case OP_DDIV: return W_F64DIV;
    case OP_DMIN: return W_F64MIN; case OP_DMAX: return W_F64MAX;
    default: return 0;
    }
}

static uint8_t cmp_opcode(uint16_t op) {
    switch (op) {
    case OP_IEQ: return W_I32EQ; case OP_INE: return W_I32NE;
    case OP_ILT: return W_I32LTS; case OP_ILE: return W_I32LES;
    case OP_IGT: return W_I32GTS; case OP_IGE: return W_I32GES;
    case OP_ILTU: return W_I32LTU; case OP_ILEU: return W_I32LEU;
    case OP_IGTU: return W_I32GTU; case OP_IGEU: return W_I32GEU;
    case OP_LEQ: return W_I64EQ; case OP_LNE: return W_I64NE;
    case OP_LLT: return W_I64LTS; case OP_LLE: return W_I64LES;
    case OP_LGT: return W_I64GTS; case OP_LGE: return W_I64GES;
    case OP_LLTU: return W_I64LTU; case OP_LLEU: return W_I64LEU;
    case OP_LGTU: return W_I64GTU; case OP_LGEU: return W_I64GEU;
    case OP_FEQ: return W_F32EQ; case OP_FNE: return W_F32NE;
    case OP_FLT: return W_F32LT; case OP_FLE: return W_F32LE;
    case OP_FGT: return W_F32GT; case OP_FGE: return W_F32GE;
    case OP_DEQ: return W_F64EQ; case OP_DNE: return W_F64NE;
    case OP_DLT: return W_F64LT; case OP_DLE: return W_F64LE;
    case OP_DGT: return W_F64GT; case OP_DGE: return W_F64GE;
    default: return 0;
    }
}

static uint8_t mem_opcode(uint16_t op) {
    switch (op) {
    case OP_IGET: return W_I32LOAD; case OP_LGET: return W_I64LOAD;
    case OP_FGET: return W_F32LOAD; case OP_DGET: return W_F64LOAD;
    case OP_IGET8S: return W_I32LOAD8S; case OP_IGET8U: return W_I32LOAD8U;
    case OP_IGET16S: return W_I32LOAD16S; case OP_IGET16U: return W_I32LOAD16U;
    case OP_LGET8S: return W_I64LOAD8S; case OP_LGET8U: return W_I64LOAD8U;
    case OP_LGET16S: return W_I64LOAD16S; case OP_LGET16U: return W_I64LOAD16U;
    case OP_LGET32S: return W_I64LOAD32S; case OP_LGET32U: return W_I64LOAD32U;
    case OP_IPUT: return W_I32STORE; case OP_LPUT: return W_I64STORE;
    case OP_FPUT: return W_F32STORE; case OP_DPUT: return W_F64STORE;
    case OP_IPUT8: return W_I32STORE8; case OP_IPUT16: return W_I32STORE16;
    case OP_LPUT8: return W_I64STORE8; case OP_LPUT16: return W_I64STORE16;
    case OP_LPUT32: return W_I64STORE32;
    default: return 0;
    }
}

static void emit_memarg(Buf *b, int64_t off) {
    bu32(b, 0);                    /* align hint 0 */
    bu32(b, (uint32_t)off);        /* static offset, wasm semantics */
}

typedef struct {
    const RiboModule *m;
    uint32_t brk_depth; /* depth of the dispatch break block from the case */
    const int *calli_sig; /* sig index per arity, -1 when unused */
} Ctx;

static void emit_instr(Buf *b, const RiboInstr *in, const Ctx *cx) {
    uint16_t op = in->op;
    int cls = op_class(op);

    if (op == OP_ILIT || op == OP_LLIT || op == OP_DLIT) {
        bput(b, W_I64CONST);
        bi64(b, in->imm);
        set_reg(b, in->dst);
        return;
    }
    if (op == OP_FLIT) {
        bput(b, W_I64CONST);
        bi64(b, (uint32_t)in->imm);
        set_reg(b, in->dst);
        return;
    }
    if (op == OP_MOV) { get_reg(b, in->src); set_reg(b, in->dst); return; }

    /* binary arithmetic (shifts handled separately: they need guards) */
    if ((op >= OP_IADD && op <= OP_IXOR) ||
        (op >= OP_LADD && op <= OP_LXOR) ||
        (op >= OP_FADD && op <= OP_FMAX) || (op >= OP_DADD && op <= OP_DMAX)) {
        push_dst(b, in->dst, cls);
        push_src(b, in, cls);
        bput(b, arith_opcode(op));
        store_result(b, in->dst, cls);
        return;
    }

    /* shifts: wasm masks the count, we fault outside 0..31/0..63 — guard */
    if (op == OP_ISHL || op == OP_ISHR || op == OP_ISAR) {
        push_dst(b, in->dst, C_I);
        if (in->src_is_lit) {
            bput(b, W_I32CONST); bi64(b, in->imm);
        } else {
            get_i32(b, in->src);
            bput(b, W_LOCALSET); bu32(b, TMP_LOCAL);
            get_reg(b, TMP_LOCAL);
            bput(b, W_I32CONST); bi64(b, 31);
            bput(b, W_I32GTU);
            bput(b, W_IF); bput(b, 0x40);
            bput(b, W_UNREACHABLE);
            bput(b, W_END);
            get_reg(b, TMP_LOCAL);
        }
        bput(b, op == OP_ISHL ? W_I32SHL : op == OP_ISHR ? W_I32SHRU : W_I32SHRS);
        store_result(b, in->dst, C_I);
        return;
    }
    if (op == OP_LSHL || op == OP_LSHR || op == OP_LSAR) {
        push_dst(b, in->dst, C_L);
        if (in->src_is_lit) {
            bput(b, W_I64CONST); bi64(b, in->imm);
        } else {
            get_reg(b, in->src);
            set_reg(b, TMP64_LOCAL);
            get_reg(b, TMP64_LOCAL);
            bput(b, W_I64CONST); bi64(b, 63);
            bput(b, W_I64GTU);
            bput(b, W_IF); bput(b, 0x40);
            bput(b, W_UNREACHABLE);
            bput(b, W_END);
            get_reg(b, TMP64_LOCAL);
        }
        bput(b, op == OP_LSHL ? W_I64SHL : op == OP_LSHR ? W_I64SHRU : W_I64SHRS);
        set_reg(b, in->dst);
        return;
    }

    /* rotations: wasm masks the count itself — no guard needed */
    if (op == OP_IROTL || op == OP_IROTR) {
        push_dst(b, in->dst, C_I);
        push_src(b, in, C_I);
        bput(b, op == OP_IROTL ? W_I32ROTL : W_I32ROTR);
        store_result(b, in->dst, C_I);
        return;
    }
    if (op == OP_LROTL || op == OP_LROTR) {
        push_dst(b, in->dst, C_L);
        push_src(b, in, C_L);
        bput(b, op == OP_LROTL ? W_I64ROTL : W_I64ROTR);
        set_reg(b, in->dst);
        return;
    }

    /* bit counting */
    if (op >= OP_ICLZ && op <= OP_IPOP) {
        get_i32(b, in->dst);
        bput(b, op == OP_ICLZ ? W_I32CLZ : op == OP_ICTZ ? W_I32CTZ : W_I32POP);
        set_from_i32(b, in->dst);
        return;
    }
    if (op >= OP_LCLZ && op <= OP_LPOP) {
        get_reg(b, in->dst);
        bput(b, op == OP_LCLZ ? W_I64CLZ : op == OP_LCTZ ? W_I64CTZ : W_I64POP);
        set_reg(b, in->dst);
        return;
    }

    /* float unaries */
    if (op >= OP_FSQRT && op <= OP_FNEG) {
        get_f32(b, in->dst);
        bput(b, op == OP_FSQRT ? W_F32SQRT : op == OP_FABS ? W_F32ABS : W_F32NEG);
        set_from_f32(b, in->dst);
        return;
    }
    if (op >= OP_DSQRT && op <= OP_DNEG) {
        get_f64(b, in->dst);
        bput(b, op == OP_DSQRT ? W_F64SQRT : op == OP_DABS ? W_F64ABS : W_F64NEG);
        set_from_f64(b, in->dst);
        return;
    }

    /* comparisons -> i32 0/1, stored as an i64 cell */
    if (op >= OP_IEQ && op <= OP_DGE) {
        push_dst(b, in->dst, cls);
        push_src(b, in, cls);
        bput(b, cmp_opcode(op));
        bput(b, W_I64EXTU);
        set_reg(b, in->dst);
        return;
    }

    /* conversions */
    switch (op) {
    case OP_L2I: get_i32(b, in->dst); set_from_i32(b, in->dst); return;
    case OP_I2L: get_i32(b, in->dst); set_from_i32(b, in->dst); return;
    case OP_I2LU: get_i32(b, in->dst); bput(b, W_I64EXTU); set_reg(b, in->dst); return;
    case OP_F2I: get_f32(b, in->dst); bput(b, W_I32TRUNC_F32S); set_from_i32(b, in->dst); return;
    case OP_F2L: get_f32(b, in->dst); bput(b, W_I64TRUNC_F32S); set_reg(b, in->dst); return;
    case OP_D2I: get_f64(b, in->dst); bput(b, W_I32TRUNC_F64S); set_from_i32(b, in->dst); return;
    case OP_D2L: get_f64(b, in->dst); bput(b, W_I64TRUNC_F64S); set_reg(b, in->dst); return;
    case OP_F2IU: get_f32(b, in->dst); bput(b, W_I32TRUNC_F32U); set_from_i32(b, in->dst); return;
    case OP_F2LU: get_f32(b, in->dst); bput(b, W_I64TRUNC_F32U); set_reg(b, in->dst); return;
    case OP_D2IU: get_f64(b, in->dst); bput(b, W_I32TRUNC_F64U); set_from_i32(b, in->dst); return;
    case OP_D2LU: get_f64(b, in->dst); bput(b, W_I64TRUNC_F64U); set_reg(b, in->dst); return;
    case OP_I2F: get_i32(b, in->dst); bput(b, W_F32CONV_I32S); set_from_f32(b, in->dst); return;
    case OP_L2F: get_reg(b, in->dst); bput(b, W_F32CONV_I64S); set_from_f32(b, in->dst); return;
    case OP_I2D: get_i32(b, in->dst); bput(b, W_F64CONV_I32S); set_from_f64(b, in->dst); return;
    case OP_L2D: get_reg(b, in->dst); bput(b, W_F64CONV_I64S); set_from_f64(b, in->dst); return;
    case OP_F2D: get_f32(b, in->dst); bput(b, W_F64PROMOTE); set_from_f64(b, in->dst); return;
    case OP_D2F: get_f64(b, in->dst); bput(b, W_F32DEMOTE); set_from_f32(b, in->dst); return;
    default: break;
    }

    /* memory loads */
    if (op >= OP_IGET && op <= OP_LGET32U) {
        get_i32(b, in->src);           /* base address */
        bput(b, mem_opcode(op));
        emit_memarg(b, in->imm);
        if (op == OP_IGET || (op >= OP_IGET8S && op <= OP_IGET16U))
            set_from_i32(b, in->dst);  /* wasm extends into i32 already */
        else if (op == OP_FGET)
            set_from_f32(b, in->dst);
        else if (op == OP_DGET)
            set_from_f64(b, in->dst);
        else
            set_reg(b, in->dst);       /* i64 loads arrive extended */
        return;
    }
    /* memory stores */
    if (op >= OP_IPUT && op <= OP_LPUT32) {
        get_i32(b, in->dst);           /* base address */
        if (op == OP_FPUT) get_f32(b, in->src);
        else if (op == OP_DPUT) get_f64(b, in->src);
        else if (op == OP_IPUT || op == OP_IPUT8 || op == OP_IPUT16) get_i32(b, in->src);
        else get_reg(b, in->src);
        bput(b, mem_opcode(op));
        emit_memarg(b, in->imm);
        return;
    }
    if (op == OP_MSIZE) {
        bput(b, W_MEMSIZE); bput(b, 0);
        bput(b, W_I64EXTU);
        set_reg(b, in->dst);
        return;
    }
    if (op == OP_MGROW) {
        get_i32(b, in->src);
        bput(b, W_MEMGROW); bput(b, 0);
        set_from_i32(b, in->dst);
        return;
    }

    if (op == OP_GGET) { bput(b, W_GLOBALGET); bu32(b, (uint32_t)in->imm); set_reg(b, in->dst); return; }
    if (op == OP_GSET) { get_reg(b, in->src); bput(b, W_GLOBALSET); bu32(b, (uint32_t)in->imm); return; }

    if (op == OP_JMP) {
        emit_set_pc_br(b, (uint32_t)in->imm, cx->brk_depth);
        return;
    }
    if (op == OP_JZ || op == OP_JNZ) {
        get_i32(b, in->dst);
        if (op == OP_JZ) bput(b, W_I32EQZ);
        bput(b, W_IF); bput(b, 0x40);
        emit_set_pc_br(b, (uint32_t)in->imm, cx->brk_depth + 1);
        bput(b, W_END);
        return; /* fall-through pc set by the case wrapper */
    }
    if (op == OP_CALL) {
        size_t idx = (size_t)in->imm;
        size_t i;
        if (idx < cx->m->func_count) {
            for (i = 0; i < in->nargs; i++) get_reg(b, in->args[i]);
            bput(b, W_CALL);
            bu32(b, (uint32_t)(cx->m->import_count + idx));
            set_reg(b, 0);
        } else {
            const RiboImportDecl *im = &cx->m->imports[idx - cx->m->func_count];
            for (i = 0; i < in->nargs; i++) get_reg(b, in->args[i]); /* ABI: i64 cells */
            bput(b, W_CALL);
            bu32(b, (uint32_t)(idx - cx->m->func_count));
            /* all imports transport results as i64 cells; void gets a 0 */
            if (im->ret == T_VOID) {
                /* discard nothing: a void wasm func pushes nothing */
                bput(b, W_I64CONST); bi64(b, 0);
            }
            set_reg(b, 0);
            (void)im;
        }
        return;
    }
    if (op == OP_CALLI) {
        size_t i;
        for (i = 0; i < in->nargs; i++) get_reg(b, in->args[i]);
        get_i32(b, in->dst);           /* the table index goes last */
        bput(b, W_CALLINDIRECT);
        bu32(b, (uint32_t)cx->calli_sig[in->nargs]);
        bput(b, 0x00);                 /* table 0 */
        set_reg(b, 0);
        return;
    }
    if (op == OP_RET) {
        if (in->nargs) get_reg(b, in->dst);
        else { bput(b, W_I64CONST); bi64(b, 0); }
        bput(b, W_RETURN);
        return;
    }
    /* OP_TRAP */
    bput(b, W_UNREACHABLE);
}

/* ---------- whole-function emission ---------- */

static void emit_func_body(Buf *b, const RiboModule *m, const RiboFunc *f,
                           const int *calli_sig) {
    Buf body = {0};
    size_t n = f->code_len, i;

    /* locals: (256 - arity) x i64, then pc + tmp (i32), then i64 scratch */
    bu32(&body, 3);
    bu32(&body, (uint32_t)(256 - f->arity));
    bput(&body, VT_I64);
    bu32(&body, 2);
    bput(&body, VT_I32);
    bu32(&body, 1);
    bput(&body, VT_I64);

    /* dispatch loop */
    bput(&body, W_LOOP); bput(&body, 0x40);  /* $top */
    bput(&body, W_BLOCK); bput(&body, 0x40); /* $break */
    for (i = 0; i < n; i++) { bput(&body, W_BLOCK); bput(&body, 0x40); }
    get_reg(&body, PC_LOCAL);
    bput(&body, W_BRTABLE);
    bu32(&body, (uint32_t)n);
    for (i = 0; i < n; i++) bu32(&body, (uint32_t)i);
    bu32(&body, (uint32_t)n);                /* default: $break */
    for (i = 0; i < n; i++) {
        Ctx cx;
        const RiboInstr *in = &f->code[i];
        bput(&body, W_END);                  /* close case block i */
        cx.m = m;
        cx.brk_depth = (uint32_t)(n - 1 - i);
        cx.calli_sig = calli_sig;
        emit_instr(&body, in, &cx);
        if (in->op == OP_RET || in->op == OP_TRAP || in->op == OP_JMP) {
            /* control already left the case */
        } else {
            emit_set_pc_br(&body, (uint32_t)(i + 1), (uint32_t)(n - 1 - i));
        }
    }
    bput(&body, W_END);                      /* close $break */
    bput(&body, W_BR); bu32(&body, 0);       /* br $top */
    bput(&body, W_END);                      /* close $top */
    bput(&body, W_UNREACHABLE);              /* the loop never falls through */
    bput(&body, W_END);                      /* func end */

    bu32(b, (uint32_t)body.len);
    bwrite(b, body.data, body.len);
    free(body.data);
}

/* export wrapper: (i64 x arity) -> declared result type */
static void emit_wrapper_body(Buf *b, const RiboFunc *f, uint32_t internal_idx) {
    Buf body = {0};
    uint32_t k;
    bu32(&body, 0);                          /* no extra locals */
    for (k = 0; k < f->arity; k++) { bput(&body, W_LOCALGET); bu32(&body, k); }
    bput(&body, W_CALL);
    bu32(&body, internal_idx);
    switch (f->ret) {
    case T_I32: bput(&body, W_I32WRAP); break;
    case T_F32: bput(&body, W_I32WRAP); bput(&body, W_F32REINT_I32); break;
    case T_F64: bput(&body, W_F64REINT_I64); break;
    case T_VOID: bput(&body, W_DROP); break;
    default: break;                          /* T_I64: as is */
    }
    bput(&body, W_END);
    bu32(b, (uint32_t)body.len);
    bwrite(b, body.data, body.len);
    free(body.data);
}

/* ---------- module emission ---------- */

static int emit_module(const RiboModule *m, Buf *out) {
    SigTab tab = {0};
    int *internal_sig = NULL, *wrapper_sig = NULL, *import_sig = NULL;
    int calli_sig[RIBO_MAX_ARGS + 1];
    size_t i, j, k, nfuncs = m->func_count, nimp = m->import_count;
    size_t nexp = 0;
    Buf sec = {0};
    int ok = 0;

    for (i = 0; i <= RIBO_MAX_ARGS; i++) calli_sig[i] = -1;

    for (i = 0; i < nfuncs; i++) if (m->funcs[i].exported) nexp++;

    internal_sig = malloc((nfuncs ? nfuncs : 1) * sizeof *internal_sig);
    wrapper_sig = malloc((nexp ? nexp : 1) * sizeof *wrapper_sig);
    import_sig = malloc((nimp ? nimp : 1) * sizeof *import_sig);
    if (!internal_sig || !wrapper_sig || !import_sig) goto done;

    /* imports: all params are i64 cells, result i64 (void -> i64 dummy so
       CALL can always store r0 uniformly... no — wasm void imports push
       nothing, the CALL site pushes a 0 cell itself) */
    for (i = 0; i < nimp; i++) {
        Sig s;
        memset(&s, 0, sizeof s);
        s.nparams = m->imports[i].arity;
        for (k = 0; k < s.nparams; k++) s.params[k] = VT_I64;
        s.result = (m->imports[i].ret == T_VOID) ? 0 : VT_I64;
        import_sig[i] = sig_add(&tab, &s);
        if (import_sig[i] < 0) goto done;
    }
    for (i = 0; i < nfuncs; i++) {
        Sig s;
        memset(&s, 0, sizeof s);
        s.nparams = m->funcs[i].arity;
        for (k = 0; k < s.nparams; k++) s.params[k] = VT_I64;
        s.result = VT_I64;
        internal_sig[i] = sig_add(&tab, &s);
        if (internal_sig[i] < 0) goto done;
    }
    {
        size_t w = 0;
        for (i = 0; i < nfuncs; i++) {
            Sig s;
            if (!m->funcs[i].exported) continue;
            memset(&s, 0, sizeof s);
            s.nparams = m->funcs[i].arity;
            for (k = 0; k < s.nparams; k++) s.params[k] = VT_I64;
            s.result = ret_valtype(m->funcs[i].ret);
            wrapper_sig[w++] = sig_add(&tab, &s);
            if (wrapper_sig[w - 1] < 0) goto done;
        }
    }
    /* call_indirect signatures: (i64 x nargs) -> i64 for every used arity */
    for (i = 0; i < nfuncs; i++)
        for (j = 0; j < m->funcs[i].code_len; j++)
            if (m->funcs[i].code[j].op == OP_CALLI) {
                size_t n = m->funcs[i].code[j].nargs;
                if (calli_sig[n] < 0) {
                    Sig s;
                    memset(&s, 0, sizeof s);
                    s.nparams = (uint8_t)n;
                    for (k = 0; k < n; k++) s.params[k] = VT_I64;
                    s.result = VT_I64;
                    calli_sig[n] = sig_add(&tab, &s);
                    if (calli_sig[n] < 0) goto done;
                }
            }

    bwrite(out, "\0asm\x01\0\0\0", 8);

    /* 1. type section */
    bu32(&sec, (uint32_t)tab.count);
    for (i = 0; i < tab.count; i++) {
        bput(&sec, 0x60);
        bu32(&sec, tab.sigs[i].nparams);
        for (k = 0; k < tab.sigs[i].nparams; k++) bput(&sec, tab.sigs[i].params[k]);
        if (tab.sigs[i].result) { bu32(&sec, 1); bput(&sec, tab.sigs[i].result); }
        else bu32(&sec, 0);
    }
    bput(out, 1); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
    sec.len = 0;

    /* 2. import section */
    if (nimp) {
        bu32(&sec, (uint32_t)nimp);
        for (i = 0; i < nimp; i++) {
            const char *dot = strchr(m->imports[i].name, '.');
            char modname[64];
            const char *field = m->imports[i].name;
            if (dot) {
                size_t ml = (size_t)(dot - m->imports[i].name);
                if (ml > 63) ml = 63;
                memcpy(modname, m->imports[i].name, ml);
                modname[ml] = '\0';
                field = dot + 1;
            } else {
                memcpy(modname, "env", 4);
            }
            bstr(&sec, modname);
            bstr(&sec, field);
            bput(&sec, 0x00);
            bu32(&sec, (uint32_t)import_sig[i]);
        }
        bput(out, 2); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    /* 3. function section */
    bu32(&sec, (uint32_t)(nfuncs + nexp));
    for (i = 0; i < nfuncs; i++) bu32(&sec, (uint32_t)internal_sig[i]);
    {
        size_t w = 0;
        for (i = 0; i < nfuncs; i++)
            if (m->funcs[i].exported) bu32(&sec, (uint32_t)wrapper_sig[w++]);
    }
    bput(out, 3); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
    sec.len = 0;

    /* 4. table section (funcref, wasm MVP: at most one) */
    if (m->has_table) {
        bu32(&sec, 1);
        bput(&sec, 0x70); /* funcref */
        if (m->table_max) {
            bput(&sec, 0x01);
            bu32(&sec, m->table_min);
            bu32(&sec, m->table_max);
        } else {
            bput(&sec, 0x00);
            bu32(&sec, m->table_min);
        }
        bput(out, 4); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    /* 5. memory section */
    if (m->has_mem) {
        bu32(&sec, 1);
        if (m->mem_max) {
            bput(&sec, 0x01);
            bu32(&sec, m->mem_min);
            bu32(&sec, m->mem_max);
        } else {
            bput(&sec, 0x00);
            bu32(&sec, m->mem_min);
        }
        bput(out, 5); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    /* 6. global section (cells are i64, zero-initialized) */
    if (m->glob_count) {
        bu32(&sec, (uint32_t)m->glob_count);
        for (i = 0; i < m->glob_count; i++) {
            bput(&sec, VT_I64);
            bput(&sec, 0x01);
            bput(&sec, W_I64CONST);
            bi64(&sec, (int64_t)m->globals[i].bits); /* GINIT-honoring init */
            bput(&sec, W_END);
        }
        bput(out, 6); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    /* 7. export section */
    {
        size_t w = 0;
        bu32(&sec, (uint32_t)(nexp + (m->has_mem ? 1 : 0)));
        for (i = 0; i < nfuncs; i++) {
            if (!m->funcs[i].exported) continue;
            bstr(&sec, m->funcs[i].name);
            bput(&sec, 0x00);
            bu32(&sec, (uint32_t)(nimp + nfuncs + w));
            w++;
        }
        if (m->has_mem) {
            bstr(&sec, "memory");
            bput(&sec, 0x02);
            bu32(&sec, 0);
        }
    }
    bput(out, 7); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
    sec.len = 0;

    /* 9. element section (active funcref segments; contiguous runs merge) */
    if (m->elem_count) {
        size_t nseg = 0;
        for (i = 0; i < m->elem_count; i++)
            if (i == 0 || m->elems[i].off != m->elems[i - 1].off + 1) nseg++;
        bu32(&sec, (uint32_t)nseg);
        for (i = 0; i < m->elem_count;) {
            size_t len = 1;
            while (i + len < m->elem_count &&
                   m->elems[i + len].off == m->elems[i + len - 1].off + 1)
                len++;
            bput(&sec, 0x00);                    /* active, table 0 */
            bput(&sec, W_I32CONST);
            bi64(&sec, (int32_t)m->elems[i].off);
            bput(&sec, W_END);
            bu32(&sec, (uint32_t)len);
            for (j = 0; j < len; j++) {
                uint32_t fn = m->elems[i + j].func;
                /* wasm funcidx space: imports first, then internal funcs */
                bu32(&sec, fn < nfuncs ? (uint32_t)(nimp + fn)
                                       : (uint32_t)(fn - nfuncs));
            }
            i += len;
        }
        bput(out, 9); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    /* 10. code section */
    bu32(&sec, (uint32_t)(nfuncs + nexp));
    for (i = 0; i < nfuncs; i++) emit_func_body(&sec, m, &m->funcs[i], calli_sig);
    for (i = 0; i < nfuncs; i++)
        if (m->funcs[i].exported)
            emit_wrapper_body(&sec, &m->funcs[i], (uint32_t)(nimp + i));
    bput(out, 10); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
    sec.len = 0;

    /* 11. data section (active segments into memory 0) */
    if (m->data_count) {
        bu32(&sec, (uint32_t)m->data_count);
        for (i = 0; i < m->data_count; i++) {
            bput(&sec, 0x00);                    /* active, memory 0 */
            bput(&sec, W_I32CONST);
            bi64(&sec, (int32_t)m->datas[i].off);
            bput(&sec, W_END);
            bu32(&sec, (uint32_t)m->datas[i].len);
            bwrite(&sec, m->datas[i].bytes, m->datas[i].len);
        }
        bput(out, 11); bu32(out, (uint32_t)sec.len); bwrite(out, sec.data, sec.len);
        sec.len = 0;
    }

    ok = 1;
done:
    free(internal_sig);
    free(wrapper_sig);
    free(import_sig);
    free(tab.sigs);
    free(sec.data);
    return ok;
}

/* In-memory emission: the caller owns *out_bytes (free()). */
int ribo_emit_wasm_mem(const RiboModule *module, uint8_t **out_bytes,
                       size_t *out_len, RiboError *error) {
    Buf out = {0};
    if (!module->func_count) {
        if (error) snprintf(error->message, sizeof error->message,
                            "emit: module has no funcs");
        return 0;
    }
    if (!emit_module(module, &out)) {
        free(out.data);
        if (error) snprintf(error->message, sizeof error->message,
                            "emit: out of memory");
        return 0;
    }
    *out_bytes = out.data;
    *out_len = out.len;
    return 1;
}

int ribo_emit_wasm(const RiboModule *module, const char *out_path,
                   RiboError *error) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    FILE *fp;
    int ok;
    if (!ribo_emit_wasm_mem(module, &bytes, &len, error)) return 0;
    fp = fopen(out_path, "wb");
    if (!fp) {
        free(bytes);
        if (error) snprintf(error->message, sizeof error->message,
                            "emit: cannot write file: %s", out_path);
        return 0;
    }
    ok = fwrite(bytes, 1, len, fp) == len;
    fclose(fp);
    free(bytes);
    if (!ok && error)
        snprintf(error->message, sizeof error->message,
                 "emit: failed writing file: %s", out_path);
    return ok;
}
