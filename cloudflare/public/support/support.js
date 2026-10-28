// [support] "Support the project" panel on the right side of the game's main menu, styled like the native menu, while the game's MAIN MENU is up (not in-game,
// not while loading). Shows the X account, matching the
// cyberpunk2077.lol home page. Main menu = the game shows its menu cursor and there is no player in a world
// (cp2077DebugState(): no player, or the menu's placeholder puppet at the world origin with no quest).
(function () {
  "use strict";
  var XURL = "https://x.com/omonkeyzz";
  var XSVG = '<svg viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M18.244 2.25h3.308l-7.227 8.26 8.502 11.24H16.17l-5.214-6.817L4.99 21.75H1.68l7.73-8.835L1.254 2.25H8.08l4.713 6.231zm-1.161 17.52h1.833L7.084 4.126H5.117z"/></svg>';
  var root = null;
  function build() {
    root = document.createElement("div"); root.id = "cp-support"; root.setAttribute("role", "navigation"); root.setAttribute("aria-label", "Support the project");
    root.innerHTML = '<div class="cps-frame">' +
      '<ul class="cps-list">' +
      '<li><a class="cps-item" href="' + XURL + '" target="_blank" rel="noopener noreferrer"><span class="cps-lbl"><span class="cps-ic">' + XSVG + '</span>@omonkeyzz</span><span class="cps-mark"><i></i></span></a></li>' +
      '</ul></div>';
    // keep clicks/keys/touches on the panel away from the game's own input handlers
    ["mousedown", "mouseup", "click", "pointerdown", "pointerup", "touchstart", "touchend", "wheel", "keydown"].forEach(function (t) {
      root.addEventListener(t, function (e) { e.stopPropagation(); }, {passive: true});
    });
    document.body.appendChild(root);
  }
  function baseMenu() {
    var de = document.documentElement;
    if (de.classList.contains("cp-loading") || document.getElementById("cp-loader") || G.CP_UNSUPPORTED) return false;
    var c = document.querySelector("#cp2077-canvas");
    if (!c) return false;
    var cursor = c.dataset.cp2077InkCursor === "1" || c.dataset.cp2077Relative !== "1";
    if (!cursor) return false;
    var m = G.cp2077EngineModule, s = null;
    try { s = m && typeof m.cp2077DebugState === "function" ? m.cp2077DebugState() : null; } catch (e) {}
    if (s && s.torn) return root && root.classList.contains("on");   // mid-write: keep the current state
    // the main menu has a placeholder puppet at the world origin and no quest; a real game has a quest or a world position
    var p = s && s.player;
    return !p || (!s.quest && Math.abs(p.x) < 50 && Math.abs(p.y) < 50 && Math.abs(p.z) < 50);
  }
  var G = globalThis;
  // [menu-only] user 2026-10-22: the panel stayed up on New Game / difficulty screens. The engine exposes no
  // "which menu screen" state, so track menu depth from input while the pre-game front end is up:
  //  - the first confirm (Enter/Space/click) passes the start screen ("press to continue") -> main menu, depth 0
  //  - a confirm on the main menu (Enter/Space, or a click in the menu column on the left) opens a sub-screen: depth+1
  //  - inside sub-screens a click on the content goes deeper; Esc / Backspace or a click on the footer (BACK) goes up
  // The panel shows only at depth 0 after the start screen. Leaving the front end (loading, in-game) resets all.
  var pastStart = false, depth = 0;
  function frontEnd() { return baseMenu(); }
  addEventListener("keydown", function (e) {
    if (!frontEnd() || e.repeat) return;
    if (e.code === "Enter" || e.code === "NumpadEnter" || e.code === "Space") { if (!pastStart) pastStart = true; else if (e.code !== "Space") depth++; }   // Space does not pick menu items
    else if ((e.code === "Escape" || e.code === "Backspace") && depth > 0) depth--;
  }, true);
  addEventListener("pointerdown", function (e) {
    var c = document.querySelector("#cp2077-canvas"); if (!c || !frontEnd() || (root && root.contains(e.target))) return;   // real clicks may land on an overlay above the canvas
    if (!pastStart) { pastStart = true; return; }
    var r = c.getBoundingClientRect(), x = (e.clientX - r.left) / r.width, y = (e.clientY - r.top) / r.height;
    if (e.button === 2 || y > 0.88) { if (depth > 0) depth--; }
    else if (depth > 0 || x < 0.42) depth++;
  }, true);
  function tick() {
    if (!document.body) return;
    if (!root) build();
    var base = baseMenu();
    if (!base && !(root && root.classList.contains("on"))) { if (document.getElementById("cp-loader") || (G.cp2077EngineModule && G.cp2077EngineModule.cp2077DebugState && (G.cp2077EngineModule.cp2077DebugState() || {}).quest)) { pastStart = false; depth = 0; } }
    var on = base && pastStart && depth === 0;
    if (on !== root.classList.contains("on")) {
      root.classList.toggle("on", on);
      if (on) requestAnimationFrame(function () { root.classList.add("vis"); }); else root.classList.remove("vis");
    }
  }
  setInterval(tick, 500);
})();
// [vidwarm] user 2026-10-22: pre-warm the lifepath-card videos (predecoded gzip frames) into Cache Storage at the main
// menu so hovering a card never waits on the network: manifest + first WARM_FRAMES frames of each, one range request
// per video, stored under the keys predecoded-bink-frames-web.mjs reads. Desktop only; once per browser (marker key).
(function () {
  var G = globalThis, HASHES = ["e2d45bc245a1f1fb62e62f61674da70250c3fdb34338d47b076bed823190e17b",
    "b04fc828038e4bbe1a9b269bed1b8eea533b93519bf670367ded62186e046d6f", "073a32b10641d5c9e74cfdbeff0d006724f88b7d2474f0b52e6cfac4af38af24"];
  var WARM_FRAMES = 60, KEY = function (h, w) { return "https://video-cache.cyberpunk2077.lol/pre/" + h + "/" + w; };
  function warmOne(c, base, h) {
    return c.match(KEY(h, "warm-done")).then(function (done) {
      if (done) return;
      return fetch(base + "/runtime/codec/bink/predecoded/" + h + "/manifest.json").then(function (r) {
        if (!r.ok) throw new Error("manifest " + r.status); return r.arrayBuffer();
      }).then(function (mb) {
        var m = JSON.parse(new TextDecoder().decode(mb)); if (m.assetSHA256 !== h) throw new Error("identity");
        c.put(KEY(h, "manifest.json"), new Response(mb));
        var rows = m.frames.slice(0, WARM_FRAMES), a = rows[0].packOffset, last = rows[rows.length - 1], b = last.packOffset + last.compressedBytes - 1;
        return fetch(base + "/runtime/codec/bink/predecoded/" + h + "/frames.gzpack", {cache: "no-store", headers: {Range: "bytes=" + a + "-" + b}})
          .then(function (r) { if (r.status !== 206) throw new Error("range " + r.status); return r.arrayBuffer(); })
          .then(function (buf) {
            if (buf.byteLength !== b - a + 1) throw new Error("short");
            var puts = rows.map(function (row, i) {
              var off = row.packOffset - a; return c.put(KEY(h, "f" + (i + 1)), new Response(buf.slice(off, off + row.compressedBytes)));
            });
            return Promise.all(puts).then(function () { return c.put(KEY(h, "warm-done"), new Response("1")); });
          });
      });
    });
  }
  G.cp2077VideoPrewarm = function () {
    if (G.cp2077VideoPrewarmStarted || G.CP_ENGINE_W32 || !G.CPB || typeof caches !== "object") return;
    G.cp2077VideoPrewarmStarted = true;
    caches.open("cp2077-video-v1").then(function (c) {
      return HASHES.reduce(function (p, h) { return p.then(function () { return warmOne(c, G.CPB, h).catch(function (e) { console.warn("CP2077 video prewarm " + h.slice(0, 8) + ": " + e); }); }); }, Promise.resolve());
    }).then(function () { console.log("CP2077 video prewarm done"); }, function () {});
  };
})();
// [vidwarm trigger] 15 s after the loader is gone (front end up), once per page
(function () {
  var t = setInterval(function () {
    if (document.getElementById("cp-loader") || document.documentElement.classList.contains("cp-loading")) return;
    clearInterval(t); setTimeout(function () { try { globalThis.cp2077VideoPrewarm(); } catch (e) {} }, 15000);
  }, 2000);
})();
// [vo] Voice-over player (default on; ?vo=0 disables). The port ships no Wwise VO banks; the engine overlay
// audioSceneUtils.cpp calls cp2077VO(ruidHi, ruidLo, variant 1=f|2=m, flags 1=player|2=rewind|4=holocall|8=helmet,
// seekSeconds) when a dialogue line starts. Lines come from one pack: <CPB>/runtime/vo/en_us.pack + en_us.index.json
// ({"<16-hex RUID>_<f|m>": [offset, length]}, real actor recordings under "r/<id>_<v>"), fetched with a Range
// request per line and decoded with the game's own AudioContext. One voice at a time: a new line stops the last one.
(function () {
  var q = new URLSearchParams(location.search);
  if (q.get("vo") === "0") return;
  var stats = globalThis.cp2077VOStats = {calls: 0, played: 0, missing: 0, errors: 0, last: null, index: 0};
  var cur = null, seq = 0, cache = new Map(), indexP = null;
  var base = function () { return globalThis.CPB + "/runtime/vo/"; };
  var index = function () {
    if (!indexP) indexP = fetch(base() + "en_us.index.json").then(function (r) { return r.ok ? r.json() : {}; })
      .then(function (j) { stats.index = Object.keys(j).length; return j; }, function () { indexP = null; stats.errors++; return {}; });
    return indexP;
  };
  var ctx = function () { var m = globalThis.cp2077EngineModule, a = m && m.cp2077Audio; return a && a.context && a.context.state !== "closed" ? a.context : null; };
  var hex8 = function (n) { return ("00000000" + (n >>> 0).toString(16)).slice(-8); };
  var load = function (c, key, ent) {
    if (cache.has(key)) return cache.get(key);
    var p = fetch(base() + "en_us.pack", {headers: {Range: "bytes=" + ent[0] + "-" + (ent[0] + ent[1] - 1)}})
      .then(function (r) { return r.ok ? r.arrayBuffer() : null; })
      .then(function (b) { return b && b.byteLength === ent[1] ? c.decodeAudioData(b) : null; })
      .catch(function () { stats.errors++; return null; });
    cache.set(key, p);
    if (cache.size > 48) cache.delete(cache.keys().next().value);
    return p;
  };
  // warm the index once the game is up so the first line does not wait for it
  var w = setInterval(function () { if (globalThis.CPB && ctx()) { clearInterval(w); index(); } }, 3000);
  globalThis.cp2077VO = async function (hi, lo, variant, flags, seek) {
    stats.calls++;
    var c = ctx(); if (!c || !globalThis.CPB) return;
    var my = ++seq, id = hex8(hi) + hex8(lo), v = variant === 2 ? "m" : "f", o = v === "m" ? "f" : "m";
    var ix = await index();
    var key = ["r/" + id + "_" + v, id + "_" + v, "r/" + id + "_" + o, id + "_" + o].find(function (k) { return ix[k]; });
    if (!key) { if (my === seq) { stats.missing++; stats.last = id + "_" + v + " (missing)"; } return; }
    var buf = await load(c, key, ix[key]);
    if (my !== seq) return;                         // a newer line started while this one was loading
    stats.last = key;
    if (!buf) { stats.missing++; return; }
    if (cur) { try { cur.stop(); } catch (e) {} }
    var s = c.createBufferSource(); s.buffer = buf; s.connect(c.destination);
    s.start(0, Math.max(0, Math.min(+seek || 0, buf.duration - 0.05)));
    cur = s; stats.played++;
    s.onended = function () { if (cur === s) cur = null; };
  };
})();
