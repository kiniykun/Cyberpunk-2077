// Resumable uploader to the PROD bucket through the Worker's gated /__upload/ API (src/worker.js).
// No wrangler 300 MiB cap: objects <= PART_MB go as one PUT, bigger ones as R2 multipart (PART_MB parts).
//   node scripts/upload-multipart.mjs                 # upload everything in out/upload-list.json, priority order
//   node scripts/upload-multipart.mjs --dry           # print the plan (order + skip decisions), upload nothing
//   options: --list <file>  --only <regex>  --skip <regex>  --streams 4  --part-mb 90  --verify (stat-only report)
// Reads WORKER_URL + UPLOAD_TOKEN from .secrets.local.json (never printed). Logs to out/upload-progress.log.
// Resume: rerun the same command. Finished objects are skipped by raw R2 size (+ source mtime metadata);
// in-flight multipart uploads continue from out/upload-state.json (uploadId + finished part etags).
// R2-only overrides: out/r2-overrides/<key> replaces the local file for that key (e.g. telemetry stub).
import fs from "node:fs";
import path from "node:path";
import {CF_ROOT, OUT, MiB} from "./common.mjs";

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const has = n => process.argv.includes(n);
const secrets = JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8"));
const BASE = secrets.WORKER_URL, TOKEN = secrets.UPLOAD_TOKEN;
const STREAMS = Math.max(1, Math.min(4, Number(arg("--streams", 4))));
const PART = Number(arg("--part-mb", 90)) * MiB;
const DRY = has("--dry"), VERIFY = has("--verify");
const listFile = arg("--list", path.join(OUT, "upload-list.json"));
const only = arg("--only") ? new RegExp(arg("--only")) : null, skipRe = arg("--skip") ? new RegExp(arg("--skip")) : null;
const STATE = path.join(OUT, "upload-state.json"), LOG = path.join(OUT, "upload-progress.log");
const OVERRIDES = path.join(OUT, "r2-overrides");

const log = msg => { const line = `${new Date().toISOString()} ${msg}`; console.log(line); fs.appendFileSync(LOG, line + "\n"); };
const sleep = ms => new Promise(r => setTimeout(r, ms));
let state = fs.existsSync(STATE) ? JSON.parse(fs.readFileSync(STATE, "utf8")) : {};
function saveState() { fs.writeFileSync(STATE + ".tmp", JSON.stringify(state)); fs.renameSync(STATE + ".tmp", STATE); }

// ---------------------------------------------------------------- plan / order
const list = JSON.parse(fs.readFileSync(listFile, "utf8"));
const PRIORITY_ARCHIVES = [/basegame_4_gamedata/, /basegame_zz_nomad\.archive/, /basegame_zz_nomad_terrain/, /basegame_1_engine/,
  /basegame_5_video/, /basegame_4_animation/, /basegame_2_mainmenu/, /lang_en_text/, /basegame_zz_sandbox/];
function rank(it) {
  if (it.group === "engine-io") return [9, 0];                    // other agent's variant bundle: may change, goes last
  if (it.group !== "archive") return [0, 0];                      // page, engine, runtime, codec, video, manifest, caches
  const i = PRIORITY_ARCHIVES.findIndex(re => re.test(it.key));
  if (i >= 0) return [1, i];
  if (it.size < 300 * MiB) return [1, 99];                        // tiny leftovers ride along with the session set
  return [2, 0];
}
const items = list.items
  .filter(it => (!only || only.test(it.key)) && (!skipRe || !skipRe.test(it.key)))
  .map((it, idx) => {
    const ov = path.join(OVERRIDES, ...it.key.split("/"));
    const file = fs.existsSync(ov) ? ov : it.file;
    const st = fs.statSync(file);
    return {...it, file, size: st.size, mtime: String(Math.round(st.mtimeMs)), override: file === ov, idx, rank: rank(it)};
  })
  .sort((a, b) => a.rank[0] - b.rank[0] || a.rank[1] - b.rank[1] || a.idx - b.idx);

