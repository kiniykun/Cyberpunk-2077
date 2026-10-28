// Build the minimal R2 upload list: page, ONLY the active engine bundle, runtime helper scripts,
// codec files the runtime actually loads, the manifest, the r6/engine caches and the archives.
// Writes out/upload-list.json + out/game-files.json and prints size / estimated R2 cost.
// Usage: node scripts/build-upload-list.mjs [--no-archives]
import fs from "node:fs";
import path from "node:path";
import {PORT_ROOT, RUNTIME, DEPOT, OUT, CF_ROOT, isFile, walk, posix, fmt, activeBundle, MiB} from "./common.mjs";
import {buildManifest} from "./build-manifest.mjs";

const noArchives = process.argv.includes("--no-archives");
// --no-game: skip every /game/ file (use with a partial list from trace-to-ranges.mjs); --out <file>
const noGame = process.argv.includes("--no-game");
const outArg = process.argv.indexOf("--out") > 0 ? process.argv[process.argv.indexOf("--out") + 1] : null;
const items = [];
const add = (key, file, group) => {
  if (!isFile(file)) throw new Error(`missing ${file} for ${key}`);
  items.push({key, file, size: fs.statSync(file).size, group});
};

// 1. pages
add("native-browser.html", path.join(PORT_ROOT, "native-browser.html"), "page");
add("cf-smoke.html", path.join(CF_ROOT, "public", "cf-smoke.html"), "page");

// 2. the active engine bundle only (+ prepared gzip siblings)
const hash = activeBundle();
const bundleDir = path.join(RUNTIME, "engine-versions", hash);
for (const name of ["bundle.json", "cp2077.js", "cp2077.wasm", "cp2077.data", "cp2077.js.gz", "cp2077.wasm.gz", "cp2077.data.gz"])
  add(`runtime/engine-versions/${hash}/${name}`, path.join(bundleDir, name), "engine");

