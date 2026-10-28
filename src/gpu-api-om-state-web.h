#pragma once
#include <webgpu/webgpu.h>
#include <array>
namespace GpuApi { struct OMStateRef; }
struct SOMState;
namespace cp2077::web {
// Copies native state and acquires one engine reference on success.
bool AcquireGpuOMState(const GpuApi::OMStateRef&, SOMState&);
void ShutdownGpuOMStates();
struct GpuOMPipelineState {
    WGPUPrimitiveState primitive = WGPU_PRIMITIVE_STATE_INIT;
    WGPUDepthStencilState depthStencil = WGPU_DEPTH_STENCIL_STATE_INIT;
    std::array<WGPUBlendState, 8> blends{};
    std::array<bool, 8> blendEnabled{};
    std::array<WGPUColorWriteMask, 8> writeMasks{};
    bool hasDepthStencil = false;
    bool alphaToCoverage = false;
    bool logicXor = false;
    // [whitewall] Target 0 blends with Src1 factors: the fragment needs the dual-source variant
    // (@location(0) @blend_src(0/1)). Only set for exactly one target on a device with dual-source-blending.
    bool dualSource = false;
    // [whitewall] Src1 factors on target 0 with more than one color target: WebGPU forbids that pipeline, so the draw
    // path replays it as two single-source draws (see CpDualSourceSplitBlend). Set together with dualSource.
    bool dualSourceMultiTarget = false;
};
// Target formats, sample count and clipping are original command/target state.
// The caller supplies primitive topology separately; it is not an OM property.
bool BuildGpuOMPipelineState(const SOMState&, bool depthClip, unsigned targetCount,
                            WGPUTextureFormat depthFormat, unsigned sampleCount,
                            GpuOMPipelineState&);
}
