#include "build.h"
#include "targetingLookAtHelper.h"

#include "hitRepresentationComponent.h"
#include "targetingSystemEvents.h"

#include "../../gameCore/include/gameInstance.h"
#include "../../gameCore/include/hitRepresentationSystemInterface.h"
#include "../../physics//include/physicsMaterialLibrary.h"
#include "../../physics/include/physXNatives.h"
#include "../../world/include/cameraSystemInterface.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../worldEntities/include/runtimeEntityRegistry.h"
#include "../../worldPhysics/include/runtimeSystemPhysics.h"


#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",off)
#endif

#include <cstdio>
#include <cmath>
extern volatile int g_coreplayLookAtProbe; // [coreplay] defined in scriptableSystemsContainer.cpp

namespace game
{
	using namespace targetingSystem;

	//-----------------------------------------------------------------------------------
	enum EProcessHitFlag : Uint8
	{
		None = RED_FLAG( 0 ),
		Valid = RED_FLAG( 1 ),
		HasHitRepresentation = RED_FLAG( 2 )
	};

	constexpr EProcessHitFlag operator|( EProcessHitFlag lhs, EProcessHitFlag rhs )
	{
		return static_cast< EProcessHitFlag >( static_cast< Uint8 >( lhs ) | rhs );
	}

	constexpr EProcessHitFlag& operator|=( EProcessHitFlag& lhs, EProcessHitFlag rhs )
	{
		lhs = lhs | rhs;
		return lhs;
	}

	constexpr EProcessHitFlag operator&( EProcessHitFlag lhs, EProcessHitFlag rhs )
	{
		return static_cast< EProcessHitFlag >( static_cast< Uint8 >( lhs ) & rhs );
	}

	constexpr EProcessHitFlag& operator&=( EProcessHitFlag& lhs, EProcessHitFlag rhs )
	{
		lhs = lhs & rhs;
		return lhs;
	}

	EProcessHitFlag ConstructHitInfo( const physics::TraceResult& traceResult, const THandle< ent::Entity >& hitEntity, const game::IHitRepresentationSystem& hitRepresentationSystem, const Vector3& queryStartPoint, const Vector3& queryEndPoint, HitInfo& outHitInfo )
	{
		EProcessHitFlag outFlag = EProcessHitFlag::Valid;

		THandle< ent::IComponent > hitComponent = traceResult.FindHitObject< ent::IComponent >();
		Vector3 closestHitPosition = traceResult.m_worldPosition;
		outHitInfo.m_physicalMaterial = traceResult.m_material;
		outHitInfo.m_isTransparent = physics::GetMaterialLibrary().GetMaterialTags( traceResult.m_material ).m_playerVisibility != physics::MaterialTag::Visibility::None;

		if( hitEntity )
		{
			THandle< HitRepresentationComponent > hitRepresentationComponent = hitRepresentationSystem.GetHitRepresentationComponent( hitEntity->GetEntityID() ).ToHandle();
			if( hitRepresentationComponent )
			{
				outFlag |= EProcessHitFlag::HasHitRepresentation;

				if( hitRepresentationComponent->HasRepresentations() )
				{
					QueryResult queryResult;
					if( hitRepresentationComponent->QueryRaycast( queryStartPoint, queryEndPoint, queryResult ) && queryResult.m_hitShapes.Size() )
					{
						const ShapeData& hitShapeData = *queryResult.m_hitShapes.Begin();
						const Vector3& hitShapePosition = hitShapeData.m_result.m_hitPositionEnter;
						closestHitPosition = hitShapePosition;
						hitComponent = hitRepresentationComponent;
					}
					else
					{
						outFlag &= static_cast< EProcessHitFlag >( ~EProcessHitFlag::Valid );
					}
				}
			}
		}

		if( outFlag & EProcessHitFlag::Valid )
		{
			outHitInfo.m_queryMask = traceResult.m_shapeQueryMask;
			outHitInfo.m_entityId = hitEntity ? hitEntity->GetEntityID() : ent::EntityID{};
			outHitInfo.m_entity = hitEntity;
			outHitInfo.m_component = hitComponent;
			outHitInfo.m_aimStartPosition = queryStartPoint;
			outHitInfo.m_closestHitPosition = closestHitPosition;
		}

		return outFlag;
	}

