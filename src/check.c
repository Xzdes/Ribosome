/* check.c — static proofs over a parsed module.
   1. Register initialization: a register may be read only if written on
      every path leading to the read (dataflow over the jump graph).
   2. RET coverage: no path may fall off the end of a FUNC.
   3. CALL arity and result-type agreement with RET.
   4. Memory opcodes require a MEM declaration.
   Errors carry the exact contract texts from LLM.md. */
#include "ribo_internal.h"
#include <stdlib.h>
#include <string.h>

#define NWORDS 4 /* 256 registers as a 4x64 bitmask */

typedef struct {
    uint64_t in[NWORDS];
    uint8_t reachable;
} Flow;

static void setbit(uint64_t *w, int b) { w[b >> 6] |= 1ull << (b & 63); }
static int getbit(const uint64_t *w, int b) { return (int)((w[b >> 6] >> (b & 63)) & 1); }

/* which registers does this instruction read (before writing)? */
static void reads_of(const RiboInstr *in, uint64_t *rd) {
    memset(rd, 0, NWORDS * sizeof *rd);
    switch (in->op) {
    case OP_MOV:
        setbit(rd, in->src);
        break;
    case OP_ILIT: case OP_LLIT: case OP_FLIT: case OP_DLIT:
    case OP_MSIZE:
        break;
    case OP_FSQRT: case OP_FABS: case OP_FNEG:
    case OP_DSQRT: case OP_DABS: case OP_DNEG:
    case OP_L2I: case OP_I2L: case OP_I2LU:
    case OP_F2I: case OP_F2L: case OP_D2I: case OP_D2L:
    case OP_I2F: case OP_L2F: case OP_I2D: case OP_L2D:
    case OP_F2D: case OP_D2F:
    case OP_F2IU: case OP_F2LU: case OP_D2IU: case OP_D2LU:
    case OP_ICLZ: case OP_ICTZ: case OP_IPOP:
    case OP_LCLZ: case OP_LCTZ: case OP_LPOP:
        setbit(rd, in->dst); /* unary: dst is read and rewritten */
        break;
    case OP_MGROW:
        setbit(rd, in->src);
        break;
    case OP_IGET: case OP_LGET: case OP_FGET: case OP_DGET:
    case OP_IGET8S: case OP_IGET8U: case OP_IGET16S: case OP_IGET16U:
    case OP_LGET8S: case OP_LGET8U: case OP_LGET16S: case OP_LGET16U:
    case OP_LGET32S: case OP_LGET32U:
        setbit(rd, in->src); /* base register */
        break;
    case OP_IPUT: case OP_LPUT: case OP_FPUT: case OP_DPUT:
    case OP_IPUT8: case OP_IPUT16:
    case OP_LPUT8: case OP_LPUT16: case OP_LPUT32:
        setbit(rd, in->dst); /* base register */
        setbit(rd, in->src); /* value register */
        break;
    case OP_GGET:
        break;
    case OP_GSET:
        setbit(rd, in->src);
        break;
    case OP_JMP:
        break;
    case OP_JZ: case OP_JNZ:
        setbit(rd, in->dst); /* tested register lives in dst */
        break;
    case OP_CALL: {
        int i;
        for (i = 0; i < in->nargs; i++) setbit(rd, in->args[i]);
        break;
    }
    case OP_CALLI: {
        int i;
        setbit(rd, in->dst); /* the table index */
        for (i = 0; i < in->nargs; i++) setbit(rd, in->args[i]);
        break;
    }
    case OP_RET:
        if (in->nargs) setbit(rd, in->dst);
        break;
    case OP_TRAP:
        break;
    default:
        /* binary / comparison: reads dst and (src unless literal) */
        setbit(rd, in->dst);
        if (!in->src_is_lit) setbit(rd, in->src);
        break;
    }
}

