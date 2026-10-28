#include "skperf-api-timer-web.h" // [skperf] round 39
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-api-command-web.h"
#include "gpu-command-web.h"
#include "engine-wgtrace-web.h"
#include "gpu-handoff-stats-web.h" // [skperf] round 5 frame probe: worker handoff totals
#include "owned-command-lease-web.h"
#include "gpu-read-only-depth-stencil-web.h"
#include "gpu-logic-xor-web.h"
#include "gpu-device-web.h"
#include "gpu-buffer-web.h"
#include "gpu-atomic-texture-buffer-web.h"
#include "gpu-compute-web.h"
#include "gpu-api-shader-web.h"
#include "gpu-api-texture-web.h"
#include "gpu-api-buffer-web.h"
#include "gpu-shader-web.h"
#include "gpu-uniform-web.h"
#include "gpu-texture-web.h"
#include "gpu-api-om-state-web.h"
#include "gpu-api-vertex-web.h"
#include "gpu-api-sampler-web.h"
#include "common/renderData/include/pipelineStateObject.h"
#include "../../cp2077SourceCode/extracted/root/R6.Root/Mainline/engine/shaders/include/globalConstantsVS.fx"
#include "../../cp2077SourceCode/extracted/root/R6.Root/Mainline/engine/shaders/include/globalConstantsPS.fx"
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstring>
#include <cmath> // [whitewall] r4 exposure probe (ldexp)
#include <condition_variable>
#include <map>
#include <unordered_set>
#include <set>
#include <unordered_set>
#include <string>
#include <optional>
#include <memory>
#include <deque>
#include <pthread.h>

namespace cp2077::web { extern std::atomic<unsigned long long> skperfReplayStats[4], skperfWriteStats[3]; } // [skperf] round 9
namespace cp2077::web { extern std::atomic<unsigned long long> skperfPassStats[11], skperfIssued[9]; } // [skperf] round 12
namespace cp2077::web { extern std::atomic<unsigned long long> skperfHoistStats[3]; } // [skperf] round 13
namespace cp2077::web { extern std::atomic<unsigned long long> skperfDrawPoolStats[3]; } // [skperf] round 14
namespace cp2077::web { extern std::atomic<unsigned long long> skperfMainDrawUs[2]; } // [skperf] round 15
extern "C" int cp2077_skperf_crit_on(); // [skperf] round 22 (render-node-cost-web.inl)
extern "C" void cp2077_skperf_crit_submit(double start, double end, double orderWaitMs, double scanMs);
extern "C" void cp2077_skperf_crit_scan_parts(double idsMs, double searchMs, double flushMs); // [skperf] round 25
namespace cp2077::web { extern std::atomic<int> skperfEncodeProbe; extern std::atomic<unsigned long long> skperfEncodeStats[8];
    std::atomic<unsigned long long> skperfBinkCalls[16], skperfBinkUs[16]; /* [skperf] round 7: bink-engine-web.cpp */ } // [skperf] gpu-command-web.cpp
namespace {
// A compute dispatch queued in encoder order (perf7). group is a worker bind-group-cache hit (owned by the
// command's retainedBindGroups); otherwise entries (one reference held on every handle since queueing) are turned
// into a group on the main thread at encode time, then released. created is that group; FlushPendingBufferCopies
// moves it into retainedBindGroups.
struct QueuedComputeDispatch {
    WGPUComputePipeline pipeline = nullptr;
    WGPUBindGroup group = nullptr, created = nullptr;
    std::vector<WGPUBindGroupEntry> entries;
    bool entriesHeld = false;
    WGPUCommandEncoder owner = nullptr;
    std::uint32_t x = 0, y = 0, z = 0;
    WGPUBuffer indirect = nullptr;
    std::uint64_t indirectOffset = 0;
    void HoldEntries() {
        if (entriesHeld) return;
        for (const auto& e : entries) {
            if (e.buffer) wgpuBufferAddRef(e.buffer);
            if (e.textureView) wgpuTextureViewAddRef(e.textureView);
            if (e.sampler) wgpuSamplerAddRef(e.sampler);
        }
        entriesHeld = true;
    }
    void ReleaseEntries() { if (entriesHeld) { entriesHeld = false; cp2077::web::ReleaseGpuBindEntriesOnMain(std::move(entries)); } entries.clear(); }
    ~QueuedComputeDispatch() { ReleaseEntries(); }
};
struct SharedConvRead; // [skperf] shareConv, defined with namespace SharedConv
namespace SharedConv { struct Conv; }
struct CommandEntry {
    struct SamplerBinding {
        GpuApi::SamplerStateRef reference = GpuApi::SamplerStateRef::Null();
        WGPUSampler sampler = nullptr;
    };
    struct TextureBinding {
        GpuApi::TextureRef reference = GpuApi::TextureRef::Null();
        WGPUTextureView view = nullptr;
        bool nativeNull = false;
        bool plain = false; // [cmdlock] default sampled view bound by BindTextures (not a stencil/mip-selected view)
    };
    struct BufferBinding {
        GpuApi::BufferRef reference = GpuApi::BufferRef::Null();
        WGPUBuffer buffer = nullptr;
        std::uint64_t bytes = 0;
        std::uint32_t stride = 0;
        WGPUBuffer counter = nullptr;
    };
    struct ConstantSnapshot {
        std::vector<unsigned char> bytes;
        bool locked = false;
        bool own = false; // [flicker3] written by THIS list (Lock/Unlock): draws after it read this value
    };
    struct BufferUploadLock {
        std::vector<unsigned char> bytes;
        WGPUBuffer destination = nullptr;
        std::uint32_t offset = 0;
        bool locked = false;
        bool drop = false; // [gamerender] unrepresentable region: CPU scratch only, unlock uploads nothing
    };
    BufferBinding indirectArguments;
    Uint32 indirectOffset = 0;
    WGPUCommandEncoder encoder;
    GpuApi::CommandListType type;
    std::thread::id owner;
    std::vector<GpuApi::BufferRef> retainedBuffers;
    std::vector<WGPUBuffer> uploads;
    std::vector<cp2077::web::GpuOrderedCommand> pendingBufferCopies;
    // Staged upload bytes for copies queued since the last flush; their copy source is filled in at flush time
    // with one pooled arena buffer (see StagingArena). Arenas return to the pool at GPU retirement.
    std::vector<unsigned char> stagingBytes;
    std::vector<std::size_t> stagedOps;
    // perf12: staging arena bound on the submitting worker for the asynchronous submit (creating an arena needs a
    // worker); the main-thread job only writes the bytes. Its lease is already in stagingArenas.
    WGPUBuffer preparedStaging = nullptr;
    // Compute dispatches queued since the last flush (perf7); see QueuedComputeDispatch.
    std::vector<std::shared_ptr<QueuedComputeDispatch>> queuedDispatches;
    struct StagingArenaLease { WGPUBuffer buffer; std::uint64_t bytes; std::uint64_t generation; WGPUBufferUsage usage; };
    std::vector<StagingArenaLease> stagingArenas;
    // Per-draw uniform payloads suballocated from pages of the persistent uniform ring (leases in uniformPages),
    // or from a pooled Uniform|CopyDst arena when the ring is exhausted (leases in stagingArenas). base is the
    // block's byte offset inside buffer; bytes[i] lands at buffer offset base+i. Bytes are gathered on the CPU
    // and written once per block just before this command list is submitted.
    struct UniformBlock { WGPUBuffer buffer = nullptr; std::uint64_t base = 0, capacity = 0; std::vector<unsigned char> bytes; };
    std::vector<UniformBlock> uniformBlocks;
    struct UniformPageLease { std::uint32_t page; std::uint64_t generation; };
    std::vector<UniformPageLease> uniformPages;
    // [flicker3] Arena slots of ordered (Dynamic_Legacy) constant buffers this list read before writing them itself.
    // Their bytes are the value as of this list's SUBMISSION (ResolveOrderedConstants), as native DX12 reads the
    // buffer at GPU time after every earlier submitted Unlock copy. One slot per (buffer, layout) per list.
    struct DeferredConstantSlot { std::uint32_t block; std::uint64_t local; const cp2077::web::UniformLayout* layout; Uint32 ref; };
    std::vector<DeferredConstantSlot> deferredConstants;
    std::map<std::pair<Uint32, const void*>, std::pair<WGPUBuffer, std::uint64_t>> deferredConstantSlots;
    // System-draw (UI text, debug geometry) vertex/index payloads suballocated from pooled engine buffers, one
    // pool per buffer category because the category selects vertex usage and index format. Same write-at-submit
    // and return-at-retirement rules as uniform blocks.
    struct SystemDrawBlock { GpuApi::BufferRef reference; WGPUBuffer buffer; GpuWrapApi::eBufferChunkCategory category;
        std::uint32_t capacity; std::vector<unsigned char> bytes; };
    std::vector<SystemDrawBlock> systemDrawBlocks;
    std::vector<std::shared_ptr<void>> retainedQueryPools;
    bool failed = false;
    const char* failedOperation = nullptr; int failedLine = 0; // first failure site, for diagnostics
    // Diagnostics only: own list id (bound.ID() is wrong for failures at submit) and per-list draw/dispatch outcomes.
    Uint32 id = 0;
    bool touchedPreview = false; // a 542x1280 (character preview RTT) target was bound at some point
    Uint32 drawsOk = 0, drawsSkipped = 0, drawsRejected = 0, drawsDroppedFailed = 0;
    Uint32 previewDrawsOk = 0, previewDrawsSkipped = 0, dispatchesSkipped = 0;
    Uint32 unavailableComputeShader = 0; // last SetShader(compute) ref id whose pipeline was unavailable
    std::unordered_map<WGPUTextureView,cp2077::web::GpuLogicXorScratch> logicXorScratch;
    // [skperf] round 16 xorBatch: the open run of queued XOR draws (same scratch, same destination): its resolve op
    // is the one a following XOR draw of the run replaces.
    struct XorRun { const void* scratch = nullptr; WGPUTextureView destination = nullptr; std::shared_ptr<cp2077::web::GpuQueuedDraw> resolve; } xorRun;
    Uint32 debugGroupDepth = 0;
    Uint64 completionFence = 0;
    GpuApi::ShaderRef computeShader = GpuApi::ShaderRef::Null();
    struct GraphicsShaderBinding {
        GpuApi::ShaderRef reference = GpuApi::ShaderRef::Null();
        WGPUShaderModule module = nullptr;
        const cp2077::web::ConvertedGraphicsShader* source = nullptr;
        // Set when the original shader has no WGSL conversion: draws using it are skipped instead of
        // failing the whole command list (which dropped every other draw recorded in it).
        bool unavailable = false;
    };
    std::array<GraphicsShaderBinding, 2> graphicsShaders;
    std::array<std::array<SamplerBinding, GpuApi::MAX_SAMPLER_STATES>, 2> graphicsSamplers;
    std::array<std::array<TextureBinding, GpuApi::MAX_SRV_BINDINGS>, 2> graphicsTextures;
    std::array<TextureBinding, GpuApi::MAX_UAV_BINDINGS> pixelStorageTextures;
    std::array<std::array<BufferBinding, GpuApi::MAX_SRV_BINDINGS>, 2> graphicsBuffers;
    std::array<std::array<GpuApi::BufferRef, GpuApi::MAX_CONSTANT_BUFFERS>, 2> graphicsConstantBuffers{};
    // [skperf] round 11 cbReuse: the last constant-buffer bind of each (stage, slot) in THIS list. A later draw whose
    // repacked bytes come from the same per-thread pack-cache slot at the same generation (i.e. byte-identical, proven
    // by the cache's memcmp) for the same layout/binding/dynamic-ness reuses the same uniform location instead of a
    // new arena slot + copy. Lives in the CommandEntry, so it starts empty for every list.
    struct CbMemo { const void* packSlot = nullptr; unsigned packGen = 0; const void* layout = nullptr; std::uint32_t binding = 0;
        bool dynamic = false; WGPUBuffer buffer = nullptr; std::uint64_t offset = 0, size = 0; };
    std::array<std::array<CbMemo, GpuApi::MAX_CONSTANT_BUFFERS>, 2> cbMemo{};
    std::array<std::vector<unsigned char>, 2> graphicsNativeConstants;
    std::array<Float, NUM_VSC_REGS * 4> vertexLegacyConstants{};
    std::array<Float, NUM_PSC_REGS * 4> pixelLegacyConstants{};
    std::array<bool, NUM_VSC_REGS> vertexLegacyInitialized{};
    std::array<bool, NUM_PSC_REGS> pixelLegacyInitialized{};
    Uint8 legacyConstantsDirty = 0;
    GpuApi::VertexLayoutRef vertexLayout = GpuApi::VertexLayoutRef::Null();
    struct GeometryBufferBinding {
        GpuApi::BufferRef reference = GpuApi::BufferRef::Null();
        WGPUBuffer buffer = nullptr;
        Uint32 offset = 0, bytes = 0;
        Uint8 stride = 0;
        WGPUIndexFormat indexFormat = WGPUIndexFormat_Undefined;
    };
    std::array<GeometryBufferBinding, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> vertexBuffers;
    GeometryBufferBinding indexBuffer;
    GpuApi::OMStateRef outputMergerRef = GpuApi::OMStateRef::Null();
    SOMState outputMergerState;
    Uint8 stencilReference = 0;
    bool depthClip = true;
    WGPUColor blendFactor{1.0, 1.0, 1.0, 1.0};
    struct RenderAttachment {
        GpuApi::TextureRef reference = GpuApi::TextureRef::Null();
        WGPUTextureView view = nullptr;
        WGPUTextureFormat format = WGPUTextureFormat_Undefined;
        Uint32 width = 0, height = 0, samples = 0;
    };
    std::array<RenderAttachment, MAX_RENDERTARGETS> colorAttachments;
    RenderAttachment depthAttachment;
    // [skperf] round 15 passHold: attachments swapped out by SetupRenderTargets, kept (view + reference) until this
    // list retires, so queued draws that still point at them need no references of their own.
    std::vector<RenderAttachment> heldAttachments;
    Uint32 colorAttachmentCount = 0;
    bool depthReadOnly = false;
    GpuApi::ViewportDesc viewport;
    bool viewportSet = false;
    Rect scissors{};
    bool scissorsSet = false;
    WGPUComputePipeline computePipeline = nullptr;
    std::vector<unsigned char> computeNativeConstants;
    std::array<GpuApi::BufferRef, GpuApi::MAX_CONSTANT_BUFFERS> computeConstantBuffers{};
    std::unordered_map<Uint32, ConstantSnapshot> constantSnapshots;
    std::unordered_map<Uint32, BufferUploadLock> bufferUploadLocks;
    std::array<SamplerBinding, GpuApi::MAX_SAMPLER_STATES> computeSamplers;
    std::vector<GpuApi::ShaderRef> retainedShaders;
    std::vector<GpuApi::SamplerStateRef> retainedSamplers;
    std::vector<WGPURenderPipeline> retainedRenderPipelines;
    std::vector<WGPUBindGroup> retainedBindGroups;
    std::vector<GpuApi::TextureRef> retainedTextures;
    // [skperf] round 18 texRetain: textures this list already retained for a sampled SRV (id -> descriptor). The list
    // holds that one reference until retirement (it is in retainedTextures), which covers every later draw of the list.
    std::unordered_map<Uint32, const GpuApi::TextureDesc*> textureRetainOnce;
    // [skperf] round 42 drawRetain: buffers this list already retained for a draw (vertex/index/indirect/storage SRV);
    // the one reference is in retainedBuffers until retirement. Render/depth attachments use textureRetainOnce.
    std::unordered_set<Uint32> bufferRetainOnce;
    // [skperf] round 45 uniMemo: immutable-uniform cache hits of this list (key = cache key) -> buffer. The list owns one
    // lease per buffer (in uploads, released at retirement), so later draws of the list with the same payload reuse it.
    std::unordered_map<std::string, WGPUBuffer> uniformMemo;
    // [skperf] round 63 viewMemo: known (format, dimension) of the engine's own SRV views bound in this list (those views
    // stay alive for the list: the texture is retained by it), so the per-draw view-info registry lookup (sharded mutex)
    // runs once per view per list.
    std::unordered_map<WGPUTextureView, std::pair<WGPUTextureFormat, WGPUTextureViewDimension>> viewInfoMemo;
    // [skperf] round 45 biasMemo: last SamplerBias payload per stage, keyed by the 16 bound (sampler ref, native) pairs;
    // only stored when every value came from the list-constant samplerOnce cache.
    struct BiasMemo { bool valid = false; Uint32 refs[16] = {}; WGPUSampler natives[16] = {}; float bias[16] = {}; };
    BiasMemo biasMemo[2];
    std::unordered_map<std::uint64_t, std::pair<WGPUShaderModule, const void*>> shaderRetainOnce; // (id << 1 | stage) -> module, ConvertedGraphicsShader
    // Samplers: (reference, bound native snapshot) -> its effective bias (GetEffectiveGpuSamplerBias, global sampler
    // mutex, 16x per SamplerBias binding) and the one list-level AcquireGpuSampler (released at retirement).
    struct SamplerOnce { Uint32 ref = 0; WGPUSampler native = nullptr; bool biasOk = false; float bias = 0; signed char acquired = -1; WGPUSampler sampler = nullptr; };
    std::unordered_map<std::uint64_t, SamplerOnce> samplerOnce;
    struct DepthFloatCompatibilityView {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        // perf11: nonzero poolKind (kind + 1) = obtained from / returnable to Perf11Pools.
        Uint32 poolKind = 0, poolWidth = 0, poolHeight = 0;
        std::uint64_t poolGeneration = 0;
    };
    // Command-local copies cannot race another list's refresh. Their native
    // source references and GPU destinations survive until queue completion.
    std::unordered_map<Uint32, DepthFloatCompatibilityView> depthFloatCompatibilityViews;
    // [npcblack] Depth16Unorm array copies of R16Unorm-backed D16U UAV textures read through depth SRVs
    // (character/NPC shadow comparison). Converted once per command list at the first read (pooled, perf11 kind 100+layers).
    std::unordered_map<Uint32, DepthFloatCompatibilityView> r16DepthViews;
    // [skperf] shareConv: shared r16depth / depth-float conversions this list reads (converted once per source write,
    // at submit in queue order, by a prefix encoder submitted right before this list), and that prefix work.
    std::vector<std::shared_ptr<SharedConvRead>> sharedReads;
    std::vector<cp2077::web::GpuOrderedCommand> sharedPrefix;
    std::vector<std::shared_ptr<SharedConv::Conv>> sharedPrefixConvs;
    // [skperf] listGen: in-list write generations. localWriteGen counts draws that wrote a texture (depth writes,
    // pixel UAVs) and dispatches (storage UAVs); depthCopyAt = (list clear/copy generation + 1, localWriteGen) at the
    // last per-list depth-float refresh, so a read with no write since then reuses the refreshed copy.
    std::unordered_map<Uint32, Uint32> localWriteGen;
    std::unordered_map<Uint32, std::pair<Uint32, Uint32>> depthCopyAt;
    unsigned fallbackReads = 0;
    DepthFloatCompatibilityView nativeNullIntegerView;
    DepthFloatCompatibilityView nativeNullEyeVolumeView;
    DepthFloatCompatibilityView nativeNullEyeArrayFloatView;
    DepthFloatCompatibilityView nativeNullEyeArrayUintView;
    struct AtomicTextureTransfer {
        cp2077::web::GpuAtomicTextureBuffer buffer;
        WGPUTexture source=nullptr;
        std::uint64_t generation=0; // perf11: device generation the buffer was created/pooled under
    };
    std::unordered_map<Uint32,AtomicTextureTransfer> atomicTextureTransfers;
    std::array<TextureBinding, GpuApi::MAX_SRV_BINDINGS> computeTextures;
    std::array<BufferBinding, GpuApi::MAX_SRV_BINDINGS> computeBuffers;
    std::array<BufferBinding, GpuApi::MAX_UAV_BINDINGS> computeStorageBuffers;
    std::array<TextureBinding, GpuApi::MAX_UAV_BINDINGS> computeStorageTextures;
};
// [cmdlock] Uncapped per-frame counters (relaxed atomics), reported from BeginRender every 300 frames:
// 0 BindTextures no-op skips, 1 BindTextures full commits, 2 sampled-view front hits, 3 front misses,
// 4 draws, 5 commandMutex contended acquisitions, 6 commandMutex wait (us), 7/8 queued-draw bind group hit/miss.
std::atomic<unsigned long long> cmdlockCounters[10];
std::mutex commandMutex;
OwnedCommandOperations commandOperations;
Uint64 commandShutdownGeneration=0;
#include "gpu-command-lock-cost-web.inl"
std::unordered_map<Uint32, CommandEntry> commands;
Uint32 nextCommandId = 1;
thread_local GpuApi::CommandListRef bound = GpuApi::CommandListRef::Null();
// [cmdlock] Lock-free access to this thread's own bound list. A bound (owned) entry is mutated only by its owner;
// submission rejects owned lists and only teardown (ShutdownGpuCommands) can erase it, after BeginClose has waited
// for every lease. So the owner caches the node pointer at BindCommandList (unordered_map node addresses survive
// rehash) and later state calls take an atomic lease instead of commandMutex: increment active, then re-check
// closing and the registry epoch (bumped by teardown). seq_cst: a lease either precedes teardown's wait (teardown
// waits for it) or sees closing and falls back to the locked lookup. ?cmdFast=0 disables (A/B).
struct BoundEntryCache { std::pair<const Uint32, CommandEntry>* node = nullptr; Uint32 id = 0; unsigned epoch = 0; };
thread_local BoundEntryCache tlBoundEntry;
std::atomic<unsigned> commandRegistryEpoch{1};
int CmdFastEnabled() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('cmdFast') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 cmdlock switches: cmdFast=%d\n", v); return v; }();
    return value;
}
void ReleaseFastCommandLease() {
    if (commandOperations.active.fetch_sub(1) == 1 && commandOperations.closing.load()) {
        std::lock_guard<std::mutex> guard(commandMutex); commandOperations.idle.notify_all();
    }
}
// Iterator stand-in so converted call sites keep `entry == commands.end()` / `entry->second`.
struct BoundIter {
    std::pair<const Uint32, CommandEntry>* node = nullptr;
    std::pair<const Uint32, CommandEntry>* operator->() const { return node; }
    bool operator==(const std::unordered_map<Uint32, CommandEntry>::iterator&) const { return node == nullptr; }
    bool operator!=(const std::unordered_map<Uint32, CommandEntry>::iterator& end) const { return !(*this == end); }
};
class CpBoundCommand {
    bool fastLease = false;
    std::optional<CpCommandLock> lock;
    const char* function; const char* file; unsigned line;
public:
    CpBoundCommand(const char* name, const char* source, unsigned location) : function(name), file(source), line(location) {}
    ~CpBoundCommand() { lock.reset(); if (fastLease) ReleaseFastCommandLease(); }
    BoundIter Find() {
        const auto& cache = tlBoundEntry;
        if (cache.node && !bound.isNull() && cache.id == bound.ID() && CmdFastEnabled()) {
            commandOperations.active.fetch_add(1);
            if (!commandOperations.closing.load() && cache.epoch == commandRegistryEpoch.load() &&
                cache.node->second.owner == std::this_thread::get_id()) {
                fastLease = true; return BoundIter{cache.node};
            }
            ReleaseFastCommandLease();
        }
        lock.emplace(commandMutex, function, file, line);
        auto found = commands.find(bound.ID());
        if (found == commands.end() || found->second.owner != std::this_thread::get_id()) return BoundIter{};
        return BoundIter{&*found};
    }
    bool Locked() const { return lock.has_value(); }
    void UnlockForOwnedOperation() { if (lock) lock->UnlockForOwnedOperation(); }
};
EM_JS(void, CaptureFirstCommandFailure, (Uint32 id, Uint32 type, const char* operation, Int32 line), {
    const record = {id, type, operation: UTF8ToString(Number(operation)), line,
        timestamp: new Date().toISOString(), stack: new Error('Original GPU command failure').stack};
    if (ENVIRONMENT_IS_PTHREAD) self.postMessage({cmd:'cp2077GpuFailureTrace', record});
    else {
        const traces = Module['cp2077GpuFailureTraces'] ||= [];
        if (traces.length < 8) traces.push(record);
    }
});
void MarkBrowserCommandFailed(CommandEntry& command, const char* operation, int line) {
    static std::atomic<unsigned> reported{0};
    // Besides the first four failures, report the first occurrence of every distinct failing site so a new
    // failure mode later in a session is never hidden behind early loading failures.
    static std::mutex sitesMutex; static std::set<std::pair<std::string,int>> sites;
    bool newSite = false;
    if (!command.failed) { std::lock_guard<std::mutex> guard(sitesMutex); newSite = sites.size() < 64 && sites.emplace(operation ? operation : "", line).second; }
    if (!command.failed && newSite && reported.load() >= 4)
        emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK, "CP2077 new command failure site: id=%u type=%u operation=%s line=%d",
            bound.ID(), unsigned(command.type), operation ? operation : "", line);
    if (!command.failed && reported.fetch_add(1) < 4) {
        CaptureFirstCommandFailure(bound.ID(), unsigned(command.type), operation, line);
        emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,
            "CP2077 first direct command failure: id=%u type=%u operation=%s line=%d",
            bound.ID(), unsigned(command.type), operation, line);
    }
    if (!command.failed) {
        // First failure of every list (one line per list by construction): all lists up to 2000 lines,
        // then only lists that bound the character preview target, up to 20000 lines in total.
        static std::atomic<unsigned> listFailuresLogged{0};
        const unsigned logged = listFailuresLogged.load();
        if (logged < 20000 && (logged < 2000 || command.touchedPreview) && listFailuresLogged.fetch_add(1) < 20000)
            std::fprintf(stderr, "CP2077 list failure: list=%u type=%u operation=%s line=%d color0=%ux%u depth=%ux%u preview=%u drawsOk=%u\n",
                command.id, unsigned(command.type), operation ? operation : "", line,
                command.colorAttachmentCount ? command.colorAttachments[0].width : 0u, command.colorAttachmentCount ? command.colorAttachments[0].height : 0u,
                command.depthAttachment.width, command.depthAttachment.height, unsigned(command.touchedPreview), command.drawsOk);
    }
    if (!command.failed) { command.failedOperation = operation; command.failedLine = line; }
    command.failed = true;
}
// Character preview render-to-texture size (either orientation).
bool IsPreviewTargetSize(Uint32 width, Uint32 height) {
    return (width == 542 && height == 1280) || (width == 1280 && height == 542);
}
enum class BoundDrawOutcome { Ok, Skipped, Rejected, DroppedFailed };
// Per-list draw accounting for diagnostics. DrawBoundGpuGraphics calls this with its own `command`
// reference on the owning thread (after UnlockForOwnedOperation the owner mutates the list exclusively),
// so it takes no lock and never looks up `bound`.
[[maybe_unused]] void NoteBoundDraw(CommandEntry& command, BoundDrawOutcome outcome) {
    const bool preview = command.colorAttachmentCount &&
        IsPreviewTargetSize(command.colorAttachments[0].width, command.colorAttachments[0].height);
    switch (outcome) {
    case BoundDrawOutcome::Ok: ++command.drawsOk; if (preview) ++command.previewDrawsOk; break;
    case BoundDrawOutcome::Skipped: ++command.drawsSkipped; if (preview) ++command.previewDrawsSkipped; break;
    case BoundDrawOutcome::Rejected: ++command.drawsRejected; break;
    case BoundDrawOutcome::DroppedFailed: ++command.drawsDroppedFailed; break;
    }
}

std::mutex renderFrameMutex;
Uint32 renderFrameIndex = 0;
bool insideRenderFrame = false;
// perf10: lock-free copy of renderFrameIndex, published by BeginRender, for per-dispatch uniform admission.
std::atomic<Uint32> publishedRenderFrame{0};
// perf10 URL switches for A/B: ?fullMappedFlush=1 restores the full mapped-buffer scan on every submit;
// ?uniformAdmitSameFrame=1 restores immutable-uniform admission after 3 sightings regardless of frame.
// Read without holding any lock (BeginRender, CloseAndSubmitCommandLists); concurrent first reads are idempotent.
constexpr bool kCbOrderDefault = true; // [flicker3] on by default (verified: floor/world flicker 16.6/min -> 0); ?cbOrder=0 = old
bool CbOrderOnFor(int v) { return (v & 8) ? true : (v & 4) ? false : kCbOrderDefault; }
std::atomic<int> perf10Switches{-1};
int Perf10Switches() {
    int value = perf10Switches.load(std::memory_order_relaxed);
    if (value < 0) {
        value = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search);
            return (q.get('fullMappedFlush') === '1' ? 1 : 0) | (q.get('uniformAdmitSameFrame') === '1' ? 2 : 0) | (q.get('cbOrder') === '0' ? 4 : 0) | (q.get('cbOrder') === '1' ? 8 : 0);
        });
        perf10Switches.store(value, std::memory_order_relaxed);
        std::fprintf(stderr, "CP2077 perf10 switches: fullMappedFlush=%d uniformAdmitSameFrame=%d cbOrder=%d\n", value & 1, (value >> 1) & 1, CbOrderOnFor(value) ? 1 : 0);
    }
    return value;
}
// [flicker3] submission-ordered constant buffers (?cbOrder=0 = old bind-time CPU snapshot). Before the first
// Perf10Switches() read (first BeginRender/submit) kCbOrderDefault applies; never proxies itself.
bool CbOrderOn() { const int v = perf10Switches.load(std::memory_order_relaxed); return v < 0 ? kCbOrderDefault : CbOrderOnFor(v); }
std::atomic<unsigned> cbOrderDeferred{0}, cbOrderReused{0}, cbOrderUnresolved{0}, cbOrderCommits{0};
void ReportCmdlockCounters() {
    static unsigned long long last[10]{}; static double lastAt = 0; static unsigned frames = 0;
    if (++frames % 300) return;
    const double now = emscripten_get_now();
    unsigned long long cur[10]; for (int i = 0; i < 10; ++i) cur[i] = cmdlockCounters[i].load(std::memory_order_relaxed);
    const double f = 300.0;
    std::fprintf(stderr, "CP2077 cmdlock counters: frames=%u msPerFrame=%.1f perFrame bindSkip=%.0f bindFull=%.0f viewFrontHit=%.0f viewFrontMiss=%.0f draws=%.0f lockContended=%.0f lockWaitMs=%.2f bgHit=%.0f bgMiss=%.0f\n",
        frames, lastAt ? (now - lastAt) / f : 0.0, (cur[0]-last[0])/f, (cur[1]-last[1])/f, (cur[2]-last[2])/f, (cur[3]-last[3])/f,
        (cur[4]-last[4])/f, (cur[5]-last[5])/f, (cur[6]-last[6])/f/1000.0, (cur[7]-last[7])/f, (cur[8]-last[8])/f);
    for (int i = 0; i < 10; ++i) last[i] = cur[i]; lastAt = now;
}
// Default before the first read (only dispatches recorded before the first BeginRender): the new policies.
bool Perf10Switch(int bit) { const int value = perf10Switches.load(std::memory_order_relaxed); return value > 0 && (value & bit); }
// perf12 URL switch for A/B: ?asyncSubmit=0 restores the synchronous CloseAndSubmitCommandLists (flush, encode and
// submit through blocking main-thread handoffs); ?asyncSubmit=wait runs the deferred job path but the submitter waits
// for its own job (bisection aid). Read once, before any lock is taken. Returns 0 sync, 1 async, 2 wait.
// Bisection sub-switches for the deferred path: ?p12flush=worker flushes (encode/uniform/staging) on the submitter as
// before and leaves only finish/submit to the job; ?p12mapped=sync uploads mapped buffers through the old synchronous
// handoff. Bit 1 = flush on worker, bit 2 = synchronous mapped flush.
int Perf12SubSwitches() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ const q = new URLSearchParams(location.search);
            return (q.get('p12flush') === 'worker' ? 1 : 0) | (q.get('p12mapped') === 'sync' ? 2 : 0) | (q.get('p12early') === '1' ? 4 : 0); });
        std::fprintf(stderr, "CP2077 perf12 sub-switches: flushOnWorker=%d mappedSync=%d asyncFromFirstFrame=%d\n", v & 1, (v >> 1) & 1, (v >> 2) & 1);
        return v;
    }();
    return value;
}
int Perf12SubmitMode() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ const s = new URLSearchParams(location.search).get('asyncSubmit');
            return s === '0' ? 0 : s === 'wait' ? 2 : 1; });
        std::fprintf(stderr, "CP2077 perf12 switches: asyncSubmit=%d (0 sync, 1 async, 2 wait)\n", v);
        return v;
    }();
    return value;
}
// Compute uniform admission to the immutable cache (perf10). The old rule (ShouldCacheGpuUniform: third sighting)
// admitted payloads repeated 3+ times inside ONE frame and never seen again (per-frame constants): ~80 admissions/s
// with the cache pinned at its 8192-entry cap, each admission a validated buffer creation plus an LRU eviction
// (destroy + bind-group invalidation on main). Now a payload is admitted only once it has been seen in two distinct
// render frames; first-frame repeats use this list's uniform ring like first sightings. Bytes delivered are identical.
// 64K slots indexed by an FNV-1a hash; each slot packs a 24-bit content tag, the low 32 bits of the last frame
// that counted, and the distinct-frame count. A tag collision only admits a payload early (keys stay exact).
std::atomic<unsigned> uniformSameFrameDeferred{0};
bool ShouldAdmitComputeUniform(std::size_t bytes, const void* data) {
    if (Perf10Switch(2)) return cp2077::web::ShouldCacheGpuUniform(bytes, data);
    static auto* const seen = new std::atomic<std::uint64_t>[1u << 16]();
    std::uint64_t hash = 1469598103934665603ull ^ bytes;
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    auto& slot = seen[hash & 0xFFFF];
    const std::uint64_t tag = (hash >> 40) << 40;
    const std::uint64_t frame = publishedRenderFrame.load(std::memory_order_relaxed);
    const auto previous = slot.load(std::memory_order_relaxed);
    const bool same = (previous & ~((std::uint64_t(1) << 40) - 1)) == tag;
    std::uint64_t count = same ? (previous & 0xFF) : 0;
    const std::uint64_t lastFrame = (previous >> 8) & 0xFFFFFFFFull;
    if (!same || !count || lastFrame != frame) {
        if (count < 255) ++count;
        slot.store(tag | (frame << 8) | count, std::memory_order_relaxed);
    } else if (count < 2) ++uniformSameFrameDeferred;
    return count >= 2;
}
std::mutex fenceMutex;
std::condition_variable submissionsRetired;
Uint64 nextFenceToken = 1;
Uint64 nextSubmissionToken = 1;
std::set<Uint64> pendingSubmissions;
// Merge completed intervals instead of treating the highest token as complete:
// lists can be created and submitted in a different order.
std::map<Uint64, Uint64> completedFences;
// Pooled staging arenas for small per-command uploads. Previously every buffer/constant unlock created its own
// CopySrc staging buffer: a synchronous main-thread allocation that waited for three error-scope promises
// (~0.6 ms each, ~100 per menu frame) plus a main-thread destroy at retirement. An arena is only reused after
// the command that used it has retired on the GPU, so its old contents are never still being read; new bytes
// are written with queue.writeBuffer, which executes before the command list that encodes copies from it.
namespace StagingArena {
std::mutex mutex;
struct Pooled { WGPUBuffer buffer; std::uint64_t bytes; std::uint64_t generation; WGPUBufferUsage usage; };
std::vector<Pooled> pool;
std::uint64_t pooledBytes = 0;
constexpr std::uint64_t kMaxPooledBytes = 32ull << 20, kMinArenaBytes = 64ull << 10;
constexpr WGPUBufferUsage kStagingUsage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
CommandEntry::StagingArenaLease Acquire(std::uint64_t needed, WGPUBufferUsage usage = kStagingUsage) {
    const auto generation = cp2077::web::GpuDeviceGeneration();
    std::vector<WGPUBuffer> stale;
    CommandEntry::StagingArenaLease lease{nullptr, 0, generation, usage};
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t best = pool.size();
        for (std::size_t i = 0; i < pool.size(); ++i) {
            if (pool[i].generation != generation || pool[i].usage != usage) continue;
            if (pool[i].bytes >= needed && (best == pool.size() || pool[i].bytes < pool[best].bytes)) best = i;
        }
        if (best != pool.size()) {
            lease = {pool[best].buffer, pool[best].bytes, generation, usage};
            pooledBytes -= pool[best].bytes; pool.erase(pool.begin() + best);
        }
        for (auto it = pool.begin(); it != pool.end();) {
            if (it->generation != generation) { stale.push_back(it->buffer); pooledBytes -= it->bytes; it = pool.erase(it); }
            else ++it;
        }
    }
    for (auto buffer : stale) cp2077::web::DestroyGpuBuffer(buffer);
    if (lease.buffer) return lease;
    std::uint64_t capacity = kMinArenaBytes;
    while (capacity < needed) capacity <<= 1;
    // A fresh arena is validated exactly like every other allocation.
    lease.buffer = cp2077::web::CreateGpuBuffer(capacity, usage, nullptr, 0,
        (usage & WGPUBufferUsage_Uniform) ? "cp-uniform-arena" : "cp-staging-arena");
    lease.bytes = lease.buffer ? capacity : 0;
    return lease;
}
void Release(const CommandEntry::StagingArenaLease& lease) {
    if (!lease.buffer) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (lease.generation == cp2077::web::GpuDeviceGeneration() && pooledBytes + lease.bytes <= kMaxPooledBytes) {
            pool.push_back({lease.buffer, lease.bytes, lease.generation, lease.usage}); pooledBytes += lease.bytes; return;
        }
    }
    cp2077::web::DestroyGpuBuffer(lease.buffer);
}
}
// Pooled engine buffers backing system draws. Previously every DrawSystem*Primitive created an immutable engine
// vertex buffer and index buffer (~63 validated creations plus destroys per menu frame). A pooled block holds the
// pool's own reference; draws retain theirs as before. It returns to the pool only when its command retires.
namespace SystemDrawArena {
std::mutex mutex;
std::vector<CommandEntry::SystemDrawBlock> pool;
constexpr std::uint32_t kBlockBytes = 256u << 10, kMaxPooledBlocks = 48;
bool Acquire(GpuWrapApi::eBufferChunkCategory category, std::uint32_t needed, CommandEntry::SystemDrawBlock& block) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = pool.begin(); it != pool.end(); ++it) {
            if (it->category != category || it->capacity < needed) continue;
            block = std::move(*it); pool.erase(it); block.bytes.clear(); return true;
        }
    }
    std::uint32_t capacity = kBlockBytes;
    while (capacity < needed) capacity <<= 1;
    GpuApi::BufferDesc descriptor{};
    descriptor.size = capacity; descriptor.category = category; descriptor.usage = GpuApi::BUT_Default;
    GpuApi::BufferInitData initial{}; initial.m_mode = GpuApi::BufferInitData::Mode::Uninitialized;
    auto reference = GpuApi::CreateBuffer(descriptor, initial);
    if (reference.isNull()) return false;
    GpuApi::BufferDesc actual; WGPUBuffer native = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(reference, actual, native) || !native) { GpuApi::Release(reference); return false; }
    GpuApi::Release(reference); // drop the probe ref; the creation ref keeps the native buffer alive
    block = {reference, native, category, capacity, {}};
    return true;
}
void Release(CommandEntry::SystemDrawBlock& block) {
    if (block.reference.isNull()) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pool.size() < kMaxPooledBlocks) { block.bytes.clear(); pool.push_back(std::move(block)); block = {}; return; }
    }
    GpuApi::Release(block.reference); block = {};
}
}
// Called with commandMutex held. Borrowed buffers remain owned by this
// command's retainedBuffers/uploads until actual GPU completion.
bool FlushPendingBufferCopies(CommandEntry& command) {
    if (command.failed) return false;
    if (command.pendingBufferCopies.empty()) return true;
    if (!command.stagingBytes.empty() && command.preparedStaging) {
        // perf12: arena bound by PrepareStagingArena on the submitter (lease already in stagingArenas).
        const auto prepared = command.preparedStaging; command.preparedStaging = nullptr;
        if (!cp2077::web::WriteGpuBufferRange(prepared, 0, command.stagingBytes.data(), command.stagingBytes.size())) {
            MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
        }
        for (const auto index : command.stagedOps) command.pendingBufferCopies[index].copy.source = prepared;
        command.stagingBytes.clear(); command.stagedOps.clear();
    }
    if (!command.stagingBytes.empty()) {
        // One arena and one queue write for every staged upload in this batch; copy order is unchanged.
        auto lease = StagingArena::Acquire(command.stagingBytes.size());
        if (!lease.buffer || !cp2077::web::WriteGpuBufferRange(lease.buffer, 0, command.stagingBytes.data(), command.stagingBytes.size())) {
            StagingArena::Release(lease);
            MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
        }
        command.stagingArenas.push_back(lease);
        for (const auto index : command.stagedOps) command.pendingBufferCopies[index].copy.source = lease.buffer;
        command.stagingBytes.clear(); command.stagedOps.clear();
    }
    const bool cpOrderedEncoded=cp2077::web::EncodeGpuOrderedCommands(command.encoder,command.pendingBufferCopies.data(),command.pendingBufferCopies.size());
    // Bind groups created during the batch belong to this command until GPU completion, even after a failure.
    for(auto& operation:command.pendingBufferCopies)
        if(operation.draw&&operation.draw->createdGroup){command.retainedBindGroups.push_back(operation.draw->createdGroup);operation.draw->createdGroup=nullptr;}
    for(auto& dispatch:command.queuedDispatches)
        if(dispatch->created){command.retainedBindGroups.push_back(dispatch->created);dispatch->created=nullptr;}
    command.queuedDispatches.clear();
    if (!cpOrderedEncoded) {
        MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
    }
    static std::atomic<unsigned> reports{0};
    if(reports.fetch_add(1)<8)std::fprintf(stderr,"CP2077 ordered command batch probe: operations=%llu\n",static_cast<unsigned long long>(command.pendingBufferCopies.size()));
    command.pendingBufferCopies.clear();return true;
}
// perf12: worker half of FlushPendingBufferCopies for the asynchronous submit (see CommandEntry::preparedStaging).
void PrepareStagingArena(CommandEntry& command) {
    if (command.failed || command.stagingBytes.empty() || command.preparedStaging) return;
    auto lease = StagingArena::Acquire(command.stagingBytes.size());
    if (!lease.buffer) { MarkBrowserCommandFailed(command,__func__,__LINE__); return; }
    command.stagingArenas.push_back(lease); command.preparedStaging = lease.buffer;
}
bool QueuePendingBufferCopy(CommandEntry& command,WGPUBuffer source,std::uint64_t sourceOffset,
    WGPUBuffer destination,std::uint64_t destinationOffset,std::uint64_t bytes) {
    if(command.failed||!command.encoder||!source||!destination||source==destination||
       (sourceOffset|destinationOffset|bytes)%4||sourceOffset>UINT64_MAX-bytes||destinationOffset>UINT64_MAX-bytes)return false;
    if(!bytes)return true;
    if(command.pendingBufferCopies.size()>=4096&&!FlushPendingBufferCopies(command))return false;
    cp2077::web::GpuOrderedCommand operation;operation.copy={source,destination,sourceOffset,destinationOffset,bytes};
    command.pendingBufferCopies.push_back(std::move(operation));return true;
}
// Queue CPU bytes for an ordered copy into destination; the staging source is bound at the next flush.
bool QueuePendingStagedUpload(CommandEntry& command,const void* bytes,std::uint64_t size,
    WGPUBuffer destination,std::uint64_t destinationOffset) {
    if(command.failed||!command.encoder||!destination||!bytes||(destinationOffset|size)%4||destinationOffset>UINT64_MAX-size)return false;
    if(!size)return true;
    if(command.pendingBufferCopies.size()>=4096&&!FlushPendingBufferCopies(command))return false;
    const std::uint64_t offset=command.stagingBytes.size();
    const auto* first=static_cast<const unsigned char*>(bytes);
    command.stagingBytes.insert(command.stagingBytes.end(),first,first+size);
    cp2077::web::GpuOrderedCommand operation;operation.copy={nullptr,destination,offset,destinationOffset,size};
    command.stagedOps.push_back(command.pendingBufferCopies.size());
    command.pendingBufferCopies.push_back(std::move(operation));return true;
}
// Persistent uniform ring: ONE Uniform|CopyDst buffer per device generation, handed out to command lists in
// fixed 64 KiB pages. Every arena-backed uniform therefore binds the same buffer handle; with explicit pipeline
// layouts the per-draw offset travels as a dynamic offset, so bind-group cache keys repeat across command lists
// and frames (per-list pooled arena buffers changed the handle in every key, ~93% bind-group misses).
// Pages, not a head/tail ring: lists retire out of order and can stay open for long, so a page is simply
// returned to the free list when the list that leased it retires on the GPU (ReleaseCommandResources), the
// same fencing the pooled arenas used; its old bytes are never overwritten while a submission can read them.
// Page bases are multiples of 64 KiB, so every offset stays 256-aligned (minUniformBufferOffsetAlignment).
// When the ring is exhausted or cannot be created, callers fall back to a pooled arena block (still correct,
// only its bind groups do not repeat).
namespace UniformRing {
constexpr std::uint64_t kPageBytes = 64ull << 10, kRingBytes = 64ull << 20;
constexpr std::uint32_t kPageCount = std::uint32_t(kRingBytes / kPageBytes);
std::mutex mutex;
WGPUBuffer buffer = nullptr;
std::uint64_t generation = 0;
bool creating = false, creationFailed = false;
std::vector<std::uint32_t> freePages;
std::uint32_t leased = 0, highWater = 0;
// Buffers of earlier device generations stay alive until their last leased page is released.
struct Retired { WGPUBuffer buffer; std::uint32_t leased; };
std::map<std::uint64_t, Retired> retired;
std::atomic<unsigned> exhaustedReports{0};
// Switches to the current device generation. Caller holds mutex. Returns a buffer to destroy (or null).
WGPUBuffer SelectGenerationLocked(std::uint64_t current) {
    if (generation == current) return nullptr;
    WGPUBuffer stale = nullptr;
    if (buffer) { if (leased) retired[generation] = {buffer, leased}; else stale = buffer; }
    buffer = nullptr; generation = current; creating = false; creationFailed = false;
    freePages.clear(); leased = 0;
    return stale;
}
bool Acquire(CommandEntry::UniformPageLease& lease, WGPUBuffer& pageBuffer, std::uint64_t& pageBase) {
    const auto current = cp2077::web::GpuDeviceGeneration();
    WGPUBuffer stale = nullptr;
    bool create = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        stale = SelectGenerationLocked(current);
        // CreateGpuBuffer refuses the main thread (returns null); never let a main-thread draw mark the ring failed
        // for the whole generation. Main-thread callers just take the pooled arena until a worker creates it.
        if (!buffer && !creating && !creationFailed && !emscripten_is_main_runtime_thread()) { creating = true; create = true; }
    }
    if (stale) cp2077::web::DestroyGpuBuffer(stale);
    if (create) {
        // One validated creation per device generation; never mapped (bytes arrive via queue.writeBuffer).
        auto created = cp2077::web::CreateGpuBuffer(kRingBytes, WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst, // [nomadperf] ovfStorage
            nullptr, 0, "cp-uniform-ring");
        bool adopted = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation == current && creating) {
                creating = false;
                if (created) {
                    buffer = created; adopted = true;
                    freePages.resize(kPageCount);
                    for (std::uint32_t i = 0; i < kPageCount; ++i) freePages[i] = kPageCount - 1 - i; // pop_back -> page 0 first
                } else creationFailed = cp2077::web::IsGpuDeviceReady(); // not ready yet: retry on a later page request
            }
        }
        if (created && !adopted) cp2077::web::DestroyGpuBuffer(created);
        std::fprintf(stderr, "CP2077 WebGPU: uniform ring %s bytes=%llu pages=%u generation=%llu\n",
            adopted ? "created" : created ? "discarded (generation changed)" : "creation failed (pooled arenas only)",
            static_cast<unsigned long long>(kRingBytes), kPageCount, static_cast<unsigned long long>(current));
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (generation != current || !buffer) return false;
    if (freePages.empty()) {
        const unsigned count = exhaustedReports.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 4 || (count & (count - 1)) == 0)
            std::fprintf(stderr, "CP2077 WebGPU: uniform ring exhausted, pooled arena fallback: pages=%u leased=%u count=%u\n",
                kPageCount, leased, count);
        return false;
    }
    lease = {freePages.back(), generation};
    freePages.pop_back();
    if (++leased > highWater) {
        highWater = leased;
        if ((highWater & (highWater - 1)) == 0 || highWater * 8 >= kPageCount * 7)
            std::fprintf(stderr, "CP2077 WebGPU: uniform ring high-water pages=%u of %u\n", highWater, kPageCount);
    }
    pageBuffer = buffer; pageBase = std::uint64_t(lease.page) * kPageBytes;
    return true;
}
void Release(const CommandEntry::UniformPageLease& lease) {
    WGPUBuffer destroy = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (lease.generation == generation && buffer) {
            if (lease.page < kPageCount && leased) { freePages.push_back(lease.page); --leased; }
        } else {
            auto old = retired.find(lease.generation);
            if (old != retired.end() && --old->second.leased == 0) { destroy = old->second.buffer; retired.erase(old); }
        }
    }
    if (destroy) cp2077::web::DestroyGpuBuffer(destroy);
}
}
// Suballocates a per-draw uniform payload from this command's uniform pages (or arena blocks). Replaces one
// validated buffer creation (three error-scope round trips) plus one main-thread destroy per varying uniform.
// Returned offsets are absolute within buffer and 256-aligned: WebGPU's default minUniformBufferOffsetAlignment
// is 256 and any device value is a power of two no larger than that, so 256 is always valid.
constexpr std::uint64_t kUniformAlignment = 256, kUniformBlockBytes = 256ull << 10, kMaxUniformBindingBytes = 64ull << 10;
static_assert(kMaxUniformBindingBytes <= UniformRing::kPageBytes && UniformRing::kPageBytes % kUniformAlignment == 0,
    "a ring page must hold the largest uniform binding at an aligned base");
bool AllocateCommandUniform(CommandEntry& command, const void* bytes, std::size_t size,
    WGPUBuffer& buffer, std::uint64_t& offset) {
    if (command.failed || !bytes || !size || size > kMaxUniformBindingBytes) return false;
    const std::uint64_t aligned = (size + kUniformAlignment - 1) & ~(kUniformAlignment - 1);
    if (command.uniformBlocks.empty() || command.uniformBlocks.back().bytes.size() + aligned > command.uniformBlocks.back().capacity) {
        CommandEntry::UniformBlock block;
        CommandEntry::UniformPageLease page{};
        if (UniformRing::Acquire(page, block.buffer, block.base)) {
            command.uniformPages.push_back(page);
            block.capacity = UniformRing::kPageBytes;
        } else {
            auto lease = StagingArena::Acquire(kUniformBlockBytes, WGPUBufferUsage_Uniform | WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
            if (!lease.buffer) return false;
            command.stagingArenas.push_back(lease);
            block.buffer = lease.buffer; block.base = 0; block.capacity = lease.bytes;
        }
        block.bytes.reserve(16u << 10);
        command.uniformBlocks.push_back(std::move(block));
    }
    auto& block = command.uniformBlocks.back();
    const std::uint64_t local = block.bytes.size();
    block.bytes.resize(local + aligned, 0);
    std::memcpy(block.bytes.data() + local, bytes, size);
    buffer = block.buffer;
    offset = block.base + local;
    return true;
}
// Called with commandMutex held, before this command list is submitted: queue.writeBuffer executes ahead of
// the later submit, so every draw that reads these offsets sees its bytes. All uniform and system-draw blocks
// go to the main thread in one handoff (was one synchronous proxy per block).
bool FlushCommandUniforms(CommandEntry& command) {
    static thread_local std::vector<cp2077::web::GpuBufferWrite> writes;
    static thread_local std::unique_ptr<bool[]> issued;
    static thread_local std::size_t issuedCapacity = 0;
    writes.clear();
    for (auto& block : command.uniformBlocks)
        if (!block.bytes.empty()) writes.push_back({block.buffer, block.base, block.bytes.data(), block.bytes.size()});
    for (auto& block : command.systemDrawBlocks)
        if (!block.bytes.empty()) writes.push_back({block.buffer, 0, block.bytes.data(), block.bytes.size()});
    if (!writes.empty()) {
        if (issuedCapacity < writes.size()) { issued.reset(new bool[writes.size()]); issuedCapacity = writes.size(); }
        bool ok = cp2077::web::WriteGpuBufferRanges(writes.data(), writes.size(), issued.get());
        for (std::size_t i = 0; ok && i < writes.size(); ++i) ok = issued[i];
        writes.clear();
        if (!ok) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
    }
    command.uniformBlocks.clear();
    for (auto& block : command.systemDrawBlocks) block.bytes.clear();
    return true;
}
// [flicker3] Called in submission order (CloseAndSubmitCommandLists holds submitOrderMutex; batch members in array
// order) before the list's uniform blocks are flushed. Fills every deferred constant slot with the value committed by
// the lists submitted before this one, then publishes this list's own final writes for the lists submitted after it.
// That is the native DX12 GPU-timeline result of Lock/Unlock copies. Before this, a list recorded on one worker
// snapshotted the buffer at bind time, i.e. whatever another worker had unlocked on the CPU by then: in some frames the
// G-buffer list got the previous (or next) frame's view constants while the depth prepass had the current ones, so
// every G-buffer fragment failed the depth-equal test against the prepass depth and the G-buffer stayed at its clear
// values (floor flicker; whole world black when the whole G-buffer list was affected).
void ResolveOrderedConstants(CommandEntry& command) {
    if (!command.failed && !command.deferredConstants.empty()) {
        static thread_local std::vector<std::pair<Uint32, std::vector<unsigned char>>> values;
        values.clear();
        for (const auto& slot : command.deferredConstants) {
            std::vector<unsigned char>* value = nullptr;
            for (auto& item : values) if (item.first == slot.ref) { value = &item.second; break; }
            if (!value) {
                values.emplace_back(slot.ref, std::vector<unsigned char>{});
                value = &values.back().second;
                if (!cp2077::web::CopyOrderedGpuConstantBuffer(GpuApi::BufferRef{slot.ref}, *value)) value->clear();
            }
            bool ok = slot.block < command.uniformBlocks.size() && value->size() >= slot.layout->nativeMinimumBytes;
            if (ok) {
                auto& block = command.uniformBlocks[slot.block];
                ok = slot.local + slot.layout->webBytes <= block.bytes.size() &&
                    cp2077::web::RepackUniform(*slot.layout, value->data(), value->size(), block.bytes.data() + slot.local, slot.layout->webBytes);
            }
            if (!ok) {
                const unsigned n = cbOrderUnresolved.fetch_add(1) + 1;
                if (n <= 8 || !(n & (n - 1)))
                    std::fprintf(stderr, "CP2077 cbOrder unresolved constant slot (zeros): list=%u ref=%u shader=%s binding=%u bytes=%zu total=%u\n",
                        command.id, slot.ref, slot.layout->shader, slot.layout->binding, value->size(), n);
            }
        }
        values.clear();
    }
    {
        static std::atomic<unsigned> resolves{0};
        const unsigned n = resolves.fetch_add(1) + 1;
        if (n == 64 || !(n & 4095))
            std::fprintf(stderr, "CP2077 cbOrder counters: lists=%u deferredSlots=%u reusedSlots=%u unresolved=%u ownCommits=%u on=%u\n",
                n, cbOrderDeferred.load(), cbOrderReused.load(), cbOrderUnresolved.load(), cbOrderCommits.load(), unsigned(CbOrderOn()));
    }
    command.deferredConstants.clear();
    command.deferredConstantSlots.clear();
    if (command.failed) return;
    for (const auto& snapshot : command.constantSnapshots)
        if (snapshot.second.own && !snapshot.second.locked && cp2077::web::IsOrderedGpuConstant(GpuApi::BufferRef{snapshot.first}) &&
            cp2077::web::CommitOrderedGpuConstantBufferWrite(GpuApi::BufferRef{snapshot.first}, snapshot.second.bytes.data(), snapshot.second.bytes.size()))
            cbOrderCommits.fetch_add(1, std::memory_order_relaxed);
}
// Queues a validated draw behind earlier ordered copies; FlushPendingBufferCopies encodes the whole run in one
// main-thread handoff. Every direct encode flushes first, so encoder order is unchanged.
bool QueuePendingGraphicsDraw(CommandEntry& command,std::shared_ptr<cp2077::web::GpuQueuedDraw> draw){
    if(command.failed||!command.encoder||!draw)return false;
    if(command.pendingBufferCopies.size()>=4096&&!FlushPendingBufferCopies(command))return false;
    cp2077::web::GpuOrderedCommand operation;operation.draw=std::move(draw);
    command.pendingBufferCopies.push_back(std::move(operation));return true;
}
// A caller-owned texture view handed to queued encoder work: released right after the work encodes it (on the
// main thread), or by the destructor if the work is dropped without running (failed or abandoned list).
struct QueuedTextureViewHold {
    WGPUTextureView view;
    explicit QueuedTextureViewHold(WGPUTextureView handle) : view(handle) {}
    void Release() { if (view) cp2077::web::ReleaseGpuTextureView(view); view = nullptr; }
    ~QueuedTextureViewHold() { Release(); }
    QueuedTextureViewHold(const QueuedTextureViewHold&) = delete;
    QueuedTextureViewHold& operator=(const QueuedTextureViewHold&) = delete;
};
// Queues main-thread encoder work in order with queued draws and copies (no synchronous handoff now).
bool QueuePendingCustom(CommandEntry& command,std::function<bool(WGPUCommandEncoder)> work){
    if(command.failed||!command.encoder||!work)return false;
    if(command.pendingBufferCopies.size()>=4096&&!FlushPendingBufferCopies(command))return false;
    cp2077::web::GpuOrderedCommand operation;operation.custom=std::move(work);
    command.pendingBufferCopies.push_back(std::move(operation));return true;
}
bool QueuePendingDebugCommand(CommandEntry& command,cp2077::web::GpuDebugCommand kind,const char* label){
    if(command.failed || !command.encoder || (kind!=cp2077::web::GpuDebugCommand::PopGroup && !label))return false;
    if(kind!=cp2077::web::GpuDebugCommand::PushGroup && kind!=cp2077::web::GpuDebugCommand::PopGroup && kind!=cp2077::web::GpuDebugCommand::Marker)return false;
    if(command.pendingBufferCopies.size()>=4096 && !FlushPendingBufferCopies(command))return false;
    cp2077::web::GpuOrderedCommand operation;operation.isDebug=true;operation.debug=kind;
    if(label)operation.label=label;
    command.pendingBufferCopies.push_back(std::move(operation));return true;
}
// perf11: per-list helper resources used to be created on first use in every command list and destroyed at its
// retirement: the R32Float depth-compatibility copy target (full-size texture + view, CRenderNode_RenderLightsIntegrate
// and other D32F SRV readers) and the R32Uint atomic bridge buffers (~11.8 MB storage + dimensions uniform). That cost
// several synchronous texture/buffer creations (validated on the main thread) plus the same number of destroys per
// frame, a new view/bind-group key every frame, and retire-worker time before the fence completed. Both are pooled.
// Safe to reuse: an entry returns to the pool only from ReleaseCommandResources, i.e. after its list's GPU work
// completed; while a list holds an entry no other list can obtain it; every use overwrites the contents before
// reading (depth copy before each read; atomic import copy before each draw). Bounded; stale device generations are
// destroyed. ?perf11NoPools=1 restores create/destroy per list for A/B.
namespace Perf11Pools {
bool Disabled() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('perf11NoPools') === '1' ? 1 : 0; });
        std::fprintf(stderr, "CP2077 perf11 switches: perf11NoPools=%d\n", v);
        return v;
    }();
    return value != 0;
}
struct CompatEntry { Uint32 kind, width, height; std::uint64_t generation; WGPUTexture texture; WGPUTextureView view; };
struct AtomicEntry { std::uint64_t generation; cp2077::web::GpuAtomicTextureBuffer buffer; };
std::mutex mutex;
std::vector<CompatEntry> compat;
std::vector<AtomicEntry> atomics;
std::uint64_t atomicBytes = 0;
std::atomic<unsigned> compatCreated{0}, compatReused{0}, atomicCreated{0}, atomicReused{0};
constexpr std::size_t kCompatLimit = 32;
// [skperf] texPool: indoor scenes ([npcblack] r16depth character-shadow copies 2048x2048x2 Depth16/R16Unorm/R32Float
// plus 256x256 lookup copies, and depth-float compat copies) check out ~6 sets per frame per command list; lists
// return them only at GPU retire, so several frames are outstanding and 32 idle slots overflowed: ~230-490 texture
// creates (synchronous, error-scope validated) + destroys per second, each destroy invalidating views/bind groups.
// The pool now holds up to 512 idle entries / 768 MiB, evicting the oldest idle entry. ?texPool=0 = old (32, refuse).
constexpr std::size_t kCompatBigLimit = 512;
constexpr std::uint64_t kCompatBigBytes = 768ull << 20;
std::uint64_t compatBytes = 0; // guarded by mutex
std::atomic<unsigned> compatEvicted{0}, compatRefused{0}, compatMisses{0};
// [skperf] round 5. ?perfprobe=1 turns on the per-second skperf probes (share/work/submit/frame) and their timers;
// default OFF so a fresh player's build prints none of them. Banner 'CP2077 skperf switches:' is always printed a
// few times (power-of-two probe windows), so a late-attached console capture still sees every switch value.
// [skperf] round 18 ?probeMask=N (with ?perfprobe=1; default 255 = everything) bisects the probe groups:
// 1 = job dispatcher (job cost probe, job probe, job timeline entries), 2 = render-node probe + R6 timeline,
// 4 = GPU-layer probes (everything gated by PerfProbeOn: frame/draw/resource/cb/pass/... lines and timers),
// 8 = mirror probe + Bink timer. Any nonzero mask keeps the cheap encode counters, the cull mesh-collect probe and
// the bink/mirror counters that only test 'flag != 0'. cp2077::web::skperfEncodeProbe holds the mask.
std::atomic<int> perfProbeState{-1};
bool PerfProbeOn() {
    int v = perfProbeState.load(std::memory_order_acquire);
    if (v < 0) {
        const int mask = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search);
            if (q.get('perfprobe') !== '1') return 0;
            const m = q.get('probeMask');
            return m === null ? 255 : (Number(m) & 255);
        });
        v = (mask & 4) ? 1 : 0;
        perfProbeState.store(v, std::memory_order_release);
        cp2077::web::skperfEncodeProbe.store(mask, std::memory_order_relaxed);
        if (mask) std::fprintf(stderr, "CP2077 skperf switches: probeMask=%d\n", mask);
    }
    return v != 0;
}
// earlyFlush (default ON, ?earlyFlush=0 old): GpuApi::Flush (once per FrameTick, bounds work in flight to two
// Flush calls) waited until the retirement worker had RELEASED every resource of the submission two Flushes ago,
// i.e. GPU completion + callback delivery + the retire worker's main-thread handoffs (buffer destroys drain the
// deferred submit backlog). It now waits only for GPU completion of those submissions (the completion callback).
// Fences, resource release and DrainGpuSubmissions keep the old post-release semantics.
std::atomic<int> earlyFlushState{-1};
bool EarlyFlushOn() {
    int v = earlyFlushState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('earlyFlush') === '0' ? 0 : 1; });
        earlyFlushState.store(v, std::memory_order_release);
    }
    return v != 0;
}
// Frame probe counters (us sums are only accumulated while PerfProbeOn()).
std::atomic<unsigned long long> frameFlushCalls{0}, frameFlushWaitUs{0}, frameFlushMaxUs{0}, frameGpuJobs{0}, frameGpuLatencyUs{0},
    frameGpuLatencyMaxUs{0}, frameGpuBusyUs{0}, frameRetireUs{0}, frameRetires{0}, frameDraws{0}, frameDrawUs{0}, frameDispatches{0},
    frameDispatchUs{0}, frameCloseSubmits{0}, frameCloseSubmitUs{0};
void NoteMax(std::atomic<unsigned long long>& peak, unsigned long long value) {
    auto previous = peak.load(std::memory_order_relaxed);
    while (value > previous && !peak.compare_exchange_weak(previous, value)) {}
}
void ThreadBusyAdd(unsigned long long us);
struct UsTimer {
    std::atomic<unsigned long long>& calls; std::atomic<unsigned long long>& total; bool on; bool busy; double start;
    UsTimer(std::atomic<unsigned long long>& c, std::atomic<unsigned long long>& t, bool recordBusy = false) : calls(c), total(t), on(PerfProbeOn()), busy(recordBusy), start(on ? emscripten_get_now() : 0) {}
    ~UsTimer() {
        if (!on) return;
        const auto us = (unsigned long long)((emscripten_get_now() - start) * 1000.0);
        calls.fetch_add(1, std::memory_order_relaxed); total.fetch_add(us, std::memory_order_relaxed);
        if (busy) ThreadBusyAdd(us);
    }
};
// [skperf] round 6. atomicQueue (default ON, ?atomicQueue=0 old): draws with lowered texture atomics (R32Uint UAV
// bridge: hair/character passes) used the immediate path: a synchronous bind-group creation, a flush, a synchronous
// import copy, a synchronous draw encode and a synchronous export copy, i.e. 4-5 blocking main-thread handoffs per
// draw (handoff probe: GraphicsBindingCreation ~30, EncodeGpuTextureBufferCopy ~10, EncodeGpuGraphicsDraw ~6 ms per
// frame). They are now queued like ordinary draws: import copy, draw (bind group deferred to the encode on miss),
// export copy, as ordered operations encoded in the same order by the batched main-thread flush.
// [skperf] round 18 texRetain (default ON, ?texRetain=0 old): a sampled SRV took two global texture-mutex
// acquisitions per binding on the recording worker (RetainGpuTexture, then GetTextureDesc) and one more at list
// retirement (Release), ~29k SRV bindings per frame from 9 threads. Now the first use of a texture in a list retains
// it once (descriptor fetched under the same lock) and later draws of that list reuse it with a list-local lookup.
// Same lifetime: draw retains were already appended to the list's retainedTextures and released at its retirement.
std::atomic<int> texRetainState{-1};
bool TexRetainOn() {
    int v = texRetainState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('texRetain') === '0' ? 0 : 1; });
        texRetainState.store(v, std::memory_order_release);
    }
    return v != 0;
}
// [skperf] round 42 drawRetain (default ON, ?drawRetain=0 old): every draw retained its color/depth attachments and
// its vertex/index/indirect/storage buffers again (RetainGpuTexture / AcquireGpuBuffer: the global textureMutex /
// bufferMutex shared by every recording worker, ~6-8 acquisitions per draw, plus the same number of Release calls at
// retirement). Now the first draw of a list that uses a texture/buffer retains it once for the list and later draws
// reuse it with a list-local lookup (texRetain already did this for sampled SRVs and shaders). Same lifetime: draw
// retains were already appended to the list's retainedTextures/retainedBuffers and released at its retirement; the set
// of buffer IDs a list references (mapped-upload flush scan) is unchanged.
// [skperf] round 45 uniMemo (default ON, ?uniMemo=0 old) and biasMemo (default ON, ?biasMemo=0 old); see
// gpu-api-graphics-draw-web.inl bindUniform / SamplerBias.
bool CbSnapReuseOn() { // [skperf] round 46, see SetBoundGpuConstantBuffers. Round 47: default OFF (tc34 failed run), ?cbSnapReuse=1 opt-in
    static const int v = [] {
        const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('cbSnapReuse') === '1' ? 1 : 0; } catch (e) { return 0; } });
        std::fprintf(stderr, "CP2077 skperf switches: cbSnapReuse=%d\n", on); return on; }();
    return v != 0;
}
bool CpUniMemoOn() {
    static const int v = [] {
        const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('uniMemo') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: uniMemo=%d\n", on); return on; }();
    return v != 0;
}
bool CpBiasMemoOn() {
    static const int v = [] {
        const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('biasMemo') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: biasMemo=%d\n", on); return on; }();
    return v != 0;
}
bool CpDrawRetainOn() {
    static const int v = [] {
        const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('drawRetain') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: drawRetain=%d\n", on); return on; }();
    return v != 0;
}
std::atomic<int> atomicQueueState{-1};
bool AtomicQueueOn() {
    int v = atomicQueueState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('atomicQueue') === '0' ? 0 : 1; });
        atomicQueueState.store(v, std::memory_order_release);
    }
    return v != 0;
}
std::atomic<unsigned long long> frameAtomicDraws{0}, frameAtomicQueued{0};
// [skperf] round 11 cbReuse (default ON, ?cbReuse=0 old) and CB sub-path counters (perfprobe only):
// 0 native-backed repack, 1 cbOrder reused, 2 cbOrder deferred, 3 snapshot, 4 mapped copy, 5 legacy, 6 null,
// 7 pack-cache hit, 8 pack-cache miss (repack), 9 cbReuse location reused, 10 new uniform location bound.
// [skperf] round 16 xorBatch (default ON, ?xorBatch=0 old). Light-channel / GI volumes are drawn into an R8_Uint target
// with logic-op XOR, emulated per draw as: initialize compute dispatch (target bits -> 8 scratch layers, full extent),
// the draw on the layers (binary XOR via blending), resolve draw (layers -> target, full extent). XOR is associative,
// so a run of consecutive XOR draws on the same target with nothing else in between (only buffer copies / debug
// markers, which touch no texture) gives the same final target with ONE initialize before the first draw and ONE
// resolve after the last: a continuing draw removes the run's previous resolve op and appends its own. The run's
// XOR draws may share one render pass (identical 8 scratch-layer targets; order kept inside the pass).
// xorStats: [0] XOR draws queued, [1] initialize dispatches skipped, [2] resolves removed.
std::atomic<int> xorBatchState{-1};
bool XorBatchOn() {
    int v = xorBatchState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('xorBatch') === '0' ? 0 : 1; });
        xorBatchState.store(v, std::memory_order_release);
    }
    return v != 0;
}
std::atomic<unsigned long long> xorStats[3];
// [skperf] round 15 passHold (default ON, ?passHold=0 old): see CommandEntry::heldAttachments.
std::atomic<int> passHoldState{-1};
bool PassHoldOn() {
    int v = passHoldState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('passHold') === '0' ? 0 : 1; });
        passHoldState.store(v, std::memory_order_release);
    }
    return v != 0;
}
// [skperf] round 15 R2a vertex-stream probe (perfprobe only, record time): [0] non-indirect draws with vertex
// slots, [1] exactly one per-vertex slot, [2] several per-vertex slots all with the same k = offset/stride,
// [3] several per-vertex slots with differing k, [4] draws with per-instance slots, [5] per-instance slots all same
// k, [6] per-vertex slots bound at a nonzero offset, [7] slots whose (buffer, offset % stride) equals the previous
// draw's slot on this thread while the offset differs (setVertexBuffer calls a fold could elide), [8] slots seen.
std::atomic<unsigned long long> vbProbe[9];
std::atomic<int> cbReuseState{-1};
bool CbReuseOn() {
    int v = cbReuseState.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('cbReuse') === '0' ? 0 : 1; });
        cbReuseState.store(v, std::memory_order_release);
    }
    return v != 0;
}
std::atomic<unsigned long long> cbPath[11];
inline void NoteCbPath(unsigned path) { if (PerfProbeOn()) cbPath[path].fetch_add(1, std::memory_order_relaxed); }
// [skperf] round 8: deferred submit job (main thread) phases: 0 mapped writes, 1 prefix encodes, 2 allocation
// service, 3 FlushPendingBufferCopies (ordered encode incl. draws), 4 FlushCommandUniforms (writeBuffer),
// 5 validation poll, 6 finish+submit; plus mapped bytes, lists and uniform blocks per window.
std::atomic<unsigned long long> mainPhaseUs[7], mainMappedBytes{0}, mainJobLists{0}, mainUniformBlocks{0};
// Sampled (every 16th draw per thread, perfprobe only) per-resource-kind time of the draw resource loop.
constexpr unsigned kResourceKinds = 16;
std::atomic<unsigned long long> resourceKindCount[kResourceKinds], resourceKindUs[kResourceKinds], resourceSampledDraws{0};
struct KindUsTimer {
    bool on; unsigned kind; double start;
    KindUsTimer(bool sample, unsigned k) : on(sample && k < kResourceKinds), kind(k), start(on ? emscripten_get_now() : 0) {}
    ~KindUsTimer() { if (on) { resourceKindCount[kind].fetch_add(1, std::memory_order_relaxed); resourceKindUs[kind].fetch_add((unsigned long long)((emscripten_get_now() - start) * 1000.0), std::memory_order_relaxed); } }
};
bool SampleDrawResources() {
    if (!PerfProbeOn()) return false;
    static thread_local unsigned tick = 0;
    if ((++tick & 15) != 0) return false;
    resourceSampledDraws.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// CloseAndSubmitCommandLists phases (perfprobe): 0 submit-order mutex wait, 1 validate+claim, 2 ordered constants,
// 3 staging arenas / worker flush, 4 retire loop (incl. shareConv decisions), 5 mapped-buffer scan, 6 enqueue/submit.
std::atomic<unsigned long long> closePhaseUs[7];
struct PhaseClock {
    bool on; double last;
    PhaseClock() : on(PerfProbeOn()), last(on ? emscripten_get_now() : 0) {}
    void mark(unsigned phase) { if (!on) return; const double now = emscripten_get_now(); closePhaseUs[phase].fetch_add((unsigned long long)((now - last) * 1000.0), std::memory_order_relaxed); last = now; }
};
// [skperf] per-window GPU work probe ('CP2077 skperf work probe', every ~2 s from the retire path): command lists
// retired, [npcblack] r16depth conversions (copy + per-layer draw, GPU bandwidth), depth-float compat copies, pool
// misses (= synchronous texture creations) and evictions, so the next run names what fills 'previous commands'.
std::atomic<unsigned long long> workLists{0}, workR16Conversions{0}, workR16Bytes{0}, workDepthCopies{0}, workDepthBytes{0};
// [skperf] shareConv counters: conversions run/skipped at submit (shared), per-list fallbacks (the list wrote the
// source before reading it), reads deduplicated within a list, reads lost on a non-deferred submit; submit calls.
std::atomic<unsigned long long> workConvRun{0}, workConvSkipped{0}, workDepthRun{0}, workDepthSkipped{0}, workShareFallback{0},
    workShareDedupe{0}, workShareLost{0}, workSubmits{0}, workSubmitLists{0};
// [skperf] round 4: per-list depth-float refreshes skipped by listGen; deferred submit job cost on the main thread.
std::atomic<unsigned long long> workDepthListSkipped{0}, workJobs{0}, workJobEncoders{0}, workJobIsolated{0}, workJobFlushUs{0}, workJobSubmitUs{0};
int ListGenOnValue();
// [skperf] round 7: probe tables are fixed-size and lock-free (no heap allocation anywhere in a perfprobe path; the
// t4f9 P run died in the engine slab allocator, so probe code must not touch the allocator at all). Scope names are
// the engine's string literals: keyed by pointer identity.
constexpr unsigned kScopeSlots = 48;
struct ScopeSlot { std::atomic<const char*> name{nullptr}; std::atomic<unsigned> lists{0}, fallback{0}; };
ScopeSlot scopeSlots[kScopeSlots];
ScopeSlot* FindScopeSlot(const char* scope) {
    if (!scope) scope = "-";
    for (unsigned i = 0; i < kScopeSlots; ++i) {
        const char* current = scopeSlots[i].name.load(std::memory_order_acquire);
        if (current == scope) return &scopeSlots[i];
        if (!current) {
            const char* expected = nullptr;
            if (scopeSlots[i].name.compare_exchange_strong(expected, scope, std::memory_order_acq_rel) || expected == scope) return &scopeSlots[i];
        }
    }
    return nullptr; // table full: not counted
}
void NoteSubmitScope(const char* scope, unsigned lists) {
    workSubmits.fetch_add(1, std::memory_order_relaxed); workSubmitLists.fetch_add(lists, std::memory_order_relaxed);
    if (!PerfProbeOn()) return; // [skperf] round 5
    if (auto* slot = FindScopeSlot(scope)) slot->lists.fetch_add(lists, std::memory_order_relaxed);
}
void NoteScopeFallback(const char* scope, unsigned reads) {
    if (!reads || !PerfProbeOn()) return;
    if (auto* slot = FindScopeSlot(scope)) slot->fallback.fetch_add(reads, std::memory_order_relaxed);
}
// Fixed line buffer (no heap): appends with snprintf, truncates safely.
struct LineBuf {
    char text[1400]; std::size_t used = 0;
    LineBuf() { text[0] = 0; }
    template<class... A> void add(const char* format, A... args) {
        if (used + 1 >= sizeof(text)) return;
        const int n = std::snprintf(text + used, sizeof(text) - used, format, args...);
        if (n > 0) used = std::min(sizeof(text) - 1, used + std::size_t(n));
    }
};
// Top-k scope deltas (lists or fallback) for this window, written into out.
void TopScopes(bool fallback, double seconds, unsigned k, LineBuf& out) {
    static unsigned lastLists[kScopeSlots], lastFallback[kScopeSlots];
    unsigned d[kScopeSlots]; bool used[kScopeSlots] = {};
    for (unsigned i = 0; i < kScopeSlots; ++i) {
        const unsigned v = fallback ? scopeSlots[i].fallback.load() : scopeSlots[i].lists.load();
        unsigned& last = fallback ? lastFallback[i] : lastLists[i];
        d[i] = v - last; last = v;
    }
    for (unsigned n = 0; n < k; ++n) {
        unsigned best = kScopeSlots;
        for (unsigned i = 0; i < kScopeSlots; ++i) if (!used[i] && d[i] && (best == kScopeSlots || d[i] > d[best])) best = i;
        if (best == kScopeSlots) break;
        used[best] = true;
        const char* name = scopeSlots[best].name.load();
        out.add(" [%.60s %.1f/s]", name ? name : "?", d[best] / seconds);
    }
}
// Per-thread recording busy time (draw + dispatch + close/submit), for the critical-path question.
constexpr unsigned kBusySlots = 48;
std::atomic<unsigned long long> threadRecordUs[kBusySlots];
std::atomic<unsigned> nextBusySlot{0};
void ThreadBusyAdd(unsigned long long us) {
    static thread_local unsigned slot = kBusySlots + 1;
    if (slot == kBusySlots + 1) { const unsigned s = nextBusySlot.fetch_add(1); slot = s < kBusySlots ? s : kBusySlots; }
    if (slot < kBusySlots) threadRecordUs[slot].fetch_add(us, std::memory_order_relaxed);
}
bool TexPoolOn();
void WorkProbeTick() {
    static std::atomic<double> windowStart{0};
    static std::atomic<unsigned long long> lastLists{0}, lastR16{0}, lastR16Bytes{0}, lastDepth{0}, lastDepthBytes{0};
    static std::atomic<unsigned> lastMisses{0}, lastEvicted{0}, lastRefused{0}, lastReused{0};
    const double now = emscripten_get_now();
    double start = windowStart.load(std::memory_order_relaxed);
    if (start == 0) { windowStart.compare_exchange_strong(start, now); return; }
    if (now - start < 2000 || !windowStart.compare_exchange_strong(start, now)) return;
    const double seconds = (now - start) / 1000.0;
    {
        // [skperf] round 5: switch banner on windows 1, 2, 4, 8, ... (always, one line each).
        static std::atomic<unsigned> windows{0};
        const unsigned w = windows.fetch_add(1) + 1;
        if (!(w & (w - 1))) {
            char banner[512] = {0};
            MAIN_THREAD_EM_ASM({
                const q = new URLSearchParams(location.search);
                const on = (name, def) => { const v = q.get(name); return v === null ? def : (v === '0' ? 0 : (v === '1' ? 1 : def)); };
                const text = 'xorPool=' + on('xorPool', 1) + ' viewIndex=' + on('viewIndex', 1) + ' bigAsyncVal=' + on('bigAsyncVal', 1) +
                    ' texPool=' + on('texPool', 1) + ' shareConv=' + on('shareConv', 1) + ' listGen=' + on('listGen', 1) +
                    ' earlyFlush=' + on('earlyFlush', 1) + ' atomicQueue=' + on('atomicQueue', 1) + ' passBatch=' + on('passBatch', 1) + ' uniBatch=' + on('uniBatch', 1) + ' cbReuse=' + on('cbReuse', 1) + ' dbgElide=' + on('dbgElide', 1) + ' idxFold=' + on('idxFold', 1) + ' copyHoist=' + on('copyHoist', 1) + ' drawPool=' + on('drawPool', 1) + ' passHold=' + on('passHold', 1) + ' xorBatch=' + on('xorBatch', 1) + ' drawTrim=' + on('drawTrim', 1) + ' beginArr=' + on('beginArr', 1) + ' texRetain=' + on('texRetain', 1) + ' perfprobe=' + (q.get('perfprobe') === '1' ? 1 : 0) +
                    ' mlRepool=' + (q.get('mlRepool') === '1' ? 1 : 0) + ' asyncPresent=' + (q.get('asyncPresent') ?? 'default');
                stringToUTF8(text, $0, 500);
            }, banner);
            std::fprintf(stderr, "CP2077 skperf switches: %s window=%u\n", banner, w);
        }
    }
    if (!PerfProbeOn()) return; // [skperf] round 5: per-second probes only with ?perfprobe=1
    auto delta = [](auto& counter, auto& last) { const auto v = counter.load(); const auto d = v - last.load(); last.store(v); return d; };
    const auto lists = delta(workLists, lastLists), r16 = delta(workR16Conversions, lastR16), r16b = delta(workR16Bytes, lastR16Bytes);
    const auto depth = delta(workDepthCopies, lastDepth), depthb = delta(workDepthBytes, lastDepthBytes);
    const auto misses = delta(compatMisses, lastMisses), evicted = delta(compatEvicted, lastEvicted), refused = delta(compatRefused, lastRefused), reused = delta(compatReused, lastReused);
    static std::atomic<unsigned long long> lastConvRun{0}, lastConvSkipped{0}, lastDepthRun{0}, lastDepthSkipped{0}, lastFallback{0}, lastDedupe{0}, lastLost{0}, lastSubmits{0}, lastSubmitLists{0};
    const auto convRun = delta(workConvRun, lastConvRun), convSkipped = delta(workConvSkipped, lastConvSkipped);
    const auto depthRun = delta(workDepthRun, lastDepthRun), depthSkipped = delta(workDepthSkipped, lastDepthSkipped);
    const auto fallback = delta(workShareFallback, lastFallback), dedupe = delta(workShareDedupe, lastDedupe), lost = delta(workShareLost, lastLost);
    const auto submits = delta(workSubmits, lastSubmits), submitLists = delta(workSubmitLists, lastSubmitLists);
    LineBuf top; TopScopes(false, seconds, 8, top); // [skperf] round 7: no heap
    std::fprintf(stderr, "CP2077 skperf share probe: convRunPerSec=%.1f convSkippedPerSec=%.1f depthCopyRunPerSec=%.1f depthCopySkippedPerSec=%.1f fallbackPerSec=%.1f dedupePerSec=%.1f lostPerSec=%.1f submitsPerSec=%.1f submittedListsPerSec=%.1f scopes:%s\n",
        convRun / seconds, convSkipped / seconds, depthRun / seconds, depthSkipped / seconds, fallback / seconds, dedupe / seconds, lost / seconds,
        submits / seconds, submitLists / seconds, top.text);
    static std::atomic<unsigned long long> lastListSkipped{0}, lastJobs{0}, lastJobEncoders{0}, lastJobIsolated{0}, lastFlushUs{0}, lastSubmitUs{0};
    const auto listSkipped = delta(workDepthListSkipped, lastListSkipped), jobs = delta(workJobs, lastJobs), jobEncoders = delta(workJobEncoders, lastJobEncoders);
    const auto jobIsolated = delta(workJobIsolated, lastJobIsolated), flushUs = delta(workJobFlushUs, lastFlushUs), submitUs = delta(workJobSubmitUs, lastSubmitUs);
    LineBuf fallbackTop; TopScopes(true, seconds, 6, fallbackTop); // [skperf] round 7: no heap
    std::fprintf(stderr, "CP2077 skperf submit probe: depthListRefreshSkippedPerSec=%.1f jobsPerSec=%.1f encodersPerSec=%.1f isolatedPerSec=%.1f jobFlushMsPerSec=%.1f jobFinishSubmitMsPerSec=%.1f fallbackScopes:%s\n",
        listSkipped / seconds, jobs / seconds, jobEncoders / seconds, jobIsolated / seconds, flushUs / 1000.0 / seconds, submitUs / 1000.0 / seconds, fallbackTop.text);
    {
        // [skperf] round 5 frame probe: what fills 'previous commands' (render workers) and the main thread.
        static std::atomic<unsigned long long> l[16];
        const auto flushCalls = delta(frameFlushCalls, l[0]), flushWait = delta(frameFlushWaitUs, l[1]);
        const auto gpuJobs = delta(frameGpuJobs, l[2]), gpuLatency = delta(frameGpuLatencyUs, l[3]), gpuBusy = delta(frameGpuBusyUs, l[4]);
        const auto retires = delta(frameRetires, l[5]), retireUs = delta(frameRetireUs, l[6]);
        const auto draws = delta(frameDraws, l[7]), drawUs = delta(frameDrawUs, l[8]), dispatches = delta(frameDispatches, l[9]), dispatchUs = delta(frameDispatchUs, l[10]);
        const auto closes = delta(frameCloseSubmits, l[11]), closeUs = delta(frameCloseSubmitUs, l[12]);
        const auto flushMax = frameFlushMaxUs.exchange(0), gpuMax = frameGpuLatencyMaxUs.exchange(0);
        static unsigned long long le[8];
        unsigned long long enc[8];
        for (unsigned i = 0; i < 8; ++i) { const auto v = cp2077::web::skperfEncodeStats[i].load(); enc[i] = v - le[i]; le[i] = v; }
        const double frames = flushCalls ? double(flushCalls) : 1.0;
        std::fprintf(stderr, "CP2077 skperf frame probe: window=%.1fs flushPerSec=%.1f flushWaitMsPerFrame=%.2f flushWaitMaxMs=%.1f gpuJobsPerSec=%.1f gpuLatencyAvgMs=%.2f gpuLatencyMaxMs=%.1f gpuBusyMsPerSec=%.0f retireMsPerSec=%.1f retiresPerSec=%.1f "
            "drawsPerFrame=%.0f drawRecordMsPerFrame=%.2f dispatchesPerFrame=%.0f dispatchRecordMsPerFrame=%.2f closeSubmitPerFrame=%.1f closeSubmitMsPerFrame=%.2f "
            "mainEncodeMsPerFrame=%.2f mainBindGroupsPerFrame=%.0f mainBindGroupMsPerFrame=%.2f mainPrepareMsPerFrame=%.2f mainPassesPerFrame=%.0f mainDrawsPerFrame=%.0f mainCustomOpsPerFrame=%.0f mainCustomMsPerFrame=%.2f\n",
            seconds, flushCalls / seconds, flushWait / 1000.0 / frames, flushMax / 1000.0, gpuJobs / seconds, gpuJobs ? gpuLatency / 1000.0 / gpuJobs : 0.0, gpuMax / 1000.0,
            gpuBusy / 1000.0 / seconds, retireUs / 1000.0 / seconds, retires / seconds,
            draws / frames, drawUs / 1000.0 / frames, dispatches / frames, dispatchUs / 1000.0 / frames, closes / frames, closeUs / 1000.0 / frames,
            enc[7] / 1000.0 / frames, enc[0] / frames, enc[1] / 1000.0 / frames, enc[2] / 1000.0 / frames, enc[3] / frames, enc[4] / frames, enc[5] / frames, enc[6] / 1000.0 / frames);
        // Worker main-thread handoff waits (rows are filled with ?probes=1): top operations by wall time this window.
        // [skperf] round 7: fixed tables (no heap). Names are copied into a static table on first sight.
        constexpr unsigned kNames = 48;
        static char handoffNames[kNames][72]; static double lastWall[kNames], lastExec[kNames]; static unsigned nameCount = 0;
        double wall[kNames] = {}, exec[kNames] = {};
        for (const auto& row : cpGpuHandoffStats::rows) {
            if (!row.calls) continue;
            char name[72]; std::memcpy(name, row.name, sizeof(name)); name[71] = 0;
            unsigned index = 0;
            while (index < nameCount && std::strncmp(handoffNames[index], name, 72) != 0) ++index;
            if (index == nameCount) { if (nameCount == kNames) continue; std::memcpy(handoffNames[nameCount++], name, 72); }
            wall[index] += row.wallMs; exec[index] += row.executionMs;
        }
        double d[kNames] = {}; double handoffTotal = 0, handoffExec = 0; bool used[kNames] = {};
        for (unsigned i = 0; i < nameCount; ++i) { d[i] = wall[i] - lastWall[i]; handoffExec += exec[i] - lastExec[i]; lastWall[i] = wall[i]; lastExec[i] = exec[i]; handoffTotal += d[i]; }
        LineBuf text;
        for (unsigned n = 0; n < 6; ++n) {
            unsigned best = kNames;
            for (unsigned i = 0; i < nameCount; ++i) if (!used[i] && d[i] > 0 && (best == kNames || d[i] > d[best])) best = i;
            if (best == kNames) break;
            used[best] = true; text.add(" [%.60s %.1f]", handoffNames[best], d[best] / frames);
        }
        std::fprintf(stderr, "CP2077 skperf handoff probe: workerHandoffWaitMsPerFrame=%.2f mainHandoffExecMsPerFrame=%.2f top(ms/frame):%s\n", handoffTotal / frames, handoffExec / frames, text.text);
        // [skperf] round 7 critical-path line: is the frame bound by one recording thread, the main thread or the GPU?
        {
            static unsigned long long lastBusy[kBusySlots];
            static unsigned long long lastJobFlush = 0, lastJobSubmit = 0;
            double maxThread = 0, sumThread = 0; unsigned active = 0;
            const unsigned slots = std::min(nextBusySlot.load(), kBusySlots);
            for (unsigned i = 0; i < slots; ++i) {
                const auto v = threadRecordUs[i].load(); const double ms = (v - lastBusy[i]) / 1000.0 / frames; lastBusy[i] = v;
                if (ms > 0.05) { ++active; sumThread += ms; if (ms > maxThread) maxThread = ms; }
            }
            const auto jf = workJobFlushUs.load(), js = workJobSubmitUs.load();
            const double deferredMain = ((jf - lastJobFlush) + (js - lastJobSubmit)) / 1000.0 / frames; lastJobFlush = jf; lastJobSubmit = js;
            const double fps = flushCalls / seconds;
            // Bink SDK round trips (blocking render workers: UI/VideoPlayersTick): calls and wall ms per frame, top op.
            static unsigned long long lastBinkCalls[16], lastBinkUs[16];
            static const char* binkOps[16] = {"open","close","registerFB","frame","next","pause","wait","shouldSkip","goto","sound","summary","frameOffsets","loop","freeGlobals","op14","op15"};
            double binkCalls = 0, binkMs = 0, topMs = 0; unsigned topOp = 0;
            for (unsigned i = 0; i < 16; ++i) {
                const auto c = cp2077::web::skperfBinkCalls[i].load(), u = cp2077::web::skperfBinkUs[i].load();
                const double dc = double(c - lastBinkCalls[i]), dm = (u - lastBinkUs[i]) / 1000.0; lastBinkCalls[i] = c; lastBinkUs[i] = u;
                binkCalls += dc; binkMs += dm; if (dm > topMs) { topMs = dm; topOp = i; }
            }
            std::fprintf(stderr, "CP2077 skperf bink probe: callsPerFrame=%.1f waitMsPerFrame=%.1f top=%s %.1fms/frame avgCallMs=%.2f\n",
                binkCalls / frames, binkMs / frames, binkOps[topOp], topMs / frames, binkCalls ? binkMs / binkCalls : 0.0);
            std::fprintf(stderr, "CP2077 skperf crit probe: fps=%.1f frameMs=%.1f recordThreads=%u recordMaxThreadMs=%.1f recordSumMs=%.1f mainKnownMs=%.1f (deferredSubmit=%.1f handoffExec=%.1f) workerHandoffWaitMs=%.1f gpuBusyMs=%.1f flushWaitMs=%.2f draws=%.0f\n",
                fps, fps > 0 ? 1000.0 / fps : 0.0, active, maxThread, sumThread, deferredMain + handoffExec / frames, deferredMain, handoffExec / frames,
                handoffTotal / frames, gpuBusy / 1000.0 / frames, flushWait / 1000.0 / frames, draws / frames);
        }
        // [skperf] round 6: close/submit phases, atomic draws, sampled resource kinds.
        static unsigned long long lastPhase[7], lastKindCount[kResourceKinds], lastKindUs[kResourceKinds], lastSampled = 0, lastAtomic = 0, lastAtomicQueued = 0;
        double phase[7];
        for (unsigned i = 0; i < 7; ++i) { const auto v = closePhaseUs[i].load(); phase[i] = (v - lastPhase[i]) / 1000.0 / frames; lastPhase[i] = v; }
        const auto atomicDraws = frameAtomicDraws.load(), atomicQueued = frameAtomicQueued.load();
        const double atomicPerFrame = (atomicDraws - lastAtomic) / frames, atomicQueuedPerFrame = (atomicQueued - lastAtomicQueued) / frames;
        lastAtomic = atomicDraws; lastAtomicQueued = atomicQueued;
        const auto sampled = resourceSampledDraws.load(); const auto sampledDelta = sampled - lastSampled; lastSampled = sampled;
        static const char* kindNames[kResourceKinds] = {"CB","SRV","Sampler","Indirection","DrawParams","SamplerBias","StorageBuffer","StorageUAV","AtomicBuffer","AtomicDims","MLPool","MLTable","k12","k13","k14","k15"};
        LineBuf kinds;
        for (unsigned i = 0; i < kResourceKinds; ++i) {
            const auto c = resourceKindCount[i].load(), u = resourceKindUs[i].load();
            const auto dc = c - lastKindCount[i], du = u - lastKindUs[i]; lastKindCount[i] = c; lastKindUs[i] = u;
            if (!dc) continue;
            // x16: every 16th draw is sampled. msPerFrame estimates the whole frame; usEach is the mean.
            kinds.add(" [%s n/frame=%.0f msPerFrame=%.2f usEach=%.2f]", kindNames[i], dc * 16.0 / frames, du * 16.0 / 1000.0 / frames, double(du) / double(dc));
        }
        std::fprintf(stderr, "CP2077 skperf close probe: orderWaitMs=%.2f validateMs=%.2f constantsMs=%.2f stagingMs=%.2f retireLoopMs=%.2f mappedScanMs=%.2f enqueueMs=%.2f (per frame) atomicDrawsPerFrame=%.1f atomicQueuedPerFrame=%.1f atomicQueue=%d\n",
            phase[0], phase[1], phase[2], phase[3], phase[4], phase[5], phase[6], atomicPerFrame, atomicQueuedPerFrame, AtomicQueueOn() ? 1 : 0);
        { static unsigned long long lastHit = 0, lastMiss = 0; const auto hit = cmdlockCounters[7].load(), miss = cmdlockCounters[8].load();
          std::fprintf(stderr, "CP2077 skperf bindgroup probe: queuedDrawHitsPerFrame=%.0f queuedDrawMissesPerFrame=%.0f hitRate=%.3f\n", (hit - lastHit) / frames, (miss - lastMiss) / frames,
              (hit - lastHit + miss - lastMiss) ? double(hit - lastHit) / double(hit - lastHit + miss - lastMiss) : 0.0); lastHit = hit; lastMiss = miss; }
        std::fprintf(stderr, "CP2077 skperf resource probe: sampledDraws=%llu kinds:%s\n", static_cast<unsigned long long>(sampledDelta), kinds.text);
        {   // [skperf] round 11: constant-buffer bind paths and cbReuse hits (per frame).
            static unsigned long long lastCb[11]; double d[11];
            for (unsigned i = 0; i < 11; ++i) { const auto v = cbPath[i].load(); d[i] = (v - lastCb[i]) / frames; lastCb[i] = v; }
            std::fprintf(stderr, "CP2077 skperf cb probe: native=%.0f orderReused=%.0f orderDeferred=%.0f snapshot=%.0f mapped=%.0f legacy=%.0f null=%.0f packHit=%.0f packMiss=%.0f reuseHit=%.0f newBind=%.0f cbReuse=%d (per frame)\n",
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], CbReuseOn() ? 1 : 0);
        }
        {   // [skperf] round 8 main probe: what the main thread's deferred submit time is made of (ms per frame).
            static unsigned long long lastMain[7], lastMapped = 0, lastLists = 0, lastBlocks = 0;
            double ms[7]; double total = 0;
            for (unsigned i = 0; i < 7; ++i) { const auto v = mainPhaseUs[i].load(); ms[i] = (v - lastMain[i]) / 1000.0 / frames; lastMain[i] = v; total += ms[i]; }
            const auto mapped = mainMappedBytes.load(), mainLists = mainJobLists.load(), blocks = mainUniformBlocks.load();
            std::fprintf(stderr, "CP2077 skperf main probe: totalMs=%.2f mappedWriteMs=%.2f prefixMs=%.2f allocServiceMs=%.2f encodeCopiesMs=%.2f uniformsMs=%.2f validationPollMs=%.2f finishSubmitMs=%.2f mappedKBPerFrame=%.0f listsPerFrame=%.1f uniformBlocksPerFrame=%.1f\n",
                total, ms[0], ms[1], ms[2], ms[3], ms[4], ms[5], ms[6], (mapped - lastMapped) / 1024.0 / frames, (mainLists - lastLists) / frames, (blocks - lastBlocks) / frames);
            lastMapped = mapped; lastLists = mainLists; lastBlocks = blocks;
            // [skperf] round 9: JS crossings issued vs saved (passBatch replay calls, uniBatch writeBuffer/getSize).
            static unsigned long long lastReplay[4], lastWrite[3];
            double r[4], wr[3];
            for (unsigned i = 0; i < 4; ++i) { const auto v = cp2077::web::skperfReplayStats[i].load(); r[i] = (v - lastReplay[i]) / frames; lastReplay[i] = v; }
            for (unsigned i = 0; i < 3; ++i) { const auto v = cp2077::web::skperfWriteStats[i].load(); wr[i] = (v - lastWrite[i]) / frames; lastWrite[i] = v; }
            std::fprintf(stderr, "CP2077 skperf calls probe: drawsReplayed=%.0f replayCalls=%.0f stateIssued=%.0f stateElided=%.0f writeRanges=%.0f writeBufferCalls=%.0f getSizeCalls=%.0f (per frame) passBatch/uniBatch see banner\n",
                r[0], r[1], r[2], r[3], wr[0], wr[1], wr[2]);
            // [skperf] round 12: why passes close, pass begin/end/replay cost, issued state calls by type (per frame).
            static unsigned long long lastPass[11], lastIssued[9]; double ps[11], is[9];
            for (unsigned i = 0; i < 11; ++i) { const auto v = cp2077::web::skperfPassStats[i].load(); ps[i] = (v - lastPass[i]) / frames; lastPass[i] = v; }
            for (unsigned i = 0; i < 9; ++i) { const auto v = cp2077::web::skperfIssued[i].load(); is[i] = (v - lastIssued[i]) / frames; lastIssued[i] = v; }
            std::fprintf(stderr, "CP2077 skperf pass probe: closeCustom=%.0f closeDebug=%.0f closeCopy=%.0f closeNonMergeable=%.0f closeTargets=%.0f closeEnd=%.0f beginMs=%.2f endMs=%.2f replayMs=%.2f debugElided=%.0f indexFolded=%.0f issued pipeline=%.0f viewport=%.0f scissor=%.0f blend=%.0f stencil=%.0f vertex=%.0f bindGroup=%.0f index=%.0f draws=%.0f\n",
                ps[0], ps[1], ps[2], ps[3], ps[4], ps[5], ps[6] / 1000.0, ps[7] / 1000.0, ps[8] / 1000.0, ps[9], ps[10],
                is[0], is[1], is[2], is[3], is[4], is[5], is[6], is[7], is[8]);
            static unsigned long long lastHoist[3]; double h[3];
            for (unsigned i = 0; i < 3; ++i) { const auto v = cp2077::web::skperfHoistStats[i].load(); h[i] = (v - lastHoist[i]) / frames; lastHoist[i] = v; }
            std::fprintf(stderr, "CP2077 skperf hoist probe: copiesHoisted=%.0f copiesConflict=%.0f copiesOpaquePass=%.0f (per frame)\n", h[0], h[1], h[2]);
            static unsigned long long lastPool[3]; double dp[3];
            for (unsigned i = 0; i < 3; ++i) { const auto v = cp2077::web::skperfDrawPoolStats[i].load(); dp[i] = (v - lastPool[i]) / frames; lastPool[i] = v; }
            // Heap allocations per queued draw: old path = 1 (make_shared) + vectors grown (up to 5); pooled = 1 control
            // block + new objects + vectors grown.
            std::fprintf(stderr, "CP2077 skperf alloc probe: queuedDraws=%.0f objectAllocs=%.0f vectorGrowths=%.0f allocsPerDraw=%.2f (per frame)\n",
                dp[0], dp[1], dp[2], dp[0] ? (dp[1] + dp[2]) / dp[0] : 0.0);
            {   // [skperf] round 15: main-thread per-draw record cost and the R2a vertex-stream fold data (per frame).
                static unsigned long long lastMd[2], lastVb[9]; double md[2], vb[9];
                for (unsigned i = 0; i < 2; ++i) { const auto v = cp2077::web::skperfMainDrawUs[i].load(); md[i] = double(v - lastMd[i]); lastMd[i] = v; }
                for (unsigned i = 0; i < 9; ++i) { const auto v = vbProbe[i].load(); vb[i] = (v - lastVb[i]) / frames; lastVb[i] = v; }
                {   static unsigned long long lastXor[3]; double xs[3];
                    for (unsigned i = 0; i < 3; ++i) { const auto v = xorStats[i].load(); xs[i] = (v - lastXor[i]) / frames; lastXor[i] = v; }
                    std::fprintf(stderr, "CP2077 skperf xor probe: xorDraws=%.0f initsSkipped=%.0f resolvesRemoved=%.0f xorBatch=%d (per frame)\n", xs[0], xs[1], xs[2], XorBatchOn() ? 1 : 0); }
                std::fprintf(stderr, "CP2077 skperf draw probe: mainRecordMs=%.2f mainRecordUsPerDraw=%.2f vbDraws=%.0f vbSingleSlot=%.0f vbSameK=%.0f vbMixedK=%.0f instDraws=%.0f instSameK=%.0f vbNonzeroOffset=%.0f vbFoldElidable=%.0f vbSlots=%.0f (per frame)\n",
                    md[0] / 1000.0 / frames, md[1] ? md[0] / md[1] : 0.0, vb[0], vb[1], vb[2], vb[3], vb[4], vb[5], vb[6], vb[7], vb[8]);
            }
        }
    }
    std::fprintf(stderr, "CP2077 skperf work probe: window=%.1fs listsPerSec=%.0f r16ConvPerSec=%.1f r16MBPerSec=%.0f depthCopyPerSec=%.1f depthCopyMBPerSec=%.0f poolMissPerSec=%.1f poolReusePerSec=%.1f poolEvictPerSec=%.1f poolRefusePerSec=%.1f idle=%u idleMB=%.0f texPool=%d\n",
        seconds, lists / seconds, r16 / seconds, double(r16b) / 1048576.0 / seconds, depth / seconds, double(depthb) / 1048576.0 / seconds,
        misses / seconds, reused / seconds, evicted / seconds, refused / seconds, unsigned(compat.size()), double(compatBytes) / 1048576.0, TexPoolOn() ? 1 : 0);
}
bool TexPoolOn() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('texPool') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: texPool=%d\n", v);
        return v;
    }();
    return value != 0;
}
// Bytes of a pooled compat texture from its kind (see the AcquireCompat callers): 0 R32Float; 100+ Depth16Unorm;
// 200+ R16Unorm scratch; 300+ R32Float; 400+ R16Float; (kind % 50) = layers for kinds >= 100.
std::uint64_t CompatBytes(Uint32 kind, Uint32 width, Uint32 height) {
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (kind < 100) return pixels * 4;
    const std::uint64_t layers = std::max<Uint32>(1, kind % 50);
    const std::uint64_t texel = (kind >= 300 && kind < 400) ? 4 : 2;
    return pixels * layers * texel;
}
constexpr std::uint64_t kAtomicByteLimit = 96ull << 20;
void Report(const char* what, unsigned created, unsigned reused) {
    const unsigned n = created + reused;
    if (n <= 4 || !(n & (n - 1))) std::fprintf(stderr, "CP2077 perf11 pool probe: %s created=%u reused=%u\n", what, created, reused);
}
// kind 0 = depth-float compatibility R32Float (RenderAttachment|TextureBinding).
bool AcquireCompat(Uint32 kind, Uint32 width, Uint32 height, CommandEntry::DepthFloatCompatibilityView& out) {
    const auto generation = cp2077::web::GpuDeviceGeneration();
    std::vector<CompatEntry> stale;
    bool found = false;
    if (!Disabled()) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = compat.begin(); it != compat.end();) {
            if (it->generation != generation) { compatBytes -= CompatBytes(it->kind, it->width, it->height); stale.push_back(*it); it = compat.erase(it); continue; }
            if (!found && it->kind == kind && it->width == width && it->height == height) {
                out.texture = it->texture; out.view = it->view; out.poolKind = kind + 1; out.poolWidth = width; out.poolHeight = height;
                out.poolGeneration = generation; compatBytes -= CompatBytes(it->kind, it->width, it->height); it = compat.erase(it); found = true; continue;
            }
            ++it;
        }
    }
    for (auto& old : stale) { if (old.view) cp2077::web::ReleaseGpuTextureView(old.view); if (old.texture) cp2077::web::DestroyGpuTexture(old.texture); }
    if (found) Report("depth-compat", compatCreated.load(), compatReused.fetch_add(1) + 1);
    else if (!Disabled()) {
        const unsigned n = compatMisses.fetch_add(1) + 1; // a miss: the caller creates a new texture
        if (n <= 4 || !(n & (n - 1)))
            std::fprintf(stderr, "CP2077 skperf compat pool miss: kind=%u %ux%u created=%u reused=%u evicted=%u refused=%u idle=%u idleMB=%.1f texPool=%d\n",
                kind, width, height, n, compatReused.load(), compatEvicted.load(), compatRefused.load(), unsigned(compat.size()), double(compatBytes) / 1048576.0, TexPoolOn() ? 1 : 0);
    }
    return found;
}
void ReleaseCompat(CommandEntry::DepthFloatCompatibilityView& item) {
    bool kept = false;
    std::vector<CompatEntry> evicted;
    const bool big = item.poolKind ? TexPoolOn() : false; // read before taking the pool lock
    if (item.poolKind && item.texture && item.view && !Disabled() && item.poolGeneration == cp2077::web::GpuDeviceGeneration()) {
        std::lock_guard<std::mutex> lock(mutex);
        const Uint32 kind = item.poolKind - 1;
        const std::uint64_t bytes = CompatBytes(kind, item.poolWidth, item.poolHeight);
        if (!big ? compat.size() < kCompatLimit : bytes <= kCompatBigBytes) {
            compat.push_back({kind, item.poolWidth, item.poolHeight, item.poolGeneration, item.texture, item.view});
            compatBytes += bytes; kept = true;
            while (big && compat.size() > 1 && (compat.size() > kCompatBigLimit || compatBytes > kCompatBigBytes)) {
                compatBytes -= CompatBytes(compat.front().kind, compat.front().width, compat.front().height);
                evicted.push_back(compat.front()); compat.erase(compat.begin());
            }
        }
    }
    if (!kept && item.poolKind) compatRefused.fetch_add(1, std::memory_order_relaxed);
    if (!evicted.empty()) compatEvicted.fetch_add(unsigned(evicted.size()), std::memory_order_relaxed);
    for (auto& old : evicted) { if (old.view) cp2077::web::ReleaseGpuTextureView(old.view); if (old.texture) cp2077::web::DestroyGpuTexture(old.texture); }
    if (!kept) {
        if (item.view) cp2077::web::ReleaseGpuTextureView(item.view);
        if (item.texture) cp2077::web::DestroyGpuTexture(item.texture);
    }
    item = {};
}
bool AcquireAtomic(std::uint32_t width, std::uint32_t height, std::uint32_t layers, cp2077::web::GpuAtomicTextureBuffer& out) {
    if (out.words || out.dimensions) return false;
    const auto generation = cp2077::web::GpuDeviceGeneration();
    std::vector<cp2077::web::GpuAtomicTextureBuffer> stale;
    bool found = false;
    if (!Disabled()) {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto it = atomics.begin(); it != atomics.end();) {
            if (it->generation != generation) { atomicBytes -= it->buffer.bytes; stale.push_back(it->buffer); it = atomics.erase(it); continue; }
            if (!found && it->buffer.width == width && it->buffer.height == height && it->buffer.layers == layers) {
                out = it->buffer; atomicBytes -= it->buffer.bytes; it = atomics.erase(it); found = true; continue;
            }
            ++it;
        }
    }
    for (auto& old : stale) cp2077::web::ReleaseGpuAtomicTextureBuffer(old);
    if (found) { Report("atomic-bridge", atomicCreated.load(), atomicReused.fetch_add(1) + 1); return true; }
    if (!cp2077::web::CreateGpuAtomicTextureBuffer(width, height, layers, out)) return false;
    Report("atomic-bridge", atomicCreated.fetch_add(1) + 1, atomicReused.load());
    return true;
}
void ReleaseAtomic(cp2077::web::GpuAtomicTextureBuffer& buffer, std::uint64_t generation) {
    bool kept = false;
    if (buffer.words && buffer.dimensions && !Disabled() && generation == cp2077::web::GpuDeviceGeneration()) {
        std::lock_guard<std::mutex> lock(mutex);
        if (atomicBytes + buffer.bytes <= kAtomicByteLimit) { atomics.push_back({generation, buffer}); atomicBytes += buffer.bytes; kept = true; }
    }
    if (!kept) cp2077::web::ReleaseGpuAtomicTextureBuffer(buffer);
    buffer = {};
}
}
// [skperf] shareConv (?shareConv=0 = old per-list conversions). The [npcblack] r16depth conversions (D16U UAV ->
// Depth16Unorm / R32Float copies, 2048x2048x2 character shadows: ~90/s, ~4.4 GB/s of GPU traffic in Street Kid) and the
// D32F -> R32Float depth-float copies (~200/s) were redone by EVERY command list that read the source (~53 lists per
// frame). Now one shared copy per (source, kind) is converted only when the source was written since its last
// conversion, in WebGPU queue order:
//  - record time: every write to a texture by a list (render/depth attachment unless read-only, clear, UAV/storage
//    binding, copy/resolve/upload destination) is noted for that list. A read in a list that has NOT written the
//    source yet binds the shared copy; a list that wrote it first keeps the old in-list conversion (exact position).
//  - submit time (CloseAndSubmitCommandLists, submit-order mutex held, lists in batch order = queue order): for each
//    read, if the source's write generation changed since the copy was converted, the conversion goes into a prefix
//    encoder submitted immediately before this list; then this list's writes bump the generations.
//  Queue order makes overwriting the shared copy safe for lists submitted earlier. Only the deferred (async) submit
//  path runs prefixes; reads are bound shared only when that path is active.
struct SharedConvRead {
    std::shared_ptr<SharedConv::Conv> conv;
    Uint32 refId = 0;
    WGPUTexture r16Source = nullptr;                    // r16: source texture, retained by the reading list
    std::shared_ptr<QueuedTextureViewHold> depthHold;   // depth-float: the bound depth view (own reference)
};
namespace SharedConv {
struct Conv {
    bool depthCompat = false;
    Uint32 width = 0, height = 0, layers = 1;
    WGPUTextureFormat target = WGPUTextureFormat_Undefined;
    CommandEntry::DepthFloatCompatibilityView copy, scratch;
    std::vector<WGPUTextureView> layerViews; // r16: (scratch layer, copy layer) pairs
    std::uint64_t convertedGen = ~0ull;      // guarded by mutex
    ~Conv() {
        for (auto view : layerViews) if (view) cp2077::web::ReleaseGpuTextureView(view);
        Perf11Pools::ReleaseCompat(copy); Perf11Pools::ReleaseCompat(scratch);
    }
};
bool On() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('shareConv') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: shareConv=%d\n", v);
        return v;
    }();
    return value != 0;
}
// Same conditions as deferSubmit in CloseAndSubmitCommandLists (minus teardown), which runs the prefixes.
bool Allowed() {
    if (!On()) return false;
    const int mode = Perf12SubmitMode();
    if (mode == 0 || Perf10Switch(1)) return false;
    return mode == 2 || (Perf12SubSwitches() & 4) || publishedRenderFrame.load(std::memory_order_relaxed) >= 1000;
}
std::mutex mutex; // leaf lock: never held across a main-thread handoff or a texture release
std::map<std::pair<Uint32, Uint32>, std::shared_ptr<Conv>> convs;
std::unordered_map<Uint32, std::uint64_t> writeGen;
std::unordered_map<Uint32, std::vector<Uint32>> listWrites;
// [skperf] listGen: per-list generation of record-call writes (clears, copies, resolves, uploads). Attachment, UAV and
// storage bindings do not bump it (draws and dispatches that actually write bump CommandEntry::localWriteGen).
std::unordered_map<Uint32, std::unordered_map<Uint32, Uint32>> listGen;
constexpr std::size_t kMaxConvs = 96;
void NoteWrite(const GpuApi::TextureRef& reference, bool bumpGen = true) {
    if (reference.isNull() || !On()) return;
    const Uint32 list = bound.ID();
    if (!list) return;
    std::lock_guard<std::mutex> lock(mutex);
    auto& writes = listWrites[list];
    if (std::find(writes.begin(), writes.end(), reference.ID()) == writes.end()) writes.push_back(reference.ID());
    if (bumpGen) ++listGen[list][reference.ID()];
}
Uint32 ListGen(Uint32 list, Uint32 reference) {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = listGen.find(list);
    if (found == listGen.end()) return 0;
    auto gen = found->second.find(reference);
    return gen == found->second.end() ? 0 : gen->second;
}
bool ListGenOn() {
    static const int value = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('listGen') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 skperf switches: listGen=%d\n", v);
        return v;
    }();
    return value != 0;
}
bool ListWrote(Uint32 list, Uint32 reference) {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = listWrites.find(list);
    return found != listWrites.end() && std::find(found->second.begin(), found->second.end(), reference) != found->second.end();
}
std::shared_ptr<Conv> Find(std::pair<Uint32, Uint32> key, Uint32 width, Uint32 height, Uint32 layers, WGPUTextureFormat target) {
    std::lock_guard<std::mutex> lock(mutex);
    auto found = convs.find(key);
    if (found == convs.end()) return nullptr;
    const auto& conv = *found->second;
    if (conv.width != width || conv.height != height || conv.layers != layers || conv.target != target ||
        conv.copy.poolGeneration != cp2077::web::GpuDeviceGeneration()) return nullptr;
    return found->second;
}
void Insert(std::pair<Uint32, Uint32> key, const std::shared_ptr<Conv>& conv) {
    std::vector<std::shared_ptr<Conv>> dropped; // released after the lock (texture releases may hand off to main)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& slot = convs[key];
        if (slot) dropped.push_back(slot);
        slot = conv;
        for (auto it = convs.begin(); convs.size() > kMaxConvs && it != convs.end();) {
            if (it->second.use_count() == 1 && it->second != conv) { dropped.push_back(it->second); it = convs.erase(it); }
            else ++it;
        }
    }
}
bool ReadAlready(const CommandEntry& command, const std::shared_ptr<Conv>& conv) {
    for (const auto& read : command.sharedReads) if (read->conv == conv) return true;
    return false;
}
// Submit time, in queue order. converting=false: the list does not run prefixes (dropped / non-deferred submit).
void Decide(CommandEntry& command, bool converting) {
    std::vector<Uint32> writes;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = listWrites.find(command.id);
    if (found != listWrites.end()) { writes.swap(found->second); listWrites.erase(found); }
    listGen.erase(command.id);
    for (const auto& read : command.sharedReads) {
        auto& conv = *read->conv;
        const auto gen = writeGen.find(read->refId);
        const std::uint64_t current = gen == writeGen.end() ? 0 : gen->second;
        if (!converting) { conv.convertedGen = ~0ull; Perf11Pools::workShareLost.fetch_add(1, std::memory_order_relaxed); continue; }
        if (conv.convertedGen == current) {
            (conv.depthCompat ? Perf11Pools::workDepthSkipped : Perf11Pools::workConvSkipped).fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        conv.convertedGen = current;
        cp2077::web::GpuOrderedCommand operation;
        auto shared = read->conv;
        if (conv.depthCompat) {
            auto hold = read->depthHold;
            operation.custom = [shared, hold](WGPUCommandEncoder encoder) {
                return hold && hold->view && cp2077::web::EncodeGpuDepthFloatCopy(encoder, hold->view, shared->copy.view, shared->width, shared->height);
            };
            Perf11Pools::workDepthRun.fetch_add(1, std::memory_order_relaxed);
            Perf11Pools::workDepthCopies.fetch_add(1, std::memory_order_relaxed);
            Perf11Pools::workDepthBytes.fetch_add(std::uint64_t(conv.width) * conv.height * 8, std::memory_order_relaxed);
        } else {
            const WGPUTexture source = read->r16Source;
            operation.custom = [shared, source](WGPUCommandEncoder encoder) {
                WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                from.texture = source; to.texture = shared->scratch.texture;
                const WGPUExtent3D extent{shared->width, shared->height, shared->layers};
                wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
                bool ok = true;
                for (std::size_t i = 0; i + 1 < shared->layerViews.size(); i += 2)
                    ok = cp2077::web::EncodeGpuR16ToDepth16Copy(encoder, shared->layerViews[i], shared->layerViews[i + 1], shared->width, shared->height, shared->target) && ok;
                return ok;
            };
            Perf11Pools::workConvRun.fetch_add(1, std::memory_order_relaxed);
            Perf11Pools::workR16Conversions.fetch_add(1, std::memory_order_relaxed);
            Perf11Pools::workR16Bytes.fetch_add(std::uint64_t(conv.width) * conv.height * conv.layers * (conv.target == WGPUTextureFormat_R32Float ? 10u : 8u), std::memory_order_relaxed);
        }
        command.sharedPrefix.push_back(std::move(operation));
        command.sharedPrefixConvs.push_back(read->conv);
    }
    for (auto id : writes) ++writeGen[id];
}
void Invalidate(const std::vector<std::shared_ptr<Conv>>& list) {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& conv : list) conv->convertedGen = ~0ull;
}
// Deferred submit job (main thread): encodes the list's prefix into its own encoder, placed before the list.
void EncodePrefix(CommandEntry& command, std::vector<WGPUCommandEncoder>& encoders, bool jobFailed) {
    if (command.sharedPrefix.empty()) return;
    WGPUCommandEncoder encoder = jobFailed ? nullptr : cp2077::web::CreateGpuCommandEncoder();
    const bool ok = encoder && cp2077::web::EncodeGpuOrderedCommands(encoder, command.sharedPrefix.data(), command.sharedPrefix.size());
    if (ok) encoders.push_back(encoder);
    else {
        if (encoder) cp2077::web::ReleaseGpuCommandEncoder(encoder);
        Invalidate(command.sharedPrefixConvs);
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 16) std::fprintf(stderr, "CP2077 skperf shareConv prefix not encoded: list=%u ops=%zu jobFailed=%d\n",
            command.id, command.sharedPrefix.size(), int(jobFailed));
    }
    command.sharedPrefix.clear();
    command.sharedPrefixConvs.clear();
}
// Record time: build a shared r16depth target (same textures/views as the per-list path, pooled).
std::shared_ptr<Conv> MakeR16(Uint32 poolKind, Uint32 width, Uint32 height, Uint32 layers, WGPUTextureFormat target, bool arrayDecl) {
    auto conv = std::make_shared<Conv>();
    conv->width = width; conv->height = height; conv->layers = layers; conv->target = target;
    Perf11Pools::AcquireCompat(poolKind, width, height, conv->copy);
    if (!conv->copy.texture) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = WGPUTextureDimension_2D; desc.format = target; desc.size = {width, height, layers};
        desc.mipLevelCount = 1; desc.sampleCount = 1; desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        conv->copy.texture = cp2077::web::CreateGpuTexture(desc);
        if (!conv->copy.texture) return nullptr;
        WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        viewDesc.dimension = arrayDecl ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
        viewDesc.format = target; viewDesc.arrayLayerCount = layers;
        conv->copy.view = cp2077::web::CreateGpuTextureView(conv->copy.texture, viewDesc);
        if (!conv->copy.view) return nullptr;
        conv->copy.poolKind = poolKind + 1; conv->copy.poolWidth = width; conv->copy.poolHeight = height;
        conv->copy.poolGeneration = cp2077::web::GpuDeviceGeneration();
    }
    Perf11Pools::AcquireCompat(200 + layers, width, height, conv->scratch);
    if (!conv->scratch.texture) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = WGPUTextureDimension_2D; desc.format = WGPUTextureFormat_R16Unorm; desc.size = {width, height, layers};
        desc.mipLevelCount = 1; desc.sampleCount = 1; desc.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_TextureBinding;
        conv->scratch.texture = cp2077::web::CreateGpuTexture(desc);
        if (!conv->scratch.texture) return nullptr;
        WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        viewDesc.dimension = WGPUTextureViewDimension_2DArray; viewDesc.format = WGPUTextureFormat_R16Unorm; viewDesc.arrayLayerCount = layers;
        conv->scratch.view = cp2077::web::CreateGpuTextureView(conv->scratch.texture, viewDesc);
        if (!conv->scratch.view) return nullptr;
        conv->scratch.poolKind = 200 + layers + 1; conv->scratch.poolWidth = width; conv->scratch.poolHeight = height;
        conv->scratch.poolGeneration = cp2077::web::GpuDeviceGeneration();
    }
    for (Uint32 layer = 0; layer < layers; ++layer) {
        WGPUTextureViewDescriptor src = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        src.dimension = WGPUTextureViewDimension_2D; src.format = WGPUTextureFormat_R16Unorm;
        src.baseArrayLayer = layer; src.arrayLayerCount = 1; src.mipLevelCount = 1;
        WGPUTextureViewDescriptor dst = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        dst.dimension = WGPUTextureViewDimension_2D; dst.format = target;
        dst.baseArrayLayer = layer; dst.arrayLayerCount = 1; dst.mipLevelCount = 1;
        const auto srcView = cp2077::web::CreateGpuTextureView(conv->scratch.texture, src);
        conv->layerViews.push_back(srcView);
        const auto dstView = cp2077::web::CreateGpuTextureView(conv->copy.texture, dst);
        conv->layerViews.push_back(dstView);
        if (!srcView || !dstView) return nullptr;
    }
    return conv;
}
std::shared_ptr<Conv> MakeDepth(Uint32 width, Uint32 height) {
    auto conv = std::make_shared<Conv>();
    conv->depthCompat = true; conv->width = width; conv->height = height; conv->target = WGPUTextureFormat_R32Float;
    Perf11Pools::AcquireCompat(0, width, height, conv->copy);
    if (!conv->copy.texture) {
        WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
        desc.dimension = WGPUTextureDimension_2D; desc.format = WGPUTextureFormat_R32Float; desc.size = {width, height, 1};
        desc.mipLevelCount = 1; desc.sampleCount = 1; desc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
        conv->copy.texture = cp2077::web::CreateGpuTexture(desc);
        if (!conv->copy.texture) return nullptr;
        WGPUTextureViewDescriptor viewDesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        viewDesc.dimension = WGPUTextureViewDimension_2D; viewDesc.format = WGPUTextureFormat_R32Float;
        conv->copy.view = cp2077::web::CreateGpuTextureView(conv->copy.texture, viewDesc);
        if (!conv->copy.view) return nullptr;
        conv->copy.poolKind = 1; conv->copy.poolWidth = width; conv->copy.poolHeight = height;
        conv->copy.poolGeneration = cp2077::web::GpuDeviceGeneration();
    }
    return conv;
}
}
namespace Perf11Pools { int ListGenOnValue() { return SharedConv::ListGenOn() ? 1 : 0; } }
void ReleaseCommandResources(CommandEntry& command) {
    command.sharedPrefix.clear(); command.sharedPrefixConvs.clear(); command.sharedReads.clear(); // [skperf] shareConv
    { std::lock_guard<std::mutex> lock(SharedConv::mutex); SharedConv::listWrites.erase(command.id); SharedConv::listGen.erase(command.id); }
    Perf11Pools::workLists.fetch_add(1, std::memory_order_relaxed); Perf11Pools::WorkProbeTick(); // [skperf]
    for(auto& item:command.atomicTextureTransfers)Perf11Pools::ReleaseAtomic(item.second.buffer,item.second.generation); // perf11: pooled
    command.atomicTextureTransfers.clear();
    for(auto& item:command.logicXorScratch)cp2077::web::RecycleGpuLogicXorScratch(item.second); // perf8: back to the pool
    command.logicXorScratch.clear();
    if (command.nativeNullIntegerView.view) cp2077::web::ReleaseGpuTextureView(command.nativeNullIntegerView.view);
    if (command.nativeNullIntegerView.texture) cp2077::web::DestroyGpuTexture(command.nativeNullIntegerView.texture);
    command.nativeNullIntegerView = {};
    if (command.nativeNullEyeVolumeView.view) cp2077::web::ReleaseGpuTextureView(command.nativeNullEyeVolumeView.view);
    if (command.nativeNullEyeVolumeView.texture) cp2077::web::DestroyGpuTexture(command.nativeNullEyeVolumeView.texture);
    command.nativeNullEyeVolumeView = {};
    for (auto* array : {&command.nativeNullEyeArrayFloatView,&command.nativeNullEyeArrayUintView}) {
        if(array->view)cp2077::web::ReleaseGpuTextureView(array->view);
        if(array->texture)cp2077::web::DestroyGpuTexture(array->texture);
        *array={};
    }
    for (auto& item : command.depthFloatCompatibilityViews) Perf11Pools::ReleaseCompat(item.second); // perf11: pooled
    command.depthFloatCompatibilityViews.clear();
    for (auto& item : command.r16DepthViews) Perf11Pools::ReleaseCompat(item.second); // [npcblack] pooled
    command.r16DepthViews.clear();
    if (!command.indirectArguments.reference.isNull()) GpuApi::Release(command.indirectArguments.reference);
    for (auto& stage : command.graphicsSamplers)
        for (auto& sampler : stage)
            if (!sampler.reference.isNull()) GpuApi::Release(sampler.reference);
    for (auto& stage : command.graphicsTextures)
        for (auto& texture : stage) {
            if (texture.view) cp2077::web::ReleaseGpuTextureView(texture.view);
            if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
        }
    for (auto& texture : command.pixelStorageTextures) {
        if (texture.view) cp2077::web::ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
    }
    for (auto& stage : command.graphicsBuffers)
        for (auto& buffer : stage)
            if (!buffer.reference.isNull()) GpuApi::Release(buffer.reference);
    for (const auto& stage : command.graphicsConstantBuffers)
        for (auto reference : stage) if (!reference.isNull()) GpuApi::Release(reference);
    for (auto& target : command.colorAttachments) {
        if (target.view) cp2077::web::ReleaseGpuTextureView(target.view);
        if (!target.reference.isNull()) GpuApi::Release(target.reference);
    }
    for (auto& held : command.heldAttachments) { // [skperf] round 15 passHold
        if (held.view) cp2077::web::ReleaseGpuTextureView(held.view);
        if (!held.reference.isNull()) GpuApi::Release(held.reference);
    }
    command.heldAttachments.clear();
    if (command.depthAttachment.view) cp2077::web::ReleaseGpuTextureView(command.depthAttachment.view);
    if (!command.depthAttachment.reference.isNull()) GpuApi::Release(command.depthAttachment.reference);
    for (auto reference : command.computeConstantBuffers)
        if (!reference.isNull()) GpuApi::Release(reference);
    for (auto& buffer : command.computeBuffers)
        if (!buffer.reference.isNull()) GpuApi::Release(buffer.reference);
    for (auto& buffer : command.computeStorageBuffers)
        if (!buffer.reference.isNull()) GpuApi::Release(buffer.reference);
    for (auto& texture : command.computeTextures) {
        if (texture.view) cp2077::web::ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
    }
    for (auto& texture : command.computeStorageTextures) {
        if (texture.view) cp2077::web::ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
    }
    for (auto shader : command.retainedShaders) GpuApi::Release(shader);
    for (auto sampler : command.retainedSamplers) GpuApi::Release(sampler);
    for (auto pipeline : command.retainedRenderPipelines) cp2077::web::ReleaseGpuRenderPipeline(pipeline);
    cp2077::web::ReleaseGpuBindGroups(std::move(command.retainedBindGroups)); // one async handoff for uncached groups (perf7)
    command.retainedBindGroups.clear();
    for (auto texture : command.retainedTextures) GpuApi::Release(texture);
    command.textureRetainOnce.clear(); // [skperf] round 18 texRetain (those references were in retainedTextures)
    command.bufferRetainOnce.clear(); // [skperf] round 42 drawRetain (those references are in retainedBuffers, released below)
    command.uniformMemo.clear(); command.biasMemo[0].valid = command.biasMemo[1].valid = false; // [skperf] round 45 (leases are in uploads, released below)
    command.shaderRetainOnce.clear();
    command.samplerOnce.clear();
    if (!command.computeShader.isNull()) GpuApi::Release(command.computeShader);
    for (const auto& shader : command.graphicsShaders)
        if (!shader.reference.isNull()) GpuApi::Release(shader.reference);
    for (const auto& buffer : command.vertexBuffers)
        if (!buffer.reference.isNull()) GpuApi::Release(buffer.reference);
    if (!command.indexBuffer.reference.isNull()) GpuApi::Release(command.indexBuffer.reference);
    if (!command.outputMergerRef.isNull()) GpuApi::Release(command.outputMergerRef);
    for (const auto& sampler : command.computeSamplers)
        if (!sampler.reference.isNull()) GpuApi::Release(sampler.reference);
    for (auto reference : command.retainedBuffers) GpuApi::Release(reference);
    for (auto buffer : command.uploads) cp2077::web::DestroyGpuBuffer(buffer);
    for (const auto& lease : command.stagingArenas) StagingArena::Release(lease);
    // Ring pages return only here (GPU retirement, dropped-list retirement or shutdown), never earlier.
    for (const auto& page : command.uniformPages) UniformRing::Release(page);
    command.uniformPages.clear();
    command.deferredConstants.clear(); command.deferredConstantSlots.clear(); // [flicker3]
    command.uniformBlocks.clear();
    for (auto& block : command.systemDrawBlocks) SystemDrawArena::Release(block);
    command.systemDrawBlocks.clear();
    command.stagingArenas.clear();
}
void MarkFenceCompleteLocked(Uint64 token) {
    if (!token) return;
    auto next = completedFences.upper_bound(token);
    Uint64 first = token, last = token;
    if (next != completedFences.begin()) {
        auto previous = std::prev(next);
        if (previous->second >= token) return;
        if (previous->second + 1 == token) { first = previous->first; completedFences.erase(previous); }
    }
    if (next != completedFences.end() && next->first == token + 1) {
        last = next->second; completedFences.erase(next);
    }
    completedFences.emplace(first, last);
}
struct RetiredSubmission { std::vector<CommandEntry> commands; Uint64 token = 0; double submittedAt = 0; };
// [skperf] earlyFlush: tokens whose GPU work has not completed yet (erased in the completion callback).
std::mutex gpuPendingMutex;
std::condition_variable gpuPendingDone;
std::set<Uint64> gpuPendingSubmissions;
void RetireSubmittedCommandsOnWorker(bool completed, void* userdata) {
    std::unique_ptr<RetiredSubmission> submission(static_cast<RetiredSubmission*>(userdata));
    Perf11Pools::UsTimer retireTimer(Perf11Pools::frameRetires, Perf11Pools::frameRetireUs); // [skperf]
    const auto token=submission->token;
    if(token<=16)std::fprintf(stderr,"CP2077 GPU retirement probe: phase=worker-begin token=%llu completed=%u main=%u\n",
        static_cast<unsigned long long>(token),unsigned(completed),unsigned(emscripten_is_main_runtime_thread()));
    for (auto& command : submission->commands) ReleaseCommandResources(command);
    {
        std::lock_guard<std::mutex> lock(fenceMutex);
        if (completed) for (const auto& command : submission->commands) MarkFenceCompleteLocked(command.completionFence);
        pendingSubmissions.erase(submission->token);
    }
    submissionsRetired.notify_all();
    if(token<=16)std::fprintf(stderr,"CP2077 GPU retirement probe: phase=returned token=%llu\n",static_cast<unsigned long long>(token));
}
struct RetirementJob {bool completed;void* userdata;};
std::mutex retirementMutex;
std::condition_variable retirementAvailable;
std::deque<RetirementJob> retirementJobs;
std::once_flag retirementStartup;
bool retirementWorkerReady=false;
void* RunRetirementWorker(void*) {
    for(;;) {
        RetirementJob job;
        {
            std::unique_lock<std::mutex> lock(retirementMutex);
            retirementAvailable.wait(lock,[]{return !retirementJobs.empty();});
            job=retirementJobs.front();retirementJobs.pop_front();
        }
        // Registry locks and proxying GPU releases must never run inside the
        // browser's queue callback: a recording worker may own those locks
        // while awaiting another asynchronous browser GPU operation.
        RetireSubmittedCommandsOnWorker(job.completed,job.userdata);
    }
    return nullptr;
}
bool EnsureRetirementWorker() {
    std::call_once(retirementStartup,[]{
        pthread_t thread;
        retirementWorkerReady=pthread_create(&thread,nullptr,RunRetirementWorker,nullptr)==0;
        if(retirementWorkerReady)pthread_detach(thread);
    });
    return retirementWorkerReady;
}
void RetireSubmittedCommands(bool completed,void* userdata) {
    // This callback only publishes work. Actual resource release and fence
    // publication still follow real GPU completion, in their original order.
    const auto token=static_cast<RetiredSubmission*>(userdata)->token;
    {
        // [skperf] GPU completion: earlyFlush release + frame probe GPU latency / busy estimate (queue order).
        const double now=emscripten_get_now();
        const double submittedAt=static_cast<RetiredSubmission*>(userdata)->submittedAt;
        if (submittedAt && Perf11Pools::PerfProbeOn()) {
            static double lastDone=0;
            const double latency=now-submittedAt, busy=now-std::max(submittedAt,lastDone);
            lastDone=now;
            Perf11Pools::frameGpuJobs.fetch_add(1,std::memory_order_relaxed);
            Perf11Pools::frameGpuLatencyUs.fetch_add((unsigned long long)(latency*1000.0),std::memory_order_relaxed);
            Perf11Pools::NoteMax(Perf11Pools::frameGpuLatencyMaxUs,(unsigned long long)(latency*1000.0));
            if (busy>0) Perf11Pools::frameGpuBusyUs.fetch_add((unsigned long long)(busy*1000.0),std::memory_order_relaxed);
        }
        { std::lock_guard<std::mutex> lock(gpuPendingMutex); gpuPendingSubmissions.erase(token); }
        gpuPendingDone.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(retirementMutex);
        retirementJobs.push_back({completed,userdata});
    }
    retirementAvailable.notify_one();
    if(token<=16)std::fprintf(stderr,"CP2077 GPU retirement probe: phase=queued token=%llu\n",static_cast<unsigned long long>(token));
}
// perf12: asynchronous list submission. CloseAndSubmitCommandLists used to encode every queued draw/copy, write the
// uniform blocks, upload mapped buffers and submit through synchronous main-thread handoffs, so the submitting render
// worker waited for the (busy) main thread every frame. It now claims and closes the lists, binds staging arenas and
// diffs mapped buffers on the worker, appends one job to the deferred GPU FIFO (in submitter/token order, under
// submitOrderMutex) and returns. The job runs on the main thread and does the same work: encode, staging/uniform/
// mapped writes, finish, submit, completion callback. Fences and resource retirement are unchanged: they complete
// only from the queue work-done callback registered after the submit, or from a failure retire after the job has
// stopped using the entries. Queue-touching main-thread handlers drain the FIFO first (DrainDeferredGpuWork), so
// writes, readback maps, idle waits, destroys and present stay ordered after earlier submissions. Validation scopes
// are polled, not awaited: a member whose scopes already failed is dropped (as before); one whose scopes are still
// pending is submitted on its own, so if it is invalid the browser rejects only that list.
struct DeferredSubmitJob {
    RetiredSubmission* submission = nullptr;
    std::vector<cp2077::web::DeferredMappedGpuWrite> mappedWrites;
    bool failed = false; // token exhaustion or mapped diff failure: nothing is submitted (old behaviour)
    // ?asyncSubmit=wait: the submitter blocks on this until the job has finished (signalled as the job's last action).
    struct Waiter { std::mutex mutex; std::condition_variable done; bool finished = false; };
    Waiter* waiter = nullptr;
};
std::atomic<unsigned> deferredSubmitJobs{0}, deferredSubmitIsolated{0}, deferredSubmitValidationDropped{0}, deferredSubmitMemberFailed{0};
void RunDeferredSubmit(void* argument) {
    std::unique_ptr<DeferredSubmitJob> job(static_cast<DeferredSubmitJob*>(argument));
    struct SignalWaiter {
        DeferredSubmitJob::Waiter* waiter;
        ~SignalWaiter() { if (!waiter) return; { std::lock_guard<std::mutex> lock(waiter->mutex); waiter->finished = true; } waiter->done.notify_all(); }
    } signalWaiter{job->waiter};
    auto* submission = job->submission;
    const bool phaseTimed = Perf11Pools::PerfProbeOn(); // [skperf] round 8 main probe
    double phaseLast = phaseTimed ? emscripten_get_now() : 0;
    auto phaseMark = [&](unsigned phase) { if (!phaseTimed) return; const double now = emscripten_get_now(); Perf11Pools::mainPhaseUs[phase].fetch_add((unsigned long long)((now - phaseLast) * 1000.0), std::memory_order_relaxed); phaseLast = now; };
    if (phaseTimed) { std::size_t bytes = 0; for (const auto& write : job->mappedWrites) bytes += write.bytes.size(); Perf11Pools::mainMappedBytes.fetch_add(bytes, std::memory_order_relaxed); Perf11Pools::mainJobLists.fetch_add(submission->commands.size(), std::memory_order_relaxed); }
    // Mapped bytes were committed to the shadow at diff time: always write them, even if no member submits.
    if (!job->mappedWrites.empty()) {
        std::vector<cp2077::web::GpuBufferWrite> writes;
        writes.reserve(job->mappedWrites.size());
        for (const auto& write : job->mappedWrites) writes.push_back({write.buffer, write.offset, write.bytes.data(), write.bytes.size()});
        std::unique_ptr<bool[]> issued(new bool[writes.size()]());
        bool ok = cp2077::web::WriteGpuBufferRanges(writes.data(), writes.size(), issued.get());
        for (std::size_t i = 0; ok && i < writes.size(); ++i) ok = issued[i];
        static std::atomic<unsigned> reports{0};
        if (!ok && reports.fetch_add(1) < 8) std::fputs("CP2077 perf12 deferred mapped write not issued (device not ready)\n", stderr);
    }
    std::vector<WGPUCommandEncoder> encoders;
    std::vector<unsigned char> isolated;
    encoders.reserve(submission->commands.size());
    const double jobStarted = emscripten_get_now(); // [skperf] submit probe
    phaseMark(0);
    for (auto& command : submission->commands) {
        SharedConv::EncodePrefix(command, encoders, job->failed); // [skperf] shareConv: before the list, queue order
        phaseMark(1);
        if (!command.encoder) continue;
        cp2077::web::ServiceGpuAllocationRequests(); // [nomadperf] workers waiting for a new buffer go first
        phaseMark(2);
        if (!job->failed) {
            if (!command.failed) FlushPendingBufferCopies(command);
            phaseMark(3);
            if (phaseTimed) Perf11Pools::mainUniformBlocks.fetch_add(command.uniformBlocks.size(), std::memory_order_relaxed);
            if (!command.failed) FlushCommandUniforms(command);
            phaseMark(4);
            if (command.failed) {
                const unsigned n = deferredSubmitMemberFailed.fetch_add(1) + 1;
                if (n <= 32) std::fprintf(stderr, "CP2077 submit batch member failed: list=%u type=%u failed=1 firstFailure=%s:%d debugDepth=0 deferred=1\n",
                    command.id, unsigned(command.type), command.failedOperation ? command.failedOperation : "-", command.failedLine);
            }
        }
        if (job->failed || command.failed) { cp2077::web::ReleaseGpuCommandEncoder(command.encoder); command.encoder = nullptr; continue; }
        encoders.push_back(command.encoder); command.encoder = nullptr;
    }
    bool validationDropped = false;
    std::size_t kept = 0;
    isolated.resize(encoders.size());
    for (std::size_t i = 0; i < encoders.size(); ++i) {
        const int state = cp2077::web::PollGpuCommandValidation(encoders[i]);
        if (state == 1) {
            const unsigned n = deferredSubmitValidationDropped.fetch_add(1) + 1;
            if (n <= 16 || (n & (n - 1)) == 0)
                std::fprintf(stderr, "CP2077 submit encoder dropped (validation): index=%zu count=%zu total=%u\n", i, encoders.size(), n);
            cp2077::web::ReleaseGpuCommandEncoder(encoders[i]);
            validationDropped = true;
            continue;
        }
        if (state == 2) deferredSubmitIsolated.fetch_add(1, std::memory_order_relaxed);
        encoders[kept] = encoders[i]; isolated[kept] = state == 2 ? 1 : 0; ++kept;
    }
    encoders.resize(kept); isolated.resize(kept);
    const unsigned n = deferredSubmitJobs.fetch_add(1) + 1;
    if (n <= 4 || (n <= (1u << 20) && (n & (n - 1)) == 0))
        std::fprintf(stderr, "CP2077 perf12 deferred submit probe: jobs=%u isolatedMembers=%u validationDropped=%u memberFailed=%u\n",
            n, deferredSubmitIsolated.load(), deferredSubmitValidationDropped.load(), deferredSubmitMemberFailed.load());
    if (job->failed || (encoders.empty() && validationDropped)) {
        std::fputs("CP2077 WebGPU: command submission failed\n", stderr);
        RetireSubmittedCommands(false, submission);
        return;
    }
    // Nothing to submit (no encoders, or every member failed while encoding): same as the old zero-count submit.
    if (encoders.empty()) { RetireSubmittedCommands(true, submission); return; }
    phaseMark(5);
    struct FinishMark { decltype(phaseMark)& mark; ~FinishMark() { mark(6); } } finishMark{phaseMark};
    const double submitStarted = emscripten_get_now(); // [skperf] submit probe: flush/encode vs finish+submit
    {
        unsigned isolatedCount = 0; for (auto flag : isolated) isolatedCount += flag ? 1u : 0u;
        Perf11Pools::workJobs.fetch_add(1, std::memory_order_relaxed);
        Perf11Pools::workJobEncoders.fetch_add(encoders.size(), std::memory_order_relaxed);
        Perf11Pools::workJobIsolated.fetch_add(isolatedCount, std::memory_order_relaxed);
        Perf11Pools::workJobFlushUs.fetch_add(static_cast<unsigned long long>((submitStarted - jobStarted) * 1000.0), std::memory_order_relaxed);
    }
    struct SubmitTimer { double started; ~SubmitTimer() { Perf11Pools::workJobSubmitUs.fetch_add(static_cast<unsigned long long>((emscripten_get_now() - started) * 1000.0), std::memory_order_relaxed); } } submitTimer{submitStarted};
    submission->submittedAt = submitStarted; // [skperf] frame probe GPU latency
    if (!cp2077::web::SubmitGpuCommandEncodersOnMain(encoders.data(), isolated.data(), encoders.size(), RetireSubmittedCommands, submission)) {
        std::fputs("CP2077 WebGPU: command submission failed\n", stderr);
        RetireSubmittedCommands(false, submission);
    }
}
bool DrainGpuSubmissions() {
    Uint64 target;
    { std::lock_guard<std::mutex> lock(fenceMutex); target = nextSubmissionToken - 1; }
    if (!cp2077::web::WaitGpuQueueIdle()) return false;
    std::unique_lock<std::mutex> lock(fenceMutex);
    submissionsRetired.wait(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; });
    return true;
}
using GeometryBinding = CommandEntry::GeometryBufferBinding;
void ReleaseGeometryBinding(GeometryBinding& binding) {
    if (!binding.reference.isNull()) GpuApi::Release(binding.reference);
    binding = {};
}
bool PrepareGeometryBinding(const GpuApi::BufferRef& reference, Uint32 offset, Uint8 stride,
                            bool index, GeometryBinding& binding) {
    if (reference.isNull()) return true;
    GpuApi::BufferDesc desc;
    WGPUBuffer native = nullptr;
    if (!cp2077::web::AcquireGpuBuffer(reference, desc, native)) return false;
    binding.reference = reference;
    binding.buffer = native;
    binding.offset = offset;
    if (!native || offset >= desc.size) return false;
    binding.bytes = desc.size - offset;
    if (index) {
        if (desc.category == GpuWrapApi::BCC_Index16Bit || desc.category == GpuWrapApi::BCC_Index16BitUAV ||
            desc.category == GpuWrapApi::BCC_VertexIndex16Bit) binding.indexFormat = WGPUIndexFormat_Uint16;
        else if (desc.category == GpuWrapApi::BCC_Index32Bit) binding.indexFormat = WGPUIndexFormat_Uint32;
        else return false;
        const Uint32 width = binding.indexFormat == WGPUIndexFormat_Uint16 ? 2 : 4;
        return offset % width == 0 && binding.bytes % width == 0;
    }
    if (desc.category != GpuWrapApi::BCC_Vertex && desc.category != GpuWrapApi::BCC_VertexUAV &&
        desc.category != GpuWrapApi::BCC_VertexIndex16Bit) return false;
    if (!stride || stride % 4 || offset % 4) return false;
    binding.stride = stride;
    return true;
}
bool CommitGeometryBindings(Uint32 start, Uint32 count, GeometryBinding* vertices,
                            bool setIndex, GeometryBinding& index,
                            bool setLayout, GpuApi::VertexLayoutRef layout) {
    std::vector<GpuApi::BufferRef> previous;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (command.type != GpuApi::CommandListType::Default) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
        for (Uint32 i = 0; i < count; ++i) {
            auto& target = command.vertexBuffers[start+i];
            previous.push_back(target.reference); target = vertices[i]; vertices[i] = {};
        }
        if (setIndex) {
            previous.push_back(command.indexBuffer.reference); command.indexBuffer = index; index = {};
        }
        if (setLayout) command.vertexLayout = layout;
    }
    for (auto ref : previous) if (!ref.isNull()) GpuApi::Release(ref);
    return true;
}
// [skperf] round 60 geoKeep (?geoKeep=1): the same checks as PrepareGeometryBinding on an already acquired buffer.
bool CpFillGeometryBinding(const GpuApi::BufferRef& reference, const GpuApi::BufferDesc& desc, WGPUBuffer native,
                           Uint32 offset, Uint8 stride, bool index, GeometryBinding& binding) {
    binding.reference = reference;
    binding.buffer = native;
    binding.offset = offset;
    if (!native || offset >= desc.size) return false;
    binding.bytes = desc.size - offset;
    if (index) {
        if (desc.category == GpuWrapApi::BCC_Index16Bit || desc.category == GpuWrapApi::BCC_Index16BitUAV ||
            desc.category == GpuWrapApi::BCC_VertexIndex16Bit) binding.indexFormat = WGPUIndexFormat_Uint16;
        else if (desc.category == GpuWrapApi::BCC_Index32Bit) binding.indexFormat = WGPUIndexFormat_Uint32;
        else return false;
        const Uint32 width = binding.indexFormat == WGPUIndexFormat_Uint16 ? 2 : 4;
        return offset % width == 0 && binding.bytes % width == 0;
    }
    if (desc.category != GpuWrapApi::BCC_Vertex && desc.category != GpuWrapApi::BCC_VertexUAV &&
        desc.category != GpuWrapApi::BCC_VertexIndex16Bit) return false;
    if (!stride || stride % 4 || offset % 4) return false;
    binding.stride = stride;
    return true;
}
bool CpGeoKeepOn() {
    static const int v = [] {
        const int r = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('geoKeep') === '0' ? 0 : 1; } catch (e) { return 1; } }); // [skperf] round 61: default ON (tc82); ?geoKeep=0 = old
        std::fprintf(stderr, "CP2077 skperf switches: geoKeep=%d\n", r); return r; }();
    return v != 0;
}
// geoKeep: one bufferMutex lock per DISTINCT buffer (shared vertex/index meshes bind the same buffer in every slot) for the
// acquires, one for all releases of the replaced bindings, and no heap vector. Same bindings, same reference counts.
bool CpBindGeometryBatchKeep(Uint32 start, Uint32 count, const GpuApi::BufferRef* refs,
                             const Uint8* strides, const Uint32* offsets, bool setIndex,
                             const GpuApi::BufferRef& indexRef, Uint32 indexOffset,
                             bool setLayout, GpuApi::VertexLayoutRef layout) {
    constexpr Uint32 kMax = GpuApi::VERTEX_LAYOUT_MAX_SLOTS + 1;
    std::array<GeometryBinding, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> vertices{};
    GeometryBinding index;
    bool valid = start <= GpuApi::VERTEX_LAYOUT_MAX_SLOTS &&
                 count <= GpuApi::VERTEX_LAYOUT_MAX_SLOTS - start &&
                 (!count || (refs && strides && offsets)) &&
                 (!setLayout || layout.isNull() || GpuApi::GetVertexLayoutDesc(layout));
    // distinct non-null references with multiplicity (slot order; index last)
    struct Distinct { GpuApi::BufferRef ref; int uses = 0; GpuApi::BufferDesc desc; WGPUBuffer native = nullptr; bool held = false; };
    std::array<Distinct, kMax> distinct; Uint32 distinctCount = 0;
    auto note = [&](const GpuApi::BufferRef& r) { if (r.isNull()) return; for (Uint32 d = 0; d < distinctCount; ++d) if (distinct[d].ref.ID() == r.ID()) { ++distinct[d].uses; return; } distinct[distinctCount].ref = r; distinct[distinctCount].uses = 1; ++distinctCount; };
    if (valid) { for (Uint32 i = 0; i < count; ++i) note(refs[i]); if (setIndex) note(indexRef); }
    if (valid) for (Uint32 d = 0; d < distinctCount; ++d) {
        if (!cp2077::web::AcquireGpuBufferMulti(distinct[d].ref, distinct[d].uses, distinct[d].desc, distinct[d].native)) { valid = false; break; }
        distinct[d].held = true;
    }
    auto find = [&](const GpuApi::BufferRef& r) -> const Distinct* { for (Uint32 d = 0; d < distinctCount; ++d) if (distinct[d].ref.ID() == r.ID()) return &distinct[d]; return nullptr; };
    if (valid) for (Uint32 i = 0; i < count; ++i) {
        if (refs[i].isNull()) continue;
        const Distinct* d = find(refs[i]);
        if (!d || !CpFillGeometryBinding(refs[i], d->desc, d->native, offsets[i], strides[i], false, vertices[i])) { valid = false; break; }
    }
    if (valid && setIndex && !indexRef.isNull()) {
        const Distinct* d = find(indexRef);
        if (!d || !CpFillGeometryBinding(indexRef, d->desc, d->native, indexOffset, 0, true, index)) valid = false;
    }
    if (!valid) {
        // give back exactly what was acquired (bindings only hold copies of the reference, never their own count)
        GpuApi::BufferRef back[2 * kMax]; std::size_t n = 0;
        for (Uint32 d = 0; d < distinctCount; ++d) if (distinct[d].held) for (int u = 0; u < distinct[d].uses && n < 2 * kMax; ++u) back[n++] = distinct[d].ref;
        cp2077::web::ReleaseGpuBuffersBatch(back, n);
        return false;
    }
    GpuApi::BufferRef previous[kMax]; std::size_t previousCount = 0; bool committed = false;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        bool ok = !(entry == commands.end() || entry->second.owner != std::this_thread::get_id());
        if (ok && entry->second.failed) ok = false;
        if (ok && entry->second.type != GpuApi::CommandListType::Default) { MarkBrowserCommandFailed(entry->second, __func__, __LINE__); ok = false; }
        if (ok) {
        auto& command = entry->second;
        for (Uint32 i = 0; i < count; ++i) {
            auto& target = command.vertexBuffers[start+i];
            previous[previousCount++] = target.reference; target = vertices[i]; vertices[i] = {};
        }
        if (setIndex) { previous[previousCount++] = command.indexBuffer.reference; command.indexBuffer = index; index = {}; }
        if (setLayout) command.vertexLayout = layout;
        committed = true;
        }
    }
    if (!committed) {
        // same as the old path: the temporary bindings are released (outside the command lock)
        GpuApi::BufferRef back[kMax]; std::size_t n = 0;
        for (Uint32 i = 0; i < count; ++i) if (!vertices[i].reference.isNull()) back[n++] = vertices[i].reference;
        if (!index.reference.isNull()) back[n++] = index.reference;
        cp2077::web::ReleaseGpuBuffersBatch(back, n);
        return false;
    }
    cp2077::web::ReleaseGpuBuffersBatch(previous, previousCount);
    return true;
}
void BindGeometryBatch(Uint32 start, Uint32 count, const GpuApi::BufferRef* refs,
                       const Uint8* strides, const Uint32* offsets, bool setIndex,
                       const GpuApi::BufferRef& indexRef, Uint32 indexOffset,
                       bool setLayout, GpuApi::VertexLayoutRef layout) {
    if (CpGeoKeepOn()) { // [skperf] round 60
        if (!CpBindGeometryBatchKeep(start, count, refs, strides, offsets, setIndex, indexRef, indexOffset, setLayout, layout)) {
            cp2077::web::FailBoundGpuCommand();
            std::fputs("CP2077 WebGPU: invalid geometry binding or unavailable graphics command list\n", stderr);
        }
        return;
    }
    std::array<GeometryBinding, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> vertices{};
    GeometryBinding index;
    bool valid = start <= GpuApi::VERTEX_LAYOUT_MAX_SLOTS &&
                 count <= GpuApi::VERTEX_LAYOUT_MAX_SLOTS - start &&
                 (!count || (refs && strides && offsets)) &&
                 (!setLayout || layout.isNull() || GpuApi::GetVertexLayoutDesc(layout));
    if (valid) for (Uint32 i = 0; i < count; ++i) {
        if (!PrepareGeometryBinding(refs[i], offsets[i], strides[i], false, vertices[i])) {
            valid = false; break;
        }
    }
    if (valid && setIndex) valid = PrepareGeometryBinding(indexRef, indexOffset, 0, true, index);
    if (valid) valid = CommitGeometryBindings(start, count, vertices.data(), setIndex, index, setLayout, layout);
    // A successful commit transferred refs and cleared the temporary bindings.
    for (auto& buffer : vertices) ReleaseGeometryBinding(buffer);
    ReleaseGeometryBinding(index);
    if (!valid) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: invalid geometry binding or unavailable graphics command list\n", stderr);
    }
}
}
namespace GpuApi {
#include "gpu-api-frame-web.inl"
#include "gpu-api-resource-state-web.inl"
namespace {
using RenderAttachment = CommandEntry::RenderAttachment;
void ReleaseRenderAttachment(RenderAttachment& target) {
    if (target.view) cp2077::web::ReleaseGpuTextureView(target.view);
    if (!target.reference.isNull()) Release(target.reference);
    target = {};
}
bool PrepareRenderAttachment(TextureRef reference, Int32 slice, bool depth, RenderAttachment& target) {
    if (reference.isNull()) return true;
    if (!cp2077::web::RetainGpuTexture(reference)) return false;
    const auto desc = GetTextureDesc(reference);
    Release(reference);
    const Uint32 mips = desc.Internal_CalcTargetNumMips(), layers = desc.CalcTargetSlices();
    if (!mips || !layers || slice < -1 || (slice < 0 && layers != 1)) return false;
    Uint32 mip = 0, layer = 0;
    if (depth) {
        if (mips != 1 || (slice >= 0 && Uint32(slice) >= layers)) return false;
        layer = slice < 0 ? 0 : Uint32(slice);
    } else if (slice >= 0) {
        if (Uint64(slice) >= Uint64(mips) * layers) return false;
        mip = Uint32(slice) % mips; layer = Uint32(slice) / mips;
    }
    if (!cp2077::web::AcquireRenderGpuTextureView(reference, mip, layer, depth, target.view, &target.format)) return false;
    target.reference = reference;
    const auto dimensions = GetTextureLevelDesc(reference, Uint16(mip));
    target.width = dimensions.width; target.height = dimensions.height;
    target.samples = 1u << Uint32(desc.sampleCount);
    return true;
}
bool RecordAttachmentClear(const TextureRef& texture, WGPUTextureView view,
                           const WGPUColor* color, Float depth, bool hasStencil = false,
                           bool clearStencil = false, Uint8 stencil = 0,
                           const math::Rect* rect = nullptr, WGPUTextureFormat format = WGPUTextureFormat_Undefined,
                           Uint32 width = 0, Uint32 height = 0, Uint32 samples = 1) {
    SharedConv::NoteWrite(texture); // [skperf]
    bool transferred = false, viewQueued = false;
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        auto entry = commands.find(bound.ID());
        if (entry != commands.end() && entry->second.owner == std::this_thread::get_id()) {
            auto& command = entry->second;
            if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
            if (!command.failed) {
                // The clear is queued in encoder order with the list's draws/copies and encoded by the next batched
                // main-thread flush. It used to flush and then encode through two synchronous handoffs while holding
                // commandMutex, so every render worker stalled for a main-thread delivery (30+ ms) per clear.
                // The queued work owns `view` (released on the main thread once encoded, or when the work is
                // dropped unexecuted). Same Encode* calls and arguments as before.
                const bool hasColor = color != nullptr, hasRect = rect != nullptr;
                const WGPUColor colorValue = color ? *color : WGPUColor{};
                const Uint32 rx = rect ? Uint32(rect->m_left) : 0, ry = rect ? Uint32(rect->m_top) : 0;
                const Uint32 rw = rect ? Uint32(rect->m_right-rect->m_left) : 0, rh = rect ? Uint32(rect->m_bottom-rect->m_top) : 0;
                auto hold = std::make_shared<QueuedTextureViewHold>(view);
                const bool queued = QueuePendingCustom(command, [=](WGPUCommandEncoder encoder) {
                    const bool issued = hasColor
                        ? hasRect ? cp2077::web::EncodeGpuColorRectClear(encoder, hold->view, format, width, height, samples,
                            rx, ry, rw, rh, colorValue)
                        : cp2077::web::EncodeGpuColorClear(encoder, hold->view, colorValue)
                        : hasRect ? cp2077::web::EncodeGpuDepthRectClear(encoder, hold->view, format, width, height, samples,
                            rx, ry, rw, rh, depth, hasStencil, clearStencil, stencil)
                        : cp2077::web::EncodeGpuDepthClear(encoder, hold->view, depth, hasStencil, clearStencil, stencil);
                    hold->Release();
                    return issued;
                });
                if (queued) { command.retainedTextures.push_back(texture); transferred = true; viewQueued = true; }
                else { hold->view = nullptr; MarkBrowserCommandFailed(command, __func__, __LINE__); }
            }
        }
    }
    if (!viewQueued) cp2077::web::ReleaseGpuTextureView(view);
    if (!transferred) Release(texture);
    return transferred;
}
void RejectAttachmentClear(const char* reason) {
    // [gamerender] A clear/setup inside an already-failed list is a cascade of the first failure: stay silent
    // (counted) so the real failure site is visible. The list is failed either way.
    if (cp2077::web::BoundGpuCommandAlreadyFailed()) {
        static std::atomic<unsigned> cascaded{0};
        const unsigned n = cascaded.fetch_add(1) + 1;
        if ((n & (n - 1)) == 0 && n >= 64) std::fprintf(stderr, "CP2077 WebGPU: attachment clears in already-failed lists: count=%u\n", n);
        return;
    }
    cp2077::web::FailBoundGpuCommand();
    std::fprintf(stderr, "CP2077 WebGPU: attachment clear failed: %s\n", reason);
}
}
void ClearColorTarget(const TextureRef& texture, const Vector4& value, Uint32 slice, const math::Rect* rect) {
    if (texture.isNull()) return;
    if (!cp2077::web::RetainGpuTexture(texture)) { RejectAttachmentClear("stale texture"); return; }
    const auto desc = GetTextureDesc(texture);
    Release(texture);
    const Uint32 mips = desc.Internal_CalcTargetNumMips();
    const Uint32 layers = desc.CalcTargetSlices();
    if (!mips || Uint64(slice) >= Uint64(mips) * layers) { RejectAttachmentClear("invalid color slice"); return; }
    const auto dimensions=GetTextureLevelDesc(texture,Uint16(slice%mips));
    if(rect && (rect->m_left<0 || rect->m_top<0 || rect->m_right<rect->m_left || rect->m_bottom<rect->m_top ||
        Uint32(rect->m_right)>dimensions.width || Uint32(rect->m_bottom)>dimensions.height)) {
        RejectAttachmentClear("invalid color clear rectangle");return;
    }
    if(rect && (rect->m_right==rect->m_left || rect->m_bottom==rect->m_top))return;
    WGPUTextureView view = nullptr;
    WGPUTextureFormat format;
    if (!cp2077::web::AcquireRenderGpuTextureView(texture, slice % mips, slice / mips, false, view, &format)) {
        RejectAttachmentClear("unavailable color render view"); return;
    }
    const auto* components = value.AsFloat();
    const WGPUColor color{components[0], components[1], components[2], components[3]};
    if (!RecordAttachmentClear(texture, view, &color, 0, false, false, 0,
        rect, format, dimensions.width, dimensions.height, 1u << Uint32(desc.sampleCount))) RejectAttachmentClear("unavailable graphics command");
}
static void ClearBrowserDepthTarget(const TextureRef& texture, Float value, Uint8 stencil, bool clearStencil,
                                   Int32 slice, const math::Rect* rect) {
    if (texture.isNull()) return;

    if (!cp2077::web::RetainGpuTexture(texture)) { RejectAttachmentClear("stale depth texture"); return; }
    const auto desc = GetTextureDesc(texture);
    Release(texture);
    const Uint32 layers = desc.CalcTargetSlices();
    Uint32 depthBits = 0;
    std::memcpy(&depthBits, &value, sizeof(depthBits));
    if (desc.Internal_CalcTargetNumMips() != 1 || slice < -1 ||
        (slice >= 0 && Uint32(slice) >= layers) || value < 0 || value > 1 ||
        (depthBits & 0x7f800000u) == 0x7f800000u) {
        RejectAttachmentClear("invalid depth slice, mip count or value"); return;
    }
    if (rect && (rect->m_left < 0 || rect->m_top < 0 || rect->m_right < rect->m_left ||
        rect->m_bottom < rect->m_top || Uint32(rect->m_right) > desc.width || Uint32(rect->m_bottom) > desc.height)) {
        RejectAttachmentClear("invalid depth clear rectangle"); return;
    }
    if (rect && (rect->m_right == rect->m_left || rect->m_bottom == rect->m_top)) return;
    const Uint32 first = slice < 0 ? 0 : Uint32(slice);
    const Uint32 last = slice < 0 ? layers : first + 1;
    for (Uint32 layer = first; layer < last; ++layer) {
        WGPUTextureView view = nullptr;
        WGPUTextureFormat format;
        if (!cp2077::web::AcquireRenderGpuTextureView(texture, 0, layer, true, view, &format)) {
            RejectAttachmentClear("unavailable depth render view"); return;
        }
        const bool hasStencil = format == WGPUTextureFormat_Depth24PlusStencil8 || format == WGPUTextureFormat_Depth32FloatStencil8;
        if (!RecordAttachmentClear(texture, view, nullptr, value, hasStencil, clearStencil, stencil,
            rect, format, desc.width, desc.height, 1u << Uint32(desc.sampleCount))) {
            RejectAttachmentClear("unavailable graphics command"); return;
        }
    }
}
void ClearDepthTarget(const TextureRef& texture, Float value, Int32 slice, const math::Rect* rect) {
    ClearBrowserDepthTarget(texture, value, 0, false, slice, rect);
}
void ClearDepthStencilTarget(const TextureRef& texture, Float value, Uint8 stencil, Int32 slice, const math::Rect* rect) {
    ClearBrowserDepthTarget(texture, value, stencil, true, slice, rect);
}
void SetupRenderTargets(const RenderTargetSetup& setup) {
    // [skperf] shareConv write notes (a read-only depth target is not a write).
    for (Uint32 index = 0; index < setup.numColorTargets && index < MAX_RENDERTARGETS; ++index) SharedConv::NoteWrite(setup.colorTargets[index], false);
    if (!setup.depthTargetReadOnly) SharedConv::NoteWrite(setup.depthTarget, false);
    for (Uint32 index = 0; index < setup.numUAVs && index < MAX_UAV_BINDINGS; ++index) SharedConv::NoteWrite(setup.unorderedAccessViews[index], false);
    std::array<RenderAttachment, MAX_RENDERTARGETS> colors{};
    RenderAttachment depth;
    std::array<CommandEntry::TextureBinding, MAX_UAV_BINDINGS> storage{};
    bool valid = setup.numColorTargets <= MAX_RENDERTARGETS &&
        setup.uavStartSlot <= MAX_UAV_BINDINGS && setup.numUAVs <= MAX_UAV_BINDINGS-setup.uavStartSlot;
    for (Uint32 index=0;valid && index<setup.numUAVs;++index) {
        // Buffer UAVs (including append counters) require their own binding
        // contract; do not reinterpret them as texture storage or ignore them.
        if (!setup.unorderedAccessViewsBuf[index].isNull()) { valid=false; break; }
        auto ref=setup.unorderedAccessViews[index];
        if (ref.isNull()) continue;
        auto& target=storage[setup.uavStartSlot+index];
        valid=cp2077::web::AcquireStorageGpuTextureView(ref,0,target.view);
        if (valid) target.reference=ref;
    }
    if (valid) valid = IsSetupSupported(setup);
    for (Uint32 index = 0; valid && index < setup.numColorTargets; ++index)
        valid = PrepareRenderAttachment(setup.colorTargets[index], setup.colorTargetsSlices[index], false, colors[index]);
    if (valid) valid = PrepareRenderAttachment(setup.depthTarget, setup.depthTargetSlice, true, depth);
    Uint32 width = 0, height = 0, samples = 0;
    auto checkSize = [&](const RenderAttachment& target) {
        if (!target.view) return true;
        if (!samples) { width = target.width; height = target.height; samples = target.samples; }
        return target.width == width && target.height == height && target.samples == samples;
    };
    for (const auto& target : colors) valid = valid && checkSize(target);
    valid = valid && checkSize(depth);
    if (valid) {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        auto entry = commands.find(bound.ID());
        valid = entry != commands.end() && entry->second.owner == std::this_thread::get_id();
        if (valid) {
            auto& command = entry->second;
            if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
            valid = !command.failed;
            if (valid) {
                // Swap old bindings into temporaries; release outside the command mutex.
                // Native DX12 updates only the supplied UAV slots; other
                // pixel UAV descriptors remain bound across target changes.
                for (Uint32 i=0;i<setup.numUAVs;++i) {
                    const auto slot=setup.uavStartSlot+i;
                    std::swap(command.pixelStorageTextures[slot],storage[slot]);
                }
                command.colorAttachments.swap(colors);
                std::swap(command.depthAttachment, depth);
                command.colorAttachmentCount = setup.numColorTargets;
                command.depthReadOnly = setup.depthTargetReadOnly;
                // [skperf] round 15 passHold: park the swapped-out attachments (now in colors/depth) on the list until
                // retirement instead of releasing them below; queued draws recorded against them stay valid.
                if (Perf11Pools::PassHoldOn()) {
                    for (auto& old : colors) if (old.view || !old.reference.isNull()) { command.heldAttachments.push_back(old); old = {}; }
                    if (depth.view || !depth.reference.isNull()) { command.heldAttachments.push_back(depth); depth = {}; }
                }
                if ((command.colorAttachmentCount && IsPreviewTargetSize(command.colorAttachments[0].width, command.colorAttachments[0].height)) ||
                    (command.depthAttachment.view && IsPreviewTargetSize(command.depthAttachment.width, command.depthAttachment.height)))
                    command.touchedPreview = true;
            }
        }
    }
    for (auto& texture : storage) {
        if (texture.view) cp2077::web::ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) Release(texture.reference);
    }
    for (auto& target : colors) ReleaseRenderAttachment(target);
    ReleaseRenderAttachment(depth);
    if (!valid) RejectAttachmentClear("unsupported render-target setup or unavailable graphics command");
}
void SetupBlankRenderTargets() {
    RenderTargetSetup setup;
    SetupRenderTargets(setup);
}
void TransitionSwapChainPresent(SwapChainRef swapChain) {
    if (swapChain.isNull()) return;
    const auto texture = GetBackBufferTexture(swapChain);
    if (texture.isNull() || !cp2077::web::RetainGpuTexture(texture)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    bool retained = false;
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        auto entry = commands.find(bound.ID());
        if (entry != commands.end() && entry->second.owner == std::this_thread::get_id()) {
            auto& command = entry->second;
            if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
            if (!command.failed) { command.retainedTextures.push_back(texture); retained = true; }
        }
    }
    if (!retained) { Release(texture); cp2077::web::FailBoundGpuCommand(); }
    // WebGPU tracks resource usages at pass boundaries. The canvas presenter
    // submits its CopySrc usage on the same queue after engine submissions;
    // it needs lifetime ownership here rather than a DX12 state barrier.
}
void SetViewport(const ViewportDesc& viewport) {
    Uint32 minBits = 0, maxBits = 0;
    std::memcpy(&minBits, &viewport.minDepth, sizeof(minBits));
    std::memcpy(&maxBits, &viewport.maxDepth, sizeof(maxBits));
    if (!viewport.width || !viewport.height || viewport.minDepth < 0 || viewport.maxDepth > 1 ||
        viewport.minDepth > viewport.maxDepth || (minBits & 0x7f800000u) == 0x7f800000u ||
        (maxBits & 0x7f800000u) == 0x7f800000u) {
        RejectAttachmentClear("invalid viewport"); return;
    }
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return;
    auto& command = entry->second;
    if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
    if (!command.failed) { command.viewport = viewport; command.viewportSet = true; }
}
void SetScissors(const Rect& rectangle) {
    if (rectangle.m_right < rectangle.m_left ||
        rectangle.m_bottom < rectangle.m_top) { RejectAttachmentClear("invalid scissors"); return; }
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return;
    auto& command = entry->second;
    if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
    if (!command.failed) { command.scissors = rectangle; command.scissorsSet = true; }
}
void SetDepthClip(Bool enabled) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: depth clipping requires a bound command list\n", stderr); return;
    }
    auto& command = entry->second;
    if (command.failed) return;
    if (command.type != CommandListType::Default) { MarkBrowserCommandFailed(command, __func__, __LINE__); return; }
    // Preserve the original value for pipeline translation, including disabled clipping.
    command.depthClip = enabled;
}
void SetBlendFactor(const Vector4& factor) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: blend factor requires a bound command list\n", stderr); return;
    }
    auto& command = entry->second;
    if (command.failed) return;
    if (command.type != CommandListType::Default) { MarkBrowserCommandFailed(command, __func__, __LINE__); return; }
    const auto* values = factor.AsFloat();
    command.blendFactor = {values[0], values[1], values[2], values[3]};
}
void SetStencilRefValue(Uint8 value) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: stencil reference requires a bound command list\n", stderr); return;
    }
    auto& command = entry->second;
    if (command.failed) return;
    if (command.type != CommandListType::Default) { MarkBrowserCommandFailed(command, __func__, __LINE__); return; }
    command.stencilReference = value;
}
void SetOutputMergerState(OMStateRef reference, Bool setStencil) {
    SOMState state;
    if (reference.isNull() || !cp2077::web::AcquireGpuOMState(reference, state)) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: output-merger state reference is invalid\n", stderr); return;
    }
    OMStateRef previous = OMStateRef::Null();
    bool transferred = false;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry != commands.end() && entry->second.owner == std::this_thread::get_id()) {
            auto& command = entry->second;
            if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
            if (!command.failed) {
                previous = command.outputMergerRef;
                command.outputMergerRef = reference; command.outputMergerState = state;
                if (setStencil) command.stencilReference = state.m_stencilRef;
                transferred = true;
            }
        }
    }
    if (!previous.isNull()) Release(previous);
    if (!transferred) {
        Release(reference);
        std::fputs("CP2077 WebGPU: output-merger state requires a usable graphics command list\n", stderr);
    }
}
void BindVertexBuffers(Uint32 start, Uint32 count, const BufferRef* buffers, /* [skperf] round 39: timed below */
                       const Uint8* strides, const Uint32* offsets) {
    CpApiTimer cpApiTimer(2); // [skperf] round 39
    BindGeometryBatch(start, count, buffers, strides, offsets, false, BufferRef::Null(), 0,
                      false, VertexLayoutRef::Null());
}
void BindNullVertexBuffers() {
    std::array<BufferRef, VERTEX_LAYOUT_MAX_SLOTS> refs;
    refs.fill(BufferRef::Null());
    Uint8 strides[VERTEX_LAYOUT_MAX_SLOTS]{};
    Uint32 offsets[VERTEX_LAYOUT_MAX_SLOTS]{};
    BindGeometryBatch(0, VERTEX_LAYOUT_MAX_SLOTS, refs.data(), strides, offsets,
                      false, BufferRef::Null(), 0, false, VertexLayoutRef::Null());
}
void BindIndexBuffer(const BufferRef& buffer, Uint32 offset) {
    CpApiTimer cpApiTimer(2); // [skperf] round 39
    BindGeometryBatch(0, 0, nullptr, nullptr, nullptr, true, buffer, offset, false, VertexLayoutRef::Null());
}
void BindIndirectArgs(const BufferRef& reference, Uint32 offset) {
    BufferDesc descriptor{}; WGPUBuffer buffer = nullptr;
    if (!reference.isNull()) {
        if (!cp2077::web::AcquireGpuBuffer(reference, descriptor, buffer)) {
            cp2077::web::FailBoundGpuCommand(); return;
        }
        if (descriptor.category != GpuWrapApi::BCC_IndirectUAV || offset % 4 || offset >= descriptor.size) {
            Release(reference); cp2077::web::FailBoundGpuCommand(); return;
        }
    }
    BufferRef previous = BufferRef::Null(); bool transferred = false;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry != commands.end() && entry->second.owner == std::this_thread::get_id() && !entry->second.failed) {
            auto& command = entry->second;
            if (command.type == CommandListType::Default || command.type == CommandListType::Compute) {
                previous = command.indirectArguments.reference;
                command.indirectArguments = {reference,buffer,descriptor.size,0};
                command.indirectOffset = offset; transferred = true;
            } else MarkBrowserCommandFailed(command, __func__, __LINE__);
        }
    }
    if (!previous.isNull()) Release(previous);
    if (!transferred) { if (!reference.isNull()) Release(reference); cp2077::web::FailBoundGpuCommand(); }
}
void BindVertexAndIndexBuffers(VertexLayoutRef layout, Uint32 count, const BufferRef* buffers,
                               const Uint8* strides, const Uint32* offsets,
                               const BufferRef& index, Uint32 indexOffset) {
    BindGeometryBatch(0, count, buffers, strides, offsets, true, index, indexOffset, true, layout);
}
void BindVertexAndIndexBuffersShared(VertexLayoutRef layout, const BufferRef& shared, Uint32 count,
                                     const Uint8* strides, const Uint32* offsets, Uint32 indexOffset) {
    if (!count || count > VERTEX_LAYOUT_MAX_SLOTS || shared.isNull() ||
        GetBufferDesc(shared).category != GpuWrapApi::BCC_VertexIndex16Bit) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: shared geometry requires a native vertex/index16 buffer\n", stderr); return;
    }
    std::array<BufferRef, VERTEX_LAYOUT_MAX_SLOTS> buffers;
    buffers.fill(shared);
    BindGeometryBatch(0, count, buffers.data(), strides, offsets, true, shared, indexOffset, true, layout);
}
void SetVertexFormatRaw(VertexLayoutRef layout, Bool) {
    // Match original DX12 behavior: setInputLayout is ignored by that backend.
    const bool valid = layout.isNull() || GetVertexLayoutDesc(layout);
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry=lock.Find();
    if (entry==commands.end() || entry->second.owner!=std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: vertex layout requires a bound command list\n", stderr); return;
    }
    auto& command=entry->second;
    if (command.failed) return;
    if (!valid || command.type!=CommandListType::Default) {
        MarkBrowserCommandFailed(command, __func__, __LINE__);
        std::fputs("CP2077 WebGPU: invalid vertex layout or non-graphics command list\n", stderr); return;
    }
    command.vertexLayout=layout;
}
void SetComputeShaderConstsRaw(Uint32 bytes, const void* data) {
    // Match the original API's zero/null behavior. Values are captured now,
    // then repacked for the exact selected shader when constructing bindings.
    if (!bytes || !data) return;
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: compute constants require a bound command list\n", stderr); return;
    }
    auto& command = entry->second;
    if (command.failed) return;
    if ((command.type != CommandListType::Default && command.type != CommandListType::Compute) || bytes > 65536) {
        MarkBrowserCommandFailed(command, __func__, __LINE__);
        std::fputs("CP2077 WebGPU: invalid compute constant block or command type\n", stderr); return;
    }
    command.computeNativeConstants.resize(bytes);
    std::memcpy(command.computeNativeConstants.data(), data, bytes);
    auto previous = command.computeConstantBuffers[6];
    command.computeConstantBuffers[6] = GpuApi::BufferRef::Null();
    if (!previous.isNull()) GpuApi::Release(previous);
}
namespace {
void SetGraphicsShaderConstants(unsigned stage, Uint32 bytes, const void* data) {
    if (!bytes || !data) return;
    BufferRef previous = BufferRef::Null();
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
            std::fputs("CP2077 WebGPU: graphics constants require a bound command list\n", stderr); return;
        }
        auto& command = entry->second;
        if (command.failed) return;
        if (command.type != CommandListType::Default || bytes > 65536) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return;
        }
        auto& constants = command.graphicsNativeConstants[stage];
        constants.resize(bytes); std::memcpy(constants.data(), data, bytes);
        // Original GPUAPI_SHARED_CB register is b6 for each independent stage.
        previous = command.graphicsConstantBuffers[stage][6];
        command.graphicsConstantBuffers[stage][6] = BufferRef::Null();
    }
    if (!previous.isNull()) Release(previous);
}
}
void SetVertexShaderConstsRaw(Uint32 bytes, const void* data) { SetGraphicsShaderConstants(0, bytes, data); }
void SetPixelShaderConstsRaw(Uint32 bytes, const void* data) { SetGraphicsShaderConstants(1, bytes, data); }
void SetVSC_Raw(Uint32 first, const Float* data, Uint32 count) {
    CpApiTimer cpApiTimer(3); // [skperf] round 39
    if (!count) return;
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return;
    auto& command = entry->second;
    if (command.failed) return;
    if (command.type != CommandListType::Default || !data || first >= NUM_VSC_REGS ||
        count > NUM_VSC_REGS - first) { MarkBrowserCommandFailed(command, __func__, __LINE__); return; }
    std::memcpy(command.vertexLegacyConstants.data() + first * 4, data, std::size_t(count) * 16);
    for (Uint32 index = first; index < first + count; ++index) command.vertexLegacyInitialized[index] = true;
    if (first <= VSC_Frequent_Last) command.legacyConstantsDirty |= 1;
    if (first + count > VSC_Custom_First) command.legacyConstantsDirty |= 2;
}
void SetPSC_Raw(Uint32 first, const Float* data, Uint32 count) {
    CpApiTimer cpApiTimer(3); // [skperf] round 39
    if (!count) return;
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return;
    auto& command = entry->second;
    if (command.failed) return;
    if (command.type != CommandListType::Default || !data || first >= NUM_PSC_REGS ||
        count > NUM_PSC_REGS - first) { MarkBrowserCommandFailed(command, __func__, __LINE__); return; }
    std::memcpy(command.pixelLegacyConstants.data() + first * 4, data, std::size_t(count) * 16);
    for (Uint32 index = first; index < first + count; ++index) command.pixelLegacyInitialized[index] = true;
    command.legacyConstantsDirty |= 4;
}
void BindMainConstantBuffers() {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return;
    auto& command = entry->second;
    if (command.type != CommandListType::Default) MarkBrowserCommandFailed(command, __func__, __LINE__);
    if (!command.failed) command.legacyConstantsDirty |= 7;
}
CommandListRef CreateCommandList(CommandListType type, Uint64) {
    if (type <= CommandListType::None || type >= CommandListType::MAX) return CommandListRef::Null();
    {std::lock_guard<std::mutex> lock(commandMutex);
        if(commandOperations.closing){
            if(cp2077::web::IsGpuDeviceReady()&&cp2077::web::GpuDeviceGeneration()!=commandShutdownGeneration&&commands.empty()&&!commandOperations.active)commandOperations.closing=false;
            else return CommandListRef::Null();
        }
    }
    auto encoder = cp2077::web::CreateGpuCommandEncoder();
    if (!encoder) return CommandListRef::Null();
    Uint32 id = 0;
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        if (!commandOperations.closing && nextCommandId <= Uint32(INT32_MAX)) {
            id = nextCommandId++;
            CommandEntry command{};
            command.encoder = encoder; command.type = type; command.id = id;
            commands.emplace(id, std::move(command));
        }
    }
    if (!id) cp2077::web::ReleaseGpuCommandEncoder(encoder);
    return CommandListRef{id};
}
void BindCommandList(CommandListRef reference) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto owner = std::this_thread::get_id();
    auto previous = commands.find(bound.ID());
    if (previous != commands.end() && previous->second.owner == owner) previous->second.owner = {};
    bound = CommandListRef::Null(); tlBoundEntry = {};
    if (reference.isNull()) return;
    auto entry = commands.find(reference.ID());
    if (entry == commands.end() || (entry->second.owner != std::thread::id{} && entry->second.owner != owner)) {
        std::fputs("CP2077 WebGPU: invalid or concurrently bound command list\n", stderr); return;
    }
    entry->second.owner = owner; bound = reference;
    tlBoundEntry = {&*entry, reference.ID(), commandRegistryEpoch.load()}; // [cmdlock]
}
CommandListType GetBoundCommandListType() {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    return entry != commands.end() && entry->second.owner == std::this_thread::get_id()
        ? entry->second.type : CommandListType::None;
}
void CloseAndSubmitCommandLists(const char* scopeName, const red::ArraySpan<CommandListRef>& lists,
                               CommandListSyncType syncType, job::Builder&) {
    Perf11Pools::UsTimer closeTimer(Perf11Pools::frameCloseSubmits, Perf11Pools::frameCloseSubmitUs, true); // [skperf] frame probe
    {
        // perf12: submit-site histogram (bisection aid): scope name and sync type, first 256 calls then powers of two.
        static std::atomic<unsigned> submitCalls{0};
        const unsigned n = submitCalls.fetch_add(1) + 1;
        if (n <= 256 || (n & (n - 1)) == 0)
            std::fprintf(stderr, "CP2077 perf12 submit site: n=%u scope=%s sync=%u lists=%u\n", n, scopeName ? scopeName : "-",
                unsigned(syncType), unsigned(lists.Size()));
    }
    if(!EnsureRetirementWorker()) {
        std::fputs("CP2077 WebGPU: GPU retirement worker creation failed\n",stderr);
        cp2077::web::FailBoundGpuCommand();return;
    }
    Perf10Switches(); // perf10: read the URL switches once, before any lock is taken
    const int submitMode = Perf12SubmitMode();
    const int subSwitches = Perf12SubSwitches();
    const bool asyncSubmitSwitch = submitMode != 0;
    // Batches reach the queue in the order submitters enter here. The flushes below no longer run under
    // commandMutex (which used to serialize submitters), so this mutex keeps submitters ordered among themselves
    // through SubmitGpuCommandEncoders, without blocking draws or state calls. Never taken by the main thread or
    // while commandMutex is held.
    // [opt] shared with the async present snapshot enqueue (gpu-device-web.cpp PresentGpuCanvas).
    Perf11Pools::PhaseClock phaseClock; // [skperf] round 6 close probe
    // [skperf] round 22 critpath: wall time of this submission, its wait for the submit-order mutex and its mapped diff
    struct CpCritSubmit { bool on; double start, locked = 0, scanStart = 0, scanMs = 0;
        ~CpCritSubmit() { if (on) cp2077_skperf_crit_submit(start, emscripten_get_now(), locked > 0 ? locked - start : 0, scanMs); } };
    const bool cpCritOn = cp2077_skperf_crit_on() != 0;
    CpCritSubmit cpCrit{cpCritOn, cpCritOn ? emscripten_get_now() : 0};
    std::lock_guard<std::mutex> submitOrder(cp2077::web::GpuSubmitOrderMutex());
    if (cpCritOn) cpCrit.locked = emscripten_get_now();
    phaseClock.mark(0);
    Perf11Pools::NoteSubmitScope(scopeName, unsigned(lists.Size())); // [skperf]
    std::vector<WGPUCommandEncoder> encoders, droppedEncoders;
    std::vector<CommandEntry> retired;
    bool failed = false;
    // A flush failure marks only this member failed (both flushes call MarkBrowserCommandFailed on every
    // false return); it is dropped below on its own instead of discarding every encoder in the batch.
    auto flushForSubmit = [](CommandEntry& command) {
        if (!command.failed) FlushPendingBufferCopies(command);
        if (!command.failed) FlushCommandUniforms(command);
    };
    // The flushes encode every queued draw/copy and write the list's uniforms through synchronous main-thread
    // handoffs (30+ ms each when the main thread is busy). They used to run with commandMutex held, which stalled
    // every render worker's draws and state calls for the whole batch. Validated members are now claimed for this
    // thread (owner set: no other thread can bind, submit or mutate them; teardown waits for the lease), flushed
    // without the lock, then retired under the lock exactly as before. While teardown is closing, the old
    // all-under-lock order is kept.
    std::vector<CommandEntry*> claimed;
    bool leased = false;
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        // Validate the entire batch before consuming any of the caller's handles.
        std::unordered_map<Uint32, bool> seen;
        for (Uint32 index = 0; index < lists.Size(); ++index) {
            auto reference = lists[index];
            if (reference.isNull()) continue;
            auto entry = commands.find(reference.ID());
            if (entry == commands.end() || entry->second.owner != std::thread::id{} || !seen.emplace(reference.ID(), true).second) {
                std::fputs("CP2077 WebGPU: stale, duplicate or bound command list submission\n", stderr); return;
            }
        }
        if (!commandOperations.closing) {
            ++commandOperations.active; leased = true;
            claimed.reserve(lists.Size());
            for (Uint32 index = 0; index < lists.Size(); ++index) {
                if (lists[index].isNull()) continue;
                auto& command = commands.at(lists[index].ID());
                command.owner = std::this_thread::get_id(); claimed.push_back(&command);
            }
        }
    }
    // unordered_map rehash keeps the claimed references stable; only this thread can erase them.
    // perf12: asynchronous submit (default) only binds staging arenas here; the deferred job flushes on main.
    // Startup loading keeps the synchronous path: asynchronous submission starts at render frame 1000 (?p12early=1
    // starts it at once). A pre-existing load-time engine race (SessionNewGame exits right after the
    // OnVehicleSystemAttach script call; seen on the fba4c9cb baseline too) showed up more often with faster loading.
    constexpr Uint32 kAsyncSubmitFromFrame = 1000;
    const bool asyncFrameReached = (subSwitches & 4) || submitMode == 2 ||
        publishedRenderFrame.load(std::memory_order_relaxed) >= kAsyncSubmitFromFrame;
    const bool deferSubmit = leased && asyncSubmitSwitch && asyncFrameReached && !Perf10Switch(1);
    {
        static std::atomic<bool> announced{false};
        if (deferSubmit && !announced.exchange(true))
            std::fprintf(stderr, "CP2077 perf12 asynchronous submit active from render frame %u\n",
                unsigned(publishedRenderFrame.load(std::memory_order_relaxed)));
    }
    phaseClock.mark(1);
    for (auto* command : claimed) ResolveOrderedConstants(*command); // [flicker3] submission order (submitOrderMutex held)
    phaseClock.mark(2);
    if (deferSubmit && !(subSwitches & 1)) for (auto* command : claimed) PrepareStagingArena(*command);
    else for (auto* command : claimed) flushForSubmit(*command);
    phaseClock.mark(3);
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        if (leased) { --commandOperations.active; commandOperations.idle.notify_all(); }
        retired.reserve(lists.Size());
        for (Uint32 index = 0; index < lists.Size(); ++index) {
            auto reference = lists[index];
            if (reference.isNull()) continue;
            auto found = commands.find(reference.ID());
            // Claimed members cannot be erased by other threads; only while teardown is closing (not leased) can
            // teardown have taken the list in between, and it then releases it itself.
            if (found == commands.end()) continue;
            auto& command = found->second;
            if (!leased) { ResolveOrderedConstants(command); flushForSubmit(command); }
            command.owner = {};
            if (command.touchedPreview) {
                static std::atomic<unsigned> previewReports{0};
                if (previewReports.fetch_add(1) < 64)
                    std::fprintf(stderr, "CP2077 preview list submit: list=%u type=%u drawsOk=%u drawsSkipped=%u drawsRejected=%u drawsDroppedFailed=%u previewDrawsOk=%u previewDrawsSkipped=%u dispatchesSkipped=%u failed=%u failure=%s:%d\n",
                        command.id, unsigned(command.type), command.drawsOk, command.drawsSkipped, command.drawsRejected, command.drawsDroppedFailed,
                        command.previewDrawsOk, command.previewDrawsSkipped, command.dispatchesSkipped, unsigned(command.failed),
                        command.failedOperation ? command.failedOperation : "-", command.failedLine);
            }
            if (command.failed || command.debugGroupDepth != 0) {
                static std::atomic<unsigned> reports{0};
                if (reports.fetch_add(1) < 32) std::fprintf(stderr, "CP2077 submit batch member failed: list=%u type=%u failed=%u firstFailure=%s:%d debugDepth=%u\n",
                    reference.ID(), unsigned(command.type), unsigned(command.failed),
                    command.failedOperation ? command.failedOperation : "-", command.failedLine, unsigned(command.debugGroupDepth));
                // Steady-state attribution: periodic histogram of the root failure site of dropped lists.
                static std::map<std::pair<std::string,int>, unsigned> siteCounts; static unsigned dropped = 0;
                ++siteCounts[{command.failedOperation ? command.failedOperation : "-", command.failedLine}];
                if ((++dropped & 1023) == 0) {
                    std::fprintf(stderr, "CP2077 dropped command lists: total=%u sites=", dropped);
                    for (const auto& site : siteCounts) std::fprintf(stderr, "%s:%d=%u ", site.first.first.c_str(), site.first.second, site.second);
                    std::fputc('\n', stderr);
                }
            }
            bool memberFailed = command.failed || command.debugGroupDepth != 0;
            for (const auto& snapshot : command.constantSnapshots)
                memberFailed = memberFailed || snapshot.second.locked;
            for (const auto& upload : command.bufferUploadLocks)
                memberFailed = memberFailed || upload.second.locked;
            SharedConv::Decide(command, !memberFailed && deferSubmit); // [skperf] shareConv, queue order
            Perf11Pools::NoteScopeFallback(scopeName, command.fallbackReads);
            // A failed list is dropped on its own; healthy lists in the same batch (UI, present) still submit.
            // The release proxies to the main thread: done after commandMutex is released.
            if (memberFailed) { droppedEncoders.push_back(command.encoder); command.encoder = nullptr; }
            else if (!deferSubmit) { encoders.push_back(command.encoder); command.encoder = nullptr; }
            // deferSubmit: the encoder stays in the entry; the deferred job encodes, finishes and submits it.
            retired.push_back(std::move(command)); commands.erase(found);
        }
    }
    for (auto encoder : droppedEncoders) cp2077::web::ReleaseGpuCommandEncoder(encoder);
    phaseClock.mark(4);
    // WebGPU has one queue. Serial submission provides stronger ordering than
    // the original fork/join queues, preserving dependencies without overlap.
    // Submission is synchronous on CPU; GPU completion/fences are separate.
    auto* submission = new RetiredSubmission{std::move(retired)};
    {
        std::lock_guard<std::mutex> lock(fenceMutex);
        if (nextSubmissionToken == UINT64_MAX) failed = true;
        else {
            submission->token = nextSubmissionToken++; pendingSubmissions.insert(submission->token);
            std::lock_guard<std::mutex> gpuLock(gpuPendingMutex); gpuPendingSubmissions.insert(submission->token); // [skperf]
        }
    }
    // perf10: upload only the persistently mapped buffers this batch's lists reference, then let the full
    // per-submit scan inside SubmitGpuCommandEncoders be skipped. Every GPU read of a buffer recorded in a list
    // (draw vertex/index/SRV/indirect, dispatch SRV/UAV/indirect, copy source/destination) retains its ref in
    // that list's retainedBuffers until retirement, so the set below is complete. A buffer this batch does not
    // reference keeps its dirty bytes against the committed shadow; the next batch that references it uploads them
    // before its own submit (queue.writeBuffer precedes that queue.submit). ?fullMappedFlush=1 restores the old scan.
    bool coveredMappedFlush = false;
    if (cpCritOn) cpCrit.scanStart = emscripten_get_now();
    DeferredSubmitJob* deferredJob = deferSubmit ? new DeferredSubmitJob{submission, {}, false} : nullptr;
    if (!failed && !Perf10Switch(1)) {
        static thread_local std::vector<unsigned> mappedIds, referencedIds;
        static thread_local std::unordered_set<unsigned> mappedSet, referencedSet; // [skperf] round 6: O(1) lookups, same result
        const double cpT0 = cpCritOn ? emscripten_get_now() : 0; // [skperf] round 25 sub-timers
        cp2077::web::CopyMappedUploadBufferIds(mappedIds);
        const double cpT1 = cpCritOn ? emscripten_get_now() : 0;
        referencedIds.clear(); mappedSet.clear(); referencedSet.clear();
        // [skperf] round 24 mapIds: there are only a handful of mapped upload buffers (4 in the bar) but every list
        // retains thousands of buffer refs; the per-ref hash-set lookups cost ~15 ms/frame in the bar (critpath
        // 'mappedScanMs'). With <= 16 mapped buffers, compare against the small array instead (same set of IDs, same
        // first-seen order). ?mapIds=0 = hash sets.
        static const int cpMapIds = [] { const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('mapIds') === '0' ? 0 : 1; } catch (e) { return 1; } });
            std::fprintf(stderr, "CP2077 skperf switches: mapIds=%d\n", v); return v; }();
        if (cpMapIds && !mappedIds.empty() && mappedIds.size() <= 16) {
            const std::size_t mappedCount = mappedIds.size(); bool seen[16] = {}; std::size_t found = 0;
            for (const auto& command : submission->commands) {
                for (const auto& reference : command.retainedBuffers) {
                    const unsigned id = reference.ID();
                    for (std::size_t k = 0; k < mappedCount; ++k) {
                        if (mappedIds[k] != id) continue;
                        if (!seen[k]) { seen[k] = true; referencedIds.push_back(id); ++found; }
                        break;
                    }
                    if (found == mappedCount) break;
                }
                if (found == mappedCount) break;
            }
        } else {
        mappedSet.insert(mappedIds.begin(), mappedIds.end());
        if (!mappedIds.empty())
            for (const auto& command : submission->commands) {
                for (const auto& reference : command.retainedBuffers) {
                    const unsigned id = reference.ID();
                    if (!mappedSet.count(id) || !referencedSet.insert(id).second) continue;
                    referencedIds.push_back(id);
                    if (referencedIds.size() == mappedIds.size()) break;
                }
                if (referencedIds.size() == mappedIds.size()) break;
            }
        }
        const double cpT2 = cpCritOn ? emscripten_get_now() : 0;
        static std::atomic<unsigned> batches{0}, skipped{0}, narrowed{0};
        const unsigned n = ++batches;
        if (referencedIds.empty()) ++skipped; else if (referencedIds.size() < mappedIds.size()) ++narrowed;
        if (n <= 4 || (n <= 65536 && !(n & (n - 1))))
            std::fprintf(stderr, "CP2077 mapped flush per-batch probe: batches=%u skippedNoMappedRef=%u narrowed=%u referenced=%zu mapped=%zu\n",
                n, skipped.load(), narrowed.load(), referencedIds.size(), mappedIds.size());
        if (deferredJob && (subSwitches & 2)) { if (!cp2077::web::FlushOriginalMappedGpuBufferIds(referencedIds.data(), referencedIds.size())) failed = true; }
        else if (deferredJob) { if (!cp2077::web::FlushOriginalMappedGpuBufferIdsDeferred(referencedIds.data(), referencedIds.size(), deferredJob->mappedWrites)) failed = true; }
        else if (!cp2077::web::FlushOriginalMappedGpuBufferIds(referencedIds.data(), referencedIds.size())) failed = true;
        else { cp2077::web::SetMappedGpuFlushCovered(true); coveredMappedFlush = true; }
        if (cpCritOn) cp2077_skperf_crit_scan_parts(cpT1 - cpT0, cpT2 - cpT1, emscripten_get_now() - cpT2);
    }
    phaseClock.mark(5);
    if (cpCritOn) cpCrit.scanMs = emscripten_get_now() - cpCrit.scanStart;
    struct EnqueueMark { Perf11Pools::PhaseClock& clock; ~EnqueueMark() { clock.mark(6); } } enqueueMark{phaseClock};
    if (deferredJob) {
        // perf12: still under submitOrderMutex, so FIFO order equals token order. Returns without waiting.
        deferredJob->failed = failed;
        DeferredSubmitJob::Waiter waiter;
        if (submitMode == 2) deferredJob->waiter = &waiter;
        cp2077::web::EnqueueDeferredGpuWork(RunDeferredSubmit, deferredJob);
        if (submitMode == 2) {
            std::unique_lock<std::mutex> lock(waiter.mutex);
            waiter.done.wait(lock, [&waiter] { return waiter.finished; });
        }
        return;
    }
    const bool submitted = !failed &&
        cp2077::web::SubmitGpuCommandEncoders(encoders.data(), encoders.size(), RetireSubmittedCommands, submission);
    // The mark is consumed by the flush inside the call; clear it in case the call returned before flushing.
    if (coveredMappedFlush) cp2077::web::SetMappedGpuFlushCovered(false);
    if (!submitted) {
        std::fputs("CP2077 WebGPU: command submission failed\n", stderr);
        for (auto encoder : encoders) cp2077::web::ReleaseGpuCommandEncoder(encoder);
        RetireSubmittedCommands(false, submission);
    }
    // Completion owns original refs, buffers, views, groups and pipelines until
    // the actual submitted queue work finishes (or fails/cancels).
}
GpuFence GetGpuFence() {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id() || entry->second.failed)
        return GpuFence::Null();
    if (!entry->second.completionFence) {
        std::lock_guard<std::mutex> fenceLock(fenceMutex);
        if (nextFenceToken == UINT64_MAX) { MarkBrowserCommandFailed(entry->second, __func__, __LINE__); return GpuFence::Null(); }
        entry->second.completionFence = nextFenceToken++;
    }
    return GpuFence(entry->second.completionFence);
}
Bool IsGpuFenceComplete(GpuFence fence, Uint64) {
    if (!fence) return true;
    // The native completedValue parameter is an optional cached-query hint.
    // Browser completion always checks the real queue callback's history.
    std::lock_guard<std::mutex> lock(fenceMutex);
    auto next = completedFences.upper_bound(fence.id);
    return next != completedFences.begin() && std::prev(next)->second >= fence.id;
}
Uint32 GetCurrentGpuThread() {
    return GetBoundCommandListType() == CommandListType::None ? UINT32_MAX : 0;
}
void Flush() {
    // The engine calls Flush once per FrameTick and documents it as non-waiting. Draining the queue here
    // serialized CPU and GPU every frame; instead bound the work in flight to two frames: wait only for the
    // submissions already issued two Flush calls ago. Per-submit completion callbacks still retire resources.
    // [flushdepth] ?flushDepth=2..4 frames of GPU work in flight (default 2). 2026-10-23 Badlands: the game thread waited
    // ~44 ms/frame here for GPU completion while every job worker was idle (GPU-process latency, not GPU load).
    static const int depth = [] { const int v = MAIN_THREAD_EM_ASM_INT({ try { const d = Number(new URLSearchParams(location.search).get('flushDepth')); return d >= 2 && d <= 4 ? d : 2; } catch (e) { return 2; } });
        std::fprintf(stderr, "CP2077 skperf switches: flushDepth=%d\n", v); return v; }();
    static std::mutex flushMutex;
    static Uint64 issuedAtFlush[4] = {0, 0, 0, 0};
    static Uint64 releaseTarget = 0; // [skperf] earlyFlush: retirement may lag GPU completion by at most one more Flush
    std::lock_guard<std::mutex> flushLock(flushMutex);
    const bool timed = Perf11Pools::PerfProbeOn(); // [skperf] frame probe: Flush wait
    const double waitStarted = timed ? emscripten_get_now() : 0;
    struct FlushTimer { bool on; double start; ~FlushTimer() { if (!on) return; const auto us = (unsigned long long)((emscripten_get_now() - start) * 1000.0);
        Perf11Pools::frameFlushCalls.fetch_add(1, std::memory_order_relaxed); Perf11Pools::frameFlushWaitUs.fetch_add(us, std::memory_order_relaxed); Perf11Pools::NoteMax(Perf11Pools::frameFlushMaxUs, us); } } flushTimer{timed, waitStarted};
    std::unique_lock<std::mutex> lock(fenceMutex);
    const Uint64 target = issuedAtFlush[0];
    for (int i = 0; i + 1 < depth; ++i) issuedAtFlush[i] = issuedAtFlush[i + 1];
    issuedAtFlush[depth - 1] = nextSubmissionToken - 1;
    if (!target) return;
    if (Perf11Pools::EarlyFlushOn()) {
        // [skperf] earlyFlush: GPU completion of the submissions two Flushes ago, and full release (old condition)
        // of the ones three Flushes ago, so retirement backlog (memory) stays bounded.
        const Uint64 released = releaseTarget;
        releaseTarget = target;
        if (released) submissionsRetired.wait(lock, [released] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > released; });
        lock.unlock();
        std::unique_lock<std::mutex> gpuLock(gpuPendingMutex);
        gpuPendingDone.wait(gpuLock, [target] { return gpuPendingSubmissions.empty() || *gpuPendingSubmissions.begin() > target; });
        return;
    }
    releaseTarget = 0;
    submissionsRetired.wait(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; });
}
void SyncWithAsyncFlush(job::Builder&) {
    if (!DrainGpuSubmissions()) std::fputs("CP2077 WebGPU: asynchronous flush synchronization failed\n", stderr);
}
}
#include "gpu-api-query-web.inl"
namespace cp2077::web {
// Caller holds commandMutex. Copy native scalar bit patterns at this command
// position, after earlier CPU uploads/GPU writes and before this shader read.
// The destination is a fresh immutable read snapshot; later native writes
// cannot change an already encoded draw/dispatch. WebGPU zeroes padding only.
[[maybe_unused]] static WGPUBuffer RepackNativeGpuConstant(CommandEntry& command, const UniformLayout& layout,
                                         const GpuApi::BufferRef& reference) {
    const auto snapshot=command.constantSnapshots.find(reference.ID());
    if (snapshot!=command.constantSnapshots.end() && snapshot->second.locked) return nullptr;
    if (!UsesNativeGpuConstantBacking(reference) || !layout.fields || !layout.fieldCount ||
        !layout.webBytes || layout.webBytes%4) return nullptr;
    GpuApi::BufferDesc descriptor;
    WGPUBuffer source=nullptr;
    if (!AcquireGpuBuffer(reference,descriptor,source)) return nullptr;
    auto reject=[&]() -> WGPUBuffer { GpuApi::Release(reference); return nullptr; };
    if (descriptor.size<layout.nativeMinimumBytes) return reject();
    for (std::size_t i=0;i<layout.fieldCount;++i) {
        const auto& field=layout.fields[i];
        if (!field.bytes || field.bytes%4 || field.nativeOffset%4 || field.webOffset%4 ||
            field.nativeOffset>layout.nativeMinimumBytes || field.bytes>layout.nativeMinimumBytes-field.nativeOffset ||
            field.webOffset>layout.webBytes || field.bytes>layout.webBytes-field.webOffset) return reject();
    }
    auto destination=CreateGpuBuffer(layout.webBytes,WGPUBufferUsage_Uniform|WGPUBufferUsage_Storage|WGPUBufferUsage_CopyDst,nullptr,0,"cp-repacked-constant");
    if (!destination) return reject();
    std::vector<GpuBufferCopyRegion> regions;
    regions.reserve(layout.fieldCount);
    for (std::size_t i=0;i<layout.fieldCount;) {
        auto run=layout.fields[i++];
        // Merge only adjacent ranges in both layouts. Native/WebGPU padding
        // remains distinct; do not copy a struct wholesale across ABI gaps.
        while (i<layout.fieldCount &&
            layout.fields[i].nativeOffset==run.nativeOffset+run.bytes &&
            layout.fields[i].webOffset==run.webOffset+run.bytes) run.bytes+=layout.fields[i++].bytes;
        regions.push_back({run.nativeOffset,run.webOffset,run.bytes});
    }
    if (!FlushPendingBufferCopies(command) || !EncodeGpuBufferCopyRegions(command.encoder,source,destination,regions.data(),regions.size())) {
        DestroyGpuBuffer(destination);return reject();
    }
    command.retainedBuffers.push_back(reference);
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1)<8) std::fprintf(stderr,
        "CP2077 native GPU constant repack: shader=%s binding=%u ref=%u nativeBytes=%u webBytes=%u fields=%llu\n",
        layout.shader,layout.binding,reference.ID(),descriptor.size,layout.webBytes,
        static_cast<unsigned long long>(layout.fieldCount));
    return destination;
}
// Draw-path variant of RepackNativeGpuConstant (same validation, same field runs, same command position).
// The repacked snapshot is suballocated from this command's uniform pages instead of a fresh validated buffer per
// draw (a synchronous creation with three error scopes, plus a main-thread destroy at retirement), and the field
// copies are queued in encoder order instead of a flush plus a synchronous copy handoff. The slot starts zeroed
// (padding reads zero, as in a fresh buffer); FlushCommandUniforms writes it with queue.writeBuffer before this
// list is submitted, so the in-encoder copies still overwrite every field at this exact command position.
// offset is absolute in buffer and 256-aligned. The buffer belongs to the uniform ring/arena: never destroy it.
static bool RepackNativeGpuConstantToUniform(CommandEntry& command, const UniformLayout& layout,
                                             const GpuApi::BufferRef& reference, WGPUBuffer& buffer, std::uint64_t& offset) {
    buffer=nullptr; offset=0;
    const auto snapshot=command.constantSnapshots.find(reference.ID());
    if (snapshot!=command.constantSnapshots.end() && snapshot->second.locked) return false;
    if (!UsesNativeGpuConstantBacking(reference) || !layout.fields || !layout.fieldCount ||
        !layout.webBytes || layout.webBytes%4) return false;
    GpuApi::BufferDesc descriptor;
    WGPUBuffer source=nullptr;
    if (!AcquireGpuBuffer(reference,descriptor,source)) return false;
    auto reject=[&]() { GpuApi::Release(reference); buffer=nullptr; offset=0; return false; };
    if (descriptor.size<layout.nativeMinimumBytes) return reject();
    for (std::size_t i=0;i<layout.fieldCount;++i) {
        const auto& field=layout.fields[i];
        if (!field.bytes || field.bytes%4 || field.nativeOffset%4 || field.webOffset%4 ||
            field.nativeOffset>layout.nativeMinimumBytes || field.bytes>layout.nativeMinimumBytes-field.nativeOffset ||
            field.webOffset>layout.webBytes || field.bytes>layout.webBytes-field.webOffset) return reject();
    }
    static thread_local std::vector<unsigned char> zeros;
    zeros.assign(layout.webBytes,0);
    if (!AllocateCommandUniform(command,zeros.data(),zeros.size(),buffer,offset)) return reject();
    for (std::size_t i=0;i<layout.fieldCount;) {
        auto run=layout.fields[i++];
        // Merge only adjacent ranges in both layouts (identical to RepackNativeGpuConstant).
        while (i<layout.fieldCount &&
            layout.fields[i].nativeOffset==run.nativeOffset+run.bytes &&
            layout.fields[i].webOffset==run.webOffset+run.bytes) run.bytes+=layout.fields[i++].bytes;
        if (!QueuePendingBufferCopy(command,source,run.nativeOffset,buffer,offset+run.webOffset,run.bytes)) return reject();
    }
    command.retainedBuffers.push_back(reference);
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1)<8) std::fprintf(stderr,
        "CP2077 native GPU constant repack (uniform pages): shader=%s binding=%u ref=%u nativeBytes=%u webBytes=%u fields=%llu\n",
        layout.shader,layout.binding,reference.ID(),descriptor.size,layout.webBytes,
        static_cast<unsigned long long>(layout.fieldCount));
    return true;
}
bool SetBoundGpuConstantBuffers(std::uint32_t slot, const GpuApi::BufferRef* refs, const bool* selected) {
    if (!refs || !selected) return false;
    if (!selected[0] && !selected[1] && !selected[2]) return true;
    std::array<GpuApi::BufferRef, 3> previous{};
    const bool cpCbKeep = cp2077::web::CpCbKeepOn(); Uint32 cpCopiedId = 0; // [skperf] round 61 cbKeep
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (slot >= GpuApi::MAX_CONSTANT_BUFFERS ||
            ((selected[0] || selected[1]) && command.type != GpuApi::CommandListType::Default) ||
            (selected[2] && command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute)) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        for (unsigned stage=0;stage<3;++stage) {
            if(!selected[stage] || refs[stage].isNull()) continue;
            const auto local=command.constantSnapshots.find(refs[stage].ID());
            if(local!=command.constantSnapshots.end() && local->second.locked) {
                MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
            }
        }
        for (unsigned stage = 0; stage < 3; ++stage) {
            if (!selected[stage]) continue;
            auto& target = stage == 2 ? command.computeConstantBuffers[slot] : command.graphicsConstantBuffers[stage][slot];
            previous[stage] = target; target = refs[stage];
            if (!target.isNull()) {
                // [flicker3] A value this list wrote itself (Lock/Unlock) stays: native DX12 copies it into the buffer in this
                // list's order, so later draws of this list read it, whatever other threads unlocked meanwhile.
                const auto own=command.constantSnapshots.find(target.ID());
                // [skperf] round 61 cbKeep: VS and PS bound to the same CB in one call refresh the same snapshot once.
                if(cpCbKeep && cpCopiedId && cpCopiedId==target.ID()) {} else
                if(!(CbOrderOn() && own!=command.constantSnapshots.end() && own->second.own)) {
                if(cpCbKeep) cpCopiedId=target.ID();
                // [skperf] round 46 cbSnapReuse (default ON, ?cbSnapReuse=0 old): a snapshot this list already has is
                // refreshed in place. CopyCommittedGpuConstantBuffer copy-assigns into its output only on success (every
                // failure returns before touching it), and copy-assignment reuses the vector's capacity, so the bytes and
                // flags are exactly those of the old path without a new vector (malloc under bufferMutex) and the free.
                if(Perf11Pools::CbSnapReuseOn() && own!=command.constantSnapshots.end()) {
                    if(CopyCommittedGpuConstantBuffer(target,own->second.bytes)) own->second.locked=false;
                } else {
                std::vector<unsigned char> committed;
                if(CopyCommittedGpuConstantBuffer(target,committed)) {
                    auto& snapshot=command.constantSnapshots[target.ID()];
                    snapshot.bytes=std::move(committed);snapshot.locked=false;
                }
                }
                }
            }
            if (slot == 6) {
                if (stage == 2) command.computeNativeConstants.clear();
                else command.graphicsNativeConstants[stage].clear();
            }
        }
    }
    if (cpCbKeep) cp2077::web::ReleaseGpuBuffersBatch(previous.data(), previous.size()); // [skperf] round 61 cbKeep: one lock
    else for (auto reference : previous) if (!reference.isNull()) GpuApi::Release(reference);
    return true;
}
void* LockBoundGpuBufferUpload(const GpuApi::BufferRef& reference, WGPUBuffer destination,
    std::uint32_t bytes, std::uint32_t flags, std::uint32_t offset, std::uint32_t size) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return nullptr;
    auto& command = entry->second;
    if (command.failed) return nullptr;
    auto previous = command.bufferUploadLocks.find(reference.ID());
    // [gamerender] Original DX12 LockBuffer ignores the lock flags for ordinary (non-transient, non-readback)
    // buffers, and a second lock before unlock simply replaces the pending upload region (FINAL has no assert).
    // A size%4 tail is allowed when it ends at the logical end: the copy is rounded up into the allocation's
    // 4-byte tail padding (same rule as LoadBufferData). Interior unaligned writes still fail, with a report.
    // Discard locks (e.g. WindImpulseVolumeUpdate: Discard, offset 0, 90 bytes of a 64 KiB buffer) give up the
    // old contents by contract, so their copy is also rounded up to whole words inside the allocation.
    const std::uint64_t rounded = (std::uint64_t(size) + 3) & ~std::uint64_t(3);
    const std::uint64_t allocation = (std::uint64_t(bytes) + 3) & ~std::uint64_t(3);
    const bool tail = size % 4 && !(offset % 4) && std::uint64_t(offset) + size <= bytes &&
        (std::uint64_t(offset) + size == bytes || (flags & GpuApi::BLF_Discard)) && offset + rounded <= allocation;
    const char* reject = !destination ? "null destination" : !size ? "zero size" :
        (offset > bytes || size > bytes-offset) ? "range" : offset % 4 ? "unaligned offset" :
        (size % 4 && !tail) ? "unaligned interior size" : (flags & GpuApi::BLF_Read) ? "read flag" : nullptr;
    if (reject && destination && size && offset <= bytes && size <= bytes-offset) {
        // [gamerender] A word-unaligned region that must preserve neighbouring bytes cannot be copied by WebGPU.
        // Hand out CPU scratch and skip only this upload (its bytes stay stale) instead of failing the list.
        static std::atomic<unsigned> dropReports{0};
        if (dropReports.fetch_add(1) < 16)
            emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,
                "CP2077 buffer upload lock dropped (list kept): reason=%s ref=%u flags=%u offset=%u size=%u total=%u",
                reject, reference.ID(), flags, offset, size, bytes);
        auto& upload = command.bufferUploadLocks[reference.ID()];
        upload.bytes.assign(size, 0); upload.destination = destination;
        upload.offset = offset; upload.locked = true; upload.drop = true;
        command.retainedBuffers.push_back(reference);
        return upload.bytes.data();
    }
    if (reject) {
        static std::atomic<unsigned> rejectReports{0};
        if (rejectReports.fetch_add(1) < 8)
            emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,
                "CP2077 buffer upload lock rejected: reason=%s ref=%u flags=%u offset=%u size=%u total=%u",
                reject, reference.ID(), flags, offset, size, bytes);
        MarkBrowserCommandFailed(command,__func__,__LINE__); return nullptr;
    }
    if (previous != command.bufferUploadLocks.end() && previous->second.locked) {
        static std::atomic<unsigned> relockReports{0};
        if (relockReports.fetch_add(1) < 8)
            std::fprintf(stderr,"CP2077 buffer upload relock (previous region dropped, DX12 FINAL semantics): ref=%u flags=%u offset=%u size=%u total=%u\n",
                reference.ID(), flags, offset, size, bytes);
    }
    auto& upload = command.bufferUploadLocks[reference.ID()];
    upload.bytes.assign(tail ? ((std::size_t(size) + 3) & ~std::size_t(3)) : size, 0); upload.destination = destination;
    upload.offset = offset; upload.locked = true; upload.drop = false;
    command.retainedBuffers.push_back(reference);
    return upload.bytes.data();
}
bool UnlockBoundGpuBufferUpload(const GpuApi::BufferRef& reference) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    auto& command = entry->second;
    auto found = command.bufferUploadLocks.find(reference.ID());
    if (found == command.bufferUploadLocks.end() || !found->second.locked || command.failed) return false;
    auto& upload = found->second;
    if (upload.drop) { upload.locked = false; upload.drop = false; return true; } // [gamerender] see Lock
    // Same ordered copy into the destination; the bytes travel through a pooled staging arena.
    if (!QueuePendingStagedUpload(command, upload.bytes.data(), upload.bytes.size(),
            upload.destination, upload.offset)) {
        MarkBrowserCommandFailed(command,__func__,__LINE__); return false;
    }
    upload.locked = false;
    return true;
}
void* LockBoundGpuConstantBuffer(const GpuApi::BufferRef& reference, std::uint32_t bytes,
    std::uint32_t flags, std::uint32_t offset, std::uint32_t size) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return nullptr;
    auto& command = entry->second;
    if (command.failed) return nullptr;
    auto previous = command.constantSnapshots.find(reference.ID());
    if ((UsesNativeGpuConstantBacking(reference) &&
            (flags!=GpuApi::BLF_Discard || offset!=0 || size!=bytes)) ||
        !size || offset > bytes || size > bytes-offset ||
        (flags != GpuApi::BLF_Discard && flags != GpuApi::BLF_NoOverwrite) ||
        (flags == GpuApi::BLF_Discard && offset != 0) ||
        (previous != command.constantSnapshots.end() && previous->second.locked) ||
        (flags == GpuApi::BLF_NoOverwrite && previous == command.constantSnapshots.end())) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return nullptr;
    }
    auto& snapshot = command.constantSnapshots[reference.ID()];
    if (flags == GpuApi::BLF_Discard) snapshot.bytes.assign(bytes, 0);
    if (snapshot.bytes.size() != bytes) { MarkBrowserCommandFailed(command, __func__, __LINE__); return nullptr; }
    snapshot.locked = true;
    snapshot.own = true; // [flicker3]
    command.retainedBuffers.push_back(reference);
    return snapshot.bytes.data()+offset;
}
bool UnlockBoundGpuConstantBuffer(const GpuApi::BufferRef& reference) {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    auto snapshot = entry->second.constantSnapshots.find(reference.ID());
    if (snapshot == entry->second.constantSnapshots.end() || !snapshot->second.locked) return false;
    if (UsesNativeGpuConstantBacking(reference)) {
        GpuApi::BufferDesc descriptor;WGPUBuffer destination=nullptr;
        if (entry->second.failed || !AcquireGpuBuffer(reference,descriptor,destination)) return false;
        const auto& bytes=snapshot->second.bytes;
        if (bytes.size()%4 || !QueuePendingStagedUpload(entry->second,bytes.data(),bytes.size(),destination,0)) {
            GpuApi::Release(reference);MarkBrowserCommandFailed(entry->second,__func__,__LINE__);return false;
        }
        entry->second.retainedBuffers.push_back(reference);
    }
    if (entry->second.failed || !CommitGpuConstantBufferWrite(reference,snapshot->second.bytes.data(),snapshot->second.bytes.size())) {
        MarkBrowserCommandFailed(entry->second,__func__,__LINE__);return false;
    }
    snapshot->second.locked = false; return true;
}
bool SetBoundGpuComputeConstantBuffer(std::uint32_t slot, const GpuApi::BufferRef& reference) {
    GpuApi::BufferRef previous = GpuApi::BufferRef::Null();
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (slot >= command.computeConstantBuffers.size() ||
            (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute)) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        if(!reference.isNull()) {
            const auto local=command.constantSnapshots.find(reference.ID());
            if(local!=command.constantSnapshots.end() && local->second.locked) {
                MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
            }
        }
        previous = command.computeConstantBuffers[slot]; command.computeConstantBuffers[slot] = reference;
        if(!reference.isNull()) {
            std::vector<unsigned char> committed;
            if(CopyCommittedGpuConstantBuffer(reference,committed)) {
                auto& snapshot=command.constantSnapshots[reference.ID()];
                if(!(CbOrderOn() && snapshot.own)) snapshot.bytes=std::move(committed); // [flicker3] keep this list's own write
            }
        }
        if (slot == 6) command.computeNativeConstants.clear();
    }
    if (!previous.isNull()) GpuApi::Release(previous);
    return true;
}
bool SetBoundGpuComputeTextures(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* references, const WGPUTextureView* views, bool storage) {
    if (count && (!references || !views)) return false;
    if (storage) for (std::size_t index = 0; index < count; ++index) SharedConv::NoteWrite(references[index], false); // [skperf]
    std::vector<CommandEntry::TextureBinding> previous;
    std::vector<GpuApi::BufferRef> previousBuffers;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        const auto slots = storage ? command.computeStorageTextures.size() : command.computeTextures.size();
        if (start > slots || count > slots - start ||
            (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute)) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (references[index].isNull() != (views[index] == nullptr)) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
        }
        previous.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto& slot = storage ? command.computeStorageTextures[start+index] : command.computeTextures[start+index];
            previous.push_back(slot); slot = {references[index], views[index]};
            if (storage) {
                previousBuffers.push_back(command.computeStorageBuffers[start+index].reference);
                command.computeStorageBuffers[start+index] = {};
            } else {
                previousBuffers.push_back(command.computeBuffers[start+index].reference);
                command.computeBuffers[start+index] = {};
            }
        }
    }
    for (const auto& texture : previous) {
        if (texture.view) ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
    }
    for (auto buffer : previousBuffers) if (!buffer.isNull()) GpuApi::Release(buffer);
    return true;
}
void CountCmdlockEvent(unsigned index, unsigned long long amount) {
    // [skperf] round 46: diagnostic shared counters (one per BindTextures call / sampled view acquire on every recording
    // worker, same cache line) only while probing, like cmdlockCounters[4/7/8] since round 38.
    if (!cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) && !cpEngineTrace::ProbesEnabled()) return;
    if (index < 10) cmdlockCounters[index].fetch_add(amount, std::memory_order_relaxed);
}
// [cmdlock] True when every selected stage's slots [start, start+count) already hold exactly these references
// (with a live view, no buffer in the slot and the same native-null flag), i.e. a BindTextures call would be a
// no-op. The bound list is owner-exclusive, so the answer stays valid until this thread changes it.
bool BoundGpuSampledTexturesMatch(std::uint32_t start, std::size_t count, const GpuApi::TextureRef* resolved,
    const bool* selected, const GpuApi::TextureRef* logicalRefs) {
    if (!count || !resolved || !selected || start > GpuApi::MAX_SRV_BINDINGS || count > GpuApi::MAX_SRV_BINDINGS - start)
        return false;
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    const auto& command = entry->second;
    if (command.failed) return false;
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!selected[stage]) continue;
        if (stage < 2 ? command.type != GpuApi::CommandListType::Default
                      : (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute))
            return false;
        const auto& textures = stage == 2 ? command.computeTextures : command.graphicsTextures[stage];
        const auto& buffers = stage == 2 ? command.computeBuffers : command.graphicsBuffers[stage];
        for (std::size_t i = 0; i < count; ++i) {
            const auto& slot = textures[start+i];
            const bool nativeNull = logicalRefs && logicalRefs[i].isNull();
            if (resolved[i].isNull() || slot.reference.ID() != resolved[i].ID() || !slot.view || !slot.plain ||
                slot.nativeNull != nativeNull || !buffers[start+i].reference.isNull()) return false;
        }
    }
    return true;
}
// [skperf] round 43 bindKeep (default ON, ?bindKeep=0 old): BindTextures re-acquired and re-committed EVERY slot of
// its range when any one slot changed (per slot and stage: texture-mutex retain + view retain, then the old slot's
// view release + texture-mutex Release). Slots that already hold exactly the requested reference are now left as
// they are, using the same equality BoundGpuSampledTexturesMatch (bindEarly) applies to the whole range; the final
// slot state is the one the re-bind produced. The bound list is owner-exclusive, so the mask stays valid until the
// commit on this thread.
bool BoundGpuSampledTexturesKeep(std::uint32_t start, std::size_t count, const GpuApi::TextureRef* resolved,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, bool* keep) {
    if (!count || !resolved || !selected || !keep || start > GpuApi::MAX_SRV_BINDINGS || count > GpuApi::MAX_SRV_BINDINGS - start)
        return false;
    for (std::size_t k = 0; k < 3 * GpuApi::MAX_SRV_BINDINGS; ++k) keep[k] = false;
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    const auto& command = entry->second;
    if (command.failed) return false;
    bool any = false;
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!selected[stage]) continue;
        if (stage < 2 ? command.type != GpuApi::CommandListType::Default
                      : (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute))
            return false; // the normal path reports/fails it
        const auto& textures = stage == 2 ? command.computeTextures : command.graphicsTextures[stage];
        const auto& buffers = stage == 2 ? command.computeBuffers : command.graphicsBuffers[stage];
        for (std::size_t i = 0; i < count; ++i) {
            const auto& slot = textures[start+i];
            const bool nativeNull = logicalRefs && logicalRefs[i].isNull();
            const bool same = !resolved[i].isNull() && slot.reference.ID() == resolved[i].ID() && slot.view && slot.plain &&
                slot.nativeNull == nativeNull && buffers[start+i].reference.isNull();
            keep[stage * GpuApi::MAX_SRV_BINDINGS + i] = same; any |= same;
        }
    }
    if (!any) for (std::size_t k = 0; k < 3 * GpuApi::MAX_SRV_BINDINGS; ++k) keep[k] = false;
    return any;
}
static bool SetBoundGpuSampledTexturesImpl(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, const bool* keep);
bool SetBoundGpuSampledTexturesKeep(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, const bool* keep) {
    return SetBoundGpuSampledTexturesImpl(start, count, refs, views, selected, logicalRefs, keep);
}
bool SetBoundGpuSampledTextures(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs) {
    return SetBoundGpuSampledTexturesImpl(start, count, refs, views, selected, logicalRefs, nullptr);
}
static bool SetBoundGpuSampledTexturesImpl(std::uint32_t start, std::size_t count,
    const GpuApi::TextureRef* const* refs, const WGPUTextureView* const* views,
    const bool* selected, const GpuApi::TextureRef* logicalRefs, const bool* keep) {
    if (!selected || !refs || !views || start > GpuApi::MAX_SRV_BINDINGS ||
        count > GpuApi::MAX_SRV_BINDINGS - start) return false;
    // [cmdlock] fixed stack storage (count <= MAX_SRV_BINDINGS, 3 stages) instead of two heap vectors per call.
    std::array<CommandEntry::TextureBinding, GpuApi::MAX_SRV_BINDINGS * 3> previous;
    std::array<GpuApi::BufferRef, GpuApi::MAX_SRV_BINDINGS * 3> previousBuffers;
    std::size_t previousCount = 0;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        for (unsigned stage = 0; stage < 3; ++stage) {
            if (!selected[stage]) continue;
            if ((stage < 2 && command.type != GpuApi::CommandListType::Default) ||
                (stage == 2 && command.type != GpuApi::CommandListType::Default &&
                               command.type != GpuApi::CommandListType::Compute) ||
                (count && (!refs[stage] || !views[stage]))) {
                MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
            }
            for (std::size_t i = 0; i < count; ++i)
                if ((!keep || !keep[stage * GpuApi::MAX_SRV_BINDINGS + i]) && refs[stage][i].isNull() != (views[stage][i] == nullptr)) {
                    MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
                }
        }
        for (unsigned stage = 0; stage < 3; ++stage) {
            if (!selected[stage]) continue;
            auto& textures = stage == 2 ? command.computeTextures : command.graphicsTextures[stage];
            auto& buffers = stage == 2 ? command.computeBuffers : command.graphicsBuffers[stage];
            for (std::size_t i = 0; i < count; ++i) {
                if (keep && keep[stage * GpuApi::MAX_SRV_BINDINGS + i]) continue; // [skperf] round 43 bindKeep: slot already exact
                previous[previousCount] = textures[start+i];
                previousBuffers[previousCount++] = buffers[start+i].reference;
                textures[start+i] = {refs[stage][i], views[stage][i],logicalRefs && logicalRefs[i].isNull(),logicalRefs != nullptr};
                buffers[start+i] = {};
            }
        }
    }
    for (std::size_t k = 0; k < previousCount; ++k) {
        const auto& texture = previous[k];
        if (texture.view) ReleaseGpuTextureView(texture.view);
        if (!texture.reference.isNull()) GpuApi::Release(texture.reference);
    }
    for (std::size_t k = 0; k < previousCount; ++k) if (!previousBuffers[k].isNull()) GpuApi::Release(previousBuffers[k]);
    return true;
}
bool SetBoundGpuComputeBuffer(std::uint32_t slot, const GpuApi::BufferRef& reference,
    WGPUBuffer buffer, std::uint64_t bytes, std::uint32_t stride) {
    CommandEntry::BufferBinding previous;
    CommandEntry::TextureBinding previousTexture;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (slot >= command.computeBuffers.size() ||
            (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute) ||
            reference.isNull() != (buffer == nullptr) || (buffer && (!bytes || !stride || bytes % stride))) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        previous = command.computeBuffers[slot];
        previousTexture = command.computeTextures[slot];
        command.computeBuffers[slot] = {reference, buffer, bytes, stride};
        command.computeTextures[slot] = {};
    }
    if (!previous.reference.isNull()) GpuApi::Release(previous.reference);
    if (previousTexture.view) ReleaseGpuTextureView(previousTexture.view);
    if (!previousTexture.reference.isNull()) GpuApi::Release(previousTexture.reference);
    return true;
}
bool SetBoundGpuStructuredBuffers(std::uint32_t slot, const GpuApi::BufferRef* refs,
    WGPUBuffer buffer, std::uint64_t bytes, std::uint32_t stride, const bool* selected) {
    if (!refs || !selected || slot >= GpuApi::MAX_SRV_BINDINGS ||
        (buffer && (!bytes || !stride || bytes % stride))) return false;
    CommandEntry::BufferBinding previous[3];
    CommandEntry::TextureBinding previousTextures[3];
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        for (unsigned stage = 0; stage < 3; ++stage) {
            if (!selected[stage]) continue;
            if (refs[stage].isNull() != (buffer == nullptr) ||
                (stage < 2 && command.type != GpuApi::CommandListType::Default) ||
                (stage == 2 && command.type != GpuApi::CommandListType::Default &&
                               command.type != GpuApi::CommandListType::Compute)) {
                MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
            }
        }
        for (unsigned stage = 0; stage < 3; ++stage) {
            if (!selected[stage]) continue;
            auto& buffers = stage == 2 ? command.computeBuffers : command.graphicsBuffers[stage];
            auto& textures = stage == 2 ? command.computeTextures : command.graphicsTextures[stage];
            previous[stage] = buffers[slot]; previousTextures[stage] = textures[slot];
            buffers[slot] = {refs[stage], buffer, bytes, stride}; textures[slot] = {};
        }
    }
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!previous[stage].reference.isNull()) GpuApi::Release(previous[stage].reference);
        if (previousTextures[stage].view) ReleaseGpuTextureView(previousTextures[stage].view);
        if (!previousTextures[stage].reference.isNull()) GpuApi::Release(previousTextures[stage].reference);
    }
    return true;
}
static bool RecordGpuComputeDispatchLocked(CommandEntry& command, const ComputeBindGroup* groups, std::size_t count,
                               std::uint32_t x, std::uint32_t y, std::uint32_t z, bool indirect=false,
                               bool* skipped=nullptr, std::shared_ptr<QueuedComputeDispatch> queued=nullptr) {
    // skipped (optional): a binding retain failure releases this dispatch's references and sets *skipped
    // instead of failing the list; the caller then skips only this dispatch.
    // queued (optional, perf7): instead of flushing and encoding now (two synchronous main-thread handoffs), the
    // dispatch is queued in encoder order and encoded by the next FlushPendingBufferCopies batch. With a null
    // queued->group (count 0 here) the group is created on the main thread from queued->entries at that time.
    if (command.failed) return false;
    static_assert(sizeof(GpuApi::SIndirectDispatchArgs) == 12 && offsetof(GpuApi::SIndirectDispatchArgs, ThreadGroupCountZ) == 8);
    if (indirect && (!command.indirectArguments.buffer || command.indirectOffset % 4 ||
        command.indirectOffset > command.indirectArguments.bytes ||
        sizeof(GpuApi::SIndirectDispatchArgs) > command.indirectArguments.bytes-command.indirectOffset)) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    // Each handle transfers ownership once, even when referenced at distinct slots.
    for (std::size_t index = 0; groups && index < count; ++index) {
        for (std::size_t previous = 0; previous < index; ++previous)
            if (groups[previous].binding == groups[index].binding) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
        // A queued dispatch's group is its own cache-lookup reference (AddRef per lookup), so an identical cached
        // group already retained by an earlier dispatch of this list is a second, separately owned reference.
        if (!queued)
            for (auto previous : command.retainedBindGroups)
                if (previous == groups[index].binding) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
    }
    WGPUComputePipeline retainedPipeline = nullptr;
    if (!AcquireGpuComputeShader(command.computeShader, retainedPipeline)) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
    // A native bind group retains the WebGPU handle, but cannot prevent the
    // original texture's last Release from explicitly destroying it. Keep the
    // original bindings alive through submission even after slots are replaced.
    std::vector<GpuApi::TextureRef> texturesForDispatch;
    auto retainBindings = [&](const auto& bindings) {
        for (const auto& texture : bindings) {
            if (texture.reference.isNull()) continue;
            if (!RetainGpuTexture(texture.reference)) return false;
            texturesForDispatch.push_back(texture.reference);
        }
        return true;
    };
    for (const auto& texture : command.computeStorageTextures) // [skperf] listGen: this dispatch may write its UAVs
        if (!texture.reference.isNull()) ++command.localWriteGen[texture.reference.ID()];
    if (!retainBindings(command.computeTextures) || !retainBindings(command.computeStorageTextures)) {
        for (auto texture : texturesForDispatch) GpuApi::Release(texture);
        GpuApi::Release(command.computeShader);
        if (skipped) { *skipped = true; return false; }
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    std::vector<GpuApi::BufferRef> buffersForDispatch;
    auto retainBufferBindings = [&](const auto& bindings) {
        for (const auto& buffer : bindings) {
            if (buffer.reference.isNull()) continue;
            GpuApi::BufferDesc descriptor; WGPUBuffer native = nullptr;
            if (!AcquireGpuBuffer(buffer.reference, descriptor, native)) return false;
            buffersForDispatch.push_back(buffer.reference);
        }
        return true;
    };
    if (!retainBufferBindings(command.computeBuffers) || !retainBufferBindings(command.computeStorageBuffers)) {
        for (auto reference : buffersForDispatch) GpuApi::Release(reference);
        for (auto texture : texturesForDispatch) GpuApi::Release(texture);
        GpuApi::Release(command.computeShader);
        if (skipped) { *skipped = true; return false; }
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    if (indirect) {
        GpuApi::BufferDesc descriptor; WGPUBuffer native;
        if (!AcquireGpuBuffer(command.indirectArguments.reference, descriptor, native)) {
            for (auto reference : buffersForDispatch) GpuApi::Release(reference);
            for (auto texture : texturesForDispatch) GpuApi::Release(texture);
            GpuApi::Release(command.computeShader); MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        buffersForDispatch.push_back(command.indirectArguments.reference);
    }
    bool encoded = false;
    if (queued) {
        queued->pipeline = retainedPipeline; queued->owner = command.encoder;
        queued->group = count ? groups[0].binding : nullptr;
        queued->x = x; queued->y = y; queued->z = z;
        queued->indirect = indirect ? command.indirectArguments.buffer : nullptr;
        queued->indirectOffset = indirect ? command.indirectOffset : 0;
        encoded = (count == 1 || (count == 0 && queued->entriesHeld)) &&
            QueuePendingCustom(command, [queued](WGPUCommandEncoder encoder) -> bool {
                // Main thread, inside the batched EncodeGpuOrderedCommands handoff.
                WGPUBindGroup group = queued->group;
                if (!group) {
                    queued->created = CreateGpuComputeBindGroupOnMain(queued->pipeline, queued->entries.data(),
                        queued->entries.size(), queued->owner);
                    queued->ReleaseEntries();
                    group = queued->created;
                    if (!group) {
                        // Same outcome as the synchronous path's "bind group creation failed": skip this dispatch.
                        static unsigned skips = 0;
                        if (++skips <= 8 || !(skips & (skips - 1)))
                            std::fprintf(stderr, "CP2077 WebGPU: skipped queued compute dispatch (bind group creation failed) count=%u\n", skips);
                        return true;
                    }
                }
                return EncodeGpuComputeDispatchOnMain(encoder, queued->pipeline, group, queued->x, queued->y, queued->z,
                    queued->indirect, queued->indirectOffset);
            });
        if (encoded) command.queuedDispatches.push_back(queued);
    } else encoded = FlushPendingBufferCopies(command) && (indirect
        ? EncodeGpuComputeDispatchIndirect(command.encoder, retainedPipeline, groups, count,
            command.indirectArguments.buffer, command.indirectOffset)
        : EncodeGpuComputeDispatch(command.encoder, retainedPipeline, groups, count, x, y, z));
    if (!encoded) {
        for (auto reference : buffersForDispatch) GpuApi::Release(reference);
        for (auto texture : texturesForDispatch) GpuApi::Release(texture);
        GpuApi::Release(command.computeShader);
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    command.retainedShaders.push_back(command.computeShader);
    command.retainedTextures.insert(command.retainedTextures.end(), texturesForDispatch.begin(), texturesForDispatch.end());
    command.retainedBuffers.insert(command.retainedBuffers.end(), buffersForDispatch.begin(), buffersForDispatch.end());
    for (std::size_t index = 0; index < count; ++index) command.retainedBindGroups.push_back(groups[index].binding);
    return true;
}
bool RecordGpuComputeDispatch(const ComputeBindGroup* groups, std::size_t count,
                               std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    // Owner-exclusive list: the flush/encode handoffs run without commandMutex (see DispatchBoundGpuCompute).
    OwnedCommandLease commandLease(commandOperations,commandMutex);
    if (commandLease) lock.UnlockForOwnedOperation();
    return RecordGpuComputeDispatchLocked(entry->second, groups, count, x, y, z);
}
namespace {
// Texel format of a WGSL "var name: texture_storage_*<format, access>;" declaration at @binding(binding)
// (Undefined when absent or unknown). Cached per thread by (static wgsl text, binding).
WGPUTextureFormat FindWgslStorageTextureFormat(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding) {
    if (!wgsl || !wgslBytes) return WGPUTextureFormat_Undefined;
    static thread_local std::map<std::pair<const char*, std::uint32_t>, WGPUTextureFormat> cache;
    auto cached = cache.find({wgsl, binding});
    if (cached != cache.end()) return cached->second;
    WGPUTextureFormat result = WGPUTextureFormat_Undefined;
    const std::string_view text(wgsl, wgslBytes);
    const std::string needle = "@binding(" + std::to_string(binding) + ")";
    std::size_t p = text.find(needle);
    if (p != std::string_view::npos) {
        const std::size_t semicolon = text.find(';', p);
        const std::size_t type = text.find("texture_storage_", p);
        if (semicolon != std::string_view::npos && type != std::string_view::npos && type < semicolon) {
            const std::size_t open = text.find('<', type);
            const std::size_t comma = open == std::string_view::npos ? open : text.find(',', open);
            if (comma != std::string_view::npos && comma < semicolon) {
                std::string_view name = text.substr(open + 1, comma - open - 1);
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
                static const struct { const char* name; WGPUTextureFormat format; } formats[] = {
                    {"rgba8unorm", WGPUTextureFormat_RGBA8Unorm}, {"rgba8snorm", WGPUTextureFormat_RGBA8Snorm},
                    {"rgba8uint", WGPUTextureFormat_RGBA8Uint}, {"rgba8sint", WGPUTextureFormat_RGBA8Sint},
                    {"rgba16uint", WGPUTextureFormat_RGBA16Uint}, {"rgba16sint", WGPUTextureFormat_RGBA16Sint},
                    {"rgba16float", WGPUTextureFormat_RGBA16Float}, {"rgba16unorm", WGPUTextureFormat_RGBA16Unorm},
                    {"rgba16snorm", WGPUTextureFormat_RGBA16Snorm},
                    {"r32uint", WGPUTextureFormat_R32Uint}, {"r32sint", WGPUTextureFormat_R32Sint},
                    {"r32float", WGPUTextureFormat_R32Float}, {"rg32uint", WGPUTextureFormat_RG32Uint},
                    {"rg32sint", WGPUTextureFormat_RG32Sint}, {"rg32float", WGPUTextureFormat_RG32Float},
                    {"rgba32uint", WGPUTextureFormat_RGBA32Uint}, {"rgba32sint", WGPUTextureFormat_RGBA32Sint},
                    {"rgba32float", WGPUTextureFormat_RGBA32Float}, {"bgra8unorm", WGPUTextureFormat_BGRA8Unorm},
                    {"r8unorm", WGPUTextureFormat_R8Unorm}, {"r8snorm", WGPUTextureFormat_R8Snorm},
                    {"r8uint", WGPUTextureFormat_R8Uint}, {"r8sint", WGPUTextureFormat_R8Sint},
                    {"rg8unorm", WGPUTextureFormat_RG8Unorm}, {"rg8snorm", WGPUTextureFormat_RG8Snorm},
                    {"rg8uint", WGPUTextureFormat_RG8Uint}, {"rg8sint", WGPUTextureFormat_RG8Sint},
                    {"r16unorm", WGPUTextureFormat_R16Unorm}, {"r16snorm", WGPUTextureFormat_R16Snorm},
                    {"r16uint", WGPUTextureFormat_R16Uint}, {"r16sint", WGPUTextureFormat_R16Sint},
                    {"r16float", WGPUTextureFormat_R16Float}, {"rg16unorm", WGPUTextureFormat_RG16Unorm},
                    {"rg16snorm", WGPUTextureFormat_RG16Snorm}, {"rg16uint", WGPUTextureFormat_RG16Uint},
                    {"rg16sint", WGPUTextureFormat_RG16Sint}, {"rg16float", WGPUTextureFormat_RG16Float},
                    {"rgb10a2uint", WGPUTextureFormat_RGB10A2Uint}, {"rgb10a2unorm", WGPUTextureFormat_RGB10A2Unorm},
                    {"rg11b10ufloat", WGPUTextureFormat_RG11B10Ufloat},
                };
                for (const auto& entry : formats)
                    if (name == entry.name) { result = entry.format; break; }
            }
        }
    }
    // [notier1] the page rewrites tier1 storage formats to core ones before compiling (engine-select.js): report the
    // format the pipeline really declares, which is the format those textures are created with.
    if (cp2077::web::GpuLacksTier1() && result != WGPUTextureFormat_Undefined)
        result = cp2077::web::Tier1CoreFormat(result, WGPUTextureUsage_StorageBinding);
    if (cache.size() < 4096) cache.emplace(std::make_pair(wgsl, binding), result);
    return result;
}
// View dimension of a write-only "texture_storage_{2d,2d_array,3d}<format, write>" at @binding(binding);
// Undefined for any other type/access (read, read_write, 1d) or when absent. Cached per thread.
WGPUTextureViewDimension FindWgslWriteOnlyStorageTextureDimension(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding) {
    if (!wgsl || !wgslBytes) return WGPUTextureViewDimension_Undefined;
    static thread_local std::map<std::pair<const char*, std::uint32_t>, WGPUTextureViewDimension> cache;
    auto cached = cache.find({wgsl, binding});
    if (cached != cache.end()) return cached->second;
    WGPUTextureViewDimension result = WGPUTextureViewDimension_Undefined;
    const std::string_view text(wgsl, wgslBytes);
    const std::string needle = "@binding(" + std::to_string(binding) + ")";
    const std::size_t p = text.find(needle);
    const std::size_t semicolon = p == std::string_view::npos ? p : text.find(';', p);
    const std::size_t type = p == std::string_view::npos ? p : text.find("texture_storage_", p);
    if (semicolon != std::string_view::npos && type != std::string_view::npos && type < semicolon) {
        const std::size_t open = text.find('<', type);
        const std::size_t comma = open == std::string_view::npos ? open : text.find(',', open);
        const std::size_t close = comma == std::string_view::npos ? comma : text.find('>', comma);
        if (open < semicolon && comma < semicolon && close < semicolon) {
            const std::string_view shape = text.substr(type + 16, open - type - 16);
            std::string_view access = text.substr(comma + 1, close - comma - 1);
            while (!access.empty() && (access.front() == ' ' || access.front() == '\t')) access.remove_prefix(1);
            while (!access.empty() && (access.back() == ' ' || access.back() == '\t')) access.remove_suffix(1);
            if (access == "write") {
                if (shape == "2d") result = WGPUTextureViewDimension_2D;
                else if (shape == "2d_array") result = WGPUTextureViewDimension_2DArray;
                else if (shape == "3d") result = WGPUTextureViewDimension_3D;
            }
        }
    }
    if (cache.size() < 4096) cache.emplace(std::make_pair(wgsl, binding), result);
    return result;
}
// Bounded fallback report: first 16, then powers of two.
bool ShouldReportComputeFallback(std::atomic<unsigned>& counter, unsigned& n) {
    n = counter.fetch_add(1) + 1;
    return n <= 16 || (n & (n - 1)) == 0;
}
// BEGIN compute auto-layout scan
// Compute pipelines use layout:auto. Dawn's inferred group-0 entries (checked with getBindGroupLayout against
// all 110 packaged compute modules): a texture_*<f32> is sampleType Float iff it is the texture argument of a
// textureSample*/textureGather* call, else UnfilterableFloat; a `sampler` is Filtering iff such a call pairs it
// with a texture_*<f32>, else NonFiltering (e.g. only gathers texture_2d<u32>). Per (static wgsl, binding).
struct ComputeAutoLayoutUse { bool filteredTexture = false; bool filteringSampler = false; };
ComputeAutoLayoutUse FindComputeAutoLayoutUse(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding) {
    if (!wgsl || !wgslBytes) return {};
    static thread_local std::map<const char*, std::map<std::uint32_t, ComputeAutoLayoutUse>> cache;
    auto shader = cache.find(wgsl);
    if (shader == cache.end()) {
        if (cache.size() >= 1024) cache.clear();
        shader = cache.emplace(wgsl, std::map<std::uint32_t, ComputeAutoLayoutUse>{}).first;
        const std::string_view text(wgsl, wgslBytes);
        auto ident = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
        auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        auto trim = [&](std::string_view s) {
            while (!s.empty() && space(s.front())) s.remove_prefix(1);
            while (!s.empty() && space(s.back())) s.remove_suffix(1);
            return s;
        };
        struct Decl { std::uint32_t binding; std::string type; };
        std::map<std::string, Decl, std::less<>> decls;
        for (std::size_t at = text.find("@binding("); at != std::string_view::npos; at = text.find("@binding(", at + 9)) {
            std::size_t p = at + 9; std::uint32_t number = 0; bool digits = false;
            while (p < text.size() && text[p] >= '0' && text[p] <= '9') { number = number * 10 + unsigned(text[p] - '0'); ++p; digits = true; }
            const std::size_t end = text.find(';', p);
            if (!digits || end == std::string_view::npos) continue;
            std::size_t var = text.find("var", p);
            if (var == std::string_view::npos || var > end) continue;
            std::size_t q = var + 3;
            if (q < end && text[q] == '<') { q = text.find('>', q); if (q == std::string_view::npos || q > end) continue; ++q; }
            const std::size_t colon = text.find(':', q);
            if (colon == std::string_view::npos || colon > end) continue;
            std::string type;
            for (char c : text.substr(colon + 1, end - colon - 1)) if (!space(c)) type += c;
            decls[std::string(trim(text.substr(q, colon - q)))] = Decl{number, type};
        }
        auto& uses = shader->second;
        for (std::size_t p = 0; p < text.size();) {
            if (!ident(text[p])) { ++p; continue; }
            std::size_t e = p; while (e < text.size() && ident(text[e])) ++e;
            const std::string_view name = text.substr(p, e - p);
            p = e;
            if (name.compare(0, 13, "textureSample") != 0 && name.compare(0, 13, "textureGather") != 0) continue;
            std::size_t at = e; while (at < text.size() && space(text[at])) ++at;
            if (at >= text.size() || text[at] != '(') continue;
            // First three top-level arguments.
            std::string_view args[3]; unsigned count = 0; int depth = 0; std::size_t begin = ++at;
            for (; at < text.size() && count < 3; ++at) {
                const char c = text[at];
                if (c == '(' || c == '[') ++depth;
                else if ((c == ')' || c == ']') && depth) --depth;
                else if ((c == ',' && !depth) || c == ')') { args[count++] = trim(text.substr(begin, at - begin)); begin = at + 1; if (c == ')') break; }
            }
            auto first = decls.find(args[0]);
            // textureGather(component, t, s, ...) has a non-texture first argument.
            const bool shifted = first == decls.end() || first->second.type.compare(0, 8, "texture_") != 0;
            const auto texture = decls.find(args[shifted ? 1 : 0]);
            const auto sampler = decls.find(args[shifted ? 2 : 1]);
            if (texture == decls.end() || sampler == decls.end()) continue;
            const std::string& t = texture->second.type;
            const bool floatTexture = t.compare(0, 8, "texture_") == 0 && t.compare(0, 14, "texture_depth_") != 0 &&
                t.compare(0, 16, "texture_storage_") != 0 && t.size() >= 5 && t.compare(t.size() - 5, 5, "<f32>") == 0;
            if (!floatTexture || sampler->second.type != "sampler") continue;
            uses[texture->second.binding].filteredTexture = true;
            uses[sampler->second.binding].filteringSampler = true;
        }
    }
    const auto found = shader->second.find(binding);
    return found == shader->second.end() ? ComputeAutoLayoutUse{} : found->second;
}
// END compute auto-layout scan
enum : unsigned { kSampleFloat = 1, kSampleUnfilterable = 2, kSampleDepth = 4, kSampleUint = 8, kSampleSint = 16 };
// WebGPU sample types a view of this (depth-only / stencil-only resolved) format can bind as.
unsigned GpuViewSampleTypes(WGPUTextureFormat format) {
    switch (format) {
    case WGPUTextureFormat_Undefined: return 0;
    case WGPUTextureFormat_Depth16Unorm: case WGPUTextureFormat_Depth24Plus: case WGPUTextureFormat_Depth24PlusStencil8:
    case WGPUTextureFormat_Depth32Float: case WGPUTextureFormat_Depth32FloatStencil8:
        return kSampleDepth | kSampleUnfilterable;
    case WGPUTextureFormat_Stencil8:
    case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_R16Uint: case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_RG8Uint: case WGPUTextureFormat_RG16Uint: case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RGBA8Uint: case WGPUTextureFormat_RGBA16Uint: case WGPUTextureFormat_RGBA32Uint:
    case WGPUTextureFormat_RGB10A2Uint:
        return kSampleUint;
    case WGPUTextureFormat_R8Sint: case WGPUTextureFormat_R16Sint: case WGPUTextureFormat_R32Sint:
    case WGPUTextureFormat_RG8Sint: case WGPUTextureFormat_RG16Sint: case WGPUTextureFormat_RG32Sint:
    case WGPUTextureFormat_RGBA8Sint: case WGPUTextureFormat_RGBA16Sint: case WGPUTextureFormat_RGBA32Sint:
        return kSampleSint;
    case WGPUTextureFormat_R32Float: case WGPUTextureFormat_RG32Float: case WGPUTextureFormat_RGBA32Float:
        return kSampleUnfilterable | (SupportsGpuFeature(WGPUFeatureName_Float32Filterable) ? kSampleFloat : 0u);
    default: return kSampleFloat | kSampleUnfilterable;
    }
}
// Why a real sampled view cannot satisfy the compute auto-layout entry for this binding (nullptr = it fits or the
// declaration/view is not known; such views are bound unchanged as before). Fills the declaration and view info.
const char* ComputeSampledViewMismatch(const char* wgsl, std::size_t wgslBytes, std::uint32_t binding, WGPUTextureView view,
    GpuWgslBinding& declared, WGPUTextureFormat& format, WGPUTextureViewDimension& dimension, bool& filtered) {
    declared = FindGpuWgslBinding(wgsl, wgslBytes, binding);
    filtered = false;
    unsigned need = 0;
    switch (declared.kind) {
    case GpuWgslBinding_Float:
        filtered = FindComputeAutoLayoutUse(wgsl, wgslBytes, binding).filteredTexture;
        need = filtered ? kSampleFloat : kSampleUnfilterable; break;
    case GpuWgslBinding_Uint: need = kSampleUint; break;
    case GpuWgslBinding_Sint: need = kSampleSint; break;
    case GpuWgslBinding_Depth: need = kSampleDepth; break;
    default: return nullptr;
    }
    if (!GetGpuTextureViewInfo(view, format, dimension)) {
        static std::atomic<unsigned> unknownReports{0};
        unsigned n = 0;
        if (ShouldReportComputeFallback(unknownReports, n))
            std::fprintf(stderr, "CP2077 WebGPU: compute SRV view info unknown (bound unchecked): binding=%u count=%u\n", binding, n);
        return nullptr;
    }
    if (dimension != declared.dimension) return "view dimension";
    if (!(GpuViewSampleTypes(format) & need))
        return need == kSampleFloat ? "view not filterable float" : "view sample type";
    return nullptr;
}
}
// [whitewall] Exposure readout (?expprobe=1, off by default). The engine's auto exposure is a compute chain:
// histogramDownscale (reads the exposure SSBO as SRV t3: preExposure = word 3) -> histogramGather/SumUp ->
// histogramAverage (UAV u0 = the exposure SSBO, writes the average) -> histogramFrameExposure (UAV u0, writes
// word0 exposure, word1 adapted luminance, word3 1/exposure, word5 ...). Once per second the 8 words of the SSBO are
// copied right after histogramFrameExposure (same encoder, in order) and logged when the copy maps:
//   'CP2077 exposure probe: w0..w7 (floats) buffer=<WGPUBuffer> ref=<id>'
// plus which buffer each stage binds ('CP2077 exposure binding probe: shader= slot= buffer= ref='), so a renamed /
// stale copy between writer and readers is visible. Read-only diagnostics; no draw or dispatch changes.
static int CpExposureProbeMode() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('expprobe');
            return (q && q !== '0' && q !== 'false') ? 1 : 0;
        });
        std::fprintf(stderr, "CP2077 whitewall switches: expprobe=%d\n", v);
        return v;
    }();
    return mode;
}
// [whitewall] r4: readbacks are scheduled on the main thread inside the encoder batch (QueuePendingCustom), mapped
// 1.5 s later with an AllowSpontaneous callback (no wait, no frame stall). r3 never logged values because the
// exposure SSBO is 28 bytes and the copy was gated on >= 32; copies now use the bound size rounded down to 4.
struct CpProbeReadback { WGPUBuffer readback; std::uint32_t bytes; int kind; std::uint32_t width, height, rowBytes; unsigned ref; };
static float CpHalfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    std::uint32_t bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else { const float v = std::ldexp(float(mantissa), -24); return (h & 0x8000u) ? -v : v; }
    } else if (exponent == 31) bits = sign | 0x7f800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    float out; std::memcpy(&out, &bits, 4); return out;
}
// Bit-level classification: the engine is built with fast-math, where std::isnan/isinf may fold to false.
static bool CpFloatIsNan(float v) { std::uint32_t b; std::memcpy(&b, &v, 4); return (b & 0x7f800000u) == 0x7f800000u && (b & 0x7fffffu); }
static bool CpFloatIsInf(float v) { std::uint32_t b; std::memcpy(&b, &v, 4); return (b & 0x7fffffffu) == 0x7f800000u; }
static void CpProbeMapLater(CpProbeReadback* pending) {
    emscripten_async_call([](void* user) {
        auto* p = static_cast<CpProbeReadback*>(user);
        WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        info.mode = WGPUCallbackMode_AllowSpontaneous;
        info.userdata1 = p;
        info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* user1, void*) {
            auto* q = static_cast<CpProbeReadback*>(user1);
            const void* range = status == WGPUMapAsyncStatus_Success ? wgpuBufferGetConstMappedRange(q->readback, 0, q->bytes) : nullptr;
            if (!range) std::fprintf(stderr, "CP2077 exposure probe: map failed kind=%d status=%d\n", q->kind, int(status));
            else if (q->kind != 2) {
                float w[8] = {}; std::memcpy(w, range, std::min<std::uint32_t>(q->bytes, 32));
                std::fprintf(stderr, "CP2077 exposure probe: after=%s w0=%g w1=%g w2=%g w3=%g w4=%g w5=%g w6=%g ref=%u\n",
                    q->kind == 0 ? "histogramFrameExposure" : "histogramAverage", w[0], w[1], w[2], w[3], w[4], w[5], w[6], q->ref);
            } else {
                // r16float luminance written by histogramDownscale (pre-exposure applied).
                const auto* bytes = static_cast<const unsigned char*>(range);
                double sum = 0; float lo = 0, hi = 0; unsigned nan = 0, inf = 0, finite = 0, zero = 0;
                for (std::uint32_t y = 0; y < q->height; ++y)
                    for (std::uint32_t x = 0; x < q->width; ++x) {
                        std::uint16_t h; std::memcpy(&h, bytes + y * q->rowBytes + x * 2, 2);
                        const float v = CpHalfToFloat(h);
                        if (CpFloatIsNan(v)) ++nan; else if (CpFloatIsInf(v)) ++inf;
                        else {
                            if (!finite) lo = hi = v;
                            ++finite; sum += v; lo = std::min(lo, v); hi = std::max(hi, v); if (v == 0.f) ++zero;
                        }
                    }
                std::fprintf(stderr, "CP2077 luminance probe: %ux%u min=%g max=%g mean=%g zero=%u nan=%u inf=%u ref=%u\n",
                    q->width, q->height, lo, hi, finite ? sum / finite : 0.0, zero, nan, inf, q->ref);
            }
            if (range) wgpuBufferUnmap(q->readback);
            wgpuBufferRelease(q->readback); delete q;
        };
        wgpuBufferMapAsync(p->readback, WGPUMapMode_Read, 0, p->bytes, info);
    }, pending, 1500);
}
static WGPUBuffer CpProbeReadbackBuffer(std::uint32_t bytes) {
    WGPUDevice device = MainThreadGpuDevice();
    if (!device) return nullptr;
    WGPUBufferDescriptor descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    descriptor.size = bytes; descriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    descriptor.label = {"cp-exposure-probe", WGPU_STRLEN};
    return wgpuDeviceCreateBuffer(device, &descriptor);
}
// [whitewall] r5: constant-buffer bytes as bound for the histogram/exposure dispatches (once per second per shader),
// logged as floats in the original cbuffer order. histogramFrameExposure = {forcedExposure, emissiveLoLumaTreshold,
// emissiveLoLumaEV, emissiveHiLumaTreshold, emissiveHiLumaEV, exposureScale, isTimeOfDayCut(int)};
// histogramAverage = {fixedAvgLuminance, adaptationSpeedUp, adaptationSpeedDown, percentileLow, percentileHigh,
// EV100Min, EV100Max, exposureScale, exposureCompensationLinear, uiMin, uiMax, uiFactor, uiSpeedUp, uiSpeedDown}.
static void CpExposureProbeConstants(const char* name, std::uint32_t binding, const std::vector<unsigned char>& bytes) {
    if (!name) return;
    int which = -1;
    if (!std::strcmp(name, "histogramFrameExposure")) which = 0;
    else if (!std::strcmp(name, "histogramAverage")) which = 1;
    else if (!std::strcmp(name, "histogramDownscale")) which = 2;
    if (which < 0) return;
    // [whitewall] r6: print only when the values change (FNV-1a over the words). histogramAverage words 1,2 and 12,13
    // are per-frame adaptation speeds (functions of the frame time) and are left out of the change test.
    std::uint64_t hash = 1469598103934665603ull ^ bytes.size();
    for (std::size_t i = 0; i + 4 <= bytes.size() && i < 64; i += 4) {
        const std::size_t word = i / 4;
        if (which == 1 && (word == 1 || word == 2 || word == 12 || word == 13)) continue;
        for (std::size_t k = 0; k < 4; ++k) { hash ^= bytes[i + k]; hash *= 1099511628211ull; }
    }
    static std::atomic<std::uint64_t> lastHash[3]{{0}, {0}, {0}};
    if (lastHash[which].exchange(hash) == hash) return;
    char text[640]; int used = std::snprintf(text, sizeof(text), "CP2077 exposure cb probe: shader=%s binding=%u bytes=%zu%s f=",
        name, binding, bytes.size(), bytes.empty() ? " (native-backed repack, bytes not visible here)" : "");
    for (std::size_t i = 0; i + 4 <= bytes.size() && i < 64 && used < 600; i += 4) {
        float f; std::int32_t n; std::memcpy(&f, bytes.data() + i, 4); std::memcpy(&n, bytes.data() + i, 4);
        used += std::snprintf(text + used, sizeof(text) - used, "%s%g", i ? "," : "", f);
        if (which == 0 && i == 24) used += std::snprintf(text + used, sizeof(text) - used, "(int %d)", int(n));
    }
    std::fprintf(stderr, "%s\n", text);
}
static void CpExposureProbeDispatch(CommandEntry& command, const char* name) {
    const bool frameExposure = std::strcmp(name, "histogramFrameExposure") == 0;
    const bool average = std::strcmp(name, "histogramAverage") == 0;
    const bool downscale = std::strcmp(name, "histogramDownscale") == 0;
    if (!frameExposure && !average && !downscale) return;
    const auto& binding = downscale ? command.computeBuffers[3] : command.computeStorageBuffers[0];
    static std::atomic<double> lastBindingLog[3]{{0.0}, {0.0}, {0.0}};
    static std::atomic<double> lastCopy[3]{{0.0}, {0.0}, {0.0}};
    const int which = frameExposure ? 0 : average ? 1 : 2;
    const double now = emscripten_get_now();
    if (now - lastBindingLog[which].load() > 2000.0) {
        lastBindingLog[which].store(now);
        std::fprintf(stderr, "CP2077 exposure binding probe: shader=%s slot=%s buffer=%p ref=%u bytes=%llu\n", name,
            downscale ? "t3" : "u0", static_cast<void*>(binding.buffer), unsigned(binding.reference.ID()),
            static_cast<unsigned long long>(binding.bytes));
    }
    if (now - lastCopy[which].load() < 1000.0) return;
    lastCopy[which].store(now);
    if (!downscale) {
        // The exposure SSBO (w0..w6) right after its writer: histogramAverage (kind 1) or histogramFrameExposure (kind 0).
        const std::uint32_t bytes = std::uint32_t(std::min<std::uint64_t>(binding.bytes, 32)) & ~3u;
        if (!binding.buffer || bytes < 4) return;
        // [whitewall] r8: no wgpu* call on this render worker (WebGPU JS objects live on the main thread). The bound
        // buffer is retained by this dispatch until its list is submitted, so the raw handle stays valid for the copy,
        // which is encoded on the main thread in list order.
        WGPUBuffer source = binding.buffer;
        const unsigned ref = unsigned(binding.reference.ID()); const int kind = frameExposure ? 0 : 1;
        QueuePendingCustom(command, [source, bytes, ref, kind](WGPUCommandEncoder encoder) -> bool {
            if (WGPUBuffer readback = CpProbeReadbackBuffer(32)) {
                wgpuCommandEncoderCopyBufferToBuffer(encoder, source, 0, readback, 0, bytes);
                CpProbeMapLater(new CpProbeReadback{readback, 32, kind, 0, 0, 0, ref});
            }
            return true;
        });
        return;
    }
    // histogramDownscale output: storage texture u0 (r16float luminance), copied after the dispatch.
    // r5: r4 resolved the texture from the bound view's parent, which is not tracked for these views (0 lines);
    // resolve it from the bound reference instead, and say why when it cannot.
    const auto& storage = command.computeStorageTextures[0];
    WGPUTexture texture = nullptr;
    if (storage.reference.isNull() || !AcquireGpuTextureForCopy(storage.reference, texture) || !texture) {
        static std::atomic<unsigned> misses{0};
        if (misses.fetch_add(1) < 4) std::fprintf(stderr, "CP2077 luminance probe: no texture for u0 ref=%u view=%p\n",
            unsigned(storage.reference.ID()), static_cast<void*>(storage.view));
        return;
    }
    // [whitewall] r8: dimensions/format from the engine descriptor (r6 called wgpuTextureGet* here, on a render
    // worker -> getJsObject assertion). The dispatch retains the texture through submission, so the acquire reference
    // is dropped right away and the raw handle is used only by the main-thread copy below.
    const auto desc = GpuApi::GetTextureDesc(storage.reference);
    GpuApi::Release(storage.reference);
    const std::uint32_t width = desc.width, height = desc.height;
    if (desc.format != GpuWrapApi::TEXFMT_R16_Float || !width || !height || width * height > 1024u * 1024u) {
        static std::atomic<unsigned> skips{0};
        if (skips.fetch_add(1) < 4) std::fprintf(stderr, "CP2077 luminance probe: skipped engineFormat=%u %ux%u\n",
            unsigned(desc.format), width, height);
        return;
    }
    const unsigned ref = unsigned(storage.reference.ID());
    QueuePendingCustom(command, [texture, width, height, ref](WGPUCommandEncoder encoder) -> bool {
        const std::uint32_t rowBytes = (width * 2 + 255u) & ~255u;
        if (WGPUBuffer readback = CpProbeReadbackBuffer(rowBytes * height)) {
            WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT; source.texture = texture;
            WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT; destination.buffer = readback;
            destination.layout.bytesPerRow = rowBytes; destination.layout.rowsPerImage = height;
            const WGPUExtent3D size{width, height, 1};
            wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &size);
            CpProbeMapLater(new CpProbeReadback{readback, rowBytes * height, 2, width, height, rowBytes, ref});
        }
        return true;
    });
}
bool DispatchBoundGpuCompute(std::uint32_t x, std::uint32_t y, std::uint32_t z, bool indirect=false) {
    Perf11Pools::UsTimer dispatchTimer(Perf11Pools::frameDispatches, Perf11Pools::frameDispatchUs, true); // [skperf] frame probe
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto current = lock.Find();
    if (current == commands.end() || current->second.owner != std::this_thread::get_id()) return false;
    auto& command = current->second;
    if (command.failed) return false;
    // Same ownership protocol as DrawBoundGpuGraphics: the owning thread mutates its bound list exclusively, so
    // commandMutex is released for the dispatch body (uniform/bind-group creation, flush and encode are synchronous
    // main-thread handoffs that used to stall every other render worker on this mutex). The lease keeps teardown
    // from swapping the registry meanwhile; while teardown is closing the lock is simply kept, as before.
    OwnedCommandLease commandLease(commandOperations,commandMutex);
    if (commandLease) lock.UnlockForOwnedOperation();
    const auto* shader = GetGpuComputeShaderSource(command.computeShader);
    if (!shader || !command.computePipeline) {
        // An unconverted or unavailable compute shader skips only this dispatch, like an unavailable draw.
        // Report each distinct shader once. bound = the ref currently on the list (0 = none: never set on this
        // list, or SetShader(Null)); requested = the last SetShader(compute) ref whose pipeline was unavailable.
        // Engine-side unconverted compute never gets here (CreateShader returns Null, so CRenderShaderCompute
        // skips Dispatch); its names come from the "requires an exact converted bytecode record" log.
        static std::atomic<unsigned> skipped{0};
        static std::mutex skipMutex; static std::set<std::pair<Uint32, Uint32>> skipKeys;
        const unsigned n = skipped.fetch_add(1) + 1;
        ++command.dispatchesSkipped;
        const Uint32 boundId = command.computeShader.ID(), requestedId = command.unavailableComputeShader;
        bool first = false; std::size_t distinct = 0;
        {
            std::lock_guard<std::mutex> guard(skipMutex);
            if (skipKeys.size() < 256) first = skipKeys.emplace(boundId, requestedId).second;
            distinct = skipKeys.size();
        }
        if (first) {
            const char* name = shader ? (shader->runtimeName ? shader->runtimeName : shader->name) : nullptr;
            if (distinct <= 8)
                emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,
                    "CP2077 WebGPU: skipped compute dispatch without pipeline: list=%u type=%u boundShader=%u requestedShader=%u name=%s groups=%ux%ux%u indirect=%u total=%u distinct=%u",
                    command.id, unsigned(command.type), boundId, requestedId, name ? name : "-", x, y, z, unsigned(indirect), n, unsigned(distinct));
            else
                std::fprintf(stderr, "CP2077 WebGPU: skipped compute dispatch without pipeline: list=%u type=%u boundShader=%u requestedShader=%u name=%s groups=%ux%ux%u indirect=%u total=%u distinct=%u\n",
                    command.id, unsigned(command.type), boundId, requestedId, name ? name : "-", x, y, z, unsigned(indirect), n, unsigned(distinct));
        } else if ((n & (n - 1)) == 0) {
            // Unbound skips all share key (0,0); sample a JS stack at power-of-two totals so later callers
            // (e.g. after the character preview loads) are still named.
            if (!boundId && n <= 4096)
                emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK,
                    "CP2077 WebGPU: skipped compute dispatch without pipeline sample: list=%u type=%u requestedShader=%u preview=%u groups=%ux%ux%u indirect=%u count=%u distinct=%u",
                    command.id, unsigned(command.type), requestedId, unsigned(command.touchedPreview), x, y, z, unsigned(indirect), n, unsigned(distinct));
            else
                std::fprintf(stderr, "CP2077 WebGPU: skipped compute dispatch without pipeline count=%u distinct=%u\n", n, unsigned(distinct));
        }
        return true;
    }
    std::vector<WGPUBindGroupEntry> entries;
    struct TextureEntry { std::size_t entry; std::uint32_t slot; bool storage; };
    std::vector<TextureEntry> textureEntries;
    // [potato] r6 one dispatch is one usage scope: two storage slots given the same scratch view alias writable
    // storage (e.g. a mip-chain downsample at 848x480 leaves u6 and u7 null) and invalidate the command buffer.
    // The n-th scratch of a (format, dimension) in this dispatch gets texture index n; the first keeps index 0
    // (the original shared scratch), so dispatches with at most one scratch per format/dim are unchanged.
    std::vector<std::pair<WGPUTextureFormat, WGPUTextureViewDimension>> scratchUsed;
    auto scratchStorageView = [&scratchUsed](WGPUTextureFormat format, WGPUTextureViewDimension dimension) {
        unsigned index = 0;
        for (const auto& used : scratchUsed) if (used.first == format && used.second == dimension) ++index;
        WGPUTextureView view = GetGpuScratchStorageTextureView(format, dimension, index);
        if (view) scratchUsed.push_back({format, dimension});
        if (index) {
            static std::atomic<unsigned> distinctReports{0};
            const unsigned n = distinctReports.fetch_add(1) + 1;
            if (n <= 4 || !(n & (n - 1)))
                std::fprintf(stderr, "CP2077 WebGPU: potato6 distinct scratch storage: format=%u dim=%u index=%u ok=%u count=%u\n",
                    unsigned(format), unsigned(dimension), index, unsigned(view != nullptr), n);
        }
        return view;
    };
    std::vector<WGPUBuffer> uniforms;
    std::vector<WGPUTextureView> nullViews;
    std::vector<GpuApi::TextureRef> nullTextures;
    auto cleanup = [&]() {
        for (auto buffer : uniforms) DestroyGpuBuffer(buffer);
        for (auto view : nullViews) ReleaseGpuTextureView(view);
        for (auto texture : nullTextures) GpuApi::Release(texture);
    };
    // A missing/unsupported reflected resource or a failed bind group skips only this dispatch (like a skipped
    // draw); the list stays usable. Releases everything acquired for this dispatch. Logs once per distinct
    // (shader, group, binding, reason).
    auto skipDispatch = [&](const ComputeResourceBinding* resource, const char* reason,
                            std::uint64_t boundBytes, std::uint32_t boundStride) {
        cleanup();
        ++command.dispatchesSkipped;
        static std::atomic<unsigned> skippedForResource{0};
        static std::mutex resourceSkipMutex; static std::set<std::string> resourceSkipKeys;
        const unsigned n = skippedForResource.fetch_add(1) + 1;
        std::string key = shader->name ? shader->name : "-";
        key += ':'; key += std::to_string(resource ? resource->group : ~0u);
        key += ':'; key += std::to_string(resource ? resource->binding : ~0u);
        key += ':'; key += reason;
        bool first = false;
        {
            std::lock_guard<std::mutex> guard(resourceSkipMutex);
            if (resourceSkipKeys.size() < 512) first = resourceSkipKeys.insert(key).second;
        }
        if (first)
            std::fprintf(stderr, "CP2077 WebGPU: skipped compute dispatch (%s): list=%u type=%u shader=%s runtime=%s entry=%s kind=%d group=%u binding=%u slot=%u storageStride=%u boundBytes=%llu boundStride=%u groups=%ux%ux%u indirect=%u total=%u\n",
                reason, command.id, unsigned(command.type), shader->name ? shader->name : "-",
                shader->runtimeName ? shader->runtimeName : "-", shader->entryPoint ? shader->entryPoint : "-",
                resource ? int(resource->kind) : -1, resource ? resource->group : 0u, resource ? resource->binding : 0u,
                resource ? resource->slot : 0u, resource ? resource->storageStride : 0u,
                static_cast<unsigned long long>(boundBytes), boundStride, x, y, z, unsigned(indirect), n);
        else if ((n & (n - 1)) == 0)
            // Aggregate of every resource skip (all keys); names the skip that reached this power of two.
            std::fprintf(stderr, "CP2077 WebGPU: skipped compute dispatch (resource) count=%u last=%s boundBytes=%llu boundStride=%u\n",
                n, key.c_str(), static_cast<unsigned long long>(boundBytes), boundStride);
        return true;
    };
    // Repacked web-layout uniform bytes (perf7, replaces CreateGpuUniformBuffer's synchronous creation per miss):
    // an exact immutable-cache hit keeps its stable buffer (stable bind-group key, worker cache hit); a payload
    // seen before is admitted to that cache as before (one creation, then hits); a first-seen payload goes to this
    // list's uniform ring slot (written at submit, static offset). Ring/arena buffers are never put in uniforms:
    // cleanup() and retirement destroy those.
    static std::atomic<unsigned> uniformHits{0}, uniformAdmitted{0}, uniformRing{0};
    auto bindComputeUniform = [&](WGPUBindGroupEntry& entry, const void* data, std::size_t size) -> bool {
        entry.offset = 0; entry.size = size;
        if ((entry.buffer = FindGpuImmutableUniformBuffer(size, data))) { uniforms.push_back(entry.buffer); ++uniformHits; return true; }
        if (!ShouldAdmitComputeUniform(size, data) && AllocateCommandUniform(command, data, size, entry.buffer, entry.offset)) {
            ++uniformRing; return true;
        }
        entry.offset = 0;
        if (!(entry.buffer = CreateGpuImmutableUniformBuffer(size, data))) return false;
        uniforms.push_back(entry.buffer); ++uniformAdmitted; return true;
    };
    std::vector<unsigned char> packedUniform;
    for (std::size_t i = 0; i < shader->resourceCount; ++i) {
        const auto& resource = shader->resources[i];
        WGPUBindGroupEntry item = WGPU_BIND_GROUP_ENTRY_INIT;
        item.binding = resource.binding;
        if (resource.group != 0) return skipDispatch(&resource, "group!=0", 0, 0);
        bool valid = true;
        const char* reason = "unavailable";
        std::uint64_t boundBytes = 0; std::uint32_t boundStride = 0;
        switch (resource.kind) {
        case ComputeResourceKind::RawUniform:
        case ComputeResourceKind::OtherUniform: {
            const auto* layout = FindUniformLayout(shader->name, resource.group, resource.binding);
            if (!layout) { valid = false; reason = "no uniform layout"; break; }
            if (resource.slot<command.computeConstantBuffers.size() &&
                !(resource.kind==ComputeResourceKind::RawUniform && !command.computeNativeConstants.empty()) &&
                UsesNativeGpuConstantBacking(command.computeConstantBuffers[resource.slot])) {
                // Uniform-page snapshot with queued field copies (as draws, perf6); the slot is never destroyed.
                valid=RepackNativeGpuConstantToUniform(command,*layout,command.computeConstantBuffers[resource.slot],item.buffer,item.offset);
                reason = "native constant repack failed";
                if (CpExposureProbeMode()) CpExposureProbeConstants(shader->name, resource.binding, {}); // [whitewall] r5: native-backed
                if(valid)item.size=layout->webBytes;
                break;
            }
            const std::vector<unsigned char>* bytes = nullptr;
            std::vector<unsigned char> mappedSnapshot;
            if (resource.kind == ComputeResourceKind::RawUniform && !command.computeNativeConstants.empty())
                bytes = &command.computeNativeConstants;
            else if (resource.slot < command.computeConstantBuffers.size()) {
                auto reference = command.computeConstantBuffers[resource.slot];
                auto snapshot = command.constantSnapshots.find(reference.ID());
                if (!reference.isNull() && snapshot != command.constantSnapshots.end() && !snapshot->second.locked)
                    bytes = &snapshot->second.bytes;
                else if (!reference.isNull() && CopyMappedGpuConstantBuffer(reference, mappedSnapshot))
                    bytes = &mappedSnapshot;
            }
            if (!bytes && resource.slot<command.computeConstantBuffers.size() &&
                command.computeConstantBuffers[resource.slot].isNull() &&
                (resource.kind!=ComputeResourceKind::RawUniform || command.computeNativeConstants.empty())) {
                item.buffer=CreateGpuImmutableUniformBuffer(layout->webBytes);
                valid=item.buffer!=nullptr; reason = "zero uniform allocation failed";
                if(valid) {uniforms.push_back(item.buffer);item.size=layout->webBytes;}
                break;
            }
            if (!bytes) { valid = false; reason = "constant bytes unavailable"; break; }
            boundBytes = bytes->size();
            if (CpExposureProbeMode()) CpExposureProbeConstants(shader->name, resource.binding, *bytes); // [whitewall] r5
            // D3D cbuffers have 16-byte granularity: the engine may pass the exact C++ struct (e.g. hair's
            // Uint32 rtSize[2] = 8 bytes) for a cbuffer the layout records as its padded 16-byte size.
            // Zero the padding tail; a shortfall beyond the 16-byte rounding still skips.
            std::vector<unsigned char> padded;
            if (bytes->size() < layout->nativeMinimumBytes) {
                if (((bytes->size() + 15) & ~std::size_t(15)) < layout->nativeMinimumBytes) {
                    valid = false; reason = "constant bytes shorter than cbuffer"; break;
                }
                padded.assign(layout->nativeMinimumBytes, 0);
                if (!bytes->empty()) std::memcpy(padded.data(), bytes->data(), bytes->size());
                static std::atomic<unsigned> padReports{0};
                unsigned padCount = 0;
                if (ShouldReportComputeFallback(padReports, padCount))
                    std::fprintf(stderr, "CP2077 WebGPU: compute constants zero-padded: shader=%s binding=%u slot=%u native=%u cbuffer=%u count=%u\n",
                        shader->name ? shader->name : "-", resource.binding, resource.slot, unsigned(bytes->size()),
                        unsigned(layout->nativeMinimumBytes), padCount);
                bytes = &padded;
            }
            if (emscripten_is_main_runtime_thread()) { valid = false; reason = "uniform upload on main thread"; break; }
            if (!IsGpuDeviceReady()) { valid = false; reason = "uniform upload device not ready"; break; }
            // Same repack as CreateGpuUniformBuffer (layout found above, bytes >= nativeMinimumBytes after padding).
            packedUniform.assign(layout->webBytes, 0);
            if (!RepackUniform(*layout, bytes->data(), bytes->size(), packedUniform.data(), packedUniform.size()) ||
                !bindComputeUniform(item, packedUniform.data(), packedUniform.size())) {
                valid = false; reason = "uniform repack or allocation failed"; break;
            }
            item.size = layout->webBytes;
            break;
        }
        case ComputeResourceKind::SampledTexture:
            valid = valid && resource.slot < command.computeTextures.size();
            if (valid) item.textureView = command.computeTextures[resource.slot].view;
            // Original HUD leaves t1 unbound when film grain is disabled.
            // DX12 starts its SRV table with null descriptors. WebGPU requires
            // an actual binding even for this conditional, unused texture.
            // The original internal zero texture preserves null-read values;
            // never replace a missing view belonging to a non-null resource.
            if (valid && !item.textureView && command.computeTextures[resource.slot].reference.isNull() &&
                resource.slot == 1 && std::strcmp(shader->name, "hud") == 0) {
                auto zero = GpuApi::GetInternalTexture(GpuApi::INTERTEX_Zero);
                if (!zero.isNull() && AcquireSampledGpuTextureView(zero, item.textureView)) {
                    nullViews.push_back(item.textureView);
                    nullTextures.push_back(zero);
                }
            }
            // D3D reads zero from a null SRV (BindComputeTextureRange already substitutes failed views, and a
            // ref/view mismatch fails the list, so a null view here is an engine-null slot). Bind a zero view
            // of the declared WGSL dimension/sample type. Borrowed device-lifetime view: never put it in
            // nullViews (both cleanup and the success path release those).
            if (valid && item.textureView) {
                // A real view must match the auto-layout entry exactly (dimension and sample type), or the bind
                // group error fails the whole submission: e.g. the engine's 2D zero texture (null SRV) in a
                // texture_3d / texture_2d_array slot, or a depth view in a gathered/sampled texture_2d<f32>
                // (layout Float). Bind a zero view of the declared type instead (D3D null-SRV semantics; the slot
                // keeps owning the real view), else skip this dispatch.
                GpuWgslBinding declared; WGPUTextureFormat viewFormat; WGPUTextureViewDimension viewDimension;
                bool filtered = false;
                if (const char* mismatch = ComputeSampledViewMismatch(shader->wgsl, shader->wgslBytes, resource.binding,
                        item.textureView, declared, viewFormat, viewDimension, filtered)) {
                    item.textureView = GetGpuDefaultTextureView(declared);
                    const auto& bound = command.computeTextures[resource.slot];
                    static std::atomic<unsigned> mismatchReports{0};
                    unsigned mismatchCount = 0;
                    if (ShouldReportComputeFallback(mismatchReports, mismatchCount))
                        std::fprintf(stderr, "CP2077 WebGPU: compute SRV mismatch zero fallback: shader=%s binding=%u slot=%u ref=%u nativeNull=%u reason=%s viewFormat=%u viewDimension=%u kind=%u dimension=%u filtered=%u ok=%u count=%u\n",
                            shader->name ? shader->name : "-", resource.binding, resource.slot, bound.reference.ID(),
                            unsigned(bound.nativeNull), mismatch, unsigned(viewFormat), unsigned(viewDimension),
                            unsigned(declared.kind), unsigned(declared.dimension), unsigned(filtered),
                            unsigned(item.textureView != nullptr), mismatchCount);
                    if (!item.textureView) { valid = false; reason = "sampled view mismatch, zero view unavailable"; break; }
                }
            } else if (valid && !item.textureView) {
                const auto declared = FindGpuWgslBinding(shader->wgsl, shader->wgslBytes, resource.binding);
                item.textureView = GetGpuDefaultTextureView(declared);
                static std::atomic<unsigned> zeroReports{0};
                unsigned zeroCount = 0;
                if (ShouldReportComputeFallback(zeroReports, zeroCount))
                    std::fprintf(stderr, "CP2077 WebGPU: compute SRV zero fallback: shader=%s binding=%u slot=%u ref=%u kind=%u dimension=%u ok=%u count=%u\n",
                        shader->name ? shader->name : "-", resource.binding, resource.slot,
                        command.computeTextures[resource.slot].reference.ID(), unsigned(declared.kind),
                        unsigned(declared.dimension), unsigned(item.textureView != nullptr), zeroCount);
                if (!item.textureView) { valid = false; reason = declared.kind == GpuWgslBinding_Unknown ?
                    "null sampled view, WGSL type unsupported for zero view" : "null sampled view, zero view unavailable"; break; }
            }
            reason = valid ? "null sampled texture view" : "sampled texture slot out of range";
            valid = valid && item.textureView; break;
        case ComputeResourceKind::StorageTexture:
            valid = valid && resource.slot < command.computeStorageTextures.size();
            if (valid) item.textureView = command.computeStorageTextures[resource.slot].view;
            reason = valid ? "null storage texture view" : "storage texture slot out of range";
            if (valid && !item.textureView) {
                // D3D drops writes to a null UAV (e.g. GenerateMipmaps of a 6-mip cube binds only u1..u5 of the
                // downsample's u1..u6). Write-only slots bind a discarded 1x1 scratch storage view of the WGSL
                // texel format/dimension (borrowed device-lifetime handle, held by the queued dispatch like the
                // overlap scratch below) instead of skipping the whole dispatch. read/read_write slots still skip.
                const auto format = FindWgslStorageTextureFormat(shader->wgsl, shader->wgslBytes, resource.binding);
                const auto dimension = FindWgslWriteOnlyStorageTextureDimension(shader->wgsl, shader->wgslBytes, resource.binding);
                if (format != WGPUTextureFormat_Undefined && dimension != WGPUTextureViewDimension_Undefined) {
                    item.textureView = scratchStorageView(format, dimension);
                    static std::atomic<unsigned> dummyReports{0};
                    unsigned dummyCount = 0;
                    if (ShouldReportComputeFallback(dummyReports, dummyCount))
                        std::fprintf(stderr, "CP2077 WebGPU: compute null storage dummy bound: shader=%s runtime=%s binding=%u slot=%u format=%u dim=%u ok=%u count=%u\n",
                            shader->name ? shader->name : "-", shader->runtimeName ? shader->runtimeName : "-",
                            resource.binding, resource.slot, unsigned(format), unsigned(dimension),
                            unsigned(item.textureView != nullptr), dummyCount);
                    if (!item.textureView) reason = "null storage texture view, dummy unavailable";
                }
                valid = item.textureView != nullptr;
                break; // the dummy matches the declared format by construction
            }
            valid = valid && item.textureView;
            if (valid) {
                // A storage view whose format differs from the WGSL texel format is an asynchronous bind-group
                // validation error that fails the whole batch. WebGPU cannot reinterpret it (view formats must
                // be listed at texture creation), so skip only this dispatch. Unknown formats are not compared.
                const auto declared = FindWgslStorageTextureFormat(shader->wgsl, shader->wgslBytes, resource.binding);
                WGPUTextureFormat boundFormat = WGPUTextureFormat_Undefined;
                if (declared != WGPUTextureFormat_Undefined &&
                    GetGpuStorageTextureFormat(command.computeStorageTextures[resource.slot].reference, boundFormat) &&
                    boundFormat != declared) {
                    valid = false; reason = "storage texture format mismatch";
                    boundBytes = unsigned(boundFormat); boundStride = unsigned(declared);
                }
            }
            break;
        case ComputeResourceKind::SamplerBias: {
            static_assert(GpuApi::MAX_SAMPLER_STATES==16,"WGSL bias array must match native slots");
            Float values[GpuApi::MAX_SAMPLER_STATES][4]{};
            for (std::size_t slot=0;valid && slot<command.computeSamplers.size();++slot) {
                const auto& sampler=command.computeSamplers[slot];
                // Slots the Sampler case replaces with the default sampler (null or unknown bias) apply bias 0.
                if (!sampler.reference.isNull() && sampler.sampler &&
                    !GetEffectiveGpuSamplerBias(sampler.reference,sampler.sampler,values[slot][0])) values[slot][0]=0;
            }
            if (!valid) { reason = "sampler bias unavailable"; break; }
            item.buffer=CreateGpuImmutableUniformBuffer(sizeof(values),values);
            valid=item.buffer!=nullptr; reason = "sampler bias allocation failed";
            if(valid) { uniforms.push_back(item.buffer);item.size=sizeof(values); }
            break;
        }
        case ComputeResourceKind::Sampler: {
            valid = valid && resource.slot < command.computeSamplers.size();
            reason = "sampler slot out of range";
            if (valid) {
                const auto& sampler=command.computeSamplers[resource.slot];
                Float bias=0;
                item.sampler=sampler.sampler;
                // Like draws: a null/unbound sampler or one whose bias is unknown binds the clamp-nearest default
                // of the declared kind (nearest fits filtering and non-filtering layouts). A real nonzero bias
                // the shader cannot apply still skips. Borrowed device-lifetime sampler: never released here.
                const bool useDefault = sampler.reference.isNull() || !sampler.sampler ||
                    !GetEffectiveGpuSamplerBias(sampler.reference,sampler.sampler,bias);
                if (useDefault) {
                    const auto declared = FindGpuWgslBinding(shader->wgsl, shader->wgslBytes, resource.binding);
                    item.sampler = GetGpuDefaultSampler(declared.kind == GpuWgslBinding_ComparisonSampler);
                    // [gamerender] Engine-null samplers in filtering kernels (GaussianBlur, simpleTemporal history)
                    // are static linear-clamp samplers on DX12; nearest made blur/temporal blocky and shimmering.
                    if (declared.kind != GpuWgslBinding_ComparisonSampler &&
                        FindComputeAutoLayoutUse(shader->wgsl, shader->wgslBytes, resource.binding).filteringSampler)
                        if (auto linear = GetGpuDefaultLinearSampler()) item.sampler = linear;
                    static std::atomic<unsigned> samplerReports{0};
                    unsigned samplerCount = 0;
                    if (ShouldReportComputeFallback(samplerReports, samplerCount))
                        std::fprintf(stderr, "CP2077 WebGPU: compute default sampler fallback: shader=%s binding=%u slot=%u ref=%u bound=%u comparison=%u ok=%u count=%u\n",
                            shader->name ? shader->name : "-", resource.binding, resource.slot, sampler.reference.ID(),
                            unsigned(sampler.sampler != nullptr), unsigned(declared.kind == GpuWgslBinding_ComparisonSampler),
                            unsigned(item.sampler != nullptr), samplerCount);
                    valid = item.sampler != nullptr;
                    reason = "null sampler, default sampler unavailable";
                } else {
                    valid = bias==0 || shader->samplerBiasLowered;
                    reason = "sampler bias unsupported";
                    // Auto layout makes a `sampler` NonFiltering unless it samples/gathers a texture_*<f32>; a
                    // linear engine sampler there fails the whole submission. Bind the engine sampler's nearest
                    // twin (same addressing/LOD clamps; gather ignores filtering), else the clamp-nearest default.
                    const auto declared = FindGpuWgslBinding(shader->wgsl, shader->wgslBytes, resource.binding);
                    if (valid && declared.kind == GpuWgslBinding_Sampler &&
                        !FindComputeAutoLayoutUse(shader->wgsl, shader->wgslBytes, resource.binding).filteringSampler) {
                        WGPUSampler nearest = GetGpuNonFilteringGatherSampler(sampler.reference, sampler.sampler);
                        const bool twin = nearest != nullptr;
                        if (!nearest) nearest = GetGpuDefaultSampler(false);
                        static std::atomic<unsigned> nonFilteringReports{0};
                        unsigned nonFilteringCount = 0;
                        if (ShouldReportComputeFallback(nonFilteringReports, nonFilteringCount))
                            std::fprintf(stderr, "CP2077 WebGPU: compute non-filtering sampler: shader=%s binding=%u slot=%u ref=%u twin=%u ok=%u count=%u\n",
                                shader->name ? shader->name : "-", resource.binding, resource.slot, sampler.reference.ID(),
                                unsigned(twin), unsigned(nearest != nullptr), nonFilteringCount);
                        item.sampler = nearest;
                        valid = nearest != nullptr;
                        if (!valid) reason = "non-filtering sampler unavailable";
                    }
                }
            }
            break;
        }
        case ComputeResourceKind::StorageBufferCounter:
            valid = valid && resource.slot < command.computeStorageBuffers.size();
            reason = "counter slot out of range";
            if (valid) {
                item.buffer=command.computeStorageBuffers[resource.slot].counter;
                item.offset=0; item.size=4; valid=item.buffer != nullptr; reason = "null counter buffer";
            }
            break;
        case ComputeResourceKind::StorageBuffer:
        case ComputeResourceKind::StorageBufferUAV:
            valid = valid && resource.slot < (resource.kind == ComputeResourceKind::StorageBufferUAV ?
                command.computeStorageBuffers.size() : command.computeBuffers.size());
            reason = "storage buffer slot out of range";
            if (valid) {
                const auto& buffer = resource.kind == ComputeResourceKind::StorageBufferUAV ?
                    command.computeStorageBuffers[resource.slot] : command.computeBuffers[resource.slot];
                boundBytes = buffer.bytes; boundStride = buffer.stride;
                // Converted shaders view structured buffers as u32 words (storageStride 4) while the engine binds
                // the real structure stride (e.g. 160 B clustered light data); any 4-byte multiple size fits.
                // [gamerender] Raw (ByteAddress, stride 4) buffers read with Load4 convert to array<vec4<u32>> indexed
                // by byteOffset/16 (e.g. m_morphTargetMeshSet); the bytes are the same, so a 4-byte bound stride fits
                // any 4-byte-multiple element stride (the size check below still requires whole elements).
                const bool strideFits = buffer.stride == resource.storageStride || resource.storageStride == 4 ||
                    (buffer.stride == 4 && resource.storageStride % 4 == 0);
                valid = buffer.buffer && resource.storageStride && strideFits &&
                    buffer.bytes >= resource.storageStride && buffer.bytes % resource.storageStride == 0;
                reason = !buffer.buffer ? "null storage buffer" : !strideFits ? "storage stride mismatch" : "storage size not stride multiple";
                if (valid) { item.buffer = buffer.buffer; item.offset = 0; item.size = buffer.bytes; }
                // Camera-less HUD explicitly binds BufferRef::Null() at t9.
                // Its original DX12 null SRV returns zero on reads. This exact
                // converted shader only reads FrameExposureData[0]; it never
                // queries buffer length, so a zeroed 28-byte element preserves
                // every observable read without inventing an exposure result.
                if (!valid && resource.kind == ComputeResourceKind::StorageBuffer &&
                    resource.group == 0 && resource.binding == 73 && resource.slot == 9 &&
                    resource.storageStride == 28 && std::strcmp(shader->name, "hud") == 0 &&
                    buffer.reference.isNull() && !buffer.buffer && !buffer.bytes && !buffer.stride) {
                    const std::uint32_t zero[7]{};
                    item.buffer = CreateGpuBuffer(sizeof(zero), WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst,
                        zero, sizeof(zero), "cp-hud-zero");
                    valid = item.buffer != nullptr;
                    if (valid) { uniforms.push_back(item.buffer); item.offset = 0; item.size = sizeof(zero); }
                }
            }
            break;
        }
        if (!valid) return skipDispatch(&resource, reason, boundBytes, boundStride);
        entries.push_back(item);
        if (resource.kind == ComputeResourceKind::SampledTexture || resource.kind == ComputeResourceKind::StorageTexture)
            textureEntries.push_back({entries.size() - 1, resource.slot, resource.kind == ComputeResourceKind::StorageTexture});
    }
    // One dispatch is one WebGPU usage scope: a subresource bound both as storage and as a sampled texture fails the
    // whole command buffer ("includes writable usage and another usage in the same synchronization scope"). D3D
    // tolerates it when the shader leaves that UAV unwritten: GenerateDepthMipmaps' second batch on the HiZ
    // RG32Float chain samples mip 6 while stale UAV slots u2..u6 still hold mips 2..6 from the first batch (the
    // shader's mips constant bounds its writes to u1). Bind a discarded 1x1 scratch storage view of the same
    // format/dimension in such a storage slot instead. Only when both selected ranges are known and overlap.
    for (const auto& storage : textureEntries) {
        if (!storage.storage) continue;
        const auto& bound = command.computeStorageTextures[storage.slot];
        auto& storageEntry = entries[storage.entry];
        if (bound.reference.isNull() || storageEntry.textureView != bound.view) continue;
        std::uint32_t sMip, sMips, sLayer, sLayers;
        if (!GetGpuTextureViewRange(storageEntry.textureView, sMip, sMips, sLayer, sLayers)) continue;
        for (const auto& sampled : textureEntries) {
            if (sampled.storage) continue;
            const auto& source = command.computeTextures[sampled.slot];
            const auto& sampledEntry = entries[sampled.entry];
            if (source.reference.isNull() || source.reference.ID() != bound.reference.ID() ||
                sampledEntry.textureView != source.view) continue;
            std::uint32_t tMip, tMips, tLayer, tLayers;
            if (!GetGpuTextureViewRange(sampledEntry.textureView, tMip, tMips, tLayer, tLayers)) continue;
            auto end = [](std::uint32_t base, std::uint32_t count) {
                return count == 0xFFFFFFFFu ? UINT64_MAX : std::uint64_t(base) + count;
            };
            const bool overlap = sMip < end(tMip, tMips) && tMip < end(sMip, sMips) &&
                sLayer < end(tLayer, tLayers) && tLayer < end(sLayer, sLayers);
            if (!overlap) continue;
            WGPUTextureFormat format; WGPUTextureViewDimension dimension;
            WGPUTextureView scratch = GetGpuTextureViewInfo(storageEntry.textureView, format, dimension)
                ? scratchStorageView(format, dimension) : nullptr;
            static std::atomic<unsigned> overlapReports{0};
            unsigned overlapCount = 0;
            if (ShouldReportComputeFallback(overlapReports, overlapCount))
                std::fprintf(stderr, "CP2077 WebGPU: compute storage/sampled overlap, scratch storage bound: shader=%s storageBinding=%u uavSlot=%u sampledBinding=%u srvSlot=%u ref=%u storageMip=%u sampledMip=%u/%u format=%u ok=%u count=%u\n",
                    shader->name ? shader->name : "-", storageEntry.binding, storage.slot, sampledEntry.binding, sampled.slot,
                    bound.reference.ID(), sMip, tMip, tMips, unsigned(format), unsigned(scratch != nullptr), overlapCount);
            if (scratch) storageEntry.textureView = scratch; // borrowed device-lifetime view
            break;
        }
    }
    // Queued in encoder order (perf7): no synchronous main-thread handoff here. A worker bind-group-cache hit is
    // used directly; otherwise the group is created on the main thread when the batch is encoded.
    static std::atomic<unsigned> queuedDispatches{0}, workerHits{0};
    auto queued = std::make_shared<QueuedComputeDispatch>();
    auto group = FindCachedGpuComputeBindGroup(command.computePipeline, entries.data(), entries.size());
    if (group) ++workerHits;
    else { queued->entries = entries; queued->HoldEntries(); }
    const ComputeBindGroup binding{0, group};
    bool retainSkipped = false;
    if (!RecordGpuComputeDispatchLocked(command, group ? &binding : nullptr, group ? 1 : 0, x, y, z, indirect, &retainSkipped, queued)) {
        if (group) ReleaseGpuBindGroup(group);
        queued.reset(); // releases the held entry references (asynchronously on main)
        if (retainSkipped) return skipDispatch(nullptr, "binding retain failed", 0, 0);
        cleanup(); return false;
    }
    {
        const unsigned n = queuedDispatches.fetch_add(1) + 1;
        if (n == 1 || (n >= 64 && !(n & (n - 1))))
            std::fprintf(stderr, "CP2077 queued compute dispatch probe: queued=%u workerGroupHits=%u uniformImmutableHits=%u uniformAdmitted=%u uniformRingSlots=%u sameFrameDeferred=%u frame=%u\n",
                n, workerHits.load(), uniformHits.load(), uniformAdmitted.load(), uniformRing.load(), uniformSameFrameDeferred.load(), unsigned(publishedRenderFrame.load()));
    }
    if (CpExposureProbeMode() && shader->name) CpExposureProbeDispatch(command, shader->name); // [whitewall] ?expprobe=1
    // Uniform buffers belong to this dispatch and stay alive through submission.
    command.uploads.insert(command.uploads.end(), uniforms.begin(), uniforms.end());
    command.retainedTextures.insert(command.retainedTextures.end(), nullTextures.begin(), nullTextures.end());
    // The successful bind group owns its views; retain original textures until
    // submission completion so their engine Release cannot destroy them early.
    for (auto view : nullViews) ReleaseGpuTextureView(view);
    return true;
}
bool SetBoundGpuComputeSampler(std::uint32_t slot, const GpuApi::SamplerStateRef& reference, WGPUSampler sampler) {
    return SetBoundGpuSampler(slot, reference, sampler, 2);
}
bool SetBoundGpuStorageBuffer(std::uint32_t slot, const GpuApi::BufferRef& reference,
    WGPUBuffer buffer, std::uint64_t bytes, std::uint32_t stride, WGPUBuffer counter) {
    CommandEntry::BufferBinding previous;
    CommandEntry::TextureBinding previousTexture;
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        const auto found = lock.Find();
        if (found == commands.end() || found->second.owner != std::this_thread::get_id() || found->second.failed) return false;
        auto& command = found->second;
        if (slot >= command.computeStorageBuffers.size() || reference.isNull() != (buffer == nullptr) ||
            (buffer && (!bytes || !stride || bytes % stride)) ||
            (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute)) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        previous=command.computeStorageBuffers[slot];
        previousTexture=command.computeStorageTextures[slot];
        command.computeStorageBuffers[slot]={reference,buffer,bytes,stride,counter};
        command.computeStorageTextures[slot]={};
    }
    if (!previous.reference.isNull()) GpuApi::Release(previous.reference);
    if (previousTexture.view) ReleaseGpuTextureView(previousTexture.view);
    if (!previousTexture.reference.isNull()) GpuApi::Release(previousTexture.reference);
    return true;
}
bool InvalidateBoundGpuSamplers() {
    std::vector<GpuApi::SamplerStateRef> retired;
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        const auto found = commands.find(bound.ID());
        if (found == commands.end() || found->second.owner != std::this_thread::get_id() || found->second.failed) return false;
        auto clear = [&](auto& samplers) {
            for (auto& binding : samplers) {
                if (!binding.reference.isNull()) retired.push_back(binding.reference);
                binding = {};
            }
        };
        for (auto& stage : found->second.graphicsSamplers) clear(stage);
        clear(found->second.computeSamplers);
    }
    for (auto reference : retired) GpuApi::Release(reference);
    return true;
}
bool SetBoundGpuSampler(std::uint32_t slot, const GpuApi::SamplerStateRef& reference,
                       WGPUSampler sampler, std::uint32_t stageIndex) {
    GpuApi::SamplerStateRef previous = GpuApi::SamplerStateRef::Null();
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (stageIndex > 2 || slot >= GpuApi::MAX_SAMPLER_STATES ||
            reference.isNull() != (sampler == nullptr) ||
            (stageIndex < 2 && command.type != GpuApi::CommandListType::Default) ||
            (stageIndex == 2 && command.type != GpuApi::CommandListType::Default &&
                                command.type != GpuApi::CommandListType::Compute)) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        auto& samplers = stageIndex == 2 ? command.computeSamplers : command.graphicsSamplers[stageIndex];
        previous = samplers[slot].reference;
        samplers[slot] = {reference, sampler};
    }
    if (!previous.isNull()) GpuApi::Release(previous);
    return true;
}
void FailBoundGpuCommand(const char* site, int line) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry != commands.end() && entry->second.owner == std::this_thread::get_id()) {
        static std::atomic<unsigned> reported{0};
        if (!entry->second.failed && reported.fetch_add(1) < 4)
            emscripten_log(EM_LOG_ERROR | EM_LOG_JS_STACK, "CP2077 first command failure: id=%u type=%u site=%s:%d", bound.ID(), unsigned(entry->second.type), site, line);
        // Attribute the failure to the API call that failed, not to this helper.
        MarkBrowserCommandFailed(entry->second, site, line);
    }
}
bool BoundGpuCommandAlreadyFailed() {
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto entry = lock.Find();
    return entry != commands.end() && entry->second.owner == std::this_thread::get_id() && entry->second.failed;
}
bool SetBoundGpuComputeShader(const GpuApi::ShaderRef& shader, WGPUComputePipeline pipeline) {
    GpuApi::ShaderRef previous = GpuApi::ShaderRef::Null();
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
        auto& command = entry->second;
        if (command.failed) return false;
        if (command.type != GpuApi::CommandListType::Default && command.type != GpuApi::CommandListType::Compute) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        previous = command.computeShader;
        command.computeShader = shader; command.computePipeline = pipeline;
        command.unavailableComputeShader = 0;
    }
    if (!previous.isNull()) GpuApi::Release(previous);
    return true;
}
// Diagnostics: remember which requested compute shader was unavailable so a later skipped dispatch can name it.
// Declared locally in gpu-api-shader-web.cpp (SetShader compute branch); call after SetBoundGpuComputeShader(Null).
void NoteBoundGpuComputeShaderUnavailable(Uint32 requestedShader) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry != commands.end() && entry->second.owner == std::this_thread::get_id())
        entry->second.unavailableComputeShader = requestedShader;
}
bool SetBoundGpuGraphicsShader(const GpuApi::ShaderRef& shader, GraphicsShaderStage stage,
                              WGPUShaderModule module, const ConvertedGraphicsShader* source) {
    GpuApi::ShaderRef previous = GpuApi::ShaderRef::Null();
    {
        CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
        auto entry = lock.Find();
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id() || entry->second.failed) {
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1) < 16) std::fprintf(stderr, "CP2077 shader bind rejected: bound=%u found=%u ownerMatch=%u type=%u failed=%u firstFailure=%s:%d\n",
                bound.ID(), unsigned(entry != commands.end()), unsigned(entry != commands.end() && entry->second.owner == std::this_thread::get_id()),
                entry != commands.end() ? unsigned(entry->second.type) : 0u, entry != commands.end() ? unsigned(entry->second.failed) : 0u,
                entry != commands.end() && entry->second.failedOperation ? entry->second.failedOperation : "-", entry != commands.end() ? entry->second.failedLine : 0);
            return false;
        }
        auto& command = entry->second;
        // Unbinding a graphics stage on a compute/copy list (missing-shader placeholder Bind) has nothing to undo.
        if (shader.isNull() && command.type != GpuApi::CommandListType::Default) return true;
        if (command.type != GpuApi::CommandListType::Default ||
            (stage != GraphicsShaderStage::Vertex && stage != GraphicsShaderStage::Pixel) ||
            (shader.isNull() ? (module || source) : (!module || !source || source->stage != stage))) {
            MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
        }
        auto& binding = command.graphicsShaders[stage == GraphicsShaderStage::Vertex ? 0 : 1];
        previous = binding.reference;
        binding = {shader, module, source};
    }
    if (!previous.isNull()) GpuApi::Release(previous);
    return true;
}
bool MarkBoundGpuGraphicsShaderUnavailable(GraphicsShaderStage stage) {
    GpuApi::ShaderRef previous = GpuApi::ShaderRef::Null();
    {
        CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
        auto entry = commands.find(bound.ID());
        if (entry == commands.end() || entry->second.owner != std::this_thread::get_id() || entry->second.failed ||
            entry->second.type != GpuApi::CommandListType::Default ||
            (stage != GraphicsShaderStage::Vertex && stage != GraphicsShaderStage::Pixel)) return false;
        auto& binding = entry->second.graphicsShaders[stage == GraphicsShaderStage::Vertex ? 0 : 1];
        previous = binding.reference;
        binding = {};
        binding.unavailable = true;
    }
    if (!previous.isNull()) GpuApi::Release(previous);
    return true;
}
bool RecordGpuBufferCopy(WGPUBuffer source, std::uint64_t sourceOffset,
    WGPUBuffer destination, std::uint64_t destinationOffset, std::uint64_t size,
    const GpuApi::BufferRef& sourceRef, const GpuApi::BufferRef& destinationRef) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    auto& command = entry->second;
    for (const auto& reference : {sourceRef,destinationRef}) {
        const auto snapshot=command.constantSnapshots.find(reference.ID());
        if (UsesNativeGpuConstantBacking(reference) && snapshot!=command.constantSnapshots.end() && snapshot->second.locked) {
            MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
        }
    }
    if (command.failed || !QueuePendingBufferCopy(command, source, sourceOffset, destination, destinationOffset, size)) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    if (sourceRef.isNull()) command.uploads.push_back(source);
    else command.retainedBuffers.push_back(sourceRef);
    command.retainedBuffers.push_back(destinationRef);
    return true;
}
bool RecordGpuTextureBufferCopy(const WGPUTexelCopyTextureInfo& texture, const WGPUTexelCopyBufferInfo& buffer,
    const WGPUExtent3D& extent, const GpuApi::TextureRef& reference, bool upload) {
    if (upload) SharedConv::NoteWrite(reference); // [skperf]
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    const auto found = commands.find(bound.ID());
    if (found == commands.end() || found->second.owner != std::this_thread::get_id() || found->second.failed) return false;
    OwnedCommandLease commandLease(commandOperations,commandMutex); // owner-exclusive: handoffs below run unlocked
    if (commandLease) lock.UnlockForOwnedOperation();
    auto& command = found->second;
    // Queued in encoder order (perf7): EncodeGpuTextureBufferCopy runs inline on the main thread inside the next
    // batched flush instead of a flush plus its own synchronous handoff. Same argument checks, now at the call.
    if (upload && cp2077::web::IsTier1RemappedTexture(texture.texture)) { // [notier1] GPU-side data: not convertible here
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 8) std::fprintf(stderr, "CP2077 notier1: buffer upload into a remapped texture (%ux%u) is not widened\n",
            extent.width, extent.height);
    }
    if (!command.encoder || !texture.texture || !buffer.buffer || !extent.width || !extent.height || !extent.depthOrArrayLayers ||
        !QueuePendingCustom(command, [texture, buffer, extent, upload](WGPUCommandEncoder encoder) {
            return EncodeGpuTextureBufferCopy(encoder, texture, buffer, extent, upload);
        })) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
    command.retainedTextures.push_back(reference);
    if (upload) command.uploads.push_back(buffer.buffer);
    return true;
}
bool RecordGpuTextureUploadFromArena(const WGPUTexelCopyTextureInfo& texture, const void* bytes, std::uint64_t size,
    std::uint32_t bytesPerRow, std::uint32_t rowsPerImage, const WGPUExtent3D& extent, const GpuApi::TextureRef& reference) {
    if (!bytes || !size || size % 4) return false;
    SharedConv::NoteWrite(reference); // [skperf]
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    const auto found = commands.find(bound.ID());
    if (found == commands.end() || found->second.owner != std::this_thread::get_id() || found->second.failed) return false;
    OwnedCommandLease commandLease(commandOperations,commandMutex); // owner-exclusive: handoffs below run unlocked
    if (commandLease) lock.UnlockForOwnedOperation();
    auto& command = found->second;
    std::vector<unsigned char> widened; // [notier1] remapped textures (e.g. an r8 atlas stored as r32float) take widened rows
    if (cp2077::web::GpuLacksTier1()) {
        WGPUTexelCopyBufferLayout source = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT, wide;
        source.bytesPerRow = bytesPerRow; source.rowsPerImage = rowsPerImage;
        if (cp2077::web::WidenTier1TextureUpload(texture, bytes, size, source, extent, widened, wide, 256)) {
            bytes = widened.data(); size = widened.size(); bytesPerRow = wide.bytesPerRow; rowsPerImage = wide.rowsPerImage;
        }
    }
    // The arena is exclusively this command's until GPU retirement, so writing it now cannot disturb other work.
    auto lease = StagingArena::Acquire(size);
    if (!lease.buffer) { MarkBrowserCommandFailed(command, __func__, __LINE__); return false; }
    command.stagingArenas.push_back(lease);
    WGPUTexelCopyBufferInfo buffer = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    buffer.buffer = lease.buffer; buffer.layout.bytesPerRow = bytesPerRow; buffer.layout.rowsPerImage = rowsPerImage;
    // Queued (perf7): the arena write (queue.writeBuffer, ahead of this list's later submit) and the copy both run
    // on the main thread inside the next batched flush, replacing two synchronous handoffs.
    auto staged = std::make_shared<std::vector<unsigned char>>(static_cast<const unsigned char*>(bytes),
        static_cast<const unsigned char*>(bytes) + size);
    if (!command.encoder || !texture.texture || !extent.width || !extent.height || !extent.depthOrArrayLayers ||
        !QueuePendingCustom(command, [texture, buffer, extent, staged](WGPUCommandEncoder encoder) {
            return cp2077::web::WriteGpuBufferRange(buffer.buffer, 0, staged->data(), staged->size()) &&
                EncodeGpuTextureBufferCopy(encoder, texture, buffer, extent, true);
        })) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    command.retainedTextures.push_back(reference);
    return true;
}
bool ShutdownGpuCommands() {
    std::unordered_map<Uint32, CommandEntry> retired;
    { std::unique_lock<std::mutex> lock(commandMutex);
        if(!commandOperations.BeginClose(lock,emscripten_is_main_runtime_thread()))return false;
        commandShutdownGeneration=GpuDeviceGeneration();commandRegistryEpoch.fetch_add(1);retired.swap(commands);
    }
    for (auto& item : retired) {
        ReleaseGpuCommandEncoder(item.second.encoder);
        ReleaseCommandResources(item.second);
    }
    bound = GpuApi::CommandListRef::Null(); tlBoundEntry = {};
    return true;
}
bool RecordGpuTextureCopies(const GpuTextureCopy* copies, std::size_t count,
    const GpuApi::TextureRef& source, const GpuApi::TextureRef& destination) {
    SharedConv::NoteWrite(destination); // [skperf]
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    OwnedCommandLease commandLease(commandOperations,commandMutex); // owner-exclusive: handoffs below run unlocked
    if (commandLease) lock.UnlockForOwnedOperation();
    auto& command = entry->second;
    // Queued (perf7): validated now, encoded inline on the main thread by the next batched flush.
    if (command.failed || source.isNull() || destination.isNull() || !command.encoder ||
        !ValidateGpuTextureCopies(copies, count) ||
        (count && !QueuePendingCustom(command, [owned = std::vector<GpuTextureCopy>(copies, copies + count)](WGPUCommandEncoder encoder) {
            return EncodeGpuTextureCopies(encoder, owned.data(), owned.size());
        }))) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    command.retainedTextures.push_back(source); command.retainedTextures.push_back(destination);
    return true;
}
bool RecordGpuDepthUnorm16Copy(WGPUTextureView source,WGPUTextureView destination,
    std::uint32_t sx,std::uint32_t sy,std::uint32_t dx,std::uint32_t dy,
    std::uint32_t width,std::uint32_t height,
    const GpuApi::TextureRef& sourceRef,const GpuApi::TextureRef& destinationRef) {
    SharedConv::NoteWrite(destinationRef); // [skperf]
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry=commands.find(bound.ID());
    if(entry==commands.end()||entry->second.owner!=std::this_thread::get_id())return false;
    OwnedCommandLease commandLease(commandOperations,commandMutex); // owner-exclusive: handoffs below run unlocked
    if (commandLease) lock.UnlockForOwnedOperation();
    auto& command=entry->second;
    if(command.failed||sourceRef.isNull()||destinationRef.isNull()||!width||!height)return false;
    const std::uint32_t region[8]{sx,sy,dx,dy,width,height,0,0};
    // perf7: the region lives in this list's uniform ring slot (written at submit) instead of a validated buffer
    // per copy, and the copy is queued; the caller releases both views on return, so the op holds its own refs.
    WGPUBuffer buffer=nullptr;std::uint64_t offset=0;
    if(!source||!destination||!command.encoder||!AllocateCommandUniform(command,region,sizeof(region),buffer,offset)){
        MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
    }
    wgpuTextureViewAddRef(source);wgpuTextureViewAddRef(destination);
    auto sourceHold=std::make_shared<QueuedTextureViewHold>(source);
    auto destinationHold=std::make_shared<QueuedTextureViewHold>(destination);
    if(!QueuePendingCustom(command,[sourceHold,destinationHold,buffer,offset,width,height](WGPUCommandEncoder encoder){
            const bool issued=EncodeGpuDepthUnorm16CopyAt(encoder,sourceHold->view,destinationHold->view,buffer,offset,width,height);
            sourceHold->Release();destinationHold->Release();return issued;
        })){
        MarkBrowserCommandFailed(command,__func__,__LINE__);return false;
    }
    command.retainedTextures.push_back(sourceRef);command.retainedTextures.push_back(destinationRef);
    static std::atomic<unsigned> reports{0};
    if(reports.fetch_add(1)<8)std::fprintf(stderr,"CP2077 depth16 UNORM copy: src=%u dst=%u sourceOrigin=%u,%u targetOrigin=%u,%u size=%ux%u\n",
        sourceRef.ID(),destinationRef.ID(),sx,sy,dx,dy,width,height);
    return true;
}
bool RecordGpuTextureResolve(WGPUTextureView source, WGPUTextureView destination,
    const GpuApi::TextureRef& sourceRef, const GpuApi::TextureRef& destinationRef) {
    SharedConv::NoteWrite(destinationRef); // [skperf]
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    OwnedCommandLease commandLease(commandOperations,commandMutex); // owner-exclusive: handoffs below run unlocked
    if (commandLease) lock.UnlockForOwnedOperation();
    auto& command = entry->second;
    if (command.failed || command.type != GpuApi::CommandListType::Default ||
        sourceRef.isNull() || destinationRef.isNull() || !command.encoder || !source || !destination || source == destination) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    // Queued (perf7); the caller releases both views on return, so the op holds its own references.
    wgpuTextureViewAddRef(source); wgpuTextureViewAddRef(destination);
    auto sourceHold = std::make_shared<QueuedTextureViewHold>(source);
    auto destinationHold = std::make_shared<QueuedTextureViewHold>(destination);
    if (!QueuePendingCustom(command, [sourceHold, destinationHold](WGPUCommandEncoder encoder) {
            const bool issued = EncodeGpuColorResolve(encoder, sourceHold->view, destinationHold->view);
            sourceHold->Release(); destinationHold->Release(); return issued;
        })) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return false;
    }
    command.retainedTextures.push_back(sourceRef); command.retainedTextures.push_back(destinationRef);
    return true;
}
#include "gpu-api-graphics-draw-web.inl"
}
namespace GpuApi {
#include "gpu-api-system-draw-web.inl"
void DrawPrimitiveIndirect(ePrimitiveType primitive, Uint32 commandsCount) {
    if (commandsCount && !cp2077::web::DrawBoundGpuGraphics(primitive, false, 0, 0, 0, 0, 0, commandsCount))
        std::fputs("CP2077 WebGPU: original indirect draw failed\n", stderr);
}
void DrawIndexedPrimitiveIndirect(ePrimitiveType primitive, Uint32 commandsCount) {
    if (commandsCount && !cp2077::web::DrawBoundGpuGraphics(primitive, true, 0, 0, 0, 0, 0, commandsCount))
        std::fputs("CP2077 WebGPU: original indexed indirect draw failed\n", stderr);
}
// NoBuffers draws need a null layout only for themselves. The geometry batcher does not rebind geometry
// (and its layout) when the geometry key is unchanged, so restore the previous layout afterwards; otherwise
// the next mesh draw sees layout=0 with its vertex buffers still bound and is skipped.
static VertexLayoutRef BoundVertexLayout() {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry=commands.find(bound.ID());
    if (entry==commands.end() || entry->second.owner!=std::this_thread::get_id()) return VertexLayoutRef::Null();
    return entry->second.vertexLayout;
}
void DrawPrimitiveIndirectNoBuffers(ePrimitiveType primitive, Uint32 commandsCount) {
    const VertexLayoutRef previous=BoundVertexLayout();
    SetVertexFormatRaw(VertexLayoutRef::Null()); DrawPrimitiveIndirect(primitive, commandsCount);
    if (!previous.isNull()) SetVertexFormatRaw(previous);
}
void DrawIndexedPrimitiveIndirectNoBuffers(ePrimitiveType primitive, Uint32 commandsCount) {
    const VertexLayoutRef previous=BoundVertexLayout();
    SetVertexFormatRaw(VertexLayoutRef::Null()); DrawIndexedPrimitiveIndirect(primitive, commandsCount);
    if (!previous.isNull()) SetVertexFormatRaw(previous);
}
void DrawPrimitiveNoBuffers(ePrimitiveType primitive, Uint32 vertexCount, Uint32 instanceCount) {
    const VertexLayoutRef previous=BoundVertexLayout();
    SetVertexFormatRaw(VertexLayoutRef::Null());
    DrawPrimitive(primitive, 0, vertexCount, instanceCount, 0);
    if (!previous.isNull()) SetVertexFormatRaw(previous);
}
void DrawPrimitive(ePrimitiveType primitive, Uint32 baseVertex, Uint32 vertexCount,
                   Uint32 instanceCount, Uint32 instanceOffset) {
    if (!cp2077::web::DrawBoundGpuGraphics(primitive, false, baseVertex, 0,
                                         vertexCount, instanceCount, instanceOffset))
        std::fputs("CP2077 WebGPU: original graphics draw failed\n", stderr);
}
void DrawIndexedPrimitive(ePrimitiveType primitive, Uint32 baseVertex, Uint32 indexOffset,
                          Uint32 indexCount, Uint32 instanceCount, Uint32 instanceOffset) {
    if (!cp2077::web::DrawBoundGpuGraphics(primitive, true, baseVertex, indexOffset,
                                         indexCount, instanceCount, instanceOffset))
        std::fputs("CP2077 WebGPU: original indexed graphics draw failed\n", stderr);
}
void DispatchCompute(Uint32 x, Uint32 y, Uint32 z, ComputeCuUsage) {
    // The desktop DX12 implementation also dispatches these exact workgroup
    // counts; CU usage is a scheduling hint without a WebGPU counterpart.
    if (!cp2077::web::DispatchBoundGpuCompute(x, y, z))
        std::fputs("CP2077 WebGPU: original compute dispatch failed\n", stderr);
}
void DispatchIndirectCompute() {
    if (!cp2077::web::DispatchBoundGpuCompute(0, 0, 0, true))
        std::fputs("CP2077 WebGPU: original indirect compute dispatch failed\n", stderr);
}
}
