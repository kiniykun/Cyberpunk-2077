/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "render-job-phase-web.h"

#include "renderInterface.h"

#include "renderShadowManager.h"
#include "renderViewport.h"
#include "renderScene.h"
#include "renderPostProcess.h"
#include "particleSimulationController.h"
#include "renderPostFx.h"
#include "renderHiZ.h"
#include "clusteredDeferred.h"
#include "renderSkinManager.h"
#include "renderDebugDrawerImpl.h"
#include "renderGraphCache.h"
#include "renderGeometryBatcher.h"
#include "renderVisibilityDebugDrawer.h"
#include "visibilityScene.h"
#include "renderFrameCamera.h"
#include "renderNodeJob.h"
#include "renderVolumetricFogManager.h"
#include "gameplayFx.h"
#include "renderHairProfile.h"
#include "renderGradient.h"
#include "../../renderData/include/renderVideo.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/renderEngineHandler.h"
#include "../../renderData/include/rendererDevConfig.h"
#include "../../redMath/include/random.h"

#include "../../resourceMaterial/include/materialCompilationManager.h"

#include "renderCommandHandler.h"
#include "renderGraphNodes.h"
#include "../../renderData/include/renderGlobalBindings.h" // [skperf] round 54
#include <atomic>
#include <cstring> // [skperf] round 60
#include <new> // [skperf] round 60
#include "renderSurfaceCache.h"
#include "renderVideoPlayer.h"
#include "renderDynamicTextureManager.h"
#include "fontCache.h"
#include "renderFoliageProfile.h"
#include "renderMeshCloth.h"
#include "renderMultilayerSetup.h"

#ifdef RED_PLATFORM_ORBIS
	// HACK Gnm - for test frame purposes
	#include "../../redIO/include/redIOPublic.h"

	// used only in render tests
	#include "renderMesh.h"
#endif

#ifdef USE_DLSS
#include "../../redCore/include/globalModeInfo.h"
#include "ngx.h"
#endif

// #pragma optimize ("",off)

using red::DynArray;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

GPUAPI_API extern Float GRenderSettingsMipBias;

CRenderCollector::CRenderCollectorData GRenderCollectorData;


#include "renderGIManager.h"
#include "renderReflectionProbeManager.h"
#include "renderPostFxConeAO.h"
#include "renderIES.h"
#include "particleDPLTexture.h"

#include "../../redJobs2/include/jobRunner.h"
#include "../../renderData/include/renderingMemory.h"
#include "../../renderData/include/captureHighResolutionScreenshot.h"
#include "../../redJobs2/include/jobSystem.h"
#include "../../redJobs2/include/jobCounterOwner.h"
#include "../../redJobs2/include/jobBuilder.h"
#if defined( RED_PLATFORM_WEB )
#include <emscripten.h>
#endif
#include <cstdio>

// [skperf] round 33 reSplit (opt-in ?reSplit=N, N = 2..4; default 1 = original): the main GBuffer opaque lists
// (GBuffer_Solid, GBufferVelocity_Solid) are recorded as N command lists of the same targets, list k drawing the k-th
// slice of the sorted chunks (renderGeometryBatcher.cpp), so their recording runs on N workers instead of one.
void CpReSplitArm( Int32 k, Int32 n, Uint32 listId );
static Int32 CpReSplitN()
{
	static const Int32 n = []
	{
#if defined( RED_PLATFORM_WEB )
		Int32 v = MAIN_THREAD_EM_ASM_INT( { try { const v = Number( new URLSearchParams( location.search ).get( 'reSplit' ) || 2 ); return ( v >= 1 && v <= 4 ) ? Math.floor( v ) : 2; } catch ( e ) { return 2; } } ); // [skperf] round 54: default 2 (tc71 Badlands 44-47 -> 49-51 fps, identical); ?reSplit=1 = original
#else
		Int32 v = 2;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: reSplit=%d\n", v );
		return v;
	}();
	return n;
}
template< Int32 K > static Bool CpReSplitGBufferCond( const SRenderNodeImplContext& rctx )
{
	if ( !rctx.Test( rend::DF_RenderMask_GBuffer ) ) return false;
	if ( rctx.HasCommandList() ) CpReSplitArm( K, CpReSplitN(), rctx.GetCommandList().ID() );
	return true;
}
static CRenderNode_RenderElements::RenderConditionFunc* CpReSplitCond( Int32 k )
{
	switch ( k ) { case 0: return &CpReSplitGBufferCond< 0 >; case 1: return &CpReSplitGBufferCond< 1 >; case 2: return &CpReSplitGBufferCond< 2 >; default: return &CpReSplitGBufferCond< 3 >; }
}
static Bool CpGBufferCond( const SRenderNodeImplContext& rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
static const char* const c_cpReSplitSolidNames[ 4 ] = { "GBuffer_Solid", "GBuffer_Solid_s1", "GBuffer_Solid_s2", "GBuffer_Solid_s3" };
static const char* const c_cpReSplitVelNames[ 4 ] = { "GBufferVelocity_Solid", "GBufferVelocity_Solid_s1", "GBufferVelocity_Solid_s2", "GBufferVelocity_Solid_s3" };

// [skperf] round 52 cascSplit (opt-in ?cascSplit=N, N = 2..4; default 1 = original): each sun cascade is recorded as N
// command lists (RenderCascadeC, RenderCascadeC_s1..), list k drawing the k-th slice of that cascade's sorted chunks;
// see renderNode_Shadows.cpp (slice 0 clears + auto-spawn, last finisher clears the collector buckets).
void CpCascadeSliceSet( Int32 k, Int32 n ); // renderNode_Shadows.cpp
static Int32 CpCascSplitN()
{
	static const Int32 n = []
	{
#if defined( RED_PLATFORM_WEB )
		Int32 v = MAIN_THREAD_EM_ASM_INT( { try { const v = Number( new URLSearchParams( location.search ).get( 'cascSplit' ) || 2 ); return ( v >= 1 && v <= 4 ) ? Math.floor( v ) : 2; } catch ( e ) { return 2; } } ); // [skperf] round 53: default 2 (tc68: plain-frame chain 21.7 -> 17.4 ms, shadows identical); ?cascSplit=1 = original
#else
		Int32 v = 2;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: cascSplit=%d\n", v );
		return v;
	}();
	return n;
}
// [skperf] round 53 mirrorDecals (opt-in ?mirrorDecals=0; default 1 = original): skip the deferred-decal batches when
// rendering a mirror camera (the only cameras with a camera builder). tc68 mirror frames: DeferredDecals 3.4 ms on the
// chain. Visual change: decals (dirt, graffiti, blood) are missing from the reflection only.
static Bool CpMirrorDecalsOff()
{
	static const Bool off = []
	{
#if defined( RED_PLATFORM_WEB )
		const Int32 v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'mirrorDecals' ) === '0' ? 1 : 0; } catch ( e ) { return 0; } } );
#else
		const Int32 v = 0;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: mirrorDecals=%d\n", v ? 0 : 1 );
		return v != 0;
	}();
	return off;
}
static Bool CpDeferredDecalsCond( const SRenderNodeImplContext& rctx )
{
	if ( !rctx.Test( rend::DF_RenderMask_GBuffer ) ) return false;
	if ( CpMirrorDecalsOff() )
	{
		const auto* cpCameraData = rctx.GetCameraData();
		if ( cpCameraData && cpCameraData->m_cameraBuilder.Get() != nullptr ) return false;
	}
	return true;
}
// [skperf] round 54 decalSplit (opt-in ?decalSplit=N, N = 2..4; default 1 = original): the main "DeferredDecals" list is
// recorded as N lists (DeferredDecals, DeferredDecals_s1, ..), list k drawing the k-th slice of the priority-sorted
// decal chunks (reSplit slicing in renderGeometryBatcher.cpp, keyed by command list id). Lists are submitted in order, so
// the blend order is unchanged. The feedback normal copy (gbuffer1 -> gbuffer1Copy) is made once in slice 0 and its
// render-flow use ends in the last slice (EndGBuffer1Feedback), exactly as for the single list; the other slices only
// re-bind the copy (texture bindings are per command list). Tc70 Badlands: DeferredDecals 13-14 ms serial per frame.
static Int32 CpDecalSplitN()
{
	static const Int32 n = []
	{
#if defined( RED_PLATFORM_WEB )
		Int32 v = MAIN_THREAD_EM_ASM_INT( { try { const v = Number( new URLSearchParams( location.search ).get( 'decalSplit' ) || 1 ); return ( v >= 2 && v <= 4 ) ? Math.floor( v ) : 1; } catch ( e ) { return 1; } } );
#else
		Int32 v = 1;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: decalSplit=%d\n", v );
		return v;
	}();
	return n;
}
template< Int32 K > static Bool CpDecalSplitCond( const SRenderNodeImplContext& rctx )
{
	if ( !CpDeferredDecalsCond( rctx ) ) return false;
	if ( rctx.HasCommandList() ) CpReSplitArm( K, CpDecalSplitN(), rctx.GetCommandList().ID() );
	return true;
}
static CRenderNode_RenderElements::RenderConditionFunc* CpDecalSplitCondFor( Int32 k )
{
	switch ( k ) { case 0: return &CpDecalSplitCond< 0 >; case 1: return &CpDecalSplitCond< 1 >; case 2: return &CpDecalSplitCond< 2 >; default: return &CpDecalSplitCond< 3 >; }
}
static const char* const c_cpDecalSplitNames[ 4 ] = { "DeferredDecals", "DeferredDecals_s1", "DeferredDecals_s2", "DeferredDecals_s3" };
// [skperf] round 58 transSplit (opt-in ?transSplit=N, N = 2..4; default 1 = original): the main "Transparents" list (on the
// render chain in ~97% of Badlands frames, tc77/tc79) records its big batch (renderstage_transparent) as N contiguous
// slices of the priority-sorted chunks, slice k in list k (Transparents, Transparents_s1, ..). Lists are submitted in
// creation order, so the blend order is unchanged. Everything else (clouds, depth write, background, back faces) stays
// in slice 0 before slice 0's part of the big batch, exactly as before; back faces get their own RenderElements node so
// that only the big batch is sliced (each batch is a separate RenderGeometry call). Those nodes explicitly disarm the
// slicing. Every slice keeps the CPU dependency on DecoupledParticleLighting.
static Int32 CpTransSplitN()
{
	static const Int32 n = []
	{
#if defined( RED_PLATFORM_WEB )
		Int32 v = MAIN_THREAD_EM_ASM_INT( { try { const v = Number( new URLSearchParams( location.search ).get( 'transSplit' ) || 2 ); return ( v >= 1 && v <= 4 ) ? Math.floor( v ) : 2; } catch ( e ) { return 2; } } ); // [skperf] round 59: default 2 (tc80); ?transSplit=1 = original
#else
		Int32 v = 2;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: transSplit=%d\n", v );
		return v;
	}();
	return n;
}
static Bool CpForwardNoSliceCond( const SRenderNodeImplContext& rctx )
{
	if ( !rctx.Test( rend::DF_RenderMask_Forward ) ) return false;
	CpReSplitArm( 0, 1, 0 );
	return true;
}
template< Int32 K > static Bool CpTransSplitCond( const SRenderNodeImplContext& rctx )
{
	if ( !rctx.Test( rend::DF_RenderMask_Forward ) ) return false;
	if ( rctx.HasCommandList() ) CpReSplitArm( K, CpTransSplitN(), rctx.GetCommandList().ID() );
	return true;
}
static CRenderNode_RenderElements::RenderConditionFunc* CpTransSplitCondFor( Int32 k )
{
	switch ( k ) { case 0: return &CpTransSplitCond< 0 >; case 1: return &CpTransSplitCond< 1 >; case 2: return &CpTransSplitCond< 2 >; default: return &CpTransSplitCond< 3 >; }
}
static const char* const c_cpTransSplitNames[ 4 ] = { "Transparents", "Transparents_s1", "Transparents_s2", "Transparents_s3" };
// Re-binds the gbuffer1 feedback copy made by slice 0 (no copy, no render-flow use of its own: the copy stays allocated
// from slice 0's RTUseBegin to the last slice's RTUseEnd, which bracket these lists in graph order).
class CpRenderNode_BindFeedbackNormalCopy : public CRenderNodeBase
{
	DECLARE_RENDER_NODE( CpRenderNode_BindFeedbackNormalCopy );
public:
	virtual RenderNodeCommandListUsage GetCommandListUsage() const override { return RenderNodeCommandListUsage::Require; }
	virtual void Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const override
	{
		if ( !rctx.IsConsumePhase() ) return;
		const GpuApi::TextureRef tex = rctx.RTGetOrNull< GpuApi::TextureRef >( rctx.RTNameTag( RENDER_NAME_TAG( "gbuffer1Copy" ) ) );
		if ( tex.isNull() )
		{
			static std::atomic< Int32 > s_reported{ 0 };
			if ( s_reported.exchange( 1 ) == 0 ) std::fprintf( stderr, "CP2077 skperf decalSplit: gbuffer1Copy not found for a later slice (decals there read no feedback normals)\n" );
			return;
		}
		GpuApi::BindTextures( rend::Binding::Textures::GBUFFER1_COPY, 1, &tex, GpuWrapApi::PixelShader );
	}
};
// Name contains "RenderShadow" so the skperf node probes label it with its command list name (RenderCascade0_s1, ...).
class CpRenderNode_RenderShadowCascadeSlice : public CRenderNode_RenderShadowCascade
{
	DECLARE_RENDER_NODE( CpRenderNode_RenderShadowCascadeSlice );
	Int32 m_cpK, m_cpN;
public:
	CpRenderNode_RenderShadowCascadeSlice( Uint32 shadowCascadeIndex, Int32 k, Int32 n )
		: CRenderNode_RenderShadowCascade( shadowCascadeIndex ), m_cpK( k ), m_cpN( n )
	{}
	virtual void Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const override
	{
		CpCascadeSliceSet( m_cpK, m_cpN ); // read by the base Execute on this same call stack
		CRenderNode_RenderShadowCascade::Execute( rctx, builder );
		CpCascadeSliceSet( 0, 1 );
	}
};


#if defined( RED_PLATFORM_WIN32 ) || defined( RED_PLATFORM_WIN64 )
#define CV_ASYNC_HAIR_CLEARS_DEFAULT false
#else
#define CV_ASYNC_HAIR_CLEARS_DEFAULT true
#endif

