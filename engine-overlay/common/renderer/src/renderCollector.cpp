/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "renderCollector.h"
#include "renderProxyMesh.h"
#include "renderProxyLight.h"
#include "renderProxyFogVolume.h"
#include "renderShadowManager.h"
#include "renderElementMeshChunk.h"
#include "renderScene.h"
#include "renderGeometryBatcher.h"
#include "renderDynamicDecalChunk.h"
#include "renderMaterial.h"
#include "renderDistantLightBatcher.h"
#include "renderShaderPair.h"
#include "../../renderData/include/renderLightBase.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/renderFrame.h"
#include "../../renderData/include/rendererDevConfig.h"
#include "renderProxyLightVolume.h"
#include "renderDistantShadows.h"

#include "../../redJobs2/include/jobBuilder.h"
#include "../../renderData/include/materialDataProvider.h"
#include "../../renderData/include/meshEnum.h"
#include "../../visibility/include/visRasterizerSSEBase.h"
#include "visibilityScene.h"
#include "visibilityQueries.h"
#include "visibilityQueryBase.h"
#include "visibilityQuerySingleFrustum.h"
#include "visibilityQueryRayTracedReflections.h"

// #pragma optimize("",off)

#ifdef USE_RAY_TRACING
#include "renderAccelerationStructure.h"
#endif

#include "renderProxyDrawable.h"
#include "../../renderData/include/timeOfDayVisibilityResource.h"

using red::DynArray;

