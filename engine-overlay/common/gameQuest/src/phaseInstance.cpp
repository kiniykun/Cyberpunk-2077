#include "build.h"
#include "phaseInstance.h"
#include <atomic> // [watson]
#include <cstring> // [watson]
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include "startNode.h"
#include "inputNode.h"
#include "outputNode.h"
#include "phaseNode.h"
#include "deletionMarkerNode.h"
#include "eventManager.h"
#include "questDebugger.h"
#include "questPerformanceMonitor.h"
#include "questPrefabsHandler.h"
#include "questResourceLoader.h"
#include "questPhaseFreezer.h"
#include "questRuntimeHelper.h"
#include "phaseNodeQuestPrefabsListener.h"

#include "../../../common/gameCore/include/fdbInterface.h"
#include "../../renderDraw/include/fragmentBuilder.h"
#include "../../world/include/gameInstance.h"

#include "../../../common/gameCore/include/functionalTestsSystemInterface.h"



#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif


using red::String;

RTTI_BEGIN_TYPE_IN_NAMESPACE( PhaseInstance, quest );
	RTTI_PARENT_TYPE( ISerializable );
RTTI_END_TYPE();


namespace quest
{
	namespace
	{
		static String DebugGetFullPathString( const FullPath& fullPath )
		{
			String str;
			for ( const auto& value : fullPath )
			{
				if ( !str.Empty() )
				{
					str += " ";
				}
				str += ToStringDirect( value );
			}
			return str;
		}

		static const AnsiChar* DebugGetNodeClassName( const ExecutionContext& executionContext, GenericId nodeId )
		{
			if ( const auto* phase = executionContext.GetCurrentPhase() )
			{
				if ( phase->GetGraph() )
				{
					if ( const auto node = phase->FindNodeById( nodeId ) )
					{
						return node->GetClass()->GetName().AsChar();
					}
				}
			}
			return "<unknown>";
		}
	}

	///////////////////////////////////////////////////////////////////////////////////////////////
	//
	//
	//
	PathHash RunRequest::GetPathHash() const
	{
		return m_pathHash;
	}

	NodeId RunRequest::GetNodeId() const
	{
		return m_nodeId;
	}

	Uint32 RunRequest::GetInputSocketIndex() const
	{
		return m_inputSocketIndex;
	}

	ExecutionActionType RunRequest::GetActionType() const
	{
		return m_actionType;
	}

	///////////////////////////////////////////////////////////////////////////////////////////////
	//
	//
	//
	PhaseInstance::PhaseInstance()
	{
	}

	PhaseInstance::~PhaseInstance()
	{
	}

	PhaseResult PhaseInstance::Resume( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, Int32 inputSocketIndex, OutputSocketArray& outputSockets )
	{
		PhaseResult ret = PhaseResult::InProgress;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// user has been already notified about missing node in PhaseInstance::RecreatePhaseInstanceTreeAfterLoad
				// just return gracefully
				return PhaseResult::InProgress;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

			PushPhase( executionContext );

			THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( phaseNodeId ) );
			if ( !childInstance )
			{
				if ( CanInstanceNotExist( executionContext ) )
				{
					// if we get here, it's because someone awakes signals in phase that was not created
					// we need to handle some stuff before executing the node
					// PMTODO handle also prefabs here
#ifdef RED_ENABLE_QUEST_DEBUGGER
					dbg::QuestDebugger* debugger = executionContext.GetDebugger();
					if( debugger )
					{
						if( !debugger->OnBlockStarted( executionContext, phaseNode.Get() ) )
						{
							// probably we hit breakpoint
							// PMTODO what to do if someone awakes a signal in a node with breakpoint?
						}
					}
#endif //RED_ENABLE_QUEST_DEBUGGER

					auto phaseGraph = phaseNode->GetGraph();
					GAME_ASSERT( phaseGraph, Quests, "Phase graph doesn't exist" );

					childInstance = CreateChildInstance( executionContext, phaseNode->GetPhaseResource(), phaseGraph, phaseNodeId, phaseNode->GetFriendlyName() );
					
					phaseNode->HandleSaveLock( executionContext.GetNodeExecutionContext(), true );
					phaseNode->HandlePrefabLoading( executionContext.GetNodeExecutionContext() );

#ifdef RED_ENABLE_QUEST_DEBUGGER
					if( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
					{
						debugger->OnPhaseStarted( executionContext, childInstance.Get() );
					}
#endif //RED_ENABLE_QUEST_DEBUGGER
				}
				else
				{
					GAME_ASSERT( childInstance, Quests, "Phase instance doesn't exist" );
				}
			}

			ret = childInstance->Resume( executionContext, fullPath, nodeId, inputSocketIndex, outputSockets );

			if ( ret == PhaseResult::Finished )
			{
				// if Resume returns false, it means that signal went through the phase and left so child instance can be deleted and signal pushed further in this phase
				RemoveChildInstance( executionContext, childInstance, SignalKillingReason::PhaseLeft );
			}

			if ( ret == PhaseResult::Finished )
			{
#ifdef RED_ENABLE_QUEST_DEBUGGER
				// notify debugger about leaving a node (it's OnBlockFinished equivalent from NodeDefinition::Execute)
				if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
				{
					debugger->OnBlockFinished( executionContext, phaseNode.Get() );
				}
#endif
			}

			PopPhase( executionContext );

			if ( ret == PhaseResult::Finished || ret == PhaseResult::InProgressAndFinished )
			{
				GAME_ASSERT( phaseNode->IsA< PhaseNodeDefinition >(), Quests, "Unexpected node type, is %hs instead of %hs", phaseNode->GetClass()->GetName().AsChar(), ClassID< PhaseNodeDefinition >()->GetName().AsChar() );
				GAME_ASSERT( outputSockets.Size() == 1, Quests, "Phase node can have only one output" );
				// send outcoming signal from the phase node
				return RunFromNode( executionContext, weakNode, outputSockets.GetFront(), false, outputSockets );
			}
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// user has been already notified about missing node in PhaseInstance::RecreatePhaseInstanceTreeAfterLoad
				// just return gracefully
				return PhaseResult::InProgress;
			}

			auto node = weakNode.ToHandle();
			if ( executionContext.GetActionType() == ExecutionActionType::Execution )
			{
				// if this is a regular execution (not a debugger command), the node should be SignalStoppingNodeDefinition
				GAME_ASSERT( node->IsA< SignalStoppingNodeDefinition >(), Quests, "Node is not a signal-stopping node" );
			}

			CName socketName;
			Bool isIncomingSignal = false;

			if ( inputSocketIndex > -1 )
			{
				auto socket = Cast< SocketDefinition >( node->GetSocketHandleByIndex( inputSocketIndex ) );
				GAME_ASSERT( socket, Quests, "Socket not found" );
				socketName = socket->GetName();
				isIncomingSignal = ( socket->GetType() != SocketType::Output );
			}
			else
			{
				socketName = NOTHING_SOCKET_NAME;
				isIncomingSignal = true;
			}

