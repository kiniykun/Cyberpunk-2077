// CP2077 browser port delivery Worker: page + immutable engine bundle + original game files from R2,
// all on one origin so COOP/COEP stay trivial. Mirrors D:\2077\serve-engine-assets.py semantics:
//   - single-range 206/416, ETag + If-Match (412) exactly as the runtime's resource filesystem expects,
//   - prepared .gz for immutable engine-versions/<sha>/cp2077.{wasm,js,data} (Content-Encoding: gzip),
//   - COOP same-origin / COEP require-corp / CORP same-origin on every response.
// Game files (/game/...) take size + ETag from the stored manifest runtime/game-files.json, so R2's own
// etags never reach the runtime and full objects, per-block "partial bucket" objects and the dev proxy
// all present identical validators.
// Access gate: shared secret -> HttpOnly cookie. Assets are copyrighted; nothing is served without it.

const GATE_COOKIE = "cp2077_gate";
const DELAY_COOKIE = "cp2077_net_delay";
const MBPS_COOKIE = "cp2077_net_mbps";
const MANIFEST_KEY = "runtime/game-files.json";
const IMMUTABLE_RE = /^runtime\/engine-versions\/[0-9a-f]{64}\/cp2077\.(wasm|js|data)$/;
const PAGE_KEYS = new Set(["native-browser.html", "cf-smoke.html"]);
// [loader io] cp2077.js?loaderio=1 = the unchanged R2 bundle file with a small prelude prepended. Pthread workers
// are spawned from the page's script URL, so they load this variant too. In workers only, the prelude counts
// completed /game/ XHR bytes and publishes them on BroadcastChannel "cp2077-loader-io" (main-thread loading
// screen readout). Posting happens inside XHR.send (busy pthreads never run timers). The R2 object is untouched.
const LOADER_IO_PRELUDE = `(function(){try{if(typeof WorkerGlobalScope==="undefined"||!(self instanceof WorkerGlobalScope)||typeof XMLHttpRequest==="undefined"||typeof BroadcastChannel==="undefined")return;
var bc=new BroadcastChannel("cp2077-loader-io"),id=Math.random().toString(36).slice(2),bytes=0,req=0,lastPost=0,P=XMLHttpRequest.prototype,send=P.send;
function count(x){if(x.__cpc||x.readyState!==4)return;x.__cpc=1;try{if((x.responseURL||"").indexOf("/game/")<0)return;var r=x.response;req++;bytes+=typeof r==="string"?r.length:(r&&r.byteLength)||0;
var now=Date.now();if(now-lastPost>=500){lastPost=now;bc.postMessage({id:id,bytes:bytes,req:req})}}catch(e){}}
P.send=function(){var x=this;try{x.addEventListener("loadend",function(){count(x)})}catch(e){}var v=send.apply(this,arguments);count(x);return v};}catch(e){}})();
`;

const TYPES = {
  html: "text/html; charset=utf-8", js: "text/javascript; charset=utf-8", mjs: "text/javascript; charset=utf-8",
  json: "application/json", wasm: "application/wasm", css: "text/css", txt: "text/plain; charset=utf-8",
  gz: "application/gzip", dll: "application/octet-stream", png: "image/png", svg: "image/svg+xml", woff2: "font/woff2",
};
const contentType = key => TYPES[key.slice(key.lastIndexOf(".") + 1).toLowerCase()] || "application/octet-stream";

// ------------------------------------------------------------------ helpers
function baseHeaders(extra) {
  const h = new Headers(extra);
  h.set("Cross-Origin-Opener-Policy", "same-origin");
  h.set("Cross-Origin-Embedder-Policy", "require-corp");
  h.set("Cross-Origin-Resource-Policy", "same-origin");
  h.set("X-Content-Type-Options", "nosniff");
  return h;
}
const plain = (status, text, extra) => new Response(text, {status, headers: baseHeaders({"Content-Type": "text/plain; charset=utf-8", "Cache-Control": "no-store", ...extra})});

