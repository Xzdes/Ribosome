/* lift.c — wasm binary -> .rna text (the reverse direction).

   Strategy (docs/architecture.md):
   - The wasm operand stack lifts to registers by static stack-depth
     modelling: slot k of the stack becomes register (8 + nlocals + k).
     r0..r7 are scratch (CALL results land in r0), wasm locals live at r8+.
   - Structured control flow lowers mechanically: every block/loop/if gets
     labels; br/br_if/br_table become JMP/JZ/JNZ cascades; a branch to a
     block with a result first MOVes the top value to the target base slot.
   - wasm opcodes are explicitly typed, so no type stack is needed: the
     opcode itself chooses the I/L/F/D prefix.
   - Input is untrusted: every read is bounds-checked, every unsupported
     feature is a clean error, never a crash. */
#include "ribo_internal.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* local text buffer (the byte buffer in emit_wasm.c is TU-private) */
typedef struct { char *data; size_t len, cap; } TBuf;

static int tb_putn(TBuf *b, const char *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        char *nd;
        while (nc < b->len + n) nc *= 2;
        nd = realloc(b->data, nc);
        if (!nd) return 0;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 1;
}

/* ---------- wasm module model ---------- */

typedef struct { uint8_t params[8]; uint8_t nparams; uint8_t result; } WType;

typedef struct {
    char name[96];        /* sanitized "module.field" */
    uint32_t type;
} WImport;

typedef struct {
    uint32_t type;
    uint32_t body_off, body_len;   /* into the original bytes (expr part) */
    uint8_t nlocal_runs;
    uint32_t local_counts[64];
    uint8_t local_types[64];
} WFunc;

typedef struct { char name[64]; uint32_t func; } WExport;
typedef struct { uint8_t type; uint64_t bits; } WGlobal;
typedef struct { uint32_t off; uint32_t func; } WElem;   /* func in wasm idx space */
typedef struct { uint32_t off; uint32_t data_off; uint32_t len; } WData;

typedef struct RiboWasm {
    uint8_t *bytes;
    size_t len;
    WType *types; size_t ntypes;
    WImport *imports; size_t nimports;
    WFunc *funcs; size_t nfuncs;
    WExport *exports; size_t nexports;
    WGlobal *globals; size_t nglobals;
    int has_mem;
    uint32_t mem_min, mem_max;
    int has_table;
    uint32_t table_min, table_max;
    WElem *elems; size_t nelems, elems_cap;
    WData *datas; size_t ndatas, datas_cap;
} RiboWasm;

/* ---------- bounds-checked reader ---------- */

typedef struct { const uint8_t *p, *end; int err; } Rd;

static uint8_t rd_u8(Rd *r) {
    if (r->p >= r->end) { r->err = 1; return 0; }
    return *r->p++;
}

static uint32_t rd_u32(Rd *r) {
    uint32_t v = 0;
    int shift = 0, i;
    for (i = 0; i < 5; i++) {
        uint8_t b = rd_u8(r);
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return v;
        shift += 7;
    }
    r->err = 1;
    return 0;
}

static int64_t rd_i64(Rd *r, unsigned bits) {
    int64_t v = 0;
    int shift = 0;
    unsigned i, max = bits / 7 + 1;
    uint8_t b = 0;
    for (i = 0; i < max; i++) {
        b = rd_u8(r);
        v |= (int64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= -(1ll << shift);
            return v;
        }
    }
    r->err = 1;
    return 0;
}

static void rd_bytes(Rd *r, void *dst, size_t n) {
    if ((size_t)(r->end - r->p) < n) { r->err = 1; memset(dst, 0, n); return; }
    memcpy(dst, r->p, n);
    r->p += n;
}

/* ---------- small utilities ---------- */

static int vt_to_t(uint8_t vt) {
    switch (vt) {
    case 0x7F: return T_I32;
    case 0x7E: return T_I64;
    case 0x7D: return T_F32;
    case 0x7C: return T_F64;
    default: return -1;
    }
}

static const char *t_name(int t) {
    switch (t) {
    case T_I32: return "i32";
    case T_I64: return "i64";
    case T_F32: return "f32";
    case T_F64: return "f64";
    default: return "void";
    }
}

static void sanitize(char *out, size_t cap, const char *in) {
    size_t i = 0;
    if (!in[0] || !(isalpha((unsigned char)in[0]) || in[0] == '_'))
        out[i++] = 'f';
    for (; *in && i + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        out[i++] = (isalnum(c) || c == '_' || c == '.') ? (char)c : '_';
    }
    out[i] = '\0';
}

/* ---------- module parsing ---------- */

static int wfail(RiboError *error, const char *fmt, ...) {
    va_list ap;
    if (!error) return 0;
    va_start(ap, fmt);
    vsnprintf(error->message, sizeof error->message, fmt, ap);
    va_end(ap);
    return 0;
}

