#pragma once
#include <cstddef>
#include <cstdint>

namespace cp2077::web {
struct UniformScalarCopy {
    std::uint32_t nativeOffset;
    std::uint32_t webOffset;
    std::uint32_t bytes;
};
struct UniformLayout {
    const char* shader;
    std::uint32_t group;
    std::uint32_t binding;
    std::uint32_t nativeMinimumBytes;
    std::uint32_t webBytes;
    const UniformScalarCopy* fields;
    std::size_t fieldCount;
};
const UniformLayout* FindUniformLayout(const char* shader, std::uint32_t group,
                                      std::uint32_t binding);
// Copies scalar bit patterns without interpreting floats or matrices. Padding
// is zeroed. Undersized or overlapping input/output fails before any write.
bool RepackUniform(const UniformLayout& layout, const void* nativeData,
                   std::size_t nativeBytes, void* webData, std::size_t webBytes);
}
