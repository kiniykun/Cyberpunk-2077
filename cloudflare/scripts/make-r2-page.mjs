// Build the R2 copy of the page with the Cyberpunk loading screen. The local D:\2077\native-browser.html is
// only READ; the result goes to out/r2-overrides/native-browser.html (served at /) together with
// out/r2-overrides/runtime/loading/{loader.js,logo.png}, and out/loader-upload-list.json for the uploader:
//   node scripts/make-r2-page.mjs [--bundle <sha>] [--io-bundle <sha>]
//   node scripts/upload-multipart.mjs --list out/loader-upload-list.json
// Site default engine bundle: --bundle, else out/site-default-bundle.json (written by promote-bundle.mjs),
// else the local active bundle. The page also understands ?engine=<sha> and ?io=shared (engine-select.js).
import fs from "node:fs";
import path from "node:path";
import {CF_ROOT, PORT_ROOT, RUNTIME, OUT, activeBundle} from "./common.mjs";

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const HEX = /^[0-9a-f]{64}$/;
const SRC = path.join(CF_ROOT, "public", "loading");
const OV = path.join(OUT, "r2-overrides");
const DEFAULT_FILE = path.join(OUT, "site-default-bundle.json");
const localHash = activeBundle();                 // the bundle native-browser.html references (checked consistent)
// out/site-default-bundle.json: {bundle, params?, classic?}. With params, `bundle` is the io default booted with
// those params and `classic` is the plain fallback (?io=classic) the page is built on; without params they coincide.
const site = fs.existsSync(DEFAULT_FILE) ? JSON.parse(fs.readFileSync(DEFAULT_FILE, "utf8")) : {bundle: localHash};
const defaultParams = site.params && Object.keys(site.params).length ? site.params : null;
const hash = arg("--bundle") || site.classic || site.bundle;      // classic: the page's built-in bundle (+ loader sizes)
const defaultBundle = defaultParams ? site.bundle : hash;
const ioBundle = arg("--io-bundle", "30ceeb825e0e2277b57615d970c0171f45dfb0481373af944d1ecdcd3c487330");   // io3
const ioStreamBundle = arg("--io-stream-bundle", "e199e2407a6b8608b4335953185c3d6284d961faefff1413d778a40945263987");   // io6 (?io=stream), based on 3f3a29d9
for (const h of [hash, defaultBundle, ioBundle, ioStreamBundle]) if (!HEX.test(h)) throw new Error("not a 64-hex bundle id: " + h);
// [w32] optional wasm32 bundle for browsers without memory64 (site-default-bundle.json "w32": {bundle, params}).
const w32 = site.w32 && site.w32.bundle ? site.w32 : null;
if (w32 && !HEX.test(w32.bundle)) throw new Error("not a 64-hex w32 bundle id: " + w32.bundle);
const w32Params = w32 && w32.params && Object.keys(w32.params).length ? w32.params : null;
if (w32Params) for (const [k, v] of Object.entries(w32Params))
  if (!/^[A-Za-z][A-Za-z0-9]{0,40}$/.test(k) || !/^[\w.,-]{0,64}$/.test(String(v))) throw new Error(`bad w32 param ${k}=${v}`);
if (defaultParams) for (const [k, v] of Object.entries(defaultParams))
  if (!/^[A-Za-z][A-Za-z0-9]{0,40}$/.test(k) || !/^[\w.,|-]{0,64}$/.test(String(v))) throw new Error(`bad default param ${k}=${v}`);   // "|": warm list alternatives (ioWarmBoot)
const bundle = path.join(RUNTIME, "engine-versions", hash);
const size = f => fs.statSync(path.join(bundle, f)).size;
const cfg = {bundle: hash, wasmSize: size("cp2077.wasm"), wasmGz: size("cp2077.wasm.gz"),
  dataSize: size("cp2077.data"), dataGz: size("cp2077.data.gz"), jsGz: size("cp2077.js.gz")};

let page = fs.readFileSync(path.join(PORT_ROOT, "native-browser.html"), "utf8");
const css = fs.readFileSync(path.join(SRC, "loader.css"), "utf8");
const fragment = fs.readFileSync(path.join(SRC, "loader-fragment.html"), "utf8");
const engineSelect = fs.readFileSync(path.join(SRC, "engine-select.js"), "utf8").replace(/__IO_BUNDLE__/g, ioBundle).replace(/__IO_STREAM_BUNDLE__/g, ioStreamBundle)
  .replace(/__DEFAULT_BUNDLE__/g, defaultBundle).replace(/__CLASSIC_BUNDLE__/g, hash)
  .replace(/__DEFAULT_PARAMS__/g, JSON.stringify(defaultParams))
  .replace(/__W32_BUNDLE__/g, JSON.stringify(w32 ? w32.bundle : null)).replace(/__W32_PARAMS__/g, JSON.stringify(w32Params));
if (/__[A-Z_]+__/.test(engineSelect)) throw new Error("unreplaced placeholder in engine-select.js");
const engineTag = `<script src="/runtime/engine-versions/${localHash}/cp2077.js"></script>`;
const locate = `new URL('/runtime/engine-versions/${localHash}/' + name, document.baseURI)`;
const must = (cond, what) => { if (!cond) throw new Error("page anchor missing: " + what); };
must(page.includes("</head>"), "</head>");
must(/<body>/.test(page), "<body>");
must(page.includes(engineTag), "engine script tag");
must(page.includes(locate), "locateFile");
must(page.includes("/runtime/live-telemetry.js"), "telemetry tag");

