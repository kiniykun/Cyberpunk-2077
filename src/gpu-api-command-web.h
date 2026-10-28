#pragma once
#include <webgpu/webgpu.h>
#include <cstdint>
#include <cstddef>
namespace GpuApi { struct BufferRef; struct ShaderRef; struct SamplerStateRef; struct TextureRef; }
namespace cp2077::web {
struct ComputeBindGroup;
struct ConvertedGraphicsShader;
struct GpuTextureCopy;
enum class GraphicsShaderStage;
// Success transfers each bind-group handle to the bound command list.
bool RecordGpuComputeDispatch(const ComputeBindGroup*, std::size_t count,
                               std::uint32_t x, std::uint32_t y, std::uint32_t z);
bool ShutdownGpuCommands();
// Success transfers one acquired ShaderRef to the bound command state.
bool SetBoundGpuComputeShader(const GpuApi::ShaderRef&, WGPUComputePipeline);
// Transfers an acquired original shader reference into the selected graphics stage.
bool SetBoundGpuGraphicsShader(const GpuApi::ShaderRef&, GraphicsShaderStage,
                              WGPUShaderModule, const ConvertedGraphicsShader*);
// Binds a stage whose original shader has no conversion; draws using it are skipped, not failed.
bool MarkBoundGpuGraphicsShaderUnavailable(GraphicsShaderStage);
void FailBoundGpuCommand(const char* site = __builtin_FUNCTION(), int line = __builtin_LINE());
// [gamerender] True when the calling thread's bound list already failed (follow-on errors are cascade only).
bool BoundGpuCommandAlreadyFailed();
bool InvalidateBoundGpuSamplers();
bool SetBoundGpuComputeSampler(std::uint32_t slot, const GpuApi::SamplerStateRef&, WGPUSampler);
// Success transfers the acquired reference into the selected shader stage.
bool SetBoundGpuSampler(std::uint32_t slot, const GpuApi::SamplerStateRef&, WGPUSampler,
                       std::uint32_t stageIndex);
// Atomically transfers all acquired refs and views for the selected stages.
bool SetBoundGpuSampledTextures(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs = nullptr);
// [cmdlock] Uncapped per-frame counters (see gpu-api-command-web.cpp cmdlockCounters).
void CountCmdlockEvent(unsigned index, unsigned long long amount = 1);
// [cmdlock] True when the selected stages' slots already hold exactly these default-view references.
bool BoundGpuSampledTexturesMatch(std::uint32_t start, std::size_t count, const GpuApi::TextureRef* resolved,
    const bool* selected, const GpuApi::TextureRef* logicalRefs);
// [skperf] round 43 bindKeep: per-slot form of BoundGpuSampledTexturesMatch. keep[stage * MAX_SRV_BINDINGS + i] is
// set for each selected slot start+i that already holds exactly resolved[i] (same predicate). False = no mask.
bool BoundGpuSampledTexturesKeep(std::uint32_t start, std::size_t count, const GpuApi::TextureRef* resolved,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, bool* keep);
// SetBoundGpuSampledTextures that leaves the kept slots untouched (not overwritten, not released; their refs/views
// entries are ignored).
bool SetBoundGpuSampledTexturesKeep(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, const bool* keep);
// Transfers one acquired original structured-buffer reference on success.
bool SetBoundGpuComputeBuffer(std::uint32_t slot, const GpuApi::BufferRef&,
    WGPUBuffer, std::uint64_t bytes, std::uint32_t stride);
bool SetBoundGpuStorageBuffer(std::uint32_t slot, const GpuApi::BufferRef&,
    WGPUBuffer, std::uint64_t bytes, std::uint32_t stride, WGPUBuffer counter = nullptr);
bool SetBoundGpuStructuredBuffers(std::uint32_t slot, const GpuApi::BufferRef* refs,
    WGPUBuffer, std::uint64_t bytes, std::uint32_t stride, const bool* selected);
bool SetBoundGpuComputeConstantBuffer(std::uint32_t slot, const GpuApi::BufferRef&);
// Atomically transfers one acquired ref per selected stage: vertex, pixel, compute.
bool SetBoundGpuConstantBuffers(std::uint32_t slot, const GpuApi::BufferRef* refs,
                               const bool* selected);
// Success transfers the acquired original reference to the current command.
void* LockBoundGpuConstantBuffer(const GpuApi::BufferRef&, std::uint32_t bytes,
    std::uint32_t flags, std::uint32_t offset, std::uint32_t size);
bool UnlockBoundGpuConstantBuffer(const GpuApi::BufferRef&);
// Real nontransient dynamic data is copied into the original backing buffer
// at unlock, in command order. Success transfers the acquired reference.
void* LockBoundGpuBufferUpload(const GpuApi::BufferRef&, WGPUBuffer,
    std::uint32_t bytes, std::uint32_t flags, std::uint32_t offset, std::uint32_t size);
bool UnlockBoundGpuBufferUpload(const GpuApi::BufferRef&);
// Success transfers the acquired texture references and owned views atomically.
bool SetBoundGpuComputeTextures(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef*, const WGPUTextureView*, bool storage);
// Success transfers source/destination original references to the bound list.
// For uploads sourceRef is null and success transfers ownership of source's staging buffer.
bool RecordGpuBufferCopy(WGPUBuffer source, std::uint64_t sourceOffset,
    WGPUBuffer destination, std::uint64_t destinationOffset, std::uint64_t size,
    const GpuApi::BufferRef& sourceRef, const GpuApi::BufferRef& destinationRef);
// Success transfers the two acquired original texture references.
bool RecordGpuTextureCopies(const GpuTextureCopy*, std::size_t count,
    const GpuApi::TextureRef& source, const GpuApi::TextureRef& destination);
bool RecordGpuDepthUnorm16Copy(WGPUTextureView source,WGPUTextureView destination,
    std::uint32_t sx,std::uint32_t sy,std::uint32_t dx,std::uint32_t dy,
    std::uint32_t width,std::uint32_t height,
    const GpuApi::TextureRef& sourceRef,const GpuApi::TextureRef& destinationRef);
bool RecordGpuTextureResolve(WGPUTextureView source, WGPUTextureView destination,
    const GpuApi::TextureRef& sourceRef, const GpuApi::TextureRef& destinationRef);
// Success transfers an acquired texture ref. Upload also transfers buffer
// ownership; readback buffer ownership stays with its pending request.
bool RecordGpuTextureBufferCopy(const WGPUTexelCopyTextureInfo&, const WGPUTexelCopyBufferInfo&,
    const WGPUExtent3D&, const GpuApi::TextureRef&, bool upload);
// Uploads CPU texel rows through a pooled staging arena owned by the bound command (no per-upload buffer
// creation); the buffer-to-texture copy is encoded in command order. Success transfers the texture ref.
bool RecordGpuTextureUploadFromArena(const WGPUTexelCopyTextureInfo&, const void* bytes, std::uint64_t size,
    std::uint32_t bytesPerRow, std::uint32_t rowsPerImage, const WGPUExtent3D&, const GpuApi::TextureRef&);
}
