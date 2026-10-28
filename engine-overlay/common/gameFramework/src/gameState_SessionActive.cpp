#include <cstdio>
#include <atomic>
extern std::atomic< int > g_cpwebMissionflowMenuReady; // [missionflow] defined in preGameRequestsHandler.cpp
/**
 * Copyright (c) 2017-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"

#include "../../../common/world/include/worldRuntimeScene.h"
#include "../../../common/gameServices/include/gameServices.h"
#include "../../../common/redMemory/include/memoryAnalyzerUtils.h"
#include "../../../common/inkCore/include/inkSystem.h"
#include "../../../common/inkCore/include/menuLayerEvents.h"
#include "../../../common/telemetry/include/telemetryHeaders.h"

#include "../game/include/gogRewardsSystem.h"

#include "gameState_Session.h"
#include "gameState_SessionActive.h"
#include "gameState_RequestEvents.h"
#include "inputMapping.h"
#include "gameStateErrors.h"

//#define ENABLE_SESSION_ACTIVE_DEBUG_TELEPORT_CHECK
#ifdef ENABLE_SESSION_ACTIVE_DEBUG_TELEPORT_CHECK
# include "../../../common/gameCore/include/gameObject.h"
# include "../../../common/gameCore/include/playerSystemInterface.h"
# include "../../../common/game/include/stateMachineEntityEvents.h"
#endif
#include "../redCore/include/commandline.h"
#include "../input/include/inputSystemMessages.h"

#include "../inkCore/include/stateMachine.h"
#include "../inkCore/include/stateMachine_State.h"
#include "../inkCore/include/systemNotificationsTypes.h"
#include "../sound/include/scriptAudioInterface.h"
#include "../sound/include/soundSystem.h"
#include "../engine/include/baseEngine.h"

#include "../gameServices/include/gameLocalStorage.h"
#include "../gameServices/include/gameServicesUser.h"

#include "../inkCore/include/systemNotificationsUtilities.h"
#include "../inkCore/include/menuLayer.h"
#include "../inkCore/include/menuInstanceInterface.h"
#include "../localization/include/localizationManager.h"
#include "../redConfig/include/inGameConfigSystem.h"


RTTI_BEGIN_TYPE_IN_NAMESPACE( State_SessionActive, gsm )
	RTTI_PARENT_TYPE( gsm::State_SessionStreamingAware )
RTTI_END_TYPE()

namespace
{
	red::Timer s_startUpTimer;

	Bool s_engagementScreenDone = false;

	// How many seconds minimum should the initialization phase last for. This is done so we don't just blink the
	// stage if everything can be done quickly and user can read all stuff.
	const Float c_minInitializationStageDuration = 2.0f;
	const Float c_maxTimeInInitializePhase = 5.0f;
}

namespace gsm
{

const CName State_SessionActive::EVT_GamePaused = RED_NAME_CONSTEXPR( "GamePaused" );
const CName State_SessionActive::EVT_GamePauseMenu = RED_NAME_CONSTEXPR( "GamePauseMenu" );
const CName State_SessionActive::EVT_Start = RED_NAME_CONSTEXPR( "Start" );
const CName State_SessionActive::EVT_Pause = RED_NAME_CONSTEXPR( "Pause" );
const CName State_SessionActive::EVT_PauseBackground = RED_NAME_CONSTEXPR( "PauseBackground" );
const CName State_SessionActive::DEBUG_EVT_TeleportLoadingCheck = RED_NAME_CONSTEXPR( "DEBUG_TeleportLoadingCheck" );
const CName State_SessionActive::DEBUG_EVT_StartExternalNetworkProfiler = RED_NAME_CONSTEXPR( "DEBUG_StartExternalNetworkProfiler" );

enum SessionActive_TransitionFlags : Uint32
{
	SAF_PauseGameMenu = RED_FLAG( 0 ),
	SAF_Loading = RED_FLAG( 1 ),
	SAF_GamePaused = RED_FLAG( 2 ),
	SAF_ReconnectController = RED_FLAG( 3 )
};

State_SessionActive::State_SessionActive()
	: State_SessionStreamingAware( "SessionActive" )
	, m_activeDeviceID( 0 )
	, m_disconnectedID( 0 )
	, m_transitionRequestFlags( 0 )
	, m_effectTimer( 0.0f )
	, m_pressFadeout( 1.0f )
	, m_state( InternalState::Invalid )
#if defined( RED_PLATFORM_ORBIS )
	, m_systemVOinstalled{}
#endif
	, m_isSingleplayerGame( false )
	, m_isPreGame( false )
	, m_uiStateTransitionRequested( false )
{
}

extern "C" void cpweb_SaveLoadWatchDisarm(); // CP2077 web [saves]: packageReadStream.cpp

Bool State_SessionActive::OnProcessInit( ILazyFunctionScheduler& lazyScheduler, const ArgsView& args, ParamsView& params )
{
	// CP2077 web [saves]: the session is running; stop failing loads on package read overruns (gameState_Session.cpp).
	cpweb_SaveLoadWatchDisarm();

#ifndef RED_CONFIGURATION_FINAL
	ink::DEBUG_PushVisitedGameStateForIMGUI( "State_SessionActive" );
#endif

	RED_MEMORY_MA_WRITE_BOOKMARK( "Session Active", "Started running the game" );

	SetTargetUIState( ParseTargetUIState( args ) );
 	SetErrorCode( ParseErrorCode( args ) );
 	SetErrorFile( ParseErrorFile( args ) );

	auto targetUIState = GetTargetUIState();
	if ( targetUIState == ink::State::PreGameMenu || targetUIState == ink::State::EngagementScreen )
	{
		m_isPreGame = true;

		if( params.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "skipStartScreen" ) ) || args.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "skipStartScreen" ) ) )
		{
			s_engagementScreenDone = true;
		}

		if ( GetErrorCode() != StateError::StateError_OK )
		{
			HandleErrorNotification();
		}

		if ( args.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "forceStartScreen" ) ) )
		{
			s_engagementScreenDone = false;
		}

#if defined( RED_PLATFORM_ORBIS )
		if ( GLocalizationManager->HasSystemVoMissing() )
		{
			InGameConfig::System &system = InGameConfig::System::GetInstance();
			m_systemVOinstalled = system.RegisterNotifier( [ this ]( const InGameConfig::NotificationType type )
			{
				HandleConfigNotification( type );
			} );
		}
#endif

		if ( s_engagementScreenDone )
		{
			HideEngagementScreen();
		}
		else
		{
			ShowEngagementScreen();
		}
	}

	if ( m_isPreGame )
	{
		if ( audio::SoundSystem* soundSystem = GEngine->GetSoundSystem() )
		{
			soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "mus_game_menus_00_title_screen_START" ) );
			soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "mus_game_menus_01_main_menu" ) );
			soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "ui_main_menu_loop_start" ) );
		}
	}

	SendUIState();

	return State_SessionStreamingAware::OnProcessInit( lazyScheduler, args, params );
}

void State_SessionActive::OnProcessShutdown( ILazyFunctionScheduler& lazyScheduler )
{
	if ( m_isPreGame )
	{
		if ( audio::SoundSystem* soundSystem = GEngine->GetSoundSystem() )
		{
			soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "mus_game_menus_03_STOP" ) );
			soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "ui_main_menu_loop_stop" ) );
		}

#if defined( RED_PLATFORM_ORBIS )
		InGameConfig::System &system = InGameConfig::System::GetInstance();
		system.UnregisterNotifier( m_systemVOinstalled );
#endif
	}

	State_SessionStreamingAware::OnProcessShutdown( lazyScheduler );
	s_engagementScreenDone = false;
}

void State_SessionActive::ProcessTick( const TickContext& context, StateTickFlagsContext& flags, TickControl& control )
{
 static unsigned lastState=~0u,lastUser=~0u,lastUI=~0u,checks=0;
 static int lastActiveUI=-100;
 const unsigned state=unsigned(m_state),user=unsigned(m_initializeUserState),ui=unsigned(GetTargetUIState());
 const int activeUI=int(GetActiveUIState());
 ++checks;
 if(state!=lastState || user!=lastUser || ui!=lastUI || activeUI!=lastActiveUI || (checks<=4096 && !(checks&(checks-1))) || (checks%4096)==0){
   std::fprintf(stderr,"CP2077 session menu gate probe: check=%u state=%u user=%u targetUI=%u activeUI=%d elapsed=%.3f engagementDone=%u\n",checks,state,user,ui,activeUI,double(m_initializePhaseDuration),unsigned(s_engagementScreenDone));
   lastState=state;lastUser=user;lastUI=ui;lastActiveUI=activeUI;
 }
 // [missionflow] main menu (PreGame session, UI state 3 = the gate cp2077-jump.js waits for) is up: the
 // PreGameRequestsHandler overlay may now start the direct ?start=nomad new game.
 { if ( m_isPreGame && activeUI == 3 ) g_cpwebMissionflowMenuReady.store( 1 ); }

	if( m_initializeUserState == InitializeUserState::GOGRewardsSystem )
	{
		if( THandle<game::GOGRewardsSystem> gogRewardsSystem = game::GOGRewardsSystem::GetActiveInstance() )
		{
			const Bool isGogRewardSystemDone = ( !gogRewardsSystem->IsBusy() ) && ( gogRewardsSystem->GetStatus() >= game::GOGRewardsSystemStatus::RegistrationPending );
			if( isGogRewardSystemDone || IsInitializationTakingTooLong() )
			{
				AdvanceInitializeUserState();
			}
		}
	}
	else if( m_initializeUserState == InitializeUserState::DistrubutionCheck )
	{
		if( IsInitializationTakingTooLong() )
		{
			// Abort non-critical game service request for distribution check.
			AdvanceInitializeUserState();
		}
	}

	switch( m_state )
	{
		case InternalState::StopVideo:
		{
			// This is a hack to workaround bug with "frozen" engagement screen caused by multiple ink transitions at the same frame
			if ( m_uiStateTransitionRequested )
			{
				m_uiStateTransitionRequested = false;
				break;
			}

			m_state = InternalState::InitializeUser;
			s_engagementScreenDone = true;

			UnmapEngagementScreenKeys();

			StartInitializeUser();
			SetTargetUIState( ink::State::InitializeUserScreen );
			SendUIState();
			break;
		}

		case InternalState::InitializeUser:
		case InternalState::InitializedUser:
		{
			m_initializePhaseDuration += context.m_timeDelta;
			if( m_state == InternalState::InitializedUser )
			{
				if( m_initializePhaseDuration >= c_minInitializationStageDuration )
				{
					m_state = InternalState::Active;

					// Force handle user errors before switching menu to show them before errors handled by script.
					HandleErrors();

#if defined( RED_PLATFORM_ORBIS )
					HandleSystemVoInstalling();
#endif

					SetTargetUIState( ink::State::PreGameMenu );
					SendUIState();
				}
			}
			break;
		}

		case InternalState::Active:
		{
			HandleErrors();

			break;
		}
	}

	State_SessionStreamingAware::ProcessTick( context, flags, control );
}

void State_SessionActive::StartInitializeUser()
{
	m_initializePhaseDuration = 0.0f;
	m_initializeUserState = InitializeUserState::None;
	AdvanceInitializeUserState();
}

void State_SessionActive::AdvanceInitializeUserState()
{
	switch( m_initializeUserState )
	{
	case InitializeUserState::None:
		m_initializeUserState = InitializeUserState::LocalStorage;
		services::GGameLocalStorage->RequestLoad( [ this ]( const services::GameLocalStorageLoadResult result )
		{
			const Bool success = result == services::GameLocalStorageLoadResult::Success;
			OnUserLocalStorageLoaded( success );
		} );
		break;
	case InitializeUserState::LocalStorage:
		if( THandle<game::GOGRewardsSystem> gogRewardsSystem = game::GOGRewardsSystem::GetActiveInstance() )
		{
			m_initializeUserState = InitializeUserState::GOGRewardsSystem;
			gogRewardsSystem->RequestInitialStatus();
		}
		else
		{
			// GOG rewards are optional for now, if user started game without it we just completed the setup.
			m_initializeUserState = InitializeUserState::DistrubutionCheck;
		}
		break;
	case InitializeUserState::GOGRewardsSystem:
		m_initializeUserState = InitializeUserState::DistrubutionCheck;
#if defined( RED_DISTRIBUTION_CHECK_ENABLED )
		telemetry::headers::SetBuildDistribution( services::GetDistributionTypeString( services::DistributionType::Unknown ) );
		GGameServices->CheckDistribution( [ this ]( services::DistributionType distributionType, services::ServiceError errorCode )
		{
			if( m_initializeUserState == InitializeUserState::DistrubutionCheck )
			{
				telemetry::headers::SetBuildDistribution( services::GetDistributionTypeString( distributionType ) );
				RED_LOG_INFO( "GameServices::CheckDistributionRequest: Completed check on time" );
				AdvanceInitializeUserState();
			}
			else
			{
				RED_LOG_WARNING( "GameServices::CheckDistributionRequest: Timed out before check could complete" );
			}
		} );
#else
		AdvanceInitializeUserState();
#endif
		break;
	case InitializeUserState::DistrubutionCheck:
		UserInitializationFinished();
		break;
	}
}

#if defined( RED_PLATFORM_ORBIS )
void State_SessionActive::HandleSystemVoInstalling()
{
	if ( !GLocalizationManager->HasSystemVoMissing() )
	{
		return;
	}

	m_systemVOToken = RequestSystemNotificationGeneric(
		true, CName::NONE(),
		RED_NAME_CONSTEXPR( "UI-SystemNotification-PlayGoDataSystemVoicePackInstalling" ) );

	if ( m_systemVOToken )
	{
		red::FixedSizeFunction< void() > accept = [ weakSelf = WeakHandleFromThis<State_SessionActive>() ]()
		{
			auto handleSelf = weakSelf.ToHandle();
			if ( !handleSelf )
			{
				return;
			}

			handleSelf->m_systemVOToken = nullptr;
		};
		m_systemVOToken->SetAcceptCallback( accept );
	}
}

void State_SessionActive::HandleSystemVoInstalled()
{
	m_systemVOToken = RequestSystemNotificationGenericYesNo(
		CName::NONE(),
		RED_NAME_CONSTEXPR( "UI-SystemNotification-PlayGoDataSystemVoicePackInstalled" ) );
	if ( m_systemVOToken )
	{
		red::FixedSizeFunction< void() > accept = [ weakSelf = WeakHandleFromThis<State_SessionActive>() ]()
		{
			auto handleSelf = weakSelf.ToHandle();
			if ( !handleSelf )
			{
				return;
			}

			GLocalizationManager->SetSystemVoMissing();

			handleSelf->m_systemVOToken = nullptr;
		};
		m_systemVOToken->SetAcceptCallback( accept );

		red::FixedSizeFunction< void() > cancel = [ weakSelf = WeakHandleFromThis<State_SessionActive>() ]()
		{
			auto handleSelf = weakSelf.ToHandle();
			if ( !handleSelf )
			{
				return;
			}

			GLocalizationManager->ResetSystemVoMissing();

			handleSelf->m_systemVOToken = nullptr;
		};
		m_systemVOToken->SetCancelCallback( cancel );
	}
}

void State_SessionActive::HandleConfigNotification( InGameConfig::NotificationType type )
{
	if ( type != InGameConfig::NotificationType::LanguagePackInstalled )
	{
		return;
	}

	HandleSystemVoInstalled();
}
#endif

void State_SessionActive::UserInitializationFinished()
{
	m_state = InternalState::InitializedUser;
	m_initializeUserState = InitializeUserState::Complete;
	GGameServices->DispatchUserSignInCallback();
}

Bool State_SessionActive::IsInitializationTakingTooLong() const
{
	return m_initializePhaseDuration > c_maxTimeInInitializePhase;
}

void State_SessionActive::OnUserLocalStorageLoaded( const Bool success )
{
	AdvanceInitializeUserState();
	// #TODO: Add error handling.
	RED_UNUSED( success );
}

void State_SessionActive::HideEngagementScreen()
{
	UnmapEngagementScreenKeys();
	m_state = InternalState::Active;
}

void State_SessionActive::ShowEngagementScreen()
{
	MapEngagementScreenKeys();
	SetTargetUIState( ink::State::EngagementScreen );

	m_state = InternalState::WaitingForButtonPress;
}

ESystemNotificationTypes GetNotificationType( const StateError errorCode )
{
	switch ( errorCode )
	{
	case StateError::StateError_CantLoadSave_CantCreateLoadStream:
	case StateError::StateError_CantLoadSave_CantLoadFile:
	//case StateError::StateError_CantLoadSave_CensorshipLevelMismatch: this one has its own localization, to be more accurate
	case StateError::StateError_CantLoadSave_Corrupted:
	case StateError::StateError_CantLoadSave_SessionDescInvalid:
	//case StateError::StateError_CantLoadSave_VersionMismatch:			this one too
		return ESystemNotificationTypes::CorruptedSaveFile;
	default:
		return ESystemNotificationTypes::GenericMenuError;
	}
}

void State_SessionActive::HandleErrors()
{
	HandleUserSettingsErrors();

	HandleUserProfileErrors();
}

void State_SessionActive::HandleUserSettingsErrors()
{
	using namespace InGameConfig;
	System& system = System::GetInstance();

	// Check settings load errors.
	if( !system.GetUserSettingsLoadStatusChecked() )
	{
		UserSettingsLoadStatus status = System::GetInstance().GetUserSettingsLoadStatus();
		if( status != UserSettingsLoadStatus::FileIsCorrupted && status != UserSettingsLoadStatus::InternalError )
		{
			system.SetUserSettingsLoadStatusChecked();
		}
		else if( !m_token )
		{
			SetErrorCode( StateError::StateError_SettingsCorrupted );
			HandleErrorNotification();
			ClearError();

			system.SetUserSettingsLoadStatusChecked();
		}
	}

	// Check settings save errors.
	if( !system.GetUserSettingsSaveStatusChecked() )
	{
		UserSettingsSaveStatus status = system.GetUserSettingsSaveStatus();
		if( status != UserSettingsSaveStatus::InternalError )
		{
			system.SetUserSettingsSaveStatusChecked();
		}
		else if( !m_token )
		{
			SetErrorCode( StateError::StateError_SettingsCorrupted_Save );
			HandleErrorNotification();
			ClearError();

			system.SetUserSettingsSaveStatusChecked();
		}
	}
}

void State_SessionActive::HandleUserProfileErrors()
{
	// Check profile loading errors.
	if( !services::GGameLocalStorage->GetLastLoadResultChecked() )
	{
		services::GameLocalStorageLoadResult result = services::GGameLocalStorage->GetLastLoadResult();
		if( result != services::GameLocalStorageLoadResult::Failed )
		{
			services::GGameLocalStorage->SetLastLoadResultChecked();
		}
		else if( !m_token )
		{
			SetErrorCode( StateError::StateError_ProfileCorrupted );
			HandleErrorNotification();
			ClearError();

			services::GGameLocalStorage->SetLastLoadResultChecked();
		}
	}

	// Check profile saving errors.
	if( !services::GGameLocalStorage->GetLastSaveResultChecked() )
	{
		services::GameLocalStorageSaveResult result = services::GGameLocalStorage->GetLastSaveResult();
		if( result != services::GameLocalStorageSaveResult::Failed )
		{
			services::GGameLocalStorage->SetLastSaveResultChecked();
		}
		else if( !m_token )
		{
			SetErrorCode( StateError::StateError_ProfileCorrupted_Save );
			HandleErrorNotification();
			ClearError();

			services::GGameLocalStorage->SetLastSaveResultChecked();
		}
	}
}

void State_SessionActive::HandleErrorNotification()
{
	const StateError errorCode = GetErrorCode();
	if ( errorCode == StateError::StateError_OK )
	{
		return;
	}

	if ( !m_token )
	{
		ESystemNotificationTypes notificationType = GetNotificationType( errorCode );

		static const CName s_prefix = RED_NAME_CONSTEXPR( "UI-SystemNotification-" );
		static const CName s_fallbackMessage = RED_NAME_CONSTEXPR( "UI-SystemNotification-StateError_Generic" );
		static const CName s_fallbackTitle = RED_NAME_CONSTEXPR( "UI-SystemNotification-StateError_Generic_Title" );

		const red::DynArray< CName >& options = GetRttiSystem().FindEnum( RED_NAME_CONSTEXPR_NOREG( "gsmStateError" ) )->GetOptions();
		RED_ASSERT( options.Size() == (Uint32)StateError::MAX, "Some options weren't registered in RTTI, this will lead to wrong messages being matched" );
		const AnsiChar* errorName = options[ (Int32)errorCode ].AsChar();

		CName errorMessage = RED_APPEND_NAME( s_prefix, errorName );
		CName errorTitle;
		if ( GLocalizationManager->GetOnScreenString( errorMessage ).Empty() )
		{
			errorTitle = s_fallbackTitle;
			errorMessage = s_fallbackMessage;
		}
		else
		{
			errorTitle = RED_APPEND_NAME( errorMessage, "_Title" );
			if ( GLocalizationManager->GetOnScreenString( errorTitle ).Empty() )
			{
				errorTitle = s_fallbackTitle;
			}
		}

		m_token = GetInkSystem().ShowSystemNotification( notificationType, errorTitle, errorMessage, nullptr, GetErrorFile() );
		if ( m_token )
		{
			red::FixedSizeFunction< void() > accept = [ weakSelf = WeakHandleFromThis<State_SessionActive>() ]()
			{
				auto handleSelf = weakSelf.ToHandle();
				if ( !handleSelf )
				{
					return;
				}

				handleSelf->m_token = nullptr;
			};
			m_token->SetAcceptCallback( accept );

			red::FixedSizeFunction< void() > cancel = [ weakSelf = WeakHandleFromThis<State_SessionActive>() ]()
			{
				auto handleSelf = weakSelf.ToHandle();
				if ( !handleSelf )
				{
					return;
				}

				handleSelf->m_token = nullptr;
			};
			m_token->SetCancelCallback( cancel );
		}
	}
}

void State_SessionActive::InitializeInputMapping( InputMappingTable& mappingTable, const ArgsView& args ) const
{
#ifndef RED_CONFIGURATION_FINAL
	mappingTable.MapKeyPress( IK_PageUp, EVT_GamePaused );
	mappingTable.MapKeyPress( IK_Pause, EVT_GamePaused );

	// Line commented as it was interferring with CYB-616233, blocking the F9 key from triggering actions
	// mappingTable.MapKeyPress( IK_F9, DEBUG_EVT_StartExternalNetworkProfiler );

#ifdef ENABLE_SESSION_ACTIVE_DEBUG_TELEPORT_CHECK
	mappingTable.MapKeyPress( IK_PageDown, DEBUG_EVT_TeleportLoadingCheck );
#endif

#endif
}

Bool State_SessionActive::InitializeContext( const ArgsView& args, ParamsView& params )
{
	auto session = GetParentState<State_Session>();
	if ( !session )
	{
		RED_LOG_ERROR( "Expected State_Session as parent!" );
		return false;
	}

	session->OnSessionActive();

	const world::RuntimeInfo& runtimeInfo = session->GetGameSession().GetScene().GetRuntimeInfo();
	m_isSingleplayerGame = runtimeInfo.IsSingleplayer();

	if ( m_isSingleplayerGame && args.Get( RED_NAME_CONSTEXPR( "Debug_ProcessOneTick" ), false ) )
	{
		PauseTheGame();
	}

	return true;
}

void State_SessionActive::OnTeleporting()
{
	// CP2077 web [citycol]: name the cause of every in-game reload (world streaming observer jumped >= 64 m in one
	// update: teleport, fast travel or the PSM out-of-world failsafe) -> SessionLoading (FastTravelLoading).
	if ( ( m_transitionRequestFlags & SAF_Loading ) == 0 )
	{
		static unsigned s_cpTeleportLoads = 0;
		++s_cpTeleportLoads;
		if ( s_cpTeleportLoads <= 64 || ( s_cpTeleportLoads & ( s_cpTeleportLoads - 1 ) ) == 0 )
		{
			std::fprintf( stderr, "CP2077 citycol: SessionActive teleport-loading #%u (world streaming observer jumped >= 64 m: teleport / fast travel / out-of-world failsafe) -> SessionLoading\n", s_cpTeleportLoads );
		}
	}
	m_transitionRequestFlags |= SAF_Loading;
}

void State_SessionActive::SendUIState()
{
	static unsigned s_cp2077SendNull = 0, s_cp2077SendNoEvent = 0, s_cp2077SendQueued = 0;
	if ( !GetInkSystem().GetGameViewportWrapper() )
	{
		++s_cp2077SendNull;
		if ( ( s_cp2077SendNull & ( s_cp2077SendNull - 1 ) ) == 0 || ( s_cp2077SendNull % 4096 ) == 0 )
		{
			std::fprintf( stderr, "CP2077 ui state probe: SendUIState dropped, game viewport wrapper is null targetUI=%d nullCount=%u noEvent=%u queued=%u\n", int( GetTargetUIState() ), s_cp2077SendNull, s_cp2077SendNoEvent, s_cp2077SendQueued );
		}
	}
	if ( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
	{
		THandle< ink::StateChangeRequestEvent > stateEvent;

		switch ( GetTargetUIState() )
		{
		case ink::State::Game:
			stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkGameState" ) );
			break;
		case ink::State::EngagementScreen:
			stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkEngagementScreenState" ) );
			break;
		case ink::State::InitializeUserScreen:
			stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkInitializeUserScreenState" ) );
			break;
		case ink::State::PreGameMenu:
			stateEvent = CreateHandle< ink::StateChangeRequestEvent >( RED_NAME_CONSTEXPR( "inkPreGameMenuState" ) );
			break;
		}

		if ( stateEvent )
		{
			viewportWrapper->QueueViewportEvent( stateEvent );
			++s_cp2077SendQueued;
			if ( s_cp2077SendQueued <= 8 || ( s_cp2077SendQueued & ( s_cp2077SendQueued - 1 ) ) == 0 || ( s_cp2077SendQueued % 4096 ) == 0 )
			{
				std::fprintf( stderr, "CP2077 ui state probe: SendUIState queued targetUI=%d activeUI=%d wrapper=%p queued=%u nullCount=%u noEvent=%u\n", int( GetTargetUIState() ), int( viewportWrapper->GetStateMachine()->GetActiveUIState() ), (void*)viewportWrapper.Get(), s_cp2077SendQueued, s_cp2077SendNull, s_cp2077SendNoEvent );
			}
		}
		else
		{
			++s_cp2077SendNoEvent;
			if ( ( s_cp2077SendNoEvent & ( s_cp2077SendNoEvent - 1 ) ) == 0 || ( s_cp2077SendNoEvent % 4096 ) == 0 )
			{
				std::fprintf( stderr, "CP2077 ui state probe: SendUIState no event for targetUI=%d noEvent=%u queued=%u nullCount=%u\n", int( GetTargetUIState() ), s_cp2077SendNoEvent, s_cp2077SendQueued, s_cp2077SendNull );
			}
		}
	}
}

ink::State State_SessionActive::GetActiveUIState() const
{
	if ( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
	{
		return viewportWrapper->GetStateMachine()->GetActiveUIState();
	}

	return ink::State::Invalid;
}

void State_SessionActive::SendOnOpenPauseMenu()
{
	if( auto viewportWrapper = GetInkSystem().GetGameViewportWrapper() )
	{
		if( auto menuLayer = viewportWrapper->GetLayerByType< ink::MenuLayer >( ink::ELayerType::Menu ) )
		{
			menuLayer->QueueEvent( CreateHandle< ink::MenuInstance_SpawnEvent >( RED_NAME_CONSTEXPR( "OnOpenPauseMenu" ) ) );
		}
	}
}

void State_SessionActive::OnUserSignedOut()
{
	m_uiStateTransitionRequested = true;
	s_engagementScreenDone = false;

	ShowEngagementScreen();
	SendUIState();
}

TransitionRequest State_SessionActive::ProcessTransition()
{
	// ORDER determines priority

	const Uint32 requestFlags = m_transitionRequestFlags;
	m_transitionRequestFlags = 0;

	if ( ( requestFlags & SAF_Loading ) != 0 )
	{
		Args args;
		args.Set( RED_NAME_CONSTEXPR( "targetUIState" ), ink::State::Game );
		args.Set( RED_NAME_CONSTEXPR( "targetUILoadingState" ), ink::State::FastTravelLoading );
		return TransitionRequest::ReplaceTop( "SessionLoading", args );
	}
	else if ( ( requestFlags & SAF_ReconnectController ) != 0 )
	{
		Args args;
		args.Set( RED_NAME_CONSTEXPR( "deviceID" ), m_disconnectedID );

		if ( m_isSingleplayerGame && !m_isPreGame && GetActiveUIState() != ink::State::InGameMenu )
		{
			args.Set( RED_NAME_CONSTEXPR( "ShowReconnectController" ), true );
			args.Set( RED_NAME_CONSTEXPR( "currentUIState" ), GetActiveUIState() );
			return TransitionRequest::ReplaceTop("SessionPaused", args);
		}
		else
		{
			return TransitionRequest::Push("ReconnectController", args);
		}
	}
	else if ( ( requestFlags & SAF_PauseGameMenu ) != 0 )
	{
		if ( m_isSingleplayerGame )
		{
			Args args;
			args.Set( RED_NAME_CONSTEXPR( "ShowPauseMenu" ), true );
			args.Set( RED_NAME_CONSTEXPR( "currentUIState" ), GetActiveUIState() );
			return TransitionRequest::ReplaceTop( "SessionPaused", args );
		}
		else
		{
			return TransitionRequest::Push( "InGamePause" );
		}
	}
	else if ( m_isSingleplayerGame && ( requestFlags & SAF_GamePaused ) != 0 )
	{
		Args args;
		args.Set( RED_NAME_CONSTEXPR( "currentUIState" ), GetActiveUIState() );
		return TransitionRequest::ReplaceTop( "SessionPaused", args );
	}

	// stay where you are
	return TransitionRequest();
}

void State_SessionActive::ClearReconnectFlag( bool isUserSignedOut )
{
	m_transitionRequestFlags &= ~SAF_ReconnectController;

	if ( isUserSignedOut )
	{
		OnUserSignedOut();
	}
}

void State_SessionActive::RenderDebug( CRenderFrameInfo& framInfo, CRenderCameraFrameInfo& cameraInfo, const tick::Info& tickInfo ) const
{
}

const Bool State_SessionActive::ProcessMappedInput( const MappedInput& input )
{
 static unsigned inputReports=0; if(inputReports++<16)std::fprintf(stderr,"CP2077 session menu gate probe: input state=%u start=%u\n",unsigned(m_state),unsigned(input.m_inputEvent==EVT_Start));
#ifndef RED_CONFIGURATION_FINAL
	if (input.m_inputEvent == DEBUG_EVT_StartExternalNetworkProfiler )
	{
		auto session = GetParentState< State_Session >();
		RED_FATAL_ASSERT( session );
		session->GetGameSession().LaunchExternalNetworkProfiler();
		return true;
	}
#endif

	if ( m_state == InternalState::WaitingForButtonPress )
	{
		if ( input.m_inputEvent == EVT_Start )
		{
			if (audio::SoundSystem* soundSystem = GEngine->GetSoundSystem())
			{
				soundSystem->GetScriptAudio()->Play( RED_NAME_CONSTEXPR( "ui_jingle_press_to_continue" ) );
			}

			m_state = InternalState::ShowUserSelection;
			m_activeDeviceID = input.m_deviceId;
			return true;
		}
	}

	if ( input.m_inputEvent == EVT_GamePauseMenu )
	{
		m_transitionRequestFlags |= SAF_PauseGameMenu;
		return true;
	}

	if (input.m_inputEvent == EVT_GamePaused )
	{
		PauseTheGame();
		return true;
	}

#ifdef ENABLE_SESSION_ACTIVE_DEBUG_TELEPORT_CHECK
	if ( input.m_inputEvent == DEBUG_EVT_TeleportLoadingCheck )
	{
		auto session = GetParentState< State_Session >();
		RED_FATAL_ASSERT( session );
		THandle< game::Object > player = session->GetGameSession().GetGameInstance().GetSystem< game::IPlayerSystem >().GetLocalPlayerMainObject();
		if ( player )
		{
			const Vector4 targetPos = Vector4::ZEROS();
			const Quaternion targetRot = Quaternion::IDENTITY();
			player->QueueEvent( CreateHandle< game::stateMachine::event::Reset >( targetPos, targetRot.AsVector() ) );
		}
	}
#endif

	return false;
}

void State_SessionActive::ProcessServices( services::GameServices& services )
{
	if ( m_state == InternalState::ShowUserSelection )
	{
		if ( !services.HasUserAssigned() )
		{
			services.AssignUser( m_activeDeviceID );

			m_state = InternalState::WaitingForUserSelection;
		}
		else
		{
			m_state = InternalState::StopVideo;
		}
	}

	State_SessionStreamingAware::ProcessServices( services );
}

void State_SessionActive::PauseTheGame()
{
	if ( GetParentState()->GetClass()->GetName() != RED_NAME_CONSTEXPR( "gsmState_PreGameSession" ) )
	{
		m_transitionRequestFlags |= SAF_GamePaused;
	}
}

void State_SessionActive::MapEngagementScreenKeys()
{
	auto &inputMapper = GetInputMapper();

	inputMapper.Map( IK_Pad_Start, SInputMapping( EComplexInputAction::ACTION_Pressed, EVT_Start ) );
	inputMapper.Map( IK_PS4_OPTIONS, SInputMapping( EComplexInputAction::ACTION_Pressed, EVT_Start ) );
	inputMapper.Map( IK_Space, SInputMapping( EComplexInputAction::ACTION_Pressed, EVT_Start ) );
}

void State_SessionActive::UnmapEngagementScreenKeys()
{
	auto &inputMapper = GetInputMapper();

	inputMapper.Unmap( IK_Pad_Start );
	inputMapper.Unmap( IK_PS4_OPTIONS );
	inputMapper.Unmap( IK_Space );
}

const Bool State_SessionActive::ProcessMessage( const ExternalMessage& msg, const StateMachine &gsm )
{
	if ( msg.m_name == RED_NAME_CONSTEXPR_NOREG( "GameServicesUserSignOutComplete" ))
	{
		if ( msg.m_params == "CurrentUser" && m_isPreGame && m_state == InternalState::Active )
		{
			OnUserSignedOut();
		}
	}
	else if ( msg.m_name == RED_NAME_CONSTEXPR_NOREG( "GameServicesAssignUser" ) )
	{
		if ( msg.m_params == "success" )
		{
			m_state = InternalState::StopVideo;
			return true;
		}
		else if ( msg.m_params == "canceled" )
		{
			if ( m_state == InternalState::WaitingForUserSelection )
			{
				m_state = InternalState::WaitingForButtonPress;
				m_initializeUserState = InitializeUserState::None;
				m_activeDeviceID = 0;
			}
			return true;
		}
	}
	else if ( msg.m_name == EVT_RequestSkipStartScreen )
	{
		// #tbd: for now no user assigned
		m_state = InternalState::StopVideo;
		m_activeDeviceID = 0;
		return true;
	}
	else if ( msg.m_name == EVT_Pause )
	{
		PauseTheGame();
	}
	else if( msg.m_name == EVT_PauseBackground )
	{
		if( GetActiveUIState() == ink::State::Game )
		{
			SendOnOpenPauseMenu();
		}
		return true;
	}
	else if (msg.m_name == input::Messages::GamepadDisconnectedMessage)
	{
		m_transitionRequestFlags |= SAF_ReconnectController;
		m_disconnectedID = FromStringDirect<Uint64>(msg.m_params);
		return true;
	}

	return false;
}

} // namespace gsm
