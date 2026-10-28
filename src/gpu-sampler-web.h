#pragma once
#include <webgpu/webgpu.h>
namespace cp2077::web {
WGPUSampler CreateGpuSampler(const WGPUSamplerDescriptor& descriptor);
bool ReleaseGpuSampler(WGPUSampler sampler);
}
