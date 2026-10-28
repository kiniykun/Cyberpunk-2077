#include "native-initialization-job-trace.h"
#include "build.h"
#include "baseGameEngine.h"
#include "gogRewardsSystem.h"

#include "../../renderData/include/debugFilter.h"
#include "../../redConfig/include/configVarSystem.h"
#include "../../worldStreaming/include/worldEnumeration.h"
#include "../../input/include/inputSystem.h"
#include "../../engine/include/engineWatchdog.h"
#include "../../imgui/include/redImGui.h"
#include "../../redReflection/include/resourceMetricsBank.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../engine/include/baseEngineInitParams.h"
#include "../../redCore/include/commandline.h"

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

RTTI_BEGIN_TYPE( BaseGameEngine );
	RTTI_PARENT_TYPE( CBaseEngine );
RTTI_END_TYPE();

static rend::DebugFilter DF_GogRewards( "GogRewards" );

namespace game
{
	namespace data
	{
		extern void InitializeMemoryPools();
	}

	namespace bb
	{
		extern void InitDefinitions();
	}

	extern void InitializePSClassBindings();
}

BaseGameEngine::BaseGameEngine()
	: m_watchdog( red::CreateUniquePtr< CEngineWatchdog > () )
#ifdef USE_RESOURCE_METRICS_BANK
#ifdef RED_CONFIGURATION_FINAL
	, m_enableResourceMetrics( true ) //TODO Change to false by default and require a cmd arg to enable (only for profile builds)
#else
	, m_enableResourceMetrics( true )
#endif
#endif
{
	SetGameMode();
}

BaseGameEngine::~BaseGameEngine()
{}

Bool BaseGameEngine::OnRootSystemInitialization( const EngineInitParams& params )
{
	if ( params.m_watchdogTimeoutSecondsOverride > 0 )
	{
		m_watchdog->SetTimeoutSecondsOverride( params.m_watchdogTimeoutSecondsOverride );
	}

	rend::SetGameRenderPreset( params.m_renderPreset );

	if (!TBaseClass::OnRootSystemInitialization( params ) )
	{
		return false;
	}

	return true;
}

void BaseGameEngine::OnRegularSystemInitialization( const EngineInitParams& params, const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
{
	job::Builder builder{ context };
	builder.DispatchJob( "BaseGameEngine/Initialization/BaseInitialization", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/BaseInitialization"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		TBaseClass::OnRegularSystemInitialization( params, context, keepRunning );
	} );

	builder.DispatchJob( "BaseGameEngine/Initialization/VirtualRUIDs", [this, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/VirtualRUIDs"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// Virtual RUIDs are editor only
		CRUID::DisableVirtualRUIDs();
	} );

#ifndef RED_CONFIGURATION_FINAL
	if ( params.m_tweakDBForceRaw )
	{
		builder.DispatchJob( "BaseGameEngine/Initialization/LoadTweakDB", [this, &keepRunning, params]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/LoadTweakDB"};
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			game::data::InitTweakDB();

			for( const String& dlc : params.m_tweakDBdlcsTEMP )
			{
				game::data::GetTweakDB().EnableDLC( dlc );
			}

			game::data::GetTweakDB().SetShowErrorDialog( !params.m_tweakDBSilentValidation );
			game::data::GetTweakDB().LoadRawFilesAsync( context, keepRunning );
		} );
	}
	else
#endif
	{
		builder.DispatchJob( "BaseGameEngine/Initialization/LoadTweakDB", [this, &keepRunning, params]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/LoadTweakDB"};
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			game::data::InitTweakDB();
			game::data::GetTweakDB().SetShowErrorDialog( !params.m_tweakDBSilentValidation );
			game::data::GetTweakDB().LoadOptimized( params.m_tweakdbBlobPath );
		} );
	}

	builder.DispatchJob( "BaseGameEngine/Initialization/Blackboard", [this, &keepRunning, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/Blackboard"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// initialize blackboard layouts
		game::bb::InitDefinitions();
	} );
	
	builder.DispatchJob( "BaseGameEngine/Initialization/EnumerateWorlds", [this, &keepRunning, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/EnumerateWorlds"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		job::Builder builder{ context };
		// make sure all worlds are enumerated for network and other users
		world::WorldEnumerator::GetInstance().Init( builder );
	} );

	builder.DispatchJob( "BaseGameEngine/Initialization/PSClassBindings", [this, &keepRunning, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/PSClassBindings"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		game::InitializePSClassBindings();
	} );

#ifdef USE_RESOURCE_METRICS_BANK
	builder.DispatchJob( "BaseGameEngine/Initialization/Snapshot", [this, &keepRunning, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseGameEngine/Initialization/Snapshot"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// Snapshot of what was just loaded
		if (GResourceLoader->GetResourceMetricsBank())
		{
			GResourceLoader->GetResourceMetricsBank()->NextLoadingRequestSnapshot( "BaseEngine" );
		}
	} );
