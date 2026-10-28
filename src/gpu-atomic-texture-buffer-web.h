#pragma once
#include "gpu-buffer-web.h"
#include "gpu-command-web.h"
#include <limits>

namespace cp2077::web {
// Exact R32Uint array transport for lowered texture atomics. The caller retains
// the native texture and these buffers through GPU completion, and must encode
// Import -> original draw -> Export in that order on the same command encoder.
// Shader lowering and native draw integration are required before using this.
struct GpuAtomicTextureBuffer {
    WGPUBuffer words=nullptr;
    WGPUBuffer dimensions=nullptr;
    std::uint32_t width=0,height=0,layers=0,rowBytes=0;
    std::uint64_t bytes=0;
};
inline void ReleaseGpuAtomicTextureBuffer(GpuAtomicTextureBuffer& bridge) {
    if(bridge.dimensions)DestroyGpuBuffer(bridge.dimensions);
    if(bridge.words)DestroyGpuBuffer(bridge.words);
    bridge={};
}
inline bool CreateGpuAtomicTextureBuffer(std::uint32_t width,std::uint32_t height,
                                         std::uint32_t layers,GpuAtomicTextureBuffer& bridge) {
    if(bridge.words || bridge.dimensions || !width || !height || !layers)return false;
    const std::uint64_t row=(std::uint64_t(width)*4+255)&~std::uint64_t(255);
    if(row>std::numeric_limits<std::uint32_t>::max())return false;
    if(row>std::numeric_limits<std::uint64_t>::max()/height)return false;
    const auto plane=row*height;
    if(plane>std::numeric_limits<std::uint64_t>::max()/layers)return false;
    // GPU storage addresses are u32 word indices in the original shader bridge.
    const auto bytes=plane*layers;
    if(bytes/4>std::numeric_limits<std::uint32_t>::max())return false;
    bridge.words=CreateGpuBuffer(bytes,WGPUBufferUsage_Storage|WGPUBufferUsage_CopySrc|WGPUBufferUsage_CopyDst,nullptr,0,"cp-atomic-bridge");
    if(!bridge.words)return false;
    const std::uint32_t layout[4]={width,height,layers,static_cast<std::uint32_t>(row/4)};
    bridge.dimensions=CreateGpuImmutableUniformBuffer(sizeof(layout),layout);
    if(!bridge.dimensions){ReleaseGpuAtomicTextureBuffer(bridge);return false;}
    bridge.width=width;bridge.height=height;bridge.layers=layers;
    bridge.rowBytes=static_cast<std::uint32_t>(row);bridge.bytes=bytes;
    return true;
}
inline bool CopyGpuAtomicTextureBuffer(WGPUCommandEncoder encoder,WGPUTexture texture,
    WGPUTextureFormat format,std::uint32_t mip,std::uint32_t firstLayer,
    const GpuAtomicTextureBuffer& bridge,bool exportToTexture) {
    if(!encoder || !texture || format!=WGPUTextureFormat_R32Uint || !bridge.words ||
       !bridge.width || !bridge.height || !bridge.layers || !bridge.rowBytes)return false;
    WGPUTexelCopyTextureInfo image=WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    image.texture=texture;image.mipLevel=mip;image.origin={0,0,firstLayer};image.aspect=WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo buffer=WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    buffer.buffer=bridge.words;buffer.layout.offset=0;
    buffer.layout.bytesPerRow=bridge.rowBytes;buffer.layout.rowsPerImage=bridge.height;
    const WGPUExtent3D extent={bridge.width,bridge.height,bridge.layers};
    return EncodeGpuTextureBufferCopy(encoder,image,buffer,extent,exportToTexture);
}
}
