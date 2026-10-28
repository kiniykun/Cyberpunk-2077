#pragma once
#include <assert.h>

// Browser failure diagnostics avoid the Windows-only wide CRT.
#include <cstdio>
#include <cstdlib>
#undef RED_FATAL_ASSERT
#undef RED_ASSERT
#define RED_FATAL_ASSERT(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "[REC CORE ASSERT] %s:%d: %s\n", __FILE__, __LINE__, #cond); std::abort(); }} while (false);
#define RED_ASSERT(cond, ...) RED_FATAL_ASSERT(cond, __VA_ARGS__)
// Offset of (should work on all platforms( ? )
#define OFFSET_OF( clazz, member ) offsetof( clazz, member )

// Function name:
#if defined( RED_COMPILER_CLANG )
# define REC_FUNCNAME 
#elif defined( RED_COMPILER_MSC )
# define REC_FUNCNAME __FUNCTION__
#else
#error Unsupported compiler!
#endif