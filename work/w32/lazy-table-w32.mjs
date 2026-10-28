// [w32-lazytable] Lazy funcref table for JSC (iPhone): Safari materialises one function wrapper per table entry for
// EVERY WebAssembly.Instance (one per pthread); the engine table has ~275k entries, so each extra thread instance is
// expensive and the phone dies at ~5 instances. This transform makes the table start EMPTY and fills an entry on
// first use, per instance:
//   * the active element segment becomes a PASSIVE segment with the same function list;
//   * every `call_indirect T 0` becomes `call $ci_T`, a helper that does
//       if (table.get(idx) == null) table.init(passive, dst=idx, src=idx-OFFSET, len=1); call_indirect T 0 (idx)
//     (out-of-range or never-populated indices still trap, as before);
//   * an exported `__cp_table_fill(i32)` does the same fill for JS readers (getWasmTableEntry patched in the glue).
// Function, type and table indices of the original module are unchanged (helpers/types are appended).
// usage: node lazy-table-w32.mjs in.wasm out.wasm
import fs from 'node:fs';
const [src, dst, initialArg] = process.argv.slice(2);   // optional: table initial size (grow on demand above it)
const INITIAL = initialArg ? Number(initialArg) : 0;
const b = fs.readFileSync(src);
let p = 0;
const u32 = () => { let r = 0, s = 0, x; do { x = b[p++]; r += (x & 127) * 2 ** s; s += 7; } while (x & 128); return r; };
const s33 = () => { let r = 0n, s = 0n, x; do { x = b[p++]; r |= BigInt(x & 127) << s; s += 7n; } while (x & 128); return r; };
const enc = x => { const o = []; do { let c = x & 127; x = Math.floor(x / 128); o.push(x ? c | 128 : c); } while (x); return o; };
const sections = []; p = 8;
while (p < b.length) { const id = b[p++]; const n = u32(); sections.push({ id, start: p, end: p + n }); p += n; }
const sec = id => sections.find(s => s.id === id);
// ---- types
const types = []; { const s = sec(1); p = s.start; const n = u32();
  for (let i = 0; i < n; i++) { const form = b[p++]; if (form !== 0x60) throw new Error('non-func type ' + form);
    const np = u32(); const params = [...b.subarray(p, p + np)]; p += np; const nr = u32(); const results = [...b.subarray(p, p + nr)]; p += nr;
    types.push({ params, results }); } }
// ---- imports (function count)
let nimp = 0; { const s = sec(2); p = s.start; const n = u32();
  for (let i = 0; i < n; i++) { let l = u32(); p += l; l = u32(); p += l; const k = b[p++];
    if (k === 0) { u32(); nimp++; } else if (k === 1) { p++; const fl = u32(); u32(); if (fl & 1) u32(); }
    else if (k === 2) { const fl = u32(); u32(); if (fl & 1) u32(); } else if (k === 3) { p += 2; } else throw new Error('import kind ' + k); } }
let nfun; { const s = sec(3); p = s.start; nfun = u32(); }
// ---- element section: one active segment, flag 0, i32.const OFFSET
let OFFSET, elemFuncs; { const s = sec(9); p = s.start; const n = u32(); if (n !== 1) throw new Error('elem segments ' + n);
  const fl = u32(); if (fl !== 0) throw new Error('elem flag ' + fl);
  if (b[p++] !== 0x41) throw new Error('elem offset not i32.const'); OFFSET = Number(s33()); if (b[p++] !== 0x0b) throw new Error('elem offset end');
  const k = u32(); elemFuncs = []; for (let i = 0; i < k; i++) elemFuncs.push(u32()); if (p !== s.end) throw new Error('elem trailing'); }
