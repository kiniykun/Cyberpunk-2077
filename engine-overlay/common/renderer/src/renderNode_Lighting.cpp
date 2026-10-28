/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "renderInterface.h"
#include "renderGraphNodes.h"
#include "clusteredDeferredHelpers.h"
#include "clusteredDeferred.h"
#include "renderGIManager.h"
#include "renderReflectionProbeManager.h"
#include "renderShadowManager.h"
#include "renderProxyLightVolume.h"
#include "renderProxyLightBlocker.h"
#include "particleDPLTexture.h"
#include "../../renderData/include/rendererDevConfig.h"
#include "../../renderData/include/renderLightBase.h"
#include "renderDistantLightBatcher.h"
#include "renderIES.h"
#include "renderGeometryBatcher.h"
#include "renderSkinProfile.h"
#include "../../../../engine/shaders/include/globalConstantsCS.fx"
#include "../../../../engine/shaders/include/include_topDownCarMap.fx"
#include "../../../../engine/shaders/include/include_classifyMaterials.fx"
#include "../../../../engine/shaders/include/include_waterMap.fx"
#include "renderHistogram.h"
#include "renderRayTracingData.h"
// [skperf] round 33 lightchan probe (?perfprobe=1, probeMask bit 2; heap-free): RenderLightChannelVolumes took
// ~23.5 ms on mirror frames vs ~1 ms otherwise. Every ~2 s: calls, total and max ms, and for the slowest call the
// split clear / setup+volumes / mip dispatch plus its render size.
#include <atomic>
#include <cstdio>
#include <emscripten.h>
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
namespace cpLightChan
{
	std::atomic<unsigned> s_calls{ 0 };
	std::atomic<double> s_totalMs{ 0 }, s_windowStart{ 0 };
	std::atomic<double> s_maxMs{ 0 }, s_maxClear{ 0 }, s_maxVolumes{ 0 }, s_maxMip{ 0 };
	std::atomic<unsigned> s_maxW{ 0 }, s_maxH{ 0 }, s_maxVolumesFlag{ 0 };
	inline bool On() { return ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 2 ) != 0; }
	void Note( double total, double clear, double volumes, double mip, unsigned w, unsigned h, bool hadVolumes )
	{
		s_calls.fetch_add( 1, std::memory_order_relaxed );
		double t = s_totalMs.load(); while ( !s_totalMs.compare_exchange_weak( t, t + total ) ) {}
		double m = s_maxMs.load();
		if ( total > m && s_maxMs.compare_exchange_strong( m, total ) ) { s_maxClear.store( clear ); s_maxVolumes.store( volumes ); s_maxMip.store( mip ); s_maxW.store( w ); s_maxH.store( h ); s_maxVolumesFlag.store( hadVolumes ? 1u : 0u ); }
		const double now = emscripten_get_now(); double start = s_windowStart.load();
		if ( start == 0 ) { s_windowStart.compare_exchange_strong( start, now ); return; }
		if ( now - start < 2000 || !s_windowStart.compare_exchange_strong( start, now ) ) return;
		const double sec = ( now - start ) / 1000.0;
		std::fprintf( stderr, "CP2077 skperf lightchan probe: window=%.1fs calls/s=%.1f msPerSec=%.1f maxMs=%.2f (max call: clear=%.2f volumes=%.2f mip=%.2f size=%ux%u volumes=%u)\n",
			sec, s_calls.exchange( 0 ) / sec, s_totalMs.exchange( 0 ) / sec, s_maxMs.exchange( 0 ), s_maxClear.load(), s_maxVolumes.load(), s_maxMip.load(), s_maxW.load(), s_maxH.load(), s_maxVolumesFlag.load() );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RED_ALIGNED_STRUCT( GBufferOverrides, 16 )
{
	Vector4					albedoOverride;
	Vector2					roughnessOverride;
	Vector2					metalnessOverride;
	Int32					materialIdOverride;
	Float					lightScale;
	Float					foliageTranslucencyScale;

	static GBufferOverrides Get( const SRenderNodeImplContext& rctx, Float* shadowMod )
	{
		auto displayMode				= rctx.GetFrameInfo().m_displaySettings.m_displayMode;

		if( rctx.Test( rend::DF_EnvProbe_Debug_Overlap ) || rctx.Test( rend::DF_EnvProbe_Debug_ID ) )
		{
			displayMode = EMM_PureGreyReflectionView;
		}

		if( shadowMod ) *shadowMod = 0.0f;

		GBufferOverrides ret;

		ret.albedoOverride		= Vector4( 0.0f, 0.0f, 0.0f, 0.0f );
		ret.roughnessOverride	= Vector2( 0.0f, 0.0f );
		ret.metalnessOverride	= Vector2( 0.0f, 0.0f );
		ret.materialIdOverride	= -1;
		ret.lightScale			= 1.0f;
#ifndef RED_CONFIGURATION_FINAL
		if ( GetRenderer()->GetDevConfig()->Foliage.IsEnabled() )
		{
			ret.foliageTranslucencyScale = GetRenderer()->GetDevConfig()->Foliage.m_translucencyScale;
		}
		else
#endif
		{
			ret.foliageTranslucencyScale = 1.0f;
		}

		switch( displayMode )
		{
		case EMM_ClayView:				// Z-Brush like view of reddish clay, mid roughness so you get a bit of 
			ret.albedoOverride		= Color( 120, 64, 70, 254 ).ToVectorLinearAccu();
			ret.metalnessOverride	= Vector2( 0.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 0.4f, 1.0f );
			ret.materialIdOverride	= 0;
			break;
		case EMM_PureGreyscaleView:		// Grey color, all non metal with fixed high roughness - 18% grey
			ret.albedoOverride		= Vector4( 0.18f, 0.18f, 0.18f, 1.0f );
			ret.metalnessOverride	= Vector2( 0.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 0.9f, 1.0f );
			break;
		case EMM_PureWhiteView:			// White color, all non metal with fixed high roughness
			ret.albedoOverride		= Vector4( 1.0f, 1.0f, 1.0f, 1.0f );
			ret.metalnessOverride	= Vector2( 0.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 0.9f, 1.0f );
			break;
		case EMM_PureReflectionView:	// Pure white, all metal with low roughness - mirror-like surface
			ret.albedoOverride		= Vector4( 1.0f, 1.0f, 1.0f, 1.0f );
			ret.metalnessOverride	= Vector2( 1.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 0.0f, 0.0f );
			ret.materialIdOverride	= 0;
			if( shadowMod ) *shadowMod = 1.0f;
			ret.lightScale			= 0.0f;
			break;
		case EMM_PureGreyReflectionView:
			ret.albedoOverride		= Vector4( 0.18f, 0.18f, 0.18f, 1.0f );
			ret.metalnessOverride	= Vector2( 0.5f, 1.0f );
			ret.roughnessOverride	= Vector2( 0.0f, 0.0f );
			// if( shadowMod ) *shadowMod = 1.0f;
			break;
		case EMM_IlluminanceMeter:	// Pure white, no metal with roughness set to 1
			ret.albedoOverride		= Vector4( 1.0f, 1.0f, 1.0f, 1.0f );
			ret.metalnessOverride	= Vector2( 0.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 1.0f, 1.0f );
			ret.materialIdOverride	= 0;
			break;
		case EMM_GlobalIllumination:
			ret.lightScale			= 0.0f;
			ret.albedoOverride		= Vector4( 1.0f, 1.0f, 1.0f, 1.0f );
			ret.roughnessOverride	= Vector2( 1.0f, 1.0f );
			ret.metalnessOverride	= Vector2( 0.0f, 1.0f );
			ret.materialIdOverride	= 0;
			if (shadowMod) *shadowMod = 1.0f;
			break;
		}

		return ret;
	}

};


red::THash32 CRenderNode_RenderLightBuffers::GenerateShaderName( Bool castContactShadows, Bool doLightBlockers, Uint8 materialMask )
{
	red::THash32 hash = 0;
	
	if ( doLightBlockers )
	{
		hash = red::CalculateHash32( castContactShadows ? "m_shaderLightsComputeGlobalLocalContactShadows_Clustered_LightBlockers_" : "m_shaderLightsComputeGlobalLocalShadows_Clustered_LightBlockers_" );
	}
	else
	{
		hash = red::CalculateHash32( castContactShadows ? "m_shaderLightsComputeGlobalLocalContactShadows_Clustered_" : "m_shaderLightsComputeGlobalLocalShadows_Clustered_" );
	}
	
	Uint8 mask = 0x80;
	for ( Uint32 i = 0; i < 8; ++i, mask >>= 1 )
	{
		hash = red::CalculateHash32( mask & materialMask ? "1" : "0", hash );
	}
	return hash;
}

void CRenderNode_RenderLightBuffers::RenderLightBlockers( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const Bool doLightCut = rctx.Test( RFF_CharacterLightBlockers );

	auto rtLightBlockers = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "lightBlockers" ) );
	auto rtDepth = rctx.RT< RFUF_Read >( RENDER_NAME_TAG( "depthBuffer" ) );

	if ( rctx.IsConsumePhase() )
	{
		const CRenderCollector& collector = *rctx.GetCollector();
		const CRenderCollector::CRenderCollectorData *rdata = collector.m_renderCollectorData;

		if ( doLightCut )
		{
			GpuApi::ClearColorTarget( rtLightBlockers, Vector4::ZEROS() );

			if ( rdata->GetNumLightBlockers() > 0 )
			{
				red::ArraySpan< const CRenderProxy_LightBlocker* > lightBlockers = rdata->GetLightBlockers();

				struct LightBlockerSortEntry
				{
					const CRenderProxy_LightBlocker*	ptr;
					Float								distanceToCamera;
				};

				red::StaticArray< LightBlockerSortEntry, CRenderCollector::CRenderCollectorData::LIGHT_BLOCKERS_CAPACITY > lightBlockersArray;

				const Vector4 cameraPosition = rctx.GetCameraInfo().GetRenderCamera().GetPosition().AsVector4();

				// Fill entries
				for ( const CRenderProxy_LightBlocker* lightBlocker : lightBlockers )
				{
					lightBlockersArray.PushBack( { lightBlocker, lightBlocker->GetLocalToWorld().GetTranslationRef().DistanceSquaredTo( cameraPosition ) } );
				}

				// sort light blockers by distance			
				std::sort( lightBlockersArray.Begin(), lightBlockersArray.End(), []( const LightBlockerSortEntry& a, const LightBlockerSortEntry& b ) { return a.distanceToCamera < b.distanceToCamera; } );

				GetRenderer()->GetShader( SHADER_NAME( "m_shaderSingleColorCB_RGB" ) )->Bind();

				rctx.BindPSO( ColorTarget( 0, rtLightBlockers ), DepthTarget( rtDepth, -1, true ) );

				// TODO: make this 1/16th of the original RenderTarget
				Uint32 width = rctx.GetWidth();
				Uint32 height = rctx.GetHeight();
				//width = red::DivideRoundingUp( width, 16 );
				//height = red::DivideRoundingUp( height, 16 );

				rctx.SetViewport( GpuApi::ViewportDesc( width, height ) );

				const Uint32 lightBlockersLimit = Min( lightBlockersArray.Size(), (Uint32)Config::cvCharacterLightBlockers_Limit.Get() );
				for ( Uint32 i = 0; i < lightBlockersLimit; ++i )
				{
					const auto& lightBlocker = lightBlockersArray[ i ];

					struct CB
					{
						Matrix		localToWorld;
						Vector4		color;
						Float		useExposure;
					} cb;

					cb.localToWorld = lightBlocker.ptr->GetLocalToWorld().Transposed();
					cb.localToWorld.SetScale33( lightBlocker.ptr->GetRadius() );

					// pack direction from [-1; 1] to [0; 1]
					Vector4 c = ( lightBlocker.ptr->GetDirection() + Vector4::ONES() ) * 0.5f;
					c.W = 1.0f;
					cb.color = c;
					cb.useExposure = 0.0f;
					rctx.SetConstant< eVERTEX >( cb );
					GetRenderer()->GetDebugDrawer().DrawUnitSphere();
				}
			}
		}
	}
}