page = page.replace(/<title>[^<]*<\/title>/, "<title>Cyberpunk 2077 Browser</title>");
page = page.replace("</head>",
  `  <!-- [cloudflare loader] R2 copy only (scripts/make-r2-page.mjs) -->
  <script>document.documentElement.classList.add('cp-loading');if(!/[?&]debug=1(&|$)/.test(location.search))document.documentElement.classList.add('cp-nodebug');</script>
  <script>/* [engine-select] public/loading/engine-select.js */
${engineSelect}</script>
  <script>/* [capcheck] public/loading/capcheck.js (after engine-select: may set CP_ENGINE_BLOCK) */
${fs.readFileSync(path.join(SRC, "capcheck.js"), "utf8")}</script>
  <link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link rel="stylesheet" crossorigin="anonymous" href="https://fonts.googleapis.com/css2?family=Rajdhani:wght@500;600;700&family=JetBrains+Mono:wght@400;500&display=swap">
  <style>
${css}  </style>
</head>`);
page = page.replace(/<body>/, "<body>\n" + fragment);
// [touch] on-screen touch controls (public/touch/): inert unless a coarse pointer / first touch / ?touch=1.
// Function replacer: the JS may contain $ sequences. The Vercel page is built from this page, so it inherits them.
const touchDir = path.join(CF_ROOT, "public", "touch");
const touchJs = fs.readFileSync(path.join(touchDir, "touch.js"), "utf8"), touchCss = fs.readFileSync(path.join(touchDir, "touch.css"), "utf8");
if (/<\/script|<\/style/i.test(touchJs + touchCss)) throw new Error("touch.js/css must not contain a closing script/style tag");
page = page.replace("</head>", () => `  <!-- [touch] public/touch/touch.{css,js} -->
  <style id="cp-touch-css">
${touchCss}  </style>
  <script id="cp-touch-js">
${touchJs}</script>
</head>`);
// [support] "Support the project" button over the game's main menu (public/support/support.{css,js}).
const supDir = path.join(CF_ROOT, "public", "support");
const supJs = fs.readFileSync(path.join(supDir, "support.js"), "utf8"), supCss = fs.readFileSync(path.join(supDir, "support.css"), "utf8");
if (/<\/script|<\/style/i.test(supJs + supCss)) throw new Error("support.js/css must not contain a closing script/style tag");
page = page.replace("</head>", () => `  <!-- [support] public/support/support.{css,js} -->
  <style id="cp-support-css">
${supCss}  </style>
  <script id="cp-support-js">
${supJs}</script>
</head>`);
// engine: site default, or ?engine= / ?io=shared (CP_ENGINE, page query forwarded to the workers via CP_ENGINE_Q)
page = page.replace(engineTag, `<script>if(!globalThis.CP_ENGINE_BLOCK)document.write('<script src="/runtime/engine-versions/'+(globalThis.CP_ENGINE||'${hash}')+'/cp2077.js?loaderio=1'+(globalThis.CP_ENGINE_Q||'')+'"><\\/script>')</script>
  <script>globalThis.cp2077LoaderConfig=${JSON.stringify(cfg)};</script>
  <script src="/runtime/loading/loader.js"></script>`);
page = page.replace(locate, `new URL('/runtime/engine-versions/'+(globalThis.CP_ENGINE||'${hash}')+'/' + name, document.baseURI)`);
if (page.includes(localHash) && localHash !== hash) throw new Error("local bundle hash left in the page");
if (/127\.0\.0\.1|localhost/.test(page)) throw new Error("page still references localhost");

fs.mkdirSync(path.join(OV, "runtime", "loading"), {recursive: true});
fs.writeFileSync(path.join(OV, "native-browser.html"), page);
fs.copyFileSync(path.join(SRC, "loader.js"), path.join(OV, "runtime", "loading", "loader.js"));
fs.copyFileSync(path.join(SRC, "logo.png"), path.join(OV, "runtime", "loading", "logo.png"));
const item = (key, file, group) => ({key, file, size: fs.statSync(file).size, group});
const items = [
  item("runtime/loading/logo.png", path.join(OV, "runtime", "loading", "logo.png"), "loader"),
  item("runtime/loading/loader.js", path.join(OV, "runtime", "loading", "loader.js"), "loader"),
  item("runtime/pipeline-warmup.json", path.join(RUNTIME, "pipeline-warmup.json"), "runtime"),   // FetchWarmManifest (was missing from R2)
  item("native-browser.html", path.join(OV, "native-browser.html"), "page"),       // last: page only after its scripts exist
];
fs.writeFileSync(path.join(OUT, "loader-upload-list.json"), JSON.stringify({bundle: hash, bucket: "cp2077-assets", generated: new Date().toISOString(), items}, null, 1));
console.log(`R2 page with loader: ${page.length} bytes, default ${defaultBundle.slice(0, 12)}${defaultParams ? " +" + new URLSearchParams(defaultParams) : ""}, classic ${hash.slice(0, 12)}, io=shared ${ioBundle.slice(0, 12)}, io=stream ${ioStreamBundle.slice(0, 12)}; wrote out/loader-upload-list.json`);
