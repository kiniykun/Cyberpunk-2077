#include "gpu-shader-web.h"
#include "gpu-device-web.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/emscripten.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <map>
#include <tuple>
#include <algorithm>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <condition_variable>
#include <pthread.h>
#include <unistd.h>

namespace cp2077::web {
#include "gpu-shader-package.inc"
#include "gpu-ui-graphics-package.inc"
#include "gpu-static-graphics-package.inc"
#include "gpu-material-graphics-package.inc"
#include "gpu-logic-xor-variant.inc"
#include "gpu-depth-gather-variant.inc"
// Pins made before the 03:03 graphics package regen (signed-integer lowering) -> regenerated WGSL SHA. The
// regen only changed u32(f)/f32(u) casts to their signed forms; for these shaders the affected values are
// never negative (frag coords - 0.5, saturated values, positive pixel indices), so the hand-written variants
// and exact workarounds keyed on the old text remain equivalent.
bool GraphicsWgslHashMatches(const ConvertedGraphicsShader* shader, const char* pinned) {
    if (!shader || !shader->wgslSHA256 || !pinned) return false;
    if (std::strcmp(shader->wgslSHA256, pinned) == 0) return true;
    static const char* const kAliases[][2] = {
        {"5331e5512b792835996ce7c7725a08907796087c58438755d7386492da3d82e5","9355e260d82ce07e924e7bebc0bf5af4da3fae1756113bc2cce14d50c1a6c367"}, // 10796363125667703835
        // stencil_component_lowering.py (stencil SRV .y -> .x): only the stencil read changed; the depth-gather
        // variant (gpu-depth-gather-variant.inc) carries the same .x rewrite.
        {"5331e5512b792835996ce7c7725a08907796087c58438755d7386492da3d82e5","7f692b7f1ce15e9dd83842a72c4d4449b69779e63ec40757ddbfca54434494ef"}, // 10796363125667703835
        {"dfc09da81a26e060926d5788aea2e25f3117698acae47776519a08ac25fdd382","3622c5151cf4c93d272126fc8254d3a878291e0dda946556dc0481fa6b7f3d44"}, // 12784076727638384549
        {"dafa0e9671832dc5e6124aecc9f2c38dee9e9b896496570b93465033510efa9d","ad3d5b0958c97e8825d2c26106d8bc3596f7d2d27f0d1ad157d5669df80a4383"}, // 16347954073033142652
        {"1c2dada5ae9a4e7aa65a0ee856dbe5026dc3dce5d37e9eaee5d6825f4b73e678","94a0603fd5d9e01a29e460d0ad79afb048343159f1924038b15f56070f7106b5"}, // 18418924690260713418
        {"1b8f4d93f3cd8e130249d929cbe54d41f9026327f7ca49eae4825ac4a05d518a","24a0e362de450c42a26202a7e073a4da32b010b30ee426b6b2f39550a2e7da03"}, // 6862283186064749778
    };
    for (const auto& alias : kAliases)
        if (std::strcmp(pinned, alias[0]) == 0 && std::strcmp(shader->wgslSHA256, alias[1]) == 0) return true;
    return false;
}
namespace {
bool WgslIdentChar(char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; }
const char* WgslSkipSpace(const char* p, const char* end) {
    while (p < end) {
        if (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') { ++p; continue; }
        if (end-p>=2 && p[0]=='/' && p[1]=='/') { while (p<end && *p!='\n') ++p; continue; }
        break;
    }
    return p;
}
// Parses "@location(N)" at p; returns the position after it or nullptr.
const char* WgslParseLocation(const char* p, const char* end, unsigned& location) {
    static const char kLocation[] = "@location";
    const std::size_t n = sizeof(kLocation)-1;
    if (std::size_t(end-p) < n || std::memcmp(p,kLocation,n)!=0) return nullptr;
    p = WgslSkipSpace(p+n,end);
    if (p>=end || *p!='(') return nullptr;
    p = WgslSkipSpace(p+1,end);
    if (p>=end || *p<'0' || *p>'9') return nullptr;
    unsigned value = 0;
    while (p<end && *p>='0' && *p<='9') { value = value*10 + unsigned(*p-'0'); if (value > 64) return nullptr; ++p; }
    p = WgslSkipSpace(p,end);
    if (p>=end || *p!=')') return nullptr;
    location = value; return p+1;
}
std::uint32_t ParseFragmentOutputMask(const char* text, std::size_t bytes) {
    constexpr std::uint32_t kUnknown = ~0u;
    if (!text || !bytes) return kUnknown;
    const char* const end = text + bytes;
    static const char kFragment[] = "@fragment";
    const char* p = std::search(text,end,kFragment,kFragment+sizeof(kFragment)-1);
    if (p == end) return kUnknown;
    p = WgslSkipSpace(p+sizeof(kFragment)-1,end);
    if (end-p<3 || std::memcmp(p,"fn",2)!=0 || WgslIdentChar(p[2])) return kUnknown;
    p = std::find(p,end,'(');
    if (p == end) return kUnknown;
    int depth = 0;
    for (; p<end; ++p) { if (*p=='(') ++depth; else if (*p==')' && --depth==0) break; }
    if (p == end) return kUnknown;
    p = WgslSkipSpace(p+1,end);
    if (p<end && *p=='{') return 0; // no return value: no color outputs
    if (end-p<2 || p[0]!='-' || p[1]!='>') return kUnknown;
    p = WgslSkipSpace(p+2,end);
    if (p<end && *p=='@') {
        unsigned location = 0;
        if (WgslParseLocation(p,end,location)) return location < 32 ? (1u<<location) : kUnknown;
        return 0; // e.g. @builtin(frag_depth)
    }
    const char* name = p;
    while (p<end && WgslIdentChar(*p)) ++p;
    if (p == name) return kUnknown;
    const std::string structName(name, p);
    // Find "struct <name>" followed by '{' and collect its @location members.
    const std::string needle = "struct " + structName;
    for (const char* s = text;;) {
        s = std::search(s,end,needle.begin(),needle.end());
        if (s == end) return kUnknown;
        const char* after = s + needle.size();
        if ((s==text || !WgslIdentChar(s[-1])) && (after==end || !WgslIdentChar(*after))) {
            const char* q = WgslSkipSpace(after,end);
            if (q<end && *q=='{') {
                std::uint32_t mask = 0;
                for (++q; q<end && *q!='}'; ++q) {
                    unsigned location = 0;
                    if (*q=='@') {
                        if (const char* next = WgslParseLocation(q,end,location)) {
                            if (location >= 32) return kUnknown;
                            mask |= 1u<<location; q = next-1;
                        }
                    }
                }
                return q<end ? mask : kUnknown;
            }
        }
        s = after;
    }
}
}
std::uint32_t GraphicsFragmentOutputMask(const ConvertedGraphicsShader* shader) {
    if (!shader || shader->stage != GraphicsShaderStage::Pixel || !shader->wgsl) return ~0u;
    // Converted shader records are static; cache per thread (draws run on several workers) without locking.
    static thread_local std::unordered_map<const ConvertedGraphicsShader*, std::uint32_t> cache;
    auto found = cache.find(shader);
    if (found != cache.end()) return found->second;
    const std::uint32_t mask = ParseFragmentOutputMask(shader->wgsl, shader->wgslBytes);
    cache.emplace(shader, mask);
    return mask;
}
const ConvertedGraphicsShader* FindConvertedGraphicsLogicXorVariant(const ConvertedGraphicsShader* original){
 if(!original || original->stage!=GraphicsShaderStage::Pixel || std::strcmp(original->originalHash,"4452171746949868818")!=0 || std::strcmp(original->wgslSHA256,"ceb2bd487699cebeb1d43b1a8396d9b4aa13db77fccfa32edda9c9f2e1a9655b")!=0)return nullptr;
 static const ConvertedGraphicsShader variant=[original]{auto v=*original;v.wgsl=kLogicXorWGSL;v.wgslBytes=sizeof(kLogicXorWGSL)-1;v.wgslSHA256=kLogicXorVariantHash;return v;}();return &variant;
}
#include "gpu-gi-rgb-output-variant.inc"

const ConvertedGraphicsShader* FindConvertedGraphicsGiRGBOutputVariant(const ConvertedGraphicsShader* original) {
    if (!original || original->stage!=GraphicsShaderStage::Pixel ||
        std::strcmp(original->originalHash,"18418924690260713418")!=0 ||
        !GraphicsWgslHashMatches(original,"1c2dada5ae9a4e7aa65a0ee856dbe5026dc3dce5d37e9eaee5d6825f4b73e678")) return nullptr;
    static const ConvertedGraphicsShader variant=[original] {
        auto v=*original;v.wgsl=kGiRGBOutputWGSL;v.wgslBytes=sizeof(kGiRGBOutputWGSL)-1;
        v.wgslSHA256=kGiRGBOutputHash;return v;
    }();
    return &variant;
}
const ConvertedGraphicsShader* FindConvertedGraphicsDepthGatherVariant(const ConvertedGraphicsShader* original) {
    if (!original || original->stage!=GraphicsShaderStage::Pixel ||
        std::strcmp(original->originalHash,"10796363125667703835")!=0 ||
        !GraphicsWgslHashMatches(original,kDepthGatherOriginalHash)) return nullptr;
    static const ConvertedGraphicsShader variant=[original] {
        auto v=*original; v.wgsl=kDepthGatherWGSL; v.wgslBytes=sizeof(kDepthGatherWGSL)-1;
        v.wgslSHA256=kDepthGatherHash; return v;
    }();
    return &variant;
}

const ConvertedGraphicsShader* FindConvertedGraphicsShaderBytecode(const void* data, std::size_t bytes) {
    if (!data) return nullptr;
    for (const auto& shader : kConvertedMaterialGraphicsShaders)
        if (bytes == shader.nativeBytecodeBytes &&
            std::memcmp(data, shader.nativeBytecode, bytes) == 0) return &shader;
    for (const auto& shader : kConvertedGraphicsShaders)
        if (bytes == shader.nativeBytecodeBytes &&
            std::memcmp(data, shader.nativeBytecode, bytes) == 0) return &shader;
    for (const auto& shader : kConvertedStaticGraphicsShaders)
        if (bytes == shader.nativeBytecodeBytes &&
            std::memcmp(data, shader.nativeBytecode, bytes) == 0) return &shader;
    return nullptr;
}
const ConvertedShader* FindConvertedShader(const char* name) {
    if (!name) return nullptr;
    for (const auto& shader : kConvertedShaders)
        if (std::strcmp(name, shader.name) == 0 ||
            std::strcmp(name, shader.runtimeName) == 0) return &shader;
    return nullptr;
}
const ConvertedShader* FindConvertedShaderBytecode(const void* data, std::size_t bytes) {
    if (!data) return nullptr;
    for (const auto& shader : kConvertedShaders)
        if (bytes == shader.bytecodeIdentityBytes &&
            std::memcmp(data, shader.bytecodeIdentity, bytes) == 0) return &shader;
    return nullptr;
}
}
namespace {
EM_JS(int, BrowserHasWGSLStorageFeature, (int tier1), {
    const features = globalThis.navigator?.gpu?.wgslLanguageFeatures;
    return features?.has(tier1 ? 'texture_formats_tier1' : 'readonly_and_readwrite_storage_textures') ? 1 : 0;
});
em_proxying_queue* ShaderQueue() {
    static auto* queue = em_proxying_queue_create(); return queue;
}
// WGSL requires every enable/requires/diagnostic directive before the first global declaration (Tint: "directives
// must come before all global declarations"). Safety net for packaged modules where a lowering pass put
// declarations above a directive: move the directive lines to the top, keeping their relative order. Returns
// false without copying when the module is already well-formed (the normal case).
void TrimWgslLine(const char*& begin, const char*& end) {
    while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r')) ++begin;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
}
bool IsWgslDirectiveLine(const char* begin, const char* end) {
    TrimWgslLine(begin, end);
    if (end == begin || end[-1] != ';') return false;
    auto keyword = [&](const char* word) {
        size_t length = std::strlen(word);
        return size_t(end - begin) > length && std::memcmp(begin, word, length) == 0 ? begin + length : nullptr;
    };
    if (const char* after = keyword("enable")) return *after == ' ' || *after == '\t';
    if (const char* after = keyword("requires")) return *after == ' ' || *after == '\t';
    if (const char* after = keyword("diagnostic")) {
        while (after < end && (*after == ' ' || *after == '\t')) ++after;
        return after < end && *after == '(';
    }
    return false;
}
bool IsWgslBlankOrCommentLine(const char* begin, const char* end) {
    TrimWgslLine(begin, end);
    return begin == end || (end - begin >= 2 && begin[0] == '/' && begin[1] == '/');
}
// [tankview] WebGPU forbids one `sampler` binding being Filtering (textureSample* on texture_*<f32>) and also
// being used by textureGather on a texture_2d<u32/i32> (needs NonFiltering). DX12 allows it (the rain PS family,
// 14926-byte 1996683356098189253 & co: s8 gathers t36/t48 u32 and samples t8). Integer gathers ignore filtering,
// so for such mixed samplers each `textureGather(0, T, S, uv)` on an integer T becomes four textureLoads with the
// same footprint (base = floor(uv*size-0.5), clamped; x=(i0,j1) y=(i1,j1) z=(i1,j0) w=(i0,j0)).
// Returns false (no change) when no mixed sampler exists. ?gatherLower=0 disables.
bool LowerMixedIntegerGathers(const std::string& text, std::string& out) {
    auto ident = [](char c) { return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'; };
    auto blank = [](char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'; };
    std::map<std::string, std::string> types; // module-scope binding name -> compact type
    for (auto at = text.find("@binding("); at != std::string::npos; at = text.find("@binding(", at + 9)) {
        const auto end = text.find(';', at); if (end == std::string::npos) break;
        auto var = text.find("var", at); if (var == std::string::npos || var > end) continue;
        auto q = var + 3;
        if (q < end && text[q] == '<') { q = text.find('>', q); if (q == std::string::npos || q > end) continue; ++q; }
        const auto colon = text.find(':', q); if (colon == std::string::npos || colon > end) continue;
        std::string name, type;
        for (auto i = q; i < colon; ++i) if (ident(text[i])) name += text[i];
        for (auto i = colon + 1; i < end; ++i) if (!blank(text[i])) type += text[i];
        types[name] = type;
    }
    auto isInt = [&](const std::string& n) { auto f = types.find(n); return f != types.end() && (f->second == "texture_2d<u32>" || f->second == "texture_2d<i32>"); };
    auto isFloat = [&](const std::string& n) {
        auto f = types.find(n);
        return f != types.end() && f->second.compare(0, 8, "texture_") == 0 && f->second.size() > 5 &&
               f->second.compare(f->second.size() - 5, 5, "<f32>") == 0;
    };
    struct Call { std::size_t begin, end; std::string texture, sampler, coords; };
    std::vector<Call> calls; std::set<std::string> floatSamplers;
    auto args = [&](std::size_t open, std::vector<std::string>& list) -> std::size_t {
        int depth = 0; std::size_t begin = open + 1;
        for (std::size_t i = open + 1; i < text.size(); ++i) {
            const char c = text[i];
            if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth) --depth;
            else if ((c == ',' && !depth) || c == ')') {
                std::string a = text.substr(begin, i - begin);
                while (!a.empty() && blank(a.front())) a.erase(0, 1);
                while (!a.empty() && blank(a.back())) a.pop_back();
                list.push_back(a); begin = i + 1;
                if (c == ')') return i + 1;
            }
        }
        return std::string::npos;
    };
    for (std::size_t p = 0; p < text.size();) {
        if (!ident(text[p])) { ++p; continue; }
        std::size_t e = p; while (e < text.size() && ident(text[e])) ++e;
        const std::string name = text.substr(p, e - p);
        const std::size_t start = p; p = e;
        const bool gather = name == "textureGather", sample = name.compare(0, 13, "textureSample") == 0;
        if (!gather && !sample) continue;
        std::size_t open = e; while (open < text.size() && blank(text[open])) ++open;
        if (open >= text.size() || text[open] != '(') continue;
        std::vector<std::string> list; const std::size_t close = args(open, list);
        if (close == std::string::npos) return false;
        if (gather && list.size() == 4 && list[0] == "0" && isInt(list[1])) calls.push_back({start, close, list[1], list[2], list[3]});
        else if (gather && list.size() >= 3 && isFloat(list[1])) floatSamplers.insert(list[2]);
        else if (sample && list.size() >= 2 && isFloat(list[0])) floatSamplers.insert(list[1]);
    }
    std::set<std::string> lowered;
    for (const auto& c : calls) if (floatSamplers.count(c.sampler)) lowered.insert(c.texture);
    if (lowered.empty()) return false;
    out.clear(); out.reserve(text.size() + 1024);
    std::size_t last = 0;
    for (const auto& c : calls) {
        if (!floatSamplers.count(c.sampler)) continue;
        out.append(text, last, c.begin - last);
        out += "cp2077GatherInt_" + c.texture + "(" + c.coords + ")";
        last = c.end;
    }
    out.append(text, last, std::string::npos);
    for (const auto& t : lowered) {
        const std::string scalar = types[t] == "texture_2d<i32>" ? "i32" : "u32";
        out += "\nfn cp2077GatherInt_" + t + "(uv: vec2<f32>) -> vec4<" + scalar + "> {\n"
               "    let d = vec2<i32>(textureDimensions(" + t + ", 0));\n"
               "    let m = max(d - vec2<i32>(1), vec2<i32>(0));\n"
               "    let b = vec2<i32>(floor(uv * vec2<f32>(d) - vec2<f32>(0.5)));\n"
               "    let p0 = clamp(b, vec2<i32>(0), m);\n"
               "    let p1 = clamp(b + vec2<i32>(1), vec2<i32>(0), m);\n"
               "    return vec4<" + scalar + ">(textureLoad(" + t + ", vec2<i32>(p0.x, p1.y), 0).x, textureLoad(" + t + ", p1, 0).x,\n"
               "        textureLoad(" + t + ", vec2<i32>(p1.x, p0.y), 0).x, textureLoad(" + t + ", p0, 0).x);\n}\n";
    }
    return true;
}
bool GatherLoweringEnabled() {
    // Main thread only (module creation runs there). ?gatherLower=0 = old behaviour (pipeline creation fails).
    static int enabled = -1;
    if (enabled < 0) enabled = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('gatherLower') === '0' ? 0 : 1; } catch (e) { return 1; } });
    return enabled != 0;
}
bool HoistWgslDirectives(const char* text, size_t bytes, std::string& out) {
    if (!text) return false;
    if (bytes == WGPU_STRLEN) bytes = std::strlen(text);
    const char* const end = text + bytes;
    auto nextLine = [end](const char* line, const char*& lineEnd) {
        auto* newline = static_cast<const char*>(std::memchr(line, '\n', size_t(end - line)));
        lineEnd = newline ? newline : end;
        return newline ? newline + 1 : end;
    };
    bool declarationSeen = false, needed = false;
    for (const char *line = text, *lineEnd = nullptr; line < end && !needed;) {
        const char* next = nextLine(line, lineEnd);
        if (IsWgslDirectiveLine(line, lineEnd)) needed = declarationSeen;
        else if (!IsWgslBlankOrCommentLine(line, lineEnd)) declarationSeen = true;
        line = next;
    }
    if (!needed) return false;
    std::string directives, rest;
    for (const char *line = text, *lineEnd = nullptr; line < end;) {
        const char* next = nextLine(line, lineEnd);
        std::string& target = IsWgslDirectiveLine(line, lineEnd) ? directives : rest;
        target.append(line, size_t(next - line));
        if (next == lineEnd) target += '\n'; // last line had no terminator
        line = next;
    }
    out = std::move(directives);
    out += rest;
    return true;
}
struct Request {
    const cp2077::web::ConvertedShader* source;
    WGPUDevice device = nullptr;
    WGPUShaderModule module = nullptr;
    WGPUComputePipeline pipeline = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 4; // async pipeline plus three allocation error scopes
    bool failed = false;
    // [boot] asynchronous creation (no waiting worker): called on the main thread with the pipeline (or null).
    void (*ready)(void* user, WGPUComputePipeline pipeline) = nullptr;
    void* user = nullptr;
};
// [boot] Ends a creation request: wakes the synchronous caller, or hands the result to the async callback.
void Finish(Request* request) {
    if (!request->ready) { emscripten_proxy_finish(request->context); return; }
    auto ready = request->ready; auto* user = request->user;
    WGPUComputePipeline pipeline = request->failed ? nullptr : request->pipeline;
    delete request;
    ready(user, pipeline);
}
void Report(WGPUStringView message, const char* shader = nullptr, const char* stage = nullptr) {
    std::string diagnostic="CP2077 WebGPU shader creation failed: ";
    if(shader){diagnostic+='[';diagnostic+=stage ? stage : "unknown";diagnostic+=' ';diagnostic+=shader;diagnostic+="] ";}
    if (message.data) {
        if (message.length == WGPU_STRLEN) diagnostic.append(message.data);
        else diagnostic.append(message.data,message.length);
    }
    std::fprintf(stderr,"%s\n",diagnostic.c_str());
}
void Complete(Request* request) {
    if (--request->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady() ||
        cp2077::web::MainThreadGpuDevice() != request->device) request->failed = true;
    if (request->module) wgpuShaderModuleRelease(request->module);
    if (request->failed && request->pipeline) {
        wgpuComputePipelineRelease(request->pipeline); request->pipeline = nullptr;
    }
    wgpuDeviceRelease(request->device);
    Finish(request);
}
void ScopeChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                  WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<Request*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true; Report(message, request->source->name, "compute");
    }
    Complete(request);
}
void PipelineCreated(WGPUCreatePipelineAsyncStatus status, WGPUComputePipeline pipeline,
                     WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<Request*>(argument);
    request->pipeline = pipeline;
    if (status != WGPUCreatePipelineAsyncStatus_Success || !pipeline) {
        request->failed = true; Report(message, request->source->name, "compute");
    }
    Complete(request);
}
// [notier1][wgsllang] Without texture-formats-tier1 the page rewrites tier1 storage formats to core ones (engine-select.js),
// so neither the device feature nor the WGSL texture_formats_tier1 language feature is needed. Firefox has neither; the
// language-feature check alone refused ~every lighting/HUD compute shader there.
bool Tier1StorageUnavailable(WGPUDevice device) {
    if (cp2077::web::GpuLacksTier1()) return false;
    return !wgpuDeviceHasFeature(device, WGPUFeatureName_TextureFormatsTier1) || !BrowserHasWGSLStorageFeature(1);
}
// Returns the first device capability a converted compute shader needs but the device lacks.
const char* MissingComputeCapability(WGPUDevice device, const cp2077::web::ConvertedShader& source) {
    using namespace cp2077::web;
    if ((source.requiredCaps & ConvertedShaderCap_Subgroups) && !wgpuDeviceHasFeature(device, WGPUFeatureName_Subgroups))
        return "subgroups";
    if ((source.requiredCaps & ConvertedShaderCap_FormatsTier1) && Tier1StorageUnavailable(device))
        return "texture-formats-tier1";
    if ((source.requiredCaps & ConvertedShaderCap_LargeWorkgroup) || source.workgroupInvocations > 256) {
        WGPULimits limits = WGPU_LIMITS_INIT;
        if (wgpuDeviceGetLimits(device, &limits) != WGPUStatus_Success ||
            limits.maxComputeInvocationsPerWorkgroup < std::max<std::uint32_t>(source.workgroupInvocations, 257u))
            return "maxComputeInvocationsPerWorkgroup";
    }
    return nullptr;
}
void BeginCreation(em_proxying_ctx* context, void* argument) {
    auto* request = static_cast<Request*>(argument);
    request->context = context;
    request->device = cp2077::web::MainThreadGpuDevice();
    if (!request->device) { request->failed = true; Finish(request); return; }
    if (const char* missing = MissingComputeCapability(request->device, *request->source)) {
        // Main thread only. No pipeline is created, so the engine binds none and the dispatch skips.
        static std::set<const cp2077::web::ConvertedShader*> reported;
        if (reported.insert(request->source).second)
            std::fprintf(stderr, "CP2077 WebGPU: compute shader not created, device lacks %s: %s\n",
                         missing, request->source->name);
        request->failed = true; Finish(request); return;
    }
    if (request->source->requiresTier1Storage && Tier1StorageUnavailable(request->device)) {
        Report({"required original shader storage feature is unavailable", WGPU_STRLEN});
        request->failed = true; Finish(request); return;
    }
    // [wgsllang] Firefox does not list readonly_and_readwrite_storage_textures; refusing here skipped the HUD tile and
    // lighting shaders and stalled boot at 57 %. Create the module anyway: the error scope rejects what the browser can't run.
    if (request->source->requiresReadWriteStorage && !BrowserHasWGSLStorageFeature(0)) {
        static bool logged = false;
        if (!logged) { logged = true; std::fprintf(stderr, "CP2077 WebGPU: WGSL readonly_and_readwrite_storage_textures not listed, creating anyway\n"); }
    }
    wgpuDeviceAddRef(request->device);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Validation);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {request->source->wgsl, request->source->wgslBytes};
    std::string hoisted; // must outlive wgpuDeviceCreateShaderModule
    if (HoistWgslDirectives(request->source->wgsl, request->source->wgslBytes, hoisted)) {
        std::fprintf(stderr, "CP2077 WebGPU: hoisted WGSL directives above global declarations: compute %s\n",
                     request->source->name);
        wgsl.code = {hoisted.data(), hoisted.size()};
    }
    WGPUShaderModuleDescriptor module = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    module.nextInChain = &wgsl.chain;
    module.label = {request->source->name, WGPU_STRLEN};
    request->module = wgpuDeviceCreateShaderModule(request->device, &module);
    if (request->module) {
        WGPUComputePipelineDescriptor descriptor = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
        descriptor.label = module.label;
        descriptor.compute.module = request->module;
        descriptor.compute.entryPoint = {request->source->entryPoint, WGPU_STRLEN};
        // Auto layout is specific to this exact shader pipeline. Bind groups
        // must use its own reflected layout, not one shared across all shaders.
        WGPUCreateComputePipelineAsyncCallbackInfo callback = WGPU_CREATE_COMPUTE_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_AllowSpontaneous;
        callback.callback = PipelineCreated; callback.userdata1 = request;
        wgpuDeviceCreateComputePipelineAsync(request->device, &descriptor, callback);
    } else {
        request->failed = true;
        --request->remaining; // no async pipeline callback was registered
    }
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = ScopeChecked; callback.userdata1 = request;
    for (unsigned index = 0; index < 3; ++index)
        wgpuDevicePopErrorScope(request->device, callback);
}
void BeginCreationAsync(void* argument) { BeginCreation(nullptr, argument); }
}
namespace cp2077::web {
// [boot] Starts the creation of a converted compute pipeline without waiting for it (worker threads). `ready`
// runs later on the main thread with the pipeline, or null on failure / device change. False = not started
// (main thread, device not ready, proxy failure); `ready` is then never called.
bool BeginGpuComputePipelineFromSource(const ConvertedShader* source, void (*ready)(void*, WGPUComputePipeline), void* user) {
    if (!source || !ready || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return false;
    auto* queue = ShaderQueue();
    if (!queue) return false;
    auto* request = new Request{source};
    request->ready = ready; request->user = user;
    if (!emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), BeginCreationAsync, request)) {
        delete request; return false;
    }
    return true;
}
WGPUComputePipeline CreateGpuComputePipelineFromSource(const ConvertedShader* source) {
    if (!source || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    auto* queue = ShaderQueue();
    if (!queue) return nullptr;
    Request request{source};
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
                                         BeginCreation, &request)) return nullptr;
    return request.pipeline;
}
WGPUComputePipeline CreateConvertedComputePipeline(const char* shader){return CreateGpuComputePipelineFromSource(FindConvertedShader(shader));}
bool ReleaseGpuComputePipeline(WGPUComputePipeline pipeline) {
    if (!pipeline) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuComputePipelineRelease(pipeline); return true; }
    auto* queue = ShaderQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuComputePipelineRelease(static_cast<WGPUComputePipeline>(argument)); }, pipeline);
}
}

