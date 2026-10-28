/*
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/
//owner: tetyana.meleshchenko, tomas.plch

#include "build.h"
#include <cstdio>
#include "runtimeSystemSmartObjects.h"

#include "smartObjectsAnimationData.h"
#include "smartObjectInitializationContext.h"
#include "smartObject.h"
#include "compiledSmartObjectsResource.h"
#include "worldSmartObjectNode.h"

#include "../../gameCore/include/smartObjectManagerSystemInterface.h"
#include "../../gameCore/include/gameEngineExtensions.h"
#include "../../game/include/gameTimeSystem.h"
#include "../../world/include/gameInstance.h"
#include "../../world/include/runtimeSystemDebugRenderingUtils.h"
#include "../../world/include/worldRuntimeScene.h"
#include "../../world/include/worldPrefab.h"
#include "../../world/include/worldNode.h"
#include "../../world/include/cameraSystemInterface.h"
#include "../../worldPhysics/include/runtimeSystemPhysics.h"
#include "../../worldNavigation/include/runtimeSystemNavigation.h"
#include "../../worldEntities/include/runtimeSystemEntityAnimation.h"
#include "../../worldEntities/include/animatedComponent.h"
#include "../../animation/include/animWrapper.h"
#include "../../redReflection/include/resourceAsyncReference.h"

#include "../../redSystem/include/stopWatch.h"
#include "../../engine/include/tickInfo.h"
#include "../../redJobs2/include/jobRunner.h"

#include "../../worldStreaming/include/runtimeSystemWorldStreaming.h"
#include "../../worldStreaming/include/worldStreamingWorld.h"

#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
	#include "worldSmartObjectNodeInstance.h"
	#include "../../imgui/include/redImGui.h"
	#include "../../world/include/worldGlobalNodeRuntimeResolver.h"
	#include "../../renderDraw/include/fragmentBuilder.h"
	#include "../redMath/include/random.h"
#endif

RTTI_BEGIN_TYPE_IN_NAMESPACE( RuntimeSystemSmartObjects, world );
	RTTI_PARENT_TYPE( world::IRuntimeSystem );
RTTI_END_TYPE();

static rend::DebugFilter DF_SmartObjectsFilterPerAnimset( "AI/SmartObjects/FilterPerAnimset" );
static rend::DebugFilter DF_ShowSpawnBoxes( "AI/SmartObjects/ShowSpawnBoxes" );

static rend::DebugFilter DF_ShowSmartObjectResolutionState( "AI/SmartObjects/ShowSmartObjectResolutionState" );
static rend::DebugFilter DF_ShowResolveBubbles( "AI/SmartObjects/ShowSmartObjectResolutionBubbles" );

//#pragma optimize("", off)

namespace world
{
	RuntimeSystemSmartObjects::RuntimeSystemSmartObjects()
		: m_nextId( s_invalidObjectId )
	{
	}

	//////////////////////////////////////////////////////////////////////////
	RuntimeSystemSmartObjects::~RuntimeSystemSmartObjects() = default;

	//////////////////////////////////////////////////////////////////////////
	SystemID RuntimeSystemSmartObjects::GetSystemID() const
	{
		return SYSTEM_ID;
	}

	//////////////////////////////////////////////////////////////////////////
#if !defined( RED_CONFIGURATION_FINAL ) && defined( RED_USE_RESOURCEPATH_STRINGS )
	res::ResourcePath ComputeCompiledSmartObjectRootPathFromWorldPath( const res::ResourcePath& worldPath )
	{
		// expected input:	...\<world_directory>\<world_name>.world
		// output:			...\<world_directory>\_generated\smartobjects.prefab

		const auto extension = red::paths::GetExtension( worldPath.ToStringView() );

		if ( extension == "world" )
		{
			const String worldDirectory = red::paths::ExtractParentPath( worldPath.ToStringView() );
			const String compiledSOPath = worldDirectory + "sectors\\_generated\\" + "all.smartobjects";
			return res::ResourcePath::Build( compiledSOPath );
		}
		
		return res::ResourcePath::EMPTY;
	}
#endif

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnInitialize( RuntimeScene& scene, const RuntimeSystemInitInfo& initInfo, job::Counter& counter )
	{
		m_navigationSystem = scene.GetSystem< RuntimeSystemNavigation >();
		m_physicsSystem = scene.GetSystem< RuntimeSystemPhysics >();

#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		m_nodeInstanceRegistry = scene.GetSystem< world::INodeInstanceRegistry >();
#endif

		m_metrics.Initialize();
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnUninitialize( RuntimeScene& scene )
	{
		m_navigationSystem = nullptr;
		m_physicsSystem = nullptr;

#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		m_nodeInstanceRegistry = nullptr;
#endif

#ifndef NO_EDITOR 
		m_notResolvedQueue.Clear();
#endif

		m_rootResource = nullptr;

		m_lightweightMode = false;

		m_sectorDatabase.Clear();
		m_smartObjectVisuals.Clear();
		m_nodesToIdsMapping.Clear();

		m_deferredInitialization.Clear();
		m_resolvedQueue.Clear();
		m_sectorMembershipLUT.Clear();
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnSetup( RuntimeScene& scene, const RuntimeSceneSetupInfo& setupInfo, job::Counter& counter )
	{
		const res::ResourcePath worldPath = setupInfo.m_runtimeInfo.GetResourcePath();
		m_lightweightMode = worldPath.IsEmpty();

		if ( !worldPath.IsEmpty() )
		{
#ifdef RED_USE_RESOURCEPATH_STRINGS
			LoadRootData( worldPath, counter );
#endif
		}
	}

	void RuntimeSystemSmartObjects::OnStreamingWorldLoaded( world::RuntimeScene& scene, job::Counter& counter )
	{
		auto streamingSystem = scene.GetSystem< world::RuntimeSystemWorldStreaming >();
		RED_FATAL_ASSERT( streamingSystem );

		// CP2077 web: asserts are compiled out in FINAL; a session whose .streamingworld failed to load
		// has a null streaming world here (RuntimeSystemWorldStreaming tolerates that by design).
		if ( !streamingSystem )
		{
			return;
		}

		auto streamingWorld = streamingSystem->GetStreamingWorld();
		RED_FATAL_ASSERT( streamingWorld );
		if ( !streamingWorld )
		{
			std::fprintf( stderr, "CP2077 smart objects guard: no streaming world loaded; skipping root data\n" );
			return;
		}

		Internal_LoadRootData( streamingWorld->m_smartObjectCompiledRootResource.GetPath(), counter );
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnPostWorldDetached( RuntimeScene& scene )
	{
		RED_SCOPE_LOCK( m_lookupLock );
		RED_SCOPE_LOCK( m_smartObjectVisualsLock );
		RED_SCOPE_LOCK( m_nodesToIdsLock );
		RED_SCOPE_LOCK( m_deferredQueueLock );
		RED_SCOPE_LOCK( m_destructionQueueLock );

		if ( m_smartObjectsManager )
		{
			for ( auto soIt : m_nodesToIdsMapping )
			{
				m_smartObjectsManager->UnregisterSmartObject( soIt.Value() );
			}
		}

		m_loadedRootDataPath = res::ResourcePath::EMPTY;
		m_rootResource = nullptr;

		m_lightweightMode = false;

		m_sectorDatabase.Clear();
		m_smartObjectVisuals.Clear();
		m_nodesToIdsMapping.Clear();
		m_smartObjectsLookup.Clear();
		m_resolvingCenters.Clear();
		m_deferredInitialization.Clear();
		m_resolvedQueue.Clear();
		m_destroyedQueue.Clear();
		m_sectorMembershipLUT.Clear();

#ifndef NO_EDITOR 
		m_notResolvedQueue.Clear();
#endif

#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		m_availableAnimsets.Clear();
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnGameInstanceAttached( game::IGameInstance& gameInstance )
	{
		m_smartObjectsManager = gameInstance.GetSystemPtr< AI::ISmartObjectManager >();
		m_cameraSystem = gameInstance.GetSystemPtr< game::ICameraSystem >();
		m_timeSystem = gameInstance.GetSystemPtr< game::TimeSystem >();
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnGameInstanceDetached( game::IGameInstance& gameInstance )
	{
		m_smartObjectsManager = nullptr;
		m_cameraSystem = nullptr;
		m_timeSystem = nullptr;
	}

	//////////////////////////////////////////////////////////////////////////
	THandle< game::SmartObjectsCompiledResource > RuntimeSystemSmartObjects::GetCompiledObjectsData() const
	{
		return m_rootResource;
	}

	//////////////////////////////////////////////////////////////////////////
#ifdef RED_USE_RESOURCEPATH_STRINGS
	void RuntimeSystemSmartObjects::LoadRootData( const res::ResourcePath& worldPath, job::Counter& counter )
	{
		if ( !worldPath.IsValid() )
		{
			return;
		}

		const auto rootCollection = ComputeCompiledSmartObjectRootPathFromWorldPath( worldPath );
		Internal_LoadRootData( rootCollection, counter );
	}
#endif

	void RuntimeSystemSmartObjects::Internal_LoadRootData( const res::ResourcePath& rootDataPath, job::Counter& counter )
	{
		if ( !rootDataPath.IsValid() )
		{
			RED_LOG_ERROR( "Cannot load root data for smart objects" );
			return;
		}

		if ( m_loadedRootDataPath.IsValid() )
		{
			if ( m_loadedRootDataPath == rootDataPath )
			{
				return;
			}
		}

		m_loadedRootDataPath = rootDataPath;
		
		// loads the root database, has no nodes
		m_rootResource = CreateHandle< game::SmartObjectsCompiledResource>();
		m_rootResource->CreateAnimationDatabase();
		m_rootResource->CreateCompiledNodes();
		m_rootResource->CreateDictionaries( {} );

		res::ResourceTokenHandle token = GResourceLoader->IssueLoadingRequest( rootDataPath );
		job::DispatchJob< AI::PoolAI_SORuntimeData >( "LoadRootData", token->GetWaitCounter(), counter, [this, token]( const job::RunContext& context ) 
		{
			THandle< CResource > res = token->GetResource();

			if ( res )
			{
				if ( auto resource = Cast< game::SmartObjectsCompiledResource >( res ) )
				{
					m_rootResource = resource;
					m_rootResource->AfterLoad( {} );
				}
				else
				{
					RED_LOG_ERROR( "Cannot find resource for SmartObjectsCompiledResource '%s' ", m_loadedRootDataPath.ToDebugString() );
				}
			}

			if ( m_rootResource )
			{
				m_rootResource->CleanupData();

#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
				if ( const auto allGates = m_rootResource->GetAnimationDatabase() )
				{
					for ( const auto& animsetEntry : allGates->GetAnimsets() )
					{
						m_availableAnimsets[animsetEntry.m_hash] = std::make_pair( red::paths::ExtractFileStem( animsetEntry.m_resourcePath ), true );
					}
				}
#endif
			}
		} );
	}

	//////////////////////////////////////////////////////////////////////////
	game::SmartObjectDictionaries RuntimeSystemSmartObjects::GetRootDictionary() const
	{
		return m_rootResource->GetSmartObjectDictionary();
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RegisterSectorData( Uint64 id, THandle< game::SmartObjectsCompiledResource > resource )
	{
#ifdef SO_MERGED_MASTER_RESOURCE
		resource->AfterLoad( GetRootDictionary() );
#else
		resource->AfterLoad( {} );
#endif
		
		if ( m_rootResource )
		{
			resource->LinkAnimationDatabase( m_rootResource->GetAnimationDatabase() );
		}

		{
			RED_SCOPE_LOCK( m_lookupLock );
			m_sectorDatabase.Insert( id, resource );
		}

		{
			RED_SCOPE_LOCK( m_deferredQueueLock );

			auto it = m_sectorMembershipLUT.Find( id );
			if ( it != m_sectorMembershipLUT.End() )
			{
				it.Value().Revive();
			}
			else
			{
				m_sectorMembershipLUT.Emplace( id,
											   resource->ExportMembership( AI::PoolAI_SORuntimeData() ),
				[ this, id ]( auto& nodeHash )
				{
					//this is expected to be locked -> m_deferredQueueLock
					auto it = m_deferredInitialization.Find( nodeHash );

					if ( it != m_deferredInitialization.End() )
					{
						auto& item = it.Value();

						if ( !item.m_sectors.Exist( id ) )
						{
							item.m_sectors.PushBack( id );
						}

						if ( !item.InAnyQueue() )
						{
#ifdef RED_ASSERTS_ENABLED
							if ( !InEditor() )
							{
								RED_ASSERT( !IsResolved( it.Key() ) );
							}
#endif

							item.m_inNotResolvedQueue = true;

#ifndef NO_EDITOR
							m_notResolvedQueue.Push( it.Key() );
#endif
						}
					}
				},
				[ this, id ]( auto& nodeHash )
				{
					//this is expected to be locked -> m_deferredQueueLock

					auto it = m_deferredInitialization.Find( nodeHash );

					if ( it != m_deferredInitialization.End() )
					{
						auto& item = it.Value();
						auto index = item.m_sectors.GetIndex( id );

						if ( index != red::INVALID_INDEX )
						{
							item.m_sectors.RemoveAt( index );
						}
					}
				} );

			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::UnregisterSectorData( Uint64 id )
	{
		{
			RED_SCOPE_LOCK( m_deferredQueueLock );

			auto it = m_sectorMembershipLUT.Find( id );
			if ( it != m_sectorMembershipLUT.End() )
			{
				it.Value().Kill();
			}
		}

		{
			RED_SCOPE_LOCK( m_lookupLock );

			auto it = m_sectorDatabase.Find( id );
			if ( it != m_sectorDatabase.End() )
			{
				m_sectorDatabase.Remove( it );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::InvalidateSmartObjects()
	{
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		if ( !m_rootResource || !m_rootResource->GetAnimationDatabase() || !m_navigationSystem || !m_physicsSystem || !m_nodeInstanceRegistry )
		{
			return;
		}

		RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );

		for ( const auto& mapping : m_nodesToIdsMapping )
		{
			const GlobalNodeID globalId( mapping.Key() );

			if ( world::NodeInstancePtr nodeInstancePtr = m_nodeInstanceRegistry->FindNodeInstance( globalId ) )
			{
				if ( const THandle< world::SmartObjectNodeInstance > soInstance = Cast< world::SmartObjectNodeInstance >( nodeInstancePtr ) )
				{
					soInstance->Invalidate();
				}
			}
		}
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::SnapSmartObjects()
	{
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		if ( !m_physicsSystem )
		{
			return;
		}

		RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );

		for ( const auto& mapping : m_nodesToIdsMapping )
		{
			const GlobalNodeID globalId( mapping.Key() );

			if ( world::NodeInstancePtr nodeInstancePtr = m_nodeInstanceRegistry->FindNodeInstance( globalId ) )
			{
				if ( const THandle< world::SmartObjectNodeInstance > soInstance = Cast< world::SmartObjectNodeInstance >( nodeInstancePtr ) )
				{
					soInstance->Snap( *m_physicsSystem );
				}
			}
		}
#endif
	}

	//////////////////////////////////////////////////////////////////////////
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
	const red::HashMap< Uint64, std::pair< String, Bool > >& RuntimeSystemSmartObjects::GetAvailableAnimsets() const
	{
		return m_availableAnimsets;
	}
#endif

	//////////////////////////////////////////////////////////////////////////
	AI::ObjectId RuntimeSystemSmartObjects::CreateSmartObject( const game::SmartObjectDefinition& def, const WorldTransform& initialTransform, const world::NodePathHashType& globalNodePathHash, OnDeferredInitializationFunc onPostInit )
	{
		AI::ObjectId id;

		if ( m_lightweightMode )
		{
			return id;
		}

		auto resourceForSO = EvaluateResource( globalNodePathHash );
		auto soDB = resourceForSO.second;

		if ( InEditor() )
		{
			id = AI::ObjectId( m_nextId.Increment() );

			const game::SmartObjectInitializationContext initializationContext( initialTransform, def, soDB, m_navigationSystem, m_physicsSystem->GetSceneID(), m_metrics );
			const ObjectVisualStubPtr visualStub = def.CreateVisuals( id );
			visualStub->Initialize( initializationContext, globalNodePathHash );

			{
				RED_SCOPE_LOCK( m_smartObjectVisualsLock );
				RED_VERIFY( m_smartObjectVisuals.Insert( id, visualStub ).IsSuccessful() );
			}
		}
		
		if ( id.IsValid() ) 
		{
			RED_SCOPE_LOCK( m_nodesToIdsLock );
			RED_VERIFY( m_nodesToIdsMapping.Insert( globalNodePathHash, id ).IsSuccessful() );
		}

		if ( IsRootResource( soDB ) )
		{
			RED_SCOPE_LOCK( m_deferredQueueLock );
			auto treeNodeId = m_smartObjectsLookup.Insert( Box( initialTransform.GetPosition().AsVector3(), 0.5f ), globalNodePathHash );
			auto& value = m_deferredInitialization.Emplace( globalNodePathHash, onPostInit, initialTransform.GetPosition().AsVector3(), treeNodeId ).Iterator().Value();

			value.m_deferredCall = onPostInit;
			value.m_toBeDestroyed = false;
		}
		else
		{
			RED_SCOPE_LOCK( m_deferredQueueLock );
			Uint32 treeNodeId = m_smartObjectsLookup.Insert( Box( initialTransform.GetPosition().AsVector3(), 0.5f ), globalNodePathHash );
			auto& value = m_deferredInitialization.Emplace( globalNodePathHash, onPostInit, initialTransform.GetPosition().AsVector3(), treeNodeId, resourceForSO.first ).Iterator().Value();
			
			value.m_deferredCall = onPostInit;
			value.m_toBeDestroyed = false;

			if ( !value.InAnyQueue() )
			{
				if ( IsResolved( globalNodePathHash ) )
				{
					value.m_inResolvedQueue = true;
					m_resolvedQueue.Push( globalNodePathHash );
				}
				else
				{
					value.m_inNotResolvedQueue = true;
#ifndef NO_EDITOR 
					m_notResolvedQueue.Push( globalNodePathHash );
#endif
				}
			}
		}

		return id;
	}

	//////////////////////////////////////////////////////////////////////////
	AI::ObjectId RuntimeSystemSmartObjects::ReinitSmartObject( const game::SmartObjectDefinition& def, const WorldTransform& initialTransform, const world::NodePathHashType& globalNodePathHash )
	{
		AI::ObjectId id;

		if ( m_lightweightMode )
			return AI::ObjectId();

		auto soDB = EvaluateResource( globalNodePathHash );

		if ( InEditor() )
		{
			const game::SmartObjectInitializationContext initializationContext( initialTransform, def, soDB.second, m_navigationSystem, m_physicsSystem->GetSceneID(), m_metrics );
			
			{
				RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );
				auto it = m_nodesToIdsMapping.Find( globalNodePathHash );

				if ( it == m_nodesToIdsMapping.End() )
				{
					return AI::ObjectId();
				}

				id = it.Value();
			}

			{
				RED_SCOPE_LOCK( m_smartObjectVisualsLock );
				auto it = m_smartObjectVisuals.Find( id );
				if ( it != m_smartObjectVisuals.End() )
				{
					it.Value()->Initialize( initializationContext, globalNodePathHash );
				}
			}
		}
		else
		{
			if ( IsRootResource( soDB.second ) )
			{
				return AI::ObjectId(); // no sense to continue
			}

			const game::SmartObjectInitializationContext initializationContext( initialTransform, def, soDB.second, m_navigationSystem, m_physicsSystem->GetSceneID(), m_metrics );
			id = m_smartObjectsManager->RegisterSmartObject( initializationContext, globalNodePathHash );

			{
				RED_SCOPE_LOCK( m_nodesToIdsLock );
				RED_VERIFY( m_nodesToIdsMapping.Insert( globalNodePathHash, id ).IsSuccessful() );
			}
		}

		return id;
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::DestroySmartObject( const world::NodePathHashType& globalNodePathHash )
	{
		if ( m_lightweightMode )
			return;

		if ( InEditor() )
		{
			RED_SCOPE_LOCK( m_deferredQueueLock );

			auto it = m_deferredInitialization.Find( globalNodePathHash );
			if ( it != m_deferredInitialization.End() )
			{
				m_smartObjectsLookup.Remove( it.Value().m_treeNodeId );
				m_deferredInitialization.Remove( it );
			}

			AI::ObjectId id;

			{
				RED_SCOPE_LOCK( m_nodesToIdsLock );
				auto it = m_nodesToIdsMapping.Find( globalNodePathHash );
				if ( it == m_nodesToIdsMapping.End() ) // this node was not stored
				{
					return;
				}

				id = it.Value();
				m_nodesToIdsMapping.Remove( it );
			}

			RED_SCOPE_LOCK( m_smartObjectVisualsLock );

			auto iter = m_smartObjectVisuals.Find( id );
			if ( iter != m_smartObjectVisuals.End() )
			{
				ObjectVisualStubPtr visualStub = iter.Value();
				RED_ASSERT( visualStub, "An attempt to delete the smart object visual stub with id '%llu' that was not created", id.GetValue() );

				RED_VERIFY( m_smartObjectVisuals.Remove( iter ).IsSuccessful() );
			}
		}
		else
		{
			// the update will take care of this being cleaned
			RED_SCOPE_LOCK( m_deferredQueueLock );

			auto it = m_deferredInitialization.Find( globalNodePathHash );
			if ( it != m_deferredInitialization.End() )
			{
				if ( !it.Value().m_toBeDestroyed && !it.Value().IsLocked() ) //we cannot schedule an item for destruction if its locked
				{
					RED_SCOPE_LOCK( m_destructionQueueLock );
					m_destroyedQueue.PushBack( globalNodePathHash );
				}

				it.Value().m_deferredCall = nullptr;
				it.Value().m_toBeDestroyed = true;
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::UpdateSmartObject( AI::ObjectId id, const game::SmartObjectDefinition& def, const WorldTransform& currentTransform, const world::NodePathHashType& globalNodePathHash )
	{
		auto soDB = EvaluateResource( globalNodePathHash );

		const game::SmartObjectInitializationContext context( currentTransform, def, soDB.second, m_navigationSystem, m_physicsSystem->GetSceneID(), m_metrics );

		if ( !InEditor() )
		{
			// if it's deferred - it has not been registered with the manager yet
			{
				RED_SCOPE_LOCK( m_deferredQueueLock );
				if ( m_deferredInitialization.Find( globalNodePathHash ) != m_deferredInitialization.End() )
					return;
			}

			m_smartObjectsManager->UpdateSmartObject( id, context );
		}
		else
		{
			RED_SCOPE_SHARED_LOCK( m_smartObjectVisualsLock );

			auto iter = m_smartObjectVisuals.Find( id );
			if ( iter != m_smartObjectVisuals.End() )
			{
				ObjectVisualStubPtr visualStub = iter.Value();
				RED_ASSERT( visualStub, "An attempt to update the smart object visual stub with id '%llu' that was not created", id.GetValue() );
				visualStub->Update( context );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RetrieveMotionData( const game::SmartObjectDefinition& def, const WorldTransform& transform, game::SmartObjectPointType type, red::DynArray< game::SmartObjectPoint >& result ) const
	{
		def.RetrieveMotionData( *m_rootResource->GetAnimationDatabase(), transform, type, result );
	}

	//////////////////////////////////////////////////////////////////////////
	static const game::SmartObjectPoint* GetOverridePoint( const red::HashSet< Uint64 >& overrideHashes, const red::DynArray< game::SmartObjectPoint >& points, CName animation )
	{
		for ( const auto& point : points )
		{
			if ( overrideHashes.Exist( point.GetAnimset() ) && point.GetAnimationName() == animation )
			{
				return &point;
			}
		}

		return nullptr;
	}
	
	//////////////////////////////////////////////////////////////////////////
	static Bool GetAnimSetHash( const ent::Entity& user, CName animationName, const THandle< ent::AnimatedComponent >& rootAnimComponent, Uint64& outAnimsetResourceHash )
    {
        const anim::AnimDataContext& dataCtx = ent::AnimatedComponent::GetDataContext( rootAnimComponent );
		// todo: verify AnimWrapperAcquireMode::GameplayAndCinematicAnims here, perhaps GameplayOnlyAnims is enough
        const anim::AnimWrapperId wrapperId = anim::AcquireAnimWrapperId( dataCtx, animationName, false, anim::AnimWrapperAcquireMode::GameplayAndCinematicAnims );	// passing isBaseGraph=false as smart objects are using injected animation setups (thus graphs) by design

        if ( wrapperId.IsValid() )
        {
            const anim::AnimWrapper& animWrapper = anim::GetAnimWrapper( dataCtx, wrapperId );

			if ( animWrapper.IsValid() )
			{
				return animWrapper.GetResourceHash( outAnimsetResourceHash );
			}
        }
        
        return false;
    }

	//////////////////////////////////////////////////////////////////////////
	static THandle< ent::AnimatedComponent > GetRootAnimatedComponent( const ent::Entity& user )
	{
		if ( THandle< ent::AnimatedComponent > animComponent = game::engineExt::FindComponentByType< ent::AnimatedComponent >( user ) )
		{
			return animComponent->GetRootAnimatedComponent();
		}

		return nullptr;
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::GetGatePoints( const ent::Entity& user, const game::SmartObjectVisualData::SmartObjectPoints& gatePoints, const PointFilterFunc& filterFunc, red::DynArray< game::SmartObjectPoint >& outPoints ) const
    {
		const world::AnimationSystem* animSystem = user.GetSceneSystem< world::AnimationSystem >();
		RED_ASSERT( animSystem );

		if ( THandle< ent::AnimatedComponent > animComponent = GetRootAnimatedComponent( user ) )
		{
			if ( const THandle< game::SmartObjectAnimationDatabase > animationDB = m_rootResource->GetAnimationDatabase() )
			{
				const anim::AnimatedObject* animObj = animSystem->FindAnimObjectFor( animComponent.Get() );
				anim::AnimVariableArray activeVariables;
				const Uint64 userRigHash = animComponent->GetRig().GetPath().GetHash();

				if ( animObj )
				{
					animObj->TEMP_GatherActiveVariables( activeVariables );
				}

				if ( const game::BodyTypeData* bodyTypeData = animationDB->GetBodyTypeData( userRigHash ) )
				{
					red::HashSet< Uint64 > overrideHashes{ AI::PoolAI_SORuntimeData() };

					for ( const auto& overrideAnimsetData : bodyTypeData->m_overrides )
					{
						Bool active = true;
						for ( Uint32 i = 0; i < overrideAnimsetData.m_variables.Size(); ++i )
						{
							active &= activeVariables.Exist( overrideAnimsetData.m_variables[i] );
						}

						if ( active )
						{
							overrideHashes.Insert( overrideAnimsetData.m_animsetHash );
						}
					}

					for ( const auto& point : gatePoints )
					{
						if ( filterFunc && !filterFunc( point ) )
							continue;

						if ( bodyTypeData->m_animsetHashes.Exist( point.GetAnimset() ) )
						{
							if ( const game::SmartObjectPoint* overridePoint = GetOverridePoint( overrideHashes, gatePoints, point.GetAnimationName() ) )
							{
								outPoints.PushBack( *overridePoint );
							}
							else
							{
								outPoints.PushBack( point );
							}
						}
					}
				}
			}
			// fallback solution - searching for the animsets in the user's entity
			if ( outPoints.Empty() )
			{
				for ( const auto& point : gatePoints )
				{
					if ( filterFunc && !filterFunc( point ) )
						continue;

					Uint64 animsetResourceHash = 0u;
					if ( GetAnimSetHash( user, point.GetAnimationName(), animComponent, animsetResourceHash ) && point.GetAnimset() == animsetResourceHash )
					{
						outPoints.PushBack( point );
					}
				}
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	Bool RuntimeSystemSmartObjects::TryToAddSmartObjectDemand( const world::NodePathHashType& globalNodePathHash )
	{
		RED_SCOPE_LOCK( m_deferredQueueLock );
		auto it = m_deferredInitialization.Find( globalNodePathHash );
		if ( it != m_deferredInitialization.End() )
		{
			auto& item = it.Value();
			
			if ( IsResolved( globalNodePathHash ) )
			{
				item.AddLock();
				return true;
			}
			else
			{
				if ( item.m_toBeDestroyed ) // we cannot resolve it
				{
					return false;
				}
				else
				{
					item.Resolve();

					if ( IsResolved( globalNodePathHash ) )
					{
						if ( !item.InAnyQueue() )
						{
							item.m_inResolvedQueue = true;
							m_resolvedQueue.Push( globalNodePathHash );
						}

						return true;
					}
					else
					{
						return false;
					}
				}
			}
		}

		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	Bool RuntimeSystemSmartObjects::TryToRemoveSmartObjectDemand( const world::NodePathHashType& globalNodePathHash )
	{
		RED_SCOPE_LOCK( m_deferredQueueLock );

		auto it = m_deferredInitialization.Find( globalNodePathHash );
		if ( it != m_deferredInitialization.End() )
		{
			it.Value().ReleaseLock();
			if ( !it.Value().IsLocked() && it.Value().m_toBeDestroyed )
			{
				RED_SCOPE_LOCK( m_destructionQueueLock );
				m_destroyedQueue.PushBack( globalNodePathHash );
			}
		}

		return true;
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::AddSmartObjectDemand( AI::ObjectId id )
	{
		RED_ASSERT( id.IsValid() );
		
		if ( m_smartObjectsManager )
		{
			auto soPtr = m_smartObjectsManager->GetSmartObject( id );
			if ( soPtr )
			{
				TryToAddSmartObjectDemand( soPtr->GetGlobalNodePathHash() );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RemoveSmartObjectDemand( AI::ObjectId id )
	{
		RED_ASSERT( id.IsValid() );

		if ( m_smartObjectsManager )
		{
			auto soPtr = m_smartObjectsManager->GetSmartObject( id );
			if ( soPtr )
			{
				TryToRemoveSmartObjectDemand( soPtr->GetGlobalNodePathHash() );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	Bool RuntimeSystemSmartObjects::IsSmartObjectDemanded( AI::ObjectId id ) const
	{
		auto soPtr = m_smartObjectsManager->GetSmartObject( id );
		if ( soPtr )
		{
			RED_SCOPE_LOCK( m_deferredQueueLock );
			auto it = m_deferredInitialization.Find( soPtr->GetGlobalNodePathHash() );
			if ( it != m_deferredInitialization.End() )
			{
				if ( it.Value().m_toBeDestroyed )
				{
					if ( IsResolved( it.Key() ) )
					{
						return it.Value().IsLocked();
					}
					else
					{
						return false;
					}
				}
				else
				{
					return it.Value().IsLocked();
				}
			}
		}
		return false;
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::AddResolvingCenter( const WeakHandle< ent::Entity >& centerObject, Float lifeTime )
	{
		RED_SCOPE_LOCK( m_resolvingCentersLock );

		auto it = m_resolvingCenters.Find( centerObject );
		if( it == m_resolvingCenters.End() )
		{
			m_resolvingCenters.Emplace( centerObject, m_timeSystem->GetSimTime() + lifeTime );
		}
		else
		{
			it.Value() = math::Max( it.Value(), static_cast<Float>( m_timeSystem->GetSimTime() ) + lifeTime);
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RemoveResolvingCenter( WeakHandle< ent::Entity >& centerObject )
	{
		RED_SCOPE_LOCK( m_resolvingCentersLock );

		m_resolvingCenters.Remove( centerObject );
	}

	//////////////////////////////////////////////////////////////////////////
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )	
	Bool RuntimeSystemSmartObjects::IsSelectedForDebugRender( Uint64 animsetHash ) const
	{
		if ( animsetHash == 0 )
		{
			return true;
		}

		const auto animsetIt = m_availableAnimsets.Find( animsetHash );
		return animsetIt != m_availableAnimsets.End() ? animsetIt.Value().second : false;
	}
#endif

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::OnRenderDebug( const RenderDebugContext& context )
	{
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
		rend::FragmentBuilder builder( context.debugDrawer );
		builder.EnableTransparency( true );

		const Bool doShowBoxes = context.debugDrawer.GetMask().Test( DF_ShowSpawnBoxes );

		if ( doShowBoxes )
		{
			auto allBox = Box();
			auto resolvedBox = Box();

			allBox.Clear();
			resolvedBox.Clear();

			{
				RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );
				RED_SCOPE_LOCK( m_deferredQueueLock );

				for ( auto it : m_deferredInitialization )
				{
					allBox.AddPoint( it.Value().m_position );

					auto soIt = m_nodesToIdsMapping.Find( it.Key() );
					if ( soIt != m_nodesToIdsMapping.End() )
					{
						resolvedBox.AddPoint( it.Value().m_position );
					}
				}
			}
			builder.SetColor( Color::BLUE() );
			builder.AddWireBox( allBox );
			builder.SetColor( Color::BLUE( 50 ) );
			builder.AddSolidBox( allBox );

			builder.SetColor( Color::GREEN() );
			builder.AddWireBox( resolvedBox );
			builder.SetColor( Color::GREEN( 50 ) );
			builder.AddSolidBox( resolvedBox );
		}

		const Bool doShowNotResolved = context.debugDrawer.GetMask().Test( DF_ShowSmartObjectResolutionState );

		if ( doShowNotResolved )
		{
			RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );
			RED_SCOPE_LOCK( m_deferredQueueLock );

			for ( auto it : m_deferredInitialization )
			{
				auto& value = it.Value();

				auto color = Color::GRAY( 100 );
				auto colorFull = Color::GRAY();

				if ( value.m_inResolvedQueue )
				{
					color = Color::GREEN( 100 );
					colorFull = Color::GREEN();
				}

				if ( value.m_inNotResolvedQueue )
				{
					color = Color::BLUE( 100 );
					colorFull = Color::BLUE();
				}

				builder.SetColor( colorFull );
				builder.AddWireSphere( value.m_position, 0.5f );
				builder.SetColor( color );
				builder.AddSolidSphere( value.m_position, 0.5f );
				builder.SetColor( colorFull );
				builder.AddWorldTextf( value.m_position, 0, 0, "%d,%d,%s,%s", value.m_sectors.Size(), value.m_lockCount, value.m_inResolvedQueue ? "R" : value.m_inNotResolvedQueue ? "N" : "-", value.m_toBeDestroyed ? "D" : "-");
			}
		}

		const Bool doShowResolveBubbles = context.debugDrawer.GetMask().Test( DF_ShowResolveBubbles );

		if ( doShowResolveBubbles )
		{
			RED_SCOPE_LOCK( m_resolvingCentersLock );

			for( auto it : m_resolvingCenters )
			{
				if( auto centerHandle = it.Key().ToHandle() )
				{
					math::Random rand( centerHandle->GetEntityID().CalcHash() );

					auto baseColor = Color( rand.Get< Uint32 >() );
					baseColor.FromVectorLinear( baseColor.ToVector().Normalized3() );
					baseColor.A = 255u;

					Vector3 centerPos = centerHandle->GetWorldPosition().AsVector3();
					{			
						auto color = baseColor * 0.5f;
						color.A = 100u;

						auto colorFull = baseColor * 0.5f;
						colorFull.A = 255u;

						builder.SetColor( colorFull );
						builder.AddWireSphere( centerPos, m_defaultResolutionDistance.first );
						builder.SetColor( color );
						builder.AddSolidSphere( centerPos, m_defaultResolutionDistance.first );
					}

					{
						auto color = baseColor;
						color.A = 100u;

						auto colorFull = baseColor;
						colorFull.A = 255u;

						builder.SetColor( colorFull );
						builder.AddWireSphere( centerPos, m_defaultResolutionDistance.second );
						builder.SetColor( color );
						builder.AddSolidSphere( centerPos, m_defaultResolutionDistance.second );
					}
				}
			}
		}
		
#endif

#ifdef RED_USE_IMGUI		
		static const CName animsetsWindowName = RED_NAME_CONSTEXPR( "FilterByAnimset##RuntimeSystemSmartObjects" );

		if ( !context.debugDrawer.GetMask().Test( DF_SmartObjectsFilterPerAnimset ) && !red::ImGui::IsWindowRequested( animsetsWindowName ) )
		{
			return;
		}

		if ( ImGui::Begin( animsetsWindowName.AsChar() ) )
		{
			if ( !m_rootResource )
			{
				ImGui::TextColored( red::ImGui::DARK_RED(), "Failed to load smart object database resource" );
			}

			if ( m_availableAnimsets.Empty() )
			{
				ImGui::TextColored( red::ImGui::DARK_ORANGE(), "No animsets containing smart objects animation found" );
			}

			for ( auto animset : m_availableAnimsets )
			{
				ImGui::Checkbox( animset.Value().first.AsChar(), &animset.Value().second );
			}
		}

		ImGui::End();
#endif
	}
	
	//////////////////////////////////////////////////////////////////////////
#if RED_CTSW( RED_SMART_OBJECTS_DEBUG_OR_EDITOR_ENABLED )
	void RuntimeSystemSmartObjects::RenderSmartObject( AI::ObjectId id, rend::IDebugDrawer& dd, rend::FragmentBuilder& builder, Bool isSelected ) const
	{
		game::SmartObjectRenderContext renderContext( dd, builder, *this, isSelected );
		if ( !InEditor() )
		{
			m_smartObjectsManager->RenderSmartObject( id, renderContext );
		}
		else
		{
			auto iter = m_smartObjectVisuals.Find( id );
			if ( iter != m_smartObjectVisuals.End() )
			{
				ObjectVisualStubPtr visualStub = iter.Value();
				RED_ASSERT( visualStub, "An attempt to update the smart object visual stub with id '%llu' that was not created", id.GetValue() );
				visualStub->RenderDebug( renderContext );
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RefreshSmartObject( AI::ObjectId id, const game::SmartObjectDefinition& def, const WorldTransform& currentTransform, const world::NodePathHashType& globalNodePathHash )
	{
		RED_SCOPE_SHARED_LOCK( m_smartObjectVisualsLock );

		const game::SmartObjectInitializationContext context( currentTransform, def, EvaluateResource( globalNodePathHash ).second, m_navigationSystem, m_physicsSystem->GetSceneID(), m_metrics );

		auto iter = m_smartObjectVisuals.Find( id );
		if ( iter != m_smartObjectVisuals.End() )
		{
			ObjectVisualStubPtr visualStub = iter.Value();
			RED_ASSERT( visualStub, "An attempt to update the smart object visual stub with id '%llu' that was not created", id.GetValue() );
			visualStub->Refresh( context );
		}
	}
#endif

	//////////////////////////////////////////////////////////////////////////
	std::pair< Uint64, THandle< game::SmartObjectsCompiledResource > > RuntimeSystemSmartObjects::EvaluateResource( const world::NodePathHashType& globalNodePathHash ) const
	{
		auto soDB = m_rootResource;

		{
			RED_SCOPE_SHARED_LOCK( m_lookupLock );
			for ( auto& db : m_sectorDatabase )
			{
				if ( db.Value()->CheckMembership( globalNodePathHash ) )
				{
					return { db.Key(), db.Value() };
				}
			}
		}

		return { 0u, soDB };
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::IsResolved( world::NodePathHashType globalNodePathHash ) const
	{
		RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );
		return m_nodesToIdsMapping.KeyExist( globalNodePathHash );
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::IsRootResource( THandle< game::SmartObjectsCompiledResource > res ) const
	{
		return res == m_rootResource;
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::InEditor() const
	{
#if defined( RED_CONFIGURATION_FINAL )
		return false;
#else
		return m_smartObjectsManager == nullptr;
#endif			
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::CanBeResolved( world::NodePathHashType globalNodePathHash ) const
	{
		return !IsRootResource( EvaluateResource( globalNodePathHash ).second );
	}
	
	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::RegisterUpdateFunctions( UpdatableSystemRegistrar& registrar )
	{
		registrar.Register( UpdateTickGroup::PreBuckets, *this, "SOSystem/Update",
							[this]( const UpdateContext& updateContext, job::Builder& builder )
		{
			const tick::Info& tickInfo = updateContext.m_tickInfo;
#if defined( RED_CONFIGURATION_FINAL )
			Update( tickInfo.m_globalTimeDilation * tickInfo.m_realTimeDelta );
#else

#ifndef NO_EDITOR 
			if ( InEditor() )
			{
				UpdateInEditor( tickInfo.m_globalTimeDilation * tickInfo.m_realTimeDelta );
			}
			else
#endif
			{
				Update( tickInfo.m_globalTimeDilation * tickInfo.m_realTimeDelta );
			}
#endif
		} );
	}

	//////////////////////////////////////////////////////////////////////////
#ifndef NO_EDITOR 
	void RuntimeSystemSmartObjects::UpdateInEditor( Float dt )
	{
		RED_ASSERT( InEditor() );
		
		RED_SCOPE_LOCK( m_deferredQueueLock );

		auto itemLimit = m_notResolvedQueue.Size();
		for ( auto i = 0u; i < itemLimit; ++i )
		{
			auto item = m_notResolvedQueue.Pop();
			auto it = m_deferredInitialization.Find( item );

			if ( it != m_deferredInitialization.End() )
			{
				it.Value().Resolve();
				it.Value().m_inNotResolvedQueue = false;
			}
		}

		m_resolvedQueue.Clear();
		m_notResolvedQueue.Clear();
	}
#endif

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::UpdateSectors()
	{
		RED_SCOPE_LOCK( m_deferredQueueLock );

		red::DynArray< Uint64 > toRemove = { job::PoolJobScope() };
		toRemove.Reserve( m_sectorMembershipLUT.Size() );

		auto budgetToBeUsedS = m_currentBudgetS * 0.25f;
		auto currentTimeBudgetS = budgetToBeUsedS;

		for ( auto it : m_sectorMembershipLUT )
		{
			red::StopWatch time;

			auto& sector = it.Value();

			sector.Update();
			if ( sector.CanBeRemoved() )
			{
				toRemove.PushBack( it.Key() );
			}

			//we go up to the maximum of 50% of the current frame's budget
			{
				currentTimeBudgetS -= time.GetDeltaF();
				if ( currentTimeBudgetS < 0.0f )
				{
					currentTimeBudgetS = 0.0f;
					break;
				}
			}
		}

		m_currentBudgetS -= budgetToBeUsedS - currentTimeBudgetS;

		for ( auto& id : toRemove )
		{
			m_sectorMembershipLUT.Remove( id );
		}
	}

	//////////////////////////////////////////////////////////////////////////
	void RuntimeSystemSmartObjects::Update( Float dt )
	{
		PC_SCOPE( "RuntimeSmartObject_Update" );
		
		auto cameraPosition = m_cameraSystem->GetActiveCameraPosition();

		m_currentBudgetS = math::ClampFromAbove( m_currentBudgetS, m_totalBudgetPerFrameS * 0.5f ) + m_totalBudgetPerFrameS; //we carry at most 50% of the time budget for the update if not used last time

		red::DynArray< std::pair< Vector3, Float > > centerPositions{ job::PoolJobScope() };

		{
			RED_SCOPE_LOCK( m_resolvingCentersLock );

			auto it = m_resolvingCenters.Begin();
			while ( it != m_resolvingCenters.End() )
			{
				auto centerHandle = it.Key().ToHandle();
				if ( !centerHandle || !centerHandle->IsAttached() || m_timeSystem->GetSimTime() > it.Value() )
				{
					it = m_resolvingCenters.Remove( it ).Iterator();
				}
				else
				{
					++it;
				}
			}

			centerPositions.Reserve( m_resolvingCenters.Size() );

			for ( auto it : m_resolvingCenters )
			{
				if ( auto centerHandle = it.Key().ToHandle() )
				{
					auto pos = centerHandle->GetWorldPosition().AsVector3();
					centerPositions.EmplaceBack( pos, pos.DistanceSquaredTo( cameraPosition ) );
				}
			}
		}

		if ( m_cameraSystem )
		{
			std::sort( centerPositions.Begin(), centerPositions.End(), [cameraPosition = m_cameraSystem->GetActiveCameraPosition()]( const auto& lhs, const auto& rhs )
			{
				return lhs.second < rhs.second;
			} );
		}


		UpdateSectors();

		//in the resolve queue - we check for SOs to be un-resolved due to distance
		{
			PC_SCOPE( "RuntimeSmartObject_Update_ResolvedQueue" );

			RED_SCOPE_LOCK( m_deferredQueueLock );
			
			auto timeLimitBudgetS = math::ClampFromBelow( m_currentBudgetS * 0.4f, m_totalBudgetPerFrameS * 0.3f ); //this takes 40% of what is left of the budget
			auto timeLimitS = timeLimitBudgetS;

			auto itemLimit = math::Min( m_soUnresolvingLimitPerFrame, m_resolvedQueue.Size() );
			auto totalLimit = m_resolvedQueue.Size();

			red::HashSet< world::NodePathHashType > nodesInsideBiggerSphere{ m_resolvedQueue.Size(), job::PoolJobScope() };

			//note - this can be fairly costly - optimization needed

			for( const auto& center : centerPositions )
			{
				m_smartObjectsLookup.Query( Box( center.first, m_defaultResolutionDistance.second ), [ &nodesInsideBiggerSphere ]( const auto& so )
				{
					nodesInsideBiggerSphere.Emplace( so );
					return true;
				} );
			}

			// RESOLVED QUEUE 
			for( auto i = 0u; ( i < itemLimit ) && ( timeLimitS >= 0.0f ) && ( totalLimit > 0u ); ++i, --totalLimit )
			{
				red::StopWatch time;

				auto item = m_resolvedQueue.Pop();

				auto it = m_deferredInitialization.Find( item );

				if( it != m_deferredInitialization.End() )
				{
					RED_ASSERT( it.Value().m_inResolvedQueue );
					RED_ASSERT( IsResolved( item ) );

					it.Value().m_inResolvedQueue = false;

					if( !it.Value().IsLocked() ) //locked items have override over everything, even being destroyed
					{
						if( !nodesInsideBiggerSphere.Exist( it.Key() ) )
						{
							RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );

							auto soIt = m_nodesToIdsMapping.Find( it.Key() );
							RED_ASSERT( soIt != m_nodesToIdsMapping.End() ); //it is resolved

							m_smartObjectsManager->UnregisterSmartObject( soIt.Value() );
							m_nodesToIdsMapping.Remove( soIt );

							if( it.Value().m_toBeDestroyed ) //we do not put this in any queue anymore
							{
								m_smartObjectsLookup.Remove( it.Value().m_treeNodeId );
								m_deferredInitialization.Remove( it );
							}
							else
							{
								it.Value().m_inNotResolvedQueue = true;
							}
						}
						else
						{
							++itemLimit;

							//the object is resolved
							it.Value().m_inResolvedQueue = true;
							m_resolvedQueue.Push( item );
						}
					}
					else
					{
						++itemLimit;

						//the object is resolved
						it.Value().m_inResolvedQueue = true;
						m_resolvedQueue.Push( item );
					}
				}

				timeLimitS -= time.GetDeltaF();
			}

			timeLimitS = math::ClampFromBelow( timeLimitS, 0.0f );
			m_currentBudgetS -= math::ClampFromBelow( timeLimitBudgetS - timeLimitS, 0.0f );
		}

		{
			PC_SCOPE( "RuntimeSmartObject_Update_NOT_ResolvedQueue" );

			RED_SCOPE_LOCK( m_deferredQueueLock );

			auto timeLimitBudgetS = math::ClampFromBelow( m_currentBudgetS * 0.8f , m_totalBudgetPerFrameS * 0.3f ); //we take 80% of what is left and guarantee 30% of the initial budget
			auto timeLimitS = timeLimitBudgetS;
			auto itemLimit = m_soResolvingLimitPerCenter;

			if ( !centerPositions.Empty() )
			{
				for( const auto& centerPos : centerPositions )
				{
					itemLimit = m_soResolvingLimitPerCenter;

					m_smartObjectsLookup.Query( Box( centerPos.first, m_defaultResolutionDistance.first ), [ & ]( const auto& item )
					{
						auto it = m_deferredInitialization.Find( item );

						if ( it != m_deferredInitialization.End() ) //we ignore objects that are not registered
						{
							if ( !it.Value().m_inNotResolvedQueue )
							{
								return true;
							}

							it.Value().m_inNotResolvedQueue = false;

							if ( IsResolved( item ) ) //this is already resolved we can move it to the proper queue
							{
								it.Value().m_inResolvedQueue = true;
								m_resolvedQueue.Push( item );
							}
							else
							{
								if ( it.Value().m_toBeDestroyed ) //it is not resolved, marked to be removed
								{
#ifdef RED_ASSERTS_ENABLED
									RED_SCOPE_SHARED_LOCK( m_nodesToIdsLock );
									RED_ASSERT( !m_nodesToIdsMapping.KeyExist( it.Key() ) );
#endif
									m_deferredInitialization.Remove( it );
								}
								else
								{
									red::StopWatch time;

									it.Value().Resolve(); //it may happen there is no sector data and this object stays unresolved	

									timeLimitS -= time.GetDeltaF();

									if ( IsResolved( item ) )
									{
										it.Value().m_inResolvedQueue = true;
										m_resolvedQueue.Push( item );
									}
									else
									{
										it.Value().m_inNotResolvedQueue = true;
									}
								}

								if ( timeLimitS < 0.0f || --itemLimit == 0u )
								{
									return false;
								}
							}
						}

						return true;
					} );

					if ( timeLimitS < 0.0f )
					{
						timeLimitS = 0.0f;
						break;
					}
				}

				timeLimitS = math::ClampFromBelow( timeLimitS, 0.0f );
				m_currentBudgetS -= math::ClampFromBelow( timeLimitBudgetS - timeLimitS, 0.0f );
			}
		}

		{
			PC_SCOPE( "RuntimeSmartObject_Update_DestructionQueue" );

			auto timeLimitBudgetS = math::ClampFromBelow( m_currentBudgetS, m_totalBudgetPerFrameS * 0.1f ); //we take what is left, guarantee 10% of the total budget
			auto timeLimitS = timeLimitBudgetS;

			RED_SCOPE_LOCK( m_destructionQueueLock );
			RED_SCOPE_LOCK( m_deferredQueueLock );

			while ( !m_destroyedQueue.Empty() && timeLimitS > 0.0f )
			{
				red::StopWatch time;

				auto item = m_destroyedQueue.Pop();

				auto it = m_deferredInitialization.Find( item );

				if ( it != m_deferredInitialization.End() ) //we ignore objects that are not registered
				{
					if ( !it.Value().m_toBeDestroyed ) continue;
					if ( it.Value().m_inResolvedQueue ) continue; //Resolve loop will managed
					if ( it.Value().IsLocked() ) continue; //locked cannot be destroyed, unlock will return it to the destroy queue

					m_smartObjectsLookup.Remove( it.Value().m_treeNodeId );
					m_deferredInitialization.Remove( it );
				}

				timeLimitS -= time.GetDeltaF();
			}

			timeLimitS = math::ClampFromBelow( timeLimitS, 0.0f );
			m_currentBudgetS -= math::ClampFromBelow( timeLimitBudgetS - timeLimitS, 0.0f );
		}

	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::DeferredSmartObjectInitialization( const OnDeferredInitializationFunc& call, const Vector3& pos, decltype( m_smartObjectsLookup )::EntryId treeNodeId, Uint64 sectorId /*= 0u */ )
		: m_deferredCall( call )
		, m_position( pos )
		, m_treeNodeId( treeNodeId )
	{
		if ( sectorId != 0u )
		{
			m_sectors.PushBack( sectorId );
		}
	}
	
	//////////////////////////////////////////////////////////////////////////
	RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::DeferredSmartObjectInitialization() = default;

	//////////////////////////////////////////////////////////////////////////
	RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::~DeferredSmartObjectInitialization() = default;

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::InAnyQueue() const
	{
		return m_inResolvedQueue || m_inNotResolvedQueue;
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE Bool RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::IsLocked() const
	{
		return m_lockCount > 0u;
	}
	
	//////////////////////////////////////////////////////////////////////////
	RED_INLINE void RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::AddLock()
	{
		++m_lockCount;
		RED_ASSERT( m_lockCount < std::numeric_limits< decltype( m_lockCount ) >::max() );
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE void RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::ReleaseLock()
	{
		if ( m_lockCount > 0u )
		{
			--m_lockCount;
		}
		//RED_ASSERT( m_lockCount > 0u );
	}

	//////////////////////////////////////////////////////////////////////////
	RED_INLINE void RuntimeSystemSmartObjects::DeferredSmartObjectInitialization::Resolve()
	{
		PC_SCOPE( SmartObject_Resolve );
		RED_ASSERT( !!m_deferredCall );
		m_deferredCall();
	}
} // namespace world

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
namespace world
{
	RuntimeSystemSmartObjects::SectorMembership::SectorMembership() = default;
	RuntimeSystemSmartObjects::SectorMembership::~SectorMembership() = default;

	//================================================================================
	RuntimeSystemSmartObjects::SectorMembership::SectorMembership( red::DynArray< world::NodePathHashType >&& src, const Call& activation, const Call& deactivation )
		: m_backlog( std::move( src ) )
		, m_activation( activation )
		, m_deactivation( deactivation )
	{
		m_processed.Reserve( m_backlog.Size() );
	}

	//================================================================================
	Bool RuntimeSystemSmartObjects::SectorMembership::CanBeRemoved() const
	{
		if ( !m_alive )
		{
			return m_processed.Empty();
		}
		else
		{
			return false;
		}
	}

	//================================================================================
	void RuntimeSystemSmartObjects::SectorMembership::Update()
	{
		auto timeLimitMS = m_updateWindow * 1000.0f;
		auto processed = 0u;

		if ( m_alive )
		{
			if ( !m_backlog.Empty() )
			{
				for ( auto& item : m_backlog.Reverse() )
				{
					red::StopWatch time;

					m_activation( item );
					m_processed.PushBack( item );
					++processed;

					timeLimitMS -= time.GetDeltaF() * 1000.f;

					if ( timeLimitMS < 0.0f ) break;
				}
				m_backlog.RemoveAt( m_backlog.Size() - processed, m_backlog.Size() );
			}
		}
		else
		{
			if ( !m_processed.Empty() )
			{
				for ( auto& item : m_processed.Reverse() )
				{
					red::StopWatch time;

					m_deactivation( item );
					m_backlog.PushBack( item );
					++processed;

					timeLimitMS -= time.GetDeltaF() * 1000.f;

					if ( timeLimitMS < 0.0f ) break;
				}
				m_processed.RemoveAt( m_processed.Size() - processed, m_processed.Size() );
			}
		}
	}

}