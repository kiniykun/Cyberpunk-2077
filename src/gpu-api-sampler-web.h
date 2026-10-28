#pragma once
#include <webgpu/webgpu.h>
#include <cstdint>
namespace GpuApi { struct SamplerStateRef; }
namespace cp2077::web {
void ShutdownGpuSamplers();
// Returns one caller-owned original API reference; preset cache owns another.
bool RequestGpuSamplerPreset(std::uint32_t, GpuApi::SamplerStateRef&);
// Success acquires one original reference for a command binding.
bool AcquireGpuSampler(const GpuApi::SamplerStateRef&, WGPUSampler&, WGPUSampler snapshot = nullptr);
bool GetEffectiveGpuSamplerBias(const GpuApi::SamplerStateRef&, WGPUSampler, float&);
// Borrowed until device shutdown. Only for a verified gather-only binding:
// filtering modes do not interpolate the four values returned by gather.
WGPUSampler GetGpuNonFilteringGatherSampler(const GpuApi::SamplerStateRef&, WGPUSampler snapshot);
}
