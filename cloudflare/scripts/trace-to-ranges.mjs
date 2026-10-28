// Turn an ?assetTrace=1 capture into a "partial bucket" upload list: only the 1 MiB blocks a session
// actually touched, stored as R2 objects blocks/<key>/<n> which the Worker serves transparently.
//   node scripts/trace-to-ranges.mjs <trace.json | console.log> [--pad N] [--full-under MB] [--out file]
// Input: JSON from cp2077AssetTrace.export(), or any console capture containing
// "CP2077 assetTrace ranges: {...}" lines (the last one wins; it is cumulative). Several inputs are unioned.
//   --pad N         also include N blocks after each touched run (read-ahead headroom, default 2)
//   --full-under MB upload whole files smaller than this instead of blocks (default 8)
// Then: node scripts/upload.mjs --target local|remote --list out/partial-upload-list.json [--execute]
import fs from "node:fs";
import path from "node:path";
import {OUT, MiB, fmt} from "./common.mjs";
import {buildManifest} from "./build-manifest.mjs";

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); if (i < 0) return def; const v = args[i + 1]; args.splice(i, 2); return v; };
const pad = Number(opt("--pad", 2));
const fullUnder = Number(opt("--full-under", 8)) * MiB;
const outFile = opt("--out", path.join(OUT, "partial-upload-list.json"));
if (!args.length) { console.error("usage: trace-to-ranges.mjs <trace.json|console.log>... [--pad N] [--full-under MB] [--out f]"); process.exit(2); }

// Accepts export JSON, or console captures with "CP2077 assetTrace ranges:" (main thread) and/or
// "CP2077 ioTrace ranges:" (pthread workers, io bundle) lines; the last line of each kind wins (cumulative).
function parseTraces(text) {
  const t = text.trim();
  if (t.startsWith("{")) return [JSON.parse(t)];
  const last = {};
  for (const line of t.split(/\r?\n/)) for (const tag of ["CP2077 assetTrace ranges: ", "CP2077 ioTrace ranges: "]) {
    const i = line.indexOf(tag);
    if (i >= 0) last[tag] = line.slice(i + tag.length).trim().replace(/^"|"$/g, "");
  }
  const found = Object.values(last);
  if (!found.length) throw new Error("no 'CP2077 assetTrace ranges:' or 'CP2077 ioTrace ranges:' line found");
  return found.map(x => JSON.parse(x));
}
const touched = new Map();   // path -> Set(blocks)
let blockBytes = 1048576;
for (const input of args) for (const trace of parseTraces(fs.readFileSync(input, "utf8"))) {
  blockBytes = trace.blockBytes || blockBytes;
  for (const [p, f] of Object.entries(trace.files)) {
    if (!p.startsWith("/game/")) continue;
    const set = touched.get(p) || new Set();
    for (const run of f.blocks.split(",").filter(Boolean)) {
      const [a, b = a] = run.split("-").map(Number);
      for (let n = a; n <= b + pad; ++n) set.add(n);
    }
    touched.set(p, set);
  }
}
if (blockBytes !== 1048576) console.warn("warning: trace block size", blockBytes, "differs from Worker PARTIAL_BLOCK_BYTES default 1048576");

const {sources, manifest} = buildManifest();
const sizes = new Map(manifest.files.map(e => [e.path, e.size]));
const fileOf = new Map(sources.map(s => [s.path, s.file]));
const items = [];
let total = 0, fullBytes = 0;
const report = [];
for (const [p, set] of [...touched].sort()) {
  const size = sizes.get(p), file = fileOf.get(p);
  if (size == null) { console.warn("not in manifest, skipped:", p); continue; }
  fullBytes += size;
  if (size < fullUnder) { items.push({key: p.slice(1), file, size}); total += size; report.push([p, size, size]); continue; }
  let bytes = 0;
  for (const n of [...set].sort((a, b) => a - b)) {
    const offset = n * blockBytes;
    if (offset >= size) continue;
    const length = Math.min(blockBytes, size - offset);
    items.push({key: `blocks/${p.slice(1)}/${n}`, file, offset, length, size: length});
    bytes += length;
  }
  total += bytes;
  report.push([p, bytes, size]);
}
fs.mkdirSync(path.dirname(outFile), {recursive: true});
fs.writeFileSync(outFile, JSON.stringify({bucket: "cp2077-assets", partial: true, blockBytes, pad, generated: new Date().toISOString(), items}, null, 1));
for (const [p, b, s] of report) console.log(`${(b / MiB).toFixed(1).padStart(9)} MiB of ${(s / MiB).toFixed(0).padStart(6)} MiB  ${p}`);
console.log(`partial bucket: ${items.length} objects, ${fmt(total)} (of ${fmt(fullBytes)} in the touched files, pad ${pad}) -> ${outFile}`);
console.log("Also upload the non-game objects: node scripts/upload.mjs --target <t> --skip ^game/archive/ [--execute]");
