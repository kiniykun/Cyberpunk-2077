// Compile original portable GPU helpers directly, without copying their logic.
// Platform device/render implementations remain in the WebGPU adapters.
#include "common/gpuApi/src/gpuApiAlloc.cpp"
#include "common/gpuApi/src/gpuApiTypes.cpp"
#include "common/gpuApi/src/gpuApiInit.cpp"
#include "common/gpuApi/src/gpuApiTextureUtils.cpp"
#include "common/gpuApi/src/gpuApiImplRenderTarget.inl"
#include "gpu-portable-texture-layout.inc"
