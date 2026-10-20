// build_fixtures.js — hand-encodes small "compiler-shaped" wasm MVP modules
// (straight structured code with locals, loops, if/else, memory) to test
// the Ribosome lifter against modules it did NOT emit itself.
// Usage: node tests/fixtures/build_fixtures.js
const fs = require('fs');
const path = require('path');

const u = (v) => { // unsigned LEB128
    const out = [];
    v >>>= 0;
    do { let b = v & 0x7F; v >>>= 7; if (v) b |= 0x80; out.push(b); } while (v);
    return out;
};
const s = (v) => { // signed LEB128 (v is a JS number or BigInt)
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

const I32 = 0x7F, I64 = 0x7E;
const F = {
    func: 0x60,
    block: 0x02, loop: 0x03, if: 0x04, else: 0x05, end: 0x0B,
    br: 0x0C, br_if: 0x0D, ret: 0x0F,
    lget: 0x20, lset: 0x21,
    i32load: 0x28,
    i32const: 0x41, i64const: 0x42,
    i32les: 0x4C, i32geu: 0x4F, i64les: 0x57,
    i32add: 0x6A, i32sub: 0x6B, i32mul: 0x6C, i32divu: 0x6E, i32and: 0x71,
    i64sub: 0x7D, i64mul: 0x7E,
};

function module1({ types, funcs, mem, table, elems, exports, code, data }) {
    const bytes = [0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00];
    bytes.push(...sec(1, vec(types.map(t =>
        [F.func, ...vec(t.params.map(p => [p])), ...vec(t.results.map(r => [r]))]))));
    bytes.push(...sec(3, vec(funcs.map(f => u(f.type)))));
    if (table) bytes.push(...sec(4, vec([[0x70, table.max ? 1 : 0, ...u(table.min), ...(table.max ? u(table.max) : [])]])));
    if (mem) bytes.push(...sec(5, vec([[mem.max ? 1 : 0, ...u(mem.min), ...(mem.max ? u(mem.max) : [])]])));
    bytes.push(...sec(7, vec(exports.map(e => [...str(e.name), e.kind, ...u(e.index)]))));
    if (elems) bytes.push(...sec(9, vec(elems.map(e =>
        [0x00, F.i32const, ...s(e.off), F.end, ...vec(e.funcs.map(f => u(f)))]))));
    bytes.push(...sec(10, vec(code.map(body => {
        const locals = vec(body.locals.map(l => [...u(l.count), l.type]));
        const full = [...locals, ...body.expr, F.end];
        return [...u(full.length), ...full];
    }))));
    if (data) bytes.push(...sec(11, vec(data.map(d =>
        [0x00, F.i32const, ...s(d.off), F.end, ...u(d.bytes.length), ...d.bytes]))));
    return Buffer.from(bytes);
}

const out = (name, buf) => {
    const p = path.join(__dirname, name);
    fs.writeFileSync(p, buf);
    console.log('wrote', p, buf.length, 'bytes');
};

// fact_iter(n): acc = 1; while (n > 1) { acc *= n; n--; } return acc
out('fact_iter.wasm', module1({
    types: [{ params: [I64], results: [I64] }],
    funcs: [{ type: 0 }],
    exports: [{ name: 'fact', kind: 0, index: 0 }],
    code: [{
        locals: [{ count: 1, type: I64 }],
        expr: [
            F.i64const, ...s(1), F.lset, ...u(1),
            F.block, 0x40,
              F.loop, 0x40,
                F.lget, ...u(0), F.i64const, ...s(1), F.i64les, F.br_if, ...u(1),
                F.lget, ...u(1), F.lget, ...u(0), F.i64mul, F.lset, ...u(1),
                F.lget, ...u(0), F.i64const, ...s(1), F.i64sub, F.lset, ...u(0),
                F.br, ...u(0),
              F.end,
            F.end,
            F.lget, ...u(1),
        ],
    }],
}));

// collatz(n): steps to reach 1 (if/else, div_u, and)
out('collatz.wasm', module1({
    types: [{ params: [I32], results: [I32] }],
    funcs: [{ type: 0 }],
    exports: [{ name: 'col', kind: 0, index: 0 }],
    code: [{
        locals: [{ count: 1, type: I32 }],
        expr: [
            F.i32const, ...s(0), F.lset, ...u(1),
            F.block, 0x40,
              F.loop, 0x40,
                F.lget, ...u(0), F.i32const, ...s(1), F.i32les, F.br_if, ...u(1),
                F.lget, ...u(0), F.i32const, ...s(1), F.i32and,
                F.if, 0x40,
                  F.lget, ...u(0), F.i32const, ...s(3), F.i32mul, F.i32const, ...s(1), F.i32add, F.lset, ...u(0),
                F.else,
                  F.lget, ...u(0), F.i32const, ...s(2), F.i32divu, F.lset, ...u(0),
                F.end,
                F.lget, ...u(1), F.i32const, ...s(1), F.i32add, F.lset, ...u(1),
                F.br, ...u(0),
              F.end,
            F.end,
            F.lget, ...u(1),
        ],
    }],
}));

// smask(a, n): a << n — wasm masks the count; the lifted .rna must too.
// smask(1, 33) === 2 in wasm, would trap without the lifter's mask fix.
out('shiftmask.wasm', module1({
    types: [{ params: [I32, I32], results: [I32] }],
    funcs: [{ type: 0 }],
    exports: [{ name: 'smask', kind: 0, index: 0 }],
    code: [{
        locals: [],
        expr: [F.lget, ...u(0), F.lget, ...u(1), 0x74 /* i32.shl */],
    }],
}));

// bits(a, n): rotl(a, n) + popcnt(a) + clz(n) — rotations and bit counting
out('bits.wasm', module1({
    types: [{ params: [I32, I32], results: [I32] }],
    funcs: [{ type: 0 }],
    exports: [{ name: 'bits', kind: 0, index: 0 }],
    code: [{
        locals: [],
        expr: [
            F.lget, ...u(0), F.lget, ...u(1), 0x77 /* i32.rotl */,
            F.lget, ...u(0), 0x69 /* i32.popcnt */, F.i32add,
            F.lget, ...u(1), 0x67 /* i32.clz */, F.i32add,
        ],
    }],
}));

// dispatch(sel, val): table[sel](val) via call_indirect; memrd(): sum of DATA
// at 16 (10+20+30+40 = 100). Table: [inc, dbl, null, null].
out('indirect.wasm', module1({
    types: [
        { params: [I32], results: [I32] },      // 0: unary i32
        { params: [I32, I32], results: [I32] }, // 1: dispatch
        { params: [], results: [I32] },         // 2: memrd
    ],
    funcs: [{ type: 0 }, { type: 0 }, { type: 1 }, { type: 2 }],
    table: { min: 4 },
    mem: { min: 1 },
    elems: [{ off: 0, funcs: [0, 1] }],
    exports: [
        { name: 'inc', kind: 0, index: 0 },
        { name: 'dbl', kind: 0, index: 1 },
        { name: 'dispatch', kind: 0, index: 2 },
        { name: 'memrd', kind: 0, index: 3 },
    ],
    code: [
        { locals: [], expr: [F.lget, ...u(0), F.i32const, ...s(1), F.i32add] },
        { locals: [], expr: [F.lget, ...u(0), F.i32const, ...s(2), F.i32mul] },
        { locals: [], expr: [
            F.lget, ...u(1),               /* value */
            F.lget, ...u(0),               /* selector (index on top) */
            0x11, ...u(0), 0x00,           /* call_indirect type0 table0 */
        ] },
        { locals: [], expr: [
            F.i32const, ...s(16), F.i32load, ...u(2), ...u(0),
            F.i32const, ...s(20), F.i32load, ...u(2), ...u(0), F.i32add,
            F.i32const, ...s(24), F.i32load, ...u(2), ...u(0), F.i32add,
            F.i32const, ...s(28), F.i32load, ...u(2), ...u(0), F.i32add,
        ] },
    ],
    data: [{ off: 16, bytes: [0x0A,0,0,0, 0x14,0,0,0, 0x1E,0,0,0, 0x28,0,0,0] }],
}));

// sum(n): acc = 0; for (i = 0; i <u n; i++) acc += mem_i32[i*4]; return acc
out('sum_arr.wasm', module1({
    types: [{ params: [I32], results: [I32] }],
    funcs: [{ type: 0 }],
    mem: { min: 1 },
    exports: [{ name: 'sum', kind: 0, index: 0 }],
    code: [{
        locals: [{ count: 2, type: I32 }],
        expr: [
            F.block, 0x40,
              F.loop, 0x40,
                F.lget, ...u(1), F.lget, ...u(0), F.i32geu, F.br_if, ...u(1),
                F.lget, ...u(2),
                F.lget, ...u(1), F.i32const, ...s(2), 0x74 /* i32.shl */,
                F.i32load, ...u(2), ...u(0) /* align, offset */,
                F.i32add, F.lset, ...u(2),
                F.lget, ...u(1), F.i32const, ...s(1), F.i32add, F.lset, ...u(1),
                F.br, ...u(0),
              F.end,
            F.end,
            F.lget, ...u(2),
        ],
    }],
}));
