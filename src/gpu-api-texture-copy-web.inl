// Original destination-first texture-copy APIs; actual encoder copies.
namespace {
struct TextureCopyResources {
    GpuApi::TextureRef sourceRef = GpuApi::TextureRef::Null(), destinationRef = GpuApi::TextureRef::Null();
    GpuApi::TextureDesc source, destination;
    WGPUTexture sourceTexture = nullptr, destinationTexture = nullptr;
    bool depthUnorm16=false;
    ~TextureCopyResources() {
        if (!sourceRef.isNull()) GpuApi::Release(sourceRef);
        if (!destinationRef.isNull()) GpuApi::Release(destinationRef);
    }
    bool Acquire(const GpuApi::TextureRef& dest, const GpuApi::TextureRef& src) {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto s = textures.find(src.ID()), d = textures.find(dest.ID());
        if (s == textures.end() || d == textures.end() ||
            s->second->references == INT32_MAX || d->second->references == INT32_MAX) return false;
        if(s->second.get()==d->second.get() && s->second->references>INT32_MAX-2)return false;
        source = s->second->descriptor; destination = d->second->descriptor;
        // Combined depth/stencil copies require separate aspect copies.
        if (HasStencilFormat(source.format) || HasStencilFormat(destination.format)) return false;
        sourceTexture = s->second->texture; destinationTexture = d->second->texture;
        ++s->second->references; ++d->second->references; sourceRef = src; destinationRef = dest;
        const auto a = HasDepthPlane(source) ? TranslateResource(source).native : StorageFormat(source.format);
        const auto b = HasDepthPlane(destination) ? TranslateResource(destination).native : StorageFormat(destination.format);
        auto linear = [](WGPUTextureFormat f) {
            switch (f) {
            case WGPUTextureFormat_RGBA8UnormSrgb: return WGPUTextureFormat_RGBA8Unorm;
            case WGPUTextureFormat_BC1RGBAUnormSrgb: return WGPUTextureFormat_BC1RGBAUnorm;
            case WGPUTextureFormat_BC2RGBAUnormSrgb: return WGPUTextureFormat_BC2RGBAUnorm;
            case WGPUTextureFormat_BC3RGBAUnormSrgb: return WGPUTextureFormat_BC3RGBAUnorm;
            case WGPUTextureFormat_BC7RGBAUnormSrgb: return WGPUTextureFormat_BC7RGBAUnorm;
            default: return f;
            }
        };
        depthUnorm16=source.format==GpuWrapApi::TEXFMT_D16U && destination.format==GpuWrapApi::TEXFMT_D16U &&
            HasDepthPlane(source) && !HasDepthPlane(destination) &&
            source.HasUsage(GpuApi::TEXUSAGE_Samplable) && destination.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess);
        return a && b && (linear(a) == linear(b) || depthUnorm16) && Uint32(source.sampleCount) == 0 &&
            Uint32(destination.sampleCount) == 0 &&
            (source.type == GpuWrapApi::TEXTYPE_3D) == (destination.type == GpuWrapApi::TEXTYPE_3D);
    }
    bool Submit(const std::vector<cp2077::web::GpuTextureCopy>& copies) {
        if(depthUnorm16) {
            // The original rain map copies one depth subresource into one
            // array layer. Reject other dimensional cases until translated.
            if(copies.size()!=1||copies[0].extent.depthOrArrayLayers!=1||
                source.type==GpuWrapApi::TEXTYPE_3D||destination.type==GpuWrapApi::TEXTYPE_3D)return false;
            const auto& copy=copies[0];
            WGPUTextureViewDescriptor sv=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT,dv=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
            sv.dimension=dv.dimension=WGPUTextureViewDimension_2D;
            sv.format=WGPUTextureFormat_Depth16Unorm;sv.aspect=WGPUTextureAspect_DepthOnly;
            dv.format=WGPUTextureFormat_R16Unorm;dv.aspect=WGPUTextureAspect_All;
            sv.baseMipLevel=copy.source.mipLevel;dv.baseMipLevel=copy.destination.mipLevel;
            sv.mipLevelCount=dv.mipLevelCount=1;
            sv.baseArrayLayer=copy.source.origin.z;dv.baseArrayLayer=copy.destination.origin.z;
            sv.arrayLayerCount=dv.arrayLayerCount=1;
            sv.usage=WGPUTextureUsage_TextureBinding;dv.usage=WGPUTextureUsage_StorageBinding;
            auto sourceView=cp2077::web::CreateGpuTextureView(sourceTexture,sv);
            auto destinationView=cp2077::web::CreateGpuTextureView(destinationTexture,dv);
            const bool copied=sourceView&&destinationView&&cp2077::web::RecordGpuDepthUnorm16Copy(sourceView,destinationView,
                copy.source.origin.x,copy.source.origin.y,copy.destination.origin.x,copy.destination.origin.y,
                copy.extent.width,copy.extent.height,sourceRef,destinationRef);
            if(sourceView)cp2077::web::ReleaseGpuTextureView(sourceView);
            if(destinationView)cp2077::web::ReleaseGpuTextureView(destinationView);
            if(!copied)return false;
        } else if (!cp2077::web::RecordGpuTextureCopies(copies.data(), copies.size(), sourceRef, destinationRef)) return false;
        sourceRef = destinationRef = GpuApi::TextureRef::Null(); return true;
    }
    bool Prepare(Uint32 dm, Uint32 dl, Uint32 sm, Uint32 sl, cp2077::web::GpuTextureCopy& copy, bool fullSize=true) {
        if (dm >= destination.Internal_CalcTargetNumMips() || sm >= source.Internal_CalcTargetNumMips() ||
            dl >= destination.CalcTargetSlices() || sl >= source.CalcTargetSlices()) return false;
        copy.source.texture = sourceTexture; copy.source.mipLevel = sm;
        copy.destination.texture = destinationTexture; copy.destination.mipLevel = dm;
        const bool volume = source.type == GpuWrapApi::TEXTYPE_3D;
        copy.source.origin.z = volume ? 0 : sl; copy.destination.origin.z = volume ? 0 : dl;
        copy.extent = {DeviceMipDimension(source.width, Uint8(sm), source.format),
            DeviceMipDimension(source.height, Uint8(sm), source.format),
            volume ? std::max(1u, Uint32(source.depthOrSliceNum) >> sm) : 1u};
        if (fullSize && !depthUnorm16 && HasDepthPlane(source) && (source.format == GpuWrapApi::TEXFMT_D32F || source.format == GpuWrapApi::TEXFMT_D16U) &&
            (copy.extent.width != DeviceMipDimension(destination.width, Uint8(dm), destination.format) ||
             copy.extent.height != DeviceMipDimension(destination.height, Uint8(dm), destination.format)))
            return false;
        return !fullSize || (copy.extent.width <= DeviceMipDimension(destination.width, Uint8(dm), destination.format) &&
            copy.extent.height <= DeviceMipDimension(destination.height, Uint8(dm), destination.format) &&
            (!volume || copy.extent.depthOrArrayLayers <= std::max(1u, Uint32(destination.depthOrSliceNum) >> dm)));
    }
};
bool RejectTextureCopy(const char* operation, Int32 line, const GpuApi::TextureRef& dest, const GpuApi::TextureRef& src) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1)<4) {
        const auto a=GpuApi::GetTextureDesc(src), b=GpuApi::GetTextureDesc(dest);
        emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,"CP2077 texture copy rejection: %s:%d src=%u dst=%u formats=%u/%u size=%ux%u/%ux%u type=%u/%u samples=%u/%u mips=%u/%u slices=%u/%u",
            operation,line,src.ID(),dest.ID(),unsigned(a.format),unsigned(b.format),unsigned(a.width),unsigned(a.height),unsigned(b.width),unsigned(b.height),unsigned(a.type),unsigned(b.type),unsigned(a.sampleCount),unsigned(b.sampleCount),a.Internal_CalcTargetNumMips(),b.Internal_CalcTargetNumMips(),a.CalcTargetSlices(),b.CalcTargetSlices());
    }
    cp2077::web::FailBoundGpuCommand();
    std::fputs("CP2077 WebGPU: invalid or unsupported texture copy\n", stderr); return false;
}
}
namespace GpuApi {
Bool ResolveTextureData(const TextureRef& dest, Uint32 dm, Uint32 dl,
                        const TextureRef& src, Uint32 sm, Uint32 sl) {
    NoteMultilayerTextureWrite(dest);
    if (dest.isNull() || src.isNull() || dest == src) return RejectTextureCopy(__func__, __LINE__, dest, src);
    WGPUTextureView sourceView = nullptr, destinationView = nullptr;
    WGPUTextureFormat sourceFormat, destinationFormat;
    if (!cp2077::web::AcquireRenderGpuTextureView(src, sm, sl, false, sourceView, &sourceFormat, true))
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    if (!cp2077::web::AcquireRenderGpuTextureView(dest, dm, dl, false, destinationView, &destinationFormat, true)) {
        cp2077::web::ReleaseGpuTextureView(sourceView); Release(src); return RejectTextureCopy(__func__, __LINE__, dest, src);
    }
    const auto source = GetTextureDesc(src), destination = GetTextureDesc(dest);
    const bool valid = Uint32(source.sampleCount) == 2 && Uint32(destination.sampleCount) == 0 &&
        sourceFormat == destinationFormat && source.format == destination.format && sm == 0 &&
        DeviceMipDimension(source.width, Uint8(sm), source.format) == DeviceMipDimension(destination.width, Uint8(dm), destination.format) &&
        DeviceMipDimension(source.height, Uint8(sm), source.format) == DeviceMipDimension(destination.height, Uint8(dm), destination.format);
    const bool recorded = valid && cp2077::web::RecordGpuTextureResolve(sourceView, destinationView, src, dest);
    cp2077::web::ReleaseGpuTextureView(sourceView); cp2077::web::ReleaseGpuTextureView(destinationView);
    if (!recorded) { Release(src); Release(dest); return RejectTextureCopy(__func__, __LINE__, dest, src); }
    return true;
}
Bool CopyTextureData(const TextureRef& dest, const TextureRef& src) {
    NoteMultilayerTextureWrite(dest);
    FlushPersistentUploadLocks(src); // [nailfix]
    TextureCopyResources resources;
    if (!resources.Acquire(dest, src)) return RejectTextureCopy(__func__, __LINE__, dest, src);
    const auto& a = resources.source; const auto& b = resources.destination;
    if (a.width != b.width || a.height != b.height ||
        (a.type == GpuWrapApi::TEXTYPE_3D && a.depthOrSliceNum != b.depthOrSliceNum) ||
        a.Internal_CalcTargetNumMips() != b.Internal_CalcTargetNumMips() || a.CalcTargetSlices() != b.CalcTargetSlices())
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    std::vector<cp2077::web::GpuTextureCopy> copies;
    for (Uint32 mip = 0; mip < a.Internal_CalcTargetNumMips(); ++mip) {
        cp2077::web::GpuTextureCopy copy;
        if (!resources.Prepare(mip, 0, mip, 0, copy)) return RejectTextureCopy(__func__, __LINE__, dest, src);
        if (a.type != GpuWrapApi::TEXTYPE_3D) copy.extent.depthOrArrayLayers = a.CalcTargetSlices();
        copies.push_back(copy);
    }
    return resources.Submit(copies) ? true : RejectTextureCopy(__func__, __LINE__, dest, src);
}
Bool CopyTextureData(const TextureRef& dest, Uint32 dm, Uint32 dl, const TextureRef& src, Uint32 sm, Uint32 sl) {
    NoteMultilayerTextureWrite(dest);
    FlushPersistentUploadLocks(src); // [nailfix]
    const auto packed=TryPackedCompressionCopy(dest,dm,dl,src,sm,sl);
    if (packed) return packed > 0 ? true : RejectTextureCopy(__func__, __LINE__, dest, src);
    TextureCopyResources resources; cp2077::web::GpuTextureCopy copy;
    if (!resources.Acquire(dest, src) || !resources.Prepare(dm, dl, sm, sl, copy)) return RejectTextureCopy(__func__, __LINE__, dest, src);
    return resources.Submit({copy}) ? true : RejectTextureCopy(__func__, __LINE__, dest, src);
}
Bool CopyRect(const TextureRef& dest, Uint32 dx, Uint32 dy, Uint32 dm, Uint32 dl,
              const TextureRef& src, const Rect& rect, Uint32 sm, Uint32 sl) {
    NoteMultilayerTextureWrite(dest);
    FlushPersistentUploadLocks(src); // [nailfix]
    TextureCopyResources resources; cp2077::web::GpuTextureCopy copy;
    if (!resources.Acquire(dest, src) || !resources.Prepare(dm, dl, sm, sl, copy, false) ||
        rect.m_left < 0 || rect.m_top < 0 || rect.m_right <= rect.m_left || rect.m_bottom <= rect.m_top)
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    const Uint32 sx = rect.m_left, sy = rect.m_top, width = Uint32(rect.m_right)-sx, height = Uint32(rect.m_bottom)-sy;
    const Uint32 sw = copy.extent.width, sh = copy.extent.height;
    const Uint32 dw = DeviceMipDimension(resources.destination.width, Uint8(dm), resources.destination.format);
    const Uint32 dh = DeviceMipDimension(resources.destination.height, Uint8(dm), resources.destination.format);
    const auto block = Translate(resources.source.format).blockSize;
    if (!block || sx > sw || width > sw-sx || sy > sh || height > sh-sy ||
        dx > dw || width > dw-dx || dy > dh || height > dh-dy || (sx|sy|dx|dy|width|height)%block)
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    copy.source.origin.x = sx; copy.source.origin.y = sy;
    copy.destination.origin.x = dx; copy.destination.origin.y = dy;
    if (sx || sy || width != sw || height != sh) copy.extent.depthOrArrayLayers = 1;
    if (resources.source.type == GpuWrapApi::TEXTYPE_3D &&
        copy.extent.depthOrArrayLayers > std::max(1u, Uint32(resources.destination.depthOrSliceNum) >> dm))
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    if (!resources.depthUnorm16 && HasDepthPlane(resources.source) && (resources.source.format == GpuWrapApi::TEXFMT_D32F || resources.source.format == GpuWrapApi::TEXFMT_D16U) &&
        (sx || sy || dx || dy || width != sw || height != sh || width != dw || height != dh))
        return RejectTextureCopy(__func__, __LINE__, dest, src);
    copy.extent.width = width; copy.extent.height = height;
    return resources.Submit({copy}) ? true : RejectTextureCopy(__func__, __LINE__, dest, src);
}
}
