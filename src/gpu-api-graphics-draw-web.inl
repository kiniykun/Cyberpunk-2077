// [nomadperf] static-binding uniform admission (see bindUniform). ?ovfAdmit=0 = old.
static std::atomic<unsigned long long> ovfAdmitted{0};
static bool OvfAdmitOn() {
    static std::atomic<int> state{-1};
    int v = state.load(std::memory_order_acquire);
    if (v < 0) {
        v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('ovfAdmit') === '1' ? 1 : 0; });
        state.store(v, std::memory_order_release);
        std::fprintf(stderr, "CP2077 nomadperf switches: ovfAdmit=%d\n", v);
    }
    return v != 0;
}
// At most 256 admissions per 250 ms window across all threads (bounds the synchronous creations during streaming).
static bool OvfAdmitToken() {
    static std::atomic<long long> window{0}; static std::atomic<unsigned> used{0};
    const long long now = static_cast<long long>(emscripten_get_now() / 250.0);
    long long w = window.load(std::memory_order_relaxed);
    if (w != now && window.compare_exchange_strong(w, now)) {
        const unsigned n = used.exchange(0);
        static std::atomic<unsigned> reports{0};
        if (n && (reports.fetch_add(1) < 8 || (now % 240) == 0))
            std::fprintf(stderr, "CP2077 nomadperf ovfAdmit probe: windowAdmissions=%u total=%llu\n", n, ovfAdmitted.load());
    }
    return used.fetch_add(1, std::memory_order_relaxed) < 256;
}
// Included inside cp2077::web, sharing the original command-state registry.
// Interface reflection replaces per-error shader allowlists. The caller still
// requires a genuine D32F, single-sample/layer/mip 2D native source before any
// copy. Integer/stencil, depth comparison, array, color and storage views keep
// their original paths. Values and shader calculations are unchanged.
#include "gpu-float-texture-interfaces.inc"
#include "gpu-draw-probe-web.inl" // [nailfix] opt-in ?drawprobe=1
// [drawmemo] 2026-10-24 city profile: this scanned ~4500 rows with two strcmp per texture binding per draw (~7 % of
// the draw path). The converted-shader records are static, so (record, binding, group) memoizes the answer per thread.
static bool NeedsDepthFloatCompatibilityViewScan(const ConvertedGraphicsShader& source, Uint32 binding, Uint32 group);
static bool NeedsDepthFloatCompatibilityView(const ConvertedGraphicsShader& source, Uint32 binding, Uint32 group) {
    static thread_local std::unordered_map<std::uint64_t, bool> memo;
    const std::uint64_t key = (std::uint64_t(reinterpret_cast<std::uintptr_t>(&source)) << 16) ^ (std::uint64_t(group) << 12) ^ binding;
    const auto found = memo.find(key);
    if (found != memo.end()) return found->second;
    const bool v = NeedsDepthFloatCompatibilityViewScan(source, binding, group);
    if (memo.size() < 65536) memo.emplace(key, v);
    return v;
}
static bool NeedsDepthFloatCompatibilityViewScan(const ConvertedGraphicsShader& source, Uint32 binding, Uint32 group) {
    for (const auto& row : kFloat2DTextureInterfaces)
        if (row.stage==source.stage && row.group==group && row.binding==binding &&
            std::strcmp(source.originalHash,row.hash)==0 &&
            std::strcmp(source.wgslSHA256,row.wgsl)==0) return true;
    return false;
}
// [skperf] round 37: per-thread totals of graphics-draw recording time / count (probeMask bit 2), read and reset by
// the engine's geometry batcher around one RenderGeometry call (cp2077_skperf_tl_draw_take).
static thread_local double tl_cpDrawMs = 0, tl_cpDrawStage[6] = {}; static thread_local unsigned tl_cpDraws = 0;
static thread_local double tl_cpApiMs[8] = {}; static thread_local unsigned tl_cpApiCalls[8] = {};
extern "C" void cp2077_skperf_tl_api_note(int category, double ms) { // [skperf] round 39 (skperf-api-timer-web.h)
    if (category < 0 || category > 7) return;
    tl_cpApiMs[category] += ms; ++tl_cpApiCalls[category];
}
// out[0] draw ms, out[1] draws, out[2..7] draw stages, out[8..15] api ms per category, out[16..23] api calls.
extern "C" void cp2077_skperf_tl_draw_take(double* out) {
    out[0] = tl_cpDrawMs; out[1] = tl_cpDraws; for (unsigned i = 0; i < 6; ++i) out[2 + i] = tl_cpDrawStage[i];
    for (unsigned i = 0; i < 8; ++i) { out[8 + i] = tl_cpApiMs[i]; out[16 + i] = tl_cpApiCalls[i]; tl_cpApiMs[i] = 0; tl_cpApiCalls[i] = 0; }
    tl_cpDrawMs = 0; tl_cpDraws = 0; for (auto& v : tl_cpDrawStage) v = 0;
}
// [skperf] round 63 viewMemo (opt-in ?viewMemo=1): see CommandEntry::viewInfoMemo.
static bool CpViewMemoOn() {
    static const int v = [] {
        const int r = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('viewMemo') === '1' ? 1 : 0; } catch (e) { return 0; } });
        std::fprintf(stderr, "CP2077 skperf switches: viewMemo=%d\n", r); return r; }();
    return v != 0;
}
// [skperf] round 57: running port+api ms of this thread WITHOUT reset (batcher engine split subtracts it per segment).
extern "C" double cp2077_skperf_tl_port_peek() {
    double v = tl_cpDrawMs; for (unsigned i = 0; i < 8; ++i) v += tl_cpApiMs[i]; return v;
}
struct CpDrawStageCost {
    const char* vertex="unknown";const char* pixel="unknown";
    const bool on=cpEngineTrace::ProbesEnabled(); // perf12: per-draw timers only with ?probes=1
    // [skperf] round 34 xordraw probe: with ?perfprobe=1 (probeMask bit 2) the stage timers also run, and a slow
    // logic-XOR draw (>= 2 ms) prints its breakdown (lock wait, scratch acquire, state, pipeline, resources, ...).
    const bool cpSkperf=(cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed)&2)!=0;
    bool cpXor=false; double cpScratchMs=0;
    double last=(on||cpSkperf)?emscripten_get_now():0, stages[6]{};
    // commandMutex wait at draw entry (part of the state stage): separates lock convoys from real state work.
    double lockWait=0;
    void markLock(){if(on||cpSkperf)lockWait=emscripten_get_now()-last;}
    unsigned completed=0;
    // Resources-stage breakdown by resource kind (0 uniform: CB/SamplerBias/DrawParameters, 1 texture: SRV/
    // indirection/storage texture, 2 sampler, 3 other), sampled on every 8th draw per thread to bound timer cost.
    double kinds[4]{};
    bool sampleKinds=false;
    double cpViewInfo=0; // [skperf] round 63: view-info lookup time (inside the texture kind), sampled draws
    void mark(unsigned stage){completed|=1u<<stage;if(!on&&!cpSkperf)return;const double now=emscripten_get_now();stages[stage]+=now-last;last=now;}
    static unsigned KindIndex(GraphicsResourceKind kind){
        switch(kind){
            case GraphicsResourceKind::ConstantBuffer: case GraphicsResourceKind::SamplerBias: case GraphicsResourceKind::DrawParameters: return 0;
            case GraphicsResourceKind::TextureSRV: case GraphicsResourceKind::TextureIndirection: case GraphicsResourceKind::StorageTextureUAV: return 1;
            case GraphicsResourceKind::Sampler: return 2;
            default: return 3;
        }
    }
    struct KindTimer {
        CpDrawStageCost& cost; unsigned kind; double start;
        KindTimer(CpDrawStageCost& c, GraphicsResourceKind k):cost(c),kind(KindIndex(k)),start(c.sampleKinds?emscripten_get_now():0){}
        ~KindTimer(){if(cost.sampleKinds)cost.kinds[kind]+=emscripten_get_now()-start;}
    };
    CpDrawStageCost(){static thread_local unsigned sampleTick=0;sampleKinds=(on||cpSkperf)&&(++sampleTick&7)==0;} // [skperf] round 63: also with perfprobe bit 2
    ~CpDrawStageCost(){
        double total=0;for(double value:stages)total+=value;
        if(cpSkperf){tl_cpDrawMs+=total;++tl_cpDraws;for(unsigned i=0;i<6;++i)tl_cpDrawStage[i]+=stages[i];} // [skperf] round 37
        if(cpSkperf && sampleKinds && (completed&4u)){ // [skperf] round 63 resources split, per thread every 2048 sampled draws
            static thread_local double rs[6]{}; static thread_local unsigned rn=0;
            rs[0]+=stages[2];for(unsigned i=0;i<4;++i)rs[1+i]+=kinds[i];rs[5]+=cpViewInfo;
            if(++rn>=2048){const double d=rn/1000.0;
                std::fprintf(stderr,"CP2077 skperf resources probe: sampledDraws=%u usPerDraw: resources=%.1f uniform=%.1f texture=%.1f sampler=%.1f other=%.1f (viewInfo=%.1f inside texture) viewMemo=%d\n",
                    rn,rs[0]/d,rs[1]/d,rs[2]/d,rs[3]/d,rs[4]/d,rs[5]/d,int(CpViewMemoOn()));
                for(auto& v:rs)v=0;rn=0;}
        }
        if(cpSkperf && cpXor && total>=2.0){ // [skperf] round 34
            static std::atomic<unsigned> xorSlow{0};const unsigned k=xorSlow.fetch_add(1)+1;
            if(k<=64 || !(k&(k-1)))std::fprintf(stderr,"CP2077 skperf xordraw probe: count=%u totalMs=%.2f lockWaitMs=%.2f scratchMs=%.2f stateMs=%.2f pipelineMs=%.2f resourcesMs=%.2f bindGroupMs=%.2f flushMs=%.2f encodeMs=%.2f PS=%s\n",
                k,total,lockWait,cpScratchMs,stages[0],stages[1],stages[2],stages[3],stages[4],stages[5],pixel);
        }
        if(!on)return; // the per-thread totals below stay ?probes=1 only
        static thread_local unsigned count=0,slow=0;
        const unsigned n=++count;
        // Per-thread stage totals every 1024 draws: shows where steady-state draw CPU time goes. Averages divide
        // by draws that reached the end of the resources stage; earlier skips/rejections (which cost almost
        // nothing) are counted separately so a change in the skip rate does not masquerade as a cost change.
        static thread_local double sums[7]{}, kindSums[4]{}, lockSum=0;
        static thread_local unsigned reached=0, sampled=0;
        for(unsigned i=0;i<6;++i)sums[i]+=stages[i];sums[6]+=total;lockSum+=lockWait;
        if(completed&4u){++reached;if(sampleKinds){++sampled;for(unsigned i=0;i<4;++i)kindSums[i]+=kinds[i];}}
        // perf12: every 1024 draws with ?probes=1, every 16384 otherwise (each stderr line is a synchronous main-thread
        // handoff from the render worker); the line and its counts stay for log greps.
        const unsigned window=on?1024u:16384u;
        if((n&(window-1))==0){
            const double d=reached?1000.0/reached:0, k=sampled?1000.0/sampled:0;
            std::fprintf(stderr,"CP2077 draw stage totals: draws=%u reached=%u early=%u per-draw-us state=%.1f pipeline=%.1f resources=%.1f bindGroup=%.1f flush=%.1f encode=%.1f total=%.1f resourcesByKind uniform=%.1f texture=%.1f sampler=%.1f other=%.1f lockWait=%.1f\n",
                n,reached,window-reached,sums[0]*d,sums[1]*d,sums[2]*d,sums[3]*d,sums[4]*d,sums[5]*d,sums[6]*d,
                kindSums[0]*k,kindSums[1]*k,kindSums[2]*k,kindSums[3]*k,lockSum*d);
            for(double& value:sums)value=0;
            for(double& value:kindSums)value=0;
            lockSum=0;
            reached=sampled=0;
        }
        if(n<=2 || (n<=4096 && (n&(n-1))==0) || (total>=100 && ++slow<=12))
            std::fprintf(stderr,"CP2077 draw stage cost probe: count=%u VS=%s PS=%s completed=%u stateMs=%.3f pipelineMs=%.3f resourcesMs=%.3f bindGroupMs=%.3f flushMs=%.3f encodeMs=%.3f totalMs=%.3f\n",
                n,vertex,pixel,completed,stages[0],stages[1],stages[2],stages[3],stages[4],stages[5],total);
    }
};
static const char* DrawShaderHash(const ConvertedGraphicsShader* shader) {
    return shader && shader->originalHash ? shader->originalHash : "-";
}
// Diagnostic dedup keys. Shader hash strings live in the static conversion tables, so their addresses
// identify a shader without hashing or copying the text.
static std::uint64_t DrawReportKey(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t h = 1469598103934665603ull;
    h ^= a; h *= 1099511628211ull; h ^= h >> 29;
    h ^= b; h *= 1099511628211ull; h ^= h >> 29;
    h ^= c; h *= 1099511628211ull; h ^= h >> 29;
    return h ? h : 1;
}
// Lock-free "first sighting" set shared by the draw diagnostics: 1024 open-addressed keys (0 = empty),
// at most 16 probes. When the table is full a key is simply not reported, like the old capped sets.
static bool FirstDrawReport(std::uint64_t key) {
    static auto* const slots = new std::atomic<std::uint64_t>[1024]();
    const std::uint64_t start = key ^ (key >> 32);
    for (unsigned i = 0; i < 16; ++i) {
        auto& slot = slots[(start + i) & 1023];
        std::uint64_t seen = slot.load(std::memory_order_relaxed);
        if (seen == key) return false;
        if (!seen) {
            if (slot.compare_exchange_strong(seen, key, std::memory_order_relaxed)) return true;
            if (seen == key) return false;
        }
    }
    return false;
}
// Exact-shader workaround/probe tests ran strcmp on every draw (and every texture resource). Converted shader
// records are static, so each (shader, expected hash, expected WGSL hash) answer is cached per thread.
// [skperf] round 18 texRetain: list-level sampler cache (see CommandEntry::samplerOnce). nullptr = not cacheable
// (switch off, null reference or a key clash): the caller uses the direct calls.
static CommandEntry::SamplerOnce* SamplerOnceFor(CommandEntry& command, const GpuApi::SamplerStateRef& reference, WGPUSampler native) {
    if (reference.isNull() || !Perf11Pools::TexRetainOn()) return nullptr;
    const std::uint64_t key = (std::uint64_t(reference.ID()) << 32) ^ std::uint64_t(reinterpret_cast<std::uintptr_t>(native));
    auto placed = command.samplerOnce.try_emplace(key);
    auto& once = placed.first->second;
    if (placed.second) { once.ref = reference.ID(); once.native = native; once.biasOk = GetEffectiveGpuSamplerBias(reference, native, once.bias); }
    else if (once.ref != reference.ID() || once.native != native) return nullptr;
    return &once;
}
static bool IsExactDrawShader(const ConvertedGraphicsShader* shader, const char* hash, const char* wgsl = nullptr) {
    if (!shader || !shader->originalHash) return false;
    struct Slot { const ConvertedGraphicsShader* shader; const char* hash; const char* wgsl; bool match; };
    static thread_local Slot cache[64]{};
    const std::uint64_t key = DrawReportKey(reinterpret_cast<std::uintptr_t>(shader), reinterpret_cast<std::uintptr_t>(hash),
                                            reinterpret_cast<std::uintptr_t>(wgsl));
    auto& slot = cache[(key ^ (key >> 32)) & 63];
    if (slot.shader == shader && slot.hash == hash && slot.wgsl == wgsl) return slot.match;
    const bool match = std::strcmp(shader->originalHash, hash) == 0 &&
        (!wgsl || GraphicsWgslHashMatches(shader, wgsl));
    slot = {shader, hash, wgsl, match};
    return match;
}
// Each distinct (site, VS, PS) skip is reported once, so skips in a later scene (the character preview)
// are not hidden behind startup ones and one site's count is never mistaken for another's.
// Lock-free: this runs for every skipped draw (tens of thousands per scene).
static void ReportSkippedDraw(const char* reason, int line, const char* vertexHash, const char* pixelHash) {
    static std::atomic<unsigned> skipped{0}, reported{0};
    const unsigned total = skipped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (reported.load(std::memory_order_relaxed) >= 256 ||
        !FirstDrawReport(DrawReportKey(std::uint64_t(unsigned(line)), reinterpret_cast<std::uintptr_t>(vertexHash),
                                       reinterpret_cast<std::uintptr_t>(pixelHash))) ||
        reported.fetch_add(1, std::memory_order_relaxed) >= 256) return;
    std::fprintf(stderr, "CP2077 draw skipped: %s line=%d VS=%s PS=%s total=%u\n", reason, line, vertexHash, pixelHash, total);
}
// [whitewall] multi-target dual-source split: 0 = normal draw, 1/2 = replay phases (see DrawBoundGpuGraphics below).
static thread_local int t_cpDualSplitPhase = 0;
static thread_local bool t_cpDualSplitRequested = false;
static bool DrawBoundGpuGraphicsImpl(ePrimitiveType primitive, bool indexed, Uint32 baseVertex,
                                 Uint32 firstIndex, Uint32 count, Uint32 instances, Uint32 firstInstance,
                                 Uint32 indirectCount = 0) {
    static_assert(sizeof(GpuApi::SIndirectDrawArgs) == 16 && offsetof(GpuApi::SIndirectDrawArgs, StartInstanceLocation) == 12);
    static_assert(sizeof(GpuApi::SIndirectIndexedDrawArgs) == 20 && offsetof(GpuApi::SIndirectIndexedDrawArgs, BaseVertexLocation) == 12 &&
                  offsetof(GpuApi::SIndirectIndexedDrawArgs, StartInstanceLocation) == 16);
    if (!indirectCount && (!count || !instances)) return true;
    Perf11Pools::UsTimer drawRecordTimer(Perf11Pools::frameDraws, Perf11Pools::frameDrawUs, true); // [skperf] frame probe
    const bool sampleResources=Perf11Pools::SampleDrawResources(); // [skperf] round 6 resource probe
    CpDrawStageCost drawCost;
    if (cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) || cpEngineTrace::ProbesEnabled()) cmdlockCounters[4].fetch_add(1, std::memory_order_relaxed); // [cmdlock] ([skperf] round 38: shared counter only while probing)
    CpBoundCommand lock(__builtin_FUNCTION(),__FILE__,__LINE__); // [cmdlock] lock-free when owner-cached
    auto current = lock.Find();
    drawCost.markLock();
    if (current == commands.end() || current->second.owner != std::this_thread::get_id()) return false;
    auto& command = current->second;
    if (command.failed) { NoteBoundDraw(command, BoundDrawOutcome::DroppedFailed); return false; }
    OwnedCommandLease commandLease(commandOperations,commandMutex);
    if(!commandLease)return false;
    // The owning thread exclusively mutates this list. Submission rejects
    // bound lists; teardown waits for the lease; unordered_map rehash keeps
    // this reference stable. Browser callbacks must not run under this lock.
    lock.UnlockForOwnedOperation();
    auto reject = [&](int failureLine) {
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1) < 16)
            std::fprintf(stderr, "CP2077 WebGPU: draw state rejection: line=%d command=%u type=%u viewport=%d OM=%u colors=%u depth=%u VS=%u PS=%u\n", failureLine, bound.ID(), unsigned(command.type), int(command.viewportSet), command.outputMergerRef.ID(), command.colorAttachmentCount, command.depthAttachment.reference.ID(), command.graphicsShaders[0].reference.ID(), command.graphicsShaders[1].reference.ID());
        NoteBoundDraw(command, BoundDrawOutcome::Rejected);
        // [gamerender] One unsupported draw no longer fails its whole command list (that dropped every other
        // pass recorded in it: ~8 clears and all later draws per failed list, i.e. black/flickering frames).
        // Nothing of this draw has been queued yet, so dropping only this draw leaves the list consistent.
        ReportSkippedDraw("rejected (list kept)", failureLine, DrawShaderHash(command.graphicsShaders[0].source),
            DrawShaderHash(command.graphicsShaders[1].source));
        return true;
    };
    // A draw with no shader program at all cannot render anything; skip it alone instead of failing every
    // other draw recorded in this list (e.g. the character preview pass).
    // Nothing is retained before pipeline creation, so these skips have nothing to release.
    auto skipDraw = [&](int skipLine, const char* reason) {
        ReportSkippedDraw(reason, skipLine, DrawShaderHash(command.graphicsShaders[0].source), DrawShaderHash(command.graphicsShaders[1].source));
        NoteBoundDraw(command, BoundDrawOutcome::Skipped);
        return true;
    };
    if (command.type == GpuApi::CommandListType::Default && command.graphicsShaders[0].reference.isNull() && !command.graphicsShaders[0].module)
        return skipDraw(__LINE__, "no vertex shader");
    if (command.type != GpuApi::CommandListType::Default || !command.viewportSet ||
        command.outputMergerRef.isNull() || (!command.colorAttachmentCount && command.depthAttachment.reference.isNull()))
        return reject(__LINE__);
    WGPUPrimitiveTopology topology;
    switch (primitive) {
    case PRIMTYPE_LineList: topology = WGPUPrimitiveTopology_LineList; break;
    case PRIMTYPE_LineStrip: topology = WGPUPrimitiveTopology_LineStrip; break;
    case PRIMTYPE_TriangleList: topology = WGPUPrimitiveTopology_TriangleList; break;
    case PRIMTYPE_TriangleStrip: topology = WGPUPrimitiveTopology_TriangleStrip; break;
    case PRIMTYPE_PointList: topology = WGPUPrimitiveTopology_PointList; break;
    default: return reject(__LINE__); // Quads/tessellation require real translation.
    }
    const auto& target = command.colorAttachmentCount ? command.colorAttachments[0] : command.depthAttachment;
    if (!target.width || !target.height || !target.samples) {
        // [nailfix] count-limited diagnostic: which attachment is empty (glass distortion-mask pass hits this)
        static std::atomic<unsigned> emptyTargetReports{0};
        if (emptyTargetReports.fetch_add(1) < 12)
            std::fprintf(stderr, "CP2077 draw rejected: empty target colors=%u c0=%ux%ux%u view0=%d c1=%ux%u c2=%ux%u depth=%ux%ux%u depthView=%d VS=%s PS=%s\n",
                unsigned(command.colorAttachmentCount), unsigned(command.colorAttachments[0].width), unsigned(command.colorAttachments[0].height),
                unsigned(command.colorAttachments[0].samples), int(command.colorAttachments[0].view != nullptr),
                unsigned(command.colorAttachments[1].width), unsigned(command.colorAttachments[1].height),
                unsigned(command.colorAttachments[2].width), unsigned(command.colorAttachments[2].height),
                unsigned(command.depthAttachment.width), unsigned(command.depthAttachment.height), unsigned(command.depthAttachment.samples),
                int(command.depthAttachment.view != nullptr), DrawShaderHash(command.graphicsShaders[0].source), DrawShaderHash(command.graphicsShaders[1].source));
        return reject(__LINE__);
    }
    GpuOMPipelineState om;
    if (!BuildGpuOMPipelineState(command.outputMergerState, command.depthClip,
        command.colorAttachmentCount, command.depthAttachment.format, target.samples, om)) return reject(__LINE__);
    if (command.depthReadOnly && om.hasDepthStencil) {
        ApplyGpuReadOnlyDepthStencilView(om.depthStencil);
    }
    // [whitewall] D3D dual-source blending with several render targets bound (glass in a 3-target pass): WebGPU only
    // allows Src1 factors with exactly one color target. For blend equations o0*S + D*F(o1) (op add, S without dst/src1,
    // F a src1/constant factor) the draw is replayed twice in the same pass by DrawBoundGpuGraphics: phase 1 writes
    // D*F(o1) (src factor zero), phase 2 adds o0*S (dst factor one). Targets 1..n are not written (D3D leaves other
    // RTs undefined with dual-source; the PS has no third output). ?dualsrc=0 keeps the old rejection.
    if (om.dualSourceMultiTarget) {
        if (!CpDualSourceSplitEnabled()) return reject(__LINE__);
        if (t_cpDualSplitPhase == 0) {
            GpuOMPipelineState probe = om;
            if (!CpDualSourceSplitBlend(probe, 1)) return reject(__LINE__);
            t_cpDualSplitRequested = true; // replayed as phases 1 and 2 by the wrapper
            return true;
        }
        if (!CpDualSourceSplitBlend(om, t_cpDualSplitPhase)) return reject(__LINE__);
    }
    if (command.graphicsShaders[0].unavailable || command.graphicsShaders[1].unavailable) {
        static std::atomic<unsigned> skipped{0};
        const unsigned n = ++skipped;
        if (n <= 4 || (n & (n - 1)) == 0) std::fprintf(stderr, "CP2077 draw skipped: unconverted original shader (count=%u)\n", n);
        NoteBoundDraw(command, BoundDrawOutcome::Skipped);
        return true;
    }
    const auto* vertex = command.graphicsShaders[0].source;
    const auto* pixel = command.graphicsShaders[1].source;
    const bool depthOnly = command.graphicsShaders[1].reference.isNull() &&
        !pixel && !command.graphicsShaders[1].module && !command.colorAttachmentCount &&
        !command.depthAttachment.reference.isNull();
    if (!vertex || !command.graphicsShaders[0].module ||
        (!depthOnly && (!pixel || !command.graphicsShaders[1].module))) return skipDraw(__LINE__, "no usable shader");
    Uint8 strides[GpuApi::VERTEX_LAYOUT_MAX_SLOTS]{};
    for (unsigned i = 0; i < GpuApi::VERTEX_LAYOUT_MAX_SLOTS; ++i) strides[i] = command.vertexBuffers[i].stride;
    // [cmdlock] Per-thread cache of built vertex layouts. A registered VertexLayoutRef id is never reused and its
    // descriptor never changes (gpu-api-vertex-web.cpp), converted shaders are static, so (layout id, shader,
    // bound strides) always builds the same result; only real (non-Null) layouts are cached because the Null-layout
    // fallback depends on registration state. Saves the per-draw allocations and layoutMutex. ?vlCache=0 = off.
    static const int vertexLayoutCache = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('vlCache') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 cmdlock switches: vlCache=%d\n", v); return v; }();
    struct VertexLayoutSlot { Uint32 layout = 0; const ConvertedGraphicsShader* shader = nullptr; std::uint64_t strides = 0;
        std::unique_ptr<GpuVertexBufferLayouts> built; };
    static thread_local VertexLayoutSlot vertexLayoutSlots[256];
    std::uint64_t packedStrides = 0;
    static_assert(GpuApi::VERTEX_LAYOUT_MAX_SLOTS == 8, "strides pack into 64 bits");
    for (unsigned i = 0; i < GpuApi::VERTEX_LAYOUT_MAX_SLOTS; ++i) packedStrides |= std::uint64_t(strides[i]) << (8 * i);
    GpuVertexBufferLayouts builtGeometry;
    const GpuVertexBufferLayouts* geometryPtr = &builtGeometry;
    VertexLayoutSlot* layoutSlot = nullptr;
    bool layoutOk = false;
    if (vertexLayoutCache && !command.vertexLayout.isNull()) {
        const std::uint64_t h = (std::uint64_t(command.vertexLayout.ID()) * 0x9E3779B97F4A7C15ull) ^
            (reinterpret_cast<std::uintptr_t>(vertex) >> 4) ^ (packedStrides * 0xC2B2AE3D27D4EB4Full);
        layoutSlot = &vertexLayoutSlots[(h ^ (h >> 29)) & 255];
        if (layoutSlot->built && layoutSlot->layout == command.vertexLayout.ID() && layoutSlot->shader == vertex &&
            layoutSlot->strides == packedStrides) { geometryPtr = layoutSlot->built.get(); layoutOk = true; }
    }
    if (!layoutOk) {
        layoutOk = BuildGpuVertexBufferLayouts(command.vertexLayout, *vertex, GpuApi::VERTEX_LAYOUT_MAX_SLOTS, strides, builtGeometry);
        if (layoutOk && layoutSlot) {
            layoutSlot->built = std::make_unique<GpuVertexBufferLayouts>(std::move(builtGeometry));
            layoutSlot->layout = command.vertexLayout.ID(); layoutSlot->shader = vertex; layoutSlot->strides = packedStrides;
            geometryPtr = layoutSlot->built.get();
        }
    }
    const GpuVertexBufferLayouts& geometry = *geometryPtr;
    if (!layoutOk) {
        // Layout/shader input mismatch affects only this draw; keep the rest of the list (UI, menu) alive.
        // Once per vertex shader, lock-free (tag 1<<40 keeps these keys apart from skip-site keys).
        static std::atomic<unsigned> layoutReports{0};
        if (FirstDrawReport(DrawReportKey(std::uint64_t(1) << 40, reinterpret_cast<std::uintptr_t>(vertex->originalHash), 0)) &&
            layoutReports.fetch_add(1, std::memory_order_relaxed) < 64)
            std::fprintf(stderr, "CP2077 draw skipped: vertex layout mismatch VS=%s layout=%u inputs=%u strides=%u,%u,%u,%u\n",
                vertex->originalHash ? vertex->originalHash : "?", command.vertexLayout.ID(), unsigned(vertex->vertexInputCount),
                unsigned(strides[0]), unsigned(strides[1]), unsigned(strides[2]), unsigned(strides[3]));
        return skipDraw(__LINE__, "vertex layout mismatch");
    }
    om.primitive.topology = topology;
    if (indexed && (topology == WGPUPrimitiveTopology_TriangleStrip || topology == WGPUPrimitiveTopology_LineStrip))
        om.primitive.stripIndexFormat = command.indexBuffer.indexFormat;
    GpuLogicXorScratch* xorScratch=nullptr;
    if(om.logicXor){
        const auto desc=GpuApi::GetTextureDesc(command.colorAttachments[0].reference);
        if(command.colorAttachmentCount!=1 || target.format!=WGPUTextureFormat_R8Uint || target.samples!=1 || desc.type!=GpuWrapApi::TEXTYPE_2D || desc.Internal_CalcTargetNumMips()!=1 || desc.CalcTargetSlices()!=1 || !FindConvertedGraphicsLogicXorVariant(pixel))return reject(__LINE__);
        auto& scratch=command.logicXorScratch[command.colorAttachments[0].view];
        drawCost.cpXor=true; const double cpScratchStart=drawCost.cpSkperf?emscripten_get_now():0; // [skperf] round 34
        if(!scratch.texture && !AcquireGpuLogicXorScratch(target.width,target.height,scratch))return reject(__LINE__);
        if(drawCost.cpSkperf)drawCost.cpScratchMs=emscripten_get_now()-cpScratchStart;
        xorScratch=&scratch;
    }
    WGPUColorTargetState targets[MAX_RENDERTARGETS];
    WGPUTextureView colors[MAX_RENDERTARGETS]{};
    // This exact original fragment only reads/writes its texture UAV and has
    // no color outputs. D3D leaves attached color targets unchanged; WebGPU
    // requires an explicit zero write mask for those unproduced locations.
    const bool originalPixelUavWithoutColors = pixel && pixel->stage == GraphicsShaderStage::Pixel &&
        IsExactDrawShader(pixel,"16347954073033142652","dafa0e9671832dc5e6124aecc9f2c38dee9e9b896496570b93465033510efa9d");
    // Exact DXIL output signature and converted WGSL both produce only SV_Target0.
    // Keep every additional attachment intact, matching the original unwritten MRTs.
    const bool originalEyeShadowOnlyTarget0 = pixel && pixel->stage == GraphicsShaderStage::Pixel &&
        (IsExactDrawShader(pixel,"12784076727638384549","dfc09da81a26e060926d5788aea2e25f3117698acae47776519a08ac25fdd382") ||
         IsExactDrawShader(pixel,"17262612989000979035","030b996f1082770f55ef1d463f5668e628b8ac693466a03d9c1ff224bcb7edf7"));
    for (unsigned i = 0; i < command.colorAttachmentCount; ++i) {
        targets[i] = WGPU_COLOR_TARGET_STATE_INIT;
        targets[i].format = command.colorAttachments[i].format;
        targets[i].blend = om.blendEnabled[i] ? &om.blends[i] : nullptr;
        targets[i].writeMask = (originalPixelUavWithoutColors || (originalEyeShadowOnlyTarget0 && i>0))
            ? WGPUColorWriteMask_None : om.writeMasks[i];
        colors[i] = command.colorAttachments[i].view;
    }
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = command.graphicsShaders[1].module; fragment.entryPoint = {"main", WGPU_STRLEN};
    bool giRgbVariant = false; // [giexplicit]
    if (FindConvertedGraphicsGiRGBOutputVariant(pixel) && command.colorAttachmentCount==1 &&
        targets[0].format==WGPUTextureFormat_RGBA16Float) {
        // This original GI shader exports RGB only; the original consumer
        // reads GlobalIllumination.rgb. Pad the browser output interface,
        // without inventing an alpha write or using it in blend/coverage math.
        if (om.blendEnabled[0] || om.alphaToCoverage) return reject(__LINE__);
        fragment.module=GetGpuGraphicsGiRGBOutputModule(command.graphicsShaders[1].reference);
        if(!fragment.module)return skipDraw(__LINE__, "gi rgb variant module unavailable");
        giRgbVariant = true;
        targets[0].writeMask=static_cast<WGPUColorWriteMask>(targets[0].writeMask &
            (WGPUColorWriteMask_Red|WGPUColorWriteMask_Green|WGPUColorWriteMask_Blue));
    }
    if (IsExactDrawShader(pixel,"10796363125667703835")) {
        const auto& input=command.graphicsTextures[1][0];
        if (!input.reference.isNull()) {
            const auto& desc=GpuApi::GetTextureDesc(input.reference);
            const auto format=desc.format;
            if ((format==GpuWrapApi::TEXFMT_D16U && (desc.HasUsage(GpuApi::TEXUSAGE_DepthStencil) ||
                    !desc.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess))) || format==GpuWrapApi::TEXFMT_D32F ||
                format==GpuWrapApi::TEXFMT_D24S8 || format==GpuWrapApi::TEXFMT_D32FS8) {
                fragment.module=GetGpuGraphicsDepthGatherModule(command.graphicsShaders[1].reference);
                // Without the gather variant this draw cannot bind its depth input; skip it alone rather than
                // failing the whole list (the character preview post-process list was dropped every frame).
                if (!fragment.module) return skipDraw(__LINE__, "depth gather variant module unavailable");
            }
        }
    }
    if(xorScratch){
        fragment.module=GetGpuGraphicsLogicXorModule(command.graphicsShaders[1].reference);if(!fragment.module)return skipDraw(__LINE__, "logic xor variant module unavailable");
        for(unsigned i=0;i<8;++i){targets[i]=WGPU_COLOR_TARGET_STATE_INIT;targets[i].format=WGPUTextureFormat_R8Unorm;
            {WGPUTextureFormat f;WGPUTextureViewDimension d; // [notier1] scratch layers are rgba8unorm without tier1
             if(cp2077::web::GetGpuTextureViewInfo(xorScratch->layers[i],f,d))targets[i].format=f;}
            om.blends[i]=WGPU_BLEND_STATE_INIT;om.blends[i].color.srcFactor=WGPUBlendFactor_OneMinusDst;om.blends[i].color.dstFactor=WGPUBlendFactor_OneMinusSrc;
            targets[i].blend=&om.blends[i];targets[i].writeMask=static_cast<WGPUColorWriteMask>(om.writeMasks[0]&WGPUColorWriteMask_Red);colors[i]=xorScratch->layers[i];}
    }
    // WebGPU rejects a pipeline whose color target has a non-zero writeMask but no @location output in the
    // fragment entry point; D3D leaves such targets unchanged. Zero the mask (keeping the format so the pass
    // stays compatible) for the plain converted module; variants manage their own outputs.
    // [nailfix] 1-sample alpha-to-coverage: emulate the D3D single coverage bit with a cutout variant (?a2c=0 = off).
    const bool nativeA2C = command.outputMergerState.m_blendModeDesc.m_alphaToCoverage;
    bool a2cVariant = false;
    // [whitewall] Dual-source blending (glass / vehicle paint / clear coat): same interface, two outputs at location 0.
    bool dualSourceVariant = false;
    if (om.dualSource && !depthOnly) {
        if (xorScratch || !pixel || command.colorAttachmentCount != 1 || fragment.module != command.graphicsShaders[1].module)
            return reject(__LINE__);
        WGPUShaderModule dual = CpDualSourceModule(pixel);
        if (!dual) return skipDraw(__LINE__, "dual-source variant module unavailable");
        fragment.module = dual; dualSourceVariant = true;
    }
    // [whitewall] Multi-target dual-source split, phase 1: SV_Target1 is routed to location 0 (SV_Target0 to location 1,
    // whose target has writeMask 0) so the multiply blend D*f(src1) can read it as the ordinary source.
    if (t_cpDualSplitPhase == 1 && !depthOnly) {
        if (xorScratch || !pixel || fragment.module != command.graphicsShaders[1].module) return reject(__LINE__);
        WGPUShaderModule swapped = CpDualSourceSwapModule(pixel);
        if (!swapped) return skipDraw(__LINE__, "dual-source split variant module unavailable");
        fragment.module = swapped; dualSourceVariant = true;
    }
    if (nativeA2C && target.samples == 1 && !xorScratch && !depthOnly && pixel && !dualSourceVariant &&
        fragment.module == command.graphicsShaders[1].module && CpAlphaToCoverageEmulation()) {
        if (WGPUShaderModule cutout = CpAlphaToCoverageModule(pixel)) { fragment.module = cutout; a2cVariant = true; }
    }
    if (!xorScratch && !depthOnly && (fragment.module == command.graphicsShaders[1].module || a2cVariant || dualSourceVariant)) {
        const std::uint32_t outputs = GraphicsFragmentOutputMask(pixel);
        for (unsigned i = 0; i < command.colorAttachmentCount; ++i)
            if (!(outputs & (1u << i))) { targets[i].writeMask = WGPUColorWriteMask_None; targets[i].blend = nullptr; }
    }
    fragment.targetCount = xorScratch?8:command.colorAttachmentCount; fragment.targets = targets;
    WGPURenderPipelineDescriptor descriptor = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.vertex.module = command.graphicsShaders[0].module; descriptor.vertex.entryPoint = {"main", WGPU_STRLEN};
    if (CpSpeedTreeWindOff() && vertex->wgsl && vertex->wgslBytes) { // [nailfix] diagnostic ?stWind=0
        if (WGPUShaderModule noWind = CpSpeedTreeNoWindModule(vertex)) descriptor.vertex.module = noWind;
    }
    descriptor.vertex.bufferCount = geometry.buffers.size(); descriptor.vertex.buffers = geometry.buffers.data();
    descriptor.fragment = depthOnly ? nullptr : &fragment; descriptor.primitive = om.primitive;
    descriptor.depthStencil = om.hasDepthStencil ? &om.depthStencil : nullptr;
    descriptor.multisample.count = target.samples; descriptor.multisample.alphaToCoverageEnabled = om.alphaToCoverage;
    const char* pixelHash = pixel ? pixel->originalHash : "native-depth-only";
    // Vertex hash only: a per-draw "VS / PS" std::string cost a heap allocation on every draw.
    descriptor.label={vertex->originalHash,WGPU_STRLEN};
    CpDrawProbe(command, vertex, depthOnly ? nullptr : pixel, om, target.samples, count, instances, nativeA2C, a2cVariant); // [nailfix]
    drawCost.vertex=vertex->originalHash;drawCost.pixel=pixelHash;
    // Explicit layout with dynamic uniform offsets (stable bind-group cache keys) for the plain shader pair only;
    // XOR, GI-RGB and depth-gather variants replace the fragment module and keep the auto layout.
    cp2077::web::GpuGraphicsPipelineLayout dynamicLayout;
    // [autoreason] why a draw uses the auto layout: 1 xor, 2 fragment variant (GI-RGB / depth gather), 3 layout acquire failed, 4 explicit pipeline failed (retry)
    // [giexplicit] The GI-RGB-output variant only pads the fragment output; its resource bindings are those of the plain
    // pixel shader, so it can use the pair explicit dynamic-offset layout (cacheable bind groups; it caused ~99% of all
    // bind-group creations in the Badlands, one new group per draw). Not when the pixel side was storage-converted
    // (ovfStorage has no GI variant of the converted module). A failure falls back to auto without rejecting the pair.
    // ?giExplicit=0 = old auto layout for the variant.
    static const int cpGiExplicit = [] { const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('giExplicit') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: giExplicit=%d\n", v); return v; }();
    // [varexplicit] dual-source / a2c cutout / GI-RGB variants share the pixel shader bindings: explicit layout too.
    const bool fragVariantNow = fragment.module != command.graphicsShaders[1].module;
    const bool giExplicit = cpGiExplicit && fragVariantNow && (giRgbVariant || dualSourceVariant || a2cVariant) && !xorScratch && !depthOnly;
    const ConvertedGraphicsShader* variantSource = !giExplicit ? nullptr : giRgbVariant ? FindConvertedGraphicsGiRGBOutputVariant(pixel) : CpVariantSource(fragment.module);
    WGPUShaderModule variantStorageModule = nullptr;
    // [autoreason] 2 GI-RGB, 5 alpha-to-coverage cutout, 6 dual-source, 7 other fragment variant (depth gather)
    unsigned autoReason = xorScratch ? 1u : !(depthOnly || fragment.module == command.graphicsShaders[1].module) ? (a2cVariant ? 5u : dualSourceVariant ? 6u : giRgbVariant ? 2u : 7u) : 0u;
    bool explicitLayout = !xorScratch && (depthOnly || fragment.module == command.graphicsShaders[1].module || giExplicit) &&
        cp2077::web::AcquireGpuGraphicsPipelineLayout(vertex, depthOnly ? nullptr : pixel, dynamicLayout) &&
        !(giExplicit && dynamicLayout.fragmentModule &&
          !(variantStorageModule = variantSource ? cp2077::web::AcquireGpuFragmentStorageVariant(vertex, pixel, variantSource) : nullptr));
    if (explicitLayout && giExplicit && fragment.module != command.graphicsShaders[1].module) autoReason = 0;
    if (!explicitLayout && !autoReason) autoReason = 3;
    descriptor.layout = explicitLayout ? dynamicLayout.layout : nullptr;
    const WGPUShaderModule plainVertexModule = descriptor.vertex.module, plainFragmentModule = fragment.module;
    const bool giExplicitUsed = explicitLayout && giExplicit && fragment.module != command.graphicsShaders[1].module;
    if (explicitLayout) { // [nomadperf] ovfStorage converted modules
        if (dynamicLayout.vertexModule) descriptor.vertex.module = dynamicLayout.vertexModule;
        if (!depthOnly && dynamicLayout.fragmentModule)
            fragment.module = giExplicitUsed ? variantStorageModule : dynamicLayout.fragmentModule;
    }
    drawCost.mark(0);
    // [hitch] Non-blocking miss: while a new pipeline compiles on the browser thread only this draw is skipped
    // (no worker wait, no auto-layout retry, no skip report). ?asyncPso=0 = old blocking creation.
    bool pipelinePending = false;
    auto pipeline = cp2077::web::CreateGpuRenderPipelineOrPending(descriptor, pipelinePending);
    if (pipelinePending) { NoteBoundDraw(command, BoundDrawOutcome::Skipped); return true; }
    if (!pipeline && explicitLayout) {
        // A reflection the explicit layout does not cover: retry with auto layout; reject the pair if that works.
        explicitLayout = false; descriptor.layout = nullptr;
        autoReason = 4;
        descriptor.vertex.module = plainVertexModule; fragment.module = plainFragmentModule;
        pipeline = cp2077::web::CreateGpuRenderPipelineOrPending(descriptor, pipelinePending);
        if (pipelinePending) { NoteBoundDraw(command, BoundDrawOutcome::Skipped); return true; }
        if (pipeline && !giExplicitUsed) cp2077::web::RejectGpuGraphicsPipelineLayout(vertex, depthOnly ? nullptr : pixel);
    }
    drawCost.mark(1);
    // Pipeline validation (format/blend/layout) fails for this shader pair only; keep the rest of the list.
    if (!pipeline) return skipDraw(__LINE__, "render pipeline creation failed");
    // Arena slots used by this draw; their bytes are written by the same main-thread call that encodes it.
    struct ArenaSlot { std::size_t block; std::uint64_t offset; std::size_t size; };
    // Per-thread scratch reused across draws instead of allocating these vectors on every draw. Nothing here
    // escapes the draw: contents are copied into the command list / queued draw, never moved. A nested call on
    // the same thread (none is known) gets a private heap copy, so two live draws never share scratch.
    struct DrawScratch {
        std::vector<WGPUBuffer> uploads;
        std::vector<GpuApi::BufferRef> retainedBuffers;
        std::vector<GpuApi::TextureRef> retainedTextures;
        std::vector<WGPUTextureView> indirectViews;
        std::vector<GpuApi::SamplerStateRef> retainedSamplers;
        std::vector<GpuApi::ShaderRef> retainedShaders;
        std::vector<WGPUBindGroupEntry> entries;
        std::vector<ArenaSlot> arenaSlots;
        std::vector<CommandEntry::AtomicTextureTransfer*> atomicTransfers;
        std::vector<GraphicsDrawBuffer> buffers;
        std::vector<cp2077::web::GraphicsUniformWrite> uniformWrites;
        std::vector<unsigned char> mappedSnapshot, packed;
        bool inUse = false;
    };
    static thread_local DrawScratch threadScratch;
    std::unique_ptr<DrawScratch> nestedScratch;
    DrawScratch* scratchState = &threadScratch;
    if (threadScratch.inUse) { nestedScratch = std::make_unique<DrawScratch>(); scratchState = nestedScratch.get(); }
    struct ScratchLease { DrawScratch& scratch; ~ScratchLease() { scratch.inUse = false; } } scratchLease{*scratchState};
    auto& scratch = *scratchState;
    scratch.inUse = true;
    scratch.uploads.clear(); scratch.retainedBuffers.clear(); scratch.retainedTextures.clear();
    scratch.indirectViews.clear(); scratch.retainedSamplers.clear(); scratch.retainedShaders.clear();
    scratch.entries.clear(); scratch.arenaSlots.clear(); scratch.atomicTransfers.clear();
    scratch.buffers.clear(); scratch.uniformWrites.clear();
    auto& uploads = scratch.uploads;
    auto& retainedBuffers = scratch.retainedBuffers;
    auto& retainedTextures = scratch.retainedTextures;
    auto& indirectViews = scratch.indirectViews;
    auto& retainedSamplers = scratch.retainedSamplers;
    auto& retainedShaders = scratch.retainedShaders;
    WGPUBindGroup group = nullptr;
    auto cleanup = [&]() {
        if (group) ReleaseGpuBindGroup(group);
        ReleaseGpuRenderPipeline(pipeline);
        for (auto buffer : uploads) DestroyGpuBuffer(buffer);
        for (auto ref : retainedBuffers) GpuApi::Release(ref);
        for (auto ref : retainedTextures) GpuApi::Release(ref);
        for (auto view : indirectViews) ReleaseGpuTextureView(view);
        for (auto ref : retainedSamplers) GpuApi::Release(ref);
        for (auto ref : retainedShaders) GpuApi::Release(ref);
    };
    bool drawQueueStarted = false; // set once this draw's own ops start entering the list (keep failing then)
    auto fail = [&](int failureLine) {
        if (drawQueueStarted) {
            cleanup(); NoteBoundDraw(command, BoundDrawOutcome::Rejected);
            MarkBrowserCommandFailed(command, "DrawBoundGpuGraphics", failureLine); return false;
        }
        // Uploads may already be copy targets in this list's encoder (see skip): retire them with the command.
        command.uploads.insert(command.uploads.end(), uploads.begin(), uploads.end());
        uploads.clear();
        cleanup(); return reject(failureLine);
    };
    // A draw whose per-draw inputs are missing (no constant data, unbound texture, retired target) is skipped by
    // itself. Failing the command list instead dropped every other draw recorded in it (the character preview).
    auto skip = [&](int skipLine) {
        // Uploads may already be the target of copies recorded in this command's encoder (repacked GPU
        // constants): destroying them now would invalidate the whole queue submission. Retire them with the
        // command instead, after GPU completion.
        command.uploads.insert(command.uploads.end(), uploads.begin(), uploads.end());
        uploads.clear();
        cleanup();
        ReportSkippedDraw("missing input", skipLine, DrawShaderHash(vertex), DrawShaderHash(pixel));
        NoteBoundDraw(command, BoundDrawOutcome::Skipped);
        return true;
    };
    // [skperf] round 42 drawRetain (Perf11Pools::CpDrawRetainOn): first use in this list retains once for the list.
    const bool cpDrawRetain = Perf11Pools::CpDrawRetainOn();
    auto retainBuffer = [&](const GpuApi::BufferRef& ref) {
        GpuApi::BufferDesc desc; WGPUBuffer buffer;
        if (cpDrawRetain && !ref.isNull()) {
            if (command.bufferRetainOnce.count(ref.ID())) return true;
            if (!AcquireGpuBuffer(ref, desc, buffer)) return false;
            command.retainedBuffers.push_back(ref); command.bufferRetainOnce.insert(ref.ID()); return true;
        }
        if (ref.isNull() || !AcquireGpuBuffer(ref, desc, buffer)) return false;
        retainedBuffers.push_back(ref); return true;
    };
    auto retainTexture = [&](const GpuApi::TextureRef& ref) {
        if (cpDrawRetain && !ref.isNull()) {
            if (command.textureRetainOnce.count(ref.ID())) return true;
            const GpuApi::TextureDesc* desc = RetainGpuTextureDesc(ref);
            if (!desc) return false;
            command.retainedTextures.push_back(ref); command.textureRetainOnce.emplace(ref.ID(), desc); return true;
        }
        if (ref.isNull() || !RetainGpuTexture(ref)) return false;
        retainedTextures.push_back(ref); return true;
    };
    auto& entries = scratch.entries;
    bool writesStorage=false;
    // Per-draw uniforms: exact cache hits keep their stable buffer (and stable bind groups); payloads seen before
    // are admitted to the immutable cache as before; first-seen payloads are suballocated from this command's
    // uniform arena instead of creating, validating and later destroying a dedicated buffer.
    auto& arenaSlots = scratch.arenaSlots;
    // Dynamic-offset bindings of the explicit layout go straight to the uniform ring: the ring buffer handle is
    // the same for every draw, so the bind-group key no longer depends on the payload. An immutable-cache hit
    // would put a payload-specific buffer into the key, and the lookup itself copied the payload into a
    // std::string and took a global mutex per constant buffer.
    auto isDynamicBinding=[&](std::uint32_t binding){
        if (!explicitLayout) return false;
        for (std::uint32_t k = 0; k < dynamicLayout.dynamicCount; ++k) if (dynamicLayout.dynamicBindings[k] == binding) return true;
        return false;
    };
    // [skperf] round 45 uniMemo (Perf11Pools::CpUniMemoOn, ?uniMemo=0 old): an immutable-cache hit is looked up with a
    // reused per-thread key (no heap string, no Allocation) and remembered for this list; the list owns that one lease
    // (command.uploads, released at retirement), so later draws of the list with the same payload bind the same
    // immutable buffer without the cache mutex or another lease. Same bytes reach the GPU either way.
    const bool cpUniMemo = Perf11Pools::CpUniMemoOn();
    auto bindUniform=[&](WGPUBindGroupEntry& entry,const void* bytes,std::size_t size,bool allowArena=true)->bool{
        entry.offset=0;entry.size=size;
        if(!(allowArena&&isDynamicBinding(entry.binding))){
            if(cpUniMemo){
                static thread_local std::string memoKey;
                BuildGpuImmutableUniformKey(memoKey,size,bytes);
                if(!memoKey.empty()){
                    const auto memo=command.uniformMemo.find(memoKey);
                    if(memo!=command.uniformMemo.end()){entry.buffer=memo->second;return true;}
                    if((entry.buffer=FindGpuImmutableUniformBufferKey(memoKey,size))){
                        command.uploads.push_back(entry.buffer);command.uniformMemo.emplace(memoKey,entry.buffer);return true;
                    }
                }
            } else if((entry.buffer=FindGpuImmutableUniformBuffer(size,bytes))){uploads.push_back(entry.buffer);return true;}
        }
        // Misses always use the arena: admitting repeated payloads created cached buffers through a synchronous
        // validated allocation (~4.5 ms per menu frame in a profile), costing more than the bind groups it saved.
        // [nomadperf] Exception: a STATIC binding (auto layout, or a uniform beyond the device's 10 dynamic slots in a
        // partially dynamic explicit layout) bound from the arena carries a per-list offset, so its bind group is
        // refused by the cache (uniformOffset) and rebuilt on the main thread for every draw (~250 creations per
        // frame outside in the Nomad area, ~12 ms of main-thread time). Payloads seen 3+ times (ShouldCacheGpuUniform)
        // now get an immutable buffer once (async-validated, priority allocation queue); later draws hit on the worker
        // with a stable buffer at offset 0, so the group is cacheable. Rate-limited. Opt-in while verified: ?ovfAdmit=1.
        if(allowArena && !isDynamicBinding(entry.binding) && size<=4096 && OvfAdmitOn() && ShouldCacheGpuUniform(size,bytes) && OvfAdmitToken()){
            if((entry.buffer=CreateGpuImmutableUniformBuffer(size,bytes))){entry.offset=0;uploads.push_back(entry.buffer);ovfAdmitted.fetch_add(1,std::memory_order_relaxed);return true;}
        }
        if(allowArena&&AllocateCommandUniform(command,bytes,size,entry.buffer,entry.offset)){
            arenaSlots.push_back({command.uniformBlocks.size()-1,entry.offset,(size+255)&~std::size_t(255)});return true;
        }
        entry.offset=0;
        if(!(entry.buffer=CreateGpuImmutableUniformBuffer(size,bytes)))return false;
        uploads.push_back(entry.buffer);return true;
    };
    auto& atomicTransfers = scratch.atomicTransfers;
    auto atomicTexture = [&](Uint32 slot)->CommandEntry::AtomicTextureTransfer* {
        if(slot>=GpuApi::MAX_UAV_BINDINGS)return nullptr;
        const auto& native=command.pixelStorageTextures[slot];
        if(native.reference.isNull() || !native.view)return nullptr;
        // Native SetupRenderTargets acquires the entire UAV array at mip 0.
        // Preserve that selection rather than silently copying a different view.
        WGPUTexture texture=nullptr;
        if(!AcquireGpuTextureForCopy(native.reference,texture))return nullptr;
        retainedTextures.push_back(native.reference);
        const auto desc=GpuApi::GetTextureDesc(native.reference);
        if(desc.type!=GpuWrapApi::TEXTYPE_ARRAY || desc.format!=GpuWrapApi::TEXFMT_R32_Uint ||
           Uint32(desc.sampleCount)!=0 || !desc.width || !desc.height || desc.CalcTargetSlices()<4)return nullptr;
        auto& transfer=command.atomicTextureTransfers[native.reference.ID()];
        if(!transfer.buffer.words){
            // perf11: pooled across lists (Perf11Pools); the import copy overwrites it before each draw.
            transfer.generation=cp2077::web::GpuDeviceGeneration();
            if(!Perf11Pools::AcquireAtomic(desc.width,desc.height,desc.CalcTargetSlices(),transfer.buffer))return nullptr;
            transfer.source=texture;
        }
        if(transfer.source!=texture || transfer.buffer.width!=desc.width || transfer.buffer.height!=desc.height ||
           transfer.buffer.layers!=desc.CalcTargetSlices())return nullptr;
        if(std::find(atomicTransfers.begin(),atomicTransfers.end(),&transfer)==atomicTransfers.end())atomicTransfers.push_back(&transfer);
        return &transfer;
    };
    static std::atomic<unsigned> shadowResourceReports{0};
    const bool traceShadowResources=IsExactDrawShader(pixel,"18333707234177685286") &&
        shadowResourceReports.fetch_add(1)<4;
    // [textures] Emulated t130 bindless (multilayer pools): fetched once per draw at the first pool resource.
    WGPUTextureView multilayerViews[cp2077::web::kGpuMultilayerPoolSlots]{};
    WGPUBuffer multilayerTable = nullptr;
    bool multilayerReady = false;
    // [skperf] round 7: duplicate-binding check. A bit per examined binding (< 4096) gives an exact "not seen"
    // answer in O(1); only a set bit falls back to the old scan of entries, so the result is identical. Multilayer
    // draws carry ~40 pool entries, which made the old per-resource scan quadratic.
    std::uint64_t seenBindings[64] = {};
    for (unsigned stage = 0; stage < 2; ++stage) {
        if (stage == 1 && depthOnly) continue;
        WGPUShaderModule module; const ConvertedGraphicsShader* source;
        const auto& shader = command.graphicsShaders[stage];
        // [skperf] round 18 texRetain also covers the two shader acquisitions per draw (global shader mutex): the
        // first draw of a list that uses a shader retains it once for the whole list (released at retirement).
        if (Perf11Pools::TexRetainOn()) {
            const std::uint64_t shaderKey = (std::uint64_t(shader.reference.ID()) << 1) | stage;
            const auto once = command.shaderRetainOnce.find(shaderKey);
            if (once != command.shaderRetainOnce.end()) {
                module = once->second.first; source = static_cast<const ConvertedGraphicsShader*>(once->second.second);
            } else {
                if (!AcquireGpuGraphicsShader(shader.reference, stage ? GraphicsShaderStage::Pixel : GraphicsShaderStage::Vertex,
                                               module, source)) return fail(__LINE__);
                command.retainedShaders.push_back(shader.reference);
                command.shaderRetainOnce.emplace(shaderKey, std::make_pair(module, static_cast<const void*>(source)));
            }
        } else {
        if (!AcquireGpuGraphicsShader(shader.reference, stage ? GraphicsShaderStage::Pixel : GraphicsShaderStage::Vertex,
                                       module, source)) return fail(__LINE__);
        retainedShaders.push_back(shader.reference);
        }
        for (std::size_t i = 0; i < source->resourceCount; ++i) {
            const auto& resource = source->resources[i];
            CpDrawStageCost::KindTimer kindTimer(drawCost, resource.kind); // sampled draws only
            Perf11Pools::KindUsTimer kindUsTimer(sampleResources, unsigned(resource.kind)); // [skperf] round 6
            if (resource.group != 0) return fail(__LINE__);
            {
                const bool fast = resource.binding < 4096;
                const std::uint64_t bit = fast ? (std::uint64_t(1) << (resource.binding & 63)) : 0;
                if (!fast || (seenBindings[resource.binding >> 6] & bit))
                    for (const auto& existing : entries) if (existing.binding == resource.binding) return fail(__LINE__);
                if (fast) seenBindings[resource.binding >> 6] |= bit;
            }
            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = resource.binding;
            if(traceShadowResources)std::fprintf(stderr,"CP2077 graphics resource probe: phase=begin command=%u shader=%s stage=%u binding=%u kind=%u\n",
                bound.ID(),source->originalHash,stage,resource.binding,unsigned(resource.kind));
            if(resource.kind==GraphicsResourceKind::StorageTextureAtomicBuffer || resource.kind==GraphicsResourceKind::TextureAtomicDimensions){
                writesStorage=true;
                if(stage!=1)return fail(__LINE__);
                auto* transfer=atomicTexture(resource.slot);if(!transfer)return fail(__LINE__);
                const bool dimensions=resource.kind==GraphicsResourceKind::TextureAtomicDimensions;
                entry.buffer=dimensions ? transfer->buffer.dimensions : transfer->buffer.words;
                entry.offset=0;entry.size=dimensions ? 16 : transfer->buffer.bytes;
            } else if (resource.kind == GraphicsResourceKind::SamplerBias) {
                static_assert(GpuApi::MAX_SAMPLER_STATES==16,"WGSL bias array must match native slots");
                Float values[GpuApi::MAX_SAMPLER_STATES][4]{};
                // [skperf] round 45 biasMemo (?biasMemo=0 old): the 16 values only depend on the bound (ref, native)
                // pairs through the list-constant samplerOnce cache; same pairs as the last SamplerBias of this stage
                // in this list -> same values, without the 16 lookups. Stored only when every value came from samplerOnce.
                auto& biasMemo=command.biasMemo[stage];
                bool biasFromMemo=false;
                if (Perf11Pools::CpBiasMemoOn() && Perf11Pools::TexRetainOn() && biasMemo.valid) {
                    biasFromMemo=true;
                    for (std::size_t slot=0;slot<GpuApi::MAX_SAMPLER_STATES;++slot)
                        if (command.graphicsSamplers[stage][slot].reference.ID()!=biasMemo.refs[slot] ||
                            command.graphicsSamplers[stage][slot].sampler!=biasMemo.natives[slot]) { biasFromMemo=false; break; }
                    if (biasFromMemo) for (std::size_t slot=0;slot<GpuApi::MAX_SAMPLER_STATES;++slot) values[slot][0]=biasMemo.bias[slot];
                }
                bool allOnce=true;
                for (std::size_t slot=0;!biasFromMemo && slot<GpuApi::MAX_SAMPLER_STATES;++slot) {
                    const auto reference=command.graphicsSamplers[stage][slot].reference;
                    // A stale/unknown sampler binds the default sampler (bias 0) in the Sampler branch; match it here.
                    if (reference.isNull()) continue;
                    if (auto* once=SamplerOnceFor(command,reference,command.graphicsSamplers[stage][slot].sampler)) { // [skperf] texRetain
                        values[slot][0]=once->biasOk ? once->bias : 0;
                        continue;
                    }
                    allOnce=false;
                    if (!GetEffectiveGpuSamplerBias(reference,command.graphicsSamplers[stage][slot].sampler,values[slot][0]))
                        values[slot][0]=0;
                }
                if (!biasFromMemo && allOnce && Perf11Pools::CpBiasMemoOn() && Perf11Pools::TexRetainOn()) {
                    for (std::size_t slot=0;slot<GpuApi::MAX_SAMPLER_STATES;++slot) {
                        biasMemo.refs[slot]=command.graphicsSamplers[stage][slot].reference.ID();
                        biasMemo.natives[slot]=command.graphicsSamplers[stage][slot].sampler;
                        biasMemo.bias[slot]=values[slot][0];
                    }
                    biasMemo.valid=true;
                }
                if (!bindUniform(entry,values,sizeof(values))) return fail(__LINE__);
            } else if (resource.kind == GraphicsResourceKind::DrawParameters) {
                // Original DXIL-derived BaseVertex/BaseInstance/DrawIndex
                // arithmetic remains intact; supply actual direct-draw values.
                // Indirect argument values require GPU-derived uniform data.
                if (indirectCount) {
                    // [gamerender] The record's start vertex/instance live in the GPU argument buffer (possibly
                    // GPU-written earlier in this list). Give the draw its own arena slot (never a shared immutable
                    // cache buffer) and copy the two words into it in list order before the draw:
                    // indexed {.., BaseVertexLocation@12, StartInstanceLocation@16}, non-indexed
                    // {.., StartVertexLocation@8, StartInstanceLocation@12} -> {BaseVertex, BaseInstance}; DrawIndex 0.
                    // Several records would need a slot each per record: skip that draw only (logged).
                    if (indirectCount != 1 || !command.indirectArguments.buffer) return skip(__LINE__);
                    const Uint32 zero[4]{0, 0, 0, 0};
                    entry.offset = 0; entry.size = sizeof(zero);
                    if (!AllocateCommandUniform(command, zero, sizeof(zero), entry.buffer, entry.offset)) return skip(__LINE__);
                    arenaSlots.push_back({command.uniformBlocks.size()-1, entry.offset, 256});
                    const std::uint64_t argumentOffset = std::uint64_t(command.indirectOffset) + (indexed ? 12 : 8);
                    if (argumentOffset + 8 > command.indirectArguments.bytes ||
                        !QueuePendingBufferCopy(command, command.indirectArguments.buffer, argumentOffset, entry.buffer, entry.offset, 8))
                        return skip(__LINE__);
                    static std::atomic<unsigned> indirectParameterReports{0};
                    if (indirectParameterReports.fetch_add(1) < 4)
                        std::fprintf(stderr, "CP2077 indirect draw parameters from GPU args: indexed=%u argsOffset=%llu\n",
                            unsigned(indexed), static_cast<unsigned long long>(argumentOffset));
                    entries.push_back(entry);
                    continue;
                }
                const Uint32 values[4]{baseVertex, firstInstance, 0, 0};
                if (!bindUniform(entry,values,sizeof(values))) return fail(__LINE__);
            } else if (resource.kind == GraphicsResourceKind::ConstantBuffer) {
                const auto* layout = FindUniformLayout(source->originalHash, resource.group, resource.binding);
                if (!layout || resource.slot >= GpuApi::MAX_CONSTANT_BUFFERS) return skip(__LINE__);
                const void* data = nullptr; std::size_t bytes = 0;
                auto& mappedSnapshot = scratch.mappedSnapshot; // only read after a successful assign below
                const auto ref = command.graphicsConstantBuffers[stage][resource.slot];
                if (UsesNativeGpuConstantBacking(ref)) {
                    // Suballocated from this list's uniform pages with queued field copies (no per-draw buffer
                    // creation or copy handoff); the ring buffer is never an upload of this draw.
                    std::uint64_t repackOffset=0;
                    if(!RepackNativeGpuConstantToUniform(command,*layout,ref,entry.buffer,repackOffset))return fail(__LINE__);
                    Perf11Pools::NoteCbPath(0); // [skperf] round 11
                    entry.offset=repackOffset;entry.size=layout->webBytes;
                    entries.push_back(entry);
                    if(traceShadowResources)std::fprintf(stderr,"CP2077 graphics resource probe: phase=returned command=%u shader=%s stage=%u binding=%u\n",
                        bound.ID(),source->originalHash,stage,resource.binding);
                    continue;
                }
                auto snapshot = command.constantSnapshots.find(ref.ID());
                // [flicker3] Dynamic_Legacy constants this list has not written itself are read as of this list's
                // submission (ResolveOrderedConstants), not as of this bind on this worker. One arena slot per
                // (buffer, layout) per list: every such draw of the list resolves to the same value. XOR draws write
                // their slots at record time, so they keep the old snapshot. ?cbOrder=0 = old.
                if (!xorScratch && !ref.isNull() && CbOrderOn() && cp2077::web::IsOrderedGpuConstant(ref) &&
                    !(snapshot != command.constantSnapshots.end() && (snapshot->second.locked || snapshot->second.own))) {
                    const std::pair<Uint32, const void*> key{ref.ID(), layout};
                    const auto reused = command.deferredConstantSlots.find(key);
                    bool placed = false;
                    if (reused != command.deferredConstantSlots.end()) {
                        entry.buffer = reused->second.first; entry.offset = reused->second.second; entry.size = layout->webBytes;
                        if (cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) || cpEngineTrace::ProbesEnabled()) cbOrderReused.fetch_add(1, std::memory_order_relaxed); // [skperf] round 46: diagnostic only
                        placed = true; Perf11Pools::NoteCbPath(1);
                    } else {
                        static thread_local std::vector<unsigned char> zeros;
                        if (zeros.size() < layout->webBytes) zeros.assign(layout->webBytes, 0);
                        entry.offset = 0; entry.size = layout->webBytes;
                        if (AllocateCommandUniform(command, zeros.data(), layout->webBytes, entry.buffer, entry.offset)) {
                            const auto blockIndex = command.uniformBlocks.size() - 1;
                            command.deferredConstants.push_back({std::uint32_t(blockIndex), entry.offset - command.uniformBlocks[blockIndex].base, layout, ref.ID()});
                            command.deferredConstantSlots.emplace(key, std::make_pair(entry.buffer, entry.offset));
                            if (cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) || cpEngineTrace::ProbesEnabled()) cbOrderDeferred.fetch_add(1, std::memory_order_relaxed); // [skperf] round 46: diagnostic only
                            placed = true; Perf11Pools::NoteCbPath(2);
                        }
                    }
                    if (placed) { entries.push_back(entry); continue; }
                    entry.offset = 0; entry.size = 0; entry.buffer = nullptr; // arena unavailable: old bind-time snapshot below
                }
                if (!ref.isNull() && snapshot != command.constantSnapshots.end() && !snapshot->second.locked) {
                    data = snapshot->second.bytes.data(); bytes = snapshot->second.bytes.size(); Perf11Pools::NoteCbPath(3);
                } else if (!ref.isNull() && cp2077::web::CopyMappedGpuConstantBuffer(ref, mappedSnapshot)) {
                    data = mappedSnapshot.data(); bytes = mappedSnapshot.size(); Perf11Pools::NoteCbPath(4);
                } else if (resource.slot == 6 && !command.graphicsNativeConstants[stage].empty()) {
                    data = command.graphicsNativeConstants[stage].data(); bytes = command.graphicsNativeConstants[stage].size();
                } else if (ref.isNull() && ((!stage && (resource.slot == 5 || resource.slot == 3)) ||
                                            (stage && resource.slot == 3))) {
                    const auto first = stage ? PSC_Custom_First : resource.slot == 5 ? VSC_Frequent_First : VSC_Custom_First;
                    const auto last = stage ? PSC_Custom_Last : resource.slot == 5 ? VSC_Frequent_Last : VSC_Custom_Last;
                    for (unsigned r = first; r <= last; ++r)
                        if ((!resource.legacyReadMaskVerified || (resource.legacyReadMask & (std::uint64_t(1) << (r-first)))) &&
                            (stage ? !command.pixelLegacyInitialized[r] : !command.vertexLegacyInitialized[r])) {
                            static std::atomic<unsigned> reports{0};
                            if(reports.fetch_add(1)<8) std::fprintf(stderr,"CP2077 original constant rejection: hash=%s stage=%u slot=%u binding=%u register=%u first=%u mask=%llu verified=%u nativeRawBytes=%llu\n",
                                source->originalHash,stage,resource.slot,resource.binding,r,unsigned(first),static_cast<unsigned long long>(resource.legacyReadMask),unsigned(resource.legacyReadMaskVerified),static_cast<unsigned long long>(command.graphicsNativeConstants[stage].size()));
                            return skip(__LINE__);
                        }
                    data = stage ? command.pixelLegacyConstants.data() + first * 4 : command.vertexLegacyConstants.data() + first * 4;
                    bytes = (last-first+1)*16; Perf11Pools::NoteCbPath(5);
                }
                // Original DX12 SFrameResources initializes table CBV slots
                // with m_nullCBV; its zero-size view reads zero when unbound.
                // Only a genuinely null reference with no native data selects
                // this path. Stale/unwritten real buffers still fail below.
                if (!data && ref.isNull() && resource.slot!=3 && resource.slot!=5 &&
                    (resource.slot!=6 || command.graphicsNativeConstants[stage].empty())) {
                    entry.buffer=CreateGpuImmutableUniformBuffer(layout->webBytes);
                    if(!entry.buffer)return fail(__LINE__);
                    Perf11Pools::NoteCbPath(6);
                    uploads.push_back(entry.buffer);entry.size=layout->webBytes;
                    entries.push_back(entry);continue;
                }
                if (!data || bytes < layout->nativeMinimumBytes) {
                    static std::atomic<unsigned> reports{0};
                    if(reports.fetch_add(1)<8) std::fprintf(stderr,"CP2077 original uniform data rejection: hash=%s stage=%u slot=%u binding=%u reference=%u hasData=%u actualBytes=%llu requiredNativeBytes=%u webBytes=%u rawBytes=%llu\n",
                        source->originalHash,stage,resource.slot,resource.binding,ref.ID(),unsigned(data!=nullptr),static_cast<unsigned long long>(bytes),layout->nativeMinimumBytes,layout->webBytes,static_cast<unsigned long long>(command.graphicsNativeConstants[stage].size()));
                    static std::atomic<unsigned> descReports{0};
                    if(!ref.isNull() && descReports.fetch_add(1)<8){
                        const auto& d=GpuApi::GetBufferDesc(ref);
                        std::fprintf(stderr,"CP2077 rejected constant buffer desc: ref=%u usage=%u category=%u group=%u size=%u snapshot=%u\n",
                            ref.ID(),unsigned(d.usage),unsigned(d.category),unsigned(d.group),unsigned(d.size),
                            unsigned(snapshot!=command.constantSnapshots.end()));
                    }
                    return skip(__LINE__);
                }
                // Same repack as RepackGpuUniformBytes, reusing the layout found above instead of scanning the
                // ~1350-row layout tables a second time per constant buffer. RepackUniform zeroes padding itself.
                // [cmdlock] Per-thread cache of repacked constant buffers: (layout, source pointer, size) -> the raw bytes it
                // was packed from + the packed result. A hit needs the raw bytes to be identical (memcmp), so any CPU
                // write to the source is seen; RepackUniform is a pure function of (layout, raw). ?cbCache=0 = off.
                static const int cbCache = [] {
                    const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('cbCache') === '0' ? 0 : 1; });
                    std::fprintf(stderr, "CP2077 cmdlock switches: cbCache=%d\n", v); return v; }();
                struct PackedSlot { const void* layout = nullptr; const void* source = nullptr; std::vector<unsigned char> raw, packed; unsigned gen = 0; };
                static thread_local PackedSlot packedSlots[128];
                PackedSlot* packedSlot = nullptr;
                bool packedHit = false;
                if (cbCache && bytes <= 16384) {
                    const std::uintptr_t h = (reinterpret_cast<std::uintptr_t>(layout) >> 3) * 0x9E3779B1u ^ (reinterpret_cast<std::uintptr_t>(data) >> 4);
                    packedSlot = &packedSlots[(h ^ (h >> 13)) & 127];
                    packedHit = packedSlot->layout == layout && packedSlot->source == data && packedSlot->raw.size() == bytes &&
                        packedSlot->packed.size() == layout->webBytes && std::memcmp(packedSlot->raw.data(), data, bytes) == 0;
                }
                // [skperf] round 11 cbReuse: same pack slot at the same generation = byte-identical packed data
                // (memcmp above), same layout / binding / dynamic-ness -> reuse this list's previous uniform location.
                // Not for XOR draws (they write their arena slots explicitly at record time).
                auto& memo = command.cbMemo[stage][resource.slot];
                const bool dynamicBinding = isDynamicBinding(entry.binding);
                const bool reuseAllowed = packedSlot && !xorScratch && Perf11Pools::CbReuseOn();
                auto remember = [&]() {
                    if (!reuseAllowed || !entry.buffer) { memo = {}; return; }
                    memo.packSlot = packedSlot; memo.packGen = packedSlot->gen; memo.layout = layout; memo.binding = entry.binding;
                    memo.dynamic = dynamicBinding; memo.buffer = entry.buffer; memo.offset = entry.offset; memo.size = entry.size;
                };
                if (packedHit) {
                    Perf11Pools::NoteCbPath(7);
                    if (reuseAllowed && memo.buffer && memo.packSlot == packedSlot && memo.packGen == packedSlot->gen &&
                        memo.layout == layout && memo.binding == entry.binding && memo.dynamic == dynamicBinding &&
                        memo.size == packedSlot->packed.size()) {
                        entry.buffer = memo.buffer; entry.offset = memo.offset; entry.size = memo.size;
                        Perf11Pools::NoteCbPath(9);
                    } else {
                        if (!bindUniform(entry, packedSlot->packed.data(), packedSlot->packed.size())) return fail(__LINE__);
                        Perf11Pools::NoteCbPath(10); remember();
                    }
                } else {
                    Perf11Pools::NoteCbPath(8);
                    auto& packed = scratch.packed;
                    packed.resize(layout->webBytes);
                    if (!RepackUniform(*layout, data, bytes, packed.data(), packed.size())) return skip(__LINE__);
                    if (packedSlot) {
                        packedSlot->layout = layout; packedSlot->source = data;
                        packedSlot->raw.assign(static_cast<const unsigned char*>(data), static_cast<const unsigned char*>(data) + bytes);
                        packedSlot->packed = packed;
                        ++packedSlot->gen; // [skperf] round 11: invalidates every cbReuse memo of the old content
                    }
                    if (!bindUniform(entry, packed.data(), packed.size())) return fail(__LINE__);
                    Perf11Pools::NoteCbPath(10); remember();
                }
            } else if (resource.kind == GraphicsResourceKind::StorageBuffer) {
                if (resource.slot >= GpuApi::MAX_SRV_BINDINGS || !resource.storageStride) return skip(__LINE__);
                const auto& buffer=command.graphicsBuffers[stage][resource.slot];
                // storageStride 4 = converted u32 word view: any engine struct stride whose size is a word multiple
                // reads correctly (same relaxation as compute dispatch).
                if(!buffer.buffer || (buffer.stride!=resource.storageStride && resource.storageStride!=4) || buffer.bytes<resource.storageStride ||
                    buffer.bytes%resource.storageStride || !retainBuffer(buffer.reference)) {
                    static std::atomic<unsigned> storageReports{0};
                    if(storageReports.fetch_add(1)<16)std::fprintf(stderr,"CP2077 storage buffer binding skipped: hash=%s stage=%u slot=%u binding=%u ref=%u bound=%u stride=%u expectedStride=%u bytes=%llu\n",
                        source->originalHash,stage,resource.slot,resource.binding,buffer.reference.ID(),unsigned(buffer.buffer!=nullptr),
                        unsigned(buffer.stride),unsigned(resource.storageStride),static_cast<unsigned long long>(buffer.bytes));
                    return skip(__LINE__);
                }
                entry.buffer=buffer.buffer;entry.offset=0;entry.size=buffer.bytes;
            } else if (resource.kind == GraphicsResourceKind::StorageTextureUAV) {
                writesStorage=true;
                if (stage != 1 || resource.slot >= GpuApi::MAX_UAV_BINDINGS) return fail(__LINE__);
                const auto& texture=command.pixelStorageTextures[resource.slot];
                if (!texture.view) return fail(__LINE__);
                // [nailfix] A storage view whose texel format differs from the WGSL texture_storage format is an
                // asynchronous bind-group error that invalidates the whole command buffer; skip only this draw (the
                // compute path has the same guard). Glass/water write-only r8uint UAVs depend on it.
                {
                    const auto declared=FindWgslStorageTextureFormat(source->wgsl,source->wgslBytes,resource.binding);
                    WGPUTextureFormat boundFormat=WGPUTextureFormat_Undefined;
                    if (declared!=WGPUTextureFormat_Undefined && GetGpuStorageTextureFormat(texture.reference,boundFormat) &&
                        boundFormat!=declared) {
                        static std::atomic<unsigned> reports{0};
                        if (reports.fetch_add(1)<16) std::fprintf(stderr,"CP2077 draw skipped: pixel storage format mismatch PS=%s binding=%u slot=%u bound=%u declared=%u\n",
                            source->originalHash?source->originalHash:"?",unsigned(resource.binding),unsigned(resource.slot),unsigned(boundFormat),unsigned(declared));
                        return skip(__LINE__);
                    }
                }
                if (!retainTexture(texture.reference)) return fail(__LINE__);
                entry.textureView=texture.view;
            } else if (resource.kind == GraphicsResourceKind::TextureSRV) {
                if (resource.slot >= GpuApi::MAX_SRV_BINDINGS) return fail(__LINE__);
                const auto& texture = command.graphicsTextures[stage][resource.slot];
                // D3D reads zero from a null, unbound or retired SRV: bind a zero view of the declared WGSL
                // dimension/sample type instead of skipping the draw. A native null holds the 2D float
                // INTERTEX_Zero view, which only fits texture_2d<f32>. Variant fragment modules (depth gather,
                // GI RGB, logic xor) may declare other types than source->wgsl, so they keep the skip.
                // [nomadfix] The [nomadperf] ovfStorage module (explicit layout, uniforms converted to storage) keeps the
                // original texture interface too; counting it as a variant disabled every null/zero-view fallback below
                // for those shaders (eye PS binding 595 -> invalid bind group -> whole command buffer dropped -> black).
                const bool ovfStorageModule=stage==1 && explicitLayout && dynamicLayout.fragmentModule &&
                    fragment.module==dynamicLayout.fragmentModule;
                const bool variantModule=stage==1 && fragment.module!=command.graphicsShaders[1].module && !a2cVariant && !dualSourceVariant && !ovfStorageModule; // [nailfix] a2c variant keeps the original interface ([whitewall] so does dual-source)
                const char* zeroReason=texture.view ? nullptr : "unbound";
                GpuWgslBinding declared;
                if (!zeroReason && texture.nativeNull && !variantModule) {
                    declared=FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding);
                    if (declared.kind!=GpuWgslBinding_Unknown &&
                        (declared.kind!=GpuWgslBinding_Float || declared.dimension!=WGPUTextureViewDimension_2D)) zeroReason="native-null";
                }
                const GpuApi::TextureDesc* srvDesc=nullptr; // [skperf] round 18 texRetain
                if (!zeroReason) {
                    if (Perf11Pools::TexRetainOn()) {
                        const auto once=command.textureRetainOnce.find(texture.reference.ID());
                        if (once!=command.textureRetainOnce.end()) srvDesc=once->second;
                        else if ((srvDesc=RetainGpuTextureDesc(texture.reference))) {
                            command.retainedTextures.push_back(texture.reference);
                            command.textureRetainOnce.emplace(texture.reference.ID(),srvDesc);
                        } else zeroReason="retain-failed";
                    } else if (!retainTexture(texture.reference)) zeroReason="retain-failed";
                }
                if (zeroReason) {
                    if (variantModule) return skip(__LINE__);
                    if (declared.kind==GpuWgslBinding_Unknown) declared=FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding);
                    // Borrowed device-lifetime view: never retained or released by this draw.
                    entry.textureView=GetGpuDefaultTextureView(declared);
                    if (!entry.textureView) return skip(__LINE__);
                    static std::atomic<unsigned> zeroReports{0};
                    const unsigned zeroCount=zeroReports.fetch_add(1)+1;
                    if (zeroCount<=16 || (zeroCount&(zeroCount-1))==0)
                        std::fprintf(stderr,"CP2077 texture SRV zero fallback: reason=%s hash=%s stage=%u binding=%u slot=%u ref=%u nativeNull=%u kind=%u dimension=%u count=%u\n",
                            zeroReason,source->originalHash,stage,resource.binding,resource.slot,texture.reference.ID(),
                            unsigned(texture.nativeNull),unsigned(declared.kind),unsigned(declared.dimension),zeroCount);
                    entries.push_back(entry);
                    continue;
                }
                entry.textureView = texture.view;
                // [whitewall] r5 diagnostic ?texdebug=1 (default off): pixel-stage material slots (< 27) bound to a tiny
                // texture (<= 4x4, one slice) read the zero view instead, so surfaces that only have tiny placeholder
                // textures turn black. A white facade turning black = it renders from placeholder textures.
                if (stage==1 && resource.slot<27 && CpTinyTextureDebug() && !variantModule) {
                    const auto desc=GpuApi::GetTextureDesc(texture.reference);
                    if (desc.width<=4 && desc.height<=4 && desc.depthOrSliceNum<=1) {
                        const GpuWgslBinding declaredTiny=FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding);
                        if (declaredTiny.kind==GpuWgslBinding_Float && declaredTiny.dimension==WGPUTextureViewDimension_2D)
                            if (WGPUTextureView zero=GetGpuDefaultTextureView(declaredTiny)) entry.textureView=zero;
                    }
                }
                const bool exactEyeGradient=stage==1 &&
                    IsExactDrawShader(source,"17262612989000979035","030b996f1082770f55ef1d463f5668e628b8ac693466a03d9c1ff224bcb7edf7");
                const bool exactEyeShadow=stage==1 &&
                    IsExactDrawShader(source,"12784076727638384549","dfc09da81a26e060926d5788aea2e25f3117698acae47776519a08ac25fdd382");
                if ((exactEyeGradient && (resource.binding==595 || resource.binding==599 || resource.binding==626)) ||
                    (exactEyeShadow && (resource.binding==594 || resource.binding==597))) {
                    static std::atomic<unsigned> reports{0};
                    if(reports.fetch_add(1)<30)std::fprintf(stderr,
                        "CP2077 eye array binding probe: hash=%s binding=%u slot=%u ref=%u nativeNull=%u\n",
                        source->originalHash,resource.binding,resource.slot,texture.reference.ID(),unsigned(texture.nativeNull));
                }
                // These exact array SRVs use samples/loads only, with no size queries.
                // Null references read all-zero channels. Integer RGBA32Uint keeps
                // every channel zero; real texture references are never changed.
                if (texture.nativeNull && exactEyeGradient &&
                    ((resource.binding==595 && resource.slot==19) ||
                     (resource.binding==599 && resource.slot==23) ||
                     (resource.binding==626 && resource.slot==50))) {
                    const bool integer=resource.binding==626;
                    auto& array=integer ? command.nativeNullEyeArrayUintView : command.nativeNullEyeArrayFloatView;
                    const auto format=integer ? WGPUTextureFormat_RGBA32Uint : WGPUTextureFormat_RGBA8Unorm;
                    if(!array.texture){
                        WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                        desc.dimension=WGPUTextureDimension_2D;desc.format=format;desc.size={1,1,1};
                        desc.mipLevelCount=1;desc.sampleCount=1;desc.usage=WGPUTextureUsage_TextureBinding;
                        array.texture=CreateGpuTexture(desc);if(!array.texture)return fail(__LINE__);
                        WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                        viewDesc.dimension=WGPUTextureViewDimension_2DArray;viewDesc.format=format;
                        array.view=CreateGpuTextureView(array.texture,viewDesc);if(!array.view)return fail(__LINE__);
                    }
                    if(!array.view)return fail(__LINE__);
                    entry.textureView=array.view;
                }
                // Exact original eye-shadow t17 only samples this 3D resource
                // with explicit LOD, without dimension queries or writes. A native
                // null SRV reads zero. Preserve that value using a dimension-correct
                // WebGPU-zero-initialized volume; never replace a real resource.
                if (texture.nativeNull && stage==1 && resource.binding==593 && resource.slot==17 &&
                    IsExactDrawShader(source,"12784076727638384549","dfc09da81a26e060926d5788aea2e25f3117698acae47776519a08ac25fdd382")) {
                    auto& nullVolume=command.nativeNullEyeVolumeView;
                    if (!nullVolume.texture) {
                        WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                        desc.dimension=WGPUTextureDimension_3D;desc.format=WGPUTextureFormat_RGBA8Unorm;
                        desc.size={1,1,1};desc.mipLevelCount=1;desc.sampleCount=1;
                        desc.usage=WGPUTextureUsage_TextureBinding;
                        nullVolume.texture=CreateGpuTexture(desc);
                        if (!nullVolume.texture) return fail(__LINE__);
                        WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                        viewDesc.dimension=WGPUTextureViewDimension_3D;viewDesc.format=WGPUTextureFormat_RGBA8Unorm;
                        nullVolume.view=CreateGpuTextureView(nullVolume.texture,viewDesc);
                        if (!nullVolume.view) return fail(__LINE__);
                        std::fputs("CP2077 native null eye-volume binding: exact t17 zero 3D SRV created\n",stderr);
                    }
                    if (!nullVolume.view) return fail(__LINE__);
                    entry.textureView=nullVolume.view;
                }
                // Original DX12 BindTextures binds a null SRV for a null
                // history reference. The exact shader only textureLoads this
                // integer resource (no size/mip queries). D3D null reads are
                // all zero; an ordinary float-colour fallback is not a Uint
                // binding in WebGPU. Never apply this to a real game texture.
                if (texture.nativeNull && stage==1 && resource.binding==578 && resource.slot==2 &&
                    IsExactDrawShader(source,"6862283186064749778","1b8f4d93f3cd8e130249d929cbe54d41f9026327f7ca49eae4825ac4a05d518a")) {
                    auto& nullView=command.nativeNullIntegerView;
                    if (!nullView.texture) {
                        WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                        desc.dimension=WGPUTextureDimension_2D;desc.format=WGPUTextureFormat_RGBA32Uint;
                        desc.size={1,1,1};desc.mipLevelCount=1;desc.sampleCount=1;
                        desc.usage=WGPUTextureUsage_TextureBinding;
                        desc.label={"Original null integer history SRV",WGPU_STRLEN};
                        nullView.texture=CreateGpuTexture(desc);
                        if (!nullView.texture) return fail(__LINE__);
                        WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                        viewDesc.dimension=WGPUTextureViewDimension_2D;viewDesc.format=WGPUTextureFormat_RGBA32Uint;
                        nullView.view=CreateGpuTextureView(nullView.texture,viewDesc);
                        if (!nullView.view) return fail(__LINE__);
                    }
                    entry.textureView=nullView.view;
                }
                const auto& nativeDesc=srvDesc ? *srvDesc : GpuApi::GetTextureDesc(texture.reference); // [skperf] texRetain
                // The selected gather variant changes t0 from float to depth.
                // Its actual depth view must survive; the RGB-output variant
                // changes no resource types and uses the original interface.
                const bool selectedDepthGatherInput=stage==1 && resource.group==0 && resource.binding==576 &&
                    FindConvertedGraphicsDepthGatherVariant(source) &&
                    fragment.module==GetGpuGraphicsDepthGatherModule(command.graphicsShaders[1].reference);
                if (nativeDesc.format==GpuWrapApi::TEXFMT_D32F && !selectedDepthGatherInput &&
                    NeedsDepthFloatCompatibilityView(*source,resource.binding,resource.group)) {
                    if (nativeDesc.type!=GpuWrapApi::TEXTYPE_2D || nativeDesc.CalcTargetSlices()!=1 ||
                        nativeDesc.Internal_CalcTargetNumMips()!=1 || Uint32(nativeDesc.sampleCount)!=0) return fail(__LINE__);
                    // [skperf] shareConv: bind the shared copy (converted at submit, once per depth write) unless this list
                    // already wrote the depth; then the old in-list refresh before each read below.
                    bool sharedDepth=false;
                    if (SharedConv::Allowed()) {
                        if (SharedConv::ListWrote(command.id,texture.reference.ID())) { Perf11Pools::workShareFallback.fetch_add(1,std::memory_order_relaxed); ++command.fallbackReads; }
                        else {
                            const std::pair<Uint32,Uint32> key{texture.reference.ID(),0x7fffffffu};
                            auto conv=SharedConv::Find(key,nativeDesc.width,nativeDesc.height,1,WGPUTextureFormat_R32Float);
                            if (!conv) { conv=SharedConv::MakeDepth(nativeDesc.width,nativeDesc.height); if (!conv) return fail(__LINE__); SharedConv::Insert(key,conv); }
                            if (SharedConv::ReadAlready(command,conv)) Perf11Pools::workShareDedupe.fetch_add(1,std::memory_order_relaxed);
                            else {
                                auto read=std::make_shared<SharedConvRead>();
                                read->conv=conv; read->refId=texture.reference.ID();
                                wgpuTextureViewAddRef(texture.view); read->depthHold=std::make_shared<QueuedTextureViewHold>(texture.view);
                                command.sharedReads.push_back(std::move(read));
                            }
                            entry.textureView=conv->copy.view; sharedDepth=true;
                        }
                    }
                    // [skperf] listGen: no write to this depth in this list since its last per-list refresh -> reuse it.
                    if (!sharedDepth && SharedConv::ListGenOn()) {
                        const Uint32 depthRef=texture.reference.ID();
                        auto existing=command.depthFloatCompatibilityViews.find(depthRef);
                        auto at=command.depthCopyAt.find(depthRef);
                        if (existing!=command.depthFloatCompatibilityViews.end() && existing->second.view && at!=command.depthCopyAt.end()) {
                            const auto local=command.localWriteGen.find(depthRef);
                            const Uint32 localGen=local==command.localWriteGen.end()?0u:local->second;
                            if (at->second.first==SharedConv::ListGen(command.id,depthRef)+1 && at->second.second==localGen) {
                                entry.textureView=existing->second.view; sharedDepth=true;
                                Perf11Pools::workDepthListSkipped.fetch_add(1,std::memory_order_relaxed);
                            }
                        }
                    }
                    if (!sharedDepth) {
                    auto& compatible=command.depthFloatCompatibilityViews[texture.reference.ID()];
                    {
                        const Uint32 depthRef=texture.reference.ID();
                        const auto local=command.localWriteGen.find(depthRef);
                        command.depthCopyAt[depthRef]={SharedConv::ListGen(command.id,depthRef)+1, local==command.localWriteGen.end()?0u:local->second};
                    }
                    // perf11: reuse a pooled R32Float target of this size (refreshed by the copy below before every read).
                    if (!compatible.texture) Perf11Pools::AcquireCompat(0,nativeDesc.width,nativeDesc.height,compatible);
                    if (!compatible.texture) {
                        WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                        desc.dimension=WGPUTextureDimension_2D;desc.format=WGPUTextureFormat_R32Float;
                        desc.size={nativeDesc.width,nativeDesc.height,1};desc.mipLevelCount=1;desc.sampleCount=1;
                        desc.usage=WGPUTextureUsage_RenderAttachment|WGPUTextureUsage_TextureBinding;
                        compatible.texture=CreateGpuTexture(desc);
                        if (!compatible.texture) return fail(__LINE__);
                        WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                        viewDesc.dimension=WGPUTextureViewDimension_2D;viewDesc.format=WGPUTextureFormat_R32Float;
                        compatible.view=CreateGpuTextureView(compatible.texture,viewDesc);
                        if (!compatible.view) return fail(__LINE__);
                        compatible.poolKind=1;compatible.poolWidth=nativeDesc.width;compatible.poolHeight=nativeDesc.height;
                        compatible.poolGeneration=cp2077::web::GpuDeviceGeneration();
                        Perf11Pools::Report("depth-compat",Perf11Pools::compatCreated.fetch_add(1)+1,Perf11Pools::compatReused.load());
                    }
                    // Refresh at the precise command position before each read,
                    // including reads following writes in this same command list.
                    // Queued in encoder order (after every earlier queued draw/copy, before this draw) and encoded by
                    // the batched main-thread flush: it used to cost a flush plus a synchronous copy handoff per
                    // depth read, and broke the queued draw run in two. The queued work holds its own reference to
                    // the bound depth view (the slot may be rebound before the flush).
                    Perf11Pools::workDepthCopies.fetch_add(1,std::memory_order_relaxed);Perf11Pools::workDepthBytes.fetch_add(std::uint64_t(nativeDesc.width)*nativeDesc.height*8,std::memory_order_relaxed); // [skperf]
                    wgpuTextureViewAddRef(texture.view);
                    auto depthView=std::make_shared<QueuedTextureViewHold>(texture.view);
                    const WGPUTextureView compatibleView=compatible.view;
                    const std::uint32_t copyWidth=nativeDesc.width, copyHeight=nativeDesc.height;
                    if (!QueuePendingCustom(command,[depthView,compatibleView,copyWidth,copyHeight](WGPUCommandEncoder encoder){
                            const bool copied=EncodeGpuDepthFloatCopy(encoder,depthView->view,compatibleView,copyWidth,copyHeight);
                            depthView->Release();
                            return copied;
                        })) return fail(__LINE__);
                    entry.textureView=compatible.view;
                    } // [skperf] !sharedDepth
                }
                // [tankview] A D16U UAV texture without a depth-stencil usage has an R16Unorm backing (see
                // TranslateResource); it cannot bind to a texture_depth_* declaration. Binding it made the bind group
                // invalid, which invalidated the WHOLE command buffer at submit. Skip only this draw.
                // [npcblack] R16Unorm views are UnfilterableFloat-only in WebGPU (texture-formats-tier1 does not make them
                // filterable), but explicit graphics layouts declare float SRVs filterable: plain R16_Unorm SRVs (small
                // lookup maps, e.g. 256x256 in NPC skin draws) get the same R32Float copy as the D16U float alias.
                const bool r16D16Uav=nativeDesc.format==GpuWrapApi::TEXFMT_D16U && nativeDesc.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess) &&
                    !nativeDesc.HasUsage(GpuApi::TEXUSAGE_DepthStencil);
                const bool r16Plain=nativeDesc.format==GpuWrapApi::TEXFMT_R16_Unorm && nativeDesc.width<=1024 && nativeDesc.height<=1024 &&
                    (nativeDesc.type==GpuWrapApi::TEXTYPE_2D || nativeDesc.type==GpuWrapApi::TEXTYPE_ARRAY);
                const GpuWgslBinding r16Declared=(r16D16Uav || r16Plain) ? FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding) : GpuWgslBinding{};
                if ((r16D16Uav && r16Declared.kind==GpuWgslBinding_Depth) || r16Declared.kind==GpuWgslBinding_Float) {
                    // [npcblack] Skipping these draws left every NPC skin/garment gbuffer draw (character shadow
                    // comparison SRV t27 = depth binding + its plain-sampling float alias) out -> black silhouettes.
                    // Bind copies holding the same 16-bit values instead: Depth16Unorm for the depth declaration,
                    // R32Float (R16Float without float32-filterable) for the float alias. Converted once per command
                    // list at the first read (after this list's earlier writes). ?r16depth=0 = old skip (depth) /
                    // old direct binding (float).
                    static const int r16DepthCopy = [] {
                        const int v = MAIN_THREAD_EM_ASM_INT({ const q = new URLSearchParams(location.search).get('r16depth'); return (q === '0' || q === 'false') ? 0 : 1; });
                        std::fprintf(stderr, "CP2077 npcblack switches: r16depth=%d (D16U UAV -> Depth16Unorm/R32Float copies for depth/float SRVs)\n", v);
                        return v;
                    }();
                    const bool depthDecl=r16Declared.kind==GpuWgslBinding_Depth;
                    const Uint32 layers=nativeDesc.CalcTargetSlices();
                    const bool arrayDecl=r16Declared.dimension==WGPUTextureViewDimension_2DArray;
                    const bool shapeOk=arrayDecl || (!depthDecl && r16Declared.dimension==WGPUTextureViewDimension_2D && layers==1);
                    WGPUTexture r16Texture=nullptr;
                    if (r16DepthCopy && shapeOk) {
                        r16Texture=texture.view ? cp2077::web::GetGpuTextureViewParent(texture.view) : nullptr;
                        if (!r16Texture && AcquireGpuTextureForCopy(texture.reference,r16Texture))
                            retainedTextures.push_back(texture.reference); // no sampled view (UAV-only backing): retain the texture itself
                    }
                    // The D16U UAV backing may lack TextureBinding (no Samplable usage) but always has CopySrc: its
                    // layers are copied into a pooled sampleable R16Unorm scratch first, then drawn into the target.
                    const bool convertible=r16DepthCopy && r16Texture && shapeOk && layers>=1 && layers<=16 &&
                        Uint32(nativeDesc.sampleCount)==0;
                    if (!convertible && depthDecl) {
                        static std::atomic<unsigned> r16DepthReports{0};
                        const unsigned n=r16DepthReports.fetch_add(1)+1;
                        if (n<=16 || (n&(n-1))==0)
                            std::fprintf(stderr,"CP2077 tankview: draw skipped, R16Unorm-backed D16U bound to depth SRV: hash=%s stage=%u binding=%u texture=%u %ux%u count=%u (r16depth=%d view=%d dim=%u layers=%u samples=%u)\n",
                                source->originalHash,stage,resource.binding,texture.reference.ID(),nativeDesc.width,nativeDesc.height,n,
                                r16DepthCopy,int(r16Texture!=nullptr),unsigned(r16Declared.dimension),layers,unsigned(nativeDesc.sampleCount));
                        return skip(__LINE__);
                    }
                    if (convertible) {
                        const Uint32 width=nativeDesc.width, height=nativeDesc.height;
                        const WGPUTextureFormat targetFormat=depthDecl ? WGPUTextureFormat_Depth16Unorm :
                            (cp2077::web::SupportsGpuFeature(WGPUFeatureName_Float32Filterable) ? WGPUTextureFormat_R32Float : WGPUTextureFormat_R16Float);
                        const Uint32 poolKind=(depthDecl ? 100u : targetFormat==WGPUTextureFormat_R32Float ? 300u : 400u)+(arrayDecl ? 0u : 50u)+layers;
                        // [skperf] shareConv: bind the shared copy (converted at submit, once per source write) unless this
                        // list already wrote the source; then the old per-list conversion.
                        bool sharedBound=false;
                        if (SharedConv::Allowed()) {
                            if (SharedConv::ListWrote(command.id,texture.reference.ID())) { Perf11Pools::workShareFallback.fetch_add(1,std::memory_order_relaxed); ++command.fallbackReads; }
                            else {
                                const std::pair<Uint32,Uint32> key{texture.reference.ID(),poolKind+1};
                                auto conv=SharedConv::Find(key,width,height,layers,targetFormat);
                                if (!conv) { conv=SharedConv::MakeR16(poolKind,width,height,layers,targetFormat,arrayDecl); if (!conv) return fail(__LINE__); SharedConv::Insert(key,conv); }
                                if (SharedConv::ReadAlready(command,conv)) Perf11Pools::workShareDedupe.fetch_add(1,std::memory_order_relaxed);
                                else {
                                    auto read=std::make_shared<SharedConvRead>();
                                    read->conv=conv; read->refId=texture.reference.ID(); read->r16Source=r16Texture;
                                    command.sharedReads.push_back(std::move(read));
                                }
                                entry.textureView=conv->copy.view; sharedBound=true;
                            }
                        }
                        if (!sharedBound) {
                        auto& copy=command.r16DepthViews[texture.reference.ID()|(depthDecl ? 0u : 0x40000000u)];
                        if (!copy.texture) {
                            Perf11Pools::AcquireCompat(poolKind,width,height,copy);
                            if (!copy.texture) {
                                WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                                desc.dimension=WGPUTextureDimension_2D;desc.format=targetFormat;
                                desc.size={width,height,layers};desc.mipLevelCount=1;desc.sampleCount=1;
                                desc.usage=WGPUTextureUsage_RenderAttachment|WGPUTextureUsage_TextureBinding;
                                copy.texture=CreateGpuTexture(desc);
                                if (!copy.texture) return fail(__LINE__);
                                WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                                viewDesc.dimension=arrayDecl ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D;
                                viewDesc.format=targetFormat;viewDesc.arrayLayerCount=layers;
                                copy.view=CreateGpuTextureView(copy.texture,viewDesc);
                                if (!copy.view) return fail(__LINE__);
                                copy.poolKind=poolKind+1;copy.poolWidth=width;copy.poolHeight=height;
                                copy.poolGeneration=cp2077::web::GpuDeviceGeneration();
                                static std::atomic<unsigned> created{0};
                                const unsigned n=created.fetch_add(1)+1;
                                if (n<=4 || (n&(n-1))==0)
                                    std::fprintf(stderr,"CP2077 npcblack r16depth: %s copy created %ux%ux%u texture=%u hash=%s binding=%u (n=%u)\n",
                                        depthDecl ? "depth16" : "float", width,height,layers,texture.reference.ID(),source->originalHash,resource.binding,n);
                            }
                            // Sampleable R16Unorm scratch (pooled, kind 200+layers, keyed by the texture id with the top bit set).
                            auto& scratch=command.r16DepthViews[texture.reference.ID()|0x80000000u];
                            if (!scratch.texture) {
                                Perf11Pools::AcquireCompat(200+layers,width,height,scratch);
                                if (!scratch.texture) {
                                    WGPUTextureDescriptor desc=WGPU_TEXTURE_DESCRIPTOR_INIT;
                                    desc.dimension=WGPUTextureDimension_2D;desc.format=WGPUTextureFormat_R16Unorm;
                                    desc.size={width,height,layers};desc.mipLevelCount=1;desc.sampleCount=1;
                                    desc.usage=WGPUTextureUsage_CopyDst|WGPUTextureUsage_TextureBinding;
                                    scratch.texture=CreateGpuTexture(desc);
                                    if (!scratch.texture) return fail(__LINE__);
                                    WGPUTextureViewDescriptor viewDesc=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                                    viewDesc.dimension=WGPUTextureViewDimension_2DArray;viewDesc.format=WGPUTextureFormat_R16Unorm;
                                    viewDesc.arrayLayerCount=layers;
                                    scratch.view=CreateGpuTextureView(scratch.texture,viewDesc);
                                    if (!scratch.view) return fail(__LINE__);
                                    scratch.poolKind=200+layers+1;scratch.poolWidth=width;scratch.poolHeight=height;
                                    scratch.poolGeneration=cp2077::web::GpuDeviceGeneration();
                                }
                            }
                            // Per-layer views: R16Unorm scratch layer and target layer (released after encoding).
                            auto views=std::make_shared<std::vector<WGPUTextureView>>();
                            for (Uint32 layer=0; layer<layers; ++layer) {
                                WGPUTextureViewDescriptor src=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                                src.dimension=WGPUTextureViewDimension_2D;src.format=WGPUTextureFormat_R16Unorm;
                                src.baseArrayLayer=layer;src.arrayLayerCount=1;src.mipLevelCount=1;
                                WGPUTextureViewDescriptor dst=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
                                dst.dimension=WGPUTextureViewDimension_2D;dst.format=targetFormat;
                                dst.baseArrayLayer=layer;dst.arrayLayerCount=1;dst.mipLevelCount=1;
                                const auto srcView=CreateGpuTextureView(scratch.texture,src);
                                const auto dstView=CreateGpuTextureView(copy.texture,dst);
                                if (!srcView || !dstView) {
                                    if (srcView) ReleaseGpuTextureView(srcView);
                                    if (dstView) ReleaseGpuTextureView(dstView);
                                    for (auto v : *views) ReleaseGpuTextureView(v);
                                    return fail(__LINE__);
                                }
                                views->push_back(srcView);views->push_back(dstView);
                            }
                            // [skperf] work probe: copy (read+write 2 B/px) + conversion draw (read 2 B, write 2 or 4 B/px) per layer.
                            Perf11Pools::workR16Conversions.fetch_add(1,std::memory_order_relaxed);
                            Perf11Pools::workR16Bytes.fetch_add(std::uint64_t(width)*height*layers*(targetFormat==WGPUTextureFormat_R32Float?10u:8u),std::memory_order_relaxed);
                            // The bound view keeps the native R16Unorm texture alive until the queued copy is encoded.
                            if (texture.view) wgpuTextureViewAddRef(texture.view);
                            auto sourceHold=std::make_shared<QueuedTextureViewHold>(texture.view);
                            const WGPUTexture scratchTexture=scratch.texture;
                            if (!QueuePendingCustom(command,[views,sourceHold,r16Texture,scratchTexture,width,height,layers,targetFormat](WGPUCommandEncoder encoder){
                                    WGPUTexelCopyTextureInfo from=WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to=WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
                                    from.texture=r16Texture;to.texture=scratchTexture;
                                    const WGPUExtent3D extent{width,height,layers};
                                    wgpuCommandEncoderCopyTextureToTexture(encoder,&from,&to,&extent);
                                    bool ok=true;
                                    for (std::size_t i=0;i+1<views->size();i+=2)
                                        ok=EncodeGpuR16ToDepth16Copy(encoder,(*views)[i],(*views)[i+1],width,height,targetFormat) && ok;
                                    for (auto v : *views) ReleaseGpuTextureView(v);
                                    views->clear();
                                    sourceHold->Release();
                                    return ok;
                                })) {
                                for (auto v : *views) ReleaseGpuTextureView(v);
                                sourceHold->Release();
                                return fail(__LINE__);
                            }
                        }
                        entry.textureView=copy.view;
                        } // [skperf] !sharedBound
                    }
                }
                if (nativeDesc.format==GpuWrapApi::TEXFMT_D16U || nativeDesc.format==GpuWrapApi::TEXFMT_D32F ||
                    nativeDesc.format==GpuWrapApi::TEXFMT_D24S8 || nativeDesc.format==GpuWrapApi::TEXFMT_D32FS8) {
                    // Was an unsynchronized static std::set (data race across render threads) plus a std::string per
                    // depth SRV per draw; now the lock-free report set (tag 2<<40), still first 24 distinct.
                    static std::atomic<unsigned> depthReports{0};
                    if (FirstDrawReport(DrawReportKey(std::uint64_t(2) << 40, reinterpret_cast<std::uintptr_t>(source->originalHash), resource.binding)) &&
                        depthReports.fetch_add(1, std::memory_order_relaxed) < 24)
                        std::fprintf(stderr,"CP2077 depth SRV probe: hash=%s stage=%u binding=%u slot=%u nativeFormat=%u texture=%u\n",
                            source->originalHash,stage,resource.binding,resource.slot,unsigned(nativeDesc.format),texture.reference.ID());
                }
                // [nomadfix] A real SRV whose view dimension differs from the WGSL declaration (e.g. the engine's 2D
                // internal texture ref=8 bound to a texture_2d_array slot during the q000 Jackie trunk scene) makes
                // the bind group invalid, which invalidates the WHOLE command buffer at submit: the world rendered
                // black for the rest of the session (~37 'Invalid CommandBuffer' errors/s). Bind a zero view of the
                // declared type instead (D3D null-SRV read semantics), exactly as the compute path already does
                // (ComputeSampledViewMismatch). Only the dimension is checked; matching views are untouched.
                if (!variantModule && entry.textureView) {
                    const GpuWgslBinding declaredView=FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding);
                    WGPUTextureFormat boundFormat=WGPUTextureFormat_Undefined;
                    WGPUTextureViewDimension boundDimension=WGPUTextureViewDimension_Undefined;
                    // [skperf] round 63: timed on sampled draws; with viewMemo the engine's own SRV view is looked up once per list
                    const double cpVi0=drawCost.sampleKinds?emscripten_get_now():0;
                    bool boundKnown=false;
                    if (CpViewMemoOn() && entry.textureView==texture.view) {
                        const auto memo=command.viewInfoMemo.find(entry.textureView);
                        if (memo!=command.viewInfoMemo.end()) { boundFormat=memo->second.first; boundDimension=memo->second.second; boundKnown=true; }
                        else if ((boundKnown=GetGpuTextureViewInfo(entry.textureView,boundFormat,boundDimension)))
                            command.viewInfoMemo.emplace(entry.textureView,std::make_pair(boundFormat,boundDimension));
                    } else boundKnown=GetGpuTextureViewInfo(entry.textureView,boundFormat,boundDimension);
                    if (drawCost.sampleKinds) drawCost.cpViewInfo+=emscripten_get_now()-cpVi0;
                    // The view-info registry is size-capped (a shard is cleared at 4096 entries), so in long sessions a
                    // view bound earlier can be unknown here. The engine's own SRV view (entry == bound view) of a plain
                    // 2D or 3D texture always has that dimension (AcquireSampledGpuTextureView); arrays/cubes are left
                    // unchecked because mip-generation views may expose them as 2D arrays.
                    if (!boundKnown && entry.textureView==texture.view &&
                        (nativeDesc.type==GpuWrapApi::TEXTYPE_2D || nativeDesc.type==GpuWrapApi::TEXTYPE_3D)) {
                        boundDimension=nativeDesc.type==GpuWrapApi::TEXTYPE_2D ? WGPUTextureViewDimension_2D : WGPUTextureViewDimension_3D;
                        boundKnown=true;
                    }
                    if (declaredView.kind!=GpuWgslBinding_Unknown && declaredView.dimension!=WGPUTextureViewDimension_Undefined &&
                        boundKnown && boundDimension!=declaredView.dimension) {
                        const WGPUTextureView zeroView=GetGpuDefaultTextureView(declaredView); // borrowed device-lifetime view
                        static std::atomic<unsigned> dimensionReports{0};
                        const unsigned n=dimensionReports.fetch_add(1)+1;
                        if (n<=16 || (n&(n-1))==0)
                            std::fprintf(stderr,"CP2077 nomadfix SRV dimension fallback: hash=%s stage=%u binding=%u slot=%u ref=%u nativeNull=%u viewFormat=%u viewDimension=%u declaredKind=%u declaredDimension=%u ok=%u count=%u\n",
                                source->originalHash,stage,resource.binding,resource.slot,texture.reference.ID(),unsigned(texture.nativeNull),
                                unsigned(boundFormat),unsigned(boundDimension),unsigned(declaredView.kind),unsigned(declaredView.dimension),
                                unsigned(zeroView!=nullptr),n);
                        if (!zeroView) return skip(__LINE__);
                        entry.textureView=zeroView;
                    }
                }
            } else if (resource.kind == GraphicsResourceKind::MultilayerPool ||
                       resource.kind == GraphicsResourceKind::MultilayerTable) {
                if (!multilayerReady) {
                    if (!cp2077::web::GetGpuMultilayerPoolBindings(multilayerViews, multilayerTable)) return skip(__LINE__);
                    multilayerReady = true;
                }
                if (resource.kind == GraphicsResourceKind::MultilayerPool) {
                    if (resource.slot >= cp2077::web::kGpuMultilayerPoolSlots) return fail(__LINE__);
                    entry.textureView = multilayerViews[resource.slot]; // borrowed device-lifetime view
                } else {
                    entry.buffer = multilayerTable; entry.offset = 0; entry.size = cp2077::web::kGpuMultilayerTableBytes;
                    // Pool copies made stale by later writes are refreshed in list order before this draw.
                    if (cp2077::web::HasGpuMultilayerDirtyCopies() &&
                        !QueuePendingCustom(command, [](WGPUCommandEncoder encoder) {
                            return cp2077::web::EncodeGpuMultilayerDirtyCopies(encoder); })) return skip(__LINE__);
                    static std::atomic<unsigned> multilayerReports{0};
                    if (multilayerReports.fetch_add(1) < 8)
                        std::fprintf(stderr, "CP2077 multilayer pool draw: hash=%s stage=%u\n", source->originalHash, stage);
                }
            } else if (resource.kind == GraphicsResourceKind::TextureIndirection) {
                if(resource.slot>UINT16_MAX)return fail(__LINE__);
                // UINT16_MAX selects the engine's Default2D bindless texture (multilayer bindless fallback).
                Uint32 indirectSlot=resource.slot;
                if(indirectSlot==UINT16_MAX){
                    // [textures] Every t130 read of this shader samples the Default2D UV gradient (pink/magenta).
                    static std::atomic<unsigned> defaultReports{0};
                    if(defaultReports.fetch_add(1)<16)std::fprintf(stderr,"CP2077 bindless table bound as Default2D: hash=%s stage=%u binding=%u\n",
                        source->originalHash,stage,resource.binding);
                    const auto fallback=GpuApi::GetTextureIndirection(GpuApi::GetInternalTexture(GpuApi::INTERTEX_Default2D));
                    if(!fallback.IsValid())return fail(__LINE__);
                    indirectSlot=fallback.m_index;
                }
                GpuApi::TextureRef texture;
                if(!AcquireIndirectGpuTextureView(static_cast<Uint16>(indirectSlot),texture,entry.textureView)){
                    // A stale or unusable bindless slot reads the engine's Default2D texture, as for UINT16_MAX above.
                    // A failed acquire holds nothing (reference and view are null).
                    const auto fallback=GpuApi::GetTextureIndirection(GpuApi::GetInternalTexture(GpuApi::INTERTEX_Default2D));
                    if(!fallback.IsValid() || fallback.m_index==indirectSlot ||
                       !AcquireIndirectGpuTextureView(fallback.m_index,texture,entry.textureView))return skip(__LINE__);
                    static std::atomic<unsigned> fallbackReports{0};
                    if(fallbackReports.fetch_add(1)<16)std::fprintf(stderr,"CP2077 bindless texture fallback to Default2D: hash=%s stage=%u binding=%u slot=%u\n",
                        source->originalHash,stage,resource.binding,unsigned(indirectSlot));
                }
                retainedTextures.push_back(texture);indirectViews.push_back(entry.textureView);
            } else if (resource.kind == GraphicsResourceKind::Sampler) {
                if (resource.slot >= GpuApi::MAX_SAMPLER_STATES) return fail(__LINE__);
                const auto& sampler = command.graphicsSamplers[stage][resource.slot];
                // A null, stale or unacquirable sampler binds a default clamp/nearest sampler of the declared
                // kind instead of skipping the draw. A real nonzero bias the shader cannot apply still skips.
                bool defaultSampler=sampler.reference.isNull() || !sampler.sampler;
                auto* samplerOnce=defaultSampler ? nullptr : SamplerOnceFor(command,sampler.reference,sampler.sampler); // [skperf] texRetain
                if (!defaultSampler) {
                    Float bias=0;
                    if (samplerOnce ? !samplerOnce->biasOk : !GetEffectiveGpuSamplerBias(sampler.reference,sampler.sampler,bias)) defaultSampler=true;
                    else {
                        if (samplerOnce) bias=samplerOnce->bias;
                        if (bias!=0 && !source->samplerBiasLowered) return skip(__LINE__);
                    }
                }
                if (!defaultSampler && samplerOnce) {
                    if (samplerOnce->acquired < 0) {
                        samplerOnce->acquired = AcquireGpuSampler(sampler.reference, samplerOnce->sampler, sampler.sampler) ? 1 : 0;
                        if (samplerOnce->acquired) command.retainedSamplers.push_back(sampler.reference);
                    }
                    if (samplerOnce->acquired) entry.sampler=samplerOnce->sampler; else defaultSampler=true;
                } else if (!defaultSampler && AcquireGpuSampler(sampler.reference, entry.sampler, sampler.sampler))
                    retainedSamplers.push_back(sampler.reference);
                else defaultSampler=true;
                if (defaultSampler) {
                    const auto declared=FindGpuWgslBinding(source->wgsl,source->wgslBytes,resource.binding);
                    // Borrowed device-lifetime sampler: never retained or released by this draw.
                    entry.sampler=GetGpuDefaultSampler(declared.kind==GpuWgslBinding_ComparisonSampler);
                    if (!entry.sampler) return skip(__LINE__);
                    static std::atomic<unsigned> samplerReports{0};
                    const unsigned samplerCount=samplerReports.fetch_add(1)+1;
                    if (samplerCount<=16 || (samplerCount&(samplerCount-1))==0)
                        std::fprintf(stderr,"CP2077 default sampler fallback: hash=%s stage=%u binding=%u slot=%u ref=%u bound=%u comparison=%u count=%u\n",
                            source->originalHash,stage,resource.binding,resource.slot,sampler.reference.ID(),unsigned(sampler.sampler!=nullptr),
                            unsigned(declared.kind==GpuWgslBinding_ComparisonSampler),samplerCount);
                }
                // [tankview] Explicit layouts declare integer-texture-only samplers NonFiltering (textureGather of a
                // texture_2d<u32>); a linear engine sampler there fails pipeline creation/bind validation. Gather ignores
                // min/mag filtering, so the nearest twin of the same sampler is exact. The default sampler is nearest.
                if (!defaultSampler && cp2077::web::GraphicsSamplerIsNonFiltering(source,resource.binding)) {
                    entry.sampler=GetGpuNonFilteringGatherSampler(sampler.reference,sampler.sampler);
                    if(!entry.sampler)return skip(__LINE__);
                }
                if (!defaultSampler && stage==1 && resource.binding==779 && resource.slot==11 &&
                    fragment.module!=command.graphicsShaders[1].module &&
                    IsExactDrawShader(source,"10796363125667703835","5331e5512b792835996ce7c7725a08907796087c58438755d7386492da3d82e5")) {
                    entry.sampler=GetGpuNonFilteringGatherSampler(sampler.reference,sampler.sampler);
                    if(!entry.sampler)return fail(__LINE__);
                }
            } else return fail(__LINE__);
            entries.push_back(entry);
            if(traceShadowResources)std::fprintf(stderr,"CP2077 graphics resource probe: phase=returned command=%u shader=%s stage=%u binding=%u\n",
                bound.ID(),source->originalHash,stage,resource.binding);
        }
    }
    static std::atomic<unsigned> pendingShadowBindingReports{0};
    const bool traceShadowBinding=IsExactDrawShader(pixel,"18333707234177685286") &&
        pendingShadowBindingReports.fetch_add(1)<4;
    if(traceShadowBinding)std::fprintf(stderr,"CP2077 shadow binding probe: phase=begin shader=%s command=%u entries=%llu\n",
        pixel->originalHash,bound.ID(),static_cast<unsigned long long>(entries.size()));
    drawCost.mark(2);
    // Explicit layout: every dynamic uniform entry binds at offset 0 (so equal buffers give equal cache keys)
    // and its real offset (arena slot, or 0) moves to setBindGroup in ascending binding order. arenaSlots keep
    // the real offsets for the uniform writes.
    static_assert(cp2077::web::kMaxGraphicsLayoutDynamicOffsets == cp2077::web::kMaxGraphicsDrawDynamicOffsets);
    std::uint32_t dynamicOffsets[cp2077::web::kMaxGraphicsDrawDynamicOffsets]{};
    std::uint32_t dynamicOffsetCount = 0;
    // [skperf] round 13 copyHoist: buffer ranges read through the bind group, with their REAL offsets (before the
    // dynamic-offset conversion below). WGPU_WHOLE_SIZE / 0 size = to the end of the buffer.
    cp2077::web::GpuQueuedDraw::ReadRange drawReadRanges[cp2077::web::GpuQueuedDraw::kMaxReadRanges];
    unsigned drawReadRangeCount = 0; bool drawReadOverflow = false;
    for (const auto& e : entries) {
        if (!e.buffer) continue;
        if (drawReadRangeCount == cp2077::web::GpuQueuedDraw::kMaxReadRanges) { drawReadOverflow = true; break; }
        const std::uint64_t end = (e.size == 0 || e.size == WGPU_WHOLE_SIZE || e.offset > UINT64_MAX - e.size) ? UINT64_MAX : e.offset + e.size;
        drawReadRanges[drawReadRangeCount++] = {e.buffer, e.offset, end};
    }
    if (explicitLayout) {
        for (std::uint32_t k = 0; k < dynamicLayout.dynamicCount; ++k) {
            WGPUBindGroupEntry* dynamicEntry = nullptr;
            for (auto& entry : entries) if (entry.binding == dynamicLayout.dynamicBindings[k]) { dynamicEntry = &entry; break; }
            if (!dynamicEntry || !dynamicEntry->buffer || dynamicEntry->offset > UINT32_MAX) return skip(__LINE__);
            dynamicOffsets[dynamicOffsetCount++] = std::uint32_t(dynamicEntry->offset);
            dynamicEntry->offset = 0;
        }
    }
    // Queued draws (the common case) take a worker-side cache hit or defer creation to the batched main-thread
    // flush, so a cache miss no longer costs its own synchronous handoff.
    // [skperf] round 6 atomicQueue: atomic-texture draws are queued too (import copy, draw, export copy in order).
    const bool atomicQueued=!xorScratch && !atomicTransfers.empty() && Perf11Pools::AtomicQueueOn();
    if(!atomicTransfers.empty()){
        Perf11Pools::frameAtomicDraws.fetch_add(1,std::memory_order_relaxed);
        if(atomicQueued)Perf11Pools::frameAtomicQueued.fetch_add(1,std::memory_order_relaxed);
    }
    const bool queueDraw=!xorScratch && (atomicTransfers.empty() || atomicQueued);
    // [skperf] round 35 xorBindDefer (default ON, ?xorBindDefer=0 old): a queued XOR draw (no atomic transfers)
    // also takes a worker-side cache hit or defers the bind-group creation to the main-thread encode, like ordinary
    // queued draws. Its synchronous creation waited for the busy main thread: 8 ms (Badlands) / 18-23 ms (bar mirror
    // frames) per light-channel volume draw (tc22 xordraw probe: all of it in bindGroupMs).
    static const int cpXorBindDefer = [] { const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('xorBindDefer') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: xorBindDefer=%d\n", v); return v; }();
    const bool xorDeferBind = cpXorBindDefer && xorScratch && atomicTransfers.empty();
    group = (queueDraw || xorDeferBind) ? FindCachedGpuGraphicsBindGroup(pipeline, entries.data(), entries.size())
                      : CreateGpuGraphicsBindGroup(pipeline, 0, entries.data(), entries.size(), command.encoder);
    if (queueDraw && (cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) || cpEngineTrace::ProbesEnabled())) cmdlockCounters[group ? 7 : 8].fetch_add(1, std::memory_order_relaxed); // [cmdlock] bind group hit/miss ([skperf] round 38: probing only)
    if (queueDraw && !group) {
        // [nomadperf] bind-group miss classes: 0 auto layout, 1 explicit with an arena-offset static uniform, 2 explicit other.
        static std::atomic<unsigned long long> missClass[3]{};
        bool staticArena = false;
        for (const auto& e : entries) if (e.buffer && e.offset && !isDynamicBinding(e.binding)) { staticArena = true; break; }
        const unsigned k = !explicitLayout ? 0 : staticArena ? 1 : 2;
        if (k == 0) { // [autoreason]
            static std::atomic<unsigned long long> reasons[8]{};
            reasons[autoReason < 8 ? autoReason : 0].fetch_add(1, std::memory_order_relaxed);
            static std::mutex topMutex; static std::unordered_map<std::string, unsigned long long> topVs;
            static std::atomic<unsigned long long> seen{0};
            const auto m = seen.fetch_add(1, std::memory_order_relaxed);
            if ((m & 63) == 0) { std::lock_guard<std::mutex> g(topMutex); if (topVs.size() < 4096) ++topVs[std::string(vertex && vertex->originalHash ? vertex->originalHash : "-") + "/" + (pixel && pixel->originalHash ? pixel->originalHash : "-") + "#" + std::to_string(autoReason)]; }
            if (((m + 1) & 65535) == 0) {
                std::lock_guard<std::mutex> g(topMutex);
                std::vector<std::pair<unsigned long long, std::string>> v; for (auto& kv : topVs) v.emplace_back(kv.second, kv.first);
                std::sort(v.begin(), v.end(), [](auto& x, auto& y) { return x.first > y.first; });
                std::string top; for (std::size_t i = 0; i < v.size() && i < 6; ++i) top += v[i].second + "=" + std::to_string(v[i].first) + " ";
                std::fprintf(stderr, "CP2077 autoreason: xor=%llu gi=%llu acquireFailed=%llu explicitFailed=%llu a2c=%llu dual=%llu otherVariant=%llu other=%llu top(1/64)=%s\n",
                    reasons[1].load(), reasons[2].load(), reasons[3].load(), reasons[4].load(), reasons[5].load(), reasons[6].load(), reasons[7].load(), reasons[0].load(), top.c_str());
            }
        }
        const auto n = missClass[k].fetch_add(1, std::memory_order_relaxed) + 1;
        if (((missClass[0].load() + missClass[1].load() + missClass[2].load()) & 65535) == 0)
            std::fprintf(stderr, "CP2077 nomadperf bind group miss classes: auto=%llu explicitStaticArena=%llu explicitOther=%llu lastVS=%s\n",
                missClass[0].load(), missClass[1].load(), missClass[2].load(), vertex && vertex->originalHash ? vertex->originalHash : "-");
        (void)n;
    }
    drawCost.mark(3);
    if(traceShadowBinding)std::fprintf(stderr,"CP2077 shadow binding probe: phase=returned shader=%s command=%u created=%u\n",
        pixel->originalHash,bound.ID(),unsigned(group!=nullptr));
    if (!group && !queueDraw && !xorDeferBind) {
        static std::atomic<unsigned> bindingReports{0};
        if (bindingReports.fetch_add(1)<4) {
            for (unsigned stage=0;stage<2;++stage) {
                const auto* source=command.graphicsShaders[stage].source;
                if (!source) continue;
                for (std::size_t i=0;i<source->resourceCount;++i) {
                    const auto& resource=source->resources[i];
                    if (resource.kind!=GraphicsResourceKind::TextureSRV || resource.slot>=GpuApi::MAX_SRV_BINDINGS) continue;
                    const auto& texture=command.graphicsTextures[stage][resource.slot];
                    const auto native=GpuApi::GetTextureDesc(texture.reference);
                    std::fprintf(stderr,"CP2077 rejected texture binding: hash=%s stage=%u binding=%u slot=%u ref=%u nativeFormat=%u nativeType=%u view=%p\n",
                        source->originalHash,stage,resource.binding,resource.slot,texture.reference.ID(),
                        unsigned(native.format),unsigned(native.type),static_cast<void*>(texture.view));
                }
            }
        }
        return skip(__LINE__);
    }
    // The real bind group retains its views. Keep native texture references
    // through GPU completion, while releasing temporary caller view handles.
    for(auto view:indirectViews)ReleaseGpuTextureView(view);
    indirectViews.clear();
    for (unsigned i = 0; i < command.colorAttachmentCount; ++i) {
        // [whitewall] r5: D3D render-target setups may leave a slot empty (null RTV between bound targets, e.g. the
        // forward-transparent pass of glass/particles/holograms/eye_shadow). PrepareRenderAttachment keeps such a slot
        // empty (no view, format Undefined = sparse WebGPU target), but retaining its null reference failed and every
        // draw of that pass was skipped as 'missing input'. Nothing to retain for an empty slot. ?nullrt=0 = old skip.
        const auto& attachment = command.colorAttachments[i];
        if (attachment.reference.isNull() && !attachment.view && CpNullRenderTargetSlotsAllowed()) continue;
        if (!retainTexture(attachment.reference)) {
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1) < 16)
                std::fprintf(stderr, "CP2077 whitewall: color attachment retain failed slot=%u ref=%u null=%d view=%p format=%u colors=%u PS=%s\n",
                    i, unsigned(attachment.reference.ID()), int(attachment.reference.isNull()), static_cast<void*>(attachment.view),
                    unsigned(attachment.format), unsigned(command.colorAttachmentCount), DrawShaderHash(command.graphicsShaders[1].source));
            return skip(__LINE__);
        }
    }
    if (!command.depthAttachment.reference.isNull() && !retainTexture(command.depthAttachment.reference)) return skip(__LINE__);
    auto& buffers = scratch.buffers;
    for (std::size_t i = 0; i < geometry.buffers.size(); ++i) {
        if(!geometry.buffers[i].attributeCount){buffers.push_back({nullptr,0,0,true});continue;}
        const auto& buffer = command.vertexBuffers[i];
        if (!retainBuffer(buffer.reference)) return skip(__LINE__);
        buffers.push_back({buffer.buffer, buffer.offset, buffer.bytes});
    }
    if (!indirectCount && Perf11Pools::PerfProbeOn()) {
        // [skperf] round 15 R2a: would folding vertex offsets into baseVertex/firstInstance be exact for this draw?
        static thread_local WGPUBuffer prevBuffer[16]; static thread_local std::uint64_t prevResidue[16], prevOffset[16];
        unsigned vertexSlots = 0, instanceSlots = 0; bool vertexSameK = true, instanceSameK = true;
        std::uint64_t vertexK = 0, instanceK = 0;
        for (std::size_t i = 0; i < geometry.buffers.size() && i < 16; ++i) {
            const auto& slot = geometry.buffers[i];
            if (!slot.attributeCount || !slot.arrayStride) continue;
            const auto& bound = command.vertexBuffers[i];
            const std::uint64_t stride = slot.arrayStride, k = bound.offset / stride, residue = bound.offset % stride;
            Perf11Pools::vbProbe[8].fetch_add(1, std::memory_order_relaxed);
            if (slot.stepMode == WGPUVertexStepMode_Instance) { if (instanceSlots++ && k != instanceK) instanceSameK = false; instanceK = k; }
            else {
                if (vertexSlots++ && k != vertexK) vertexSameK = false;
                vertexK = k;
                if (bound.offset) Perf11Pools::vbProbe[6].fetch_add(1, std::memory_order_relaxed);
            }
            if (prevBuffer[i] == bound.buffer && prevResidue[i] == residue && prevOffset[i] != bound.offset) Perf11Pools::vbProbe[7].fetch_add(1, std::memory_order_relaxed);
            prevBuffer[i] = bound.buffer; prevResidue[i] = residue; prevOffset[i] = bound.offset;
        }
        if (vertexSlots) {
            Perf11Pools::vbProbe[0].fetch_add(1, std::memory_order_relaxed);
            Perf11Pools::vbProbe[vertexSlots == 1 ? 1 : vertexSameK ? 2 : 3].fetch_add(1, std::memory_order_relaxed);
        }
        if (instanceSlots) {
            Perf11Pools::vbProbe[4].fetch_add(1, std::memory_order_relaxed);
            if (instanceSameK) Perf11Pools::vbProbe[5].fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (indexed && !retainBuffer(command.indexBuffer.reference)) return skip(__LINE__);
    if (indirectCount && (!command.indirectArguments.buffer || !retainBuffer(command.indirectArguments.reference))) return fail(__LINE__);
    GraphicsDraw draw;
    draw.pipeline = pipeline; draw.colors = colors; draw.colorCount = xorScratch?8:command.colorAttachmentCount;
    draw.depth = command.depthAttachment.view; draw.depthReadOnly = command.depthReadOnly;
    draw.hasStencil = command.depthAttachment.format == WGPUTextureFormat_Depth24PlusStencil8 ||
        command.depthAttachment.format == WGPUTextureFormat_Depth32FloatStencil8;
    draw.targetWidth = target.width; draw.targetHeight = target.height;
    draw.vertices = buffers.data(); draw.vertexCount = buffers.size();
    draw.indices = {command.indexBuffer.buffer, command.indexBuffer.offset, command.indexBuffer.bytes};
    draw.indexFormat = command.indexBuffer.indexFormat;
    // A deferred group is created at encode time; the placeholder only satisfies validation and is replaced.
    GraphicsDrawGroup binding{0, group ? group : reinterpret_cast<WGPUBindGroup>(std::uintptr_t(1))}; draw.groups = &binding; draw.groupCount = 1;
    binding.dynamicOffsetCount = dynamicOffsetCount;
    std::copy(dynamicOffsets, dynamicOffsets + dynamicOffsetCount, binding.dynamicOffsets);
    draw.viewportX = command.viewport.x; draw.viewportY = command.viewport.y;
    draw.viewportWidth = command.viewport.width; draw.viewportHeight = command.viewport.height;
    draw.minDepth = command.viewport.minDepth; draw.maxDepth = command.viewport.maxDepth;
    // DX12 clips signed scissor rectangles to the physical target extent.
    auto clip = [](Int32 value, Uint32 limit) -> Uint32 {
        if (value <= 0) return 0; return Uint32(value) > limit ? limit : Uint32(value);
    };
    draw.scissorX = command.scissorsSet ? clip(command.scissors.m_left, target.width) : 0;
    draw.scissorY = command.scissorsSet ? clip(command.scissors.m_top, target.height) : 0;
    draw.scissorWidth = (command.scissorsSet ? clip(command.scissors.m_right, target.width) : target.width) - draw.scissorX;
    draw.scissorHeight = (command.scissorsSet ? clip(command.scissors.m_bottom, target.height) : target.height) - draw.scissorY;
    draw.blendFactor = command.blendFactor; draw.stencilReference = command.stencilReference;
    draw.indexed = indexed; draw.count = count; draw.instances = instances; draw.firstInstance = firstInstance;
    draw.indirectCount = indirectCount;
    draw.mergeablePass = !writesStorage;
    draw.indirectArguments = {command.indirectArguments.buffer, command.indirectOffset, command.indirectArguments.bytes};
    draw.first = indexed ? firstIndex : baseVertex; std::memcpy(&draw.baseVertex, &baseVertex, sizeof(baseVertex));
    // Arena/ring uniform bytes reach the GPU through FlushCommandUniforms (one batched queue.writeBuffer per
    // block right before this list is submitted, which every draw of the list precedes). Queued and immediate
    // draws no longer repeat them per draw on the main thread (that was one JS writeBuffer per uniform per draw,
    // writing the same bytes a second time). Only the XOR path below still writes its slots explicitly.
    // slot.offset is absolute in the block's buffer (ring pages start at block.base).
    auto& uniformWrites = scratch.uniformWrites;
    if(xorScratch){
        uniformWrites.reserve(arenaSlots.size());
        for(const auto& slot:arenaSlots){
            auto& block=command.uniformBlocks[slot.block];
            uniformWrites.push_back({block.buffer,slot.offset,block.bytes.data()+(slot.offset-block.base),slot.size});
        }
    }
    draw.uniformWrites=nullptr;draw.uniformWriteCount=0;
    drawQueueStarted=true;
    // Common case: queue the draw so a whole run of draws costs one main-thread handoff at the next flush.
    // Atomic-texture draws (and XOR draws with atomic transfers) keep the immediate path because they interleave
    // their own encodes; plain XOR draws are queued below (perf8).
    if(queueDraw){
        std::shared_ptr<cp2077::web::GpuQueuedDraw> queued;
        if(!cp2077::web::PrepareQueuedGpuGraphicsDraw(draw,queued,!Perf11Pools::PassHoldOn()))return fail(__LINE__); // [skperf] round 15 passHold
        drawCost.mark(4);
        if(queued){ // [skperf] round 13 copyHoist read ranges
            for(unsigned k=0;k<drawReadRangeCount;++k)queued->readRanges[k]=drawReadRanges[k];
            queued->readRangeCount=drawReadRangeCount;queued->readOverflow=drawReadOverflow;
        }
        if(queued && !group){
            queued->bindPipeline=pipeline;queued->bindEntries=entries;queued->bindOwner=command.encoder;
            for(const auto& e:queued->bindEntries){if(e.buffer)wgpuBufferAddRef(e.buffer);if(e.textureView)wgpuTextureViewAddRef(e.textureView);if(e.sampler)wgpuSamplerAddRef(e.sampler);}
            queued->bindRefsHeld=true;
        }
        if(queued && atomicQueued)
            for(auto* transfer:atomicTransfers){
                const WGPUTexture source=transfer->source; const auto bridge=transfer->buffer;
                if(!QueuePendingCustom(command,[source,bridge](WGPUCommandEncoder encoder){
                        return CopyGpuAtomicTextureBuffer(encoder,source,WGPUTextureFormat_R32Uint,0,0,bridge,false);}))return fail(__LINE__);
            }
        if(queued && !QueuePendingGraphicsDraw(command,std::move(queued)))return fail(__LINE__);
        if(atomicQueued)
            for(auto* transfer:atomicTransfers){
                const WGPUTexture source=transfer->source; const auto bridge=transfer->buffer;
                if(!QueuePendingCustom(command,[source,bridge](WGPUCommandEncoder encoder){
                        return CopyGpuAtomicTextureBuffer(encoder,source,WGPUTextureFormat_R32Uint,0,0,bridge,true);}))return fail(__LINE__);
            }
        drawCost.mark(5);
    }
    // perf8: an XOR draw without atomic transfers is queued too, as three ordered operations (initialize dispatch,
    // the XOR draw, the resolve draw) carrying the same encodes and arguments EncodeGpuLogicXorDraw issued. Its
    // immediate form cost a flush, one handoff per uniform write, a synchronous bind group and three encodes.
    const bool queueXor=xorScratch && atomicTransfers.empty();
    const bool copiesFlushed=queueDraw || queueXor || FlushPendingBufferCopies(command);
    if(!queueDraw)drawCost.mark(4);
    if (!copiesFlushed) return fail(__LINE__);
    if(!atomicQueued)for(auto* transfer:atomicTransfers)
        if(!CopyGpuAtomicTextureBuffer(command.encoder,transfer->source,WGPUTextureFormat_R32Uint,0,0,transfer->buffer,false))return fail(__LINE__);
    bool drawEncoded=queueDraw;
    if(queueDraw){}
    else if(queueXor){
        const auto& s=*xorScratch;
        if(draw.colorCount!=8||draw.targetWidth!=s.width||draw.targetHeight!=s.height||!s.initialize||!s.resolve||!s.resolveGroup)return fail(__LINE__);
        WGPUTextureView destination=command.colorAttachments[0].view;
        // [skperf] round 16 xorBatch: continue the open XOR run when its resolve op is still the last non-copy op.
        const bool batchXor=Perf11Pools::XorBatchOn();
        auto& xorRun=command.xorRun;
        bool continueRun=false; std::size_t resolveAt=0;
        if(batchXor && xorRun.resolve && xorRun.scratch==static_cast<const void*>(xorScratch) && xorRun.destination==destination){
            auto& ops=command.pendingBufferCopies;
            for(std::size_t k=ops.size();k-->0;){
                if(ops[k].draw==xorRun.resolve){resolveAt=k;continueRun=true;break;}
                if(ops[k].draw||ops[k].custom)break; // anything but a buffer copy / debug marker ends the run
            }
        }
        Perf11Pools::xorStats[0].fetch_add(1,std::memory_order_relaxed);
        std::shared_ptr<QueuedComputeDispatch> init;
        if(continueRun){
            auto& ops=command.pendingBufferCopies;
            ops.erase(ops.begin()+std::ptrdiff_t(resolveAt)); // the run's resolve moves to after this draw
            for(auto& index:command.stagedOps)if(index>resolveAt)--index;
            Perf11Pools::xorStats[1].fetch_add(1,std::memory_order_relaxed);Perf11Pools::xorStats[2].fetch_add(1,std::memory_order_relaxed);
        } else {
        WGPUTextureView sourceView=nullptr;const auto ref=command.colorAttachments[0].reference;
        if(!AcquireSampledGpuTextureView(ref,sourceView,false,0,false,0))return fail(__LINE__);
        // The initialize group is created on the main thread at encode time; its entries hold their own references.
        init=std::make_shared<QueuedComputeDispatch>();
        init->pipeline=s.initialize;init->owner=command.encoder;
        init->x=(s.width+7)/8;init->y=(s.height+7)/8;init->z=1;
        init->entries.assign(2,WGPU_BIND_GROUP_ENTRY_INIT);
        init->entries[0].binding=0;init->entries[0].textureView=sourceView;
        init->entries[1].binding=1;init->entries[1].textureView=s.storage;
        init->HoldEntries();
        ReleaseGpuTextureView(sourceView);GpuApi::Release(ref); // the list retains the attachment reference
        }
        // Same uniform bytes as the former WriteGpuBufferRange calls, written by the main thread before the XOR draw.
        draw.uniformWrites=uniformWrites.data();draw.uniformWriteCount=uniformWrites.size();
        draw.mergeablePass=batchXor; // [skperf] round 16: XOR draws of a run may share one pass (same 8 scratch layers)
        GraphicsDraw resolved;resolved.pipeline=s.resolve;resolved.colors=&destination;resolved.colorCount=1;
        resolved.targetWidth=s.width;resolved.targetHeight=s.height;resolved.viewportWidth=float(s.width);resolved.viewportHeight=float(s.height);
        resolved.scissorWidth=s.width;resolved.scissorHeight=s.height;resolved.count=3;resolved.instances=1;
        resolved.mergeablePass=false; // each of the three keeps its own pass, as before
        GraphicsDrawGroup resolveBinding{0,s.resolveGroup};resolved.groups=&resolveBinding;resolved.groupCount=1;
        std::shared_ptr<cp2077::web::GpuQueuedDraw> xorDraw,resolveDraw;
        if(!cp2077::web::PrepareQueuedGpuGraphicsDraw(draw,xorDraw,!Perf11Pools::PassHoldOn()) || !cp2077::web::PrepareQueuedGpuGraphicsDraw(resolved,resolveDraw,!Perf11Pools::PassHoldOn()))
            return fail(__LINE__);
        if(init){
        if(!QueuePendingCustom(command,[init](WGPUCommandEncoder encoder)->bool{
                // Main thread, inside the batched EncodeGpuOrderedCommands handoff.
                init->created=cp2077::web::CreateGpuComputeBindGroupOnMain(init->pipeline,init->entries.data(),init->entries.size(),init->owner);
                init->ReleaseEntries();
                return init->created && cp2077::web::EncodeGpuComputeDispatchOnMain(encoder,init->pipeline,init->created,init->x,init->y,init->z,nullptr,0);
            }))return fail(__LINE__);
        command.queuedDispatches.push_back(init); // FlushPendingBufferCopies retains the created group
        }
        if(xorDraw && !group){ // [skperf] round 35 xorBindDefer: created on the main thread at encode (cache miss)
            xorDraw->bindPipeline=pipeline;xorDraw->bindEntries=entries;xorDraw->bindOwner=command.encoder;
            for(const auto& e:xorDraw->bindEntries){if(e.buffer)wgpuBufferAddRef(e.buffer);if(e.textureView)wgpuTextureViewAddRef(e.textureView);if(e.sampler)wgpuSamplerAddRef(e.sampler);}
            xorDraw->bindRefsHeld=true;
        }
        if(xorDraw && !QueuePendingGraphicsDraw(command,std::move(xorDraw)))return fail(__LINE__);
        {   // [skperf] round 16: remember this run's resolve (a following XOR draw of the run replaces it)
            std::shared_ptr<cp2077::web::GpuQueuedDraw> keep=resolveDraw;
            if(resolveDraw && !QueuePendingGraphicsDraw(command,std::move(resolveDraw)))return fail(__LINE__);
            if(batchXor && keep){xorRun.scratch=xorScratch;xorRun.destination=destination;xorRun.resolve=std::move(keep);}
            else xorRun={};
        }
        drawEncoded=true;
        static std::atomic<unsigned> reports{0};if(reports.fetch_add(1)<8)std::fprintf(stderr,"CP2077 integer XOR draw probe: originalPixel=%s width=%u height=%u encoded=1 queued=1\n",pixel->originalHash,target.width,target.height);
    }
    else if(xorScratch){
        WGPUTextureView sourceView=nullptr;const auto ref=command.colorAttachments[0].reference;
        if(!AcquireSampledGpuTextureView(ref,sourceView,false,0,false,0))return fail(__LINE__);
        for(const auto& write:uniformWrites)if(!cp2077::web::WriteGpuBufferRange(write.buffer,write.offset,write.bytes,write.size))return fail(__LINE__);
        WGPUBindGroup initGroup=nullptr;drawEncoded=EncodeGpuLogicXorDraw(command.encoder,draw,command.colorAttachments[0].view,sourceView,*xorScratch,initGroup);
        ReleaseGpuTextureView(sourceView);GpuApi::Release(ref);
        if(initGroup)command.retainedBindGroups.push_back(initGroup);
        static std::atomic<unsigned> reports{0};if(reports.fetch_add(1)<8)std::fprintf(stderr,"CP2077 integer XOR draw probe: originalPixel=%s width=%u height=%u encoded=%u\n",pixel->originalHash,target.width,target.height,unsigned(drawEncoded));
    }else drawEncoded=EncodeGpuGraphicsDraw(command.encoder,draw);
    if(!queueDraw)drawCost.mark(5);
    if (!drawEncoded) return fail(__LINE__);
    if(!atomicQueued)for(auto* transfer:atomicTransfers)
        if(!CopyGpuAtomicTextureBuffer(command.encoder,transfer->source,WGPUTextureFormat_R32Uint,0,0,transfer->buffer,true))return fail(__LINE__);
    command.retainedRenderPipelines.push_back(pipeline); if(group)command.retainedBindGroups.push_back(group);
    command.uploads.insert(command.uploads.end(), uploads.begin(), uploads.end());
    command.retainedBuffers.insert(command.retainedBuffers.end(), retainedBuffers.begin(), retainedBuffers.end());
    command.retainedTextures.insert(command.retainedTextures.end(), retainedTextures.begin(), retainedTextures.end());
    command.retainedSamplers.insert(command.retainedSamplers.end(), retainedSamplers.begin(), retainedSamplers.end());
    command.retainedShaders.insert(command.retainedShaders.end(), retainedShaders.begin(), retainedShaders.end());
    // [skperf] listGen: this draw's texture writes (depth only when depth writes are enabled and not read-only).
    if (om.hasDepthStencil && !command.depthReadOnly && om.depthStencil.depthWriteEnabled==WGPUOptionalBool_True &&
        !command.depthAttachment.reference.isNull()) ++command.localWriteGen[command.depthAttachment.reference.ID()];
    for (const auto& storageTexture : command.pixelStorageTextures)
        if (!storageTexture.reference.isNull()) ++command.localWriteGen[storageTexture.reference.ID()];
    NoteBoundDraw(command, BoundDrawOutcome::Ok);
    return true;
}
// [whitewall] Public entry: a normal draw, or the two replay phases of a multi-target dual-source draw (see Impl).
static bool DrawBoundGpuGraphics(ePrimitiveType primitive, bool indexed, Uint32 baseVertex,
                                 Uint32 firstIndex, Uint32 count, Uint32 instances, Uint32 firstInstance,
                                 Uint32 indirectCount = 0) {
    t_cpDualSplitPhase = 0; t_cpDualSplitRequested = false;
    bool result = DrawBoundGpuGraphicsImpl(primitive, indexed, baseVertex, firstIndex, count, instances, firstInstance, indirectCount);
    if (!t_cpDualSplitRequested) return result;
    t_cpDualSplitRequested = false;
    static std::atomic<unsigned> splits{0};
    const unsigned n = splits.fetch_add(1) + 1;
    if (n <= 8 || !(n & (n - 1))) std::fprintf(stderr, "CP2077 whitewall: multi-target dual-source split draw count=%u\n", n);
    t_cpDualSplitPhase = 1;
    result = DrawBoundGpuGraphicsImpl(primitive, indexed, baseVertex, firstIndex, count, instances, firstInstance, indirectCount);
    if (result) {
        t_cpDualSplitPhase = 2;
        result = DrawBoundGpuGraphicsImpl(primitive, indexed, baseVertex, firstIndex, count, instances, firstInstance, indirectCount);
    }
    t_cpDualSplitPhase = 0;
    return result;
}
