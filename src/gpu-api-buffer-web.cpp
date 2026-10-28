#include "skperf-api-timer-web.h" // [skperf] round 39
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-api-buffer-web.h"
#include "gpu-buffer-web.h"
#include "gpu-command-web.h"
#include "gpu-api-command-web.h"
#include "gpu-api-texture-web.h"
#include "mapped-buffer-diff-web.h"
#include <emscripten/emscripten.h>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <vector>
#include <thread>
#include "gpu-api-resource-pack-web.inl"
#include "gpu-api-buffer-grab-web.inl"

namespace {
struct BufferEntry {
    GpuApi::BufferDesc descriptor;
    WGPUBuffer buffer;
    Uint64 allocatedBytes;
    Int32 references = 1;
    GpuApi::BufferRef backing = GpuApi::BufferRef::Null();
    std::vector<unsigned char> mappedBytes;
    std::vector<unsigned char> committedMappedBytes;
    // Opt-in: owner-reported GPU-readable ranges (offset -> end). When set, only these are diffed at submit.
    bool liveRangeTracked = false;
    bool liveRangeFullScanPending = false;
    std::map<std::size_t, std::size_t> liveRanges;
    // [skperf] round 23 mapTrack: owner-reported dirty pages (see NoteMappedGpuWrite)
    std::unique_ptr<std::atomic<std::uint64_t>[]> dirtyWords;
    std::unique_ptr<cp2077::web::MappedWriteTracker> tracker;
    std::vector<unsigned char> committedConstantBytes;
    // [skperf] round 57 cbCow (?cbCow=1): immutable copy of committedConstantBytes, swapped under bufferMutex and read
    // (memcpy) outside it. Set together with committedConstantBytes at its two writers; null with the switch off.
    std::shared_ptr<const std::vector<unsigned char>> committedShared;
    std::vector<unsigned char> orderedConstantBytes; // [flicker3] value as of the last SUBMITTED Unlock (submission order)
    WGPUBuffer counter = nullptr;
    bool nativeGpuConstantBacking = false;
    bool readbackLocked = false;
    bool readbackReady = false;
    std::thread::id readbackOwner;
};
std::mutex bufferMutex;
// [skperf] round 57 cbCow: read once, never while bufferMutex is held (MAIN_THREAD proxy).
bool CpCbCowOn() {
    static const int v = [] {
        const int r = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('cbCow') === '1' ? 1 : 0; } catch (e) { return 0; } });
        std::fprintf(stderr, "CP2077 skperf switches: cbCow=%d\n", r); return r; }();
    return v != 0;
}
std::unordered_map<Uint32, std::unique_ptr<BufferEntry>> buffers;
Uint32 nextBufferId = 1;
// perf10: IDs of the persistently mapped non-constant buffers (the only ones FlushOriginalMappedGpuBuffers uploads),
// so a flush visits a handful of entries instead of the whole registry. Guarded by bufferMutex.
std::vector<Uint32> mappedUploadIds;
// [skperf] round 25 mapIdsCache: bumped (under bufferMutex) whenever mappedUploadIds changes, so submitters can reuse
// their per-thread copy without taking the hot bufferMutex every submit. ?mapIdsCache=0 = always copy under the lock.
std::atomic<unsigned> mappedUploadIdsVersion{1};
bool IsMappedUploadEntry(const BufferEntry& entry) {
    return entry.descriptor.usage == GpuApi::BUT_Mapped && entry.descriptor.category != GpuWrapApi::BCC_Constant;
}
// Set by a submitter that already flushed every mapped buffer its batch references (see
// FlushOriginalMappedGpuBufferIds); the next FlushOriginalMappedGpuBuffers() on that thread is then a no-op.
thread_local bool mappedFlushCovered = false;
std::atomic<unsigned> mappedFlushCoveredCalls{0};
bool UsesOrderedUploadLock(const GpuApi::BufferDesc& descriptor) {
    if (descriptor.usage != GpuApi::BUT_Dynamic_Legacy && descriptor.usage != GpuApi::BUT_Default) return false;
    // Original DX12 locks ordinary buffers into an in-frame upload region
    // and copies precisely that range at UnlockBuffer. Vertex/index buffers
    // use the same operation; untouched bytes and earlier GPU reads survive.
    switch (descriptor.category) {
    case GpuWrapApi::BCC_Vertex:
    case GpuWrapApi::BCC_VertexUAV:
    case GpuWrapApi::BCC_Index16Bit:
    case GpuWrapApi::BCC_Index32Bit:
    case GpuWrapApi::BCC_VertexIndex16Bit:
    case GpuWrapApi::BCC_Index16BitUAV:
    case GpuWrapApi::BCC_Structured:
    case GpuWrapApi::BCC_StructuredUAV:
    case GpuWrapApi::BCC_Raw: return true;
    default: return false;
    }
}
WGPUBufferUsage TranslateUsage(const GpuApi::BufferDesc& descriptor) {
    if(descriptor.usage==GpuApi::BUT_Readback)
        return descriptor.category==GpuWrapApi::BCC_Staging ? WGPUBufferUsage_MapRead|WGPUBufferUsage_CopyDst : WGPUBufferUsage_None;
    WGPUBufferUsage usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    switch (descriptor.category) {
    case GpuWrapApi::BCC_Staging: return usage;
    // Original DX12 creates a raw SRV for ordinary vertex/index buffers too.
    case GpuWrapApi::BCC_Vertex: return usage | WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_VertexUAV: return usage | WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_Index16Bit:
    case GpuWrapApi::BCC_Index32Bit: return usage | WGPUBufferUsage_Index | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_VertexIndex16Bit: return usage | WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_Constant: return usage | WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage; // [nomadperf] ovfStorage
    case GpuWrapApi::BCC_Structured:
    case GpuWrapApi::BCC_StructuredUAV:
    case GpuWrapApi::BCC_StructuredAppendUAV:
    case GpuWrapApi::BCC_Raw: return usage | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_Index16BitUAV: return usage | WGPUBufferUsage_Index | WGPUBufferUsage_Storage;
    case GpuWrapApi::BCC_IndirectUAV: return usage | WGPUBufferUsage_Indirect | WGPUBufferUsage_Storage;
    // R32_UINT typed buffers: converted shaders lower RWBuffer<uint> to array<u32> (element i = bytes 4i..4i+3, the
    // exact DX12 R32_UINT typed view). Other typed formats still need packing and stay unsupported.
    case GpuWrapApi::BCC_TypedUAV:
        return descriptor.format == GpuWrapApi::TEXFMT_R32_Uint ? usage | WGPUBufferUsage_Storage : WGPUBufferUsage_None;
    // Typed views and append counters require shader/view translation first.
    default: return WGPUBufferUsage_None;
    }
}
}

