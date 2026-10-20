/* api.c — the browser-facing exports. All exchange happens through static
   buffers; JS writes UTF-8 source into ribo_in(), reads results from
   ribo_out(). Text in/out is always NUL-terminated JSON or .rna. */
#include "ribo.h"
#include "ribo_internal.h" /* the shell is part of the build: it may peek */
#include <string.h>

/* return type of a func by name: T_* or -1 */
static int func_ret_type(const RiboModule *m, const char *name) {
    size_t i;
    for (i = 0; i < m->func_count; i++)
        if (!strcmp(m->funcs[i].name, name)) return m->funcs[i].ret;
    return -1;
}

#define IN_CAP   (4u * 1024u * 1024u)
#define ARGS_CAP 4096u
#define OUT_CAP  (2u * 1024u * 1024u)
#define WASM_CAP (8u * 1024u * 1024u)

static uint8_t in_buf[IN_CAP];
static uint8_t args_buf[ARGS_CAP];
static uint8_t out_buf[OUT_CAP];
static uint8_t wasm_buf[WASM_CAP];

__attribute__((visibility("default"))) uint8_t *ribo_in(void) { return in_buf; }
__attribute__((visibility("default"))) uint8_t *ribo_args(void) { return args_buf; }
__attribute__((visibility("default"))) uint8_t *ribo_out(void) { return out_buf; }
__attribute__((visibility("default"))) uint8_t *ribo_wasmout(void) { return wasm_buf; }
__attribute__((visibility("default"))) uint32_t ribo_in_cap(void) { return IN_CAP; }

static void out_json_error(const char *stage, const char *msg) {
    /* messages are internal contract texts: no quotes or control chars */
    snprintf((char *)out_buf, OUT_CAP,
             "{\"ok\":false,\"error\":{\"stage\":\"%s\",\"message\":\"%s\"}}",
             stage, msg);
}

/* ---- check: in = .rna source (NUL-terminated) ---- */
__attribute__((visibility("default"))) int ribo_do_check(void) {
    RiboError err;
    RiboModule *m = ribo_compile_text((char *)in_buf, 0, &err);
    if (!m) {
        out_json_error("compile", err.message);
        return 0;
    }
    snprintf((char *)out_buf, OUT_CAP, "{\"ok\":true}");
    ribo_module_release(m);
    return 1;
}

/* parse "func arg arg ..." from args_buf; args: float iff the token
   contains '.', 'e', 'inf' or 'nan' (same rule as the CLI) */
static int parse_run_line(char *fname, size_t fcap, RiboValue *args, size_t *nargs) {
    char *p = (char *)args_buf;
    size_t n = 0;
    while (*p == ' ') p++;
    {
        char *s = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        if (!*s) return 0;
        snprintf(fname, fcap, "%s", s);
    }
    while (*p) {
        char *tok, *end;
        while (*p == ' ') p++;
        if (!*p) break;
        tok = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        if (n >= RIBO_MAX_ARGS) return 0;
        if (strpbrk(tok, ".eE") || strstr(tok, "inf") || strstr(tok, "nan")) {
            double d = strtod(tok, &end);
            uint64_t b;
            if (*end) return 0;
            memcpy(&b, &d, 8);
            args[n++] = b;
        } else {
            /* signed decimal */
            int neg = 0;
            uint64_t v = 0;
            const char *q = tok;
            if (*q == '-') { neg = 1; q++; }
            if (!*q) return 0;
            while (*q) {
                if (*q < '0' || *q > '9') return 0;
                v = v * 10 + (uint64_t)(*q - '0');
                q++;
            }
            args[n++] = neg ? (uint64_t)(0 - v) : v;
        }
    }
    *nargs = n;
    return 1;
}