// 3. runtime helper scripts referenced by the page (+ the opt-in trace hook)
const page = fs.readFileSync(path.join(PORT_ROOT, "native-browser.html"), "utf8");
const pageRefs = new Set([...page.matchAll(/["'](\/runtime\/[A-Za-z0-9_.\-/]+\.m?js)["']/g)].map(m => m[1].slice(9)).filter(r => !r.startsWith("engine-versions/")));
pageRefs.add("asset-trace.js");
pageRefs.add("block-fetch.js");
for (const rel of [...pageRefs].sort()) add("runtime/" + rel, path.join(RUNTIME, rel), "runtime");

// 3b. cloudflare io variants (make-io-bundle.mjs -> cfio, make-io-bundle2.mjs -> cfio2): test page + bundle, if present
for (const pageName of ["native-browser-cfio.html", "native-browser-cfio2.html", "native-browser-cfio3.html", "native-browser-cfio4.html", "native-browser-cfio5.html", "native-browser-cfio6.html", "native-browser-cfio7.html"]) {
  const ioPage = path.join(RUNTIME, pageName);
  if (!isFile(ioPage)) continue;
  const ioHash = /engine-versions\/([0-9a-f]{64})\//.exec(fs.readFileSync(ioPage, "utf8"))?.[1];
  if (!ioHash || ioHash === hash) continue;
  add("runtime/" + pageName, ioPage, "page");
  const ioDir = path.join(RUNTIME, "engine-versions", ioHash);
  for (const name of ["bundle.json", "cp2077.js", "cp2077.wasm", "cp2077.data", "cp2077.js.gz", "cp2077.wasm.gz", "cp2077.data.gz", "cp2077-io-helper.js"])
    if (name !== "cp2077-io-helper.js" || isFile(path.join(ioDir, name)))
      add(`runtime/engine-versions/${ioHash}/${name}`, path.join(ioDir, name), "engine-io");
}

// 4. codec: Oodle pool + Bink worker modules (no *-bak, no .o3/.pre-bulk variants, no lossless-sample)
const codec = path.join(RUNTIME, "codec");
for (const f of ["oodle-decoder-pool.mjs", "oodle-decoder-pool-worker.mjs", "oodle-decoder-worker.mjs", "oodle-decoder.mjs", "oodle-decoder.wasm"])
  add("runtime/codec/" + f, path.join(codec, f), "codec");
const bink = path.join(codec, "bink");
for (const f of ["bink-decoder-worker.js", "unicorn_x86.js", "bink-sdk-vm.mjs", "bink-platform-web.mjs", "bink-framebuffers-web.mjs",
  "bink-memory-client-web.mjs", "cached-bink-movie-web.mjs", "predecoded-bink-frames-web.mjs", "lossless-browser-bink-frames-web.mjs"])
  add("runtime/codec/bink/" + f, path.join(bink, f), "codec");
// Videos: cached-bink-movie-web.mjs picks lossless (frames.ivf + predecoded manifest) for a fixed set,
// predecoded (frames.gzpack) for the rest, with no fallback. Parse that set instead of hardcoding it.
const movieSrc = fs.readFileSync(path.join(bink, "cached-bink-movie-web.mjs"), "utf8");
const losslessBlock = /losslessAssets\s*=\s*new Set\(\[([\s\S]*?)\]\)/.exec(movieSrc);
if (!losslessBlock) throw new Error("could not parse losslessAssets in cached-bink-movie-web.mjs");
const lossless = new Set([...losslessBlock[1].matchAll(/[0-9a-f]{64}/g)].map(m => m[0]));
for (const h of fs.readdirSync(path.join(bink, "predecoded")).filter(h => /^[0-9a-f]{64}$/.test(h)).sort()) {
  add(`runtime/codec/bink/predecoded/${h}/manifest.json`, path.join(bink, "predecoded", h, "manifest.json"), "video");
  if (lossless.has(h)) {
    add(`runtime/codec/bink/lossless/${h}/manifest.json`, path.join(bink, "lossless", h, "manifest.json"), "video");
    add(`runtime/codec/bink/lossless/${h}/frames.ivf`, path.join(bink, "lossless", h, "frames.ivf"), "video");
  } else add(`runtime/codec/bink/predecoded/${h}/frames.gzpack`, path.join(bink, "predecoded", h, "frames.gzpack"), "video");
}
// The verified Bink SDK binary the runtime fetches from /sdk/ (read from the depot, never executed).
add("sdk/bink2w64.dll", path.join(DEPOT, "dev", "external", "bink2", "lib", "x64_DLL.Release", "bink2w64.dll"), "codec");

// 5. manifest (stored object) + game files
const {manifest, sources} = buildManifest();
fs.mkdirSync(OUT, {recursive: true});
fs.writeFileSync(path.join(OUT, "game-files.json"), JSON.stringify(manifest));
add("runtime/game-files.json", path.join(OUT, "game-files.json"), "manifest");
for (const {path: p, file} of sources) {
  const group = p.startsWith("/game/archive/") ? "archive" : "cache";
  if ((noArchives && group === "archive") || noGame) continue;
  add(p.slice(1), file, group);
}

// ---- report
fs.writeFileSync((outArg || path.join(OUT, "upload-list.json")), JSON.stringify({bundle: hash, bucket: "cp2077-assets", generated: new Date().toISOString(), items}, null, 1));
const by = {};
for (const it of items) (by[it.group] ||= {n: 0, bytes: 0}), by[it.group].n++, by[it.group].bytes += it.size;
const total = items.reduce((a, it) => a + it.size, 0);
console.log(`upload list: ${items.length} objects, ${fmt(total)} (bundle ${hash.slice(0, 12)})`);
for (const [g, v] of Object.entries(by)) console.log(`  ${g.padEnd(9)} ${String(v.n).padStart(3)} objects  ${fmt(v.bytes)}`);
const big = items.filter(it => it.size > 300 * MiB);
console.log(`  ${big.length} objects > 300 MiB need S3 multipart (rclone); ${items.length - big.length} go through wrangler`);
// R2 Standard list prices (verify: https://developers.cloudflare.com/r2/pricing/): $0.015/GB-month storage
// after 10 GB-month free; Class A $4.50/M (writes; each multipart part counts); Class B $0.36/M after 10M free; egress free.
const gbMonth = total / 1e9;
const storage = Math.max(0, gbMonth - 10) * 0.015;
const parts = big.reduce((a, it) => a + Math.ceil(it.size / (100 * MiB)) + 2, 0) + (items.length - big.length);
console.log(`estimated R2 cost: storage ~$${storage.toFixed(2)}/month (${gbMonth.toFixed(1)} GB-month, first 10 free); ` +
  `upload ~${parts} Class A ops (~$${(parts * 4.5e-6).toFixed(4)}, inside 1M/month free); ` +
  `reads: 1 Class B op per 1 MiB block request (~$0.36 per million after 10M/month free); egress $0. ` +
  `Workers: every block is also a Worker request (free plan 100k/day; paid $5/month incl. 10M). Verify prices on the pricing pages.`);
console.log(`wrote ${(outArg || path.join(OUT, "upload-list.json"))}`);
