// Browser implementations of the original GPU device lifecycle interface.
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-device-web.h"
#include "gpu-api-buffer-web.h"
#include "gpu-api-command-web.h"
#include "gpu-api-texture-web.h"
#include "gpu-api-sampler-web.h"
#include "gpu-api-shader-web.h"
#include "gpu-api-om-state-web.h"
#include "common/renderData/include/deviceNotifier.h"
#include <atomic>
#include <mutex>
#include <cstdio>

namespace cp2077::web { bool VerifyOriginalGpuReadback(); }
namespace {
std::atomic<IDeviceNotify*> deviceNotify{nullptr};
std::atomic<bool> initializedOnce{false}, lossNotified{false};
std::mutex paramsMutex;
GpuApi::DeviceParams storedParams;
}

namespace GpuApi {
Bool InitEnv() { return cp2077::web::IsGpuEnvironmentAvailable(); }
Bool InitDevice(const DeviceParams& params) {
    { std::lock_guard<std::mutex> lock(paramsMutex); storedParams = params; }
    if (!cp2077::web::IsGpuDeviceReady()) {
        if(!cp2077::web::ShutdownGpuCommands())return false;
        cp2077::web::ShutdownGpuSwapChains();
        cp2077::web::ShutdownGpuOMStates();
        cp2077::web::ShutdownGpuTextureCompressor();
        cp2077::web::ShutdownGpuShaders();
        cp2077::web::ShutdownGpuBuffers();
        cp2077::web::ShutdownGpuTextures();
        cp2077::web::ShutdownGpuSamplers();
    }
    const bool initialized = cp2077::web::InitializeGpuDevice();
    if (initialized && !cp2077::web::VerifyOriginalGpuReadback()) { ShutDevice(); return false; }
    if (initialized && !cp2077::web::InitializeGpuInternalTextures()) { ShutDevice(); return false; }
    if (initialized) { initializedOnce.store(true); lossNotified.store(false); }
    return initialized;
}
Bool IsInit() { return cp2077::web::IsGpuDeviceReady(); }
const Capabilities& GetCapabilities() {
    // Advertise none of the original DX12-specific capabilities until a real
    // implementation exists. BC texture support is validated separately by WebGPU.
    static const Capabilities capabilities;
    return capabilities;
}
eDeviceCooperativeLevel TestDeviceCooperativeLevel() {
    if (IsInit()) return DEVICECOOPLVL_Operational;
    if (initializedOnce.load() && !lossNotified.exchange(true))
        if (auto* notify = deviceNotify.load()) notify->OnDeviceLost();
    return DEVICECOOPLVL_Removed;
}
void RegisterDeviceNotify(IDeviceNotify* notify) { deviceNotify.store(notify); }
Bool ResetDevice() {
    DeviceParams params;
    { std::lock_guard<std::mutex> lock(paramsMutex); params=storedParams; }
    ShutDevice();
    if (!InitDevice(params)) return false;
    if (auto* notify = deviceNotify.load()) notify->OnDeviceRestored();
    return true;
}
eDeviceType GetDeviceType() {
    // The selected original shader/cache/content profile is PC. This enum
    // selects asset/platform conventions; GPU operations still execute WebGPU.
    return eDeviceType::ePC;
}
Bool HasMinimumRequiredGPU() { return cp2077::web::HasBrowserMinimumGpuFeatures(); }
Uint64 GetGPUTotalMemory() {
    // WebGPU does not expose adapter VRAM. Zero reports unavailable; maxBufferSize
    // and JavaScript heap limits must never be misrepresented as physical VRAM.
    static std::once_flag once;
    std::call_once(once, [] { std::fputs("CP2077 WebGPU: adapter VRAM capacity is unavailable through the browser API\n",stderr); });
    return 0;
}
Int32 GetMonitorCount() {
    Uint32 width=0,height=0;
    return cp2077::web::BrowserScreenResolution(width,height) ? 1 : 0;
}
void GetNativeResolution(Uint32 monitor, Uint32& width, Uint32& height) {
    width=height=0;
    if (monitor==0) cp2077::web::BrowserScreenResolution(width,height);
    // Browser screen CSS dimensions scaled to drawable device pixels. Browsers
    // cannot enumerate private OS display modes or guarantee a zoom-free size.
}
Bool EnumerateDisplayModes(Int32 monitor, Uint32* count, DisplayModeDesc** descriptions) {
    if (!count) return false;
    *count=0;
    if (monitor > 0) return false;
    Uint32 width=0,height=0;
    if (!cp2077::web::BrowserScreenResolution(width,height) || (descriptions && !*descriptions)) return false;
    if (descriptions) {
        (*descriptions)[0].width=width; (*descriptions)[0].height=height;
        (*descriptions)[0].refreshRate={0,1}; // Unknown refresh rate, not guessed 60 Hz.
    }
    *count=1; return true;
}
Int32 GetCurrentDisplayRefreshRate(Int32) { return -1; } // Native unavailable sentinel.
Uint32 GetSupportedHDRModesMask(Int32, Bool) {
    // The current presentation boundary configures SDR RGBA8. Do not advertise
    // HDR merely because a monitor or CSS media query reports HDR hardware.
    return 0;
}
Float GetMonitorMaxLuminanceNits(Int32) {
    // Same ultimate fallback as original IGPUVendorApi, not a measurement.
    return 500.f;
}
void GetMonitorPresentParams(Int32 monitor, GpuWrapApi::eHDRMode mode, MonitorPresentParams& params, Float, Float) {
    params=MonitorPresentParams();
    if (monitor > 0 || mode != GpuWrapApi::HDRMODE_None)
        std::fputs("CP2077 WebGPU: requested monitor/HDR presentation is unavailable\n",stderr);
}
void ShutDevice() { if(!cp2077::web::ShutdownGpuCommands()){std::fputs("CP2077 WebGPU: active draw teardown must run on the application worker\n",stderr);return;} cp2077::web::ShutdownGpuSwapChains(); cp2077::web::ShutdownGpuOMStates(); cp2077::web::ShutdownGpuTextureCompressor(); cp2077::web::ShutdownGpuShaders(); cp2077::web::ShutdownGpuBuffers(); cp2077::web::ShutdownGpuTextures(); cp2077::web::ShutdownGpuSamplers(); cp2077::web::ShutdownGpuDevice(); }
}