/* which registers does this instruction write? */
static void writes_of(const RiboInstr *in, uint64_t *wr) {
    memset(wr, 0, NWORDS * sizeof *wr);
    switch (in->op) {
    case OP_IPUT: case OP_LPUT: case OP_FPUT: case OP_DPUT:
    case OP_IPUT8: case OP_IPUT16:
    case OP_LPUT8: case OP_LPUT16: case OP_LPUT32:
    case OP_GSET:
    case OP_JMP: case OP_JZ: case OP_JNZ:
    case OP_TRAP:
        break;
    case OP_CALL:
    case OP_CALLI:
        setbit(wr, 0); /* result lands in r0 */
        break;
    case OP_RET:
        break;
    default:
        setbit(wr, in->dst);
        break;
    }
}

static int is_memory_op(uint16_t op) {
    return (op >= OP_IGET && op <= OP_LGET32U) ||
           (op >= OP_IPUT && op <= OP_LPUT32) ||
           op == OP_MSIZE || op == OP_MGROW;
}

static int check_func(RiboModule *m, RiboFunc *f, RiboDiag *diag) {
    size_t n = f->code_len, i;
    Flow *flow;
    int changed, end_reachable;
    const char *lt;

    if (n == 0) {
        ribo_diag_set(diag, "check", f->decl_line, "",
                      "func %s may reach END without RET", f->name);
        return 0;
    }
    flow = calloc(n, sizeof *flow);
    if (!flow) { ribo_diag_set(diag, "check", 0, "", "out of memory"); return 0; }

    /* must-analysis: "written on EVERY path" — start at TOP (all set),
       join at merge points is INTERSECTION; only the entry is exact */
    for (i = 1; i < n; i++) memset(flow[i].in, 0xFF, sizeof flow[i].in);
    flow[0].reachable = 1;
    memset(flow[0].in, 0, sizeof flow[0].in);
    for (i = 0; i < f->arity; i++) setbit(flow[0].in, (int)i);

    end_reachable = 0;
    do {
        changed = 0;
        for (i = 0; i < n; i++) {
            RiboInstr *in = &f->code[i];
            uint64_t out[NWORDS], wr[NWORDS];
            int w;
            if (!flow[i].reachable) continue;
            memcpy(out, flow[i].in, sizeof out);
            writes_of(in, wr);
            for (w = 0; w < NWORDS; w++) out[w] |= wr[w];
            /* successors */
            if (in->op == OP_RET || in->op == OP_TRAP) continue;
            if (in->op == OP_JMP) {
                size_t t = (size_t)in->imm;
                if (t >= n) { end_reachable = 1; continue; }
                if (!flow[t].reachable) { flow[t].reachable = 1; changed = 1; }
                for (w = 0; w < NWORDS; w++) {
                    uint64_t nw = flow[t].in[w] & out[w]; /* join: intersect */
                    if (nw != flow[t].in[w]) { flow[t].in[w] = nw; changed = 1; }
                }
                continue;
            }
            if (in->op == OP_JZ || in->op == OP_JNZ) {
                size_t t = (size_t)in->imm;
                if (t >= n) end_reachable = 1;
                else {
                    if (!flow[t].reachable) { flow[t].reachable = 1; changed = 1; }
                    for (w = 0; w < NWORDS; w++) {
                        uint64_t nw = flow[t].in[w] & out[w];
                        if (nw != flow[t].in[w]) { flow[t].in[w] = nw; changed = 1; }
                    }
                }
            }
            /* fall-through */
            if (i + 1 >= n) { end_reachable = 1; continue; }
            if (!flow[i + 1].reachable) { flow[i + 1].reachable = 1; changed = 1; }
            for (w = 0; w < NWORDS; w++) {
                uint64_t nw = flow[i + 1].in[w] & out[w];
                if (nw != flow[i + 1].in[w]) { flow[i + 1].in[w] = nw; changed = 1; }
            }
        }
    } while (changed);

    /* reads must be initialized on every reachable path */
    for (i = 0; i < n; i++) {
        RiboInstr *in = &f->code[i];
        uint64_t rd[NWORDS];
        int b;
        if (!flow[i].reachable) continue;
        reads_of(in, rd);
        for (b = 0; b < 256; b++) {
            if (getbit(rd, b) && !getbit(flow[i].in, b)) {
                lt = (in->line <= m->line_count) ? m->lines[in->line - 1] : "";
                ribo_diag_set(diag, "check", in->line, lt,
                              "read of uninitialized register r%d on a reachable path", b);
                free(flow);
                return 0;
            }
        }
        /* CALL arity */
        if (in->op == OP_CALL) {
            size_t idx = (size_t)in->imm;
            unsigned need, got = in->nargs;
            const char *nm;
            if (idx < m->func_count) {
                need = m->funcs[idx].arity;
                nm = m->funcs[idx].name;
            } else {
                need = m->imports[idx - m->func_count].arity;
                nm = m->imports[idx - m->func_count].name;
            }
            if (need != got) {
                lt = (in->line <= m->line_count) ? m->lines[in->line - 1] : "";
                ribo_diag_set(diag, "check", in->line, lt,
                              "func %s expects %u arguments, got %u", nm, need, got);
                free(flow);
                return 0;
            }
        }
        if (is_memory_op(in->op) && !m->has_mem) {
            lt = (in->line <= m->line_count) ? m->lines[in->line - 1] : "";
            ribo_diag_set(diag, "check", in->line, lt,
                          "module has no MEM declaration");
            free(flow);
            return 0;
        }
        /* RET shape vs signature */
        if (in->op == OP_RET) {
            if (f->ret == T_VOID && in->nargs) {
                lt = (in->line <= m->line_count) ? m->lines[in->line - 1] : "";
                ribo_diag_set(diag, "check", in->line, lt,
                              "void func %s must use RET _ _", f->name);
                free(flow);
                return 0;
            }
            if (f->ret != T_VOID && !in->nargs) {
                lt = (in->line <= m->line_count) ? m->lines[in->line - 1] : "";
                ribo_diag_set(diag, "check", in->line, lt,
                              "func %s returns a value; use RET rN _", f->name);
                free(flow);
                return 0;
            }
        }
    }

    if (end_reachable) {
        ribo_diag_set(diag, "check", f->decl_line, "",
                      "func %s may reach END without RET", f->name);
        free(flow);
        return 0;
    }
    free(flow);
    return 1;
}

