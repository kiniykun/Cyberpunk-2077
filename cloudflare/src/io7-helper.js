// [io7] phases from the main thread: warm lanes capped at bootLanes until the engine is Running; the later lists start
// at the main menu (or after laterDelayMs). List specs "a|b" warm the first list that exists.
// [io6] warm lanes yield to game reads (ioWarmBusyLanes while sync misses are in flight), 4-block chunks,
// lifepath detection from first demand of lifepath-exclusive blocks (runtime/warm/lifepaths.json), warm names deduped.
// [io5] warm on request ({cmd:'warm'}), byte-bounded persist queue while the store opens.
// [io4] IO helper worker (dedicated worker, idle event loop; the busy pthreads cannot await).
//  - async adaptive prefetch from the shared queue (io3 rules: budget, per-file auto-off)
//  - persistent block store in OPFS: append-only pack files + JSON index, keyed by path + ETag + block;
//    presence published in the shared table so pthreads ask for a local fill instead of a network read
//  - "load" / "persist" requests from every pthread over its own MessagePort
//  - warm lists (/runtime/warm/<name>.json): parallel async download of a recorded hot block set into the store
// Needs io4-shared-cache.js prepended (make-io-bundle4.mjs does that).
let C = null, files = null, inflight = 0, cap = 4, trace = false, pct = 10, store = null, storeWaiters = [], waiterBytes = 0, warmWidth = 16;
let readyResolve; const ready = new Promise(r => readyResolve = r);
const WAITER_MAX = 512 * 1048576;
let warmActive = 0, busyLanes = 0, warmChunk = 4, bootLanes = 4, running = false, laterSpecs = [], laterStarted = false;
const phases = new Set();
function startLater(why) {
  if (laterStarted) return;
  laterStarted = true;
  log("later warm lists start (" + why + "): " + laterSpecs.join(","));
  ready.then(() => warm(laterSpecs, warmWidth));
}
const activePlans = new Set();
const warmedNames = new Set();
const sleep = ms => new Promise(r => setTimeout(r, ms));
// lifepath detector: block "fi|n" -> lifepath name (blocks recorded for exactly one lifepath)
let detect = null;   // {map, counts, seen, K, done}
function noteDemandForDetect(fi, n) {
  const D = detect;
  if (!D || D.done) return;
  const key = fi + "|" + n;
  if (D.seen.has(key)) return;
  D.seen.add(key);
  const lp = D.map.get(key);
  if (!lp) return;
  const c = D.counts[lp] = (D.counts[lp] || 0) + 1;
  let second = 0;
  for (const [name, v] of Object.entries(D.counts)) if (name !== lp && v > second) second = v;
  if (c >= D.K && c >= 2 * second) {
    D.done = true;
    log("lifepath detected from reads: " + lp + " " + JSON.stringify(D.counts));
    self.postMessage({cmd: "lifepath", name: lp});
    warm([lp], warmWidth);
  }
}
async function loadDetector(K) {
  try {
    const res = await fetch("/runtime/warm/lifepaths.json", {cache: "no-store"});
    if (!res.ok) { log("lifepath detector off (no lifepaths.json: " + res.status + ")"); return; }
    const j = await res.json();
    if (!j || j.version !== 1 || j.blockBytes !== C.B) { log("lifepath detector off (incompatible lifepaths.json)"); return; }
    const fileIndex = new Map(files.map((f, i) => [f.path, i])), map = new Map();
    for (const [name, lp] of Object.entries(j.lifepaths || {})) {
      if (!/^[A-Za-z0-9_-]{1,40}$/.test(name)) continue;
      for (const [path, first, count] of lp.exclusive || []) {
        const fi = fileIndex.get(path);
        if (fi === undefined || (j.etags && j.etags[path] && j.etags[path] !== files[fi].etag)) continue;
        for (let n = first; n < first + count; ++n) map.set(fi + "|" + n, name);
      }
    }
    detect = {map, counts: {}, seen: new Set(), K, done: false};
    log("lifepath detector on: " + map.size + " exclusive blocks, K=" + K);
  } catch (e) { log("lifepath detector failed: " + e); }
}
const FREE_KB = 4096, PACK_MAX = 256 * 1048576, STORE_DIR = "cp2077-blocks-v1";
const pending = [];
const T = {requests: 0, bytes: 0, ms: 0, maxMs: 0, blocks: new Map, last: 0};
const log = m => { try { console.log("CP2077 io helper: " + m); } catch (e) {} };