namespace Config
{
#ifdef USE_DLSS
	TConfigVar<Bool> cvDLSSEnableCustomMipBias( "DLSS", "EnableCustomMipBias", true, eConsoleVarFlag_Developer );
#endif // USE_DLSS
#ifdef USE_RAY_TRACING
	TConfigVar<Bool> cvAsyncRayTracingASBuild( "Rendering/AsyncCompute", "RaytraceASBuild", true, eConsoleVarFlag_Developer );
#endif // USE_RAY_TRACING
	TConfigVar<Bool> cvAsyncComputeEnable( "Rendering/AsyncCompute", "Enable", true, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncHairClears( "Rendering/AsyncCompute", "HairClears", CV_ASYNC_HAIR_CLEARS_DEFAULT, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncSSAO( "Rendering/AsyncCompute", "SSAO", true, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncLutGeneration( "Rendering/AsyncCompute", "LutGeneration", true, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncDynamicTextureGeneration( "Rendering/AsyncCompute", "DynamicTexture", true, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncFlattenNormals( "Rendering/AsyncCompute", "FlattenNormals", true, eConsoleVarFlag_Developer );
	TConfigVar<Bool> cvAsyncBuildDepthChain( "Rendering/AsyncCompute", "BuildDepthChain", true, eConsoleVarFlag_Developer );

	// #TODO: Better way to detect changes here... Should we just put 
#ifdef USE_RAY_TRACING
	static Bool previousAsyncRayTracingASBuild = true;
#endif // USE_RAY_TRACING
	static Bool previousAsyncComputeEnable = true;
	static Bool previousAsyncHairClears = true;
	static Bool previousAsyncSSAO = true;
	static Bool previousAsyncLutGeneration = true;
	static Bool previousAsyncDynamicTextureGeneration = true;
	static Bool previousAsyncFlattenNormals = true;
	static Bool previousAsyncBuildDepthChain = true;
}

class RenderTexturePreviewData : public ISceneStorageCustomData
{
	RTTI_DECLARE_TYPE( RenderTexturePreviewData );
	DECLARE_CUSTOM_SCENE_DATA( 3 );

public:
	virtual void Initialize() override {}
	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData ) override {}
	virtual void Evict() override {}

	red::LightMutex							m_lock;

	red::DynArray<SDynamicPreviewTexture>	m_dynamicTexturePreview{ rend::PoolRenderDebug() };
	red::DynArray<SDynamicPreviewTexture>	m_dynamicTexturePreviewFill{ rend::PoolRenderDebug() };
};

RTTI_BEGIN_TYPE( RenderTexturePreviewData )
{
	RTTI_PARENT_TYPE( ISceneStorageCustomData );
}
RTTI_END_TYPE();


GPUFrameTimer::GPUFrameTimer()
{
	Reset();
}

// on XBox you could possibly get this very slightly more light weight by using Write64BitValueBottomOfPipeX, 
// D3D12XBOX_WRITE_VALUE_BOP_FLAG_GPU_TIMESTAMP, but hardly worth the engineering effort, keep to common code
void GPUFrameTimer::Initialize()
{
	const Uint32 numQueries = (Uint32)MarkerType::Count * MaxScenes * MaxFrames;
	m_queryPool = GpuApi::CreateQueryPool( GpuApi::QueryType::Timestamp, numQueries );
}

void GPUFrameTimer::Reset()
{
	m_queryPool = nullptr;
	m_frameIndex = 0;
	m_frequency = 0.0f;

	m_lastMainSceneTime = 0.0f;
	m_lastSubsceneTime = 0.0f;
	m_lastDrawBuffersTime = 0.0f;
}

void GPUFrameTimer::Update()
{
	PC_SCOPE( UpdateGpuFrameTimer );

	m_pastFrameInfos[ m_frameIndex % MaxFrames ] = m_currentFrameInfo;

	// resolve accumulated queries
	{
		const Uint32 frameIndex = Uint32( m_frameIndex % MaxFrames );
		const Uint32 baseIndex = frameIndex * (Uint32)MarkerType::Count * MaxScenes;
		const Uint32 numScenes = m_currentFrameInfo.scenes.Size() + 1;
		GpuApi::ResolveQueries( m_queryPool, baseIndex, numScenes * (Uint32)MarkerType::Count );
	}
	
	// move to next frame
	m_currentFrameInfo.drawBufferIssued = false;
	m_currentFrameInfo.scenes.Clear();
	m_frameIndex++;

	if ( m_frequency <= 0.0f )
	{
		Uint64 frequencyHz = GpuApi::GetGpuFrequency( GpuApi::GetCurrentGpuThread() );

		if ( !frequencyHz )
		{
			return;
		}

		m_frequency = 1.0f / Float(frequencyHz);
	}

	const Uint32 pastFrameIndex = Uint32( m_frameIndex % MaxFrames );
	const Uint32 baseIndex = pastFrameIndex * (Uint32)MarkerType::Count * MaxScenes;
	const FrameInfo& pastFrameInfo = m_pastFrameInfos[ pastFrameIndex ];

	if ( GpuApi::AcquireQueries( m_queryPool, baseIndex, ( pastFrameInfo.scenes.Size() + 1 ) * (Uint32)MarkerType::Count ) ) // +1 for draw buffers
	{
		m_lastMainSceneTime = 0.0f;
		m_lastSubsceneTime = 0.0f;
		m_lastDrawBuffersTime = 0.0f;

		if ( pastFrameInfo.drawBufferIssued )
		{
			Uint64 startTime = 0, endTime = 0;
			GpuApi::GetQueryResult( m_queryPool, baseIndex + 0, startTime );
			GpuApi::GetQueryResult( m_queryPool, baseIndex + 1, endTime );

			if ( endTime >= startTime && startTime > 0 )
			{
				m_lastDrawBuffersTime += m_frequency * Float( endTime - startTime );
			}
		}

		for ( Uint32 i = 0; i < pastFrameInfo.scenes.Size(); ++i )
		{
			Uint64 startTime = 0, endTime = 0;
			GpuApi::GetQueryResult( m_queryPool, baseIndex + ( i + 1 ) * (Uint32)MarkerType::Count + 0, startTime );
			GpuApi::GetQueryResult( m_queryPool, baseIndex + ( i + 1 ) * (Uint32)MarkerType::Count + 1, endTime );

			if ( endTime >= startTime && startTime > 0 )
			{
				const Float t = m_frequency * Float( endTime - startTime );

				// accumulate to proper bucket
				switch ( pastFrameInfo.scenes[ i ] )
				{
				case RenderType::MainScene:
					m_lastMainSceneTime += t;
					break;
				case RenderType::Subscene:
					m_lastSubsceneTime += t;
					break;
				default:
					RED_FATAL( "Invalid render type" );
				}
			}
		}
	}
	GpuApi::ReleaseQueries( m_queryPool );
}

void GPUFrameTimer::BeginSceneRender( RenderType renderType )
{
	Uint32 sceneIndex = 0;
	if ( renderType == RenderType::MainScene || renderType == RenderType::Subscene )
	{
		if ( m_currentFrameInfo.scenes.Full() )
		{
			// apparently we can have missing frame tick between multiple subscene renderings, so just silently skip it....
			return;
		}

		sceneIndex = m_currentFrameInfo.scenes.Size() + 1; // scene 0 is reserved for draw buffers
	}

	GpuApi::IssueQuery( m_queryPool, GetTimerIndex( sceneIndex, m_frameIndex % MaxFrames, MarkerType::Begin ) );
}

void GPUFrameTimer::EndSceneRender( RenderType renderType )
{
	Uint32 sceneIndex = 0;
	if ( renderType == RenderType::MainScene || renderType == RenderType::Subscene )
	{
		sceneIndex = m_currentFrameInfo.scenes.Size() + 1; // scene 0 is reserved for draw buffers

		if ( m_currentFrameInfo.scenes.Full() )
		{
			// apparently we can have missing frame tick between multiple subscene renderings, so just silently skip it....
			return;
		}

		m_currentFrameInfo.scenes.PushBack( renderType );
	}
	else
	{
		m_currentFrameInfo.drawBufferIssued = true;
	}

	GpuApi::IssueQuery( m_queryPool, GetTimerIndex( sceneIndex, m_frameIndex % MaxFrames, MarkerType::End ) );
}



void CRenderInterface::PushTexturePreview( const SDynamicPreviewTexture& desc, CRenderSceneEx* scene /*= nullptr*/ )
{
	if( desc.m_texture.isNull() == false )
	{
		GpuApi::AddRef( desc.m_texture );

		if( scene )
		{
			auto& data = scene->GetCameraStorage()->GetCustomData<RenderTexturePreviewData>();

			RED_SCOPE_LOCK( data.m_lock );
			data.m_dynamicTexturePreview.PushBack( desc );
		}
		else
		{
			RED_SCOPE_LOCK( m_dynamicTextureLock );
			m_dynamicTexturePreview.PushBack( desc );
		}
	}
}

void CRenderInterface::PushTexturePreviewFill( const SDynamicPreviewTexture& settings, CRenderSceneEx* scene /*= nullptr*/ )
{
	if ( !settings.m_texture.isNull() )
	{
		GpuApi::AddRef( settings.m_texture );

		if( scene )
		{
			auto& data = scene->GetCameraStorage()->GetCustomData<RenderTexturePreviewData>();

			RED_SCOPE_LOCK( data.m_lock );
			data.m_dynamicTexturePreviewFill.PushBack( settings );
		}
		else
		{
			RED_SCOPE_LOCK( m_dynamicTextureLock );
			m_dynamicTexturePreviewFill.PushBack( settings );
		}
	}
}



void CRenderInterface::RenderTexturePreview(const SRenderNodeImplContext& rctx)
{
	if( rctx.HasAnyCameraStorage() )
	{
		auto& texturePreviews = rctx.GetCameraStorage()->GetCustomData<RenderTexturePreviewData>();

		RED_SCOPE_LOCK( texturePreviews.m_lock );
		RED_SCOPE_LOCK( m_dynamicTextureLock );

		m_dynamicTexturePreview.PushBack( texturePreviews.m_dynamicTexturePreview );
		texturePreviews.m_dynamicTexturePreview.Clear();

		m_dynamicTexturePreviewFill.PushBack( texturePreviews.m_dynamicTexturePreviewFill );
		texturePreviews.m_dynamicTexturePreviewFill.Clear();
	}
	
	if( !FramePurpose::IsNormal( rctx.GetFrameInfo().GetFramePurpose() ) )
		return;

	if ( rctx.Test( rend::DF_GlyphCache ) )
	{
		PushTexturePreview( SDynamicPreviewTexture( GetGlyphCache()->GetFontTexture() ).Channel( 0 ) );
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 

	if( rctx.Test( rend::DF_EnvProbe_Debug_ScreenPreview ) && rctx.HasAnyCameraStorage() )
	{
		constexpr Float MAX_VALUE = 1.0f;

		auto& sceneData = rctx.GetCameraStorage()->GetCustomData<SceneCustomData_ReflectionAtlas>();

		if( auto atlas = sceneData.GetAtlas( false ) )
		{
			// All slots
			{
				for( const auto& stage : sceneData.GetStages() )
				{
					PushTexturePreview( SDynamicPreviewTexture( stage->GetIntermediate() ).Mip( 0 ).Range( 0.0f, MAX_VALUE ).Channels(1,1,1).Tonemap() );
				}

				m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );

				const EngineTime now = EngineTime::GetNow();

				for( Uint32 i = 0; i < atlas->GetNumSlices(); ++i )
				{
					if(atlas->IsSlotActive( i ))
					{
						const EngineTime lastRenderTime = atlas->GetLastRenderTime( i );
						const Vector4 border( 0.0f, 1.0f, 0.0f, 1.0f - Clamp<Float>( (Float)((now - lastRenderTime) / 0.5f), 0.0f, 1.0f ) );

						PushTexturePreview( SDynamicPreviewTexture( atlas->GetTexture() ).Mip( 0 ).Slice( i ).Range( 0.0f, MAX_VALUE ).Rgb().Border( border ).Tonemap() );
					}
					else
					{
						PushTexturePreview( SDynamicPreviewTexture( atlas->GetTexture() ).Mip( 0 ).Slice( i ).Range( 0.0f, MAX_VALUE ).Rgb().Border( Vector4(1,0,0,0.5f) ).Tonemap() );
					}
				}

				m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );
			}

			// Draw dynamic relitter
			if( const auto litTex = sceneData.GetRelighter()->GetLitTexture() )
			{
				const auto& desc = GpuApi::GetTextureDesc( litTex );

				for( Uint32 face = 0; face < 6; ++face )
				{
					PushTexturePreview( SDynamicPreviewTexture( litTex ).Slice( GpuApi::CalculateCubemapSliceIndex( desc, 0, face, 0 ) ).Range( 0.0f, MAX_VALUE ).Rgb().Tonemap() );
				}
				for( Uint32 face = 0; face < 6; ++face )
				{
					PushTexturePreview( SDynamicPreviewTexture( litTex ).Slice( GpuApi::CalculateCubemapSliceIndex( desc, 0, face, 3 ) ).Range( 0.0f, MAX_VALUE ).Rgb().Tonemap() );
				}

				// Row breaker
				m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );
			}

			// Allocated cubemaps
			// for( const auto& slot : m_reflectionProbeManager->GetSlots() )
			// {
			// 	if( slot.second.m_cubemapAlbedo )
			// 	{
			// 		for( Uint32 face = 0; face < 6; ++face )
			// 		{
			// 			PushTexturePreview( slot.second.m_cubemapAlbedo, 0, face, 0.0f, 1.0f, 1 | 2 | 4, 3 );
			// 		}
			// 		for( Uint32 face = 0; face < 6; ++face )
			// 		{
			// 			PushTexturePreview( slot.second.m_cubemapData, 0, face, 0.0f, 1.0f, 1 | 2, 2 );
			// 		}
			// 		// Row breaker
			// 		m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );
			// 	}
			// }

			// Sky
			{
				const auto skyTex = sceneData.GetRelighter()->GetSkyTexture();
				const auto& desc = GpuApi::GetTextureDesc( skyTex );

				for( Uint32 face = 0; face < 6; ++face )
				{
					PushTexturePreview( SDynamicPreviewTexture( skyTex ).Slice( GpuApi::CalculateCubemapSliceIndex( desc, 0, face, 0 ) ).Range( 0.0f, MAX_VALUE ).Rgb().Tonemap() );
				}
			}

			// Sky Parabolid
			{
				const auto skyTex = sceneData.GetRelighter()->GetSkyParaboloidTexture();
				const auto& desc = GpuApi::GetTextureDesc( skyTex );

				PushTexturePreview( SDynamicPreviewTexture( skyTex ).Range( 0.0f, MAX_VALUE ).Rgb().Tonemap() );
			}


		}
	}
	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 
	if( rctx.Test( rend::DF_Render_Debug_Textures ) )
	{
		for( Uint32 id = 0; id < GpuApi::INTERTEX_Max; ++id )
		{
			const auto ref = GpuApi::GetInternalTexture( GpuApi::eInternalTexture( id ) );
			const auto& desc = GpuApi::GetTextureDesc( ref );

			if( desc.type == GpuWrapApi::TEXTYPE_CUBE )
			{
				for( Uint32 f = 0; f < 6; ++f )
				{
					for( Uint32 i = 0; i < desc.numMips; ++i )
					{
						PushTexturePreview( SDynamicPreviewTexture( ref ).Mip( i ).Slice( GpuApi::CalculateCubemapSliceIndex( desc, 0, f, 0 ) ) );
					}
				}
			}
			else
			{
				for( Uint32 i = 0; i < desc.numMips; ++i )
				{
					PushTexturePreview( SDynamicPreviewTexture( ref ).Mip( i ) );
				}
			}
			m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );
		}
	}

	// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- 

	if ( rctx.Test( rend::DF_SurfaceCacheAtlas ) )
	{
		PushTexturePreview( SDynamicPreviewTexture( m_surfaceCache->GetMLBaseColorAtlas() ).Rgb() );	// BaseColor
		PushTexturePreview( SDynamicPreviewTexture( m_surfaceCache->GetMLNormalAtlas()    ).Rgb() );		// Normal
		PushTexturePreview( SDynamicPreviewTexture( m_surfaceCache->GetMLBaseColorAtlas() ).Alpha() );	// Roughness
		PushTexturePreview( SDynamicPreviewTexture( m_surfaceCache->GetMLNormalAtlas()    ).Alpha() );		// Metalness
		m_dynamicTexturePreview.PushBack( SDynamicPreviewTexture() );
	}

	RED_SCOPE_LOCK( m_dynamicTextureLock );

	if (false == m_dynamicTexturePreview.Empty())
	{
		Uint32 x = 50;
		Uint32 y = 50;

		const Float  denom = std::sqrt( (Float)m_dynamicTexturePreview.Size() );
		const Uint32 count = Min( rctx.GetFrameInfo().GetViewportWidth(), rctx.GetFrameInfo().GetViewportHeight() ) - 100u;

		const Uint32 previewSize = Clamp<Uint32>( red::RoundDownToPowerOf2( static_cast< Uint32 >(count / denom) ), 32u, 256u );

		for (const auto& it : m_dynamicTexturePreview)
		{
			if (it.m_texture.isNull())
			{
				x = UINT32_MAX - previewSize;	// Force to go to the next row
			}
			else
			{
				SDynamicPreviewTexture settings = it;
				settings.Rect( Rect::Make( x, y, previewSize, previewSize ) );
				m_debugDrawer->DrawTexturePreviewTile( rctx, settings );

				x += previewSize + 1;

				GpuApi::Release(it.m_texture);
			}

			// Go to the next row if outside right side
			if (x + previewSize > (Uint32)rctx.GetFrameInfo().GetViewportWidth()) { x = 50; y += previewSize + 1; }
		}

		m_dynamicTexturePreview.Clear();
	}
	
	if( false == m_dynamicTexturePreviewFill.Empty() )
	{
		const auto viewportW = rctx.GetFrameInfo().GetViewportWidth();
		const auto viewportH = rctx.GetFrameInfo().GetViewportHeight();

		for( const auto& it : m_dynamicTexturePreviewFill )
		{
			const auto& desc = GpuApi::GetTextureDesc( it.m_texture );

			Rect placementRect = it.m_rect;
			if ( placementRect.IsEmpty() )
			{
				const Float ratio = Min( Float( viewportW ) / Float( desc.width ), Float( viewportH ) / Float( desc.height ) );
				const auto texW = static_cast< Uint32 >( desc.width * ratio );
				const auto texH = static_cast< Uint32 >( desc.height * ratio );

				placementRect = Rect::Make( ( viewportW - texW ) / 2, ( viewportH - texH ) / 2, texW, texH );
			}

			SDynamicPreviewTexture settings = it;
			settings.Rect( placementRect );
			settings.FlipVertical();
			m_debugDrawer->DrawTexturePreviewTile( rctx, settings );

			GpuApi::Release( it.m_texture );
		}

		m_dynamicTexturePreviewFill.Clear();
	}
}

Uint32 CRenderInterface::GetCascadesNumInFrustrum( const CRenderCamera& camera, const CRenderFrameInfo &info )
{
	// Split array, take into consideration cascades that are closer that camera far plane
	Uint16 effectiveCascadesNum = 0;
	const Float farPlane = camera.GetFarPlane();
	for ( Uint32 i = 0; i < info.m_requestedNumCascades; ++i )
	{
		++effectiveCascadesNum;
		if( farPlane < info.GetCascadeDistance( i ) ) 
		{
			break;
		}
	}

	return Clamp<Uint32>( effectiveCascadesNum, 1, Config::GetCascadedShadowsMaxCount() );
}

RenderFeatureSet CRenderInterface::CreateRenderFeatureSet( const CRenderFrameCameraStorage::SCameraData& cameraData, const CRenderFrameInfo* frameInfo ) const
{
	// TODO: rewrite all DF_ (debug filters usage) to Config::GGlobalRenderingSettings usage.
	// TODO: same goes for the trivial "true" accepts. Consider removing them or adding GGlobalRenderingSettings usage.

	RenderFeatureSet set;

	const CRenderCameraFrameInfo& info = cameraData.GetInfo();

	// Do nothing ...
	if( !IRenderScene::IsValid( info.m_renderScene ) )
	{
		return set;
	}

	const auto& rendMask	= info.GetRenderingMask();

	auto framePurpose = frameInfo->GetFramePurpose();

	// Extraction features
	set.Set( RFF_DepthExtraction, static_cast< Bool >( frameInfo->m_depthResult ) );
	set.Set( RFF_GBufferExtraction, frameInfo->m_gBufferAlbedo8888Result || frameInfo->m_gBufferAlbedo1010102Result ||
		frameInfo->m_gBufferMaterialResult || frameInfo->m_gBufferNormalsResult || frameInfo->m_giVolumeResult );
	set.Set( RFF_ColorExtraction, frameInfo->m_colorResult || frameInfo->m_colorResultHDR );
	set.Set( RFF_BackgroundDump, static_cast< Bool >( frameInfo->m_backgroundDump ) );
	set.Set( RFF_ImmediateMode, framePurpose != FP_GlobalIllumination && framePurpose != FP_GlobalIlluminationBackfaces );

	if ( frameInfo->GetRenderingMode() == ERenderingMode::RM_GBufferOnly )
	{
		return set;
	}

	const Bool isSubScene			= (framePurpose == EFramePurpose::FP_NormalSubscene) && (info.m_sceneLayer == RenderSceneLayer::Default);
	const Bool hasPostProcesses		= true;
	const Bool isShadedRenderMode	= frameInfo->GetRenderingMode() <= RM_ShadedMax;
	const Bool isPickingPass		= RM_HitProxies == frameInfo->GetRenderingMode();
	const Bool hasLightingPass		= frameInfo->m_displaySettings.HasDetailedView();
	const Bool hasShadowPass		= true;	// Can't reason about this. We will get reflection probes ( possibly GI too ) relight without shadows and have to restabilize after getting back to EMM_None
	const Bool isThumbnail			= info.m_renderScene->GetType() == IRenderScene::Type::THUMBNAIL;
	const Bool isWorldRenderScene	= info.m_renderScene->GetType() == IRenderScene::Type::WORLD;
	const Bool isNormalFrame		= FramePurpose::IsNormal( framePurpose );
	const Bool isScreenShot			= FramePurpose::IsScreenshot( framePurpose ) && ( frameInfo->m_colorResult || frameInfo->m_colorResultHDR );
	const Bool isNormalScreenshot	= isScreenShot && !frameInfo->m_highResoulutionScreenshotInterface && frameInfo->m_colorResult;
	const Bool isHitProxyDebug		= frameInfo->m_displaySettings.m_displayMode == EMM_DebugHitProxies;
	const Bool isNormalOrScreenshot = isNormalFrame || isNormalScreenshot;
	const Bool isNormalOrHQScreenshot = isNormalFrame || isScreenShot;
	const Bool isNormalOrAnyScreenshot = isNormalFrame || isNormalScreenshot || isScreenShot;

	{
		set.Set( RFF_HUD, true );
		set.Set( RFF_EnableAsyncCompute, true );
		set.Set( RFF_Hair, Config::cvHair.Get() );
		set.Set( RFF_SubsurfaceScattering, Config::cvCharacterSubsurface.Get() );
		set.Set( RFF_SkinTranslucency, Config::cvCharacterSubsurfaceTranslucency.Get() );
		set.Set( RFF_RuntimeTangentUpdate, Config::GGlobalRenderingSettings.allowRuntimeTangentUpdate && Config::cvRuntimeTangentUpdate.Get() );
		set.Set( RFF_DynamicDecals, Config::cvDynamicDecals.Get() );
		set.Set( RFF_MirrorMaterial, Config::GGlobalRenderingSettings.allowMirrorMaterial );
		set.Set( RFF_FocusMode, true );
		set.Set( RFF_RainMap, !isSubScene && Config::GGlobalRenderingSettings.allowRainMap && Config::cvRainMap.Get() );
	}

	// Postprocess related
	if ( hasPostProcesses )
	{
		// TODO: add config vars to allow switching some things off/on in runtime. NOT THROUGH DEBUG FILTERS!!!
		// Used always
		set.Set( RFF_Tonemapping, true );
		set.Set( RFF_Wrinklemaps, true );
		set.Set( RFF_ReflectionProbes, Config::GGlobalRenderingSettings.allowReflectionProbes );

		set.Set( RFF_CharacterRimEnhancement,	Config::cvCharacterRim.Get() );
		set.Set( RFF_CharacterLightBlockers,	Config::cvCharacterLightBlockers.Get() && GpuApi::GetDeviceType() == GpuApi::eDeviceType::ePC );
		set.Set( RFF_ScreenSpaceUnderwater,		Config::cvScreenSpaceUnderwater.Get());// || info.m_shouldRenderUnderwater );
		set.Set( RFF_ScreenSpaceHeatHaze,		Config::cvScreenSpaceHeatHaze.Get() && GpuApi::GetDeviceType() == GpuApi::eDeviceType::ePC );
		set.Set( RFF_ScreenSpaceRain,			Config::GGlobalRenderingSettings.allowAtmosphere && Config::cvScreenSpaceRain.Get() && Config::cvWeather.Get() );

		if( hasShadowPass )
		{
			set.Set( RFF_CascadeShadows,	!isSubScene );
			set.Set( RFF_DistantShadows,	!isSubScene && Config::GGlobalRenderingSettings.allowDistantShadows && Config::cvDistantShadows.Get() );
			set.Set( RFF_LocalShadows,		Config::GGlobalRenderingSettings.allowLocalShadows && Config::cvLocalShadows.Get() );
		}

		Bool enableRayTracedAmbientOcclusion = false;
		Bool enableRayTracedDiffuseIllumination = false;
		Bool enableRayTracedReflection = false;
		Bool enableRayTracedTransparentReflection = false;
		Bool enableRayTracedDirectionalShadow = false;
		Bool enableRayTracingNRD = false;

		Bool enableRayTracingDebugMode = false;
		Bool enableRayTracing = Config::GGlobalRenderingSettings.allowRaytracing;
#ifndef RED_CONFIGURATION_FINAL
		if (Config::cvRayTracingOverrideEnable.Get())
		{
			enableRayTracingDebugMode = Config::cvRayTracingDebugMode.Get() != 0;
			enableRayTracing = Config::cvRayTracingEnable.Get();
		}
#endif

#ifdef USE_RAY_TRACING
		Bool accelerationStructureResourcesAreResident = GpuApi::MakeResidentAccelerationStructureResources();
		if ( GpuApi::GetCapabilities().rayTracingSupport && enableRayTracing && accelerationStructureResourcesAreResident )
		{
			const Bool isMainCamera = ( framePurpose == FP_Normal || FramePurpose::IsScreenshot( framePurpose ) ) && cameraData.IsOnscreen();

			// Use ray tracing only with IRenderScene::Type::WORLD to avoid interference with other scene types
			// (e.g. mesh editor window will not use ray tracing)
			if (isWorldRenderScene && isShadedRenderMode && !enableRayTracingDebugMode )
			{
				if (isMainCamera)
				{
#ifndef RED_CONFIGURATION_FINAL
					if ( Config::cvRayTracingOverrideEnable.Get() )
					{
						enableRayTracedAmbientOcclusion = Config::cvRayTracingEnableAmbientOcclusion.Get();
						enableRayTracedDiffuseIllumination = Config::cvRayTracingEnableDiffuseIllumination.Get();
						enableRayTracedReflection = Config::cvRayTracingEnableReflection.Get();
						enableRayTracedTransparentReflection = Config::cvRayTracingEnableTransparentReflection.Get();
					}
					else
#endif
					{
						enableRayTracedAmbientOcclusion = Config::GGlobalRenderingSettings.RTX_EnableAmbientOcclusion;
						enableRayTracedDiffuseIllumination = Config::GGlobalRenderingSettings.RTX_EnableDiffuseIllumination;
						enableRayTracedReflection = Config::GGlobalRenderingSettings.RTX_EnableReflections;
						enableRayTracedTransparentReflection = Config::GGlobalRenderingSettings.RTX_EnableTransparentReflections;
					}

					enableRayTracingNRD = Config::cvRayTracingEnableNRD.Get();
				}

#ifndef RED_CONFIGURATION_FINAL
				if ( Config::cvRayTracingOverrideEnable.Get() )
				{
					enableRayTracedDirectionalShadow = hasShadowPass && Config::cvRayTracingEnableDirectionalShadow.Get();
				}
				else
#endif
				{
					enableRayTracedDirectionalShadow = hasShadowPass && Config::GGlobalRenderingSettings.RTX_EnableDirectionalShadow;
				}
			}
		}
#endif

		if ( ( !isThumbnail && isShadedRenderMode && hasLightingPass ) || (frameInfo->m_displaySettings.m_displayMode == EMM_RayTracingDebug) )
		{
			const Bool enableSSAO = Config::GGlobalRenderingSettings.allowSSAO && Config::cvSSAO.Get();

			set.Set( RFF_SimpleFog,			isSubScene && Config::GGlobalRenderingSettings.allowSimpleFog );
			set.Set( RFF_VolumetricFog,		!isSubScene && Config::GGlobalRenderingSettings.allowVolumetricFog && Config::cvVolumetricFog.Get() );
			set.Set( RFF_DistantFog,		!isSubScene && Config::GGlobalRenderingSettings.allowDistantFog && Config::cvDistantFog.Get() );
			set.Set( RFF_DistantVolumetricFog, Config::GGlobalRenderingSettings.allowDistantVolFog && Config::cvDistantVolFog.Get() );
			set.Set( RFF_RayTracedAmbientOcclusion, enableRayTracedAmbientOcclusion );
			set.Set( RFF_RayTracedDiffuseIllumination, enableRayTracedDiffuseIllumination );
			set.Set( RFF_RayTracedReflection, enableRayTracedReflection );
			set.Set( RFF_RayTracedTranparentReflection, enableRayTracedTransparentReflection );
			set.Set( RFF_RayTracedDirectionalShadow, enableRayTracedDirectionalShadow );
			set.Set( RFF_RayTracingNRD,		enableRayTracingNRD );
			set.Set( RFF_RayTracingDebug,	enableRayTracingDebugMode );

			set.Set( RFF_FilmGrain,			Config::GGlobalRenderingSettings.AllowFilmGrain && isNormalOrScreenshot && Config::cvFilmGrain.Get() );
			set.Set( RFF_VolumetricClouds,	Config::GGlobalRenderingSettings.allowVolumetricClouds && Config::cvVolumetricClouds.Get() );
			
			set.Set( RFF_ScreenSpaceReflections, Config::GGlobalRenderingSettings.allowSSR && Config::cvSSR.Get() );
			set.Set( RFF_ScreenSpacePlanarReflections, set.Get( RFF_ScreenSpaceReflections ) && Config::cvSSR_Planar.Get() ); // It depends fully on regular SSR
			set.Set( RFF_Bloom,				Config::GGlobalRenderingSettings.allowBloom && Config::cvBloom.Get() );
			set.Set( RFF_ImageBasedFlares,	( !isSubScene && isNormalOrAnyScreenshot && info.m_renderScene->GetType() == IRenderScene::Type::WORLD ) && Config::GGlobalRenderingSettings.allowImageBasedFlares && Config::cvImageBasedFlares.Get() );
			set.Set( RFF_DepthOfField,		Config::GGlobalRenderingSettings.allowDOF && Config::cvDepthOfField.Get() );
			set.Set( RFF_CutsceneDOF,		true );		// Not implemented
			set.Set( RFF_Sharpen,			isNormalOrScreenshot );	// Not implemented
			set.Set( RFF_Vignette,			isNormalOrAnyScreenshot );
			set.Set( RFF_SSAO,				enableSSAO);
			set.Set( RFF_MotionBlur,		Config::GGlobalRenderingSettings.allowMotionBlur && Config::cvMotionBlur.Get() );
			set.Set( RFF_ChromaticAberration, isNormalOrScreenshot && Config::GGlobalRenderingSettings.allowChromaticAberration && Config::cvChromaticAberration.Get() );
			set.Set( RFF_Selection,			info.GetRenderingMask().Test( rend::DF_Selection ) && isNormalFrame );
			set.Set( RFF_ContactShadows_Global, !isSubScene && Config::GGlobalRenderingSettings.allowContactShadows_Global && Config::cvContactShadows.Get() );
			set.Set( RFF_ContactShadows_Local, Config::GGlobalRenderingSettings.allowContactShadows_Local && Config::cvContactShadows.Get() );
			set.Set( RFF_Atmosphere,		Config::GGlobalRenderingSettings.allowAtmosphere );

#ifdef USE_DLSS
			Bool isDLSSEnabled = GetNGX() && GetNGX()->IsDLSSEnabled() && cameraData.IsOnscreen() && !isHitProxyDebug && ( isNormalFrame || isNormalScreenshot ) && Config::Helper::IsAntialiasOn();
#else
			Bool isDLSSEnabled = false;
#endif
			// DLSS preempts any other way of antialiasing and supersampling. If DLSS is enabled, they have to be disabled.
			if ( isDLSSEnabled )
			{
				set.Set( RFF_DLSS, true );
			}
			else
			{
				set.Set( RFF_AntialiasingFXAA, Config::GGlobalRenderingSettings.allowFXAA && isNormalOrScreenshot && Config::Helper::IsAntialiasOn() );
				set.Set( RFF_AntialiasingTXAA, Config::GGlobalRenderingSettings.allowTXAA && isNormalOrScreenshot && Config::Helper::IsAntialiasOn() );
				set.Set( RFF_ContrastAdaptiveSharpening, isShadedRenderMode && !isHitProxyDebug );
			}
		}
	}

	// Global features
	if( !isThumbnail )
	{
		if( !isSubScene )
		{
			set.Set( RFF_GlobalIlluminationCompute, Config::GGlobalRenderingSettings.allowGI && Config::cvGlobalIllumination.Get() );
			set.Set( RFF_GlobalIllumination, Config::GGlobalRenderingSettings.allowGI && Config::cvGlobalIllumination.Get() && GetRenderer()->GetGIManager() );
			set.Set( RFF_DistantGI, Config::GGlobalRenderingSettings.allowDistantGI && Config::cvDistantGI.Get() && (GetRenderer()->GetDistantGIManager() && GetRenderer()->GetDistantGIManager()->HasValidData()) );
		}
		set.Set( RFF_EyeAdaptation, true ); // user can control it in weather system and it doesn't really make sense as a quality option so let's not bloat the options with this
	}
	
	if ( frameInfo && frameInfo->m_displaySettings.m_displayMode == EMM_VelocityBuffer )
	{
		set.Set( RFF_VelocityBuffer, true );
	}

	// Miscellaneous features
	set.Set( RFF_DebugColoring,		frameInfo->m_enableDebugMeshColoring );

	// These flags are mostly true unless we explicitly set them later to false (e.g. mirror scene rendering)
	set.Set(RFF_GlobalLight, !frameInfo->m_isTimeShiftGlitchPrevention && !isSubScene );
	set.Set(RFF_Particles, true);

	set &= cameraData.GetInfo().m_allowedFeatureFlags;

	// Dependent features
	{
		if (set.Get( RFF_RayTracedAmbientOcclusion ))
		{
			set.Set( RFF_SSAO, false );
		}

		if (set.Get( RFF_RayTracedReflection ))
		{
			set.Set( RFF_ScreenSpaceReflections, false );
			set.Set( RFF_ScreenSpacePlanarReflections, false );
		}

		if (set.Get( RFF_MotionBlur ) ||
			set.Get( RFF_SSAO ) ||
			set.Get( RFF_AntialiasingTXAA ) ||
			set.Get( RFF_ScreenSpaceReflections ) ||
			set.Get( RFF_RayTracedAmbientOcclusion ) ||
			set.Get( RFF_RayTracedDiffuseIllumination ) ||
			set.Get( RFF_RayTracedReflection ) ||
			set.Get( RFF_RayTracedTranparentReflection ) ||
			set.Get( RFF_DLSS ))
		{
			set.Set( RFF_VelocityBuffer, true );
		}

		// We use a combined pass to do those (relying on velocity buffer BTW)
		// So let's derive weapon blur from motion blur. Not ideal, but helps perf (0.5ms gain).
		// Can revisit later if this really is an issue.
		if ( set.Get( RFF_MotionBlur ) )
		{
			set.Set( RFF_WeaponPlaneBlur, true );
		}

#ifdef RENDER_LET_CAS_CLEAR_COMPOSITION
		if ( set.Get( RFF_HUD ) && set.Get( RFF_ContrastAdaptiveSharpening ) )
		{
			set.Set( RFF_ClearCompositionDuringCAS, true );
		}
#endif
	}

	return set;
}

void CRenderInterface::RenderFullscreenVideo( const SRenderNodeImplContext& rctx )
{
	auto rtFinal = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );

	if ( m_videoPlayer->HasFullscreenVideo() )
	{
		// Setup global camera parameters
		if ( rctx.IsConsumePhase() )
		{
			const CRenderFrameInfo &info = rctx.GetFrameInfo();
			const Uint32 width = info.GetViewportWidth();
			const Uint32 height = info.GetViewportHeight();

			rctx.BindPSO( ColorTarget( 0, rtFinal ) );

			RenderBindManager::SetCamera2D(rctx, width, height );

			{
				PC_SCOPE( RenderVideo );
				PC_GPU_SCOPE( FullscreenVideo );
				m_videoPlayer->RenderFullscreenVideo( rctx );
			}

			m_stateManager->Reset();
		}
	}
}

void CRenderInterface::Render2D( const SRenderNodeImplContext& rctx, EFeatureFlag baseFeatureFlag )
{
#ifdef USE_PROFILER
	PC_SCOPE( Render2D );

	const CRenderFrameInfo &info = rctx.GetFrameInfo();
	const Uint32 width = rctx.GetFrameInfo().GetViewportWidth();
	const Uint32 height = rctx.GetFrameInfo().GetViewportHeight();

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 
	// Put here some render flow render targets that You want to keep until drawing them here

	if( rctx.Test( rend::DF_PostprocessExposureLuminance_Debug ) )
	{
		auto rtLum		= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "exposureSceneLum" ) );
	}

	if ( rctx.Test( rend::DF_DepthPrepass_Debug ) )
	{
		auto rtDepthPrepassDebug = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthPrepass" ) );
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 
	// Render 2D screen fragments
	if ( rctx.IsConsumePhase() )
	{
		PC_SCOPE( Render_Debug_2D_fragments );

		// Setup global camera parameters
		RenderBindManager::SetCamera2D(rctx, width, height );

		// Draw texture preview
		if ( FEATFLAG_Default == baseFeatureFlag )
		{
			PC_GPU_SCOPE( RenderTexturePreview );
			RenderTexturePreview(rctx);
		}

		// Draw non camera related 2d
		const auto& debugDrawer = rctx.GetFrameInfo().m_nonCameraDebugDrawer;
		if( debugDrawer.Get() )
		{	
			// Collects chunks to temporary stage collector (lightweight - don't worry - heavy stuff is in allocator)
			// Imo it's better to collect here in this case instead of introducing some new stageCollector somewhere
			RenderStageCollector stageCollector;
			{
				stageCollector.SetAllocator( GetRenderer()->GetRenderStageChunksAllocator() );
				debugDrawer->Collect( baseFeatureFlag, stageCollector, nullptr, width, height, Vector2::ONES(), Vector2::ZEROS() );
			}

			// Build renderStageId
			RenderStageId debugRenderStageId;
			switch ( baseFeatureFlag )
			{
			case FEATFLAG_Default:		debugRenderStageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_debugdraw", 0 ); break;
			case FEATFLAG_Wireframe:	debugRenderStageId = GetRenderer()->GetRenderStageRegistry().RegisterStageId( "renderstage_debugdraw_wireframe", 0 ); break;
			default:					RED_ASSERT( !"not handled" );
			}

			// TODO get rid of this when glyph cache texture is updated in FrameTick()
			// same as in CRenderNode_DrawComposition::Execute
			GetRenderer()->GetGlyphCache()->UpdateTexture();

			// Render				
			GeometryBatcherStats localStats; // TODO accumulate somewhere
			RenderBindManager::SetCamera2D(rctx, width, height );

			CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );
			renderCtx.sortingMode = SM_KeepOrder;

			{
				PC_GPU_SCOPE( Debug2D );
				GetRenderer()->GetGeometryBatcher()->RenderGeometry( renderCtx, stageCollector.GetBucket( debugRenderStageId, SRenderStageCluster::Init( DDSC_2D ) ) );
			}

			GetRenderer()->RenderImGui( rctx );

			{
				PC_GPU_SCOPE( Debug2D_PostImGui );
				GetRenderer()->GetGeometryBatcher()->RenderGeometry( renderCtx, stageCollector.GetBucket( debugRenderStageId, SRenderStageCluster::Init( DDSC_2D_PostImGui ) ) );
			}

			// Give back collected chunks to allocator
			stageCollector.Clear();
		}
	}
#endif
}

void CRenderInterface::RenderImGui( const SRenderNodeImplContext& rctx )
{
#ifdef RED_USE_IMGUI
	PC_GPU_SCOPE( RenderImGui );

	const CRenderFrameInfo &info = rctx.GetFrameInfo();

	GetShader( SHADER_NAME( "m_shaderTextured" ) )->Bind();

	const Uint32 width = rctx.GetFrameInfo().GetViewportWidth();
	const Uint32 height = rctx.GetFrameInfo().GetViewportHeight();
	RenderBindManager::SetCamera2D(rctx, width, height );

	GpuApi::SetPSC( PSC_Custom_0, Vector4::ONES() );  // gamma exponent

	GpuApi::SetSamplerStatePreset( 0, GpuApi::SAMPSTATEPRESET_WrapLinearMip );

	static GpuApi::VertexLayoutRef vertexLayout = GpuApi::GetVertexLayout( DebugVertexUV::GetVertexLayoutDesc() );
	GpuApi::SetVertexFormatRaw( vertexLayout );

	for ( const ImGuiDrawCommandList& cmdList : info.m_imGuiDrawCommandList )
	{
		PC_GPU_SCOPE( ImGuiCommandList );

		GpuApi::BeginProfilerBlock( "TextureBatches_Preamble" );

		// recreate vertex buffer
		if ( m_imGuiVertexBufferSize < cmdList.vertices.Size() )
		{
			m_imGuiVertexBufferSize = cmdList.vertices.Size();

			GpuApi::BufferDesc desc;
			desc.size = m_imGuiVertexBufferSize * sizeof( DebugVertexUV );
			desc.category = GpuWrapApi::BCC_Vertex;
			desc.usage = GpuApi::BUT_Dynamic_Legacy;
			desc.group = GpuWrapApi::BufferGroup::Debug;
			m_imGuiVertexBuffer = GpuApi::CreateBuffer( desc );
		}

		// recreate index buffer
		if ( m_imGuiIndexBufferSize < cmdList.indices.Size() )
		{
			m_imGuiIndexBufferSize = cmdList.indices.Size();

			GpuApi::BufferDesc desc;
			desc.size = m_imGuiIndexBufferSize * sizeof( Uint32 );
			desc.category = GpuWrapApi::BCC_Index32Bit;
			desc.usage = GpuApi::BUT_Dynamic_Legacy;
			desc.group = GpuWrapApi::BufferGroup::Debug;
			m_imGuiIndexBuffer = GpuApi::CreateBuffer( desc );
		}

		const Uint32 vbOffset = 0;
		const Uint8 vbStride = sizeof( DebugVertexUV );

		GpuApi::UpdateBuffer( m_imGuiVertexBuffer, cmdList.vertices.DataSize(), cmdList.vertices.Data() );
		GpuApi::UpdateBuffer( m_imGuiIndexBuffer, cmdList.indices.DataSize(), cmdList.indices.Data() );

		GpuApi::BindVertexBuffers( 0, 1, &m_imGuiVertexBuffer, &vbStride, &vbOffset );
		GpuApi::BindIndexBuffer( m_imGuiIndexBuffer, 0 );

		GpuApi::TextureRef currentTexture;

		for ( const ImGuiDrawCommandList::Command& cmd : cmdList.commands )
		{
			const CRenderTextureBase* texture = static_cast<const CRenderTextureBase*>( cmd.texture.Get() );
			const GpuApi::TextureRef textureRef = texture ? texture->GetTextureRef() : GpuApi::GetInternalTexture( GpuApi::INTERTEX_White );
			if ( textureRef != currentTexture )
			{
#ifndef RED_CONFIGURATION_FINAL
				GpuApi::EndProfilerBlock();
				GpuApi::BeginProfilerBlock( texture ? texture->GetDebugName().AsChar() : "<no texture>" );
#endif

				GpuApi::BindTextures( 0, 1, &textureRef, GpuWrapApi::PixelShader );
				currentTexture = textureRef;
			}

			GpuApi::SetScissors( cmd.clippingRect );
			GpuApi::DrawIndexedPrimitive( PRIMTYPE_TriangleList, 0, cmd.indexBufferOffset, cmd.indexCount );
		}

		GpuApi::EndProfilerBlock();
	}

	const Rect fullRect = { 0, (Int32)info.GetViewportWidth(), 0, (Int32)info.GetViewportHeight() };
	GpuApi::SetScissors( fullRect );
#endif
}


void CRenderInterface::NewFrame( const CRenderFrameInfo& frameInfo )
{
	// Allow simulation controller to prepare for the next frame
	if ( m_particleSimulationController )
	{
		m_particleSimulationController->OnNewFrame( frameInfo );
	}

	if( m_volumetricFogManager )
	{
		m_volumetricFogManager->OnNewFrame();
	}

	// Buffer current mesh streaming stats
	m_meshStreamingStats.NextFrame();

	// Let particle data buffers know about new frame
	IParticleData::OnNewFrame();

	UpdatePreviousFrameState();

#ifndef RED_CONFIGURATION_FINAL
	CRenderCollector::s_wasLightAllocationOverflow.SetValue( false ); 
	CRenderCollector::s_wasParticleEmitterOverflow.SetValue( false );
	CRenderCollector::s_wasParticleAmountOverflow.Decrement();
	CRenderCollector::s_wasParticleAmountOverflow.CompareExchange( 0, -1 );
	CBatchDataAllocator::s_wasGeometryDataOverflow.SetValue( false );
	CBatchDataAllocator::s_wasPsoDataOverflow.SetValue( false );
	CBatchDataAllocator::s_wasParamsOverflow.SetValue( false );
	CBatchDataAllocator::s_wasProxyDataOverflow.SetValue( false );
	CBatchDataAllocator::s_wasInstanceOverflow.SetValue( false );
#endif // RED_CONFIGURATION_FINAL
}

