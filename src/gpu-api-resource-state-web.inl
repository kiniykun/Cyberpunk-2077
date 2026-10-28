// WebGPU owns implicit state transitions. Each draw/dispatch ends its pass,
// and all command lists submit to one queue. Native UAV ordering therefore
// maps to these real pass boundaries plus validated resource lifetime holds.
namespace {
enum class BrowserResourceUse { Retire, ShaderRead, StorageWrite, RenderTarget };
bool IsBrowserStorageBuffer(GpuWrapApi::eBufferChunkCategory category, bool write) {
    switch (category) {
    case GpuWrapApi::BCC_Structured: return !write;
    case GpuWrapApi::BCC_StructuredUAV:
    case GpuWrapApi::BCC_StructuredAppendUAV:
    case GpuWrapApi::BCC_Raw:
    case GpuWrapApi::BCC_IndirectUAV:
    case GpuWrapApi::BCC_TypedUAV: // only R32_UINT typed buffers are ever created (TranslateUsage)
    case GpuWrapApi::BCC_VertexUAV:
    case GpuWrapApi::BCC_Index16BitUAV: return true;
    default: return false;
    }
}
bool CommitBrowserResourceHolds(std::vector<BufferRef>& buffers, std::vector<TextureRef>& textures) {
    std::lock_guard<std::mutex> lock(commandMutex);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id() || entry->second.failed) return false;
    auto& command = entry->second;
    command.retainedBuffers.insert(command.retainedBuffers.end(), buffers.begin(), buffers.end());
    command.retainedTextures.insert(command.retainedTextures.end(), textures.begin(), textures.end());
    buffers.clear(); textures.clear(); return true;
}
// Holds are lifetime retention only; WebGPU has no real barrier to issue.
// A ref that is stale or lacks the usage flag is skipped (not held) and the
// command list survives. Each distinct (kind, use, flags, format) logs once.
bool FirstBrowserHoldSkip(std::uint64_t key) {
    static std::mutex skipMutex;
    static std::set<std::uint64_t> reported;
    std::lock_guard<std::mutex> lock(skipMutex);
    return reported.insert(key).second;
}
void HoldBrowserBuffers(Uint32 count, const BufferRef* refs, BrowserResourceUse use) {
    if (!count) return;
    if (!refs) {
        if (FirstBrowserHoldSkip((std::uint64_t(1) << 60) | std::uint64_t(use)))
            std::fprintf(stderr, "CP2077 WebGPU: skipped buffer hold: null ref array count=%u use=%u\n", unsigned(count), unsigned(use));
        return;
    }
    // Per-thread scratch: transitions run per bind on every render thread; no heap allocation per call.
    static thread_local std::vector<BufferRef> acquired; static thread_local std::vector<TextureRef> unused;
    acquired.clear(); unused.clear();
    for (Uint32 i = 0; i < count; ++i) {
        if (refs[i].isNull()) continue;
        BufferDesc descriptor; WGPUBuffer buffer;
        if (!cp2077::web::AcquireGpuBuffer(refs[i], descriptor, buffer)) {
            if (FirstBrowserHoldSkip((std::uint64_t(2) << 60) | std::uint64_t(use)))
                std::fprintf(stderr, "CP2077 WebGPU: skipped buffer hold: stale ref=%u use=%u\n", unsigned(refs[i].ID()), unsigned(use));
            continue;
        }
        if (!buffer || !(use == BrowserResourceUse::Retire ||
            IsBrowserStorageBuffer(descriptor.category, use == BrowserResourceUse::StorageWrite))) {
            Release(refs[i]);
            if (FirstBrowserHoldSkip((std::uint64_t(3) << 60) | (std::uint64_t(use) << 40) |
                    (std::uint64_t(buffer != nullptr) << 32) | std::uint64_t(descriptor.category)))
                std::fprintf(stderr, "CP2077 WebGPU: skipped buffer hold: ref=%u use=%u category=%u size=%u wgpu=%u\n",
                    unsigned(refs[i].ID()), unsigned(use), unsigned(descriptor.category), unsigned(descriptor.size), unsigned(buffer != nullptr));
            continue;
        }
        acquired.push_back(refs[i]);
    }
    const bool valid = CommitBrowserResourceHolds(acquired, unused);
    for (auto ref : acquired) Release(ref);
    if (!valid) {
        const bool cascade = cp2077::web::BoundGpuCommandAlreadyFailed(); // [gamerender] silent follow-on
        cp2077::web::FailBoundGpuCommand();
        if (!cascade) std::fputs("CP2077 WebGPU: invalid buffer transition/lifetime hold\n", stderr);
    }
}
void HoldBrowserTextures(Uint32 count, const TextureRef* refs, BrowserResourceUse use) {
    if (!count) return;
    if (!refs) {
        if (FirstBrowserHoldSkip((std::uint64_t(4) << 60) | std::uint64_t(use)))
            std::fprintf(stderr, "CP2077 WebGPU: skipped texture hold: null ref array count=%u use=%u\n", unsigned(count), unsigned(use));
        return;
    }
    static thread_local std::vector<TextureRef> acquired; static thread_local std::vector<BufferRef> unused;
    acquired.clear(); unused.clear();
    for (Uint32 i = 0; i < count; ++i) {
        if (refs[i].isNull()) continue;
        if (!cp2077::web::RetainGpuTexture(refs[i])) {
            if (FirstBrowserHoldSkip((std::uint64_t(5) << 60) | std::uint64_t(use)))
                std::fprintf(stderr, "CP2077 WebGPU: skipped texture hold: stale ref=%u use=%u\n", unsigned(refs[i].ID()), unsigned(use));
            continue;
        }
        const auto descriptor = GetTextureDesc(refs[i]);
        bool usable = true;
        switch (use) {
        case BrowserResourceUse::Retire: break;
        case BrowserResourceUse::ShaderRead:
            usable = descriptor.HasUsage(TEXUSAGE_Samplable) || descriptor.HasUsage(TEXUSAGE_Bindless); break;
        case BrowserResourceUse::StorageWrite: usable = descriptor.HasUsage(TEXUSAGE_UnorderedAccess); break;
        case BrowserResourceUse::RenderTarget: usable = descriptor.HasUsage(TEXUSAGE_RenderTarget); break;
        }
        if (!usable) {
            Release(refs[i]);
            if (FirstBrowserHoldSkip((std::uint64_t(6) << 60) | (std::uint64_t(use) << 56) |
                    ((std::uint64_t(descriptor.format) & 0xFFFF) << 40) | ((std::uint64_t(descriptor.usageEx) & 0xFF) << 32) |
                    std::uint64_t(std::uint32_t(descriptor.usage))))
                std::fprintf(stderr, "CP2077 WebGPU: skipped texture hold (usage mismatch): ref=%u use=%u w=%u h=%u fmt=%u usage=0x%x usageEx=0x%x\n",
                    unsigned(refs[i].ID()), unsigned(use), unsigned(descriptor.width), unsigned(descriptor.height),
                    unsigned(descriptor.format), unsigned(descriptor.usage), unsigned(descriptor.usageEx));
            continue;
        }
        acquired.push_back(refs[i]);
    }
    const bool valid = CommitBrowserResourceHolds(unused, acquired);
    for (auto ref : acquired) Release(ref);
    if (!valid) {
        const bool cascade = cp2077::web::BoundGpuCommandAlreadyFailed(); // [gamerender] silent follow-on
        cp2077::web::FailBoundGpuCommand();
        if (!cascade) std::fputs("CP2077 WebGPU: invalid texture transition/lifetime hold\n", stderr);
    }
}
}
void TransitionBufferSRV(Uint32 count, const BufferRef* refs) { HoldBrowserBuffers(count,refs,BrowserResourceUse::ShaderRead); }
void TransitionBufferUAV(Uint32 count, const BufferRef* refs) { HoldBrowserBuffers(count,refs,BrowserResourceUse::StorageWrite); }
void BarrierBufferUAV(Uint32 count, const BufferRef* refs) { HoldBrowserBuffers(count,refs,BrowserResourceUse::StorageWrite); }
void TransitionTextureSRV(Uint32 count, const TextureRef* refs) { HoldBrowserTextures(count,refs,BrowserResourceUse::ShaderRead); }
void TransitionTextureRTV(Uint32 count, const TextureRef* refs) { HoldBrowserTextures(count,refs,BrowserResourceUse::RenderTarget); }
void TransitionTextureUAV(Uint32 count, const TextureRef* refs) { HoldBrowserTextures(count,refs,BrowserResourceUse::StorageWrite); }
void TransitionTextureUAV(TextureRef ref) { HoldBrowserTextures(1,&ref,BrowserResourceUse::StorageWrite); }
void BarrierTextureUAV(Uint32 count, const TextureRef* refs) { HoldBrowserTextures(count,refs,BrowserResourceUse::StorageWrite); }
void MakeStateSafeToRetire(BufferRef ref) { HoldBrowserBuffers(1,&ref,BrowserResourceUse::Retire); }
void MakeStateSafeToRetire(TextureRef ref) { HoldBrowserTextures(1,&ref,BrowserResourceUse::Retire); }
void BarrierBufferAliasing(Bool, BufferRef before, BufferRef after) {
    // Every encoder operation ends its pass, and all submissions use one queue.
    // WebGPU owns the ordering; retain both logical aliases until submission
    // completes so neither can destroy the shared physical backing early.
    const BufferRef refs[]{before, after};
    HoldBrowserBuffers(2, refs, BrowserResourceUse::Retire);
}
void BarrierTextureAliasing(Bool, TextureRef before, TextureRef after) {
    const TextureRef refs[]{before, after};
    HoldBrowserTextures(2, refs, BrowserResourceUse::Retire);
}
void DiscardTexture(TextureRef reference, Int32 slice) {
    if (reference.isNull()) return;
    // Discard is only a hint: a stale or unsuitable texture skips it (logged
    // once per kind) and the command list survives.
    if (!cp2077::web::RetainGpuTexture(reference)) {
        if (FirstBrowserHoldSkip(std::uint64_t(7) << 60))
            std::fprintf(stderr, "CP2077 WebGPU: skipped texture discard: stale ref=%u slice=%d\n", unsigned(reference.ID()), int(slice));
        return;
    }
    const auto descriptor=GetTextureDesc(reference);
    const bool valid=slice >= -1 && (slice < 0 || Uint32(slice) < descriptor.CalcTargetSlices()) &&
        (descriptor.HasUsage(TEXUSAGE_RenderTarget) || descriptor.HasUsage(TEXUSAGE_DepthStencil) ||
         descriptor.HasUsage(TEXUSAGE_UnorderedAccess));
    Release(reference);
    if (!valid) {
        if (FirstBrowserHoldSkip((std::uint64_t(8) << 60) | ((std::uint64_t(descriptor.format) & 0xFFFF) << 40) |
                ((std::uint64_t(descriptor.usageEx) & 0xFF) << 32) | std::uint64_t(std::uint32_t(descriptor.usage))))
            std::fprintf(stderr, "CP2077 WebGPU: skipped texture discard: ref=%u slice=%d slices=%u w=%u h=%u fmt=%u usage=0x%x usageEx=0x%x\n",
                unsigned(reference.ID()), int(slice), unsigned(descriptor.CalcTargetSlices()), unsigned(descriptor.width),
                unsigned(descriptor.height), unsigned(descriptor.format), unsigned(descriptor.usage), unsigned(descriptor.usageEx));
        return;
    }
    // Discard is a content-undefined optimization hint, not a zero clear. WebGPU
    // has no standalone discardResource operation. Its ended-pass/one-queue
    // ordering and real lifetime hold preserve valid native discard semantics;
    // contents remain undefined to the caller until the next write.
    HoldBrowserTextures(1,&reference,BrowserResourceUse::Retire);
}
void ClearBufferUAV_Zero(const BufferRef& ref) {
    BufferDesc descriptor; WGPUBuffer buffer;
    if (ref.isNull() || !cp2077::web::AcquireGpuBuffer(ref,descriptor,buffer)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    bool valid = IsBrowserStorageBuffer(descriptor.category,true), transferred = false;
    if (valid) {
        std::lock_guard<std::mutex> lock(commandMutex);
        auto entry = commands.find(bound.ID());
        if (entry != commands.end() && entry->second.owner == std::this_thread::get_id() && !entry->second.failed) {
            auto& command = entry->second;
            // Original engine logical size can be 2-byte index data. Browser
            // allocations round to 4 bytes; clearing inaccessible padding is safe.
            const std::uint64_t bytes = (std::uint64_t(descriptor.size)+3)&~std::uint64_t(3);
            // Queued draws/copies precede this clear in encoder order. Queued itself (encoded by the next batched
            // main-thread flush) instead of a flush plus a synchronous handoff under commandMutex; the buffer
            // stays alive through retainedBuffers until GPU retirement.
            if (QueuePendingCustom(command,[buffer,bytes](WGPUCommandEncoder encoder){return cp2077::web::EncodeGpuBufferZero(encoder,buffer,bytes);})) {
                command.retainedBuffers.push_back(ref); transferred = true;
            } else MarkBrowserCommandFailed(command, __func__, __LINE__);
        }
    }
    if (!transferred) {
        Release(ref); cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: original UAV buffer clear failed\n", stderr);
    }
}
