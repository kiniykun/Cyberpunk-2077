/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <mutex> // [skperf] round 52

#include "renderInterface.h"
#include "renderGraphNodes.h"
#include "renderGeometryBatcher.h"
#include "renderShadowManager.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "renderProxyAutoSpawner.h"
#include "clusteredDeferred.h"
#include "renderVolumetricClouds.h"
#include "renderDistantShadows.h"
#include "../../../../engine/shaders/include/globalConstantsCS.fx"
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace Config
{
	TConfigVar<Bool>			cvCascadeEnableScissors( "Rendering/Shadows", "CascadeEnableScissors", true );
}

// [skperf] round 52 cascSplit (opt-in ?cascSplit=N, N = 2..4; default 1 = this function unchanged): in the bar the sun
// cascades are the longest serial recordings of an ordinary frame (tc67 timeline: RenderCascade0 8.8-11.2 ms and
// RenderCascade1 8.5-9.5 ms, each on one worker, on the chain in 116 of 120 short frames). renderRenderFrame.cpp then
// builds N command lists per cascade (RenderCascadeC, RenderCascadeC_s1, ...) with CpRenderNode_RenderShadowCascadeSlice,
// which sets the slice (k of N) for this call only (same thread, same call stack). Slice k draws the k-th contiguous
// part of the sorted chunks of both buckets (renderGeometryBatcher.cpp reSplit slicing, keyed by command list id);
// only slice 0 clears the cascade and renders terrain auto-spawn casters; the collector buckets are cleared by whichever
// slice finishes last. The lists are submitted in creation order, so slice 0's clear precedes the other slices' draws,
// and depth-only rendering of the same set of chunks gives the same shadow map.
void CpReSplitArm( Int32 k, Int32 n, Uint32 listId ); // renderGeometryBatcher.cpp
namespace CpCascSplit
{
	thread_local Int32 tl_k = 0, tl_n = 1;
	struct Done { const void* collector = nullptr; Uint32 cascade = 0; Int32 count = 0; };
	Done s_done[ 16 ];
	std::mutex s_lock;
	// true for the slice that completes the set (n slices of this collector's cascade)
	Bool Finish( const void* collector, Uint32 cascade, Int32 n )
	{
		std::lock_guard<std::mutex> guard( s_lock );
		Done* slot = nullptr;
		for ( Done& d : s_done ) if ( d.collector == collector && d.cascade == cascade && d.count > 0 ) { slot = &d; break; }
		if ( !slot ) for ( Done& d : s_done ) if ( d.count == 0 ) { slot = &d; break; }
		if ( !slot ) return true; // table full (cannot happen with <= 4 cameras x 4 cascades): clear now, as the unsplit path
		slot->collector = collector; slot->cascade = cascade;
		if ( ++slot->count >= n ) { slot->count = 0; return true; }
		return false;
	}
}
void CpCascadeSliceSet( Int32 k, Int32 n ) { CpCascSplit::tl_k = k; CpCascSplit::tl_n = n; }

