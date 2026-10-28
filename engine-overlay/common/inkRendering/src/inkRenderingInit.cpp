/**
* Copyright (C)2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "inkRenderingMemoryPools.h"

RED_MODULE(inkRendering)
{
	inkRend::InitializeMemoryPools();
}
