/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

// this file is auto generated and contains registration function for rtti types in this project
#include "../rtti/functionalTestsEngine/typeRegistryFuncImpl.h"

RED_MODULE(functionalTestsEngine)
{
	RegisterFunctionalTestsEngineRTTI();

	extern void RegisterFunctionalTestsLoggers();
	RegisterFunctionalTestsLoggers();
}