// ---- code rewrite
const helperOf = new Map();   // typeidx -> helper slot
const helpers = [];           // {type}
const helperFor = t => { if (!helperOf.has(t)) { helperOf.set(t, helpers.length); helpers.push({ type: t }); } return nimp + nfun + 1 + helperOf.get(t); };
// function index nimp+nfun = __cp_table_fill; helpers follow
const FILL = nimp + nfun;
const code = sec(10); p = code.start; const nbodies = u32(); if (nbodies !== nfun) throw new Error('body count');
const outBodies = []; let replaced = 0;
const memarg = () => { const a = u32(); if (a & 64) u32(); u32(); };
for (let f = 0; f < nbodies; f++) {
  const pre = p; const size = u32(); const bstart = p, bend = p + size;
  const nl = u32(); for (let i = 0; i < nl; i++) { u32(); const t = b[p++]; if (t === 0x63 || t === 0x64) s33(); }
  const chunks = []; let last = bstart;
  while (p < bend) {
    const op = b[p++];
    switch (op) {
      case 0x00: case 0x01: case 0x05: case 0x0b: case 0x0f: case 0x1a: case 0x1b: case 0xd1: break;
      case 0x02: case 0x03: case 0x04: { const t = b[p]; if (t === 0x40 || (t >= 0x6f && t <= 0x7f) || t === 0x63 || t === 0x64) { p++; if (t === 0x63 || t === 0x64) s33(); } else s33(); break; }
      case 0x0c: case 0x0d: u32(); break;
      case 0x0e: { const n = u32(); for (let i = 0; i <= n; i++) u32(); break; }
      case 0x10: case 0x12: u32(); break;
      case 0x11: { const at = p - 1; const t = u32(); const tbl = u32();
        if (tbl === 0) { chunks.push(b.subarray(last, at), Buffer.from([0x10, ...enc(helperFor(t))])); last = p; replaced++; }
        break; }
      case 0x13: u32(); u32(); break;
      case 0x1c: { const n = u32(); for (let i = 0; i < n; i++) { const t = b[p++]; if (t === 0x63 || t === 0x64) s33(); } break; }
      case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: u32(); break;
      case 0x3f: case 0x40: u32(); break;
      case 0x41: case 0x42: s33(); break;
      case 0x43: p += 4; break;
      case 0x44: p += 8; break;
      case 0xd0: { const t = b[p]; if (t >= 0x69 && t <= 0x74) p++; else s33(); break; }
      case 0xd2: u32(); break;
      case 0xfc: { const sub = u32();
        if (sub <= 7) break;
        if (sub === 8) { u32(); u32(); } else if (sub === 9) u32(); else if (sub === 10) { u32(); u32(); } else if (sub === 11) u32();
        else if (sub === 12) { u32(); u32(); } else if (sub === 13) u32(); else if (sub === 14) { u32(); u32(); }
        else if (sub >= 15 && sub <= 17) u32(); else throw new Error(`fc ${sub} in f${f}`);
        break; }
      case 0xfd: { const sub = u32();
        if (sub <= 11 || sub === 92 || sub === 93) memarg();
        else if (sub === 12 || sub === 13) p += 16;
        else if (sub >= 21 && sub <= 34) p += 1;
        else if (sub >= 84 && sub <= 91) { memarg(); p += 1; }
        break; }
      case 0xfe: { const sub = u32(); if (sub === 3) p += 1; else memarg(); break; }
      default:
        if (op >= 0x28 && op <= 0x3e) { memarg(); break; }
        if (op >= 0x45 && op <= 0xc4) break;
        throw new Error(`opcode 0x${op.toString(16)} at ${p - 1} in function ${f}`);
    }
  }
  if (p !== bend) throw new Error('body overrun ' + f);
  if (!chunks.length) outBodies.push(b.subarray(pre, bend));   // unchanged (original size prefix included)
  else { chunks.push(b.subarray(last, bend)); const body = Buffer.concat([b.subarray(bstart, bstart), ...chunks]);
    outBodies.push(Buffer.concat([Buffer.from(enc(body.length)), body])); }
}
// ---- new types: helper type = params(T)+i32 -> results(T); fill type = (i32)->()
const typeKey = t => t.params.join(',') + '>' + t.results.join(',');
const typeIndex = new Map(types.map((t, i) => [typeKey(t), i]));
const newTypes = [];
const getType = t => { const k = typeKey(t); if (typeIndex.has(k)) return typeIndex.get(k); const i = types.length + newTypes.length; newTypes.push(t); typeIndex.set(k, i); return i; };
const fillType = getType({ params: [0x7f], results: [] });
const helperTypes = helpers.map(h => getType({ params: [...types[h.type].params, 0x7f], results: types[h.type].results }));
const PASSIVE = 0;  // the only element segment, now passive
// grow the table (per instance) when idx is beyond its current size, then fill from the passive segment if null
const growCode = idxLocal => INITIAL ? [0x20, ...enc(idxLocal), 0xfc, 0x10, 0x00, 0x4f, 0x04, 0x40,
  0xd0, 0x70, 0x20, ...enc(idxLocal), 0x41, 0x01, 0x6a, 0xfc, 0x10, 0x00, 0x6b, 0xfc, 0x0f, 0x00, 0x1a, 0x0b] : [];
