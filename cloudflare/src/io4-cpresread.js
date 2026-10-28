// [cloudflare io] opt-in switches + worker trace; defaults identical to the base bundle.
// [io4] adds: persistent OPFS tier through the helper (ioPersist=1), warm lists (ioWarm=a,b), sequential
// widening of sync misses (ioSeqWiden, default 4 in shared mode), demand-order recording for warm lists.
function cp2077IoConfig() {
  let q;
  try { q = new URLSearchParams(self.location.search); } catch (e) { q = new URLSearchParams(""); }
  const num = (k, lo, hi, d) => { const v = Number(q.get(k)); return q.has(k) && Number.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : d; };
  const MB = 1048576, shared = q.get("ioShared") === "1";
  const blockSize = Math.floor(num("ioBlockMB", 1, 16, 1)) * MB;
  const cacheLimit = Math.floor(num("ioCacheMB", 4, 1024, 16)) * MB;
  // Never let one widened request evict more than half of the cache.
  const ahead = Math.max(0, Math.min(Math.floor(num("ioAhead", 0, 32, 0)), Math.floor(cacheLimit / 2 / blockSize) - 1));
  // [io3] shared mode defaults to 2-block async read-ahead after ioSeqMin (3) consecutive blocks.
  const prefetch = Math.floor(num("ioPrefetch", 0, 32, shared ? 2 : 0));
  const seqMin = Math.floor(num("ioSeqMin", 2, 16, 3));
  // [io4] a sync miss inside a sequential run (>= seqMin blocks) fetches up to ioSeqWiden blocks in one request.
  const widen = Math.floor(num("ioSeqWiden", 1, 16, shared ? 4 : 1));
  return {blockSize, cacheLimit, ahead, prefetch, seqMin, widen, trace: q.get("assetTrace") === "1" || q.get("ioTrace") === "1"};
}

function cp2077IoTraceFlush(R, force) {
  const T = R.trace, now = performance.now();
  if (!T || (!force && now - T.last < 5000)) return;
  T.last = now;
  if (!T.requests && !T.hits && !T.dem.length) return;
  const blocks = {};
  for (const [p, s] of T.blocks) blocks[p] = {n: [...s], r: s.r || 0, b: s.b || 0};
  try {
    self.postMessage({cmd: "cp2077IoTrace", d: {requests: T.requests, bytes: T.bytes, ms: T.ms, maxMs: T.maxMs, hits: T.hits,
      reads: T.reads, readMs: T.readMs, cfg: {blockMB: R.cfg.blockSize / 1048576, cacheMB: R.cfg.cacheLimit / 1048576, ahead: R.cfg.ahead, widen: R.cfg.widen}, blocks, dem: T.dem}});
  } catch (e) {}
  T.requests = T.bytes = T.ms = T.maxMs = T.hits = T.reads = T.readMs = 0;
  T.blocks = new Map; T.dem = [];
}
// [io4] demand-order recording (first demand of each block in this thread), for warm lists.
function cp2077IoRecordDemand(R, path, n) {
  const T = R.trace;
  if (!T) return;
  const seen = R.demSeen || (R.demSeen = new Set), key = path + "|" + n;
  if (seen.has(key)) return;
  seen.add(key);
  T.dem.push([path, n]);
}
function cp2077IoPort(R) {
  const s = globalThis.__cpIoShared;
  return s && s.persist && s.port ? s.port : null;
}
function cp2077IoAllPersisted(SH, index, offset, count) {
  for (let n = Math.floor(offset / SH.B), last = Math.floor((offset + count - 1) / SH.B); n <= last; ++n)
    if (!sbcPersisted(SH, sbcKey(index, n))) return false;
  return true;
}