void CRenderNode_RenderLightBuffers::RenderTiledLights( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const Bool doLightBlockers = rctx.Test( RFF_CharacterLightBlockers );

	auto rtLightBufferDiffuse		= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "lightBufferDiffuse" ) );
	auto rtLightBufferSpecular		= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "lightBufferSpecular" ) );
	auto rtGBuffer0					= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "gbuffer0" ) );
	auto rtGBuffer1					= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "gbuffer1" ) );
	auto rtGBuffer2					= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "gbuffer2" ) );
	auto rtDepth					= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDepthWeaponCorrected 	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );
	auto rtShadowmask				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "shadowmask" ) );
	auto rtLightsIndirectArgs		= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "materialClassificationTileCount" ) );
	auto rtChannel					= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightChannel" ) );
	auto rtMaterialClassification	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "materialClassificationTileList" ) );
	auto rtLightBlockers 			= doLightBlockers ? rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightBlockers" ) ) : rctx.RTNull();

	struct CB
	{
		Uint32						firstTileListIndex;
		Float						shadowMod;
		GpuApi::cbBool				isSubsurfaceEnabled;
		GpuApi::cbBool				isLightBlockerEnabled;
		GBufferOverrides			overrides;
		CBContactShadowsSettings	contactShadows;
		SKernelDualSpecular			kernelDualSpecular[8];
	} cb;

	if( rctx.IsConsumePhase() )
	{
		const GpuApi::TextureRef texRefs[] =
		{
			/* 0  */ rtDepthWeaponCorrected.GetSafe<GpuApi::TextureRef>(),
			/* 1  */ rtGBuffer0,
			/* 2  */ rtGBuffer1,
			/* 3  */ rtChannel,
			/* 4  */ rtGBuffer2,
			/* 5  */ rtShadowmask,
		};

		cb.overrides = GBufferOverrides::Get( rctx, &cb.shadowMod );
		cb.isSubsurfaceEnabled = rctx.Test( RFF_SubsurfaceScattering );
		cb.isLightBlockerEnabled = rctx.Test( RFF_CharacterLightBlockers );

		cb.contactShadows.Init( rctx.GetFrameInfo().m_worldRenderSettings.m_contactShadows );

		red::Memzero( cb.kernelDualSpecular, sizeof( cb.kernelDualSpecular ) );
		red::Memcpy( cb.kernelDualSpecular, GetRenderer()->GetSkinProfilesManager()->GetKernelDualSpecular(), sizeof( cb.kernelDualSpecular ) );

		// If we have TXAA enabled, also enable shadow jitter. This create noise, but it gets smoothed over.
		if( rctx.Test( RFF_AntialiasingTXAA ) || rctx.Test( RFF_DLSS ) || rctx.GetFrameInfo().GetFramePurpose() == FP_ScreenShot )
		{
			cb.contactShadows.jitterStrength = 1.0f;
		}

		GpuApi::SetupBlankRenderTargets();
		rctx.BindTextures<0, eCOMPUTE>( texRefs );
		GpuApi::BindTextureStencil( 12, rtDepth, GpuWrapApi::ComputeShader );

		if (doLightBlockers)
		{
			rctx.BindTexture< 9, eCOMPUTE >( rtLightBlockers );
		}

		RenderBindManager::BindGlobalConstants(rctx, GpuWrapApi::ComputeShader );
	}

	CClusteredDeferred::BindingSlots slots;
	slots.headSlot = 6;
	slots.listSlot = 7;
	slots.listAsRW = false;
	slots.lightData = 8;

	GetRenderer()->GetClustered()->BindCommon( rctx, GpuWrapApi::ComputeShader, slots );

	if( rctx.IsConsumePhase() )
	{
		PC_SCOPE( RenderLightBuffers );

		const GpuApi::TextureRef targetRefs[2] =
		{
			/* 0  */ rtLightBufferDiffuse,
			/* 1  */ rtLightBufferSpecular
		};

		// Bind IES profiles texture
		{
			const GpuApi::TextureRef textureRef = GetRenderer()->GetIES()->GetTextureLUT();
			GpuApi::BindTextures( rend::Binding::Textures::IES_PROFILES, 1, &textureRef, GpuWrapApi::ComputeShader );
		}

		GpuApi::BindBufferSRV( rtMaterialClassification, 11, GpuWrapApi::ComputeShader );

		rctx.BindTextureUAVs<0>( targetRefs );

		Int32 flowSpace = rctx.GetCameraData()->m_renderFlowSpace;
		CClusteredDeferred::SLocalLights lightsCount = GetRenderer()->GetClustered()->GetLightsCount( flowSpace );
		const Bool castContactShadows = lightsCount.m_contactShadows && rctx.Test( RFF_ContactShadows_Local );

		const Uint32 sizeWidth = red::DivideRoundingUp( rctx.GetColorTargetWidth(), MATERIAL_CLASSIFICATION_TILE_SIZE );
		const Uint32 sizeHeight = red::DivideRoundingUp( rctx.GetColorTargetHeight(), MATERIAL_CLASSIFICATION_TILE_SIZE );

		GpuApi::BufferRef LightsIndirectArgs = rtLightsIndirectArgs;

		Uint32 lightMod = 0;

		const auto dispatch = [=,&lightMod,&cb,&rctx]( Bool castContactShadows, Bool doLightBlockers, Uint8 materialMask )
		{
			cb.firstTileListIndex = sizeWidth * sizeHeight * lightMod;
			rctx.SetConstant<eCOMPUTE>(cb);

			GpuApi::BindIndirectArgs(LightsIndirectArgs, 3 * sizeof(Uint32) * lightMod);

			GetRenderer()->GetShader( GenerateShaderName(castContactShadows, doLightBlockers, materialMask) )->AsCompute()->DispatchIndirect();

			++lightMod;
		};
		
		for ( Uint32 withContactShadows = 0; withContactShadows <= 1; ++withContactShadows )
		{
			for ( Uint32 k = 0; k < MATERIAL_CLASSIFICATION_SIMPLE_BUCKET_COUNT; ++k )
			{
				dispatch(castContactShadows && (withContactShadows != 0), doLightBlockers, gMaterialClassificationSimpleBuckets[k]);
			}

			for ( Uint32 j = 0; j < MATERIAL_CLASSIFICATION_COMPLEX_BUCKET_COUNT; ++j )
			{
				dispatch( castContactShadows && ( withContactShadows != 0 ), doLightBlockers, gMaterialClassificationComplexBuckets[j] );
			}

			{
				dispatch(castContactShadows && (withContactShadows != 0), doLightBlockers, 0xff );
			}
		}

		GpuApi::BarrierTextureUAV( targetRefs[0] );
		GpuApi::BarrierTextureUAV( targetRefs[1] );

#ifndef RED_CONFIGURATION_FINAL
		const auto& renderingMask = rctx.GetCameraInfo().GetRenderingMask();
		if ( renderingMask.Test( rend::DF_ContactShadowOverflow_Debug ) )
		{
			if ( auto shader = GetRenderer()->GetShader( SHADER_NAME( "m_shaderLightsContactShadowOverflow" ) ) )
			{
				Uint32 width = rctx.GetWidth();
				Uint32 height = rctx.GetHeight();

				shader->Bind();
				shader->Dispatch( ( width + CLUSTER_TILE_SIZE - 1 ) / CLUSTER_TILE_SIZE, ( height + CLUSTER_TILE_SIZE - 1 ) / CLUSTER_TILE_SIZE );
			}
		}
#endif
	}

	GetRenderer()->GetClustered()->UnbindCommon( rctx, GpuWrapApi::ComputeShader, slots );
}

