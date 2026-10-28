#pragma once
#include <webgpu/webgpu.h>
#include <cstdint>
#include <mutex>

namespace cp2077::web {
bool IsGpuEnvironmentAvailable();
// Called on an engine worker; browser callbacks run on the main runtime thread.
bool InitializeGpuDevice();
void ShutdownGpuDevice();
bool IsGpuDeviceReady();
// Atomic lifecycle generation usable from worker cache lookups.
std::uint64_t GpuDeviceGeneration();
bool SupportsGpuDepthClipControl();
// Thread-safe; false before device creation and after loss/shutdown. Covers the
// optional features requested at device creation (see kOptionalGpuFeatures).
bool SupportsGpuFeature(WGPUFeatureName feature);
bool SupportsGpuSubgroups();
bool SupportsGpuTextureFormatsTier1();
// [nobc] Sticky after device creation: the device lacks texture-compression-bc (most phone GPUs) or ?noBC=1.
// BC textures are then created uncompressed and their cooked blocks decoded on the uploading worker.
bool GpuDecodesBC();
// [notier1] Sticky after device creation: no texture-formats-tier1 (or ?noTier1=1); 16-bit unorm/snorm and rg11b10
// textures are then stored as core float formats and their uploads converted on the CPU.
bool GpuLacksTier1();
// Sticky after device creation: rg11b10ufloat-renderable (render targets keep rg11b10ufloat without tier1).
bool GpuRendersRG11B10();
// [notier1] Core format a texture of this format/usage is created with when the device lacks tier1 (gpu-texture-web.cpp).
WGPUTextureFormat Tier1CoreFormat(WGPUTextureFormat format, WGPUTextureUsage usage);
// Borrowed handles. Use only on the browser main runtime thread. Resources and
// command submission will be marshalled to that same owner by the GPU backend.
WGPUDevice MainThreadGpuDevice();
WGPUQueue MainThreadGpuQueue();
bool MainThreadGpuLimits(WGPULimits& limits);
bool BrowserScreenResolution(std::uint32_t& width, std::uint32_t& height);
bool HasBrowserMinimumGpuFeatures();
bool SetGpuCanvasFullscreen(bool fullscreen);
bool IsGpuCanvasFullscreen();
// Worker-facing SDR canvas boundary. Source stays alive through return.
bool ConfigureGpuCanvas(std::uint32_t width, std::uint32_t height);
bool PresentGpuCanvas(WGPUTexture source, std::uint32_t width, std::uint32_t height);
// [opt] Orders deferred-FIFO enqueues between list submitters (CloseAndSubmitCommandLists) and the async present
// snapshot. Never taken by the main thread or while commandMutex is held.
std::mutex& GpuSubmitOrderMutex();
}