namespace {
struct GraphicsModuleRequest {
    const cp2077::web::ConvertedGraphicsShader* source;
    WGPUDevice device = nullptr;
    WGPUShaderModule module = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 4; // compilation information and allocation scopes
    bool failed = false;
};
void ReadWarmSwitchesOnMain(); // [warmup] gpu-pipeline-warmup-web.inl
// [warmup] Graphics modules deduplicated by converted source (main thread only). CreateShader made a fresh
// module for every engine shader object, and the render-pipeline cache keys on module handles, so equal WGSL
// compiled into distinct pipelines and a warmup could never pre-create the key a draw would look up. The cache
// holds one reference per source; every Create returns its own reference. ?modDedup=0 = old (no dedup).
struct DedupedGraphicsModule { WGPUShaderModule module = nullptr; WGPUDevice device = nullptr; std::uint64_t generation = 0; };
std::unordered_map<const cp2077::web::ConvertedGraphicsShader*, DedupedGraphicsModule> dedupedGraphicsModules;
std::unordered_map<WGPUShaderModule, const cp2077::web::ConvertedGraphicsShader*> graphicsModuleSources; // recording
std::atomic<std::uint64_t> graphicsModuleDedupHits{0}, graphicsModuleCreates{0};
int GraphicsModuleDedupEnabled() {
    static int state = -1;
    if (state < 0) state = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('modDedup') === '0' ? 0 : 1; });
    return state;
}
void DropDedupedGraphicsModulesIfStale(WGPUDevice device) {
    static WGPUDevice current = nullptr; static std::uint64_t currentGeneration = 0;
    const auto generation = cp2077::web::GpuDeviceGeneration();
    if (device == current && generation == currentGeneration) return;
    for (auto& item : dedupedGraphicsModules) wgpuShaderModuleRelease(item.second.module);
    dedupedGraphicsModules.clear(); graphicsModuleSources.clear();
    current = device; currentGeneration = generation;
}
void CompleteGraphicsModule(GraphicsModuleRequest* request) {
    if (--request->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady() ||
        cp2077::web::MainThreadGpuDevice() != request->device) request->failed = true;
    if (request->failed && request->module) {
        wgpuShaderModuleRelease(request->module); request->module = nullptr;
    }
    if (request->module && GraphicsModuleDedupEnabled()) {
        DropDedupedGraphicsModulesIfStale(request->device);
        auto inserted = dedupedGraphicsModules.emplace(request->source,
            DedupedGraphicsModule{request->module, request->device, cp2077::web::GpuDeviceGeneration()});
        if (inserted.second) { wgpuShaderModuleAddRef(request->module); graphicsModuleSources[request->module] = request->source; }
    }
    wgpuDeviceRelease(request->device);
    emscripten_proxy_finish(request->context);
}
void GraphicsScopeChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                          WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<GraphicsModuleRequest*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true; Report(message, request->source->originalHash, "graphics");
    }
    CompleteGraphicsModule(request);
}
void GraphicsCompilationChecked(WGPUCompilationInfoRequestStatus status,
                                 const WGPUCompilationInfo* info, void* argument, void*) {
    auto* request = static_cast<GraphicsModuleRequest*>(argument);
    if (status != WGPUCompilationInfoRequestStatus_Success || !info) request->failed = true;
    if (info) for (std::size_t i = 0; i < info->messageCount; ++i)
        if (info->messages[i].type == WGPUCompilationMessageType_Error) {
            request->failed = true; Report(info->messages[i].message, request->source->originalHash, "graphics");
        }
    CompleteGraphicsModule(request);
}
void BeginGraphicsModule(em_proxying_ctx* context, void* argument) {
    auto* request = static_cast<GraphicsModuleRequest*>(argument);
    request->context = context;
    request->device = cp2077::web::MainThreadGpuDevice();
    if (!request->device) { emscripten_proxy_finish(context); return; }
    ReadWarmSwitchesOnMain();
    if (GraphicsModuleDedupEnabled()) {
        DropDedupedGraphicsModulesIfStale(request->device);
        auto found = dedupedGraphicsModules.find(request->source);
        if (found != dedupedGraphicsModules.end()) {
            request->module = found->second.module; wgpuShaderModuleAddRef(request->module);
            ++graphicsModuleDedupHits; emscripten_proxy_finish(context); return;
        }
    }
    ++graphicsModuleCreates;
    wgpuDeviceAddRef(request->device);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Internal);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_OutOfMemory);
    wgpuDevicePushErrorScope(request->device, WGPUErrorFilter_Validation);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = {request->source->wgsl, request->source->wgslBytes};
    std::string hoisted; // must outlive wgpuDeviceCreateShaderModule
    if (HoistWgslDirectives(request->source->wgsl, request->source->wgslBytes, hoisted)) {
        std::fprintf(stderr, "CP2077 WebGPU: hoisted WGSL directives above global declarations: graphics %s\n",
                     request->source->originalHash);
        wgsl.code = {hoisted.data(), hoisted.size()};
    }
    std::string gatherLowered; // [tankview] must outlive wgpuDeviceCreateShaderModule
    if (GatherLoweringEnabled() &&
        LowerMixedIntegerGathers(std::string(wgsl.code.data, wgsl.code.length), gatherLowered)) {
        static std::atomic<unsigned> gatherReports{0};
        if (gatherReports.fetch_add(1) < 32)
            std::fprintf(stderr, "CP2077 tankview: integer gathers on a mixed sampler lowered to textureLoad: graphics %s\n",
                         request->source->originalHash);
        wgsl.code = {gatherLowered.data(), gatherLowered.size()};
    }
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &wgsl.chain;
    descriptor.label = {request->source->originalHash, WGPU_STRLEN};
    request->module = wgpuDeviceCreateShaderModule(request->device, &descriptor);
    if (request->module) {
        WGPUCompilationInfoCallbackInfo callback = WGPU_COMPILATION_INFO_CALLBACK_INFO_INIT;
        callback.mode = WGPUCallbackMode_AllowSpontaneous;
        callback.callback = GraphicsCompilationChecked; callback.userdata1 = request;
        wgpuShaderModuleGetCompilationInfo(request->module, callback);
    } else { request->failed = true; --request->remaining; }
    WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowSpontaneous;
    callback.callback = GraphicsScopeChecked; callback.userdata1 = request;
    for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(request->device, callback);
}
}
namespace cp2077::web {
void MaybeStartPipelineWarmup(); // [warmup]
WGPUShaderModule CreateConvertedGraphicsModule(const ConvertedGraphicsShader* source) {
    if (!source || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    auto* queue = ShaderQueue(); if (!queue) return nullptr;
    GraphicsModuleRequest request{source};
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
                                        BeginGraphicsModule, &request)) return nullptr;
    MaybeStartPipelineWarmup();
    return request.module;
}
bool ReleaseGpuGraphicsModule(WGPUShaderModule module) {
    if (!module) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuShaderModuleRelease(module); return true; }
    auto* queue = ShaderQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuShaderModuleRelease(static_cast<WGPUShaderModule>(argument)); }, module);
}
}

