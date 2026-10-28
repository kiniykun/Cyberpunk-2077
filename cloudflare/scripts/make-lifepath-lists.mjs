// Build lifepath-aware warm lists from per-lifepath recordings (CDP dump of cp2077EngineModule.cp2077IoTrace.record(),
// or console captures with "CP2077 ioWarm record:" lines). Needs at least two lifepaths.
//   node scripts/make-lifepath-lists.mjs --streetkid <rec> --nomad <rec> [--corpo <rec>] [--menu <rec>] [--max-mb 8192]
// [io7] phase split: with --menu <io7 record of a fresh boot to the main menu> (or io7 lifepath records that carry
// marks), also writes boot.json (blocks demanded before the engine reached Running), menu.json (then up to the main
// menu) and common-rest.json (common minus boot and menu); <lp>.json then also excludes boot/menu blocks.
// Writes out/warm/:
//   common.json     blocks recorded in EVERY given lifepath (menu, character creation, shared game data),
//                   ordered by earliest normalised first-demand rank across the recordings  -> warmed at boot
//   <lp>.json       that lifepath's blocks minus common, in its own first-demand order        -> warmed once known
//   lifepaths.json  per lifepath the blocks recorded for it ALONE (the helper's read-based lifepath detector)
// Upload each as R2 key runtime/warm/<file>.
import fs from "node:fs";
import path from "node:path";
import {OUT} from "./common.mjs";

const args = process.argv.slice(2);
const opt = (k, d) => { const i = args.indexOf(k); return i < 0 ? d : args[i + 1]; };
const maxMb = Number(opt("--max-mb", 8192));
const names = ["streetkid", "nomad", "corpo"].filter(n => opt("--" + n, null));
if (names.length < 2) { console.error("need at least two of --streetkid/--nomad/--corpo <record>"); process.exit(2); }
const manifest = JSON.parse(fs.readFileSync(path.join(OUT, "game-files.json"), "utf8"));
const byPath = new Map(manifest.files.map(f => [f.path, f]));
const tag = "CP2077 ioWarm record: ";
function readOrder(file) {
  const text = fs.readFileSync(file, "utf8").trim();
  let last = null;
  if (text.startsWith("{")) last = text;
  else for (const line of text.split(/\r?\n/)) { const i = line.indexOf(tag); if (i >= 0) last = line.slice(i + tag.length).trim().replace(/^"|"$/g, ""); }
  if (!last) throw new Error("no ioWarm record in " + file);
  const rec = JSON.parse(last), out = [], seen = new Set;
  out.marks = rec.marks || [];
  if (rec.blockBytes && rec.blockBytes !== 1048576) throw new Error("unexpected blockBytes in " + file);
  for (const [p, first, count] of rec.order || []) for (let n = first; n < first + count; ++n) {
    const k = p + "|" + n;
    if (!seen.has(k) && byPath.has(p)) { seen.add(k); out.push(k); }
  }
  return out;
}
const orders = Object.fromEntries(names.map(n => [n, readOrder(opt("--" + n))]));
const sets = Object.fromEntries(names.map(n => [n, new Set(orders[n])]));
const rank = Object.fromEntries(names.map(n => [n, new Map(orders[n].map((k, i) => [k, i / orders[n].length]))]));