void CRenderInterface::EndFrame()
{
	if( m_particleSimulationController )
	{
		m_particleSimulationController->OnEndFrame();
	}
}

void CRenderInterface::RenderFinal2D( const SRenderNodeImplContext& rctx, EFeatureFlag baseFeatureFlag )
{
	const CRenderFrameInfo &info = rctx.GetFrameInfo();
	RenderViewport* viewport = static_cast< RenderViewport* >( info.GetViewport() );

#ifdef RENDER_FINAL_TO_BACKBUFFER

	if ( viewport )
	{
		if ( rctx.IsConsumePhase() )
		{
			GpuApi::SwapChainRef swapChain = viewport->GetSwapChain();
			GpuApi::TextureRef backbufferTex = GpuApi::GetBackBufferTexture( swapChain );

			rctx.BindPSO( ColorTarget( 0, backbufferTex ) );
			rctx.SetViewport( GpuApi::ViewportDesc( info.GetViewportWidth(), info.GetViewportHeight() ) );
		}

		Render2D( rctx, baseFeatureFlag );
	}

#else
	{
		auto rtFinal = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );

		if ( rctx.IsConsumePhase() )
		{
			rctx.BindPSO(ColorTarget(0, rtFinal ) );
			rctx.SetViewport( GpuApi::ViewportDesc( info.GetViewportWidth(), info.GetViewportHeight() ) );
		}

		Render2D( rctx, baseFeatureFlag );
	}

	// Present
	if ( viewport )
	{
		auto rtFinal = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "color" ) );
		if ( rctx.IsConsumePhase() )
		{
			Rect presentSourceRect;
			presentSourceRect.m_left = 0;
			presentSourceRect.m_top  = 0;
			presentSourceRect.m_right  = info.GetViewportWidth();
			presentSourceRect.m_bottom = info.GetViewportHeight();

			Rect presentTargetRect;
			presentTargetRect.m_left = (viewport->GetFullWidth() - viewport->GetWidth()) / 2;
			presentTargetRect.m_top = (viewport->GetFullHeight() - viewport->GetHeight()) / 2;
			presentTargetRect.m_right = viewport->GetFullWidth() - ((viewport->GetFullWidth() - viewport->GetWidth()) / 2);
			presentTargetRect.m_bottom = viewport->GetFullHeight() - ((viewport->GetFullHeight() - viewport->GetHeight()) / 2);

			GetPostProcess()->PresentCopy( rctx, rctx.RTGet<GpuApi::TextureRef>( rtFinal ), GpuApi::TextureRef(), presentSourceRect, presentTargetRect );
		}
	}
#endif
}


void CRenderInterface::RetirePendingResources( job::Builder& builder )
{
	// Flush GpuApi, and synchronize with it. RetirePendingResources isn't meant to be used for per-frame processing,
	// but rather in special case backend type stuff, so we need to be a bit more careful about letting GpuApi finish
	// its stuff.
	GpuApi::Flush();
	GpuApi::SyncWithAsyncFlush( builder );

	// Let surface cache do a full release.
	m_surfaceCache->ReleaseUnusedSurfaces();

	// #HACK: GpuApi queued resource deletions don't work if no command lists are submitted... So just submit an empty
	// one. This should be fixed, and probably would be a fairly straightforward thing, but RetirePendingResources isn't
	// something we do in game, so it's not super important now.
	CName clName = RED_NAME_CONSTEXPR( "RetirePendingResources" );
	GpuApi::CommandListRef commandList = GpuApi::CreateCommandList( GpuApi::CommandListType::Default, clName.GetHash() );
	GpuApi::SetResourceDebugName( commandList, clName.AsChar() );

	GpuApi::BindCommandList( commandList );
	GpuApi::BeginProfilerBlock( "RetirePendingResources" );
	{
		GetGeometryBatcher()->OnFrameTick();
	}
	GpuApi::EndProfilerBlock();
	GpuApi::BindCommandList( {} );

	GpuApi::CommandListRef cls[] = { commandList };
	GpuApi::CloseAndSubmitCommandLists( "Submit_RetirePendingResources", cls, GpuApi::CommandListSyncType::None, builder );
}


void CRenderInterface::FrameTickPrepare( red::ArraySpan< TRenderPtr<IRenderScene> > scenes )
{
	rend::PoolDoubleBufferedFrameRendering::GetAllocator().Reset();

	// Check if the static shader needs to be reloaded
	FlushReloadSimpleShaders();
	
	// #NOTE: Keep this work light! It should be running from main thread, before we've kicked off any pending commands.

	// #TODO: Probably combine this with things from CRenderInterface::Tick(), make it all handled from one place.

	const Uint32 tickCounter = ++m_tickCounter;

	// Skinning manager needs to do some small work, synchronized with engine tick, to ensure we only get skinning data
	// from the frame we want to render, and not from the next frame.
	m_skinningManager->PrepareForFlush();
	
	{
		red::StaticArray<CRenderSceneEx*,32> scenesEx;
		for( TRenderPtr<IRenderScene>& scenePtr : scenes )
		{
			scenesEx.PushBack( static_cast<CRenderSceneEx*>(scenePtr.Get() ) );
		}

		red::alg::RemoveDuplicates( scenesEx );
		for( CRenderSceneEx* scenePtr : scenesEx )
		{
			scenePtr->AllocateFrame();
			scenePtr->PrepareForUpdate( tickCounter );
		}
	}
}

