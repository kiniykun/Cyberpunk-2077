#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include <webgpu/webgpu.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <string>
#include "gpu-api-vertex-web.h"
#include "gpu-shader-web.h"

namespace {
using namespace GpuWrapApi::VertexPacking;
WGPUVertexFormat PackingFormat(ePackingType type) {
    switch (type) {
    case PT_Float1: return WGPUVertexFormat_Float32;
    case PT_Float2: return WGPUVertexFormat_Float32x2;
    case PT_Float3: return WGPUVertexFormat_Float32x3;
    case PT_Float4: return WGPUVertexFormat_Float32x4;
    case PT_Float16_2: return WGPUVertexFormat_Float16x2;
    case PT_Float16_4: return WGPUVertexFormat_Float16x4;
    case PT_UShort2: return WGPUVertexFormat_Uint16x2;
    case PT_UShort4: return WGPUVertexFormat_Uint16x4;
    case PT_UShort4N: return WGPUVertexFormat_Unorm16x4;
    case PT_Short2: return WGPUVertexFormat_Sint16x2;
    case PT_Short4: return WGPUVertexFormat_Sint16x4;
    case PT_Short4N: return WGPUVertexFormat_Snorm16x4;
    case PT_UInt1: return WGPUVertexFormat_Uint32;
    case PT_UInt2: return WGPUVertexFormat_Uint32x2;
    case PT_UInt3: return WGPUVertexFormat_Uint32x3;
    case PT_UInt4: return WGPUVertexFormat_Uint32x4;
    case PT_Int1: return WGPUVertexFormat_Sint32;
    case PT_Int2: return WGPUVertexFormat_Sint32x2;
    case PT_Int3: return WGPUVertexFormat_Sint32x3;
    case PT_Int4: return WGPUVertexFormat_Sint32x4;
    case PT_Color: case PT_UByte4N: return WGPUVertexFormat_Unorm8x4;
    case PT_UByte4: return WGPUVertexFormat_Uint8x4;
    case PT_Byte4N: return WGPUVertexFormat_Snorm8x4;
    case PT_Dec4: return WGPUVertexFormat_Unorm10_10_10_2;
    default: return static_cast<WGPUVertexFormat>(0);
    }
}
std::mutex layoutMutex;
std::unordered_map<Uint32, std::unique_ptr<GpuWrapApi::VertexLayoutDesc>> layouts;
Uint32 nextLayout = 1;
bool SameLayout(const GpuWrapApi::VertexLayoutDesc& a, const GpuWrapApi::VertexLayoutDesc& b) {
    if (a.m_elements.Size() != b.m_elements.Size() || a.m_slotStrides.Size() != b.m_slotStrides.Size() ||
        a.m_slotMask != b.m_slotMask) return false;
    for (Uint32 i=0; i<a.m_elements.Size(); ++i) if (a.m_elements[i] != b.m_elements[i]) return false;
    for (Uint32 i=0; i<a.m_slotStrides.Size(); ++i) if (a.m_slotStrides[i] != b.m_slotStrides[i]) return false;
    return true;
}
}
namespace GpuApi {
Bool IsDescSupported(const GpuWrapApi::VertexLayoutDesc& desc) {
    if (desc.m_elements.Size() > VERTEX_LAYOUT_MAX_ELEMENTS || desc.m_slotStrides.Size() > VERTEX_LAYOUT_MAX_SLOTS)
        return false;
    std::array<Uint32, VERTEX_LAYOUT_MAX_SLOTS> offsets{};
    std::array<Int32, VERTEX_LAYOUT_MAX_SLOTS> streams; streams.fill(ST_Invalid);
    std::array<bool, PS_Max*VERTEX_LAYOUT_USAGE_INDEX_COUNT> used{};
    for (const auto& element : desc.m_elements) {
        if (element.IsEmpty()) break;
        if (element.m_type < 0 || element.m_type >= PT_Max || element.m_usage < 0 || element.m_usage >= PS_Max ||
            element.m_streamIndex >= desc.m_slotStrides.Size() ||
            element.m_usageIndex >= VERTEX_LAYOUT_USAGE_INDEX_COUNT ||
            (element.m_streamType != ST_PerVertex && element.m_streamType != ST_PerInstance)) return false;
        const Uint32 slot = element.m_streamIndex;
        if (streams[slot] != ST_Invalid && streams[slot] != element.m_streamType) return false;
        streams[slot] = element.m_streamType;
        const auto size = GetPackingTypeSize(element.m_type);
        if (!size) return false;
        if (element.m_usage != PS_Padding) {
            if (!PackingFormat(element.m_type)) return false;
            const Uint32 key = element.m_usage*VERTEX_LAYOUT_USAGE_INDEX_COUNT + element.m_usageIndex;
            if (used[key]) return false;
            used[key] = true;
            const AnsiChar* semantic = nullptr; Uint32 index = 0;
            if (!GpuWrapApi::MapPackingElementToSemanticAndIndex(GpuWrapApi::VERTPACKPLATFORM_NonOrbis,
                    element, semantic, index) || !semantic) return false;
            // WebGPU offsets align to the smaller of 4 and the format size.
            if (offsets[slot] % (size < 4 ? size : 4)) return false;
        }
        offsets[slot] += size;
        if (offsets[slot] > desc.m_slotStrides[slot]) return false;
    }
    for (Uint32 slot=0; slot<desc.m_slotStrides.Size(); ++slot)
        if (offsets[slot] && desc.m_slotStrides[slot] % 4) return false;
    return true;
}
VertexLayoutRef GetVertexLayout(const GpuWrapApi::VertexLayoutDesc& desc) {
    if (!IsDescSupported(desc)) {
        std::fputs("CP2077 WebGPU: unsupported native vertex packing layout\n", stderr);
        return VertexLayoutRef::Null();
    }
    std::lock_guard<std::mutex> lock(layoutMutex);
    for (const auto& entry : layouts) if (SameLayout(*entry.second, desc)) return VertexLayoutRef{entry.first};
    if (nextLayout > Uint32(INT32_MAX)) return VertexLayoutRef::Null();
    const auto id=nextLayout++;
    layouts.emplace(id, std::make_unique<GpuWrapApi::VertexLayoutDesc>(desc));
    return VertexLayoutRef{id};
}
const GpuWrapApi::VertexLayoutDesc* GetVertexLayoutDesc(const VertexLayoutRef& reference) {
    std::lock_guard<std::mutex> lock(layoutMutex);
    auto entry=layouts.find(reference.ID());
    return entry==layouts.end() ? nullptr : entry->second.get();
}
Uint8 GetVertexLayoutStride(const VertexLayoutRef& reference, Uint32 slot) {
    auto* desc=GetVertexLayoutDesc(reference);
    return desc && slot<desc->m_slotStrides.Size() ? desc->m_slotStrides[slot] : 0;
}
}
namespace {
bool EqualSemantic(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b) {
        const auto upper=[](unsigned char value) { return value >= 'a' && value <= 'z' ? value-'a'+'A' : value; };
        if (upper(*a) != upper(*b)) return false;
    }
    return *a == *b;
}
bool SemanticPrefix(const char* value, const char* prefix) {
    if (!value || !prefix) return false;
    for (; *prefix; ++value, ++prefix) {
        const auto upper=[](unsigned char c) { return c >= 'a' && c <= 'z' ? c-'a'+'A' : c; };
        if (!*value || upper(*value) != upper(*prefix)) return false;
    }
    return true;
}
std::size_t RegisteredLayoutCount() {
    std::lock_guard<std::mutex> lock(layoutMutex);
    return layouts.size();
}
}
namespace cp2077::web {
namespace {
bool BuildFromLayoutDesc(const GpuWrapApi::VertexLayoutDesc& desc, const ConvertedGraphicsShader& shader,
                         std::uint32_t strideCount, const std::uint8_t* strides, GpuVertexBufferLayouts& output) {
    GpuVertexBufferLayouts result;
    result.attributes.resize(GpuApi::VERTEX_LAYOUT_MAX_SLOTS);
    std::array<Uint32, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> offsets{};
    std::array<EStreamType, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> step;
    step.fill(ST_Invalid);
    std::vector<bool> matched(shader.vertexInputCount, false);
    Uint32 buffersNeeded = 0;
    for (const auto& element : desc.m_elements) {
        if (element.IsEmpty()) break;
        const Uint32 slot = element.m_streamIndex;
        const Uint32 offset = offsets[slot];
        const Uint32 bytes = GetPackingTypeSize(element.m_type);
        offsets[slot] += bytes;
        if (element.m_usage == PS_Padding) continue;
        const AnsiChar* semantic = nullptr; Uint32 index = 0;
        if (!GpuWrapApi::MapPackingElementToSemanticAndIndex(GpuWrapApi::VERTPACKPLATFORM_NonOrbis,
                element, semantic, index)) return false;
        for (std::size_t i = 0; i < shader.vertexInputCount; ++i) {
            const auto& input = shader.vertexInputs[i];
            if (input.semanticIndex != index || !EqualSemantic(input.semantic, semantic)) continue;
            if (matched[i]) return false;
            const Uint32 stride = strides ? (slot < strideCount ? strides[slot] : 0) : desc.m_slotStrides[slot];
            const auto format = PackingFormat(element.m_type);
            if (!format || !stride || stride % 4 || offset > stride || bytes > stride-offset) return false;
            WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
            attribute.format = format; attribute.offset = offset; attribute.shaderLocation = input.location;
            result.attributes[slot].push_back(attribute);
            step[slot] = element.m_streamType;
            if (slot+1 > buffersNeeded) buffersNeeded = slot+1;
            matched[i] = true;
        }
    }
    for (bool found : matched) if (!found) return false;
    result.buffers.resize(buffersNeeded);
    result.attributes.resize(buffersNeeded);
    for (Uint32 slot = 0; slot < buffersNeeded; ++slot) {
        auto& buffer = result.buffers[slot];
        buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        // Undefined step mode encodes a null WebGPU layout slot. An empty
        // non-null layout still requires a binding, unlike the native gap.
        if(result.attributes[slot].empty())continue;
        buffer.stepMode = step[slot] == ST_PerInstance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        buffer.arrayStride = result.attributes[slot].empty() ? 0 :
            (strides ? strides[slot] : desc.m_slotStrides[slot]);
        buffer.attributeCount = result.attributes[slot].size();
        buffer.attributes = result.attributes[slot].data();
    }
    output = std::move(result);
    return true;
}
// Vertex layout used for a draw whose engine-side layout is Null; cached per (shader, bound strides).
struct FallbackPlan {
    std::vector<std::vector<WGPUVertexAttribute>> attributes;
    std::vector<WGPUVertexStepMode> steps;
};
bool SamePlan(const FallbackPlan& a, const FallbackPlan& b) {
    if (a.attributes.size() != b.attributes.size() || a.steps != b.steps) return false;
    for (std::size_t slot = 0; slot < a.attributes.size(); ++slot) {
        const auto& x = a.attributes[slot]; const auto& y = b.attributes[slot];
        if (x.size() != y.size()) return false;
        for (std::size_t i = 0; i < x.size(); ++i)
            if (x[i].format != y[i].format || x[i].offset != y[i].offset || x[i].shaderLocation != y[i].shaderLocation)
                return false;
    }
    return true;
}
FallbackPlan PlanFromBuilt(const GpuVertexBufferLayouts& built) {
    FallbackPlan plan;
    plan.attributes.resize(built.buffers.size());
    plan.steps.resize(built.buffers.size(), WGPUVertexStepMode_Undefined);
    for (std::size_t slot = 0; slot < built.buffers.size(); ++slot) {
        if (slot < built.attributes.size()) plan.attributes[slot] = built.attributes[slot];
        if (!plan.attributes[slot].empty()) plan.steps[slot] = built.buffers[slot].stepMode;
    }
    return plan;
}
void ApplyPlan(const FallbackPlan& plan, const std::uint8_t* strides, GpuVertexBufferLayouts& output) {
    GpuVertexBufferLayouts result;
    result.attributes = plan.attributes;
    result.buffers.resize(plan.attributes.size());
    for (std::size_t slot = 0; slot < result.buffers.size(); ++slot) {
        auto& buffer = result.buffers[slot];
        buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
        if (result.attributes[slot].empty()) continue;
        buffer.stepMode = plan.steps[slot]; buffer.arrayStride = strides[slot];
        buffer.attributeCount = result.attributes[slot].size();
        buffer.attributes = result.attributes[slot].data();
    }
    output = std::move(result);
}
// Tier 1: the engine registered the real layout for this geometry (renderMesh.cxx GetVertexLayout), but the
// command carries Null. Accept only if every registered desc that covers all shader inputs, with per-vertex slot
// strides equal to the bound strides, yields the same attribute set.
bool MatchRegisteredLayouts(const ConvertedGraphicsShader& shader, const std::uint8_t* strides,
                            FallbackPlan& plan, Uint32& chosenId, Uint32& candidates) {
    std::vector<std::pair<Uint32, const GpuWrapApi::VertexLayoutDesc*>> snapshot;
    {
        std::lock_guard<std::mutex> lock(layoutMutex);
        snapshot.reserve(layouts.size());
        for (const auto& entry : layouts) snapshot.emplace_back(entry.first, entry.second.get());
    }
    std::sort(snapshot.begin(), snapshot.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    candidates = 0; chosenId = 0;
    bool ambiguous = false;
    for (const auto& candidateDesc : snapshot) {
        const auto* desc = candidateDesc.second;
        GpuVertexBufferLayouts built;
        if (!BuildFromLayoutDesc(*desc, shader, GpuApi::VERTEX_LAYOUT_MAX_SLOTS, strides, built)) continue;
        bool exact = true;
        for (std::size_t slot = 0; slot < built.buffers.size(); ++slot)
            if (built.buffers[slot].stepMode == WGPUVertexStepMode_Vertex &&
                desc->m_slotStrides[slot] != strides[slot]) { exact = false; break; }
        if (!exact) continue;
        FallbackPlan candidate = PlanFromBuilt(built);
        if (!candidates++) { plan = std::move(candidate); chosenId = candidateDesc.first; }
        else if (!SamePlan(plan, candidate)) ambiguous = true;
    }
    return candidates && !ambiguous;
}
// Tier 2 (last resort): guess formats from semantics so inputs, in location order, exactly fill the bound
// streams. Byte sizes cannot reveal order inside a stream, so a stream that interleaves semantics
// (e.g. BLENDINDICES, BLENDWEIGHT, BLENDINDICES) is rejected, as is any non-unique size assignment.
struct GuessOption { Uint32 size; WGPUVertexFormat format; };
struct GuessPlacement { Uint32 slot, offset; GuessOption option; };
std::vector<GuessOption> GuessOptions(const GraphicsVertexInput& input) {
    std::vector<GuessOption> out;
    const Uint32 c = input.components;
    if (c < 1 || c > 4) return out;
    const auto add = [&](Uint32 size, WGPUVertexFormat format) {
        for (const auto& option : out) if (option.size == size) return;
        out.push_back({size, format});
    };
    static const WGPUVertexFormat floats[5] = {WGPUVertexFormat(0), WGPUVertexFormat_Float32,
        WGPUVertexFormat_Float32x2, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x4};
    static const WGPUVertexFormat uints[5] = {WGPUVertexFormat(0), WGPUVertexFormat_Uint32,
        WGPUVertexFormat_Uint32x2, WGPUVertexFormat_Uint32x3, WGPUVertexFormat_Uint32x4};
    const char* semantic = input.semantic;
    if (EqualSemantic(semantic, "BLENDINDICES")) {
        add(4, WGPUVertexFormat_Uint8x4); add(8, WGPUVertexFormat_Uint16x4); add(4*c, uints[c]);
    } else if (SemanticPrefix(semantic, "INSTANCE_SKINNING")) {
        add(4*c, uints[c]);
    } else if (EqualSemantic(semantic, "POSITION")) {
        add(4*c, floats[c]); if (c >= 3) add(8, WGPUVertexFormat_Snorm16x4);
    } else if (EqualSemantic(semantic, "COLOR") || EqualSemantic(semantic, "BLENDWEIGHT")) {
        add(4, WGPUVertexFormat_Unorm8x4); add(4*c, floats[c]);
    } else if (EqualSemantic(semantic, "NORMAL") || EqualSemantic(semantic, "TANGENT")) {
        if (c >= 3) add(4, WGPUVertexFormat_Unorm10_10_10_2);
        add(4*c, floats[c]); if (c >= 3) add(8, WGPUVertexFormat_Float16x4);
    } else {
        add(4*c, floats[c]);
        if (c == 2) add(4, WGPUVertexFormat_Float16x2);
        if (c >= 3) add(8, WGPUVertexFormat_Float16x4);
    }
    return out;
}
struct LayoutGuess {
    std::vector<const GraphicsVertexInput*> inputs;
    std::vector<std::vector<GuessOption>> options;
    std::vector<Uint32> slots;
    const std::uint8_t* strides = nullptr;
    std::vector<GuessPlacement> current, found;
    unsigned solutions = 0, budget = 200000;
    // kind: 0 = stream still empty, 1 = per-vertex, 2 = per-instance (INSTANCE_* semantics only).
    void Search(std::size_t input, std::size_t stream, Uint32 offset, int kind) {
        if (solutions > 1 || !budget) return;
        --budget;
        if (stream >= slots.size()) return;
        const Uint32 stride = strides[slots[stream]];
        const bool closable = !offset || (kind == 2 ? offset <= stride : offset == stride);
        if (input == inputs.size()) {
            if (closable && ++solutions == 1) found = current;
            return;
        }
        const int inputKind = SemanticPrefix(inputs[input]->semantic, "INSTANCE_") ? 2 : 1;
        if (!kind || kind == inputKind) for (const auto& option : options[input]) {
            const Uint32 align = option.size < 4 ? option.size : 4;
            if (offset % align || offset + option.size > stride) continue;
            current.push_back({slots[stream], offset, option});
            Search(input+1, stream, offset+option.size, inputKind);
            current.pop_back();
        }
        if (closable) Search(input, stream+1, 0, 0);
    }
};
bool GuessLayout(const ConvertedGraphicsShader& shader, const std::uint8_t* strides, FallbackPlan& plan) {
    LayoutGuess guess;
    guess.strides = strides;
    for (Uint32 slot = 0; slot < GpuApi::VERTEX_LAYOUT_MAX_SLOTS; ++slot)
        if (strides[slot] && strides[slot] % 4 == 0) guess.slots.push_back(slot);
    for (std::size_t i = 0; i < shader.vertexInputCount; ++i) guess.inputs.push_back(&shader.vertexInputs[i]);
    std::stable_sort(guess.inputs.begin(), guess.inputs.end(), [](auto* a, auto* b) { return a->location < b->location; });
    for (const auto* input : guess.inputs) {
        guess.options.push_back(GuessOptions(*input));
        if (guess.options.back().empty()) return false;
    }
    guess.Search(0, 0, 0, 0);
    if (guess.solutions != 1 || guess.found.size() != guess.inputs.size()) return false;
    plan = FallbackPlan{};
    plan.attributes.resize(GpuApi::VERTEX_LAYOUT_MAX_SLOTS);
    plan.steps.resize(GpuApi::VERTEX_LAYOUT_MAX_SLOTS, WGPUVertexStepMode_Undefined);
    std::array<const char*, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> lastSemantic{};
    std::array<std::vector<const char*>, GpuApi::VERTEX_LAYOUT_MAX_SLOTS> closedSemantics;
    Uint32 needed = 0;
    for (std::size_t i = 0; i < guess.found.size(); ++i) {
        const auto& placed = guess.found[i];
        const auto* input = guess.inputs[i];
        const Uint32 slot = placed.slot;
        if (!lastSemantic[slot] || !EqualSemantic(lastSemantic[slot], input->semantic)) {
            for (const char* closed : closedSemantics[slot]) if (EqualSemantic(closed, input->semantic)) return false;
            if (lastSemantic[slot]) closedSemantics[slot].push_back(lastSemantic[slot]);
            lastSemantic[slot] = input->semantic;
        }
        WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
        attribute.format = placed.option.format; attribute.offset = placed.offset;
        attribute.shaderLocation = input->location;
        plan.attributes[slot].push_back(attribute);
        plan.steps[slot] = SemanticPrefix(input->semantic, "INSTANCE_") ? WGPUVertexStepMode_Instance
                                                                         : WGPUVertexStepMode_Vertex;
        if (slot+1 > needed) needed = slot+1;
    }
    plan.attributes.resize(needed);
    plan.steps.resize(needed);
    return needed != 0;
}
struct FallbackEntry {
    std::shared_ptr<const FallbackPlan> plan;
    std::size_t layoutCount = 0;
    bool registeredMatch = false;
};
std::mutex fallbackMutex;
std::map<std::pair<const ConvertedGraphicsShader*, std::uint64_t>, FallbackEntry> fallbackCache;
std::set<std::string> fallbackLogged;
void LogFallback(const ConvertedGraphicsShader& shader, const std::uint8_t* strides, const char* source,
                 Uint32 layoutId, Uint32 candidates, std::size_t registered, const FallbackPlan* plan) {
    const char* hash = shader.originalHash ? shader.originalHash : "?";
    {
        std::lock_guard<std::mutex> lock(fallbackMutex);
        if (fallbackLogged.size() >= 128 || !fallbackLogged.insert(std::string(hash) + ":" + source).second) return;
    }
    std::string line;
    char part[128];
    std::snprintf(part, sizeof(part), "CP2077 vertex layout fallback: %s VS=%s", source, hash);
    line += part;
    std::snprintf(part, sizeof(part), " layoutId=%u candidates=%u registered=%zu inputs=%zu strides=",
        unsigned(layoutId), unsigned(candidates), registered, shader.vertexInputCount);
    line += part;
    for (Uint32 slot = 0; slot < GpuApi::VERTEX_LAYOUT_MAX_SLOTS; ++slot) {
        std::snprintf(part, sizeof(part), slot ? ",%u" : "%u", unsigned(strides[slot]));
        line += part;
    }
    if (plan) for (std::size_t slot = 0; slot < plan->attributes.size(); ++slot)
        for (const auto& attribute : plan->attributes[slot]) {
            std::snprintf(part, sizeof(part), " s%zu%s@%u:fmt0x%x->loc%u", slot,
                plan->steps[slot] == WGPUVertexStepMode_Instance ? "i" : "", unsigned(attribute.offset),
                unsigned(attribute.format), unsigned(attribute.shaderLocation));
            line += part;
        }
    line += '\n';
    std::fputs(line.c_str(), stderr);
}
bool BuildNullLayoutFallback(const ConvertedGraphicsShader& shader, std::uint32_t strideCount,
                             const std::uint8_t* strides, GpuVertexBufferLayouts& output) {
    static_assert(GpuApi::VERTEX_LAYOUT_MAX_SLOTS <= 8, "strides are packed into 64 bits");
    std::uint8_t bound[GpuApi::VERTEX_LAYOUT_MAX_SLOTS]{};
    std::uint64_t packed = 0;
    for (Uint32 slot = 0; slot < GpuApi::VERTEX_LAYOUT_MAX_SLOTS && slot < strideCount; ++slot) {
        bound[slot] = strides[slot];
        packed |= std::uint64_t(strides[slot]) << (8*slot);
    }
    if (!packed) return false;
    const auto key = std::make_pair(&shader, packed);
    // Per-thread front for final (registered-match) plans, keyed exactly like fallbackCache: skinned body meshes
    // hit this on every draw, and the shared mutex+map lookup serialized the render workers (perf7).
    struct RecentPlan { const ConvertedGraphicsShader* shader = nullptr; std::uint64_t packed = 0; std::shared_ptr<const FallbackPlan> plan; };
    static thread_local RecentPlan recent[64];
    auto& front = recent[((reinterpret_cast<std::uintptr_t>(&shader) >> 4) ^ packed ^ (packed >> 29)) & 63];
    if (front.plan && front.shader == &shader && front.packed == packed) { ApplyPlan(*front.plan, bound, output); return true; }
    std::size_t registered = 0;
    std::shared_ptr<const FallbackPlan> plan;
    bool cached = false;
    bool isFinal = false;
    {
        std::lock_guard<std::mutex> lock(fallbackMutex);
        auto entry = fallbackCache.find(key);
        if (entry != fallbackCache.end() && entry->second.registeredMatch) { plan = entry->second.plan; cached = true; isFinal = true; }
    }
    if (!cached) {
        // Failed or guessed lookups are retried once more layouts have been registered (meshes stream in later),
        // so an early guess is replaced by the engine's registered layout when it appears.
        registered = RegisteredLayoutCount();
        std::lock_guard<std::mutex> lock(fallbackMutex);
        auto entry = fallbackCache.find(key);
        if (entry != fallbackCache.end() && entry->second.layoutCount == registered) { plan = entry->second.plan; cached = true; }
    }
    if (!cached) {
        FallbackPlan computed;
        Uint32 layoutId = 0, candidates = 0;
        const char* source = nullptr;
        if (MatchRegisteredLayouts(shader, bound, computed, layoutId, candidates)) source = "registered";
        else if (GuessLayout(shader, bound, computed)) source = "guessed";
        if (source) plan = std::make_shared<const FallbackPlan>(std::move(computed));
        LogFallback(shader, bound, source ? source : "none", layoutId, candidates, registered, plan.get());
        std::lock_guard<std::mutex> lock(fallbackMutex);
        auto& entry = fallbackCache[key];
        entry.plan = plan; entry.layoutCount = registered; entry.registeredMatch = source && source[0] == 'r';
        isFinal = entry.registeredMatch;
    }
    if (!plan) return false;
    if (isFinal) { front.shader = &shader; front.packed = packed; front.plan = plan; }
    ApplyPlan(*plan, bound, output);
    return true;
}
}
bool BuildGpuVertexBufferLayouts(const GpuApi::VertexLayoutRef& reference, const ConvertedGraphicsShader& shader,
                                std::uint32_t strideCount, const std::uint8_t* strides,
                                GpuVertexBufferLayouts& output) {
    output = GpuVertexBufferLayouts{};
    if (shader.stage != GraphicsShaderStage::Vertex ||
        strideCount > GpuApi::VERTEX_LAYOUT_MAX_SLOTS || (strideCount && !strides) ||
        (shader.vertexInputCount && !shader.vertexInputs)) return false;
    if (!shader.vertexInputCount) return true;
    const auto* desc = GpuApi::GetVertexLayoutDesc(reference);
    if (!desc && reference.isNull() && strideCount && strides[0]) {
        // No input layout bound (DX12 ignores setInputLayout; ink/UI draw buffers rely on the PSO). Derive a packed
        // slot-0 layout from the shader inputs in location order: float32 per component, COLOR as unorm8x4 when that
        // is what makes the packing match the bound stride (ink vertex: float3 pos, float2 uv, rgba8 color = 24 bytes).
        std::vector<const GraphicsVertexInput*> ordered;
        for (std::size_t i = 0; i < shader.vertexInputCount; ++i) ordered.push_back(&shader.vertexInputs[i]);
        std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->location < b->location; });
        static const WGPUVertexFormat floats[5] = {WGPUVertexFormat(0), WGPUVertexFormat_Float32,
            WGPUVertexFormat_Float32x2, WGPUVertexFormat_Float32x3, WGPUVertexFormat_Float32x4};
        for (int packedColor = 0; packedColor < 2; ++packedColor) {
            std::vector<WGPUVertexAttribute> attributes;
            Uint32 offset = 0;
            bool ok = true;
            for (const auto* input : ordered) {
                const bool color = packedColor && EqualSemantic(input->semantic, "COLOR") && input->components == 4;
                if (input->components < 1 || input->components > 4) { ok = false; break; }
                WGPUVertexAttribute attribute = WGPU_VERTEX_ATTRIBUTE_INIT;
                attribute.format = color ? WGPUVertexFormat_Unorm8x4 : floats[input->components];
                attribute.offset = offset; attribute.shaderLocation = input->location;
                attributes.push_back(attribute);
                offset += color ? 4 : 4 * input->components;
            }
            if (!ok || offset != strides[0]) continue;
            GpuVertexBufferLayouts derived;
            derived.attributes.push_back(std::move(attributes));
            derived.buffers.push_back(WGPU_VERTEX_BUFFER_LAYOUT_INIT);
            auto& buffer = derived.buffers[0];
            buffer.stepMode = WGPUVertexStepMode_Vertex; buffer.arrayStride = strides[0];
            buffer.attributeCount = derived.attributes[0].size(); buffer.attributes = derived.attributes[0].data();
            output = std::move(derived);
            return true;
        }
    }
    // Multi-stream geometry with a Null layout (e.g. skinned instanced meshes, VS 6443099603939483409 with strides
    // 24,4,8,8 + slot 7): recover the registered engine layout, else a unique semantic guess, else skip the draw.
    if (!desc && reference.isNull()) return BuildNullLayoutFallback(shader, strideCount, strides, output);
    if (!desc) return false;
    return BuildFromLayoutDesc(*desc, shader, strideCount, strides, output);
}
}
