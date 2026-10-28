// Copyright (c) 2019-2020 CD Projekt Red. All Rights Reserved.

#include <cstdio>
#include <mutex>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#endif
#include "build.h"
#include "scnChoiceHubSystem.h"

#include "scnChoiceFailsafe.h"
#include "scnChoiceHubMappingManager.h"
#include "scnChoiceHubSystemLoader.h"
#include "scnChoiceHubSystemRetrofix.h"
#include "scnChoiceNode.h"
#include "scnDebug.h"
#include "scnSceneSystem.h"
#include "scnsChoiceConditions.h"
#include "scnsChoiceHubMappinBuilder.h"
#include "scnsChoiceNodeExecutionContext.h"
#include "scnsChoiceNodeLookAtTypes.h"
#include "scnsSceneEntitiesManager.h"

#include "../../game/include/scanningController.h"
#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/gameVisionModeSystemInterface.h"
#include "../../gameCore/include/gameWatchdogSystemInterface.h"
#include "../../gameCore/include/interactionManagerInterface.h"
#include "../../gameInteractions/include/interactionChoice.h"
#include "../../gameInteractions/include/interactionDescriptorResource.h"
#include "../../gameInteractions/include/interactionHotSpot.h"
#include "../../gameInteractions/include/interactionHotSpotLayerDefinition.h"
#include "../../gameInteractions/include/interactionHotSpotSetupData.h"
#include "../../gameInteractions/include/interactionOrbTypes.h"
#include "../../gameInteractions/include/interactionVisualizerTypes.h"


namespace scn
{
	// CP2077 web [nomadplay]: scene-choice parts that never get a hub (their entity / world anchor did not map) are
	// invisible. Trace them, and with &choiceFailsafe=1 turn them into the engine's own failsafe (plain HUD) choice.
	static std::mutex s_npChoiceMutex;
	static std::vector< std::pair< const void*, Double > > s_npNoHub;
	static std::vector< bool > s_npNoHubLog;
	static Double NpChoiceNow()
	{
#ifdef __EMSCRIPTEN__
		return emscripten_get_now() * 0.001;
#else
		return 0.0;
#endif
	}
	static void NpChoicePartState( const void* part, Bool hasHub, Bool hasEntity, Uint32 flags )
	{
		std::lock_guard< std::mutex > lock( s_npChoiceMutex );
		for ( size_t i = 0; i < s_npNoHub.size(); ++i )
		{
			if ( s_npNoHub[ i ].first == part )
			{
				if ( hasHub )
				{
					if ( s_npNoHubLog[ i ] ) std::fprintf( stderr, "[nomadplay] choice part %p got a hub after %.1fs\n", part, NpChoiceNow() - s_npNoHub[ i ].second );
					s_npNoHub.erase( s_npNoHub.begin() + i );
					s_npNoHubLog.erase( s_npNoHubLog.begin() + i );
				}
				return;
			}
		}
		if ( !hasHub )
		{
			if ( flags & 1u ) std::fprintf( stderr, "[nomadplay] choice part %p has NO HUB (entity id defined=%d flags=0x%x)\n", part, hasEntity ? 1 : 0, flags );
			s_npNoHub.emplace_back( part, NpChoiceNow() );
			s_npNoHubLog.push_back( ( flags & 1u ) != 0 );
		}
	}
	static Double NpChoicePartNoHubSeconds( const void* part )
	{
		std::lock_guard< std::mutex > lock( s_npChoiceMutex );
		for ( const auto& e : s_npNoHub )
		{
			if ( e.first == part )
			{
				return NpChoiceNow() - e.second;
			}
		}
		return 0.0;
	}
	// 0 = off (&choiceFailsafe=0), 1 = default: only parts the designers flagged EnableNoEntityFailsafe (quest-important
	// choices; the original engine shows these as failsafe choices when its watchdog handles the fallback),
	// 2 = every part without a hub (&choiceFailsafe=all, test only).
	static Int32 NpForcedChoiceFailsafe()
	{
		static Int32 s_on = -1;
		if ( s_on < 0 )
		{
			s_on = 1;
#ifdef __EMSCRIPTEN__
			s_on = MAIN_THREAD_EM_ASM_INT( { try { const v = new URLSearchParams( location.search ).get( 'choiceFailsafe' ); return v === '0' ? 0 : ( v === 'all' ? 2 : 1 ); } catch ( e ) { return 1; } } );
#endif
			std::fprintf( stderr, "[nomadplay] forced scene-choice failsafe mode %d (0 off, 1 EnableNoEntityFailsafe parts [default], 2 all)\n", s_on );
		}
		return s_on;
	}
	//////////////////////////////////////////////////////////////////////////////////////
	//
	namespace
	{
		CName GetLinkedLayersName( const InteractionDescriptorBuilder& builder )
		{
			if( builder.IsScreen() )
			{
				return ChoiceConstants::c_screenLinkedLayersName;
			}
			else
			{
				return ChoiceConstants::c_entityLinkedLayersName;
			}
		}

		ChoiceHubInteractionLayer LayerTagToChoiceHubInteractionLayer( const game::interactions::CHotSpotLayerDefinition& layerDef )
		{
			if( layerDef.GetTag() == game::interactions::vis::c_indicationLayerName )
			{
				return ChoiceHubInteractionLayer::Indication;
			}
			else if( layerDef.GetTag() == game::interactions::vis::c_availabilityLayerName )
			{
				return ChoiceHubInteractionLayer::Availability;
			}
			else
			{
				return ChoiceHubInteractionLayer::Other;
			}
		}

		ChoiceHubInteractionLayer LayerGroupTypeToChoiceHubInteractionLayer( const game::interactions::CHotSpotLayerDefinition& layerDef )
		{
			if( layerDef.GetGroup() == game::interactions::EGroupType::EGT_hint )
			{
				return ChoiceHubInteractionLayer::Hint;
			}
			else
			{
				return ChoiceHubInteractionLayer::Other;
			}
		}
	}


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceInteractionListener::ChoiceInteractionListener( ChoiceHubSystem& ownerSystem, const ChoiceHubId& hubId )
		: m_ownerSystem{ ownerSystem }
		, m_hubId{ hubId }
	{}

	ChoiceInteractionListener::~ChoiceInteractionListener() = default;

	void ChoiceInteractionListener::OnInteractionEvent( const game::interactions::CInteractionEventDescription& description, const game::interactions::CHotSpot& parent, const game::interactions::CHotSpotLayerDefinition& hotSpotLayer, const game::interactions::CActivator& activator )
	{
		const game::interactions::EInteractionEventType eventType = description.GetEventType();
		if( m_ownerSystem.IsScreenChoiceHub( m_hubId ) )
		{
			const ChoiceHubInteractionLayer layerType = LayerTagToChoiceHubInteractionLayer( hotSpotLayer );
			if( layerType == ChoiceHubInteractionLayer::Availability )
			{
				NotifyOnInteraction( ChoiceHubInteractionLayer::Hint, eventType );
				NotifyOnInteraction( ChoiceHubInteractionLayer::Indication, eventType );
				NotifyOnInteraction( ChoiceHubInteractionLayer::Availability, eventType );
			}
		}
		else
		{
			NotifyOnInteraction( LayerTagToChoiceHubInteractionLayer( hotSpotLayer ), eventType );
			NotifyOnInteraction( LayerGroupTypeToChoiceHubInteractionLayer( hotSpotLayer ), eventType );
		}
	}

	void ChoiceInteractionListener::OnChoiceEvent( const game::interactions::CInteractionEventDescriptionChoice& description, const game::interactions::CHotSpot& parent, const game::interactions::CHotSpotLayerDefinition& hotSpotLayer, const game::interactions::CActivator& activator )
	{
		RED_ASSERT( description.GetChoice().m_data.Size() >= 1, "Expected choice data to contain at least one element - index of attempted option." );
		const Uint32 optionIndex = description.GetChoice().m_data[ 0 ].Get< Uint32 >();
		const ChoiceHubPartId choiceHubPartId = description.GetChoice().m_data[ 1 ].Get< ChoiceHubPartId >();
		m_ownerSystem.OnChoice( m_hubId, choiceHubPartId, optionIndex );
	}

	void ChoiceInteractionListener::NotifyOnInteraction( ChoiceHubInteractionLayer layerType, game::interactions::EInteractionEventType eventType )
	{
		if( layerType != ChoiceHubInteractionLayer::Other )
		{
			if( eventType == game::interactions::EInteractionEventType::EIET_activate )
			{
				NotifyOnInteraction( layerType, true );
			}
			else if( eventType == game::interactions::EInteractionEventType::EIET_deactivate )
			{
				NotifyOnInteraction( layerType, false );
			}
			else
			{
				RED_FATAL( "Other types are not supported" );
			}
		}
	}