const cap = Math.floor(maxMb);   // 1 MiB blocks
const commonKeys = [...sets[names[0]]].filter(k => names.every(n => sets[n].has(k)));
commonKeys.sort((a, b) => Math.min(...names.map(n => rank[n].get(a))) - Math.min(...names.map(n => rank[n].get(b))));
const commonSet = new Set(commonKeys);
// ---- [io7] phase split
const markAt = (o, name) => { const m = (o.marks || []).find(x => x.name === name); return m ? m.at : -1; };
const runningAt = o => { const m = (o.marks || []).find(x => x.name !== "Initialization" && x.name !== "menu"); return m ? m.at : -1; };
let bootKeys = [], menuKeys = [];
const menuRec = opt("--menu", null) ? readOrder(opt("--menu", null)) : null;
if (menuRec) {
  const r = runningAt(menuRec), mm = markAt(menuRec, "menu");
  if (r < 0) console.warn("--menu record has no engine phase marks (not an io7 recording?): all of it counts as boot");
  bootKeys = menuRec.slice(0, r < 0 ? menuRec.length : r);
  menuKeys = menuRec.slice(r < 0 ? menuRec.length : r);
  if (mm < 0) console.warn("--menu record has no main-menu mark: everything after Running counts as menu");
} else {
  const marked = names.filter(n => runningAt(orders[n]) >= 0);
  if (marked.length) {
    const phase = k => Math.min(...marked.map(n => { const o = orders[n], i = o.indexOf(k), r = runningAt(o), mm = markAt(o, "menu"); return i < 0 ? 9 : i < r ? 0 : (mm >= 0 && i < mm) ? 1 : 2; }));
    for (const k of commonKeys) { const ph = phase(k); if (ph === 0) bootKeys.push(k); else if (ph === 1) menuKeys.push(k); }
  } else console.warn("no phase marks (records are pre-io7 and no --menu given): boot/menu lists not written; common-rest = common");
}
const bootSet = new Set(bootKeys), menuSetKeys = menuKeys.filter(k => !bootSet.has(k)), earlySet = new Set([...bootKeys, ...menuSetKeys]);

const runsOf = keys => {
  const runs = [];
  for (const k of keys) {
    const i = k.lastIndexOf("|"), p = k.slice(0, i), n = Number(k.slice(i + 1)), l = runs[runs.length - 1];
    if (l && l[0] === p && l[1] + l[2] === n) ++l[2]; else runs.push([p, n, 1]);
  }
  return runs;
};
const etagsOf = runs => Object.fromEntries([...new Set(runs.map(r => r[0]))].map(p => [p, byPath.get(p).etag]));
const dir = path.join(OUT, "warm");
fs.mkdirSync(dir, {recursive: true});
const write = (name, keys, extra = {}) => {
  if (keys.length > cap) { console.warn(`${name}: trimming ${keys.length} blocks to --max-mb ${maxMb}`); keys = keys.slice(0, cap); }
  const order = runsOf(keys), file = path.join(dir, name + ".json");
  fs.writeFileSync(file, JSON.stringify({version: 1, name, blockBytes: 1048576, blocks: keys.length, generated: new Date().toISOString(), etags: etagsOf(order), order, ...extra}));
  console.log(`${name.padEnd(10)} ${String(keys.length).padStart(5)} blocks (${keys.length} MiB)  ${order.length} runs  ${(fs.statSync(file).size / 1024).toFixed(0)} KB -> runtime/warm/${name}.json`);
};
write("common", commonKeys, {from: names});
if (bootKeys.length) write("boot", bootKeys, {phase: "before engine Running"});
if (menuSetKeys.length) write("menu", menuSetKeys, {phase: "Running to main menu"});
write("common-rest", commonKeys.filter(k => !earlySet.has(k)), {from: names, minus: ["boot", "menu"]});
for (const n of names) write(n, orders[n].filter(k => !commonSet.has(k) && !earlySet.has(k)));

const lifepaths = {}, allEtags = {};
for (const n of names) {
  const exclusive = orders[n].filter(k => names.every(o => o === n || !sets[o].has(k)));
  const runs = runsOf(exclusive);
  lifepaths[n] = {blocks: exclusive.length, firstExclusiveRank: exclusive.length ? Math.round(rank[n].get(exclusive[0]) * 1000) / 1000 : null, exclusive: runs};
  Object.assign(allEtags, etagsOf(runs));
}
const lpFile = path.join(dir, "lifepaths.json");
fs.writeFileSync(lpFile, JSON.stringify({version: 1, blockBytes: 1048576, generated: new Date().toISOString(), etags: allEtags, lifepaths}));
for (const n of names) {
  // how early does the detector get evidence? position (fraction of the recording) of the 6th exclusive block
  const ex = orders[n].filter(k => names.every(o => o === n || !sets[o].has(k)));
  const sixth = ex[5] ? rank[n].get(ex[5]) : null;
  console.log(`lifepaths.json ${n}: ${lifepaths[n].blocks} exclusive blocks; 6th exclusive block at ${sixth === null ? "-" : Math.round(sixth * 1000) / 10 + "%"} of its recording`);
}
console.log(`lifepaths.json ${(fs.statSync(lpFile).size / 1024).toFixed(0)} KB -> runtime/warm/lifepaths.json`);
