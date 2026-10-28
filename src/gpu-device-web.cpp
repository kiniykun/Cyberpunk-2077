#define WGTRACE_IMPLEMENTATION
#include "engine-wgtrace-web.h"
#include "gpu-buffer-web.h"
#include "gpu-texture-web.h"
#include "gpu-compute-web.h"
// Authored WebGPU platform boundary; does not replace the game's UI or renderer.
#include "gpu-device-web.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/html5.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <pthread.h>

namespace {
struct DeviceState {
    WGPUInstance instance = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    WGPUSurface surface = nullptr;
    std::uint32_t canvasWidth = 0, canvasHeight = 0;
    WGPULimits limits = WGPU_LIMITS_INIT;
    std::atomic<bool> ready{false};
    std::atomic<bool> depthClipControl{false};
    // Bit (1 << feature) for each enabled optional core feature (enum < 64).
    std::atomic<std::uint64_t> enabledFeatures{0};
} state;
struct OptionalGpuFeature { WGPUFeatureName name; const char* label; };
// Requested only when the adapter reports them; absence never fails device creation.
constexpr OptionalGpuFeature kOptionalGpuFeatures[] = {
    {WGPUFeatureName_TextureCompressionBC, "texture-compression-bc"},
    {WGPUFeatureName_TextureFormatsTier1, "texture-formats-tier1"},
    {WGPUFeatureName_TextureFormatsTier2, "texture-formats-tier2"},
    {WGPUFeatureName_IndirectFirstInstance, "indirect-first-instance"},
    {WGPUFeatureName_Depth32FloatStencil8, "depth32float-stencil8"},
    {WGPUFeatureName_TimestampQuery, "timestamp-query"},
    {WGPUFeatureName_ClipDistances, "clip-distances"},
    {WGPUFeatureName_Float32Filterable, "float32-filterable"},
    {WGPUFeatureName_Float32Blendable, "float32-blendable"},
    {WGPUFeatureName_DepthClipControl, "depth-clip-control"},
    {WGPUFeatureName_Subgroups, "subgroups"},
    {WGPUFeatureName_RG11B10UfloatRenderable, "rg11b10ufloat-renderable"},
    {WGPUFeatureName_BGRA8UnormStorage, "bgra8unorm-storage"},
    // [whitewall] D3D dual-source blending (glass, vehicle paint, clear coat: SV_Target1 feeds the Src1 factors).
    {WGPUFeatureName_DualSourceBlending, "dual-source-blending"},
};
constexpr std::size_t kOptionalGpuFeatureCount = sizeof(kOptionalGpuFeatures) / sizeof(kOptionalGpuFeatures[0]);
std::uint64_t FeatureBit(WGPUFeatureName feature) {
    const auto value = static_cast<std::uint32_t>(feature);
    return value < 64 ? (std::uint64_t{1} << value) : 0;
}
void LogEnabledGpuCapabilities(WGPUDevice device, const WGPULimits& limits) {
    std::fputs("CP2077 WebGPU: device features:", stderr);
    for (const auto& feature : kOptionalGpuFeatures)
        if (wgpuDeviceHasFeature(device, feature.name)) std::fprintf(stderr, " %s", feature.label);
    std::fputc('\n', stderr);
    std::fprintf(stderr, "CP2077 WebGPU: device limits: maxComputeInvocationsPerWorkgroup=%u maxComputeWorkgroupSize=%ux%ux%u maxComputeWorkgroupStorageSize=%u maxVertexAttributes=%u maxSampledTexturesPerShaderStage=%u\n",
        limits.maxComputeInvocationsPerWorkgroup, limits.maxComputeWorkgroupSizeX, limits.maxComputeWorkgroupSizeY,
        limits.maxComputeWorkgroupSizeZ, limits.maxComputeWorkgroupStorageSize, limits.maxVertexAttributes,
        limits.maxSampledTexturesPerShaderStage);
}
std::atomic<bool> gpuDecodesBC{false}; // [nobc]
std::atomic<bool> gpuLacksTier1{false}; // [notier1]
std::atomic<bool> gpuRendersRG11B10{false}; // [notier1]
std::mutex lifecycleMutex;
std::atomic<std::uint64_t> deviceGeneration{1};
struct Request { em_proxying_ctx* context = nullptr; bool succeeded = false; };
em_proxying_queue* ProxyQueue() {
    static auto* queue = em_proxying_queue_create();
    return queue;
}
void Report(const char* operation, WGPUStringView message) {
    std::fprintf(stderr, "CP2077 WebGPU: %s: ", operation);
    if (message.data) {
        if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
        else std::fwrite(message.data, 1, message.length, stderr);
    }
    std::fputc('\n', stderr);
}
void Release() {
    state.ready.store(false, std::memory_order_release);
    deviceGeneration.fetch_add(1,std::memory_order_acq_rel);
    state.depthClipControl.store(false,std::memory_order_release);
    state.enabledFeatures.store(0,std::memory_order_release);
    if (state.surface) { wgpuSurfaceUnconfigure(state.surface); wgpuSurfaceRelease(state.surface); }
    state.surface = nullptr; state.canvasWidth = state.canvasHeight = 0;
    if (state.queue) wgpuQueueRelease(state.queue);
    if (state.device) { wgpuDeviceDestroy(state.device); cp2077::web::ClearGpuBindGroupCache();cp2077::web::ClearGpuTextureViewCache();cp2077::web::ClearGpuImmutableUniformCache();wgpuDeviceRelease(state.device); }
    if (state.adapter) wgpuAdapterRelease(state.adapter);
    if (state.instance) wgpuInstanceRelease(state.instance);
    state.queue = nullptr; state.device = nullptr;
    state.adapter = nullptr; state.instance = nullptr;
    state.limits = WGPU_LIMITS_INIT;
}
#include "gpu-canvas-web.inl"
void Finish(Request* request, bool succeeded) {
    request->succeeded = succeeded;
    emscripten_proxy_finish(request->context);
}
void DeviceRequested(WGPURequestDeviceStatus status, WGPUDevice device,
                     WGPUStringView message, void* userdata, void*) {
    auto* request = static_cast<Request*>(userdata);
    if (status != WGPURequestDeviceStatus_Success || !device) {
        Report("requestDevice failed", message);
        if (device) wgpuDeviceRelease(device);
        Release(); Finish(request, false); return;
    }
    state.device = device;
    state.queue = wgpuDeviceGetQueue(device);
    if (!state.queue || wgpuDeviceGetLimits(device, &state.limits) != WGPUStatus_Success) {
        std::fputs("CP2077 WebGPU: device queue or limits unavailable\n", stderr);
        Release(); Finish(request, false); return;
    }
    state.depthClipControl.store(wgpuDeviceHasFeature(device,WGPUFeatureName_DepthClipControl),std::memory_order_release);
    std::uint64_t enabled = 0;
    for (const auto& feature : kOptionalGpuFeatures)
        if (wgpuDeviceHasFeature(device, feature.name)) enabled |= FeatureBit(feature.name);
    state.enabledFeatures.store(enabled, std::memory_order_release);
    LogEnabledGpuCapabilities(device, state.limits);
    // [nobc] 2026-10-24 probes: 560 of 560 Android sessions without BC hung in GatheredResources (createTexture with a
    // bc* format throws on the main thread and the creating worker waits forever). Decode instead.
    {   const int forced = EM_ASM_INT({ try { return new URLSearchParams(location.search).get("noBC") === "1" ? 1 : 0; } catch (e) { return 0; } });
        const bool decode = forced || !wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC);
        gpuDecodesBC.store(decode, std::memory_order_release);
        if (decode) std::fprintf(stderr, "CP2077 nobc: BC textures are decoded on the CPU (forced=%d)\n", forced); }
    // [notier1] 2026-10-26: browsers without texture-formats-tier1 (Firefox, older Chrome) threw in createTexture(rgba16unorm)
    // and hung. Those formats are stored as core float formats instead (uploads converted on the CPU).
    {   const int forced = EM_ASM_INT({ try { return new URLSearchParams(location.search).get("noTier1") === "1" ? 1 : 0; } catch (e) { return 0; } });
        const bool lacks = forced || !wgpuDeviceHasFeature(device, WGPUFeatureName_TextureFormatsTier1);
        gpuLacksTier1.store(lacks, std::memory_order_release);
        gpuRendersRG11B10.store(wgpuDeviceHasFeature(device, WGPUFeatureName_RG11B10UfloatRenderable), std::memory_order_release);
        if (lacks) std::fprintf(stderr, "CP2077 notier1: 16-bit unorm/snorm and rg11b10 textures use float formats (forced=%d)\n", forced); }
    cpEngineTrace::InitProbesOnMain(); // perf12: ?probes=1 (main thread, before any render work uses the flag)
    state.ready.store(true, std::memory_order_release);
    Finish(request, true);
}
void AdapterRequested(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                      WGPUStringView message, void* userdata, void*) {
    auto* request = static_cast<Request*>(userdata);
    if (status != WGPURequestAdapterStatus_Success || !adapter) {
        Report("requestAdapter failed", message);
        if (adapter) wgpuAdapterRelease(adapter);
        Release(); Finish(request, false); return;
    }
    state.adapter = adapter;
    WGPUDeviceDescriptor descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    WGPULimits supportedLimits=WGPU_LIMITS_INIT;
    WGPULimits requiredLimits=WGPU_LIMITS_INIT;
    // Original character layouts use input location 16. Negotiate the
    // adapter's real attribute limit instead of silently dropping that input.
    if(wgpuAdapterGetLimits(adapter,&supportedLimits)==WGPUStatus_Success){
        if(supportedLimits.maxVertexAttributes>16)
            requiredLimits.maxVertexAttributes=supportedLimits.maxVertexAttributes;
        // Original multilayer materials exceed the default 16 sampled slots.
        // Negotiate only the adapter's advertised capacity. This requests a
        // limit, not texture allocations; dynamic descriptor lowering still
        // must respect this finite bound and preserve every original sample.
        if(supportedLimits.maxSampledTexturesPerShaderStage>16)
            requiredLimits.maxSampledTexturesPerShaderStage=supportedLimits.maxSampledTexturesPerShaderStage;
        // Converted compute (e.g. LUTGenerate 8x8x8 = 512 invocations) exceeds
        // the 256 default. Request the adapter's own values, which are always valid.
        requiredLimits.maxComputeInvocationsPerWorkgroup=supportedLimits.maxComputeInvocationsPerWorkgroup;
        requiredLimits.maxComputeWorkgroupSizeX=supportedLimits.maxComputeWorkgroupSizeX;
        requiredLimits.maxComputeWorkgroupSizeY=supportedLimits.maxComputeWorkgroupSizeY;
        requiredLimits.maxComputeWorkgroupSizeZ=supportedLimits.maxComputeWorkgroupSizeZ;
        requiredLimits.maxComputeWorkgroupStorageSize=supportedLimits.maxComputeWorkgroupStorageSize;
        // Explicit graphics layouts bind every uniform constant buffer with a dynamic offset; materials can need more than 8.
        requiredLimits.maxDynamicUniformBuffersPerPipelineLayout=supportedLimits.maxDynamicUniformBuffersPerPipelineLayout;
        // [notier1] Without texture-formats-tier1 R8/RG8 storage targets become RGBA8 (8 bytes per sample each), so an
        // 8-target G-buffer pass needs 64 bytes per sample; the default is 32.
        if(supportedLimits.maxColorAttachmentBytesPerSample>32)
            requiredLimits.maxColorAttachmentBytesPerSample=supportedLimits.maxColorAttachmentBytesPerSample;
        // Converted depth downsample (m_downsampleDepth*) writes 6 storage mips in one dispatch; the default is 4.
        if(supportedLimits.maxStorageTexturesPerShaderStage>4)
            requiredLimits.maxStorageTexturesPerShaderStage=supportedLimits.maxStorageTexturesPerShaderStage;
        // [opt] ovfStorage turns overflow uniforms into DYNAMIC read-only storage buffers; without this request the
        // device keeps the WebGPU defaults (4 dynamic storage per layout, 8 storage per stage), so the Nomad pair
        // VS=227417293712834098 (5 overflow buffers) kept its uncacheable partial layout ('ovfStorage not applied
        // room=4'). Adapter values are always valid requests. ?ovfRoom=0 = old (defaults).
        const int ovfRoom = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('ovfRoom') === '0' ? 0 : 1; } catch (e) { return 1; } });
        if(ovfRoom){
            if(supportedLimits.maxDynamicStorageBuffersPerPipelineLayout>4)
                requiredLimits.maxDynamicStorageBuffersPerPipelineLayout=supportedLimits.maxDynamicStorageBuffersPerPipelineLayout;
            if(supportedLimits.maxStorageBuffersPerShaderStage>8)
                requiredLimits.maxStorageBuffersPerShaderStage=supportedLimits.maxStorageBuffersPerShaderStage;
        }
        std::fprintf(stderr,"CP2077 opt switches: ovfRoom=%d adapter maxDynamicStorageBuffersPerPipelineLayout=%u maxStorageBuffersPerShaderStage=%u maxDynamicUniformBuffersPerPipelineLayout=%u\n",
            ovfRoom,supportedLimits.maxDynamicStorageBuffersPerPipelineLayout,supportedLimits.maxStorageBuffersPerShaderStage,
            supportedLimits.maxDynamicUniformBuffersPerPipelineLayout);
        descriptor.requiredLimits=&requiredLimits;
    }
    // Request the game's compressed texture formats only when the actual
    // adapter supports them. Unsupported formats still fail validation.
    WGPUFeatureName features[kOptionalGpuFeatureCount];
    // [whitewall] ?dualsrc=0 = old behaviour (feature not requested -> dual-source OM states stay rejected).
    const bool dualSourceAllowed = MAIN_THREAD_EM_ASM_INT({
        const q = new URLSearchParams(location.search).get('dualsrc');
        return (q === '0' || q === 'false') ? 0 : 1;
    }) != 0;
    std::fprintf(stderr, "CP2077 whitewall switches: dualsrc=%d adapterDualSource=%d\n", int(dualSourceAllowed),
        int(wgpuAdapterHasFeature(adapter, WGPUFeatureName_DualSourceBlending)));
    for (const auto& feature : kOptionalGpuFeatures)
        if (wgpuAdapterHasFeature(adapter, feature.name) &&
            (dualSourceAllowed || feature.name != WGPUFeatureName_DualSourceBlending))
            features[descriptor.requiredFeatureCount++] = feature.name;
    descriptor.requiredFeatures = features;
    descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    descriptor.deviceLostCallbackInfo.callback = [](WGPUDevice const* device,
            WGPUDeviceLostReason reason, WGPUStringView message, void*, void*) {
        // Destroy callbacks from a previous device must not invalidate its successor.
        if (device && *device == state.device) state.ready.store(false, std::memory_order_release);
        if (reason != WGPUDeviceLostReason_Destroyed) Report("device lost", message);
    };
    descriptor.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const*,
            WGPUErrorType, WGPUStringView message, void*, void*) { Report("validation error", message); };
    WGPURequestDeviceCallbackInfo callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = DeviceRequested; callback.userdata1 = request;
    // Limits and features are checked by WebGPU, not assumed from native GPUs.
    wgpuAdapterRequestDevice(adapter, &descriptor, callback);
}
}

