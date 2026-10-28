// Make an uploaded engine bundle the site default (bare /game and / on workers.dev):
//   node scripts/promote-bundle.mjs <sha>                                  plain default (also the classic fallback)
//   node scripts/promote-bundle.mjs <io-sha> --params "ioShared=1&..." [--classic <sha>]
//        io default: bare /game boots <io-sha> with those params; ?io=classic boots the classic bundle with no io
//        params (default classic: the previous classic / previous plain default).
// Checks the bundle(s) are complete in R2, records them in out/site-default-bundle.json, rebuilds the R2 page and
// the Vercel page (vercel-site/game/) and re-uploads the R2 page. Does NOT deploy Vercel.
// Roll back: promote the previous values again (printed below and kept as "previous" in the json).
import fs from "node:fs";
import path from "node:path";
import {spawnSync} from "node:child_process";
import {CF_ROOT, RUNTIME, OUT} from "./common.mjs";

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const HEX = /^[0-9a-f]{64}$/;
const sha = process.argv[2];
if (!HEX.test(sha || "")) { console.error("usage: node scripts/promote-bundle.mjs <sha> [--params \"k=v&...\"] [--classic <sha>]"); process.exit(2); }
const paramsArg = arg("--params");
const params = paramsArg ? Object.fromEntries(new URLSearchParams(paramsArg)) : null;
const DEFAULT_FILE = path.join(OUT, "site-default-bundle.json");
const prev = fs.existsSync(DEFAULT_FILE) ? JSON.parse(fs.readFileSync(DEFAULT_FILE, "utf8")) : null;
const classic = params ? (arg("--classic") || prev?.classic || prev?.bundle) : sha;
if (!HEX.test(classic || "")) { console.error("io default needs a classic fallback: pass --classic <sha>"); process.exit(2); }

const s = JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8"));
async function checkBundle(h, needLocalSizes) {
  const dir = path.join(RUNTIME, "engine-versions", h);
  if (needLocalSizes) for (const f of ["cp2077.wasm", "cp2077.wasm.gz", "cp2077.data", "cp2077.data.gz", "cp2077.js.gz"])
    if (!fs.existsSync(path.join(dir, f))) { console.error(`local bundle dir lacks ${f} (needed for the loader's size table): ${dir}`); process.exit(2); }
  const base = `${s.WORKER_URL}/k/${encodeURIComponent(s.ACCESS_TOKEN)}/runtime/engine-versions/${h}/`;
  for (const f of ["bundle.json", "cp2077.js", "cp2077.wasm", "cp2077.data", "cp2077.js.gz", "cp2077.wasm.gz", "cp2077.data.gz"]) {
    const r = await fetch(base + f, {headers: {"Accept-Encoding": "identity", Range: "bytes=0-0"}});
    const total = Number(/\/(\d+)$/.exec(r.headers.get("content-range") || "")?.[1]); await r.arrayBuffer();
    const local = path.join(dir, f), want = fs.existsSync(local) ? fs.statSync(local).size : null;
    if (r.status !== 206 || (want != null && total !== want)) {
      console.error(`R2 copy of ${h.slice(0, 12)}/${f} is missing or differs (${r.status}); run: node scripts/upload-bundle.mjs ${h}`); process.exit(1);
    }
  }
}
await checkBundle(classic, true);
if (sha !== classic) await checkBundle(sha, false);

// [w32] the wasm32 (no-memory64) bundle is carried over unchanged; --w32 <sha> [--w32-params "k=v&..."] replaces it.
const w32Arg = arg("--w32"); if (w32Arg && !HEX.test(w32Arg)) { console.error("--w32 needs a 64-hex sha"); process.exit(2); }
if (w32Arg) await checkBundle(w32Arg, false);
const w32 = w32Arg ? {bundle: w32Arg, params: Object.fromEntries(new URLSearchParams(arg("--w32-params", "mem=ios&binkRecycle=1")))} : (prev?.w32 ?? null);
const record = {bundle: sha, ...(params ? {params, classic} : {}), ...(w32 ? {w32} : {}), previous: prev ? {bundle: prev.bundle, params: prev.params, classic: prev.classic} : null,
  promoted: new Date().toISOString()};
fs.writeFileSync(DEFAULT_FILE, JSON.stringify(record, null, 1));

const run = (...a) => {
  const r = spawnSync(process.execPath, a.map((x, i) => i === 0 ? path.join(CF_ROOT, "scripts", x) : x), {cwd: CF_ROOT, encoding: "utf8"});
  process.stdout.write((r.stdout || "").split("\n").filter(l => !/^\S+ +(upload|  )/.test(l)).join("\n"));
  if (r.status !== 0) { console.error(r.stderr); process.exit(1); }
};
run("make-r2-page.mjs");
run("upload-multipart.mjs", "--list", path.join(OUT, "loader-upload-list.json"));
run("make-vercel-page.mjs", "--cf", "https://cdn.cyberpunk2077.lol", "--cf-fallback", "https://cp2077-port.fn-web-worker.workers.dev", "--copy-to", "vercel-site/game/index.html");   // [cdn-fallback] 2026-10-22: /game uses the cdn custom domain, falls back to workers.dev
const desc = r => r ? `${String(r.bundle).slice(0, 12)}${r.params ? " +" + new URLSearchParams(r.params) + " (classic " + String(r.classic).slice(0, 12) + ")" : ""}` : "(local active bundle)";
console.log(`\npromoted ${desc(record)}; previous: ${desc(record.previous)}. R2 page live now; deploy vercel-site for cyberpunk2077.lol/game.`);