RiboWasm *ribo_wasm_parse(const uint8_t *bytes, size_t length, RiboError *error) {
    RiboWasm *w;
    Rd r;
    if (length < 8 || memcmp(bytes, "\0asm\x01\0\0\0", 8)) {
        wfail(error, "lift: not a wasm MVP binary");
        return NULL;
    }
    w = calloc(1, sizeof *w);
    if (!w) { wfail(error, "lift: out of memory"); return NULL; }
    w->bytes = malloc(length);
    if (!w->bytes) { free(w); wfail(error, "lift: out of memory"); return NULL; }
    memcpy(w->bytes, bytes, length);
    w->len = length;
    r.p = bytes + 8;
    r.end = bytes + length;
    r.err = 0;

    while (r.p < r.end && !r.err) {
        uint8_t id = rd_u8(&r);
        uint32_t size = rd_u32(&r);
        const uint8_t *sec_end;
        if (r.err || (size_t)(r.end - r.p) < size) { wfail(error, "lift: truncated section"); goto fail; }
        sec_end = r.p + size;
        switch (id) {
        case 0: r.p = sec_end; break; /* custom: skipped */
        case 1: { /* types */
            uint32_t n = rd_u32(&r), i;
            w->types = calloc(n ? n : 1, sizeof *w->types);
            if (!w->types) { wfail(error, "lift: out of memory"); goto fail; }
            w->ntypes = n;
            for (i = 0; i < n; i++) {
                uint32_t np, nr, k;
                if (rd_u8(&r) != 0x60) { wfail(error, "lift: bad functype"); goto fail; }
                np = rd_u32(&r);
                if (np > RIBO_MAX_ARGS) { wfail(error, "lift: func type with %u params exceeds the 8-arg limit", np); goto fail; }
                w->types[i].nparams = (uint8_t)np;
                for (k = 0; k < np; k++) rd_u8(&r); /* param types: cells anyway */
                nr = rd_u32(&r);
                if (nr > 1) { wfail(error, "lift: multi-value results not supported"); goto fail; }
                w->types[i].result = nr ? (uint8_t)vt_to_t(rd_u8(&r)) : T_VOID;
            }
            break;
        }
        case 2: { /* imports */
            uint32_t n = rd_u32(&r), i;
            w->imports = calloc(n ? n : 1, sizeof *w->imports);
            if (!w->imports) { wfail(error, "lift: out of memory"); goto fail; }
            for (i = 0; i < n; i++) {
                char mod[64], field[64], joined[160];
                uint32_t ml = rd_u32(&r), fl, k;
                if (ml >= sizeof mod) { wfail(error, "lift: import name too long"); goto fail; }
                rd_bytes(&r, mod, ml);
                mod[ml] = '\0';
                fl = rd_u32(&r);
                if (fl >= sizeof field) { wfail(error, "lift: import name too long"); goto fail; }
                rd_bytes(&r, field, fl);
                field[fl] = '\0';
                if (rd_u8(&r) != 0x00) { wfail(error, "lift: only function imports are supported"); goto fail; }
                snprintf(joined, sizeof joined, "%s.%s", mod, field);
                sanitize(w->imports[i].name, sizeof w->imports[i].name, joined);
                w->imports[i].type = rd_u32(&r);
                w->nimports++;
                (void)k;
            }
            break;
        }
        case 3: { /* functions */
            uint32_t n = rd_u32(&r), i;
            w->funcs = calloc(n ? n : 1, sizeof *w->funcs);
            if (!w->funcs) { wfail(error, "lift: out of memory"); goto fail; }
            w->nfuncs = n;
            for (i = 0; i < n; i++) w->funcs[i].type = rd_u32(&r);
            break;
        }
        case 4: { /* table */
            uint32_t n = rd_u32(&r);
            uint32_t flags;
            if (n != 1) { wfail(error, "lift: expected at most one table"); goto fail; }
            if (rd_u8(&r) != 0x70) { wfail(error, "lift: only funcref tables"); goto fail; }
            flags = rd_u32(&r);
            w->table_min = rd_u32(&r);
            w->table_max = (flags & 1) ? rd_u32(&r) : 0;
            w->has_table = 1;
            break;
        }
        case 5: { /* memory */
            uint32_t n = rd_u32(&r);
            uint32_t flags;
            if (n != 1) { wfail(error, "lift: expected exactly one memory"); goto fail; }
            flags = rd_u32(&r);
            w->mem_min = rd_u32(&r);
            w->mem_max = (flags & 1) ? rd_u32(&r) : 0;
            w->has_mem = 1;
            break;
        }
        case 6: { /* globals */
            uint32_t n = rd_u32(&r), i;
            w->globals = calloc(n ? n : 1, sizeof *w->globals);
            if (!w->globals) { wfail(error, "lift: out of memory"); goto fail; }
            w->nglobals = n;
            for (i = 0; i < n; i++) {
                int t = vt_to_t(rd_u8(&r));
                uint8_t op;
                rd_u8(&r); /* mutability */
                if (t < 0) { wfail(error, "lift: bad global type"); goto fail; }
                w->globals[i].type = (uint8_t)t;
                op = rd_u8(&r);
                if (op == 0x41) w->globals[i].bits = (uint64_t)(int64_t)(int32_t)rd_i64(&r, 32);
                else if (op == 0x42) w->globals[i].bits = (uint64_t)rd_i64(&r, 64);
                else if (op == 0x43) { uint32_t b; rd_bytes(&r, &b, 4); w->globals[i].bits = b; }
                else if (op == 0x44) { uint64_t b; rd_bytes(&r, &b, 8); w->globals[i].bits = b; }
                else { wfail(error, "lift: non-constant global initializer"); goto fail; }
                if (rd_u8(&r) != 0x0B) { wfail(error, "lift: bad global init expr"); goto fail; }
            }
            break;
        }
        case 7: { /* exports */
            uint32_t n = rd_u32(&r), i;
            w->exports = calloc(n ? n : 1, sizeof *w->exports);
            if (!w->exports) { wfail(error, "lift: out of memory"); goto fail; }
            for (i = 0; i < n; i++) {
                char name[64];
                uint32_t nl = rd_u32(&r), kind;
                if (nl >= sizeof name) { wfail(error, "lift: export name too long"); goto fail; }
                rd_bytes(&r, name, nl);
                name[nl] = '\0';
                kind = rd_u8(&r);
                if (kind == 0x00) {
                    sanitize(w->exports[w->nexports].name, 64, name);
                    w->exports[w->nexports].func = rd_u32(&r);
                    w->nexports++;
                } else {
                    rd_u32(&r); /* non-func export: ignored */
                }
            }
            break;
        }
        case 8: wfail(error, "lift: start section not supported"); goto fail;
        case 9: { /* element segments: active, table 0, i32.const offset only */
            uint32_t n = rd_u32(&r), i, k;
            for (i = 0; i < n; i++) {
                uint32_t flags = rd_u32(&r), cnt;
                uint8_t op;
                int64_t off;
                if (flags != 0) { wfail(error, "lift: only active table-0 element segments"); goto fail; }
                op = rd_u8(&r);
                if (op != 0x41) { wfail(error, "lift: non-constant element offset"); goto fail; }
                off = rd_i64(&r, 32);
                if (rd_u8(&r) != 0x0B) { wfail(error, "lift: bad element offset expr"); goto fail; }
                cnt = rd_u32(&r);
                for (k = 0; k < cnt; k++) {
                    WElem *el;
                    if (w->nelems == w->elems_cap) {
                        size_t nc = w->elems_cap ? w->elems_cap * 2 : 8;
                        WElem *ne = realloc(w->elems, nc * sizeof *ne);
                        if (!ne) { wfail(error, "lift: out of memory"); goto fail; }
                        w->elems = ne;
                        w->elems_cap = nc;
                    }
                    el = &w->elems[w->nelems++];
                    el->off = (uint32_t)(off + k);
                    el->func = rd_u32(&r);
                }
            }
            break;
        }
        case 10: { /* code */
            uint32_t n = rd_u32(&r), i;
            if (n != w->nfuncs) { wfail(error, "lift: code/func count mismatch"); goto fail; }
            for (i = 0; i < n; i++) {
                uint32_t blen = rd_u32(&r), nl, k;
                const uint8_t *bend;
                if ((size_t)(sec_end - r.p) < blen) { wfail(error, "lift: truncated code"); goto fail; }
                bend = r.p + blen;
                nl = rd_u32(&r);
                if (nl > 64) { wfail(error, "lift: too many local runs"); goto fail; }
                w->funcs[i].nlocal_runs = (uint8_t)nl;
                for (k = 0; k < nl; k++) {
                    w->funcs[i].local_counts[k] = rd_u32(&r);
                    w->funcs[i].local_types[k] = rd_u8(&r);
                }
                w->funcs[i].body_off = (uint32_t)(r.p - bytes); /* r walks the ORIGINAL buffer */
                w->funcs[i].body_len = (uint32_t)(bend - r.p);
                r.p = bend;
            }
            break;
        }
        case 11: { /* data segments: active, memory 0, i32.const offset only */
            uint32_t n = rd_u32(&r), i;
            for (i = 0; i < n; i++) {
                uint32_t flags = rd_u32(&r), len;
                uint8_t op;
                int64_t off;
                WData *d;
                if (flags != 0) { wfail(error, "lift: only active memory-0 data segments"); goto fail; }
                op = rd_u8(&r);
                if (op != 0x41) { wfail(error, "lift: non-constant data offset"); goto fail; }
                off = rd_i64(&r, 32);
                if (rd_u8(&r) != 0x0B) { wfail(error, "lift: bad data offset expr"); goto fail; }
                len = rd_u32(&r);
                if ((size_t)(sec_end - r.p) < len) { wfail(error, "lift: truncated data segment"); goto fail; }
                if (w->ndatas == w->datas_cap) {
                    size_t nc = w->datas_cap ? w->datas_cap * 2 : 8;
                    WData *nd = realloc(w->datas, nc * sizeof *nd);
                    if (!nd) { wfail(error, "lift: out of memory"); goto fail; }
                    w->datas = nd;
                    w->datas_cap = nc;
                }
                d = &w->datas[w->ndatas++];
                d->off = (uint32_t)off;
                d->data_off = (uint32_t)(r.p - bytes); /* into the copy later */
                d->len = len;
                r.p += len;
            }
            break;
        }
        default: wfail(error, "lift: unknown section id %u", (unsigned)id); goto fail;
        }
        if (r.err) { wfail(error, "lift: malformed binary"); goto fail; }
        r.p = sec_end;
    }
    return w;

fail:
    ribo_wasm_release(w);
    return NULL;
}

