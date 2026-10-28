// Build a warm list (runtime/warm/<name>.json) from io4 recordings: the last "CP2077 ioWarm record: {...}" line of
// each console capture (blocks in first-demand order). Several captures are merged, keeping first-seen order.
//   node scripts/make-warm-list.mjs <name> <console.txt|record.json>... [--max-mb 8192] [--skip-regex <re>]
//        [--minus <list-or-record>]... [--intersect <list-or-record>]...
// Inputs: console captures, or the JSON of cp2077EngineModule.cp2077IoTrace.record() (CDP dump), or another
// warm list (out/warm/x.json). --minus drops blocks present in those lists (e.g. lifepath minus menu);
// --intersect keeps only blocks present in ALL of them (e.g. the new-game blocks common to every lifepath).
// Output: out/warm/<name>.json (upload as R2 key runtime/warm/<name>.json; tiny JSON, a few KB).
// ETags come from the local manifest (out/game-files.json); the helper skips files whose ETag changed.
import fs from "node:fs";
import path from "node:path";
import {OUT} from "./common.mjs";

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); if (i < 0) return d; const v = args[i + 1]; args.splice(i, 2); return v; };
const multi = k => { const out = []; for (let i; (i = args.indexOf(k)) >= 0;) { out.push(args[i + 1]); args.splice(i, 2); } return out; };
const minus = multi("--minus"), intersect = multi("--intersect");
const maxMb = Number(opt("--max-mb", 8192)), skip = opt("--skip-regex", null);
const [name, ...inputs] = args;
if (!name || !/^[A-Za-z0-9_-]{1,40}$/.test(name) || !inputs.length) { console.error("usage: make-warm-list.mjs <name> <console.txt>... [--max-mb N] [--skip-regex re]"); process.exit(2); }
const manifest = JSON.parse(fs.readFileSync(path.join(OUT, "game-files.json"), "utf8"));
const byPath = new Map(manifest.files.map(f => [f.path, f]));
const tag = "CP2077 ioWarm record: ";
// read a record (console capture / record JSON / warm list) into an ordered list of "path|block"
function readBlocks(file) {
  const text = fs.readFileSync(file, "utf8").trim();
  let last = null;
  if (text.startsWith("{")) last = text;
  else for (const line of text.split(/\r?\n/)) { const i = line.indexOf(tag); if (i >= 0) last = line.slice(i + tag.length).trim().replace(/^"|"$/g, ""); }
  if (!last) { console.warn("no ioWarm record in " + file); return []; }
  const rec = JSON.parse(last), out = [];
  for (const [p, first, count] of rec.order || []) for (let n = first; n < first + count; ++n) out.push(p + "|" + n);
  return out;
}
const minusSet = new Set(minus.flatMap(readBlocks));
const interSets = intersect.map(f => new Set(readBlocks(f)));
const order = [], seen = new Set;
let blockBytes = 1048576;
for (const input of inputs) {
  const text = fs.readFileSync(input, "utf8");
  let last = null;
  if (text.trim().startsWith("{")) last = text.trim();
  else for (const line of text.split(/\r?\n/)) { const i = line.indexOf(tag); if (i >= 0) last = line.slice(i + tag.length).trim().replace(/^"|"$/g, ""); }
  if (!last) { console.warn("no ioWarm record line in " + input); continue; }
  const rec = JSON.parse(last);
  blockBytes = rec.blockBytes || blockBytes;
  for (const [p, first, count] of rec.order) for (let n = first; n < first + count; ++n) {
    const k = p + "|" + n;
    if (seen.has(k) || !byPath.has(p) || (skip && new RegExp(skip).test(p)) || minusSet.has(k) || interSets.some(x => !x.has(k))) continue;
    seen.add(k); order.push([p, n]);
  }
}
const cap = Math.floor(maxMb * 1048576 / blockBytes);
if (order.length > cap) { console.warn(`trimming ${order.length} blocks to --max-mb ${maxMb}`); order.length = cap; }
const runs = [];
for (const [p, n] of order) { const l = runs[runs.length - 1]; if (l && l[0] === p && l[1] + l[2] === n) ++l[2]; else runs.push([p, n, 1]); }
const etags = {};
for (const [p] of runs) etags[p] = byPath.get(p).etag;
const out = {version: 1, name, blockBytes, blocks: order.length, generated: new Date().toISOString(), etags, order: runs};
const dir = path.join(OUT, "warm");
fs.mkdirSync(dir, {recursive: true});
const file = path.join(dir, name + ".json");
fs.writeFileSync(file, JSON.stringify(out));
const per = {};
for (const [p] of order) per[p] = (per[p] || 0) + 1;
for (const [p, n] of Object.entries(per).sort((a, b) => b[1] - a[1]).slice(0, 12)) console.log(`${String(n).padStart(6)} MiB  ${p}`);
console.log(`warm list ${name}: ${order.length} blocks (${(order.length * blockBytes / 1048576).toFixed(0)} MiB) in ${runs.length} runs -> ${file} (${fs.statSync(file).size} bytes); R2 key runtime/warm/${name}.json`);