// ---------------------------------------------------------------- HTTP
async function api(method, op, params, body, {tries = 6, timeoutMs = 20 * 60000} = {}) {
  const u = new URL(BASE + "/__upload/" + op);
  for (const [k, v] of Object.entries(params)) u.searchParams.set(k, v);
  let last;
  for (let t = 1; t <= tries; ++t) {
    try {
      const r = await fetch(u, {method, body, headers: {"X-Upload-Token": TOKEN}, signal: AbortSignal.timeout(timeoutMs)});
      const text = await r.text();
      let j; try { j = JSON.parse(text); } catch { j = {error: text.slice(0, 200)}; }
      if (r.ok) return j;
      last = new Error(`${method} ${op} ${params.key} -> ${r.status} ${j.error || ""}`);
      last.status = r.status; last.body = j;
      if (r.status === 401 || r.status === 400) throw last;          // not retryable
    } catch (e) {
      last = e;
      if (e.status === 401 || e.status === 400) throw e;
    }
    if (t < tries) { const w = Math.min(60000, 2000 * 2 ** (t - 1)); log(`  retry ${t}/${tries - 1} in ${w / 1000}s: ${last.message}`); await sleep(w); }
  }
  throw last;
}
const stat = key => api("GET", "stat", {key});
function readSlice(file, offset, length) {
  const fd = fs.openSync(file, "r");
  try { const b = Buffer.allocUnsafe(length); let got = 0; while (got < length) { const n = fs.readSync(fd, b, got, length - got, offset + got); if (!n) throw new Error("short read " + file); got += n; } return b; }
  finally { fs.closeSync(fd); }
}
async function done(it) {
  const s = await stat(it.key);
  return s.exists && s.size === it.size && (!s.meta?.srcMtime || s.meta.srcMtime === it.mtime);
}

// ---------------------------------------------------------------- transfer
let sentBytes = 0, doneBytes = 0;
const t0 = Date.now();
const total = items.reduce((a, it) => a + it.size, 0);
const gb = n => (n / 1e9).toFixed(2);
function progress() {
  const s = (Date.now() - t0) / 1000, rate = sentBytes / Math.max(1, s);
  const left = total - doneBytes;
  return `[${gb(doneBytes)}/${gb(total)} GB, ${(rate * 8 / 1e6).toFixed(1)} Mbit/s, ETA ${rate > 0 ? (left / rate / 3600).toFixed(1) + " h" : "?"}]`;
}

async function putSmall(it) {
  await api("PUT", "put", {key: it.key, mtime: it.mtime}, readSlice(it.file, 0, it.size));
  sentBytes += it.size;
}

