/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <cstdio>
#include "baseGameSession.h"
#include "gameSessionDesc.h"
#include "characterCustomizationSystemInterface.h"

#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/gameAutoSaveSystemInterface.h"
#include "../../gameCore/include/questsSystemInterface.h"
#include "../../gameCore/include/playerManagerInterface.h"
#include "../../gameCore/include/populationSystemInterface.h"
#include "../../gameCore/include/communitySystemInterface.h"
#include "../../gameCore/include/player.h"

#include "../../inkCore/include/inkSystem.h"

#include "../../world/include/runtimeSystemRendering.h"
#include "../../world/include/runtimeSystemNodeStreaming.h"
#include "../../world/include/runtimeSystemFoliage.h"
#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h" // ABUSE
#include "../../worldStreaming/include/worldStreamingTester.h" // ABUSE
#include "../../worldEntities/include/runtimeSystemEntity.h"

#include "../../engine/include/systemsUpdater.h"
#include "../../engine/include/scriptHelper.h"
#include "../../engine/include/fadeOutSupervisorInterface.h"

#include "../../worldEntities/include/runtimeSystemWeather.h"

#include "../../worldUserInterface/include/runtimeSystemUI.h"

#include "../redJobs2/include/jobBuilder.h"
#include "../../redReflection/include/resourceDepot.h" // CP2077 web [saves]
extern "C" void cpweb_SaveLoadWatchArm(); // CP2077 web [saves]: packageReadStream.cpp

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

RTTI_BEGIN_ABSTRACT_TYPE_IN_NAMESPACE( BaseGameSession, game )
RTTI_END_TYPE()

#if defined( RED_PLATFORM_DURANGO )
extern void DispatchEvents();
#endif

game::BaseGameSession::BaseGameSession()
	: m_processor( nullptr )
	, m_gameInstance( nullptr )
	, m_scene( nullptr )
	, m_gameStatus( GameStatus::NotReady )
	, m_isInitialized( false )
{
}

game::BaseGameSession::~BaseGameSession()
{
	RED_FATAL_ASSERT( !m_gameInstance );
	RED_FATAL_ASSERT( !m_scene );
}

