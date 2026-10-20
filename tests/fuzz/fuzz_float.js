// fuzz_float.js — random .rna f32/f64 arithmetic chains, `ribo verify` each.
// Finite inputs only: NaN payloads are spec-nondeterministic across engines.
// Usage: node tests/fuzz/fuzz_float.js [count] [seed0]
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const RIBO = path.join(__dirname, '..', '..', 'build', 'ribo.exe');
const TMP = path.join(__dirname, 'fuzz_float_tmp.rna');

function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

const COUNT = Number(process.argv[2] || 200);
const SEED0 = Number(process.argv[3] || 1);
let fails = 0;

for (let i = 0; i < COUNT; i++) {
    const rnd = mulberry32(SEED0 + i);
    const ri = (n) => (rnd() * n) | 0;
    const is64 = rnd() < 0.5;
    const P = is64 ? 'D' : 'F';
    const steps = 2 + ri(5);
    const lines = [`EXPORT f _`, `FUNC f ${is64 ? 'f64' : 'f32'}/0`, `${P}LIT r0 ${(rnd() * 200 - 100).toPrecision(10)}`];
    for (let k = 0; k < steps; k++) {
        const w = rnd();
        if (w < 0.5) {
            lines.push(`${P}LIT r1 ${(rnd() * 200 - 100).toPrecision(10)}`);
            lines.push(`${P}${['ADD', 'SUB', 'MUL', 'DIV', 'MIN', 'MAX'][ri(6)]} r0 r1`);
        } else if (w < 0.7) {
            lines.push(`${P}${['SQRT', 'ABS', 'NEG'][ri(3)]} r0 _`); // SQRT of <0 -> NaN, still deterministic per engine pair
        } else if (w < 0.85 && is64) {
            lines.push(`D2F r0 _`, `F2D r0 _`); // narrowing round-trip
        } else if (!is64) {
            lines.push(`F2D r0 _`, `D2F r0 _`);
        }
    }
    lines.push(`RET r0 _`, `END _ _`);
    fs.writeFileSync(TMP, lines.join('\n'));
    const r = spawnSync(RIBO, ['verify', TMP, 'f'], { encoding: 'utf8', timeout: 15000 });
    const line = (r.stdout || '').trim().split('\n').pop() || '';
    let j = null;
    try { j = JSON.parse(line); } catch { /* fallthrough */ }
    if (!j || !j.ok) {
        console.log(`seed ${SEED0 + i}: ${line || (r.stderr || '').slice(0, 150)}`);
        fs.copyFileSync(TMP, path.join(__dirname, `fuzz_float_fail_${SEED0 + i}.rna`));
        fails++;
    }
}
fs.rmSync(TMP, { force: true });
console.log(`\n${COUNT - fails}/${COUNT} float verify-match`);
process.exit(fails ? 1 : 0);
