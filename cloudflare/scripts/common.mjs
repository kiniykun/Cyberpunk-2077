// Shared paths and helpers for the Cloudflare scripts. Only stats files; never hashes the 41 GB of archives.
import fs from "node:fs";
import path from "node:path";
import {fileURLToPath} from "node:url";

export const CF_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
export const PORT_ROOT = path.resolve(CF_ROOT, "..");                       // D:\2077
export const OUT = path.join(CF_ROOT, "out");
export const DEPOT = process.env.CP2077_DEPOT || "D:\\cp2077SourceCode\\extracted\\root\\R6.Root\\Mainline"; // read-only: stat/read only
export const RUNTIME = path.join(PORT_ROOT, "build", "game");
export const BUCKET = process.env.CP2077_BUCKET || "cp2077-assets";
export const MiB = 1024 * 1024, GB = 1e9;

export const isFile = p => { try { return fs.statSync(p).isFile(); } catch { return false; } };
// Same validator as serve-engine-assets.py: '"%x-%x"' % (st_size, st_mtime_ns)
export function localEtag(p) {
  const s = fs.statSync(p, {bigint: true});
  return {size: Number(s.size), mtime: Number(s.mtimeNs) / 1e6, etag: `"${s.size.toString(16)}-${s.mtimeNs.toString(16)}"`};
}
export function walk(dir) {
  const out = [];
  if (!fs.existsSync(dir)) return out;
  for (const d of fs.readdirSync(dir, {withFileTypes: true})) {
    const p = path.join(dir, d.name);
    if (d.isDirectory()) out.push(...walk(p)); else if (d.isFile()) out.push(p);
  }
  return out;
}
export const posix = p => p.split(path.sep).join("/");
export const fmt = n => (n / GB).toFixed(2) + " GB";

// Active immutable engine bundle; must agree with the hash the live page references.
export function activeBundle() {
  const active = JSON.parse(fs.readFileSync(path.join(PORT_ROOT, "build", "active-engine-bundle.json"), "utf8"));
  const page = fs.readFileSync(path.join(PORT_ROOT, "native-browser.html"), "utf8");
  const pageHashes = [...new Set([...page.matchAll(/engine-versions\/([0-9a-f]{64})\//g)].map(m => m[1]))];
  if (pageHashes.length !== 1 || pageHashes[0] !== active.BuildID)
    throw new Error(`native-browser.html references ${pageHashes.join(",") || "no"} bundle but active-engine-bundle.json says ${active.BuildID}. ` +
      "A publish is probably in progress; rerun when they agree.");
  return active.BuildID;
}