function CpResRead(index, offset, dest, length) {
  const R = globalThis.__cpResIO;
  if (!R || !R.files) return -2;
  const f = R.files[index];
  if (!f || !(offset >= 0) || length < 0) return -1;
  const count = Math.min(length, Math.max(0, f.size - offset));
  if (count === 0) return 0;
  const C = R.cfg || (R.cfg = cp2077IoConfig());
  const T = C.trace ? (R.trace || (R.trace = {requests: 0, bytes: 0, ms: 0, maxMs: 0, hits: 0, reads: 0, readMs: 0, blocks: new Map, dem: [], last: performance.now()})) : null;
  const began = performance.now();
  const url = f.path.split("/").map(encodeURIComponent).join("/");
  const fetchRange = (start, end) => {
    const t0 = T ? performance.now() : 0;
    try {
      const x = new XMLHttpRequest;
      x.open("GET", url, false);
      x.responseType = "arraybuffer";
      x.setRequestHeader("Range", "bytes=" + start + "-" + end);
      x.setRequestHeader("If-Match", f.etag);
      x.send(null);
      ++R.stats.requests;
      if (x.status !== 206 || x.getResponseHeader("ETag") !== f.etag || x.getResponseHeader("Content-Range") !== "bytes " + start + "-" + end + "/" + f.size) return null;
      const bytes = new Uint8Array(x.response);
      if (T) {
        const ms = performance.now() - t0;
        ++T.requests; T.bytes += bytes.length; T.ms += ms; if (ms > T.maxMs) T.maxMs = ms;
        let s = T.blocks.get(f.path);
        if (!s) T.blocks.set(f.path, s = new Set);
        s.r = (s.r || 0) + 1; s.b = (s.b || 0) + bytes.length;
        for (let n = Math.floor(start / 1048576); n <= Math.floor(end / 1048576); ++n) s.add(n);
      }
      return bytes.length === end - start + 1 ? bytes : null;
    } catch (e) {
      return null;
    }
  };
  const blockSize = C.blockSize, cacheLimit = C.cacheLimit;
  const insert = (key, bytes) => {
    while (R.bytes + bytes.length > cacheLimit && R.cache.size) {
      const oldest = R.cache.keys().next().value;
      R.bytes -= R.cache.get(oldest).length;
      R.cache.delete(oldest);
    }
    R.cache.set(key, bytes);
    R.bytes += bytes.length;
  };
  const block = number => {
    const key = index + ":" + number;
    const cached = R.cache.get(key);
    if (cached) {
      R.cache.delete(key);
      R.cache.set(key, cached);
      ++R.stats.hits;
      if (T) ++T.hits;
      return cached;
    }
    const start = number * blockSize, end = Math.min(start + blockSize, f.size) - 1;
    if (C.ahead > 0) {
      // One request for this block and up to C.ahead following ones not already cached.
      let last = number;
      const lastBlock = Math.floor((f.size - 1) / blockSize);
      while (last < number + C.ahead && last < lastBlock && !R.cache.has(index + ":" + (last + 1))) ++last;
      if (last > number) {
        const wideEnd = Math.min((last + 1) * blockSize, f.size) - 1;
        const wide = fetchRange(start, wideEnd);
        if (!wide) return null;
        for (let n = last; n > number; --n) insert(index + ":" + n, wide.slice((n - number) * blockSize, Math.min((n - number + 1) * blockSize, wide.length)));
        const bytes = wide.slice(0, end - start + 1);
        insert(key, bytes);   // demanded block last = most recently used
        return bytes;
      }
    }
    const bytes = fetchRange(start, end);
    if (!bytes) return null;
    insert(key, bytes);
    return bytes;
  };
  const target = Number(dest);
  let copied = 0;
  // P: port to the IO helper, unless its persistent store failed to open (then nothing is sent)
  const SH = cp2077IoSharedCtx(R), P = SH && Atomics.load(SH.H, SBC_PS_READY) !== -1 ? cp2077IoPort(R) : null;
  if (count >= 524288 && !(P && cp2077IoAllPersisted(SH, index, offset, count))) {
    if (T && SH) for (let n = Math.floor(offset / SH.B), last = Math.floor((offset + count - 1) / SH.B); n <= last; ++n) cp2077IoRecordDemand(R, f.path, n);
    const bytes = fetchRange(offset, offset + count - 1);
    if (!bytes) {
      ++R.stats.failures;
      return -1;
    }
    growMemViews();
    (growMemViews(), HEAPU8).set(bytes, target);
    copied = count;
    // [io4] keep the whole blocks of a large read in the persistent store (buffer transferred, no copy)
    if (P) { try { P.postMessage({cmd: "persist", file: index, start: offset, buf: bytes.buffer, off: bytes.byteOffset, len: bytes.length}, [bytes.buffer]); } catch (e) {} }
  } else {
    // [io2] one SharedArrayBuffer block cache for all pthreads; misses are fetched once (in-flight dedup).
    // Falls back to the private per-thread cache when the shared buffers have not arrived (a read before
    // the first message), are invalid, or anything in the shared path throws.
    if (SH) {
    const SB = SH.B, H = SH.H, r = {}, r2 = {};
    let held = -1, heldKey = 0;
    try {
    while (copied < count) {
      const at = offset + copied, n = Math.floor(at / SB), within = at % SB, want = Math.min(count - copied, SB - within);
      const k = sbcKey(index, n);
      const run = cp2077IoNoteDemand(R, SH, index, n, f);
      let got, tries = 0, asked = false;
      for (;;) {
        growMemViews();
        got = sbcRead(SH, k, within, want, HEAPU8, target + copied, true, r);
        if (got === -1 && tries++ < 4) { sbcWait(SH, r.slot, 5000); continue; }
        if (got === -3 && !asked && P && sbcPersisted(SH, k)) {
          // [io4] local persistent copy: the helper fills our claimed slot from OPFS (no network)
          asked = true; held = r.slot; heldKey = k;
          Atomics.add(H, SBC_PS_LOADREQ, 1);
          P.postMessage({cmd: "load", slot: r.slot, key: k, file: index, block: n});
          sbcWait(SH, r.slot, 3000);
          held = -1; tries = 0;
          continue;                     // ready -> hit; aborted (not found) -> claim again -> network
        }
        break;
      }
      if (got > 0) { ++R.stats.hits; if (T) ++T.hits; copied += got; continue; }
      if (got === 0) { ++R.stats.failures; return -1; }
      const claimed = got === -3, slot = r.slot, start = n * SB;
      if (claimed) { held = slot; heldKey = k; }
      // [io4] widen inside a sequential run: this block + following ones not cached / not persisted
      let lastN = n;
      if (claimed && C.widen > 1 && run >= C.seqMin) {
        const lastBlock = Math.floor((f.size - 1) / SB);
        while (lastN < n + C.widen - 1 && lastN < lastBlock) {
          const k2 = sbcKey(index, lastN + 1);
          if (sbcHas(SH, k2) || (P && sbcPersisted(SH, k2))) break;
          ++lastN;
        }
      }
      const end = Math.min((lastN + 1) * SB, f.size) - 1;
      Atomics.add(H, SBC_MISSES, 1);
      if (lastN > n) Atomics.add(H, SBC_WIDEN, 1);
      const now = Atomics.add(H, SBC_INFL, 1) + 1;
      for (let m = Atomics.load(H, SBC_INFLMAX); now > m; m = Atomics.load(H, SBC_INFLMAX)) if (Atomics.compareExchange(H, SBC_INFLMAX, m, now) === m) break;
      const bytes = fetchRange(start, end);
      Atomics.sub(H, SBC_INFL, 1);
      if (!bytes) { if (claimed) sbcAbort(SH, slot, k); held = -1; ++R.stats.failures; return -1; }
      Atomics.add(H, SBC_KB_SYNC, bytes.length >> 10);
      if (claimed) sbcFill(SH, slot, k, bytes.subarray(0, Math.min(SB, bytes.length)), false);
      held = -1;
      for (let j = n + 1; j <= lastN; ++j) {
        const kj = sbcKey(index, j);
        if (sbcRead(SH, kj, 0, 0, null, 0, true, r2) === -3) sbcFill(SH, r2.slot, kj, bytes.subarray((j - n) * SB, Math.min((j - n + 1) * SB, bytes.length)), true);
      }
      const take = Math.min(want, bytes.length - within);
      if (take <= 0) { ++R.stats.failures; return -1; }
      growMemViews();
      (growMemViews(), HEAPU8).set(bytes.subarray(within, within + take), target + copied);
      copied += take;
      if (P) { try { P.postMessage({cmd: "persist", file: index, start, buf: bytes.buffer, off: bytes.byteOffset, len: bytes.length}, [bytes.buffer]); } catch (e) {} }
    }
    } catch (e) {
      if (held >= 0) { try { sbcAbort(SH, held, heldKey); } catch (e2) {} }
      R.sh = null; R.shOff = true;
      err("CP2077 io shared cache disabled in this thread, private cache used: " + e);
    }
    }
    while (copied < count) {
      const at = offset + copied, bytes = block(Math.floor(at / blockSize));
      if (!bytes) {
        ++R.stats.failures;
        return -1;
      }
      const within = at % blockSize, take = Math.min(count - copied, bytes.length - within);
      if (take <= 0) {
        ++R.stats.failures;
        return -1;
      }
      growMemViews();
      (growMemViews(), HEAPU8).set(bytes.subarray(within, within + take), target + copied);
      copied += take;
    }
  }
  const ms = performance.now() - began;
  ++R.stats.reads;
  R.stats.bytes += copied;
  R.stats.ms += ms;
  if (ms > R.stats.maxMs) R.stats.maxMs = ms;
  if (T) { ++T.reads; T.readMs += ms; cp2077IoTraceFlush(R, false); }
  if ((R.stats.reads & 1023) === 0) err("CP2077 off-main io probe: reads=" + R.stats.reads + " MiB=" + (R.stats.bytes / 1048576).toFixed(1) + " ms=" + R.stats.ms.toFixed(0) + " maxMs=" + R.stats.maxMs.toFixed(1) + " requests=" + R.stats.requests + " blockHits=" + R.stats.hits + " failures=" + R.stats.failures);
  return copied;
}

