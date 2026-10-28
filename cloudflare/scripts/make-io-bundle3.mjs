// [io3] v3: like v2 but prefetch only after ioSeqMin (3) sequential blocks, ioPrefetch default 2 in shared mode,
// prefetch budget 4 MiB + ioPrefetchPct (10)% of demand bytes, per-file auto-off when used/(used+wasted) < 50%.
// Sources: src/io3-shared-cache.js, src/io3-helper.js; page native-browser-cfio3.html.
// [io2] v2 of make-io-bundle.mjs: adds ?ioShared=1 (one SharedArrayBuffer block cache for all pthreads, in-flight
// dedup), ?ioSharedMB= (default 256), ?ioPrefetch=K (async, adaptive: after 2+ consecutive blocks, via a helper
// worker), ?ioInflight=N (async prefetch concurrency, default 4). Everything below from v1 still applies.
// Make a NEW versioned engine bundle whose pthread resource reader (CpResRead) gets opt-in switches and a
// worker-side asset trace. The live bundle and native-browser.html are never modified.
//   node scripts/make-io-bundle.mjs [--base <64-hex bundle>]   (default: build/active-engine-bundle.json)
// Output: build/game/engine-versions/<sha256 of new cp2077.js>/ (wasm/data copied byte-identical) and the
// test page build/game/native-browser-cfio3.html (served as /runtime/native-browser-cfio3.html).
// Switches (page URL; forwarded to the pthread workers via the cp2077.js script URL query):
//   ioBlockMB=1..16   block size (default 1)   ioCacheMB=4..1024  per-worker LRU (default 16)
//   ioAhead=0..32     extra blocks fetched in the same request on a miss (default 0)
//   assetTrace=1 | ioTrace=1   workers post per-block traffic to the main thread, which logs
//                    "CP2077 ioTrace: {...}" and "CP2077 ioTrace ranges: {...}" every 30 s.
// With no switches the reader behaves exactly like the base bundle (1 MiB blocks, 16 MiB LRU, no read-ahead).
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import crypto from "node:crypto";
import {RUNTIME, CF_ROOT} from "./common.mjs";
const CF_SRC = path.join(CF_ROOT, "src");

const ORIGINAL_CPRESREAD_SHA = "999ac524c191ca2b55a2faf19ce2e821393d600bd454ee5ffa71c66c42fd1748";
const i = process.argv.indexOf("--base");
const base = i > 0 ? process.argv[i + 1] : JSON.parse(fs.readFileSync(path.join(RUNTIME, "..", "active-engine-bundle.json"), "utf8")).BuildID;
const baseDir = path.join(RUNTIME, "engine-versions", base);
let js = fs.readFileSync(path.join(baseDir, "cp2077.js"), "utf8");
const sha = s => crypto.createHash("sha256").update(s).digest("hex");

// ---- 1. replace CpResRead (verified byte-for-byte against the version this patch was written for)
const startTok = "function CpResRead(", endTok = "function cp2077StorageFlushBegin(";
const a = js.indexOf(startTok), b = js.indexOf(endTok);
if (a < 0 || b < a || js.indexOf(startTok, a + 1) >= 0) throw new Error("CpResRead not found exactly once");
const original = js.slice(a, b);
if (sha(original.replace(/\r\n/g, "\n")) !== ORIGINAL_CPRESREAD_SHA && sha(original) !== ORIGINAL_CPRESREAD_SHA)
  throw new Error("CpResRead differs from the audited version; refusing to patch");

