#ifdef RED_PLATFORM_WEB
#include <cstdio>
#endif
/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "gameApp.h"
#ifdef __EMSCRIPTEN__
# include <emscripten/emscripten.h>
# include "engine-wgtrace-web.h"
# include <emscripten/heap.h>
# include <cstring>
# include <algorithm>
# include <cmath>
# include <cstdio>
extern "C" void CpWebReportAllocatorMemory();
#endif
#include "gameAppState.h"
#include "baseEngine.h"
#include "../../app/include/appPath.h"
#include "../../app/include/appCensorship.h"
#include "../../redConfig/include/inGameConfigGroup.h"
#include "../../redConfig/include/inGameConfigSystem.h"
#include "../../redConfig/include/inGameConfigVar.h"
#include "../../redCore/include/languageRegion.h"
#include "../../resource/include/censorshipSwapList.h"
#include "../../game/include/gameVersion.h"
#include "../../telemetry/include/telemetry.h"

namespace dd
{
	static red::CrashData< red::String > gamePatch{ "Game", "Patch" };
}

namespace Config
{
	TConfigVar< String > cvBuildSKU( "General", "BuildSKU", "", eConsoleVarFlag_ReadOnly );
}

namespace
{
#if defined( RED_PLATFORM_WINPC )
	const char c_platformSpecificConfigurationPath[] = "platform\\pc";
#elif defined( RED_PLATFORM_ORBIS )
	const char c_platformSpecificConfigurationPath[] = "platform\\orbis";
#elif defined( RED_PLATFORM_DURANGO )
	const char c_platformSpecificConfigurationPath[] = "platform\\durango";
#else
	// TODO: Add linux and other entries here
	const char c_platformSpecificConfigurationPath[] = "platform\\other";
#endif
}

namespace red
{
	GameAppInstance::GameAppInstance( const char* processName )
		: m_states( red::PoolEngine() )
		, m_currentState( nullptr )
		, m_nextState( nullptr )
		, m_currentContext( StateContext::StateContext_EnterState )
		, m_processName( processName )
		, m_returnValue( 0 )
	{
		dd::gamePatch.Set( GAME_VERSION );
	}

	GameAppInstance::~GameAppInstance()
	{
		m_states.Clear();
	}

	bool GameAppInstance::RegisterState( IGameAppState* state )
	{
		return m_states.Insert( state->GetId(), state ).IsSuccessful();
	}

	void GameAppInstance::SetReturnValue( Int32 value )
	{
		m_returnValue = value;
	}

	const char* GameAppInstance::GetName() const
	{
		return m_processName;
	}

	void GameAppInstance::RequestNextStateTransition( Int32 stateId )
	{
		IGameAppState* nextState;
		m_states.Find( stateId, nextState );

		if ( !nextState )
		{
			RED_LOG_ERROR( "[GameApplication] Could not find requested game state '%d'", stateId );
			return;
		}

		if ( nextState == m_nextState )
		{
			RED_LOG_WARNING( "[GameApplication] Game state '%s' was already request", nextState->GetName() );
			return;
		}

		RED_ASSERT( !m_nextState, "[GameApplication] Cannot set next game state to '%s', different game state ('%s') is already requested", nextState->GetName(), m_nextState->GetName() );
		if ( !m_nextState )
		{
			RED_LOG_INFO( "[GameApplication] Scheduled transition to the next game state ('%s')", nextState->GetName() );
			m_nextState = nextState;
		}

		m_currentContext = StateContext::StateContext_ExitState;
	}