// [io2] this thread's view of the shared cache, or null (not delivered yet / invalid / disabled after an error).
function cp2077IoSharedCtx(R) {
  if (R.shOff) return null;
  if (R.sh) return R.sh;
  const s = globalThis.__cpIoShared;
  if (!s) return null;               // read before the shared buffers arrived: private path, retry next read
  const c = sbcOpen(s.m);
  if (!c) { R.shOff = true; err("CP2077 io shared cache rejected (invalid buffers); private cache used"); return null; }
  return R.sh = c;
}

// Sequential-run tracking per file and thread (returns the current run length). [io3] async read-ahead
// only after ioSeqMin consecutive demanded blocks: queue the next ioPrefetch blocks for the helper.
function cp2077IoNoteDemand(R, SH, index, n, f) {
  cp2077IoRecordDemand(R, f.path, n);
  const s = R.seq || (R.seq = new Map);
  let e = s.get(index);
  if (!e) s.set(index, e = {last: -2, run: 0, upTo: -1});
  if (n === e.last) return e.run;
  e.run = n === e.last + 1 ? e.run + 1 : 1;
  e.last = n;
  const depth = R.cfg.prefetch;
  if (!depth || !globalThis.__cpIoShared.prefetch || e.run < R.cfg.seqMin || !sbcPfAllowed(SH, index)) return e.run;
  const lastBlock = Math.floor((f.size - 1) / SH.B), to = Math.min(lastBlock, n + depth);
  for (let b = Math.max(n + 1, e.upTo + 1); b <= to; ++b) if (!sbcEnqueue(SH, index, b)) break;
  e.upTo = to;
  return e.run;
}

