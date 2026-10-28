// Plan B cross-origin prelude (inlined first in the Vercel /game page by scripts/make-vercel-page.mjs).
// The page lives on another site (cyberpunk2077.lol); every engine/game URL the runtime builds as a
// same-origin absolute path (/runtime/..., /game/archive|engine|r6/..., /sdk/...) is rewritten to the
// Cloudflare Worker as <cf>/k/<ACCESS_TOKEN>/<path> (token in the path: relative module imports keep it).
// Workers: same-origin blob trampolines that install this prelude, then importScripts / import the CF script.
// Both functions must stay self-contained: their source text is shipped into every trampoline.
function __cpXoInstall(cfg) {
  var G = globalThis;
  if (G.__cpXo) return;
  G.__cpXo = cfg;
  var RE = /^\/(runtime\/|game\/(archive|engine|r6)\/|sdk\/)/;
  function abs(u) {                                   // never resolve against blob: base URLs
    u = String(u && u.href !== undefined ? u.href : u);
    if (u.charAt(0) === "/" && u.charAt(1) !== "/") return cfg.pageOrigin + u;
    if (/^[a-z][a-z0-9+.-]*:/i.test(u)) return u;
    try { return new URL(u, G.location.href).href; } catch (e) { return u; }
  }
  function map(u) {                                   // -> CF URL or null
    var a = abs(u);
    if (a.indexOf(cfg.cfOrigin + "/") === 0) return a;
    if (a.indexOf(cfg.pageOrigin + "/") !== 0) return null;
    var rest = a.slice(cfg.pageOrigin.length), path = rest.split(/[?#]/)[0];
    return RE.test(path) ? cfg.base + rest : null;
  }
  G.__cpXoMap = map;
  // fetch
  if (G.fetch) {
    var F = G.fetch;
    G.fetch = function (input, init) {
      try {
        if (typeof input === "string" || input instanceof URL) { var m = map(input); if (m && m !== abs(input)) input = m; }
        else if (input && input.url) { var r = map(input.url); if (r && r !== input.url) input = new Request(r, input); }
      } catch (e) {}
      var p = F.call(this, input, init);
      // [io=shared] count /game/ bytes fetched by worker-side fetch (io2 prefetch helper) for the loader readout
      if (cfg.countFetch && typeof G.WorkerGlobalScope !== "undefined" && typeof G.BroadcastChannel !== "undefined") {
        var u = String(input && input.url || input);
        if (u.indexOf("/game/") >= 0) p = p.then(function (r) {
          if (!r.body || typeof TransformStream === "undefined") return r;
          var C = G.__cpXoIo || (G.__cpXoIo = {bc: new BroadcastChannel("cp2077-loader-io"), id: "f" + Math.random().toString(36).slice(2), bytes: 0, last: 0});
          var ts = new TransformStream({transform: function (chunk, ctl) {
            C.bytes += chunk.byteLength; var now = Date.now();
            if (now - C.last >= 500) { C.last = now; try { C.bc.postMessage({id: C.id, bytes: C.bytes}); } catch (e) {} }
            ctl.enqueue(chunk);
          }, flush: function () { try { C.bc.postMessage({id: C.id, bytes: C.bytes}); } catch (e) {} }});
          return new Response(r.body.pipeThrough(ts), {status: r.status, statusText: r.statusText, headers: r.headers});
        });
      }
      return p;
    };
  }
  // XMLHttpRequest (sync reads from pthreads included)
  if (G.XMLHttpRequest) {
    var XP = G.XMLHttpRequest.prototype, XO = XP.open;
    XP.open = function (method, url) {
      var args = Array.prototype.slice.call(arguments);
      try { var m = map(url); if (m) args[1] = m; } catch (e) {}
      return XO.apply(this, args);
    };
  }
  // importScripts (classic workers)
  if (typeof G.importScripts === "function") {
    var IS = G.importScripts;
    G.importScripts = function () {
      var a = Array.prototype.map.call(arguments, function (u) { var m = map(u); return m || u; });
      return IS.apply(this, a);
    };
  }
  // Classic worker scripts with RELATIVE dynamic import() (bink-decoder-worker.js: import("./cached-bink-movie-web.mjs")).
  // importScripts() of a cross-origin script makes it "muted": its import() base URL is about:blank. So fetch the
  // text with CORS, make relative import() specifiers absolute against the script's own CF URL, and run it from a
  // same-origin blob. Absolute import(...) URLs (rewritten in postMessage below) and importScripts keep working.
  G.__cpXoLoadPatched = function (t) {
    var x = new G.XMLHttpRequest();
    x.open("GET", t, false);
    x.send();
    if (x.status !== 200) throw new Error("worker script " + x.status + " " + t);
    var text = x.responseText.replace(/\bimport\(\s*(["'])(\.\.?\/[^"']+)\1\s*\)/g, function (m, q, p) {
      return "import(" + JSON.stringify(new URL(p, t).href) + ")";
    });
    G.importScripts(URL.createObjectURL(new Blob([text + "\n//# sourceURL=" + t], {type: "text/javascript"})));
  };
  var PATCH_IMPORTS = /\/runtime\/codec\/[^?#]*\.js(\?|#|$)/;
  // Worker: CF or rewritable script URLs -> same-origin blob trampoline
  if (G.Worker) {
    var W = G.Worker;
    var src = cfg.src;
    var conf = function () { return JSON.stringify(cfg); };
    var NW = function (url, opts) {
      var s = String(url && url.href !== undefined ? url.href : url);
      var target = s.indexOf("blob:") === 0 ? null : map(s);
      if (!target) return new W(url, opts);
      var module = opts && opts.type === "module", code;
      if (module) {
        var pre = URL.createObjectURL(new Blob([src + "\n__cpXoInstall(" + conf() + ");\n"], {type: "text/javascript"}));
        code = "import " + JSON.stringify(pre) + ";\nimport " + JSON.stringify(target) + ";\n";
      } else code = src + "\n__cpXoInstall(" + conf() + ");\n" +
        // [io=shared] the io bundle reads its switches from self.location.search: expose the script URL's query
        (cfg.locShim ? "(function(t){try{var u=new URL(t);if(!u.search)return;var L=self.location;var o={href:u.href,search:u.search,hash:'',pathname:u.pathname,origin:L.origin,protocol:L.protocol,host:L.host,hostname:L.hostname,port:L.port,toString:function(){return u.href}};Object.defineProperty(self,'location',{configurable:true,enumerable:true,get:function(){return o}})}catch(e){}})(" + JSON.stringify(target) + ");\n" : "") +
        (PATCH_IMPORTS.test(target) ? "__cpXoLoadPatched(" : "importScripts(") + JSON.stringify(target) + ");\n";
      var w = new W(URL.createObjectURL(new Blob([code], {type: "text/javascript"})), opts);
      // Messages that carry script URLs (Bink worker: runtimeUrl, vmUrl, ...) -> absolute CF URLs.
      var P = w.postMessage;
      w.postMessage = function (msg) {
        try {
          if (msg && typeof msg === "object" && Object.prototype.toString.call(msg) === "[object Object]" && !Array.isArray(msg)) {
            var copy = null;
            for (var k in msg) {
              var v = msg[k];
              if (typeof v === "string" && /Url$/.test(k)) { var m = map(v); if (m) { copy = copy || Object.assign({}, msg); copy[k] = m; } }
            }
            if (copy) { var a = Array.prototype.slice.call(arguments); a[0] = copy; return P.apply(this, a); }
          }
        } catch (e) {}
        return P.apply(this, arguments);
      };
      return w;
    };
    NW.prototype = W.prototype;
    G.Worker = NW;
  }
}
// Page bootstrap (main thread only).
//  - Bare /game boots the ORIGINAL flow (start screen -> main menu -> New Game). ?start=nomadfree = opt-in sandbox.
//  - Access code: ?key=<code> (shortcut link) or the code-entry panel; validated with ONE HEAD to the Worker,
//    stored in localStorage, then the page reloads straight into loading. Wrong code -> "Access denied".
//    Without a valid code no engine script is requested (CPB stays null).
function __cpXoBoot(opts) {
  // [cdn-fallback] 2026-10-22: the asset origin may be a new hostname (cdn.cyberpunk2077.lol) that some DNS resolvers
  // do not know yet. One synchronous probe before anything is requested: if the primary origin does not answer,
  // use the fallback origin (workers.dev) for the whole page. NXDOMAIN fails fast; ?cf=fallback forces it.
  if (opts.cfFallback) {
    var useFallback = new URLSearchParams(location.search).get("cf") === "fallback";
    if (!useFallback) {
      try { var px = new XMLHttpRequest(); px.open("GET", opts.cfOrigin + "/__launch?p=" + Date.now(), false); px.send(); if (px.status !== 200) useFallback = true; }
      catch (e) { useFallback = true; }
    }
    if (useFallback) opts = {cfOrigin: opts.cfFallback};
    globalThis.cp2077CfOrigin = opts.cfOrigin;
  }
  var q = new URLSearchParams(location.search), key = q.get("key"), store = "cp2077_link_key", changed = false;
  // [ask-always] user 2026-10-22: until the countdown ends every visit asks for the code. Never remember it: an
  // entered code only survives the one reload that starts loading (one-shot sessionStorage), and an old stored code
  // is wiped. ?key= works for that page load only.
  var once = "cp2077_code_once";
  try { localStorage.removeItem(store); } catch (e) {}
  if (!key) { try { key = sessionStorage.getItem(once); sessionStorage.removeItem(once); } catch (e) {} }
  if (q.has("key")) { q.delete("key"); changed = true; }
  if (changed) { try { history.replaceState(history.state, "", location.pathname + (q.toString() ? "?" + q : "") + location.hash); } catch (e) {} }
  var probe = function (code) { return fetch(opts.cfOrigin + "/k/" + encodeURIComponent(code) + "/runtime/game-files.json", {method: "HEAD", cache: "no-store"}); };
  var forget = function () { try { localStorage.removeItem(store); } catch (e) {} };
  var whenDom = function (f) { if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", f); else f(); };
  function codePanel(message) {
    globalThis.cp2077LoaderPreError = null;
    whenDom(function () {
      var l = document.getElementById("cp-loader"); if (!l || globalThis.CP_UNSUPPORTED) return;   // capcheck.js panel instead
      l.classList.add("locked");
      var s = document.getElementById("cp-stage"); if (s) s.textContent = "Access required";
      var wrap = l.querySelector(".cp-wrap"), f = document.getElementById("cp-code");
      if (!f) {
        f = document.createElement("form"); f.id = "cp-code"; f.className = "cp-code"; f.autocomplete = "off";
        f.innerHTML = '<label for="cp-code-in">EARLY ACCESS CODE</label><div class="cp-code-row"><input id="cp-code-in" type="password" inputmode="text" enterkeyhint="go" spellcheck="false" autocapitalize="off" autocomplete="off" required>' +
          '<button type="submit">ENTER</button></div><p class="cp-code-msg" id="cp-code-msg" role="alert"></p>';
        var hint = document.getElementById("cp-hint"); (wrap || l).insertBefore(f, hint ? hint.nextSibling : null);
        f.addEventListener("submit", function (ev) {
          ev.preventDefault();
          var inp = document.getElementById("cp-code-in"), msg = document.getElementById("cp-code-msg"), btn = f.querySelector("button");
          var code = inp.value.trim(); if (!code) return;
          btn.disabled = true; msg.className = "cp-code-msg"; msg.textContent = "Checking…";
          probe(code).then(function (r) {
            if (r.ok) { try { sessionStorage.setItem(once, code); } catch (e) {} msg.textContent = "Access granted"; location.reload(); return; }
            msg.className = "cp-code-msg bad"; msg.textContent = r.status === 401 ? "Access denied" : "Server error (" + r.status + "), try again";
            btn.disabled = false; inp.select();
          }, function () { msg.className = "cp-code-msg bad"; msg.textContent = "Network error, try again"; btn.disabled = false; });
        });
      }
      var m = document.getElementById("cp-code-msg"); if (m && message) { m.className = "cp-code-msg bad"; m.textContent = message; }
      var inp = document.getElementById("cp-code-in");
      if (inp && !inp.cpScroll) {   // phones: keep the field visible above the on-screen keyboard
        inp.cpScroll = true;
        var keep = function () { setTimeout(function () {   // only while the on-screen keyboard covers part of the page
          var vv = globalThis.visualViewport; if (vv && vv.height > innerHeight * 0.85) { try { scrollTo(0, 0); } catch (e) {} return; }
          try { f.scrollIntoView({block: "center", behavior: "smooth"}); } catch (e) {} }, 250); };
        inp.addEventListener("blur", function () { setTimeout(function () { try { scrollTo(0, 0); var w = f.closest(".cp-wrap"); if (w) w.scrollTop = 0; } catch (e) {} }, 300); });
        inp.addEventListener("focus", keep);
        if (globalThis.visualViewport) visualViewport.addEventListener("resize", function () { if (document.activeElement === inp) keep(); });
      }
      if (inp && !matchMedia("(pointer:coarse)").matches) { try { inp.focus({preventScroll: true}); } catch (e) { inp.focus(); } }
    });
  }
  // [launch] user 2026-10-22: no access code for players. Before LAUNCH a countdown replaces the loader; from then
  // on the public path token "play" (Worker gate) boots the game. A stored/linked code still works (testing).
  var LAUNCH = Date.parse("2026-10-22T22:00:00Z"), PUB = "play", launchedKey = "cp2077_launched";
  function countdown() {
    codePanel("");                                  // early-access code field under the countdown (user 2026-10-22)
    globalThis.cp2077LoaderPreError = null;
    var skew = 0, done = false;
    fetch(opts.cfOrigin + "/__launch", {cache: "no-store"}).then(function (r) { return r.json(); })
      .then(function (j) { if (j && j.now) { skew = j.now - Date.now(); if (j.launch) LAUNCH = j.launch; } }, function () {});
    whenDom(function () {
      var l = document.getElementById("cp-loader"); if (!l || globalThis.CP_UNSUPPORTED) return;
      l.classList.add("locked", "launch");
      var st = document.getElementById("cp-stage"); if (st) st.textContent = "Launching soon";
      var wrap = l.querySelector(".cp-wrap"), c = document.getElementById("cp-count");
      if (!c) {
        c = document.createElement("div"); c.id = "cp-count"; c.className = "cp-count"; c.setAttribute("role", "timer");
        c.innerHTML = '<div class="cp-count-lbl">LAUNCHES IN</div><div class="cp-count-dig" id="cp-count-dig" aria-live="off"></div>' +
          '<div class="cp-count-when">OCT 22 &middot; 5:00 PM CT</div>';
        var hint = document.getElementById("cp-hint"); (wrap || l).insertBefore(c, hint ? hint.nextSibling : null);
      }
      var dig = document.getElementById("cp-count-dig"), p2 = function (n) { return (n < 10 ? "0" : "") + n; };
      var u = function (n, t) { return '<span class="u"><b>' + p2(n) + '</b><i>' + t + '</i></span>'; };
      var tick = function () {
        if (done) return;
        var ms = LAUNCH - (Date.now() + skew);
        if (ms <= 0) {
          done = true; dig.innerHTML = u(0, "HRS") + u(0, "MIN") + u(0, "SEC");
          var cf = document.getElementById("cp-code"); if (cf) cf.remove();
          if (st) st.textContent = "Starting";
          var go = function () {
            probe(PUB).then(function (r) {
              if (r.ok) { try { sessionStorage.setItem(launchedKey, "1"); } catch (e) {} location.reload(); }
              else setTimeout(go, 3000);
            }, function () { setTimeout(go, 3000); });
          };
          return go();
        }
        var t = Math.floor(ms / 1000), d = Math.floor(t / 86400), h = Math.floor(t / 3600) % 24, m = Math.floor(t / 60) % 60, sec = t % 60;
        dig.innerHTML = (d ? u(d, "DAYS") : "") + u(h, "HRS") + u(m, "MIN") + u(sec, "SEC");
        setTimeout(tick, 1000 - ((Date.now() + skew) % 1000) + 5);
      };
      tick();
    });
  }
  globalThis.CPB = null;
  var launched = Date.now() >= LAUNCH;
  try { if (sessionStorage.getItem(launchedKey) === "1") launched = true; else if (sessionStorage.getItem("cp2077_wait") === "1") launched = false; } catch (e) {}   // wait: server said not yet (client clock ahead)
  if (!key) { if (!launched) return countdown(); key = PUB; }
  // [one-tab] user 2026-10-22: a second game tab hangs at "Mounting archives" (the first one holds the OPFS cache
  // files). The newest tab wins: it tells older game tabs to leave (they go to the home page and free the files).
  try {
    var bc = new BroadcastChannel("cp2077-game");
    bc.onmessage = function (e) { if (e.data && e.data.t === "takeover") { try { bc.close(); } catch (x) {} location.replace("/"); } };
    bc.postMessage({t: "takeover", at: Date.now()});
  } catch (e) {}
  var cfg = {cfOrigin: opts.cfOrigin, pageOrigin: location.origin, base: opts.cfOrigin + "/k/" + encodeURIComponent(key)};
  // ?engine= / ?io=shared selection lives in engine-select.js (runs after this); it sets cfg.locShim/countFetch
  // through globalThis.__cpXo before any worker exists.
  cfg.src = __cpXoInstall.toString();
  globalThis.CPB = cfg.base;
  __cpXoInstall(cfg);
  probe(key).then(function (r) {                   // stale/revoked stored code: back to the panel
    if (r.status === 401) { forget(); location.reload(); }      // stale stored code: fall back to the public launch flow
    else if (r.status === 403 && key === PUB) { try { sessionStorage.removeItem(launchedKey); sessionStorage.setItem("cp2077_wait", "1"); } catch (e) {} globalThis.CPB = null; location.reload(); }
  }, function () {});
}