void game::BaseGameSession::Initialize(
	const job::RunContext& context,
	red::Atomic< red::AsyncStatus >& result,
	const GameSessionDesc& sessionDesc,
	save::IStream* loadStream,
	SystemsUpdater* processor,
	IGameInstance* gameInstance,
	world::RuntimeScene* scene )
{
	// Handle possible transition to suspended mode
#if defined( RED_PLATFORM_DURANGO )
	DispatchEvents();
#endif

	m_streamingTestRequested = sessionDesc.IsStreamingTestRequested();

	// Be sure scripts are loaded
	if ( !GEngine->HasScriptsLoaded() )
	{
		RED_LOG_ERROR( "GameSession: Cannot initialize game session if scripts are not loaded" );
		result.SetValue( red::AsyncStatus::Error );
		return;
	}

	red::SharedPtr< GameSessionDesc, red::PoolEngine > gameSessionDesc = red::CreateSharedPtr< GameSessionDesc, red::PoolEngine >( sessionDesc );

	GameSessionDesc::SetCrashDumpData( *gameSessionDesc );

	// CP2077 web [saves]: restoring a save.
	//  - The save's world must exist in the mounted archives. Only the .streamingworld is loaded (WorldID::IsValid
	//    and the loading request above use it; the cooked gyms ship no .world). A save of a world that is not
	//    cooked/shipped would otherwise load a null world and crash later; failing here
	//    takes the original error path (RestoreFromSave: "Game session initialize failed" -> main menu).
	//  - Arm the package-read watch: out-of-range reads while this saved session loads fail the load
	//    (gameState_Session.cpp), instead of continuing on zero-filled data.
	if ( loadStream )
	{
		const auto& worldID = gameSessionDesc->GetSessionConfig().m_worldID;
		res::IResourceDepot* depot = GetResourceDepot();
		Bool hasWorldFile = false;
		if ( worldID.IsValid() && depot )
		{
			const Bool hasWorld = worldID.GetWorldPath().IsValid() && depot->ResourceExists( worldID.GetWorldPath() );
			hasWorldFile = hasWorld;
			const Bool hasStreamingWorld = depot->ResourceExists( worldID.GetStreamingWorldPath() );
			if ( !hasStreamingWorld )
			{
				std::fprintf( stderr, "CP2077 saves: save '%s' needs a world that is not in the archives (world=%d streamingworld=%d) -> load failed, back to main menu\n",
					gameSessionDesc->GetSaveName().AsChar(), hasWorld ? 1 : 0, hasStreamingWorld ? 1 : 0 );
				result.SetValue( red::AsyncStatus::Error );
				return;
			}
		}
		cpweb_SaveLoadWatchArm();
		std::fprintf( stderr, "CP2077 saves: restoring save '%s' (streaming world present, .world=%d), package-read watch armed\n", gameSessionDesc->GetSaveName().AsChar(), hasWorldFile ? 1 : 0 );
	}

	// start loading streaming world resource
	res::ResourceTokenHandle worldToken;
	if ( gameSessionDesc->GetSessionConfig().m_worldID.IsValid() )
	{
		auto worldPath = gameSessionDesc->GetSessionConfig().m_worldID.GetStreamingWorldPath();
		worldToken = GResourceLoader->IssueLoadingRequest( res::Convert( worldPath, world::GetCommonLoadingOptions() ) );
	}

	//setup processor
	RED_FATAL_ASSERT( processor, "Given processor does not exist" );
	RED_FATAL_ASSERT( !m_processor, "Processor is already set" );
	m_processor = processor;

	RED_LOG_INFO( "GameSession: Setup runtime scene" );

	job::Builder builder{ context };

	// Setup runtime scene
	RED_FATAL_ASSERT( scene, "Given runtime scene does not exist" );
	RED_FATAL_ASSERT( !m_scene, "Runtime scene is already set" );
	m_scene = scene;

	builder.DispatchJob("SetupRuntimeScene", [this, config = gameSessionDesc->GetSessionConfig()]( const job::RunContext& context )
	{
		SetupRuntimeScene( config, *context.continuationContext.counter );
	} );

	RED_LOG_INFO( "GameSession: Setup game instance" );

	builder.DispatchJob( "LoadGame", [this, gameInstance, gameSessionDesc, loadStream, &result]( const job::RunContext& context )
	{
		// Setup game instance
		RED_FATAL_ASSERT( gameInstance, "Given game instance does not exist" );
		RED_FATAL_ASSERT( !m_gameInstance, "Game instance is already set" );
		m_gameInstance = gameInstance;
		SetupGameInstance( gameSessionDesc->GetSessionConfig() );

		// Additional systems initialization
		if ( !InitializeSystemsBeforeWorldAttached( *gameSessionDesc ) )
		{
			RED_LOG_ERROR( "GameSession: Failed to initialize systems before world attach" );
			result.SetValue( red::AsyncStatus::Error );
			return;
		}

		// attach the game instance to world
		RED_LOG_INFO( "GameSession: Attach world" );
		m_gameInstance->AttachWorld( *m_scene );

		RED_LOG_INFO( "GameSession: Attach game" );
		m_scene->AttachGame( *m_gameInstance );

		job::Builder builder{ context };

		// restore state (optional)
		if ( loadStream )
		{
			red::SharedPtr< Bool, red::PoolEngine > isGameLoaded = red::CreateSharedPtr< Bool, red::PoolEngine >( true );
			builder.DispatchJob( "LoadGame", [this, loadStream, isGameLoaded]( const job::RunContext& context )
			{
				RED_LOG_INFO( "GameSession: Load stream" );
				LoadGame( context, *isGameLoaded, *loadStream );
			} );

			builder.DispatchJob( "OnGameLoaded", [this, isGameLoaded, &result]( const job::RunContext& context )
			{
				if ( !*isGameLoaded)
				{
					RED_LOG_ERROR( "GameSession: Failed to load the game" );
					result.SetValue( red::AsyncStatus::Error );
					return;
				}
			} );
		}

		builder.DispatchJob( "OnGameLoaded", [this, gameSessionDesc, &result]( const job::RunContext& context )
		{
			// Additional systems initialization
			InitializeSystemsAfterWorldAttached( *gameSessionDesc, context );
		} );
	} );

	builder.DispatchJob( "OnGamePrepared", [this, &result, worldToken]( const job::RunContext& context )
	{
		RED_LOG_INFO( "GameSession: OnGamePrepared" );

		m_gameInstance->OnGamePrepared();

		IPlayerManager& playerManager = m_gameInstance->GetSystem< IPlayerManager >();
		if ( Player* player = playerManager.GetLocalPlayer() )
		{
			GetInkSystem().SetInputManager( player->GetManagerView() );
		}

		OnGameSessionInitialized();
		result.SetValue( red::AsyncStatus::Done );
	} );
}

