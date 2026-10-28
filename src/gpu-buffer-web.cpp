// Actual WebGPU allocation; no emulated geometry or UI is produced here.
#include "gpu-buffer-web.h"
#include "gpu-compute-web.h"
#include "gpu-device-web.h"
#include "gpu-uniform-web.h"
#include "gpu-handoff-stats-web.h"
#include "engine-wgtrace-web.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include <atomic>
#include <map>
#include <list>
#include <string>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <emscripten/emscripten.h>

namespace {
std::atomic<cp2077::web::GpuMappedBufferFlusher> mappedBufferFlusher{nullptr};
em_proxying_queue* BufferQueue() {
    static auto* queue = em_proxying_queue_create();
    return queue;
}
// [nomadperf] Buffer creations ride their own proxy queue, which the main thread also services between the
// command lists of a long deferred submit (ServiceGpuAllocationRequests). Before, a render worker that needed a
// new buffer (immutable uniform / staging / vertex) waited behind the whole deferred encode backlog: 20-30 ms per
// call in the q000 scene ('GPU allocation wait probe' deliveryMs). Creation is independent of earlier submissions
// (new handle, initial bytes via mappedAtCreation), so it may run before them. Writes, reads and destroys keep
// BufferQueue and its drain-first ordering. ?allocPrio=0 = old (allocations on BufferQueue, no mid-submit service).
std::atomic<int> allocPrioState{-1}; // -1 unknown, 0 off, 1 on; written on the main thread
bool AllocPrioOn() {
    int v = allocPrioState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('allocPrio') === '0' ? 0 : 1; });
        allocPrioState.store(v, std::memory_order_release);
        std::fprintf(stderr, "CP2077 nomadperf switches: allocPrio=%d\n", v);
    }
    return v != 0;
}
em_proxying_queue* AllocationQueue() {
    static auto* queue = em_proxying_queue_create();
    return queue;
}
std::atomic<unsigned> allocationServiced{0}, allocationServicePolls{0};
struct Allocation {
    std::uint64_t size;
    WGPUBufferUsage usage;
    const void* initialData;
    std::size_t initialSize;
    WGPUBuffer buffer = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned pendingScopes = 3;
    bool failed = false;
    WGPUDevice device=nullptr;bool immutable=false;std::string immutableKey;
    double queuedAt=0,beganAt=0,encodedAt=0,completedAt=0;
    bool cacheHit=false;
    const char* label=nullptr; // WebGPU debug label, shown in browser validation messages
    bool workerCacheHit=false;
    cpEngineTrace::Request trace;
};
#include "gpu-immutable-uniform-cache-web.inl"
void AllocationChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                       WGPUStringView message, void* userdata, void*) {
    auto* allocation = static_cast<Allocation*>(userdata);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        allocation->failed = true;
        std::fputs("CP2077 WebGPU buffer allocation failed: ", stderr);
        if (message.data) {
            if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
            else std::fwrite(message.data, 1, message.length, stderr);
        }
        std::fputc('\n', stderr);
    }
    if (--allocation->pendingScopes != 0) return;
    if (!cp2077::web::IsGpuDeviceReady() || allocation->device!=cp2077::web::MainThreadGpuDevice()) allocation->failed = true;
    if (allocation->failed && allocation->buffer) {
        wgpuBufferDestroy(allocation->buffer);
        wgpuBufferRelease(allocation->buffer);
        allocation->buffer = nullptr;
    }
    StoreUniform(*allocation);
    wgpuDeviceRelease(allocation->device);
    allocation->completedAt=emscripten_get_now();
    allocation->trace.Settled(allocation->failed);
    emscripten_proxy_finish(allocation->context);
}
// perf9: small non-mappable buffers return to the worker as soon as they are created; their error scopes still
// run, but settle asynchronously. The synchronous form blocked the allocating render worker for a GPU-process
// round trip per creation (body screen: ~60 ms/s of worker blocking, 0.4-1.3 ms per call, 14-57 ms under load).
// The descriptor is already checked here (size <= maxBufferSize, no map usage, mapped size 4-aligned), so the
// remaining failures are out-of-memory/internal errors. Such a buffer is an error object: it is logged, evicted
// from the immutable cache, and any bind group or submit that uses it fails its own (existing) validation, which
// drops that command list. ?syncBufferValidation=1 restores the synchronous wait.
constexpr std::uint64_t kAsyncValidatedBufferMaxBytes = 1ull << 20;
std::atomic<unsigned> asyncValidatedAllocations{0}, asyncValidationFailures{0};
struct AsyncAllocationCheck { WGPUDevice device; WGPUBuffer buffer; std::uint64_t size; WGPUBufferUsage usage; unsigned pending = 3; bool failed = false; };
// [skperf] Street Kid bar: one 14,745,600-byte (1280x720x16) non-mappable buffer per frame waited ~33 ms of its
// ~38 ms on the synchronous error-scope round trip (GPU process behind the frame's queue), stalling a render worker
// every frame. Non-mappable buffers up to 256 MiB now validate asynchronously too (same failure handling as perf9:
// an OOM buffer is an error object; its users fail their own validation). ?bigAsyncVal=0 = old 1 MiB limit.
constexpr std::uint64_t kAsyncValidatedBigBufferMaxBytes = 256ull << 20;
bool UsesAsyncAllocationValidation(const Allocation& allocation) {
    static int synchronous = -1; // main thread only (BeginAllocation)
    if (synchronous < 0) synchronous = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('syncBufferValidation') === '1' ? 1 : 0; });
    static int big = -1;
    if (big < 0) { big = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('bigAsyncVal') === '0' ? 0 : 1; }); std::fprintf(stderr, "CP2077 skperf switches: bigAsyncVal=%d\n", big); }
    return !synchronous && allocation.size <= (big ? kAsyncValidatedBigBufferMaxBytes : kAsyncValidatedBufferMaxBytes) &&
        !(allocation.usage & (WGPUBufferUsage_MapRead | WGPUBufferUsage_MapWrite));
}
void AsyncAllocationChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                            WGPUStringView message, void* userdata, void*) {
    auto* check = static_cast<AsyncAllocationCheck*>(userdata);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        // A cancelled scope (device lost / shutdown) is not reported; the device-lost path covers it.
        if (status == WGPUPopErrorScopeStatus_Success && !check->failed) {
            const unsigned n = asyncValidationFailures.fetch_add(1) + 1;
            if (n <= 16 || !(n & (n - 1))) {
                std::fprintf(stderr, "CP2077 WebGPU buffer allocation failed (async validation): count=%u bytes=%llu usage=0x%llx ",
                    n, static_cast<unsigned long long>(check->size), static_cast<unsigned long long>(check->usage));
                if (message.data) {
                    if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
                    else std::fwrite(message.data, 1, message.length, stderr);
                }
                std::fputc('\n', stderr);
            }
        }
        check->failed = true;
    }
    if (--check->pending != 0) return;
    // Drop cache ownership so later lookups cannot hand out the failed buffer (no-op for uncached buffers).
    if (check->failed) EvictUniform(check->buffer);
    wgpuBufferRelease(check->buffer);
    wgpuDeviceRelease(check->device);
    delete check;
}
void BeginAllocation(em_proxying_ctx* context, void* argument) {
    auto* allocation = static_cast<Allocation*>(argument);
    allocation->context = context;
    allocation->beganAt=emscripten_get_now();
    allocation->trace.Begin();
    auto device = cp2077::web::MainThreadGpuDevice();
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (!device || !cp2077::web::MainThreadGpuLimits(limits) || allocation->size > limits.maxBufferSize) {
        std::fputs("CP2077 WebGPU: buffer exceeds device limit or device is unavailable\n", stderr);
        allocation->encodedAt=allocation->completedAt=emscripten_get_now();
        emscripten_proxy_finish(context); return;
    }
    allocation->device=device;
    if(ReuseUniform(*allocation))return;
    wgpuDeviceAddRef(device);
    // Reject invalid/out-of-memory handles before publishing them to the engine.
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.size = allocation->size;
    descriptor.usage = allocation->usage;
    descriptor.mappedAtCreation = allocation->initialData != nullptr;
    if (allocation->label) descriptor.label = {allocation->label, WGPU_STRLEN};
    allocation->buffer = wgpuDeviceCreateBuffer(device, &descriptor);
    if (!allocation->buffer) allocation->failed = true;
    if (allocation->buffer && descriptor.mappedAtCreation) {
        void* mapped = wgpuBufferGetMappedRange(allocation->buffer, 0, allocation->size);
        if (!mapped) allocation->failed = true;
        else {
            // WebGPU guarantees zero initialization; explicitly zero padding
            // when supplying initial data to preserve deterministic upload bytes.
            std::memset(mapped, 0, static_cast<std::size_t>(allocation->size));
            std::memcpy(mapped, allocation->initialData, allocation->initialSize);
        }
        wgpuBufferUnmap(allocation->buffer);
    }
    if (!allocation->failed && UsesAsyncAllocationValidation(*allocation)) {
        // The check owns the device reference taken above and one extra buffer reference.
        auto* check = new AsyncAllocationCheck{device, allocation->buffer, allocation->size, allocation->usage};
        wgpuBufferAddRef(check->buffer);
        allocation->encodedAt = emscripten_get_now();
        allocation->trace.CallEnd(); allocation->trace.Scoped(true);
        // Cache before popping, so a failure reported even synchronously still finds (and evicts) the record.
        StoreUniform(*allocation);
        WGPUPopErrorScopeCallbackInfo asyncCallback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
        asyncCallback.mode = WGPUCallbackMode_AllowSpontaneous;
        asyncCallback.callback = AsyncAllocationChecked;
        asyncCallback.userdata1 = check;
        for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, asyncCallback);
        const unsigned n = asyncValidatedAllocations.fetch_add(1) + 1;
        if (n <= 2 || (n <= 65536 && !(n & (n - 1))))
            std::fprintf(stderr, "CP2077 async buffer validation probe: allocations=%u failures=%u\n",
                n, asyncValidationFailures.load());
        allocation->completedAt = emscripten_get_now();
        allocation->trace.Settled(false);
        emscripten_proxy_finish(context);
        return;
    }
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = AllocationChecked;
    callback.userdata1 = allocation;
    allocation->encodedAt=emscripten_get_now();
    allocation->trace.CallEnd();allocation->trace.Scoped();
    for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
}
bool RunAllocation(Allocation& request) {
    auto* queue=AllocPrioOn()?AllocationQueue():BufferQueue();if(!queue)return false;
    std::call_once(cpGpuHandoffStats::registration,cpGpuHandoffStats::Register);
    request.queuedAt=emscripten_get_now();
    request.trace.Queue("BufferAllocation");
    const bool dispatched=ReuseUniformOnWorker(request) ||
        emscripten_proxy_sync_with_ctx(queue,emscripten_main_runtime_thread_id(),BeginAllocation,&request);
    const double ended=emscripten_get_now(),wall=ended-request.queuedAt;
    request.trace.WaitEnd();
    const unsigned kind=request.cacheHit?0:request.immutable?1:2;
    if(!request.workerCacheHit){
        const double encoded=request.cacheHit?request.completedAt:request.encodedAt;
        // Usage flags identify the allocating call site in the handoff table (uniform, staging, vertex, ...).
        char label[64];std::snprintf(label,sizeof(label),"%s usage=0x%llx",
            kind==0?"BufferMainCacheHit":kind==1?"BufferImmutableMiss":"BufferOrdinaryAllocation",
            static_cast<unsigned long long>(request.usage));
        cpGpuHandoffStats::Record(label,
            wall,encoded-request.beganAt,request.beganAt-request.queuedAt,
            request.completedAt?ended-request.completedAt:0,request.completedAt?request.completedAt-encoded:0);
    }
    struct Totals{unsigned calls=0,slowSamples=0;double wallMs=0;};
    static thread_local Totals totals[3];auto& t=totals[kind];++t.calls;t.wallMs+=wall;
    if(t.calls<=4 || (t.calls<=4096 && !(t.calls&(t.calls-1))) || (wall>=20 && ++t.slowSamples<=16)) {
        const double encoded=request.cacheHit?request.completedAt:request.encodedAt;
        std::fprintf(stderr,"CP2077 GPU allocation wait probe: kind=%s label=%s usage=0x%llx calls=%u bytes=%llu wallMs=%.3f totalWallMs=%.3f deliveryMs=%.3f creationMs=%.3f validationAndCacheMs=%.3f returnMs=%.3f success=%u\n",
            request.workerCacheHit?"immutable-worker-hit":kind==0?"immutable-hit":kind==1?"immutable-miss":"ordinary",request.label?request.label:"-",static_cast<unsigned long long>(request.usage),t.calls,static_cast<unsigned long long>(request.size),wall,t.wallMs,
            request.beganAt-request.queuedAt,encoded-request.beganAt,
            request.completedAt?request.completedAt-encoded:0,request.completedAt?ended-request.completedAt:0,unsigned(dispatched&&!request.failed&&request.buffer));
    }
    return dispatched;
}
}

