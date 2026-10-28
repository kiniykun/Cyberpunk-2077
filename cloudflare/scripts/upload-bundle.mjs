// Upload one engine bundle dir to R2 so the live site can boot it with ?engine=<sha>:
//   node scripts/upload-bundle.mjs <64-hex sha> [--dry]
// Source: D:\2077\build\game\engine-versions\<sha>\ — bundle.json, cp2077.{js,wasm,data} + .gz siblings,
// cp2077-io-helper.js if present. Uses upload-multipart.mjs (skips objects already in R2 at the same size and
// source mtime; multipart for > 90 MiB; verifies each object's size). Then checks the files via the Worker.
import fs from "node:fs";
import path from "node:path";
import {spawnSync} from "node:child_process";
import {CF_ROOT, RUNTIME, OUT} from "./common.mjs";

const sha = process.argv[2];
if (!/^[0-9a-f]{64}$/.test(sha || "")) { console.error("usage: node scripts/upload-bundle.mjs <64-hex bundle sha> [--dry]"); process.exit(2); }
const dir = path.join(RUNTIME, "engine-versions", sha);
if (!fs.existsSync(path.join(dir, "bundle.json"))) { console.error("no bundle.json in " + dir); process.exit(2); }
const required = ["bundle.json", "cp2077.js", "cp2077.wasm", "cp2077.data", "cp2077.js.gz", "cp2077.wasm.gz", "cp2077.data.gz"];
const optional = ["cp2077-io-helper.js"];
const missing = required.filter(f => !fs.existsSync(path.join(dir, f)));
if (missing.length) { console.error("bundle incomplete, missing: " + missing.join(", ")); process.exit(2); }
const names = [...required, ...optional.filter(f => fs.existsSync(path.join(dir, f)))];
// bundle.json last: its presence is what the page checks before booting the bundle
names.push(names.splice(names.indexOf("bundle.json"), 1)[0]);
const items = names.map((f, i) => ({key: `runtime/engine-versions/${sha}/${f}`, file: path.join(dir, f), size: fs.statSync(path.join(dir, f)).size, group: "bundle"}));
const listFile = path.join(OUT, `bundle-${sha.slice(0, 12)}-list.json`);
fs.writeFileSync(listFile, JSON.stringify({bundle: sha, bucket: "cp2077-assets", generated: new Date().toISOString(), items}, null, 1));
const total = items.reduce((a, it) => a + it.size, 0);
console.log(`bundle ${sha.slice(0, 12)}: ${items.length} files, ${(total / 1e6).toFixed(1)} MB -> R2`);

// two passes: every file first, bundle.json only after they are all verified (the page checks bundle.json)
for (const pass of [["--skip", "/bundle\\.json$"], ["--only", "/bundle\\.json$"]]) {
  const r = spawnSync(process.execPath, [path.join(CF_ROOT, "scripts", "upload-multipart.mjs"), "--list", listFile, ...pass,
    ...(process.argv.includes("--dry") ? ["--dry"] : [])], {cwd: CF_ROOT, encoding: "utf8"});
  const lines = (r.stdout || "").split("\n").filter(l => /\b(done|skip|FAILED|HAVE|MISS)\b/.test(l));
  for (const l of lines) console.log("  " + l.replace(/^\S+ /, "").replace(/runtime\/engine-versions\/[0-9a-f]{64}\//, ""));
  if (r.status !== 0) { console.error(r.stderr || "upload failed"); process.exit(1); }
}
if (process.argv.includes("--dry")) process.exit(0);

// verify through the Worker (what the page will see)
const s = JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8"));
let bad = 0;
for (const it of items) {
  const res = await fetch(`${s.WORKER_URL}/k/${encodeURIComponent(s.ACCESS_TOKEN)}/${it.key}`, {headers: {"Accept-Encoding": "identity", Range: "bytes=0-0"}});
  const len = Number(/\/(\d+)$/.exec(res.headers.get("content-range") || "")?.[1]); await res.arrayBuffer();
  const ok = res.status === 206 && len === it.size;
  if (!ok) { ++bad; console.log(`  VERIFY FAIL ${path.basename(it.key)}: ${res.status} len=${len} want ${it.size}`); }
}
console.log(bad ? `${bad} file(s) failed verification` : `verified ${items.length}/${items.length} via the Worker; boot it with /game?engine=${sha}`);
process.exitCode = bad ? 1 : 0;
