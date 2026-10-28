/**
* Copyright (c) 2013 CD Projekt Red. All Rights Reserved.
*/
#ifndef RED_THREADS_UTILS_H
#define RED_THREADS_UTILS_H
#pragma once

#ifdef RED_COMPILER_MSC
#include <intrin.h>
#pragma intrinsic( _ReadWriteBarrier )
#endif

//////////////////////////////////////////////////////////////////////////
// Memory barriers
#if defined ( RED_COMPILER_MSC )
#	define RED_THREADS_MEMORY_BARRIER() do{ _ReadWriteBarrier(); MemoryBarrier(); }while(false)
#elif defined( RED_PLATFORM_WEB )
# include <atomic>
# define RED_THREADS_MEMORY_BARRIER() std::atomic_thread_fence( std::memory_order_seq_cst )
#elif defined ( RED_COMPILER_CLANG )
#	include <x86intrin.h>
#	define RED_THREADS_MEMORY_BARRIER() _mm_mfence()
#else
#	error Compiler/platform not supported
#endif

#endif // RED_THREADS_UTILS_H