// Copyright (c) 2016-2020 CD Projekt Red. All Rights Reserved.

#include "build.h"
#include "interactionActivator.h"

#include "interactionActivationContext.h"
#include "interactionActivationVisitor.h"
#include "interactionActivatorMappinController.h"
#include "interactionActivatorMappinHandle.h"
#include "interactionDebugStructs.h"
#include "interactionGroupProvider.h"
#include "interactionGroupRegistry.h"
#include "interactionGuide.h"
#include "interactionHotSpot.h"
#include "interactionHotSpotDefinitionSet.h"
#include "interactionLookAtProvider.h"
#include "interactionOrbSystem.h"
#include "interactionPublisher.h"
#include "interactionTargetFilter.h"
#include "interactionVisualizerController.h"

#include "../../game/include/targetingSystemTarget.h"
#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/gameObject.h"
#include "../../gameCore/include/targetingSystemInterface.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../redJobs2/include/jobDecl.h"
#include "../../redJobs2/include/jobSystem.h"
#include "../../world/include/cameraSystemInterface.h"
#include "../../gameCore/include/targetingSearchFilter.h"


#ifdef DEBUG_GAMEPLAY
#pragma optimize("",off)
#endif

#include <cstdio>
#include <cmath>
extern volatile int g_coreplayActivatorProbe; // [coreplay] defined in game/src/scriptableSystemsContainer.cpp overlay
namespace game { namespace interactions { class CActivator; } }
static WeakHandle< game::Object > s_cpRetryOwner; // [coreplay]
static const game::interactions::CActivator* s_cpRetryActivator = nullptr; // [coreplay]

namespace game
{
	namespace interactions
	{
		namespace
		{
			static const Uint32 c_hotSpotsPerFrame = 200;

			Bool TestEntity( ent::EntityID entityID, const ent::Entity* obj )
			{
				if( obj && entityID.IsDefined() )
				{
					return obj->GetEntityID() == entityID;
				}
				return false;
			}

			Bool TestComponent( WeakHandle<ent::IComponent> cmp1, const ent::IComponent* cmp2 )
			{
				auto strongCmp1 = cmp1.ToHandle();
				if( strongCmp1 && cmp2 )
				{
					return strongCmp1 == cmp2;
				}
				return false;
			}
		}


		/////////////////////////////////////////////////////////////////////////////////
		//
		ActivatorTicket::ActivatorTicket() = default;
		ActivatorTicket::~ActivatorTicket() = default;


		/////////////////////////////////////////////////////////////////////////////////
		//
		ActivatorLookAtResult::ActivatorLookAtResult() = default;
		ActivatorLookAtResult::~ActivatorLookAtResult() = default;


		/////////////////////////////////////////////////////////////////////////////////
		//
		ActivatorInitContext::ActivatorInitContext( 
				const WeakHandle< game::Object >& owner,
				const game::IGameInstance* gameInstance,
				OrbSystem* orbSystem,
				const HotSpotGroupDefinitionProvider& groupDefinitionProvider,
				vis::Cache* visCache )
			: m_owner{ owner }
			, m_gameInstance{ gameInstance }
			, m_orbSystem{ orbSystem }
			, m_groupDefinitionProvider{ groupDefinitionProvider }
			, m_visualizerCache{ visCache }
		{}

		ActivatorInitContext::~ActivatorInitContext() = default;


		/////////////////////////////////////////////////////////////////////////////////
		//
		CActivator::CActivator()
			: m_internalListener{ nullptr }
			, m_orbSystem{ nullptr }
		{
			m_updateTemps.frameUpdateHotSpots.Reserve( c_hotSpotsPerFrame );
		}

		CActivator::~CActivator() = default;