int ribo_check_module(RiboModule *m, RiboDiag *diag) {
    size_t i;
    /* module-level proofs: DATA needs MEM and fits the initial size;
       TELEM needs TABLE and fits the initial size; CALLI needs a table */
    if (m->data_count && !m->has_mem)
        return ribo_diag_set(diag, "check", 0, "",
                             "DATA requires a MEM declaration"), 0;
    for (i = 0; i < m->data_count; i++)
        if ((uint64_t)m->datas[i].off + m->datas[i].len >
            (uint64_t)m->mem_min * 65536u)
            return ribo_diag_set(diag, "check", 0, "",
                                 "DATA segment exceeds the initial memory size"), 0;
    if (m->elem_count && !m->has_table)
        return ribo_diag_set(diag, "check", 0, "",
                             "TELEM requires a TABLE declaration"), 0;
    for (i = 0; i < m->elem_count; i++)
        if (m->elems[i].off >= m->table_min)
            return ribo_diag_set(diag, "check", 0, "",
                                 "TELEM entry exceeds the initial table size"), 0;
    if (!m->has_table)
        for (i = 0; i < m->func_count; i++) {
            size_t j;
            for (j = 0; j < m->funcs[i].code_len; j++)
                if (m->funcs[i].code[j].op == OP_CALLI)
                    return ribo_diag_set(diag, "check",
                                         m->funcs[i].code[j].line, "",
                                         "CALLI requires a TABLE declaration"), 0;
        }
    for (i = 0; i < m->func_count; i++)
        if (!check_func(m, &m->funcs[i], diag)) return 0;
    return 1;
}
