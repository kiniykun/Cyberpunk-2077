#pragma once
#include <webgpu/webgpu.h>
namespace cp2077::web {
// Native read-only DSV flags override write-enabling state. Tests, comparison
// functions, read masks and stencil reference remain unchanged.
// Direct3D functional specification section 17.13; native DX12 texture.cpp
// creates the read-only DSV with both READ_ONLY_DEPTH and READ_ONLY_STENCIL.
inline void ApplyGpuReadOnlyDepthStencilView(WGPUDepthStencilState& state){
    state.depthWriteEnabled=WGPUOptionalBool_False;
    state.stencilWriteMask=0;
}
}
