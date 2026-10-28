#pragma once
#include <webgpu/webgpu.h>
#include <cstdint>
namespace GpuApi { struct TextureRef; struct TextureDesc; }
namespace cp2077::web {
void ShutdownGpuTextures();
bool InitializeGpuInternalTextures();
void ShutdownGpuTextureCompressor();
WGPUTextureFormat TranslateGpuTextureFormat(std::uint32_t nativeFormat);
void ShutdownGpuSwapChains();
// Retain an existing original texture without allocating another view.
bool RetainGpuTexture(const GpuApi::TextureRef&);
// [skperf] round 18 texRetain: RetainGpuTexture + the texture's descriptor under one lock. The descriptor stays valid
// while the caller holds the reference it got here (the entry is only destroyed at reference count zero).
const GpuApi::TextureDesc* RetainGpuTextureDesc(const GpuApi::TextureRef&);
// Success retains the original reference; caller releases it after GPU completion.
bool AcquireGpuTextureForCopy(const GpuApi::TextureRef&,WGPUTexture&);
// Resolves a live native indirection descriptor to its actual sampled view.
// Success owns the view and one texture reference, like direct acquisition.
bool AcquireIndirectGpuTextureView(std::uint16_t, GpuApi::TextureRef&, WGPUTextureView&);
// Success retains one original texture reference and returns an owned view.
// The command/binding owner must release the view and the original reference.
bool AcquireSampledGpuTextureView(const GpuApi::TextureRef&, WGPUTextureView&, bool stencil = false,
    std::uint32_t mip = UINT32_MAX, bool mipGeneration = false, std::uint32_t slice = UINT32_MAX);
bool AcquireStorageGpuTextureView(const GpuApi::TextureRef&, std::uint32_t mip, WGPUTextureView&);
// Format of the view AcquireStorageGpuTextureView creates for this live texture (false if unknown).
bool GetGpuStorageTextureFormat(const GpuApi::TextureRef&, WGPUTextureFormat&);
// Selects one physical array layer and mip for a native render attachment.
// Success owns a view and retains one original texture reference.
bool AcquireRenderGpuTextureView(const GpuApi::TextureRef&, std::uint32_t mip,
    std::uint32_t layer, bool depth, WGPUTextureView&, WGPUTextureFormat* format = nullptr,
    bool nativeBaseFormat = false);
}
