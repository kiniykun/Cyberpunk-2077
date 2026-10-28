#pragma once

// Platform window for the original renderer. The browser canvas is the window;
// rendering, Ink menus and game state remain owned by the original engine.
#include "common/renderData/include/renderViewportWindowInterface.h"
#include <emscripten/html5.h>
#include <emscripten/threading.h>
#include <cstdio>

class ViewportWindowWeb final : public IViewportWindow
{
public:
    ViewportWindowWeb(Uint32 width, Uint32 height) : m_width(width), m_height(height), m_sinks(red::PoolEngine()), m_listeners(red::PoolEngine())
    {
        const int found = MAIN_THREAD_EM_ASM_INT({
            const canvas = document.getElementById('cp2077-canvas');
            if (!canvas) return 0;
            canvas.width = Number($0); canvas.height = Number($1);
            return 1;
        }, width, height);
        if (!found) return;
        const auto resize = emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, this, false, OnResize);
        const auto focus = emscripten_set_focus_callback("#cp2077-canvas", this, false, OnFocus);
        const auto blur = emscripten_set_blur_callback("#cp2077-canvas", this, false, OnFocus);
        m_ready = resize == EMSCRIPTEN_RESULT_SUCCESS && focus == EMSCRIPTEN_RESULT_SUCCESS && blur == EMSCRIPTEN_RESULT_SUCCESS;
        if (!m_ready) std::fprintf(stderr, "CP2077 browser viewport event registration failed: %d %d %d\n", resize, focus, blur);
    }
    ~ViewportWindowWeb() override
    {
        emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, nullptr, false, nullptr);
        emscripten_set_focus_callback("#cp2077-canvas", nullptr, false, nullptr);
        emscripten_set_blur_callback("#cp2077-canvas", nullptr, false, nullptr);
    }
    bool IsReady() const { return m_ready; }
    ViewportID GetViewportID() const override { return reinterpret_cast<ViewportID>(this); }
    void Bind(RenderViewport* viewport) override { m_viewport = viewport; }
    void AttachMessageSink(IViewportWindowMessageSink* sink) override
    {
        // DOM keyboard/mouse events use the existing original input bridge.
        // This is an opaque viewport identity, never a fabricated Windows HWND.
        if (sink && !m_sinks.Exist(sink)) { m_sinks.PushBack(sink); sink->OnAttach(this); }
    }
    void RegisterActionListener(IViewportWindowActionListener* listener) override
    {
        if (listener && !m_listeners.Exist(listener)) m_listeners.PushBack(listener);
    }
    void UnregisterActionListener(IViewportWindowActionListener* listener) override { m_listeners.Remove(listener); }
private:
    // Emscripten dispatches callbacks to the registering game pthread. Snapshots
    // permit a listener to unregister itself while processing its own event.
    static bool OnResize(int, const EmscriptenUiEvent*, void* data)
    {
        auto& self = *static_cast<ViewportWindowWeb*>(data);
        double width = 0, height = 0;
        if (emscripten_get_element_css_size("#cp2077-canvas", &width, &height) != EMSCRIPTEN_RESULT_SUCCESS || width <= 0 || height <= 0) return false;
        self.m_width = static_cast<Uint32>(width); self.m_height = static_cast<Uint32>(height);
        const auto listeners = self.m_listeners;
        for (auto* listener : listeners) { listener->OnResizing(&self); listener->OnSizeChanged(&self, self.m_width, self.m_height); }
        return false;
    }
    static bool OnFocus(int type, const EmscriptenFocusEvent*, void* data)
    {
        auto& self = *static_cast<ViewportWindowWeb*>(data);
        const auto listeners = self.m_listeners;
        for (auto* listener : listeners) listener->OnActivationChanged(&self, type == EMSCRIPTEN_EVENT_FOCUS);
        return false;
    }
    Uint32 m_width = 0, m_height = 0;
    bool m_ready = false;
    RenderViewport* m_viewport = nullptr;
    MessageSinks m_sinks;
    ActionListeners m_listeners;
};