namespace Config
{
#ifdef USE_RAY_TRACING
	TConfigVar<Float> cvRayTracingVisibilityFrustumOffset( "RayTracing", "VisibilityFrustumOffset", 200.0f, eConsoleVarFlag_Developer );
	TConfigVar<Float> cvRayTracingVisibilityCullingRadius( "RayTracing", "VisibilityCullingRadius", 2000.0f, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvRayTracingEnableDirectionalShadowCulling( "RayTracing", "EnableDirectionalShadowCulling", true, eConsoleVarFlag_Developer );
#endif

	TConfigVar<Int32> cvOcclusionBufferScale( "Occlusion", "DepthBufferDimScale", 6, eConsoleVarFlag_Developer );
	TConfigVar<Float> cvOcclusionMinNearPlane( "Occlusion", "MinNearPlane", 0.2f, 0.0f, 10.0f, eConsoleVarFlag_Developer );
	TConfigVar<Float> cvOcclusionMaxFarPlane( "Occlusion", "MaxFarPlane", 8000.0f, 0.0f, 20000.0f, eConsoleVarFlag_Developer );
}

#ifndef RED_CONFIGURATION_FINAL
red::Atomic<Bool> CRenderCollector::s_wasLocalShadowsOverflow( false );
red::Atomic<Bool> CRenderCollector::s_wasLocalShadowsQueueOverflow( false );
red::Atomic<Bool> CRenderCollector::s_wasLightAllocationOverflow( false );
red::Atomic<Bool> CRenderCollector::s_wasParticleEmitterOverflow( false );
red::Atomic<Int32> CRenderCollector::s_wasParticleAmountOverflow( 0 );
red::Atomic<Bool> CRenderCollector::s_wasRenderStageChunkAllocatorOverflow( false );
#endif // RED_CONFIGURATION_FINAL

CRenderCollector::CRenderCollector( )
	: m_frameIndex( 100 )
	, m_info( nullptr )
	, m_camera( nullptr )
	, m_scene( nullptr )
	, m_renderCollectorData( nullptr )
	, m_baseFeatureFlag( FEATFLAG_Default )
{}

CRenderCollector::~CRenderCollector()
{
	// Validation. Pointers SHOULD be cleared during last Reset on RenderFrame. But if somehow it wont happen
	// and here there will be pointers, shutting down will cause crash due to dangling pointers

	Bool hasAnyPointers = false;
	hasAnyPointers |= ( m_scene != nullptr );
	hasAnyPointers |= ( m_renderCollectorData != nullptr );
	hasAnyPointers |= ( m_info != nullptr );
	hasAnyPointers |= ( m_camera != nullptr );
		
	RED_FATAL_ASSERT( hasAnyPointers == false , "One of the poiters are still there. Please, investigae !" );

	Reset();
}

void CRenderCollector::Setup( const CRenderFrameInfo& info, EFeatureFlag baseFeatureFlag, const CRenderCamera& camera, CRenderSceneEx* scene, CRenderCollectorData* data, const CRenderCameraFrameInfo* cameraInfo )
{
	PC_SCOPE( RenderCollector_Setup )

	m_renderStageCollector.SetAllocator( GetRenderer()->GetRenderStageChunksAllocator() );
	m_localShadowsCollector.m_renderStageCollectorStatic.SetAllocator(GetRenderer()->GetRenderStageChunksAllocator());
	m_localShadowsCollector.m_renderStageCollectorDynamic.SetAllocator(GetRenderer()->GetRenderStageChunksAllocator());

	m_frameIndex = 100;
	m_mainCameraQuery = nullptr;

	m_baseFeatureFlag = baseFeatureFlag;

	m_scene					= scene;
	m_renderCollectorData	= data;
	m_info					= &info;
	m_camera				= &camera;
	m_cameraInfo			= cameraInfo;

	m_renderingStats.Reset();
	
	if ( scene )
	{
		m_frameIndex = scene->GetLastAllocatedFrame();
		m_dissolveSynchronizer = scene->GetDissolveSynchronizer();

		m_renderCollectorData->m_visibleCpuParticlesMask.Resize( m_scene->m_rpParticles.Size() );
		m_renderCollectorData->m_visibleCpuParticlesMask.ClearAll();
	}

	// Reset collector data
	RED_ASSERT( m_renderCollectorData );
	m_renderCollectorData->Reset();	

	// always reset the shadow manager
	m_localShadowsCollector.Reset();

	if ( scene )
	{
		// Spawn dynamic decals queued on a worker thread during particles simulation
		//scene->SpawnQueuedDynamicDecals();
	}

	VerifyData();
}


void CRenderCollector::Reset()
{
	// Clear when we're done with it. Otherwise the data could potentially end up holding references to stuff long after scene
	// rendering (e.g. during shutdown, if it's a global object).
	if (m_renderCollectorData)
	{
		m_renderCollectorData->Reset();
	}

	m_cascades.Reset();

	m_renderStageCollector.Clear();
	m_localShadowsCollector.Reset();

	// Clear pointers on the end of the frame. Not clearing pointers would lead to leave dangling pointers
	// and in destructor there could be crash
	m_scene						= nullptr;
	m_renderCollectorData		= nullptr;
	m_info						= nullptr;
	m_camera					= nullptr;
	m_mainCameraQuery			= nullptr;

	m_numDecals.SetValue( 0 );

	m_renderingStats.Reset();
}

void CRenderCollector::CalculateCascadesData( const CRenderCamera& camera, struct SCascadesStabilizer* stabilizer )
{
	PC_SCOPE( CalculateCascadesData );

	RED_FATAL_ASSERT( m_scene, "Assert because previously in cascades.init function there was a condition nullptr != scene - imo always satisfied" );
	RED_FATAL_ASSERT( nullptr != m_info, "Missing frame info" );

	m_cascades.Init( *m_info, *m_cameraInfo, camera, stabilizer );
}

// [skperf] round 19 mirrorOccl (renderMaterialModifiers.cpp overlay): occlusion near plane for a mirror camera.
Bool CpMirrorOcclusionNear( const WorldPosition& cameraPosition, Float& outNear, Bool& outOk );
// [skperf] round 48/49 mirrorCull: angle (radians) for a published mirror camera (by camera hash or builder, round 49),
// 0 otherwise (renderMaterialModifiers.cpp); the query side (visibilityQuerySingleFrustum.cpp) is keyed by the vis
// query camera's exact position, which this function registers from the same params the query copies.
// [skperf] round 50 mirrorFrustum: true for a published mirror camera with the switch on (outAngle = mirrorCull angle).
Bool CpMirrorQueryFor( Uint64 cameraHash, const void* builder, Float& outAngle );
void CpMirrorCullRegisterQuery( Float x, Float y, Float z, Float angle, const Float* boxMinMax );

void CRenderCollector::DoCull_PrepareSceneQuery( const SRenderNodeImplContext& rctx, job::Builder& builder )
{
	PC_SCOPE( DoCull_PrepareSceneQuery );

	const vis::ViewID visViewId = rctx.GetVisibilityView();

	RED_FATAL_ASSERT( m_mainCameraQuery == nullptr, "Already have a scene query" );
	RED_FATAL_ASSERT( nullptr != m_info, "Missing frame info" );

	auto& visScene = m_scene->GetVisibilityScene();
	const rend::DebugFilterMask& filters = m_cameraInfo->GetRenderingMask();

	CRenderCamera nonJitteredCamera = *m_camera;
	nonJitteredCamera.SetSubpixelOffset( 0.0f, 0.0f, 1, 1 );

	vis::SQueryParams* queryParams = nullptr;
	vis::SQueryParams_SingleFrustum queryParams_SingleFrustum;
	vis::SQueryParams_Box queryParams_Box;
	
	// [skperf] round 50 mirrorFrustum (opt-in ?mirrorFrustum=1 or ?mirrorCull=N): a mirror camera with a culling box is
	// collected by its single-frustum query (box kept as a per-object filter) instead of the box-only query.
	Float cpMirrorAngle = 0.f;
	Bool cpMirrorFrustum = false;
	if ( !m_cameraInfo->m_cullingBox.IsEmpty() )
	{
		const auto* cpCameraData = rctx.GetCameraData();
		cpMirrorFrustum = cpCameraData && CpMirrorQueryFor( cpCameraData->m_cameraHash, cpCameraData->m_cameraBuilder.Get(), cpMirrorAngle );
	}
	if (m_cameraInfo->m_cullingBox.IsEmpty() || cpMirrorFrustum)
	{
		queryParams_SingleFrustum.m_viewId = visViewId;
		queryParams_SingleFrustum.m_camera.Set( nonJitteredCamera.GetWorldToView(), nonJitteredCamera.GetViewToScreenRevProjAware() );
		if ( cpMirrorFrustum ) // [skperf] round 50: register this query (keyed by its vis camera position) with the box + angle
		{
			const Box& cpBox = m_cameraInfo->m_cullingBox;
			const Float cpBoxMinMax[ 6 ] = { cpBox.Min.X, cpBox.Min.Y, cpBox.Min.Z, cpBox.Max.X, cpBox.Max.Y, cpBox.Max.Z };
			const auto& cpPos = queryParams_SingleFrustum.m_camera.GetPosition();
			CpMirrorCullRegisterQuery( cpPos.X, cpPos.Y, cpPos.Z, cpMirrorAngle, cpBoxMinMax );
		}

		// adjusted camera for occlusion culling
		// we're lowering far plane and increasing near plane for better precission (occlusion buffer is only 16-bit)
		Float cpMirrorNear = 0.f; Bool cpMirrorOk = false; // [skperf] round 19 mirrorOccl (opt-in; no entry = unchanged)
		const Bool cpIsMirror = CpMirrorOcclusionNear( m_camera->GetPosition(), cpMirrorNear, cpMirrorOk );
		{
			if ( cpIsMirror && cpMirrorOk ) nonJitteredCamera.SetNearPlane( Max( nonJitteredCamera.GetNearPlane(), cpMirrorNear ) );
			nonJitteredCamera.SetNearPlane( Max( nonJitteredCamera.GetNearPlane(), Config::cvOcclusionMinNearPlane.Get() ) );
			nonJitteredCamera.SetFarPlane( Min( nonJitteredCamera.GetFarPlane(), Config::cvOcclusionMaxFarPlane.Get() ) );
			nonJitteredCamera.CalculateMatrices();
			queryParams_SingleFrustum.m_occlusionCamera.Set( nonJitteredCamera.GetWorldToView(), nonJitteredCamera.GetViewToScreenRevProjAware() );
		}

		// Find a resolution that matches the render's aspect ratio, but the same number of pixels as 1920x1080, and use
		// that for the occlusion buffer. This way it should have a reasonably consistent number of tiles at different
		// resolutions, so similar memory/processing footprints.
		//    virtualWidth/virtualHeight = widthOverHeight
		//    virtualWidth*virtualHeight = targetNumPixels
		const Float widthOverHeight = Float(rctx.GetWidth()) / Float(rctx.GetHeight());
		const Float targetNumPixels = 1920*1080;
		const Uint32 virtualWidth = Uint32( MCeil( MSqrt( targetNumPixels*widthOverHeight ) ) );
		const Uint32 virtualHeight = Uint32( MCeil( MSqrt( targetNumPixels/widthOverHeight ) ) );

		Int32 scale = Max( Config::cvOcclusionBufferScale.Get(), 1 );
		queryParams_SingleFrustum.m_occlusionWidth = virtualWidth/scale;
		queryParams_SingleFrustum.m_occlusionHeight = virtualHeight/scale;


		// Only use occlusion if we're actually rendering the scene. If this is a debug fragment only render, then even
		// thinking about occlusion is wasted effort.
		// #TODO: Really, even creating the query is a waste, since debug fragments aren't tested against it.
		if ( m_cameraInfo != nullptr && IRenderScene::IsValid( m_cameraInfo->m_renderScene ) )
		{
			// Only use occlusion if we're in the default scene layer.
			if ( m_cameraInfo->m_sceneLayer == RenderSceneLayer::Default )
			{
				// NOTE: The debug filter will only influence cameras that specified m_useCulling to be true.
				queryParams_SingleFrustum.m_useOcclusion = filters.Test( rend::DF_VisCameraUseOcclusion ) && m_cameraInfo->m_useOcclusionCulling;
				if ( cpIsMirror && !cpMirrorOk ) queryParams_SingleFrustum.m_useOcclusion = false; // [skperf] grazing mirror view
			}
		}

		queryParams_SingleFrustum.m_queryMask.m_scene = true;
		queryParams = &queryParams_SingleFrustum;
	}
	else
	{
		queryParams_Box.m_queryMask.m_scene = true;
		queryParams_Box.m_box = m_cameraInfo->m_cullingBox;

		queryParams = &queryParams_Box;
	}

	if ( filters.Test( rend::DF_VisCameraShowOcclusionBuffer ) )
	{
		if ( filters.Test( rend::DF_VisOccBufferRoughMin ) )
		{
			queryParams->m_debugSettings.OcclusionBufferRoughMin = true;
		}
		else if ( filters.Test( rend::DF_VisOccBufferRoughMax ) )
		{
			queryParams->m_debugSettings.OcclusionBufferRoughMax = true;
		}
		else if ( filters.Test( rend::DF_VisOccBufferInternal ) )
		{
			queryParams->m_debugSettings.OcclusionBufferInternal = true;
		}
		else
		{
			queryParams->m_debugSettings.OcclusionBuffer = true;
		}
	}
	queryParams->m_debugSettings.OccludersUsed = filters.Test(rend::DF_VisCameraShowOccluders);
	queryParams->m_debugSettings.OccludersRasterized = filters.Test(rend::DF_VisCameraShowOccludersRasterized);
	queryParams->m_debugSettings.SectorsUsed = filters.Test(rend::DF_VisCameraShowSectors);
	queryParams->m_debugSettings.VisibleTargetBounds = filters.Test(rend::DF_VisCameraVisibleTargets);
	
#ifndef RED_CONFIGURATION_FINAL
	//
	// Debug: Visualize light channels in occlusion buffer.
	//
	if ( rctx.GetCameraInfo().m_lightChannelsOccBuffPreviewRequested )
	{
		auto& visScene = m_scene->GetVisibilityScene();
		{
			const auto boxVisibilityCallback = []( const Box& boundingBox, const EFrustumResult parentFrustumResult )
			{
				return true;//TODO - could actually do some frustum test... but it's just for debug...
			};

			const auto traverseCallback = [&rctx, &visScene, queryParams]( auto&& clustersCollected )
			{
				for ( const auto& currNode : clustersCollected )
				{
					for ( const auto& nodeData : currNode.GetNode()->m_objects )//TODO - could actually do some frustum test... but it's just for debug...
					{
						IRenderProxyCollectable * proxy = static_cast<IRenderProxyCollectable *>( nodeData.m_userData );

						if ( proxy->GetType() == RPT_LightVolume )
						{
							CRenderProxy_LightVolume* vol = static_cast<CRenderProxy_LightVolume *>( proxy );
							if ( vol->GetGroup() == rend::LightChannelGroup::LCG_LightClippingVolume /* &&( vol->GetChannels() & rend::LC_ChannelWorld ) == 0 */ )
							{
								vis::SQueryParams::LCMeshDebugInfo lcmdi = {};
								lcmdi.m_vertices = vol->GetVertices();
								lcmdi.m_indices = vol->GetIndices();
								lcmdi.m_l2w = vol->GetLocalToWorld();

								queryParams->m_cachedDebugMeshesInfo.PushBack( lcmdi );
							}
						}
					}
				}
			};

			VisibilityGridStats visGridStats = m_scene->m_rpMainSceneProxies.TraverseGridAndQueryNodes<2048, 64>( traverseCallback, boxVisibilityCallback );
		}
	}
#endif

	m_mainCameraQuery = visScene.CreateQuery( *queryParams, builder );

#ifdef USE_STATIC_PROXIES_BVH_TREE
	Bool forceBVHImproveMemorylayout = false;
#ifndef RED_CONFIGURATION_FINAL
	if ( auto devConfig = GetRenderer()->GetDevConfig() )
	{
		if ( devConfig->VisibilityBVH.m_fullOptimalRebuild )
		{
			m_scene->m_rpMainSceneProxies.RebuildWithOptimalStructure_Debug();

			devConfig->VisibilityBVH.m_fullOptimalRebuild = false;
			forceBVHImproveMemorylayout = true;
		}
	}
#endif

	//Prepare BVH structure, linearize memory layout of the nodes in the BVH to make traversal faster
	m_scene->m_rpMainSceneProxies.ImproveMemoryLayout( forceBVHImproveMemorylayout );

#endif

}

void CRenderCollector::DoCull_MainScene( const SRenderNodeImplContext& rctx, job::Builder& builder )
{
	if ( m_scene )
	{
		RED_ASSERT( m_mainCameraQuery, "Have no scene query" );

		// Background stuff
		m_scene->CollectBackgroundDrawablesNoFence( rctx, *this, builder );

		// Normal drawables relative to the viewport
		m_scene->CollectMainSceneDrawablesNoFence( rctx, *this, m_mainCameraQuery.Get(), builder);

	}

	// Make sure done collecting drawables
	builder.DispatchFenceExplicitly();
}

RED_FORCE_INLINE static void PrefetchProxy( IRenderProxyBase* proxy )
{
	const char* ptr = (const char*)proxy;
	_mm_prefetch( ptr,          _MM_HINT_NTA );
	_mm_prefetch( ptr + 1 * 64, _MM_HINT_NTA );
	_mm_prefetch( ptr + 2 * 64, _MM_HINT_NTA );
	_mm_prefetch( ptr + 3 * 64, _MM_HINT_NTA );
	_mm_prefetch( ptr + 4 * 64, _MM_HINT_NTA );
}

void CRenderCollector::DoCull_Cascades( const SRenderNodeImplContext& rctx, vis::ViewID visViewId, job::Builder& builder )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( DoCull_Cascades );