// [skperf] round 64 frametick probe (perfprobe, probeMask bit 1 or 2): RenderFrameTick took 1.2-1.45 s per frame in the
// Kabuki post-load hitch (tc92). Wall ms per FrameTick section; one line at most per second when a tick took >= 30 ms.
// [skperf] round 65 videoPar: access to RenderVideoPlayer's private video list and overflow flags without changing the
// shared header (explicit template instantiation may name private members).
template< auto M > struct CpRobVideoListT { friend auto CpRobVideoList() { return M; } };
auto CpRobVideoList();
template struct CpRobVideoListT< &RenderVideoPlayer::m_offscreenVideos >;
template< auto M > struct CpRobVideoOverflowT { friend auto CpRobVideoOverflow() { return M; } };
auto CpRobVideoOverflow();
template struct CpRobVideoOverflowT< &RenderVideoPlayer::m_offscreenVideosOverflow >;
template< auto M > struct CpRobVideoSingleOverflowT { friend auto CpRobVideoSingleOverflow() { return M; } };
auto CpRobVideoSingleOverflow();
template struct CpRobVideoSingleOverflowT< &RenderVideoPlayer::m_offscreenVideosSingleOverflow >;
namespace Config { extern TConfigVar< Int32 > cvMaxFrameUpload; } // renderVideoPlayer.cpp
static Bool CpVideoParOn()
{
	static const Bool on = []
	{
#if defined( RED_PLATFORM_WEB )
		const Int32 v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'videoPar' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
#else
		const Int32 v = 0;
#endif
		std::fprintf( stderr, "CP2077 skperf switches: videoPar=%d\n", v );
		return v != 0;
	}();
	return on;
}
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
namespace cpFrameTickProbe
{
	enum { Flush, CmdList, Skinning, Batcher, Mipgen, DynTex, Hair, Gradient, Ies, Video, Foliage, Gi, Cloth, Destruction, Multilayer, Surface, Glyph, Count };
	static const char* const c_names[ Count ] = { "flush", "cmdList", "skinning", "batcher", "mipgen", "dynTex", "hair", "gradient", "ies", "video", "foliage", "gi", "cloth", "destruction", "multilayer", "surfaceCache", "glyph" };
	struct Timer
	{
		Bool on; double t[ Count ] = {}; double last = 0, start = 0;
		Timer() : on( ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 3 ) != 0 ) { if ( on ) start = last = emscripten_get_now(); }
		void Mark( int i ) { if ( !on ) return; const double now = emscripten_get_now(); t[ i ] += now - last; last = now; }
		~Timer()
		{
			if ( !on ) return;
			const double total = emscripten_get_now() - start;
			static std::atomic< double > s_lastPrint{ 0 };
			const double now = emscripten_get_now();
			if ( total < 30.0 || now - s_lastPrint.load( std::memory_order_relaxed ) < 1000.0 ) return;
			s_lastPrint.store( now, std::memory_order_relaxed );
			char line[ 900 ]; int used = std::snprintf( line, sizeof( line ), "CP2077 skperf frametick probe: totalMs=%.1f", total );
			for ( int i = 0; i < Count && used > 0 && used < ( int )sizeof( line ) - 40; ++i ) used += std::snprintf( line + used, sizeof( line ) - used, " %s=%.1f", c_names[ i ], t[ i ] );
			std::fprintf( stderr, "%s\n", line );
		}
	};
}
void CRenderInterface::FrameTick( job::Builder& builder, Bool flushSkinning )
{
	cpFrameTickProbe::Timer cpFt; // [skperf] round 64
	// This doesn't wait for work to finish, just gives GpuApi a chance to kick off any pending work, free unused
	// resources, etc. Good to call every now and then.
	GpuApi::Flush();
	cpFt.Mark( cpFrameTickProbe::Flush );

	static const CName clName = RED_NAME_CONSTEXPR("FrameTick");
	GpuApi::CommandListRef commandList;

	{
		PC_SCOPE( CreateCommandList );
		commandList = GpuApi::CreateCommandList( GpuApi::CommandListType::Default, clName.GetHash() );
		GpuApi::SetResourceDebugName( commandList, clName.AsChar() );
	}

	GpuApi::BindCommandList( commandList );
	GpuApi::BeginProfilerBlock( clName.AsChar() );

#ifdef USE_PROFILER
	Uint32 gpuFrameTickID = 0;
	if ( m_gpuProfiler )
	{
		{
			PC_SCOPE( ProcessQueries );
			// read back gpu queries for the profiler
			m_gpuProfiler->ProcessQueries();
		}

		m_gpuProfiler->OnFrameBegin();

		Uint32 gpuFrameTickIndex = m_gpuProfiler->AddEntry( clName );
		gpuFrameTickID = m_gpuProfiler->StartEntry( gpuFrameTickIndex );
	}
#endif

	cpFt.Mark( cpFrameTickProbe::CmdList );
	m_GpuFrameTimer.Update();
	m_lastMainSceneTime = 1000.0f * m_GpuFrameTimer.GetLastMainSceneTime();
	m_lastSubsceneTime = 1000.0f * m_GpuFrameTimer.GetLastSubsceneTime();
	m_lastDrawBuffersTime = 1000.0f * m_GpuFrameTimer.GetLastDrawBuffersTime();

	if ( flushSkinning )
	{
		PC_SCOPE( SkinningUpdate );
		RenderSkinningManager* skinningManager = GetRenderer()->GetSkinningManager();
		skinningManager->Flush( commandList, builder );
	}

	cpFt.Mark( cpFrameTickProbe::Skinning );
	GetGeometryBatcher()->OnFrameTick();
	cpFt.Mark( cpFrameTickProbe::Batcher );

	m_mipmapGenerator->FrameTick();
	cpFt.Mark( cpFrameTickProbe::Mipgen );

	// Run dynamic texture updates
	if ( m_dynamicTextureManager != nullptr )
	{
		PC_SCOPE( DynamicTextureManager );
		PC_GPU_SCOPE( DynamicTextureManager );
		m_dynamicTextureManager->Generate( false );
	}

	cpFt.Mark( cpFrameTickProbe::DynTex );
	if ( m_hairProfilesManager != nullptr )
	{
		PC_SCOPE( HairProfilesManager );
		PC_GPU_SCOPE( HairProfilesManager );
		m_hairProfilesManager->Tick();
	}

	cpFt.Mark( cpFrameTickProbe::Hair );
	if ( m_gradientManager != nullptr )
	{
		PC_SCOPE( GradientManager );
		PC_GPU_SCOPE( GradientManager );
		m_gradientManager->Tick();
	}

	cpFt.Mark( cpFrameTickProbe::Gradient );
	if( m_iesProfile )
	{
		PC_SCOPE( IESProfile );
		PC_GPU_SCOPE( IESProfile );
		m_iesProfile->FlushPendingCommands();
	}

	// [skperf] round 65 videoPar (opt-in ?videoPar=1): every Bink control call is a blocking round trip through the
	// browser main thread to the decoder worker (tc93: frametick 'video' = all of RenderFrameTick, 33 ms steady and
	// 0.2-1.4 s per tick in the post-load hitch). The offscreen videos' Update() calls (Bink control/decode scheduling
	// only, per video state) now run as a parallel-for, so their round trips overlap instead of adding up. Everything
	// that records into this tick's command list (the uploads, in the original order and budget, and the rest of the
	// tick from foliage to glyphs, which keeps running after the video updates as before) runs in one job after them,
	// with the list re-bound there; the submit job follows as before.
	auto cpRest = [this]( cpFrameTickProbe::Timer* cpT )
	{
		if (m_foliageProfilesManager != nullptr)
		{
			PC_SCOPE( FoliageProfilesManager );
			PC_GPU_SCOPE( FoliageProfilesManager );
			m_foliageProfilesManager->UpdateFoliageProfileMap();
		}

		if ( cpT ) cpT->Mark( cpFrameTickProbe::Foliage );
		m_giStreamingTracker.Tick();
		if ( cpT ) cpT->Mark( cpFrameTickProbe::Gi );

		{
			decltype(m_clothsPendingUpdate) dirtyCloths{ m_clothsPendingUpdate.GetPool() };

			{
				RED_SCOPE_LOCK( m_clothLock );
				std::swap( dirtyCloths, m_clothsPendingUpdate );
			}

			if( !dirtyCloths.Empty() )
			{
				PC_SCOPE( ClothMeshUpdates );
				PC_GPU_SCOPE( ClothMeshUpdates );

				for( ClothMeshPtr& cloth : dirtyCloths )
				{
					cloth->UploadToGPU();
				}
			}
		}

		{
			if ( cpT ) cpT->Mark( cpFrameTickProbe::Cloth );
			decltype(m_destructionBuffers) dirtyDestruction{ m_destructionBuffers.GetPool() };

			{
				RED_SCOPE_LOCK( m_destructionBuffersLock );
				std::swap( dirtyDestruction, m_destructionBuffers );
			}

			if( !dirtyDestruction.Empty() )
			{
				PC_SCOPE( DestructionMeshUpdates );
				PC_GPU_SCOPE( DestructionMeshUpdates );

				for( auto& dstr : dirtyDestruction )
				{
					GpuApi::UpdateBuffer( dstr.first, dstr.second.DataSize(), dstr.second.TypedData() );
					GpuApi::SafeRelease( dstr.first );
				}
			}
		}

		if ( cpT ) cpT->Mark( cpFrameTickProbe::Destruction );
		RenderMultilayerSetup::FlushPendingUploads();
		if ( cpT ) cpT->Mark( cpFrameTickProbe::Multilayer );

		{
			PC_SCOPE( SurfaceCache );
			PC_GPU_SCOPE( SurfaceCache );
			m_surfaceCache->Update();
		}

		{
			if ( cpT ) cpT->Mark( cpFrameTickProbe::Surface );
			PC_SCOPE( GlyphCache );
			m_glyphCache->NextFrame();
		}

	};
	if ( CpVideoParOn() )
	{
		cpFt.Mark( cpFrameTickProbe::Ies );
		GpuApi::BindCommandList( {} ); // re-bound by the job below
		GRender->ProcessRenderVideoDestroyQueue();
		RenderVideoPlayer* cpPlayer = &*m_videoPlayer;
		auto& cpVideos = cpPlayer->*CpRobVideoList();
		using CpVid = typename std::remove_reference< decltype( cpVideos[ 0 ] ) >::type;
		std::sort( cpVideos.Begin(), cpVideos.End(), []( const CpVid& x, const CpVid& y ) { return x.ticksSinceUpload > y.ticksSinceUpload; } );
		builder.DispatchParallelForJob( "CpVideoUpdate", job::ImmediateValue{ static_cast< Uint32 >( cpVideos.Size() ) }, [cpPlayer]( Uint32 i, const job::RunContext& )
		{
			auto& v = ( cpPlayer->*CpRobVideoList() )[ i ];
			if ( !v.video->IsPaused() ) v.video->Update();
		} );
		builder.DispatchJob( "CpVideoUploadAndTickRest", [cpPlayer, commandList, cpRest]( const job::RunContext& )
		{
			GpuApi::BindCommandList( commandList );
			{
				// == RenderVideoPlayer::Update's upload loop (the per-video Update() already ran above)
				const Uint32 maxUploaded = Config::cvMaxFrameUpload.Get();
				Uint32 totalUploaded = 0;
				Bool outOfBudget = false, singleOutOfBudget = false;
				for ( auto& offscreenVideo : cpPlayer->*CpRobVideoList() )
				{
					if ( offscreenVideo.video->IsPaused() ) continue;
					switch ( offscreenVideo.video->UploadTextures( maxUploaded, totalUploaded ) )
					{
					case IRenderVideo::UploadResult::NotNeeded:
					case IRenderVideo::UploadResult::Uploaded: offscreenVideo.ticksSinceUpload = 0; break;
					case IRenderVideo::UploadResult::OutOfBudget: outOfBudget = true; break;
					case IRenderVideo::UploadResult::TooBigForBudget: singleOutOfBudget = true; break;
					}
				}
				( cpPlayer->*CpRobVideoOverflow() ).SetValue( outOfBudget );
				( cpPlayer->*CpRobVideoSingleOverflow() ).SetValue( singleOutOfBudget );
			}
			cpRest( nullptr );
			GpuApi::BindCommandList( {} );
		} );
	}
	else
	{
	{
		cpFt.Mark( cpFrameTickProbe::Ies );
		PC_SCOPE( UpdateRenderVideoPlayer );
		PC_GPU_SCOPE( UpdateRenderVideoPlayer );
		m_videoPlayer->Update();
	}

	cpFt.Mark( cpFrameTickProbe::Video );
	cpRest( &cpFt );
	}

	cpFt.Mark( cpFrameTickProbe::Glyph );
	// #IMPORTANT: Unbind command list, so we can safely use it in the Submit job below.
	GpuApi::BindCommandList( {} );

	// Finally we can submit everything.
	builder.DispatchJob( "RenderTickSubmitCommandList", [=]( const job::RunContext& runContext )
	{
		GpuApi::BindCommandList( commandList );
#ifdef USE_PROFILER
		if ( CRenderGpuProfiler* gpuProfiler = static_cast<CRenderGpuProfiler*>(GetRenderer()->GetGpuProfiler()) )
		{
			gpuProfiler->EndEntry( gpuFrameTickID );

			// Push a FrameEnd as well, even though it's probably not the end of the frame. FrameEnd is able to stack up
			// (can be called multiple times for a single OnFrameBegin), so shouldn't harm anything. But if it turns out
			// we don't render a frame after this (e.g. in editor with no editor windows open), then we still need to
			// end it...
			gpuProfiler->OnFrameEnd();
		}
#endif
		GpuApi::EndProfilerBlock();
		GpuApi::BindCommandList( {} );

		job::Builder childBuilder { runContext };
		GpuApi::CommandListRef cls[] = { commandList };
		GpuApi::CloseAndSubmitCommandLists( "Submit_FrameTick", cls, GpuApi::CommandListSyncType::None, childBuilder );
	} );
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum
{
	UNIQUE_RENDERNODE_StartRender,
	UNIQUE_RENDERNODE_EndRender,
	UNIQUE_RENDERNODE_AdvanceWind,
	UNIQUE_RENDERNODE_TonemappingLUT,
	UNIQUE_RENDERNODE_DebugZoom,
	UNIQUE_RENDERNODE_OffscreenParticlesSim,
	UNIQUE_RENDERNODE_ExtractDepthFlush,
	UNIQUE_RENDERNODE_ExtractDepthGrab,
	UNIQUE_RENDERNODE_ExtractSelectionGrab,
	UNIQUE_RENDERNODE_ExtractColor,
	UNIQUE_RENDERNODE_FlushTextureGrabs,
	UNIQUE_RENDERNODE_FlushBufferGrabs,
	UNIQUE_RENDERNODE_ExtractSelectionProcess,
	UNIQUE_RENDERNODE_EndFrame,
	UNIQUE_RENDERNODE_CleanupBatchDataAllocator,
	UNIQUE_RENDERNODE_FinalFlush,
	UNIQUE_RENDERNODE_Present,
	UNIQUE_RENDERNODE_PrepareRainMap,
	UNIQUE_RENDERNODE_WindUpdateAndRainMap,
};

enum
{
	SEQUENCE_RENDERNODE_Camera,
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void AddParticleSimThreads( rend::NodeGroupID groupId, rend::NodeGraphFactory &factory, Bool addOnScreenCpu, Bool addOffscreenCpu )
{
	if( addOnScreenCpu )
	{
		for( Uint32 i = 0; i < CRenderNode_SimulateOffScreenCPUParticles::sParticleSimThreads; ++i )
		{
			ADD_NODE( groupId, "SimulateOnScreenCPUParticles", CRenderNode_SimulateOnScreenCPUParticles, vis::CollectThreadingSetup( i, CRenderNode_SimulateOffScreenCPUParticles::sParticleSimThreads ) );
		}
	}

	if ( addOffscreenCpu )
	{
		ADD_UNIQUE( groupId, "SimulateOffScreenCPUParticles", UNIQUE_RENDERNODE_OffscreenParticlesSim, CRenderNode_SimulateOffScreenCPUParticles );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderInterface::BuildRenderGraphBlank( CRenderNodeGraph &graph, rend::NodesContainer &nodes, Bool isWireframe, Bool isPrewarm )
{
	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None = rend::NodeGroupID::None;

	RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );
	RENDER_COMMAND_LIST( G_None, "RenderGraph_Blank" )
	{
		ADD_SUBNODE( "DeclCommonResAllocs", CRenderNode_DeclareCommonResourceAllocs_FinalOnly );
		ADD_SUBNODE( "PrepSceneRendering", CRenderNode_PrepareBlankRendering );

		if( isPrewarm )
		{
			ADD_SUBNODE( "RenderSkyScattering", CRenderNode_RenderSkyScattering );
			ADD_SUBNODE( "ReflectionProbes", CRenderNode_ReflectionProbes );
			ADD_SUBNODE( "GlobalIllumination", CRenderNode_GlobalIllumination );
			ADD_SUBNODE( "VolumetricFog", CRenderNode_VolumetricFog );
		}

		ADD_SUBNODE( "ClearFinalColorTarget", CRenderNode_ClearFinalColorTarget );
		ADD_SUBNODE( "DrawComposition", CRenderNode_DrawComposition );
		ADD_SUBNODE( "CompositionPostProcess", CRenderNode_CompositionPostProcess );
		ADD_SUBNODE( "DrawHUD", CRenderNode_DrawHUD );
		ADD_SUBNODE( "FullscreenVideo", CRenderNode_FullscreenVideo );
		ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, (isWireframe ? FEATFLAG_Wireframe : FEATFLAG_Default) );
		ADD_SUBNODE( "ExtractionFinalColor", CRenderNode_ExtractionFinalColor );
	}
	RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );

	ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphBlank" );
	ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	// Link
	factory.LinkGPU();
	factory.LinkCPU();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderInterface::BuildRenderGraphHitProxies( CRenderNodeGraph &graph, rend::NodesContainer &nodes, Bool isMultiLayer, Bool isDebugView )
{
	using REBatch = RenderElementsBatch;

	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None		= rend::NodeGroupID::None;
	const rend::NodeGroupID G_Render	= rend::NodeGroupID(1);
	const rend::NodeGroupID G_PreRender	= rend::NodeGroupID(2);
	const rend::NodeGroupID G_Culling	= rend::NodeGroupID(3);

	auto startRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto cameraStartID= ADD_SEQ_BEGIN( G_None, "CameraStart_HP", SEQUENCE_RENDERNODE_Camera );

	ADD_NODE( G_PreRender, "DeclCommonResAllocs_HP", CRenderNode_DeclareCommonResourceAllocs_HitProxy );
	ADD_NODE( G_PreRender, "PrepCollector_HP", CRenderNode_PrepareCollector, FEATFLAG_HitProxies );

	auto renderPrepID = RENDER_COMMAND_LIST( G_PreRender, "RenderPrep" )
	{
		ADD_SUBNODE( "UpdateGlobalBindings", CRenderNode_UpdateGlobalBindings );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	ADD_NODE( G_Culling, "DoCulling", CRenderNode_DoCulling, cullingMod::MainScene::YES, cullingMod::Cascades::NO, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::NO );
	ADD_NODE( G_Culling, "DebugCollectHighPriority", CRenderNode_CollectDebug, true );
	ADD_NODE( G_Culling, "DebugCollectLowPriority", CRenderNode_CollectDebug, false );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	Uint32 commonFlags = 0;
	Uint32 clearFlags = 0;
	if ( !isMultiLayer )
	{
		commonFlags = RTFINAL_BindDepth | RTFINAL_AllowDepthWrite;
		clearFlags = commonFlags | RTFINAL_ClearColor | RTFINAL_ClearDepth;
	}
	else
	{
		commonFlags = RTFINAL_UseMultilayerSelection;
		clearFlags = commonFlags | RTFINAL_ClearColor | RTFINAL_ClearMultilayerSelection;
	}

	RENDER_COMMAND_LIST( G_Render, "HitProxy_Solid" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetAndClearFinalTargets", CRenderNode_SetRenderTargetsHitProxy, clearFlags );

		ADD_SUBNODE( "PrepHitProxyRendering", CRenderNode_PrepareHitProxyRendering );
		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
			, REBatch( REBF_CAM_MainScene|REBF_DEBUG_NoStats, "renderstage_gbuffer_regular", SRenderStageCluster::Init(RPl_Scene), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndFinalTargetsUse", CRenderNode_EndRenderTargetsHitProxy, clearFlags );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "HitProxy_Discard" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetFinalTargets_HP", CRenderNode_SetRenderTargetsHitProxy, commonFlags );

		ADD_SUBNODE( "PrepHitProxyRendering_HP", CRenderNode_PrepareHitProxyRendering );
		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
			, REBatch( REBF_CAM_MainScene|REBF_DEBUG_NoStats, "renderstage_gbuffer_discarded", SRenderStageCluster::Init(RPl_Scene), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndFinalTargetsUse_HP", CRenderNode_EndRenderTargetsHitProxy, commonFlags );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "HitProxy_Late" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetFinalTargets_HP", CRenderNode_SetRenderTargetsHitProxy, commonFlags );

		ADD_SUBNODE( "PrepHitProxyRendering_HP", CRenderNode_PrepareHitProxyRendering );
		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBufferLate ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
			, REBatch( REBF_CAM_MainScene|REBF_DEBUG_NoStats, "renderstage_late_gbuffer_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene|REBF_DEBUG_NoStats, "renderstage_late_gbuffer_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndFinalTargetsUse_HP", CRenderNode_EndRenderTargetsHitProxy, commonFlags );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "HitProxy_Debug" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetFinalTargets_HP", CRenderNode_SetRenderTargetsHitProxy, commonFlags );
		ADD_SUBNODE( "PrepHitProxyRendering_HP", CRenderNode_PrepareHitProxyRendering );
		ADD_SUBNODE( "RenderDebugOverlay_World_HP", CRenderNode_RenderDebugFragments, DebugFragments::WorldAndWorldOverlay, FEATFLAG_HitProxies );
		ADD_SUBNODE( "RenderDebugOverlay_Screen_HP", CRenderNode_RenderDebugFragments, DebugFragments::Screen, FEATFLAG_HitProxies );
		ADD_SUBNODE( "EndFinalTargetsUse_HP", CRenderNode_EndRenderTargetsHitProxy, commonFlags );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "Apply" )
	{
		if ( isDebugView )
		{
			ADD_SUBNODE( "ApplyDebugPreview", CRenderNode_ApplyDebugPreview );
		}

		if ( isMultiLayer )
		{
			ADD_SUBNODE( "ExtractionMultiLayerSelection_HP", CRenderNode_ExtractionMultiLayerSelection );
		}
		else
		{
			ADD_SUBNODE( "ExtractionSelection_HP", CRenderNode_ExtractionSelection );
		}


		if ( isDebugView )
		{
			ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, FEATFLAG_HitProxies );
		}
		
		ADD_SUBNODE( "ExtractionFinalColor", CRenderNode_ExtractionFinalColor );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	auto cameraEndID = ADD_SEQ_END( G_None, "CameraEnd_HP", SEQUENCE_RENDERNODE_Camera );

	//auto extractColorID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "ExtractionColor", UNIQUE_RENDERNODE_ExtractColor, CRenderNode_ExtractionColor );
	auto endRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender_HP", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );
	auto idFlushGpu = ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphHitProxies" );

	CRenderNodeGraph::ItemId presentID;
	if ( isDebugView )
	{
		presentID = ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	}

	auto flushTexGrabsID = ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	auto flushBufferGrabsID = ADD_UNIQUE( G_None, "FlushBufferGrabs", UNIQUE_RENDERNODE_FlushBufferGrabs, CRenderNode_FlushBufferGrabs );

	auto extractionSelectionProcess = ADD_UNIQUE( G_None, "extractionSelectionProcess", UNIQUE_RENDERNODE_ExtractSelectionProcess, CRenderNode_ExtractionSelectionProcess );

	auto cleanupBatchDataAllocatorID = ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID = ADD_UNIQUE( G_None, "EndFrame_HP", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	{
		factory.LinkGPU();

		factory.LinkCPUToNextGPU( startRenderID );

		factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
		factory.Link( cameraStartID, G_PreRender, RNDT_Cpu );
		factory.Link( G_PreRender, G_Culling, RNDT_Cpu );
		factory.Link( G_Culling, G_Render, RNDT_Cpu );
		factory.Link( G_Render, cameraEndID, RNDT_Cpu );
		factory.Link( cameraEndID, endRenderID, RNDT_Cpu );
		factory.Link( G_Render, cleanupBatchDataAllocatorID, RNDT_Cpu );
		factory.Link( endRenderID, idFlushGpu, RNDT_Cpu );

		if ( isDebugView )
		{
			factory.Link( idFlushGpu, presentID, RNDT_Cpu );
			factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
		}
		else
		{
			factory.Link( idFlushGpu, flushTexGrabsID, RNDT_Cpu );
		}

		factory.Link( flushTexGrabsID, flushBufferGrabsID, RNDT_Cpu );
		factory.Link( flushBufferGrabsID, extractionSelectionProcess, RNDT_Cpu );
		factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );
		factory.Link( extractionSelectionProcess, endFrameID, RNDT_Cpu );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderInterface::BuildRenderGraphGBufferOnly( CRenderNodeGraph &graph, rend::NodesContainer &nodes )
{
	using REBatch = RenderElementsBatch;

	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None		= rend::NodeGroupID::None;
	const rend::NodeGroupID G_Render	= rend::NodeGroupID(1);
	const rend::NodeGroupID G_PreRender	= rend::NodeGroupID(2);
	const rend::NodeGroupID G_Culling	= rend::NodeGroupID(3);

	auto startRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto cameraStartID = ADD_SEQ_BEGIN( G_None, "CameraStart", SEQUENCE_RENDERNODE_Camera );

	auto advWindID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "AdvanceWind", UNIQUE_RENDERNODE_AdvanceWind, CRenderNode_AdvanceSpeedTreeWind );

	ADD_NODE( G_PreRender, "DeclCommonResAllocs", CRenderNode_DeclareCommonResourceAllocs_GBufferOnly );
	ADD_NODE( G_PreRender, "PrepCollector", CRenderNode_PrepareCollector, FEATFLAG_Default );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	ADD_NODE( G_Culling, "DoCulling", CRenderNode_DoCulling, cullingMod::MainScene::YES, cullingMod::Cascades::NO, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::NO );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "PrepSceneRendering" )
	{
		ADD_SUBNODE( "CamResDepScopeOpen", CRenderNode_CameraResourceDependencyScope, true );
		ADD_SUBNODE( "PrepSceneRendering", CRenderNode_PrepareSceneRendering );
	}

	RENDER_COMMAND_LIST( G_Render, "GBufferOnly_Regular" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff_Main", CRenderNode_SetRenderTargetsGBuffer, targetMod::ClearGBuffer|targetMod::WriteDepth_YES );
		ADD_SUBNODE( "RenderElements_Main", CRenderNode_RenderElements
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
		);
		ADD_SUBNODE( "EndRenderToGbuff_Main", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "GBufferOnly_RenderGIVolumes" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "RenderGIVolumes", CRenderNode_RenderGIVolumes );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "GBufferOnly_Discard" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff_Main", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_YES );
		ADD_SUBNODE( "RenderElements_Main", CRenderNode_RenderElements
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
		);
		ADD_SUBNODE( "EndRenderToGbuff_Main", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "GBuffer_Late" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff_Main", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_YES );
		ADD_SUBNODE( "RenderElements_Main", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBufferLate ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_late_gbuffer_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_late_gbuffer_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
		);
		ADD_SUBNODE( "EndRenderToGbuff_Main", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "GBufferOnly_DeferredDecals" )
	{
		ADD_SUBNODE( "PrepareFeedbackNormalBuffer", CRenderNode_PrepareFeedbackNormalBuffer );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_NO );
		ADD_SUBNODE( "RenderElements_PostGBuffer", CRenderNode_RenderElements
			, REBatch(REBF_CAM_MainScene, "renderstage_post_gbuffer", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
		);
		ADD_SUBNODE( "EndRenderToGbuff", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "EndGBuffer1Feedback", CRenderNode_EndGBuffer1Feedback );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "Finalize" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "CopyToTexture", CRenderNode_CopyToTexture );
		ADD_SUBNODE( "ExtractionDepthBuffPrep", CRenderNode_ModifyDepth, eDepthModificationType::Linearize );
		ADD_SUBNODE( "ExtractionColor", CRenderNode_ExtractionGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		ADD_SUBNODE( "CamResDepScopeClose", CRenderNode_CameraResourceDependencyScope, false );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto cameraEndID = ADD_SEQ_END( G_None, "CameraEnd", SEQUENCE_RENDERNODE_Camera );

	auto extractDepthGrabID = RENDER_UNIQUE_COMMAND_LIST( G_None, UNIQUE_RENDERNODE_EndRender, "ExtractionDepthBuffGrab" )
	{
		ADD_SUBNODE( "ExtractionDepthBuffGrab", CRenderNode_ExtractionDepthBufferGrab );
		ADD_SUBNODE( "EndRender", CRenderNode_EndRender );
	}

	auto flushGpuID = ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphGBufferOnly" );
	auto presentID = ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	auto flushTextureGrabsID = ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	auto cleanupBatchDataAllocatorID = ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID = ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	{
		factory.LinkGPU();

		factory.LinkCPUToNextGPU( startRenderID );

		factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
		factory.Link( cameraStartID, G_PreRender, RNDT_Cpu );
		factory.Link( G_PreRender, G_Culling, RNDT_Cpu );
		factory.Link( G_Culling, G_Render, RNDT_Cpu );
		factory.Link( advWindID, G_Render, RNDT_Cpu );
		factory.Link( G_Render, cameraEndID, RNDT_Cpu );
		factory.Link( cameraEndID, extractDepthGrabID, RNDT_Cpu );
		factory.Link( extractDepthGrabID, flushGpuID, RNDT_Cpu );
		factory.Link( flushGpuID, presentID, RNDT_Cpu );
		factory.Link( presentID, flushTextureGrabsID, RNDT_Cpu );
		factory.Link( G_Render, cleanupBatchDataAllocatorID, RNDT_Cpu );
		factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );
		factory.Link( flushTextureGrabsID, endFrameID, RNDT_Cpu );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Bool IsHairEnabled( const SRenderNodeImplContext &rctx )
{
	return rctx.Test( RFF_Hair );
}

void CRenderInterface::BuildRenderGraphCamera( CRenderNodeGraph &graph, rend::NodesContainer &nodes, const RenderFeatureSet& cameraFeatures, EEnvManagerModifier displayMode )
{
	using REBatch = RenderElementsBatch;

	// #TODO: Clean these up.

	constexpr targetMod rt_GBuffer_NoClear = targetMod::WriteDepth_YES;

	constexpr targetMod rt_Unlit =
		targetMod::SamplableNormalsFeedback|
		targetMod::WeaponDepthCorrected_NO|targetMod::PostAA_NO|
		targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;

	// Transparent passes can bind additional targets to aid TAA. When we have DLSS, we write to velocity buffer because
	// that's what it wants to use. Without DLSS we have a mask of where to not apply TAA.
	// IZNV: The current state of things is that DLSS is gonna use ColorBiasMask, so we hopefuly won't need to write anything to the velocity buffer.
	// So I commented the switch below
	const targetMod rtmod_Transparent_TXAAMask = /* cameraFeatures.Get( RFF_DLSS ) ? targetMod::TransparencyTXAAMask_NO : */ targetMod::TransparencyTXAAMask_YES;
	const targetMod rtmod_Transparent_Velocity = /* cameraFeatures.Get( RFF_DLSS ) ? targetMod::VelocityBuffer_YES : */ targetMod::VelocityBuffer_NO;

	const targetMod rt_TransparentRWDepth =
		targetMod::WriteDepth_YES|targetMod::SamplableHologramDepth|targetMod::SamplableDPL|
		targetMod::WeaponDepthCorrected_YES|targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|rtmod_Transparent_Velocity;

	const targetMod rt_TransparentBackground =
		targetMod::WriteDepth_NO|targetMod::SamplableHologramDepth|targetMod::SamplableDPL|
		targetMod::WeaponDepthCorrected_YES|targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|rtmod_Transparent_Velocity;

	// Disable velocity output as it is not supported by the current pass
	const targetMod rt_Transparent =
		targetMod::WriteDepth_NO|targetMod::SamplableHologramDepth|targetMod::SamplableDPL|
		targetMod::WeaponDepthCorrected_YES|targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|targetMod::VelocityBuffer_NO;

	const targetMod rt_Transparent_WeaponAndScreen =
		targetMod::WriteDepth_NO|targetMod::SamplableColorFeedback|targetMod::SamplableHologramDepth|targetMod::SamplableDPL|
		targetMod::WeaponDepthCorrected_YES|targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|rtmod_Transparent_Velocity;

	const targetMod rt_NoTxaa =
		targetMod::SamplableDPL|targetMod::WeaponDepthCorrected_NO|
		(cameraFeatures.Get( RFF_DLSS ) ? targetMod::PostAA_YES : targetMod::PostAA_NO)|
		targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;

	const targetMod rt_OpaqueNoTxaa = rt_NoTxaa|targetMod::WriteDepth_YES|targetMod::PostAA_INITIALIZE;

	const targetMod rt_TransparentNoTxaa = rt_NoTxaa|targetMod::WriteDepth_NO|targetMod::SamplableColorFeedback;


#ifdef GPUAPI_EXTENDED_SYNC
	constexpr GpuApi::InterSyncPoint sync_DepthChain = GpuApi::InterSyncPoint(1);
#endif


	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None 					= rend::NodeGroupID::None;
	const rend::NodeGroupID G_CullingScene 			= rend::NodeGroupID( 1 );
	const rend::NodeGroupID G_CullingRayTracing 	= rend::NodeGroupID( 2 );
	const rend::NodeGroupID G_CullingCascades 		= rend::NodeGroupID( 3 );
	const rend::NodeGroupID G_CullingLocalShadows 	= rend::NodeGroupID( 4 );
	const rend::NodeGroupID G_PostCullingDebug		= rend::NodeGroupID( 5 );
	const rend::NodeGroupID G_RenderPrep 			= rend::NodeGroupID( 6 );
	const rend::NodeGroupID G_Render 				= rend::NodeGroupID( 7 );
	const rend::NodeGroupID G_ParticlesOnScreenSim	= rend::NodeGroupID( 8 );
	const rend::NodeGroupID G_ParticlesOffScreenSim	= rend::NodeGroupID( 9 );
	const rend::NodeGroupID G_PostRender 			= rend::NodeGroupID( 10 );

	const Bool hasFlushGrabs = ( cameraFeatures.Get( RFF_GBufferExtraction ) && cameraFeatures.Get( RFF_ImmediateMode ) ) ||
								 cameraFeatures.Get( RFF_ColorExtraction ) ||
							   ( cameraFeatures.Get( RFF_DepthExtraction ) && cameraFeatures.Get( RFF_ImmediateMode ) ) ||
								 cameraFeatures.Get( RFF_BackgroundDump );

#ifdef USE_RAY_TRACING
	Bool useRayTracing = false;
	if ( cameraFeatures.Get( RFF_RayTracedAmbientOcclusion ) ||
		cameraFeatures.Get( RFF_RayTracedDiffuseIllumination ) ||
		cameraFeatures.Get( RFF_RayTracedDirectionalShadow ) ||
		cameraFeatures.Get( RFF_RayTracedReflection ) ||
		cameraFeatures.Get( RFF_RayTracedTranparentReflection ) ||
		cameraFeatures.Get( RFF_RayTracingDebug ) )
	{
		RED_ASSERT( GpuApi::GetCapabilities().rayTracingSupport, "Ray traced effects are enabled for render graph, but GpuApi reports no support!" );
		useRayTracing = true;
	}
	const Bool rayTracingAsyncASBuild = useRayTracing && Config::cvAsyncRayTracingASBuild.Get();
#endif

	const Bool doAsyncCompute = Config::cvAsyncComputeEnable.Get() && cameraFeatures.Get( RFF_EnableAsyncCompute );
	const Bool doAsyncHairClear = Config::cvAsyncHairClears.Get() && doAsyncCompute;
	const Bool doAsyncSSAO = cameraFeatures.Get( RFF_SSAO ) && Config::cvAsyncSSAO.Get() && doAsyncCompute;
	const Bool doAsyncLutGeneration = Config::cvAsyncLutGeneration.Get() && doAsyncCompute && cameraFeatures.Get( RFF_Tonemapping );
	const Bool doAsyncDynamicTextureGeneration = Config::cvAsyncDynamicTextureGeneration.Get() && doAsyncCompute;
	const Bool doAsyncFlattenNormals = Config::cvAsyncFlattenNormals.Get() && doAsyncCompute;
	const Bool doAsyncBuildDepthChain = Config::cvAsyncBuildDepthChain.Get() && doAsyncCompute;

	const Bool enableAsyncComputeDuringShadowmaps =
		doAsyncSSAO ||
		doAsyncLutGeneration ||
		doAsyncDynamicTextureGeneration ||
		doAsyncFlattenNormals ||
		doAsyncBuildDepthChain
#ifdef USE_RAY_TRACING
		|| rayTracingAsyncASBuild
#endif
		;

	// SSR can issue a clear, so we can only run SSR on async compute if clear uses a compute shader 
	// #TODO: Check PS4. Other platforms need carefully timing to check this is a win
	// #TODO: SSR's clear could be switched to a ClearUAV, then PC could even overlap it. But need to be careful :)
#if defined(GPUAPI_CLEAR_CAN_RUN_ON_ASYNC_COMPUTE) && defined(RED_PLATFORM_DURANGO)
	const Bool doAsyncSSR =  true;
#else
	const Bool doAsyncSSR = false;
#endif

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto startRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto prepDistantShadowsID = ADD_NODE( G_None, "PrepDistantShadows", CRenderNode_PrepareDistantShadows );
	auto prepareRainMapID = ADD_UNIQUE( G_None, "PrepareRainMap", UNIQUE_RENDERNODE_PrepareRainMap, CRenderNode_PrepareRainMap );

	auto windUpdateAndRainMapID = RENDER_UNIQUE_COMMAND_LIST( G_None, UNIQUE_RENDERNODE_WindUpdateAndRainMap, "WindUpdateAndRainMap" )
	{
		ADD_SUBNODE( "AdvanceWind", CRenderNode_AdvanceSpeedTreeWind );
		ADD_SUBNODE( "RenderRainMap", CRenderNode_RenderRainMap );
	}

	auto cameraStartID = ADD_SEQ_BEGIN( G_None, "CamStart", SEQUENCE_RENDERNODE_Camera );

	auto prepareCollectorID = ADD_NODE( G_None, "PrepCollector", CRenderNode_PrepareCollector, FEATFLAG_Default );

	auto debugCollectHighPriority = ADD_NODE( G_None, "DebugCollectHighPriority", CRenderNode_CollectDebug, true );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 


	ADD_NODE( G_CullingScene, "DoCulling", CRenderNode_DoCulling, cullingMod::MainScene::YES, cullingMod::Cascades::NO, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::YES );
#ifdef USE_RAY_TRACING
	if ( useRayTracing )
	{
		ADD_NODE( G_CullingRayTracing, "DoCullingRayTracing", CRenderNode_DoCulling, cullingMod::MainScene::NO, cullingMod::Cascades::NO, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::YES, cullingMod::QueryOnlyBoxes::NO );
	}
#endif
	ADD_NODE( G_CullingCascades, "DoCullingCascades", CRenderNode_DoCulling, cullingMod::MainScene::NO, cullingMod::Cascades::YES, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::NO );
	ADD_NODE( G_CullingLocalShadows, "DoCullingLocalShadows", CRenderNode_DoCulling, cullingMod::MainScene::NO, cullingMod::Cascades::NO, cullingMod::DynamicShadows::YES, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::NO );

	auto visDebugID = ADD_NODE( G_PostCullingDebug, "FlushVisiDebug", CRenderNode_FlushVisibilityDebug );
	auto debugCollectLowPriority = ADD_NODE( G_PostCullingDebug, "DebugCollectLowPriority", CRenderNode_CollectDebug, false );

	AddParticleSimThreads( G_ParticlesOnScreenSim, factory, true, false );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto descAllocsID = ADD_NODE( G_RenderPrep, "DeclCommonResAllocs", CRenderNode_DeclareCommonResourceAllocs );

	auto renderPrepID = RENDER_COMMAND_LIST( G_RenderPrep, "RenderPrep" )
	{
		ADD_SUBNODE( "CamResDepScopeOpen", CRenderNode_CameraResourceDependencyScope, true );

#ifdef GPUAPI_EXTENDED_SYNC
		ADD_SUBNODE( "InterCommandListSyncStartFrame", CRenderNode_InterCommandListSyncStartFrame );
#endif

		ADD_SUBNODE( "PrepSceneRendering", CRenderNode_PrepareSceneRendering );

		ADD_SUBNODE( "SkinningCompute", CRenderNode_ComputeSkinningAndTangentUpdates );

		// #HACK: this needs to set some state in GRender.... ewww...
		ADD_SUBNODE( "UpdateGlobalBindings", CRenderNode_UpdateGlobalBindings );

#ifdef USE_RAY_TRACING
		if (GpuApi::GetCapabilities().rayTracingSupport)
		{
			Bool rayTracingEnabled = Config::GGlobalRenderingSettings.allowRaytracing;
#ifndef RED_CONFIGURATION_FINAL
			if (Config::cvRayTracingOverrideEnable.Get())
			{
				rayTracingEnabled = Config::cvRayTracingEnable.Get();
			}
#endif
			if (useRayTracing || !rayTracingEnabled)
			{
				ADD_SUBNODE( "AccelerationStructurePrepare", CRenderNode_AccelerationStructurePrepare, useRayTracing );
			}
		}
#endif // USE_RAY_TRACING
	}

	RENDER_COMMAND_LIST( G_Render, "PrepareGloballyAccessibleSystems" )
	{
		//we don't want to attach CRenderNode_BindGlobalConstants here because it would bind Wind Volume texture as SRV 
		//which is bound in CRenderNode_WindImpulseVolumeUpdate as UAV and invalidate the SRV which could lead to possible state mismatch in further nodes 
		//in the same command list
		ADD_SUBNODE( "WindImpulseVolumeGen", CRenderNode_WindImpulseVolumeUpdate );
		ADD_SUBNODE( "PrepareAutoSpawnOnTerrain", CRenderNode_PrepareAutoSpawnOnTerrain );

		ADD_SUBNODE( "RenderTopDownCarProxies", CRenderNode_RenderTopDownCarProxies );
		ADD_SUBNODE( "RenderTopDownRainDepth", CRenderNode_RenderTopDownRainDepth );

		ADD_SUBNODE( "UpdateParticleRenderData", CRenderNode_UpdateParticlesRenderData );

		ADD_SUBNODE( "SpawnDynamicDecals", CRenderNode_SpawnDynamicDecals );

		ADD_SUBNODE( "GlobalSystemsSetCommonUsage", CRenderNode_GlobalSystemsSetCommonUsage, true );
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// GBuffer
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	RENDER_COMMAND_LIST( G_Render, "DepthPrepass" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		// TODO maybe only clear depth here and do color targets clears in other passes?
		ADD_SUBNODE( "SetRenderToDepth", CRenderNode_SetRenderTargetsGBuffer, targetMod::ClearGBuffer|targetMod::WriteDepth_YES|targetMod::DepthOnly );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_DepthPrepass ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_depth_prepass", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_depth_prepass", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndRenderToDepth", CRenderNode_EndRenderTargetsGBuffer );

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "DepthPrepassPreview", CRenderNode_DepthPrepassPreview );
#endif

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	if ( cameraFeatures.Get( RFF_VelocityBuffer ) )
	{
		RENDER_COMMAND_LIST( G_Render, "GBufferEarly_Velocity_Weapon" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "BeginRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, true );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
				, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_velbuff_regular", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_velbuff_discarded", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "EndRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, false );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

	for ( Int32 cpK = 0; cpK < CpReSplitN(); ++cpK ) // [skperf] round 33 reSplit (1 list = original)
	{
	RENDER_COMMAND_LIST( G_Render, c_cpReSplitSolidNames[ cpK ] )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff0", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, CpReSplitN() > 1 ? CpReSplitCond( cpK ) : &CpGBufferCond
			, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_regular", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
		);

		ADD_SUBNODE( "EndRenderToGbuff0", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}
	}

	RENDER_COMMAND_LIST( G_Render, "GBuffer_Discard" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff0", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_discarded", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
		);

		ADD_SUBNODE( "AutoSpawnOnTerrain", CRenderNode_AutoSpawnOnTerrain );

		ADD_SUBNODE( "EndRenderToGbuff0", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );

		// gbuffer stuff that should be rendered as last (e.g. terrain)

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff0", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBufferLate ); }
			, REBatch( REBF_CAM_MainWeapon,	"renderstage_late_gbuffer_regular",		SRenderStageCluster::Init( RPl_Weapon ),	SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene,	"renderstage_late_gbuffer_regular",		SRenderStageCluster::Init( RPl_Scene ),		SM_OptimizedDistanceBatching )
			, REBatch( REBF_CAM_MainWeapon,	"renderstage_late_gbuffer_discarded",	SRenderStageCluster::Init( RPl_Weapon ),	SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene,	"renderstage_late_gbuffer_discarded",	SRenderStageCluster::Init( RPl_Scene ),		SM_OptimizedDistanceBatching )
		);

		ADD_SUBNODE( "EndRenderToGbuff0", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

#ifndef RED_CONFIGURATION_FINAL
	if ( cameraFeatures.Get( RFF_DebugColoring ) )
	{
		RENDER_COMMAND_LIST( G_Render, "GBuffer_DebugColor_Solid" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "SetRenderToGbuff0", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
				, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_regular_debugcoloring", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_regular_debugcoloring", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
			);

			ADD_SUBNODE( "EndRenderToGbuff0", CRenderNode_EndRenderTargetsGBuffer );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}

		RENDER_COMMAND_LIST( G_Render, "GBuffer_DebugColor_Discard" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "SetRenderToGbuff0", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
				, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_discarded_debugcoloring", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_discarded_debugcoloring", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
			);

			ADD_SUBNODE( "EndRenderToGbuff0", CRenderNode_EndRenderTargetsGBuffer );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
#endif // RED_CONFIGURATION_FINAL

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// GBuffer + Velocity Buffer
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	if ( cameraFeatures.Get( RFF_VelocityBuffer ) )
	{
		for ( Int32 cpK = 0; cpK < CpReSplitN(); ++cpK ) // [skperf] round 33 reSplit (1 list = original)
		{
		RENDER_COMMAND_LIST( G_Render, c_cpReSplitVelNames[ cpK ] )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			if ( cpK == 0 ) ADD_SUBNODE( "InitVelocityBuffer", CRenderNode_InitVelocityBuffer ); // the clear stays in the first slice only

			ADD_SUBNODE( "BeginRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, true );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, CpReSplitN() > 1 ? CpReSplitCond( cpK ) : &CpGBufferCond
				, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_velbuff_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedDistanceBatching )
			);

			ADD_SUBNODE( "EndRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, false );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
		} // [skperf] round 33 reSplit for


		RENDER_COMMAND_LIST( G_Render, "GBufferVelocity_Discard" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "BeginRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, true );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
				, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_velbuff_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "EndRenderToGbuff0WithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, false );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Opaque Hair - render targets clears
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	if ( doAsyncHairClear )
	{
		SYNC_SUBMIT( G_None, "RenderGraphCamera_GBuffer", GpuApi::CommandListSyncType::ForkAsyncCompute );

		COMPUTE_COMMAND_LIST( G_None, "Hair_RT_Clears" )
		{
			ADD_SUBNODE( "Hair_ClearDepths_CopySceneDepth", CRenderNode_HairFullscreenPass, HairRenderStage::FS_ClearDepths_CopySceneDepth );
		}
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Decals / GBuffer mods
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	for ( Int32 cpK = 0; cpK < CpDecalSplitN(); ++cpK ) // [skperf] round 54 decalSplit (1 list = original)
	{
	RENDER_COMMAND_LIST( G_Render, c_cpDecalSplitNames[ cpK ] )
	{
		if ( cpK == 0 ) ADD_SUBNODE( "PrepareFeedbackNormalBuffer", CRenderNode_PrepareFeedbackNormalBuffer );
		else ADD_SUBNODE( "BindFeedbackNormalCopy", CpRenderNode_BindFeedbackNormalCopy );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "SetRenderToGbuff", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_NO );

		// don't draw decals on top of debug-colored meshes
		if ( !cameraFeatures.Get( RFF_DebugColoring ) )
		{
			ADD_SUBNODE( "RenderElements_PostGBuffer", CRenderNode_RenderElements
				, CpDecalSplitN() > 1 ? CpDecalSplitCondFor( cpK ) : &CpDeferredDecalsCond // [skperf] round 53 mirrorDecals / round 54 decalSplit
				, REBatch( REBF_CAM_MainWeapon, "renderstage_post_gbuffer", SRenderStageCluster::Init( RPl_Weapon ), SM_ByPriority )
				, REBatch( REBF_CAM_MainScene, "renderstage_post_gbuffer", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);
		}

		ADD_SUBNODE( "EndRenderToGbuff", CRenderNode_EndRenderTargetsGBuffer );

		if ( cpK == CpDecalSplitN() - 1 ) ADD_SUBNODE( "EndGBuffer1Feedback", CRenderNode_EndGBuffer1Feedback );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}
	} // [skperf] round 54 decalSplit for

	if ( doAsyncHairClear )
	{
		SYNC_SUBMIT( G_None, "RenderGraphCamera_DecalsDuringHairClear", GpuApi::CommandListSyncType::JoinAsyncCompute );
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Opaque Hair
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	RENDER_COMMAND_LIST( G_Render, "Hair_Opaque" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		if ( !doAsyncHairClear )
		{
			ADD_SUBNODE( "Hair_ClearDepths_CopySceneDepth", CRenderNode_HairFullscreenPass, HairRenderStage::FS_ClearDepths_CopySceneDepth );
		}

		// Hair alpha/depth accumulation
		ADD_SUBNODE( "HairAlpha_SetRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_AlphaAccum, HairRenderTargetsOp::Set );
		ADD_SUBNODE( "RenderElements_AlphaAccum", CRenderNode_RenderElements
			, IsHairEnabled
			, REBatch( REBF_CAM_MainWeapon, "renderstage_hair_alpha_accum", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_hair_alpha_accum", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "HairAlpha_EndRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_AlphaAccum, HairRenderTargetsOp::Unset );

		// Resolve weights and alpha
		ADD_SUBNODE( "Hair_ResolveAlphaWeights_ResolveDepthFar", CRenderNode_HairFullscreenPass, HairRenderStage::FS_ResolveAlphaWeights_ResolveDepthFarSolid );

		// Blend base color
		ADD_SUBNODE( "HairBaseColor_SetRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_BaseColorBlend, HairRenderTargetsOp::Set );
		ADD_SUBNODE( "RenderElements_BaseColorBlend", CRenderNode_RenderElements
			, IsHairEnabled
			, REBatch( REBF_CAM_MainScene, "renderstage_hair_basecolor_blend", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "HairBaseColor_EndRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_BaseColorBlend, HairRenderTargetsOp::Unset );

		// Draw solid hair to gbuffer
		ADD_SUBNODE( "HairGbuffer_SetGbuff", CRenderNode_SetRenderTargetsGBuffer, rt_GBuffer_NoClear );
		ADD_SUBNODE( "HairGbuffer_SetHairRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_GBufferSolid, HairRenderTargetsOp::Set );
		ADD_SUBNODE( "RenderElements_GBufferSolid", CRenderNode_RenderElements
			, IsHairEnabled
			, REBatch( REBF_CAM_MainScene, "renderstage_hair_gbuffer_solid", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "HairGbuffer_EndHairRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_GBufferSolid, HairRenderTargetsOp::Unset );
		ADD_SUBNODE( "HairGbuffer_EndGbuff", CRenderNode_EndRenderTargetsGBuffer );

		if ( cameraFeatures.Get( RFF_VelocityBuffer ) )
		{
			ADD_SUBNODE( "HairGbuffer_SetGbuffWithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, true );
			ADD_SUBNODE( "HairGbuffer_SetHairRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_GBufferSolid, HairRenderTargetsOp::Set );
			ADD_SUBNODE( "RenderElements_Velocity", CRenderNode_RenderElements
				, IsHairEnabled
				, REBatch( REBF_CAM_MainScene, "renderstage_hair_gbuffer_velbuff_solid", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);
			ADD_SUBNODE( "HairGbuffer_EndHairRT", CRenderNode_HairRenderTargets, HairRenderStage::Geom_GBufferSolid, HairRenderTargetsOp::Unset );
			ADD_SUBNODE( "HairGbuffer_EndGbuffWithVel", CRenderNode_SetRenderTargetsGBufferWithVelocityBuffer, false );
		}

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Classify materials - needed for effective deferred lighting
	// We can kick it as soon as we fill G-buffer, and it needs to be done before deferred lighting pass
	// TODO: move it to async compute
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "PostGBufferOverrides" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		if ( cameraFeatures.Get( RFF_VelocityBuffer ) )
		{
			ADD_SUBNODE( "SetRenderToVelBuf", CRenderNode_SetupRenderTargetsVelocityBuffer, true );

			ADD_SUBNODE( "RenderElements_VelocityOnly", CRenderNode_RenderElements
				, REBatch( REBF_CAM_MainWeapon, "renderstage_velocitybuffer", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, "renderstage_velocitybuffer", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "EndRenderToVelBuf", CRenderNode_SetupRenderTargetsVelocityBuffer, false );
		}

		// As a first one just to be sure that if for some reason we decide to use it in e.g. CRenderNode_ScreenSpaceRain it is not forgotten to be called.
		ADD_SUBNODE( "CorrectWeaponPlaneDepth", CRenderNode_ModifyDepth, eDepthModificationType::CorrectWeaponPlane );

		// Needs weapon-plane-corrected depth
		ADD_SUBNODE( "BuildTemporalInvalidationMask", CRenderNode_BuildTemporalInvalidationMask );

		ADD_SUBNODE( "ScreenSpaceRain", CRenderNode_ScreenSpaceRain );

		if ( !doAsyncFlattenNormals )
		{
			ADD_SUBNODE( "FlattenNormals", CRenderNode_FlattenNormals );
		}
		if ( !doAsyncBuildDepthChain )
		{
			ADD_SUBNODE( "BuildDepthChain", CRenderNode_BuildDepthChain );
		}

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	if ( !doAsyncDynamicTextureGeneration )
	{
		RENDER_SIMPLE_COMMAND_LIST( G_None, "GenerateDynamicTextures", CRenderNode_GenerateAsyncDynamicTextures );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

#ifdef USE_RAY_TRACING
	auto accelerationStructureUpdateStatic = CRenderNodeGraph::INVALID_ITEM_ID;
	auto accelerationStructureUpdateDynamic = CRenderNodeGraph::INVALID_ITEM_ID;
	auto accelerationStructureUpdateEpilogue = CRenderNodeGraph::INVALID_ITEM_ID;

	if ( useRayTracing && !rayTracingAsyncASBuild )
	{
		accelerationStructureUpdateStatic = RENDER_COMMAND_LIST( G_Render, "AccelerationStructureUpdateStatic" )
		{
			ADD_SUBNODE( "AccelerationStructureUpdateStatic", CRenderNode_AccelerationStructureUpdateStatic );
		}
		accelerationStructureUpdateDynamic = RENDER_COMMAND_LIST( G_Render, "AccelerationStructuresUpdateDynamic" )
		{
			ADD_SUBNODE( "AccelerationStructureUpdateDynamic", CRenderNode_AccelerationStructureUpdateDynamic );
		}
		accelerationStructureUpdateEpilogue = RENDER_COMMAND_LIST( G_Render, "AccelerationStructuresUpdateEpilogue" )
		{
			ADD_SUBNODE( "AccelerationStructureUpdateEpilogue", CRenderNode_AccelerationStructureUpdateEpilogue );
		}
	}
#endif


	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Async Compute work during shadowmap rendering
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	if ( enableAsyncComputeDuringShadowmaps )
	{
		SYNC_SUBMIT( G_None, "RenderGraphCamera_FinishGBuffer", GpuApi::CommandListSyncType::ForkAsyncCompute );

#ifdef USE_RAY_TRACING
		if ( useRayTracing && rayTracingAsyncASBuild )
		{
			accelerationStructureUpdateStatic = COMPUTE_COMMAND_LIST( G_Render, "AccelerationStructureUpdateStatic" )
			{
				ADD_SUBNODE( "AccelerationStructureUpdateStatic", CRenderNode_AccelerationStructureUpdateStatic );
			}
			accelerationStructureUpdateDynamic = COMPUTE_COMMAND_LIST( G_Render, "AccelerationStructuresUpdateDynamic" )
			{
				ADD_SUBNODE( "AccelerationStructureUpdateDynamic", CRenderNode_AccelerationStructureUpdateDynamic );
			}
			accelerationStructureUpdateEpilogue = COMPUTE_COMMAND_LIST( G_Render, "AccelerationStructuresUpdateEpilogue" )
			{
				ADD_SUBNODE( "AccelerationStructureUpdateEpilogue", CRenderNode_AccelerationStructureUpdateEpilogue );
			}
		}
#endif

		COMPUTE_COMMAND_LIST( G_Render, "AsyncComputeDuringShadowmaps" )
		{
			// Don't need global constants for SSAO, but if we do something that does need it, should be able to add.
			// Only thing is we can only bind CS resources, and can't bind shadowmaps (since they'll be written to at
			// the same time).
			//ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			if ( doAsyncFlattenNormals )
			{
				ADD_SUBNODE( "FlattenNormals", CRenderNode_FlattenNormals );
			}
			if ( doAsyncBuildDepthChain )
			{
				ADD_SUBNODE( "BuildDepthChain", CRenderNode_BuildDepthChain );
			}

			// #NOTE: If anything is added above here, it should be handled in CRenderNode_WaitIntermediateSyncPoint
			// below. Need to be sure resource states are consistent.

#ifdef GPUAPI_EXTENDED_SYNC
			ADD_SUBNODE( "SignalDepthChainComplete", CRenderNode_SignalIntermediateSyncPoint, sync_DepthChain );
#endif

			if ( doAsyncSSAO )
			{
				ADD_SUBNODE( "RenderSSAO", CRenderNode_DrawConeAO );
			}
			if ( doAsyncLutGeneration )
			{
				ADD_SUBNODE( "GenerateTonemappingLUT", CRenderNode_GenerateTonemappingLUT );
			}
			if ( doAsyncDynamicTextureGeneration )
			{
				ADD_SUBNODE( "GenerateAsyncDynamicTextures", CRenderNode_GenerateAsyncDynamicTextures );
			}

			//ADD_SUBNODE( "CRenderNode_UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		};
	}

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	//
	// Shadowmaps
	//
	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	auto renderDistantShadowsID = RENDER_SIMPLE_COMMAND_LIST( G_None, "RenderDistantShadows", CRenderNode_RenderDistantShadows );

#ifndef RED_CONFIGURATION_FINAL
	red::DynArray< CRenderNodeGraph::ItemId > idsRenderCascadesShadowmaps{ red::PoolDebug() };
#endif

	RED_STATIC_ASSERT( MAX_CASCADES == 4 );
	for ( Uint32 cascadeIndex = 0; cascadeIndex < Config::GGlobalRenderingSettings.NumShadowCascades; cascadeIndex++ )
	for ( Int32 cpK = 0; cpK < CpCascSplitN(); ++cpK ) // [skperf] round 52 cascSplit (1 list per cascade = original)
	{
		char commandListName[ 64 ];
		if ( cpK == 0 ) red::SNPrintFSafe( commandListName, sizeof( commandListName ), "RenderCascade%u", cascadeIndex );
		else red::SNPrintFSafe( commandListName, sizeof( commandListName ), "RenderCascade%u_s%d", cascadeIndex, cpK );

#ifndef RED_CONFIGURATION_FINAL
		auto idRenderCascadesShadowmaps =
#endif
		RENDER_COMMAND_LIST( G_Render, commandListName )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			if ( CpCascSplitN() > 1 ) ADD_SUBNODE( "RenderCascade0", CpRenderNode_RenderShadowCascadeSlice, cascadeIndex, cpK, CpCascSplitN() );
			else ADD_SUBNODE( "RenderCascade0", CRenderNode_RenderShadowCascade, cascadeIndex );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}

#ifndef RED_CONFIGURATION_FINAL
		idsRenderCascadesShadowmaps.PushBack( idRenderCascadesShadowmaps );
#endif
	}

#ifndef RED_CONFIGURATION_FINAL
	red::DynArray< CRenderNodeGraph::ItemId > idsRenderLocalShadowmaps { red::PoolDebug() };
#endif

	for ( Uint32 lightIndex = 0; lightIndex < Config::GGlobalRenderingSettings.LocalShadowsProcessedPerFrame; lightIndex++ )
	{
		char commandListName[ 64 ];
		red::SNPrintFSafe( commandListName, sizeof( commandListName ), "RenderLocalShadows%u", lightIndex );

#ifndef RED_CONFIGURATION_FINAL
		auto idRenderLocalShadowmaps = 
#endif
		RENDER_COMMAND_LIST( G_Render, commandListName )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "RenderLocalShadows", CRenderNode_RenderLocalShadowMaps, lightIndex );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}

#ifndef RED_CONFIGURATION_FINAL
		idsRenderLocalShadowmaps.PushBack( idRenderLocalShadowmaps );
#endif
	}

	// TODO check on shadows-heavy location if we can have a single commandlist
	if ( 0 )
	{
		RENDER_COMMAND_LIST( G_Render, "RenderLocalShadows" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			for ( Uint32 lightIndex = 0; lightIndex < Config::GGlobalRenderingSettings.LocalShadowsProcessedPerFrame; lightIndex++ )
			{
				char nodeName[ 64 ];
				red::SNPrintFSafe( nodeName, sizeof( nodeName ), "RenderLocalShadows%u", lightIndex );
				ADD_SUBNODE( nodeName, CRenderNode_RenderLocalShadowMaps, lightIndex );
			}

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

#ifndef GPUAPI_EXTENDED_SYNC
	if ( enableAsyncComputeDuringShadowmaps )
	{
		SYNC_SUBMIT( G_None, "RenderGraphCamera_Shadows", GpuApi::CommandListSyncType::JoinAsyncCompute );
	}
#endif

	////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	auto idLighting = RENDER_COMMAND_LIST( G_Render, "Lighting" )
	{
#ifdef GPUAPI_EXTENDED_SYNC
		if ( enableAsyncComputeDuringShadowmaps )
		{
			ADD_SUBNODE( "WaitDepthChainComplete", CRenderNode_WaitIntermediateSyncPoint, sync_DepthChain,
				[]( const SRenderNodeImplContext& rctx )
				{
					// After depth chain and flattened normals are done, they're in a UAV state on the compute queue. We
					// need to tell GpuApi that, otherwise it may add some additional state changes that we don't want.
					auto rtHiDepth = rctx.RT< RFUF_Read >( "hierarchicalDepthBuffer" );
					auto rtMaxDepth = rctx.RT< RFUF_Read >( "maxDepthBuffer" );
					auto rtNormals	= rctx.RT<RFUF_Read>( "normals" );
					if ( rctx.IsConsumePhase() )
					{
						GpuApi::TellTextureUAV( rtHiDepth );
						GpuApi::TellTextureUAV( rtMaxDepth );
						GpuApi::TellTextureUAV( rtNormals );
					}
				} );
		}
#endif
		ADD_SUBNODE( "GlobalSystemsSetCommonUsage", CRenderNode_GlobalSystemsSetCommonUsage, false );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "VolumetricCloudsLighting", CRenderNode_RenderVolumetricCloudsLighting ); //this has to execute here for CRenderNode_RenderSkyScattering to have a valid cloudsShadow texture
		ADD_SUBNODE( "RenderSkyScattering", CRenderNode_RenderSkyScattering );

		ADD_SUBNODE( "RenderLightChannelVolumes", CRenderNode_RenderLightChannelVolumes );
		ADD_SUBNODE( "RenderGIVolumes", CRenderNode_RenderGIVolumes );

		if ( !doAsyncSSAO )
		{
			ADD_SUBNODE( "RenderSSAO", CRenderNode_DrawConeAO );
		}

#ifdef USE_RAY_TRACING
		ADD_SUBNODE( "MarkTransparentReflection", CRenderNode_MarkTransparentReflection );
		ADD_SUBNODE( "RenderRayTracedDirectionalShadow", CRenderNode_RenderRayTracedDirectionalShadow );
#endif

		ADD_SUBNODE( "ExposureUpdate", CRenderNode_HistogramUpdate, CRenderNode_HistogramUpdate::eFRAME_EXPOSURE );
		ADD_SUBNODE( "ClusteredLightsCull", CRenderNode_ClusteredLightsCull );
		ADD_SUBNODE( "RenderShadowMask", CRenderNode_RenderShadowmask );
		ADD_SUBNODE( "Classify_Materials", CRenderNode_ClassifyMaterials );
		ADD_SUBNODE( "ReflectionProbes", CRenderNode_ReflectionProbes );
		ADD_SUBNODE( "GlobalIllumination", CRenderNode_GlobalIllumination );
		ADD_SUBNODE( "VolumetricFog", CRenderNode_VolumetricFog );

#ifdef USE_RAY_TRACING
		ADD_SUBNODE( "RenderRayTracedAmbientOcclusion", CRenderNode_RenderRayTracedAmbientOcclusion );
		ADD_SUBNODE( "RenderRayTracedReflections", CRenderNode_RenderRayTracedReflections );
		ADD_SUBNODE( "RayTracingFilterOutput", CRenderNode_RayTracingFilterOutput );
#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "RayTracingRenderDebug", CRenderNode_RayTracingRenderDebug );
#endif // RED_CONFIGURATION_FINAL
#endif // USE_RAY_TRACING

		// CRenderNode_RenderLightBuffers may require "shadowMask" from CRenderNode_RayTracingFilterOutput
		ADD_SUBNODE( "LocalLightBuffersRender", CRenderNode_RenderLightBuffers );
		ADD_SUBNODE( "BackgroundRender", CRenderNode_RenderBackground );

		if ( !doAsyncSSR )
		{
			ADD_SUBNODE( "ScreenSpaceReflections", CRenderNode_ScreenSpaceReflections );
			ADD_SUBNODE( "IntegrateLightsRender", CRenderNode_RenderLightsIntegrate, RenderLightsIntegratePass::CalcGI );
		}

#ifdef GPUAPI_EXTENDED_SYNC
		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	if (enableAsyncComputeDuringShadowmaps)
	{
		SYNC_SUBMIT(G_None, "RenderGraphCamera_ShadowsAndLighting", GpuApi::CommandListSyncType::JoinAsyncCompute);
	}

	// Running SSR on async compute at the same time as GI worth 725us saving at MB on Scorpio
	CRenderNodeGraph::ItemId idLightingMid;
	if ( doAsyncSSR )
	{
		SYNC_SUBMIT( G_None, "RenderGraphCamera_ForkSSR", GpuApi::CommandListSyncType::ForkAsyncCompute );
		COMPUTE_COMMAND_LIST( G_Render, "AsyncSSRDuringGI" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants, GpuWrapApi::ComputeShader );

			ADD_SUBNODE( "ScreenSpaceReflections", CRenderNode_ScreenSpaceReflections );

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants, GpuWrapApi::ComputeShader );
		}
		idLightingMid = RENDER_COMMAND_LIST( G_Render, "LightingMid" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

			ADD_SUBNODE( "IntegrateLightsRender", CRenderNode_RenderLightsIntegrate, RenderLightsIntegratePass::CalcGI_LimitedOccupancy );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
		SYNC_SUBMIT(G_None, "RenderGraphCamera_SSR", GpuApi::CommandListSyncType::JoinAsyncCompute );
	}

	auto idLightingLate = RENDER_COMMAND_LIST(G_Render, "LightingLate")
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
#endif

		ADD_SUBNODE( "IntegrateLightsRender", CRenderNode_RenderLightsIntegrate, RenderLightsIntegratePass::Integrate );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	auto idDecoupledParticleLighting = RENDER_COMMAND_LIST(G_Render, "DecoupledParticleLighting")
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants, GpuWrapApi::ComputeShader );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants, false, false, GpuWrapApi::ComputeShader );

		ADD_SUBNODE( "DecoupledParticleLighting", CRenderNode_DecoupledParticleLighting );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants, false, false, GpuWrapApi::ComputeShader );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants, GpuWrapApi::ComputeShader );
	}

#ifndef RED_CONFIGURATION_FINAL
	CRenderNodeGraph::ItemId idParticleNumLights;

	if ( displayMode == EMM_DecalOverdraw )
	{
		RENDER_COMMAND_LIST( G_Render, "DeferredDecals_Overdraw" )
		{
			ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsDebug, targetMod::ClearColor );

			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
				// NOTE: we can use optimal sorting for overdraw visualization
				, REBatch( REBF_CAM_MainWeapon, "renderstage_decal_overdraw", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, "renderstage_decal_overdraw", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsDebug );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
	else if ( displayMode == EMM_ParticleOverdraw )
	{
		RENDER_COMMAND_LIST( G_Render, "Particle_Overdraw" )
		{
			ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsDebug, targetMod::ClearColor );
			ADD_SUBNODE( "PrepareFeedbackColorBuffer", CRenderNode_PrepareFeedbackColorBuffer, true );

			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

			ADD_SUBNODE( "RenderElements_Transparent", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Particles ); }
				, REBatch( REBF_CAM_MainScene, "renderstage_particle_overdraw", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);

			ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsDebug );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
	else if ( displayMode == EMM_ParticleNumLights )
	{
		idParticleNumLights = RENDER_COMMAND_LIST( G_Render, "Particle_NumLights" )
		{
			ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsDebug, targetMod::ClearColor );
			ADD_SUBNODE( "PrepareFeedbackColorBuffer", CRenderNode_PrepareFeedbackColorBuffer, true );

			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

			ADD_SUBNODE( "RenderElements_Transparent", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Particles ); }
				, REBatch( REBF_CAM_MainScene, "renderstage_count_lights", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);

			ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsDebug );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

#endif // RED_CONFIGURATION_FINAL

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "Skin_Post_and_Forward_Opaque" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants, true, true, GpuWrapApi::PixelShader );

		ADD_SUBNODE( "SkinBackDepthForTranslucency", CRenderNode_RenderSkinBackDepthForTranslucency );
		ADD_SUBNODE( "SetRT_SSS_Emissive", CRenderNode_SetRT_SSS_Emissive );
		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_subsurface_emissive", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "EndRT_SSS_Emissive", CRenderNode_EndRT_SSS_Emissive );
		ADD_SUBNODE( "SSS_Blur", CRenderNode_SSS_Blur );

		ADD_SUBNODE( "BindEnvProbesConstantBuffer", CRenderNode_BindEnvProbes );

		ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsMain, rt_Unlit|targetMod::WriteDepth_NO );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Unlit ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_unlit_no_depth", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_unlit_no_depth", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_Unlit|targetMod::WriteDepth_NO );

		ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsMain, rt_Unlit|targetMod::WriteDepth_YES );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Unlit ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_unlit", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_unlit", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "UnbindEnvProbesConstantBuffer", CRenderNode_UnbindEnvProbes );

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "DebugForwardRender", CRenderNode_RenderDebugSystems, CRenderNode_RenderDebugSystems::eGraphPhase::GP_ForwardOpaque );
#endif // RED_CONFIGURATION_FINAL

		ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_Unlit|targetMod::WriteDepth_YES );

		ADD_SUBNODE( "ApplyGLobalFog", CRenderNode_RenderFogOverlay );

		ADD_SUBNODE( "ScreenSpacePlanarReflections", CRenderNode_ScreenSpacePlanarReflections );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants, true, true, GpuWrapApi::PixelShader );

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "RenderElements_TopDownCarProxyDebug", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_TopDownCarProxyDebug ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_top_down_car_proxy_debug", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
#endif // RED_CONFIGURATION_FINAL

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	if ( cameraFeatures.Get( RFF_GBufferExtraction ) )
	{
		RENDER_SIMPLE_COMMAND_LIST( G_Render, "ExtractionGBuffer", CRenderNode_ExtractionGBuffer );
	}
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto idHologramsDepthDistortion = RENDER_COMMAND_LIST( G_Render, "HologramDepth_and_Distortion" )
	{
#ifdef RED_PLATFORM_DURANGO
		// #TODO: Should be possible to set up MoveColorToESRAM to also swap in the colorCopy target, and then be able
		// to skip the PrepareFeedbackColorBuffer below. When I tried just swapping render targets, it didn't give the
		// results I was looking for. Needs more investigation.
		if ( GpuApi::GetCapabilities().esram )
		{
			ADD_SUBNODE( "MoveColorToESRAM", CRenderNode_MoveColorToESRAM );
		}
#endif // RED_PLATFORM_DURANGO

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "SetDepthBuffer", CRenderNode_SetHologramDepthBuffer );
		ADD_SUBNODE( "RenderElements_Hologram_Depth", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_hologram_depth", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "EndDepthBuffer", CRenderNode_EndHologramDepthBuffer );

		ADD_SUBNODE( "SetRTDistortion", CRenderNode_SetRenderTargetDistortion );
		ADD_SUBNODE( "RenderElements_Distortion", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_distortion", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
		);
		ADD_SUBNODE( "ScreenSpaceHeatHaze", CRenderNode_ScreenSpaceHeatHaze );
		ADD_SUBNODE( "EndRTDistortion", CRenderNode_EndRenderTargetDistortion );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		ADD_SUBNODE( "ResolveDistortion", CRenderNode_ResolveDistortion );
	}

	auto idTransparents = RENDER_COMMAND_LIST( G_Render, "Transparents" )
	{
		if ( displayMode != EMM_ParticleNumLights && displayMode != EMM_ParticleOverdraw )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
			ADD_SUBNODE( "BindEnvProbesConstantBuffer", CRenderNode_BindEnvProbes );

			ADD_SUBNODE( "VolumetricClouds", CRenderNode_RenderVolumetricClouds );

			ADD_SUBNODE( "SetRTsMainRWDepth", CRenderNode_SetRenderTargetsMain, rt_TransparentRWDepth );
			ADD_SUBNODE( "RenderElements_Depth_Write", CRenderNode_RenderElements
				, CpTransSplitN() > 1 ? &CpForwardNoSliceCond : static_cast< CRenderNode_RenderElements::RenderConditionFunc* >( []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); } ) // [skperf] round 58
				, REBatch( REBF_CAM_MainScene, "renderstage_transparent_depth_write", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);
			ADD_SUBNODE( "EndRTsMainRWDepth", CRenderNode_EndRenderTargetsMain, rt_TransparentRWDepth );

			ADD_SUBNODE( "SetRTsMainRODepth", CRenderNode_SetRenderTargetsMain, rt_TransparentBackground );

			ADD_SUBNODE( "RenderElements_Transparent_Background", CRenderNode_RenderElements
				, CpTransSplitN() > 1 ? &CpForwardNoSliceCond : static_cast< CRenderNode_RenderElements::RenderConditionFunc* >( []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); } ) // [skperf] round 58
				, REBatch( REBF_CAM_MainScene, "renderstage_transparent_background", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);
			if ( CpTransSplitN() > 1 ) // [skperf] round 58 transSplit: back faces whole, then slice 0 of the big batch
			{
				ADD_SUBNODE( "RenderElements_Scene_BackFace", CRenderNode_RenderElements
					, &CpForwardNoSliceCond
					, REBatch( REBF_CAM_MainScene, "renderstage_transparent_back_face", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
				);
				ADD_SUBNODE( "RenderElements_Scene", CRenderNode_RenderElements
					, CpTransSplitCondFor( 0 )
					, REBatch( REBF_CAM_MainScene, "renderstage_transparent", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
				);
			}
			else
			{
			ADD_SUBNODE( "RenderElements_Scene", CRenderNode_RenderElements
				, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
				, REBatch( REBF_CAM_MainScene, "renderstage_transparent_back_face", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
				, REBatch( REBF_CAM_MainScene, "renderstage_transparent", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);
			}

			ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_TransparentBackground );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
	decltype( idTransparents ) cpTransSliceIds[ 3 ] = {}; // [skperf] round 58 transSplit: slices 1..N-1
	for ( Int32 cpK = 1; cpK < CpTransSplitN(); ++cpK )
	{
	cpTransSliceIds[ cpK - 1 ] = RENDER_COMMAND_LIST( G_Render, c_cpTransSplitNames[ cpK ] )
	{
		if ( displayMode != EMM_ParticleNumLights && displayMode != EMM_ParticleOverdraw )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
			ADD_SUBNODE( "BindEnvProbesConstantBuffer", CRenderNode_BindEnvProbes );
			ADD_SUBNODE( "SetRTsMainRODepth", CRenderNode_SetRenderTargetsMain, rt_TransparentBackground );
			ADD_SUBNODE( "RenderElements_Scene", CRenderNode_RenderElements
				, CpTransSplitCondFor( cpK )
				, REBatch( REBF_CAM_MainScene, "renderstage_transparent", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);
			ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_TransparentBackground );
			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
	}

	auto idTransparentsWeaponScreen = RENDER_COMMAND_LIST( G_Render, "Transparents_WeaponAndScreen" )
	{
		if ( displayMode != EMM_ParticleNumLights && displayMode != EMM_ParticleOverdraw )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
			ADD_SUBNODE( "BindEnvProbesConstantBuffer", CRenderNode_BindEnvProbes );

			ADD_SUBNODE( "PrepareFeedbackColorBuffer", CRenderNode_PrepareFeedbackColorBuffer, true );

			ADD_SUBNODE( "SetRTsMainRODepth", CRenderNode_SetRenderTargetsMain, rt_Transparent_WeaponAndScreen );

			ADD_SUBNODE( "RenderElements_ScreenSpaceVfx", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
				, REBatch( REBF_CAM_MainScene, "renderstage_screenspace_vfx", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			);

			ADD_SUBNODE( "RenderElements_Weapon", CRenderNode_RenderElements
				, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
				, REBatch( REBF_CAM_MainWeapon | REBF_CAM_DisableSqueezedDepthInWeaponPlane, "renderstage_transparent_back_face", SRenderStageCluster::Init( RPl_Weapon ), SM_ByPriority )
				, REBatch( REBF_CAM_MainWeapon | REBF_CAM_DisableSqueezedDepthInWeaponPlane, "renderstage_transparent", SRenderStageCluster::Init( RPl_Weapon ), SM_ByPriority )
			);

			ADD_SUBNODE( "ScreenSpaceUnderwater", CRenderNode_ScreenSpaceUnderwater );

			ADD_SUBNODE( "UnbindEnvProbesConstantBuffer", CRenderNode_UnbindEnvProbes );
			ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_Transparent_WeaponAndScreen );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

#if SURFACE_CACHE_ENABLE_RUNTIME_BAKING
	RENDER_SIMPLE_COMMAND_LIST( G_Render, "SurfaceCacheGpuRequests", CRenderNode_SurfaceCacheAddGpuRequests );
#endif

	RENDER_COMMAND_LIST( G_Render, "TXAA_Blurs" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

#ifdef USE_DLSS
		ADD_SUBNODE( "ApplyDLSS", CRenderNode_ApplyDLSS );
#endif // USE_DLSS
		ADD_SUBNODE( "ApplyTXAA", CRenderNode_ApplyTXAA );
		ADD_SUBNODE( "PrepareFeedbackSSR", CRenderNode_PrepareFeedbackSSRBuffer );
		ADD_SUBNODE( "ApplyDoF", CRenderNode_DepthOfFieldSeparable );

#if 1	// TODO: Once final motion blur gets implemented and tested, we can choose whether to delete the original nodes.
		ADD_SUBNODE( "CombinedBlurs", CRenderNode_CombinedBlurs );
#else
		ADD_SUBNODE( "ApplyMotionBlur", CRenderNode_ApplyMotionBlur );
		ADD_SUBNODE( "ApplyWeaponPlaneBlur", CRenderNode_WeaponPlaneBlur );
#endif

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	// Forward stuff after TXAA - like on-scene placed HUD's and stuff
	RENDER_COMMAND_LIST( G_Render, "Forward_NoTXAA" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
		ADD_SUBNODE( "BindEnvProbesConstantBuffer", CRenderNode_BindEnvProbes );

		ADD_SUBNODE( "SetRTs_OpaqueNoTXAA", CRenderNode_SetRenderTargetsMain, rt_OpaqueNoTxaa );

		ADD_SUBNODE( "RenderElements_OpaqueNoTXAA", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_ForwardNoTXAA ); }
			, REBatch( REBF_CAM_MainScene|REBF_CAM_DisableJitter, "renderstage_opaque_notxaa", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon|REBF_CAM_DisableJitter, "renderstage_opaque_notxaa", SRenderStageCluster::Init( RPl_Weapon ), SM_ByPriority )
		);

		ADD_SUBNODE( "EndRTs_OpaqueNoTXAA", CRenderNode_EndRenderTargetsMain, rt_OpaqueNoTxaa );

		// TODO: remove when mirror gets moved pre-full screen vfx (eg, to gbuffer pass)
		if ( cameraFeatures.Get( RFF_MirrorMaterial ) )
		{
			ADD_SUBNODE( "PrepareFeedbackColorBuffer", CRenderNode_PrepareFeedbackColorBuffer, cameraFeatures.Get( RFF_DLSS ), cameraFeatures.Get( RFF_DLSS ) );
		}

		ADD_SUBNODE( "SetRTs_TransparentNoTXAA", CRenderNode_SetRenderTargetsMain, rt_TransparentNoTxaa );

		// Used by braindance fog of war
		ADD_SUBNODE( "RenderElements_BackgroundNoTXAA", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_ForwardNoTXAA ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent_notxaa_background", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon, "renderstage_transparent_notxaa_background", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
		);

		ADD_SUBNODE( "RenderElements_TransparentNoTXAA", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_ForwardNoTXAA ); }
			, REBatch( REBF_CAM_MainScene|REBF_CAM_DisableJitter, "renderstage_transparent_notxaa", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon|REBF_CAM_DisableJitter, "renderstage_transparent_notxaa", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
		);

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "DebugSystemsNoTXAA", CRenderNode_RenderDebugSystems, CRenderNode_RenderDebugSystems::eGraphPhase::GP_TransparentsNoTXAA);
#endif // RED_CONFIGURATION_FINAL

		ADD_SUBNODE( "EndRTs_TransparentNoTXAA", CRenderNode_EndRenderTargetsMain, rt_TransparentNoTxaa );

		ADD_SUBNODE( "UnbindEnvProbesConstantBuffer", CRenderNode_UnbindEnvProbes );
		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "PostFX" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );
		
		ADD_SUBNODE( "UpdatePersistentFrameData", CRenderNode_PersistentCameraData);

		ADD_SUBNODE( "FullscreenCircularBlur", CRenderNode_FullscreenCircularBlur );
		ADD_SUBNODE( "AvgLuminanceUpdate", CRenderNode_HistogramUpdate, CRenderNode_HistogramUpdate::eAVERAGE_LUMINANCE );
		if ( cameraFeatures.Get( RFF_Tonemapping ) )
		{
			if ( !doAsyncLutGeneration )
			{
				ADD_SUBNODE( "GenerateTonemappingLUT", CRenderNode_GenerateTonemappingLUT );
			}
			ADD_SUBNODE( "ApplyBloomAndTonemapping", CRenderNode_ApplyBloomAndTonemapping );
		}
		ADD_SUBNODE( "Gameplay_PostBloom", CRenderNode_GameplayPostFX, EPO_POST_BLOOM );
		ADD_SUBNODE( "RenderVisionElements", CRenderNode_RenderVisionElements );
		ADD_SUBNODE( "ApplyFXAA", CRenderNode_ApplyFXAA );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "Debug" )
	{
		if ( !cameraFeatures.Get( RFF_DLSS ) )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "ApplyDebugPreview", CRenderNode_ApplyDebugPreview );
			ADD_SUBNODE( "Heatmap", CRenderNode_ApplyDebugHeatmap );
			ADD_SUBNODE( "Selection", CRenderNode_Selection );
			ADD_SUBNODE( "RenderDebugWorld", CRenderNode_RenderDebugFragments, DebugFragments::WorldAndWorldOverlay, FEATFLAG_Default );
			ADD_SUBNODE( "WindImpulseDebugRender", CRenderNode_WindImpulseVolumeDebugRender );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}


	// I separated this since it may potentially use local/cascade shadows stage collectors in the same time as rendering local 
	// shadowmaps/cascade shadowmaps as they are both in G_Render and needed a dependency.
	auto idShadowsDebug = RENDER_COMMAND_LIST( G_Render, "ShadowsDebug" )
	{
		if ( !cameraFeatures.Get( RFF_DLSS ) )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "LocalShadowsDebug", CRenderNode_RenderLocalShadowMapsDebug );
			ADD_SUBNODE( "CascadesShadowsDebug", CRenderNode_RenderCascadesShadowMapsDebug );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

	RENDER_COMMAND_LIST( G_Render, "Finalize_and_HUD" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
#endif // RED_CONFIGURATION_FINAL

		if ( !cameraFeatures.Get( RFF_DLSS ) )
		{
			ADD_SUBNODE( "ApplySharpening", CRenderNode_ApplySharpening );
		}

		if ( cameraFeatures.Get( RFF_DepthExtraction ) )
		{
			ADD_SUBNODE( "ExtractionDepthBuffPrep", CRenderNode_ModifyDepth, eDepthModificationType::Linearize );
		}

		if ( cameraFeatures.Get( RFF_ColorExtraction ) )
		{
			ADD_SUBNODE( "ExtractionSceneColor", CRenderNode_ExtractionSceneColor );
		}

		ADD_SUBNODE( "UpdateGlyphCacheTexture", CRenderNode_UpdateGlyphCacheTexture );

		ADD_SUBNODE( "CopyToTexture", CRenderNode_CopyToTexture );

		if ( cameraFeatures.Get( RFF_HUD ) )
		{
			ADD_SUBNODE( "DrawComposition", CRenderNode_DrawComposition );
			ADD_SUBNODE( "CompositionPostProcess", CRenderNode_CompositionPostProcess );
			ADD_SUBNODE( "DrawHUD", CRenderNode_DrawHUD );
			ADD_SUBNODE( "FullscreenVideo", CRenderNode_FullscreenVideo );
		}

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

#ifdef USE_PROFILER
	if ( cameraFeatures.Get( RFF_HUD ) )
	{
		RENDER_COMMAND_LIST( G_Render, "RenderDebugFragments" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "RenderDebugFragments", CRenderNode_RenderDebugFragments, DebugFragments::Screen, FEATFLAG_Default );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}
#endif

	RENDER_COMMAND_LIST( G_PostRender, "RenderFinal2D" )
	{
		ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, FEATFLAG_Default );

		if ( cameraFeatures.Get( RFF_ColorExtraction ) )
		{
			ADD_SUBNODE( "ExtractionFinalColor", CRenderNode_ExtractionFinalColor );
		}

		ADD_SUBNODE( "CamResDepScopeClose", CRenderNode_CameraResourceDependencyScope, false );
	}

	auto cameraEndID=			ADD_SEQ_END( G_None,	"CameraEnd",							SEQUENCE_RENDERNODE_Camera );
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto extractDepthGrabID = CRenderNodeGraph::INVALID_ITEM_ID;
	if( cameraFeatures.Get( RFF_DepthExtraction ) )
	{
		extractDepthGrabID	=	RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "ExtractionDepthBuffGrab", UNIQUE_RENDERNODE_ExtractDepthGrab, CRenderNode_ExtractionDepthBufferGrab );
	}

	AddParticleSimThreads(G_ParticlesOffScreenSim, factory, false, true);

#ifndef RED_CONFIGURATION_FINAL
	auto rendZoomID	=				RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "ApplyDebugZoom", UNIQUE_RENDERNODE_DebugZoom, CRenderNode_ApplyDebugZoom );
#endif // RED_CONFIGURATION_FINAL
	auto endRenderID =				RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );
	auto finalFlushID =				ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphCamera" );
	auto presentID =				ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );

	auto flushTexGrabsID = CRenderNodeGraph::INVALID_ITEM_ID;
	if ( hasFlushGrabs )
	{
		flushTexGrabsID = ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	}
	 
	auto cleanupBatchDataAllocatorID =	ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID =					ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	{
		factory.LinkGPU();

		// This pass requires full CPU synchronization due to injected command flushes
		if( cameraFeatures.Get( RFF_GBufferExtraction ) )
		{
			factory.LinkCPU();
		}
		else
		{
			factory.LinkCPUToNextGPU( startRenderID );

			factory.Link( descAllocsID, renderPrepID, RNDT_Cpu );
			factory.Link( startRenderID, prepDistantShadowsID, RNDT_Cpu );
			factory.Link( startRenderID, windUpdateAndRainMapID, RNDT_Cpu );
			factory.Link( prepDistantShadowsID, renderDistantShadowsID, RNDT_Cpu );
			factory.Link( prepareRainMapID, windUpdateAndRainMapID, RNDT_Cpu );
			factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
			factory.Link( cameraStartID, prepareCollectorID, RNDT_Cpu );
			factory.Link( prepareCollectorID, debugCollectHighPriority, RNDT_Cpu );
			factory.Link( debugCollectHighPriority, G_CullingScene, RNDT_Cpu );
			factory.Link( debugCollectHighPriority, G_CullingRayTracing, RNDT_Cpu );
			factory.Link( debugCollectHighPriority, G_CullingCascades, RNDT_Cpu );
			factory.Link( G_CullingScene, G_CullingLocalShadows, RNDT_Cpu );
			factory.Link( G_CullingRayTracing, G_PostCullingDebug, RNDT_Cpu );
			factory.Link( G_CullingLocalShadows, G_PostCullingDebug, RNDT_Cpu );
			factory.Link( G_CullingCascades, G_PostCullingDebug, RNDT_Cpu );
			factory.Link( visDebugID, debugCollectLowPriority, RNDT_Cpu );
			factory.Link( G_CullingRayTracing, G_RenderPrep, RNDT_Cpu );
			factory.Link( G_CullingLocalShadows, G_RenderPrep, RNDT_Cpu );
			factory.Link( G_CullingCascades, G_RenderPrep, RNDT_Cpu );
			factory.Link( prepDistantShadowsID, G_RenderPrep, RNDT_Cpu );
			factory.Link( prepareRainMapID, G_RenderPrep, RNDT_Cpu );
			factory.Link( G_RenderPrep, G_Render, RNDT_Cpu );
			factory.Link( debugCollectLowPriority, G_Render, RNDT_Cpu );

			factory.Link( prepDistantShadowsID, idLighting, RNDT_Cpu );
			factory.Link( prepDistantShadowsID, G_CullingCascades, RNDT_Cpu );

#ifdef GPUAPI_EXTENDED_SYNC
			factory.Link( idLighting, idLightingLate, RNDT_Cpu );
			if ( doAsyncSSR )
			{
				factory.Link( idLighting, idLightingMid, RNDT_Cpu );
			}
#endif

			factory.Link( G_CullingScene, G_ParticlesOnScreenSim, RNDT_Cpu );
			factory.Link( G_CullingCascades, G_ParticlesOnScreenSim, RNDT_Cpu );
			factory.Link( G_ParticlesOnScreenSim, G_Render, RNDT_Cpu );

			factory.Link( G_CullingScene, G_ParticlesOffScreenSim, RNDT_Cpu );
			factory.Link( G_CullingCascades, G_ParticlesOffScreenSim, RNDT_Cpu );
			factory.Link( G_ParticlesOffScreenSim, endRenderID, RNDT_Cpu );

			factory.Link( idDecoupledParticleLighting, idTransparents, RNDT_Cpu );
			for ( Int32 cpK = 1; cpK < CpTransSplitN(); ++cpK ) factory.Link( idDecoupledParticleLighting, cpTransSliceIds[ cpK - 1 ], RNDT_Cpu ); // [skperf] round 58 transSplit
			factory.Link( idDecoupledParticleLighting, idTransparentsWeaponScreen, RNDT_Cpu );

#ifndef RED_CONFIGURATION_FINAL
			if ( displayMode == EMM_ParticleNumLights )
			{
				factory.Link( idDecoupledParticleLighting, idParticleNumLights, RNDT_Cpu );
			}

			// Connect any spawned cmdlist for rendering local shadowmaps (they may differ depending on 
			// number of ligts we are supporting in the shadows queue in current settings) with the debug
			// view calls as they share usage of local shadows stage collector.
			for ( CRenderNodeGraph::ItemId id : idsRenderLocalShadowmaps )
			{
				factory.Link( id, idShadowsDebug, RNDT_Cpu );
			}

			// Connect any spawned cmdlist for rendering cascdes shadowmaps (they may differ depending on 
			// number of cascades we are supporting in current settings) with the debug view calls as they 
			//share usage of cascades shadows stage collector.
			for ( CRenderNodeGraph::ItemId id : idsRenderCascadesShadowmaps )
			{
				factory.Link( id, idShadowsDebug, RNDT_Cpu );
			}

#endif // RED_CONFIGURATION_FINAL

			factory.Link( G_PostCullingDebug, G_PostRender, RNDT_Cpu );
			factory.Link( G_Render, G_PostRender, RNDT_Cpu );
			factory.Link( G_PostRender, cameraEndID, RNDT_Cpu );

			// Link end-of-camera to any gpu work coming before it. This is needed in case we have multiple cameras,
			// using async compute. Otherwise we don't seem to get the correct sequencing of command lists and syncs
			// between the camera subgraphs.
			factory.LinkCPUToPreviousGPU( cameraEndID );

#ifdef USE_RAY_TRACING
			if ( useRayTracing )
			{
				factory.Link( renderPrepID, accelerationStructureUpdateStatic, RNDT_Cpu );
				factory.Link( renderPrepID, accelerationStructureUpdateDynamic, RNDT_Cpu );
				factory.Link( G_CullingRayTracing, accelerationStructureUpdateStatic, RNDT_Cpu );
				factory.Link( G_CullingRayTracing, accelerationStructureUpdateDynamic, RNDT_Cpu );
				factory.Link( accelerationStructureUpdateStatic, accelerationStructureUpdateEpilogue, RNDT_Cpu );
				factory.Link( accelerationStructureUpdateDynamic, accelerationStructureUpdateEpilogue, RNDT_Cpu );
				factory.Link( accelerationStructureUpdateEpilogue, endRenderID, RNDT_Cpu );
			}
#endif // USE_RAY_TRACING

			if( cameraFeatures.Get( RFF_DepthExtraction ) )
			{
				factory.Link( cameraEndID, extractDepthGrabID, RNDT_Cpu );
#ifndef RED_CONFIGURATION_FINAL
				factory.Link( extractDepthGrabID, rendZoomID, RNDT_Cpu );
#else
				factory.Link( extractDepthGrabID, endRenderID, RNDT_Cpu );
#endif // RED_CONFIGURATION_FINAL
			}
			else
			{
#ifndef RED_CONFIGURATION_FINAL
				factory.Link( cameraEndID, rendZoomID, RNDT_Cpu );
#else
				factory.Link( cameraEndID, endRenderID, RNDT_Cpu );
#endif // RED_CONFIGURATION_FINAL
			}

#ifndef RED_CONFIGURATION_FINAL
			factory.Link( rendZoomID, endRenderID, RNDT_Cpu );
#endif // RED_CONFIGURATION_FINAL	
			factory.LinkCPUToPreviousGPU( finalFlushID );
			//factory.Link( endRenderID, finalFlushID, RNDT_Cpu );
			factory.Link( finalFlushID, presentID, RNDT_Cpu );

			factory.Link( G_PostRender, cleanupBatchDataAllocatorID, RNDT_Cpu );
			factory.Link( renderDistantShadowsID, cleanupBatchDataAllocatorID, RNDT_Cpu );
			factory.Link( windUpdateAndRainMapID, cleanupBatchDataAllocatorID, RNDT_Cpu );
			factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );

			if ( hasFlushGrabs )
			{
				factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
				factory.Link( flushTexGrabsID, endFrameID, RNDT_Cpu );
			}
			else
			{
				factory.Link( presentID, endFrameID, RNDT_Cpu );
			}
			
		}
	}
}

void CRenderInterface::BuildRenderGraphSafeMode( CRenderNodeGraph &graph, rend::NodesContainer &nodes, const RenderFeatureSet& cameraFeatures )
{
	using REBatch = RenderElementsBatch;

	constexpr targetMod rt_Unlit =
		targetMod::WriteDepth_YES|targetMod::WeaponDepthCorrected_NO|targetMod::PostAA_NO|
		targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;

	const targetMod rtmod_Transparent_TXAAMask = cameraFeatures.Get( RFF_DLSS ) ? targetMod::TransparencyTXAAMask_NO : targetMod::TransparencyTXAAMask_YES;
	const targetMod rtmod_Transparent_Velocity = cameraFeatures.Get( RFF_DLSS ) ? targetMod::VelocityBuffer_YES : targetMod::VelocityBuffer_NO;

	const targetMod rt_TransparentRWDepth =
		targetMod::WriteDepth_YES|targetMod::WeaponDepthCorrected_NO|
		targetMod::PostAA_NO|targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;

	const targetMod rt_TransparentBackground =
		targetMod::WriteDepth_NO|targetMod::WeaponDepthCorrected_NO|
		targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|rtmod_Transparent_Velocity;

	const targetMod rt_Transparent =
		targetMod::WriteDepth_NO|targetMod::WeaponDepthCorrected_NO|
		targetMod::PostAA_NO|rtmod_Transparent_TXAAMask|rtmod_Transparent_Velocity;

	const targetMod rt_TransparentNoTxaa =
		targetMod::WriteDepth_NO|targetMod::WeaponDepthCorrected_NO|
		targetMod::PostAA_NO|targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;


	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None				= rend::NodeGroupID::None;
	const rend::NodeGroupID G_Culling			= rend::NodeGroupID(1);
	const rend::NodeGroupID G_PostCullingDebug	= rend::NodeGroupID(2);
	const rend::NodeGroupID G_RenderPrep		= rend::NodeGroupID(3);
	const rend::NodeGroupID G_Render			= rend::NodeGroupID(4);
	const rend::NodeGroupID G_PostRender		= rend::NodeGroupID(5);

	const Bool hasFlushGrabs =	( cameraFeatures.Get( RFF_GBufferExtraction ) && cameraFeatures.Get( RFF_ImmediateMode ) ) ||
								  cameraFeatures.Get( RFF_ColorExtraction ) ||
								( cameraFeatures.Get( RFF_DepthExtraction ) && cameraFeatures.Get( RFF_ImmediateMode ) ) ||
								  cameraFeatures.Get( RFF_BackgroundDump );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto startRenderID =				RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None,		"StartRender",							UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto cameraStartID =				ADD_SEQ_BEGIN( G_None,	"CamStart",								SEQUENCE_RENDERNODE_Camera );

	auto advWindID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "AdvanceWind", UNIQUE_RENDERNODE_AdvanceWind, CRenderNode_AdvanceSpeedTreeWind );

	auto prepareCollectorID =			ADD_NODE( G_None,		"PrepCollector",						CRenderNode_PrepareCollector, FEATFLAG_Default );
	auto debugCollectHighPriority =		ADD_NODE( G_None,		"DebugCollectHighPriority",				CRenderNode_CollectDebug, true );

										ADD_NODE( G_Culling,	"DoCulling",							CRenderNode_DoCulling, cullingMod::MainScene::YES, cullingMod::Cascades::NO, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::YES );

	auto visDebugID =					ADD_NODE( G_PostCullingDebug, "FlushVisiDebug",					CRenderNode_FlushVisibilityDebug );
	auto debugCollectLowPriority =		ADD_NODE( G_PostCullingDebug, "DebugCollectLowPriority",		CRenderNode_CollectDebug, false );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto descAllocsID =					ADD_NODE( G_RenderPrep,		"DeclCommonResAllocs",					CRenderNode_DeclareCommonResourceAllocs_SafeMode );

	RENDER_COMMAND_LIST( G_RenderPrep, "PrepRendering" )
	{
		ADD_SUBNODE( "CamResDepScopeOpen", CRenderNode_CameraResourceDependencyScope, true );
		ADD_SUBNODE( "PrepSceneRendering", CRenderNode_PrepareSceneRendering );

		ADD_SUBNODE( "PrepareDummyData", CRenderNode_PrepareDummyLightingData );
	}

	RENDER_COMMAND_LIST( G_Render, "GBuffer_Solid" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetRenderToGbuff", CRenderNode_SetRenderTargetsGBuffer, targetMod::ClearGBuffer|targetMod::WriteDepth_YES );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_regular", SRenderStageCluster::Init(RPl_Weapon), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_regular", SRenderStageCluster::Init(RPl_Scene), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndRenderToGbuff", CRenderNode_EndRenderTargetsGBuffer );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "GBuffer_Discard_and_Late" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetRenderToGbuff", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_YES );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBuffer ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
			, REBatch( REBF_CAM_MainWeapon, "renderstage_gbuffer_discarded", SRenderStageCluster::Init(RPl_Weapon), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_gbuffer_discarded", SRenderStageCluster::Init(RPl_Scene), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, [] ( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_GBufferLate ) && rctx.Test( rend::DF_RenderMask_Discarded ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_late_gbuffer_regular", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_late_gbuffer_discarded", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "EndRenderToGbuff", CRenderNode_EndRenderTargetsGBuffer );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "DeferredDecals" )
	{
		ADD_SUBNODE( "PrepareFeedbackNormalBuffer", CRenderNode_PrepareFeedbackNormalBuffer );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "SetRenderToGbuff", CRenderNode_SetRenderTargetsGBuffer, targetMod::WriteDepth_NO );

		ADD_SUBNODE( "RenderElements_PostGBuffer", CRenderNode_RenderElements
			, &CpDeferredDecalsCond // [skperf] round 53 mirrorDecals (was: GBuffer render-mask test only)
			, REBatch( REBF_CAM_MainWeapon, "renderstage_post_gbuffer", SRenderStageCluster::Init( RPl_Weapon ), SM_ByPriority )
			, REBatch( REBF_CAM_MainScene, "renderstage_post_gbuffer", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
		);

		ADD_SUBNODE( "EndRenderToGbuff", CRenderNode_EndRenderTargetsGBuffer );

		ADD_SUBNODE( "EndGBuffer1Feedback", CRenderNode_EndGBuffer1Feedback );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "FinishOpaque" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "PresentBaseColor", CRenderNode_PresentBaseColor );

		ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsMain, rt_Unlit );

		ADD_SUBNODE( "RenderElements_Unlit", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Unlit ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_unlit", SRenderStageCluster::Init(RPl_Scene), SM_OptimizedDistanceBatching )
		);

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "DebugForwardRender", CRenderNode_RenderDebugSystems, CRenderNode_RenderDebugSystems::eGraphPhase::GP_ForwardOpaque );
#endif // RED_CONFIGURATION_FINAL

		ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_Unlit );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	auto extractDepthPrepareID = CRenderNodeGraph::INVALID_ITEM_ID;
	auto extractGBufferID = CRenderNodeGraph::INVALID_ITEM_ID;
	if ( cameraFeatures.Get( RFF_GBufferExtraction ) )
	{
		extractGBufferID =				RENDER_SIMPLE_COMMAND_LIST( G_Render,		"ExtractionGBuffer",					CRenderNode_ExtractionGBuffer );
	}
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	RENDER_COMMAND_LIST( G_Render, "Transparents" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "SetRTsMainRWDepth", CRenderNode_SetRenderTargetsMain, rt_TransparentRWDepth );
		ADD_SUBNODE( "RenderElements_Depth_Write", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent_depth_write", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);
		ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_TransparentRWDepth );

		ADD_SUBNODE( "SetRTsMainRODepth", CRenderNode_SetRenderTargetsMain, rt_TransparentBackground );

		//Moved water rendering into a separate node happening before other transparents for E3
		ADD_SUBNODE( "RenderElements_Transparent_Background", CRenderNode_RenderElements
					 , []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
					 , REBatch( REBF_CAM_MainScene, "renderstage_transparent_background", SRenderStageCluster::Init( RPl_Scene ), SM_ByPriority )
		);

		ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_TransparentBackground );

		ADD_SUBNODE( "PrepareFeedbackColorBuffer", CRenderNode_PrepareFeedbackColorBuffer, true );

		ADD_SUBNODE( "SetRTsMainRODepth", CRenderNode_SetRenderTargetsMain, rt_Transparent );

		ADD_SUBNODE( "RenderElements_Transparent", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_Forward ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent_back_face", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon, "renderstage_transparent_back_face", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon, "renderstage_transparent", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
		);

		ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_Transparent );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Transparent stuff after TXAA - like on-scene placed HUD's and stuff
	RENDER_COMMAND_LIST( G_Render, "Transparents_NoTXAA" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "SetRTsMainRWDepth", CRenderNode_SetRenderTargetsMain, rt_TransparentNoTxaa );

		// Used by braindance fog of war
		ADD_SUBNODE( "RenderElements_NoTXAABackground", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_ForwardNoTXAA ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent_notxaa_background", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon, "renderstage_transparent_notxaa_background", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
		);

		ADD_SUBNODE( "RenderElements_NoTXAA", CRenderNode_RenderElements
			, []( const SRenderNodeImplContext &rctx ) { return rctx.Test( rend::DF_RenderMask_ForwardNoTXAA ); }
			, REBatch( REBF_CAM_MainScene, "renderstage_transparent_notxaa", SRenderStageCluster::Init(RPl_Scene), SM_ByPriority )
			, REBatch( REBF_CAM_MainWeapon, "renderstage_transparent_notxaa", SRenderStageCluster::Init(RPl_Weapon), SM_ByPriority )
		);

		ADD_SUBNODE( "EndRTsMainRODepth", CRenderNode_EndRenderTargetsMain, rt_TransparentNoTxaa );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	RENDER_COMMAND_LIST( G_Render, "PostFX" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "Gameplay_PostBloom", CRenderNode_GameplayPostFX, EPO_POST_BLOOM );
		ADD_SUBNODE( "RenderVisionElements", CRenderNode_RenderVisionElements );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	auto renderDebug = RENDER_COMMAND_LIST( G_Render, "Debug" )
	{
		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

		ADD_SUBNODE( "RenderDebugWorld", CRenderNode_RenderDebugFragments, DebugFragments::WorldAndWorldOverlay, FEATFLAG_Default );
		ADD_SUBNODE( "DebugForwardRender", CRenderNode_RenderDebugSystems, CRenderNode_RenderDebugSystems::eGraphPhase::GP_TransparentsNoTXAA );

		ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	if ( cameraFeatures.Get( RFF_DepthExtraction ) )
	{
		extractDepthPrepareID = RENDER_COMMAND_LIST( G_Render, "ExtractionDepthBuffPrep" )
		{
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "ExtractionDepthBuffPrep", CRenderNode_ModifyDepth, eDepthModificationType::Linearize );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

	if ( cameraFeatures.Get( RFF_ColorExtraction ) )
	{
		RENDER_SIMPLE_COMMAND_LIST( G_Render, "ExtractionSceneColor", CRenderNode_ExtractionSceneColor );
	}

	auto hudID = CRenderNodeGraph::INVALID_ITEM_ID;
	if ( cameraFeatures.Get( RFF_HUD ) )
	{
		hudID = RENDER_COMMAND_LIST( G_Render, "HUD" )
		{
			ADD_SUBNODE( "UpdateGlyphCacheTexture", CRenderNode_UpdateGlyphCacheTexture );
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
			ADD_SUBNODE( "BindLightingGlobalConstants", CRenderNode_BindLightingGlobalConstants );

			ADD_SUBNODE( "CopyToTexture", CRenderNode_CopyToTexture );
			ADD_SUBNODE( "DrawComposition", CRenderNode_DrawComposition );
			ADD_SUBNODE( "CompositionPostProcess", CRenderNode_CompositionPostProcess );
			ADD_SUBNODE( "DrawHUD", CRenderNode_DrawHUD );
			ADD_SUBNODE( "FullscreenVideo", CRenderNode_FullscreenVideo );
			ADD_SUBNODE( "RenderDebugScreen", CRenderNode_RenderDebugFragments, DebugFragments::Screen, FEATFLAG_Default );

			ADD_SUBNODE( "UnbindLightingGlobalConstants", CRenderNode_UnbindLightingGlobalConstants );
			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}
	}

	RENDER_COMMAND_LIST( G_PostRender, "RenderFinal2D" )
	{
		// render after G_Render to make sure all debug stuff (like texture previews are collected)
		ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, FEATFLAG_Default );

		if ( cameraFeatures.Get( RFF_ColorExtraction ) )
		{
			ADD_SUBNODE( "ExtractionFinalColor", CRenderNode_ExtractionFinalColor );
		}

		ADD_SUBNODE( "CamResDepScopeClose", CRenderNode_CameraResourceDependencyScope, false );
	}

	auto cameraEndID = ADD_SEQ_END( G_None, "CameraEnd", SEQUENCE_RENDERNODE_Camera );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto extractDepthGrabID = CRenderNodeGraph::INVALID_ITEM_ID;
	if ( cameraFeatures.Get( RFF_DepthExtraction ) )
	{
		extractDepthGrabID =	RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "ExtractionDepthBuffGrab", UNIQUE_RENDERNODE_ExtractDepthGrab, CRenderNode_ExtractionDepthBufferGrab );
	}

	auto endRenderID =			RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );
	auto finalFlushID =			ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphSafeMode" );
	auto presentID =			ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );

	auto flushTexGrabsID = CRenderNodeGraph::INVALID_ITEM_ID;
	if ( hasFlushGrabs )
	{
		flushTexGrabsID = ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	}

	auto cleanupBatchDataAllocatorID = ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID =			ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	{
		factory.LinkGPU();

		// This pass requires full CPU synchronization due to injected command flushes
		if ( cameraFeatures.Get( RFF_GBufferExtraction ) )
		{
			factory.LinkCPU();
		}
		else
		{
			factory.LinkCPUToNextGPU( startRenderID );

			factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
			factory.Link( cameraStartID, prepareCollectorID, RNDT_Cpu );
			factory.Link( prepareCollectorID, debugCollectHighPriority, RNDT_Cpu );
			factory.Link( debugCollectHighPriority, G_Culling, RNDT_Cpu );
			factory.Link( G_Culling, G_PostCullingDebug, RNDT_Cpu );
			factory.Link( visDebugID, debugCollectLowPriority, RNDT_Cpu );
			factory.Link( G_Culling, G_RenderPrep, RNDT_Cpu );
			factory.Link( G_RenderPrep, G_Render, RNDT_Cpu );
			factory.Link( advWindID, G_Render, RNDT_Cpu );
			factory.Link( G_PostCullingDebug, G_PostRender, RNDT_Cpu );
			factory.Link( G_Render, G_PostRender, RNDT_Cpu );
			factory.Link( G_PostRender, cameraEndID, RNDT_Cpu );
			factory.Link( debugCollectLowPriority, renderDebug, RNDT_Cpu );
			factory.Link( debugCollectLowPriority, hudID, RNDT_Cpu );

			if ( cameraFeatures.Get( RFF_DepthExtraction ) )
			{
				factory.Link( cameraEndID, extractDepthGrabID, RNDT_Cpu );
				factory.Link( extractDepthGrabID, endRenderID, RNDT_Cpu );
			}
			else
			{
				factory.Link( cameraEndID, endRenderID, RNDT_Cpu );
			}

			factory.Link( endRenderID, finalFlushID, RNDT_Cpu );
			factory.Link( finalFlushID, presentID, RNDT_Cpu );

			factory.Link( G_PostRender, cleanupBatchDataAllocatorID, RNDT_Cpu );
			factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );

			if ( hasFlushGrabs )
			{
				factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
				factory.Link( flushTexGrabsID, endFrameID, RNDT_Cpu );
			}
			else
			{
				factory.Link( presentID, endFrameID, RNDT_Cpu );
			}
		}
	}
}


