#include "gpu-command-web.h"
#include "gpu-logic-xor-web.h"
#include "gpu-compute-web.h"
#include "gpu-texture-web.h"
#include "gpu-shader-web.h"
#include "gpu-device-web.h"
#include "gpu-buffer-web.h"
#include "gpu-handoff-stats-web.h"
#include "engine-wgtrace-web.h"
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <map>
#include <array>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace {
em_proxying_queue* CommandQueue() {
    static auto* queue = em_proxying_queue_create(); return queue;
}
bool OnMain(void (*function)(void*), void* argument, const char* operation = __builtin_FUNCTION()) {
    if (emscripten_is_main_runtime_thread()) { function(argument); return true; }
    auto* queue = CommandQueue();
    if (!queue) return false;
    if (!cpEngineTrace::ProbesEnabled() && !cpEngineTrace::Enabled()) {
        // perf12: no per-handoff timers or stats tables unless ?probes=1 (or a wgtrace capture) asks for them.
        struct Plain { void (*function)(void*); void* argument; } plain{function, argument};
        return emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), [](void* data) {
            cp2077::web::DrainDeferredGpuWork(); // perf12: earlier asynchronous submissions reach the queue first
            auto& call = *static_cast<Plain*>(data);
            call.function(call.argument);
        }, &plain);
    }
    std::call_once(cpGpuHandoffStats::registration,cpGpuHandoffStats::Register);
    struct Invocation { void (*function)(void*); void* argument; double executionMs = 0, beganAt=0, finishedAt=0; unsigned frame=0,req=0; } invocation{function, argument};
    if(cpEngineTrace::Enabled()){invocation.frame=cpEngineTrace::FrameId();invocation.req=wgtrace::next_id(wgtrace::ID_REQ);}
    const double started = emscripten_get_now();
    const bool dispatched = emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* data) {
            auto& call = *static_cast<Invocation*>(data);
            cp2077::web::DrainDeferredGpuWork(); // perf12: earlier asynchronous submissions reach the queue first
            const double begin = emscripten_get_now();call.beganAt=begin;
            if(call.req)cpEngineTrace::Emit(wgtrace::PROXY_DEQ,call.frame,call.req,0,0,0,begin);
            call.function(call.argument);
            call.finishedAt=emscripten_get_now();call.executionMs = call.finishedAt - begin;
            if(call.req)cpEngineTrace::Emit(wgtrace::PROXY_CALL,call.frame,call.req,0,0,begin,call.finishedAt);
        }, &invocation);
    const double elapsed = emscripten_get_now() - started;
    if(invocation.req)cpEngineTrace::Emit(wgtrace::WAIT,invocation.frame,wgtrace::tl_job,wgtrace::W_PROXY,invocation.req,started,started+elapsed);
    cpGpuHandoffStats::Record(operation,elapsed,invocation.executionMs,
        dispatched ? invocation.beganAt-started : elapsed,
        dispatched ? started+elapsed-invocation.finishedAt : 0);
    static thread_local unsigned slowHandoffs=0;
    if(elapsed>=20 && slowHandoffs++<24)
        std::fprintf(stderr,"CP2077 GPU handoff delay probe: operation=%s wallMs=%.3f browserExecutionMs=%.3f outsideExecutionMs=%.3f deliveryMs=%.3f returnMs=%.3f dispatched=%u\n",
            operation,elapsed,invocation.executionMs,elapsed-invocation.executionMs,invocation.beganAt-started,started+elapsed-invocation.finishedAt,unsigned(dispatched));
    // Per-worker totals distinguish browser execution from synchronous handoff wait.
    static thread_local unsigned count = 0;
    static thread_local double wallMs = 0, executionMs = 0, maxMs = 0;
    struct OperationTotals { unsigned calls = 0; double wallMs = 0, executionMs = 0; };
    static thread_local std::unordered_map<const char*, OperationTotals> operations;
    auto& totals = operations[operation];
    ++totals.calls; totals.wallMs += elapsed; totals.executionMs += invocation.executionMs;
    ++count; wallMs += elapsed; executionMs += invocation.executionMs;
    if (elapsed > maxMs) maxMs = elapsed;
    if (count >= 64 && count <= 65536 && !(count & (count - 1)))
        std::fprintf(stderr, "CP2077 GPU handoff probe: calls=%u wallMs=%.3f browserExecutionMs=%.3f maxMs=%.3f dispatched=%u\n",
            count, wallMs, executionMs, maxMs, unsigned(dispatched));
    if (count >= 1024 && count <= 65536 && !(count & (count - 1))) {
        for (const auto& entry : operations)
            std::fprintf(stderr, "CP2077 GPU handoff operation probe: sample=%u operation=%s calls=%u wallMs=%.3f browserExecutionMs=%.3f\n",
                count, entry.first, entry.second.calls, entry.second.wallMs, entry.second.executionMs);
    }
    return dispatched;
}
}
namespace cp2077::web {
// Workers take encoders from a pool refilled 32 at a time in one main-thread handoff (perf7): CreateCommandList
// used to pay one synchronous handoff per list (~as many as EncodeGpuOrderedCommands). Encoders are tagged with
// the device generation that created them; stale ones are released on main at the next refill.
// [opt] encPrefill (default ON, ?encPrefill=0 = old): the q000 CPU profile (S/nomadperf/q1-scene.prof.json) showed
// ~190 ms/s of render-worker time blocked in this refill. The refill went through OnMain, which drains the WHOLE
// deferred GPU FIFO first, so a worker that found the pool empty (about once per frame: ~22-33 lists per frame vs a
// batch of 32, and every worker that found it empty sent its own refill) waited for the main thread's entire pending
// encode backlog, 20-40 ms. Encoder creation does not depend on queue order. Now the main thread keeps the pool
// topped up itself (ServiceGpuEncoderPool, called at the start of every EncodeGpuOrderedCommands batch and every 128
// operations, i.e. between the lists of a deferred submit), and a worker that still finds the pool empty refills
// through its own proxy queue, without the drain, which the main thread also services at those points.
namespace {
constexpr unsigned kEncoderPoolBatch = 32;
constexpr unsigned kEncoderPoolLow = 64, kEncoderPoolTarget = 128; // [opt] ~2-4 frames of lists
std::mutex encoderPoolMutex;
std::vector<std::pair<WGPUCommandEncoder, std::uint64_t>> encoderPool;
std::atomic<unsigned> encoderPoolSize{0}; // mirrors encoderPool.size() (written under encoderPoolMutex)
std::atomic<int> encPrefillState{-1};     // -1 unknown (old path until the main thread reads the URL), 0 off, 1 on
std::atomic<unsigned> encoderPrefills{0}, encoderPrefilled{0}, encoderWorkerRefills{0};
em_proxying_queue* EncoderQueue() { static auto* queue = em_proxying_queue_create(); return queue; }
}
void ServiceGpuEncoderPool() {
    if (!emscripten_is_main_runtime_thread()) return;
    int on = encPrefillState.load(std::memory_order_relaxed);
    if (on < 0) {
        on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('encPrefill') === '0' ? 0 : 1; } catch (e) { return 1; } });
        encPrefillState.store(on, std::memory_order_release);
        std::fprintf(stderr, "CP2077 opt switches: encPrefill=%d\n", on);
    }
    if (on != 1) return;
    if (auto* queue = EncoderQueue()) emscripten_proxy_execute_queue(queue); // waiting worker refills (no drain)
    if (encoderPoolSize.load(std::memory_order_relaxed) >= kEncoderPoolLow) return;
    auto device = MainThreadGpuDevice();
    if (!device) return;
    const std::uint64_t generation = GpuDeviceGeneration();
    std::vector<WGPUCommandEncoder> stale;
    unsigned want = 0;
    {
        std::lock_guard<std::mutex> lock(encoderPoolMutex);
        for (auto it = encoderPool.begin(); it != encoderPool.end();) {
            if (it->second != generation) { stale.push_back(it->first); it = encoderPool.erase(it); } else ++it;
        }
        want = encoderPool.size() < kEncoderPoolTarget ? kEncoderPoolTarget - unsigned(encoderPool.size()) : 0;
        encoderPoolSize.store(unsigned(encoderPool.size()), std::memory_order_relaxed);
    }
    for (auto encoder : stale) wgpuCommandEncoderRelease(encoder);
    WGPUCommandEncoder created[kEncoderPoolTarget];
    unsigned count = 0;
    WGPUCommandEncoderDescriptor descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    for (; count < want; ++count)
        if (!(created[count] = wgpuDeviceCreateCommandEncoder(device, &descriptor))) break;
    if (!count) return;
    {
        std::lock_guard<std::mutex> lock(encoderPoolMutex);
        for (unsigned i = 0; i < count; ++i) encoderPool.emplace_back(created[i], generation);
        encoderPoolSize.store(unsigned(encoderPool.size()), std::memory_order_relaxed);
    }
    const unsigned n = encoderPrefills.fetch_add(1) + 1;
    const unsigned total = encoderPrefilled.fetch_add(count) + count;
    if (n <= 4 || !(n & (n - 1)))
        std::fprintf(stderr, "CP2077 opt encoder prefill probe: prefills=%u encoders=%u lastCount=%u workerRefills=%u stale=%u\n",
            n, total, count, encoderWorkerRefills.load(), unsigned(stale.size()));
}
WGPUCommandEncoder CreateGpuCommandEncoder() {
    WGPUCommandEncoder encoder = nullptr;
    if (!emscripten_is_main_runtime_thread()) {
        if (!IsGpuDeviceReady()) return nullptr;
        const std::uint64_t generation = GpuDeviceGeneration();
        struct Refill {
            std::vector<WGPUCommandEncoder> stale;
            WGPUCommandEncoder created[kEncoderPoolBatch]{};
            unsigned want = 0, count = 0; std::uint64_t generation = 0;
        } refill;
        {
            std::lock_guard<std::mutex> lock(encoderPoolMutex);
            while (!encoderPool.empty()) {
                const auto item = encoderPool.back(); encoderPool.pop_back();
                if (item.second == generation) { encoder = item.first; break; }
                refill.stale.push_back(item.first);
            }
            encoderPoolSize.store(unsigned(encoderPool.size()), std::memory_order_relaxed);
        }
        if (encoder && refill.stale.empty()) return encoder;
        refill.want = encoder ? 0 : kEncoderPoolBatch;
        auto refillOnMain = [](void* argument) {
            auto& refill = *static_cast<Refill*>(argument);
            for (auto stale : refill.stale) wgpuCommandEncoderRelease(stale);
            auto device = MainThreadGpuDevice();
            if (!device || !refill.want) return;
            refill.generation = GpuDeviceGeneration();
            WGPUCommandEncoderDescriptor descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
            for (; refill.count < refill.want; ++refill.count)
                if (!(refill.created[refill.count] = wgpuDeviceCreateCommandEncoder(device, &descriptor))) break;
        };
        bool handed;
        if (encPrefillState.load(std::memory_order_acquire) == 1) {
            // [opt] no drain-first: creation/release of encoders is independent of earlier submissions.
            encoderWorkerRefills.fetch_add(1, std::memory_order_relaxed);
            auto* queue = EncoderQueue();
            handed = queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), refillOnMain, &refill);
        } else handed = OnMain(refillOnMain, &refill);
        if (!handed) return encoder; // stale ones leak only if the handoff itself failed
        if (encoder) return encoder;
        if (!refill.count) return nullptr;
        encoder = refill.created[0];
        if (refill.count > 1) {
            std::lock_guard<std::mutex> lock(encoderPoolMutex);
            for (unsigned i = 1; i < refill.count; ++i) encoderPool.emplace_back(refill.created[i], refill.generation);
            encoderPoolSize.store(unsigned(encoderPool.size()), std::memory_order_relaxed);
        }
        static std::atomic<unsigned> refills{0};
        const unsigned n = refills.fetch_add(1) + 1;
        if (n <= 4 || !(n & (n - 1)))
            std::fprintf(stderr, "CP2077 command encoder pool refill: refills=%u created=%u stale=%u\n",
                n, refill.count, unsigned(refill.stale.size()));
        return encoder;
    }
    if (!OnMain([](void* argument) {
        auto device = MainThreadGpuDevice();
        if (device) {
            WGPUCommandEncoderDescriptor descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
            *static_cast<WGPUCommandEncoder*>(argument) = wgpuDeviceCreateCommandEncoder(device, &descriptor);
        }
    }, &encoder)) return nullptr;
    return encoder;
}
bool ReleaseGpuCommandEncoder(WGPUCommandEncoder encoder) {
    if (!encoder) return true;
    AbandonGpuCommandValidation(encoder);
    return OnMain([](void* argument) { wgpuCommandEncoderRelease(static_cast<WGPUCommandEncoder>(argument)); }, encoder);
}
bool EncodeGpuDebugCommand(WGPUCommandEncoder encoder, GpuDebugCommand operation, const char* label) {
    if (!encoder || (operation != GpuDebugCommand::PopGroup && !label)) return false;
    struct Request { WGPUCommandEncoder encoder; GpuDebugCommand operation; const char* label; bool issued=false; }
        request{encoder, operation, label};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        WGPUStringView label{request.label, WGPU_STRLEN};
        switch (request.operation) {
        case GpuDebugCommand::PushGroup: wgpuCommandEncoderPushDebugGroup(request.encoder, label); break;
        case GpuDebugCommand::PopGroup: wgpuCommandEncoderPopDebugGroup(request.encoder); break;
        case GpuDebugCommand::Marker: wgpuCommandEncoderInsertDebugMarker(request.encoder, label); break;
        default: return;
        }
        request.issued = true;
    }, &request) && request.issued;
}
bool EncodeGpuBufferZero(WGPUCommandEncoder encoder, WGPUBuffer buffer, std::uint64_t bytes) {
    if (!encoder || !buffer || !bytes || bytes % 4) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUBuffer buffer; std::uint64_t bytes; bool issued=false; }
        request{encoder,buffer,bytes};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        wgpuCommandEncoderClearBuffer(request.encoder, request.buffer, 0, request.bytes);
        request.issued = true;
    }, &request) && request.issued;
}
bool EncodeGpuBufferCopy(WGPUCommandEncoder encoder, WGPUBuffer source,
                         std::uint64_t sourceOffset, WGPUBuffer destination,
                         std::uint64_t destinationOffset, std::uint64_t size) {
    if (!encoder || !source || !destination || source == destination ||
        (sourceOffset | destinationOffset | size) % 4) return false;
    if (!size) return true;
    struct Copy {
        WGPUCommandEncoder encoder; WGPUBuffer source; WGPUBuffer destination;
        std::uint64_t sourceOffset; std::uint64_t destinationOffset; std::uint64_t size;
        bool encoded = false;
    } copy{encoder, source, destination, sourceOffset, destinationOffset, size};
    return OnMain([](void* argument) {
        auto& copy = *static_cast<Copy*>(argument);
        if (!MainThreadGpuDevice()) return;
        wgpuCommandEncoderCopyBufferToBuffer(copy.encoder, copy.source, copy.sourceOffset,
                                             copy.destination, copy.destinationOffset, copy.size);
        copy.encoded = true; // Validation errors are reported by the device callback.
    }, &copy) && copy.encoded;
}
bool EncodeGpuBufferTransfers(WGPUCommandEncoder encoder, const GpuBufferTransfer* transfers, std::size_t count) {
    if (!encoder || (count && !transfers)) return false;
    for (std::size_t i=0;i<count;++i) {
        const auto& c=transfers[i];
        if (!c.source || !c.destination || c.source==c.destination ||
            (c.sourceOffset|c.destinationOffset|c.bytes)%4 ||
            c.sourceOffset>UINT64_MAX-c.bytes || c.destinationOffset>UINT64_MAX-c.bytes) return false;
    }
    if (!count) return true;
    struct Request {WGPUCommandEncoder encoder; const GpuBufferTransfer* transfers; std::size_t count; bool issued=false;} request{encoder,transfers,count};
    return OnMain([](void* argument){
        auto& request=*static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        for (std::size_t i=0;i<request.count;++i) {
            const auto& c=request.transfers[i];
            if (c.bytes) wgpuCommandEncoderCopyBufferToBuffer(request.encoder,c.source,c.sourceOffset,c.destination,c.destinationOffset,c.bytes);
        }
        request.issued=true;
    },&request)&&request.issued;
}
static bool EncodeGraphicsDrawOnMain(WGPUCommandEncoder encoder, const GraphicsDraw& draw);
static bool PrepareGraphicsDrawOnMain(const GraphicsDraw& draw);
static WGPURenderPassEncoder BeginGraphicsDrawPass(WGPUCommandEncoder encoder, const GraphicsDraw& draw);
static void RecordGraphicsDrawInPass(WGPURenderPassEncoder pass, const GraphicsDraw& draw);
static void FlushPassReplay(WGPURenderPassEncoder pass); // [skperf] round 9 passBatch
static void FlushPassReplayTo(WGPURenderPassEncoder token, WGPURenderPassEncoder real); // [skperf] round 13
static bool ReplayBatchActive(); // [skperf] round 13: passDedup && passReplay && passBatch
extern bool replayDeferredPass; // [skperf] round 13: no early 64k flush while the pass is only a token
static void ResetPassStateForLazyPass(); // [skperf] round 13
// [skperf] round 9: [0] draws replayed, [1] JS replay calls (crossings), [2] state calls issued, [3] state calls elided.
std::atomic<unsigned long long> skperfReplayStats[4];
// [skperf] round 12 (perfprobe only). skperfPassStats: pass closes by reason [0] custom op, [1] debug op, [2] buffer
// copy, [3] non-mergeable draw, [4] different targets, [5] end of list; [6] pass begin us, [7] pass end us (incl.
// replay flush), [8] replay flush us, [9] debug ops elided, [10] index binds folded into firstIndex.
std::atomic<unsigned long long> skperfPassStats[11];
// skperfIssued: state calls issued by type [0] pipeline [1] viewport [2] scissor [3] blend [4] stencil [5] vertex
// [6] bind group [7] index [8] draws.
std::atomic<unsigned long long> skperfIssued[9];
// [skperf] round 12 dbgElide (default ON, ?dbgElide=0 old): engine debug markers/groups are not encoded. They only
// label captures, but each one closed the open render pass (an encoder-level debug call is illegal inside a pass),
// splitting runs of draws into extra passes. Record-time debug-group depth checks are unchanged.
// [skperf] round 13 copyHoist (default ON, ?copyHoist=0 old). A buffer copy inside a run of draws used to end the
// open render pass (copies are encoder-level). Now the pass is opened lazily: its draws are replayed into it when it
// closes, so a copy that arrives while it is pending can be encoded on the encoder BEFORE the pass begins, i.e.
// before every draw of that pass, when that is equivalent: the pass holds only mergeable (non-storage-writing)
// draws, every draw's read set is known, and the copy's destination range overlaps no buffer range those earlier
// draws read (vertex, index, indirect and bind-group buffers). Later draws see the new bytes either way; copies keep
// their relative order. Otherwise the pass is closed first exactly as before. Needs the replay path
// (passDedup + passReplay + passBatch); else off.
bool CopyHoistOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('copyHoist') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: copyHoist=%d\n", v); return v; }();
    return on != 0;
}
std::atomic<unsigned long long> skperfHoistStats[3]; // [0] copies hoisted, [1] copies that closed the pass (conflict), [2] opaque passes
// [skperf] round 15 drawTrim (default ON, ?drawTrim=0 old): per-draw main-thread trims: the IndirectFirstInstance
// feature query is cached per device generation (it was a JS call for every indirect draw) and the replay word buffer
// is reserved once. beginArr (default ON, ?beginArr=0 old): pass descriptors use a fixed stack array of colour
// attachments instead of a heap vector per pass. skperfMainDrawUs: [0] us spent in RecordGraphicsDrawInPass,
// [1] draws timed (perfprobe only).
std::atomic<unsigned long long> skperfMainDrawUs[2];
bool DrawTrimOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('drawTrim') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: drawTrim=%d\n", v); return v; }();
    return on != 0;
}
bool BeginArrOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('beginArr') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: beginArr=%d\n", v); return v; }();
    return on != 0;
}
bool DbgElideOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('dbgElide') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: dbgElide=%d\n", v); return v; }();
    return on != 0;
}
static bool SameGraphicsDrawTargets(const GraphicsDraw& a, const GraphicsDraw& b);
// [skperf] round 5 frame probe (?perfprobe=1; set by gpu-api-command-web): main-thread split of the ordered encode.
// [0] bind groups created on main, [1] their us, [2] PrepareGraphicsDrawOnMain us, [3] render passes begun,
// [4] draws encoded, [5] custom ops, [6] custom op us, [7] whole ordered-encode us.
std::atomic<int> skperfEncodeProbe{0};
std::atomic<unsigned long long> skperfEncodeStats[8];
bool EncodeGpuOrderedCommands(WGPUCommandEncoder encoder,const GpuOrderedCommand* operations,std::size_t count) {
    if(!encoder || (count && !operations))return false;
    for(std::size_t i=0;i<count;++i){
        const auto& op=operations[i];
        if(op.draw||op.custom)continue; // validated when queued
        if(op.isDebug){
            if(op.debug!=GpuDebugCommand::PushGroup && op.debug!=GpuDebugCommand::PopGroup && op.debug!=GpuDebugCommand::Marker)return false;
        }else{
            const auto& c=op.copy;
            if(!c.source || !c.destination || c.source==c.destination ||
                (c.sourceOffset|c.destinationOffset|c.bytes)%4 ||
                c.sourceOffset>UINT64_MAX-c.bytes || c.destinationOffset>UINT64_MAX-c.bytes)return false;
        }
    }
    if(!count)return true;
    struct Request {WGPUCommandEncoder encoder;const GpuOrderedCommand* operations;std::size_t count;bool issued=false;} request{encoder,operations,count};
    return OnMain([](void* argument){
        auto& request=*static_cast<Request*>(argument);
        if(!MainThreadGpuDevice())return;
        const bool timed=skperfEncodeProbe.load(std::memory_order_relaxed)!=0; // [skperf]
        struct WholeTimer{bool on;double start;~WholeTimer(){if(on)skperfEncodeStats[7].fetch_add((unsigned long long)((emscripten_get_now()-start)*1000.0),std::memory_order_relaxed);}} wholeTimer{timed,timed?emscripten_get_now():0};
        auto addUs=[](unsigned index,double since){skperfEncodeStats[index].fetch_add((unsigned long long)((emscripten_get_now()-since)*1000.0),std::memory_order_relaxed);};
        auto addUs10=[](unsigned index,double since){skperfPassStats[index].fetch_add((unsigned long long)((emscripten_get_now()-since)*1000.0),std::memory_order_relaxed);};
        ServiceGpuEncoderPool(); // [opt] keep the worker encoder pool topped up between the lists of a deferred submit
        WGPURenderPassEncoder openPass=nullptr;const GraphicsDraw* openDraw=nullptr;
        // [skperf] round 9 passBatch: the pass's batched replay words are flushed before the pass ends.
        // [skperf] round 13 copyHoist: with hoist on, openPass is a token for a lazily opened pass (never passed to
        // WebGPU); the real pass is begun from openDraw, replayed and ended in closePass.
        const bool hoist=CopyHoistOn() && ReplayBatchActive();
        bool lazyPass=false; static std::uintptr_t tokenSeq=0;
        constexpr unsigned kPassRanges=512;
        static GpuQueuedDraw::ReadRange passRanges[kPassRanges]; // main thread only
        unsigned passRangeCount=0; bool passOpaque=false;
        auto addRange=[&](WGPUBuffer buffer,std::uint64_t offset,std::uint64_t bytes){
            if(!buffer)return;
            if(passRangeCount==kPassRanges){passOpaque=true;return;}
            const std::uint64_t end=(bytes==0||bytes==WGPU_WHOLE_SIZE||offset>UINT64_MAX-bytes)?UINT64_MAX:offset+bytes;
            passRanges[passRangeCount++]={buffer,offset,end};
        };
        auto noteDrawReads=[&](const GpuQueuedDraw& queued){
            if(queued.readOverflow||!queued.draw.mergeablePass){passOpaque=true;return;}
            for(unsigned k=0;k<queued.readRangeCount;++k)
                {if(passRangeCount==kPassRanges){passOpaque=true;return;} passRanges[passRangeCount++]=queued.readRanges[k];}
            const auto& d=queued.draw;
            for(std::size_t k=0;k<d.vertexCount;++k)if(!d.vertices[k].unused)addRange(d.vertices[k].buffer,d.vertices[k].offset,d.vertices[k].bytes);
            if(d.indexed)addRange(d.indices.buffer,d.indices.offset,d.indices.bytes);
            if(d.indirectCount)addRange(d.indirectArguments.buffer,d.indirectArguments.offset,d.indirectArguments.bytes);
        };
        auto copyConflicts=[&](const GpuBufferTransfer& c){
            if(passOpaque)return true;
            const std::uint64_t begin=c.destinationOffset,end=c.destinationOffset+c.bytes;
            for(unsigned k=0;k<passRangeCount;++k){const auto& r=passRanges[k];if(r.buffer==c.destination&&begin<r.end&&r.begin<end)return true;}
            return false;
        };
        auto closePass=[&]{if(openPass){
                const double endStarted=timed?emscripten_get_now():0;
                WGPURenderPassEncoder real=openPass;
                if(lazyPass){
                    real=openDraw?BeginGraphicsDrawPass(request.encoder,*openDraw):nullptr;
                    if(timed)addUs10(6,endStarted);
                    FlushPassReplayTo(openPass,real);
                    replayDeferredPass=false;lazyPass=false;
                } else FlushPassReplay(openPass);
                if(timed)addUs10(8,endStarted);
                if(real){wgpuRenderPassEncoderEnd(real);wgpuRenderPassEncoderRelease(real);}
                openPass=nullptr;openDraw=nullptr;passRangeCount=0;passOpaque=false;
                if(timed)addUs10(7,endStarted);
            }};
        // [skperf] round 12: count why a pass closes (perfprobe only).
        auto closeFor=[&](unsigned reason){ if(openPass && timed) skperfPassStats[reason].fetch_add(1,std::memory_order_relaxed); closePass(); };
        const bool elideDebug=DbgElideOn();
        for(std::size_t i=0;i<request.count;++i){
            const auto& op=request.operations[i];
            if((i&127)==127){ServiceGpuAllocationRequests();ServiceGpuEncoderPool();} // [nomadperf] long lists: let waiting workers' buffer creations through ([opt] + encoder refills)
            if(!op.draw && !op.custom && op.isDebug && elideDebug){ if(timed)skperfPassStats[9].fetch_add(1,std::memory_order_relaxed); continue; } // [skperf] round 12 dbgElide
            if(!op.draw && !op.custom && !op.isDebug && !op.copy.bytes)continue; // [skperf] round 13: a zero-byte copy encodes nothing
            if(!op.draw && !op.custom && !op.isDebug && openPass && lazyPass && op.copy.bytes){
                // [skperf] round 13 copyHoist: encode before the pending pass when no earlier draw of it reads the range.
                if(!copyConflicts(op.copy)){
                    const auto& c=op.copy;
                    wgpuCommandEncoderCopyBufferToBuffer(request.encoder,c.source,c.sourceOffset,c.destination,c.destinationOffset,c.bytes);
                    if(timed)skperfHoistStats[0].fetch_add(1,std::memory_order_relaxed);
                    continue;
                }
                if(timed)skperfHoistStats[passOpaque?2:1].fetch_add(1,std::memory_order_relaxed);
            }
            if(!op.draw)closeFor(op.custom?0u:op.isDebug?1u:2u);
            if(op.custom){
                const double started=timed?emscripten_get_now():0;
                const bool ok=op.custom(request.encoder);
                if(timed){skperfEncodeStats[5].fetch_add(1,std::memory_order_relaxed);addUs(6,started);}
                if(!ok)return;continue;
            }
            if(op.draw){
                auto& queued=*op.draw;
                if(timed)skperfEncodeStats[4].fetch_add(1,std::memory_order_relaxed);
                if(queued.bindPipeline){
                    const double started=timed?emscripten_get_now():0;
                    queued.createdGroup=CreateGpuGraphicsBindGroupOnMain(queued.bindPipeline,queued.bindEntries.data(),queued.bindEntries.size(),queued.bindOwner);
                    if(timed){skperfEncodeStats[0].fetch_add(1,std::memory_order_relaxed);addUs(1,started);}
                    if(queued.bindRefsHeld){
                        for(const auto& e:queued.bindEntries){if(e.buffer)wgpuBufferRelease(e.buffer);if(e.textureView)wgpuTextureViewRelease(e.textureView);if(e.sampler)wgpuSamplerRelease(e.sampler);}
                        queued.bindRefsHeld=false;
                    }
                    if(!queued.createdGroup||queued.groups.empty()){closePass();return;}
                    queued.groups[0].group=queued.createdGroup;queued.draw.groups=queued.groups.data();
                }
                {
                    const double started=timed?emscripten_get_now():0;
                    const bool prepared=PrepareGraphicsDrawOnMain(queued.draw);
                    if(timed)addUs(2,started);
                    if(!prepared){closePass();return;}
                }
                if(openPass&&!(queued.draw.mergeablePass&&openDraw->mergeablePass&&SameGraphicsDrawTargets(*openDraw,queued.draw)))
                    closeFor((queued.draw.mergeablePass&&openDraw->mergeablePass)?4u:3u);
                if(!openPass){
                    const double beginStarted=timed?emscripten_get_now():0;
                    if(hoist){
                        // Lazily opened pass: reset the per-pass state cache now (as BeginGraphicsDrawPass does), begin later.
                        ResetPassStateForLazyPass();
                        openPass=reinterpret_cast<WGPURenderPassEncoder>(std::uintptr_t(0x10)+((++tokenSeq)<<4));
                        lazyPass=true;replayDeferredPass=true;passRangeCount=0;passOpaque=false;
                    } else {
                        openPass=BeginGraphicsDrawPass(request.encoder,queued.draw);if(!openPass)return;
                    }
                    openDraw=&queued.draw;
                    if(timed){skperfEncodeStats[3].fetch_add(1,std::memory_order_relaxed);addUs10(6,beginStarted);}
                }
                {
                    const double recordStarted=timed?emscripten_get_now():0; // [skperf] round 15 per-draw main record timer
                    RecordGraphicsDrawInPass(openPass,queued.draw);
                    if(timed){skperfMainDrawUs[0].fetch_add((unsigned long long)((emscripten_get_now()-recordStarted)*1000.0),std::memory_order_relaxed);skperfMainDrawUs[1].fetch_add(1,std::memory_order_relaxed);}
                }
                if(lazyPass)noteDrawReads(queued);
                continue;
            }
            if(op.isDebug){
                WGPUStringView label{op.label.c_str(),op.label.size()};
                switch(op.debug){
                case GpuDebugCommand::PushGroup:wgpuCommandEncoderPushDebugGroup(request.encoder,label);break;
                case GpuDebugCommand::PopGroup:wgpuCommandEncoderPopDebugGroup(request.encoder);break;
                case GpuDebugCommand::Marker:wgpuCommandEncoderInsertDebugMarker(request.encoder,label);break;
                }
            }else{
                const auto& c=op.copy;
                if(c.bytes)wgpuCommandEncoderCopyBufferToBuffer(request.encoder,c.source,c.sourceOffset,c.destination,c.destinationOffset,c.bytes);
            }
        }
        closeFor(5);
        request.issued=true;
    },&request)&&request.issued;
}
bool EncodeGpuBufferCopyRegions(WGPUCommandEncoder encoder, WGPUBuffer source, WGPUBuffer destination,
    const GpuBufferCopyRegion* regions, std::size_t count) {
    if (!encoder || !source || !destination || source == destination || (count && !regions)) return false;
    for (std::size_t index=0; index<count; ++index) {
        const auto& region=regions[index];
        if ((region.sourceOffset | region.destinationOffset | region.bytes) % 4 ||
            region.sourceOffset > UINT64_MAX-region.bytes ||
            region.destinationOffset > UINT64_MAX-region.bytes) return false;
    }
    if (!count) return true;
    struct Request { WGPUCommandEncoder encoder; WGPUBuffer source, destination;
        const GpuBufferCopyRegion* regions; std::size_t count; bool issued=false;
    } request{encoder,source,destination,regions,count};
    return OnMain([](void* argument) {
        auto& request=*static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        for (std::size_t index=0; index<request.count; ++index) {
            const auto& region=request.regions[index];
            if (region.bytes) wgpuCommandEncoderCopyBufferToBuffer(request.encoder, request.source,
                region.sourceOffset, request.destination, region.destinationOffset, region.bytes);
        }
        request.issued=true; // Device validation remains authoritative.
    }, &request) && request.issued;
}
bool EncodeGpuTextureBufferCopy(WGPUCommandEncoder encoder, const WGPUTexelCopyTextureInfo& texture,
    const WGPUTexelCopyBufferInfo& buffer, const WGPUExtent3D& extent, bool upload) {
    if (!encoder || !texture.texture || !buffer.buffer || !extent.width || !extent.height || !extent.depthOrArrayLayers) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUTexelCopyTextureInfo texture;
        WGPUTexelCopyBufferInfo buffer; WGPUExtent3D extent; bool upload; bool issued = false; }
        request{encoder, texture, buffer, extent, upload};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        if (request.upload) wgpuCommandEncoderCopyBufferToTexture(request.encoder, &request.buffer, &request.texture, &request.extent);
        else wgpuCommandEncoderCopyTextureToBuffer(request.encoder, &request.texture, &request.buffer, &request.extent);
        request.issued = true;
    }, &request) && request.issued;
}
WGPUQuerySet CreateGpuTimestampQuerySet(std::uint32_t count) {
    if (!count || count > 4096) return nullptr;
    struct Request { std::uint32_t count; WGPUQuerySet result = nullptr; } request{count};
    if (!OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        auto device = MainThreadGpuDevice();
        if (!device || !wgpuDeviceHasFeature(device, WGPUFeatureName_TimestampQuery)) return;
        WGPUQuerySetDescriptor descriptor = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        descriptor.type = WGPUQueryType_Timestamp; descriptor.count = request.count;
        request.result = wgpuDeviceCreateQuerySet(device, &descriptor);
    }, &request)) return nullptr;
    return request.result;
}
bool ReleaseGpuQuerySet(WGPUQuerySet queries) {
    if (!queries) return true;
    return OnMain([](void* argument) { auto queries = static_cast<WGPUQuerySet>(argument);
        wgpuQuerySetDestroy(queries); wgpuQuerySetRelease(queries); }, queries);
}
bool EncodeGpuTimestamp(WGPUCommandEncoder encoder, WGPUQuerySet queries, std::uint32_t index) {
    if (!encoder || !queries) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUQuerySet queries; std::uint32_t index; bool issued = false; }
        request{encoder,queries,index};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        // Portable WebGPU supports pass timestamp writes. Encoder.writeTimestamp
        // is a Dawn extension and is not available in standard browser engines.
        WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
        writes.querySet = request.queries; writes.beginningOfPassWriteIndex = request.index;
        WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
        descriptor.timestampWrites = &writes;
        auto pass = wgpuCommandEncoderBeginComputePass(request.encoder, &descriptor);
        if (!pass) return;
        wgpuComputePassEncoderEnd(pass); wgpuComputePassEncoderRelease(pass);
        request.issued = true;
    }, &request) && request.issued;
}
bool EncodeGpuQueryResolve(WGPUCommandEncoder encoder, WGPUQuerySet queries, std::uint32_t start,
    std::uint32_t count, WGPUBuffer resolve, WGPUBuffer readback) {
    if (!encoder || !queries || !resolve || !readback || !count) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUQuerySet queries; std::uint32_t start,count;
        WGPUBuffer resolve,readback; bool issued=false; } request{encoder,queries,start,count,resolve,readback};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        wgpuCommandEncoderResolveQuerySet(request.encoder, request.queries, request.start, request.count, request.resolve, 0);
        wgpuCommandEncoderCopyBufferToBuffer(request.encoder, request.resolve, 0, request.readback,
            std::uint64_t(request.start) * 8, std::uint64_t(request.count) * 8);
        request.issued=true;
    }, &request) && request.issued;
}
bool SubmitGpuCommandEncoders(WGPUCommandEncoder* encoders, std::size_t count,
                             GpuSubmissionComplete complete, void* userdata) {
    if (!count) { if (complete) complete(true, userdata); return true; }
    if (!encoders) return false;
    // Measurement only: time the validation drain (W_SCOPE, target = encoder count) and the mapped-buffer
    // flush (W_OTHER, target 2) so traces separate them from the main-thread submit proxy.
    const bool cpPhaseTrace = cpEngineTrace::Enabled();
    const double cpDrainStart = cpPhaseTrace ? wgtrace::now() : 0;
    // An encoder whose async validation failed (e.g. one invalid compute bind group) is dropped on its own:
    // WebGPU rejects a whole queue.submit when any member is invalid, so returning false here used to make the
    // caller discard every list of the batch (character preview G-buffer, lighting, UI). Healthy members are
    // compacted to the front and still submitted. Dropped handles are released and nulled (caller contract:
    // a handle left non-null still belongs to the caller).
    std::size_t kept=0;
    for(std::size_t i=0;i<count;++i){
        auto encoder=encoders[i];
        if(encoder&&!DrainGpuCommandValidation(encoder)){
            static std::atomic<unsigned> dropped{0};
            const unsigned n=dropped.fetch_add(1)+1;
            if(n<=16||(n&(n-1))==0)
                std::fprintf(stderr,"CP2077 submit encoder dropped (validation): index=%zu count=%zu total=%u\n",i,count,n);
            ReleaseGpuCommandEncoder(encoder);
            encoders[i]=nullptr;
            continue;
        }
        encoders[kept++]=encoder;
    }
    for(std::size_t i=kept;i<count;++i)encoders[i]=nullptr;
    if(!kept)return false;
    count=kept;
    const double cpFlushStart = cpPhaseTrace ? wgtrace::now() : 0;
    if (cpPhaseTrace) cpEngineTrace::Emit(wgtrace::WAIT, cpEngineTrace::FrameId(), wgtrace::tl_job, wgtrace::W_SCOPE, unsigned(count), cpDrainStart, cpFlushStart);
    if (!FlushMappedGpuBuffers()) return false;
    if (cpPhaseTrace) cpEngineTrace::Emit(wgtrace::WAIT, cpEngineTrace::FrameId(), wgtrace::tl_job, wgtrace::W_OTHER, 2, cpFlushStart, wgtrace::now());
    struct Submission { WGPUCommandEncoder* encoders; std::size_t count;
        GpuSubmissionComplete complete; void* userdata; bool submitted = false;
    } request{encoders, count, complete, userdata};
    if (!OnMain([](void* argument) {
        auto& request = *static_cast<Submission*>(argument);
        auto queue = MainThreadGpuQueue();
        std::vector<WGPUCommandBuffer> commands;
        commands.reserve(request.count);
        bool valid = queue != nullptr;
        for (std::size_t index = 0; index < request.count; ++index) {
            auto encoder = request.encoders[index];
            if (!encoder) { valid = false; continue; }
            WGPUCommandBufferDescriptor descriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
            auto command = wgpuCommandEncoderFinish(encoder, &descriptor);
            wgpuCommandEncoderRelease(encoder);
            request.encoders[index] = nullptr;
            if (command) commands.push_back(command); else valid = false;
        }
        if (valid) {
            wgpuQueueSubmit(queue, commands.size(), commands.data());
            request.submitted = true; // Submitted, not GPU-completed or runtime-validated.
            if (request.complete) {
                struct Completion { GpuSubmissionComplete callback; void* userdata; WGPUDevice device; };
                auto* completion = new Completion{request.complete, request.userdata, MainThreadGpuDevice()};
                wgpuDeviceAddRef(completion->device);
                WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
                info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = completion;
                info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*) {
                    auto* completion = static_cast<Completion*>(userdata1);
                    const bool ok = status == WGPUQueueWorkDoneStatus_Success &&
                        completion->device == MainThreadGpuDevice() && IsGpuDeviceReady();
                    completion->callback(ok, completion->userdata);
                    wgpuDeviceRelease(completion->device); delete completion;
                };
                wgpuQueueOnSubmittedWorkDone(queue, info);
            }
        }
        for (auto command : commands) wgpuCommandBufferRelease(command);
    }, &request)) return false;
    return request.submitted;
}
// perf12: main thread only (deferred submission job). Finishes every non-null encoder in array order and releases
// it. Consecutive members with isolated[i] == 0 go to the queue in one submit; a member with isolated[i] != 0
// (validation scopes still pending) gets its own submit, so if it turns out invalid the browser rejects it alone
// instead of the whole batch. Submit order equals array order. One completion callback is registered after the
// last submit (onSubmittedWorkDone covers all earlier submits). Returns false, with userdata still owned by the
// caller, when nothing could be submitted.
bool SubmitGpuCommandEncodersOnMain(WGPUCommandEncoder* encoders, const unsigned char* isolated, std::size_t count,
                                    GpuSubmissionComplete complete, void* userdata) {
    if (!emscripten_is_main_runtime_thread() || (count && !encoders)) return false;
    auto queue = MainThreadGpuQueue();
    auto device = MainThreadGpuDevice();
    std::vector<WGPUCommandBuffer> run;
    run.reserve(count);
    std::size_t submitted = 0;
    auto submitRun = [&] {
        if (run.empty()) return;
        wgpuQueueSubmit(queue, run.size(), run.data());
        submitted += run.size();
        for (auto command : run) wgpuCommandBufferRelease(command);
        run.clear();
    };
    for (std::size_t index = 0; index < count; ++index) {
        auto encoder = encoders[index];
        if (!encoder) continue;
        encoders[index] = nullptr;
        if (!queue || !device) { wgpuCommandEncoderRelease(encoder); continue; }
        WGPUCommandBufferDescriptor descriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
        auto command = wgpuCommandEncoderFinish(encoder, &descriptor);
        wgpuCommandEncoderRelease(encoder);
        if (!command) continue;
        if (isolated && isolated[index]) { submitRun(); run.push_back(command); submitRun(); }
        else run.push_back(command);
    }
    submitRun();
    if (!submitted) return false;
    if (complete) {
        struct Completion { GpuSubmissionComplete callback; void* userdata; WGPUDevice device; };
        auto* completion = new Completion{complete, userdata, device};
        wgpuDeviceAddRef(completion->device);
        WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = completion;
        info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata1, void*) {
            auto* completion = static_cast<Completion*>(userdata1);
            const bool ok = status == WGPUQueueWorkDoneStatus_Success &&
                completion->device == MainThreadGpuDevice() && IsGpuDeviceReady();
            completion->callback(ok, completion->userdata);
            wgpuDeviceRelease(completion->device); delete completion;
        };
        wgpuQueueOnSubmittedWorkDone(queue, info);
    }
    return true;
}
bool WaitGpuQueueIdle() {
    if (emscripten_is_main_runtime_thread()) return false;
    struct Wait { em_proxying_ctx* context = nullptr; WGPUDevice device = nullptr; bool completed = false; } wait;
    auto* queue = CommandQueue();
    if (!queue) return false;
    const bool dispatched = emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* wait = static_cast<Wait*>(argument); wait->context = context;
            DrainDeferredGpuWork(); // perf12: idle means after every asynchronous submission issued so far
            auto queue = MainThreadGpuQueue(); wait->device = MainThreadGpuDevice();
            if (!queue || !wait->device || !IsGpuDeviceReady()) { emscripten_proxy_finish(context); return; }
            wgpuDeviceAddRef(wait->device);
            WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
            info.mode = WGPUCallbackMode_AllowSpontaneous; info.userdata1 = wait;
            info.callback = [](WGPUQueueWorkDoneStatus status, WGPUStringView, void* userdata, void*) {
                auto* wait = static_cast<Wait*>(userdata);
                wait->completed = status == WGPUQueueWorkDoneStatus_Success &&
                    wait->device == MainThreadGpuDevice() && IsGpuDeviceReady();
                wgpuDeviceRelease(wait->device); emscripten_proxy_finish(wait->context);
            };
            wgpuQueueOnSubmittedWorkDone(queue, info);
        }, &wait);
    return dispatched && wait.completed;
}
// Main-thread cache of the helper pipelines used by rect clears and depth-float copies. Each call used to compile a
// shader module and a render pipeline on the (saturated) main thread. Keyed by every value the descriptor depends
// on; dropped when the device or its generation changes. Only the main thread touches it, so no lock. Beyond the
// cap a pipeline is created and released per call, as before.
namespace {
struct HelperPipelineCache {
    WGPUDevice device = nullptr; std::uint64_t generation = 0;
    std::map<std::array<std::uint32_t, 8>, WGPURenderPipeline> pipelines;
};
HelperPipelineCache& HelperPipelines() { static HelperPipelineCache cache; return cache; }
// Returns a pipeline for key, creating it with create() on a miss. owned=true: not cached, the caller releases it.
template <class Create>
WGPURenderPipeline AcquireHelperPipeline(WGPUDevice device, const std::array<std::uint32_t, 8>& key, Create&& create, bool& owned) {
    owned = false;
    auto& cache = HelperPipelines();
    const std::uint64_t generation = GpuDeviceGeneration();
    if (cache.device != device || cache.generation != generation) {
        for (auto& item : cache.pipelines) wgpuRenderPipelineRelease(item.second);
        cache.pipelines.clear(); cache.device = device; cache.generation = generation;
    }
    auto found = cache.pipelines.find(key);
    if (found != cache.pipelines.end()) return found->second;
    WGPURenderPipeline pipeline = create();
    if (!pipeline) return nullptr;
    if (cache.pipelines.size() < 256) cache.pipelines.emplace(key, pipeline);
    else owned = true;
    return pipeline;
}
}
bool EncodeGpuColorClear(WGPUCommandEncoder encoder, WGPUTextureView view, const WGPUColor& color) {
    if (!encoder || !view) return false;
    struct Clear { WGPUCommandEncoder encoder; WGPUTextureView view; WGPUColor color; bool encoded = false; } clear{encoder, view, color};
    return OnMain([](void* argument) {
        auto& clear = *static_cast<Clear*>(argument);
        if (!MainThreadGpuDevice()) return;
        WGPURenderPassColorAttachment target = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        target.view = clear.view; target.loadOp = WGPULoadOp_Clear;
        target.storeOp = WGPUStoreOp_Store; target.clearValue = clear.color;
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &target;
        auto pass = wgpuCommandEncoderBeginRenderPass(clear.encoder, &descriptor);
        if (!pass) return;
        wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
        clear.encoded = true; // Issued; device validation and execution are separate.
    }, &clear) && clear.encoded;
}
bool EncodeGpuColorRectClear(WGPUCommandEncoder encoder, WGPUTextureView view, WGPUTextureFormat format,
    std::uint32_t width, std::uint32_t height, std::uint32_t samples,
    std::uint32_t x, std::uint32_t y, std::uint32_t rectWidth, std::uint32_t rectHeight, const WGPUColor& color) {
    if(!encoder || !view || !width || !height || !samples || !rectWidth || !rectHeight ||
       x>width || y>height || rectWidth>width-x || rectHeight>height-y) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUTextureView view; WGPUTextureFormat format;
        std::uint32_t width,height,samples,x,y,rw,rh; WGPUColor color; bool encoded=false;
    } request{encoder,view,format,width,height,samples,x,y,rectWidth,rectHeight,color};
    return OnMain([](void* argument) {
        auto& r=*static_cast<Request*>(argument); auto device=MainThreadGpuDevice(); if(!device)return;
        float values[4]={float(r.color.r),float(r.color.g),float(r.color.b),float(r.color.a)};
        std::uint32_t bits[4]; std::memcpy(bits,values,sizeof(bits));
        bool owned=false;
        auto pipeline=AcquireHelperPipeline(device,{1u,std::uint32_t(r.format),r.samples,0u,bits[0],bits[1],bits[2],bits[3]},[&]()->WGPURenderPipeline{
            char code[1024];
            std::snprintf(code,sizeof(code),
                "@vertex fn clear_vertex(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {"
                "let p=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));return vec4f(p[i],0,1); }"
                "@fragment fn clear_color()->@location(0) vec4f {return vec4f(bitcast<f32>(%uu),"
                "bitcast<f32>(%uu),bitcast<f32>(%uu),bitcast<f32>(%uu));}",bits[0],bits[1],bits[2],bits[3]);
            WGPUShaderSourceWGSL wgsl=WGPU_SHADER_SOURCE_WGSL_INIT;wgsl.code={code,WGPU_STRLEN};
            WGPUShaderModuleDescriptor moduleDesc=WGPU_SHADER_MODULE_DESCRIPTOR_INIT;moduleDesc.nextInChain=&wgsl.chain;
            auto module=wgpuDeviceCreateShaderModule(device,&moduleDesc);if(!module)return nullptr;
            WGPUColorTargetState colorTarget=WGPU_COLOR_TARGET_STATE_INIT;
            colorTarget.format=r.format;colorTarget.writeMask=WGPUColorWriteMask_All;
            WGPUFragmentState fragment=WGPU_FRAGMENT_STATE_INIT;
            fragment.module=module;fragment.entryPoint={"clear_color",WGPU_STRLEN};fragment.targetCount=1;fragment.targets=&colorTarget;
            WGPURenderPipelineDescriptor pipelineDesc=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pipelineDesc.vertex.module=module;pipelineDesc.vertex.entryPoint={"clear_vertex",WGPU_STRLEN};
            pipelineDesc.fragment=&fragment;pipelineDesc.multisample.count=r.samples;
            auto created=wgpuDeviceCreateRenderPipeline(device,&pipelineDesc);wgpuShaderModuleRelease(module);return created;
        },owned);
        if(!pipeline)return;
        WGPURenderPassColorAttachment target=WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        target.view=r.view;target.loadOp=WGPULoadOp_Load;target.storeOp=WGPUStoreOp_Store;
        WGPURenderPassDescriptor passDesc=WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        passDesc.colorAttachmentCount=1;passDesc.colorAttachments=&target;
        auto pass=wgpuCommandEncoderBeginRenderPass(r.encoder,&passDesc);
        if(pass){
            wgpuRenderPassEncoderSetPipeline(pass,pipeline);
            wgpuRenderPassEncoderSetViewport(pass,0,0,float(r.width),float(r.height),0,1);
            wgpuRenderPassEncoderSetScissorRect(pass,r.x,r.y,r.rw,r.rh);
            wgpuRenderPassEncoderDraw(pass,3,1,0,0);
            wgpuRenderPassEncoderEnd(pass);wgpuRenderPassEncoderRelease(pass);r.encoded=true;
        }
        if(owned)wgpuRenderPipelineRelease(pipeline);
    },&request)&&request.encoded;
}
bool EncodeGpuDepthClear(WGPUCommandEncoder encoder, WGPUTextureView view, float depth,
                        bool hasStencil, bool clearStencil, std::uint8_t stencil) {
    if (!encoder || !view) return false;
    struct Clear { WGPUCommandEncoder encoder; WGPUTextureView view; float depth;
        bool hasStencil, clearStencil; std::uint8_t stencil; bool encoded = false;
    } clear{encoder, view, depth, hasStencil, clearStencil, stencil};
    return OnMain([](void* argument) {
        auto& clear = *static_cast<Clear*>(argument);
        if (!MainThreadGpuDevice()) return;
        WGPURenderPassDepthStencilAttachment target = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        target.view = clear.view; target.depthLoadOp = WGPULoadOp_Clear;
        target.depthStoreOp = WGPUStoreOp_Store; target.depthClearValue = clear.depth;
        if (clear.hasStencil) {
            target.stencilLoadOp = clear.clearStencil ? WGPULoadOp_Clear : WGPULoadOp_Load;
            target.stencilStoreOp = WGPUStoreOp_Store; target.stencilClearValue = clear.stencil;
        }
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.depthStencilAttachment = &target;
        auto pass = wgpuCommandEncoderBeginRenderPass(clear.encoder, &descriptor);
        if (!pass) return;
        wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
        clear.encoded = true;
    }, &clear) && clear.encoded;
}
bool EncodeGpuDepthRectClear(WGPUCommandEncoder encoder, WGPUTextureView view, WGPUTextureFormat format,
    std::uint32_t width, std::uint32_t height, std::uint32_t samples,
    std::uint32_t x, std::uint32_t y, std::uint32_t rectWidth, std::uint32_t rectHeight,
    float depth, bool hasStencil, bool clearStencil, std::uint8_t stencil) {
    if (!encoder || !view || !width || !height || !samples || !(depth >= 0 && depth <= 1) ||
        !rectWidth || !rectHeight || x > width || y > height ||
        rectWidth > width-x || rectHeight > height-y) return false;
    struct Request {
        WGPUCommandEncoder encoder; WGPUTextureView view; WGPUTextureFormat format;
        std::uint32_t width,height,samples,x,y,rectWidth,rectHeight;
        float depth; bool hasStencil,clearStencil; std::uint8_t stencil; bool encoded=false;
    } request{encoder,view,format,width,height,samples,x,y,rectWidth,rectHeight,depth,hasStencil,clearStencil,stencil};
    return OnMain([](void* argument) {
        auto& r=*static_cast<Request*>(argument);
        auto device=MainThreadGpuDevice(); if (!device) return;
        static const char shader[]=R"WGSL(@vertex
fn clear_depth(@builtin(vertex_index) i:u32)->@builtin(position) vec4<f32> {
    let p=array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3));
    return vec4<f32>(p[i],0,1);
})WGSL";
        bool owned=false;
        auto pipeline=AcquireHelperPipeline(device,{2u,std::uint32_t(r.format),r.samples,std::uint32_t(r.clearStencil),0u,0u,0u,0u},[&]()->WGPURenderPipeline{
            WGPUShaderSourceWGSL source=WGPU_SHADER_SOURCE_WGSL_INIT;
            source.code={shader,sizeof(shader)-1};
            WGPUShaderModuleDescriptor moduleDesc=WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            moduleDesc.nextInChain=&source.chain;
            auto module=wgpuDeviceCreateShaderModule(device,&moduleDesc); if (!module) return nullptr;
            WGPUDepthStencilState state=WGPU_DEPTH_STENCIL_STATE_INIT;
            state.format=r.format; state.depthWriteEnabled=WGPUOptionalBool_True;
            state.depthCompare=WGPUCompareFunction_Always;
            // A depth-only clear must preserve every stencil bit.
            state.stencilReadMask=0xff; state.stencilWriteMask=r.clearStencil?0xff:0;
            state.stencilFront.compare=WGPUCompareFunction_Always;
            state.stencilBack.compare=WGPUCompareFunction_Always;
            state.stencilFront.passOp=state.stencilBack.passOp=r.clearStencil?WGPUStencilOperation_Replace:WGPUStencilOperation_Keep;
            WGPURenderPipelineDescriptor pipelineDesc=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pipelineDesc.vertex.module=module; pipelineDesc.vertex.entryPoint={"clear_depth",WGPU_STRLEN};
            pipelineDesc.depthStencil=&state; pipelineDesc.multisample.count=r.samples;
            auto created=wgpuDeviceCreateRenderPipeline(device,&pipelineDesc);
            wgpuShaderModuleRelease(module); return created;
        },owned);
        if (!pipeline) return;
        WGPURenderPassDepthStencilAttachment target=WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        target.view=r.view; target.depthLoadOp=WGPULoadOp_Load; target.depthStoreOp=WGPUStoreOp_Store;
        target.depthClearValue=0;
        if (r.hasStencil) {
            target.stencilLoadOp=WGPULoadOp_Load; target.stencilStoreOp=WGPUStoreOp_Store;
        }
        WGPURenderPassDescriptor descriptor=WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.depthStencilAttachment=&target;
        auto pass=wgpuCommandEncoderBeginRenderPass(r.encoder,&descriptor);
        if (pass) {
            wgpuRenderPassEncoderSetPipeline(pass,pipeline);
            // Equal viewport depth bounds map all triangle samples to the exact
            // native clear depth without a uniform allocation or shader rewrite.
            wgpuRenderPassEncoderSetViewport(pass,0,0,float(r.width),float(r.height),r.depth,r.depth);
            wgpuRenderPassEncoderSetScissorRect(pass,r.x,r.y,r.rectWidth,r.rectHeight);
            if (r.clearStencil) wgpuRenderPassEncoderSetStencilReference(pass,r.stencil);
            wgpuRenderPassEncoderDraw(pass,3,1,0,0);
            wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass); r.encoded=true;
        }
        if (owned) wgpuRenderPipelineRelease(pipeline);
    },&request) && request.encoded;
}
bool EncodeGpuDepthFloatCopy(WGPUCommandEncoder encoder, WGPUTextureView source,
    WGPUTextureView destination, std::uint32_t width, std::uint32_t height) {
    if (!encoder || !source || !destination || source==destination || !width || !height) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUTextureView source,destination;
        std::uint32_t width,height; bool issued=false; } request{encoder,source,destination,width,height};
    return OnMain([](void* argument) {
        auto& r=*static_cast<Request*>(argument);
        auto device=MainThreadGpuDevice(); if (!device) return;
        static const char code[]=R"CPDEPTHCOPY(
@group(0) @binding(0) var sourceDepth: texture_depth_2d;
@vertex fn copyVertex(@builtin(vertex_index) index:u32)->@builtin(position) vec4<f32> {
    let triangle=array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3));
    return vec4<f32>(triangle[index],0,1);
}
@fragment fn copyDepth(@builtin(position) pixel:vec4<f32>)->@location(0) f32 {
    return textureLoad(sourceDepth,vec2<i32>(pixel.xy),0);
})CPDEPTHCOPY";
        bool owned=false;
        auto pipeline=AcquireHelperPipeline(device,{3u,0u,0u,0u,0u,0u,0u,0u},[&]()->WGPURenderPipeline{
            WGPUShaderSourceWGSL wgsl=WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code={code,sizeof(code)-1};
            WGPUShaderModuleDescriptor shaderDesc=WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            shaderDesc.nextInChain=&wgsl.chain;
            auto module=wgpuDeviceCreateShaderModule(device,&shaderDesc); if (!module) return nullptr;
            WGPUColorTargetState color=WGPU_COLOR_TARGET_STATE_INIT; color.format=WGPUTextureFormat_R32Float;
            WGPUFragmentState fragment=WGPU_FRAGMENT_STATE_INIT;
            fragment.module=module;fragment.entryPoint={"copyDepth",WGPU_STRLEN};
            fragment.targetCount=1;fragment.targets=&color;
            WGPURenderPipelineDescriptor pipelineDesc=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pipelineDesc.vertex.module=module;pipelineDesc.vertex.entryPoint={"copyVertex",WGPU_STRLEN};
            pipelineDesc.fragment=&fragment;
            auto created=wgpuDeviceCreateRenderPipeline(device,&pipelineDesc);
            wgpuShaderModuleRelease(module); return created;
        },owned);
        if (!pipeline) return;
        auto layout=wgpuRenderPipelineGetBindGroupLayout(pipeline,0);
        if (!layout) {if (owned) wgpuRenderPipelineRelease(pipeline);return;}
        WGPUBindGroupEntry entry=WGPU_BIND_GROUP_ENTRY_INIT;entry.binding=0;entry.textureView=r.source;
        WGPUBindGroupDescriptor groupDesc=WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        groupDesc.layout=layout;groupDesc.entryCount=1;groupDesc.entries=&entry;
        auto group=wgpuDeviceCreateBindGroup(device,&groupDesc);
        wgpuBindGroupLayoutRelease(layout);
        if (!group) {if (owned) wgpuRenderPipelineRelease(pipeline);return;}
        WGPURenderPassColorAttachment attachment=WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        attachment.view=r.destination;attachment.loadOp=WGPULoadOp_Clear;attachment.storeOp=WGPUStoreOp_Store;
        attachment.clearValue={0,0,0,0};
        WGPURenderPassDescriptor passDesc=WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        passDesc.colorAttachmentCount=1;passDesc.colorAttachments=&attachment;
        auto pass=wgpuCommandEncoderBeginRenderPass(r.encoder,&passDesc);
        if (pass) {
            wgpuRenderPassEncoderSetPipeline(pass,pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass,0,group,0,nullptr);
            wgpuRenderPassEncoderSetViewport(pass,0,0,float(r.width),float(r.height),0,1);
            wgpuRenderPassEncoderSetScissorRect(pass,0,0,r.width,r.height);
            wgpuRenderPassEncoderDraw(pass,3,1,0,0);
            wgpuRenderPassEncoderEnd(pass);wgpuRenderPassEncoderRelease(pass);r.issued=true;
        }
        wgpuBindGroupRelease(group);if (owned) wgpuRenderPipelineRelease(pipeline);
    },&request) && request.issued;
}
bool EncodeGpuR16ToDepth16Copy(WGPUCommandEncoder encoder, WGPUTextureView source,
    WGPUTextureView destination, std::uint32_t width, std::uint32_t height, WGPUTextureFormat target) {
    // [npcblack] R16Unorm (D16U UAV backing) -> Depth16Unorm (frag_depth, exact 16-bit codes) or a float colour
    // target (R32Float/R16Float, same normalized values).
    if (!encoder || !source || !destination || source==destination || !width || !height) return false;
    const bool depth=target==WGPUTextureFormat_Depth16Unorm;
    if (!depth && target!=WGPUTextureFormat_R32Float && target!=WGPUTextureFormat_R16Float) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUTextureView source,destination;
        std::uint32_t width,height; WGPUTextureFormat target; bool depth; bool issued=false; }
        request{encoder,source,destination,width,height,target,depth};
    return OnMain([](void* argument) {
        auto& r=*static_cast<Request*>(argument);
        auto device=MainThreadGpuDevice(); if (!device) return;
        static const char code[]=R"CPR16DEPTH(
@group(0) @binding(0) var sourceCodes: texture_2d<f32>;
@vertex fn copyVertex(@builtin(vertex_index) index:u32)->@builtin(position) vec4<f32> {
    let triangle=array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3));
    return vec4<f32>(triangle[index],0,1);
}
@fragment fn copyDepth(@builtin(position) pixel:vec4<f32>)->@builtin(frag_depth) f32 {
    return textureLoad(sourceCodes,vec2<i32>(pixel.xy),0).r;
}
@fragment fn copyColor(@builtin(position) pixel:vec4<f32>)->@location(0) vec4<f32> {
    return vec4<f32>(textureLoad(sourceCodes,vec2<i32>(pixel.xy),0).r,0,0,1);
})CPR16DEPTH";
        bool owned=false;
        auto pipeline=AcquireHelperPipeline(device,{9u,std::uint32_t(r.target),0u,0u,0u,0u,0u,0u},[&]()->WGPURenderPipeline{
            WGPUShaderSourceWGSL wgsl=WGPU_SHADER_SOURCE_WGSL_INIT;
            wgsl.code={code,sizeof(code)-1};
            WGPUShaderModuleDescriptor shaderDesc=WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
            shaderDesc.nextInChain=&wgsl.chain;
            auto module=wgpuDeviceCreateShaderModule(device,&shaderDesc); if (!module) return nullptr;
            WGPUColorTargetState color=WGPU_COLOR_TARGET_STATE_INIT; color.format=r.target;
            WGPUFragmentState fragment=WGPU_FRAGMENT_STATE_INIT;
            fragment.module=module;
            fragment.entryPoint={r.depth ? "copyDepth" : "copyColor",WGPU_STRLEN};
            fragment.targetCount=r.depth ? 0 : 1;fragment.targets=r.depth ? nullptr : &color;
            WGPUDepthStencilState state=WGPU_DEPTH_STENCIL_STATE_INIT;
            state.format=WGPUTextureFormat_Depth16Unorm;state.depthWriteEnabled=WGPUOptionalBool_True;
            state.depthCompare=WGPUCompareFunction_Always;
            // R16Unorm is UnfilterableFloat without texture-formats-tier1 filtering: explicit layout (auto = Float).
            WGPUBindGroupLayoutEntry bglEntry=WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
            bglEntry.binding=0;bglEntry.visibility=WGPUShaderStage_Fragment;
            bglEntry.texture.sampleType=WGPUTextureSampleType_UnfilterableFloat;
            bglEntry.texture.viewDimension=WGPUTextureViewDimension_2D;bglEntry.texture.multisampled=false;
            WGPUBindGroupLayoutDescriptor bglDesc=WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
            bglDesc.entryCount=1;bglDesc.entries=&bglEntry;
            auto bgl=wgpuDeviceCreateBindGroupLayout(device,&bglDesc);
            if (!bgl) {wgpuShaderModuleRelease(module);return nullptr;}
            WGPUPipelineLayoutDescriptor plDesc=WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
            plDesc.bindGroupLayoutCount=1;plDesc.bindGroupLayouts=&bgl;
            auto pipelineLayout=wgpuDeviceCreatePipelineLayout(device,&plDesc);
            wgpuBindGroupLayoutRelease(bgl);
            if (!pipelineLayout) {wgpuShaderModuleRelease(module);return nullptr;}
            WGPURenderPipelineDescriptor pipelineDesc=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pipelineDesc.layout=pipelineLayout;
            pipelineDesc.vertex.module=module;pipelineDesc.vertex.entryPoint={"copyVertex",WGPU_STRLEN};
            pipelineDesc.fragment=&fragment;pipelineDesc.depthStencil=r.depth ? &state : nullptr;
            auto created=wgpuDeviceCreateRenderPipeline(device,&pipelineDesc);
            wgpuPipelineLayoutRelease(pipelineLayout);
            wgpuShaderModuleRelease(module); return created;
        },owned);
        if (!pipeline) return;
        auto layout=wgpuRenderPipelineGetBindGroupLayout(pipeline,0);
        if (!layout) {if (owned) wgpuRenderPipelineRelease(pipeline);return;}
        WGPUBindGroupEntry entry=WGPU_BIND_GROUP_ENTRY_INIT;entry.binding=0;entry.textureView=r.source;
        WGPUBindGroupDescriptor groupDesc=WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        groupDesc.layout=layout;groupDesc.entryCount=1;groupDesc.entries=&entry;
        auto group=wgpuDeviceCreateBindGroup(device,&groupDesc);
        wgpuBindGroupLayoutRelease(layout);
        if (!group) {if (owned) wgpuRenderPipelineRelease(pipeline);return;}
        WGPURenderPassDepthStencilAttachment depthTarget=WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depthTarget.view=r.destination;depthTarget.depthLoadOp=WGPULoadOp_Clear;depthTarget.depthStoreOp=WGPUStoreOp_Store;
        depthTarget.depthClearValue=0;
        WGPURenderPassColorAttachment colorTarget=WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        colorTarget.view=r.destination;colorTarget.loadOp=WGPULoadOp_Clear;colorTarget.storeOp=WGPUStoreOp_Store;
        colorTarget.clearValue={0,0,0,0};
        WGPURenderPassDescriptor passDesc=WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        if (r.depth) passDesc.depthStencilAttachment=&depthTarget;
        else {passDesc.colorAttachmentCount=1;passDesc.colorAttachments=&colorTarget;}
        auto pass=wgpuCommandEncoderBeginRenderPass(r.encoder,&passDesc);
        if (pass) {
            wgpuRenderPassEncoderSetPipeline(pass,pipeline);
            wgpuRenderPassEncoderSetBindGroup(pass,0,group,0,nullptr);
            wgpuRenderPassEncoderSetViewport(pass,0,0,float(r.width),float(r.height),0,1);
            wgpuRenderPassEncoderSetScissorRect(pass,0,0,r.width,r.height);
            wgpuRenderPassEncoderDraw(pass,3,1,0,0);
            wgpuRenderPassEncoderEnd(pass);wgpuRenderPassEncoderRelease(pass);r.issued=true;
        }
        wgpuBindGroupRelease(group);if (owned) wgpuRenderPipelineRelease(pipeline);
    },&request) && request.issued;
}
#include "gpu-depth-unorm16-copy-web.inl"
bool EncodeGpuColorResolve(WGPUCommandEncoder encoder, WGPUTextureView source, WGPUTextureView destination) {
    if (!encoder || !source || !destination || source == destination) return false;
    struct Request { WGPUCommandEncoder encoder; WGPUTextureView source, destination; bool issued=false; }
        request{encoder,source,destination};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        WGPURenderPassColorAttachment target = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        target.view = request.source; target.resolveTarget = request.destination;
        target.loadOp = WGPULoadOp_Load; target.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = 1; descriptor.colorAttachments = &target;
        auto pass = wgpuCommandEncoderBeginRenderPass(request.encoder, &descriptor);
        if (!pass) return;
        wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
        request.issued = true;
    }, &request) && request.issued;
}
static int ValidateGraphicsDraw(const GraphicsDraw& draw) {
    auto finite = [](float value) {
        std::uint32_t bits; std::memcpy(&bits, &value, sizeof(bits));
        return (bits & 0x7f800000u) != 0x7f800000u;
    };
    if (!draw.pipeline || (!draw.colorCount && !draw.depth) ||
        draw.colorCount > 8 || (draw.colorCount && !draw.colors) ||
        !draw.targetWidth || !draw.targetHeight ||
        (draw.vertexCount && !draw.vertices) || (draw.groupCount && !draw.groups) ||
        !finite(draw.viewportX) || !finite(draw.viewportY) ||
        !finite(draw.viewportWidth) || !finite(draw.viewportHeight) ||
        !finite(draw.minDepth) || !finite(draw.maxDepth) ||
        draw.viewportX < 0 || draw.viewportY < 0 ||
        draw.viewportWidth <= 0 || draw.viewportHeight <= 0 ||
        draw.viewportX + draw.viewportWidth > draw.targetWidth ||
        draw.viewportY + draw.viewportHeight > draw.targetHeight ||
        draw.minDepth < 0 || draw.maxDepth > 1 || draw.minDepth > draw.maxDepth ||
        draw.scissorX > draw.targetWidth || draw.scissorWidth > draw.targetWidth - draw.scissorX ||
        draw.scissorY > draw.targetHeight || draw.scissorHeight > draw.targetHeight - draw.scissorY)
        return -1;
    for (std::size_t i = 0; i < draw.vertexCount; ++i) {
        const auto& vertex=draw.vertices[i];
        if(vertex.unused){if(vertex.buffer||vertex.offset||vertex.bytes)return -1;continue;}
        if (!vertex.buffer || !vertex.bytes || vertex.offset % 4) return -1;
    }
    for (std::size_t i = 0; i < draw.groupCount; ++i) {
        if (!draw.groups[i].group || draw.groups[i].dynamicOffsetCount > kMaxGraphicsDrawDynamicOffsets) return -1;
        for (std::uint32_t k = 0; k < draw.groups[i].dynamicOffsetCount; ++k)
            if (draw.groups[i].dynamicOffsets[k] % 256) return -1; // minUniformBufferOffsetAlignment
        for (std::size_t j = 0; j < i; ++j)
            if (draw.groups[i].index == draw.groups[j].index) return -1;
    }
    if (draw.indexed) {
        const auto width = draw.indexFormat == WGPUIndexFormat_Uint16 ? 2u :
            draw.indexFormat == WGPUIndexFormat_Uint32 ? 4u : 0u;
        if (!width || !draw.indices.buffer || !draw.indices.bytes ||
            draw.indices.offset % width || draw.indices.bytes % width ||
            (!draw.indirectCount && std::uint64_t(draw.first) + draw.count > draw.indices.bytes / width)) return -1;
    }
    if (draw.indirectCount) {
        const auto stride = draw.indexed ? 20u : 16u;
        if (!draw.indirectArguments.buffer || draw.indirectArguments.offset % 4 ||
            draw.indirectArguments.offset > draw.indirectArguments.bytes ||
            std::uint64_t(draw.indirectCount)*stride > draw.indirectArguments.bytes-draw.indirectArguments.offset)
            return -1;
    }
    // Zero-count/empty-scissor commands have no fragments, matching native draws.
    if ((!draw.indirectCount && (!draw.count || !draw.instances)) || !draw.scissorWidth || !draw.scissorHeight) {
        // Silent no-op draws hid the fullscreen present/mip passes; report the first ones and then powers of two.
        static std::atomic<unsigned> emptyDraws{0};
        const unsigned n=++emptyDraws;
        if(n<=16 || !(n&(n-1)))std::fprintf(stderr,"CP2077 empty draw dropped: count=%u instances=%u indirect=%u scissor=%u,%u %ux%u target=%ux%u vertexBuffers=%u total=%u\n",
            unsigned(draw.count),unsigned(draw.instances),unsigned(draw.indirectCount),unsigned(draw.scissorX),unsigned(draw.scissorY),
            unsigned(draw.scissorWidth),unsigned(draw.scissorHeight),unsigned(draw.targetWidth),unsigned(draw.targetHeight),unsigned(draw.vertexCount),n);
        return 0;
    }
    return 1;
}
// Main-thread half of a graphics draw: one render pass with this draw. Shared by immediate and queued encoding.
// Main-thread halves of a graphics draw. Batched draws reuse one render pass across consecutive draws that
// target the same attachments (see EncodeGpuOrderedCommands); a lone draw gets its own pass.
static bool PrepareGraphicsDrawOnMain(const GraphicsDraw& draw) {
        if (!MainThreadGpuDevice()) return false;
        // Arena-backed uniforms for this draw ride along with its encode instead of costing their own proxy.
        if (draw.uniformWriteCount) {
            auto queue = MainThreadGpuQueue(); if (!queue) return false;
            for (std::size_t i = 0; i < draw.uniformWriteCount; ++i) {
                const auto& write = draw.uniformWrites[i];
                wgpuQueueWriteBuffer(queue, write.buffer, write.offset, write.bytes, write.size);
            }
        }
        // Without this feature, GPU-supplied nonzero firstInstance silently
        // discards the indirect draw. Require faithful argument interpretation.
        if (draw.indirectCount) {
            if (DrawTrimOn()) { // [skperf] round 15: one feature query per device generation (main thread only)
                static std::uint64_t cachedGeneration = 0; static bool cachedHas = false;
                if (cachedGeneration != GpuDeviceGeneration()) { cachedHas = wgpuDeviceHasFeature(MainThreadGpuDevice(), WGPUFeatureName_IndirectFirstInstance); cachedGeneration = GpuDeviceGeneration(); }
                if (!cachedHas) return false;
            } else if (!wgpuDeviceHasFeature(MainThreadGpuDevice(), WGPUFeatureName_IndirectFirstInstance)) return false;
        }
        return true;
}
// [nomadperf] Per-pass redundant state elision (main thread only). Every queued draw used to re-send pipeline,
// viewport, scissor, blend constant, stencil reference, every vertex buffer, every bind group and the index buffer,
// ~12-15 JS WebGPU calls per draw (each a wasm->JS crossing with BigInt pointer conversion under memory64).
// WebGPU render-pass state persists across draws and across setPipeline, so a call whose arguments equal the
// state already set in THIS pass is a no-op. The cache is reset whenever a draw pass begins, so it never carries
// over to another pass (a released pass handle may be reused). ?passDedup=0 restores the old per-draw calls.
namespace {
struct PassStateCache {
    bool valid = false;
    WGPURenderPipeline pipeline = nullptr;
    bool viewportSet = false; float vp[6]{};
    bool scissorSet = false; std::uint32_t sc[4]{};
    bool blendSet = false; WGPUColor blend{};
    bool stencilSet = false; std::uint32_t stencil = 0;
    static constexpr std::size_t kVertexSlots = 16, kGroups = 8;
    GraphicsDrawBuffer vertex[kVertexSlots]; bool vertexSet[kVertexSlots]{};
    WGPUBindGroup group[kGroups]{}; std::uint32_t groupOffsetCount[kGroups]{}; std::uint32_t groupOffsets[kGroups][kMaxGraphicsDrawDynamicOffsets]{}; bool groupSet[kGroups]{};
    bool indexSet = false; GraphicsDrawBuffer index; WGPUIndexFormat indexFormat = WGPUIndexFormat_Undefined;
    void Reset() { *this = PassStateCache{}; valid = true; }
};
PassStateCache passState; // main thread only
std::uint64_t passDedupCalls[2]{}; // [0]=issued, [1]=elided
bool PassDedupOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('passDedup') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 nomadperf switches: passDedup=%d\n", v); return v; }();
    return on != 0;
}
}
static WGPURenderPassEncoder BeginGraphicsDrawPass(WGPUCommandEncoder encoder, const GraphicsDraw& draw) {
        passState.Reset();
        // [skperf] round 15 beginArr: fixed stack array for the common case (<= 8 attachments); else the old vector.
        std::array<WGPURenderPassColorAttachment, 8> fixedColors;
        std::vector<WGPURenderPassColorAttachment> heapColors;
        const bool useFixed = BeginArrOn() && draw.colorCount <= fixedColors.size();
        if (!useFixed) heapColors.assign(draw.colorCount, WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT);
        WGPURenderPassColorAttachment* colorData = useFixed ? fixedColors.data() : heapColors.data();
        for (std::size_t i = 0; i < draw.colorCount; ++i) {
            if (useFixed) colorData[i] = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            colorData[i].view = draw.colors[i];
            colorData[i].loadOp = WGPULoadOp_Load; colorData[i].storeOp = WGPUStoreOp_Store;
        }
        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depth.view = draw.depth; depth.depthReadOnly = draw.depthReadOnly;
        // The installed Emdawn helper emits the C initializer's optional NaN
        // sentinel as a JS float. Load/read-only passes never consume this
        // clear value, but WebIDL still requires a finite supplied float.
        // Explicit native depth clears keep their own value in EncodeGpuDepthClear.
        depth.depthClearValue = 0.0f;
        // Original DX12 creates its read-only DSV with READ_ONLY_DEPTH and,
        // for stencil formats, READ_ONLY_STENCIL. Preserve both planes; a
        // writable stencil attachment conflicts with sampling this texture.
        depth.stencilReadOnly = draw.hasStencil && draw.depthReadOnly;
        if (!draw.depthReadOnly) {
            depth.depthLoadOp = WGPULoadOp_Load; depth.depthStoreOp = WGPUStoreOp_Store;
        }
        if (draw.hasStencil && !depth.stencilReadOnly) {
            depth.stencilLoadOp = WGPULoadOp_Load; depth.stencilStoreOp = WGPUStoreOp_Store;
        }
        WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        descriptor.colorAttachmentCount = draw.colorCount; descriptor.colorAttachments = draw.colorCount ? colorData : nullptr;
        descriptor.depthStencilAttachment = draw.depth ? &depth : nullptr;
        return wgpuCommandEncoderBeginRenderPass(encoder, &descriptor);
}
// [nomadperf] Replay path: the same deduplicated state calls plus the draw, written as u32 words and replayed by ONE
// JS call (cp2077_replayPassStream in webgpu-lowalloc-web.js). ?passReplay=0 = one glue call per state call.
extern "C" void cp2077_replayPassStream(WGPURenderPassEncoder pass, const std::uint32_t* words, int count);
namespace {
bool PassReplayOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('passReplay') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 nomadperf switches: passReplay=%d\n", v); return v; }();
    return on != 0;
}
std::vector<std::uint32_t> replayWords; // main thread only
// [skperf] round 9 passBatch (default ON, ?passBatch=0 = one replay call per draw): the deduplicated state calls and
// draws of ALL queued draws in one render pass go to cp2077_replayPassStream in one call, flushed when the pass is
// closed (EncodeGpuOrderedCommands closePass, before wgpuRenderPassEncoderEnd) or when the stream grows past 64k
// words. Same calls in the same order on the same pass; only the wasm->JS crossings are batched (~6000 -> ~150 per
// frame). Every handle in the stream is retained by its command list until GPU retirement, so it is still alive at
// the flush. replayPass records which pass the pending words belong to; a mismatch (never expected) flushes first.
WGPURenderPassEncoder replayPass = nullptr; // main thread only
// [skperf] round 12 idxFold (default ON, ?idxFold=0 old): a non-indirect indexed draw whose index binding is
// (buffer, offset, bytes) is replayed with the WHOLE index buffer bound at offset 0 and firstIndex += offset/indexSize.
// drawIndexed reads exactly the same indices (offset is a multiple of the index size; WebGPU requires that), so draws
// from one pooled index buffer no longer re-send setIndexBuffer each. Indirect draws keep the original binding
// (their firstIndex lives in GPU memory). Buffer sizes are cached per pass only (handles stay alive during it).
bool IdxFoldOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('idxFold') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: idxFold=%d\n", v); return v; }();
    return on != 0;
}
struct IndexSizeCache { WGPUBuffer buffer[16]{}; std::uint64_t size[16]{}; unsigned next = 0; };
IndexSizeCache indexSizes; // main thread only, reset with the pass state
std::uint64_t CachedBufferSize(WGPUBuffer buffer) {
    for (unsigned i = 0; i < 16; ++i) if (indexSizes.buffer[i] == buffer) return indexSizes.size[i];
    const std::uint64_t size = wgpuBufferGetSize(buffer);
    indexSizes.buffer[indexSizes.next] = buffer; indexSizes.size[indexSizes.next] = size; indexSizes.next = (indexSizes.next + 1) & 15;
    return size;
}
bool PassBatchOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('passBatch') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: passBatch=%d\n", v); return v; }();
    return on != 0;
}
inline void PutPtr(const void* p) { const auto v = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); replayWords.push_back(std::uint32_t(v)); replayWords.push_back(std::uint32_t(v >> 32)); }
inline void Put64(std::uint64_t v) { replayWords.push_back(std::uint32_t(v)); replayWords.push_back(std::uint32_t(v >> 32)); }
inline void PutF(float f) { std::uint32_t u; std::memcpy(&u, &f, 4); replayWords.push_back(u); }
}
bool replayDeferredPass = false; // main thread only
static bool ReplayBatchActive() { return PassDedupOn() && PassReplayOn() && PassBatchOn(); }
// [skperf] round 13: replay the pending words of token (a lazily opened pass) into the real pass encoder.
static void FlushPassReplayTo(WGPURenderPassEncoder token, WGPURenderPassEncoder real) {
    if (!replayWords.empty() && replayPass == token && real) {
        cp2077_replayPassStream(real, replayWords.data(), int(replayWords.size()));
        skperfReplayStats[1].fetch_add(1, std::memory_order_relaxed);
    }
    replayWords.clear(); replayPass = nullptr;
}
static void ResetPassStateForLazyPass() { passState.Reset(); }
static void FlushPassReplay(WGPURenderPassEncoder pass) {
    if (replayWords.empty()) { replayPass = nullptr; return; }
    // Words always belong to replayPass (set when they were written); flush them to it while it is still open.
    if (replayPass && (!pass || replayPass == pass)) {
        cp2077_replayPassStream(replayPass, replayWords.data(), int(replayWords.size()));
        skperfReplayStats[1].fetch_add(1, std::memory_order_relaxed);
    }
    replayWords.clear(); replayPass = nullptr;
}
static void RecordGraphicsDrawReplay(WGPURenderPassEncoder pass, const GraphicsDraw& draw) {
    const bool batch = PassBatchOn();
    if (DrawTrimOn() && replayWords.capacity() < 65536) replayWords.reserve(65536); // [skperf] round 15: once
    if (batch && replayPass && replayPass != pass) { // defensive (never expected): the old pass already ended; drop, never replay into it
        static bool warned = false; if (!warned) { warned = true; std::fprintf(stderr, "CP2077 skperf passBatch: pending words of an ended pass dropped\n"); }
        replayWords.clear(); replayPass = nullptr; }
    auto& st = passState; auto& w = replayWords; if (!batch) w.clear(); unsigned issued = 0, elided = 0;
    if (replayPass != pass) indexSizes = IndexSizeCache{}; // [skperf] round 12: sizes cached per pass only
    replayPass = pass;
    const bool counting = skperfEncodeProbe.load(std::memory_order_relaxed) != 0;
    const unsigned issuedBefore[1] = {0}; (void)issuedBefore;
    auto countIssued = [&](unsigned type) { if (counting) skperfIssued[type].fetch_add(1, std::memory_order_relaxed); };
    if (st.pipeline != draw.pipeline) { w.push_back(1); PutPtr(draw.pipeline); st.pipeline = draw.pipeline; ++issued; countIssued(0); } else ++elided;
    const float vp[6] = {draw.viewportX, draw.viewportY, draw.viewportWidth, draw.viewportHeight, draw.minDepth, draw.maxDepth};
    if (!st.viewportSet || std::memcmp(st.vp, vp, sizeof(vp))) { w.push_back(2); for (float v : vp) PutF(v); std::memcpy(st.vp, vp, sizeof(vp)); st.viewportSet = true; ++issued; countIssued(1); } else ++elided;
    const std::uint32_t sc[4] = {draw.scissorX, draw.scissorY, draw.scissorWidth, draw.scissorHeight};
    if (!st.scissorSet || std::memcmp(st.sc, sc, sizeof(sc))) { w.push_back(3); for (auto v : sc) w.push_back(v); std::memcpy(st.sc, sc, sizeof(sc)); st.scissorSet = true; ++issued; countIssued(2); } else ++elided;
    if (!st.blendSet || std::memcmp(&st.blend, &draw.blendFactor, sizeof(WGPUColor))) {
        w.push_back(4); PutF(float(draw.blendFactor.r)); PutF(float(draw.blendFactor.g)); PutF(float(draw.blendFactor.b)); PutF(float(draw.blendFactor.a));
        st.blend = draw.blendFactor; st.blendSet = true; ++issued; countIssued(3); } else ++elided;
    if (!st.stencilSet || st.stencil != draw.stencilReference) { w.push_back(5); w.push_back(draw.stencilReference); st.stencil = draw.stencilReference; st.stencilSet = true; ++issued; countIssued(4); } else ++elided;
    for (std::size_t i = 0; i < draw.vertexCount; ++i) {
        const auto& v = draw.vertices[i]; if (v.unused) continue;
        if (i < PassStateCache::kVertexSlots && st.vertexSet[i] && st.vertex[i].buffer == v.buffer && st.vertex[i].offset == v.offset && st.vertex[i].bytes == v.bytes) { ++elided; continue; }
        w.push_back(6); w.push_back(std::uint32_t(i)); PutPtr(v.buffer); Put64(v.offset); Put64(v.bytes); ++issued; countIssued(5);
        if (i < PassStateCache::kVertexSlots) { st.vertex[i] = v; st.vertexSet[i] = true; }
    }
    for (std::size_t i = 0; i < draw.groupCount; ++i) {
        const auto& g = draw.groups[i]; const auto gi = g.index; const auto n = g.dynamicOffsetCount;
        if (gi < PassStateCache::kGroups && st.groupSet[gi] && st.group[gi] == g.group && st.groupOffsetCount[gi] == n &&
            (!n || !std::memcmp(st.groupOffsets[gi], g.dynamicOffsets, n * sizeof(std::uint32_t)))) { ++elided; continue; }
        w.push_back(7); w.push_back(gi); PutPtr(g.group); w.push_back(n); for (std::uint32_t k = 0; k < n; ++k) w.push_back(g.dynamicOffsets[k]); ++issued; countIssued(6);
        if (gi < PassStateCache::kGroups && n <= kMaxGraphicsDrawDynamicOffsets) {
            st.group[gi] = g.group; st.groupOffsetCount[gi] = n; if (n) std::memcpy(st.groupOffsets[gi], g.dynamicOffsets, n * sizeof(std::uint32_t)); st.groupSet[gi] = true;
        } else if (gi < PassStateCache::kGroups) st.groupSet[gi] = false;
    }
    std::uint32_t firstIndex = draw.first; // [skperf] round 12 idxFold may add the folded offset
    if (draw.indexed) {
        bool folded = false;
        if (!draw.indirectCount && IdxFoldOn() && draw.indices.buffer &&
            (draw.indexFormat == WGPUIndexFormat_Uint16 || draw.indexFormat == WGPUIndexFormat_Uint32)) {
            const std::uint64_t indexSize = draw.indexFormat == WGPUIndexFormat_Uint16 ? 2 : 4;
            const std::uint64_t shift = draw.indices.offset / indexSize;
            if (draw.indices.offset % indexSize == 0 && shift + std::uint64_t(draw.first) <= 0xffffffffull) {
                if (!(st.indexSet && st.index.buffer == draw.indices.buffer && st.index.offset == 0 && st.indexFormat == draw.indexFormat &&
                      st.index.bytes == CachedBufferSize(draw.indices.buffer))) {
                    const std::uint64_t whole = CachedBufferSize(draw.indices.buffer);
                    if (whole >= draw.indices.offset + draw.indices.bytes) {
                        w.push_back(8); w.push_back(std::uint32_t(draw.indexFormat)); PutPtr(draw.indices.buffer); Put64(0); Put64(whole);
                        st.index.buffer = draw.indices.buffer; st.index.offset = 0; st.index.bytes = whole; st.indexFormat = draw.indexFormat; st.indexSet = true;
                        ++issued; countIssued(7); folded = true;
                    }
                } else { ++elided; folded = true; }
                if (folded) { firstIndex = std::uint32_t(shift + draw.first); if (counting && shift) skperfPassStats[10].fetch_add(1, std::memory_order_relaxed); }
            }
        }
        if (!folded) {
            if (st.indexSet && st.index.buffer == draw.indices.buffer && st.index.offset == draw.indices.offset &&
                st.index.bytes == draw.indices.bytes && st.indexFormat == draw.indexFormat) ++elided;
            else {
                w.push_back(8); w.push_back(std::uint32_t(draw.indexFormat)); PutPtr(draw.indices.buffer); Put64(draw.indices.offset); Put64(draw.indices.bytes);
                st.index = draw.indices; st.indexFormat = draw.indexFormat; st.indexSet = true; ++issued; countIssued(7);
            }
        }
    }
    if (counting) skperfIssued[8].fetch_add(1, std::memory_order_relaxed);
    if (draw.indirectCount) {
        const std::uint64_t stride = draw.indexed ? 20 : 16;
        for (std::uint32_t i = 0; i < draw.indirectCount; ++i) {
            w.push_back(draw.indexed ? 12 : 11); PutPtr(draw.indirectArguments.buffer); Put64(draw.indirectArguments.offset + std::uint64_t(i)*stride);
        }
    } else if (draw.indexed) {
        std::uint32_t base; std::memcpy(&base, &draw.baseVertex, 4);
        w.push_back(10); w.push_back(draw.count); w.push_back(draw.instances); w.push_back(firstIndex); w.push_back(base); w.push_back(draw.firstInstance);
    } else { w.push_back(9); w.push_back(draw.count); w.push_back(draw.instances); w.push_back(draw.first); w.push_back(draw.firstInstance); }
    skperfReplayStats[0].fetch_add(1, std::memory_order_relaxed);
    skperfReplayStats[2].fetch_add(issued, std::memory_order_relaxed); skperfReplayStats[3].fetch_add(elided, std::memory_order_relaxed);
    if (!batch) { cp2077_replayPassStream(pass, w.data(), int(w.size())); skperfReplayStats[1].fetch_add(1, std::memory_order_relaxed); w.clear(); replayPass = nullptr; }
    else if (w.size() >= 65536 && !replayDeferredPass) FlushPassReplay(pass);
    passDedupCalls[0] += issued; passDedupCalls[1] += elided;
    static std::uint64_t draws = 0;
    if ((++draws & ((1u << 16) - 1)) == 0)
        std::fprintf(stderr, "CP2077 nomadperf pass replay: draws=%llu stateCallsIssued=%llu elided=%llu\n",
            (unsigned long long)draws, (unsigned long long)passDedupCalls[0], (unsigned long long)passDedupCalls[1]);
}
static void RecordGraphicsDrawInPass(WGPURenderPassEncoder pass, const GraphicsDraw& draw) {
    if (PassDedupOn() && passState.valid && PassReplayOn()) { RecordGraphicsDrawReplay(pass, draw); return; }
    if (!PassDedupOn() || !passState.valid) {
        wgpuRenderPassEncoderSetPipeline(pass, draw.pipeline);
        wgpuRenderPassEncoderSetViewport(pass, draw.viewportX, draw.viewportY,
            draw.viewportWidth, draw.viewportHeight, draw.minDepth, draw.maxDepth);
        wgpuRenderPassEncoderSetScissorRect(pass, draw.scissorX, draw.scissorY, draw.scissorWidth, draw.scissorHeight);
        wgpuRenderPassEncoderSetBlendConstant(pass, &draw.blendFactor);
        wgpuRenderPassEncoderSetStencilReference(pass, draw.stencilReference);
        for (std::size_t i = 0; i < draw.vertexCount; ++i)
            if(!draw.vertices[i].unused)wgpuRenderPassEncoderSetVertexBuffer(pass, i, draw.vertices[i].buffer,
                                                 draw.vertices[i].offset, draw.vertices[i].bytes);
        for (std::size_t i = 0; i < draw.groupCount; ++i)
            wgpuRenderPassEncoderSetBindGroup(pass, draw.groups[i].index, draw.groups[i].group,
                draw.groups[i].dynamicOffsetCount, draw.groups[i].dynamicOffsetCount ? draw.groups[i].dynamicOffsets : nullptr);
        if (draw.indexed) {
            wgpuRenderPassEncoderSetIndexBuffer(pass, draw.indices.buffer, draw.indexFormat,
                                               draw.indices.offset, draw.indices.bytes);
        }
        passState.valid = false; // state set without the cache: no later draw in this pass may elide against it
    } else {
        auto& st = passState; unsigned issued = 0, elided = 0;
        if (st.pipeline != draw.pipeline) { wgpuRenderPassEncoderSetPipeline(pass, draw.pipeline); st.pipeline = draw.pipeline; ++issued; } else ++elided;
        const float vp[6] = {draw.viewportX, draw.viewportY, draw.viewportWidth, draw.viewportHeight, draw.minDepth, draw.maxDepth};
        if (!st.viewportSet || std::memcmp(st.vp, vp, sizeof(vp))) {
            wgpuRenderPassEncoderSetViewport(pass, vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
            std::memcpy(st.vp, vp, sizeof(vp)); st.viewportSet = true; ++issued; } else ++elided;
        const std::uint32_t sc[4] = {draw.scissorX, draw.scissorY, draw.scissorWidth, draw.scissorHeight};
        if (!st.scissorSet || std::memcmp(st.sc, sc, sizeof(sc))) {
            wgpuRenderPassEncoderSetScissorRect(pass, sc[0], sc[1], sc[2], sc[3]);
            std::memcpy(st.sc, sc, sizeof(sc)); st.scissorSet = true; ++issued; } else ++elided;
        if (!st.blendSet || std::memcmp(&st.blend, &draw.blendFactor, sizeof(WGPUColor))) {
            wgpuRenderPassEncoderSetBlendConstant(pass, &draw.blendFactor); st.blend = draw.blendFactor; st.blendSet = true; ++issued; } else ++elided;
        if (!st.stencilSet || st.stencil != draw.stencilReference) {
            wgpuRenderPassEncoderSetStencilReference(pass, draw.stencilReference); st.stencil = draw.stencilReference; st.stencilSet = true; ++issued; } else ++elided;
        for (std::size_t i = 0; i < draw.vertexCount; ++i) {
            const auto& v = draw.vertices[i]; if (v.unused) continue;
            if (i < PassStateCache::kVertexSlots && st.vertexSet[i] && st.vertex[i].buffer == v.buffer && st.vertex[i].offset == v.offset && st.vertex[i].bytes == v.bytes) { ++elided; continue; }
            wgpuRenderPassEncoderSetVertexBuffer(pass, i, v.buffer, v.offset, v.bytes); ++issued;
            if (i < PassStateCache::kVertexSlots) { st.vertex[i] = v; st.vertexSet[i] = true; }
        }
        for (std::size_t i = 0; i < draw.groupCount; ++i) {
            const auto& g = draw.groups[i]; const auto gi = g.index;
            const auto n = g.dynamicOffsetCount;
            if (gi < PassStateCache::kGroups && st.groupSet[gi] && st.group[gi] == g.group && st.groupOffsetCount[gi] == n &&
                (!n || !std::memcmp(st.groupOffsets[gi], g.dynamicOffsets, n * sizeof(std::uint32_t)))) { ++elided; continue; }
            wgpuRenderPassEncoderSetBindGroup(pass, gi, g.group, n, n ? g.dynamicOffsets : nullptr); ++issued;
            if (gi < PassStateCache::kGroups && n <= kMaxGraphicsDrawDynamicOffsets) {
                st.group[gi] = g.group; st.groupOffsetCount[gi] = n; if (n) std::memcpy(st.groupOffsets[gi], g.dynamicOffsets, n * sizeof(std::uint32_t)); st.groupSet[gi] = true;
            } else if (gi < PassStateCache::kGroups) st.groupSet[gi] = false;
        }
        if (draw.indexed) {
            if (st.indexSet && st.index.buffer == draw.indices.buffer && st.index.offset == draw.indices.offset &&
                st.index.bytes == draw.indices.bytes && st.indexFormat == draw.indexFormat) ++elided;
            else {
                wgpuRenderPassEncoderSetIndexBuffer(pass, draw.indices.buffer, draw.indexFormat, draw.indices.offset, draw.indices.bytes);
                st.index = draw.indices; st.indexFormat = draw.indexFormat; st.indexSet = true; ++issued;
            }
        }
        passDedupCalls[0] += issued; passDedupCalls[1] += elided;
        static std::uint64_t draws = 0;
        if ((++draws & ((1u << 16) - 1)) == 0)
            std::fprintf(stderr, "CP2077 nomadperf pass dedup: draws=%llu stateCallsIssued=%llu elided=%llu\n",
                (unsigned long long)draws, (unsigned long long)passDedupCalls[0], (unsigned long long)passDedupCalls[1]);
    }
        if (draw.indirectCount) {
            const std::uint64_t stride = draw.indexed ? 20 : 16;
            for (std::uint32_t i = 0; i < draw.indirectCount; ++i) {
                const auto offset = draw.indirectArguments.offset + std::uint64_t(i)*stride;
                if (draw.indexed) wgpuRenderPassEncoderDrawIndexedIndirect(pass, draw.indirectArguments.buffer, offset);
                else wgpuRenderPassEncoderDrawIndirect(pass, draw.indirectArguments.buffer, offset);
            }
        } else if (draw.indexed)
            wgpuRenderPassEncoderDrawIndexed(pass, draw.count, draw.instances, draw.first, draw.baseVertex, draw.firstInstance);
        else wgpuRenderPassEncoderDraw(pass, draw.count, draw.instances, draw.first, draw.firstInstance);
}
static bool SameGraphicsDrawTargets(const GraphicsDraw& a, const GraphicsDraw& b) {
    if (a.colorCount != b.colorCount || a.depth != b.depth || a.depthReadOnly != b.depthReadOnly || a.hasStencil != b.hasStencil) return false;
    for (std::size_t i = 0; i < a.colorCount; ++i) if (a.colors[i] != b.colors[i]) return false;
    return true;
}
static bool EncodeGraphicsDrawOnMain(WGPUCommandEncoder encoder, const GraphicsDraw& draw) {
    if (!PrepareGraphicsDrawOnMain(draw)) return false;
    auto pass = BeginGraphicsDrawPass(encoder, draw);
    if (!pass) return false;
    RecordGraphicsDrawInPass(pass, draw);
    wgpuRenderPassEncoderEnd(pass); wgpuRenderPassEncoderRelease(pass);
    return true;
}
bool EncodeGpuGraphicsDraw(WGPUCommandEncoder encoder, const GraphicsDraw& draw) {
    if (!encoder) return false;
    const int state = ValidateGraphicsDraw(draw);
    if (state <= 0) return state == 0;
    struct Request { WGPUCommandEncoder encoder; const GraphicsDraw* draw; bool encoded = false; } request{encoder, &draw};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        request.encoded = EncodeGraphicsDrawOnMain(request.encoder, *request.draw);
    }, &request) && request.encoded;
}
// [skperf] round 14 drawPool (default ON, ?drawPool=0 = old make_shared per draw). Queued draws come from a pool:
// a recycled object keeps the capacity of its five vectors, so a typical draw costs one shared_ptr control block
// instead of the object plus up to five vector allocations. The deleter (run wherever the last reference drops:
// main thread after encoding, or the retirement worker) calls Recycle(), which releases exactly what the destructor
// releases, then returns the object to a per-thread cache (spilling in batches to a mutex-guarded global list that
// recording threads refill from). Pool sizes are bounded; beyond them objects are deleted.
// skperfDrawPoolStats: [0] queued draws, [1] object/control-block heap allocations (pool misses add one more),
// [2] vector growths (each a heap allocation).
std::atomic<unsigned long long> skperfDrawPoolStats[3];
namespace {
bool DrawPoolOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('drawPool') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: drawPool=%d\n", v); return v; }();
    return on != 0;
}
std::mutex drawPoolMutex;
std::vector<GpuQueuedDraw*> drawPoolGlobal; // guarded by drawPoolMutex
constexpr std::size_t kDrawPoolLocal = 256, kDrawPoolBatch = 64, kDrawPoolGlobalMax = 16384;
struct DrawPoolLocal {
    std::vector<GpuQueuedDraw*> items;
    ~DrawPoolLocal() { for (auto* item : items) delete item; }
};
DrawPoolLocal& LocalDrawPool() { static thread_local DrawPoolLocal local; return local; }
GpuQueuedDraw* AcquirePooledDraw() {
    auto& local = LocalDrawPool().items;
    if (local.empty()) {
        std::lock_guard<std::mutex> lock(drawPoolMutex);
        const std::size_t take = std::min(kDrawPoolBatch, drawPoolGlobal.size());
        local.insert(local.end(), drawPoolGlobal.end() - take, drawPoolGlobal.end());
        drawPoolGlobal.resize(drawPoolGlobal.size() - take);
    }
    if (local.empty()) { skperfDrawPoolStats[1].fetch_add(1, std::memory_order_relaxed); return new GpuQueuedDraw(); }
    auto* item = local.back(); local.pop_back(); return item;
}
void ReleasePooledDraw(GpuQueuedDraw* item) {
    item->Recycle();
    auto& local = LocalDrawPool().items;
    if (local.size() < kDrawPoolLocal) { local.push_back(item); return; }
    // Spill a batch (plus this one) to the global list; delete what does not fit.
    std::vector<GpuQueuedDraw*> overflow;
    {
        std::lock_guard<std::mutex> lock(drawPoolMutex);
        std::size_t moved = 0;
        while (moved < kDrawPoolBatch && !local.empty() && drawPoolGlobal.size() < kDrawPoolGlobalMax) { drawPoolGlobal.push_back(local.back()); local.pop_back(); ++moved; }
        if (drawPoolGlobal.size() < kDrawPoolGlobalMax) drawPoolGlobal.push_back(item); else overflow.push_back(item);
    }
    for (auto* extra : overflow) delete extra;
}
}
namespace {
// [skperf] round 44 ctrlPool (default ON with drawPool, ?ctrlPool=0 old): the pooled GpuQueuedDraw still got a fresh
// shared_ptr control block per draw (malloc on the recording worker, free where the last reference drops, usually the
// main-thread encode; both under the one global allocator lock every thread shares). The control block now comes from
// a fixed-size block pool with the same two-level design as drawPool (thread-local list, batched spill to a global list
// under a mutex). Ownership, deleter and release timing/thread are unchanged; only the block's memory is reused.
template <std::size_t Size>
struct CpBlockPool {
    static constexpr std::size_t kLocal = 256, kBatch = 64, kGlobalMax = 16384;
    static std::mutex& Mutex() { static std::mutex m; return m; }
    static std::vector<void*>& Global() { static auto* g = new std::vector<void*>(); return *g; } // never destroyed
    struct Local { std::vector<void*> items; ~Local() { for (void* p : items) ::operator delete(p); } };
    static std::vector<void*>& LocalItems() { static thread_local Local local; return local.items; }
    static void* Acquire() {
        auto& local = LocalItems();
        if (local.empty()) {
            if (local.capacity() < kLocal + kBatch + 1) local.reserve(kLocal + kBatch + 1);
            std::lock_guard<std::mutex> lock(Mutex());
            auto& global = Global();
            const std::size_t take = std::min(kBatch, global.size());
            local.insert(local.end(), global.end() - take, global.end());
            global.resize(global.size() - take);
        }
        if (local.empty()) return ::operator new(Size);
        void* p = local.back(); local.pop_back(); return p;
    }
    static void Release(void* p) {
        auto& local = LocalItems();
        if (local.capacity() < kLocal + kBatch + 1) local.reserve(kLocal + kBatch + 1);
        if (local.size() < kLocal) { local.push_back(p); return; }
        bool kept = false;
        {
            std::lock_guard<std::mutex> lock(Mutex());
            auto& global = Global();
            if (global.capacity() < kGlobalMax + 1) global.reserve(kGlobalMax + 1);
            std::size_t moved = 0;
            while (moved < kBatch && !local.empty() && global.size() < kGlobalMax) { global.push_back(local.back()); local.pop_back(); ++moved; }
            if (global.size() < kGlobalMax) { global.push_back(p); kept = true; }
        }
        if (!kept) ::operator delete(p);
    }
};
template <class T>
struct CpCtrlAlloc {
    using value_type = T;
    CpCtrlAlloc() = default;
    template <class U> CpCtrlAlloc(const CpCtrlAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        static_assert(alignof(T) <= alignof(std::max_align_t), "pool blocks use operator new alignment");
        if (n != 1) return static_cast<T*>(::operator new(n * sizeof(T)));
        return static_cast<T*>(CpBlockPool<sizeof(T)>::Acquire());
    }
    void deallocate(T* p, std::size_t n) noexcept {
        if (n != 1) { ::operator delete(p); return; }
        CpBlockPool<sizeof(T)>::Release(p);
    }
    template <class U> bool operator==(const CpCtrlAlloc<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const CpCtrlAlloc<U>&) const noexcept { return false; }
};
bool CtrlPoolOn() {
    static const int on = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('ctrlPool') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: ctrlPool=%d\n", v); return v; }();
    return on != 0;
}
}
bool PrepareQueuedGpuGraphicsDraw(const GraphicsDraw& draw, std::shared_ptr<GpuQueuedDraw>& queued, bool holdTargets) {
    queued.reset();
    const int state = ValidateGraphicsDraw(draw);
    if (state <= 0) return state == 0;
    const bool cpCount = skperfEncodeProbe.load(std::memory_order_relaxed) != 0; // [skperf] round 38: shared counters only while probing
    if (cpCount) skperfDrawPoolStats[0].fetch_add(1, std::memory_order_relaxed);
    const bool pooled = DrawPoolOn();
    const auto pooledDeleter = [](GpuQueuedDraw* item) { ReleasePooledDraw(item); };
    std::shared_ptr<GpuQueuedDraw> copy = !pooled ? std::make_shared<GpuQueuedDraw>()
        : CtrlPoolOn() ? std::shared_ptr<GpuQueuedDraw>(AcquirePooledDraw(), pooledDeleter, CpCtrlAlloc<GpuQueuedDraw>()) // [skperf] round 44
                       : std::shared_ptr<GpuQueuedDraw>(AcquirePooledDraw(), pooledDeleter);
    if (cpCount) skperfDrawPoolStats[1].fetch_add(1, std::memory_order_relaxed); // make_shared (old) or the shared_ptr control block (pooled)
    const std::size_t capBefore = copy->colors.capacity() + copy->vertices.capacity() + copy->groups.capacity() + copy->writes.capacity() + copy->writeBytes.capacity();
    copy->draw = draw;
    copy->colors.assign(draw.colors, draw.colors + draw.colorCount);
    copy->vertices.assign(draw.vertices, draw.vertices + draw.vertexCount);
    copy->groups.assign(draw.groups, draw.groups + draw.groupCount);
    std::size_t total = 0;
    for (std::size_t i = 0; i < draw.uniformWriteCount; ++i) total += draw.uniformWrites[i].size;
    copy->writeBytes.resize(total);
    copy->writes.assign(draw.uniformWrites, draw.uniformWrites + draw.uniformWriteCount);
    std::size_t at = 0;
    for (auto& write : copy->writes) {
        std::memcpy(copy->writeBytes.data() + at, write.bytes, write.size);
        write.bytes = copy->writeBytes.data() + at; at += write.size;
    }
    copy->draw.colors = copy->colors.data(); copy->draw.vertices = copy->vertices.data(); copy->draw.groups = copy->groups.data();
    if (holdTargets) copy->HoldTargetViews(); // [skperf] round 15 passHold: else the list holds the attachments
    copy->draw.uniformWrites = copy->writes.data();
    {   // [skperf] round 14: count vectors that had to grow (heap allocations) for this draw.
        const std::size_t capAfter = copy->colors.capacity() + copy->vertices.capacity() + copy->groups.capacity() + copy->writes.capacity() + copy->writeBytes.capacity();
        if (cpCount && capAfter != capBefore) skperfDrawPoolStats[2].fetch_add(1, std::memory_order_relaxed);
    }
    queued = std::move(copy);
    return true;
}
bool EncodeGpuTextureCopies(WGPUCommandEncoder encoder, const GpuTextureCopy* copies, std::size_t count) {
    if (!encoder || !ValidateGpuTextureCopies(copies, count)) return false;
    struct Request { WGPUCommandEncoder encoder; const GpuTextureCopy* copies; std::size_t count; bool issued=false; }
        request{encoder,copies,count};
    return OnMain([](void* argument) {
        auto& request = *static_cast<Request*>(argument);
        if (!MainThreadGpuDevice()) return;
        for (std::size_t i = 0; i < request.count; ++i)
            wgpuCommandEncoderCopyTextureToTexture(request.encoder, &request.copies[i].source,
                &request.copies[i].destination, &request.copies[i].extent);
        request.issued = true; // Issued, not GPU-completed or runtime-validated.
    }, &request) && request.issued;
}
bool EncodeGpuComputeDispatchOnMain(WGPUCommandEncoder encoder, WGPUComputePipeline pipeline, WGPUBindGroup group,
    std::uint32_t x, std::uint32_t y, std::uint32_t z, WGPUBuffer indirect, std::uint64_t indirectOffset) {
    // Same limit checks and the same single-dispatch pass as EncodeGpuComputeDispatch's Begin, without the
    // error scopes (encoder errors surface at finish, exactly as for batched graphics draws).
    if (!emscripten_is_main_runtime_thread() || !encoder || !pipeline || !group || (indirect && indirectOffset % 4)) return false;
    static std::uint64_t limitsGeneration = ~std::uint64_t(0);
    static WGPULimits limits = WGPU_LIMITS_INIT;
    const auto generation = GpuDeviceGeneration();
    if (limitsGeneration != generation) {
        limits = WGPU_LIMITS_INIT;
        if (!MainThreadGpuLimits(limits)) return false;
        limitsGeneration = generation;
    }
    if (!limits.maxBindGroups || (!indirect && (x > limits.maxComputeWorkgroupsPerDimension ||
        y > limits.maxComputeWorkgroupsPerDimension || z > limits.maxComputeWorkgroupsPerDimension))) return false;
    WGPUComputePassDescriptor descriptor = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    auto pass = wgpuCommandEncoderBeginComputePass(encoder, &descriptor);
    if (!pass) return false;
    wgpuComputePassEncoderSetPipeline(pass, pipeline);
    wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    if (indirect) wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, indirect, indirectOffset);
    else wgpuComputePassEncoderDispatchWorkgroups(pass, x, y, z);
    wgpuComputePassEncoderEnd(pass); wgpuComputePassEncoderRelease(pass);
    return true;
}
void ReleaseGpuBindEntriesOnMain(std::vector<WGPUBindGroupEntry>&& entries) {
    if (entries.empty()) return;
    auto release = [](void* argument) {
        auto* list = static_cast<std::vector<WGPUBindGroupEntry>*>(argument);
        for (const auto& e : *list) {
            if (e.buffer) wgpuBufferRelease(e.buffer);
            if (e.textureView) wgpuTextureViewRelease(e.textureView);
            if (e.sampler) wgpuSamplerRelease(e.sampler);
        }
        delete list;
    };
    auto* list = new std::vector<WGPUBindGroupEntry>(std::move(entries));
    if (emscripten_is_main_runtime_thread()) { release(list); return; }
    // Fire-and-forget: a worker never drops the last reference itself.
    if (auto* queue = CommandQueue(); !queue || !emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), release, list)) {
        static std::atomic<unsigned> leaks{0};
        if (leaks.fetch_add(1) < 4) std::fputs("CP2077 queued bind entries release proxy failed (references leaked)\n", stderr);
        delete list;
    }
}
bool ValidateGpuTextureCopies(const GpuTextureCopy* copies, std::size_t count) {
    if (count && !copies) return false;
    for (std::size_t i = 0; i < count; ++i) {
        if (!copies[i].source.texture || !copies[i].destination.texture ||
            !copies[i].extent.width || !copies[i].extent.height || !copies[i].extent.depthOrArrayLayers) return false;
        const auto& copy=copies[i];
        if(copy.source.texture==copy.destination.texture && copy.source.mipLevel==copy.destination.mipLevel) {
            auto overlap=[](std::uint32_t a,std::uint32_t b,std::uint32_t size){
                return std::uint64_t(a)<std::uint64_t(b)+size && std::uint64_t(b)<std::uint64_t(a)+size;
            };
            if(overlap(copy.source.origin.x,copy.destination.origin.x,copy.extent.width) &&
               overlap(copy.source.origin.y,copy.destination.origin.y,copy.extent.height) &&
               overlap(copy.source.origin.z,copy.destination.origin.z,copy.extent.depthOrArrayLayers))return false;
        }
    }
    return true;
}
#include "gpu-logic-xor-web.inl"

}