	if ( !m_scene )
	{
		return;
	}

	{
		// Do we have anything to draw ?
		if ( m_cascades.m_numCascades > 0 )
		{
			RED_FATAL_ASSERT( m_mainCameraQuery, "Main camera query is expected to be initialized at this point" );

			auto& visScene = m_scene->GetVisibilityScene();

			red::AtomicSharedPtr<vis::QueryBase> query;
			{
				RED_ASSERT( m_cascades.m_numCascades <= 4, "Too many cascades: %u", m_cascades.m_numCascades );

				// Below we want to construct four sets of query parameters (up to four - one per cascade)...
				vis::SQueryParams_MultiFrustum queryParams;
				queryParams.m_viewId = visViewId;

				// ... To do that we'll start with cascade0 bit, and perform a bit-shift by one for each subsequent cascade
				ShadowFlags tempShadowFlags; 
				tempShadowFlags.m_cascade0 = true;
				for ( Uint32 i = 0; i < m_cascades.m_numCascades; ++i )
				{
					queryParams.m_cullingPlanes[i] = m_cascades.m_cascades[i].m_cullingPlanes;
					queryParams.m_queryMasks[ i ].m_shadows.m_mask = tempShadowFlags.m_mask << i;
				}
				queryParams.m_numFrustums = m_cascades.m_numCascades;
				queryParams.m_overallCullingPlanes = m_cascades.m_overallCullingPlanes;
				queryParams.m_maxVerticalSize = m_cascades.m_maxVerticalSize;
				// [cull r5] only hand the cascades a rasterizer that was built this frame: GetRasterizer() also returns a
				// previous frame's m_raster when Prepare() skipped the build (empty occluder collection, ?occl=0)
				{
					vis::OcclusionBuffer& cpMainOcc = static_cast<vis::QuerySingleFrustum*> ( m_mainCameraQuery.Get() )->GetOcclusionBuffer();
					queryParams.m_rasterizer = cpMainOcc.IsValid() ? cpMainOcc.GetRasterizer() : nullptr;
				}
				queryParams.m_shadowDir = m_cascades.m_shadowDir;
				queryParams.m_shadowRight = m_cascades.m_shadowRight;
				queryParams.m_shadowUp = m_cascades.m_shadowUp;

#ifndef RED_CONFIGURATION_FINAL
				const renderDev::SConfig* devConfig = GetRenderer()->GetDevConfig();
				const Bool useGlobalOcclusion = !devConfig->CascadesDebug.IsEnabled() || devConfig->CascadesDebug.m_globalOcclusionEnable;
#else
				const Bool useGlobalOcclusion = true;
#endif
				if ( useGlobalOcclusion )
				{
					SceneCustomData_DistantShadows& shadowsData = rctx.GetCameraStorage()->GetCustomData<SceneCustomData_DistantShadows>();
					queryParams.m_globalOcclusionRasterizer = shadowsData.GetGlobalOcclusionRasterizer();

#ifndef RED_CONFIGURATION_FINAL
					queryParams.m_debugSettings.OcclusionBufferRoughWorld = devConfig->CascadesDebug.IsEnabled() && devConfig->CascadesDebug.m_globalOcclusionShowDebugRough;
					queryParams.m_debugSettings.OcclusionBufferFineWorld = devConfig->CascadesDebug.IsEnabled() && devConfig->CascadesDebug.m_globalOcclusionShowDebugFine;
#endif
				}

				// If todvis is enabled mark correct time of the day and force query to take it into account.
				if ( rctx.GetCameraData()->GetInfo().m_applyCascadesTODVis )
				{
					queryParams.m_queryMaskTimeOfDay = ( 1u << TimeOfDayVisibilityResource::HourToBit( m_cascades.m_timeOfDay ) );
				}

				queryParams.m_debugSettings.VisibleTargetBounds = m_cameraInfo->GetRenderingMask().Test( rend::DF_VisCascadeVisibleTargets );
				queryParams.m_debugSettings.QueryBounds = m_cameraInfo->GetRenderingMask().Test( rend::DF_VisCascadeFrustums );

				query = visScene.CreateQuery( queryParams );
			}

			const auto boxVisibilityCallback = [query]( const Box& boundingBox, const EFrustumResult parentFrustumResult )
			{
				return (Uint8)query->GetBoxVisibilityMask( boundingBox, parentFrustumResult );
			};

			const auto traverseCallback = [&rctx, collector = this, query, &builder, &visScene]( auto&& clustersCollected )
			{
				builder.DispatchJob<job::Fence::None>( "Cascades_TestTargetGroups", [rctx, collector, query, &visScene, &cascadesData = collector->m_cascades, clustersCollected = std::move( clustersCollected )]( const job::RunContext& runCtx ) mutable
				{
					vis::QueryResults visResults;

					for ( const vis::visGrid::SGatheredGridCellData& currNode : clustersCollected )
					{
						const Bool doFrustumCheck = currNode.GetFrustumResult() == EFrustumResult::FR_Intersecting;
						query->BVHTestTargets( currNode.GetNode()->m_objects, doFrustumCheck, visResults );
					}

					auto visibleGroups = visResults.GetVisibleTargets();
					if ( visibleGroups.Size() > 0 )
					{
						// Perform collect
						PC_SCOPE( Cascades_CollectElements );

#ifndef RED_CONFIGURATION_FINAL
						{
							// append stats
							RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
							const VisibilitySystemStats& statsToAdd = visResults.GetStats();
							VisibilitySystemStats& stats = rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats;
							stats.Append( statsToAdd );
							stats.m_cascadesNumTestedTargets += statsToAdd.m_numObjectsTested;
							stats.m_cascadesNumPassedTargets += statsToAdd.m_numObjectsVisible;
						}
#endif // RED_CONFIGURATION_FINAL

						auto visibleTargetMasks = visResults.GetVisibleTargetMasks();

						IRenderProxyBase::SUpdateContext updateContext( rctx, *collector );

						constexpr Uint32 ProxyPrefetchLookahead = 2;
						constexpr Uint32 ProxyDataPrefetchLookahead = 1;

						for ( Uint32 i = 0; i < Min( ProxyPrefetchLookahead, visibleGroups.Size() ); i++ )
						{
							PrefetchProxy( static_cast<IRenderProxyCollectable*>( visibleGroups[ i ] ) );
						}

						for ( Uint32 i = 0; i < visibleGroups.Size(); i++ )
						{
							if ( i + ProxyPrefetchLookahead < visibleGroups.Size() )
							{
								PrefetchProxy( static_cast<IRenderProxyCollectable*>( visibleGroups[i + ProxyPrefetchLookahead] ) );
							}

							IRenderProxyDrawable* proxy = static_cast<IRenderProxyCollectable*>( visibleGroups[ i ] )->AsDrawable();
							RED_ASSERT( proxy->CanCastGlobalShadows(), "Proxy should cast global shadows." );
							proxy->CollectCascadeShadowElements( updateContext, cascadesData, visibleTargetMasks[ i ] );
						}
					}

				} );
			};

			VisibilityGridStats visGridStats = m_scene->m_rpMainSceneProxies.TraverseGridAndQueryNodes<1024, 32>( traverseCallback, boxVisibilityCallback );

#ifndef RED_CONFIGURATION_FINAL
			RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
			visGridStats.m_cascadesGridCellsTraversed = visGridStats.m_totalGridCellsTraversed;
			visGridStats.m_cascadesGridCellsPassedVisibility = visGridStats.m_totalGridCellsPassedVisibility;
			visGridStats.m_cascadesGridCellsWithOneElem = visGridStats.m_totalGridCellsWithOneElem;
			rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats.m_visGridStats.Append( visGridStats );
#endif

			builder.DispatchFenceExplicitly();
		}
	}
}