function flushTrace(force) {
  const now = performance.now();
  if (!trace || (!force && now - T.last < 5000) || !T.requests) return;
  T.last = now;
  const blocks = {};
  for (const [p, s] of T.blocks) blocks[p] = {n: [...s], r: s.r || 0, b: s.b || 0};
  self.postMessage({cmd: "trace", d: {requests: T.requests, bytes: T.bytes, ms: T.ms, maxMs: T.maxMs, hits: 0, reads: 0, readMs: 0, cfg: null, blocks}});
  T.requests = T.bytes = T.ms = T.maxMs = 0; T.blocks = new Map;
}
function noteFetch(f, start, end, bytes, began) {
  if (!trace) return;
  const ms = performance.now() - began;
  ++T.requests; T.bytes += bytes; T.ms += ms; if (ms > T.maxMs) T.maxMs = ms;
  let s = T.blocks.get(f.path); if (!s) T.blocks.set(f.path, s = new Set);
  s.r = (s.r || 0) + 1; s.b = (s.b || 0) + bytes;
  for (let n = Math.floor(start / 1048576); n <= Math.floor(end / 1048576); ++n) s.add(n);
}
async function fetchRange(f, start, end) {
  const began = performance.now();
  const res = await fetch(f.path.split("/").map(encodeURIComponent).join("/"), {headers: {Range: `bytes=${start}-${end}`, "If-Match": f.etag}, cache: "no-store"});
  const bytes = new Uint8Array(await res.arrayBuffer());
  if (res.status !== 206 || res.headers.get("ETag") !== f.etag || res.headers.get("Content-Range") !== `bytes ${start}-${end}/${f.size}` || bytes.length !== end - start + 1)
    throw new Error("range identity mismatch");
  noteFetch(f, start, end, bytes.length, began);
  return bytes;
}

