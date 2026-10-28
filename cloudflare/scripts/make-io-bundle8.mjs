// [io8] v8: io7 + lookahead hints for an [ioahead] engine base (?ioLookahead=N, ioHintInflight, ioHintWarmLanes) and
// per-thread read stalls / non-IO-thread miss call sites in ioTrace. Sources: src/io8-*.js. Usage: --base <[ioahead] base sha>.
// [io7] v7: io6 + boot-phase warming: boot,menu lists first with <= 4 warm lanes until the engine is Running, the
// rest (common-rest|common, jump lifepath) once the main menu is up or after 45 s; phase marks in recordings.
// [io6] v6: io5 + ioWarm defaults to common, jump=newgame lifepath warmed at boot, lifepath detected from reads
// (runtime/warm/lifepaths.json), warm lanes throttled while game sync reads are in flight. Sources: src/io6-*.js.
// [io5] v5: io4 + large reads persisted block-aligned (repeat-visit network leak), lifepath-aware warming:
// the engine's 'CP2077 missionflow: StartNewGame ... lifepathSpawnTag=#q000_<kid|nomad|corpo>' line starts warm
// list streetkid|nomad|corpo; Module.cp2077IoWarm(name) for page scripts. Sources: src/io5-*.js.
// [io4] v4: io3 + OPFS persistent block tier via the IO helper (ioPersist=1, ioPersistMB=4096), warm lists
// (ioWarm=name[,name] -> /runtime/warm/<name>.json, ioWarmInflight=16), sequential widening of sync misses
// (ioSeqWiden=4 in shared mode), demand-order recording ("CP2077 ioWarm record:"). Sources: src/io4-*.js.
// Usage: node scripts/make-io-bundle4.mjs --base <64-hex engine bundle>
// [io3] v3: like v2 but prefetch only after ioSeqMin (3) sequential blocks, ioPrefetch default 2 in shared mode,
// prefetch budget 4 MiB + ioPrefetchPct (10)% of demand bytes, per-file auto-off when used/(used+wasted) < 50%.
// Sources: src/io3-shared-cache.js, src/io3-helper.js; page native-browser-cfio8.html.
// [io2] v2 of make-io-bundle.mjs: adds ?ioShared=1 (one SharedArrayBuffer block cache for all pthreads, in-flight
// dedup), ?ioSharedMB= (default 256), ?ioPrefetch=K (async, adaptive: after 2+ consecutive blocks, via a helper
// worker), ?ioInflight=N (async prefetch concurrency, default 4). Everything below from v1 still applies.
// Make a NEW versioned engine bundle whose pthread resource reader (CpResRead) gets opt-in switches and a
// worker-side asset trace. The live bundle and native-browser.html are never modified.
//   node scripts/make-io-bundle.mjs [--base <64-hex bundle>]   (default: build/active-engine-bundle.json)
// Output: build/game/engine-versions/<sha256 of new cp2077.js>/ (wasm/data copied byte-identical) and the
// test page build/game/native-browser-cfio8.html (served as /runtime/native-browser-cfio8.html).
// Switches (page URL; forwarded to the pthread workers via the cp2077.js script URL query):
//   ioBlockMB=1..16   block size (default 1)   ioCacheMB=4..1024  per-worker LRU (default 16)
//   ioAhead=0..32     extra blocks fetched in the same request on a miss (default 0)
//   assetTrace=1 | ioTrace=1   workers post per-block traffic to the main thread, which logs
//                    "CP2077 ioTrace: {...}" and "CP2077 ioTrace ranges: {...}" every 30 s.
// With no switches the reader behaves exactly like the base bundle (1 MiB blocks, 16 MiB LRU, no read-ahead).
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import crypto from "node:crypto";
import {RUNTIME, CF_ROOT} from "./common.mjs";
const CF_SRC = path.join(CF_ROOT, "src");

const ORIGINAL_CPRESREAD_SHA = "999ac524c191ca2b55a2faf19ce2e821393d600bd454ee5ffa71c66c42fd1748";
const W32_CPRESREAD_SHA = "fdbcefa54dbc91aa79f4ab860bc6af497326a52498b28d1de051f6b17265b15b"; // [w32] iPhone build reader (unsigned dest, 2 MiB iOS cache, 1 MiB pieces): same contract
const i = process.argv.indexOf("--base");
const base = i > 0 ? process.argv[i + 1] : JSON.parse(fs.readFileSync(path.join(RUNTIME, "..", "active-engine-bundle.json"), "utf8")).BuildID;
const baseDir = path.join(RUNTIME, "engine-versions", base);
let js = fs.readFileSync(path.join(baseDir, "cp2077.js"), "utf8");
const sha = s => crypto.createHash("sha256").update(s).digest("hex");