void game::BaseGameSession::OnGameSessionInitialized()
{
	m_processor->Link();
	m_isInitialized.SetValue( true );
	GEngine->GameSessionInitilized( true );

#ifdef STREAMING_TESTER_ENABLED
	if( m_streamingTestRequested )
	{
		if( auto streamingSystem = m_scene->GetSystem< world::RuntimeSystemWorldStreaming >() )
		{
			world::StreamingTester* const streamingTester = streamingSystem->GetTester();
			RED_ASSERT( streamingTester );

			streamingTester->SetCustomStreamingTestFinishedCallback( [this]()
			{
				m_streamingTestFinished = true;
			} );
		}
	}
#endif // #ifdef STREAMING_TESTER_ENABLED
}

Bool game::BaseGameSession::SetupGameInstance( const SessionConfig& config )
{
	PC_SCOPE( SetupGameInstance );

	// setup game instance
	world::RuntimeInfo runtimeInfo( config.m_runtimeMode, net::PeerID::Host, config.m_worldID );
	game::InstanceSetupInfo instanceSetup( runtimeInfo );
	instanceSetup.m_processor = m_processor;

	// create a game instance
	return m_gameInstance->Setup( instanceSetup );
}

void game::BaseGameSession::SetupRuntimeScene( const SessionConfig& config, job::Counter& counter )
{
	PC_SCOPE( SetupRuntimeScene );

	// setup scene
	world::RuntimeInfo runtimeInfo( config.m_runtimeMode, net::PeerID::Host, config.m_worldID );
	world::RuntimeSceneSetupInfo sceneSetup ( runtimeInfo );
	sceneSetup.m_processor = m_processor;
	sceneSetup.m_streamingTestRequested = m_streamingTestRequested;

	// initialize the new world
	m_scene->Setup( sceneSetup, counter );
}

Bool game::BaseGameSession::InitializeSystemsBeforeWorldAttached( const GameSessionDesc& sessionDesc )
{
	if ( !SetupGameRules( sessionDesc.GetGameRulesConfig() ) )
	{
		return false;
	}

	if ( !InitializePopulation( sessionDesc.GetPopulationConfig() ) )
	{
		return false;
	}

	return true;

}