void CRenderNode_RenderLightBuffers::RenderMeshLights( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	auto rtLightBufferDiffuse	= rctx.RT< RFUF_Write >( RENDER_NAME_TAG( "lightBufferDiffuse" ) );
	auto rtLightBufferSpecular	= rctx.RT< RFUF_Write >( RENDER_NAME_TAG( "lightBufferSpecular" ) );
	auto rtDepth				= rctx.RT< RFUF_Read >( RENDER_NAME_TAG( "depthBuffer" ) );

	if( rctx.IsConsumePhase() && rctx.GetCollector()->HasChunks( "renderstage_lighting" ) )
	{
		rctx.BindPSO( ColorTarget( 0, rtLightBufferDiffuse ),
					  ColorTarget( 1, rtLightBufferSpecular ),
					  DepthTarget( rtDepth, -1, true ) );
		
		rctx.BindTexture< 2, ePIXEL >( rtDepth );

		rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

		GetRenderer()->GetStateManager().SetCamera(rctx, rctx.GetRenderCamera(), rctx.GetWidth(), rctx.GetHeight(), nullptr, false );

		CRenderGeometryBatcher* batcher = GetRenderer()->GetGeometryBatcher();

		CRenderCollector &collector	= *rctx.GetCollector();
		RenderStageCollector& stageCollector = collector.m_renderStageCollector;

		GeometryBatcherStats localStats;
		CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );

		{
			const RenderStageId stageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_lighting" );
			batcher->RenderGeometry( renderCtx, stageCollector.GetBucket( stageId ) );
		}

		GpuApi::SetupBlankRenderTargets();
	}

}

void CRenderNode_RenderLightBuffers::RenderLightOverdraw( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
#ifndef RED_CONFIGURATION_FINAL
	if( rctx.GetFrameInfo().m_displaySettings.m_displayMode == EMM_LightOverdraw )
	{
		SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();

		auto rtDebug			= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "debug" ) , desc );
		auto rtDepth			= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
		auto rtChannel			= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightChannel" ) );

		CClusteredDeferred::BindingSlots slots;
		slots.headSlot = 6;
		slots.listSlot = 7;
		slots.listAsRW = false;
		slots.lightData = 8;

		GetRenderer()->GetClustered()->BindCommon( rctx, GpuWrapApi::ComputeShader, slots );

		if( rctx.IsConsumePhase() )
		{
			// LIGHTS_OVERDRAW
			if( auto shader = GetRenderer()->GetShader( SHADER_NAME( "m_shaderLightsOverdraw" ) ) )
			{
				GpuApi::SetupBlankRenderTargets();

				shader->Bind();

				rctx.BindTextureUAV<0>( rtDebug );

				rctx.BindTexture<0, eCOMPUTE>( rtDepth );
				rctx.BindTexture<3, eCOMPUTE>( rtChannel );
				GpuApi::BindTextureStencil( 12, rctx.RTGet<GpuApi::TextureRef>( rtDepth ), GpuWrapApi::ComputeShader );

				Uint32 width	= rctx.GetWidth();
				Uint32 height	= rctx.GetHeight();

				shader->Dispatch( (width+31)/32, (height+31)/32 );
			}

		}

		GetRenderer()->GetClustered()->UnbindCommon( rctx, GpuWrapApi::ComputeShader, slots );

	}
#endif
}