#ifdef USE_RAY_TRACING

void CRenderCollector::DoCull_RayTracedObjects( const SRenderNodeImplContext& rctx, vis::ViewID visViewId, job::Builder& builder )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( DoCull_RayTracedObjects );

	auto framePurpose = m_info->GetFramePurpose();

	if ( !m_scene || !( FramePurpose::IsNormal( framePurpose ) || FramePurpose::IsScreenshot( framePurpose ) ) )
	{
		//RED_LOG_WARNING("Skipping raytracing cull for frame %d", m_info->GetFramePurpose());
		return;
	}

	auto& visScene = m_scene->GetVisibilityScene();

	red::AtomicSharedPtr<vis::QueryBase> query;
	vis::QueryMask queryMask;
	queryMask.m_rayTraceBVH = true;
	queryMask.m_prefabProxy = true;
	Uint32 viewId = visViewId;

	Bool enableReflections = Config::GGlobalRenderingSettings.RTX_EnableReflections || Config::GGlobalRenderingSettings.RTX_EnableTransparentReflections;
	Bool enableDirectionalShadow = Config::GGlobalRenderingSettings.RTX_EnableDirectionalShadow;
#ifndef RED_CONFIGURATION_FINAL
	if ( Config::cvRayTracingOverrideEnable.Get() )
	{
		enableReflections = Config::cvRayTracingEnableReflection.Get() || Config::cvRayTracingEnableTransparentReflection.Get();
		enableDirectionalShadow = Config::cvRayTracingEnableDirectionalShadow.Get();
	}
