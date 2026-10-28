#pragma once

#if defined RED_PLATFORM_WINPC
	#if defined(REC_EXPORT)
		#define REC_API __declspec(dllexport)
	#else
		#define REC_API //__declspec(dllimport)
	#endif
#endif

#if defined RED_PLATFORM_ORBIS
	#if defined(REC_EXPORT)
		#define REC_API __declspec(dllexport)
	#else
		#define REC_API __declspec(dllimport) // on PS4 importing is only possible via stub lib.
	#endif
#endif

#if defined RED_PLATFORM_DURANGO
	#if defined(REC_EXPORT)
		#define REC_API __declspec(dllexport)
	#else
		#define REC_API //__declspec(dllimport)
	#endif
#endif
#ifdef RED_PLATFORM_WEB
#define REC_API
#endif
