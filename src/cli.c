/* cli.c — ribo command line. See LLM.md "Agent workflow".
     ribo check prog.rna [--json] [--repair]
     ribo run   prog.rna <func> [args...] [--json]
     ribo test  prog.rna cases.csv
   lower/verify/lift/roundtrip arrive with their milestones. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ribo_internal.h"

static int usage(void) {
    fputs("ribo " RIBO_VERSION " - WebAssembly, lifted for machines\n"
          "usage: ribo <command> ...\n"
          "  check prog.rna [--json] [--repair]\n"
          "  run   prog.rna <func> [args...]\n"
          "  test  prog.rna cases.csv\n"
          "  lower prog.rna out.wasm      (milestone 0.3)\n"
          "  verify prog.rna <func> ...   (milestone 0.3)\n"
          "  lift  mod.wasm out.rna       wasm -> .rna\n"
          "  roundtrip mod.wasm f a..     lift + lower + differential vs original\n",
          stderr);
    return 2;
}

static void json_escape(const char *s, FILE *out) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(out, "\\%c", c);
        else if (c < 32) fprintf(out, "\\u%04x", c);
        else fputc(c, out);
    }
}

static void print_diag_json(const RiboDiag *d) {
    fputs("{\"ok\":false,\"error\":{\"stage\":\"", stdout);
    json_escape(d->stage, stdout);
    fputs("\",\"message\":\"", stdout);
    json_escape(d->message, stdout);
    fprintf(stdout, "\",\"line\":%u,\"line_text\":\"", (unsigned)d->line);
    json_escape(d->line_text, stdout);
    fputs("\"}}\n", stdout);
}

/* parse a CLI/CSV argument according to the parameter type */
static int parse_value(const char *s, int type, RiboValue *out) {
    char *end;
    errno = 0;
    switch (type) {
    case T_I32: {
        long long v = strtoll(s, &end, 10);
        if (*end) return 0;
        *out = (uint64_t)(int64_t)(int32_t)v; /* wrap like the CLI of Codons */
        return 1;
    }
    case T_I64: {
        long long v = strtoll(s, &end, 10);
        if (*end) {
            unsigned long long u;
            errno = 0;
            u = strtoull(s, &end, 10);
            if (errno || *end) return 0;
            *out = (uint64_t)u;
            return 1;
        }
        if (errno == ERANGE) {
            unsigned long long u;
            errno = 0;
            u = strtoull(s, &end, 10);
            if (errno || *end) return 0;
            *out = (uint64_t)u;
            return 1;
        }
        *out = (uint64_t)v;
        return 1;
    }
    case T_F32: {
        float v = strtof(s, &end);
        uint32_t b;
        if (*end) return 0;
        memcpy(&b, &v, 4);
        *out = b;
        return 1;
    }
    case T_F64: {
        double v = strtod(s, &end);
        uint64_t b;
        if (*end) return 0;
        memcpy(&b, &v, 8);
        *out = b;
        return 1;
    }
    default:
        return 0;
    }
}

static void print_value(RiboValue v, int type) {
    switch (type) {
    case T_I32: printf("%d\n", (int32_t)(uint32_t)v); break;
    case T_I64: printf("%lld\n", (long long)(int64_t)v); break;
    case T_F32: { float f; uint32_t b = (uint32_t)v; memcpy(&f, &b, 4);
                  printf("%.9g\n", (double)f); break; }
    case T_F64: { double d; memcpy(&d, &v, 8);
                  printf("%.17g\n", d); break; }
    default: puts("void"); break;
    }
}