#endif
	if ( enableReflections || enableDirectionalShadow )
	{
		vis::SQueryParams_RayTracedReflections queryParams;
		queryParams.m_camera.Set( m_camera->GetWorldToView(), m_camera->GetViewToScreenRevProjAware(), Config::cvRayTracingVisibilityFrustumOffset.Get() );
		queryParams.m_cullingRadiusSquared = enableReflections ? powf(Config::cvRayTracingVisibilityCullingRadius.Get(), 2.0f) : 0;
		queryParams.m_enableDirectionalShadow = enableDirectionalShadow && Config::cvRayTracingEnableDirectionalShadowCulling.Get();
		if ( enableDirectionalShadow )
		{
			// #TODO: currently rely on shadow cascades
			queryParams.m_shadowCullingPlanes = m_cascades.m_overallUncappedCullingPlanes;
		}
		queryParams.m_queryMask = queryMask;
		queryParams.m_viewId = viewId;
		query = visScene.CreateQuery( queryParams );
	}
	else
	{
		vis::SQueryParams_SingleFrustum queryParams;
		queryParams.m_camera.Set( m_camera->GetWorldToView(), m_camera->GetViewToScreenRevProjAware(), Config::cvRayTracingVisibilityFrustumOffset.Get() );
		queryParams.m_queryMask = queryMask;
		queryParams.m_viewId = viewId;
		query = visScene.CreateQuery( queryParams );
	}

	// Reset render proxies so that the size is OK.
	auto accelerationStructureManager = GetRenderer()->GetAccelerationStructureManager();
	accelerationStructureManager->HookScene( m_scene );

	auto boxVisibilityCallback = [query]( const Box& boundingBox, const EFrustumResult parentFrustumResult )
	{
		return (Uint8)query->GetBoxVisibilityMask( boundingBox, parentFrustumResult );
	};

	const auto traverseCallback = [&rctx, collector = this, accelerationStructureManager, query, &builder, &visScene]( auto&& clustersCollected )
	{
		builder.DispatchJob<job::Fence::None>( "RayTracedMainSceneObjects_TestTargetGroups", [rctx, accelerationStructureManager, collector, query, &visScene, clustersCollected = std::move( clustersCollected )]( const job::RunContext& runCtx )
		{
			vis::QueryResults visResults;
			for ( const vis::visGrid::SGatheredGridCellData& currNode : clustersCollected )
			{
				const Bool doFrustumCheck = currNode.GetFrustumResult() == EFrustumResult::FR_Intersecting;
				query->BVHTestTargets( currNode.GetNode()->m_objects, doFrustumCheck, visResults );
			}

			const auto& visibleProxies = visResults.GetVisibleTargets();
			if ( visibleProxies.Size() > 0 )
			{
				PC_SCOPE( MainScene_RayTracedObjects_Collect );
				//Process all visible proxies and collect stats
				IRenderProxyBase::SUpdateContext updateContext(rctx, *collector);
				updateContext.isMainCameraView = true;

#ifndef RED_CONFIGURATION_FINAL
				{
					RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
					rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats.Append( visResults.GetStats() );
				}
#endif

				accelerationStructureManager->AddProxies( updateContext, visResults );
			}
		} );
	};

	VisibilityGridStats visGridStats = m_scene->m_rpMainSceneProxies.TraverseGridAndQueryNodes<1024, 48>( traverseCallback, boxVisibilityCallback );

#ifndef RED_CONFIGURATION_FINAL
	{
		RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
		rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats.m_RTXVisGridStats.Append( visGridStats );
	}
#endif

	builder.DispatchFenceExplicitly();
}
#endif

namespace
{
	static RED_INLINE Bool IsLightVisibleInVolumes( Uint16 lightChannels, Uint16 visibleLightVolumes )
	{
		// If the light has no channels assigned or world channel assigned - assume it is visible in all of them.
		// NOTE: By design light channels treat the LC_World as 'visible in everything besides other volumes'. 
		// In terms of local shadows we would like to just collect it if its in player's frustum & not occluded, 
		// but since there are no volumes for LC_World placed on the world we need to treat it in special way.

		if( 0 != (lightChannels & ~rend::LC_AllChannels) || rend::LC_DefaultAll == lightChannels )
		{
			return true;
		}

		return (visibleLightVolumes & lightChannels) != 0;
	}
}

