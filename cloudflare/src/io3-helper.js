// [io3] Async prefetch helper (budget: prefetch bytes <= 4 MiB + ioPrefetchPct% of demand bytes; per-file off switch) (a plain dedicated worker with an idle event loop, unlike the busy pthreads).
// Drains the shared prefetch queue, claims cache slots and fills them with async Range fetches, at most
// `inflight` at a time. Needs io-shared-cache.js prepended (make-io-bundle2.mjs does that).
let C = null, files = null, inflight = 0, cap = 4, trace = false, pct = 10;
const FREE_KB = 4096;
const pending = [];
const T = {requests: 0, bytes: 0, ms: 0, maxMs: 0, blocks: new Map, last: 0};
function flushTrace(force) {
  const now = performance.now();
  if (!trace || (!force && now - T.last < 5000) || !T.requests) return;
  T.last = now;
  const blocks = {};
  for (const [p, s] of T.blocks) blocks[p] = {n: [...s], r: s.r || 0, b: s.b || 0};
  self.postMessage({cmd: "trace", d: {requests: T.requests, bytes: T.bytes, ms: T.ms, maxMs: T.maxMs, hits: 0, reads: 0, readMs: 0, cfg: null, blocks}});
  T.requests = T.bytes = T.ms = T.maxMs = 0; T.blocks = new Map;
}
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
    const slot = r.slot, began = performance.now();
    ++inflight; Atomics.add(C.H, SBC_PF_INFL, 1); Atomics.add(C.H, SBC_PF_ISSUED, 1);
    fetch(f.path.split("/").map(encodeURIComponent).join("/"), {headers: {Range: `bytes=${start}-${end}`, "If-Match": f.etag}, cache: "no-store"})
      .then(async res => {
        const bytes = new Uint8Array(await res.arrayBuffer());
        if (res.status !== 206 || res.headers.get("ETag") !== f.etag || res.headers.get("Content-Range") !== `bytes ${start}-${end}/${f.size}` || bytes.length !== end - start + 1)
          throw new Error("range identity mismatch");
        if (sbcFill(C, slot, k, bytes, true)) { Atomics.add(C.H, SBC_PF_DONE, 1); Atomics.add(C.H, SBC_KB_PF, bytes.length >> 10); }
        if (trace) {
          const ms = performance.now() - began;
          ++T.requests; T.bytes += bytes.length; T.ms += ms; if (ms > T.maxMs) T.maxMs = ms;
          let s = T.blocks.get(f.path); if (!s) T.blocks.set(f.path, s = new Set);
          s.r = (s.r || 0) + 1; s.b = (s.b || 0) + bytes.length;
          for (let n = Math.floor(start / 1048576); n <= Math.floor(end / 1048576); ++n) s.add(n);
        }
      })
      .catch(() => { sbcAbort(C, slot, k); Atomics.add(C.H, SBC_PF_ERR, 1); })
      .finally(() => { --inflight; Atomics.sub(C.H, SBC_PF_INFL, 1); flushTrace(false); pump(); });
  }
}
async function loop() {
  for (;;) {
    const bell = Atomics.load(C.H, SBC_BELL);
    for (const it of sbcDequeue(C, 1024)) pending.push(it);
    // Newest demand matters most: keep the queue short, drop the oldest.
    if (pending.length > 256) { Atomics.add(C.H, SBC_PF_SKIP, pending.length - 256); pending.splice(0, pending.length - 256); }
    pump();
    flushTrace(false);
    // Wake on the doorbell (waitAsync) or after 20 ms at worst; never rely on waitAsync alone.
    const w = Atomics.waitAsync ? Atomics.waitAsync(C.H, SBC_BELL, bell, 1000) : null;
    if (!w || w.async) await Promise.race([w ? w.value : null, new Promise(r => setTimeout(r, 20))].filter(Boolean));
  }
}
self.onmessage = async e => {
  if (!e.data || e.data.cmd !== "init" || C) return;
  C = sbcOpen(e.data.shared); cap = e.data.inflight || 4; trace = !!e.data.trace; pct = e.data.pct ?? 10;
  if (!C) return;
  files = (await (await fetch("/runtime/game-files.json", {cache: "no-store"})).json()).files;
  loop();
};
