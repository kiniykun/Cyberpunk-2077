#include "gpu-compute-web.h"
#include "gpu-device-web.h"
#include "gpu-texture-web.h"
#include "gpu-handoff-stats-web.h"
#include "engine-wgtrace-web.h"
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <memory>
#include <atomic>
#include <mutex>
#include <set>
#include <list>
#include <deque>
#include <unordered_map>

namespace {
em_proxying_queue* ComputeQueue() { static auto* queue = em_proxying_queue_create(); return queue; }
struct CommandValidation {
    WGPUCommandEncoder owner=nullptr;
    unsigned pending=0;
    bool failed=false;
    bool unscoped=false; // perf12: holds a bind group created without error scopes (submit it on its own)
    em_proxying_ctx* waiter=nullptr;
    bool* result=nullptr;
    ~CommandValidation(){if(owner)wgpuCommandEncoderRelease(owner);}
};
std::map<WGPUCommandEncoder,std::shared_ptr<CommandValidation>> commandValidation;
unsigned pendingValidationOperations=0;
struct Request {
    WGPUDevice device = nullptr;
    em_proxying_ctx* context = nullptr;
    WGPUComputePipeline pipeline = nullptr;
    WGPURenderPipeline graphicsPipeline = nullptr;
    WGPUBindGroup created = nullptr;
    std::uint32_t group = 0;
    const WGPUBindGroupEntry* entries = nullptr;
    std::size_t count = 0;
    WGPUCommandEncoder encoder = nullptr;
    const cp2077::web::ComputeBindGroup* groups = nullptr;
    std::uint32_t x = 0, y = 0, z = 0;
    WGPUBuffer indirectArguments = nullptr;
    std::uint64_t indirectOffset = 0;
    unsigned remaining = 3;
    bool failed = false;
    double queuedAt=0,beganAt=0,encodedAt=0,checkedAt=0;
    double firstCallbackAt=0,lastCallbackAt=0,cacheMs=0;
    std::string bindingKey;
    WGPUCommandEncoder validationOwner=nullptr;
    std::shared_ptr<CommandValidation> validation;
    std::vector<WGPUBindGroupEntry> ownedEntries;
    bool deferred=false;
    std::uint64_t deviceGeneration=0;
    cpEngineTrace::Request trace;
};
// perf12: binding/dispatch timing probes only with ?probes=1 (zero otherwise; summary counts still print).
inline double CpProbeNow(){return cpEngineTrace::ProbesEnabled()?emscripten_get_now():0;}
#include "gpu-bind-group-cache-web.inl"
void Checked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
             WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<Request*>(argument);
    if (!request->firstCallbackAt) request->firstCallbackAt=CpProbeNow();
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true;
        std::string diagnostic="CP2077 WebGPU binding/encoding failed: ";
        if (message.data) {
            if (message.length == WGPU_STRLEN) diagnostic.append(message.data);
            else diagnostic.append(message.data,message.length);
        }
        // One stdio call holds the stream lock across the complete diagnostic.
        std::fprintf(stderr,"%s\n",diagnostic.c_str());
    }
    if (--request->remaining) return;
    request->lastCallbackAt=CpProbeNow();
    if (!cp2077::web::IsGpuDeviceReady() || cp2077::web::MainThreadGpuDevice() != request->device || request->deviceGeneration!=cp2077::web::GpuDeviceGeneration()) request->failed = true;
    if (request->failed && request->created) { wgpuBindGroupRelease(request->created); request->created = nullptr; }
    const double cacheStarted=CpProbeNow();
    StoreBinding(*request);
    request->cacheMs=CpProbeNow()-cacheStarted;
    request->checkedAt=CpProbeNow();
    request->trace.Settled(request->failed);
    wgpuDeviceRelease(request->device);
    if(request->deferred){
        auto state=request->validation;
        state->failed|=request->failed;
        // The caller owns its returned bind group independently of this lease.
        if(request->created)wgpuBindGroupRelease(request->created);
        if(request->pipeline)wgpuComputePipelineRelease(request->pipeline);
        if(request->graphicsPipeline)wgpuRenderPipelineRelease(request->graphicsPipeline);
        for(const auto& entry:request->ownedEntries){
            if(entry.buffer)wgpuBufferRelease(entry.buffer);
            if(entry.textureView)wgpuTextureViewRelease(entry.textureView);
            if(entry.sampler)wgpuSamplerRelease(entry.sampler);
        }
        --state->pending;--pendingValidationOperations;
        delete request;
        if(!state->pending && state->waiter){
            auto* context=state->waiter;*state->result=!state->failed;
            state->waiter=nullptr;state->result=nullptr;
            emscripten_proxy_finish(context);
        }
        return;
    }
    if(request->context)emscripten_proxy_finish(request->context);
}
void Begin(em_proxying_ctx* context, void* argument) {
    auto& caller = *static_cast<Request*>(argument);
    caller.beganAt=CpProbeNow();
    caller.trace.Begin();
    caller.context=context;caller.device=cp2077::web::MainThreadGpuDevice();
    if(!caller.device){caller.failed=true;if(context)emscripten_proxy_finish(context);return;}
    caller.deviceGeneration=cp2077::web::GpuDeviceGeneration();
    if(!caller.encoder && ReuseBinding(caller))return;
    Request* active=&caller;
    // Bound the number of in-flight leases. Overflow keeps the checked serial
    // path, rather than dropping validation or retaining unbounded resources.
    if(caller.validationOwner && pendingValidationOperations<1024){
        active=new Request(caller);active->deferred=true;active->context=nullptr;
        auto& state=commandValidation[caller.validationOwner];
        if(!state){state=std::make_shared<CommandValidation>();state->owner=caller.validationOwner;wgpuCommandEncoderAddRef(state->owner);}
        active->validation=state;++state->pending;++pendingValidationOperations;
        if(!caller.encoder && caller.count){active->ownedEntries.assign(caller.entries,caller.entries+caller.count);active->entries=active->ownedEntries.data();}
        if(active->pipeline)wgpuComputePipelineAddRef(active->pipeline);
        if(active->graphicsPipeline)wgpuRenderPipelineAddRef(active->graphicsPipeline);
        for(const auto& entry:active->ownedEntries){
            if(entry.buffer)wgpuBufferAddRef(entry.buffer);
            if(entry.textureView)wgpuTextureViewAddRef(entry.textureView);
            if(entry.sampler)wgpuSamplerAddRef(entry.sampler);
        }
    }
    auto& request=*active;
    wgpuDeviceAddRef(request.device);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(request.device, WGPUErrorFilter_Validation);
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (!cp2077::web::MainThreadGpuLimits(limits)) request.failed = true;
    if (request.encoder) {
        if (request.count > limits.maxBindGroups || request.x > limits.maxComputeWorkgroupsPerDimension ||
            request.y > limits.maxComputeWorkgroupsPerDimension || request.z > limits.maxComputeWorkgroupsPerDimension) request.failed = true;
        for (std::size_t index = 0; !request.failed && index < request.count; ++index) {
            if (!request.groups[index].binding || request.groups[index].group >= limits.maxBindGroups) request.failed = true;
            for (std::size_t previous = 0; previous < index; ++previous)
                if (request.groups[previous].group == request.groups[index].group) request.failed = true;
        }
        if (!request.failed) {
            WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
            auto pass = wgpuCommandEncoderBeginComputePass(request.encoder, &descriptor);
            if (pass) {
                wgpuComputePassEncoderSetPipeline(pass, request.pipeline);
                for (std::size_t index = 0; index < request.count; ++index)
                    wgpuComputePassEncoderSetBindGroup(pass, request.groups[index].group, request.groups[index].binding, 0, nullptr);
                if (request.indirectArguments)
                    wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, request.indirectArguments, request.indirectOffset);
                else wgpuComputePassEncoderDispatchWorkgroups(pass, request.x, request.y, request.z);
                wgpuComputePassEncoderEnd(pass); wgpuComputePassEncoderRelease(pass);
            } else request.failed = true;
        }
    } else {
        if (request.group >= limits.maxBindGroups || request.count > limits.maxBindingsPerBindGroup) request.failed = true;
        if (!request.failed) {
            auto layout = request.graphicsPipeline
                ? wgpuRenderPipelineGetBindGroupLayout(request.graphicsPipeline, request.group)
                : wgpuComputePipelineGetBindGroupLayout(request.pipeline, request.group);
            if (layout) {
                WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
                descriptor.layout = layout; descriptor.entryCount = request.count; descriptor.entries = request.entries;
                request.created = wgpuDeviceCreateBindGroup(request.device, &descriptor);
                wgpuBindGroupLayoutRelease(layout);
            }
            if (!request.created) request.failed = true;
        }
    }
    request.encodedAt=CpProbeNow();
    request.trace.CallEnd();request.trace.Scoped(request.deferred);
    const bool deferred=request.deferred;
    if(deferred){
        caller.created=request.created;caller.failed=request.failed;
        caller.encodedAt=request.encodedAt;
        if(request.created)wgpuBindGroupAddRef(request.created);
    }
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous; callback.callback = Checked; callback.userdata1 = &request;
    for (unsigned index = 0; index < 3; ++index) wgpuDevicePopErrorScope(request.device, callback);
    if(deferred&&context)emscripten_proxy_finish(context);
}
bool Execute(Request& request) {
    if ((!request.pipeline && !request.graphicsPipeline) ||
        (request.graphicsPipeline && (request.pipeline || request.encoder)) ||
        emscripten_is_main_runtime_thread() || !cp2077::web::IsGpuDeviceReady()) return false;
    auto* queue = ComputeQueue();
    std::call_once(cpGpuHandoffStats::registration,cpGpuHandoffStats::Register);
    const double started=CpProbeNow();request.queuedAt=started;
    request.trace.Queue(request.encoder?"ComputeDispatch":"BindingCreation");
    const bool completed=ReuseBindingOnWorker(request) || (queue && emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(), Begin, &request) && !request.failed);
    const double elapsed=CpProbeNow()-started;
    request.trace.WaitEnd();
    const unsigned kind=request.encoder?0:request.graphicsPipeline?1:2;
    if(request.beganAt){
        const double ended=started+elapsed;
        cpGpuHandoffStats::Record(kind==0?"ComputeDispatch":kind==1?"GraphicsBindingCreation":"ComputeBindingCreation",
            elapsed,request.encodedAt-request.beganAt,request.beganAt-started,
            request.checkedAt?ended-request.checkedAt:ended-request.encodedAt,
            request.checkedAt?request.checkedAt-request.encodedAt:0);
    }
    struct Timing {unsigned count=0;double totalMs=0,maxMs=0;};
    static thread_local Timing timings[3];auto& timing=timings[kind];
    ++timing.count;timing.totalMs+=elapsed;if(elapsed>timing.maxMs)timing.maxMs=elapsed;
    if(timing.count<=4||(timing.count<=4096&&!(timing.count&(timing.count-1))))
        std::fprintf(stderr,"CP2077 GPU binding wait probe: kind=%s calls=%u totalMs=%.3f maxMs=%.3f success=%u\n",
            kind==0?"compute-dispatch":kind==1?"graphics-bind-group":"compute-bind-group",timing.count,timing.totalMs,timing.maxMs,unsigned(completed));
    static thread_local unsigned slowCallbacks=0;
    if(elapsed>=20 && slowCallbacks++<24 && request.checkedAt>0)
        std::fprintf(stderr,"CP2077 GPU binding delay probe: kind=%s wallMs=%.3f deliveryMs=%.3f encodingMs=%.3f validationMs=%.3f returnMs=%.3f firstCallbackDelayMs=%.3f callbackSpanMs=%.3f cacheMs=%.3f success=%u\n",
            kind==0?"compute-dispatch":kind==1?"graphics-bind-group":"compute-bind-group",elapsed,
            request.beganAt-started,request.encodedAt-request.beganAt,request.checkedAt-request.encodedAt,
            started+elapsed-request.checkedAt,request.firstCallbackAt-request.encodedAt,
            request.lastCallbackAt-request.firstCallbackAt,request.cacheMs,unsigned(completed));
    return completed;
}
}
namespace cp2077::web {
bool DrainGpuCommandValidation(WGPUCommandEncoder owner){
    if(!owner)return false;
    if(emscripten_is_main_runtime_thread()){
        auto it=commandValidation.find(owner);if(it==commandValidation.end())return true;
        if(it->second->pending)return false;
        const bool ok=!it->second->failed;commandValidation.erase(it);return ok;
    }
    struct Wait {WGPUCommandEncoder owner;bool ok=false;} wait{owner};
    auto* queue=ComputeQueue();
    return queue && emscripten_proxy_sync_with_ctx(queue,emscripten_main_runtime_thread_id(),[](em_proxying_ctx* context,void* argument){
        auto& wait=*static_cast<Wait*>(argument);auto it=commandValidation.find(wait.owner);
        if(it==commandValidation.end()){wait.ok=true;emscripten_proxy_finish(context);return;}
        auto state=it->second;commandValidation.erase(it);
        if(!state->pending){wait.ok=!state->failed;emscripten_proxy_finish(context);return;}
        state->waiter=context;state->result=&wait.ok;
    },&wait) && wait.ok;
}
int PollGpuCommandValidation(WGPUCommandEncoder owner){
    if(!owner||!emscripten_is_main_runtime_thread())return 1;
    auto it=commandValidation.find(owner);if(it==commandValidation.end())return 0;
    // Pending callbacks keep the state alive through their own shared_ptr; no waiter is installed.
    auto state=it->second;commandValidation.erase(it);
    if(state->failed)return 1;
    return (state->pending||state->unscoped)?2:0;
}
namespace {
std::mutex deferredGpuWorkMutex;
std::deque<std::pair<void(*)(void*),void*>> deferredGpuWork;
std::atomic<unsigned> deferredGpuWorkPending{0};
bool deferredGpuWorkRunning=false; // main thread only
}
void DrainDeferredGpuWork(){
    if(!deferredGpuWorkPending.load(std::memory_order_acquire)||deferredGpuWorkRunning||!emscripten_is_main_runtime_thread())return;
    deferredGpuWorkRunning=true;
    for(;;){
        std::pair<void(*)(void*),void*> job;
        {
            std::lock_guard<std::mutex> lock(deferredGpuWorkMutex);
            if(deferredGpuWork.empty())break;
            job=deferredGpuWork.front();deferredGpuWork.pop_front();
        }
        job.first(job.second);
        deferredGpuWorkPending.fetch_sub(1,std::memory_order_release);
    }
    deferredGpuWorkRunning=false;
}
// [deferyield] 2026-10-24 worker profile: render threads mostly wait on main-thread proxies queued behind the whole
// deferred-submit backlog (~13 ms/frame run in one task). ?deferYield=1: each job's own wake-up runs only ONE job,
// so proxies from other threads interleave. Ordering is unchanged: proxies that need it still drain the FIFO first.
static bool CpDeferYield(){
    static const int v=[]{const int r=EM_ASM_INT({try{return new URLSearchParams(location.search).get('deferYield')==='1'?1:0;}catch(e){return 0;}});
        std::fprintf(stderr,"CP2077 deferyield switches: deferYield=%d\n",r);return r;}();
    return v!=0;
}
static void DrainDeferredGpuWorkOne(){
    if(!deferredGpuWorkPending.load(std::memory_order_acquire)||deferredGpuWorkRunning)return;
    std::pair<void(*)(void*),void*> job;
    {
        std::lock_guard<std::mutex> lock(deferredGpuWorkMutex);
        if(deferredGpuWork.empty())return;
        job=deferredGpuWork.front();deferredGpuWork.pop_front();
    }
    deferredGpuWorkRunning=true;job.first(job.second);deferredGpuWorkRunning=false;
    deferredGpuWorkPending.fetch_sub(1,std::memory_order_release);
}
bool EnqueueDeferredGpuWork(void (*work)(void*), void* argument){
    if(!work)return false;
    if(emscripten_is_main_runtime_thread()){DrainDeferredGpuWork();work(argument);return true;}
    {
        std::lock_guard<std::mutex> lock(deferredGpuWorkMutex);
        deferredGpuWork.emplace_back(work,argument);
        deferredGpuWorkPending.fetch_add(1,std::memory_order_release);
    }
    // One wake-up per job; a drain that finds the FIFO already empty returns at once. If the wake-up cannot be
    // posted the job stays queued and runs at the next drain point (any queue-touching main-thread handler).
    auto* queue=ComputeQueue();
    if(!queue||!emscripten_proxy_async(queue,emscripten_main_runtime_thread_id(),[](void*){if(CpDeferYield())DrainDeferredGpuWorkOne();else DrainDeferredGpuWork();},nullptr)){
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1)<4)std::fputs("CP2077 deferred GPU work wake-up proxy failed (runs at next drain)\n",stderr);
    }
    return true;
}
void AbandonGpuCommandValidation(WGPUCommandEncoder owner){
    auto abandon=[](void* argument){commandValidation.erase(static_cast<WGPUCommandEncoder>(argument));};
    if(emscripten_is_main_runtime_thread())abandon(owner);
    else if(auto* queue=ComputeQueue())emscripten_proxy_sync(queue,emscripten_main_runtime_thread_id(),abandon,owner);
}
void ClearGpuBindGroupCache(){if(emscripten_is_main_runtime_thread())ClearBindings();}
void InvalidateGpuBindGroupsForBuffer(WGPUBuffer buffer){
    if(!emscripten_is_main_runtime_thread())return;
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    auto indexed=bindingKeysByBuffer.find(buffer);if(indexed==bindingKeysByBuffer.end())return;
    const std::vector<std::string> keys(indexed->second.begin(),indexed->second.end());
    for(const auto& key:keys){auto current=bindingCache.find(key);if(current!=bindingCache.end())EraseBinding(current);}
    PublishBindings();
}
void InvalidateGpuBindGroupsForTexture(WGPUTexture texture){
    if(!emscripten_is_main_runtime_thread())return;
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    auto indexed=bindingKeysByTexture.find(texture);if(indexed==bindingKeysByTexture.end())return;
    const std::vector<std::string> keys(indexed->second.begin(),indexed->second.end());
    for(const auto& key:keys){auto current=bindingCache.find(key);if(current!=bindingCache.end())EraseBinding(current);}
    PublishBindings();
}