const NEW_CPRESREAD = String.raw`// [cloudflare io] opt-in switches + worker trace; defaults identical to the base bundle.
function cp2077IoConfig() {
  let q;
  try { q = new URLSearchParams(self.location.search); } catch (e) { q = new URLSearchParams(""); }
  const num = (k, lo, hi, d) => { const v = Number(q.get(k)); return q.has(k) && Number.isFinite(v) ? Math.max(lo, Math.min(hi, v)) : d; };
  const MB = 1048576;
  const blockSize = Math.floor(num("ioBlockMB", 1, 16, 1)) * MB;
  const cacheLimit = Math.floor(num("ioCacheMB", 4, 1024, 16)) * MB;
  // Never let one widened request evict more than half of the cache.
  const ahead = Math.max(0, Math.min(Math.floor(num("ioAhead", 0, 32, 0)), Math.floor(cacheLimit / 2 / blockSize) - 1));
  // [io3] shared mode defaults to 2-block async read-ahead after ioSeqMin (3) consecutive blocks.
  const prefetch = Math.floor(num("ioPrefetch", 0, 32, q.get("ioShared") === "1" ? 2 : 0));
  const seqMin = Math.floor(num("ioSeqMin", 2, 16, 3));
  return {blockSize, cacheLimit, ahead, prefetch, seqMin, trace: q.get("assetTrace") === "1" || q.get("ioTrace") === "1"};
}

function cp2077IoTraceFlush(R, force) {
  const T = R.trace, now = performance.now();
  if (!T || (!force && now - T.last < 5000)) return;
  T.last = now;
  if (!T.requests && !T.hits) return;
  const blocks = {};
  for (const [p, s] of T.blocks) blocks[p] = {n: [...s], r: s.r || 0, b: s.b || 0};
  try {
    self.postMessage({cmd: "cp2077IoTrace", d: {requests: T.requests, bytes: T.bytes, ms: T.ms, maxMs: T.maxMs, hits: T.hits,
      reads: T.reads, readMs: T.readMs, cfg: {blockMB: R.cfg.blockSize / 1048576, cacheMB: R.cfg.cacheLimit / 1048576, ahead: R.cfg.ahead}, blocks}});
  } catch (e) {}
  T.requests = T.bytes = T.ms = T.maxMs = T.hits = T.reads = T.readMs = 0;
  T.blocks = new Map;
}

function CpResRead(index, offset, dest, length) {
  const R = globalThis.__cpResIO;
  if (!R || !R.files) return -2;
  const f = R.files[index];
  if (!f || !(offset >= 0) || length < 0) return -1;
  const count = Math.min(length, Math.max(0, f.size - offset));
  if (count === 0) return 0;
  const C = R.cfg || (R.cfg = cp2077IoConfig());
  const T = C.trace ? (R.trace || (R.trace = {requests: 0, bytes: 0, ms: 0, maxMs: 0, hits: 0, reads: 0, readMs: 0, blocks: new Map, last: performance.now()})) : null;
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
  if (count >= 524288) {
    const bytes = fetchRange(offset, offset + count - 1);
    if (!bytes) {
      ++R.stats.failures;
      return -1;
    }
    growMemViews();
    (growMemViews(), HEAPU8).set(bytes, target);
    copied = count;
  } else {
    // [io2] one SharedArrayBuffer block cache for all pthreads; misses are fetched once (in-flight dedup).
    // Falls back to the private per-thread cache when the shared buffers have not arrived (a read before
    // the first message), are invalid, or anything in the shared path throws.
    const SH = cp2077IoSharedCtx(R);
    if (SH) {
    const SB = SH.B, H = SH.H, r = {};
    let held = -1, heldKey = 0;
    try {
    while (copied < count) {
      const at = offset + copied, n = Math.floor(at / SB), within = at % SB, want = Math.min(count - copied, SB - within);
      const k = sbcKey(index, n);
      cp2077IoNoteDemand(R, SH, index, n, f);
      let got, tries = 0;
      for (;;) {
        growMemViews();
        got = sbcRead(SH, k, within, want, HEAPU8, target + copied, true, r);
        if (got === -1 && tries++ < 3) { sbcWait(SH, r.slot, 5000); continue; }
        break;
      }
      if (got > 0) { ++R.stats.hits; if (T) ++T.hits; copied += got; continue; }
      if (got === 0) { ++R.stats.failures; return -1; }
      const claimed = got === -3, slot = r.slot, start = n * SB, end = Math.min(start + SB, f.size) - 1;
      if (claimed) { held = slot; heldKey = k; }
      Atomics.add(H, SBC_MISSES, 1);
      const now = Atomics.add(H, SBC_INFL, 1) + 1;
      for (let m = Atomics.load(H, SBC_INFLMAX); now > m; m = Atomics.load(H, SBC_INFLMAX)) if (Atomics.compareExchange(H, SBC_INFLMAX, m, now) === m) break;
      const bytes = fetchRange(start, end);
      Atomics.sub(H, SBC_INFL, 1);
      if (!bytes) { if (claimed) sbcAbort(SH, slot, k); held = -1; ++R.stats.failures; return -1; }
      Atomics.add(H, SBC_KB_SYNC, bytes.length >> 10);
      if (claimed) sbcFill(SH, slot, k, bytes, false);
      held = -1;
      const take = Math.min(want, bytes.length - within);
      if (take <= 0) { ++R.stats.failures; return -1; }
      growMemViews();
      (growMemViews(), HEAPU8).set(bytes.subarray(within, within + take), target + copied);
      copied += take;
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

// [io2] adaptive async read-ahead: only after 2+ consecutive demanded blocks of one file in this thread,
// queue the next ioPrefetch blocks for the helper worker (off the critical path). Random reads stay 1 block.
function cp2077IoNoteDemand(R, SH, index, n, f) {
  const depth = R.cfg.prefetch;
  if (!depth || !globalThis.__cpIoShared.prefetch) return;
  const s = R.seq || (R.seq = new Map);
  let e = s.get(index);
  if (!e) s.set(index, e = {last: -2, run: 0, upTo: -1});
  if (n === e.last) return;
  e.run = n === e.last + 1 ? e.run + 1 : 1;
  e.last = n;
  if (e.run < R.cfg.seqMin || !sbcPfAllowed(SH, index)) return;
  const lastBlock = Math.floor((f.size - 1) / SH.B), to = Math.min(lastBlock, n + depth);
  for (let b = Math.max(n + 1, e.upTo + 1); b <= to; ++b) if (!sbcEnqueue(SH, index, b)) break;
  e.upTo = to;
}

// [io2] main thread: create the shared cache + prefetch helper once (only with ?ioShared=1) and hand the
// shared buffers to every pthread worker as its first message.
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
    const m = sbcCreate(slots, B, 1024);
    const c = sbcOpen(m);
    if (!c) { err("CP2077 io shared cache could not be created (invalid buffers); shared mode off"); return null; }
    const S = Module["cp2077IoShared"] = {m, c, prefetch, inflight, helper: null};
    if (prefetch > 0) {
      const helper = S.helper = new Worker(new URL("cp2077-io-helper.js", _scriptName).href, {name: "cp2077-io-helper"});
      helper.onmessage = e => { if (e.data && e.data.cmd === "trace") cp2077IoTraceMain("helper", e.data.d); };
      helper.onerror = e => err("CP2077 io helper error: " + (e.message || e));
      helper.postMessage({cmd: "init", shared: m, inflight, pct, trace: q.get("assetTrace") === "1" || q.get("ioTrace") === "1"});
    }
    return S;
  } catch (e) {
    err("CP2077 io shared cache unavailable: " + e);
    return null;
  }
}
function cp2077IoSharedPost(worker) {
  const S = cp2077IoSharedMain();
  if (S) worker.postMessage({cmd: "cp2077IoShared", d: {m: S.m, prefetch: S.prefetch}});
}

// Main thread: aggregate worker io traces; log every 30 s (format compatible with trace-to-ranges.mjs).
function cp2077IoTraceMain(workerID, d) {
  const A = Module["cp2077IoTrace"] || (Module["cp2077IoTrace"] = (() => {
    const S = {t0: performance.now(), workers: new Set, cfg: null, requests: 0, bytes: 0, ms: 0, maxMs: 0, hits: 0, reads: 0, readMs: 0, files: new Map};
    const rle = set => { const v = [...set].sort((x, y) => x - y), o = []; for (let i = 0; i < v.length;) { let j = i; while (j + 1 < v.length && v[j + 1] === v[j] + 1) ++j; o.push(i === j ? "" + v[i] : v[i] + "-" + v[j]); i = j + 1; } return o.join(","); };
    S.export = () => { const out = {version: 1, blockBytes: 1048576, t: Math.round((performance.now() - S.t0) / 1000), files: {}}; for (const [p, r] of S.files) out.files[p] = {blocks: rle(r.blocks), requests: r.requests, bytes: r.bytes}; return out; };
    S.summary = () => {
      let uniq = 0; const files = [];
      for (const [p, r] of S.files) { uniq += r.blocks.size; files.push({p: p.replace("/game/archive/pc/", ""), req: r.requests, mb: Math.round(r.bytes / 104857.6) / 10, uniqMb: r.blocks.size}); }
      files.sort((x, y) => y.req - x.req);
      return {t: Math.round((performance.now() - S.t0) / 1000), cfg: S.cfg, workers: S.workers.size,
        totals: {requests: S.requests, mb: Math.round(S.bytes / 104857.6) / 10, uniqueMb: uniq, requestMs: Math.round(S.ms), maxRequestMs: Math.round(S.maxMs),
          avgRequestMs: S.requests ? Math.round(S.ms / S.requests) : 0, blockHits: S.hits, reads: S.reads, readStallMs: Math.round(S.readMs)},
        shared: (() => { try { const X = Module["cp2077IoShared"]; return X ? sbcStats(X.c) : null; } catch (e) { return null; } })(), files};
    };
    S.dump = () => { try { console.log("CP2077 ioTrace: " + JSON.stringify(S.summary())); console.log("CP2077 ioTrace ranges: " + JSON.stringify(S.export())); } catch (e) {} };
    setInterval(S.dump, 30000);
    return S;
  })());
  A.workers.add(workerID); A.cfg = d.cfg;
  A.requests += d.requests; A.bytes += d.bytes; A.ms += d.ms; A.hits += d.hits; A.reads += d.reads; A.readMs += d.readMs;
  if (d.maxMs > A.maxMs) A.maxMs = d.maxMs;
  for (const p in d.blocks) {
    let r = A.files.get(p);
    if (!r) A.files.set(p, r = {blocks: new Set, requests: 0, bytes: 0});
    const e = d.blocks[p]; r.requests += e.r; r.bytes += e.b;
    for (const n of e.n) r.blocks.add(n);
  }
}

`;
// Inline every SBC_* constant as a literal: the main thread calls sbcCreate() from PThread.initMainThread(),
// which runs before the injected top-level `var SBC_... = n` statement executes (tc38 crash: SBC_HDR undefined
// -> SharedArrayBuffer(NaN) -> zero-length header -> RangeError in every pthread).
const SHARED_RAW = fs.readFileSync(path.join(CF_SRC, "io3-shared-cache.js"), "utf8");
const SBC_CONSTS = new Map([...SHARED_RAW.matchAll(/\b(SBC_[A-Z_]+) = (\d+)/g)].map(m => [m[1], m[2]]));
if (SBC_CONSTS.size < 20) throw new Error("SBC constants not parsed: " + SBC_CONSTS.size);
const inlineConsts = src => src.replace(/\bSBC_[A-Z_]+\b(?! = \d)/g, name => {
  if (!SBC_CONSTS.has(name)) throw new Error("unknown constant " + name);
  return SBC_CONSTS.get(name) + "/*" + name + "*/";
});
const SHARED_SRC = inlineConsts(SHARED_RAW);
js = js.slice(0, a) + SHARED_SRC + "\n" + inlineConsts(NEW_CPRESREAD) + js.slice(b);
// The base bundle uses CRLF line endings; accept either.
const once = (from, to) => {
  if (js.split(from).length !== 2) { from = from.split("\n").join("\r\n"); to = to.split("\n").join("\r\n"); }
  if (js.split(from).length !== 2) throw new Error("anchor not found exactly once: " + from.slice(0, 60));
  js = js.split(from).join(to);
};