namespace GpuApi {
Bool IsBufferAliasable(const BufferDesc& descriptor) {
    // Native constant data currently uses per-draw CPU snapshots keyed by
    // logical ref. Sharing GPU backing alone cannot alias those snapshots.
    return descriptor.size && descriptor.category != GpuWrapApi::BCC_Constant &&
        descriptor.category != GpuWrapApi::BCC_StructuredAppendUAV && TranslateUsage(descriptor) &&
        (descriptor.usage == BUT_Default || descriptor.usage == BUT_Immutable ||
         descriptor.usage == BUT_Dynamic_Legacy) &&
        (!(descriptor.category == GpuWrapApi::BCC_Structured || descriptor.category == GpuWrapApi::BCC_StructuredUAV) ||
         (descriptor.structureStride && descriptor.size % descriptor.structureStride == 0));
}
Bool CanBuffersAlias(BufferRef existing, const BufferDesc& descriptor) {
    if (!IsBufferAliasable(descriptor)) return false;
    if (existing.isNull()) return true;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto found = buffers.find(existing.ID());
    if (found == buffers.end()) return false;
    const auto& entry = *found->second;
    return IsBufferAliasable(entry.descriptor) && entry.descriptor.group == descriptor.group &&
        ((Uint64(descriptor.size) + 3) & ~Uint64(3)) <= entry.allocatedBytes &&
        (TranslateUsage(descriptor) & ~TranslateUsage(entry.descriptor)) == 0;
}
BufferRef CreateAliasingBuffer(BufferRef existing, const BufferDesc& descriptor) {
    if (!IsBufferAliasable(descriptor)) return BufferRef::Null();
    if (existing.isNull()) return CreateBuffer(descriptor);
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto found = buffers.find(existing.ID());
    if (found == buffers.end() || nextBufferId > Uint32(INT32_MAX)) return BufferRef::Null();
    auto& entry = *found->second;
    if (!IsBufferAliasable(entry.descriptor) || entry.references == INT32_MAX ||
        entry.descriptor.group != descriptor.group ||
        ((Uint64(descriptor.size) + 3) & ~Uint64(3)) > entry.allocatedBytes ||
        (TranslateUsage(descriptor) & ~TranslateUsage(entry.descriptor))) return BufferRef::Null();
    const auto id = nextBufferId++;
    auto alias = std::make_unique<BufferEntry>(BufferEntry{descriptor, entry.buffer, entry.allocatedBytes, 1, existing});
    buffers.emplace(id, std::move(alias));
    ++entry.references; // The alias owns its backing, including chains of aliases.
    return BufferRef{id};
}
void* LockBuffer(const BufferRef& reference, Uint32 flags, Uint32 offset, Uint32 size) {
    CpApiTimer cpApiTimer(0); // [skperf] round 39
    BufferDesc descriptor;
    WGPUBuffer native = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(reference, descriptor, native)) {
        cp2077::web::FailBoundGpuCommand(); return nullptr;
    }
    // [gamerender] Original DX12 LockBuffer: "if ( !ref || size < 1 ) return nullptr;" - no error, and callers
    // (UpdateBuffer) skip the unlock. Empty per-frame updates (e.g. zero instances) must not fail the list.
    if(!size){ Release(reference); return nullptr; }
    if(descriptor.usage==BUT_Readback){
        void* result=nullptr;Uint64 mappedSize=0;
        {
            std::lock_guard<std::mutex> guard(bufferMutex);
            auto found=buffers.find(reference.ID());
            if(found!=buffers.end() && flags==BLF_Read && size && offset<descriptor.size &&
                Uint64(offset)+size<=descriptor.size && !found->second->readbackLocked){
                auto& entry=*found->second;
                entry.mappedBytes.resize(descriptor.size);
                entry.readbackLocked=true;entry.readbackReady=false;entry.readbackOwner=std::this_thread::get_id();
                result=entry.mappedBytes.data();mappedSize=entry.allocatedBytes;
            }
        }
        // Retained reference and exclusive lock state keep the real CPU snapshot
        // alive across async GPU mapping. Caller must have submitted its copy.
        const bool read=result && cp2077::web::ReadGpuBuffer(native,mappedSize,result,descriptor.size);
        {
            std::lock_guard<std::mutex> guard(bufferMutex);
            auto found=buffers.find(reference.ID());
            if(found!=buffers.end() && found->second->readbackOwner==std::this_thread::get_id()){
                auto& entry=*found->second;
                if(read)entry.readbackReady=true;
                else if(result){entry.readbackLocked=false;entry.readbackOwner={};std::vector<unsigned char>().swap(entry.mappedBytes);}
            }
        }
        if(read)return static_cast<unsigned char*>(result)+offset;
        Release(reference);cp2077::web::FailBoundGpuCommand();return nullptr;
    }
    if (UsesOrderedUploadLock(descriptor)) {
        auto* data = cp2077::web::LockBoundGpuBufferUpload(reference,native,descriptor.size,flags,offset,size);
        if (!data) { Release(reference); cp2077::web::FailBoundGpuCommand(); }
        else if (descriptor.category==GpuWrapApi::BCC_Vertex && descriptor.usage==BUT_Default) {
            static std::atomic<unsigned> reports{0};
            if(reports.fetch_add(1)<16)
                std::fprintf(stderr,"CP2077 buffer upload probe: ref=%u category=%u usage=%u flags=%u offset=%u size=%u total=%u\n",
                    reference.ID(),unsigned(descriptor.category),unsigned(descriptor.usage),flags,offset,size,descriptor.size);
        }
        return data;
    }
    if (descriptor.category != GpuWrapApi::BCC_Constant ||
        (descriptor.usage != BUT_Dynamic_Legacy && descriptor.usage != BUT_Transient)) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 16)
            std::fprintf(stderr,"CP2077 buffer lock probe: ref=%u category=%u usage=%u flags=%u offset=%u size=%u total=%u stride=%u\n",
                reference.ID(),unsigned(descriptor.category),unsigned(descriptor.usage),flags,offset,size,descriptor.size,descriptor.structureStride);
        Release(reference); cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: buffer lock mode/category requires further translation\n", stderr); return nullptr;
    }
    auto* data = cp2077::web::LockBoundGpuConstantBuffer(reference, descriptor.size, flags, offset, size);
    if (!data) { Release(reference); cp2077::web::FailBoundGpuCommand(); }
    return data;
}
void UnlockBuffer(const BufferRef& reference) {
    CpApiTimer cpApiTimer(0); // [skperf] round 39
    if (cp2077::web::CpCbKeepOn()) { // [skperf] round 61 cbKeep: the temporary acquire+release becomes one locked peek
        BufferDesc descriptor; WGPUBuffer native = nullptr;
        if (!cp2077::web::PeekGpuBuffer(reference,descriptor,native)) { cp2077::web::FailBoundGpuCommand(); return; }
        if (descriptor.usage != BUT_Readback) {
            if (UsesOrderedUploadLock(descriptor)) {
                if (!cp2077::web::UnlockBoundGpuBufferUpload(reference)) cp2077::web::FailBoundGpuCommand();
                return;
            }
            if (!cp2077::web::UnlockBoundGpuConstantBuffer(reference)) cp2077::web::FailBoundGpuCommand();
            return;
        }
        // readback: original path below
    }
    BufferDesc descriptor; WGPUBuffer native = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(reference,descriptor,native)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    if(descriptor.usage==BUT_Readback){
        bool unlocked=false;
        {
            std::lock_guard<std::mutex> guard(bufferMutex);
            auto found=buffers.find(reference.ID());
            if(found!=buffers.end() && found->second->readbackLocked && found->second->readbackReady &&
                found->second->readbackOwner==std::this_thread::get_id()){
                auto& entry=*found->second;entry.readbackLocked=false;entry.readbackReady=false;entry.readbackOwner={};
                std::vector<unsigned char>().swap(entry.mappedBytes);unlocked=true;
            }
        }
        Release(reference); // Unlock's temporary acquisition.
        if(unlocked)Release(reference); // Lock's retained snapshot ownership.
        else cp2077::web::FailBoundGpuCommand();
        return;
    }
    const bool upload = UsesOrderedUploadLock(descriptor);
    Release(reference);
    if (upload) {
        if (!cp2077::web::UnlockBoundGpuBufferUpload(reference)) cp2077::web::FailBoundGpuCommand();
        return;
    }
    if (!cp2077::web::UnlockBoundGpuConstantBuffer(reference))
        cp2077::web::FailBoundGpuCommand();
}
void BindConstantBuffer(Uint32 slot, BufferRef reference, GpuWrapApi::eShaderType stage) {
    if (stage != GpuWrapApi::VertexShader && stage != GpuWrapApi::PixelShader && stage != GpuWrapApi::ComputeShader) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    BindConstantBuffer(slot, reference, GpuWrapApi::ShaderTypeMask(stage));
}
void BindConstantBuffer(Uint32 slot, BufferRef reference, GpuWrapApi::ShaderTypeMask stages) {
    CpApiTimer cpApiTimer(1); // [skperf] round 39
    if (!stages) return;
    const GpuWrapApi::ShaderTypeMask supported(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader);
    if ((stages.Get() & ~supported.Get()) || slot >= MAX_CONSTANT_BUFFERS) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    const bool selected[3]{stages.Has(GpuWrapApi::VertexShader), stages.Has(GpuWrapApi::PixelShader), stages.Has(GpuWrapApi::ComputeShader)};
    BufferRef acquired[3]{};
    bool valid = true;
    if (cp2077::web::CpCbKeepOn()) { // [skperf] round 61 cbKeep: all selected stages' references in one lock
        int uses = 0; for (unsigned stage = 0; stage < 3; ++stage) if (selected[stage] && !reference.isNull()) ++uses;
        if (uses) {
            BufferDesc descriptor; WGPUBuffer native = nullptr;
            if (!cp2077::web::AcquireGpuBufferMulti(reference, uses, descriptor, native)) valid = false;
            else {
                for (unsigned stage = 0; stage < 3; ++stage) if (selected[stage]) acquired[stage] = reference;
                if (descriptor.category != GpuWrapApi::BCC_Constant) valid = false;
            }
        }
        if (valid) valid = cp2077::web::SetBoundGpuConstantBuffers(slot, acquired, selected);
        if (!valid) { cp2077::web::ReleaseGpuBuffersBatch(acquired, 3); cp2077::web::FailBoundGpuCommand(); }
        return;
    }
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!selected[stage] || reference.isNull()) continue;
        BufferDesc descriptor;
        WGPUBuffer native = nullptr;
        if (!cp2077::web::AcquireGpuBuffer(reference, descriptor, native)) { valid = false; break; }
        acquired[stage] = reference;
        if (descriptor.category != GpuWrapApi::BCC_Constant) { valid = false; break; }
    }
    if (valid) valid = cp2077::web::SetBoundGpuConstantBuffers(slot, acquired, selected);
    if (!valid) {
        for (auto held : acquired) if (!held.isNull()) Release(held);
        cp2077::web::FailBoundGpuCommand();
    }
}
void BindBufferSRV(const BufferRef& reference, Uint32 slot, GpuWrapApi::eShaderType stage) {
    if (stage != GpuWrapApi::VertexShader && stage != GpuWrapApi::PixelShader && stage != GpuWrapApi::ComputeShader) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: unsupported buffer SRV stage\n", stderr); return;
    }
    BindBufferSRV(reference, slot, GpuWrapApi::ShaderTypeMask(stage));
}
void BindBufferUAV(const BufferRef& reference, Uint32 slot, Uint32 initialCount) {
    if (slot >= MAX_UAV_BINDINGS) { cp2077::web::FailBoundGpuCommand(); return; }
    BufferDesc descriptor; WGPUBuffer native = nullptr, counter = nullptr;
    Uint32 stride = 0;
    bool valid = reference.isNull();
    if (!reference.isNull() && cp2077::web::AcquireGpuBuffer(reference, descriptor, native)) {
        if (descriptor.usage == BUT_Mapped) {
            // Upload shadows preserve CPU writes before GPU submission. A UAV
            // could write back behind that shadow and needs a different contract.
            Release(reference);
            cp2077::web::FailBoundGpuCommand();
            std::fputs("CP2077 WebGPU: mapped upload buffers cannot be bound for GPU writes\n", stderr);
            return;
        }
        switch (descriptor.category) {
        case GpuWrapApi::BCC_StructuredUAV: stride = descriptor.structureStride; valid = true; break;
        case GpuWrapApi::BCC_StructuredAppendUAV: {
            stride = descriptor.structureStride;
            valid = cp2077::web::AcquireGpuBufferCounter(reference,counter);
            if (valid && initialCount != UINT32_MAX) {
                const auto upload = cp2077::web::CreateGpuBuffer(4,WGPUBufferUsage_CopySrc,&initialCount,4,"cp-uav-count-upload");
                if (!upload || !cp2077::web::RecordGpuBufferCopy(upload,0,counter,0,4,BufferRef::Null(),reference)) {
                    if (upload) cp2077::web::DestroyGpuBuffer(upload);
                    Release(reference); valid = false;
                }
                // Successful copy transferred this counter's extra original ref.
            } else if (valid) Release(reference);
            break;
        }
        case GpuWrapApi::BCC_Raw:
        case GpuWrapApi::BCC_VertexUAV:
        case GpuWrapApi::BCC_Index16BitUAV:
        case GpuWrapApi::BCC_IndirectUAV: stride = 4; valid = descriptor.size % 4 == 0; break;
        // descriptor.format shares storage with structureStride: never read the stride of a typed buffer.
        case GpuWrapApi::BCC_TypedUAV:
            stride = 4; valid = descriptor.format == GpuWrapApi::TEXFMT_R32_Uint && descriptor.size % 4 == 0; break;
        default: break;
        }
    }
    if (valid) valid = cp2077::web::SetBoundGpuStorageBuffer(slot,reference,native,native ? descriptor.size : 0,stride,counter);
    if (!valid) {
        if (native) Release(reference);
        cp2077::web::FailBoundGpuCommand();
    }
}
void BindBufferSRV(const BufferRef& reference, Uint32 slot, GpuWrapApi::ShaderTypeMask stages) {
    if (!stages) return;
    // Original DX12 BindBufferSRV leaves the descriptor table unchanged for
    // a null buffer; this is not an unbind or a request for fabricated data.
    if (reference.isNull()) return;
    const GpuWrapApi::ShaderTypeMask supported(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader);
    if ((stages.Get() & ~supported.Get()) || slot >= MAX_SRV_BINDINGS) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    const bool selected[3] = {stages.Has(GpuWrapApi::VertexShader), stages.Has(GpuWrapApi::PixelShader),
                              stages.Has(GpuWrapApi::ComputeShader)};
    BufferRef acquired[3]{};
    BufferDesc descriptor{};
    WGPUBuffer native = nullptr;
    bool valid = true;
    bool raw = false;
    const char* rejected = nullptr;
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!selected[stage] || reference.isNull()) continue;
        if (!cp2077::web::AcquireGpuBuffer(reference, descriptor, native)) {
            valid = false; rejected = "stale ref"; break;
        }
        acquired[stage] = reference;
        switch (descriptor.category) {
        case GpuWrapApi::BCC_Raw:
        case GpuWrapApi::BCC_IndirectUAV: // Original DX12 views indirect args as a byte buffer.
        case GpuWrapApi::BCC_Vertex:
        case GpuWrapApi::BCC_VertexUAV:
        case GpuWrapApi::BCC_Index16Bit:
        case GpuWrapApi::BCC_Index32Bit:
        case GpuWrapApi::BCC_Index16BitUAV:
        case GpuWrapApi::BCC_VertexIndex16Bit:
            raw = true; break;
        case GpuWrapApi::BCC_Structured:
        case GpuWrapApi::BCC_StructuredUAV:
        case GpuWrapApi::BCC_StructuredAppendUAV:
            break;
        default: valid = false; rejected = "unsupported category"; break;
        }
        if (!valid) break;
    }
    // Raw native SRVs address 32-bit words; structured SRVs retain their exact
    // declared element stride. The backing and bytes remain the real buffer.
    const Uint32 stride = raw ? 4 : descriptor.structureStride;
    if (valid && (!stride || descriptor.size % stride)) { valid = false; rejected = "size/stride mismatch"; }
    if (valid) {
        // A false return here was already reported, or the list was already failed.
        if (!cp2077::web::SetBoundGpuStructuredBuffers(slot, acquired, native, descriptor.size, stride, selected)) {
            for (auto held : acquired) if (!held.isNull()) Release(held);
            cp2077::web::FailBoundGpuCommand();
        }
        return;
    }
    // A bad SRV affects only itself: clear the slot (never keep the previous
    // buffer) so draws/dispatches that read it are skipped by their own checks.
    for (auto held : acquired) if (!held.isNull()) Release(held);
    const BufferRef none[3]{};
    if (cp2077::web::SetBoundGpuStructuredBuffers(slot, none, nullptr, 0, 0, selected)) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 16)
            std::fprintf(stderr, "CP2077 WebGPU: buffer SRV left unbound (%s): ref=%u category=%u slot=%u size=%u stride=%u\n",
                rejected ? rejected : "invalid", reference.ID(), unsigned(descriptor.category), slot,
                unsigned(descriptor.size), unsigned(descriptor.structureStride));
    }
}
BufferRef CreateBuffer(const BufferDesc& descriptor, const BufferInitData& initial) {
    if (!descriptor.size || descriptor.usage >= BUT_MAX) return BufferRef::Null();
    if (descriptor.usage == BUT_ImmutableInPlace) {
        std::fputs("CP2077 WebGPU: immutable in-place buffer mode not implemented\n", stderr);
        return BufferRef::Null();
    }
    if(descriptor.usage==BUT_Readback && (descriptor.category!=GpuWrapApi::BCC_Staging || initial.m_mode==BufferInitData::Mode::Data)){
        std::fputs("CP2077 WebGPU: readback buffers require staging category without CPU initializer\n",stderr);return BufferRef::Null();
    }
    // Original UI transient constants are renamed through per-command snapshots
    // by LockBuffer/UnlockBuffer. Font atlas raw uploads already record genuine
    // staging copies and retain their backing until GPU completion. Transient
    // vertex/index lock renaming needs its own integration before enabling it.
    if (descriptor.usage == BUT_Transient && descriptor.category != GpuWrapApi::BCC_Constant &&
        descriptor.category != GpuWrapApi::BCC_Raw) {
        std::fputs("CP2077 WebGPU: transient buffer category requires lock/rename integration\n", stderr);
        return BufferRef::Null();
    }
    if (descriptor.usage == BUT_Mapped && descriptor.category != GpuWrapApi::BCC_Constant &&
        descriptor.category != GpuWrapApi::BCC_Vertex && descriptor.category != GpuWrapApi::BCC_Index16Bit &&
        descriptor.category != GpuWrapApi::BCC_Index32Bit && descriptor.category != GpuWrapApi::BCC_VertexIndex16Bit &&
        descriptor.category != GpuWrapApi::BCC_Raw && descriptor.category != GpuWrapApi::BCC_Structured) {
        std::fputs("CP2077 WebGPU: persistent mapping with GPU writes requires bidirectional coherence\n", stderr);
        return BufferRef::Null();
    }
    if ((descriptor.category == GpuWrapApi::BCC_Structured ||
         descriptor.category == GpuWrapApi::BCC_StructuredUAV || descriptor.category == GpuWrapApi::BCC_StructuredAppendUAV) &&
        (!descriptor.structureStride || descriptor.size % descriptor.structureStride)) return BufferRef::Null();
    auto usage = TranslateUsage(descriptor);
    if (!usage) {
        std::fputs("CP2077 WebGPU: typed/append buffer views not implemented\n", stderr);
        return BufferRef::Null();
    }
    const void* data = nullptr;
    std::vector<unsigned char> filled;
    switch (initial.m_mode) {
    case BufferInitData::Mode::Uninitialized: break;
    case BufferInitData::Mode::ClearToZero: break; // WebGPU allocations are zero initialized.
    case BufferInitData::Mode::Data:
        if (initial.m_data.m_rawPtr) data = initial.m_data.m_rawPtr;
        else {
            if (!initial.m_data.m_fillFunc) return BufferRef::Null();
            filled.resize(descriptor.size);
            initial.m_data.m_fillFunc(filled.data());
            data = filled.data();
        }
        break;
    default: return BufferRef::Null();
    }
    {
        // Sampled creation probe: identifies which original systems create GPU buffers during steady frames.
        static std::atomic<unsigned> creations{0};
        const unsigned n = ++creations;
        if (n > 20000 && (n % 4096) == 0) {
            char stack[3072];
            const int length = emscripten_get_callstack(EM_LOG_C_STACK, stack, sizeof(stack));
            std::fprintf(stderr, "CP2077 buffer creation probe: n=%u usage=%u category=%u group=%u size=%u init=%u\n%.*s\n",
                n, unsigned(descriptor.usage), unsigned(descriptor.category), unsigned(descriptor.group),
                unsigned(descriptor.size), unsigned(initial.m_mode), length > 0 ? length : 0, stack);
        }
    }
    static const char* const cpCategoryLabels[] = {"cp-engine-buffer-c0","cp-engine-buffer-c1","cp-engine-buffer-c2","cp-engine-buffer-c3","cp-engine-buffer-c4","cp-engine-buffer-c5","cp-engine-buffer-c6","cp-engine-buffer-c7","cp-engine-buffer-c8","cp-engine-buffer-c9","cp-engine-buffer-c10","cp-engine-buffer-c11","cp-engine-buffer-c12","cp-engine-buffer-c13","cp-engine-buffer-c14","cp-engine-buffer-c15"};
    const char* cpLabel = descriptor.usage == BUT_Mapped ? "cp-engine-mapped" : cpCategoryLabels[unsigned(descriptor.category) & 15];
    auto native = cp2077::web::CreateGpuBuffer(descriptor.size, usage, data, data ? descriptor.size : 0, cpLabel);
    if (!native) return BufferRef::Null();
    WGPUBuffer counter = nullptr;
    if (descriptor.category == GpuWrapApi::BCC_StructuredAppendUAV) {
        counter = cp2077::web::CreateGpuBuffer(4,WGPUBufferUsage_Storage|WGPUBufferUsage_CopySrc|WGPUBufferUsage_CopyDst,nullptr,0,"cp-uav-counter");
        if (!counter) { cp2077::web::DestroyGpuBuffer(native); return BufferRef::Null(); }
    }
    Uint32 id = 0;
    const bool cpCow = CpCbCowOn(); // [skperf] round 57 (before the lock)
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        // Never recycle an ID into a stale original BufferRef. Generation-aware
        // reuse can be added alongside the original resource container later.
        if (nextBufferId <= Uint32(INT32_MAX)) {
            id = nextBufferId++;
            auto entry = std::make_unique<BufferEntry>(BufferEntry{
                descriptor, native, (Uint64(descriptor.size) + 3) & ~Uint64(3), 1});
            entry->counter=counter;
            entry->nativeGpuConstantBacking = descriptor.category==GpuWrapApi::BCC_Constant &&
                descriptor.usage==BUT_Dynamic_Legacy && descriptor.group==GpuWrapApi::BufferGroup::ReflectionProbe;
            // Shader ABI repacking needs the genuine initializer bytes after
            // immutable material constants have been uploaded to the GPU.
            // Never-written buffers remain distinct from explicit zero/data.
            if (descriptor.category==GpuWrapApi::BCC_Constant &&
                (descriptor.usage==BUT_Immutable || descriptor.usage==BUT_Dynamic_Legacy || descriptor.usage==BUT_Default) &&
                (data || initial.m_mode==BufferInitData::Mode::ClearToZero)) {
                entry->committedConstantBytes.resize(descriptor.size);
                if(data)std::memcpy(entry->committedConstantBytes.data(),data,descriptor.size);
                entry->orderedConstantBytes=entry->committedConstantBytes; // [flicker3]
                if(cpCow)entry->committedShared=std::make_shared<const std::vector<unsigned char>>(entry->committedConstantBytes); // [skperf] round 57
            }
            if (descriptor.usage == BUT_Mapped) {
                entry->mappedBytes.resize(static_cast<std::size_t>(entry->allocatedBytes));
                if (data) std::memcpy(entry->mappedBytes.data(), data, descriptor.size);
                // Initializer bytes or WebGPU's guaranteed zeros are already
                // on the GPU. Keep that exact baseline for changed-page scans.
                entry->committedMappedBytes=entry->mappedBytes;
            }
            if (IsMappedUploadEntry(*entry)) { mappedUploadIds.push_back(id); mappedUploadIdsVersion.fetch_add(1, std::memory_order_release); }
            buffers.emplace(id, std::move(entry));
        }
    }
    if (!id) { cp2077::web::DestroyGpuBuffer(native); cp2077::web::DestroyGpuBuffer(counter); }
    else if (descriptor.usage == BUT_Mapped) {
        cp2077::web::SetGpuMappedBufferFlusher(cp2077::web::FlushOriginalMappedGpuBuffers);
        // Diagnostic: which persistently mapped buffers make up the per-submit dirty scan.
        std::fprintf(stderr,"CP2077 mapped buffer created: id=%u bytes=%u category=%u group=%u stride=%u\n",
            id,descriptor.size,unsigned(descriptor.category),unsigned(descriptor.group),unsigned(descriptor.structureStride));
    }
    return BufferRef{id};
}
void* GetMappedBufferPtr(const BufferRef& reference) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found = buffers.find(reference.ID());
    if (found == buffers.end() || found->second->descriptor.usage != BUT_Mapped) return nullptr;
    // Stable CPU upload memory, flushed to the real GPU allocation before every
    // queue submission. GPU-writable persistent mappings are not advertised.
    return found->second->mappedBytes.data();
}
void AddRef(const BufferRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    if (entry != buffers.end() && entry->second->references < INT32_MAX) ++entry->second->references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing buffer reference\n", stderr);
}
Int32 Release(const BufferRef& reference) {
    if (reference.isNull()) return 0;
    WGPUBuffer native = nullptr;
    Int32 remaining = 0;
    WGPUBuffer counter = nullptr;
    BufferRef backing = BufferRef::Null();
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        auto entry = buffers.find(reference.ID());
        if (entry == buffers.end()) {
            std::fputs("CP2077 WebGPU: release of stale buffer reference\n", stderr); return 0;
        }
        remaining = --entry->second->references;
        if (!remaining) { backing = entry->second->backing;
            counter=entry->second->counter;
            if (backing.isNull()) native = entry->second->buffer;
            if (IsMappedUploadEntry(*entry->second)) {
                mappedUploadIds.erase(std::remove(mappedUploadIds.begin(), mappedUploadIds.end(), reference.ID()), mappedUploadIds.end());
                mappedUploadIdsVersion.fetch_add(1, std::memory_order_release);
            }
            buffers.erase(entry); }
    }
    if (native && !cp2077::web::DestroyGpuBuffer(native))
        std::fputs("CP2077 WebGPU: buffer destruction proxy failed\n", stderr);
    if (!backing.isNull()) Release(backing);
    if (counter) cp2077::web::DestroyGpuBuffer(counter);
    return remaining;
}
const BufferDesc& GetBufferDesc(const BufferRef& reference) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    if (entry != buffers.end()) return entry->second->descriptor;
    // A valid caller holds a reference throughout use of the returned descriptor.
    std::fputs("CP2077 WebGPU: descriptor requested for stale buffer\n", stderr);
    static const BufferDesc empty;
    return empty;
}
Uint64 GetBufferUsedVideoMemory(BufferRef reference) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    return entry != buffers.end() ? entry->second->allocatedBytes + (entry->second->counter ? 4 : 0) : 0;
}
void LoadBufferData(const BufferRef& destination, Uint32 offset, Uint32 size, const void* source) {
    if (!size) return;
    BufferDesc descriptor;
    WGPUBuffer native = nullptr;
    if (!source || !cp2077::web::AcquireGpuBuffer(destination, descriptor, native)) {
        std::fputs("CP2077 WebGPU: invalid buffer upload source/destination\n", stderr); return;
    }
    const Uint64 end = Uint64(offset) + size;
    if (descriptor.category == GpuWrapApi::BCC_Constant && !cp2077::web::UsesNativeGpuConstantBacking(destination)) {
        // Skip only this upload (the constants stay stale); the command list stays valid.
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8)
            std::fprintf(stderr, "CP2077 WebGPU: skipped constant upload without native backing: ref=%u usage=%u offset=%u size=%u total=%u\n",
                destination.ID(), unsigned(descriptor.usage), offset, size, descriptor.size);
        Release(destination); return;
    }
    // Tail padding can be written without altering logical bytes. Arbitrary
    // unaligned subranges need a read/modify/write implementation first.
    if (descriptor.usage == BUT_Readback || descriptor.usage == BUT_Immutable || descriptor.usage == BUT_Mapped || offset % 4 || end > descriptor.size ||
        (size % 4 && end != descriptor.size)) {
        std::fputs("CP2077 WebGPU: immutable, out-of-range or unaligned buffer upload\n", stderr);
        Release(destination); return;
    }
    Uint64 copySize = (Uint64(size) + 3) & ~Uint64(3);
    auto staging = cp2077::web::CreateGpuBuffer(copySize, WGPUBufferUsage_CopySrc, source, size, "cp-load-staging");
    if (!staging || !cp2077::web::RecordGpuBufferCopy(staging, 0, native, offset, copySize, BufferRef::Null(), destination)) {
        std::fputs("CP2077 WebGPU: buffer upload requires a valid bound command list\n", stderr);
        if (staging) cp2077::web::DestroyGpuBuffer(staging);
        Release(destination);
    }
}
void CopyBuffer(const BufferRef& destination, Uint32 destinationOffset,
                const BufferRef& source, Uint32 sourceOffset, Uint32 length) {
    if (!length) return;
    BufferDesc destinationDesc, sourceDesc;
    WGPUBuffer destinationNative = nullptr, sourceNative = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(destination, destinationDesc, destinationNative)) return;
    if (!cp2077::web::AcquireGpuBuffer(source, sourceDesc, sourceNative)) { Release(destination); return; }
    if ((destinationDesc.category == GpuWrapApi::BCC_Constant && !cp2077::web::UsesNativeGpuConstantBacking(destination)) ||
        (sourceDesc.category == GpuWrapApi::BCC_Constant && !cp2077::web::UsesNativeGpuConstantBacking(source))) {
        // Skip only this copy (the destination stays stale); the command list stays valid.
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8)
            std::fprintf(stderr, "CP2077 WebGPU: skipped constant-buffer copy without native backing: dst=%u src=%u dstUsage=%u srcUsage=%u dstOffset=%u srcOffset=%u length=%u\n",
                destination.ID(), source.ID(), unsigned(destinationDesc.usage), unsigned(sourceDesc.usage),
                destinationOffset, sourceOffset, length);
        Release(source); Release(destination); return;
    }
    bool valid = destinationDesc.usage != BUT_Immutable && destinationDesc.usage != BUT_Mapped &&
        Uint64(destinationOffset) + length <= destinationDesc.size &&
        Uint64(sourceOffset) + length <= sourceDesc.size &&
        !(destinationOffset % 4 || sourceOffset % 4 || length % 4) && destinationNative != sourceNative;
    if (!valid || !cp2077::web::RecordGpuBufferCopy(sourceNative, sourceOffset, destinationNative,
             destinationOffset, length, source, destination)) {
        std::fputs("CP2077 WebGPU: invalid buffer copy or missing command list\n", stderr);
        Release(source); Release(destination);
    }
}
void CopyStructureCount(const BufferRef& destination, Uint32 offset, const BufferRef& source) {
    BufferDesc descriptor; WGPUBuffer target = nullptr, counter = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(destination,descriptor,target)) { cp2077::web::FailBoundGpuCommand(); return; }
    if (offset % 4 || offset > descriptor.size || descriptor.size-offset < 4 ||
        descriptor.usage == BUT_Mapped || descriptor.usage == BUT_Transient ||
        descriptor.usage == BUT_Immutable || descriptor.category == GpuWrapApi::BCC_Constant ||
        !cp2077::web::AcquireGpuBufferCounter(source,counter)) {
        Release(destination); cp2077::web::FailBoundGpuCommand(); return;
    }
    if (!cp2077::web::RecordGpuBufferCopy(counter,0,target,offset,4,source,destination)) {
        Release(source); Release(destination); cp2077::web::FailBoundGpuCommand();
    }
    // Copy the real GPU counter in command order; no CPU count is substituted.
}
}