void CRenderCollector::AcquireAndSortShadowCastingLights
( 
	red::DynArray<IRenderProxyLight*>& sortedShadowCastingLights,
	const SRenderNodeImplContext& rctx
)
{
	red::DynArray<IRenderProxyLight*> sortedShadowCastingLightsIntermediate { job::PoolJobScope() };
	red::DynArray<math::Vector4> occluderVertices{ job::PoolJobScope() };

	// Acquire visible light volumes
	Uint16 combinedVolumesMask = 0;
	red::DynArray<const CRenderProxy_LightVolume*> visibleLCs{ job::PoolJobScope() };
	{
		const auto volumes = lightVolume::GetVolumes( *rctx.GetCameraData() , rend::LCG_LightClippingVolume );
		vis::QuerySingleFrustum* singleFrustrumQuery = static_cast< vis::QuerySingleFrustum* > ( m_mainCameraQuery.Get() );
				
		const Vector4 cameraPosition = rctx.GetCameraData()->GetRenderCamera().GetPosition().AsVector4();
		for ( const CRenderProxy_LightVolume* vol : volumes )
		{
			// In case player camera is already in LC, we treat it as always visible.
			if ( vol->ContainsPoint( cameraPosition ) )
			{
				combinedVolumesMask |= vol->GetChannels();
				visibleLCs.PushBack( vol );
				continue;
			}

			// HACK
			const auto vertices = vol->GetVertices();
			const auto indices = vol->GetIndices();
			occluderVertices.Resize( vertices.Size() );
			for ( Uint32 i = 0; i < vertices.Size(); ++i )
			{
				occluderVertices[ i ] = vertices[ i ];
			}

			Uint32 drawFlags = vis::rast::IRasterizer::DRAW_Default | vis::rast::IRasterizer::DRAW_TwoSided | vis::rast::IRasterizer::DRAW_ClipToFrustum;
			auto l2w = vol->GetLocalToWorld();
			if ( FR_Intersecting == singleFrustrumQuery->GetOcclusionBuffer().TestTriangles( occluderVertices.TypedData(), occluderVertices.Size(), indices.Data(), indices.Size(), vol->GetLocalToWorld(), drawFlags, FR_Intersecting ) )
			{
				combinedVolumesMask |= vol->GetChannels();
				visibleLCs.PushBack( vol );
			}
		}
	}

	// Gather shadow casting lights
	{	
		PC_SCOPE( GatherShadowCastingLights );
		const Uint32 lightCount = m_renderCollectorData->GetNumLights();
		for ( Uint32 light_i = 0; light_i < lightCount; ++light_i )
		{
			IRenderProxyLight* light = m_renderCollectorData->m_lights[ light_i ];
			if ( light->UpdateShadowParams( *this ) && light->ShouldPrepareLocalShadowmaps() )
			{
				// If light has no light channels assigned or the default setting, just collect it and continue.
				if ( 0 != ( light->GetChannels() & ~rend::LC_AllChannels ) || rend::LC_DefaultAll == light->GetChannels() )
				{
					sortedShadowCastingLightsIntermediate.PushBack( light );
					continue;
				}

				// 'Broad phase' check if light was set to any of the visible volumes.
				if( ( combinedVolumesMask & light->GetChannels() ) != 0 )
				{
					// 'Narrow phase' check if light resides in at least of one visible volume.
					for ( const CRenderProxy_LightVolume* vol : visibleLCs )
					{
						// NOTE: For now we are using light's position and radius to build a sphere that is used within ContainsPoint. 
						//       So sphere is way bigger then it should be for spot lights for example.
						if ( ( ( light->GetChannels() & vol->GetChannels() ) != 0 ) && vol->ContainsPoint( light->GetFinalPosition(), light->GetShadowRadius() ) )
						{
							sortedShadowCastingLightsIntermediate.PushBack( light );
							break;
						}
					}

				}
			}
		}
	}

	// Sort shadow casting lights based on distance to camera
	{
		PC_SCOPE( SortByDistanceToCamera );
		std::sort( sortedShadowCastingLightsIntermediate.Begin(), sortedShadowCastingLightsIntermediate.End(), [cameraPos = rctx.GetRenderCamera().GetPosition().AsVector4()]( const IRenderProxyLight* a, const IRenderProxyLight* b )
				   {
					   Float distanceA = a->GetFinalPosition().DistanceSquaredTo( cameraPos );
					   Float distanceB = b->GetFinalPosition().DistanceSquaredTo( cameraPos );
					   return distanceA < distanceB;
				   } );
	}

	// Pick proper amount of lights to fit shadow manager's budget
	Uint32 standardLightsCount { 0 };
	Uint32 staticOnlyLightsCount { 0 };
	const Uint32 maxStandardLights = c_staticOnlySlicesStartIndex;
	const Uint32 maxStaticOnlyLights = c_maxShadowSlices - c_staticOnlySlicesStartIndex;
	for ( IRenderProxyLight* rpl : sortedShadowCastingLightsIntermediate )
	{
		if ( rpl->IsCastingLocalShadowsFromStaticsOnly() )
		{
			if ( staticOnlyLightsCount++ < maxStaticOnlyLights )
			{
				sortedShadowCastingLights.PushBack( rpl );
			}
		}
		else
		{
			if ( standardLightsCount++ < maxStandardLights )
			{
				sortedShadowCastingLights.PushBack( rpl );
			}
		}

		// Exit if we maxed out the budget
		if ( standardLightsCount >= maxStandardLights && staticOnlyLightsCount >= maxStaticOnlyLights )
			break;
	}
}

void CRenderCollector::DoCull_LocalShadowMaps( const SRenderNodeImplContext& rctx, vis::ViewID visViewId, job::Builder& builder )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );

	PC_SCOPE( DoCull_LocalShadowMaps );

	if ( m_scene == nullptr || !rctx.Test(RFF_LocalShadows) )
	{
		return;
	}

	const Uint32 lightCount = m_renderCollectorData->GetNumLights();
	if( !lightCount )
	{
		return;
	}

#ifndef RED_CONFIGURATION_FINAL
	//
	//	Process freezing shadowmaps.
	//
	if ( rctx.GetCameraInfo().m_freezeShadowmaps )
	{
		SceneCustomData_ShadowManager& shadowManager = m_scene->GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
		auto& visScene = m_scene->GetVisibilityScene();
		const auto& camFrameInfo = rctx.GetCameraInfo();
		shadowManager.DebugProcessFreezeShadowmaps( m_localShadowsCollector.m_localShadowQueries, visViewId, visScene, camFrameInfo.m_shadowSliceShowVisibleTargetsMask, camFrameInfo.m_showLightsWithShadowmapsFrusta );
		CullAndCollectLocalShadowMaps( rctx, builder );
		return;
	}