			InputSocket inputSocket( socketName );
			if (	executionContext.GetActionType() == ExecutionActionType::DebugCommandAwake
				 || executionContext.GetActionType() == ExecutionActionType::DebugCommandExecuteSingleNode
				 || executionContext.GetActionType() == ExecutionActionType::ExecuteSingleNode )
			{
				// it's execution of debug command
				auto& questsSystem = executionContext.GetGameInstance().GetSystem< IQuestsSystem >();
				if ( questsSystem.ArePrefabsLoaded( GetFullPathHash() ) )
				{
					// prefabs are ready, let's run from node
					return RunFromNode( executionContext, weakNode, inputSocket, isIncomingSignal, outputSockets );
				}
				else
				{
					// there can be some prefabs that require to be loaded, let schedule until we make sure prefabs are loaded
					RunRequest request( GetFullPathHash(), node->GetId(), inputSocketIndex, executionContext.GetActionType() );
					questsSystem.PostponeRunRequest( std::move( request ) );

					return PhaseResult::InProgress;
				}
			}
			else
			{
				// else it's a regular execution, run from node
				return RunFromNode( executionContext, weakNode, inputSocket, isIncomingSignal, outputSockets );
			}
		}

		return ret;
	}

	Bool PhaseInstance::KillSignal( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId )
	{
		Bool ret = true;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( !childInstance )
			{
				// phase seems to no longer be active
				return true;
			}

			PushPhase( executionContext );
			ret = childInstance->KillSignal( executionContext, fullPath, nodeId );
			PopPhase( executionContext );
		}
		else
		{
			auto node = m_graph->GetNodeById( nodeId );
			GAME_ASSERT( !node.Expired(), Quests, "Node not found in graph" );

			auto disableableNode = Cast< DisableableNodeDefinition >( node.ToHandle() );
			GAME_ASSERT( disableableNode, Quests, "Node is not a signal-stopping node" );

			PushPhase( executionContext );
			disableableNode->KillSignal( executionContext.GetNodeExecutionContext(), false, SignalKillingReason::DebuggerCommand ); // false, killing by debugger command is never permanent
			PopPhase( executionContext );

			return true;
		}

		return ret;
	}

	void PhaseInstance::StopPhase( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, SignalKillingReason reason )
	{
		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure?
				return;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			if ( !phaseNode )
			{
				// changed quest structure?
				return;
			}

			PushPhase( executionContext );

			PhaseInstance* childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( childInstance )
			{
				// it may not exist since phase may be not yet thawed when unfreezing
				childInstance->StopPhase( executionContext, fullPath, nodeId, reason );
			}

			PopPhase( executionContext );
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure?
				return;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			if ( !phaseNode )
			{
				// changed quest structure?
				return;
			}

			// stop execution
			for ( Uint32 i = 0; i < m_childInstances.Size(); ++i )
			{
				const auto childInstance = m_childInstances[ i ];

				const auto& childFullPath = childInstance->GetFullPath();
				RED_FATAL_ASSERT( !childFullPath.Empty() );

				if ( nodeId == childFullPath[ childFullPath.Size() - 1 ] )
				{
					Stop( i, executionContext, reason );

#ifdef RED_ENABLE_QUEST_DEBUGGER
					if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
					{
						PushPhase( executionContext );

						debugger->OnPhaseFinished( executionContext, childInstance.Get() );
						debugger->OnBlockFinished( executionContext, phaseNode.Get() );

						PopPhase( executionContext );
					}
#endif

					break;
				}
			}
		}
	}

	Bool PhaseInstance::CanBeFrozen( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId )
	{
		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			Bool ret = false;

			PushPhase( executionContext );

			PhaseInstance* childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( !childInstance )
			{
				return true;
			}
			ret = childInstance->CanBeFrozen( executionContext, fullPath, nodeId );

			PopPhase( executionContext );

			return ret;
		}
		else
		{
			PhaseInstance* childInstance = FindInstanceByNodeId( nodeId );
			if ( !childInstance )
			{
				return true;
			}

			return childInstance->CanPhaseBeFrozen_Internal();
		}
	}

	void PhaseInstance::FreezeImmediately( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId )
	{
		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// user has been already notified about missing node in PhaseInstance::RecreatePhaseInstanceTreeAfterLoad
				// just return gracefully
				return;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

			PushPhase( executionContext );

			PhaseInstance* childInstance = FindInstanceByNodeId( phaseNodeId );
			GAME_ASSERT( childInstance, Quests, "Phase instance doesn't exist" );
			childInstance->FreezeImmediately( executionContext, fullPath, nodeId );

			PopPhase( executionContext );
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// user has been already notified about missing node in PhaseInstance::RecreatePhaseInstanceTreeAfterLoad
				// just return gracefully
				return;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

			const auto& unfreezingNodeRef = phaseNode->GetUnfreezeingTriggerNodeRef();
			RED_FATAL_ASSERT( unfreezingNodeRef.IsDefined() );

			const auto unfreezingTriggerGlobalNodeID = RuntimeHelper::NodeRefToGlobalNodeID( unfreezingNodeRef );

			PushPhase( executionContext );
			const HashedNodePath hashedNodePath = phaseNode->CreateHashedNodePath( executionContext.GetNodeExecutionContext() );
			PopPhase( executionContext );

			// collect freezing hashed node paths
			red::DynArray< FrozenNodeEntry > frozenNodeEntries{ game::PoolGMPL_QuestDynamic() };
			FreezeInternal( executionContext, frozenNodeEntries, nodeId );

			// and store them
#ifdef RED_ENABLE_QUEST_FREEZER_DEBUG_DATA
			String debugClassName, debugIdPath, debugNamePath;
			const auto questSystem = executionContext.GetNodeExecutionContext().GetQuestsSystem();
			questSystem->DebugGetPathStringsFromPathHash( hashedNodePath, debugClassName, debugIdPath, debugNamePath );
#endif

			auto freezer = executionContext.GetNodeExecutionContext().GetQuestPhaseFreezer();
			RED_FATAL_ASSERT( freezer );
			freezer->StoreFrozenNodeEntries(	unfreezingTriggerGlobalNodeID
												, hashedNodePath
												, std::move( frozenNodeEntries )
#ifdef RED_ENABLE_QUEST_FREEZER_DEBUG_DATA
												, std::move( debugIdPath )
												, std::move( debugNamePath )
#endif
											);
		}
	}

	Bool PhaseInstance::Unfreeze_MakeReady( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId )
	{
		Bool ret = false;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// change quest structure, consider it ready
				return true;
			}

			THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( phaseNodeId ) );
			if ( !childInstance )
			{
				// we need to create a child instance for a phase that has neither phase resource nor prefab resources to be loaded
				auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
				GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

				// recreate phase instance correctly
				Bool shouldWrapperBeCreated = phaseNode->ShouldWrapperBeCreatedRightNow();
				Bool hasWrapperBeenCreated = !!FindEventListener( ExtendedNodeId::CreateDefault( phaseNodeId ) );

				if ( shouldWrapperBeCreated )
				{
					if ( !hasWrapperBeenCreated )
					{
						// run the node to create wrapper
						InputSocket inputSocket( NOTHING_SOCKET_NAME );
						OutputSocketArray dummyOutputSockets;
						auto runRet = RunFromNode( executionContext, weakNode, inputSocket, true, dummyOutputSockets );
						GAME_ASSERT( runRet == PhaseResult::InProgress, Quests, "Ret value be in progress, we just activate the node, not run something inside" );
						GAME_ASSERT( !!FindEventListener( ExtendedNodeId::CreateDefault( phaseNodeId ) ), Quests, "Wrapper should be created at this point" );
					}
				}

				Bool isReady = false;

				// let's find listener
				if ( shouldWrapperBeCreated )
				{
					const auto* wrapper = FindEventListener( ExtendedNodeId::CreateDefault( phaseNodeId ) );
					GAME_ASSERT( wrapper, Quests, "Wrapper should be created at this point" );

					const auto* phaseWrapper = static_cast<const PhaseNodeListenerWrapper*>( wrapper );
					if ( phaseWrapper )
					{
						auto stage = phaseWrapper->GetStage();
						if ( stage == PhaseStage::Running )
						{
							// consider it ready
							isReady = true;
						}
					}
				}
				else
				{
					// phase doesn't require listener, consider it ready
					isReady = true;
				}

				if ( !isReady )
				{
					return false;
				}

				PushPhase( executionContext );

				auto phaseGraph = phaseNode->GetGraph();
				GAME_ASSERT( phaseGraph, Quests, "Phase graph doesn't exist" );

				childInstance = CreateChildInstance( executionContext, phaseNode->GetPhaseResource(), phaseGraph, phaseNodeId, phaseNode->GetFriendlyName() );

				phaseNode->HandleSaveLock( executionContext.GetNodeExecutionContext(), true );

#ifdef RED_ENABLE_QUEST_DEBUGGER
				if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
				{
					debugger->OnBlockStarted( executionContext, phaseNode.Get() );
					debugger->OnPhaseStarted( executionContext, childInstance.Get() );
				}
#endif //RED_ENABLE_QUEST_DEBUGGER

				PopPhase( executionContext );
			}

			PushPhase( executionContext );

			ret = childInstance->Unfreeze_MakeReady( executionContext, fullPath, nodeId );

			PopPhase( executionContext );
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// try getting deletion marker mapping
				NodeId deletedNodeId = GetDeletedNodeId( nodeId );
				if ( deletedNodeId != INVALID_QUEST_NODE_ID )
				{
					// hopefully we find it now
					weakNode = m_graph->GetNodeById( deletedNodeId );
				}

				if ( weakNode.Expired() )
				{
					// there is no node even when taking deletion markers info account
					// just return gracefully
					return true;
				}
			}

			// let's handle phase node, then run the node to create a listener so required resources are loaded
			if ( auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() ) )
			{
				Bool shouldWrapperBeCreated = phaseNode->ShouldWrapperBeCreatedRightNow();
				Bool hasWrapperBeenCreated = !!FindEventListener( ExtendedNodeId::CreateDefault( nodeId ) );

				if ( shouldWrapperBeCreated )
				{
					if ( !hasWrapperBeenCreated )
					{
						// run the node to create wrapper
						InputSocket inputSocket( NOTHING_SOCKET_NAME );
						OutputSocketArray dummyOutputSockets;
						auto runRet = RunFromNode( executionContext, weakNode, inputSocket, true, dummyOutputSockets );
						GAME_ASSERT( runRet == PhaseResult::InProgress, Quests, "Ret value be in progress, we just activate the node, not run something inside" );
						GAME_ASSERT( !!FindEventListener( ExtendedNodeId::CreateDefault( nodeId ) ), Quests, "Wrapper should be created at this point" );
					}
				}

				Bool isReady = false;

				// let's find listener
				if ( shouldWrapperBeCreated )
				{
					const auto* wrapper = FindEventListener( ExtendedNodeId::CreateDefault( nodeId ) );
					GAME_ASSERT( wrapper, Quests, "Wrapper should be created at this point" );

					const auto* phaseWrapper = static_cast<const PhaseNodeListenerWrapper*>( wrapper );
					if ( phaseWrapper )
					{
						auto stage = phaseWrapper->GetStage();
						if ( stage == PhaseStage::Running )
						{
							// consider it ready
							isReady = true;
						}
					}
				}
				else
				{
					// phase doesn't require listener, consider it ready
					isReady = true;
				}

				if ( isReady )
				{
					// create instance if doesn't exist yet
					THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( nodeId ) );
					if ( !childInstance )
					{
						PushPhase( executionContext );

						auto phaseGraph = phaseNode->GetGraph();
						GAME_ASSERT( phaseGraph, Quests, "Phase graph doesn't exist" );

						childInstance = CreateChildInstance( executionContext, phaseNode->GetPhaseResource(), phaseGraph, nodeId, phaseNode->GetFriendlyName() );

						phaseNode->HandleSaveLock( executionContext.GetNodeExecutionContext(), true );

#ifdef RED_ENABLE_QUEST_DEBUGGER
						if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
						{
							debugger->OnBlockStarted( executionContext, phaseNode.Get() );
							debugger->OnPhaseStarted( executionContext, childInstance.Get() );
						}
#endif //RED_ENABLE_QUEST_DEBUGGER

						PopPhase( executionContext );
					}
				}

				return isReady;
			}
			else
			{
				// not a phase, consider it ready
				return true;
			}
		}

		return ret;
	}

	namespace
	{
		static Bool IsSceneOrPhaseNode( const THandle< NodeDefinition >& node )
		{
			static const CName allowedClassNames[] = {	RED_NAME_CONSTEXPR( "questSceneNodeDefinition" ),
														RED_NAME_CONSTEXPR( "questPhaseNodeDefinition" ) };

			if ( node )
			{
				CName className = node->GetClass()->GetName();
				for ( const CName allowedClassName : allowedClassNames )
				{
					if ( className == allowedClassName )
					{
						return true;
					}
				}
			}

			return false;
		}
	}

	PhaseResult PhaseInstance::Unfreeze_Run( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, CName inputSocketName, OutputSocketArray& outputSockets, UnfreezingStage stage )
	{
		PhaseResult ret = PhaseResult::InProgress;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure, just return
				return PhaseResult::InProgress;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

			THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( phaseNodeId ) );
			if ( !childInstance )
			{
				// if there is no phase instance, it means it was already removed when a signal left it
				// consider this signal was killed and do nothing
				return PhaseResult::InProgress;
			}

			PushPhase( executionContext );

			ret = childInstance->Unfreeze_Run( executionContext, fullPath, nodeId, inputSocketName, outputSockets, stage );

			if ( ret == PhaseResult::Finished )
			{
				// if Resume returns false, it means that signal went through the phase and left so child instance can be deleted and signal pushed further in this phase
				RemoveChildInstance( executionContext, childInstance, SignalKillingReason::PhaseLeft );
			}

			if ( ret == PhaseResult::Finished )
			{
#ifdef RED_ENABLE_QUEST_DEBUGGER
				// notify debugger about leaving a node (it's OnBlockFinished equivalent from NodeDefinition::Execute)
				if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
				{
					debugger->OnBlockFinished( executionContext, phaseNode.Get() );
				}
#endif
			}

			PopPhase( executionContext );

			if ( ret == PhaseResult::Finished || ret == PhaseResult::InProgressAndFinished )
			{
				GAME_ASSERT( phaseNode->IsA< PhaseNodeDefinition >(), Quests, "Unexpected node type, is %hs instead of %hs", phaseNode->GetClass()->GetName().AsChar(), ClassID< PhaseNodeDefinition >()->GetName().AsChar() );
				GAME_ASSERT( outputSockets.Size() == 1, Quests, "Phase node can have only one output" );
				// send outcoming signal from the phase node
				return RunFromNode( executionContext, weakNode, outputSockets.GetFront(), false, outputSockets );
			}
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// try getting deletion marker mapping
				NodeId deletedNodeId = GetDeletedNodeId( nodeId );
				if ( deletedNodeId != INVALID_QUEST_NODE_ID )
				{
					// hopefully we find it now
					weakNode = m_graph->GetNodeById( deletedNodeId );
				}

				if ( weakNode.Expired() )
				{
					// there is no node even when taking deletion markers info account
					// just return gracefully
					return PhaseResult::InProgress;
				}
			}

			////////////////////////////////
			//
			//
			//
			/*
			static red::DynArray< NodeId > arr{ game::PoolGMPL_Quest() };
			if ( arr.Empty() )
			{
				arr = { 0, 2, 82, 3, 19, 1, 9, 71, 7, 53, 17 };
			}
			if ( fullPath == arr )
			{
				RED_LOG( "" );
			}
			*/
			//
			//
			//
			////////////////////////////////

			Bool runNode = false;
			if ( auto node = weakNode.ToHandle() )
			{
				if ( stage == UnfreezingStage::SceneAndPhaseNodes )
				{
					runNode = IsSceneOrPhaseNode( node  );
				}
				else if ( stage == UnfreezingStage::RegularNodes )
				{
					runNode = !IsSceneOrPhaseNode( node );
				}
			}

			if ( !runNode )
			{
				return PhaseResult::InProgress;
			}

			// if it's a phase node, create instance
			if ( auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() ) )
			{
				PushPhase( executionContext );

				THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( nodeId ) );
				if ( !childInstance )
				{
					auto phaseGraph = phaseNode->GetGraph();
					GAME_ASSERT( phaseGraph, Quests, "Phase graph doesn't exist" );

					childInstance = CreateChildInstance( executionContext, phaseNode->GetPhaseResource(), phaseGraph, nodeId, phaseNode->GetFriendlyName() );

					phaseNode->HandleSaveLock( executionContext.GetNodeExecutionContext(), true );

#ifdef RED_ENABLE_QUEST_DEBUGGER
					if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
					{
						debugger->OnBlockStarted( executionContext, phaseNode.Get() );
						debugger->OnPhaseStarted( executionContext, childInstance.Get() );
					}
#endif //RED_ENABLE_QUEST_DEBUGGER
				}

				PopPhase( executionContext );
			}

			// eventually run the node
			InputSocket inputSocket( inputSocketName );
			return RunFromNode( executionContext, weakNode, inputSocket, true, outputSockets );
		}

		return ret;
	}

	PhaseResult PhaseInstance::UnfreezeImmediately( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, CName inputSocketName, OutputSocketArray& outputSockets )
	{
		PhaseResult ret = PhaseResult::InProgress;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		if ( pathIndex < fullPath.Size() )
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto weakNode = m_graph->GetNodeById( phaseNodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// just return gracefully
				return PhaseResult::InProgress;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( weakNode.ToHandle() );
			GAME_ASSERT( phaseNode, Quests, "Node is not a phase node" );

			THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( phaseNodeId ) );
			if ( !childInstance )
			{
				// it's for backward compatibility for savegames that contain frozen phases that should be already stopped
				// see CYB-567494 and CYB-566413
				return PhaseResult::InProgress;
			}

			PushPhase( executionContext );

			ret = childInstance->UnfreezeImmediately( executionContext, fullPath, nodeId, inputSocketName, outputSockets );

			if ( ret == PhaseResult::Finished )
			{
				// if Resume returns false, it means that signal went through the phase and left so child instance can be deleted and signal pushed further in this phase
				RemoveChildInstance( executionContext, childInstance, SignalKillingReason::PhaseLeft );
			}

			if ( ret == PhaseResult::Finished )
			{
#ifdef RED_ENABLE_QUEST_DEBUGGER
				// notify debugger about leaving a node (it's OnBlockFinished equivalent from NodeDefinition::Execute)
				if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
				{
					debugger->OnBlockFinished( executionContext, phaseNode.Get() );
				}
#endif
			}

			PopPhase( executionContext );

			if ( ret == PhaseResult::Finished || ret == PhaseResult::InProgressAndFinished )
			{
				GAME_ASSERT( phaseNode->IsA< PhaseNodeDefinition >(), Quests, "Unexpected node type, is %hs instead of %hs", phaseNode->GetClass()->GetName().AsChar(), ClassID< PhaseNodeDefinition >()->GetName().AsChar() );
				GAME_ASSERT( outputSockets.Size() == 1, Quests, "Phase node can have only one output" );
				// send outcoming signal from the phase node
				return RunFromNode( executionContext, weakNode, outputSockets.GetFront(), false, outputSockets );
			}
		}
		else
		{
			auto weakNode = m_graph->GetNodeById( nodeId );
			if ( weakNode.Expired() )
			{
				// changed quest structure
				// just return gracefully
				return PhaseResult::InProgress;
			}

			// run the node
			InputSocket inputSocket( inputSocketName );
			return RunFromNode( executionContext, weakNode, inputSocket, true, outputSockets );
		}

		return ret;
	}

	// similar to PhaseInstance::Resume
	Bool PhaseInstance::RecreatePhaseInstanceTreeAfterLoad( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, CName debugName )
	{
		Bool ret = true;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		// PMTODO unlike Resume, this doesn't have to be recursive, consider replacing with while
		if ( pathIndex < fullPath.Size() )
		{
			PushPhase( executionContext );

			NodeId phaseNodeId = fullPath[ pathIndex ];
			executionContext.SetCurrentNodeId( phaseNodeId );

			auto node = m_graph->GetNodeById( phaseNodeId );
			if ( node.Expired() )
			{
				String text = String::Printf( "Missing phase node during load, quest structure was changed: %hs <id: %d>, destination node: (%hs)",
											  executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathString().AsChar(),
											  phaseNodeId,
											  debugName.AsChar() );

				GAME_DATA_ERROR( false, Quests, text.AsChar() );

				executionContext.DebugIncrementMissingNodeCount();
				executionContext.DebugSetLoadReportText( std::move( text ) );

				PopPhase( executionContext );
				return true;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( node.ToHandle() );
			if ( !phaseNode )
			{
				String text = String::Printf( "Expected phase node, but found %hs node at %hs [%hs]",
											  node.ToHandle()->GetClass()->GetName().AsChar(),
											  executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathStringWithNode().AsChar(),
											  executionContext.GetNodeExecutionContext().DebugGetFullPathStringWithNode().AsChar() );

				GAME_DATA_ERROR( false, Quests, text.AsChar() );

				executionContext.DebugIncrementChangedNodeCount();
				executionContext.DebugSetLoadReportText( std::move( text ) );

				PopPhase( executionContext );
				return true;
			}

			auto childInstance = RecreatePhaseInstanceAfterLoad_Internal( executionContext, *phaseNode );
			if ( !childInstance )
			{
				// debug  stuff handled inside
				PopPhase( executionContext );
				return true;
			}

			ret = childInstance->RecreatePhaseInstanceTreeAfterLoad( executionContext, fullPath, nodeId, debugName );

			PopPhase( executionContext );
		}
		else
		{
			PushPhase( executionContext );

			executionContext.SetCurrentNodeId( nodeId );

			auto node = m_graph->GetNodeById( nodeId ).ToHandle();
			if ( !node )
			{
				// try getting deletion marker mapping
				NodeId deletedNodeId = GetDeletedNodeId( nodeId );
				if ( deletedNodeId != INVALID_QUEST_NODE_ID )
				{
					// hopefully we find it now
					node = m_graph->GetNodeById( deletedNodeId ).ToHandle();
				}
			}

			if ( !node )
			{
				String text = String::Printf( "Missing regular node during load, quest structure was changed: %hs <id: %d> (%hs)",
											  executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathString().AsChar(),
											  nodeId,
											  debugName.AsChar() );

				GAME_DATA_ERROR( false, Quests, text.AsChar() );

				executionContext.DebugIncrementMissingNodeCount();
				executionContext.DebugSetLoadReportText( std::move( text ) );

				PopPhase( executionContext );
				return true;
			}
			else
			{
				auto phaseNode = Cast< PhaseNodeDefinition >( node );
				if ( phaseNode )
				{
					// look like it's a phase node at the end of the path, let's create a instance for it so we can load prefabs
					RecreatePhaseInstanceAfterLoad_Internal( executionContext, *phaseNode );
				}
			}

			PopPhase( executionContext );
		}

		return ret;
	}

	THandle< PhaseInstance > PhaseInstance::RecreatePhaseInstanceAfterLoad_Internal( ExecutionContext& executionContext, const PhaseNodeDefinition& phaseNode )
	{
		THandle< PhaseInstance > childInstance = HandleFromPtr( FindInstanceByNodeId( phaseNode.GetId() ) );
		if ( childInstance )
		{
			return childInstance;
		}

		auto phaseResource = phaseNode.GetPhaseResource();
		// phase resource can be null here (resourceless phase node)

		auto phaseGraph = phaseNode.GetGraph();
		if ( !phaseGraph )
		{
			auto phaseResourcePath = phaseNode.GetPhaseResourcePath().ToString();
			if ( !phaseResourcePath.Empty() )
			{
				// there is no graph due to a missing resource, let's fire game data assert
				GAME_DATA_ERROR( false, Quests,
								 "Missing phase resource [%hs] in a node %hs %hs",
								 phaseResourcePath.AsChar(),
								 executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathStringWithNode().AsChar(),
								 executionContext.GetNodeExecutionContext().DebugGetFullPathStringWithNode().AsChar() );
				executionContext.DebugIncrementMissingPhaseResourceCount();
			}
			else
			{
				// there is no graph while there was no external phase resource, that's more serious
				GAME_ASSERT( false, Quests,
							 "Phase graph doesn't exist in a node %hs %hs",
							 executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathStringWithNode().AsChar(),
							 executionContext.GetNodeExecutionContext().DebugGetFullPathStringWithNode().AsChar() );
			}

			// either way return
			return nullptr;
		}

#ifdef RED_ENABLE_QUEST_DEBUGGER
		dbg::QuestDebugger* debugger = executionContext.GetDebugger();
		if ( debugger )
		{
			debugger->OnBlockStarted( executionContext, &phaseNode );
		}
#endif //RED_ENABLE_QUEST_DEBUGGER

		childInstance = CreateChildInstance( executionContext, phaseResource, phaseGraph, phaseNode.GetId(), phaseNode.GetFriendlyName() );

		phaseNode.HandleSaveLock( executionContext.GetNodeExecutionContext(), true );

		if ( !phaseNode.GetPhasePrefabs().Empty() )
		{
			red::DynArray< QuestPrefabEntry > copy{ job::PoolJobScope() };
			auto* questPrefabsHandler = executionContext.GetNodeExecutionContext().GetQuestPrefabsHandler();
			copy = phaseNode.GetPhasePrefabs();
			for ( auto& entry : copy )
			{
				GAME_DATA_ERROR( entry.m_prefabNodeRef.IsDefined(),
								 Quests,
								 "Phase node has empty prefab added %hs [%hs]",
								 executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathStringWithNode().AsChar(),
								 executionContext.GetNodeExecutionContext().DebugGetFullPathStringWithNode().AsChar() );
			}

			// these prefabs were loaded when we saved the game
			// we can safely assume that they should be loaded now
			childInstance->m_onLoadGamePrefabsLoadRequestId = questPrefabsHandler->EnablePrefabsStreaming(	copy,
																											[]( Uint32 id )
																											{
																											} // intentionally empty
#ifndef RED_CONFIGURATION_FINAL
																											, executionContext.GetNodeExecutionContext().DebugGetFriendlyFullPathStringWithNode()
#endif
			);
		}

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
		{
			debugger->OnPhaseStarted( executionContext, childInstance.Get() );
		}
#endif //RED_ENABLE_QUEST_DEBUGGER

		return childInstance;
	}

	// similar to PhaseInstance::Resume
	Bool PhaseInstance::RecreateSignalsAfterLoad( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId )
	{
		Bool ret = true;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		// PMTODO unlike Resume, this doesn't have to be recursive, consider replacing with while
		if ( pathIndex < fullPath.Size() )
		{
			PushPhase( executionContext );

			NodeId phaseNodeId = fullPath[ pathIndex ];
			executionContext.SetCurrentNodeId( phaseNodeId );

			auto node = m_graph->GetNodeById( phaseNodeId );
			if ( node.Expired() )
			{
				// missing phase node, handled in RecreatePhaseInstanceTreeAfterLoad
				PopPhase( executionContext );
				return true;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( node.ToHandle() );
			if ( !phaseNode )
			{
				// changed phase node, handled in RecreatePhaseInstanceTreeAfterLoad
				PopPhase( executionContext );
				return true;
			}

			Bool shouldWrapperBeCreated = phaseNode->IsFreezable();
			Bool hasWrapperBeenCreated = !!FindEventListener( ExtendedNodeId::CreateDefault( phaseNode->GetId() ) );

			if ( shouldWrapperBeCreated )
			{
				if ( !hasWrapperBeenCreated )
				{
					ret = phaseNode->LoadGame( executionContext.GetNodeExecutionContext(), false );
				}
			}

			auto childInstance = FindInstanceByNodeId( phaseNodeId );
			GAME_ASSERT( childInstance, Quests, "Instance should have been already created in RecreatePhaseInstanceTreeAfterLoad" );
			if ( !childInstance )
			{
				PopPhase( executionContext );
				return true;
			}

			ret = childInstance->RecreateSignalsAfterLoad( executionContext, fullPath, nodeId );

			PopPhase( executionContext );
		}
		else
		{
			PushPhase( executionContext );

			executionContext.SetCurrentNodeId( nodeId );

			auto node = m_graph->GetNodeById( nodeId ).ToHandle();
			if ( !node )
			{
				// try getting deletion marker mapping
				NodeId deletedNodeId = GetDeletedNodeId( nodeId );
				if ( deletedNodeId != INVALID_QUEST_NODE_ID )
				{
					// hopefully we find it now
					node = m_graph->GetNodeById( deletedNodeId ).ToHandle();

					// clear entry in event manager
					executionContext.GetNodeExecutionContext().ClearEvent( ExtendedNodeId::CreateDefault( nodeId ) );
				}
			}

			if ( !node )
			{
				// missing node, handled in RecreatePhaseInstanceTreeAfterLoad
				PopPhase( executionContext );
				return true;
			}

			auto signalStoppingNode = Cast< SignalStoppingNodeDefinition >( node );
			if ( !signalStoppingNode )
			{
				// changed node, handled in RecreatePhaseInstanceTreeAfterLoad
				PopPhase( executionContext );
				return true;
			}

#ifdef RED_ENABLE_QUEST_DEBUGGER
			dbg::QuestDebugger* debugger = executionContext.GetDebugger();
			if ( debugger )
			{
				debugger->OnBlockStarted( executionContext, signalStoppingNode.Get() );
			}
#endif //RED_ENABLE_QUEST_DEBUGGER

			ret = signalStoppingNode->LoadGame( executionContext.GetNodeExecutionContext(), true );

			PopPhase( executionContext );
		}

		return ret;
	}

	Bool PhaseInstance::RecreateLostSignals( ExecutionContext& executionContext, const FullPath& fullPath, NodeId nodeId, Bool forceWithNoInput, NodeId additionalNodeToRun )
	{
		Bool ret = true;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		// PMTODO unlike Resume, this doesn't have to be recursive, consider replacing with while
		if ( pathIndex < fullPath.Size() )
		{
			PushPhase( executionContext );

			NodeId phaseNodeId = fullPath[ pathIndex ];
			executionContext.SetCurrentNodeId( phaseNodeId );

			auto node = m_graph->GetNodeById( phaseNodeId );
			if ( node.Expired() )
			{
				// missing node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( node.ToHandle() );
			if ( !phaseNode )
			{
				// changed phase node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}

			auto childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( !childInstance )
			{
				// instance doesn't exist (can be frozen)
				// return false as it might exist in the future
				PopPhase( executionContext );
				return false;
			}

			ret = childInstance->RecreateLostSignals( executionContext, fullPath, nodeId, forceWithNoInput, additionalNodeToRun );

			PopPhase( executionContext );
		}
		else
		{
			executionContext.SetCurrentNodeId( nodeId );

			auto node = m_graph->GetNodeById( nodeId ).ToHandle();
			if ( !node )
			{
				// try getting deletion marker mapping
				NodeId deletedNodeId = GetDeletedNodeId( nodeId );
				if ( deletedNodeId != INVALID_QUEST_NODE_ID )
				{
					// hopefully we find it now
					node = m_graph->GetNodeById( deletedNodeId ).ToHandle();
				}
			}

			if ( !node )
			{
				// missing node
				// return true, we're not going to recover it again
				return true;
			}

			if ( node->GetClass()->GetName() != RED_NAME_CONSTEXPR_NOREG( "questSceneNodeDefinition" ) )
			{
				// changed node
				// return true, we're not going to recover it again
				return true;
			}

			// find input socket
			CName inputSocketName;
			for ( Uint32 i = 0; i < node->GetSocketCount(); ++i )
			{
				auto socket = Cast< SocketDefinition >( node->GetSocketHandleByIndex( i ) );
				if ( socket )
				{
					if ( socket->GetType() == SocketType::Input )
					{
						if ( socket->HasAnyConnections() )
						{
							if ( inputSocketName )
							{
								inputSocketName = CName::NONE();
								break;
							}
							else
							{
								inputSocketName = socket->GetName();
							}
						}
					}
				}
			}

			if ( forceWithNoInput )
			{
				InputSocket inputSocket( NOTHING_SOCKET_NAME );
				OutputSocketArray outputSockets;
				RunFromNode( executionContext, node, inputSocket, true, outputSockets );

				// additional node to run
				if ( additionalNodeToRun != INVALID_QUEST_NODE_ID )
				{
					auto additionalNode = m_graph->GetNodeById( additionalNodeToRun ).ToHandle();
					if ( !additionalNode )
					{
						// try getting deletion marker mapping
						NodeId deletedNodeId = GetDeletedNodeId( additionalNodeToRun );
						if ( deletedNodeId != INVALID_QUEST_NODE_ID )
						{
							// hopefully we find it now
							additionalNode = m_graph->GetNodeById( deletedNodeId ).ToHandle();
						}
					}

					if ( additionalNode )
					{
						InputSocket inputSocket( NOTHING_SOCKET_NAME );
						OutputSocketArray outputSockets;
						executionContext.ForceSetActionType( ExecutionActionType::Execution );
						RunFromNode( executionContext, additionalNode, inputSocket, true, outputSockets );
					}
				}

				// return true, the signal was recovered
				ret = true;
			}
			else if ( inputSocketName )
			{
				InputSocket inputSocket( inputSocketName );
				OutputSocketArray outputSockets;
				RunFromNode( executionContext, node, inputSocket, true, outputSockets );

				// return true, the signal was recovered
				ret = true;
			}
			else
			{
				// return false, signal was not recovered because of multiple connected input sockets
				ret = false;
			}
		}

		return ret;
	}

	Bool PhaseInstance::RecreateLostStsSignals( ExecutionContext& executionContext, const FullPath& fullPath )
	{
		Bool ret = false;

		GAME_ASSERT( m_graph, Quests, "Graph not found" );

		Uint32 pathIndex = executionContext.GetPhaseStackSize();
		GAME_ASSERT( pathIndex >= 1, Quests, "Incorrect phase stack size" );

		// PMTODO unlike Resume, this doesn't have to be recursive, consider replacing with while
		if ( pathIndex < fullPath.Size() - 1 )
		{
			PushPhase( executionContext );

			NodeId phaseNodeId = fullPath[ pathIndex ];
			executionContext.SetCurrentNodeId( phaseNodeId );

			auto node = m_graph->GetNodeById( phaseNodeId );
			if ( node.Expired() )
			{
				// missing node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( node.ToHandle() );
			if ( !phaseNode )
			{
				// changed phase node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}

			auto childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( !childInstance )
			{
				// instance doesn't exist (can be frozen)
				// return false as it might exist in the future
				PopPhase( executionContext );
				return false;
			}

			ret = childInstance->RecreateLostStsSignals( executionContext, fullPath );

			PopPhase( executionContext );
		}
		else
		{
			NodeId phaseNodeId = fullPath[ pathIndex ];

			auto childInstance = FindInstanceByNodeId( phaseNodeId );
			if ( !childInstance )
			{
				// instance doesn't exist (can be frozen)
				// return false as it might exist in the future
				return false;
			}

			if ( childInstance->GetChildrenCount() == 0 )
			{
				auto node = m_graph->GetNodeById( phaseNodeId );
				if ( node.Expired() )
				{
					// missing node
					// return true, we're not going to recover it again
					ret = false;
				}
				else
				{
					InputSocket inputSocket( RED_NAME( "In1" ) );
					OutputSocketArray outputSockets;
					RunFromNode( executionContext, node, inputSocket, true, outputSockets );

					ret = true;
				}
			}
			else
			{
				ret = false;
			}

			/*
			auto node = m_graph->GetNodeById( phaseNodeId );
			if ( node.Expired() )
			{
				// missing node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}

			auto phaseNode = Cast< PhaseNodeDefinition >( node.ToHandle() );
			if ( !phaseNode )
			{
				// changed phase node
				// return true, we're not going to recover it again
				PopPhase( executionContext );
				return true;
			}
			*/

		}

		/*
		else
		{
			if ( m_childInstances.Empty() )
			{
				//ret = true;

				//InputSocket inputSocket( inputSocketName );
				//OutputSocketArray outputSockets;
				//RunFromNode( executionContext, node, inputSocket, true, outputSockets );
			}
			else
			{
				ret = false;
			}
		}
		*/

		return ret;
	}

	void PhaseInstance::RunFromStartNode( ExecutionContext& executionContext )
	{
		GAME_DATA_ERROR( m_graph, Quests, "Graph doesn't exist" );
		if ( !m_graph )
		{
			return;
		}

		WeakHandle< NodeDefinition > startNode = m_graph->GetFirstNode< StartNodeDefinition >();

		GAME_DATA_ERROR( !startNode.Expired(), Quests, "No Start node found" );
		if ( startNode.Expired() )
		{
			return;
		}

		OutputSocketArray outputsockets;
		PhaseResult ret = RunFromNode( executionContext, startNode, InputSocket( NOTHING_SOCKET_NAME ), true, outputsockets );
		GAME_ASSERT( ret == PhaseResult::InProgress, Quests, "This should never happened, in this case returned value should be PhaseResult::InProgress" );

		// since it's meant for quest instances, there's no need to return value as quest instances are not going to be removed
	}

	// ---------------------------------------------------------------------------------------------------------
	// CP2077 web [watson]: ?start=watson = debug start in the post-prologue q001 state (opt-in, test pages only).
	// The new game itself is the normal one (missionflow StartNewGame, default Nomad character, cyberpunk2077.gamedef,
	// cp77_new_game -> Prologue "Q000 Main"). When that first signal enters prologue.questphase through its
	// "Q000 Main" input, it is sent once to CDPR's own debug input of the same phase instead - the inputs the
	// original cp77_chicken_test.scene uses ("Q001 Vroom" / "Q001"), so q000 (lifepath + montage + VR tutorial)
	// is skipped and q001 runs from the original graph. Facts set at that moment = what the chicken-test branch
	// sets plus the shared q000 tail (Global cleanup / q000 out) that is true after any prologue:
	//   both:      q000_done=1 (chicken test, q000 out), radio_on=1, tv_on=1, air_traffic_off=0 (Global cleanup),
	//              q000_hym_real_time=1 (montage), q000_nomad_full_car_tutorial=1 (Nomad V: radio tower car tutorial;
	//              q001_car_tutorial waits on it, the NotNomad branch on the q001_intro journal entry that only the
	//              skipped lifepath phases activate)
	//   apartment: q001_mission0_complete=1 (chicken test), q001_active=1 (set by Mission 0, true after it)
	// q000_vr_tutorial_enabled stays 0, so Mission 0 takes its own "normal q001 path" setup (teleport to
	// #q001_spn_player_start, gear, time 13:40, q001_active=1, ...), exactly as on the chicken-test "Q001" entry.
	// &watsonEntry=q001 (alias rescue) = "Q001" (Mission 0 "The Rescue", the literal first q001 step after any
	// prologue); default / &watsonEntry=apartment = "Q001 Vroom" (V's apartment wake-up, q001_apartment).
	// The spawn point is set in gameState_Session.cpp, saves are gated in gameState_Session.cpp/gameAutoSaveSystem.cpp.
	// ---------------------------------------------------------------------------------------------------------
	static Int32 WatsonStartMode()
	{
		static std::atomic< Int32 > s_mode{ -1 };
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
					return ( e === 'q001' || e === 'rescue' || e === 'mission0' ) ? 2 : 1;
				} catch ( e ) { return 0; }
			} );
#endif
			s_mode.store( v );
		}
		return v;
	}

	static CName WatsonRedirectSocket( const PhaseInstance& phase, const res::ResourcePath& path, const CName socketName, ExecutionContext& executionContext )
	{
		static std::atomic< Int32 > s_fired{ 0 };
		if ( s_fired.load() || executionContext.GetActionType() != ExecutionActionType::Execution )
		{
			return socketName;
		}
		static const Uint64 c_prologue = res::ResourcePath::Build( red::StringView( "base\\quest\\main_quests\\prologue\\prologue.questphase" ) ).GetHash();
		if ( path.GetHash() != c_prologue || socketName != RED_NAME_CONSTEXPR( "Q000 Main" ) )
		{
			return socketName;
		}
		const Int32 mode = WatsonStartMode();
		if ( mode <= 0 || s_fired.exchange( 1 ) )
		{
			return socketName;
		}
		struct Fact { const char* name; Int32 value; };
		static const Fact c_common[] = { { "q000_done", 1 }, { "radio_on", 1 }, { "tv_on", 1 }, { "air_traffic_off", 0 },
			{ "q000_hym_real_time", 1 }, { "q000_nomad_full_car_tutorial", 1 } };
		static const Fact c_apartment[] = { { "q001_mission0_complete", 1 }, { "q001_active", 1 } };
		auto& db = executionContext.GetNodeExecutionContext().GetFactsDB();
		auto set = [ &db ]( const Fact& f )
		{
			const Int32 before = Int32( db.Get( USER_TABLE_ID(), FactID( f.name ) ) );
			db.Set( USER_TABLE_ID(), FactID( f.name ), f.value );
			std::fprintf( stderr, "CP2077 watson: fact %s %d -> %d\n", f.name, before, f.value );
		};
		for ( const Fact& f : c_common ) set( f );
		if ( mode == 1 ) for ( const Fact& f : c_apartment ) set( f );
		const CName target = mode == 2 ? RED_NAME_CONSTEXPR( "Q001" ) : RED_NAME_CONSTEXPR( "Q001 Vroom" );
		std::fprintf( stderr, "CP2077 watson: prologue.questphase input 'Q000 Main' -> '%s' (entry=%s; q000 lifepath/montage/VR tutorial skipped)\n",
			target.AsChar(), mode == 2 ? "q001 Mission 0 (The Rescue)" : "apartment (q001_apartment wake-up)" );
		( void )phase;
		return target;
	}

	PhaseResult PhaseInstance::RunFromInputNode( ExecutionContext& executionContext, const CName socketNameIn, OutputSocketArray& outputSockets )
	{
		GAME_DATA_ERROR( m_graph, Quests, "No m_graph in PhaseInstance" );
		if ( !m_graph )
		{
			// return true to leave phase active so it can be seen in debugger
			return PhaseResult::InProgress;
		}

		const CName socketName = WatsonRedirectSocket( *this, m_resource ? m_resource->GetPath() : res::ResourcePath(), socketNameIn, executionContext ); // [watson]

		WeakHandle< NodeDefinition > inputNode;

		switch ( executionContext.GetActionType() )
		{
		case ExecutionActionType::Execution:
		case ExecutionActionType::Unfreeze:
		case ExecutionActionType::DebugCommandAwake:
		case ExecutionActionType::DebugCommandExecuteSingleNode:
		case ExecutionActionType::ExecuteSingleNode:
			inputNode = m_graph->GetIONode< InputNodeDefinition >( socketName );
			break;
		default:
			RED_ASSERT( false, "Unexpected action type" );
		}

		GAME_DATA_ERROR( !inputNode.Expired(), Quests, "No Input node found" );
		if ( inputNode.Expired() )
		{
			// return true to leave phase active so it can be seen in debugger
			return PhaseResult::InProgress;
		}

		return RunFromNode( executionContext, inputNode, InputSocket( NOTHING_SOCKET_NAME ), true, outputSockets );
	}

	PhaseResult PhaseInstance::RunFromNode( ExecutionContext& executionContext, const WeakHandle< NodeDefinition >& node, const InputSocket& inputSocket, Bool isIncomingSignal, OutputSocketArray& outputSockets )
	{
		RED_ASSERT( !node.Expired(), "Node not found in graph!" );
		if ( node.Expired() )
		{
			// return true to leave phase active so it can be seen in debugger
			return PhaseResult::InProgress;
		}

		PushPhase( executionContext );

		PhaseResult ret = Process( executionContext, node, inputSocket, isIncomingSignal, outputSockets );
		PopPhase( executionContext );

		return ret;
	}

	Bool PhaseInstance::Stop( Uint32 index, ExecutionContext& executionContext, SignalKillingReason reason )
	{
		PushPhase( executionContext );

		if ( index >= m_childInstances.Size() )
		{
			return false;
		}

		m_childInstances[ index ]->OnRemove( executionContext, reason );

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( IsRoot() )
		{
			if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
			{
				debugger->OnQuestFinished( executionContext, m_childInstances[ index ].Get() );
			}
		}
#endif

		m_childInstances.RemoveAt( index );

		PopPhase( executionContext );

		return true;
	}

	Bool PhaseInstance::StopAll( ExecutionContext& executionContext, SignalKillingReason reason )
	{
		for ( auto& instance : m_childInstances )
		{
			instance->OnRemove( executionContext, reason );

#ifdef RED_ENABLE_QUEST_DEBUGGER
			if ( IsRoot() )
			{
				if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
				{
					debugger->OnQuestFinished( executionContext, instance.Get() );
				}
			}
#endif
		}
		m_childInstances.Clear();

		return true;
	}

	Bool PhaseInstance::CanInstanceNotExist( const ExecutionContext& executionContext ) const
	{
		return	executionContext.GetActionType() == ExecutionActionType::DebugCommandAwake ||
				executionContext.GetActionType() == ExecutionActionType::DebugCommandExecuteSingleNode ||
				executionContext.GetActionType() == ExecutionActionType::ExecuteSingleNode;
	}

	PhaseInstance* PhaseInstance::FindInstanceByNodeId( NodeId nodeId ) const
	{
		PathHash childPathHash = HashedNodePath::CreateChildPathHash( m_fullPathHash, nodeId );

		for ( auto& instance : m_childInstances )
		{
			if ( instance->GetFullPathHash() == childPathHash )
			{
				return instance.Get();
			}
		}
		return nullptr;
	}

	THandle< PhaseInstance > PhaseInstance::CreateChildInstance( ExecutionContext& executionContext, const THandle< QuestPhaseResource > resource, const THandle< GraphDefinition > graph, GenericId genericId, const String& name )
	{
		THandle< PhaseInstance > childInstance = CreateHandle< PhaseInstance >();

		m_childInstances.PushBack( childInstance );
		childInstance->OnCreate( executionContext, resource, graph, m_fullPath, genericId, name );

		return childInstance;
	}

	void PhaseInstance::RemoveChildInstance( ExecutionContext& executionContext, const THandle< PhaseInstance >& childInstance, SignalKillingReason reason )
	{
		childInstance->OnRemove( executionContext, reason );

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( dbg::QuestDebugger* debugger = executionContext.GetDebugger() )
		{
			debugger->OnPhaseFinished( executionContext, childInstance.Get() );
		}
#endif

		m_childInstances.Remove( childInstance );
	}

	Bool PhaseInstance::HasChild( const PhaseInstance* childInstance ) const
	{
		return m_childInstances.Exist( HandleFromPtr( childInstance ) );
	}

	Uint32 PhaseInstance::GetChildrenCount() const
	{
		return m_childInstances.Size();
	}

	THandle< PhaseInstance> PhaseInstance::GetChild( Uint32 childIdx ) const
	{
		RED_ASSERT( childIdx < m_childInstances.Size(), "Trying to access invalid phase instance child no #%u", childIdx );

		return m_childInstances[childIdx];
	}

	THandle< PhaseInstance > PhaseInstance::GetChildById( NodeId id ) const
	{
		for ( const auto& childInstance : m_childInstances )
		{
			const auto& fullPath = childInstance->GetFullPath();
			if ( fullPath.Size() > 1 )
			{
				if ( id == fullPath[ fullPath.Size() - 1 ] )
				{
					return childInstance;
				}
			}
		}

		return nullptr;
	}

	THandle< PhaseInstance > PhaseInstance::GetChildByIdEx( NodeId id ) const
	{
		for ( const auto& childInstance : m_childInstances )
		{
			const auto& fullPath = childInstance->GetFullPath();
			if ( fullPath.Size() > 0 )
			{
				if ( id == fullPath[ fullPath.Size() - 1 ] )
				{
					return childInstance;
				}
			}
		}

		return nullptr;
	}

	THandle< NodeDefinition > PhaseInstance::FindNodeById( NodeId nodeId ) const
	{
		return m_graph->GetNodeById( nodeId ).ToHandle();
	}

	Bool PhaseInstance::CanPhaseBeFrozen_Internal() const
	{
		for ( const auto& listener : m_listeners )
		{
			NodeId nodeId = listener.Key().GetNodeId();
			if ( auto node = m_graph->GetNodeById( nodeId ).ToHandle() )
			{
				auto classType = node->GetClass();

				// can't check with RTTI because of project dependencies
				if ( classType->GetName() == RED_NAME_CONSTEXPR_NOREG( "questSceneNodeDefinition" ) )
				{
					rtti::ValuePtr value;
					if ( classType->ReadValue( IRTTIContext::GetDefault(), node.Get(), rtti::AccessPath( "notAllowedToBeFrozen" ), value ) )
					{
						Bool notAllowedToBeFrozen = FromStringDirect< Bool >( value->GetSingleValue() );
						if ( notAllowedToBeFrozen )
						{
							return false;
						}
					}
				}
			}
		}

		for ( const auto& childInstance : m_childInstances )
		{
			if ( !childInstance->CanPhaseBeFrozen_Internal() )
			{
				return false;
			}
		}

		return true;
	}

	void PhaseInstance::AddEventListener( ExtendedNodeId extendedNodeId, const red::SharedPtr< EventListenerWrapper >& listener )
	{
		GAME_ASSERT( !m_listeners.KeyExist( extendedNodeId ), Quests, "Some listener is already assigned to the NodeId [%d]", extendedNodeId.GetNodeId() );

		m_listeners[ extendedNodeId ] = listener;
	}

	void PhaseInstance::RemoveEventListener( ExtendedNodeId extendedNodeId, ExecutionContext& executionContext )
	{
		auto foundIt = m_listeners.Find( extendedNodeId );
		GAME_ASSERT( foundIt != m_listeners.End(), Quests, "No listener is assigned to the NodeId [%d]", extendedNodeId.GetNodeId() );

		auto found = foundIt.Value();
		GAME_ASSERT( found, Quests, "No listener is assigned to the NodeId [%d]", extendedNodeId.GetNodeId() );

		auto& context = executionContext.GetNodeExecutionContext();

		found.Get()->OnDestroy( context );
		m_listeners.Remove( foundIt );

		auto& questsSystem = context.GetGameInstance().GetSystem< IQuestsSystem >();
		questsSystem.PostponeListenerDeletion( std::move( found ) );
	}

	EventListenerWrapper* PhaseInstance::FindEventListener( ExtendedNodeId extendedNodeId ) const
	{
		if ( extendedNodeId.GetUniqueId() == DEFAULT_GENERIC_ID )
		{
			// if it is a root listener, just find it in the map
			auto found = m_listeners.FindPtr( extendedNodeId );
			if ( found )
			{
				return found->Get();
			}
			// it may not exist, if we check if condition is fulfilled for the first time
			return nullptr;
		}

		// if it is a child listener, get the root listener first
		auto rootListener = m_listeners.FindPtr( ExtendedNodeId( extendedNodeId.GetNodeId(), DEFAULT_GENERIC_ID ) );
		if ( rootListener )
		{
			// and then look for child listener inside
			return rootListener->Get()->FindEventListener( extendedNodeId );
		}
		return nullptr;
	}

	void PhaseInstance::DisableNode( ExecutionContext& executionContext, const HashedNodePath& hashedPath, Bool disable )
	{
		// set runtime data (for quicker access in NodeDefinition::Execute function)
		if ( disable )
		{
			if ( !m_disabledNodes.Exist( hashedPath.GetExtendedNodeId().GetNodeId() ) )
			{
				m_disabledNodes.Insert( hashedPath.GetExtendedNodeId().GetNodeId() );
			}
		}
		else
		{
			m_disabledNodes.Remove( hashedPath.GetExtendedNodeId().GetNodeId() );
		}

		//set factsdb (so runtime data can be restored when loading a savegame)
		auto& questDB = executionContext.GetNodeExecutionContext().GetFactsDB();
		quest::SetFact< Bool >( questDB, DISABLED_NODES_TABLE_ID(), hashedPath.CalculateHash32(), disable );
	}

	Bool PhaseInstance::IsNodeDisabled( NodeId nodeId ) const
	{
		return m_disabledNodes.Exist( nodeId );
	}

	NodeId PhaseInstance::GetDeletedNodeId( NodeId deletionMarkerNodeId ) const
	{
		if ( auto nodeIdPtr = m_deletionMarkerMapping.FindPtr( deletionMarkerNodeId ) )
		{
			return *nodeIdPtr;
		}
		return INVALID_QUEST_NODE_ID;
	}

	String PhaseInstance::DebugGetFilename() const
	{
		if ( IsRoot() )
		{
			return "<root>";
		}
		if ( m_resource )
		{
			return m_resource->GetPath().ToString();
		}
		return "<none>";
	}

	void PhaseInstance::OnRenderDebugShowInstances( rend::FragmentBuilder& builder, Uint32 depth, Int32 x, Int32& y ) const
	{
		const Int32 INDENT_COEF = 8;
		const Int32 LINE_HEIGHT = 15;

		if ( IsRoot() )
		{
			builder.SetColor( Color::YELLOW() );
			builder.AddFlatTextf( x, y, "PHASE INSTANCE TREE" );
			y += LINE_HEIGHT;
		}
		builder.SetColor( Color::WHITE() );

		if ( depth == 0 )
		{
			builder.AddFlatTextf( x, y, DebugGetFilename().AsChar() );
			builder.AddFlatTextf( x + 600, y, "%s", ToString().AsChar() );
		}
		else
		{
			builder.AddFlatTextf( x, y, "%*s%s", depth * INDENT_COEF, " ", DebugGetFilename().AsChar() );
			builder.AddFlatTextf( x + 600, y, "%s", ToString().AsChar() );
		}
		y += LINE_HEIGHT;

		for ( auto& instance : m_childInstances )
		{
			instance->OnRenderDebugShowInstances( builder, depth + 1, x, y );
		}
	}

	String PhaseInstance::ToString() const
	{
		String nodePath;

		for ( Uint32 i = 0; i < m_fullPath.Size(); ++i )
		{
			nodePath += String::Printf( "%u", m_fullPath[ i ] );
			if ( i < m_fullPath.Size() - 1 )
			{
				nodePath += " ";
			}
		}

		return String::Printf( "(%d) [%s]", m_listeners.Size(), nodePath.AsChar() );
	}

#ifdef RED_ENABLE_QUEST_DEBUGGER

	void PhaseInstance::AddSingleExecutionNode( HashedNodePath hashedPath )
	{
		if ( !m_singleExecutionNodes.Exist( hashedPath ) )
		{
			m_singleExecutionNodes.PushBack( hashedPath );
		}
	}

	Bool PhaseInstance::RemoveSingleExecutionNode( HashedNodePath hashedPath )
	{
		auto res = m_singleExecutionNodes.Remove( hashedPath );
		return res.IsSuccessful();
	}

#endif

	void PhaseInstance::PushPhase( ExecutionContext& executionContext )
	{
		executionContext.PushPhase( this );
	}

	void PhaseInstance::PopPhase( ExecutionContext& executionContext )
	{
		const PhaseInstance* currPhase = executionContext.PopPhase();
		GAME_ASSERT( currPhase == this, Quests, "Invalid phase popped from phase stack" );
	}

	PhaseResult PhaseInstance::Process( ExecutionContext& executionContext, const WeakHandle< NodeDefinition >& node, const TypedSignalSocket& socket, Bool isIncomingSignal, OutputSocketArray& outputSockets )
	{
		//RED_LOG( "-------------------------------------------------------------------------------------------------" );

		SignalContext context( executionContext );

		SignalPosition signal( node, socket );
		if ( isIncomingSignal )
		{
			context.m_incomingSignals.PushBack( signal );
		}
		else
		{
			context.m_outcomingSignals.PushBack( signal );
		}

		Bool isNonTerminatingOutput = false;
		OutputSocketArray nonTeminatingOutputSockets;

		while ( !context.m_incomingSignals.Empty() ||
				!context.m_outcomingSignals.Empty() )
		{
			while ( !context.m_incomingSignals.Empty() )
			{
				// execute nodes
				PhaseResult ret = ProcessIncomingSignal( context, outputSockets );
				if ( ret == PhaseResult::Finished )
				{
					// signal left that phase, return Finished to remove the phase and kill everything inside the phase
					return ret;
				}
				else if ( ret == PhaseResult::InProgressAndFinished )
				{
					// signal is supposed to leave the phase, but things inside are not supposed to get killed, just set a bool to true, save output socket, finish processing other signals and then return InProgressAndFinished
					isNonTerminatingOutput = true;
					nonTeminatingOutputSockets = outputSockets;
					// PMTODO overriding nonTeminatingOutputSockets will cause that only the last non-terminating output will pass the signal to upper phase
				}
			}
			while ( !context.m_outcomingSignals.Empty() )
			{
				// push signal further
				ProcessOutcomingSignal( context );
			}
		}

		if ( isNonTerminatingOutput )
		{
			// handle postponed non-terminating output
			outputSockets = nonTeminatingOutputSockets;
			return PhaseResult::InProgressAndFinished;
		}

		// return true to not remove the phase
		return PhaseResult::InProgress;
	}

	// ---------------------------------------------------------------------------------------------------------
	// CP2077 web [nomadplay]: quest node trace on stderr ('[nomadplay] qn ...'). For phases whose file contains
	// 'q000' (and the root graph): PASS = node let the signal through at once, ENTER = node keeps the signal
	// (waits), LEAVE = a waiting node finally let it go. NomadplayQuestDump() (QuestsSystem::Tick) prints nodes
	// that waited > 5 s once ('WAIT') and the whole waiting list every 30 s. ?questlog=all logs every phase.
	// ---------------------------------------------------------------------------------------------------------
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

	struct NpQuestEntry { const void* phase; Uint32 id; Double t0; Bool waitLogged; std::string what; };
	static std::mutex s_npMutex;
	static std::vector< NpQuestEntry > s_npEntries;
	static std::unordered_map< const void*, std::pair< std::string, Bool > > s_npPhaseNames;
	static Int32 s_npAll = -1;
	Double NomadplayNowSeconds()
	{
#ifdef __EMSCRIPTEN__
		return emscripten_get_now() * 0.001;
#else
		return 0.0;
#endif
	}
	static const std::pair< std::string, Bool >& NpPhaseName( const PhaseInstance* phase, const res::ResourcePath& path )
	{
		auto it = s_npPhaseNames.find( phase );
		if ( it != s_npPhaseNames.end() )
		{
			return it->second;
		}
		static const char* const c_q000[] = { "q000", 
			"custom_multiple_objectives_tutorial",
			"q000_00_no_tell",
			"q000_character_creator",
			"q000_corpo",
			"q000_corpo_01_arasaka_tower",
			"q000_corpo_01b_flight_to_lizzies",
			"q000_corpo_02_lizzies_club",
			"q000_corpo_line_time_randomizer",
			"q000_debug_fadein",
			"q000_flux_capacitor",
			"q000_kid",
			"q000_kid_01_coyote",
			"q000_kid_02_parking_lot",
			"q000_kid_03_back_to_coyote",
			"q000_montage",
			"q000_nomad",
			"q000_nomad_01_garage",
			"q000_nomad_02_town",
			"q000_nomad_03_radio_tower",
			"q000_nomad_04_meeting_jackie",
			"q000_nomad_05_border_crossing",
			"q000_nomad_06_av_chase",
			"q000_nomad_07_finale",
			"q000_vr_melee_attacks_controller",
			"q000_vr_minigame_tutorial",
			"q000_vr_tutorial",
			"q000_vr_tutorial_reminder_handler" };
		static std::vector< std::pair< Uint64, std::string > > s_known;
		if ( s_npAll < 0 )
		{
			s_npAll = 0;
#ifdef __EMSCRIPTEN__
			s_npAll = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'questlog' ) === 'all' ? 1 : 0; } catch ( e ) { return 0; } } );