namespace cp2077::web {
bool UsesNativeGpuConstantBacking(const GpuApi::BufferRef& reference) {
    // [cmdlock] Called for every constant buffer of every draw; bufferMutex was a top worker contention point.
    // nativeGpuConstantBacking is fixed when the entry is created and buffer ids are never reused, so a per-thread
    // direct-mapped cache of (id -> flag) for live ids gives the same answer without the lock.
    struct Slot { Uint32 id = 0; bool native = false; };
    static thread_local Slot slots[1024];
    const Uint32 id = reference.ID();
    if (!id) return false;
    auto& slot = slots[(id * 2654435761u >> 22) & 1023];
    if (slot.id == id) return slot.native;
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(id);
    if (found==buffers.end()) return false;
    slot.id = id; slot.native = found->second->nativeGpuConstantBacking;
    return slot.native;
}
bool AcquireGpuBufferCounter(const GpuApi::BufferRef& reference, WGPUBuffer& counter) {
    counter=nullptr;
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto entry=buffers.find(reference.ID());
    if (entry==buffers.end() || !entry->second->counter || entry->second->references == INT32_MAX) return false;
    ++entry->second->references; counter=entry->second->counter; return true;
}
bool CommitGpuConstantBufferWrite(const GpuApi::BufferRef& reference,const void* data,std::size_t bytes) {
    // [skperf] round 57 cbCow: the shared copy is built before the lock; the replaced one is freed after it.
    const bool cpCow=CpCbCowOn();
    std::shared_ptr<const std::vector<unsigned char>> cpFresh, cpOld;
    if(cpCow && data && bytes){const auto* f=static_cast<const unsigned char*>(data);cpFresh=std::make_shared<const std::vector<unsigned char>>(f,f+bytes);}
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(reference.ID());
    if(found==buffers.end() || found->second->descriptor.category!=GpuWrapApi::BCC_Constant || !data || bytes!=found->second->descriptor.size) return false;
    auto& entry=*found->second;
    if(entry.descriptor.usage==GpuApi::BUT_Transient) return true;
    if(entry.descriptor.usage!=GpuApi::BUT_Dynamic_Legacy || !entry.backing.isNull()) return false;
    const auto* first=static_cast<const unsigned char*>(data);
    entry.committedConstantBytes.assign(first,first+bytes);
    if(cpCow){cpOld=std::move(entry.committedShared);entry.committedShared=std::move(cpFresh);}
    return true;
}
bool CopyCommittedGpuConstantBuffer(const GpuApi::BufferRef& reference,std::vector<unsigned char>& output) {
    const bool cpCow=CpCbCowOn(); // [skperf] round 57: with cbCow the bytes are copied after bufferMutex is released
    std::shared_ptr<const std::vector<unsigned char>> cpSnap;
    {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(reference.ID());
    if(found==buffers.end() || found->second->descriptor.category!=GpuWrapApi::BCC_Constant ||
       (found->second->descriptor.usage!=GpuApi::BUT_Dynamic_Legacy && found->second->descriptor.usage!=GpuApi::BUT_Immutable &&
        found->second->descriptor.usage!=GpuApi::BUT_Default) ||
       !found->second->backing.isNull()) return false;
    const auto& entry=*found->second;
    if (entry.nativeGpuConstantBacking) return false; // GPU writes invalidate CPU snapshots.
    if(entry.committedConstantBytes.size()!=entry.descriptor.size) return false;
    if(!cpCow || !entry.committedShared || entry.committedShared->size()!=entry.committedConstantBytes.size()){output=entry.committedConstantBytes;return true;}
    cpSnap=entry.committedShared;
    }
    output.assign(cpSnap->begin(),cpSnap->end());
    return true;
}
// [flicker3] see gpu-api-buffer-web.h. Flags fixed at creation (ids never reused) -> per-thread cache like above.
bool IsOrderedGpuConstant(const GpuApi::BufferRef& reference) {
    struct Slot { Uint32 id = 0; bool ordered = false; };
    static thread_local Slot slots[1024];
    const Uint32 id = reference.ID();
    if (!id) return false;
    auto& slot = slots[(id * 2654435761u >> 22) & 1023];
    if (slot.id == id) return slot.ordered;
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(id);
    if (found==buffers.end()) return false;
    const auto& entry=*found->second;
    slot.id = id;
    slot.ordered = entry.descriptor.category==GpuWrapApi::BCC_Constant && entry.descriptor.usage==GpuApi::BUT_Dynamic_Legacy &&
        entry.backing.isNull() && !entry.nativeGpuConstantBacking;
    return slot.ordered;
}
bool CopyOrderedGpuConstantBuffer(const GpuApi::BufferRef& reference,std::vector<unsigned char>& output) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(reference.ID());
    if(found==buffers.end()) return false;
    const auto& entry=*found->second;
    // Before any submitted write: the creation bytes (or, if none, the latest CPU-committed write as before).
    const auto& source = entry.orderedConstantBytes.size()==entry.descriptor.size ? entry.orderedConstantBytes : entry.committedConstantBytes;
    if(source.size()!=entry.descriptor.size) return false;
    output.assign(source.begin(), source.end());
    return true;
}
bool CommitOrderedGpuConstantBufferWrite(const GpuApi::BufferRef& reference,const void* data,std::size_t bytes) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found=buffers.find(reference.ID());
    if(found==buffers.end() || !data || bytes!=found->second->descriptor.size) return false;
    const auto* first=static_cast<const unsigned char*>(data);
    found->second->orderedConstantBytes.assign(first,first+bytes);
    return true;
}
bool CopyMappedGpuConstantBuffer(const GpuApi::BufferRef& reference, std::vector<unsigned char>& output) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found = buffers.find(reference.ID());
    if (found == buffers.end() || found->second->descriptor.usage != GpuApi::BUT_Mapped ||
        found->second->descriptor.category != GpuWrapApi::BCC_Constant) return false;
    output.assign(found->second->mappedBytes.begin(), found->second->mappedBytes.begin() + found->second->descriptor.size);
    return true;
}
std::atomic<unsigned long long> mapTrackPagedBytes{0}, mapTrackFullBytes{0};
const MappedWriteTracker* EnableMappedGpuWriteTracking(const GpuApi::BufferRef& reference) {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mapTrack') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: mapTrack=%d\n", v); return v; }();
    if (!on || reference.isNull()) return nullptr;
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found = buffers.find(reference.ID());
    if (found == buffers.end() || !IsMappedUploadEntry(*found->second) || found->second->mappedBytes.empty()) return nullptr;
    auto& entry = *found->second;
    if (!entry.tracker) {
        const std::size_t bytes = entry.mappedBytes.size(), pages = (bytes + 4095) >> 12, words = (pages + 63) >> 6;
        entry.dirtyWords.reset(new std::atomic<std::uint64_t>[words]);
        for (std::size_t i = 0; i < words; ++i) entry.dirtyWords[i].store(0, std::memory_order_relaxed);
        entry.tracker.reset(new MappedWriteTracker{entry.dirtyWords.get(), words, bytes});
        entry.liveRangeFullScanPending = true; // one full diff covers any write made before tracking began
        std::fprintf(stderr, "CP2077 skperf mapTrack: buffer %u (%zu bytes, %zu pages) write-tracked\n", unsigned(reference.ID()), bytes, pages);
    }
    return entry.tracker.get();
}
bool TrackMappedBufferRange(const GpuApi::BufferRef& reference, unsigned long long offset, unsigned long long size, bool live) {
    std::lock_guard<std::mutex> lock(bufferMutex);
    const auto found = buffers.find(reference.ID());
    if (found == buffers.end() || found->second->descriptor.usage != GpuApi::BUT_Mapped) return false;
    auto& entry = *found->second;
    if (!entry.liveRangeTracked) {
        // Opting in must not lose bytes written before tracking began: the next flush still scans the whole
        // buffer once, then only live ranges.
        entry.liveRangeTracked = true;
        entry.liveRangeFullScanPending = true;
    }
    if (!size) return true;
    const std::size_t begin = static_cast<std::size_t>(offset), end = static_cast<std::size_t>(offset + size);
    if (live) {
        auto& current = entry.liveRanges[begin];
        current = std::max(current, end);
    } else {
        // Allocations are disjoint, so the block's start identifies its range. The free path may only know the
        // requested size, not the allocator's rounded size, so the size is not compared.
        (void)end;
        entry.liveRanges.erase(begin);
    }
    return true;
}
// perf10: uploads changed bytes of the given mapped buffers (all mapped upload buffers when ids is null).
// The registry lock is held only to snapshot the entries and take a reference on each; the page diff
// (~10 MB of memcmp, ~0.8 ms) runs without bufferMutex, so binds/acquires on other workers no longer wait
// for it. Safe because mappedBytes/committedMappedBytes are sized at creation and never reallocated, the held
// reference keeps the entry alive, and committedMappedBytes is written only here, under flushMutex.
extern "C" void cp2077_skperf_crit_core_parts(double lockMs, double collectMs, double diffMs, double commitMs, double releaseMs, double bytes, double patches); // [skperf] round 26/27
// [skperf] round 27 mapNoDiff (default ON, ?mapNoDiff=0 = compare the dirty pages against the shadow first): pages the
// owner reported as written are uploaded as they are. Same GPU contents (unchanged bytes inside a written page are
// rewritten with their own value); saves the compare and the shadow copy for write-tracked buffers.
bool MapMergeOn() {
    static const int v = [] { const int x = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mapMerge') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: mapMerge=%d\n", x); return x; }();
    return v != 0;
}
// [skperf] round 28: per-buffer mapped-flush statistics (critpath probe only): every ~2 s one line with, per buffer,
// its size, mode (paged = write-tracked, live, full = whole-buffer diff), flushes, uploaded KB, patches and ms per second.
namespace cpMapStat {
struct Row { std::atomic<unsigned> id{0}; std::atomic<unsigned long long> size{0}, flushes{0}, bytes{0}, patches{0}, us{0}; std::atomic<int> mode{0}; };
Row rows[16];
std::atomic<double> windowStart{0};
}
void MapStatNote(unsigned id, std::size_t size, bool paged, bool live, std::size_t bytes, std::size_t patches, double ms) {
    using namespace cpMapStat;
    Row* row = nullptr;
    for (auto& r : rows) { unsigned cur = r.id.load(std::memory_order_acquire); if (cur == id) { row = &r; break; }
        if (!cur) { unsigned expected = 0; if (r.id.compare_exchange_strong(expected, id) || expected == id) { row = &r; break; } } }
    if (!row) return;
    row->size.store(size, std::memory_order_relaxed); row->mode.store(paged ? 2 : (live ? 1 : 0), std::memory_order_relaxed);
    row->flushes.fetch_add(1, std::memory_order_relaxed); row->bytes.fetch_add(bytes, std::memory_order_relaxed);
    row->patches.fetch_add(patches, std::memory_order_relaxed); row->us.fetch_add((unsigned long long)(ms * 1000.0), std::memory_order_relaxed);
    const double now = emscripten_get_now(); double start = windowStart.load();
    if (start == 0) { windowStart.compare_exchange_strong(start, now); return; }
    if (now - start < 2000 || !windowStart.compare_exchange_strong(start, now)) return;
    const double sec = (now - start) / 1000.0;
    char line[2000]; std::size_t used = 0;
    auto add = [&](const char* f, auto... a) { if (used + 1 >= sizeof(line)) return; const int n = std::snprintf(line + used, sizeof(line) - used, f, a...);
        if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1; };
    add("CP2077 skperf mapstat: window=%.1fs per-second:", sec);
    static const char* modes[3] = {"full", "live", "paged"};
    for (auto& r : rows) {
        const unsigned rid = r.id.load(); if (!rid) continue;
        const auto fl = r.flushes.exchange(0), by = r.bytes.exchange(0), pa = r.patches.exchange(0), us = r.us.exchange(0);
        if (!fl) continue;
        add(" [buf%u %lluKB %s flushes=%.0f upKB=%.0f patches=%.0f ms=%.1f]", rid, r.size.load() / 1024, modes[r.mode.load() % 3], fl / sec, by / 1024.0 / sec, pa / sec, us / 1000.0 / sec);
    }
    std::fprintf(stderr, "%s\n", line);
}
bool MapNoDiffOn() {
    static const int v = [] { const int x = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mapNoDiff') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: mapNoDiff=%d\n", x); return x; }();
    return v != 0;
}
extern "C" int cp2077_skperf_crit_on();
bool FlushMappedGpuBuffersCore(const unsigned* ids, std::size_t idCount, std::vector<DeferredMappedGpuWrite>* deferred = nullptr) {
    const double flushStarted=emscripten_get_now();
    // [skperf] round 26 sub-timers (critpath probe only)
    const bool cpParts = cp2077_skperf_crit_on() != 0; double cpLocked = 0, cpCollected = 0, cpDiffed = 0, cpCommitted = 0;
    double scanMs=0,writeMs=0;std::uint64_t scanBytes=0,patchBytes=0;unsigned patchCount=0;
    // Serialize commits across simultaneous command-list submissions. Engine
    // synchronization still governs writes through its stable raw mappings.
    static std::mutex flushMutex;
    std::lock_guard<std::mutex> flushGuard(flushMutex);
    if (cpParts) cpLocked = emscripten_get_now();
    struct Patch { std::size_t offset; std::vector<unsigned char> bytes; };
    struct Scan { Uint32 id; WGPUBuffer buffer; const unsigned char* current; unsigned char* committed; std::size_t bytes;
        bool live; bool fullScanPending; std::vector<std::pair<std::size_t,std::size_t>> liveRanges; std::vector<Patch> patches;
        std::atomic<std::uint64_t>* dirtyWords = nullptr; std::vector<std::pair<std::size_t,std::uint64_t>> takenPages; bool paged = false;
        std::size_t dirtyWordCount = 0; bool trackedPending = false; };
    static thread_local std::vector<Uint32> selected;
    std::vector<Scan> scans;
    std::vector<Uint32> fullScanCommitted;
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        if (ids) selected.assign(ids, ids + idCount); else selected = mappedUploadIds;
        scans.reserve(selected.size());
        for (const auto id : selected) {
            const auto found = buffers.find(id);
            if (found == buffers.end()) continue;
            auto& entry = *found->second;
            if (!IsMappedUploadEntry(entry) || entry.mappedBytes.empty()) continue;
            if (entry.committedMappedBytes.size()!=entry.mappedBytes.size() || entry.references == INT32_MAX) {
                for (const auto& scan : scans) --buffers.at(scan.id)->references;
                return false;
            }
            ++entry.references;
            Scan scan{id, entry.buffer, entry.mappedBytes.data(), entry.committedMappedBytes.data(), entry.mappedBytes.size(),
                entry.liveRangeTracked && !entry.liveRangeFullScanPending, entry.liveRangeFullScanPending, {}, {}};
            if (entry.tracker) {
                // [skperf] round 30: only note the tracker here; its bitmap is read after bufferMutex is released
                // (the entry and its bitmap stay alive: this scan holds a reference). The live-range map (thousands
                // of skinning blocks) is no longer copied for tracked buffers.
                scan.dirtyWords = entry.dirtyWords.get(); scan.dirtyWordCount = entry.tracker->wordCount;
                scan.trackedPending = entry.liveRangeFullScanPending;
                if (!scan.trackedPending) { scan.paged = true; scan.live = true; }
            } else if (scan.live) scan.liveRanges.assign(entry.liveRanges.begin(), entry.liveRanges.end());
            scans.push_back(std::move(scan));
        }
    }
    // [skperf] mapTrack: take the dirty pages outside bufferMutex (cleared before the bytes are read below); clean
    // 64-page words cost one atomic load each.
    for (auto& scan : scans) {
        if (!scan.dirtyWords) continue;
        for (std::size_t w = 0; w < scan.dirtyWordCount; ++w) {
            if (!scan.dirtyWords[w].load(std::memory_order_relaxed)) continue;
            const std::uint64_t bits = scan.dirtyWords[w].exchange(0, std::memory_order_acq_rel);
            if (bits) scan.takenPages.push_back({w, bits});
        }
        if (scan.paged)
            for (const auto& taken : scan.takenPages)
                for (std::uint64_t bits = taken.second; bits;) {
                    const unsigned b = unsigned(__builtin_ctzll(bits)); bits &= bits - 1;
                    const std::size_t begin = ((taken.first << 6) + b) << 12, end = std::min(begin + 4096, scan.bytes);
                    if (begin >= end) continue;
                    if (!scan.liveRanges.empty() && scan.liveRanges.back().second == begin) scan.liveRanges.back().second = end;
                    else scan.liveRanges.push_back({begin, end});
                }
    }
    if (cpParts) cpCollected = emscripten_get_now();
    for (auto& scan : scans) {
        const double scanStarted=emscripten_get_now();
        std::vector<MappedGpuDirtyRange> ranges;
        if (scan.paged && MapNoDiffOn()) {
            // [skperf] round 27 mapNoDiff: the written pages themselves are the patches (no compare)
            for (const auto& live : scan.liveRanges) {
                const std::size_t begin=std::min(live.first,scan.bytes), end=std::min(live.second,scan.bytes);
                if (end > begin) ranges.push_back({begin, end - begin});
            }
        } else if (scan.live) {
            // Only owner-reported live ranges can be read by the GPU; diff exactly those.
            for (const auto& live : scan.liveRanges) {
                const std::size_t begin=std::min(live.first,scan.bytes);
                const std::size_t end=std::min(live.second,scan.bytes);
                if(end<=begin)continue;
                for(auto r:FindMappedGpuDirtyRanges(scan.current+begin,scan.committed+begin,end-begin)){
                    r.offset+=begin;
                    if(!ranges.empty() && ranges.back().offset+ranges.back().size==r.offset)ranges.back().size+=r.size;
                    else ranges.push_back(r);
                }
                scanBytes+=end-begin;
            }
        } else {
            ranges=FindMappedGpuDirtyRanges(scan.current,scan.committed,scan.bytes);
            scanBytes+=scan.bytes;
            // Cleared only once this full scan's patches are committed (see below); a failed flush retries it.
            if(scan.fullScanPending)fullScanCommitted.push_back(scan.id);
        }
        // [skperf] round 28 mapMerge (default ON, ?mapMerge=0 old): ranges separated by a gap of <= 16 KB become one
        // patch (the gap is uploaded with its current bytes: unchanged since the last commit for diffed buffers, the
        // final value of a page whose writer will mark it again for tracked ones). The bar sent ~960 patches/frame,
        // each one allocation here and one queue.writeBuffer on the main thread.
        if (MapMergeOn() && ranges.size() > 1) {
            std::size_t out = 0;
            for (std::size_t i = 1; i < ranges.size(); ++i) {
                auto& cur = ranges[out]; const auto& next = ranges[i];
                if (next.offset >= cur.offset + cur.size && next.offset - (cur.offset + cur.size) <= 16384) cur.size = next.offset + next.size - cur.offset;
                else ranges[++out] = next;
            }
            ranges.resize(out + 1);
        }
        scanMs+=emscripten_get_now()-scanStarted;
        for(const auto& range:ranges)scan.patches.push_back({range.offset,
            std::vector<unsigned char>(scan.current+range.offset,scan.current+range.offset+range.size)});
        if (cpParts) { // [skperf] round 28 per-buffer breakdown
            std::size_t b = 0; for (const auto& range : ranges) b += range.size;
            MapStatNote(scan.id, scan.bytes, scan.paged, scan.live, b, ranges.size(), emscripten_get_now() - scanStarted);
        }
    }
    if (cpParts) cpDiffed = emscripten_get_now();
    bool success = true;
    if (deferred) {
        // perf12: commit the shadow now and hand the bytes to the deferred submission job (no main-thread handoff).
        for (auto& scan : scans) for (auto& patch : scan.patches) {
            if (patch.offset + patch.bytes.size() > scan.bytes) { success = false; continue; }
            if (!(scan.paged && MapNoDiffOn())) std::memcpy(scan.committed + patch.offset, patch.bytes.data(), patch.bytes.size()); // shadow unused for tracked pages
            patchBytes += patch.bytes.size(); ++patchCount;
            deferred->push_back({scan.buffer, std::uint64_t(patch.offset), std::move(patch.bytes)});
        }
        for (auto& scan : scans) scan.patches.clear();
    }
    // All dirty ranges go to the GPU queue in one main-thread handoff instead of one handoff per range.
    std::vector<GpuBufferWrite> writes;
    for (const auto& scan : scans) for (const auto& patch : scan.patches)
        writes.push_back({scan.buffer, patch.offset, patch.bytes.data(), patch.bytes.size()});
    std::unique_ptr<bool[]> issuedWrites(new bool[writes.size() ? writes.size() : 1]());
    if (!writes.empty()) {
        const double writeStarted=emscripten_get_now();
        if (!WriteGpuBufferRanges(writes.data(), writes.size(), issuedWrites.get())) success = false;
        writeMs+=emscripten_get_now()-writeStarted;
    }
    std::size_t writeIndex = 0;
    for (const auto& scan : scans) {
        for(const auto& patch:scan.patches){
            const bool issued=issuedWrites[writeIndex++];
            if(!issued){success=false;continue;}
            patchBytes+=patch.bytes.size();++patchCount;
            // A failed upload keeps the old shadow so the next flush retries. The held reference keeps the entry
            // (and its never-reallocated shadow) alive; only this function, under flushMutex, writes it.
            if(patch.offset+patch.bytes.size()>scan.bytes){success=false;continue;}
            std::memcpy(scan.committed+patch.offset,patch.bytes.data(),patch.bytes.size());
        }
    }
    // [skperf] mapTrack: count paged vs full diff bytes; a failed flush re-marks the pages it took
    for (const auto& scan : scans) {
        if (scan.dirtyWords && !success) for (const auto& taken : scan.takenPages) scan.dirtyWords[taken.first].fetch_or(taken.second, std::memory_order_release);
        if (scan.paged) for (const auto& r : scan.liveRanges) mapTrackPagedBytes.fetch_add(r.second - r.first, std::memory_order_relaxed);
        else if (!scan.live) mapTrackFullBytes.fetch_add(scan.bytes, std::memory_order_relaxed);
    }
    if (success && !fullScanCommitted.empty()) {
        std::lock_guard<std::mutex> lock(bufferMutex);
        for (const auto id : fullScanCommitted) {
            auto found = buffers.find(id);
            if (found != buffers.end()) found->second->liveRangeFullScanPending = false;
        }
    }
    if (cpParts) cpCommitted = emscripten_get_now();
    for (const auto& scan : scans) GpuApi::Release(GpuApi::BufferRef{scan.id});
    if (cpParts) cp2077_skperf_crit_core_parts(cpLocked - flushStarted, cpCollected - cpLocked, cpDiffed - cpCollected, cpCommitted - cpDiffed, emscripten_get_now() - cpCommitted, double(patchBytes), double(patchCount));
    static std::atomic<unsigned> flushCount{0};const unsigned n=++flushCount;
    if(n<=4 || (n<=65536 && !(n&(n-1))))
        std::fprintf(stderr,"CP2077 mapped upload probe: count=%u scanBytes=%llu patchBytes=%llu patches=%u scanMs=%.3f writeMs=%.3f totalMs=%.3f success=%u buffers=%zu selective=%u coveredSubmits=%u | mapTrack totals: pagedBytes=%llu fullBytes=%llu\n",n,(unsigned long long)scanBytes,(unsigned long long)patchBytes,patchCount,scanMs,writeMs,emscripten_get_now()-flushStarted,unsigned(success),scans.size(),unsigned(ids!=nullptr),mappedFlushCoveredCalls.load(),mapTrackPagedBytes.load(),mapTrackFullBytes.load());
    return success;
}
bool FlushOriginalMappedGpuBuffers() {
    if (mappedFlushCovered) {
        // The submitter already flushed every mapped buffer its batch references (FlushOriginalMappedGpuBufferIds).
        mappedFlushCovered = false; ++mappedFlushCoveredCalls; return true;
    }
    return FlushMappedGpuBuffersCore(nullptr, 0);
}
bool FlushOriginalMappedGpuBufferIds(const unsigned* ids, std::size_t count) {
    if (!count) return true;
    return FlushMappedGpuBuffersCore(ids, count);
}
bool FlushOriginalMappedGpuBufferIdsDeferred(const unsigned* ids, std::size_t count, std::vector<DeferredMappedGpuWrite>& writes) {
    if (!count) return true;
    return FlushMappedGpuBuffersCore(ids, count, &writes);
}
void CopyMappedUploadBufferIds(std::vector<unsigned>& ids) {
    static const int cacheOn = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mapIdsCache') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: mapIdsCache=%d\n", v); return v; }();
    static thread_local unsigned cachedVersion = 0;
    static thread_local std::vector<unsigned> cached;
    if (cacheOn && cachedVersion && mappedUploadIdsVersion.load(std::memory_order_acquire) == cachedVersion) { ids = cached; return; }
    std::lock_guard<std::mutex> lock(bufferMutex);
    ids.assign(mappedUploadIds.begin(), mappedUploadIds.end());
    // A stale id (buffer released after the copy) is skipped by the flush (registry lookup); a buffer created after
    // the copy bumped the version first, and any list referencing it was recorded after that (happens-before).
    if (cacheOn) { cached = ids; cachedVersion = mappedUploadIdsVersion.load(std::memory_order_relaxed); }
}
void SetMappedGpuFlushCovered(bool covered) { mappedFlushCovered = covered; }
bool AcquireGpuBufferMulti(const GpuApi::BufferRef& reference, int count, GpuApi::BufferDesc& descriptor, WGPUBuffer& native) {
    native = nullptr;
    if (count <= 0) return false;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    if (entry == buffers.end() || entry->second->references > INT32_MAX - count) return false;
    entry->second->references += count;
    descriptor = entry->second->descriptor; native = entry->second->buffer;
    return true;
}
bool CpCbKeepOn() {
    static const int v = [] {
        const int r = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('cbKeep') === '0' ? 0 : 1; } catch (e) { return 1; } }); // [skperf] round 62: default ON (tc83 57.5-59.9); ?cbKeep=0 = old
        std::fprintf(stderr, "CP2077 skperf switches: cbKeep=%d\n", r); return r; }();
    return v != 0;
}
bool PeekGpuBuffer(const GpuApi::BufferRef& reference, GpuApi::BufferDesc& descriptor, WGPUBuffer& native) {
    native = nullptr;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    if (entry == buffers.end() || entry->second->references == INT32_MAX) return false;
    descriptor = entry->second->descriptor; native = entry->second->buffer;
    return true;
}
void ReleaseGpuBuffersBatch(const GpuApi::BufferRef* refs, std::size_t count) {
    GpuApi::BufferRef slow[40]; std::size_t slowCount = 0;
    {
        std::lock_guard<std::mutex> lock(bufferMutex);
        for (std::size_t i = 0; i < count; ++i) {
            if (refs[i].isNull()) continue;
            auto entry = buffers.find(refs[i].ID());
            if (entry != buffers.end() && entry->second->references > 1) { --entry->second->references; continue; }
            if (slowCount < 40) slow[slowCount++] = refs[i];
        }
    }
    for (std::size_t i = 0; i < slowCount; ++i) GpuApi::Release(slow[i]);
    // count > 40 with that many final releases cannot happen (callers pass <= VERTEX_LAYOUT_MAX_SLOTS * 2 + 2)
}
bool AcquireGpuBuffer(const GpuApi::BufferRef& reference, GpuApi::BufferDesc& descriptor, WGPUBuffer& native) {
    native = nullptr;
    std::lock_guard<std::mutex> lock(bufferMutex);
    auto entry = buffers.find(reference.ID());
    if (entry == buffers.end() || entry->second->references == INT32_MAX) return false;
    ++entry->second->references;
    descriptor = entry->second->descriptor; native = entry->second->buffer;
    return true;
}
void ShutdownGpuBuffers() {
    ShutdownBrowserResourcePacks();
    ShutdownBrowserBufferGrabs();
    std::unordered_map<Uint32, std::unique_ptr<BufferEntry>> retired;
    { std::lock_guard<std::mutex> lock(bufferMutex); retired.swap(buffers); mappedUploadIds.clear(); mappedUploadIdsVersion.fetch_add(1, std::memory_order_release); }
    for (const auto& item : retired) {
        if (item.second->backing.isNull()) DestroyGpuBuffer(item.second->buffer);
        DestroyGpuBuffer(item.second->counter);
    }
}
}