namespace cp2077::web {
void ServiceGpuAllocationRequests(){
    // Main thread only; cheap when nothing is queued. Nested execution of this queue is guarded by emscripten.
    if(!emscripten_is_main_runtime_thread() || allocPrioState.load(std::memory_order_relaxed)!=1)return;
    allocationServicePolls.fetch_add(1,std::memory_order_relaxed);
    emscripten_proxy_execute_queue(AllocationQueue());
}
void ClearGpuImmutableUniformCache(){if(emscripten_is_main_runtime_thread())ClearUniforms();}

void SetGpuMappedBufferFlusher(GpuMappedBufferFlusher flusher) { mappedBufferFlusher.store(flusher); }
bool FlushMappedGpuBuffers() {
    const auto flusher=mappedBufferFlusher.load();
    // The standalone bridge has no original buffer registry or mapped buffers.
    // Original mapped allocations install their real upload callback below it.
    return !flusher || flusher();
}
bool WriteGpuBuffer(WGPUBuffer buffer, const void* bytes, std::size_t size) {
    return WriteGpuBufferRange(buffer,0,bytes,size);
}
bool WriteGpuBufferRange(WGPUBuffer buffer, std::uint64_t offset, const void* bytes, std::size_t size) {
    if (!buffer || !bytes || !size || size % 4 || offset % 4) return false;
    struct Request { WGPUBuffer buffer; std::uint64_t offset; const void* bytes; std::size_t size; bool issued = false; cpEngineTrace::Request trace; bool proxied = false; } request{buffer, offset, bytes, size};
    request.trace.Queue("WriteGpuBufferRange");
    auto perform = [](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (request.proxied) DrainDeferredGpuWork(); // perf12: after earlier asynchronous submissions
        request.trace.Begin();
        auto queue = MainThreadGpuQueue();
        if (!queue || !IsGpuDeviceReady() || IsImmutableUniformRecord(request.buffer)) return;
        const auto capacity=wgpuBufferGetSize(request.buffer);
        if(request.offset>capacity || request.size>capacity-request.offset)return;
        wgpuQueueWriteBuffer(queue, request.buffer, request.offset, request.bytes, request.size);
        request.issued = true;
        request.trace.CallEnd();
    };
    if (emscripten_is_main_runtime_thread()) perform(&request);
    else {
        auto* queue = BufferQueue();
        request.proxied = true;
        if (!queue || !emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), perform, &request)) return false;
    }
    request.trace.WaitEnd();return request.issued;
}
// [skperf] round 9 uniBatch (default ON, ?uniBatch=0 = one writeBuffer per range): consecutive ranges in the array
// that target the same buffer and are contiguous in it (offset == previous end) are copied into one staging block
// and written with ONE queue.writeBuffer; the buffer size is queried once per run of the same buffer. Writes keep
// their array order (only neighbours merge, never overlapping ranges), every range passes the same validity checks,
// and issued[i] is set exactly as before. Counters: [0] ranges, [1] writeBuffer calls, [2] getSize calls.
std::atomic<unsigned long long> skperfWriteStats[3];
static bool UniBatchOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('uniBatch') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: uniBatch=%d\n", v); return v; }();
    return on != 0;
}
bool WriteGpuBufferRanges(const GpuBufferWrite* writes, std::size_t count, bool* issued) {
    if (!count) return true;
    if (!writes || !issued) return false;
    for (std::size_t i = 0; i < count; ++i) issued[i] = false;
    const bool batch = UniBatchOn(); // resolved before any main-thread handoff below
    struct Request { const GpuBufferWrite* writes; std::size_t count; bool* issued; bool proxied = false; bool batch = false; } request{writes, count, issued};
    request.batch = batch;
    auto perform = [](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (request.proxied) DrainDeferredGpuWork(); // perf12: after earlier asynchronous submissions
        auto queue = MainThreadGpuQueue();
        if (!queue || !IsGpuDeviceReady()) return;
        skperfWriteStats[0].fetch_add(request.count, std::memory_order_relaxed);
        if (request.batch) {
            static std::vector<unsigned char> staging; // main thread only
            WGPUBuffer lastBuffer = nullptr; std::uint64_t capacity = 0; bool immutable = false;
            std::size_t i = 0;
            while (i < request.count) {
                const auto& first = request.writes[i];
                if (!first.buffer || !first.bytes || !first.size || first.size % 4 || first.offset % 4) { ++i; continue; }
                if (first.buffer != lastBuffer) {
                    lastBuffer = first.buffer; immutable = IsImmutableUniformRecord(first.buffer);
                    capacity = immutable ? 0 : wgpuBufferGetSize(first.buffer); skperfWriteStats[2].fetch_add(1, std::memory_order_relaxed);
                }
                if (immutable || first.offset > capacity || first.size > capacity - first.offset) { ++i; continue; }
                // Extend the run while the next range is valid, same buffer and starts where this one ends.
                std::size_t j = i + 1; std::uint64_t end = first.offset + first.size;
                while (j < request.count) {
                    const auto& next = request.writes[j];
                    if (next.buffer != first.buffer || !next.bytes || !next.size || next.size % 4 || next.offset != end ||
                        next.size > capacity - next.offset) break;
                    end += next.size; ++j;
                }
                if (j == i + 1) {
                    wgpuQueueWriteBuffer(queue, first.buffer, first.offset, first.bytes, first.size);
                } else {
                    staging.resize(static_cast<std::size_t>(end - first.offset));
                    std::size_t at = 0;
                    for (std::size_t k = i; k < j; ++k) { std::memcpy(staging.data() + at, request.writes[k].bytes, request.writes[k].size); at += request.writes[k].size; }
                    wgpuQueueWriteBuffer(queue, first.buffer, first.offset, staging.data(), staging.size());
                }
                skperfWriteStats[1].fetch_add(1, std::memory_order_relaxed);
                for (std::size_t k = i; k < j; ++k) request.issued[k] = true;
                i = j;
            }
            return;
        }
        for (std::size_t i = 0; i < request.count; ++i) {
            const auto& w = request.writes[i];
            if (!w.buffer || !w.bytes || !w.size || w.size % 4 || w.offset % 4 || IsImmutableUniformRecord(w.buffer)) continue;
            const auto capacity = wgpuBufferGetSize(w.buffer);
            if (w.offset > capacity || w.size > capacity - w.offset) continue;
            wgpuQueueWriteBuffer(queue, w.buffer, w.offset, w.bytes, w.size);
            request.issued[i] = true;
            skperfWriteStats[1].fetch_add(1, std::memory_order_relaxed); skperfWriteStats[2].fetch_add(1, std::memory_order_relaxed);
        }
    };
    if (emscripten_is_main_runtime_thread()) { perform(&request); return true; }
    auto* queue = BufferQueue();
    request.proxied = true;
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), perform, &request);
}
bool ReadGpuBuffer(WGPUBuffer buffer, std::uint64_t mappedBytes, void* output, std::size_t outputBytes) {
    if (!buffer || !output || !outputBytes || outputBytes > mappedBytes || mappedBytes % 4 ||
        mappedBytes > std::numeric_limits<std::size_t>::max() || emscripten_is_main_runtime_thread()) return false;
    struct Request {
        WGPUBuffer buffer;
        std::uint64_t mappedBytes;
        void* output;
        std::size_t outputBytes;
        em_proxying_ctx* context = nullptr;
        bool success = false;
        cpEngineTrace::Request trace;
    } request{buffer, mappedBytes, output, outputBytes};
    request.trace.Queue("ReadGpuBufferMapAsync");
    auto* queue = BufferQueue();
    const bool result=queue && emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto& request = *static_cast<Request*>(argument);
            request.context = context;
            DrainDeferredGpuWork(); // perf12: map after every asynchronous submission issued so far
            request.trace.Begin();request.trace.Scoped();
            if (!MainThreadGpuDevice()) { emscripten_proxy_finish(context); return; }
            WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.userdata1 = &request;
            callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata, void*) {
                auto& request = *static_cast<Request*>(userdata);
                if (status == WGPUMapAsyncStatus_Success) {
                    const auto* bytes = wgpuBufferGetConstMappedRange(request.buffer, 0, request.mappedBytes);
                    if (bytes && IsGpuDeviceReady()) {
                        std::memcpy(request.output, bytes, request.outputBytes);
                        request.success = true;
                    }
                    wgpuBufferUnmap(request.buffer);
                }
                request.trace.Settled(!request.success);emscripten_proxy_finish(request.context);
            };
            // An immediate native error may finish the proxy and wake its
            // worker inside this API call. Keep tracing data on this stack.
            auto trace=request.trace;
            wgpuBufferMapAsync(request.buffer, WGPUMapMode_Read, 0, request.mappedBytes, callback);
            trace.CallEnd();
        }, &request) && request.success;
    request.trace.WaitEnd();return result;
}
WGPUBuffer CreateGpuImmutableUniformBuffer(std::size_t bytes,const void* data){
 if(!bytes || bytes>std::numeric_limits<std::size_t>::max()-3 || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady())return nullptr;
 if(bytes>immutableUniformBudget){
  Allocation allocation{(bytes+3)&~std::size_t(3),WGPUBufferUsage_Uniform|WGPUBufferUsage_Storage,data,data?bytes:0};allocation.immutable=true;allocation.label="cp-immutable-uniform-large";
  if(!RunAllocation(allocation))return nullptr;
  return allocation.buffer;
 }
 const auto padded=(bytes+3)&~std::size_t(3);
 // Separate immutable uniform and immutable upload keys without changing
 // the actual bytes supplied to the GPU.
 std::string key(padded+1,'\0');key[0]=1;if(data)std::memcpy(key.data()+1,data,bytes);
 Allocation allocation{padded,WGPUBufferUsage_Uniform|WGPUBufferUsage_Storage,key.data()+1,padded};allocation.immutable=true;allocation.immutableKey=key;allocation.label="cp-immutable-uniform";
 if(!RunAllocation(allocation))return nullptr;
 return allocation.buffer;
}
WGPUBuffer CreateGpuUniformBuffer(const char* shader, std::uint32_t group,
                                 std::uint32_t binding, const void* nativeData,
                                 std::size_t nativeBytes) {
    const auto* layout = FindUniformLayout(shader, group, binding);
    if (!layout || !nativeData || nativeBytes < layout->nativeMinimumBytes ||
        emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    std::vector<unsigned char> packed(layout->webBytes);
    if (!RepackUniform(*layout, nativeData, nativeBytes, packed.data(), packed.size()))
        return nullptr;
    // Creation waits for mapped upload and WebGPU validation callbacks before
    // returning, so this owned temporary remains alive throughout the upload.
    return CreateGpuImmutableUniformBuffer(packed.size(),packed.data());
}
bool RepackGpuUniformBytes(const char* shader, std::uint32_t group, std::uint32_t binding,
                           const void* nativeData, std::size_t nativeBytes, std::vector<unsigned char>& packed) {
    const auto* layout = FindUniformLayout(shader, group, binding);
    if (!layout || !nativeData || nativeBytes < layout->nativeMinimumBytes) return false;
    packed.assign(layout->webBytes, 0);
    return RepackUniform(*layout, nativeData, nativeBytes, packed.data(), packed.size());
}
WGPUBuffer FindGpuImmutableUniformBuffer(std::size_t bytes, const void* data) {
    if (!bytes || !data || bytes > immutableUniformBudget || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const auto padded=(bytes+3)&~std::size_t(3);
    std::string key(padded+1,'\0');key[0]=1;std::memcpy(key.data()+1,data,bytes);
    Allocation allocation{padded,WGPUBufferUsage_Uniform,key.data()+1,padded};allocation.immutable=true;allocation.immutableKey=std::move(key);
    return ReuseUniformOnWorker(allocation) ? allocation.buffer : nullptr;
}
// [skperf] round 45 uniMemo: key format identical to FindGpuImmutableUniformBuffer (0x01 + payload padded to 4 bytes
// with zeros); assign() reuses the caller's capacity.
void BuildGpuImmutableUniformKey(std::string& key, std::size_t bytes, const void* data) {
    if (!bytes || !data || bytes > immutableUniformBudget) { key.clear(); return; }
    const auto padded=(bytes+3)&~std::size_t(3);
    key.assign(padded+1,'\0');key[0]=1;std::memcpy(key.data()+1,data,bytes);
}
// Same checks and the same hit bookkeeping as ReuseUniformOnWorker for an Allocation{padded, ..., immutableKey=key}
// (the Allocation's timestamps/trace are never read for a worker hit).
WGPUBuffer FindGpuImmutableUniformBufferKey(const std::string& key, std::size_t bytes) {
    if (!bytes || key.empty() || bytes > immutableUniformBudget || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const std::uint64_t padded=(bytes+3)&~std::size_t(3);
    std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
    if(!cp2077::web::IsGpuDeviceReady() || !immutableUniformDevice ||
       immutableUniformGeneration!=cp2077::web::GpuDeviceGeneration())return nullptr;
    auto found=immutableUniformKeys.find(key);
    if(found==immutableUniformKeys.end())return nullptr;
    auto record=immutableUniformRecords.find(found->second);
    if(record==immutableUniformRecords.end() || !record->second.cached || record->second.bytes!=padded)return nullptr;
    wgpuBufferAddRef(record->second.buffer);++record->second.leases;TouchUniform(record->second);
    ++immutableUniformHits;++immutableUniformWorkerHits;
    return record->second.buffer;
}
bool ShouldCacheGpuUniform(std::size_t bytes, const void* data) {
    // Admission after three sightings, over 64K slots indexed by an FNV-1a content hash. Each slot stores a
    // 32-bit content tag beside its count, so a different payload landing in the slot restarts the count instead
    // of inheriting it (untagged counters saturated over time and admitted nearly everything on first sight).
    // A tag collision only admits a payload early; cached bytes are still keyed by exact content.
    // Heap-allocated: static data counts against the link's fixed INITIAL_MEMORY.
    static auto* const seen = new std::atomic<std::uint64_t>[1u << 16]();
    std::uint64_t hash = 1469598103934665603ull ^ bytes;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    auto& slot = seen[hash & 0xFFFF];
    const std::uint64_t tag = (hash >> 32) << 8;
    const auto previous = slot.load(std::memory_order_relaxed);
    const std::uint64_t count = (previous & ~std::uint64_t(0xFF)) == tag ? (previous & 0xFF) : 0;
    if (count < 255) slot.store(tag | (count + 1), std::memory_order_relaxed);
    return count >= 2;
}
WGPUBuffer CreateGpuBuffer(std::uint64_t size, WGPUBufferUsage usage,
                          const void* initialData, std::size_t initialSize, const char* label) {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    if (!size || !usage || initialSize > size || (!initialData && initialSize) ||
        size > std::numeric_limits<std::size_t>::max() - 3) return nullptr;
    // Mapped-at-creation size must be four-byte aligned, including small index buffers.
    Allocation allocation{(size + 3) & ~std::uint64_t(3), usage, initialData, initialSize};
    allocation.label = label;
    // A CopySrc-only buffer cannot be remapped or written after its initial
    // upload: it has neither MapWrite nor CopyDst usage. Reuse only exact
    // validated bytes; retain a real lease until original GPU retirement.
    if (usage == WGPUBufferUsage_CopySrc && initialData && allocation.size <= immutableUniformBudget) {
        allocation.immutable=true;
        allocation.immutableKey.assign(static_cast<std::size_t>(allocation.size)+1,'\0');
        allocation.immutableKey[0]=2;
        std::memcpy(allocation.immutableKey.data()+1,initialData,initialSize);
    }
    if (!RunAllocation(allocation)) return nullptr;
    return allocation.buffer;
}
// [skperf] Buffer destroy churn probe (main thread): real destroys bucketed by (size, usage); top buckets every 4096.
static bool SkperfProbeOn() { // [skperf] round 5: per-second / churn probes only with ?perfprobe=1
    static const int value = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('perfprobe') === '1' ? 1 : 0; });
    return value != 0;
}
static void NoteGpuBufferDestroy(WGPUBuffer buffer) {
    if (!SkperfProbeOn()) return;
    static std::unordered_map<std::uint64_t, unsigned> buckets;
    static unsigned destroyed = 0; static double windowStart = 0;
    if (!windowStart) windowStart = emscripten_get_now();
    const std::uint64_t size = wgpuBufferGetSize(buffer), usage = std::uint64_t(wgpuBufferGetUsage(buffer)) & 0xffff;
    ++buckets[(size & 0xffffffffffull) | (usage << 40)];
    if ((++destroyed & 4095) != 0) return;
    const double now = emscripten_get_now(), perSecond = 4096000.0 / std::max(1.0, now - windowStart); windowStart = now;
    std::vector<std::pair<unsigned, std::uint64_t>> top; for (const auto& b : buckets) top.push_back({b.second, b.first});
    std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::fprintf(stderr, "CP2077 skperf buffer destroy churn: total=%u perSec=%.0f buckets=%u top:", destroyed, perSecond, unsigned(buckets.size()));
    for (std::size_t i = 0; i < top.size() && i < 8; ++i)
        std::fprintf(stderr, " [bytes=%llu usage=0x%x n=%u]", static_cast<unsigned long long>(top[i].second & 0xffffffffffull), unsigned(top[i].second >> 40), top[i].first);
    std::fputc('\n', stderr); buckets.clear();
}
bool DestroyGpuBuffer(WGPUBuffer buffer) {
    if (!buffer) return true;
    if (emscripten_is_main_runtime_thread()) {
        if(ReleaseUniformLease(buffer))return true;NoteGpuBufferDestroy(buffer);InvalidateGpuBindGroupsForBuffer(buffer);wgpuBufferDestroy(buffer); wgpuBufferRelease(buffer); return true;
    }
    if(ReleaseCachedUniformLeaseOnWorker(buffer))return true;
    auto* queue = BufferQueue();
    struct Request{WGPUBuffer buffer;cpEngineTrace::Request trace;double began=0,ended=0;} request{buffer};
    request.trace.Queue("BufferDestroyOrFinalRelease");const double started=emscripten_get_now();
    const bool result=queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) {
            auto& request=*static_cast<Request*>(argument);DrainDeferredGpuWork();request.began=emscripten_get_now();request.trace.Begin();auto buffer=request.buffer;
            if(!ReleaseUniformLease(buffer)){NoteGpuBufferDestroy(buffer);InvalidateGpuBindGroupsForBuffer(buffer);wgpuBufferDestroy(buffer);wgpuBufferRelease(buffer);}
            request.trace.CallEnd();request.ended=emscripten_get_now();
        }, &request);
    const double ended=emscripten_get_now();request.trace.WaitEnd();
    cpGpuHandoffStats::Record("BufferDestroyOrFinalRelease",ended-started,request.ended-request.began,request.began-started,ended-request.ended);
    return result;
}
}