const fillCode = idxLocal => [...growCode(idxLocal), 0x20, ...enc(idxLocal), 0x25, 0x00, 0xd1, 0x04, 0x40,
  0x20, ...enc(idxLocal), 0x20, ...enc(idxLocal), 0x41, ...sleb(OFFSET), 0x6b, 0x41, 0x01, 0xfc, 0x0c, ...enc(PASSIVE), 0x00, 0x0b];
function sleb(x) { const o = []; let more = true; while (more) { let c = x & 127; x >>= 7; if ((x === 0 && !(c & 64)) || (x === -1 && (c & 64))) more = false; else c |= 128; o.push(c); } return o; }
const newBodies = [];
newBodies.push([0x00, ...fillCode(0), 0x0b]);                        // __cp_table_fill(idx)
helpers.forEach((h, i) => { const t = types[h.type]; const n = t.params.length;
  const body = [0x00, ...fillCode(n)]; for (let k = 0; k < n; k++) body.push(0x20, ...enc(k));
  body.push(0x20, ...enc(n), 0x11, ...enc(h.type), 0x00, 0x0b); newBodies.push(body); });
// ---- assemble
const out = [Buffer.from(b.subarray(0, 8))];
const emit = (id, payload) => out.push(Buffer.from([id, ...enc(payload.length)]), payload);
for (const s of sections) {
  const raw = b.subarray(s.start, s.end);
  if (s.id === 1) { p = s.start; const n = u32(); const rest = b.subarray(p, s.end);
    const add = []; for (const t of newTypes) add.push(0x60, ...enc(t.params.length), ...t.params, ...enc(t.results.length), ...t.results);
    emit(1, Buffer.concat([Buffer.from(enc(n + newTypes.length)), rest, Buffer.from(add)])); }
  else if (s.id === 3) { p = s.start; const n = u32(); const rest = b.subarray(p, s.end);
    emit(3, Buffer.concat([Buffer.from(enc(n + 1 + helpers.length)), rest, Buffer.from([...enc(fillType), ...helperTypes.flatMap(t => enc(t))])])); }
  else if (s.id === 7) { p = s.start; const n = u32(); const rest = b.subarray(p, s.end); const name = Buffer.from('__cp_table_fill');
    emit(7, Buffer.concat([Buffer.from(enc(n + 1)), rest, Buffer.from([...enc(name.length)]), name, Buffer.from([0x00, ...enc(FILL)])])); }
  else if (s.id === 4 && INITIAL) { p = s.start; const n = u32(); if (n !== 1) throw new Error('tables ' + n); const t = b[p++]; const fl = u32(); const min = u32(); const max = fl & 1 ? u32() : min;
    emit(4, Buffer.from([1, t, 1, ...enc(Math.min(INITIAL, min)), ...enc(Math.max(min, max, OFFSET + elemFuncs.length))])); }
  else if (s.id === 9) { const seg = [1, ...enc(1), 0x00, ...enc(elemFuncs.length)]; const list = []; for (const f of elemFuncs) list.push(...enc(f));
    emit(9, Buffer.from([...seg, ...list])); }
  else if (s.id === 10) { const parts = [Buffer.from(enc(nfun + 1 + helpers.length))];
    for (const body of outBodies) parts.push(body);
    for (const nb of newBodies) parts.push(Buffer.from([...enc(nb.length), ...nb]));
    emit(10, Buffer.concat(parts)); }
  else emit(s.id, raw);
}
const result = Buffer.concat(out);
fs.writeFileSync(dst, result);
console.log(`elem entries ${elemFuncs.length} (offset ${OFFSET}) -> passive; call_indirect rewritten ${replaced} via ${helpers.length} helpers; new types ${newTypes.length}; ${(b.length / 1048576).toFixed(1)} -> ${(result.length / 1048576).toFixed(1)} MiB`);
