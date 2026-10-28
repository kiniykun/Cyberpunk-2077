// Convert wrangler dev logs ("[cf] 206 <source> GET <path> bytes=a-b <ms>ms", TRACE_LOG=1) into the trace
// export format that trace-to-ranges.mjs reads, so a partial bucket can be built from a real session.
//   node scripts/wranglerlog-to-ranges.mjs <wrangler.log>... [--out out/wranglerlog-trace.json]
// Then: node scripts/trace-to-ranges.mjs out/wranglerlog-trace.json --pad 2
import fs from "node:fs";
import path from "node:path";
import {OUT, MiB} from "./common.mjs";

const args = process.argv.slice(2);
const oi = args.indexOf("--out");
const outFile = oi >= 0 ? args.splice(oi, 2)[1] : path.join(OUT, "wranglerlog-trace.json");
if (!args.length) { console.error("usage: wranglerlog-to-ranges.mjs <wrangler.log>... [--out file]"); process.exit(2); }
const B = 1048576, files = new Map();
let lines = 0, requests = 0, bytes = 0;
const re = /\[cf\] (\d{3}) \S+ GET (\/game\/\S+) bytes=(\d+)-(\d+)/;
for (const input of args) for (const line of fs.readFileSync(input, "utf8").split(/\r?\n/)) {
  ++lines;
  const m = re.exec(line);
  if (!m || (m[1] !== "206" && m[1] !== "200")) continue;
  const p = m[2], s = Number(m[3]), e = Number(m[4]);
  let r = files.get(p);
  if (!r) files.set(p, r = {blocks: new Set, requests: 0, bytes: 0});
  ++r.requests; r.bytes += e - s + 1; ++requests; bytes += e - s + 1;
  for (let n = Math.floor(s / B); n <= Math.floor(e / B); ++n) r.blocks.add(n);
}
const rle = set => { const v = [...set].sort((x, y) => x - y), o = []; for (let i = 0; i < v.length;) { let j = i; while (j + 1 < v.length && v[j + 1] === v[j] + 1) ++j; o.push(i === j ? "" + v[i] : v[i] + "-" + v[j]); i = j + 1; } return o.join(","); };
const out = {version: 1, blockBytes: B, source: args, files: {}};
let uniq = 0;
for (const [p, r] of [...files].sort((a, b) => b[1].requests - a[1].requests)) {
  out.files[p] = {blocks: rle(r.blocks), requests: r.requests, bytes: r.bytes};
  uniq += r.blocks.size;
  console.log(`${String(r.requests).padStart(6)} req ${(r.bytes / MiB).toFixed(0).padStart(6)} MiB  ${String(r.blocks.size).padStart(5)} unique MiB  ${p}`);
}
fs.mkdirSync(path.dirname(outFile), {recursive: true});
fs.writeFileSync(outFile, JSON.stringify(out));
console.log(`game range requests ${requests}, ${(bytes / MiB).toFixed(0)} MiB transferred, ${uniq} unique 1 MiB blocks -> ${outFile}`);
