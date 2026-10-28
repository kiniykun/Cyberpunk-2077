/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/
//owner: andrzej.drabarek, tomas.plch

#include "build.h"
#include <cstdio>

#include "runtimeSystemTraffic.h"

#include "../../engine/include/nativeFunctionsRegistry.h"
#include "../../engine/include/tickInfo.h"
#include "../../redSystem/include/utility.h"
#include "../../redSystem/include/stopWatch.h"
#include "../../renderDraw/include/fragmentBuilder.h"

#include "../../redMath/include/random.h"
#include "../../redMath/include/numericalUtils.h"
#include "../../redReflection/include/mathUtils.h"
#include "../../redReflection/include/engineTime.h"

#include "../../triggers/include/areaShape.h"

#include "../../world/include/renderDebugContext.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../world/include/gameInstance.h"

#include "../../worldNavigation/include/trafficPersistentResource.h"

#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"
#include "../../worldStreaming/include/worldStreamingWorld.h"

#include "../../worldPhysics/include/runtimeSystemPhysics.h"
#include "../../gameTweakDB/include/records/tweakDBCharacter.h"
#include "../../gameTweakDB/include/records/tweakDBVehicle.h"
#include "../../gameTweakDB/include/records/tweakDBVehicleDestruction.h"
#include "../../gameTweakDB/include/records/tweakDBVehicle.h"
#include "../../gameTweakDB/include/tweakDBHelpers.h"

#include "trafficDebugUtils.h"
#include "trafficHelpers.h"
#include "trafficLaneEventsBroadcaster.h"
#include "trafficLaneFilters.h"
#include "trafficLocalPath.h"
#include "trafficPathfinding.h"
#include "trafficPersistentResource.h"
#include "trafficRuntimeData.h"
#include "trafficLocalPath.h"
#include "trafficCollisionResource.h"
#include "trafficCollisionDebug.h"
#include "trafficCompiledNode.h"
#include "trafficGreenWaveManager.h"
#include "trafficScriptInterface.h"
#include "deadEndDetector.h"
#include "trafficSpawning.h"

#include "spatialGrid.h"

#include "../../worldNavigation/include/runtimeSystemNavigation.h"


#ifdef RED_USE_IMGUI 
#include "../../imgui/include/redImGui.h"

#include "../../redConfig/include/configVarSystem.h"
#include "../../redConfig/include/configVarRegistry.h"

#endif


#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
#include "../../redCore/include/absolutePath.h"
#include "../../redFileSystem/include/fileSys.h"
#endif

#ifdef DEBUG_TRAFFIC
#pragma optimize("", off)
#endif


namespace Config
{
TConfigVar< Float > cvTeleportDistance( "Traffic", "TeleportationDistance", std::numeric_limits< Float >::max() );
TConfigVar< Float > cvMaxLocalPathLength( "Traffic", "MaxLocalPathLength", 100.0f ); // Not used, why ?
TConfigVar< Float > cvTrafficSpotDetectionPrecision( "Traffic", "SpotDetectionPrecision", 5.0f );
TConfigVar< Float > cvTrafficSpotDetectionRange( "Traffic", "SpotDetectionRange", 3.0f );
TConfigVar< Int32 > cvMaxPathLanes( "Traffic", "MaxPathLanes", 5u );
TConfigVar< Float > cvSlotLaneOccupancyAreaPedestrian( "Traffic", "SlotLaneOccupancyArea_Pedestrian", 2.5f );
TConfigVar< Float > cvSlotLaneOccupancyAreaVehicleLength( "Traffic", "SlotLaneOccupancyArea_Vehicle_Length", 6.0f );
TConfigVar< Bool > cvAvoidZebra2Zebra( "Traffic", "AvoidZebra2Zebra", true );


TConfigVar< Bool > cvDisableSpawnRequests( "Traffic", "StopSpawn", false );

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
TConfigVar< Float > cvInitTimeFrameForDebugLanesSpawn( "Traffic", "LaneSpawnLogWindow", 30.f );
TConfigVar< Bool > cvSnapshottingEnabled( "Debug", "CollisionSnapshotEnabled", false );
#endif

	// IMPORTANT: Setting this to true will potentially crash if the resource is too big!
WORLD_NAVIGATION_API TConfigVar< Bool > cvLoadCollisionDebugResource( "Editor/Traffic", "LoadCollisionDebugResource", false );

}

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeSystemTraffic, world );
RTTI_PARENT_TYPE( IRuntimeSystem );
RTTI_END_TYPE();

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace
{
static rend::DebugFilter DF_TrafficLanes( "Traffic/Lanes" );
static rend::DebugFilter DF_TrafficStaticCollision( "Traffic/StaticCollision" );
static rend::DebugFilter DF_TrafficLaneConnections( "Traffic/LaneConnections" );
static rend::DebugFilter DF_TrafficLaneSubgraphIDs( "Traffic/SubgraphIDs" );
static rend::DebugFilter DF_TrafficMemoryDebug( "Traffic/MemoryDebug" );
static rend::DebugFilter DF_TrafficSpots( "Traffic/Spots" );
static rend::DebugFilter DF_TrafficLaneUIDs( "Traffic/LaneUIDs" );
static rend::DebugFilter DF_TrafficPrintCachedCrossingLane( "Traffic/PrintCachedCrossingLanes" );

// --------------------------------------------------------------------------------

template< typename... Args >
world::TrafficSlotPtr CreateSlot( Args&&... args )
{
	return red::CreateSharedPtr< world::TrafficSlot, nav::PoolTrafficSlots >( std::forward<Args>( args )... );
}

// --------------------------------------------------------------------------------

#ifdef RED_USE_IMGUI
void DrawGodRay( Color color, Vector3 position, Float radius, rend::FragmentBuilder& fragmentBuilder )
{
	Box rayBox;

	fragmentBuilder.SetColor( color );
	rayBox.Min.X = position.X;
	rayBox.Max.X = position.X + radius;
	rayBox.Min.Y = position.Y;
	rayBox.Max.Y = position.Y + radius;
	rayBox.Min.Z = 0.f;
	rayBox.Max.Z = 10000.f;
	fragmentBuilder.AddSolidBox( rayBox );
}

// --------------------------------------------------------------------------------

void DrawDataErrors( rend::FragmentBuilder& fragmentBuilder, const world::TrafficPersistentData& trafficPersistentData, const red::DynArray< world::TrafficLaneUID >& brokenUIDs,
					 Bool showOnMap, ImVec4 color, Float radius, Bool showGodRays, Color godRaysColor, Float godRaysRadius )
{
	for ( const auto& id : brokenUIDs )
	{
		auto lanePtr = trafficPersistentData.FindLane( id );
		if ( lanePtr )
		{
			auto lane = *lanePtr;

			Vector4 endPoint = lane.m_outline.Back();

			red::ImGui::CanvasCircleFilled( ToImVec2( endPoint ), radius, color );

			if ( showGodRays )
			{
				DrawGodRay( godRaysColor, endPoint, godRaysRadius, fragmentBuilder );
			}
		}
	}
}
#endif

}	//anonymous namespace

//////////   TRAFFIC PERSISTENT RESOURCE /////////////////////////////////
namespace world
{
struct TrafficSystemPersistentResources
{
	RED_USE_MEMORY_POOL( nav::PoolTrafficNavigation );

	res::ResourceTokenHandle m_persToken;
	res::ResourceTokenHandle m_spatialResourceToken;
	res::ResourceTokenHandle m_connectionsToken;
	res::ResourceTokenHandle m_polygonsToken;
	res::ResourceTokenHandle m_collisionsToken;
	res::ResourceTokenHandle m_nullAreasToken;

	TrafficSystemPersistentResources() = default;
	~TrafficSystemPersistentResources() = default;
};

//================================================================================

res::ResourceTokenHandle TryLoadTrafficResource( TResAsyncRef< CResource > resourceReference, const char* errorMessage )
{
	if ( !resourceReference.IsValid() )
	{
		RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Traffic, errorMessage );
		return nullptr;
	}

	return resourceReference.IssueLoadingRequest();
}

TrafficSystemPersistentResources CreateLoadResource( world::RuntimeSystemWorldStreaming* streamingWorldSystem, const RuntimeSceneSetupInfo* setupInfo )
{
	auto tokens = TrafficSystemPersistentResources();

#if !defined( RED_CONFIGURATION_FINAL ) && defined( RED_USE_RESOURCEPATH_STRINGS )
	if ( setupInfo )
	{
		const res::ResourcePath worldPath = setupInfo->m_runtimeInfo.GetResourcePath();
		const auto extension = red::paths::GetExtension( worldPath.ToStringView() );

		if ( extension == "world" )
		{
			tokens.m_persToken = GResourceLoader->IssueLoadingRequest( CalcTrafficPersistentResourcePath( worldPath ) );
			tokens.m_spatialResourceToken = GResourceLoader->IssueLoadingRequest( CalcTrafficPersistentSpatialResourcePath( worldPath ) );
			tokens.m_connectionsToken = GResourceLoader->IssueLoadingRequest( CalcTrafficPersistentLaneConnectionsPath( worldPath ) );
			tokens.m_polygonsToken = GResourceLoader->IssueLoadingRequest( CalcTrafficPersistentLanePolygonsPath( worldPath ) );

			tokens.m_collisionsToken = GResourceLoader->IssueLoadingRequest( CalcTrafficCollisionResourcePath( worldPath ) );
			tokens.m_nullAreasToken = GResourceLoader->IssueLoadingRequest( CalcTrafficNullAreaCollisionResourcePath( worldPath ) );

			return tokens;
		}
		else
		{
			return tokens;
		}
	}
#endif
	
	RED_FATAL_ASSERT( streamingWorldSystem, "No streaming world system provided, cannot proceede" );
	// CP2077 web: asserts are compiled out in FINAL; a session whose .streamingworld failed to load has a
	// null streaming world. Return empty tokens (the loader below already handles null tokens).
	if ( !streamingWorldSystem )
	{
		return tokens;
	}

	auto streamingWorld = streamingWorldSystem->GetStreamingWorld();
	RED_FATAL_ASSERT( streamingWorld, "No streaming world for the streaming world system" );
	if ( !streamingWorld )
	{
		std::fprintf( stderr, "CP2077 traffic guard: no streaming world loaded; no traffic data\n" );
		return tokens;
	}

	tokens.m_persToken = TryLoadTrafficResource( streamingWorld->m_trafficPersistentResource, "Traffic persistent data resource path is not valid." );
	tokens.m_spatialResourceToken = TryLoadTrafficResource( streamingWorld->m_trafficSpatialRepresentationResource, "Traffic spatial data resource path is not valid." );
	tokens.m_connectionsToken = TryLoadTrafficResource( streamingWorld->m_trafficLaneConnectivityResource, "Traffic lanes connectivity data resource path is not valid." );
	tokens.m_polygonsToken = TryLoadTrafficResource( streamingWorld->m_trafficLanePolygonsResource, "Traffic lane polygons data resource path is not valid." );
	tokens.m_collisionsToken = TryLoadTrafficResource( streamingWorld->m_trafficCollisionResource, "Traffic collision data resource path is not valid." );
	tokens.m_nullAreasToken = TryLoadTrafficResource( streamingWorld->m_trafficNullAreaCollisionResource, "Traffic null areas data resource path is not valid." );
	
	return tokens;
}

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Anonymous statics //
namespace world
{
	// Note: returns reference to one of arrays: outLanes or preferedLanes - this is to spare redundant array copy operations
static red::DynArray< TrafficConnectivityOutLane >& GetFilteredOutLanes(
	red::DynArray< TrafficConnectivityOutLane >& outLanes,
	red::DynArray< TrafficConnectivityOutLane >& preferedLanes,
	const red::DynArray< TrafficConnectivityOutLane >& unfilteredLanes,
	const TrafficLanePersistent& currentLane,
	const red::DynArray< TrafficGlobalPath::Lane >& lanesAddedSoFar,
	const TrafficOutLaneFilter& outLaneFilter,
	const TrafficPreferredOutLaneFilter& preferedOutlanesFilter,
	const RuntimeSystemTraffic& trafficSystem,
	const red::DynArray< TrafficAreaShape >& nullAreas )
{
	// Filter out lanes invalid for traffic
	outLanes = unfilteredLanes;
	red::alg::EraseIf( outLanes, [&lanesAddedSoFar, &currentLane, &outLaneFilter, &trafficSystem, &nullAreas]( const TrafficConnectivityOutLane &outLane )
	{
		const TrafficLanePersistent& destinationLane = trafficSystem.GetPersistentData()->GetLane( outLane.laneIndex );
		const TrafficLaneUID uid = destinationLane.GetLaneId();

		if ( !trafficSystem.IsLaneValidForUse( uid ) )
		{
			return true;
		}

		if ( outLaneFilter && outLaneFilter( currentLane, outLane, &trafficSystem ) == TrafficFilterOperation::Remove )
		{
			return true;
		}

		if ( destinationLane.HasFlag( TrafficLanePersistent::Flags::TrafficDisabled ) ) return true;

		// Eliminate the scenario where a lane is left and its reverse is visited again immediately.
		if ( lanesAddedSoFar.Size() > 1 && lanesAddedSoFar[lanesAddedSoFar.Size() - 2].m_laneId == uid.GetReverse() )
			return true;

		// Eliminate the scenario where a lane is entered further than its supposed to be left.
		// lane |----------------------------------------------------------------->
		//              ^ exit here                      ^ entered here
		// This would require backtracking along the lane, which is not allowed (lanes are one directional).
		Float exitEntryDist = ( outLane.thisLaneExitPosition - lanesAddedSoFar.Back().m_entryPosition );
		if ( exitEntryDist < 0.0f )
			return true;

		// Null area check
		if ( trafficSystem.DoesLaneIntersectWithAreas( destinationLane, nullAreas, outLane.nextLaneEntryPosition ) )
			return true;
		
		return false;
	} );

	// Try to add some preferred conditions
	preferedLanes = outLanes;
	red::alg::EraseIf( preferedLanes, [&trafficSystem, &preferedOutlanesFilter, &currentLane]( const TrafficConnectivityOutLane &outLane )
	{
		if ( outLane.isSharpAngle )
			return true;

		if ( preferedOutlanesFilter && ( preferedOutlanesFilter( currentLane, outLane, &trafficSystem ) == TrafficFilterOperation::Remove ) )
		{
			return true;
		}

		const TrafficLanePersistent& destinationLane = trafficSystem.GetPersistentData()->GetLane( outLane.laneIndex );
		const TrafficLaneUID uid = destinationLane.GetLaneId();

		const Uint32 slotsCount = trafficSystem.GetSlotsCountOnLane( uid ) + trafficSystem.GetSlotsCountOnLane( uid.GetReverse() );
		const Uint32 preferredSlotCount = destinationLane.HasFlag( TrafficLanePersistent::Flags::Pavement )
			? trafficSystem.GetPreferredSlotsCountOnLane( uid, TrafficMovementBehavior::Pedestrian )
			: trafficSystem.GetPreferredSlotsCountOnLane( uid, TrafficMovementBehavior::Car );

		if ( slotsCount >= preferredSlotCount )
			return true;

		return false;
	} );

	return !preferedLanes.Empty() ? preferedLanes : outLanes;
}

// --------------------------------------------------------------------------------

static const TrafficConnectivityOutLane& PickRandomOutLane( const red::DynArray< TrafficConnectivityOutLane >& outLanes )
{
	RED_ASSERT( !outLanes.Empty() );

	const TrafficConnectivityOutLane* pickedLane = nullptr;

	Float totalProbability = 0.0f;
	for ( const TrafficConnectivityOutLane &ol : outLanes )
	{
		totalProbability += ol.GetExitProbability();
	}

	Float randomValue = math::DefaultRandom().Get< Float >( 0.0f, totalProbability );

	Float sum = 0.0f;
	for ( const TrafficConnectivityOutLane &ol : outLanes )
	{
		sum += ol.GetExitProbability();

		if ( sum >= randomValue )
		{
			pickedLane = &ol;
			break;
		}
	}

	RED_ASSERT( pickedLane != nullptr );
	return *pickedLane;
}

// --------------------------------------------------------------------------------

RED_INLINE void RuntimeSystemTraffic::LaneLookup::Clear()
{
	m_pavement.Clear();
#ifdef ENABLE_FULL_LANE_LOOKUP
	m_roads.Clear();
	m_zebras.Clear();
	m_intersections.Clear();
#endif
}

// --------------------------------------------------------------------------------
RED_INLINE void RuntimeSystemTraffic::LaneLookup::Shrink()
{
	m_pavement.Shrink();
#ifdef ENABLE_FULL_LANE_LOOKUP
	m_roads.Shrink();
	m_zebras.Shrink();
	m_intersections.Shrink();
#endif
}

// --------------------------------------------------------------------------------
RED_INLINE Uint32 RuntimeSystemTraffic::LaneLookup::DataSize() const
{
	return
		m_pavement.DataSize()
#ifdef ENABLE_FULL_LANE_LOOKUP
		+ m_roads.DataSize() + m_zebras.DataSize() + m_intersections.DataSize()
#endif
		;
}

// --------------------------------------------------------------------------------
RED_INLINE Uint32 RuntimeSystemTraffic::LaneLookup::DataCapacity() const
{
	return
		m_pavement.DataCapacity()
#ifdef ENABLE_FULL_LANE_LOOKUP
		+ m_roads.DataCapacity() + m_zebras.DataCapacity() + m_intersections.DataCapacity()
#endif
		;
}

}

//////////////////////////////////////////////////////////////////////////

namespace world
{
//================================================================================
RED_INLINE void RuntimeSystemTraffic::Evaluators::Update( RuntimeSystemTraffic& sys )
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	for ( auto v : m_evaluators )
	{
		auto& eval = v.Value();
		if ( eval.m_active )
		{
			eval.m_call( sys );
		}
	}
}

