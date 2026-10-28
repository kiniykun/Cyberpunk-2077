#include "gpu-uniform-web.h"
#include <cstring>

namespace cp2077::web {
#include "gpu-uniform-layouts.inc"
#include "gpu-graphics-uniform-layouts.inc"
#include "gpu-static-graphics-uniform-layouts.inc"
#include "gpu-material-graphics-uniform-layouts.inc"

// shader already has its m_ prefix removed.
static const UniformLayout* ScanUniformLayouts(const char* shader, std::uint32_t group,
                                               std::uint32_t binding) {
    for (const auto& layout : kMaterialGraphicsUniformLayouts)
        if (layout.group == group && layout.binding == binding &&
            std::strcmp(shader, layout.shader) == 0) return &layout;
    for (const auto& layout : kUniformLayouts)
        if (layout.group == group && layout.binding == binding &&
            std::strcmp(shader, layout.shader) == 0) return &layout;
    for (const auto& layout : kGraphicsUniformLayouts)
        if (layout.group == group && layout.binding == binding &&
            std::strcmp(shader, layout.shader) == 0) return &layout;
    for (const auto& layout : kStaticGraphicsUniformLayouts)
        if (layout.group == group && layout.binding == binding &&
            std::strcmp(shader, layout.shader) == 0) return &layout;
    return nullptr;
}

const UniformLayout* FindUniformLayout(const char* shader, std::uint32_t group,
                                      std::uint32_t binding) {
    if (!shader) return nullptr;
    // Runtime registry names carry the original m_ prefix. Conversion tables
    // retain the same variant identity without that prefix.
    if (std::strncmp(shader, "m_", 2) == 0) shader += 2;
    // Draws look up every constant buffer of both stages on every draw and the tables hold ~1350 rows, so a
    // linear scan per lookup cost microseconds. Per-thread direct-mapped cache keyed by (name pointer, group,
    // binding). A hit is confirmed by comparing the name text, so a reused or non-static name pointer can never
    // return another shader's layout. Misses (nullptr) are not cached and keep scanning.
    // [negcache] misses are cached too (most per-draw lookups of buffers without a converted layout used to rescan
    // all rows every draw), confirmed by an FNV-1a hash of the name text; 2048 slots (was 512) to cut collisions.
    struct Slot { const char* shader; std::uint32_t group, binding; const UniformLayout* layout; std::uint64_t nameHash; bool used; };
    static thread_local Slot cache[2048]{};
    std::uint64_t key = std::uint64_t(reinterpret_cast<std::uintptr_t>(shader)) * 0x9E3779B97F4A7C15ull;
    key ^= ((std::uint64_t(group) << 32) | binding) * 0xC2B2AE3D27D4EB4Full;
    auto& slot = cache[(key >> 40) & 2047];
    if (slot.used && slot.shader == shader && slot.group == group && slot.binding == binding) {
        if (slot.layout) { if (std::strcmp(shader, slot.layout->shader) == 0) return slot.layout; }
        else {
            std::uint64_t h = 0xcbf29ce484222325ull; for (const char* c = shader; *c; ++c) h = (h ^ std::uint8_t(*c)) * 0x100000001b3ull;
            if (h == slot.nameHash) return nullptr;
        }
    }
    const auto* layout = ScanUniformLayouts(shader, group, binding);
    std::uint64_t h = 0;
    if (!layout) { h = 0xcbf29ce484222325ull; for (const char* c = shader; *c; ++c) h = (h ^ std::uint8_t(*c)) * 0x100000001b3ull; }
    slot = {shader, group, binding, layout, h, true};
    return layout;
}

bool RepackUniform(const UniformLayout& layout, const void* nativeData,
                   std::size_t nativeBytes, void* webData, std::size_t webBytes) {
    if (!nativeData || !webData || !layout.fields ||
        nativeBytes < layout.nativeMinimumBytes || webBytes < layout.webBytes)
        return false;
    const auto source = reinterpret_cast<std::uintptr_t>(nativeData);
    const auto destination = reinterpret_cast<std::uintptr_t>(webData);
    if ((source <= destination && destination - source < layout.nativeMinimumBytes) ||
        (destination < source && source - destination < layout.webBytes)) return false;
    // Validate the entire map before modifying output, including externally
    // supplied layouts. Subtraction avoids offset-plus-size overflow.
    for (std::size_t index = 0; index < layout.fieldCount; ++index) {
        const auto& field = layout.fields[index];
        if (!field.bytes || field.nativeOffset > layout.nativeMinimumBytes ||
            field.bytes > layout.nativeMinimumBytes - field.nativeOffset ||
            field.webOffset > layout.webBytes ||
            field.bytes > layout.webBytes - field.webOffset) return false;
    }
    std::memset(webData, 0, layout.webBytes);
    for (std::size_t index = 0; index < layout.fieldCount; ++index) {
        const auto& field = layout.fields[index];
        std::memcpy(static_cast<unsigned char*>(webData) + field.webOffset,
                    static_cast<const unsigned char*>(nativeData) + field.nativeOffset,
                    field.bytes);
    }
    return true;
}
}