	Uint64 GetMask( const red::DynArray< TargetFilter* >& filters )
	{
		const Uint64 zeroMask = physics::QueryFilter::ZERO().GetQueryMask();
		Uint64 newMask = zeroMask;
		for( const TargetFilter* filter : filters )
		{
			newMask = newMask | filter->GetFilterMask();
		}
		return newMask;
	}


	//-----------------------------------------------------------------------------------
	Bool TargetingLookAtHelper::TraceData::IsProcessable() const
	{
		return m_trace && m_trace->IsProcessed();
	}


	//-----------------------------------------------------------------------------------
	TargetingLookAtHelper::TargetingLookAtHelper( ent::EntityID ownerID, const game::IGameInstance& gameInstance )
		: m_ownerID{ ownerID }
		, m_cameraSystem{ gameInstance.GetSystem< ICameraSystem >() }
		, m_hitRepresentationSystem{ gameInstance.GetSystem< IHitRepresentationSystem >() }
		, m_physicsSystem{ gameInstance.GetScene().GetSystem< world::RuntimeSystemPhysics >() }
		, m_entityRegistry{ gameInstance.GetScene().GetSystem< world::RuntimeEntityRegistry >() }
		, m_nextCallbackId{ 1 }
	{
		m_LoSOpaqueFilter = CreateHandle< TargetFilter_ClosestOpaque >();
		m_LoSOpaqueFilter->SetFilter( physics::QueryFilter::MakeNot( RED_NAME_CONSTEXPR( "Player" ), 
																	 RED_NAME_CONSTEXPR( "PlayerBlocker" ), 
																	 RED_NAME_CONSTEXPR( "Interaction" ), 
																	 RED_NAME_CONSTEXPR( "NPCBlocker" ), 
																	 RED_NAME_CONSTEXPR( "VehicleBlocker" ),
																	 RED_NAME_CONSTEXPR( "TankBlocker" ),
																	 RED_NAME_CONSTEXPR( "Audible" ), 
																	 RED_NAME_CONSTEXPR( "Particle" ) ) );
		m_LoSOpaqueFilter->InitStates();

		m_LoSFilter = CreateHandle< TargetFilter_Closest >();
		m_LoSFilter->SetFilter( physics::QueryFilter::MakeNot( RED_NAME_CONSTEXPR( "Player" ),
															   RED_NAME_CONSTEXPR( "PlayerBlocker" ),
															   RED_NAME_CONSTEXPR( "NPCBlocker" ),
															   RED_NAME_CONSTEXPR( "VehicleBlocker" ),
															   RED_NAME_CONSTEXPR( "TankBlocker" ) ) );
		m_LoSFilter->InitStates();

		m_NoLoSFilter = CreateHandle< TargetFilter_Closest >();
		m_NoLoSFilter->SetFilter( physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "AI" ), RED_NAME_CONSTEXPR( "Collider" ), RED_NAME_CONSTEXPR( "Ragdoll" ), RED_NAME_CONSTEXPR( "Interaction" ) ) );
		m_NoLoSFilter->InitStates();
	}

	TargetingLookAtHelper::~TargetingLookAtHelper()
	{}

	void TargetingLookAtHelper::Tick()
	{
		PC_SCOPE( TargetingLookAtHelper_Tick );

		ent::EntityID prevLoSHitEntId = m_LoSFilter->GetHitEntityID();

		ProcessTrace();

		ent::EntityID curLoSHitEntId = m_LoSFilter->GetHitEntityID();
		if( prevLoSHitEntId != curLoSHitEntId )
		{
			FireHitEntityChangedCallbacks( curLoSHitEntId );
			OnLookAtObjectChanged( curLoSHitEntId, prevLoSHitEntId );
		}
	}

	ent::EntityID TargetingLookAtHelper::GetHitEntityID( Bool withLOS, Bool ignoreTransparent ) const
	{
		if( withLOS && ignoreTransparent )
		{
			return m_LoSOpaqueFilter->GetHitEntityID();
		}
		else if( withLOS )
		{
			return m_LoSFilter->GetHitEntityID();
		}
		else
		{
			return m_NoLoSFilter->GetHitEntityID();
		}
	}

	WeakHandle< ent::IComponent > TargetingLookAtHelper::GetHitComponent( Bool withLOS, Bool ignoreTransparent ) const
	{
		if( withLOS && ignoreTransparent )
		{
			return m_LoSOpaqueFilter->GetHitComponent();
		}
		else if( withLOS )
		{
			return m_LoSFilter->GetHitComponent();
		}
		else
		{
			return m_NoLoSFilter->GetHitComponent();
		}
	}

	Uint32 TargetingLookAtHelper::RegisterHitEntityChangedCallback( ITargetingSystem::LookAtObjectCallbackFunc&& func )
	{
		RED_SCOPE_LOCK( m_callbacksLock );

		Uint32 callbackId = m_nextCallbackId.Increment();
		m_callbacksData.EmplaceBack( std::move( func ), callbackId );

		return callbackId;
	}

	void TargetingLookAtHelper::UnregisterHitEntityChangedCallback( Uint32 callbackId )
	{
		RED_SCOPE_LOCK( m_callbacksLock );

		auto it = std::find_if( m_callbacksData.Begin(), m_callbacksData.End(), [ callbackId ]( const CallbackEntry& callbackEntry )
		{
			return callbackEntry.m_callbackId == callbackId;
		} );

		if( it != m_callbacksData.End() )
		{
			m_callbacksData.Remove( it );
		}
	}

	void TargetingLookAtHelper::AddIgnoredEntity( ent::EntityID entityId )
	{
		red::alg::PushBackUnique( m_ignoredEntities, entityId );
	}

	void TargetingLookAtHelper::RemoveIgnoredEntity( ent::EntityID entityId )
	{
		m_ignoredEntities.RemoveReorder( entityId );
	}

	TargetFilterTicket TargetingLookAtHelper::RegisterFilter( THandle< TargetFilter > filter )
	{
		ProcessFilter( *filter );

		Uint32 newId = m_nextFilterId.Increment();
		TargetFilterTicket newFilterTicket{ newId };

		{
			RED_SCOPE_LOCK( m_filterLock );
			m_externalFilters.EmplaceBack( newId, filter );
		}

		return newFilterTicket;
	}

	void TargetingLookAtHelper::UnregisterFilter( const TargetFilterTicket& filterId )
	{
		RED_SCOPE_LOCK( m_filterLock );

		auto it = std::find_if( m_externalFilters.Begin(), m_externalFilters.End(), [ filterId ]( const FilterEntry& filterEntry )
		{
			return filterEntry.m_filterId == filterId;
		} );

		if( it != m_externalFilters.End() )
		{
			m_externalFilters.Remove( it );
		}
	}

	void TargetingLookAtHelper::ProcessFilter( TargetFilter& filter )
	{
		filter.InitStates();

		red::DynArray< TargetFilter* > filters{ red::PoolFrame() };
		filters.EmplaceBack( &filter );

		{
			RED_SCOPE_SHARED_LOCK( m_traceDataLock );
			ProcessFilters( m_cachedTraceData, filters );
		}
	}

	void TargetingLookAtHelper::ProcessFilters( const TraceData& traceData, const red::DynArray< THandle< TargetFilter > >& filters )
	{
		red::DynArray< TargetFilter* > filterPtrs{ red::PoolFrame() };
		filterPtrs.Reserve( filters.Size() );
		for( const THandle< TargetFilter >& filter : filters )
		{
			filterPtrs.EmplaceBack( filter.Get() );
		}

		ProcessFilters( traceData, filterPtrs );
	}

	void TargetingLookAtHelper::ProcessFilters( const TraceData& traceData, const red::DynArray< TargetFilter* >& filters )
	{
		PC_SCOPE( TargetingLookAtHelper_ProcessFilters );

		if( !traceData.IsProcessable() )
		{
			return;
		}

		for( TargetFilter* filter : filters )
		{
			filter->PreFilter();
		}

		const Uint64 completeMask = GetMask( filters );
		const Uint64 zeroMask = physics::QueryFilter::ZERO().GetQueryMask();
		for( const HitInfo& hitInfo : traceData.m_hits )
		{
			const Uint64 shapeQueryMask = hitInfo.m_queryMask;
			if( ( completeMask & shapeQueryMask ) == zeroMask )
			{
				continue;
			}

			for( TargetFilter* filter : filters )
			{
				if( filter->GetStatus() == ETargetFilterStatus::Continue &&
					filter->TestFilterMask( shapeQueryMask ) )
				{
					filter->Filter( hitInfo );
				}
			}
		}

		for( TargetFilter* filter : filters )
		{
			filter->PostFilter();
		}
	}

	void TargetingLookAtHelper::ProcessTrace()
	{
		red::DynArray< THandle< TargetFilter > > filters{ red::PoolFrame() };
		{
			RED_SCOPE_LOCK( m_filterLock );
			filters.Reserve( 3 + m_externalFilters.Size() );
			filters.EmplaceBack( m_LoSFilter );
			filters.EmplaceBack( m_NoLoSFilter );
			filters.EmplaceBack( m_LoSOpaqueFilter );

			for ( Uint32 i = 0; i < m_externalFilters.Size(); ++i )
			{
				const FilterEntry& entry = m_externalFilters[ i ];
				if( THandle< TargetFilter > hFilter = entry.m_filter.ToHandle() )
				{
					filters.EmplaceBack( hFilter );
				}
				else
				{
					RED_LOG_ERROR( "Registered filter got destroyed. Did you forget to unregister it?" );
					m_externalFilters.RemoveAtReorder( i-- );
				}
			}
		}

		ProcessHits( m_traceData );
		ProcessFilters( m_traceData, filters );
		if( g_coreplayLookAtProbe > 0 && g_coreplayLookAtProbe % 50 == 0 )
		{
			std::fprintf( stderr, "[coreplay] lookAt filters: %u (processable=%d hits=%u)\n", Uint32( filters.Size() ), m_traceData.IsProcessable() ? 1 : 0, Uint32( m_traceData.m_hits.Size() ) );
			for( const THandle< TargetFilter >& f : filters )
			{
				if( !f ) continue;
				std::fprintf( stderr, "[coreplay]   filter %s status=%d mask=%llx valid=%d hitEnt=%s\n", f->GetClass()->GetName().AsChar(), Int32( f->GetStatus() ),
					( unsigned long long )f->GetFilterMask(), f->IsValid() ? 1 : 0, f->GetHitEntityID().ToDebugString().AsChar() );
			}
		}

		if( !m_traceData.m_trace || m_traceData.m_trace->IsProcessed() || m_traceData.m_trace->IsInvalid() )
		{
			{
				PC_SCOPE( TargetingLookAtHelper_SwapTraceData );
				RED_SCOPE_LOCK( m_traceDataLock );
				m_cachedTraceData = m_traceData;
			}

			// in multiplayer we need to know which player we are talking about 
			THandle<ent::Entity> owner = nullptr;
			if( m_cameraSystem.GetGameInstance()->GetRuntimeInfo().IsMultiplayer() )
			{
				// is this worth caching a weakhandle instead?? 
				// that would then add the requirement that all code needs keep the weekhandle and the m_ownerID in sync
				owner = m_entityRegistry->FindEntity( m_ownerID );
			}

			Transform cameraTransform{ Transform::IDENTITY() };
			m_cameraSystem.GetActiveCameraWorldTransform( cameraTransform, owner );

			const Vector4 aimPosition = cameraTransform.GetPosition();
			const Vector4 aimForward = cameraTransform.GetForward();

			auto params = physics::RaycastParams::Create( aimPosition, aimForward, c_maxLookAtDistance, physics::QueryFilter::ALL() ).SetUseCase( physics::QueryUseCase::Gameplay );
			m_traceData.m_trace = m_physicsSystem->AsyncRaycastN( params );
			m_traceData.m_startPosition = aimPosition;
			m_traceData.m_endPosition = aimPosition + aimForward * c_maxLookAtDistance;
		}
	}

	void TargetingLookAtHelper::ProcessHits( TraceData& traceData )
	{
		PC_SCOPE( TargetingLookAtHelper_ProcessHits );

		if( !traceData.IsProcessable() )
			return;

		traceData.m_hits.Clear();

		// Note: george.tatoulis We are not considering the 0 distance case here means we are penetrating the collision volume
		// there are probably cases where this is valid and probably cases where it is not. No plan yet if this needs to be resolved. 

		red::DynArray< ent::EntityID > hitRepresentationEntities{ red::PoolFrame() };
		for( const physics::TraceResult& result : traceData.m_trace->m_results )
		{
			THandle<ent::Entity> hitEntity = result.FindHitObject<ent::Entity>();
			ent::EntityID hitEntityId = hitEntity ? hitEntity->GetEntityID() : ent::EntityID{};

			if( m_ignoredEntities.Exist( hitEntityId ) )
			{
				continue;
			}

			if( hitRepresentationEntities.Exist( hitEntityId ) )
			{
				HitInfo clonedHitInfo;
				for( const HitInfo& hitInfo : traceData.m_hits )
				{
					if( hitInfo.m_entityId == hitEntityId )
					{
						clonedHitInfo = hitInfo;
						break;
					}
				}

				if( clonedHitInfo.m_entityId.IsDefined() )
				{
					clonedHitInfo.m_queryMask = result.m_shapeQueryMask;
					traceData.m_hits.PushBack( std::move( clonedHitInfo ) );
				}
			}
			else
			{
				HitInfo hitInfo;
				EProcessHitFlag processFlag = ConstructHitInfo( result, hitEntity, m_hitRepresentationSystem, traceData.m_startPosition, traceData.m_endPosition, hitInfo );
				if( processFlag & EProcessHitFlag::HasHitRepresentation )
				{
					hitRepresentationEntities.PushBack( hitEntityId );
				}

				if( processFlag & EProcessHitFlag::Valid )
				{
					traceData.m_hits.PushBack( std::move( hitInfo ) );
				}
			}
		}

		std::sort( traceData.m_hits.Begin(), traceData.m_hits.End(), []( const HitInfo& lhs, const HitInfo& rhs ) {
			return lhs.GetSquaredDistanceToHitPosition() < rhs.GetSquaredDistanceToHitPosition();
		} );
		// CP2077 web [coreplay]: lookAt raycast probe, armed by the sandbox test (op 8 sets g_coreplayLookAtProbe = N traces)
		Bool cpAnyInteraction = false;
		for( const HitInfo& h : traceData.m_hits ) { if( h.m_queryMask & physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Interaction" ) ).GetQueryMask() ) cpAnyInteraction = true; }
		if( g_coreplayLookAtProbe > 0 && ( --g_coreplayLookAtProbe % 50 == 0 || cpAnyInteraction ) )
		{
			static const Uint64 c_inter = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Interaction" ) ).GetQueryMask();
			static const Uint64 c_stat = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Static" ) ).GetQueryMask();
			static const Uint64 c_dyn = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Dynamic" ) ).GetQueryMask();
			static const Uint64 c_coll = physics::QueryFilter::Make( RED_NAME_CONSTEXPR( "Collider" ) ).GetQueryMask();
			std::fprintf( stderr, "[coreplay] lookAt trace: %u raw results, %u hits (masks Interaction=%llx Static=%llx Dynamic=%llx Collider=%llx)\n",
				Uint32( traceData.m_trace->m_results.Size() ), Uint32( traceData.m_hits.Size() ), ( unsigned long long )c_inter, ( unsigned long long )c_stat, ( unsigned long long )c_dyn, ( unsigned long long )c_coll );
			static Bool s_namesLogged = false;
			if( !s_namesLogged )
			{
				s_namesLogged = true;
				static const char* const c_names[] = { "Player", "Static", "Dynamic", "Vehicle", "Tank", "Destructible", "Terrain", "Collider", "Particle", "Ragdoll", "Ragdoll Inner", "Debris", "Cloth",
					"PlayerBlocker", "VehicleBlocker", "TankBlocker", "DestructibleCluster", "NPCBlocker", "Visibility", "Audible", "Interaction", "Shooting", "Water", "NetworkDevice",
					"NPCTraceObstacle", "PhotoModeCamera", "FoliageDestructible", "NPCNameplate", "NPCCollision", "AI", "Default" };
				for( const char* nm : c_names )
				{
					std::fprintf( stderr, "[coreplay] query group %s = %llx\n", nm, ( unsigned long long )physics::QueryFilter::Make( RED_NAME( nm ) ).GetQueryMask() );
				}
			}
			Uint32 n = 0;
			for( const physics::TraceResult& result : traceData.m_trace->m_results )
			{
				if( n++ >= 8 ) break;
				THandle<ent::Entity> e = result.FindHitObject<ent::Entity>();
				std::fprintf( stderr, "[coreplay]   raw %u: entity %s (%s) shapeMask=%llx\n", n, e ? e->GetEntityID().ToDebugString().AsChar() : "none",
					e ? e->GetClass()->GetName().AsChar() : "-", ( unsigned long long )result.m_shapeQueryMask );
			}
			n = 0;
			for( const HitInfo& h : traceData.m_hits )
			{
				if( n++ >= 8 ) break;
				std::fprintf( stderr, "[coreplay]   hit %u: entity %s mask=%llx%s%s%s%s dist %.2f\n", n, h.m_entityId.ToDebugString().AsChar(), ( unsigned long long )h.m_queryMask,
					( h.m_queryMask & c_inter ) ? " INTERACTION" : "", ( h.m_queryMask & c_stat ) ? " static" : "", ( h.m_queryMask & c_dyn ) ? " dynamic" : "", ( h.m_queryMask & c_coll ) ? " collider" : "",
					std::sqrt( h.GetSquaredDistanceToHitPosition() ) );
			}
		}
	}

	void TargetingLookAtHelper::FireHitEntityChangedCallbacks( const ent::EntityID& entityId )
	{
		RED_SCOPE_LOCK( m_callbacksLock );

		for( const CallbackEntry& callbackEntry : m_callbacksData )
		{
			callbackEntry.m_callbackFunc( entityId );
		}
	}

	void TargetingLookAtHelper::OnLookAtObjectChanged( ent::EntityID newObjectID, ent::EntityID oldObjectID )
	{
		OnLookAtStopped( oldObjectID );
		OnLookAtStarted( newObjectID );
	}

	void TargetingLookAtHelper::OnLookAtStarted( ent::EntityID lookAtObjectID )
	{
		if( THandle< game::Object > lookAtObjectStrong = GetLookAtObjectFromEntID( lookAtObjectID ) )
		{
			const THandle< game::targetingSystem::ObjectLookedAtEvent > lookAtEvent = CreateHandle< game::targetingSystem::ObjectLookedAtEvent >( m_ownerID, true );
			lookAtObjectStrong->QueueEvent( lookAtEvent );
		}
	}

	void TargetingLookAtHelper::OnLookAtStopped( ent::EntityID lookAtObjectID )
	{
		if( THandle< game::Object > lookAtObjectStrong = GetLookAtObjectFromEntID( lookAtObjectID ) )
		{
			const THandle< game::targetingSystem::ObjectLookedAtEvent > lookAtEvent = CreateHandle< game::targetingSystem::ObjectLookedAtEvent >( m_ownerID, false );
			lookAtObjectStrong->QueueEvent( lookAtEvent );
		}
	}

	THandle< game::Object > TargetingLookAtHelper::GetLookAtObjectFromEntID( ent::EntityID id )
	{
		THandle< game::Object > lookAtObject = nullptr;

		if( THandle< ent::Entity > entity = m_entityRegistry->FindEntity( id ) )
		{
			lookAtObject = Cast< game::Object >( entity );
		}

		return lookAtObject;
	}

	TargetingLookAtHelper::CallbackEntry::CallbackEntry( ITargetingSystem::LookAtObjectCallbackFunc&& func, Uint32 id )
		: m_callbackFunc( std::move( func ) )
		, m_callbackId( id )
	{}

	TargetingLookAtHelper::FilterEntry::FilterEntry( const TargetFilterTicket& filterId, THandle< TargetFilter > filter )
		: m_filterId{ filterId }
		, m_filter{ filter }
	{}
}

#ifdef DEBUG_GAMEPLAY
	#pragma optimize("",on)
#endif
