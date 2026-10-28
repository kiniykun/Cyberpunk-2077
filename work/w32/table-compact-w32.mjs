// [w32-tablecompact] Renumber the funcref table so the functions that actually run (desktop profile, by name) occupy
// the low indices, and shrink the table's INITIAL size to that hot range. With the lazy table (lazy-table-w32.mjs,
// grow on demand), each JSC instance then allocates a table of ~hot size instead of 275k slots.
// Needs the link output's relocations (wasm-ld --emit-relocs): every TABLE_INDEX relocation in code and data is
// rewritten to the new index (padded LEB / i32 in place). The linking/reloc custom sections are removed afterwards.
// usage: node table-compact-w32.mjs in.wasm out.wasm hot-names.txt
import fs from 'node:fs';
const [src, dst, hotFile] = process.argv.slice(2);
const b = Buffer.from(fs.readFileSync(src));
let p = 0;
const u32 = () => { let r = 0, s = 0, x; do { x = b[p++]; r += (x & 127) * 2 ** s; s += 7; } while (x & 128); return r; };
const s32 = () => { let r = 0, s = 0, x; do { x = b[p++]; r |= (x & 127) << s; s += 7; } while (x & 128); if (s < 32 && (x & 64)) r |= -1 << s; return r; };
const str = () => { const l = u32(); const v = b.toString('utf8', p, p + l); p += l; return v; };
const enc = x => { const o = []; do { let c = x & 127; x = Math.floor(x / 128); o.push(x ? c | 128 : c); } while (x); return o; };
const sections = []; p = 8;
while (p < b.length) { const id = b[p++]; const n = u32(); const start = p; let name = null; if (id === 0) { name = str(); } sections.push({ id, start, end: start + n, name, payload: p }); p = start + n; }
const sec = id => sections.find(s => s.id === id);
const custom = name => sections.find(s => s.id === 0 && s.name === name);
// imports -> function import count
let nimp = 0; { const s = sec(2); p = s.start; const n = u32();
  for (let i = 0; i < n; i++) { str(); str(); const k = b[p++];
    if (k === 0) { u32(); nimp++; } else if (k === 1) { p++; const fl = u32(); u32(); if (fl & 1) u32(); }
    else if (k === 2) { const fl = u32(); u32(); if (fl & 1) u32(); } else if (k === 3) p += 2; else throw new Error('import kind ' + k); } }
// element segment (single active, flag 0, i32.const OFFSET)
const es = sec(9); p = es.start; if (u32() !== 1) throw new Error('elem count'); if (u32() !== 0) throw new Error('elem flag');
if (b[p++] !== 0x41) throw new Error('elem offset'); const OFFSET = s32(); if (b[p++] !== 0x0b) throw new Error('elem end');
const nElem = u32(); const elemFuncs = []; for (let i = 0; i < nElem; i++) elemFuncs.push(u32());
const tableIndexOfFunc = new Map(); elemFuncs.forEach((f, i) => { if (!tableIndexOfFunc.has(f)) tableIndexOfFunc.set(f, OFFSET + i); });
// function names (name section) for the hot mapping
const names = new Map(); { const s = custom('name'); if (!s) throw new Error('no name section'); p = s.payload;
  while (p < s.end) { const sub = b[p++]; const n = u32(); const e = p + n;
    if (sub === 1) { const c = u32(); for (let i = 0; i < c; i++) { const idx = u32(); names.set(idx, str()); } }
    p = e; } }
const hotNames = new Set(fs.readFileSync(hotFile, 'utf8').split(/\r?\n/).filter(Boolean));
// new order: hot table entries first (original relative order), then the rest
const order = [...elemFuncs.keys()];
const isHot = i => hotNames.has(names.get(elemFuncs[i]));
const hotIdx = order.filter(isHot), coldIdx = order.filter(i => !isHot(i));
const newPos = new Int32Array(nElem); [...hotIdx, ...coldIdx].forEach((oldI, newI) => { newPos[oldI] = newI; });
const remapIndex = old => { const i = old - OFFSET; if (i < 0 || i >= nElem) throw new Error('table index out of range ' + old); return OFFSET + newPos[i]; };
// symbols (linking section)
const syms = []; { const s = custom('linking'); p = s.payload; const ver = u32(); if (ver !== 2) throw new Error('linking version ' + ver);
  while (p < s.end) { const sub = b[p++]; const n = u32(); const e = p + n;
    if (sub === 8) { const c = u32(); for (let i = 0; i < c; i++) { const kind = b[p++]; const flags = u32(); const undef = flags & 0x10, explicit = flags & 0x40;
      if (kind === 0 || kind === 2 || kind === 4 || kind === 5) { const idx = u32(); if (!undef || explicit) str(); syms.push({ kind, idx }); }
      else if (kind === 1) { str(); if (!undef) { u32(); u32(); u32(); } syms.push({ kind }); }
      else if (kind === 3) { u32(); syms.push({ kind }); }
      else throw new Error('symbol kind ' + kind); } }
    p = e; } }
// apply TABLE_INDEX relocations in CODE and DATA
const tableRelocTypes = new Set([1, 2]);   // R_WASM_TABLE_INDEX_SLEB, R_WASM_TABLE_INDEX_I32
const addendTypes = new Set([3, 4, 5, 8, 9, 11, 14, 15, 16, 17, 21, 22, 23, 25, 26]);  // memory addr / offsets
let patched = 0, seen = 0, skipped = 0;
for (const name of ['reloc.CODE', 'reloc.DATA']) {
  const s = custom(name); if (!s) throw new Error('missing ' + name); p = s.payload;
  const target = u32(); const tsec = sections[target]; const count = u32();
  for (let i = 0; i < count; i++) {
    const type = u32(), off = u32(), index = u32(); if (addendTypes.has(type)) s32();
    if (!tableRelocTypes.has(type)) { if (type === 12 || type === 18 || type === 19 || type === 24) throw new Error('unsupported table reloc ' + type); continue; }
    seen++;
    const sym = syms[index]; if (!sym || sym.kind !== 0) throw new Error('table reloc to non-function symbol');
    const old = tableIndexOfFunc.get(sym.idx);
    if (old === undefined) {   // a table reloc whose function is not in the table: the linker wrote 0 (or keeps a value); verify and skip
      const at0 = tsec.start + off; let cur; if (type === 1) { const save = p; p = at0; cur = s32(); p = save; } else cur = b.readInt32LE(at0);
      if (cur >= OFFSET && cur < OFFSET + nElem) throw new Error(`function ${sym.idx} not in table but site holds ${cur}`);
      skipped = (skipped || 0) + 1; continue;
    }
    const nw = remapIndex(old); const at = tsec.start + off;
    if (type === 1) { const save = p; p = at; const cur = s32(); p = save; if (cur !== old) throw new Error(`sleb mismatch ${cur} vs ${old}`);
      let v = nw; for (let k = 0; k < 5; k++) { const c = v & 127; v >>= 7; b[at + k] = k < 4 ? c | 128 : c; } }
    else { const cur = b.readInt32LE(at); if (cur !== old) throw new Error(`i32 mismatch ${cur} vs ${old}`); b.writeInt32LE(nw, at); }
    patched++;
  }
}
// rewrite element list in the new order, table limits: min = hot end, max = full
const newElem = [...hotIdx, ...coldIdx].map(i => elemFuncs[i]);
const hotEnd = OFFSET + hotIdx.length;
const out = [Buffer.from(b.subarray(0, 8))];
const emit = (id, payload) => out.push(Buffer.from([id, ...enc(payload.length)]), Buffer.from(payload));
for (const s of sections) {
  if (s.id === 0 && (s.name === 'linking' || s.name.startsWith('reloc.') || s.name === '.objcopy.removed')) continue;
  if (s.id === 9) { const body = [1, 0, 0x41, ...sleb(OFFSET), 0x0b, ...enc(newElem.length)]; for (const f of newElem) body.push(...enc(f)); emit(9, body); continue; }
  out.push(Buffer.from([s.id, ...enc(s.end - s.start)]), b.subarray(s.start, s.end));
}
function sleb(x) { const o = []; let more = true; while (more) { let c = x & 127; x >>= 7; if ((x === 0 && !(c & 64)) || (x === -1 && (c & 64))) more = false; else c |= 128; o.push(c); } return o; }
const result = Buffer.concat(out); fs.writeFileSync(dst, result);
fs.writeFileSync(dst + '.hotend', String(hotEnd));   // read by lazy-table-w32.mjs (table initial size)
console.log(`table ${nElem} entries (offset ${OFFSET}); hot ${hotIdx.length} -> initial size ${hotEnd}; table relocs ${seen}, patched ${patched}, skipped ${skipped}; ${(b.length / 1048576).toFixed(1)} -> ${(result.length / 1048576).toFixed(1)} MiB`);