function cookies(request) {
  const out = {};
  for (const part of (request.headers.get("Cookie") || "").split(";")) {
    const i = part.indexOf("=");
    if (i > 0) out[part.slice(0, i).trim()] = part.slice(i + 1).trim();
  }
  return out;
}
function safeEqual(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; ++i) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}
async function sha256Hex(text) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(d)].map(b => b.toString(16).padStart(2, "0")).join("");
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// Inclusive single range, same rules as serve-engine-assets.py selected_range().
function selectRange(header, size) {
  if (header == null) return {first: 0, last: size - 1, partial: false};
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m || size === 0) return null;
  if (!m[1]) {
    const count = Number(m[2] || "0");
    if (!(count > 0)) return null;
    return {first: Math.max(0, size - count), last: size - 1, partial: true};
  }
  const first = Number(m[1]);
  const last = m[2] ? Math.min(Number(m[2]), size - 1) : size - 1;
  if (first >= size || last < first) return null;
  return {first, last, partial: true};
}
function acceptsGzip(header) {
  const q = {};
  for (const item of (header || "").split(",")) {
    const parts = item.trim().toLowerCase().split(";");
    let v = 1;
    for (const p of parts.slice(1)) if (p.trim().startsWith("q=")) v = Number(p.trim().slice(2)) || 0;
    q[parts[0].trim()] = v;
  }
  const v = q.gzip ?? q["*"] ?? 0;
  return v > 0 && v <= 1;
}

