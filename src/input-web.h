#pragma once
#include "common/input/include/inputSystem.h"
#include "common/input/include/inputBufferedInputEvent.h"
#include <emscripten/html5.h>
#include <mutex>
#include <vector>

namespace input {
bool BrowserMouseUsesAbsoluteCoordinates();
// [cursorpin] InkSystem reports each frame whether the active ink layer shows its own menu cursor.
// A session (character creation, in-game menus) keeps the OS cursor hidden (gameState_Session), but the
// visible ink cursor must still follow an unlocked browser pointer, and a click must not grab pointer lock.
void SetBrowserInkCursorVisible(bool visible);
class InputSystemWeb final : public IInputSystem {
    RTTI_DECLARE_TYPE(InputSystemWeb);
    RED_BASE_CLASS(IInputSystem);
public:
    InputSystemWeb();
    ~InputSystemWeb() override;
    void Initialize() override;
    void Update(InputCollection& out, Float delta) override;
    void RequestReset() override;
    void RefreshDevices() override;
    bool UsesAbsolutePointer() const;
    Bool IsDeviceConnected(DeviceID id) const override;
    void SetRumble(DeviceID, Float, Float) override;
    void SetBacklightColor(DeviceID, const Color&) override;
    void ResetBacklightColor(DeviceID) override;
    CName GetDeviceName(DeviceID id) const override;
    EInputDevice GetDeviceType(DeviceID id) const override;
    void OnAttach(void* handle) override;
    Bool IsCursorVisible() const override;
    Bool IsCursorForced() const override;
    void ForceCursor(const CName reason, Bool show) override;
    void ShowCursor(const CName reason) override;
    void HideCursor(const CName reason) override;
    void GetCursorPos(ViewportID, Float& x, Float& y) const override;
    void GetCursorPos(ViewportID, Int32& x, Int32& y) const override;
    void SetCursorPos(ViewportID, Int32, Int32) const override;
private:
    static bool Keyboard(int, const EmscriptenKeyboardEvent*, void*);
    static bool Mouse(int, const EmscriptenMouseEvent*, void*);
    static bool Wheel(int, const EmscriptenWheelEvent*, void*);
    static bool Blur(int, const EmscriptenFocusEvent*, void*);
    void Queue(EInputKey, EInputAction, Float, Int32, Int32);
    void ReleaseKeys();
    void ApplyCursor();
    mutable std::mutex m_mutex;
    std::vector<BufferedInputEvent> m_events;
    std::vector<CName> m_hiddenReasons;
    std::vector<CName> m_forcedShowReasons;
    bool m_down[IK_Count]{};
    ViewportID m_viewport{};
    Int32 m_x{}, m_y{};
    bool m_initialized{};
    bool m_pointerLocked{};
};
}