void game::BaseGameSession::InitializeSystemsAfterWorldAttached( const GameSessionDesc& sessionDesc, const job::RunContext& context )
{
	job::Builder builder{ context };
	builder.DispatchJob( "InitializeWorldStreaming", [this, config = sessionDesc.GetSessionConfig()]( const job::RunContext& context )
	{
		// -- Runtime Systems
		InitializeWorldStreaming( config, context );
	} );

	red::SharedPtr< GameSessionDesc, red::PoolEngine > param = red::CreateSharedPtr< GameSessionDesc, red::PoolEngine >( sessionDesc );
	builder.DispatchJob( "InitializeGame", [this, param]( const job::RunContext& context )
	{
		// -- Game Systems
#ifdef __EMSCRIPTEN__
		// CP2077 web [nomad2]: after an aborted save restore (package-read watch -> menu) a following session trapped here
		// with 'null function': m_gameInstance->GetSystem<IGameRulesSystem>() through a null/stale game instance.
		std::fprintf( stderr, "CP2077 nomad2 session: InitializeGame gameInstance=%p loadingSave=%d\n", (void*)m_gameInstance, param->IsLoadingSavedSession() ? 1 : 0 );
		if ( m_gameInstance == nullptr )
		{
			std::fprintf( stderr, "CP2077 nomad2 session: InitializeGame without a game instance - skipped\n" );
			return;
		}
#endif

		// Initialize game rules system
		if ( !InitializeGameRules( param->GetGameRulesConfig(), param->GetQuestConfig() ) )
		{
			return;
		}

		// initialize after mounted world, since requires spawn markers to have been loaded
		if ( !InitializeLocalPlayer( param->GetLocalPlayerConfig() ) )
		{
			return;
		}

		// if not from save
		if ( !param->IsLoadingSavedSession() )
		{
			if ( const auto& state = param->GetCharacterCustomizationState() )
			{
				m_gameInstance->GetSystem<game::ui::ICharacterCustomizationSystem>().InitializeFromFinalizedState( state );
			}
#ifdef CHARACTER_CUSTOMIZATION_PRESETS_ENABLED
			else
			{
				m_gameInstance->GetSystem<game::ui::ICharacterCustomizationSystem>().InitializeFallbackFinalizedState( param->GetLocalPlayerConfig().m_spawnParams.m_gender );
			}
#endif

			// setup quest
			if ( !InitializeQuest( param->GetQuestConfig() ) )
			{
				return;
			}
		}
	} );
}

void game::BaseGameSession::InitializeWorldStreaming( const SessionConfig& sessionConfig, const job::RunContext& context )
{
	// ask the streaming system to mount the requested world
	// Mount world *after* init game, because now entities can have a dependency on the game
	// and "always loaded" entities will be created when mounting the world
	if ( auto streamingSystem = m_scene->GetSystem< world::RuntimeSystemWorldStreaming >() )
	{
		job::Builder builder{ context };
		builder.DispatchJob( "LoadStreamingWorld", [this, streamingSystem, worldID = sessionConfig.m_worldID]( const job::RunContext& context )
		{
			streamingSystem->LoadStreamingWorld( *m_scene, worldID, context );
		} );

		builder.DispatchJob( "InitializeEnvironmentSystem", [this, streamingSystem, worldID = sessionConfig.m_worldID]( const job::RunContext& context )
		{
			// Custom init for systems needing streaming world access but in a different project
			// NOTE: Maybe it's better to move these into the systems themselves and have a hacky include for the streaming world
			
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=get-world-begin\n");
#endif
auto streamingWorld = streamingSystem->GetStreamingWorld();
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=get-world-end\n");
#endif

			if ( streamingWorld )
			{
				auto runtimeSystemWeather = m_scene->GetSystem< world::RuntimeSystemWeather >();
				if ( runtimeSystemWeather )
				{
					
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=environment-resource-begin\n");
#endif
const auto& cpEnvironmentDefinition = streamingWorld->m_environmentDefinition.Get();
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=environment-resource-end\n");
#endif

#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=weather-set-begin\n");
#endif
runtimeSystemWeather->SetEnvironmentDefinition( cpEnvironmentDefinition );
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=weather-set-end\n");
#endif

				}

				auto runtimeSystemFoliage = m_scene->GetSystem< world::RuntimeSystemFoliage >();
				if ( runtimeSystemFoliage )
				{
					
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=foliage-begin\n");
#endif
runtimeSystemFoliage->SetFoliageMappingPath( streamingWorld->m_autoFoliageMapping.GetPath() );
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=foliage-end\n");
#endif

				}
			}

			
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=scene-callbacks-begin\n");
#endif
m_scene->OnStreamingWorldLoaded( *(context.continuationContext.counter) );
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=scene-callbacks-end\n");
#endif


			// initialization step that requires streaming world to be loaded
			
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=game-callbacks-begin\n");
#endif
m_gameInstance->OnStreamingWorldLoaded( *m_scene, context );
#ifdef __EMSCRIPTEN__
std::fprintf(stderr,"CP2077 environment startup probe: phase=game-callbacks-end\n");
#endif

		} );

		builder.DispatchJob( "MountWorld", [streamingSystem, worldID = sessionConfig.m_worldID]( const job::RunContext& context )
		{
			// start streaming stuff now
			streamingSystem->MountWorld( worldID, context );
		} );
	}
}