void CRenderNode_RenderShadowCascade::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( CRenderNode_RenderShadowCascade );

	RED_FATAL_ASSERT( m_shadowCascadeIndex >= 0 && m_shadowCascadeIndex < MAX_CASCADES, "Invalid context" );

	const CRenderFrameInfo &info = rctx.GetFrameInfo();
	const CRenderCamera &camera = rctx.GetRenderCamera();
	CRenderSceneEx *scene = rctx.GetScene();

	auto meshBatcher	= GetRenderer()->GetGeometryBatcher();

	if( !scene )
	{
		return;
	}

	// Check if requested cascade needs to be rendered. 
	if ( m_shadowCascadeIndex >= CRenderInterface::GetCascadesNumInFrustrum( camera, info ) )
	{
		return;
	}

	const auto rtCascades = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "cascades" ) );

	// Check if current camera requires cascades.
	if ( !rctx.Test( RFF_CascadeShadows ) )
	{
		// Clear cascade texture slice to ensure that nothing was carried over from last frame.
		// Cascade slice could be still used in the rest of the frame as normal. Bit hacky.
		if ( rctx.IsConsumePhase() && CpCascSplit::tl_k == 0 ) // [skperf] round 52: only slice 0 clears
		{
			GpuApi::ClearDepthStencilTarget( rtCascades, 1.0f, 0, m_shadowCascadeIndex );
		}

		return;
	}
	else if ( rctx.IsConsumePhase() ) // Draw
	{
		PC_SCOPE( RenderCascade );
		const Int32 cpK = CpCascSplit::tl_k, cpN = CpCascSplit::tl_n; // [skperf] round 52 cascSplit (1 = unsplit)

		const Uint32 cascadesResolution = Config::GetCascadedShadowsResolution();
		RED_ASSERT( cascadesResolution > 0 );

		const CRenderCameraFrameInfo& cameraInfo = rctx.GetCameraData()->GetInfo();
		CRenderCollector* collector = rctx.GetCollector();
		const SShadowCascade& cascade = collector->m_cascades.m_cascades[m_shadowCascadeIndex];

#ifdef USE_RAY_TRACING
		if (m_shadowCascadeIndex >= collector->m_cascades.m_numCascades)
		{
			return;
		}
#endif

		GpuApi::SetDepthClip( false );

		const auto& shadowCascadeCfg = info.m_worldRenderSettings.m_shadows.m_cascade[m_shadowCascadeIndex];

		if ( cpK != 0 )
		{
			// [skperf] round 52: slices 1..N-1 draw on top of slice 0's (earlier submitted) clear
		}
		else if ( cameraInfo.GetRenderingMask().Test( rend::DF_GlobalShadows_Debug ) || !Config::cvCascadeEnableScissors.Get() )
		{
			// clear the whole texture when debug preview is enabled (for better visibility)
			GpuApi::ClearDepthTarget( rtCascades, 1.0f, m_shadowCascadeIndex );
		}
		else
		{
			GpuApi::ClearDepthTarget( rtCascades, 1.0f, m_shadowCascadeIndex, &cascade.m_clippingRect );
		}

		rctx.BindPSO( NullColorTarget(), DepthTarget( rtCascades, m_shadowCascadeIndex ) );
		rctx.SetViewport( GpuApi::ViewportDesc( cascadesResolution, cascadesResolution ) );

		if ( Config::cvCascadeEnableScissors.Get() )
		{
			// enable scissor test based on main camera frustum slice coverage on the shadow map
			GpuApi::SetScissors( cascade.m_clippingRect );
		}

		// Calculate biased camera
		CRenderCamera biasedCamera = cascade.m_camera;
		{
			const Float biasValue = shadowCascadeCfg.m_biasOffset;
			biasedCamera.SetNearPlane( biasedCamera.GetNearPlane() - biasValue );
			biasedCamera.SetFarPlane( biasedCamera.GetFarPlane() - biasValue );
			biasedCamera.CalculateMatrices();
		}

		// Setup global camera parameters
		RenderBindManager::SetCamera(rctx, biasedCamera, cascadesResolution, cascadesResolution, nullptr );

		GeometryBatcherStats localStats;
		CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );
		if ( cpN > 1 && rctx.HasCommandList() ) CpReSplitArm( cpK, cpN, rctx.GetCommandList().ID() ); // [skperf] round 52: slice both buckets
		Bool cpClearSolid = false, cpClearDiscarded = false;

		// Render solid meshes
		{
			const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_regular" );
			const RenderStageCollectorBucket& bucket = collector->m_renderStageCollector.GetBucket( stageId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
			{
				PC_SCOPE( SolidMeshes );
				meshBatcher->RenderGeometry( renderCtx, bucket );
			}
#ifndef RED_CONFIGURATION_FINAL
			// NOTE: we clear collector for cascades in a twin call in CRenderNode_RenderCascadesShadowMapsDebug::Execute in case the debug is on.
			if ( !cameraInfo.GetRenderingMask().Test( rend::DF_CascadesShadows_DebugMeshesInMainView ) )
#endif
			{
				if ( cpN > 1 ) cpClearSolid = true; // [skperf] round 52: cleared below by the last slice
				else
				{
				PC_SCOPE( ClearCollector_SolidMeshes );
				collector->m_renderStageCollector.Clear( stageId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
				}
			}
		}

		// Render elements with discards
		{
			const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_discarded" );
			const RenderStageCollectorBucket& bucket = collector->m_renderStageCollector.GetBucket( stageId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
			{
				PC_SCOPE( DissolvedMeshes );
				meshBatcher->RenderGeometry( renderCtx, bucket );
			}
#ifndef RED_CONFIGURATION_FINAL
			// NOTE: we clear collector for cascades in a twin call in CRenderNode_RenderCascadesShadowMapsDebug::Execute in case the debug is on.
			if ( !cameraInfo.GetRenderingMask().Test( rend::DF_CascadesShadows_DebugMeshesInMainView ) )
#endif
			{
				if ( cpN > 1 ) cpClearDiscarded = true; // [skperf] round 52: cleared below by the last slice
				else
				{
				PC_SCOPE( ClearCollector_DissolvedMeshes );
				collector->m_renderStageCollector.Clear( stageId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
				}
			}
		}
		if ( cpN > 1 ) // [skperf] round 52 cascSplit: disarm, then the slice that finishes last clears both buckets
		{
			CpReSplitArm( 0, 1, 0 );
			if ( CpCascSplit::Finish( collector, m_shadowCascadeIndex, cpN ) )
			{
				const RenderStageId solidId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_regular" );
				const RenderStageId discardedId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_discarded" );
				if ( cpClearSolid ) collector->m_renderStageCollector.Clear( solidId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
				if ( cpClearDiscarded ) collector->m_renderStageCollector.Clear( discardedId, SRenderStageCluster::Init( m_shadowCascadeIndex ) );
			}
		}

		//Render autoSpawn meshes into first 2 cascades
		const Bool allowAutoSpawnShadows = Config::GGlobalRenderingSettings.allowAutoSpawnSystemShadows;
		if( allowAutoSpawnShadows && m_shadowCascadeIndex <= 1 && cpK == 0 ) // [skperf] round 52: auto-spawn casters in slice 0 only
		{
			CRenderTerrainCellCollector& terrainCellsCollector = rctx.GetCameraStorage()->GetCustomData<CRenderTerrainCellCollector>();
			
			if ( terrainCellsCollector.ShouldRenderAutoSpawnMeshes( rctx ) )
			{
				FeatureFlagsMask featureFlags;
				featureFlags.Set( FEATFLAG_Shadows );
				terrainCellsCollector.RenderAutoSpawnMeshes( rctx, featureFlags, m_shadowCascadeIndex );
			}
		}

		GpuApi::SetDepthClip( true );

#ifndef RED_CONFIGURATION_FINAL
		// Update scene stats
		if( m_shadowCascadeIndex < RenderingStats::NUM_CASCADES )
		{
			RED_SCOPE_LOCK( collector->m_renderingStatsLock );
			collector->m_renderingStats.m_batcherStatsCascades[ m_shadowCascadeIndex ].Append( localStats );
		}
		else
		{
			RED_FATAL( "Insuficient cascaded capacity. Num of cascaded: %u, max: %u", m_shadowCascadeIndex, RenderingStats::NUM_CASCADES );
		}
#endif
	}
}

#ifndef RED_CONFIGURATION_FINAL
void CRenderNode_RenderCascadesShadowMapsDebug::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const auto& cameraInfo = rctx.GetCameraData()->GetInfo();
	if ( cameraInfo.GetRenderingMask().Test( rend::DF_CascadesShadows_DebugMeshesInMainView ) )	// can crash due to debug filter not being synced with renderer.
	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_format = GpuWrapApi::TEXFMT_D32F;
		rctx.RTAlloc( RENDER_NAME_TAG( "cascades_shadows_debug_rendered_meshes" ), desc );

		auto rtSceneDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
		auto rtColor = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );
		auto rtCascadesShadowsDebugRenderedMeshesDepth = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "cascades_shadows_debug_rendered_meshes" ) );

		if ( rctx.IsConsumePhase() )
		{
			//
			// Render to things from cascades collector into rtCascadesShadowsDebugRenderedMeshesDepth.
			//
			{
				rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );
				rctx.BindPSO( DepthTarget( rtCascadesShadowsDebugRenderedMeshesDepth ) );
				GpuApi::ClearDepthTarget( rtCascadesShadowsDebugRenderedMeshesDepth, 1.0f );

				auto meshBatcher = GetRenderer()->GetGeometryBatcher();
				GeometryBatcherStats localStats;
				CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );
				CRenderCollector* collector = rctx.GetCollector();

				CRenderCamera renderCamera = rctx.GetRenderCamera( RPl_Scene );
				renderCamera.SetReversedProjection( false );
				RenderBindManager::SetCamera( rctx, renderCamera, rctx.GetWidth(), rctx.GetHeight(), nullptr );

				for ( Uint32 cascadeIndex = 0; cascadeIndex < Config::GGlobalRenderingSettings.NumShadowCascades; cascadeIndex++ )
				{
					// Render solid meshes
					{
						const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_regular" );
						const RenderStageCollectorBucket& bucket = collector->m_renderStageCollector.GetBucket( stageId, SRenderStageCluster::Init( cascadeIndex ) );
						{
							PC_SCOPE( SolidMeshes );
							meshBatcher->RenderGeometry( renderCtx, bucket );
						}
						{
							// NOTE: in case the debug view is on we disable twin call in CRenderNode_RenderShadowCascade::Execute.
							PC_SCOPE( ClearCollector_SolidMeshes );
							collector->m_renderStageCollector.Clear( stageId, SRenderStageCluster::Init( cascadeIndex ) );
						}
					}

					// Render elements with discards
					{
						const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_cascade_discarded" );
						const RenderStageCollectorBucket& bucket = collector->m_renderStageCollector.GetBucket( stageId, SRenderStageCluster::Init( cascadeIndex ) );
						{
							PC_SCOPE( DissolvedMeshes );
							meshBatcher->RenderGeometry( renderCtx, bucket );
						}
						{
							// NOTE: in case the debug view is on we disable twin call in CRenderNode_RenderShadowCascade::Execute.
							PC_SCOPE( ClearCollector_DissolvedMeshes );
							collector->m_renderStageCollector.Clear( stageId, SRenderStageCluster::Init( cascadeIndex ) );
						}
					}
				}
			}

			//
			// Merge intermediate depth buffer with main scene.
			//
			{
				if ( rctx.IsConsumePhase() )
				{
					rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );
					//rctx.BindPSO( ColorTarget( 0, rtColor ) );
					rctx.BindTexture<0, eCOMPUTE>( rtSceneDepth );
					rctx.BindTexture<1, eCOMPUTE>( rtCascadesShadowsDebugRenderedMeshesDepth );
					rctx.BindTextureUAV< 0 >( rtColor );

					CRenderShaderCompute* shaderCascadesShadowsRenderedMeshes = GetRenderer()->GetShader( SHADER_NAME( "m_shaderCascadesShadowsRenderedMeshes" ) )->AsCompute();
					RED_FATAL_ASSERT( shaderCascadesShadowsRenderedMeshes, "m_shaderCascadesShadowsRenderedMeshes missing" );

					shaderCascadesShadowsRenderedMeshes->Dispatch( red::DivideRoundingUp( rctx.GetWidth(), DEFAULT_THREAD_GROUP_SIZE ), red::DivideRoundingUp( rctx.GetHeight(), DEFAULT_THREAD_GROUP_SIZE ) );

					rctx.Unbind();
				}
			}
		}
	}

}
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderLocalShadowMaps::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( !rctx.Test( RFF_LocalShadows ) )
	{
		return;
	}

	auto sharedDepth = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "localShadowsSharedDepth" ) );
	auto sharedVSMIntermediate = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "localShadowsSharedVSMIntermediate" ) );

	if ( rctx.IsConsumePhase() )
	{
		CRenderCollector& collector = *rctx.GetCollector();
		PC_SCOPE( RenderLocalLightsShadows );

		SceneCustomData_ShadowManager* shadowManager = &rctx.GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
		const auto& visQueryResults = rctx.GetCollector()->m_localShadowsCollector.m_localShadowQueries;

		{
			/// Do the clear only once, on first job no matter what is on shadows queue.
			if ( m_localShadowQueryResultsIdx == 0 )
			{
				shadowManager->ClearColorTargetsOfUnusedSlices( rctx );

#ifndef RED_CONFIGURATION_FINAL
				shadowManager->DebugUpdateInfoAboutCachedTris( rctx );
#endif
			}

			/// Early exit if local shadows queue collected less requests than scheduled jobs.
			if ( ( m_localShadowQueryResultsIdx + 1 ) > visQueryResults.Size() )
			{
				return;
			}

			/// Render slice for given query result.
			{
				shadowManager->RenderShadowSlice( rctx, visQueryResults[ m_localShadowQueryResultsIdx ], sharedDepth, sharedVSMIntermediate );
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef RED_CONFIGURATION_FINAL
void CRenderNode_RenderLocalShadowMapsDebug::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( !rctx.Test( RFF_LocalShadows ) )
	{
		return;
	}

	PC_SCOPE( RenderLocalLightsShadows_Debug );

	SceneCustomData_ShadowManager* shadowManager = &rctx.GetCameraStorage()->GetCustomData<SceneCustomData_ShadowManager>();
	RED_ASSERT( shadowManager, "Shadow manager missing." );
	
	// Were the debug filters thread safe? If not probably DF_LocalShadows_Debug and DF_LocalShadows_DebugMeshesInMainView
	// should be quired and saved during frame info creation.

	const auto& cameraInfo = rctx.GetCameraData()->GetInfo();

	if ( cameraInfo.GetRenderingMask().Test( rend::DF_LocalShadows_DebugMeshesInMainView ) )
	{
		shadowManager->RenderLocalShadowMeshesPreview( rctx );
	}

	if ( cameraInfo.GetRenderingMask().Test( rend::DF_LocalShadows_Debug ) )
	{
		shadowManager->RenderLocalShadowSlicesDebugPreview( rctx );
	}
}
#endif
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderShadowmask::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	if( ! ( rctx.Test( RFF_CascadeShadows ) || rctx.Test( RFF_DistantShadows ) || rctx.Test( RFF_ContactShadows_Global ) || rctx.Test( RFF_ContactShadows_Local ) ) )
	{
		return;
	}

	auto rtShadowmask					= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "shadowmask" ) );
	auto rtDepth						= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDepthBufferWeaponCorrected	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );
	auto rtGbuffer2						= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "gbuffer2" ) );
	auto rtNormals						= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "normals" ) ); 


	CClusteredDeferred::BindingSlots slots;
	slots.headSlot = 3;
	slots.listSlot = 4;
	slots.listAsRW = false;
	slots.lightData = 5;

	GetRenderer()->GetClustered()->BindCommon( rctx, GpuWrapApi::PixelShader, slots );

	if( rctx.IsConsumePhase() )
	{
		static_assert( 0 == GLOBAL_SHADOW_BUFFER_CHANNEL_SHADOW, "Red channel" ); 
		rctx.BindPSO( ColorTarget( 0, rtShadowmask ) );
		rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

		RenderBindManager::SetCamera(rctx, rctx.GetRenderCamera(), rctx.GetWidth(), rctx.GetHeight(), nullptr, false );

		rctx.BindTexture<0, ePIXEL>( rtDepthBufferWeaponCorrected );
		rctx.BindTextureStencil<1, ePIXEL>( rtDepth );

		rctx.BindTexture<2, ePIXEL>( rtGbuffer2 );
		rctx.BindTexture<6, ePIXEL>( rtNormals );

		CRenderCollector& collector = *rctx.GetCollector();

		// Cascades
		{
#ifndef RED_CONFIGURATION_FINAL
			// #ToDo: More rendering cascades debug somewhere else
			const auto& cameraInfo = rctx.GetCameraData()->GetInfo();
			if( cameraInfo.GetRenderingMask().Test( rend::DF_GlobalShadows_Debug ) )
			{
				for( Uint32 i = 0; i < collector.m_cascades.m_numCascades; ++i )
				{
					GetRenderer()->PushTexturePreview( SDynamicPreviewTexture( rctx.RTGet<GpuApi::TextureRef>(RENDER_NAME_TAG( "cascades" )) ).Slice( i ) );
				}
			}
#endif
		}

		const CRenderFrameInfo& frameInfo = rctx.GetFrameInfo();

		struct CB
		{
			CBContactShadowsSettings	contactShadows;
			Float						cascadeShadowReceiverBiasAlongNormal;
		} cbData;

		cbData.contactShadows.Init( frameInfo.m_worldRenderSettings.m_contactShadows );
		cbData.cascadeShadowReceiverBiasAlongNormal = Config::cvCascadeShadowReceiverBiasAlongNormal.Get();

		// If we have TXAA enabled, also enable shadow jitter. This create noise, but it gets smoothed over.
		if ( rctx.Test( RFF_AntialiasingTXAA ) || rctx.Test( RFF_DLSS ) || rctx.GetFrameInfo().GetFramePurpose() == FP_ScreenShot )
		{
			cbData.contactShadows.jitterStrength = 1.0f;
		}

		rctx.SetConstant<ePIXEL>( cbData );

		if ( rctx.Test( rend::DF_DistantShadowsPreview ) )
		{
			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadow_DistantPreview" ) );
		}
		else if ( rctx.Test( rend::DF_GlobalShadowsContactPreview ) )
		{
			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadow_ContactPreview" ) );
		}
		else if ( rctx.Test( rend::DF_GlobalShadowsContactAreaPreview ) )
		{
			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadow_ContactAreaPreview" ) );
		}
		else
		{
			if ( rctx.Test( RFF_RayTracedDirectionalShadow ) )
			{
				if ( rctx.Test( RFF_ContactShadows_Global ) || rctx.Test( RFF_ContactShadows_Local ) )
				{
					GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderRayTracedGlobalShadowsWithLocalContact" ) );
				}
				else
				{
					GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderRayTracedGlobalShadows" ) );
				}
			}
			else
			{
				// Contact shadows via console variable/in-game menu settings are either on or off, which corresponds
				// to both: RFF_ContactShadows_Global and RFF_ContactShadows_Local being set or not, respectively.
				// This path renders standard global shadows with contacts from local lights and sun.
				if ( rctx.Test( RFF_ContactShadows_Global ) && rctx.Test( RFF_ContactShadows_Local ) )
				{
					GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadowWithContact" ) );
				}
				// We distinguish this case for holo calls, where one wants to have only global contact shadows 
				// and no local contact shadows.
				else if( rctx.Test( RFF_ContactShadows_Local ) )
				{
					GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadowContactOnly" ) );
				}
				// NOTE: whether RFF_ContactShadows_Global is set or not it does not matter - we default to "m_shaderGlobalShadow"
				//       which means standard global shadows and no contacts.
				else
				{
					GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_shaderGlobalShadow" ) );
				}
			}
		}

	}

	GetRenderer()->GetClustered()->UnbindCommon( rctx, GpuWrapApi::ComputeShader, slots );

	if( rctx.IsConsumePhase() && rctx.GetCollector()->HasChunks( "renderstage_shadowmask" ) )
	{
		rctx.BindPSO( ColorTarget( 0, rtShadowmask ),
					  DepthTarget( rtDepth, -1, true ) );

		rctx.BindTexture< 2, ePIXEL >( rtDepth );

		rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

		RenderBindManager::SetCamera(rctx, rctx.GetRenderCamera(), rctx.GetWidth(), rctx.GetHeight(), nullptr, false );

		CRenderGeometryBatcher* batcher = GetRenderer()->GetGeometryBatcher();

		CRenderCollector &collector	= *rctx.GetCollector();
		RenderStageCollector& stageCollector = collector.m_renderStageCollector;

		GeometryBatcherStats localStats;
		CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );

		{
			const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_shadowmask" );
			batcher->RenderGeometry( renderCtx, stageCollector.GetBucket( stageId ) );
		}

		GpuApi::SetupBlankRenderTargets();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
