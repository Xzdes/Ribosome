#ifndef RIBO_H
#define RIBO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define RIBO_VERSION "1.0.1"

/* Default limits. Every limit is a runtime configuration field, never a
   baked-in constant: hosts (a browser tab, a CLI, a test harness) have
   different budgets. These are the defaults when RiboLimits is zeroed. */
#define RIBO_DEFAULT_STEP_LIMIT 10000000u
#define RIBO_MAX_REGISTERS 256u
#define RIBO_MAX_ARGS 8u
#define RIBO_MAX_DEPTH 1024u
#define RIBO_MAX_PAGES 1024u /* 64 MiB */
#define RIBO_MAX_FUNCS 4096u
#define RIBO_MAX_IMPORTS 64u

typedef struct RiboModule RiboModule; /* parsed + checked .rna, immutable */
typedef struct RiboWasm   RiboWasm;   /* parsed .wasm pending lift       */

typedef struct {
    char message[1536];
} RiboError;

/* All execution limits in one place. A zeroed struct means defaults. */
typedef struct {
    uint64_t step_limit;   /* executed instructions per invocation */
    uint32_t max_depth;    /* active call frames */
    uint32_t max_pages;    /* linear memory ceiling, 64 KiB pages */
} RiboLimits;

/* Host capabilities. A module's IMPORT is bound by name to a callback; an
   unbound import traps with "IMPORT requires host capability: <name>".
   Arguments arrive as raw 64-bit register cells; the callback writes the
   result cell (ignored for void imports) and returns nonzero on success. */
typedef struct {
    const char *name;                       /* e.g. "env.log"             */
    int (*call)(void *user, const uint64_t *args, uint32_t arg_count,
                uint64_t *result);
    void *user;
} RiboImport;

typedef struct {
    const RiboImport *imports;
    size_t import_count;
} RiboIO;

/* A single argument or result cell. The low 32 bits carry i32/f32. */
typedef uint64_t RiboValue;

/* ---- module life cycle ---- */

/* Parse and check a .rna file. With repair enabled, unambiguous omitted '_'
   fields are restored; ambiguous instructions become a runtime quarantine.
   The source file is never changed. Returns NULL on failure with error set. */
RiboModule *ribo_compile(const char *path, int repair, RiboError *error);
/* In-memory variant for hosts without a filesystem (the browser shell).
   text is the whole .rna source, NUL-terminated. */
RiboModule *ribo_compile_text(const char *text, int repair, RiboError *error);
void ribo_module_release(RiboModule *module);

/* ---- execution (reference interpreter) ---- */

/* Invoke an exported (or any) function by name with up to RIBO_MAX_ARGS
   argument cells. Returns 1 on success and stores the result bits; on a fault
   returns 0 and fills error with the exact contract text from LLM.md. */
int ribo_interpret(const RiboModule *module, const char *func,
                   const RiboValue *args, size_t arg_count,
                   const RiboIO *io, const RiboLimits *limits,
                   RiboValue *result, RiboError *error);

/* ---- wasm emission (no external toolchain) ---- */

/* Emit a valid wasm binary. The emitted module, executed by any conforming
   engine, must agree with ribo_interpret on results and on fault outcomes. */
int ribo_emit_wasm(const RiboModule *module, const char *out_path,
                   RiboError *error);
/* In-memory emission; the caller owns *out_bytes (free()). */
int ribo_emit_wasm_mem(const RiboModule *module, uint8_t **out_bytes,
                       size_t *out_len, RiboError *error);

/* ---- lifting (0.5) ---- */

/* Parse a wasm binary for lifting. Returns NULL on malformed input. */
RiboWasm *ribo_wasm_parse(const uint8_t *bytes, size_t length, RiboError *error);
void ribo_wasm_release(RiboWasm *wasm);
/* Lift to a .rna source file. lift(lift-source) roundtrip equivalence is
   proven by differential testing, not assumed. */
int ribo_wasm_lift(const RiboWasm *wasm, const char *out_path,
                   RiboError *error);
/* In-memory lift; the caller owns *out_text (free(), NUL-terminated). */
int ribo_wasm_lift_mem(const RiboWasm *wasm, char **out_text,
                       RiboError *error);
/* Parameter types (T_*) of an exported function, in order. Returns the
   arity, or -1 when the export does not exist. Needed by differential
   harnesses to encode float arguments into i64 cells correctly. */
int ribo_wasm_param_types(const RiboWasm *wasm, const char *func,
                          uint8_t *types, int max);

#endif
