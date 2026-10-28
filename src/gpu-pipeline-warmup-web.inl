// [warmup] Render-pipeline warmup from a build-time manifest (runtime/pipeline-warmup.json).
// Chrome's GPU process compiles new pipelines while the game runs, so each new material/state the player meets
// stalls frames for ~0.1-1 s. The manifest lists the shader identities + fixed-function state of every render
// pipeline that ?sandbox=1 sessions created (recorded with ?psoRecord=1, merged by S/warmup/mkmanifest.mjs).
// From the first graphics module creation on, a warm thread re-creates them with createRenderPipelineAsync,
// at most K in flight, into the same render-pipeline cache the draw path uses (graphics modules are deduplicated
// per converted source, so the keys match). A draw that meets a key still warming joins it (pending skip).
// Switches (read on the main thread): ?psoWarm=0 off, ?psoWarmK=<1..16> concurrency (default 4: K=1 3.2/s, K=2 6.2/s, K=4 9.1/s measured), ?psoRecord=1.
// JS: Module.cp2077PipelineWarmup = {total,missing,layoutMiss,started,skipped,done,failed,inFlight,k,t0,t1,finished}.
//     Module.cp2077PsoRecord = [{vs,fs,lay,w}] (first-seen order) when recording.
// Included at the end of gpu-shader-web.cpp (needs the layout cache and the render request machinery).
namespace {
std::atomic<int> warmSwitchesRead{0}, warmEnabled{1}, warmK{4}, psoRecordEnabled{0};
std::atomic<int> warmInFlight{0};
std::atomic<std::uint64_t> warmTotal{0}, warmMissing{0}, warmLayoutMiss{0};
std::atomic<std::uint64_t> warmStarted{0}, warmSkipped{0}, warmDone{0}, warmFailed{0};
void ReadWarmSwitchesOnMain() {
    if (warmSwitchesRead.load(std::memory_order_acquire)) return;
    const int packed = MAIN_THREAD_EM_ASM_INT({
        const q = new URLSearchParams(location.search);
        const k = Math.max(1, Math.min(16, parseInt(q.get('psoWarmK') || '4', 10) || 4));
        return (q.get('psoWarm') === '0' ? 0 : 1) | (q.get('psoRecord') === '1' ? 2 : 0) | (k << 8);
    });
    warmEnabled.store(packed & 1); psoRecordEnabled.store((packed >> 1) & 1); warmK.store(packed >> 8);
    warmSwitchesRead.store(1, std::memory_order_release);
    std::fprintf(stderr, "CP2077 warmup switches: psoWarm=%d psoWarmK=%d psoRecord=%d modDedup=%d\n",
                 packed & 1, packed >> 8, (packed >> 1) & 1, GraphicsModuleDedupEnabled());
}
EM_JS(void, PublishWarmStats, (double total, double missing, double layoutMiss, double started, double skipped,
                               double done, double failed, int inFlight, int k, int finished), {
    const s = Module['cp2077PipelineWarmup'] ||= {t0: performance.now()};
    s.total = total; s.missing = missing; s.layoutMiss = layoutMiss; s.started = started; s.skipped = skipped;
    s.done = done; s.failed = failed; s.inFlight = inFlight; s.k = k; s.t1 = performance.now();
    if (finished) { s.finished = true; s.tFinished = performance.now(); }
});
void PublishWarmStatsOnMain(int finished) {
    PublishWarmStats(double(warmTotal.load()), double(warmMissing.load()), double(warmLayoutMiss.load()),
                     double(warmStarted.load()), double(warmSkipped.load()), double(warmDone.load()),
                     double(warmFailed.load()), warmInFlight.load(), warmK.load(), finished);
}
void PublishWarmFinishedOnMain(void*) { PublishWarmStatsOnMain(1); }
// Main thread: a warm request completed (CompleteRenderPipeline) or was not started.
void WarmRenderPipelineFinished(bool ok) {
    (ok ? warmDone : warmFailed).fetch_add(1);
    warmInFlight.fetch_sub(1);
    PublishWarmStatsOnMain(0);
}

// ---- state serialization: every non-pointer descriptor field, as 32-bit words (version 1) ----
bool SerializeRenderState(const WGPURenderPipelineDescriptor& d, std::vector<std::uint32_t>& w) {
    w.clear(); w.push_back(1);
    auto u64 = [&](std::uint64_t v) { w.push_back(std::uint32_t(v)); return v <= 0xffffffffull; };
    auto f32 = [&](float v) { std::uint32_t b; std::memcpy(&b, &v, 4); w.push_back(b); };
    auto stencil = [&](const WGPUStencilFaceState& s) { w.push_back(s.compare); w.push_back(s.failOp); w.push_back(s.depthFailOp); w.push_back(s.passOp); };
    auto blend = [&](const WGPUBlendComponent& b) { w.push_back(b.operation); w.push_back(b.srcFactor); w.push_back(b.dstFactor); };
    w.push_back(std::uint32_t(d.vertex.bufferCount));
    for (std::size_t j = 0; j < d.vertex.bufferCount; ++j) {
        const auto& b = d.vertex.buffers[j];
        w.push_back(b.stepMode); if (!u64(b.arrayStride)) return false; w.push_back(std::uint32_t(b.attributeCount));
        for (std::size_t i = 0; i < b.attributeCount; ++i) {
            const auto& a = b.attributes[i];
            w.push_back(a.format); if (!u64(a.offset)) return false; w.push_back(a.shaderLocation);
        }
    }
    w.push_back(d.primitive.topology); w.push_back(d.primitive.stripIndexFormat); w.push_back(d.primitive.frontFace);
    w.push_back(d.primitive.cullMode); w.push_back(d.primitive.unclippedDepth);
    w.push_back(d.depthStencil ? 1 : 0);
    if (d.depthStencil) {
        const auto& s = *d.depthStencil;
        w.push_back(s.format); w.push_back(s.depthWriteEnabled); w.push_back(s.depthCompare);
        stencil(s.stencilFront); stencil(s.stencilBack);
        w.push_back(s.stencilReadMask); w.push_back(s.stencilWriteMask);
        w.push_back(std::uint32_t(s.depthBias)); f32(s.depthBiasSlopeScale); f32(s.depthBiasClamp);
    }
    w.push_back(d.multisample.count); w.push_back(d.multisample.mask); w.push_back(d.multisample.alphaToCoverageEnabled);
    w.push_back(d.fragment ? 1 : 0);
    if (d.fragment) {
        w.push_back(std::uint32_t(d.fragment->targetCount));
        for (std::size_t i = 0; i < d.fragment->targetCount; ++i) {
            const auto& t = d.fragment->targets[i];
            w.push_back(t.format); if (!u64(t.writeMask)) return false; w.push_back(t.blend ? 1 : 0);
            if (t.blend) { blend(t.blend->color); blend(t.blend->alpha); }
        }
    }
    return true;
}
// Fills request's owned arrays and descriptor (modules/layout set by the caller). False on malformed words.
bool DeserializeRenderState(const std::vector<std::uint32_t>& w, RenderPipelineRequest& r) {
    std::size_t p = 0; bool ok = true;
    auto next = [&]() -> std::uint32_t { if (p >= w.size()) { ok = false; return 0; } return w[p++]; };
    auto f32 = [&]() { std::uint32_t b = next(); float v; std::memcpy(&v, &b, 4); return v; };
    auto stencil = [&](WGPUStencilFaceState& s) {
        s.compare = WGPUCompareFunction(next()); s.failOp = WGPUStencilOperation(next());
        s.depthFailOp = WGPUStencilOperation(next()); s.passOp = WGPUStencilOperation(next());
    };
    auto blend = [&](WGPUBlendComponent& b) {
        b.operation = WGPUBlendOperation(next()); b.srcFactor = WGPUBlendFactor(next()); b.dstFactor = WGPUBlendFactor(next());
    };
    if (next() != 1) return false;
    auto& d = r.descriptor; d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    const std::uint32_t buffers = next(); if (!ok || buffers > 16) return false;
    r.buffers.assign(buffers, WGPU_VERTEX_BUFFER_LAYOUT_INIT); r.attributes.assign(buffers, {});
    for (std::uint32_t j = 0; j < buffers; ++j) {
        auto& b = r.buffers[j];
        b.stepMode = WGPUVertexStepMode(next()); b.arrayStride = next();
        const std::uint32_t count = next(); if (!ok || count > 64) return false;
        r.attributes[j].assign(count, WGPU_VERTEX_ATTRIBUTE_INIT);
        for (auto& a : r.attributes[j]) { a.format = WGPUVertexFormat(next()); a.offset = next(); a.shaderLocation = next(); }
        b.attributeCount = count; b.attributes = r.attributes[j].data();
    }
    d.vertex.bufferCount = buffers; d.vertex.buffers = r.buffers.data();
    d.vertex.entryPoint = {"main", WGPU_STRLEN};
    d.primitive.topology = WGPUPrimitiveTopology(next()); d.primitive.stripIndexFormat = WGPUIndexFormat(next());
    d.primitive.frontFace = WGPUFrontFace(next()); d.primitive.cullMode = WGPUCullMode(next()); d.primitive.unclippedDepth = next();
    if (next()) {
        auto& s = r.depth; s = WGPU_DEPTH_STENCIL_STATE_INIT;
        s.format = WGPUTextureFormat(next()); s.depthWriteEnabled = WGPUOptionalBool(next()); s.depthCompare = WGPUCompareFunction(next());
        stencil(s.stencilFront); stencil(s.stencilBack);
        s.stencilReadMask = next(); s.stencilWriteMask = next();
        s.depthBias = std::int32_t(next()); s.depthBiasSlopeScale = f32(); s.depthBiasClamp = f32();
        d.depthStencil = &r.depth;
    }
    d.multisample.count = next(); d.multisample.mask = next(); d.multisample.alphaToCoverageEnabled = next();
    if (next()) {
        const std::uint32_t targets = next(); if (!ok || targets > 8) return false;
        r.targets.assign(targets, WGPU_COLOR_TARGET_STATE_INIT); r.blends.assign(targets, WGPU_BLEND_STATE_INIT);
        for (std::uint32_t i = 0; i < targets; ++i) {
            auto& t = r.targets[i];
            t.format = WGPUTextureFormat(next()); t.writeMask = WGPUColorWriteMask(next());
            if (next()) { blend(r.blends[i].color); blend(r.blends[i].alpha); t.blend = &r.blends[i]; }
        }
        r.fragment = WGPU_FRAGMENT_STATE_INIT;
        r.fragment.entryPoint = {"main", WGPU_STRLEN};
        r.fragment.targetCount = targets; r.fragment.targets = r.targets.data();
        d.fragment = &r.fragment;
    }
    return ok && p == w.size();
}

// ---- recording (main thread, from PublishCompletedRenderPipeline) ----
EM_JS(void, PushPsoRecord, (const char* text), {
    (Module['cp2077PsoRecord'] ||= []).push(JSON.parse(UTF8ToString(Number(text))));
});
bool ExplicitLayoutPairOf(WGPUPipelineLayout layout, const cp2077::web::ConvertedGraphicsShader*& vertex,
                          const cp2077::web::ConvertedGraphicsShader*& pixel);
bool EntryIsMain(const WGPUStringView& view) {
    if (!view.data) return false;
    const std::size_t n = view.length == WGPU_STRLEN ? std::strlen(view.data) : view.length;
    return n == 4 && std::memcmp(view.data, "main", 4) == 0;
}
void AppendShaderIdentity(std::string& out, const cp2077::web::ConvertedGraphicsShader* s) {
    out += '"'; out += s->originalHash; out += ':'; out += s->wgslSHA256; out += '"';
}
std::uint64_t psoRecordSkipped = 0;
void RecordCompletedRenderPipeline(const RenderPipelineRequest& request) {
    if (!psoRecordEnabled.load(std::memory_order_relaxed)) return;
    const auto& d = request.descriptor;
    auto skip = [](const char* why) {
        if (psoRecordSkipped++ < 32) std::fprintf(stderr, "CP2077 warmup record skipped: %s\n", why);
    };
    auto vs = graphicsModuleSources.find(d.vertex.module);
    if (vs == graphicsModuleSources.end()) return skip("vertex module without converted source");
    const cp2077::web::ConvertedGraphicsShader* fs = nullptr;
    if (d.fragment) {
        auto found = graphicsModuleSources.find(d.fragment->module);
        if (found == graphicsModuleSources.end()) return skip("fragment module without converted source");
        fs = found->second;
        if (!EntryIsMain(d.fragment->entryPoint)) return skip("fragment entry point");
    }
    if (!EntryIsMain(d.vertex.entryPoint)) return skip("vertex entry point");
    int lay = 0;
    if (d.layout) {
        const cp2077::web::ConvertedGraphicsShader *lv = nullptr, *lp = nullptr;
        if (!ExplicitLayoutPairOf(d.layout, lv, lp) || lv != vs->second || lp != fs) return skip("layout not an explicit pair layout");
        lay = 1;
    }
    std::vector<std::uint32_t> words;
    if (!SerializeRenderState(d, words)) return skip("state value out of range");
    std::string text = "{\"vs\":"; AppendShaderIdentity(text, vs->second);
    text += ",\"fs\":"; if (fs) AppendShaderIdentity(text, fs); else text += "null";
    text += ",\"lay\":"; text += char('0' + lay); text += ",\"w\":\"";
    char hex[9];
    for (auto v : words) { std::snprintf(hex, sizeof(hex), "%08x", v); text += hex; }
    text += "\"}";
    PushPsoRecord(text.c_str());
}

// ---- replay ----
const cp2077::web::ConvertedGraphicsShader* FindGraphicsShaderByIdentity(const std::string& identity) {
    using namespace cp2077::web;
    const auto colon = identity.find(':'); if (colon == std::string::npos) return nullptr;
    const std::string hash = identity.substr(0, colon), sha = identity.substr(colon + 1);
    auto match = [&](const ConvertedGraphicsShader* s) {
        return s && hash == s->originalHash && sha == s->wgslSHA256;
    };
    auto scan = [&](const ConvertedGraphicsShader* begin, const ConvertedGraphicsShader* end) -> const ConvertedGraphicsShader* {
        for (auto* s = begin; s != end; ++s) {
            if (hash != s->originalHash) continue;
            if (match(s)) return s;
            for (auto* v : {FindConvertedGraphicsGiRGBOutputVariant(s), FindConvertedGraphicsDepthGatherVariant(s),
                            FindConvertedGraphicsLogicXorVariant(s)})
                if (match(v)) return v;
        }
        return nullptr;
    };
    if (auto* s = scan(std::begin(kConvertedMaterialGraphicsShaders), std::end(kConvertedMaterialGraphicsShaders))) return s;
    if (auto* s = scan(std::begin(kConvertedGraphicsShaders), std::end(kConvertedGraphicsShaders))) return s;
    return scan(std::begin(kConvertedStaticGraphicsShaders), std::end(kConvertedStaticGraphicsShaders));
}
// Worker: sync XHR of the manifest, converted to "vs fs lay words" lines. Returns byte length (0 = none).
EM_JS(int, FetchWarmManifest, (), {
    try {
        const x = new XMLHttpRequest();
        x.open('GET', location.origin + '/runtime/pipeline-warmup.json', false); x.send();
        if (x.status !== 200) return 0;
        const m = JSON.parse(x.responseText);
        const row = e => [e.vs, e.fs || '-', e.lay ? 1 : 0, e.w].join(' ');
        // [boot] m.session: pipelines first met in a game session (Nomad garage / mirror scene). They are warmed
        // only once the first session is active (see RunPipelineWarmup), never during base initialization.
        let text = (m.render || []).map(row).join('\n');
        if ((m.session || []).length) text += '\n#session\n' + m.session.map(row).join('\n');
        globalThis.__cp2077WarmBytes = new TextEncoder().encode(text);
        return globalThis.__cp2077WarmBytes.length;
    } catch (e) { return 0; }
});
EM_JS(void, CopyWarmManifest, (char* out), {
    if (typeof growMemViews === 'function') growMemViews();
    HEAPU8.set(globalThis.__cp2077WarmBytes, Number(out)); globalThis.__cp2077WarmBytes = null;
});
// Main thread: register the key as pending (draws join it) and start creation, or drop when already known.
void BeginWarmRenderPipeline(void* argument) {
    auto* request = static_cast<RenderPipelineRequest*>(argument);
    request->context = nullptr;
    request->device = cp2077::web::MainThreadGpuDevice();
    bool start = false;
    if (request->device) {
        request->deviceGeneration = cp2077::web::GpuDeviceGeneration();
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        SelectPipelineCacheDevice(request->device);
        BuildRenderPipelineKey(request->cacheKey, request->device, request->deviceGeneration, request->descriptor);
        request->cacheHash = HashRenderPipelineKey(request->cacheKey);
        if (!FindCachedRenderPipeline(request->cacheKey, request->cacheHash) &&
            !failedRenderPipelines.count(request->cacheHash) && !pendingRenderPipelines.count(request->cacheKey)) {
            pendingRenderPipelines.emplace(request->cacheKey, std::vector<RenderPipelineRequest*>{});
            request->compileStarted = emscripten_get_now(); start = true;
        }
    }
    if (!start) {
        wgpuShaderModuleRelease(request->descriptor.vertex.module);
        if (request->descriptor.fragment) wgpuShaderModuleRelease(request->fragment.module);
        if (request->descriptor.layout) wgpuPipelineLayoutRelease(request->descriptor.layout);
        delete request;
        warmSkipped.fetch_add(1); warmInFlight.fetch_sub(1); PublishWarmStatsOnMain(0);
        return;
    }
    warmStarted.fetch_add(1);
    wgpuDeviceAddRef(request->device);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Validation);
    WGPUCreateRenderPipelineAsyncCallbackInfo pipeline = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
    pipeline.mode = WGPUCallbackMode_AllowSpontaneous;
    pipeline.callback = RenderPipelineCreated; pipeline.userdata1 = request;
    wgpuDeviceCreateRenderPipelineAsync(request->device, &request->descriptor, pipeline);
    WGPUPopErrorScopeCallbackInfo scopes = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    scopes.mode = WGPUCallbackMode_AllowSpontaneous;
    scopes.callback = RenderScopeChecked; scopes.userdata1 = request;
    for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(request->device, scopes);
}
std::vector<std::uint32_t> ParseHexWords(const std::string& hex) {
    std::vector<std::uint32_t> w;
    if (hex.size() % 8) return w;
    for (std::size_t i = 0; i < hex.size(); i += 8) w.push_back(std::uint32_t(std::strtoul(hex.substr(i, 8).c_str(), nullptr, 16)));
    return w;
}
void* RunPipelineWarmup(void*) {
    using namespace cp2077::web;
    const double t0 = emscripten_get_now();
    const int bytes = FetchWarmManifest();
    std::string text(std::size_t(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) CopyWarmManifest(text.data());
    struct Line { std::string vs, fs, words; int lay = 0; int phase = 0; };
    std::vector<Line> lines;
    int phase = 0;
    for (std::size_t at = 0; at < text.size();) {
        std::size_t end = text.find('\n', at); if (end == std::string::npos) end = text.size();
        const std::string row = text.substr(at, end - at); at = end + 1;
        if (row == "#session") { phase = 1; continue; }
        char vs[160] = {}, fs[160] = {}; int lay = 0;
        static thread_local std::string words; words.assign(row.size(), '\0');
        if (std::sscanf(row.c_str(), "%159s %159s %d %s", vs, fs, &lay, words.data()) != 4) continue;
        lines.push_back({vs, fs, std::string(words.c_str()), lay, phase});
    }
    warmTotal.store(lines.size());
    auto* queue = ShaderQueue();
    std::unordered_map<std::string, WGPUShaderModule> modules; // warm thread's references (released at the end)
    auto moduleFor = [&](const ConvertedGraphicsShader* source) -> WGPUShaderModule {
        auto found = modules.find(source->wgslSHA256);
        if (found != modules.end()) return found->second;
        auto module = CreateConvertedGraphicsModule(source);
        modules.emplace(source->wgslSHA256, module); return module;
    };
    bool sessionPhaseChecked = false;
    for (const auto& line : lines) {
        if (line.phase == 1 && !sessionPhaseChecked) {
            // [boot] session phase: wait (warm thread only) until the first game session is active. Off for
            // ?sandbox* links (their worlds do not use these pipelines) and with ?psoWarmSession=0.
            sessionPhaseChecked = true;
            int go = 0;
            for (int i = 0; i < 1200 && go == 0; ++i) { // <= 10 min
                go = MAIN_THREAD_EM_ASM_INT({
                    try {
                        const q = new URLSearchParams(location.search);
                        if (q.get('psoWarmSession') === '0' || (q.get('sandbox') !== null && q.get('sandbox') !== '0') || q.has('sandboxWorld') || q.has('sandboxGamedef')) return -1;
                        const m = globalThis.cp2077EngineModule || Module;
                        const rows = m.cp2077NativeLoadingStageDiagnostics || [];
                        // rows is a 64-entry ring (oldest shifted out): scan all of it on every poll.
                        return rows.some(r => /phase=active class=gsmState_SessionActive/.test((r && r.message) || '')) ? 1 : 0;
                    } catch (e) { return -1; }
                });
                if (go == 0) usleep(500000);
            }
            std::fprintf(stderr, "CP2077 [boot] warmup session phase: %s after %.0f ms (%llu done so far)\n",
                go == 1 ? "start" : "skipped", emscripten_get_now() - t0, (unsigned long long)warmDone.load());
            if (go != 1) { warmSkipped.fetch_add(std::uint64_t(&lines.back() - &line) + 1); break; }
        }
        if (!IsGpuDeviceReady()) break;
        const auto* vs = FindGraphicsShaderByIdentity(line.vs);
        const auto* fs = line.fs == "-" ? nullptr : FindGraphicsShaderByIdentity(line.fs);
        if (!vs || (line.fs != "-" && !fs)) { warmMissing.fetch_add(1); continue; }
        auto request = std::make_unique<RenderPipelineRequest>();
        if (!DeserializeRenderState(ParseHexWords(line.words), *request) || bool(request->descriptor.fragment) != bool(fs)) {
            warmMissing.fetch_add(1); continue;
        }
        if (GpuLacksTier1()) { // [notier1] recorded with tier1: storage targets of these formats are created as other formats
            bool remapped = false;
            for (const auto& target : request->targets)
                remapped = remapped || Tier1CoreFormat(target.format, WGPUTextureUsage_StorageBinding | WGPUTextureUsage_RenderAttachment) != target.format;
            if (remapped) { warmSkipped.fetch_add(1); continue; }
        }
        WGPUShaderModule vm = moduleFor(vs), fm = fs ? moduleFor(fs) : nullptr;
        if (!vm || (fs && !fm)) { warmMissing.fetch_add(1); continue; }
        request->descriptor.vertex.module = vm;
        if (fs) request->fragment.module = fm;
        if (line.lay) {
            GpuGraphicsPipelineLayout layout;
            if (!AcquireGpuGraphicsPipelineLayout(vs, fs, layout)) { warmLayoutMiss.fetch_add(1); continue; }
            request->descriptor.layout = layout.layout;
            if (layout.vertexModule) { vm = layout.vertexModule; request->descriptor.vertex.module = vm; } // [nomadperf] ovfStorage
            if (fs && layout.fragmentModule) { fm = layout.fragmentModule; request->fragment.module = fm; }
        }
        while (warmInFlight.load() >= warmK.load()) usleep(2000);
        wgpuShaderModuleAddRef(vm); if (fm) wgpuShaderModuleAddRef(fm);
        if (request->descriptor.layout) wgpuPipelineLayoutAddRef(request->descriptor.layout);
        request->owned = true; request->warm = true; request->label = "warmup";
        request->descriptor.label = {request->label.data(), request->label.size()};
        warmInFlight.fetch_add(1);
        if (!queue || !emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), BeginWarmRenderPipeline, request.get())) {
            warmInFlight.fetch_sub(1); break; // references leak (released on main only); stop warming
        }
        request.release();
    }
    while (warmInFlight.load() > 0) usleep(5000);
    for (auto& item : modules) if (item.second) ReleaseGpuGraphicsModule(item.second);
    std::fprintf(stderr, "CP2077 warmup done: total=%llu started=%llu done=%llu failed=%llu skipped=%llu missing=%llu layoutMiss=%llu k=%d ms=%.0f\n",
        (unsigned long long)warmTotal.load(), (unsigned long long)warmStarted.load(), (unsigned long long)warmDone.load(),
        (unsigned long long)warmFailed.load(), (unsigned long long)warmSkipped.load(), (unsigned long long)warmMissing.load(),
        (unsigned long long)warmLayoutMiss.load(), warmK.load(), emscripten_get_now() - t0);
    if (queue) emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), PublishWarmFinishedOnMain, nullptr);
    return nullptr;
}
}
namespace cp2077::web {
// Worker: called after the first graphics module creation returned (switches were read on main by then).
void MaybeStartPipelineWarmup() {
    static std::atomic<int> started{0};
    if (started.load(std::memory_order_relaxed) || !warmSwitchesRead.load(std::memory_order_acquire)) return;
    if (started.exchange(1)) return;
    if (!warmEnabled.load()) { std::fputs("CP2077 warmup disabled (?psoWarm=0)\n", stderr); return; }
    pthread_t thread;
    if (pthread_create(&thread, nullptr, RunPipelineWarmup, nullptr) == 0) pthread_detach(thread);
    else std::fputs("CP2077 warmup thread could not start\n", stderr);
}
}