// [skperf] round 56 bgLast (opt-in ?bgLast=1; default 0 = original): consecutive draws of one material usually build
// identical group-0 entries (uniforms bind the ring at offset 0 with dynamic offsets), yet every draw built the string
// key, took the shared bindingCacheMutex and did the std::map lookup (tc70 Badlands: bindGroup stage 4.9 us/draw). Each
// worker remembers its last 4 cache hits (pipeline + entries copied field by field + the group, holding its own reference
// + the cache epoch/device generation). A draw whose pipeline and entries are equal field by field, with no shared-cache
// erase since (epoch) and the same device, gets that same group (+1 reference): the group the cache returns for that
// key. Every 32nd memo hit still goes through the shared cache so its LRU keeps hot groups.
namespace cpBgLast {
int On() {
    static const int v = [] { const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('bgLast') === '1' ? 1 : 0; } catch (e) { return 0; } });
        std::fprintf(stderr, "CP2077 skperf switches: bgLast=%d\n", on); return on; }();
    return v;
}
struct Slot { WGPURenderPipeline pipeline = nullptr; std::vector<WGPUBindGroupEntry> entries; WGPUBindGroup group = nullptr; std::uint64_t epoch = 0, generation = 0; };
bool SameEntry(const WGPUBindGroupEntry& a, const WGPUBindGroupEntry& b) {
    return a.nextInChain == b.nextInChain && a.binding == b.binding && a.buffer == b.buffer && a.offset == b.offset && a.size == b.size &&
        a.sampler == b.sampler && a.textureView == b.textureView;
}
}
WGPUBindGroup FindCachedGpuGraphicsBindGroup(WGPURenderPipeline pipeline, const WGPUBindGroupEntry* entries, std::size_t count) {
    if (emscripten_is_main_runtime_thread() || !pipeline || (count && !entries)) return nullptr;
    const bool memo = cpBgLast::On() != 0;
    thread_local cpBgLast::Slot slots[4]; thread_local unsigned nextSlot = 0, memoHits = 0;
    std::uint64_t epoch = 0, generation = 0;
    if (memo) {
        epoch = bindingEpoch.load(std::memory_order_seq_cst); generation = cp2077::web::GpuDeviceGeneration();
        for (auto& slot : slots) {
            if (!slot.group || slot.pipeline != pipeline || slot.entries.size() != count || slot.epoch != epoch || slot.generation != generation) continue;
            bool same = true;
            for (std::size_t i = 0; i < count && same; ++i) same = cpBgLast::SameEntry(slot.entries[i], entries[i]);
            if (!same) continue;
            if ((++memoHits & 31) != 0) { wgpuBindGroupAddRef(slot.group); return slot.group; }
            break; // periodic pass through the shared cache (LRU touch); falls through below
        }
    }
    Request request{}; request.graphicsPipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
    if (!ReuseBindingOnWorker(request)) return nullptr;
    if (memo) { // remember this hit (one extra reference held by the slot)
        cpBgLast::Slot* slot = nullptr;
        for (auto& s : slots) if (s.group == request.created && s.pipeline == pipeline) { slot = &s; break; }
        WGPUBindGroup previous = nullptr;
        if (!slot) { slot = &slots[nextSlot++ & 3]; previous = slot->group; slot->group = request.created; wgpuBindGroupAddRef(slot->group); }
        slot->pipeline = pipeline; slot->entries.assign(entries, entries + count); slot->epoch = epoch; slot->generation = generation;
        if (previous) cp2077::web::ReleaseGpuBindGroup(previous);
    }
    return request.created;
}
// perf12: bind groups created on the main thread while encoding queued draws/dispatches skip the three error scopes
// (push x3, pop x3 and their promise callbacks per creation; ~180 creations per BODY TYPE frame on the now critical
// main thread). An invalid group still reports through the device's uncaptured-error handler, and the owning
// encoder is marked so the deferred submit gives that list its own queue.submit: if it is invalid, the browser
// rejects only that list (the same isolation the scopes gave). The group is cached at creation. ?scopes=1 restores
// the scoped creation.
bool Perf12UnscopedMainBindGroups(){
    static const int value=[]{
        const int v=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('scopes')==='1'?0:1;});
        std::fprintf(stderr,"CP2077 perf12 switches: unscopedMainBindGroups=%d\n",v);
        return v;
    }();
    return value!=0;
}
WGPUBindGroup CreateUnscopedBindGroupOnMain(Request& request, WGPUCommandEncoder owner){
    request.device=cp2077::web::MainThreadGpuDevice();
    if(!request.device)return nullptr;
    request.deviceGeneration=cp2077::web::GpuDeviceGeneration();
    if(ReuseBinding(request))return request.created;
    auto layout=request.graphicsPipeline?wgpuRenderPipelineGetBindGroupLayout(request.graphicsPipeline,0)
                                        :wgpuComputePipelineGetBindGroupLayout(request.pipeline,0);
    if(!layout)return nullptr;
    WGPUBindGroupDescriptor descriptor=WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout=layout;descriptor.entryCount=request.count;descriptor.entries=request.entries;
    request.created=wgpuDeviceCreateBindGroup(request.device,&descriptor);
    wgpuBindGroupLayoutRelease(layout);
    if(!request.created)return nullptr;
    StoreBinding(request); // the cache takes its own reference; the caller owns this one
    auto& state=commandValidation[owner];
    if(!state){state=std::make_shared<CommandValidation>();state->owner=owner;wgpuCommandEncoderAddRef(owner);}
    state->unscoped=true;
    static unsigned created=0;
    if(++created<=4||!(created&(created-1)))
        std::fprintf(stderr,"CP2077 perf12 unscoped main-thread bind groups created=%u\n",created);
    return request.created;
}
WGPUBindGroup CreateGpuGraphicsBindGroupOnMain(WGPURenderPipeline pipeline, const WGPUBindGroupEntry* entries,
                                              std::size_t count, WGPUCommandEncoder owner) {
    if (emscripten_is_main_runtime_thread() && pipeline && owner && (!count || entries) && Perf12UnscopedMainBindGroups()) {
        Request request{}; request.graphicsPipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
        return CreateUnscopedBindGroupOnMain(request, owner);
    }
    // Same creation, cache and asynchronous per-encoder validation as the worker path (Begin), run inline on the
    // main thread inside a batched handoff. Only the deferred-validation form is used: the caller stack frame does
    // not outlive the error-scope callbacks, so with the validation backlog full this declines instead.
    if (!emscripten_is_main_runtime_thread() || !pipeline || !owner || (count && !entries)) return nullptr;
    if (pendingValidationOperations >= 1024) {
        // Declining fails the batch (the list is dropped); report it so a dropped list can be attributed (perf7).
        static unsigned declines = 0;
        if (++declines <= 8 || !(declines & (declines - 1)))
            std::fprintf(stderr, "CP2077 queued graphics bind group declined (validation backlog full): count=%u pending=%u\n",
                declines, pendingValidationOperations);
        return nullptr;
    }
    Request request{}; request.graphicsPipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
    request.validationOwner = owner;
    Begin(nullptr, &request);
    if (request.failed && request.created) { wgpuBindGroupRelease(request.created); request.created = nullptr; }
    return request.created;
}
WGPUBindGroup FindCachedGpuComputeBindGroup(WGPUComputePipeline pipeline, const WGPUBindGroupEntry* entries, std::size_t count) {
    if (emscripten_is_main_runtime_thread() || !pipeline || (count && !entries)) return nullptr;
    Request request{}; request.pipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
    return ReuseBindingOnWorker(request) ? request.created : nullptr;
}
WGPUBindGroup CreateGpuComputeBindGroupOnMain(WGPUComputePipeline pipeline, const WGPUBindGroupEntry* entries,
                                             std::size_t count, WGPUCommandEncoder owner) {
    if (emscripten_is_main_runtime_thread() && pipeline && owner && (!count || entries) && Perf12UnscopedMainBindGroups()) {
        Request request{}; request.pipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
        return CreateUnscopedBindGroupOnMain(request, owner);
    }
    // Queued compute dispatches (perf7): same creation, main-side cache and per-encoder deferred validation as the
    // worker path (Begin), run inline inside the batched handoff. A full validation backlog cannot use Begin's
    // serial form here (its error-scope callbacks would outlive this frame), so it creates the group unscoped:
    // an invalid group then reports through the device's uncaptured-error path and invalidates the encoder at
    // finish, instead of skipping the dispatch. Counted and reported (bounded).
    if (!emscripten_is_main_runtime_thread() || !pipeline || !owner || (count && !entries)) return nullptr;
    if (pendingValidationOperations < 1024) {
        Request request{}; request.pipeline = pipeline; request.group = 0; request.entries = entries; request.count = count;
        request.validationOwner = owner;
        Begin(nullptr, &request);
        if (request.failed && request.created) { wgpuBindGroupRelease(request.created); request.created = nullptr; }
        return request.created;
    }
    static unsigned unscoped = 0;
    ++unscoped;
    if (unscoped <= 8 || !(unscoped & (unscoped - 1)))
        std::fprintf(stderr, "CP2077 queued compute bind group created unscoped (validation backlog full): count=%u pending=%u\n",
            unscoped, pendingValidationOperations);
    auto device = cp2077::web::MainThreadGpuDevice();
    if (!device) return nullptr;
    auto layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    if (!layout) return nullptr;
    WGPUBindGroupDescriptor descriptor = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    descriptor.layout = layout; descriptor.entryCount = count; descriptor.entries = entries;
    auto created = wgpuDeviceCreateBindGroup(device, &descriptor);
    wgpuBindGroupLayoutRelease(layout);
    return created;
}
WGPUBindGroup CreateGpuComputeBindGroup(WGPUComputePipeline pipeline, std::uint32_t group,
                                        const WGPUBindGroupEntry* entries, std::size_t count, WGPUCommandEncoder owner) {
    if (count && !entries) return nullptr;
    Request request{}; request.pipeline = pipeline; request.group = group; request.entries = entries; request.count = count;request.validationOwner=owner;
    return Execute(request) ? request.created : nullptr;
}
void ReleaseGpuBindGroups(std::vector<WGPUBindGroup>&& groups) {
    // Retirement of a whole command list (perf7): cached groups drop their lease on the worker as before; every
    // uncached group (ring-slot uniforms are never cached) goes to the main thread in ONE asynchronous handoff
    // instead of one synchronous proxy each. Their GPU work has completed; nothing waits on the release.
    if (groups.empty()) return;
    if (emscripten_is_main_runtime_thread()) { for (auto group : groups) if (group) wgpuBindGroupRelease(group); groups.clear(); return; }
    auto* rest = new std::vector<WGPUBindGroup>();
    for (auto group : groups) if (group && !ReleaseCachedBindingOnWorker(group)) rest->push_back(group);
    groups.clear();
    if (rest->empty()) { delete rest; return; }
    static std::atomic<unsigned> batches{0};
    const unsigned n = batches.fetch_add(1) + 1;
    if (n <= 4 || (n <= 65536 && !(n & (n - 1))))
        std::fprintf(stderr, "CP2077 bind group batch release probe: batches=%u lastUncached=%zu\n", n, rest->size());
    auto* queue = ComputeQueue();
    if (!queue || !emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), [](void* argument) {
            auto* list = static_cast<std::vector<WGPUBindGroup>*>(argument);
            for (auto group : *list) wgpuBindGroupRelease(group);
            delete list;
        }, rest)) {
        // Fall back to the synchronous per-group release rather than leak.
        for (auto group : *rest) ReleaseGpuBindGroup(group);
        delete rest;
    }
}
bool ReleaseGpuBindGroup(WGPUBindGroup group) {
    if (!group) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuBindGroupRelease(group); return true; }
    if(ReleaseCachedBindingOnWorker(group))return true;
    auto* queue = ComputeQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuBindGroupRelease(static_cast<WGPUBindGroup>(argument)); }, group);
}
WGPUBindGroup CreateGpuGraphicsBindGroup(WGPURenderPipeline pipeline, std::uint32_t group,
                                        const WGPUBindGroupEntry* entries, std::size_t count, WGPUCommandEncoder owner) {
    if (count && !entries) return nullptr;
    Request request{}; request.graphicsPipeline = pipeline;
    request.group = group; request.entries = entries; request.count = count;request.validationOwner=owner;
    return Execute(request) ? request.created : nullptr;
}
bool EncodeGpuComputeDispatch(WGPUCommandEncoder encoder, WGPUComputePipeline pipeline,
                              const ComputeBindGroup* groups, std::size_t count,
                              std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!encoder || (count && !groups)) return false;
    Request request{}; request.pipeline = pipeline; request.encoder = encoder; request.groups = groups;request.validationOwner=encoder;
    request.count = count; request.x = x; request.y = y; request.z = z;
    return Execute(request);
}
bool EncodeGpuComputeDispatchIndirect(WGPUCommandEncoder encoder, WGPUComputePipeline pipeline,
    const ComputeBindGroup* groups, std::size_t count, WGPUBuffer arguments, std::uint64_t offset) {
    if (!encoder || !arguments || offset % 4 || (count && !groups)) return false;
    Request request{}; request.pipeline = pipeline; request.encoder = encoder; request.groups = groups;request.validationOwner=encoder;
    request.count = count; request.indirectArguments = arguments; request.indirectOffset = offset;
    return Execute(request);
}
}
