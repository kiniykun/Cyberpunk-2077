#pragma once
// Use the unchanged SDK's four-lane SSE implementation. Emscripten translates
// these intrinsics to real wasm SIMD under the component's msimd128/msse4.1 flags.
#if !defined(__EMSCRIPTEN__) || !defined(__wasm_simd128__)
#error This audio SIMD adapter requires the browser SIMD target
#endif
#include <AK/SoundEngine/Platforms/SSE/AkSimd.h>
// Original POSIX lane-access definitions used with its native SSE backend.
#define AKSIMD_GETELEMENT_V4F32(vector, index) ((float*)&(vector))[(index)]
#define AKSIMD_GETELEMENT_V2F32(vector, index) ((float*)&(vector))[(index)]
#define AKSIMD_GETELEMENT_V4I32(vector, index) ((int*)&(vector))[(index)]
