/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#pragma once


//////////////////////////////////////////////////////////////////////////

// #define INK_DISABLE_ICU				//	-> disable ICU external library from compilation

//////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////
//                          GLOBAL ICU SETTINGS                         //
//////////////////////////////////////////////////////////////////////////


//////////////////////////////////////////////////////////////////////////
//                      SETTINGS PER CONFIGURATION                      //
//////////////////////////////////////////////////////////////////////////
#if defined( RED_CONFIGURATION_DEBUG )

// EMPTY

#elif defined( RED_CONFIGURATION_NOPTS )

// EMPTY

#elif defined( RED_CONFIGURATION_RELEASE )

// EMPTY

#elif defined( RED_CONFIGURATION_FINAL )

// EMPTY

#endif


//////////////////////////////////////////////////////////////////////////
//                         SETTINGS PER PLATFORM                        //
//////////////////////////////////////////////////////////////////////////
#if defined( RED_PLATFORM_WEB )
	#define RED_USE_FREE_TYPE
	#define RED_USE_HARFBUZZ

#elif defined( RED_PLATFORM_WIN32 ) || defined ( RED_PLATFORM_WIN64 )
	#define RED_USE_FREE_TYPE
	#define RED_USE_HARFBUZZ

#elif defined( RED_PLATFORM_DURANGO )

#ifndef U_PLATFORM
	#define U_PLATFORM U_PF_DURANGO
#endif

	#define RED_USE_FREE_TYPE
	#define RED_USE_HARFBUZZ

#elif defined( RED_PLATFORM_ORBIS )

#ifndef U_PLATFORM
	#define U_PLATFORM U_PF_ORBIS
#endif
	
	#define RED_USE_FREE_TYPE
	#define RED_USE_HARFBUZZ

#elif defined( RED_PLATFORM_LINUX )

	#define INK_DISABLE_ICU

#endif