// ------------------------------------------------------------------ persistent store (OPFS)
async function openStore(budgetMB) {
  if (!self.navigator || !navigator.storage || !navigator.storage.getDirectory) throw new Error("OPFS unavailable");
  const root = await navigator.storage.getDirectory();
  const dir = await root.getDirectoryHandle(STORE_DIR, {create: true});
  const S = {dir, packs: new Map, entries: new Map, total: 0, budget: 0, cur: -1, seq: 0, dirty: false, idx: [], writing: Promise.resolve(), lastFlush: performance.now()};
  // index: two alternating files, highest valid seq wins (a torn write leaves the other one intact)
  let best = null;
  for (let i = 0; i < 2; ++i) {
    const fh = await dir.getFileHandle("index-" + i + ".json", {create: true});
    const h = await fh.createSyncAccessHandle();
    S.idx[i] = h;
    try {
      const n = h.getSize();
      if (n > 0) { const buf = new Uint8Array(n); h.read(buf, {at: 0}); const j = JSON.parse(new TextDecoder().decode(buf)); if (j && j.v === 1 && (!best || j.seq > best.seq)) best = j; }
    } catch (e) {}
  }
  const keep = new Set;
  if (best && best.blockBytes === C.B) {
    S.seq = best.seq;
    for (const p of best.packs) {
      try {
        const fh = await dir.getFileHandle("pack-" + p.id + ".bin");
        const h = await fh.createSyncAccessHandle();
        S.packs.set(p.id, {h, size: h.getSize()});
        keep.add("pack-" + p.id + ".bin");
      } catch (e) {}
    }
    for (const [path, etag, n, pack, at, len] of best.entries) {
      const pk = S.packs.get(pack);
      if (!pk || at + len > pk.size) continue;
      S.entries.set(path + "|" + etag + "|" + n, {pack, at, len});
    }
  }
  // remove packs the index does not know (crash between append and index flush) — they are unreferenced bytes
  try { for await (const name of dir.keys()) if (/^pack-\d+\.bin$/.test(name) && !keep.has(name)) await dir.removeEntry(name); } catch (e) {}
  for (const [id, pk] of S.packs) { S.total += pk.size; if (id > S.cur) S.cur = id; }
  S.nextId = S.cur + 1;
  S.cur = -1;                                 // always start a fresh pack for this session's writes
  let avail = Infinity;
  try { const est = await navigator.storage.estimate(); if (est && est.quota) avail = Math.max(0, est.quota - (est.usage || 0)) + S.total; } catch (e) {}
  S.budget = Math.max(64 * 1048576, Math.min(budgetMB * 1048576, Math.floor(avail * 0.6)));
  // publish presence for entries whose path + ETag match this manifest
  const fileIndex = new Map(files.map((f, i) => [f.path + "|" + f.etag, i]));
  for (const key of S.entries.keys()) {
    const cut = key.lastIndexOf("|"), fi = fileIndex.get(key.slice(0, cut));
    if (fi !== undefined) sbcPtabAdd(C, sbcKey(fi, Number(key.slice(cut + 1))));
  }
  return S;
}
function storeKey(fi, n) { const f = files[fi]; return f.path + "|" + f.etag + "|" + n; }
function storeRead(fi, n) {
  const e = store && store.entries.get(storeKey(fi, n));
  if (!e) return null;
  const pk = store.packs.get(e.pack);
  if (!pk) return null;
  const buf = new Uint8Array(e.len);
  return pk.h.read(buf, {at: e.at}) === e.len ? buf : null;
}
async function storeEnsurePack() {
  const S = store;
  let pk = S.packs.get(S.cur);
  if (pk && pk.size < PACK_MAX) return pk;
  const id = S.nextId++;
  const fh = await S.dir.getFileHandle("pack-" + id + ".bin", {create: true});
  const h = await fh.createSyncAccessHandle();
  h.truncate(0);
  pk = {h, size: 0};
  S.packs.set(id, pk); S.cur = id;
  return pk;
}
async function storeEvict() {
  const S = store;
  while (S.total > S.budget && S.packs.size > 1) {
    let oldest = Infinity;
    for (const id of S.packs.keys()) if (id !== S.cur && id < oldest) oldest = id;
    if (oldest === Infinity) return;
    const pk = S.packs.get(oldest);
    const fileIndex = new Map(files.map((f, i) => [f.path + "|" + f.etag, i]));
    for (const [key, e] of S.entries) if (e.pack === oldest) {
      S.entries.delete(key);
      const cut = key.lastIndexOf("|"), fi = fileIndex.get(key.slice(0, cut));
      if (fi !== undefined) sbcPtabDel(C, sbcKey(fi, Number(key.slice(cut + 1))));
    }
    try { pk.h.close(); } catch (e) {}
    S.packs.delete(oldest); S.total -= pk.size; S.dirty = true;
    try { await S.dir.removeEntry("pack-" + oldest + ".bin"); } catch (e) {}
    Atomics.add(C.H, SBC_PS_EVICT, 1);
  }
}
// Persist whole blocks of [start, start+bytes.length) (partial edge blocks only if they end the file).
function storeWriteRange(fi, start, bytes) {
  if (!store) return;
  const B = C.B, f = files[fi];
  store.writing = store.writing.then(async () => {
    for (let n = Math.ceil(start / B); n * B < start + bytes.length; ++n) {
      const bs = n * B, be = Math.min(bs + B, f.size);
      if (be > start + bytes.length) break;                   // incomplete block
      const key = storeKey(fi, n);
      if (store.entries.has(key)) continue;
      const pk = await storeEnsurePack();
      const part = bytes.subarray(bs - start, be - start);
      if (pk.h.write(part, {at: pk.size}) !== part.length) continue;
      store.entries.set(key, {pack: store.cur, at: pk.size, len: part.length});
      pk.size += part.length; store.total += part.length; store.dirty = true;
      sbcPtabAdd(C, sbcKey(fi, n));
      Atomics.add(C.H, SBC_PS_WRITES, 1); Atomics.add(C.H, SBC_PS_KB, part.length >> 10);
    }
    if (store.total > store.budget) await storeEvict();
    if (performance.now() - store.lastFlush > 5000) storeFlush();
  }).catch(e => log("persist failed: " + e));
}
function storeFlush() {
  const S = store;
  if (!S || !S.dirty) return;
  S.lastFlush = performance.now();
  try {
    for (const pk of S.packs.values()) pk.h.flush();
    const entries = [];
    for (const [key, e] of S.entries) { const a = key.lastIndexOf("|"), b = key.lastIndexOf("|", a - 1); entries.push([key.slice(0, b), key.slice(b + 1, a), Number(key.slice(a + 1)), e.pack, e.at, e.len]); }
    const json = new TextEncoder().encode(JSON.stringify({v: 1, seq: ++S.seq, blockBytes: C.B, packs: [...S.packs].map(([id, pk]) => ({id, size: pk.size})), entries}));
    const h = S.idx[S.seq % 2];
    h.truncate(0); h.write(json, {at: 0}); h.flush();
    S.dirty = false;
  } catch (e) { log("index flush failed: " + e); }
}

