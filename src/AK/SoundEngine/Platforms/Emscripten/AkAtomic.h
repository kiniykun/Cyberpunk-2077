#pragma once
#if !defined(__EMSCRIPTEN__) || !defined(__EMSCRIPTEN_PTHREADS__)
#error Wwise atomic adapter requires an Emscripten pthread build
#endif
// The SDK implementation uses Clang/GCC atomic builtins only; no Linux syscalls.
// Emscripten lowers these to real WebAssembly atomics on shared memory.
#include <stdint.h>
#include <stddef.h>
#include <AK/SoundEngine/Platforms/Linux/AkAtomic.h>
