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
const BINOPSF64 = [0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5]; // add..max
const BINOPSF32 = [0x92, 0x93, 0x94, 0x95, 0x96, 0x97];
const UNOPS32 = [OP.clz, OP.ctz, OP.pop, OP.eqz];
const UNOPS64 = [OP64.clz, OP64.ctz, OP64.pop];
const UNOPSF64 = [0x9F, 0x99, 0x9A];                   // sqrt, abs, neg
const UNOPSF32 = [0x91, 0x8B, 0x8C];

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
let lastMode = 0; // mode of the most recently generated module
function genModule(seed) {
    const rnd = mulberry32(seed);
    const ri = (n) => (rnd() * n) | 0;
    // mode: 0=i32, 1=i64, 2=f64, 3=f32
    const mode = [0, 0, 1, 1, 2, 2, 3][ri(7)];
    lastMode = mode;
    const is64 = mode === 1;
    const isF = mode >= 2;
    const isF64 = mode === 2;
    const WT = [I32, 0x7E, 0x7C, 0x7D][mode];
    const COP = is64 ? OP64.lconst : OP.i32const;
    const LOP = [OP.i32load, OP64.lload, 0x2B, 0x2A][mode];
    const SOP = [OP.i32store, OP64.lstore, 0x39, 0x38][mode];
    const ALN = [2, 3, 3, 2][mode];
    const MASK = [1020, 1016, 1016, 1020][mode];
    const BINOPS = [BINOPS32, BINOPS64, BINOPSF64, BINOPSF32][mode];
    const UNOPS = [UNOPS32, UNOPS64, UNOPSF64, UNOPSF32][mode];
    // one W-typed constant (finite values only)
    const constExpr = () => {
        if (!isF) return [COP, ...s(ri(100))];
        const dv = new DataView(new ArrayBuffer(8));
        if (isF64) { dv.setFloat64(0, rnd() * 200 - 100, true);
                     return [0x44, ...new Uint8Array(dv.buffer)]; }
        dv.setFloat32(0, rnd() * 200 - 100, true);
        return [0x43, ...new Uint8Array(dv.buffer.slice(0, 4))];
    };
    // int result of memory.size/grow converted to W
    const i32ToW = isF ? [isF64 ? 0xB7 : 0xB2] : (is64 ? [0xAC] : []);
    const nfuncs = 1 + ri(3);            // 1..3
    const hasMem = rnd() < 0.6;
    const hasTable = nfuncs > 1 && rnd() < 0.5;
    const hasGlob = rnd() < 0.5;         // mutable global g0 of width W
    const nlocals = ri(4);               // extra locals per func (uniform: fine)

    // addresses are always i32 (wasm), conditions too (via i64.eqz in 64 mode)
    const addrExpr = (d) => [...genExprI32(d), OP.i32const, ...s(MASK), OP.and];
    const condExpr = (d) => is64 ? [...genExpr(d), 0x50 /* i64.eqz */]
                       : isF ? [...genExpr(d), ...genExpr(d),
                                isF64 ? 0x63 /* f64.lt */ : 0x5D /* f32.lt */]
                       : genExpr(d);
    // sub-width loads/stores available in each mode (op, align, mask)
    // value width must match the stack: 32-mode uses i32.* only, 64-mode i64.*
    const SUBLOADS32 = [[0x2C, 0, 1023], [0x2D, 0, 1023], [0x2E, 1, 1022], [0x2F, 1, 1022]];
    const SUBLOADS64 = [[0x30, 0, 1023], [0x31, 0, 1023],
                        [0x32, 1, 1022], [0x33, 1, 1022], [0x34, 2, 1020], [0x35, 2, 1020]];
    const SUBSTORES32 = [[0x3A, 0, 1023], [0x3B, 1, 1022]];
    const SUBSTORES64 = [[0x3C, 0, 1023], [0x3D, 1, 1022], [0x3E, 2, 1020]];
    const SUBL = is64 ? SUBLOADS64 : SUBLOADS32;
    const SUBS = is64 ? SUBSTORES64 : SUBSTORES32;
    function genExprI32(depth) {         // small i32 producer for addresses
        if (depth <= 0) return [OP.i32const, ...s(ri(1024))];
        return [...genExprI32(depth - 1), ...genExprI32(0),
                [OP.add, OP.xor, OP.sub][ri(3)]];
    }

    let curFidx = 0;                     // calls may only target EARLIER funcs
    function genExpr(depth) {            // leaves one W on the stack
        const localIdx = () => ri(2 + nlocals);
        if (depth <= 0) {
            const w = rnd();
            if (w < 0.35) return constExpr();
            if (w < 0.7) return [OP.lget, ...u(localIdx())];
            if (w < 0.8 && hasGlob) return [0x23, 0x00]; // global.get 0
            if (hasMem) return [...addrExpr(1), LOP, ...u(ALN), ...u(0)];
            return constExpr();
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
        if (w < 0.64 && hasMem) { // masked natural load
            return [...addrExpr(depth - 1), LOP, ...u(ALN), ...u(0)];
        }
        if (w < 0.70 && hasMem && !isF) { // masked sub-width load (int modes)
            const [op, al, mk] = SUBL[ri(SUBL.length)];
            return [...genExprI32(depth - 1), OP.i32const, ...s(mk), OP.and,
                    op, ...u(al), ...u(0)];
        }
        if (w < 0.74 && hasMem) { // memory.size (always i32 — convert to W)
            return [0x3F, 0x00, ...i32ToW];
        }
        if (w < 0.78 && hasMem) { // memory.grow by 0..3 pages -> old size / -1
            return [...genExprI32(0), OP.i32const, ...s(3), OP.and, 0x40, 0x00,
                    ...i32ToW];
        }
        if (w < 0.84 && curFidx > 0) { // direct call: strictly earlier funcs
            const f = ri(curFidx);      // (a DAG — recursion would let a
            return [...genExpr(depth - 1), ...genExpr(depth - 1), // memory.grow
                    OP.call, ...u(f)];  //  per frame blow up O(n^2) in V8)
        }
        if (w < 0.90 && hasTable && curFidx > 0) { // indirect: table holds
            return [...genExpr(depth - 1), ...genExpr(depth - 1), // only f0,
                    ...genExprI32(depth - 1),   /* index: always i32 */ // which
                    OP.i32const, ...s(1), OP.remu,                 // never calls
                    OP.calli, ...u(0), 0x00];
        }
        if (w < 0.95) { // if/else producing a value
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
        curFidx = fidx;
        if (hasGlob && rnd() < 0.4) expr.push(...genExpr(1), 0x24, 0x00); // g0 = ...
        if (nlocals > 0 && !isF && rnd() < 0.4) { // bounded countdown loop
            const li = 2 + ri(nlocals);
            expr.push(
                COP, ...s(2 + ri(4)), OP.lset, ...u(li),
                OP.loop, 0x40,
                  ...genExpr(0), OP.drop,
                  OP.lget, ...u(li), COP, ...s(1), (is64 ? OP64.sub : OP.sub),
                  OP.lset, ...u(li),
                  OP.lget, ...u(li),
                  ...(is64 ? [0x50 /* i64.eqz */, OP.eqz] : []),
                  OP.br_if, ...u(0),
                OP.end);
        }
        if (hasMem && rnd() < 0.5) { // a couple of masked stores first
            for (let k = 0; k < 2; k++) {
                if (isF || rnd() < 0.5) {
                    expr.push(...addrExpr(1), ...genExpr(0), SOP, ...u(ALN), ...u(0));
                } else { // sub-width store
                    const [op, al, mk] = SUBS[ri(SUBS.length)];
                    expr.push(...genExprI32(0), OP.i32const, ...s(mk), OP.and,
                              ...genExpr(0), op, ...u(al), ...u(0));
                }
            }
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
        bytes.push(...sec(4, vec([[0x70, 0x00, ...u(1)]])));
    if (hasMem)
        bytes.push(...sec(5, vec([[0x00, ...u(1)]])));
    if (hasGlob) // one mutable global, random init
        bytes.push(...sec(6, vec([[WT, 0x01, ...constExpr(), OP.end]])));
    bytes.push(...sec(7, vec([[...str('main'), 0x00, ...u(0)]])));
    if (hasTable) { // only func 0 (it never calls: curFidx==0 there)
        bytes.push(...sec(9, vec([[0x00, OP.i32const, ...s(0), OP.end,
                                   ...vec([u(0)])]])));
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

const sleep = (ms) => Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
function runSeed(seed) {
    const buf = genModule(seed);
    const tmp = path.join(__dirname, `fuzz_tmp_${seed}.wasm`);
    for (let attempt = 0; attempt < 5; attempt++) {
        try { fs.writeFileSync(tmp, buf); break; }
        catch (e) {                       // AV scanner holds the fresh file
            if (attempt === 4) throw e;
            sleep(50 * (attempt + 1));
        }
    }
    const a = (seed * 7) % 97, b = (seed * 13) % 89;
    // float modules: decimal args so the original gets real values and the
    // lifted side gets correctly-typed cells (ribo reads the wasm types)
    const sa = lastMode >= 2 ? `${a}.5` : String(a);
    const sb = lastMode >= 2 ? `${b}.25` : String(b);
    const r = spawnSync(RIBO, ['roundtrip', tmp, 'main', sa, sb],
                        { encoding: 'utf8', timeout: 15000 });
    fs.rmSync(tmp, { force: true });
    return r;
}

for (let i = 0; i < COUNT; i++) {
    const seed = SEED0 + i;
    let r = runSeed(seed);
    let line = (r.stdout || '').trim().split('\n').pop() || '';
    let j = null;
    try { j = JSON.parse(line); } catch { /* fallthrough */ }
    if (!j && /being used|EPERM|EBUSY/i.test(r.stderr || '')) {
        sleep(200);                        // transient lock flake: retry once
        r = runSeed(seed);
        line = (r.stdout || '').trim().split('\n').pop() || '';
        try { j = JSON.parse(line); } catch { /* fallthrough */ }
    }
    const save = () => fs.writeFileSync(path.join(__dirname, `fuzz_fail_${seed}.wasm`),
                                        genModule(seed));
    if (!j) {
        console.log(`seed ${seed}: CRASH/NO-JSON exit=${r.status} ${(r.stderr || '').trim().slice(0, 200)}`);
        save();
        fails++;
        continue;
    }
    if (j.verdict && j.verdict !== 'roundtrip-match') {
        console.log(`seed ${seed}: MISMATCH interp=${j.interpreter} lifted=${j.lifted} orig=${j.original}`);
        save();
        fails++;
        mismatches++;
    }
}
console.log(`\n${COUNT - fails}/${COUNT} roundtrip-match (${lifts} lift fails, ${mismatches} mismatches)`);
process.exit(fails ? 1 : 0);