// ------------------------------------------------------------------ pthread requests
function onPortMessage(e) {
  const d = e.data;
  if (!d || !C) return;
  if (d.cmd === "miss") {                            // a pthread is about to block on a network read of this block
    noteDemandForDetect(d.file, d.block);
    const key = d.file + "|" + d.block;
    for (const plan of activePlans) {
      const i = plan.index.get(key);
      if (i !== undefined && i >= plan.next) {
        for (let j = plan.next; j < i; ++j) plan.skipped.push(plan.chunks[j]);
        plan.next = i + 1;
        Atomics.add(C.H, SBC_WARM_SKIPAHEAD, 1);
      }
    }
    return;
  }
  if (d.cmd === "load") {
    noteDemandForDetect(d.file, d.block);
    let ok = false;
    try { const bytes = storeRead(d.file, d.block); if (bytes) ok = sbcFill(C, d.slot, d.key, bytes, false); } catch (err) {}
    Atomics.add(C.H, ok ? SBC_PS_LOADHIT : SBC_PS_LOADMISS, 1);
    if (!ok) sbcAbort(C, d.slot, d.key);             // pthread wakes and reads from the network
  } else if (d.cmd === "persist") {
    if (detect && !detect.done) for (let n = Math.floor(d.start / C.B), last = Math.floor((d.start + d.len - 1) / C.B); n <= last; ++n) noteDemandForDetect(d.file, n);
    if (store) storeWriteRange(d.file, d.start, new Uint8Array(d.buf, d.off || 0, d.len));
    else if (Atomics.load(C.H, SBC_PS_READY) === 0 && waiterBytes + d.len <= WAITER_MAX) { storeWaiters.push(d); waiterBytes += d.len; }
    else Atomics.add(C.H, SBC_PS_SKIPPED, 1);
  }
}