	Int32 GameAppInstance::Run()
	{
#ifdef __EMSCRIPTEN__
		// Keep the original state machine, scheduling one iteration per browser frame.
		// Browser PumpMessages and rendering backends still need implementation.
		emscripten_set_main_loop_arg( []( void* context )
		{
			auto& game = *static_cast<GameAppInstance*>( context );
            // [gamefeel] Optional outer-loop pacing (?loopFps=N, off by default). Measured: pacing the loop alone does NOT
            // even out presents (a present lands on the first animation frame after the render finishes), so the default
            // pacing is done on the present itself in gpu-canvas-web.inl (?fps=N, default 30, Running state only).
            static double cpPaceMs=-1, cpPaceLast=0;
            static unsigned long long cpPaceRan=0, cpPaceSkipped=0;
            if(cpPaceMs<0){
                const double fps=MAIN_THREAD_EM_ASM_DOUBLE({
                    try{const v=new URLSearchParams(location.search).get('loopFps');if(!v)return 0;const n=Number(v);return Number.isFinite(n)&&n>0?n:0;}catch(e){return 0;}
                });
                cpPaceMs=fps>0 ? 1000.0/fps : 0;
                std::fprintf(stderr,"CP2077 frame pacing probe: configured loop targetFps=%.1f targetMs=%.3f (opt-in ?loopFps=N; presents are paced in gpu-canvas-web.inl via ?fps)\n",fps,cpPaceMs);
            }
            {
                // Tell the present path (gpu-canvas-web.inl, browser main thread) whether the engine is in Running.
                static int cpRunningReported=-1;
                const int running=game.m_currentState && std::strcmp(game.m_currentState->GetName(),"Running")==0 ? 1 : 0;
                if(running!=cpRunningReported){ cpRunningReported=running; MAIN_THREAD_ASYNC_EM_ASM({ Module["cpPresentPacingRunning"]=$0; },running); }
            }
            if(cpPaceMs>0 && game.m_currentState && std::strcmp(game.m_currentState->GetName(),"Running")==0){
                // Vsync-aware: the display period is measured from back-to-back rAF callbacks (a skipped callback returns
                // at once, so the next one arrives exactly one refresh later). The target is rounded to a whole number of
                // refreshes (60 Hz: 2 x 16.7 = 33.3 ms; 144 Hz: 5 x 6.94 = 34.7 ms) so every frame spans the same count.
                static double cpVsyncMs=1000.0/60.0, cpPrevCallback=0;
                static bool cpPrevSkipped=false;
                const double now=emscripten_get_now();
                if(cpPrevSkipped && cpPrevCallback>0){
                    // Median of the last 31 refresh intervals, snapped to a common display rate (worker rAF timing
                    // jitters by ~1 ms, which made a running average drift between 4 and 5 refreshes per frame).
                    static double cpIntervals[31]; static unsigned cpIntervalCount=0;
                    const double d=now-cpPrevCallback;
                    if(d>3.0 && d<45.0){
                        cpIntervals[cpIntervalCount++%31]=d;
                        if(cpIntervalCount>=15){
                            const unsigned n=cpIntervalCount<31?cpIntervalCount:31; double s[31];
                            for(unsigned i=0;i<n;++i)s[i]=cpIntervals[i];
                            std::sort(s,s+n);
                            const double median=s[n/2];
                            static const double rates[]={240,165,144,120,100,90,75,60,50,30};
                            double best=median,err=1e9;
                            for(double hz:rates){const double ms=1000.0/hz,e=std::fabs(ms-median)/ms;if(e<err){err=e;best=ms;}}
                            cpVsyncMs=err<0.08?best:median;
                        }
                    }
                }
                cpPrevCallback=now;
                double refreshes=cpPaceMs/cpVsyncMs; refreshes=refreshes<1.0 ? 1.0 : (double)(long long)(refreshes+0.5);
                const double period=refreshes*cpVsyncMs;
                if(cpPaceLast>0 && now-cpPaceLast<period-cpVsyncMs*0.5){ ++cpPaceSkipped; cpPrevSkipped=true; return; }
                cpPrevSkipped=false;
                cpPaceLast=now;
                if((++cpPaceRan%600)==0) std::fprintf(stderr,"CP2077 frame pacing probe: ran=%llu skipped=%llu targetMs=%.3f vsyncMs=%.3f periodMs=%.3f\n",cpPaceRan,cpPaceSkipped,cpPaceMs,cpVsyncMs,period);
            }
            cpEngineTrace::Frame cpTraceFrame;
            static unsigned long long sample=0;
            static double previousStart=0;
            const double start=emscripten_get_now();
            const double interval=previousStart ? start-previousStart : 0;
            previousStart=start;
            const bool trace=++sample<=4 || (sample&(sample-1))==0 || sample%120==0;
            const double beforePump=emscripten_get_now();
			if ( !game.PumpMessages() )
			{
				RED_LOG_ERROR( "[GameApplication] PumpMessages failed. Requesting for game exit" );
				GEngine->RequestExit( -66 );
			}
            const double afterPump=emscripten_get_now();
			game.TickStates();
            const double afterTick=emscripten_get_now();
            if(trace) std::fprintf(stderr,
                "CP2077 outer frame probe: count=%llu intervalMs=%.3f pumpMs=%.3f tickMs=%.3f heapCapacityBytes=%llu state=%s\n",
                sample,interval,afterPump-beforePump,afterTick-afterPump,
                static_cast<unsigned long long>(emscripten_get_heap_size()),
                game.m_currentState ? game.m_currentState->GetName() : "none");
            if(sample==1 || sample==32 || sample==128 || sample==512)
                CpWebReportAllocatorMemory();
			if ( !game.m_currentState )
			{
				emscripten_cancel_main_loop();
			}
		}, this, 0, false );
		return 0; // Loop scheduled; this is not the final game return value.
#else
		do
		{
			if ( !PumpMessages() )
			{
				RED_LOG_ERROR( "[GameApplication] PumpMessages failed. Requesting for game exit" );
				GEngine->RequestExit( -66 );
			}

			TickStates();
		} while ( m_currentState );

		return m_returnValue;
#endif
	}