//================================================================================
RED_INLINE void RuntimeSystemTraffic::Evaluators::Clear()
{
	RED_SCOPE_LOCK( m_lock );
	m_evaluators.Clear();
}

}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace world
{

#ifndef RED_CONFIGURATION_FINAL
Config::TConfigVar< Float > RuntimeSystemTraffic::s_laneDebugRenderPosZOffset( "Traffic/GeneralDebugDraw", "laneZOffset", 0.35f );
#endif

	// --------------------------------------------------------------------------------

RuntimeSystemTraffic::RuntimeSystemTraffic()
	: m_navigationSystem( nullptr )
	, m_persistentData( nullptr )
	, m_runtimeState( this )
	, m_lightChangeListenerRegistry( *this )
	, m_collisionListener( *this )
{
#if !defined( NO_DEBUG_CONSOLE )
	GetNativeFunRegistry().RegisterNativeFunction( "Traffic.ReverseLights", "Reverses all traffic lights", &ConsoleCommandReverseTrafficLights, this );
#endif

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	m_timeFrame = Config::cvInitTimeFrameForDebugLanesSpawn.Get();
#endif

	m_unregisteredRemovedLanes.Reserve( 128 );
	m_unregisteredRemovedNodes.Reserve( 128 );


#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	m_dynamicCollisionsMap.m_snapshotsEnabled = Config::cvSnapshottingEnabled.Get();
#endif
}

// --------------------------------------------------------------------------------

RuntimeSystemTraffic::~RuntimeSystemTraffic()
{
#if !defined( NO_DEBUG_CONSOLE )
	GetNativeFunRegistry().UnregisterNativeFunction( "Traffic.ReverseLights" );
#endif
}

// --------------------------------------------------------------------------------

SystemID RuntimeSystemTraffic::GetSystemID() const
{
	return SYSTEM_ID;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterUpdateFunctions( UpdatableSystemRegistrar& registrar )
{
	//================================================================================
	registrar.Register( UpdateTickGroup::FrameBegin, *this, "Traffic_FrameStart",
						[this]( const UpdateContext& updateContext, job::Builder& builder )
	{
		PC_SCOPE( RuntimeSystemTraffic_FrameBegin );

		auto dt = updateContext.m_tickInfo.m_globalTimeDilation * updateContext.m_tickInfo.m_realTimeDelta;
		m_trafficTime += dt;

		builder.DispatchJob( "TrafficSystem_LaneRegistration", [this]( const job::RunContext& jobCtx )
		{
			ProcessAllPendingLanesRegistration();
		} );

		builder.DispatchJob( "TrafficSystem_UpdateLanes", [this, dt]( const job::RunContext& jobCtx )
		{
			{
				PC_SCOPE( "TrafficSystem_UpdateLanes_TraverseLanes" );

				RED_SCOPE_LOCK( m_lock );

				// Purge lanes with removed slots
				for ( const TrafficLaneUID& id : m_lanesToPurge )
				{
					auto it = m_laneLUT.Find( id );
					if ( it != m_laneLUT.End() )
					{
						it.Value()->ClearExpiredSlots();
					}
				}
				

#ifndef RED_CONFIGURATION_FINAL
				{
					PC_SCOPE( "TrafficSystem_UpdateLanes_DebugUpdate" );
					for ( auto it : m_laneLUT )
					{
						auto& lane = it.Value();

						if ( !lane->AreAllSlotsValid() )
						{
							auto it = std::find_if( m_lanesToPurge.Begin(), m_lanesToPurge.End(), [&]( auto& id ) {	return id == lane->GetPersistentData().GetLaneId();	} );
							RED_LOG_ERROR( " Lane '%s' has some invalid slots (purge lane status %s)", lane->GetPersistentData().GetLaneId().ToDebugString().AsChar(), it == m_lanesToPurge.End() ? "NotPresent" : "Present" );
						}

						lane->UpdateDebug( dt );
					}
				}
#endif

				m_lanesToPurge.Clear();

				// Update lanes with traffic light changes
				for ( const TrafficLaneUID& id : m_lanesWithTrafficLights )
				{
					auto it = m_laneLUT.Find( id );
					if ( it != m_laneLUT.End() )
					{
						it.Value()->UpdateBlockedPositions();
					}
				}
				m_lanesWithTrafficLights.Clear();
			}

			{
				PC_SCOPE( "TrafficSystem_UpdateLanes_CheckNodeDumps" );
				RED_SCOPE_SHARED_LOCK( m_lock );

				if ( m_checkNodeDump )
				{
					RED_ASSERT( m_unregisteredRemovedLanes.Empty() );
					RED_ASSERT( m_unregisteredRemovedNodes.Empty() );

					for ( auto& it : m_nodeDump )
					{
						auto nodeIt = m_nodeUsage.Find( it );
						RED_ASSERT( nodeIt != m_nodeUsage.End() );
						if ( nodeIt.Value() == 0u ) //all the nodes' uses dropped
						{
							auto laneIt = m_nodeToLaneLUT.Find( it );
							RED_ASSERT( laneIt != m_nodeToLaneLUT.End() );

							auto isOccupied = false;

							for ( auto& laneID : laneIt.Value() )
							{
								auto lane = FindLane( laneID );
								if ( lane->GetSlotsCount() != 0u )
								{
									isOccupied = true;
									break;
								}
							}

							if ( isOccupied ) continue;

							m_unregisteredRemovedLanes.PushBack( laneIt.Value() );
							m_unregisteredRemovedNodes.PushBack( it );
						}
					}

					m_checkNodeDump = false;
				}
			}

			if ( !m_unregisteredRemovedLanes.Empty() )
			{
				PC_SCOPE( "TrafficSystem_UpdateLanes_RemovedLanes" );
				{
					PC_SCOPE( "TrafficSystem_UpdateLanes_TrafficEventCallbacks" );
					m_trafficLaneEventsBroadcaster->BroadcastEvent( m_unregisteredRemovedLanes, TrafficLaneEvent::BeforeUnregister );
				}

				{
					PC_SCOPE( "TrafficSystem_UpdateLanes_LaneRemovalProcess" );
					RED_SCOPE_LOCK( m_lock );

					//lane removal
					for ( auto& laneId : m_unregisteredRemovedLanes )
					{
						m_laneLUT.Remove( laneId );
						m_laneLookup.Remove( laneId );
					}

					{
						PC_SCOPE( "TrafficSystem_UpdateLanes_CollisionDataManagement" );

						//we return the collision data
						m_staticCollisions.VisitEntriesByReferences( m_unregisteredRemovedNodes, [&]( auto entry, const GlobalNodeID& nodeId )
						{
							m_persistentStaticCollisions.InjectCollisions( entry->m_data, nodeId );
						} );

						{
							PC_SCOPE( "TrafficSystem_UpdateLanes_StaticCollisionRemoved" );
							m_staticCollisions.Remove( m_unregisteredRemovedNodes );
						}
					}

					//node removal
					for ( auto& nodeId : m_unregisteredRemovedNodes )
					{
						m_nodeToLaneLUT.Remove( nodeId );
						m_nodeDump.Remove( nodeId );
						m_nodeUsage.Remove( nodeId );
						m_laneLookup.Remove( nodeId );

						auto it = m_intersections.Find( nodeId );
						if ( it != m_intersections.End() )
						{
							m_lightChangeListenerRegistry.OnIntersectionDestroy( it.Value().Get() );
							m_intersections.Remove( it );
						}
					}

					m_unregisteredRemovedLanes.Clear();
					m_unregisteredRemovedNodes.Clear();
					
				}
			}

		} );

		builder.DispatchJob( "TrafficSystem_GreenWaveTick", [this]( const job::RunContext& jobCtx )
		{
			m_greenWaveManager->Tick();
		} );


		builder.DispatchJob( "TrafficSystem_Intersections", [this, dt]( const job::RunContext& jobCtx )
		{
			UpdateIntersections( dt );
		} );
		
		builder.DispatchJob( "TrafficSystem_DeadEnds", [this, dt]( const job::RunContext& jobCtx )
		{
			if ( m_deadEndDetector )
			{
				m_deadEndDetector->CheckLanes();
			}
		} );

		builder.DispatchJob( "TrafficSystem_Collisions", [this, dt]( const job::RunContext& jobCtx )
		{
			m_collisionListener.UpdateCollisions();
		} );

		builder.DispatchJob( "TrafficSystem_Pathfinding", [this, dt]( const job::RunContext& jobCtx )
		{
			ProcessFindGlobalPathRequests();
		} );

		//////////////////////////////////////////////////////////////////////////
		builder.DispatchJob( "TrafficSystem_SpawnRequests", [&]( const job::RunContext& jobCtx )
		{
			RED_SCOPE_LOCK( m_spawnLock );
			m_spawnRequests.Remove( std::remove_if( m_spawnRequests.Begin(), m_spawnRequests.End(), [&]( auto& rq )
			{
				return ( m_trafficTime - rq.m_timestamp ) > TrafficSpawning::SpawnSupport::SpawnRequest::s_defaultTimeout;
			} ), m_spawnRequests.End() );
		} );

		//////////////////////////////////////////////////////////////////////////

		builder.DispatchJob( "TrafficSystem_ClearUpdateCollisions", [this, dt]( const job::RunContext& jobCtx )
		{
			job::Builder builder( jobCtx );

			builder.DispatchJob<job::Fence::None>( "TrafficSystem_DynamicCollisions", [&, dt]( const job::RunContext& jobCtx )
			{
				m_dynamicCollisionsMap.Update( dt );
			} );
			builder.DispatchJob<job::Fence::None>( "TrafficSystem_Disposal", [&, dt]( const job::RunContext& jobCtx )
			{
				m_disposalMap.Update( dt );
			} );
			builder.DispatchJob<job::Fence::None>( "TrafficSystem_ExpectedCollisions", [&, dt]( const job::RunContext& jobCtx )
			{
				m_expectedCollisionsMap.Update( dt );
			} );

			builder.DispatchFenceExplicitly();
		} );

		builder.DispatchJob( "TrafficSystem_Maintanance", [&]( const job::RunContext& jobCtx )
		{
			job::Builder builder( jobCtx );

			//evaluators have their own locks
			builder.DispatchJob<job::Fence::None>( "TrafficSystem_Evaluators", [&]( const job::RunContext& jobCtx )
			{
				m_evaluators.Update( *this );
			} );

			builder.DispatchJob<job::Fence::None>( "TrafficSystem_SpawnQueryCache", [&]( const job::RunContext& jobCtx )
			{
				m_spawnQueryCache.Update( *this );

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
				m_timeFrame = Config::cvInitTimeFrameForDebugLanesSpawn.Get();
				SanitizeSpawnDebugs();
#endif
			} );

			builder.DispatchFenceExplicitly();
		} );
	} );

}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterLaneToBeSlotPurgedNoLock( const TrafficLane& lane )
{
	m_lanesToPurge.Insert( lane.GetPersistentData().GetLaneId() );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterLaneToBeSlotPurged( const TrafficLaneUID& laneID )
{
	RED_SCOPE_LOCK( m_lock );
	m_lanesToPurge.Insert( laneID );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UpdateIntersections( const Float dt )
{
	PC_SCOPE( RuntimeSystemTraffic_UpdateIntersections );

	RED_SCOPE_LOCK( m_lock );

	red::DynArray< TrafficLaneUID > ids { job::PoolJobScope() };
	/* This number is extracted from the most common case scenario */
	ids.Reserve( 32 );
	for ( auto it : m_intersections )
	{
		it.Value()->Update( dt );

		if ( it.Value()->HaveLightsChanged() )
		{
			it.Value()->GetLanesWithTrafficLightChanges( ids );
		}
	}

	/* Register the lanes that need their collisions updated for the system */
	if ( !ids.Empty() )
	{
		m_lanesWithTrafficLights.Reserve( ids.Size() + m_lanesWithTrafficLights.Size() );
		for ( const TrafficLaneUID& id : ids )
		{
			m_lanesWithTrafficLights.Insert( id );
		}
	}
}


// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::OnSetup( RuntimeScene& scene, const RuntimeSceneSetupInfo& setupInfo, job::Counter& counter )
{
	m_navigationSystem = scene.GetSystem< RuntimeSystemNavigation >();

	m_trafficLaneEventsBroadcaster = red::CreateUniquePtr< TrafficLaneEventsBroadcaster, nav::PoolTrafficLanes >();
	m_collisionMap.SetEventsBroadcaster( *m_trafficLaneEventsBroadcaster );

	m_scriptInterface = CreateHandle< TrafficScriptInterface >( *this );

	RuntimeSystemPhysics* physicsSystem = scene.GetSystem< RuntimeSystemPhysics >();
	if ( physicsSystem )
	{
		physicsSystem->GetCollisionListenerRegistry().RegisterCollisionListener( &m_collisionListener );
	}

	m_greenWaveManager = red::CreateUniquePtr< TrafficGreenWaveManager >( *this );

#if !defined( RED_CONFIGURATION_FINAL ) && defined( RED_USE_RESOURCEPATH_STRINGS ) && !defined( NO_EDITOR )
    LoadStreamingWorld(scene, counter, &setupInfo);
#endif
}

// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::OnStreamingWorldLoaded( world::RuntimeScene& scene, job::Counter& counter )
{
	LoadStreamingWorld( scene, counter, nullptr );
}

// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::LoadStreamingWorld( world::RuntimeScene& scene, job::Counter& counter, const RuntimeSceneSetupInfo* setupInfo )
{
	auto tokens = CreateLoadResource( scene.GetSystem< world::RuntimeSystemWorldStreaming >(), setupInfo );

	job::Counter finishedCounter;
	job::Counter loadedCounter;

	if ( tokens.m_persToken ) loadedCounter += tokens.m_persToken->GetWaitCounter();
	if ( tokens.m_spatialResourceToken ) loadedCounter += tokens.m_spatialResourceToken->GetWaitCounter();
	if ( tokens.m_connectionsToken ) loadedCounter += tokens.m_connectionsToken->GetWaitCounter();
	if ( tokens.m_polygonsToken ) loadedCounter += tokens.m_polygonsToken->GetWaitCounter();
	if ( tokens.m_collisionsToken ) loadedCounter += tokens.m_collisionsToken->GetWaitCounter();
	if ( tokens.m_nullAreasToken ) loadedCounter += tokens.m_nullAreasToken->GetWaitCounter();

	// NOTE: For now just have one job which does the init after all the resources have loaded, see note below for how to do more async assigning of loaded values
	job::DispatchJob< nav::PoolTrafficNavigation >( "LoadTrafficResources/OnLoaded", loadedCounter, finishedCounter, [this, tokens]( const job::RunContext& )
	{
		if ( tokens.m_persToken &&( m_persistentResource = Cast< TrafficPersistentResource >( tokens.m_persToken->GetResource() ) ) )
		{
			OnPersistentSpatialResourceLoaded( Cast< world::TrafficPersistentSpatialResource >( tokens.m_spatialResourceToken->GetResource() ) );

			/* Reconstruct persistentResource to maintain backward compatibility */
			MergeTrafficPersistentSplitResource( Cast< world::TrafficPersistentLaneConnectionsResource >(  tokens.m_connectionsToken->GetResource() )
												, Cast< world::TrafficPersistentLanePolygonResource >(  tokens.m_polygonsToken->GetResource() ) );

			TrafficPersistentData& persData = m_persistentResource->GetData();
			RegisterPersistentData( &persData );
		}
		else
		{
			RED_LOG_CATEGORY_ERROR(red::LoggerCategory_Traffic,"Traffic persistent data not present. Regenerate traffic (Build and Play in Full mode).");
		}

		if ( tokens.m_collisionsToken && tokens.m_collisionsToken->IsLoaded() )
		{
			OnStaticCollisionsResourceLoaded( Cast< TrafficCollisionResource >( tokens.m_collisionsToken->GetResource() ) );
		}

		if ( tokens.m_nullAreasToken && tokens.m_nullAreasToken->IsLoaded() )
		{
			OnNullAreaResourceLoaded(  Cast< TrafficNullAreaCollisionResource >( tokens.m_nullAreasToken->GetResource() ) );
		}

		if ( m_persistentData == nullptr )
		{
			RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Traffic, "Traffic persistent data not present - traffic will not function properly. To fix, rebuild traffic data (Build and Play in Full mode)." );
		}

#ifndef NO_EDITOR
		const WeakHandle< RuntimeSystemTraffic > weakHandle = HandleFromThis< RuntimeSystemTraffic >();
		if ( tokens.m_persToken != nullptr && tokens.m_spatialResourceToken != nullptr &&  tokens.m_connectionsToken != nullptr &&  tokens.m_polygonsToken != nullptr )
		{
			SetupResourceMonitorsForTraffic( weakHandle, tokens.m_persToken, tokens.m_spatialResourceToken, tokens.m_connectionsToken, tokens.m_polygonsToken );
		}
		if ( tokens.m_collisionsToken != nullptr )
		{
			SetupResourceMonitorForStaticCollisions ( weakHandle, tokens.m_collisionsToken );
		}
		if ( tokens.m_nullAreasToken != nullptr )
		{
			SetupResourceMonitorForNullAreas( weakHandle, tokens.m_nullAreasToken );
		}
#endif // NO_EDITOR

	} );

	
	// NOTE: If you want to assign the loaded resources more asynchronously you can split them off using jobs like this:
	//job::DispatchJob< nav::PoolTrafficNavigation >( "LoadTrafficResources/StaticCollisionsLoaded", tokens->m_collisionsToken->GetWaitCounter(), finishedCounter, 
	//	[this, token = tokens->m_collisionsToken]( const job::RunContext& )
	//{
	//	OnStaticCollisionsResourceLoaded( token );
	//} );

#if defined( RED_USE_IMGUI ) && !defined( RED_CONFIGURATION_FINAL ) && defined( RED_USE_RESOURCEPATH_STRINGS )

	if ( Config::cvLoadCollisionDebugResource.Get() )
	{
		if ( auto streamingSystem = scene.GetSystem< world::RuntimeSystemWorldStreaming >() )
		{
			if ( auto streamingWorld = streamingSystem->GetStreamingWorld() )
			{
				const res::ResourcePath worldPath = streamingWorld->GetPath();
				LoadDebugResources( worldPath, finishedCounter );
			}
		}
	}

#endif

	counter += finishedCounter;
}

#ifndef NO_EDITOR

void RuntimeSystemTraffic::SetupResourceMonitorsForTraffic( WeakHandle< world::RuntimeSystemTraffic > trafficSystem, res::ResourceTokenHandle persToken, res::ResourceTokenHandle spatialResourceToken, res::ResourceTokenHandle connectionsToken, res::ResourceTokenHandle polygonsToken )
{
	RegisterMonitorForTrafficResource< world::TrafficPersistentResource >( trafficSystem, persToken->GetPath(), []( TrafficReloadedResources* resources, THandle< world::TrafficPersistentResource > resource )
	{
		resources->m_persistentResource = resource;
	});

	RegisterMonitorForTrafficResource< world::TrafficPersistentSpatialResource >( trafficSystem, spatialResourceToken->GetPath(), []( TrafficReloadedResources* resources, THandle< world::TrafficPersistentSpatialResource > resource )
	{
		resources->m_spatialResource = resource;
	});

	RegisterMonitorForTrafficResource< world::TrafficPersistentLaneConnectionsResource >( trafficSystem, connectionsToken->GetPath(), []( TrafficReloadedResources* resources, THandle< world::TrafficPersistentLaneConnectionsResource > resource )
	{
		resources->m_laneConnectionsResource = resource;
	});

	RegisterMonitorForTrafficResource< world::TrafficPersistentLanePolygonResource >( trafficSystem, polygonsToken->GetPath(), []( TrafficReloadedResources* resources, THandle< world::TrafficPersistentLanePolygonResource > resource )
	{
		resources->m_lanePolygonsResource = resource;
	});
}

void RuntimeSystemTraffic::RegisterReloadedResources()
{
	RED_ASSERT( m_reloadedResources.AreResourcesReady() );

	UnregisterPersistentData();

	m_persistentResource = m_reloadedResources.m_persistentResource;
	OnPersistentSpatialResourceLoaded( m_reloadedResources.m_spatialResource );

	/* Reconstruct persistentResource to maintain backward compatibility */
	MergeTrafficPersistentSplitResource( m_reloadedResources.m_laneConnectionsResource, m_reloadedResources.m_lanePolygonsResource );

	TrafficPersistentData& persData = m_persistentResource->GetData();
	RegisterPersistentData( &persData );
}

void RuntimeSystemTraffic::SetupResourceMonitorForStaticCollisions( WeakHandle< world::RuntimeSystemTraffic > trafficSystem, res::ResourceTokenHandle staticCollisionsToken )
{
	m_trafficCollisionsResourceMonitor = red::CreateUniquePtr< res::Monitor >();
	m_trafficCollisionsResourceMonitor->SetCallback( [ trafficSystem ]( const res::ResourcePath &path, const CName &message, const res::MonitorUserDataPtr &userData )
	{
		if ( message == RED_NAME_CONSTEXPR_NOREG( "reload" ) )
		{
			const THandle< world::RuntimeSystemTraffic > handle = trafficSystem.ToHandle();
			if ( handle )
			{
				handle->OnStaticCollisionsResourceLoaded( res::GetMonitorUserData< THandle< world::TrafficCollisionResource > >( userData ) );
			}
		}
	});
	m_trafficCollisionsResourceMonitor->Bind( staticCollisionsToken->GetPath() );
}

void RuntimeSystemTraffic::SetupResourceMonitorForNullAreas( WeakHandle< world::RuntimeSystemTraffic > trafficSystem, res::ResourceTokenHandle nullareasToken )
{
	m_nullAreasResource = red::CreateUniquePtr< res::Monitor >();
	m_nullAreasResource->SetCallback( [ trafficSystem ]( const res::ResourcePath &path, const CName &message, const res::MonitorUserDataPtr &userData )
	{
		if ( message == RED_NAME_CONSTEXPR_NOREG( "reload" ) )
		{
			const THandle< world::RuntimeSystemTraffic > handle = trafficSystem.ToHandle();
			if ( handle )
			{
				handle->OnNullAreaResourceLoaded( res::GetMonitorUserData< THandle< world::TrafficNullAreaCollisionResource > >( userData ) );
			}
		}
	});
	m_nullAreasResource->Bind( nullareasToken->GetPath() );
}

#endif // NO_EDITOR

void RuntimeSystemTraffic::OnPersistentSpatialResourceLoaded( THandle< TrafficPersistentSpatialResource > spatialResource )
{
	/* Lane Spatial Representations */
	if ( spatialResource != nullptr )
	{
		TrafficPersistentData& persData = m_persistentResource->GetData();
		persData.InitializeSpatialDataFromResource( std::move( spatialResource->ExtractNeighborGroups() ), 
			std::move( spatialResource->ExtractLanesLUT() ),
			std::move( spatialResource->ExtractLaneGrid() ),
			std::move( spatialResource->ExtractReachabilityData() ) );
	}
}

void RuntimeSystemTraffic::MergeTrafficPersistentSplitResource( THandle < TrafficPersistentLaneConnectionsResource > connectionsResource, THandle < TrafficPersistentLanePolygonResource > polygonsResource )
{
	/* Lane Connections, polygons and spots  */
	if ( connectionsResource && polygonsResource )
	{
		red::HashMap< TrafficLaneIndex, TrafficPersistentLaneConnections > connections = { nav::PoolTrafficPersistentLanes() };
		connections = std::move( connectionsResource->ExtractLaneConnections() );

		red::HashMap< TrafficLaneIndex, TrafficLanePolygonRepresentation > polygons = { nav::PoolTrafficPersistentLanes() };
		polygons = std::move( polygonsResource->ExtractLanePolygons() );

		TrafficPersistentData& persData = m_persistentResource->GetData();

		RED_ASSERT( connections.Size() == persData.GetLanes().Size(),
			"Mismatch between traffic connections resource (%u entries) and persistent resource (%u lanes).\n"
			"Please take a screenshot of your r6\\depot\\base\\worlds\\03_night_city\\sectors\\_generated\\traffic "
			"with file Size and Date modified columns enabled and report to CYB-601736.\n"
			"To fix the issue rebuild your traffic data.",
			connections.Size(),
			persData.GetLanes().Size() );
		RED_ASSERT( polygons.Size() == persData.GetLanes().Size(),
			"Mismatch between traffic polygons resource (%u entries) and persistent resource (%u lanes).\n"
			"Please take a screenshot of your r6\\depot\\base\\worlds\\03_night_city\\sectors\\_generated\\traffic "
			"with file Size and Date modified columns enabled and report to CYB-601736.\n"
			"To fix the issue rebuild your traffic data.",
			polygons.Size(),
			persData.GetLanes().Size() );

		persData.FillPersistentLanesFromResource( std::move( connections ), std::move( polygons ) );

		/* Prepare the persistent data */
		persData.InitializeSubGraphSizes();
		persData.CacheLaneReferences();
		persData.CalculateRoadLaneOrderInfos();
	}
}

void RuntimeSystemTraffic::OnStaticCollisionsResourceLoaded( THandle< TrafficCollisionResource > collisionsResource  )
{
	if ( collisionsResource != nullptr )
	{
		if ( THandle< TrafficStaticCollisionData > collision = collisionsResource->GetData() )
		{
			RED_SCOPE_LOCK( m_lock );
			m_persistentStaticCollisions.Clear();

			const red::DynArray< StaticLaneCollisions >& cols = collision->GetLaneCollisions();
			for ( const StaticLaneCollisions& col : cols )
			{
				m_persistentStaticCollisions.AddCollisions( std::move( col.m_collisions ), GlobalNodeID{ col.m_lane.m_nodeRefHash } );

				/* Setup the dead ends created by the static collisions */
				if ( m_persistentData && col.m_deadEndStart != std::numeric_limits< Float >::infinity() )
				{
					if ( const TrafficLanePersistent* lane = FindPersistentLane(col.m_lane) )
					{
						m_runtimeState.SetDeadEnd( *lane, col.m_deadEndStart );
						m_runtimeState.PropagateDeadEnds( *lane );
					}
				}
			}

			collision->Clear();
		}
		else
		{
			RED_LOG_CATEGORY_ERROR(red::LoggerCategory_Traffic, "Outdated static collision resource. Wait for the server to generate new one, or generate one yourself and Play (without Build).");
		}
	}
	else
	{
		RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Traffic, "Traffic collision data not present. Regenerate traffic (Build&Play in Full mode)." );
	}
}

void RuntimeSystemTraffic::OnNullAreaResourceLoaded( THandle< TrafficNullAreaCollisionResource > nullAreaResource )
{
	if ( nullAreaResource != nullptr )
	{
		if ( THandle< TrafficNullAreaCollisionData > nullAreas = nullAreaResource->GetTrafficNullAreasCollisionData() )
		{
			RED_SCOPE_LOCK( m_lock );
			
			m_nullAreaHeader = nullAreas->GetHeader();
			m_nullAreaMap.Clear();

			const red::DynArray< CrowdNullAreaCollisionData >& nullAreaCollisions = nullAreas->GetNullAreasCollisionData();
			for ( const CrowdNullAreaCollisionData &data : nullAreaCollisions )
			{
				/* This is here for backwards compatibility */
				if ( data.m_collisions.Empty() )
				{
					continue;
				}

				m_nullAreaMap.Insert( data.m_areaID, std::move( data.m_collisions ) );
			}
		}
		else
		{
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Traffic, "Outdated null areas resource. Wait for the server to generate new one, or generate one yourself and Play (without Build)." );
		}

		if ( THandle< TrafficNullAreaDynamicBlockadeData > nullAreasBlockades = nullAreaResource->GetTrafficNullAreasDynamicBlockadeData() )
		{
			RED_SCOPE_LOCK( m_lock );
			m_nullAreaBlockadeDataMap.Clear();

			const red::DynArray< TrafficNullAreaDynamicBlockade >& blockadesData = nullAreasBlockades->GetNullAreasBlockadesData();
			for ( const TrafficNullAreaDynamicBlockade& blockadeData : blockadesData )
			{
				m_nullAreaBlockadeDataMap.Insert( blockadeData.m_areaID, blockadeData );
			}
		}
	}
}

#if defined( RED_USE_IMGUI ) && defined( RED_USE_RESOURCEPATH_STRINGS )

struct TrafficSystemDebugResources
{
	RED_USE_MEMORY_POOL(nav::PoolTrafficNavigation);

	res::ResourceTokenHandle m_persToken;
	res::ResourceTokenHandle m_collisionsToken;
};

void RuntimeSystemTraffic::LoadDebugResources( const res::ResourcePath streamingWorldPath, job::Counter& waitCounter )
{
	job::Counter finishedCounter;
	job::Counter loadingCounter { job::Priority::Latent };

	const res::ResourcePath worldPath = world::ResolveWorldPath( streamingWorldPath.ToStringView() );

	auto debugTokens = red::CreateSharedPtr< TrafficSystemDebugResources >();

	const res::ResourcePath persistentDebugResourcePath = CalcTrafficPersistentDebugResourcePath( worldPath );
	debugTokens->m_persToken = GResourceLoader->IssueLoadingRequest( persistentDebugResourcePath );
	loadingCounter += debugTokens->m_persToken->GetWaitCounter();

	if ( Config::cvLoadCollisionDebugResource.Get() )
	{
		res::ResourcePath collisionDebugResourcePath = CalcTrafficCollisionDebugResourcePath( worldPath );
		debugTokens->m_collisionsToken = GResourceLoader->IssueLoadingRequest( collisionDebugResourcePath );
		loadingCounter += debugTokens->m_collisionsToken->GetWaitCounter();
	}

	job::DispatchJob< nav::PoolTrafficDebug >( "TrafficSystem_LoadDebugResources_Initialization", loadingCounter, finishedCounter, [this, debugTokens]( const job::RunContext& ctx )
	{
		if ( THandle< TrafficPersistentDebugResource > resource = Cast< TrafficPersistentDebugResource >( debugTokens->m_persToken->GetResource() ) )
		{
			if ( m_persistentData != nullptr )
			{
				m_persistentData->m_brokenUIDs = resource->ExtractBrokenUIDs();
				m_persistentData->m_brokenUIDsDeadEnds = resource->ExtractBrokenUIDsDeadEnds();
			}
		}

		if ( Config::cvLoadCollisionDebugResource.Get() )
		{
			if ( THandle< TrafficCollisionDebugResource > collisionDebugResource = Cast< TrafficCollisionDebugResource >( debugTokens->m_collisionsToken->GetResource() ) )
			{
				m_collisionDebug = collisionDebugResource->GetData();
			}
		}
	} );
}
#endif

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::Unload( world::RuntimeScene& scene )
{
	if ( m_persistentResource )
	{
		UnregisterPersistentData();
		m_persistentResource = nullptr;
	}

	if ( auto physicsSystem = scene.GetSystem< RuntimeSystemPhysics >() )
	{
		physicsSystem->GetCollisionListenerRegistry().UnregisterCollisionListener( &m_collisionListener );
	}

	m_greenWaveManager.Reset();
	m_trafficLaneEventsBroadcaster.Reset();
	m_deadEndDetector.Reset();

	m_registrationQueue.Clear();
	m_unregistrationQueue.Clear();
	m_nodeDump.Clear();

	m_nodeToLaneLUT.Clear();
	m_laneLUT.Clear();
	m_intersections.Clear();
	m_nodeUsage.Clear();
	m_lanesToPurge.Clear();
	m_lanesWithTrafficLights.Clear();

	m_persistentStaticCollisions.Clear();
	m_collisionMap.Clear();

	m_dynamicCollisionsMap.Clear();
	m_disposalMap.Clear();
	m_expectedCollisionsMap.Clear();

	m_staticCollisions.Clear();

	m_globalPathRequests.Clear();
	m_cancelledGlobalPathRequests.Clear();
	
	if ( m_currentGlobalPathRequest.IsValid() )
	{
		if ( m_pathFinder != nullptr )
		{
            m_pathFinder->CancelPathfinding();
		}
		m_currentGlobalPathRequest.Reset();
	}

	m_globalPathResults.Clear();

	m_spawnRequests.Clear();
	m_scriptInterface.Reset();

	m_spawnQueryCache.Clear();
	m_laneLookup.Clear();
	m_evaluators.Clear();

	m_runtimeState.Clear();

#ifndef RED_CONFIGURATION_FINAL
	m_collisionDebug.Reset();
#endif

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	{
		RED_SCOPE_LOCK( m_debugLock );
		m_invalidSpawnRequests.Clear();
		m_validSpawnRequests.Clear();
		m_spawnResultDebug.Clear();
		m_disposalRequests.Clear();
	}
#endif
	
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::OnPostWorldDetached( RuntimeScene& scene )
{
	Unload( scene );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::OnUninitialize( RuntimeScene& scene )
{
	Unload( scene );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::GenerateNullAreaCollisions( Uint64 nullAreaId, red::DynArray< DynamicCollisionRepresentation::CollisionSphere > &collisions ) const
{
	PC_SCOPE( RuntimeSystemTraffic_GenerateNullAreaCollisions );

	collisions.Clear();

	const red::DynArray< TrafficStaticCollisionSphere >* nullAreaMapEntry = m_nullAreaMap.FindPtr( nullAreaId );
	if ( nullAreaMapEntry == nullptr )
	{
		return;
	}

	const red::DynArray< TrafficStaticCollisionSphere >& keyCollisionPositions = *nullAreaMapEntry;
	const Uint32 sphereCount = keyCollisionPositions.Size();

	/* Difficult to reserve the exact quantity here */
	collisions.Reserve( sphereCount * 3 );

	constexpr Float horizontalStepRate = 1.f;
	for (Uint32 i = 0; i < sphereCount - 1; i+=2)
	{
		const Vector3 initialPosition = keyCollisionPositions[ i ].m_worldPos;
		const Vector3 finalPosition = keyCollisionPositions[ i + 1 ].m_worldPos;
		const Float distance = ( finalPosition - initialPosition ).Mag();
		const Vector3 directionNormalized = ( finalPosition - initialPosition ).Normalized();

		const Uint32 horizontalSteps = static_cast< Uint32 >( std::ceil( distance / horizontalStepRate ) );
		for ( Uint32 j = 0; j < horizontalSteps; ++j )
		{
			collisions.EmplaceBack( Sphere( initialPosition + ( directionNormalized * static_cast< Float >( j ) ), m_nullAreaHeader.m_radius ) );
		}

		collisions.EmplaceBack( Sphere( finalPosition, m_nullAreaHeader.m_radius ) );
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::ShouldApplyBlockadeChanges( Uint64 nullAreaId ) const
{
	return m_nullAreaBlockadeDataMap.Find( nullAreaId ) != m_nullAreaBlockadeDataMap.End();
}

void RuntimeSystemTraffic::ApplyNavmeshBlockadeChangesOnNullAreaEnabled( Uint64 nullAreaId )
{
	if ( const TrafficNullAreaDynamicBlockade* blockade = m_nullAreaBlockadeDataMap.FindPtr( nullAreaId ) )
	{
		// Apply offmesh link changes in navigation system
		for ( OffMeshConnectionID id : blockade->m_offmeshLinks )
		{
			m_navigationSystem->DisableOffMeshFlags( id, NavGen::AgentSize::Human , nav::NavPathFlags::Open | nav::NavPathFlags::AvailableToPlayer );
		}
	}
}

void RuntimeSystemTraffic::ApplyNavmeshBlockadeChangesOnNullAreaDisabled( Uint64 nullAreaId )
{
	if ( const TrafficNullAreaDynamicBlockade* blockade = m_nullAreaBlockadeDataMap.FindPtr( nullAreaId ) )
	{
		// Apply offmesh link changes in navigation system
		for ( OffMeshConnectionID id : blockade->m_offmeshLinks )
		{
			m_navigationSystem->EnableOffMeshFlags( id, NavGen::AgentSize::Human , nav::NavPathFlags::Open | nav::NavPathFlags::AvailableToPlayer );
		}
	}
}

void RuntimeSystemTraffic::ApplyFlagsToBlockedTrafficLanes( Uint64 nullAreaId )
{
	if ( const TrafficNullAreaDynamicBlockade* blockade = m_nullAreaBlockadeDataMap.FindPtr( nullAreaId ) )
	{
		m_persistentData->ApplyBlockadeChangesInLanesForGPS( blockade->m_affectedTrafficLanes, true );
	}
}

void RuntimeSystemTraffic::RemoveFlagsFromBlockedTrafficLanes( Uint64 nullAreaId )
{
	if ( const TrafficNullAreaDynamicBlockade* blockade = m_nullAreaBlockadeDataMap.FindPtr( nullAreaId ) )
	{
		m_persistentData->ApplyBlockadeChangesInLanesForGPS( blockade->m_affectedTrafficLanes, false );
	}
}

// --------------------------------------------------------------------------------

TrafficSlotPtr RuntimeSystemTraffic::RequestSingleSlotOnLane( TrafficLaneUID laneID, TrafficSpawning::SpawnDimensions dimensions, TrafficMovementBehavior bhv, const Vector3& worldPosition, const TrafficOutLaneFilter& outLaneFilter, const TrafficPreferredOutLaneFilter& preferredFilter )
{
	PC_SCOPE( RuntimeSystemTraffic_RequestSingleSlotOnLane_1D );

	if ( const TrafficLanePersistent* lanePersistent = FindPersistentLane( laneID ) )
	{
		GenerateGlobalPathContext context( lanePersistent->GetLaneId(), outLaneFilter, preferredFilter, bhv, 0.f );
		if ( auto globalPath = GenerateRandomGlobalPath( context ) )
		{
			return RequestSingleSlotOnLane( laneID, dimensions, bhv, worldPosition, *globalPath );
		}
	}

	return nullptr;
}

// --------------------------------------------------------------------------------

TrafficSlotPtr RuntimeSystemTraffic::RequestSingleSlotOnLane( TrafficLaneUID laneID, Float slotRadius, TrafficMovementBehavior bhv, const Vector3& worldPosition, const Vector3& direction, const world::TrafficGlobalPath& globalPath, const world::TrafficLocalPath& localPath )
{
	PC_SCOPE( RuntimeSystemTraffic_RequestSingleSlotOnLane_1D_Pedestrian );

	RED_ASSERT( bhv == TrafficMovementBehavior::Pedestrian );

	if ( auto slot = CreateSlot( globalPath, localPath, worldPosition, direction, slotRadius, bhv, *this ) )
	{
		RED_SCOPE_LOCK( m_lock );
		if ( auto lane = FindLane( laneID ) )
		{
			lane->AddSlot( slot );
			m_slotLUT.Emplace( slot->GetId(), slot );
		}

		RED_ASSERT( std::isfinite( slot->movementState.position.X ) && std::isfinite( slot->movementState.position.Y ) && std::isfinite( slot->movementState.position.Z ) );
		return slot;
	}

	return nullptr;
}

//================================================================================

TrafficSlotPtr RuntimeSystemTraffic::RequestSingleSlotOnLane( TrafficLaneUID laneID, TrafficSpawning::SpawnDimensions dimensions, TrafficMovementBehavior bhv, const Vector3& worldPosition, const world::TrafficGlobalPath& globalPath )
{
	PC_SCOPE( RuntimeSystemTraffic_RequestSingleSlotOnLane_1D_Vehicle );

	if ( auto slot = CreateSlot( globalPath, worldPosition, dimensions, bhv, *this ) )
	{
		RED_SCOPE_LOCK( m_lock );
		if ( auto lane = FindLane( laneID ) )
		{
			lane->AddSlot( slot );
		}

		RED_ASSERT( std::isfinite( slot->movementState.position.X ) && std::isfinite( slot->movementState.position.Y ) && std::isfinite( slot->movementState.position.Z ) );
		return slot;
	}

	return nullptr;
}


//================================================================================
TrafficSlotPtr RuntimeSystemTraffic::RequestSlotOnLane( const TrafficSpawning::SpawnQuery& spawnRequest )
{
	RED_SCOPE_LOCK( m_spawnLock ); //we need to lock down this section - we cannot have concurrent spawn requests since they may end up on the same lane, thus leading to overlaps

	//how to paralelize this - receive a batch of requests that can be split into different ones based on the lanes

	if ( Config::cvDisableSpawnRequests.Get() ) return nullptr;

	auto spawnResult = RequestSpawn( spawnRequest );

	if ( spawnResult.Valid() )
	{
		if ( auto lanePersistent = FindPersistentLane( spawnRequest.m_laneId ) )
		{
			if ( lanePersistent->IsDeadEnd() ) return nullptr; //we do not spawn on dead ends

			auto globalPath = GenerateRandomGlobalPath( GenerateGlobalPathContext( spawnRequest.m_laneId, spawnRequest.m_outLaneFilter, spawnRequest.m_preferredFilter, spawnRequest.m_behavior, 0.0f ) );

			if ( !globalPath ) return nullptr;

			if ( auto slot = CreateSlot( *globalPath, spawnResult.m_worldPosition, spawnRequest.m_dimensions, spawnRequest.m_behavior, *this ) )
			{
				RED_SCOPE_LOCK( m_lock );

				if ( auto lane = FindLane( spawnRequest.m_laneId ) )
				{
					lane->AddSlot( slot );

					m_slotLUT.Emplace( slot->GetId(), slot );
				}

				return slot;
			}
		}
	}

	return nullptr;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::OnNullAreaActivated( Uint64 areaId )
{
	red::DynArray< DynamicCollisionRepresentation::CollisionSphere > collisions { nav::PoolTrafficNullAreas() };
	GenerateNullAreaCollisions( areaId, collisions );

	if ( !collisions.Empty() )
	{
		auto representation = m_staticCollisions.CreateRepresentation( [&]( auto& representation )
		{
			representation.m_id = areaId;
			representation.m_type = decltype( representation.m_type )::NullArea;

			representation.AddSet( collisions );

		}, nav::PoolTrafficNullAreas() );

		representation->Shrink();

		m_staticCollisions.Add( areaId, representation
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
								, []( auto& ){}, String::Printf( "Null %llu", areaId )
#endif
		);

		m_disposalMap.Add( areaId, representation
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
								, []( auto& )
		{}, String::Printf( "Null %llu", areaId )
#endif
		);
	}

	m_nullAreaHandler.OnNullAreaActivated( areaId );

	if ( ShouldApplyBlockadeChanges( areaId ) )
	{
		ApplyNavmeshBlockadeChangesOnNullAreaEnabled( areaId );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::OnNullAreaDeactivated( Uint64 areaId )
{
	m_staticCollisions.Remove( areaId );
	m_disposalMap.Remove( areaId );

	m_nullAreaHandler.OnNullAreaDeActivated( areaId );

	if ( ShouldApplyBlockadeChanges( areaId ) )
	{
		ApplyNavmeshBlockadeChangesOnNullAreaDisabled( areaId );
	}
}

Bool RuntimeSystemTraffic::GenerateTrafficPath( const GenerateGlobalPathContext& context, GenerateGlobalPathResult& result ) const
{
	PC_SCOPE( RuntimeSystemTraffic_GenerateGlobalPath );

	if ( !m_persistentData || !m_persistentData->IsDataInitialized() )
	{
		return false;
	}

	const TrafficLanePersistent* startLanePersistent = m_persistentData->FindLane( context.m_startLaneId );
	const TrafficLanePersistent* pickedLane = nullptr;
	const TrafficConnectivityOutLane* pickedOutlane = nullptr;
	RED_ASSERT( startLanePersistent );

	red::DynArray< world::TrafficGlobalPath::Lane > globalPath{ job::PoolJobScope() };
	red::DynArray< TrafficConnectivityOutLane > candidateOutLanes{ job::PoolJobScope() };
	red::DynArray< TrafficConnectivityOutLane > preferredOutLanes{ job::PoolJobScope() };
	globalPath.Reserve( Config::cvMaxPathLanes.Get() );

	// We assume the first lane is valid.
	globalPath.PushBack( context.m_startLaneId );
	globalPath.Back().m_entryPosition = context.m_initialEntryPosition;
	globalPath.Back().m_flags = startLanePersistent->m_flags;

	const TrafficLanePersistent* currentLane = startLanePersistent;

	const Uint32 maxPathLanes = static_cast< Uint32 >( Config::cvMaxPathLanes.Get() );
	Float totalLength = 0.f;
	while( true )
	{
		globalPath.Back().m_exitPosition = currentLane->m_length;

		candidateOutLanes.Clear();
		candidateOutLanes.Reserve( currentLane->m_outLanes.Size() );
		candidateOutLanes.PushBack( currentLane->m_outLanes );

		red::alg::EraseIf( candidateOutLanes, [ & ]( const TrafficConnectivityOutLane& candidateOutLane )
		{
			const TrafficLanePersistent& destinationLane = m_persistentData->GetLane( candidateOutLane.laneIndex );
			const TrafficLaneUID destinationLaneId = destinationLane.GetLaneId();

			if ( !IsLaneValidForUse( destinationLaneId ) )
				return true;

			if ( destinationLane.HasFlag( TrafficLanePersistent::Flags::TrafficDisabled ) )
				return true;

			// Eliminate the scenario where a lane is left and its reverse is visited again immediately.
			if ( globalPath.Size() > 1 && globalPath[ globalPath.Size() - 2 ].m_laneId == destinationLaneId.GetReverse() )
				return true;

			const Float exitEntryDist = ( candidateOutLane.thisLaneExitPosition - globalPath.Back().m_entryPosition );
			if ( math::AlmostEqualsOrSmaller( exitEntryDist, 0.f ) )
				return true;

			if ( context.m_outLaneFilter && context.m_outLaneFilter( *currentLane, candidateOutLane, this ) == TrafficFilterOperation::Remove )
				return true;

			if ( DoesLaneIntersectWithAreas( destinationLane, context.m_nullAreas, candidateOutLane.nextLaneEntryPosition ) )
				return true;

			if ( std::find_if( globalPath.Begin(), globalPath.End(), [&]( const auto& lane ) { return lane.m_laneId == destinationLaneId; } ) != globalPath.End() )
				return true;

			return false;
		} );

		if ( candidateOutLanes.Size() > 1u )
		{
			preferredOutLanes.Clear();
			preferredOutLanes.Reserve( candidateOutLanes.Size() );
			preferredOutLanes.PushBack( candidateOutLanes );

			red::alg::EraseIf( preferredOutLanes, [&]( const TrafficConnectivityOutLane& preferredOutLane )
			{
				if ( preferredOutLane.isSharpAngle )
					return true;

				if ( context.m_preferedOutlanesFilter && ( context.m_preferedOutlanesFilter( *currentLane, preferredOutLane, this ) == TrafficFilterOperation::Remove ) )
					return true;

				const TrafficLanePersistent& destinationLane = m_persistentData->GetLane( preferredOutLane.laneIndex );
				if ( !EnsureLaneSlotCapacity( destinationLane.GetLaneId(), TrafficMovementBehavior::Pedestrian ) )
					return true;

				return false;
			} );

			if ( !preferredOutLanes.Empty() && preferredOutLanes.Size() != candidateOutLanes.Size() )
			{
				candidateOutLanes = preferredOutLanes;
			}
		}

		if ( !candidateOutLanes.Empty() )
		{
			pickedOutlane = &PickRandomOutLane( candidateOutLanes );
			pickedLane = &m_persistentData->GetLane( pickedOutlane->laneIndex );

			globalPath.Back().m_exitPosition = pickedOutlane->thisLaneExitPosition;
		}

		const Float lanePortionLength = globalPath.Back().m_exitPosition - globalPath.Back().m_entryPosition;
		totalLength += lanePortionLength;

		const Bool hasReachedMaxLength = totalLength >= GenerateGlobalPathContext::s_maximumPedestrianPathLength;
		if ( hasReachedMaxLength )
		{
			const Float diff = lanePortionLength - ( totalLength - GenerateGlobalPathContext::s_maximumPedestrianPathLength );
			globalPath.Back().m_exitPosition = globalPath.Back().m_entryPosition + diff;

			totalLength = GenerateGlobalPathContext::s_maximumPedestrianPathLength;
		}

		if ( hasReachedMaxLength || candidateOutLanes.Empty() || globalPath.Size() == maxPathLanes )
		{
			break;
		}

		globalPath.EmplaceBack( pickedLane->GetLaneId(), 0.0f, pickedOutlane->nextLaneEntryPosition );
		globalPath.Back().m_flags = pickedLane->m_flags;

		currentLane = pickedLane;
	}

	result.m_globalPath = red::CreateUniquePtr< TrafficGlobalPath >();
	for ( const TrafficGlobalPath::Lane& lane : globalPath )
	{
		const TrafficLanePersistent* lp = FindPersistentLane( lane.m_laneId );
		result.m_globalPath->AddLane( lane, lp->m_outline );
	}

	result.m_length = totalLength;
	result.m_localPath = GenerateLocalPathFromGlobalPath( *result.m_globalPath );
	return true;
}

// --------------------------------------------------------------------------------

TrafficGlobalPathPtr RuntimeSystemTraffic::GenerateRandomGlobalPath( const GenerateGlobalPathContext& context ) const
{
	PC_SCOPE( RuntimeSystemTraffic_GenerateRandomGlobalPath );

	if ( m_persistentData == nullptr )
	{
		return nullptr;
	}

	if ( !m_persistentData->IsDataInitialized() )
	{
		return nullptr;
	}


	const TrafficLanePersistent* startLanePersistent = m_persistentData->FindLane( context.m_startLaneId );
	RED_ASSERT( startLanePersistent );

	red::DynArray< world::TrafficGlobalPath::Lane > lanesToAdd{ job::PoolJobScope() };
	lanesToAdd.Reserve( Config::cvMaxPathLanes.Get() );
	lanesToAdd.PushBack( context.m_startLaneId );
	lanesToAdd.Back().m_entryPosition = context.m_initialEntryPosition;
	lanesToAdd.Back().m_flags = startLanePersistent->m_flags;

	red::DynArray< TrafficConnectivityOutLane > outLanes{ job::PoolJobScope() };
	red::DynArray< TrafficConnectivityOutLane > preferedLanes{ job::PoolJobScope() };

	red::DynArray< TrafficConnectivityOutLane > lastLaneFiltering{ job::PoolJobScope() };

	const TrafficLanePersistent* currentLane = m_persistentData->FindLane( context.m_startLaneId );
	RED_ASSERT( currentLane );

	//NOTE
	//TODO - add a check for dead end here - however since we can join even on places that are deadends due to quests etc, it would require a bit more rework

	auto maxPathLanes = static_cast<Uint32>( Config::cvMaxPathLanes.Get() );
	auto aggregatedLength = 0.0f;

	while ( true )
	{
		aggregatedLength += currentLane->GetLength();

		if ( currentLane->m_outLanes.Empty() )
		{
			break;
		}

		
		red::DynArray< TrafficConnectivityOutLane >& filteredOutLanes = GetFilteredOutLanes( outLanes
																							  , preferedLanes
																							  , currentLane->m_outLanes
																							  , *currentLane
																							  , lanesToAdd
																							  , context.m_outLaneFilter
																							  , context.m_preferedOutlanesFilter
																							  , *this
																							  , context.m_nullAreas );
		
		if ( lanesToAdd.Size() == maxPathLanes && math::AlmostEqualsOrGreater( aggregatedLength, context.m_minimumLength ) ) //we need to filter what comes next
		{
			lastLaneFiltering = filteredOutLanes;
			break;
		}

		//we cannot have loops
		filteredOutLanes.Remove( std::remove_if( filteredOutLanes.Begin(), filteredOutLanes.End(), [&]( auto& connection )
		{
			auto it = std::find_if( lanesToAdd.Begin(), lanesToAdd.End(), [&]( auto& value )
			{
				return m_persistentData->GetLaneUID( connection.laneIndex ) == value.m_laneId;
			} );
			return it != lanesToAdd.End();
		}), filteredOutLanes.End() );

		if ( filteredOutLanes.Empty() )
		{
			RED_LOG( "No more lanes to find, should not happen" );
			break;
		}

		const TrafficConnectivityOutLane& pickedOutLane = PickRandomOutLane( filteredOutLanes );
		const TrafficLanePersistent& pickedLane = m_persistentData->GetLane( pickedOutLane.laneIndex );

		lanesToAdd.Back().m_exitPosition = pickedOutLane.thisLaneExitPosition;
		lanesToAdd.EmplaceBack( pickedLane.GetLaneId(), 0.0f, pickedOutLane.nextLaneEntryPosition );
		lanesToAdd.Back().m_flags = pickedLane.m_flags;

		currentLane = &pickedLane;
	}

	TrafficGlobalPathPtr result = red::CreateUniquePtr< TrafficGlobalPath >();

	// Last added lane needs to have an exit position corresponding to first outLane, otherwise path will possibly require backtracking.
	Float lastExitPosition = std::numeric_limits< Float >::infinity();

	//we need to apply filtering here

	for ( const TrafficConnectivityOutLane &ol : lastLaneFiltering )
	{
		if ( ol.thisLaneExitPosition > lanesToAdd.Back().m_entryPosition )
		{
			lastExitPosition = math::Min( lastExitPosition, ol.thisLaneExitPosition );
		}
	}

	lanesToAdd.Back().m_exitPosition = std::isfinite( lastExitPosition ) ? lastExitPosition : 0.0f;

	for ( const TrafficGlobalPath::Lane& lane : lanesToAdd )
	{
		const TrafficLanePersistent* lp = FindPersistentLane( lane.m_laneId );
		result->AddLane( lane, lp->m_outline );
	}

	return result;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::CalcStartLocalPathLane( const TrafficSlot& slot, Vector2& outLanePos, const TrafficLanePersistent*& outLaneData ) const
{
	TrafficGlobalPathProgression prog = slot.GetGlobalPathProgression();
	const TrafficGlobalPath::Lane* startLane = prog.GetCurrentLane();

	TrafficLaneUID id = startLane->m_laneId;
	if ( !id.IsValid() ) return false;

	Bool pathContinuation = !slot.localPath.IsEmpty();

	const TrafficLanePersistent *lane = FindPersistentLane( id );
	if ( !lane ) return false;

	// note[andrzej.drabarek]: This doesn't need to be a Vector2 anymore - all lane positions are always with offset 0. Offset is now handled on movement level.
	Vector2 lanePos;

	if ( pathContinuation )
	{
		lanePos = slot.GetLastPathLanePos();
	}
	else
	{
		Vector4 startPosition = slot.movementState.position;
		lanePos = Vector2( CalcSplineProjectionX<Vector3>( lane->m_outline, startPosition ), 0.0f );

		lanePos.X = Clamp( lanePos.X, prog.GetCurrentLane()->m_entryPosition, lane->m_length );
	}

	// [CYB-230178] clamp pose on Y for pedestrian regarding current lane
	if ( slot.movementState.behav == TrafficMovementBehavior::Pedestrian )
	{
		Float margin = Min( 1.0f, lane->m_width * 0.25f );
		lanePos.Y = Clamp( lanePos.Y, -lane->m_width * 0.5f + margin, lane->m_width * 0.5f - margin );
	}
	else // @GSO: remove move around Y position for vehicles for now...
	{
		lanePos.Y = 0.0f;
	}

	outLanePos = lanePos;
	outLaneData = lane;

	return true;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::CalcNextLocalPathLane( TrafficSlot& slot, Vector2& inOutLanePos, const TrafficLanePersistent& prevDef, const TrafficLanePersistent*& outLaneDef ) const
{
	TrafficLaneUID nextLaneId = slot.GetCurrentLocalPathLane();
	if ( !nextLaneId.IsValid() )
		return false;

	const TrafficLanePersistent *lane = FindPersistentLane( nextLaneId );
	if ( lane == nullptr )
		return false;

	RED_ASSERT( !math::EqualsZero( prevDef.m_width ) );
	inOutLanePos.Y = ( inOutLanePos.Y / prevDef.m_width ) * lane->m_width;

	inOutLanePos.X = slot.GetGlobalPathProgression().GetCurrentLane()->m_entryPosition;

	slot.SetLastPathLanePos( inOutLanePos );

	outLaneDef = lane;

	return true;
}

// --------------------------------------------------------------------------------

TrafficLocalPathPtr RuntimeSystemTraffic::TryToGetLocalPath( TrafficSlot& slot ) const
{
	if ( slot.GetGlobalPath().IsEmpty() ) return nullptr;

	TrafficLocalPathPtr res = red::CreateUniquePtr< world::TrafficLocalPath >();
	res->SetEdgeLimit( slot.GetAvoidanceSettings()->baseRadius );
	res->SetAutoBalance( true );

	TrafficGlobalPathProgression& pathProg = slot.GetGlobalPathProgression();

	const TrafficGlobalPath& globalPath = slot.GetGlobalPath();
	if ( !res->FromGlobalPath( globalPath, pathProg, *this ) )
	{
		res = nullptr;
	}

	return res;
}

// --------------------------------------------------------------------------------

TrafficLocalPathPtr RuntimeSystemTraffic::GenerateLocalPathFromGlobalPath( const TrafficGlobalPath& globalPath ) const
{
	PC_SCOPE( RuntimeSystemTraffic_GenerateLocalPathFromGlobalPath );

	RED_ASSERT( !globalPath.IsEmpty() );
	TrafficLocalPathPtr res = red::CreateUniquePtr< world::TrafficLocalPath >();

	res->FromGlobalPath( globalPath, *this );
	return res;
}

// --------------------------------------------------------------------------------

TrafficGlobalPathToken RuntimeSystemTraffic::FindGlobalPath( const TrafficGlobalPathQuery& query )
{
	TrafficGlobalPathToken token = m_globalPathTokenCounter.Increment();

	{
		RED_SCOPE_LOCK( m_lock );

		if ( m_globalPathRequests.Full() )
		{
			return s_invalidTrafficGlobalPathToken;
		}

		m_globalPathRequests.EmplaceBack( token, query );
	}

	return token;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::CancelFindGlobalPath( TrafficGlobalPathToken token )
{
	RED_SCOPE_LOCK( m_lock );
	m_cancelledGlobalPathRequests.Insert( token );
}

// --------------------------------------------------------------------------------

TrafficGlobalPathPtr RuntimeSystemTraffic::TryToGetGlobalPath( TrafficGlobalPathToken token, TrafficPathfindingInfo* info )
{
	TrafficGlobalPathPtr result{ nullptr };

	{
		RED_SCOPE_LOCK( m_lock );

		auto it = m_globalPathResults.Find( token );
		if ( it != m_globalPathResults.End() )
		{
			result = std::move( it.Value().path );
			if ( info )
			{
				*info = it.Value().info;
			}
			m_globalPathResults.Remove( token );
		}
	}

	return result;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::ProcessFindGlobalPathRequests()
{
	PC_SCOPE( ProcessFindGlobalPathRequests );
    if ( m_pathFinder == nullptr )
    {
        return;
    }


	red::StopWatch timer;
	Uint32 numHandledRequests = 0;
	while ( m_currentGlobalPathRequest.IsValid() ||
			numHandledRequests < m_globalPathRequests.Size() )
	{
		// Do we have any CPU time budget left?

		const Float timeLeftInMilliSeconds = c_maxGlobalPathRequestsTimeBudgetInMilliSeconds - static_cast< Float >( timer.GetDeltaMS() );
		if ( timeLeftInMilliSeconds <= 0.0f )
		{
			break;
		}

		// Grab next request

		if ( !m_currentGlobalPathRequest.IsValid() )
		{
			RED_ASSERT( !m_pathFinder->IsPathfindingInProgress() );
			m_currentGlobalPathRequest = m_globalPathRequests[ numHandledRequests++ ];
		}

		// Is it cancelled? - remove it

		if ( m_cancelledGlobalPathRequests.Exist( m_currentGlobalPathRequest.token ) )
		{
			if ( m_pathFinder->IsPathfindingInProgress() )
			{
                m_pathFinder->CancelPathfinding();
			}
			m_cancelledGlobalPathRequests.Remove( m_currentGlobalPathRequest.token );
			m_currentGlobalPathRequest.Reset();
			continue;
		}

		// Start or resume current query

		Bool isQueryDone = false;
		Result result;
		if ( !m_pathFinder->IsPathfindingInProgress() )
		{
			if ( !StartFindGlobalPathNoLock( m_currentGlobalPathRequest ) )
			{
				isQueryDone = true;
				result.info.m_numVisitedNodes = 0;
				result.info.m_queryDurationInMicroSeconds = 0;
			}
		}
		if ( !isQueryDone )
		{
			isQueryDone = ContinueFindGlobalPathNoLock( m_currentGlobalPathRequest, timeLeftInMilliSeconds, result );
		}

		if ( !isQueryDone )
		{
			break; // The only possible reason it's not finished is it didn't fit in time limit - query will be resumed
		}

		if ( !result.path ) // TODO: Remove creating dummy path - requires fixing up all AI/GPS/other use cases that expect valid path when query is done
		{
			result.path = red::CreateUniquePtr< TrafficGlobalPath >();
		}

		// Store the result
		
		m_globalPathResults.Emplace( m_currentGlobalPathRequest.token, std::move( result ) );
		m_currentGlobalPathRequest.Reset();
	}

	// Batch remove processed requests

	if ( numHandledRequests > 0 )
	{
		m_globalPathRequests.RemoveAt( 0, numHandledRequests );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::FindNeighborLanes( const TrafficLaneUID& refLaneId, const Bool searchOnlyReversed, const TrafficLaneFilter& laneFilter, red::DynArray< const TrafficLanePersistent* >& result ) const
{
	if ( !m_persistentData )
	{
		return;
	}

	if ( const TrafficLanePersistent* refLane = m_persistentData->FindLane( refLaneId ) )
	{
		return FindNeighborLanes( *refLane, searchOnlyReversed, laneFilter, result );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::FindNeighborLanes( const TrafficLanePersistent& refLane, const Bool searchOnlyOppositeDirection, const TrafficLaneFilter& laneFilter, red::DynArray< const TrafficLanePersistent* >& result ) const
{
	const red::DynArray< TrafficLaneIndex >* neighborIDs = m_persistentData->GetNeighbors( refLane );
	if ( !neighborIDs )
	{
		return;
	}

	const TrafficLaneIndex refLaneIndex = m_persistentData->GetLaneIndex( refLane );
	result.Reserve( result.Size() + neighborIDs->Size() );
	for ( const TrafficLaneIndex index : *neighborIDs )
	{
		if ( index == refLaneIndex )
		{
			continue;
		}

		const TrafficLanePersistent& lane = m_persistentData->GetLane( index );
		if ( searchOnlyOppositeDirection && lane.m_isReversed == refLane.m_isReversed )
		{
			continue;
		}

		if ( laneFilter( lane ) )
		{
			result.PushBack( &lane );
		}
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::StartFindGlobalPathNoLock( const GlobalPathRequest& req ) const
{
	PC_SCOPE( ProcessFindGlobalPathInternal );

	if ( !m_persistentData )
	{
		return false;
	}

	// Find nearest start and end nodes

	FindNearestLaneResult nearestStartLaneResult = FindNearestLane( req.query.start, req.query.laneFilter );
	if ( !nearestStartLaneResult.lane )
	{
		return false;
	}

	FindNearestLaneResult nearestEndLaneResult = FindNearestLane( req.query.end, req.query.laneFilter );
	if( !nearestEndLaneResult.lane )
	{
		return false;
	}

	// Lane that is part of a larger subgraph is used as a reference
	// The other one, unless contained in the same subgraph, has to be found again
	// Note: Here we take advantage of subgraph id feature: the larger the id, the more (or equal) number of lanes it contains

	TrafficSubGraphType subGraphType = TrafficSubGraphType::Regular;
	switch ( req.query.searchMode )
	{
		case traffic::PathfindingSearchMode::GPSPlayerOnFoot:
		case traffic::PathfindingSearchMode::GPSPlayerInVehicle:
			subGraphType = TrafficSubGraphType::PlayerGPS;
			break;
	}

	if ( nearestStartLaneResult.lane->GetSubGraphId( subGraphType ) < nearestEndLaneResult.lane->GetSubGraphId( subGraphType ) )
	{
		const TrafficLaneFilter startLaneFilter = nearestEndLaneResult.lane->CreateMatchSubgraphFilter( subGraphType, req.query.laneFilter );
		nearestStartLaneResult = FindNearestLane( req.query.start, startLaneFilter );
		if ( !nearestStartLaneResult.lane )
		{
			return false;
		}
	}
	else if ( nearestStartLaneResult.lane->GetSubGraphId( subGraphType ) > nearestEndLaneResult.lane->GetSubGraphId( subGraphType ) )
	{
		const TrafficLaneFilter endLaneFilter = nearestStartLaneResult.lane->CreateMatchSubgraphFilter( subGraphType, req.query.laneFilter );
		nearestEndLaneResult = FindNearestLane( req.query.end, endLaneFilter );
		if ( !nearestEndLaneResult.lane )
		{
			return false;
		}
	}

	// Build start and end nodes array

	red::DynArray< const TrafficLanePersistent* > startLanes{ red::PoolFrame() };
	startLanes.PushBack( nearestStartLaneResult.lane );
	if ( req.query.tryNeighborLanesForStart )
	{
		FindNeighborLanes( *nearestStartLaneResult.lane, false, req.query.laneFilter, startLanes );
		if ( const TrafficLanePersistent* revStartLane = FindPersistentLane( nearestStartLaneResult.lane->GetLaneId().GetReverse() ) )
		{
			startLanes.PushBack( revStartLane );
		}
	}

	red::DynArray< const TrafficLanePersistent* > endLanes{red::PoolFrame()};
	endLanes.PushBack( nearestEndLaneResult.lane );
	if( req.query.tryNeighborLanesForEnd )
	{
		FindNeighborLanes( *nearestEndLaneResult.lane, false, req.query.laneFilter, endLanes );
		if( const TrafficLanePersistent* revEndLane = FindPersistentLane( nearestEndLaneResult.lane->GetLaneId().GetReverse() ) )
		{
			endLanes.PushBack( revEndLane );
		}
	}

	// Add lanes from nearest surrounding of the start and end lanes

	red::DynArray< FindNearestLaneResult > lanesInRange{ red::PoolFrame() };
	FindLanesInRange( nearestStartLaneResult.lanePoint, c_startAndEndLaneSearchArea, lanesInRange, req.query.laneFilter );
	startLanes.Reserve( lanesInRange.Size() );
	for ( const FindNearestLaneResult& laneNearStart : lanesInRange )
	{
		if ( math::Abs( laneNearStart.lanePoint.Z - nearestStartLaneResult.lanePoint.Z ) < c_startAndEndLaneMaxSearchZDiff )
		{
			red::alg::PushBackUnique( startLanes, laneNearStart.lane );
		}
	}

	lanesInRange.Clear();
	FindLanesInRange( nearestEndLaneResult.lanePoint, c_startAndEndLaneSearchArea, lanesInRange, req.query.laneFilter );
	endLanes.Reserve( lanesInRange.Size() );
	for ( const FindNearestLaneResult& laneNearEnd : lanesInRange )
	{
		if ( math::Abs( laneNearEnd.lanePoint.Z - nearestEndLaneResult.lanePoint.Z ) < c_startAndEndLaneMaxSearchZDiff )
		{
			red::alg::PushBackUnique( endLanes, laneNearEnd.lane );
		}
	}

	// Start / set up the search
	return m_pathFinder->StartPathfinding(
		startLanes,
		endLanes,
		req.query.searchMode,
		req.query.start,
		req.query.end );
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::ContinueFindGlobalPathNoLock( const GlobalPathRequest& req, const Float maxTimeInMilliSeconds, Result& result ) const
{
	PC_SCOPE( ContinueProcessFindGlobalPathInternal );
	RED_ASSERT( m_pathFinder->IsPathfindingInProgress() );

	// Run the actual pathfinding logic

	if ( !m_pathFinder->ContinuePathfinding( maxTimeInMilliSeconds, result.path, result.info ) )
	{
		return false;
	}

	// Optionally: remove loops from the path

	if ( req.query.removeLoops )
	{
		Uint32 newFirstIdx = 0;
		const red::DynArray< TrafficGlobalPath::Lane >& lanes = result.path->GetLanes();
		const world::TrafficLaneUID& firstLaneUID = lanes[ 0 ].m_laneId;
		for ( Uint32 laneI = 0; laneI < lanes.Size(); ++laneI )
		{
			const world::TrafficLaneUID& lid = lanes[laneI].m_laneId;
			if ( lid.m_nodeRefHash == firstLaneUID.m_nodeRefHash && lid.m_isReversed == !firstLaneUID.m_isReversed )
			{
				newFirstIdx = laneI;
				break;
			}
		}

		TrafficGlobalPathProgression clipProgression;
		clipProgression.Reset( *result.path, newFirstIdx );
		result.path->ClipStart( clipProgression, {} );
	}

	return true;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterPersistentData( TrafficPersistentData* persistentData )
{
	RED_ASSERT( persistentData != nullptr );
	if ( !TrafficPersistentData::Verify( *persistentData ) )
	{
		RED_DATA_ERROR( red::LoggerCategory_Traffic, false, "Broken connectivity data! Please regenerate traffic (run Build & Play in Full mode)." );
		return;
	}

	RED_SCOPE_LOCK( m_lock );
    m_pathFinder = red::CreateUniquePtr< traffic::PathFinder, nav::PoolPathfindingNavigation >( *persistentData, m_runtimeState);
	m_runtimeState.SetPersistentData( persistentData );
	m_deadEndDetector = red::CreateUniquePtr< ::traffic::DeadEndDetector, nav::PoolTrafficLanes >( m_collisionMap, *persistentData, m_runtimeState, *m_trafficLaneEventsBroadcaster );
	m_persistentData = persistentData;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UnregisterPersistentData()
{
	// This is never called in game. It is only required for reloading connectivity in the editor.

	RED_SCOPE_LOCK( m_lock );

	for ( auto lane : m_laneLUT )
	{
		for ( auto& slot : lane.Value()->GetSlots() )
		{
			RemoveSlotNoLock( slot.Lock() );
		}
	}

	m_registrationQueue.Clear();
	m_unregistrationQueue.Clear();
	m_nodeDump.Clear();

	m_nodeToLaneLUT.Clear();
	m_laneLUT.Clear();
	m_intersections.Clear();

	m_deadEndDetector.Reset();

	if ( m_currentGlobalPathRequest.IsValid() && m_pathFinder != nullptr )
	{
        m_pathFinder->CancelPathfinding();
		m_currentGlobalPathRequest.Reset();
	}
    m_pathFinder.Reset();

	m_runtimeState.SetPersistentData( nullptr );
	m_persistentData = nullptr;

}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::ScheduleRegisterLanes( THandle< TrafficCompiledNode > node )
{
	RED_SCOPE_LOCK( m_lock );
	auto it = std::find( m_unregistrationQueue.Begin(), m_unregistrationQueue.End(), node );

	if ( it != m_unregistrationQueue.End() ) //we need to cancel out the registration and unregistration
	{
		m_unregistrationQueue.RemoveReorder( it );
	}
	else
	{
		red::alg::PushBackUnique( m_registrationQueue, node );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::ScheduleUnregisterLanes( THandle< TrafficCompiledNode > node )
{
	RED_SCOPE_LOCK( m_lock );
	auto it = std::find( m_registrationQueue.Begin(), m_registrationQueue.End(), node );

	if ( it != m_registrationQueue.End() )
	{
		m_registrationQueue.RemoveReorder( it );
	}
	else
	{
		red::alg::PushBackUnique( m_unregistrationQueue, node );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::ProcessAllPendingLanesRegistration()
{
	if ( m_persistentData == nullptr )
	{
		// data not loaded yet
		return;
	}

	if ( !m_persistentData->IsDataInitialized() )
	{
		// data partially loaded
		return;
	}


	decltype( m_registrationQueue) regQueue = { job::PoolJobScope() };
	decltype( m_unregistrationQueue ) unRegQueue = { job::PoolJobScope() };

	{
		RED_SCOPE_LOCK( m_lock );
		regQueue = m_registrationQueue;
		unRegQueue = m_unregistrationQueue;

		m_registrationQueue.Clear();
		m_unregistrationQueue.Clear();
	}

	for ( auto& node : regQueue )
	{
		RegisterLanes( node );
	}

	for ( auto& node : unRegQueue )
	{
		UnregisterLanes( node );
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterLanes( THandle< TrafficCompiledNode > node )
{
	PC_SCOPE( RuntimeSystemTraffic_RegisterLanes );

	auto& inputLanes = node->GetLanes();

	if ( !node->HasNodeId() ) return;

	auto nodeId = node->GetNodeId();
	auto id = node->GetLanes().Front().uid.m_nodeRefHash;

#ifdef RED_ASSERTS_ENABLED
	{
		for ( auto& lane : inputLanes )
		{
			RED_ASSERT( lane.uid.m_nodeRefHash == inputLanes.Front().uid.m_nodeRefHash );
		}
	}
#endif
	{
		RED_SCOPE_SHARED_LOCK( m_lock );
		if( m_nodeDump.Remove( nodeId ).IsSuccessful() ) // if the node is in the dump, we can simply revive it
		{
			return; //nothing was removed
		}
	}


	red::DynArray< std::pair< TrafficLaneUID, TrafficLaneStreamed* > > newLanes = { job::PoolJobScope() };

	auto nodeBox = Box::EMPTY();

	newLanes.Reserve( inputLanes.Size() );

	for ( auto& lane : inputLanes )
	{
		newLanes.PushBack( { lane.uid, &lane } );
	}

	//////////////////////////////////////////////////////////////////////////
	{
		RED_SCOPE_LOCK( m_lock );
		auto it = m_nodeToLaneLUT.Find( nodeId );
		RED_ASSERT( it == m_nodeToLaneLUT.End() );
		RED_ASSERT( m_intersections.Find( nodeId ) == m_intersections.End() );

		it = m_nodeToLaneLUT.Emplace( nodeId, m_nodeToLaneLUT.GetPool() ).Iterator();

#ifndef RED_CONFIGURATION_FINAL
		for ( auto i = 0u; i < newLanes.Size(); ++i )
		{
			auto pData = FindPersistentLane( newLanes[i].first );
			if ( !pData )
			{
				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Traffic, "Persistent data for lane %s not found - it's not going to be registered. Corrupted data?", newLanes[i].first.ToDebugString().AsChar() );
				newLanes[i].first = TrafficLaneUID();
			}
			else
			{
				if ( !CrowdAccessible( pData ) ) continue;

				if ( FindLane( newLanes[i].first ) )
				{
					RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Navigation, "Trying to register a traffic lane %s twice. Corrupted data?", newLanes[i].first.ToDebugString().AsChar() );
					newLanes[i].first = TrafficLaneUID();
				}

			}

			newLanes.Remove( std::remove_if( newLanes.Begin(), newLanes.End(), [&]( auto& newLaneId )
			{
				return newLaneId.first == TrafficLaneUID();
			} ), newLanes.End() );

			if ( newLanes.Empty() ) return;
		}
#endif

		m_nodeUsage.Insert( GlobalNodeID{ nodeId }, 0u );
		{
			auto representation = m_staticCollisions.CreateRepresentation( [&]( auto& data )
			{
				data.m_id = id;
				data.m_box = Box::EMPTY();
				data.m_type = StaticCollisionMap::value_type::Type::Static;

				m_persistentStaticCollisions.ExtractCollisions( data.m_data, GlobalNodeID{ nodeId } );

				data.UpdateBox();
				data.UpdateLookup();
			} );

			if ( !representation->Empty() )
			{
				m_staticCollisions.Add( id, representation
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
									, []( auto& ){}, "static"
#endif
				);
			}
		}
			
		{
			//now we input the new lanes
			for ( auto i = 0u; i < newLanes.Size(); ++i )
			{
				auto id = newLanes[i].first;
				auto pData = FindPersistentLane( id );

				red::UniquePtr< TrafficLane > lane = red::CreateUniquePtr< TrafficLane >( *newLanes[i].second, *pData );
				m_laneLUT.Insert( id, std::move( lane ) );
				/* When a new lane is streamed in, we need to update their collisions in case it was streamed in as red light */
				m_lanesWithTrafficLights.Insert( id );
				it.Value().PushBack( id );

				auto laneBox = pData->ComputePolygonBox();
				nodeBox.AddBox( laneBox );

				if ( CrowdAccessible( *pData ) && !pData->HasFlag( TrafficLanePersistent::Flags::TrafficDisabled ) )
				{
					if ( pData->HasFlag( TrafficLanePersistent::Flags::Road ) )
					{
#ifdef ENABLE_FULL_LANE_LOOKUP
						m_laneLookup.m_roads.Insert( laneBox, id );
#endif
					}
					else if ( pData->HasFlag( TrafficLanePersistent::Flags::Pavement ) && !pData->HasFlag( world::TrafficLanePersistent::Flags::CrossWalk ) )
					{
						m_laneLookup.m_pavement.Insert( laneBox, id );
					}
					if ( pData->HasFlag( TrafficLanePersistent::Flags::CrossWalk ) )
					{
#ifdef ENABLE_FULL_LANE_LOOKUP
						m_laneLookup.m_zebras.Insert( laneBox, id );
#endif
					}
				}
			}
		}
	}
	//this requires for the lanes to be already LUT stored
	
	auto canSkipIntersection = true;

	for ( auto i = 0u; i < newLanes.Size(); ++i )
	{
		if ( newLanes[i].second->ValidLights() )
		{
			canSkipIntersection = false;
			break;
		}
	}

	if ( !canSkipIntersection )
	{
		red::DynArray< TrafficLaneUID > laneIds = { job::PoolJobScope() };
		laneIds.Reserve( newLanes.Size() );
		std::for_each( newLanes.Begin(), newLanes.End(), [&]( auto& value ){ laneIds.PushBack( value.first ); } );

		red::UniquePtr< TrafficIntersection > intersection = red::CreateUniquePtr< TrafficIntersection >( *this, nodeId, laneIds );
		auto pIntersection = intersection.Get();

		{
			RED_SCOPE_LOCK( m_lock );
			m_intersections.Insert( nodeId, std::move( intersection ) );

			m_lightChangeListenerRegistry.OnIntersectionCreated( pIntersection );
		}

#ifdef ENABLE_FULL_LANE_LOOKUP
		if ( !nodeBox.IsEmpty() )
		{
			m_laneLookup.m_intersections.Insert( nodeBox, GlobalNodeID{ nodeId } );
		}
#endif
	}

#ifndef RED_CONFIGURATION_FINAL
	for ( auto i = 0u; i < newLanes.Size(); ++i )
	{
		auto id = newLanes[i].first;
		auto it = m_laneLUT.Find( id );
		auto lane = FindPersistentLane( id );

		RED_ASSERT( it != m_laneLUT.End() );
		RED_ASSERT( lane );

		if ( ( lane->m_flags & TrafficLanePersistent::PatrolRoute ) != 0 )
		{
			static const Color s_patrolDebugColor = Color::BLUE();
			it.Value()->SetDebugColor( s_patrolDebugColor );
		}
		

#ifdef RED_ASSERTS_ENABLED
		if ( newLanes[i].second->ValidLights() )
		{
			it.Value()->SanityCheck();
		}
		else
		{
			RED_ASSERT( !it.Value()->HasLights() );
		}
#endif

	}
#endif

	//broadcast on adding lane
	{
		PC_SCOPE( RuntimeSystemTraffic_BroadcastEvent );
		red::DynArray< TrafficLaneUID > laneIds = { job::PoolJobScope() };
		laneIds.Reserve( newLanes.Size() );
		std::for_each( newLanes.Begin(), newLanes.End(), [&]( auto& value ) { laneIds.PushBack( value.first );	} );
		m_trafficLaneEventsBroadcaster->BroadcastEvent( laneIds, TrafficLaneEvent::Registered );
	}

#ifdef RED_ASSERTS_ENABLED
	{
		for ( auto it : m_laneLUT )
		{
			it.Value()->SanityCheck();
		}
	}
#endif

}


// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UnregisterLanes( THandle< TrafficCompiledNode > node )
{
	PC_SCOPE( RuntimeSystemTraffic_UnregisterUnit );

	if( !node->HasNodeId() )
	{
		return;
	}
	auto nodeId = node->GetLanes().Front().uid.m_nodeRefHash;

	{
		RED_SCOPE_LOCK( m_lock );
		auto it = m_nodeToLaneLUT.Find( GlobalNodeID{ nodeId } );
		RED_ASSERT( it != m_nodeToLaneLUT.End() );
		
		m_nodeDump.Insert( GlobalNodeID{ nodeId } );
		m_checkNodeDump = true;
	}

}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RemoveSlot( const TrafficSlotPtr &slotPtr )
{
	PC_SCOPE_FUNC();

	{
		RED_SCOPE_LOCK( m_lock ); //this manipulation will block the parallel update

		if( TrafficLane* lane = FindLane( slotPtr->m_lanePositionCache.cachedLane ) )
		{
			lane->RemoveSlot( *slotPtr );
			RegisterLaneToBeSlotPurgedNoLock( *lane );
		}
		m_slotLUT.Remove( slotPtr->GetId() );
	}

	slotPtr->SlotClearedFromSystem();
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RemoveSlotNoLock( TrafficSlotPtr slot )
{
	if ( !slot ) return;

	if( TrafficLane* lane = FindLane( slot->m_lanePositionCache.cachedLane ) )
	{
		lane->RemoveSlot( *slot );
		RegisterLaneToBeSlotPurgedNoLock( *lane );
	}
	slot->SlotClearedFromSystem();
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UpdateSlot( const TrafficSlotPtr &slot, Float destinationDistance )
{
	//the slot needs to be guaranteed to be valid during the whole call
	//there cannot be multiple updates per one slot concurrently

	RED_ASSERT( slot );
	RED_ASSERT( std::isfinite( slot->movementState.position.X ) && std::isfinite( slot->movementState.position.Y ) && std::isfinite( slot->movementState.position.Z ) );
	RED_ASSERT( slot->movementState.behav == TrafficMovementBehavior::Car );
	if ( slot->GetGlobalPath().IsEmpty() ) return;

	TrafficSlot::PathUpdateInfo updateInfo = slot->UpdatePathPosition( destinationDistance );

	// Update slot - lane affinity
	if ( updateInfo.currLane != updateInfo.prevLane )
	{
		RED_SCOPE_LOCK( m_lock ); //this manipulation will block the parallel update

		//note this "updateInfo.prevLane" cannot be trusted
		auto prevLane = FindLane( slot->m_lanePositionCache.cachedLane );
		auto currLane = FindLane( updateInfo.currLane );

		if ( prevLane != currLane )
		{
			if ( prevLane )
			{
				prevLane->RemoveSlot( *slot );
				RegisterLaneToBeSlotPurgedNoLock( *prevLane );
			}
			if ( currLane )	currLane->AddSlot( slot );
		}
	}
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UpdateSlot( const TrafficSlotPtr& slot, Bool& shouldProgress )
{
	PC_SCOPE_FUNC();

	RED_ASSERT( slot );
	RED_ASSERT( std::isfinite( slot->movementState.position.X ) && std::isfinite( slot->movementState.position.Y ) && std::isfinite( slot->movementState.position.Z ) );

	const world::TrafficLaneUID previousLaneId = slot->m_lanePositionCache.cachedLane;

	//TODO: Start search from cached segmentIndex instead of 0
	Uint32 segmentIndex = 0u;
	const Float xProjectionOnLocalPath = world::CalcSplineProjectionX< Vector4 >( slot->localPath.GetPoints(), slot->movementState.position, nullptr, &segmentIndex );

	const red::DynArray< TrafficLaneUID >& localPathLanes = slot->localPath.GetLanes();

	RED_ASSERT( segmentIndex >= 0u && segmentIndex < localPathLanes.Size() );
	const world::TrafficLaneUID currentLaneId = localPathLanes[segmentIndex];

	slot->m_lanePositionCache.segmentIndex = segmentIndex;

	// Update slot - lane affinity
	if ( previousLaneId != currentLaneId )
	{
		shouldProgress = ChangeLane( slot, currentLaneId );
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::ChangeLane( const TrafficSlotPtr& slot, const TrafficLaneUID& newLaneId )
{
	RED_SCOPE_LOCK( m_lock );

	auto prevLane = FindLane( slot->m_lanePositionCache.cachedLane );
	auto currLane = FindLane( newLaneId );

	if ( prevLane == currLane ) return true;

	// FindLane may fail because of unstreamed lane
	if ( prevLane )
	{
		prevLane->RemoveSlot( *slot );
		RegisterLaneToBeSlotPurgedNoLock( *prevLane );
	}

	if ( currLane )
	{
		currLane->AddSlot( slot );
	}

	return prevLane && currLane;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UpdateSlotSpots( const TrafficSlotPtr& slot )
{
	PC_SCOPE_FUNC();

	const TrafficGlobalPath::Lane* currentLane = slot->GetGlobalPath().GetLane( slot->m_lanePositionCache.cachedLane );
	if ( !currentLane || currentLane->CheckFlag( TrafficLanePersistent::CrossWalk ) )
	{
		return;
	}

	VisitLane( slot->m_lanePositionCache.cachedLane, [&]( auto& lane )
	{
		// Update spots the slot can use.
		
		TrafficWorkspots& availableSpots = slot->currentSpots;
		availableSpots.Clear();
		availableSpots.Reserve( lane.m_spots.Size() );

		const Float slotXPositionOnLane = slot->xPositionOnLane;
		for ( const auto& spotHandle : lane.m_spots )
		{
			const Float spotXpositionOnLane = spotHandle->m_lanePosition.X;

			// Is the spot located on the lane portion the slot/NPC will navigate
			if ( spotXpositionOnLane < currentLane->m_entryPosition || spotXpositionOnLane > currentLane->m_exitPosition )
				continue;

			// Is the slot/NPC currently within the spot's access range
			if ( !math::InRangeIncluding( slotXPositionOnLane, ComputeTrafficSpotXRange( *spotHandle ) ) )
				continue;

			// Is the spot located in a safe location
			if ( IsPointInsideNullArea( spotHandle->GetSpotStartPosition() ) )
				continue;

			availableSpots.EmplaceBack( spotHandle );
		}
	} );
}

// --------------------------------------------------------------------------------

TrafficIntersection* RuntimeSystemTraffic::FindIntersection( const GlobalNodeID& nodeID ) const
{
	const red::UniquePtr< TrafficIntersection >* result = m_intersections.FindPtr( nodeID );
	return result ? result->Get() : nullptr;
}

// --------------------------------------------------------------------------------

TrafficLane* RuntimeSystemTraffic::FindLane( const TrafficLaneUID& laneId ) const
{
	auto it = m_laneLUT.Find( laneId );
	return it != m_laneLUT.End() ? it.Value().Get() : nullptr;
}

// --------------------------------------------------------------------------------

const TrafficLanePersistent* RuntimeSystemTraffic::FindPersistentLane( const TrafficLaneUID& laneID ) const
{
	return m_persistentData ? m_persistentData->FindLane( laneID ) : nullptr;
}

// --------------------------------------------------------------------------------

const TrafficLanePersistent* RuntimeSystemTraffic::GetPersistentLane( const TrafficLaneIndex laneIndex ) const
{
	return m_persistentData ? &m_persistentData->GetLane( laneIndex ) : nullptr;
}

// --------------------------------------------------------------------------------

const TrafficLaneUID RuntimeSystemTraffic::GetLaneUID( const TrafficLaneIndex laneIndex ) const
{
	return m_persistentData ? m_persistentData->GetLaneUID( laneIndex ) : TrafficLaneUID();
}

// --------------------------------------------------------------------------------

const TrafficLaneStreamed* RuntimeSystemTraffic::FindCompiledLaneByID( const TrafficLaneUID& laneId ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	auto lane = FindLane( laneId );
	return lane ? &( lane->GetStreamedData() ) : nullptr;
}

// --------------------------------------------------------------------------------

FindNearestLaneResult RuntimeSystemTraffic::FindNearestLane( const Vector4& worldPos, const TrafficLaneFilter& filter /* = TrafficLaneFilter() */ ) const
{
	PC_SCOPE( FindNearestLane );
	if ( m_persistentData == nullptr ) return FindNearestLaneResult();

	return m_persistentData->FindNearestLane( worldPos, filter );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::FindLanesInRange( const Vector4& worldPos, const Float range, red::DynArray<FindNearestLaneResult>& outResult, const TrafficLaneFilter& filter /* = TrafficLaneFilter() */, const red::memory::Pool& pool /*= red::PoolFrame()*/ ) const
{
	PC_SCOPE( FindLanesInRange );

	if ( m_persistentData == nullptr ) return;

	red::DynArray< TrafficLaneGridInfo > foundLanes{ pool };

	const Float rangeSqr = std::pow( range, 2 );
	const math::Vector2 rangeHalfSide( range );
	math::Vector2 collectionRectMin = worldPos.AsVector2() - rangeHalfSide;
	math::Vector2 collectionRectMax = worldPos.AsVector2() + rangeHalfSide;

	const TrafficPersistentData::LaneGrid &grid = m_persistentData->GetLanesGrid();

	grid.CollectInRectangleRough( collectionRectMin, collectionRectMax, foundLanes );

	std::sort( foundLanes.Begin(), foundLanes.End() );
	auto it = std::unique( foundLanes.Begin(), foundLanes.End() );
	foundLanes.Remove( it, foundLanes.End() );

	for ( const TrafficLaneGridInfo &lgi : foundLanes )
	{
		const TrafficLanePersistent& lane = m_persistentData->GetLane( lgi.laneIndex );

		if ( !filter( lane ) )
		{
			continue;
		}

		FindNearestLaneResult resultForLane;
		resultForLane.distSqr = rangeSqr;
		for ( Uint32 pI = 1; pI < lane.m_outline.Size(); ++pI )
		{
			Float lineRatio;
			const Vector4 p0 = lane.m_outline[pI - 1];
			const Vector4 p1 = lane.m_outline[pI];
			const Float distSqr = MathUtils::GeometryUtils::DistanceSqrPointToLineSeg( worldPos, p0, p1, lineRatio );
			if ( distSqr < resultForLane.distSqr )
			{
				resultForLane.lane = &lane;
				resultForLane.distSqr = distSqr;
				resultForLane.lanePoint = math::Lerp( lineRatio, p0, p1 );
			}
		}
		if ( resultForLane.distSqr < rangeSqr )
		{
			outResult.PushBack( resultForLane );
		}
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::IsPathCrossingNullArea( const red::DynArray< Vector3 >& path, Float from, TrafficNullAreaFilter filter ) const
{
	if ( path.Size() == 1u )
	{
		return IsPointInsideNullArea( path.Back() );
	}
	else
	{
		RED_SCOPE_SHARED_LOCK( m_nullAreaLock );

		red::DynArray< world::TrafficAreaShape > nullAreas{ job::PoolJobScope() };
		nullAreas.Reserve( m_activeNullAreas.Size() );
		for ( const auto& nullArea : m_activeNullAreas )
		{
			if ( filter( nullArea ) == TrafficFilterOperation::Keep )
			{
				nullAreas.PushBack( nullArea );
			}
		}

		if ( math::AlmostEquals( from, 0.f ) )
		{
			return IsPathCrossingNullArea( path, nullAreas );
		}
		else
		{
			red::DynArray< Vector3 > subSpline{ job::PoolJobScope() };
			SubSpline< Vector3 >( path, from, subSpline );
			return IsPathCrossingNullArea( subSpline, nullAreas );
		}
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::IsPathCrossingNullArea( const red::DynArray<Vector3>& path, const red::DynArray< TrafficAreaShape >& areas ) const
{
	PC_SCOPE( RuntimeSystemTraffic_IsPathCrossingNullArea );

	for ( const TrafficAreaShape& area : areas )
	{
		if ( area.FindSplineIntersection( path ) )
		{
			return true;
		}
	}
	return false;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::IsPointInsideNullArea( const Vector4& point ) const
{
	PC_SCOPE( RuntimeSystemTraffic_IsPointInsideNullArea );

	RED_SCOPE_SHARED_LOCK( m_nullAreaLock );

	for ( const TrafficAreaShape& area : m_activeNullAreas )
	{
		if ( area.PointOverlap( point ) )
		{
			return true;
		}
	}
	return false;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::DoesLaneIntersectWithArea( const TrafficLanePersistent& lane, const TrafficAreaShape& area, Float from ) const
{
	// Right now checks if lane outline intersects with Area.
	const auto& outline = lane.GetOutline();
	return DoesSplineIntersectWithArea( outline, area, from );
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::DoesLaneIntersectWithAreas( const TrafficLanePersistent& lane, const red::DynArray< TrafficAreaShape >& areas, Float from ) const
{
	PC_SCOPE_FUNC();

	const auto& outline = lane.GetOutline();
	if ( math::AlmostEquals( from, 0.f ) )
	{
		return IsPathCrossingNullArea( outline, areas );
	}
	else
	{
		red::DynArray< Vector3 > subSpline{ job::PoolJobScope() };
		SubSpline< Vector3 >( outline, from, subSpline );

		return IsPathCrossingNullArea( subSpline, areas );
	}
}
// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::CheckIfLaneHasInLanes( const TrafficLaneUID& laneId ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	const TrafficLanePersistent* laneData = FindPersistentLane( laneId );
	return laneData ? !laneData->m_inLanes.Empty() : false;
}

Bool RuntimeSystemTraffic::IsLaneStreamedIn( const TrafficLaneUID& laneId ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );
	return m_laneLUT.Find( laneId ) != m_laneLUT.End();
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::IsCrosswalk( const TrafficLaneUID& laneId, TrafficLightColor& outTrafficLightColor ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	auto lane = FindLane( laneId );
	if ( !lane )
	{
		return false;
	}

	const TrafficLanePersistent& persistentLane = lane->GetPersistentData();
	if ( !persistentLane.HasFlag( world::TrafficLanePersistent::Flags::CrossWalk ) )
	{
		return false;
	}

	auto it = m_intersections.Find( GlobalNodeID{ laneId.m_nodeRefHash } );
	if ( it != m_intersections.End() )
	{
		return it.Value()->GetLightColorAlongLane( laneId, outTrafficLightColor );
	}

	return false;
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::GetCrosswalkInfo( const TrafficLaneUID& laneId, const Vector3& currentAgentPosition, Float& outTotalCrosswalkDistance, Float& outCrosswalkDistanceLeft ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	const TrafficLanePersistent* persistentLane = FindPersistentLane( laneId );
	RED_ASSERT( persistentLane );
	RED_ASSERT( persistentLane->HasFlag( world::TrafficLanePersistent::Flags::CrossWalk ) );

	outTotalCrosswalkDistance = persistentLane->m_length;
	const Float agentXOffsetOnLane = world::CalcSplineProjectionX<Vector3>( persistentLane->m_outline, currentAgentPosition );
	outCrosswalkDistanceLeft = outTotalCrosswalkDistance - agentXOffsetOnLane;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::IsPathCrossingLane( const red::DynArray< Vector4 >& path, Vector4& outIntersection, const VerticalLimits& verticalLimits, Float intersectionDistance /*= 0.f*/, const TrafficLaneFilter& filter /*= TrafficLaneFilter()*/, const red::memory::Pool& pool /*= red::PoolFrame() */ ) const
{
	Box pathBoundingBox = Box{ Box::RESET_STATE };

	for ( auto& point : path )
	{
		pathBoundingBox.AddPoint( point );
	}

	{
		if ( verticalLimits.ValidUp() ) pathBoundingBox.AddPoint( pathBoundingBox.Max + ( Vector4::EZ() * verticalLimits.up ) );
		if ( verticalLimits.ValidDown() ) pathBoundingBox.AddPoint( pathBoundingBox.Min - ( Vector4::EZ() * verticalLimits.down ) );
	}

	red::DynArray<FindNearestLaneResult> result{ pool };
	FindLanesInRange( pathBoundingBox.CalcCenter(), pathBoundingBox.CalcExtents().Mag3(), result, filter, pool );

	for ( Uint32 i = 0; i < path.Size() - 1; ++i )
	{
		Bool intersects = false;
		Float bestT = 1;

		for ( auto& nearestLane : result )
		{
			if ( nearestLane.lane != nullptr )
			{
				world::PathPolygons::TestLine2PolygonIntersection2D( nearestLane.lane->GetPolygon(),
																	 [&intersects, &bestT, &pathBoundingBox, &verticalLimits]( const Vector4& ptA, const Vector4& ptB, Float ratioPoly, Float ratioLine )
				{
					if ( !verticalLimits.Is2D() )
					{
						auto pointOfContact = ptA + ( ptB - ptA ) * ratioPoly;

						if ( !pathBoundingBox.Contains( pointOfContact ) )
						{
							return true;
						}
					}

					if ( ratioLine < bestT )
					{
						bestT = ratioLine;
					}
					intersects = true;
					return true;
				}
				, path[i], path[i + 1] );
			}
		}

		if ( intersects )
		{
			outIntersection = math::Lerp( bestT, path[i], path[i + 1] );

#ifndef RED_CONFIGURATION_FINAL
			if( m_cacheCrossingLaneDebugs )
			{
				if( m_crossingLaneCachedDebugs.Full() )
				{
					m_crossingLaneCachedDebugs.RemoveAt( 0 );
				}
				m_crossingLaneCachedDebugs.PushBack( CrossingLaneDebugHelper{ path[ 0 ], outIntersection } );
			}
#endif

			//executed if the client wants a point in a given distance from lane
			while ( i >= 0 && intersectionDistance > 0.f )
			{
				Float distToPoint = ( outIntersection - path[i] ).Mag3();

				//current line is shorter, than distance
				if ( distToPoint < intersectionDistance )
				{
					outIntersection = path[i];
					//We are at the beginning, no place to move
					if ( i == 0 )
					{
						intersectionDistance = 0.f;
					}
					//Need to move to previous point
					else
					{
						intersectionDistance -= distToPoint;
						--i;
					}
				}
				//current line is longer, we just need to move the point backwards by the required distance
				else
				{
					Vector3 direction = ( path[i] - path[i + 1] ).Normalized3();
					outIntersection += direction * intersectionDistance;
					break;
				}
			}

			return true;
		}
	}

	return false;
}

// --------------------------------------------------------------------------------

#ifdef RED_USE_IMGUI
static const CName s_debugOverlayName = RED_NAME_CONSTEXPR( "Traffic Runtime Debug##Game" );
#endif

void RuntimeSystemTraffic::OnRenderDebug( const RenderDebugContext& context )
{
	PC_SCOPE( RuntimeSystemTraffic_OnRenderDebug );

	if ( m_persistentData == nullptr )
	{
#ifdef RED_USE_IMGUI
		if ( red::ImGui::IsWindowRequested( s_debugOverlayName ) )
		{
			static const char* s_errorName = "Error!##runtimeSystemTraffic";


			//Set the error's window pos in the middle of the screen whenever it's brought up.
			{
				const Vector2 displaySize = ToVector2( ImGui::GetIO().DisplaySize );
				const Vector2 windowSize = red::ImGui::GetWindowSize( s_errorName );
				const ImVec2 newWindowPos = ToImVec2( ( displaySize + windowSize ) / 2.f );
				ImGui::SetNextWindowPos( newWindowPos, ImGuiCond_Appearing );
			}

			constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;

			ImGui::PushStyleColor( ImGuiCol_TitleBg, red::ImGui::DARK_RED() );
			ImGui::PushStyleColor( ImGuiCol_TitleBgActive, red::ImGui::RED() );

			if ( ImGui::Begin( s_errorName, nullptr, flags ) )
			{
				ImGui::Text( "./sectors/_generated/traffic/all.traffic_persistent" );
				ImGui::SameLine();
				ImGui::TextColored( red::ImGui::RED(), "is missing!" );

				ImGui::TextColored( red::ImGui::RED(), "Try removing" );
				ImGui::SameLine();
				ImGui::Text( "./sectors/_generated_traffic/" );
				ImGui::SameLine();
				ImGui::TextColored( red::ImGui::RED(), "folder from your world directory" );

				ImGui::TextColored( red::ImGui::RED(), "and either" );
				ImGui::SameLine();
				ImGui::TextDisabled( "Build&Play (In Full mode)" );
				ImGui::SameLine();
				ImGui::TextColored( red::ImGui::RED(), "or Right Click on the World in the world editor tree" );

				ImGui::TextColored( red::ImGui::RED(), "and go to" );
				ImGui::SameLine();
				ImGui::TextDisabled( "Compile... -> Communities/Traffic" );
			}

			ImGui::End();

			ImGui::PopStyleColor( 2 );
		}
#endif
		return;
	}

	RED_SCOPE_SHARED_LOCK( m_lock );

	auto mask = context.debugDrawer.GetMask();

	rend::FragmentBuilder builder( context.debugDrawer );

	if ( mask.Test( DF_TrafficLanes ) )
	{
		for ( auto it : m_laneLUT )
		{
			it.Value()->RenderDebug( context.debugDrawer, this );
		}
	}

#ifdef RED_USE_IMGUI
	if( mask.Test( DF_TrafficMemoryDebug ) )
	{
		RenderMemoryDebug( context );
	}
#endif

	if ( mask.Test( DF_TrafficSpots ) )
	{
		builder.SetColor( Color::LIGHT_BLUE() );

		for ( auto it : m_laneLUT )
		{
			auto lane = it.Value().Get();
		
			for ( auto& compiledSpot : lane->GetStreamedData().spots )
			{
				auto& outline = lane->GetPersistentData().m_outline;
				Vector2 lanePosition = compiledSpot->m_lanePosition;
				Vector4 startPos = CalcPositionOnSpline<Vector3>( outline, lanePosition.X );
				lanePosition.X += compiledSpot->m_length;
				Vector4 endPos = CalcPositionOnSpline<Vector3>( outline, lanePosition.X );
				builder.AddSolidSphere( startPos, 0.25f );
				builder.AddSolidSphere( endPos, 0.25f );
				builder.AddArrowHead( startPos, endPos - startPos, 1.0f );
			}
		}
		
	}

	//TODO
	if ( mask.Test( DF_TrafficLaneConnections ) )
	{
		for ( auto it : m_laneLUT )
		{
			auto& lane = *( it.Value() );
			
			const TrafficLanePersistent& persistent = lane.GetPersistentData();
			Vector4 startPoint = persistent.m_outline[0];
			Vector4 endPoint = persistent.m_outline.Back();

			Color color = GetTrafficLaneDebugRenderColor( persistent.GetLaneId() );
			// ignore alpha change for reverse lanes.
			color.A = 0xff;
			builder.SetColor( color );

			const Float c_zOffset = 0.2f;
			endPoint.Z += c_zOffset;

			Vector3 zOffset = Vector4( 0.0f, 0.0f, 0.0f );
			for ( const TrafficConnectivityOutLane& outLane : persistent.m_outLanes )
			{
				const TrafficLaneUID outLaneUID = m_persistentData->GetLaneUID( outLane.laneIndex );
				auto otherLane = FindLane( outLaneUID );
				if ( otherLane )
				{
					const TrafficLanePersistent &otherPersistent = otherLane->GetPersistentData();

					if ( otherPersistent.m_outline.Size() < 2 )
						continue;

					Vector4 exitPoint = Vector4( CalcPositionOnSpline<Vector3>( persistent.m_outline, outLane.thisLaneExitPosition ) );
					Vector4 entryPoint = outLane.nextLaneEntryPosition == 0.0f ? Vector4( otherPersistent.m_outline.Front() ) : Vector4( CalcPositionOnSpline<Vector3>( otherPersistent.m_outline, outLane.nextLaneEntryPosition ) );

					Vector4 dir = entryPoint - exitPoint;
					dir.Z = 0.0f;
					Float len = Max( dir.Normalize2(), 1.0f );

					if ( outLane.leadsToDeadEnd )
					{
						builder.AddFatArrow( exitPoint + zOffset, exitPoint + dir * len + zOffset, 0.05f );
					}
					else
					{
						builder.AddArrow( exitPoint + zOffset, exitPoint + dir * len + zOffset );
					}

					zOffset.Z += 0.1f;
				}
			}
		}
	}

	if ( mask.Test( DF_TrafficLaneSubgraphIDs ) )
	{
		builder.SetColor( Color::WHITE() );

		for ( const TrafficLanePersistent& lane : m_persistentData->GetLanes() )
		{
			if ( lane.m_outline.Size() < 2 )
			{
				RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Traffic, "A lane has an outline with less than 2 points." );
				continue;
			}

			builder.AddWorldTextf( lane.m_outline[1], 0, 0, "%u", lane.GetSubGraphId( TrafficSubGraphType::Regular ) );
		}
	}

	if ( mask.Test( DF_TrafficLaneUIDs ) )
	{
		builder.SetColor( Color::WHITE() );

		for ( auto it : m_laneLUT )
		{
			auto& lane = *it.Value();
			const TrafficLanePersistent& lp = lane.GetPersistentData();
			builder.AddWorldTextf( lp.m_outline[0], 0, 0, lp.GetLaneId().ToDebugString().AsChar() );
		}
	}

#ifndef RED_CONFIGURATION_FINAL
	if ( mask.Test( DF_TrafficStaticCollision ) && m_collisionDebug )
	{
		m_collisionDebug->DrawDebug( context.m_position, context.debugDrawer );
	}

	m_cacheCrossingLaneDebugs = mask.Test( DF_TrafficPrintCachedCrossingLane ) ? true : false;

	if( m_cacheCrossingLaneDebugs )
	{
		for( auto pair : m_crossingLaneCachedDebugs )
		{
			builder.SetColor( Color::GREEN() );
			builder.AddSolidSphere( pair.start, 0.5 );
			builder.SetColor( Color::RED() );
			builder.AddSolidSphere( pair.end, 0.5 );
			builder.AddLine( pair.start, pair.end );
		}
	}
#endif

#ifdef RED_USE_IMGUI
	DrawTrafficMap( context );
#endif

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	DrawSpawnDebug( context );
#endif
}

//================================================================================
#ifdef RED_USE_IMGUI

inline Uint64 RuntimeSystemTraffic::DataSize() const
{
	Uint64 intersectionsSize = 0;
	std::for_each( m_intersections.Begin(), m_intersections.End(), [&intersectionsSize]( const auto& intersection ) { intersectionsSize += sizeof( intersection ) + intersection.Value()->DataSize(); } );

	Uint64 nullAreaMapDataSize = m_nullAreaMap.DataSize();
	std::for_each( m_nullAreaMap.Begin(), m_nullAreaMap.End(), [&nullAreaMapDataSize]( const auto& nullArea ) { nullAreaMapDataSize += nullArea.Value().DataSize(); } );

	return
		sizeof( *this ) +
		intersectionsSize +
		m_collisionMap.DataSize() +
		m_greenWaveManager->DataSize() +
		m_persistentData->DataSize() +
		m_laneLookup.DataSize() +
		nullAreaMapDataSize;
}

//================================================================================
inline Uint64 RuntimeSystemTraffic::DataCapacity() const
{
	Uint64 intersectionsSize = 0;
	std::for_each( m_intersections.Begin(), m_intersections.End(), [&intersectionsSize]( const auto& intersection ) { intersectionsSize += sizeof( intersection ) + intersection.Value()->DataCapacity(); } );

	Uint64 nullAreaMapDataCapacity = m_nullAreaMap.DataCapacity();
	std::for_each( m_nullAreaMap.Begin(), m_nullAreaMap.End(), [&nullAreaMapDataCapacity]( const auto& nullArea ) { nullAreaMapDataCapacity += nullArea.Value().DataCapacity(); } );

	return
		sizeof( *this ) +
		intersectionsSize +
		m_collisionMap.DataCapacity() +
		m_greenWaveManager->DataCapacity() +
		m_persistentData->DataCapacity() +
		m_laneLookup.DataCapacity() +
		nullAreaMapDataCapacity;
}

//================================================================================
void RuntimeSystemTraffic::RecalculateDataSize() const 
{
	m_persistentData->RecalculateDataSize();

	DebugDataSize& data = m_debugDataSize;
	data.Clear();
	data.totalSize = DataSize();
	data.totalAllocated = DataCapacity();
	data.count = 1;
	data.other.totalSize = DataSize();

	auto updateComponentStats = []( RuntimeSystemTraffic::DebugDataSize & data, const CName name, Uint64 size, Uint32 count, Uint64 capacity )
	{
		auto component = std::find_if( data.components.Begin(), data.components.End(), [&name]( RuntimeSystemTraffic::DebugDataSizeComponent& visitedComponent ) {
			return visitedComponent.name == name;
		} );

		if( component == data.components.End() )
		{
			data.components.EmplaceBack();
			component = data.components.End() - 1;
			component->name = name;
		}

		component->totalSize += size;
		component->totalAllocated += capacity;
		component->count += count;

		data.other.totalSize -= size;
	};

	Uint64 intersectionsSize = 0;
	Uint64 intersectionsCapacity = 0;
	std::for_each( m_intersections.Begin(), m_intersections.End(), [&intersectionsSize, &intersectionsCapacity]( const auto& intersection ) { 
		intersectionsSize += sizeof( intersection ) + intersection.Value()->DataSize(); 
		intersectionsCapacity += sizeof( intersection ) + intersection.Value()->DataCapacity();
	} );

	Uint32 nullAreaMapSize = 0;
	Uint64 nullAreaMapDataSize = m_nullAreaMap.DataSize();
	Uint64 nullAreaMapDataCapacity = m_nullAreaMap.DataCapacity();
	std::for_each( m_nullAreaMap.Begin(), m_nullAreaMap.End(), [&nullAreaMapSize, &nullAreaMapDataSize, &nullAreaMapDataCapacity]( const auto& nullArea ) {
		nullAreaMapSize += nullArea.Value().Size();
		nullAreaMapDataSize += nullArea.Value().DataSize();
		nullAreaMapDataCapacity += nullArea.Value().DataCapacity();
	} );

	updateComponentStats( data, RED_NAME_CONSTEXPR( ".m_nullAreaMap" ), nullAreaMapDataSize, nullAreaMapSize, nullAreaMapDataCapacity );
	updateComponentStats( data, RED_NAME_CONSTEXPR( ".m_persistentData" ), m_persistentData->DataSize(), 1, m_persistentData->DataCapacity() );
	updateComponentStats( data, RED_NAME_CONSTEXPR( ".m_intersections" ), intersectionsSize, m_intersections.Size(), intersectionsCapacity );
	updateComponentStats( data, RED_NAME_CONSTEXPR( ".m_collisionMap" ), m_collisionMap.DataSize(), m_collisionMap.Size(), m_collisionMap.DataCapacity() );
	updateComponentStats( data, RED_NAME_CONSTEXPR( ".m_greenWaveManager" ), m_greenWaveManager->DataSize(), m_greenWaveManager->Size(), m_greenWaveManager->DataCapacity() );

	std::sort( data.components.Begin(), data.components.End(), []( const DebugDataSizeComponent& c0, const DebugDataSizeComponent& c1 ) {
		return c0.totalSize > c1.totalSize;
	} );
}

void RuntimeSystemTraffic::RenderMemoryDebug( const RenderDebugContext& context ) 
{
	if ( ImGui::Begin( "Traffic Data Memory Debug" ) )
	{	
		if( ImGui::Button( "Recalculate" ) )
		{
			RecalculateDataSize();
		}

		if ( ImGui::BeginTabBar( "PersistentData Tabs" ) )
		{
			if ( ImGui::BeginTabItem( "RuntimeSystemTraffic" ) )
			{
				RuntimeSystemTraffic::DebugDataSize& data = m_debugDataSize;

				static const Float s_indent = 10.f;
				static const ImColor s_headerColor( red::ImGui::LIGHT_GREEN() );

				ImGui::Columns( 5, "TrafficPersistentData" );

				ImGui::TextColored( s_headerColor, "Component" );
				ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "Count" );
				ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "Total size" );
				ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "%% Size" );
				ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "Allocated" );
				ImGui::NextColumn();

				ImGui::Separator();
				ImGui::Text( "All components" );
				ImGui::NextColumn();
				ImGui::Text( "%d", data.count );
				ImGui::NextColumn();
				ImGui::Text( "%.2f MB", (float)data.totalSize / 1024 / 1024 );
				ImGui::NextColumn();
				ImGui::Text( "%.0f %%", (double)data.totalSize / data.totalSize * 100 );
				ImGui::NextColumn();
				ImGui::Text( "%.2f MB", (float)data.totalAllocated / 1024 / 1024 );
				ImGui::NextColumn();

				for( auto& component : data.components )
				{
					ImGui::Separator();

					ImGui::Text( "%s", component.name.AsChar() );
					ImGui::NextColumn();
					ImGui::Text( "%d records", component.count );
					ImGui::NextColumn();
					ImGui::Text( "%.2f MB", (float)component.totalSize / 1024 / 1024 );
					ImGui::NextColumn();
					ImGui::Text( "%.0f %%", (double)component.totalSize / data.totalSize * 100 );
					ImGui::NextColumn();
					ImGui::Text( "%.2f MB", (float)component.totalAllocated / 1024 / 1024 );
					ImGui::NextColumn();
				}

				ImGui::Separator();

				auto& other = data.other;

				ImGui::Text( "Other" );
				ImGui::NextColumn();
				ImGui::Text( "%d records", other.count );
				ImGui::NextColumn();
				ImGui::Text( "%.2f MB", (float)other.totalSize / 1024 / 1024 );
				ImGui::NextColumn();
				ImGui::Text( "%.0f %%", (double)other.totalSize / data.totalSize * 100 );
				ImGui::NextColumn();
				ImGui::Text( "--" );
				ImGui::NextColumn();

				ImGui::Columns();

				ImGui::EndTabItem();
			}
			if ( ImGui::BeginTabItem( "TrafficPersistentData" ) )
			{
				TrafficPersistentData::DebugDataSizeTotal& data = m_persistentData->m_debugDataSize;

				static const Float s_indent = 10.f;
				static const ImColor s_headerColor( red::ImGui::LIGHT_GREEN() );

				ImGui::Columns( 4, "TrafficPersistentData" );

				ImGui::TextColored( s_headerColor, "Component" ); ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "Count" ); ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "Total size" ); ImGui::NextColumn();
				ImGui::TextColored( s_headerColor, "%% Size" ); ImGui::NextColumn();

				ImGui::Separator();
				ImGui::Text( "Serialized data" ); ImGui::NextColumn();
				ImGui::Text( "%d", data.count ); ImGui::NextColumn();
				ImGui::Text( "%.2f MB", ( float )data.serializedSize / 1024 / 1024 );	ImGui::NextColumn();
				ImGui::Text( "%.0f %%", ( double )data.serializedSize / data.serializedSize * 100 );	ImGui::NextColumn();

				ImGui::Separator();
				ImGui::Text( "All components" ); ImGui::NextColumn();
				ImGui::Text( "%d", data.count ); ImGui::NextColumn();
				ImGui::Text( "%.2f MB", ( float )data.totalSize / 1024 / 1024 );	ImGui::NextColumn();
				ImGui::Text( "%.0f %%", ( double )data.totalSize / data.totalSize * 100 );	ImGui::NextColumn();

				for( auto& component : data.components )
				{
					ImGui::Separator();

					ImGui::Text( "%s", component.name.AsChar() );
					ImGui::NextColumn();
					ImGui::Text( "%d records", component.count );
					ImGui::NextColumn();
					ImGui::Text( "%.2f MB", (float)component.totalSize / 1024 / 1024 );
					ImGui::NextColumn();
					ImGui::Text( "%.0f %%", (double)component.totalSize / data.totalSize * 100 );
					ImGui::NextColumn();
				}

				ImGui::Separator();

				auto& other = data.other;

				ImGui::Text( "Other" );
				ImGui::NextColumn();
				ImGui::Text( "%d records", other.count );
				ImGui::NextColumn();
				ImGui::Text( "%.2f MB", (float)other.totalSize / 1024 / 1024 );
				ImGui::NextColumn();
				ImGui::Text( "%.0f %%", (double)other.totalSize / data.totalSize * 100 );
				ImGui::NextColumn();

				ImGui::Columns();

				ImGui::EndTabItem();
			}
			if( ImGui::BeginTabItem( "TrafficLanePersistent" ) )
			{
				red::HashMap< CName, TrafficPersistentData::DebugDataSize >& dataByFlag = m_persistentData->m_debugDataSize.trafficLanesByFlag;

				static const Float s_indent = 10.f;
				static const ImColor s_headerColor( red::ImGui::LIGHT_GREEN() );

				for( const auto& data : dataByFlag )
				{
					if( ImGui::CollapsingHeader( data.Key().AsChar(), ImGuiTreeNodeFlags_DefaultOpen ) )
					{
						ImGui::Indent( s_indent );
						ImGui::Columns( 4, "TrafficLanePersistent" );

						ImGui::TextColored( s_headerColor, "Component" );
						ImGui::NextColumn();
						ImGui::TextColored( s_headerColor, "Count" );
						ImGui::NextColumn();
						ImGui::TextColored( s_headerColor, "Total size" );
						ImGui::NextColumn();
						ImGui::TextColored( s_headerColor, "%% size of parent" );
						ImGui::NextColumn();

						ImGui::Separator();

						ImGui::Text( "%s lanes:", data.Key().AsChar() );
						ImGui::NextColumn();
						ImGui::Text( "%d records", data.Value().count );
						ImGui::NextColumn();
						ImGui::Text( "%.2f MB", (float)data.Value().totalSize / 1024 / 1024 );
						ImGui::NextColumn();
						ImGui::Text( "%.0f %%", (double)data.Value().totalSize / dataByFlag[ RED_NAME_CONSTEXPR( "All" ) ].totalSize * 100 );
						ImGui::NextColumn();

						for( const auto& component : data.Value().components )
						{
							ImGui::Separator();

							ImGui::Text( "%s", component.name.AsChar() );
							ImGui::NextColumn();
							ImGui::Text( "%d records", component.count );
							ImGui::NextColumn();
							ImGui::Text( "%.2f MB", (float)component.totalSize / 1024 / 1024 );
							ImGui::NextColumn();
							ImGui::Text( "%.0f %%", (double)component.totalSize / data.Value().totalSize * 100 );
							ImGui::NextColumn();
						}

						ImGui::Separator();

						auto& other = data.Value().other;

						ImGui::Text( "Other" );
						ImGui::NextColumn();
						ImGui::Text( "%d records", other.count );
						ImGui::NextColumn();
						ImGui::Text( "%.2f MB", (float)other.totalSize / 1024 / 1024 );
						ImGui::NextColumn();
						ImGui::Text( "%.0f %%", (double)other.totalSize / data.Value().totalSize * 100 );
						ImGui::NextColumn();

						ImGui::Columns();
						ImGui::Unindent( s_indent );
					}
				}

				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

void RuntimeSystemTraffic::DrawNullAreaCollisionsOnWorld( const RenderDebugContext& context, bool showLogicRepresentation ) const
{
	rend::FragmentBuilder builder( context.debugDrawer );

	const Float drawingThresholdSquared = math::Pow( 500.f, 2.f ); 
	const Vector3 &cameraLocation = context.m_position;

	builder.SetColor( Color::LIGHT_MAGENTA() );
	for ( auto nullArea : m_nullAreaMap )
	{
		red::DynArray< TrafficStaticCollisionSphere >& spheres = nullArea.Value();

		if ( showLogicRepresentation )
		{
			for ( Uint32 i = 0; i < spheres.Size(); ++i )
			{
				const TrafficStaticCollisionSphere& sphere = spheres[ i ]; 
				if ( cameraLocation.DistanceSquaredTo( sphere.m_worldPos ) <= drawingThresholdSquared )
				{
					builder.AddWireSphere( sphere.m_worldPos, m_nullAreaHeader.m_radius );
					builder.AddWorldText( sphere.m_worldPos + Vector3::EZ(), 0, 0, String::Printf( "%u", i ) );
				}
			}
		}
		else
		{
			red::DynArray< DynamicCollisionRepresentation::CollisionSphere > finalRepresentation { red::PoolDebug() };
			GenerateNullAreaCollisions( nullArea.Key(), finalRepresentation );

			for ( const DynamicCollisionRepresentation::CollisionSphere& sphere : finalRepresentation )
			{
				const Vector3& position = sphere.data.CenterRadius.AsVector3();
				const Float& radius = sphere.data.CenterRadius.W;

				if ( cameraLocation.DistanceSquaredTo( position ) <= drawingThresholdSquared )
				{
					builder.AddWireSphere( position, radius );
				}
			}
		}
	}
}

#endif

//================================================================================
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
void RuntimeSystemTraffic::DrawSpawnDebug( const RenderDebugContext& context )
{
	static const CName s_debugOverlayName = RED_NAME_CONSTEXPR( "Spawning##Game" );
	static const CName s_statsDebugOverlayName = RED_NAME_CONSTEXPR( "SpawningStats##Game" );

	rend::FragmentBuilder builder( context.debugDrawer );

#ifdef RED_USE_IMGUI
	if ( red::ImGui::IsWindowRequested( s_statsDebugOverlayName ) )
	{
		if ( ImGui::Begin( s_statsDebugOverlayName.AsChar() ) )
		{
			ImGui::Checkbox( "Sort by ratio", &m_sortStatsByRatio );


			red::HashMap< TrafficLaneUID, std::pair< Uint32, Uint32> > stats = { red::PoolFrame() };

			RED_SCOPE_LOCK( m_spawnRqDebugLock );

			stats.Reserve( m_spawnResultDebug.Size() );

			for ( auto& result : m_spawnResultDebug )
			{
				if ( result.IsFailed() )
				{
					++stats[result.m_laneId].second;
				}
				else
				{
					++stats[result.m_laneId].first;
				}
			}

			red::DynArray< std::pair< TrafficLaneUID, Float > > ratios = { red::PoolFrame() };
			ratios.Reserve( stats.Size() );

			for ( auto it : stats )
			{
				auto& stat = it.Value();
				auto total = static_cast<Float>( stat.first + stat.second );

				ratios.PushBack( { it.Key(), stat.second / total } );
			}

			std::sort( ratios.Begin(), ratios.End(), [&]( auto& a, auto& b )
			{
				if ( m_sortStatsByRatio )
				{
					return a.second < b.second;
				}
				else
				{
					return a.first.ToDebugString() < b.first.ToDebugString();
				}
			} );

			for ( auto& value : ratios )
			{
				red::ImGui::BeginChild( String::Printf( "%s", value.first.ToDebugString().AsChar() ).AsChar(), {}, true );

				ImGui::Text( "Lane %s", value.first.ToDebugString().AsChar() );
				ImGui::Text( "Ratio %f (success: %d fails: %d)", value.second, stats[value.first].first, stats[value.first].second );

				red::ImGui::EndChild();
			}
		}

		ImGui::End();
	}
#endif

#ifdef RED_USE_IMGUI
	if ( red::ImGui::IsWindowRequested( s_debugOverlayName ) )
	{
		Vector4 zOffset = Vector4::ZEROS();
		{
			Config::IConfigVar* var = Config::GetConfigSystem().GetRegistry().Find( "Traffic/GeneralDebugDraw", "laneZOffset" );
			Float value = 0.0f;
			var->GetRaw( &value, Config::EConfigVarType::eConsoleVarType_Float );

			zOffset.Z = value;
		}

		if ( !ImGui::Begin( s_debugOverlayName.AsChar() ) )
		{
			ImGui::End();
			return;
		}

		red::ImGui::BeginChild( "Listing", {}, false );

		RED_SCOPE_LOCK( m_spawnRqDebugLock );
		//can be sorted by time, laneID etc.

		ImGui::Checkbox( "Filter selected", &m_filterSelected );
		ImGui::Checkbox( "Show selected", &m_showSelected );
		ImGui::Checkbox( "Show Details", &m_showDetails );
		ImGui::Text( "Current time %6.3f", GetTrafficTime() );
		ImGui::SameLine();
		ImGui::InputFloat( "Time Frame", &m_filteredFrameTime );

		ImGui::InputText( "Filter Entity Id", m_idFilter, 255, ImGuiInputTextFlags_CharsDecimal );
		ImGui::InputText( "Filter Lane Id", m_laneFilter, 255, ImGuiInputTextFlags_CharsDecimal );

		String idFilter = m_idFilter;
		String laneFilter = m_laneFilter;

		if ( ImGui::Button( "Delete all requests " ) )
		{
			m_spawnResultDebug.Clear();
			m_spawnResultLUT.Clear();
		}

		auto spawnResultDebug = m_spawnResultDebug;

		if ( ( m_filterSelected || m_showSelected ) && m_persistentData )
		{
			red::Set< String > selected = { red::PoolFrame() };
			for ( const TrafficLaneIndex laneIndex : m_persistentData->m_debugState.m_selectedLanes )
			{
				selected.Insert( m_persistentData->GetLaneUID( laneIndex ).ToDebugString() );
			}

			red::alg::EraseIf( spawnResultDebug, [&]( auto& value )
			{
				return selected.Find( value.m_laneId.ToDebugString() ) == selected.End();
			} );
		}

		red::alg::EraseIf( spawnResultDebug, [&]( auto& value )
		{
			return ( GetTrafficTime() - value.m_time ) > m_filteredFrameTime;
		} );

		if ( !idFilter.Empty() )
		{
			red::alg::EraseIf( spawnResultDebug, [&]( auto& value )
			{
				return !value.m_entityID.ToDebugString().Contains( idFilter );
			} );
		}

		if ( !laneFilter.Empty() )
		{
			red::alg::EraseIf( spawnResultDebug, [&]( auto& value )
			{
				return !value.m_laneId.ToDebugString().Contains( idFilter );
			} );
		}


		std::sort( spawnResultDebug.Begin(), spawnResultDebug.End(), []( auto& a, auto& b )
		{
			return a.m_time < b.m_time;
		} );

		red::ImGui::BeginChild( "DataSpawn", {}, true, ImGuiWindowFlags_AlwaysVerticalScrollbar );

		for ( auto& value : spawnResultDebug )
		{
			{
				VisitLane( value.m_laneId, [&]( auto& trafficLane )//TrafficLane&
				{
					auto& slots = trafficLane.GetSlots();
					for ( auto& slot : slots )
					{
						if ( auto slotLocked = slot.Lock() )
						{
							builder.AddWorldText( slotLocked->movementState.position + Vector3::EZ(), 0, 0, String::Printf( "%s", slotLocked->m_entityID.ToDebugString().AsChar() ).AsChar() );
						}
					}
				} );
			}

			auto it = m_spawnResultLUT.Find( value.m_id );
			if ( it == m_spawnResultLUT.End() ) continue;

			red::ImGui::BeginChild( String::Printf( "Spawn_%d", value.m_id ).AsChar(), {}, true );

			ImGui::PushStyleColor( ImGuiCol_Text, value.IsFailed() ? ImVec4( ImColor( 255, 0, 0 ) ) : ImVec4( ImColor( 0, 255, 0 ) ) );

			ImGui::Text( "%s (%d)", value.m_entityID.ToDebugString().AsChar(), value.m_id );

			ImGui::Checkbox( String::Printf( "Alive: %6.3f (Created at: %6.3f)", GetTrafficTime() - value.m_time, value.m_time ).AsChar(), &( m_spawnResultDebug[it.Value()].m_show ) );
			ImGui::SameLine();
			ImGui::Text( "%s", value.m_laneId.ToDebugString().AsChar() );
			ImGui::SameLine();
			ImGui::Text( String::Printf( "%6.3f", value.m_length ).AsChar() );

			if ( m_showDetails )
			{
				ImGui::BeginChild( String::Printf( "Details(%d)", value.m_id ).AsChar(), ImVec2( 300.0f, 100.0f ), true );
				ImGui::Text( String::Printf( "Dimensions W %6.3f L %6.3f H %6.3f", value.m_query.m_dimensions.width, value.m_query.m_dimensions.length, value.m_query.m_dimensions.height ).AsChar() );
				ImGui::Text( String::Printf( "Limits %6.3f - %6.3f", value.m_query.m_limit.first, value.m_query.m_limit.second ).AsChar() );
				ImGui::Text( String::Printf( "Char record %s", value.m_query.m_characterRecordId.ToString().AsChar() ).AsChar() );
				ImGui::Text( String::Printf( "Entity ID %s", value.m_query.m_expectedEntityID.ToDebugString().AsChar() ).AsChar() );
				ImGui::EndChild();
			}

			ImGui::PopStyleColor();
			red::ImGui::EndChild();
		}

		red::ImGui::EndChild();
		red::ImGui::EndChild();
		ImGui::End();



		builder.EnableTransparency( true );

		for ( auto& value : spawnResultDebug )
		{
			if ( value.m_show || m_showSelected )
			{
				auto& path = value.m_segmentedPath->m_subSegmentedPath;

				for ( auto i = 0u; i < value.m_segments.Size(); ++i )
				{
					auto polyColor = Color::GREEN();
					auto edgeColor = Color::GREEN();
					auto edgeWidth = 0.01f;

					{
						if ( value.m_segments[i] == TrafficSpawning::SpawnSupport::SegmentValidity::Occupied )
						{
							edgeColor = polyColor = Color::RED();
						}

						if ( value.m_chosenFreeSegment != std::numeric_limits<Uint32>::max() )
						{
							auto& freeSegment = value.m_freeSegments[value.m_chosenFreeSegment];

							if ( ( i >= freeSegment.startIndex ) && ( i <= freeSegment.endIndex ) )
							{
								edgeColor = Color::GREEN();
							}
						}

						if ( i == value.m_chosenSubSegment ) continue;
						if ( ( i >= value.m_spawnGapStart ) && ( i <= value.m_spawnGapEnd ) ) continue;
					}

					auto polygon = path.GetPolygonTemp<Vector3>( i, zOffset.AsVector3() );

					builder.SetColor( polyColor, 0.4f );

					builder.AddSolidPolygon( polygon.TypedData(), 4 );

					builder.SetColor( edgeColor, 1.0f );

					builder.AddFatLine( polygon[0], polygon[1], edgeWidth );
					builder.AddFatLine( polygon[1], polygon[2], edgeWidth );
					builder.AddFatLine( polygon[2], polygon[3], edgeWidth );
					builder.AddFatLine( polygon[3], polygon[0], edgeWidth );
				}

				if ( value.m_spawnGapStart != std::numeric_limits<Uint32>::max() )
				{
					for ( auto i = value.m_spawnGapStart; i <= value.m_spawnGapEnd; ++i )
					{
						if ( i == value.m_chosenSubSegment ) continue;

						builder.SetColor( Color::ORANGE(), 1.0f );

						auto polygon = path.GetPolygonTemp<Vector3>( i, zOffset.AsVector3() );

						builder.AddSolidPolygon( polygon.TypedData(), 4 );

						builder.AddFatLine( polygon[0], polygon[1], 0.05f );
						builder.AddFatLine( polygon[1], polygon[2], 0.05f );
						builder.AddFatLine( polygon[2], polygon[3], 0.05f );
						builder.AddFatLine( polygon[3], polygon[0], 0.05f );
					}
				}

				if ( value.m_chosenSubSegment != std::numeric_limits<Uint32>::max() )
				{
					builder.SetColor( Color::YELLOW(), 1.0f );

					auto polygon = path.GetPolygonTemp<Vector3>( value.m_chosenSubSegment, zOffset.AsVector3() );

					builder.AddSolidPolygon( polygon.TypedData(), 4 );

					builder.AddFatLine( polygon[0], polygon[1], 0.05f );
					builder.AddFatLine( polygon[1], polygon[2], 0.05f );
					builder.AddFatLine( polygon[2], polygon[3], 0.05f );
					builder.AddFatLine( polygon[3], polygon[0], 0.05f );
				}
			}
		}
	}
#endif
}

//================================================================================
void RuntimeSystemTraffic::SanitizeSpawnDebugs() const
{
	auto trafficTime = GetTrafficTime();

	RED_SCOPE_LOCK( m_spawnRqDebugLock );

	m_spawnResultDebug.Remove( std::remove_if( m_spawnResultDebug.Begin(), m_spawnResultDebug.End(), [&]( auto& v )
	{
		return ( v.m_time + m_timeFrame < trafficTime );
	} ), m_spawnResultDebug.End() );

	m_spawnResultLUT.Clear();

	for ( auto i = 0u; i < m_spawnResultDebug.Size(); ++i )
	{
		m_spawnResultLUT[m_spawnResultDebug[i].m_id] = i;
	}

}

//================================================================================
void RuntimeSystemTraffic::AddDisposalRequestLog( const ent::EntityID& rq )
{
	RED_SCOPE_LOCK( m_debugLock );
	m_disposalRequests.PushBack( rq );
}

#endif

	// --------------------------------------------------------------------------------
#ifdef RED_USE_IMGUI
void RuntimeSystemTraffic::DrawTrafficMap( const RenderDebugContext& context ) const
{
	if ( m_persistentData == nullptr ) return;

	if ( !red::ImGui::IsWindowRequested( s_debugOverlayName ) )
	{
		return;
	}

	if ( !ImGui::Begin( s_debugOverlayName.AsChar() ) )
	{
		ImGui::End();
		return;
	}

	if ( m_persistentData )
	{
		m_persistentData->DrawDebug( context.m_position, context.m_forward, context.debugDrawer, *this );
		m_persistentData->DrawSubgraphsDebug( context.m_position, context.m_forward, context.debugDrawer );
	}

	thread_local static const Float c_godRayRadius = 2.5f;
	rend::FragmentBuilder builderRays( context.debugDrawer );
	const auto cameraPosition = context.m_position;

	static constexpr ImGuiCanvasFlags flags = ImGuiCanvasFlags_ShowAxes | ImGuiCanvasFlags_ShowScale | ImGuiCanvasFlags_DrawCenter | ImGuiCanvasFlags_AllowMouseScrollZoom | ImGuiCanvasFlags_AllowMouseDrag | ImGuiCanvasFlags_HighlightOnHover;
	red::ImGui::CanvasBeginData canvasBeginData;
	canvasBeginData.minScale = 0.01f;
	canvasBeginData.maxScale = 10.f;

	static Bool allowMapToggle = true, showDeadEndRays = false, showInconsistentIntersectionRays = false, showMap = false,
		showDeadEnds = true, showBrokenIntersections = true, useFilteringRadius = false, showConnectionAngles = false, showConnectionGap = false, showCameraPosition = true;

	static Bool showAllDeadEnds = false;

	static Float cameraSize = 25.f, dataErrorsSize = 12.f, filteringRadius = 500.f, zoomValueThresholdForOptimize = 0.25f, connectionAngle = TrafficPersistentData::c_sharpAngle, connectionGap = 4.0f;

	static Float dist = 200.0f;

	if ( ImGui::Button( "Dump Persistent Data" ) )
	{
		DumpPersistentData( "roads.gv", context.m_position, dist, []( auto& lane )
		{
			return lane.HasFlag( world::TrafficLanePersistent::Flags::Road ) || lane.HasFlag( world::TrafficLanePersistent::Flags::Intersection );
		} );

		DumpPersistentData( "pavements.gv", context.m_position, dist, []( auto& lane )
		{
			return lane.HasFlag( world::TrafficLanePersistent::Flags::CrossWalk ) || lane.HasFlag( world::TrafficLanePersistent::Flags::Pavement );
		} );
	}
	ImGui::SameLine();
	ImGui::InputFloat( "Dump distance", &dist, 1.0f, 10.0f );

	ImGui::Checkbox( "Show traffic map", &showMap );
	ImGui::Checkbox( "Show camera position", &showCameraPosition );
	ImGui::Checkbox( "Allow map toggle", &allowMapToggle );
	ImGui::Checkbox( "Show all dead ends", &showAllDeadEnds );

	if ( !allowMapToggle )
	{
		ImGui::Checkbox( "use filtering radius", &useFilteringRadius );

		ImGui::InputFloat( "camera size on the map", &cameraSize, 1.f, 10.f );

		if ( useFilteringRadius )
		{
			ImGui::InputFloat( "filtering radius", &filteringRadius, 10.f, 100.f );
		}
	}

	ImGui::Checkbox( "Show dead ends on map", &showDeadEnds );
	ImGui::SameLine();
	ImGui::Checkbox( "Show broken intersections on map", &showBrokenIntersections );
	ImGui::Checkbox( "Show god rays for dead ends", &showDeadEndRays );
	ImGui::SameLine();
	ImGui::Checkbox( "Show god rays for broken intersection", &showInconsistentIntersectionRays );
	ImGui::InputFloat( "data errors size", &dataErrorsSize, 1.f, 10.f );

	ImGui::InputFloat( "zoom value threshold for preview mode", &zoomValueThresholdForOptimize, 0.1f, 1.f );
	ImGui::Checkbox( "Show connection angles", &showConnectionAngles );
	if ( showConnectionAngles )
	{
		ImGui::InputFloat( "angle value", &connectionAngle, 1.f, 10.f );
	}
	ImGui::Checkbox( "Show connection gaps", &showConnectionGap );
	if ( showConnectionGap )
	{
		ImGui::InputFloat( "gap value", &connectionGap, .1f, 10.f );
	}

	red::ImGui::BeginCanvas( s_debugOverlayName.AsChar(), canvasBeginData, flags );
	if ( !allowMapToggle )
	{
		red::ImGui::CanvasSetOrigin( ToImVec2( cameraPosition ) );
	}
	red::DynArray< world::TrafficLaneUID > deadEndsIds{ red::PoolFrame() };

	static auto s_showDynamicCollisions = false;
	static auto s_showDisposalMap = false;
	static auto s_showExpectedCollisions = false;

	static auto s_showDynamicCollisionsData = false;

	static auto s_showNullAreaCollisions = false;
	static auto s_showNullAreaCollisionsLogicRepresentation = false;

	static auto s_showDisposalMapData = false;
	static auto s_showDisposalLookup = false;
	static auto s_linearizationFactor = 0.0f;
	static auto s_showDisposalRequests = false;

	static auto s_showExpectedCollisionsData = false;
	static auto s_showStaticCollisions = false;


	static auto s_showSpawnDebugInvalid = false;
	static auto s_showSpawnDebugValid = false;


	static auto s_showPresentTrafficLanes = false;
	static auto s_showDumpedTrafficLanes = false;
	static auto s_showIntersections = false;
	static auto s_showUsedLanes = false;

	static auto s_showTrafficLanes = true;
	static auto s_showSideWalkLanes = true;
	static auto s_othersLane = false;

	static auto s_swapSlotA = 0;
	static auto s_swapSlotB = 0;
	static auto s_showSwaps = false;


	ImGui::Checkbox( "Dynamic Collisions", &s_showDynamicCollisions );
	if ( s_showDynamicCollisions )
	{
		ImGui::SameLine();
		ImGui::Checkbox( "Dynamic Data", &s_showDynamicCollisionsData );

		ImGui::Checkbox( "Enable snapshots", &m_dynamicCollisionsMap.m_snapshotsEnabled );
		ImGui::SameLine();
		ImGui::Checkbox( "Show snapshots", &m_dynamicCollisionsMap.m_showSnapshots );
		
		if ( ImGui::Button( "<-" ) ) --m_dynamicCollisionsMap.m_currentSnapshot;
		ImGui::SameLine();
		ImGui::SliderInt( "Dynamic Snapshot", &( m_dynamicCollisionsMap.m_currentSnapshot ), 0u, m_dynamicCollisionsMap.m_snapshots.Size() - 1 );
		ImGui::SameLine();
		if ( ImGui::Button( "->" ) ) ++m_dynamicCollisionsMap.m_currentSnapshot;
		ImGui::InputFloat( "Snapshot Time Frame", &( m_dynamicCollisionsMap.m_timeFrame ), 0.1f, 1.0f, 2 );
	}

	ImGui::Checkbox( "Show All Null Area Collisions", &s_showNullAreaCollisions );
	if ( s_showNullAreaCollisions )
	{
		ImGui::SameLine();
		ImGui::Checkbox( "Logic Representation", &s_showNullAreaCollisionsLogicRepresentation );
	}

	ImGui::Checkbox( "Disposal Collisions", &s_showDisposalMap );
	if ( s_showDisposalMap )
	{
		ImGui::SameLine();
		ImGui::Checkbox( "Disposal Data", &s_showDisposalMapData );
		ImGui::SameLine();
		ImGui::Checkbox( "Disposal Lookup", &s_showDisposalLookup );
		ImGui::SameLine();
		ImGui::Checkbox( "Disposal Queries", &s_showDisposalRequests );
		ImGui::InputFloat( "Linearization factor debug", &s_linearizationFactor, 0.0001f, 0.01f, 5 );
	}

	ImGui::Checkbox( "Expected Collisions", &s_showExpectedCollisions );
	if ( s_showExpectedCollisions )
	{
		ImGui::SameLine();
		ImGui::Checkbox( "Expected Data", &s_showExpectedCollisionsData );
	}

	ImGui::Checkbox( "Static Collisions", &s_showStaticCollisions );

	ImGui::Checkbox( "Present Traffic Lanes", &s_showPresentTrafficLanes );
	ImGui::Checkbox( "Dumped Traffic Lanes", &s_showDumpedTrafficLanes );
	ImGui::Checkbox( "Show Intersections", &s_showIntersections );
	ImGui::Checkbox( "Show used lanes", &s_showUsedLanes );

	ImGui::Checkbox( "Show traffic lanes", &s_showTrafficLanes ); ImGui::SameLine();
	ImGui::Checkbox( "Show sidewalks lanes", &s_showSideWalkLanes ); ImGui::SameLine();
	ImGui::Checkbox( "Show all lanes", &s_othersLane );

	ImGui::Checkbox( "Show invalid spawn request ", &s_showSpawnDebugInvalid );
	ImGui::Checkbox( "Show valid spawn request ", &s_showSpawnDebugValid );

	if ( ImGui::Button( "Clear invalid spawn request " ) )
	{
		RED_SCOPE_LOCK( m_debugLock );
		m_invalidSpawnRequests.Clear();
	}

	if ( ImGui::Button( "Clear valid spawn request " ) )
	{
		RED_SCOPE_LOCK( m_debugLock );
		m_validSpawnRequests.Clear();
	}

	if ( s_showSpawnDebugInvalid || s_showSpawnDebugValid )
	{
		rend::FragmentBuilder builder( context.debugDrawer );

		RED_SCOPE_LOCK( m_debugLock );

		if ( s_showSpawnDebugValid )
		{
			builder.SetColor( Color::WHITE( 100 ) );
			for ( auto& s : m_validSpawnRequests )
			{
				builder.AddSolidSphere( s.pos, s.r );
			}
		}
		if ( s_showSpawnDebugInvalid )
		{
			builder.SetColor( Color::BLACK( 100 ) );
			for ( auto& s : m_invalidSpawnRequests )
			{
				builder.AddSolidSphere( s.pos, s.r );
			}
		}
	}

	if ( s_showDynamicCollisions )
	{
		m_dynamicCollisionsMap.OnRenderDebug( context, s_showDynamicCollisionsData );
	}

	if ( s_showNullAreaCollisions )
	{
		DrawNullAreaCollisionsOnWorld( context, s_showNullAreaCollisionsLogicRepresentation );
	}

	if ( s_showDisposalMap )
	{
		m_disposalMap.OnRenderDebug( context, s_showDisposalMapData );

		if ( s_showDisposalLookup )
		{
			rend::FragmentBuilder builder( context.debugDrawer );
			builder.EnableTransparency( true );

			m_disposalMap.VisitAllEntries( [&]( auto& entry )
			{
				red::DynArray< Box > boxes{ red::PoolDebug() };
				for ( auto& sphere : entry )
				{
					boxes.PushBack( Box( sphere.GetCenter(), sphere.GetRadius() ) );
				}

				for ( auto& box : world::PathPolygons::LinearizeToBoxSet( boxes, red::PoolDebug(), s_linearizationFactor ) )
				{
					builder.SetColor( Color::RED() );
					builder.AddWireBox( box.first );
					builder.SetColor( Color::RED( 50 ) );
					builder.AddSolidBox( box.first );
				}

				return true;
			} );
		}

		if ( s_showDisposalRequests )
		{
			RED_SCOPE_LOCK( m_debugLock );

			if ( red::ImGui::BeginChild( "Disposal Requests", {}, true, ImGuiWindowFlags_AlwaysAutoResize ) )
			{
				for ( auto& rq : m_disposalRequests )
				{
					ImGui::Text( "%s", rq.ToDebugString().AsChar() );
				}
			}

			red::ImGui::EndChild();
		}
	}

	if ( s_showExpectedCollisions )
	{
		m_expectedCollisionsMap.OnRenderDebug( context, s_showExpectedCollisionsData );
	}

	if ( s_showStaticCollisions )
	{
		m_staticCollisions.OnRenderDebug( context, false );
	}

	//=====   Lane Cache   ===========================================================

	builderRays.EnableTransparency( true );
	auto validateLane = [](auto& lane, auto& box)
	{
		auto show = s_othersLane;

		if ( !show )
		{
			if ( !lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::GPSOnly ) &&
				 !lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::TrafficDisabled ) )
			{
				if ( s_showTrafficLanes )
				{
					show |= lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::FromRoadSpline );
					show |= lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::Road );
				}

				if ( s_showSideWalkLanes )
				{
					show |= lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::Pavement );
					show |= lane->GetPersistentData().HasFlag( TrafficLanePersistent::Flags::CrossWalk );
				}
			}
		}

		if ( show )
		{
			box.AddBox( lane->GetPersistentData().ComputePolygonBox() );
		}
	};

	if ( s_showPresentTrafficLanes )
	{
		for ( auto it : m_nodeToLaneLUT )
		{
			auto drawBox = Box::EMPTY();

			if ( !m_nodeDump.Exist( it.Key() ) )
			{
				for ( auto& id : it.Value() )
				{
					auto lane = FindLane( id );
					RED_ASSERT( lane );

					validateLane( lane, drawBox );
				}

				builderRays.SetColor( Color::YELLOW( 100 ) );
				builderRays.AddSolidBox( drawBox );

				builderRays.SetColor( Color::BLACK() );
				builderRays.AddWireBox( drawBox );
			}
		}
	}

	if ( s_showDumpedTrafficLanes )
	{
		for ( auto it : m_nodeToLaneLUT )
		{
			auto drawBox = Box::EMPTY();

			if ( !m_nodeDump.Exist( it.Key() ) )
			{
				for ( auto& id : it.Value() )
				{
					auto lane = FindLane( id );
					RED_ASSERT( lane );

					validateLane( lane, drawBox );
				}

				builderRays.SetColor( Color::RED( 100 ) );
				builderRays.AddSolidBox( drawBox );

				builderRays.SetColor( Color::BLACK() );
				builderRays.AddWireBox( drawBox );
			}
		}
	}

	if ( s_showIntersections )
	{
		for ( auto it : m_nodeToLaneLUT )
		{
			auto drawBox = Box::EMPTY();

			if ( m_intersections.Find( it.Key() ) != m_intersections.End() )
			{
				for ( auto& id : it.Value() )
				{
					auto lane = FindLane( id );
					RED_ASSERT( lane );

					validateLane( lane, drawBox );
				}

				builderRays.SetColor( Color::BLUE( 100 ) );
				builderRays.AddSolidBox( drawBox );

				builderRays.SetColor( Color::BLACK() );
				builderRays.AddWireBox( drawBox );
			}
		}
	}

	if ( s_showUsedLanes )
	{
		for ( auto it : m_nodeToLaneLUT )
		{
			auto drawBox = Box::EMPTY();

			auto nodeIt = m_nodeUsage.Find( it.Key() );
			if ( nodeIt != m_nodeUsage.End() )
			{
				if ( nodeIt.Value() != 0u )
				{
					auto drawBox = Box::EMPTY();

					for ( auto& id : it.Value() )
					{
						auto lane = FindLane( id );
						RED_ASSERT( lane );

						validateLane( lane, drawBox );
					}

					auto dumpIt = m_nodeDump.Find( it.Key() );
					builderRays.SetColor( dumpIt != m_nodeDump.End() ? Color::RED( 100 ) : Color::GREEN( 100 ));
					builderRays.AddSolidBox( drawBox );

					builderRays.SetColor( Color::BLACK() );
					builderRays.AddWireBox( drawBox );

				}
			}
		}		
	}

	
	
	//================================================================================

	if ( showMap )
	{
		static const thread_local ImVec4 okLaneColor = red::ImGui::GREEN();
		static const thread_local ImVec4 deadEndLaneColor = red::ImGui::RED();

		for ( const auto &lane : m_persistentData->GetLanes() )
		{

			auto showDeadEnd = false;

			Float distanceFromCamera = ( lane.m_outline[0].AsVector2() - cameraPosition.AsVector2() ).Mag();
			if ( allowMapToggle || !useFilteringRadius || distanceFromCamera < filteringRadius )
			{
				if ( ShouldDisplayAsDeadEnd( lane, GetRuntimeState(), showAllDeadEnds ) )
				{
					showDeadEnd = true;
					deadEndsIds.PushBack( lane.GetLaneId() );
				}

				const Uint32 lanePointsCount = lane.m_outline.Size();

				auto laneColor = okLaneColor;

				if ( red::ImGui::CanvasGetScale( s_debugOverlayName.AsChar() ) < zoomValueThresholdForOptimize )
				{
					red::ImGui::CanvasLine( ToImVec2( lane.m_outline[0] ), ToImVec2( lane.m_outline[lanePointsCount - 1] ), laneColor, 1.f );
				}
				else
				{
					auto outline = lane.m_outline;
					auto deadEndIndex = std::numeric_limits<Uint32>::max();
					{
						if ( showDeadEnd )
						{
							auto deadEndStart = GetRuntimeState().GetDeadEndStart( lane );

							if ( deadEndStart != std::numeric_limits<Float>::infinity() )
							{
								if ( math::EqualsZero( deadEndStart ) )
								{
									deadEndIndex = 0u;
								}
								else
								{
									if ( math::AlmostEquals( deadEndStart, lane.m_length ) )
									{
										deadEndIndex = outline.Size() - 1;
									}
									else
									{
										for ( auto i = 0u; i < outline.Size() - 1; ++i )
										{
											auto ptA = outline[i];
											auto ptB = outline[i + 1];

											auto a2b = ptB - ptA;
											auto d = a2b.Mag();

											if ( d > deadEndStart ) //on this segment we get a dead end
											{
												deadEndIndex = i + 1;
												outline.Insert( outline.Begin() + i + 1, ptA + a2b.Normalized() * deadEndStart );
												break;
											}
											else
											{
												deadEndStart -= d;
											}
										}
									}
								}
							}
						}
					}


					for ( Uint32 pI = 1u; pI < lane.m_outline.Size(); ++pI )
					{
						math::Vector3 segOffset( math::Vector3::ZEROS() );
						if ( lane.HasFlag( TrafficLanePersistent::Bidirectional ) )
						{
							const math::Vector3 segDir = lane.m_outline[pI] - lane.m_outline[pI - 1];
							segOffset = segDir.Cross( math::Vector3::EZ() ).Normalized() * 0.3f;
						}

						if ( showDeadEnd )
						{
							if ( ( pI - 1 ) >= deadEndIndex )
							{
								laneColor = deadEndLaneColor;
							}
						}

						red::ImGui::CanvasLine( ToImVec2( lane.m_outline[pI - 1] + segOffset ), ToImVec2( lane.m_outline[pI] + segOffset ), laneColor, 0.01f );
					}
				}


				struct LaneConnection
				{
					TrafficLaneUID m_laneA;
					TrafficLaneUID m_laneB;

					LaneConnection() = default;
					LaneConnection( const TrafficLaneUID& lA, const TrafficLaneUID& lB )
						: m_laneA( lA )
						, m_laneB( lB )
					{}

					RED_INLINE Bool operator ==( const LaneConnection& other ) const
					{
						return ( m_laneA == other.m_laneA && m_laneB == other.m_laneB )
							|| ( m_laneA == other.m_laneB && m_laneB == other.m_laneA );
					}
					RED_INLINE Bool operator !=( const LaneConnection& other ) const
					{
						return !operator==( other );
					}
					RED_INLINE LaneConnection& operator=( Int32 value )
					{
						m_laneA = TrafficLaneUID( value, value, value, value );
						m_laneB = TrafficLaneUID( value, value, value, value );
						return *this;
					}
					RED_INLINE Bool IsValid() const
					{
						return m_laneA.IsValid() && m_laneB.IsValid();
					}
				};

				if ( !showConnectionAngles && !showConnectionGap )
				{
					continue;
				}

				static LaneConnection selectedConnection;
				for ( const auto& outLane : lane.m_outLanes )
				{
					const TrafficLaneUID laneId = lane.GetLaneId(); 

					auto destinationLane = GetPersistentLane( outLane.laneIndex );
					const TrafficLaneUID destinationLaneId = destinationLane->GetLaneId();
					if ( destinationLaneId == laneId.GetReverse()
						 || destinationLane->HasFlag( TrafficLanePersistent::Flags::TrafficDisabled )
						 || outLane.leadsToDeadEnd )
					{
						continue;
					}

					if ( showConnectionAngles )
					{
						if ( !selectedConnection.IsValid() || selectedConnection == LaneConnection( destinationLaneId, laneId ) )
						{
							Vector3 lastSegStartPos, lastSegDir, nextSegStartPos, nextSegDir;
							const Float angleValue = GetApproximateLaneConnexionAngle( lane.GetOutline(), destinationLane->GetOutline(), outLane.thisLaneExitPosition, outLane.nextLaneEntryPosition, &lastSegStartPos, &lastSegDir, &nextSegStartPos, &nextSegDir );
							if ( angleValue >= connectionAngle )
							{
								const auto startFirstSegment = lastSegStartPos;
								const auto endFirstSegment = lastSegStartPos + lastSegDir;
								const auto startNextSegment = nextSegStartPos;
								const auto endNextSegment = nextSegStartPos + nextSegDir;

								Bool connectionHovered = false;
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( startFirstSegment ), 1.f, red::ImGui::DARK_ORANGE() );
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( endFirstSegment ), 1.f, red::ImGui::DARK_ORANGE() );
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( startNextSegment ), 1.f, red::ImGui::DARK_ORANGE() );
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( endNextSegment ), 1.f, red::ImGui::DARK_ORANGE() );

								auto laneColor = red::ImGui::DARK_ORANGE();
								if ( connectionHovered )
								{
									laneColor = red::ImGui::LIGHT_ORANGE();

									const auto directionA = lastSegDir.AsVector2().Normalized();
									const auto directionB = nextSegDir.AsVector2().Normalized();
									red::ImGui::DrawDirectionTriangle( startFirstSegment.AsVector2() + ( directionA * ( endFirstSegment - startFirstSegment ).Mag() * 0.5f ), directionA, laneColor, 2.f );
									red::ImGui::DrawDirectionTriangle( startNextSegment.AsVector2() + ( directionB * ( endNextSegment - startNextSegment ).Mag() * 0.5f ), directionB, laneColor, 2.f );

									if ( selectedConnection.IsValid() )
									{
										red::ImGui::CanvasText( ToImVec2( endFirstSegment ), "B", red::ImGui::WHITE(), 15.f );
										red::ImGui::CanvasText( ToImVec2( startNextSegment ), "\nC", red::ImGui::WHITE(), 15.f );
										red::ImGui::CanvasText( ToImVec2( endNextSegment ), "\nD", red::ImGui::WHITE(), 15.f );
										red::ImGui::CanvasText( ToImVec2( startFirstSegment ), "\nA", red::ImGui::WHITE(), 15.f );
									}
								}

								red::ImGui::CanvasText( ToImVec2( startFirstSegment ), String::Printf( "\n%d", static_cast<Int32>( angleValue ) ).AsChar(), laneColor, 15.f );
								red::ImGui::CanvasLine( ToImVec2( startFirstSegment ), ToImVec2( endFirstSegment ), laneColor, 0.25f );
								red::ImGui::CanvasLine( ToImVec2( startNextSegment ), ToImVec2( endNextSegment ), laneColor, 0.25f );

								if ( connectionHovered && ImGui::IsMouseClicked( 0, false )
									 && selectedConnection != LaneConnection( destinationLaneId, laneId ) )
								{
									selectedConnection = LaneConnection( destinationLaneId, laneId );
								}
							}
						}
					}

					if ( showConnectionGap )
					{
						if ( !selectedConnection.IsValid() || selectedConnection == LaneConnection( destinationLaneId, laneId ) )
						{
							const auto endFirstSegment = CalcPositionOnSpline<Vector3>( lane.GetOutline(), outLane.thisLaneExitPosition );
							const auto startNextSegment = CalcPositionOnSpline<Vector3>( destinationLane->GetOutline(), outLane.nextLaneEntryPosition );
							const Float distance = endFirstSegment.DistanceTo( startNextSegment );
							if ( distance >= connectionGap )
							{
								static const auto gapColor = red::ImGui::LIGHT_MAGENTA();
								red::ImGui::CanvasText( ToImVec2( endFirstSegment ), String::Printf( "\n%d", static_cast<Int32>( distance ) ).AsChar(), gapColor, 15.f );
								red::ImGui::CanvasLine( ToImVec2( endFirstSegment ), ToImVec2( startNextSegment ), gapColor, 0.25f );

								Bool connectionHovered = false;
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( endFirstSegment ), 1.f, gapColor );
								connectionHovered |= red::ImGui::CanvasCircleFilled( ToImVec2( startNextSegment ), 1.f, gapColor );

								const auto direction = startNextSegment - endFirstSegment;
								red::ImGui::DrawDirectionTriangle( endFirstSegment.AsVector2() + ( direction.AsVector2().Normalized() * ( direction ).Mag() * 0.5f ), direction.AsVector2().Normalized(), gapColor, 2.f );

								if ( connectionHovered && ImGui::IsMouseClicked( 0, false )
									 && selectedConnection != LaneConnection( destinationLaneId, laneId ) )
								{
									selectedConnection = LaneConnection( destinationLaneId, laneId );
								}
							}
						}
					}

					if ( ImGui::IsMouseClicked( 1, false ) )
					{
						selectedConnection = 0;
					}
				}
			}
		}
	}

	if ( showBrokenIntersections )
	{
		DrawDataErrors( builderRays, *m_persistentData, m_persistentData->m_brokenUIDs, showBrokenIntersections, red::ImGui::RED(),
						dataErrorsSize, showInconsistentIntersectionRays, Color::RED( 128 ), c_godRayRadius );
	}
	if ( showDeadEnds )
	{
		DrawDataErrors( builderRays, *m_persistentData, deadEndsIds, showDeadEnds, red::ImGui::BLACK(),
						dataErrorsSize, showDeadEndRays, Color::BLACK( 128 ), c_godRayRadius );

	}

	if ( showCameraPosition )
	{
		red::ImGui::CanvasCircleFilled( ToImVec2( cameraPosition ), cameraSize, red::ImGui::BLUE( 180u ) );
	}

	red::ImGui::EndCanvas();
	ImGui::End();

}
#endif
	// --------------------------------------------------------------------------------

Uint32 RuntimeSystemTraffic::GetSlotsCountOnLane( const TrafficLaneUID& laneId ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	return GetSlotsCountOnLane_NoLock( laneId );
}

// --------------------------------------------------------------------------------

Uint32 RuntimeSystemTraffic::GetSlotsCountOnLane_NoLock( const TrafficLaneUID& laneId ) const
{
	auto lane = FindLane( laneId );
	return lane != nullptr ? lane->GetSlotsCount() : 0;
}

// --------------------------------------------------------------------------------

Uint32 RuntimeSystemTraffic::GetPreferredSlotsCountOnLane( const TrafficLaneUID& laneId, world::TrafficMovementBehavior type ) const
{
	const TrafficLanePersistent* destinationLane = FindPersistentLane( laneId );
	if ( destinationLane )
	{
		Float slotOccupancy = 1.0f;
		if ( type == TrafficMovementBehavior::Pedestrian )
		{
			slotOccupancy = math::Pow( Config::cvSlotLaneOccupancyAreaPedestrian.Get(), 2u );
		}
		else
		{
			// Cars can't occupy more than 1 slot in Y lane coordinate
			slotOccupancy = Config::cvSlotLaneOccupancyAreaVehicleLength.Get() * destinationLane->m_width;
		}

		const Uint32 maxSlots = static_cast<Uint32>( ::MFloor( destinationLane->m_area / slotOccupancy ) );
		return maxSlots;
	}
	return 0u;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::GetSlotsFromLane( const TrafficLaneUID& laneId, red::DynArray< TrafficSlotWeakPtr >& out_slots ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	auto lane = FindLane( laneId );
	if ( !lane )
	{
		return false;
	}

	out_slots.PushBack( lane->GetSlots() );
	return true;
}

void RuntimeSystemTraffic::GetSlotsFromLanes( red::ArraySpan< const TrafficLaneUID > laneIds, red::DynArray< TrafficSlotWeakPtr >& out_slots ) const
{
	for ( const TrafficLaneUID& laneId : laneIds )
	{
		auto lane = FindLane(laneId);
		if ( lane == nullptr )
		{
			continue;
		}

		out_slots.PushBack( lane->GetSlots() );
	}
}


// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RetrieveNearbySlots( const Vector4& worldPos, Float range, const std::function< Bool( TrafficSlot& ) >& validation ) const
{
	auto queryBox = Box::EMPTY();
	queryBox.AddPoint( worldPos );
	queryBox.Extrude( range );

	RED_SCOPE_SHARED_LOCK( m_lock );

	m_dynamicCollisionsMap.Query( queryBox, [&]( auto& representation )
	{
		if ( !representation.RepresentsTrafficSlot() ) return true;

		auto it = m_slotLUT.Find( world::SlotId( representation.GetUserData() ) );
		if ( it != m_slotLUT.End() )
		{
			if ( auto slot = it.Value().Lock() )
			{
				return validation( *slot );
			}
		}

		return true;
	} );
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::FillLaneBlockObstacles( const TrafficSlot& slot, red::DynArray< LaneBlockObstacle >& outObstacles, Float distance )
{
	TrafficLaneUID currentLaneId = slot.GetCurrentLaneID();
	if ( !currentLaneId.IsValid() ) return false;

	RED_SCOPE_SHARED_LOCK( m_lock );

	auto lane = FindLane( currentLaneId );
	if ( !lane )
	{
		return false;
	}

	lane->FillLaneBlockObstacles( slot, outObstacles, 0.0f );

	// check along distance
	Float sumDistance = 0.0f;
	Uint32 idx = slot.GetPositionProgression().GetCurrentLaneIndex() + 1;

	if ( idx < slot.GetGlobalPath().GetLanes().Size() )
	{
		const TrafficGlobalPath::Lane* nextLane = &slot.GetGlobalPath().GetLanes()[idx];

		while ( nextLane && nextLane->m_laneId.IsValid() && sumDistance < distance )
		{
			auto lane = FindLane( nextLane->m_laneId );
			if ( lane )
			{
				lane->FillLaneBlockObstacles( slot, outObstacles, 0.0f );
				sumDistance += lane->GetPersistentData().m_length;
			}
			else
			{
				break;
			}

			++idx;
			if ( idx < slot.GetGlobalPath().GetLanes().Size() )
				nextLane = &slot.GetGlobalPath().GetLanes()[idx];
			else
				nextLane = nullptr;
		}
	}

	return true;
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::RegisterLightChangeListener( const GlobalNodeID gid, const Uint32 groupIdx, TrafficLightChangeFunc listener, TrafficLightListenerID& outId )
{
	RED_SCOPE_LOCK( m_lock );
	return m_lightChangeListenerRegistry.RegisterLightChangeListener( gid, groupIdx, listener, outId );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UnregisterLightChangeListener( const GlobalNodeID gid, TrafficLightListenerID id )
{
	RED_SCOPE_LOCK( m_lock );
	m_lightChangeListenerRegistry.UnregisterLightChangeListener( gid, id );
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::UpdateGreenWaveFromNavPath( const nav::PathPtr& path, const Uint32 currentPlayerPathIndex, const Vector3& currentPlayerPosition, const Vector3& currentPlayerForwardVector, const Bool isPlayerInVehicle )
{
	RED_SCOPE_SHARED_LOCK( m_lock );
	m_greenWaveManager->UpdateFromNavPath( path, currentPlayerPathIndex, currentPlayerPosition, currentPlayerForwardVector, isPlayerInVehicle );
}

void RuntimeSystemTraffic::UpdateGreenWaveFromVehicleSlot( const world::TrafficSlotPtr& slot )
{
	RED_SCOPE_SHARED_LOCK( m_lock );
	m_greenWaveManager->UpdateFromVehicleSlot( slot );
}

// --------------------------------------------------------------------------------
const TrafficLaneEventsBroadcaster& RuntimeSystemTraffic::GetTrafficLaneEventsBroadcaster() const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	return *m_trafficLaneEventsBroadcaster;
}

// --------------------------------------------------------------------------------
TrafficLaneEventsBroadcaster& RuntimeSystemTraffic::GetTrafficLaneEventsBroadcaster()
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	return *m_trafficLaneEventsBroadcaster;
}

// --------------------------------------------------------------------------------

const world::TrafficPersistentData* RuntimeSystemTraffic::GetPersistentData() const
{
	return m_persistentData;
}

// --------------------------------------------------------------------------------

const TrafficRuntimeState& RuntimeSystemTraffic::GetRuntimeState() const
{
	return m_runtimeState;
}

// --------------------------------------------------------------------------------

TrafficRuntimeState& RuntimeSystemTraffic::GetRuntimeState()
{
	return m_runtimeState;
}

// --------------------------------------------------------------------------------

TrafficFilterOperation RuntimeSystemTraffic::NoDeadEndsFilter( const TrafficLanePersistent& lane, const TrafficConnectivityOutLane& outLane, const RuntimeSystemTraffic* trafficSystem )
{
	const TrafficLanePersistent& outLanePersistent = trafficSystem->GetPersistentData()->GetLane( outLane.laneIndex );
	if ( !CrowdAccessible( &outLanePersistent ) ) return TrafficFilterOperation::Remove;

	if ( outLane.leadsToDeadEnd )
	{
		return TrafficFilterOperation::Remove;
	}

	return TrafficFilterOperation::Keep;
}

// --------------------------------------------------------------------------------

Vector3 RuntimeSystemTraffic::GenerateRandomPositionOnLane( const TrafficLanePersistent* lanePersistent, Float xOffset, Float radius, Float& yOffset, Vector3* direction )
{
	red::ArraySpan< const Vector3 > laneOutline = lanePersistent->m_outline;
	const Float yOffsetLimit = lanePersistent->m_width * 0.5f;
	const Float posY = math::DefaultRandom().Get( math::ClampFromAbove( -yOffsetLimit + radius, 0.0f ), math::ClampFromBelow( yOffsetLimit - radius, 0.0f ) );
	yOffset = posY;

	const auto forward = ( laneOutline[1u] - laneOutline[0u] ).Normalized();
	const auto right = Vector4::Cross( forward, Vector4::EZ() ).Normalized3();
	return CalcPositionOnSpline<Vector3>( laneOutline, xOffset, direction ) + ( right * posY );
}

// --------------------------------------------------------------------------------

std::pair<Float,Float> RuntimeSystemTraffic::ComputeTrafficSpotXRange( const TrafficSpotCompiled& spot )
{
	const Float minXEnter = math::Max( spot.m_lanePosition.X - Config::cvTrafficSpotDetectionPrecision.Get(), 0.f );
	const Float maxXEnter = minXEnter + Config::cvTrafficSpotDetectionRange.Get();
	return { minXEnter, maxXEnter };
}

// --------------------------------------------------------------------------------

void RuntimeSystemTraffic::RegisterSpawnRequest( const Vector3& position, const Vector3& direction, Float radius )
{
	RED_SCOPE_LOCK( m_spawnLock );
	m_spawnRequests.PushBack( { m_trafficTime, position, radius * 2.0f } );
}

// --------------------------------------------------------------------------------

RuntimeSystemTraffic::SpawnValidityResult RuntimeSystemTraffic::CheckSpawnValidity( const Vector3& position, const Vector3& direction, Float radius, Bool registerRequest )
{
	PC_SCOPE( RuntimeSystemTraffic_CheckSpawnValidity );

	{
		RED_SCOPE_SHARED_LOCK( m_spawnLock );

		auto rqCheck = Sphere( position, radius );
		auto found = false;
		for ( auto& rq : m_spawnRequests )
		{
			if ( rq.Check( rqCheck ) )
			{
				found = true;
				break;
			}
		}
		if ( found ) return RuntimeSystemTraffic::SpawnValidityResult::RequestedPosition;
	}

	if ( registerRequest )
	{
		RegisterSpawnRequest( position, direction, radius );
	}

	constexpr Float c_maxSlotSearchRadius = 3.0f;	//TODO - bartlomiej.walecki - there should be one common place where sizes of slots are defined

	const auto nearPositionPredicate = []( const Vector3& slotPosition, const Vector3& slotDirection, Float slotRadius, const Vector3& otherElementPosition, Float otherElementRadius )
	{
		constexpr Float c_frontDistanceBuffer = 2.0f;

		const auto slotToOther = otherElementPosition - slotPosition;
		const Float slotDirectionDot = math::Clamp( slotDirection.Dot( slotToOther.Normalized() ), 0.f, 1.0f );
		const Float radiusBuffer = slotRadius + ( c_frontDistanceBuffer * slotDirectionDot );

		const Float maxCollidingDistSquared = math::Pow( otherElementRadius + slotRadius + radiusBuffer, 2u );
		if ( slotToOther.SquareMag() <= maxCollidingDistSquared )
		{
			return true;
		}
		return false;
	};

	auto collisionFound = false;
	auto box = Box( position, c_maxSlotSearchRadius );

	m_staticCollisions.DeepQuery( box, [ & ]( auto& collision )
	{
		collisionFound = nearPositionPredicate( position, direction, radius, collision.GetCenter(), collision.GetRadius() );
		return !collisionFound;
	} );

	if ( collisionFound )
	{
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
		RED_SCOPE_LOCK( m_debugLock );
		m_invalidSpawnRequests.PushBack( { m_trafficTime, position, radius } );
#endif
		return RuntimeSystemTraffic::SpawnValidityResult::StaticCollision;
	}

	m_dynamicCollisionsMap.Query( box, [ & ]( auto& stream )
	{
		stream.Query( box, [ & ]( auto& part )
		{
			collisionFound = nearPositionPredicate( position, direction, radius, part.GetPosition(), part.GetRadius() );
			return !collisionFound;
		} );

		return !collisionFound;
	} );

	if ( collisionFound )
	{
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
		RED_SCOPE_LOCK( m_debugLock );
		m_invalidSpawnRequests.PushBack( { m_trafficTime, position, radius } );
#endif
		return RuntimeSystemTraffic::SpawnValidityResult::DynamicCollision;
	}

	m_disposalMap.Query( box, [ & ]( auto& stream )
	{
		stream.Query( box, [ & ]( auto& part )
		{
			collisionFound = nearPositionPredicate( position, direction, radius, part.GetCenter(), part.GetRadius() );
			return !collisionFound;
		} );

		return !collisionFound;
	} );

	if ( collisionFound )
	{
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
		RED_SCOPE_LOCK( m_debugLock );
		m_invalidSpawnRequests.PushBack( { m_trafficTime, position, radius } );
#endif
		return RuntimeSystemTraffic::SpawnValidityResult::DisposalCollision;
	}

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	{
		RED_SCOPE_LOCK( m_debugLock );
		m_validSpawnRequests.PushBack( { m_trafficTime, position, radius } );
	}
#endif

	return RuntimeSystemTraffic::SpawnValidityResult::Success;
}


#if !defined( RED_CONFIGURATION_FINAL )
	// Exposed for functional tests.
Bool RuntimeSystemTraffic::GetCurrentLightColor( GlobalNodeID gid, Uint32 groupIdx, TrafficLightColor& outColor, String& outErrorInfo ) const
{
	RED_SCOPE_LOCK( m_lock );

	if ( !gid.IsDefined() )
	{
		outErrorInfo = red::String::Printf( "Provided invalid global node id [%hs]", gid.ToDebugString().AsChar() );
		return false;
	}

	TrafficIntersection* intersection = FindIntersection( gid );
	if ( !intersection )
	{
		outErrorInfo = red::String::Printf( "Cannot find intersection with global node id [%hs]", gid.ToDebugString().AsChar() );
		return false;
	}

	auto it = m_nodeToLaneLUT.Find( gid );
	if ( it != m_nodeToLaneLUT.End() )
	{
		for ( auto& lane : it.Value() )
		{
			auto stopExecution = false;
			VisitLane( lane, [&]( auto& lane )
			{
				stopExecution = lane.GetCurrentLightColor( groupIdx, outColor, outErrorInfo );
			} );

			if ( stopExecution ) break;
		}
	}
	
	outErrorInfo = red::String::Printf( "Cannot find light with groupIdx [%hu]", groupIdx );
	return false;
}

// --------------------------------------------------------------------------------
math::Color RuntimeSystemTraffic::GetLaneDebugRenderColor( const TrafficLaneUID& laneId ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	auto lane = FindLane( laneId );
	return ( lane ? lane->GetDebugRenderColor() : math::Color::WHITE() );
}
#endif

#if !defined( NO_DEBUG_CONSOLE )
// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::DEBUG_ReverseTrafficLights()
{
	for ( auto it = m_intersections.Begin(); it != m_intersections.End(); ++it )
	{
		it.Value()->DEBUG_ReverseLights();
	}
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::ConsoleCommandReverseTrafficLights( ::IDebugConsole* callingConsole, const red::DynArray< red::String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
{
	GAME_ASSERT( userData, TweakDB, "null check" );
	RuntimeSystemTraffic* trafficSystem = static_cast<RuntimeSystemTraffic *>( userData );
	trafficSystem->DEBUG_ReverseTrafficLights();
	return true;
}
#endif

//================================================================================
Bool RuntimeSystemTraffic::CheckSlotPositionValidity( world::TrafficLaneUID laneID, Float slotRadius, world::TrafficMovementBehavior slotMoveType, const Vector3& worldPos, const Vector3& direction )
{
	PC_SCOPE( RuntimeSystemTraffic_CheckSlotPositionValidity );

	if ( !IsLaneValidForUse( laneID ) ) return false;

	return EnsureLaneSlotCapacity( laneID, slotMoveType )
		&& CheckSpawnValidity( worldPos, direction, slotRadius ) == RuntimeSystemTraffic::SpawnValidityResult::Success
		&& !IsPointInsideNullArea( worldPos );
}

//================================================================================
RuntimeSystemTraffic::PedestrianPathValidityResult RuntimeSystemTraffic::CheckPedestrianPathValidity( world::TrafficLaneUID laneID, Float xOffsetOnLane, const TrafficOutLaneFilter& outLaneFilter, const TrafficPreferredOutLaneFilter& preferredLaneFilter, const TrafficNullAreaFilter& nullAreaFilter, world::TrafficGlobalPath& globalPath, world::TrafficLocalPath& localPath ) const
{
	PC_SCOPE( RuntimeSystemTraffic_CheckPedestrianPathValidity );

	if ( !IsLaneValidForUse( laneID ) ) return PedestrianPathValidityResult::InvalidForUse;

	GenerateGlobalPathContext pathContext( laneID, outLaneFilter, preferredLaneFilter, TrafficMovementBehavior::Pedestrian, xOffsetOnLane );

	for ( const auto& nullArea : m_activeNullAreas )
	{
		if ( nullAreaFilter( nullArea ) == TrafficFilterOperation::Keep )
		{
			pathContext.m_nullAreas.PushBack( nullArea );
		}
	}

	GenerateGlobalPathResult result;
	if ( GenerateTrafficPath( pathContext, result ) )
	{
		globalPath = *result.m_globalPath;
		localPath = *result.m_localPath;

		RED_ASSERT( result.m_length <= GenerateGlobalPathContext::s_maximumPedestrianPathLength );
		if ( result.m_length < GenerateGlobalPathContext::s_minimumPedestrianPathLength )
		{
			return PedestrianPathValidityResult::ShortPath;
		}
		else if ( IsPathCrossingNullArea( result.m_globalPath->GetOutline(), pathContext.m_nullAreas ) )
		{
			return PedestrianPathValidityResult::NullArea;
		}
		return PedestrianPathValidityResult::Success;
	}

	return PedestrianPathValidityResult::InvalidForUse;
}

// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::AddActiveNullArea( const world::TrafficAreaShape& nullAreaShape )
{
	{
		RED_SCOPE_LOCK( m_nullAreaLock );
		auto activeIt = std::find( m_activeNullAreas.Begin(), m_activeNullAreas.End(), nullAreaShape.m_id );

		RED_ASSERT( activeIt == m_activeNullAreas.End() );
		m_activeNullAreas.PushBack( nullAreaShape );
	}
	OnNullAreaActivated( nullAreaShape.m_id );
}

// --------------------------------------------------------------------------------
void RuntimeSystemTraffic::RemoveActiveNullArea( Uint64 nullAreaId )
{
	{
		RED_SCOPE_LOCK( m_nullAreaLock );
		auto activeIt = std::find( m_activeNullAreas.Begin(), m_activeNullAreas.End(), nullAreaId );

		RED_ASSERT( activeIt != m_activeNullAreas.End() );
		m_activeNullAreas.RemoveReorder( activeIt );
	}
	OnNullAreaDeactivated( nullAreaId );
}

// --------------------------------------------------------------------------------

Bool RuntimeSystemTraffic::EnsureLaneSlotCapacity( const world::TrafficLaneUID& laneID, world::TrafficMovementBehavior type ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );

	const Uint32 slotsCount = GetSlotsCountOnLane_NoLock( laneID ) + GetSlotsCountOnLane_NoLock( laneID.GetReverse() );
	const Uint32 preferredSlotCount = GetPreferredSlotsCountOnLane( laneID, type );
	return slotsCount < preferredSlotCount;
}

//================================================================================

TrafficSpawning::SpawnDimensions RuntimeSystemTraffic::GetExpectedSlotDimensions( game::data::RecordID characterRecordId ) const
{
	if ( auto vehicleRecord = game::data::GetRecord< game::data::Vehicle_Record >( characterRecordId ).ToHandle() )
	{
		TrafficSpawning::SpawnDimensions toRet;

		toRet.width = 3.1f;
		toRet.length = 9.2f;
		toRet.height = 3.5f;

		if ( auto destructionRecord = vehicleRecord->destructionGet().ToHandle() )
		{
			toRet.width = math::Max( destructionRecord->vehicleDimensions().X, destructionRecord->gridDimensions().X );
			toRet.length = math::Max( destructionRecord->vehicleDimensions().Y, destructionRecord->gridDimensions().Y );
			toRet.height = math::ClampFromBelow( math::Max( destructionRecord->vehicleDimensions().Z, destructionRecord->gridDimensions().Z ), 2.0f );
		}

		return toRet;
	}
	else
	{
		//its a pedestrian 
		return { 0.3f, 0.3f, 2.0f };
	}
}

//================================================================================

TrafficSpawning::SpawnQueryResult RuntimeSystemTraffic::RequestSpawn( const TrafficSpawning::SpawnQuery& spawnQuery ) const
{
	PC_SCOPE( RuntimeSystemTraffic_RequestSpawn );

	if ( !IsLaneValidForUse( spawnQuery.m_laneId ) ) return {};

	TrafficSpawning::SpawnQueryResult output;

	red::DynArray< TrafficSpawning::SpawnSupport::SegmentValidity > segments = { red::PoolFrame() };
	constexpr auto spawnQueryAddedTolerance = 1.25f;

	auto expectedLength = Vector2( spawnQuery.m_dimensions.width, spawnQuery.m_dimensions.length ).Mag() * spawnQueryAddedTolerance;

	const TrafficLanePersistent* persistentData = nullptr;

	VisitLane( spawnQuery.m_laneId, [&]( auto& trafficLane )
	{
		persistentData = &trafficLane.GetPersistentData();
	} );

	if ( !persistentData ) return {};

	auto ptr = m_spawnQueryCache.Access( spawnQuery.m_laneId, *this );
	if ( !ptr ) return {};

	auto& subSegmentedPath = ptr->m_subSegmentedPath;
	auto& subSegmentLookup = ptr->m_lookup;

	thread_local static Vector3 polygonHeight = Vector3::EZ() * 2.0f;

	auto polyBox = persistentData->ComputePolygonBox();
	polyBox.AddPoint( polyBox.Max + polygonHeight );

	auto noLimits = math::EqualsZero( spawnQuery.m_limit.first ) && ( ( spawnQuery.m_limit.second == std::numeric_limits<Float>::infinity() ) || math::AlmostEquals( persistentData->m_length, spawnQuery.m_limit.second ) );

	segments.Resize( subSegmentedPath.ExpectedSegmentCount(), noLimits ? TrafficSpawning::SpawnSupport::SegmentValidity::Available : TrafficSpawning::SpawnSupport::SegmentValidity::Occupied ); //we mark all as inaccessible, we allow only the range between the limits

	if ( !noLimits )
	{
		auto validStartLen = math::ClampFromBelow( spawnQuery.m_limit.first + ( spawnQuery.GetLength() * 0.5f ), 0.0f );
		auto validEndLen = spawnQuery.m_limit.second - ( spawnQuery.GetLength() * 0.5f );

		if ( math::AlmostEqualsOrGreater( validStartLen, validEndLen ) ) return {};

		for ( auto i = 0u; i < segments.Size(); ++i )
		{
			auto len = subSegmentedPath.SegmentLength( i );

			validStartLen -= len;
			validEndLen -= len;

			if ( math::AlmostEqualsOrSmaller( validStartLen, 0.0f ) )
			{
				segments[i] = TrafficSpawning::SpawnSupport::SegmentValidity::Available;
			}

			if ( math::AlmostEqualsOrSmaller( validEndLen, 0.0f ) )
			{
				break;
			}
		}

	}

	auto markSubsegment = [&]( const Sphere& sphere )
	{
		auto center = sphere.GetCenter();
		auto radius = sphere.GetRadius();
		auto box = Box( center, radius );

		subSegmentLookup.Query( box, [&]( auto& i )
		{
			if ( segments[i] == TrafficSpawning::SpawnSupport::SegmentValidity::Available )
			{
				if ( MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D( subSegmentedPath.GetPolygonTemp<Vector4>( i ), center.AsVector2(), radius ) )
				{
					segments[i] = TrafficSpawning::SpawnSupport::SegmentValidity::Occupied;

					if ( i < ( segments.Size() - 1 ) ) segments[i + 1] = TrafficSpawning::SpawnSupport::SegmentValidity::Occupied;
					if ( i > 0u ) segments[i - 1] = TrafficSpawning::SpawnSupport::SegmentValidity::Occupied;
				}
			}

			return true;
		} );
	};

	VisitActiveNullAreaPolygons( [&]( auto& polyline )
	{
		auto queryBox = Box::EMPTY();

		for ( auto& pt : polyline )
		{
			queryBox.AddPoint( pt );
		}

		queryBox.AddPoint( queryBox.Max + polygonHeight );

		if ( !queryBox.Touches( polyBox ) ) return; //they do not touch

		subSegmentLookup.Query( queryBox, [&]( auto& i )
		{
			if ( segments[i] == TrafficSpawning::SpawnSupport::SegmentValidity::Available )
			{
				auto poly = subSegmentedPath.GetPolygonTemp<Vector3>( i );

				for ( auto& pt : poly )
				{
					if ( MathUtils::GeometryUtils::IsPointInPolygon2D( polyline, pt ) )
					{
						segments[i] = TrafficSpawning::SpawnSupport::SegmentValidity::Occupied;
						return true; //no need to continue
					}
				}

				if ( MathUtils::GeometryUtils::IsPolygonsIntersecting2D( polyline, poly ) )
				{
					segments[i] = TrafficSpawning::SpawnSupport::SegmentValidity::Occupied;
				}
			}

			return true;
		} );
	} );


	{
		m_dynamicCollisionsMap.Query( polyBox, [&]( auto& stream )
		{
			stream.Query( polyBox, [&]( auto& part )
			{
				markSubsegment( part.ToSphere() );
				return true;
			} );

			return true;
		} );

		m_disposalMap.Query( polyBox, [&]( auto& stream )
		{
			stream.Query( polyBox, [&]( auto& part )
			{
				markSubsegment( part );
				return true;
			} );

			return true;
		} );


		m_expectedCollisionsMap.Query( polyBox, [&]( auto& stream )
		{
			stream.Query( polyBox, [&]( auto& part )
			{
				markSubsegment( part.GetSphere() );
				return true;
			} );

			return true;
		} );

		GetStaticCollisions().DeepQuery( polyBox, [&]( auto& collision )
		{
			markSubsegment( collision );
			return true;
		} );
	}

	
	//TODO - add lane width to check for match

	red::DynArray< TrafficSpawning::SpawnSupport::FreeSegment > freeSegments = { red::PoolFrame() };
	freeSegments.Reserve( static_cast<Uint32>( subSegmentedPath.Size() * 0.5f ) );

	for ( auto i = 0u; i < subSegmentedPath.Size() - 1; ++i )
	{
		//True ~ Occupied segment

		if ( segments[i] == TrafficSpawning::SpawnSupport::SegmentValidity::Available )
		{
			if ( freeSegments.Empty() )
			{
				freeSegments.PushBack( { i, i, subSegmentedPath.SegmentLength( i ) } );
			}
			else
			{
				auto& last = freeSegments.Back();
				if ( ( last.endIndex + 1 ) == i )
				{
					last.endIndex = i;
					last.length += subSegmentedPath.SegmentLength( i );
				}
				else
				{
					freeSegments.PushBack( { i, i, subSegmentedPath.SegmentLength( i ) } );
				}
			}
		}
	}

	red::alg::EraseIf( freeSegments, [&]( auto& segment )
	{
		return segment.length < expectedLength;
	} );

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	auto debugResult = TrafficSpawning::SpawnSupport::SpawnRequestDebug();
	debugResult.m_laneId = spawnQuery.m_laneId;
	debugResult.m_length = expectedLength;
	debugResult.m_time = GetTrafficTime();
	debugResult.m_segments = segments;
	debugResult.m_freeSegments = freeSegments;
	debugResult.m_segmentedPath = ptr;
	debugResult.m_entityID = spawnQuery.m_expectedEntityID;
	debugResult.m_query = spawnQuery;

	auto addSpawnDebug = [&]()
	{
		RED_SCOPE_LOCK( m_spawnRqDebugLock );
		m_spawnResultLUT[debugResult.m_id] = m_spawnResultDebug.Size();
		m_spawnResultDebug.PushBack( debugResult );
	};

#endif

	if ( !freeSegments.Empty() )
	{
		auto chosenSegment = math::DefaultRandom().Get< Uint32 >( 0u, freeSegments.Size() );

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
		debugResult.m_chosenFreeSegment = chosenSegment;
#endif
		auto lengthHalf = expectedLength * 0.5f;

		auto startIndex = freeSegments[chosenSegment].startIndex;
		auto endIndex = freeSegments[chosenSegment].endIndex;

		auto lenToConsider = lengthHalf;

		while ( lenToConsider > 0.0f )
		{
			lenToConsider -= subSegmentedPath.SegmentLength( startIndex );
			++startIndex;
		}

		lenToConsider = lengthHalf;
		while ( lenToConsider > 0.0f )
		{
			lenToConsider -= subSegmentedPath.SegmentLength( endIndex );
			if ( endIndex == 0u )
			{
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
				addSpawnDebug();
#endif
				return {};
			}

			--endIndex;
		}

		if ( startIndex > segments.Size() )	return {};

		auto chosenSubSegment = std::numeric_limits<Uint32>::max();

		if ( startIndex == endIndex )
		{
			chosenSubSegment = startIndex;
		}
		else if ( startIndex > endIndex )
		{
#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
			addSpawnDebug();
#endif
			return {};
		}
		else
		{
			chosenSubSegment = math::DefaultRandom().Get< Uint32 >( startIndex, endIndex + 1 );
		}

		RED_ASSERT( chosenSubSegment != std::numeric_limits<Uint32>::max() );

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
		debugResult.m_chosenSubSegment = chosenSubSegment;

		{
			auto sphere = Sphere( subSegmentedPath.GetSegmentCenter( chosenSubSegment ), lengthHalf );
			for ( auto i = freeSegments[chosenSegment].startIndex; i <= freeSegments[chosenSegment].endIndex; ++i )
			{
				auto edge = subSegmentedPath.GetSegmentEdge( i );
				if ( sphere.Contains( edge.first ) || sphere.Contains( edge.second ) )
				{
					debugResult.m_spawnGapStart = math::Min( debugResult.m_spawnGapStart, i );
					debugResult.m_spawnGapEnd = i;
				}
			}
		}

#endif

			//finalization
		output.m_worldPosition = subSegmentedPath.GetSegmentCenter( chosenSubSegment );
		output.m_progressionInLane = subSegmentedPath.SegmentLength( chosenSubSegment ) * 0.5f;

		for ( auto i = 0u; i < chosenSubSegment; ++i )
		{
			output.m_progressionInLane += subSegmentedPath.SegmentLength( i );
		}


		output.m_laneId = spawnQuery.m_laneId;
	}

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
	debugResult.m_result = output;

	addSpawnDebug();
#endif
	return output;
}


//================================================================================

TrafficSpawning::FreeSpawnQueryResult RuntimeSystemTraffic::RequestSpawn( const TrafficSpawning::FreeStandingSpawnQuery& spawnQuery ) const
{
	PC_SCOPE( RuntimeSystemTraffic_RequestSpawn );

	TrafficSpawning::FreeSpawnQueryResult output;

	if ( CheckActiveNullAreasForSpawn( spawnQuery ) )
	{
		return {};
	}

	constexpr auto spawnQueryAddedTolerance = 1.25f;

	auto expectedLength = Vector2( spawnQuery.m_dimensions.width, spawnQuery.m_dimensions.length ).Mag() * spawnQueryAddedTolerance;
	
	auto queryBox = Box::EMPTY();

	queryBox.AddPoint( spawnQuery.m_position );
	
	auto transformL2W = WorldTransform( spawnQuery.m_position, Quaternion::IDENTITY().BuildFromDirectionVector( spawnQuery.m_direction ) );
	auto transformW2L = transformL2W.GetInverse();

	{
		auto y = spawnQuery.m_dimensions.length * 0.5f;
		auto x = spawnQuery.m_dimensions.width * 0.5f;
		auto z = spawnQuery.m_dimensions.height * 0.5f;

		queryBox.AddPoint( transformL2W.TransformPoint( Vector3( x, y, z) ).AsVector3() );
		queryBox.AddPoint( transformL2W.TransformPoint( Vector3( -x, -y, -z ) ).AsVector3() );
	}

	auto edgeY = Vector3::EY() * spawnQuery.m_dimensions.length * 0.5f;
	auto edgeX = Vector3::EX() * spawnQuery.m_dimensions.width * 0.5f;

	auto startPt = Vector3::ZEROS() - edgeX - edgeY;
	
	red::FixedArray< Vector3, 4 > carPoly = { startPt, startPt + ( edgeX * 2.0f ), startPt + ( edgeX * 2.0f )+ ( edgeY * 2.0f ), startPt + ( edgeY * 2.0f ) };

	auto overlapDetected = false;

	{
		if ( !overlapDetected )
		{
			m_dynamicCollisionsMap.DeepQuery( queryBox, [&]( auto& sphere )
			{
				overlapDetected = MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D<Vector3>( carPoly.TypedData(), 4u, transformW2L.TransformPoint( sphere.GetCenter() ).AsVector3(), sphere.GetRadius() );
				return !overlapDetected;
			} );
		}

		if ( !overlapDetected )
		{
			m_disposalMap.DeepQuery( queryBox, [&]( auto& sphere )
			{
				overlapDetected = MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D<Vector3>( carPoly.TypedData(), 4u, transformW2L.TransformPoint( sphere.GetCenter() ).AsVector3(), sphere.GetRadius() );
				return !overlapDetected;
			} );
		}
		if ( !overlapDetected )
		{
			m_expectedCollisionsMap.DeepQuery( queryBox, [&]( auto& sphere )
			{
				overlapDetected = MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D<Vector3>( carPoly.TypedData(), 4u, transformW2L.TransformPoint( sphere.GetPosition() ).AsVector3(), sphere.GetRadius() );
				return !overlapDetected;
			} );

		}
		
		GetStaticCollisions().DeepQuery( queryBox, [&]( auto& sphere )
		{
			overlapDetected = MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D<Vector3>( carPoly.TypedData(), 4u, transformW2L.TransformPoint( sphere.GetCenter() ).AsVector3(), sphere.GetRadius() );
			return !overlapDetected;
		} );
	}

	if ( !overlapDetected )
	{
		output.m_worldPosition = spawnQuery.m_position;
	}

	return output;
}

//================================================================================
Bool RuntimeSystemTraffic::CheckActiveNullAreasForSpawn( const TrafficSpawning::FreeStandingSpawnQuery& spawnQuery )const
{
	PathUtils::OrientedBox box = { spawnQuery.m_position, spawnQuery.m_direction, spawnQuery.m_dimensions.width, spawnQuery.m_dimensions.length, spawnQuery.m_dimensions.height };

	RED_SCOPE_SHARED_LOCK( m_lock );

	for ( auto& area : m_activeNullAreas )
	{
		for ( auto& convex : area.m_areaShape )
		{
			if ( PathUtils::CheckOverlap( convex->GetOutline(), 2.0f, box ) )
			{
				return false;
			}
		}
		
	}
	return false;
}

//================================================================================
Bool RuntimeSystemTraffic::QueryLane( const TrafficLaneUID& laneId, const red::DynArray< Sphere >& what, const QuerySettings& settings )
{
	PC_SCOPE( RuntimeSystemTraffic_CheckTouchesLane );
	auto pLane = FindPersistentLane( laneId );
	const red::DynArray< Vector2 >& polygon = pLane->GetPolygon();

	red::DynArray< Vector2 > outline{ job::PoolJobScope() };
	outline.Reserve( pLane->GetOutline().Size() );

	for ( auto& pos : pLane->GetOutline() )
	{
		outline.PushBack( pos.AsVector2() );
	}

	return QueryLane( polygon, outline, what, settings );
}

//================================================================================
Bool RuntimeSystemTraffic::HasCollisionOnPath( const TrafficSlotPtr& slot, Float length, Bool checkStaticCollision, Bool checkDynamicCollisions )
{
	auto polyBox = Box();
	polyBox.Clear();
		
	auto currentDistance = slot->GetLocalPathProgression().GetOffsetOnPath();

	//TODO - make this more efficient in respect to cropping the polygon and the path

	auto subSegmentedPath = PathPolygons::SubSegmentPath<Vector3, nav::PoolTrafficLanes >( 1.0f, slot->localPath.GetPoints(), slot->localPath.GetPolygon(), length + currentDistance );
	red::BVH< Box, Uint32 > lookup = { red::PoolFrame() };

	auto skipDistance = currentDistance;
	auto coverDistance = length;

	for ( auto i = 0u; i < subSegmentedPath.ExpectedSegmentCount(); ++i )
	{
		auto segmentDist = subSegmentedPath.SegmentLength( i );
		skipDistance -= segmentDist;
		if ( skipDistance > 0.0f ) continue;

		if ( coverDistance < 0.0f ) break;
			
		lookup.Insert( subSegmentedPath.GetSegmentBox( i ), i );
		polyBox.AddBox( subSegmentedPath.GetSegmentBox(i) );

		coverDistance -= segmentDist;
	}

	auto checkSubSegment = [&]( Sphere sphere )
	{
		auto center = sphere.GetCenter();
		auto radius = sphere.GetRadius();
		auto box = Box( center, radius );

		auto found = false;

		lookup.Query( box, [&]( auto& i )
		{
			if ( MathUtils::GeometryUtils::TestIntersectionPolygonCircle2D( subSegmentedPath.GetPolygonTemp<Vector4>( i ), center.AsVector2(), radius ) )
			{
				found = true;
				return false; //no need to search more
			}
			else
			{
				return true;
			}
		} );

		return found;
	};

	auto found = false;

	{

		RED_SCOPE_SHARED_LOCK( m_lock );

		if ( !found && checkDynamicCollisions )
		{
			m_dynamicCollisionsMap.Query( polyBox, [&]( auto& stream )
			{
				if ( stream.GetUserData() == slot->GetId() ) return true;
				
				stream.Query( polyBox, [&]( auto& part )
				{
					found = checkSubSegment( part.ToSphere() );
					return !found;
				} );
				
				return !found;
			} );

			m_disposalMap.Query( polyBox, [&]( auto& stream )
			{
				if ( stream.m_id == slot->GetId() ) return true;

				stream.Query( polyBox, [&]( auto& part )
				{
					found = checkSubSegment( part );
					return !found;
				} );

				return !found;
			} );
		}
	}

	return found;
}

//================================================================================
void RuntimeSystemTraffic::AddLaneUsage( const TrafficLaneUID& id )
{
	PC_SCOPE_FUNC();

	{
		RED_SCOPE_SHARED_LOCK( m_lock );
		auto it = m_nodeUsage.Find( GlobalNodeID{ id.m_nodeRefHash } );
		if ( it != m_nodeUsage.End() )
		{
			++( it.Value() );
			return;
		}
	}

	{
		RED_SCOPE_LOCK( m_lock );
		auto it = m_nodeUsage.Insert( GlobalNodeID{ id.m_nodeRefHash }, 0u ).Iterator();
		RED_ASSERT( it != m_nodeUsage.End() );
		++( it.Value() );
	}
}

//================================================================================
void RuntimeSystemTraffic::DropLaneUsage( const TrafficLaneUID& id )
{
	PC_SCOPE_FUNC();

	RED_SCOPE_SHARED_LOCK( m_lock );
	auto it = m_nodeUsage.Find( GlobalNodeID{ id.m_nodeRefHash } );
	if ( it != m_nodeUsage.End() )
	{
		if ( it.Value() != 0u )
		{
			--( it.Value() );
		}
		else
		{
			RED_LOG_WARNING( "This should not happen" );
		}

		if ( it.Value() == 0u )
		{
			m_checkNodeDump = true;
		}
	}
}

//================================================================================
red::DynArray< Uint64 > RuntimeSystemTraffic::GetCurrentActiveNullAreas( const red::memory::Pool& pool ) const
{
	red::DynArray< Uint64 > ret{ pool };
	ret.Reserve( m_nullAreaMap.Size() );
	for ( auto it : m_nullAreaMap )
	{
		ret.PushBack( it.Key() );
	}
	return ret;
}

//================================================================================
Bool RuntimeSystemTraffic::IsLaneValidForUse( const TrafficLaneUID& id ) const
{
	RED_SCOPE_SHARED_LOCK( m_lock );
	if ( m_nodeDump.Exist( GlobalNodeID{ id.m_nodeRefHash } ) ) return false;
	if ( !m_nodeToLaneLUT.KeyExist( GlobalNodeID{ id.m_nodeRefHash } ) ) return false;

	return true;
}

}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace world
{
	// --------------------------------------------------------------------------------
	RuntimeSystemTraffic::GlobalPathRequest::~GlobalPathRequest() = default;

	// --------------------------------------------------------------------------------
	RuntimeSystemTraffic::GlobalPathRequest::GlobalPathRequest( const TrafficGlobalPathToken& t, const TrafficGlobalPathQuery & q ) : token( t )
		, query( q )
	{}

	// --------------------------------------------------------------------------------
	Bool RuntimeSystemTraffic::GlobalPathRequest::IsValid() const
	{
		return token != s_invalidTrafficGlobalPathToken;
	}
	
	// --------------------------------------------------------------------------------
	void RuntimeSystemTraffic::GlobalPathRequest::Reset()
	{
		token = s_invalidTrafficGlobalPathToken;
	}

	// --------------------------------------------------------------------------------

	RuntimeSystemTraffic::GlobalPathRequest::GlobalPathRequest() = default;

	// --------------------------------------------------------------------------------

	Float RuntimeSystemTraffic::GenerateGlobalPathContext::s_minimumCarPathLength = 100.0f;

	Float RuntimeSystemTraffic::GenerateGlobalPathContext::s_minimumPedestrianPathLength = 15.0f;
	Float RuntimeSystemTraffic::GenerateGlobalPathContext::s_maximumPedestrianPathLength = 50.0f;

	// --------------------------------------------------------------------------------
	RuntimeSystemTraffic::GenerateGlobalPathContext::GenerateGlobalPathContext( const TrafficLaneUID& startLaneId )
		: m_startLaneId( startLaneId )
	{}

	RuntimeSystemTraffic::GenerateGlobalPathContext::GenerateGlobalPathContext( const TrafficLaneUID& startLaneId, const TrafficOutLaneFilter& outLaneFilter, const TrafficPreferredOutLaneFilter& prefferedLaneFilter, TrafficMovementBehavior behavior, Float laneInitialEntry )
		: m_startLaneId( startLaneId )
		, m_outLaneFilter( outLaneFilter )
		, m_preferedOutlanesFilter( prefferedLaneFilter )
		, m_behavior( behavior )
		, m_initialEntryPosition( laneInitialEntry )
	{
		if ( behavior == TrafficMovementBehavior::Car )
		{
			m_minimumLength = s_minimumCarPathLength;
		}
	}

	RuntimeSystemTraffic::GenerateGlobalPathContext::GenerateGlobalPathContext( const TrafficLaneUID& startLaneId, TrafficMovementBehavior behavior, Float laneInitialEntry )
		: m_startLaneId( startLaneId )
		, m_behavior( behavior )
		, m_initialEntryPosition( laneInitialEntry )
	{
		if ( behavior == TrafficMovementBehavior::Car )
		{
			m_minimumLength = s_minimumCarPathLength;
		}
	}

	// --------------------------------------------------------------------------------
	RuntimeSystemTraffic::GenerateGlobalPathContext::GenerateGlobalPathContext() = default;
	RuntimeSystemTraffic::GenerateGlobalPathContext::~GenerateGlobalPathContext() = default;

	RuntimeSystemTraffic::VerticalLimits::VerticalLimits() = default;
	RuntimeSystemTraffic::VerticalLimits::~VerticalLimits() = default;
	RuntimeSystemTraffic::VerticalLimits::VerticalLimits( Float upwards, Float downwards )
		: up( upwards )
		, down( downwards )
	{}


} // namespace world

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#if RED_CTSW( RED_DEBUG_DRAW_AI_IMGUI )
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace world
{

void RuntimeSystemTraffic::DumpPersistentData( const String& dataName, const Vector3& pos, Float radius
												   , std::function < bool( const TrafficLanePersistent& data ) > filter
												   , std::function < std::pair< String, String >( const TrafficLanePersistent& data ) > getColor ) const
{

	struct Connector
	{
		Bool in;
		String id;
		Float position;
		Bool toDeadend;
		String data;
	};

	const red::AbsolutePath configPath = GFileManager->GetEngineRoot().AddFilePath( "../temp/" + dataName );
	GFileManager->DeleteFile( configPath );
	auto file = GFileManager->CreateFileWriter( configPath, FOF_AbsolutePath );

	red::AppendStringToFile( *file, "digraph G {" );
	red::AppendStringToFile( *file, "node[shape = record];ranksep=.75;overlap=prism;sep=\"+20.0,+30.0\";" );

	red::DynArray< std::pair< TrafficLaneIndex, red::DynArray<Connector> > > clustering = { red::PoolDebug() };

	red::HashSet< TrafficLaneIndex > baseLanes = { red::PoolDebug() };
	red::HashSet< TrafficLaneIndex > extensionLanes = { red::PoolDebug() };

	red::DynArray< const TrafficLanePersistent* > lanes = { red::PoolDebug() };

	m_persistentData->VisitInRectangleRough( pos, radius * 2.0f, [&](auto& lane)
	{
		if ( !filter( lane ) ) return true;

		lanes.PushBack( &lane );
		baseLanes.Insert( m_persistentData->GetLaneIndex( lane ) );
		return true;
	} );

	for ( auto& pLane : lanes )
	{
		auto& lane = *pLane;
		for ( auto& lane : lane.m_outLanes )
		{
			if ( baseLanes.Exist( lane.laneIndex ) ) continue;
			
			extensionLanes.Insert( lane.laneIndex );
		}

		for ( auto& lane : lane.m_inLanes )
		{
			if ( baseLanes.Exist( lane.laneIndex ) ) continue;

			extensionLanes.Insert( lane.laneIndex );
		}
	}

	for ( auto extension : extensionLanes )
	{
		lanes.PushBack( &m_persistentData->GetLane( extension ));
	}

	//displayed lanes
	for ( auto& pLane : lanes )
	{
		auto& lane = *pLane;
		const TrafficLaneIndex laneIndex = m_persistentData->GetLaneIndex( lane );

		auto nodeName = lane.GetLaneId().ToDebugString("_");
		auto nodeLabel = lane.GetLaneId().ToDebugStrings();
		
		clustering.PushBack( { laneIndex, { red::PoolDebug() } } );
		auto& connections = clustering.Back().second;

		for( auto& outLane : lane.m_outLanes )
		{
			if ( baseLanes.Exist( outLane.laneIndex ) || extensionLanes.Exist( outLane.laneIndex ) )
			{
				connections.PushBack( { false, m_persistentData->GetLaneUID( outLane.laneIndex ).ToDebugString( "_" ), outLane.thisLaneExitPosition, outLane.leadsToDeadEnd } );
			}
		}

		for ( auto& inLane : lane.m_inLanes )
		{
			if ( baseLanes.Exist( inLane.laneIndex ) || extensionLanes.Exist( inLane.laneIndex) )
			{
				auto& outLanes = m_persistentData->GetLane( inLane.laneIndex ).m_outLanes;

				auto it = std::find_if( outLanes.Begin(), outLanes.End(), [&]( auto& value )
				{
					return value.laneIndex == laneIndex;
				} );
				if ( it != outLanes.End() )
				{
					connections.PushBack( { true, m_persistentData->GetLaneUID( inLane.laneIndex ).ToDebugString( "_" ), ( *it ).nextLaneEntryPosition, false } );
				}
			}
		}

		std::sort( connections.Begin(), connections.End(), []( auto& a, auto& b )
		{
			return a.position < b.position;
		} );

		for (auto& con : connections )
		{
			auto data = String();

			if ( con.in )
			{
				con.data = String::Printf( "in_%s", con.id.AsChar() );
			}
			else //out
			{
				con.data = String::Printf( "out_%s", con.id.AsChar() );
			}
		}
	}

	for ( auto& lane : clustering )
	{
		const TrafficLaneIndex index = lane.first;
		const TrafficLaneUID id = m_persistentData->GetLaneUID( index );
		
		auto nodeLabels = id.ToDebugStrings();
		auto nodeName = id.ToDebugString( "_" );

		auto colors = getColor( *m_persistentData->FindLane( id ) );

		auto bgColor = colors.first;
		auto fontColor = colors.second;
	
		red::AppendStringToFile( *file, String::Printf( "struct_%s [shape=record,width=0.0,margin=0.02,height=0.1,style=filled,fontcolor=%s,fillcolor=%s ", nodeName.AsChar(),fontColor.AsChar(), bgColor.AsChar() ) );
		
		auto data = String::Printf( "label = \"%s_%s_%s_%s", nodeLabels[0].RightString( 4 ).AsChar(), nodeLabels[1].AsChar(), nodeLabels[2].AsChar(), nodeLabels[3].AsChar() ); //label = "XXX|
		for ( auto& con : lane.second )
		{
			data += String::Printf( "|<%s>", con.data.AsChar() );
		}

		data += "|\"];\n";

		red::AppendStringToFile( *file, data.AsChar());
		// struct_XXX [shape=record,width=0.0,margin=1.0 label="XXX|<1>|<2>..."];
	}


	//connections

	for ( auto& lane : lanes )
	{
		auto source = lane->GetLaneId().ToDebugString( "_" );

		for ( auto& outLane : lane->m_outLanes )
		{
			if ( baseLanes.Exist( outLane.laneIndex ) || extensionLanes.Exist( outLane.laneIndex ) )
			{
				auto destination = m_persistentData->GetLaneUID( outLane.laneIndex ).ToDebugString( "_" );

				auto from = String::Printf( "struct_%s:out_%s", source.AsChar(), destination.AsChar() );
				auto to = String::Printf( "struct_%s:in_%s", destination.AsChar(), source.AsChar() );

				auto lineSetup = String();

				if ( outLane.leadsToDeadEnd )
				{
					lineSetup = "[style=dashed, color=red, penwidth=2]";
				}
				else
				{
					lineSetup = "[style=solid color=black, penwidth=2]";
				}

				auto data = String::Printf( "%s -> %s %s;\n", from.AsChar(), to.AsChar(), lineSetup.AsChar() );
				red::AppendStringToFile( *file, data );
			}
		}
	}

	//struct_1:1->struct2:2;
	

	red::AppendStringToFile( *file, "}" );
}


}
#endif

#ifdef DEBUG_TRAFFIC
	#pragma optimize("", on)
#endif