namespace {
// [nomadperf] async present snapshot textures (main thread only); see PresentGpuCanvas.
// [opt] valid = the snapshot copy of this seq was actually issued (source passed the same checks as the sync present).
struct PresentSlot { WGPUTexture texture = nullptr; std::uint32_t width = 0, height = 0; WGPUDevice device = nullptr; unsigned seq = 0; bool valid = false; };
std::atomic<unsigned> presentSeqMismatches{0};
PresentSlot presentSlots[2];
WGPUTexture ResolvePresentSlot(int slot, WGPUDevice device, unsigned seq) {
    if (slot < 0 || slot > 1) return nullptr;
    const auto& s = presentSlots[slot];
    if (s.seq != seq) {
        // [opt] never show an out-of-order frame: fail this present (the canvas keeps the previous frame) and let
        // PresentGpuCanvas fall back to the synchronous path for the rest of the session.
        const unsigned n = presentSeqMismatches.fetch_add(1) + 1;
        if (n <= 8 || !(n & (n - 1))) std::fprintf(stderr, "CP2077 nomadperf async present SEQ MISMATCH: slot=%d want=%u have=%u count=%u (falling back to sync present)\n", slot, seq, s.seq, n);
        return nullptr;
    }
    return s.valid && s.texture && s.device == device ? s.texture : nullptr;
}
}
namespace cp2077::web {
EM_JS(int, ReadBrowserScreen, (std::uint32_t* output), {
    if (typeof screen === 'undefined' || !screen.width || !screen.height) return 0;
    const ratio = Number(globalThis.devicePixelRatio || 1);
    // The browser drawable can have a selected render resolution independent
    // of the physical monitor's pixel density. Report that surface to the
    // original engine so its render targets use the requested dimensions.
    const configured = Module['cp2077RenderWidth'] !== undefined && Module['cp2077RenderHeight'] !== undefined;
    let width = configured ? Number(Module['cp2077RenderWidth']) : Math.round(screen.width * ratio);
    let height = configured ? Number(Module['cp2077RenderHeight']) : Math.round(screen.height * ratio);
    // [potato] ?potato=2 -> 0.8, ?potato=3 -> 0.667 of the configured render size (level 1 and default: unchanged);
    // ?potatoScale=0.5..1 overrides. The canvas keeps its CSS size, so the browser compositor upscales it (bilinear,
    // free); input maps through c.width/r.width. The engine renders everything (UI included) at the smaller size.
    try {
        const q = new URLSearchParams(location.search);
        const p = q.get('potato');
        const lv = p === null ? 0 : (p.length === 0 || p === 'true' || p === 'on') ? 1 : Math.max(0, Math.min(3, parseInt(p, 10) || 0));
        let scale = lv >= 3 ? 2 / 3 : lv >= 2 ? 0.8 : 1;
        const o = Number(q.get('potatoScale'));
        if (q.has('potatoScale') && Number.isFinite(o) && o >= 0.5 && o <= 1) scale = o;
        if ((q.get('potatoKeep') || '').split(',').some(k => k.trim() === 'scale')) scale = 1;
        if (scale < 1 && Number.isInteger(width) && Number.isInteger(height)) {
            const w0 = width, h0 = height;
            // [potato r3] multiples of 16 (1280x720 is 80x45 blocks): the engine's half/quarter/
            // eighth-res targets and depth chains must divide evenly, else attachments of one pass differ in size
            // (854x480 broke L3 on cc564815: RejectAttachmentClear / unsupported render-target setup).
            width = Math.max(16, Math.round(w0 * scale / 16) * 16); height = Math.max(16, Math.round(h0 * scale / 16) * 16);
            // [potato] input-web.cpp scales relative mouse deltas and cp2077-jump.js maps its 1280x720-space clicks with these.
            Module['cp2077PotatoScaleX'] = width / w0; Module['cp2077PotatoScaleY'] = height / h0;
            if (!Module['cp2077PotatoScaleLogged']) { Module['cp2077PotatoScaleLogged'] = 1;
                console.log('CP2077 potato render scale: level=' + lv + ' scale=' + scale.toFixed(3) + ' render=' + width + 'x' + height + ' from=' + w0 + 'x' + h0 + ' upscale=browser canvas (CSS)'); }
        }
    } catch (e) {}
    if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width > 65535 || height > 65535) return 0;
    const values = new Uint32Array(wasmMemory.buffer, Number(output), 2);
    values[0] = width; values[1] = height;
    return 1;
});
bool BrowserScreenResolution(std::uint32_t& width, std::uint32_t& height) {
    struct Screen { std::uint32_t values[2]{}; bool available = false; } request;
    auto perform = [](void* argument) { auto& request = *static_cast<Screen*>(argument);
        request.available = ReadBrowserScreen(request.values) != 0; };
    if (emscripten_is_main_runtime_thread()) perform(&request);
    else {
        auto* queue = ProxyQueue();
        if (!queue || !emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), perform, &request)) return false;
    }
    width = request.values[0]; height = request.values[1]; return request.available;
}
bool HasBrowserMinimumGpuFeatures() {
    // The native renderer can ask this through its function table before
    // InitDevice. Obtain a real browser device instead of rejecting that order
    // merely because no adapter has been requested yet.
    if (!IsGpuDeviceReady() && !InitializeGpuDevice()) return false;
    bool supported = false;
    auto perform = [](void* argument) {
        auto device = MainThreadGpuDevice(); WGPULimits limits = WGPU_LIMITS_INIT;
        *static_cast<bool*>(argument) = device && MainThreadGpuLimits(limits) &&
            (wgpuDeviceHasFeature(device, WGPUFeatureName_TextureCompressionBC) || GpuDecodesBC()) && // [nobc]
            limits.maxTextureDimension2D >= 8192 && limits.maxUniformBufferBindingSize >= 65536 &&
            limits.maxSampledTexturesPerShaderStage >= 16 && limits.maxSamplersPerShaderStage >= 16 &&
            limits.maxStorageBuffersPerShaderStage >= 4 && limits.maxComputeInvocationsPerWorkgroup >= 256;
    };
    if (emscripten_is_main_runtime_thread()) perform(&supported);
    else {
        auto* queue = ProxyQueue();
        if (!queue || !emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), perform, &supported)) return false;
    }
    return supported;
}
struct FullscreenRequest { em_proxying_ctx* context = nullptr; bool fullscreen; bool succeeded = false; };
extern "C" EMSCRIPTEN_KEEPALIVE void cp2077_fullscreen_complete(std::uintptr_t address, int success) {
    auto& request = *reinterpret_cast<FullscreenRequest*>(address);
    request.succeeded = success != 0;
    emscripten_proxy_finish(request.context);
}
EM_JS(void, StartBrowserFullscreen, (std::uintptr_t request, int fullscreen), {
    const complete = ok => _cp2077_fullscreen_complete(request, ok ? 1 : 0);
    const canvas = typeof document !== 'undefined' && document.querySelector('#cp2077-canvas');
    if (!canvas) { complete(false); return; }
    const desired = !!fullscreen;
    if ((document.fullscreenElement === canvas) === desired && (!document.fullscreenElement || desired)) { complete(true); return; }
    try {
        let operation;
        if (desired) {
            if (!document.fullscreenEnabled || !canvas.requestFullscreen || !navigator.userActivation || !navigator.userActivation.isActive) { complete(false); return; }
            operation = canvas.requestFullscreen({navigationUI:'hide'});
        } else {
            if (document.fullscreenElement !== canvas || !document.exitFullscreen) { complete(false); return; }
            operation = document.exitFullscreen();
        }
        Promise.resolve(operation).then(() => complete((document.fullscreenElement === canvas) === desired), () => complete(false));
    } catch (_) { complete(false); }
});
EM_JS(int, ReadBrowserFullscreen, (), {
    if (typeof document === 'undefined') return 0;
    const canvas = document.querySelector('#cp2077-canvas');
    return canvas && document.fullscreenElement === canvas ? 1 : 0;
});
bool SetGpuCanvasFullscreen(bool fullscreen) {
    if (emscripten_is_main_runtime_thread()) return false;
    FullscreenRequest request{nullptr,fullscreen};
    auto* queue = ProxyQueue();
    return queue && emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) { auto& request = *static_cast<FullscreenRequest*>(argument);
            request.context = context; StartBrowserFullscreen(reinterpret_cast<std::uintptr_t>(&request), request.fullscreen); }, &request) && request.succeeded;
}
bool IsGpuCanvasFullscreen() {
    bool fullscreen = false;
    auto perform = [](void* argument) { *static_cast<bool*>(argument) = ReadBrowserFullscreen() != 0; };
    if (emscripten_is_main_runtime_thread()) perform(&fullscreen);
    else {
        auto* queue = ProxyQueue();
        if (!queue || !emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), perform, &fullscreen)) return false;
    }
    return fullscreen;
}
EM_JS(int, BrowserHasWebGpu, (), {
    return typeof navigator !== 'undefined' && !!navigator.gpu && globalThis.isSecureContext ? 1 : 0;
});
bool IsGpuEnvironmentAvailable() {
    if (emscripten_is_main_runtime_thread()) return BrowserHasWebGpu() != 0;
    auto* queue = ProxyQueue();
    bool available = false;
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { *static_cast<bool*>(argument) = BrowserHasWebGpu() != 0; },
        &available) && available;
}
bool InitializeGpuDevice() {
    if (emscripten_is_main_runtime_thread()) {
        std::fputs("CP2077 WebGPU: synchronous initialization requires an engine worker\n", stderr);
        return false;
    }
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    if (state.ready.load(std::memory_order_acquire)) return true;
    auto* queue = ProxyQueue();
    if (!queue) return false;
    Request request;
    bool completed = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* request = static_cast<Request*>(argument);
            request->context = context;
            Release();
            state.instance = wgpuCreateInstance(nullptr);
            if (!state.instance) { Finish(request, false); return; }
            WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
            options.powerPreference = WGPUPowerPreference_HighPerformance;
            WGPURequestAdapterCallbackInfo callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = AdapterRequested; callback.userdata1 = request;
            wgpuInstanceRequestAdapter(state.instance, &options, callback);
        }, &request);
    return completed && request.succeeded && IsGpuDeviceReady();
}
void ShutdownGpuDevice() {
    // Lifecycle operations are worker-owned; never block the browser event loop.
    if (emscripten_is_main_runtime_thread()) {
        std::fputs("CP2077 WebGPU: shutdown requires an engine worker\n", stderr);
        return;
    }
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    auto* queue = ProxyQueue();
    if (queue) emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void*) { Release(); }, nullptr);
}
bool IsGpuDeviceReady() { return state.ready.load(std::memory_order_acquire); }
bool GpuDecodesBC() { return gpuDecodesBC.load(std::memory_order_acquire); }
bool GpuLacksTier1() { return gpuLacksTier1.load(std::memory_order_acquire); }
bool GpuRendersRG11B10() { return gpuRendersRG11B10.load(std::memory_order_acquire); }
std::uint64_t GpuDeviceGeneration(){return deviceGeneration.load(std::memory_order_acquire);}
bool SupportsGpuDepthClipControl(){return IsGpuDeviceReady()&&state.depthClipControl.load(std::memory_order_acquire);}
bool SupportsGpuFeature(WGPUFeatureName feature){
    const std::uint64_t bit=FeatureBit(feature);
    return bit&&IsGpuDeviceReady()&&(state.enabledFeatures.load(std::memory_order_acquire)&bit)!=0;
}
bool SupportsGpuSubgroups(){return SupportsGpuFeature(WGPUFeatureName_Subgroups);}
bool SupportsGpuTextureFormatsTier1(){return SupportsGpuFeature(WGPUFeatureName_TextureFormatsTier1);}
bool SupportsGpuDualSourceBlending(){return SupportsGpuFeature(WGPUFeatureName_DualSourceBlending);} // [whitewall]
WGPUDevice MainThreadGpuDevice() {
    return emscripten_is_main_runtime_thread() && IsGpuDeviceReady() ? state.device : nullptr;
}
WGPUQueue MainThreadGpuQueue() {
    return emscripten_is_main_runtime_thread() && IsGpuDeviceReady() ? state.queue : nullptr;
}
bool MainThreadGpuLimits(WGPULimits& limits) {
    if (!emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return false;
    limits = state.limits; return true;
}
static bool ExecuteCanvasRequest(CanvasRequest& request) {
    if (emscripten_is_main_runtime_thread()) return false;
    request.trace.Queue(request.source?"CanvasPresentation":"CanvasConfiguration");
    std::lock_guard<std::mutex> lock(lifecycleMutex);
    auto* queue = ProxyQueue();
    const bool result=queue && emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        BeginCanvasRequest, &request) && request.issued && !request.failed;
    if(request.presentId)cpEngineTrace::Emit(wgtrace::WAIT,request.trace.frame,request.trace.job,wgtrace::W_PRESENT,request.presentId,request.trace.queued,wgtrace::now());
    else request.trace.WaitEnd();
    return result;
}
namespace { void WaitForAsyncPresentIdle(); } // [opt] defined with the async present below
bool ConfigureGpuCanvas(std::uint32_t width, std::uint32_t height) {
    if (!emscripten_is_main_runtime_thread()) WaitForAsyncPresentIdle(); // [opt] a pending old-size present must not race the reconfigure
    CanvasRequest request; request.width = width; request.height = height;
    return ExecuteCanvasRequest(request);
}
// [nomadperf] Pipelined present. The render job used to block in Present for the rest of the main-thread encode of
// its frame, the paced animation frame and the canvas copy's error-scope round trip (~15-20 ms of every ~50 ms
// Nomad-area frame, no engine thread working meanwhile). Now:
//  1. the worker appends a SNAPSHOT job to the deferred GPU FIFO: in submission order (after every list of this frame,
//     before any list of the next) the main thread copies the backbuffer into one of two private present textures;
//  2. a dedicated present thread performs the unchanged synchronous canvas request on that present texture (pacer,
//     rAF copy, validation) while the render job returns and the engine starts the next frame;
//  3. at most one present is in flight: the next Present waits for the previous one first (no run-ahead).
// Costs: one extra full-frame texture copy on the GPU per frame and up to one frame of extra input latency.
// [opt] Hardening (offline review, 2026-10-17):
//  - the snapshot enqueue takes GpuSubmitOrderMutex, so a list submitter that already holds its submission token is
//    in the FIFO before the snapshot (same ordering rule as the submitters among themselves);
//  - the snapshot validates the source exactly like the synchronous present (RGBA8Unorm, size, 1 sample, 1 layer,
//    CopySrc); an invalid source fails that present instead of showing a stale/unwritten slot;
//  - a slot whose seq does not match fails the present and switches the session to the synchronous path;
//  - a failed async present is reported by the NEXT Present's return value (original 'swap-chain canvas submission
//    failed' log + FailBoundGpuCommand), since the present itself completes after Present returns;
//  - ConfigureGpuCanvas waits for the in-flight present first.
// Switch: ?asyncPresent=1 on, ?asyncPresent=0 off; without the parameter the page global
// globalThis.cp2077AsyncPresentDefault === 1 turns it on (so the default can flip in native-browser.html without a
// relink). The wasm default stays OFF until the visual/flicker test passes.
namespace {
struct SnapshotJob { WGPUTexture source; std::uint32_t width, height; unsigned slot, seq; };
void RunPresentSnapshot(void* argument) {
    std::unique_ptr<SnapshotJob> job(static_cast<SnapshotJob*>(argument));
    auto& slot = presentSlots[job->slot];
    slot.valid = false;
    const bool sourceOk = job->source && wgpuTextureGetFormat(job->source) == WGPUTextureFormat_RGBA8Unorm &&
        wgpuTextureGetWidth(job->source) == job->width && wgpuTextureGetHeight(job->source) == job->height &&
        wgpuTextureGetSampleCount(job->source) == 1 && wgpuTextureGetDepthOrArrayLayers(job->source) == 1 &&
        (wgpuTextureGetUsage(job->source) & WGPUTextureUsage_CopySrc);
    if (!sourceOk) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "CP2077 opt async present snapshot rejected the source (format/size/usage) seq=%u\n", job->seq);
    }
    if (sourceOk && state.ready.load(std::memory_order_acquire) && state.device && state.queue) {
        if (!slot.texture || slot.width != job->width || slot.height != job->height || slot.device != state.device) {
            if (slot.texture) { wgpuTextureDestroy(slot.texture); wgpuTextureRelease(slot.texture); slot.texture = nullptr; }
            WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
            d.size = {job->width, job->height, 1}; d.format = WGPUTextureFormat_RGBA8Unorm;
            d.usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst; d.label = {"cp-present-snapshot", WGPU_STRLEN};
            slot.texture = wgpuDeviceCreateTexture(state.device, &d); slot.width = job->width; slot.height = job->height; slot.device = state.device;
        }
        if (slot.texture) {
            WGPUCommandEncoderDescriptor ed = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
            if (auto encoder = wgpuDeviceCreateCommandEncoder(state.device, &ed)) {
                WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                from.texture = job->source; to.texture = slot.texture;
                WGPUExtent3D extent{job->width, job->height, 1};
                wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
                WGPUCommandBufferDescriptor fd = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
                if (auto commands = wgpuCommandEncoderFinish(encoder, &fd)) {
                    wgpuQueueSubmit(state.queue, 1, &commands); wgpuCommandBufferRelease(commands); slot.valid = true;
                }
                wgpuCommandEncoderRelease(encoder);
            }
        }
    }
    slot.seq = job->seq;
    wgpuTextureRelease(job->source);
}
struct PresentWork { unsigned slot = 0, seq = 0; std::uint32_t width = 0, height = 0; };
std::mutex presentMutex; std::condition_variable presentCv;
bool presentPending = false, presentBusy = false, presentThreadStarted = false, presentLastFailed = false; PresentWork presentWork;
std::atomic<unsigned> asyncPresents{0}, asyncPresentFailures{0};
void WaitForAsyncPresentIdle() {
    std::unique_lock<std::mutex> lock(presentMutex);
    if (presentThreadStarted) presentCv.wait(lock, [] { return !presentPending && !presentBusy; });
}
void* PresentThreadMain(void*) {
    for (;;) {
        PresentWork work;
        { std::unique_lock<std::mutex> lock(presentMutex); presentCv.wait(lock, [] { return presentPending; }); work = presentWork; presentPending = false; presentBusy = true; }
        // The snapshot job ran before this request's DrainDeferredGpuWork in BeginCanvasRequest, so the slot texture
        // exists and holds this frame. It is read on the main thread only (inside the request).
        CanvasRequest request; request.width = work.width; request.height = work.height; request.presentSlot = int(work.slot); request.presentSeq = work.seq;
        const bool ok = ExecuteCanvasRequest(request);
        const unsigned n = asyncPresents.fetch_add(1) + 1;
        if (!ok) { const unsigned f = asyncPresentFailures.fetch_add(1) + 1; if (f <= 8 || !(f & (f - 1))) std::fprintf(stderr, "CP2077 nomadperf async present failed: count=%u of %u\n", f, n); }
        if (n <= 2 || (n & (n - 1)) == 0) std::fprintf(stderr, "CP2077 nomadperf async present probe: presents=%u failures=%u seqMismatches=%u\n", n, asyncPresentFailures.load(), presentSeqMismatches.load());
        { std::lock_guard<std::mutex> lock(presentMutex); presentBusy = false; if (!ok) presentLastFailed = true; }
        presentCv.notify_all();
    }
    return nullptr;
}
bool AsyncPresentOn() {
    static std::atomic<int> v{-1};
    int x = v.load(std::memory_order_acquire);
    if (x < 0) {
        x = MAIN_THREAD_EM_ASM_INT({
            try {
                const p = new URLSearchParams(location.search).get('asyncPresent');
                if (p === '1') return 1;
                if (p === '0') return 0;
                return globalThis.cp2077AsyncPresentDefault === 1 ? 1 : 0;
            } catch (e) { return 0; }
        });
        v.store(x, std::memory_order_release);
        std::fprintf(stderr, "CP2077 nomadperf switches: asyncPresent=%d\n", x);
    }
    return x != 0;
}
}
std::mutex& GpuSubmitOrderMutex() { static std::mutex m; return m; }
// [skperf] round 21 presentDepth (default 2, ?presentDepth=1 = old). With depth 1 the render job's Present waited
// until the PREVIOUS present had completely finished, and that present first drains the deferred GPU FIFO, i.e. the
// main thread's encode of the previous frame: tc7 critpath measured a fixed ~9 ms after the last render node in both
// the bar and Badlands. Depth 2 only waits until the present thread has PICKED UP the previous request (one present
// executing, one queued). Still a single present thread (FIFO order) and two snapshot slots: the new snapshot writes
// slot seq&1 while the executing present reads slot (seq-1)&1, and slot (seq-2)&1 is free because that present is
// done (the present thread runs one request at a time). Up to one more frame of latency.
extern "C" void cp2077_skperf_crit_present_wait(double ms); // render-node-cost-web.inl (no-op unless probing)
int PresentDepth() {
    static std::atomic<int> v{-1};
    int x = v.load(std::memory_order_acquire);
    if (x < 0) {
        // tc9: depth 2 gave no gain (presentWaitMs 0.0) -> default back to 1; ?presentDepth=2 keeps the option.
        x = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('presentDepth') === '2' ? 2 : 1; } catch (e) { return 1; } });
        v.store(x, std::memory_order_release);
        std::fprintf(stderr, "CP2077 skperf switches: presentDepth=%d\n", x);
    }
    return x;
}
bool PresentGpuCanvas(WGPUTexture source, std::uint32_t width, std::uint32_t height) {
    if (!source) return false;
    if (emscripten_is_main_runtime_thread() || !AsyncPresentOn() || presentSeqMismatches.load(std::memory_order_relaxed)) {
        if (!emscripten_is_main_runtime_thread()) WaitForAsyncPresentIdle(); // after a seq-mismatch fallback: never two presents at once
        CanvasRequest request; request.source = source; request.width = width; request.height = height;
        return ExecuteCanvasRequest(request);
    }
    static unsigned nextSlot = 0; // render-thread sequence; guarded by presentMutex below
    std::unique_lock<std::mutex> lock(presentMutex);
    if (!presentThreadStarted) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, PresentThreadMain, nullptr) != 0) {
            lock.unlock(); CanvasRequest request; request.source = source; request.width = width; request.height = height;
            return ExecuteCanvasRequest(request);
        }
        pthread_detach(thread); presentThreadStarted = true;
    }
    // Depth 1: wait for the previous present (its slot is then free; the other slot is used now anyway).
    // [skperf] depth 2: wait only until the previous request was picked up (see PresentDepth above).
    const bool depth2 = PresentDepth() >= 2;
    const double waitStart = emscripten_get_now();
    presentCv.wait(lock, [depth2] { return !presentPending && (depth2 || !presentBusy); });
    cp2077_skperf_crit_present_wait(emscripten_get_now() - waitStart);
    const bool previousOk = !presentLastFailed; presentLastFailed = false;
    const unsigned seq = ++nextSlot; const unsigned slot = seq & 1;
    wgpuTextureAddRef(source); // released by the snapshot job on the main thread (atomic refcount in emdawn)
    bool enqueued;
    {
        std::lock_guard<std::mutex> order(GpuSubmitOrderMutex());
        enqueued = EnqueueDeferredGpuWork(RunPresentSnapshot, new SnapshotJob{source, width, height, slot, seq});
    }
    if (!enqueued) { wgpuTextureRelease(source); return false; }
    presentWork = PresentWork{slot, seq, width, height}; presentPending = true;
    lock.unlock(); presentCv.notify_all();
    return previousOk;
}
}
