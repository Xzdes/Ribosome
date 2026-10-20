# Ribosome — machine reference for code-generating models

Ribosome is a tiny register language whose only backend is WebAssembly.
Programs are `.rna` text files. Every program has a reference interpreter and
a wasm emitter; both must agree, and lifted modules must agree with their
origin — so any program you write has one exact meaning. This file is the
complete contract. When in doubt, imitate the idioms at the end — do not
improvise syntax.

## Absolute syntax rules

1. One instruction per line, always **exactly 3 fields** separated by spaces or
   tabs. Never more, never less.
2. Unused fields are written as `_`: `RET r0 _`, `LABEL loop _`, `END _ _`.
3. Operand groups use commas **without spaces**: `IGET r0 r1,4`.
   `CALL add r1, r2` (space after comma) is the most common syntax error.
4. `#` starts a comment (outside quoted strings). Blank lines are fine.
5. Everything is case-sensitive. Identifiers: `[A-Za-z_][A-Za-z0-9_.]*`,
   max 63 chars; a dot is legal inside (used for import namespaces).
6. The third field of `RET TRAP LABEL JMP MSIZE IMPORT EXPORT GLOB MEM TABLE
   END` must be `_` unless documented otherwise.
7. A module is a sequence of declarations. Declarations of one kind may be
   interleaved freely, but a `FUNC` body is atomic: `FUNC name T/N` …
   `END _ _`.
8. Files are UTF-8, max 4 MiB total, 511 bytes per line. Tabs are fine.

## Value model (one register class — the Codons lesson)

| Class | Names | Width | Notes |
|---|---|---|---|
| Registers | `r0`..`r255` | 64-bit cell | Per call frame. Holds raw bits of any type. |

There are **no typed register classes**. An opcode's prefix decides how the
bits of its operands are interpreted:

| Prefix | Type | Interpretation |
|---|---|---|
| `I` | i32 | low 32 bits, signed semantics; high bits ignored on write, zeroed on read |
| `L` | i64 | all 64 bits, signed |
| `F` | f32 | IEEE-754 binary32 in the low 32 bits |
| `D` | f64 | IEEE-754 binary64, all 64 bits |

Consequences:
- No WIDEN/NARROW. Conversions are explicit opcodes (`I2L`, `L2I`, …).
- `MOV r0 r1` copies all 64 raw bits, type-blind.
- Function argument types come from the callee's `FUNC T/N` signature, not
  from the registers.

Registers are **not** zero-initialized: you must write before you read on every
execution path (the compiler proves it; see "Initialization").

## Grammar

```
module   := { decl }
decl     := IMPORT name T "/" digit        # host capability (dot-namespaced)
          | EXPORT name _                  # func visible to the host
          | GLOB   name T                  # one module-level cell
          | GINIT  name lit                # non-zero initial value for a GLOB
          | MEM    name digit ["," digit]  # linear memory, min[,max] 64KiB pages
          | TABLE  name digit ["," digit]  # funcref table for CALLI
          | TELEM  off f1,f2               # table[off..] = funcs (imports ok)
          | DATA   off hex                 # memory[off..] = bytes (even-length hex)
          | FUNC   name T "/" digit { instr } END "_" "_"
instr    := op a b                         # always 3 fields
T        := "i32" | "i64" | "f32" | "f64" | "void"
```

- `FUNC add i32/2`: returns i32, takes 2 args (arrive in `r0`, `r1`).
  Arity is 0..8. Result type `void` requires `RET _ _`; any other type
  requires `RET rN _` reachable on every path.
- `IMPORT env.log void/1`: calls the host. A module run without that
  capability traps: `IMPORT requires host capability: env.log`.
- `GLOB counter i64`: zero-initialized module cell, shared by all funcs.
  `GINIT counter 66560` gives it a non-zero start (literal in the GLOB's
  type; float literals in hex-float form round-trip exactly).
- `MEM mem 1,16`: linear memory, min 1 page, max 16 (64 KiB pages).
  Addresses are i32 values held in registers.
- `TABLE t 4,16` + `TELEM 0 inc,dbl`: a funcref table; `t[0]=inc`, `t[1]=dbl`.
  Entries must fit the initial size. A void-returning import may not be a
  target (its wasm signature differs — parity with V8).
- `DATA 16 0a000000`: copies bytes into memory at instantiation; must fit the
  initial MEM size. Multiple DATA lines concatenate at their offsets.

## Opcodes

`dst` is a register written; `src` a register read; `lit` a literal.
**I- and L-ops accept `src` OR `lit` as the second operand** (`IADD r0 5`,
`LADD r0 r1`). F- and D-ops take registers only — load constants with
`FLIT`/`DLIT` first. Float literals keep full round-trip precision
(`DLIT r0 0.1`, hex forms `0x1.999999999999ap-4` accepted).