#endif
			for ( const char* n : c_q000 )
			{
				const std::string base = "base\\quest\\main_quests\\prologue\\q000\\";
				const std::string full = std::string( n ) == "q000" ? base + "q000.questphase" : base + "phases\\" + n + ".questphase";
				s_known.emplace_back( res::ResourcePath::Build( red::StringView( full.c_str() ) ).GetHash(), n );
				if ( std::string( n ) == "q000" )
				{
					s_known.emplace_back( res::ResourcePath::Build( red::StringView( ( base + "q000.quest" ).c_str() ) ).GetHash(), "q000.quest" );
				}
			}
		}
		std::string n;
		Bool on = s_npAll > 0;
		for ( const auto& k : s_known )
		{
			if ( k.first == path.GetHash() )
			{
				n = k.second;
				on = true;
				break;
			}
		}
		static std::vector< std::vector< Uint32 > > s_tracedRoots;
		const auto& fullPath = phase->GetFullPath();
		if ( on && !n.empty() && fullPath.Size() >= 2 )
		{
			std::vector< Uint32 > fp;
			for ( Uint32 i = 0; i < fullPath.Size(); ++i )
			{
				fp.push_back( Uint32( fullPath[ i ] ) );
			}
			s_tracedRoots.push_back( fp );
		}
		if ( n.empty() && fullPath.Size() > 0 )
		{
			for ( const auto& r : s_tracedRoots )
			{
				Bool prefix = r.size() < fullPath.Size();
				for ( Uint32 i = 0; prefix && i < r.size(); ++i )
				{
					prefix = r[ i ] == Uint32( fullPath[ i ] );
				}
				if ( prefix )
				{
					// [nomadplay] embedded sub-phase (no own resource) of a traced q000 phase
					on = true;
					n = "sub";
					for ( Uint32 i = 1; i < fullPath.Size(); ++i )
					{
						n += "." + std::to_string( Uint32( fullPath[ i ] ) );
					}
					break;
				}
			}
		}
		if ( n.empty() )
		{
			n = phase->DebugGetFilename().AsChar();
			const std::string friendly = phase->GetName().AsChar();
			if ( !friendly.empty() )
			{
				n += "(" + friendly + ")";
			}
			if ( n.find( "q000" ) != std::string::npos )
			{
				on = true;
			}
		}
		static Int32 s_npPhaseLogs = 0;
		if ( on || s_npPhaseLogs < 16 )
		{
			++s_npPhaseLogs;
			std::fprintf( stderr, "[nomadplay] qphase %s traced=%d hash=%016llx\n", n.c_str(), on ? 1 : 0, ( unsigned long long )path.GetHash() );
		}
		return s_npPhaseNames.emplace( phase, std::make_pair( n, on ) ).first->second;
	}
	static void NpTrace( const PhaseInstance* phase, const res::ResourcePath& path, const NodeDefinition& node, NodeResult ret )
	{
		if ( !NpLogOn() )
		{
			return;
		}
		std::lock_guard< std::mutex > lock( s_npMutex );
		const auto& pn = NpPhaseName( phase, path );
		if ( !pn.second )
		{
			return;
		}
		const Uint32 id = Uint32( node.GetId() );
		const Bool stays = ( ret == NodeResult::StayInNode || ret == NodeResult::StayAndLeaveNode );
		const Double now = NomadplayNowSeconds();
		for ( size_t i = 0; i < s_npEntries.size(); ++i )
		{
			NpQuestEntry& e = s_npEntries[ i ];
			if ( e.phase == phase && e.id == id )
			{
				if ( !stays )
				{
					std::fprintf( stderr, "[nomadplay] qn LEAVE %s#%u after %.1fs %s ret=%d\n", pn.first.c_str(), id, now - e.t0, e.what.c_str(), Int32( ret ) );
					s_npEntries.erase( s_npEntries.begin() + i );
				}
				return;
			}
		}
		std::string what = node.GetClass()->GetName().AsChar();
		const String desc = node.GetDescription();
		if ( !desc.Empty() )
		{
			what += " '";
			what += std::string( desc.AsChar() ).substr( 0, 200 );
			what += "'";
		}
		for ( char& c : what )
		{
			if ( c == '\n' || c == '\r' || c == '\t' )
			{
				c = ' ';
			}
		}
		std::fprintf( stderr, "[nomadplay] qn %s %s#%u %s ret=%d\n", stays ? "ENTER" : "PASS", pn.first.c_str(), id, what.c_str(), Int32( ret ) );
		if ( stays )
		{
			s_npEntries.push_back( NpQuestEntry{ phase, id, now, false, what } );
		}
	}
	void NomadplayQuestDump( Bool all )
	{
		std::lock_guard< std::mutex > lock( s_npMutex );
		const Double now = NomadplayNowSeconds();
		for ( NpQuestEntry& e : s_npEntries )
		{
			const Double age = now - e.t0;
			if ( ( all && age > 2.0 ) || ( !e.waitLogged && age > 5.0 ) )
			{
				auto it = s_npPhaseNames.find( e.phase );
				std::fprintf( stderr, "[nomadplay] qn %s %s#%u waiting %.0fs %s\n", all ? "STILL" : "WAIT", it != s_npPhaseNames.end() ? it->second.first.c_str() : "?", e.id, age, e.what.c_str() );
				e.waitLogged = true;
			}
		}
	}
	void NomadplayQuestForget( const void* phase )
	{
		std::lock_guard< std::mutex > lock( s_npMutex );
		for ( size_t i = s_npEntries.size(); i-- > 0; )
		{
			if ( s_npEntries[ i ].phase == phase )
			{
				s_npEntries.erase( s_npEntries.begin() + i );
			}
		}
		s_npPhaseNames.erase( phase );
	}

	NodeResult PhaseInstance::ProcessNode( const NodeDefinition& node, ExecutionContext& executionContext, const InputSocket& inputSocket, OutputSocketArray& outputSockets )
	{
		GAME_ASSERT( executionContext.GetCurrentPhase() == this, Quests, "Wrong phase: node can only be processed in current phase." );

		if( node.IsA< DisableableNodeDefinition >() && IsNodeDisabled( node.GetId() ) )
		{
			// return false to kill signal here
			return NodeResult::StayInNode;
		}

		NodeResult ret = node.PreExecute( executionContext.GetNodeExecutionContext(), inputSocket );
		if ( ret == NodeResult::StayInNode )
		{
			return NodeResult::StayInNode;
		}

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if( executionContext.GetActionType() == ExecutionActionType::DebugCommandExecuteSingleNode )
		{
			AddSingleExecutionNode( node.CreateHashedNodePath( executionContext.GetNodeExecutionContext() ) );
		}
#endif

#ifdef RED_ENABLE_QUEST_DEBUGGER
		dbg::QuestDebugger* debugger = executionContext.GetDebugger();
		if( debugger )
		{
			const Bool resetFailed = ( inputSocket.GetName() != NOTHING_SOCKET_NAME );
			if( !debugger->OnBlockStarted( executionContext, &node, resetFailed ) )
			{
				// probably we hit breakpoint
				return NodeResult::StayInNode;
			}
		}
#endif

		// should be removed in final version of the game.
		THandle< FunctionalTests::IGameSystem > functionalTestsGameSystem = executionContext.GetGameInstance().GetSystemPtr<FunctionalTests::IGameSystem>();
		if( functionalTestsGameSystem != nullptr )
		{
			functionalTestsGameSystem->OnQuestBlockStarted( executionContext, &node );
		}

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		auto hashedNodePath = node.CreateHashedNodePath( executionContext.GetNodeExecutionContext() );

		auto performanceMonitor = executionContext.GetPerformanceMonitor();
		if ( performanceMonitor )
		{
			performanceMonitor->OnNodeExecutionStarted( hashedNodePath, executionContext.GetOperationIndex() );
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

		ret = node.Execute( executionContext.GetNodeExecutionContext(), inputSocket, outputSockets );

		if ( executionContext.GetActionType() == ExecutionActionType::Unfreeze )
		{
			if ( ret == NodeResult::LeaveNode )
			{
				// when we leave the node during unfreezing, make sure to change action type to regular Execution
				// as the signal may get to the scene node (where behaves differently depending whether it's Unfreeze or Execution)
				executionContext.ForceSetActionType( ExecutionActionType::Execution );
			}
		}

#ifdef RED_ENABLE_QUEST_PERFORMANCE_MONITOR
		if ( performanceMonitor )
		{
			performanceMonitor->OnNodeExecutionFinished( hashedNodePath );
		}
#endif //RED_ENABLE_QUEST_PERFORMANCE_MONITOR

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( ( ret == NodeResult::LeaveNode || ret == NodeResult::Kill ) && debugger )
		{
			debugger->OnBlockFinished( executionContext, &node );
		}
#endif

		// should be removed in final version of the game.
		if( functionalTestsGameSystem != nullptr && ( ret == NodeResult::LeaveNode || ret == NodeResult::Kill ))
		{
			functionalTestsGameSystem->OnQuestBlockFinished( executionContext, &node );
		}

		ret = node.PostExecute( executionContext.GetNodeExecutionContext(), ret, outputSockets );
		NpTrace( this, m_resource ? m_resource->GetPath() : res::ResourcePath(), node, ret ); // [nomadplay]

#ifdef RED_ENABLE_QUEST_DEBUGGER
		if ( ( ret == NodeResult::LeaveNode || ret == NodeResult::Kill ) && RemoveSingleExecutionNode( node.CreateHashedNodePath( executionContext.GetNodeExecutionContext() ) ) )
		{
			ret = NodeResult::Kill;
		}
#endif

		if ( executionContext.GetActionType() == ExecutionActionType::ExecuteSingleNode )
		{
			ret = NodeResult::Kill;
		}

		return ret;
	}

	PhaseResult PhaseInstance::ProcessIncomingSignal( SignalContext& context, OutputSocketArray& outputSockets )
	{
		RED_ASSERT( !context.m_incomingSignals.Empty(), "m_incomingSignals are empty!" );

		// get the first incoming signal and delete it from the array
		const SignalPosition incomingSignal = context.m_incomingSignals.Front();
		context.m_incomingSignals.RemoveAt( 0 );

		// execute node
		THandle< NodeDefinition > incomingSignalNode = incomingSignal.m_node.ToHandle();
		GAME_ASSERT( incomingSignalNode, Quests, "Invalid node in PhaseInstance::ProcessIncomingSignal" );

		NodeResult ret = ProcessNode( *incomingSignalNode, context.m_executionContext, incomingSignal.m_signal, outputSockets );
		if( ret == NodeResult::LeaveNode )
		{
			// if ProcessNode() returns true, push the signal further
			auto outputNode = Cast< OutputNodeDefinition >( incomingSignalNode.Get() );
			if( outputNode )
			{
				// if executed node is the OutputNodeDefinition one, this phase instance should be left and deleted and the signal should go back to upper phase instance
				GAME_ASSERT( outputSockets.Size() == 1, Quests, "OutputNodeDefinition should have only one output" );
				outputSockets.Clear();
				outputSockets.AddOutput( outputNode->GetSocketName() );
				if ( outputNode->IsNonTerminating() )
				{
					return PhaseResult::InProgressAndFinished;
				}
				return PhaseResult::Finished;
			}
		}

		if( ret == NodeResult::LeaveNode || ret == NodeResult::StayAndLeaveNode )
		{
			for( const auto& outputSocket : outputSockets )
			{
				if( outputSocket.GetName() != NOTHING_SOCKET_NAME )
				{
					// push signal to array of outcoming signals
					SignalPosition outcomingSignal( incomingSignal.m_node, outputSocket );
					// PMTODO what about doubled signals?
					context.m_outcomingSignals.PushBack( outcomingSignal );
				}
			}
		}
		// node has been executed, progress the signal further
		return PhaseResult::InProgress;
	}
	
	void PhaseInstance::ProcessOutcomingSignal( SignalContext& context ) const
	{
		RED_ASSERT( !context.m_outcomingSignals.Empty(), "m_outcomingSignals are empty!" );

		// get the first outcoming signal and delete it from the array
		const SignalPosition outcomingSignal = context.m_outcomingSignals.Front();
		context.m_outcomingSignals.RemoveAt( 0 );

		// get all sockets connected to output socket
		graph::GraphSocketArray sockets{ red::PoolFrame() };
		THandle< NodeDefinition > outcomingSignalNode = outcomingSignal.m_node.ToHandle();
		GAME_ASSERT( outcomingSignalNode, Quests, "Invalid node in PhaseInstance::ProcessOutcomingSignal" );
		Bool socketExists = outcomingSignalNode->GetConnectedSockets( outcomingSignal.m_signal.GetName(), false, true, sockets );
		outcomingSignalNode->OnValidateOutputConnection( socketExists, outcomingSignal.m_signal.GetName() );

		for ( auto& socket : sockets )
		{
			GAME_ASSERT( socket, Quests, "Node is connected to a socket that does not exist [%hs%d]", context.m_executionContext.GetNodeExecutionContext().DebugGetFullPathString().AsChar(), outcomingSignalNode->GetId() );
			GAME_ASSERT( socket->GetParentNode().ToHandle(), Quests, "Node is connected to a socket that has no parent [%hs%d]", context.m_executionContext.GetNodeExecutionContext().DebugGetFullPathString().AsChar(), outcomingSignalNode->GetId() );
			SignalPosition incomingSignal( Cast< NodeDefinition >( socket->GetParentNode() ), socket->GetName(), outcomingSignal.m_signal.GetType() );
			// PMTODO what about doubled signals?
			context.m_incomingSignals.PushBack( incomingSignal );
		}
	}

	void PhaseInstance::OnCreate( ExecutionContext& executionContext, const THandle< QuestPhaseResource > resource, const THandle< GraphDefinition > graph, const FullPath& parentFullPath, GenericId nodeId, const String& name )
	{
		GAME_ASSERT(	parentFullPath.Empty() || executionContext.GetCurrentPhase()->FindNodeById( nodeId )->IsA< PhaseNodeDefinition >(),
						Quests,
						"Phase node must be of PhaseNodeDefinition type: path [%hs], nodeId [%d], node type [%hs]",
						DebugGetFullPathString( parentFullPath ).AsChar(),
						nodeId,
						DebugGetNodeClassName( executionContext, nodeId ) );
	
		m_onLoadGamePrefabsLoadRequestId = 0;

		PushPhase( executionContext );

		m_resource = resource;
		m_graph = graph;
#ifdef RED_ENABLE_PHASE_INSTANCE_NAMES
		m_name = name;
#endif //RED_ENABLE_PHASE_INSTANCE_NAMES

		// m_resource is null if graph is embedded and not from separate resource
		// m_graph cannot be null
		GAME_DATA_ERROR( m_graph, Quests, "No m_graph in PhaseInstance" );

		m_fullPath = parentFullPath;
		m_fullPath.PushBack( nodeId );
		m_fullPathHash = HashedNodePath::CreatePathHash( m_fullPath );

		auto& nodePaths = executionContext.GetNodePathMap();
		if ( !nodePaths.KeyExist( m_fullPathHash ) )
		{
			nodePaths.Insert( m_fullPathHash, m_fullPath );
		}

		// PMTODO
		// the following lines are not necessary ATM, will be needed for saving/loading
		// so let's just make sure, they are unique right now in case a phase is revisited
		auto& activeInstances = executionContext.GetActiveInstancesArray();
		if ( !activeInstances.Exists( m_fullPath ) )
		{
			activeInstances.PushBack( m_fullPath );
		}

		// restore runtime data from FactsDB when revisiting a phase
		// and create Deletion Marker mappings
		if ( m_graph )
		{
			auto& questDB = executionContext.GetNodeExecutionContext().GetFactsDB();
			for ( graph::NodeIterator nodeIterator( *m_graph ); nodeIterator; ++nodeIterator )
			{
				// notify all nodes that the signal is entering phase instance and they should clean things up in FactsDB
				auto commonNode = Cast< NodeDefinition >( *nodeIterator );
				if ( commonNode )
				{
					commonNode->OnEnteringPhase( executionContext.GetNodeExecutionContext() );
				}

				// restore disabled state
				auto disableableNode = Cast< DisableableNodeDefinition >( *nodeIterator );
				if ( disableableNode )
				{
					auto hashedPath = disableableNode->CreateHashedNodePath( executionContext.GetNodeExecutionContext() );
					auto hash32 = hashedPath.CalculateHash32();
					const Bool disabled = GetFact< Bool >( questDB, DISABLED_NODES_TABLE_ID(), hash32 );

					if ( disabled )
					{
						if ( !m_disabledNodes.Exist( disableableNode->GetId() ) )
						{
							m_disabledNodes.Insert( disableableNode->GetId() );
						}
					}
				}

				// create mappings [deleted NodeId] -> [Deletion Marker NodeId]
				auto deletionMarkerNode = Cast< DeletionMarkerNodeDefinition >( *nodeIterator );
				if ( deletionMarkerNode )
				{
					const auto& deletedNodeIds = deletionMarkerNode->GetDeletedNodeIds();
					
					NodeId deletionMarkerNodeId = deletionMarkerNode->GetId();
					for ( const auto deletedNodeId : deletedNodeIds )
					{
						RED_FATAL_ASSERT( !m_deletionMarkerMapping.KeyExist( deletedNodeId ) );
						m_deletionMarkerMapping[ deletedNodeId ] = deletionMarkerNodeId;
					}
				}
			}
		}

		PopPhase( executionContext );
	}

	void PhaseInstance::OnRemove( ExecutionContext& executionContext, SignalKillingReason reason )
	{
		NomadplayQuestForget( this ); // [nomadplay]
		PushPhase( executionContext );

		// notify nodes that have signal that it should be killed
		while ( !m_listeners.Empty() )
		{
			ExtendedNodeId extendedNodeId = m_listeners.Begin().Key();
			NodeId nodeId = extendedNodeId.GetNodeId();
			auto node = GetGraph()->GetNodeById( nodeId ).ToHandle();
			GAME_ASSERT( node, Quests, "Node %u not found", nodeId );
			auto signalStoppingNode = Cast< SignalStoppingNodeDefinition >( node );
			GAME_ASSERT( signalStoppingNode, Quests, "Node %u is not a signal stopping one", nodeId );

			signalStoppingNode->KillSignal( executionContext.GetNodeExecutionContext(), false, reason ); // false, killing signal when leaving a phase is never permanent
		}

		// notify all nodes that the signal is leaving corresponding phase instance and they should clean things up in FactsDB
		GAME_DATA_ERROR( m_graph, Quests, "No m_graph in PhaseInstance, is a quest empty?" );
		if ( m_graph )
		{
			for ( graph::NodeIterator nodeIterator( *m_graph ); nodeIterator; ++nodeIterator )
			{
				auto node = Cast< NodeDefinition >( *nodeIterator );
				if ( node )
				{
					node->OnLeavingPhase( executionContext.GetNodeExecutionContext(), reason );
				}
			}
		}

		// remove any child phase instances
		while ( !m_childInstances.Empty() )
		{
			RemoveChildInstance( executionContext, m_childInstances[ 0 ], reason );
		}

		// if it loaded any prefabs - unload them
		if ( m_onLoadGamePrefabsLoadRequestId )
		{
			executionContext.GetNodeExecutionContext().GetQuestPrefabsHandler()->DisablePrefabsStreaming( m_onLoadGamePrefabsLoadRequestId );
		}

		PopPhase( executionContext );

		// unload a resource (if there is any) associated with this phase
		if ( m_resource )
		{
			auto resourcePath = m_resource->GetPath();

			auto resourceLoader = executionContext.GetNodeExecutionContext().GetResourceLoader();
			resourceLoader->UnloadResource( resourcePath );
		}

		InformPhaseNodeAboutPhaseInstanceRemoved( executionContext, reason );

		//PMTODO when phase is left, consider removing its path from from the hashmap executionContext.GetNodePathMap() and array executionContext.GetActiveInstancesArray()
	}

	void PhaseInstance::InformPhaseNodeAboutPhaseInstanceRemoved( ExecutionContext& executionContext, SignalKillingReason reason )
	{
		if ( !IsRoot() )
		{
			const PhaseInstance* parentInstance = executionContext.GetCurrentPhase();
			GAME_ASSERT( parentInstance, Quests, "this instance must have a parent" );
			GAME_ASSERT( parentInstance->GetGraph() || parentInstance->IsRoot(), Quests, "parent instance must have a valid graph" );
		
			const THandle< GraphDefinition > parentGraph = parentInstance->GetGraph();
			if ( parentGraph )
			{
				GAME_ASSERT( !m_fullPath.Empty(), Quests, "full path must contain at least the id of this phase's node" );
				const NodeId phaseNodeId = m_fullPath.Back();

				THandle< NodeDefinition > phaseNodeAsBaseClass = parentGraph->GetNodeById( phaseNodeId ).ToHandle();
				GAME_ASSERT( phaseNodeAsBaseClass, Quests, "this phase instance must have a phase node representation" );

				PhaseNodeDefinition* phaseNode = SafeCast< PhaseNodeDefinition >( phaseNodeAsBaseClass.Get() );
				phaseNode->OnPhaseInstanceRemoved( executionContext.GetNodeExecutionContext(), reason );
			}
		}
	}

	void PhaseInstance::FreezeInternal( ExecutionContext& executionContext, red::DynArray< FrozenNodeEntry >& frozenNodeEntries, const NodeId& selectedNodeId )
	{
		PushPhase( executionContext );

		for ( const auto listener : m_listeners )
		{
			NodeId currentNodeId = listener.Key().GetNodeId();
			if ( selectedNodeId == INVALID_QUEST_NODE_ID || selectedNodeId == currentNodeId )
			{
				HashedNodePath hashedNodePath = executionContext.GetNodeExecutionContext().CreateHashedNodePath( listener.Key() );

#ifdef RED_ENABLE_QUEST_FREEZER_DEBUG_DATA
				String debugClassName, debugIdPath, debugNamePath;

				const auto* questsSystem = executionContext.GetNodeExecutionContext().GetQuestsSystem();
				questsSystem->DebugGetPathStringsFromPathHash( hashedNodePath, debugClassName, debugIdPath, debugNamePath );
#endif

				red::DynArray< CName > dummyInputEntries{ game::PoolGMPL_QuestDynamic() };
				// PMTODO get input from prefab handler?

				frozenNodeEntries.PushBack( {	hashedNodePath
												, dummyInputEntries
#ifdef RED_ENABLE_QUEST_FREEZER_DEBUG_DATA
												, debugClassName
												, debugIdPath
												, debugNamePath
#endif
											} );

			}
		}

		for ( auto& childInstance : m_childInstances )
		{
			if ( selectedNodeId == INVALID_QUEST_NODE_ID )
			{
				childInstance->FreezeInternal( executionContext, frozenNodeEntries, INVALID_QUEST_NODE_ID );
			}
			else
			{
				const auto& childFullPath = childInstance->GetFullPath();
				RED_FATAL_ASSERT( !childFullPath.Empty() );
				{
					if ( selectedNodeId == childFullPath[ childFullPath.Size() - 1 ] )
					{
						childInstance->FreezeInternal( executionContext, frozenNodeEntries, INVALID_QUEST_NODE_ID );
					}
				}
			}
		}

		PopPhase( executionContext );
	}

} // namespace quest

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif

