// Browser swap-chain implementation sharing the original texture registry.
namespace {
struct BrowserSwapChain {
    GpuApi::TextureRef backbuffer = GpuApi::TextureRef::Null();
    Uint32 width = 0, height = 0;
    Int32 references = 1;
};
std::mutex swapChainMutex;
std::unordered_map<Uint32, BrowserSwapChain> browserSwapChains;
Uint32 nextSwapChainId = 1;
bool AcquireBrowserBackbuffer(const GpuApi::TextureRef& ref, WGPUTexture& native) {
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(ref.ID());
    if (entry == textures.end() || entry->second->references == INT32_MAX ||
        !entry->second->descriptor.HasUsage(GpuApi::TEXUSAGEEX_Internal_BackBuffer)) return false;
    ++entry->second->references; native = entry->second->texture; return true;
}
}
namespace GpuApi {
Bool ToggleFullscreen(const SwapChainRef& reference, Bool fullscreen, Int32 monitor) {
    if (monitor > 0) return false;
    {
        std::lock_guard<std::mutex> lock(swapChainMutex);
        if (!browserSwapChains.count(reference.ID())) return false;
    }
    return cp2077::web::SetGpuCanvasFullscreen(fullscreen);
}
Bool ToggleFullscreen(const SwapChainRef& reference, Bool fullscreen) { return ToggleFullscreen(reference,fullscreen,0); }
Bool GetFullscreenState(const SwapChainRef& reference) {
    { std::lock_guard<std::mutex> lock(swapChainMutex); if (!browserSwapChains.count(reference.ID())) return false; }
    return cp2077::web::IsGpuCanvasFullscreen();
}
SwapChainRef CreateSwapChainWithBackBuffer(const SwapChainDesc& descriptor) {
    if (!descriptor.width || !descriptor.height || descriptor.width > 65535 || descriptor.height > 65535 ||
        descriptor.fullscreen || GpuWrapApi::GetHdrModeTextureFormat(descriptor.hdrMode) != GpuWrapApi::TEXFMT_R8G8B8A8_Unorm) {
        std::fputs("CP2077 WebGPU: swap chain currently requires windowed SDR dimensions\n", stderr);
        return SwapChainRef::Null();
    }
    std::lock_guard<std::mutex> lock(swapChainMutex);
    if (!browserSwapChains.empty() || nextSwapChainId > Uint32(INT32_MAX)) return SwapChainRef::Null();
    if (!cp2077::web::ConfigureGpuCanvas(descriptor.width, descriptor.height)) return SwapChainRef::Null();
    TextureDesc texture;
    texture.width = descriptor.width; texture.height = descriptor.height; texture.depthOrSliceNum = 1;
    texture.type = GpuWrapApi::TEXTYPE_2D; texture.format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
    texture.sampleCount = static_cast<GpuWrapApi::eTextureSampleCount>(0); texture.numMips = 1;
    texture.usage = TEXUSAGE_RenderTarget;
    texture.usageEx = TEXUSAGEEX_Internal_BackBuffer;
    TextureInitData initial; initial.m_mode = TextureInitData::Mode::Uninitialized;
    const auto backbuffer = CreateTexture(texture, initial);
    if (backbuffer.isNull()) return SwapChainRef::Null();
    if (descriptor.allowSrgbRenderTarget) {
        std::lock_guard<std::mutex> textureLock(textureMutex);
        textures.at(backbuffer.ID())->renderFormat = WGPUTextureFormat_RGBA8UnormSrgb;
    }
    const auto id = nextSwapChainId++;
    browserSwapChains.emplace(id, BrowserSwapChain{backbuffer,descriptor.width,descriptor.height,1});
    return SwapChainRef{id};
}
void AddRef(const SwapChainRef& ref) {
    if (ref.isNull()) return;
    std::lock_guard<std::mutex> lock(swapChainMutex);
    auto entry = browserSwapChains.find(ref.ID());
    if (entry != browserSwapChains.end() && entry->second.references < INT32_MAX) ++entry->second.references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing swap-chain reference\n", stderr);
}
Int32 Release(const SwapChainRef& ref) {
    if (ref.isNull()) return 0;
    TextureRef retired = TextureRef::Null(); Int32 count = 0;
    {
        std::lock_guard<std::mutex> lock(swapChainMutex);
        auto entry = browserSwapChains.find(ref.ID());
        if (entry == browserSwapChains.end()) return 0;
        count = --entry->second.references;
        if (!count) { retired = entry->second.backbuffer; browserSwapChains.erase(entry); }
    }
    if (!retired.isNull()) Release(retired);
    return count;
}
TextureRef GetBackBufferTexture(SwapChainRef ref) {
    // Original API returns a borrowed ref; callers explicitly retain if needed.
    std::lock_guard<std::mutex> lock(swapChainMutex);
    auto entry = browserSwapChains.find(ref.ID());
    return entry == browserSwapChains.end() ? TextureRef::Null() : entry->second.backbuffer;
}
void Present(const SwapChainRef& ref, Bool useVsync, Uint32 threshold) {
    if (useVsync && threshold > 1) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: multi-frame presentation intervals are unfinished\n", stderr); return;
    }
    TextureRef backbuffer = TextureRef::Null(); WGPUTexture source = nullptr; Uint32 width = 0, height = 0;
    {
        std::lock_guard<std::mutex> lock(swapChainMutex);
        auto entry = browserSwapChains.find(ref.ID());
        if (entry != browserSwapChains.end()) {
            backbuffer = entry->second.backbuffer; width = entry->second.width; height = entry->second.height;
            if (!AcquireBrowserBackbuffer(backbuffer, source)) backbuffer = TextureRef::Null();
        }
    }
    const bool presented = source && cp2077::web::PresentGpuCanvas(source, width, height);
    if (!backbuffer.isNull()) Release(backbuffer);
    // Browsers pace canvas presentation with animation frames even when the
    // native caller requests no vsync; tearing cannot be enabled by this API.
    if (!presented) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: original swap-chain canvas submission failed\n", stderr);
    }
}
}
namespace cp2077::web {
void ShutdownGpuSwapChains() {
    std::unordered_map<Uint32, BrowserSwapChain> retired;
    { std::lock_guard<std::mutex> lock(swapChainMutex); retired.swap(browserSwapChains); }
    for (auto& item : retired) GpuApi::Release(item.second.backbuffer);
}
}
