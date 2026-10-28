// Smoke-check the deployed Worker. Reads tokens from .secrets.local.json and never prints them.
//   node scripts/verify-prod.mjs
import fs from "node:fs";
import path from "node:path";
import {CF_ROOT} from "./common.mjs";
const s = JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8"));
const B = s.WORKER_URL;
const list = JSON.parse(fs.readFileSync(path.join(CF_ROOT, "out", "upload-list.json"), "utf8"));
const siteDefault = path.join(CF_ROOT, "out", "site-default-bundle.json");   // promote-bundle.mjs
const hash = fs.existsSync(siteDefault) ? JSON.parse(fs.readFileSync(siteDefault, "utf8")).bundle : list.bundle;
let fails = 0;
const check = (name, ok, extra = "") => { console.log(`${ok ? "PASS" : "FAIL"} ${name} ${extra}`); if (!ok) ++fails; };
const get = (p, h = {}, method = "GET") => fetch(B + p, {method, headers: h, redirect: "manual"});

let r = await get("/");
check("no token -> 401", r.status === 401, r.status);
r = await get("/", {Accept: "text/html"});
check("no token html -> friendly 401 page", r.status === 401 && (await r.text()).includes("Access required"), r.status);
r = await get("/?key=wrong-key-xxxxxxxxxxxxxxxxxxxxxxx");
check("bad key -> 401", r.status === 401, r.status);
r = await get("/__upload/stat?key=native-browser.html");
check("upload API without UPLOAD_TOKEN -> 401", r.status === 401, r.status);
r = await get("/__upload/stat?key=native-browser.html", {"X-Upload-Token": s.ACCESS_TOKEN});
check("upload API with ACCESS_TOKEN -> 401", r.status === 401, r.status);

r = await get(`/?start=nomadfree&key=${encodeURIComponent(s.ACCESS_TOKEN)}`);
const setCookie = r.headers.get("set-cookie") || "";
const cookie = setCookie.split(";")[0];
check("key -> 302 + gate cookie", r.status === 302 && cookie.startsWith("cp2077_gate=") && /HttpOnly/i.test(setCookie) && /Secure/i.test(setCookie),
  `${r.status} loc=${r.headers.get("location")} max-age=${/Max-Age=(\d+)/i.exec(setCookie)?.[1]} samesite=${/SameSite=(\w+)/i.exec(setCookie)?.[1]}`);
check("redirect strips key", !(r.headers.get("location") || "").includes("key="));
const C = {Cookie: cookie};

r = await get("/", C);
const page = await r.text();
check("page 200 with cookie", r.status === 200 && page.includes(hash), r.status);
check("COOP same-origin", r.headers.get("cross-origin-opener-policy") === "same-origin");
check("COEP require-corp", r.headers.get("cross-origin-embedder-policy") === "require-corp");
check("CORP same-origin", r.headers.get("cross-origin-resource-policy") === "same-origin");
check("page has no localhost URL", !/127\.0\.0\.1|localhost/.test(page));

r = await get("/runtime/game-files.json", C);
const man = r.ok ? await r.json() : null;
check("game-files.json", r.status === 200 && man && man.files?.length > 0, `${r.status} files=${man?.files?.length}`);

r = await get(`/runtime/engine-versions/${hash}/cp2077.wasm`, {...C, "Accept-Encoding": "gzip"}, "HEAD");
check("wasm gzip (HEAD)", r.status === 200 && r.headers.get("content-encoding") === "gzip" && /immutable/.test(r.headers.get("cache-control") || ""),
  `${r.status} enc=${r.headers.get("content-encoding")} len=${r.headers.get("content-length")} cc=${r.headers.get("cache-control")}`);
r = await get(`/runtime/engine-versions/${hash}/cp2077.wasm`, {...C, Range: "bytes=0-3"});
const magic = Buffer.from(await r.arrayBuffer());
check("wasm identity range", r.status === 206 && magic.equals(Buffer.from([0, 0x61, 0x73, 0x6d])), r.status);

r = await get("/runtime/live-telemetry.js", C);
check("telemetry stub served", r.ok && (await r.text()).includes("[cloudflare R2 copy]"), r.status);

for (const p of ["/sdk/bink2w64.dll", "/game/engine/shader_final.cache", "/game/r6/cache/tweakdb.bin",
  "/game/archive/pc/content/basegame_4_gamedata.archive", "/game/archive/pc/content/basegame_zz_nomad.archive"]) {
  const e = man?.files?.find(f => f.path === p);
  r = await get(p, {...C, Range: "bytes=65536-131071"});
  const body = Buffer.from(await r.arrayBuffer());
  check(`range read ${p}`, r.status === 206 && body.length === 65536 && (!e || r.headers.get("etag") === e.etag),
    `${r.status} len=${body.length} cr=${r.headers.get("content-range")}`);
}
console.log(fails ? `${fails} FAILED` : "ALL PASS");
process.exitCode = fails ? 1 : 0;
