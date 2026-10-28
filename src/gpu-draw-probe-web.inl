// [nailfix] Included by gpu-api-graphics-draw-web.inl inside cp2077::web.
// (1) Opt-in per-draw probe (?drawprobe=1): one stderr line per unique (VS, PS, native alpha-to-coverage), at most
//     600: shader hashes, target/depth/blend/cull state, native and emulated A2C and every bound pixel SRV slot as
//     slot:TEXFMT:WxHxD:mips(:null). Off: one static int test per draw.
// (2) Single-sample alpha-to-coverage emulation (default ON, ?a2c=0 = old behaviour). D3D12 accepts A2C on a
//     1-sample target, where the single coverage bit is set when SV_Target0.a >= 0.5 (a cutout); WebGPU only allows
//     A2C with multisampling, so om-state drops it and foliage/fence cards drew as solid quads. The variant module
//     is the original converted WGSL with one test before the entry point's return:
//         if (<output0>.w < 0.5f) { discard; }
//     Resources, bindings and every other instruction are unchanged.
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <cstring>
static int CpDrawProbeMode() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('drawprobe');
            return (q && q !== '0' && q !== 'false') ? 1 : 0;
        });
        std::fprintf(stderr, "CP2077 draw probe: mode=%d\n", v);
        return v;
    }();
    return mode;
}
static int CpAlphaToCoverageEmulation() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('a2c');
            return (q === '0' || q === 'false') ? 0 : 1;
        });
        std::fprintf(stderr, "CP2077 nailfix switches: a2c=%d (1-sample alpha-to-coverage cutout emulation)\n", v);
        return v;
    }();
    return mode;
}
// Builds the cutout WGSL text; empty when the fragment entry point does not have the expected naga shape
// (struct FragmentOutput whose first member is @location(0) vec4<f32>, one "return FragmentOutput(" in main).
static std::string CpAlphaToCoverageWgsl(const ConvertedGraphicsShader* pixel) {
    if (!pixel || !pixel->wgsl || !pixel->wgslBytes) return {};
    std::string text(pixel->wgsl, pixel->wgslBytes);
    const auto st = text.find("struct FragmentOutput {");
    if (st == std::string::npos) return {};
    const auto firstMember = text.find("@location(", st);
    const auto structEnd = text.find('}', st);
    if (firstMember == std::string::npos || firstMember > structEnd || text.compare(firstMember, 12, "@location(0)") != 0) return {};
    const auto memberEnd = text.find(',', firstMember);
    if (memberEnd == std::string::npos || text.substr(firstMember, memberEnd - firstMember).find("vec4<f32>") == std::string::npos) return {};
    const auto entry = text.find("@fragment");
    if (entry == std::string::npos) return {};
    const std::string marker = "return FragmentOutput(";
    const auto ret = text.find(marker, entry);
    if (ret == std::string::npos || text.find(marker, ret + 1) != std::string::npos) return {};
    const auto argStart = ret + marker.size();
    const auto argEnd = text.find_first_of(",)", argStart);
    if (argEnd == std::string::npos) return {};
    std::string arg = text.substr(argStart, argEnd - argStart);
    while (!arg.empty() && arg.back() == ' ') arg.pop_back();
    while (!arg.empty() && arg.front() == ' ') arg.erase(arg.begin());
    if (arg.empty() || arg.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) return {};
    text.insert(ret, "if (" + arg + ".w < 0.5f) { discard; }\n    ");
    return text;
}
// [varexplicit] variant module -> its ConvertedGraphicsShader copy (modified WGSL, same bindings as the pixel shader).
static std::mutex& CpVariantSourceMutex() { static std::mutex m; return m; }
static std::unordered_map<WGPUShaderModule, const ConvertedGraphicsShader*>& CpVariantSources() { static std::unordered_map<WGPUShaderModule, const ConvertedGraphicsShader*> m; return m; }
static const ConvertedGraphicsShader* CpVariantSource(WGPUShaderModule module) {
    std::lock_guard<std::mutex> guard(CpVariantSourceMutex()); auto it = CpVariantSources().find(module); return it == CpVariantSources().end() ? nullptr : it->second;
}
static WGPUShaderModule CpAlphaToCoverageModule(const ConvertedGraphicsShader* pixel) {
    struct Variant { ConvertedGraphicsShader* source = nullptr; std::string wgsl; WGPUShaderModule module = nullptr; bool failed = false; };
    static std::mutex s_mutex;
    static std::unordered_map<const ConvertedGraphicsShader*, Variant*> s_variants;
    Variant* variant = nullptr;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        auto& slot = s_variants[pixel];
        if (!slot) slot = new Variant(); // device lifetime, never freed (like the static package)
        variant = slot;
        if (variant->module || variant->failed) return variant->module;
        if (!variant->source) {
            variant->wgsl = CpAlphaToCoverageWgsl(pixel);
            if (variant->wgsl.empty()) { variant->failed = true; return nullptr; }
            variant->source = new ConvertedGraphicsShader(*pixel);
            variant->source->wgsl = variant->wgsl.c_str(); variant->source->wgslBytes = variant->wgsl.size();
        }
    }
    WGPUShaderModule module = CreateConvertedGraphicsModule(variant->source);
    std::lock_guard<std::mutex> guard(s_mutex);
    if (!variant->module) { variant->module = module; variant->failed = module == nullptr; }
    if (variant->module && variant->source) { std::lock_guard<std::mutex> g(CpVariantSourceMutex()); CpVariantSources()[variant->module] = variant->source; }
    else if (module) ReleaseGpuGraphicsModule(module);
    static unsigned reports = 0;
    if (reports++ < 24) std::fprintf(stderr, "CP2077 nailfix: alpha-to-coverage cutout variant PS=%s module=%d\n",
        pixel->originalHash ? pixel->originalHash : "?", int(variant->module != nullptr));
    return variant->module;
}
// [whitewall] D3D dual-source blending: the original PS writes SV_Target0 and SV_Target1 while the OM state blends
// the single render target with Src1 factors. naga emits two outputs at @location(0) and @location(1); WebGPU wants
// both at @location(0) with @blend_src(0)/@blend_src(1) plus 'enable dual_source_blending;'. Only the two output
// attributes change; every instruction, resource and binding is the original converted WGSL.
static std::string CpDualSourceWgsl(const ConvertedGraphicsShader* pixel) {
    if (!pixel || !pixel->wgsl || !pixel->wgslBytes) return {};
    std::string text(pixel->wgsl, pixel->wgslBytes);
    const auto st = text.find("struct FragmentOutput {");
    if (st == std::string::npos || text.find("struct FragmentOutput {", st + 1) != std::string::npos) return {};
    const auto end = text.find('}', st);
    if (end == std::string::npos) return {};
    std::string body = text.substr(st, end - st);
    // Exactly two @location members (0 and 1), both vec4<f32>; builtins (e.g. frag_depth) are left as they are.
    std::size_t count = 0, at = 0;
    while ((at = body.find("@location(", at)) != std::string::npos) { ++count; at += 10; }
    const auto l0 = body.find("@location(0) "), l1 = body.find("@location(1) ");
    if (count != 2 || l0 == std::string::npos || l1 == std::string::npos) return {};
    const auto e0 = body.find(',', l0), e1 = body.find(',', l1);
    if (e0 == std::string::npos || e1 == std::string::npos ||
        body.substr(l0, e0 - l0).find(": vec4<f32>") == std::string::npos ||
        body.substr(l1, e1 - l1).find(": vec4<f32>") == std::string::npos) return {};
    body.replace(l1, 13, "@location(0) @blend_src(1) ");
    body.replace(l0, 13, "@location(0) @blend_src(0) ");
    text.replace(st, end - st, body);
    return "enable dual_source_blending;\n" + text;
}
static WGPUShaderModule CpDualSourceModule(const ConvertedGraphicsShader* pixel) {
    struct Variant { ConvertedGraphicsShader* source = nullptr; std::string wgsl; WGPUShaderModule module = nullptr; bool failed = false; };
    static std::mutex s_mutex;
    static std::unordered_map<const ConvertedGraphicsShader*, Variant*> s_variants;
    Variant* variant = nullptr;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        auto& slot = s_variants[pixel];
        if (!slot) slot = new Variant(); // device lifetime, never freed (like the static package)
        variant = slot;
        if (variant->module || variant->failed) return variant->module;
        if (!variant->source) {
            variant->wgsl = CpDualSourceWgsl(pixel);
            if (variant->wgsl.empty()) {
                variant->failed = true;
                std::fprintf(stderr, "CP2077 whitewall: dual-source variant unavailable (output shape) PS=%s\n", pixel && pixel->originalHash ? pixel->originalHash : "?");
                return nullptr;
            }
            variant->source = new ConvertedGraphicsShader(*pixel);
            variant->source->wgsl = variant->wgsl.c_str(); variant->source->wgslBytes = variant->wgsl.size();
        }
    }
    WGPUShaderModule module = CreateConvertedGraphicsModule(variant->source);
    std::lock_guard<std::mutex> guard(s_mutex);
    if (!variant->module) { variant->module = module; variant->failed = module == nullptr; }
    if (variant->module && variant->source) { std::lock_guard<std::mutex> g(CpVariantSourceMutex()); CpVariantSources()[variant->module] = variant->source; }
    else if (module) ReleaseGpuGraphicsModule(module);
    static unsigned reports = 0;
    if (reports++ < 48) std::fprintf(stderr, "CP2077 whitewall: dual-source blend variant PS=%s module=%d\n",
        pixel->originalHash ? pixel->originalHash : "?", int(variant->module != nullptr));
    return variant->module;
}
// [whitewall] r5 diagnostic: ?texdebug=1 binds the zero view for tiny (<= 4x4) pixel material textures.
static int CpTinyTextureDebug() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('texdebug');
            return (q && q !== '0' && q !== 'false') ? 1 : 0;
        });
        std::fprintf(stderr, "CP2077 whitewall switches: texdebug=%d\n", v);
        return v;
    }();
    return mode;
}
// [whitewall] r5: ?nullrt=0 = old behaviour (draws into a setup with an empty color slot are skipped).
static int CpNullRenderTargetSlotsAllowed() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('nullrt');
            return (q === '0' || q === 'false') ? 0 : 1;
        });
        std::fprintf(stderr, "CP2077 whitewall switches: nullrt=%d\n", v);
        return v;
    }();
    return mode;
}
// [whitewall] ?dualsrc=0 also disables the multi-target split (old behaviour: those draws are rejected).
static int CpDualSourceSplitEnabled() {
    static const int mode = [] {
        return MAIN_THREAD_EM_ASM_INT({
            const q = new URLSearchParams(location.search).get('dualsrc');
            return (q === '0' || q === 'false') ? 0 : 1;
        });
    }();
    return mode;
}
// [whitewall] Rewrites target 0's blend for one phase of the multi-target dual-source split. Valid only for
// result = o0*S + D*F(o1) per component: op add, S in {zero, one, src*, constant*}, F in {src1*, zero, one, constant*}.
// Phase 1: src = SV_Target1 (swapped module), blend (zero, F') with F' = F with src1 -> src.  Phase 2: (S, one).
// Other targets are not written in either phase; phase 1 also leaves depth and stencil unwritten.
static bool CpDualSourceSplitBlend(GpuOMPipelineState& om, int phase) {
    if (!om.blendEnabled[0]) return false;
    auto srcOk = [](WGPUBlendFactor f) {
        return f == WGPUBlendFactor_Zero || f == WGPUBlendFactor_One || f == WGPUBlendFactor_Src ||
            f == WGPUBlendFactor_OneMinusSrc || f == WGPUBlendFactor_SrcAlpha || f == WGPUBlendFactor_OneMinusSrcAlpha ||
            f == WGPUBlendFactor_Constant || f == WGPUBlendFactor_OneMinusConstant;
    };
    auto dstMap = [](WGPUBlendFactor f, WGPUBlendFactor& out) {
        switch (f) {
        case WGPUBlendFactor_Src1: out = WGPUBlendFactor_Src; return true;
        case WGPUBlendFactor_OneMinusSrc1: out = WGPUBlendFactor_OneMinusSrc; return true;
        case WGPUBlendFactor_Src1Alpha: out = WGPUBlendFactor_SrcAlpha; return true;
        case WGPUBlendFactor_OneMinusSrc1Alpha: out = WGPUBlendFactor_OneMinusSrcAlpha; return true;
        case WGPUBlendFactor_Zero: case WGPUBlendFactor_One: case WGPUBlendFactor_Constant: case WGPUBlendFactor_OneMinusConstant:
            out = f; return true;
        default: return false;
        }
    };
    WGPUBlendComponent* parts[2] = {&om.blends[0].color, &om.blends[0].alpha};
    WGPUBlendFactor mapped[2];
    for (int k = 0; k < 2; ++k) {
        if (parts[k]->operation != WGPUBlendOperation_Add || !srcOk(parts[k]->srcFactor) || !dstMap(parts[k]->dstFactor, mapped[k]))
            return false;
    }
    for (int k = 0; k < 2; ++k) {
        if (phase == 1) { parts[k]->srcFactor = WGPUBlendFactor_Zero; parts[k]->dstFactor = mapped[k]; }
        else parts[k]->dstFactor = WGPUBlendFactor_One;
    }
    for (unsigned i = 1; i < om.writeMasks.size(); ++i) { om.writeMasks[i] = WGPUColorWriteMask_None; om.blendEnabled[i] = false; }
    if (phase == 1 && om.hasDepthStencil) {
        om.depthStencil.depthWriteEnabled = WGPUOptionalBool_False;
        om.depthStencil.stencilWriteMask = 0;
    }
    om.dualSource = false; om.dualSourceMultiTarget = false;
    return true;
}
// [whitewall] Phase-1 module of the split: swaps the two output locations (SV_Target1 -> location 0, SV_Target0 ->
// location 1, whose target is write-masked). Instructions, resources and bindings unchanged.
static WGPUShaderModule CpDualSourceSwapModule(const ConvertedGraphicsShader* pixel) {
    struct Variant { ConvertedGraphicsShader* source = nullptr; std::string wgsl; WGPUShaderModule module = nullptr; bool failed = false; };
    static std::mutex s_mutex;
    static std::unordered_map<const ConvertedGraphicsShader*, Variant*> s_variants;
    Variant* variant = nullptr;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        auto& slot = s_variants[pixel];
        if (!slot) slot = new Variant();
        variant = slot;
        if (variant->module || variant->failed) return variant->module;
        if (!variant->source) {
            // Reuse the dual-source shape check, then swap the two location attributes in the original text.
            if (CpDualSourceWgsl(pixel).empty()) { variant->failed = true; return nullptr; }
            std::string text(pixel->wgsl, pixel->wgslBytes);
            const auto st = text.find("struct FragmentOutput {");
            const auto end = text.find('}', st);
            std::string body = text.substr(st, end - st);
            const auto l0 = body.find("@location(0) "), l1 = body.find("@location(1) ");
            body.replace(l1, 13, "@location(0) ");
            body.replace(l0, 13, "@location(1) ");
            text.replace(st, end - st, body);
            variant->wgsl = std::move(text);
            variant->source = new ConvertedGraphicsShader(*pixel);
            variant->source->wgsl = variant->wgsl.c_str(); variant->source->wgslBytes = variant->wgsl.size();
        }
    }
    WGPUShaderModule module = CreateConvertedGraphicsModule(variant->source);
    std::lock_guard<std::mutex> guard(s_mutex);
    if (!variant->module) { variant->module = module; variant->failed = module == nullptr; }
    if (variant->module && variant->source) { std::lock_guard<std::mutex> g(CpVariantSourceMutex()); CpVariantSources()[variant->module] = variant->source; }
    else if (module) ReleaseGpuGraphicsModule(module);
    static unsigned reports = 0;
    if (reports++ < 24) std::fprintf(stderr, "CP2077 whitewall: dual-source split variant PS=%s module=%d\n",
        pixel->originalHash ? pixel->originalHash : "?", int(variant->module != nullptr));
    return variant->module;
}
// [nailfix] Diagnostic A/B only (?stWind=0): speedtree vertex shaders read their per-vertex wind/leaf data from
// EXTRA_DATA[0..2]; this variant feeds zeros there (all other inputs and instructions unchanged) so a run can tell
// whether the shredded close-up foliage comes from the wind/leaf-motion path. Default off.
static int CpSpeedTreeWindOff() {
    static const int mode = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('stWind') === '0' ? 1 : 0; });
        std::fprintf(stderr, "CP2077 nailfix switches: stWind=%d\n", v ? 0 : 1);
        return v;
    }();
    return mode;
}
static WGPUShaderModule CpSpeedTreeNoWindModule(const ConvertedGraphicsShader* vertex) {
    struct Variant { ConvertedGraphicsShader* source = nullptr; std::string wgsl; WGPUShaderModule module = nullptr; bool failed = false; };
    static std::mutex s_mutex;
    static std::unordered_map<const ConvertedGraphicsShader*, Variant*> s_variants;
    Variant* variant = nullptr;
    {
        std::lock_guard<std::mutex> guard(s_mutex);
        auto& slot = s_variants[vertex];
        if (!slot) slot = new Variant();
        variant = slot;
        if (variant->module || variant->failed) return variant->module;
        if (!variant->source) {
            std::string text(vertex->wgsl, vertex->wgslBytes);
            int replaced = 0;
            for (const char* name : {"EXTRA_DATA_1 = EXTRA_DATA;", "EXTRA_DATA_1_1 = EXTRA_DATA_1_;", "EXTRA_DATA_2_1 = EXTRA_DATA_2_;"}) {
                const auto at = text.find(name);
                if (at == std::string::npos) continue;
                const std::string lhs = std::string(name).substr(0, std::string(name).find(" ="));
                text.replace(at, std::strlen(name), lhs + " = vec4<f32>(0.0f);");
                ++replaced;
            }
            if (!replaced) { variant->failed = true; return nullptr; }
            variant->wgsl = std::move(text);
            variant->source = new ConvertedGraphicsShader(*vertex);
            variant->source->wgsl = variant->wgsl.c_str(); variant->source->wgslBytes = variant->wgsl.size();
        }
    }
    WGPUShaderModule module = CreateConvertedGraphicsModule(variant->source);
    std::lock_guard<std::mutex> guard(s_mutex);
    if (!variant->module) { variant->module = module; variant->failed = module == nullptr; }
    if (variant->module && variant->source) { std::lock_guard<std::mutex> g(CpVariantSourceMutex()); CpVariantSources()[variant->module] = variant->source; }
    else if (module) ReleaseGpuGraphicsModule(module);
    return variant->module;
}
static void CpDrawProbe(const CommandEntry& command, const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                        const GpuOMPipelineState& om, unsigned samples, Uint32 count, Uint32 instances,
                        bool nativeA2C, bool a2cVariant) {
    if (!CpDrawProbeMode()) return;
    const char* vs = vertex && vertex->originalHash ? vertex->originalHash : "-";
    const char* ps = pixel && pixel->originalHash ? pixel->originalHash : "-";
    // [whitewall] Armed dump: in the page console set globalThis.cp2077DrawProbeArm = N (e.g. 4000) while the view
    // of interest is on screen; the next N draws are logged WITHOUT the per-pair dedupe as 'CP2077 draw probe armed:'.
    // Polled every 256 probe calls (only with ?drawprobe=1).
    static std::atomic<unsigned> s_calls{0};
    static std::atomic<int> s_armed{0};
    if ((s_calls.fetch_add(1) & 255u) == 0) {
        const int arm = MAIN_THREAD_EM_ASM_INT({
            const n = Number(globalThis.cp2077DrawProbeArm || 0); globalThis.cp2077DrawProbeArm = 0;
            return (n > 0 && n < 1000000) ? n : 0;
        });
        if (arm > 0) { s_armed.store(arm); std::fprintf(stderr, "CP2077 draw probe armed: start n=%d\n", arm); }
    }
    bool armedDraw = false;
    if (s_armed.load(std::memory_order_relaxed) > 0) armedDraw = s_armed.fetch_sub(1) > 0;
    if (!armedDraw) {
        static std::mutex s_mutex; static std::set<std::string> s_seen;
        std::lock_guard<std::mutex> guard(s_mutex);
        if (s_seen.size() >= 600 || !s_seen.insert(std::string(vs) + "/" + ps + (nativeA2C ? "/a2c" : "")).second) return;
    }
    char textures[1024]; int used = 0; textures[0] = 0;
    for (unsigned slot = 0; slot < command.graphicsTextures[1].size() && used < 960; ++slot) {
        const auto& t = command.graphicsTextures[1][slot];
        if (t.reference.isNull() && !t.view) continue;
        if (t.reference.isNull()) { used += std::snprintf(textures + used, sizeof(textures) - used, " %u:view", slot); continue; }
        const auto desc = GpuApi::GetTextureDesc(t.reference);
        used += std::snprintf(textures + used, sizeof(textures) - used, " %u:%u:%ux%ux%u:%u%s", slot, unsigned(desc.format),
            unsigned(desc.width), unsigned(desc.height), unsigned(desc.depthOrSliceNum), unsigned(desc.Internal_CalcTargetNumMips()),
            t.nativeNull ? ":null" : "");
    }
    std::fprintf(stderr, "CP2077 draw probe%s: VS=%s PS=%s targets=%u fmt0=%u depth=%u cmp=%u zwrite=%u cull=%u blend0=%u nativeA2C=%u a2cVariant=%u samples=%u count=%u inst=%u tex=[%s ]\n",
        armedDraw ? " armed" : "", vs, ps, unsigned(command.colorAttachmentCount), command.colorAttachmentCount ? unsigned(command.colorAttachments[0].format) : 0u,
        unsigned(om.hasDepthStencil ? om.depthStencil.format : 0), unsigned(om.hasDepthStencil ? om.depthStencil.depthCompare : 0),
        unsigned(om.hasDepthStencil && om.depthStencil.depthWriteEnabled == WGPUOptionalBool_True), unsigned(om.primitive.cullMode),
        unsigned(om.blendEnabled[0]), unsigned(nativeA2C), unsigned(a2cVariant), samples, count, instances, textures);
}