	bool GameAppInstance::Initialize_InGameConfig( const app::InitializationSettings& settings )
	{
		const red::AbsolutePath baseSettingsPath = app::paths::MakeConfigPath( GetGameRootPath() ).AddDirPath( "settings" );
		const red::AbsolutePath platformSettingsPath = baseSettingsPath.AddDirPath( c_platformSpecificConfigurationPath );

		auto &system = InGameConfig::System::GetInstance();
		system.SetMode( InGameConfig::Mode::Game );
		system.LoadTemplates( baseSettingsPath, platformSettingsPath );

#ifdef RED_PLATFORM_WINPC
		// On PC we can load settings here, on consoles after 'press start button' stage,
		// when we are sure that user profile exists
		system.LoadUserSettings();
		system.Update();
#endif

		return true;
	}

	void GameAppInstance::Shutdown_InGameConfig()
	{
#ifdef RED_PLATFORM_WINPC
		// On PC we can save settings here
		auto &system = InGameConfig::System::GetInstance();
		system.SaveUserSettings();
#endif
	}

	void GameAppInstance::Initialize_Censorship( const app::InitializationSettings& settings )
	{
		// If we've explicitly been asked to censor things then apply censorship regardless
		if ( settings.m_censorshipRegionOverride != CName::NONE() )
		{
			app::ApplyCensorshipFromRegion( settings.m_censorshipRegionOverride, app::CensorRegionSource::FromCommandLine );
			res::UpdateCensorshipSwapListToResourceLoader();
			return;
		}

#if defined( RED_PLATFORM_ORBIS ) || defined( RED_PLATFORM_DURANGO )
		if ( Config::cvBuildSKU.Get() == "ENJA" )
		{
			app::ApplyCensorshipFromRegion( RED_NAME_CONSTEXPR( "jp" ), app::CensorRegionSource::FromSKU );
			res::UpdateCensorshipSwapListToResourceLoader();
			return;
		}
#endif

#if defined( RED_PLATFORM_ORBIS )
		// On Orbis we have to guess at the censorship region based on the language that we are running in
		// and then when the user logs into PSN we can figure out what the real censorship region is and apply that
		auto languageCountry = RED_NAME( red::GetDefaultCountryCode() );
		app::ApplyCensorshipFromRegion( languageCountry, app::CensorRegionSource::FromLanguage );

#elif defined( RED_PLATFORM_DURANGO )
		// On Durango we do a similar thing to Orbis where we guess the censorship region first
		// and then when the user logs in we get the store region and apply that
		auto languageCountry = RED_NAME( red::GetDefaultCountryCode() );
		app::ApplyCensorshipFromRegion( languageCountry, app::CensorRegionSource::FromLanguage );

#else
		// On PC only apply censorship if we explicitly ask for it
		// Otherwise the PC is completely uncensored, see CYB-570007
		app::ApplyCensorshipFromRegion( CName::NONE(), app::CensorRegionSource::Default );
#endif

		res::UpdateCensorshipSwapListToResourceLoader();
	}

	void GameAppInstance::Shutdown_Censorship()
	{
	}

	void GameAppInstance::Initialize_Telemetry( const app::InitializationSettings& settings )
	{
		telemetry::AppType appType = telemetry::AppType::Game;
		if ( settings.m_engineClass == "BackendGameEngine" )
		{
			appType = telemetry::AppType::Tools;
		}

		telemetry::Startup( appType );
	}

	void GameAppInstance::Shutdown_Telemetry()
	{
		telemetry::Shutdown();
	}

	void GameAppInstance::TickStates()
	{
		if ( m_currentState )
		{
			switch ( m_currentContext )
			{
			case StateContext::StateContext_EnterState:
#ifdef RED_PLATFORM_WEB
                std::fprintf(stderr, "CP2077 native state enter: %s\n", m_currentState->GetName());
#endif
				if ( m_currentState->OnEnterState( *this ) )
				{
					m_currentContext = StateContext::StateContext_Tick;
				}
				else
				{
					m_currentContext = StateContext::StateContext_ExitState;
				}
				break;

			case StateContext::StateContext_Tick:
				if ( m_currentState->OnTick( *this ) )
				{
					m_currentContext = StateContext::StateContext_ExitState;
				}
				break;

			case StateContext::StateContext_ExitState:
#ifdef RED_PLATFORM_WEB
                std::fprintf(stderr, "CP2077 native state exit: %s\n", m_currentState->GetName());
#endif
				m_currentState->OnExitState( *this );
				m_currentContext = StateContext::StateContext_NextState;
				break;

			case StateContext::StateContext_NextState:
				m_currentState = m_nextState;
				m_currentContext = StateContext::StateContext_EnterState;
				m_nextState = nullptr;
				break;
			}
		}
		else if ( m_nextState )
		{
			m_currentState = m_nextState;
			m_currentContext = StateContext::StateContext_EnterState;
			m_nextState = nullptr;
		}
	}
}