// ---- 2. main-thread handler for worker io trace messages
const faultLine = 'if (cmd === "cp2077NativeFault")';
once(faultLine, 'if (cmd === "cp2077IoTrace") { cp2077IoTraceMain(worker.workerID, d.d); return; }\n      ' + faultLine);
// ---- 2b. [io2] hand the shared cache to each pthread worker as its first message (only with ?ioShared=1)
once("    worker.workerID = PThread.nextWorkerID++;\n    PThread.unusedWorkers.push(worker);",
  "    worker.workerID = PThread.nextWorkerID++;\n    cp2077IoSharedPost(worker);\n    PThread.unusedWorkers.push(worker);");
// ---- 2c. [io2] pthread side: accept it before the Emscripten load command
once("      var cmd = msgData.cmd;\n      if (cmd == 1) {",
  "      var cmd = msgData.cmd;\n      if (cmd === \"cp2077IoShared\") { globalThis.__cpIoShared = msgData.d; return; }\n      if (cmd == 1) {");

// ---- 3. write the new immutable bundle
// The helper is part of the bundle: version the directory by both files.
const newHash = sha(js + "\n/*helper*/\n" + fs.readFileSync(path.join(CF_SRC, "io3-helper.js"), "utf8"));
const outDir = path.join(RUNTIME, "engine-versions", newHash);
fs.mkdirSync(outDir, {recursive: true});
fs.writeFileSync(path.join(outDir, "cp2077.js"), js);
fs.writeFileSync(path.join(outDir, "cp2077.js.gz"), zlib.gzipSync(js, {level: 9}));
fs.writeFileSync(path.join(outDir, "cp2077-io-helper.js"), "// [cloudflare io2] prefetch helper (generated by make-io-bundle3.mjs)\n" + SHARED_SRC + "\n" + inlineConsts(fs.readFileSync(path.join(CF_SRC, "io3-helper.js"), "utf8")));
for (const f of ["cp2077.wasm", "cp2077.data", "cp2077.wasm.gz", "cp2077.data.gz"]) {
  const dst = path.join(outDir, f);
  if (!fs.existsSync(dst) || fs.statSync(dst).size !== fs.statSync(path.join(baseDir, f)).size) fs.copyFileSync(path.join(baseDir, f), dst);
}
const baseBundle = JSON.parse(fs.readFileSync(path.join(baseDir, "bundle.json"), "utf8"));
fs.writeFileSync(path.join(outDir, "bundle.json"), JSON.stringify({BuildID: newHash, Files: {...baseBundle.Files, "cp2077.js": sha(js)},
  Scope: `cloudflare io variant of ${base}: CpResRead switches ioBlockMB/ioCacheMB/ioAhead, io2 shared cache ioShared/ioSharedMB/ioPrefetch/ioInflight + worker ioTrace. wasm/data identical.`, Base: base}, null, 2));

// ---- 4. test page: live page with the new hash; cp2077.js URL carries the page query to the pthreads
const pageSrc = fs.readFileSync(path.join(RUNTIME, "..", "..", "native-browser.html"), "utf8");
if (!pageSrc.includes(base)) throw new Error("native-browser.html does not reference base bundle " + base);
const tag = `<script src="/runtime/engine-versions/${base}/cp2077.js"></script>`;
if (pageSrc.split(tag).length !== 2) throw new Error("engine script tag not found exactly once");
let page = pageSrc.replace(tag, `<script>/* [cloudflare io] switches reach the pthread workers through the script URL */document.write('<script src="/runtime/engine-versions/${newHash}/cp2077.js'+location.search.replace(/[^\\w=&.,%?-]/g,'')+'"><\\/script>')</script>`);
page = page.split(base).join(newHash).replace("<title>CP2077 native browser port</title>", "<title>CP2077 native browser port (cf io3)</title>");
fs.writeFileSync(path.join(RUNTIME, "native-browser-cfio3.html"), page);
console.log(`io bundle ${newHash}\n  base ${base}\n  dir  ${outDir}\n  page ${path.join(RUNTIME, "native-browser-cfio3.html")}`);
