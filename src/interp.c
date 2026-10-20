/* interp.c — the reference interpreter. The single source of truth for
   semantics: every other execution path (wasm emission, the browser) is
   judged against THIS, never the reverse.
   Per-frame r0..r255 64-bit cells, step budget from RiboLimits, linear
   memory with per-access bounds checks, capability imports via RiboIO. */
#include "ribo_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    size_t func;           /* index into module funcs */
    uint32_t pc;
    uint64_t r[RIBO_MAX_REGISTERS];
} Frame;

typedef struct {
    const RiboModule *m;
    const RiboIO *io;
    RiboLimits limits;
    uint64_t steps_left;
    uint8_t *mem;
    size_t mem_size;       /* bytes */
    uint32_t mem_pages;
    uint64_t *globals;
    int32_t *table;        /* dense table: -1 = empty, else func/import index */
    Frame *frames;         /* heap call stack, owned by ribo_interpret */
    int faulted;           /* sticky fault flag (no setjmp: wasm freestanding
                              has none; the exec loop checks it every step) */
    char fault[1024];
} Exec;

static void fault(Exec *e, const char *fmt, ...) {
    va_list ap;
    if (e->faulted) return; /* keep the first fault */
    e->faulted = 1;
    va_start(ap, fmt);
    vsnprintf(e->fault, sizeof e->fault, fmt, ap);
    va_end(ap);
}

/* ---------- cell conversions ---------- */

static int32_t as_i32(uint64_t v) { return (int32_t)(uint32_t)v; }
static uint64_t from_i32(int32_t v) { return (uint64_t)(int64_t)v; }
static int64_t as_i64(uint64_t v) { return (int64_t)v; }
static float as_f32(uint64_t v) { uint32_t b = (uint32_t)v; float f; memcpy(&f, &b, 4); return f; }
static uint64_t from_f32(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static double as_f64(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static uint64_t from_f64(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }

/* ---------- linear memory ---------- */

static uint8_t *mem_at(Exec *e, uint64_t base_cell, int64_t off, size_t width) {
    uint64_t ea = (uint64_t)(uint32_t)base_cell + (uint64_t)(uint32_t)off;
    if (ea + width > e->mem_size || ea + width < ea) {
        fault(e, "memory access out of bounds");
        return NULL;
    }
    return e->mem + ea;
}

static uint64_t mem_load(Exec *e, uint64_t base, int64_t off, size_t width, int sign) {
    uint8_t *p = mem_at(e, base, off, width);
    uint64_t v = 0;
    size_t i;
    if (!p) return 0; /* faulted: the exec loop bails before the value matters */
    for (i = 0; i < width; i++) v |= (uint64_t)p[i] << (8 * i); /* little-endian */
    if (sign && width < 8) {
        uint64_t signbit = 1ull << (width * 8 - 1);
        if (v & signbit) v |= ~((1ull << (width * 8)) - 1);
    }
    return v;
}

static void mem_store(Exec *e, uint64_t base, int64_t off, size_t width, uint64_t v) {
    uint8_t *p = mem_at(e, base, off, width);
    size_t i;
    if (!p) return; /* faulted: nothing is written */
    for (i = 0; i < width; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/* wasm float min/max: NaN propagates; min(-0,+0) = -0, max(-0,+0) = +0 */
static float fmin_w(float a, float b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a < b) return a;
    if (b < a) return b;
    return signbit(a) ? a : b;
}
static float fmax_w(float a, float b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a > b) return a;
    if (b > a) return b;
    return signbit(a) ? b : a;
}
static double dmin_w(double a, double b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a < b) return a;
    if (b < a) return b;
    return signbit(a) ? a : b;
}
static double dmax_w(double a, double b) {
    if (isnan(a) || isnan(b)) return NAN;
    if (a > b) return a;
    if (b > a) return b;
    return signbit(a) ? b : a;
}

/* truncating float->int conversions with wasm trap semantics */
static int32_t trunc_to_i32(Exec *e, double d) {
    if (isnan(d) || d >= 2147483648.0 || d <= -2147483649.0)
        fault(e, "invalid truncation to integer");
    return (int32_t)d;
}
static int64_t trunc_to_i64(Exec *e, double d) {
    if (isnan(d) || d >= 9223372036854775808.0 || d < -9223372036854775808.0)
        fault(e, "invalid truncation to integer");
    return (int64_t)d;
}

/* unsigned variants: wasm semantics — NaN, <= -1 or >= 2^N traps */
static uint32_t trunc_to_u32(Exec *e, double d) {
    if (isnan(d) || d >= 4294967296.0 || d <= -1.0) {
        fault(e, "invalid truncation to integer");
        return 0;
    }
    return (uint32_t)(uint64_t)d;
}
static uint64_t trunc_to_u64(Exec *e, double d) {
    if (isnan(d) || d >= 18446744073709551616.0 || d <= -1.0) {
        fault(e, "invalid truncation to integer");
        return 0;
    }
    return (uint64_t)d;
}

/* ---------- the interpreter core ---------- */

static uint64_t exec_run(Exec *e, size_t entry, const uint64_t *args) {
    Frame *frames = e->frames;
    size_t sp = 0;
    const RiboModule *m = e->m;

    frames[sp].func = entry;
    frames[sp].pc = 0;
    memset(frames[sp].r, 0, sizeof frames[sp].r);
    {
        size_t i, na = m->funcs[entry].arity;
        for (i = 0; i < na; i++) frames[sp].r[i] = args[i];
    }
    sp++;

    while (sp > 0) {
        Frame *fr = &frames[sp - 1];
        const RiboFunc *f = &m->funcs[fr->func];
        const RiboInstr *in;
        uint64_t av, bv;

        if (e->faulted) return 0; /* sticky fault: unwind the frame stack */
        if (fr->pc >= f->code_len) {
            fault(e, "internal: fell off func %s", f->name);
            return 0;
        }
        in = &f->code[fr->pc++];
        if (e->steps_left == 0) {
            fault(e, "execution budget exhausted (limit: %llu steps)",
                  (unsigned long long)e->limits.step_limit);
            return 0;
        }
        e->steps_left--;

#define SRC64 (in->src_is_lit ? (uint64_t)in->imm : fr->r[in->src])
#define SRC32 as_i32(SRC64)
#define SRC64S as_i64(SRC64)
        switch (in->op) {
        case OP_ILIT: fr->r[in->dst] = from_i32((int32_t)in->imm); break;
        case OP_LLIT: fr->r[in->dst] = (uint64_t)in->imm; break;
        case OP_FLIT: fr->r[in->dst] = (uint32_t)in->imm; break;
        case OP_DLIT: fr->r[in->dst] = (uint64_t)in->imm; break;
        case OP_MOV:  fr->r[in->dst] = fr->r[in->src]; break;

#define IBIN(opname, expr) case opname: av = fr->r[in->dst]; fr->r[in->dst] = from_i32((int32_t)(expr)); break
        case OP_IADD: fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) + SRC32); break;
        case OP_ISUB: fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) - SRC32); break;
        case OP_IMUL: fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) * SRC32); break;
        case OP_IDIV: {
            int32_t a = as_i32(fr->r[in->dst]), b = SRC32;
            if (b == 0) { fault(e, "division by zero"); break; }
            if (a == INT32_MIN && b == -1) { fault(e, "division overflow"); break; }
            fr->r[in->dst] = from_i32(a / b);
            break;
        }
        case OP_IMOD: {
            int32_t a = as_i32(fr->r[in->dst]), b = SRC32;
            if (b == 0) { fault(e, "division by zero"); break; }
            if (a == INT32_MIN && b == -1) { fault(e, "division overflow"); break; }
            fr->r[in->dst] = from_i32(a % b);
            break;
        }
        case OP_IDIVU: {
            uint32_t a = (uint32_t)fr->r[in->dst], b = (uint32_t)SRC32;
            if (b == 0) { fault(e, "division by zero"); break; }
            fr->r[in->dst] = from_i32((int32_t)(a / b));
            break;
        }
        case OP_IMODU: {
            uint32_t a = (uint32_t)fr->r[in->dst], b = (uint32_t)SRC32;
            if (b == 0) { fault(e, "division by zero"); break; }
            fr->r[in->dst] = from_i32((int32_t)(a % b));
            break;
        }
        case OP_IAND: fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) & SRC32); break;
        case OP_IOR:  fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) | SRC32); break;
        case OP_IXOR: fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst]) ^ SRC32); break;
        case OP_ISHL: case OP_ISHR: case OP_ISAR: {
            uint32_t a = (uint32_t)fr->r[in->dst];
            int64_t c = SRC64S;
            if (c < 0 || c > 31) { fault(e, "shift count out of range (0..31)"); break; }
            if (in->op == OP_ISHL) a <<= c;
            else if (in->op == OP_ISHR) a >>= c;
            else a = (uint32_t)((int32_t)a >> c);
            fr->r[in->dst] = from_i32((int32_t)a);
            break;
        }
        case OP_LADD: fr->r[in->dst] = (uint64_t)(as_i64(fr->r[in->dst]) + SRC64S); break;
        case OP_LSUB: fr->r[in->dst] = (uint64_t)(as_i64(fr->r[in->dst]) - SRC64S); break;
        case OP_LMUL: fr->r[in->dst] = (uint64_t)(as_i64(fr->r[in->dst]) * SRC64S); break;
        case OP_LDIV: {
            int64_t a = as_i64(fr->r[in->dst]), b = SRC64S;
            if (b == 0) { fault(e, "division by zero"); break; }
            if (a == INT64_MIN && b == -1) { fault(e, "division overflow"); break; }
            fr->r[in->dst] = (uint64_t)(a / b);
            break;
        }
        case OP_LMOD: {
            int64_t a = as_i64(fr->r[in->dst]), b = SRC64S;
            if (b == 0) { fault(e, "division by zero"); break; }
            if (a == INT64_MIN && b == -1) { fault(e, "division overflow"); break; }
            fr->r[in->dst] = (uint64_t)(a % b);
            break;
        }
        case OP_LDIVU: {
            uint64_t a = fr->r[in->dst], b = SRC64;
            if (b == 0) { fault(e, "division by zero"); break; }
            fr->r[in->dst] = a / b;
            break;
        }
        case OP_LMODU: {
            uint64_t a = fr->r[in->dst], b = SRC64;
            if (b == 0) { fault(e, "division by zero"); break; }
            fr->r[in->dst] = a % b;
            break;
        }
        case OP_LAND: fr->r[in->dst] &= SRC64; break;
        case OP_LOR:  fr->r[in->dst] |= SRC64; break;
        case OP_LXOR: fr->r[in->dst] ^= SRC64; break;
        case OP_LSHL: case OP_LSHR: case OP_LSAR: {
            uint64_t a = fr->r[in->dst];
            int64_t c = SRC64S;
            if (c < 0 || c > 63) { fault(e, "shift count out of range (0..63)"); break; }
            if (in->op == OP_LSHL) a <<= c;
            else if (in->op == OP_LSHR) a >>= c;
            else a = (uint64_t)((int64_t)a >> c);
            fr->r[in->dst] = a;
            break;
        }

        /* rotations: the count is masked like wasm — no trap (unlike shifts) */
        case OP_IROTL: {
            uint32_t a = (uint32_t)fr->r[in->dst];
            uint32_t c = (uint32_t)SRC64S & 31u;
            fr->r[in->dst] = from_i32((int32_t)((a << c) | (a >> ((32u - c) & 31u))));
            break;
        }
        case OP_IROTR: {
            uint32_t a = (uint32_t)fr->r[in->dst];
            uint32_t c = (uint32_t)SRC64S & 31u;
            fr->r[in->dst] = from_i32((int32_t)((a >> c) | (a << ((32u - c) & 31u))));
            break;
        }
        case OP_LROTL: {
            uint64_t a = fr->r[in->dst];
            uint64_t c = (uint64_t)SRC64 & 63u;
            fr->r[in->dst] = (a << c) | (a >> ((64u - c) & 63u));
            break;
        }
        case OP_LROTR: {
            uint64_t a = fr->r[in->dst];
            uint64_t c = (uint64_t)SRC64 & 63u;
            fr->r[in->dst] = (a >> c) | (a << ((64u - c) & 63u));
            break;
        }

        /* bit counting: no traps; clz/ctz of 0 give the width */
        case OP_ICLZ: {
            uint32_t v = (uint32_t)fr->r[in->dst];
            fr->r[in->dst] = from_i32(v ? __builtin_clz(v) : 32);
            break;
        }
        case OP_ICTZ: {
            uint32_t v = (uint32_t)fr->r[in->dst];
            fr->r[in->dst] = from_i32(v ? __builtin_ctz(v) : 32);
            break;
        }
        case OP_IPOP:
            fr->r[in->dst] = from_i32(__builtin_popcount((uint32_t)fr->r[in->dst]));
            break;
        case OP_LCLZ: {
            uint64_t v = fr->r[in->dst];
            fr->r[in->dst] = (uint64_t)(v ? __builtin_clzll(v) : 64);
            break;
        }
        case OP_LCTZ: {
            uint64_t v = fr->r[in->dst];
            fr->r[in->dst] = (uint64_t)(v ? __builtin_ctzll(v) : 64);
            break;
        }
        case OP_LPOP:
            fr->r[in->dst] = (uint64_t)__builtin_popcountll(fr->r[in->dst]);
            break;

        case OP_FADD: fr->r[in->dst] = from_f32(as_f32(fr->r[in->dst]) + as_f32(fr->r[in->src])); break;
        case OP_FSUB: fr->r[in->dst] = from_f32(as_f32(fr->r[in->dst]) - as_f32(fr->r[in->src])); break;
        case OP_FMUL: fr->r[in->dst] = from_f32(as_f32(fr->r[in->dst]) * as_f32(fr->r[in->src])); break;
        case OP_FDIV: fr->r[in->dst] = from_f32(as_f32(fr->r[in->dst]) / as_f32(fr->r[in->src])); break;
        case OP_FMIN: fr->r[in->dst] = from_f32(fmin_w(as_f32(fr->r[in->dst]), as_f32(fr->r[in->src]))); break;
        case OP_FMAX: fr->r[in->dst] = from_f32(fmax_w(as_f32(fr->r[in->dst]), as_f32(fr->r[in->src]))); break;
        case OP_FSQRT: fr->r[in->dst] = from_f32(sqrtf(as_f32(fr->r[in->dst]))); break;
        case OP_FABS:  fr->r[in->dst] = from_f32(fabsf(as_f32(fr->r[in->dst]))); break;
        case OP_FNEG:  fr->r[in->dst] = from_f32(-as_f32(fr->r[in->dst])); break;

        case OP_DADD: fr->r[in->dst] = from_f64(as_f64(fr->r[in->dst]) + as_f64(fr->r[in->src])); break;
        case OP_DSUB: fr->r[in->dst] = from_f64(as_f64(fr->r[in->dst]) - as_f64(fr->r[in->src])); break;
        case OP_DMUL: fr->r[in->dst] = from_f64(as_f64(fr->r[in->dst]) * as_f64(fr->r[in->src])); break;
        case OP_DDIV: fr->r[in->dst] = from_f64(as_f64(fr->r[in->dst]) / as_f64(fr->r[in->src])); break;
        case OP_DMIN: fr->r[in->dst] = from_f64(dmin_w(as_f64(fr->r[in->dst]), as_f64(fr->r[in->src]))); break;
        case OP_DMAX: fr->r[in->dst] = from_f64(dmax_w(as_f64(fr->r[in->dst]), as_f64(fr->r[in->src]))); break;
        case OP_DSQRT: fr->r[in->dst] = from_f64(sqrt(as_f64(fr->r[in->dst]))); break;
        case OP_DABS:  fr->r[in->dst] = from_f64(fabs(as_f64(fr->r[in->dst]))); break;
        case OP_DNEG:  fr->r[in->dst] = from_f64(-as_f64(fr->r[in->dst])); break;