// [io2] main thread: create the shared cache + IO helper once (only with ?ioShared=1) and hand the
// shared buffers (and, with ?ioPersist=1, a MessagePort to the helper) to every pthread as its first message.
function cp2077IoSharedMain() {
  if (Module["cp2077IoShared"] !== undefined) return Module["cp2077IoShared"];
  Module["cp2077IoShared"] = null;
  try {
    const q = new URLSearchParams(globalThis.location ? location.search : "");
    if (q.get("ioShared") !== "1" || typeof SharedArrayBuffer === "undefined") return null;
    const num = (k, lo, hi, d) => { const v = Number(q.get(k)); return q.has(k) && Number.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : d; };
    const B = Math.floor(num("ioBlockMB", 1, 16, 1)) * 1048576;
    const slots = Math.max(8, Math.floor(num("ioSharedMB", 16, 2048, 256) * 1048576 / B));
    const prefetch = Math.floor(num("ioPrefetch", 0, 32, 2)), inflight = Math.floor(num("ioInflight", 1, 16, 4)), pct = num("ioPrefetchPct", 0, 100, 10);
    const persist = q.get("ioPersist") === "1" && typeof MessageChannel !== "undefined";
    const persistMB = persist ? Math.floor(num("ioPersistMB", 256, 65536, 4096)) : 0;
    const warm = (q.get("ioWarm") || "").split(",").filter(x => /^[A-Za-z0-9_-]{1,40}$/.test(x)).slice(0, 4);
    const warmInflight = Math.floor(num("ioWarmInflight", 1, 64, 16));
    const m = sbcCreate(slots, B, 1024, persist ? Math.max(4096, Math.ceil(persistMB * 1048576 / B) * 2) : 0);
    const c = sbcOpen(m);
    if (!c) { err("CP2077 io shared cache could not be created (invalid buffers); shared mode off"); return null; }
    const S = Module["cp2077IoShared"] = {m, c, prefetch, inflight, persist: false, helper: null};
    if (prefetch > 0 || persist) {
      const helper = S.helper = new Worker(new URL("cp2077-io-helper.js", _scriptName).href, {name: "cp2077-io-helper"});
      helper.onmessage = e => { if (e.data && e.data.cmd === "trace") cp2077IoTraceMain("helper", e.data.d); };
      helper.onerror = e => err("CP2077 io helper error: " + (e.message || e));
      helper.postMessage({cmd: "init", shared: m, inflight, pct, persistMB, warm: persist ? warm : [], warmInflight,
        trace: q.get("assetTrace") === "1" || q.get("ioTrace") === "1"});
      S.persist = persist;
    }
    return S;
  } catch (e) {
    err("CP2077 io shared cache unavailable: " + e);
    return null;
  }
}
function cp2077IoSharedPost(worker) {
  const S = cp2077IoSharedMain();
  if (!S) return;
  if (S.persist && S.helper) {
    try {
      const ch = new MessageChannel();
      S.helper.postMessage({cmd: "port", port: ch.port1}, [ch.port1]);
      worker.postMessage({cmd: "cp2077IoShared", d: {m: S.m, prefetch: S.prefetch, persist: true, port: ch.port2}}, [ch.port2]);
      return;
    } catch (e) { err("CP2077 io helper port failed, no persistent tier for this thread: " + e); }
  }
  worker.postMessage({cmd: "cp2077IoShared", d: {m: S.m, prefetch: S.prefetch}});
}