void CRenderInterface::BuildRenderGraphNoScene( CRenderNodeGraph &graph, rend::NodesContainer &nodes, Bool wireframe )
{
	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None		= rend::NodeGroupID::None;
	const rend::NodeGroupID G_PreRender	= rend::NodeGroupID(1);
	const rend::NodeGroupID G_Render	= rend::NodeGroupID(2);


	EFeatureFlag baseFeatureFlag = wireframe ? FEATFLAG_Wireframe : FEATFLAG_Default;

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto startRenderID =		RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto cameraStartID =		ADD_SEQ_BEGIN( G_None, "CamStart", SEQUENCE_RENDERNODE_Camera );

	ADD_NODE( G_PreRender, "DeclCommonResAllocs", CRenderNode_DeclareCommonResourceAllocs_FinalOnly );
	auto idCollector = ADD_NODE( G_PreRender, "PrepCollector", CRenderNode_PrepareCollector, baseFeatureFlag );
	auto idCollectDebugHighPriority = ADD_NODE( G_PreRender, "DebugCollectHighPriority", CRenderNode_CollectDebug, true );
	auto idCollectDebugLowPriority = ADD_NODE( G_PreRender, "DebugCollectLowPriority", CRenderNode_CollectDebug, false );
	auto idPrepRender = RENDER_SIMPLE_COMMAND_LIST( G_PreRender, "PrepSceneRendering", CRenderNode_PrepareSceneRendering );

	RENDER_COMMAND_LIST( G_Render, "RenderGraph_NoScene" )
	{
		ADD_SUBNODE( "UpdateGlyphCacheTexture", CRenderNode_UpdateGlyphCacheTexture );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "ClearFinalTarget", CRenderNode_ClearFinalTarget );

#ifndef RED_CONFIGURATION_FINAL
		ADD_SUBNODE( "RenderDebugOverlay", CRenderNode_RenderDebugFragments, DebugFragments::WorldAndWorldOverlay, baseFeatureFlag );
		ADD_SUBNODE( "RenderDebugOverlay", CRenderNode_RenderDebugFragments, DebugFragments::Screen, baseFeatureFlag );
		ADD_SUBNODE( "ApplyDebugPreview", CRenderNode_ApplyDebugPreview );
#endif // RED_CONFIGURATION_FINAL

		ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, baseFeatureFlag );

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	auto cameraEndID =					ADD_SEQ_END( G_None, "CameraEnd", SEQUENCE_RENDERNODE_Camera );

	auto endRenderID =					RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );
	auto finalFlushID =					ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphNoScene" );
	auto presentID =					ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	auto flushTexGrabsID =				ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	auto cleanupBatchDataAllocatorID =	ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID =					ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );

	{
		factory.LinkGPU();
		factory.LinkCPUToNextGPU( startRenderID );
		factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
		factory.Link( cameraStartID, G_PreRender, RNDT_Cpu );
		factory.Link( idCollector, idCollectDebugHighPriority, RNDT_Cpu );
		factory.Link( idCollectDebugHighPriority, idCollectDebugLowPriority, RNDT_Cpu );
		factory.Link( idCollector, idPrepRender, RNDT_Cpu );
		factory.Link( G_PreRender, G_Render, RNDT_Cpu );
		factory.Link( G_Render, cameraEndID, RNDT_Cpu );
		factory.Link( cameraEndID, endRenderID, RNDT_Cpu );
		factory.Link( endRenderID, finalFlushID, RNDT_Cpu );
		factory.Link( finalFlushID, presentID, RNDT_Cpu );
		factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
		factory.Link( G_Render, cleanupBatchDataAllocatorID, RNDT_Cpu );
		factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );
		factory.Link( flushTexGrabsID, endFrameID, RNDT_Cpu );
	}
}