#define ICMP(opn, cond) case opn: av = fr->r[in->dst]; bv = SRC64; fr->r[in->dst] = (cond) ? 1 : 0; break
        ICMP(OP_IEQ, as_i32(av) == as_i32(bv));
        ICMP(OP_INE, as_i32(av) != as_i32(bv));
        ICMP(OP_ILT, as_i32(av) < as_i32(bv));
        ICMP(OP_ILE, as_i32(av) <= as_i32(bv));
        ICMP(OP_IGT, as_i32(av) > as_i32(bv));
        ICMP(OP_IGE, as_i32(av) >= as_i32(bv));
        ICMP(OP_ILTU, (uint32_t)av < (uint32_t)bv);
        ICMP(OP_ILEU, (uint32_t)av <= (uint32_t)bv);
        ICMP(OP_IGTU, (uint32_t)av > (uint32_t)bv);
        ICMP(OP_IGEU, (uint32_t)av >= (uint32_t)bv);
        ICMP(OP_LEQ, as_i64(av) == as_i64(bv));
        ICMP(OP_LNE, as_i64(av) != as_i64(bv));
        ICMP(OP_LLT, as_i64(av) < as_i64(bv));
        ICMP(OP_LLE, as_i64(av) <= as_i64(bv));
        ICMP(OP_LGT, as_i64(av) > as_i64(bv));
        ICMP(OP_LGE, as_i64(av) >= as_i64(bv));
        ICMP(OP_LLTU, av < bv);
        ICMP(OP_LLEU, av <= bv);
        ICMP(OP_LGTU, av > bv);
        ICMP(OP_LGEU, av >= bv);
        ICMP(OP_FEQ, as_f32(av) == as_f32(bv));
        ICMP(OP_FNE, as_f32(av) != as_f32(bv));
        ICMP(OP_FLT, as_f32(av) < as_f32(bv));
        ICMP(OP_FLE, as_f32(av) <= as_f32(bv));
        ICMP(OP_FGT, as_f32(av) > as_f32(bv));
        ICMP(OP_FGE, as_f32(av) >= as_f32(bv));
        ICMP(OP_DEQ, as_f64(av) == as_f64(bv));
        ICMP(OP_DNE, as_f64(av) != as_f64(bv));
        ICMP(OP_DLT, as_f64(av) < as_f64(bv));
        ICMP(OP_DLE, as_f64(av) <= as_f64(bv));
        ICMP(OP_DGT, as_f64(av) > as_f64(bv));
        ICMP(OP_DGE, as_f64(av) >= as_f64(bv));

        case OP_L2I:  fr->r[in->dst] = from_i32(as_i32(fr->r[in->dst])); break;
        case OP_I2L:  fr->r[in->dst] = (uint64_t)(int64_t)as_i32(fr->r[in->dst]); break;
        case OP_I2LU: fr->r[in->dst] = (uint32_t)fr->r[in->dst]; break;
        case OP_F2I:  fr->r[in->dst] = from_i32(trunc_to_i32(e, as_f32(fr->r[in->dst]))); break;
        case OP_F2L:  fr->r[in->dst] = (uint64_t)trunc_to_i64(e, as_f32(fr->r[in->dst])); break;
        case OP_D2I:  fr->r[in->dst] = from_i32(trunc_to_i32(e, as_f64(fr->r[in->dst]))); break;
        case OP_D2L:  fr->r[in->dst] = (uint64_t)trunc_to_i64(e, as_f64(fr->r[in->dst])); break;
        case OP_F2IU: fr->r[in->dst] = from_i32((int32_t)trunc_to_u32(e, as_f32(fr->r[in->dst]))); break;
        case OP_F2LU: fr->r[in->dst] = trunc_to_u64(e, as_f32(fr->r[in->dst])); break;
        case OP_D2IU: fr->r[in->dst] = from_i32((int32_t)trunc_to_u32(e, as_f64(fr->r[in->dst]))); break;
        case OP_D2LU: fr->r[in->dst] = trunc_to_u64(e, as_f64(fr->r[in->dst])); break;
        case OP_I2F:  fr->r[in->dst] = from_f32((float)as_i32(fr->r[in->dst])); break;
        case OP_L2F:  fr->r[in->dst] = from_f32((float)as_i64(fr->r[in->dst])); break;
        case OP_I2D:  fr->r[in->dst] = from_f64((double)as_i32(fr->r[in->dst])); break;
        case OP_L2D:  fr->r[in->dst] = from_f64((double)as_i64(fr->r[in->dst])); break;
        case OP_F2D:  fr->r[in->dst] = from_f64((double)as_f32(fr->r[in->dst])); break;
        case OP_D2F:  fr->r[in->dst] = from_f32((float)as_f64(fr->r[in->dst])); break;

        case OP_IGET: fr->r[in->dst] = from_i32((int32_t)(uint32_t)mem_load(e, fr->r[in->src], in->imm, 4, 0)); break;
        case OP_LGET: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 8, 0); break;
        case OP_FGET: fr->r[in->dst] = (uint32_t)mem_load(e, fr->r[in->src], in->imm, 4, 0); break;
        case OP_DGET: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 8, 0); break;
        case OP_IGET8S:  fr->r[in->dst] = from_i32((int32_t)mem_load(e, fr->r[in->src], in->imm, 1, 1)); break;
        case OP_IGET8U:  fr->r[in->dst] = from_i32((int32_t)mem_load(e, fr->r[in->src], in->imm, 1, 0)); break;
        case OP_IGET16S: fr->r[in->dst] = from_i32((int32_t)mem_load(e, fr->r[in->src], in->imm, 2, 1)); break;
        case OP_IGET16U: fr->r[in->dst] = from_i32((int32_t)mem_load(e, fr->r[in->src], in->imm, 2, 0)); break;
        case OP_LGET8S:  fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 1, 1); break;
        case OP_LGET8U:  fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 1, 0); break;
        case OP_LGET16S: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 2, 1); break;
        case OP_LGET16U: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 2, 0); break;
        case OP_LGET32S: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 4, 1); break;
        case OP_LGET32U: fr->r[in->dst] = mem_load(e, fr->r[in->src], in->imm, 4, 0); break;

        case OP_IPUT: mem_store(e, fr->r[in->dst], in->imm, 4, fr->r[in->src]); break;
        case OP_LPUT: mem_store(e, fr->r[in->dst], in->imm, 8, fr->r[in->src]); break;
        case OP_FPUT: mem_store(e, fr->r[in->dst], in->imm, 4, fr->r[in->src]); break;
        case OP_DPUT: mem_store(e, fr->r[in->dst], in->imm, 8, fr->r[in->src]); break;
        case OP_IPUT8:  mem_store(e, fr->r[in->dst], in->imm, 1, fr->r[in->src]); break;
        case OP_IPUT16: mem_store(e, fr->r[in->dst], in->imm, 2, fr->r[in->src]); break;
        case OP_LPUT8:  mem_store(e, fr->r[in->dst], in->imm, 1, fr->r[in->src]); break;
        case OP_LPUT16: mem_store(e, fr->r[in->dst], in->imm, 2, fr->r[in->src]); break;
        case OP_LPUT32: mem_store(e, fr->r[in->dst], in->imm, 4, fr->r[in->src]); break;

        case OP_MSIZE:
            fr->r[in->dst] = from_i32((int32_t)e->mem_pages);
            break;
        case OP_MGROW: {
            uint32_t delta = (uint32_t)fr->r[in->src];
            uint32_t maxp = e->m->mem_max ? e->m->mem_max : e->limits.max_pages;
            uint64_t newp = (uint64_t)e->mem_pages + delta;
            if (maxp > e->limits.max_pages) maxp = e->limits.max_pages;
            if (newp > maxp || newp > 0xFFFFFFFFull) {
                fr->r[in->dst] = from_i32(-1);
            } else {
                uint8_t *nm = realloc(e->mem, (size_t)newp * 65536u);
                uint32_t old = e->mem_pages;
                if (!nm && newp > 0) { fr->r[in->dst] = from_i32(-1); break; }
                e->mem = nm;
                memset(e->mem + (size_t)old * 65536u, 0, (size_t)delta * 65536u);
                e->mem_pages = (uint32_t)newp;
                e->mem_size = (size_t)newp * 65536u;
                fr->r[in->dst] = from_i32((int32_t)old);
            }
            break;
        }

        case OP_GGET: fr->r[in->dst] = e->globals[in->imm]; break;
        case OP_GSET: e->globals[in->imm] = fr->r[in->src]; break;

        case OP_JMP: fr->pc = (uint32_t)in->imm; break;
        case OP_JZ:  if (as_i32(fr->r[in->dst]) == 0) fr->pc = (uint32_t)in->imm; break;
        case OP_JNZ: if (as_i32(fr->r[in->dst]) != 0) fr->pc = (uint32_t)in->imm; break;

        case OP_CALL:
        case OP_CALLI: {
            size_t idx;
            uint64_t result = 0;
            if (in->op == OP_CALLI) {
                uint64_t ti = fr->r[in->dst];
                if (!e->table || ti >= m->table_min) {
                    fault(e, "indirect call index out of bounds");
                    break;
                }
                if (e->table[ti] < 0) {
                    fault(e, "indirect call to uninitialized table entry");
                    break;
                }
                idx = (size_t)e->table[ti];
                /* arity must match what the caller actually passes */
                {
                    uint8_t want = idx < m->func_count
                        ? m->funcs[idx].arity
                        : m->imports[idx - m->func_count].arity;
                    if (want != in->nargs) {
                        fault(e, "indirect call type mismatch");
                        break;
                    }
                }
                /* a void import has a different wasm signature — trap like V8 */
                if (idx >= m->func_count &&
                    m->imports[idx - m->func_count].ret == T_VOID) {
                    fault(e, "indirect call type mismatch");
                    break;
                }
            } else {
                idx = (size_t)in->imm;
            }
            if (idx < m->func_count) {
                const RiboFunc *callee = &m->funcs[idx];
                size_t i;
                if (sp >= e->limits.max_depth) {
                    fault(e, "call depth exhausted (limit: %u)", (unsigned)e->limits.max_depth);
                    break;
                }
                frames[sp].func = idx;
                frames[sp].pc = 0;
                memset(frames[sp].r, 0, sizeof frames[sp].r);
                for (i = 0; i < callee->arity; i++)
                    frames[sp].r[i] = fr->r[in->args[i]];
                sp++;
            } else {
                const RiboImportDecl *im = &m->imports[idx - m->func_count];
                const RiboImport *cap = NULL;
                size_t i;
                if (e->io)
                    for (i = 0; i < e->io->import_count; i++)
                        if (!strcmp(e->io->imports[i].name, im->name))
                            cap = &e->io->imports[i];
                if (!cap || !cap->call) {
                    fault(e, "IMPORT requires host capability: %s", im->name);
                    break;
                }
                {
                    uint64_t argv[RIBO_MAX_ARGS];
                    for (i = 0; i < im->arity; i++) argv[i] = fr->r[in->args[i]];
                    if (!cap->call(cap->user, argv, im->arity, &result)) {
                        fault(e, "the host import callback failed");
                        break;
                    }
                    if (im->ret == T_VOID) result = 0;
                }
                fr->r[0] = result;
            }
            break;
        }

        case OP_RET: {
            uint64_t result = in->nargs ? fr->r[in->dst] : 0;
            sp--;
            if (sp == 0) return result;
            frames[sp - 1].r[0] = result;
            break;
        }

        case OP_TRAP: fault(e, "unreachable"); break;
        default:      fault(e, "internal: bad opcode %u", (unsigned)in->op); break;
        }
