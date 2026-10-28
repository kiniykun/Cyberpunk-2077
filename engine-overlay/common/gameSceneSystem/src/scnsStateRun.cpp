// Copyright (C) 2017-2020 CD Projekt Red. All Rights Reserved.

#include "build.h"
#include "scnsStateRun.h"
#include "scnsQuestNode.h" // [nomadplay]
#include "../../gameQuest/include/node.h" // [nomadplay]
#include <cstdio>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include <mutex>
#include <string>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif
#include "scnsStateUtils.h"

#include "scnSceneSystem.h"
#include "scnSceneGraph.h"
#include "scnActionPartFilter.h"
#include "scnsCommandChoiceNode.h"
#include "scnChoiceHubSystem.h"
#include "scnsSupervisor.h"

#include "../../world/include/gameInstance.h"

namespace scn
{
	// CP2077 web [nomadplay]: scene node trace ('[nomadplay] sn ...'): START when a scene graph node reports inprogress the
	// first time, END when it reports finished (with duration), and a 'SCN WAIT' line once a node stays in progress > 20 s
	// (logged from the same tick, so a dead scene that stops ticking prints nothing new).
	// CP2077 web [nomadplay]: tracing is on for ?start=nomadquest (or &questlog=1|all), off elsewhere (&questlog=0 forces off)
	static bool NpLogOn()
	{
		static int s_on = -1;
		if ( s_on < 0 )
		{
			s_on = 0;
#ifdef __EMSCRIPTEN__
			s_on = MAIN_THREAD_EM_ASM_INT( { try { const q = new URLSearchParams( location.search ); const v = q.get( 'questlog' ); if ( v === '0' ) return 0; return ( v !== null || q.get( 'start' ) === 'nomadquest' ) ? 1 : 0; } catch ( e ) { return 0; } } );
#endif
		}
		return s_on > 0;
	}

	struct NpScnEntry { const void* inst; Uint32 node; Double t0; Bool waitLogged; std::string cls; };
	static std::mutex s_npScnMutex;
	static std::vector< NpScnEntry > s_npScn;
	static Double NpScnNow()
	{
#ifdef __EMSCRIPTEN__
		return emscripten_get_now() * 0.001;
#else
		return 0.0;
#endif
	}
	static void NpSceneNode( const void* inst, Uint32 nodeId, const char* cls, NodeState state )
	{
		if ( state == NodeState::notchanged )
		{
			return;
		}
		std::lock_guard< std::mutex > lock( s_npScnMutex );
		const Double now = NpScnNow();
		for ( size_t i = 0; i < s_npScn.size(); ++i )
		{
			NpScnEntry& e = s_npScn[ i ];
			if ( e.inst == inst && e.node == nodeId )
			{
				if ( state == NodeState::finished || state == NodeState::notstarted )
				{
					std::fprintf( stderr, "[nomadplay] sn END %p#%u %s after %.1fs\n", inst, nodeId, e.cls.c_str(), now - e.t0 );
					s_npScn.erase( s_npScn.begin() + i );
				}
				return;
			}
		}
		if ( state == NodeState::inprogress )
		{
			std::fprintf( stderr, "[nomadplay] sn START %p#%u %s\n", inst, nodeId, cls );
			s_npScn.push_back( NpScnEntry{ inst, nodeId, now, false, cls } );
		}
		else if ( state == NodeState::finished )
		{
			std::fprintf( stderr, "[nomadplay] sn PASS %p#%u %s\n", inst, nodeId, cls );
		}
	}
	static void NpSceneWaitCheck()
	{
		std::lock_guard< std::mutex > lock( s_npScnMutex );
		const Double now = NpScnNow();
		static Double s_nextAll = 0.0;
		const Bool all = now >= s_nextAll;
		if ( all )
		{
			s_nextAll = now + 30.0;
		}
		for ( NpScnEntry& e : s_npScn )
		{
			const Double age = now - e.t0;
			if ( ( !e.waitLogged && age > 20.0 ) || ( all && age > 20.0 ) )
			{
				std::fprintf( stderr, "[nomadplay] sn %s %p#%u %s in progress %.0fs\n", e.waitLogged ? "STILL" : "WAIT", e.inst, e.node, e.cls.c_str(), age );
				e.waitLogged = true;
			}
		}
	}
	/*
	Ctor.
	*/
	SciStateRun::SciStateRun()
		: SciState( SciStateName::run )
	{}

	void SciStateRun::DoEnter( SceneInstance& scInst )
	{
		scInst.m_sideeffectsExecutor.Cancel();
	}

	void SciStateRun::DoExit( SceneInstance& scInst )
	{
		scInst.ConcludeRunsession();

		if ( scInst.ParticipatesInFastForwarding() )
		{
			scInst.GetFastForwarder().ClearSceneInstanceParticipation( scInst.GetSceneInstanceId() );
		}
	}

	// \param timeWindow Time window, in Scene System time space. #szi_todo Rename timeWindow to sstTimeWindow. Also, rename all other variables related to time so they properly use sit/sst prefixes.
	SciTransitionName SciStateRun::DoTick( SceneInstance& scInst, TimeWindow timeWindow, ControlRequest* req )
	{
		// TODO: Collect and process world events routed to this scene.

		const SceneTime dt = timeWindow.GetDuration();

		SciTransitionName transitionName( SciTransitionName::null );

		//this collection is filled only with one element
		red::StaticArray< SignalToken, 1 > cancelTokenBroadcast;

		if ( req )
		{
			switch ( req->GetType() )
			{
			case Control::Request::Type::noop:
			{
				RED_FATAL_ASSERT( req->GetStatus().m_status == ControlRequestStatus::Status::notstarted, "Invalid request: expected request that has not yet started." );
				req->SetStatus( ControlRequestStatus( ControlRequestStatus::Status::completedOk ) );
				transitionName = SciTransitionName::null;
				break;
			}

			case Control::Request::Type::pause:
			{
				RED_FATAL_ASSERT( req->GetStatus().m_status == ControlRequestStatus::Status::notstarted, "Invalid request: expected request that has not yet started." );
				req->SetStatus( ControlRequestStatus( ControlRequestStatus::Status::completedOk ) );
				transitionName = SciTransitionName::run_pus;
				break;
			}

			case Control::Request::Type::stop:
			{
				RED_FATAL_ASSERT( req->GetStatus().m_status == ControlRequestStatus::Status::notstarted, "Invalid request: expected request that has not yet started." );
				RED_FATAL( "TODO: Implement ControlRequestType::stop handling." );
				break;
			}

			case Control::Request::Type::finish:
			{
				RED_FATAL_ASSERT( req->GetStatus().m_status == ControlRequestStatus::Status::notstarted, "Invalid request: expected request that has not yet started." );
#ifdef DEBUG_SCN_TOKENS
				SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ), GEngine->GetCurrentEngineTick() );
#else
				SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ) );
#endif // DEBUG_SCN_TOKENS
				cancelTokenBroadcast.PushBack( cancelToken );