void CRenderNode_RenderLightBuffers::Execute(const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	RenderLightOverdraw( rctx, builder );

	RenderLightBlockers( rctx, builder );

	RenderTiledLights( rctx, builder );

	GetRenderer()->GetDistantLightBatcher()->RenderLights( rctx );

	RenderMeshLights( rctx, builder );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderLightsIntegrate::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	const auto& renderingMask = rctx.GetCameraInfo().GetRenderingMask();

	// Debug draw shit
#ifndef RED_CONFIGURATION_FINAL

	if( renderingMask.Test( rend::DF_ClusteredLightsCount_Debug ) || renderingMask.Test( rend::DF_LightsWithContactShadowCount_Debug ) )
	{
		GetRenderer()->GetClustered()->DebugView( rctx, GetRenderer()->GetHiZ(), rctx.GetFrameInfo(), renderingMask.Test( rend::DF_LightsWithContactShadowCount_Debug ) );

		return;
	}

#endif

	auto& info = rctx.GetCameraInfo();
	const Bool enableGlobalIllum	= info.HasFeature( RFF_GlobalIllumination ) && rctx.GetCameraData()->IsOnscreen();
	const Bool isGIPass = ( m_pass == RenderLightsIntegratePass::CalcGI || m_pass == RenderLightsIntegratePass::CalcGI_LimitedOccupancy );

	if ( isGIPass && !enableGlobalIllum )
	{
		return;
	}

	const Bool enableSSR 			= info.HasFeature( RFF_ScreenSpaceReflections );
	const Bool enableReflection		= info.HasFeature( RFF_RayTracedReflection ) || enableSSR;
	const Bool enableSubsurface		= info.HasFeature( RFF_SubsurfaceScattering );
	const Bool enableCharacterRim	= info.HasFeature( RFF_CharacterRimEnhancement );
	const Bool enableLightBlockers	= info.HasFeature( RFF_CharacterLightBlockers );

	auto rtLightBufferDiffuse	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightBufferDiffuse" ) );
	auto rtLightBufferSpecular	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightBufferSpecular" ) );
	auto rtLightBufferReflection= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "bufferReflection" ) );
	auto rtDepth				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDepthWeaponCorrected = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );
	auto rtColor				= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );
	auto rtGBuffer0				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "gbuffer0" ) );
	auto rtGBuffer1				= rctx.RT<RFUF_Read >( RENDER_NAME_TAG( "gbuffer1" ) );
	auto rtNormals				= rctx.RT<RFUF_Read >( RENDER_NAME_TAG( "normals" ) );
	auto rtGBuffer2				= rctx.RT<RFUF_Read >( RENDER_NAME_TAG( "gbuffer2" ) );
	auto rtConeAOBuffer			= info.HasFeature( RFF_RayTracedAmbientOcclusion ) ? SRenderNodeImplContext::ScopeNameTag::CreateNull() : rctx.RT<RFUF_Read >( RENDER_NAME_TAG( "coneAO" ) );
	auto rtReflection			= enableReflection && enableSSR ? rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "SSR_Result" ) ) : SRenderNodeImplContext::ScopeNameTag::CreateNull();
	auto rtSSRFade				= enableSSR ? rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "SSR_Fade" ) ) : SRenderNodeImplContext::ScopeNameTag::CreateNull();
	auto rtChannel				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "lightChannel" ) );
	auto rtGIVolume				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "giVolume" ) );
	auto rtGlobalIllumination	= rctx.RT<RFUF_Read | RFUF_Write>( RENDER_NAME_TAG( "globalIllumination" ) );

	if ( rctx.IsConsumePhase() )
	{
		auto renderer = GetRenderer();

		// Bind camera constants. 
		renderer->GetStateManager().SetCamera(rctx, rctx.GetRenderCamera(), rctx.GetWidth(),
			rctx.GetHeight(), &rctx.GetCameraData()->GetLastFrameData());

		rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

		GpuApi::TextureRef ambientOcclusionTexture = rtConeAOBuffer;
#ifdef USE_RAY_TRACING
		auto& rayTracingData = rctx.GetCameraData()->GetCustomData<RayTracingCustomData>();
		if (info.HasFeature( RFF_RayTracedAmbientOcclusion ))
		{
			ambientOcclusionTexture = rayTracingData.GetAmbientOcclusionTexture();
		}
#endif // USE_RAY_TRACING

		const GpuApi::TextureRef texCommon[] =
		{
			/* 0  */ rtDepthWeaponCorrected,
			/* 1  */ rtGBuffer0,
			/* 2  */ rtGBuffer1,
			/* 3  */ rtGBuffer2,
			/* 4  */ ambientOcclusionTexture
		};
		rctx.BindTextures<0, ePIXEL>( texCommon );
		const Uint32 texCommonCount = RED_ARRAY_COUNT( texCommon );

		struct PIX
		{
			Vector4					globalIlluminationVolumeScale;
			Vector4					globalIlluminationVolumeBias;

			Vector3					ambientBRDFCoefficients_Skin;
			Float					ambientBRDFCoefficients_CharacterGlobalInfluence;

			Vector3					ambientBRDFCoefficients_Foliage;
			Float					ambientBRDFCoefficients_SkinRoughnessFactorScale;

			Vector3					ambientBRDFCoefficients_Standard;
			Float					ambientBRDFCoefficients_SkinRoughnessFactorBias;

			Vector3					ambientBRDFCoefficients_Weapon;
			Float					giStrength;

			GpuApi::cbBool			isSubsurfaceEnabled;
			Float					globalIlluminationOverlay;
			Float					globalIlluminationEnabled;

			Float					fWeightDistanceShadow;

			Float					fWeightDistanceProbe;
			Float					fWeightNormal;
			Float					fWeightBias;
			Float					fThreshold;

			Uint32					uMaxSteps;
			Int32					iShowSteps;

			Float					useRayTracingSpecularScale;
			Float					rayTracingDiffuseIlluminationAOModulation;

			GpuApi::cbBool			useRayTracedAmbientOcclusion;
			GpuApi::cbBool			useRayTracedDiffuseIllumination;
			GpuApi::cbBool			useRayTracedReflection;
			GpuApi::cbBool			showInteriorPixels;

			GpuApi::cbBool			applyDithering;
			GpuApi::cbBool			enableMissingTetras;
			Float					ambientBRDFCoefficients_LightBlockerInfluence;
			Int32					acceleration_structure_resolution;
			Float					pad2;

			GBufferOverrides		overrides;
		} pix;

		auto& frameInfo = rctx.GetFrameInfo();
		auto displayMode				= frameInfo.m_displaySettings.m_displayMode;

		// Put it in space
		pix.globalIlluminationEnabled		= enableGlobalIllum ? 1.0f : 0.0f;
		pix.globalIlluminationOverlay		= 0.0f;
		pix.globalIlluminationVolumeScale	= Vector4( 0, 0, 0, 0 );
		pix.globalIlluminationVolumeBias	= Vector4( -40000, -40000, -40000, 0 );

		if ( !enableCharacterRim )
		{
			pix.ambientBRDFCoefficients_Skin		= Vector3( 1.0f, 1.0f, 0.f );
			pix.ambientBRDFCoefficients_Foliage		= Vector3( 1.0f, 1.0f, 0.f );
			pix.ambientBRDFCoefficients_Standard	= Vector3( 1.0f, 1.0f, 0.f );

			pix.ambientBRDFCoefficients_CharacterGlobalInfluence	= 0.f;
			pix.ambientBRDFCoefficients_SkinRoughnessFactorScale	= 0.f;
			pix.ambientBRDFCoefficients_SkinRoughnessFactorBias		= 0.f;

			pix.ambientBRDFCoefficients_LightBlockerInfluence		= 0.f;
		}
		else
		{
			if ( Config::GGlobalRenderingSettings.allowRaytracing )
			{
				pix.ambientBRDFCoefficients_Skin		= Vector3( Config::cvCharacterRim_RayTracing_Skin_Specular.Get(),		Config::cvCharacterRim_RayTracing_Skin_Fresnel.Get(),		Config::cvCharacterRim_RayTracing_Skin_ConstOffset.Get()		);
				pix.ambientBRDFCoefficients_Foliage		= Vector3( Config::cvCharacterRim_RayTracing_Foliage_Specular.Get(),	Config::cvCharacterRim_RayTracing_Foliage_Fresnel.Get(),	Config::cvCharacterRim_RayTracing_Foliage_ConstOffset.Get()		);
				pix.ambientBRDFCoefficients_Standard	= Vector3( Config::cvCharacterRim_RayTracing_Standard_Specular.Get(),	Config::cvCharacterRim_RayTracing_Standard_Fresnel.Get(),	Config::cvCharacterRim_RayTracing_Standard_ConstOffset.Get()	);
				pix.ambientBRDFCoefficients_Weapon		= Vector3( Config::cvCharacterRim_RayTracing_Weapon_Specular.Get(),		Config::cvCharacterRim_RayTracing_Weapon_Fresnel.Get(),		Config::cvCharacterRim_RayTracing_Weapon_ConstOffset.Get()		);
				pix.ambientBRDFCoefficients_CharacterGlobalInfluence	= Config::cvCharacterRim_RayTracing_Character_Fresnel.Get();
				pix.ambientBRDFCoefficients_SkinRoughnessFactorScale	= Config::cvCharacterRim_RayTracing_RoughnessFactor_Scale.Get();
				pix.ambientBRDFCoefficients_SkinRoughnessFactorBias		= Config::cvCharacterRim_RayTracing_RoughnessFactor_Bias.Get();
				pix.ambientBRDFCoefficients_LightBlockerInfluence		= Config::cvCharacterRim_RayTracing_LightBlockerInfluence.Get() * ( enableLightBlockers ? 1.f : 0.f );
			}
			else
			{
				pix.ambientBRDFCoefficients_Skin		= Vector3( Config::cvCharacterRim_Skin_Specular.Get(),		Config::cvCharacterRim_Skin_Fresnel.Get(),		Config::cvCharacterRim_Skin_ConstOffset.Get()		);
				pix.ambientBRDFCoefficients_Foliage		= Vector3( Config::cvCharacterRim_Foliage_Specular.Get(),	Config::cvCharacterRim_Foliage_Fresnel.Get(),	Config::cvCharacterRim_Foliage_ConstOffset.Get()	);
				pix.ambientBRDFCoefficients_Standard	= Vector3( Config::cvCharacterRim_Standard_Specular.Get(),	Config::cvCharacterRim_Standard_Fresnel.Get(),	Config::cvCharacterRim_Standard_ConstOffset.Get()	);
				pix.ambientBRDFCoefficients_Weapon		= Vector3( Config::cvCharacterRim_Weapon_Specular.Get(),	Config::cvCharacterRim_Weapon_Fresnel.Get(),	Config::cvCharacterRim_Weapon_ConstOffset.Get()		);
				pix.ambientBRDFCoefficients_CharacterGlobalInfluence	= Config::cvCharacterRim_Character_Fresnel.Get();
				pix.ambientBRDFCoefficients_SkinRoughnessFactorScale	= Config::cvCharacterRim_RoughnessFactor_Scale.Get();
				pix.ambientBRDFCoefficients_SkinRoughnessFactorBias		= Config::cvCharacterRim_RoughnessFactor_Bias.Get();
				pix.ambientBRDFCoefficients_LightBlockerInfluence		= Config::cvCharacterRim_LightBlockerInfluence.Get() * ( enableLightBlockers ? 1.f : 0.f );
			}
		}

		if( enableGlobalIllum )
		{
			// Open up with the loaded values from the 
			GetRenderer()->GetGIManager()->GetWeights(pix.fWeightDistanceShadow,
				pix.fWeightDistanceProbe,
				pix.fWeightNormal,
				pix.fWeightBias,
				pix.fThreshold);
			pix.uMaxSteps = GI::MAX_TETRA_STEPS;
			pix.iShowSteps = false;
			pix.showInteriorPixels = false;
			pix.acceleration_structure_resolution = Config::GGlobalRenderingSettings.globalIllumination.accelerationStructureResolution;

#ifndef RED_CONFIGURATION_FINAL
			pix.globalIlluminationOverlay	= displayMode == EMM_GlobalIllumination ? 1.0f : 0.0f;

			if ( GetRenderer()->GetDevConfig()->GlobalIllumination.IsEnabled() )
			{
				pix.fWeightDistanceShadow = GetRenderer()->GetDevConfig()->GlobalIllumination.m_weightDistanceShadow;
				pix.fWeightDistanceProbe = GetRenderer()->GetDevConfig()->GlobalIllumination.m_weightDistanceProbe;
				pix.fWeightNormal = GetRenderer()->GetDevConfig()->GlobalIllumination.m_weightNormal;
				pix.fWeightBias = GetRenderer()->GetDevConfig()->GlobalIllumination.m_weightBias;
				pix.fThreshold = GetRenderer()->GetDevConfig()->GlobalIllumination.m_threshold;
			}
			pix.uMaxSteps = GetRenderer()->GetDevConfig()->GlobalIllumination.m_maxSteps;
			pix.iShowSteps = GetRenderer()->GetDevConfig()->GlobalIllumination.m_showSteps;
			pix.showInteriorPixels = GetRenderer()->GetDevConfig()->GlobalIllumination.m_showInteriorPixels;

			pix.enableMissingTetras = rctx.Test( rend::DF_GI_Debug_MissingTetra );

#endif //  RED_CONFIGURATION_FINAL

			GetRenderer()->GetGIManager()->GetWordScaleBias( pix.globalIlluminationVolumeScale,
															 pix.globalIlluminationVolumeBias );

			rctx.BindBufferSRV<9, ePIXEL>( GetRenderer()->GetGIManager()->GetRelevantTetraLookupsData() );
			rctx.BindBufferSRV<10, ePIXEL>( info.HasFeature( RFF_RayTracedDiffuseIllumination ) ? GetRenderer()->GetGIManager()->GetLitProbes_NoSky() : GetRenderer()->GetGIManager()->GetLitProbes() );
			rctx.BindBufferSRV<11, ePIXEL>( GetRenderer()->GetGIManager()->GetRelevantTetraInterpolationsData() );
			rctx.BindBufferSRV<12, ePIXEL>( GetRenderer()->GetGIManager()->GetRelevantTetraOffsetsData() );
		}

		const auto& ambientOverride = rctx.GetFrameInfo().m_worldRenderSettings.m_ambientOverride;

		pix.overrides			= GBufferOverrides::Get( rctx, nullptr );
		pix.isSubsurfaceEnabled = enableSubsurface;
		pix.giStrength			= GetRenderer()->GetGIStrength();

		pix.useRayTracingSpecularScale = 1.0f;
		pix.rayTracingDiffuseIlluminationAOModulation = Config::cvRayTracingDiffuseIlluminationAOModulation.Get();
		pix.useRayTracedAmbientOcclusion = info.HasFeature(RFF_RayTracedAmbientOcclusion);
		pix.useRayTracedDiffuseIllumination = info.HasFeature(RFF_RayTracedDiffuseIllumination);
		pix.useRayTracedReflection = info.HasFeature( RFF_RayTracedReflection );

		// apply dithering when outputting to R11G11B10
		pix.applyDithering = !Config::GGlobalRenderingSettings.UseHighPrecisionRenderTargets;

		rctx.SetConstant<ePIXEL>( pix );

		// Global Illumination Integrate
		if ( isGIPass )
		{
			PC_GPU_SCOPE( GICalculate );

			TStaticMetaEntryHash shaderName = SHADER_NAME( "m_shaderGICalculate" );

			const GpuApi::TextureRef textureRefs[] =
			{
				GetRenderer()->GetGIManager()->GetVolumeTetraAccelerationStartingTetra(),
				GetRenderer()->GetGIManager()->GetVolumeTetraAccelerationStartingSlot(),
				rtGIVolume,
			};
			rctx.BindTextures<texCommonCount, ePIXEL>( textureRefs );
			rctx.BindTexture<2,ePIXEL>( rtNormals );

			rctx.BindPSO( ColorTarget( 0, rtGlobalIllumination ),
						  DepthTarget( rtDepth, -1, true ) );

			// TODO switch to full screen triangle render
			GetRenderer()->GetShader(shaderName)->Bind();

			GpuApi::ComputeCuUsage cuUsage = GpuApi::ComputeCuUsage::Max;

			// looks like reducing CU ussage on base XBox makes things slower
			// TODO check other platforms
			if ( GpuApi::GetDeviceType() != GpuApi::eDeviceType::eXBOX_ONE && GpuApi::GetDeviceType() != GpuApi::eDeviceType::eXBOX_ONE_S )
			{
				if ( m_pass == RenderLightsIntegratePass::CalcGI_LimitedOccupancy )
				{
					cuUsage = GpuApi::ComputeCuUsage::Low;
				}
			}

			GetRenderer()->GetDebugDrawer().DrawQuad( Vector4( -1, -1, 0, 1 ), Vector4( 1, 1, 0, 1 ), rctx.GetRenderCamera().GetDepthBufferClearValue(), cuUsage );

			rctx.BindTextures<0, ePIXEL>( texCommon );
		}

		if ( m_pass == RenderLightsIntegratePass::Integrate )
		{
			PC_GPU_SCOPE( RenderLightIntegrate );

			GpuApi::BindTexture( 12, rtChannel, GpuWrapApi::PixelShader );
			GpuApi::BindTextureStencil( 13, rtDepth, GpuWrapApi::PixelShader );

			TStaticMetaEntryHash shaderName = 0;

			// Reflection probes
			{
				auto& dt = rctx.GetCameraData()->GetCustomData<CameraCustomData_ReflectionProbes>();
				auto& refl = rctx.GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>();

				if ( rctx.GetFrameInfo().GetRenderingMode() == RM_Shaded_NoAmbient )
				{
					// TODO: refactor this nicely after demo
					shaderName = SHADER_NAME( "m_shaderLightIntegrate_NoAmbient" );
				}
				else
				{
					if ( auto ptr = refl.GetRelighter() )
					{
						GpuApi::ScopedTextureUnlock unlock( rend::Binding::Textures::SKY_IRRADIANCE );
						// GpuApi::ScopedTextureUnlock unlock( rend::Binding::Textures::SKY_MODEL ); // This is kinda stupid - foward globals are not blocking all slots

						{
							const GpuApi::TextureRef textureRef = ptr->GetSkyTexture();
							GpuApi::BindTexture( rend::Binding::Textures::SKY_IRRADIANCE, textureRef, GpuWrapApi::PixelShader );
						}
						{
							const GpuApi::TextureRef textureRef = ptr->GetSkyParaboloidTexture();
							GpuApi::BindTexture( rend::Binding::Textures::SKY_MODEL, textureRef, GpuWrapApi::PixelShader );
						}
					}

					{
						const GpuApi::BufferRef exposure = CameraCustomData_Histogram::GetFrameExposure( rctx );
						GpuApi::BindBufferSRV( exposure, rend::Binding::Textures::FRAME_EXPOSURE, GpuWrapApi::PixelShader );
						GpuApi::BindBufferSRV( exposure, rend::Binding::Textures::FRAME_EXPOSURE, GpuWrapApi::VertexShader );
					}

					if ( auto mgr = GetRenderer()->GetDistantGIManager() )
					{
						const GpuApi::TextureRef textureRef = mgr->GetIrradiance();
						GpuApi::BindTexture( rend::Binding::Textures::WORLD_IRRADIANCE, textureRef, GpuWrapApi::PixelShader );
					}

					if ( !ambientOverride.m_enabled && false == dt.GetProbesData().isNull() && false == dt.GetProbesIndices().isNull() )
					{
#ifdef USE_RAY_TRACING
						if ( info.HasFeature( RFF_RayTracedDiffuseIllumination ) )
						{
							GpuApi::BindConstantBuffer( rend::Binding::Constants::CUSTOM, dt.GetProbesNoSkyData(), GpuWrapApi::PixelShader );
						}
						else
#endif
						{
							GpuApi::BindConstantBuffer( rend::Binding::Constants::CUSTOM, dt.GetProbesData(), GpuWrapApi::PixelShader );
						}

						GpuApi::BindBufferSRV( dt.GetProbesIndices(), rend::Binding::Textures::ENV_INDICES, GpuWrapApi::PixelShader );

#ifndef RED_CONFIGURATION_FINAL
						if ( renderingMask.Test( rend::DF_EnvProbe_Debug_TilesCount ) )
							shaderName = SHADER_NAME( "m_shaderLightIntegrate_EnvTilesCount" );
						else if ( renderingMask.Test( rend::DF_EnvProbe_Debug_ID ) )
							shaderName = SHADER_NAME( "m_shaderLightIntegrate_EnvID" );
						else if ( renderingMask.Test( rend::DF_EnvProbe_Debug_Overlap ) )
							shaderName = SHADER_NAME( "m_shaderLightIntegrate_EnvOverlap" );
#endif

					}
					else
					{
						shaderName = SHADER_NAME( "m_shaderLightIntegrate_NoEnvProbes" );
					}
				}
			}

			// Enables using IBL cube as a pure reflection. Disabled for now
			// shaderName = "m_shaderLightIntegrate_CubeIBL";

			GpuApi::TextureRef reflectionTexture = enableReflection ? rtReflection : GpuApi::TextureRef::Null();
			GpuApi::TextureRef rayTracedDiffuseIllumination = GpuApi::TextureRef::Null();
#ifdef USE_RAY_TRACING
			if (info.HasFeature( RFF_RayTracedDiffuseIllumination ))
			{
				rayTracedDiffuseIllumination = rayTracingData.GetDiffuseIlluminationTexture();
			}

			if (info.HasFeature( RFF_RayTracedReflection ))
			{
				reflectionTexture = rayTracingData.GetReflectionTexture();
			}
#endif // USE_RAY_TRACING

			const GpuApi::TextureRef textureRefs[] =
			{
				rtLightBufferDiffuse,
				rtLightBufferSpecular,
				enableGlobalIllum ? rtGlobalIllumination : GpuApi::TextureRef::Null(),
				reflectionTexture,
				enableSSR ? rtSSRFade : GpuApi::TextureRef::Null(),
				rayTracedDiffuseIllumination,
			};
			const Uint32 texRefsCount = RED_ARRAY_COUNT( textureRefs );
			if ( (pix.useRayTracedDiffuseIllumination || pix.useRayTracedReflection) && !shaderName )
			{
				GpuApi::BindTextures( texCommonCount, texRefsCount, textureRefs, GpuWrapApi::PixelShader );

				if ( rctx.Test( RFF_RayTracingNRD ) )
				{
					shaderName = pix.useRayTracedDiffuseIllumination && pix.useRayTracedReflection 
						? SHADER_NAME( "m_shaderLightIntegrate_RayTracing_All_NRD" )
						: pix.useRayTracedDiffuseIllumination
							? SHADER_NAME( "m_shaderLightIntegrate_RayTracing_Diffuse_NRD" )
							: SHADER_NAME( "m_shaderLightIntegrate_RayTracing_Reflection_NRD" );
				}
				else
				{
					shaderName = SHADER_NAME( "m_shaderLightIntegrate_RayTracing" );
				}
			}
			else
			{
				GpuApi::BindTextures( texCommonCount, texRefsCount - 1, textureRefs, GpuWrapApi::PixelShader );

				shaderName = shaderName ? shaderName : SHADER_NAME( "m_shaderLightIntegrate" );
			}

			rctx.BindPSO( ColorTarget( 0, rtColor ),
						  ColorTarget( 1, rtLightBufferReflection ),
						  DepthTarget( rtDepth, -1, true ) );

			GetRenderer()->DrawTriangleNoBuffers( shaderName );

			rctx.Unbind();
		}

	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderFogOverlay::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	auto& info = rctx.GetCameraInfo();

#ifndef RED_CONFIGURATION_FINAL
	if( !rctx.Test( rend::DF_RenderMask_Fog ) )
		return;

	const Bool enableSimpleFogDebug	= rctx.Test( rend::DF_SimpleFogPreview );
#else
	const Bool enableSimpleFogDebug	= false;
#endif

	if( rctx.GetCameraData()->GetInfo().m_sceneLayer == RenderSceneLayer::WorldMap )
		return;

	const Bool enableSimpleFog		= ( info.HasFeature( RFF_SimpleFog ) || rctx.GetCameraData()->IsOffscreen() || enableSimpleFogDebug ) && rctx.GetFrameInfo().m_worldRenderSettings.m_distanceFog.m_simpleDensity > 0.0f;
	const Bool enableVolFog			= info.HasFeature( RFF_VolumetricFog );
	const Bool enableDistantFog		= info.HasFeature( RFF_DistantFog );

	if( !enableVolFog && !enableDistantFog && !enableSimpleFog )
		return;

	auto rtDepthWeaponCorrected = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );
	auto rtColor				= rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );

	if( rctx.IsConsumePhase() )
	{
		TStaticMetaEntryHash shaderName = SHADER_NAME( "m_applyFogOverlay" );

		if( enableSimpleFog )
		{
			shaderName = SHADER_NAME( "m_applyFogOverlay_Simple" );

			struct ShaderData
			{
				Vector4		colorDensity;
			} pix;

			const auto& info = rctx.GetFrameInfo().m_worldRenderSettings.m_distanceFog;

			pix.colorDensity	= info.m_simpleColor.GetHDR();
			pix.colorDensity.W	= Max( info.m_simpleDensity, 0.0f );

			rctx.SetConstant<ePIXEL>( pix );
		}
		else
		{
			auto& refl = rctx.GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>();

			if( auto ptr = refl.GetRelighter() )
			{
				GpuApi::ScopedTextureUnlock unlock( rend::Binding::Textures::SKY_IRRADIANCE );
				// GpuApi::ScopedTextureUnlock unlock( rend::Binding::Textures::SKY_MODEL ); // This is kinda stupid - foward globals are not blocking all slots

				{
					const GpuApi::TextureRef textureRef = ptr->GetSkyTexture();
					GpuApi::BindTexture( rend::Binding::Textures::SKY_IRRADIANCE, textureRef, GpuWrapApi::PixelShader );
				}
				{
					const GpuApi::TextureRef textureRef = ptr->GetSkyParaboloidTexture();
					GpuApi::BindTexture( rend::Binding::Textures::SKY_MODEL, textureRef, GpuWrapApi::PixelShader );
				}
			}
		}

		{
			const GpuApi::BufferRef exposure = CameraCustomData_Histogram::GetFrameExposure( rctx );
			GpuApi::BindBufferSRV( exposure, rend::Binding::Textures::FRAME_EXPOSURE, GpuWrapApi::PixelShader );
			GpuApi::BindBufferSRV( exposure, rend::Binding::Textures::FRAME_EXPOSURE, GpuWrapApi::VertexShader );
		}

		rctx.BindTexture<0, ePIXEL>( rtDepthWeaponCorrected.GetSafe<GpuApi::TextureRef>() );

		rctx.BindPSO( ColorTarget( 0, rtColor ) );

		GetRenderer()->DrawTriangleNoBuffers( shaderName );

		rctx.Unbind();
	}

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_DecoupledParticleLighting::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( DecoupledParticleLighting );

	CParticleDPLTexture::AllocateParticleTiles( rctx );	// FIXME: move this earlier in the frame
	CParticleDPLTexture::ComputeParticleLighting( rctx );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderTopDownCarProxies::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( RenderTopDownCarProxies );
	constexpr Uint32 topDownProxyTextureRes = 256;

	auto rtTopDownCarProxiesDepth = rctx.RT< RFUF_Write >( RENDER_NAME_TAG( "topDownCarProxiesDepth" ) );
	auto rtTopDownCarProxiesDepthBackFace = rctx.RT< RFUF_Write >( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ) );

	if ( rctx.IsConsumePhase() )
	{
		Vector4 clearValue = Vector4::ZEROS();
		GpuApi::ClearColorTarget( rtTopDownCarProxiesDepth, clearValue );
		GpuApi::ClearColorTarget( rtTopDownCarProxiesDepthBackFace, clearValue );

		for ( Uint16 face = 0; face < 2; ++face )
		{
			RenderStageId renderStageId = face? GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_top_down_car_proxy_back_face", 0 ) : 
				GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_top_down_car_proxy", 0 );
			SRenderStageCluster renderStageCluster = SRenderStageCluster::Init( RPl_Scene );

			CRenderCollector& collector = *rctx.GetCollector();
			RenderStageCollector& stageCollector = collector.m_renderStageCollector;
			RenderBindManager& stateManager = GetRenderer()->GetStateManager();

			GeometryBatcherStats localRenderingStats;
			CRenderGeometryBatcher::RenderContext renderCtx( rctx, localRenderingStats );

			//
			// Set up terrain camera
			ERenderingPlane renderingPlane = RPl_Scene;
			CRenderCamera playerCamera = rctx.GetRenderCamera( renderingPlane );
			Vector4 playerPosition = playerCamera.GetPosition().AsVector4();

			//Set camera position and look direction based on collected AutoSpawner proxies
			constexpr float topDownCameraFarPlane = 128.f;
			constexpr float topDownCameraHeightOffset = 64.f;

			Vector3 topDownCameraWorldPos = playerPosition;
			topDownCameraWorldPos.Z += topDownCameraHeightOffset; //TODO

			const float textureResToTexSize = float( TOP_DOWN_CAR_MAP_RES ) / TOP_DOWN_CAR_MAP_WORLD_SIZE;

			//clamp camera position to 1 meter to avoid subpixel precision errors
			topDownCameraWorldPos = Vector3( floorf( topDownCameraWorldPos.X * TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE ) / TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE,
				floorf( topDownCameraWorldPos.Y * TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE ) / TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE,
				floorf( topDownCameraWorldPos.Z * TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE ) / TOP_DOWN_CAR_MAP_RES_TO_WORLD_SIZE );

			// max radius for spawned meshes is 128 and we are in the center (to avoid subpixel errors when rotating camera).
			// So setting orthoViewLength to 256 to cover entire visible area.
			constexpr float orthoViewLength = TOP_DOWN_CAR_MAP_WORLD_SIZE;

			Quaternion cameraRotation = Quaternion::IDENTITY();
			cameraRotation = cameraRotation.BuildFromDirectionVector( Vector4( 0, 0, -1, 0 ) );

			CRenderCamera topDownCamera;

			CRenderCamera::CreationContext topDownCameraContext;
			{
				topDownCameraContext.position = WorldPosition( topDownCameraWorldPos );
				topDownCameraContext.rotation = cameraRotation;
				topDownCameraContext.fov = 0.0f;
				topDownCameraContext.aspect = 1.0f;
				topDownCameraContext.nearPlane = 1.0f;
				topDownCameraContext.farPlane = topDownCameraFarPlane;
				topDownCameraContext.zoom = orthoViewLength;
			}
			topDownCamera = CRenderCamera( topDownCameraContext );
			//no jitter
			topDownCamera.SetSubpixelOffset( 0, 0, 1, 1 );

			stateManager.SetCamera(rctx, topDownCamera, topDownProxyTextureRes, topDownProxyTextureRes,
				&rctx.GetCameraData()->GetLastFrameData( renderingPlane ) );

			rctx.SetViewport( GpuApi::ViewportDesc( topDownProxyTextureRes, topDownProxyTextureRes ) );

			renderCtx.sortingMode = SM_OptimizedBatching;

			rctx.BindPSO( ColorTarget( 0, face ? rtTopDownCarProxiesDepthBackFace : rtTopDownCarProxiesDepth ) );

			GpuApi::BeginProfilerBlock( GetRenderer()->GetRenderStageRegistry().GetRegisteredStageName( renderStageId ).Data() );
			const RenderStageCollectorBucket& bucket = stageCollector.GetBucket( renderStageId, renderStageCluster );
			GetRenderer()->GetGeometryBatcher()->RenderGeometry( renderCtx, bucket );
			GpuApi::BindTextures( face ? rend::Binding::Textures::TOP_DOWN_CAR_BACK_FACE_MAP : rend::Binding::Textures::TOP_DOWN_CAR_MAP, 
				1, &( ( const GpuApi::TextureRef& )(face? rtTopDownCarProxiesDepthBackFace : rtTopDownCarProxiesDepth) ), GpuWrapApi::PixelShader );
			GpuApi::EndProfilerBlock();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderTopDownRainDepth::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( RenderTopDownRainDepth );

	auto rtTopDownRainDepth = rctx.RT< RFUF_Write >( RENDER_NAME_TAG( "topDownRainDepth" ) );

	if (rctx.IsConsumePhase())
	{
		RenderStageId renderStageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_top_down_rain_depth", 0 );
		SRenderStageCluster renderStageCluster = SRenderStageCluster::Init( RPl_Scene );

		Vector4 clearValue = Vector4::ZEROS();
		GpuApi::ClearColorTarget( rtTopDownRainDepth, clearValue );

		CRenderCollector& collector = *rctx.GetCollector();
		RenderStageCollector& stageCollector = collector.m_renderStageCollector;
		RenderBindManager& stateManager = GetRenderer()->GetStateManager();

		GeometryBatcherStats localRenderingStats;
		CRenderGeometryBatcher::RenderContext renderCtx( rctx, localRenderingStats );

		//
		// Set up terrain camera
		ERenderingPlane renderingPlane = RPl_Scene;
		CRenderCamera playerCamera = rctx.GetRenderCamera( renderingPlane );
		Vector4 playerPosition = playerCamera.GetPosition().AsVector4();

		//Set camera position and look direction based on collected AutoSpawner proxies
		constexpr float topDownCameraFarPlane = TOP_DOWN_WATER_MAP_DEPTH;
		constexpr float topDownCameraHeightOffset = TOP_DOWN_WATER_MAP_DEPTH / 2;

		Vector3 topDownCameraWorldPos = playerPosition;
		topDownCameraWorldPos.Z += topDownCameraHeightOffset; //TODO


		//clamp camera position to 1 meter to avoid subpixel precision errors
		
		topDownCameraWorldPos = Vector3( floorf( topDownCameraWorldPos.X * TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE) / TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE,
										 floorf( topDownCameraWorldPos.Y * TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE) / TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE,
										 floorf( topDownCameraWorldPos.Z * TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE) / TOP_DOWN_WATER_MAP_RES_TO_WORLD_SIZE);
		
		// max radius for spawned meshes is 128 and we are in the center (to avoid subpixel errors when rotating camera).
		// So setting orthoViewLength to 256 to cover entire visible area.
		constexpr float orthoViewLength = TOP_DOWN_WATER_MAP_WORLD_SIZE;

		Quaternion cameraRotation = Quaternion::IDENTITY();
		cameraRotation = cameraRotation.BuildFromDirectionVector( Vector4( 0, 0, -1, 0 ) );

		CRenderCamera topDownCamera;

		CRenderCamera::CreationContext topDownCameraContext;
		{
			topDownCameraContext.position = WorldPosition( topDownCameraWorldPos );
			topDownCameraContext.rotation = cameraRotation;
			topDownCameraContext.fov = 0.0f;
			topDownCameraContext.aspect = 1.0f;
			topDownCameraContext.nearPlane = 1.0f;
			topDownCameraContext.farPlane = topDownCameraFarPlane;
			topDownCameraContext.zoom = orthoViewLength;
		}
		topDownCamera = CRenderCamera( topDownCameraContext );
		//no jitter
		topDownCamera.SetSubpixelOffset( 0, 0, 1, 1 );

		stateManager.SetCamera(rctx, topDownCamera, TOP_DOWN_WATER_MAP_RES, TOP_DOWN_WATER_MAP_RES,
								&rctx.GetCameraData()->GetLastFrameData( renderingPlane ) );

		rctx.SetViewport( GpuApi::ViewportDesc( TOP_DOWN_WATER_MAP_RES, TOP_DOWN_WATER_MAP_RES ) );

		renderCtx.sortingMode = SM_OptimizedBatching;

		rctx.BindPSO( ColorTarget( 0, rtTopDownRainDepth ) );

		GpuApi::BeginProfilerBlock( GetRenderer()->GetRenderStageRegistry().GetRegisteredStageName( renderStageId ).Data() );
		const RenderStageCollectorBucket& bucket = stageCollector.GetBucket( renderStageId, renderStageCluster );
		GetRenderer()->GetGeometryBatcher()->RenderGeometry( renderCtx, bucket );
		GpuApi::BindTextures( rend::Binding::Textures::TOP_DOWN_CAR_MAP, 1, &( ( const GpuApi::TextureRef& )rtTopDownRainDepth ), GpuWrapApi::PixelShader );
		GpuApi::EndProfilerBlock();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ClassifyMaterials::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( ClassifyMaterials );

	SRenderFlowTargetDesc descMaterialClass;
	descMaterialClass.m_extraUsageFlags = 0;
	descMaterialClass.m_extraUsageExFlags = 0;
	descMaterialClass.m_format = GpuWrapApi::TEXFMT_R8_Uint;
	descMaterialClass.m_isSamplable = true;
	descMaterialClass.m_maxWidth = red::DivideRoundingUp( rctx.GetMaxWidth(), MATERIAL_CLASSIFICATION_TILE_SIZE );
	descMaterialClass.m_maxHeight = red::DivideRoundingUp( rctx.GetMaxHeight(), MATERIAL_CLASSIFICATION_TILE_SIZE );
	descMaterialClass.m_sizeWidth = red::DivideRoundingUp( rctx.GetColorTargetWidth(),  MATERIAL_CLASSIFICATION_TILE_SIZE );
	descMaterialClass.m_sizeHeight = red::DivideRoundingUp( rctx.GetColorTargetHeight(), MATERIAL_CLASSIFICATION_TILE_SIZE );
	auto rtMaterialClass		= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "materialClassification" ), descMaterialClass );

	auto rtDepth				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtHiDepth				= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "hierarchicalDepthBuffer" ) );

	SRenderFlowTargetDesc descTileCount;
	descTileCount.m_type = FRT_Buffer;
	// 3 for num of indirect draw calls args, 2 for with/without contact shadows
	descTileCount.m_maxElemCount = descTileCount.m_elemCount = MATERIAL_CLASSIFICATION_TOTAL_BUCKET_COUNT * 3 * 2; 
	descTileCount.m_elemStride	= sizeof( Uint32 );
	descTileCount.m_category	= GpuWrapApi::BCC_IndirectUAV;
	descTileCount.m_usage		= GpuApi::BUT_Default;
	auto bufTileCount			= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "materialClassificationTileCount" ), descTileCount );

	SRenderFlowTargetDesc descTileList;
	descTileList.m_type = FRT_Buffer;
	// 1 for xy dimensions, 1 for with/without contact shadows
	descTileList.m_maxElemCount	= MATERIAL_CLASSIFICATION_TOTAL_BUCKET_COUNT * descMaterialClass.m_maxWidth * descMaterialClass.m_maxHeight * 2;
	descTileList.m_elemCount	= MATERIAL_CLASSIFICATION_TOTAL_BUCKET_COUNT * descMaterialClass.m_sizeWidth * descMaterialClass.m_sizeHeight * 2;
	descTileList.m_elemStride	= sizeof( Uint32 );
	descTileList.m_category		= GpuWrapApi::BCC_StructuredUAV;
	descTileList.m_usage		= GpuApi::BUT_Default;
	auto bufTileList			= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "materialClassificationTileList" ), descTileList );

	CClusteredDeferred::BindingSlots slots;
	slots.headSlot = 2;
	GetRenderer()->GetClustered()->BindCommon( rctx, GpuWrapApi::ComputeShader, slots );

	if ( rctx.IsConsumePhase() )
	{
		struct COM
		{
			Vector2		invScreenSize;
		} com;

		// Get gbuffer size
		{
			const auto desc = GpuApi::GetTextureDesc( rtDepth );

			com.invScreenSize.X = 1.0f / Float( desc.width );
			com.invScreenSize.Y = 1.0f / Float( desc.height );
		}

		CRenderShaderCompute* shaderClassifyMaterials = GetRenderer()->GetShader( SHADER_NAME( "m_classifyMaterials" ) )->AsCompute();
		RED_FATAL_ASSERT( shaderClassifyMaterials, "Couldn't load allocate material classification shader!" );

		CRenderShaderCompute* shaderBuildMaterialTileLists = GetRenderer()->GetShader( SHADER_NAME( "m_buildMaterialTileLists" ) )->AsCompute();
		RED_FATAL_ASSERT( shaderBuildMaterialTileLists, "Couldn't load build material tile lists shader!" );

		rctx.BindTextureUAV<0>( rtMaterialClass );

		rctx.BindTextureStencil<0, eCOMPUTE>( rtDepth );
		rctx.BindTexture<1, eCOMPUTE>( rtDepth );
		rctx.SetConstant<eCOMPUTE>( com );

		shaderClassifyMaterials->Dispatch( descMaterialClass.m_sizeWidth, descMaterialClass.m_sizeHeight );

		GpuApi::BarrierTextureUAV( rtMaterialClass ); // FIXME: use a split barrier here instead

		GpuApi::ClearBufferUAV_Zero( bufTileCount );

		rctx.BindBufferUAV<0>( bufTileCount );
		rctx.BindBufferUAV<1>( bufTileList );

		const GpuApi::TextureRef texRefsBuildTileLists[] =
		{
			/* 0 */ rtHiDepth,
			/* 1 */ rtMaterialClass,
		};

		rctx.BindTextures<0, eCOMPUTE>( texRefsBuildTileLists );

		SBuildMaterialTileListsConsts bmtlConsts;
		bmtlConsts.listLength = descMaterialClass.m_sizeWidth * descMaterialClass.m_sizeHeight;
		rctx.SetConstant<eCOMPUTE>( bmtlConsts );

		shaderBuildMaterialTileLists->Dispatch( red::DivideRoundingUp( descMaterialClass.m_sizeWidth, DEFAULT_THREAD_GROUP_SIZE ),
												red::DivideRoundingUp( descMaterialClass.m_sizeHeight, DEFAULT_THREAD_GROUP_SIZE ) );

		rctx.Unbind();

		GpuApi::BarrierBufferUAV( bufTileCount );
		GpuApi::BarrierBufferUAV( bufTileList );
	}

	GetRenderer()->GetClustered()->UnbindCommon( rctx, GpuWrapApi::ComputeShader, slots );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderLightChannelVolumes::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( RenderLightChannelVolumes );

	SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc(GpuWrapApi::TEXFMT_R8_Uint);
	desc.m_denyUnorderedAccess = true;

	auto rtChannel			= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "lightChannel" ), desc );
	auto rtDepth			= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );

	desc.m_denyUnorderedAccess = false;
	desc.m_maxWidth		= red::DivideRoundingUp( desc.m_maxWidth, 32 );
	desc.m_maxHeight	= red::DivideRoundingUp( desc.m_maxHeight, 32 );
	desc.m_sizeWidth	= red::DivideRoundingUp( desc.m_sizeWidth, 32 );
	desc.m_sizeHeight	= red::DivideRoundingUp( desc.m_sizeHeight, 32 );
	desc.m_format		= GpuWrapApi::TEXFMT_R16_Uint;	// This is bcoz we need to keep one extra bit from "no volume' 9th bit

	auto rtChannelMip		= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "lightChannelMip" ), desc );

	{
		if( rctx.IsConsumePhase() )
		{
			const bool cpProbe = cpLightChan::On(); // [skperf] round 33
			const double cpT0 = cpProbe ? emscripten_get_now() : 0;
			double cpT1 = cpT0, cpT2 = cpT0; bool cpHadVolumes = false;
			GpuApi::ClearColorTarget( rtChannel, Vector4(0,0,0,0) );
			if ( cpProbe ) cpT1 = emscripten_get_now();


#ifndef RED_CONFIGURATION_FINAL
			if( !rctx.Test( rend::DF_RenderMask_LightChannels ) )
			{
				GpuApi::ClearColorTarget( rtChannelMip, Vector4( 1<<8, 0, 0, 0 ) );
				return;
			}
#endif

			if( lightVolume::HasAnyVolumes( rctx, rend::LCG_LightClippingVolume ) )
			{
				cpHadVolumes = true;

				rctx.BindPSO( ColorTarget( 0, rtChannel ),
							  DepthTarget( rtDepth, -1, true ) );

				rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

				// Render volumes
				if( auto shader = GetRenderer()->GetShader( SHADER_NAME( "m_lightChannelVolumes" ) ) )
				{
					shader->Bind();

					lightVolume::RenderVolumes( rctx, rend::LCG_LightClippingVolume );
				}
			}

			if ( cpProbe ) cpT2 = emscripten_get_now();
			// Second pass
			if( auto shader = GetRenderer()->GetShader( SHADER_NAME( "m_lightChannelVolumesMip" ) ) )
			{
				GpuApi::SetupBlankRenderTargets();

				rctx.BindTextureUAV<0>( rtChannelMip );
				rctx.BindTexture<0, eCOMPUTE>( rtChannel );
				rctx.BindTextureStencil<1, eCOMPUTE>( rtDepth );

				const GpuApi::TextureDesc& texDesc = GpuApi::GetTextureDesc( rtChannel );

				struct Com
				{
					Vector2		uInvMaskSize;
				} data;

				data.uInvMaskSize = Vector2( 1.0f / texDesc.width, 1.0f / texDesc.height );

				rctx.SetConstant<eCOMPUTE>( data );

				shader->Dispatch( desc.m_sizeWidth, desc.m_sizeHeight );

				GpuApi::BarrierTextureUAV( rtChannelMip );
			}
			if ( cpProbe )
			{
				const double cpT3 = emscripten_get_now();
				cpLightChan::Note( cpT3 - cpT0, cpT1 - cpT0, cpT2 - cpT1, cpT3 - cpT2, Uint32( rctx.GetWidth() ), Uint32( rctx.GetHeight() ), cpHadVolumes );
			}

		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderGIVolumes::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	PC_SCOPE( RenderGIVolumes );

	SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc(GpuWrapApi::TEXFMT_R8_Uint);
	desc.m_denyUnorderedAccess = true;

	auto rtChannelFinal = rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "giVolume" ), desc );
	auto rtDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );

	desc.m_denyUnorderedAccess = false;

	{
		if ( rctx.IsConsumePhase() )
		{
			GpuApi::ClearColorTarget( rtChannelFinal, Vector4( 0, 0, 0, 0 ) );

			if ( lightVolume::HasAnyVolumes( rctx, rend::LCG_GIInterior ) )
			{
				rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

				rctx.BindPSO( ColorTarget( 0, rtChannelFinal ),
							  DepthTarget( rtDepth, -1, true ) );

				rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

				// Render volumes
				if ( auto shader = GetRenderer()->GetShader( SHADER_NAME( "m_lightChannelVolumes" ) ) )
				{
					shader->Bind();

					lightVolume::RenderVolumes( rctx, rend::LCG_GIInterior );
				}
			}

		}
	}
}

///