async function putMultipart(it) {
  let st = state[it.key];
  if (st && (st.size !== it.size || st.mtime !== it.mtime || st.partSize !== PART)) {
    log(`  source changed / part size changed, aborting old upload for ${it.key}`);
    await api("DELETE", "mpu/abort", {key: it.key, uploadId: st.uploadId}, undefined, {tries: 2}).catch(() => {});
    st = null;
  }
  if (!st) {
    const {uploadId} = await api("POST", "mpu/create", {key: it.key, mtime: it.mtime});
    st = state[it.key] = {uploadId, size: it.size, mtime: it.mtime, partSize: PART, parts: {}};
    saveState();
  }
  const nParts = Math.ceil(it.size / PART);
  const todo = [];
  for (let n = 1; n <= nParts; ++n) if (!st.parts[n]) todo.push(n);
  if (todo.length < nParts) log(`  resuming ${it.key}: ${nParts - todo.length}/${nParts} parts already uploaded`);
  for (let n = 1; n <= nParts; ++n) if (st.parts[n]) doneBytes += Math.min(PART, it.size - (n - 1) * PART);
  let expired = false;
  async function lane() {
    while (todo.length && !expired) {
      const n = todo.shift();
      const off = (n - 1) * PART, len = Math.min(PART, it.size - off);
      try {
        const r = await api("PUT", "mpu/part", {key: it.key, uploadId: st.uploadId, part: String(n)}, readSlice(it.file, off, len));
        st.parts[n] = r.etag; saveState();
        sentBytes += len; doneBytes += len;
        log(`  ${it.key} part ${n}/${nParts} ok ${progress()}`);
      } catch (e) {
        if (/NoSuchUpload|upload.*(not|does not) exist|10024/i.test(e.message)) { expired = true; return; }
        throw e;
      }
    }
  }
  await Promise.all(Array.from({length: STREAMS}, lane));
  if (expired) {
    log(`  multipart upload expired for ${it.key}; starting over`);
    for (let n = 1; n <= nParts; ++n) if (st.parts[n]) doneBytes -= Math.min(PART, it.size - (n - 1) * PART);
    delete state[it.key]; saveState();
    return putMultipart(it);
  }
  const parts = Object.entries(st.parts).map(([n, etag]) => ({partNumber: Number(n), etag})).sort((a, b) => a.partNumber - b.partNumber);
  try {
    await api("POST", "mpu/complete", {key: it.key, uploadId: st.uploadId}, JSON.stringify(parts), {tries: 3});
  } catch (e) {
    if (await done(it)) { log(`  complete reported an error but the object is whole: ${it.key}`); }
    else throw e;
  }
  delete state[it.key]; saveState();
  doneBytes -= it.size;                                            // re-added by caller
}

async function uploadOne(it) {
  if (await done(it)) { doneBytes += it.size; log(`skip (already in R2) ${it.key} ${progress()}`); return; }
  const big = it.size > PART;
  log(`upload ${big ? "multipart" : "put"} ${it.key} ${(it.size / MiB).toFixed(1)} MiB${it.override ? " (R2 override)" : ""}`);
  if (big) await putMultipart(it); else await putSmall(it);
  const s = await stat(it.key);
  if (!s.exists || s.size !== it.size) throw new Error(`size check failed for ${it.key}: R2 ${s.size} vs local ${it.size}`);
  doneBytes += it.size;
  log(`done ${it.key} (verified ${s.size} bytes) ${progress()}`);
}

// ---------------------------------------------------------------- main
log(`=== upload start: ${items.length} objects, ${gb(total)} GB, ${STREAMS} streams, ${PART / MiB} MiB parts, list ${path.basename(listFile)}${DRY ? " (DRY)" : ""}${VERIFY ? " (VERIFY)" : ""}`);
if (DRY || VERIFY) {
  let have = 0, haveBytes = 0;
  for (const it of items) {
    const ok = await done(it); if (ok) { ++have; haveBytes += it.size; }
    log(`${ok ? "HAVE" : "MISS"} r${it.rank.join(".")} ${(it.size / MiB).toFixed(1).padStart(9)} MiB ${it.key}${it.override ? " (override)" : ""}`);
  }
  log(`=== ${have}/${items.length} objects present, ${gb(haveBytes)}/${gb(total)} GB`);
  process.exit(0);
}
let failed = [];
// Small objects: up to STREAMS in parallel. Multipart objects: one at a time, STREAMS parts in flight.
let i = 0;
while (i < items.length) {
  if (items[i].size > PART) {
    const it = items[i++];
    try { await uploadOne(it); } catch (e) { failed.push(it.key); log(`FAILED ${it.key}: ${e.message}`); }
    continue;
  }
  const batch = [];
  while (i < items.length && items[i].size <= PART) batch.push(items[i++]);
  let j = 0;
  await Promise.all(Array.from({length: STREAMS}, async () => {
    while (j < batch.length) {
      const it = batch[j++];
      try { await uploadOne(it); } catch (e) { failed.push(it.key); log(`FAILED ${it.key}: ${e.message}`); }
    }
  }));
}
log(`=== upload end: ${failed.length ? "FAILED " + failed.length + ": " + failed.join(", ") : "ALL OK"} ${progress()}`);
process.exitCode = failed.length ? 1 : 0;