Bool game::BaseGameSession::InitializeLocalPlayer( const LocalPlayerConfig& localPlayerConfig ) const
{
	// create player
	if ( localPlayerConfig.m_spawnLocalPlayer )
	{
		// spawn local player
		IPlayerManager& playerManager = m_gameInstance->GetSystem< IPlayerManager >();
		playerManager.CreateLocalPlayer( localPlayerConfig.m_spawnParams );
	}
	return true;
}

Bool game::BaseGameSession::InitializeQuest( const QuestConfig& questConfig ) const
{
	if ( questConfig.m_runQuest )
	{
		quest::IQuestsSystem& questsSystem = m_gameInstance->GetSystem< quest::IQuestsSystem >();
		questsSystem.RunAsync( questConfig.m_questPath );
	}
	return true;
}

Bool game::BaseGameSession::InitializePopulation( const PopulationConfig& populationConfig ) const
{
	const game::IGameInstance& gameInstance = *m_gameInstance;
	if ( !gameInstance.GetRuntimeInfo().IsClient() ) //neither population system nor community system are present on clients
	{
#if !defined( RED_CONFIGURATION_FINAL )
		game::IPopulationSystem& populationSystem = gameInstance.GetSystem< game::IPopulationSystem >();
		populationSystem.Debug_SetAlwaysSpawnedPolicy( populationConfig.m_alwaysSpawnedDisabled ?
														PopulationAlwaysSpawnedPolicy::IgnoreAlwaysSpawned : PopulationAlwaysSpawnedPolicy::RespectAlwaysSpawned );
		populationSystem.Debug_SetSpawnInViewPolicy( populationConfig.m_forceSpawnInView ?
														PopulationSpawnInViewPolicy::AlwaysSpawnInView : PopulationSpawnInViewPolicy::RespectSpawnInView );
#endif

		game::ICommunitySystem& communitySystem = gameInstance.GetSystem< game::ICommunitySystem >();
		communitySystem.Debug_NoReducedCrowd( populationConfig.m_noReducedCrowd );
#if !defined( RED_CONFIGURATION_FINAL )
		communitySystem.Debug_NoCrowd( populationConfig.m_noCrowd );
#endif
	}

	return true;
}

Bool game::BaseGameSession::SetupGameRules( const GameRulesConfig& gameRulesConfig )
{
	IGameRulesSystem& gameRulesSystem = m_gameInstance->GetSystem< IGameRulesSystem >();
	gameRulesSystem.SetGameRulesName( gameRulesConfig.m_gameRulesName );
	return true;
}

Bool game::BaseGameSession::InitializeGameRules( const GameRulesConfig& gameRulesConfig, const QuestConfig& questConfig )
{
	IGameRulesSystem& gameRulesSystem = m_gameInstance->GetSystem< IGameRulesSystem >();
	gameRulesSystem.SetupConfig( gameRulesConfig, questConfig );
	return true;
}

void game::BaseGameSession::SetTickFilters( const TTickFilters& enabledClassTypes, const TTickFilters& exceptClassTypes /*= TTickFilters() */ )
{
	m_processor->SetTickFilters( enabledClassTypes, exceptClassTypes );
}

