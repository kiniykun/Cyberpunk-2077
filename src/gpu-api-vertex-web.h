#pragma once
#include <webgpu/webgpu.h>
#include <vector>
#include <cstdint>
namespace GpuApi { struct VertexLayoutRef; }
namespace cp2077::web {
struct ConvertedGraphicsShader;
struct GpuVertexBufferLayouts {
    std::vector<WGPUVertexBufferLayout> buffers;
    std::vector<std::vector<WGPUVertexAttribute>> attributes;
    GpuVertexBufferLayouts() = default;
    GpuVertexBufferLayouts(const GpuVertexBufferLayouts&) = delete;
    GpuVertexBufferLayouts& operator=(const GpuVertexBufferLayouts&) = delete;
    GpuVertexBufferLayouts(GpuVertexBufferLayouts&&) = default;
    GpuVertexBufferLayouts& operator=(GpuVertexBufferLayouts&&) = default;
};
// Output owns all attribute storage. Explicit strides are native command state;
// an absent stride array uses the original layout descriptor's own strides.
bool BuildGpuVertexBufferLayouts(const GpuApi::VertexLayoutRef&, const ConvertedGraphicsShader&,
                                std::uint32_t strideCount, const std::uint8_t* strides,
                                GpuVertexBufferLayouts&);
}