### Constants and raw moves
```
ILIT dst lit    dst = i32 literal (sign-extended bits)
LLIT dst lit    dst = i64 literal, full range
FLIT dst lit    dst = f32 literal
DLIT dst lit    dst = f64 literal
MOV  dst src    dst = all 64 bits of src (type-blind)
```

### Integer arithmetic (I/L; wraps mod 2^32 / 2^64)
```
ADD SUB MUL            e.g. IADD r0 r1, LSUB r0 1
DIV MOD                signed, trunc toward 0
DIVU MODU              unsigned
  faults: division by zero; MIN / -1 -> "division overflow".
  A literal 0 divisor is a COMPILE error.
AND OR XOR             bitwise
SHL SHR SAR            count 0..31 (I) / 0..63 (L) else fault;
                       SHR is logical, SAR is arithmetic
ROTL ROTR              rotate; count is MASKED mod 32/64 — no fault
                       (wasm semantics; unlike shifts, on purpose)
CLZ CTZ POP            bit counting, unary: ICLZ r0 _; of 0 = width
```

### Float arithmetic (F/D; IEEE-754 round-to-nearest-even)
```
ADD SUB MUL DIV
MIN MAX                wasm semantics: propagate NaN, -0 < +0
SQRT ABS NEG           unary: FSQRT r0 _
```

### Comparisons (all four prefixes) — dst = 0/1 as i32 bits
```
EQ NE LT LE GT GE      e.g. ILT r0 r1, DGE r2 r3
                       I/L: signed (…U suffix = unsigned: ILTU, IGEU)
                       comparisons OVERWRITE their first operand
```

### Conversions (op dst `_`)
```
L2I  dst _    wrap low 32 bits
I2L  dst _    sign-extend      I2LU dst _    zero-extend
F2I  F2L      trunc toward 0;  D2I  D2L      fault on NaN/overflow
F2IU F2LU     unsigned trunc;  D2IU D2LU     same faults (NaN/<= -1/>= 2^N)
I2F  I2D                       L2F  L2D      convert (signed)
F2D  dst _    widen            D2F  dst _    narrow (round)
```

### Linear memory (requires a MEM declaration)
Address = register holding i32 base + literal byte offset; group without
spaces: `IGET r0 r1,4`. Misaligned access is legal (wasm semantics).
Out of bounds -> fault `memory access out of bounds`.
```
IGET/LGET/FGET/DGET dst base,off    load natural width
IGET8S IGET8U IGET16S IGET16U       sub-width loads, sign/zero extend
LGET8S LGET8U LGET16S LGET16U LGET32S LGET32U
IPUT/LPUT/FPUT/DPUT base,off src    store natural width
IPUT8 IPUT16 LPUT8 LPUT16 LPUT32    sub-width stores (truncate)
MSIZE dst _                         dst = current size in pages
MGROW dst src                       grow by src pages; dst = old size or -1
```

### Globals
```
GGET dst name      dst = global bits
GSET name src      global = src bits (type from GLOB decl)
```

### Control flow
```
LABEL name _        local jump target (per-func namespace)
JMP  name _
JZ   src name       jump if low i32 of src == 0
JNZ  src name
CALL name r1,r2     up to 8 register args, must match callee arity;
                    result bits land in r0 (clobbered!)
CALLI rI r1,r2      indirect: call TABLE[low i32 of rI] with args r1,r2
                    (`CALLI rI _` for 0 args); result bits land in r0.
                    Faults: "indirect call index out of bounds",
                    "indirect call to uninitialized table entry",
                    "indirect call type mismatch" (arity differs)
RET  src _          return src (T != void); RET _ _ for void
TRAP _ _            unconditional fault "unreachable"
```

## Initialization rule

A register may be read only if it is written on **every** path leading to the
read. Function arguments `r0..r(N-1)` start initialized. After a branch, only
registers written on **both** arms count as initialized. Registers written only
inside a loop body are **not** initialized after the loop.

```
ILIT r1 0          # initialize BEFORE the loop
LABEL loop _
...
JNZ r2 loop
RET r1 _           # legal
```

Error text: `read of uninitialized register r1 on a reachable path`.

## Canonical idioms — copy these shapes

### if / else
```
JZ r0 else
# then-arm
JMP done _
LABEL else _
# else-arm
LABEL done _
```

### countdown loop (accumulator initialized before the loop, zero-trip guard)
```
ILIT r1 0          # acc
JZ r0 done         # n == 0 -> skip the loop entirely
LABEL loop _
IADD r1 r0         # acc += n
ISUB r0 1
JNZ r0 loop
LABEL done _
RET r1 _
```

### absolute value (i32) — compare a copy, and note: no integer NEG op
```
MOV r1 r0
ILT r1 0           # compare a COPY: comparisons overwrite their dst
JZ r1 done
MOV r2 r0          # negate via subtraction: r0 = 0 - r0
ILIT r0 0
ISUB r0 r2
LABEL done _
RET r0 _
```