	void ChoiceInteractionListener::NotifyOnInteraction( ChoiceHubInteractionLayer layerType, Bool isActivated )
	{
		if( layerType != ChoiceHubInteractionLayer::Other )
		{
			m_ownerSystem.OnInteraction( m_hubId, *this, layerType, isActivated );
		}
	}


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::BlockHubRequest::BlockHubRequest( const ChoiceHubId& hubId, const SceneInstanceId& blockingSciId )
		: m_hubId{ hubId }
		, m_blockingSceneInstanceId{ blockingSciId }
	{}

	ChoiceHubSystem::BlockHubRequest::~BlockHubRequest() = default;


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::HintAreaRequest::HintAreaRequest( const ChoiceHubId& hubId, Bool isAreaActivated )
		: m_hubId{ hubId }
		, m_isAreaActivated{ isAreaActivated }
	{}

	ChoiceHubSystem::HintAreaRequest::~HintAreaRequest() = default;


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::BlockedHubEntry::BlockedHubEntry() = default;

	ChoiceHubSystem::BlockedHubEntry::~BlockedHubEntry() = default;


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::ChoiceHubPart::ChoiceHubPart( ChoiceHubPartId partId, NodeId nodeId, const SceneInstanceId& sciInsId )
		: m_partId{ partId }
		, m_nodeId{ nodeId }
		, m_sceneInstanceId{ sciInsId }
	{}

	ChoiceHubSystem::ChoiceHubPart::~ChoiceHubPart() = default;


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::ChoiceHubData::ChoiceHubData( const ChoiceHubId& hubId )
		: m_hubId{ hubId }
	{}

	ChoiceHubSystem::ChoiceHubData::~ChoiceHubData() = default;


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::SceneListenerEntry::SceneListenerEntry( const SceneInstanceId& sciId )
		: m_sceneInstanceId{ sciId }
	{}


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::SceneImportantNotification::SceneImportantNotification( const SceneInstanceId& sceneInstanceId, Type type )
		: m_sceneInstanceId{ sceneInstanceId }
		, m_type{ type }
	{}


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::ExclusiveFocusClueData::ExclusiveFocusClueData() = default;
	
	ChoiceHubSystem::ExclusiveFocusClueData::ExclusiveFocusClueData( ent::EntityID entityId, const SceneInstanceId& sceneInstanceId, const NodeId& nodeId )
		: m_entityId{ entityId }
		, m_sceneInstanceId{ sceneInstanceId }
		, m_nodeId{ nodeId }
	{}


	//////////////////////////////////////////////////////////////////////////////////////
	//
	ChoiceHubSystem::ChoiceHubSystem( SceneSystem& sceneSystem, SceneEntitiesManager& entitiesManager, const game::IGameInstance& gameInstance )
		: m_sceneSystem{ sceneSystem }
		, m_interactionManager{ gameInstance.GetSystem< game::interactions::IManager >() }
		, m_scanningController{ gameInstance.GetSystem< game::IVisionModeSystem >().GetScanningController() }
		, m_mappingManager{ red::CreateUniquePtr< ChoiceHubMappingManager >( gameInstance, entitiesManager ) }
		, m_choiceNodeExecutionContext{ red::CreateUniquePtr< ChoiceNodeExecutionContext >( *static_cast< quest::IEventReceiver* >( this ), gameInstance ) }
		, m_choiceConditions{ red::CreateUniquePtr< ChoiceConditions >( *m_choiceNodeExecutionContext ) }
#if RED_CTSW( RED_GAME_FALLBACKS_ENABLED )
		, m_failsafeController{ red::CreateUniquePtr< ChoiceFailsafeController >( gameInstance ) }
#else
		, m_failsafeController{ red::CreateUniquePtr< NullChoiceFailsafeController >() }
#endif
	{
		auto removeBlocked = [ this ]( const SceneInstanceId& sceneInstanceId, ChoiceHubPartId partId )
		{
			if( ChoiceHubData* hubData = FindChoiceHub( partId ) )
			{
				if( hubData->m_blockingSceneInstanceId == sceneInstanceId )
				{
					RemoveHubBlocker( sceneInstanceId, *hubData );
					UpdateInteractionOptions( *hubData );
				}
			}
		};


		m_retrofixer = red::CreateUniquePtr< ChoiceHubSystemRetrofixer >( std::move( removeBlocked ) );
	}

	ChoiceHubSystem::~ChoiceHubSystem()
	{
		for( auto listener : m_registeredSceneListeners )
		{
			m_sceneSystem.DeregisterListener( listener.m_sceneInstanceId, *this );
		}
		m_registeredSceneListeners.Clear();
	}

	void ChoiceHubSystem::Init()
	{
		m_mappingManager->Init();
	}

	void ChoiceHubSystem::OnSceneInstanceStubLoaded( const SceneInstanceId& sceneInstanceId, SciStateName state )
	{
		if( m_loader )
		{
			RawExclusiveFocusClueEntry rawEntry;
			Bool isActive = false;

			if( m_loader->RetrieveRawExclusiveFocusClueEntry( sceneInstanceId, rawEntry, isActive ) )
			{
				// can only be active if scene is not interrupted (backward compatibility)
				if( isActive )
				{
					isActive &= state != SciStateName::itr;
				}

				AddExclusiveFocusClue( rawEntry.m_focusClueEntityId, sceneInstanceId, rawEntry.m_nodeId, isActive );
			}
		}
	}

	void ChoiceHubSystem::SaveState( ChoiceHubSystemSavedata& saveData )
	{
		red::DynArray< RawChoiceHubBlocker > rawBlockers{ PoolSceneSystem{} };

		if( m_loader )
		{
			rawBlockers.Reserve( m_loader->GetRawBlockersCount() + m_blockedScenes.Size() );
			m_loader->GetRawChoiceHubBlockers( rawBlockers );
		}
		else
		{
			rawBlockers.Reserve( m_blockedScenes.Size() );
		}
		
		for( const BlockedHubEntry& entry : m_blockedScenes )
		{
			const SceneId blockingSceneId = entry.m_blockingSceneInstanceId.GetSceneId();
			const SceneInstanceOwnerId blockingSceneOwnerId = entry.m_blockingSceneInstanceId.GetOwnerId();

			auto it = std::find_if( rawBlockers.Begin(), rawBlockers.End(), [ blockingSceneId, blockingSceneOwnerId ]( const RawChoiceHubBlocker& rawBlocker )
			{
				return blockingSceneId == rawBlocker.m_sceneId && blockingSceneOwnerId == rawBlocker.m_sceneOwnerId;
			} );

			RawChoiceHubBlocker* rawBlocker = nullptr;
			if( it == rawBlockers.End() )
			{
				rawBlocker = &rawBlockers.EmplaceBack( blockingSceneId, blockingSceneOwnerId );
			}
			else
			{
				rawBlocker = &*it;
			}

			ChoiceHubPartIdSpan partIds = m_mappingManager->GetParts( entry.m_blockedHubId );
			rawBlocker->m_nodes.Reserve( rawBlocker->m_nodes.Size() + partIds.Size() );

			for( const ChoiceHubPartId& partId : partIds )
			{
				const ChoiceHubPart& hubPart = GetChoiceHubPart( partId );
				rawBlocker->m_nodes.Insert( ChoiceHubNodeRawID{ 
					hubPart.m_sceneInstanceId.GetSceneId(),
					hubPart.m_sceneInstanceId.GetOwnerId(),
					hubPart.m_nodeId } 
				);
			}
		}

		saveData.WriteBlockers( rawBlockers );

		red::DynArray< RawExclusiveFocusClueEntry > rawExclusiveFocusClues{ PoolSceneSystem{} };
		rawExclusiveFocusClues.Reserve( m_storedExclusiveFocusClues.Size() );
		for( const ExclusiveFocusClueData& focusClueData : m_storedExclusiveFocusClues )
		{
			rawExclusiveFocusClues.EmplaceBack( focusClueData.m_sceneInstanceId, focusClueData.m_nodeId, focusClueData.m_entityId );
		}

		saveData.WriteExclusiveFocusClues( rawExclusiveFocusClues, m_hasActiveExclusiveFocusClue );
	}

	Bool ChoiceHubSystem::LoadState( const ChoiceHubSystemSavedata& saveData )
	{
		m_loader = red::CreateUniquePtr< ChoiceHubSystemLoader >( m_sceneSystem );
		if( !m_loader->LoadState( saveData ) )
		{
			return false;
		}

		return true;
	}

	void ChoiceHubSystem::Tick( Float deltaTime )
	{
		ProcessLoader();
		ProcessBlockHubRequests();
		ProcessImportantNotifications();
		ProcessFrozenScenes();
		ProcessStatusChanges();
		ProcessFocusClueActivity();
		ProcessHintActivatedHubs();
		ProcessRetrofixer( deltaTime );
		ProcessFailsafe( deltaTime );
	}

	game::interactions::OrbID ChoiceHubSystem::GetOrb( scn::NodeId nodeId ) const
	{
		const ChoiceHubPart* nodePart = FindChoiceHubPart( nodeId );
		if( nodePart )
		{
			if( const game::interactions::OrbID* orbId = m_orbMappings.FindPtr( nodePart->m_partId ) )
			{
				return *orbId;
			}
		}
		return game::interactions::OrbID{};
	}

