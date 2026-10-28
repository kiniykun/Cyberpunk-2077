// Copyright (C) 2018-2020 CD Projekt Red. All Rights Reserved.

#include "build.h"
#include "scnsSceneEntitiesManager.h"
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
#include "scnsSceneEntitiesPatch.h"
#include "scnsSceneEntityAssignment.h"
#include "scnsSceneEntityProcedure.h"
#include "scnsSceneEntityResolver.h"
#include "scnsSceneEntitiesListener.h"

#include "scnSceneInstance.h"
#include "scnDebug.h"
#include "scnsComposer.h"

#include "../../gameTweakDB/include/records/tweakDBProp.h"
#include "../../gameTweakDB/include/records/tweakDBCharacter.h"
#include "../../gameTweakDB/include/records/tweakDBItem.h"
#include "../../gameCore/include/entitySpawnerEventsBroadcasterInterface.h"
#include "../../gameCore/include/dynamicEntityIDSystemInterface.h"
#include "../../game/include/attachmentSlotsListener.h"
#include "../../gameSpawn/include/spawnListener.h"

#include "../../world/include/gameInstance.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../world/include/runtimeSystemNodeStreaming.h"
#include "../../worldEntities/include/runtimeEntityRegistry.h"
#include "../../worldEntities/include/worldNodeInstanceEntity.h"
#include "../../worldNavigation/include/runtimeSystemNavigation.h"
#include "../../game/include/aiRuntimeUtils.h"

namespace scn
{
	namespace prv
	{
		struct SpawnDespawnItem
		{
			RTTI_DECLARE_TYPE( SpawnDespawnItem );
			game::data::RecordID m_recordID;
			Transform m_finalTransform;
		};
	}

	namespace
	{
		template < typename Container >
		void ShrinkOvergrownBuffer( Container& container )
		{
			Bool shrinkBuffer = ( container.Capacity() > 2 * container.Size() );
			if ( shrinkBuffer )
			{
				container.Shrink();
			}
		}
	}
}

RTTI_BEGIN_TYPE_IN_NAMESPACE( SpawnDespawnItem, scn, prv )
	RTTI_PROPERTY( m_recordID );
	RTTI_PROPERTY( m_finalTransform );
RTTI_END_TYPE()

namespace scn
{
	class EntityRegistryCallbackImpl : public world::IEntityRegistryCallback
	{
		RED_USE_MEMORY_POOL( PoolSceneSystem );
	public:
		EntityRegistryCallbackImpl( ent::EntityID entityId, const game::IGameInstance& gameInstance, SynchronizedEntityEvents& entityEvents )
			: m_entityId( entityId )
			, m_gameInstance( gameInstance )
			, m_entityEvents( entityEvents )
		{
			m_gameInstance.GetScene().GetSystemRef< world::RuntimeEntityRegistry >().RegisterCallback( m_entityId, *this );
		}

		virtual ~EntityRegistryCallbackImpl() override
		{
			m_gameInstance.GetScene().GetSystemRef< world::RuntimeEntityRegistry >().UnregisterCallback( m_entityId, *this );
		}

		virtual void OnRegisterEntity( ent::Entity& obj ) override
		{
			auto accessToken = m_entityEvents.AcquireAccess();
			EntityEvents& events = accessToken.GetContents();
			events.PushBack( { obj.GetEntityID(), EntityEvent::Type::attach } );
		}

		virtual void OnUnregisterEntity( ent::Entity& obj ) override
		{
			auto accessToken = m_entityEvents.AcquireAccess();
			EntityEvents& events = accessToken.GetContents();
			events.PushBack( { obj.GetEntityID(), EntityEvent::Type::detach } );
		}

	private:
		ent::EntityID m_entityId;
		const game::IGameInstance& m_gameInstance;
		SynchronizedEntityEvents& m_entityEvents;
	};

	class EntityItemsListener : public game::IAttachmentSlotsListener
	{
		RTTI_DECLARE_TYPE( EntityItemsListener );
		RED_USE_MEMORY_POOL( PoolSceneSystem );
	public:
		EntityItemsListener() : EntityItemsListener( ent::EntityID(), nullptr ) {}
		EntityItemsListener( const ent::EntityID ownerEntityId, SynchronizedEntityEvents* entityEvents )
			: m_ownerEntityId( ownerEntityId )
			, m_entityEvents( entityEvents )
		{}

		void OnItemEquipped( const game::ItemID& itemID, const game::data::RecordID& slotId ) override
		{
			auto accessToken = m_entityEvents->AcquireAccess();
			EntityEvents& events = accessToken.GetContents();
			events.PushBack( { m_ownerEntityId, EntityEvent::Type::itemEquipped } );
		}

		void OnItemUnequipped( const game::ItemID& itemID, const game::data::RecordID& slotId ) override
		{
			auto accessToken = m_entityEvents->AcquireAccess();
			EntityEvents& events = accessToken.GetContents();
			events.PushBack( { m_ownerEntityId, EntityEvent::Type::itemUnequipped } );
		}

		void OnAttachmentPreRefreshed( const game::ItemID& itemID, const game::data::RecordID& slotId ) override {};
		void OnAttachmentRefreshed( const game::ItemID& itemID, const game::data::RecordID& slotId ) override {};

	private:
		const ent::EntityID m_ownerEntityId;
		SynchronizedEntityEvents* m_entityEvents;
	};

	SceneEntityAssignmentInfo::SceneEntityAssignmentInfo( ent::EntityID entityId, SceneEntityAssignmentId sceneEntityAssignmentId, SceneEntityAssignmentStatus assignmentStatus, scn::SceneInstanceId sceneInstanceId )
		: m_entityId( entityId )
		, m_sceneEntityAssignmentId( sceneEntityAssignmentId )
		, m_assignmentStatus( assignmentStatus )
		, m_sceneInstanceId( sceneInstanceId )
	{
	}

	SceneEntitiesManager::AssignEntityRequest::AssignEntityRequest( const SceneEntityAssignmentId sceneEntityAssignmentId, EntityAcquisition sceneEntityAcquisition, AssignmentProcedures procedures, ent::EntitySpawnPriority spawnPriority )
		: m_sceneEntityAssignmentId( sceneEntityAssignmentId )
		, m_sceneEntityAcquisition( std::move( sceneEntityAcquisition ) )
		, m_procedures( std::move( procedures ) )
		, m_spawnPriority( spawnPriority )
	{}

	SceneEntitiesManager::SceneEntitiesManager( const game::IGameInstance& gameInstance, SceneEntityStateRepository& repository, ForwardToEntityParams forwardToEntityParams )
		: m_gameInstance( gameInstance )
		, m_entityStateRepository( repository )
		, m_sceneEntitySpawnService( gameInstance )
		, m_spawnerObjectCallbackId( 0 )
		, m_sceneEntityAssignments{ PoolSceneSystem() }
		, m_sceneEntities{ PoolSceneSystem() }
		, m_sceneEntityListeners{ PoolSceneSystem() }
		, m_forwardToEntityParams( forwardToEntityParams )
	{
	}

	SceneEntitiesManager::~SceneEntitiesManager()
	{
		RED_ASSERT( m_spawnerObjectCallbackId == 0, "Shutdown was never called!");
	}

	void SceneEntitiesManager::Initialize()
	{
		game::IEntitySpawnerEventsBroadcaster& spawnerEventsBroadcaster = m_gameInstance.GetSystem< game::IEntitySpawnerEventsBroadcaster >();
		m_spawnerObjectCallbackId = spawnerEventsBroadcaster.RegisterCallback( [ this ]( const game::EntitySpawnerEventType eventType, const ent::EntityID& entityId, const game::CommunityID& communityId, game::EntityStub* stub )
		{
			switch ( eventType )
			{
				case game::EntitySpawnerEventType::StubCreation:
				{
					auto accessToken = m_entityEvents.AcquireAccess();
					EntityEvents& events = accessToken.GetContents();
					events.PushBack( { entityId, EntityEvent::Type::stubCreation } );
					break;
				}
				case game::EntitySpawnerEventType::StubDeletion:
				{
					auto accessToken = m_entityEvents.AcquireAccess();
					EntityEvents& events = accessToken.GetContents();
					events.PushBack( { entityId, EntityEvent::Type::stubDeletion } );
					break;
				}
				default:
				{
					break;
				}
			}
		}, "SpawnerCallback_SceneEntitiesManager" );
	}
	
	void SceneEntitiesManager::Shutdown()
	{
		game::IEntitySpawnerEventsBroadcaster& spawnerEventsBroadcaster = m_gameInstance.GetSystem< game::IEntitySpawnerEventsBroadcaster >();
		spawnerEventsBroadcaster.UnregisterCallback( m_spawnerObjectCallbackId );
		m_spawnerObjectCallbackId = 0;

		ProcessPendingRequests( false );

		// release assignments

		auto accessToken = m_sceneEntityAssignments.AcquireAccess();
		SceneEntityAssignments& assignments = accessToken.GetContents();

		// release dependent assignments
		{
			auto iter = std::remove_if( m_dependentEntityAssignments.Begin(), m_dependentEntityAssignments.End(), [ this, &assignments ]( const DependentEntityAssignment& dependentAssignment )
			{
				const auto foundAssignment = assignments.Find( dependentAssignment.m_sceneEntityAssignmentId );
				if ( foundAssignment != assignments.End() )
				{
					const SceneEntityAssignment& assignment = foundAssignment.Value();
					if ( assignment.m_assignmentStatus == SceneEntityAssignmentStatus::Success )
					{
						ReleaseSceneEntity( assignment.m_assignmentId, assignment.m_entityId );
					}

					assignments.Remove( foundAssignment );
				}
				return true;
			} );

			m_dependentEntityAssignments.Remove( iter, m_dependentEntityAssignments.End() );
		}

		// release currently being spawned entities
		{
			for( const LocalEntitySpawning& les : m_localEntitiesSpawning )
			{
				ReleaseSceneLocalEntity( les.m_sceneEntityAssignmentId );
				assignments.Remove( les.m_sceneEntityAssignmentId );
			}

			m_localEntitiesSpawning.Clear();
		}

		// release assignments
		{
			for ( const auto& it : assignments )
			{
				if ( it.Value().m_assignmentStatus == SceneEntityAssignmentStatus::Success )
				{
					ReleaseSceneEntity( it.Key(), it.Value().m_entityId );
				}
			}

			assignments.Clear();
		}
	}

	SceneEntityAssignmentId SceneEntitiesManager::Assign( const EntityAcquisition& entityAcquisition, PerformerId performerId, const SceneInstance& sceneInstance, AssignmentProcedures procedures, ent::EntitySpawnPriority spawnPriority )
	{
		const SceneEntityAssignmentId sceneEntityAssignmentId = m_assignmentsIdGen.Generate();

		// add new assignment
		{
			auto accessToken = m_sceneEntityAssignments.AcquireAccess();
			SceneEntityAssignments& assignments = accessToken.GetContents();
			RED_VERIFY( assignments.Emplace( sceneEntityAssignmentId, sceneEntityAssignmentId, performerId, sceneInstance ).IsSuccessful() );
		}

		// add new assign entity request
		{
			auto accessToken = m_assignEntityRequests.AcquireAccess();
			AssignEntityRequests& requests = accessToken.GetContents();
			requests.EmplaceBack( sceneEntityAssignmentId, entityAcquisition, std::move( procedures ), spawnPriority );
		}

		return sceneEntityAssignmentId;
	}

	void SceneEntitiesManager::Release( const SceneEntityAssignmentId sceneEntityAssignmentId )
	{
		ReleaseEntityRequest releaseRequest;
		releaseRequest.m_sceneEntityAssignmentId = sceneEntityAssignmentId;

		// remove assignment
		{
			auto accessToken = m_sceneEntityAssignments.AcquireAccess();
			SceneEntityAssignments& assignments = accessToken.GetContents();
			const auto foundAssignment = assignments.Find( sceneEntityAssignmentId );
			RED_ASSERT( foundAssignment != assignments.End(), "Assignment does not exist." );

			SceneEntityAssignment& assignment = foundAssignment.Value();
			releaseRequest.m_entityId = assignment.m_entityId;
			releaseRequest.m_performerId = assignment.GetPerformerId();
			releaseRequest.m_sceneInstanceId = assignment.GetSceneInstanceId();

			assignments.Remove( foundAssignment );
		}

		{
			auto accessToken = m_releaseEntityRequests.AcquireAccess();
			ReleaseEntityRequests& requests = accessToken.GetContents();
			requests.PushBack( std::move( releaseRequest ) );
		}
	}

	void SceneEntitiesManager::RegisterListener( ISceneEntitiesListener& listener )
	{
		auto token = m_sceneEntityListeners.AcquireAccess();
		SceneEntityListeners& content = token.GetContents();
		RED_VERIFY( content.Insert( &listener ).IsSuccessful() );
	}
	
	void SceneEntitiesManager::UnregisterListener( ISceneEntitiesListener& listener )
	{
		auto token = m_sceneEntityListeners.AcquireAccess();
		SceneEntityListeners& content = token.GetContents();
		RED_VERIFY( content.Remove( &listener ).IsSuccessful() );
	}

	void SceneEntitiesManager::ActivateSceneLocalEntity( ent::EntityID entityId )
	{
		m_entityStateRepository.PushActivateLocalEntityRequest( entityId, false );
	}

	void SceneEntitiesManager::ActivateSceneLocalEntity( CName uniqueName, Bool isCustomTransform, WorldTransform customTransform )
	{
		const ent::EntityID entityId = m_gameInstance.GetSystem< game::IDynamicEntityIDSystem >().AcquireEntityIDByUniqueName( uniqueName );
		m_entityStateRepository.PushActivateLocalEntityRequest( entityId, isCustomTransform, customTransform );
	}
	
	void SceneEntitiesManager::DeactivateSceneLocalEntity( ent::EntityID entityId )
	{
		m_entityStateRepository.PushDeactivateLocalEntityRequest( entityId );
	}

	Bool SceneEntitiesManager::IsSceneLocalEntityActive( ent::EntityID entityId ) const
	{
		return m_entityStateRepository.IsSceneLocalEntityActive( entityId );
	}

	Bool SceneEntitiesManager::AreSceneLocalEntityAssignmentsResolved( ent::EntityID entityId ) const
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		const SceneEntityAssignments& assignments = accessToken.GetContents();

		for( const auto& it : assignments )
		{
			const ent::EntityID entityID = it.Value().m_entityId;
			if ( entityID.IsDefined() )
			{
				if ( entityID == entityId )
				{
					if ( it.Value().m_assignmentStatus == SceneEntityAssignmentStatus::InProgress )
					{
						return false;
					}
				}
			}
		}

		return true;
	}

	SceneEntityAssignmentStatus SceneEntitiesManager::GetSceneEntityAssignmentStatus( const SceneEntityAssignmentId sceneEntityAssignmentId ) const
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		const SceneEntityAssignments& assignments = accessToken.GetContents();
		const auto foundAssignment = assignments.Find( sceneEntityAssignmentId );
		RED_ASSERT( foundAssignment != assignments.End(), "Assignment does not exist." );
		return foundAssignment.Value().m_assignmentStatus;
	}

	ent::EntityID SceneEntitiesManager::GetAssignedSceneEntityId( const SceneEntityAssignmentId sceneEntityAssignmentId ) const
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		const SceneEntityAssignments& assignments = accessToken.GetContents();
		auto foundAssignment = assignments.Find( sceneEntityAssignmentId );
		return ( foundAssignment != assignments.End() ) ? foundAssignment.Value().m_entityId : ent::EntityID(); 
	}

	SceneEntity* SceneEntitiesManager::GetSceneEntity( const SceneEntityAssignmentId sceneEntityAssignmentId ) const
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		const SceneEntityAssignments& assignments = accessToken.GetContents();
		auto foundAssignment = assignments.Find( sceneEntityAssignmentId );
		return ( foundAssignment != assignments.End() ) ? foundAssignment.Value().GetSceneEntity() : nullptr; 
	}

	SceneEntity* SceneEntitiesManager::GetSceneEntity( const ent::EntityID entityId ) const
	{
		auto accessToken = m_sceneEntities.AcquireSharedAccess();
		const SceneEntities& sceneEntities = accessToken.GetContents();
		auto foundEntity = sceneEntities.Find( entityId );
		return ( foundEntity != sceneEntities.End() ) ? foundEntity.Value().Get() : nullptr;
	}

	void SceneEntitiesManager::GetSceneEntityAssignmentsInfo( red::DynArray< SceneEntityAssignmentInfo >& outAssignmentsInfo ) const
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		const SceneEntityAssignments& assignments = accessToken.GetContents();
		
		outAssignmentsInfo.Reserve( assignments.Size() );
		for( const auto& iCurr : assignments )
		{
			const SceneEntityAssignment& assignment = iCurr.Value();
			outAssignmentsInfo.EmplaceBack( assignment.m_entityId, iCurr.Key(), assignment.m_assignmentStatus, assignment.GetSceneInstanceId() );
		}
	}

	void SceneEntitiesManager::ProcessPendingRequests( Bool isLoadingSave )
	{
		red::HashSet< ent::EntityID > scheduledToDeleteStubs { PoolFrameSceneSystem() };
		red::HashSet< ent::EntityID > scheduledToDespawnEntities { PoolFrameSceneSystem() };
		ProcessEntityEvents( scheduledToDeleteStubs, scheduledToDespawnEntities );

		// process release entity requests
		{
			auto accessToken = m_releaseEntityRequests.AcquireAccess();
			ReleaseEntityRequests& requests = accessToken.GetContents();

			auto iter = std::remove_if( requests.Begin(), requests.End(), [ this ]( const ReleaseEntityRequest& request ) { return ProcessReleaseEntityRequests( request ); } );

			Bool shrinkBuffer = ( requests.Capacity() > 2 * requests.Size() );

			requests.Remove( iter, requests.End() );

			if ( shrinkBuffer )
			{
				requests.Shrink();
			}
		}

		// process assign entity requests
		{
			auto accessToken = m_assignEntityRequests.AcquireAccess();
			AssignEntityRequests& requests = accessToken.GetContents();

			auto iter = std::remove_if( requests.Begin(), requests.End(), [ this, &scheduledToDeleteStubs, &scheduledToDespawnEntities ]( AssignEntityRequest& request )
			{ 
				if ( !ProcessAssignEntityRequests( request, scheduledToDeleteStubs, scheduledToDespawnEntities ) )
				{
					m_pendingAssignEntityRequests.PushBack( std::move( request ) );
				}

				return true;
			} );

			Bool shrinkBuffer = ( requests.Capacity() > requests.Size() * 2 );

			requests.Remove( iter, requests.End() );

			if ( shrinkBuffer )
			{
				requests.Shrink();
			}
		}

		LinkPendingStaticEntities();

		// Process local entities requests
		{
			m_entityStateRepository.ForEachLocalEntityRequests( [this, isLoadingSave]( const SceneLocalEntityRequest& ler ) { return ProcessLocalEntityRequest( ler, isLoadingSave ); } );
		}

		// process entity spawning
		{
			auto iter = std::remove_if( m_localEntitiesSpawning.Begin(), m_localEntitiesSpawning.End(), [ this, &scheduledToDeleteStubs, &scheduledToDespawnEntities ]( LocalEntitySpawning& les ) { return ProcessLocalEntitiesSpawn( les, scheduledToDeleteStubs, scheduledToDespawnEntities ); } );
			m_localEntitiesSpawning.Remove( iter, m_localEntitiesSpawning.End() );
		}

		ProcessDependancyAssignments( scheduledToDeleteStubs, scheduledToDespawnEntities );

		PublishSceneEntityNotifications();
		ShrinkBuffers();
	}

	void SceneEntitiesManager::ShrinkBuffers()
	{
		ShrinkOvergrownBuffer( m_dependentAssignmentsChange );
		ShrinkOvergrownBuffer( m_pendingStaticEntitiesToLink );
		ShrinkOvergrownBuffer( m_entityRegistryCallbacks );
		ShrinkOvergrownBuffer( m_localEntities );
		ShrinkOvergrownBuffer( m_localEntitiesSpawning );
		ShrinkOvergrownBuffer( m_sceneEntities.AcquireAccess().GetContents() );
	}

	void SceneEntitiesManager::TickEntities( Float deltaTime )
	{
		auto accessToken = m_sceneEntities.AcquireSharedAccess();
		SceneEntities& sceneEntities = accessToken.GetContents();

		for ( auto it : sceneEntities )
		{
			it.Value()->Tick( deltaTime );
		}
	}

	void SceneEntitiesManager::DispatchRequests()
	{
		m_forwardToEntityParams.m_animRequestGatherer.DispatchRequests();
	}

	Bool SceneEntitiesManager::GetSpawnTime( ent::EntityID entityId, EngineTime& startSpawn, EngineTime& endSpawn ) const
	{
		auto found = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [entityId]( const LocalEntity & localEntity ){ return localEntity.m_entityId == entityId; } );
		if( found != m_localEntities.End() )
		{
			startSpawn = found->m_startSpawn;
			endSpawn = found->m_endSpawn;
			return true;
		}
		return false;
	}

	// CP2077 web [nomadplay]: scene performer acquisition trace: 'PENDING' the first time a performer cannot be
	// resolved (spawner/community/spawnset/findInWorld not available yet), 'STILL PENDING' every 30 s, 'RESOLVED'.
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

	struct NpPendingEntry { std::string name; Int32 plan; Double t0; Double next; };
	static std::mutex s_npSceneMutex;
	static std::vector< NpPendingEntry > s_npScenePending;
	static void NpScenePending( const String& performerName, Int32 plan, Bool resolved )
	{
		if ( !NpLogOn() )
		{
			return;
		}
#ifdef __EMSCRIPTEN__
		const Double now = emscripten_get_now() * 0.001;
#else
		const Double now = 0.0;
#endif
		std::lock_guard< std::mutex > lock( s_npSceneMutex );
		const std::string name = performerName.AsChar();
		for ( size_t i = 0; i < s_npScenePending.size(); ++i )
		{
			NpPendingEntry& e = s_npScenePending[ i ];
			if ( e.name == name && e.plan == plan )
			{
				if ( resolved )
				{
					std::fprintf( stderr, "[nomadplay] scene performer RESOLVED '%s' plan=%d after %.1fs\n", name.c_str(), plan, now - e.t0 );
					s_npScenePending.erase( s_npScenePending.begin() + i );
				}
				else if ( now >= e.next )
				{
					e.next = now + 30.0;
					std::fprintf( stderr, "[nomadplay] scene performer STILL PENDING '%s' plan=%d for %.0fs\n", name.c_str(), plan, now - e.t0 );
				}
				return;
			}
		}
		if ( !resolved )
		{
			std::fprintf( stderr, "[nomadplay] scene performer PENDING '%s' plan=%d (1 ctx 2 world 3 spawnDespawn 4 inEntity 5 spawnSet 6 community 7 spawner 8 node)\n", name.c_str(), plan );
			s_npScenePending.push_back( NpPendingEntry{ name, plan, now, now + 30.0 } );
		}
	}

	Bool SceneEntitiesManager::ProcessAssignEntityRequests( AssignEntityRequest& request, const red::HashSet< ent::EntityID >& scheduledToDeleteStubs, const red::HashSet< ent::EntityID >& scheduledToDespawnEntities )
	{
		const SceneEntityAssignmentId sceneEntityAssignmentId = request.m_sceneEntityAssignmentId;

		auto accessToken = m_sceneEntityAssignments.AcquireAccess();
		SceneEntityAssignments& assignments = accessToken.GetContents();
		auto foundAssignment = assignments.Find( sceneEntityAssignmentId );
		if ( foundAssignment == assignments.End() )
		{
			return true;// assignment already released
		}

		SceneEntityAssignment& assignment = foundAssignment.Value();
		RED_ASSERT( assignment.m_assignmentStatus == SceneEntityAssignmentStatus::InProgress );

		const EntityAcquisitionPlan plan = request.m_sceneEntityAcquisition.m_plan;
		const String& performerName = request.m_sceneEntityAcquisition.m_name;
		const rtti::Variant& params = request.m_sceneEntityAcquisition.m_params;
		const SceneInstance& sceneInstance = assignment.GetSceneInstance();

		switch ( plan )
		{
			case EntityAcquisitionPlan::findInContext:
			{
				const resolver::ResolveResult result = resolver::ResolveEntityFindInContext( performerName, params.Get< FindEntityInContextParams >(), sceneInstance.GetContextActors(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				ContextualActorName contextualActorName = params.Get< FindEntityInContextParams >().m_contextualName;

				RequiredLinkageLevel linkageLevel;

				switch ( contextualActorName )
				{
					case ContextualActorName::Player:
					case ContextualActorName::ContextActorName:
						linkageLevel = RequiredLinkageLevel::full;
						break;

					case ContextualActorName::VoicesetHolder:
					case ContextualActorName::Voice:
					case ContextualActorName::SpecificVoicetagHolder:
						linkageLevel = RequiredLinkageLevel::entity;
						break;

					default:
						RED_FATAL( "Not supported linkage" );
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, linkageLevel, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::findInWorld:
			{
				resolver::ResolveResult result = resolver::ResolveEntityFindInWorld( performerName, params.Get< FindEntityInWorldParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::findInEntity:
			{
				if( !params.Get< FindEntityInEntityParams >().m_enabled )
				{
					return true;
				}

				DependentEntityAssignment& depAssignment = m_dependentEntityAssignments.EmplaceBack();
				depAssignment.m_plan = DependentEntityAssignment::Plan::findInEntity;
				depAssignment.m_performerName = performerName;
				depAssignment.m_params = params;
				depAssignment.m_sceneEntityAssignmentId = sceneEntityAssignmentId;
				depAssignment.m_sceneInstaceId = sceneInstance.GetSceneInstanceId();
				depAssignment.m_ownerPerformerId = params.Get< FindEntityInEntityParams >().GetPerformerId();
				depAssignment.m_procedures = std::move( request.m_procedures );
				const SceneEntityAssignmentId sceneEntityAssignmentId = sceneInstance.GetPerformerManager().GetPerformerAssignmentId( depAssignment.m_ownerPerformerId );
				if ( sceneEntityAssignmentId != SceneEntityAssignmentId::invalid )
				{
					m_dependentAssignmentsChange.Insert( sceneEntityAssignmentId );
				}
				
				return true;
			}

			case EntityAcquisitionPlan::spawnSet:
			{
				resolver::ResolveResult result = resolver::ResolveEntitySpawnSet( performerName, params.Get< SpawnSetParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::community:
			{
				resolver::ResolveResult result = resolver::ResolveEntityCommunity( performerName, params.Get< CommunityParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::spawner:
			{
				resolver::ResolveResult result = resolver::ResolveEntitySpawner( performerName, params.Get< SpawnerParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::findInNode:
			{
				resolver::ResolveResult result = resolver::ResolveEntityFindInNode( performerName, params.Get< FindEntityInNodeParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::entity, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::findNetworkPlayer:
			{
				resolver::ResolveResult result = resolver::ResolveEntityFindNetworkPlayer( performerName, params.Get< FindNetworkPlayerParams >(), m_gameInstance );
				if ( !result.IsResolved() )
				{
					NpScenePending( performerName, Int32( plan ), false ); // [nomadplay]
					return false;
				}

				NpScenePending( performerName, Int32( plan ), true ); // [nomadplay]
				FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, request.m_procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
				return true;
			}

			case EntityAcquisitionPlan::spawnDespawn:
			{
				const Bool npOk = TrySpawnSceneLocalEntity( performerName, params.Get< SpawnDespawnEntityParams >(), assignment, std::move( request.m_procedures ), request.m_spawnPriority );
				NpScenePending( performerName, Int32( plan ), npOk ); // [nomadplay]
				return npOk;
			}

			default:
			{
				RED_FATAL( "Unexpected EntityAcquisitionPlan." );
				break;
			}
		}

		return true;
	}

	void SceneEntitiesManager::FinalizeEntityAssignment( SceneEntityAssignment& assignment, const resolver::ResolveResult& result, RequiredLinkageLevel linkageLevel, AssignmentProcedures& procedures, const red::HashSet< ent::EntityID >& scheduledToDeleteStubs, const red::HashSet< ent::EntityID >& scheduledToDespawnEntities )
	{
		if ( result.IsSuccessful() )
		{
			assignment.m_entityId = result.GetEntityId();
			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Success;

			AssignSceneEntity( assignment, result.GetEntityId(), linkageLevel, std::move( procedures ), scheduledToDeleteStubs, scheduledToDespawnEntities );
		}
		else
		{
			RED_ASSERT( result.IsFailure() );

			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Fail;
			assignment.SetSceneEntity( nullptr );

			ReleaseSceneLocalEntity( assignment.m_assignmentId );

			if ( assignment.m_entityId.IsDefined() )
			{
				QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LifecycleNotice::assignmentFailed, assignment.m_assignmentId, assignment.m_entityId ) );
			}

			GAME_DATA_ERROR_BEGIN( false, Scenes, "%s", result.GetErrorMsg().Data() );
			GAME_DATA_ERROR_ACTION_SHOW_ASSET( assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString() );
			GAME_DATA_ERROR_END();
		}
	}

	void SceneEntitiesManager::AssignSceneEntity( SceneEntityAssignment& assignment, const ent::EntityID entityId, RequiredLinkageLevel linkageLevel, AssignmentProcedures procedures, const red::HashSet< ent::EntityID >& scheduledToDeleteStubs, const red::HashSet< ent::EntityID >& scheduledToDespawnEntities )
	{
		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& sceneEntities = accessToken.GetContents();
		auto foundEntity = sceneEntities.Find( entityId );
		if ( foundEntity == sceneEntities.End() )
		{
			SceneEntityCreation creationParams =
			{ 
				m_gameInstance, 
				m_forwardToEntityParams.m_animRequestGatherer, 
				CreateHandle< EntityItemsListener >( entityId, &m_entityEvents ),
				SpawnListener( entityId, m_entityEvents ),
				m_forwardToEntityParams.m_commandQueueCallback
			};

			auto sceneEntity = red::CreateUniquePtr< SceneEntity >( entityId, m_entityStateRepository.AccessEntityState( entityId ), creationParams );
			foundEntity = sceneEntities.Insert( entityId, std::move( sceneEntity ) ).Iterator();

			if ( entityId.IsStatic() )
			{
				m_gameInstance.GetScene().GetSystemRef< world::RuntimeSystemNodeStreaming >().SetNodeStreamingCallback( world::GlobalNodeID( entityId.GetHash() ), world::NodeStreamingListener::scene,
					[entityId, this]( const world::INodeInstance& nodeInstance, world::NodeStreamingNotice notice )
				{ 
					// Check if node type is correct. Sometimes, incorrect nodes are used. We need to handle this case in runtime.
					if ( THandle< world::EntityNodeInstance > entityNodeInstance = Cast< world::EntityNodeInstance >( HandleFromPtr( &nodeInstance ) ) )
					{
						auto accessToken = m_entityEvents.AcquireAccess();
						EntityEvents& events = accessToken.GetContents();
						switch( notice )
						{
						case world::NodeStreamingNotice::streamedIn:
							events.PushBack( { entityId, EntityEvent::Type::entityNodeStreamIn } );
							break;

						case world::NodeStreamingNotice::streamedOut:
							events.PushBack( { entityId, EntityEvent::Type::entityNodeStreamOut } );
							break;
						}
					}
				} );

				m_gameInstance.GetSystem< game::IObjectCarrySystem >().EnablePermanentState( entityId );
			}
			else
			{
				RED_VERIFY( m_entityRegistryCallbacks.Insert( entityId, red::CreateUniquePtr< EntityRegistryCallbackImpl> ( entityId, m_gameInstance, m_entityEvents ) ).IsSuccessful() );
			}
		}

		SceneEntity& sceneEntity = *foundEntity.Value();
		RED_ASSERT( sceneEntity.GetEntityID() == entityId );
		sceneEntity.AddAssignment( assignment.m_assignmentId, linkageLevel, std::move( procedures ) );

		assignment.SetSceneEntity( &sceneEntity );

		QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LifecycleNotice::assignmentCreated, assignment.m_assignmentId, assignment.m_entityId ) );

		// Do not link if entity is going to be despawned.
		if ( !scheduledToDeleteStubs.Exist( sceneEntity.GetEntityID() ) && !scheduledToDespawnEntities.Exist( sceneEntity.GetEntityID() ) )
		{
			TryLinking( sceneEntity );
		}
	}

	Bool SceneEntitiesManager::ProcessReleaseEntityRequests( const ReleaseEntityRequest& request )
	{
		// process dependent assignments
		{
			auto iter = std::remove_if( m_dependentEntityAssignments.Begin(), m_dependentEntityAssignments.End(), [ this, &request ]( DependentEntityAssignment& dependentAssignment )
			{
				// Check if dependent assignment owner is released. If yes, than all dependent assignments have to be released as well.
				if ( dependentAssignment.m_ownerPerformerId == request.m_performerId && dependentAssignment.m_sceneInstaceId == request.m_sceneInstanceId )
				{
					RED_ASSERT( dependentAssignment.m_sceneEntityAssignmentId != request.m_sceneEntityAssignmentId, "Should never happen" );

					auto accessToken = m_sceneEntityAssignments.AcquireAccess();
					SceneEntityAssignments& assignments = accessToken.GetContents();
					auto foundAssignment = assignments.Find( dependentAssignment.m_sceneEntityAssignmentId );
					if ( foundAssignment != assignments.End() )
					{
						AdjournDependentEntityAssignment( foundAssignment.Value() );
					}
				}

				// remove dependent assignment
				return dependentAssignment.m_sceneEntityAssignmentId == request.m_sceneEntityAssignmentId;
			} );

			m_dependentEntityAssignments.Remove( iter, m_dependentEntityAssignments.End() );
		}

		auto executeReleaseReq = [ this ](const ReleaseEntityRequest& request )
		{
			if ( request.m_entityId.IsDefined() )
			{
				ReleaseSceneEntity( request.m_sceneEntityAssignmentId, request.m_entityId );
			}
		};

		auto iter = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [ &request ]( const LocalEntity& le ) { return le.m_sceneEntityAssignmentId == request.m_sceneEntityAssignmentId; } );
		if ( iter != m_localEntities.End() )
		{
			const ent::EntityID entityId = iter->m_entityId;
			const SceneEntitySpawnId spawnId = iter->m_spawnId;
			const Bool isItemRecord = game::data::IsOfType<game::data::Item_Record>( iter->m_tweakRecordId );

			executeReleaseReq( request );
			m_localEntities.Remove( iter );
		}
		else
		{
			executeReleaseReq( request );
		}

		return true;
	}

	void SceneEntitiesManager::ReleaseSceneEntity( const SceneEntityAssignmentId sceneEntityAssignmentId, const ent::EntityID entityId )
	{
		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& sceneEntities = accessToken.GetContents();
		auto foundEntity = sceneEntities.Find( entityId );
		if ( foundEntity != sceneEntities.End() )
		{
			SceneEntity& sceneEntity = *foundEntity.Value();
			sceneEntity.RemoveAssignment( sceneEntityAssignmentId );

			if ( const Bool toDispose = !sceneEntity.HasValidAssignment() )
			{
				UnlinkBody( sceneEntity );
				UnlinkStub( sceneEntity );
				sceneEntities.Remove( foundEntity );

				QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LifecycleNotice::assignmentDestroyed, sceneEntityAssignmentId, entityId ) );

				if ( entityId.IsStatic() )
				{
					m_pendingStaticEntitiesToLink.Remove( entityId );
					m_gameInstance.GetScene().GetSystemRef< world::RuntimeSystemNodeStreaming >().SetNodeStreamingCallback( world::GlobalNodeID( entityId.GetHash() ), world::NodeStreamingListener::scene, nullptr );
					m_gameInstance.GetSystem< game::IObjectCarrySystem >().DisablePermanentState( entityId );
				}
				else
				{
					RED_VERIFY( m_entityRegistryCallbacks.Remove( entityId ).IsSuccessful() );
				}

				// as the last, remove entity state
				m_entityStateRepository.RemoveEntityState( entityId );
			}
		}

		ReleaseSceneLocalEntity( sceneEntityAssignmentId );
	}

	void SceneEntitiesManager::ReleaseSceneLocalEntity( const SceneEntityAssignmentId sceneEntityAssignmentId )
	{
		// NOTE: Multiple scene local entities have same spawn Id.
		// Check how many entities use same spawn id and despawn only if nothing except removed entity uses it.
		scn::SceneEntitySpawnId releasedSpawnId = SceneEntitySpawnId::invalid;

		ent::EntityID foundEntityId;
		Bool keepAlive = false;

		auto iter = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [ sceneEntityAssignmentId ]( const LocalEntity& le ) { return le.m_sceneEntityAssignmentId == sceneEntityAssignmentId; } );
		if ( iter != m_localEntities.End() )
		{
			LocalEntity& le = *iter;
			releasedSpawnId = le.m_spawnId;
			le.m_spawnId = SceneEntitySpawnId::invalid;

			foundEntityId = le.m_entityId;
			keepAlive = le.m_keepAlive;
		}

		if ( releasedSpawnId != SceneEntitySpawnId::invalid )
		{
			const Bool noRef = std::none_of( m_localEntities.Begin(), m_localEntities.End(), [ releasedSpawnId ]( const LocalEntity& le ) { return le.m_spawnId == releasedSpawnId; } );

			if ( noRef )
			{
				if ( !keepAlive )
				{
					m_sceneEntitySpawnService.Dispose( releasedSpawnId );

					if ( foundEntityId.IsDefined() )
					{
						m_entityStateRepository.OnEntityDeactiveted( foundEntityId );
					}
				}
			}
		}
	}

	Bool SceneEntitiesManager::TrySpawnSceneLocalEntity( red::StringView performerName, const SpawnDespawnEntityParams& params, SceneEntityAssignment& assignment, AssignmentProcedures procedures, ent::EntitySpawnPriority spawnPriority )
	{
		const game::data::RecordID tweakRecordId = params.GetSpecRecordId();

		if ( tweakRecordId == SpecialNames::RecordIdNone() || game::data::GetRecord< game::data::TweakDBRecord >( tweakRecordId ).Expired() )
		{
			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Fail;

			GAME_DATA_ERROR_BEGIN( false, Scenes, String::Printf( "Invalid TweakDB record '%hs' for item %hs", tweakRecordId.ToString().AsChar(),  performerName.Data() ).AsChar() );
			GAME_DATA_ERROR_ACTION_SHOW_ASSET( assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString() );
			GAME_DATA_ERROR_END();
			return true;
		}

		if ( !params.IsEnabled() )
		{
			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Fail;
			return true;
		}

		const Bool isItemRecord = game::data::IsOfType<game::data::Item_Record>( tweakRecordId );

		if ( !isItemRecord && params.m_dynamicEntityUniqueName.Empty() )
		{
			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Fail;

			GAME_DATA_ERROR_BEGIN( false, Scenes, String::Printf( "Invalid dynamic entity unique name for entity %hs", performerName.Data() ).AsChar() );
			GAME_DATA_ERROR_ACTION_SHOW_ASSET( assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString() );
			GAME_DATA_ERROR_END();
			return true;
		}

		if ( isItemRecord && params.GetItemOwnerId() == PerformerId::invalid )
		{
			assignment.m_assignmentStatus = SceneEntityAssignmentStatus::Fail;

			GAME_DATA_ERROR_BEGIN( false, Scenes, String::Printf( "Invalid performer owner id for item %hs", performerName.Data() ).AsChar() );
			GAME_DATA_ERROR_ACTION_SHOW_ASSET( assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString() );
			GAME_DATA_ERROR_END();

			return true;
		}

		assignment.m_assignmentStatus = SceneEntityAssignmentStatus::InProgress;

		WorldTransform nodeTransform;
		assignment.GetSceneInstance().GetLocAuthority().GetTransformWS( params.GetSpawnMarker(), nodeTransform );

		Transform finalTranform = params.GetSpawnOffset() * nodeTransform._ToXForm();

		if ( isItemRecord )
		{
			finalTranform.SetOrientation( Quaternion::IDENTITY() );
		}
		else
		{
			assignment.m_entityId = m_gameInstance.GetSystem< game::IDynamicEntityIDSystem >().AcquireEntityIDByUniqueName( params.m_dynamicEntityUniqueName );
		}

		Bool prefetchAppearance = false;
		if( params.IsPrefetchAppearance() )
		{
			prefetchAppearance = true;
		}
		else if( params.IsAlwaysSpawned()  && params.GetForceMaxVisibility() )
		{
			prefetchAppearance = true;
		}

		LocalEntity localEntity =
		{
			assignment.m_entityId,
			tweakRecordId,
			params.GetAppearance(),
			finalTranform,
			performerName.ToString(),
			std::move( procedures ),
			assignment.m_assignmentId,
			spawnPriority,
			params.IsAlwaysSpawned(),
			params.IsKeepAlive(),
			prefetchAppearance,
			SceneEntitySpawnId(),
			EngineTime::GetNow(),
			EngineTime::GetNow()
		};

#ifndef RED_CONFIGURATION_FINAL
		//For now we want to validate only actors
		if (params.IsValidateSpawnPostion() && game::data::IsOfType< game::data::Character_Record >(localEntity.m_tweakRecordId))
		{
			world::RuntimeSystemNavigation* navigationSystem = m_gameInstance.GetScene().GetSystem< world::RuntimeSystemNavigation >();
			if (!AI::runtimeUtils::TestLocation(*navigationSystem, localEntity.m_transform.GetPosition()))
			{
				GAME_DATA_ERROR_BEGIN(false, Scenes, String::Printf("Actor %hs (X=%f, Y=%f, Z=%f) not spawned on navmesh. Scene %hs",
					performerName.Data(),
					localEntity.m_transform.GetPosition3().X, localEntity.m_transform.GetPosition3().Y, localEntity.m_transform.GetPosition3().Z,
					assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString()).AsChar());
				GAME_DATA_ERROR_ACTION_SHOW_ASSET(assignment.GetSceneInstance().GetSceneId().GetResourcePath().ToDebugString());
				GAME_DATA_ERROR_END();
			}
		}
#endif

		m_localEntities.EmplaceBack( std::move( localEntity ) );

		if ( isItemRecord )
		{
			DependentEntityAssignment& depAssignment = m_dependentEntityAssignments.EmplaceBack();
			depAssignment.m_performerName = performerName.ToString();
			depAssignment.m_plan = DependentEntityAssignment::Plan::spawnDespawnItem;
			depAssignment.m_params = rtti::Variant( prv::SpawnDespawnItem { tweakRecordId, localEntity.m_transform } );
			depAssignment.m_sceneEntityAssignmentId = assignment.m_assignmentId;
			depAssignment.m_sceneInstaceId = assignment.GetSceneInstance().GetSceneInstanceId();
			depAssignment.m_ownerPerformerId = params.GetItemOwnerId();
			depAssignment.m_procedures = std::move( procedures );

			const SceneEntityAssignmentId sceneEntityAssignmentId = assignment.GetSceneInstance().GetPerformerManager().GetPerformerAssignmentId( depAssignment.m_ownerPerformerId );
			if ( sceneEntityAssignmentId != SceneEntityAssignmentId::invalid )
			{
				m_dependentAssignmentsChange.Insert( sceneEntityAssignmentId );
			}
		}
		else
		{
			// Entity ID is not defined for items. For props & actors it is.
			if ( assignment.m_entityId.IsDefined() )
			{
				if ( IsSceneLocalEntityActive( assignment.m_entityId ) )
				{
					RED_VERIFY( SpawnSceneLocalEntity( m_localEntities.Back(), false, true, false ) );
				}
				else if ( params.IsSpawnedOnStart() )
				{
					if ( SpawnSceneLocalEntity( m_localEntities.Back(), params.IsFindInWorld(), false, false ) )
					{
						m_entityStateRepository.OnEntityActiveted( assignment.m_entityId );
					}
				}
			}
		}

		return true;
	}

	Bool SceneEntitiesManager::SpawnSceneLocalEntity( LocalEntity& localEntity, Bool findInWorld, Bool isRestoration, Bool useCustomTransform, WorldTransform customTransform )
	{
		const Transform transform = useCustomTransform ? customTransform._ToXForm() : localEntity.m_transform;
		if ( game::data::IsOfType< game::data::Character_Record >( localEntity.m_tweakRecordId ) )
		{
			if ( isRestoration && m_sceneEntitySpawnService.IsEntitySavable( localEntity.m_entityId ) )
			{
				localEntity.m_spawnId = m_sceneEntitySpawnService.RestoreCharacterEntity( localEntity.m_tweakRecordId, localEntity.m_entityId, localEntity.m_isAlwaysSpawned, localEntity.m_prefetchAppearance, localEntity.m_appearanceName );
			}
			else
			{
				localEntity.m_spawnId = m_sceneEntitySpawnService.SpawnCharacterEntity( localEntity.m_tweakRecordId, localEntity.m_entityId, localEntity.m_isAlwaysSpawned, localEntity.m_prefetchAppearance, localEntity.m_appearanceName, transform );
			}

			m_localEntitiesSpawning.EmplaceBack( LocalEntitySpawning { localEntity.m_sceneEntityAssignmentId } );

			return true;
		}
		else if ( game::data::IsOfType<game::data::Prop_Record>( localEntity.m_tweakRecordId ) )
		{
			if ( m_gameInstance.GetSystem< game::IObjectCarrySystem >().IsObjectSpawner( localEntity.m_entityId ) )
			{
				if ( !isRestoration && !findInWorld )
				{
					RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Scenes, "Expecting prop '%hs' not to exist in the world but it was found", localEntity.m_performerName.AsChar() );
				}

				localEntity.m_spawnId = m_sceneEntitySpawnService.RestorePropEntity( localEntity.m_entityId, localEntity.m_isAlwaysSpawned );
			}
			else
			{
				if ( findInWorld )
				{
					RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Scenes, "Expecting prop '%hs' to exist in the world but it can't be find", localEntity.m_performerName.AsChar() );
				}
			
				if ( isRestoration )
				{
					RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Scenes, "Expecting prop '%hs' restoration but it doesn't exist", localEntity.m_performerName.AsChar() );
				}
		
				localEntity.m_spawnId = m_sceneEntitySpawnService.SpawnPropEntity( localEntity.m_tweakRecordId, localEntity.m_entityId, localEntity.m_isAlwaysSpawned, localEntity.m_appearanceName, transform, localEntity.m_spawnPriority );
			}

			m_localEntitiesSpawning.EmplaceBack( LocalEntitySpawning { localEntity.m_sceneEntityAssignmentId } );

			return true;
		}

		return false;
	}

	Bool SceneEntitiesManager::ProcessLocalEntitiesSpawn( const LocalEntitySpawning& les, const red::HashSet< ent::EntityID >& scheduledToDeleteStubs, const red::HashSet< ent::EntityID >& scheduledToDespawnEntities )
	{
		auto accessToken = m_sceneEntityAssignments.AcquireAccess();
		SceneEntityAssignments& assignments = accessToken.GetContents();
		auto foundAssignment = assignments.Find( les.m_sceneEntityAssignmentId );
		if ( foundAssignment == assignments.End() )
		{
			// assignment already released 
			ReleaseSceneLocalEntity( les.m_sceneEntityAssignmentId );
			return true;
		}

		SceneEntityAssignment& assignment = foundAssignment.Value();
		RED_ASSERT ( assignment.m_assignmentStatus == SceneEntityAssignmentStatus::InProgress );

		// If it's dependent assignment check if owner is still embodied. Fail the process otherwise.
		if ( les.m_ownerEntityAssignmentId != SceneEntityAssignmentId::invalid )
		{
			Bool ownerBodyLinked = false;
			auto foundOwnerAssignment = assignments.Find( les.m_ownerEntityAssignmentId );
			if ( foundOwnerAssignment != assignments.End() )
			{
				const SceneEntityAssignment& ownerAssignment = foundOwnerAssignment.Value();
				const SceneEntity* ownerEntity = ownerAssignment.GetSceneEntity();
				ownerBodyLinked = ( ownerEntity && ownerEntity->IsBodyLinked() );
			}

			// If owner is not embodied we can't make proper assignment.
			if ( !ownerBodyLinked ) 
			{
				AdjournDependentEntityAssignment( assignment );
				return true;
			}
		}

		auto entIter = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [ &les ]( const LocalEntity& le ) { return les.m_sceneEntityAssignmentId == le.m_sceneEntityAssignmentId; } );
		RED_ASSERT( entIter != m_localEntities.End(), "Expecting existing params" );
		const LocalEntity& localEntity = *entIter;
		const resolver::ResolveResult result = resolver::ResolveEntitySpawnDespawn( localEntity.m_performerName, localEntity.m_spawnId, m_sceneEntitySpawnService );

		if ( !result.IsResolved() )
		{
			return false;
		}

		AssignmentProcedures procedures = CloneProcedures( localEntity.m_procedures );
		FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
		return true;
	}

	Bool SceneEntitiesManager::ProcessLocalEntityRequest( const SceneLocalEntityRequest& ler, Bool isLoadingSave )
	{
		auto accessToken = m_sceneEntityAssignments.AcquireAccess();
		SceneEntityAssignments& assignments = accessToken.GetContents();

		switch ( ler.m_type )
		{
			case SceneLocalEntityRequest::Type::activate:
			{
				Bool processed = false;
				Bool activated = false;

				if ( !m_entityStateRepository.IsSceneLocalEntityActive( ler.m_entityId ) )
				{
					for ( LocalEntity& le : m_localEntities )
					{
						if ( le.m_entityId == ler.m_entityId )
						{
							auto foundAssignment = assignments.Find( le.m_sceneEntityAssignmentId );
							if ( foundAssignment != assignments.End() )
							{
								processed = true;

								const SceneEntityAssignment& assignment = foundAssignment.Value();
								if ( assignment.m_assignmentStatus == SceneEntityAssignmentStatus::InProgress )
								{
									const Bool isSpawning = std::any_of( m_localEntitiesSpawning.Begin(), m_localEntitiesSpawning.End(), [ &le ]( const LocalEntitySpawning& les ) { return le.m_sceneEntityAssignmentId == les.m_sceneEntityAssignmentId; } );
									if ( !isSpawning )
									{
										activated |= SpawnSceneLocalEntity( le, false, false, ler.m_isCustomTransform, ler.m_customTransform );
									}
								}
							}
						}
					}
				}
				else
				{
					processed = true;
				}

				if ( activated )
				{
					m_entityStateRepository.OnEntityActiveted( ler.m_entityId );
				}

				// While save is loading do not remove requests. Assignments may not be requested yet.
				return isLoadingSave ? processed : true;
			}

			case SceneLocalEntityRequest::Type::deactivate:
			{
				Bool processed = false;

				if ( m_entityStateRepository.IsSceneLocalEntityActive( ler.m_entityId ) )
				{
					for ( LocalEntity& le : m_localEntities )
					{
						if ( le.m_entityId == ler.m_entityId )
						{
							auto foundAssignment = assignments.Find( le.m_sceneEntityAssignmentId );
							if ( foundAssignment != assignments.End() )
							{
								SceneEntityAssignment& assignment = foundAssignment.Value();

								switch ( assignment.m_assignmentStatus )
								{
									case SceneEntityAssignmentStatus::Fail:
									{
										break;
									}

									case SceneEntityAssignmentStatus::InProgress:
									{
										// stop spawning procedure
										auto iter = std::find_if( m_localEntitiesSpawning.Begin(), m_localEntitiesSpawning.End(), [ &le ]( const LocalEntitySpawning& les ) { return le.m_sceneEntityAssignmentId == les.m_sceneEntityAssignmentId; } );
										if ( iter != m_localEntitiesSpawning.End() )
										{
											ReleaseSceneLocalEntity( le.m_sceneEntityAssignmentId );
											m_localEntitiesSpawning.RemoveReorder( iter );
										}

										break;
									}

									case SceneEntityAssignmentStatus::Success:
									{
										ReleaseSceneEntity( le.m_sceneEntityAssignmentId, ler.m_entityId );
										assignment.m_entityId = ent::EntityID();
										assignment.SetSceneEntity( nullptr );
										break;
									}
								}

								assignment.m_assignmentStatus = SceneEntityAssignmentStatus::InProgress;
							}

							processed = true;
						}
					}
				}
				else
				{
					processed = true;
				}

				// While save is loading do not remove requests. Assignments may not be requested yet.
				return isLoadingSave ? processed : true;
			}
		}

		return true;
	}

	// Embodies all entities.
	// Expect that entity may not actually be embodied afterwards.
	void SceneEntitiesManager::EmbodyEntities()
	{
		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& entities = accessToken.GetContents();

		for ( auto it : entities )
		{
			SceneEntity& se = *it.Value();

			if ( se.IsBodyLinked() && se.LinksValidEntity() )
			{
				se.Embody();
			}
			else if ( se.IsStubLinked() )
			{
				se.Stubody();
			}
		}
	}

	// Disembodies all entities.
	// Entities that are already disembodied, stay disembodied.
	void SceneEntitiesManager::DisembodyEntities()
	{
		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& entities = accessToken.GetContents();

		for ( auto it : entities )
		{
			SceneEntity& sceneEntity = *it.Value();

			switch( sceneEntity.GetEmbodiment() )
			{
				case Embodiment::embodied:
				{
					sceneEntity.Disembody();

					break;
				}

				case Embodiment::stubodied:
				{
					sceneEntity.Disstubody();

					break;
				}

				case Embodiment::disembodied:
				{
					break;
				}
			}
		}
	}

	void SceneEntitiesManager::TryLinking( SceneEntity& se )
	{
		LinkBody( se );

		if ( !se.IsBodyLinked() )
		{
			LinkStub( se );
		}
	}

	void SceneEntitiesManager::LinkBody( SceneEntity& se )
	{
		if ( !se.IsBodyLinked() )
		{
			RED_ASSERT( se.GetEntityID().IsDefined(), "Expecting valid entity id" );

			if ( ent::EntityPtr body = resolver::FindEntityByID( se.GetEntityID(), m_gameInstance, false ) )
			{
				if ( body->IsAttached() )
				{
					auto found = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [body]( const LocalEntity & localEntity ){ return localEntity.m_entityId == body->GetEntityID(); } );
					if( found != m_localEntities.End() )
					{
						if( found->m_startSpawn == found->m_endSpawn )
						{
							found->m_endSpawn = EngineTime::GetNow();
						}
					}

					UnlinkStub( se );

					se.LinkBody( body );
					se.IterateAssignments( [ this, &se ]( const SceneEntityAssignmentId assignmentId )
					{
						QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LinkageNotice::bodyLinked, assignmentId, se.GetEntityID() ) );
						m_dependentAssignmentsChange.Insert( assignmentId );
					} );
				}
				else
				{
					if ( se.GetEntityID().IsStatic() )
					{
						m_pendingStaticEntitiesToLink.Insert( se.GetEntityID() );
					}
				}
			}
		}
	}

	void SceneEntitiesManager::LinkStub( SceneEntity& se )
	{
		if ( !se.IsStubLinked() && se.HasFullLinkageAssignment() )
		{
			RED_ASSERT( se.GetEntityID().IsDefined(), "Expecting valid entity id" );

			if ( game::EntityStub* stub = resolver::FindEntityStubByID( se.GetEntityID(), m_gameInstance ) )
			{
				se.LinkStub( *stub );
				se.IterateAssignments( [ this, &se ]( const SceneEntityAssignmentId assignmentId )
				{
					QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LinkageNotice::stubLinked, assignmentId, se.GetEntityID() ) );
				} );
			}
		}
	}

	void SceneEntitiesManager::PutrefyBody( SceneEntity& se )
	{
		if ( se.IsBodyLinked() )
		{
			se.IterateAssignments( [ this, &se ]( const SceneEntityAssignmentId assignmentId )
			{
				QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LinkageNotice::bodyPutrefied, assignmentId, se.GetEntityID() ) );
				m_dependentAssignmentsChange.Insert( assignmentId );
			} );

			se.PutrefyBody();
		}
	}

	void SceneEntitiesManager::UnlinkStub( SceneEntity& se )
	{
		if ( se.IsStubLinked() )
		{
			se.IterateAssignments( [ this, &se ]( const SceneEntityAssignmentId assignmentId )
			{
				QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LinkageNotice::stubUnlinked, assignmentId, se.GetEntityID() ) );
			} );

			se.UnlinkStub();
		}
	}

	void SceneEntitiesManager::UnlinkBody( SceneEntity& se )
	{
		if ( se.IsBodyLinked() )
		{
			se.IterateAssignments( [ this, &se ]( const SceneEntityAssignmentId assignmentId )
			{
				QueueSceneEntityNotification( SceneEntityNotification( SceneEntityNotification::LinkageNotice::bodyUnlinked, assignmentId, se.GetEntityID() ) );
				m_dependentAssignmentsChange.Insert( assignmentId );
			} );

			se.UnlinkBody();
		}
	}

	void SceneEntitiesManager::LinkPendingStaticEntities()
	{
		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& sceneEntities = accessToken.GetContents();

		// Link pending entities.
		for ( auto iter = m_pendingStaticEntitiesToLink.Begin(); iter != m_pendingStaticEntitiesToLink.End(); )
		{
			auto foundEntity = sceneEntities.Find( *iter );
			RED_ASSERT( foundEntity != sceneEntities.End() );
			LinkBody( *foundEntity.Value() );

			if ( foundEntity.Value()->IsBodyLinked() )
			{
				iter = m_pendingStaticEntitiesToLink.Remove( iter ).Iterator();
			}
			else
			{
				++iter;
			}
		}
	}

	void SceneEntitiesManager::ProcessDependancyAssignments( const red::HashSet< ent::EntityID >& scheduledToDeleteStubs, const red::HashSet< ent::EntityID >& scheduledToDespawnEntities )
	{
		auto accessToken = m_sceneEntityAssignments.AcquireSharedAccess();
		SceneEntityAssignments& assignments = accessToken.GetContents();

		// We need to swap m_dependentAssignmentsChange to a temporary, because it can be invalidated while iterating over it
		red::HashSet< SceneEntityAssignmentId > dependentAssignmentsChange{ std::move( m_dependentAssignmentsChange ) };
		m_dependentAssignmentsChange.Clear();

		for ( const SceneEntityAssignmentId sceneEntityAssignmentId : dependentAssignmentsChange )
		{
			auto foundOwnerAssignment = assignments.Find( sceneEntityAssignmentId );
			if ( foundOwnerAssignment == assignments.End() )
			{
				continue; // This case is handled when assignment is released. All dependent assignments are adjourned immediately
			}

			const SceneEntityAssignment& ownerAssignment = foundOwnerAssignment.Value();
			for ( const DependentEntityAssignment& dependentAssignment : m_dependentEntityAssignments )
			{
				if ( dependentAssignment.m_ownerPerformerId == ownerAssignment.GetPerformerId() && dependentAssignment.m_sceneInstaceId == ownerAssignment.GetSceneInstanceId() )
				{
					auto foundAssignment = assignments.Find( dependentAssignment.m_sceneEntityAssignmentId );
					RED_ASSERT( foundAssignment != assignments.End(), "Expecting assignment to exist" );
					SceneEntityAssignment&  assignment = foundAssignment.Value();

					switch ( assignment.m_assignmentStatus )
					{
						case SceneEntityAssignmentStatus::InProgress:
						{
							// Case when assignment is being made. Owner assignment is ready. If body is already linked, we can make dependent assignment.
							if ( ownerAssignment.m_assignmentStatus == SceneEntityAssignmentStatus::Success )
							{
								const SceneEntity* ownerEntity = ownerAssignment.GetSceneEntity();
								RED_ASSERT( ownerEntity, "Owner should exist. " );

								// If body is linked.
								if ( ownerEntity->IsBodyLinked() )
								{
									switch ( dependentAssignment.m_plan )
									{
										case DependentEntityAssignment::Plan::findInEntity:
										{
											resolver::ResolveResult result = resolver::ResolveEntityFindInEntity( dependentAssignment.m_performerName, dependentAssignment.m_params.Get< FindEntityInEntityParams >(), ownerEntity->GetLinkedEntity(), m_gameInstance );
											if ( result.IsResolved() )
											{
												AssignmentProcedures procedures = CloneProcedures( dependentAssignment.m_procedures );
												FinalizeEntityAssignment( assignment, result, RequiredLinkageLevel::full, procedures, scheduledToDeleteStubs, scheduledToDespawnEntities );
											}

											break;
										}

										case DependentEntityAssignment::Plan::spawnDespawnItem:
										{
											if ( std::none_of( m_localEntitiesSpawning.Begin(), m_localEntitiesSpawning.End(), [&ownerAssignment, assignmentId = dependentAssignment.m_sceneEntityAssignmentId]( const LocalEntitySpawning& les ){ return les.m_sceneEntityAssignmentId == assignmentId; } ) )
											{
												auto iter = std::find_if( m_localEntities.Begin(), m_localEntities.End(), [assignmentId = dependentAssignment.m_sceneEntityAssignmentId]( const LocalEntity& le ) { return le.m_sceneEntityAssignmentId == assignmentId; } );
												RED_ASSERT( iter != m_localEntities.End() );

												LocalEntity& localEntity = *iter;
												const prv::SpawnDespawnItem& params = dependentAssignment.m_params.Get< prv::SpawnDespawnItem >();
												localEntity.m_spawnId = m_sceneEntitySpawnService.SpawnItemEntity( params.m_recordID, *ownerEntity->GetLinkedEntity(), params.m_finalTransform );

												m_localEntitiesSpawning.EmplaceBack( LocalEntitySpawning{ dependentAssignment.m_sceneEntityAssignmentId, ownerAssignment.m_assignmentId } );
											}

											break;
										}

										default:
											RED_FATAL( "Not supported plan" );
									}
								}
							}

							break;
						}
						case SceneEntityAssignmentStatus::Success:
						{
							Bool breakAssignment = ( ownerAssignment.m_assignmentStatus != SceneEntityAssignmentStatus::Success );
							if ( !breakAssignment )
							{
								// Owner assignment is success. But we may have only stub
								const SceneEntity* ownerEntity = ownerAssignment.GetSceneEntity();
								RED_ASSERT( ownerEntity, "Owner should exist " );

								// If body is not linked, we have to break assignment
								breakAssignment = !ownerEntity->IsBodyLinked();
							}

							// Break assignment.
							if ( breakAssignment )
							{
								AdjournDependentEntityAssignment( assignment );
							}

							break;
						}
						case SceneEntityAssignmentStatus::Fail:
						{
							// do nothing
							break;
						}
					}
				}
			}
		}
	}

	void SceneEntitiesManager::AdjournDependentEntityAssignment( SceneEntityAssignment& assignment )
	{
		switch ( assignment.m_assignmentStatus )
		{
			case SceneEntityAssignmentStatus::Success:
				ReleaseSceneEntity( assignment.m_assignmentId, assignment.m_entityId );
				assignment.m_assignmentStatus = SceneEntityAssignmentStatus::InProgress;
				assignment.m_entityId = ent::EntityID();
				assignment.SetSceneEntity( nullptr );
				break;

			case SceneEntityAssignmentStatus::InProgress:
				ReleaseSceneLocalEntity( assignment.m_assignmentId );
				assignment.m_assignmentStatus = SceneEntityAssignmentStatus::InProgress;
				assignment.m_entityId = ent::EntityID();
				assignment.SetSceneEntity( nullptr );
				break;

			case SceneEntityAssignmentStatus::Fail:
				break;
		}
	}

	void SceneEntitiesManager::ProcessEntityEvents( red::HashSet< ent::EntityID >& outScheduledToDeleteStubs, red::HashSet< ent::EntityID >& outScheduledToDespawnEntities )
	{
		EntityEvents allEvents { PoolFrameSceneSystem() };
		{
			auto accessToken = m_entityEvents.AcquireAccess();
			EntityEvents& events = accessToken.GetContents();
			allEvents.PushBack( std::move( events ) );
			events.Clear();

			if ( events.Capacity() > ( allEvents.Size() + ( allEvents.Size() / 2 ) ) )
			{
				events.Shrink();
			}
		}

		auto accessToken = m_sceneEntities.AcquireAccess();
		SceneEntities& sceneEntities = accessToken.GetContents();

		for ( const EntityEvent& e : allEvents )
		{
			switch ( e.m_type )
			{
				case EntityEvent::Type::attach:
				{
					auto foundEntity = sceneEntities.Find( e.m_entityId );
					if ( foundEntity != sceneEntities.End() )
					{
						TryLinking( *foundEntity.Value() );
					}

					outScheduledToDespawnEntities.Remove( e.m_entityId );
					break;
				}

				case EntityEvent::Type::detach:
				{
					auto foundEntity = sceneEntities.Find( e.m_entityId );
					if ( foundEntity != sceneEntities.End() )
					{
						PutrefyBody( *foundEntity.Value() );

						if ( !outScheduledToDeleteStubs.Exist( e.m_entityId ) )
						{
							LinkStub( *foundEntity.Value() );
						}
					}

					outScheduledToDespawnEntities.Insert( e.m_entityId );
					break;
				}

				case EntityEvent::Type::stubCreation:
				{
					auto accessToken = m_assignEntityRequests.AcquireAccess();
					AssignEntityRequests& requests = accessToken.GetContents();

					requests.Reserve( requests.Size() + m_pendingAssignEntityRequests.Size() );

					for ( AssignEntityRequest& request : m_pendingAssignEntityRequests )
					{
						requests.PushBack( std::move( request ) );
					}

					m_pendingAssignEntityRequests.Clear();

					outScheduledToDeleteStubs.Remove( e.m_entityId );
					break;
				}

				case EntityEvent::Type::stubDeletion:
				{
					auto foundEntity = sceneEntities.Find( e.m_entityId );
					if ( foundEntity != sceneEntities.End() )
					{
						UnlinkStub( *foundEntity.Value() );
					}

					outScheduledToDeleteStubs.Insert( e.m_entityId );
					break;
				}

				// notification that item was equipped/unequipped by some object
				case EntityEvent::Type::itemEquipped:
				case EntityEvent::Type::itemUnequipped:
				{
					auto foundEntity = sceneEntities.Find( e.m_entityId );
					if ( foundEntity != sceneEntities.End() )
					{
						foundEntity.Value()->IterateAssignments( [ this ]( const SceneEntityAssignmentId assignmentId )
						{
							m_dependentAssignmentsChange.Insert( assignmentId );
						} );
					}

					break;
				}

				case EntityEvent::Type::entityNodeStreamIn:
				{
					m_pendingStaticEntitiesToLink.Insert( e.m_entityId );
					outScheduledToDespawnEntities.Remove( e.m_entityId );
					break;
				}

				case EntityEvent::Type::entityNodeStreamOut:
				{
					m_pendingStaticEntitiesToLink.Remove( e.m_entityId );
					outScheduledToDespawnEntities.Insert( e.m_entityId );

					auto foundEntity = sceneEntities.Find( e.m_entityId );
					RED_ASSERT ( foundEntity != sceneEntities.End(), "Scene entity should exist!" );
					UnlinkBody( *foundEntity.Value() );
					break;
				}
			}
		}
	}

	void SceneEntitiesManager::QueueSceneEntityNotification( SceneEntityNotification notification )
	{
		m_sceneEntityNotificationsQueue.PushBack( std::move( notification ) );
	}

	void SceneEntitiesManager::PublishSceneEntityNotifications()
	{
		if ( !m_sceneEntityNotificationsQueue.Empty() )
		{
			auto token = m_sceneEntityListeners.AcquireSharedAccess();

			for ( ISceneEntitiesListener* listener : token.GetContents() )
			{
				listener->OnNotificationsPublished( m_sceneEntityNotificationsQueue );
			}

			Bool shrinkBuffer = ( m_sceneEntityNotificationsQueue.Capacity() > ( m_sceneEntityNotificationsQueue.Size() + ( m_sceneEntityNotificationsQueue.Size() / 2 ) ) );

			m_sceneEntityNotificationsQueue.Clear();
			
			if ( shrinkBuffer )
			{
				m_sceneEntityNotificationsQueue.Shrink();
			}
		}
	}

} // namespace scn


RTTI_BEGIN_TYPE_IN_NAMESPACE( EntityItemsListener, scn );
RTTI_PARENT_TYPE( game::IAttachmentSlotsListener );
RTTI_END_TYPE();
