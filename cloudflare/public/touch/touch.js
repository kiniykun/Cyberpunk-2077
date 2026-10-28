// [touch] On-screen touch controls for phones/tablets (cloudflare/public/touch/touch.js, inlined into the page by
// scripts/make-r2-page.mjs; the Vercel page inherits it). Active only on a coarse pointer (or after the first touch
// event), or with ?touch=1 on desktop for testing; ?touch=0 disables it. Without either, nothing is built or hooked,
// so the desktop page behaves exactly as before. Design: public/touch/mockup.html.
//
// Event delivery (see src/input-web.cpp): the engine reads emscripten keydown/keyup on window (keyCode), mousedown on
// #cp2077-canvas, mouseup on window and mousemove on the canvas (movementX/Y = camera / relative cursor, clientX/Y =
// absolute ink cursor). In gameplay it drops unlocked moves unless Module.cp2077SyntheticPointer is set during the
// dispatch (the path cp2077-jump.js uses), so look deltas are dispatched as synthetic canvas mousemoves with that
// flag. Phones have no pointer lock: the canvas mousedown handler's requestPointerLock is shadowed only while a
// synthetic mousedown is being dispatched.
//
// Context (which controls show): the engine's own state. Module.cp2077TouchState() (engine-overlay gameInput
// inputManager.cpp + gameInteractions interactionVisualizerController.cpp, [touch]) returns the active player / UI input
// context (Exploration, Aiming, Vision, VehicleDrive, ...) and the number of on-screen interaction prompts (dialog =
// choice hubs). Menus = the game shows a cursor (canvas.dataset.cp2077InkCursor / cp2077Relative, input-web.cpp).
// "Weapon drawn" has no input context of its own: it follows Aiming and recent FIRE / AIM / RELOAD / DRAW use.
(() => {
  'use strict';
  // ---- pure helpers (also unit-tested from node) ----
  const DIAG = 0.3827;   // sin(22.5deg): 8 equal 45deg sectors
  function dirKeys(dx, dy, radius, dead) {
    const len = Math.hypot(dx, dy);
    if (len < (dead == null ? 0.22 : dead) * radius || !len) return {keys: [], len: len / radius};
    const ux = dx / len, uy = dy / len, keys = [];
    if (uy < -DIAG) keys.push('W');
    if (uy > DIAG) keys.push('S');
    if (ux < -DIAG) keys.push('A');
    if (ux > DIAG) keys.push('D');
    return {keys, len: len / radius};
  }
  // s: {loading, menu, ctx, prompts, dialog, combat}
  function pickContext(s) {
    if (s.loading) return 'loading';
    if (s.menu) return 'menu';
    const c = s.ctx || '';
    if (c === 'VehiclePassenger') return 'passenger';
    if (/^Vehicle/.test(c)) return 'vehicle';   // VehicleDrive / TankDrive / DriverCombat / OnlyForward / NoDrive...
    if (s.dialog > 0 && !s.combat) return 'dialogue';
    if (s.combat || c === 'Aiming') return 'combat';
    return 'foot';
  }
  if (typeof window === 'undefined') { if (typeof module !== 'undefined') module.exports = {dirKeys, pickContext}; return; }

  const Q = location.search;
  if (/[?&]touch=0(&|$)/.test(Q)) return;
  const forced = /[?&]touch=1(&|$)/.test(Q);
  const numQ = (name, def, lo, hi) => { const m = new RegExp('[?&]' + name + '=([\\d.]+)').exec(Q); const v = m ? Number(m[1]) : NaN; return isFinite(v) ? Math.max(lo, Math.min(hi, v)) : def; };
  const SENS = numQ('touchSens', 1.6, 0.1, 10);
  // Menu clicks: the engine pushes queued button events BEFORE the frame's mouse axis events, so the press must come
  // a few frames after the move that brought the ink cursor there (jump.js waits 400 ms); then hold it a while.
  const CLICK_DELAY = numQ('touchClickDelay', 200, 0, 2000), CLICK_HOLD = numQ('touchClickHold', 120, 20, 2000);
  const COMBAT_MS = numQ('touchCombatSec', 20, 2, 600) * 1000;   // weapon-drawn layout lingers this long after combat input
  const coarse = !!(window.matchMedia && matchMedia('(pointer: coarse)').matches);
  let started = false;
  const start = () => {
    if (started) return; started = true;
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', build, {once: true}); else build();
  };

  // ---- look: frames + line glyphs (24x24, stroke only), matching mockup.html ----
  const cut = (w, h, c) => `M${c},0 H${w} V${h - c} L${w - c},${h} H0 V${c} Z`;   // top-left + bottom-right corners cut
  const G = {
    fire: '<circle cx="12" cy="12" r="6"/><path d="M12 1v5M12 18v5M1 12h5M18 12h5"/><circle cx="12" cy="12" r="1"/>',
    aim: '<circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3.5"/><path d="M12 3v3M12 18v3M3 12h3M18 12h3"/>',
    reload: '<path d="M19 12a7 7 0 1 1-2.1-5"/><path d="M19 3v5h-5"/>',
    jump: '<path d="M5 13l7-7 7 7"/><path d="M5 19l7-7 7 7"/>',
    crouch: '<path d="M5 5l7 7 7-7"/><path d="M4 19h16"/>',
    dodge: '<path d="M4 6l6 6-6 6"/><path d="M12 6l6 6-6 6"/>',
    weapon: '<path d="M3 9h15l2 2v2H11l-1 6H6l1-6H3z"/><path d="M14 13v2"/>',
    heal: '<rect x="4" y="4" width="16" height="16"/><path d="M12 8v8M8 12h8"/>',
    scan: '<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="2.5"/>',
    map: '<path d="M3 6l6-2 6 2 6-2v14l-6 2-6-2-6 2z"/><path d="M9 4v14M15 6v14"/>',
    menu: '<path d="M4 6h16M4 12h16M4 18h10"/>',
    fs: '<path d="M4 9V4h5M15 4h5v5M20 15v5h-5M9 20H4v-5"/>',
    mode: '<path d="M4 8h14l-3-3M20 16H6l3 3"/>',
    up: '<path d="M5 15l7-7 7 7"/>', down: '<path d="M5 9l7 7 7-7"/>',
    left: '<path d="M15 5l-7 7 7 7"/>', right: '<path d="M9 5l7 7-7 7"/>',
    select: '<path d="M4 12l5 5L20 6"/>',
    skip: '<path d="M5 6l6 6-6 6M13 6v12M17 6v12"/>',
    throttle: '<path d="M12 4l7 9H5z"/><path d="M12 13v7"/>',
    brake: '<path d="M12 20l7-9H5z"/><path d="M12 11V4"/>',
    hand: '<rect x="4" y="4" width="16" height="16"/><path d="M9 17V7h4a3 3 0 0 1 0 6H9"/>',
    exit: '<path d="M10 4H4v16h6"/><path d="M14 8l4 4-4 4M18 12H8"/>',
    horn: '<path d="M3 10v4h4l6 4V6L7 10z"/><path d="M16 9a4 4 0 0 1 0 6M19 6a8 8 0 0 1 0 12"/>',
    camera: '<rect x="3" y="7" width="18" height="12"/><circle cx="12" cy="13" r="3.5"/><path d="M8 7l2-3h4l2 3"/>',
    radio: '<rect x="3" y="8" width="18" height="12"/><circle cx="15" cy="14" r="3"/><path d="M6 12h4M6 16h4M7 8l9-5"/>',
    back: '<path d="M10 6l-6 6 6 6"/><path d="M4 12h16"/>',
    interact: '<path d="M8 12V5a1.5 1.5 0 0 1 3 0v6M11 10V4a1.5 1.5 0 0 1 3 0v7M14 10a1.5 1.5 0 0 1 3 0v4c0 4-2 7-6 7s-5-2-7-6l-1-3a1.5 1.5 0 0 1 2.6-1.4L8 13"/>',
  };
  const glyph = n => `<svg class="g" viewBox="0 0 24 24" aria-hidden="true">${G[n] || ''}</svg>`;
  const frame = (c, inner) => `<svg class="f" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true"><path class="o" d="${cut(100, 100, c)}"/>${inner ? '<path class="i" d="M24,8 H92 V70"/>' : ''}</svg>`;

  // ---- layout (mockup.html coordinates: a 1280x709 game frame; scaled by --u) ----
  // anchor r: x/y = centre's distance from the right / bottom edge; c: x = offset from the screen centre, y from bottom.
  // key = the PC key the control sends (documentation only; never shown). kind: b = button (w), p = pedal (w x h), chip = wide chip (h). ctx = contexts it shows in. prompt = only while
  // the game shows an interaction prompt. hold = key / mouse button held while touched; act = special action.
  const F = ['foot'], C = ['combat'], V = ['vehicle'], D = ['dialogue'];
  const CONTROLS = [
    // on foot (weapon holstered)
    {id: 'jump-f', kind: 'b', a: 'r', x: 140, y: 169, w: 82, icon: 'jump', label: 'Jump', key: 'SPC', hold: 'SPACE', ctx: F, primary: 1},
    {id: 'crouch-f', kind: 'b', a: 'r', x: 240, y: 109, w: 56, icon: 'crouch', label: 'Crouch', key: 'C', hold: 'C', ctx: F},
    {id: 'draw-f', kind: 'b', a: 'r', x: 234, y: 221, w: 56, icon: 'weapon', label: 'Draw', key: '1', hold: 'ONE', ctx: F, combat: 1},
    {id: 'dodge-f', kind: 'b', a: 'r', x: 328, y: 143, w: 48, icon: 'dodge', label: 'Dodge', act: 'dodge', ctx: F},
    {id: 'use', kind: 'chip', a: 'r', x: 480, y: 314, h: 40, icon: 'interact', label: 'Interact', key: 'F', hold: 'F', ctx: ['foot', 'combat'], prompt: 1},
    // weapon drawn
    {id: 'fire', kind: 'b', a: 'r', x: 142, y: 179, w: 94, icon: 'fire', label: 'Fire', hold: 'LMB', look: 1, ctx: C, primary: 1, combat: 1},
    {id: 'aim', kind: 'b', a: 'r', x: 254, y: 247, w: 62, icon: 'aim', label: 'Aim', hold: 'RMB', look: 1, ctx: C, combat: 1},
    {id: 'reload', kind: 'b', a: 'r', x: 130, y: 305, w: 52, icon: 'reload', label: 'Reload', key: 'R', hold: 'R', ctx: C, combat: 1},
    {id: 'jump-c', kind: 'b', a: 'r', x: 252, y: 117, w: 52, icon: 'jump', label: 'Jump', key: 'SPC', hold: 'SPACE', ctx: C},
    {id: 'crouch-c', kind: 'b', a: 'r', x: 342, y: 91, w: 46, icon: 'crouch', label: 'Crouch', key: 'C', hold: 'C', ctx: C},
    {id: 'heal', kind: 'b', a: 'r', x: 350, y: 189, w: 46, icon: 'heal', label: 'Heal', key: 'X', hold: 'X', ctx: C},
    {id: 'weapon', kind: 'b', a: 'r', x: 46, y: 253, w: 44, icon: 'weapon', label: 'Weapon', key: '1', hold: 'ONE', ctx: C, combat: 1},
    {id: 'dodge-c', kind: 'b', a: 'r', x: 355, y: 279, w: 44, icon: 'dodge', label: 'Dodge', act: 'dodge', ctx: C},
    // driving: pedals stay left of the game's own key-hint panel on the right edge (Switch Camera / Exit / Radio,
    // about x 1120-1250, y 535-620 of the 1280x709 frame)
    {id: 'gas', kind: 'p', a: 'r', x: 215, y: 239, w: 74, h: 170, icon: 'throttle', label: 'Gas', hold: 'W', ctx: V, primary: 1},
    {id: 'brake', kind: 'p', a: 'r', x: 305, y: 214, w: 74, h: 120, icon: 'brake', label: 'Brake', hold: 'S', ctx: V},
    {id: 'handbrake', kind: 'b', a: 'r', x: 395, y: 179, w: 54, icon: 'hand', label: 'Handbrake', key: 'SPC', hold: 'SPACE', ctx: V},
    {id: 'horn', kind: 'b', a: 'r', x: 390, y: 269, w: 44, icon: 'horn', label: 'Horn', key: 'E', hold: 'E', ctx: V},
    {id: 'exit', kind: 'chip', a: 'r', x: 140, y: 337, h: 34, icon: 'exit', label: 'Exit', key: 'F', hold: 'F', ctx: ['vehicle', 'passenger']},
    // dialogue choices (next to the game's choice list, right of the screen centre)
    {id: 'ch-up', kind: 'b', a: 'c', x: 232, y: 231, w: 50, icon: 'up', key: '↑', hold: 'UP', ctx: D},
    {id: 'ch-down', kind: 'b', a: 'c', x: 232, y: 129, w: 50, icon: 'down', key: '↓', hold: 'DOWN', ctx: D},
    {id: 'ch-select', kind: 'b', a: 'c', x: 335, y: 180, w: 80, icon: 'select', label: 'Select', key: 'F', hold: 'F', ctx: D, primary: 1},
    {id: 'ch-skip', kind: 'chip', a: 'c', x: 470, y: 91, h: 30, icon: 'skip', label: 'Skip', key: 'SPC', hold: 'SPACE', ctx: D},
  ];
  // top strip (top-centre in play, top-left in menus)
  const STRIP = [
    {id: 's-back', icon: 'back', label: 'Back', key: 'ESC', hold: 'ESC', ctx: ['menu']},
    {id: 's-scan', icon: 'scan', label: 'Scan', key: 'TAB', hold: 'TAB', ctx: ['foot', 'combat']},
    {id: 's-cam', icon: 'camera', label: 'Cam', key: 'Q', hold: 'Q', ctx: ['vehicle', 'passenger']},
    {id: 's-radio', icon: 'radio', label: 'Radio', key: 'R', hold: 'R', ctx: ['vehicle', 'passenger']},
    {id: 's-map', icon: 'map', label: 'Map', key: 'M', hold: 'M', ctx: ['foot', 'combat', 'vehicle', 'passenger']},
    {id: 's-menu', icon: 'menu', label: 'Menu', key: 'ESC', hold: 'ESC', ctx: ['foot', 'combat', 'vehicle', 'passenger', 'dialogue']},
    {id: 's-mode', icon: 'mode', act: 'mode', ctx: ['foot', 'combat', 'vehicle', 'passenger', 'dialogue', 'menu'], title: 'Switch game / menu controls'},
    {id: 's-fs', icon: 'fs', act: 'fs', ctx: ['foot', 'combat', 'vehicle', 'passenger', 'dialogue', 'menu'], title: 'Fullscreen'},
  ];
  const STICK_CTX = ['foot', 'combat', 'dialogue', 'vehicle'], LOOK_CTX = ['foot', 'combat', 'dialogue', 'vehicle', 'passenger'];

  function build() {
    const de = document.documentElement;
    de.classList.add('cp-touch');
    const canvas = () => document.getElementById('cp2077-canvas');
    const mod = () => globalThis.cp2077EngineModule;

    // ---- page: no scroll / zoom while playing ----
    const vp = document.querySelector('meta[name=viewport]');
    if (vp) vp.setAttribute('content', 'width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover');
    const stop = e => { if (e.cancelable) e.preventDefault(); };
    for (const t of ['gesturestart', 'gesturechange', 'gestureend']) document.addEventListener(t, stop, {passive: false});
    document.addEventListener('touchmove', e => { if (e.touches.length > 1 || !e.target.closest || !e.target.closest('input,textarea,select')) stop(e); }, {passive: false});
    document.addEventListener('dblclick', stop, {passive: false});
    // A real finger reaching the bare canvas must not become a compat mousedown (= Left Mouse shot, and the canvas
    // handler's requestPointerLock, which iOS lacks). Real mouse input on desktop is unaffected.
    let guarded = null;
    const guardCanvas = () => { const c = canvas(); if (c && c !== guarded) { guarded = c; c.addEventListener('touchstart', stop, {passive: false}); } };
    guardCanvas();

    // ---- synthetic input ----
    let curX = null, curY = null, lastMoveT = 0;   // last synthetic client position (menus) and when it was sent
    const settle = () => Math.max(0, CLICK_DELAY - (performance.now() - lastMoveT));
    const centre = () => { const r = canvas().getBoundingClientRect(); return [r.left + r.width / 2, r.top + r.height / 2]; };
    function mouse(type, cx, cy, mx, my, button, buttons) {
      const c = canvas(), m = mod();
      if (!c || !m) return;
      const ev = new MouseEvent(type, {bubbles: true, cancelable: true, view: window, clientX: cx, clientY: cy, screenX: cx, screenY: cy,
        movementX: mx, movementY: my, button, buttons});
      // Safari may ignore movementX in the init dict; emscripten reads e.movementX.
      Object.defineProperty(ev, 'movementX', {get: () => mx});
      Object.defineProperty(ev, 'movementY', {get: () => my});
      const shadow = type === 'mousedown';
      if (shadow) c.requestPointerLock = () => Promise.resolve();   // no pointer lock on touch (and none from a synthetic click)
      m.cp2077SyntheticPointer = true;
      try { c.dispatchEvent(ev); } finally {
        m.cp2077SyntheticPointer = false;
        if (shadow) delete c.requestPointerLock;
      }
      if (shadow) try { c.focus({preventScroll: true}); } catch (_) {}
    }
    let remX = 0, remY = 0;
    function look(dx, dy) {
      const fx = dx * SENS + remX, fy = dy * SENS + remY;
      const mx = Math.round(fx), my = Math.round(fy);
      remX = fx - mx; remY = fy - my;
      if (!mx && !my) return;
      const [cx, cy] = centre();
      mouse('mousemove', cx, cy, mx, my, 0, held.has('LMB') ? 1 : 0);
    }
    function moveTo(cx, cy, buttons) {
      const mx = curX == null ? 0 : Math.round(cx - curX), my = curY == null ? 0 : Math.round(cy - curY);
      curX = cx; curY = cy; lastMoveT = performance.now();
      mouse('mousemove', cx, cy, mx, my, 0, buttons);
    }
    function mouseButton(down, button) {
      let [cx, cy] = curX == null ? centre() : [curX, curY];
      if (ctx !== 'menu') [cx, cy] = centre();
      mouse(down ? 'mousedown' : 'mouseup', cx, cy, 0, 0, button, down ? (button === 2 ? 2 : 1) : 0);
    }
    const KEY = {
      W: [87, 'w', 'KeyW'], A: [65, 'a', 'KeyA'], S: [83, 's', 'KeyS'], D: [68, 'd', 'KeyD'],
      SHIFT: [16, 'Shift', 'ShiftRight', 2],   // Sprint_Button = IK_RShift (hold); IK_LShift is the sprint toggle
      SPACE: [32, ' ', 'Space'], C: [67, 'c', 'KeyC'], R: [82, 'r', 'KeyR'], F: [70, 'f', 'KeyF'], X: [88, 'x', 'KeyX'],
      E: [69, 'e', 'KeyE'], Q: [81, 'q', 'KeyQ'],
      ONE: [49, '1', 'Digit1'], TAB: [9, 'Tab', 'Tab'], M: [77, 'm', 'KeyM'], ESC: [27, 'Escape', 'Escape'], ENTER: [13, 'Enter', 'Enter'],
      UP: [38, 'ArrowUp', 'ArrowUp'], DOWN: [40, 'ArrowDown', 'ArrowDown'],   // IK_Up/IK_Down: UI_DialogFocus / UI_MoveUp/Down (choices)
    };
    function fireKey(type, name) {
      const [code, key, phys, location] = KEY[name];
      const ev = new KeyboardEvent(type, {key, code: phys, location: location || 0, bubbles: true, cancelable: true, view: window});
      for (const p of ['keyCode', 'which']) Object.defineProperty(ev, p, {get: () => code});
      window.dispatchEvent(ev);
    }
    // Reference-counted holds (stick + buttons may share a key). LMB/RMB are mouse buttons.
    const held = new Map();
    function press(name) {
      const n = (held.get(name) || 0) + 1; held.set(name, n);
      if (n !== 1) return;
      if (name === 'LMB' || name === 'RMB') mouseButton(true, name === 'LMB' ? 0 : 2); else fireKey('keydown', name);
    }
    function release(name) {
      const n = (held.get(name) || 0) - 1;
      if (n > 0) { held.set(name, n); return; }
      if (!held.has(name)) return;
      held.delete(name);
      if (name === 'LMB' || name === 'RMB') mouseButton(false, name === 'LMB' ? 0 : 2); else fireKey('keyup', name);
    }
    function releaseAll() {
      for (const b of buttons) b.cancel();
      for (const name of [...held.keys()]) { held.set(name, 1); release(name); }
      stickEnd(); lookId = null; endMenuTouch();
    }

    // ---- DOM ----
    const root = document.createElement('div');
    root.id = 'cp-touch';
    const keyChip = () => '';   // mobile only: icons + short words, no PC key chips
    const ctrlHtml = c => {
      const pos = `--x:${c.x};--y:${c.y};--w:${c.w || 0};--h:${c.h || c.w || 0}`;
      const cls = `cpt-el cpt-${c.kind} a-${c.a}${c.primary ? ' primary' : ''}`;
      const data = `data-id="${c.id}"`;
      if (c.kind === 'chip') return `<div class="${cls}" ${data} style="${pos}">${frame(22)}${keyChip(c.key)}${glyph(c.icon)}<span class="t">${c.label || ''}</span></div>`;
      if (c.kind === 'p') return `<div class="${cls}" ${data} style="${pos}">${frame(14, c.primary)}${glyph(c.icon)}<span class="t">${c.label || ''}</span></div>`;
      return `<div class="${cls}" ${data} style="${pos}">${frame(18, c.primary)}${glyph(c.icon)}${keyChip(c.key)}${c.label ? `<span class="lbl">${c.label}</span>` : ''}</div>`;
    };
    const stripHtml = c => `<div class="cpt-sc" data-id="${c.id}"${c.title ? ` title="${c.title}"` : ''}>${frame(22)}${keyChip(c.key)}${glyph(c.icon)}${c.label ? `<span class="t">${c.label}</span>` : ''}</div>`;
    const ticks = [0, 45, 90, 135, 180, 225, 270, 315].map(a => `<line x1="0" y1="-44" x2="0" y2="${a % 90 ? -40 : -37}" transform="rotate(${a})"/>`).join('');
    root.innerHTML = `
      <div class="cpt-look" data-zone="look"></div>
      <div class="cpt-stickzone" data-zone="stick">
        <div class="cpt-stick"><svg viewBox="-60 -60 120 120" aria-hidden="true"><circle class="spr" r="56"/><circle class="ring" r="44"/><g class="tick">${ticks}</g><circle class="knob" r="14"/></svg></div>
        <div class="cpt-steer">${frame(20)}${glyph('left')}<div class="mid"></div><div class="knob"></div>${glyph('right')}</div>
      </div>
      <div class="cpt-menu-catch" data-zone="menu"></div>
      ${CONTROLS.map(ctrlHtml).join('')}
      <div class="cpt-strip">${STRIP.map(stripHtml).join('')}</div>
      <div class="cpt-hint"></div>
      <div class="cpt-rotate" data-id="rotate"><div><b>ROTATE YOUR DEVICE</b><span>Cyberpunk 2077 plays in landscape. Tap to dismiss.</span></div></div>`;
    document.body.appendChild(root);
    const $ = s => root.querySelector(s);
    root.addEventListener('contextmenu', stop);
    const specs = new Map([...CONTROLS, ...STRIP, {id: 'rotate', act: 'rotate'}].map(c => [c.id, c]));

    // ---- context: loading / menu / foot / combat / vehicle / passenger / dialogue ----
    let ctx = null, override = null, lastSig = null, lastCombat = -1e9, eng = null, engAt = 0;
    let startTap = false, startTapUsed = false;   // [starttap] see the menu tap handler
    const prompts = () => eng ? eng.prompts > 0 : true;   // engines without the hook: keep INTERACT available
    function readEngine() {
      const m = mod(), f = m && m.cp2077TouchState;
      if (m) {   // tell the engine touch mode is on: it hides its keyboard hint lists (gameUI inputHintManagerController.cpp)
        m.cp2077TouchActive = true;
        if (typeof m.cp2077SetTouchActive === 'function') m.cp2077SetTouchActive(1);
      }
      if (typeof f !== 'function') return;
      let s = null;
      try { s = f(); } catch (_) {}
      if (!s) return;   // mid-write; next poll
      eng = s; engAt = performance.now();
      const c = canvas();
      if (c) { c.dataset.cp2077InputCtx = s.ctx; c.dataset.cp2077InputUi = s.ui; c.dataset.cp2077Prompts = s.prompts + '/' + s.dialog; }
    }
    function update() {
      readEngine();
      const c = canvas();
      const loading = de.classList.contains('cp-loading') || !!document.getElementById('cp-loader');
      // Menu = the game shows a cursor: its own ink cursor (Menu/notification/photo-mode layer) or the OS cursor. The
      // MODE button overrides the detection until the engine's cursor state next changes.
      const sig = c ? (c.dataset.cp2077InkCursor || '-') + '/' + (c.dataset.cp2077Relative || '-') : '';
      if (sig !== lastSig) { lastSig = sig; override = null; }
      const autoMenu = !c || c.dataset.cp2077InkCursor === '1' || c.dataset.cp2077Relative !== '1';
      const menu = override == null ? autoMenu : override;
      if (eng && eng.ctx === 'Aiming') lastCombat = performance.now();
      const next = pickContext({loading, menu, ctx: eng && eng.ctx, prompts: eng ? eng.prompts : 0, dialog: eng ? eng.dialog : 0,
        combat: performance.now() - lastCombat < COMBAT_MS});
      setContext(next, prompts());
      api.engine = eng;
    }
    let promptOn = null;
    const shown = sp => !sp.ctx || (sp.ctx.includes(ctx) && (!sp.prompt || promptOn));
    function setContext(next, prompt) {
      if (next === ctx && prompt === promptOn) return;
      const prev = ctx;
      ctx = next; promptOn = prompt;
      root.dataset.ctx = next;
      for (const el of root.querySelectorAll('.cpt-el,.cpt-sc')) el.classList.toggle('vis', shown(specs.get(el.dataset.id)));
      for (const b of buttons) if (!b.visible()) b.cancel();   // release what just disappeared
      if (next === prev) return;
      if (!STICK_CTX.includes(next) || (prev === 'vehicle') !== (next === 'vehicle')) stickEnd();
      if (!LOOK_CTX.includes(next)) lookId = null;
      if (next !== 'menu') endMenuTouch();
      if (next === 'menu' && !startTapUsed) startTap = true;   // [starttap] first menu after page load (title screen)
      curX = curY = null;
      api.mode = next === 'loading' ? 'loading' : next === 'menu' ? 'menu' : 'game';
      api.ctx = next;
      if (['foot', 'combat', 'vehicle'].includes(next)) showHintOnce();
    }
    const api = globalThis.cp2077Touch = {active: true, mode: null, ctx: null, sens: SENS, engine: null, releaseAll,
      setMode: v => { override = v === 'menu' ? true : v === 'game' ? false : null; update(); },
      combat: on => { lastCombat = on ? performance.now() : -1e9; update(); }};

    // ---- first-use hint (once per browser) ----
    function showHintOnce() {
      try { if (localStorage.getItem('cp2077TouchHint3')) return; localStorage.setItem('cp2077TouchHint3', '1'); } catch (_) { return; }
      const h = $('.cpt-hint');
      h.textContent = 'Left side: move (push past the ring to sprint) · right side: look';
      h.classList.add('vis');
      setTimeout(() => h.classList.remove('vis'), 6000);
    }

    // ---- stick (left; floats to where the thumb lands) / steering in a vehicle ----
    const zone = $('.cpt-stickzone'), stick = $('.cpt-stick'), knob = $('.cpt-stick .knob'), steer = $('.cpt-steer'), steerKnob = $('.cpt-steer .knob');
    let stickId = null, sx = 0, sy = 0, stickKeys = [], stickSprint = false, latch = false, lastStickEnd = 0;
    const steering = () => ctx === 'vehicle';
    const visual = () => steering() ? steer : stick;
    function setStickKeys(keys, sprint) {
      for (const k of stickKeys) if (!keys.includes(k)) release(k);
      for (const k of keys) if (!stickKeys.includes(k)) press(k);
      stickKeys = keys;
      if (sprint !== stickSprint) { stickSprint = sprint; sprint ? press('SHIFT') : release('SHIFT'); }
    }
    function stickEnd() {
      if (stickId == null) return;
      stickId = null; lastStickEnd = performance.now();
      setStickKeys([], false); latch = false;
      for (const v of [stick, steer]) { v.classList.remove('on', 'sprint'); v.style.left = v.style.top = ''; }
      knob.setAttribute('cx', 0); knob.setAttribute('cy', 0); steerKnob.style.left = '';
    }
    zone.addEventListener('pointerdown', e => {
      stop(e); if (stickId != null) return;
      stickId = e.pointerId; try { zone.setPointerCapture(e.pointerId); } catch (_) {}
      const r = zone.getBoundingClientRect(), v = visual();
      sx = e.clientX; sy = e.clientY;
      v.style.left = (e.clientX - r.left) + 'px'; v.style.top = (e.clientY - r.top) + 'px';
      v.classList.add('on');
      latch = !steering() && performance.now() - lastStickEnd < 300;   // double-tap = sprint for this hold
      setStickKeys([], false);
    });
    zone.addEventListener('pointermove', e => {
      if (e.pointerId !== stickId) return; stop(e);
      const dx = e.clientX - sx, dy = e.clientY - sy;
      if (steering()) {
        const half = steer.offsetWidth / 2 - 14 || 100;
        const k = Math.max(-1, Math.min(1, dx / half));
        steerKnob.style.left = (50 + k * 42) + '%';
        setStickKeys(k < -0.2 ? ['A'] : k > 0.2 ? ['D'] : [], false);
        return;
      }
      const R = stick.offsetWidth * 44 / 120 || 47;   // ring radius in px
      const {keys, len} = dirKeys(dx, dy, R);
      const k = Math.min(1, R / (Math.hypot(dx, dy) || 1)) * 120 / (stick.offsetWidth || 128);
      knob.setAttribute('cx', (dx * k).toFixed(1)); knob.setAttribute('cy', (dy * k).toFixed(1));
      const sprint = keys.length > 0 && (latch || len > 1.25);   // push past the outer ring = sprint
      stick.classList.toggle('sprint', sprint);
      setStickKeys(keys, sprint);
    });
    for (const t of ['pointerup', 'pointercancel', 'lostpointercapture']) zone.addEventListener(t, e => { if (e.pointerId === stickId) stickEnd(); });

    // ---- look (right side; FIRE / AIM also look while held) ----
    const lookEl = $('.cpt-look');
    let lookId = null;
    const lastPos = new Map();
    function lookMove(e) {
      const p = lastPos.get(e.pointerId); if (!p) return;
      const evs = e.getCoalescedEvents ? e.getCoalescedEvents() : [];   // full finger path at the display rate
      const last = evs.length ? evs[evs.length - 1] : e;
      look(last.clientX - p[0], last.clientY - p[1]);
      lastPos.set(e.pointerId, [last.clientX, last.clientY]);
    }
    lookEl.addEventListener('pointerdown', e => {
      stop(e); if (lookId != null) return;
      lookId = e.pointerId; lastPos.set(e.pointerId, [e.clientX, e.clientY]);
      try { lookEl.setPointerCapture(e.pointerId); } catch (_) {}
    });
    lookEl.addEventListener('pointermove', e => { if (e.pointerId === lookId) { stop(e); lookMove(e); } });
    for (const t of ['pointerup', 'pointercancel', 'lostpointercapture']) lookEl.addEventListener(t, e => { if (e.pointerId === lookId) { lookId = null; lastPos.delete(e.pointerId); } });

    // ---- buttons ----
    async function dodge() {
      // Keyboard dodge = double-tap a direction (DodgeForward/Right/Back/Left_Button); default back.
      const k = stickKeys.find(x => x !== 'SHIFT') || 'S';
      const wait = ms => new Promise(r => setTimeout(r, ms));
      const wasHeld = held.has(k);
      // ~100 ms steps: the engine samples once per frame, shorter steps can collapse into one frame at phone frame rates
      if (wasHeld) { fireKey('keyup', k); await wait(100); }
      fireKey('keydown', k); await wait(100); fireKey('keyup', k); await wait(100); fireKey('keydown', k); await wait(120);
      if (!held.has(k)) fireKey('keyup', k);
    }
    function fullscreen() {
      const fe = document.fullscreenElement || document.webkitFullscreenElement;
      if (fe) { (document.exitFullscreen || document.webkitExitFullscreen).call(document); return; }
      const el = de, req = el.requestFullscreen || el.webkitRequestFullscreen;
      if (!req) return;
      const p = req.call(el, {navigationUI: 'hide'});
      const lock = () => { try { const q = screen.orientation && screen.orientation.lock && screen.orientation.lock('landscape'); if (q && q.catch) q.catch(() => {}); } catch (_) {} };
      if (p && p.then) p.then(lock, () => {}); else lock();
    }
    if (!(de.requestFullscreen || de.webkitRequestFullscreen)) $('[data-id="s-fs"]').remove();
    const buttons = [];
    root.querySelectorAll('.cpt-el,.cpt-sc,.cpt-rotate').forEach(el => {
      const sp = specs.get(el.dataset.id);
      if (!sp) return;
      let id = null;
      const end = () => {
        if (id == null) return;
        lastPos.delete(id); id = null; el.classList.remove('on');
        if (sp.hold) release(sp.hold);
      };
      buttons.push({cancel: end, visible: () => shown(sp)});
      el.addEventListener('pointerdown', e => {
        stop(e); if (id != null) return;
        id = e.pointerId; try { el.setPointerCapture(e.pointerId); } catch (_) {}
        el.classList.add('on');
        if (sp.look) lastPos.set(e.pointerId, [e.clientX, e.clientY]);
        if (sp.combat) lastCombat = performance.now();
        if (sp.hold) press(sp.hold);
        if (sp.act === 'dodge') dodge();
        else if (sp.act === 'fs') fullscreen();
        else if (sp.act === 'mode') { override = ctx !== 'menu'; update(); }
        else if (sp.act === 'rotate') root.classList.add('cpt-rotate-off');
        if (sp.combat && ctx === 'foot') setTimeout(update, 0);   // DRAW / FIRE switch to the weapon layout
      });
      el.addEventListener('pointermove', e => { if (e.pointerId === id && sp.look) { stop(e); lookMove(e); } });
      for (const t of ['pointerup', 'pointercancel', 'lostpointercapture']) el.addEventListener(t, e => { if (e.pointerId === id) end(); });
    });

    // ---- menus: taps = left clicks at that position, drags = moves with the button held, 2 fingers = wheel ----
    const catcher = $('.cpt-menu-catch');
    const menuPts = new Map();
    let menuId = null, menuDown = false, menuDownTimer = 0, wheelAcc = 0, wheelY = null;
    // [starttap] 2026-10-24: the title screen after loading ("PRESS [Space] TO CONTINUE") takes only the Space key, so
    // phones were stuck there. The first tap in the first menu after the loader sends Space instead of a click.
    // (startTap / startTapUsed are declared with ctx above: setContext uses them)
    function endMenuTouch() {
      clearTimeout(menuDownTimer);
      if (menuId != null && menuDown) mouse('mouseup', curX, curY, 0, 0, 0, 0);
      menuId = null; menuDown = false; menuPts.clear(); wheelY = null;
    }
    catcher.addEventListener('pointerdown', e => {
      stop(e);
      menuPts.set(e.pointerId, [e.clientX, e.clientY]);
      try { catcher.setPointerCapture(e.pointerId); } catch (_) {}
      if (menuPts.size === 2) {   // second finger: switch to scrolling, drop the pending click
        clearTimeout(menuDownTimer);
        if (menuDown) mouse('mouseup', curX, curY, 0, 0, 0, 0);
        menuDown = false; menuId = null;
        wheelY = [...menuPts.values()].reduce((s, p) => s + p[1], 0) / 2; wheelAcc = 0;
        return;
      }
      if (menuPts.size > 2 || menuId != null) return;
      menuId = e.pointerId;
      moveTo(e.clientX, e.clientY, 0);
      // let the cursor reach the tap point for a few frames before pressing (see CLICK_DELAY)
      menuDownTimer = setTimeout(() => { if (menuId === e.pointerId && !menuDown) { menuDown = true; mouse('mousedown', curX, curY, 0, 0, 0, 1); } }, CLICK_DELAY);
    });
    catcher.addEventListener('pointermove', e => {
      if (!menuPts.has(e.pointerId)) return; stop(e);
      menuPts.set(e.pointerId, [e.clientX, e.clientY]);
      if (wheelY != null && menuPts.size >= 2) {
        const y = [...menuPts.values()].reduce((s, p) => s + p[1], 0) / menuPts.size;
        wheelAcc += y - wheelY; wheelY = y;
        const c = canvas();
        while (c && Math.abs(wheelAcc) >= 40) {
          const step = wheelAcc > 0 ? 1 : -1; wheelAcc -= step * 40;
          const [cx, cy] = curX == null ? [e.clientX, e.clientY] : [curX, curY];
          // finger down = content follows the finger = wheel up (deltaY < 0)
          c.dispatchEvent(new WheelEvent('wheel', {bubbles: true, cancelable: true, view: window, clientX: cx, clientY: cy, deltaY: -step * 100, deltaMode: 0}));
        }
        return;
      }
      if (e.pointerId === menuId) moveTo(e.clientX, e.clientY, menuDown ? 1 : 0);
    });
    const menuUp = e => {
      if (!menuPts.has(e.pointerId)) return;
      menuPts.delete(e.pointerId);
      if (menuPts.size === 0) wheelY = null;
      if (e.pointerId !== menuId) return;
      menuId = null;
      clearTimeout(menuDownTimer);
      const x = curX, y = curY;
      if (menuDown) {   // drag end: let the last move land first
        menuDown = false;
        setTimeout(() => mouse('mouseup', x, y, 0, 0, 0, 0), Math.min(settle(), 60));
      } else if (e.type === 'pointerup' && startTap) {
        startTap = false; startTapUsed = true;
        fireKey('keydown', 'SPACE'); setTimeout(() => fireKey('keyup', 'SPACE'), CLICK_HOLD);
      } else if (e.type === 'pointerup') {   // quick tap: press once the cursor has settled, then hold CLICK_HOLD
        setTimeout(() => { mouse('mousedown', x, y, 0, 0, 0, 1); setTimeout(() => mouse('mouseup', x, y, 0, 0, 0, 0), CLICK_HOLD); }, settle());
      }
    };
    for (const t of ['pointerup', 'pointercancel', 'lostpointercapture']) catcher.addEventListener(t, menuUp);

    // ---- watch the engine / page state ----
    const mo = new MutationObserver(update);
    mo.observe(de, {attributes: true, attributeFilter: ['class']});
    mo.observe(document.body, {childList: true});
    const watchCanvas = () => { const c = canvas(); if (c && !c.cp2077TouchWatched) { c.cp2077TouchWatched = 1; mo.observe(c, {attributes: true, attributeFilter: ['data-cp2077-ink-cursor', 'data-cp2077-relative']}); } };
    watchCanvas(); update();
    setInterval(() => { watchCanvas(); guardCanvas(); update(); }, 200);   // engine context poll (cheap: one small struct read)

    window.addEventListener('blur', releaseAll);
    document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
    console.log('CP2077 touch controls active (' + (forced ? '?touch=1' : coarse ? 'pointer: coarse' : 'first touch') + ', sens ' + SENS + ')');
  }

  // start last: build() uses the layout constants above
  if (forced || coarse) start();
  else window.addEventListener('touchstart', start, {once: true, passive: true, capture: true});
})();