	ChoiceHubRegisterResult ChoiceHubSystem::RegisterPerformerChoiceHub( const SceneEntityAssignmentId& assigmentId, const ChoiceHubInitData& hubInitData )
	{
		ChoiceHubPart& part = CreateChoiceHubPart( hubInitData );
		m_mappingManager->RegisterForPerformerUpdate( part.m_partId, assigmentId );
		return ChoiceHubRegisterResult{ part.m_partId };
	}

	ChoiceHubRegisterResult ChoiceHubSystem::RegisterObjectChoiceHub( const world::NodeRef& nodeRef, const ChoiceHubInitData& hubInitData )
	{
		ChoiceHubPart& part = CreateChoiceHubPart( hubInitData );
		m_mappingManager->RegisterForObjectUpdate( part.m_partId, nodeRef );
		return ChoiceHubRegisterResult{ part.m_partId };
	}

	ChoiceHubRegisterResult ChoiceHubSystem::RegisterDynamicObjectChoiceHub( const Transform& worldTransform, Float radius, const ChoiceHubInitData& hubInitData )
	{
		ChoiceHubPart& part = CreateChoiceHubPart( hubInitData );
		m_mappingManager->RegisterForDynamicObjectUpdate( part.m_partId, worldTransform, radius );
		return ChoiceHubRegisterResult{ part.m_partId };
	}

	ChoiceHubRegisterResult ChoiceHubSystem::RegisterPlayerChoiceHub( const red::String& groupName, const ChoiceHubInitData& hubInitData )
	{
		ChoiceHubPart& part = CreateChoiceHubPart( hubInitData );
		m_mappingManager->RegisterForPlayerUpdate( part.m_partId, groupName );
		return ChoiceHubRegisterResult{ part.m_partId };
	}

	void ChoiceHubSystem::UnregisterChoiceHub( const ChoiceHubPartId& hubPartId )
	{
		RED_ASSERT( m_parts.KeyExist( hubPartId ), "Failed to find choice hub part for id %d", hubPartId.GetId() );
		m_mappingManager->UnregisterChoiceHubPart( hubPartId );

		red::UniquePtr< ChoiceHubPart >& part = *m_parts.FindPtr( hubPartId );
		m_retrofixer->UnregisterChoiceHub( part->m_sceneInstanceId, part->m_nodeId, hubPartId );
	}

	void ChoiceHubSystem::UpdateOptions( const ChoiceHubPartId& hubPartId, const red::DynArray< ChoiceHubOption >& options )
	{
		ChoiceHubPart* hubPart = FindChoiceHubPart( hubPartId );
		RED_ASSERT( hubPart );

		hubPart->m_options = options;
		const ChoiceHubId hubId = m_mappingManager->FindHub( hubPartId );
		if( hubId )
		{
			UpdateInteractionOptions( GetChoiceHubData( hubId ) );
		}
	}

	game::interactions::OrbID ChoiceHubSystem::GetOrbID( const ChoiceHubPartId& hubPartId ) const
	{
		const ChoiceHubData* hubData = FindChoiceHub( hubPartId );
		RED_ASSERT( hubData, "Trying to get orb id for non-existing choice hub" );
		RED_ASSERT( hubData->IsInstalled(), "Trying to get orb id for uninstalled interaction" );

		if( const game::interactions::OrbID* orbId = m_orbMappings.FindPtr( hubPartId ) )
		{
			return *orbId;
		}
		else
		{
			return game::interactions::OrbID{};
		}
	}

	Bool ChoiceHubSystem::GetHighlightedOption( const ChoiceHubPartId& hubPartId, ent::EntityID playerEntityId, Uint32& outOptionIdx ) const
	{
		const ChoiceHubData* hubData = FindChoiceHub( hubPartId );
		RED_ASSERT( hubData, "Trying to get highlighted option for non-existing choice hub" );
		RED_ASSERT( hubData->m_ticket.IsValid(), "Trying to get highlighted option for uninstalled interaction" );
		if( hubData && hubData->m_ticket.IsValid() )
		{
			game::interactions::Choice highlightedChoice;
			if( m_interactionManager.GetVisualizerChoice( hubData->m_ticket, playerEntityId, GetLinkedLayersName( hubData->m_mergedInteractionDescriptor ), highlightedChoice ) )
			{
				const ChoiceHubPartId choicePartId = highlightedChoice.m_data[ 1 ].Get< ChoiceHubPartId >();

				// make sure the highlighted choice belongs to the part (choice merging)
				if( choicePartId == hubPartId )
				{
					outOptionIdx = highlightedChoice.m_data[ 0 ].Get< Uint32 >();
					return true;
				}
			}
		}
		return false;
	}

	Bool ChoiceHubSystem::IsScreenChoiceHub( const ChoiceHubId& hubId ) const
	{
		const ChoiceHubData& hubData = GetChoiceHubData( hubId );
		return hubData.m_mergedInteractionDescriptor.IsScreen();
	}

	void ChoiceHubSystem::NotifyActivate( const SceneInstanceId& scnInsId, ent::EntityID hubOwnerId )
	{
		const ChoiceHubId hubId = m_mappingManager->FindHubForEntity( hubOwnerId );
		if( hubId )
		{
			ChoiceHubData* hubData = m_hubs.FindPtr( hubId );
			RED_ASSERT( hubData, "Hub id %d is registered but there is no matching hub data. Entity %hs", hubId.GetId(), hubOwnerId.ToDebugString().AsChar() );
			if( hubData->m_blockingSceneInstanceId == scnInsId )
			{
				RemoveHubBlocker( hubData->m_blockingSceneInstanceId, *hubData );
				UpdateInteractionOptions( *hubData );
			}
		}
	}

	void ChoiceHubSystem::AddExclusiveFocusClue( ent::EntityID focusClueEntityId, const SceneInstanceId& sceneInstanceId, const NodeId& nodeId, Bool activate )
	{
		RED_ASSERT( sceneInstanceId );
		RED_ASSERT( !HasExclusiveFocusClueData( sceneInstanceId ) );

		m_storedExclusiveFocusClues.EmplaceBack( focusClueEntityId, sceneInstanceId, nodeId );
		RegisterSceneListener( sceneInstanceId );

		if( activate )
		{
			ActivateExclusiveFocusClue( sceneInstanceId );
		}
	}

	void ChoiceHubSystem::RemoveExclusiveFocusClue( const SceneInstanceId& sceneInstanceId )
	{
		const Uint32 index = FindExclusiveFocusClueDataIndex( sceneInstanceId );
		if( index != constants::c_invalidExclusiveFocusClueIndex )
		{
			RemoveExclusiveFocusClue( index );
		}
	}

	red::SharedPtr< ChoiceConditionsWrapper > ChoiceHubSystem::CreateConditionsWrapper( const SceneInstanceId& sceneInstanceId, const NodeId& nodeId, ConditionsWrapperCallback&& callback )
	{
		return m_choiceConditions->CreateWrapper( sceneInstanceId, nodeId, std::move( callback ) );
	}

	template< class TFunc >
	void ChoiceHubSystem::ForEachHubPart( const ChoiceHubId& hubId, TFunc&& func )
	{
		ChoiceHubPartIdSpan partIds = m_mappingManager->GetParts( hubId );
		for( const ChoiceHubPartId& partId : partIds )
		{
			ChoiceHubPart& hubPart = GetChoiceHubPart( partId );
			func( hubPart );
		}
	}

	void ChoiceHubSystem::OnInteraction( const ChoiceHubId& hubId, const ChoiceInteractionListener& listener, ChoiceHubInteractionLayer layer, Bool isActivated )
	{
		if( layer == ChoiceHubInteractionLayer::Hint )
		{
			auto accessToken = m_hintAreaRequests.AcquireAccess();
			HintAreaRequests& requests = accessToken.GetContents();
			requests.EmplaceBack( hubId, isActivated );
		}

		ForEachHubPart( hubId, [ layer, isActivated ]( const ChoiceHubPart& part ){
			part.m_onInteractionCallback( layer, isActivated );
		} );
	}

	void ChoiceHubSystem::OnChoice( const ChoiceHubId& hubId, const ChoiceHubPartId& partId, Uint32 optionIndex )
	{
		ChoiceHubPart* hubPart = FindChoiceHubPart( partId );
		RED_ASSERT( hubPart );

		hubPart->m_onChoiceSelectedCallback( optionIndex );

		auto accessToken = m_blockHubRequests.AcquireAccess();
		accessToken.GetContents().PushBack( { hubId, hubPart->m_sceneInstanceId } );
	}

	ChoiceHubSystem::ChoiceHubData& ChoiceHubSystem::GetOrCreateChoiceHub( const ChoiceHubId& hubId )
	{
		if( ChoiceHubData* hubData = m_hubs.FindPtr( hubId ) )
		{
			return *hubData;
		}
		else
		{
			return m_hubs.Insert( hubId, ChoiceHubData{ hubId } ).Iterator().Value();
		}
	}

