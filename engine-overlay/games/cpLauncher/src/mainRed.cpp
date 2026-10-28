/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "../../../common/redCore/include/commandline.h"
#include "../../common/engine/include/applicationUtils.h"
#include "../../common/engine/include/gameApp.h"
#include "../../common/engine/include/gameAppBaseInitializationState.h"
#include "../../common/engine/include/gameAppInitializationState.h"
#include "../../common/engine/include/gameAppRunningState.h"
#include "../../common/engine/include/gameAppShutdownState.h"

//------------------------------------------------------------------------------

extern void InitModuleDependencies();

#ifdef __EMSCRIPTEN__
# define CP_BROWSER_LIFETIME static
#else
# define CP_BROWSER_LIFETIME
#endif

#if defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )
//#define DEBUG_UNHANDLED_EXCEPTION_FILTER
#endif

namespace
{
	class FinalGameAppBaseInitializationState final : public red::GameAppBaseInitializationState
	{
	protected:
		virtual void CreateEngineInitParams( const red::CommandLine& commandLine, EngineInitParams& engineInitParams ) const override
		{
			red::GameAppBaseInitializationState::CreateEngineInitParams( commandLine, engineInitParams );

# if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER ) || defined( RED_PLATFORM_WINPC ) // TODO: disable on PC before shipping
			engineInitParams.m_showVersionWatermark = true;
# endif
		}

		virtual CName GetEngineTypeName( const red::CommandLine& commandLine ) const override
		{
#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
			if( commandLine.HasOption( "functionalTestsMode" ) )
			{
				return RED_NAME_CONSTEXPR( "FunctionalTestsGameEngine" );
			}

			if( commandLine.HasOption( "engine" ) )
			{
				String value;
				if( commandLine.GetFirstOption( "engine", value ) )
				{
					return RED_NAME( value );
				}
			}
#endif
			return RED_NAME_CONSTEXPR( "CGameEngine" );
		}
	};

	int mainRedGuarded()
	{
#ifdef RED_PLATFORM_WINPC
		SetProcessDPIAware();
#endif

		red::InitializeLowLevelSystems();

		InitModuleDependencies();

		CP_BROWSER_LIFETIME red::GameAppInstance game{ "cpLauncher" };

		CP_BROWSER_LIFETIME FinalGameAppBaseInitializationState gameBaseInitState;
		game.RegisterState( &gameBaseInitState );

		CP_BROWSER_LIFETIME red::GameAppInitializationState gameInitState;
		game.RegisterState( &gameInitState );

		CP_BROWSER_LIFETIME red::GameAppRunningState gameRunningState;
		game.RegisterState( &gameRunningState );

		CP_BROWSER_LIFETIME red::GameAppShutdownState shutdownState;
		game.RegisterState( &shutdownState );

		game.RequestNextStateTransition( red::GameAppState_BaseInitialization );

		return game.Run();
	}
}

//------------------------------------------------------------------------------

#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
extern LONG WINAPI ReportUnhandledExceptionFilter( _EXCEPTION_POINTERS *exceptionInfo );
#endif

int mainRed()
{
	int ret = 1;
#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
	__try
#endif
	{
		ret = mainRedGuarded();
	}
#ifdef DEBUG_UNHANDLED_EXCEPTION_FILTER
	__except( ReportUnhandledExceptionFilter( GetExceptionInformation() ) )
	{
		::OutputDebugStringA( "SEH exception occured!\n" );
	}
#endif

	return ret;
}