void ribo_wasm_release(RiboWasm *wasm) {
    if (!wasm) return;
    free(wasm->bytes);
    free(wasm->types);
    free(wasm->imports);
    free(wasm->funcs);
    free(wasm->exports);
    free(wasm->globals);
    free(wasm->elems);
    free(wasm->datas);
    free(wasm);
}

/* ---------- the lifter ---------- */

#define CTL_MAX 256
#define SCRATCH 8 /* r0..r7 reserved: r0 is the CALL result cell */

typedef struct {
    int kind;          /* 0 block, 1 loop, 2 if */
    int end_l, else_l, header_l;
    uint32_t base;
    int has_result;
    int else_seen;
} Ctl;

typedef struct {
    const RiboWasm *w;
    TBuf *out;
    Rd r;
    Ctl ctl[CTL_MAX];
    int ctl_n;
    uint32_t height, max_height;
    uint32_t nlocals;
    int label_n;
    int unreachable;
    int err;
    char errmsg[512];
} Lift;

static void lfail(Lift *l, const char *fmt, ...) {
    va_list ap;
    if (l->err) return;
    l->err = 1;
    va_start(ap, fmt);
    vsnprintf(l->errmsg, sizeof l->errmsg, fmt, ap);
    va_end(ap);
}

static void lemit(Lift *l, const char *fmt, ...) {
    char line[256];
    va_list ap;
    if (l->err) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    tb_putn(l->out, line, strlen(line));
    tb_putn(l->out, "\n", 1);
}

static int new_label(Lift *l) { return l->label_n++; }
static uint32_t slotreg(Lift *l, uint32_t h) {
    uint32_t r = SCRATCH + l->nlocals + h;
    if (r >= RIBO_MAX_REGISTERS)
        lfail(l, "function too large to lift (register budget exhausted)");
    return r;
}
static uint32_t localreg(Lift *l, uint32_t x) {
    if (SCRATCH + x >= RIBO_MAX_REGISTERS)
        lfail(l, "too many locals to lift (register budget exhausted)");
    return SCRATCH + x;
}

static const char *func_name(Lift *l, uint32_t fidx, char *buf, size_t cap) {
    const RiboWasm *w = l->w;
    size_t i;
    if (fidx < w->nimports) return w->imports[fidx].name;
    for (i = 0; i < w->nexports; i++)
        if (w->exports[i].func == fidx) {
            snprintf(buf, cap, "%s", w->exports[i].name);
            return buf;
        }
    snprintf(buf, cap, "f%u", fidx - (unsigned)w->nimports);
    return buf;
}

static int type_result(Lift *l, uint32_t tidx) {
    if (tidx >= l->w->ntypes) { lfail(l, "bad type index"); return T_VOID; }
    return l->w->types[tidx].result;
}
static int type_nparams(Lift *l, uint32_t tidx) {
    if (tidx >= l->w->ntypes) { lfail(l, "bad type index"); return 0; }
    return l->w->types[tidx].nparams;
}

