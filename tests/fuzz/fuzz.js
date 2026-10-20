// fuzz.js — generates random "compiler-shaped" wasm MVP modules and runs
// `ribo roundtrip` on each: lift -> lower -> differential vs the original
// under V8. Any mismatch or lift failure is a bug.
// Usage: node tests/fuzz/fuzz.js [count] [seed0]
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const RIBO = path.join(__dirname, '..', '..', 'build', 'ribo.exe');
const TMP = path.join(__dirname, 'fuzz_tmp.wasm');

// ---------- tiny wasm encoder (same shapes as build_fixtures.js) ----------
const u = (v) => {
    const out = [];
    v >>>= 0;
    do { let b = v & 0x7F; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v);
    return out;
};
const s = (v) => {
    v = BigInt(v);
    const out = [];
    for (;;) {
        let b = Number(v & 0x7Fn);
        v >>= 7n;
        if ((v === 0n && !(b & 0x40)) || (v === -1n && (b & 0x40))) { out.push(b); break; }
        out.push(b | 0x80);
    }
    return out;
};
const str = (x) => [...u(x.length), ...Buffer.from(x, 'utf8')];
const sec = (id, payload) => [id, ...u(payload.length), ...payload];
const vec = (items) => [...u(items.length), ...items.flat()];

const I32 = 0x7F;
const OP = {
    block: 0x02, loop: 0x03, if: 0x04, else: 0x05, end: 0x0B,
    br: 0x0C, br_if: 0x0D, call: 0x10, calli: 0x11,
    drop: 0x1A, select: 0x1B,
    lget: 0x20, lset: 0x21, ltee: 0x22,
    i32load: 0x28, i32store: 0x36,
    i32const: 0x41,
    clz: 0x67, ctz: 0x68, pop: 0x69,
    add: 0x6A, sub: 0x6B, mul: 0x6C, divs: 0x6D, divu: 0x6E,
    rems: 0x6F, remu: 0x70, and: 0x71, or: 0x72, xor: 0x73,
    shl: 0x74, shrs: 0x75, shru: 0x76, rotl: 0x77, rotr: 0x78,
    eqz: 0x45, eq: 0x46, ne: 0x47, lts: 0x48, ltu: 0x49,
};
const OP64 = {
    lconst: 0x42, lload: 0x29, lstore: 0x37,
    clz: 0x79, ctz: 0x7A, pop: 0x7B,
    add: 0x7C, sub: 0x7D, mul: 0x7E, divs: 0x7F, divu: 0x80,
    rems: 0x81, remu: 0x82, and: 0x83, or: 0x84, xor: 0x85,
    shl: 0x86, shrs: 0x87, shru: 0x88, rotl: 0x89, rotr: 0x8A,
};
const BINOPS32 = [OP.add, OP.sub, OP.mul, OP.and, OP.or, OP.xor,
                  OP.divs, OP.divu, OP.rems, OP.remu,
                  OP.shl, OP.shrs, OP.shru, OP.rotl, OP.rotr,
                  OP.eq, OP.ne, OP.lts, OP.ltu];
const BINOPS64 = [OP64.add, OP64.sub, OP64.mul, OP64.and, OP64.or, OP64.xor,
                  OP64.divs, OP64.divu, OP64.rems, OP64.remu,
                  OP64.shl, OP64.shrs, OP64.shru, OP64.rotl, OP64.rotr];
const UNOPS32 = [OP.clz, OP.ctz, OP.pop, OP.eqz];
const UNOPS64 = [OP64.clz, OP64.ctz, OP64.pop];

// ---------- seeded PRNG ----------
function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

