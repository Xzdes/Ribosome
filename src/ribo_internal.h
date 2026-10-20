#ifndef RIBO_INTERNAL_H
#define RIBO_INTERNAL_H

#include "ribo.h"

/* result types (FUNC/IMPORT signatures) */
enum { T_VOID = 0, T_I32 = 1, T_I64 = 2, T_F32 = 3, T_F64 = 4 };

typedef enum {
    OP_ILIT, OP_LLIT, OP_FLIT, OP_DLIT, OP_MOV,
    OP_IADD, OP_ISUB, OP_IMUL, OP_IDIV, OP_IMOD, OP_IDIVU, OP_IMODU,
    OP_IAND, OP_IOR, OP_IXOR, OP_ISHL, OP_ISHR, OP_ISAR,
    OP_LADD, OP_LSUB, OP_LMUL, OP_LDIV, OP_LMOD, OP_LDIVU, OP_LMODU,
    OP_LAND, OP_LOR, OP_LXOR, OP_LSHL, OP_LSHR, OP_LSAR,
    OP_FADD, OP_FSUB, OP_FMUL, OP_FDIV, OP_FMIN, OP_FMAX,
    OP_FSQRT, OP_FABS, OP_FNEG,
    OP_DADD, OP_DSUB, OP_DMUL, OP_DDIV, OP_DMIN, OP_DMAX,
    OP_DSQRT, OP_DABS, OP_DNEG,
    OP_IEQ, OP_INE, OP_ILT, OP_ILE, OP_IGT, OP_IGE,
    OP_ILTU, OP_ILEU, OP_IGTU, OP_IGEU,
    OP_LEQ, OP_LNE, OP_LLT, OP_LLE, OP_LGT, OP_LGE,
    OP_LLTU, OP_LLEU, OP_LGTU, OP_LGEU,
    OP_FEQ, OP_FNE, OP_FLT, OP_FLE, OP_FGT, OP_FGE,
    OP_DEQ, OP_DNE, OP_DLT, OP_DLE, OP_DGT, OP_DGE,
    OP_L2I, OP_I2L, OP_I2LU,
    OP_F2I, OP_F2L, OP_D2I, OP_D2L,
    OP_I2F, OP_L2F, OP_I2D, OP_L2D, OP_F2D, OP_D2F,
    OP_IGET, OP_LGET, OP_FGET, OP_DGET,
    OP_IGET8S, OP_IGET8U, OP_IGET16S, OP_IGET16U,
    OP_LGET8S, OP_LGET8U, OP_LGET16S, OP_LGET16U, OP_LGET32S, OP_LGET32U,
    OP_IPUT, OP_LPUT, OP_FPUT, OP_DPUT,
    OP_IPUT8, OP_IPUT16, OP_LPUT8, OP_LPUT16, OP_LPUT32,
    OP_MSIZE, OP_MGROW,
    OP_GGET, OP_GSET,
    OP_JMP, OP_JZ, OP_JNZ, OP_CALL, OP_RET, OP_TRAP,
    /* 1.0 additions: unsigned truncations, bit counting, rotations.
       Rotations mask the count like wasm (no trap) — unlike shifts. */
    OP_F2IU, OP_F2LU, OP_D2IU, OP_D2LU,
    OP_ICLZ, OP_ICTZ, OP_IPOP, OP_LCLZ, OP_LCTZ, OP_LPOP,
    OP_IROTL, OP_IROTR, OP_LROTL, OP_LROTR,
    /* call through the function table (wasm call_indirect) */
    OP_CALLI,
    OP__COUNT
} RiboOp;

typedef struct { uint32_t off; uint32_t func; } RiboElem;  /* table entry */
typedef struct { uint32_t off; uint8_t *bytes; size_t len; } RiboData;

typedef struct {
    uint16_t op;
    uint8_t dst;         /* first operand register (or src for JCC/RET) */
    uint8_t src;         /* second operand register when src_is_lit == 0 */
    uint8_t src_is_lit;  /* second operand is the literal in imm */
    uint8_t nargs;       /* CALL: number of argument registers */
    uint8_t args[RIBO_MAX_ARGS];
    int64_t imm;         /* literal / resolved jump target / func / global idx */
    uint32_t line;       /* 1-based source line */
} RiboInstr;

typedef struct {
    char name[64];
    uint8_t type;
    uint64_t bits;       /* zero-initialized at parse */
} RiboGlobal;

typedef struct {
    char name[64];
    uint8_t ret;
    uint8_t arity;
} RiboImportDecl;

typedef struct {
    char name[64];
    uint8_t ret, arity, exported;
    uint32_t decl_line;
    RiboInstr *code;
    size_t code_len, code_cap;
} RiboFunc;

struct RiboModule {
    RiboFunc *funcs;
    size_t func_count, func_cap;
    RiboGlobal *globals;
    size_t glob_count, glob_cap;
    RiboImportDecl *imports;
    size_t import_count, import_cap;
    int has_mem;
    uint32_t mem_min, mem_max; /* mem_max == 0 means "runtime limit" */
    int has_table;
    uint32_t table_min, table_max; /* table_max == 0 means "runtime limit" */
    RiboElem *elems;
    size_t elem_count, elem_cap;
    RiboData *datas;
    size_t data_count, data_cap;
    char **lines;              /* source lines for line_text in errors */
    size_t line_count;
};

/* Structured diagnostic: what the CLI renders as JSON and the public API
   flattens into RiboError.message. */
typedef struct {
    char stage[16];       /* "parse" | "check" | "run" */
    uint32_t line;        /* 0 when not applicable */
    char line_text[512];
    char message[1024];
} RiboDiag;

void ribo_diag_set(RiboDiag *d, const char *stage, uint32_t line,
                   const char *line_text, const char *fmt, ...);
void ribo_diag_to_public(const RiboDiag *d, RiboError *error);

/* parse.c */
int ribo_parse_source(const char *text, int repair, RiboModule **out,
                      RiboDiag *diag);
/* check.c */
int ribo_check_module(RiboModule *m, RiboDiag *diag);
/* interp.c (also the ribo_interpret public entry) */
RiboFunc *ribo_find_func(RiboModule *m, const char *name);

#endif
