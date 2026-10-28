#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>
#if defined(__wasm_simd128__)
#include <wasm_simd128.h>
#endif
namespace cp2077::web {
struct MappedGpuDirtyRange { std::size_t offset, size; };
inline bool MappedGpuBytesEqual(const unsigned char* a,const unsigned char* b,std::size_t bytes){
#if defined(__wasm_simd128__)
    // Equality needs no byte-order result. Compare 64 bytes per iteration
    // without reading beyond either allocation, even for unaligned mappings.
    while(bytes>=64){
        const auto x0=wasm_v128_xor(wasm_v128_load(a),wasm_v128_load(b));
        const auto x1=wasm_v128_xor(wasm_v128_load(a+16),wasm_v128_load(b+16));
        const auto x2=wasm_v128_xor(wasm_v128_load(a+32),wasm_v128_load(b+32));
        const auto x3=wasm_v128_xor(wasm_v128_load(a+48),wasm_v128_load(b+48));
        if(wasm_v128_any_true(wasm_v128_or(wasm_v128_or(x0,x1),wasm_v128_or(x2,x3))))return false;
        a+=64;b+=64;bytes-=64;
    }
    while(bytes>=16){if(wasm_v128_any_true(wasm_v128_xor(wasm_v128_load(a),wasm_v128_load(b))))return false;a+=16;b+=16;bytes-=16;}
#endif
    return !bytes || std::memcmp(a,b,bytes)==0;
}
// Compare exact bytes, including padding. Coalesce adjacent changed pages.
inline std::vector<MappedGpuDirtyRange> FindMappedGpuDirtyRanges(const unsigned char* current,const unsigned char* committed,std::size_t bytes){
    std::vector<MappedGpuDirtyRange> ranges;
    constexpr std::size_t pageBytes=4096;
    for(std::size_t offset=0;offset<bytes;offset+=pageBytes){
        const auto count=std::min(pageBytes,bytes-offset);
        if(MappedGpuBytesEqual(current+offset,committed+offset,count))continue;
        if(!ranges.empty() && ranges.back().offset+ranges.back().size==offset)ranges.back().size+=count;
        else ranges.push_back({offset,count});
    }
    return ranges;
}
}