				req->SetStatus( ControlRequestStatus( ControlRequestStatus::Status::completedOk ) );
				transitionName = SciTransitionName::run_fin;
				break;
			}

			case Control::Request::Type::terminate:
			{
				RED_FATAL_ASSERT( req->GetStatus().m_status == ControlRequestStatus::Status::notstarted, "Invalid request: expected request that has not yet started." );
#ifdef DEBUG_SCN_TOKENS
				SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ), GEngine->GetCurrentEngineTick() );
#else
				SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ) );
#endif // DEBUG_SCN_TOKENS
				cancelTokenBroadcast.PushBack( cancelToken );

				req->SetStatus( ControlRequestStatus( ControlRequestStatus::Status::completedOk ) );
				transitionName = SciTransitionName::run_dsp;
				break;
			}

			default:
				// Ignore request we can't handle. "Running" transitions spontaneously to next state - that state may be able to handle the request.
				break;
			}
		}

		scInst.m_monitor.NoticeTickStart( timeWindow );

		// We do this before processing choice section nodes, making sure we use up to date persistent choice data in processing of choice section node.
		scInst.GetChoiceNodesMemories( scInst.m_choiceManager.GetChoiceNodesMemories() );

		scInst.m_execContext.Update();
		scInst.m_processingContext.Update( scInst.m_execContext.GetLocResolver().GetResolutionContext() );

		RED_ASSERT( ( scInst.m_info.m_sstRunsessionTimestamp == constants::unknownTime ) == ( scInst.m_info.m_sitRunsessionTimestamp == constants::unknownTime ), "Expected both timestamps to be set or both timestamps to be not set." );
		if ( const Bool runsessionStarts = scInst.m_info.m_sstRunsessionTimestamp == constants::unknownTime )
		{
			scInst.OpenRunsession( timeWindow.m_start );
		}

		scInst.m_clock.ProgressTime( dt );

		scInst.m_exestreamReconstructor.NoticeTimeProgression( scInst.m_clock.GetTimeWindow() );
		scInst.m_info.m_sitClock = scInst.m_clock.m_timeWindow.m_end;

		// #AitorTejedor #fastforward disable runsession timestamp assert in preview as clock is not being progressed when playhead moves, so there's no proper support. It's still valid in game.
		// #GiuseppeMarano #fastforward disable runsession timestamp assert when the sync to music is enabled as the scene clock will run at a different speed (fast forward is forbidden in this situation).
		RED_ASSERT( scInst.GetExecutionContext().GetGameInstance().GetRuntimeInfo().IsGamePreview() || scInst.IsSyncToMusicEnabled() || scInst.m_info.m_sstRunsessionTimestamp + scInst.m_clock.m_timeWindow.m_start == timeWindow.m_start + scInst.m_info.m_sitRunsessionTimestamp,
			"Expected timings to match, otherwise Fast Forwarding will not work, scene: %hs, sstRunsessionTimestamp: %u, sitRunsessionTimestamp: %u, sstWindowStart: %u, sitWindowStart: %u",
			scInst.GetSceneId().GetResourcePath().ToDebugString(), scInst.m_info.m_sstRunsessionTimestamp.m_stu, scInst.m_info.m_sitRunsessionTimestamp.m_stu, timeWindow.m_start.m_stu, scInst.m_clock.m_timeWindow.m_start.m_stu );

		for ( FastForwardTimeline& ffTimeline : scInst.m_ffTimelines )
		{
			ffTimeline.RemoveItemsBefore( scInst.m_clock.GetTimeWindow().m_start );
		}

		ProcessTokens( dt, scInst, cancelTokenBroadcast );

		// Generate pending stimulations.
		for ( const SignalToken& signalToken : scInst.m_sigTokensInactive )
		{
			const SceneTime nextTickStart = scInst.m_clock.GetTimeWindow().m_end;
			const Stimulation stimulation( signalToken.GetNodeId(), SystemNodepoints::start, signalToken.GetInputSocketStamp() );
			scInst.m_exestreamReconstructor.NoticePendingStimulation( ExestreamReconstructor::StimulationReport( nextTickStart, stimulation ) );
		}

		scInst.ShrinkBuffers();

		scInst.m_circumstancesTracker.DevourExestream( scInst.m_clock.GetTimeWindow() );
		scInst.m_circumstancesTracker.UpdateTrackingState( scInst.m_clock.GetTimeWindow(), scInst.GetExecutionTagManager() );
		scInst.m_circumstancesTracker.NotifyInspectionServices();

		scInst.m_signalProber.Probe();
		scInst.m_resourcePreloader.UpdateResources();

		scInst.m_multiSpeakerLineExecManager.Update( scInst.GetExecutionContext() );

		const Interruptor::SignalGeneration signalGeneration = scInst.m_interruptor.OnUpdate( dt );

		signalGeneration.ProduceSignals( scInst );

		if ( scInst.m_interruptor.GetInterruptionPhase() == InterruptionPhase::WaitForInterrupted )
		{
			if ( transitionName == SciTransitionName::null )
			{
				transitionName = SciTransitionName::run_itr;
			}
		}

		// Clients count ending the scene as a sync node in a shared scene
		if ( !scInst.GetMultiplayerModule().IsClientAndSharedScene( scInst.GetSceneInstanceId() ) )
		{
			if ( transitionName == SciTransitionName::null )
			{
				// Instance if finished when there are no tokens (because all tokens are consumed by end nodes) and no Command-s.
				if ( const Bool sceneFinished = ( scInst.m_sigTokensInactive.Empty() && scInst.m_exeTokensInactive.Empty() && scInst.m_executor.GetNumCommands() == 0
					&& !scInst.m_stimuli.ContainsFutureStimuli( scInst.m_clock.GetTimeWindow().m_end ) ) )
				{
					transitionName = SciTransitionName::run_fin;
				}
			}
		}

		// We do this after processing choice section nodes, making sure we have up to date persistent choice data in scene system (that could be used by other scene instances).
		scInst.SetChoiceNodesMemories( scInst.m_choiceManager.GetChoiceNodesMemories() );

		scInst.GetEffectManager().PruneInvalidEffects();

		scInst.m_monitor.NoticeTickEnd();

		return transitionName;
	}

	/*
	Activates tokens.

	\param dt Time delta.

	During activation, tokens are assigned time budgets.
	*/
	void SciStateRun::ActivateTokens( SceneTime dt, SceneInstance& scInst )
	{
		// Activate entry points

		// #szi_todo_save Checklist: After loading scene instance, make sure clock is properly restored. Also, this way we won't activate stimuli again.
		const TimeWindow timeWindow = scInst.m_clock.GetTimeWindow();

		scInst.m_stimuli.RemoveStimuli( timeWindow, [ &scInst ]( const Stimulus& stimulus )
		{
			const SceneResource& scDef = scInst.GetSceneManager().GetSceneResource( scInst.m_sceneHandle );
			CName entryPointName = ( stimulus.m_entryPoint == Stimulus::anyEntryPoint ) ? scDef.GetFirstEntryPointName() : stimulus.m_entryPoint;
			EntryPoint entryPoint = scDef.GetEntryPoint( entryPointName );

			RED_DATA_ERROR_BEGIN( red::LoggerCategory_Scenes, entryPoint != EntryPoint::invalid, "Expected entry point '%hs' to exist in scene '%hs'.", entryPointName.AsChar(), scDef.GetPath().ToDebugString() );
			RED_DATA_ERROR_ACTION_SHOW_ASSET( scDef.GetPath().ToDebugString() );
			RED_DATA_ERROR_END();

			if ( entryPoint != EntryPoint::invalid )
			{
#ifdef DEBUG_SCN_TOKENS
				scInst.m_exeTokensInactive.PushBack( ExecutionToken( entryPoint.m_nodeId, TokenState::inactive, SceneTime( 0 ), false, NodeProcessingState(), GEngine->GetCurrentEngineTick() ) );
#else
				scInst.m_exeTokensInactive.PushBack( ExecutionToken( entryPoint.m_nodeId, TokenState::inactive, SceneTime( 0 ), false ) );
#endif // DEBUG_SCN_TOKENS
			}

			const Bool continueIteration = true;
			return continueIteration;
		} );

		// TODO: Here goes the logic that determines which tokens to activate. For now, it's ok activate all of them.
		// Think about where this "activation logic" is actually placed - "activation counters/timers" will be placed
		// in tokens themselves so tokens themselves should know whether they become active when they instance asks
		// them to activate. On the other hand its the scene instance that should decide about token activation.
		// Solution - scene instances sets up the tokens so it may also set up activation counters/timers. From now
		// on, tokens themselves are responsible for managing their activation according to activation counters/timers.
		// Of course, scene instance may change activation counters/timers settings.

		// Assert there are no active tokens. If there are, previous tick wouldn't finish and we wouldn't be here.
		RED_FATAL_ASSERT( scInst.m_sigTokensActive.Empty() && scInst.m_exeTokensActive.Empty(), "" );

		scInst.m_sigTokensActive.Reserve( scInst.m_sigTokensInterrupted.Size() + scInst.m_sigTokensInactive.Size() );
		scInst.m_exeTokensActive.Reserve( scInst.m_exeTokensInactive.Size() );

		// Send activation signals to all tokens and move all activated tokens to list of active tokens.
		// Note: we're processing the list in backward direction which means we're changing the order of
		// tokens. We want to have stable and predictable order in which tokens are executed but we'll
		// achieve that by sorting the tokens according to some priorities. We don't want to care about
		// order of tokens here as we still couldn't guarantee any specific order - that's because tokens
		// will get generated/consumed etc.

		for ( SignalToken& token : scInst.m_sigTokensInactive )
		{
			const Bool activated = token.ProcessActivation( dt );
			RED_ASSERT( activated, "Update the logic!" );
		}

		scInst.m_sigTokensActive.Swap( scInst.m_sigTokensInactive );

		// during interruption state we could restive signals from commands which have to be consumed in first run state after interruption
		
		if ( !scInst.m_sigTokensInterrupted.Empty() )
		{
			scInst.m_sigTokensActive.PushBack( scInst.m_sigTokensInterrupted );
			scInst.m_sigTokensInterrupted.Clear();
		}

		for ( ExecutionToken& token : scInst.m_exeTokensInactive )
		{
			const Bool activated = token.ProcessActivation( dt );
			RED_ASSERT( activated, "Update the logic!" );
		}

		scInst.m_exeTokensActive.Swap( scInst.m_exeTokensInactive );
	}

	void SciStateRun::ProcessTokens( SceneTime dt, SceneInstance& scInst, red::StaticArray< SignalToken, 1 >& cancelTokenBroadcast )
	{
		red::DynArray< NodeId > nodesToProcess{ PoolFrameSceneSystem() };
		red::DynArray< StateToken > producedStateTokens{ PoolFrameSceneSystem() };
		ProcessingResult processingResult{ PoolFrameSceneSystem() };

		ActivateTokens( dt, scInst );

		Bool doContinueStepping = false;
		auto continueStepping = [ &cancelTokenBroadcast, &scInst, &doContinueStepping ]()
		{
			const Bool activeTokensExist = !scInst.m_sigTokensActive.Empty() || !scInst.m_exeTokensActive.Empty() || !cancelTokenBroadcast.Empty();
			const Bool pendingCommandMessages = scInst.m_executor.GetNumMessages() > 0;

			return doContinueStepping || activeTokensExist || pendingCommandMessages;
		};

		while ( continueStepping() )
		{
			doContinueStepping = false;
			scInst.m_monitor.NoticeStepStart();

			// #szi_todo_fastforwarding Currently, we assume each operation is performed at the beginning of tick. It's ok for now but we should improve this.
			auto performFFOperation = [ &scInst ]( FastForwardMode ffMode, FastForwardTimelineOperation op )
			{
				RED_ASSERT( scInst.ParticipatesInFastForwarding() );

				const scn::SceneTime operationTime = scInst.m_clock.GetTimeWindow().m_start;

				// Convert from node time space to scene instance time space.
				op.m_timerange.m_start += operationTime;
				op.m_timerange.m_end += operationTime;

				FastForwardTimeline& ffTimeline = scInst.GetFastForwardTimeline( ffMode );
				ffTimeline.PerformOperation( op, scInst.GetSceneInstanceId() );
			};

			auto fundProcessFFTimelineOperations = [doPerform = scInst.ParticipatesInFastForwarding(), &performFFOperation]( const FastForwardMode mode, const Command::Result::FastForwardingOperations& operations )
			{
				if ( doPerform )
				{
					for ( const FastForwardTimelineOperation& op : operations )
					{
						performFFOperation( mode, op );
					}
				}
			};

			auto funcProcessSignals = [ &scInst, dt ]( red::ArraySpan< const Signal > signals )
			{
				for ( const Signal& signal : signals )
				{
					RED_ASSERT( signal != Signal::Invalid(), "Expected valid signal." );

					const SceneGraphNode& currNode = scInst.GetSceneDefinition().GetGraphNode( signal.m_osockId.m_nodeId );
					currNode.GenerateOutputSignalTokens( scInst.m_sigTokensActive, signal.m_osockId.m_osockStamp, TokenState::active, SceneTime( dt ) );

					SignalToken token( InputSocketId( signal.m_osockId.m_nodeId, InputSocketStamp( SystemSockets::produceTrigger, 0 ) ), TokenState::active, SceneTime( dt ) );
					SetTokenData( token, TokenData< TokenDataType::produceTriggerData >( signal.m_osockId, signal.m_triggerOrigin, signal.m_triggerType ) );
					scInst.m_sigTokensActive.PushBack( std::move( token ) );
				}
			};

			auto funcProcessCompletion = [ &scInst, dt ]( Completion completion )
			{
				SignalToken token( InputSocketId( completion.m_invoker, InputSocketStamp( SystemSockets::commandCompleted, 0 ) ), TokenState::active, SceneTime( dt ) );
				SetTokenData( token, TokenData< TokenDataType::completionData >( completion.m_data ) );
				scInst.m_sigTokensActive.PushBack( std::move( token ) );
			};

			scInst.m_executor.ProcessCommands( scInst.m_execContext, funcProcessCompletion, funcProcessSignals, fundProcessFFTimelineOperations );

			// Usage of static arrays is pure optimization. It's faster to use static arrays.

			static constexpr Uint32 c_sigTokensBuffersSize = 512;
			static constexpr Uint32 c_exeTokensBuffersSize = 256;
			static constexpr Uint32 c_eventsBuffersSize = 256;

			struct
			{
				red::StaticArray< SignalToken, c_sigTokensBuffersSize > m_sigTokensActive;
				red::StaticArray< SignalToken, c_sigTokensBuffersSize > m_sigTokensInactive;
				red::StaticArray< ExecutionToken, c_exeTokensBuffersSize > m_exeTokensActive;
				red::StaticArray< ExecutionToken, c_exeTokensBuffersSize > m_exeTokensInactive;
				red::StaticArray< InstanceEvent, c_eventsBuffersSize > m_instanceEvents;

			} stepBuffers;

			const Uint32 maxNodesToProcess = scInst.m_sigTokensActive.Size() + scInst.m_sigTokensInactive.Size() + scInst.m_exeTokensActive.Size() + scInst.m_exeTokensInactive.Size() + scInst.m_stateTokens.Size();

			nodesToProcess.Clear();
			nodesToProcess.Reserve( maxNodesToProcess );

			producedStateTokens.Clear();
			producedStateTokens.Reserve( maxNodesToProcess );

			// Sort tokens by NodeId.
			std::sort( scInst.m_sigTokensActive.Begin(), scInst.m_sigTokensActive.End(), []( const SignalToken& a, const SignalToken& b ) { return a.GetNodeId() < b.GetNodeId(); } );
			std::sort( scInst.m_sigTokensInactive.Begin(), scInst.m_sigTokensInactive.End(), []( const SignalToken& a, const SignalToken& b ) { return a.GetNodeId() < b.GetNodeId(); } );
			std::sort( scInst.m_exeTokensActive.Begin(), scInst.m_exeTokensActive.End(), []( const ExecutionToken& a, const ExecutionToken& b ) { return a.GetNodeId() < b.GetNodeId(); } );
			std::sort( scInst.m_exeTokensInactive.Begin(), scInst.m_exeTokensInactive.End(), []( const ExecutionToken& a, const ExecutionToken& b ) { return a.GetNodeId() < b.GetNodeId(); } );
			std::sort( scInst.m_stateTokens.Begin(), scInst.m_stateTokens.End(), []( const StateToken& a, const StateToken& b ) { return a.GetNodeId() < b.GetNodeId(); } );

			// Create list of nodes with any tokens sitting on them.
			// Note: We're okay with the same node appearing many times in the list.
			ListNodesWithTokens< StateToken >( nodesToProcess, scInst.m_stateTokens );
			ListNodesWithTokens< SignalToken >( nodesToProcess, scInst.m_sigTokensActive );
			ListNodesWithTokens< SignalToken >( nodesToProcess, scInst.m_sigTokensInactive );
			ListNodesWithTokens< ExecutionToken >( nodesToProcess, scInst.m_exeTokensActive );
			ListNodesWithTokens< ExecutionToken >( nodesToProcess, scInst.m_exeTokensInactive );
			std::sort( nodesToProcess.Begin(), nodesToProcess.End() );

			// Broadcast tokens to all nodes with tokens.
			if ( const Bool broadcastTokens = !cancelTokenBroadcast.Empty() )
			{
				auto itNodeCurr = nodesToProcess.Begin();
				const auto itNodeLast = nodesToProcess.End();

				while ( itNodeCurr != itNodeLast )
				{
					const NodeId currNodeId = *itNodeCurr;

					for ( SignalToken& tokenTemplate : cancelTokenBroadcast )
					{
						// Rewrite token so it's placed on proper node.
#ifdef DEBUG_SCN_TOKENS
						const SignalToken token( InputSocketId( currNodeId, tokenTemplate.GetInputSocketStamp() ), tokenTemplate.GetState(), tokenTemplate.GetTimeBudget(), tokenTemplate.GetDbgEngineTick() );
#else
						const SignalToken token( InputSocketId( currNodeId, tokenTemplate.GetInputSocketStamp() ), tokenTemplate.GetState(), tokenTemplate.GetTimeBudget() );
#endif // DEBUG_SCN_TOKENS
						RED_FATAL_ASSERT( tokenTemplate.GetState() == TokenState::active, "We assume that only active signal tokens may be broadcast." );

						scInst.m_sigTokensActive.PushBack( token );
					}
					
					itNodeCurr = std::upper_bound( itNodeCurr, itNodeLast, currNodeId );
				}

				std::sort( scInst.m_sigTokensActive.Begin(), scInst.m_sigTokensActive.End(), [](const SignalToken& a, const SignalToken& b) { return a.GetNodeId() < b.GetNodeId(); });

				cancelTokenBroadcast.Clear();
			}

			auto itCurr = nodesToProcess.Begin();
			const auto itLast = nodesToProcess.End();

			auto itCurrSigToken = scInst.m_sigTokensActive.Begin();
			const auto itLastSigToken = scInst.m_sigTokensActive.End();

			auto itCurrSigTokenInactive = scInst.m_sigTokensInactive.Begin();
			const auto itLastSigTokenInactive = scInst.m_sigTokensInactive.End();

			auto itCurrExeToken = scInst.m_exeTokensActive.Begin();
			const auto itLastExeToken = scInst.m_exeTokensActive.End();

			auto itCurrExeTokenInactive = scInst.m_exeTokensInactive.Begin();
			const auto itLastExeTokenInactive = scInst.m_exeTokensInactive.End();

			auto itCurrStateToken = scInst.m_stateTokens.Begin();
			const auto itLastStateToken = scInst.m_stateTokens.End();

			// Call node function for all nodes with tokens.
			while (itCurr != itLast)
			{
				const NodeId currNodeId = *itCurr;

				// Gather active signal tokens placed on current node.
				auto itLowbSigToken = std::lower_bound( itCurrSigToken, itLastSigToken, currNodeId, []( const SignalToken& sigToken, NodeId nodeId ) { return sigToken.GetNodeId().m_id < nodeId.m_id; } );
				auto itUppbSigToken = std::upper_bound( itLowbSigToken, itLastSigToken, currNodeId, []( NodeId nodeId, const SignalToken& sigToken ) { return nodeId.m_id < sigToken.GetNodeId().m_id; } );
				SignalToken* sigRangeBegin = scInst.m_sigTokensActive.TypedData() + ( itLowbSigToken - scInst.m_sigTokensActive.Begin() );
				SignalToken* sigRangeEnd = scInst.m_sigTokensActive.TypedData() + ( itUppbSigToken - scInst.m_sigTokensActive.Begin() );
				red::ArraySpan< SignalToken > sigTokens( sigRangeBegin, sigRangeEnd );

				// Gather inactive signal tokens placed on current node.
				auto itLowbSigTokenInactive = std::lower_bound( itCurrSigTokenInactive, itLastSigTokenInactive, currNodeId, []( const SignalToken& sigToken, NodeId nodeId ) { return sigToken.GetNodeId().m_id < nodeId.m_id; } );
				auto itUppbSigTokenInactive = std::upper_bound( itLowbSigTokenInactive, itLastSigTokenInactive, currNodeId, []( NodeId nodeId, const SignalToken& sigToken ) { return nodeId.m_id < sigToken.GetNodeId().m_id; } );
				SignalToken* sigInactiveRangeBegin = scInst.m_sigTokensInactive.TypedData() + ( itLowbSigTokenInactive - scInst.m_sigTokensInactive.Begin() );
				SignalToken* sigInactiveRangeEnd = scInst.m_sigTokensInactive.TypedData() + ( itUppbSigTokenInactive - scInst.m_sigTokensInactive.Begin() );
				red::ArraySpan< SignalToken > sigTokensInactive( sigInactiveRangeBegin, sigInactiveRangeEnd );

				// Gather active execution tokens placed on current node.
				auto itLowbExeToken = std::lower_bound( itCurrExeToken, itLastExeToken, currNodeId, []( const ExecutionToken& exeToken, NodeId nodeId ) { return exeToken.GetNodeId().m_id < nodeId.m_id; } );
				auto itUppbExeToken = std::upper_bound( itLowbExeToken, itLastExeToken, currNodeId, []( NodeId nodeId, const ExecutionToken& exeToken ) { return nodeId.m_id < exeToken.GetNodeId().m_id; } );
				ExecutionToken* exeRangeBegin = scInst.m_exeTokensActive.TypedData() + ( itLowbExeToken - scInst.m_exeTokensActive.Begin() );
				ExecutionToken* exeRangeEnd = scInst.m_exeTokensActive.TypedData() + ( itUppbExeToken - scInst.m_exeTokensActive.Begin() );
				red::ArraySpan< ExecutionToken > exeTokens( exeRangeBegin, exeRangeEnd );

				// Gather inactive execution tokens placed on current node.
				auto itLowbExeTokenInactive = std::lower_bound( itCurrExeTokenInactive, itLastExeTokenInactive, currNodeId, []( const ExecutionToken& exeToken, NodeId nodeId ) { return exeToken.GetNodeId().m_id < nodeId.m_id; } );
				auto itUppbExeTokenInactive = std::upper_bound( itLowbExeTokenInactive, itLastExeTokenInactive, currNodeId, []( NodeId nodeId, const ExecutionToken& exeToken ) { return nodeId.m_id < exeToken.GetNodeId().m_id; } );
				ExecutionToken* exeInactiveRangeBegin = scInst.m_exeTokensInactive.TypedData() + ( itLowbExeTokenInactive - scInst.m_exeTokensInactive.Begin() );
				ExecutionToken* exeInactiveRangeEnd = scInst.m_exeTokensInactive.TypedData() + ( itUppbExeTokenInactive - scInst.m_exeTokensInactive.Begin() );
				red::ArraySpan< ExecutionToken > exeTokensInactive( exeInactiveRangeBegin, exeInactiveRangeEnd );

				// Gather state tokens placed on current node.
				auto itLowbStateToken = std::lower_bound( itCurrStateToken, itLastStateToken, currNodeId, []( const StateToken& stateToken, NodeId nodeId ) { return stateToken.GetNodeId().m_id < nodeId.m_id; } );
				auto itUppbStateToken = std::upper_bound( itLowbStateToken, itLastStateToken, currNodeId, []( NodeId nodeId, const StateToken& stateToken ) { return nodeId.m_id < stateToken.GetNodeId().m_id; } );
				StateToken* stateRangeBegin = scInst.m_stateTokens.TypedData() + ( itLowbStateToken - scInst.m_stateTokens.Begin() );
				StateToken* stateRangeEnd = scInst.m_stateTokens.TypedData() + ( itUppbStateToken - scInst.m_stateTokens.Begin() );
				red::ArraySpan< StateToken > stateTokens( stateRangeBegin, stateRangeEnd );

				// Update iterators for next iteration.
				itCurr = std::upper_bound( itCurr, itLast, currNodeId );
				itCurrSigToken = itUppbSigToken;
				itCurrSigTokenInactive = itUppbSigTokenInactive;
				itCurrExeToken = itUppbExeToken;
				itCurrExeTokenInactive = itUppbExeTokenInactive;
				itCurrStateToken = itUppbStateToken;

				if ( const Bool activeTokenInArgs = !sigTokens.Empty() || !exeTokens.Empty() )
				{
					const SceneGraphNode& currNode = scInst.GetSceneDefinition().GetGraphNode( currNodeId );

					auto result = scInst.m_sceneInstanceState.m_guState.AccessNodeAccount( currNodeId );
					if ( result.m_isNewAccount )
					{
						currNode.ResetMemories( result.m_nodeAccount );
					}

					processingResult.Reset();
					
					// Call node function.
					ProcessingArgs processingArgs( sigTokens, sigTokensInactive, exeTokens, exeTokensInactive, stateTokens );
					currNode.Process( processingResult, processingArgs, result.m_nodeAccount, scInst.m_processingContext, scInst.m_execContext );
					if ( processingResult.GetNodeState() != NodeState::notchanged && NpLogOn() ) // [nomadplay]
					{
						std::string npCls = currNode.GetClass()->GetName().AsChar();
						if ( processingResult.GetNodeState() == NodeState::inprogress && currNode.IsA< QuestNode >() )
						{
							if ( const auto qn = static_cast< const QuestNode& >( currNode ).GetNodeDefinition() )
							{
								npCls += std::string( " -> " ) + qn->GetClass()->GetName().AsChar();
								std::string d = qn->GetDescription().AsChar();
								for ( char& c : d )
								{
									if ( c == '\n' || c == '\r' || c == '\t' )
									{
										c = ' ';
									}
								}
								npCls += " '" + d.substr( 0, 200 ) + "'";
							}
						}
						NpSceneNode( &scInst, Uint32( currNodeId.m_id ), npCls.c_str(), processingResult.GetNodeState() );
					}
					if ( NpLogOn() ) NpSceneWaitCheck(); // [nomadplay]

					// TODO: This may be done better. We should store node state (active, finished etc.) in state token and we should inspect that state token here.

					// #szi_todo_exestream Properly track node activity time windows. Using scInst.m_clock.GetTimeWindow() is unacceptably inaccurate.
					// We should probably add occurrence time to SignalToken. This way we'd have enough information to properly set timing information
					// for ActionRecords and stimulations.

					TimeWindow realTwActivity;
					if ( !processingResult.GetExecutionTokens().Empty() )
					{
						realTwActivity = processingResult.GetExecutionTokens().Front().GetNodeProcessingState().m_realTwActivity;
					}

					const Bool nodeActivityDetected = processingResult.GetNodeState() != NodeState::notchanged;
					if ( nodeActivityDetected )
					{
						scInst.m_monitor.NoticeNodeProcessed( currNodeId, processingResult.GetNodeState(), realTwActivity );
					}

					const TimeWindow twActivity = scInst.m_clock.GetTimeWindow();
					RED_ASSERT( realTwActivity.m_start <= twActivity.m_start, "Node processing start time should always be lower or equal than scene processing time." );
					scInst.m_exestreamReconstructor.NoticeNodeActivity( ExestreamReconstructor::ActivityReport( currNodeId, twActivity, realTwActivity, processingResult.GetTriggers(), processingResult.GetStimulations() ) );

					if ( scInst.m_supervisor )
					{
						scInst.m_supervisor->ProcessExecutionResult( scInst.GetSceneInstanceId(), currNodeId, processingResult );
					}

					processingResult.IterateInputs( [ &currNode, &stepBuffers ]( const Input& input )
					{
						auto& targetList = ( input.m_sigToken.GetState() == TokenState::active ) ? stepBuffers.m_sigTokensActive : stepBuffers.m_sigTokensInactive;
						targetList.PushBack( currNode.GenerateInputSignalToken( input ) );
					} );

					// Process outputs produced by node function and collect SignalToken-s produced by output sockets.
					processingResult.IterateOutputs( [ &scInst, &currNode, &stepBuffers ]( const Output& output )
					{
						SignalToken sigToken = output.m_sigToken;
						if ( !scInst.m_monitor.RequestContinuationPermit() )
						{
							sigToken.Deactivate();
						}

						auto& targetList = ( sigToken.GetState() == TokenState::active ) ? stepBuffers.m_sigTokensActive : stepBuffers.m_sigTokensInactive;

						Uint32 index = targetList.Size();
						currNode.GenerateOutputSignalTokens( targetList, output.m_osockStamp, sigToken.GetState(), sigToken.GetTimeBudget() );

						// Don't send out signals on client if it is a sync node and a shared scene
						if ( currNode.IsSyncNodeType() && scInst.GetMultiplayerModule().IsClientAndSharedScene( scInst.GetSceneInstanceId() ) )
						{
							targetList.RemoveAt( index, targetList.Size() );
						}

						// Add signal tokens on server to multiplayer module
						if ( currNode.IsSyncNodeType() && scInst.GetMultiplayerModule().IsServerAndSharedScene( scInst.GetSceneInstanceId() ) )
						{
							red::ArraySpan< SignalToken > tokens{ targetList };
							scInst.GetMultiplayerModule().AddSignalTokens( scInst.GetSceneInstanceId(), tokens.SubSpan( index, targetList.Size() - index ) );
						}
					} );

					// Collect ExecutionToken-s produced by node function.
					for ( ExecutionToken& exeToken : processingResult.GetExecutionTokens() )
					{
						auto& targetList = ( exeToken.GetState() == TokenState::active ) ? stepBuffers.m_exeTokensActive : stepBuffers.m_exeTokensInactive;
						targetList.PushBack( std::move( exeToken ) );
					}

					// Collect StateToken-s produced by node function.
					producedStateTokens.PushBack( processingResult.GetStateTokens() );

					// Collect InstanceEvent-s produced by node function.
					stepBuffers.m_instanceEvents.PushBack( processingResult.GetInstanceEvents() );

					// Process fast-forwarding operations.
					if ( scInst.ParticipatesInFastForwarding() )
					{
						processingResult.IterateFastForwardTimelineOperations( performFFOperation );
					}

					// Execute commands.
					for ( CommandPtr& command : processingResult.GetCommands() )
					{
						const Command::Result initialResult = scInst.m_executor.ExecuteCommand( std::move( command ), scInst.m_execContext );
						if ( !initialResult.m_signals.Empty() || !initialResult.m_completion.Empty() )
						{
							doContinueStepping = true;
						}
					}

					scInst.m_signalProber.NoticeProgression( ProgressionRaport{ processingResult.GetStimulations(), currNode } );

					// Execute actions.
					// Collector and Executor might be part of SciStateRun (after we implement per-instance states). Executor itself might also be part of scene system
					// (for this it mustn't contain any per-instance state).
					scInst.m_collector.AddActionParts( processingResult.GetGeneratedActionParts() );

					const ActionPartFilter filter{ currNode, scInst.m_execContext, scInst.m_actorManager };
					scInst.m_collector.ApplyFilter( filter, scInst.m_memoryControl.GetActionPartAlloc() );

					scInst.m_executor.ExecuteActions( scInst.m_collector, scInst.m_execContext );
					// release action part memory
					scInst.m_memoryControl.GetActionPartAlloc().DeleteObjects( scInst.m_collector.GetActionParts() );
					scInst.m_collector.Clear();
				}
				else
				{
					stepBuffers.m_sigTokensInactive.PushBack( sigTokensInactive );
					stepBuffers.m_exeTokensInactive.PushBack( exeTokensInactive );
					producedStateTokens.PushBack( stateTokens );
				}
			}

			HandleInstanceEvents( dt, scInst, stepBuffers.m_instanceEvents, cancelTokenBroadcast );

			// Previously existing tokens are already processed and in zombie state so we remove them.
			scInst.m_sigTokensActive.Clear();
			scInst.m_sigTokensInactive.Clear();
			scInst.m_exeTokensActive.Clear();
			scInst.m_exeTokensInactive.Clear();
			scInst.m_stateTokens.Clear();

			// Schedule newly produced tokens to be processed next iteration.

			scInst.m_sigTokensActive.PushBack( stepBuffers.m_sigTokensActive );
			scInst.m_sigTokensInactive.PushBack( stepBuffers.m_sigTokensInactive );
			scInst.m_exeTokensActive.PushBack( stepBuffers.m_exeTokensActive );
			scInst.m_exeTokensInactive.PushBack( stepBuffers.m_exeTokensInactive );
			scInst.m_stateTokens.PushBack( producedStateTokens );

			scInst.m_monitor.NoticeStepEnd();
		}

		RED_ASSERT( scInst.m_sigTokensActive.Empty(), "Active token detected at end of tick." );
		RED_ASSERT( scInst.m_exeTokensActive.Empty(), "Active token detected at end of tick." );
	}

	void SciStateRun::HandleInstanceEvents( SceneTime dt, SceneInstance& scInst, red::ArraySpan< InstanceEvent > instanceEvents, red::StaticArray< SignalToken, 1 >& cancelTokenBroadcast )
	{
		// Handle instance events.
		for ( InstanceEvent& ev : instanceEvents )
		{
			switch ( ev.m_type )
			{
				case InstanceEventType::entryPoint:
				{
					const InstanceEventData< InstanceEventType::entryPoint > evData( ev );

					const EntryPoint entryPoint = scInst.GetSceneDefinition().GetEntryPoint( evData.m_entryPointNodeId );
					RED_FATAL_ASSERT( entryPoint != EntryPoint::invalid, "Expected valid entry point." );

					// Handle event by creating scene notification.
					SceneNotification notification( scInst.GetSceneInstanceId(), scInst.GetSceneId(), SceneNotificationType::instanceEntryPoint );
					notification.m_data.m_instanceEntryPoint.m_entryPoint = entryPoint.m_name;
					notification.m_data.m_valid = true;
					scInst.m_notificationQueue.PushBack( notification );

					// TODO: Consider whether we should make this state immediately observable to external parties or whether we should publish it
					// later, at the end of scene instance tick, at the same time when notifications are published. As inspection record pertains
					// to all instances of the same scene (not only to one scene instance), we may even consider publishing inspection record after
					// all those instances are ticked.
					scInst.m_execContext.GetInspectionServices().NoticeEntryPointActivated( scInst.GetSceneId(), entryPoint.m_name );

					break;
				}

				case InstanceEventType::exitPoint:
				{
					const InstanceEventData< InstanceEventType::exitPoint > evData( ev );

					const ExitPoint exitPoint = scInst.GetSceneDefinition().GetExitPoint( evData.m_exitPointNodeId );
					RED_FATAL_ASSERT( exitPoint != ExitPoint::invalid, "Expected valid exit point." );

					// Handle event by creating scene notification.
					SceneNotification notification( scInst.GetSceneInstanceId(), scInst.GetSceneId(), SceneNotificationType::instanceExitPoint );
					notification.m_data.m_instanceExitPoint.m_exitPoint = exitPoint.m_name;
					notification.m_data.m_valid = true;
					scInst.m_notificationQueue.PushBack( notification );

					// TODO: Consider whether we should make this state immediately observable to external parties or whether we should publish it
					// later, at the end of scene instance tick, at the same time when notifications are published. As inspection record pertains
					// to all instances of the same scene (not only to one scene instance), we may even consider publishing inspection record after
					// all those instances are ticked.
					scInst.m_execContext.GetInspectionServices().NoticeExitPointActivated( scInst.GetSceneId(), exitPoint.m_name );

					break;
				}

				case InstanceEventType::notablePoint:
				{
					const InstanceEventData< InstanceEventType::notablePoint > evData( ev );
					const SceneGraphNode& sceneGraphNode = scInst.GetSceneDefinition().GetGraphNode( evData.m_notablePointNodeId );
					const Bool isRewindableSection = sceneGraphNode.IsA< RewindableSectionNode >();
					const Bool isSection = sceneGraphNode.IsA< SectionNode >();

					if ( isSection || isRewindableSection )
					{
						switch ( evData.m_eventKind )
						{
						case InstanceEventData< InstanceEventType::notablePoint >::EventKind::activation:
						{
							RED_ASSERT( scInst.m_nodeRunstateStorage.IsSectionRunstateExist( evData.m_notablePointNodeId ) );
							break;
						}

						case InstanceEventData< InstanceEventType::notablePoint >::EventKind::deactivation:
						{
							scInst.m_nodeRunstateStorage.RemoveSectionRunstate( evData.m_notablePointNodeId );
							break;
						}

						default:
							RED_FATAL( "Update the code" );
						}

						if ( isRewindableSection )
						{
							SceneNotification notification( scInst.GetSceneInstanceId(), scInst.GetSceneId(), SceneNotificationType::instanceRewindableNotablePoint );
							notification.m_data.m_valid = true;
							notification.m_data.m_instanceRewindableNotablePoint.m_eventKind = scInst.m_rewindableSectionNodeController.IsRewindableSectionActive() ?
								SceneNotificationData< SceneNotificationType::instanceRewindableNotablePoint >::EventKind::activation : SceneNotificationData< SceneNotificationType::instanceRewindableNotablePoint >::EventKind::deactivation;
							scInst.m_notificationQueue.PushBack( notification );
						}
					}

					const NotablePoint notablePoint = scInst.GetSceneDefinition().GetNotablePoint( evData.m_notablePointNodeId );
					if ( notablePoint != NotablePoint::invalid )
					{
						// Handle event by creating scene notification.
						SceneNotification notification( scInst.GetSceneInstanceId(), scInst.GetSceneId(), SceneNotificationType::instanceNotablePoint );
						notification.m_data.m_instanceNotablePoint.m_notablePoint = notablePoint.m_name;
						notification.m_data.m_instanceNotablePoint.m_eventKind = static_cast<SceneNotificationData< SceneNotificationType::instanceNotablePoint >::EventKind>( evData.m_eventKind );
						notification.m_data.m_valid = true;
						scInst.m_notificationQueue.PushBack( notification );

						// TODO: Consider whether we should make this state immediately observable to external parties or whether we should publish it
						// later, at the end of scene instance tick, at the same time when notifications are published. As inspection record pertains
						// to all instances of the same scene (not only to one scene instance), we may even consider publishing inspection record after
						// all those instances are ticked.
						if ( const Bool activation = evData.m_eventKind == InstanceEventData< InstanceEventType::notablePoint >::EventKind::activation )
						{
							scInst.m_execContext.GetInspectionServices().NoticeSectionStarted( scInst.GetSceneId(), notablePoint.m_name );
						}
						else // deactivation
						{
							scInst.m_execContext.GetInspectionServices().NoticeSectionFinished( scInst.GetSceneId(), notablePoint.m_name );
						}
					}

					break;
				}

				case InstanceEventType::cancelPoint:
				{
					if ( cancelTokenBroadcast.Empty() ) // cancel token already exist
					{
						// Handle event by broadcasting cancel token.
	#ifdef DEBUG_SCN_TOKENS
						SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ), GEngine->GetCurrentEngineTick() );
	#else
						SignalToken cancelToken( InputSocketId( NodeId::invalid, InputSocketStamp( SystemSockets::cancelExecution, 0 ) ), TokenState::active, SceneTime( dt ) );
	#endif // DEBUG_SCN_TOKENS

						cancelTokenBroadcast.PushBack( cancelToken );
					}

					RED_ASSERT( cancelTokenBroadcast[0].GetInputSocketStamp() == InputSocketStamp( SystemSockets::cancelExecution, 0 ), "Expecting cancel token" );

					break;
				}

				case InstanceEventType::nodeCanceled:
				{
					// Handle event by canceling command associated with canceled node.
					const InstanceEventData< InstanceEventType::nodeCanceled > evData( ev );
					scInst.m_executor.CancelCommand( evData.m_canceledNodeId, scInst.m_execContext );

					const SceneGraphNode& sceneGraphNode = scInst.GetSceneDefinition().GetGraphNode( evData.m_canceledNodeId );
					const Bool isRewindableSection = sceneGraphNode.IsA< RewindableSectionNode >();
					const Bool isSection = sceneGraphNode.IsA< SectionNode >();

					if ( isRewindableSection || isSection )
					{
						scInst.m_nodeRunstateStorage.RemoveSectionRunstate( evData.m_canceledNodeId );
					}

					if ( isRewindableSection )
					{
						SceneNotification notification( scInst.GetSceneInstanceId(), scInst.GetSceneId(), SceneNotificationType::instanceRewindableNotablePoint );
						notification.m_data.m_valid = true;
						notification.m_data.m_instanceRewindableNotablePoint.m_eventKind = scInst.m_rewindableSectionNodeController.IsRewindableSectionActive() ? SceneNotificationData< SceneNotificationType::instanceRewindableNotablePoint >::EventKind::activation : SceneNotificationData< SceneNotificationType::instanceRewindableNotablePoint >::EventKind::deactivation;
						scInst.m_notificationQueue.PushBack( notification );
					}

					break;
				}

				case InstanceEventType::reactivateChoiceGroup:
				{
					// Handle event by reactivating specified choice group.
					const InstanceEventData< InstanceEventType::reactivateChoiceGroup > evData( ev );
					scInst.m_execContext.GetChoiceManager().EnableChoiceGroup( evData.m_choiceGroup );

					break;
				}

				case InstanceEventType::endTimeLimitedFinish:
				{
					// Handle event by ending time limited finish on command invoked by event caller.
					const InstanceEventData< InstanceEventType::endTimeLimitedFinish > evData( ev );
					const NodeId nodeId( evData.m_nodeId );
					auto isInvoker = [ nodeId ]( CommandPtr& commandPtr ) { return nodeId == commandPtr->GetInvoker(); };
					auto tryEndTimeLimitedFinish = []( CommandPtr& commandPtr ) { static_cast<ChoiceNodeCommand*>( commandPtr.Get() )->TryEndTimeLimitedFinish(); };
					scInst.m_executor.DoCommandActionIf( isInvoker, tryEndTimeLimitedFinish );

					break;
				}

				case InstanceEventType::choiceHubSystemNotifyActivate:
				{
					// Handle event by notifying choice hub system about activation.
					const InstanceEventData< InstanceEventType::choiceHubSystemNotifyActivate > evData( ev );
					ChoiceHubSystem& choiceHubSystem = scInst.GetExecutionContext().GetChoiceHubSystem();
					choiceHubSystem.NotifyActivate( scInst.GetSceneInstanceId(), evData.m_choiceHubOwnerId );

					break;
				}

				default:
				{
					RED_FATAL( "Unexpected instance event." );
					break;
				}
			}
		}
	}
	
} // namespace scn
