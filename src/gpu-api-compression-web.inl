namespace {
std::mutex compressorMutex;
std::unique_ptr<GpuApi::TextureCompressor> textureCompressor;
GpuApi::ShaderRef registeredCompressionShaders[GpuApi::TextureCompressor::eMAX]{};
// [gamerender] Defined in gpu-api-texture-transfer-web.inl: byte-identical RGBA8_UNORM copy of an
// R8G8B8A8_Uint_Typeless (r32uint-backed) morph texture, recorded in the bound list. Returns an owned ref.
GpuApi::TextureRef CreateMorphUnormCopy(const GpuApi::TextureRef& source);
Int32 CompressionIndex(GpuWrapApi::eTextureFormat format) {
    switch (format) {
    case GpuWrapApi::TEXFMT_BC1: case GpuWrapApi::TEXFMT_BC1_SRGB: return 0;
    case GpuWrapApi::TEXFMT_BC3: case GpuWrapApi::TEXFMT_BC3_SRGB: return 1;
    case GpuWrapApi::TEXFMT_BC4: return 2;
    case GpuWrapApi::TEXFMT_BC5: return 3;
    default: return -1;
    }
}
}
namespace GpuApi {
void SetupTextureCompressionShaders(const ShaderRef& shader, Uint32 index) {
    const char* expected[]{"m_textureCompressBC1","m_textureCompressBC3","m_textureCompressBC4","m_textureCompressBC5"};
    WGPUComputePipeline pipeline=nullptr;
    if (index >= TextureCompressor::eMAX || !cp2077::web::AcquireGpuComputeShader(shader,pipeline)) {
        cp2077::web::FailBoundGpuCommand(); std::fputs("CP2077 WebGPU: compression shader is unavailable\n",stderr); return;
    }
    const auto* source=cp2077::web::GetGpuComputeShaderSource(shader);
    if (!source || std::strcmp(source->runtimeName,expected[index])) {
        Release(shader); cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: compression shader does not match its original registry slot\n",stderr); return;
    }
    {
        std::lock_guard<std::mutex> lock(compressorMutex);
        if (!textureCompressor) textureCompressor=std::make_unique<TextureCompressor>();
        textureCompressor->RegisterCompressorShader(shader,static_cast<TextureCompressor::ECompressorType>(index));
        registeredCompressionShaders[index]=shader; // Borrowed from the native compressor's owned ref.
    }
    Release(shader);
}
void Compression_MakeTextureDescCompatible(const TextureRef& source, TextureDesc& destination) {
    if (!cp2077::web::RetainGpuTexture(source)) { cp2077::web::FailBoundGpuCommand(); return; }
    const auto descriptor=GetTextureDesc(source);
    if (descriptor.width < 4 || descriptor.height < 4 || descriptor.width % 4 || descriptor.height % 4 ||
        destination.type != GpuWrapApi::TEXTYPE_2D || destination.depthOrSliceNum != 1) {
        Release(source); cp2077::web::FailBoundGpuCommand(); return;
    }
    TextureCompressor::MakeTextureDescCompatible(source,destination);
    // [nobc] Without BC the "compressed" texture is an RGBA8 copy of the source (CompressTexture copies instead).
    if (cp2077::web::GpuDecodesBC() && CompressionIndex(destination.format) >= 0)
        destination.format = (destination.format == GpuWrapApi::TEXFMT_BC1_SRGB || destination.format == GpuWrapApi::TEXFMT_BC3_SRGB)
            ? GpuWrapApi::TEXFMT_R8G8B8A8_Unorm_SRGB : GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
    Release(source);
}
void CompressTexture(const TextureRef& source, const TextureRef& destination, Uint32 slice) {
    if (!cp2077::web::RetainGpuTexture(source)) { cp2077::web::FailBoundGpuCommand(); return; }
    if (!cp2077::web::RetainGpuTexture(destination)) { Release(source); cp2077::web::FailBoundGpuCommand(); return; }
    const auto sourceDesc=GetTextureDesc(source),destinationDesc=GetTextureDesc(destination);
    if (cp2077::web::GpuDecodesBC()) {
        // [nobc] Copy each level of the source (same 4x4-multiple size) into the uncompressed destination.
        TextureRef unorm=sourceDesc.format == GpuWrapApi::TEXFMT_R8G8B8A8_Uint_Typeless ? CreateMorphUnormCopy(source) : TextureRef::Null();
        const TextureRef& from=unorm.isNull() ? source : unorm;
        const Uint32 mips=std::min(destinationDesc.Internal_CalcTargetNumMips(),sourceDesc.Internal_CalcTargetNumMips());
        Uint32 copied=0;
        for (Uint32 mip=0; mip<mips; ++mip) copied+=CopyTextureData(destination,mip,slice,from,mip,0) ? 1 : 0;
        if (copied!=mips) {
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1)<8) std::fprintf(stderr,"CP2077 nobc: compression copy skipped: srcFormat=%u dstFormat=%u levels=%u/%u\n",
                unsigned(sourceDesc.format),unsigned(destinationDesc.format),copied,mips);
        }
        if (!unorm.isNull()) Release(unorm);
        Release(source); Release(destination); return;
    }
    const auto index=CompressionIndex(destinationDesc.format);
    bool valid=index >= 0 && sourceDesc.type == GpuWrapApi::TEXTYPE_2D && sourceDesc.HasUsage(TEXUSAGE_Samplable) &&
        source != destination && slice < destinationDesc.CalcTargetSlices() &&
        destinationDesc.width >= 4 && destinationDesc.height >= 4 && !(destinationDesc.width % 4 || destinationDesc.height % 4) &&
        destinationDesc.numMips && (destinationDesc.width >> (destinationDesc.numMips-1)) >= 4 &&
        (destinationDesc.height >> (destinationDesc.numMips-1)) >= 4;
    // [gamerender] DX12 samples the morph working texture through its R8G8B8A8_UNORM SRV.
    TextureRef unorm=TextureRef::Null();
    if (valid && sourceDesc.format == GpuWrapApi::TEXFMT_R8G8B8A8_Uint_Typeless) {
        unorm=CreateMorphUnormCopy(source);
        valid=!unorm.isNull();
    }
    {
        std::lock_guard<std::mutex> lock(compressorMutex);
        valid=valid && textureCompressor && !registeredCompressionShaders[index].isNull();
        if (valid) textureCompressor->CompressTexture(unorm.isNull() ? source : unorm,destination,slice);
    }
    if (!unorm.isNull()) Release(unorm);
    Release(source); Release(destination);
    if (!valid) { cp2077::web::FailBoundGpuCommand(); std::fputs("CP2077 WebGPU: invalid native compression request or missing original kernel\n",stderr); }
}
}
namespace cp2077::web {
void ShutdownGpuTextureCompressor() {
    std::unique_ptr<GpuApi::TextureCompressor> retired;
    {
        std::lock_guard<std::mutex> lock(compressorMutex); retired=std::move(textureCompressor);
        for (auto& ref : registeredCompressionShaders) ref=GpuApi::ShaderRef::Null();
    }
}
}