void CRenderInterface::BuildRenderGraphTodvis( CRenderNodeGraph &graph, rend::NodesContainer &nodes, Bool isDebugView )
{
	using REBatch = RenderElementsBatch;
	rend::NodeGraphFactory factory( &graph, &nodes );

	// Nodes groups for easier CPU linking.
	const rend::NodeGroupID G_None		= rend::NodeGroupID::None;
	const rend::NodeGroupID G_Render	= rend::NodeGroupID( 1 );
	const rend::NodeGroupID G_PreRender = rend::NodeGroupID( 2 );

	auto startRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender_TODVIS", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );
	auto cameraStartID = ADD_SEQ_BEGIN( G_None, "CameraStart_TODVIS", SEQUENCE_RENDERNODE_Camera );


	ADD_NODE( G_PreRender, "DeclCommonResAllocs_TODIVS", CRenderNode_DeclareCommonResourceAllocs_HitProxy );																	//< Proxy IDs, depth buffer and final targets for generator to grab.
	ADD_NODE( G_PreRender, "PrepCollector_TODVIS", CRenderNode_PrepareCollector, FEATFLAG_HitProxies /*| FEATFLAG_Shadows added in collecttodviselements. */ );					//< TODVIS is rendered from main camera and we can leverage occluders using vis system.

	Uint32 flags = RTFINAL_BindDepth | RTFINAL_AllowDepthWrite | RTFINAL_ClearDepth | RTFINAL_ClearColor;

	RENDER_COMMAND_LIST( G_Render, "HitProxy_TODVIS" )
	{
		ADD_SUBNODE( "SetFinalTargets_HP", CRenderNode_SetRenderTargetsHitProxy, flags );
		ADD_SUBNODE( "PrepHitProxyRendering_HP", CRenderNode_PrepareHitProxyRendering );
		ADD_SUBNODE( "RenderElementsTodvis", CRenderNode_RenderElementsTodvis );
		ADD_SUBNODE( "EndFinalTargetsUse_HP", CRenderNode_EndRenderTargetsHitProxy, 0 );
	}
	
	RENDER_COMMAND_LIST( G_Render, "Apply" )
	{
		if ( isDebugView )
		{
			ADD_SUBNODE( "ApplyDebugPreview", CRenderNode_ApplyDebugPreview );
			ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, FEATFLAG_HitProxies );
		}
		ADD_SUBNODE( "ExtractionSelection_TODVIS", CRenderNode_ExtractionSelection );			//< Schedule selection IDs extraction.
		ADD_SUBNODE( "ExtractionFinalColor_TODVIS", CRenderNode_ExtractionFinalColor );			//< Schedule color extraction for potential screenshots.
	}
	
	auto cameraEndID = ADD_SEQ_END( G_None, "CameraEnd_TODVIS", SEQUENCE_RENDERNODE_Camera );												
	auto endRenderID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender_TODVIS", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );										//< Mainly stats and ticking some proxies.
	auto idFlushGpu = ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphHitProxies" );				//< Submit commandlists.

	// In case of EMM preview we actually show it on the screen
	CRenderNodeGraph::ItemId presentID;
	if ( isDebugView )
	{
		presentID = ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	}

	auto flushTexGrabsID = ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );										//< Wait for scheduled extractions to finish.
	auto extractionSelectionProcess = ADD_UNIQUE( G_None, "extractionSelectionProcess", UNIQUE_RENDERNODE_ExtractSelectionProcess, CRenderNode_ExtractionSelectionProcess );	//< Process selection results.

	auto cleanupBatchDataAllocatorID = ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );	//< Reset bath data allocator (geometry, PSOs etc.)
	auto endFrameID = ADD_UNIQUE( G_None, "EndFrame_HP", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );																	// Flush profiling queries, flus removed dynamic decals etc.

	{
		factory.LinkGPU();
		factory.LinkCPUToNextGPU( startRenderID );
		factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
		factory.Link( cameraStartID, G_PreRender, RNDT_Cpu );
		factory.Link( G_PreRender, G_Render, RNDT_Cpu );		
		factory.Link( G_Render, cameraEndID, RNDT_Cpu );
		factory.Link( cameraEndID, endRenderID, RNDT_Cpu );
		factory.Link( G_Render, cleanupBatchDataAllocatorID, RNDT_Cpu );
		factory.Link( endRenderID, idFlushGpu, RNDT_Cpu );

		if ( isDebugView )
		{
			factory.Link( idFlushGpu, presentID, RNDT_Cpu );
			factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
		}
		else
		{
			factory.Link( idFlushGpu, flushTexGrabsID, RNDT_Cpu );
		}

		factory.Link( flushTexGrabsID, extractionSelectionProcess, RNDT_Cpu );
		factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );
		factory.Link( extractionSelectionProcess, endFrameID, RNDT_Cpu );
	}
}