// Main thread: aggregate worker io traces; log every 30 s (format compatible with trace-to-ranges.mjs).
// [io4] also "CP2077 ioWarm record: {...}" = blocks in first-demand order, for scripts/make-warm-list.mjs.
function cp2077IoTraceMain(workerID, d) {
  const A = Module["cp2077IoTrace"] || (Module["cp2077IoTrace"] = (() => {
    const S = {t0: performance.now(), workers: new Set, cfg: null, requests: 0, bytes: 0, ms: 0, maxMs: 0, hits: 0, reads: 0, readMs: 0, files: new Map,
      order: [], orderSeen: new Set};
    const rle = set => { const v = [...set].sort((x, y) => x - y), o = []; for (let i = 0; i < v.length;) { let j = i; while (j + 1 < v.length && v[j + 1] === v[j] + 1) ++j; o.push(i === j ? "" + v[i] : v[i] + "-" + v[j]); i = j + 1; } return o.join(","); };
    S.export = () => { const out = {version: 1, blockBytes: 1048576, t: Math.round((performance.now() - S.t0) / 1000), files: {}}; for (const [p, r] of S.files) out.files[p] = {blocks: rle(r.blocks), requests: r.requests, bytes: r.bytes}; return out; };
    S.record = () => {
      const runs = [];
      for (const [p, n] of S.order) { const last = runs[runs.length - 1]; if (last && last[0] === p && last[1] + last[2] === n) ++last[2]; else runs.push([p, n, 1]); }
      return {version: 1, blockBytes: 1048576, t: Math.round((performance.now() - S.t0) / 1000), blocks: S.order.length, order: runs};
    };
    S.summary = () => {
      let uniq = 0; const files = [];
      for (const [p, r] of S.files) { uniq += r.blocks.size; files.push({p: p.replace("/game/archive/pc/", ""), req: r.requests, mb: Math.round(r.bytes / 104857.6) / 10, uniqMb: r.blocks.size}); }
      files.sort((x, y) => y.req - x.req);
      return {t: Math.round((performance.now() - S.t0) / 1000), cfg: S.cfg, workers: S.workers.size,
        totals: {requests: S.requests, mb: Math.round(S.bytes / 104857.6) / 10, uniqueMb: uniq, requestMs: Math.round(S.ms), maxRequestMs: Math.round(S.maxMs),
          avgRequestMs: S.requests ? Math.round(S.ms / S.requests) : 0, blockHits: S.hits, reads: S.reads, readStallMs: Math.round(S.readMs), demandBlocks: S.order.length},
        shared: (() => { try { const X = Module["cp2077IoShared"]; return X ? sbcStats(X.c) : null; } catch (e) { return null; } })(), files};
    };
    S.dump = () => { try {
      console.log("CP2077 ioTrace: " + JSON.stringify(S.summary()));
      console.log("CP2077 ioTrace ranges: " + JSON.stringify(S.export()));
      if (S.order.length) console.log("CP2077 ioWarm record: " + JSON.stringify(S.record()));
    } catch (e) {} };
    setInterval(S.dump, 30000);
    return S;
  })());
  A.workers.add(workerID); if (d.cfg) A.cfg = d.cfg;
  A.requests += d.requests; A.bytes += d.bytes; A.ms += d.ms; A.hits += d.hits; A.reads += d.reads; A.readMs += d.readMs;
  if (d.maxMs > A.maxMs) A.maxMs = d.maxMs;
  for (const p in d.blocks) {
    let r = A.files.get(p);
    if (!r) A.files.set(p, r = {blocks: new Set, requests: 0, bytes: 0});
    const e = d.blocks[p]; r.requests += e.r; r.bytes += e.b;
    for (const n of e.n) r.blocks.add(n);
  }
  if (d.dem) for (const [p, n] of d.dem) { const key = p + "|" + n; if (!A.orderSeen.has(key) && A.order.length < 200000) { A.orderSeen.add(key); A.order.push([p, n]); } }
}