// ---- 1. replace CpResRead (verified byte-for-byte against the version this patch was written for)
const startTok = "function CpResRead(", endTok = js.includes("function CpResHintJS(") ? "function CpResHintJS(" : "function cp2077StorageFlushBegin(";
const a = js.indexOf(startTok), b = js.indexOf(endTok);
if (a < 0 || b < a || js.indexOf(startTok, a + 1) >= 0) throw new Error("CpResRead not found exactly once");
const original = js.slice(a, b);
if (![ORIGINAL_CPRESREAD_SHA, W32_CPRESREAD_SHA].includes(sha(original.replace(/\r\n/g, "\n"))) && sha(original) !== ORIGINAL_CPRESREAD_SHA)
  throw new Error("CpResRead differs from the audited version; refusing to patch");

const NEW_CPRESREAD = fs.readFileSync(path.join(CF_SRC, "io8-cpresread.js"), "utf8");
// Inline every SBC_* constant as a literal: the main thread calls sbcCreate() from PThread.initMainThread(),
// which runs before the injected top-level `var SBC_... = n` statement executes (tc38 crash: SBC_HDR undefined
// -> SharedArrayBuffer(NaN) -> zero-length header -> RangeError in every pthread).
const SHARED_RAW = fs.readFileSync(path.join(CF_SRC, "io8-shared-cache.js"), "utf8");
const SBC_CONSTS = new Map([...SHARED_RAW.matchAll(/\b(SBC_[A-Z_]+) = (\d+)/g)].map(m => [m[1], m[2]]));
if (SBC_CONSTS.size < 20) throw new Error("SBC constants not parsed: " + SBC_CONSTS.size);
const inlineConsts = src => src.replace(/\bSBC_[A-Z_]+\b(?! = \d)/g, name => {
  if (!SBC_CONSTS.has(name)) throw new Error("unknown constant " + name);
  return SBC_CONSTS.get(name) + "/*" + name + "*/";
});
const SHARED_SRC = inlineConsts(SHARED_RAW);
js = js.slice(0, a) + SHARED_SRC + "\n" + inlineConsts(NEW_CPRESREAD) + js.slice(b);
// The base bundle uses CRLF line endings; accept either.
const once = (from, to) => {
  if (js.split(from).length !== 2) { from = from.split("\n").join("\r\n"); to = to.split("\n").join("\r\n"); }
  if (js.split(from).length !== 2) throw new Error("anchor not found exactly once: " + from.slice(0, 60));
  js = js.split(from).join(to);
};

// ---- 2. main-thread handler for worker io trace messages
const faultLine = 'if (cmd === "cp2077NativeFault")';
once(faultLine, 'if (cmd === "cp2077IoTrace") { cp2077IoTraceMain(worker.workerID, d.d); return; }\n      ' + faultLine);
// ---- 2b. [io2] hand the shared cache to each pthread worker as its first message (only with ?ioShared=1)
once("    worker.workerID = PThread.nextWorkerID++;\n    PThread.unusedWorkers.push(worker);",
  "    worker.workerID = PThread.nextWorkerID++;\n    cp2077IoSharedPost(worker);\n    PThread.unusedWorkers.push(worker);");
// ---- 2c. [io2] pthread side: accept it before the Emscripten load command
// ---- 2e. [io7] main-thread printErr lines also pass the phase watcher (pthread lines go through 2d)
once("  if (Module[\"printErr\"]) err = Module[\"printErr\"];", "  if (Module[\"printErr\"]) { const cpPrintErr = Module[\"printErr\"]; err = (...a) => { try { cp2077IoWatchLine(a[0]); } catch (e) {} return cpPrintErr(...a); }; }");
// ---- 2d. [io5] main thread: every proxied pthread printErr line passes the lifepath watcher first
once("        Module[d.handler](...d.args);", "        if (d.handler === \"printErr\") cp2077IoWatchLine(d.args[0]);\n        Module[d.handler](...d.args);");
once("      var cmd = msgData.cmd;\n      if (cmd == 1) {",
  "      var cmd = msgData.cmd;\n      if (cmd === \"cp2077IoShared\") { globalThis.__cpIoShared = msgData.d; return; }\n      if (cmd == 1) {");