namespace {
struct RenderPipelineRequest {
    WGPURenderPipelineDescriptor descriptor;
    WGPUFragmentState fragment{};
    WGPUDepthStencilState depth{};
    std::vector<WGPUVertexBufferLayout> buffers;
    std::vector<std::vector<WGPUVertexAttribute>> attributes;
    std::vector<WGPUColorTargetState> targets;
    std::vector<WGPUBlendState> blends;
    std::string label, vertexEntry, fragmentEntry;
    WGPUDevice device = nullptr;
    WGPURenderPipeline pipeline = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 4;
    bool failed = false;
    bool validationFailed = false; // deterministic failure: eligible for the negative pipeline cache
    std::string cacheKey;
    std::uint64_t cacheHash=0;
    double compileStarted=0;
    std::uint64_t deviceGeneration=0;
    bool owned=false; // [hitch] heap request from CreateGpuRenderPipelineOrPending: no waiting worker, deleted on completion
    bool warm=false;  // [warmup] owned request from the pipeline warmup (own in-flight counter, not recorded)
};
void RecordCompletedRenderPipeline(const RenderPipelineRequest&); // [warmup] gpu-pipeline-warmup-web.inl
void WarmRenderPipelineFinished(bool ok);
void OwnString(WGPUStringView& view, std::string& storage) {
    if (!view.data) return;
    storage.assign(view.data, view.length == WGPU_STRLEN ? std::strlen(view.data) : view.length);
    view = {storage.data(), storage.size()};
}
// Chained extensions and specialization constants require explicit adapters.
// Reject them rather than quietly changing the caller's rendering behavior.
// Checked before the worker cache lookup, which reads the caller's arrays directly.
bool ValidRenderDescriptor(const WGPURenderPipelineDescriptor& source) {
    if (source.nextInChain || source.vertex.nextInChain || source.primitive.nextInChain ||
        source.multisample.nextInChain || source.vertex.constantCount ||
        !source.vertex.module || source.vertex.bufferCount > 16 ||
        (source.vertex.bufferCount && !source.vertex.buffers) ||
        (source.depthStencil && source.depthStencil->nextInChain) ||
        (source.fragment && (source.fragment->nextInChain || source.fragment->constantCount ||
                            !source.fragment->module || source.fragment->targetCount > 8 ||
                            (source.fragment->targetCount && !source.fragment->targets)))) return false;
    for (std::size_t i = 0; i < source.vertex.bufferCount; ++i) {
        const auto& buffer = source.vertex.buffers[i];
        if (buffer.nextInChain || buffer.attributeCount > 64 ||
            (buffer.attributeCount && !buffer.attributes)) return false;
    }
    if (source.fragment)
        for (std::size_t i = 0; i < source.fragment->targetCount; ++i)
            if (source.fragment->targets[i].nextInChain) return false;
    return true;
}
bool CopyRenderDescriptor(RenderPipelineRequest& request, const WGPURenderPipelineDescriptor& source) {
    if (!ValidRenderDescriptor(source)) return false;
    request.descriptor = source;
    OwnString(request.descriptor.label, request.label);
    OwnString(request.descriptor.vertex.entryPoint, request.vertexEntry);
    request.buffers.resize(source.vertex.bufferCount);
    request.attributes.resize(source.vertex.bufferCount);
    for (std::size_t i = 0; i < source.vertex.bufferCount; ++i) {
        const auto& buffer = source.vertex.buffers[i];
        if (buffer.nextInChain || buffer.attributeCount > 64 ||
            (buffer.attributeCount && !buffer.attributes)) return false;
        request.buffers[i] = buffer;
        if (buffer.attributeCount)
            request.attributes[i].assign(buffer.attributes, buffer.attributes + buffer.attributeCount);
        request.buffers[i].attributes = request.attributes[i].data();
    }
    request.descriptor.vertex.buffers = request.buffers.data();
    if (source.depthStencil) {
        request.depth = *source.depthStencil; request.descriptor.depthStencil = &request.depth;
    }
    if (source.fragment) {
        request.fragment = *source.fragment;
        OwnString(request.fragment.entryPoint, request.fragmentEntry);
        request.targets.resize(source.fragment->targetCount);
        request.blends.resize(source.fragment->targetCount);
        for (std::size_t i = 0; i < source.fragment->targetCount; ++i) {
            const auto& target = source.fragment->targets[i];
            if (target.nextInChain) return false;
            request.targets[i] = target;
            if (target.blend) {
                request.blends[i] = *target.blend; request.targets[i].blend = &request.blends[i];
            }
        }
        request.fragment.targets = request.targets.data();
        request.descriptor.fragment = &request.fragment;
    }
    return true;
}
// [hitch] non-blocking pipeline state (see BeginOwnedRenderPipeline).
std::atomic<std::uint64_t> pipelinePendingSkips{0}, pipelineAsyncStarted{0};
// In-flight owned creations (worker increments when queued, completion/drop decrements). Bounded so a burst of new
// keys cannot flood the browser GPU process; over the cap the draw is skipped without starting (retried next frame).
std::atomic<int> pipelineAsyncInFlight{0};
constexpr int kMaxAsyncRenderPipelinesInFlight = 8;
// ?asyncPso switch, read on the MAIN thread only (a worker-side MAIN_THREAD_EM_ASM from inside a draw holding the
// bound-command lock could deadlock): -1 unknown (workers use the sync path), 0 off, 1 on.
std::atomic<int> asyncRenderPipelinesState{-1};
void ReadAsyncRenderPipelineSwitchOnMain(){
    if(asyncRenderPipelinesState.load(std::memory_order_relaxed)>=0)return;
    const int v=MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('asyncPso') === '0' ? 0 : 1; });
    asyncRenderPipelinesState.store(v);
    std::fprintf(stderr, "CP2077 hitch switches: asyncPso=%d\n", v);
}
#include "gpu-render-pipeline-cache-web.inl"
void CompleteRenderPipeline(RenderPipelineRequest* request) {
    if (--request->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady() ||
        cp2077::web::MainThreadGpuDevice() != request->device ||
        cp2077::web::GpuDeviceGeneration()!=request->deviceGeneration) request->failed = true;
    if (request->failed && request->pipeline) {
        wgpuRenderPipelineRelease(request->pipeline); request->pipeline = nullptr;
    }
    PublishCompletedRenderPipeline(*request);
    wgpuShaderModuleRelease(request->descriptor.vertex.module);
    if (request->descriptor.fragment) wgpuShaderModuleRelease(request->fragment.module);
    if (request->descriptor.layout) wgpuPipelineLayoutRelease(request->descriptor.layout);
    wgpuDeviceRelease(request->device);
    if (request->owned) {
        if (request->warm) WarmRenderPipelineFinished(request->pipeline != nullptr);
        else pipelineAsyncInFlight.fetch_sub(1);
        if (request->pipeline) wgpuRenderPipelineRelease(request->pipeline); // the cache keeps its own reference
        delete request; return;
    }
    emscripten_proxy_finish(request->context);
}
void RenderScopeChecked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
                        WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<RenderPipelineRequest*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true; Report(message, request->label.c_str(), "render-pipeline");
        if (status == WGPUPopErrorScopeStatus_Success && type == WGPUErrorType_Validation) request->validationFailed = true;
    }
    CompleteRenderPipeline(request);
}
void RenderPipelineCreated(WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline,
                           WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<RenderPipelineRequest*>(argument);
    request->pipeline = pipeline;
    if (status != WGPUCreatePipelineAsyncStatus_Success || !pipeline) {
        request->failed = true; Report(message, request->label.c_str(), "render-pipeline");
        if (status == WGPUCreatePipelineAsyncStatus_ValidationError) request->validationFailed = true;
    }
    CompleteRenderPipeline(request);
}
void BeginRenderPipeline(em_proxying_ctx* context, void* argument) {
    ReadAsyncRenderPipelineSwitchOnMain();
    auto* request = static_cast<RenderPipelineRequest*>(argument);
    request->context = context;
    request->device = cp2077::web::MainThreadGpuDevice();
    if (!request->device) { emscripten_proxy_finish(context); return; }
    request->deviceGeneration=cp2077::web::GpuDeviceGeneration();
    if(TryReuseRenderPipeline(*request))return;
    wgpuDeviceAddRef(request->device);
    wgpuShaderModuleAddRef(request->descriptor.vertex.module);
    if (request->descriptor.fragment) wgpuShaderModuleAddRef(request->fragment.module);
    if (request->descriptor.layout) wgpuPipelineLayoutAddRef(request->descriptor.layout);
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
// [hitch] Non-blocking pipeline misses. Before this, every render worker that met a new pipeline key waited in
// emscripten_proxy_sync until createRenderPipelineAsync completed (tens of ms per miss), stalling the frame.
// Owned requests are heap state (not the worker's stack); the worker registers the key in pendingRenderPipelines
// and takes references on the modules/layout, so sync requests for the same key join it as waiters.
void DropOwnedRenderPipeline(RenderPipelineRequest* request) {
    {
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        auto pending=pendingRenderPipelines.find(request->cacheKey);
        if(pending!=pendingRenderPipelines.end()){
            auto waiters=std::move(pending->second);pendingRenderPipelines.erase(pending);
            for(auto* waiter:waiters){waiter->pipeline=nullptr;emscripten_proxy_finish(waiter->context);}
        }
    }
    wgpuShaderModuleRelease(request->descriptor.vertex.module);
    if (request->descriptor.fragment) wgpuShaderModuleRelease(request->fragment.module);
    if (request->descriptor.layout) wgpuPipelineLayoutRelease(request->descriptor.layout);
    pipelineAsyncInFlight.fetch_sub(1);
    delete request;
}
void BeginOwnedRenderPipeline(void* argument) {
    auto* request = static_cast<RenderPipelineRequest*>(argument);
    request->context = nullptr;
    request->device = cp2077::web::MainThreadGpuDevice();
    if (!request->device) { DropOwnedRenderPipeline(request); return; }
    request->deviceGeneration=cp2077::web::GpuDeviceGeneration();
    {
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        SelectPipelineCacheDevice(request->device);
        std::string key; BuildRenderPipelineKey(key,request->device,request->deviceGeneration,request->descriptor);
        // Device or generation changed since the worker built the key: drop it (a later draw retries).
        if (key!=request->cacheKey) { DropOwnedRenderPipeline(request); return; }
        ++pipelineMisses; request->compileStarted=emscripten_get_now(); UpdatePipelineCacheStats();
    }
    ++pipelineAsyncStarted;
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
bool AsyncRenderPipelinesEnabled() { return asyncRenderPipelinesState.load(std::memory_order_relaxed) == 1; }
}
namespace cp2077::web {
std::uint64_t GpuRenderPipelinePendingSkips() { return pipelinePendingSkips.load(std::memory_order_relaxed); }
WGPURenderPipeline CreateGpuRenderPipelineOrPending(const WGPURenderPipelineDescriptor& descriptor, bool& pending, bool blockWhenBusy) {
    pending = false;
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    if (!AsyncRenderPipelinesEnabled()) return CreateGpuRenderPipeline(descriptor);
    auto* queue = ShaderQueue(); if (!queue) return nullptr;
    if (!ValidRenderDescriptor(descriptor)) {
        std::fputs("CP2077 WebGPU: unsupported or invalid render pipeline descriptor\n", stderr);
        return nullptr;
    }
    WGPURenderPipeline cached = nullptr;
    if (TryReuseRenderPipelineOnWorker(descriptor, cached)) return cached;
    WGPUDevice device=nullptr; std::uint64_t generation=0;
    {
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        if(renderPipelineCacheDevice && renderPipelineCacheGeneration==GpuDeviceGeneration()){
            device=renderPipelineCacheDevice; generation=renderPipelineCacheGeneration;
        }
    }
    // Until the main thread has selected the cache device the key cannot be built here: sync path. Called with
    // the cache mutex RELEASED (the main-thread side takes it; holding it across the proxy deadlocks).
    if (!device) return CreateGpuRenderPipeline(descriptor);
    auto request = std::make_unique<RenderPipelineRequest>();
    if (!CopyRenderDescriptor(*request, descriptor)) {
        std::fputs("CP2077 WebGPU: unsupported or invalid render pipeline descriptor\n", stderr);
        return nullptr;
    }
    BuildRenderPipelineKey(request->cacheKey, device, generation, request->descriptor);
    request->cacheHash = HashRenderPipelineKey(request->cacheKey);
    bool busy = false;
    {
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        if (renderPipelineCacheDevice!=device || renderPipelineCacheGeneration!=generation) return nullptr;
        if (auto* found = FindCachedRenderPipeline(request->cacheKey, request->cacheHash)) {
            auto pipeline = found->pipeline; wgpuRenderPipelineAddRef(pipeline);
            found->stamp=++pipelineStamp; ++pipelineHits; ++pipelineWorkerHits; return pipeline;
        }
        if (IsFailedRenderPipeline(request->cacheKey, request->cacheHash)) return nullptr;
        if (pendingRenderPipelines.count(request->cacheKey)) {
            pending = true; pipelinePendingSkips.fetch_add(1, std::memory_order_relaxed); return nullptr;
        }
        if (pipelineAsyncInFlight.load() >= kMaxAsyncRenderPipelinesInFlight) {
            if (!blockWhenBusy) { pending = true; pipelinePendingSkips.fetch_add(1, std::memory_order_relaxed); return nullptr; }
            busy = true;
        } else {
            pipelineAsyncInFlight.fetch_add(1);
            pendingRenderPipelines.emplace(request->cacheKey, std::vector<RenderPipelineRequest*>{});
        }
    }
    // Precache over the cap: wait like before (cache mutex released) so its variants are still warmed.
    if (busy) return CreateGpuRenderPipeline(descriptor);
    // References for the main-thread creation; released there (CompleteRenderPipeline / DropOwnedRenderPipeline).
    wgpuShaderModuleAddRef(request->descriptor.vertex.module);
    if (request->descriptor.fragment) wgpuShaderModuleAddRef(request->fragment.module);
    if (request->descriptor.layout) wgpuPipelineLayoutAddRef(request->descriptor.layout);
    request->owned = true;
    if (!emscripten_proxy_async(queue, emscripten_main_runtime_thread_id(), BeginOwnedRenderPipeline, request.get())) {
        // Not queued: unregister the key. The references above leak (handles are released on main only).
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        pendingRenderPipelines.erase(request->cacheKey); pipelineAsyncInFlight.fetch_sub(1); return nullptr;
    }
    request.release();
    pending = true; pipelinePendingSkips.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
WGPURenderPipeline CreateGpuRenderPipeline(const WGPURenderPipelineDescriptor& descriptor) {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    auto* queue = ShaderQueue(); if (!queue) return nullptr;
    if (!ValidRenderDescriptor(descriptor)) {
        std::fputs("CP2077 WebGPU: unsupported or invalid render pipeline descriptor\n", stderr);
        return nullptr;
    }
    WGPURenderPipeline cached = nullptr;
    if (TryReuseRenderPipelineOnWorker(descriptor, cached)) return cached;
    RenderPipelineRequest request{};
    if (!CopyRenderDescriptor(request, descriptor)) {
        std::fputs("CP2077 WebGPU: unsupported or invalid render pipeline descriptor\n", stderr);
        return nullptr;
    }
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
                                       BeginRenderPipeline, &request)) return nullptr;
    return request.pipeline;
}
bool ReleaseGpuRenderPipeline(WGPURenderPipeline pipeline) {
    if (!pipeline) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuRenderPipelineRelease(pipeline); return true; }
    if(ReleaseCachedRenderPipelineOnWorker(pipeline))return true;
    auto* queue = ShaderQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuRenderPipelineRelease(static_cast<WGPURenderPipeline>(argument)); }, pipeline);
}
}