#undef SRC64
#undef SRC32
#undef SRC64S
    }
    fault(e, "internal: empty call stack");
    return 0;
}

/* ---------- public entry ---------- */

int ribo_interpret(const RiboModule *module, const char *func,
                   const RiboValue *args, size_t arg_count,
                   const RiboIO *io, const RiboLimits *limits,
                   RiboValue *result, RiboError *error) {
    Exec e;
    size_t i;
    const RiboFunc *f = NULL;
    RiboLimits lim;

    memset(&e, 0, sizeof e);
    lim.step_limit = RIBO_DEFAULT_STEP_LIMIT;
    lim.max_depth = RIBO_MAX_DEPTH;
    lim.max_pages = RIBO_MAX_PAGES;
    if (limits) {
        if (limits->step_limit) lim.step_limit = limits->step_limit;
        if (limits->max_depth) lim.max_depth = limits->max_depth;
        if (limits->max_pages) lim.max_pages = limits->max_pages;
    }

    for (i = 0; i < module->func_count; i++)
        if (!strcmp(module->funcs[i].name, func)) f = &module->funcs[i];
    if (!f) {
        if (error) snprintf(error->message, sizeof error->message,
                            "run: undefined func: %s", func);
        return 0;
    }
    if (arg_count != f->arity) {
        if (error) snprintf(error->message, sizeof error->message,
                            "run: func %s expects %u arguments, got %u",
                            func, (unsigned)f->arity, (unsigned)arg_count);
        return 0;
    }

    e.m = module;
    e.io = io;
    e.limits = lim;
    e.steps_left = lim.step_limit;
    e.frames = malloc(lim.max_depth * sizeof *e.frames);
    if (!e.frames) {
        if (error) snprintf(error->message, sizeof error->message, "run: out of memory");
        return 0;
    }
    if (module->glob_count) {
        e.globals = calloc(module->glob_count, sizeof *e.globals);
        if (!e.globals) {
            free(e.frames);
            if (error) snprintf(error->message, sizeof error->message, "run: out of memory");
            return 0;
        }
        for (i = 0; i < module->glob_count; i++) e.globals[i] = module->globals[i].bits;
    }
    if (module->has_mem) {
        e.mem_pages = module->mem_min;
        e.mem_size = (size_t)e.mem_pages * 65536u;
        e.mem = calloc(e.mem_size ? e.mem_size : 1, 1);
        if (!e.mem) {
            free(e.frames);
            free(e.globals);
            if (error) snprintf(error->message, sizeof error->message, "run: out of memory");
            return 0;
        }
        /* DATA segments (bounds proven statically by the checker) */
        for (i = 0; i < module->data_count; i++)
            memcpy(e.mem + module->datas[i].off, module->datas[i].bytes,
                   module->datas[i].len);
    }
    if (module->has_table) {
        e.table = malloc((size_t)module->table_min * sizeof *e.table);
        if (!e.table) {
            free(e.frames);
            free(e.mem);
            free(e.globals);
            if (error) snprintf(error->message, sizeof error->message, "run: out of memory");
            return 0;
        }
        for (i = 0; i < module->table_min; i++) e.table[i] = -1;
        for (i = 0; i < module->elem_count; i++)
            e.table[module->elems[i].off] = (int32_t)module->elems[i].func;
    }

    {
        RiboValue r = exec_run(&e, (size_t)(f - module->funcs), args);
        if (e.faulted) {
            if (error) snprintf(error->message, sizeof error->message,
                                "run: %s", e.fault);
            free(e.frames);
            free(e.mem);
            free(e.globals);
            free(e.table);
            return 0;
        }
        if (result) *result = r;
    }
    free(e.frames);
    free(e.mem);
    free(e.globals);
    free(e.table);
    return 1;
}
