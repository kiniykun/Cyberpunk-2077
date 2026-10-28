// Plan B checks (node only): cross-origin /k/<token>/ asset access from an allowed page origin.
//   node scripts/verify-planb.mjs [--page http://127.0.0.1:8099/game] [--origin http://127.0.0.1:8099]
import fs from "node:fs";
import path from "node:path";
import {CF_ROOT} from "./common.mjs";
const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const s = JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8"));
const CF = s.WORKER_URL, PAGE = arg("--page", "http://127.0.0.1:8099/game"), ORIGIN = arg("--origin", new URL(PAGE).origin);
const K = CF + "/k/" + encodeURIComponent(s.ACCESS_TOKEN);
const hash = JSON.parse(fs.readFileSync(path.join(CF_ROOT, "out", "upload-list.json"), "utf8")).bundle;
let fails = 0;
const check = (name, ok, extra = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${extra}`); if (!ok) ++fails; };
const H = r => ["access-control-allow-origin", "access-control-expose-headers", "cross-origin-resource-policy"].map(h => r.headers.get(h));

let r = await fetch(PAGE);
const page = await r.text();
check("page COOP/COEP", r.status === 200 && r.headers.get("cross-origin-opener-policy") === "same-origin" && r.headers.get("cross-origin-embedder-policy") === "require-corp", `${r.status}`);
check("page has prelude + no baked key", page.includes("__cpXoBoot(") && !page.includes(s.ACCESS_TOKEN) && !page.includes(s.UPLOAD_TOKEN));
check("page: no same-origin /runtime script src", !/src="\/runtime\//.test(page));

const man = await (await fetch(K + "/runtime/game-files.json", {headers: {Origin: ORIGIN}})).json();
const arc = man.files.find(f => f.path === "/game/archive/pc/content/basegame_4_gamedata.archive");
// preflight like the pthread sync XHR (Range + If-Match)
r = await fetch(K + arc.path, {method: "OPTIONS", headers: {Origin: ORIGIN, "Access-Control-Request-Method": "GET", "Access-Control-Request-Headers": "if-match,range"}});
check("preflight archive", r.status === 204 && r.headers.get("access-control-allow-origin") === ORIGIN && /if-match/i.test(r.headers.get("access-control-allow-headers")) && Number(r.headers.get("access-control-max-age")) >= 7200,
  `${r.status} max-age=${r.headers.get("access-control-max-age")}`);
r = await fetch(K + arc.path, {headers: {Origin: ORIGIN, Range: "bytes=1048576-2097151", "If-Match": arc.etag}});
const body = await r.arrayBuffer();
check("archive range + If-Match cross-origin", r.status === 206 && body.byteLength === 1048576 && r.headers.get("etag") === arc.etag && H(r)[0] === ORIGIN && /Content-Range/.test(H(r)[1]) && H(r)[2] === "cross-origin", `${r.status} ${H(r).join(" | ")}`);
r = await fetch(K + arc.path, {headers: {Origin: ORIGIN, Range: "bytes=0-1", "If-Match": '"stale"'}});
check("If-Match mismatch -> 412", r.status === 412, r.status);
r = await fetch(K + `/runtime/engine-versions/${hash}/cp2077.wasm`, {method: "HEAD", headers: {Origin: ORIGIN, "Accept-Encoding": "gzip"}});
check("gzip wasm cross-origin", r.status === 200 && r.headers.get("content-encoding") === "gzip" && H(r)[0] === ORIGIN, `${r.status} ${r.headers.get("content-encoding")}`);
r = await fetch(K + `/runtime/engine-versions/${hash}/cp2077.js?loaderio=1`);   // no-cors <script>/importScripts: needs CORP
check("engine js no-cors CORP cross-origin", r.status === 200 && H(r)[2] === "cross-origin", `${r.status} ${H(r)[2]}`);
for (const p of ["/runtime/codec/oodle-decoder-pool.mjs", "/runtime/codec/oodle-decoder.wasm", "/runtime/codec/bink/bink-decoder-worker.js", "/runtime/codec/bink/bink-sdk-vm.mjs",
  "/runtime/loading/loader.js", "/runtime/loading/logo.png", "/runtime/pipeline-warmup.json", "/sdk/bink2w64.dll", "/game/r6/cache/tweakdb.bin"]) {
  r = await fetch(K + p, {method: "HEAD", headers: {Origin: ORIGIN}});
  check("asset " + p, r.status === 200 && H(r)[0] === ORIGIN && H(r)[2] === "cross-origin", r.status);
}
r = await fetch(CF + "/k/not-the-key-xxxxxxxxxxxxxxxxxxxx/runtime/game-files.json", {headers: {Origin: ORIGIN}});
check("bad path key -> 401 (with CORS so the page can tell)", r.status === 401 && H(r)[0] === ORIGIN, r.status);
r = await fetch(CF + "/runtime/game-files.json", {headers: {Origin: ORIGIN}});
check("no key -> 401", r.status === 401, r.status);
r = await fetch(K + "/runtime/game-files.json", {headers: {Origin: "https://evil.example"}});
check("other origin: no ACAO", !r.headers.get("access-control-allow-origin"), `${r.status} acao=${r.headers.get("access-control-allow-origin")}`);
r = await fetch(K + "/__upload/stat?key=native-browser.html");
check("upload API not reachable via /k/", r.status !== 200, r.status);
console.log(fails ? `${fails} FAILED` : "ALL PASS");
process.exitCode = fails ? 1 : 0;
