#pragma once
#include <vector>
#include <webgpu/webgpu.h>
#include <cstddef>
#include <cstdint>
namespace cp2077::web {
// GPU owner thread only; destruction invalidates all views of that texture.
void ClearGpuTextureViewCache();
void InvalidateGpuTextureViews(WGPUTexture);
// GPU owner thread only. Non-owning metadata for successful wrapper views.
WGPUTexture GetGpuTextureViewParent(WGPUTextureView);
WGPUTexture CreateGpuTexture(const WGPUTextureDescriptor& descriptor);
bool DestroyGpuTexture(WGPUTexture texture);
WGPUTextureView CreateGpuTextureView(WGPUTexture texture, const WGPUTextureViewDescriptor& descriptor);
// [notier1] Widen data laid out in a remapped texture's original format to its actual format (rows padded to rowAlign;
// 256 for buffer copies). False when the texture isn't remapped or the pair has no conversion.
bool WidenTier1TextureUpload(const WGPUTexelCopyTextureInfo& destination, const void* data, std::size_t dataSize,
    const WGPUTexelCopyBufferLayout& layout, const WGPUExtent3D& extent, std::vector<unsigned char>& out, WGPUTexelCopyBufferLayout& outLayout,
    std::size_t rowAlign);
bool IsTier1RemappedTexture(WGPUTexture texture);
bool ReleaseGpuTextureView(WGPUTextureView view);
// Any thread. Descriptor format/dimension of a view returned by CreateGpuTextureView (false if unknown or
// either was left Undefined). Exact for any live (retained) view.
bool GetGpuTextureViewInfo(WGPUTextureView view, WGPUTextureFormat& format, WGPUTextureViewDimension& dimension);
// Any thread. Descriptor subresource range of such a view (mipCount/layerCount may be the WGPU *_UNDEFINED
// "rest of the texture" values). False if unknown.
bool GetGpuTextureViewRange(WGPUTextureView view, std::uint32_t& baseMip, std::uint32_t& mipCount,
                            std::uint32_t& baseLayer, std::uint32_t& layerCount);
// Worker threads only. Takes one more reference on view (released with ReleaseGpuTextureView) when it is still
// owned by the shared view cache of the current device generation, re-recording its info from recorded
// (format, dimension, mip and layer range). False otherwise (nothing taken).
bool RetainCachedGpuTextureViewOnWorker(WGPUTextureView view, const WGPUTextureViewDescriptor& recorded);
// Worker threads only. 1x1 StorageBinding-only view of format/dimension (2D, 2DArray, 3D) whose writes are
// discarded; borrowed device-lifetime handle (never release it). nullptr if unavailable.
// [potato] r6 index selects a distinct texture of the same format/dimension (0 = the original shared one), so one
// dispatch with several scratch storage bindings does not alias writable storage (WebGPU validation error).
WGPUTextureView GetGpuScratchStorageTextureView(WGPUTextureFormat format, WGPUTextureViewDimension dimension,
                                                unsigned index = 0);
// Initial image upload only: caller keeps bytes alive until this returns.
struct GpuTextureUploadRegion {
    WGPUTexelCopyTextureInfo destination; const void* data; std::size_t size;
    WGPUTexelCopyBufferLayout layout; WGPUExtent3D extent;
};
// [compress] All regions of one initial image in ONE main-thread proxy and one error-scope set (in order).
bool UploadInitialGpuTextureRegions(const GpuTextureUploadRegion* regions, std::size_t count);
bool UploadInitialGpuTexture(const WGPUTexelCopyTextureInfo& destination,
    const void* data, std::size_t dataSize, const WGPUTexelCopyBufferLayout& layout,
    const WGPUExtent3D& extent);
// Declared type of one WGSL @binding, for zero stand-ins of null/unbound shader inputs.
enum GpuWgslBindingKind : std::uint32_t {
    GpuWgslBinding_Unknown = 0, // not a sampled texture or sampler (uniform, storage, multisampled...)
    GpuWgslBinding_Float, GpuWgslBinding_Uint, GpuWgslBinding_Sint, GpuWgslBinding_Depth,
    GpuWgslBinding_Sampler, GpuWgslBinding_ComparisonSampler,
};
struct GpuWgslBinding {
    GpuWgslBindingKind kind = GpuWgslBinding_Unknown;
    WGPUTextureViewDimension dimension = WGPUTextureViewDimension_Undefined;
};
// Any thread. Parses the declaration once per (wgsl pointer, binding); wgsl must be static text.
GpuWgslBinding FindGpuWgslBinding(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding);
// Worker threads only (nullptr on the main thread). Zero-initialized 1x1 texture view / clamp-nearest
// sampler matching the declaration (D3D reads zero from a null SRV). Borrowed, device-lifetime
// handles: never release them and never pair them with a GpuApi reference. nullptr if unsupported.
WGPUTextureView GetGpuDefaultTextureView(const GpuWgslBinding& binding);
WGPUSampler GetGpuDefaultSampler(bool comparison);
// [gamerender] Clamp + linear (min/mag/mip) stand-in for engine-null samplers whose WGSL use is filtering
// (static linear-clamp samplers in the original root signatures: blur, temporal history).
WGPUSampler GetGpuDefaultLinearSampler();
// [textures] Emulated bindless (t130) for multilayered pixel shaders: every texture whose bindless index the
// engine reads (GpuApi::GetTextureIndirection) is copied into a texture_2d_array pool chunk of its exact class
// (format, size, mips); a 64K-entry u32 table maps index -> ((chunk + 1) << 16) | layer, 0 = unmapped.
constexpr unsigned kGpuMultilayerPoolSlots = 40;
constexpr std::uint64_t kGpuMultilayerTableBytes = 65536ull * 4;
// Any thread (proxies to the main thread). Copies all mips of source into a pool layer and maps index.
// False when the class cannot be pooled (format/limits) - the shader then reads its neutral fallback.
bool PoolGpuMultilayerTexture(std::uint16_t index, WGPUTexture source, WGPUTextureFormat format,
    std::uint32_t width, std::uint32_t height, std::uint32_t mips,
    std::uint32_t blockWidth, std::uint32_t blockHeight, std::uint32_t blockBytes);
// Any thread. Unmaps index and frees its layer (before the source texture is destroyed).
void UnpoolGpuMultilayerIndex(std::uint16_t index, WGPUTexture source);
// Any thread. Marks the pool copy of source stale after a later write into source.
void NoteGpuMultilayerSourceWrite(WGPUTexture source);
bool HasGpuMultilayerDirtyCopies();
// Main thread, inside a command encoder (between passes): refresh stale pool copies.
bool EncodeGpuMultilayerDirtyCopies(WGPUCommandEncoder encoder);
// Any thread. Borrowed device-lifetime views (pool chunk or a 1x1 dummy) for every slot, and the table buffer.
bool GetGpuMultilayerPoolBindings(WGPUTextureView (&views)[kGpuMultilayerPoolSlots], WGPUBuffer& table);
// [wcfix] Any thread. Table state of each index: 0 = unmapped (never offered or unpooled), 1 = mapped,
// 2 = mapped in a chunk slot >= 37 (beyond the smallest capped multilayer PS), 3 = last offer rejected.
// rejectReasons (optional) gets the reject reason text for state 3. Returns the number of chunk slots in use.
unsigned GetGpuMultilayerIndexStates(const std::uint16_t* ids, unsigned count, std::uint8_t* states,
    const char** rejectReasons = nullptr);
}