### memory fill (dst pointer in r0, byte value in r1, count in r2)
```
ILIT r3 0          # i
LABEL loop _
MOV r4 r3
ILT r4 r2          # i < n?
JZ r4 done
MOV r5 r0
IADD r5 r3         # p + i
IPUT8 r5,0 r1
IADD r3 1
JMP loop _
LABEL done _
```

## Semantic pitfalls (these compile fine but compute garbage)

- **Comparisons overwrite their first operand.** `IEQ r1 r2`, `ILT r1 4`
  write `r1`. To keep a value, compare a copy. The top silent bug.
- **F/D comparisons need registers on both sides.** `DLT r0 1.5` is a syntax
  error; `DLIT r9 1.5` then `DLT r0 r9`.
- **A bottom-of-loop `JNZ r1 loop` must see the counter itself,** not a
  comparison flag left in the counter's register.
- **Guard zero-trip loops.** `ISUB r0 1` on 0 gives -1 and `JNZ` loops
  forever. Put `JZ r0 done` before the label.
- **CALL clobbers r0.** Save it first if you still need it.
- **MOD sign follows the dividend:** `-7 MOD 3` is `-1`.
- **Integer NEG does not exist.** Use `0 - x`. (Float NEG does: `FNEG`.)
- **F2I/D2I truncations fault** on NaN or out-of-range, like wasm `trunc`.
- **MGROW can return -1.** Check it before using new pages.

## Pitfalls (exact error → fix)

- `expected exactly 3 fields; found 4` → space after a comma. `IGET r0 r1,4`.
- `found 2` → missing field; pad with `_`: `RET r0 _`.
- `read of uninitialized register rN` → write it before this point on every
  path.
- `undefined label L in func main` → labels are per-func; add `LABEL L _`.
- `undefined func: f` → declare `FUNC f i32/N ... END _ _` or IMPORT it.
- `func f expects 2 arguments, got 3` → match the callee's arity (max 8).
- `module has no MEM declaration` → memory opcodes require `MEM name min[,max]`.
- `CALLI requires a TABLE declaration` → add `TABLE name min[,max]`.
- `DATA requires a MEM declaration` / `TELEM requires a TABLE declaration` →
  declare the container first.
- `DATA segment exceeds the initial memory size` → raise `MEM` min or move the
  segment; same for `TELEM entry exceeds the initial table size`.
- `IMPORT requires host capability: env.log` → run with the capability bound.
- `literal shift count must be in 0..31` (or 0..63 for L) → fix the constant.
- `division by literal zero` → constants are checked at compile time.

## Agent workflow

```
ribo check prog.rna --json     # fastest validation; errors are JSON:
                               # {"ok":false,"error":{"stage":..,"message":..,
                               #   "line":..,"line_text":..}}
ribo run prog.rna add 3 4      # reference interpreter
ribo lower prog.rna out.wasm   # emit wasm binary (no external toolchain)
ribo verify prog.rna add 3 4   # interpreter and wasm engine must agree
ribo lift mod.wasm out.rna     # wasm -> .rna (the other direction)
ribo roundtrip mod.wasm fn a.. # lift + lower + differential vs the original
                               # module under V8; a.. are the call arguments
ribo test prog.rna cases.csv   # CSV expectations: `add,3,4,7` or
                               # `div,1,0,trap:division by zero`
```

## Runtime faults (exact contract — both engines agree on the class)

| Text | Raised by |
|---|---|
| `division by zero` | IDIV/IMOD/… with a zero register divisor |
| `division overflow` | MIN_VALUE / -1 |
| `shift count out of range (0..31)` (or `0..63`) | SHL/SHR/SAR with a bad register count |
| `memory access out of bounds` | any load/store past the current memory |
| `invalid truncation to integer` | F2I/D2I/F2IU/… on NaN or out of range |
| `unreachable` | TRAP |
| `indirect call index out of bounds` | CALLI past the table |
| `indirect call to uninitialized table entry` | CALLI into a hole |
| `indirect call type mismatch` | CALLI arity (or void import) mismatch |
| `IMPORT requires host capability: <name>` | missing import binding |
| `call depth exhausted (limit: N)` | recursion past the configured depth |
| `execution budget exhausted (limit: N steps)` | step budget spent |

Lifting notes: stack slots become registers above the locals (r0..r7 are
scratch: CALL/CALLI results land in r0), blocks become LABEL/JMP pairs. wasm
shift counts are masked by the lifter (IAND/LAND before ISHL...) to preserve
semantics; rotations and clz/ctz/popcnt map 1:1. Tables become TABLE/TELEM,
call_indirect becomes CALLI, active data segments become DATA lines.
Lifter limits: no passive/declared element segments, no passive data, no
multi-value, one memory, one table, function imports only, arity ≤ 8,
register budget 256.

Limits (defaults; every one is runtime-configurable): 256 registers per func,
8 call arguments, call depth 1024, step budget 10 000 000 per invocation,
memory max 1024 pages (64 MiB) unless MEM says otherwise, 4096 funcs per
module, 64 imports.
