#include "native-initialization-job-trace.h"
#include <atomic>
#include <cstdio>
/**
 * Copyright (c) 2015-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"

#include "gameStateMachineImpl.h"
#include "gameStateStack.h"
#include "gameStateObserver_UserSignInOut.h"
#include "gameStateObserver_FunctionalTests.h"
#include "gameStateObserver_ForceShutdown.h"
#include "gameStateObserver_SessionAutomation.h"
#include "gameStateObserver_RichPresence.h"
#include "gameStateObserver_SessionChanged.h"

#include "../../renderData/include/renderer.h"
#include "../../renderData/include/renderCommandHandler.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../engine/include/tickInfo.h"

using red::DynArray;
using red::StaticArray;

//--------------------------------------

namespace Config
{
	TConfigVar< red::String > cvGameFrameworkInitialState( "GameFramework", "InitialState", "Initialization" );
	TConfigVar< Bool > cvGameFrameworkDebugStateMachine( "GameFramework", "DebugStateMachine", false );
	TConfigVar< red::String > cvGameFunctionalTestsState( "GameFramework", "FunctionalTestsState", "FunctionalTests" );
}

namespace dd
{
	static red::CrashData<res::ResourcePath> gameDef("Game", "GameDefinition");
}

namespace temp
{
	extern RED_ENGINE_API void SetGameDefPathNameForWatermark(const res::ResourcePath& resourcePath);
}

//--------------------------------------

namespace
{
	struct StateMachineInitDataContext
	{
	public:
		StateMachineInitDataContext( gsm::Params& paramStack )
		{
			gsm::ParamsView view{ paramStack, 0 };
			hasGameDef = view.Find( RED_NAME_CONSTEXPR_NOREG( "gameDefinition" ), gameDefinition );
			hasDirectState = view.Find< red::String >( RED_NAME_CONSTEXPR_NOREG( "state" ), directStateName );
			hasSave = view.Find< red::String >( RED_NAME_CONSTEXPR_NOREG( "save" ), saveName );
		}

		red::String gameDefinition;
		red::String directStateName;
		red::String saveName;

		Bool hasGameDef;
		Bool hasDirectState;
		Bool hasSave;
	};
}

namespace gsm
{
	//-------------------

	StateMachine::LazyOperation::LazyOperation( const TLazyFunction& func )
		: m_func( func )
	{
	}

	red::AsyncStatus StateMachine::LazyOperation::Process( const Float timeDelta, const tick::Info& tickInfo )
	{
		return m_func( timeDelta, tickInfo );
	}

	//-------------------

	StateMachine::LazyOperations::LazyOperations()
	{
	}

	void StateMachine::LazyOperations::AddFunction( const TLazyFunction& function )
	{
		m_operations.PushBack( red::CreateUniquePtr<LazyOperation>( function ) );
	}

	red::AsyncStatus StateMachine::LazyOperations::Process( const Float timeDelta, const tick::Info& tickInfo )
	{
		auto it = m_operations.Begin();
		while ( it != m_operations.End() )
		{
			const auto result = ( *it )->Process( timeDelta, tickInfo );

			if ( result == red::AsyncStatus::Error )
			{
				RED_LOG_ERROR( "GSM: Async operation has failed" );
				return red::AsyncStatus::Error;
			}
			else if ( result == red::AsyncStatus::Done )
			{
				it = m_operations.Remove( it ).Iterator();
			}
			else
			{
				++it;
			}
		}

		// there are no more async tasks, we are done
		if ( m_operations.Empty() )
		{
			return red::AsyncStatus::Done;
		}

		// we are done if there are no operations on the list, if there are still some operations and we are here it means that at least one returned NotDone
		return red::AsyncStatus::NotDone;
	}

	//-------------------

	StateMachine::RetiringState::RetiringState( const StatePtr& state )
		: m_state( state )
	{
		m_state->ProcessShutdown( m_lazyOperations ); // this may spawn async ops
	}

	StateMachine::RetiringState::~RetiringState() = default;

	red::AsyncStatus StateMachine::RetiringState::Process( const Float timeDelta, const tick::Info& tickInfo )
	{
		return m_lazyOperations.Process( timeDelta, tickInfo );
	}

	//--------------------------------------

	StateMachine::IncomingState::IncomingState( const StatePtr& state )
		: m_state( state )
		, m_status( red::AsyncStatus::NotDone )
	{
	}

	StateMachine::IncomingState::~IncomingState() = default;

	Bool StateMachine::IncomingState::Initialize( const ArgsView& args, ParamsView& params )
	{
		return m_state->ProcessInit( m_lazyOperations, args, params );
	}

	red::AsyncStatus StateMachine::IncomingState::Process( const Float timeDelta, const tick::Info& tickInfo )
	{
		m_status = m_lazyOperations.Process( timeDelta, tickInfo );
		return m_status;
	}

	void StateMachine::IncomingState::ProcessServices( services::GameServices& services )
	{
		m_state->ProcessServices( services );
	}

	StateMachine* StateMachine::Create( ParamsView& initialParams, SystemsUpdater* processor, game::IGameInstance* gameInstance, world::RuntimeScene* scene )
	{
		return RED_NEW( StateMachine )( initialParams, processor, gameInstance, scene );
	}

	//--------------------------------------

	StateMachine::StateMachine( ParamsView& initialParams, SystemsUpdater* processor, game::IGameInstance* gameInstance, world::RuntimeScene* scene )
		: m_processor( processor )
		, m_gameInstance( gameInstance )
		, m_scene( scene )
		, m_stack( red::CreateUniquePtr< StateStack >() )
		, m_paramStack( red::CreateUniquePtr< Params >() )
		, m_incomingState( nullptr )
	{
		// set initial global parameters of the whole state machine
		// usually this is a good way to pass globals (like multiplayer session manager)
		// or external parameters like world to play and initial camera position
		initialParams.Extract( *m_paramStack );

		// create the observers i.e reconnect controller and handle user sign out
		CreateObservers( initialParams );
	}

	StateMachine::~StateMachine()
	{
		if ( m_incomingState )
		{
			RED_DELETE( m_incomingState );
		}

		RED_FATAL_ASSERT( !m_stack->Size() );
		RED_FATAL_ASSERT( m_retiringStates.Empty() );
	}

	void StateMachine::DispatchInitialization( const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
	{
		red::SharedPtr< StateMachineInitDataContext, red::PoolEngine > dataContext = red::CreateSharedPtr< StateMachineInitDataContext, red::PoolEngine >( *m_paramStack );

		job::Builder builder{ context };

		if ( dataContext->hasGameDef )
		{
			const auto gameDefPath = res::ResourcePath::Build( dataContext->gameDefinition );
			auto token = GResourceLoader->IssueLoadingRequest( gameDefPath );
			const job::Counter& counter = token->GetWaitCounter();
			builder.DispatchWait( counter );

			builder.DispatchJob( "StateMachine/OnGameDefLoaded", [ this, token, dataContext, &keepRunning ]( const job::RunContext& context )
			{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"StateMachine/OnGameDefLoaded"};
				if ( !keepRunning.GetValue() )
				{
					return;
				}

				auto gameDef = Cast< GameDefinition >( token->GetResource() );
				if ( !gameDef )
				{
					RED_LOG_ERROR( "StateMachine: Failed to load game definition (%s)", token->GetPath().ToDebugString() );
					keepRunning.SetValue( 0 );
					return;
				}

				dd::gameDef.Set( token->GetPath() );
				temp::SetGameDefPathNameForWatermark( token->GetPath() );

				ParamsView view{ *m_paramStack, 0 };
				view.Push( RED_NAME_CONSTEXPR( "world" ), gameDef->GetWorldID() );

				auto startQuestPath = gameDef->GetMainQuestPath();
				if ( startQuestPath.IsValid() )
				{
					view.Push( RED_NAME_CONSTEXPR( "startQuest" ), TResAsyncRef< quest::QuestResource >( startQuestPath ) );
				}

				view.Push( RED_NAME_CONSTEXPR( "spawnTags" ), gameDef->GetSpawnPointTags() );
			} );
		}

		builder.DispatchJob( "StateMachine/OnGameDefLoaded", [ this, dataContext, &keepRunning ]( const job::RunContext& context )
		{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"StateMachine/OnGameDefLoaded"};
			if ( !keepRunning.GetValue() )
			{
				return;
			}

			ParamsView view{ *m_paramStack, 0 };
			if ( view.Get< Bool >( RED_NAME_CONSTEXPR( "functionalTestsMode" ) ) )
			{
				ProcessStateTransitions( TransitionRequest::ReplaceAll( Config::cvGameFunctionalTestsState.Get().AsChar() ), 0 );
			}
			else
			{
				const auto& initialStateName = Config::cvGameFrameworkInitialState.Get();
				ProcessStateTransitions( TransitionRequest::ReplaceAll( initialStateName.AsChar() ), 0 );
			}
		} );
	}

	const Bool StateMachine::ProcessFrame( const gsm::Frame& frame )
	{
		for ( const auto& msg : frame.m_messages )
		{
			ProcessMessage( msg );
		}

		red::DynArray< ExternalMessage > tempMessages{ red::PoolEngine() };
		{
			RED_SCOPE_LOCK( m_messagesLock );
			tempMessages = std::move( m_messages );
		}
		m_automationEventSinkProxy.AppendReadyMessages( tempMessages );

		for ( const auto& msg : tempMessages )
		{
			ProcessMessage( msg );
		}

		if ( GGameServices )
		{
			ProcessServices( *GGameServices );

		}

		ProcessInput( frame.m_input );

		// Observe the original loop without changing state transitions or input.
		static std::atomic<unsigned> browserFrames{0};
		const auto browserFrame=browserFrames.fetch_add(1)+1;
		const Bool reportBrowserFrame=browserFrame<=8 || (browserFrame<=1024 && !(browserFrame&(browserFrame-1)));
		if(reportBrowserFrame)
		{
			auto state=m_stack->GetTopmostState();
			std::fprintf(stderr,"CP2077 game frame probe: phase=begin frame=%u timeDelta=%g realTimeDelta=%g dilation=%g state=%s incoming=%u\n",
				browserFrame,double(frame.m_timeDelta),double(frame.m_tickInfo.m_realTimeDelta),double(frame.m_tickInfo.m_globalTimeDilation),
				state?state->GetClass()->GetName().AsChar():"<empty>",unsigned(m_incomingState!=nullptr));
		}
		ProcessTick( frame.m_timeDelta, frame.m_tickInfo );
		if(reportBrowserFrame)std::fprintf(stderr,"CP2077 game frame probe: phase=tick-returned frame=%u\n",browserFrame);

		if ( frame.m_frameInfo && frame.m_cameraFrameInfo )
		{
			ProcessRender( frame.m_freeCamera.Get(), *frame.m_frameInfo, frame.m_tickInfo, *frame.m_cameraFrameInfo );
			if(reportBrowserFrame)std::fprintf(stderr,"CP2077 game frame probe: phase=render-returned frame=%u\n",browserFrame);
		}

		// always keep the game running while the state machine is not empty
		const auto keepRunning = ( m_stack->Size() || IsInTransition() );
		return keepRunning;
	}

	void StateMachine::ProcessTick( Float timeDelta, const tick::Info& tickInfo )
	{
		TickContext context( timeDelta, tickInfo );

		// dump state each frame
		if ( Config::cvGameFrameworkDebugStateMachine.Get() )
		{
			DebugDump();
		}

		TransitionRequest transitionRequest;

		// advance incoming state, it may be advanced to full state
		transitionRequest = ProcessIncomingState( timeDelta, tickInfo );

		// tick states from top to bottom, the higher states may block ticking of the the lower states
		TickControl control = TickControl::AllowNext;
		StateTickFlagsContext flags;
		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			it->ProcessTick( context, flags, control );
			if ( control == TickControl::Consume )
			{
				break;
			}
		}

		// see if we can go somewhere
		if ( !IsInTransition() )
		{
			// if we don't have a transition request already:
			// first ask the state observers if they want to go anywhere
			// then ask the state statck for the transition requests, start with the top most state always
			Uint32 stackDepth = 1;
			if ( !transitionRequest )
			{
				for ( const auto& observer : m_observers )
				{
					transitionRequest = observer->ProcessTransition();
					if ( transitionRequest )
					{
						break;
					}
				}
			}

			if ( !transitionRequest )
			{
				for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
				{
					transitionRequest = it->ProcessTransition();

					if ( transitionRequest || transitionRequest.IsIgnore() )
					{
						break;
					}

					stackDepth += 1; // stack depth is tracked so in case of the Replace transition mode the correct states will be unwind
				}
			}

			// process the state transition (retire -> initialize)
			if ( transitionRequest && !transitionRequest.IsIgnore() )
			{
				ProcessStateTransitions( transitionRequest, stackDepth );
			}
		}

		// advance retiring states, the ones that finish the shutdown will be removed before we render this frame
		ProcessRetiringStates( timeDelta, tickInfo );
	}

	void StateMachine::ProcessRetiringStates( const Float timeDelta, const tick::Info& tickInfo )
	{
		auto it = m_retiringStates.Begin();
		while ( it != m_retiringStates.End() )
		{
			const auto ret = ( *it ).Process( timeDelta, tickInfo );
			if ( ret == red::AsyncStatus::NotDone )
			{
				++it;
			}
			else
			{
				it = m_retiringStates.Remove( it ).Iterator();
			}
		}
	}

	TransitionRequest StateMachine::ProcessIncomingState( const Float timeDelta, const tick::Info& tickInfo )
	{
		TransitionRequest retVal;
		if ( m_incomingState )
		{
			auto status = m_incomingState->m_status;
			if ( status == red::AsyncStatus::NotDone )
			{
				status = m_incomingState->Process( timeDelta, tickInfo );
			}

			if ( status == red::AsyncStatus::Error )
			{
				// in case of an error, it may want to go to some specific state (like: presenting a message to the user)
				retVal = m_incomingState->m_state->ProcessTransition();

				// don't delete the incoming state immediately, wait for the retiring states to finish
				// this will allow transitioning to the new state after error
				if ( m_retiringStates.Empty() )
				{
					RED_LOG_ERROR( "GSM: Async operations for incoming state failed" );
					RED_DELETE( m_incomingState );
					m_incomingState = nullptr;
				}
			}
			else if ( status == red::AsyncStatus::Done )
			{
				std::fprintf(stderr,"CP2077 game state probe: phase=active class=%s id=%d\n",
					m_incomingState->m_state->GetClass()->GetName().AsChar(),int(m_incomingState->m_state->GetDynamicId()));
				RED_LOG( "GSM: Async operations finished" );

				m_stack->Push( m_incomingState->m_state );

				RED_DELETE( m_incomingState );
				m_incomingState = nullptr;
			}
		}

		return retVal;
	}

	void StateMachine::ProcessRender( const CRenderCamera* freeCamera, CRenderFrameInfo& frame, const tick::Info& tickInfo, CRenderCameraFrameInfo& cameraInfo )
	{
		PC_SCOPE( ProcessRender );

		// TODO: find the lowest state that should be rendered (opaque)

		// the Stack represents the baseline (states that are not changing in a transition)
		// the retiring states were previously on the stack, now they are being removed
		// the incoming state will be added on top of the stack

		// prepare rendering context
		RenderContext context( frame, cameraInfo, tickInfo );
		RenderControl control;

		// set default camera from free camera
		if ( freeCamera != nullptr )
		{
			cameraInfo.SetRenderCamera( *freeCamera );
		}

		// render normal states - back to front
		for ( StateStack::BackToFrontIterator it( *m_stack ); it; ++it )
		{
			it->ProcessRender( context, control );
		}

		// render retiring states
		for ( const auto& retiringState : m_retiringStates )
		{
			retiringState.m_state->ProcessRender( context, control );
		}

		// render the incoming state - note that the exit animation for the retiring state and the intro animation for the incoming state may overlap in time
		if ( m_incomingState )
		{
			m_incomingState->m_state->ProcessRender( context, control );
		}

		if ( const auto& scene = frame.GetScenePtr() )
		{
			// Pass camera to renderer
			GRender->GetCommands()->RegisterCamera( scene, cameraInfo.GetRenderCamera(), SCameraRenderViewport::Fullscreen(), 0 );
			GRender->GetCommands()->UpdateCameraData( scene, 0, cameraInfo );

			frame.IssueCamera( 0 );
		}
	}

	void StateMachine::ProcessInput( input::IInputContext& context )
	{
		// process input directly in states
		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			( *it )->ProcessInput( context );
		}
	}

	void StateMachine::ProcessServices( services::GameServices& services )
	{
		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			( *it )->ProcessServices( services );
		}
	}

	void StateMachine::ProcessMessage( const ExternalMessage& msg )
	{
		for ( const red::UniquePtr<IStateObserver>& observer : m_observers )
		{
			observer->ProcessMessage( msg, *this );
		}

		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			if ( ( *it )->ProcessMessage( msg, *this ) )
			{
				break;
			}
		}
	}

	Bool StateMachine::IsInTransition() const
	{
		return m_incomingState || !m_retiringStates.Empty();
	}

	void StateMachine::RetireStackTop( const Uint32 numTopStatesToRetire /* = 1 */ )
	{
		RED_FATAL_ASSERT( m_retiringStates.Empty(), "We already have states to retire. Recursive call or out of order call." );

		// add stack in the normal order
		Uint32 left = numTopStatesToRetire;
		while ( left && m_stack->Size() )
		{
			// get the top state
			auto topState = m_stack->Pop();

			// notify parent state about child retiring
			if ( topState->GetParentState() )
			{
				topState->GetParentState()->OnChildStateShutdown();
			}

			// start shutting down
			m_retiringStates.PushBack( RetiringState( topState ) );

			// remove arguments from the parameter stack
			m_paramStack->RemoveStateValues( topState->GetDynamicId() );

			// debug info
			if ( Config::cvGameFrameworkDebugStateMachine.Get() )
			{
				RED_LOG( "GameState: Retired state '%hs', ID %d", topState->GetClass()->GetName().AsChar(), topState->GetDynamicId() );
			}

			// state retired
			left -= 1;
		}
	}

	Bool StateMachine::PushNewState( const StatePtr& state, const Args& args, TransitionMode mode )
	{
		RED_FATAL_ASSERT( !m_incomingState, "There already is an incoming state" );
		RED_FATAL_ASSERT( state, "Trying to push invalid state" );

		ParamsView paramView( *m_paramStack, state->GetDynamicId() ); // create a view object to prevent caching
		const ArgsView argView( args ); // prevent modifications and caching

		// prepare state for execution
		StatePtr parentState = nullptr;
		if ( mode == TransitionMode::Replace )
		{
			parentState = m_stack->GetStateAtDepth( 1 ); // we will be replacing the last state
		}
		else if ( mode == TransitionMode::Push )
		{
			parentState = m_stack->GetStateAtDepth( 0 );// we will be adding on top of the last state
		}

		state->Prepare( this, parentState, m_processor, m_gameInstance, m_scene, argView );

		// start initialization
		auto* stateInfo = RED_NEW( IncomingState )( state );
		if ( !stateInfo->Initialize( argView, paramView ) )
		{
			RED_LOG( "GSM: Failed to initialize state '%hs'", state->GetClass()->GetName().AsChar() );
			RED_DELETE( stateInfo );

			m_paramStack->RemoveStateValues( state->GetDynamicId() );
			return false;
		}

		// set as new incoming state
		m_incomingState = stateInfo;
		std::fprintf(stderr,"CP2077 game state probe: phase=incoming class=%s id=%d\n",
			state->GetClass()->GetName().AsChar(),int(state->GetDynamicId()));

		// debug info
		if ( Config::cvGameFrameworkDebugStateMachine.Get() )
		{
			RED_LOG( "GameState: New incoming state '%hs', ID %d", state->GetClass()->GetName().AsChar(), state->GetDynamicId() );
		}

		return true;
	}

	Bool StateMachine::ProcessStateTransitions( const TransitionRequest& transitionRequest, const Uint32 stackDepth )
	{
		RED_FATAL_ASSERT( m_retiringStates.Empty(), "Cannot process another transition while in transition" );
		RED_FATAL_ASSERT( m_incomingState == nullptr, "Cannot process another transition while in transition" );

		// empty transition request, we stay where we are
		if ( !transitionRequest )
		{
			return true;
		}

		ParamsView paramView( *m_paramStack, 0 );
		const Bool keepFunctionalTestsState =
			paramView.Get< Bool >( RED_NAME_CONSTEXPR( "functionalTestsMode" ) ) ||
			paramView.Get< Bool >( RED_NAME_CONSTEXPR( "keepFunctionalTestsState" ) );

		Uint32 stackSize = m_stack->Size();
		if ( keepFunctionalTestsState && stackSize > 0 )
		{
			stackSize = stackSize - 1;
		}

		// pop - remove current state
		if ( transitionRequest.m_mode == TransitionMode::PopTop )
		{
			RetireStackTop( 1 );
			return true;
		}
		else if ( transitionRequest.m_mode == TransitionMode::PopAll )
		{

			RetireStackTop( stackSize );
			return true;
		}
		else if ( transitionRequest.m_mode == TransitionMode::PopSelf )
		{
			RetireStackTop( stackDepth );
			return true;
		}

		// all transitions but the top require a valid state, check that before doing anything else
		auto newState = transitionRequest.m_state.Resolve();
		if ( !newState )
		{
			RED_LOG_ERROR( "GSM: Unable to resolve target state for state transtiion" );
			return false;
		}

		// push new state, may fail
		if ( !PushNewState( newState, transitionRequest.m_args, transitionRequest.m_mode ) )
		{
			return false;
		}

		// retire existing states
		if ( transitionRequest.m_mode == TransitionMode::Replace )
		{
			RetireStackTop( stackDepth );
		}
		else if ( transitionRequest.m_mode == TransitionMode::ReplaceFullStack )
		{
			RetireStackTop( stackSize );
		}

		return true;
	}

	void StateMachine::DebugDump()
	{
		static Int32 tickIndex = 0; // to prevent "stationary log effect"

		RED_LOG( "GameState: GameStateMachine state, index %d", tickIndex++ );
		RED_LOG( "GameState:   %d states on stack", m_stack->Size() );

		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			const auto* state = *it;
			RED_LOG( "GameState:     State ID %d, Class '%hs'", state->GetDynamicId(), state->GetClass()->GetName().AsChar() );
		}

		if ( !m_retiringStates.Empty() )
		{
			RED_LOG( "GameState:   %d retiring states", m_retiringStates.Size() );

			for ( const auto& info : m_retiringStates )
			{
				if ( info.m_state )
				{
					RED_LOG( "GameState:     State ID %d, Class '%hs'", info.m_state->GetDynamicId(), info.m_state->GetClass()->GetName().AsChar() );
				}
			}
		}

		if ( m_incomingState != nullptr )
		{
			RED_LOG( "GameState:   1 incoming state" );
			RED_LOG( "GameState:     State ID '%d', Class '%hs'",
				m_incomingState->m_state->GetDynamicId(),
				m_incomingState->m_state->GetClass()->GetName().AsChar() );
		}

		if ( !m_paramStack->Empty() )
		{
			RED_LOG( "GameState:   Param stack" );
			m_paramStack->DebugDump();
		}
	}

	Bool StateMachine::HasStateInStack( const String &stateName ) const
	{
		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			if ( ( *it )->GetStateName() == stateName )
			{
				return true;
			}
		}

		return false;
	}

	Bool StateMachine::IsTopmostState( const String &stateName ) const
	{
		StatePtr state = m_stack->GetTopmostState();
		return state ? state->GetStateName() == stateName : false;
	}

	Bool StateMachine::IsBottommostState( const String &stateName ) const
	{
		StatePtr state = m_stack->GetBottommostState();
		return state ? state->GetStateName() == stateName : false;
	}

	const State* StateMachine::GetStateFromStack( const String &stateName ) const
	{
		for ( StateStack::TopDownIterator it( *m_stack ); it; ++it )
		{
			if ( ( *it )->GetStateName() == stateName )
			{
				return *it;
			}
		}

		return nullptr;
	}

	void StateMachine::PostMessage( const ExternalMessage& message )
	{
		RED_SCOPE_LOCK( m_messagesLock );
		m_messages.PushBack( message );
	}

	void StateMachine::CreateObservers( ParamsView& initialParams )
	{
		if ( initialParams.Get< Bool >( RED_NAME_CONSTEXPR( "functionalTestsMode" ) ) )
		{
			RED_LOG( "GameState:   Skip StateObserver_UserSignInOut registration in functionalTestsMode." );
			RED_LOG( "GameState:   StateObserver_UserSignInOut override functional test trasitions on consoles." );

			FunctionalTestsObserverSetup setup;
			setup.keepFunctionalTestsStateCallback = [ this ]( Bool enable )
			{
				ParamsView paramView( *m_paramStack, 0 );
				paramView.Push( RED_NAME_CONSTEXPR( "keepFunctionalTestsState" ), enable );
			};

			AttachObserver( red::CreateUniquePtr< gameStateObserver_FunctionalTests >( setup ) );
		}
		else
		{
			auto rtti_ptr = GetRttiSystem().FindClass( GetTypeName< StateObserver_UserSignInOut >() );
			RED_ASSERT( rtti_ptr, "Error: Couldn't find rtti class for %s", "StateObserver_UserSignInOut" );

			StateObserverPtr user_signin_out = StateObserverPtr( rtti_ptr->CreateObject<IStateObserver>() );
			AttachObserver( std::move( user_signin_out ) );
		}

		if ( initialParams.Has< Bool >( RED_NAME_CONSTEXPR_NOREG( "forceShutdownAfterSessionLoaded" ) ) )
		{
			AttachObserver( red::CreateUniquePtr< StateObserver_ForceShutdown >() );
		}

		if ( initialParams.Get< Bool >( RED_NAME_CONSTEXPR( "automator" ), false ) )
		{
			SessionAutomationSetup setup;
			const Uint32 delaySeconds = 10; // hardcoded for the moment
			setup.automationID = 0;
			setup.automationFinishedCallback = [ this, delaySeconds ]()
			{
				RED_UNUSED( delaySeconds ); // making clang happy
				m_automationEventSinkProxy.RequestExitToMainMenu( delaySeconds );
			};

			setup.heartbeatCallback = [ this ]()
			{
				m_automationEventSinkProxy.Heartbeat();
			};

			AttachObserver( red::CreateUniquePtr< StateObserver_SessionAutomation >( setup, &m_automationEventSinkProxy ) );
		}

		AttachObserver( red::CreateUniquePtr< StateObserver_RichPresence >( m_gameInstance ) );
		
#ifdef RED_PLATFORM_WINPC
		AttachObserver( red::CreateUniquePtr<StateObserver_SessionChanged>() );
#endif
	}

	void StateMachine::AttachObserver( StateObserverPtr&& observer )
	{
		m_observers.PushBack( std::move( observer ) );
	}
} // namespace gsm
