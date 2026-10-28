// Generate runtime/game-files.json exactly as serve-engine-assets.py serve_manifest() does,
// so the Worker can present the same sizes/ETags. Output: cloudflare/out/game-files.json
// Usage: node scripts/build-manifest.mjs [--compare http://127.0.0.1:8078]
import fs from "node:fs";
import path from "node:path";
import {PORT_ROOT, DEPOT, OUT, walk, posix, isFile, localEtag} from "./common.mjs";

export function buildManifest() {
  const files = [];   // {entry, file}
  const archiveRoot = path.join(PORT_ROOT, "archive", "pc");
  const archives = walk(archiveRoot).filter(p => [".archive", ".bin", ".cache", ".h64"].includes(path.extname(p)));
  // Python sorts pathlib paths (case-insensitive on Windows); order is irrelevant to the runtime.
  archives.sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()));
  for (const p of archives) files.push({path: "/game/archive/pc/" + posix(path.relative(archiveRoot, p)), file: p});
  for (const [name, dir] of [["master.redscripts", "cook-lab/r6/cache"], ["final.redscripts", "cook-lab/r6/cache"], ["tweakdb.bin", "cooked/pc/cache"]]) {
    const p = path.join(PORT_ROOT, dir, name);
    if (isFile(p)) files.push({path: "/game/r6/cache/" + name, file: p});
  }
  const shader = path.join(DEPOT, "engine", "shader_final.cache");
  if (isFile(shader)) files.push({path: "/game/engine/shader_final.cache", file: shader});
  const entries = files.map(({path: p, file}) => { const s = localEtag(file); return {path: p, size: s.size, mtime: s.mtime, etag: s.etag}; });
  return {manifest: {version: 1, files: entries}, sources: files};
}

if (import.meta.url === `file:///${process.argv[1].replace(/\\/g, "/")}` || process.argv[1].endsWith("build-manifest.mjs")) {
  const {manifest} = buildManifest();
  fs.mkdirSync(OUT, {recursive: true});
  const target = path.join(OUT, "game-files.json");
  fs.writeFileSync(target, JSON.stringify(manifest));
  const total = manifest.files.reduce((a, e) => a + e.size, 0);
  console.log(`manifest: ${manifest.files.length} files, ${(total / 1e9).toFixed(2)} GB -> ${target}`);
  const i = process.argv.indexOf("--compare");
  if (i > 0) {
    const origin = process.argv[i + 1] || "http://127.0.0.1:8078";
    const live = await (await fetch(origin + "/runtime/game-files.json")).json();
    const mine = new Map(manifest.files.map(e => [e.path, e]));
    let diffs = 0;
    for (const e of live.files) {
      const m = mine.get(e.path);
      if (!m || m.size !== e.size || m.etag !== e.etag) { ++diffs; console.log("DIFF", e.path, e.etag, m && m.etag); }
    }
    if (live.files.length !== manifest.files.length) { ++diffs; console.log("DIFF count", live.files.length, manifest.files.length); }
    console.log(diffs ? `compare: ${diffs} differences vs ${origin}` : `compare: identical paths/sizes/etags to ${origin}`);
    process.exitCode = diffs ? 1 : 0;
  }
}
