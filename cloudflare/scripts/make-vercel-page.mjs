// Plan B: build the cross-origin /game page for another site (cyberpunk2077.lol on Vercel).
// Input: the R2 page with loader (out/r2-overrides/native-browser.html, from make-r2-page.mjs).
// Output: out/vercel-game/index.html — the same page, with xo-prelude.js inlined first and every
// /runtime/ script tag pointed at <cf>/k/<token>/runtime/... (token read from ?key= at runtime, never baked in).
//   node scripts/make-r2-page.mjs && node scripts/make-vercel-page.mjs [--cf https://cp2077-port...workers.dev]
//   [--copy-to vercel-site/game/index.html]
import fs from "node:fs";
import path from "node:path";
import {CF_ROOT, OUT} from "./common.mjs";

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const cf = arg("--cf", JSON.parse(fs.readFileSync(path.join(CF_ROOT, ".secrets.local.json"), "utf8")).WORKER_URL).replace(/\/$/, "");
const cfFallbackArg = arg("--cf-fallback", null);
const cfFallback = cfFallbackArg ? cfFallbackArg.replace(/\/$/, "") : null;   // [cdn-fallback] used if --cf does not answer
const prelude = fs.readFileSync(path.join(CF_ROOT, "public", "loading", "xo-prelude.js"), "utf8");
let page = fs.readFileSync(path.join(OUT, "r2-overrides", "native-browser.html"), "utf8");

const head = `<head>
  <script>/* [plan-b] cross-origin asset prelude (cloudflare/public/loading/xo-prelude.js) */
${prelude}
__cpXoBoot(${JSON.stringify(cfFallback ? {cfOrigin: cf, cfFallback} : {cfOrigin: cf})});
  </script>
  <style>:root{--cp-logo-url:url(/game/logo.png)}</style>
  <link rel="icon" type="image/png" sizes="32x32" href="/assets/icon-32.png" />
  <link rel="icon" type="image/png" sizes="512x512" href="/assets/icon-512.png" />
  <link rel="apple-touch-icon" href="/assets/icon-180.png" />`;
if (!/<head>/.test(page)) throw new Error("no <head>");
if (!page.includes('<script id="cp-touch-js">')) throw new Error("touch controls missing from the R2 page (make-r2-page.mjs, public/touch/)");   // [touch]
page = page.replace("<head>", head);
// document.write('<script src="/runtime/...">') strings -> concatenate the CF base
let n1 = 0, n2 = 0;
page = page.replace(/'<script src="\/runtime\//g, () => (++n1, `'<script src="'+CPB+'/runtime/`));
// engine script: only once an access code exists (CPB) and engine-select did not block it
if (!page.includes("<script>if(!globalThis.CP_ENGINE_BLOCK)document.write(")) throw new Error("engine document.write not found");
page = page.replace("<script>if(!globalThis.CP_ENGINE_BLOCK)document.write(", "<script>if(CPB&&!globalThis.CP_ENGINE_BLOCK)document.write(");
// the loader UI script is served by the page's own site (works before an access code exists)
page = page.replace('<script src="/runtime/loading/loader.js"></script>', '<script src="/game/loader.js"></script>');
// static <script src="/runtime/..."></script> -> document.write with the CF base (keeps parse order + currentScript)
page = page.replace(/<script src="\/runtime\/([^"]+)"><\/script>/g, (_, rel) => (++n2, `<script>if(CPB)document.write('<script src="'+CPB+'/runtime/${rel}"><\\/script>')</script>`));
if (/src="\/runtime\//.test(page)) throw new Error("unrewritten /runtime/ src left");
if (/127\.0\.0\.1|localhost/.test(page)) throw new Error("page references localhost");
const outDir = path.join(OUT, "vercel-game");
fs.mkdirSync(outDir, {recursive: true});
fs.writeFileSync(path.join(outDir, "index.html"), page);
const copyTo = arg("--copy-to");
const logo = path.join(CF_ROOT, "public", "loading", "logo.png");   // served by the page's own site at /game/logo.png
const loaderJs = path.join(CF_ROOT, "public", "loading", "loader.js");
fs.copyFileSync(logo, path.join(outDir, "logo.png"));
fs.copyFileSync(loaderJs, path.join(outDir, "loader.js"));
if (copyTo) {
  fs.mkdirSync(path.dirname(path.resolve(CF_ROOT, copyTo)), {recursive: true});
  fs.copyFileSync(loaderJs, path.join(path.dirname(path.resolve(CF_ROOT, copyTo)), "loader.js"));
  const dst = path.resolve(CF_ROOT, copyTo);
  fs.mkdirSync(path.dirname(dst), {recursive: true});
  fs.writeFileSync(dst, page);
  fs.copyFileSync(logo, path.join(path.dirname(dst), "logo.png"));
}
console.log(`vercel /game page: ${page.length} bytes, cf ${cf}, rewrote ${n1} document.write + ${n2} static script tags${copyTo ? ", copied to " + copyTo : ""}`);