	ChoiceHubSystem::ChoiceHubData& ChoiceHubSystem::GetChoiceHubData( const ChoiceHubId& hubId )
	{
		RED_ASSERT( m_hubs.KeyExist( hubId ), "Missing hub for id %d", hubId.GetId() );
		return *m_hubs.FindPtr( hubId );
	}

	const ChoiceHubSystem::ChoiceHubData& ChoiceHubSystem::GetChoiceHubData( const ChoiceHubId& hubId ) const
	{
		RED_ASSERT( m_hubs.KeyExist( hubId ), "Missing hub for id %d", hubId.GetId() );
		return *m_hubs.FindPtr( hubId );
	}

	ChoiceHubSystem::ChoiceHubData* ChoiceHubSystem::FindChoiceHub( const ChoiceHubPartId& hubPartId )
	{
		const ChoiceHubId hubId = m_mappingManager->FindHub( hubPartId );
		return m_hubs.FindPtr( hubId );
	}

	const ChoiceHubSystem::ChoiceHubData* ChoiceHubSystem::FindChoiceHub( const ChoiceHubPartId& hubPartId ) const
	{
		const ChoiceHubId hubId = m_mappingManager->FindHub( hubPartId );
		return m_hubs.FindPtr( hubId );
	}

	ChoiceHubSystem::ChoiceHubPart& ChoiceHubSystem::CreateChoiceHubPart( const ChoiceHubInitData& hubInitData )
	{
		const ChoiceHubPartId partId{ ++m_partIdGenerator };
		auto result = m_parts.Insert( partId, red::CreateUniquePtr< ChoiceHubPart >( partId, hubInitData.m_nodeId, hubInitData.m_sceneInstanceId ) );
		RED_ASSERT( result.IsSuccessful(), "Failed to add part" );

		ChoiceHubPart& newPart = *result.Iterator().Value();
		newPart.m_descriptor = hubInitData.m_descriptor;
		newPart.m_onStatusCallback = hubInitData.m_onStatusCallback;
		newPart.m_onInteractionCallback = hubInitData.m_onInteractionCallback;
		newPart.m_onChoiceSelectedCallback = hubInitData.m_onChoiceSelectedCallback;
		newPart.m_flags = hubInitData.m_flags;

		m_choiceHubPartFailsafes.Refresh( partId );

		if( enumUtils::HasFlag( hubInitData.m_flags, ChoiceHubFlags::FocusClue ) )
		{
			m_focusCluesParts.Insert( partId );
		}

		m_retrofixer->RegisterChoiceHub( hubInitData.m_sceneInstanceId, hubInitData.m_nodeId, partId );

		return newPart;
	}

	ChoiceHubSystem::ChoiceHubPart& ChoiceHubSystem::GetChoiceHubPart( const ChoiceHubPartId& hubPartId )
	{
		RED_ASSERT( m_parts.KeyExist( hubPartId ), "Hub part missing for part %d", hubPartId.GetId() );
		return *m_parts[ hubPartId ];
	}

	const ChoiceHubSystem::ChoiceHubPart& ChoiceHubSystem::GetChoiceHubPart( const ChoiceHubPartId& hubPartId ) const
	{
		RED_ASSERT( m_parts.KeyExist( hubPartId ), "Hub part missing for part %d", hubPartId.GetId() );
		return *m_parts[ hubPartId ];
	}

	ChoiceHubSystem::ChoiceHubPart* ChoiceHubSystem::FindChoiceHubPart( const ChoiceHubPartId& hubPartId )
	{
		auto it = m_parts.Find( hubPartId );
		if( it != m_parts.End() )
		{
			return it.Value().Get();
		}
		else
		{
			return nullptr;
		}
	}

	const ChoiceHubSystem::ChoiceHubPart* ChoiceHubSystem::FindChoiceHubPart( const ChoiceHubPartId& hubPartId ) const
	{
		auto it = m_parts.Find( hubPartId );
		if( it != m_parts.End() )
		{
			return it.Value().Get();
		}
		else
		{
			return nullptr;
		}
	}

	const ChoiceHubSystem::ChoiceHubPart* ChoiceHubSystem::FindChoiceHubPart( scn::NodeId nodeId ) const
	{
		// suboptimal but used only in debug draw in the editor
		auto it = std::find_if( m_parts.Begin(), m_parts.End(), [ nodeId ]( const auto iterator ) {
			return nodeId == iterator.Value()->m_nodeId;
		} );

		if( it != m_parts.End() )
		{
			return it.Value().Get();
		}
		else
		{
			return nullptr;
		}
	}

	void ChoiceHubSystem::OnNotificationsPublished( const SceneNotificationQueue& publishedNotifications )
	{
		for( const SceneNotification& notification : publishedNotifications )
		{
			SceneImportantNotification::Type notificationType = SceneImportantNotification::Type::None;

			switch( notification.m_notificationType )
			{

			case SceneNotificationType::instanceFinished:
			case SceneNotificationType::instanceDisposed:
				notificationType = SceneImportantNotification::Type::Finish;
				break;

			case SceneNotificationType::instanceInterrupted:
				notificationType = SceneImportantNotification::Type::Interrupt;
				break;

			case SceneNotificationType::instanceReturned:
				notificationType = SceneImportantNotification::Type::Return;
				break;

			}

			if( notificationType != SceneImportantNotification::Type::None )
			{
				m_importantNotifications.EmplaceBack( notification.m_sceneInstanceId, notificationType );
			}
		}	
	}

	void ChoiceHubSystem::OnEvent( Int32 eventIndex )
	{
		m_choiceConditions->OnQuestEvent( eventIndex );
	}

	void ChoiceHubSystem::ProcessLoader()
	{
		if( m_loader )
		{
			m_loader->Tick();
			if( !m_loader->HasMoreToLoad() )
			{
				m_loader.Reset();
			}
		}
	}

	void ChoiceHubSystem::ProcessBlockHubRequests()
	{
		auto accessToken = m_blockHubRequests.AcquireAccess();
		for( const BlockHubRequest& request : accessToken.GetContents() )
		{
			if( ChoiceHubData* hub = m_hubs.FindPtr( request.m_hubId ) )
			{
				AddHubBlocker( request.m_blockingSceneInstanceId, *hub );
				UpdateInteractionOptions( *hub );
			}
		}
		accessToken.GetContents().Clear();
	}

	void ChoiceHubSystem::ProcessImportantNotifications()
	{
		for( const SceneImportantNotification& importantNotification : m_importantNotifications )
		{
			const SceneInstanceId& sceneInstanceId = importantNotification.m_sceneInstanceId;
			switch( importantNotification.m_type )
			{

			case SceneImportantNotification::Type::Finish:
			{
				ClearHubBlocker( sceneInstanceId );
				RemoveExclusiveFocusClue( sceneInstanceId );
			}
			break;

			case SceneImportantNotification::Type::Interrupt:
			{
				if( IsExclusiveFocusClueActive( sceneInstanceId ) )
				{
					DeactivateExclusiveFocusClue();
				}
			}
			break;

			case SceneImportantNotification::Type::Return:
			{
				ActivateExclusiveFocusClue( sceneInstanceId );
			}
			break;

			default:
				RED_FATAL( "Unsupported type" );

			}


		}
		
		m_importantNotifications.Clear();
	}

	void ChoiceHubSystem::ProcessFrozenScenes()
	{
		ExclusiveFocusClueData activeFocusClueData = GetActiveExclusiveFocusClueData();
		if( activeFocusClueData.m_sceneInstanceId )
		{
			if( m_sceneSystem.MeetsFreezeCriteria( activeFocusClueData.m_sceneInstanceId ) )
			{
				DeactivateExclusiveFocusClue();
			}
		}
	}

	void ChoiceHubSystem::ProcessRetrofixer( Float deltaTime )
	{
		m_retrofixer->Tick( deltaTime );
	}

	void ChoiceHubSystem::ProcessFailsafe( Float deltaTime )
	{
		if( !m_failsafeController->IsActive() )
		{
			return;
		}

		m_failsafeController->Tick( deltaTime );

		ProcessChoiceHubFailsafes();
		ProcessChoiceHubPartFailsafes();
	}

	void ChoiceHubSystem::ProcessStatusChanges()
	{
		ChoiceHubMappingUpdates updates;
		m_mappingManager->Process( updates );

		const ChoiceHubInteractionStatus abortStatus{ true };
		for( const ChoiceHubPartId& cancelledPart : updates.m_cancelledParts )
		{
			ChoiceHubPart& hubPart = GetChoiceHubPart( cancelledPart );
			SendStatusUpdate( abortStatus, hubPart );
		}

		for( const ChoiceHubId& hubId : updates.m_modifiedHubs )
		{
			UpdateChoiceHub( hubId );
		}

		for( const ChoiceHubPartId& partId : updates.m_modifiedParts )
		{
			UpdateChoiceHubPart( partId );
		}

		for( const ChoiceHubPartId& removedPart : updates.m_removedParts )
		{
			RemoveChoiceHubPart( removedPart );
		}

		for( const ChoiceHubId& hubId : updates.m_deletedHubs )
		{
			RemoveChoiceHub( hubId );
		}
	}

