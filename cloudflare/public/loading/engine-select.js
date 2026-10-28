// Engine bundle selection (inlined into <head> of the R2 page and the Vercel /game page by make-r2-page.mjs).
//   (no switch)           SITE DEFAULT: __DEFAULT_BUNDLE__ with its io params (out/site-default-bundle.json;
//                         promote-bundle.mjs). Without params the page's classic bundle boots as-is.
//   ?io=classic           opt-out: the classic bundle (__CLASSIC_BUNDLE__), no io params
//   ?engine=<64-hex sha>  boot that bundle from R2 runtime/engine-versions/<sha>/ (test builds; no io params added)
//   ?io=shared            io3 shared-cache bundle (__IO_BUNDLE__) with ioShared=1&ioSharedMB=128&ioInflight=4
//   (no memory64)         [w32] browsers without 64-bit WebAssembly (iPhone/iPad Safari) but with wasm threads,
//                         cross-origin isolation, SharedArrayBuffer and WebGPU boot the wasm32 bundle __W32_BUNDLE__
//                         with its params (out/site-default-bundle.json "w32", e.g. mem=ios); capcheck.js then skips
//                         only its memory64 test (CP_ENGINE_W32). Explicit ?engine= / ?io= still win.
//   ?io=stream            io6 bundle (__IO_STREAM_BUNDLE__) with the above + ioPersist=1&ioPersistMB=6144 (warm lists:
//                         io6 defaults; OPFS tier in the helper worker, OPFS belongs to the page's origin)
// The page query is forwarded to the pthread workers through the engine script URL (CP_ENGINE_Q), and on the
// cross-origin page the worker stubs expose it as location.search (cfg.locShim). Explicit URL params always win.
(function () {
  var q = new URLSearchParams(location.search), HEX = /^[0-9a-f]{64}$/;
  var IO = "__IO_BUNDLE__", IO_STREAM = "__IO_STREAM_BUNDLE__", DEF = "__DEFAULT_BUNDLE__", CLASSIC = "__CLASSIC_BUNDLE__";
  var DEF_PARAMS = __DEFAULT_PARAMS__;
  var W32 = __W32_BUNDLE__, W32_PARAMS = __W32_PARAMS__;   // null when no wasm32 bundle is configured
  var STREAM_PARAMS = {ioShared: "1", ioSharedMB: "128", ioInflight: "4", ioPersist: "1", ioPersistMB: "6144"};
  var G = globalThis, sel = null, defs = null, changed = false;
  G.CP_ENGINE = null; G.CP_ENGINE_Q = ""; G.CP_ENGINE_BLOCK = false; G.CP_ENGINE_W32 = false;
  var wasmOk = function (b) { try { return typeof WebAssembly === "object" && WebAssembly.validate(new Uint8Array(b)); } catch (err) { return false; } };
  var noMemory64 = function () { return !wasmOk([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 5, 3, 1, 4, 0]); };    // (memory i64 0)
  var wasm32Ready = function () {                     // shared (threads) wasm memory + isolation + WebGPU
    return wasmOk([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 5, 4, 1, 3, 1, 1]) && !!G.crossOriginIsolated &&
      typeof SharedArrayBuffer === "function" && !!(G.navigator && navigator.gpu);
  };
  var fail = function (m) { G.cp2077LoaderPreError = m; if (G.cp2077LoaderFail) try { G.cp2077LoaderFail(m); } catch (e) {} };
  var e = q.get("engine"), io = q.get("io");
  if (e != null) {
    if (HEX.test(e)) { sel = e; if (q.get("ios") === "1" && W32) { defs = W32_PARAMS; G.CP_ENGINE_W32 = true; } }   // [w32-test] ?ios=1&engine=<sha>: test a wasm32 bundle in iPhone mode
    else { G.CP_ENGINE_BLOCK = true; return fail("Invalid ?engine= value: it must be a 64-character hex bundle id."); }
  } else if (io === "classic") {
    sel = null;                                     // the page's built-in (classic) bundle, no io params
  } else if (io === "shared") {
    sel = IO; defs = {ioShared: "1", ioSharedMB: "128", ioInflight: "4"};
  } else if (io === "stream") {
    sel = IO_STREAM; defs = STREAM_PARAMS;
  } else if (W32 && (noMemory64() || location.pathname === "/game/ios" || location.pathname === "/game/ios/" || q.get("ios") === "1") && wasm32Ready()) {   // [ios-route] user 2026-10-22: /game/ios (or ?ios=1) boots the iPhone build + its params on any device
    sel = W32; defs = W32_PARAMS; G.CP_ENGINE_W32 = true;   // [w32] iPhone/iPad Safari etc.
  } else if (DEF !== CLASSIC) {
    sel = DEF; defs = DEF_PARAMS;                   // site default (e.g. io6 + its params)
  }
  if (defs) for (var d in defs) if (!q.has(d)) { q.set(d, defs[d]); changed = true; }
  // [lowmem] 2026-10-24: Android device-lost (GPU process killed) was more common on <= 4 GB phones; there the
  // wasm64 site default drops two texture mips instead of the [texdrop] default of one (explicit ?texDrop wins).
  if (sel && sel === DEF && /Android/.test(navigator.userAgent) && (navigator.deviceMemory || 8) <= 4 && !q.has("texDrop")) { q.set("texDrop", "2"); changed = true; }
  // [androidpotato] 2026-10-24 city A/B (clean paired runs): potato=2 (0.8 render scale, small-object/prop/foliage/decal
  // distance caps) ~49.6 vs ~44 fps on desktop; phones are far slower, so Android gets it by default (iPhone already does).
  if (sel && sel === DEF && /Android/.test(navigator.userAgent) && !q.has("potato")) { q.set("potato", "2"); changed = true; }
  // [warmflood] 2026-10-24 paired runs: floodKeep=1 (loading screen keeps 1 job worker free instead of 2) world load
  // 5.6-5.8 vs 6.8-7.0 s with local data. First visits are bimodal (~40-55 s or ~85-115 s) whatever floodKeep is
  // (fixed-harness A/B), so every visit gets 1. Explicit ?floodKeep wins.
  if (sel && sel === DEF && !q.has("floodKeep")) { q.set("floodKeep", "1"); changed = true; }
  // [gpulite] loader.js set this after a GPU device loss in this tab: lightest render settings for the retry.
  var lite = false; try { lite = sessionStorage.getItem("cp2077_gpulite") === "1"; } catch (err) {}
  if (sel && lite) { if (q.get("potato") !== "3") { q.set("potato", "3"); changed = true; } if (!(+q.get("texDrop") >= 2)) { q.set("texDrop", "2"); changed = true; } }
  if (changed) { try { history.replaceState(history.state, "", location.pathname + "?" + q + location.hash); } catch (err) {} }
  if (!sel) return;
  G.CP_ENGINE = sel;
  G.CP_ENGINE_Q = "&" + q.toString().replace(/[^\w=&.,%-]/g, "");
  if (G.__cpXo) { G.__cpXo.locShim = true; G.__cpXo.countFetch = true; }
  if (G.CPB === null) return;                       // cross-origin page without an access code: nothing loads yet
  fetch("/runtime/engine-versions/" + sel + "/bundle.json", {cache: "no-store"}).then(function (r) {
    if (!r.ok) fail("Engine bundle " + sel.slice(0, 12) + "… is not on the server (" + r.status + "). Upload it with scripts/upload-bundle.mjs.");
  }, function () {});
})();
// [w32-telemetry] Boot telemetry for the wasm32/iPhone engine (mem=ios): small JSON progress reports posted to the
// Worker's /__probe (one object per page load, overwritten each time, cumulative "events"), so a tab that iOS kills
// still leaves its last state behind. Read back with GET /__upload/probes (admin). No personal data: UA, screen,
// boot marks, engine memory numbers, error messages. Active when engine-select picked the wasm32 bundle, or with
// ?mem=ios / ?telemetry=1; ?telemetry=0 turns it off.
(function () { try {        // never let telemetry break the boot
  var G = globalThis, q = new URLSearchParams(location.search);
  if (q.get("telemetry") === "0" || !(G.CP_ENGINE_W32 || q.get("mem") === "ios" || q.get("telemetry") === "1")) return;
  var URL_ = "https://cp2077-port.fn-web-worker.workers.dev/__probe?id=";
  var id = "b" + (Math.random().toString(36) + Math.random().toString(36)).replace(/[^a-z0-9]/g, "").slice(0, 11);
  var t0 = Date.now(), now = function () { return Math.round(performance.now()); };
  var R = {v: 2, kind: "boot", id: id, ua: navigator.userAgent, t: t0, engine: G.CP_ENGINE, w32: !!G.CP_ENGINE_W32,
    q: location.search.slice(0, 300), screen: G.screen ? [screen.width, screen.height, G.devicePixelRatio || 1] : null,
    cores: navigator.hardwareConcurrency || 0, mem: navigator.deviceMemory || 0,
    stage: "page", seq: 0, events: [], errors: [], tail: [], memPeak: null, last: null, posted: 0};
  G.cp2077Telemetry = R;
  var cut = function (s, n) { n = n || 300; s = String(s); return s.length > n ? s.slice(0, n) + "..." : s; };
  var pending = null, lastPost = 0;
  // [bigint] 2026-10-23 probes: 728 launches died with "Do not know how to serialize a BigInt": an event value from the
  // 64-bit engine (wasm64 pointers/sizes are BigInt) reached JSON.stringify, which threw out of the hooked engine/WebGPU
  // call into the launch. Serialize BigInt as Number and never let the reporter throw into the game.
  var ser = function (o) { return JSON.stringify(o, function (k, v) { return typeof v === "bigint" ? Number(v) : v; }); };
  function send() {
    try { sendUnsafe(); } catch (e) {}
  }
  function sendUnsafe() {
    pending = null; lastPost = Date.now(); R.seq++; R.posted = lastPost - t0; R.uptime = now();
    var body = ser(R);
    while (body.length > 60000 && R.events.length > 20) {   // drop the oldest periodic memory samples first
      var i = -1;
      for (var k = 0; k < R.events.length; k++) if (R.events[k].k === "mem") { i = k; break; }
      R.events.splice(i < 0 ? 0 : i, 1); body = ser(R);
    }
    if (body.length > 60000) { R.tail = R.tail.slice(-5); R.errors = R.errors.slice(0, 5); body = ser(R); }
    try {   // first 15 s: sendBeacon (survives an abrupt tab kill best), then fetch keepalive
      if (!((now() < 15000 || G.CP_ENGINE_W32) && navigator.sendBeacon && navigator.sendBeacon(URL_ + id, body)))   // iOS: beacons throughout
        fetch(URL_ + id, {method: "POST", mode: "no-cors", headers: {"Content-Type": "text/plain"}, body: body, keepalive: body.length < 60000})
          .catch(function () {});
    } catch (e) {}
  }
  function post(urgent) {               // coalesce: at most one post per 2 s unless urgent (or in the first 15 s)
    if (urgent || now() < 15000) { if (pending) clearTimeout(pending); send(); return; }
    if (pending) return;
    pending = setTimeout(send, Math.max(0, 2000 - (Date.now() - lastPost)));
  }
  function ev(k, data, urgent) {
    var e = {k: k, ms: now()}; if (data != null) e.d = data;
    R.events.push(e); if (k !== "mem") R.stage = k; post(urgent);
  }
  ev("page", null, true);
  // [w32-telemetry v3] early boot: pthread workers created, wasm memory size, GPU adapter/device, first submit/frame,
  // the first engine line (main() running), shader-package lines; a 1 s heartbeat for the first 15 s.
  var mems = [], workers = 0;
  var heapMiB = function () { var m = 0; for (var i = 0; i < mems.length; i++) try { m = Math.max(m, mems[i].buffer.byteLength); } catch (e) {} return Math.round(m / 1048576); };
  R.workers = 0;
  try {
    var WK = G.Worker;
    if (WK) {
      G.Worker = function (u, o) {
        var w = o === undefined ? new WK(u) : new WK(u, o);
        workers++; R.workers = workers;
        if (workers <= 40) ev("worker", {n: workers, url: String(u).replace(/^.*\//, "").slice(0, 60), heap: heapMiB()}, true);
        try {   // first message back from the worker = its wasm instance is up (Emscripten "loaded") or it ran
          var n0 = workers, seen = false;
          w.addEventListener("message", function (e) {
            if (seen) return; seen = true;
            var c = e && e.data && (e.data.cmd || e.data.type); if (n0 <= 40) ev("workerUp", {n: n0, cmd: String(c || "?").slice(0, 20), heap: heapMiB()}, true);
          });
          w.addEventListener("error", function (e) { ev("workerError", {n: n0, msg: cut(e && e.message, 160)}, true); });
        } catch (e) {}
        return w;
      };
      G.Worker.prototype = WK.prototype;
    }
    var WM = G.WebAssembly && G.WebAssembly.Memory;
    if (WM) {
      G.WebAssembly.Memory = function (d) {
        var m = new WM(d); mems.push(m);
        ev("wasmMemory", {initialPages: d && d.initial, maximumPages: d && d.maximum, shared: !!(d && d.shared)}, true);
        return m;
      };
      G.WebAssembly.Memory.prototype = WM.prototype;
    }
    var gp = G.navigator && navigator.gpu, ra = gp && gp.requestAdapter;
    if (ra) gp.requestAdapter = function () {
      ev("gpuAdapterRequest", null, true);
      return ra.apply(this, arguments).then(function (a) { ev("gpuAdapter", a ? (a.info ? {vendor: a.info.vendor, arch: a.info.architecture} : 1) : 0, true); return a; },
        function (err) { ev("gpuAdapterFail", cut(err && err.message || err), true); throw err; });
    };
    var Q = G.GPUQueue && G.GPUQueue.prototype, qs = Q && Q.submit, firstSubmit = true;
    if (qs) Q.submit = function () { if (firstSubmit) { firstSubmit = false; ev("firstSubmit", {heap: heapMiB()}, true); } return qs.apply(this, arguments); };
    var C = G.GPUCanvasContext && G.GPUCanvasContext.prototype, gct = C && C.getCurrentTexture, firstFrame = true;
    if (gct) C.getCurrentTexture = function () { if (firstFrame) { firstFrame = false; ev("firstFrame", {heap: heapMiB()}, true); } return gct.apply(this, arguments); };
  } catch (e) {}
  var beat = 0, beatTimer = setInterval(function () {
    if (++beat > (G.CP_ENGINE_W32 || q.get("mem") === "ios" ? 90 : 15)) { clearInterval(beatTimer); return; }   // iOS: 1 s beats for 90 s
    R.heapMiB = heapMiB(); R.workers = workers;
    var gg = R.gpu || {}; ev("tick", {s: beat, heap: R.heapMiB, workers: workers, gb: gg.bufMiB, gt: gg.texMiB}, true);
  }, 1000);
  var firstEngine = true, shaderLines = 0;
  // [w32-diag] Rotating iOS diagnostic (one question per reload; localStorage counter, ?diag=1|2|3 forces one):
  //   1 = instance cost only: compile the engine module, instantiate it on the page with stub imports, then in 5
  //       workers; run no engine code; hold 25 s.
  //   2 = execution, single instance: as 1 on the page only, then run the engine's static constructors.
  //   3 = full boot (normal).
  var diag = 3;
  try {
    var dq = q.get("diag");
    if (/^[1234]$/.test(dq || "")) diag = +dq;
    else if (G.CP_ENGINE_W32 && q.get("diag") === "rotate") {   // rotation only on request now (default: full boot)
      var n = 0; try { n = +(localStorage.getItem("cp2077.iosdiag") || 0); } catch (e) {}
      diag = (n % 4) + 1;
      try { localStorage.setItem("cp2077.iosdiag", String(n + 1)); } catch (e) {}
    }
  } catch (e) {}
  R.diag = diag;
  if (diag === 4 && G.CP_ENGINE_W32) {   // 4 = full boot of the cold-stubbed module (never-profiled functions -> unreachable)
    G.CP_ENGINE = "0d94cea2065f3063948fc9a92f78d820ced871ce8110f84a1b1480f26ade4541"; R.engine = G.CP_ENGINE;
  }
  if (diag !== 3) {
    G.CP_ENGINE_BLOCK = true;   // the page's engine script tag is not written
    var note = function (t) {
      var d = document.getElementById("cp-diag-note");
      if (!d) { d = document.createElement("div"); d.id = "cp-diag-note";
        d.style.cssText = "position:fixed;left:12px;right:12px;bottom:16px;z-index:99999;background:#111;color:#f3e600;" +
          "font:14px/1.4 system-ui,sans-serif;padding:12px 14px;border:1px solid #f3e600;border-radius:8px";
        (document.body || document.documentElement).appendChild(d); }
      d.textContent = t;
    };
    var stubImports = function (mod, mem) {
      var env = {}, imp = {};
      WebAssembly.Module.imports(mod).forEach(function (i) {
        imp[i.module] = imp[i.module] || {};
        imp[i.module][i.name] = i.kind === "memory" ? mem : i.kind === "function" ? function () { return 0; } : undefined;
      });
      return imp;
    };
    var runDiag = async function () {
      note("Diagnostic run " + diag + "/4 (no game this time). Keep this tab open about 30 s, then reload the page.");
      for (var wt = 0; !G.CPB && wt < 120; wt++) await new Promise(function (res) { setTimeout(res, 500); });
      var base = G.CPB, url = base + "/runtime/engine-versions/" + G.CP_ENGINE + "/cp2077.wasm";
      ev("diag:start", {variant: diag}, true);
      var mod;
      try {
        if (WebAssembly.compileStreaming) mod = await WebAssembly.compileStreaming(fetch(url, {mode: "cors", credentials: "omit"}));
        else mod = await WebAssembly.compile(await (await fetch(url, {mode: "cors", credentials: "omit"})).arrayBuffer());
      } catch (e) { ev("diag:compileFail", cut(e && e.message || e), true); note("Diagnostic " + diag + ": download/compile failed. Please reload."); return; }
      ev("diag:compiled", {heap: heapMiB()}, true);
      var mem = new WebAssembly.Memory({initial: 3072, maximum: 65536, shared: true}); mems.push(mem);
      var inst;
      try { inst = (await WebAssembly.instantiate(mod, stubImports(mod, mem))); }
      catch (e) { ev("diag:instFail", cut(e && e.message || e), true); return; }
      ev("diag:instMain", {heap: heapMiB()}, true);
      if (diag === 1) {
        var src = "onmessage=function(e){try{var imp={};WebAssembly.Module.imports(e.data.mod).forEach(function(i){imp[i.module]=imp[i.module]||{};" +
          "imp[i.module][i.name]=i.kind==='memory'?e.data.mem:function(){return 0}});self.keep=new WebAssembly.Instance(e.data.mod,imp);postMessage('up')}catch(x){postMessage('fail '+x)}}";
        var wurl = URL.createObjectURL(new Blob([src], {type: "text/javascript"}));
        for (var k = 1; k <= 5; k++) {
          var r = await new Promise(function (res) { var w = new Worker(wurl); w.onmessage = function (m) { res(String(m.data)); };
            w.onerror = function (m) { res("error " + (m && m.message)); }; w.postMessage({mod: mod, mem: mem}); (G.__diagWorkers = G.__diagWorkers || []).push(w); });
          ev("diag:workerInst", {n: k, r: r.slice(0, 80), heap: heapMiB()}, true);
          await new Promise(function (res) { setTimeout(res, 1500); });
        }
      } else {
        // run the engine's static constructors in ONE worker instance (the page stays responsive; a spin is cut at 25 s)
        var src2 = "onmessage=function(e){var imp={};WebAssembly.Module.imports(e.data.mod).forEach(function(i){imp[i.module]=imp[i.module]||{};" +
          "imp[i.module][i.name]=i.kind==='memory'?e.data.mem:function(){return 0}});var ins;try{ins=new WebAssembly.Instance(e.data.mod,imp)}catch(x){postMessage('instfail '+x);return}" +
          "postMessage('inst');['_emscripten_tls_init','__wasm_call_ctors'].forEach(function(n){var f=ins.exports[n];if(typeof f!=='function'){postMessage('noexport '+n);return}" +
          "var t=Date.now();postMessage('start '+n);try{f();postMessage('ran '+n+' '+(Date.now()-t)+'ms')}catch(x){postMessage('trap '+n+' '+(Date.now()-t)+'ms '+String(x).slice(0,120))}});postMessage('end')}";
        var w2 = new Worker(URL.createObjectURL(new Blob([src2], {type: "text/javascript"})));
        w2.onmessage = function (m) { ev("diag:exec", {m: String(m.data).slice(0, 160), heap: heapMiB()}, true); };
        w2.onerror = function (m) { ev("diag:execError", cut(m && m.message, 160), true); };
        w2.postMessage({mod: mod, mem: mem});
        setTimeout(function () { try { w2.terminate(); ev("diag:execStopped", null, true); } catch (e) {} }, 25000);
      }
      for (var s = 0; s < 12; s++) { await new Promise(function (res) { setTimeout(res, 2000); }); ev("diag:alive", {s: (s + 1) * 2, heap: heapMiB()}, true); }
      ev("diag:done", null, true);
      note("Diagnostic run " + diag + "/4 finished. Please reload the page.");
      G.__diagKeep = [mod, inst];
    };
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", function () { runDiag().catch(function (e) { ev("diag:error", cut(e && e.message || e), true); }); });
    else runDiag().catch(function (e) { ev("diag:error", cut(e && e.message || e), true); });
  }
  var sizeTries = 0; setTimeout(function wasmSize() {   // engine module size as delivered (code-size context for each attempt)
    try {
      var L = G.cp2077Loader && G.cp2077Loader.state;   // cross-origin resource timing hides sizes: use the loader's byte count
      if (L && L.wasmDone && L.wasmBytes) ev("wasmSize", {MiB: Math.round(L.wasmBytes / 104857.6) / 10}, true);
      else if (++sizeTries < 240) setTimeout(wasmSize, 500);
    } catch (e) {}
  }, 500);
  // Boot marks from loader.js: wasmRequest/wasmResponse/wasmDownloaded/compileStart/compiled/mounted/state:*/menuUi/
  // sessionActive/ready.
  var seenMarks = {};
  function pollMarks() {
    var m = G.cp2077Loader && G.cp2077Loader.state && G.cp2077Loader.state.marks; if (!m) return;
    for (var n in m) if (!seenMarks[n]) { seenMarks[n] = 1; R.events.push({k: "mark:" + n, ms: m[n]}); R.stage = n; post(); }
  }
  // wasm compile / instantiate of the main-thread engine module (the loader's wrappers chain onto these).
  var W = G.WebAssembly;
  if (W) {
    var wrap = function (name, label) {
      var f = W[name]; if (typeof f !== "function") return;
      W[name] = function (src) {
        var big = false;
        try {
          big = !!src && ((typeof Response === "function" && src instanceof Response) || typeof src.then === "function" ||
            (src.byteLength || 0) > 1048576 || src instanceof W.Module);
        } catch (e) {}
        var r = f.apply(this, arguments);
        if (big) {
          ev(label + ":start", null);
          r.then(function () { ev(label + ":done", null, true); }, function (err) { ev(label + ":fail", cut(err && err.message || err), true); });
        }
        return r;
      };
    };
    wrap("instantiateStreaming", "wasmInstantiate"); wrap("compileStreaming", "wasmCompile");
    wrap("instantiate", "wasmInstantiate"); wrap("compile", "wasmCompile");
  }
  // Console: engine memory probe numbers, the first 20 real errors (engine stderr info lines start with "CP2077 " or
  // "[" and only go to the tail ring), and a ring of the last 25 non-periodic lines.
  var noisy = /phases probe|citycol probe|memory site|malloc site|outer frame probe|audio ring probe|coreplay\] move|cmdlock|cbOrder|allocation wait probe|allocator memory probe/;
  function onLine(level, a) {
    try {
      var s = cut(Array.prototype.map.call(a, function (x) { return x && x.stack ? x.stack : x; }).join(" "), 400);
      if (/^CP2077 w32 memory probe:/.test(s)) {
        var o = {};
        s.replace(/(\w+)=(\d+)/g, function (_, k, v) { o[k.replace(/Bytes$/, "MiB")] = Math.round(Number(v) / 104857.6) / 10; return _; });
        o.estResidentMiB = Math.round(((o.mallocInUseMiB || 0) + (o.staticImageMiB || 0)) * 10) / 10;
        R.last = o; if (!R.memPeak || o.estResidentMiB > R.memPeak.estResidentMiB) R.memPeak = o;
        return;
      }
      if (noisy.test(s)) return;
      R.tail.push(level.charAt(0) + now() + " " + s); if (R.tail.length > 25) R.tail.shift();
      if (firstEngine && /^CP2077 /.test(s)) { firstEngine = false; ev("engineFirstLine", cut(s, 160), true); }
      if (/shader (package|pack)|inflate/i.test(s) && shaderLines < 6) { shaderLines++; ev("shaderPkg", cut(s, 200), true); }
      if (/WebGPU(: device lost| not available)/.test(s)) ev("gpuLost", s, true);
      else if (/^CP2077 game state probe: phase=active/.test(s) || /^CP2077 w32 menu (scenario|puppets: loading)/.test(s) || /^CP2077 (native initializer begin|static shader cache|\[lazycs\]|async compute pipelines|nobc decode probe: regions=(64|256|1024))/.test(s) || /^CP2077 w32 memory profile/.test(s) ||
        /^CP2077 (native init job: phase=enter name=(BaseGameEngine\/Initialization\/LoadTweakDB|CGameEngine\/Initialization\/InkSystem|CGameFramework\/(GameInstance|RuntimeScene|StateMachine)|Engine\/InitializationFinished)|loading stage probe: phase=enter)/.test(s)) ev("engine", cut(s, 120), true); // [w32-telemetry v4] init/loading stages
      else if (level === "error" && R.errors.length < 20 && !/^(CP2077 |\[)/.test(s)) { R.errors.push({ms: now(), s: s}); post(); }
    } catch (e) {}
  }
  ["error", "warn", "log", "debug"].forEach(function (level) {
    var f = console[level]; if (typeof f !== "function") return;
    console[level] = function () { onLine(level, arguments); return f.apply(this, arguments); };
  });
  G.addEventListener("error", function (e) {
    ev("onerror", cut((e.message || "") + " @" + String(e.filename || "").replace(/^.*\//, "") + ":" + (e.lineno || 0)), true);
  });
  G.addEventListener("unhandledrejection", function (e) { var r = e.reason; ev("rejection", cut(r && ((r.name ? r.name + ": " : "") + (r.message || "") + " | " + (r.stack || "")) || r), true); });
  G.addEventListener("pagehide", function () { ev("pagehide", null, true); });
  document.addEventListener("visibilitychange", function () { ev("visibility", document.visibilityState, true); });
  // [w32-wgsl] Safari rejects function-level @diagnostic(...) attributes ("invalid attribute for function
  // declaration"): move them to a module-level diagnostic directive (same severity) on the wasm32/iOS path.
  try {
    var SD = G.GPUDevice && G.GPUDevice.prototype, csm = SD && SD.createShaderModule, fixed = 0;
    if (csm && G.CP_ENGINE_W32) SD.createShaderModule = function (d) {
      try {
        var code = d && typeof d.code === "string" ? d.code : null;
        if (code && code.indexOf("@diagnostic(") >= 0) {
          var dirs = {}, out = code.replace(/@diagnostic\(\s*(\w+)\s*,\s*([\w.]+)\s*\)\s*(?=(?:@[\w]+(?:\([^)]*\))?\s*)*fn\b)/g,
            function (_, sev, rule) { dirs[rule] = sev; return ""; });
          var head = ""; for (var r in dirs) head += "diagnostic(" + dirs[r] + ", " + r + ");\n";
          if (head) { d = Object.assign({}, d, {code: head + out}); if (fixed++ < 3) ev("wgslDiagFix", Object.keys(dirs).join(","), false); }
        }
      } catch (e) {}
      return csm.call(this, d);
    };
  } catch (e) {}
  // WebGPU: device creation, lost reason, first uncaptured errors.
  try {
    var A = G.GPUAdapter && G.GPUAdapter.prototype, rd = A && A.requestDevice;
    if (rd) A.requestDevice = function (desc) {
      try { var rl = desc && desc.requiredLimits || {}; ev("gpuDeviceRequest", {maxBuf: rl.maxBufferSize, maxStorage: rl.maxStorageBufferBindingSize, heap: heapMiB()}, true); } catch (e) {}
      return rd.apply(this, arguments).then(function (dev) {
        try {
          ev("gpuDevice", {maxBuf: dev.limits.maxBufferSize, maxStorage: dev.limits.maxStorageBufferBindingSize, maxTex2D: dev.limits.maxTextureDimension2D, maxLayers: dev.limits.maxTextureArrayLayers, bc: dev.features.has("texture-compression-bc") ? 1 : 0, etc2: dev.features.has("texture-compression-etc2") ? 1 : 0, astc: dev.features.has("texture-compression-astc") ? 1 : 0, req: (desc && desc.requiredFeatures ? Array.from(desc.requiredFeatures).join(",") : "").slice(0, 120)});
          dev.lost.then(function (i) { ev("gpuLost", {reason: i.reason, message: cut(i.message)}, true); });
          var n = 0;
          dev.addEventListener("uncapturederror", function (e) { if (n++ < 5) ev("gpuError", cut(e.error && e.error.message), true); });
        } catch (e) {}
        return dev;
      }, function (err) { ev("gpuDeviceFail", cut(err && err.message || err), true); throw err; });
    };
  } catch (e) {}
  // WebGPU live bytes (buffers exact, textures estimated from format/size/mips), so the phone's GPU share is visible.
  var gpu = R.gpu = {bufMiB: 0, texMiB: 0, peakMiB: 0}, gpuB = 0, gpuT = 0, sizes = new WeakMap();
  try {
    var D = G.GPUDevice && G.GPUDevice.prototype;
    var texBytes = function (d) {
      var f = String(d.format || ""), b = 4, blk = 1;
      if (/^r8/.test(f)) b = 1; else if (/^(rg8|r16|depth16)/.test(f)) b = 2; else if (/^(rgba16|rg32)/.test(f)) b = 8;
      else if (/^rgba32/.test(f)) b = 16; else if (/^(bc|etc2|eac|astc)/.test(f)) { blk = 4; b = /^(bc[14]-|etc2-rgb8|eac-r11)/.test(f) ? 8 : 16; }
      var z0 = d.size, w = +(z0.width != null ? z0.width : z0[0]), h = +((z0.height != null ? z0.height : z0[1]) || 1);
      var z = +((z0.depthOrArrayLayers != null ? z0.depthOrArrayLayers : z0[2]) || 1), t = 0;
      for (var m = 0; m < (d.mipLevelCount || 1); m++)
        t += Math.ceil(Math.max(1, w >> m) / blk) * Math.ceil(Math.max(1, h >> m) / blk) * b * (d.dimension === "3d" ? Math.max(1, z >> m) : z);
      return t * (d.sampleCount || 1);
    };
    var upd = function () {
      gpu.bufMiB = Math.round(gpuB / 104857.6) / 10; gpu.texMiB = Math.round(gpuT / 104857.6) / 10;
      var tot = gpu.bufMiB + gpu.texMiB; if (tot > gpu.peakMiB) gpu.peakMiB = Math.round(tot * 10) / 10;
    };
    if (D && D.createBuffer && D.createTexture && G.GPUBuffer && G.GPUTexture) {
      var texErrN = 0, cb = D.createBuffer, ct = D.createTexture, db = G.GPUBuffer.prototype.destroy, dt = G.GPUTexture.prototype.destroy;
      D.createBuffer = function (d) { var o = cb.apply(this, arguments); try { var n = +d.size || 0; sizes.set(o, n); gpuB += n; upd(); } catch (e) {} return o; };
      D.createTexture = function (d) { var o; try { o = ct.apply(this, arguments); } catch (err) { /* [texerr] which texture iPhones reject */ try { var z0 = d && d.size || {}; if ((texErrN = (texErrN || 0) + 1) <= 3) ev("texErr", {m: cut(err && err.message || err, 160), f: d && d.format, w: z0.width != null ? z0.width : z0[0], h: z0.height != null ? z0.height : z0[1], z: z0.depthOrArrayLayers != null ? z0.depthOrArrayLayers : z0[2], mips: d && d.mipLevelCount, usage: d && d.usage, dim: d && d.dimension, vf: d && d.viewFormats && d.viewFormats.join(",")}, true); } catch (e2) {} throw err; } try { var n = texBytes(d); sizes.set(o, n); gpuT += n; upd(); } catch (e) {} return o; };
      G.GPUBuffer.prototype.destroy = function () { var n = sizes.get(this); if (n) { sizes.delete(this); gpuB -= n; upd(); } return db.apply(this, arguments); };
      G.GPUTexture.prototype.destroy = function () { var n = sizes.get(this); if (n) { sizes.delete(this); gpuT -= n; upd(); } return dt.apply(this, arguments); };
    }
  } catch (e) {}
  // Every 10 s: engine memory probe (parsed above from its console line) + JS heap + presented frames.
  setInterval(function () {
    pollMarks();
    var M = G.cp2077EngineModule, s = {};
    try { if (M && M._CpWebMemoryProbe) M._CpWebMemoryProbe(); } catch (e) { s.probeErr = cut(e && e.message || e, 120); }
    var pm = performance.memory;
    if (pm) { s.jsUsedMiB = Math.round(pm.usedJSHeapSize / 1048576); s.jsLimitMiB = Math.round(pm.jsHeapSizeLimit / 1048576); }
    if (R.last) { s.res = R.last.estResidentMiB; s.malloc = R.last.mallocInUseMiB; s.heap = R.last.heapCapacityMiB; s.nz = R.last.nonZeroPageMiB; }
    s.gpuBuf = gpu.bufMiB; s.gpuTex = gpu.texMiB;
    s.frames = (M && M.cp2077PresentationStats && M.cp2077PresentationStats.validatedFrames) || 0;
    ev("mem", s);
  }, 10000);
  setInterval(pollMarks, 1000);
} catch (e) {} })();
// [notier1] Browsers without the WebGPU feature texture-formats-tier1 (Firefox, older Chrome) have no storage access
// for 8/16-bit and packed formats. The engine (gpu-texture-web.cpp Tier1CoreFormat) stores those textures in the
// nearest core format; here every WGSL storage-texture declaration and bind-group-layout entry is rewritten to the
// same format, so shaders, layouts and textures agree. textureStore/textureLoad take vec4 for every format, so only
// the format name changes. Devices that have the feature are untouched.
(function () { try {
  var G = globalThis, D = G.GPUDevice && G.GPUDevice.prototype;
  if (!D || !D.createShaderModule || !D.createBindGroupLayout) return;
  // Channel counts are kept (gpu-texture-web.cpp Tier1CoreFormat): 1/2-channel 8/16-bit formats become r32*/rg32*.
  var MAP = {r8unorm: "r32float", rg8unorm: "rg32float", r8snorm: "r32float", rg8snorm: "rg32float", r8uint: "r32uint",
    rg8uint: "rg32uint", r8sint: "r32sint", rg8sint: "rg32sint", r16uint: "r32uint", r16sint: "r32sint", rg16uint: "rg32uint",
    rg16sint: "rg32sint", r16float: "r32float", rg16float: "rg32float", rgb10a2unorm: "rgba16float", rgb10a2uint: "rgba16uint",
    rg11b10ufloat: "rgba16float", r16unorm: "r32float", r16snorm: "r32float", rg16unorm: "rgba16float", rg16snorm: "rgba16float",
    rgba16unorm: "rgba16float", rgba16snorm: "rgba16float"};
  var lacks = function (dev) { try { return !!(dev && dev.features && !dev.features.has("texture-formats-tier1")); } catch (e) { return false; } };
  var RE = /texture_storage_(1d|2d|2d_array|3d)\s*<\s*([a-z0-9]+)\s*,/g, csm = D.createShaderModule, cbgl = D.createBindGroupLayout, n = 0;
  D.createShaderModule = function (d) {
    try {
      if (d && typeof d.code === "string" && d.code.indexOf("texture_storage_") >= 0 && lacks(this)) {
        var code = d.code.replace(RE, function (m, dim, f) { return MAP[f] ? "texture_storage_" + dim + "<" + MAP[f] + "," : m; });
        if (code !== d.code) { d = Object.assign({}, d, {code: code}); if (n++ < 3) console.log("CP2077 notier1: WGSL storage formats rewritten to core formats"); }
      }
    } catch (e) {}
    return csm.call(this, d);
  };
  D.createBindGroupLayout = function (d) {
    try {
      if (d && d.entries && lacks(this)) {
        var changed = false, entries = Array.prototype.map.call(d.entries, function (e) {
          if (e && e.storageTexture && MAP[e.storageTexture.format]) { changed = true;
            return Object.assign({}, e, {storageTexture: Object.assign({}, e.storageTexture, {format: MAP[e.storageTexture.format]})}); }
          return e;
        });
        if (changed) d = Object.assign({}, d, {entries: entries});
      }
    } catch (e) {}
    return cbgl.call(this, d);
  };
  // Render targets: same core formats the engine gives those textures (gpu-api-texture-web Tier1Fallback,
  // gpu-texture-web Tier1CoreFormat), so attachment and pipeline formats agree.
  // Channel counts are kept (a vec2 output can't target rgba16float).
  var RT = {r16unorm: "r32float", r16snorm: "r32float", rg16unorm: "rg16float", rg16snorm: "rg16float",
    rgba16unorm: "rgba16float", rgba16snorm: "rgba16float", rg11b10ufloat: "rgba16float",
    r8snorm: "r16float", rg8snorm: "rg16float", rgba8snorm: "rgba16float"}, m = 0;
  var fixTargets = function (dev, d) {
    try {
      var f = d && d.fragment; if (!f || !f.targets || !lacks(dev)) return d;
      var changed = false, targets = Array.prototype.map.call(f.targets, function (t) {
        if (t && RT[t.format] && !(t.format === "rg11b10ufloat" && dev.features.has("rg11b10ufloat-renderable"))) {
          changed = true; return Object.assign({}, t, {format: RT[t.format]}); }
        return t;
      });
      if (!changed) return d;
      if (m++ < 3) console.log("CP2077 notier1: render target formats mapped to core formats");
      return Object.assign({}, d, {fragment: Object.assign({}, f, {targets: targets})});
    } catch (e) { return d; }
  };
  ["createRenderPipeline", "createRenderPipelineAsync"].forEach(function (name) {
    var orig = D[name]; if (!orig) return;
    D[name] = function (d) { return orig.call(this, fixTargets(this, d)); };
  });
} catch (e) {} })();
// [pipeerr] emdawnwebgpu asserts (abort) when create*PipelineAsync rejects with anything but a GPUPipelineError
// (e.g. a TypeError for a format the device lacks). Turn those into validation failures so the engine skips the pipeline.
(function () { try {
  var D = typeof GPUDevice !== "undefined" && GPUDevice.prototype; if (!D) return; var n = 0;
  ["createRenderPipelineAsync", "createComputePipelineAsync"].forEach(function (name) {
    var orig = D[name]; if (!orig) return;
    D[name] = function () {
      var p; try { p = orig.apply(this, arguments); } catch (e) { p = Promise.reject(e); }
      return p.then(null, function (e) {
        if (e && (e.reason === "validation" || e.reason === "internal")) throw e;
        if (n++ < 8) console.error("CP2077 pipeerr: " + name + " rejected without reason: " + (e && (e.name + ": " + e.message)));
        throw {reason: "validation", message: String(e && e.message || e)};
      });
    };
  });
} catch (e) {} })();
// [devretry] 2026-10-27 probes: ~20 Chrome sessions/day never got an answer to requestDevice (boot sat at 57 % until the
// stall reload). After 15 s (?devRetryMs=) ask once more on a fresh adapter; the first device wins, a late one is destroyed.
(function () { try {
  var G = globalThis, A = G.GPUAdapter && G.GPUAdapter.prototype, rd = A && A.requestDevice;
  if (!rd || !G.navigator || !navigator.gpu) return;
  var ms = +new URLSearchParams(location.search).get("devRetryMs") || 15000;
  A.requestDevice = function () {
    var args = arguments, first = rd.apply(this, args);
    return new Promise(function (res, rej) {
      var done = false, fails = 0, tries = 1;
      var win = function (dev) { if (done) { try { dev && dev.destroy(); } catch (e) {} return; } done = true; res(dev); };
      var lose = function (err) { if (done) return; if (++fails >= tries) { done = true; rej(err); } };
      first.then(win, lose);
      setTimeout(function () {
        if (done) return;
        tries++; console.log("CP2077 devretry: requestDevice unanswered after " + ms + " ms, retrying on a new adapter");
        navigator.gpu.requestAdapter().then(function (a) { if (!a) throw new Error("no adapter on retry"); return rd.apply(a, args); }).then(win, lose);
      }, ms);
    });
  };
} catch (e) {} })();