// URL path -> R2 key (or null). Only the routes the local server exposes.
function keyFor(pathname) {
  let route;
  try { route = decodeURIComponent(pathname); } catch { return null; }
  if (route.includes("\0") || route.includes("\\") || route.includes(":")) return null;
  if (route.split("/").some(p => p === "." || p === "..")) return null;
  if (route === "/" || route === "/native-browser.html") return "native-browser.html";
  if (route === "/cf-smoke.html") return "cf-smoke.html";
  if (route === "/sdk/bink2w64.dll") return "sdk/bink2w64.dll";
  if (route.startsWith("/runtime/") && route.length > 9) return route.slice(1);
  if (/^\/game\/(archive|engine|r6)\//i.test(route)) return route.slice(1);
  return null;
}

// ------------------------------------------------------------------ manifest (per isolate, 60 s)
let manifestCache = null;
async function manifest(env) {
  if (manifestCache && Date.now() - manifestCache.at < 60000) return manifestCache;
  const obj = await env.BUCKET.get(MANIFEST_KEY);
  let text = obj ? await obj.text() : null;
  if (text == null && env.DEV_MODE === "1" && env.DEV_ORIGIN) {
    const r = await fetch(env.DEV_ORIGIN + "/" + MANIFEST_KEY);
    if (r.ok) text = await r.text();
  }
  const byPath = new Map();
  if (text) for (const e of JSON.parse(text).files || []) byPath.set(e.path, e);
  manifestCache = {at: Date.now(), text, byPath};
  return manifestCache;
}

// ------------------------------------------------------------------ gate
// Public launch (user 2026-10-22: countdown on /game, no access code; playable at 5 pm Central on Oct 22 = 22:00 UTC).
// From LAUNCH_MS on, the path token "play" opens the assets for everyone; before that it answers 403. The access
// code keeps working at any time (testing).
const LAUNCH_MS = Date.parse("2026-10-22T22:00:00Z");
const PUBLIC_TOKEN = "play";
async function gate(request, env, url, pathToken) {
  if (pathToken === PUBLIC_TOKEN)
    return Date.now() >= LAUNCH_MS ? {ok: true} : {deny: new Response("Not launched yet", {status: 403, headers: baseHeaders({"Content-Type": "text/plain", "Cache-Control": "no-store"})})};
  const token = env.ACCESS_TOKEN;
  const dev = env.DEV_MODE === "1";
  if (!token || (!dev && (token.length < 3 || token === "local-dev-gate")))   // user 2026-10-21: short code "123"
    return {deny: plain(503, "Access gate is not configured (wrangler secret put ACCESS_TOKEN).")};
  const expected = await sha256Hex("cp2077-gate:" + token);
  if (pathToken != null)
    return safeEqual(await sha256Hex("cp2077-gate:" + pathToken), expected) ? {ok: true} : {deny: plain(401, "Access denied (bad link key).")};
  const key = url.searchParams.get("key");
  if (key != null) {
    if (!safeEqual(await sha256Hex("cp2077-gate:" + key), expected)) return {deny: denied(request, "Access denied.")};
    const clean = new URL(url);
    clean.searchParams.delete("key");
    const secure = url.protocol === "https:" ? "; Secure" : "";
    const h = baseHeaders({Location: clean.pathname + clean.search + clean.hash, "Cache-Control": "no-store"});
    h.append("Set-Cookie", `${GATE_COOKIE}=${expected}; Path=/; HttpOnly; SameSite=Lax; Max-Age=2592000${secure}`);
    return {deny: new Response(null, {status: 302, headers: h})};
  }
  const auth = request.headers.get("Authorization") || "";
  if (auth.startsWith("Bearer ") && safeEqual(await sha256Hex("cp2077-gate:" + auth.slice(7)), expected)) return {ok: true};
  if (safeEqual(cookies(request)[GATE_COOKIE] || "", expected)) return {ok: true};
  return {deny: denied(request, "Access denied. Open /?key=<access token> once to set the gate cookie.")};
}

// Friendly gate page for browser navigations (status stays 401).
const ACCESS_PAGE = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Access required</title><style>:root{color-scheme:dark}body{margin:0;min-height:100vh;display:grid;place-items:center;
font:16px/1.5 system-ui,sans-serif;background:#111;color:#eee}main{max-width:28rem;padding:2rem 16px;text-align:center}
h1{font-size:1.4rem;margin:0 0 .5rem}p{opacity:.8;margin:.25rem 0}</style></head><body><main><h1>Access required</h1>
<p>This is a private build. Open the full link you were given (it ends in <code>?key=...</code>) once in this browser.</p>
<p>After that, this address works on its own.</p></main></body></html>`;
function denied(request, text) {
  if ((request.headers.get("Accept") || "").includes("text/html"))
    return new Response(ACCESS_PAGE, {status: 401, headers: baseHeaders({"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store"})});
  return plain(401, text);
}

// ------------------------------------------------------------------ gated upload API (separate UPLOAD_TOKEN)
// Used by scripts/upload-multipart.mjs. Bodies stream straight into R2 (never buffered: 128 MB isolate).
//   GET    /__upload/stat?key=K                          -> raw R2 head {exists,size,etag,meta} (NOT the manifest)
//   PUT    /__upload/put?key=K[&mtime=]                   body = whole object (<= ~90 MiB, Content-Length set)
//   POST   /__upload/mpu/create?key=K[&mtime=]            -> {uploadId}
//   PUT    /__upload/mpu/part?key=K&uploadId=U&part=N     body = part bytes -> {partNumber, etag}
//   POST   /__upload/mpu/complete?key=K&uploadId=U        body = [{partNumber,etag}] -> {size, etag}
//   DELETE /__upload/mpu/abort?key=K&uploadId=U
// Disable after the upload by deleting the secret: `wrangler secret delete UPLOAD_TOKEN` (then always 401).
const UPLOAD_KEY_RE = /^(runtime\/|game\/|sdk\/|native-browser\.html$|cf-smoke\.html$)/;
const json = (status, obj) => new Response(JSON.stringify(obj), {status, headers: baseHeaders({"Content-Type": "application/json", "Cache-Control": "no-store"})});
async function uploadApi(request, env, url) {
  const token = env.UPLOAD_TOKEN;
  const given = request.headers.get("X-Upload-Token") || "";
  if (!token || token.length < 24 || !given ||
      !safeEqual(await sha256Hex("cp2077-upload:" + given), await sha256Hex("cp2077-upload:" + token)))
    return json(401, {error: "upload token required"});
  if (url.pathname === "/__upload/probes" && request.method === "GET") {   // admin: list/read device probe reports
    const one = url.searchParams.get("get");
    if (one) { const o = /^probe\/[\w.-]+$/.test(one) ? await env.BUCKET.get(one) : null; return o ? new Response(o.body, {headers: baseHeaders({"Content-Type": "application/json", "Cache-Control": "no-store"})}) : json(404, {error: "no probe"}); }
    const pre = "probe/" + (url.searchParams.get("prefix") || "").replace(/[^\w.-]/g, "");   // [probelist] ?prefix=2026-10-22&cursor=
    const l = await env.BUCKET.list({prefix: pre, limit: 1000, cursor: url.searchParams.get("cursor") || undefined});
    if (url.searchParams.has("prefix") || url.searchParams.has("cursor")) return json(200, {objects: l.objects.map(o => ({key: o.key, size: o.size, uploaded: o.uploaded})), cursor: l.truncated ? l.cursor : null});
    return json(200, l.objects.map(o => ({key: o.key, size: o.size, uploaded: o.uploaded})));
  }
  const key = url.searchParams.get("key") || "";
  if (!UPLOAD_KEY_RE.test(key) || key.length > 1024 || key.split("/").some(p => p === "" || p === "." || p === "..") || /[\\\0:]/.test(key))
    return json(400, {error: "bad key"});
  const op = url.pathname.slice("/__upload/".length), m = request.method;
  const meta = () => ({httpMetadata: {contentType: contentType(key)},
    customMetadata: url.searchParams.has("mtime") ? {srcMtime: url.searchParams.get("mtime")} : {}});
  if (op === "stat" && m === "GET") {
    const h = await env.BUCKET.head(key);
    return json(200, h ? {exists: true, size: h.size, etag: h.etag, meta: h.customMetadata || {}} : {exists: false});
  }
  if (op === "put" && m === "PUT") {
    if (!request.body) return json(400, {error: "no body"});
    const o = await env.BUCKET.put(key, request.body, meta());
    return json(200, {size: o.size, etag: o.etag});
  }
  if (op === "mpu/create" && m === "POST") {
    const u = await env.BUCKET.createMultipartUpload(key, meta());
    return json(200, {uploadId: u.uploadId});
  }
  const uploadId = url.searchParams.get("uploadId");
  if (!uploadId) return json(400, {error: "uploadId required"});
  const mpu = env.BUCKET.resumeMultipartUpload(key, uploadId);
  if (op === "mpu/part" && m === "PUT") {
    const n = Number(url.searchParams.get("part"));
    if (!(Number.isInteger(n) && n >= 1 && n <= 10000) || !request.body) return json(400, {error: "bad part"});
    const p = await mpu.uploadPart(n, request.body);
    return json(200, {partNumber: p.partNumber, etag: p.etag});
  }
  if (op === "mpu/complete" && m === "POST") {
    const o = await mpu.complete(await request.json());
    return json(200, {size: o.size, etag: o.etag});
  }
  if (op === "mpu/abort" && m === "DELETE") { await mpu.abort(); return json(200, {aborted: true}); }
  return json(404, {error: "unknown upload op"});
}

// ------------------------------------------------------------------ cross-origin page (Plan B: page on another site)
// The page at e.g. https://cyberpunk2077.lol/game loads every asset from here under /k/<ACCESS_TOKEN>/<path>
// (token in the PATH so relative module imports keep it; third-party cookies are blocked in Brave).
// CORS is granted only to CORS_ORIGINS (+ CORS_TEST_ORIGINS, temporary). /k/ responses get CORP cross-origin.
function corsOrigins(env) {
  return new Set(`${env.CORS_ORIGINS || ""},${env.CORS_TEST_ORIGINS || ""}`.split(",").map(s => s.trim()).filter(Boolean));
}
const CORS_EXPOSE = "Content-Range, ETag, Content-Length, Content-Encoding, Accept-Ranges";
function addCors(response, origin, crossPath) {
  const set = h => {
    if (crossPath) h.set("Cross-Origin-Resource-Policy", "cross-origin");
    if (origin) {
      h.set("Access-Control-Allow-Origin", origin);
      h.set("Access-Control-Allow-Credentials", "true");
      h.set("Access-Control-Expose-Headers", CORS_EXPOSE);
      h.append("Vary", "Origin");
    }
  };
  try { set(response.headers); return response; }
  catch { const r = new Response(response.body, response); set(r.headers); return r; }
}
function preflight(origin) {
  return new Response(null, {status: 204, headers: baseHeaders({
    "Access-Control-Allow-Origin": origin, "Access-Control-Allow-Credentials": "true",
    "Access-Control-Allow-Methods": "GET, HEAD", "Access-Control-Allow-Headers": "Range, If-Match",
    "Access-Control-Max-Age": "86400", "Cross-Origin-Resource-Policy": "cross-origin", "Vary": "Origin", "Cache-Control": "no-store"})});
}

// ------------------------------------------------------------------ simulated WAN
function netProfile(request, env, url, key) {
  const c = cookies(request);
  let delay = Number(env.NET_DELAY_MS || 0) || 0, mbps = Number(env.NET_MBPS || 0) || 0;
  const setCookies = [];
  if (env.DEV_MODE === "1") {
    if (c[DELAY_COOKIE] != null) delay = Number(c[DELAY_COOKIE]) || 0;
    if (c[MBPS_COOKIE] != null) mbps = Number(c[MBPS_COOKIE]) || 0;
    if (PAGE_KEYS.has(key) || key.endsWith(".html")) {
      for (const [param, name, setter] of [["netDelay", DELAY_COOKIE, v => (delay = v)], ["netMbps", MBPS_COOKIE, v => (mbps = v)]]) {
        if (!url.searchParams.has(param)) continue;
        const v = Math.max(0, Math.min(10000, Number(url.searchParams.get(param)) || 0));
        setter(v);
        setCookies.push(v ? `${name}=${v}; Path=/; SameSite=Strict` : `${name}=; Path=/; Max-Age=0; SameSite=Strict`);
      }
    }
  }
  return {delay, mbps, setCookies};
}

// ------------------------------------------------------------------ R2 / partial / proxy readers
// Returns {body, source} for [first,last] of a manifest game file, or null if no stored bytes exist.
async function readGameRange(env, key, entry, first, last) {
  const length = last - first + 1;
  const obj = await env.BUCKET.get(key, {range: {offset: first, length}});
  if (obj) {
    if (obj.size !== entry.size) throw new Error("R2 object size differs from manifest: " + key);
    return {body: obj.body, source: "r2"};
  }
  // Partial bucket: blocks/<key>/<n>, each PARTIAL_BLOCK_BYTES of the original file.
  const B = Number(env.PARTIAL_BLOCK_BYTES || 1048576);
  const b0 = Math.floor(first / B), b1 = Math.floor(last / B);
  const blockKey = n => `blocks/${key}/${n}`;
  if (b0 === b1) {
    const part = await env.BUCKET.get(blockKey(b0), {range: {offset: first - b0 * B, length}});
    return part ? {body: part.body, source: "block"} : null;
  }
  for (let n = b0; n <= b1; ++n) if (!(await env.BUCKET.head(blockKey(n)))) return null;
  const {readable, writable} = new FixedLengthStream(length);
  (async () => {
    try {
      for (let n = b0; n <= b1; ++n) {
        const s = Math.max(first, n * B) - n * B, e = Math.min(last, n * B + B - 1) - n * B;
        const part = await env.BUCKET.get(blockKey(n), {range: {offset: s, length: e - s + 1}});
        if (!part) throw new Error("block vanished " + blockKey(n));
        await part.body.pipeTo(writable, {preventClose: true});
      }
      await writable.close();
    } catch (err) { await writable.abort(err); }
  })();
  return {body: readable, source: "blocks"};
}

async function proxy(env, url, rangeHeader, method) {
  if (env.DEV_MODE !== "1" || !env.DEV_ORIGIN) return null;
  const headers = {"Accept-Encoding": "identity"};
  if (rangeHeader) headers.Range = rangeHeader;
  const r = await fetch(env.DEV_ORIGIN + url.pathname, {method, headers});
  if (r.status === 404) return null;
  return r;
}

// ------------------------------------------------------------------ handlers
async function serveGameFile(request, env, ctx, url, key, entry, log) {
  const size = entry.size, etag = entry.etag;
  const ifMatch = request.headers.get("If-Match");
  if (ifMatch != null && ifMatch !== etag && ifMatch !== "*") return plain(412, "");
  const rangeHeader = request.headers.get("Range");
  const range = selectRange(rangeHeader, size);
  if (!range) return new Response(null, {status: 416, headers: baseHeaders({"Content-Range": `bytes */${size}`, "Content-Length": "0"})});
  const {first, last, partial} = range;
  const length = Math.max(0, last - first + 1);
  const headers = baseHeaders({
    "Content-Type": "application/octet-stream", "Accept-Ranges": "bytes", "ETag": etag,
    "Cache-Control": "no-cache", "Content-Length": String(length),
  });
  if (partial) headers.set("Content-Range", `bytes ${first}-${last}/${size}`);
  const status = partial ? 206 : 200;
  if (request.method === "HEAD") return new Response(null, {status, headers});

  // Edge block cache (production): Cache API needs 200 responses, so store bare bytes per exact range.
  const useCache = env.EDGE_BLOCK_CACHE === "1" && partial && length <= 8 * 1048576 && typeof caches !== "undefined";
  const cacheKey = useCache ? new Request(new URL(`/__blockcache/${key}?e=${encodeURIComponent(etag)}&r=${first}-${last}`, url).toString()) : null;
  if (useCache) {
    const hit = await caches.default.match(cacheKey);
    if (hit) { log.source = "edge-cache"; return new Response(hit.body, {status, headers}); }
  }
  let got = await readGameRange(env, key, entry, first, last);
  if (!got) {
    const r = await proxy(env, url, partial ? `bytes=${first}-${last}` : null, "GET");
    if (!r) return plain(404, "Game file bytes are not in this bucket: " + url.pathname);
    const total = /\/(\d+)$/.exec(r.headers.get("Content-Range") || "")?.[1];
    if (r.status !== status || (partial && Number(total) !== size)) return plain(502, "Dev origin returned a different file identity");
    got = {body: r.body, source: "proxy"};
  }
  log.source = got.source;
  if (useCache) {
    const bytes = await new Response(got.body).arrayBuffer();
    if (bytes.byteLength !== length) return plain(502, "Short read from storage; retry");   // [player-fix] never cache a short block
    ctx.waitUntil(caches.default.put(cacheKey, new Response(bytes, {headers: {"Cache-Control": "public, max-age=604800", "Content-Type": "application/octet-stream"}})));
    return new Response(bytes, {status, headers});
  }
  return new Response(got.body, {status, headers});
}

async function serveObject(request, env, url, key, log) {
  const immutable = IMMUTABLE_RE.test(key);
  const rangeHeader = request.headers.get("Range");
  const headers = baseHeaders({
    "Content-Type": contentType(key), "Accept-Ranges": "bytes",
    "Cache-Control": immutable ? "public, max-age=31536000, immutable" : (key === MANIFEST_KEY ? "no-store" : "no-cache"),
  });
  if (immutable) headers.set("Vary", "Accept-Encoding");
  if (immutable && key.endsWith("/cp2077.js") && url.searchParams.get("loaderio") === "1") {
    const obj = await env.BUCKET.get(key);
    if (!obj) return plain(404, "No object at this path");
    headers.set("ETag", `"loaderio-${obj.etag}"`);
    headers.delete("Accept-Ranges");
    log.source = "r2-loaderio";
    return new Response(request.method === "HEAD" ? null : LOADER_IO_PRELUDE + await obj.text(), {status: 200, headers});
  }

  // Prepared gzip for immutable engine files (never with Range: offsets refer to identity bytes).
  if (immutable && rangeHeader == null && acceptsGzip(request.headers.get("Accept-Encoding"))) {
    const gz = request.method === "HEAD" ? await env.BUCKET.head(key + ".gz") : await env.BUCKET.get(key + ".gz");
    if (gz) {
      const ifMatch = request.headers.get("If-Match");
      if (ifMatch != null && ifMatch !== gz.httpEtag && ifMatch !== "*") return plain(412, "");
      headers.set("ETag", gz.httpEtag);
      headers.set("Content-Encoding", "gzip");
      headers.set("Content-Length", String(gz.size));
      log.source = "r2-gz";
      // encodeBody "manual": body is already gzip; the runtime must not compress it again.
      return new Response(request.method === "HEAD" ? null : gz.body, {status: 200, headers, encodeBody: "manual"});
    }
  }

  const head = await env.BUCKET.head(key);
  if (!head) {
    const r = await proxy(env, url, rangeHeader, request.method);
    if (!r) return plain(404, "No object at this path");
    log.source = "proxy";
    for (const name of ["ETag", "Content-Range", "Content-Length", "Content-Type"]) {
      const v = r.headers.get(name);
      if (v != null) headers.set(name, v);
    }
    return new Response(r.body, {status: r.status, headers});
  }
  const ifMatch = request.headers.get("If-Match");
  if (ifMatch != null && ifMatch !== head.httpEtag && ifMatch !== "*") return plain(412, "");
  const range = selectRange(rangeHeader, head.size);
  if (!range) return new Response(null, {status: 416, headers: baseHeaders({"Content-Range": `bytes */${head.size}`, "Content-Length": "0"})});
  const {first, last, partial} = range;
  const length = Math.max(0, last - first + 1);
  headers.set("ETag", head.httpEtag);
  headers.set("Content-Length", String(length));
  if (partial) headers.set("Content-Range", `bytes ${first}-${last}/${head.size}`);
  const status = partial ? 206 : 200;
  log.source = "r2";
  if (request.method === "HEAD") return new Response(null, {status, headers});
  const obj = await env.BUCKET.get(key, partial ? {range: {offset: first, length}} : undefined);
  if (!obj) return plain(404, "Object vanished");
  return new Response(obj.body, {status, headers});
}

export default {
  async fetch(request, env, ctx) {
    const began = Date.now();
    let url = new URL(request.url);
    const log = {source: "-"};
    let response;
    const reqOrigin = request.headers.get("Origin");
    const corsOrigin = reqOrigin && corsOrigins(env).has(reqOrigin) ? reqOrigin : null;
    let pathToken = null;
    if (url.pathname.startsWith("/k/")) {
      const end = url.pathname.indexOf("/", 3);
      if (end > 3) {
        try { pathToken = decodeURIComponent(url.pathname.slice(3, end)); } catch { pathToken = ""; }
        url = new URL(url); url.pathname = url.pathname.slice(end);
      }
    }
    try {
      if (request.method === "OPTIONS") response = corsOrigin ? preflight(corsOrigin) : plain(405, "Method not allowed");
      else if (url.pathname === "/__probe" && request.method === "POST") {
        // Device capability report from capcheck.js (unsupported browsers). Small JSON only; one object per id.
        const body = await request.text();
        const id = (url.searchParams.get("id") || "").replace(/[^a-z0-9]/g, "").slice(0, 12) || "x";
        if (body.length > 65536 || !body.startsWith("{")) response = plain(400, "bad probe");
        else {
          await env.BUCKET.put(`probe/${new Date().toISOString().slice(0, 10)}-${id}.json`, body, {httpMetadata: {contentType: "application/json"}});
          { const h = baseHeaders(); h.set("Cross-Origin-Resource-Policy", "cross-origin"); response = new Response(null, {status: 204, headers: h}); }   // CORP: the COEP page posts this no-cors
        }
      }
      else if (url.pathname === "/__launch") {   // server clock for the /game countdown
        response = new Response(JSON.stringify({now: Date.now(), launch: LAUNCH_MS}), {headers: baseHeaders({"Content-Type": "application/json", "Cache-Control": "no-store"})});
        response = addCors(response, corsOrigin, true);
      }
      else if (url.pathname.startsWith("/__upload/") && pathToken == null) {
        try { response = await uploadApi(request, env, url); }
        catch (err) { response = json(500, {error: String(err && err.message || err)}); }
      } else if (request.method !== "GET" && request.method !== "HEAD") response = plain(405, "Method not allowed");
      else {
        const g = await gate(request, env, url, pathToken);
        if (g.deny) response = g.deny;
        else {
          const key = keyFor(url.pathname);
          if (!key) response = plain(404, "No asset or compiled runtime at this path");
          else {
            const net = netProfile(request, env, url, key);
            const m = key.startsWith("game/") || key === MANIFEST_KEY ? await manifest(env) : null;
            if (key === MANIFEST_KEY && m.text != null) {
              log.source = "manifest";
              response = new Response(request.method === "HEAD" ? null : m.text, {headers: baseHeaders({"Content-Type": "application/json", "Cache-Control": "no-store"})});
            } else if (m && m.byPath.has("/" + key)) {
              response = await serveGameFile(request, env, ctx, url, key, m.byPath.get("/" + key), log);
            } else {
              response = await serveObject(request, env, url, key, log);
            }
            if (net.setCookies.length) {
              response = new Response(response.body, response);
              for (const c of net.setCookies) response.headers.append("Set-Cookie", c);
            }
            // Simulated WAN: one round trip plus optional bandwidth time, before the first byte.
            const bytes = Number(response.headers.get("Content-Length") || 0);
            const wait = net.delay + (net.mbps > 0 ? (bytes * 8) / (net.mbps * 1000) : 0);
            if (wait > 0) await sleep(wait);
          }
        }
      }
    } catch (err) {
      console.error("[cf] error", url.pathname, err && err.stack || err);
      response = plain(500, "Worker error");
    }
    if (corsOrigin || pathToken != null) response = addCors(response, corsOrigin, pathToken != null);
    if (env.TRACE_LOG === "1")
      console.log(`[cf] ${response.status} ${log.source} ${request.method} ${url.pathname} ${request.headers.get("Range") || ""} ${Date.now() - began}ms`);
    return response;
  },
};
