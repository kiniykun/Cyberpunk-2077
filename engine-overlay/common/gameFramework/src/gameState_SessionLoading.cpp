/**
 * Copyright (c) 2015-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include <cstdio>
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h> // [behindload]
#endif

#include "../../engine/include/fadeOutSupervisorInterface.h"
#include "../../world/include/gameInstance.h"
#include "../../renderData/include/renderFrameInfo.h"
#include "../../world/include/runtimeSystemRendering.h"
#include "../../world/include/runtimeSystemNodeStreaming.h"
#include "../../world/include/runtimeSystemTrigger.h"
#include "../../world/include/cameraSystemInterface.h"
#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"
#include "../../worldEntities/include/runtimeSystemEntity.h"
#include "../../worldEntities/include/runtimeSystemEntityAnimation.h"
#include "../../worldUserInterface/include/runtimeSystemUI.h"
#include "../../gameAudio/include/runtimeSystemAudio.h"

#include "../../inkCore/include/inkSystem.h"

#include "../../gameCore/include/questsSystemInterface.h"
#include "../../gameCore/include/gamePersistencySystemInterface.h"
#include "../../gameCore/include/gameISceneSystemInterface.h"
#include "../../gameCore/include/communitySystemInterface.h"
#include "../../gameCore/include/populationSystemInterface.h"
#include "../../gameCore/include/entitySpawnerEventsBroadcasterInterface.h"
#include "../../gameCore/include/playerSystemInterface.h"
#include "../../gameCore/include/entityStubSystem.h"
#include "../../gameCore/include/itemFactorySystemInterface.h"
#include "../../gameCore/include/mountingFacilityInterface.h"
#include "../../gameCore/include/transactionSystemInterface.h"

#include "../../renderDraw/include/fragmentBuilder.h"

#include "inputMappingTable.h"
#include "gameState_Session.h"
#include "gameState_SessionLoading.h"
#include "../../input/include/inputContext.h"
#include "../../redFileSystem/include/fileSys.h"
#include "../../redReflection/include/resourceLoaderThrottler.h"
#include "gameState_NotifierEvents.h"
#include "gameState_RequestEvents.h"
#include "../../worldPhysics/include/runtimeSystemPhysics.h"
#include "../../worldEntities/include/runtimeSystemRepellerComponents.h"
#include "../../redIO/include/redIOAsyncIO.h"
#include "../../redMemory/include/memoryAnalyzerUtils.h"
#include "../../sound/include/soundSystem.h"
#include "gameLoadingScreenCutscenePlayer.h"
#include "../../gameCore/include/questWorldStateSystemInterface.h"
#include "../../worldStreaming/include/worldStreamingPrefetch.h"

#include "../../inkCore/include/stateMachine_State.h"
#include "../../inkCore/include/stateMachine_States.h"

#include "../../input/include/inputSystemMessages.h"

#ifndef RED_CONFIGURATION_FINAL
#include "../../inkCore/include/stateMachine.h"
#endif
#include "../../redFileSystem/include/memoryFileWriter.h"
#include "../../redReflection/include/serializationSaver.h"
#include "../../redReflection/include/resourceSnapshot.h"
#include "../../redCore/include/commandline.h"
#include "gameStateErrors.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( State_SessionLoading, gsm )
RTTI_PARENT_TYPE( gsm::State )
RTTI_END_TYPE()

namespace dd
{
	static red::CrashData<String> loadingStage("Game", "LoadingStage");
}

namespace Config
{
	// #tbd: Normally should be for whole loading screen. But for now just per stage timeout.
	const constexpr Int32 TRCLoadingMax = 60;
	TConfigVar< Int32 > cvLoadingTimeoutSeconds( "Session/Loading", "LoadingTimeoutSeconds", 60, 1, TRCLoadingMax );

	// Minimum time the loading screen is up for, so it doesn't just flash
	// Can also be used to help debug teleport issues, so the debug text is up for a little longer
	TConfigVar< Float > cvMinLoadingScreenDurationSeconds( "Session/Loading", "MinDurationSeconds", 2.f );

	// Assume nobody read the message, avoid P0s after a certain time
	TConfigVar< Bool > cvDebugAutoSkipLoadingTimeout( "Session/Loading", "DebugAutoSkipLoadingTimeout", true );

	// Fake fadeout, ticking the game normally
	TConfigVar< Int32 > cvDebugTickFinishCounterMax( "Session/Loading", "DebugTickFinishCounterMax", 1 );

	TConfigVar< Float> cvUpdateBehindLoadingScreenDuration( "Session/Loading", "UpdateBehindLoadingScreenDuration", 2.f );

	// Warn if loading again too soon. High since there's a lot of unnecssary quest delays at the moment. Will need to revise.
	TConfigVar< Int32 > cvDebugLoadingCooldownWarning( "Session/Loading", "DebugLoadingCooldownWarning", 30 );

	TConfigVar< Float > cvLoadingResetIOQueuesSeconds("Session/Loading", "LoadingResetIOQueuesSeconds", 2.f);
}

namespace gsm
{

static rend::DebugFilter s_loadingScreenDebugFilter( "Overlay/LoadingScreen" );

const CName State_SessionLoading::EVT_SkipLoadingStage = RED_NAME_CONSTEXPR( "DebugSkipLoading" );

const Bool c_useDebugLoadingScreenQuest = false;
const res::GatheredResourceReference c_debugLoadingScreenQuest( RED_CONST_RESOURCEPATH( "base\\media\\loading_screens\\quests\\loading_screen_2_during_q001.quest" ), GRF_Debug );

enum SessionLoading_TransitionFlags : Uint32
{
	SLF_DisconnectedController = RED_FLAG( 0 ),
};

State_SessionLoading::State_SessionLoading()
	: State( "SessionLoading" )
	, m_asyncStatus( red::AsyncStatus::NotDone )
	, m_transitionFlags( 0 )
	, m_loadingCompleted( false )
	, m_loadingStage( LoadingStage::Invalid )
	, m_debugFinishingTickCounter( 0 )
	, m_transitionRequestFlags( 0 )
	, m_debugIsAllowSkipLoadingStage( false )
	, m_debugIsSkipLoadingStageRequested( false )
	, m_debugIsLoadingCooldownSecondsValid( false )
	, m_debugIsSessionPaused( false )
	, m_debugNormalizedStreamingProgress( 0.f )
	, m_debugLoadingCooldownSeconds( 0.f )
	, m_runtimeSystemsPendingReason( nullptr )
	, m_resetInput( false )
	, m_resettingInput( false )
	, m_playQuestRecap( false )
	, m_waitingForControllerReconection( false )
	, m_deviceId(0)
{
	m_loadingTimes.Resize( (Uint32)LoadingStage::MAX );
}

State_SessionLoading::~State_SessionLoading() = default;

Bool State_SessionLoading::OnProcessInit( ILazyFunctionScheduler& lazyScheduler, const ArgsView& args, ParamsView& params )
{
#ifndef RED_CONFIGURATION_FINAL
	ink::DEBUG_PushVisitedGameStateForIMGUI( "State_SessionLoading" );
#endif

	RED_MEMORY_MA_WRITE_BOOKMARK( "Session Loading", "Started loading the game" );

	SetTargetUIState( ParseTargetUIState( args ) );
	SetTargetUILoadingState( ParseTargetUILoadingState( args ) );

	if ( args.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "forceStartScreen" ) ) )
	{
		ForceTargetUIStartScreen();
	}

	if ( args.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "skipStartScreen" ) ) )
	{
		SkipTargetUIStartScreen();
	}

	if ( args.Has< StateError >( RED_NAME_CONSTEXPR_NOREG( "errorCode" ) ) )
	{
		SetErrorCode( args.Get< StateError >( RED_NAME_CONSTEXPR( "errorCode" ) ) );
	}

	if ( args.Has< String >( RED_NAME_CONSTEXPR_NOREG( "errorFile" ) ) )
	{
		SetErrorFile( args.Get< String >( RED_NAME_CONSTEXPR( "errorFile" ), String::EMPTY() ) );
	}

	m_resetInput = true;
	m_resettingInput = false;

	// read all required parameters
	if ( !InitializeContext( args, params ) )
	{
		OnError( StateError::StateError_CannotInitializeContext, "Cannot initialize context" );
		return false;
	}

	GetEventSink().PostMessage(EVT_NotifySessionLoadingInitialized, String::EMPTY());

	if ( audio::SoundSystem *soundSystem = GEngine->GetSoundSystem() )
	{
		GEngine->GetSoundSystem()->OnLoadingStart();
	}

	return true;
}

const State_SessionLoading::StageInfo State_SessionLoading::s_stageInfos[] = {
	StageInfo( LoadingStage::WaitingForObserver, &State_SessionLoading::UpdateObserverStatus ),
	StageInfo( LoadingStage::WaitingForStreaming, &State_SessionLoading::UpdateWorldStreamingStatus ),
	StageInfo( LoadingStage::WaitingForPopulation, &State_SessionLoading::UpdatePopulationStatus ),
	StageInfo( LoadingStage::WaitingForEntitiesFirstPhase, &State_SessionLoading::UpdateEntitySpawningStatus ),
	StageInfo( LoadingStage::WaitingForEntitiesSecondPhase, &State_SessionLoading::UpdateEntitySpawningStatus ),
	StageInfo( LoadingStage::WaitingForEntitiesThirdPhase, &State_SessionLoading::UpdateEntitySpawningStatus ),
	StageInfo( LoadingStage::WaitingForEntitiesFourthPhase, &State_SessionLoading::UpdateEntitySpawningStatus ),
	StageInfo( LoadingStage::WaitingForResourceLoadingFirstPhase, &State_SessionLoading::UpdateResourceLoadingStatus ),
	StageInfo( LoadingStage::WaitingForResourceLoadingSecondPhase, &State_SessionLoading::UpdateResourceLoadingStatus ),
	StageInfo( LoadingStage::WaitingForSystems, &State_SessionLoading::UpdateRuntimeSystems ),
	StageInfo( LoadingStage::WaitingForPhysicsSpatialTreeRebuild, &State_SessionLoading::UpdatePhysicsSpatialTree ),
	StageInfo( LoadingStage::WaitingForQuestRecapCutscene, &State_SessionLoading::UpdateQuestRecapCutscene),
	StageInfo( LoadingStage::WaitingForUIAcceptance, &State_SessionLoading::UpdateWaitingForUIAcceptance),
	StageInfo( LoadingStage::Finishing, &State_SessionLoading::UpdateFinishing ),
	StageInfo( LoadingStage::WaitingForUpdateBehindLoadingScreen, &State_SessionLoading::UpdateBehindLoadingScreen ),
};

Bool State_SessionLoading::InitializeContext( const ArgsView& args, ParamsView& params )
{
	args.Find( RED_NAME_CONSTEXPR_NOREG( "PlayQuestRecap" ), m_playQuestRecap );
	args.Find( RED_NAME_CONSTEXPR_NOREG( "isPointOfNoReturnSave" ), m_isPointOfNoReturnSave );

	auto session = GetParentState<State_Session>();
	if ( !session )
	{
		RED_LOG_ERROR( "Expected State_Session as parent!" );
		return false;
	}

	m_debugIsLoadingCooldownSecondsValid = session->DebugGetLoadingCooldownSeconds( m_debugLoadingCooldownSeconds );

	m_debugIsSessionPaused = args.Get( RED_NAME_CONSTEXPR( "Debug_SessionPaused" ), false );

#ifdef USE_SESSION_DEBUG_HELPER
	session->GetDebugHelper().m_debugMessage.m_text = "Game LOADING...";
	session->GetDebugHelper().m_debugMessage.m_timer = DebugMessage::DURATION;
	session->GetDebugHelper().m_debugMessage.m_color = Color::YELLOW();
#endif // USE_SESSION_DEBUG_HELPER

	EnterNextLoadingStage( *session );

	auto& scene = session->GetGameSession().GetScene();
	auto& gameInstance = session->GetGameSession().GetGameInstance();

	scene.GetSystemRef< world::RuntimeSystemEntity >().DisableSpawnThrottling();

	auto* questsSystem = gameInstance.GetSystemPtr< quest::IQuestsSystem >().Get();
	if (questsSystem)
	{
		RED_FATAL_ASSERT( !questsSystem->IsPaused(), "Quest system already paused?!" );
		questsSystem->Pause( true );
	}

	if (m_playQuestRecap && questsSystem && c_useDebugLoadingScreenQuest)
	{
		game::LoadingScreenCutscenePlayer::Setup setup;
		setup.questSceneResource.SetPath(c_debugLoadingScreenQuest.GetPath());
		m_cutscenePlayer = red::CreateUniquePtr< game::LoadingScreenCutscenePlayer >(session->GetGameSession(), setup);
	}

	// #tbd: some kind of unified loading mode manager
	io::GAsyncIO.EnabledLoadingMode(true);
	GResourceLoader->EnableGameCacheLoadingMode(true);
	scene.GetSystemRef< world::RuntimeSystemNodeStreaming >().EnableLoadingMode(true);

	TickRuntimeSystems( *session, true );

	ShowLoadingScreen();

	m_loadingTimer.Reset();
	m_cachedLoadingState = 0u;
	m_currentProgress = 0.0f;

	return true;
}

namespace helper
{
	static void CreateQuickLaunchSnapshot()
	{
		// #tbd: make configurable so archivesBuilding knows the path
		auto path = red::AbsolutePath::CreateFilePath("Z:\\quicklaunch.rsnapshot");
		auto snapshot = GResourceLoader->CreateResourceBankSnapshot(true);
		red::DynArray< Uint8 > dataBuffer{ red::PoolBackend() };
		CMemoryFileWriter writer(dataBuffer);
		serialization::SavingContext savingContext(snapshot);
		if (serialization::SaveToMemory(savingContext, writer))
		{
			red::UniquePtr< IFile > fileWriter(GFileManager->CreateFileWriter(path, FOF_AbsolutePath));
			if (fileWriter)
			{
				fileWriter->Serialize(dataBuffer.Data(), dataBuffer.Size());
				if (fileWriter->HasErrors())
				{
					GFileManager->DeleteFile(path); // delete the empty file
				}
			}
		}
	}

	// #fixme: RestoreGameSession() gets called twice... on finishing and during process shutdown
	static void RestoreGameSession( game::BaseGameSession& gameSession )
	{
		auto& scene = gameSession.GetScene();

		io::GAsyncIO.EnabledLoadingMode(false);
		scene.GetSystemRef< world::RuntimeSystemNodeStreaming >().EnableLoadingMode( false );
		GResourceLoader->EnableGameCacheLoadingMode( false );

		scene.GetSystemRef< world::RuntimeSystemRendering >().EnableNonInteractiveRendering( false );
		scene.GetSystemRef< world::RuntimeSystemEntity >().EnableSpawnThrottling();

#ifdef RED_USE_IMGUI
		if (auto* worldDebugging = scene.GetSystem< world::RuntimeSystemWorldDebugging >())
		{
			worldDebugging->GetStreamingEventLog().ClearEvents();
		}
#endif

#ifndef RED_CONFIGURATION_FINAL
		static const Bool createQuickLaunchSnapshot = red::CommandLine().Get().HasOption("create_quicklaunch_snapshot");
		if (createQuickLaunchSnapshot)
		{
			CreateQuickLaunchSnapshot();
		}
#endif

	}
}

void State_SessionLoading::OnProcessShutdown( ILazyFunctionScheduler& lazyScheduler )
{
	State::OnProcessShutdown( lazyScheduler );

	// Ensure game state restored even if state popped off the stack without going through all the loading stages
	auto session = GetParentState<State_Session>();
	RED_FATAL_ASSERT( session );

#ifdef USE_SESSION_DEBUG_HELPER
	session->GetDebugHelper().m_debugMessage.Clear();
#endif

	if ( THandle<State_Session> session = GetParentState<State_Session>() )
	{
		auto worldEntities = session->GetGameSession().GetGameInstance().GetScene().GetSystem< world::RuntimeSystemEntity >();
		RED_FATAL_ASSERT( worldEntities );
		worldEntities->EnableEntityTick( true );
		session->GetGameSession().ClearTickFilters();

		if( auto* questsSystem = session->GetGameSession().GetGameInstance().GetSystemPtr< quest::IQuestsSystem >().Get() )
		{
			questsSystem->Pause( false );
		}
	}

#ifdef USE_RESOURCE_METRICS_BANK
	// Snapshot of what was just loaded
	if (GResourceLoader->GetResourceMetricsBank())
	{
		GResourceLoader->GetResourceMetricsBank()->NextLoadingRequestSnapshot("LoadingScreen");
	}
#endif

	helper::RestoreGameSession(	session->GetGameSession() );
	DebugPrintLoadingTimes();

	session->DebugResetLoadingCooldown();
	// #tbd: Loading screen fadeout

	if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
	{
		loadingScreen->SetGameSessionLoadingFinished( false );
	}

	// TODO UI: should be triggered from input the same as the normal loading screen controller
	if (m_cutscenePlayer)
	{
		m_cutscenePlayer->RequestFinish();
	}

	HandlePointOfNoReturn( session->GetGameSession().GetGameInstance() );

	dd::loadingStage.Set("Finished");

	GetEventSink().PostMessage( EVT_NotifySessionLoadingFinished, String::EMPTY() );
}

const AnsiChar* State_SessionLoading::GetLoadingStageText( LoadingStage stage ) const
{
	switch ( stage )
	{
		case LoadingStage::Invalid: return "INVALID";
		case LoadingStage::WaitingForObserver: return "Spawning player";
		case LoadingStage::WaitingForStreaming: return "Loading world";
		case LoadingStage::WaitingForPopulation: return "Updating population";
		case LoadingStage::WaitingForEntitiesFirstPhase: return "Spawning entities";
		case LoadingStage::WaitingForEntitiesSecondPhase: return "Spawning entities";
		case LoadingStage::WaitingForEntitiesThirdPhase: return "Spawning entities";
		case LoadingStage::WaitingForEntitiesFourthPhase: return "Spawning entities";
		case LoadingStage::WaitingForResourceLoadingFirstPhase: return "Loading resources";
		case LoadingStage::WaitingForResourceLoadingSecondPhase: return "Loading resources";
		case LoadingStage::WaitingForSystems: return m_runtimeSystemsPendingReason ? m_runtimeSystemsPendingReason : "Waiting for systems";
		case LoadingStage::WaitingForPhysicsSpatialTreeRebuild: return "Rebuilding physics spatial tree.";
		case LoadingStage::WaitingForQuestRecapCutscene: return "WaitingForQuestRecapCutscene";
		case LoadingStage::WaitingForUIAcceptance: return "WaitingForUIAcceptance";
		case LoadingStage::Finishing: return "Finishing";
		case LoadingStage::WaitingForUpdateBehindLoadingScreen: return "WaitingForUpdateBehindLoadingScreen";
		default:
			break;
	}

	return "<UNKNOWN STAGE>";
}

void State_SessionLoading::DebugPrintLoadingTimes()
{
#ifdef RED_LOGGING_ENABLED
	RED_LOG_INFO( "State_SessionLoading completed. Loading stage times" );
	for ( Uint32 i = (Uint32)LoadingStage::Invalid + 1; i < (Uint32)LoadingStage::MAX; ++i )
	{
		const LoadingStage stage = LoadingStage( i );
		RED_LOG_INFO( ">> %hs: %.2f sec", GetLoadingStageText( stage ), m_loadingTimes[ i ] );
	}
	RED_LOG_INFO( "<<" );
#endif
}

void State_SessionLoading::EnterNextLoadingStage( State_Session& session )
{
	const LoadingStage newLoadingStage = (LoadingStage)( (Uint32)(m_loadingStage)+1 );
	if( newLoadingStage == LoadingStage::MAX )
	{
		return;
	}

	m_loadingTimes[ (Uint32)m_loadingStage ] = m_loadingStageTimer.GetDeltaF();
	RED_FATAL_ASSERT( newLoadingStage > m_loadingStage, "Always make progress. If want to revisit a stage, then will need to make it another enum" );

	const red::StaticArray< const rtti::ClassType*, 16 > baseLoadingTickFilterTypes = {
		ClassID< world::RuntimeScene >(),
		ClassID< world::RuntimeSystemNodeStreaming >(), // drives streaming
		ClassID< quest::IQuestsSystem >(), // drives other loading requests. Not adding questsHelper because it seems to be way to dependant on AI and active gamestate.
		ClassID< game::IPersistencySystem >(), // can be needed by quest system to progress and for persistent storage events
		ClassID< game::EntityStubSystem >(), // stub creation requests must be handled when restoring a loaded game
		ClassID< game::IEntitySpawnerEventsBroadcaster >(), // spawn/despawn events must be always handled ASAP
		ClassID< game::ISceneSystem >(), // drives other loading requests
		ClassID< world::RuntimeSystemAudio >(), //Keep wwise ticking while we load so we don't overflow the command buffer
		ClassID< world::ui::RuntimeSystemUI >(), // loading screen
		ClassID< game::IItemFactorySystem >(), // items spawning, scenes can't start if items are not spawned
		ClassID< game::mounting::IMountingFacility >(), // mounting / unmounting
		ClassID< world::RuntimeSystemTriggers >(), // update performance and other triggers while loading game
	};

	auto& scene = session.GetGameSession().GetScene();

	dd::loadingStage.Set(GetLoadingStageText(newLoadingStage));
	std::fprintf(stderr,"CP2077 loading stage probe: phase=enter stage=%s value=%u\n",GetLoadingStageText(newLoadingStage),unsigned(newLoadingStage));
	if ( m_loadingStage != LoadingStage::Invalid && m_loadingStage != LoadingStage::MAX )
	{
		GetEventSink().PostMessage( EVT_NotifyLoadingStageFinished, GetLoadingStageText( m_loadingStage) );
	}

	if ( newLoadingStage != LoadingStage::Invalid && m_loadingStage != LoadingStage::MAX )
	{
		GetEventSink().PostMessage( EVT_NotifyLoadingStageStarted, GetLoadingStageText( newLoadingStage ) );
		io::GAsyncIO.ProfileMarkEvent( GetLoadingStageText( newLoadingStage ) );
	}

	//

	switch ( newLoadingStage )
	{
	case LoadingStage::WaitingForObserver:
		{
			NotifyStateSubAction( "WaitingForObserver" );

			red::StaticArray< const rtti::ClassType*, 7 > observerTickFilterTypes = {
				ClassID< world::RuntimeScene >(), // drives streaming, and needed to actually set the observer position
				ClassID< world::IRuntimeSystem >(), // tick all engine systems, like physics. Apparently can be needed to avoid teleports to the origin.
				ClassID< game::EntityStubSystem >(), // stub creation requests must be handled when spawning player objects
				ClassID< game::IPlayerSystem >(), // player game objects spawning
				ClassID< game::ICameraSystem >(), // camera streaming observer update
				ClassID< game::IPopulationSystem >(),
				ClassID< game::IEntitySpawnerEventsBroadcaster >(),
			};

			red::StaticArray< const rtti::ClassType*, 2 > exceptTickFilterTypes = {
				ClassID< world::RuntimeSystemPhysics >(),	// please don't update the physics while the world is still loading
				ClassID< world::RuntimeSystemRepellerComponents >(), // this system depends on physics system
			};
			session.GetGameSession().SetTickFilters( observerTickFilterTypes, exceptTickFilterTypes );
		}
	break;
	case LoadingStage::WaitingForStreaming:
		{
			NotifyStateSubAction( "WaitingForStreaming" );

			auto& sysWorldStreaming = scene.GetSystemRef< world::RuntimeSystemWorldStreaming >();
			auto& sysNodeStreaming = scene.GetSystemRef< world::RuntimeSystemNodeStreaming >();
			auto& sysRendering = scene.GetSystemRef< world::RuntimeSystemRendering >();

			session.GetGameSession().SetTickFilters( baseLoadingTickFilterTypes );
			
			io::GAsyncIO.EnabledLoadingMode(true);
			sysNodeStreaming.EnableLoadingMode( true );

			sysRendering.EnableNonInteractiveRendering( true );

			auto session = GetParentState<State_Session>();
			RED_FATAL_ASSERT(session);
			auto& gameInstance = session->GetGameSession().GetGameInstance();

			// Start the saved prefetch here while anything in the loading screen still cares about the streaming completion state
			auto* sysWorldStateSystem = gameInstance.GetSystemPtr< quest::IWorldStateSystem >().Get();
			if (sysWorldStateSystem)
			{
				// Ideally even during OnGameLoad() but he world hasn't been mounted yet.
				// If a prefetch quest node reactivates, then it will cancel and start a new prefetch with its own callback
				// This is for the case where the prefetch was ready, and signalled the quest node, so the signal exited the node.
				// This way I shouldn't have to care about some possible race condition either, to make sure everything is in sync.
				// It seems as if QuestsSystem::RecreateSignalsAfterLoad() should be called after this. If there's some race, then the prefab variants above
				// would also be affected.
				world::StreamingPrefetchParams params;
				if (sysWorldStateSystem->TryExtractSavedPrefetch(params))
				{
					sysWorldStreaming.BeginStreamingPrefetch(params, nullptr);
				}
			}
		}
		break;
	case LoadingStage::WaitingForPopulation:
		{
			NotifyStateSubAction( "WaitingForPopulation" );

			if ( !scene.GetRuntimeInfo().IsClient() )	//community and population systems are not present on clients
			{
				auto tickFilterTypes = baseLoadingTickFilterTypes;
				tickFilterTypes.PushBack( ClassID< world::IRuntimeSystem >() );
				tickFilterTypes.PushBack( ClassID< game::ICommunitySystem >() );
				tickFilterTypes.PushBack( ClassID< game::IPopulationSystem >() );

				red::StaticArray< const rtti::ClassType*, 2 > exceptTickFilterTypes = {
					ClassID< world::RuntimeSystemPhysics >(),	// please don't update the physics while the world is still loading
					ClassID< world::RuntimeSystemRepellerComponents >(), // this system depends on physics system
				};

				session.GetGameSession().SetTickFilters( tickFilterTypes, exceptTickFilterTypes );
			}
		}
		break;
	case LoadingStage::WaitingForEntitiesFirstPhase:
		{
			NotifyStateSubAction( "WaitingForEntitiesFirstPhase" );
			WaitingForEntitiesStage( newLoadingStage, baseLoadingTickFilterTypes, session, scene );
		}
		break;
	case LoadingStage::WaitingForEntitiesSecondPhase:
		{
			NotifyStateSubAction( "WaitingForEntitiesSecondPhase" );
			WaitingForEntitiesStage( newLoadingStage, baseLoadingTickFilterTypes, session, scene );
		}
		break;
	case LoadingStage::WaitingForEntitiesThirdPhase:
		{
			NotifyStateSubAction( "WaitingForEntitiesThirdPhase" );
			WaitingForEntitiesStage( newLoadingStage, baseLoadingTickFilterTypes, session, scene );
		}
		break;
	case LoadingStage::WaitingForEntitiesFourthPhase:
		{
			NotifyStateSubAction( "WaitingForEntitiesFourthPhase" );
			WaitingForEntitiesStage( newLoadingStage, baseLoadingTickFilterTypes, session, scene );
		}
		break;
	case LoadingStage::WaitingForResourceLoadingFirstPhase:
		{
			NotifyStateSubAction( "WaitingForResourceLoadingFirstPhase" );
			WaitingForResourceLoadingStage();
		}
		break;
	case LoadingStage::WaitingForResourceLoadingSecondPhase:
		{
			NotifyStateSubAction( "WaitingForResourceLoadingSecondPhase" );
			WaitingForResourceLoadingStage();
		}
		break;
	case LoadingStage::WaitingForSystems:
		{
			NotifyStateSubAction( "WaitingForSystems" );

			// Rendering needs to be updated as normal for certain systems to finish. E.g., distant shadows
			scene.GetSystemRef< world::RuntimeSystemRendering >().EnableNonInteractiveRendering(false);

			// Do nothing, just wait for all the deferred pending render proxy attaches to start
			// If we time out here, then it means we're attaching an insane amount of things are loading should have been finished
		}
		break;
	case LoadingStage::WaitingForPhysicsSpatialTreeRebuild:
		{
			NotifyStateSubAction( "WaitingForPhysicsSpatialTreeRebuild" );
		}
		break;
	case LoadingStage::WaitingForQuestRecapCutscene:
		{
			NotifyStateSubAction("WaitingForQuestRecapCutscene");
		}
		break;
	case LoadingStage::WaitingForUIAcceptance:
		{
			NotifyStateSubAction( "WaitingForUIAcceptance" );
			if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
			{
				loadingScreen->LoadingAssetsComplete();
			}
		}
		break;
	case LoadingStage::Finishing:
		{
			const red::StaticArray< const rtti::ClassType*, 15 > baseLoadingTickFilterTypesNoScene = {
			ClassID< world::RuntimeSystemNodeStreaming >(), // drives streaming
			ClassID< quest::IQuestsSystem >(), // drives other loading requests. Not adding questsHelper because it seems to be way to dependant on AI and active gamestate.
			ClassID< game::IPersistencySystem >(), // can be needed by quest system to progress and for persistent storage events
			ClassID< game::EntityStubSystem >(), // stub creation requests must be handled when restoring a loaded game
			ClassID< game::IEntitySpawnerEventsBroadcaster >(), // spawn/despawn events must be always handled ASAP
			ClassID< world::ui::RuntimeSystemUI >(), // loading screen
			ClassID< game::IItemFactorySystem >(), // items spawning, scenes can't start if items are not spawned
			ClassID< world::AnimationSystem >(), // without AnimationSystem update animated objects would stay in A-pose as no data is passed to skinning
				};

			red::StaticArray< const rtti::ClassType*, 3 > exceptTickFilterTypes = {
				ClassID< world::RuntimeScene >(),
				ClassID< world::RuntimeSystemPhysics >(),	// please don't update the physics while the world is still loading
				ClassID< world::RuntimeSystemRepellerComponents >(), // this system depends on physics system
			};
			session.GetGameSession().SetTickFilters( baseLoadingTickFilterTypesNoScene, exceptTickFilterTypes );
			auto worldEntities = scene.GetSystem< world::RuntimeSystemEntity >();
			worldEntities->EnableEntityTick( false );

			NotifyStateSubAction( "Finishing" );

			if ( audio::SoundSystem *soundSystem = GEngine->GetSoundSystem() )
			{
				GEngine->GetSoundSystem()->OnGameStart();
			}

			helper::RestoreGameSession( session.GetGameSession() );
		}
		break;
	case LoadingStage::WaitingForUpdateBehindLoadingScreen:
		{
			auto worldEntities = scene.GetSystem< world::RuntimeSystemEntity >();
			worldEntities->EnableEntityTick( true );

			const red::StaticArray< const rtti::ClassType*, 1 > exclusionList = { ClassID< quest::IQuestsSystem >() };
			session.GetGameSession().ClearTickFilters();
			session.GetGameSession().ModTickFilters( exclusionList, false );

			if ( auto supervisor = GEngine->GetFadeOutSupervisor() )
			{
				supervisor->OnForceUpdateTime( true );
			}
			session.ResetUpdateBehindLoadingScreenTimer();
			NotifyStateSubAction( "WaitingForUpdateBehindLoadingScreen" );
		}
		break;
	default:
		RED_FATAL( "Unhandled loading stage: (%hs) %u", GetLoadingStageText( m_loadingStage ), m_loadingStage );
		break;
	}

	m_loadingStage = newLoadingStage;
	m_loadingStageTimer.Reset();
	m_debugIsAllowSkipLoadingStage = false;
	m_debugIsSkipLoadingStageRequested = false; // could be a race between next stage and processing event meant for previous, but just debug
}

void State_SessionLoading::WaitingForEntitiesStage( LoadingStage newLoadingStage, const red::StaticArray< const rtti::ClassType*, 16 >& baseLoadingTickFilterTypes, State_Session& session, world::RuntimeScene& scene )
{
	auto worldEntities = scene.GetSystem< world::RuntimeSystemEntity >();
	RED_FATAL_ASSERT( worldEntities );
	m_spawnerFences = worldEntities->CreateSpawnerFences();
	// #tbd: could get problematic if entities start attacking the player or each other. Need to tick in order for the game object spawners to do anything...
	// plus have the spawners check their results and do something with the object
	auto entitiesTickFilterTypes = baseLoadingTickFilterTypes;
	entitiesTickFilterTypes.PushBack( ClassID< world::IRuntimeSystem >() ); // all engine systems enabled, including entity system
	entitiesTickFilterTypes.PushBack( ClassID< game::IPopulationSystem >() );
	worldEntities->EnableEntityTick( false );

	red::StaticArray< const rtti::ClassType*, 2 > exceptTickFilterTypes = {
		ClassID< world::RuntimeSystemPhysics >(),	// please don't update the physics while the world is still loading
		ClassID< world::RuntimeSystemRepellerComponents >(), // this system depends on physics system
	};

	session.GetGameSession().SetTickFilters( entitiesTickFilterTypes, exceptTickFilterTypes );

	// call OnGameStateRestored() in the last phase of loading entities, when the game is being restored from save
	// this is for any initialization that needs to happen when all the systems and entities are loaded
	if ( newLoadingStage == LoadingStage::WaitingForEntitiesFourthPhase )
	{
		session.GetGameSession().GetGameInstance().OnGameRestored();
	}
}

void State_SessionLoading::WaitingForResourceLoadingStage()
{
	job::Counter loadingFenceCounter{ job::Priority::Latent };
	GResourceLoader->SyncLoadingFence( loadingFenceCounter );
	m_loadingCompleted.SetValue( false );
	job::Builder builder{ job::Priority::Latent };
	builder.DispatchWait( loadingFenceCounter );
	builder.DispatchJob( "WaitForResourceLoading", [this]( const job::RunContext & context )
	{
		m_loadingCompleted.SetValue( true );
	} );
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateObserverStatus( const State_Session& session )
{
	auto& scene = session.GetGameSession().GetScene();
	auto worldStreaming = scene.GetSystem< world::RuntimeSystemWorldStreaming >();

	Float debugNormalizedProgress = 0.f;
	const Bool hasValidObserverPosition = worldStreaming->HasValidObserverPosition();

	THandle<game::IPlayerSystem> playerSystem = session.GetGameSession().GetGameInstance().GetSystemPtr<game::IPlayerSystem>();
	const Bool playerObjectSpawningFinished = playerSystem->AreLocalPlayerObjectsSpawned();
	// Can't start streaming if no observer yet
	if ( !hasValidObserverPosition || !playerObjectSpawningFinished )
	{
		return StageStatus::Pending;
	}

	return StageStatus::Finished;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateWorldStreamingStatus( const State_Session& session )
{
	Float debugNormalizedProgress = 0.f;
	auto& scene = session.GetGameSession().GetScene();
	auto worldStreaming = scene.GetSystem< world::RuntimeSystemWorldStreaming >();

	if ( worldStreaming->IsStreaming( debugNormalizedProgress ) )
	{
		DebugProcessStreamingProgress( session, debugNormalizedProgress );

		return StageStatus::Pending;
	}

	io::GAsyncIO.EnabledLoadingMode(false);
	scene.GetSystemRef< world::RuntimeSystemNodeStreaming >().EnableLoadingMode( false );

	return StageStatus::Finished;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdatePopulationStatus( const State_Session& session )
{
	const game::IGameInstance& gameInstance = session.GetGameSession().GetGameInstance();
	const game::ICommunitySystem* communitySystem = gameInstance.GetSystemPtr< game::ICommunitySystem >().Get();
	const game::IPopulationSystem* populationSystem = gameInstance.GetSystemPtr< game::IPopulationSystem >().Get();

	//community and population systems are not present on clients
	Bool communitySystemReady = ( communitySystem == nullptr || !communitySystem->HasPendingRequests() );
	Bool populationSystemReady = ( populationSystem == nullptr || populationSystem->IsReady() );

	return ( communitySystemReady && populationSystemReady ) ? StageStatus::Finished : StageStatus::Pending;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateEntitySpawningStatus( const State_Session& session )
{
	const auto pred = []( const red::UniquePtr< ent::EntitySpawnerFence >& fence ) { return fence->IsReady(); };
	m_spawnerFences.Remove( std::remove_if( m_spawnerFences.Begin(), m_spawnerFences.End(), pred ), m_spawnerFences.End() );

	auto& scene = session.GetGameSession().GetScene();
	auto worldEntities = scene.GetSystem< world::RuntimeSystemEntity >();
	RED_FATAL_ASSERT( worldEntities );
	if ( !m_spawnerFences.Empty() )
	{
		return StageStatus::Pending;
	}

	return StageStatus::Finished;
}

void State_SessionLoading::ProcessInput( input::IInputContext& context )
{
	State::ProcessInput( context );

	Bool doProcessInput = false;

	if( m_resetInput )
	{
		// First update of SesstionLoading
		// Request is scheduled, reset will be performed next frame
		context.RequestReset();
		m_resetInput = false;
		m_resettingInput = true;

		// But we want to process input this time, because input state might changed before reset
		doProcessInput = true;
	}
	else if( m_resettingInput )
	{
		// Second update of SesstionLoading
		// This time context contains reseted input
		m_resettingInput = false;
		doProcessInput = true;
	}

	if( doProcessInput )
	{
		auto session = GetParentState<State_Session>();
		RED_FATAL_ASSERT( session );
		session->GetGameSession().ProcessInput( context );
	}
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateResourceLoadingStatus( const State_Session& session )
{
	if ( !m_loadingCompleted.GetValue() )
	{
		return StageStatus::Pending;
	}

	return StageStatus::Finished;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateRuntimeSystems( const State_Session& session )
{
#if defined(__EMSCRIPTEN__)
    // Observe the original readiness gate without changing its result.
    static Uint32 readinessPoll = 0;
    const Uint32 poll = ++readinessPoll;
    const Bool traceReadiness = poll <= 4 || (poll & (poll - 1)) == 0;
#endif

	// Quest system
	{
		auto& gameInstance = session.GetGameSession().GetGameInstance();

		// #KN: #TODO:
		// There should be something like gameInstance.IsReady() that internally interates over all systemMap items and checks via interface
		// if that system is ready. This way we don't need to have this kind of special cases for particular systems here

		if(auto* questsSystem = gameInstance.GetSystemPtr< quest::IQuestsSystem >().Get())
		{
			if( false == questsSystem->IsReady() )
			{
#if defined(__EMSCRIPTEN__)
                if (traceReadiness) std::fprintf(stderr, "CP2077 runtime readiness probe: poll=%u system=quests pending=1\n", unsigned(poll));
#endif
				return StageStatus::Pending;
			}
		}
	}

	// Runtime systems
	{
		auto& scene = session.GetGameSession().GetScene();

		for(Uint32 idx = 0; idx < world::RuntimeScene::MAX_SYSTEMS; ++idx)
		{
			if(auto system = scene.GetSystem( idx ))
			{
				if( auto reason = system->IsReady() )
				{
					m_runtimeSystemsPendingReason = reason;
#if defined(__EMSCRIPTEN__)
                    if (traceReadiness) std::fprintf(stderr, "CP2077 runtime readiness probe: poll=%u systemIndex=%u reason=%s pending=1\n", unsigned(poll), unsigned(idx), reason);
#endif

					return StageStatus::Pending;
				}
			}
		}
	}

	return StageStatus::Finished;
}

gsm::State_SessionLoading::StageStatus State_SessionLoading::UpdatePhysicsSpatialTree( const State_Session& session )
{
	auto& scene = session.GetGameSession().GetScene();
	physics::ForceSpatialTreeRebuild( world::GetSceneID( scene ), true, true );

	return StageStatus::Finished;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateQuestRecapCutscene(const State_Session& session)
{
	if (!m_cutscenePlayer || m_cutscenePlayer->CanRequestFinish())
	{
		return StageStatus::Finished;
	}

	return StageStatus::Pending;
}

gsm::State_SessionLoading::StageStatus State_SessionLoading::UpdateWaitingForUIAcceptance(const State_Session& session)
{
	if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
	{
		if( loadingScreen->ShouldFinishLoadingState() )
		{
			return StageStatus::Finished;
		}
		else
		{
			return StageStatus::Pending;
		}
	}

	return StageStatus::Finished;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateFinishing( const State_Session& session )
{
	if ( m_debugFinishingTickCounter >= (Uint32)Config::cvDebugTickFinishCounterMax.Get() )
	{
		return StageStatus::Finished;
	}

	m_debugFinishingTickCounter += 1;
	return StageStatus::Pending;
}

State_SessionLoading::StageStatus State_SessionLoading::UpdateBehindLoadingScreen( const State_Session& session )
{
#if defined(__EMSCRIPTEN__)
	// [behindload] 2026-10-24: a fixed 2 s of simulation behind the loading screen ran after everything was ready.
	// ?behindLoad=<seconds 0..5> overrides (A/B against the first visible frame); default = the config value.
	static const double cpBehind = MAIN_THREAD_EM_ASM_DOUBLE( { try { const v = new URLSearchParams( location.search ).get( 'behindLoad' ); const n = v === null ? -1 : Number( v ); return Number.isFinite( n ) && n >= 0 && n <= 5 ? n : -1; } catch ( e ) { return -1; } } );
	const Float cpBehindSeconds = cpBehind >= 0 ? Float( cpBehind ) : Config::cvUpdateBehindLoadingScreenDuration.Get();
	if ( session.GetUpdateBehindLoadingScreenElapsedSeconds() > cpBehindSeconds )
#else
	if ( session.GetUpdateBehindLoadingScreenElapsedSeconds() > Config::cvUpdateBehindLoadingScreenDuration.Get() )
#endif
	{
		if ( auto supervisor = GEngine->GetFadeOutSupervisor() )
		{
			supervisor->OnForceUpdateTime( true );
		}
		m_asyncStatus = red::AsyncStatus::Done;
		return StageStatus::Finished;
	}

	return StageStatus::Pending;
}

void State_SessionLoading::DebugProcessStreamingProgress( const State_Session& session, Float debugNormalizedProgress )
{
	// Not 100% accurate - if suddenly more proxies to load. Just be less confusing about progress going backwards.
	if ( debugNormalizedProgress > m_debugNormalizedStreamingProgress )
	{
		m_debugNormalizedStreamingProgress = debugNormalizedProgress;
	}
}

void State_SessionLoading::ShowLoadingScreen() const
{
	if( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
	{
		loadingScreen->SetGameSessionLoadingFinished( false );

		loadingScreen->SetLoadingState( ink::ILoadingScreen::State::Loading );

		GetInkSystem().PlaySound( RED_NAME_CONSTEXPR( "ST_Loading_Screen_On_Set_State" ) );

		if ( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
		{
			THandle< ink::StateChangeRequestEvent > stateEvent;

			switch ( GetTargetUILoadingState() )
			{
			case ink::State::InitEngine:
				stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkInitEngineState" ) );
				break;
			case ink::State::InitialLoading:
				stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkInitialLoadingState" ) );
				break;
			case ink::State::FastTravelLoading:
				stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkFastTravelLoadingState" ) );
				break;
			case ink::State::EndGameLoading:
				stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkEndGameLoadingState" ) );
				break;
			}

			RED_ASSERT( stateEvent );
			viewportWrapper->QueueViewportEvent( stateEvent );
		}

#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
		GEngine->PauseScreenshotCapture();
#endif
	}
}

void State_SessionLoading::HideLoadingScreen() const
{
	if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
	{
		auto state = GetTargetUISkipStartScreen() ? ink::ILoadingScreen::State::Hidden : ink::ILoadingScreen::State::WaitForKey;

		loadingScreen->SetLoadingState( state );

		GetInkSystem().PlaySound( RED_NAME_CONSTEXPR( "ST_Loading_Screen_Off_Set_State" ) );

#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
		GEngine->ResumeScreenshotCapture();
#endif
	}
}

void State_SessionLoading::CalculateLoadingScreenProgress( Float deltaTime )
{
	const Float averageStateLoadingTime = 20.0f;

	if ( m_loadingStage == LoadingStage::MAX )
	{
		return SetLoadingScreenProgress( 1.0f );
	}

	const Uint32 totalStates = RED_ARRAY_COUNT_U32( s_stageInfos ) - 1;

	Uint32 loadedStates = ( Uint32 )m_loadingStage;
	loadedStates = loadedStates > 0 ? loadedStates - 1 : loadedStates;

	if ( m_cachedLoadingState != loadedStates )
	{
		m_currentProgress = ( Float )loadedStates / totalStates;
		if ( m_cachedLoadingState == 0 )
		{
			m_averageProgressSpeed = 1 / ( totalStates * averageStateLoadingTime );
		}
		else
		{
			m_averageProgressSpeed = loadedStates / ( totalStates * m_loadingTimer.GetDeltaF() );
		}
		m_cachedLoadingState = loadedStates;
	}

	const Float nextState = ( loadedStates + 1.0f ) / totalStates;
	const Float maxStep = ( nextState - m_currentProgress ) * deltaTime / 4.0f;
	const Float step = m_averageProgressSpeed * deltaTime;

	m_currentProgress = Clamp( m_currentProgress + Min( maxStep, step ), 0.0f, 1.0f );
	SetLoadingScreenProgress( m_currentProgress );
}

void State_SessionLoading::SetLoadingScreenProgress( Float progress ) const
{
	if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
	{
		loadingScreen->SetProgress( progress );
	}
}

void State_SessionLoading::TickRuntimeSystems( const State_Session& session, Bool isFirstFrame )
{
	auto& scene = session.GetGameSession().GetScene();

	auto sysRendering = scene.GetSystem<world::RuntimeSystemRendering>();

	if ( m_cutscenePlayer && m_cutscenePlayer->IsSceneStarted())
	{
		// #tbd: cleanup the management of this, don't want to switch it back on since
		// disabled later for rendering systems to finish up, but probably won't with the camera
		// at the wrong position.
		if (sysRendering)
		{
			sysRendering->EnableNonInteractiveRendering(false);
		}
	}

	for(Uint32 idx = 0; idx < world::RuntimeScene::MAX_SYSTEMS; ++idx)
	{
		if(auto system = scene.GetSystem( idx ))
		{
			// Tick system while its under loading stuff
			system->TickWhileLoading( isFirstFrame );
		}
	}
}

void State_SessionLoading::ProcessTick( const TickContext& context, StateTickFlagsContext& flags, TickControl& control )
{
	State::ProcessTick( context, flags, control );

	auto session = GetParentState<State_Session>();
	RED_FATAL_ASSERT( session );

	if ( flags.TryConsumeFlag( StateTickFlagsContext::ThrottleLoading ) )
	{
		if ( GResourceLoader && GResourceLoader->GetThrottler() )
		{
			GResourceLoader->GetThrottler()->SetLoadingSpeed( res::ThrottleLoadingSpeed::Flood );
		}
	}

	if (m_cutscenePlayer)
	{
		// HACK: because the loading screen apparently isn't available at first
		if (auto loadingScreen = GetInkSystem().GetLoadingScreen())
		{
			if (loadingScreen->GetLoadingState() != ink::ILoadingScreen::State::WaitForKey) // TODO: don't show some video if we were playing a cutscene
			{
				loadingScreen->SetLoadingState(ink::ILoadingScreen::State::Hidden);
			}
		}
		m_cutscenePlayer->Tick(context.m_timeDelta, context.m_tickInfo);
	}

	job::Builder builder;
	session->GetGameSession().BuildTickChain( builder, context.m_timeDelta, context.m_tickInfo );
	job::FlushCounter( builder.ExtractWaitCounter() );

	// If there is no active game go to main menu; Can only happen in multiplayer when server disconnects.
	if ( session->GetGameSession().GetGameStatus() == game::GameStatus::Idle )
	{
		GetEventSink().PostMessage( EVT_RequestExitToMainMenu, "" );
	}

	game::data::GetTweakDB().TickSync( context.m_tickInfo.m_realTimeDelta ); //#tbd: not sure if really need to tick this

	auto findStageInfo = [this]( LoadingStage stage ) -> const StageInfo& {
		RED_UNUSED( this );
		for ( Uint32 i = 0; i < RED_ARRAY_COUNT_U32( s_stageInfos ); ++i )
		{
			if ( s_stageInfos[ i ].m_stage == stage )
				return s_stageInfos[ i ];
		}

		RED_FATAL_ASSERT( "Could not find loading stage %s (%u)", GetLoadingStageText( stage ), stage );
		static StageInfo nullInfo;
		return nullInfo;
	};

	if ( (m_loadingStageTimer.GetDeltaF() > Config::cvLoadingTimeoutSeconds.Get()) && ( LoadingStage::WaitingForUIAcceptance != (LoadingStage)m_loadingStage) )
	{
		m_debugIsAllowSkipLoadingStage = true;
	}

#ifndef RED_CONFIGURATION_FINAL
# ifndef RED_CONFIGURATION_NOPTS // too slow in noopts and may also be debugging loading
	if (  m_debugIsAllowSkipLoadingStage && m_loadingStageTimer.GetDeltaF() > 2 * Config::cvLoadingTimeoutSeconds.Get() )
	{
		if ( Config::cvDebugAutoSkipLoadingTimeout.Get() )
		{
			m_debugIsSkipLoadingStageRequested = true;
		}
	}
# endif
#endif

	const Bool doSkipLoadingStage = m_debugIsAllowSkipLoadingStage && m_debugIsSkipLoadingStageRequested;
	m_debugIsSkipLoadingStageRequested = false;
	RED_WARNING( !doSkipLoadingStage, "SessionLoading: Skipping loading stage %hs", GetLoadingStageText( m_loadingStage ) );

	// Tick runtime systems while loading
	TickRuntimeSystems( *session, false );

	// Note: we tick again afer each stage transition. Good for letting a system start kicking things off now that a certain state is reached.
	// #tbd: quests might cause prefab variants to load, or teleports to far off lands. Might need to reset loadingStage and check again at some point.
	// Also need texture/animation streaming etc.
	if ( m_loadingStage == LoadingStage::Invalid )
	{
		EnterNextLoadingStage( *session );
	}
	else
	{
		auto& stageInfo = findStageInfo( m_loadingStage );
		const StageStatus status = ( this->*stageInfo.m_func )( *session );
		if ( status == StageStatus::Finished || doSkipLoadingStage )
		{
			EnterNextLoadingStage( *session );
		}
	}

	CalculateLoadingScreenProgress( context.m_tickInfo.m_realTimeDelta );
}

TransitionRequest State_SessionLoading::ProcessTransition()
{
	Uint32 transitionFlags = m_transitionFlags;
	m_transitionFlags = 0;

	// ORDER determines priority
	const Bool finishLoading = m_asyncStatus == red::AsyncStatus::Done;

	const Uint32 requestFlags = m_transitionRequestFlags;

	if( ( requestFlags & SLF_DisconnectedController ) != 0 )
	{
		// Show disconnected message after we finish loading.
		if( finishLoading )
		{
			m_waitingForControllerReconection = true;
			m_transitionRequestFlags = 0;

			Args args;
			args.Set( RED_NAME_CONSTEXPR( "deviceID" ), m_deviceId);
			return TransitionRequest::Push( "ReconnectController", args );
		}
	}

	if ( finishLoading && m_loadingTimer.GetDeltaF() > Config::cvMinLoadingScreenDurationSeconds.Get() && !m_waitingForControllerReconection )
	{
		Bool loadingScreenFinishedPlaying{ true };

		if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
		{
			if ( !loadingScreen->IsGameSessionLoadingFinished() )
			{
				loadingScreen->SetGameSessionLoadingFinished( finishLoading );
				HideLoadingScreen();
			}

			loadingScreenFinishedPlaying = !loadingScreen->IsLoadingScreenUp();
		}

		if ( loadingScreenFinishedPlaying )
		{
			Args args;
			args.Set( RED_NAME_CONSTEXPR( "Debug_ProcessOneTick" ), m_debugIsSessionPaused );
			args.Set( RED_NAME_CONSTEXPR( "targetUIState" ), GetTargetUIState() );

			ForwardCommonArgs( args );

			return TransitionRequest::ReplaceTop( "SessionActive", args );
		}
	}

	// stay where you are
	return TransitionRequest();
}

void State_SessionLoading::OnChildStateShutdown()
{
	m_waitingForControllerReconection = false;
}

void State_SessionLoading::ProcessRender( const RenderContext& context, RenderControl& control )
{
	State::ProcessRender( context, control );

	// render additional debug
	RenderDebug( context.m_frame, context.m_cameraFrameInfo, context.m_tickInfo );
}

void State_SessionLoading::RenderDebug( CRenderFrameInfo& frameInfo, CRenderCameraFrameInfo& cameraInfo, const tick::Info& tickInfo ) const
{
#ifdef RED_PLATFORM_WINPC
	const Bool isPC = true;
#else
	const Bool isPC = false;
#endif

	if (!cameraInfo.GetDebugDrawer().GetMask().Test(s_loadingScreenDebugFilter))
	{
		if (!isPC || !m_debugIsAllowSkipLoadingStage)
		{
			return;
		}
	}

	auto& dd = cameraInfo.GetDebugDrawer();
	rend::FragmentBuilder builder( dd );

	builder.SetColor( Color::YELLOW() );

	auto session = GetParentState<State_Session>();

	char msgBuf[ 256 ] = "";
	if( m_loadingStage == LoadingStage::WaitingForStreaming )
	{
		red::SNPrintFUnsafe( msgBuf, RED_ARRAY_COUNT_U32( msgBuf ), "%s: %d%%...", GetLoadingStageText( m_loadingStage ), static_cast< Int32 >( m_debugNormalizedStreamingProgress * 100.f ) );
	}
	else if (m_loadingStage == LoadingStage::WaitingForSystems)
	{
		const char* pendingReason = (m_runtimeSystemsPendingReason && *m_runtimeSystemsPendingReason ) ? m_runtimeSystemsPendingReason : "<Unknown runtimesystem reason>";
		red::SNPrintFUnsafe(msgBuf, RED_ARRAY_COUNT_U32(msgBuf), "%s: %.1f sec...", pendingReason, m_loadingStageTimer.GetDeltaF());
	}
	else
	{
		red::SNPrintFUnsafe( msgBuf, RED_ARRAY_COUNT_U32( msgBuf ), "%s: %.1f sec...", GetLoadingStageText( m_loadingStage ), m_loadingStageTimer.GetDeltaF() );
	}
	builder.AddFlatText( 100, 100, msgBuf );
	if (m_debugIsAllowSkipLoadingStage)
	{
		rend::FragmentBuilder builder{ cameraInfo.GetDebugDrawer() };
		builder.SetPostImGui(true);

		builder.SetColor(Color::YELLOW());
		builder.AddFlatText(20, builder.GetScreenHeight() / 2, msgBuf);
	}


	if( m_debugIsAllowSkipLoadingStage )
	{
		const char* skipText = "***TIMEOUT**: Press ESC/B to FORCE skip loading stage";
		builder.SetColor( Color::RED() );
		builder.AddFlatText( 100, 120, skipText );
	}

	auto& scene = session->GetGameSession().GetScene();
	auto worldStreaming = scene.GetSystem< world::RuntimeSystemWorldStreaming >();

	const Vector3 lastObserverPos = worldStreaming->GetDebugLastObserverPosition();

	builder.SetColor( Color::YELLOW() );

	if( m_debugIsLoadingCooldownSecondsValid && m_debugLoadingCooldownSeconds <= ( Float ) Config::cvDebugLoadingCooldownWarning.Get() )
	{
		red::SNPrintFUnsafe( msgBuf, RED_ARRAY_COUNT_U32( msgBuf ), "*** Pos: [%.1f,%.1f,%.1f], Teleported: [%.2f m], [RELOAD WITHIN %1.2f sec ] ***",
							 lastObserverPos.X, lastObserverPos.Y, lastObserverPos.Z,
							 worldStreaming->GetDebugLastTeleportDistance(), m_debugLoadingCooldownSeconds );
	}
	else
	{
		red::SNPrintFUnsafe( msgBuf, RED_ARRAY_COUNT_U32( msgBuf ), "Pos: [%.1f,%.1f,%.1f] Teleported: [%.2f m]",
							 lastObserverPos.X, lastObserverPos.Y, lastObserverPos.Z,
							 worldStreaming->GetDebugLastTeleportDistance() );
	}

	builder.AddFlatText( 400, 40, msgBuf );
}

void State_SessionLoading::ProcessServices( services::GameServices& services )
{
}

void State_SessionLoading::InitializeInputMapping( InputMappingTable& mappingTable, const ArgsView& args ) const
{
#ifndef RED_CONFIGURATION_FINAL
	mappingTable.MapKeyPress( IK_Escape, EVT_SkipLoadingStage );
	mappingTable.MapKeyPress( IK_Pad_B_CIRCLE, EVT_SkipLoadingStage );
#endif
}

const Bool State_SessionLoading::ProcessMessage( const ExternalMessage& msg, const StateMachine &gsm )
{
	if ( msg.m_name == EVT_SkipLoadingStage )
	{
		m_debugIsSkipLoadingStageRequested = true;
		return true;
	}
	else if( msg.m_name == input::Messages::GamepadDisconnectedMessage )
	{
		m_deviceId = FromStringDirect<Uint64>(msg.m_params);
		m_transitionRequestFlags |= SLF_DisconnectedController;
		return true;
	}
	else if( msg.m_name == input::Messages::GamepadConnectedMessage )
	{
		m_transitionRequestFlags &= ~SLF_DisconnectedController;
		return true;
	}

	return false;
}

const Bool State_SessionLoading::ProcessMappedInput( const MappedInput& input )
{
	if ( input.m_inputEvent == EVT_SkipLoadingStage )
	{
		GetEventSink().PostMessage( EVT_SkipLoadingStage, String::EMPTY() );
	}

	return false;
}

void State_SessionLoading::OnError( const StateError errorCode, const AnsiChar* debugMessage, const AnsiChar* filePath )
{
	const THandle< State_Session > parentSession = GetParentState< State_Session >();
	if ( parentSession )
	{
		parentSession->OnError( errorCode, debugMessage, filePath);
	}
}

void State_SessionLoading::HandlePointOfNoReturn( game::IGameInstance& gameInstance )
{
	// First add rewards. Then let now quest system to set facts based on that.
	// After PonR loading scene is being handled by the quest system checking PONR_FACT_NAME fact
	// Displaying rewards prompt is also being triggered by quest system checking PONR_REWARD_FACT_NAME fact
	if( m_isPointOfNoReturnSave )
	{
		Bool rewardsAdded = false;

		THandle < game::ITransactionSystem> inventory = gameInstance.GetSystemPtr<game::ITransactionSystem>();
		if( inventory )
		{
			rewardsAdded = inventory->HandlePointOfNoReturnLoot();
		}

		THandle < quest::IQuestsSystem > questSystem = gameInstance.GetSystemPtr<quest::IQuestsSystem>();
		if( questSystem )
		{
			questSystem->HandlePointOfNoReturn( gameInstance, rewardsAdded );
		}

		// Hide couple of frames between after loading PonR and triggering playing welcoming scene
		// This FadeOut is supposed to be disabled by FadeIn in quests after welcoming scene is done
		if( auto supervisor = GEngine->GetFadeOutSupervisor() )
		{
			supervisor->OnFadeOut( true, 0.f );
		}
	}
}

} // namespace gsm