	void ChoiceHubSystem::ProcessFocusClueActivity()
	{
		if( auto scanningController = m_scanningController.ToHandle() )
		{
			if( IsExclusiveFocusClueActive() )
			{
				// check if an exclusive focus clue entity was modified externally
				const ent::EntityID exclusiveFocusClueEntity = scanningController->GetExclusiveFocusClueEntity();
				if( GetActiveExclusiveFocusClueEntity() != exclusiveFocusClueEntity )
				{
					RED_ASSERT( !exclusiveFocusClueEntity.IsDefined(), "Only resetting exclusive focus clue entity from scanning controller is supported" );
					RemoveActiveExclusiveFocusClue();
				}
			}
		}
	}

	void ChoiceHubSystem::ProcessHintActivatedHubs()
	{
		auto token = m_hintAreaRequests.AcquireAccess();
		HintAreaRequests& requests = token.GetContents();
		for( const HintAreaRequest& request : requests )
		{
			if( ChoiceHubData* hubData = m_hubs.FindPtr( request.m_hubId ) )
			{
				enumUtils::SetFlag( hubData->m_hubFlags, ChoiceHubFlags::HintAreaActive, request.m_isAreaActivated );
				if( request.m_isAreaActivated )
				{
					UpdateInteractionOptions( *hubData );
				}
			}
		}
		requests.Clear();
	}

	void ChoiceHubSystem::InstallInteraction( ChoiceHubData& hubData )
	{
		const game::interactions::HotspotSource source{ hubData.m_owner };
		hubData.m_ticket = m_interactionManager.AddHotSpot( source, CreateInteractionSetupData( hubData ) );
		RED_ASSERT( hubData.m_ticket.IsValid(), "Couldn't add hot spot. Investigate." );

		ToggleInteractionLayers( hubData );

		hubData.m_listener = red::CreateSharedPtr< ChoiceInteractionListener >( *this, hubData.m_hubId );
		m_interactionManager.SetHotSpotInternalListener( hubData.m_ticket, hubData.m_listener );

		m_choiceHubFailsafes.m_refresh.Insert( hubData.m_hubId );
	}

	void ChoiceHubSystem::UninstallInteraction( ChoiceHubData& hubData )
	{
		if( !m_interactionManager.RemoveHotSpot( hubData.m_ticket ) )
		{
			RED_LOG_WARNING( "Can`t remove HotSpot." );
		}

		hubData.m_listener = nullptr;
		hubData.m_ticket.Reset();

		m_choiceHubFailsafes.m_refresh.Insert( hubData.m_hubId );
	}

	void ChoiceHubSystem::RefreshInteraction( ChoiceHubData& hubData )
	{
		m_interactionManager.ModifyHotSpot( hubData.m_ticket, CreateInteractionSetupData( hubData ) );
		ToggleInteractionLayers( hubData );

		m_choiceHubFailsafes.m_refresh.Insert( hubData.m_hubId );
	}

	void ChoiceHubSystem::UpdateInteractionOptions( ChoiceHubData& hubData )
	{
		struct OptionEntry
		{
			ChoiceHubOption* m_option = nullptr;
			Uint64 m_sceneIdHash = 0;
			NodeId::UnderlyingType m_nodeId = 0;
		};

		const InteractionDescriptorBuilder& descriptor = hubData.m_mergedInteractionDescriptor;
		if( !hubData.IsBlocked() )
		{
			if( !enumUtils::HasFlag( hubData.m_hubFlags, ChoiceHubFlags::HintAreaActive ) )
			{
				return;
			}

			const Bool isExclusiveHub = descriptor.IsExclusive();

			// collect all options
			red::StaticArray< OptionEntry, ChoiceNode::MaxOptions() > activationList;

			// helper for preparing mappin data
			ChoiceHubMappinBuilder mappinBuilder;

			ForEachHubPart( hubData.m_hubId, [ &activationList, &mappinBuilder, isExclusiveHub ]( ChoiceHubPart& hubPart ) {

				const InteractionDescriptorBuilder& descriptor = hubPart.m_descriptor;
				Bool addOptions = !isExclusiveHub || descriptor.IsExclusive();

				if( addOptions )
				{
					mappinBuilder.ProcessInteractionDescriptor( descriptor, hubPart.m_options );

					for( ChoiceHubOption& option : hubPart.m_options )
					{
						activationList.PushBack( { &option, hubPart.m_sceneInstanceId.GetHash(), hubPart.m_nodeId.m_id } );
					}
				}
			} );

			if( hubData.m_mappinLogic )
			{
				red::ArraySpan< const ChoiceHubMappinDescriptor > mappinDescriptors = mappinBuilder.GetDescriptors();
				hubData.m_mappinLogic->SetDescriptors( mappinDescriptors );

				if( mappinDescriptors.Size() > 0 )
				{
					// in case of merged choice hubs, when one choice section has a mappin enabled and another doesn't,
					// we need to ensure that all of them have mappinIdx set. Otherwise, it would be inconsistent with the mappin logic.
					for( OptionEntry& optionEntry : activationList )
					{
						game::interactions::MappinIdx& mappinIdx = optionEntry.m_option->m_hotSpotChoice.m_mappinIdx;
						if( mappinIdx == game::interactions::c_invalidMappinIdx )
						{
							mappinIdx = 0;
						}
					}
				}
			}

			// sort them by priority
			std::sort( activationList.Begin(), activationList.End(), []( OptionEntry a, OptionEntry b )
			{
				//if it's the same priority sort by sceneId hash
				if( a.m_option->m_priority == b.m_option->m_priority )
				{
					// if its the same scene sort by node id's
					if( a.m_sceneIdHash == b.m_sceneIdHash )
					{
						return a.m_nodeId < b.m_nodeId;
					}

					return a.m_sceneIdHash < b.m_sceneIdHash;
				}

				return a.m_option->m_priority < b.m_option->m_priority;
			} );

			// Install option list.
			{
				red::DynArray< game::interactions::Choice > hotSpotChoices{ PoolFrameSceneSystem() };
				hotSpotChoices.Reserve( activationList.Size() );

				for( OptionEntry optionEntry : activationList )
				{
					hotSpotChoices.PushBack( optionEntry.m_option->m_hotSpotChoice );
				}

				m_interactionManager.SetHotSpotChoices( hubData.m_ticket, hotSpotChoices, GetLinkedLayersName( descriptor ) );
			}
		}
		else
		{
			m_interactionManager.ResetHotSpotChoices( hubData.m_ticket, GetLinkedLayersName( descriptor ) );
		}
	}

