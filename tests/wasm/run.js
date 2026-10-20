// run.js — differential harness: execute an exported function of a wasm
// module and print ONE canonical line for comparison with the interpreter.
//
//   node run.js module.wasm <func> <rettype> [args...]
//
// Output: "ok <canonical>" or "trap <class>".
// Canonical forms (must match src/cli.c exactly):
//   i32: signed decimal      i64: signed decimal (BigInt)
//   f32: 8 hex digits        f64: 16 hex digits (raw bits)
//   void: "void"
// Args arrive as i64 cells (BigInt). Trap classes: div0, divovf, oob,
// trunc, trap, stack.
const fs = require('fs');

const [, , file, fname, rettype, ...argstrs] = process.argv;

function classify(msg) {
    const m = String(msg);
    if (/divide by zero/.test(m)) return 'div0';
    if (/divide result unrepresentable/.test(m)) return 'divovf';
    if (/integer overflow/.test(m)) return 'divovf';
    if (/out of bounds/.test(m)) return 'oob';
    if (/unrepresentable|integer result/.test(m)) return 'trunc';
    if (/call stack|stack/i.test(m)) return 'stack';
    return 'trap';
}

function bits32(x) {
    const b = new DataView(new ArrayBuffer(4));
    b.setFloat32(0, x, true);
    return b.getUint32(0, true).toString(16).padStart(8, '0');
}
function bits64(x) {
    const b = new DataView(new ArrayBuffer(8));
    b.setFloat64(0, x, true);
    return b.getBigUint64(0, true).toString(16).padStart(16, '0');
}

let bytes;
try {
    bytes = fs.readFileSync(file);
} catch (e) {
    console.log('trap io');
    process.exit(0);
}

let result;
try {
    const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {});
    const f = inst.exports[fname];
    if (typeof f !== 'function') {
        console.log('trap noexport');
        process.exit(0);
    }
    // Ribosome-lowered modules take i64 cells (BigInt); original wasm
    // modules may have typed params (i32/f64 need Number). Integers map
    // to BigInt first; if the call rejects BigInt, retry with Number.
    const asBig = argstrs.map(a => /^-?\d+$/.test(a) ? BigInt(a) : Number(a));
    try {
        result = f(...asBig);
    } catch (e) {
        if (e instanceof TypeError) result = f(...argstrs.map(Number));
        else throw e;
    }
} catch (e) {
    console.log('trap ' + classify(e && e.message));
    process.exit(0);
}

let canon;
switch (rettype) {
    case 'i32': canon = String(result | 0); break;        // JS number, signed
    case 'i64': canon = BigInt.asIntN(64, result).toString(); break;
    case 'f32': canon = bits32(result); break;
    case 'f64': canon = bits64(result); break;
    default: canon = 'void'; break;
}
console.log('ok ' + canon);
