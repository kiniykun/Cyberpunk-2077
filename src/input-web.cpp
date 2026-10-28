#include "common/input/src/build.h"
#include "input-web.h"
#include <emscripten.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

RTTI_BEGIN_TYPE_IN_NAMESPACE(InputSystemWeb, input);
    RTTI_PARENT_TYPE(input::IInputSystem)
RTTI_END_TYPE();

namespace {
std::mutex lifetimeMutex;
input::InputSystemWeb* activeInput = nullptr;
constexpr input::DeviceID keyboardMouse = 1;
// [gamefeel] Mouse motion is handled on the browser main runtime thread (no proxy to the engine pthread,
// no synchronous main-thread round trip per event) and accumulated; Update() emits one summed
// IK_MouseX/IK_MouseY axis event per engine frame (UserMappingRelative sums per frame anyway).
// The main-thread path never takes a mutex: the engine thread holds m_mutex/lifetimeMutex while it waits
// on synchronous MAIN_THREAD_EM_ASM calls (ApplyCursor, button events), so blocking here could deadlock.
std::atomic<void*> mouseContext{nullptr};
std::atomic<int> pendingMouseX{0}, pendingMouseY{0}, lastMouseX{0}, lastMouseY{0};
std::atomic<bool> pointerLocked{false};
// [cursorpin] Active ink layer shows a menu cursor (set by InkSystem::ProcessInput on the engine thread).
// positionDirty: the mapped pointer position changed since the last engine frame (carried as a zero axis
// event when no nonzero delta goes out, so the absolute ink adaptation still sees the new position).
std::atomic<bool> inkCursorVisible{false}, positionDirty{false};
void CheckCallback(EMSCRIPTEN_RESULT result) {
    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        std::fprintf(stderr, "CP2077: browser input callback registration failed (%d)\n", result);
        std::abort();
    }
}
}
namespace input {
namespace {
// [gamefeel] Browsers report Shift/Ctrl as keyCode 16/17 (IK_Shift/IK_Ctrl), but inputUserMappings.xml binds only the
// sided keys IK_LShift/RShift/LControl/RControl (sprint, crouch, vehicle throttle/brake), as the Win32 raw input path
// delivers. Map by KeyboardEvent.location / code. Alt stays IK_Alt (the mappings bind IK_Alt).
EInputKey MapBrowserKey(const EmscriptenKeyboardEvent* e) {
    const bool right = e->location == 2 || !std::strcmp(e->code, "ShiftRight") || !std::strcmp(e->code, "ControlRight");
    if (e->keyCode == 16) return right ? IK_RShift : IK_LShift;
    if (e->keyCode == 17) return right ? IK_RControl : IK_LControl;
    return static_cast<EInputKey>(e->keyCode);
}
}
bool BrowserMouseUsesAbsoluteCoordinates() { std::lock_guard<std::mutex> guard(lifetimeMutex); return activeInput && activeInput->UsesAbsolutePointer(); }
bool InputSystemWeb::UsesAbsolutePointer() const { return !pointerLocked.load(std::memory_order_relaxed); }
void SetBrowserInkCursorVisible(bool visible) {
    if (inkCursorVisible.exchange(visible, std::memory_order_relaxed) == visible) return;
    std::fprintf(stderr, "CP2077 browser ink cursor: visible=%d locked=%d\n", visible ? 1 : 0, pointerLocked.load(std::memory_order_relaxed) ? 1 : 0);
    // Non-blocking: the engine thread must never wait on the main thread here.
    MAIN_THREAD_ASYNC_EM_ASM({
        const c = document.querySelector('#cp2077-canvas');
        if (c) c.dataset.cp2077InkCursor = $0 ? '1' : '0';
    }, visible ? 1 : 0);
}
InputSystemWeb::InputSystemWeb() = default;
InputSystemWeb::~InputSystemWeb() {
    { std::lock_guard<std::mutex> guard(lifetimeMutex); if (activeInput == this) activeInput = nullptr; }
    mouseContext.store(nullptr);
    if (!m_initialized) return;
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
    emscripten_set_mousedown_callback("#cp2077-canvas", nullptr, true, nullptr);
    emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
    emscripten_set_mousemove_callback_on_thread("#cp2077-canvas", nullptr, true, nullptr, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
    emscripten_set_wheel_callback("#cp2077-canvas", nullptr, true, nullptr);
    emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, true, nullptr);
    MAIN_THREAD_EM_ASM({
        const c = document.querySelector('#cp2077-canvas');
        if (c && c.cp2077MouseHandler) c.removeEventListener('mousedown', c.cp2077MouseHandler);
        if (c && c.cp2077ContextHandler) c.removeEventListener('contextmenu', c.cp2077ContextHandler);
        if (c && c.cp2077LockProbe) { document.removeEventListener('pointerlockchange', c.cp2077LockProbe); document.removeEventListener('pointerlockerror', c.cp2077LockProbe); }
        if (c) { delete c.cp2077MouseHandler; delete c.cp2077ContextHandler; delete c.cp2077LockProbe; }
    });
}
void InputSystemWeb::Initialize() {
    if (m_initialized) return;
    { std::lock_guard<std::mutex> guard(lifetimeMutex);
      if (activeInput && activeInput != this) std::abort();
      activeInput = this; }
    mouseContext.store(this);
    CheckCallback(emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, Keyboard));
    CheckCallback(emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, Keyboard));
    CheckCallback(emscripten_set_mousedown_callback("#cp2077-canvas", this, true, Mouse));
    CheckCallback(emscripten_set_mouseup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, Mouse));
    CheckCallback(emscripten_set_mousemove_callback_on_thread("#cp2077-canvas", this, true, Mouse, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD));
    CheckCallback(emscripten_set_wheel_callback("#cp2077-canvas", this, true, Wheel));
    CheckCallback(emscripten_set_blur_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, true, Blur));
    m_initialized = true;
    MAIN_THREAD_EM_ASM({
        const c = document.querySelector('#cp2077-canvas');
        if (!c) throw new Error('CP2077 input requires the engine canvas');
        c.tabIndex = 0;
        // Click-to-capture like a PC FPS: while the engine hides the cursor (gameplay), a click on the canvas
        // locks the pointer (raw, unaccelerated deltas when the browser supports unadjustedMovement); Esc releases it.
        c.cp2077MouseHandler = () => {
            c.focus();
            // [cursorpin] Not while a session menu shows the ink cursor (character creation, in-game menus): the menu
            // follows the physical pointer there; locking would switch it to relative deltas off the warped ink cursor.
            if (c.dataset.cp2077Relative === '1' && c.dataset.cp2077InkCursor !== '1' && document.pointerLockElement !== c) {
                const plain = () => { const q = c.requestPointerLock(); if (q && q.catch) q.catch(e => console.warn('CP2077 pointer lock:', e && e.name, e && e.message, 'hasFocus=' + document.hasFocus())); };
                let p;
                try { p = c.requestPointerLock({unadjustedMovement: true}); } catch (e) { p = null; }
                if (p && p.catch) p.catch(e => { if (e && e.name === 'NotSupportedError') plain(); else console.warn('CP2077 pointer lock:', e && e.name, e && e.message, 'hasFocus=' + document.hasFocus()); });
                else if (!p) plain();
            }
        };
        c.cp2077LockProbe = e => console.log('CP2077 pointer lock probe: event=' + e.type + ' locked=' + (document.pointerLockElement === c ? 1 : 0) +
            ' relative=' + (c.dataset.cp2077Relative || '-') + ' hasFocus=' + (document.hasFocus() ? 1 : 0) + ' t=' + performance.now().toFixed(0));
        document.addEventListener('pointerlockchange', c.cp2077LockProbe);
        document.addEventListener('pointerlockerror', c.cp2077LockProbe);
        c.cp2077ContextHandler = e => e.preventDefault();
        c.addEventListener('mousedown', c.cp2077MouseHandler);
        c.addEventListener('contextmenu', c.cp2077ContextHandler);
    });
}
void InputSystemWeb::Queue(EInputKey key, EInputAction action, Float value, Int32 x, Int32 y) {
    if (key <= IK_None || key >= IK_Count) return;
    std::lock_guard<std::mutex> guard(m_mutex);
    if (action == IACT_Press || action == IACT_Release) {
        const bool down = action == IACT_Press;
        if (m_down[key] == down) return;
        m_down[key] = down;
    }
    m_x = x; m_y = y;
    m_events.emplace_back(key, action, value, x, y, m_viewport, keyboardMouse);
}
bool InputSystemWeb::Keyboard(int type, const EmscriptenKeyboardEvent* event, void* context) {
    std::lock_guard<std::mutex> guard(lifetimeMutex);
    if (!activeInput || activeInput != context || event->repeat) return false;
    if (!event->keyCode || event->keyCode >= 256) return false;
    Int32 x, y; activeInput->GetCursorPos(0, x, y);
    activeInput->Queue(MapBrowserKey(event), type == EMSCRIPTEN_EVENT_KEYDOWN ? IACT_Press : IACT_Release, 0, x, y);
    return true;
}
bool InputSystemWeb::Mouse(int type, const EmscriptenMouseEvent* event, void* context) {
    // mousemove: browser main runtime thread (inline JS, no locks). mousedown/up: engine thread (proxied).
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE ? mouseContext.load() != context : false) return false;
    // Button events arrive on the engine thread; they reuse the last pointer position/lock state instead of a
    // synchronous main-thread round trip per click. mousemove runs on the main thread, where this JS is inline.
    // [potato] [2],[3] = relative movement in engine pixels (scaled by the potato render scale when one is active).
    double position[4]{double(lastMouseX.load(std::memory_order_relaxed)),double(lastMouseY.load(std::memory_order_relaxed)),double(event->movementX),double(event->movementY)};
    int pointerState=pointerLocked.load(std::memory_order_relaxed)?1:0;
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) pointerState=MAIN_THREAD_EM_ASM_INT({
        const c=Module.cp2077InputCanvas&&Module.cp2077InputCanvas.isConnected?Module.cp2077InputCanvas:(Module.cp2077InputCanvas=document.querySelector('#cp2077-canvas'));
        const r=c.getBoundingClientRect();
        const at=Number($0)/8;
        HEAPF64[at]=(Number($1)-r.left)*c.width/r.width;
        HEAPF64[at+1]=(Number($2)-r.top)*c.height/r.height;
        // [potato] Absolute positions above already map CSS -> canvas pixels. Relative deltas (camera, and the original
        // menus' relative cursor) were raw CSS pixels; with a potato render scale (canvas smaller than its unscaled
        // render size) scale them by the same factor (remainder carried) so cursor/camera keep the default feel and the
        // menu cursor stays under the pointer. No potato scale (default): untouched raw deltas.
        const psx=Module['cp2077PotatoScaleX'];
        const psy=Module['cp2077PotatoScaleY'];
        if(psx&&psy){
          const fx=Number($3)*psx+(Module['cp2077PotatoRemX']||0);
          const fy=Number($4)*psy+(Module['cp2077PotatoRemY']||0);
          HEAPF64[at+2]=Math.round(fx);
          HEAPF64[at+3]=Math.round(fy);
          Module['cp2077PotatoRemX']=fx-HEAPF64[at+2];
          Module['cp2077PotatoRemY']=fy-HEAPF64[at+3];
        }
        return (document.pointerLockElement===c?1:0)|(c.dataset.cp2077Relative==='1'?2:0)|(Module.cp2077SyntheticPointer?4:0);
    },position,event->clientX,event->clientY,event->movementX,event->movementY);
    const int locked=pointerState&1;
    const Int32 x=static_cast<Int32>(position[0]),y=static_cast<Int32>(position[1]);
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) pointerLocked.store(locked!=0, std::memory_order_relaxed);
    static std::atomic<unsigned> reports{0};const unsigned report=reports++;if(report<64||(report&(report-1))==0)std::fprintf(stderr,"CP2077 browser pointer probe: n=%u type=%d x=%d y=%d dx=%d dy=%d locked=%d relative=%d\n",report,type,x,y,event->movementX,event->movementY,locked,(pointerState>>1)&1);
    if (type == EMSCRIPTEN_EVENT_MOUSEMOVE) {
        // Accumulate; Update() emits the per-frame sum on the engine thread.
        if (lastMouseX.exchange(x, std::memory_order_relaxed) != x) positionDirty.store(true, std::memory_order_relaxed);
        if (lastMouseY.exchange(y, std::memory_order_relaxed) != y) positionDirty.store(true, std::memory_order_relaxed);
        // [cursorpin] Log relative/locked/ink-cursor mode changes (the counted probe above goes quiet by the menus).
        static std::atomic<int> lastMode{-1};
        const int mode = (pointerState & 7) | (inkCursorVisible.load(std::memory_order_relaxed) ? 8 : 0);
        if (lastMode.exchange(mode, std::memory_order_relaxed) != mode)
            std::fprintf(stderr, "CP2077 browser pointer mode: locked=%d relative=%d synthetic=%d inkCursor=%d x=%d y=%d\n", mode & 1, (mode >> 1) & 1, (mode >> 2) & 1, (mode >> 3) & 1, x, y);
        // Gameplay (engine hid the cursor) without pointer lock: an unlocked OS cursor merely passing over the canvas
        // must not turn the camera (it did: stray moves pitched the view to the floor). Click the canvas to capture.
        // [saves] Except synthetic test-driver moves (cp2077-jump.js sets Module.cp2077SyntheticPointer while it
        // dispatches): they cannot take pointer lock (needs a user gesture) and the original menus also run in
        // relative mode, so dropping them froze the menu cursor and broke ?jump after the first click.
        // [cursorpin] A session keeps the OS cursor hidden (relative) in its menus too; a visible ink cursor there follows
        // the unlocked pointer (InkSystem absolute adaptation), so only drop when no ink cursor is shown.
        if ((pointerState & 2) && !locked && !(pointerState & 4) && !inkCursorVisible.load(std::memory_order_relaxed)) return true;
        pendingMouseX.fetch_add(static_cast<int>(position[2]), std::memory_order_relaxed);
        pendingMouseY.fetch_add(static_cast<int>(position[3]), std::memory_order_relaxed);
        return true;
    }
    std::lock_guard<std::mutex> guard(lifetimeMutex);
    if (!activeInput || activeInput != context) return false;
    const EInputKey buttons[] = {IK_LeftMouse, IK_MiddleMouse, IK_RightMouse, IK_Mouse4, IK_Mouse5};
    if (event->button < 0 || event->button >= 5) return false;
    activeInput->Queue(buttons[event->button], type == EMSCRIPTEN_EVENT_MOUSEDOWN ? IACT_Press : IACT_Release, 0, x, y);
    return true;
}
bool InputSystemWeb::Wheel(int, const EmscriptenWheelEvent* event, void* context) {
    std::lock_guard<std::mutex> guard(lifetimeMutex);
    if (!activeInput || activeInput != context) return false;
    // Windows represents one wheel notch as +/-1. Pixel-mode scaling is provisional.
    const Float unit = event->deltaMode == 1 ? 3.0f : (event->deltaMode == 2 ? 1.0f : 100.0f);
    activeInput->Queue(IK_MouseZ, IACT_Axis, static_cast<Float>(-event->deltaY) / unit, event->mouse.targetX, event->mouse.targetY);
    return true;
}
bool InputSystemWeb::Blur(int, const EmscriptenFocusEvent*, void* context) {
    std::lock_guard<std::mutex> guard(lifetimeMutex);
    if (activeInput && activeInput == context) activeInput->RequestReset();
    return false;
}
void InputSystemWeb::ReleaseKeys() {
    for (int key = 1; key < IK_Count; ++key) if (m_down[key]) {
        m_events.emplace_back(static_cast<EInputKey>(key), IACT_Release, 0, m_x, m_y, m_viewport, keyboardMouse);
        m_down[key] = false;
    }
}
void InputSystemWeb::RequestReset() { std::lock_guard<std::mutex> guard(m_mutex); ReleaseKeys(); }
void InputSystemWeb::Update(InputCollection& out, Float) {
    std::lock_guard<std::mutex> guard(m_mutex);
    const int dx = pendingMouseX.exchange(0), dy = pendingMouseY.exchange(0);
    if (IsDisableEventsForFunctionalTests()) { m_events.clear(); return; }
    for (const auto& event : m_events) out.PushBack(event);
    m_events.clear();
    // One summed relative axis event per frame; zero deltas are not reported (as the Win32 raw input path).
    const bool moved = positionDirty.exchange(false, std::memory_order_relaxed);
    // [cursorpin] A position change with zero browser movementX/Y (seen from synthetic pointers) still reaches the
    // visible, unlocked ink menu cursor as one zero-valued axis event (no camera/rotation effect: value 0).
    const bool carry = moved && !dx && !dy && inkCursorVisible.load(std::memory_order_relaxed) && !pointerLocked.load(std::memory_order_relaxed);
    if (dx || dy || carry) { m_x = lastMouseX.load(std::memory_order_relaxed); m_y = lastMouseY.load(std::memory_order_relaxed); }
    if (dx || carry) out.PushBack(BufferedInputEvent(IK_MouseX, IACT_Axis, static_cast<Float>(dx), m_x, m_y, m_viewport, keyboardMouse));
    if (dy) out.PushBack(BufferedInputEvent(IK_MouseY, IACT_Axis, static_cast<Float>(dy), m_x, m_y, m_viewport, keyboardMouse));
    UpdateKeyStates(out);
}
void InputSystemWeb::RefreshDevices() {}
Bool InputSystemWeb::IsDeviceConnected(DeviceID id) const { return id == keyboardMouse && m_initialized; }
void InputSystemWeb::SetRumble(DeviceID, Float, Float) { std::fputs("CP2077: keyboard/mouse device has no rumble motors.\n", stderr); }
void InputSystemWeb::SetBacklightColor(DeviceID, const Color&) { std::fputs("CP2077: keyboard/mouse backlight control is unavailable.\n", stderr); }
void InputSystemWeb::ResetBacklightColor(DeviceID id) { SetBacklightColor(id, Color{}); }
CName InputSystemWeb::GetDeviceName(DeviceID id) const { return id == keyboardMouse ? RED_NAME_CONSTEXPR("BrowserKeyboardMouse") : CName::NONE(); }
EInputDevice InputSystemWeb::GetDeviceType(DeviceID id) const { return id == keyboardMouse ? KBD_MOUSE : INVALID; }
void InputSystemWeb::OnAttach(void* handle) { std::lock_guard<std::mutex> guard(m_mutex); m_viewport = reinterpret_cast<uintptr_t>(handle); }
Bool InputSystemWeb::IsCursorVisible() const { std::lock_guard<std::mutex> guard(m_mutex); return m_hiddenReasons.empty() || !m_forcedShowReasons.empty(); }
Bool InputSystemWeb::IsCursorForced() const { std::lock_guard<std::mutex> guard(m_mutex); return !m_forcedShowReasons.empty(); }
void InputSystemWeb::ForceCursor(const CName reason, Bool show) {
    std::lock_guard<std::mutex> guard(m_mutex);
    if (show) {
        if (std::find(m_forcedShowReasons.begin(), m_forcedShowReasons.end(), reason) == m_forcedShowReasons.end()) m_forcedShowReasons.push_back(reason);
    } else m_forcedShowReasons.erase(std::remove(m_forcedShowReasons.begin(), m_forcedShowReasons.end(), reason), m_forcedShowReasons.end());
    ApplyCursor();
}
void InputSystemWeb::ApplyCursor() {
    MAIN_THREAD_EM_ASM({
        const c = document.querySelector('#cp2077-canvas');
        if (c) { c.style.cursor = $0 ? 'default' : 'none'; c.dataset.cp2077Relative = $0 ? '0' : '1'; }
        if ($0 && document.pointerLockElement === c) document.exitPointerLock();
    }, m_hiddenReasons.empty() || !m_forcedShowReasons.empty());
}
void InputSystemWeb::ShowCursor(const CName reason) { std::lock_guard<std::mutex> guard(m_mutex); m_hiddenReasons.erase(std::remove(m_hiddenReasons.begin(), m_hiddenReasons.end(), reason), m_hiddenReasons.end()); ApplyCursor(); }
void InputSystemWeb::HideCursor(const CName reason) { std::lock_guard<std::mutex> guard(m_mutex); if (std::find(m_hiddenReasons.begin(), m_hiddenReasons.end(), reason) == m_hiddenReasons.end()) m_hiddenReasons.push_back(reason); ApplyCursor(); }
void InputSystemWeb::GetCursorPos(ViewportID, Float& x, Float& y) const { std::lock_guard<std::mutex> guard(m_mutex); x = m_x; y = m_y; }
void InputSystemWeb::GetCursorPos(ViewportID, Int32& x, Int32& y) const { std::lock_guard<std::mutex> guard(m_mutex); x = m_x; y = m_y; }
void InputSystemWeb::SetCursorPos(ViewportID, Int32, Int32) const { std::fputs("CP2077: browser cursor warping is unavailable.\n", stderr); }
}