// ------------------------------------------------------------------ io3 adaptive prefetch (into the shared cache)
function pump() {
  while (inflight < cap && pending.length) {
    const [file, block] = pending.shift(), f = files[file];
    if (!f) continue;
    const start = block * C.B;
    if (start >= f.size) continue;
    if (!sbcPfAllowed(C, file)) { Atomics.add(C.H, SBC_PF_OFFSKIP, 1); continue; }
    const blockKb = C.B >> 10, budgetKb = FREE_KB + pct / 100 * Atomics.load(C.H, SBC_KB_SYNC);
    if (Atomics.load(C.H, SBC_KB_PF) + (inflight + 1) * blockKb > budgetKb) { Atomics.add(C.H, SBC_PF_BUDGET, 1); continue; }
    const end = Math.min(start + C.B, f.size) - 1, k = sbcKey(file, block), r = {};
    if (sbcRead(C, k, 0, 0, null, 0, true, r) !== -3) { Atomics.add(C.H, SBC_PF_SKIP, 1); continue; }
    const slot = r.slot;
    ++inflight; Atomics.add(C.H, SBC_PF_INFL, 1); Atomics.add(C.H, SBC_PF_ISSUED, 1);
    (async () => {
      const local = storeRead(file, block);
      const bytes = local || await fetchRange(f, start, end);
      if (sbcFill(C, slot, k, bytes, true)) { Atomics.add(C.H, SBC_PF_DONE, 1); if (!local) Atomics.add(C.H, SBC_KB_PF, bytes.length >> 10); }
      if (!local) storeWriteRange(file, start, bytes);
    })().catch(() => { sbcAbort(C, slot, k); Atomics.add(C.H, SBC_PF_ERR, 1); })
      .finally(() => { --inflight; Atomics.sub(C.H, SBC_PF_INFL, 1); flushTrace(false); pump(); });
  }
}
async function loop() {
  for (;;) {
    const bell = Atomics.load(C.H, SBC_BELL);
    for (const it of sbcDequeue(C, 1024)) pending.push(it);
    if (pending.length > 256) { Atomics.add(C.H, SBC_PF_SKIP, pending.length - 256); pending.splice(0, pending.length - 256); }
    pump();
    flushTrace(false);
    if (store && performance.now() - store.lastFlush > 5000) storeFlush();
    // Wake on the doorbell (waitAsync) or after 20 ms at worst; never rely on waitAsync alone.
    const w = Atomics.waitAsync ? Atomics.waitAsync(C.H, SBC_BELL, bell, 1000) : null;
    if (!w || w.async) await Promise.race([w ? w.value : null, new Promise(r => setTimeout(r, 20))].filter(Boolean));
  }
}

// ------------------------------------------------------------------ warm lists
async function warm(names, width) {
  const fileIndex = new Map(files.map((f, i) => [f.path, i]));
  for (const spec of names) {
    let list = null, name = null;
    for (const alt of String(spec).split("|")) {
      if (warmedNames.has(alt)) { name = null; list = null; break; }
      try {
        const res = await fetch("/runtime/warm/" + encodeURIComponent(alt) + ".json", {cache: "no-store"});
        if (!res.ok) { log("warm list " + alt + " not found (" + res.status + ")"); continue; }
        list = await res.json(); name = alt;
        break;
      } catch (e) { log("warm list " + alt + " failed: " + e); }
    }
    if (!list || !name || warmedNames.has(name)) continue;
    warmedNames.add(name);
    if (!list || list.version !== 1 || list.blockBytes !== C.B) { log("warm list " + name + " incompatible"); continue; }
    // chunks of up to 8 contiguous missing blocks, in recorded first-demand order
    const chunks = [];
    for (const [path, first, count] of list.order) {
      const fi = fileIndex.get(path), f = files[fi];
      if (fi === undefined || (list.etags && list.etags[path] && list.etags[path] !== f.etag)) continue;
      let a = -1;
      for (let n = first; n <= first + count; ++n) {
        const need = n < first + count && n * C.B < f.size && !sbcPersisted(C, sbcKey(fi, n));
        if (need && a < 0) a = n;
        if (a >= 0 && (!need || n - a === warmChunk)) { chunks.push([fi, a, n - 1]); a = need ? n : -1; }
      }
    }
    const planned = chunks.reduce((s, c) => s + c[2] - c[1] + 1, 0);
    Atomics.add(C.H, SBC_WARM_PLAN, planned);
    log(`warm ${name}: ${planned} blocks to fetch, ${width} in flight` + (store ? "" : " (no persistent store: skipped)"));
    if (!store || !planned) continue;
    const began = performance.now();
    // [io6] demand-following cursor: a pthread sync miss on a block that is AHEAD of the cursor means the warm fell
    // behind the game; jump the cursor past it (skipped chunks are retried at the end, already-stored ones dropped).
    const plan = {chunks, next: 0, skipped: [], index: new Map};
    chunks.forEach(([fi, a, b], i) => { for (let n = a; n <= b; ++n) plan.index.set(fi + "|" + n, i); });
    activePlans.add(plan);
    const take = () => {
      if (plan.next < chunks.length) return chunks[plan.next++];
      return plan.skipped.length ? plan.skipped.shift() : null;
    };
    const lane = async () => {
      for (;;) {
        // optional: while any pthread waits on a sync miss, at most busyLanes warm requests run (ioWarmBusyLanes, 0 = off)
        while ((!running && bootLanes > 0 && warmActive >= bootLanes) || (busyLanes > 0 && warmActive >= busyLanes && Atomics.load(C.H, SBC_INFL) > 0)) { Atomics.add(C.H, SBC_WARM_THROTTLE, 1); await sleep(8); }
        const ch = take();
        if (!ch) break;
        const [fi, a, b] = ch, f = files[fi], start = a * C.B, end = Math.min((b + 1) * C.B, f.size) - 1;
        let stored = true;
        for (let n = a; n <= b && stored; ++n) stored = sbcPersisted(C, sbcKey(fi, n));
        if (stored) { Atomics.add(C.H, SBC_WARM_DUPSKIP, 1); continue; }      // the game (or another list) got it first
        ++warmActive;
        try {
          const bytes = await fetchRange(f, start, end).finally(() => --warmActive);
          storeWriteRange(fi, start, bytes);
          Atomics.add(C.H, SBC_WARM_DONE, b - a + 1); Atomics.add(C.H, SBC_WARM_KB, bytes.length >> 10);
        } catch (e) { Atomics.add(C.H, SBC_WARM_ERR, 1); }
        flushTrace(false);
      }
    };
    await Promise.all(Array.from({length: Math.min(width, chunks.length)}, lane));
    activePlans.delete(plan);
    await store.writing; storeFlush();
    log(`warm ${name}: done in ${Math.round(performance.now() - began)} ms, ${Math.round(Atomics.load(C.H, SBC_WARM_KB) / 1024)} MiB total`);
  }
}