void game::BaseGameSession::ModTickFilters( const TTickFilters& modifiedClassTypes, Bool enabled )
{
	m_processor->ModTickFilters( modifiedClassTypes, enabled );
}

void game::BaseGameSession::ClearTickFilters()
{
	m_processor->ClearTickFilters();
}

void game::BaseGameSession::BuildTickChain( job::Builder& builder, const Float timeDelta, const tick::Info& tickInfo )
{
	//#tbd: Build now.
	// Update processor
	builder.DispatchJob( "Update/Systems", [this, timeDelta, &tickInfo] ( const job::RunContext& runContext ) {
		job::Builder depBuilder{ runContext };
		// update the actual game
		m_processor->BuildJobChain( depBuilder, timeDelta, tickInfo );
	} );
}

void game::BaseGameSession::SetPaused( const Bool paused )
{
	if( m_gameInstance )
	{
		m_gameInstance->SetGameSessionPaused( paused );
	}
}

void game::BaseGameSession::Shutdown()
{
	RED_LOG_INFO( "GameSession: Started shutdown" );

	// Multiple calls to DispatchEvents to handle possible transition to suspended mode
#if defined( RED_PLATFORM_DURANGO )
	DispatchEvents();
#endif

	m_isInitialized.SetValue( false );
	if ( m_gameInstance )
	{
		RED_LOG_INFO( "GameSession: Started destroying game instance" );

		IPlayerManager& playerManager = m_gameInstance->GetSystem< IPlayerManager >();
		if( Player* player = playerManager.GetLocalPlayer() )
		{
			GetInkSystem().ResetInputManager( player->GetManagerView() );
		}

		world::RuntimeSystemEntity * entityScene = m_scene->GetSystem< world::RuntimeSystemEntity >();
		entityScene->CloseSpawnService();

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		world::RuntimeSystemNodeStreaming * nodeStreamingSystem = m_scene->GetSystem< world::RuntimeSystemNodeStreaming >();
		nodeStreamingSystem->CleanupNodeInstances();

		m_gameInstance->PreDetachWorld( *m_scene );

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		m_scene->DetachGame( *m_gameInstance );

		entityScene->Editor_ForceSceneUpdate( 0.f );
		m_gameInstance->DetachWorld( *m_scene );

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		if( m_processor )
		{
			m_processor->Clear();
		}

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		m_scene->Shutdown();

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		m_gameInstance->Shutdown();

		if( auto supervisor = GEngine->GetFadeOutSupervisor() )
		{
			supervisor->OnSessionShutdown();
		}

#if defined( RED_PLATFORM_DURANGO )
		DispatchEvents();
#endif

		GEngine->GameSessionInitilized( false );
		m_gameInstance = nullptr;
		m_scene = nullptr;
		m_processor = nullptr;
	}

	SetGameStatus( GameStatus::ToDelete );
	RED_LOG_INFO( "GameSession: Finished shutdown" );
}

void game::BaseGameSession::IsSavingLocked( red::DynArray< game::SaveLock >& locks, const bool isForced ) const
{
	if ( m_gameInstance )
	{
		m_gameInstance->IsSavingLocked( locks, isForced );
	}
}

game::ShouldAutoSave game::BaseGameSession::ShouldAutoSaveGameInstance() const
{
	if ( m_gameInstance )
	{
		return m_gameInstance->ConsumeAllAutoSaveRequests();
	}
	return { false, 0 };
}

void game::BaseGameSession::ProcessView( const gsm::RenderContext& context )
{
	if ( !m_isInitialized.GetValue() )
	{
		return;
	}

	if ( !m_gameInstance )
	{
		return;
	}

	IPlayerManager& playerManager = m_gameInstance->GetSystem< IPlayerManager >();
	if ( Player* player = playerManager.GetLocalPlayer() )
	{
		player->CalcCamera( context );
	}
}