		void CActivator::Init( const ActivatorInitContext& initCtx )
		{
			if( initCtx.m_gameInstance )
			{
				RED_FATAL_ASSERT( initCtx.m_orbSystem );
				RED_FATAL_ASSERT( initCtx.m_visualizerCache );

				m_targetingSystem = initCtx.m_gameInstance->GetSystemPtr< ITargetingSystem >();
				m_cameraSystem = initCtx.m_gameInstance->GetSystemPtr< ICameraSystem >();

				m_visualizerController = red::CreateUniquePtr< vis::Controller >( *initCtx.m_gameInstance, *this, *initCtx.m_visualizerCache, *initCtx.m_orbSystem );
				m_mappinController = red::CreateUniquePtr< ActivatorMappinController >( *initCtx.m_gameInstance, *this );
			}

			m_orbSystem = initCtx.m_orbSystem;

			m_hotSpotGroupRegistry = initCtx.m_groupDefinitionProvider.CreateRegistry( *this );
			m_hotSpotGroupCollector = initCtx.m_groupDefinitionProvider.CreateCollector( job::GetNumDispatcherThreads() + 1 );

			if( THandle<game::Object> ownerHandle = initCtx.m_owner.ToHandle() )
			{
				m_ownerPeerID = ownerHandle->GetControllingPeerID();
			}
			else
			{
				RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Interactions, "CActivator::Init: Couldn't resolve controlling PeerID" );
			}

			if( auto targetingSystem = m_targetingSystem.ToHandle() )
			{
				m_logicalFilter = CreateHandle<TargetFilter_Logical>();
				m_logicalFilterResult = SafeCast<TargetFilterResult_Logical>( m_logicalFilter->CreateResult() );
				m_logicalFilterTicket = targetingSystem->RegisterLookAtFilter( initCtx.m_owner, m_logicalFilter );
				// CP2077 web [coreplay]: RegisterLookAtFilter silently fails when the targeting system has no user for the owner
				// yet (init-order race); without the logical filter the activator never sees interactables (no prompts, F dead).
				// Remember the owner and retry from PrepareForActivationTests until it succeeds.
				std::fprintf( stderr, "[coreplay] activator init: logical lookAt filter registration %s\n", m_logicalFilterTicket.IsValid() ? "ok" : "FAILED (targeting user not ready) - will retry" );
				if( !m_logicalFilterTicket.IsValid() )
				{
					s_cpRetryOwner = initCtx.m_owner;
					s_cpRetryActivator = this;
				}
			}

			m_debugDataStorage = red::CreateUniquePtr< DebugDataStorage >();
		}

		Bool CActivator::TestVisibleTargets( const THandle< game::Object>& objectHandle ) const
		{
			Bool found = false;
			if( auto targetingHandle = m_targetingSystem.ToHandle() )
			{
				targetingHandle->ForEachTarget( GetObject(), game::TargetingSet::Visible,
					[ &objectHandle, &found ]( const game::targeting::Target& tgt, const game::targeting::TargetPlayerData& tgtPlayerData )
				{
					if( auto visibleObject = tgt.m_object.ToHandle() )
					{
						if( visibleObject == objectHandle )
						{
							found = true;
						}
					}
				} );
			}
			return found;
		}

		Bool CActivator::TestClearlyVisibleTargets( const THandle< game::Object>& objectHandle ) const
		{
			Bool found = false;
			if( auto targetingHandle = m_targetingSystem.ToHandle() )
			{
				targetingHandle->ForEachTarget( GetObject(), game::TargetingSet::ClearlyVisible,
					[ &objectHandle, &found ]( const game::targeting::Target& tgt, const game::targeting::TargetPlayerData& tgtPlayerData )
				{
					if( auto visibleObject = tgt.m_object.ToHandle() )
					{
						if( visibleObject == objectHandle )
						{
							found = true;
						}
					}
				} );
			}
			return found;
		}

		Bool CActivator::TestLookAt( const ent::Entity* obj ) const
		{
			if( auto targetingHandle = m_targetingSystem.ToHandle() )
			{
				ent::EntityID tracedObjID = targetingHandle->GetLookAtObject( GetObject() );
				return TestEntity( tracedObjID, obj );
			}
			return false;
		}


		Bool CActivator::TestLookAt( const ent::IComponent* cmp ) const
		{
			if( auto targetingHandle = m_targetingSystem.ToHandle() )
			{
				WeakHandle< ent::IComponent > targetCmp = targetingHandle->GetLookAtComponent( GetObject() );
				return TestComponent( targetCmp, cmp );
			}
			return false;
		}

		OrbActivationResult CActivator::TestOrb( const ent::Entity& obj, const CHotSpotLayerDefinition* layerDef, Bool extendedArea ) const
		{
			if( m_orbSystem )
			{
				const Transform& cameraTransform = m_inputData.GetCameraTransform();

				OrbActivationQuery query;
				query.m_activatorId = GetRef();
				query.m_testedId = obj.GetEntityID();
				query.m_layerDef = layerDef;
				query.m_queryPosition = cameraTransform.GetPosition();
				query.m_queryForward = cameraTransform.GetForward();
				query.m_extendedArea = extendedArea;
				return m_orbSystem->TestActivation( query );
			}

			return OrbActivationResult::INVALID();
		}

		Bool CActivator::TestVisualizerActivation( CName visFamilyName ) const
		{
			return m_visualizerController->IsVisualizerPrereqTestSuccess( visFamilyName );
		}

		Bool CActivator::IsLayerActive( const LayerGlobalID& layerGlobalId ) const
		{
			return m_hotSpotGroupRegistry->IsActive( layerGlobalId );
		}

		red::Optional< Float > CActivator::GetNormalizedLayerPriority( const LayerGlobalID& layerGlobalId ) const
		{
			return m_hotSpotGroupRegistry->GetNormalizedLayerPriority( layerGlobalId );
		}

		red::Optional< HotSpotGroupRegistryEntry > CActivator::GetRegistryEntry( const LayerGlobalID& layerGlobalId ) const
		{
			return m_hotSpotGroupRegistry->GetRegistryData( layerGlobalId );
		}

		LookAtContext CActivator::CreateLookAtContext() const
		{
			LookAtContext ctx;
			ctx.m_activatorId = GetRef();
			ctx.m_referencePt = m_inputData.GetCameraTransform().GetPosition3();
			ctx.m_referenceForward = m_inputData.GetCameraTransform().GetForward();
			return ctx;
		}

		ActivatorMappinHandle CActivator::CreateMappinHandle( const HotSpotID& hotSpotId ) const
		{
			if( m_mappinController && m_mappinController->HasMappin( hotSpotId ) )
			{
				return ActivatorMappinHandle{ *m_mappinController, hotSpotId };
			}
			else
			{
				return ActivatorMappinHandle{};
			}
		}

		ActivatorLookAtResult CActivator::GetInteractionTargetFilterResult( const ent::Entity* obj ) const
		{
			const ent::EntityID entityId = obj->GetEntityID();

			ActivatorLookAtResult lookAtResult;
			lookAtResult.m_entityId = entityId;
			lookAtResult.m_hitType = ActivatorLookAtResult::HitType::None;

			for( const auto& entry : m_logicalFilterResult->m_allInteractable )
			{
				if( entry.m_entityId == entityId )
				{
					if( entityId == m_logicalFilterResult->m_idealEntId )
					{
						lookAtResult.m_hitType = ActivatorLookAtResult::HitType::Ideal;
					}
					else if( entityId == m_logicalFilterResult->m_hitEntId )
					{
						lookAtResult.m_hitType = ActivatorLookAtResult::HitType::Closest;
					}
					else
					{
						lookAtResult.m_hitType = ActivatorLookAtResult::HitType::Any;
					}
					lookAtResult.m_isObstructed = entry.m_entityId != m_logicalFilterResult->m_obstructingEntId && entry.m_squaredDistance > m_logicalFilterResult->m_sqDistToObstruction;
					lookAtResult.m_squaredDistance = entry.m_squaredDistance;
					break;
				}
			}

			return lookAtResult;
		}

		void CActivator::SetChoices( const HotSpotNodeID& hotSpotLinkedLayersId, const red::DynArray< Choice >& choices )
		{
			if( m_visualizerController )
			{
				m_visualizerController->SetChoices( hotSpotLinkedLayersId, choices );
			}
		}

		void CActivator::ResetChoices( const HotSpotNodeID& hotSpotLinkedLayersId )
		{
			if( m_visualizerController )
			{
				m_visualizerController->ResetChoices( hotSpotLinkedLayersId );
			}
		}

		Bool CActivator::GetChoice( const HotSpotNodeID& hotSpotLinkedLayersId, Choice& outChoice )
		{
			if( m_visualizerController )
			{
				return m_visualizerController->GetChoice( hotSpotLinkedLayersId, outChoice );
			}

			return false;
		}

		void CActivator::OnModifiedHotSpot( const CHotSpot& hotSpot )
		{
			red::ArraySpan< const LayerGlobalID > hotSpotLayers = hotSpot.GetLayerGlobalIDs();
			red::DynArray< LayerLocalID > activeLayers{ red::PoolFrame{} };
			activeLayers.Reserve( hotSpotLayers.Size() );

			for( Uint32 layerLocalIdx : hotSpotLayers.Indices() )
			{
				const LayerGlobalID& layerGlobalId = hotSpotLayers[ layerLocalIdx ];
				if( IsLayerActive( layerGlobalId ) )
				{
					activeLayers.PushBack( LayerLocalID{ static_cast< LayerLocalID::UnderlyingType >( layerLocalIdx + 1 ) } );
				}
			}

			if( !activeLayers.Empty() )
			{
				if( m_visualizerController )
				{
					m_visualizerController->OnModifiedHotSpot( hotSpot, activeLayers );
				}

				if( m_mappinController )
				{
					m_mappinController->OnModifiedHotSpot( hotSpot, activeLayers );
				}
			}
		}

		void CActivator::SetGlobalVisualizerActivationPrereq( const THandle< game::IPrereq >& prereq )
		{
			m_visualizerController->SetGlobalActivationPrereq( prereq );
		}

		void CActivator::Dbg_RefreshVisualizers()
		{
			if( m_visualizerController )
			{
				m_visualizerController->Dbg_Refresh();
			}
		}

		void CActivator::PrepareForActivationTests( const THotSpotSharedPtrs& hotSpots, const LayerGlobalIDMask& removeMask )
		{
			m_updateTemps.removeMask = removeMask;

			if( s_cpRetryActivator == this && !m_logicalFilterTicket.IsValid() && m_logicalFilter )
			{
				if( auto targetingSystem = m_targetingSystem.ToHandle() )
				{
					m_logicalFilterTicket = targetingSystem->RegisterLookAtFilter( s_cpRetryOwner, m_logicalFilter );
					if( m_logicalFilterTicket.IsValid() )
					{
						std::fprintf( stderr, "[coreplay] activator: logical lookAt filter registered on retry (interactions enabled)NL" );
						s_cpRetryActivator = nullptr;
					}
				}
			}

			// cache result from logical filter
			if( m_logicalFilter )
			{
				m_logicalFilter->GetResult( m_logicalFilterResult );
			}

			if( m_visualizerController )
			{
				// update visualizer prereq activation tests
				m_visualizerController->UpdateVisualizerPrereqs();
			}

			// TODO_MW: NEXT_ITR => right now IgnoresLookAtTest scope only exists and it doesn't make sense to create entire system just for this functionality
			// However, it would be great to sieve things by activator prereq tests
			// Also, it would be great to prevent updating transform layers if linkedlayers are disabled
			for( const auto& entry : m_logicalFilterResult->m_allInteractable )
			{
				m_updateTemps.lookAtEntities.Emplace( entry.m_entityId );
			}

			// CP2077 web [coreplay]: activator probe (armed by the sandbox test together with the lookAt probe)
			if( g_coreplayActivatorProbe > 0 && ( --g_coreplayActivatorProbe % 50 == 0 || ( m_logicalFilterResult && m_logicalFilterResult->m_allInteractable.Size() > 0 ) ) )
			{
				const Vector4 camPos = m_inputData.GetCameraTransform().GetPosition();
				std::fprintf( stderr, "[coreplay] activator: hotspots=%u allInteractable=%u ideal=%s obstructing=%s camera=(%.1f, %.1f, %.1f)\n",
					Uint32( hotSpots.Size() ), Uint32( m_logicalFilterResult ? m_logicalFilterResult->m_allInteractable.Size() : 0 ),
					m_logicalFilterResult ? m_logicalFilterResult->m_idealEntId.ToDebugString().AsChar() : "-",
					m_logicalFilterResult ? m_logicalFilterResult->m_obstructingEntId.ToDebugString().AsChar() : "-", camPos.X, camPos.Y, camPos.Z );
				if( m_logicalFilterResult )
				{
					for( const auto& e : m_logicalFilterResult->m_allInteractable )
					{
						std::fprintf( stderr, "[coreplay] activator   interactable %s dist %.2f\n", e.m_entityId.ToDebugString().AsChar(), std::sqrt( e.m_squaredDistance ) );
					}
				}
				Uint32 n = 0;
				for( const THotSpotSharedPtr& hs : hotSpots )
				{
					if( n++ >= 40 ) break;
					std::fprintf( stderr, "[coreplay] activator   hotspot ref %s active=%d enabled=%d validOwner=%d\n", hs->GetRef().ToDebugString().AsChar(), hs->IsActive() ? 1 : 0, hs->IsEnabled() ? 1 : 0, hs->IsValidOwner() ? 1 : 0 );
				}
			}

			// always update hot spots at which player is looking
			Uint32 lookAtUpdatesCount = 0;
			for( const THotSpotSharedPtr& hotSpot : hotSpots )
			{
				if( m_updateTemps.lookAtEntities.Exist( hotSpot->GetRef() ) )
				{
					++lookAtUpdatesCount;

					hotSpot->MarkLayersForUpdate( m_updateTemps.updateMask );
					m_updateTemps.frameUpdateHotSpots.Insert( hotSpot.Get() );
				}
			}

			// fill out the rest of available slots in the circular order
			const Uint32 hotSpotCount = hotSpots.Size();
			const Uint32 thisFrameCount = Min( c_hotSpotsPerFrame, hotSpotCount );
			const Uint32 startingIndex = m_updateTemps.nextHotSpotToUpdate;
			for( Uint32 i = lookAtUpdatesCount; i < thisFrameCount; ++i )
			{
				m_updateTemps.nextHotSpotToUpdate = m_updateTemps.nextHotSpotToUpdate % hotSpotCount;
				const THotSpotSharedPtr& hotSpot = hotSpots[ m_updateTemps.nextHotSpotToUpdate++ ];

				hotSpot->MarkLayersForUpdate( m_updateTemps.updateMask );
				m_updateTemps.frameUpdateHotSpots.Insert( hotSpot.Get() );
			}

			// prepare debug storage for this frame
			m_debugDataStorage->Prepare( hotSpots.Size() );
		}

		void CActivator::ProcessInteractions( Float timeDelta, const THotSpotSharedPtrs& hotSpots, TPublishRequests& events, job::Builder& builder, const LayerGlobalIDMask& removeMask )
		{
			RED_ASSERT( m_updateTemps.updateMask.IsNoneSet() );
			RED_ASSERT( m_updateTemps.removeMask.IsNoneSet() );
			RED_ASSERT( m_updateTemps.frameUpdateHotSpots.Empty() );
			RED_ASSERT( m_updateTemps.lookAtEntities.Empty() );

			PrepareForActivationTests( hotSpots, removeMask );

			builder.DispatchParallelForJobWithEpilogue( "CActivator::ProcessInteractionsWithEpilogue",
				{ m_updateTemps.frameUpdateHotSpots.Size() },
			[ this, &hotSpots, timeDelta ]( Uint32 index, const job::RunContext& runCtx ) {
				CHotSpot* hotspot = *( m_updateTemps.frameUpdateHotSpots.Begin() + index);
				if( hotspot->IsActive() )
				{
					INTER_PC_SCOPE( CActivator::InteractionActivationTest );

#ifdef INTERACTION_DEBUG_ENABLED
					ActivationContext ctx{ runCtx.dispatcherThreadIndex, *hotspot, *this, timeDelta, m_debugDataStorage->GrabCollector( index, *hotspot ) };
#else
					ActivationContext ctx{ runCtx.dispatcherThreadIndex, *hotspot, *this, timeDelta, nullptr };
#endif
					auto activationGuide = CreateGuide< ActivationVisitor >( ctx );
					const HotSpotDefinitionSet& definitionSet = hotspot->GetDefinitionSet();

					const Bool useIgnoresLookAtTestScope = !m_updateTemps.lookAtEntities.Exist( hotspot->GetRef() ) && definitionSet.HasScopedDefinition( HotSpotScopeType::IgnoresLookAtTest );

					if( useIgnoresLookAtTestScope )
					{
						activationGuide.Guide( definitionSet.GetScopedDefinition( HotSpotScopeType::IgnoresLookAtTest ) );
					}
					else
					{
						activationGuide.Guide( definitionSet.GetMainDefinition() );
					}
				}
			},
				[ this, &events ]( const job::RunContext& ) {
					m_hotSpotGroupRegistry->Process( events, *m_hotSpotGroupCollector, m_updateTemps.updateMask, m_updateTemps.removeMask );

					m_updateTemps.frameUpdateHotSpots.Clear();
					m_updateTemps.updateMask.ClearAll();
					m_updateTemps.removeMask.ClearAll();
					m_updateTemps.lookAtEntities.Clear();
			} );
		}

		void CActivator::ProcessEpilogue( Float timeDelta )
		{
			if( m_visualizerController )
			{
				m_visualizerController->Tick( timeDelta );
			}

			if( m_mappinController )
			{
				m_mappinController->Tick();
			}
		}

		void CActivator::OnInteractionEvent( EInteractionEventType eventType, const CHotSpot& hotSpot, const CHotSpotLayerDefinition& layerDef ) const
		{
			const Bool isActivated = eventType == EInteractionEventType::EIET_activate;

			if( m_orbSystem )
			{
				m_orbSystem->NotifyActivation( GetRef(), layerDef );
			}

			if( m_mappinController )
			{
				m_mappinController->OnActivationChanged( isActivated, hotSpot, layerDef );
			}

			if( m_visualizerController )
			{
				m_visualizerController->OnActivationChanged( isActivated, hotSpot, layerDef );
			}

			if( m_internalListener )
			{
				CInteractionEventDescription description{ eventType };
				m_internalListener->OnInteractionEvent( description, hotSpot, layerDef, *this );
			}
		}

		void CActivator::OnRemoved()
		{
			if( auto targetingSys = m_targetingSystem.ToHandle() )
			{
				targetingSys->UnregisterLookAtFilter( GetObject(), m_logicalFilterTicket );
			}

			m_hotSpotGroupCollector->Clear();
			// processing group collector with no entries will generate deactivate events for all active hotspots

			LayerGlobalIDMask allMask;
			allMask.SetAll();

			TPublishRequests events{ game::PoolGMPL_Interactions() };
			m_hotSpotGroupRegistry->Process( events, *m_hotSpotGroupCollector, allMask, LayerGlobalIDMask{} );
			for( CPublishRequest& deactivateEvent : events )
			{
				deactivateEvent.Call();
			}

			if( m_visualizerController )
			{
				m_visualizerController.Reset();
			}
		}

		void CActivator::SetInternalListener( IListener* internalListener )
		{
			m_internalListener = internalListener;
		}
	}
}

#ifdef DEBUG_GAMEPLAY
#pragma optimize("",on)
#endif

