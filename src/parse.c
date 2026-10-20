/* parse.c — .rna text -> RiboModule.
   Strict 3-field lines, '_' padding, exact errors with line/line_text.
   Repair mode pads unambiguous omitted '_' fields. The source is never
   modified by us; we work on an in-memory copy. */
#include "ribo_internal.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---------- opcode table ---------- */

enum { /* operand forms */
    FM_LIT, FM_MOV, FM_BIN, FM_UNARY, FM_CMP, FM_CVT,
    FM_LOAD, FM_STORE, FM_MSIZE, FM_MGROW,
    FM_GGET, FM_GSET, FM_JMP, FM_JCC, FM_CALL, FM_RET, FM_TRAP, FM_CALLI
};
enum { C_N, C_I, C_L, C_F, C_D }; /* value class (literal policy) */

typedef struct { const char *name; uint16_t op; uint8_t form, cls; } OpDef;

static const OpDef OPS[] = {
    { "ILIT", OP_ILIT, FM_LIT, C_I }, { "LLIT", OP_LLIT, FM_LIT, C_L },
    { "FLIT", OP_FLIT, FM_LIT, C_F }, { "DLIT", OP_DLIT, FM_LIT, C_D },
    { "MOV",  OP_MOV,  FM_MOV, C_N },
#define ROW6(p, o, f, c) \
    { p "ADD", o, f, c }, { p "SUB", (o + 1), f, c }, \
    { p "MUL", (o + 2), f, c }, { p "DIV", (o + 3), f, c }
    ROW6("I", OP_IADD, FM_BIN, C_I), { "IMOD", OP_IMOD, FM_BIN, C_I },
    { "IDIVU", OP_IDIVU, FM_BIN, C_I }, { "IMODU", OP_IMODU, FM_BIN, C_I },
    { "IAND", OP_IAND, FM_BIN, C_I }, { "IOR", OP_IOR, FM_BIN, C_I },
    { "IXOR", OP_IXOR, FM_BIN, C_I }, { "ISHL", OP_ISHL, FM_BIN, C_I },
    { "ISHR", OP_ISHR, FM_BIN, C_I }, { "ISAR", OP_ISAR, FM_BIN, C_I },
    ROW6("L", OP_LADD, FM_BIN, C_L), { "LMOD", OP_LMOD, FM_BIN, C_L },
    { "LDIVU", OP_LDIVU, FM_BIN, C_L }, { "LMODU", OP_LMODU, FM_BIN, C_L },
    { "LAND", OP_LAND, FM_BIN, C_L }, { "LOR", OP_LOR, FM_BIN, C_L },
    { "LXOR", OP_LXOR, FM_BIN, C_L }, { "LSHL", OP_LSHL, FM_BIN, C_L },
    { "LSHR", OP_LSHR, FM_BIN, C_L }, { "LSAR", OP_LSAR, FM_BIN, C_L },
    ROW6("F", OP_FADD, FM_BIN, C_F),
    { "FMIN", OP_FMIN, FM_BIN, C_F }, { "FMAX", OP_FMAX, FM_BIN, C_F },
    { "FSQRT", OP_FSQRT, FM_UNARY, C_F }, { "FABS", OP_FABS, FM_UNARY, C_F },
    { "FNEG", OP_FNEG, FM_UNARY, C_F },
    ROW6("D", OP_DADD, FM_BIN, C_D),
    { "DMIN", OP_DMIN, FM_BIN, C_D }, { "DMAX", OP_DMAX, FM_BIN, C_D },
    { "DSQRT", OP_DSQRT, FM_UNARY, C_D }, { "DABS", OP_DABS, FM_UNARY, C_D },
    { "DNEG", OP_DNEG, FM_UNARY, C_D },
#undef ROW6
#define CMP6(p, o, c) \
    { p "EQ", o, FM_CMP, c }, { p "NE", (o + 1), FM_CMP, c }, \
    { p "LT", (o + 2), FM_CMP, c }, { p "LE", (o + 3), FM_CMP, c }, \
    { p "GT", (o + 4), FM_CMP, c }, { p "GE", (o + 5), FM_CMP, c }
    CMP6("I", OP_IEQ, C_I),
    { "ILTU", OP_ILTU, FM_CMP, C_I }, { "ILEU", OP_ILEU, FM_CMP, C_I },
    { "IGTU", OP_IGTU, FM_CMP, C_I }, { "IGEU", OP_IGEU, FM_CMP, C_I },
    CMP6("L", OP_LEQ, C_L),
    { "LLTU", OP_LLTU, FM_CMP, C_L }, { "LLEU", OP_LLEU, FM_CMP, C_L },
    { "LGTU", OP_LGTU, FM_CMP, C_L }, { "LGEU", OP_LGEU, FM_CMP, C_L },
    CMP6("F", OP_FEQ, C_F),
    CMP6("D", OP_DEQ, C_D),
#undef CMP6
    { "L2I", OP_L2I, FM_CVT, C_N }, { "I2L", OP_I2L, FM_CVT, C_N },
    { "I2LU", OP_I2LU, FM_CVT, C_N }, { "F2I", OP_F2I, FM_CVT, C_N },
    { "F2L", OP_F2L, FM_CVT, C_N }, { "D2I", OP_D2I, FM_CVT, C_N },
    { "D2L", OP_D2L, FM_CVT, C_N }, { "I2F", OP_I2F, FM_CVT, C_N },
    { "L2F", OP_L2F, FM_CVT, C_N }, { "I2D", OP_I2D, FM_CVT, C_N },
    { "L2D", OP_L2D, FM_CVT, C_N }, { "F2D", OP_F2D, FM_CVT, C_N },
    { "D2F", OP_D2F, FM_CVT, C_N },
    { "IGET", OP_IGET, FM_LOAD, C_I }, { "LGET", OP_LGET, FM_LOAD, C_L },
    { "FGET", OP_FGET, FM_LOAD, C_F }, { "DGET", OP_DGET, FM_LOAD, C_D },
    { "IGET8S", OP_IGET8S, FM_LOAD, C_I }, { "IGET8U", OP_IGET8U, FM_LOAD, C_I },
    { "IGET16S", OP_IGET16S, FM_LOAD, C_I }, { "IGET16U", OP_IGET16U, FM_LOAD, C_I },
    { "LGET8S", OP_LGET8S, FM_LOAD, C_L }, { "LGET8U", OP_LGET8U, FM_LOAD, C_L },
    { "LGET16S", OP_LGET16S, FM_LOAD, C_L }, { "LGET16U", OP_LGET16U, FM_LOAD, C_L },
    { "LGET32S", OP_LGET32S, FM_LOAD, C_L }, { "LGET32U", OP_LGET32U, FM_LOAD, C_L },
    { "IPUT", OP_IPUT, FM_STORE, C_I }, { "LPUT", OP_LPUT, FM_STORE, C_L },
    { "FPUT", OP_FPUT, FM_STORE, C_F }, { "DPUT", OP_DPUT, FM_STORE, C_D },
    { "IPUT8", OP_IPUT8, FM_STORE, C_I }, { "IPUT16", OP_IPUT16, FM_STORE, C_I },
    { "LPUT8", OP_LPUT8, FM_STORE, C_L }, { "LPUT16", OP_LPUT16, FM_STORE, C_L },
    { "LPUT32", OP_LPUT32, FM_STORE, C_L },
    { "MSIZE", OP_MSIZE, FM_MSIZE, C_N }, { "MGROW", OP_MGROW, FM_MGROW, C_N },
    { "GGET", OP_GGET, FM_GGET, C_N }, { "GSET", OP_GSET, FM_GSET, C_N },
    { "JMP", OP_JMP, FM_JMP, C_N }, { "JZ", OP_JZ, FM_JCC, C_N },
    { "JNZ", OP_JNZ, FM_JCC, C_N }, { "CALL", OP_CALL, FM_CALL, C_N },
    { "RET", OP_RET, FM_RET, C_N }, { "TRAP", OP_TRAP, FM_TRAP, C_N },
    { "CALLI", OP_CALLI, FM_CALLI, C_N },
    { "F2IU", OP_F2IU, FM_CVT, C_N }, { "F2LU", OP_F2LU, FM_CVT, C_N },
    { "D2IU", OP_D2IU, FM_CVT, C_N }, { "D2LU", OP_D2LU, FM_CVT, C_N },
    { "ICLZ", OP_ICLZ, FM_UNARY, C_I }, { "ICTZ", OP_ICTZ, FM_UNARY, C_I },
    { "IPOP", OP_IPOP, FM_UNARY, C_I }, { "LCLZ", OP_LCLZ, FM_UNARY, C_L },
    { "LCTZ", OP_LCTZ, FM_UNARY, C_L }, { "LPOP", OP_LPOP, FM_UNARY, C_L },
    { "IROTL", OP_IROTL, FM_BIN, C_I }, { "IROTR", OP_IROTR, FM_BIN, C_I },
    { "LROTL", OP_LROTL, FM_BIN, C_L }, { "LROTR", OP_LROTR, FM_BIN, C_L },
};