// ---- 3. write the new immutable bundle
// The helper is part of the bundle: version the directory by both files.
const newHash = sha(js + "\n/*helper*/\n" + fs.readFileSync(path.join(CF_SRC, "io8-helper.js"), "utf8") + "\n/*cache*/\n" + SHARED_SRC);
const outDir = path.join(RUNTIME, "engine-versions", newHash);
fs.mkdirSync(outDir, {recursive: true});
fs.writeFileSync(path.join(outDir, "cp2077.js"), js);
fs.writeFileSync(path.join(outDir, "cp2077.js.gz"), zlib.gzipSync(js, {level: 9}));
fs.writeFileSync(path.join(outDir, "cp2077-io-helper.js"), "// [cloudflare io2] IO helper (generated by make-io-bundle8.mjs)\n" + SHARED_SRC + "\n" + inlineConsts(fs.readFileSync(path.join(CF_SRC, "io8-helper.js"), "utf8")));
for (const f of ["cp2077.wasm", "cp2077.data", "cp2077.wasm.gz", "cp2077.data.gz"]) {
  const dst = path.join(outDir, f);
  if (!fs.existsSync(dst) || fs.statSync(dst).size !== fs.statSync(path.join(baseDir, f)).size) fs.copyFileSync(path.join(baseDir, f), dst);
}
const baseBundle = JSON.parse(fs.readFileSync(path.join(baseDir, "bundle.json"), "utf8"));
fs.writeFileSync(path.join(outDir, "bundle.json"), JSON.stringify({BuildID: newHash, Files: {...baseBundle.Files, "cp2077.js": sha(js)},
  Scope: `cloudflare io variant of ${base}: CpResRead switches ioBlockMB/ioCacheMB/ioAhead, io2 shared cache ioShared/ioSharedMB/ioPrefetch/ioInflight, io3 selective prefetch, io4 ioPersist/ioPersistMB/ioWarm/ioWarmInflight/ioSeqWiden, io5 aligned large-read persistence + lifepath warming (ioWarmLifepath), io6 common boot warm + jump/read-detected lifepath warm + warm throttling, io7 boot-phase warming (ioWarmBoot/ioWarmBootLanes/ioWarmDelay) + phase marks + worker ioTrace/ioWarm record, io8 lookahead hints (ioLookahead/ioHintInflight/ioHintWarmLanes) + per-thread ioTrace. wasm/data identical.`, Base: base}, null, 2));

// ---- 4. test page: live page with the new hash; cp2077.js URL carries the page query to the pthreads
const pageSrc = fs.readFileSync(path.join(RUNTIME, "..", "..", "native-browser.html"), "utf8");
// The test page is the local native-browser.html pointed at the new bundle (the deploy /game page is separate).
const pageBase = /engine-versions\/([0-9a-f]{64})\/cp2077\.js/.exec(pageSrc)?.[1];
if (!pageBase) throw new Error("native-browser.html has no engine script tag");
if (pageBase !== base) console.warn("note: test page template references " + pageBase.slice(0, 8) + ", bundle base is " + base.slice(0, 8) + " (page arguments may differ from the deploy page)");
const tag = `<script src="/runtime/engine-versions/${pageBase}/cp2077.js"></script>`;
if (pageSrc.split(tag).length !== 2) throw new Error("engine script tag not found exactly once");
let page = pageSrc.replace(tag, `<script>/* [cloudflare io] switches reach the pthread workers through the script URL */document.write('<script src="/runtime/engine-versions/${newHash}/cp2077.js'+location.search.replace(/[^\\w=&.,%?-]/g,'')+'"><\\/script>')</script>`);
page = page.split(pageBase).join(newHash).replace("<title>CP2077 native browser port</title>", "<title>CP2077 native browser port (cf io8)</title>");
fs.writeFileSync(path.join(RUNTIME, "native-browser-cfio8.html"), page);
console.log(`io bundle ${newHash}\n  base ${base}\n  dir  ${outDir}\n  page ${path.join(RUNTIME, "native-browser-cfio8.html")}`);