#endif
}

Bool BaseGameEngine::InitializeInput( const CName inputInterfaceClassName )
{
	const auto* classType = GetRttiSystem().FindClass( inputInterfaceClassName );
	RED_FATAL_ASSERT( classType, "Error: Invalid input system class: [%hs]", inputInterfaceClassName.AsChar() );

	m_inputSystem = red::UniquePtr<input::IInputSystem>( classType->CreateObject< input::IInputSystem >() );
	m_inputSystem->Initialize();
	return m_inputSystem != nullptr;
}

void BaseGameEngine::Shutdown()
{
	game::data::DeinitTweakDB();
	m_watchdog.Reset();
	TBaseClass::Shutdown();
}

Bool BaseGameEngine::ProcessFrame( const Float scaledEngineTimeDelta, const tick::Info& tickInfo, input::IInputContext& input )
{
	PC_SCOPE( BaseGameEngine_ProcessFrame );


	TBaseClass::ProcessFrame( scaledEngineTimeDelta, tickInfo, input );

#ifdef USE_RESOURCE_METRICS_BANK
	if( !IsHeadless( ) )
	{
		red::ScopedLock<red::SpinLock> guard( m_metricsDataLock );//synchronize access for both stages
		PC_SCOPE( BaseGameEngine_UpdateGpuResourceMetrics );
		UpdateGpuResourceMetrics( );
	}
#endif

	{
		PC_SCOPE( BaseGameEgine_DebugFilters );

		static auto lastSavedGameDebugFiltersMask = rend::DebugFilterMask::GetFilterMask( rend::GetGameFilterName() );
		const auto& gameDebugFiltersMask = rend::DebugFilterMask::GetFilterMask( rend::GetGameFilterName() );
		if ( lastSavedGameDebugFiltersMask != gameDebugFiltersMask )
		{
			String filterHierarchy = String_CreateExternal_OnStack( RED_KILO_BYTE( 16 ) );
			rend::DebugFilterMask::FilterHierarchyAsString( rend::GetGameFilterName(), filterHierarchy );
			Config::GetConfigSystem().SetValue( "DebugFilters", rend::GetGameFilterName(), filterHierarchy );
			Config::GetConfigSystem().Save();
			lastSavedGameDebugFiltersMask = gameDebugFiltersMask;
		}
	}

	// keep application running
	m_watchdog->Kick();

	return !RequestedExit();
}

void BaseGameEngine::ForceKillWithWatchdog(CName reason)
{
	m_watchdog->ForceKill(reason);
}

#ifdef USE_RESOURCE_METRICS_BANK
void BaseGameEngine::UpdateGpuResourceMetrics()
{
#ifdef RED_USE_IMGUI
	if ( m_enableResourceMetrics )
	{
		static const CName userDataName = RED_NAME_CONSTEXPR( "GPUMemory_LoadedResourceMetrics" );
		auto resourceMetricsBank = GResourceLoader->GetResourceMetricsBank();
		resourceMetricsBank->FetchMetrics( &m_loadedResourceMetrics );

		red::ImGui::SetWindowUserData( userDataName, &m_loadedResourceMetrics );
	}
#endif
}

void BaseGameEngine::GetLoadedMetricsDataCopy( res::RawResourceMetrics& copy )
{
	red::ScopedLock<red::SpinLock> guard( m_metricsDataLock );
	static const CName userDataName = RED_NAME_CONSTEXPR( "GPUMemory_LoadedResourceMetrics" );
	auto resourceMetricsBank = GResourceLoader->GetResourceMetricsBank();
	resourceMetricsBank->FetchMetrics( &m_loadedResourceMetrics );
	copy = m_loadedResourceMetrics;
}

#endif

Bool BaseGameEngine::InitializeGOGRewardsSystem()
{
	if ( red::CommandLine::Get().HasOption( "gogRewardsDisabled" ) )
	{
		return true;
	}

	m_gogRewardsSystem = CreateHandle<game::GOGRewardsSystem>();
	return m_gogRewardsSystem && m_gogRewardsSystem->Initialize();
}

void BaseGameEngine::ShutdownGOGRewardsSystem()
{
	if( m_gogRewardsSystem )
	{
		m_gogRewardsSystem->Shutdown();
		m_gogRewardsSystem.Reset();
	}
}

void BaseGameEngine::UpdateGOGRewardsSystem( const Float realTimeDelta )
{
	if( m_gogRewardsSystem )
	{
		m_gogRewardsSystem->Update( realTimeDelta );

#ifdef RED_USE_IMGUI
		rend::DebugFilterMask& mask = rend::DebugFilterMask::GetFilterMask( rend::GetGameFilterName() );
		
		if ( mask.Test( DF_GogRewards ) )
		{
			m_gogRewardsSystem->DrawDebugImGui();
		}
#endif
	}
}

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

