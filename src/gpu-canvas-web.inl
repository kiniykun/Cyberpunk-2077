// Included in the device bridge's anonymous namespace; main-thread ownership.
EM_JS(int, HasPresentationCanvas, (), {
    return typeof document !== 'undefined' &&
        document.querySelector('#cp2077-canvas') instanceof HTMLCanvasElement ? 1 : 0;
});
EM_JS(void, RecordValidatedNativePresentation, (double presentedAt), {
    const stats=Module['cp2077PresentationStats'] ||= {validatedFrames:0,lastPresentedAt:0,intervalsMs:[]};
    if(stats.lastPresentedAt && presentedAt>stats.lastPresentedAt) {
        const interval=presentedAt-stats.lastPresentedAt;
        stats.lastIntervalMs=interval;
        stats.instantaneousFps=1000/interval;
        stats.intervalsMs.push(interval);
        if(stats.intervalsMs.length>120)stats.intervalsMs.shift();
        stats.recentFps=1000*stats.intervalsMs.length/stats.intervalsMs.reduce((a,b)=>a+b,0);
    }
    stats.lastPresentedAt=presentedAt;stats.validatedFrames++;
});
struct CanvasRequest {
    cpEngineTrace::Request trace;unsigned presentId=0;double scheduledAt=0;
    em_proxying_ctx* context = nullptr;
    WGPUDevice device = nullptr;
    WGPUTexture source = nullptr;
    std::uint32_t width = 0, height = 0;
    double presentedAt = 0;
    unsigned remaining = 3;
    bool failed = false, issued = false;
    long rafId = 0, timeoutId = 0; bool scheduledRan = false;
    unsigned presentSeq = 0; int presentSlot = -1; // [nomadperf] async present: source = that private snapshot texture (resolved on main)
};
void CanvasChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                   WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<CanvasRequest*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true; Report("canvas operation failed", message);
    }
    if (--request->remaining) return;
    if (!state.ready.load(std::memory_order_acquire) || request->device != state.device) request->failed = true;
    if (!request->source) {
        if (request->failed) {
            if (state.surface) wgpuSurfaceUnconfigure(state.surface);
            state.canvasWidth = state.canvasHeight = 0;
        } else { state.canvasWidth = request->width; state.canvasHeight = request->height; }
    }
    if(request->source && request->issued && !request->failed) RecordValidatedNativePresentation(request->presentedAt);
    wgpuDeviceRelease(request->device);
    request->trace.Settled(request->failed);
    emscripten_proxy_finish(request->context);
}
void CheckCanvasRequest(CanvasRequest& request) {
    request.trace.Scoped();
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = CanvasChecked; callback.userdata1 = &request;
    for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(request.device, callback);
}
EM_BOOL CopyCanvasFrame(double, void* argument) {
    auto& request = *static_cast<CanvasRequest*>(argument);
    cp2077::web::DrainDeferredGpuWork(); // perf12: present after every asynchronous submission issued so far
    if(request.presentId)cpEngineTrace::Emit(wgtrace::PRESENT,request.trace.frame,request.presentId,request.trace.req,request.trace.scope,request.scheduledAt,wgtrace::now());
    if (state.device != request.device || !state.ready.load(std::memory_order_acquire) || !state.surface) {
        request.failed = true; CheckCanvasRequest(request); return EM_FALSE;
    }
    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    const double cpGetTextureAt=wgtrace::now();
    wgpuSurfaceGetCurrentTexture(state.surface, &current);
    if(request.presentId)cpEngineTrace::Emit(wgtrace::GCT,request.trace.frame,request.presentId,0,0,cpGetTextureAt,wgtrace::now());
    if (!current.texture || (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
                            current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)) {
        request.failed = true;
        if (current.texture) wgpuTextureRelease(current.texture);
        CheckCanvasRequest(request); return EM_FALSE;
    }
    WGPUCommandEncoderDescriptor descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    auto encoder = wgpuDeviceCreateCommandEncoder(request.device, &descriptor);
    if (encoder) {
        WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = request.source; to.texture = current.texture;
        WGPUExtent3D extent{request.width, request.height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
        WGPUCommandBufferDescriptor finish = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
        auto commands = wgpuCommandEncoderFinish(encoder, &finish);
        if (commands && state.queue) {
            const auto cpSubmitId=request.presentId?wgtrace::next_id(wgtrace::ID_SUBMIT):0;
            const double cpSubmitAt=wgtrace::now();wgpuQueueSubmit(state.queue, 1, &commands);request.presentedAt=emscripten_get_now();request.issued = true;
            if(cpSubmitId){cpEngineTrace::Emit(wgtrace::SUBMIT,request.trace.frame,cpSubmitId,0,1,cpSubmitAt,request.presentedAt);cpEngineTrace::Emit(wgtrace::PRESENT_DONE,request.trace.frame,request.presentId,cpSubmitId,0,request.presentedAt,0);}
        }
        else request.failed = true;
        if (commands) wgpuCommandBufferRelease(commands);
        wgpuCommandEncoderRelease(encoder);
    } else request.failed = true;
    wgpuTextureRelease(current.texture);
    // Browser presents after this animation frame. Explicit SurfacePresent
    // aborts in the installed Emdawn library and must never be called here.
    CheckCanvasRequest(request);
    return EM_FALSE;
}
// Hidden/occluded pages never run animation frames; without a fallback the
// render job blocks in the synchronous proxy until the engine watchdog aborts.
// Whichever of rAF or the timeout fires first presents; the other is cancelled.
// [gamefeel] Present pacing (adaptive, phase-locked). A present lands on the first animation frame after the render
// finishes, so a render of 15-35 ms alternated between refresh counts (60 Hz: 1/2/3, 144 Hz: 3..7) = uneven delivery.
// While the engine is in its Running state (gameApp.cpp sets Module.cpPresentPacingRunning), presents are held to a
// whole number of display refreshes: refreshes = ceil((p80 of the render-ready time + 2 ms) / refresh), never fewer
// than the ?fps=N cap allows (default 60 since [skperf] round 17, was 30; ?fps=30 = old default, ?fps=0 disables pacing).
// The cap is only a floor on the refresh count: the adaptive count still holds frames to whole refreshes (60 Hz:
// 16.7/33.3/50 ms), rising at once and stepping down only after a run of fitting frames, so a game that renders in
// 17-33 ms keeps the same steady 30 FPS cadence as before and only frames that really fit run faster. The count rises at once when frames no longer fit
// and drops only after 90 consecutive frames fit the smaller count, so the cadence stays flat instead of flipping.
// rAF timestamps are refresh-exact; the refresh period is the median of back-to-back deferred rAF deltas.
// The hidden-page rAF+200 ms timeout fallback is unchanged (a deferral re-requests rAF only; the timeout stays armed).
namespace cpPace {
double capMs = -1, vsyncMs = 1000.0 / 60.0, nextDeadline = 0, lastPresent = 0, lastDeferredAt = 0, requestArrivedAt = 0;
double ready[32]; unsigned readyCount = 0;
double vsyncSamples[16]; unsigned vsyncCount = 0;
double refreshes = 0; unsigned fitStreak = 0, presents = 0, deferrals = 0;
double Percentile(const double* values, unsigned count, double q) {
    double s[32]; const unsigned n = count < 32 ? count : 32;
    for (unsigned i = 0; i < n; ++i) s[i] = values[i];
    std::sort(s, s + n); return n ? s[static_cast<unsigned>(q * (n - 1) + 0.5)] : 0;
}
double CeilDiv(double ms, double period) { double r = ms / period; double c = static_cast<double>(static_cast<long long>(r)); return c < r ? c + 1 : c; }
}
EM_BOOL CopyCanvasFrameFromAnimation(double time, void* argument) {
    auto& request = *static_cast<CanvasRequest*>(argument);
    if (request.scheduledRan) return EM_FALSE;
    using namespace cpPace;
    if (capMs < 0) {
        const double fps = MAIN_THREAD_EM_ASM_DOUBLE({
            try { const v = new URLSearchParams(location.search).get("fps"); if (!v) return 60; const n = Number(v); return Number.isFinite(n) && n > 0 ? n : 0; } catch (e) { return 60; }
        });
        capMs = fps > 0 ? 1000.0 / fps : 0;
        std::fprintf(stderr, "CP2077 present pacing probe: capFps=%.1f adaptive refresh count (engine Running state only; ?fps=0 disables)\n", fps);
    }
    const bool pacing = capMs > 0 && EM_ASM_INT({ return Module["cpPresentPacingRunning"] ? 1 : 0; });
    if (lastDeferredAt > 0) {  // this callback directly follows a deferral: exactly one refresh later
        const double d = time - lastDeferredAt;
        if (d > 3.0 && d < 45.0) {
            vsyncSamples[vsyncCount++ % 16] = d;
            if (vsyncCount >= 8) {
                const double median = Percentile(vsyncSamples, vsyncCount < 16 ? vsyncCount : 16, 0.5);
                static const double rates[] = {240, 165, 144, 120, 100, 90, 75, 60, 50, 30};
                double best = median, err = 1e9;
                for (double hz : rates) { const double ms = 1000.0 / hz, e = (ms > median ? ms - median : median - ms) / ms; if (e < err) { err = e; best = ms; } }
                vsyncMs = err < 0.08 ? best : median;
            }
        }
        lastDeferredAt = 0;
    }
    if (pacing && lastPresent > 0) {
        // Render-ready time of this frame: request arrival minus the previous present.
        if (requestArrivedAt > lastPresent) ready[readyCount++ % 32] = requestArrivedAt - lastPresent;
        const unsigned n = readyCount < 32 ? readyCount : 32;
        const double minRefreshes = CeilDiv(capMs - 0.25 * vsyncMs, vsyncMs);
        // [cmdlock] The refresh count used to follow p80(ready)+2 ms, so a render averaging just under the cap budget
        // (body type: ~31 ms vs 33.3) kept stepping up to 3 refreshes (20 FPS) and the mix landed at 28.5 FPS. Now the
        // count follows the median render-ready time (+1 ms); a late frame only costs itself one refresh and the
        // deadline keeps its phase. ?pacePct=80 restores the old percentile/margin rule.
        static const int oldRule = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('pacePct') === '80' ? 1 : 0; } catch (e) { return 0; } });
        const double q = oldRule ? 0.8 : 0.5, margin = oldRule ? 2.0 : 1.0;
        double need = n >= 8 ? CeilDiv(Percentile(ready, n, q) + margin, vsyncMs) : minRefreshes;
        if (need < minRefreshes) need = minRefreshes;
        // Never hold a present for a cadence slower than ~20 FPS: slower frames (loading, hitches) present at once.
        const double maxRefreshes = CeilDiv(50.0, vsyncMs);
        if (need > maxRefreshes) need = maxRefreshes;
        if (refreshes <= 0 || need > refreshes) { refreshes = need; fitStreak = 0; }
        else if (need < refreshes) { if (++fitStreak >= 30 && CeilDiv(Percentile(ready, n, oldRule ? 0.95 : 0.8) + margin, vsyncMs) < refreshes) { refreshes -= 1; fitStreak = 0; } }
        else fitStreak = 0;
        // [cmdlock] At the cap, deadlines advance by exactly the cap period instead of a whole number of refreshes, so a
        // 144 Hz display averages 30.0 FPS (alternating 5/4 refreshes) rather than 28.8 (always 5); 60 Hz is unchanged.
        const double period = (!oldRule && refreshes <= minRefreshes) ? capMs : refreshes * vsyncMs;
        if (nextDeadline <= 0 || nextDeadline > time + period) nextDeadline = lastPresent + period;
        if (time < nextDeadline - 0.5 * vsyncMs && time - lastPresent < 50.0) {
            ++deferrals; lastDeferredAt = time;
            request.rafId = emscripten_request_animation_frame(CopyCanvasFrameFromAnimation, &request);
            return EM_FALSE;
        }
        nextDeadline = time > nextDeadline + 0.5 * vsyncMs ? time + period : nextDeadline + period;
        if ((++presents % 300) == 0)
            std::fprintf(stderr, "CP2077 present pacing probe: presents=%u deferrals=%u refreshMs=%.3f refreshes=%.0f periodMs=%.2f readyP50=%.2f readyP80=%.2f readyP95=%.2f\n",
                presents, deferrals, vsyncMs, refreshes, period, Percentile(ready, n, 0.5), Percentile(ready, n, 0.8), Percentile(ready, n, 0.95));
    } else { nextDeadline = 0; refreshes = 0; }
    lastPresent = time;
    request.scheduledRan = true;
    if (request.timeoutId) emscripten_clear_timeout(request.timeoutId);
    return CopyCanvasFrame(time, argument);
}
void CopyCanvasFrameFromTimeout(void* argument) {
    auto& request = *static_cast<CanvasRequest*>(argument);
    if (request.scheduledRan) return;
    request.scheduledRan = true;
    // [gamefeel] keep the present pacer's clock right when a present takes the hidden-page path (else the measured
    // render-ready time includes the 200 ms timeout and the refresh count ratchets up: 4.5 FPS on dfeb7cb8).
    cpPace::lastPresent = emscripten_performance_now(); cpPace::nextDeadline = 0; cpPace::lastDeferredAt = 0;
    if (request.rafId) emscripten_cancel_animation_frame(request.rafId);
    CopyCanvasFrame(emscripten_get_now(), argument);
}
WGPUTexture ResolvePresentSlot(int slot, WGPUDevice device, unsigned seq); // [nomadperf] gpu-device-web.cpp
void BeginCanvasRequest(em_proxying_ctx* context, void* argument) {
    auto& request = *static_cast<CanvasRequest*>(argument);
    request.context = context; request.device = state.device;
    request.trace.Begin();
    if (!state.ready.load(std::memory_order_acquire) || !request.device || !HasPresentationCanvas() ||
        !request.width || !request.height || request.width > state.limits.maxTextureDimension2D ||
        request.height > state.limits.maxTextureDimension2D) {
        request.failed = true; emscripten_proxy_finish(context); return;
    }
    if (request.presentSlot >= 0) {
        // [nomadperf] run the snapshot job (and every list before it) first, then present its texture.
        cp2077::web::DrainDeferredGpuWork();
        request.source = ResolvePresentSlot(request.presentSlot, request.device, request.presentSeq);
        if (!request.source) { request.failed = true; emscripten_proxy_finish(context); return; }
    }
    wgpuDeviceAddRef(request.device);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_Validation);
    if (request.source) {
        if (!state.surface || state.canvasWidth != request.width || state.canvasHeight != request.height ||
            wgpuTextureGetFormat(request.source) != WGPUTextureFormat_RGBA8Unorm ||
            wgpuTextureGetWidth(request.source) != request.width || wgpuTextureGetHeight(request.source) != request.height ||
            wgpuTextureGetSampleCount(request.source) != 1 || wgpuTextureGetDepthOrArrayLayers(request.source) != 1 ||
            !(wgpuTextureGetUsage(request.source) & WGPUTextureUsage_CopySrc)) request.failed = true;
        if (!request.failed) {
            // This API returns the browser's animation-frame request ID, not
            // an EMSCRIPTEN_RESULT. Keep the synchronous proxy context alive
            // until the scheduled copy and its actual GPU validation complete.
            request.trace.CallEnd();
            if(request.trace.req){request.presentId=wgtrace::next_id(wgtrace::ID_PRESENT);request.trace.scope=wgtrace::next_id(wgtrace::ID_SCOPE);request.scheduledAt=wgtrace::now();}
            cpPace::requestArrivedAt = emscripten_performance_now(); // same clock as rAF timestamps (emscripten_get_now is timeOrigin-based in pthread builds)
            cp2077::web::DrainDeferredGpuWork(); // [gamefeel] submit queued GPU work now, so it runs while the present waits
            request.rafId = emscripten_request_animation_frame(CopyCanvasFrameFromAnimation, &request);
            request.timeoutId = emscripten_set_timeout(CopyCanvasFrameFromTimeout, 200.0, &request);
            return;
        }
        request.failed = true;
    } else {
        if (!state.surface) {
            WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
            canvas.selector = {"#cp2077-canvas", WGPU_STRLEN};
            WGPUSurfaceDescriptor descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
            descriptor.nextInChain = &canvas.chain;
            state.surface = wgpuInstanceCreateSurface(state.instance, &descriptor);
        }
        WGPUSurfaceCapabilities capabilities = WGPU_SURFACE_CAPABILITIES_INIT;
        bool rgba = false;
        if (state.surface && wgpuSurfaceGetCapabilities(state.surface, state.adapter, &capabilities) == WGPUStatus_Success) {
            for (std::size_t i = 0; i < capabilities.formatCount; ++i)
                if (capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm) rgba = true;
            // This installed Emdawn implementation populates formats and modes
            // but leaves usages unset (zero). In that case validate the actual
            // configure operation through the device error scopes below. A
            // populated usage mask still has to explicitly support CopyDst.
            if (capabilities.usages && !(capabilities.usages & WGPUTextureUsage_CopyDst)) rgba = false;
        }
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
        if (rgba) {
            WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
            configuration.device = request.device; configuration.format = WGPUTextureFormat_RGBA8Unorm;
            configuration.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
            configuration.width = request.width; configuration.height = request.height;
            configuration.presentMode = WGPUPresentMode_Fifo; configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
            wgpuSurfaceConfigure(state.surface, &configuration); request.issued = true;
        } else {
            request.failed = true;
            std::fputs("CP2077 WebGPU: canvas surface does not support the required RGBA format/usage\n", stderr);
        }
    }
    request.trace.CallEnd();
    CheckCanvasRequest(request);
}