	void ChoiceHubSystem::ToggleInteractionLayers( const ChoiceHubData& hubData )
	{
		const Bool isScreen = hubData.m_mergedInteractionDescriptor.IsScreen();
		const Bool showOptions = enumUtils::HasFlag( hubData.m_combinedFlags, ChoiceHubFlags::ShowOptions );
		if( isScreen )
		{
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, showOptions, ChoiceConstants::c_screenLinkedLayersName );
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, false, ChoiceConstants::c_entityLinkedLayersName );
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, false, ChoiceConstants::c_indicationAreaLinkedLayersName );
		}
		else
		{
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, true, ChoiceConstants::c_indicationAreaLinkedLayersName );
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, false, ChoiceConstants::c_screenLinkedLayersName );

			if( showOptions )
			{
				const Bool isAdaptive = hubData.m_mergedInteractionDescriptor.IsAdaptive();
				const Bool usesScreenSpaceTest = hubData.m_mergedInteractionDescriptor.UsesScreenSpaceTest();

				const Bool enableScreenSpaceLookAtLayer = usesScreenSpaceTest;
				const Bool enableBasicLookAtLayer = !usesScreenSpaceTest;

				m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, enableScreenSpaceLookAtLayer, ChoiceConstants::c_entityLinkedLayersName, game::interactions::vis::c_entityScreenSpaceLookAtLayerTag );
				m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, enableBasicLookAtLayer, ChoiceConstants::c_entityLinkedLayersName, game::interactions::vis::c_entityBasicLookAtLayerTag );
				//m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, enableAdaptiveLookAtLayer, ChoiceConstants::c_entityLinkedLayersName, game::interactions::vis::c_entityAdaptiveLookAtLayerTag );
			}
			else
			{
				m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, false, ChoiceConstants::c_entityLinkedLayersName );
			}
		}

		const ent::EntityID exclusiveFocusClueEntityId = GetActiveExclusiveFocusClueEntity();
		if( exclusiveFocusClueEntityId.IsDefined() && enumUtils::HasFlag( hubData.m_combinedFlags, ChoiceHubFlags::FocusClue ) )
		{
			const ent::EntityID hubEntityId = m_mappingManager->GetEntityID( hubData.m_hubId );
			m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, hubEntityId == exclusiveFocusClueEntityId );
		}
	}

	void ChoiceHubSystem::InstallOrb( const game::interactions::HotSpotTicket& ticket, const ChoiceHubPart& part )
	{
		RED_ASSERT( ticket.IsValid(), "Interaction is not created" );
		RED_ASSERT( part.m_descriptor.GetOrbDefinition(), "Orb definition undefined" );
		RED_ASSERT( !m_orbMappings.KeyExist( part.m_partId ), "Part already has an orb registered" );
		m_orbMappings.Insert( part.m_partId, m_interactionManager.RegisterOrb( ticket, *part.m_descriptor.GetOrbDefinition() ) );
	}

	void ChoiceHubSystem::UninstallOrb( const ChoiceHubPartId& partId )
	{
		auto it = m_orbMappings.Find( partId );
		RED_ASSERT( it != m_orbMappings.End(), "No orb has been created for the given part" );
		m_interactionManager.UnregisterOrb( it.Value() );
		m_orbMappings.Remove( it );
	}

	void ChoiceHubSystem::ToggleChoiceHubInteraction( ChoiceHubData& hubData, Bool isEnabled )
	{
		m_interactionManager.SetHotSpotEnabled( hubData.m_ticket, isEnabled );
	}

	void ChoiceHubSystem::AddHubBlocker( const SceneInstanceId& sceneInstanceId, ChoiceHubData& hubData )
	{
		RED_ASSERT( !hubData.m_blockingSceneInstanceId, "Choice hub is already blocked" );
		RED_ASSERT( !FindBlockingSceneForHub( hubData.m_hubId ), "Choice hub is already registered for blocked scenes" );

		hubData.m_blockingSceneInstanceId = sceneInstanceId;

		BlockedHubEntry& newEntry = m_blockedScenes.EmplaceBack();
		newEntry.m_blockingSceneInstanceId = sceneInstanceId;
		newEntry.m_blockedHubId = hubData.m_hubId;

		RegisterSceneListener( sceneInstanceId );
	}

	void ChoiceHubSystem::RemoveHubBlocker( const SceneInstanceId& sceneInstanceId, ChoiceHubData& hubData )
	{
		RED_ASSERT( hubData.m_blockingSceneInstanceId, "Choice hub is not blocked by any scene" );
		RED_ASSERT( FindBlockingSceneForHub( hubData.m_hubId ), "Choice hub is not present in blocked scenes set" );

		for( Uint32 idx : m_blockedScenes.Indices() )
		{
			if( m_blockedScenes[ idx ].m_blockedHubId == hubData.m_hubId )
			{
				UnregisterSceneListener( sceneInstanceId );
				m_blockedScenes.RemoveAtReorder( idx );
				break;
			}
		}

		hubData.m_blockingSceneInstanceId = SceneInstanceId::invalid;
	}

	void ChoiceHubSystem::ClearHubBlocker( const SceneInstanceId& sceneInstanceId )
	{
		for( Uint32 idx : m_blockedScenes.ReverseIndices() )
		{
			const BlockedHubEntry& blockedScene = m_blockedScenes[ idx ];
			if( blockedScene.m_blockingSceneInstanceId == sceneInstanceId )
			{
				ChoiceHubData& hubData = GetChoiceHubData( blockedScene.m_blockedHubId );

				UnregisterSceneListener( sceneInstanceId );

				hubData.m_blockingSceneInstanceId = SceneInstanceId::invalid;
				m_blockedScenes.RemoveAtReorder( idx );
			}
		}
	}

	Bool ChoiceHubSystem::FindBlockingSceneForHub( const ChoiceHubId& hubId ) const
	{
		auto it = std::find_if( m_blockedScenes.Begin(), m_blockedScenes.End(), [ hubId ]( const BlockedHubEntry& blockedScene )
		{
			return hubId == blockedScene.m_blockedHubId;
		} );

		return it != m_blockedScenes.End();
	}

	SceneInstanceId ChoiceHubSystem::RetrieveLoadedHubBlocker( const ChoiceHubNodeRawID& extendedNodeId )
	{
		if( m_loader )
		{
			return m_loader->RetrieveHubBlocker( extendedNodeId );
		}

		return SceneInstanceId::invalid;
	}

	void ChoiceHubSystem::RegisterSceneListener( const SceneInstanceId& sceneInstanceId )
	{
		auto entryIt = std::find_if( m_registeredSceneListeners.Begin(), m_registeredSceneListeners.End(),
									 [ sceneInstanceId ]( const SceneListenerEntry& entry )
		{
			return entry.m_sceneInstanceId == sceneInstanceId;
		} );

		SceneListenerEntry* entry = nullptr;
		if( entryIt == m_registeredSceneListeners.End() )
		{
			entry = &m_registeredSceneListeners.EmplaceBack( sceneInstanceId );
			m_sceneSystem.RegisterListener( sceneInstanceId, *this );
		}
		else
		{
			entry = &*entryIt;
		}

		++entry->m_count;
	}

	void ChoiceHubSystem::UnregisterSceneListener( const SceneInstanceId& sceneInstanceId )
	{
		auto entryIt = std::find_if( m_registeredSceneListeners.Begin(), m_registeredSceneListeners.End(),
									 [ sceneInstanceId ]( const SceneListenerEntry& entry )
		{
			return entry.m_sceneInstanceId == sceneInstanceId;
		} );

		RED_ASSERT( entryIt != m_registeredSceneListeners.End() );

		SceneListenerEntry* entry = &*entryIt;
		if( --entry->m_count == 0 )
		{
			m_sceneSystem.DeregisterListener( sceneInstanceId, *this );
			m_registeredSceneListeners.RemoveReorder( entryIt );
		}
	}

	game::interactions::HotSpotSetupData ChoiceHubSystem::CreateInteractionSetupData( const ChoiceHubData& hubData )
	{
		const InteractionDescriptorBuilder& descr = hubData.m_mergedInteractionDescriptor;
		red::UniquePtr< game::interactions::OverrideParams > overrideParams = descr.CreateOverrideParams();
		overrideParams->m_mappinLogic = hubData.m_mappinLogic;
		return game::interactions::HotSpotSetupData{ descr.GetDefinitionSet(), std::move( overrideParams ), descr.GetTransformUpdater() };
	}

	void ChoiceHubSystem::UpdateInteractionResource( ChoiceHubData& hubData )
	{
		ChoiceHubPartIdSpan partIds = m_mappingManager->GetParts( hubData.m_hubId );

		red::DynArray< ChoiceHubPart* > hubParts{ PoolFrameSceneSystem{} };
		hubParts.Reserve( partIds.Size() );
		for( const ChoiceHubPartId& partId : partIds )
		{
			hubParts.PushBack( m_parts[ partId ].Get() );
		}

		const ChoiceHubPart& initialPart = *hubParts[ 0 ];
		Uint32 baseDescriptorIdx = 0;
		Uint32 shapeDescriptorIdx = 0;
		Uint8 hubPriority = initialPart.m_descriptor.GetHubPriority();
		Bool hasMappin = initialPart.m_descriptor.GetMappinParams().IsValid();
		ChoiceHubFlags combinedFlags = initialPart.m_flags;

		for( Uint32 i = 1; i < hubParts.Size(); ++i )
		{
			const InteractionDescriptorBuilder& baseDescriptor = hubParts[ baseDescriptorIdx ]->m_descriptor;
			const InteractionDescriptorBuilder& shapeDescriptor = hubParts[ shapeDescriptorIdx ]->m_descriptor;
			const InteractionDescriptorBuilder& candidateDescriptor = hubParts[ i ]->m_descriptor;

			hubPriority = Max( hubPriority, candidateDescriptor.GetHubPriority() );
			combinedFlags |= hubParts[ i ]->m_flags;

			hasMappin |= candidateDescriptor.GetMappinParams().IsValid();

			// first criteria - exclusive interactions
			if( candidateDescriptor.IsExclusive() )
			{
				GAME_DATA_ERROR( !baseDescriptor.IsExclusive(), Scenes, "More than one exclusive interaction descriptor is active on the same choice hub owner" );
				baseDescriptorIdx = i;
				shapeDescriptorIdx = i;
				continue;
			}
			else if( baseDescriptor.IsExclusive() )
			{
				continue;
			}

			// screen choice takes the priority over other ones
			if( candidateDescriptor.IsScreen() )
			{
				baseDescriptorIdx = i;
				shapeDescriptorIdx = i;
				continue;
			}
			else if( baseDescriptor.IsScreen() )
			{
				continue;
			}

			// adaptive descriptors are more important than others
			if( candidateDescriptor.IsAdaptive() && !baseDescriptor.IsAdaptive() )
			{
				baseDescriptorIdx = i;
			}

			const Bool selectedHasActivationYawLimit = shapeDescriptor.GetActivationYawLimit() > 0.0f;
			const Float selectedRange = shapeDescriptor.GetActivationRange();
			const Bool otherHasActivationYawLimit = candidateDescriptor.GetActivationYawLimit() > 0.0f;
			const Float otherRange = candidateDescriptor.GetActivationRange();

			// second criteria - activation yaw limit
			if( otherHasActivationYawLimit )
			{
				GAME_DATA_ERROR( !selectedHasActivationYawLimit, Scenes, "More than one interaction descriptor with activation yaw limit is active on the same choice hub owner" );
				shapeDescriptorIdx = i;
				continue;
			}
			else if( selectedHasActivationYawLimit )
			{
				continue;
			}

			// third criteria - activation range (selecting the smallest one)
			if( otherRange < selectedRange )
			{
				shapeDescriptorIdx = i;
			}
		}

		hubData.m_combinedFlags = combinedFlags;
		hubData.m_baseDescriptorIdx = baseDescriptorIdx;
		hubData.m_mergedInteractionDescriptor = hubParts[ baseDescriptorIdx ]->m_descriptor;
		if( baseDescriptorIdx != shapeDescriptorIdx )
		{
			const InteractionDescriptorBuilder& shapeDescriptor = hubParts[ shapeDescriptorIdx ]->m_descriptor;
			hubData.m_mergedInteractionDescriptor.SetShape( shapeDescriptor.GetShapeParams(), shapeDescriptor.GetDefaultRange(), shapeDescriptor.GetDefaultYawLimit() );
		}

		hubData.m_mergedInteractionDescriptor.SetHubPriority( hubPriority );
		hubData.m_mergedInteractionDescriptor.SetQuestImportant( enumUtils::HasFlag( combinedFlags, ChoiceHubFlags::QuestImportant ) );

		if( m_choiceHubFailsafes.m_active.Exist( hubData.m_hubId ) )
		{
			hubData.m_mergedInteractionDescriptor.SetupFailsafeInteraction();
		}

		if( hasMappin )
		{
			if( !hubData.m_mappinLogic )
			{
				hubData.m_mappinLogic = red::CreateSharedPtr< ChoiceHubMappinLogic >();
			}
		}
		else
		{
			hubData.m_mappinLogic = nullptr;
		}
	}

	void ChoiceHubSystem::ReinitializeChoiceHub( ChoiceHubData& hubData )
	{
		const ChoiceHubMapping* mapping = m_mappingManager->FindMapping( hubData.m_hubId );
		THandle< game::Object > interactionOwner = hubData.m_owner.ToHandle();
		THandle< game::Object > mappingOwner = mapping->m_owner.ToHandle();
		if( interactionOwner != mappingOwner )
		{
			ExpireChoiceHub( hubData );
			hubData.m_owner = mappingOwner;
			InstallInteraction( hubData );
		}
		else
		{
			RefreshInteraction( hubData );
			UpdateInteractionOptions( hubData );
		}
	}

	void ChoiceHubSystem::ExpireChoiceHub( ChoiceHubData& hubData )
	{
		if( hubData.IsInstalled() )
		{
			UninstallInteraction( hubData );
		}

		hubData.m_owner = nullptr;
	}

	void ChoiceHubSystem::UpdateChoiceHub( const ChoiceHubId& hubId )
	{
		ChoiceHubData& hubData = GetOrCreateChoiceHub( hubId );
		UpdateInteractionResource( hubData );
		ReinitializeChoiceHub( hubData );
	}

	void ChoiceHubSystem::RemoveChoiceHub( const ChoiceHubId& hubId )
	{
		auto it = m_hubs.Find( hubId );
		RED_ASSERT( it != m_hubs.End(), "Hub %d doesn't exist", hubId.GetId() );

		ChoiceHubData& hubData = it.Value();

		m_choiceHubFailsafes.Remove( hubData.m_hubId );
		m_failsafeController->ClearFailsafe_ValidInteractionInTier( hubData.m_owner );

		ExpireChoiceHub( hubData );

		if( hubData.m_blockingSceneInstanceId )
		{
			RemoveHubBlocker( hubData.m_blockingSceneInstanceId, hubData );
		}

		m_hubs.Remove( it );
	}

	void ChoiceHubSystem::UpdateChoiceHubPart( const ChoiceHubPartId& hubPartId )
	{
		if( m_orbMappings.KeyExist( hubPartId ) )
		{
			UninstallOrb( hubPartId );
		}

		const ChoiceHubPart& hubPart = GetChoiceHubPart( hubPartId );
		if( ChoiceHubData* hubData = FindChoiceHub( hubPartId ) )
		{
			if( hubPart.m_descriptor.GetOrbDefinition() )
			{
				InstallOrb( hubData->m_ticket, hubPart );
			}

			if( const SceneInstanceId loadedBlocker = RetrieveLoadedHubBlocker( ChoiceHubNodeRawID{ hubPart.m_sceneInstanceId, hubPart.m_nodeId } ) )
			{
				if( hubData->m_blockingSceneInstanceId != loadedBlocker )
				{
					AddHubBlocker( loadedBlocker, *hubData );
				}
			}
			else if( hubData->m_blockingSceneInstanceId == hubPart.m_sceneInstanceId )
			{
				RemoveHubBlocker( hubData->m_blockingSceneInstanceId, *hubData );
			}

			SendStatusUpdate( ChoiceHubInteractionStatus{ hubData->m_ticket, hubData->m_owner.ToHandle() }, hubPart );
			if( enumUtils::HasFlag( hubData->m_hubFlags, ChoiceHubFlags::HintAreaActive ) )
			{
				hubPart.m_onInteractionCallback( ChoiceHubInteractionLayer::Hint, true );
			}
		}
		else
		{
			SendStatusUpdate( ChoiceHubInteractionStatus{}, hubPart );
		}

		m_choiceHubPartFailsafes.Refresh( hubPartId );
	}

	void ChoiceHubSystem::RemoveChoiceHubPart( const ChoiceHubPartId& hubPartId )
	{
		auto it = m_parts.Find( hubPartId );
		RED_ASSERT( it != m_parts.End(), "Part %d doesn't exist", hubPartId.GetId() );

		if( m_orbMappings.KeyExist( hubPartId ) )
		{
			UninstallOrb( hubPartId );
		}

		if( enumUtils::HasFlag( it.Value()->m_flags, ChoiceHubFlags::FocusClue ) )
		{
			m_focusCluesParts.Remove( hubPartId );
		}

		m_choiceHubPartFailsafes.Remove( hubPartId );
		m_failsafeController->ClearFailsafe_NoEntity( hubPartId );

		m_parts.Remove( it );
	}

	void ChoiceHubSystem::ProcessChoiceHubFailsafes()
	{
		for( const ChoiceHubId& hubId : m_choiceHubFailsafes.m_refresh )
		{
			if( ChoiceHubData* hubData = m_hubs.FindPtr( hubId ) )
			{
				RefreshChoiceHubFailsafe( *hubData );
			}
		}
		m_choiceHubFailsafes.m_refresh.Clear();

		for( const ChoiceHubId& hubId : m_choiceHubFailsafes.m_valid )
		{
			if( m_choiceHubFailsafes.m_active.Exist( hubId ) )
			{
				continue;
			}

			if( ChoiceHubData* hubData = m_hubs.FindPtr( hubId ) )
			{
				if( m_failsafeController->IsFailsafeTriggered_ValidInteractionInTier( hubData->m_owner ) )
				{
					m_choiceHubFailsafes.m_active.Insert( hubData->m_hubId );

					UpdateInteractionResource( *hubData );
					RefreshInteraction( *hubData );
				}
			}
		}

	}

	void ChoiceHubSystem::RefreshChoiceHubFailsafe( ChoiceHubData& hubData )
	{
		ChoiceHubPartIdSpan partIds = m_mappingManager->GetParts( hubData.m_hubId );
		m_tmpSceneInstances.Reserve( partIds.Size() );
		for( const ChoiceHubPartId& partId : partIds )
		{
			m_tmpSceneInstances.PushBack( m_parts[ partId ]->m_sceneInstanceId );
		}

		Bool installFailsafe = false;
		if( enumUtils::HasFlag( hubData.m_combinedFlags, ChoiceHubFlags::EnableValidInteractionFailsafe ) )
		{
			installFailsafe = !hubData.m_owner.Expired() && !hubData.m_mergedInteractionDescriptor.IsFailsafe();
		}

		if( installFailsafe )
		{
			m_failsafeController->SetFailsafe_ValidInteractionInTier( hubData.m_owner, m_tmpSceneInstances );
			m_choiceHubFailsafes.m_valid.Insert( hubData.m_hubId );
		}
		else
		{
			m_failsafeController->ClearFailsafe_ValidInteractionInTier( hubData.m_owner );
			m_choiceHubFailsafes.m_valid.Remove( hubData.m_hubId );
		}

		m_tmpSceneInstances.Clear();
	}

	void ChoiceHubSystem::ProcessChoiceHubPartFailsafes()
	{
		if ( NpForcedChoiceFailsafe() ) // [nomadplay]
		{
			for ( auto kv : m_parts )
			{
				ChoiceHubPart* hubPart = kv.Value().Get();
				if ( !hubPart || m_choiceHubPartFailsafes.m_active.Exist( hubPart->m_partId ) || m_mappingManager->FindHub( hubPart->m_partId ) )
				{
					continue;
				}
				if ( NpForcedChoiceFailsafe() < 2 && !enumUtils::HasFlag( hubPart->m_flags, ChoiceHubFlags::EnableNoEntityFailsafe ) )
				{
					continue;
				}
				if ( NpChoicePartNoHubSeconds( hubPart ) > 6.0 )
				{
					m_choiceHubPartFailsafes.m_active.Insert( hubPart->m_partId );
					hubPart->m_descriptor.SetupFailsafeInteraction();
					m_mappingManager->SetFailsafeMode( hubPart->m_partId );
					std::fprintf( stderr, "[nomadplay] choice part %p: no hub for >6 s -> forced failsafe (plain HUD choice)\n", ( void* )hubPart );
				}
			}
		}
		for( const ChoiceHubPartId& partId : m_choiceHubPartFailsafes.m_refresh )
		{
			if( ChoiceHubPart* hubPart = FindChoiceHubPart( partId ) )
			{
				RefreshChoiceHubPartFailsafe( *hubPart );
			}
		}
		m_choiceHubPartFailsafes.m_refresh.Clear();

		for( const ChoiceHubPartId& partId : m_choiceHubPartFailsafes.m_valid )
		{
			if( m_choiceHubPartFailsafes.m_active.Exist( partId ) )
			{
				continue;
			}

			if( ChoiceHubPart* hubPart = FindChoiceHubPart( partId ) )
			{
				if( m_failsafeController->IsFailsafeTriggered_NoEntity( partId ) )
				{
					m_choiceHubPartFailsafes.m_active.Insert( partId );

					hubPart->m_descriptor.SetupFailsafeInteraction();
					m_mappingManager->SetFailsafeMode( partId );
				}
			}
		}
	}


	void ChoiceHubSystem::RefreshChoiceHubPartFailsafe( ChoiceHubPart& part )
	{
		NpChoicePartState( &part, m_mappingManager->FindHub( part.m_partId ) ? true : false, m_mappingManager->TryGetEntityID( part.m_partId ).IsDefined(), Uint32( part.m_flags ) ); // [nomadplay]
		if( m_mappingManager->FindHub( part.m_partId ) )
		{
			m_failsafeController->ClearFailsafe_NoEntity( part.m_partId );
			m_choiceHubPartFailsafes.m_valid.Remove( part.m_partId );
		}
		else
		{
			if( enumUtils::HasFlag( part.m_flags, ChoiceHubFlags::EnableNoEntityFailsafe ) )
			{
				m_failsafeController->SetFailsafe_NoEntity( part.m_partId, part.m_sceneInstanceId, m_mappingManager->TryGetEntityID( part.m_partId ) );
				m_choiceHubPartFailsafes.m_valid.Insert( part.m_partId );
			}
		}
	}

	void ChoiceHubSystem::RemoveActiveExclusiveFocusClue()
	{
		if( m_hasActiveExclusiveFocusClue )
		{
			RemoveExclusiveFocusClue( constants::c_activeExclusiveFocusClueIndex );
		}
	}

	void ChoiceHubSystem::RemoveExclusiveFocusClue( Uint32 exclusiveFocusClueIndex )
	{
		if( m_hasActiveExclusiveFocusClue )
		{
			if( exclusiveFocusClueIndex == constants::c_activeExclusiveFocusClueIndex )
			{
				DeactivateExclusiveFocusClue();
			}
		}

		const ExclusiveFocusClueData& focusClueData = m_storedExclusiveFocusClues[ exclusiveFocusClueIndex ];
		if( focusClueData.m_sceneInstanceId )
		{
			UnregisterSceneListener( focusClueData.m_sceneInstanceId );
		}

		m_storedExclusiveFocusClues.RemoveAtReorder( exclusiveFocusClueIndex );
	}

	void ChoiceHubSystem::ActivateExclusiveFocusClue( const SceneInstanceId& sceneInstanceId )
	{
		const Uint32 currentIndex = FindExclusiveFocusClueDataIndex( sceneInstanceId );
		if( currentIndex != constants::c_invalidExclusiveFocusClueIndex )
		{
			RED_ASSERT( !IsExclusiveFocusClueActive(), "Double exclusive focus clue activation" );

			if( IsExclusiveFocusClueActive() )
			{
				DeactivateExclusiveFocusClue();
			}

			if( currentIndex != constants::c_activeExclusiveFocusClueIndex )
			{
				std::swap( m_storedExclusiveFocusClues[ constants::c_activeExclusiveFocusClueIndex ], m_storedExclusiveFocusClues[ currentIndex ] );
			}
			
			const ExclusiveFocusClueData& activeFocusClueData = m_storedExclusiveFocusClues[ constants::c_activeExclusiveFocusClueIndex ];
			if( auto scanningController = m_scanningController.ToHandle() )
			{
				red::String debugString{ String::Printf( "Scene: %hs ==> NodeId: %d", sceneInstanceId.ToDebugString().AsChar(), activeFocusClueData.m_nodeId.m_id ) };
				scanningController->SetExclusiveFocusClueEntity( activeFocusClueData.m_entityId, std::move( debugString ) );
			}

			m_hasActiveExclusiveFocusClue = true;

			UpdateFocusClueChoiceHubs();
		}
	}

	void ChoiceHubSystem::DeactivateExclusiveFocusClue()
	{
		RED_ASSERT( m_hasActiveExclusiveFocusClue );

		if( auto scanningController = m_scanningController.ToHandle() )
		{
			scanningController->ClearExclusiveFocusClueEntity();
		}

		m_hasActiveExclusiveFocusClue = false;

		UpdateFocusClueChoiceHubs();
	}

	Bool ChoiceHubSystem::IsExclusiveFocusClueActive() const
	{
		return m_hasActiveExclusiveFocusClue;
	}

	Bool ChoiceHubSystem::IsExclusiveFocusClueActive( const SceneInstanceId& sceneInstanceId ) const
	{
		if( IsExclusiveFocusClueActive() )
		{
			return m_storedExclusiveFocusClues[ constants::c_activeExclusiveFocusClueIndex ].m_sceneInstanceId == sceneInstanceId;
		}

		return false;
	}

	Bool ChoiceHubSystem::HasExclusiveFocusClueData( const SceneInstanceId& sceneInstanceId ) const
	{
		return FindExclusiveFocusClueDataIndex( sceneInstanceId ) != constants::c_invalidExclusiveFocusClueIndex;
	}

	Uint32 ChoiceHubSystem::FindExclusiveFocusClueDataIndex( const SceneInstanceId& sceneInstanceId ) const
	{
		for( Uint32 i : m_storedExclusiveFocusClues.Indices() )
		{
			if( m_storedExclusiveFocusClues[ i ].m_sceneInstanceId == sceneInstanceId )
			{
				return i;
			}
		}

		return constants::c_invalidExclusiveFocusClueIndex;
	}

	ChoiceHubSystem::ExclusiveFocusClueData ChoiceHubSystem::GetActiveExclusiveFocusClueData() const
	{
		if( IsExclusiveFocusClueActive() )
		{
			return m_storedExclusiveFocusClues[ constants::c_activeExclusiveFocusClueIndex ];
		}

		return ExclusiveFocusClueData{};
	}

	ent::EntityID ChoiceHubSystem::GetActiveExclusiveFocusClueEntity() const
	{
		if( IsExclusiveFocusClueActive() )
		{
			return m_storedExclusiveFocusClues[ constants::c_activeExclusiveFocusClueIndex ].m_entityId;
		}

		return ent::EntityID{};
	}

	void ChoiceHubSystem::UpdateFocusClueChoiceHubs()
	{
		if( IsExclusiveFocusClueActive() )
		{
			const ent::EntityID exclusiveFocusClueEntityId = GetActiveExclusiveFocusClueEntity();
			for( const ChoiceHubPartId& partId : m_focusCluesParts )
			{
				if( const ChoiceHubMapping* mapping = m_mappingManager->FindMapping( partId ) )
				{
					if( mapping->m_entityId.IsDefined() )
					{
						ChoiceHubData* hubData = m_hubs.FindPtr( mapping->m_hubId );
						RED_ASSERT( hubData, "No hub has been created for existing entity" );
						ToggleChoiceHubInteraction( *hubData, mapping->m_entityId == exclusiveFocusClueEntityId );
					}
				}
			}
		}
		else
		{
			for( const ChoiceHubPartId& partId : m_focusCluesParts )
			{
				if( ChoiceHubData* hubData = FindChoiceHub( partId ) )
				{
					ToggleChoiceHubInteraction( *hubData, true );
				}
			}
		}
	}

	void ChoiceHubSystem::SendStatusUpdate( const ChoiceHubInteractionStatus& status, const ChoiceHubPart& hubPart )
	{
		if( hubPart.m_onStatusCallback )
		{
			hubPart.m_onStatusCallback( status );
		}
	}
}