/* load+parse+check with the internal (structured) diagnostics */
static RiboModule *load_checked(const char *path, int repair, RiboDiag *diag) {
    FILE *fp = fopen(path, "rb");
    long size;
    char *text;
    RiboModule *m = NULL;
    if (!fp) {
        ribo_diag_set(diag, "parse", 0, "", "cannot open file: %s", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    text = malloc((size_t)size + 1);
    if (!text || (size > 0 && fread(text, 1, (size_t)size, fp) != (size_t)size)) {
        free(text);
        fclose(fp);
        ribo_diag_set(diag, "parse", 0, "", "cannot read file: %s", path);
        return NULL;
    }
    fclose(fp);
    text[size] = '\0';
    if (!ribo_parse_source(text, repair, &m, diag)) { free(text); return NULL; }
    free(text);
    if (!ribo_check_module(m, diag)) { ribo_module_release(m); return NULL; }
    return m;
}

static int cmd_check(int argc, char **argv) {
    const char *path = NULL;
    int json = 0, repair = 0, i;
    RiboDiag diag;
    RiboModule *m;
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--repair")) repair = 1;
        else path = argv[i];
    }
    if (!path) return usage();
    memset(&diag, 0, sizeof diag);
    m = load_checked(path, repair, &diag);
    if (!m) {
        if (json) print_diag_json(&diag);
        else {
            fprintf(stderr, "%s: %s", diag.stage, diag.message);
            if (diag.line) fprintf(stderr, " (line %u: %s)",
                                   (unsigned)diag.line, diag.line_text);
            fputc('\n', stderr);
        }
        return 1;
    }
    if (json)
        printf("{\"ok\":true,\"funcs\":%u,\"imports\":%u,\"globals\":%u}\n",
               (unsigned)m->func_count, (unsigned)m->import_count,
               (unsigned)m->glob_count);
    else
        printf("ok: %u funcs, %u imports, %u globals%s\n",
               (unsigned)m->func_count, (unsigned)m->import_count,
               (unsigned)m->glob_count, m->has_mem ? ", mem" : "");
    ribo_module_release(m);
    return 0;
}

static int cmd_run(int argc, char **argv) {
    const char *path, *fname;
    RiboDiag diag;
    RiboModule *m;
    RiboFunc *f;
    RiboValue args[RIBO_MAX_ARGS];
    size_t i;
    RiboError err;
    RiboValue result;
    if (argc < 4) return usage();
    path = argv[2];
    fname = argv[3];
    memset(&diag, 0, sizeof diag);
    m = load_checked(path, 0, &diag);
    if (!m) { ribo_diag_to_public(&diag, &err); fprintf(stderr, "%s\n", err.message); return 1; }
    f = ribo_find_func(m, fname);
    if (!f) { fprintf(stderr, "run: undefined func: %s\n", fname); ribo_module_release(m); return 1; }
    if (argc - 4 != (int)f->arity) {
        fprintf(stderr, "run: func %s expects %u arguments, got %u\n",
                fname, (unsigned)f->arity, (unsigned)(argc - 4));
        ribo_module_release(m);
        return 1;
    }
    for (i = 0; i < f->arity; i++) {
        /* registers are raw cells; integer args arrive as i64 and I-ops read
           the low 32 bits. An arg containing '.', 'e' (or inf/nan) is taken
           as f64 bits — param types live in registers, not in the signature */
        const char *s = argv[4 + i];
        int ptype = (strpbrk(s, ".eE") || strstr(s, "inf") || strstr(s, "nan"))
                        ? T_F64 : T_I64;
        if (!parse_value(s, ptype, &args[i])) {
            fprintf(stderr, "run: bad argument: %s\n", s);
            ribo_module_release(m);
            return 1;
        }
    }
    if (!ribo_interpret(m, fname, args, f->arity, NULL, NULL, &result, &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_module_release(m);
        return 1;
    }
    print_value(result, f->ret);
    ribo_module_release(m);
    return 0;
}

static int cmd_test(int argc, char **argv) {
    const char *path, *csv_path;
    RiboDiag diag;
    RiboModule *m;
    RiboFunc *mainf;
    FILE *fp;
    char line[1024];
    unsigned passed = 0, total = 0, lineno = 1;
    char func_name[64] = "main";
    if (argc < 4) return usage();
    path = argv[2];
    csv_path = argv[3];
    memset(&diag, 0, sizeof diag);
    m = load_checked(path, 0, &diag);
    if (!m) { fprintf(stderr, "%s: %s\n", diag.stage, diag.message); return 1; }
    /* the tested func: the single exported one, else "main", else the first */
    {
        size_t i;
        mainf = NULL;
        for (i = 0; i < m->func_count; i++)
            if (m->funcs[i].exported) { mainf = &m->funcs[i]; break; }
        if (!mainf) mainf = ribo_find_func(m, func_name);
        if (!mainf && m->func_count) mainf = &m->funcs[0];
    }
    if (!mainf) { fprintf(stderr, "test: no funcs in %s\n", path); ribo_module_release(m); return 1; }
    fp = fopen(csv_path, "r");
    if (!fp) { fprintf(stderr, "test: cannot open file: %s\n", csv_path); ribo_module_release(m); return 1; }
    if (!fgets(line, sizeof line, fp)) { fprintf(stderr, "test: empty csv\n"); fclose(fp); ribo_module_release(m); return 1; }
    while (fgets(line, sizeof line, fp)) {
        char *cols[RIBO_MAX_ARGS + 1];
        unsigned ncols = 0;
        char *tok, *save = NULL;
        RiboValue args[RIBO_MAX_ARGS], result;
        RiboValue expected;
        RiboError err;
        unsigned ok = 1, i;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!line[0]) continue;
        lineno++;
        for (tok = strtok_s(line, ",", &save); tok && ncols <= RIBO_MAX_ARGS;
             tok = strtok_s(NULL, ",", &save))
            cols[ncols++] = tok;
        if (ncols != (unsigned)mainf->arity + 1) {
            printf("line %u: expected %u columns, got %u — skipped\n",
                   lineno, (unsigned)mainf->arity + 1, ncols);
            continue;
        }
        total++;
        for (i = 0; i < mainf->arity; i++) {
            int ptype = (strpbrk(cols[i], ".eE") || strstr(cols[i], "inf") ||
                         strstr(cols[i], "nan")) ? T_F64 : T_I64;
            if (!parse_value(cols[i], ptype, &args[i])) {
                printf("line %u: bad argument '%s'\n", lineno, cols[i]);
                ok = 0;
            }
        }
        if (!ok) continue;
        if (!parse_value(cols[mainf->arity], mainf->ret == T_VOID ? T_I64 : mainf->ret,
                         &expected)) {
            printf("line %u: bad expected value '%s'\n", lineno, cols[mainf->arity]);
            continue;
        }
        if (!ribo_interpret(m, mainf->name, args, mainf->arity, NULL, NULL,
                            &result, &err)) {
            printf("line %u: FAULT: %s\n", lineno, err.message);
            continue;
        }
        /* compare by result type */
        switch (mainf->ret) {
        case T_I32:
            ok = (int32_t)(uint32_t)result == (int32_t)(uint32_t)expected;
            break;
        case T_F32: {
            float a, b;
            uint32_t ab = (uint32_t)result, bb = (uint32_t)expected;
            memcpy(&a, &ab, 4); memcpy(&b, &bb, 4);
            ok = (a == b);
            break;
        }
        case T_F64: {
            double a, b;
            memcpy(&a, &result, 8); memcpy(&b, &expected, 8);
            ok = (a == b);
            break;
        }
        default:
            ok = result == expected;
            break;
        }
        if (ok) passed++;
        else {
            printf("line %u: mismatch: got ", lineno);
            print_value(result, mainf->ret);
            printf("         expected ");
            print_value(expected, mainf->ret);
        }
    }
    fclose(fp);
    printf("%u/%u passed\n", passed, total);
    ribo_module_release(m);
    return passed == total ? 0 : 1;
}

static const char *ret_name(int t) {
    switch (t) {
    case T_I32: return "i32";
    case T_I64: return "i64";
    case T_F32: return "f32";
    case T_F64: return "f64";
    default: return "void";
    }
}

/* canonical result string, must match tests/wasm/run.js exactly */
static void canon_result(RiboValue v, int type, char *out, size_t cap) {
    switch (type) {
    case T_I32: snprintf(out, cap, "%d", (int32_t)(uint32_t)v); break;
    case T_I64: snprintf(out, cap, "%lld", (long long)(int64_t)v); break;
    case T_F32: snprintf(out, cap, "%08x", (uint32_t)v); break;
    case T_F64: snprintf(out, cap, "%016llx", (unsigned long long)v); break;
    default: snprintf(out, cap, "void"); break;
    }
}

/* map our fault text to the harness's trap classes */
static const char *fault_class(const char *msg) {
    if (strstr(msg, "division by zero")) return "div0";
    if (strstr(msg, "division overflow")) return "divovf";
    if (strstr(msg, "out of bounds")) return "oob"; /* memory AND table */
    if (strstr(msg, "invalid truncation")) return "trunc";
    if (strstr(msg, "call depth")) return "stack";
    if (strstr(msg, "execution budget")) return "budget";
    if (strstr(msg, "unreachable")) return "trap";
    return "trap";
}

static int cmd_lower(int argc, char **argv) {
    RiboDiag diag;
    RiboModule *m;
    RiboError err;
    if (argc < 4) return usage();
    memset(&diag, 0, sizeof diag);
    m = load_checked(argv[2], 0, &diag);
    if (!m) { ribo_diag_to_public(&diag, &err); fprintf(stderr, "%s\n", err.message); return 1; }
    if (!ribo_emit_wasm(m, argv[3], &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_module_release(m);
        return 1;
    }
    printf("ok: %s\n", argv[3]);
    ribo_module_release(m);
    return 0;
}

static int node_run(const char *wasm, const char *func, const char *rettype,
                    const char **argstrs, size_t nargs, char *out, size_t cap);

/* canon strings for f32/f64 are hex bit patterns; the wasm spec makes NaN
   payloads/sign nondeterministic, so any two NaNs compare equal */
static int canon_nan_equal(const char *a, const char *b, int rettype) {
    unsigned long long ba, bb, exp_mask, man_mask;
    char *ea, *eb;
    if (rettype != T_F32 && rettype != T_F64) return 0;
    if (strncmp(a, "ok ", 3) || strncmp(b, "ok ", 3)) return 0;
    ba = strtoull(a + 3, &ea, 16);
    bb = strtoull(b + 3, &eb, 16);
    if (*ea || *eb) return 0;
    if (rettype == T_F32) { exp_mask = 0x7F800000ull; man_mask = 0x007FFFFFull; }
    else { exp_mask = 0x7FF0000000000000ull; man_mask = 0x000FFFFFFFFFFFFFull; }
    return (ba & exp_mask) == exp_mask && (ba & man_mask) &&
           (bb & exp_mask) == exp_mask && (bb & man_mask);
}

static int cmd_verify(int argc, char **argv) {
    RiboDiag diag;
    RiboModule *m;
    RiboFunc *f;
    RiboError err;
    RiboValue args[RIBO_MAX_ARGS], result;
    size_t i;
    char ours[128], node_out[256];
    const char *wasm_path = "work\\verify_tmp.wasm";

    if (argc < 4) return usage();
    memset(&diag, 0, sizeof diag);
    m = load_checked(argv[2], 0, &diag);
    if (!m) { ribo_diag_to_public(&diag, &err); fprintf(stderr, "%s\n", err.message); return 1; }
    f = ribo_find_func(m, argv[3]);
    if (!f) { fprintf(stderr, "verify: undefined func: %s\n", argv[3]); ribo_module_release(m); return 1; }
    if (argc - 4 != (int)f->arity) {
        fprintf(stderr, "verify: func %s expects %u arguments, got %u\n",
                argv[3], (unsigned)f->arity, (unsigned)(argc - 4));
        ribo_module_release(m);
        return 1;
    }
    for (i = 0; i < f->arity; i++) {
        const char *s = argv[4 + i];
        int ptype = (strpbrk(s, ".eE") || strstr(s, "inf") || strstr(s, "nan"))
                        ? T_F64 : T_I64;
        if (!parse_value(s, ptype, &args[i])) {
            fprintf(stderr, "verify: bad argument: %s\n", s);
            ribo_module_release(m);
            return 1;
        }
    }

    /* 1. reference interpreter */
    if (!ribo_interpret(m, f->name, args, f->arity, NULL, NULL, &result, &err)) {
        snprintf(ours, sizeof ours, "trap %s", fault_class(err.message));
    } else {
        char canon[64];
        canon_result(result, f->ret, canon, sizeof canon);
        snprintf(ours, sizeof ours, "ok %s", canon);
    }
    if (!strncmp(ours, "trap budget", 11)) {
        printf("{\"ok\":true,\"verdict\":\"skip\",\"reason\":\"budget exhaustion "
               "has no wasm counterpart (the engine loops)\"}\n");
        ribo_module_release(m);
        return 0;
    }

    /* 2. emit + run under V8; the verified func is exported ad hoc so the
       harness can reach it even without an EXPORT decl */
    f->exported = 1;
    if (!ribo_emit_wasm(m, wasm_path, &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_module_release(m);
        return 1;
    }
    {
        const char *cellstr[RIBO_MAX_ARGS];
        char cellbuf[RIBO_MAX_ARGS][32];
        for (i = 0; i < f->arity; i++) {
            snprintf(cellbuf[i], sizeof cellbuf[i], "%llu",
                     (unsigned long long)args[i]);
            cellstr[i] = cellbuf[i];
        }
        if (!node_run(wasm_path, f->name, ret_name(f->ret), cellstr,
                      f->arity, node_out, sizeof node_out)) {
            fprintf(stderr, "verify: node harness failed (is node on PATH, "
                            "are we at the repo root?)\n");
            ribo_module_release(m);
            return 1;
        }
    }

    /* 3. compare */
    if (!strcmp(ours, node_out) || canon_nan_equal(ours, node_out, f->ret)) {
        printf("{\"ok\":true,\"verdict\":\"match\",\"result\":\"%s\"}\n", ours);
        ribo_module_release(m);
        return 0;
    }
    printf("{\"ok\":false,\"verdict\":\"MISMATCH\",\"interpreter\":\"%s\","
           "\"v8\":\"%s\"}\n", ours, node_out);
    ribo_module_release(m);
    return 1;
}

static uint8_t *read_file(const char *path, size_t *len_out) {
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0 || n > 64L * 1024 * 1024) { fclose(fp); return NULL; }
    buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    *len_out = (size_t)n;
    return buf;
}

static int cmd_lift(int argc, char **argv) {
    size_t len;
    uint8_t *bytes;
    RiboWasm *w;
    RiboError err;
    if (argc < 4) return usage();
    bytes = read_file(argv[2], &len);
    if (!bytes) { fprintf(stderr, "lift: cannot read file: %s\n", argv[2]); return 1; }
    w = ribo_wasm_parse(bytes, len, &err);
    free(bytes);
    if (!w) { fprintf(stderr, "%s\n", err.message); return 1; }
    if (!ribo_wasm_lift(w, argv[3], &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_wasm_release(w);
        return 1;
    }
    printf("ok: %s\n", argv[3]);
    ribo_wasm_release(w);
    return 0;
}

/* run one wasm module under V8, return its canonical line.
   argstrs are passed through verbatim: run.js maps integers to BigInt and
   decimal-looking values to Number (see the harness). */
static int node_run(const char *wasm, const char *func, const char *rettype,
                    const char **argstrs, size_t nargs, char *out, size_t cap) {
    /* pipe directly, no scratch file: a shared verify_node.out under rapid
       churn gets locked by AV real-time scans and fails the whole storm */
    char cmd[4096];
    size_t off = 0, i;
    FILE *fp;
    int rc;
    off += snprintf(cmd + off, sizeof cmd - off, "node tests\\wasm\\run.js %s %s %s",
                    wasm, func, rettype);
    for (i = 0; i < nargs; i++)
        off += snprintf(cmd + off, sizeof cmd - off, " %s", argstrs[i]);
    snprintf(cmd + off, sizeof cmd - off, " 2>&1");
    fp = _popen(cmd, "r");
    if (!fp) return 0;
    if (!fgets(out, (int)cap, fp)) out[0] = '\0';
    rc = _pclose(fp);
    if (rc != 0 && !out[0]) return 0; /* no output AND failed: real spawn error */
    { char *nl = strpbrk(out, "\r\n"); if (nl) *nl = '\0'; }
    return 1;
}

static int cmd_roundtrip(int argc, char **argv) {
    size_t len, i;
    uint8_t *bytes;
    RiboWasm *w;
    RiboError err;
    RiboDiag diag;
    RiboModule *m;
    RiboFunc *f;
    RiboValue args[RIBO_MAX_ARGS];
    char orig[256], lifted[256];
    const char *cellstr[RIBO_MAX_ARGS];
    char cellbuf[RIBO_MAX_ARGS][32];
    const char **rawstr;
    int ret;
    size_t arity;
    const char *rna_path = "work\\roundtrip.rna";
    const char *wasm2_path = "work\\roundtrip.wasm";
    uint8_t ptypes[RIBO_MAX_ARGS];
    int ptype_n;

    if (argc < 4) return usage();
    bytes = read_file(argv[2], &len);
    if (!bytes) { fprintf(stderr, "roundtrip: cannot read file: %s\n", argv[2]); return 1; }
    w = ribo_wasm_parse(bytes, len, &err);
    if (!w) { free(bytes); fprintf(stderr, "%s\n", err.message); return 1; }
    ptype_n = ribo_wasm_param_types(w, argv[3], ptypes, RIBO_MAX_ARGS);
    if (!ribo_wasm_lift(w, rna_path, &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_wasm_release(w);
        free(bytes);
        return 1;
    }
    ribo_wasm_release(w);

    memset(&diag, 0, sizeof diag);
    m = load_checked(rna_path, 0, &diag);
    if (!m) {
        ribo_diag_to_public(&diag, &err);
        fprintf(stderr, "roundtrip: lifted .rna does not compile: %s\n", err.message);
        free(bytes);
        return 1;
    }
    f = ribo_find_func(m, argv[3]);
    if (!f) {
        fprintf(stderr, "roundtrip: no func %s in lifted module\n", argv[3]);
        ribo_module_release(m);
        free(bytes);
        return 1;
    }
    if (argc - 4 != (int)f->arity) {
        fprintf(stderr, "roundtrip: func %s expects %u arguments, got %u\n",
                argv[3], (unsigned)f->arity, (unsigned)(argc - 4));
        ribo_module_release(m);
        free(bytes);
        return 1;
    }
    ret = f->ret;
    arity = f->arity;
    for (i = 0; i < arity; i++) {
        const char *s = argv[4 + i];
        /* float args are encoded by the ORIGINAL module's param type:
           lifted f32 cells hold f32 bits, f64 cells f64 bits */
        int ptype = ((int)i < ptype_n) ? ptypes[i]
                    : (strpbrk(s, ".eE") || strstr(s, "inf") || strstr(s, "nan"))
                        ? T_F64 : T_I64;
        if (!parse_value(s, ptype, &args[i])) {
            fprintf(stderr, "roundtrip: bad argument: %s\n", s);
            ribo_module_release(m);
            free(bytes);
            return 1;
        }
        snprintf(cellbuf[i], sizeof cellbuf[i], "%llu", (unsigned long long)args[i]);
        cellstr[i] = cellbuf[i];
    }
    rawstr = (const char **)&argv[4];
    f->exported = 1;
    if (!ribo_emit_wasm(m, wasm2_path, &err)) {
        fprintf(stderr, "%s\n", err.message);
        ribo_module_release(m);
        free(bytes);
        return 1;
    }
    ribo_module_release(m);

    /* original module gets the raw arg strings (typed params), the
       re-lowered module gets i64 cell bits (its wrapper ABI) */
    if (!node_run(argv[2], argv[3], ret_name(ret), rawstr, arity, orig, sizeof orig) ||
        !node_run(wasm2_path, argv[3], ret_name(ret), cellstr, arity, lifted, sizeof lifted)) {
        fprintf(stderr, "roundtrip: node harness failed\n");
        free(bytes);
        return 1;
    }
    free(bytes);
    if (!strcmp(orig, lifted) || canon_nan_equal(orig, lifted, ret)) {
        printf("{\"ok\":true,\"verdict\":\"roundtrip-match\",\"result\":\"%s\"}\n", orig);
        return 0;
    }
    printf("{\"ok\":false,\"verdict\":\"ROUNDTRIP MISMATCH\",\"original\":\"%s\","
           "\"lifted\":\"%s\"}\n", orig, lifted);
    return 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
        puts(RIBO_VERSION);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "check")) return cmd_check(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "run")) return cmd_run(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "test")) return cmd_test(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "lower")) return cmd_lower(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "verify")) return cmd_verify(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "lift")) return cmd_lift(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "roundtrip")) return cmd_roundtrip(argc, argv);
    return usage();
}
