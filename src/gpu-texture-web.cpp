#include "gpu-compute-web.h"
#include "gpu-texture-web.h"
#include "gpu-device-web.h"
#include "gpu-sampler-web.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>
#include <map>
#include <string>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <unordered_map>
#include <vector>
#include <emscripten/emscripten.h>

namespace {
// View handle -> descriptor format/dimension. Written on every successful CreateGpuTextureView return
// (new view or cache reuse), so a recycled handle is overwritten before it can be bound again; never erased.
// Sharded by handle (16 independent mutex+map pairs): this runs on every view acquisition on every render worker
// (each bindless/SRV draw), and one global mutex serialized them. Same per-handle semantics as the single map;
// the 65536 bound is now per shard (4096 each).
// Also records the selected subresource range (perf7: same-dispatch sampled/storage overlap check).
struct ViewInfo {
    WGPUTextureFormat format; WGPUTextureViewDimension dimension;
    std::uint32_t baseMip, mipCount, baseLayer, layerCount;
};
struct ViewInfoShard {
    std::mutex mutex;
    std::unordered_map<WGPUTextureView, ViewInfo> infos;
};
ViewInfoShard& ViewInfoShardFor(WGPUTextureView view) {
    static ViewInfoShard shards[16];
    const auto bits = reinterpret_cast<std::uintptr_t>(view);
    return shards[(bits >> 4 ^ bits >> 12) & 15];
}
WGPUTextureView RecordGpuTextureViewInfo(WGPUTextureView view, const WGPUTextureViewDescriptor& descriptor) {
    if (!view) return view;
    auto& shard = ViewInfoShardFor(view);
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.infos.size() >= 4096 && !shard.infos.count(view)) shard.infos.clear(); // live views re-record on next acquire
    shard.infos[view] = {descriptor.format, descriptor.dimension, descriptor.baseMipLevel, descriptor.mipLevelCount,
        descriptor.baseArrayLayer, descriptor.arrayLayerCount};
    return view;
}
em_proxying_queue* TextureQueue() {
    static auto* queue = em_proxying_queue_create(); return queue;
}
struct Allocation {
    WGPUTextureDescriptor descriptor;
    WGPUTexture texture = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 3;
    bool failed = false;
};
struct Upload {
    WGPUTexelCopyTextureInfo destination;
    const void* data;
    std::size_t size;
    WGPUTexelCopyBufferLayout layout;
    WGPUExtent3D extent;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 3;
    bool succeeded = true;
};
struct ViewRequest {
    WGPUTexture texture;
    WGPUTextureViewDescriptor descriptor;
    WGPUTextureView view = nullptr;
    WGPUDevice device=nullptr;std::string key;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 3;
    bool failed = false;
    std::uint64_t generation=0;
};
#include "gpu-texture-view-cache-web.inl"
void ViewChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                 WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<ViewRequest*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true;
        std::fputs("CP2077 WebGPU texture view failed: ", stderr);
        if (message.data) {
            if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
            else std::fwrite(message.data, 1, message.length, stderr);
        }
        std::fputc('\n', stderr);
    }
    if (--request->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady() || request->device!=cp2077::web::MainThreadGpuDevice() || request->generation!=cp2077::web::GpuDeviceGeneration()) request->failed = true;
    if (request->failed && request->view) { wgpuTextureViewRelease(request->view); request->view = nullptr; }
    StoreTextureView(*request);
    wgpuDeviceRelease(request->device);
    emscripten_proxy_finish(request->context);
}
void UploadChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                   WGPUStringView message, void* argument, void*) {
    auto* upload = static_cast<Upload*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        upload->succeeded = false;
        std::fputs("CP2077 WebGPU image upload failed: ", stderr);
        if (message.data) {
            if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
            else std::fwrite(message.data, 1, message.length, stderr);
        }
        std::fputc('\n', stderr);
    }
    if (--upload->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady()) upload->succeeded = false;
    emscripten_proxy_finish(upload->context);
}
void Checked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
             WGPUStringView message, void* argument, void*) {
    auto* allocation = static_cast<Allocation*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        allocation->failed = true;
        std::fputs("CP2077 WebGPU texture allocation failed: ", stderr);
        if (message.data) {
            if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
            else std::fwrite(message.data, 1, message.length, stderr);
        }
        std::fputc('\n', stderr);
    }
    if (--allocation->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady()) allocation->failed = true;
    if (allocation->failed && allocation->texture) {
        wgpuTextureDestroy(allocation->texture); wgpuTextureRelease(allocation->texture);
        allocation->texture = nullptr;
    }
    emscripten_proxy_finish(allocation->context);
}
}
namespace cp2077::web {
WGPUTexture GetGpuTextureViewParent(WGPUTextureView view){
 if(!emscripten_is_main_runtime_thread())return nullptr;
 std::lock_guard<std::recursive_mutex> guard(textureViewMutex);
 auto it=textureViewParents.find(view);return it==textureViewParents.end()?nullptr:it->second;
}
void ClearGpuTextureViewCache(){if(emscripten_is_main_runtime_thread())ClearViews();}
void InvalidateGpuTextureViews(WGPUTexture texture){
 if(!emscripten_is_main_runtime_thread())return;
 std::lock_guard<std::recursive_mutex> guard(textureViewMutex);
 if(ViewIndexOn()){
  // [skperf] indexed: touch only this texture's cached views and parent entries.
  auto keys=cacheKeysByTexture.find(texture);
  if(keys!=cacheKeysByTexture.end()){
   const std::vector<std::string> copy(keys->second);
   for(const auto& key:copy){auto current=textureViewCache.find(key);if(current!=textureViewCache.end()&&current->second.texture==texture)EraseView(current);}
   cacheKeysByTexture.erase(texture);
  }
  auto views=parentViewsByTexture.find(texture);
  if(views!=parentViewsByTexture.end()){
   for(auto view:views->second){auto parent=textureViewParents.find(view);if(parent!=textureViewParents.end()&&parent->second==texture)textureViewParents.erase(parent);}
   parentViewsByTexture.erase(views);
  }
 }else{
 for(auto it=textureViewCache.begin();it!=textureViewCache.end();){auto current=it++;if(current->second.texture==texture)EraseView(current);}
 for(auto it=textureViewParents.begin();it!=textureViewParents.end();){if(it->second==texture)it=textureViewParents.erase(it);else ++it;}
 cacheKeysByTexture.erase(texture);parentViewsByTexture.erase(texture);
 }
 PublishViews();
}

// [notier1] Without texture-formats-tier1 these format/usage pairs do not exist (createTexture fails validation or
// throws): Firefox, older Chrome. Textures get the nearest core format; their views follow (pipelines read the
// recorded view format). Compute shaders that need tier1 storage are already skipped (gpu-shader-web.cpp).
std::mutex tier1RemapMutex;
std::unordered_map<WGPUTexture, std::pair<WGPUTextureFormat, WGPUTextureFormat>> tier1Remapped;
WGPUTextureFormat Tier1CoreFormat(WGPUTextureFormat f, WGPUTextureUsage usage) {
    // Storage remaps keep the channel count (r8 -> r32, rg8/rg16 -> rg32): a render pipeline rejects a fragment output
    // with fewer components than its target, and the WGSL rewrite (engine-select.js [notier1]) maps by name only.
    const bool storage = (usage & WGPUTextureUsage_StorageBinding) != 0, target = (usage & WGPUTextureUsage_RenderAttachment) != 0;
    switch (f) {
    case WGPUTextureFormat_R16Unorm: case WGPUTextureFormat_R16Snorm: return WGPUTextureFormat_R32Float;
    case WGPUTextureFormat_RG16Unorm: case WGPUTextureFormat_RG16Snorm: // render targets keep two channels: a vec2
        return target && !storage ? WGPUTextureFormat_RG16Float : WGPUTextureFormat_RGBA16Float; // output can't fill rgba
    case WGPUTextureFormat_RGBA16Unorm: case WGPUTextureFormat_RGBA16Snorm: return WGPUTextureFormat_RGBA16Float;
    case WGPUTextureFormat_RG11B10Ufloat: case WGPUTextureFormat_RGB10A2Unorm: return storage ? WGPUTextureFormat_RGBA16Float : f;
    case WGPUTextureFormat_RG16Float: return storage ? WGPUTextureFormat_RG32Float : f;
    case WGPUTextureFormat_R16Float: return storage ? WGPUTextureFormat_R32Float : f;
    case WGPUTextureFormat_R8Unorm: return storage ? WGPUTextureFormat_R32Float : f;
    case WGPUTextureFormat_RG8Unorm: return storage ? WGPUTextureFormat_RG32Float : f;
    case WGPUTextureFormat_R8Snorm: return storage ? WGPUTextureFormat_R32Float : target ? WGPUTextureFormat_R16Float : f;
    case WGPUTextureFormat_RG8Snorm: return storage ? WGPUTextureFormat_RG32Float : target ? WGPUTextureFormat_RG16Float : f;
    case WGPUTextureFormat_RGBA8Snorm: return target ? WGPUTextureFormat_RGBA16Float : f;
    case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_R16Uint: return storage ? WGPUTextureFormat_R32Uint : f;
    case WGPUTextureFormat_RG8Uint: case WGPUTextureFormat_RG16Uint: return storage ? WGPUTextureFormat_RG32Uint : f;
    case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_R16Sint: return storage ? WGPUTextureFormat_R32Sint : f;
    case WGPUTextureFormat_RG8Sint: case WGPUTextureFormat_RG16Sint: return storage ? WGPUTextureFormat_RG32Sint : f;
    case WGPUTextureFormat_RGB10A2Uint: return storage ? WGPUTextureFormat_RGBA16Uint : f;
    default: return f;
    }
}
void ForgetTier1Remap(WGPUTexture texture) {
    if (!GpuLacksTier1()) return;
    std::lock_guard<std::mutex> lock(tier1RemapMutex); tier1Remapped.erase(texture);
}
// [notier1] Initial data for a remapped texture is still laid out in the original format: widen it texel by texel
// (missing channels zero, 16-bit ints extended, halves to floats). False when the pair needs no or no known conversion.
static float Tier1HalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = std::uint32_t(h & 0x8000u) << 16, exp = (h >> 10) & 0x1fu, man = h & 0x3ffu;
    std::uint32_t bits;
    if (!exp) { if (!man) bits = sign; else { std::uint32_t e = 113, m = man; while (!(m & 0x400u)) { m <<= 1; --e; } bits = sign | (e << 23) | ((m & 0x3ffu) << 13); } }
    else if (exp == 31) bits = sign | 0x7f800000u | (man << 13);
    else bits = sign | ((exp + 112) << 23) | (man << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
}
static std::uint16_t F32ToF16Tier1(float value) {
    std::uint32_t bits; std::memcpy(&bits, &value, 4);
    const std::uint32_t sign = (bits >> 16) & 0x8000u; const int exp = int((bits >> 23) & 0xffu) - 127 + 15;
    const std::uint32_t man = bits & 0x7fffffu;
    if (exp <= 0) return std::uint16_t(sign | (exp < -10 ? 0 : ((man | 0x800000u) >> (14 - exp))));
    if (exp >= 31) return std::uint16_t(sign | 0x7c00u);
    return std::uint16_t(sign | (std::uint32_t(exp) << 10) | (man >> 13));
}
static bool ExpandTier1Upload(const WGPUTexelCopyTextureInfo& destination, const void* data, std::size_t dataSize,
    const WGPUTexelCopyBufferLayout& layout, const WGPUExtent3D& extent, std::vector<unsigned char>& out, WGPUTexelCopyBufferLayout& outLayout,
    std::size_t rowAlign = 1) {
    if (!GpuLacksTier1() || !destination.texture || !data) return false;
    WGPUTextureFormat from, to;
    {
        std::lock_guard<std::mutex> lock(tier1RemapMutex);
        const auto found = tier1Remapped.find(destination.texture);
        if (found == tier1Remapped.end()) return false;
        from = found->second.first; to = found->second.second;
    }
    // Source component kind: 0 u8, 1 s8, 2 unorm8, 3 snorm8, 4 u16, 5 s16, 6 f16; destinations keep the channel count
    // (32-bit, or half for snorm8 render targets).
    unsigned srcCh = 0, comp = 0, dstCh = 0, dstComp = 0; int kind = -1;
    switch (from) {
    case WGPUTextureFormat_R8Uint: srcCh = 1; comp = 1; kind = 0; break;   case WGPUTextureFormat_RG8Uint: srcCh = 2; comp = 1; kind = 0; break;
    case WGPUTextureFormat_R8Sint: srcCh = 1; comp = 1; kind = 1; break;   case WGPUTextureFormat_RG8Sint: srcCh = 2; comp = 1; kind = 1; break;
    case WGPUTextureFormat_R8Unorm: srcCh = 1; comp = 1; kind = 2; break;  case WGPUTextureFormat_RG8Unorm: srcCh = 2; comp = 1; kind = 2; break;
    case WGPUTextureFormat_R8Snorm: srcCh = 1; comp = 1; kind = 3; break;  case WGPUTextureFormat_RG8Snorm: srcCh = 2; comp = 1; kind = 3; break;
    case WGPUTextureFormat_R16Uint: srcCh = 1; comp = 2; kind = 4; break;  case WGPUTextureFormat_RG16Uint: srcCh = 2; comp = 2; kind = 4; break;
    case WGPUTextureFormat_R16Sint: srcCh = 1; comp = 2; kind = 5; break;  case WGPUTextureFormat_RG16Sint: srcCh = 2; comp = 2; kind = 5; break;
    case WGPUTextureFormat_R16Float: srcCh = 1; comp = 2; kind = 6; break; case WGPUTextureFormat_RG16Float: srcCh = 2; comp = 2; kind = 6; break;
    default: return false;
    }
    switch (to) {
    case WGPUTextureFormat_R32Uint: case WGPUTextureFormat_R32Sint: case WGPUTextureFormat_R32Float: dstCh = 1; dstComp = 4; break;
    case WGPUTextureFormat_RG32Uint: case WGPUTextureFormat_RG32Sint: case WGPUTextureFormat_RG32Float: dstCh = 2; dstComp = 4; break;
    case WGPUTextureFormat_R16Float: dstCh = 1; dstComp = 2; break;
    case WGPUTextureFormat_RG16Float: dstCh = 2; dstComp = 2; break;
    default: return false;
    }
    if (dstCh != srcCh) return false;
    const bool toFloat = to == WGPUTextureFormat_R32Float || to == WGPUTextureFormat_RG32Float;
    if (dstComp == 2 && kind != 3) return false; // snorm8 render targets -> half
    const std::size_t srcTexel = std::size_t(srcCh) * comp, dstTexel = std::size_t(dstCh) * dstComp;
    const std::size_t srcRow = layout.bytesPerRow != WGPU_COPY_STRIDE_UNDEFINED ? layout.bytesPerRow : extent.width * srcTexel;
    const std::size_t rows = layout.rowsPerImage != WGPU_COPY_STRIDE_UNDEFINED ? layout.rowsPerImage : extent.height;
    const std::size_t dstRow = (extent.width * dstTexel + rowAlign - 1) / rowAlign * rowAlign, depth = extent.depthOrArrayLayers ? extent.depthOrArrayLayers : 1;
    if (!extent.width || !extent.height || srcRow < extent.width * srcTexel ||
        layout.offset + (depth - 1) * srcRow * rows + (extent.height - 1) * srcRow + extent.width * srcTexel > dataSize) return false;
    out.assign(dstRow * extent.height * depth, 0);
    const auto* src = static_cast<const unsigned char*>(data) + layout.offset;
    for (std::size_t z = 0; z < depth; ++z)
        for (std::size_t y = 0; y < extent.height; ++y) {
            const unsigned char* s = src + z * srcRow * rows + y * srcRow;
            unsigned char* d = out.data() + (z * extent.height + y) * dstRow;
            for (std::size_t x = 0; x < extent.width; ++x, s += srcTexel, d += dstTexel)
                for (unsigned c = 0; c < srcCh; ++c) {
                    const unsigned char* sc = s + c * comp; unsigned char* dc = d + c * dstComp;
                    std::uint16_t h = 0; if (comp == 2) std::memcpy(&h, sc, 2);
                    float f = 0; std::int64_t i = 0;
                    switch (kind) {
                    case 0: i = sc[0]; f = float(i); break;
                    case 1: i = std::int8_t(sc[0]); f = float(i); break;
                    case 2: f = sc[0] / 255.0f; break;
                    case 3: f = std::max(-1.0f, std::int8_t(sc[0]) / 127.0f); break;
                    case 4: i = h; f = float(i); break;
                    case 5: i = std::int16_t(h); f = float(i); break;
                    default: f = Tier1HalfToFloat(h); break;
                    }
                    if (dstComp == 2) { const std::uint16_t o = F32ToF16Tier1(f); std::memcpy(dc, &o, 2); }
                    else if (toFloat) std::memcpy(dc, &f, 4);
                    else { const std::int32_t o = std::int32_t(i); std::memcpy(dc, &o, 4); }
                }
        }
    outLayout = layout; outLayout.offset = 0; outLayout.bytesPerRow = std::uint32_t(dstRow); outLayout.rowsPerImage = std::uint32_t(extent.height);
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8) std::fprintf(stderr, "CP2077 notier1: widened upload format %u -> %u %ux%ux%u\n",
        unsigned(from), unsigned(to), extent.width, extent.height, unsigned(depth));
    return true;
}
bool WidenTier1TextureUpload(const WGPUTexelCopyTextureInfo& destination, const void* data, std::size_t dataSize,
    const WGPUTexelCopyBufferLayout& layout, const WGPUExtent3D& extent, std::vector<unsigned char>& out, WGPUTexelCopyBufferLayout& outLayout,
    std::size_t rowAlign) {
    return ExpandTier1Upload(destination, data, dataSize, layout, extent, out, outLayout, rowAlign);
}
bool IsTier1RemappedTexture(WGPUTexture texture) {
    if (!GpuLacksTier1() || !texture) return false;
    std::lock_guard<std::mutex> lock(tier1RemapMutex);
    const auto found = tier1Remapped.find(texture);
    return found != tier1Remapped.end() && found->second.first != found->second.second;
}
WGPUTextureView CreateGpuTextureView(WGPUTexture texture, const WGPUTextureViewDescriptor& viewDescriptor) {
    if (emscripten_is_main_runtime_thread() || !texture || !IsGpuDeviceReady()) return nullptr;
    auto* queue = TextureQueue();
    if (!queue) return nullptr;
    WGPUTextureViewDescriptor descriptor = viewDescriptor;
    if (GpuLacksTier1()) {
        std::lock_guard<std::mutex> lock(tier1RemapMutex);
        const auto found = tier1Remapped.find(texture);
        if (found != tier1Remapped.end()) // every view of a remapped texture uses its actual format (callers may ask for the
            descriptor.format = found->second.second; // original or for the Tier1Fallback format of the original)
        else if (descriptor.format != WGPUTextureFormat_Undefined) // formats that never exist without tier1 (textures came
            descriptor.format = Tier1CoreFormat(descriptor.format, WGPUTextureUsage_None); // through Translate already)
    }
    ViewRequest request{texture, descriptor};
    if(ReuseTextureViewOnWorker(request))return RecordGpuTextureViewInfo(request.view, descriptor);
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* request = static_cast<ViewRequest*>(argument);
            request->context = context;
            auto device = MainThreadGpuDevice();
            if (!device) { emscripten_proxy_finish(context); return; }
            request->device=device;
            request->generation=GpuDeviceGeneration();
            if(ReuseTextureView(*request))return;
            wgpuDeviceAddRef(device);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
            request->view = wgpuTextureCreateView(request->texture, &request->descriptor);
            if (!request->view) request->failed = true;
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = ViewChecked; callback.userdata1 = request;
            for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
        }, &request)) return nullptr;
    return RecordGpuTextureViewInfo(request.view, descriptor);
}
bool GetGpuTextureViewInfo(WGPUTextureView view, WGPUTextureFormat& format, WGPUTextureViewDimension& dimension) {
    format = WGPUTextureFormat_Undefined; dimension = WGPUTextureViewDimension_Undefined;
    if (!view) return false;
    auto& shard = ViewInfoShardFor(view);
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto found = shard.infos.find(view);
    if (found == shard.infos.end()) return false;
    format = found->second.format; dimension = found->second.dimension;
    return format != WGPUTextureFormat_Undefined && dimension != WGPUTextureViewDimension_Undefined;
}
bool GetGpuTextureViewRange(WGPUTextureView view, std::uint32_t& baseMip, std::uint32_t& mipCount,
                            std::uint32_t& baseLayer, std::uint32_t& layerCount) {
    baseMip = mipCount = baseLayer = layerCount = 0;
    if (!view) return false;
    auto& shard = ViewInfoShardFor(view);
    std::lock_guard<std::mutex> lock(shard.mutex);
    const auto found = shard.infos.find(view);
    if (found == shard.infos.end()) return false;
    baseMip = found->second.baseMip; mipCount = found->second.mipCount;
    baseLayer = found->second.baseLayer; layerCount = found->second.layerCount;
    return true;
}
bool RetainCachedGpuTextureViewOnWorker(WGPUTextureView view, const WGPUTextureViewDescriptor& recorded) {
    if (!view || emscripten_is_main_runtime_thread()) return false;
    {
        std::shared_lock<std::shared_mutex> guard(textureViewRw); // perf11: worker read path
        if (!IsGpuDeviceReady() || !textureViewCacheDevice || textureViewGeneration != GpuDeviceGeneration() ||
            !cachedTextureViewHandles.count(view)) return false;
        wgpuTextureViewAddRef(view); ++textureViewHits; ++textureViewWorkerHits;
    }
    RecordGpuTextureViewInfo(view, recorded); // same re-record as every CreateGpuTextureView return
    return true;
}
bool ReleaseGpuTextureView(WGPUTextureView view) {
    if (!view) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuTextureViewRelease(view); return true; }
    if(ReleaseCachedTextureViewOnWorker(view))return true;
    auto* queue = TextureQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuTextureViewRelease(static_cast<WGPUTextureView>(argument)); }, view);
}
bool UploadInitialGpuTexture(const WGPUTexelCopyTextureInfo& destination,
    const void* data, std::size_t dataSize, const WGPUTexelCopyBufferLayout& layout,
    const WGPUExtent3D& extent) {
    if (emscripten_is_main_runtime_thread() || !data || !dataSize || !destination.texture) return false;
    auto* queue = TextureQueue();
    if (!queue) return false;
    std::vector<unsigned char> widened; WGPUTexelCopyBufferLayout widenedLayout; // [notier1]
    const bool widen = ExpandTier1Upload(destination, data, dataSize, layout, extent, widened, widenedLayout);
    Upload upload{destination, widen ? widened.data() : data, widen ? widened.size() : dataSize, widen ? widenedLayout : layout, extent};
    const bool completed = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* upload = static_cast<Upload*>(argument);
            upload->context = context;
            DrainDeferredGpuWork(); // perf12: after earlier asynchronous submissions
            auto device = MainThreadGpuDevice();
            auto queue = MainThreadGpuQueue();
            if (!device || !queue) {
                upload->succeeded = false; emscripten_proxy_finish(context); return;
            }
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
            // writeTexture accepts tightly packed rows. Initial uploads finish
            // before the resource is published, so later queue submissions see them.
            wgpuQueueWriteTexture(queue, &upload->destination, upload->data, upload->size,
                                   &upload->layout, &upload->extent);
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = UploadChecked; callback.userdata1 = upload;
            for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
        }, &upload);
    return completed && upload.succeeded;
}
namespace {
struct UploadBatch { const GpuTextureUploadRegion* regions; std::size_t count; Upload status; };
}
bool UploadInitialGpuTextureRegions(const GpuTextureUploadRegion* regions, std::size_t count) {
    if (emscripten_is_main_runtime_thread() || !regions || !count) return false;
    for (std::size_t i = 0; i < count; ++i)
        if (!regions[i].data || !regions[i].size || !regions[i].destination.texture) return false;
    auto* queue = TextureQueue();
    if (!queue) return false;
    std::vector<GpuTextureUploadRegion> widenedRegions; std::vector<std::vector<unsigned char>> widenedData; // [notier1]
    if (GpuLacksTier1()) {
        for (std::size_t i = 0; i < count; ++i) {
            std::vector<unsigned char> bytes; WGPUTexelCopyBufferLayout l;
            if (!ExpandTier1Upload(regions[i].destination, regions[i].data, regions[i].size, regions[i].layout, regions[i].extent, bytes, l)) continue;
            if (widenedRegions.empty()) { widenedRegions.assign(regions, regions + count); widenedData.resize(count); }
            widenedData[i] = std::move(bytes);
            widenedRegions[i].data = widenedData[i].data(); widenedRegions[i].size = widenedData[i].size(); widenedRegions[i].layout = l;
        }
        if (!widenedRegions.empty()) regions = widenedRegions.data();
    }
    UploadBatch batch{regions, count, Upload{}};
    const bool completed = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* batch = static_cast<UploadBatch*>(argument);
            auto* upload = &batch->status;
            upload->context = context;
            DrainDeferredGpuWork(); // same ordering guarantee as UploadInitialGpuTexture
            auto device = MainThreadGpuDevice();
            auto queue = MainThreadGpuQueue();
            if (!device || !queue) { upload->succeeded = false; emscripten_proxy_finish(context); return; }
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
            for (std::size_t i = 0; i < batch->count; ++i) {
                const auto& r = batch->regions[i];
                wgpuQueueWriteTexture(queue, &r.destination, r.data, r.size, &r.layout, &r.extent);
            }
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = UploadChecked; callback.userdata1 = upload;
            for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
        }, &batch);
    return completed && batch.status.succeeded;
}
namespace {
// [skperf] Worker-side wall time of synchronous texture creations and destroys ('CP2077 skperf texture wait probe',
// every ~2 s): each creation waits for the main thread plus a GPU-process error-scope round trip, each worker destroy
// drains the deferred submit backlog first. Shows how much render-worker time per second goes to them.
std::atomic<unsigned long long> texCreateCalls{0}, texCreateUs{0}, texCreateMaxUs{0}, texDestroyCalls{0}, texDestroyUs{0}, texDestroyMaxUs{0};
static bool SkperfProbeOn() { // [skperf] round 5: per-second / churn probes only with ?perfprobe=1
    static const int value = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('perfprobe') === '1' ? 1 : 0; });
    return value != 0;
}
void NoteTextureWait(bool create, double ms) {
    if (!SkperfProbeOn()) return;
    const auto us = static_cast<unsigned long long>(ms * 1000.0);
    auto& calls = create ? texCreateCalls : texDestroyCalls; auto& total = create ? texCreateUs : texDestroyUs; auto& peak = create ? texCreateMaxUs : texDestroyMaxUs;
    calls.fetch_add(1, std::memory_order_relaxed); total.fetch_add(us, std::memory_order_relaxed);
    auto previous = peak.load(std::memory_order_relaxed); while (us > previous && !peak.compare_exchange_weak(previous, us)) {}
    static std::atomic<double> windowStart{0};
    const double now = emscripten_get_now(); double start = windowStart.load(std::memory_order_relaxed);
    if (start == 0) { windowStart.compare_exchange_strong(start, now); return; }
    if (now - start < 2000 || !windowStart.compare_exchange_strong(start, now)) return;
    const double seconds = (now - start) / 1000.0;
    const auto cc = texCreateCalls.exchange(0), cu = texCreateUs.exchange(0), cm = texCreateMaxUs.exchange(0);
    const auto dc = texDestroyCalls.exchange(0), du = texDestroyUs.exchange(0), dm = texDestroyMaxUs.exchange(0);
    std::fprintf(stderr, "CP2077 skperf texture wait probe: window=%.1fs createsPerSec=%.1f createWaitMsPerSec=%.1f createMaxMs=%.2f destroysPerSec=%.1f destroyWaitMsPerSec=%.1f destroyMaxMs=%.2f\n",
        seconds, cc / seconds, cu / 1000.0 / seconds, cm / 1000.0, dc / seconds, du / 1000.0 / seconds, dm / 1000.0);
}
}
WGPUTexture CreateGpuTexture(const WGPUTextureDescriptor& descriptor) {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    // [nobc] createTexture THROWS (TypeError, not a validation error) for a format whose feature is not enabled; the
    // throw skipped emscripten_proxy_finish and the creating worker waited forever (Android 57 % hang). Fail instead.
    auto isBC = [](WGPUTextureFormat f) { return f >= WGPUTextureFormat_BC1RGBAUnorm && f <= WGPUTextureFormat_BC7RGBAUnormSrgb; };
    bool needsBC = isBC(descriptor.format);
    for (std::size_t i = 0; i < descriptor.viewFormatCount; ++i) needsBC = needsBC || isBC(descriptor.viewFormats[i]);
    if (needsBC && !SupportsGpuFeature(WGPUFeatureName_TextureCompressionBC)) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "CP2077 nobc: refused BC texture on a device without BC: format=%u %ux%u\n",
            unsigned(descriptor.format), descriptor.size.width, descriptor.size.height);
        return nullptr;
    }
    auto* queue = TextureQueue();
    if (!queue) return nullptr;
    WGPUTextureDescriptor adjusted = descriptor; // [notier1]
    const bool remap = GpuLacksTier1() && Tier1CoreFormat(descriptor.format, descriptor.usage) != descriptor.format;
    if (remap) {
        adjusted.format = Tier1CoreFormat(descriptor.format, descriptor.usage); adjusted.viewFormatCount = 0; adjusted.viewFormats = nullptr;
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 16) std::fprintf(stderr, "CP2077 notier1: texture format %u -> %u (usage 0x%x) %ux%u\n",
            unsigned(descriptor.format), unsigned(adjusted.format), unsigned(descriptor.usage), descriptor.size.width, descriptor.size.height);
    }
    Allocation allocation{adjusted};
    const double waitStarted = emscripten_get_now();
    struct WaitNote { double started; ~WaitNote() { NoteTextureWait(true, emscripten_get_now() - started); } } waitNote{waitStarted};
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* allocation = static_cast<Allocation*>(argument);
            allocation->context = context;
            auto device = MainThreadGpuDevice();
            if (!device) { emscripten_proxy_finish(context); return; }
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
            allocation->texture = wgpuDeviceCreateTexture(device, &allocation->descriptor);
            if (!allocation->texture) allocation->failed = true;
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = Checked; callback.userdata1 = allocation;
            for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
        }, &allocation)) return nullptr;
    // rg11b10 render targets keep their format (gpu-api-texture-web CreateTexture) but are viewed as the rgba16float that
    // Tier1Fallback gives the engine format: record them too so those views use the real format.
    if ((remap || (GpuLacksTier1() && descriptor.format == WGPUTextureFormat_RG11B10Ufloat)) && allocation.texture) {
        std::lock_guard<std::mutex> lock(tier1RemapMutex); tier1Remapped[allocation.texture] = {descriptor.format, adjusted.format}; }
    return allocation.texture;
}
namespace {
// [skperf] Texture destroy churn probe (main thread): buckets real destroys by descriptor and prints the top
// buckets every 4096 destroys, with the window's rate, so the next per-frame create/destroy source is named.
void NoteGpuTextureDestroy(WGPUTexture texture) {
    if (!SkperfProbeOn()) return;
    static std::unordered_map<std::uint64_t, unsigned> buckets;
    static unsigned destroyed = 0; static double windowStart = 0;
    if (!windowStart) windowStart = emscripten_get_now();
    const std::uint64_t w = wgpuTextureGetWidth(texture), h = wgpuTextureGetHeight(texture), d = wgpuTextureGetDepthOrArrayLayers(texture);
    const std::uint64_t f = std::uint64_t(wgpuTextureGetFormat(texture)) & 0xff, u = std::uint64_t(wgpuTextureGetUsage(texture)) & 0xff;
    ++buckets[(w & 0x3fff) | ((h & 0x3fff) << 14) | ((d & 0x7ff) << 28) | (f << 39) | (u << 47)];
    if ((++destroyed & 4095) != 0) return;
    const double now = emscripten_get_now(), perSecond = 4096000.0 / std::max(1.0, now - windowStart); windowStart = now;
    std::vector<std::pair<unsigned, std::uint64_t>> top; for (const auto& b : buckets) top.push_back({b.second, b.first});
    std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::fprintf(stderr, "CP2077 skperf texture destroy churn: total=%u perSec=%.0f buckets=%u top:", destroyed, perSecond, unsigned(buckets.size()));
    for (std::size_t i = 0; i < top.size() && i < 8; ++i) { const auto k = top[i].second;
        std::fprintf(stderr, " [%ux%ux%u fmt=%u usage=0x%x n=%u]", unsigned(k & 0x3fff), unsigned((k >> 14) & 0x3fff), unsigned((k >> 28) & 0x7ff), unsigned((k >> 39) & 0xff), unsigned((k >> 47) & 0xff), top[i].first); }
    std::fputc('\n', stderr); buckets.clear();
}
}
bool DestroyGpuTexture(WGPUTexture texture) {
    if (!texture) return true;
    ForgetTier1Remap(texture); // [notier1]
    if (emscripten_is_main_runtime_thread()) {
        NoteGpuTextureDestroy(texture);InvalidateGpuBindGroupsForTexture(texture);InvalidateGpuTextureViews(texture);wgpuTextureDestroy(texture); wgpuTextureRelease(texture); return true;
    }
    auto* queue = TextureQueue();
    struct WaitNote { double started; ~WaitNote() { NoteTextureWait(false, emscripten_get_now() - started); } } waitNote{emscripten_get_now()};
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) {
            auto texture = static_cast<WGPUTexture>(argument);
            DrainDeferredGpuWork(); // perf12: never destroy ahead of an earlier asynchronous submission
            NoteGpuTextureDestroy(texture);InvalidateGpuBindGroupsForTexture(texture);InvalidateGpuTextureViews(texture);wgpuTextureDestroy(texture); wgpuTextureRelease(texture);
        }, texture);
}
namespace {
GpuWgslBinding ParseGpuWgslBinding(const char* wgsl, std::size_t bytes, std::uint32_t binding) {
    GpuWgslBinding result;
    if (!wgsl || !bytes) return result;
    char needle[32];
    const int length = std::snprintf(needle, sizeof(needle), "@binding(%u)", unsigned(binding));
    if (length <= 0 || std::size_t(length) >= sizeof(needle)) return result;
    const char* end = wgsl + bytes;
    const char* p = std::search(wgsl, end, needle, needle + length);
    if (p == end) return result;
    p += length;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (p < end && space(*p)) ++p;
    // "var name: type;" only; var<uniform>/var<storage> are buffers.
    if (end - p < 4 || std::strncmp(p, "var", 3) != 0 || !space(p[3])) return result;
    const char* semicolon = std::find(p, end, ';');
    const char* colon = std::find(p, semicolon, ':');
    if (colon == semicolon) return result;
    p = colon + 1;
    while (p < semicolon && space(*p)) ++p;
    const char* typeEnd = semicolon;
    while (typeEnd > p && space(typeEnd[-1])) --typeEnd;
    const std::string type(p, typeEnd);
    if (type == "sampler") { result.kind = GpuWgslBinding_Sampler; return result; }
    if (type == "sampler_comparison") { result.kind = GpuWgslBinding_ComparisonSampler; return result; }
    static const struct { const char* name; WGPUTextureViewDimension dimension; } depthTypes[] = {
        {"texture_depth_2d", WGPUTextureViewDimension_2D}, {"texture_depth_2d_array", WGPUTextureViewDimension_2DArray},
        {"texture_depth_cube", WGPUTextureViewDimension_Cube}, {"texture_depth_cube_array", WGPUTextureViewDimension_CubeArray},
    };
    for (const auto& depth : depthTypes)
        if (type == depth.name) { result.kind = GpuWgslBinding_Depth; result.dimension = depth.dimension; return result; }
    static const struct { const char* prefix; WGPUTextureViewDimension dimension; } sampledTypes[] = {
        {"texture_1d<", WGPUTextureViewDimension_1D}, {"texture_2d<", WGPUTextureViewDimension_2D},
        {"texture_2d_array<", WGPUTextureViewDimension_2DArray}, {"texture_cube<", WGPUTextureViewDimension_Cube},
        {"texture_cube_array<", WGPUTextureViewDimension_CubeArray}, {"texture_3d<", WGPUTextureViewDimension_3D},
    };
    for (const auto& sampled : sampledTypes) {
        const std::size_t prefix = std::strlen(sampled.prefix);
        if (type.compare(0, prefix, sampled.prefix) != 0) continue;
        const std::string component = type.substr(prefix);
        if (component == "f32>") result.kind = GpuWgslBinding_Float;
        else if (component == "u32>") result.kind = GpuWgslBinding_Uint;
        else if (component == "i32>") result.kind = GpuWgslBinding_Sint;
        else return result;
        result.dimension = sampled.dimension; return result;
    }
    return result;
}
std::mutex wgslBindingMutex;
std::map<std::pair<const char*, std::uint32_t>, GpuWgslBinding> wgslBindings;
// Zero stand-ins, rebuilt per device generation. Handles of an older generation are leaked on purpose:
// queued bind groups may still name them, and a lost device makes them inert.
std::mutex defaultResourceMutex;
struct DefaultResources {
    std::uint64_t generation = 0;
    WGPUTexture textures[4][3]{};          // [kind][shape: layered 2D x6, 3D, 1D]
    WGPUTextureView views[4][7]{};         // [kind][WGPUTextureViewDimension]
    WGPUSampler samplers[2]{};             // [comparison]
} defaultResources;
void ResetDefaultResourcesLocked(std::uint64_t generation) {
    if (defaultResources.generation == generation) return;
    defaultResources = DefaultResources{}; defaultResources.generation = generation;
}
}
GpuWgslBinding FindGpuWgslBinding(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding) {
    if (!wgsl) return {};
    // Per-thread direct-mapped front: draws ask this for every engine-null SRV, so hits take no lock.
    struct Slot { const char* wgsl; std::uint32_t binding; GpuWgslBinding value; };
    static thread_local Slot recent[512]{};
    auto& slot = recent[((reinterpret_cast<std::uintptr_t>(wgsl) >> 4) ^ (std::uintptr_t(binding) * 2654435761u)) & 511];
    if (slot.wgsl == wgsl && slot.binding == binding) return slot.value;
    GpuWgslBinding parsed;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(wgslBindingMutex);
        auto entry = wgslBindings.find({wgsl, binding});
        if (entry != wgslBindings.end()) { parsed = entry->second; found = true; }
    }
    if (!found) {
        parsed = ParseGpuWgslBinding(wgsl, wgslBytes, binding);
        std::lock_guard<std::mutex> lock(wgslBindingMutex);
        if (wgslBindings.size() < 65536) wgslBindings.emplace(std::make_pair(wgsl, binding), parsed);
    }
    slot = {wgsl, binding, parsed};
    return parsed;
}
WGPUTextureView GetGpuDefaultTextureView(const GpuWgslBinding& binding) {
    unsigned kind;
    WGPUTextureFormat format;
    switch (binding.kind) {
    case GpuWgslBinding_Float: kind = 0; format = WGPUTextureFormat_RGBA8Unorm; break; // float + unfilterable-float
    case GpuWgslBinding_Uint: kind = 1; format = WGPUTextureFormat_RGBA32Uint; break;
    case GpuWgslBinding_Sint: kind = 2; format = WGPUTextureFormat_RGBA32Sint; break;
    case GpuWgslBinding_Depth: kind = 3; format = WGPUTextureFormat_Depth32Float; break;
    default: return nullptr;
    }
    unsigned shape, layers = 1;
    switch (binding.dimension) {
    case WGPUTextureViewDimension_2D: shape = 0; break;
    case WGPUTextureViewDimension_2DArray: case WGPUTextureViewDimension_Cube:
    case WGPUTextureViewDimension_CubeArray: shape = 0; layers = 6; break;
    case WGPUTextureViewDimension_3D: shape = 1; break;
    case WGPUTextureViewDimension_1D: shape = 2; break;
    default: return nullptr;
    }
    if (kind == 3 && shape != 0) return nullptr;
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const std::uint64_t generation = GpuDeviceGeneration();
    // Per-thread copy of published views: hits on the draw path take no lock.
    static thread_local struct { std::uint64_t generation = 0; WGPUTextureView views[4][7]{}; } local;
    if (local.generation != generation) { local = {}; local.generation = generation; }
    auto& localView = local.views[kind][unsigned(binding.dimension)];
    if (localView) return localView;
    // Creation proxies synchronously to the main thread, which never takes this mutex.
    std::lock_guard<std::mutex> lock(defaultResourceMutex);
    ResetDefaultResourcesLocked(generation);
    auto& view = defaultResources.views[kind][unsigned(binding.dimension)];
    if (view) return localView = view;
    auto& texture = defaultResources.textures[kind][shape];
    if (!texture) {
        // WebGPU zero-initializes the contents; nothing is uploaded.
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = shape == 1 ? WGPUTextureDimension_3D : shape == 2 ? WGPUTextureDimension_1D : WGPUTextureDimension_2D;
        desc.format = format; desc.size = {1, 1, shape == 0 ? 6u : 1u};
        desc.mipLevelCount = 1; desc.sampleCount = 1; desc.usage = WGPUTextureUsage_TextureBinding;
        desc.label = {"CP2077 default zero texture", WGPU_STRLEN};
        texture = CreateGpuTexture(desc);
        if (!texture) return nullptr;
    }
    WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    viewDesc.format = format; viewDesc.dimension = binding.dimension;
    viewDesc.baseMipLevel = 0; viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0; viewDesc.arrayLayerCount = layers;
    viewDesc.aspect = WGPUTextureAspect_All;
    view = CreateGpuTextureView(texture, viewDesc);
    return localView = view;
}
WGPUSampler GetGpuDefaultSampler(bool comparison) {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const std::uint64_t generation = GpuDeviceGeneration();
    static thread_local struct { std::uint64_t generation = 0; WGPUSampler samplers[2]{}; } local;
    if (local.generation != generation) { local = {}; local.generation = generation; }
    auto& localSampler = local.samplers[comparison ? 1 : 0];
    if (localSampler) return localSampler;
    std::lock_guard<std::mutex> lock(defaultResourceMutex);
    ResetDefaultResourcesLocked(generation);
    auto& sampler = defaultResources.samplers[comparison ? 1 : 0];
    if (sampler) return localSampler = sampler;
    // Nearest filtering binds to both filtering and non-filtering layout entries.
    WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
    desc.addressModeU = desc.addressModeV = desc.addressModeW = WGPUAddressMode_ClampToEdge;
    desc.magFilter = desc.minFilter = WGPUFilterMode_Nearest;
    desc.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    if (comparison) desc.compare = WGPUCompareFunction_LessEqual;
    desc.label = {"CP2077 default sampler", WGPU_STRLEN};
    sampler = CreateGpuSampler(desc);
    return localSampler = sampler;
}
WGPUSampler GetGpuDefaultLinearSampler() {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const std::uint64_t generation = GpuDeviceGeneration();
    static thread_local struct { std::uint64_t generation = 0; WGPUSampler sampler = nullptr; } local;
    if (local.generation == generation && local.sampler) return local.sampler;
    static std::mutex mutex; static std::uint64_t sharedGeneration = 0; static WGPUSampler shared = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    if (sharedGeneration != generation || !shared) { // older generations are leaked, like the other defaults
        WGPUSamplerDescriptor desc = WGPU_SAMPLER_DESCRIPTOR_INIT;
        desc.addressModeU = desc.addressModeV = desc.addressModeW = WGPUAddressMode_ClampToEdge;
        desc.magFilter = desc.minFilter = WGPUFilterMode_Linear;
        desc.mipmapFilter = WGPUMipmapFilterMode_Linear;
        desc.label = {"CP2077 default linear sampler", WGPU_STRLEN};
        shared = CreateGpuSampler(desc); sharedGeneration = generation;
    }
    local.generation = generation; local.sampler = shared;
    return shared;
}
WGPUTextureView GetGpuScratchStorageTextureView(WGPUTextureFormat format, WGPUTextureViewDimension dimension,
                                                unsigned index) {
    if (format == WGPUTextureFormat_Undefined || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    if (dimension != WGPUTextureViewDimension_2D && dimension != WGPUTextureViewDimension_2DArray &&
        dimension != WGPUTextureViewDimension_3D) return nullptr;
    if (index >= 32) return nullptr; // [potato] r6 bounded: no shader has that many UAV slots
    const std::uint64_t generation = GpuDeviceGeneration();
    // Same lifetime rule as the zero stand-ins: older generations are leaked, never released.
    static std::uint64_t scratchGeneration = 0;
    static std::map<std::pair<std::pair<WGPUTextureFormat, WGPUTextureViewDimension>, unsigned>, WGPUTextureView> scratchViews;
    std::lock_guard<std::mutex> lock(defaultResourceMutex);
    if (scratchGeneration != generation) { scratchViews.clear(); scratchGeneration = generation; }
    auto& view = scratchViews[{{format, dimension}, index}];
    if (view) return view;
    WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
    desc.dimension = dimension == WGPUTextureViewDimension_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    desc.format = format; desc.size = {1, 1, 1};
    desc.mipLevelCount = 1; desc.sampleCount = 1; desc.usage = WGPUTextureUsage_StorageBinding;
    desc.label = {"CP2077 scratch storage texture", WGPU_STRLEN};
    auto texture = CreateGpuTexture(desc);
    if (!texture) return nullptr;
    WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    viewDesc.format = format; viewDesc.dimension = dimension;
    viewDesc.baseMipLevel = 0; viewDesc.mipLevelCount = 1;
    viewDesc.baseArrayLayer = 0; viewDesc.arrayLayerCount = 1;
    viewDesc.aspect = WGPUTextureAspect_All; viewDesc.usage = WGPUTextureUsage_StorageBinding;
    view = CreateGpuTextureView(texture, viewDesc);
    return view;
}
}
#include <vector>
// ---------------------------------------------------------------------------------------------------------------
// [textures] Emulated t130 bindless table for multilayered pixel shaders (see gpu-texture-web.h).
// The converted WGSL samples cp2077_ml_pool_<slot> (texture_2d_array) at the layer the table gives for the
// engine's bindless index. Chunks are exact (format, width, height, mips) classes, so LOD selection, wrapping and
// filtering are those of the original texture. All GPU objects are created/written on the main thread.
namespace {
struct MlChunk {
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    std::uint32_t width = 0, height = 0, mips = 0, capacity = 0, used = 0;
    WGPUTexture texture = nullptr; WGPUTextureView view = nullptr;
    std::vector<std::uint16_t> freeLayers;
};
struct MlEntry {
    WGPUTexture source = nullptr; unsigned chunk = 0; std::uint16_t layer = 0;
    std::uint32_t blockWidth = 1, blockHeight = 1; bool dirty = false;
    // Encodes left that re-copy this entry: the writer's list may be submitted after the first refreshing list.
    unsigned dirtyEncodes = 0;
    bool debugZero = false; // [whitewall] r6 ?texdebug=1: tiny (<= 4x4) layer zeroed, never re-copied
};
// [whitewall] r6 heap-free pool counters (read by the multilayer fallback probe in gpu-api-texture-web.cpp).
std::atomic<unsigned> mlCountPooled{0}, mlCountTinyPooled{0}, mlCountSameSource{0}, mlCountReplaced{0},
    mlCountRewrites{0}, mlCountRefreshCopies{0}, mlCountDebugZeroed{0};
int MlTinyTextureDebug() {
    static const int value = MAIN_THREAD_EM_ASM_INT({
        const q = new URLSearchParams(location.search).get('texdebug');
        return (q && q !== '0' && q !== 'false') ? 1 : 0;
    });
    return value;
}
struct MlState {
    std::mutex mutex;
    std::uint64_t generation = ~0ull;
    MlChunk chunks[cp2077::web::kGpuMultilayerPoolSlots];
    unsigned chunkCount = 0;
    WGPUBuffer table = nullptr;
    WGPUTextureView dummy = nullptr;
    std::unordered_map<std::uint16_t, MlEntry> entries;
    std::unordered_map<std::uint16_t, const char*> rejects; // [wcfix] last reject reason per index (static strings)
    unsigned pooled = 0, rejected = 0;
};
MlState& Ml() { static MlState state; return state; }
// Pool/dummy views get a known parent so bind groups that contain them are cacheable
// (gpu-bind-group-cache-web.inl skips groups with a view of unknown parent). ?mlBgCache=0 disables (A/B).
void MlRegisterViewParent(WGPUTextureView view, WGPUTexture texture) {
    static const int enabled = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('mlBgCache') === '0' ? 0 : 1; });
    if (!enabled || !view || !texture) return;
    std::lock_guard<std::recursive_mutex> guard(textureViewMutex);
    SetViewParentLocked(view, texture); // [skperf] keeps the reverse index
}
std::atomic<unsigned> mlDirtyCount{0};
std::atomic<bool> mlAnyPooled{false};
// Caller holds state.mutex, main thread. Resets on a new device generation (old objects are leaked, as for the
// other device-lifetime stand-ins) and creates the table and the dummy array view.
bool EnsureMlBasicsOnMain(MlState& state) {
    const auto generation = cp2077::web::GpuDeviceGeneration();
    auto device = cp2077::web::MainThreadGpuDevice();
    if (!device) return false;
    if (state.generation != generation) {
        for (auto& chunk : state.chunks) chunk = MlChunk{};
        state.chunkCount = 0; state.table = nullptr; state.dummy = nullptr; state.entries.clear(); state.rejects.clear();
        mlDirtyCount = 0; state.generation = generation;
    }
    if (!state.table) {
        WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        desc.size = cp2077::web::kGpuMultilayerTableBytes;
        desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
        desc.label = {"CP2077 multilayer bindless table", WGPU_STRLEN};
        state.table = wgpuDeviceCreateBuffer(device, &desc); // zero-initialized: every index unmapped
    }
    if (!state.dummy) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = WGPUTextureDimension_2D; desc.format = WGPUTextureFormat_RGBA8Unorm;
        desc.size = {1, 1, 1}; desc.mipLevelCount = 1; desc.sampleCount = 1;
        desc.usage = WGPUTextureUsage_TextureBinding;
        desc.label = {"CP2077 multilayer empty pool", WGPU_STRLEN};
        if (auto texture = wgpuDeviceCreateTexture(device, &desc)) {
            WGPUTextureViewDescriptor view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            view.format = desc.format; view.dimension = WGPUTextureViewDimension_2DArray;
            view.baseMipLevel = 0; view.mipLevelCount = 1; view.baseArrayLayer = 0; view.arrayLayerCount = 1;
            state.dummy = wgpuTextureCreateView(texture, &view);
            MlRegisterViewParent(state.dummy, texture);
        }
    }
    return state.table && state.dummy;
}
std::uint32_t MlMipExtent(std::uint32_t size, std::uint32_t mip, std::uint32_t block) {
    std::uint32_t value = std::max<std::uint32_t>(1u, size >> mip);
    return (value + block - 1) / block * block;
}
void EncodeMlCopy(WGPUCommandEncoder encoder, const MlChunk& chunk, const MlEntry& entry) {
    for (std::uint32_t mip = 0; mip < chunk.mips; ++mip) {
        WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = entry.source; from.mipLevel = mip; from.origin = {0, 0, 0};
        to.texture = chunk.texture; to.mipLevel = mip; to.origin = {0, 0, entry.layer};
        const WGPUExtent3D extent{MlMipExtent(chunk.width, mip, entry.blockWidth), MlMipExtent(chunk.height, mip, entry.blockHeight), 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
    }
}
void MlWriteTable(MlState& state, std::uint16_t index, std::uint32_t value) {
    if (auto queue = cp2077::web::MainThreadGpuQueue())
        wgpuQueueWriteBuffer(queue, state.table, std::uint64_t(index) * 4, &value, sizeof(value));
}
void MlChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type, WGPUStringView message, void*, void*) {
    if (status != WGPUPopErrorScopeStatus_Success || type == WGPUErrorType_NoError) return;
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8)
        std::fprintf(stderr, "CP2077 multilayer pool GPU error: type=%u %.*s\n", unsigned(type),
            int(message.length == WGPU_STRLEN ? 256 : std::min<std::size_t>(message.length, 512)), message.data ? message.data : "");
}
bool IsMlPoolableFormat(WGPUTextureFormat format) {
    switch (format) {
    case WGPUTextureFormat_BC1RGBAUnorm: case WGPUTextureFormat_BC1RGBAUnormSrgb:
    case WGPUTextureFormat_BC2RGBAUnorm: case WGPUTextureFormat_BC2RGBAUnormSrgb:
    case WGPUTextureFormat_BC3RGBAUnorm: case WGPUTextureFormat_BC3RGBAUnormSrgb:
    case WGPUTextureFormat_BC4RUnorm: case WGPUTextureFormat_BC4RSnorm:
    case WGPUTextureFormat_BC5RGUnorm: case WGPUTextureFormat_BC5RGSnorm:
    case WGPUTextureFormat_BC6HRGBUfloat: case WGPUTextureFormat_BC6HRGBFloat:
    case WGPUTextureFormat_BC7RGBAUnorm: case WGPUTextureFormat_BC7RGBAUnormSrgb:
    case WGPUTextureFormat_RGBA8Unorm: case WGPUTextureFormat_RGBA8UnormSrgb: case WGPUTextureFormat_RGBA8Snorm:
    case WGPUTextureFormat_BGRA8Unorm: case WGPUTextureFormat_BGRA8UnormSrgb:
    case WGPUTextureFormat_R8Unorm: case WGPUTextureFormat_RG8Unorm: case WGPUTextureFormat_R8Snorm: case WGPUTextureFormat_RG8Snorm:
    case WGPUTextureFormat_R16Float: case WGPUTextureFormat_RG16Float: case WGPUTextureFormat_RGBA16Float:
    case WGPUTextureFormat_RG11B10Ufloat: case WGPUTextureFormat_RGB10A2Unorm: case WGPUTextureFormat_RGB9E5Ufloat:
        return true;
    default: return false;
    }
}
struct MlPoolRequest {
    std::uint16_t index; WGPUTexture source; WGPUTextureFormat format;
    std::uint32_t width, height, mips, blockWidth, blockHeight, blockBytes;
    bool ok = false; const char* reason = "unknown"; unsigned chunk = 0; std::uint16_t layer = 0;
};
void PoolOnMain(void* argument) {
    auto& request = *static_cast<MlPoolRequest*>(argument);
    auto& state = Ml();
    cp2077::web::DrainDeferredGpuWork(); // after earlier asynchronous submissions (source contents)
    std::lock_guard<std::mutex> lock(state.mutex);
    auto device = cp2077::web::MainThreadGpuDevice();
    auto queue = cp2077::web::MainThreadGpuQueue();
    if (!device || !queue || !EnsureMlBasicsOnMain(state)) { request.reason = "device"; return; }
    auto existing = state.entries.find(request.index);
    if (existing != state.entries.end()) {
        if (existing->second.source == request.source) {
            // [whitewall] r6: a re-offer only happens when the index is now claimed by another engine TextureRef
            // (GetTextureIndirection pools when multilayerPooledIds[index] != ref). Same WGPUTexture handle means an
            // aliasing ref or a recycled handle after destroy/recreate; the layer may hold the old contents, so
            // refresh it in list order at the next multilayer draws. ?mlRefresh=0 = old (keep the layer as is).
            ++mlCountSameSource;
            static const int refresh = MAIN_THREAD_EM_ASM_INT({
                return new URLSearchParams(location.search).get('mlRefresh') === '0' ? 0 : 1; });
            if (refresh && !existing->second.debugZero) {
                existing->second.dirtyEncodes = 4;
                if (!existing->second.dirty) { existing->second.dirty = true; ++mlDirtyCount; }
            }
            request.ok = true; request.chunk = existing->second.chunk; request.layer = existing->second.layer; return;
        }
        ++mlCountReplaced;
        // The index now names another texture: release the old layer first.
        state.chunks[existing->second.chunk].freeLayers.push_back(existing->second.layer);
        if (existing->second.dirty) --mlDirtyCount;
        state.entries.erase(existing);
        MlWriteTable(state, request.index, 0);
    }
    unsigned slot = state.chunkCount;
    for (unsigned i = 0; i < state.chunkCount; ++i) {
        const auto& chunk = state.chunks[i];
        if (chunk.format == request.format && chunk.width == request.width && chunk.height == request.height &&
            chunk.mips == request.mips && (!chunk.freeLayers.empty() || chunk.used < chunk.capacity)) { slot = i; break; }
    }
    if (slot == state.chunkCount) {
        if (state.chunkCount >= cp2077::web::kGpuMultilayerPoolSlots) { request.reason = "all pool slots in use"; return; }
        WGPULimits limits = WGPU_LIMITS_INIT;
        std::uint32_t maxLayers = 256;
        if (wgpuDeviceGetLimits(device, &limits) == WGPUStatus_Success && limits.maxTextureArrayLayers)
            maxLayers = std::min<std::uint32_t>(limits.maxTextureArrayLayers, 256);
        std::uint64_t layerBytes = 0;
        for (std::uint32_t mip = 0; mip < request.mips; ++mip)
            layerBytes += std::uint64_t(MlMipExtent(request.width, mip, request.blockWidth) / request.blockWidth) *
                          (MlMipExtent(request.height, mip, request.blockHeight) / request.blockHeight) * request.blockBytes;
        // About 24 MiB per chunk: a class with more textures than fit takes another slot.
        std::uint64_t capacity = layerBytes ? (24ull << 20) / layerBytes : 1;
        capacity = std::min<std::uint64_t>(std::max<std::uint64_t>(capacity, 4), maxLayers);
        auto& chunk = state.chunks[slot];
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = WGPUTextureDimension_2D; desc.format = request.format;
        desc.size = {request.width, request.height, std::uint32_t(capacity)};
        desc.mipLevelCount = request.mips; desc.sampleCount = 1;
        desc.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        desc.label = {"CP2077 multilayer pool", WGPU_STRLEN};
        wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
        chunk.texture = wgpuDeviceCreateTexture(device, &desc);
        WGPUPopErrorScopeCallbackInfo checked = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        checked.mode = WGPUCallbackMode_AllowSpontaneous; checked.callback = MlChecked;
        wgpuDevicePopErrorScope(device, checked);
        if (!chunk.texture) { chunk = MlChunk{}; request.reason = "pool texture"; return; }
        WGPUTextureViewDescriptor view = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        view.format = request.format; view.dimension = WGPUTextureViewDimension_2DArray;
        view.baseMipLevel = 0; view.mipLevelCount = request.mips;
        view.baseArrayLayer = 0; view.arrayLayerCount = std::uint32_t(capacity);
        chunk.view = wgpuTextureCreateView(chunk.texture, &view);
        MlRegisterViewParent(chunk.view, chunk.texture);
        chunk.format = request.format; chunk.width = request.width; chunk.height = request.height;
        chunk.mips = request.mips; chunk.capacity = std::uint32_t(capacity); chunk.used = 0;
        ++state.chunkCount;
        std::fprintf(stderr, "CP2077 multilayer pool chunk: slot=%u format=%u size=%ux%u mips=%u layers=%u layerBytes=%llu\n",
            slot, unsigned(request.format), request.width, request.height, request.mips, chunk.capacity,
            static_cast<unsigned long long>(layerBytes));
    }
    auto& chunk = state.chunks[slot];
    std::uint16_t layer;
    if (!chunk.freeLayers.empty()) { layer = chunk.freeLayers.back(); chunk.freeLayers.pop_back(); }
    else layer = std::uint16_t(chunk.used++);
    MlEntry entry; entry.source = request.source; entry.chunk = slot; entry.layer = layer;
    entry.blockWidth = request.blockWidth; entry.blockHeight = request.blockHeight;
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    auto encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    EncodeMlCopy(encoder, chunk, entry);
    auto commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands); wgpuCommandEncoderRelease(encoder);
    WGPUPopErrorScopeCallbackInfo checked = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    checked.mode = WGPUCallbackMode_AllowSpontaneous; checked.callback = MlChecked;
    wgpuDevicePopErrorScope(device, checked);
    // Contents uploaded through a recorded but not yet submitted list are not visible to the copy above: refresh
    // once more in list order at the next multilayer draws (EncodeGpuMultilayerDirtyCopies).
    ++mlCountPooled;
    const bool tiny = request.width <= 4 && request.height <= 4;
    if (tiny) ++mlCountTinyPooled;
    if (tiny && MlTinyTextureDebug()) {
        // [whitewall] r6 ?texdebug=1: zero this tiny layer (all mips) so multilayer surfaces built from placeholder
        // layer textures turn black; the entry is never re-copied from its source.
        std::vector<unsigned char> zeros;
        for (std::uint32_t mip = 0; mip < request.mips; ++mip) {
            const std::uint32_t w = MlMipExtent(request.width, mip, request.blockWidth), h = MlMipExtent(request.height, mip, request.blockHeight);
            const std::uint32_t rowBytes = (w / request.blockWidth) * request.blockBytes, rows = h / request.blockHeight;
            zeros.assign(std::size_t(rowBytes) * rows, 0);
            WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            to.texture = chunk.texture; to.mipLevel = mip; to.origin = {0, 0, layer};
            WGPUTexelCopyBufferLayout dataLayout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            dataLayout.bytesPerRow = rowBytes; dataLayout.rowsPerImage = rows;
            const WGPUExtent3D extent{w, h, 1};
            wgpuQueueWriteTexture(queue, &to, zeros.data(), zeros.size(), &dataLayout, &extent);
        }
        entry.debugZero = true;
        const unsigned zeroed = ++mlCountDebugZeroed;
        if (zeroed <= 32) std::fprintf(stderr, "CP2077 whitewall: texdebug zeroed multilayer layer index=%u format=%u size=%ux%u mips=%u\n",
            unsigned(request.index), unsigned(request.format), request.width, request.height, request.mips);
    } else { entry.dirty = true; entry.dirtyEncodes = 4; ++mlDirtyCount; }
    state.entries[request.index] = entry;
    MlWriteTable(state, request.index, ((slot + 1u) << 16) | layer);
    ++state.pooled; mlAnyPooled = true;
    request.ok = true; request.chunk = slot; request.layer = layer;
}
struct MlIndexRequest { std::uint16_t index; WGPUTexture source; };
void UnpoolOnMain(void* argument) {
    const auto index = static_cast<MlIndexRequest*>(argument)->index;
    const auto source = static_cast<MlIndexRequest*>(argument)->source;
    // Earlier asynchronously queued lists may still read this index/layer: run them first (never under state.mutex,
    // their encodes take it for dirty copies), as DestroyGpuTexture does before destroying the source.
    cp2077::web::DrainDeferredGpuWork();
    auto& state = Ml();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.generation != cp2077::web::GpuDeviceGeneration()) return;
    auto found = state.entries.find(index);
    if (found == state.entries.end() || found->second.source != source) return;
    state.chunks[found->second.chunk].freeLayers.push_back(found->second.layer);
    if (found->second.dirty) --mlDirtyCount;
    state.entries.erase(found);
    MlWriteTable(state, index, 0);
}
void EnsureMlBasicsProxy(void* argument) {
    auto& state = Ml();
    std::lock_guard<std::mutex> lock(state.mutex);
    *static_cast<bool*>(argument) = EnsureMlBasicsOnMain(state);
}
bool RunMlOnMain(void (*function)(void*), void* argument) {
    if (emscripten_is_main_runtime_thread()) { function(argument); return true; }
    auto* queue = TextureQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), function, argument);
}
}
namespace cp2077::web {
bool PoolGpuMultilayerTexture(std::uint16_t index, WGPUTexture source, WGPUTextureFormat format,
    std::uint32_t width, std::uint32_t height, std::uint32_t mips,
    std::uint32_t blockWidth, std::uint32_t blockHeight, std::uint32_t blockBytes) {
    if (!source || !IsGpuDeviceReady() || !width || !height || !mips || !blockWidth || !blockHeight || !blockBytes) return false;
    // A/B switch: ?mlPools=0 leaves every index unmapped (the shaders read their neutral fallback).
    static const int enabled = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('mlPools') === '0' ? 0 : 1; });
    static std::atomic<bool> announced{false};
    if (!announced.exchange(true)) std::fprintf(stderr, "CP2077 multilayer pools: enabled=%d (?mlPools=0 disables)\n", enabled);
    if (!enabled) return false;
    MlPoolRequest request{index, source, format, width, height, mips, blockWidth, blockHeight, blockBytes};
    if (!IsMlPoolableFormat(format)) request.reason = "format";
    else if (!RunMlOnMain(PoolOnMain, &request)) request.reason = "proxy";
    auto& state = Ml();
    static std::atomic<unsigned> reports{0};
    const unsigned count = reports.fetch_add(1) + 1;
    {
        // [wcfix] remember the last reject reason per index for the multilayer fallback audit
        std::lock_guard<std::mutex> lock(state.mutex);
        if (request.ok) state.rejects.erase(index); else state.rejects[index] = request.reason;
    }
    if (!request.ok) {
        ++state.rejected;
        // [wcfix] rejects have their own counter (they shared the success counter, so late rejects went unlogged)
        static std::atomic<unsigned> rejectReports{0};
        const unsigned rejectCount = rejectReports.fetch_add(1) + 1;
        if (rejectCount <= 32 || !(rejectCount & (rejectCount - 1)))
            std::fprintf(stderr, "CP2077 multilayer pool rejected: index=%u format=%u size=%ux%u mips=%u reason=%s\n",
                unsigned(index), unsigned(format), width, height, mips, request.reason);
    } else if (count <= 16 || !(count & (count - 1))) {
        std::fprintf(stderr, "CP2077 multilayer pool texture: index=%u format=%u size=%ux%u mips=%u slot=%u layer=%u pooled=%u rejected=%u\n",
            unsigned(index), unsigned(format), width, height, mips, request.chunk, unsigned(request.layer), state.pooled, state.rejected);
    }
    return request.ok;
}
void UnpoolGpuMultilayerIndex(std::uint16_t index, WGPUTexture source) {
    if (!mlAnyPooled.load(std::memory_order_relaxed)) return;
    {
        auto& state = Ml();
        std::lock_guard<std::mutex> lock(state.mutex);
        auto found = state.entries.find(index);
        if (found == state.entries.end() || found->second.source != source) return;
    }
    MlIndexRequest request{index, source};
    RunMlOnMain(UnpoolOnMain, &request);
}
void NoteGpuMultilayerSourceWrite(WGPUTexture source) {
    if (!source || !mlAnyPooled.load(std::memory_order_relaxed)) return;
    auto& state = Ml();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& [index, entry] : state.entries)
        if (entry.source == source) {
            if (entry.debugZero) continue; // [whitewall] r6 texdebug layer stays zero
            ++mlCountRewrites;
            entry.dirtyEncodes = 4;
            if (entry.dirty) continue;
            entry.dirty = true; ++mlDirtyCount;
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1) < 16) std::fprintf(stderr, "CP2077 multilayer pool source rewritten: index=%u\n", unsigned(index));
        }
}
bool HasGpuMultilayerDirtyCopies() { return mlDirtyCount.load(std::memory_order_relaxed) != 0; }
// [whitewall] r6: declared locally in gpu-api-texture-web.cpp (keeps gpu-texture-web.h unchanged).
void GetGpuMultilayerPoolCounters(unsigned (&out)[8]) {
    out[0] = mlCountPooled.load(); out[1] = mlCountTinyPooled.load(); out[2] = mlCountSameSource.load();
    out[3] = mlCountReplaced.load(); out[4] = mlCountRewrites.load(); out[5] = mlCountRefreshCopies.load();
    out[6] = mlCountDebugZeroed.load(); out[7] = mlDirtyCount.load();
}
bool EncodeGpuMultilayerDirtyCopies(WGPUCommandEncoder encoder) {
    if (!encoder) return false;
    auto& state = Ml();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.generation != GpuDeviceGeneration()) return true;
    for (auto& [index, entry] : state.entries) {
        if (!entry.dirty) continue;
        EncodeMlCopy(encoder, state.chunks[entry.chunk], entry);
        ++mlCountRefreshCopies; // [whitewall] r6
        if (entry.dirtyEncodes) --entry.dirtyEncodes;
        if (!entry.dirtyEncodes) { entry.dirty = false; --mlDirtyCount; }
    }
    return true;
}
bool GetGpuMultilayerPoolBindings(WGPUTextureView (&views)[kGpuMultilayerPoolSlots], WGPUBuffer& table) {
    auto& state = Ml();
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.generation == GpuDeviceGeneration() && state.table && state.dummy) {
                for (unsigned slot = 0; slot < kGpuMultilayerPoolSlots; ++slot)
                    views[slot] = slot < state.chunkCount && state.chunks[slot].view ? state.chunks[slot].view : state.dummy;
                table = state.table;
                return true;
            }
        }
        bool ready = false;
        if (!RunMlOnMain(EnsureMlBasicsProxy, &ready) || !ready) return false;
    }
    return false;
}
unsigned GetGpuMultilayerIndexStates(const std::uint16_t* ids, unsigned count, std::uint8_t* states,
    const char** rejectReasons) {
    auto& state = Ml();
    std::lock_guard<std::mutex> lock(state.mutex);
    const bool current = state.generation == GpuDeviceGeneration();
    for (unsigned i = 0; i < count; ++i) {
        states[i] = 0;
        if (rejectReasons) rejectReasons[i] = nullptr;
        if (!current) continue;
        const auto found = state.entries.find(ids[i]);
        if (found != state.entries.end()) { states[i] = found->second.chunk >= 37 ? 2 : 1; continue; }
        const auto rejected = state.rejects.find(ids[i]);
        if (rejected != state.rejects.end()) { states[i] = 3; if (rejectReasons) rejectReasons[i] = rejected->second; }
    }
    return current ? state.chunkCount : 0;
}
}