/* branch to a control target: move the result value, then jump */
static void emit_branch(Lift *l, uint32_t depth) {
    Ctl *c;
    if (depth >= (uint32_t)l->ctl_n) { lfail(l, "bad branch depth"); return; }
    c = &l->ctl[l->ctl_n - 1 - depth];
    if (c->kind != 1 && c->has_result) /* loop labels take no values (MVP) */
        lemit(l, "MOV r%u r%u", slotreg(l, c->base), slotreg(l, l->height - 1));
    lemit(l, "JMP L%d _", c->kind == 1 ? c->header_l : c->end_l);
}

/* opcode name tables for numeric ops */
static const char *binop_name(uint8_t op) {
    switch (op) {
    case 0x6A: return "IADD"; case 0x6B: return "ISUB"; case 0x6C: return "IMUL";
    case 0x6D: return "IDIV"; case 0x6E: return "IDIVU"; case 0x6F: return "IMOD";
    case 0x70: return "IMODU"; case 0x71: return "IAND"; case 0x72: return "IOR";
    case 0x73: return "IXOR";
    case 0x77: return "IROTL"; case 0x78: return "IROTR";
    case 0x7C: return "LADD"; case 0x7D: return "LSUB"; case 0x7E: return "LMUL";
    case 0x7F: return "LDIV"; case 0x80: return "LDIVU"; case 0x81: return "LMOD";
    case 0x82: return "LMODU"; case 0x83: return "LAND"; case 0x84: return "LOR";
    case 0x85: return "LXOR";
    case 0x89: return "LROTL"; case 0x8A: return "LROTR";
    case 0x92: return "FADD"; case 0x93: return "FSUB"; case 0x94: return "FMUL";
    case 0x95: return "FDIV"; case 0x96: return "FMIN"; case 0x97: return "FMAX";
    case 0xA0: return "DADD"; case 0xA1: return "DSUB"; case 0xA2: return "DMUL";
    case 0xA3: return "DDIV"; case 0xA4: return "DMIN"; case 0xA5: return "DMAX";
    default: return NULL;
    }
}

static const char *cmpop_name(uint8_t op) {
    switch (op) {
    case 0x46: return "IEQ"; case 0x47: return "INE";
    case 0x48: return "ILT"; case 0x49: return "ILTU";
    case 0x4A: return "IGT"; case 0x4B: return "IGTU";
    case 0x4C: return "ILE"; case 0x4D: return "ILEU";
    case 0x4E: return "IGE"; case 0x4F: return "IGEU";
    case 0x51: return "LEQ"; case 0x52: return "LNE";
    case 0x53: return "LLT"; case 0x54: return "LLTU";
    case 0x55: return "LGT"; case 0x56: return "LGTU";
    case 0x57: return "LLE"; case 0x58: return "LLEU";
    case 0x59: return "LGE"; case 0x5A: return "LGEU";
    case 0x5B: return "FEQ"; case 0x5C: return "FNE"; case 0x5D: return "FLT";
    case 0x5E: return "FGT"; case 0x5F: return "FLE"; case 0x60: return "FGE";
    case 0x61: return "DEQ"; case 0x62: return "DNE"; case 0x63: return "DLT";
    case 0x64: return "DGT"; case 0x65: return "DLE"; case 0x66: return "DGE";
    default: return NULL;
    }
}

static const char *load_name(uint8_t op) {
    switch (op) {
    case 0x28: return "IGET"; case 0x29: return "LGET";
    case 0x2A: return "FGET"; case 0x2B: return "DGET";
    case 0x2C: return "IGET8S"; case 0x2D: return "IGET8U";
    case 0x2E: return "IGET16S"; case 0x2F: return "IGET16U";
    case 0x30: return "LGET8S"; case 0x31: return "LGET8U";
    case 0x32: return "LGET16S"; case 0x33: return "LGET16U";
    case 0x34: return "LGET32S"; case 0x35: return "LGET32U";
    default: return NULL;
    }
}

static const char *store_name(uint8_t op) {
    switch (op) {
    case 0x36: return "IPUT"; case 0x37: return "LPUT";
    case 0x38: return "FPUT"; case 0x39: return "DPUT";
    case 0x3A: return "IPUT8"; case 0x3B: return "IPUT16";
    case 0x3C: return "LPUT8"; case 0x3D: return "LPUT16";
    case 0x3E: return "LPUT32";
    default: return NULL;
    }
}

static const char *cvt_name(uint8_t op) {
    switch (op) {
    case 0xA7: return "L2I"; case 0xA8: return "F2I"; case 0xAA: return "D2I";
    case 0xAC: return "I2L"; case 0xAD: return "I2LU";
    case 0xAE: return "F2L"; case 0xB0: return "D2L";
    case 0xA9: return "F2IU"; case 0xAB: return "D2IU";
    case 0xAF: return "F2LU"; case 0xB1: return "D2LU";
    case 0xB2: return "I2F"; case 0xB4: return "L2F"; case 0xB6: return "D2F";
    case 0xB8: return "I2D"; case 0xBA: return "L2D"; case 0xBB: return "F2D";
    default: return NULL;
    }
}

/* block type: 0x40 empty, valtype, or s33 type index (multi-value: nope) */
static int rd_blocktype(Lift *l) {
    uint8_t b = rd_u8(&l->r);
    int t;
    if (b == 0x40) return T_VOID;
    t = vt_to_t(b);
    if (t < 0) lfail(l, "multi-value block types not supported");
    return t;
}