void CRenderInterface::BuildRenderGraphDebugVisualization( CRenderNodeGraph &graph, rend::NodesContainer &nodes, const RenderFeatureSet& cameraFeatures, EEnvManagerModifier mode )
{
	using REBatch = RenderElementsBatch;

	constexpr targetMod rt_Regular =
		targetMod::WriteDepth_YES|targetMod::WeaponDepthCorrected_NO|targetMod::PostAA_NO|
		targetMod::TransparencyTXAAMask_NO|targetMod::VelocityBuffer_NO;
	constexpr targetMod rt_FirstPass = rt_Regular|targetMod::ClearColor|targetMod::ClearDepth;

	rend::NodeGraphFactory factory( &graph, &nodes );

	const rend::NodeGroupID G_None					= rend::NodeGroupID::None;
	const rend::NodeGroupID G_Render				= rend::NodeGroupID(2);
	const rend::NodeGroupID G_Culling				= rend::NodeGroupID(3);
	const rend::NodeGroupID G_ParticlesOnScreenSim	= rend::NodeGroupID(4);
	const rend::NodeGroupID G_ParticlesOffScreenSim	= rend::NodeGroupID(6);

	EFeatureFlag baseFeatureFlag = FEATFLAG_Default;
	if ( mode == EMM_WireframeSolid || mode == EMM_WireframeSeethrough )
	{
		baseFeatureFlag = FEATFLAG_Wireframe;
	}
	else if ( mode == EMM_Overdraw || mode == EMM_OverdrawSeethrough )
	{
		baseFeatureFlag = FEATFLAG_Overdraw;
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	auto startRenderID =		RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "StartRender", UNIQUE_RENDERNODE_StartRender, CRenderNode_StartRender );

	auto cameraStartID=			ADD_SEQ_BEGIN( G_None, "CamStart", SEQUENCE_RENDERNODE_Camera );

	auto prepareCollectorID	=	ADD_NODE( G_None, "PrepCollector", CRenderNode_PrepareCollector, baseFeatureFlag );

	auto advWindID = RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "AdvanceWind", UNIQUE_RENDERNODE_AdvanceWind, CRenderNode_AdvanceSpeedTreeWind );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	ADD_NODE( G_Culling, "DebugCollectHighPriority", CRenderNode_CollectDebug, true );
	ADD_NODE( G_Culling, "DoCulling", CRenderNode_DoCulling, cullingMod::MainScene::YES, cullingMod::Cascades::YES, cullingMod::DynamicShadows::NO, cullingMod::RayTracedObjects::NO, cullingMod::QueryOnlyBoxes::NO );
	ADD_NODE( G_Culling, "DebugCollectLowPriority", CRenderNode_CollectDebug, false );

	AddParticleSimThreads( G_ParticlesOnScreenSim, factory, true, false );

	auto updateParticleDataID = RENDER_SIMPLE_COMMAND_LIST( G_None, "UpdateParticleRenderData", CRenderNode_UpdateParticlesRenderData );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	auto visDebugID   =			ADD_NODE( G_None, "FlushVisiDebug", CRenderNode_FlushVisibilityDebug );

	auto descAllocsID =			ADD_NODE( G_Render, "DeclCommonResAllocs", CRenderNode_DeclareCommonResourceAllocs );

	auto prepSkinningCompute =	RENDER_SIMPLE_COMMAND_LIST( G_Render, "SkinningCompute", CRenderNode_ComputeSkinningAndTangentUpdates );

	auto prepScenID = RENDER_COMMAND_LIST( G_Render, "PrepSceneRendering" )
	{
		ADD_SUBNODE( "CamResDepScopeOpen", CRenderNode_CameraResourceDependencyScope, true );
		ADD_SUBNODE( "PrepSceneRendering", CRenderNode_PrepareSceneRendering );
	}

	auto idSolid = RENDER_COMMAND_LIST( G_Render, "Solid" )
	{
		ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsMain, rt_FirstPass );
		
		if ( mode == EMM_WireframeSolid || mode == EMM_Overdraw || mode == EMM_OverdrawSeethrough || mode == EMM_ParticleOverdraw || mode == EMM_ParticleNumLights )
		{
			const char* renderStageName = mode == EMM_WireframeSolid ? "renderstage_wireframe_solid" : "renderstage_overdraw";

			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
				, REBatch( REBF_CAM_MainWeapon, renderStageName, SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, renderStageName, SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
		}

		ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_FirstPass );
	}

	auto idWireframeUnlit = RENDER_COMMAND_LIST( G_Render, "Wireframe_Unlit" )
	{
		ADD_SUBNODE( "SetRTsMain", CRenderNode_SetRenderTargetsMain, rt_Regular );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

		ADD_SUBNODE( "RenderElements", CRenderNode_RenderElements
			, REBatch( REBF_CAM_MainWeapon, "renderstage_unlit", SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
			, REBatch( REBF_CAM_MainScene, "renderstage_unlit", SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
		);

		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );

		ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_Regular );
	}

	auto idWireframeLines = RENDER_COMMAND_LIST( G_Render, "Wireframe_Lines" )
	{
		if ( mode == EMM_WireframeSolid || mode == EMM_WireframeSeethrough )
		{
			const char* renderStageName = "renderstage_wireframe";

			ADD_SUBNODE( "SetRTsMain_WireframeLines", CRenderNode_SetRenderTargetsMain, rt_Regular );
	
			ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );

			ADD_SUBNODE( "RenderElements_WireframeLines", CRenderNode_RenderElements
				, REBatch( REBF_CAM_MainWeapon, renderStageName, SRenderStageCluster::Init( RPl_Weapon ), SM_OptimizedBatching )
				, REBatch( REBF_CAM_MainScene, renderStageName, SRenderStageCluster::Init( RPl_Scene ), SM_OptimizedBatching )
			);

			ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );

			ADD_SUBNODE( "EndRenderTargetsMain", CRenderNode_EndRenderTargetsMain, rt_Regular );
		}
	}

	if ( cameraFeatures.Get( RFF_ColorExtraction ) )
	{
		RENDER_SIMPLE_COMMAND_LIST( G_Render, "ExtractionSceneColor", CRenderNode_ExtractionSceneColor );
	}

	RENDER_COMMAND_LIST( G_Render, "HUD" )
	{
		ADD_SUBNODE( "CamResDepScopeClose", CRenderNode_CameraResourceDependencyScope, false );

		ADD_SUBNODE( "BindGlobalConstants", CRenderNode_BindGlobalConstants );
		ADD_SUBNODE( "ApplyDebugPreview", CRenderNode_ApplyDebugPreview );
		ADD_SUBNODE( "Selection", CRenderNode_Selection );
		ADD_SUBNODE( "RenderDebugWorld", CRenderNode_RenderDebugFragments, DebugFragments::WorldAndWorldOverlay, baseFeatureFlag );
		ADD_SUBNODE( "ApplySharpening", CRenderNode_ApplySharpening );
		ADD_SUBNODE( "RenderDebugOverlay", CRenderNode_RenderDebugFragments, DebugFragments::Screen, baseFeatureFlag );
		ADD_SUBNODE( "RenderFinal2D", CRenderNode_RenderFinal2D, baseFeatureFlag );
		ADD_SUBNODE( "UnbindGlobalConstants", CRenderNode_UnbindGlobalConstants );
	}

	if ( cameraFeatures.Get( RFF_ColorExtraction ) )
	{
		RENDER_SIMPLE_COMMAND_LIST( G_Render, "ExtractionFinalColor", CRenderNode_ExtractionFinalColor );
	}

	auto cameraEndID = ADD_SEQ_END( G_None, "CameraEnd", SEQUENCE_RENDERNODE_Camera );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	AddParticleSimThreads( G_ParticlesOffScreenSim, factory, false, true );

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	auto rendZoomID	=				RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "ApplyDebugZoom", UNIQUE_RENDERNODE_DebugZoom, CRenderNode_ApplyDebugZoom );
	auto endRenderID =				RENDER_UNIQUE_SIMPLE_COMMAND_LIST( G_None, "EndRender", UNIQUE_RENDERNODE_EndRender, CRenderNode_EndRender );
	auto finalFlushID =				ADD_UNIQUE( G_None, "FlushGpu", UNIQUE_RENDERNODE_FinalFlush, CRenderNode_Synchronize, GpuApi::CommandListSyncType::None, "Submit_RenderGraphDebugVisualization" );
	auto presentID =				ADD_UNIQUE( G_None, "Present", UNIQUE_RENDERNODE_Present, CRenderNode_Present );
	auto flushTexGrabsID =				ADD_UNIQUE( G_None, "FlushTextureGrabs", UNIQUE_RENDERNODE_FlushTextureGrabs, CRenderNode_FlushTextureGrabs );
	auto cleanupBatchDataAllocatorID =	ADD_UNIQUE( G_None, "CleanupBatchDataAllocator", UNIQUE_RENDERNODE_CleanupBatchDataAllocator, CRenderNode_CleanupBatchDataAllocator );
	auto endFrameID =					ADD_UNIQUE( G_None, "EndFrame", UNIQUE_RENDERNODE_EndFrame, CRenderNode_EndFrame );
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 

	{
		factory.LinkGPU();
		factory.LinkCPUToNextGPU( startRenderID );
		factory.Link( startRenderID, cameraStartID, RNDT_Cpu );
		factory.Link( cameraStartID, prepareCollectorID, RNDT_Cpu );
		factory.Link( prepareCollectorID, G_Culling, RNDT_Cpu );
		factory.Link( G_Culling, visDebugID, RNDT_Cpu );
		factory.Link( visDebugID, G_Render, RNDT_Cpu );
		factory.Link( G_Culling, G_ParticlesOnScreenSim, RNDT_Cpu );
		factory.Link( G_Culling, prepSkinningCompute, RNDT_Cpu );
		factory.Link( G_ParticlesOnScreenSim, updateParticleDataID, RNDT_Cpu );
		factory.Link( updateParticleDataID, G_Render, RNDT_Cpu );
		factory.Link( advWindID, G_Render, RNDT_Cpu );
		factory.Link( prepSkinningCompute, prepScenID, RNDT_Cpu );
		factory.Link( G_Render, cameraEndID, RNDT_Cpu );
		factory.Link( cameraEndID, rendZoomID, RNDT_Cpu );
		factory.Link( rendZoomID, endRenderID, RNDT_Cpu );
		factory.Link( cameraEndID, G_ParticlesOffScreenSim, RNDT_Cpu );
		factory.Link( G_ParticlesOffScreenSim, endRenderID, RNDT_Cpu );
		factory.Link( endRenderID, finalFlushID, RNDT_Cpu );
		factory.Link( finalFlushID, presentID, RNDT_Cpu );
		factory.Link( presentID, flushTexGrabsID, RNDT_Cpu );
		factory.Link( G_Render, cleanupBatchDataAllocatorID, RNDT_Cpu );
		factory.Link( cleanupBatchDataAllocatorID, endFrameID, RNDT_Cpu );
		factory.Link( flushTexGrabsID, endFrameID, RNDT_Cpu );
	}
}

void CRenderInterface::StartFrameRendering( const SRenderNodeImplContext& rctx, job::Builder& builder )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );

	const CRenderFrameInfo& info = rctx.GetFrameInfo();
	CRenderSceneEx* scene = static_cast< CRenderSceneEx* >( info.GetScene() );
	
	PC_SCOPE( StartFrameRendering );

	NewFrame( info );

	// For picking best mipbias, we will need to evaluate internal vs final resolution.
	// Initiate with no scaling, based on viewport directly.
	Uint32 internalRenderHeight = info.GetViewportHeight();
	Uint32 finalRenderHeight = internalRenderHeight;

	if ( rctx.IsCameraDataAvailable() )
	{
		// We have camera, so we can actually differentiate internal from final res
		internalRenderHeight = rctx.GetHeight();
		finalRenderHeight = rctx.GetFinalHeight();
	}

	// Figure out mip-bias adjustment factor.
	float mipBias = std::log2f( (Float)internalRenderHeight / (Float)finalRenderHeight );
	{
#ifdef USE_DLSS
		if ( m_ngx && m_ngx->IsDLSSEnabled() )
		{
			mipBias = std::log2f( m_ngx->GetScaleFactor() );
		}
#endif // USE_DLSS

		if ( !math::AlmostEquals( GRenderSettingsMipBias, mipBias ) )
		{
			GRenderSettingsMipBias = mipBias;
			GpuApi::ResetSamplerStates();
		}
	}

	GpuApi::RenderSettings renderSettings;
	if ( info.GetFramePurpose() == FP_GlobalIllumination || info.GetFramePurpose() == FP_GlobalIlluminationBackfaces )
	{
		// We don't care about texture quality, we want it low to speed up GI baking
		renderSettings.anisotropy = 1;
		renderSettings.mipMapBias = 2;
#if !defined( RED_CONFIGURATION_FINAL) && defined( RED_PLATFORM_WINPC ) 
		// renderSettings.forceConservative = true; TODO{LI}: Check why the surfels are messed up with it.
#endif // BAKE ONLY
	}
	else if ( rend::CCaptureHighResolutionScreenshot* scrIntreface = info.m_highResoulutionScreenshotInterface.Get() )
	{
		renderSettings.anisotropy = scrIntreface->GetTextureAnisotropy();
		renderSettings.mipMapBias = scrIntreface->GetTextureMipBias();
	}
	else
	{
		renderSettings.anisotropy = Config::GGlobalRenderingSettings.AnisotropyLevel;
#ifdef USE_DLSS
		if (m_ngx && m_ngx->IsDLSSEnabled() && Config::cvDLSSEnableCustomMipBias.Get())
		{
			renderSettings.mipMapBias = -1.0f;
		}
		else
#endif // USE_DLSS
		{
			renderSettings.mipMapBias = mipBias + Config::GGlobalRenderingSettings.TextureMipBias;
		}
	}

#if defined( USE_PROFILER ) && defined( RED_PLATFORM_CONSOLE )
	if ( auto devConfig = GetDevConfig() )
	{
		if ( devConfig->ConsoleSamplers.IsEnabled() )
		{
			renderSettings.samplerMipClampRegionSize = devConfig->ConsoleSamplers.samplerMipClampRegionSize;
			renderSettings.samplerZClampRegionSize = devConfig->ConsoleSamplers.samplerZClampRegionSize;
			renderSettings.samplerAnisoThreshold = devConfig->ConsoleSamplers.samplerAnisoThreshold;
			renderSettings.samplerAnisoBias = devConfig->ConsoleSamplers.samplerAnisoBias;
		}
	}
#endif

#if !defined( RED_CONFIGURATION_FINAL) && defined( RED_PLATFORM_WINPC ) 
	// For baking cascades we are bypassing culling based on triangles orientation in order to leverage occlusion of one sided geometry oriented away from sun.
	if ( info.m_displaySettings.m_displayMode == EMM_TodvisBakePreview ||
			info.m_displaySettings.m_displayMode == EMM_TodvisRuntimePreview ||
			info.GetFramePurpose() == FP_CascadesBaking )
	{
		renderSettings.forceTwoSided = true;
	}

	// When doing any screenshot/baking/etc type of render, we allow immediate PSO compilation, so we don't end up
	// missing anything.
	if ( !FramePurpose::IsNormal( info.GetFramePurpose() ) )
	{
		renderSettings.allowImmediatePSOs = true;
	}
