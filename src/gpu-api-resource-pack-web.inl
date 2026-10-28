// Original eight-slot material resource packs, translated into the existing
// real WebGPU bindings. A pack owns native resources independently of its caller.
namespace {
struct BrowserResourcePack {
    GpuApi::ResourcePackDesc descriptor;
    Int32 references = 1;
};
std::mutex packMutex;
std::unordered_map<Uint32, BrowserResourcePack> resourcePacks;
Uint32 nextPackId = 1;
void ReleasePackEntries(const GpuApi::ResourcePackDesc& descriptor) {
    for (const auto& entry : descriptor.entries) {
        if (entry.type == GpuApi::ResourcePackEntryType::Texture && !entry.texture.isNull()) GpuApi::Release(entry.texture);
        if (entry.type == GpuApi::ResourcePackEntryType::Buffer && !entry.buffer.isNull()) GpuApi::Release(entry.buffer);
    }
}
void ShutdownBrowserResourcePacks() {
    std::unordered_map<Uint32, BrowserResourcePack> retired;
    { std::lock_guard<std::mutex> lock(packMutex); retired.swap(resourcePacks); }
    for (const auto& item : retired) ReleasePackEntries(item.second.descriptor);
}
}
namespace GpuApi {
ResourcePackRef CreateResourcePack(const ResourcePackDesc& descriptor) {
    ResourcePackDesc acquired;
    bool valid = true;
    for (Uint32 index = 0; valid && index < c_resourcePackSize; ++index) {
        const auto& entry = descriptor.entries[index];
        switch (entry.type) {
        case ResourcePackEntryType::Null: break;
        case ResourcePackEntryType::Texture:
            if (!entry.texture.isNull()) {
                valid = cp2077::web::RetainGpuTexture(entry.texture);
                if (valid) acquired.entries[index] = entry;
                if (valid && !GetTextureDesc(entry.texture).HasUsage(TEXUSAGE_Samplable)) valid = false;
            }
            break;
        case ResourcePackEntryType::Buffer: {
            BufferDesc bufferDescriptor; WGPUBuffer native;
            valid = !entry.buffer.isNull() && cp2077::web::AcquireGpuBuffer(entry.buffer, bufferDescriptor, native);
            if (valid) {
                acquired.entries[index] = entry;
                valid = bufferDescriptor.category == GpuWrapApi::BCC_Structured || bufferDescriptor.category == GpuWrapApi::BCC_StructuredUAV ||
                    bufferDescriptor.category == GpuWrapApi::BCC_StructuredAppendUAV;
            }
            break;
        }
        default: valid = false; break;
        }
    }
    Uint32 id = 0;
    if (valid) {
        std::lock_guard<std::mutex> lock(packMutex);
        if (nextPackId <= Uint32(INT32_MAX)) {
            id = nextPackId++;
            resourcePacks.emplace(id, BrowserResourcePack{acquired, 1});
        }
    }
    if (!id) ReleasePackEntries(acquired);
    return ResourcePackRef{id};
}
void AddRef(const ResourcePackRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(packMutex);
    const auto found = resourcePacks.find(reference.ID());
    if (found != resourcePacks.end() && found->second.references < INT32_MAX) ++found->second.references;
    else std::fputs("CP2077 WebGPU: invalid resource pack reference\n", stderr);
}
Int32 Release(const ResourcePackRef& reference) {
    if (reference.isNull()) return 0;
    ResourcePackDesc retired;
    Int32 remaining;
    {
        std::lock_guard<std::mutex> lock(packMutex);
        const auto found = resourcePacks.find(reference.ID());
        if (found == resourcePacks.end()) return 0;
        remaining = --found->second.references;
        if (!remaining) { retired = found->second.descriptor; resourcePacks.erase(found); }
    }
    if (!remaining) ReleasePackEntries(retired);
    return remaining;
}
void BindResourcePack(ResourcePackRef reference, GpuWrapApi::eShaderType stage) {
    if (reference.isNull()) return; // Match native null-pack semantics.
    ResourcePackDesc descriptor;
    {
        std::lock_guard<std::mutex> lock(packMutex);
        const auto found = resourcePacks.find(reference.ID());
        if (found == resourcePacks.end() || found->second.references == INT32_MAX) {
            cp2077::web::FailBoundGpuCommand(); return;
        }
        ++found->second.references;
        descriptor = found->second.descriptor;
    }
    for (Uint32 index = 0; index < c_resourcePackRange1Size; ++index) {
        const auto slot = c_resourcePackRange1SlotBegin + index;
        const auto& entry = descriptor.entries[c_resourcePackRange1IndexBegin + index];
        if (entry.type == ResourcePackEntryType::Buffer) BindBufferSRV(entry.buffer, slot, stage);
        else BindTexture(slot, entry.type == ResourcePackEntryType::Texture ? entry.texture : TextureRef::Null(), stage);
    }
    Release(reference);
}
}
