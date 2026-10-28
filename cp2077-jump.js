// Test/user convenience: ?jump=difficulty|lifepath|bodytype|newgame drives the ORIGINAL engine menus
// with synthetic mouse/keyboard input (same path as a real user; no UI is replaced). Inactive unless
// the jump URL parameter is present.
//   jump=difficulty|lifepath|bodytype : stop on that screen (state 'done').
//   jump=newgame : continue through body type -> customization -> attributes -> summary -> StartNewGame
//                  and watch the new-game loading (states customize, attributes, summary, loading,
//                  done-newgame).
// Optional: lifepath=nomad|streetkid|corpo (default nomad for newgame, streetkid otherwise),
// bodytype=male|female (default male), jumpWait=<s> between steps (default 30), jumpSettle=<s> after
// menu ready (default 10), jumpMinFrames=<n> presented frames before the menu counts as ready
// (default 2600), jumpPuppetTimeout=<s> soft wait for body-type puppets (default 180),
// jumpLoadTimeout=<s> new-game loading timeout (default 1800), jumpConfirmKey=<keyCode> (default 13
// Enter; 70 = F is the other DialogConfirm key), jumpSkipKey=<keyCode> (default 32 Space,
// skip_loading), jumpSkipLoading=0 to not press the skip key on the loading screen.
(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  // [missionflow] ?start=nomad / ?start=nomadquest no longer click through the menus: the engine starts the Nomad new game
  // itself (gameFramework preGameRequestsHandler.cpp overlay). The old menu-driving flow stays available as ?jump=newgame.
  const target = (params.get('jump') || '').toLowerCase();
  if (!target) return;
  const order = ['difficulty', 'lifepath', 'bodytype', 'newgame'];
  const log = (...a) => console.log('[cp2077-jump] CP2077 jump:', ...a);
  const state = globalThis.cp2077Jump = { target, state: 'waiting-for-menu', step: 0, log: [], lifepath: null, stages: [], lastStage: null };
  const note = msg => { state.log.push({ at: performance.now(), msg }); log(msg); };
  if (!order.includes(target)) { state.state = 'error'; note(`unknown jump target "${target}" (use ${order.join('|')})`); return; }
  const num = (k, d) => { const v = Number(params.get(k)); return Number.isFinite(v) && params.has(k) ? v : d; };
  const stepWaitMs = num('jumpWait', 30) * 1000;
  const settleMs = num('jumpSettle', 10) * 1000;
  const minFrames = num('jumpMinFrames', 2600);
  const puppetTimeoutMs = num('jumpPuppetTimeout', 180) * 1000;
  const loadTimeoutMs = num('jumpLoadTimeout', 1800) * 1000;
  const confirmKey = num('jumpConfirmKey', 13);
  const skipKey = num('jumpSkipKey', 32);
  const skipLoading = num('jumpSkipLoading', 1) !== 0;
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const frames = () => globalThis.cp2077EngineModule?.cp2077PresentationStats?.validatedFrames || 0;
  const loadingRows = () => globalThis.cp2077EngineModule?.cp2077NativeLoadingStageDiagnostics || [];

  // Lifepath cards on the V'S LIFEPATH screen (canvas space; cards span y ~112-480, x Nomad 208-457,
  // Streetkid 507-756, Corpo 806-1055). Street Kid (727, 257.5) is the coordinate the original
  // bodytype driver used.
  const lifepaths = { nomad: [332, 257.5], streetkid: [727, 257.5], corpo: [930, 257.5] };
  const lifepathAlias = { street: 'streetkid', street_kid: 'streetkid', kid: 'streetkid', corporate: 'corpo' };
  let lifepath = (params.get('lifepath') || (target === 'newgame' ? 'nomad' : 'streetkid')).toLowerCase();
  lifepath = lifepathAlias[lifepath] || lifepath;
  if (!lifepaths[lifepath]) { state.state = 'error'; note(`unknown lifepath "${lifepath}" (use ${Object.keys(lifepaths).join('|')})`); return; }
  state.lifepath = lifepath;
  // BODY TYPE screen: male panel on the left (x ~355-620), female on the right (x ~670-935), y ~80-670.
  const bodyType = (params.get('bodytype') || 'male').toLowerCase() === 'female' ? 'female' : 'male';
  const bodyPanels = { male: [487, 397.5], female: [800, 397.5] };

  // Coordinates are in the engine's 1280x720 canvas space (derived from the CDP test scripts,
  // whose 1280x765 viewport put the canvas at y offset 22.5).
  let cursor = null;
  const canvasEl = () => document.getElementById('cp2077-canvas');
  const toClient = (x, y) => {
    const c = canvasEl(), r = c.getBoundingClientRect();
    // [potato] x/y are authored in the UNSCALED render space; with ?potato's render scale the canvas is smaller
    // (e.g. 1024x576), so divide its size by the scale gpu-device-web.cpp publishes (absent = 1 = old behaviour).
    const m = globalThis.cp2077EngineModule, sx = (m && m.cp2077PotatoScaleX) || 1, sy = (m && m.cp2077PotatoScaleY) || 1;
    return [r.left + x * r.width / ((c.width || 1280) / sx), r.top + y * r.height / ((c.height || 720) / sy)];
  };
  const fire = (type, x, y, extra = {}) => {
    const c = canvasEl();
    const [cx, cy] = toClient(x, y);
    const [px, py] = cursor ? toClient(cursor[0], cursor[1]) : [cx, cy];
    const ev = new MouseEvent(type, {
      bubbles: true, cancelable: true, view: window, clientX: cx, clientY: cy, screenX: cx, screenY: cy,
      movementX: Math.round(cx - px), movementY: Math.round(cy - py), button: 0, buttons: 0, ...extra,
    });
    cursor = [x, y];
    // [saves] Mark the dispatch as synthetic: input-web.cpp otherwise drops unlocked moves while the engine is in
    // relative-cursor mode (menus too), and a script cannot take pointer lock without a user gesture.
    const m = globalThis.cp2077EngineModule; if (m) m.cp2077SyntheticPointer = true;
    try { c.dispatchEvent(ev); } finally { if (m) m.cp2077SyntheticPointer = false; } // mouseup bubbles to window, where the engine listens for it
  };
  const walk = async (x0, y0, x1, y1, n = 40) => {
    for (let i = 0; i <= n; i++) { fire('mousemove', x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n); await sleep(35); }
  };
  const walkTo = async (x, y, n = 40) => { const [x0, y0] = cursor || [640, 360]; await walk(x0, y0, x, y, n); };
  const click = async (x, y) => {
    await sleep(400);
    fire('mousedown', x, y, { buttons: 1 });
    await sleep(150);
    fire('mouseup', x, y);
  };
  // Keyboard: the engine registers keydown/keyup on window and reads keyCode (input-web.cpp ignores
  // keyCode 0). Chrome drops keyCode from the KeyboardEvent init dict, so it is set on the instance.
  // The keyup is always sent: InputSystemWeb::Queue ignores a press while the key is still down.
  const keyNames = { 13: ['Enter', 'Enter'], 32: [' ', 'Space'], 70: ['f', 'KeyF'], 27: ['Escape', 'Escape'] };
  const fireKey = (type, keyCode) => {
    const [key, code] = keyNames[keyCode] || [String.fromCharCode(keyCode), ''];
    const ev = new KeyboardEvent(type, { key, code, bubbles: true, cancelable: true, view: window });
    for (const p of ['keyCode', 'which']) Object.defineProperty(ev, p, { get: () => keyCode });
    window.dispatchEvent(ev);
  };
  const pressKey = async (keyCode, label) => {
    note(`key ${label} (keyCode ${keyCode})`);
    try { fireKey('keydown', keyCode); await sleep(250); } finally { fireKey('keyup', keyCode); }
    await sleep(100);
  };
  const waitFrames = async (ms, label) => {
    const f0 = frames(), t0 = performance.now();
    await sleep(ms);
    note(`${label}: waited ${Math.round(performance.now() - t0)} ms, presented +${frames() - f0} (total ${frames()})`);
  };
  const softWait = async (pred, ms, label) => {
    const t0 = performance.now();
    while (performance.now() - t0 < ms) {
      if (pred()) { note(`${label}: ok after ${Math.round(performance.now() - t0)} ms`); return true; }
      await sleep(250);
    }
    note(`${label}: soft timeout after ${ms} ms, continuing`);
    return false;
  };

  // New-game loading watcher. The diagnostics array is a 64-entry ring shared with puppet/readiness
  // probes, so it is polled and only NEW "loading stage probe: phase=enter" rows count; rows already
  // in the ring when watching starts (menu session) are marked seen.
  const seenRows = new WeakSet();
  let watching = false;
  const stageRe = /^CP2077 loading stage probe: phase=enter stage=(.*) value=(\d+)/;
  const pollStages = () => {
    if (!watching) return;
    for (const row of loadingRows()) {
      if (!row || seenRows.has(row)) continue;
      seenRows.add(row);
      const m = stageRe.exec(row.message || '');
      if (!m) continue;
      const entry = { stage: m[1], value: Number(m[2]), at: row.timestamp };
      state.stages.push(entry); state.lastStage = entry;
      note(`new-game loading stage: ${entry.stage} (value ${entry.value})`);
    }
  };
  setInterval(pollStages, 250);
  const startWatchingStages = () => {
    if (watching) return;
    for (const row of loadingRows()) if (row) seenRows.add(row);
    watching = true;
  };
  const loadingStarted = () => { pollStages(); return state.stages.length > 0; };
  const puppetSignalSince = since => () => loadingRows()
    .some(r => r && Date.parse(r.timestamp) >= since && /^CP2077 puppet (ready probe|customization probe: phase=completed)/.test(r.message));

  let bodyTypeShownAt = 0;
  const menuRowStep = num('jumpMenuRowStep', 37.5); // measured: CONTINUE 308.5, NEW GAME 346.5, LOAD GAME 384 (canvas y)
  // [saves] A NEW GAME click must never restore a save: game state probes (captured in the loading
  // ring by native-browser.html) showing gsmState_SessionRestoreFromSave after step 1 = wrong menu item.
  const restoreStartedSince = since => loadingRows()
    .some(r => r && Date.parse(r.timestamp) >= since && /^CP2077 game state probe: phase=(incoming|active) class=gsmState_SessionRestoreFromSave/.test(r.message));
  // [state name after the step, screen being acted on, action]
  const steps = [
    ['difficulty', 'main menu NEW GAME', async () => {
      // [saves] With any save present the original menu lists CONTINUE first and NEW GAME second.
      // filesystem-web.js exposes the restored slots (Module.cp2077RestoredSaveSlots); ?jumpMenuRow=<n>
      // (0-based) forces the row. Rows are menuRowStep apart in canvas space (?jumpMenuRowStep).
      const slots = globalThis.cp2077EngineModule?.cp2077RestoredSaveSlots;
      const row = params.has('jumpMenuRow') ? num('jumpMenuRow', 0) : (Array.isArray(slots) && slots.length ? 1 : 0);
      const y = 308.5 + row * menuRowStep;
      note(`main menu: ${Array.isArray(slots) ? slots.length : 'unknown'} restored save slot(s) -> NEW GAME is row ${row} (y=${y})`);
      await walk(640, 360, 163, y, 20); await click(163, y);
    }],
    ['lifepath', 'difficulty selection', async () => {
      await walk(152, 245.5, 541, 587.5); await walk(388, 537.5, 640, 597.5);
      await walk(640, 597.5, 640, 607.5); await click(640, 607.5);
    }],
    ['bodytype', `life path selection (${lifepath})`, async () => {
      const [x, y] = lifepaths[lifepath];
      await walk(487, 557.5, x, y - 5); await walk(x, y - 5, x, y, 5); await click(x, y);
      bodyTypeShownAt = Date.now();
    }],
    ['customize', `body type (${bodyType})`, async () => {
      // The panels accept a click before the puppets are ready, but customization inherits the
      // puppets, so give them time (soft: the puppet probes are count-capped).
      await softWait(puppetSignalSince(bodyTypeShownAt - 1000), puppetTimeoutMs, 'body type puppets');
      const [x, y] = bodyPanels[bodyType];
      await walkTo(x, y - 20); await walk(x, y - 20, x, y, 5); await click(x, y);
    }],
    ['attributes', 'customization (confirm = one_click_confirm -> NextMenu)', async () => {
      await pressKey(confirmKey, 'confirm on customization');
    }],
    ['summary', 'attributes (confirm shows the unspent-points popup, confirm again accepts)', async () => {
      // If no points were unspent the first confirm already goes to the summary, and a second
      // confirm there is still a valid start, so watch for loading from here on.
      startWatchingStages();
      await pressKey(confirmKey, 'confirm on attributes');
      await sleep(3000);
      if (loadingStarted()) return;
      await pressKey(confirmKey, 'confirm on attributes popup');
    }],
    ['loading', 'summary (confirm -> outro -> StartNewGame)', async () => {
      // Every accepted confirm re-runs the outro that calls StartNewGame, so retry only after a long
      // quiet window (m_loadingFinished / WaitForRunningInstalations can reject early presses).
      for (let attempt = 1; attempt <= 3; attempt++) {
        if (loadingStarted()) { note('loading already started, not pressing confirm'); return; }
        await pressKey(confirmKey, `confirm on summary (attempt ${attempt}/3)`);
        const t0 = performance.now();
        while (performance.now() - t0 < 25000) { if (loadingStarted()) return; await sleep(250); }
        note('no new loading stage 25 s after summary confirm');
      }
      throw new Error('summary confirm did not start a new game (no loading stage after 3 attempts)');
    }],
  ];
  const stepCount = target === 'newgame' ? steps.length : order.indexOf(target) + 1;

  const watchNewGameLoading = async () => {
    state.state = 'loading';
    const t0 = performance.now();
    let skipPresses = 0, lastSkip = 0, lastReport = t0;
    for (;;) {
      pollStages();
      const last = state.lastStage?.stage || '';
      if (last === 'WaitingForUpdateBehindLoadingScreen') break;
      if (skipLoading && last === 'WaitingForUIAcceptance' && skipPresses < 12 && performance.now() - lastSkip > 5000) {
        // InitialLoadingScreen registers its UI_Skip (Space) handler once assets are complete and it
        // fires on press. Only pressed in this stage (Space is jump in gameplay).
        lastSkip = performance.now(); skipPresses++;
        await pressKey(skipKey, `skip loading screen (${skipPresses})`);
        continue;
      }
      if (performance.now() - lastReport > 30000) {
        lastReport = performance.now();
        note(`loading: ${Math.round((lastReport - t0) / 1000)} s, stage=${last || 'none'} presented=${frames()}`);
      }
      if (performance.now() - t0 > loadTimeoutMs) {
        throw new Error(`new-game loading timeout after ${loadTimeoutMs / 1000} s, last stage=${last || 'none'}`);
      }
      await sleep(250);
    }
    note(`new game loaded: ${state.stages.length} stages in ${Math.round((performance.now() - t0) / 1000)} s`);
    state.state = 'done-newgame';
  };

  const menuReady = () => {
    const m = globalThis.cp2077EngineModule;
    if (!m) return false;
    const gate = (m.cp2077NativeSessionMenuDiagnostics || []).some(r => /activeUI=3\b/.test(r.message));
    const stage = (m.cp2077NativeLoadingStageDiagnostics || []).some(r => /WaitingForUpdateBehindLoadingScreen/.test(r.message));
    return (gate || stage) && frames() >= minFrames;
  };

  (async () => {
    note(`active, target=${target} lifepath=${lifepath} bodytype=${bodyType} wait=${stepWaitMs}ms settle=${settleMs}ms minFrames=${minFrames}`);
    let lastReport = performance.now();
    while (!menuReady()) {
      await sleep(250);
      if (performance.now() - lastReport > 15000) {
        lastReport = performance.now();
        const stage = loadingRows().at(-1)?.message || 'engine not started';
        note(`still waiting for main menu: presented=${frames()} stage=${stage.replace(/^CP2077 /, '')}`);
      }
    }
    note(`main menu ready (presented=${frames()}), settling`);
    await waitFrames(settleMs, 'settle');
    for (let i = 0; i < stepCount; i++) {
      const [name, from, run] = steps[i];
      state.step = i + 1; state.state = `clicking:${name}`;
      note(`step ${i + 1}/${stepCount}: ${from} -> ${name}`);
      const stepStartedAt = Date.now() - 1000;
      await run();
      if (name === 'loading') break; // the loading watcher takes over
      // newgame exposes the screen it is on (customize/attributes/summary) after body type.
      state.state = target === 'newgame' && i >= 3 ? name : `waiting:${name}`;
      await waitFrames(stepWaitMs, `after step ${i + 1}`);
      if (i === 0 && restoreStartedSince(stepStartedAt)) {
        throw new Error('step 1 clicked CONTINUE (gsmState_SessionRestoreFromSave started), not NEW GAME: clear the test profile saves or set ?jumpMenuRow');
      }
    }
    if (target === 'newgame') { await watchNewGameLoading(); note('done, new game started'); return; }
    state.state = 'done';
    note(`done, should now be on ${target}`);
  })().catch(e => { state.state = 'error'; note(`failed: ${e && e.stack || e}`); });
})();
// [sandbox-floor] ?sandbox sessions: the engine already skips the long loading screen for ?sandbox
// (splashScreenLoadingScreenLogicController overlay). This is the page-side fallback for bundles
// without that fix: while the newest loading stage is WaitingForUIAcceptance, press the skip key
// (default Space, ?jumpSkipKey) every 5 s, at most 12 times. Stops at the next stage. ?jumpSkipLoading=0
// disables it. State in globalThis.cp2077SandboxSkip.
(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  const sb = params.get('sandbox') ?? params.get('start'); // [nomad] ?start=nomad sessions skip too
  if (sb === null || sb === '0' || sb === 'false' || sb === 'nomadquest' || sb === 'nomad' || params.get('jump')) return; // [missionflow] nomad: own block below
  if (params.get('jumpSkipLoading') === '0') return;
  const skipKey = Number(params.get('jumpSkipKey')) || 32;
  const st = globalThis.cp2077SandboxSkip = { state: 'watching', presses: 0, lastStage: null, log: [] };
  const note = m => { st.log.push({ at: performance.now(), m }); console.log('[cp2077-jump] CP2077 sandbox skip:', m); };
  const seen = new WeakSet();
  const re = /^CP2077 loading stage probe: phase=enter stage=(.*) value=(\d+)/;
  const fireKey = (type) => {
    const ev = new KeyboardEvent(type, { key: skipKey === 32 ? ' ' : String.fromCharCode(skipKey), code: skipKey === 32 ? 'Space' : '', bubbles: true, cancelable: true, view: window });
    for (const p of ['keyCode', 'which']) Object.defineProperty(ev, p, { get: () => skipKey });
    window.dispatchEvent(ev);
  };
  let lastPress = 0;
  const timer = setInterval(() => {
    const rows = globalThis.cp2077EngineModule?.cp2077NativeLoadingStageDiagnostics || [];
    for (const row of rows) {
      if (!row || seen.has(row)) continue;
      seen.add(row);
      const m = re.exec(row.message || '');
      if (m) st.lastStage = m[1];
    }
    if (st.lastStage === 'WaitingForUIAcceptance') {
      if (st.presses < 12 && performance.now() - lastPress > 5000) {
        lastPress = performance.now(); st.presses++;
        note(`WaitingForUIAcceptance: pressing keyCode ${skipKey} (${st.presses})`);
        fireKey('keydown'); setTimeout(() => fireKey('keyup'), 250);
      }
    } else if (st.lastStage === 'Finishing' || st.lastStage === 'WaitingForUpdateBehindLoadingScreen') {
      st.state = st.presses ? 'done-pressed' : 'done-no-press-needed';
      note(`stage ${st.lastStage}: ${st.state}`);
      clearInterval(timer);
    }
  }, 250);
})();
// [missionflow] ?start=nomad (alias ?start=nomadquest): the engine starts the REAL q000_nomad new game by itself once
// the main menu is up (gameFramework preGameRequestsHandler.cpp overlay -> the menu's own StartNewGame with the default
// Nomad character). This block only (1) shows a "Starting the Nomad prologue..." banner until the player is in the
// game, (2) presses the loading screen's continue key (Space) at WaitingForUIAcceptance like a player would
// (&jumpSkipLoading=0 off), (3) adds the H help overlay (controls + where the objective is). It never sends mouse input
// and never stops key events (the engine still receives every key). State in globalThis.cp2077Missionflow.
(() => {
  'use strict';
  const params = new URLSearchParams(location.search);
  const start = params.get('start');
  if ((start !== 'nomad' && start !== 'nomadquest') || params.has('jump') || params.get('missionflow') === '0') return;
  const st = globalThis.cp2077Missionflow = { phase: 'menu', sessionsActive: 0, skipPresses: 0, lastStage: null, help: false, log: [] };
  const note = m => { st.log.push({ at: Math.round(performance.now()), m }); console.log('[missionflow] ' + m); };
  const css = `
    #mf-banner{position:fixed;left:50%;top:18px;transform:translateX(-50%);z-index:2147483000;pointer-events:none;
      font:600 15px/1.35 system-ui,"Segoe UI",sans-serif;color:#f3e600;background:rgba(10,10,14,.82);border:1px solid rgba(243,230,0,.55);
      padding:10px 18px;border-radius:4px;text-align:center;letter-spacing:.02em;max-width:calc(100vw - 32px);box-sizing:border-box}
    #mf-banner small{display:block;font-weight:400;color:#d8d8d8;font-size:12.5px;margin-top:3px}
    #mf-help{position:fixed;right:16px;top:50%;transform:translateY(-50%);z-index:2147483000;pointer-events:none;
      font:13px/1.45 system-ui,"Segoe UI",sans-serif;color:#e8e8e8;background:rgba(10,10,14,.86);border:1px solid rgba(94,246,255,.4);
      padding:12px 16px;border-radius:4px;width:310px;max-width:calc(100vw - 32px);box-sizing:border-box}
    #mf-help[hidden]{display:none}
    ${params.get('perf') === '1' ? '' : '#cp2077-performance-overlay{display:none!important}'}
    #mf-help h3{margin:0 18px 6px 0;font-size:14px;color:#5ef6ff;letter-spacing:.04em}
    #mf-help p{margin:6px 0 8px;color:#f3e600}
    #mf-help table{border-collapse:collapse;width:100%}
    #mf-help td{padding:1px 0;vertical-align:top}
    #mf-help td:first-child{color:#5ef6ff;white-space:nowrap;padding-right:10px;font-weight:600}
    #mf-help .mf-foot{margin-top:8px;color:#a0a0a0;font-size:12px}
    #mf-help button{pointer-events:auto;position:absolute;right:6px;top:4px;background:none;border:0;color:#a0a0a0;font-size:16px;cursor:pointer}`;
  const help = `<button type="button" aria-label="Close help">&#x2715;</button><h3>NOMAD PROLOGUE - CONTROLS</h3>
    <p>Your current objective is shown at the top right of the screen. Follow the yellow quest marker and the yellow route on the minimap.</p>
    <table>
    <tr><td>Click</td><td>capture the mouse (Esc releases it)</td></tr>
    <tr><td>W A S D</td><td>move / drive</td></tr>
    <tr><td>Mouse</td><td>look around</td></tr>
    <tr><td>F</td><td>interact, pick the highlighted dialogue option, get in / out of the car</td></tr>
    <tr><td>Wheel / Q E</td><td>move between dialogue options</td></tr>
    <tr><td>Space</td><td>jump (in the car: handbrake)</td></tr>
    <tr><td>Shift / C</td><td>sprint / crouch</td></tr>
    <tr><td>Q (car)</td><td>switch camera</td></tr>
    <tr><td>C</td><td>continue a tutorial popup</td></tr>
    <tr><td>M / J</td><td>map / journal (objectives)</td></tr>
    <tr><td>Esc</td><td>pause menu</td></tr>
    </table>
    <div class="mf-foot">Press H to show or hide this help. Checkpoints are saved automatically.</div>`;
  let banner = null, helpEl = null, helpTimer = 0;
  const showHelp = (on, autoHideMs) => {
    if (!ensureDom()) return;
    helpEl.hidden = !on; st.help = on; clearTimeout(helpTimer);
    if (on && autoHideMs) helpTimer = setTimeout(() => showHelp(false), autoHideMs);
  };
  function ensureDom() {
    if (banner) return true;
    if (!document.body || !document.head) return false;
    const s = document.createElement('style'); s.textContent = css; document.head.appendChild(s);
    banner = document.createElement('div'); banner.id = 'mf-banner'; banner.setAttribute('role', 'status'); document.body.appendChild(banner);
    helpEl = document.createElement('div'); helpEl.id = 'mf-help'; helpEl.innerHTML = help; helpEl.hidden = true; document.body.appendChild(helpEl);
    helpEl.querySelector('button').addEventListener('click', e => { e.preventDefault(); showHelp(false); });
    return true;
  }
  let bannerText = '';
  const setBanner = (title, sub) => {
    const text = title ? `${title}<small>${sub || ''}</small>` : '';
    if (text === bannerText || !ensureDom()) return;
    bannerText = text; banner.hidden = !title; banner.innerHTML = text;
  };
  // H toggles the help (the key still reaches the engine: H is only UI_ContextHelp inside menus).
  window.addEventListener('keydown', e => {
    if (e.repeat || !(e.code === 'KeyH' || e.key === 'h' || e.key === 'H')) return;
    showHelp(helpEl ? helpEl.hidden : true);
  }, true);
  const skipKey = Number(params.get('jumpSkipKey')) || 32;
  const skip = params.get('jumpSkipLoading') !== '0';
  const fireKey = type => {
    const ev = new KeyboardEvent(type, { key: skipKey === 32 ? ' ' : String.fromCharCode(skipKey), code: skipKey === 32 ? 'Space' : '', bubbles: true, cancelable: true, view: window });
    for (const p of ['keyCode', 'which']) Object.defineProperty(ev, p, { get: () => skipKey });
    window.dispatchEvent(ev);
  };
  const seen = new WeakSet();
  const stageRe = /^CP2077 loading stage probe: phase=enter stage=(.*) value=(\d+)/;
  const activeRe = /^CP2077 game state probe: phase=active class=gsmState_(\w+)/;
  const t0 = performance.now();
  let lastPress = 0, pressesThisLoad = 0;
  const tick = () => {
    if (!ensureDom()) return;
    const m = globalThis.cp2077EngineModule;
    for (const row of m?.cp2077NativeLoadingStageDiagnostics || []) {
      if (!row || seen.has(row)) continue;
      seen.add(row);
      const msg = row.message || '';
      let r;
      if ((r = stageRe.exec(msg))) {
        st.lastStage = r[1];
        if (r[1] !== 'WaitingForUIAcceptance') pressesThisLoad = 0;
      } else if ((r = activeRe.exec(msg))) {
        if (r[1] === 'SessionActive') {
          st.sessionsActive++;
          note(`SessionActive #${st.sessionsActive} after ${Math.round((performance.now() - t0) / 1000)} s`);
          if (st.sessionsActive === 1) st.phase = 'menu-active';
          else { st.phase = 'in-game'; showHelp(true, 30000); note('in game: banner hidden, help shown for 30 s (H toggles)'); }
        } else if (r[1] === 'Session' && st.sessionsActive >= 1 && st.phase !== 'in-game') {
          st.phase = 'loading-newgame'; note('new game session started (StartNewGame, Nomad)');
        }
      }
    }
    if (st.phase === 'menu') setBanner('Starting the Nomad prologue...', 'Loading the engine and the main menu. No input needed.');
    else if (st.phase === 'menu-active') setBanner('Starting the Nomad prologue...', 'Creating V (Nomad) and starting a new game. No input needed.');
    else if (st.phase === 'loading-newgame') setBanner('Starting the Nomad prologue...', `Loading the Badlands (${st.lastStage || 'starting'}). Press H for the controls.`);
    else setBanner('');
    if (skip && st.lastStage === 'WaitingForUIAcceptance' && pressesThisLoad < 12 && performance.now() - lastPress > 5000) {
      lastPress = performance.now(); pressesThisLoad++; st.skipPresses++;
      note(`loading screen WaitingForUIAcceptance: pressing keyCode ${skipKey} (${pressesThisLoad})`);
      fireKey('keydown'); setTimeout(() => fireKey('keyup'), 250);
    }
    if (st.phase === 'in-game' && performance.now() - lastPress > 6000) { clearInterval(timer); note('done'); }
  };
  const timer = setInterval(tick, 250);
})();
