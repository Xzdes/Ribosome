# Ribosome

**Ribosome lifts WebAssembly into a form an AI can read, edit, and rebuild with
proven equivalence — entirely in the browser.**

Wasm modules are everywhere (plugins, smart contracts, serverless), but their
readable form, WAT, is a stack machine with nested S-expressions: hostile to
both humans and language models. Existing decompilers produce pseudo-C you can
read but cannot safely edit and recompile.

Ribosome defines a tiny register-based language (`.rna` files), derived
mechanically from the WebAssembly MVP, with one rule above all: **round-trip
with proof**. Lift a `.wasm` to `.rna`, edit it (by hand or by model), lower it
back to `.wasm`, and a differential harness proves behavioral equivalence
against the original — using the browser's own engine as the oracle.

```
somebody.wasm ──lift──> somebody.rna ──edit──> lower ──> patched.wasm
                  │                                      │
                  └──── differential run, same V8 ───────┘
                        outputs and traps must match
```

## Design inheritance

Ribosome is a sibling of [Codons](../Codons) — same engineering discipline,
different domain. Codons is a closed world for pure computation and stays
untouched. Ribosome takes its culture and leaves its constraints behind:

| Inherited from Codons | Left behind |
|---|---|
| exactly 3 fields per line, `_` padding | Win64 JIT (the browser's wasm engine *is* the JIT) |
| exact machine errors with `line_text` | split i32/i64 register classes (one 64-bit class) |
| capability model for the outside world | string/stdout machinery (output is DOM/canvas) |
| differential verification as law | hardcoded limits (all limits are runtime config) |
| LLM.md as a first-class artifact | CLI-first workflow (browser is the IDE) |

## Status

**1.0 — the wasm MVP is fully closed.** Parse, check, interpret, emit, lift,
verify: all layers cover the numeric core, memory, globals, tables with
indirect calls, and data segments. The lifter survived ~1000 fuzzed random
modules round-tripping bit-exact against V8, traps included.

## Run it

```
cmake --build build            # the CLI: build/ribo.exe
ribo check prog.rna --json     # validate, exact JSON errors
ribo verify prog.rna add 3 4   # interpreter vs V8 must agree
ribo roundtrip mod.wasm f a..  # lift + lower + diff against the original
node tests/fuzz/fuzz.js 500    # fuzz the lifter
```

## The browser page

`web/` is fully static: `index.html` + `ribo.wasm` (~122 KB, freestanding,
no libc). Any static host works (GitHub Pages serves `application/wasm`
correctly). Locally:

```
node web/serve.js 8086         # then open http://127.0.0.1:8086/
```

The page edits `.rna`, runs it in the reference interpreter *and* as compiled
wasm in the same tab, and diffs the two bit-for-bit. Drag a `.wasm` file onto
it to lift.

## Layout

```
include/ribo.h   public C API contract
src/             core: parser, interpreter, wasm emitter, lifter
web/             browser shell (editor + lift/lower/verify)
tests/           differential suites
examples/        reference .rna programs with test cases
docs/            architecture, design decisions, roadmap
```

## Principles

1. **Sized to the domain, not a byte larger.** The opcode set is derived from
   the wasm MVP, not invented. No sugar.
2. **Verification is not optional.** Every claim the toolchain makes is
   checkable by a machine in the same session.
3. **The contract fits in one head.** LLM.md is complete; if a feature cannot
   be described there, it does not exist.
4. **Browser-native.** No installation, no build step for the user; the whole
   toolchain compiles to wasm and runs in a tab.