// ---------- module generator ----------
// All funcs share one type: (W, W) -> W where W is i32 or i64 per module.
// 2 params + up to 3 locals.
function genModule(seed) {
    const rnd = mulberry32(seed);
    const ri = (n) => (rnd() * n) | 0;
    const is64 = rnd() < 0.4;
    const WT = is64 ? 0x7E : I32;
    const COP = is64 ? OP64.lconst : OP.i32const;
    const LOP = is64 ? OP64.lload : OP.i32load;
    const SOP = is64 ? OP64.lstore : OP.i32store;
    const ALN = is64 ? 3 : 2;
    const MASK = is64 ? 1016 : 1020;
    const BINOPS = is64 ? BINOPS64 : BINOPS32;
    const UNOPS = is64 ? UNOPS64 : UNOPS32;
    const nfuncs = 1 + ri(3);            // 1..3
    const hasMem = rnd() < 0.6;
    const hasTable = nfuncs > 1 && rnd() < 0.5;
    const nlocals = ri(4);               // extra locals per func (uniform: fine)

    // addresses are always i32 (wasm), conditions too (via i64.eqz in 64 mode)
    const addrExpr = (d) => [...genExprI32(d), OP.i32const, ...s(MASK), OP.and];
    const condExpr = (d) => is64 ? [...genExpr(d), 0x50 /* i64.eqz */]
                                 : genExpr(d);
    function genExprI32(depth) {         // small i32 producer for addresses
        if (depth <= 0) return [OP.i32const, ...s(ri(1024))];
        return [...genExprI32(depth - 1), ...genExprI32(0),
                [OP.add, OP.xor, OP.sub][ri(3)]];
    }

    function genExpr(depth) {            // leaves one W on the stack
        const localIdx = () => ri(2 + nlocals);
        if (depth <= 0) {
            const w = rnd();
            if (w < 0.4) return [COP, ...s(ri(100))];
            if (w < 0.8) return [OP.lget, ...u(localIdx())];
            if (hasMem) return [...addrExpr(1), LOP, ...u(ALN), ...u(0)];
            return [COP, ...s(ri(100))];
        }
        const w = rnd();
        if (w < 0.45) { // binary op
            return [...genExpr(depth - 1), ...genExpr(depth - 1),
                    BINOPS[ri(BINOPS.length)]];
        }
        if (w < 0.55) { // unary
            return [...genExpr(depth - 1), UNOPS[ri(UNOPS.length)]];
        }
        if (w < 0.62) { // select (condition must be i32)
            return [...genExpr(depth - 1), ...genExpr(depth - 1),
                    ...condExpr(depth - 1), OP.select];
        }
        if (w < 0.68 && hasMem) { // masked load
            return [...addrExpr(depth - 1), LOP, ...u(ALN), ...u(0)];
        }
        if (w < 0.76 && nfuncs > 1) { // direct call to a random func
            const f = ri(nfuncs);
            return [...genExpr(depth - 1), ...genExpr(depth - 1),
                    OP.call, ...u(f)];
        }
        if (w < 0.82 && hasTable) { // indirect call, index mod nfuncs
            return [...genExpr(depth - 1), ...genExpr(depth - 1),
                    ...genExprI32(depth - 1),   /* index: always i32 */
                    OP.i32const, ...s(nfuncs), OP.remu,
                    OP.calli, ...u(0), 0x00];
        }
        if (w < 0.9) { // if/else producing a value
            return [...condExpr(depth - 1),
                    OP.if, WT,
                      ...genExpr(depth - 1),
                    OP.else,
                      ...genExpr(depth - 1),
                    OP.end];
        }
        // block with a value + br_if escape hatch
        return [OP.block, WT,
                  ...genExpr(depth - 1),
                  ...condExpr(depth - 1), OP.br_if, ...u(0),
                  OP.drop, ...genExpr(depth - 1),
                OP.end];
    }

    function genBody(fidx) {
        const expr = [];
        if (hasMem && rnd() < 0.5) { // a couple of masked stores first
            for (let k = 0; k < 2; k++)
                expr.push(...addrExpr(1), ...genExpr(0), SOP, ...u(ALN), ...u(0));
        }
        if (nlocals > 0 && rnd() < 0.5) { // scribble into extra locals
            for (let k = 0; k < nlocals; k++)
                if (rnd() < 0.7) expr.push(...genExpr(0), OP.lset, ...u(2 + k));
        }
        expr.push(...genExpr(3));
        return { locals: nlocals ? [{ count: nlocals, type: WT }] : [], expr };
    }

    const bytes = [0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00];
    bytes.push(...sec(1, vec([[0x60, ...vec([[WT], [WT]]), ...vec([[WT]])]])));
    bytes.push(...sec(3, vec(Array.from({ length: nfuncs }, () => u(0)))));
    if (hasTable)
        bytes.push(...sec(4, vec([[0x70, 0x00, ...u(nfuncs)]])));
    if (hasMem)
        bytes.push(...sec(5, vec([[0x00, ...u(1)]])));
    bytes.push(...sec(7, vec([[...str('main'), 0x00, ...u(0)]])));
    if (hasTable) {
        const order = Array.from({ length: nfuncs }, (_, i) => i);
        for (let i = order.length - 1; i > 0; i--) { // shuffle
            const j = ri(i + 1);
            [order[i], order[j]] = [order[j], order[i]];
        }
        bytes.push(...sec(9, vec([[0x00, OP.i32const, ...s(0), OP.end,
                                   ...vec(order.map(f => u(f)))]])));
    }
    bytes.push(...sec(10, vec(Array.from({ length: nfuncs }, (_, i) => {
        const body = genBody(i);
        const locals = vec(body.locals.map(l => [...u(l.count), l.type]));
        const full = [...locals, ...body.expr, OP.end];
        return [...u(full.length), ...full];
    }))));
    return Buffer.from(bytes);
}

// ---------- runner ----------
const COUNT = Number(process.argv[2] || 100);
const SEED0 = Number(process.argv[3] || 1);
let fails = 0, lifts = 0, mismatches = 0;

for (let i = 0; i < COUNT; i++) {
    const seed = SEED0 + i;
    const buf = genModule(seed);
    fs.writeFileSync(TMP, buf);
    const a = (seed * 7) % 97, b = (seed * 13) % 89;
    const r = spawnSync(RIBO, ['roundtrip', TMP, 'main', String(a), String(b)],
                        { encoding: 'utf8', timeout: 15000 });
    const line = (r.stdout || '').trim().split('\n').pop() || '';
    let j = null;
    try { j = JSON.parse(line); } catch { /* fallthrough */ }
    if (!j) {
        console.log(`seed ${seed}: CRASH/NO-JSON exit=${r.status} ${(r.stderr || '').trim().slice(0, 200)}`);
        fs.copyFileSync(TMP, path.join(__dirname, `fuzz_fail_${seed}.wasm`));
        fails++;
        continue;
    }
    if (!j.ok) {
        const liftBug = /lift|parse|check/.test(j.error && j.error.message || '');
        console.log(`seed ${seed}: FAIL ${j.error && j.error.message}`);
        fs.copyFileSync(TMP, path.join(__dirname, `fuzz_fail_${seed}.wasm`));
        fails++;
        if (liftBug) lifts++;
        continue;
    }
    if (j.verdict !== 'roundtrip-match') {
        console.log(`seed ${seed}: MISMATCH interp=${j.interpreter} lifted=${j.lifted} orig=${j.original}`);
        fs.copyFileSync(TMP, path.join(__dirname, `fuzz_fail_${seed}.wasm`));
        fails++;
        mismatches++;
    }
}
fs.rmSync(TMP, { force: true });
console.log(`\n${COUNT - fails}/${COUNT} roundtrip-match (${lifts} lift fails, ${mismatches} mismatches)`);
process.exit(fails ? 1 : 0);
