#pragma once
// Port-specific type definitions. This is not a Wwise browser sound engine.
// Preserve the SDK's POSIX integer, pointer, pthread and file-handle types.
#if !defined(__EMSCRIPTEN__)
#error This Wwise type adapter is for Emscripten only
#endif
#define AK_EMSCRIPTEN
#if __SIZEOF_POINTER__ == 8
// The original queue-pointer wrapper must not append its 32-bit padding field
// to a real wasm64 pointer. Preserve the native eight-byte message layout.
#define AK_POINTER_64
#endif
#define AK_71AUDIO
#define AK_LFECENTER
#define AK_REARCHANNELS
#include <AK/SoundEngine/Platforms/POSIX/AkTypes.h>
#ifdef __cplusplus
static_assert(sizeof(AkUIntPtr) == sizeof(void*), "Wwise pointer ABI mismatch");
static_assert(sizeof(AkUInt64) == 8 && sizeof(AkUInt32) == 4, "Wwise integer ABI mismatch");
#else
_Static_assert(sizeof(AkUIntPtr) == sizeof(void*), "Wwise pointer ABI mismatch");
_Static_assert(sizeof(AkUInt64) == 8 && sizeof(AkUInt32) == 4, "Wwise integer ABI mismatch");
#endif