static void lift_func(Lift *l, uint32_t fidx) {
    const RiboWasm *w = l->w;
    const WFunc *f = &w->funcs[fidx - (uint32_t)w->nimports];
    const WType *t = &w->types[f->type];
    char name[96];
    uint32_t i;
    int done = 0;

    l->nlocals = t->nparams;
    for (i = 0; i < f->nlocal_runs; i++) l->nlocals += f->local_counts[i];
    l->height = 0;
    l->max_height = 0;
    l->ctl_n = 0;
    l->label_n = 0;
    l->unreachable = 0;

    lemit(l, "FUNC %s %s/%u", func_name(l, fidx, name, sizeof name),
          t_name(t->result), (unsigned)t->nparams);
    /* params arrive in r0..r(N-1); move them to their local cells r8+ */
    for (i = 0; i < t->nparams; i++)
        lemit(l, "MOV r%u r%u", localreg(l, i), i);
    /* wasm locals are zero-initialized; our registers are not */
    for (i = t->nparams; i < l->nlocals; i++)
        lemit(l, "LLIT r%u 0", localreg(l, i));

    /* the function body is an implicit block with the result on top */
    l->ctl[0].kind = 0;
    l->ctl[0].base = 0;
    l->ctl[0].has_result = (t->result != T_VOID);
    l->ctl[0].end_l = -1;
    l->ctl[0].else_l = -1;
    l->ctl[0].header_l = -1;
    l->ctl[0].else_seen = 0;
    l->ctl_n = 1;

    l->r.p = w->bytes + f->body_off;
    l->r.end = l->r.p + f->body_len;
    l->r.err = 0;

    while (!done && !l->err) {
        uint8_t op = rd_u8(&l->r);
        if (l->r.err) { lfail(l, "truncated code body"); break; }

        /* structural opcodes are tracked even in unreachable code */
        if (op == 0x02 || op == 0x03) { /* block / loop */
            int bt;
            Ctl *c;
            if (l->unreachable) { /* still must track nesting */
                rd_blocktype(l);
                if (l->ctl_n >= CTL_MAX) { lfail(l, "blocks nested too deep"); break; }
                c = &l->ctl[l->ctl_n++];
                memset(c, 0, sizeof *c);
                c->kind = (op == 0x03) ? 1 : 0;
                c->end_l = -2; /* marker: entered unreachable */
                continue;
            }
            bt = rd_blocktype(l);
            if (l->err) break;
            if (l->ctl_n >= CTL_MAX) { lfail(l, "blocks nested too deep"); break; }
            c = &l->ctl[l->ctl_n++];
            memset(c, 0, sizeof *c);
            c->kind = (op == 0x03) ? 1 : 0;
            c->base = l->height;
            c->has_result = (bt != T_VOID);
            c->end_l = new_label(l);
            if (c->kind == 1) {
                c->header_l = new_label(l);
                lemit(l, "LABEL L%d _", c->header_l);
            }
            continue;
        }
        if (op == 0x04) { /* if */
            int bt;
            Ctl *c;
            if (l->unreachable) {
                rd_blocktype(l);
                if (l->ctl_n >= CTL_MAX) { lfail(l, "blocks nested too deep"); break; }
                c = &l->ctl[l->ctl_n++];
                memset(c, 0, sizeof *c);
                c->kind = 2;
                c->end_l = -2;
                continue;
            }
            bt = rd_blocktype(l);
            if (l->err) break;
            if (l->height == 0) { lfail(l, "stack underflow at if"); break; }
            l->height--;
            if (l->ctl_n >= CTL_MAX) { lfail(l, "blocks nested too deep"); break; }
            c = &l->ctl[l->ctl_n++];
            memset(c, 0, sizeof *c);
            c->kind = 2;
            c->base = l->height;
            c->has_result = (bt != T_VOID);
            c->else_l = new_label(l);
            c->end_l = new_label(l);
            lemit(l, "JZ r%u L%d", slotreg(l, l->height), c->else_l);
            continue;
        }
        if (op == 0x05) { /* else */
            Ctl *c;
            if (l->ctl_n == 0 || l->ctl[l->ctl_n - 1].kind != 2) {
                lfail(l, "else without if");
                break;
            }
            c = &l->ctl[l->ctl_n - 1];
            c->else_seen = 1;
            if (c->end_l == -2) continue; /* unreachable */
            lemit(l, "JMP L%d _", c->end_l);
            lemit(l, "LABEL L%d _", c->else_l);
            l->height = c->base;
            l->unreachable = 0;
            continue;
        }
        if (op == 0x0B) { /* end */
            Ctl *c;
            if (l->ctl_n == 0) { lfail(l, "stray end"); break; }
            c = &l->ctl[--l->ctl_n];
            if (l->ctl_n == 0) { /* function body closed */
                if (t->result != T_VOID)
                    lemit(l, "RET r%u _", slotreg(l, 0));
                else
                    lemit(l, "RET _ _");
                done = 1;
                continue;
            }
            if (c->end_l != -2) {
                if (c->kind == 2 && !c->else_seen && c->has_result) {
                    lfail(l, "if with result needs an else arm");
                    break;
                }
                if (c->kind == 2 && !c->else_seen)
                    lemit(l, "LABEL L%d _", c->else_l); /* JZ target lands here */
                lemit(l, "LABEL L%d _", c->end_l);
                l->height = c->base + (c->has_result ? 1 : 0);
                l->unreachable = 0;
            }
            continue;
        }

        if (l->unreachable) {
            /* skip immediates without emitting */
            if (op == 0x0C || op == 0x0D) rd_u32(&l->r);
            else if (op == 0x0E) { uint32_t n = rd_u32(&l->r), k; for (k = 0; k <= n; k++) rd_u32(&l->r); }
            else if (op == 0x10 || (op >= 0x20 && op <= 0x24)) rd_u32(&l->r);
            else if (op >= 0x28 && op <= 0x3E) { rd_u32(&l->r); rd_u32(&l->r); }
            else if (op == 0x3F || op == 0x40) rd_u8(&l->r);
            else if (op == 0x41) rd_i64(&l->r, 32);
            else if (op == 0x42) rd_i64(&l->r, 64);
            else if (op == 0x43) { uint32_t b4; rd_bytes(&l->r, &b4, 4); }
            else if (op == 0x44) { uint64_t b8; rd_bytes(&l->r, &b8, 8); }
            continue;
        }

        switch (op) {
        case 0x00: lemit(l, "TRAP _ _"); l->unreachable = 1; break;
        case 0x01: break; /* nop */
        case 0x0C: { /* br */
            emit_branch(l, rd_u32(&l->r));
            l->unreachable = 1;
            break;
        }
        case 0x0D: { /* br_if */
            uint32_t d = rd_u32(&l->r);
            int skip;
            Ctl *c;
            if (d >= (uint32_t)l->ctl_n) { lfail(l, "bad branch depth"); break; }
            if (l->height == 0) { lfail(l, "stack underflow at br_if"); break; }
            skip = new_label(l);
            c = &l->ctl[l->ctl_n - 1 - d];
            lemit(l, "JZ r%u L%d", slotreg(l, l->height - 1), skip);
            l->height--; /* the condition is consumed */
            if (c->kind != 1 && c->has_result)
                lemit(l, "MOV r%u r%u", slotreg(l, c->base), slotreg(l, l->height - 1));
            lemit(l, "JMP L%d _", c->kind == 1 ? c->header_l : c->end_l);
            lemit(l, "LABEL L%d _", skip);
            break;
        }
        case 0x0E: { /* br_table: cascade of comparisons */
            uint32_t n = rd_u32(&l->r), k;
            uint32_t idxreg, t1, t2, base_h;
            uint32_t *targets = malloc((n + 1) * sizeof *targets);
            int *clabels = malloc((n ? n : 1) * sizeof *clabels);
            if (!targets || !clabels) { lfail(l, "out of memory"); free(targets); free(clabels); break; }
            for (k = 0; k <= n; k++) {
                targets[k] = rd_u32(&l->r);
                if (targets[k] >= (uint32_t)l->ctl_n) {
                    lfail(l, "bad branch depth in br_table");
                    break;
                }
            }
            if (l->err) { free(targets); free(clabels); break; }
            if (l->height == 0) { lfail(l, "stack underflow at br_table"); free(targets); free(clabels); break; }
            idxreg = slotreg(l, l->height - 1);
            base_h = l->height - 1;
            t1 = slotreg(l, l->height);
            t2 = slotreg(l, l->height + 1);
            for (k = 0; k < n; k++) {
                clabels[k] = new_label(l);
                lemit(l, "ILIT r%u %u", t1, k);
                lemit(l, "MOV r%u r%u", t2, idxreg);
                lemit(l, "IEQ r%u r%u", t2, t1);
                lemit(l, "JNZ r%u L%d", t2, clabels[k]);
            }
            /* default target inline */
            {
                Ctl *c = &l->ctl[l->ctl_n - 1 - targets[n]];
                l->height = base_h; /* index consumed */
                if (c->kind != 1 && c->has_result)
                    lemit(l, "MOV r%u r%u", slotreg(l, c->base), slotreg(l, base_h - 1));
                lemit(l, "JMP L%d _", c->kind == 1 ? c->header_l : c->end_l);
            }
            for (k = 0; k < n; k++) {
                Ctl *c = &l->ctl[l->ctl_n - 1 - targets[k]];
                lemit(l, "LABEL L%d _", clabels[k]);
                if (c->kind != 1 && c->has_result)
                    lemit(l, "MOV r%u r%u", slotreg(l, c->base), slotreg(l, base_h - 1));
                lemit(l, "JMP L%d _", c->kind == 1 ? c->header_l : c->end_l);
            }
            free(targets);
            free(clabels);
            l->unreachable = 1;
            break;
        }
        case 0x0F: /* return */
            if (t->result != T_VOID) {
                if (l->height == 0) { lfail(l, "stack underflow at return"); break; }
                lemit(l, "RET r%u _", slotreg(l, l->height - 1));
            } else {
                lemit(l, "RET _ _");
            }
            l->unreachable = 1;
            break;
        case 0x10: { /* call */
            uint32_t cf = rd_u32(&l->r);
            uint32_t tidx;
            if (cf >= w->nimports + (uint32_t)w->nfuncs) { lfail(l, "bad call target"); break; }
            tidx = (cf < w->nimports) ? w->imports[cf].type
                                      : w->funcs[cf - w->nimports].type;
            int np = type_nparams(l, tidx);
            int res = type_result(l, tidx);
            char nm[96];
            char argstr[128];
            size_t off = 0;
            int k;
            if (l->err) break;
            if (l->height < (uint32_t)np) { lfail(l, "stack underflow at call"); break; }
            for (k = 0; k < np; k++)
                off += snprintf(argstr + off, sizeof argstr - off, "%sr%u",
                                k ? "," : "", slotreg(l, l->height - np + k));
            lemit(l, "CALL %s %s", func_name(l, cf, nm, sizeof nm),
                  np ? argstr : "_");
            l->height -= (uint32_t)np;
            if (res != T_VOID) {
                lemit(l, "MOV r%u r0", slotreg(l, l->height));
                l->height++;
            }
            break;
        }
        case 0x11: { /* call_indirect */
            uint32_t tidx = rd_u32(&l->r);
            int np, res, k;
            char argstr[128];
            size_t off = 0;
            (void)rd_u32(&l->r);           /* table index: only table 0 exists */
            if (!w->has_table) { lfail(l, "call_indirect without a table"); break; }
            np = type_nparams(l, tidx);
            res = type_result(l, tidx);
            if (l->err) break;
            if (l->height < (uint32_t)(np + 1)) {
                lfail(l, "stack underflow at call_indirect");
                break;
            }
            /* stack: ... args(np) idx — idx is on top */
            for (k = 0; k < np; k++)
                off += snprintf(argstr + off, sizeof argstr - off, "%sr%u",
                                k ? "," : "", slotreg(l, l->height - 1 - np + k));
            lemit(l, "CALLI r%u %s", slotreg(l, l->height - 1),
                  np ? argstr : "_");
            l->height -= (uint32_t)(np + 1);
            if (res != T_VOID) {
                lemit(l, "MOV r%u r0", slotreg(l, l->height));
                l->height++;
            }
            break;
        }
        case 0x1A: /* drop */
            if (l->height == 0) { lfail(l, "stack underflow at drop"); break; }
            l->height--;
            break;
        case 0x1B: { /* select */
            int ld;
            if (l->height < 3) { lfail(l, "stack underflow at select"); break; }
            ld = new_label(l);
            lemit(l, "JNZ r%u L%d", slotreg(l, l->height - 1), ld);
            lemit(l, "MOV r%u r%u", slotreg(l, l->height - 3), slotreg(l, l->height - 2));
            lemit(l, "LABEL L%d _", ld);
            l->height -= 2;
            break;
        }
        case 0x20: /* local.get */
            lemit(l, "MOV r%u r%u", slotreg(l, l->height), localreg(l, rd_u32(&l->r)));
            l->height++;
            break;
        case 0x21: /* local.set */
            if (l->height == 0) { lfail(l, "stack underflow at local.set"); break; }
            lemit(l, "MOV r%u r%u", localreg(l, rd_u32(&l->r)), slotreg(l, l->height - 1));
            l->height--;
            break;
        case 0x22: /* local.tee */
            if (l->height == 0) { lfail(l, "stack underflow at local.tee"); break; }
            lemit(l, "MOV r%u r%u", localreg(l, rd_u32(&l->r)), slotreg(l, l->height - 1));
            break;
        case 0x23: /* global.get */
            lemit(l, "GGET r%u g%u", slotreg(l, l->height), rd_u32(&l->r));
            l->height++;
            break;
        case 0x24: /* global.set */
            if (l->height == 0) { lfail(l, "stack underflow at global.set"); break; }
            lemit(l, "GSET g%u r%u", rd_u32(&l->r), slotreg(l, l->height - 1));
            l->height--;
            break;
        case 0x3F: rd_u8(&l->r); lemit(l, "MSIZE r%u _", slotreg(l, l->height)); l->height++; break;
        case 0x40: /* memory.grow */
            rd_u8(&l->r);
            if (l->height == 0) { lfail(l, "stack underflow at memory.grow"); break; }
            lemit(l, "MGROW r%u r%u", slotreg(l, l->height - 1), slotreg(l, l->height - 1));
            break;
        case 0x41: lemit(l, "ILIT r%u %lld", slotreg(l, l->height), (long long)rd_i64(&l->r, 32)); l->height++; break;
        case 0x42: lemit(l, "LLIT r%u %lld", slotreg(l, l->height), (long long)rd_i64(&l->r, 64)); l->height++; break;
        case 0x43: {
            uint32_t b;
            float fv;
            char num[64];
            rd_bytes(&l->r, &b, 4);
            memcpy(&fv, &b, 4);
            snprintf(num, sizeof num, "%a", (double)fv);
            lemit(l, "FLIT r%u %s", slotreg(l, l->height), num);
            l->height++;
            break;
        }
        case 0x44: {
            uint64_t b;
            double dv;
            char num[64];
            rd_bytes(&l->r, &b, 8);
            memcpy(&dv, &b, 8);
            snprintf(num, sizeof num, "%a", dv);
            lemit(l, "DLIT r%u %s", slotreg(l, l->height), num);
            l->height++;
            break;
        }
        default: {
            const char *nm;
            if (op >= 0x28 && op <= 0x35 && (nm = load_name(op))) {
                uint32_t off;
                rd_u32(&l->r); /* align */
                off = rd_u32(&l->r);
                if (l->height == 0) { lfail(l, "stack underflow at load"); break; }
                lemit(l, "%s r%u r%u,%u", nm, slotreg(l, l->height - 1),
                      slotreg(l, l->height - 1), off);
            } else if (op >= 0x36 && op <= 0x3E && (nm = store_name(op))) {
                uint32_t off;
                rd_u32(&l->r);
                off = rd_u32(&l->r);
                if (l->height < 2) { lfail(l, "stack underflow at store"); break; }
                lemit(l, "%s r%u,%u r%u", nm, slotreg(l, l->height - 2), off,
                      slotreg(l, l->height - 1));
                l->height -= 2;
            } else if (op == 0x45) { /* i32.eqz */
                if (l->height == 0) { lfail(l, "stack underflow at eqz"); break; }
                lemit(l, "IEQ r%u 0", slotreg(l, l->height - 1));
            } else if (op == 0x50) { /* i64.eqz */
                if (l->height == 0) { lfail(l, "stack underflow at eqz"); break; }
                lemit(l, "LEQ r%u 0", slotreg(l, l->height - 1));
            } else if ((nm = cmpop_name(op))) {
                if (l->height < 2) { lfail(l, "stack underflow at comparison"); break; }
                lemit(l, "%s r%u r%u", nm, slotreg(l, l->height - 2), slotreg(l, l->height - 1));
                l->height--;
            } else if (op >= 0x67 && op <= 0x69) { /* i32 clz/ctz/popcnt */
                if (l->height == 0) { lfail(l, "stack underflow at bitop"); break; }
                lemit(l, "%s r%u _", op == 0x67 ? "ICLZ" : op == 0x68 ? "ICTZ" : "IPOP",
                      slotreg(l, l->height - 1));
            } else if (op >= 0x79 && op <= 0x7B) { /* i64 clz/ctz/popcnt */
                if (l->height == 0) { lfail(l, "stack underflow at bitop"); break; }
                lemit(l, "%s r%u _", op == 0x79 ? "LCLZ" : op == 0x7A ? "LCTZ" : "LPOP",
                      slotreg(l, l->height - 1));
            } else if ((op >= 0x74 && op <= 0x76) || (op >= 0x86 && op <= 0x88)) {
                /* wasm masks the shift count; Ribosome shifts trap outside
                   0..31/63 — lift with an explicit mask to keep parity */
                int i32 = op <= 0x76;
                uint32_t a, c, t;
                if (l->height < 2) { lfail(l, "stack underflow at shift"); break; }
                a = l->height - 2;
                c = l->height - 1;
                t = l->height; /* scratch above the stack top */
                lemit(l, "MOV r%u r%u", slotreg(l, t), slotreg(l, c));
                lemit(l, "%s r%u %d", i32 ? "IAND" : "LAND", slotreg(l, t), i32 ? 31 : 63);
                lemit(l, "%s r%u r%u",
                      op == 0x74 ? "ISHL" : op == 0x75 ? "ISAR" : op == 0x76 ? "ISHR" :
                      op == 0x86 ? "LSHL" : op == 0x87 ? "LSAR" : "LSHR",
                      slotreg(l, a), slotreg(l, t));
                l->height--;
            } else if ((nm = binop_name(op))) {
                if (l->height < 2) { lfail(l, "stack underflow at %s", nm); break; }
                lemit(l, "%s r%u r%u", nm, slotreg(l, l->height - 2), slotreg(l, l->height - 1));
                l->height--;
            } else if (op == 0x8B || op == 0x8C || op == 0x91) { /* f32 abs/neg/sqrt */
                if (l->height == 0) { lfail(l, "stack underflow"); break; }
                lemit(l, "%s r%u _", op == 0x8B ? "FABS" : op == 0x8C ? "FNEG" : "FSQRT",
                      slotreg(l, l->height - 1));
            } else if (op == 0x99 || op == 0x9A || op == 0x9F) { /* f64 */
                if (l->height == 0) { lfail(l, "stack underflow"); break; }
                lemit(l, "%s r%u _", op == 0x99 ? "DABS" : op == 0x9A ? "DNEG" : "DSQRT",
                      slotreg(l, l->height - 1));
            } else if ((nm = cvt_name(op))) {
                if (l->height == 0) { lfail(l, "stack underflow"); break; }
                lemit(l, "%s r%u _", nm, slotreg(l, l->height - 1));
            } else if (op == 0xBC || op == 0xBD || op == 0xBE || op == 0xBF) {
                /* reinterpret: identity on raw cells — no code at all */
            } else {
                lfail(l, "unsupported opcode 0x%02X (reserved for 1.0)", op);
            }
            break;
        }
        }
        if (l->height > l->max_height) l->max_height = l->height;
    }
    lemit(l, "END _ _");
}

