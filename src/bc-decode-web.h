// bc-decode-web.h - CPU BC1-BC7 block decoder for WebGPU devices without texture-compression-bc.
// Stateless and thread-safe (no mutable globals); intended for worker threads.
#pragma once
#include <cstddef>
#include <cstdint>

namespace cp2077 { namespace bc {

enum class Kind { BC1, BC2, BC3, BC4U, BC4S, BC5U, BC5S, BC6HU, BC6HS, BC7 };

// Output formats: BC1/2/3/7 -> RGBA8 (raw, no sRGB conversion), BC4U -> R8, BC4S -> R8 snorm,
// BC5U -> RG8, BC5S -> RG8 snorm, BC6HU/BC6HS -> RGBA16F (half bits, alpha = 0x3C00).
unsigned DecodedBytesPerTexel(Kind k);
unsigned BlockBytes(Kind k); // 8 for BC1/BC4, else 16

// Decode one 4x4 block into out: row-major, tightly packed (16 * DecodedBytesPerTexel bytes).
void DecodeBlock(Kind k, const uint8_t* block, uint8_t* out);

// Decode blocksWide x blocksHigh blocks (src row pitch srcRowBytes) and write only the first
// width x height texels into dst (row pitch dstRowBytes). Partial last block column/row is clipped.
void DecodeRegion(Kind k, const uint8_t* src, size_t srcRowBytes, unsigned blocksWide, unsigned blocksHigh,
                  uint8_t* dst, size_t dstRowBytes, unsigned width, unsigned height);

}} // namespace cp2077::bc
