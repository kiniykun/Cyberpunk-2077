#include "skperf-api-timer-web.h" // [skperf] round 39
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-api-shader-web.h"
#include "gpu-api-command-web.h"
#include "gpu-shader-web.h"
#include "gpu-api-vertex-web.h"
#include "gpu-api-texture-web.h"
#include "gpu-api-om-state-web.h"
#include "gpu-device-web.h"
#include "common/renderData/include/pipelineStateObject.h"
#include <vector>
#include "common/shaderCache/include/shaderCacheData.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

namespace {
struct Entry {
    const cp2077::web::ConvertedShader* source;
    WGPUComputePipeline pipeline;
    GpuApi::ShaderOptions options;
    Int32 references = 1;
    const cp2077::web::ConvertedGraphicsShader* graphicsSource = nullptr;
    WGPUShaderModule graphicsModule = nullptr;
    WGPUShaderModule depthGatherModule = nullptr;
    WGPUShaderModule giRGBOutputModule = nullptr;
    WGPUShaderModule logicXorModule = nullptr;
    std::string debugName; // diagnostics only (compute entries)
    // Converted compute shader whose pipeline could not be created at CreateShader time (main thread, device not
    // ready, or creation failed). The ref stays valid and SetShader retries creation on a worker, at most once per
    // device generation (lazyAttempted: a creation was tried with a ready device in lazyAttemptGeneration).
    bool lazyCompute = false;
    bool lazyAttempting = false;
    bool lazyAttempted = false;
    std::uint64_t lazyAttemptGeneration = 0;
    // [boot] converted compute pipeline still being created asynchronously (CreateShader no longer waits for it).
    bool pendingCompute = false;
    // [lazygfx] graphics modules are created on first use; a failed creation is not retried in this device generation.
    std::uint64_t gfxFailedGeneration = ~std::uint64_t(0);
};
std::mutex shaderMutex;
// [boot] Signalled when a pending compute entry finished.
std::condition_variable computeReadyCv;
std::atomic<unsigned> asyncComputeStarted{0}, asyncComputeReady{0}, asyncComputeFailed{0}, asyncComputeWaits{0};
std::atomic<unsigned> asyncComputeWaitMsMax{0}, asyncComputeWaitMsTotal{0};
std::unordered_map<Uint32, std::unique_ptr<Entry>> shaders;
Uint32 nextShaderId = 1;
std::mutex precacheMutex;
std::vector<WGPURenderPipeline> precachedPipelines;
// Keep a failed conversion distinct from the native intentional null shader.
// Binding this record fails AcquireGpuGraphicsShader and the command; it can
// never become a vertex-only pass by being mistaken for ShaderRef::Null().
GpuApi::ShaderRef RegisterUnavailableGraphicsShader(const GpuApi::ShaderOptions& options) {
    std::lock_guard<std::mutex> lock(shaderMutex);
    if (nextShaderId > Uint32(INT32_MAX)) return GpuApi::ShaderRef::Null();
    const Uint32 id = nextShaderId++;
    shaders.emplace(id, std::make_unique<Entry>(Entry{nullptr, nullptr, options, 1}));
    return GpuApi::ShaderRef{id};
}
// Logs each distinct unavailable compute ShaderRef once, with its debug name and why it has no pipeline.
// Takes shaderMutex only briefly and never calls into the command file while holding it.
void ReportUnavailableComputeShader(const GpuApi::ShaderRef& reference) {
    static std::mutex reportMutex; static std::set<Uint32> reported;
    {
        std::lock_guard<std::mutex> guard(reportMutex);
        if (reported.size() >= 512 || !reported.insert(reference.ID()).second) return;
    }
    std::string name; const char* reason = "stale reference";
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(reference.ID());
        if (entry != shaders.end()) {
            name = entry->second->debugName;
            reason = entry->second->graphicsModule || entry->second->graphicsSource ? "graphics shader bound as compute"
                   : entry->second->lazyCompute && !entry->second->pipeline ? "converted compute pipeline not created"
                   : !entry->second->pipeline ? "unconverted graphics placeholder bound as compute" : "reference count overflow";
        }
    }
    std::fprintf(stderr, "CP2077 WebGPU: unavailable compute shader bound: ref=%u reason=%s name=%s\n",
        reference.ID(), reason, name.empty() ? "-" : name.c_str());
}
// Creates the pipeline of a lazy converted compute entry (see Entry::lazyCompute). Worker threads only; never holds
// shaderMutex across the synchronous main-thread proxy. Returns true if the entry now has a pipeline.
bool TryCreateLazyComputePipeline(const GpuApi::ShaderRef& reference) {
    if (reference.isNull() || emscripten_is_main_runtime_thread() || !cp2077::web::IsGpuDeviceReady()) return false;
    const std::uint64_t generation = cp2077::web::GpuDeviceGeneration();
    const cp2077::web::ConvertedShader* source = nullptr;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(reference.ID());
        if (entry == shaders.end() || !entry->second->lazyCompute || entry->second->pipeline || !entry->second->source ||
            entry->second->lazyAttempting ||
            (entry->second->lazyAttempted && entry->second->lazyAttemptGeneration == generation)) return false;
        entry->second->lazyAttempting = true; entry->second->lazyAttempted = true; entry->second->lazyAttemptGeneration = generation;
        source = entry->second->source; name = entry->second->debugName;
    }
    auto pipeline = cp2077::web::CreateGpuComputePipelineFromSource(source);
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(reference.ID());
        if (entry != shaders.end()) {
            entry->second->lazyAttempting = false;
            if (pipeline && !entry->second->pipeline) { entry->second->pipeline = pipeline; stored = true; }
        }
    }
    if (pipeline && !stored) cp2077::web::ReleaseGpuComputePipeline(pipeline);
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 512)
        std::fprintf(stderr, "CP2077 WebGPU: deferred compute pipeline %s: ref=%u name=%s debug=%s generation=%llu\n",
            stored ? "created" : "failed", reference.ID(), source->name, name.empty() ? "-" : name.c_str(),
            static_cast<unsigned long long>(generation));
    return stored;
}
}
namespace cp2077::web {
// Defined in gpu-shader-web.cpp ([boot] asynchronous compute pipeline creation; `ready` runs on the main thread).
bool BeginGpuComputePipelineFromSource(const ConvertedShader* source, void (*ready)(void*, WGPUComputePipeline), void* user);
}
namespace {
void ReportAsyncCompute(const char* why) {
    std::fprintf(stderr, "CP2077 [boot] async compute pipelines (%s): started=%u ready=%u failed=%u waits=%u waitMsTotal=%u waitMsMax=%u\n",
        why, asyncComputeStarted.load(), asyncComputeReady.load(), asyncComputeFailed.load(), asyncComputeWaits.load(),
        asyncComputeWaitMsTotal.load(), asyncComputeWaitMsMax.load());
}
// Main thread: an asynchronous compute creation finished. A failed one becomes a lazy entry (SetShader retries
// synchronously once per device generation, as before); a released entry just drops the pipeline.
void ComputePipelineReady(void* user, WGPUComputePipeline pipeline) {
    const Uint32 id = Uint32(reinterpret_cast<std::uintptr_t>(user));
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(id);
        if (entry != shaders.end() && entry->second->pendingCompute) {
            entry->second->pendingCompute = false;
            if (pipeline) { entry->second->pipeline = pipeline; stored = true; }
            else { entry->second->lazyCompute = true; entry->second->lazyAttempted = false; }
        }
    }
    computeReadyCv.notify_all();
    if (pipeline && !stored) wgpuComputePipelineRelease(pipeline);
    (pipeline ? asyncComputeReady : asyncComputeFailed).fetch_add(1);
    const unsigned total = asyncComputeReady.load() + asyncComputeFailed.load();
    const bool all = total == asyncComputeStarted.load();
    if (all || (total & (total - 1)) == 0) ReportAsyncCompute(all ? "all done" : "progress");
}
}
namespace cp2077::web {
// Defined in gpu-api-command-web.cpp (diagnostics: lets a skipped dispatch name the unavailable shader).
void NoteBoundGpuComputeShaderUnavailable(Uint32 requestedShader);
}
namespace {
// [capture] The unknown-bytecode capture below is a dev tool (offline conversion). Each capture is a synchronous
// MAIN_THREAD_EM_ASM + base64 of up to 512 KiB, and boot hits ~100 unconverted compute shaders (ray tracing, DLSS,
// debug) while the main thread is busy draining init jobs, so every hop waits for it. Only with ?shaderCapture=1.
bool ShaderCaptureEnabled() {
    static const int on = MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('shaderCapture') === '1' ? 1 : 0; } catch (e) { return 0; }
    });
    return on != 0;
}
}
namespace {
// [lazygfx] Boot used to compile every graphics shader module (plus depth-gather / GI / logic-xor variants) inside
// CreateShader: ~240 synchronous hops to the busy main thread while the static shader map was built (~9 s of the
// 2026-10-23 fresh boot). Now CreateShader only records the converted source and the first draw / pipeline precache
// that needs the shader creates its modules (worker threads; shaderMutex is never held across the proxy).
// ?lazyGfx=0 = old eager creation.
bool LazyGraphicsModules() {
    static const int on = MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('lazyGfx') === '0' ? 0 : 1; } catch (e) { return 1; }
    });
    return on != 0;
}
// [lazycs] Boot started all ~245 converted compute pipelines at once; init then needed two of them and waited ~8 s
// for them behind the whole compile queue (2026-10-23 trace). Until the first frame draws, CreateShader only records
// a deferred (lazy) entry: a shader that is actually bound compiles at once on its own (SetShader's lazy path), and
// StartDeferredComputePipelines (first graphics module creation = first rendered frame) starts the rest in the
// background so gameplay finds them ready. ?bootCompute=1 = old start-everything-at-CreateShader.
std::atomic<bool> deferredComputeStarted{false};
std::vector<Uint32> deferredCompute; // guarded by shaderMutex
bool DeferBootCompute() {
    static const int on = MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('bootCompute') === '1' ? 0 : 1; } catch (e) { return 1; }
    });
    return on != 0;
}
void StartDeferredComputePipelines() {
    if (deferredComputeStarted.exchange(true)) return;
    std::vector<std::pair<Uint32, const cp2077::web::ConvertedShader*>> start;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        for (const Uint32 id : deferredCompute) {
            auto entry = shaders.find(id);
            if (entry == shaders.end() || entry->second->pipeline || !entry->second->lazyCompute ||
                entry->second->lazyAttempting || !entry->second->source) continue;
            entry->second->lazyCompute = false; entry->second->pendingCompute = true;
            start.emplace_back(id, entry->second->source);
        }
        deferredCompute.clear(); deferredCompute.shrink_to_fit();
    }
    for (const auto& item : start) {
        asyncComputeStarted.fetch_add(1);
        if (cp2077::web::BeginGpuComputePipelineFromSource(item.second, ComputePipelineReady,
                                                           reinterpret_cast<void*>(std::uintptr_t(item.first)))) continue;
        asyncComputeStarted.fetch_sub(1);
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(item.first);
        if (entry != shaders.end()) { entry->second->pendingCompute = false; entry->second->lazyCompute = true; entry->second->lazyAttempted = false; }
    }
    computeReadyCv.notify_all();
    std::fprintf(stderr, "CP2077 [lazycs] deferred compute pipelines started in the background: %u\n", unsigned(start.size()));
}
bool EnsureGraphicsModules(Uint32 id) {
    if (!deferredComputeStarted.load() && !emscripten_is_main_runtime_thread()) StartDeferredComputePipelines();
    const cp2077::web::ConvertedGraphicsShader* graphics = nullptr;
    const bool worker = !emscripten_is_main_runtime_thread();
    const std::uint64_t generation = cp2077::web::GpuDeviceGeneration();
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(id);
        if (entry == shaders.end()) return false;
        if (entry->second->graphicsModule) return true;
        if (!entry->second->graphicsSource || entry->second->gfxFailedGeneration == generation) return false;
        graphics = entry->second->graphicsSource;
    }
    if (!worker || !cp2077::web::IsGpuDeviceReady()) return false;
    WGPUShaderModule module = cp2077::web::CreateConvertedGraphicsModule(graphics);
    if (!module) {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(id);
        if (entry != shaders.end()) entry->second->gfxFailedGeneration = generation;
        return false;
    }
    WGPUShaderModule depthModule = nullptr, giModule = nullptr, xorModule = nullptr;
    if (const auto* variant = cp2077::web::FindConvertedGraphicsDepthGatherVariant(graphics))
        depthModule = cp2077::web::CreateConvertedGraphicsModule(variant);
    if (const auto* variant = cp2077::web::FindConvertedGraphicsGiRGBOutputVariant(graphics))
        giModule = cp2077::web::CreateConvertedGraphicsModule(variant);
    if (const auto* variant = cp2077::web::FindConvertedGraphicsLogicXorVariant(graphics))
        xorModule = cp2077::web::CreateConvertedGraphicsModule(variant);
    bool stored = false, present = false;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(id);
        if (entry != shaders.end()) {
            if (!entry->second->graphicsModule) {
                entry->second->graphicsModule = module; entry->second->depthGatherModule = depthModule;
                entry->second->giRGBOutputModule = giModule; entry->second->logicXorModule = xorModule;
                stored = true;
            }
            present = entry->second->graphicsModule != nullptr;
        }
    }
    if (!stored) {
        cp2077::web::ReleaseGpuGraphicsModule(module);
        if (depthModule) cp2077::web::ReleaseGpuGraphicsModule(depthModule);
        if (giModule) cp2077::web::ReleaseGpuGraphicsModule(giModule);
        if (xorModule) cp2077::web::ReleaseGpuGraphicsModule(xorModule);
    }
    static std::atomic<unsigned> created{0};
    const unsigned n = stored ? created.fetch_add(1) + 1 : 0;
    if (n && (n & (n - 1)) == 0) std::fprintf(stderr, "CP2077 [lazygfx] graphics shader modules created on first use: %u\n", n);
    return present;
}
}
namespace GpuApi {
ShaderRef CreateShader(GpuWrapApi::eShaderType type, const red::BlobView& data,
                       const ShaderOptions& options, const red::StringView& debugName) {
    if (type == GpuWrapApi::VertexShader || type == GpuWrapApi::PixelShader) {
        const auto* graphics = cp2077::web::FindConvertedGraphicsShaderBytecode(data.Data(), data.Size());
        const auto stage = type == GpuWrapApi::VertexShader ? cp2077::web::GraphicsShaderStage::Vertex
                                                         : cp2077::web::GraphicsShaderStage::Pixel;
        if (!graphics || graphics->stage != stage) {
            // Capture exact missing bytecode locally for conversion. No name
            // or byte-size heuristic is used to substitute another shader.
            if(data.Data() && data.Size() && data.Size()<=512*1024 && ShaderCaptureEnabled()){
                MAIN_THREAD_EM_ASM({
                    const size=Number($2);const ptr=Number($1);const stage=Number($0);
                    const records=globalThis.cp2077UnknownGraphicsBytecode ||= [];
                    if(records.length>=64)return;
                    const bytes=HEAPU8.subarray(ptr,ptr+size);let binary='';
                    for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
                    const base64=btoa(binary);
                    if(records.some(r=>r.stage===stage && r.base64===base64))return;
                    records.push({stage,size,name:UTF8ToString(Number($3),Math.min(Number($4),256)),base64});
                },unsigned(type),data.Data(),data.Size(),debugName.Data(),debugName.Size());
            }
            std::fprintf(stderr, "CP2077 WebGPU: graphics shader has no exact original-bytecode conversion: stage=%u bytes=%llu name=%.*s\n", unsigned(type), static_cast<unsigned long long>(data.Size()), int(debugName.Size()), debugName.Data() ? debugName.Data() : "");
            return RegisterUnavailableGraphicsShader(options);
        }
        const bool shadowName=debugName.Data() &&
            ((debugName.Size()>=sizeof("m_shaderGlobalShadow")-1 && std::strncmp(debugName.Data(),"m_shaderGlobalShadow",sizeof("m_shaderGlobalShadow")-1)==0) ||
             (debugName.Size()>=sizeof("m_shaderRayTracedGlobalShadows")-1 && std::strncmp(debugName.Data(),"m_shaderRayTracedGlobalShadows",sizeof("m_shaderRayTracedGlobalShadows")-1)==0));
        if ((std::strcmp(graphics->originalHash,"2331754078733808635")==0 && shadowName) ||
            std::strcmp(graphics->originalHash,"17899529396732822749")==0)
            std::fprintf(stderr,"CP2077 shader identity probe: hash=%s stage=%u name=%.*s\n",
                graphics->originalHash,unsigned(type),int(debugName.Size()),debugName.Data()?debugName.Data():"");
        WGPUShaderModule module=nullptr, depthModule=nullptr, giModule=nullptr, xorModule=nullptr;
        if (!LazyGraphicsModules()) {   // [lazygfx] otherwise created by EnsureGraphicsModules on first use
        module = cp2077::web::CreateConvertedGraphicsModule(graphics);
        if (!module) return RegisterUnavailableGraphicsShader(options);
        if (const auto* variant=cp2077::web::FindConvertedGraphicsDepthGatherVariant(graphics))
            depthModule=cp2077::web::CreateConvertedGraphicsModule(variant);
        if (const auto* variant=cp2077::web::FindConvertedGraphicsGiRGBOutputVariant(graphics))
            giModule=cp2077::web::CreateConvertedGraphicsModule(variant);
        if(const auto* variant=cp2077::web::FindConvertedGraphicsLogicXorVariant(graphics))xorModule=cp2077::web::CreateConvertedGraphicsModule(variant);
        }
        Uint32 id = 0;
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            if (nextShaderId <= Uint32(INT32_MAX)) {
                id = nextShaderId++;
                shaders.emplace(id, std::make_unique<Entry>(Entry{nullptr, nullptr, options, 1, graphics, module, depthModule,giModule,xorModule}));
            }
        }
        if (!id) {
            if (module) cp2077::web::ReleaseGpuGraphicsModule(module);
            if (depthModule) cp2077::web::ReleaseGpuGraphicsModule(depthModule);
            if (giModule) cp2077::web::ReleaseGpuGraphicsModule(giModule);
            if (xorModule) cp2077::web::ReleaseGpuGraphicsModule(xorModule);
        }
        return ShaderRef{id};
    }
    // Only explicit converted cache records can resolve to these pipelines.
    // Debug names and legacy DXIL bytecode never select a substitute shader.
    const auto* source = cp2077::web::FindConvertedShaderBytecode(data.Data(), data.Size());
    if (type != GpuWrapApi::ComputeShader || !source) {
        // Capture exact missing compute bytecode locally for offline conversion (tools/headless).
        if(type == GpuWrapApi::ComputeShader && data.Data() && data.Size() && data.Size()<=512*1024 && ShaderCaptureEnabled()){
            MAIN_THREAD_EM_ASM({
                const size=Number($1);const ptr=Number($0);
                const records=globalThis.cp2077UnknownComputeBytecode ||= [];
                if(records.length>=512)return;
                const bytes=HEAPU8.subarray(ptr,ptr+size);let binary='';
                for(let i=0;i<bytes.length;i+=8192)binary+=String.fromCharCode(...bytes.subarray(i,i+8192));
                const base64=btoa(binary);
                if(records.some(r=>r.base64===base64))return;
                records.push({size,name:UTF8ToString(Number($2),Math.min(Number($3),256)),base64});
            },data.Data(),data.Size(),debugName.Data(),debugName.Size());
        }
        std::fprintf(stderr, "CP2077 WebGPU: shader requires an exact converted bytecode record: stage=%u bytes=%llu name=%.*s\n", unsigned(type), static_cast<unsigned long long>(data.Size()), int(debugName.Size()), debugName.Data() ? debugName.Data() : "");
        return ShaderRef::Null();
    }
    // [boot] Start the pipeline asynchronously and return at once: base initialization creates ~333 compute shaders,
    // and waiting for each GPU-process compile in turn (sync proxy) kept the engine's first tick busy for seconds.
    // The pipeline is stored when ready; the first SetShader of a still-pending shader waits for it (workers only).
    // ?asyncCompute=0 = old synchronous creation.
    static const int asyncComputeOn = MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('asyncCompute') === '0' ? 0 : 1; } catch (e) { return 1; }
    });
    if (!deferredComputeStarted.load() && DeferBootCompute()) {   // [lazycs]
        Uint32 id = 0;
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            if (nextShaderId <= Uint32(INT32_MAX)) {
                id = nextShaderId++;
                auto& entry = shaders.emplace(id, std::make_unique<Entry>(Entry{source, nullptr, options, 1})).first->second;
                if (debugName.Data()) entry->debugName.assign(debugName.Data(), debugName.Size());
                entry->lazyCompute = true; entry->lazyAttempted = false;
                deferredCompute.push_back(id);
            }
        }
        if (!id) return ShaderRef::Null();
        return ShaderRef{id};
    }
    if (asyncComputeOn && !emscripten_is_main_runtime_thread() && cp2077::web::IsGpuDeviceReady()) {
        Uint32 id = 0;
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            if (nextShaderId <= Uint32(INT32_MAX)) {
                id = nextShaderId++;
                auto& entry = shaders.emplace(id, std::make_unique<Entry>(Entry{source, nullptr, options, 1})).first->second;
                if (debugName.Data()) entry->debugName.assign(debugName.Data(), debugName.Size());
                entry->pendingCompute = true;
            }
        }
        if (!id) return ShaderRef::Null();
        asyncComputeStarted.fetch_add(1);
        if (cp2077::web::BeginGpuComputePipelineFromSource(source, ComputePipelineReady,
                                                           reinterpret_cast<void*>(std::uintptr_t(id))))
            return ShaderRef{id};
        asyncComputeStarted.fetch_sub(1);
        // Not started: keep the ref as a deferred (lazy) entry; SetShader creates it synchronously.
        {
            std::lock_guard<std::mutex> lock(shaderMutex);
            auto entry = shaders.find(id);
            if (entry != shaders.end()) {
                entry->second->pendingCompute = false; entry->second->lazyCompute = true; entry->second->lazyAttempted = false;
            }
        }
        return ShaderRef{id};
    }
    // Create from the exact matched record (a by-name lookup could resolve a different record with the same name).
    const bool attempted = !emscripten_is_main_runtime_thread() && cp2077::web::IsGpuDeviceReady();
    const std::uint64_t generation = attempted ? cp2077::web::GpuDeviceGeneration() : 0;
    auto pipeline = cp2077::web::CreateGpuComputePipelineFromSource(source);
    if (!pipeline) {
        // Main thread / device not ready / proxy failure return null silently. Keep a valid ref without a pipeline
        // (not ShaderRef::Null(), which the engine would keep forever): SetShader retries creation on a worker.
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 256)
            std::fprintf(stderr, "CP2077 WebGPU: compute pipeline unavailable at CreateShader, deferred: name=%s mainThread=%d deviceReady=%d\n",
                source->name, emscripten_is_main_runtime_thread(), int(cp2077::web::IsGpuDeviceReady()));
    }
    Uint32 id = 0;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        if (nextShaderId <= Uint32(INT32_MAX)) {
            id = nextShaderId++;
            auto& entry = shaders.emplace(id, std::make_unique<Entry>(Entry{source, pipeline, options, 1})).first->second;
            if (debugName.Data()) entry->debugName.assign(debugName.Data(), debugName.Size());
            if (!pipeline) { entry->lazyCompute = true; entry->lazyAttempted = attempted; entry->lazyAttemptGeneration = generation; }
        }
    }
    if (!id) cp2077::web::ReleaseGpuComputePipeline(pipeline);
    return ShaderRef{id};
}
void AddRef(const ShaderRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(shaderMutex);
    auto entry = shaders.find(reference.ID());
    if (entry != shaders.end() && entry->second->references < INT32_MAX) ++entry->second->references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing shader reference\n", stderr);
}
Int32 Release(const ShaderRef& reference) {
    if (reference.isNull()) return 0;
    WGPUComputePipeline pipeline = nullptr;
    WGPUShaderModule module = nullptr, depthModule = nullptr, giModule = nullptr, xorModule=nullptr;
    Int32 count = 0;
    {
        std::lock_guard<std::mutex> lock(shaderMutex);
        auto entry = shaders.find(reference.ID());
        if (entry == shaders.end()) return 0;
        count = --entry->second->references;
        if (!count) {
            pipeline = entry->second->pipeline; module = entry->second->graphicsModule; depthModule = entry->second->depthGatherModule;
            giModule=entry->second->giRGBOutputModule;xorModule=entry->second->logicXorModule;shaders.erase(entry);
        }
    }
    if (pipeline && !cp2077::web::ReleaseGpuComputePipeline(pipeline))
        std::fputs("CP2077 WebGPU: shader pipeline release proxy failed\n", stderr);
    if (module && !cp2077::web::ReleaseGpuGraphicsModule(module))
        std::fputs("CP2077 WebGPU: graphics shader module release proxy failed\n", stderr);
    if (depthModule) cp2077::web::ReleaseGpuGraphicsModule(depthModule);
    if (giModule) cp2077::web::ReleaseGpuGraphicsModule(giModule);
    if(xorModule)cp2077::web::ReleaseGpuGraphicsModule(xorModule);
    return count;
}
void UnlinkBinary(const ShaderRef& reference) {
    // No caller BlobView is retained: compilation consumed the validated
    // package at creation, and the pipeline owns its compiled GPU program.
    std::lock_guard<std::mutex> lock(shaderMutex);
    if (shaders.find(reference.ID()) == shaders.end())
        std::fputs("CP2077 WebGPU: stale shader binary-unlink reference\n", stderr);
}
void SetShader(const ShaderRef& reference, GpuWrapApi::eShaderType type) {
    CpApiTimer cpApiTimer(4); // [skperf] round 39
    if (type == GpuWrapApi::VertexShader || type == GpuWrapApi::PixelShader) {
        const auto stage = type == GpuWrapApi::VertexShader ? cp2077::web::GraphicsShaderStage::Vertex
                                                         : cp2077::web::GraphicsShaderStage::Pixel;
        WGPUShaderModule module = nullptr;
        const cp2077::web::ConvertedGraphicsShader* source = nullptr;
        if (!reference.isNull() && !cp2077::web::AcquireGpuGraphicsShader(reference, stage, module, source)) {
            if (cp2077::web::MarkBoundGpuGraphicsShaderUnavailable(stage)) return;
            cp2077::web::FailBoundGpuCommand();
            std::fputs("CP2077 WebGPU: stale or wrong-stage graphics shader binding\n", stderr); return;
        }
        if (!cp2077::web::SetBoundGpuGraphicsShader(reference, stage, module, source)) {
            if (!reference.isNull()) Release(reference);
            std::fputs("CP2077 WebGPU: graphics shader requires a usable default command list\n", stderr);
        }
        return;
    }
    if (type != GpuWrapApi::ComputeShader) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: graphics shader binding is unfinished\n", stderr); return;
    }
    WGPUComputePipeline pipeline = nullptr;
    if (!reference.isNull() && !cp2077::web::AcquireGpuComputeShader(reference, pipeline) &&
        !(TryCreateLazyComputePipeline(reference) && cp2077::web::AcquireGpuComputeShader(reference, pipeline))) {
        // No pipeline (creation failed or feature unavailable): bind nothing so dispatches skip instead of failing the list.
        ReportUnavailableComputeShader(reference);
        if (cp2077::web::SetBoundGpuComputeShader(GpuApi::ShaderRef::Null(), nullptr))
            cp2077::web::NoteBoundGpuComputeShaderUnavailable(reference.ID());
        return;
    }
    if (!cp2077::web::SetBoundGpuComputeShader(reference, pipeline)) {
        if (!reference.isNull()) Release(reference);
        if (!cp2077::web::BoundGpuCommandAlreadyFailed()) // [gamerender] silent in already-failed lists
            std::fputs("CP2077 WebGPU: no usable bound compute command list\n", stderr);
    }
}
void PrecachePSO(ShaderRef reference, job::Builder*) {
    // Compute creation already waits for the real completed pipeline. There is
    // no deferred compilation job to schedule, including for critical shaders.
    std::lock_guard<std::mutex> lock(shaderMutex);
    auto entry = shaders.find(reference.ID());
    if (entry == shaders.end() || (!entry->second->pipeline && !entry->second->lazyCompute && !entry->second->pendingCompute))
        std::fputs("CP2077 WebGPU: compute precache requires a compiled pipeline\n", stderr);
}
}
namespace cp2077::web {
// Called by the adapted original read-only cache loader after deserialization.
// Native metadata and entry hashes remain authoritative. Only an exact known
// runtime variant receives a converted bytecode record; other binaries remain
// intact and are rejected explicitly by CreateShader until converted.
int ConvertStaticShaderEntry(const char* runtimeName, StaticShaderEntry& entry) {
    const auto* source = FindConvertedShader(runtimeName);
    if (!source || std::strcmp(source->runtimeName, runtimeName) != 0) return 0;
    if (!entry.m_hash || !entry.m_data.Size() || source->bytecodeIdentityBytes > UINT32_MAX) return -1;
    if (const auto* existing = FindConvertedShaderBytecode(entry.m_data.Data(), entry.m_data.Size())) {
        if (existing == source) return 1;
        // Variants compiled from identical DXIL (e.g. computeSkinning_Mesh* == _GarmentMesh*) share
        // one cache entry and identical converted WGSL; the bytecode lookup resolves to the first
        // package record. That is equivalent, not a conflict.
        return existing->bytecodeIdentityBytes == source->bytecodeIdentityBytes &&
            std::memcmp(existing->bytecodeIdentity, source->bytecodeIdentity, source->bytecodeIdentityBytes) == 0 &&
            std::strcmp(existing->wgslSHA256, source->wgslSHA256) == 0 &&
            existing->requiredCaps == source->requiredCaps ? 1 : -1;
    }
    entry.m_data = DataBuffer::Copy(source->bytecodeIdentity,
        static_cast<Uint32>(source->bytecodeIdentityBytes), rend::PoolShaderCacheData_ShaderBinaries());
    return entry.m_data.Size() == source->bytecodeIdentityBytes ? 1 : -1;
}
const ConvertedShader* GetGpuComputeShaderSource(const GpuApi::ShaderRef& reference) {
    std::lock_guard<std::mutex> lock(shaderMutex);
    auto entry = shaders.find(reference.ID());
    return entry == shaders.end() ? nullptr : entry->second->source;
}
bool AcquireGpuComputeShader(const GpuApi::ShaderRef& reference, WGPUComputePipeline& pipeline) {
    pipeline = nullptr;
    // [lazycs] a deferred (lazy) entry gets its pipeline here for every caller, not only SetShader: the texture
    // compressor registers its four BC kernels through this at boot and skipped them when they had no pipeline yet.
    if (!reference.isNull()) {
        bool lazy = false;
        {
            std::lock_guard<std::mutex> guard(shaderMutex);
            auto found = shaders.find(reference.ID());
            lazy = found != shaders.end() && !found->second->pipeline && found->second->lazyCompute && !found->second->pendingCompute;
        }
        if (lazy) TryCreateLazyComputePipeline(reference);
    }
    std::unique_lock<std::mutex> lock(shaderMutex);
    auto entry = shaders.find(reference.ID());
    // [boot] still being created asynchronously: workers wait for it (the main thread delivers the result, so it
    // never waits). Bounded so a lost callback cannot hang a frame forever; it then counts as unavailable.
    if (entry != shaders.end() && entry->second->pendingCompute && !emscripten_is_main_runtime_thread()) {
        const auto t0 = std::chrono::steady_clock::now();
        const Uint32 id = reference.ID();
        computeReadyCv.wait_for(lock, std::chrono::seconds(20), [&] {
            auto found = shaders.find(id); return found == shaders.end() || !found->second->pendingCompute;
        });
        const unsigned ms = unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        asyncComputeWaits.fetch_add(1); asyncComputeWaitMsTotal.fetch_add(ms);
        unsigned prev = asyncComputeWaitMsMax.load();
        while (ms > prev && !asyncComputeWaitMsMax.compare_exchange_weak(prev, ms)) {}
        if ((asyncComputeWaits.load() & (asyncComputeWaits.load() - 1)) == 0) ReportAsyncCompute("waited");
        entry = shaders.find(id);
    }
    if (entry == shaders.end() || !entry->second->pipeline || entry->second->references == INT32_MAX) return false;
    ++entry->second->references; pipeline = entry->second->pipeline; return true;
}
WGPUShaderModule GetGpuGraphicsDepthGatherModule(const GpuApi::ShaderRef& ref) {
    EnsureGraphicsModules(ref.ID());
    std::lock_guard<std::mutex> lock(shaderMutex);
    const auto found=shaders.find(ref.ID());
    return found==shaders.end()?nullptr:found->second->depthGatherModule;
}
WGPUShaderModule GetGpuGraphicsGiRGBOutputModule(const GpuApi::ShaderRef& ref) {
    EnsureGraphicsModules(ref.ID());
    std::lock_guard<std::mutex> lock(shaderMutex);
    const auto found=shaders.find(ref.ID());
    return found==shaders.end()?nullptr:found->second->giRGBOutputModule;
}
WGPUShaderModule GetGpuGraphicsLogicXorModule(const GpuApi::ShaderRef& ref){EnsureGraphicsModules(ref.ID());std::lock_guard<std::mutex> lock(shaderMutex);auto it=shaders.find(ref.ID());return it==shaders.end()?nullptr:it->second->logicXorModule;}
bool AcquireGpuGraphicsShader(const GpuApi::ShaderRef& reference, GraphicsShaderStage stage,
                             WGPUShaderModule& module, const ConvertedGraphicsShader*& source) {
    module = nullptr; source = nullptr;
    if (!reference.isNull()) EnsureGraphicsModules(reference.ID());
    std::lock_guard<std::mutex> lock(shaderMutex);
    auto entry = shaders.find(reference.ID());
    if (entry == shaders.end() || entry->second->references == INT32_MAX ||
        !entry->second->graphicsModule || !entry->second->graphicsSource ||
        entry->second->graphicsSource->stage != stage) return false;
    ++entry->second->references;
    module = entry->second->graphicsModule; source = entry->second->graphicsSource;
    return true;
}
void ShutdownGpuShaders() {
    std::vector<WGPURenderPipeline> precached;
    { std::lock_guard<std::mutex> lock(precacheMutex); precached.swap(precachedPipelines); }
    for (auto pipeline : precached) ReleaseGpuRenderPipeline(pipeline);
    std::unordered_map<Uint32, std::unique_ptr<Entry>> retired;
    { std::lock_guard<std::mutex> lock(shaderMutex); retired.swap(shaders); }
    for (const auto& entry : retired) {
        ReleaseGpuComputePipeline(entry.second->pipeline);
        if (entry.second->graphicsModule) ReleaseGpuGraphicsModule(entry.second->graphicsModule);
        if (entry.second->depthGatherModule) ReleaseGpuGraphicsModule(entry.second->depthGatherModule);
        if (entry.second->giRGBOutputModule) ReleaseGpuGraphicsModule(entry.second->giRGBOutputModule);
        if (entry.second->logicXorModule) ReleaseGpuGraphicsModule(entry.second->logicXorModule);
    }
}
}
#include "gpu-api-pipeline-precache-web.inl"
