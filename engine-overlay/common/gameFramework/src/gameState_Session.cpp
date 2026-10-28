/**
 * Copyright (c) 2015-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include "gameState_Session.h"
#include "inputMappingTable.h"

#if defined( RED_SAVE_SERVER_ENABLED )
#include "saveServer.h"
#endif

#include "../../../common/renderData/include/renderFrameInfo.h"
#include "../../../common/game/include/gameSession.h"
#include "../../../common/multiplayerGame/include/clientGameSession.h"
#include "../../../common/input/include/inputContext.h"
#include "../../../common/inkCore/include/inkSystem.h"
#include "../../../common/redCore/include/commandline.h"

#include "../../../common/renderDraw/include/fragmentBuilder.h"
#include "../../redReflection/include/resourceLoaderThrottler.h"
#include "gameState_NotifierEvents.h"
#include "gameState_RequestEvents.h"
#include "inGameRequestsHandler.h"
#include "gameStateErrors.h"
#include "../../gameCore/include/gameAutoSaveSystemInterface.h"
#include "../../gameCore/include/gameSessionProperties.h"
#include "../../gameCore/include/characterCustomizationSystemInterface.h"
#include "../../gameCore/include/telemetrySystemInterface.h"
#include "../../world/include/gameInstance.h"
#include "../../sound/include/soundSystem.h"
#include "../../game/include/puppetHelper.h"
#include "../../inkCore/include/systemNotificationsUtilities.h"
#include "../../inkCore/include/stateMachine_States.h"
#include "../../inkCore/include/stateMachine_State.h"
#include "../../gameServices/include/gameServices.h"
#include "../../gameTweakDB/include/types/tweakDBGender.h"
#include "../../inkCore/include/stateMachine.h"
#include "../../redIO/include/redIOAsyncIO.h"

RTTI_BEGIN_TYPE_IN_NAMESPACE( State_Session, gsm )
	RTTI_PARENT_TYPE( gsm::State )
RTTI_END_TYPE()

rend::DebugFilter DF_RenderInput( "Input/KeyStates" );

#ifndef RED_CONFIGURATION_FINAL
red::StaticArray< Bool, EInputKey::IK_Count > g_keyStates;
Float g_wheelDelta;
#endif

namespace Config
{
	TConfigVar< Int32 > cvCheckpointSavePeriod( "SaveConfig", "CheckpointSavePeriod", 60, 0, 3600, eConsoleVarFlag_Save );
}

// CP2077 web [saves]:
//  (1) ?sandbox / ?sandboxWorld / ?sandboxGamedef pages never write saves (auto, quick, manual, PONR):
//      a gym save became the main menu's CONTINUE and loading it crashed. &sandboxSaves=1 = debug opt-in.
//  (2) save-load watch: packageReadStream.cpp counts out-of-range package reads; while a saved session
//      loads (armed in BaseGameSession::Initialize, disarmed when SessionActive starts) any overrun fails
//      the load through the original error path (StateError_CantLoadSave_Corrupted -> PreGameSession
//      with errorCode = main menu + error message) instead of running on zero-filled data.
#include <atomic>
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
extern "C" Uint32 cpweb_SaveLoadWatchTripped();
extern "C" void cpweb_SaveLoadWatchDisarm();
extern "C" Uint32 cpweb_PackageReadOverrunCount();
namespace cpweb_session_saves
{
	static Bool IsSandboxNoSavePage()
	{
		static std::atomic< Int32 > s_value{ -1 };
		Int32 v = s_value.load();
		if ( v < 0 )
		{
#ifdef __EMSCRIPTEN__
			v = MAIN_THREAD_EM_ASM_INT( {
				try {
					const q = new URLSearchParams( location.search );
					const sandbox = q.has( 'sandbox' ) || q.has( 'sandboxWorld' ) || q.has( 'sandboxGamedef' );
					const allow = q.get( 'sandboxSaves' );
					const watson = q.get( 'start' ) === 'watson' && !( q.get( 'watsonSaves' ) === '1' || allow === '1' || allow === 'true' );
					return ( ( sandbox && !( allow === '1' || allow === 'true' ) ) || watson ) ? 1 : 0;
				} catch ( e ) { return 0; }
			} );
#else
			v = 0;
#endif
			s_value.store( v );
		}
		return v > 0;
	}

	// CP2077 web [watson]: ?start=watson (debug start in the post-prologue q001 state, see gameQuest phaseInstance.cpp).
	// The new-game session (character state present, not a save load) spawns the player at the original spawn-point
	// marker of the chosen q001 entry (positions composed from the depot prefab chain, read-only research):
	//   apartment (default): #q001_spn_player       (tag q001_spn_player, q001_v_room.prefab "test" group, V's room in
	//                        loc_megabuilding_a) = (-1381.496, 1271.756, 123.089) yaw -94.36 - the cp77_chicken_test
	//                        "Q001 Vroom" teleport target
	//   q001 / rescue:       #q001_spn_player_start (tag q001_spn_start_test = q001.gamedef's tag, q001_00a_before_mission.prefab)
	//                        = (-438.977, 414.679, 23.283) yaw -92.04 - Mission 0's own teleport target
	// &watsonPos=x,y,z / &watsonYaw=deg override. Saves stay off (as on ?start=nomadfree); &watsonSaves=1 re-enables.
	static Bool WatsonSpawn( Vector3& pos, Float& yaw )
	{
		static std::atomic< Int32 > s_mode{ -1 };
		static Float s_p[ 4 ] = {};
		Int32 v = s_mode.load();
		if ( v < 0 )
		{
			v = 0;
#ifdef __EMSCRIPTEN__
			v = MAIN_THREAD_EM_ASM_INT( {
				try {
					const q = new URLSearchParams( location.search );
					if ( q.get( 'start' ) !== 'watson' || q.get( 'missionflow' ) === '0' || q.has( 'jump' ) ) return 0;
					const e = ( q.get( 'watsonEntry' ) || '' ).toLowerCase();
					const rescue = e === 'q001' || e === 'rescue' || e === 'mission0';
					let p = rescue ? ( [ -438.977, 414.679, 23.283, -92.04 ] ) : ( [ -1381.496, 1271.756, 123.089, -94.36 ] );
					const o = ( q.get( 'watsonPos' ) || '' ).split( ',' ).map( Number );
					if ( o.length === 3 && o.every( Number.isFinite ) ) { p[ 0 ] = o[ 0 ]; p[ 1 ] = o[ 1 ]; p[ 2 ] = o[ 2 ]; }
					const y = Number( q.get( 'watsonYaw' ) );
					if ( q.has( 'watsonYaw' ) && Number.isFinite( y ) ) p[ 3 ] = y;
					const out = Number( $0 );
					for ( let i = 0; i < 4; ++i ) HEAPF32[ Math.floor( out / 4 ) + i ] = p[ i ];
					return rescue ? 2 : 1;
				} catch ( e ) { return 0; }
			}, ( double )( uintptr_t )&s_p[ 0 ] );
#endif
			s_mode.store( v );
		}
		if ( v <= 0 )
		{
			return false;
		}
		pos = Vector3( s_p[ 0 ], s_p[ 1 ], s_p[ 2 ] );
		yaw = s_p[ 3 ];
		return true;
	}
}

namespace gsm
{
	static constexpr CName EVT_AuthLost = RED_NAME_CONSTEXPR_NOREG( "GameServicesAuthLost" );
	static constexpr CName EVT_AutoSave = RED_NAME_CONSTEXPR_NOREG( "DoAutoSave" );		// auto-created by the game
	static constexpr CName EVT_QuickSave = RED_NAME_CONSTEXPR_NOREG( "DoQuickSave" );   // player-created (like: F5), but on a special slot
	static constexpr CName EVT_ManualSave = RED_NAME_CONSTEXPR_NOREG( "DoManualSave" ); // regular player-created save
	static constexpr CName EVT_PointOfNoReturnSave = RED_NAME_CONSTEXPR_NOREG( "PointOfNoReturnSave" ); // Point of no return save
	static constexpr CName EVT_EndGameSave = RED_NAME_CONSTEXPR_NOREG( "EndGameSave" ); // End game save.

	State_Session::State_Session( const char* name )
		: State( name )
		, m_processor( nullptr )
		, m_gameInstance( nullptr )
		, m_scene( nullptr )
		, m_inputDeviceID( std::numeric_limits<::input::DeviceID >::max() )
		, m_saveLastSuccessFlags( 0 )
		, m_saveLastSuccessTime( 0 )
#ifdef USE_SESSION_DEBUG_HELPER
		, m_debugHelper( red::CreateUniquePtr< SessionDebugHelper >( m_gameSession ) )
#endif // USE_SESSION_DEBUG_HELPER
		, m_debugIsLoadingCooldownTimerValid( false )
		, m_isExitGameRequested( false )
		, m_isExitSessionRequested( false )
		, m_isExitToMainMenuRequested( false )
		, m_hasTransitioned( false )
		, m_gameSaveInProgress( false )
		, m_gameSaveListRequested( false )
		, m_streamingTestFinished( false )
	{
#ifndef RED_CONFIGURATION_FINAL
		g_keyStates.Resize( IK_Count );
#endif
	}

	State_Session::~State_Session()
	{
		if( m_gameSession )
		{
			m_gameSession->Shutdown();
		}
	}

	void State_Session::Prepare( IEventSink* eventSink, const StatePtr& parentState, SystemsUpdater* processor, game::IGameInstance* gameInstance, world::RuntimeScene* scene, const ArgsView& args )
	{
		State::Prepare( eventSink, parentState, processor, gameInstance, scene, args );

		RED_FATAL_ASSERT( !m_processor, "Processor is already set" );
		m_processor = processor;

		RED_FATAL_ASSERT( !m_gameInstance, "Game instance is already set" );
		m_gameInstance = gameInstance;

		RED_FATAL_ASSERT( !m_scene, "Runtime scene is already set" );
		m_scene = scene;
	}

	void State_Session::InitializeInputMapping( InputMappingTable& mappingTable, const ArgsView& args ) const
	{
#ifndef RED_CONFIGURATION_FINAL
		mappingTable.MapKeyPress( IK_F10, EVT_RequestExitGame );
#endif

#ifdef USE_SESSION_DEBUG_HELPER
		m_debugHelper->InitializeInputMapping( mappingTable );
#endif // USE_SESSION_DEBUG_HELPER
	}

	Bool State_Session::OnProcessInit( ILazyFunctionScheduler& lazyScheduler, const ArgsView& args, ParamsView& params )
	{
#ifndef RED_CONFIGURATION_FINAL
		if ( !args.Has< Bool >( RED_NAME_CONSTEXPR( "DEBUG_DontPushVisitedGameStateForIMGUI" ) ) )
		{
			ink::DEBUG_PushVisitedGameStateForIMGUI( "State_Session" );
		}
#endif

		if ( GetTargetUIState() == ink::State::Invalid )
		{
			const ink::State uiState = ParseTargetUIState( args );
			const ink::State uiLoadingState = ParseTargetUILoadingState( args );

			if ( uiState != ink::State::Invalid )
			{
				SetTargetUIState( uiState );
				SetTargetUILoadingState( uiLoadingState );
			}
			else
			{
				SetTargetUIState( ink::State::Game );
				SetTargetUILoadingState( ink::State::InitialLoading );
			}
		}

		RED_FATAL_ASSERT( !GetParentState() || GetParentState()->GetClass()->GetName() != RED_NAME_CONSTEXPR_NOREG( "gsmState_Session" ),
						  "Invalid parent state %hs", GetParentState()->GetClass()->GetName().AsChar() );

		// read all required parameters
		if( !InitializeContext( args, params ) )
		{
			OnError( StateError::StateError_CannotInitializeContext, "Cannot initialize context" );
			return false;
		}

		SetupSystemHandler( m_handler );
		GetInkSystem().SetSystemRequestsHandler( m_handler );

#if !defined( NO_DEBUG_CONSOLE )
		// "pragmatic solution"
		lazyScheduler.AddFunction( [this]( const Float timeDelta, const tick::Info& tickInfo ) {
			CNativeFunctionsRegistry& reg = GetNativeFunRegistry();
			reg.RegisterNativeFunction( "autosave", "Trigger creation of an autosave",
										[]( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData ) {
											reinterpret_cast< IEventSink* >( userData )->PostMessage( EVT_AutoSave, parameters.Empty() ? String::EMPTY() : parameters[ 0 ] );
											return true;
										},
										&GetEventSink() );
			reg.RegisterNativeFunction( "quicksave", "Trigger creation of a quick save",
										[]( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData ) {
											reinterpret_cast< IEventSink* >( userData )->PostMessage( EVT_QuickSave, parameters.Empty() ? String::EMPTY() : parameters[ 0 ] );
											return true;
										},
										&GetEventSink() );

			reg.RegisterNativeFunction( "save", "Trigger creation of a manual save",
										[]( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData ) {
											reinterpret_cast< IEventSink* >( userData )->PostMessage( EVT_ManualSave, parameters.Empty() ? String::EMPTY() : parameters[ 0 ] );
											return true;
										},
										&GetEventSink() );
			reg.RegisterNativeFunction( "PONRsave", "Trigger creation of a point of no return save",
										[] ( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData ) {
										reinterpret_cast< IEventSink* >( userData )->PostMessage( EVT_PointOfNoReturnSave, parameters.Empty() ? String::EMPTY() : parameters[0] );
										return true;
										},
										&GetEventSink() );
			reg.RegisterNativeFunction( "endgamesave", "Trigger creation of an end game save",
										[] ( IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData ) {
										reinterpret_cast< IEventSink* >( userData )->PostMessage( EVT_EndGameSave, parameters.Empty() ? String::EMPTY() : parameters[0] );
										return true;
										},
										&GetEventSink() );

			return red::AsyncStatus::Done;
		} );
#endif

		// we still need to wait for the session to initialize
		return true;
	}

	Bool State_Session::InitializeContext( const ArgsView& args, ParamsView& params )
	{
		// get session description
		m_gameSessionDesc = args.Get< game::GameSessionDesc >( RED_NAME_CONSTEXPR_NOREG( "gameSessionDesc" ), game::GameSessionDesc() );

		// create game session
		switch( m_gameSessionDesc.GetSessionConfig().m_runtimeMode )
		{
		case world::RuntimeMode::Singleplayer:
			m_gameSession = red::CreateUniquePtr< game::GameSession >();
			break;
		case world::RuntimeMode::Multiplayer_Client:
			m_gameSession = red::CreateUniquePtr< mp::ClientGameSession >();
			break;
		default:
			RED_LOG_ERROR( "GameState: State_Session: Unable to initialize game session" );
			return false;
		}

		// get input device id
		m_inputDeviceID = args.Get< Uint64 >( RED_NAME_CONSTEXPR_NOREG( "inputDeviceID" ), 0 );

		// get player spawn info
		game::PlayerSpawnParams spawnParams;

		TweakDBID playerRecordID;
		if ( args.Find< TweakDBID >( RED_NAME_CONSTEXPR_NOREG( "playerRecordID" ), playerRecordID ) )
		{
			spawnParams.m_recordId = playerRecordID;
		}
		else
		{
			spawnParams.m_recordId = params.Get< TweakDBID >( RED_NAME_CONSTEXPR_NOREG( "playerRecordID" ), GetDefaultPlayerCharacterRecordId() );
		}

		// allow to use spawn points from params
		if( params.Has< Vector3 >( RED_NAME_CONSTEXPR_NOREG( "startPosition" ) ) &&
			params.Has< EulerAngles >( RED_NAME_CONSTEXPR_NOREG( "startRotation" ) ) )
		{
			RED_LOG( "InitializeContext: using startPosition" );
			spawnParams.m_spawnPoint.SetPosition( params.Get< Vector3 >( RED_NAME_CONSTEXPR_NOREG( "startPosition" ), Vector3() ) );
			spawnParams.m_spawnPoint.SetOrientation( params.Get< EulerAngles >( RED_NAME_CONSTEXPR_NOREG( "startRotation" ), EulerAngles() ) );
			spawnParams.m_useSpecifiedStartPoint = true;
		}
		else if( args.Find( RED_NAME_CONSTEXPR_NOREG( "spawnTags" ), spawnParams.m_spawnTags ) )
		{
			params.Find< red::TagList >( RED_NAME_CONSTEXPR_NOREG( "spawnTags" ), spawnParams.m_spawnTags );
			RED_LOG( "InitializeContext: spawnTags=%hs", spawnParams.m_spawnTags.ToString().AsChar() );
		}

		// [watson] ?start=watson: the new-game session (character state, not a save load, not the menu session) spawns
		// at the original q001 entry marker instead of the lifepath's newGameSpawnTag (see cpweb_session_saves::WatsonSpawn).
		{
			Vector3 watsonPos;
			Float watsonYaw = 0.f;
			if( !m_gameSessionDesc.IsLoadingSavedSession() && m_gameSessionDesc.GetCharacterCustomizationState() &&
				!spawnParams.m_useSpecifiedStartPoint && cpweb_session_saves::WatsonSpawn( watsonPos, watsonYaw ) )
			{
				spawnParams.m_spawnPoint.SetPosition( watsonPos );
				spawnParams.m_spawnPoint.SetOrientation( EulerAngles( 0.f, 0.f, watsonYaw ) );
				spawnParams.m_useSpecifiedStartPoint = true;
				std::fprintf( stderr, "CP2077 watson: new-game spawn at (%.3f, %.3f, %.3f) yaw %.2f (was spawnTags=%s)\n",
					watsonPos.X, watsonPos.Y, watsonPos.Z, watsonYaw, spawnParams.m_spawnTags.ToString().AsChar() );
			}
		}

		if( !m_gameSessionDesc.IsLoadingSavedSession() )
		{
			static const thread_local CName c_defaultGenderName = RED_NAME_CONSTEXPR( "Default" );
			CName gender = c_defaultGenderName;

			if ( !IsMultiplayer() )
			{
				if ( const auto& ccState = m_gameSessionDesc.GetCharacterCustomizationState() )
				{
					gender = game::PuppetHelper::EnumToName( ccState->IsBodyGenderMale() ? game::data::Gender::Male : game::data::Gender::Female );
				}
				else
				{
					String playerGender;

					if ( !args.Find< String >( RED_NAME_CONSTEXPR_NOREG( "playerGender" ), playerGender ) )
					{
						if ( params.Has< String >( RED_NAME_CONSTEXPR_NOREG( "playerGender" ) ) )
						{
							playerGender = params.Get< String >( RED_NAME_CONSTEXPR_NOREG( "playerGender" ), String() );
						}
					}

					if ( !playerGender.Empty() )
					{
						gender = RED_NAME( playerGender );
					}
				}
			}

			spawnParams.m_gender = gender;

			String playerNickname;
			if ( red::CommandLine::Get().GetFirstParam( "playerNickname", playerNickname ) )
			{
				spawnParams.m_nickname = playerNickname;
			}
			else
			{
				spawnParams.m_nickname = game::GetCurrentUserName();
			}
		}

		// set player config
		game::LocalPlayerConfig localPLayerConfig;
		localPLayerConfig.m_spawnLocalPlayer = !m_gameSessionDesc.IsLoadingSavedSession();
		localPLayerConfig.m_spawnParams = spawnParams;
		m_gameSessionDesc.SetLocalPlayerConfig( localPLayerConfig );

		// setup quest config
		String startQuest;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "startQuest" ), startQuest ) )
		{
			game::QuestConfig questConfig;
			questConfig.m_runQuest = true;
			questConfig.m_questPath = res::ResourcePath::Build( startQuest );

			m_gameSessionDesc.SetQuestConfig( questConfig );
		}

		// setup population config
		game::PopulationConfig populationConfig;
		Bool alwaysSpawnedDisabled = false;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "alwaysSpawnedDisabled" ), alwaysSpawnedDisabled ) )
		{
			populationConfig.m_alwaysSpawnedDisabled = alwaysSpawnedDisabled;
		}
		Bool forceSpawnInView = game::PopulationConfig::c_forceSpawnInViewDefault;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "forceSpawnInView" ), forceSpawnInView ) )
		{
			populationConfig.m_forceSpawnInView = forceSpawnInView;
		}
		Float spawnRadius = 0.0f;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "entitySpawnRadius" ), spawnRadius ) )
		{
			populationConfig.m_spawnRadius = spawnRadius;
		}
		Bool noCrowd = false;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "noCrowd" ), noCrowd ) )
		{
			populationConfig.m_noCrowd = noCrowd;
		}
		Bool noReducedCrowd = false;
		if( params.Find( RED_NAME_CONSTEXPR_NOREG( "noReducedCrowd" ), noReducedCrowd ) )
		{
			populationConfig.m_noReducedCrowd = noReducedCrowd;
		}
		m_gameSessionDesc.SetPopulationConfig( populationConfig );

		if( params.Has< String >( RED_NAME_CONSTEXPR_NOREG( "gameDefinition" ) ) )
		{
			m_gameDefinition = params.Get< String >( RED_NAME_CONSTEXPR_NOREG( "gameDefinition" ), String::EMPTY() );
		}

		const Bool streamingTestRequested = args.Get<Bool>( RED_NAME_CONSTEXPR_NOREG( "streamingTest" ), false );
		m_gameSessionDesc.SetStreamingTestRequested( streamingTestRequested );

		m_saveLastSuccessTime = EngineTime::GetNow();

		// check if desc is valid, if game session is restored from a save, desc will be read later.
		return m_gameSessionDesc.IsLoadingSavedSession() || m_gameSessionDesc.Validate();
	}

	void State_Session::OnProcessShutdown( ILazyFunctionScheduler& lazyScheduler )
	{
		State::OnProcessShutdown( lazyScheduler );

#if !defined( NO_DEBUG_CONSOLE )
		CNativeFunctionsRegistry& reg = GetNativeFunRegistry();
		reg.UnregisterNativeFunction( "save" );
		reg.UnregisterNativeFunction( "quicksave" );
		reg.UnregisterNativeFunction( "autosave" );
#endif

		// #tbd: if we ever really want unlimited anywhere
		// Unlimit in case we didn't get ticked again.
		if( GResourceLoader && GResourceLoader->GetThrottler() )
		{
			GResourceLoader->GetThrottler()->SetLoadingSpeed( res::ThrottleLoadingSpeed::Unlimited );
		}

		GetEventSink().PostMessage( EVT_NotifySessionExitFinished, String::EMPTY() );
	}

	void State_Session::ProcessTick( const TickContext& context, StateTickFlagsContext& flags, TickControl& control )
	{
		if ( GEngine->RequestedExit() )
		{
			m_isExitSessionRequested = true;
		}

		if( m_gameSession->IsStreamingTestFinished() )
		{
			m_streamingTestFinished = true;
		}

		State::ProcessTick( context, flags, control );

		// CP2077 web [saves]: fail a saved-session load that read out of range (see top of file).
		if( cpweb_SaveLoadWatchTripped() )
		{
			cpweb_SaveLoadWatchDisarm();
			const Bool savedLoad = m_gameSessionDesc.IsLoadingSavedSession();
			std::fprintf( stderr, "CP2077 saves: save-load watch tripped (package read overruns=%u, savedSession=%d save='%s') -> StateError_CantLoadSave_Corrupted, back to main menu\n",
				cpweb_PackageReadOverrunCount(), savedLoad ? 1 : 0, m_gameSessionDesc.GetSaveName().AsChar() );
			if( savedLoad )
			{
				OnError( StateError::StateError_CantLoadSave_Corrupted,
					"Can't load save file, it seems to be corrupted (out-of-range data).",
					m_gameSessionDesc.GetSaveName().AsChar() );
			}
		}

		if( m_handler )
		{
			m_handler->Update();
		}

		if( flags.TryConsumeFlag( StateTickFlagsContext::ThrottleLoading ) )
		{
			if( GResourceLoader && GResourceLoader->GetThrottler() )
			{
				// #tbd: may want flood here instead, but for now just go full speed as a default
				GResourceLoader->GetThrottler()->SetLoadingSpeed( res::ThrottleLoadingSpeed::Unlimited );
			}
		}

#ifdef USE_SESSION_DEBUG_HELPER
		m_debugHelper->ProcessTick( context, flags, control );
#endif // USE_SESSION_DEBUG_HELPER
	}

	TransitionRequest State_Session::ProcessTransition()
	{
		if( m_handler )
		{
			if( TransitionRequest transitionRequest = m_handler->ProcessTransition() )
			{
				RED_LOG_INFO( "State_Session::ProcessTransition: Processing transition in BaseRequestHandler" );
				return transitionRequest;
			}
		}

		// ORDER determines priority
		if( !m_hasTransitioned )
		{
			m_hasTransitioned = true;

			Args args;
			args.Set( RED_NAME_CONSTEXPR( "targetUIState" ), GetTargetUIState() );
			args.Set( RED_NAME_CONSTEXPR( "targetUILoadingState" ), GetTargetUILoadingState() );

			ForwardCommonArgs( args );

			if( m_gameSessionDesc.IsLoadingSavedSession() )
			{
				RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transion to SessionRestoreFromSave" );
				return TransitionRequest::Push( "SessionRestoreFromSave", args );
			}
			else
			{
				RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transion to SessionNewGame" );
				return TransitionRequest::Push( "SessionNewGame", args );
			}
		}

		if ( IsErrorLocal() && GetErrorCode() != StateError::StateError_OK )
		{
			Args args;
			args.Set( RED_NAME_CONSTEXPR( "skipStartScreen" ), true );
			args.Set( RED_NAME_CONSTEXPR( "targetUILoadingState" ), ink::State::EndGameLoading );
			args.Set( RED_NAME_CONSTEXPR( "errorCode" ), GetErrorCode() );
			args.Set( RED_NAME_CONSTEXPR( "errorFile" ), GetErrorFile() );

			return TransitionRequest::ReplaceAll( "PreGameSession", args );
		}

		if( m_isExitGameRequested && !m_gameSaveInProgress )
		{
			RED_LOG_INFO( "State_Session::ProcessTransition: Requesting game exit" );
			m_isExitGameRequested = false;
			return TransitionRequest::PopAll();
		}

		if( m_isExitSessionRequested && !m_gameSaveInProgress )
		{
			RED_LOG_INFO( "State_Session::ProcessTransition: Requesting session exit" );

			m_isExitSessionRequested = false;

			// Reset to InitEngine state for functional tests
			if ( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
			{
				THandle< ink::StateChangeRequestEvent > stateEvent;
				stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkInitEngineState" ) );
				viewportWrapper->QueueViewportEvent( stateEvent );
			}

			return TransitionRequest::PopSelf();
		}

		if( m_isExitToMainMenuRequested && !m_gameSaveInProgress )
		{
			RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transition to MainMenu" );

			m_isExitToMainMenuRequested = false;
			return TransitionRequest::ReplaceAll( "MainMenu" );
		}

		if( m_gameSession && m_gameSession->GetGameStatus() >= game::GameStatus::Running && m_gameSession->HasGameInstance() && m_gameSession->GetGameInstance().GetNetSession() != nullptr )
		{
			const net::PropertyArray& properties = m_gameSession->GetGameInstance().GetNetSession()->GetProperties();

			if( properties.Find( game::SessionPropertyID::ReconnectRequested ) != nullptr )
			{
				if( red::MultiplayerSetup::IsBoothMode() )
				{
					RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transition to BoothModeMainMenu" );
					return TransitionRequest::ReplaceAll( "BoothModeMainMenu" );
				}
				else
				{
					Args args;
					args.Set( RED_NAME_CONSTEXPR( "inputDeviceID" ), m_inputDeviceID );
					args.Set( RED_NAME_CONSTEXPR( "gameSessionDesc" ), m_gameSessionDesc );

					RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transition to Session" );
					return TransitionRequest::ReplaceAll( "Session", args );
				}
			}
			else if ( m_gameSession->GetGameStatus() == game::GameStatus::ToDelete )
			{
				// Handle session closure..
				RED_LOG_INFO( "State_Session::ProcessTransition: Requesting transition to MainMenu" );
				return TransitionRequest::ReplaceAll( "MainMenu" );
			}
		}

		if( m_streamingTestFinished )
		{
			m_streamingTestFinished = false;
			Args args;
			args.Set( RED_NAME_CONSTEXPR( "targetUIState" ), ink::State::EndGameLoading );
			return TransitionRequest::ReplaceAll( "PreGameSession", args );
		}

		// stay where you are
		return TransitionRequest();
	}

	void State_Session::InitializeGameSession(
		const game::GameSessionDesc& gameSessionDesc,
		save::IStream* loadStream,
		const job::RunContext& context,
		red::Atomic< red::AsyncStatus >& result )
	{
		ShowLoadingScreen();
		m_gameSessionDesc = gameSessionDesc;
		m_gameSession->Initialize( context, result, m_gameSessionDesc, loadStream, m_processor, m_gameInstance, m_scene );
	}

	void State_Session::InitializeGameSession(
		const job::RunContext& context,
		red::Atomic< red::AsyncStatus >& result )
	{
		ShowLoadingScreen();
		m_gameSession->Initialize( context, result, m_gameSessionDesc, nullptr, m_processor, m_gameInstance, m_scene );
	}

	void State_Session::RestoreGameSession(
		const job::RunContext& context,
		Bool& result,
		save::IStream& stream )
	{
		m_gameSession->LoadGame( context, result, stream );
	}

	void State_Session::SetLoadedMetadata( const red::SharedPtr< save::Metadata >& metadata )
	{
		m_gameDefinition = metadata->m_gameDefinition;
	}

	Bool State_Session::DebugGetLoadingCooldownSeconds( Float& outSeconds ) const
	{
		if( m_debugIsLoadingCooldownTimerValid )
		{
			outSeconds = m_debugLoadingCooldownTimer.GetDeltaF();
			return true;
		}
		outSeconds = 0.f;
		return false;
	}

	void State_Session::DebugResetLoadingCooldown()
	{
		m_debugLoadingCooldownTimer.Reset();
		m_debugIsLoadingCooldownTimerValid = true;
	}

	Float State_Session::GetUpdateBehindLoadingScreenElapsedSeconds() const
	{
		return m_updateBehindLoadingScreenTimer.GetDeltaF();
	}

	void State_Session::ResetUpdateBehindLoadingScreenTimer()
	{
		m_updateBehindLoadingScreenTimer.Reset();
	}

	void State_Session::OnSessionPaused()
	{
		if ( auto soundSystem = GEngine->GetSoundSystem() )
		{
			soundSystem->OnGamePaused();
		}
		m_handler->OnSessionPaused();
	}

	void State_Session::OnSessionActive()
	{
		if ( auto soundSystem = GEngine->GetSoundSystem() )
		{
			soundSystem->OnGameActive();
		}
		m_handler->OnSessionActive();
	}

	void State_Session::SetupSystemHandler(THandle< BaseRequestsHandler >& handler)
	{
		handler = CreateHandle< InGameRequestsHandler >( GetEventSink() );
	}

	void State_Session::ProcessRender( const RenderContext& context, RenderControl& control )
	{
		State::ProcessRender( context, control );

		// process view
		m_gameSession->ProcessView( context );

		// render frame
		m_gameSession->RenderFrame( context.m_frame, context.m_cameraFrameInfo );

		// render additional debug
		RenderDebug( context.m_frame, context.m_cameraFrameInfo, context.m_tickInfo );
	}

	void State_Session::RenderDebug( CRenderFrameInfo& framInfo, CRenderCameraFrameInfo& cameraInfo, const tick::Info& tickInfo ) const
	{
		// render session debug
		m_gameSession->RenderDebug( framInfo, cameraInfo );

#ifdef USE_SESSION_DEBUG_HELPER
		m_debugHelper->RenderDebug( cameraInfo.GetDebugDrawer(), tickInfo );
#endif // USE_SESSION_DEBUG_HELPER

#ifndef RED_CONFIGURATION_FINAL
		rend::FragmentBuilder builder( cameraInfo.GetDebugDrawer() );
		if( cameraInfo.GetDebugDrawer().GetMask().Test( DF_RenderInput ) )
		{
			const char statsTxt[] =
				{
					"Keyboard Key States:\n"
					"   w: %s\n"
					"   s: %s\n"
					"   a: %s\n"
					"   d: %s\n"
					"Mouse Button States:\n"
					"   LMB %s\n"
					"   MMB %s\n"
					"   RMB %s\n"
					"   WheelDelta:\n"
					"       %f"};

			builder.AddFlatTextf( 1, 30, statsTxt,
								  g_keyStates[ IK_W ] ? "down" : "up",
								  g_keyStates[ IK_S ] ? "down" : "up",
								  g_keyStates[ IK_A ] ? "down" : "up",
								  g_keyStates[ IK_D ] ? "down" : "up",
								  g_keyStates[ IK_LeftMouse ] ? "down" : "up",
								  g_keyStates[ IK_MiddleMouse ] ? "down" : "up",
								  g_keyStates[ IK_RightMouse ] ? "down" : "up",
								  g_wheelDelta );
		}
#endif
	}

	const Bool State_Session::ProcessMappedInput( const MappedInput& input )
	{
		if( input.m_inputEvent == EVT_RequestExitGame )
		{
			GetEventSink().PostMessage( EVT_RequestExitGame, "" );
		}

#ifdef USE_SESSION_DEBUG_HELPER
		m_debugHelper->ProcessMappedInput( input );
#endif // USE_SESSION_DEBUG_HELPER

		return false;
	}

	const Bool State_Session::ProcessMessage( const ExternalMessage& msg, const StateMachine &gsm )
	{
		switch ( msg.m_name.GetHash() )
		{
			case EVT_AuthLost.GetHash():
				m_gameSession->HandleAuthLost();
				return true;

			case EVT_RequestExitGame.GetHash():
				m_isExitGameRequested = true;
				return true;

			case EVT_RequestExitSession.GetHash():
				m_isExitSessionRequested = true;
				return true;

			case EVT_RequestExitToMainMenu.GetHash():
				m_isExitToMainMenuRequested = true;
				return true;

			// #tbd: should really be triggered when in active state only
			case EVT_AutoSave.GetHash():
			case EVT_QuickSave.GetHash():
			case EVT_ManualSave.GetHash():
			case EVT_PointOfNoReturnSave.GetHash():
			case EVT_EndGameSave.GetHash():
			{
				Uint8 saveFlags = 0;
				red::String param = msg.m_params;
				if ( param.ContainsNoCase( "force" ) )
				{
					saveFlags |= game::SaveRequestFlag::Force;
				}
				if ( param.ContainsNoCase( "checkpoint" ) )
				{
					saveFlags |= game::SaveRequestFlag::Checkpoint;
				}
				param.Clear();

				red::String saveName;
				switch ( msg.m_name.GetHash() )
				{
					case EVT_AutoSave.GetHash():
						saveName = save::AUTOSAVE_NAME;
						break;

					case EVT_QuickSave.GetHash():
						saveName = save::QUICKSAVE_NAME;
						break;

					case EVT_ManualSave.GetHash():
						saveName = param.Empty() ? save::MANUALSAVE_NAME : param;
						break;

					case EVT_PointOfNoReturnSave.GetHash():
						saveName = save::POINT_OF_NO_RETURNSAVE_NAME;
						saveFlags |= game::SaveRequestFlag::PointOfNoReturn;
						break;

					case EVT_EndGameSave.GetHash():
						saveName = save::END_GAMESAVE_NAME;
						saveFlags |= game::SaveRequestFlag::EndGame;
						break;
				}

				RequestSaving( std::move( saveName ), saveFlags );
				return true;
			}

			default:
				return false;
		}
	}

	void State_Session::ProcessServices( services::GameServices& services )
	{
		if( m_handler )
		{
			m_handler->ProcessServices( services, *m_gameInstance );
		}

		ProcessSaving( services );

#if defined( RED_SAVE_SERVER_ENABLED )
		red::SaveServer::GetInstance().ProcessServices( services );
#endif
	}

	void State_Session::RequestSaving( red::String&& name, Uint8 flags )
	{
		if( flags & game::SaveRequestFlag::Periodic )
		{
			flags = game::SaveRequestFlag::Periodic;
		}

		if( flags & game::SaveRequestFlag::PointOfNoReturn )
		{
			flags |= game::SaveRequestFlag::Force;
		}

		if( flags & game::SaveRequestFlag::EndGame )
		{
			flags |= game::SaveRequestFlag::Force;
		}

		m_savingRequest.RequestSaving( std::move( name ), flags );
	}

	void State_Session::ProcessSaving( services::GameServices& services )
	{
		if( m_isExitGameRequested || m_isExitSessionRequested || m_isExitToMainMenuRequested || m_gameSaveInProgress )
		{
			return;
		}

		const auto shouldAutoSave = m_gameSession->ShouldAutoSaveGameInstance();
 		if( shouldAutoSave.yes )
		{
			save::EnumFlags< game::SaveRequestFlag, Uint8 > flags{ shouldAutoSave.flags };
			const bool force = flags.Has( game::SaveRequestFlag::Force );
			const bool pointOfNoReturn = flags.Has( game::SaveRequestFlag::PointOfNoReturn );
			const bool endGame = flags.Has( game::SaveRequestFlag::EndGame );

			// NOTE check against last successful save time
			if ( !force && ( m_saveLastSuccessTime > 0 ) )
			{
				const float timeSinceSuccessfulSave = EngineTime::GetNow() - m_saveLastSuccessTime;
				if( timeSinceSuccessfulSave < static_cast< float >( Config::cvCheckpointSavePeriod.Get() ) )
				{
					m_gameSession->OnSaveGameFailed();
					return;
				}
			}

			red::String saveName;
			if( pointOfNoReturn )
			{
				saveName = save::POINT_OF_NO_RETURNSAVE_NAME;
			}
			else if( endGame )
			{
				saveName = save::END_GAMESAVE_NAME;
			}
			else
			{
				saveName = save::AUTOSAVE_NAME;
			}

			RequestSaving( std::move( saveName ), shouldAutoSave.flags ); // periodic auto-saves can not be forced
		}

		if( m_savingRequest.m_operation != save::Request::Operation::Save && !m_handler->GetSaveRequest( m_savingRequest ) )
		{
			return; // no request was made
		}

		// CP2077 web [saves]: sandbox pages never write saves (see top of file).
		if( cpweb_session_saves::IsSandboxNoSavePage() )
		{
			static std::atomic< Uint32 > s_refused{ 0 };
			const Uint32 refused = s_refused.fetch_add( 1 ) + 1;
			if( refused <= 4 || ( refused & ( refused - 1 ) ) == 0 )
			{
				std::fprintf( stderr, "CP2077 saves: sandbox page, save request refused (#%u)\n", refused );
			}
			red::DynArray< game::SaveLock > noLocks{ red::PoolFrame() };
			m_savingRequest.RequestNothing();
			if( m_handler )
			{
				m_handler->OnSavingComplete( false, noLocks );
			}
			m_gameSession->OnSaveGameFailed();
			return;
		}

		const WeakHandle< game::ITelemetrySystem > telemetrySys = m_gameSession->GetGameInstance().GetSystemPtr< game::ITelemetrySystem >();
		if( m_savingRequest.m_metadata->m_isPointOfNoReturn )
		{
			if( const auto telemetrySysStrong = telemetrySys.ToHandle() )
			{
				telemetrySysStrong->CreatePointOfNoReturnId();
			}
		}

		if( !ProcessSaveLocks() )
		{
			// saving was requested, but we have a lock that can't be ignored
			m_gameSession->OnSaveGameFailed();
			return;
		}

		if ( !m_gameSaveListRequested )
		{
			services.RequestSaves( []( const services::GameSaveViews& saves, services::ServiceError errorCode ) {} );
			m_gameSaveListRequested = true;
			return;
		}

#ifdef USE_SESSION_DEBUG_HELPER
		m_debugHelper->m_debugMessage.m_text = "Started Saving Game..";
		m_debugHelper->m_debugMessage.m_timer = DebugMessage::DURATION;
		m_debugHelper->m_debugMessage.m_color = Color::GREEN();
#endif // USE_SESSION_DEBUG_HELPER

		m_savingRequest.m_metadata->m_gameDefinition = m_gameDefinition;

		// TODO: custom UI for saves?

		auto onSaveStarted = [ptr = WeakHandleFromThis(), telemetrySys ]( const red::StringView saveName, save::IStream& stream, save::Metadata& metadata, red::Atomic< Bool >& isFinished, services::ServiceError& errorCode )
		{
			const auto logSaveFailureToTelemetry = [ & ]()
			{
				if( const auto telemetrySystemStrong = telemetrySys.ToHandle() )
				{
					telemetrySystemStrong->LogSaveLowLevelFailure( saveName.ToString(), errorCode );
				}
			};
			
			if ( errorCode != services::ServiceError_OK )
			{
				logSaveFailureToTelemetry();

				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_GameStateMachine, "State_Session::ProcessSaving: Save stream could not be prepared" );
				isFinished.SetValue( true );
				return;
			}

			if ( !stream.IsOK() )
			{
				errorCode = services::ServiceError_FailedToLoadSave;
				logSaveFailureToTelemetry();
				isFinished.SetValue( true );
				return;
			}

			auto self = Cast< State_Session >( ptr.ToHandle() );
			if( !self )
			{
				errorCode = services::ServiceError_InternalError;
				logSaveFailureToTelemetry();
				isFinished.SetValue( true );
				return;
			}

			self->m_gameSession->SaveGame( stream, metadata, self->m_gameSessionDesc, isFinished );
		};

		auto onSaveFinished = [ptr = WeakHandleFromThis(), telemetrySys ]( const services::GameSaveView& saveView, const services::ServiceError errorCode )
		{
			const auto isSuccess = errorCode == services::ServiceError_OK;
			if( !isSuccess )
			{
				if( const auto telemetrySysStrong = telemetrySys.ToHandle() )
				{
					telemetrySysStrong->LogSaveLowLevelFailure( saveView.name, errorCode );
				}
			}

			if ( auto self = Cast< State_Session >( ptr.ToHandle() ) )
			{
#ifdef USE_SESSION_DEBUG_HELPER
				auto& debugHelper = self->m_debugHelper;
				switch ( errorCode )
				{
				case services::ServiceError_OK:
					debugHelper->m_debugMessage.m_text = String::Printf( "%s: Game SAVED.", saveView.name.AsChar() );
					debugHelper->m_debugMessage.m_timer = DebugMessage::DURATION;
					debugHelper->m_debugMessage.m_color = Color::GREEN();
					RED_LOG_CATEGORY_INFO( red::LoggerCategory_GameStateMachine, "State_Session::ProcessSaving(): %s", debugHelper->m_debugMessage.m_text.AsChar() );
					break;

				default:
					debugHelper->m_debugMessage.m_timer = DebugMessage::DURATION;
					debugHelper->m_debugMessage.m_color = Color::RED();

					switch ( errorCode )
					{
					case services::ServiceError_InvalidUser:
						debugHelper->m_debugMessage.m_text = String::Printf( "Game NOT SAVED, invalid user" );
						break;

					case services::ServiceError_NotEnoughSlots:
						debugHelper->m_debugMessage.m_text = String::Printf( "Game NOT SAVED, could not find free save slot" );
						break;

					case services::ServiceError_NotEnoughSpace:
						debugHelper->m_debugMessage.m_text = String::Printf( "Game NOT SAVED, not enough space to store save" );
						break;

					case services::ServiceError_InvalidSaveID:
						debugHelper->m_debugMessage.m_text = String::Printf( "Game NOT SAVED, invalid save id" );
						break;

					case services::ServiceError_InternalError:
					default:
						debugHelper->m_debugMessage.m_text = String::Printf( "Game NOT SAVED, game services internal error. Please refer to the log for more details." );
						break;
					}

					RED_LOG_CATEGORY_ERROR( red::LoggerCategory_GameStateMachine, "State_Session::ProcessSaving(): %s", debugHelper->m_debugMessage.m_text.AsChar() );
					break;
				}
#endif // USE_SESSION_DEBUG_HELPER

				if ( !isSuccess )
				{
					self->m_gameSession->OnSaveGameFailed();
				}

				if (self->m_handler)
				{
					red::DynArray< game::SaveLock > locks{ red::PoolFrame() };
					switch ( errorCode )
					{
					case services::ServiceError_NotEnoughSpace:
						locks.PushBack( game::SaveLock{ game::SaveLock::Reason::NotEnoughSpace, red::String::EMPTY() } );
						break;
					case services::ServiceError_NotEnoughSlots:
						locks.PushBack( game::SaveLock{ game::SaveLock::Reason::NotEnoughSlots, red::String::EMPTY() } );
						break;
					}

					self->m_handler->OnSavingComplete( isSuccess, locks );
				}

				self->SetGameSaveInProgressEnd( isSuccess );
			}
		};

		auto metadata = m_savingRequest.m_metadata;

		SetGameSaveInProgressBegin();

		services.Save( metadata, std::move( onSaveStarted ), std::move( onSaveFinished ) );
	}

	Bool State_Session::ProcessSaveLocks()
	{
		const save::EnumFlags< game::SaveRequestFlag, Uint8 > flags{ m_savingRequest.m_flags };
		const bool isPeriodic = flags.Has( game::SaveRequestFlag::Periodic );
		const bool isCheckpoint = flags.Has( game::SaveRequestFlag::Checkpoint );
		const bool isForced = flags.Has( game::SaveRequestFlag::Force );

		// Forced checkpoints (from quest checkpoint node) no longer break through locks, but they
		// will not be subject to the CheckpointSavePeriod cooldown.
		// PonR and End Game save have isCheckpoint flag disabled
		if( isForced && !isCheckpoint )
		{
			return true;
		}

		red::DynArray< game::SaveLock > locks{ red::PoolFrame() };
		m_gameSession->IsSavingLocked( locks, isForced );
		if( locks.Empty() )
		{
			return true;
		}

#ifdef USE_SESSION_DEBUG_HELPER
		red::StringBuffer< 4096 > debugString;
		if ( isPeriodic )
		{
			debugString += "Periodic AutoSave requested, but locks are present. Postponing.\n\n";
			for( const auto& lock : locks )
			{
				debugString += lock.debugString;
				debugString += "\n";
			}
			m_debugHelper->m_debugMessage.m_text = debugString.AsChar();
			m_debugHelper->m_debugMessage.m_color = Color::YELLOW();
			m_debugHelper->m_debugMessage.m_scale = 1.5f;
		}
		else
		{
			debugString += "Can't save now.\n\n";
			for( const auto& lock : locks )
			{
				debugString += lock.debugString;
				debugString += "\n";
			}
			debugString += "\nUse force flag to ignore the lock.";

			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_GameStateMachine, "State_Session::ProcessSaving(): %s", debugString.AsChar() );
			m_debugHelper->m_debugMessage.m_text = debugString.AsChar();
			m_debugHelper->m_debugMessage.m_color = Color::ORANGE();
		}
		m_debugHelper->m_debugMessage.m_timer = DebugMessage::EXTRA_DURATION;
#endif // USE_SESSION_DEBUG_HELPER

		if( flags.Has( game::SaveRequestFlag::PointOfNoReturn ) )
		{
			if( const auto telemetrySys = m_gameSession->GetGameInstance().GetSystemPtr< game::ITelemetrySystem >() )
			{
				red::String errorMsg;
				for( const auto& lock : locks )
				{
					errorMsg += lock.debugString + "|";
				}

				const red::String& saveName = m_savingRequest.m_metadata ? m_savingRequest.m_metadata->GetName() : save::POINT_OF_NO_RETURNSAVE_NAME;
				telemetrySys->LogSavePerformed( saveName, errorMsg );
			}
		}


		m_savingRequest.RequestNothing();

		if( m_handler )
		{
			m_handler->OnSavingComplete( false, locks );
		}

		return false;
	}

	void State_Session::ShowLoadingScreen()
	{
		if ( auto loadingScreen = GetInkSystem().GetLoadingScreen() )
		{
			loadingScreen->SetGameSessionLoadingFinished( false );
			loadingScreen->SetLoadingState( ink::ILoadingScreen::State::Loading );

			GetInkSystem().PlaySound( RED_NAME_CONSTEXPR( "ST_Loading_Screen_On_Set_State" ) );

			if ( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
			{
				viewportWrapper->GetStateMachine()->RequestNewState( RED_NAME_CONSTEXPR( "inkInitEngineState" ) );
			}

#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
			GEngine->PauseScreenshotCapture();
#endif
		}
	}

	void State_Session::SetGameSaveInProgressBegin()
	{
		m_gameSaveInProgress = true;

		m_discOperationIdicator = GetInkSystem().ShowSystemNotification( DiscOperationIndicator );
		m_savingRequest.RequestNothing();

		io::GAsyncIO.EnableSavingMode( true );
	}

	void State_Session::SetGameSaveInProgressEnd( const bool isSuccess )
	{
		io::GAsyncIO.EnableSavingMode( false );

		if( isSuccess )
		{
			m_saveLastSuccessFlags = m_savingRequest.m_flags;
			m_saveLastSuccessTime = EngineTime::GetNow();
		}

		m_savingRequest.RequestNothing();
		m_discOperationIdicator = nullptr;

		m_gameSaveInProgress = false;
	}

	void State_Session::ProcessInput(input::IInputContext& context)
	{
		State::ProcessInput( context );

		if( !context.IsCursorForced() && context.IsCursorVisible() )
		{
			context.HideCursor( RED_NAME_CONSTEXPR( "gameState_Session" ) );
		}

		for( auto& evt : context.GetBufferedInputData() )
		{
			//@todo [william.mcvicar] get the device id correctly
			m_inputDeviceID = evt.m_deviceID;
			break;
		}

#ifndef RED_CONFIGURATION_FINAL
		for( auto& evt : context.GetBufferedInputData() )
		{
			if( evt.m_type == input::BufferedInputEvent::Type::Raw )
			{
				if( evt.m_raw.m_action == IACT_Press )
					g_keyStates[ evt.m_raw.m_key ] = true;
				else if( evt.m_raw.m_action == IACT_Release )
					g_keyStates[ evt.m_raw.m_key ] = false;
				if( evt.m_raw.m_key == IK_MouseZ )
					g_wheelDelta = evt.m_raw.m_data;
			}
		}
#endif

		context.FlushConsumedEvents();
	}

	TweakDBID State_Session::GetDefaultPlayerCharacterRecordId() const
	{
		return IsMultiplayer()
			? TDBID( "Character.Cpo_Muppet_Default" )
			: TDBID( "Character.Player_Puppet_Base" );
	}

	Bool State_Session::IsMultiplayer() const
	{
		const world::RuntimeMode runtimeMode = m_gameSessionDesc.GetSessionConfig().m_runtimeMode;
		return runtimeMode == world::RuntimeMode::Multiplayer_Client ||
			runtimeMode == world::RuntimeMode::Multiplayer_Server;
	}

} // namespace gsm