self.onmessage = async e => {
  const d = e.data;
  if (!d) return;
  if (d.cmd === "port") { d.port.onmessage = onPortMessage; return; }
  if (d.cmd === "warm") { ready.then(() => warm(d.names || [], d.warmInflight || warmWidth)); return; }
  if (d.cmd === "phase") {
    phases.add(d.name);
    if (d.name !== "Initialization" && !running) { running = true; log("engine " + d.name + ": warm lanes uncapped"); }
    if (d.name === "menu") startLater("main menu up");
    return;
  }
  if (d.cmd !== "init" || C) return;
  C = sbcOpen(d.shared); cap = d.inflight || 4; trace = !!d.trace; pct = d.pct ?? 10;
  if (!C) return;
  files = (await (await fetch("/runtime/game-files.json", {cache: "no-store"})).json()).files;
  if (d.persistMB > 0) {
    try {
      store = await openStore(d.persistMB);
      Atomics.store(C.H, SBC_PS_READY, 1);
      log(`persistent store ready: ${store.entries.size} blocks, ${Math.round(store.total / 1048576)} MiB, budget ${Math.round(store.budget / 1048576)} MiB`);
      for (const w of storeWaiters.splice(0)) storeWriteRange(w.file, w.start, new Uint8Array(w.buf, w.off || 0, w.len));
      waiterBytes = 0;
    } catch (err) { store = null; Atomics.store(C.H, SBC_PS_READY, -1); log("persistent store unavailable, network only: " + err); }
  }
  storeWaiters.length = 0; waiterBytes = 0;
  warmWidth = d.warmInflight || 16; busyLanes = d.busyLanes ?? 0; warmChunk = d.warmChunk || 4; bootLanes = d.bootLanes ?? 4;
  laterSpecs = d.warm || [];
  if (laterSpecs.length) setTimeout(() => startLater("timeout"), d.laterDelayMs ?? 45000);
  if (store && d.detect) await loadDetector(d.detectK || 6);
  readyResolve();
  loop();
  if (d.warmBoot && d.warmBoot.length) warm(d.warmBoot, warmWidth);
};