#endif

	{
		PC_SCOPE( CreateNewQueries );

		auto& visScene = m_scene->GetVisibilityScene();
		SceneCustomData_ShadowManager& shadowManager = m_scene->GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
		const Uint8 perFrameSlices = Config::GGlobalRenderingSettings.LocalShadowsProcessedPerFrame;
		
		// Get up tp first c_maxDynamicSlices lights closest to the camera & register them in shadow manager
		red::DynArray<IRenderProxyLight*> sortedShadowCastingLights { rend::PoolLighting() };
		sortedShadowCastingLights.Reserve( c_maxShadowSlices );
		AcquireAndSortShadowCastingLights( sortedShadowCastingLights, rctx );
		{
			// NOTE: Maybe it would be nice to see which types of lights actually exceeded the budget.
#ifndef RED_CONFIGURATION_FINAL
			s_wasLocalShadowsOverflow.SetValue( sortedShadowCastingLights.Size() > c_maxShadowSlices );
#endif // RED_CONFIGURATION_FINAL
			shadowManager.RegisterLightsForCurrentFrame( *this, sortedShadowCastingLights );

#ifndef RED_CONFIGURATION_FINAL
			s_wasLocalShadowsQueueOverflow.SetValue( shadowManager.GetShadowsQueueSize() > perFrameSlices );
#endif // RED_CONFIGURATION_FINAL
		}
		
		for ( Uint8 i = 0; i < perFrameSlices; i++ )
		{
			// All lights on the queue always have assigned shadow slices and can be scheduled for vis query.
			IRenderProxyLight* rpl = shadowManager.PopFirstShadowLight();

			if ( !rpl )	break;

			Bool showVisibleTargetsDebug = false;
			Bool showFrusta = false;
#ifndef RED_CONFIGURATION_FINAL
			const ShadowSlice* lightSS = shadowManager.GetLightShadowSlice( rpl );
			RED_ASSERT( lightSS );
			showVisibleTargetsDebug = ( ( rctx.GetCameraInfo().m_shadowSliceShowVisibleTargetsMask & RED_FLAG( lightSS->m_index ) ) != 0 );
			showFrusta = rctx.GetCameraInfo().m_showLightsWithShadowmapsFrusta;
#endif
			red::AtomicSharedPtr<vis::QueryBase> query = rpl->CreateShadowmapQuery( visViewId, visScene, showFrusta );
			if ( query == nullptr )
			{
				RED_HALT( "Failed to create shadowmap query!" );
				continue;
			}

			LocalShadowQueryInfo q;
			q.m_visQuery = query;
			q.m_light = rpl;

#ifndef RED_CONFIGURATION_FINAL
			q.m_showdebugBBoxes = showVisibleTargetsDebug;
			m_renderingStats.m_lightingStats.m_numOfLocalShadowsVisibilityQueries++;
#endif
			m_localShadowsCollector.m_localShadowQueries.PushBack( q );

		}
	}

	CullAndCollectLocalShadowMaps( rctx, builder );
}

void CRenderCollector::DoCull_QueryOnlyBoxes( const SRenderNodeImplContext& rctx, job::Builder& builder )
{
	const Float distanceMultiplier = ( ( m_camera->GetFOV() > 0.0f ) ? m_camera->GetFOVMultiplier() : m_camera->GetZoom() );
	m_scene->m_queryOnlyBoxes.FetchResults( &m_scene->GetVisibilityScene(), m_mainCameraQuery, m_camera->GetPosition().AsVector4(), distanceMultiplier, builder );
}

Bool CRenderCollector::HasChunks( red::StringView stageName ) const
{
	const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( stageName );

	return m_renderStageCollector.GetBucket( stageId ).HasChunks();
}

void CRenderCollector::CullAndCollectLocalShadowMaps( const SRenderNodeImplContext& rctx,  job::Builder& builder )
{
	PC_SCOPE(CollectLocalLightsStatic);

	const Uint32 count = m_localShadowsCollector.m_localShadowQueries.Size();
	if ( count > 0 )
	{
		builder.DispatchParallelForJob( "LocalShadows_CullAndCollect_PerLight", { count }, [collector = this, rctx ]( Uint32 lightIdx, const job::RunContext& context )
		{
			LocalShadowQueryInfo & query	= collector->m_localShadowsCollector.m_localShadowQueries[ lightIdx ];
			IRenderProxyLight* light		= query.m_light;

			if( light->ShouldPrepareLocalShadowmaps() )
			{
				job::Builder depBuilder { context };

				// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -
				// Test proxies against the light camera.
				// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -
				{
					// No need to dispatch dynamic & static vis queries if light does not need them both. 
					SceneCustomData_ShadowManager* shadowManager = &collector->m_scene->GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
					const ShadowSlice* shadowSlice = shadowManager->GetLightShadowSlice( light );
					RED_ASSERT( shadowSlice, "Light scheduled for CullLocalShadowMaps must have valid slice." );
					if ( shadowSlice == nullptr )
					{
						return;
					}
					const Uint32 shadowSliceIndex = shadowSlice->m_index;

					// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -
					// Collect static and dynamic objects for given light. Cluster ID is matched with shadow slice ID.
					// - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -  - - - - - - - - -

					const Bool shouldUpdateDynamics = shadowSlice->m_shouldUpdateDynamics.GetValue();
					const Bool shouldUpdateStatics = shadowSlice->m_shouldUpdateStatics.GetValue();
					if ( shouldUpdateStatics || shouldUpdateDynamics )
					{
						PC_SCOPE( LocalShadows_TestTargetGroups );

						auto& visResults = query.m_visProxiesResults;
						visResults.Reserve<vis::QueryResultsVisMaskInclusion::VMI_NO>( vis::c_queryVisibleProxiesPreallocation );

						const auto boxVisibilityCallback = [ &visQuery = query.m_visQuery]( const Box& boundingBox, const EFrustumResult parentFrustumResult )
						{
							return (Uint8)visQuery->GetBoxVisibilityMask( boundingBox, parentFrustumResult );
						};

						const auto traverseCallback = [&rctx, collector, &query, &visResults, shadowSliceIndex, shouldUpdateStatics, shouldUpdateDynamics]( auto&& clustersCollected )
						{
							for ( const auto& currNode : clustersCollected )
							{
								const Bool doFrustumCheck = currNode.GetFrustumResult() == EFrustumResult::FR_Intersecting;
								query.m_visQuery->BVHTestTargets( currNode.GetNode()->m_objects, doFrustumCheck, visResults );
							}

							const auto& visibleProxies = visResults.GetVisibleTargets();
							if ( visibleProxies.Size() > 0 )
							{
								PC_SCOPE( LocalShadows_Collect );

#ifndef RED_CONFIGURATION_FINAL
								{
									// append stats
									RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
									VisibilitySystemStats& collectorStats = rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats;
									auto& visStats = visResults.GetStats();
									collectorStats.Append( visStats );
									collectorStats.m_localShadowsNumTestedTargets += visStats.m_numObjectsTested;
									collectorStats.m_localShadowsNumPassedTargets += visStats.m_numObjectsVisible;
								}
#endif // RED_CONFIGURATION_FINAL


								IRenderProxyBase::SUpdateContext updateContext( rctx, *collector );
								const SRenderStageCluster clusterID = SRenderStageCluster::Init( shadowSliceIndex );

#ifndef RED_CONFIGURATION_FINAL
								vis::DebugDrawBuffer debugDrawBuffer;
								const Bool debugBBoxesRequested		= query.m_showdebugBBoxes;
								const Bool staticBBoxesRequested	= debugBBoxesRequested && shouldUpdateStatics;
								const Bool dynamicBBoxesRequested	= debugBBoxesRequested && shouldUpdateDynamics;
#endif // RED_CONFIGURATION_FINAL

								for ( vis::MainSceneAccelerationStructure::IBVHUserData* proxyCollectable : visibleProxies )
								{
									CRenderProxy_Mesh* rpm = static_cast<CRenderProxy_Mesh*>( proxyCollectable );
									const Bool isDynamic = rpm->IsDynamic();
									if ( ( shouldUpdateDynamics && isDynamic ) || ( shouldUpdateStatics && !isDynamic ) )
									{
										RED_ASSERT( rpm );
										RED_ASSERT( rpm->CanCastLocalShadows() );

										// After frustum test we do additional one which takes into account light's shadow radius.
										// Reason for that are lights with huge outer angles that end up spawning enormous frusta
										// even with small radii.
										{
											const Vector4 lightPos			= query.m_light->GetFinalPosition();
											const Float lightShadowRadius	= query.m_light->GetShadowRadius();
											const Box& rpmBBox				= rpm->GetBoundingBox();

											if ( !LightProxyHelpers::CalcLightVsBoxIntersectionApproxBySphereVsSphereTest( lightPos, lightShadowRadius, rpmBBox ) )
											{
												continue;
											}
										}
#ifdef RED_CONFIGURATION_FINAL
										rpm->CollectLocalShadowElements( updateContext, isDynamic, clusterID );
#else
										if ( rpm->CollectLocalShadowElements( updateContext, isDynamic, clusterID ) )
										{
											// NOTE: local shadows debug boxes were exposed here cause query base has no notion of proxy type
											//       and it is crucial for this debug view to display boxes only for things rendered to shadowmap
											//       not everything collected there.
											if ( ( staticBBoxesRequested && !isDynamic ) || ( dynamicBBoxesRequested && isDynamic ) )
											{
												debugDrawBuffer.AddLocalShadowsVisibleBBox( rpm->GetBoundingBox() );
											}
										}
#endif
										query.m_numStaticsCollected += ( !isDynamic );
										query.m_numDynamicsCollected += ( isDynamic );
									}
								}

#ifndef RED_CONFIGURATION_FINAL
								query.m_visQuery->PushDebugDrawBuffer( debugDrawBuffer );
#endif // RED_CONFIGURATION_FINAL

								visResults.ClearVisibleTargetsAndStats<vis::QueryResultsVisMaskInclusion::VMI_NO>();
							}
						};

						VisibilityGridStats visGridStats = collector->m_scene->m_rpMainSceneProxies.TraverseGridAndQueryNodesInBox<1024, 4, true>( Box( light->GetFinalPosition(), light->GetShadowRadius() ), traverseCallback, boxVisibilityCallback );

#ifndef RED_CONFIGURATION_FINAL
						RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
						visGridStats.m_localShadowsGridCellsTraversed = visGridStats.m_totalGridCellsTraversed;
						visGridStats.m_localShadowsGridCellsPassedVisibility = visGridStats.m_totalGridCellsPassedVisibility;
						visGridStats.m_localShadowsGridCellsWithOneElem = visGridStats.m_totalGridCellsWithOneElem;
						rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats.m_visGridStats.Append( visGridStats );
#endif

					}
				}
			}
		} );
	}
}

