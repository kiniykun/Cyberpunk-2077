namespace {
struct BrowserTextureRegion {
    GpuApi::TextureRef reference = GpuApi::TextureRef::Null();
    GpuApi::TextureDesc descriptor;
    Format format{};
    WGPUTexelCopyTextureInfo texture = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUExtent3D extent{};
    Uint32 rowBytes = 0, pitch = 0, rows = 0;
    Uint64 bytes = 0;
    ~BrowserTextureRegion() { if (!reference.isNull()) GpuApi::Release(reference); }
    bool Acquire(GpuApi::TextureRef ref, Uint32 mip, Uint32 slice, const Rect* rect) {
        {
            std::lock_guard<std::mutex> lock(textureMutex);
            const auto found = textures.find(ref.ID());
            if (found == textures.end() || found->second->references == INT32_MAX) return false;
            descriptor = found->second->descriptor;
            format = TranslateResource(descriptor);
            if (!format.blockBytes || HasDepthPlane(descriptor) || Uint32(descriptor.sampleCount) != 0 ||
                mip >= descriptor.Internal_CalcTargetNumMips() || slice >= descriptor.CalcTargetSlices() ||
                descriptor.type == GpuWrapApi::TEXTYPE_3D) return false;
            ++found->second->references; reference = ref;
            texture.texture = found->second->texture;
        }
        const auto width = std::max(1u, Uint32(descriptor.width) >> mip);
        const auto height = std::max(1u, Uint32(descriptor.height) >> mip);
        Uint32 x = 0, y = 0, w = width, h = height;
        if (rect) {
            if (rect->m_left < 0 || rect->m_top < 0 || rect->m_right <= rect->m_left || rect->m_bottom <= rect->m_top ||
                Uint32(rect->m_right) > width || Uint32(rect->m_bottom) > height) return false;
            x = rect->m_left; y = rect->m_top; w = rect->m_right - rect->m_left; h = rect->m_bottom - rect->m_top;
        }
        if (x % format.blockSize || y % format.blockSize ||
            (w % format.blockSize && x + w != width) || (h % format.blockSize && y + h != height)) return false;
        texture.mipLevel = mip; texture.origin = {x,y,slice};
        texture.aspect = WGPUTextureAspect_All;
        rows = (h + format.blockSize - 1) / format.blockSize;
        rowBytes = ((w + format.blockSize - 1) / format.blockSize) * format.blockBytes;
        pitch = (rowBytes + 255) & ~Uint32(255);
        bytes = Uint64(pitch) * rows;
        // WebGPU compressed mip footprints use rounded physical block extents.
        extent = {((w + format.blockSize - 1) / format.blockSize) * format.blockSize,
            rows * format.blockSize, 1};
        return bytes <= std::numeric_limits<std::size_t>::max();
    }
    WGPUTexelCopyBufferInfo BufferCopy(WGPUBuffer buffer) const {
        WGPUTexelCopyBufferInfo result = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
        result.buffer = buffer; result.layout.bytesPerRow = pitch; result.layout.rowsPerImage = rows;
        return result;
    }
};
bool UploadBrowserTexture(BrowserTextureRegion& region, const void* source, Uint32 pitch) {
    std::vector<unsigned char> packed(static_cast<std::size_t>(region.bytes));
    const auto cooked = TranslateSource(region.descriptor.format);
    if (ConvertsTier1(cooked)) {
        // [notier1] the caller's bytes are in the tier1 format; the texture is its float fallback
        std::vector<unsigned char> texels; Uint32 rowBytes = 0;
        const Uint32 w = region.extent.width, h = region.extent.height;
        if (!source || !ConvertTier1Subresource(cooked, source, pitch, h, w, h, 1, texels, rowBytes) || rowBytes > region.pitch) return false;
        for (Uint32 row = 0; row < h; ++row)
            std::memcpy(packed.data() + std::size_t(row) * region.pitch, texels.data() + std::size_t(row) * rowBytes, rowBytes);
    } else if (cooked.blockSize != region.format.blockSize) {
        // [nobc] the caller's bytes are BC blocks (pitch = bytes per block row); the texture is uncompressed
        std::vector<unsigned char> texels; Uint32 rowBytes = 0;
        const Uint32 w = region.extent.width, h = region.extent.height, blockRows = (h + 3) / 4;
        if (!source || !DecodeBCSubresource(cooked, source, pitch, blockRows, w, h, 1, texels, rowBytes) || rowBytes > region.pitch) return false;
        for (Uint32 row = 0; row < h; ++row)
            std::memcpy(packed.data() + std::size_t(row) * region.pitch, texels.data() + std::size_t(row) * rowBytes, rowBytes);
    } else {
    if (!source || pitch < region.rowBytes) return false;
    for (Uint32 row = 0; row < region.rows; ++row)
        std::memcpy(packed.data() + std::size_t(row) * region.pitch,
            static_cast<const unsigned char*>(source) + std::size_t(row) * pitch, region.rowBytes);
    }
    // Pooled staging arena instead of a new validated buffer per upload (video frames upload every frame).
    if (!cp2077::web::RecordGpuTextureUploadFromArena(region.texture, packed.data(), packed.size(), region.pitch, region.rows,
            region.extent, region.reference)) return false;
    region.reference = GpuApi::TextureRef::Null(); return true;
}
bool IsPackedCompressionCopy(const GpuApi::TextureDesc& destination, const GpuApi::TextureDesc& source) {
    const auto index=CompressionIndex(destination.format);
    return index >= 0 && source.format == ((index == 0 || index == 2) ?
        GpuWrapApi::TEXFMT_R32G32_Uint : GpuWrapApi::TEXFMT_R32G32B32A32_Uint);
}
// 0: ordinary texture copy, 1: encoded native BC packing copy, -1: invalid.
Int32 TryPackedCompressionCopy(GpuApi::TextureRef destination, Uint32 dm, Uint32 dl,
    GpuApi::TextureRef source, Uint32 sm, Uint32 sl) {
    if (!IsPackedCompressionCopy(GpuApi::GetTextureDesc(destination),GpuApi::GetTextureDesc(source))) return 0;
    if (cp2077::web::GpuDecodesBC()) return -1; // [nobc] the destination is uncompressed; CompressTexture copies directly
    BrowserTextureRegion sourceRegion,destinationRegion;
    if (!sourceRegion.Acquire(source,sm,sl,nullptr) || !destinationRegion.Acquire(destination,dm,dl,nullptr) ||
        sourceRegion.extent.width * 4 != destinationRegion.extent.width ||
        sourceRegion.extent.height * 4 != destinationRegion.extent.height ||
        sourceRegion.rowBytes != destinationRegion.rowBytes || sourceRegion.rows != destinationRegion.rows ||
        sourceRegion.bytes != destinationRegion.bytes) return -1;
    // DX12 permits integer texels to be copied as BC blocks. Standard WebGPU
    // requires an explicit GPU buffer intermediate between different formats.
    // These are the exact shader-produced block words, without CPU re-encoding.
    const auto staging=cp2077::web::CreateGpuBuffer(sourceRegion.bytes,WGPUBufferUsage_CopySrc|WGPUBufferUsage_CopyDst,nullptr,0,"cp-texture-copy-staging");
    if (!staging) return -1;
    if (!cp2077::web::RecordGpuTextureBufferCopy(sourceRegion.texture,sourceRegion.BufferCopy(staging),
        sourceRegion.extent,sourceRegion.reference,false)) {
        cp2077::web::DestroyGpuBuffer(staging); return -1;
    }
    sourceRegion.reference=GpuApi::TextureRef::Null();
    if (!cp2077::web::RecordGpuTextureBufferCopy(destinationRegion.texture,destinationRegion.BufferCopy(staging),
        destinationRegion.extent,destinationRegion.reference,true)) {
        // Mark the list failed before destroying this now-unowned intermediate;
        // the earlier encoded copy must not submit with a destroyed buffer.
        cp2077::web::FailBoundGpuCommand(); cp2077::web::DestroyGpuBuffer(staging); return -1;
    }
    destinationRegion.reference=GpuApi::TextureRef::Null();
    return 1;
}
GpuApi::TextureRef CreateMorphUnormCopy(const GpuApi::TextureRef& source) {
    auto descriptor=GpuApi::GetTextureDesc(source);
    if (descriptor.format != GpuWrapApi::TEXFMT_R8G8B8A8_Uint_Typeless || descriptor.type != GpuWrapApi::TEXTYPE_2D ||
        descriptor.CalcTargetSlices() != 1) return GpuApi::TextureRef::Null();
    descriptor.format=GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
    descriptor.usage=GpuApi::TEXUSAGE_Samplable; descriptor.usageEx=0;
    descriptor.inPlaceType=GpuApi::INPLACE_None;
    auto copy=GpuApi::CreateTexture(descriptor);
    if (copy.isNull()) return copy;
    const Uint32 mips=descriptor.Internal_CalcTargetNumMips();
    for (Uint32 mip=0; mip<mips; ++mip) {
        BrowserTextureRegion sourceRegion,destinationRegion;
        if (!sourceRegion.Acquire(source,mip,0,nullptr) || !destinationRegion.Acquire(copy,mip,0,nullptr) ||
            sourceRegion.rowBytes != destinationRegion.rowBytes || sourceRegion.rows != destinationRegion.rows ||
            sourceRegion.bytes != destinationRegion.bytes) { GpuApi::Release(copy); return GpuApi::TextureRef::Null(); }
        // Same 4-byte texels: r32uint words -> buffer -> rgba8unorm bytes (DX12 typeless reinterpretation).
        const auto staging=cp2077::web::CreateGpuBuffer(sourceRegion.bytes,WGPUBufferUsage_CopySrc|WGPUBufferUsage_CopyDst,nullptr,0,"cp-morph-unorm-staging");
        if (!staging) { GpuApi::Release(copy); return GpuApi::TextureRef::Null(); }
        if (!cp2077::web::RecordGpuTextureBufferCopy(sourceRegion.texture,sourceRegion.BufferCopy(staging),
            sourceRegion.extent,sourceRegion.reference,false)) {
            cp2077::web::DestroyGpuBuffer(staging); GpuApi::Release(copy); return GpuApi::TextureRef::Null();
        }
        sourceRegion.reference=GpuApi::TextureRef::Null();
        if (!cp2077::web::RecordGpuTextureBufferCopy(destinationRegion.texture,destinationRegion.BufferCopy(staging),
            destinationRegion.extent,destinationRegion.reference,true)) {
            cp2077::web::FailBoundGpuCommand(); cp2077::web::DestroyGpuBuffer(staging);
            GpuApi::Release(copy); return GpuApi::TextureRef::Null();
        }
        destinationRegion.reference=GpuApi::TextureRef::Null();
    }
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1)<8) std::fprintf(stderr,"CP2077 morph texture UNORM copy for compression: src=%u copy=%u size=%ux%u mips=%u\n",
        source.ID(),copy.ID(),descriptor.width,descriptor.height,mips);
    return copy;
}
struct BrowserTextureGrab {
    WGPUBuffer staging;
    GpuApi::GpuFence fence;
    Uint64 bytes;
    GpuApi::Utils::TextureGrabParams conversion;
};
std::mutex textureGrabMutex;
std::vector<BrowserTextureGrab> textureGrabs;
void ShutdownBrowserTextureGrabs() {
    std::vector<BrowserTextureGrab> retired;
    { std::lock_guard<std::mutex> lock(textureGrabMutex); retired.swap(textureGrabs); }
    for (const auto& item : retired) cp2077::web::DestroyGpuBuffer(item.staging);
}
struct BrowserTextureLock {
    BrowserTextureRegion region;
    std::vector<unsigned char> bytes;
};
std::mutex textureLockMutex;
std::unordered_map<Uint64,std::unique_ptr<BrowserTextureLock>> textureLocks;
Uint64 TextureLockKey(GpuApi::TextureRef ref, Uint32 mip, Uint32 slice) {
    return (Uint64(ref.ID()) << 32) | (Uint64(slice) << 8) | mip;
}
void ShutdownBrowserTextureLocks() {
    std::unordered_map<Uint64,std::unique_ptr<BrowserTextureLock>> retired;
    { std::lock_guard<std::mutex> lock(textureLockMutex); retired.swap(textureLocks); }
}
// [nailfix] Persistently mapped TEXUSAGE_Upload textures. Some native systems lock an upload texture once and keep
// the pointer for the texture's lifetime, writing into it and then GpuApi::CopyTextureData()-ing it to a GPU
// texture (CFoliageProfilesManager "FoliageProfileMap" + its staging texture, the hair profiles). Here the GPU
// texture only receives the CPU bytes on UnlockLevel, which those systems never call, so every copy moved zeros:
// the foliage profile map was all zero -> speedtree alphaCutoff 0 -> leaf cards never clipped ("shredded" bushes).
// Before a copy reads an upload texture that is still locked, upload the current CPU bytes of its locked levels.
// Returns the number of levels uploaded.
unsigned FlushPersistentUploadLocks(const GpuApi::TextureRef& source) {
    if (source.isNull()) return 0;
    unsigned uploaded = 0;
    std::lock_guard<std::mutex> lock(textureLockMutex);
    for (auto& entry : textureLocks) {
        if (Uint32(entry.first >> 32) != source.ID() || !entry.second) continue;
        auto& request = *entry.second;
        if (request.region.descriptor.usage != GpuApi::TEXUSAGE_Upload) continue;
        if (UploadBrowserTexture(request.region, request.bytes.data(), request.region.pitch)) ++uploaded;
    }
    if (uploaded) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "CP2077 nailfix: persistent upload lock flushed before copy: texture=%u levels=%u\n", source.ID(), uploaded);
    }
    return uploaded;
}
bool QueueBrowserTextureGrab(GpuApi::TextureRef reference, Uint32 x, Uint32 y, Uint32 w, Uint32 h,
    void* output, Uint32 stride, bool alpha, bool floating) {
    if (!output || !w || !h || Uint64(x) + w > INT32_MAX || Uint64(y) + h > INT32_MAX) return false;
    Rect rect; rect.m_left=x; rect.m_top=y; rect.m_right=x+w; rect.m_bottom=y+h;
    BrowserTextureRegion region;
    if (!region.Acquire(reference, 0, 0, &rect) || region.descriptor.type != GpuWrapApi::TEXTYPE_2D) return false;
    using namespace GpuWrapApi;
    if (floating) {
        if (region.descriptor.format != TEXFMT_R16G16B16A16_Float && region.descriptor.format != TEXFMT_R32G32B32A32_Float &&
            region.descriptor.format != TEXFMT_R32_Float) return false;
    } else {
        if ((region.descriptor.format != TEXFMT_R8G8B8A8_Unorm && region.descriptor.format != TEXFMT_R8G8B8A8_Unorm_SRGB) ||
            !stride || stride > 4 || (alpha && stride != 4)) return false;
    }
    const auto fence = GpuApi::GetGpuFence();
    if (!fence) return false;
    const auto staging = cp2077::web::CreateGpuBuffer(region.bytes, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead, nullptr, 0, "cp-texture-readback");
    if (!staging) return false;
    if (!cp2077::web::RecordGpuTextureBufferCopy(region.texture, region.BufferCopy(staging), region.extent, reference, false)) {
        cp2077::web::DestroyGpuBuffer(staging); return false;
    }
    region.reference = GpuApi::TextureRef::Null();
    GpuApi::Utils::TextureGrabParams params{};
    params.sourceRowPitch = region.pitch; params.width = w; params.height = h;
    params.destData = output; params.destStride = stride; params.sourceStride = 4;
    params.forceFullAlpha = alpha; params.format = floating ? region.descriptor.format : TEXFMT_NULL;
    std::lock_guard<std::mutex> lock(textureGrabMutex);
    textureGrabs.push_back({staging, fence, region.bytes, params});
    return true;
}
}
namespace GpuApi {
void* LockLevel(const TextureRef& reference, Uint32 mip, Uint32 slice, Uint32& pitch) {
    pitch = 0;
    if (mip > 255 || slice > 0xffffff) return nullptr;
    const auto key=TextureLockKey(reference,mip,slice);
    std::lock_guard<std::mutex> lock(textureLockMutex);
    if (textureLocks.count(key)) return nullptr;
    auto request=std::make_unique<BrowserTextureLock>();
    auto& region=request->region;
    if (!region.Acquire(reference,mip,slice,nullptr) ||
        (region.descriptor.usage != TEXUSAGE_Upload && region.descriptor.usage != TEXUSAGE_Staging)) return nullptr;
    request->bytes.resize(static_cast<std::size_t>(region.bytes));
    if (region.descriptor.usage == TEXUSAGE_Upload) {
        std::lock_guard<std::mutex> texturesLock(textureMutex);
        const auto found=textures.find(reference.ID());
        if (found==textures.end()) return nullptr;
        const auto previous=found->second->uploadCopies.find((slice<<8)|mip);
        if (previous!=found->second->uploadCopies.end()) request->bytes=previous->second;
    } else {
        // Native callers must have submitted their staging copy before locking.
        // Read from the actual texture after earlier queue submissions; no CPU
        // mesh/pixel surrogate or zero-filled successful read is provided.
        auto staging=cp2077::web::CreateGpuBuffer(region.bytes,WGPUBufferUsage_CopyDst|WGPUBufferUsage_MapRead,nullptr,0,"cp-texture-readback2");
        if (!staging) return nullptr;
        auto encoder=cp2077::web::CreateGpuCommandEncoder();
        bool valid=encoder && cp2077::web::EncodeGpuTextureBufferCopy(encoder,region.texture,region.BufferCopy(staging),region.extent,false);
        if (valid) {
            valid=cp2077::web::SubmitGpuCommandEncoders(&encoder,1);
            // Submission nulls consumed handles, including finish failures.
            // A handle left non-null still belongs to this caller.
            if (valid) encoder=nullptr;
        }
        if (encoder) cp2077::web::ReleaseGpuCommandEncoder(encoder);
        if (valid) valid=cp2077::web::WaitGpuQueueIdle() &&
            cp2077::web::ReadGpuBuffer(staging,region.bytes,request->bytes.data(),request->bytes.size());
        cp2077::web::DestroyGpuBuffer(staging);
        if (!valid) return nullptr;
    }
    pitch=region.pitch;
    auto* bytes=request->bytes.data();
    textureLocks.emplace(key,std::move(request));
    return bytes;
}
void UnlockLevel(const TextureRef& reference, Uint32 mip, Uint32 slice) {
    NoteMultilayerTextureWrite(reference);
    if (mip > 255 || slice > 0xffffff) { cp2077::web::FailBoundGpuCommand(); return; }
    std::unique_ptr<BrowserTextureLock> request;
    {
        std::lock_guard<std::mutex> lock(textureLockMutex);
        const auto found=textureLocks.find(TextureLockKey(reference,mip,slice));
        if (found==textureLocks.end()) { cp2077::web::FailBoundGpuCommand(); return; }
        request=std::move(found->second); textureLocks.erase(found);
    }
    if (request->region.descriptor.usage==TEXUSAGE_Upload) {
        {
            std::lock_guard<std::mutex> lock(textureMutex);
            const auto found=textures.find(reference.ID());
            if (found==textures.end()) { cp2077::web::FailBoundGpuCommand(); return; }
            found->second->uploadCopies[(slice<<8)|mip]=request->bytes;
        }
        if (!UploadBrowserTexture(request->region,request->bytes.data(),request->region.pitch)) {
            cp2077::web::FailBoundGpuCommand(); std::fputs("CP2077 WebGPU: texture unlock upload failed\n",stderr);
        }
    }
}
void LoadTextureData2D(const TextureRef& destination, Uint32 mip, Uint32 slice, const Rect* rect, const void* source, Uint32 pitch) {
    NoteMultilayerTextureWrite(destination);
    BrowserTextureRegion region;
    const bool acquired=region.Acquire(destination,mip,slice,rect);
    const bool immutable=acquired && region.descriptor.HasUsage(TEXUSAGE_Immutable);
    const bool upload=acquired && region.descriptor.HasUsage(TEXUSAGE_Upload);
    const bool copied=acquired && !immutable && !upload && UploadBrowserTexture(region,source,pitch);
    if (!copied) {
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1)<4) emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,"CP2077 texture upload rejection: texture=%u mip=%u slice=%u acquired=%u immutable=%u upload=%u pitch=%u required=%u rows=%u format=%u size=%ux%u source=%u",
            destination.ID(),mip,slice,unsigned(acquired),unsigned(immutable),unsigned(upload),pitch,region.rowBytes,region.rows,unsigned(region.descriptor.format),unsigned(region.descriptor.width),unsigned(region.descriptor.height),unsigned(source!=nullptr));
        cp2077::web::FailBoundGpuCommand(); std::fputs("CP2077 WebGPU: invalid or unsupported texture upload\n", stderr);
    }
}
Bool GrabTexturePixels(const TextureRef& reference, Uint32 x, Uint32 y, Uint32 w, Uint32 h,
    Uint8* output, Uint32 stride, Bool alpha) {
    return QueueBrowserTextureGrab(reference,x,y,w,h,output,stride,alpha,false);
}
Bool GrabTexturePixels(const TextureRef& reference, Uint32 x, Uint32 y, Uint32 w, Uint32 h, Float* output, Bool alpha) {
    return QueueBrowserTextureGrab(reference,x,y,w,h,output,0,alpha,true);
}
Uint32 TryFinalizeTextureGrabs() {
    std::vector<BrowserTextureGrab> ready;
    {
        std::lock_guard<std::mutex> lock(textureGrabMutex);
        for (auto it = textureGrabs.begin(); it != textureGrabs.end();) {
            if (IsGpuFenceComplete(it->fence)) { ready.push_back(*it); it=textureGrabs.erase(it); }
            else ++it;
        }
    }
    Uint32 completed = 0;
    for (auto& grab : ready) {
        std::vector<unsigned char> bytes(static_cast<std::size_t>(grab.bytes));
        if (cp2077::web::ReadGpuBuffer(grab.staging, grab.bytes, bytes.data(), bytes.size())) {
            grab.conversion.sourceData = bytes.data();
            Utils::DoTextureGrab(grab.conversion); ++completed;
        } else std::fputs("CP2077 WebGPU: texture readback failed\n",stderr);
        cp2077::web::DestroyGpuBuffer(grab.staging);
    }
    return completed;
}
Uint32 FlushTextureGrabs() {
    Flush();
    return TryFinalizeTextureGrabs();
}
}
