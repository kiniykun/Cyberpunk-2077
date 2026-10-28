// Browser capability check (inlined into <head> of the R2 page and the Vercel /game page by make-r2-page.mjs,
// right after engine-select.js). Runs before the access-code panel and the engine boot.
// Needs: 64-bit WebAssembly memory (memory64), cross-origin isolation + SharedArrayBuffer, WebGPU with an adapter.
// If anything is missing: CP_UNSUPPORTED = reason, the engine is not booted (CP_ENGINE_BLOCK), and a
// "can't run" panel replaces the loader UI. ?force=1 skips the check (testing).
(function () {
  var G = globalThis;
  G.CP_UNSUPPORTED = null;
  if (/[?&]force=1(&|$)/.test(location.search)) return;
  var ua = navigator.userAgent || "";
  var iOS = /iPhone|iPad|iPod/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1);
  var safari = /Safari\//.test(ua) && !/Chrome\/|Chromium|CriOS|FxiOS|EdgiOS|Edg\/|OPR\/|Android/.test(ua);
  function memory64() {
    try {   // (module (memory i64 0)): memory section, 1 memory, limits flag 0x04 (64-bit index), min 0
      return WebAssembly.validate(new Uint8Array([0, 0x61, 0x73, 0x6d, 1, 0, 0, 0, 5, 3, 1, 4, 0]));
    } catch (e) { return false; }
  }
  var missing = null;
  if (typeof WebAssembly !== "object" || (!memory64() && !G.CP_ENGINE_W32))   // [w32] engine-select picked the wasm32 bundle
    missing = safari || iOS ? (iOS && !safari ? "iPhone and iPad browsers use Safari's engine, which doesn't support 64-bit WebAssembly yet."
                                              : "Safari doesn't support 64-bit WebAssembly yet.")
                            : "This browser doesn't support 64-bit WebAssembly (memory64).";
  else if (!G.crossOriginIsolated || typeof SharedArrayBuffer !== "function")
    missing = "This browser or tab can't use shared memory (SharedArrayBuffer / cross-origin isolation).";
  else if (!navigator.gpu)
    missing = "WebGPU isn't available in this browser.";
  function show(reason) {
    if (G.CP_UNSUPPORTED) return;
    G.CP_UNSUPPORTED = reason;
    G.CP_ENGINE_BLOCK = true;
    var render = function () {
      var l = document.getElementById("cp-loader");
      if (!l) return;
      l.classList.add("unsupported");
      var wrap = l.querySelector(".cp-wrap") || l;
      var box = document.getElementById("cp-unsup") || wrap.appendChild(document.createElement("div"));
      box.id = "cp-unsup"; box.className = "cp-unsup"; box.setAttribute("role", "alert");
      box.innerHTML = "<h2>This browser can't run Cyberpunk 2077 Browser yet</h2><p class=\"cp-unsup-why\"></p>" +
        "<p class=\"cp-unsup-fix\">Use Chrome, Edge or Brave on a desktop PC, or Chrome on Android.</p>";
      box.querySelector(".cp-unsup-why").textContent = reason;
      if (iOS || safari || /[?&]probe=1(&|$)/.test(location.search)) probe(box);
    };
    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", render); else render();
  }
  // [probe] Device capability report for the wasm32/iPhone port: WebGPU features + limits, the largest shared
  // wasm memory the device grants, wasm feature support and a large-module compile test. Shown on the panel and
  // posted (no personal data) to the Worker's /__probe; each stage is posted as it finishes, so a tab killed by a
  // memory test still leaves the stages before it.
  function leb(x) { var o = []; do { var b = x & 127; x = Math.floor(x / 128); o.push(x ? b | 128 : b); } while (x); return o; }
  function probe(box) {
    var R = {v: 1, ua: ua, t: Date.now(), screen: [screen.width, screen.height, devicePixelRatio], coi: !!G.crossOriginIsolated,
      sab: typeof SharedArrayBuffer === "function", cores: navigator.hardwareConcurrency || 0, mem: navigator.deviceMemory || 0, stages: []};
    var pre = document.createElement("pre"); pre.className = "cp-probe";
    pre.style.cssText = "text-align:left;font-size:11px;white-space:pre-wrap;max-height:40vh;overflow:auto;opacity:.8";
    box.appendChild(pre);
    var id = (Math.random() * 1e9 | 0).toString(36);
    function post(stage) {
      R.stages.push(stage); pre.textContent = JSON.stringify(R, null, 1);
      try { fetch("https://cp2077-port.fn-web-worker.workers.dev/__probe?id=" + id, {method: "POST", mode: "no-cors",
        headers: {"Content-Type": "text/plain"}, body: JSON.stringify(R), keepalive: true}); } catch (e) {}
    }
    var H = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
    function sec(id, bytes) { return [id].concat(leb(bytes.length), bytes); }
    function v(bytes) { try { return WebAssembly.validate(new Uint8Array(bytes)); } catch (e) { return false; } }
    var T = sec(1, [1, 0x60, 0, 0]), F = sec(3, [1, 0]);
    function fn(body) { var b = [0].concat(body, [0x0b]); return sec(10, [1].concat(leb(b.length), b)); }
    R.wasm = {
      simd: v(H.concat(T, F, fn([0xfd, 12].concat(new Array(16).fill(0), [0x1a])))),   // v128.const 0; drop
      threads: v(H.concat(sec(5, [1, 3, 1, 1]))),                                        // shared memory 1..1
      memory64: memory64(),
      exceptions: v(H.concat(T, F, fn([6, 0x40, 0x0b])))                                 // try (legacy EH) end
    };
    post("basic");
    var steps = [];
    // growable shared memory, touched page by page (real commit, like the engine heap). Runs LAST, posts every
    // 256 MiB, and remembers progress in localStorage: if iOS kills the tab, the last posted step is the ceiling.
    function growStep(m, mib) {
      return function () {
        try { var before = m.buffer.byteLength; m.grow(4096); var u = new Uint8Array(m.buffer);
          for (var p = before; p < u.length; p += 4096) u[p] = 1; R.growMiB = mib; }
        catch (e) { R.growErr = String(e && e.message || e).slice(0, 120); R.growStop = true; }
        try { localStorage.setItem("cpProbeGrow", String(R.growMiB || 0)); } catch (e) {}
        post("grow" + mib); return Promise.resolve();
      };
    }
    // big module compile: mb functions of ~1 MB each (i32.const 0; drop, repeated)
    function bigModule(mb) {
      var reps = 349525, inner = 1 + 3 * reps + 1, size = leb(inner), per = size.length + inner;
      var fsec = sec(3, leb(mb).concat(new Array(mb).fill(0))), cnt = leb(mb), codeLen = cnt.length + mb * per;
      var head = H.concat(T, fsec, [10], leb(codeLen), cnt);
      var out = new Uint8Array(head.length + mb * per), o = head.length; out.set(head);
      for (var f = 0; f < mb; f++) { out.set(size, o); o += size.length; out[o++] = 0;
        for (var r = 0; r < reps; r++) { out[o++] = 0x41; out[o++] = 0; out[o++] = 0x1a; } out[o++] = 0x0b; }
      return out;
    }
    [32, 128, 256].forEach(function (mb) {
      steps.push(function () {
        var t0 = performance.now(), bytes;
        try { bytes = bigModule(mb); } catch (e) { (R.compile = R.compile || []).push({mb: mb, err: "build " + e}); post("compile" + mb); return Promise.resolve(); }
        return WebAssembly.compile(bytes).then(function () { (R.compile = R.compile || []).push({mb: mb, ok: true, ms: Math.round(performance.now() - t0)}); },
          function (e) { (R.compile = R.compile || []).push({mb: mb, ok: false, err: String(e && e.message || e).slice(0, 120)}); })
          .then(function () { bytes = null; post("compile" + mb); });
      });
    });
    var gm = null; try { gm = new WebAssembly.Memory({initial: 0, maximum: 65536, shared: true}); } catch (e) { R.growErr = "create " + e; }
    try { R.lastGrowCrash = Number(localStorage.getItem("cpProbeGrow") || 0) || null; } catch (e) {}
    if (gm && /[?&]probe=grow(&|$)/.test(location.search)) for (var g = 256; g <= 4096 - 256; g += 256) steps.push(growStep(gm, g));
    function run(i) { if (i < steps.length) setTimeout(function () { if (R.growStop) return post("done"); steps[i]().then(function () { run(i + 1); }); }, 80); else post("done"); }
    // WebGPU adapter first (cheap), then the heavy steps
    var gpu = navigator.gpu ? navigator.gpu.requestAdapter().then(function (a) {
      if (!a) { R.gpu = {adapter: false}; return; }
      var lim = {}; for (var k in a.limits) lim[k] = a.limits[k];
      R.gpu = {adapter: true, features: Array.from(a.features || []).sort(), limits: lim,
        info: a.info ? {vendor: a.info.vendor, arch: a.info.architecture, device: a.info.device, desc: a.info.description} : null};
    }, function (e) { R.gpu = {adapter: false, err: String(e)}; }) : Promise.resolve(R.gpu = {api: false});
    gpu.then(function () { post("gpu"); run(0); });
  }
  if (missing) return show(missing);
  // WebGPU adapter (async): the engine may already be starting; the panel then replaces the loader.
  try {
    navigator.gpu.requestAdapter().then(function (a) {
      if (!a) show("No compatible GPU was found. Hardware acceleration is probably off: in Chrome, Edge or Brave open Settings > System, turn on “Use graphics acceleration when available”, then relaunch the browser.");
      // [tier1] 2026-10-23 probes: 112 desktop sessions (mostly Chrome 114-134 on the wasm32 bundle, also Android) failed
      // CreateTexture (RG11B10Ufloat + StorageBinding needs texture-formats-tier1), never drew a frame and sat on the
      // loader for minutes. Say so up front. iPhone/iPad are exempt (no such failures in the probes).
      // [notier1] 2026-10-26: the 64-bit engine stores those formats as core ones (r32float etc., which need float32-filterable)
      // and runs without the feature (Firefox, older Chrome). The wasm32 engine has no such fallback yet.
      else if (!iOS && a.features && !a.features.has("texture-formats-tier1") && (G.CP_ENGINE_W32 || !a.features.has("float32-filterable")) &&
               !/[?&]tier1Check=0(&|$)/.test(location.search))   // [tier1] ?tier1Check=0: test the fallback path
        show(/Android/.test(ua) ? "This phone's browser or GPU is missing a WebGPU feature the game needs (texture-formats-tier1). Update Chrome from the Play Store; if it's already up to date, this phone's GPU isn't supported yet."
                                : "This browser version is too old for the game (it's missing the WebGPU feature texture-formats-tier1). Update Chrome, Edge or Brave to the latest version (Settings > About), relaunch it, then reload this page.");
    }, function () { show("WebGPU couldn't start in this browser."); });
  } catch (e) { show("WebGPU couldn't start in this browser."); }
})();
