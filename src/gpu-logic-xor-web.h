#pragma once
#include "gpu-command-web.h"
#include <array>
namespace cp2077::web {
struct GpuLogicXorScratch {
    WGPUTexture texture=nullptr;
    WGPUTextureView storage=nullptr,sampled=nullptr;
    std::array<WGPUTextureView,8> layers{};
    WGPUComputePipeline initialize=nullptr;
    WGPURenderPipeline resolve=nullptr;
    WGPUBindGroup resolveGroup=nullptr;
    std::uint32_t width=0,height=0;
    // Device generation of the shared pipelines above (borrowed, never released with the scratch).
    std::uint64_t generation=0;
};
bool CreateGpuLogicXorScratch(std::uint32_t,std::uint32_t,GpuLogicXorScratch&);
void ReleaseGpuLogicXorScratch(GpuLogicXorScratch&);
// perf8: a command list checks a scratch out of a process-wide pool (exclusive until recycled) instead of
// creating the texture, views, pipelines and bind group for every list; recycling returns it (CPU only).
bool AcquireGpuLogicXorScratch(std::uint32_t,std::uint32_t,GpuLogicXorScratch&);
void RecycleGpuLogicXorScratch(GpuLogicXorScratch&);
bool EncodeGpuLogicXorDraw(WGPUCommandEncoder,const GraphicsDraw&,WGPUTextureView destination,
    WGPUTextureView destinationSampled,GpuLogicXorScratch&,WGPUBindGroup& retainedInitializeGroup);
}