#endif

	GpuApi::SetRenderSettings( renderSettings );

	// Proceed frame for batchDataAllocator
	GetRenderer()->GetBatchDataAllocator().NewAllocFrame();

	// At start of frame, scene's visibility scene needs a chance to prepare. Seems like an okay place to put it for now
	if ( scene != nullptr )
	{
		auto& visScene = scene->GetVisibilityScene();
		visScene.PrepareForQueries();
	}

	// #NOTE: If this changes, have a look in CRenderNode_DrawHUD::Execute() as well. On PS4 we need to close and reopen
	// the SceneRender scope.
	const Bool isOnscreen = info.m_present;
	m_GpuFrameTimer.BeginSceneRender( isOnscreen ? GPUFrameTimer::RenderType::MainScene : GPUFrameTimer::RenderType::Subscene );

	// Start rendering
	{
		PC_SCOPE(BeginRender);

		GpuApi::BeginRender();

#ifndef RED_CONFIGURATION_FINAL
		// Setup blank renderTarget in order to start with clean slate.
		// We were getting some assert from leftovers from previous frame (viewport sizes etc) when windows was resized.
		GpuApi::SetupBlankRenderTargets();
#endif

		if ( scene )
		{
			{
				// Update scene tickables
				PC_SCOPE( UpdateSceneTickables );

				if ( !m_resourceEngineHandler->IsTakingScreenshot() )
				{
					const Float frameTime = GetRenderer()->GetLastTickDelta();
					scene->TickProxies( frameTime );
				}

				scene->SetTickCollectionEnabled( !m_resourceEngineHandler->IsTakingScreenshot() );
			}
			scene->BeginRendering();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderInterface::EndFrameRendering( const SRenderNodeImplContext& rctx, IRenderFrame* frame )
{
	CRenderSceneEx* scene = static_cast< CRenderSceneEx* >( frame->GetFrameInfo().GetScene() );

	// End rendering
	{
		PC_SCOPE(EndRender);

#if !defined(RED_CONFIGURATION_FINAL) && defined(GPUAPI_DEBUG_NAMES)
		{	// A bit hacky
			static volatile Uint32 counter = 0;
			if( counter++ % 50 == 0 )
			{
				GpuApi::DumpResourceStats();
			}
		}
#endif

#ifdef USE_PROFILER
		{

			PC_SCOPE( UpdateRenderingStats );
#if !defined(RED_CONFIGURATION_FINAL)
			RenderingStats particleStats;
			particleStats.m_maxCpuParticlesAllocated = IParticleBuffer::g_numParticlesAllocated.GetValue();
			particleStats.m_numActiveCpuParticles = IParticleBuffer::g_numActiveParticles.GetValue();
			particleStats.m_particleVBFillRate = CParticleVertexBuffer::GetFillRate();
			particleStats.m_dplFillRate = CParticleDPLTexture::GetDPLTextureFillRate();
#endif
			for ( Uint32 camera_i = 0; camera_i < rctx.GetNumCameras(); ++camera_i )
			{
				CRenderCollector& collector = rctx.GetCameraDataByIndex( camera_i )->GetCollector();
				if ( collector.m_scene )
				{
					collector.m_scene->UpdateSceneStats( collector.m_renderingStats );
#if !defined(RED_CONFIGURATION_FINAL)
					collector.m_scene->UpdateSceneStats( particleStats );
#endif
				}
			}
		}
#endif

		{
			PC_SCOPE( ClearCollectors );
			for ( Uint32 camera_i = 0; camera_i < rctx.GetNumCameras(); ++camera_i )
			{
				rctx.GetCameraDataByIndex( camera_i )->GetCollector().Reset();
			}
		}

		if ( scene )
		{
			// Just in case, tick anything that might still be sitting in the collected tick list.
			scene->TickCollectedProxies( rctx );

			scene->EndRendering();


			// We're done using any vis queries, so notify the vis scene.
			auto& visScene = scene->GetVisibilityScene();
			visScene.FinishQueries();
		}

		EndFrame();

#ifdef USE_PROFILER
		if ( m_gpuProfiler )
		{
			m_gpuProfiler->OnFrameEnd();
		}
#endif

		const Bool isOnscreen = frame->GetFrameInfo().m_present;
		m_GpuFrameTimer.EndSceneRender( isOnscreen ? GPUFrameTimer::RenderType::MainScene : GPUFrameTimer::RenderType::Subscene );

		GpuApi::EndRender();
	}
}

namespace helper
{
	struct RunRenderJobParameter
	{
		const SRenderNodeContext nodeContext;
		const SRenderNodeParameters nodeParameters;
	};

	static void RunRenderJob( const RunRenderJobParameter & param, const job::RunContext& runContext )
	{
		auto renderNode = param.nodeParameters.m_impl;
		if( renderNode )
		{
			cpRenderJobPhase::Guard cpPhase(runContext.dispatcherThreadIndex,renderNode->GetName().AsChar());
			ScopedProfilerChannel forcedChannels( PBC_RENDER );
			SRenderNodeImplContext::SInitData nodeImplContextInitData( runContext.dispatcherThreadIndex );
			nodeImplContextInitData.m_frame			= CRenderNodeJob::GetJobsRenderFrame();
			nodeImplContextInitData.m_resAlloc		= GetRenderer()->GetRenderFlowAllocator();			
			nodeImplContextInitData.m_cameraIndex	= param.nodeContext.m_cameraIndex;

			job::Builder depBuilder{ runContext };

			// setup impl context
			SRenderNodeImplContext nodeImplContext;
			cpPhase.Phase(2);
			nodeImplContext.Init( nodeImplContextInitData );
			cpPhase.Phase(3);
			nodeImplContext.SetupNodeData( param.nodeContext, param.nodeParameters );

			cpPhase.Phase(4);
			renderNode->Process(nodeImplContext, &depBuilder);
			cpPhase.Phase(5);
		}
	}

	struct RenderJobNode
	{
		RenderJobNode() = default;
		explicit RenderJobNode( job::Priority p ) : nodeCounter{ p } {} // [skperf] round 60 critPrio
		job::Counter nodeCounter{ job::Priority::RenderPath }; // single render job node
		
		// incoming deps; could have multiple parents so need this counter to collect on vs waiting on a single parent's counter directly
		job::Counter incomingDependenciesCounter{ job::Priority::RenderPath };
		Uint32 numIncomingDependencies{0};
		Uint32 numOutgoingDependencies{0};
	};

	struct RenderJobCompletionDeferral
	{
		job::CompletionDeferral nodeDeferral;
		job::CompletionDeferral incomingDependenciesDeferral;
	};

	static void LinkRenderNodeGraphDependencies( const CRenderNodeGraph& graph, red::DynArray< RenderJobNode >& nodes, const job::Counter& renderJobsExternalDependency )
	{
		PC_SCOPE( LinkRenderNodeGraphDependencies );

		// Link dependencies
		const Uint32 numDeps = graph.GetNumDependencies();
		for ( Uint32 i = 0; i < numDeps; ++i )
		{
			const CRenderNodeGraph::SDependencyData &depData = graph.GetDependency( i );
			if ( RNDT_Cpu != depData.m_type )
			{
				continue;
			}

			const Uint32 parentIdx = graph.GetNodeIndex( depData.m_parentNode );
			const Uint32 childIdx = graph.GetNodeIndex( depData.m_childNode );

			// Synchronize; dependencies counter waits on nodeCounter
			nodes[ childIdx ].incomingDependenciesCounter += nodes[ parentIdx ].nodeCounter;

			nodes[ childIdx ].numIncomingDependencies += 1;
			nodes[ parentIdx ].numOutgoingDependencies += 1;
		}

		// link jobs to an external dependency
		Bool syncedToRenderStart = false;
		for ( RenderJobNode& node : nodes )
		{
			if ( node.numIncomingDependencies == 0 )
			{
				node.incomingDependenciesCounter += renderJobsExternalDependency;
				syncedToRenderStart = true;
			}
		}

		// Sanity check incoming node dependencies
		RED_FATAL_ASSERT( nodes.Empty() || syncedToRenderStart, "Cycle in render graph? All nodes have incoming dependencies" );
	}

	static void FinishContinuation( const DynArray< RenderJobNode >& nodes, job::Counter& renderFinishedCounter )
	{
		Bool syncedToRenderFinish = false;
		for ( const auto& node : nodes )
		{
			if ( node.numOutgoingDependencies == 0 )
			{
				RED_FATAL_ASSERT( !syncedToRenderFinish, "Render graph should have at most one finish job to end the frame" );
				syncedToRenderFinish = true;
				renderFinishedCounter += node.nodeCounter;
			}
		}

		RED_FATAL_ASSERT( nodes.Empty() || syncedToRenderFinish, "Render jobs not synced with frame!" );
	}

	class DebugSharedJobsCounter
	{
	public:
		DebugSharedJobsCounter();
		void Add( Int32 value ) const;
		void Decrement() const;
		Bool IsZero_Snapshot() const ;
		
	private:
#ifdef RED_ASSERTS_ENABLED
		mutable red::SharedPtr< red::Atomic< Int32 >, red::PoolDebug > m_counter;
#endif
	};



	DebugSharedJobsCounter::DebugSharedJobsCounter()
	{
#ifdef RED_ASSERTS_ENABLED
		m_counter = red::CreateSharedPtr< red::Atomic<Int32 >, red::PoolDebug >();
#endif
	}

	void DebugSharedJobsCounter::Add( Int32 value ) const
	{
#ifdef RED_ASSERTS_ENABLED
		m_counter->ExchangeAdd( value );
#endif
	}

	void DebugSharedJobsCounter::Decrement() const
	{
#ifdef RED_ASSERTS_ENABLED
		const Int32 newValue = m_counter->Decrement();
		RED_FATAL_ASSERT( newValue >= 0 );
#endif
	}

	Bool DebugSharedJobsCounter::IsZero_Snapshot() const
	{
#ifdef RED_ASSERTS_ENABLED
		return m_counter->GetValue() == 0;
#else
		return true;
#endif
	}

	static red::InstrumentationObject g_dummyNodeInstrObject( "dummy render node", PBC_RENDER );

	// [skperf] round 60 critPrio (opt-in ?critPrio=1): the render jobs that feed the late serial chain (tc80 ready probe:
	// DecoupledParticleLighting runnable at frame start but started ~6 ms later behind the big GBuffer/decal/cascade lists;
	// then HologramDepth_and_Distortion -> ResolveDistortion -> Transparents) run at CriticalPath instead of RenderPath
	// (the job's accumulate counter carries the queue priority). Only queue order changes; dependencies are untouched.
	static Bool CpCritPrioOn()
	{
		static const Bool on = []
		{
#if defined( RED_PLATFORM_WEB )
			const Int32 v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'critPrio' ) === '0' ? 0 : 1; } catch ( e ) { return 1; } } ); // [skperf] round 61: default ON (tc82); ?critPrio=0 = old
#else
			const Int32 v = 1;
#endif
			std::fprintf( stderr, "CP2077 skperf switches: critPrio=%d\n", v );
			return v != 0;
		}();
		return on;
	}
	// [skperf] round 62 critDecals (opt-in ?critDecals=1): tc83 DeferredDecals is the chain's top item (6.0 ms/frame in 185
	// of 241 frames) and its job, runnable at frame start, starts ~6 ms later behind the GBuffer lists; queue it (and its
	// decalSplit slices) at CriticalPath too. Needs critPrio on (default).
	static Bool CpCritDecalsOn()
	{
		static const Bool on = []
		{
#if defined( RED_PLATFORM_WEB )
			const Int32 v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'critDecals' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
#else
			const Int32 v = 0;
#endif
			std::fprintf( stderr, "CP2077 skperf switches: critDecals=%d\n", v );
			return v != 0;
		}();
		return on;
	}
	static Bool CpIsChainNode( const char* n )
	{
		if ( !n ) return false;
		if ( CpCritDecalsOn() && std::strncmp( n, "DeferredDecals", 14 ) == 0 && std::strcmp( n, "DeferredDecals_Overdraw" ) != 0 ) return true;
		static const char* const names[] = { "Lighting", "LightingMid", "LightingLate", "DecoupledParticleLighting", "HologramDepth_and_Distortion",
			"ReflectionProbes", "LocalLightBuffersRender", "ResolveDistortion", "Skin_Post_and_Forward_Opaque" };
		for ( const char* x : names ) if ( std::strcmp( n, x ) == 0 ) return true;
		return std::strncmp( n, "Transparents", 12 ) == 0;
	}

	static void DispatchRenderJobs( const CRenderNodeGraph& graph, DynArray< RenderJobNode >& nodes, const DebugSharedJobsCounter& debugCounter )
	{
		PC_SCOPE( DispatchRenderJobs );

		const Uint32 numNodes = graph.GetNumNodes();

		// Create actual jobs now that the dependencies are set up
		for ( Uint32 i = 0; i < numNodes; ++i )
		{
			CRenderNodeGraph::ItemId nodeId = graph.GetNode( i );
			RunRenderJobParameter param = { graph.GetNodeContext( nodeId ), graph.GetNodeParameters( nodeId ) };
			CRenderNodeBase * node = param.nodeParameters.m_impl;
			RenderJobNode& jobNode = nodes[ i ];

#ifdef USE_PROFILER
			red::InstrumentationObject& profilerObject = node ? *node->GetInstrumentationObject() : g_dummyNodeInstrObject;
#else
			static const char profilerObject[] = "RunRenderJob";
#endif

			const Bool debugIsFrameFinishJob = jobNode.numOutgoingDependencies == 0;
			job::JobDecl jobDecl;
			job::BuildJob< rend::PoolRenderGraph >( jobDecl, profilerObject, [param, debugCounter, debugIsFrameFinishJob]( const job::RunContext& context )
			{
				RunRenderJob( param, context );
				debugCounter.Decrement();
				RED_FATAL_ASSERT( !debugIsFrameFinishJob || debugCounter.IsZero_Snapshot(), "Finished frame before all render jobs completed!" );
				RED_UNUSED( debugIsFrameFinishJob );
			} );

			// Run job, waiting on dependencies
			const auto& waitForZeroCounter = jobNode.incomingDependenciesCounter;
			auto& accumulateCounter = jobNode.nodeCounter;
			job::RunJob( jobDecl, waitForZeroCounter, accumulateCounter );
		}

		// Add here at the end, no jobs should actually run yet until the counter signals ready.
		debugCounter.Add( numNodes );
	}

	// Note: this already synced the start of the frame
	void RunRenderNodeJobs( const CRenderNodeGraph& graph, const DebugSharedJobsCounter& debugCounter, const job::RunContext& runContext, const job::Counter& renderJobsExternalDependency )
	{
		PC_SCOPE( RunRenderNodeJobs );

		const Uint32 numNodes = graph.GetNumNodes();

		red::DynArray< RenderJobNode > nodes{ job::PoolJobScope() };
		nodes.Resize( numNodes );
		if ( CpCritPrioOn() ) // [skperf] round 60 critPrio
		{
			static std::atomic< Int32 > s_cpReported{ 0 };
			const Bool cpReport = s_cpReported.load( std::memory_order_relaxed ) < 2 && numNodes > 40;
			char cpLine[ 1800 ]; Int32 cpUsed = 0, cpBoosted = 0;
			for ( Uint32 i = 0; i < numNodes; ++i )
			{
				CRenderNodeBase* cpNode = graph.GetNodeParameters( graph.GetNode( i ) ).m_impl;
				const char* cpName = cpNode ? cpNode->GetName().AsChar() : nullptr;
				if ( !CpIsChainNode( cpName ) ) continue;
				nodes[ i ].~RenderJobNode();
				new ( &nodes[ i ] ) RenderJobNode( job::Priority::CriticalPath );
				++cpBoosted;
				if ( cpReport && cpUsed < 1700 ) { const Int32 w = std::snprintf( cpLine + cpUsed, sizeof( cpLine ) - cpUsed, " %s", cpName ); if ( w > 0 ) cpUsed += w; }
			}
			if ( cpReport && s_cpReported.fetch_add( 1 ) < 2 ) { cpLine[ cpUsed < 1799 ? cpUsed : 1799 ] = 0; std::fprintf( stderr, "CP2077 skperf critPrio: graph nodes=%u boosted=%d:%s\n", numNodes, cpBoosted, cpUsed ? cpLine : " (none)" ); }
		}

		red::DynArray< RenderJobCompletionDeferral > deferrals{ job::PoolJobScope() };
		deferrals.Reserve( numNodes );
		{
			PC_SCOPE( CreateDeferrals );
			for ( auto& it : nodes )
			{
				it.incomingDependenciesCounter += renderJobsExternalDependency;
				deferrals.EmplaceBack( RenderJobCompletionDeferral{ it.nodeCounter.CreateDeferral(), it.incomingDependenciesCounter.CreateDeferral() } );
			}
		}

		LinkRenderNodeGraphDependencies( graph, nodes, renderJobsExternalDependency );

		job::Builder depBuilder{ runContext };
		{
			job::Counter renderFinishedCounter{ job::Priority::RenderPath };
			FinishContinuation(nodes, renderFinishedCounter);
			depBuilder.DispatchWait(renderFinishedCounter);
		}

		DispatchRenderJobs( graph, nodes, debugCounter );
		
		debugCounter.Decrement(); // debugCounter started with +1
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



void AppendRenderGraphCameraHash( CRenderGraphCache::SCameraHash &refCameraSetupHash, IRenderFrame* frame, Uint32 cameraIndex )
{
	const ERenderingMode renderingMode = frame->GetFrameInfo().GetRenderingMode();
	CRenderFrameInfo &info = frame->GetFrameInfo();
	CRenderSceneEx* scene = static_cast< CRenderSceneEx* >( info.GetScene() );
	CRenderFrameCameraStorage* cameraStorage = scene ? scene->GetCameraStorage() : nullptr;
	CRenderFrameCameraStorage::SCameraData* cameraData = cameraStorage ? cameraStorage->GetCameraData( cameraIndex ) : nullptr;

	const EEnvManagerModifier displayMode = (EEnvManagerModifier)info.m_displaySettings.m_displayMode;
	const Bool isFullRender = cameraData && IRenderScene::IsValid( cameraData->m_info.m_renderScene );

	refCameraSetupHash.Append( renderingMode );
	refCameraSetupHash.Append( displayMode );
	refCameraSetupHash.Append( isFullRender );

	// Camera features are stored in a bit set. Internally it can be composed of many Uint64s and the camera hash must consider them all
	// in order to cover situation where we have more than 64 RFFs. We are hashing RFFs as they influence graph structure.
	const RenderFeatureSet& cameraFeatures = cameraData->GetInfo().GetFeatures();
	const Uint64* cameraFeaturesBitsetData = cameraFeatures.Data(); 
	const Uint32 singleDataSize = sizeof( RenderFeatureSet::StorageType ) * 8;
	const Uint32 cameraFeaturesBitsetDataSize =  ( cameraFeatures.Size() + singleDataSize - 1 ) / singleDataSize;
	for ( Uint32 i = 0; i < cameraFeaturesBitsetDataSize; i++ )
	{
		refCameraSetupHash.Append( cameraFeaturesBitsetData[ i ] );
	}
}

#ifdef RED_ASSERTS_ENABLED
red::Atomic< Bool > GDebugRenderFrame{ false };

struct ScopeAtomicRelease
{
	red::Atomic< Bool >*	atomic;

	ScopeAtomicRelease( red::Atomic< Bool >* atom )
		: atomic( atom )
	{
		while( atomic->Exchange( true ) )
		{
			RED_WARNING_ONCE( false, "Waiting on other render frame to finish" );
			red::SleepOnCurrentThread(1);
		}
	}

	~ScopeAtomicRelease()
	{
		if( atomic )
		{
			RED_FATAL_ASSERT( atomic->Exchange( false ) == true, "Wasn't rendering a frame?!" );
		}
	}
};

#endif

void CRenderInterface::RenderFrame( SRenderFrameContext& renderFrameContext )
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );

	const auto& frame = renderFrameContext.m_frame;

#ifdef RED_ASSERTS_ENABLED
	ScopeAtomicRelease scopedAtomic( &GDebugRenderFrame );
#endif

	PC_SCOPE( CRenderInterface_RenderFrame );

	CRenderFrameInfo & info		= frame->GetFrameInfo();
	const RenderViewport* viewport	= static_cast< const RenderViewport* >( info.GetViewport() );
	CRenderSceneEx* scene		= static_cast< CRenderSceneEx* >( info.GetScene() );
	CRenderSceneEx* originalScene = static_cast< CRenderSceneEx* >( info.GetOriginalScene() );
	CRenderSceneEx* nonInteractiveScene = static_cast< CRenderSceneEx* >(info.GetNonInteractiveScene());
	CRenderFrameCameraStorage* cameraStorage = scene ? scene->GetCameraStorage() : nullptr;

	const ERenderingMode renderingMode	= frame->GetFrameInfo().GetRenderingMode();
	const EFramePurpose framePurpose	= frame->GetFrameInfo().GetFramePurpose();

	// viewport can be null - eg. generating thumbnails
	if ( viewport )
	{
		// Prepare viewport - if device lost, do not render
		if ( IsDeviceLost() )
		{
			return;
		}
	}

	// early exit if input data invalid
	if ( 0 == info.GetViewportWidth() || 0 == info.GetViewportHeight() )
	{
		return;
	}

	// Toggle DLSS
	// #TODO: This could all be replaced with a more generic upsampler interface, then could be used for PC with DLSS
	// or some fallback, for Xbox with dual-plane presents and dynamic resolution, and maybe something on PS4 too?
	// #TODO: Maybe in FrameTickPrepare?
#ifdef USE_DLSS
	if ( m_ngx && Config::cvRayTracingOverrideEnable.Get() )
	{
		m_ngx->EnableDLSS( Config::cvDLSSEnable.Get() );
	}
#endif // USE_DLSS

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Scene prewarming
	{
		switch( framePurpose )
		{
		case FP_Blank:
		case FP_ScreenShot:
		case FP_DeferredScreenshot:
		case FP_EnvProbe:
		case FP_GlobalIllumination:
		case FP_GlobalIlluminationBackfaces:
			if ( scene )
			{
				PC_SCOPE( FinishDissolveSynchronizer );
				scene->FinishDissolveSynchronizer();
			}
			break;
		}
		
		const Bool todvisRuntimePreview = ( ( EEnvManagerModifier ) info.m_displaySettings.m_displayMode ) == EMM_TodvisRuntimePreview;
		const Bool todvisBakePreview = ( ( EEnvManagerModifier ) info.m_displaySettings.m_displayMode ) == EMM_TodvisBakePreview;
		if ( ( renderingMode == RM_TodvisBake || todvisRuntimePreview || todvisBakePreview ) && scene )
		{
			PC_SCOPE( FinishDissolveSynchronizer );
			scene->FinishDissolveSynchronizer();
		}
	}
	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Camera setup
	if ( cameraStorage )
	{
		// Don't flush temporary camera dependencies if we aren't doing a proper render. Otherwise a HitProxy render can
		// come in and invalidate those connections for the next normal render (dependent cameras flicker on click).

		SCameraPrepareContext ctx;
		ctx.m_frameInfo					= &info;
		ctx.m_allowCameraJitter			= info.m_displaySettings.HasLightingPass(); // Allow it here, something else can change it on the way
		ctx.m_flushTemporaries			= (renderingMode <= RM_ShadedMax || renderingMode == RM_SafeMode);
		ctx.m_renderViewport			= math::Rect( 0, info.GetViewportWidth(), 0, info.GetViewportHeight() );
		cameraStorage->AllocateCameraData( ctx, info.m_cameras );
	}

	const Uint32 numCameras = cameraStorage && renderingMode != RM_OverlayOnly ? cameraStorage->GetNumCameras() : 0;

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Graph caching
	CRenderNodeGraph* graph = nullptr;
	{
		PC_SCOPE( GraphCaching );

		const EEnvManagerModifier displayMode = (EEnvManagerModifier)info.m_displaySettings.m_displayMode;
		const Bool isWireframe = displayMode == EMM_WireframeSeethrough || displayMode == EMM_WireframeSolid;
		const Bool isTodvisDebug = ( displayMode == EMM_TodvisBakePreview  || displayMode == EMM_TodvisRuntimePreview );
		const Bool isOverdraw = displayMode == EMM_Overdraw || displayMode == EMM_OverdrawSeethrough;
		const Bool isHitProxiesDebug = displayMode == EMM_DebugHitProxies;
		const Bool isDebugVisualization = isWireframe || isOverdraw;

		// Build hash that defines unique set of current view  (mode+cameras)
		CRenderGraphCache::SCameraHash allCamerasHash;
		allCamerasHash.Append( numCameras );
		allCamerasHash.Append( displayMode );
		allCamerasHash.Append( Config::GGlobalRenderingSettings.LocalShadowsProcessedPerFrame );
		for ( Uint32 cameraIndex = 0; cameraIndex < numCameras; ++cameraIndex )
		{
			AppendRenderGraphCameraHash( allCamerasHash, frame.Get(), cameraIndex );
		}

		const Bool hack_asyncComputeFlagsChanged = false
#ifdef USE_RAY_TRACING
			|| Config::cvAsyncRayTracingASBuild.Get() != Config::previousAsyncRayTracingASBuild
#endif
			|| Config::cvAsyncComputeEnable.Get() != Config::previousAsyncComputeEnable
			|| Config::cvAsyncSSAO.Get() != Config::previousAsyncSSAO
			|| Config::cvAsyncHairClears.Get() != Config::previousAsyncHairClears
			|| Config::cvAsyncLutGeneration.Get() != Config::previousAsyncLutGeneration
			|| Config::cvAsyncDynamicTextureGeneration.Get() != Config::previousAsyncDynamicTextureGeneration
			|| Config::cvAsyncFlattenNormals.Get() != Config::previousAsyncFlattenNormals
			|| Config::cvAsyncBuildDepthChain.Get() != Config::previousAsyncBuildDepthChain
			;

		Uint32 numCameraSetups = (numCameras == 0 || framePurpose == FP_Blank) ? 1 : numCameras;

		Bool needsRebuild = false;
		CRenderGraphCache::CacheEntry& renderGraphCachEntry = m_graphCache->GetGraph( GetLastTickCounter(), allCamerasHash, numCameraSetups, needsRebuild, hack_asyncComputeFlagsChanged );
		graph = &renderGraphCachEntry.m_graph;

		// Rebuild graph if needed
		if( needsRebuild )
		{
#ifdef USE_RAY_TRACING
			Config::previousAsyncRayTracingASBuild = Config::cvAsyncRayTracingASBuild.Get();
#endif
			Config::previousAsyncComputeEnable = Config::cvAsyncComputeEnable.Get();
			Config::previousAsyncHairClears = Config::cvAsyncHairClears.Get();
			Config::previousAsyncSSAO = Config::cvAsyncSSAO.Get();
			Config::previousAsyncLutGeneration = Config::cvAsyncLutGeneration.Get();
			Config::previousAsyncDynamicTextureGeneration = Config::cvAsyncDynamicTextureGeneration.Get();
			Config::previousAsyncFlattenNormals = Config::cvAsyncFlattenNormals.Get();
			Config::previousAsyncBuildDepthChain = Config::cvAsyncBuildDepthChain.Get();

			// generate renderGraph
			CRenderNodeGraph& graph = renderGraphCachEntry.m_graph;

			if( numCameras == 0 )
			{
				CRenderGraphCache::SCameraSetupData& storage = *renderGraphCachEntry.m_cameraBuildData.Front();
				BuildRenderGraphBlank( storage.m_graph, storage.m_nodes, isWireframe, false );
				graph.AddGraph( storage.m_graph, true, 0, true );
			}
			else if( framePurpose == FP_Blank )
			{
				CRenderGraphCache::SCameraSetupData& storage = *renderGraphCachEntry.m_cameraBuildData.Front();
				BuildRenderGraphBlank( storage.m_graph, storage.m_nodes, isWireframe, true );
				graph.AddGraph( storage.m_graph, true, 0, true );
			}
			else
			{
				RED_FATAL_ASSERT( !info.m_nonCameraDebugDrawer, "This should be used only when there's no camera" );

				for( Uint32 cameraIndex = 0; cameraIndex < numCameras; ++cameraIndex )
				{
					// Quick fix to avoid rendering hit proxies for offscreen cameras.
					// TODO: make it nicer...
					const CRenderFrameCameraStorage::SCameraData* cameraData = cameraStorage->GetCameraData( cameraIndex );
					RED_ASSERT( cameraData, "Cameras supposed to exist at this point" );
					if ( cameraData->IsOffscreen() && renderingMode== RM_HitProxies ) 
						continue;

					CRenderGraphCache::SCameraHash cameraSetupHash;
					AppendRenderGraphCameraHash( cameraSetupHash, frame.Get(), cameraIndex );
					
					CRenderGraphCache::SCameraSetupData* storage = renderGraphCachEntry.FindCameraSetup( cameraSetupHash );
					RED_FATAL_ASSERT( nullptr != storage , "Missing graph storage" );

					// If this setup has no valid data. Build them. Reuse in other case.
					if( storage->DoNeedRebuild( cameraSetupHash ) )
					{
						switch ( renderingMode )
						{
						case RM_HitProxies:
						{
							BuildRenderGraphHitProxies( storage->m_graph, storage->m_nodes, info.m_multilayerSelection, /*isDebugView*/ false );
						}
						break;
						case RM_TodvisBake:
						{
							BuildRenderGraphTodvis( storage->m_graph, storage->m_nodes, /*isDebugView*/ false );
						}
						break;
						case RM_Shaded:
						case RM_Shaded_NoAmbient:
						{
							const CRenderFrameCameraStorage::SCameraData* camData = cameraStorage->GetCameraData( cameraIndex );

							if( IRenderScene::IsValid( camData->m_info.m_renderScene ) )
							{
								if ( isTodvisDebug )
								{
									BuildRenderGraphTodvis( storage->m_graph, storage->m_nodes, /*isDebugView*/ true );
								}
								else if ( isDebugVisualization )
								{
									BuildRenderGraphDebugVisualization( storage->m_graph, storage->m_nodes, camData->GetFeatures(), displayMode );
								}
								else if ( isHitProxiesDebug )
								{
									BuildRenderGraphHitProxies( storage->m_graph, storage->m_nodes, /*isMultilayer*/ false, /*isDebugView*/ true );
								}
								else
								{
									BuildRenderGraphCamera( storage->m_graph, storage->m_nodes, camData->GetFeatures(), displayMode );
								}
							}
							else
							{
								BuildRenderGraphNoScene( storage->m_graph, storage->m_nodes, isWireframe );
							}
						}
						break;

						case RM_SafeMode:
						{
							const CRenderFrameCameraStorage::SCameraData* camData = cameraStorage->GetCameraData( cameraIndex );

							if ( IRenderScene::IsValid( camData->m_info.m_renderScene ) )
							{
								BuildRenderGraphSafeMode( storage->m_graph, storage->m_nodes, camData->GetFeatures() );
							}
							else
							{
								BuildRenderGraphNoScene( storage->m_graph, storage->m_nodes, isWireframe );
							}
						}
						break;

						case RM_GBufferOnly:
						{
							BuildRenderGraphGBufferOnly( storage->m_graph, storage->m_nodes );
						}
						break;

						default:
							RED_FATAL( "Invalid rendering mode" );
						}					
					}

					graph.AddGraph( storage->m_graph, true, cameraIndex, true );
				}
			}

			graph.BuildRenderFlowGroups();

			renderGraphCachEntry.PostBuildClear();
		}
	}

	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Set jobs render frame
	{
		RED_FATAL_ASSERT( !CRenderNodeJob::GetJobsRenderFrame(), "Some pending frame from last frame of something? That should get cleared out" );
		CRenderNodeJob::SetJobsRenderFrame( frame.Get() );
	}


	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Start setting up render node jobs
	// This can take some time to do, linking dependencies and such, and doesn't depend on anything
	// other than having the graph built. So we start doing that early.

	// Node jobs need to wait for a couple things to finish before they can run, so set up a counter they can wait for.
	// It includes the DrawBuffers which were issued before we came in here, and Flow Allocator setup stuff.
	job::Counter nodesKickoffCounter;
	nodesKickoffCounter += m_renderCommandHandler->GetDrawBuffersWaitCounter();
	job::CompletionDeferral nodesKickoffDeferral = nodesKickoffCounter.CreateDeferral();

	helper::DebugSharedJobsCounter debugCounter;

	job::Builder renderNodeJobsBuilder { job::Priority::RenderPath };
	{
		graph->AcquireExclusiveUpdateFlag();

		// Can start building and linking the node jobs immediately. We'll pass in flowCounter to ensure the
		// nodes themselves don't run until draw buffers and flow allocator resolve are all finished.

		debugCounter.Add( 1 ); // RunRenderNodeJobs will decrement after all jobs setup and before signalready()
		renderNodeJobsBuilder.DispatchJob( "RunRenderNodeJobs", [graph, debugCounter, nodesKickoffCounter=std::move( nodesKickoffCounter )]( const job::RunContext& runContext )
		{
			job::Builder depBuilder { runContext };
			helper::RunRenderNodeJobs( *graph, debugCounter, runContext, nodesKickoffCounter );
		} );
	}


	// // // // // // // // // // // // // // // // // // // // // // // // // // // // // // 
	// Execute graph
	{
		PC_SCOPE( ExecuteGraph );

		// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		SRenderNodeImplContext rctx;
		{
			SRenderNodeImplContext::SInitData nodeImplContextInitData( renderFrameContext.m_dispatcherThreadIndex );
			nodeImplContextInitData.m_resAlloc	= GetRenderer()->GetRenderFlowAllocator();
			nodeImplContextInitData.m_frame		= frame;

			rctx.Init( nodeImplContextInitData );
		}

		// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		{
			Uint32 numNodes = graph->GetNumNodes();

			GetFrameCommandLists().PrepareForFrame( numNodes + ReservedCls::CL_COUNT );
		}

		// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

		if( rctx.HasAnyCameraStorage() )
		{
			PC_SCOPE( ProcessCameraData );

			auto storage = rctx.GetCameraStorage();

			{
				GpuApi::CommandListRef commandList = GpuApi::CreateCommandList( GpuApi::CommandListType::Default, red::CalculateAnsiHash32("StorageData") );
#ifndef RED_CONFIGURATION_FINAL
				GpuApi::SetResourceDebugName( commandList, "StorageData" );
#endif
				GetRenderer()->GetFrameCommandLists().SetCommandList( ReservedCls::CL_STORAGE_DATA , commandList );
				GpuApi::BindCommandList( commandList );
				GpuApi::BeginProfilerBlock( "StorageData" );
			}

			// Prepare scene custom data
			storage->PrepareCustomData( rctx );

			// For every camera
			for( Uint32 cameraIdx = 0, numCameras = storage->GetNumCameras(); cameraIdx < numCameras; ++cameraIdx )
			{
				// Fetch camera data
				CRenderFrameCameraStorage::SCameraData &cameraData = *storage->GetCameraData( cameraIdx );

				// Prepare camera data
				for( auto& data : cameraData.m_customData )
				{
					if( data != nullptr )
					{
						// HACK setting up node data here to make sure camera data is available 
						// when calling TXAACustomData::Prepare
						SRenderNodeContext dummyContext;
						dummyContext.m_cameraIndex = cameraIdx;
						SRenderNodeParameters dummyParams;
						rctx.SetupNodeData(dummyContext, dummyParams);

						data->SetScene( rctx.GetScene() );
						data->Prepare( rctx, cameraData );
						data->MarkAsPrepared();
					}
				}

				cameraData.MarkAsUsed();
			}

			{
				GpuApi::EndProfilerBlock();
				GpuApi::BindCommandList( {} );
			}
		}

#ifdef RED_PLATFORM_DURANGO
		{
			// Can't use ESRAM if we have some debug view enabled. Otherwise we might need to carry some ESRAM-placed
			// texture later into the frame than expected, causing something else to fail to go into ESRAM, and leading
			// to continuous reallocation of that.
			const Bool canUseESRAM =
				FramePurpose::IsNormal( info.GetFramePurpose() ) &&
				info.m_displaySettings.m_displayMode == EMM_None;
			m_renderFlowAllocator->SetAllowESRAM( canUseESRAM );
		}
#endif

		// In-game, if this is an offscreen render, or not rendering a full scene, don't process flow resource eviction.
		// When we go into a menu, we don't want to evict everything that's normally used for rendering the game. We'll
		// just have to re-allocate it when we leave the menu.
		// Outside of game, things are probably a bit more complicated, so just always process it, and don't worry.
		{
			const Bool haveViewport = (info.GetViewport() != nullptr);
			const Bool isBlankFrame = (numCameras == 0 || framePurpose == FP_Blank);
			const Bool processEviction = (IsGameMode() ? (haveViewport && !isBlankFrame) : true);
			m_renderFlowAllocator->SetProcessEviction( processEviction );
		}

		{
			PC_SCOPE( FlowAllocator_Startup );
			m_renderFlowAllocator->SetPhase( RFP_Startup );
		}

		{
			PC_SCOPE( FlowAllocator_PreConsume );
			m_renderFlowAllocator->SetPhase( RFP_PreConsume );
			graph->ExecuteParallel( rctx, renderFrameContext.m_builder );
		}

		renderFrameContext.m_builder.DispatchJob( "FlowAllocator_Resolve", [this]( const job::RunContext& runContext ) mutable
		{
			job::Builder depBuilder { runContext };
			m_renderFlowAllocator->SetPhase( RFP_Consume, &depBuilder );
		} );

		// We need to hold the nodes kickoff deferral until the Resolve is finished, so they won't run early.
		renderFrameContext.m_builder.DispatchJob( "FlowAllocator_Resolve_Finish", [nodesKickoffDeferral=std::move( nodesKickoffDeferral )]( const job::RunContext& runContext ) mutable
		{
			nodesKickoffDeferral.FinishDeferral();
		} );

		{
#ifdef RED_ASSERTS_ENABLED
			// At this point we no longer need this atomic to be reset
			scopedAtomic.atomic = nullptr;
#endif

			// Node jobs were issued separately, so we need to sync our main builder here with those.
			renderFrameContext.m_builder.DispatchWait( renderNodeJobsBuilder.ExtractWaitCounter() );

			renderFrameContext.m_builder.DispatchJob( "RunRenderNodeJobs/Cleanup", [graph, debugCounter]( const job::RunContext& runContext )
			{
				RED_FATAL_ASSERT( debugCounter.IsZero_Snapshot(), "Render jobs not finished!" );
				RED_UNUSED( debugCounter );
				graph->ReleaseExclusiveUpdateFlag();
			} );
		}
	}
}