void game::BaseGameSession::RenderFrame( CRenderFrameInfo& frame, CRenderCameraFrameInfo& cameraFrameInfo )
{
	if ( !m_isInitialized.GetValue() )
	{
		return;
	}

	if ( !m_scene )
	{
		return;
	}

	// get runtime system rendering
	auto& runtimeSystemRendering = m_scene->GetSystemRef< world::RuntimeSystemRendering >();

	if ( !runtimeSystemRendering.IsNonInteractiveRenderingEnabled() )
	{
		// assign the world's render scene to the rendering frame, this makes the world show up in the viewport
		frame.SetScene( runtimeSystemRendering.GetLowLevelRenderScene() );
	}
	else
	{
		frame.SetNonInteractiveScene( runtimeSystemRendering.GetLowLevelRenderScene() );
	}

	// Set frame info reference position
	{
		frame.m_cameraReferencePosition = cameraFrameInfo.GetRenderCamera().GetPosition().AsVector3();
	}

	// initialize environment info
	auto& runtimeSystemWeather = m_scene->GetSystemRef< world::RuntimeSystemWeather >();
	runtimeSystemWeather.InitializeEnvironmentInfo( frame );
	runtimeSystemWeather.FinalizeEnvironmentInfo( frame );

	const auto& streamingSystem = m_scene->GetSystemRef< world::RuntimeSystemWorldStreaming >();
	const auto& streamingWorld = streamingSystem.GetStreamingWorld();
	frame.m_worldMaxBounds = streamingWorld ? streamingWorld->m_worldBoundingBox : Box::EMPTY();

	if( auto uiSystem = m_scene->GetSystem< world::ui::RuntimeSystemUI >() )
	{
		uiSystem->RenderFrame( frame );
	}
}

void game::BaseGameSession::RenderDebug( CRenderFrameInfo& frameInfo, CRenderCameraFrameInfo& cameraInfo ) const
{
	if ( !m_isInitialized.GetValue() )
	{
		return;
	}

#ifdef USE_PROFILER

	if ( m_scene )
	{
		// scene debug
		m_scene->RenderDebug( frameInfo, cameraInfo );
	}

	if ( m_gameInstance )
	{
		// game instance debug
		m_gameInstance->RenderDebug( frameInfo, cameraInfo );
	}

	if ( m_processor )
	{
		m_processor->RenderDebug();
	}

#endif

#ifdef RED_MONITOR_SCRIPTABLE_THREAD_SAFETY
	ScriptsHelper::DebugDrawScriptableThreadSafetyMonitor();
#endif
}

void game::BaseGameSession::ProcessInput( ::input::IInputContext& context )
{
	if ( !m_isInitialized.GetValue() )
	{
		return;
	}

	RED_FATAL_ASSERT ( m_gameInstance );

	IPlayerManager& playerManager = m_gameInstance->GetSystem< IPlayerManager >();
	if ( Player* player = playerManager.GetLocalPlayer() )
	{
		player->ProcessRawInput( context );
	}
#ifndef RED_CONFIGURATION_FINAL
	m_gameInstance->ProcessDebugInput( context );
#endif
}

void game::BaseGameSession::LoadGame( const job::RunContext& context, Bool& result, save::IStream& stream )
{
	m_gameInstance->LoadGame( context, result, stream );
}

// TODO? return future/task instead
void game::BaseGameSession::SaveGame( save::IStream& stream, save::GameMetadata& metadata, game::GameSessionDesc& desc, red::Atomic< Bool >& isFinished )
{
	m_gameInstance->SaveGame( stream, metadata, desc, isFinished );
}

void game::BaseGameSession::OnSaveGameFailed()
{
	m_gameInstance->OnSaveGameFailed();
}

void game::BaseGameSession::HandleAuthLost()
{
}

#ifndef RED_CONFIGURATION_FINAL
void game::BaseGameSession::LaunchExternalNetworkProfiler()
{
}
#endif

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif
