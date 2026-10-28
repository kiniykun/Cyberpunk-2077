/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

// this file is auto generated and contains registration function for rtti types in this project
#include "../rtti/galaxy/typeRegistryFuncImpl.h"

RED_MODULE(galaxy)
{
#if defined(RED_PLATFORM_WINPC) || defined(RED_PLATFORM_WEB)
	RegisterGalaxyRTTI();
#endif
}