// Explicit graphics pipeline layouts with dynamic uniform offsets. Arena-suballocated uniforms differ only in
// offset from draw to draw; with dynamic offsets the bind group (and its cache key) carries offset 0 and the
// real offset goes to setBindGroup, so the bind-group cache hits instead of missing on every draw.
// The layout must accept everything the default (auto) layout would: entries come from the reflected
// resources, types from the WGSL declarations, following the default-layout rules:
//   f32 texture -> "float" only when it is the texture argument of a sampler-taking builtin, else
//   "unfilterable-float"; sampler -> "filtering"; sampler_comparison -> "comparison".
// Anything not described with certainty (multisampled, depth texture with a non-comparison sampler, unknown
// storage format, texture/sampler type outside a binding declaration, unparsed builtin argument) keeps the
// auto layout for that pair. A pipeline that still fails with the explicit layout is retried with auto
// layout by the draw and the pair is rejected (RejectGpuGraphicsPipelineLayout).
namespace {
struct WgslBindingDecl {
    enum class Kind { Uniform, ReadOnlyStorage, Storage, Texture, StorageTexture, Sampler, ComparisonSampler };
    Kind kind = Kind::Uniform;
    std::uint32_t binding = 0;
    std::string name;
    WGPUTextureSampleType sampleType = WGPUTextureSampleType_Undefined;
    WGPUTextureViewDimension dimension = WGPUTextureViewDimension_Undefined;
    WGPUStorageTextureAccess access = WGPUStorageTextureAccess_Undefined;
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    bool floatTexture = false, depth = false;
    // [tankview] sampler pairing: a `sampler` only ever used with texture_*<u32/i32> must be NonFiltering.
    bool pairedFloat = false, pairedInteger = false;
};
bool WgslIdentChar(char c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
}
bool WgslSpace(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
std::string WgslCompact(const std::string& text) {
    std::string out; out.reserve(text.size());
    for (char c : text) if (!WgslSpace(c)) out += c;
    return out;
}
// at points just past "(": optional spaces, decimal digits.
bool WgslNumberAfter(const std::string& text, std::size_t at, std::uint32_t& value) {
    while (at<text.size() && WgslSpace(text[at])) ++at;
    if (at>=text.size() || text[at]<'0' || text[at]>'9') return false;
    std::uint64_t v = 0;
    while (at<text.size() && text[at]>='0' && text[at]<='9') {
        v = v*10 + std::uint64_t(text[at]-'0'); if (v>0xffffffffull) return false; ++at;
    }
    value = std::uint32_t(v); return true;
}
WGPUTextureViewDimension WgslDimension(const std::string& d) {
    if (d=="1d") return WGPUTextureViewDimension_1D;
    if (d=="2d") return WGPUTextureViewDimension_2D;
    if (d=="2d_array") return WGPUTextureViewDimension_2DArray;
    if (d=="3d") return WGPUTextureViewDimension_3D;
    if (d=="cube") return WGPUTextureViewDimension_Cube;
    if (d=="cube_array") return WGPUTextureViewDimension_CubeArray;
    return WGPUTextureViewDimension_Undefined;
}
WGPUTextureFormat WgslStorageFormat(const std::string& f) {
    static const struct { const char* name; WGPUTextureFormat format; } kFormats[] = {
        {"r8unorm",WGPUTextureFormat_R8Unorm},{"r8snorm",WGPUTextureFormat_R8Snorm},{"r8uint",WGPUTextureFormat_R8Uint},
        {"r8sint",WGPUTextureFormat_R8Sint},{"r16uint",WGPUTextureFormat_R16Uint},{"r16sint",WGPUTextureFormat_R16Sint},
        {"r16float",WGPUTextureFormat_R16Float},{"rg8unorm",WGPUTextureFormat_RG8Unorm},{"rg8snorm",WGPUTextureFormat_RG8Snorm},
        {"rg8uint",WGPUTextureFormat_RG8Uint},{"rg8sint",WGPUTextureFormat_RG8Sint},{"r32float",WGPUTextureFormat_R32Float},
        {"r32uint",WGPUTextureFormat_R32Uint},{"r32sint",WGPUTextureFormat_R32Sint},{"rg16uint",WGPUTextureFormat_RG16Uint},
        {"rg16sint",WGPUTextureFormat_RG16Sint},{"rg16float",WGPUTextureFormat_RG16Float},{"rgba8unorm",WGPUTextureFormat_RGBA8Unorm},
        {"rgba8snorm",WGPUTextureFormat_RGBA8Snorm},{"rgba8uint",WGPUTextureFormat_RGBA8Uint},{"rgba8sint",WGPUTextureFormat_RGBA8Sint},
        {"bgra8unorm",WGPUTextureFormat_BGRA8Unorm},{"rgb10a2uint",WGPUTextureFormat_RGB10A2Uint},
        {"rgb10a2unorm",WGPUTextureFormat_RGB10A2Unorm},{"rg11b10ufloat",WGPUTextureFormat_RG11B10Ufloat},
        {"rg32float",WGPUTextureFormat_RG32Float},{"rg32uint",WGPUTextureFormat_RG32Uint},{"rg32sint",WGPUTextureFormat_RG32Sint},
        {"rgba16uint",WGPUTextureFormat_RGBA16Uint},{"rgba16sint",WGPUTextureFormat_RGBA16Sint},
        {"rgba16float",WGPUTextureFormat_RGBA16Float},{"rgba32float",WGPUTextureFormat_RGBA32Float},
        {"rgba32uint",WGPUTextureFormat_RGBA32Uint},{"rgba32sint",WGPUTextureFormat_RGBA32Sint},
    };
    for (const auto& row : kFormats) if (f==row.name) return row.format;
    return WGPUTextureFormat_Undefined;
}
bool ParseWgslBindingType(const std::string& space, const std::string& type, WgslBindingDecl& decl) {
    using Kind = WgslBindingDecl::Kind;
    if (!space.empty()) {
        if (space=="uniform") decl.kind = Kind::Uniform;
        else if (space=="storage" || space=="storage,read") decl.kind = Kind::ReadOnlyStorage;
        else if (space=="storage,read_write") decl.kind = Kind::Storage;
        else return false;
        return true;
    }
    if (type=="sampler") { decl.kind = Kind::Sampler; return true; }
    if (type=="sampler_comparison") { decl.kind = Kind::ComparisonSampler; return true; }
    if (type.compare(0,14,"texture_depth_")==0) {
        // texture_depth_multisampled_2d maps to no dimension and keeps the auto layout.
        decl.kind = Kind::Texture; decl.depth = true; decl.sampleType = WGPUTextureSampleType_Depth;
        decl.dimension = WgslDimension(type.substr(14));
        return decl.dimension!=WGPUTextureViewDimension_Undefined;
    }
    const auto open = type.find('<');
    if (open==std::string::npos || type.back()!='>') return false;
    const std::string inner = type.substr(open+1, type.size()-open-2);
    if (type.compare(0,16,"texture_storage_")==0) {
        decl.kind = Kind::StorageTexture;
        decl.dimension = WgslDimension(type.substr(16, open-16));
        if (decl.dimension==WGPUTextureViewDimension_Undefined || decl.dimension==WGPUTextureViewDimension_Cube ||
            decl.dimension==WGPUTextureViewDimension_CubeArray) return false;
        const auto comma = inner.find(',');
        if (comma==std::string::npos) return false;
        decl.format = WgslStorageFormat(inner.substr(0, comma));
        const std::string access = inner.substr(comma+1);
        decl.access = access=="write" ? WGPUStorageTextureAccess_WriteOnly :
                      access=="read" ? WGPUStorageTextureAccess_ReadOnly :
                      access=="read_write" ? WGPUStorageTextureAccess_ReadWrite : WGPUStorageTextureAccess_Undefined;
        return decl.format!=WGPUTextureFormat_Undefined && decl.access!=WGPUStorageTextureAccess_Undefined;
    }
    if (type.compare(0,8,"texture_")!=0) return false;
    decl.kind = Kind::Texture;
    decl.dimension = WgslDimension(type.substr(8, open-8)); // multisampled/external stay Undefined
    if (decl.dimension==WGPUTextureViewDimension_Undefined) return false;
    if (inner=="f32") { decl.floatTexture = true; decl.sampleType = WGPUTextureSampleType_UnfilterableFloat; }
    else if (inner=="i32") decl.sampleType = WGPUTextureSampleType_Sint;
    else if (inner=="u32") decl.sampleType = WGPUTextureSampleType_Uint;
    else return false;
    return true;
}
bool ParseWgslBindings(const char* wgsl, std::size_t bytes, std::vector<WgslBindingDecl>& decls, const char*& reason) {
    using Kind = WgslBindingDecl::Kind;
    if (!wgsl || !bytes) { reason = "no WGSL"; return false; }
    const std::string text(wgsl, bytes);
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    for (auto at = text.find("@binding("); at!=std::string::npos; at = text.find("@binding(", at+9)) {
        auto start = text.find_last_of(";{}", at); start = start==std::string::npos ? 0 : start+1;
        const auto end = text.find(';', at);
        if (end==std::string::npos) { reason = "unterminated binding declaration"; return false; }
        const std::string statement = text.substr(start, end-start);
        std::uint32_t group = 0; WgslBindingDecl decl;
        const auto groupAt = statement.find("@group(");
        const auto bindingAt = statement.find("@binding(");
        if (groupAt==std::string::npos || !WgslNumberAfter(statement, groupAt+7, group) || group!=0 ||
            bindingAt==std::string::npos || !WgslNumberAfter(statement, bindingAt+9, decl.binding)) {
            reason = "binding attributes"; return false;
        }
        std::size_t var = std::string::npos;
        for (auto p = statement.find("var"); p!=std::string::npos; p = statement.find("var", p+3))
            if ((p==0 || !WgslIdentChar(statement[p-1])) && p+3<statement.size() && !WgslIdentChar(statement[p+3])) { var = p; break; }
        if (var==std::string::npos) { reason = "binding without var"; return false; }
        std::size_t p = var+3; std::string space;
        while (p<statement.size() && WgslSpace(statement[p])) ++p;
        if (p<statement.size() && statement[p]=='<') {
            const auto close = statement.find('>', p);
            if (close==std::string::npos) { reason = "address space"; return false; }
            space = WgslCompact(statement.substr(p+1, close-p-1)); p = close+1;
        }
        const auto colon = statement.find(':', p);
        if (colon==std::string::npos) { reason = "binding type"; return false; }
        decl.name = WgslCompact(statement.substr(p, colon-p));
        bool nameOk = !decl.name.empty();
        for (char c : decl.name) nameOk = nameOk && WgslIdentChar(c);
        if (!nameOk || !ParseWgslBindingType(space, WgslCompact(statement.substr(colon+1)), decl)) {
            reason = "unsupported binding type"; return false;
        }
        for (const auto& existing : decls)
            if (existing.binding==decl.binding || existing.name==decl.name) { reason = "duplicate binding"; return false; }
        decls.push_back(std::move(decl)); ranges.push_back({start, end});
    }
    auto inDeclaration = [&](std::size_t pos) {
        for (const auto& range : ranges) if (pos>=range.first && pos<range.second) return true;
        return false;
    };
    // Texture/sampler types anywhere else (function parameters, aliases) mean uses this scan cannot follow.
    for (const char* needle : {"texture_", "sampler"})
        for (auto p = text.find(needle); p!=std::string::npos; p = text.find(needle, p+1))
            if ((p==0 || !WgslIdentChar(text[p-1])) && !inDeclaration(p)) { reason = "handle type outside declarations"; return false; }
    auto find = [&](const std::string& name) -> WgslBindingDecl* {
        for (auto& decl : decls) if (decl.name==name) return &decl;
        return nullptr;
    };
    // Reads one plain identifier argument followed by ','.
    auto argument = [&](std::size_t& at, std::string& name) {
        while (at<text.size() && WgslSpace(text[at])) ++at;
        const auto begin = at;
        while (at<text.size() && WgslIdentChar(text[at])) ++at;
        name = text.substr(begin, at-begin);
        while (at<text.size() && WgslSpace(text[at])) ++at;
        if (name.empty() || at>=text.size() || text[at]!=',') return false;
        ++at; return true;
    };
    for (std::size_t p = 0; p<text.size();) {
        if (!WgslIdentChar(text[p])) { ++p; continue; }
        std::size_t e = p; while (e<text.size() && WgslIdentChar(text[e])) ++e;
        const bool sample = text.compare(p, 13, "textureSample")==0 && e-p>=13;
        const bool gather = text.compare(p, 13, "textureGather")==0 && e-p>=13;
        const bool compare = sample || gather ? text.substr(p, e-p).find("Compare")!=std::string::npos : false;
        p = e;
        if (!sample && !gather) continue;
        std::size_t at = e;
        while (at<text.size() && WgslSpace(text[at])) ++at;
        if (at>=text.size() || text[at]!='(') continue;
        ++at;
        std::string first, samplerName;
        if (!argument(at, first)) { reason = "texture builtin argument"; return false; }
        WgslBindingDecl* texture = find(first);
        if (gather && !compare && (!texture || texture->kind!=Kind::Texture)) {
            // textureGather(component, texture, sampler, ...)
            if (!argument(at, first)) { reason = "texture builtin argument"; return false; }
            texture = find(first);
        }
        if (!texture || texture->kind!=Kind::Texture || !argument(at, samplerName)) { reason = "texture builtin argument"; return false; }
        WgslBindingDecl* sampler = find(samplerName);
        if (!sampler || (sampler->kind!=Kind::Sampler && sampler->kind!=Kind::ComparisonSampler)) {
            reason = "texture builtin sampler"; return false;
        }
        if (texture->depth && sampler->kind==Kind::Sampler) { reason = "depth texture with non-comparison sampler"; return false; }
        if (texture->floatTexture && sampler->kind==Kind::Sampler) texture->sampleType = WGPUTextureSampleType_Float;
        if (texture->floatTexture || texture->depth) sampler->pairedFloat = true;
        else sampler->pairedInteger = true;
    }
    return true;
}
bool AppendGraphicsStageLayout(const cp2077::web::ConvertedGraphicsShader& source, WGPUShaderStage visibility,
                               std::vector<WGPUBindGroupLayoutEntry>& entries, const char*& reason) {
    using cp2077::web::GraphicsResourceKind;
    using Kind = WgslBindingDecl::Kind;
    std::vector<WgslBindingDecl> decls;
    if (!ParseWgslBindings(source.wgsl, source.wgslBytes, decls, reason)) return false;
    for (std::size_t i = 0; i<source.resourceCount; ++i) {
        const auto& resource = source.resources[i];
        if (resource.group!=0) { reason = "group != 0"; return false; }
        for (const auto& existing : entries)
            if (existing.binding==resource.binding) { reason = "duplicate reflected binding"; return false; }
        const WgslBindingDecl* decl = nullptr;
        for (const auto& candidate : decls) if (candidate.binding==resource.binding) { decl = &candidate; break; }
        if (!decl) { reason = "reflected binding not declared"; return false; }
        const bool uniform = decl->kind==Kind::Uniform;
        const bool buffer = uniform || decl->kind==Kind::ReadOnlyStorage || decl->kind==Kind::Storage;
        bool matches = false;
        switch (resource.kind) {
        case GraphicsResourceKind::ConstantBuffer:
        case GraphicsResourceKind::SamplerBias:
        case GraphicsResourceKind::DrawParameters: matches = uniform; break;
        case GraphicsResourceKind::StorageBuffer: matches = buffer && !uniform; break;
        case GraphicsResourceKind::StorageTextureAtomicBuffer:
        case GraphicsResourceKind::TextureAtomicDimensions: matches = buffer; break;
        case GraphicsResourceKind::MultilayerTable: matches = buffer && !uniform; break;
        case GraphicsResourceKind::MultilayerPool: matches = decl->kind==Kind::Texture; break;
        case GraphicsResourceKind::StorageTextureUAV: matches = decl->kind==Kind::StorageTexture; break;
        case GraphicsResourceKind::TextureSRV:
        case GraphicsResourceKind::TextureIndirection: matches = decl->kind==Kind::Texture; break;
        case GraphicsResourceKind::Sampler: matches = decl->kind==Kind::Sampler || decl->kind==Kind::ComparisonSampler; break;
        }
        if (!matches) { reason = "reflected kind does not match WGSL"; return false; }
        WGPUBindGroupLayoutEntry entry = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        entry.binding = resource.binding; entry.visibility = visibility;
        if (buffer) {
            entry.buffer = WGPU_BUFFER_BINDING_LAYOUT_INIT;
            entry.buffer.type = uniform ? WGPUBufferBindingType_Uniform :
                decl->kind==Kind::Storage ? WGPUBufferBindingType_Storage : WGPUBufferBindingType_ReadOnlyStorage;
            entry.buffer.minBindingSize = 0; // checked at draw time against the shader, as with any bound size
        } else if (decl->kind==Kind::Texture) {
            entry.texture = WGPU_TEXTURE_BINDING_LAYOUT_INIT;
            entry.texture.sampleType = decl->sampleType; entry.texture.viewDimension = decl->dimension;
            entry.texture.multisampled = WGPU_FALSE;
        } else if (decl->kind==Kind::StorageTexture) {
            entry.storageTexture = WGPU_STORAGE_TEXTURE_BINDING_LAYOUT_INIT;
            entry.storageTexture.access = decl->access; entry.storageTexture.format = decl->format;
            entry.storageTexture.viewDimension = decl->dimension;
        } else {
            entry.sampler = WGPU_SAMPLER_BINDING_LAYOUT_INIT;
            entry.sampler.type = decl->kind==Kind::ComparisonSampler ? WGPUSamplerBindingType_Comparison :
                (decl->pairedInteger && !decl->pairedFloat) ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;
        }
        entries.push_back(entry);
    }
    return true;
}
struct GraphicsLayoutEntry {
    WGPUPipelineLayout layout = nullptr;
    WGPUShaderModule vertexModule = nullptr, fragmentModule = nullptr; // [nomadperf] ovfStorage variants
    WGPUShaderModule giFragmentModule = nullptr; // [giexplicit]
    std::vector<std::uint32_t> fsStorage; // [varexplicit] fragment uniforms converted to dynamic storage
    std::uint32_t bindings[cp2077::web::kMaxGraphicsLayoutDynamicOffsets]{};
    std::uint32_t count = 0;
    std::uint64_t generation = 0;
    bool built = false, rejected = false, creating = false;
};
std::mutex graphicsLayoutMutex;
// Signalled when a pair's main-thread layout creation finishes; only callers for that pair wait on it.
std::condition_variable graphicsLayoutReady;
std::map<std::pair<const void*, const void*>, GraphicsLayoutEntry> graphicsLayouts;
struct GraphicsLayoutCreation {
    std::vector<WGPUBindGroupLayoutEntry>* entries = nullptr;
    WGPUPipelineLayout layout = nullptr;
    const cp2077::web::ConvertedGraphicsShader* vertex = nullptr; const cp2077::web::ConvertedGraphicsShader* pixel = nullptr;
    WGPUShaderModule vertexModule = nullptr, fragmentModule = nullptr; std::uint32_t storageConverted = 0;
    WGPUShaderModule giFragmentModule = nullptr; // [giexplicit]
    std::vector<std::uint32_t> fsStorage; // [varexplicit]
    std::uint32_t bindings[cp2077::web::kMaxGraphicsLayoutDynamicOffsets]{};
    std::uint32_t count = 0, uniforms = 0, limit = 0;
};
// [nomadperf] ovfStorage. The adapter allows 10 dynamic uniform buffers per layout; Nomad-area materials bind 11-15
// constant buffers, so the rest stayed static with their per-list arena offset inside the bind group. Such groups
// can never be cached (a new offset every list) and were created on the main thread for every draw: ~250-450
// creations per frame, about a third of all main-thread time. The overflow uniforms now become dynamic READ-ONLY
// STORAGE buffers (8 more dynamic slots): the WGSL declaration changes from var<uniform> to var<storage, read> (same
// memory layout; storage has no extra layout rules), the layout entry takes a dynamic offset, and the arena/uniform
// buffers carry Storage usage. A pair whose WGSL cannot be rewritten keeps the old partial layout; a pipeline that
// fails with the converted modules falls back to the auto layout like any explicit-layout failure.
// ?ovfStorage=0 = old partial layout.
bool OvfStorageOnMain() {
    static int v = -1;
    if (v < 0) { v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('ovfStorage') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 nomadperf switches: ovfStorage=%d\n", v); }
    return v != 0;
}
// Same source transformations as BeginGraphicsModule (directive hoisting, mixed integer gather lowering).
std::string GraphicsModuleCode(const cp2077::web::ConvertedGraphicsShader& source) {
    std::string code(source.wgsl, source.wgslBytes), hoisted, lowered;
    if (HoistWgslDirectives(source.wgsl, source.wgslBytes, hoisted)) code = hoisted;
    if (GatherLoweringEnabled() && LowerMixedIntegerGathers(code, lowered)) code = lowered;
    return code;
}
// Rewrites `@binding(N) ... var<uniform>` to `var<storage, read>` for every listed binding of group 0.
bool RewriteUniformsToStorage(std::string& code, const std::vector<std::uint32_t>& bindings) {
    for (auto b : bindings) {
        bool done = false;
        for (std::size_t at = code.find("@binding"); at != std::string::npos && !done; at = code.find("@binding", at + 8)) {
            std::size_t p = at + 8; while (p < code.size() && WgslSpace(code[p])) ++p;
            if (p >= code.size() || code[p] != '(') continue;
            std::uint32_t value = 0; if (!WgslNumberAfter(code, p + 1, value) || value != b) continue;
            const std::size_t end = code.find(';', p);
            const std::size_t var = code.find("var<uniform>", p);
            if (var == std::string::npos || end == std::string::npos || var > end) return false;
            code.replace(var, 12, "var<storage, read>"); done = true;
        }
        if (!done) return false;
    }
    return true;
}
WGPUShaderModule CreateStorageVariantModule(WGPUDevice device, const cp2077::web::ConvertedGraphicsShader& source,
                                            const std::vector<std::uint32_t>& bindings) {
    std::string code = GraphicsModuleCode(source);
    if (!RewriteUniformsToStorage(code, bindings)) return nullptr;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT; wgsl.code = {code.data(), code.size()};
    WGPUShaderModuleDescriptor descriptor = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    descriptor.nextInChain = &wgsl.chain; descriptor.label = {source.originalHash, WGPU_STRLEN};
    return wgpuDeviceCreateShaderModule(device, &descriptor);
}
void CreateGraphicsLayoutOnMain(void* argument) {
    auto& creation = *static_cast<GraphicsLayoutCreation*>(argument);
    const auto device = cp2077::web::MainThreadGpuDevice();
    if (!device) return;
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (wgpuDeviceGetLimits(device, &limits)!=WGPUStatus_Success) return;
    creation.limit = limits.maxDynamicUniformBuffersPerPipelineLayout;
    const std::uint32_t cap = std::min<std::uint32_t>(creation.limit, cp2077::web::kMaxGraphicsLayoutDynamicOffsets);
    // Entries are sorted by binding: the first `cap` uniforms take dynamic offsets (setBindGroup order is
    // ascending binding); any beyond the device limit stay static, with their offset in the bind group.
    for (auto& entry : *creation.entries) {
        if (entry.buffer.type!=WGPUBufferBindingType_Uniform) continue;
        ++creation.uniforms;
        if (creation.count<cap) { entry.buffer.hasDynamicOffset = WGPU_TRUE; creation.bindings[creation.count++] = entry.binding; }
    }
    if (creation.uniforms > creation.count && creation.count == cap && creation.vertex && OvfStorageOnMain()) {
        // Remaining static uniforms all have higher binding numbers than the dynamic ones (entries are sorted), so
        // appending them keeps the dynamic-offset order ascending by binding.
        const std::uint32_t room = std::min<std::uint32_t>(limits.maxDynamicStorageBuffersPerPipelineLayout,
                                                           cp2077::web::kMaxGraphicsLayoutDynamicOffsets - creation.count);
        std::vector<std::uint32_t> vs, fs; std::vector<WGPUBindGroupLayoutEntry*> picked;
        for (auto& entry : *creation.entries) {
            if (entry.buffer.type!=WGPUBufferBindingType_Uniform || entry.buffer.hasDynamicOffset) continue;
            if (picked.size() >= room) { picked.clear(); break; } // not all fit: keep the old partial layout
            picked.push_back(&entry);
            ((entry.visibility & WGPUShaderStage_Vertex) ? vs : fs).push_back(entry.binding);
        }
        WGPUShaderModule vm = nullptr, fm = nullptr; bool ok = !picked.empty();
        if (ok && !vs.empty()) ok = (vm = CreateStorageVariantModule(device, *creation.vertex, vs)) != nullptr;
        if (ok && !fs.empty()) ok = creation.pixel && (fm = CreateStorageVariantModule(device, *creation.pixel, fs)) != nullptr;
        if (ok) {
            for (auto* entry : picked) {
                entry->buffer.type = WGPUBufferBindingType_ReadOnlyStorage; entry->buffer.hasDynamicOffset = WGPU_TRUE;
                creation.bindings[creation.count++] = entry->binding;
            }
            creation.vertexModule = vm; creation.fragmentModule = fm; creation.storageConverted = std::uint32_t(picked.size());
            if (fm) creation.fsStorage = fs; // [varexplicit]
            // [giexplicit] the GI-RGB-output variant gets the same storage conversion so it can share this layout.
            if (fm) if (const auto* gi = cp2077::web::FindConvertedGraphicsGiRGBOutputVariant(creation.pixel))
                if ((creation.giFragmentModule = CreateStorageVariantModule(device, *gi, fs))) graphicsModuleSources[creation.giFragmentModule] = gi;
            // [skperf] round 68 psoRecord: the ovfStorage variant modules map back to their converted sources, so pipelines
            // built from them are recorded (as the pair's explicit layout, lay=1; the warmer then takes the same variant
            // modules from AcquireGpuGraphicsPipelineLayout). The map is read only by the recorder (?psoRecord=1).
            if (vm) graphicsModuleSources[vm] = creation.vertex;
            if (fm) graphicsModuleSources[fm] = creation.pixel;
        } else {
            if (vm) wgpuShaderModuleRelease(vm); if (fm) wgpuShaderModuleRelease(fm);
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1) < 16) std::fprintf(stderr, "CP2077 nomadperf ovfStorage not applied: VS=%s PS=%s overflow=%u room=%u\n",
                creation.vertex->originalHash, creation.pixel ? creation.pixel->originalHash : "-", creation.uniforms - creation.count, room);
        }
    }
    if (!creation.count) return;
    WGPUBindGroupLayoutDescriptor groupDescriptor = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    groupDescriptor.entryCount = creation.entries->size(); groupDescriptor.entries = creation.entries->data();
    WGPUBindGroupLayout group = wgpuDeviceCreateBindGroupLayout(device, &groupDescriptor);
    if (!group) return;
    WGPUPipelineLayoutDescriptor layoutDescriptor = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    layoutDescriptor.bindGroupLayoutCount = 1; layoutDescriptor.bindGroupLayouts = &group;
    creation.layout = wgpuDeviceCreatePipelineLayout(device, &layoutDescriptor);
    wgpuBindGroupLayoutRelease(group); // the pipeline layout keeps its own reference
}
void ReportGraphicsLayout(const char* what, const char* detail, const cp2077::web::ConvertedGraphicsShader* vertex,
                          const cp2077::web::ConvertedGraphicsShader* pixel) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1)>=64) return;
    std::fprintf(stderr, "CP2077 WebGPU: explicit graphics layout %s (%s) VS=%s PS=%s\n", what, detail,
                 vertex && vertex->originalHash ? vertex->originalHash : "-", pixel && pixel->originalHash ? pixel->originalHash : "-");
}
// [warmup] layout -> (vertex, pixel) pair, under graphicsLayoutMutex; read on main when recording.
std::unordered_map<WGPUPipelineLayout, std::pair<const cp2077::web::ConvertedGraphicsShader*, const cp2077::web::ConvertedGraphicsShader*>> graphicsLayoutPairs;
bool ExplicitLayoutPairOf(WGPUPipelineLayout layout, const cp2077::web::ConvertedGraphicsShader*& vertex,
                          const cp2077::web::ConvertedGraphicsShader*& pixel) {
    std::lock_guard<std::mutex> guard(graphicsLayoutMutex);
    auto found = graphicsLayoutPairs.find(layout);
    if (found == graphicsLayoutPairs.end()) return false;
    vertex = found->second.first; pixel = found->second.second; return true;
}
// Bumped (under graphicsLayoutMutex) by every rejection; invalidates the per-thread answers below.
std::atomic<std::uint64_t> graphicsLayoutRejects{0};
}
namespace cp2077::web {
// [tankview] True when the explicit layout declares this sampler binding NonFiltering (only paired with
// texture_*<u32/i32>, e.g. textureGather of a stencil/material-id SRV); the draw must bind a nearest sampler.
bool GraphicsSamplerIsNonFiltering(const ConvertedGraphicsShader* shader, std::uint32_t binding) {
    if (!shader || !shader->wgsl) return false;
    static std::mutex cacheMutex;
    static std::map<const char*, std::vector<std::uint32_t>> cache;
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto found = cache.find(shader->wgsl);
    if (found == cache.end()) {
        std::vector<WgslBindingDecl> decls; const char* reason = nullptr; std::vector<std::uint32_t> list;
        if (ParseWgslBindings(shader->wgsl, shader->wgslBytes, decls, reason))
            for (const auto& decl : decls)
                if (decl.kind==WgslBindingDecl::Kind::Sampler && decl.pairedInteger && !decl.pairedFloat) list.push_back(decl.binding);
        found = cache.emplace(shader->wgsl, std::move(list)).first;
    }
    for (auto b : found->second) if (b==binding) return true;
    return false;
}
// Slow path: answers under graphicsLayoutMutex and reports the rejection epoch that answer is valid for.
static bool AcquireGraphicsPipelineLayoutLocked(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                                                std::uint64_t generation, GpuGraphicsPipelineLayout& out,
                                                std::uint64_t& rejects) {
    // Released during the one-time main-thread creation (std::map references stay valid); other callers for
    // the same pair wait for it, callers for other pairs proceed.
    std::unique_lock<std::mutex> guard(graphicsLayoutMutex);
    auto& entry = graphicsLayouts[{vertex, pixel}];
    graphicsLayoutReady.wait(guard, [&entry] { return !entry.creating; });
    rejects = graphicsLayoutRejects.load(std::memory_order_relaxed);
    if (!entry.built || entry.generation!=generation) {
        // A previous generation's layout is not released: a worker may still borrow it in a pipeline request.
        entry = GraphicsLayoutEntry{}; entry.built = true; entry.generation = generation;
        std::vector<WGPUBindGroupLayoutEntry> entries; const char* reason = "unknown";
        if (!AppendGraphicsStageLayout(*vertex, WGPUShaderStage_Vertex, entries, reason) ||
            (pixel && !AppendGraphicsStageLayout(*pixel, WGPUShaderStage_Fragment, entries, reason))) {
            ReportGraphicsLayout("not used", reason, vertex, pixel);
            return false;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const WGPUBindGroupLayoutEntry& a, const WGPUBindGroupLayoutEntry& b) { return a.binding<b.binding; });
        GraphicsLayoutCreation creation; creation.entries = &entries; creation.vertex = vertex; creation.pixel = pixel;
        auto* queue = ShaderQueue();
        entry.creating = true;
        guard.unlock();
        const bool proxied = queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), CreateGraphicsLayoutOnMain, &creation);
        guard.lock();
        entry.creating = false;
        graphicsLayoutReady.notify_all();
        rejects = graphicsLayoutRejects.load(std::memory_order_relaxed);
        if (!proxied) return false;
        entry.layout = creation.layout; entry.count = creation.count;
        entry.vertexModule = creation.layout ? creation.vertexModule : nullptr; entry.fragmentModule = creation.layout ? creation.fragmentModule : nullptr;
        entry.giFragmentModule = creation.layout ? creation.giFragmentModule : nullptr; // [giexplicit]
        if (creation.layout) entry.fsStorage = creation.fsStorage; // [varexplicit]
        if (creation.storageConverted) {
            static std::atomic<unsigned> converted{0}; const unsigned n = ++converted;
            if (n <= 4 || (n & (n - 1)) == 0) std::fprintf(stderr, "CP2077 nomadperf ovfStorage layouts=%u (last: uniforms=%u dynamicUniform=%u storageConverted=%u)\n",
                n, creation.uniforms, creation.count - creation.storageConverted, creation.storageConverted);
        }
        std::copy(creation.bindings, creation.bindings+creation.count, entry.bindings);
        if (creation.layout && creation.uniforms>creation.count && !creation.storageConverted) {
            char detail[96];
            std::snprintf(detail, sizeof(detail), "uniforms=%u dynamic=%u limit=%u", creation.uniforms, creation.count, creation.limit);
            ReportGraphicsLayout("partially dynamic", detail, vertex, pixel);
        }
        static std::atomic<unsigned> created{0};
        const unsigned n = creation.layout ? ++created : created.load();
        if (creation.layout && (n<=4 || (n&(n-1))==0))
            std::fprintf(stderr, "CP2077 WebGPU: explicit graphics layouts created=%u (dynamic uniforms=%u limit=%u)\n",
                         n, creation.count, creation.limit);
    }
    if (entry.layout) graphicsLayoutPairs[entry.layout] = {vertex, pixel}; // [warmup] recording
    if (entry.rejected || !entry.layout) return false;
    out.layout = entry.layout; out.dynamicCount = entry.count;
    out.vertexModule = entry.vertexModule; out.fragmentModule = entry.fragmentModule;
    out.giFragmentModule = entry.giFragmentModule; // [giexplicit]
    std::copy(entry.bindings, entry.bindings+entry.count, out.dynamicBindings);
    return true;
}
bool AcquireGpuGraphicsPipelineLayout(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                                      GpuGraphicsPipelineLayout& out) {
    out = GpuGraphicsPipelineLayout{};
    if (!vertex || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return false;
    const auto generation = GpuDeviceGeneration();
    // Runs for every draw on every render thread: a pair's answer is final for a device generation unless the
    // pair is rejected, so a per-thread direct-mapped copy (checked against the rejection epoch) avoids the
    // global mutex and map lookup on repeat draws. Cached layouts are never released, so copies stay valid.
    struct ThreadSlot {
        const void* vertex = nullptr; const void* pixel = nullptr;
        std::uint64_t generation = 0, rejects = 0; bool valid = false, ok = false;
        GpuGraphicsPipelineLayout layout;
    };
    static thread_local ThreadSlot slots[64];
    const std::uintptr_t mix = (reinterpret_cast<std::uintptr_t>(vertex) >> 3) * 31u + (reinterpret_cast<std::uintptr_t>(pixel) >> 3);
    auto& slot = slots[(mix ^ (mix >> 6) ^ (mix >> 12)) & 63];
    if (slot.valid && slot.vertex==vertex && slot.pixel==pixel && slot.generation==generation &&
        slot.rejects==graphicsLayoutRejects.load(std::memory_order_acquire)) {
        if (slot.ok) out = slot.layout;
        return slot.ok;
    }
    std::uint64_t rejects = 0;
    const bool ok = AcquireGraphicsPipelineLayoutLocked(vertex, pixel, generation, out, rejects);
    slot.vertex = vertex; slot.pixel = pixel; slot.generation = generation; slot.rejects = rejects;
    slot.ok = ok; slot.layout = ok ? out : GpuGraphicsPipelineLayout{}; slot.valid = true;
    return ok;
}
void RejectGpuGraphicsPipelineLayout(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel) {
    std::lock_guard<std::mutex> guard(graphicsLayoutMutex);
    auto found = graphicsLayouts.find({vertex, pixel});
    if (found==graphicsLayouts.end() || found->second.rejected) return;
    found->second.rejected = true;
    graphicsLayoutRejects.fetch_add(1, std::memory_order_release);
    ReportGraphicsLayout("rejected", "pipeline failed with explicit layout, auto layout used", vertex, pixel);
}
}
namespace cp2077::web {
WGPUShaderModule AcquireGpuFragmentStorageVariant(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                                                  const ConvertedGraphicsShader* variant) {
    if (!vertex || !pixel || !variant || emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    const auto generation = GpuDeviceGeneration();
    struct Key { const void* v; const void* p; const void* x; bool operator<(const Key& o) const { return std::tie(v, p, x) < std::tie(o.v, o.p, o.x); } };
    struct Item { WGPUShaderModule module = nullptr; std::uint64_t generation = 0; bool done = false; };
    static std::mutex mutex; static std::map<Key, Item> items;
    std::vector<std::uint32_t> bindings;
    {
        std::lock_guard<std::mutex> guard(mutex);
        auto& item = items[Key{vertex, pixel, variant}];
        if (item.done && item.generation == generation) return item.module;
    }
    {
        std::lock_guard<std::mutex> guard(graphicsLayoutMutex);
        auto found = graphicsLayouts.find({vertex, pixel});
        if (found == graphicsLayouts.end() || found->second.generation != generation || !found->second.layout || found->second.fsStorage.empty()) return nullptr;
        bindings = found->second.fsStorage;
    }
    struct Request { const ConvertedGraphicsShader* source; const std::vector<std::uint32_t>* bindings; WGPUShaderModule module = nullptr; } request{variant, &bindings};
    auto* queue = ShaderQueue();
    const bool proxied = queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), [](void* argument) {
        auto& r = *static_cast<Request*>(argument);
        if (const auto device = MainThreadGpuDevice()) {
            r.module = CreateStorageVariantModule(device, *r.source, *r.bindings);
            if (r.module) graphicsModuleSources[r.module] = r.source;
        }
    }, &request);
    std::lock_guard<std::mutex> guard(mutex);
    auto& item = items[Key{vertex, pixel, variant}];
    if (item.done && item.generation == generation) { if (request.module) wgpuShaderModuleRelease(request.module); return item.module; }
    item.module = proxied ? request.module : nullptr; item.generation = generation; item.done = proxied;
    static std::atomic<unsigned> made{0}; const unsigned n = ++made;
    if (n <= 8 || (n & (n - 1)) == 0) std::fprintf(stderr, "CP2077 varexplicit storage variants=%u (last ok=%d PS=%s)\n", n, int(item.module != nullptr), pixel->originalHash ? pixel->originalHash : "?");
    return item.module;
}
}
#include "gpu-pipeline-warmup-web.inl"
