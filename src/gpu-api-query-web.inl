namespace {
struct BrowserQueryStorage {
    WGPUQuerySet queries = nullptr;
    WGPUBuffer resolve = nullptr, readback = nullptr;
    Uint32 count = 0;
    std::mutex mutex;
    std::vector<bool> issued;
    std::vector<GpuApi::GpuFence> fences;
    std::vector<Uint64> acquiredValues;
    Uint32 acquiredStart = 0, acquiredCount = 0;
    ~BrowserQueryStorage() {
        cp2077::web::ReleaseGpuQuerySet(queries);
        cp2077::web::DestroyGpuBuffer(resolve);
        cp2077::web::DestroyGpuBuffer(readback);
    }
};
}
namespace GpuApi {
struct QueryPool { std::shared_ptr<BrowserQueryStorage> storage; };
void QueryPoolDeleter::operator()(QueryPool* pool) const { delete pool; }
QueryPoolPtr CreateQueryPool(QueryType type, Uint32 count) {
    // Occlusion needs integration with actual geometry passes. Pipeline
    // statistics have no standard WebGPU query equivalent.
    if (type != QueryType::Timestamp || !count || count > 4096) return QueryPoolPtr{};
    auto storage = std::make_shared<BrowserQueryStorage>();
    storage->queries = cp2077::web::CreateGpuTimestampQuerySet(count);
    if (!storage->queries) return QueryPoolPtr{};
    storage->count = count; storage->issued.resize(count); storage->fences.resize(count);
    storage->resolve = cp2077::web::CreateGpuBuffer(Uint64(count) * 8, WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc, nullptr, 0, "cp-query-resolve");
    storage->readback = cp2077::web::CreateGpuBuffer(Uint64(count) * 8, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, nullptr, 0, "cp-query-readback");
    if (!storage->resolve || !storage->readback) return QueryPoolPtr{};
    return QueryPoolPtr{new QueryPool{std::move(storage)}};
}
static bool GpuTimerReadOn(); // defined below
static bool QueryGuardOn() { // [skperf] round 47, see IssueQuery
    static const int v = [] {
        const int on = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('queryGuard') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: queryGuard=%d\n", on); return on; }();
    return v != 0;
}
void IssueQuery(const QueryPoolPtr& pool, Uint32 index) {
    if (!pool) { cp2077::web::FailBoundGpuCommand(); return; }
    const bool cpQueryGuard = !GpuTimerReadOn() && QueryGuardOn(); // [skperf] round 47: switch reads (main-thread proxy) before the locks
    const auto storage = pool->storage;
    std::lock_guard<std::mutex> commandsLock(commandMutex);
    std::lock_guard<std::mutex> queryLock(storage->mutex);
    const auto found = commands.find(bound.ID());
    if (found == commands.end() || found->second.owner != std::this_thread::get_id()) return;
    auto& command = found->second;
    // The timestamp pass is queued in encoder order and encoded by the next batched main-thread flush (it used to
    // flush plus hand off synchronously with commandMutex held). retainedQueryPools keeps the query set alive.
    WGPUQuerySet querySet = storage->queries;
    // [skperf] round 47 queryGuard (default ON, ?queryGuard=0 old): reusing a timestamp slot whose previous resolve has
    // not completed on the GPU (GPU running behind: pipeline compilation, a heavy scene) failed the WHOLE command list,
    // so every later call of that list was rejected and the frame lost its rendering (tc34: black screen / 1 fps bar,
    // also the tc31 late collapse). With gpuTimerRead off (default) the timestamps are never read back (AcquireQueries
    // returns false), so skipping this one timestamp write is invisible; the list and its draws are kept.
    if (!command.failed && index < storage->count && querySet && cpQueryGuard &&
        (storage->acquiredCount || (storage->fences[index] && !IsGpuFenceComplete(storage->fences[index])))) {
        static std::atomic<unsigned> guarded{0};
        const unsigned n = guarded.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n & (n - 1)) == 0)
            std::fprintf(stderr, "CP2077 skperf queryGuard: timestamp slot %u still in flight, write skipped, list kept (count=%u)\n", index, n);
        return;
    }
    if (command.failed || index >= storage->count || storage->acquiredCount ||
        (storage->fences[index] && !IsGpuFenceComplete(storage->fences[index])) || !querySet ||
        !QueuePendingCustom(command, [querySet, index](WGPUCommandEncoder encoder) {
            return cp2077::web::EncodeGpuTimestamp(encoder, querySet, index); })) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return;
    }
    storage->issued[index] = true; storage->fences[index] = GpuFence::Null();
    command.retainedQueryPools.push_back(storage);
}
void ResolveQueries(const QueryPoolPtr& pool, Uint32 start, Uint32 count) {
    if (!pool || !count) { cp2077::web::FailBoundGpuCommand(); return; }
    // Reserve the completion fence before taking commandMutex.
    const auto fence = GetGpuFence();
    const auto storage = pool->storage;
    std::lock_guard<std::mutex> commandsLock(commandMutex);
    std::lock_guard<std::mutex> queryLock(storage->mutex);
    const auto found = commands.find(bound.ID());
    if (found == commands.end() || found->second.owner != std::this_thread::get_id()) return;
    auto& command = found->second;
    bool valid = fence && !command.failed && !storage->acquiredCount && start < storage->count && count <= storage->count-start;
    // The original GPUFrameTimer resolves its complete frame range at startup,
    // including slots not yet written. WebGPU resolveQuerySet has no issued-slot
    // precondition. Resolve on the actual GPU; never manufacture CPU timestamps.
    // Existing bounds, acquisition, command and fence checks remain required.
    // Queued like IssueQuery: encoded in order by the next batched flush; the pool is retained by the list.
    WGPUQuerySet querySet = storage->queries; WGPUBuffer resolve = storage->resolve, readback = storage->readback;
    if (!valid || !querySet || !resolve || !readback ||
        !QueuePendingCustom(command, [querySet, start, count, resolve, readback](WGPUCommandEncoder encoder) {
            return cp2077::web::EncodeGpuQueryResolve(encoder, querySet, start, count, resolve, readback); })) {
        MarkBrowserCommandFailed(command, __func__, __LINE__); return;
    }
    for (Uint32 i=0;i<count;++i) { storage->fences[start+i]=fence; storage->issued[start+i]=false; }
    command.retainedQueryPools.push_back(storage);
}
// [nomadperf] GPUFrameTimer::Update (render frame tick, every frame) acquired last frame's timestamps through a
// synchronous MAP_READ: a proxy that waits for the main thread to drain every queued submission, then for the GPU to
// finish them, i.e. a full CPU<->GPU sync on the render critical path (~20 ms of each ~60 ms Nomad-area frame).
// The values only feed the dynamic-resolution heuristic (GetLastGPUFrameDuration), which then reads 0 as before
// any result existed. ?gpuTimerRead=1 = old blocking readback.
static bool GpuTimerReadOn() {
    static std::atomic<int> v{-1};
    int x = v.load(std::memory_order_acquire);
    if (x < 0) {
        x = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('gpuTimerRead') === '1' ? 1 : 0; });
        v.store(x, std::memory_order_release);
        std::fprintf(stderr, "CP2077 nomadperf switches: gpuTimerRead=%d\n", x);
    }
    return x != 0;
}
Bool AcquireQueries(const QueryPoolPtr& pool, Uint32 start, Uint32 count) {
    if (!pool || !count) return false;
    if (!GpuTimerReadOn()) return false;
    const auto storage = pool->storage;
    std::lock_guard<std::mutex> lock(storage->mutex);
    if (storage->acquiredCount || start >= storage->count || count > storage->count-start) return false;
    for (Uint32 i=0;i<count;++i)
        if (!storage->fences[start+i] || !IsGpuFenceComplete(storage->fences[start+i])) return false;
    std::vector<Uint64> values(storage->count);
    if (!cp2077::web::ReadGpuBuffer(storage->readback, Uint64(storage->count)*8, values.data(), values.size()*sizeof(Uint64))) return false;
    storage->acquiredValues.assign(values.begin()+start,values.begin()+start+count);
    storage->acquiredStart=start; storage->acquiredCount=count;
    return true;
}
void ReleaseQueries(const QueryPoolPtr& pool) {
    if (!pool) return;
    const auto storage=pool->storage;
    std::lock_guard<std::mutex> lock(storage->mutex);
    storage->acquiredCount=0; storage->acquiredValues.clear();
}
void GetQueryResult(const QueryPoolPtr& pool, Uint32 index, Uint64& result) {
    if (!pool) return;
    const auto storage=pool->storage;
    std::lock_guard<std::mutex> lock(storage->mutex);
    if (!storage->acquiredCount || index < storage->acquiredStart || index-storage->acquiredStart >= storage->acquiredCount) {
        std::fputs("CP2077 WebGPU: timestamp result requested outside acquired range\n", stderr); return;
    }
    result=storage->acquiredValues[index-storage->acquiredStart];
}
Uint64 GetGpuFrequency(Uint32 thread) {
    // WebGPU timestamps use nanoseconds. Browser precision quantization is
    // retained; values are actual GPU timestamps, never CPU clock substitutes.
    return thread == 0 ? 1000000000ull : 0;
}
}
