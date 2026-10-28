// Upload an upload list to R2. THE USER RUNS THIS (after `npx wrangler login`); nothing here logs in.
//   node scripts/upload.mjs --target remote            # dry run: prints every command + total size
//   node scripts/upload.mjs --target remote --execute  # real upload
//   node scripts/upload.mjs --target local --max-mb 300 --execute   # seed miniflare's local R2 for `wrangler dev`
// Options:
//   --list <file>        default out/upload-list.json (or out/partial-upload-list.json from trace-to-ranges.mjs)
//   --only <regex>       only keys matching regex          --skip <regex>  skip keys matching regex
//   --max-mb <n>         skip objects larger than n MiB
//   --rclone-remote <r>  rclone remote for objects > 300 MiB (default "r2"; configure once with `rclone config`,
//                        type s3 / provider Cloudflare, an R2 API token's access key + secret, endpoint
//                        https://<account_id>.r2.cloudflarestorage.com). wrangler r2 object put caps at 300 MiB.
import fs from "node:fs";
import path from "node:path";
import {spawnSync} from "node:child_process";
import {CF_ROOT, OUT, BUCKET, MiB, fmt} from "./common.mjs";

const arg = (name, def) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : def; };
const has = name => process.argv.includes(name);
const target = arg("--target");
if (target !== "local" && target !== "remote") { console.error("need --target local|remote"); process.exit(2); }
const execute = has("--execute");
const listFile = arg("--list", path.join(OUT, "upload-list.json"));
const only = arg("--only") ? new RegExp(arg("--only")) : null, skip = arg("--skip") ? new RegExp(arg("--skip")) : null;
const maxBytes = arg("--max-mb") ? Number(arg("--max-mb")) * MiB : Infinity;
const remote = arg("--rclone-remote", "r2");
const WRANGLER_LIMIT = 300 * MiB;

const list = JSON.parse(fs.readFileSync(listFile, "utf8"));
const bucket = list.bucket || BUCKET;
const items = list.items.filter(it => (!only || only.test(it.key)) && (!skip || !skip.test(it.key)));
const tmpDir = path.join(OUT, "tmp");
const wranglerBin = path.join(CF_ROOT, "node_modules", ".bin", process.platform === "win32" ? "wrangler.cmd" : "wrangler");
const wrangler = fs.existsSync(wranglerBin) ? `"${wranglerBin}"` : "npx wrangler";

function run(cmd) {
  console.log("  $ " + cmd);
  if (!execute) return true;
  const r = spawnSync(cmd, {shell: true, stdio: "inherit", cwd: CF_ROOT, env: {...process.env, WRANGLER_SEND_METRICS: "false"}});
  return r.status === 0;
}

let planned = 0, skipped = 0, failed = 0, n = 0;
for (const it of items) {
  const size = it.length ?? it.size;
  if (size > maxBytes) { ++skipped; continue; }
  ++n; planned += size;
  console.log(`[${n}] ${it.key} (${(size / MiB).toFixed(1)} MiB)`);
  let file = it.file;
  if (it.offset != null) {                       // partial-bucket block: slice of the original file
    file = path.join(tmpDir, "slice.bin");
    if (execute) {
      fs.mkdirSync(tmpDir, {recursive: true});
      const fd = fs.openSync(it.file, "r"), buf = Buffer.alloc(it.length);
      fs.readSync(fd, buf, 0, it.length, it.offset); fs.closeSync(fd);
      fs.writeFileSync(file, buf);
    }
  }
  let ok;
  if (target === "remote" && size > WRANGLER_LIMIT)
    ok = run(`rclone copyto "${file}" "${remote}:${bucket}/${it.key}" --s3-chunk-size 100M --s3-upload-concurrency 4 --s3-no-check-bucket --progress`);
  else
    ok = run(`${wrangler} r2 object put "${bucket}/${it.key}" --file "${file}" --content-type application/octet-stream ${target === "local" ? "--local" : "--remote"}`);
  if (!ok) { ++failed; console.error("  FAILED " + it.key); }
}
console.log(`${execute ? "uploaded" : "DRY RUN (add --execute)"}: ${n} objects, ${fmt(planned)} to ${target} bucket ${bucket}; skipped ${skipped} over --max-mb; failed ${failed}`);
process.exitCode = failed ? 1 : 0;