CRenderCollector::CRenderCollectorData::CRenderCollectorData()
	: m_visibleCpuParticlesMask( rend::PoolParticleData() )
{
}

void CRenderCollector::CRenderCollectorData::Reset()
{
	m_numLights.SetValue(0);

	m_numLightBlockers.SetValue(0);
}

CRenderCollector::CRenderCollectorData::~CRenderCollectorData()
{
}

Bool CRenderCollector::CRenderCollectorData::AddLight( IRenderProxyLight *light )
{
	const Uint32 idx = m_numLights.PostIncrement();

	if ( idx < LIGHTS_CAPACITY )
	{
		m_lights[ idx ] = light;
		return true;
	}

#ifndef RED_CONFIGURATION_FINAL
	s_wasLightAllocationOverflow.SetValue( true );
#endif // RED_CONFIGURATION_FINAL
	return false;
}

Bool CRenderCollector::CRenderCollectorData::AddLightBlocker( CRenderProxy_LightBlocker* lightBlocker )
{
	const Uint32 idx = m_numLightBlockers.PostIncrement();
	if ( idx < LIGHT_BLOCKERS_CAPACITY )
	{
		m_lightBlockers[ idx ] = lightBlocker;
		return true;
	}
	return false;
}

void CRenderCollector::CRenderCollectorData::SortLightsByType()
{
	struct
	{
		RED_FORCE_INLINE Bool operator()( const IRenderProxyLight* a, const IRenderProxyLight* b ) const
		{
			return a->GetType() < b->GetType();
		}
	} cmp;

	std::sort( m_lights, m_lights + GetNumLights(), cmp );
}

void CRenderCollector::ProcessAutoHideProxy( IRenderProxyBase* proxy )
{
	if ( IRenderProxyDrawable* drawable = proxy->AsDrawable() )
	{
		RED_FATAL( "UNIMPLEMENTED" );
		// drawable->OnNotVisibleFromAutoHide( *this );
	}
}

void CRenderCollector::CpuParticleVisibilityMark( Uint16 index )
{
	m_renderCollectorData->CpuParticleVisibilityMark( index );
}

Bool CRenderCollector::CheckParticleVisibility()
{
	return m_renderCollectorData->m_visibleCpuParticlesMask.Size() > 0;
}

Bool CRenderCollector::CMirrorDebugCollector::PopCamera( CRenderCamera& rCam )
{
	RED_SCOPE_LOCK( m_mirrorCamerasMutex );
	if ( !m_mirrorCameras.Empty() )
	{
		rCam = m_mirrorCameras.Pop();
		return true;
	}
	return false;
}

Bool CRenderCollector::CMirrorDebugCollector::PopPlane( Matrix& rPlane )
{
	RED_SCOPE_LOCK( m_mirrorPlanesMutex );
	if ( !m_mirrorPlanes.Empty() )
	{
		rPlane = m_mirrorPlanes.Pop();
		return true;
	}
	return false;
}

Bool CRenderCollector::CMirrorDebugCollector::PopLocationMarker( Matrix& location )
{
	RED_SCOPE_LOCK( m_mirrorPositionsMutex );
	if ( !m_mirrorPositions.Empty() )
	{
		location = m_mirrorPositions.Pop();
		return true;
	}
	return false;
}

void CRenderCollector::CMirrorDebugCollector::PushCamera( const CRenderCamera& rCam )
{
	RED_SCOPE_LOCK( m_mirrorCamerasMutex );
	m_mirrorCameras.Push( rCam );
}

void CRenderCollector::CMirrorDebugCollector::PushPlane( const Matrix& rPlane )
{
	RED_SCOPE_LOCK( m_mirrorPlanesMutex );
	m_mirrorPlanes.Push( rPlane );
}

void CRenderCollector::CMirrorDebugCollector::PushLocationMarker( const Matrix& location )
{
	RED_SCOPE_LOCK( m_mirrorPositionsMutex );
	m_mirrorPositions.Push( location );
}