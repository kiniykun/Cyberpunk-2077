#include "skperf-api-timer-web.h" // [skperf] round 39
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-api-sampler-web.h"
#include "gpu-device-web.h"
#include <emscripten.h>
#include <atomic>
#include "gpu-sampler-web.h"
#include "gpu-api-command-web.h"
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <vector>
#include <cmath>
#include <algorithm>
#include <map>

// Original gpuApiCommon.cpp defines this setting in global scope. The native
// renderer updates this exact variable; a GpuApi-scoped copy would be unrelated.
extern GPUAPI_API Float GRenderSettingsMipBias;

namespace {
WGPUAddressMode Address(GpuApi::eTextureAddress address) {
    switch (address) {
    case GpuApi::TEXADDR_Wrap: return WGPUAddressMode_Repeat;
    case GpuApi::TEXADDR_Mirror: return WGPUAddressMode_MirrorRepeat;
    case GpuApi::TEXADDR_Clamp: return WGPUAddressMode_ClampToEdge;
    // WebGPU has no border colour or mirror-once modes. A rejected sampler fails the whole bound command list
    // (every later draw in it is dropped), so use the nearest modes: identical for in-range coordinates.
    case GpuApi::TEXADDR_Border: return WGPUAddressMode_ClampToEdge;
    case GpuApi::TEXADDR_MirrorOnce: return WGPUAddressMode_MirrorRepeat;
    default: return WGPUAddressMode_Undefined;
    }
}
struct Entry { GpuApi::SamplerStateDesc descriptor; WGPUSampler sampler; Int32 references = 1; };
std::mutex samplerMutex;
std::mutex presetSamplerMutex;
std::map<Uint32,GpuApi::SamplerStateRef> presetSamplers;
Uint64 presetSamplerGeneration=0;
std::once_flag presetStatsRegistration;
struct PresetStats {std::atomic<Uint64> hits{0},misses{0},entries{0};} presetStats;
void RegisterPresetStats(){
    MAIN_THREAD_EM_ASM({
        const base=Number($0);const stats={};
        for(const [name,offset] of [['hits',0],['misses',8],['entries',16]])Object.defineProperty(stats,name,{enumerable:true,get(){growMemViews();return Number(Atomics.load(HEAPU64,(base+offset)/8));}});
        Module['cp2077SamplerPresetStats']=stats;
    },&presetStats);
}
std::unordered_map<Uint32, std::unique_ptr<Entry>> samplers;
struct BiasSnapshot { Uint32 owner; Float bias; };
std::unordered_map<WGPUSampler, BiasSnapshot> samplerBiasSnapshots;
Float EffectiveBias(const GpuApi::SamplerStateDesc& desc,const GpuApi::RenderSettings& settings) {
    return desc.mipLODBias+(desc.allowSettingsBias ? settings.mipMapBias+GRenderSettingsMipBias : 0.f);
}
Uint32 nextSamplerId = 1;
GpuApi::RenderSettings renderSettings;
bool BuildNativeSampler(const GpuApi::SamplerStateDesc& descriptor, const GpuApi::RenderSettings& settings, WGPUSamplerDescriptor& native) {
    using namespace GpuApi;
    const Float bias = EffectiveBias(descriptor,settings);
    const bool aniso = descriptor.filterMin == TEXFILTERMIN_Aniso || descriptor.filterMin == TEXFILTERMIN_AnisoLow;
    if (!std::isfinite(bias) || (bias != 0 && descriptor.comparisonFunction != COMP_None) || Address(descriptor.addressU) == WGPUAddressMode_Undefined ||
        Address(descriptor.addressV) == WGPUAddressMode_Undefined || Address(descriptor.addressW) == WGPUAddressMode_Undefined ||
        (!aniso && descriptor.filterMin != TEXFILTERMIN_Point && descriptor.filterMin != TEXFILTERMIN_Linear) ||
        (descriptor.filterMag != TEXFILTERMAG_Point && descriptor.filterMag != TEXFILTERMAG_Linear) ||
        (descriptor.filterMip != TEXFILTERMIP_None && descriptor.filterMip != TEXFILTERMIP_Point && descriptor.filterMip != TEXFILTERMIP_Linear)) return false;
    native = WGPU_SAMPLER_DESCRIPTOR_INIT;
    native.addressModeU = Address(descriptor.addressU); native.addressModeV = Address(descriptor.addressV); native.addressModeW = Address(descriptor.addressW);
    native.minFilter = descriptor.filterMin == TEXFILTERMIN_Point ? WGPUFilterMode_Nearest : WGPUFilterMode_Linear;
    native.magFilter = aniso || descriptor.filterMag == TEXFILTERMAG_Linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    native.mipmapFilter = aniso || descriptor.filterMip == TEXFILTERMIP_Linear ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    native.maxAnisotropy = Uint16(MapAnisotropy(descriptor.filterMin, settings.anisotropy));
    native.lodMaxClamp = descriptor.filterMip == TEXFILTERMIP_None ? 0.f : 32.f;
    switch (descriptor.comparisonFunction) {
    case COMP_None: break;
    case COMP_Less: native.compare = WGPUCompareFunction_Less; break;
    default: return false;
    }
    return native.maxAnisotropy >= 1 && native.maxAnisotropy <= 16;
}
std::vector<WGPUSampler> retiredSamplers;
std::map<std::pair<Uint32,WGPUSampler>,WGPUSampler> gatherSamplerViews;
bool RebuildBrowserSamplers(const GpuApi::RenderSettings& settings) {
    std::lock_guard<std::mutex> lock(samplerMutex);
    std::vector<std::pair<Entry*,WGPUSampler>> replacement;
    for (const auto& item : samplers) {
        WGPUSamplerDescriptor native;
        WGPUSampler sampler = nullptr;
        if (BuildNativeSampler(item.second->descriptor, settings, native)) sampler = cp2077::web::CreateGpuSampler(native);
        if (!sampler) {
            for (const auto& previous : replacement) cp2077::web::ReleaseGpuSampler(previous.second);
            return false;
        }
        replacement.push_back({item.second.get(),sampler});
    }
    for (const auto& item : replacement) {
        // Older command lists can still borrow the old handle before submission.
        // Hold its native reference until device shutdown; bind groups retain
        // their own objects and future acquisitions use the new descriptor.
        const auto owner=samplerBiasSnapshots.at(item.first->sampler).owner;
        samplerBiasSnapshots.emplace(item.second,BiasSnapshot{owner,EffectiveBias(item.first->descriptor,settings)});
        retiredSamplers.push_back(item.first->sampler); item.first->sampler = item.second;
    }
    renderSettings = settings;
    return true;
}
}
namespace GpuApi {
#include "gpu-sampler-presets.inc"
void SetSamplerStatePreset(Uint32 slot, const eSamplerStatePreset& preset, GpuWrapApi::eShaderType stage) {
    if (stage != GpuWrapApi::VertexShader && stage != GpuWrapApi::PixelShader && stage != GpuWrapApi::ComputeShader) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    SetSamplerStatePreset(slot, preset, GpuWrapApi::ShaderTypeMask(stage));
}
void SetSamplerStatePreset(Uint32 slot, const eSamplerStatePreset& preset, GpuWrapApi::ShaderTypeMask stages) {
    if (!stages) return;
    const GpuWrapApi::ShaderTypeMask supported(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader);
    const auto type = GetBoundCommandListType();
    if (slot >= MAX_SAMPLER_STATES || (stages.Get() & ~supported.Get()) ||
        (type != CommandListType::Default && type != CommandListType::Compute) ||
        (type == CommandListType::Compute && (stages.Has(GpuWrapApi::VertexShader) || stages.Has(GpuWrapApi::PixelShader)))) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    SamplerStateDesc descriptor;
    if (!BuildBrowserSamplerPreset(preset, descriptor)) { cp2077::web::FailBoundGpuCommand(); return; }
    SamplerStateRef sampler=SamplerStateRef::Null();
    if (!cp2077::web::RequestGpuSamplerPreset(Uint32(preset),sampler)) { cp2077::web::FailBoundGpuCommand(); return; }
    // Each stage binding retains the original sampler independently. The local
    // factory ref retires only after all selected bindings have been attempted.
    for (auto stage : {GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader})
        if (stages.Has(stage)) SetSamplerState(slot, sampler, stage);
    Release(sampler);
}
void SetSamplerState(Uint32 slot, const SamplerStateRef& reference, GpuWrapApi::eShaderType stage) {
    CpApiTimer cpApiTimer(6); // [skperf] round 39
    const Uint32 stageIndex = stage == GpuWrapApi::VertexShader ? 0 :
        stage == GpuWrapApi::PixelShader ? 1 : stage == GpuWrapApi::ComputeShader ? 2 : 3;
    if (stageIndex > 2 || slot >= MAX_SAMPLER_STATES) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: unsupported sampler stage or slot\n", stderr); return;
    }
    WGPUSampler sampler = nullptr;
    if (!reference.isNull() && !cp2077::web::AcquireGpuSampler(reference, sampler)) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: stale sampler binding\n", stderr); return;
    }
    if (!cp2077::web::SetBoundGpuSampler(slot, reference, sampler, stageIndex)) {
        if (!reference.isNull()) Release(reference);
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: no usable bound sampler state\n", stderr);
    }
}
Bool IsDescSupported(const SamplerStateDesc& descriptor) {
    std::lock_guard<std::mutex> lock(samplerMutex);
    WGPUSamplerDescriptor native;
    return BuildNativeSampler(descriptor,renderSettings,native);
}
void InvalidateSamplerStates() {
    if (!cp2077::web::InvalidateBoundGpuSamplers()) cp2077::web::FailBoundGpuCommand();
}
void ResetSamplerStates() {
    RenderSettings settings;
    { std::lock_guard<std::mutex> lock(samplerMutex); settings=renderSettings; }
    if (!RebuildBrowserSamplers(settings)) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: sampler recreation failed\n",stderr); return;
    }
    // Re-acquire on subsequent draws/dispatches; already recorded native bind
    // groups keep their own sampler objects alive.
    if (GetBoundCommandListType() != CommandListType::None) InvalidateSamplerStates();
}
void SetRenderSettings(const RenderSettings& requested) {
    RenderSettings settings=requested;
    settings.anisotropy=std::max(1u,std::min(16u,settings.anisotropy));
    settings.anisotropy |= settings.anisotropy >> 1;
    settings.anisotropy |= settings.anisotropy >> 2;
    settings.anisotropy |= settings.anisotropy >> 4;
    settings.anisotropy -= settings.anisotropy >> 1;
    // Render settings affect only samplers whose descriptor opts into their
    // bias. Preserve the requested setting and validate the actual rebuilt
    // samplers; no-bias samplers remain exact even when the global bias is set.
    // Ordinary sampler bias is supplied to converted sampling operations.
    // Draw/dispatch rejects nonzero bias when its shader has no such lowering.
    // perf11: StartFrameRendering calls this every frame with the same settings. The rebuild proxied one
    // CreateGpuSampler per live sampler to the main thread under samplerMutex (blocking every worker's sampler
    // lookups), leaked the old handles (retiredSamplers/bias snapshots/gather samplers grew every frame) and gave
    // every sampler a new handle, so no cached bind group survived a frame. BuildNativeSampler reads only
    // anisotropy and mipMapBias (+ GRenderSettingsMipBias, whose changes go through ResetSamplerStates), so
    // equal values reproduce the existing samplers exactly: keep them. ?samplerRebuildEveryFrame=1 restores the
    // old per-call rebuild for A/B.
    static const int rebuildEveryFrame = [] {
        const int value = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('samplerRebuildEveryFrame') === '1' ? 1 : 0; });
        std::fprintf(stderr, "CP2077 perf11 switches: samplerRebuildEveryFrame=%d\n", value);
        return value;
    }();
    if (!rebuildEveryFrame && std::isfinite(settings.mipMapBias)) {
        static std::atomic<Uint64> skipped{0};
        bool unchanged = false;
        {
            std::lock_guard<std::mutex> lock(samplerMutex);
            if (renderSettings.anisotropy == settings.anisotropy && renderSettings.mipMapBias == settings.mipMapBias) {
                renderSettings = settings; unchanged = true;
            }
        }
        if (unchanged) {
            const Uint64 n = skipped.fetch_add(1) + 1;
            if (n == 1 || n == 64 || n == 4096) std::fprintf(stderr, "CP2077 perf11 sampler rebuild skipped (settings unchanged): count=%llu\n", (unsigned long long)n);
            return;
        }
    }
    if (!std::isfinite(settings.mipMapBias) || !RebuildBrowserSamplers(settings)) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: render settings require unsupported sampler bias translation or sampler recreation failed\n",stderr); return;
    }
    if (GetBoundCommandListType() != CommandListType::None) InvalidateSamplerStates();
}
SamplerStateRef RequestSamplerState(const SamplerStateDesc& descriptor) {
    WGPUSamplerDescriptor native;
    std::lock_guard<std::mutex> lock(samplerMutex);
    if (!BuildNativeSampler(descriptor,renderSettings,native)) {
        static unsigned reports=0;
        if(reports++<16) std::fprintf(stderr,"CP2077 WebGPU: unsupported sampler descriptor: address=%u/%u/%u filter=%u/%u/%u compare=%u bias=%g anisotropy=%u\n",
            unsigned(descriptor.addressU),unsigned(descriptor.addressV),unsigned(descriptor.addressW),
            unsigned(descriptor.filterMin),unsigned(descriptor.filterMag),unsigned(descriptor.filterMip),
            unsigned(descriptor.comparisonFunction),double(EffectiveBias(descriptor,renderSettings)),unsigned(renderSettings.anisotropy));
        return SamplerStateRef::Null();
    }
    auto sampler = cp2077::web::CreateGpuSampler(native);
    if (!sampler) return SamplerStateRef::Null();
    Uint32 id = 0;
    {
        if (nextSamplerId <= Uint32(INT32_MAX)) {
            id = nextSamplerId++;
            samplers.emplace(id, std::make_unique<Entry>(Entry{descriptor, sampler, 1}));
            samplerBiasSnapshots.emplace(sampler,BiasSnapshot{id,EffectiveBias(descriptor,renderSettings)});
        }
    }
    if (!id) cp2077::web::ReleaseGpuSampler(sampler);
    return SamplerStateRef{id};
}
void AddRef(const SamplerStateRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(samplerMutex);
    auto entry = samplers.find(reference.ID());
    if (entry != samplers.end() && entry->second->references < INT32_MAX) ++entry->second->references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing sampler reference\n", stderr);
}
Int32 Release(const SamplerStateRef& reference) {
    if (reference.isNull()) return 0;
    WGPUSampler sampler = nullptr;
    Int32 count = 0;
    {
        std::lock_guard<std::mutex> lock(samplerMutex);
        auto entry = samplers.find(reference.ID());
        if (entry == samplers.end()) return 0;
        count = --entry->second->references;
        if (!count) { sampler = entry->second->sampler; samplerBiasSnapshots.erase(sampler); samplers.erase(entry); }
    }
    if (sampler && !cp2077::web::ReleaseGpuSampler(sampler)) std::fputs("CP2077 WebGPU: sampler release proxy failed\n", stderr);
    return count;
}
void GetSamplerStateDesc(const SamplerStateRef& reference, SamplerStateDesc& descriptor, GpuWrapApi::eShaderType) {
    std::lock_guard<std::mutex> lock(samplerMutex);
    auto entry = samplers.find(reference.ID());
    if (entry != samplers.end()) descriptor = entry->second->descriptor;
    else { descriptor = SamplerStateDesc{}; std::fputs("CP2077 WebGPU: stale sampler descriptor reference\n", stderr); }
}
}
namespace cp2077::web {
bool RequestGpuSamplerPreset(std::uint32_t preset,GpuApi::SamplerStateRef& result){
    result=GpuApi::SamplerStateRef::Null();GpuApi::SamplerStateDesc descriptor;
    if(preset>=Uint32(GpuApi::SAMPSTATEPRESET_Max)||!IsGpuDeviceReady()||
        !GpuApi::BuildBrowserSamplerPreset(GpuApi::eSamplerStatePreset(preset),descriptor))return false;
    std::lock_guard<std::mutex> presetsLock(presetSamplerMutex);
    std::call_once(presetStatsRegistration,RegisterPresetStats);
    const auto generation=GpuDeviceGeneration();
    if(presetSamplerGeneration!=generation){
        for(const auto& item:presetSamplers)GpuApi::Release(item.second);
        presetSamplers.clear();presetStats.entries.store(0);presetSamplerGeneration=generation;
    }
    auto cached=presetSamplers.find(preset);
    if(cached!=presetSamplers.end()){
        std::lock_guard<std::mutex> lock(samplerMutex);auto entry=samplers.find(cached->second.ID());
        if(entry==samplers.end()||entry->second->references==INT32_MAX)return false;
        ++entry->second->references;result=cached->second;++presetStats.hits;return true;
    }
    ++presetStats.misses;const auto created=GpuApi::RequestSamplerState(descriptor);
    if(created.isNull())return false;
    // The factory reference becomes cache ownership; acquire a separate caller lease.
    presetSamplers.emplace(preset,created);presetStats.entries.store(presetSamplers.size());
    {std::lock_guard<std::mutex> lock(samplerMutex);auto entry=samplers.find(created.ID());
        if(entry==samplers.end()||entry->second->references==INT32_MAX)return false;
        ++entry->second->references;result=created;}
    return true;
}
WGPUSampler GetGpuNonFilteringGatherSampler(const GpuApi::SamplerStateRef& reference,WGPUSampler snapshot) {
    std::lock_guard<std::mutex> lock(samplerMutex);
    const auto owner=samplers.find(reference.ID());
    const auto state=samplerBiasSnapshots.find(snapshot);
    if(owner==samplers.end() || state==samplerBiasSnapshots.end() || state->second.owner!=reference.ID() ||
        owner->second->descriptor.comparisonFunction!=GpuApi::COMP_None) return nullptr;
    const auto key=std::make_pair(reference.ID(),snapshot);
    const auto cached=gatherSamplerViews.find(key);
    if(cached!=gatherSamplerViews.end()) return cached->second;
    WGPUSamplerDescriptor desc;
    if(!BuildNativeSampler(owner->second->descriptor,renderSettings,desc)) return nullptr;
    // Keep addressing and LOD clamps. Gather returns the four individual texels,
    // independent of min/mag/mip interpolation and anisotropic filtering.
    desc.minFilter=WGPUFilterMode_Nearest;desc.magFilter=WGPUFilterMode_Nearest;
    desc.mipmapFilter=WGPUMipmapFilterMode_Nearest;desc.maxAnisotropy=1;
    auto view=CreateGpuSampler(desc);if(!view)return nullptr;
    gatherSamplerViews.emplace(key,view);retiredSamplers.push_back(view);
    return view;
}
bool GetEffectiveGpuSamplerBias(const GpuApi::SamplerStateRef& reference, WGPUSampler native, float& bias) {
    std::lock_guard<std::mutex> lock(samplerMutex);
    const auto entry=samplers.find(reference.ID());
    const auto snapshot=samplerBiasSnapshots.find(native);
    if (entry==samplers.end() || snapshot==samplerBiasSnapshots.end() || snapshot->second.owner!=reference.ID()) return false;
    bias=snapshot->second.bias;
    return std::isfinite(bias);
}
bool AcquireGpuSampler(const GpuApi::SamplerStateRef& reference, WGPUSampler& sampler, WGPUSampler snapshot) {
    sampler = nullptr;
    std::lock_guard<std::mutex> lock(samplerMutex);
    auto entry = samplers.find(reference.ID());
    if (entry == samplers.end() || entry->second->references == INT32_MAX) return false;
    if (snapshot) {
        const auto found=samplerBiasSnapshots.find(snapshot);
        if(found==samplerBiasSnapshots.end() || found->second.owner!=reference.ID()) return false;
    }
    ++entry->second->references; sampler = snapshot ? snapshot : entry->second->sampler; return true;
}
void ShutdownGpuSamplers() {
    std::unordered_map<Uint32, std::unique_ptr<Entry>> retired;
    { std::lock_guard<std::mutex> presetsLock(presetSamplerMutex);std::lock_guard<std::mutex> lock(samplerMutex);retired.swap(samplers);presetSamplers.clear();presetSamplerGeneration=0;presetStats.entries.store(0); }
    std::vector<WGPUSampler> old;
    { std::lock_guard<std::mutex> lock(samplerMutex); old.swap(retiredSamplers); samplerBiasSnapshots.clear(); gatherSamplerViews.clear(); }
    for (const auto& item : retired) ReleaseGpuSampler(item.second->sampler);
    for (auto sampler : old) ReleaseGpuSampler(sampler);
}
}