/* In-memory lift: the caller owns *out_text (free()). NUL-terminated. */
int ribo_wasm_lift_mem(const RiboWasm *wasm, char **out_text, RiboError *error) {
    TBuf out = {0};
    Lift l;
    size_t i;

    memset(&l, 0, sizeof l);
    l.w = wasm;
    l.out = &out;

    /* module header: imports, globals, memory */
    for (i = 0; i < wasm->nimports; i++) {
        const WType *t;
        if (wasm->imports[i].type >= wasm->ntypes) {
            free(out.data);
            return wfail(error, "lift: bad import type index");
        }
        t = &wasm->types[wasm->imports[i].type];
        lemit(&l, "IMPORT %s %s/%u", wasm->imports[i].name,
              t_name(t->result), (unsigned)t->nparams);
    }
    for (i = 0; i < wasm->nglobals; i++) {
        const WGlobal *g = &wasm->globals[i];
        lemit(&l, "GLOB g%u %s", (unsigned)i, t_name(g->type));
        if (g->bits) {
            if (g->type == T_I32) lemit(&l, "GINIT g%u %d", (unsigned)i, (int32_t)(uint32_t)g->bits);
            else if (g->type == T_I64) lemit(&l, "GINIT g%u %lld", (unsigned)i, (long long)(int64_t)g->bits);
            else if (g->type == T_F32) { float fv; uint32_t b = (uint32_t)g->bits; char num[64]; memcpy(&fv, &b, 4); snprintf(num, sizeof num, "%a", (double)fv); lemit(&l, "GINIT g%u %s", (unsigned)i, num); }
            else { double dv; char num[64]; memcpy(&dv, &g->bits, 8); snprintf(num, sizeof num, "%a", dv); lemit(&l, "GINIT g%u %s", (unsigned)i, num); }
        }
    }
    if (wasm->has_mem) {
        if (wasm->mem_max) lemit(&l, "MEM mem %u,%u", wasm->mem_min, wasm->mem_max);
        else lemit(&l, "MEM mem %u", wasm->mem_min);
    }
    if (wasm->has_table) {
        if (wasm->table_max) lemit(&l, "TABLE t %u,%u", wasm->table_min, wasm->table_max);
        else lemit(&l, "TABLE t %u", wasm->table_min);
    }
    /* element segments: merge contiguous runs into TELEM lines */
    for (i = 0; i < wasm->nelems;) {
        size_t len = 1, k;
        char names[512];
        size_t noff = 0;
        while (i + len < wasm->nelems &&
               wasm->elems[i + len].off == wasm->elems[i + len - 1].off + 1 &&
               len < 32) /* one TELEM line holds at most ~16 names */
            len++;
        names[0] = '\0';
        for (k = 0; k < len; k++) {
            uint32_t fn = wasm->elems[i + k].func;
            char nm[96];
            noff += (size_t)snprintf(names + noff, sizeof names - noff,
                                     "%s%s", k ? "," : "",
                                     func_name(&l, fn, nm, sizeof nm));
        }
        lemit(&l, "TELEM %u %s", wasm->elems[i].off, names);
        i += len;
    }
    /* data segments: DATA off hex (split so lines stay well under 511 bytes) */
    for (i = 0; i < wasm->ndatas; i++) {
        uint32_t off = wasm->datas[i].off;
        uint32_t left = wasm->datas[i].len;
        const uint8_t *src = wasm->bytes + wasm->datas[i].data_off;
        while (left) {
            uint32_t chunk = left > 200 ? 200 : left, k;
            char hex[401];
            for (k = 0; k < chunk; k++)
                snprintf(hex + 2 * k, 3, "%02x", (unsigned)src[k]);
            hex[2 * chunk] = '\0';
            lemit(&l, "DATA %u %s", off, hex);
            off += chunk;
            src += chunk;
            left -= chunk;
        }
    }
    for (i = 0; i < wasm->nexports; i++)
        lemit(&l, "EXPORT %s _", wasm->exports[i].name);
    for (i = 0; i < wasm->nfuncs; i++) {
        if (wasm->funcs[i].type >= wasm->ntypes) {
            free(out.data);
            return wfail(error, "lift: bad func type index");
        }
        lift_func(&l, wasm->nimports + (uint32_t)i);
        if (l.err) {
            free(out.data);
            return wfail(error, "lift: func f%u: %s", (unsigned)i, l.errmsg);
        }
    }
    if (l.err) {
        free(out.data);
        return wfail(error, "lift: %s", l.errmsg);
    }
    if (!tb_putn(&out, "\0", 1)) {
        free(out.data);
        return wfail(error, "lift: out of memory");
    }
    *out_text = out.data;
    return 1;
}

int ribo_wasm_lift(const RiboWasm *wasm, const char *out_path, RiboError *error) {
    char *text = NULL;
    FILE *fp;
    int ok;
    if (!ribo_wasm_lift_mem(wasm, &text, error)) return 0;
    fp = fopen(out_path, "wb");
    if (!fp) {
        free(text);
        return wfail(error, "lift: cannot write file: %s", out_path);
    }
    ok = fwrite(text, 1, strlen(text), fp) == strlen(text);
    fclose(fp);
    free(text);
    if (!ok) return wfail(error, "lift: failed writing file: %s", out_path);
    return 1;
}
