#pragma once
#include <AK/SoundEngine/Common/AkTypes.h>
#include <time.h>
#define AkAlloca(size) __builtin_alloca(size)
namespace AKPLATFORM {
inline void PerformanceCounter(AkInt64* result) {
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) __builtin_trap();
    *result = static_cast<AkInt64>(now.tv_sec) * 1000000000 + now.tv_nsec;
}
inline void PerformanceFrequency(AkInt64* result) { *result = 1000000000; }
}
// Original SDK POSIX helpers already include Emscripten-specific thread handling.
#include <AK/Tools/POSIX/AkPlatformFuncs.h>