namespace cp2077::web {
bool VerifyOriginalGpuReadback(){
    static std::once_flag once;static bool passed=false;
    std::call_once(once,[]{
        const Uint32 expected[4]={0x10203040,0xabcdef01,0x55667788,0x99887766};
        auto source=CreateGpuBuffer(sizeof(expected),WGPUBufferUsage_CopySrc,expected,sizeof(expected));
        GpuApi::BufferDesc desc;desc.size=sizeof(expected);desc.category=GpuWrapApi::BCC_Staging;desc.usage=GpuApi::BUT_Readback;desc.group=GpuWrapApi::BufferGroup::Misc;
        auto reference=GpuApi::CreateBuffer(desc);GpuApi::BufferDesc actual;WGPUBuffer destination=nullptr;
        bool acquired=AcquireGpuBuffer(reference,actual,destination);
        auto encoder=CreateGpuCommandEncoder();
        bool ok=source && !reference.isNull() && acquired && encoder;
        if(ok)ok=EncodeGpuBufferCopy(encoder,source,0,destination,0,sizeof(expected));
        if(ok){ok=SubmitGpuCommandEncoders(&encoder,1);encoder=nullptr;}
        if(ok)ok=WaitGpuQueueIdle();
        if(ok){
            auto* first=static_cast<const Uint32*>(GpuApi::LockBuffer(reference,GpuApi::BLF_Read,4,8));
            ok=first && first[0]==expected[1] && first[1]==expected[2];
            if(first)GpuApi::UnlockBuffer(reference);
        }
        if(ok){
            auto* second=GpuApi::LockBuffer(reference,GpuApi::BLF_Read,0,sizeof(expected));
            ok=second && !std::memcmp(second,expected,sizeof(expected));
            if(second)GpuApi::UnlockBuffer(reference);
        }
        if(encoder)ReleaseGpuCommandEncoder(encoder);
        if(acquired)GpuApi::Release(reference);
        if(!reference.isNull()){const auto remaining=GpuApi::Release(reference);ok=ok && remaining==0;}
        if(source)DestroyGpuBuffer(source);
        passed=ok;
        std::fprintf(stderr,"CP2077 original GPU readback checks: passed=%u comparedWords=6 pairedLocks=2\n",unsigned(ok));
    });
    return passed;
}
}