static const OpDef *find_op(const char *name) {
    size_t i;
    for (i = 0; i < sizeof OPS / sizeof OPS[0]; i++)
        if (!strcmp(OPS[i].name, name)) return &OPS[i];
    return NULL;
}

/* ---------- small helpers ---------- */

static int is_ident(const char *s) {
    size_t i, n = strlen(s);
    if (!n || n > 63) return 0;
    if (!(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (i = 1; i < n; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.'))
            return 0;
    return 1;
}

/* "r12" -> 12, or -1 */
static int parse_reg(const char *s) {
    long v;
    char *end;
    if (s[0] != 'r' || !isdigit((unsigned char)s[1])) return -1;
    errno = 0;
    v = strtol(s + 1, &end, 10);
    if (errno || *end || v < 0 || v >= (long)RIBO_MAX_REGISTERS) return -1;
    return (int)v;
}

static int parse_type(const char *s, int *type) {
    if (!strcmp(s, "i32")) *type = T_I32;
    else if (!strcmp(s, "i64")) *type = T_I64;
    else if (!strcmp(s, "f32")) *type = T_F32;
    else if (!strcmp(s, "f64")) *type = T_F64;
    else if (!strcmp(s, "void")) *type = T_VOID;
    else return 0;
    return 1;
}

/* "i32/2" -> type + arity */
static int parse_sig(const char *s, int *type, int *arity, int allow_void) {
    char buf[32];
    const char *slash = strchr(s, '/');
    long a;
    char *end;
    if (!slash || (size_t)(slash - s) >= sizeof buf) return 0;
    memcpy(buf, s, (size_t)(slash - s));
    buf[slash - s] = '\0';
    if (!parse_type(buf, type)) return 0;
    if (!allow_void && *type == T_VOID) return 0;
    if (!isdigit((unsigned char)slash[1])) return 0;
    a = strtol(slash + 1, &end, 10);
    if (*end || a < 0 || a > (long)RIBO_MAX_ARGS) return 0;
    *arity = (int)a;
    return 1;
}

/* split line into up to 3 fields (in place); returns field count */
static int split_fields(char *line, char **f) {
    int n = 0;
    char *p = line;
    while (*p && n < 4) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n == 3) return 4; /* a 4th field exists */
        f[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return n;
}

static int parse_i64(const char *s, int64_t *out) {
    char *end;
    long long v;
    if (!*s) return 0;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno == ERANGE || *end) return 0;
    *out = (int64_t)v;
    return 1;
}

/* ---------- fixups (resolved after the whole module is parsed) ---------- */

enum { FX_CALL, FX_GLOB, FX_GINIT, FX_EXPORT, FX_TELEM };

static int hexnib(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    size_t func, instr;
    int kind;
    char name[64];
    char text[80];       /* FX_GINIT: the literal as written */
    uint32_t line;
} Fixup;

typedef struct {
    RiboModule *m;
    RiboDiag *diag;
    int repair;
    /* per-func temporaries */
    struct { char name[64]; size_t index; } labels[512];
    size_t label_count;
    Fixup *fixups;
    size_t fx_count, fx_cap;
} Parser;

static int fail(Parser *p, uint32_t line, const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ribo_diag_set(p->diag, "parse", line,
                  (line && line <= p->m->line_count) ? p->m->lines[line - 1] : "",
                  "%s", msg);
    return 0;
}

static int add_fixup2(Parser *p, size_t func, size_t instr, int kind,
                      const char *name, const char *text, uint32_t line) {
    Fixup *fx;
    if (p->fx_count == p->fx_cap) {
        size_t nc = p->fx_cap ? p->fx_cap * 2 : 32;
        Fixup *nf = realloc(p->fixups, nc * sizeof *nf);
        if (!nf) return 0;
        p->fixups = nf;
        p->fx_cap = nc;
    }
    fx = &p->fixups[p->fx_count++];
    fx->func = func;
    fx->instr = instr;
    fx->kind = kind;
    snprintf(fx->name, sizeof fx->name, "%s", name);
    snprintf(fx->text, sizeof fx->text, "%s", text ? text : "");
    fx->line = line;
    return 1;
}

static int add_fixup(Parser *p, size_t func, size_t instr, int kind,
                     const char *name, uint32_t line) {
    return add_fixup2(p, func, instr, kind, name, NULL, line);
}

static RiboFunc *find_func(RiboModule *m, const char *name) {
    size_t i;
    for (i = 0; i < m->func_count; i++)
        if (!strcmp(m->funcs[i].name, name)) return &m->funcs[i];
    return NULL;
}
RiboFunc *ribo_find_func(RiboModule *m, const char *name) { return find_func(m, name); }

static int find_import(RiboModule *m, const char *name) {
    size_t i;
    for (i = 0; i < m->import_count; i++)
        if (!strcmp(m->imports[i].name, name)) return (int)i;
    return -1;
}

static int find_global(RiboModule *m, const char *name) {
    size_t i;
    for (i = 0; i < m->glob_count; i++)
        if (!strcmp(m->globals[i].name, name)) return (int)i;
    return -1;
}

static int emit(RiboFunc *f, RiboInstr *in) {
    if (f->code_len == f->code_cap) {
        size_t nc = f->code_cap ? f->code_cap * 2 : 64;
        RiboInstr *ni = realloc(f->code, nc * sizeof *ni);
        if (!ni) return 0;
        f->code = ni;
        f->code_cap = nc;
    }
    f->code[f->code_len++] = *in;
    return 1;
}

/* ---------- instruction parsing ---------- */

/* parse "base,offset" into reg + offset */
static int parse_addr(const char *s, int *reg, int64_t *off) {
    char buf[96];
    const char *comma = strchr(s, ',');
    if (!comma || (size_t)(comma - s) >= sizeof buf) return 0;
    memcpy(buf, s, (size_t)(comma - s));
    buf[comma - s] = '\0';
    *reg = parse_reg(buf);
    if (*reg < 0) return 0;
    return parse_i64(comma + 1, off);
}

static int parse_instr(Parser *p, RiboFunc *f, char **fld, uint32_t line) {
    const OpDef *d = find_op(fld[0]);
    RiboInstr in;
    int r;
    if (!d) return fail(p, line, "unknown opcode: %s", fld[0]);
    memset(&in, 0, sizeof in);
    in.op = d->op;
    in.line = line;

    switch (d->form) {
    case FM_LIT: {
        if (!strcmp(fld[2], "_")) return fail(p, line, "%s requires a literal", d->name);
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        in.dst = (uint8_t)r;
        if (d->cls == C_I) {
            int64_t v;
            if (!parse_i64(fld[2], &v)) return fail(p, line, "bad i32 literal: %s", fld[2]);
            if (v < INT32_MIN || v > INT32_MAX)
                return fail(p, line, "i32 literal out of range: %s", fld[2]);
            in.imm = v;
        } else if (d->cls == C_L) {
            if (!parse_i64(fld[2], &in.imm))
                return fail(p, line, "bad i64 literal: %s", fld[2]);
        } else if (d->cls == C_F) {
            char *end;
            float v = strtof(fld[2], &end);
            if (*end) return fail(p, line, "bad f32 literal: %s", fld[2]);
            { uint32_t b; memcpy(&b, &v, 4); in.imm = b; }
        } else {
            char *end;
            double v = strtod(fld[2], &end);
            if (*end) return fail(p, line, "bad f64 literal: %s", fld[2]);
            memcpy(&in.imm, &v, 8);
        }
        break;
    }
    case FM_MOV:
    case FM_BIN:
    case FM_CMP: {
        int64_t lit;
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        in.dst = (uint8_t)r;
        r = parse_reg(fld[2]);
        if (r >= 0) { in.src = (uint8_t)r; break; }
        if (d->form == FM_MOV)
            return fail(p, line, "expected a register, got '%s'", fld[2]);
        if (d->cls == C_F || d->cls == C_D)
            return fail(p, line,
                        "float opcodes take register operands only "
                        "(load constants with FLIT/DLIT)");
        if (!parse_i64(fld[2], &lit))
            return fail(p, line, "expected a register or literal, got '%s'", fld[2]);
        if (d->cls == C_I && (lit < INT32_MIN || lit > INT32_MAX))
            return fail(p, line, "i32 literal out of range: %s", fld[2]);
        in.src_is_lit = 1;
        in.imm = lit;
        /* compile-time checks from the contract */
        if ((d->op == OP_IDIV || d->op == OP_IMOD || d->op == OP_IDIVU ||
             d->op == OP_IMODU || d->op == OP_LDIV || d->op == OP_LMOD ||
             d->op == OP_LDIVU || d->op == OP_LMODU) && lit == 0)
            return fail(p, line, "division by literal zero");
        if (d->op == OP_ISHL || d->op == OP_ISHR || d->op == OP_ISAR) {
            if (lit < 0 || lit > 31)
                return fail(p, line, "literal shift count must be in 0..31");
        }
        if (d->op == OP_LSHL || d->op == OP_LSHR || d->op == OP_LSAR) {
            if (lit < 0 || lit > 63)
                return fail(p, line, "literal shift count must be in 0..63");
        }
        break;
    }
    case FM_UNARY:
    case FM_CVT:
    case FM_MSIZE:
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        if (strcmp(fld[2], "_"))
            return fail(p, line, "%s takes one operand; pad the third field with _", d->name);
        in.dst = (uint8_t)r;
        break;
    case FM_MGROW:
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        in.dst = (uint8_t)r;
        r = parse_reg(fld[2]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[2]);
        in.src = (uint8_t)r;
        break;
    case FM_LOAD: {
        int64_t off;
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        in.dst = (uint8_t)r;
        if (!parse_addr(fld[2], &r, &off))
            return fail(p, line, "expected base,offset (no spaces), got '%s'", fld[2]);
        if (off < INT32_MIN || off > INT32_MAX)
            return fail(p, line, "memory offset out of range: %s", fld[2]);
        in.src = (uint8_t)r;
        in.imm = off;
        break;
    }
    case FM_STORE: {
        int64_t off;
        if (!parse_addr(fld[1], &r, &off))
            return fail(p, line, "expected base,offset (no spaces), got '%s'", fld[1]);
        if (off < INT32_MIN || off > INT32_MAX)
            return fail(p, line, "memory offset out of range: %s", fld[1]);
        in.dst = (uint8_t)r; /* base register */
        in.imm = off;
        r = parse_reg(fld[2]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[2]);
        in.src = (uint8_t)r; /* value register */
        break;
    }
    case FM_GGET:
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        if (!is_ident(fld[2])) return fail(p, line, "bad global name: %s", fld[2]);
        in.dst = (uint8_t)r;
        if (!add_fixup(p, f - p->m->funcs, f->code_len, FX_GLOB, fld[2], line))
            return fail(p, line, "out of memory");
        break;
    case FM_GSET:
        if (!is_ident(fld[1])) return fail(p, line, "bad global name: %s", fld[1]);
        r = parse_reg(fld[2]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[2]);
        in.src = (uint8_t)r;
        if (!add_fixup(p, f - p->m->funcs, f->code_len, FX_GLOB, fld[1], line))
            return fail(p, line, "out of memory");
        break;
    case FM_JMP:
        if (!is_ident(fld[1])) return fail(p, line, "bad label name: %s", fld[1]);
        if (strcmp(fld[2], "_"))
            return fail(p, line, "%s takes one operand; pad the third field with _", d->name);
        in.imm = -1;
        {   /* resolve against labels collected so far, or record a forward ref */
            size_t i;
            for (i = 0; i < p->label_count; i++)
                if (!strcmp(p->labels[i].name, fld[1])) { in.imm = (int64_t)p->labels[i].index; break; }
            if (in.imm < 0) {
                /* store label name in a fixup-like local list: reuse labels with
                   a marker — simpler: keep a second pass list */
                if (!add_fixup(p, f - p->m->funcs, f->code_len, -1, fld[1], line))
                    return fail(p, line, "out of memory");
            }
        }
        break;
    case FM_JCC: {
        size_t i;
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        if (!is_ident(fld[2])) return fail(p, line, "bad label name: %s", fld[2]);
        in.dst = (uint8_t)r;
        in.imm = -1;
        for (i = 0; i < p->label_count; i++)
            if (!strcmp(p->labels[i].name, fld[2])) { in.imm = (int64_t)p->labels[i].index; break; }
        if (in.imm < 0 && !add_fixup(p, f - p->m->funcs, f->code_len, -1, fld[2], line))
            return fail(p, line, "out of memory");
        break;
    }
    case FM_CALL: {
        char buf[256], *tok, *save = NULL;
        if (!is_ident(fld[1])) return fail(p, line, "bad func name: %s", fld[1]);
        in.nargs = 0;
        if (strcmp(fld[2], "_")) {
            snprintf(buf, sizeof buf, "%s", fld[2]);
            for (tok = strtok_s(buf, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
                r = parse_reg(tok);
                if (r < 0) return fail(p, line, "expected a register, got '%s'", tok);
                if (in.nargs >= RIBO_MAX_ARGS)
                    return fail(p, line, "CALL accepts at most eight register arguments");
                in.args[in.nargs++] = (uint8_t)r;
            }
        }
        if (!add_fixup(p, f - p->m->funcs, f->code_len, FX_CALL, fld[1], line))
            return fail(p, line, "out of memory");
        break;
    }
    case FM_CALLI: {
        char buf[256], *tok, *save = NULL;
        r = parse_reg(fld[1]);
        if (r < 0) return fail(p, line, "expected a register, got '%s'", fld[1]);
        in.dst = (uint8_t)r; /* the table index register */
        in.nargs = 0;
        if (strcmp(fld[2], "_")) {
            snprintf(buf, sizeof buf, "%s", fld[2]);
            for (tok = strtok_s(buf, ",", &save); tok; tok = strtok_s(NULL, ",", &save)) {
                r = parse_reg(tok);
                if (r < 0) return fail(p, line, "expected a register, got '%s'", tok);
                if (in.nargs >= RIBO_MAX_ARGS)
                    return fail(p, line, "CALLI accepts at most eight register arguments");
                in.args[in.nargs++] = (uint8_t)r;
            }
        }
        break;
    }
    case FM_RET:
        if (!strcmp(fld[1], "_")) { in.nargs = 0; }
        else {
            r = parse_reg(fld[1]);
            if (r < 0) return fail(p, line, "expected a register or _, got '%s'", fld[1]);
            in.dst = (uint8_t)r;
            in.nargs = 1;
        }
        if (strcmp(fld[2], "_"))
            return fail(p, line, "RET takes one operand; pad the third field with _");
        break;
    case FM_TRAP:
        if (strcmp(fld[1], "_") || strcmp(fld[2], "_"))
            return fail(p, line, "TRAP takes no operands: TRAP _ _");
        break;
    default:
        return fail(p, line, "internal: unhandled form");
    }
    if (!emit(f, &in)) return fail(p, line, "out of memory");
    return 1;
}

/* ---------- module-level parsing ---------- */

static int module_add_func(RiboModule *m, RiboFunc **out) {
    RiboFunc *nf;
    if (m->func_count >= RIBO_MAX_FUNCS) return 0;
    if (m->func_count == m->func_cap) {
        size_t nc = m->func_cap ? m->func_cap * 2 : 16;
        nf = realloc(m->funcs, nc * sizeof *nf);
        if (!nf) return 0;
        m->funcs = nf;
        m->func_cap = nc;
    }
    nf = &m->funcs[m->func_count++];
    memset(nf, 0, sizeof *nf);
    *out = nf;
    return 1;
}

static int parse_decl(Parser *p, char **fld, uint32_t line, RiboFunc **cur) {
    RiboModule *m = p->m;
    if (!strcmp(fld[0], "FUNC")) {
        RiboFunc *f;
        int type, arity;
        if (*cur) return fail(p, line, "FUNC inside FUNC %s", (*cur)->name);
        if (!is_ident(fld[1])) return fail(p, line, "bad func name: %s", fld[1]);
        if (!parse_sig(fld[2], &type, &arity, 1))
            return fail(p, line, "bad signature (want i32/N..f64/N or void/N): %s", fld[2]);
        if (find_func(m, fld[1]) || find_import(m, fld[1]) >= 0)
            return fail(p, line, "duplicate func: %s", fld[1]);
        if (!module_add_func(m, &f)) return fail(p, line, "too many funcs");
        snprintf(f->name, sizeof f->name, "%s", fld[1]);
        f->ret = (uint8_t)type;
        f->arity = (uint8_t)arity;
        f->decl_line = line;
        p->label_count = 0;
        *cur = f;
        return 1;
    }
    if (!strcmp(fld[0], "END")) {
        if (strcmp(fld[1], "_") || strcmp(fld[2], "_"))
            return fail(p, line, "END takes no operands: END _ _");
        if (!*cur) return fail(p, line, "END outside FUNC");
        *cur = NULL;
        return 1;
    }
    if (*cur) {
        /* inside a FUNC body: LABEL or an instruction */
        if (!strcmp(fld[0], "LABEL")) {
            if (!is_ident(fld[1])) return fail(p, line, "bad label name: %s", fld[1]);
            if (strcmp(fld[2], "_"))
                return fail(p, line, "LABEL takes one operand; pad the third field with _");
            if (p->label_count >= 512)
                return fail(p, line, "too many labels in func %s", (*cur)->name);
            { size_t i;
              for (i = 0; i < p->label_count; i++)
                  if (!strcmp(p->labels[i].name, fld[1]))
                      return fail(p, line, "duplicate label %s in func %s",
                                  fld[1], (*cur)->name); }
            snprintf(p->labels[p->label_count].name, 64, "%s", fld[1]);
            p->labels[p->label_count++].index = (*cur)->code_len;
            return 1;
        }
        if (!strcmp(fld[0], "IMPORT") || !strcmp(fld[0], "EXPORT") ||
            !strcmp(fld[0], "GLOB") || !strcmp(fld[0], "MEM") ||
            !strcmp(fld[0], "TABLE") || !strcmp(fld[0], "TELEM") ||
            !strcmp(fld[0], "DATA"))
            return fail(p, line, "declaration inside FUNC %s", (*cur)->name);
        return parse_instr(p, *cur, fld, line);
    }
    if (!strcmp(fld[0], "IMPORT")) {
        RiboImportDecl *im;
        int type, arity;
        if (!is_ident(fld[1])) return fail(p, line, "bad import name: %s", fld[1]);
        if (!parse_sig(fld[2], &type, &arity, 1))
            return fail(p, line, "bad import signature: %s", fld[2]);
        if (find_import(m, fld[1]) >= 0 || find_func(m, fld[1]))
            return fail(p, line, "duplicate import: %s", fld[1]);
        if (m->import_count >= RIBO_MAX_IMPORTS)
            return fail(p, line, "too many imports");
        if (m->import_count == m->import_cap) {
            size_t nc = m->import_cap ? m->import_cap * 2 : 8;
            RiboImportDecl *ni = realloc(m->imports, nc * sizeof *ni);
            if (!ni) return fail(p, line, "out of memory");
            m->imports = ni;
            m->import_cap = nc;
        }
        im = &m->imports[m->import_count++];
        snprintf(im->name, sizeof im->name, "%s", fld[1]);
        im->ret = (uint8_t)type;
        im->arity = (uint8_t)arity;
        return 1;
    }
    if (!strcmp(fld[0], "EXPORT")) {
        if (!is_ident(fld[1])) return fail(p, line, "bad export name: %s", fld[1]);
        if (strcmp(fld[2], "_"))
            return fail(p, line, "EXPORT takes one operand: EXPORT name _");
        if (!add_fixup(p, SIZE_MAX, SIZE_MAX, FX_EXPORT, fld[1], line))
            return fail(p, line, "out of memory");
        return 1;
    }
    if (!strcmp(fld[0], "GLOB")) {
        RiboGlobal *g;
        int type;
        if (!is_ident(fld[1])) return fail(p, line, "bad global name: %s", fld[1]);
        if (!parse_type(fld[2], &type) || type == T_VOID)
            return fail(p, line, "bad global type (want i32/i64/f32/f64): %s", fld[2]);
        if (find_global(m, fld[1]) >= 0)
            return fail(p, line, "duplicate global: %s", fld[1]);
        if (m->glob_count == m->glob_cap) {
            size_t nc = m->glob_cap ? m->glob_cap * 2 : 8;
            RiboGlobal *ng = realloc(m->globals, nc * sizeof *ng);
            if (!ng) return fail(p, line, "out of memory");
            m->globals = ng;
            m->glob_cap = nc;
        }
        g = &m->globals[m->glob_count++];
        snprintf(g->name, sizeof g->name, "%s", fld[1]);
        g->type = (uint8_t)type;
        g->bits = 0;
        return 1;
    }
    if (!strcmp(fld[0], "GINIT")) {
        if (!is_ident(fld[1])) return fail(p, line, "bad global name: %s", fld[1]);
        if (!strcmp(fld[2], "_"))
            return fail(p, line, "GINIT requires a literal: GINIT name lit");
        if (!add_fixup2(p, SIZE_MAX, SIZE_MAX, FX_GINIT, fld[1], fld[2], line))
            return fail(p, line, "out of memory");
        return 1;
    }
    if (!strcmp(fld[0], "MEM")) {
        char buf[64];
        const char *comma = strchr(fld[2], ',');
        int64_t mn, mx = 0;
        if (m->has_mem) return fail(p, line, "duplicate MEM declaration");
        if (!is_ident(fld[1])) return fail(p, line, "bad memory name: %s", fld[1]);
        snprintf(buf, sizeof buf, "%s", fld[2]);
        if (comma) {
            buf[comma - fld[2]] = '\0';
            if (!parse_i64(buf, &mn) || !parse_i64(comma + 1, &mx))
                return fail(p, line, "bad MEM bounds: %s", fld[2]);
        } else if (!parse_i64(buf, &mn)) {
            return fail(p, line, "bad MEM bounds: %s", fld[2]);
        }
        if (mn < 0 || mn > (int64_t)RIBO_MAX_PAGES || mx < 0 ||
            mx > (int64_t)RIBO_MAX_PAGES || (mx && mx < mn))
            return fail(p, line, "MEM bounds out of range (0..%u): %s",
                        (unsigned)RIBO_MAX_PAGES, fld[2]);
        m->has_mem = 1;
        m->mem_min = (uint32_t)mn;
        m->mem_max = (uint32_t)mx;
        return 1;
    }
    if (!strcmp(fld[0], "TABLE")) {
        char buf[64];
        const char *comma = strchr(fld[2], ',');
        int64_t mn, mx = 0;
        if (m->has_table) return fail(p, line, "duplicate TABLE declaration");
        if (!is_ident(fld[1])) return fail(p, line, "bad table name: %s", fld[1]);
        snprintf(buf, sizeof buf, "%s", fld[2]);
        if (comma) {
            buf[comma - fld[2]] = '\0';
            if (!parse_i64(buf, &mn) || !parse_i64(comma + 1, &mx))
                return fail(p, line, "bad TABLE bounds: %s", fld[2]);
        } else if (!parse_i64(buf, &mn)) {
            return fail(p, line, "bad TABLE bounds: %s", fld[2]);
        }
        if (mn < 0 || mn > 1048576 || mx < 0 || mx > 1048576 || (mx && mx < mn))
            return fail(p, line, "TABLE bounds out of range (0..1048576): %s", fld[2]);
        m->has_table = 1;
        m->table_min = (uint32_t)mn;
        m->table_max = (uint32_t)mx;
        return 1;
    }
    if (!strcmp(fld[0], "TELEM")) {
        int64_t off;
        if (!parse_i64(fld[1], &off) || off < 0)
            return fail(p, line, "bad TELEM offset: %s", fld[1]);
        if (!strcmp(fld[2], "_"))
            return fail(p, line, "TELEM requires funcs: TELEM off f1,f2");
        if (!add_fixup2(p, SIZE_MAX, (size_t)off, FX_TELEM, "_", fld[2], line))
            return fail(p, line, "out of memory");
        return 1;
    }
    if (!strcmp(fld[0], "DATA")) {
        int64_t off;
        size_t hl, bl, i;
        RiboData *d;
        if (!parse_i64(fld[1], &off) || off < 0)
            return fail(p, line, "bad DATA offset: %s", fld[1]);
        hl = strlen(fld[2]);
        if (!hl || (hl & 1))
            return fail(p, line, "DATA wants an even-length hex string: %s", fld[2]);
        bl = hl / 2;
        if (m->data_count == m->data_cap) {
            size_t nc = m->data_cap ? m->data_cap * 2 : 8;
            RiboData *nd = realloc(m->datas, nc * sizeof *nd);
            if (!nd) return fail(p, line, "out of memory");
            m->datas = nd;
            m->data_cap = nc;
        }
        d = &m->datas[m->data_count];
        d->bytes = malloc(bl);
        if (!d->bytes) return fail(p, line, "out of memory");
        for (i = 0; i < bl; i++) {
            int h = hexnib(fld[2][2 * i]), lo = hexnib(fld[2][2 * i + 1]);
            if (h < 0 || lo < 0) {
                free(d->bytes);
                return fail(p, line, "bad hex in DATA: %s", fld[2]);
            }
            d->bytes[i] = (uint8_t)((h << 4) | lo);
        }
        d->off = (uint32_t)off;
        d->len = bl;
        m->data_count++;
        return 1;
    }
    return fail(p, line, "expected IMPORT/EXPORT/GLOB/GINIT/MEM/TABLE/TELEM/DATA/FUNC, got '%s'", fld[0]);
}

/* resolve label/CALL/GLOBAL/EXPORT fixups */
static int resolve(Parser *p) {
    RiboModule *m = p->m;
    size_t i, fi;
    /* exports: mark funcs */
    for (i = 0; i < p->fx_count; i++) {
        Fixup *fx = &p->fixups[i];
        if (fx->kind != FX_EXPORT) continue;
        { RiboFunc *f = find_func(m, fx->name);
          if (!f) return fail(p, fx->line, "undefined func in EXPORT: %s", fx->name);
          f->exported = 1; }
    }
    for (i = 0; i < p->fx_count; i++) {
        Fixup *fx = &p->fixups[i];
        RiboInstr *in = NULL;
        if (fx->kind == FX_EXPORT) continue;
        if (fx->kind != FX_GINIT)
            in = &m->funcs[fx->func].code[fx->instr];
        if (fx->kind == -1) { /* jump label */
            /* labels were per-func; recompute: find the parser label list of
               that func — labels were reset per func, so resolve jumps now
               from the func's own instruction stream is impossible. Jumps are
               resolved at END instead (see parse loop). */
            continue;
        }
        if (fx->kind == FX_CALL) {
            RiboFunc *callee = find_func(m, fx->name);
            int imp;
            if (callee) in->imm = (int64_t)(callee - m->funcs);
            else if ((imp = find_import(m, fx->name)) >= 0)
                in->imm = (int64_t)m->func_count + imp;
            else return fail(p, fx->line, "undefined func: %s", fx->name);
        } else if (fx->kind == FX_GLOB) {
            int g = find_global(m, fx->name);
            if (g < 0) return fail(p, fx->line, "undefined global: %s", fx->name);
            in->imm = g;
        } else if (fx->kind == FX_GINIT) {
            int g = find_global(m, fx->name);
            RiboGlobal *gl;
            if (g < 0) return fail(p, fx->line, "undefined global in GINIT: %s", fx->name);
            gl = &m->globals[g];
            if (gl->type == T_I32) {
                int64_t v;
                if (!parse_i64(fx->text, &v) || v < INT32_MIN || v > INT32_MAX)
                    return fail(p, fx->line, "bad i32 literal: %s", fx->text);
                gl->bits = (uint64_t)(int64_t)(int32_t)v;
            } else if (gl->type == T_I64) {
                int64_t v;
                if (!parse_i64(fx->text, &v))
                    return fail(p, fx->line, "bad i64 literal: %s", fx->text);
                gl->bits = (uint64_t)v;
            } else if (gl->type == T_F32) {
                char *end;
                float v = strtof(fx->text, &end);
                uint32_t b;
                if (*end) return fail(p, fx->line, "bad f32 literal: %s", fx->text);
                memcpy(&b, &v, 4);
                gl->bits = b;
            } else {
                char *end;
                double v = strtod(fx->text, &end);
                if (*end) return fail(p, fx->line, "bad f64 literal: %s", fx->text);
                memcpy(&gl->bits, &v, 8);
            }
        }
    }
    /* table elements: TELEM off f1,f2,... */
    for (i = 0; i < p->fx_count; i++) {
        Fixup *fx = &p->fixups[i];
        char buf[80], *tok, *save = NULL;
        uint32_t off;
        if (fx->kind != FX_TELEM) continue;
        off = (uint32_t)fx->instr;
        snprintf(buf, sizeof buf, "%s", fx->text);
        for (tok = strtok_s(buf, ",", &save); tok; tok = strtok_s(NULL, ",", &save), off++) {
            RiboFunc *callee = find_func(m, tok);
            int imp;
            RiboElem *el;
            if (m->elem_count == m->elem_cap) {
                size_t nc = m->elem_cap ? m->elem_cap * 2 : 8;
                RiboElem *ne = realloc(m->elems, nc * sizeof *ne);
                if (!ne) return fail(p, fx->line, "out of memory");
                m->elems = ne;
                m->elem_cap = nc;
            }
            el = &m->elems[m->elem_count++];
            el->off = off;
            if (callee) el->func = (uint32_t)(callee - m->funcs);
            else if ((imp = find_import(m, tok)) >= 0)
                el->func = (uint32_t)m->func_count + imp;
            else return fail(p, fx->line, "undefined func in TELEM: %s", tok);
        }
    }
    /* second pass: resolve forward jump fixups now that all labels are known.
       Label lists are per-func and were reset; so instead jumps were recorded
       with kind -1 and must be resolved here against a re-scan of labels.
       We re-scan: labels are not stored in code, so we track them in a
       per-func array saved at END. */
    (void)fi;
    return 1;
}

/* We need labels to survive until after the whole module parse for forward
   jumps. Simplest correct approach: resolve jumps at END of each FUNC
   (labels are per-func, and by END all labels of that func are known). */
static int resolve_jumps_at_end(Parser *p, RiboFunc *f) {
    size_t i;
    for (i = 0; i < p->fx_count; i++) {
        Fixup *fx = &p->fixups[i];
        size_t j;
        if (fx->kind != -1 || fx->func != (size_t)(f - p->m->funcs)) continue;
        for (j = 0; j < p->label_count; j++) {
            if (!strcmp(p->labels[j].name, fx->name)) {
                p->m->funcs[fx->func].code[fx->instr].imm = (int64_t)p->labels[j].index;
                fx->kind = -2; /* done */
                break;
            }
        }
        if (fx->kind == -1)
            return fail(p, fx->line, "undefined label %s in func %s",
                        fx->name, f->name);
    }
    return 1;
}

/* ---------- public entry ---------- */

static void module_free(RiboModule *m) {
    size_t i;
    if (!m) return;
    for (i = 0; i < m->func_count; i++) free(m->funcs[i].code);
    for (i = 0; i < m->data_count; i++) free(m->datas[i].bytes);
    free(m->datas);
    free(m->elems);
    free(m->funcs);
    free(m->globals);
    free(m->imports);
    free(m->lines);
    free(m);
}

int ribo_parse_source(const char *text, int repair, RiboModule **out,
                      RiboDiag *diag) {
    Parser p;
    RiboModule *m;
    RiboFunc *cur = NULL;
    char *buf, *line, *save = NULL;
    uint32_t lineno = 0;
    size_t cap = 0;

    *out = NULL;
    /* a UTF-8 BOM is skipped, not an error */
    if ((unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF)
        text += 3;
    memset(&p, 0, sizeof p);
    p.repair = repair;
    p.diag = diag;
    m = calloc(1, sizeof *m);
    if (!m) { ribo_diag_set(diag, "parse", 0, "", "out of memory"); return 0; }
    p.m = m;

    if (strlen(text) > 4u * 1024u * 1024u) {
        ribo_diag_set(diag, "parse", 0, "", "file exceeds 4 MiB");
        module_free(m);
        return 0;
    }
    buf = malloc(strlen(text) + 1);
    if (!buf) { module_free(m); ribo_diag_set(diag, "parse", 0, "", "out of memory"); return 0; }
    strcpy(buf, text);

    for (line = strtok_s(buf, "\n", &save); line; line = strtok_s(NULL, "\n", &save)) {
        char raw[512];
        char *f[3];
        char *hash, *cr;
        int nf;
        lineno++;
        cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        if (strlen(line) > 511) {
            fail(&p, lineno, "line exceeds 511 bytes");
            goto fail;
        }
        /* keep the raw line for line_text diagnostics */
        if (m->line_count == cap) {
            size_t nc = cap ? cap * 2 : 64;
            char **nl = realloc(m->lines, nc * sizeof *nl);
            if (!nl) { fail(&p, lineno, "out of memory"); goto fail; }
            m->lines = nl;
            cap = nc;
        }
        snprintf(raw, sizeof raw, "%s", line);
        { char *dup = malloc(strlen(raw) + 1);
          if (!dup) { fail(&p, lineno, "out of memory"); goto fail; }
          strcpy(dup, raw);
          m->lines[m->line_count++] = dup; }
        hash = strchr(line, '#');
        if (hash) *hash = '\0';
        nf = split_fields(line, f);
        if (nf == 0) continue;
        if (nf == 4 || (nf != 3 && !repair)) {
            fail(&p, lineno, "expected exactly 3 fields; found %d", nf);
            goto fail;
        }
        if (nf == 1) { f[1] = (char *)"_"; f[2] = (char *)"_"; }
        else if (nf == 2) { f[2] = (char *)"_"; }
        if (!parse_decl(&p, f, lineno, &cur)) goto fail;
        /* resolve per-func jumps when a func closes */
        if (cur == NULL) {
            /* parse_decl may have just closed a func; resolve jumps for the
               most recently completed func */
            if (m->func_count > 0 && !strcmp(f[0], "END")) {
                if (!resolve_jumps_at_end(&p, &m->funcs[m->func_count - 1]))
                    goto fail;
            }
        }
    }
    if (cur) {
        fail(&p, lineno, "FUNC %s is not closed with END _ _", cur->name);
        goto fail;
    }
    if (!resolve(&p)) goto fail;
    free(p.fixups);
    free(buf);
    *out = m;
    return 1;

fail:
    free(p.fixups);
    free(buf);
    module_free(m);
    return 0;
}

/* ---------- public API ---------- */

RiboModule *ribo_compile(const char *path, int repair, RiboError *error) {
    FILE *fp;
    long size;
    char *text;
    RiboModule *m = NULL;
    RiboDiag diag;
    memset(&diag, 0, sizeof diag);

    fp = fopen(path, "rb");
    if (!fp) {
        ribo_diag_set(&diag, "parse", 0, "", "cannot open file: %s", path);
        ribo_diag_to_public(&diag, error);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0 || (unsigned long)size > 4u * 1024u * 1024u) {
        fclose(fp);
        ribo_diag_set(&diag, "parse", 0, "", "file exceeds 4 MiB");
        ribo_diag_to_public(&diag, error);
        return NULL;
    }
    text = malloc((size_t)size + 1);
    if (!text) { fclose(fp); ribo_diag_set(&diag, "parse", 0, "", "out of memory");
                 ribo_diag_to_public(&diag, error); return NULL; }
    if (size > 0 && fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        fclose(fp);
        ribo_diag_set(&diag, "parse", 0, "", "cannot read file: %s", path);
        ribo_diag_to_public(&diag, error);
        return NULL;
    }
    fclose(fp);
    text[size] = '\0';

    if (!ribo_parse_source(text, repair, &m, &diag) ||
        !ribo_check_module(m, &diag)) {
        if (m) module_free(m);
        free(text);
        ribo_diag_to_public(&diag, error);
        return NULL;
    }
    free(text);
    return m;
}

void ribo_module_release(RiboModule *module) { module_free(module); }

/* In-memory variant for hosts without a filesystem (the browser shell). */
RiboModule *ribo_compile_text(const char *text, int repair, RiboError *error) {
    RiboModule *m = NULL;
    RiboDiag diag;
    memset(&diag, 0, sizeof diag);
    if (!ribo_parse_source(text, repair, &m, &diag) ||
        !ribo_check_module(m, &diag)) {
        if (m) module_free(m);
        ribo_diag_to_public(&diag, error);
        return NULL;
    }
    return m;
}