/* ---- run: in = source, args_buf = "func args..." ---- */
__attribute__((visibility("default"))) int ribo_do_run(void) {
    RiboError err;
    RiboModule *m;
    char fname[64];
    RiboValue args[RIBO_MAX_ARGS], result = 0;
    size_t nargs = 0;
    int rettype = -1;
    size_t i;

    if (!parse_run_line(fname, sizeof fname, args, &nargs)) {
        out_json_error("run", "bad invocation line (want: func arg arg ...)");
        return 0;
    }
    m = ribo_compile_text((char *)in_buf, 0, &err);
    if (!m) { out_json_error("compile", err.message); return 0; }
    if (!ribo_interpret(m, fname, args, nargs, NULL, NULL, &result, &err)) {
        out_json_error("run", err.message);
        ribo_module_release(m);
        return 0;
    }
    rettype = func_ret_type(m, fname);
    switch (rettype) {
    case T_I32:
        snprintf((char *)out_buf, OUT_CAP,
                 "{\"ok\":true,\"type\":\"i32\",\"value\":\"%d\"}",
                 (int)(int32_t)(uint32_t)result);
        break;
    case T_I64:
        snprintf((char *)out_buf, OUT_CAP,
                 "{\"ok\":true,\"type\":\"i64\",\"value\":\"%lld\"}",
                 (long long)(int64_t)result);
        break;
    case T_F32: {
        uint32_t b = (uint32_t)result;
        float f;
        memcpy(&f, &b, 4);
        snprintf((char *)out_buf, OUT_CAP,
                 "{\"ok\":true,\"type\":\"f32\",\"value\":\"%a\",\"bits\":\"%08x\"}",
                 (double)f, (unsigned)b);
        break;
    }
    case T_F64: {
        double d;
        memcpy(&d, &result, 8);
        snprintf((char *)out_buf, OUT_CAP,
                 "{\"ok\":true,\"type\":\"f64\",\"value\":\"%a\",\"bits\":\"%016llx\"}",
                 d, (unsigned long long)result);
        break;
    }
    default:
        snprintf((char *)out_buf, OUT_CAP, "{\"ok\":true,\"type\":\"void\"}");
        break;
    }
    (void)i;
    ribo_module_release(m);
    return 1;
}

/* ---- lower: in = source -> wasm bytes in wasm_buf, returns length ---- */
__attribute__((visibility("default"))) int ribo_do_lower(void) {
    RiboError err;
    RiboModule *m = ribo_compile_text((char *)in_buf, 0, &err);
    uint8_t *bytes = NULL;
    size_t len = 0;
    if (!m) { out_json_error("compile", err.message); return 0; }
    if (!ribo_emit_wasm_mem(m, &bytes, &len, &err)) {
        out_json_error("emit", err.message);
        ribo_module_release(m);
        return 0;
    }
    ribo_module_release(m);
    if (len > WASM_CAP) {
        free(bytes);
        out_json_error("emit", "wasm output exceeds the shell buffer");
        return 0;
    }
    memcpy(wasm_buf, bytes, len);
    free(bytes);
    return (int)len;
}

/* ---- lift: in = wasm bytes (length given) -> .rna text in out ---- */
__attribute__((visibility("default"))) int ribo_do_lift(uint32_t len) {
    RiboError err;
    RiboWasm *w;
    char *text = NULL;
    size_t tl;
    if (len > IN_CAP) { out_json_error("lift", "input too large"); return 0; }
    w = ribo_wasm_parse(in_buf, len, &err);
    if (!w) { out_json_error("lift", err.message); return 0; }
    if (!ribo_wasm_lift_mem(w, &text, &err)) {
        ribo_wasm_release(w);
        out_json_error("lift", err.message);
        return 0;
    }
    ribo_wasm_release(w);
    tl = strlen(text);
    if (tl + 1 > OUT_CAP) {
        free(text);
        out_json_error("lift", "lifted source exceeds the shell buffer");
        return 0;
    }
    memcpy(out_buf, text, tl + 1);
    free(text);
    return 1;
}
