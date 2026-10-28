/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <atomic>
#include <cstdio>
#endif

#include "renderInterface.h"

#include "gameplayFx.h"
#include "renderGeometryBatcher.h"
#include "renderSkybox.h"
#include "renderShadowManager.h"
#include "renderVolumetricFogManager.h"
#include "renderGpuNoiseManager.h"
#include "renderPostProcess.h"
#include "renderHiZ.h"
#include "renderGIManager.h"
#include "renderReflectionProbeManager.h"
#include "renderDistantLightBatcher.h"
#include "clusteredDeferred.h"
#include "renderPostFx.h"
#include "renderCube.h"
#include "renderVisibilityDebugDrawer.h"
#include "visibilityScene.h"
#include "visibilityQueries.h"
#include "renderDebugDrawerImpl.h"
#include "renderSurfaceCache.h"
#include "renderVideoPlayer.h"
#include "renderViewport.h"
#include "renderSkinManager.h"

#include "../../renderData/include/renderFrameInfo.h"
#include "../../renderData/include/renderSelectable.h"
#include "../../renderData/include/renderSelectionResult.h"
#include "../../renderData/include/renderGlobalBindings.h"
#include "../../renderData/include/renderEngineHandler.h"
#include "../../redJobs2/include/jobBuilder.h"

#include "renderNodeJob.h"
#include "renderGraphNodes.h"
#include "speedTreeWindSimulation.h"
#include "renderScene.h"
#include "renderBRDF.h"
#include "renderHistogram.h"
#include "renderDynamicTextureManager.h"
#include "renderPostFxSelection.h"
#include "fontCache.h"
#include "renderGradient.h"
#include "renderNodeGraph.h"
#include "windImpulseCollector.h"
#include "interiorMapDataUpdater.h"
#include "renderRainMap.h"
#include "particleDPLTexture.h"

#include "../../../../engine/shaders/include/include_topDownCarMap.fx"
#include "../../../../engine/shaders/include/include_waterMap.fx"
#include "../../../../engine/shaders/include/globalConstantsCS.fx"
#include "../../../../engine/shaders/include/include_distortion.fx"

#include "renderProxyAutoSpawner.h"


#ifdef RENDER_LET_CAS_CLEAR_COMPOSITION
static constexpr Bool c_compositionDenyUnorderedAccess = false;
#else
static constexpr Bool c_compositionDenyUnorderedAccess = true;
#endif


using namespace PostProcessUtilities;
using red::DynArray;

namespace Config
{
	TConfigVar< Int32 >			cvHiResEntityCustomShadowmapSize( "Rendering", "HiResEntityCustomShadowmapSize", -1, eConsoleVarFlag_Developer );
	TConfigVar< Int32 >			cvFrostedGlassBlurQuality( "Rendering/FrostedGlass", "GlassBlurQuality", 0, 0, CRenderNode_ResolveDistortion::kNumBlurQualities, eConsoleVarFlag_Developer );
	TConfigVar< Int32 >			cvFrostedGlassAAQuality( "Rendering/FrostedGlass", "GlassAAQuality", 0, 0, CRenderNode_ResolveDistortion::kNumSimpleTemporalQualities, eConsoleVarFlag_Developer );

	extern TConfigVar< Int32 >		cvHairLateAllocVSLimit;

	extern TConfigVar< Int32 > cvMultiLayerSelectionMaxLayersCount;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	RED_FORCE_INLINE static Uint32 CountTrailingZerosAndClear( Uint64& mask )
	{
#ifdef _MSC_VER
		unsigned long index = 0;
		::_BitScanForward64( &index, mask );
		const Uint32 slot = index;
#else
		const Uint32 slot = __builtin_ctzll( mask );
#endif
		mask &= mask - 1;
		return slot;
	}
}

#ifdef USE_PROFILER
red::InstrumentationObject CRenderNode_Dummy::sm_instrumentationObject( "RenderNodeDummy" );
#endif


GpuApi::CommandListRef CRenderNodeBase::CreateCommandList() const
{
	RED_HALT("Render node doesn't support command list creation");
	return GpuApi::CommandListRef::Null();
}


void CRenderNodeBase::Process( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	RED_ASSERT( rctx.GetDispatcherThreadIndex() != -1, "Uninitialized dispatcherThreadIndex in CRenderNodeBase::Process" );

	const Bool consumePhase = rctx.IsConsumePhase();
	const RenderNodeCommandListUsage clUsage = GetCommandListUsage();
	const Bool useCommandList = consumePhase && HasCommandList( clUsage );

	if ( consumePhase )
	{
		if ( clUsage == RenderNodeCommandListUsage::Own )
		{
			PC_SCOPE(OpenCommandList);

			GpuApi::CommandListRef commandList = CreateCommandList();
			rctx.SetCommandList( commandList );
		}
	}

	const TRenderFlowGroup renderFlowGroup = rctx.GetRenderFlowGroup();
	if ( useCommandList )
	{
		GpuApi::BindCommandList( rctx.GetCommandList() );
		BeginProfilerBlock();
	}

	// Check if we modifing some global bindings
	if ( consumePhase && m_globalBindingMod != 0 )
	{
		RED_ASSERT(useCommandList, "globalBindingMod can only be used if the node is using a command list");

		// If so, unbind it one by one
		Uint64 slots = m_globalBindingMod;

		while( slots )
		{
			const Uint32 slot = CountTrailingZerosAndClear( slots );

			GpuApi::UnlockTexturesSlots( slot, 1 );

			const auto& res = GetRenderer()->GetGlobalBinding( slot );

			// Unbind this and release modification lock
			if( res.m_isTexture )
			{
				GpuApi::BindTexture( slot, GpuApi::TextureRef::Null(), GpuWrapApi::ShaderTypeMask(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader) );
			}
			else
			{
				GpuApi::BindBufferSRV( GpuApi::BufferRef::Null(), slot, GpuWrapApi::ShaderTypeMask(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader) );
			}
		}
	}

	// Execute node depended stuff
	{
		rctx.BeginNewNode();

		this->Execute( rctx, builder );
	}

	if ( useCommandList )
	{
		GpuApi::BindCommandList({});

		if ( GetJobBuilderUsage() )
		{
			builder->DispatchJob( "CRenderNode_Epilogue", [this, rctx]( const job::RunContext & runContext )
			{
				ProcessEpilogue( rctx );
			} );
		}
		else
		{
			ProcessEpilogue( rctx );
		}
	}
}

void CRenderNodeBase::ProcessEpilogue( const SRenderNodeImplContext& rctx ) const
{
	RED_ASSERT(rctx.IsConsumePhase(), "Should not be running epilogue outside of consume phase");

	const RenderNodeCommandListUsage clUsage = GetCommandListUsage();
	RED_ASSERT(clUsage != RenderNodeCommandListUsage::None, "Should not be running epilogue when node doesn't use a command list");

	RED_ASSERT(rctx.HasCommandList(), "Render node context has no active command list");
	GpuApi::CommandListRef commandList = rctx.GetCommandList();

	const TRenderFlowGroup renderFlowGroup = rctx.GetRenderFlowGroup();

	GpuApi::BindCommandList( commandList );

	rctx.Unbind( !m_rtBinder );


	// Restore old global bindings if needed
	// If so, unbind it one by one
	Uint64 slots = m_globalBindingMod;
	while(slots)
	{
		const Uint32 slot = CountTrailingZerosAndClear( slots );

		const auto& res = GetRenderer()->GetGlobalBinding( slot );

		// Unbind this and release modification lock
		if(res.m_isTexture)
		{
			GpuApi::BindTexture( slot, res.m_texture, GpuWrapApi::ShaderTypeMask( GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader));
		}
		else
		{
			GpuApi::BindBufferSRV( res.m_buffer, slot, GpuWrapApi::ShaderTypeMask(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader));
		}

		GpuApi::LockTexturesSlots( slot, 1 );
	}

	EndProfilerBlock();

	GpuApi::BindCommandList({});
}

void CRenderNodeBase::BeginProfilerBlock() const
{
#ifdef USE_PROFILER
	if ( AllowGpuScope() )
	{
		const CName name = GetName();
		GpuApi::BeginProfilerBlock( name.AsChar() );
	}

	if ( IRenderGpuProfiler* gpuProfiler = GetRenderer()->GetGpuProfiler() )
	{
		if ( AllowGpuScope() )
		{
			const CName name = GetName();
			auto entry = gpuProfiler->AddEntry( name );
			m_gpuProfilerID = gpuProfiler->StartEntry( entry );
		}
	}
#endif
}

void CRenderNodeBase::EndProfilerBlock() const
{
#ifdef USE_PROFILER
	if ( IRenderGpuProfiler* gpuProfiler = GetRenderer()->GetGpuProfiler() )
	{
		if ( m_gpuProfilerID != CRenderGpuProfiler::MAX_ENTRIES )
		{
			gpuProfiler->EndEntry( m_gpuProfilerID );
			m_gpuProfilerID = CRenderGpuProfiler::MAX_ENTRIES;
		}
	}

	if ( AllowGpuScope() )
	{
		GpuApi::EndProfilerBlock();
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

CRenderNode_Synchronize::CRenderNode_Synchronize( GpuApi::CommandListSyncType sync, const char* name /*= nullptr*/ )
	: m_sync( sync )
{
	if ( name != nullptr )
	{
		m_name = name;
	}
}

void CRenderNode_Synchronize::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	rctx.GetResourceAllocator()->RequestQueueSync( rctx.GetRenderFlowGroup(), m_sync );

	if ( rctx.IsConsumePhase() )
	{
		RED_ASSERT( builder, "Job builder expected" );
		RED_ASSERT( !m_name.Empty(), "Must provide a name when doing RenderSync::Flush" );
		GetRenderer()->GetFrameCommandLists().Submit( m_name.AsChar(), rctx.GetRenderFlowGroup(), m_sync, *builder );
	}
}


void CRenderNode_StartRender::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo& frameInfo = rctx.GetFrameInfo();

	// Start frame rendering
	if( rctx.IsConsumePhase() )
	{
		RED_ASSERT( builder, "Job builder expected" );
		GetRenderer()->StartFrameRendering( rctx, *builder );
	}

	// allocate extracted depthBuffer
	if( nullptr != frameInfo.m_depthResult )
	{
		SRenderFlowTargetDesc desc = rctx.GetResolvedFinalTargetDesc();
		desc.m_format = GpuWrapApi::TEXFMT_R32_Float;
		rctx.RTSharedAlloc( RENDER_NAME_TAG( "extractedDepthBuffer" ), desc );
	}

	if ( rctx.GetFrameInfo().GetRenderingMode() == RM_HitProxies )
	{
		SRenderFlowTargetDesc desc;
		desc.m_type			= FRT_Buffer;
		desc.m_maxElemCount	= desc.m_elemCount = (Uint32)(Config::cvMultiLayerSelectionMaxLayersCount.Get() * frameInfo.m_selectionArea.Width() * frameInfo.m_selectionArea.Height());
		desc.m_elemStride	= sizeof( rend::SelectionResultType::GPUBufferPixel );
		desc.m_category		= GpuWrapApi::BCC_StructuredAppendUAV;
		desc.m_usage		= GpuApi::BUT_Default;

		rctx.RTSharedAlloc( RENDER_NAME_TAG( "multiLayerSelectionUAV" ), desc );
	}

	// #TODO: Doesn't need to be per-render. Could probably just do it during startup.
	{
		// update ambient brdf
		if ( auto brdf = GetRenderer()->GetBRDF() )
		{
			brdf->GenerateLUT( rctx );
		}

		// initialize noise textures
		if ( auto noiseMgr = GetRenderer()->GetGpuNoiseManager() )
		{
			noiseMgr->Initialize( rctx );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_EndRender::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		GetRenderer()->EndFrameRendering( rctx, rctx.GetFrame() );
	}
}


void CRenderNode_Present::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		const CRenderFrameInfo& info = rctx.GetFrameInfo();
		RenderViewport* viewport = static_cast< RenderViewport* >( info.GetViewport() );

		// Present on viewport (if it is not a render to texture scene), if texture streaming flush was requested do not show
		if ( viewport && info.m_present )
		{
			viewport->Present();
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

Vector4 GetGBufferDefaultClearColor( Uint32 gbufferIndex );


void CRenderNode_DeclareCommonResourceAllocs::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo& info = rctx.GetFrameInfo();
	const auto colorDesc = rctx.GetRegularPrecisionColorTargetDesc();

	const Uint32 surfacesWidth  = colorDesc.m_sizeWidth;
	const Uint32 surfacesHeight = colorDesc.m_sizeHeight;

	RED_ASSERT( info.GetRenderingMode() == RM_Shaded, "This node is intended to declare resources for shaded mode" );

	// Some stuff is only needed if we're doing the full scene render. If we aren't, then don't even declare them, to
	// make sure they aren't accidentally created.
	if( rctx.HasScene() )
	{
		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;

			desc.m_format = GpuWrapApi::TEXFMT_R10G10B10A2_Unorm;
			RED_FATAL_ASSERT( nullptr == info.m_gBufferAlbedo8888Result, "10-bit albedo is rendered, but 8888 format result requested" );

			desc.m_isSamplable = true;
			desc.m_denyUnorderedAccess = true;
			desc.m_sizeWidth = surfacesWidth;
			desc.m_sizeHeight = surfacesHeight;
			desc.m_maxWidth = rctx.GetMaxWidth();
			desc.m_maxHeight = rctx.GetMaxHeight();

			desc.m_extraUsageExFlags |= GpuApi::TEXUSAGEEX_EnableColorFastClear;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 0 ) );

			{
				desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
				rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer0" ), desc);
				desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			}

			// higher precision for normals
			desc.m_format = GpuWrapApi::TEXFMT_R10G10B10A2_Unorm;
			desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 1 ) );
			rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer1" ), desc );
			desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;

			// material properties (roughness,metalness,translucency,emissive)
			desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 2 ) );

			{
				desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
				rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer2" ), desc);
				desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			}

			if ( rctx.Test( RFF_VelocityBuffer ) )
			{
				rctx.RTAlloc( RENDER_NAME_TAG( "velocityBuffer" ), rctx.GetVelocityBufferDesc() );
			}

			desc.m_extraUsageExFlags &= ~GpuApi::TEXUSAGEEX_EnableColorFastClear;
			desc.SetOptimalClearValue( Vector4::ZEROS() );

			// In R global light is stored, on G packed contact shadows from local lights
			desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
			desc.m_denyUnorderedAccess = false;
			desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
			rctx.RTAlloc(RENDER_NAME_TAG( "normals" ), desc);

			desc.m_format = GpuWrapApi::TEXFMT_R8G8_Unorm;
			desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			desc.m_denyUnorderedAccess = true;

#ifdef USE_RAY_TRACING
			if ( rctx.Test( RFF_RayTracedDirectionalShadow ) )
			{
				desc.m_denyUnorderedAccess = false;
			}
#endif
			rctx.RTAlloc( RENDER_NAME_TAG( "shadowmask" ), desc );
		}

#if SURFACE_CACHE_ENABLE_RUNTIME_BAKING
		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;
			desc.m_format = GpuWrapApi::TEXFMT_R32_Uint;
			desc.m_isSamplable = true;
			desc.m_sizeWidth = Max( 1u, surfacesWidth / 2 );
			desc.m_sizeHeight = Max( 1u, surfacesHeight / 2 );
			desc.m_maxWidth = Max( 1u, rctx.GetMaxWidth() / 2 );
			desc.m_maxHeight = Max( 1u, rctx.GetMaxHeight() / 2 );
			rctx.RTAlloc( RENDER_NAME_TAG( "surfaceRequests" ), desc );
		}
#endif

		{
			SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();
			rctx.RTAlloc( RENDER_NAME_TAG( "color" ), desc );
			rctx.RTAlloc( RENDER_NAME_TAG( "lightBufferDiffuse" ), desc );
			rctx.RTAlloc( RENDER_NAME_TAG( "lightBufferSpecular" ), desc );
			rctx.RTAlloc( RENDER_NAME_TAG( "bufferReflection" ), desc );
			rctx.RTAlloc( RENDER_NAME_TAG( "globalIllumination" ), desc );
		}

		if( rctx.Test( RFF_Hair ) )
		{
			SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();

			// #TODO: If I normalize the adjusted alpha weights, I can probably get away with this being RGBA8.
			desc.m_denyUnorderedAccess = true;
			rctx.RTAlloc( RENDER_NAME_TAG( "hair_color" ), desc );

			desc.m_format = GpuWrapApi::TEXFMT_A8_Unorm;
			desc.SetOptimalClearValue( Vector4::ONES() );
			rctx.RTAlloc( RENDER_NAME_TAG( "hair_alpha" ), desc );
			desc.m_denyUnorderedAccess = false;

			desc.m_format = GpuWrapApi::TEXFMT_R32_Uint;
			desc.SetOptimalClearValue( Vector4::ZEROS() );
			desc.m_numSlices = 4;
			rctx.RTAlloc( RENDER_NAME_TAG( "hair_depths" ), desc );
		}

		if ( rctx.Test( RFF_CharacterLightBlockers ) )
		{
			SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc(GpuWrapApi::TEXFMT_R8G8B8A8_Unorm);
			desc.m_denyUnorderedAccess	= true;

			// TODO: make this 1/16th of the original RenderTarget, right now it is used along with depth, so size must match
			//desc.m_sizeWidth			= red::DivideRoundingUp( desc.m_sizeWidth, 2 );
			//desc.m_sizeHeight			= red::DivideRoundingUp( desc.m_sizeHeight, 2 );

			rctx.RTAlloc( RENDER_NAME_TAG( "lightBlockers" ), desc );
		}

		if ( rctx.Test( RFF_ScreenSpaceReflections ) )
		{
			// To avoid overcomplication of code for minimal gain, let's make SSR 32bpp always.
			SRenderFlowTargetDesc descR = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R11G11B10_Float );
			SRenderFlowTargetDesc descF = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R8_Unorm );
			descR.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
			rctx.RTAlloc( RENDER_NAME_TAG( "SSR_Result" ), descR );
			rctx.RTAlloc( RENDER_NAME_TAG( "SSR_Fade" ), descF );
		}

		if ( rctx.Test( RFF_LocalShadows ) )
		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;
			desc.m_format = GpuWrapApi::TEXFMT_D32F;
			desc.m_isSamplable = true;
			desc.m_maxWidth = desc.m_sizeWidth = Config::GGlobalRenderingSettings.LocalShadowsResolution;
			desc.m_maxHeight = desc.m_sizeHeight = Config::GGlobalRenderingSettings.LocalShadowsResolution;
			desc.m_optimalClearValue[0] = 1.0f;
			rctx.RTAlloc( RENDER_NAME_TAG( "localShadowsSharedDepth" ), desc );

			desc.m_format = GpuWrapApi::TEXFMT_R16G16_Unorm;
			rctx.RTAlloc( RENDER_NAME_TAG( "localShadowsSharedVSMIntermediate" ), desc );
		}

		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;
			desc.m_format = GpuWrapApi::TEXFMT_D16U;
			desc.m_isSamplable = true;
			desc.m_numSlices = Config::GetCascadedShadowsMaxCount();

			if( rctx.Test( RFF_CascadeShadows ) )
			{
				desc.m_sizeWidth = Config::GetCascadedShadowsResolution();
				desc.m_sizeHeight = Config::GetCascadedShadowsResolution();
			}
			else // Create super small texture to simply reduce memory overhead and clearing cost
			{
				desc.m_sizeWidth = 32;
				desc.m_sizeHeight = 32;
			}
			desc.m_maxWidth = desc.m_sizeWidth;
			desc.m_maxHeight = desc.m_sizeHeight;

			desc.m_optimalClearValue[0] = 1.0f;

			rctx.RTAlloc( RENDER_NAME_TAG( "cascades" ), desc );
		}

		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
			desc.m_isSamplable = true;
			desc.m_maxWidth = desc.m_sizeWidth = Config::GGlobalRenderingSettings.volumetricCloudsShadowResolution;
			desc.m_maxHeight = desc.m_sizeHeight = Config::GGlobalRenderingSettings.volumetricCloudsShadowResolution;

			rctx.RTAlloc( RENDER_NAME_TAG( "cloudsShadow" ), desc );
		}

		{
			SRenderFlowTargetDesc rtDesc;
			rtDesc.m_type = FRT_Texture;
			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_extraUsageExFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Uint;
			rtDesc.m_isSamplable = true;
			rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_CAR_MAP_RES;
			rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_CAR_MAP_RES;
			rtDesc.m_denyUnorderedAccess = true;

			rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepth" ), rtDesc );
			rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ), rtDesc );
		}

		{
			SRenderFlowTargetDesc rtDesc;
			rtDesc.m_type = FRT_Texture;
			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Unorm;
			rtDesc.m_isSamplable = true;
			rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_WATER_MAP_RES;
			rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_WATER_MAP_RES;
			rtDesc.m_denyUnorderedAccess = true;

			rctx.RTAlloc( RENDER_NAME_TAG( "topDownRainDepth" ), rtDesc );
		}	

		{
			SRenderFlowTargetDesc rtDesc;
			rtDesc.m_type = FRT_Texture;
			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;
			rtDesc.m_isSamplable = true;
			rtDesc.m_maxWidth = rctx.GetMaxWidth();
			rtDesc.m_maxHeight = rctx.GetMaxHeight();
			rtDesc.m_sizeWidth = surfacesWidth;
			rtDesc.m_sizeHeight = surfacesHeight;
			rtDesc.m_denyUnorderedAccess = true;

			rctx.RTAlloc( RENDER_NAME_TAG( "transparencyTXAAMask" ), rtDesc );
		}

		{
			SRenderFlowTargetDesc rtDesc;
			rtDesc.m_type = FRT_Texture;
			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;
			rtDesc.m_isSamplable = true;
			rtDesc.m_maxWidth = rctx.GetMaxWidth() / 2;
			rtDesc.m_maxHeight = rctx.GetMaxHeight() / 2;
			rtDesc.m_sizeWidth = surfacesWidth / 2;
			rtDesc.m_sizeHeight = surfacesHeight / 2;
			rtDesc.m_denyUnorderedAccess = true;

			rctx.RTAlloc( RENDER_NAME_TAG( "temporalInvalidationMask" ), rtDesc );
		}

		{
			SRenderFlowTargetDesc rtDesc;
			rtDesc.m_type = FRT_Texture;
			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R16G16_Snorm;
			rtDesc.m_isSamplable = true;
			rtDesc.m_maxWidth = rctx.GetMaxWidth();
			rtDesc.m_maxHeight = rctx.GetMaxHeight();
			rtDesc.m_sizeWidth = surfacesWidth;
			rtDesc.m_sizeHeight = surfacesHeight;
			rtDesc.m_denyUnorderedAccess = true;
			rtDesc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

			rctx.RTAlloc( RENDER_NAME_TAG( "distortionMask" ), rtDesc );

			rtDesc.m_extraUsageFlags = 0;
			rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;
			rctx.RTAlloc( RENDER_NAME_TAG( "glassBlurRadius" ), rtDesc );

			rtDesc.m_maxWidth = red::DivideRoundingUp( rctx.GetMaxWidth(), RESOLVE_DISTORTION_TILE_SIZE );
			rtDesc.m_maxHeight = red::DivideRoundingUp( rctx.GetMaxHeight(), RESOLVE_DISTORTION_TILE_SIZE );
			rtDesc.m_sizeWidth = red::DivideRoundingUp( surfacesWidth, RESOLVE_DISTORTION_TILE_SIZE );
			rtDesc.m_sizeHeight = red::DivideRoundingUp( surfacesHeight, RESOLVE_DISTORTION_TILE_SIZE );
			rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Uint;
			rtDesc.m_denyUnorderedAccess = false;
			rtDesc.m_extraUsageFlags = 0;

			rctx.RTAlloc( RENDER_NAME_TAG( "distortionMaskTiles" ), rtDesc );
			rctx.RTAlloc( RENDER_NAME_TAG( "frostedGlassBlurMaskTiles" ), rtDesc );
		}
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_optimalClearValue[0] = 0.0f;
		desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident|GpuApi::TEXUSAGE_ESRAMResident_LargeLane;

		rctx.RTAlloc( RENDER_NAME_TAG( "depthBuffer" ), desc );
		desc.m_extraUsageFlags &= ~(GpuApi::TEXUSAGE_ESRAMResident|GpuApi::TEXUSAGE_ESRAMResident_LargeLane);

		if ( rctx.Test( RFF_SkinTranslucency ) )
		{
			desc.m_optimalClearValue[0] = 1.0f;
			rctx.RTAlloc( RENDER_NAME_TAG( "rtSkinTranslucency" ), desc );
		}
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_format = GpuWrapApi::TEXFMT_D32F;
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ), desc );
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBufferHologram" ), desc );
	}

	if ( rctx.Test( RFF_HUD ) && rctx.GetFrameInfo().m_composition != nullptr )
	{
		SRenderFlowTargetDesc compTargetDesc = rctx.GetCompositionDesc();
		compTargetDesc.m_denyUnorderedAccess = c_compositionDenyUnorderedAccess;
		compTargetDesc.m_extraUsageFlags = GpuApi::TEXUSAGE_ESRAMResident|GpuApi::TEXUSAGE_ESRAMResident_LargeLane;

		rctx.RTAlloc( RENDER_NAME_TAG( "composition" ), compTargetDesc );
	}

#ifdef USE_RAY_TRACING
	{
		Bool useRayTracing = rctx.Test( RFF_RayTracedAmbientOcclusion ) ||
			rctx.Test( RFF_RayTracedDiffuseIllumination ) ||
			rctx.Test( RFF_RayTracedReflection ) ||
			rctx.Test( RFF_RayTracedTranparentReflection ) ||
			rctx.Test( RFF_RayTracedDirectionalShadow ) ||
			rctx.Test( RFF_RayTracingDebug );

		if (useRayTracing)
		{
			SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();
			desc.m_format = GpuWrapApi::TEXFMT_R16G16B16A16_Float;

#ifndef RED_CONFIGURATION_FINAL
			rctx.RTAlloc(RENDER_NAME_TAG( "rayTracingDebug" ), desc);
#endif

			if (rctx.Test( RFF_RayTracedTranparentReflection ))
			{
				desc.m_denyUnorderedAccess = false;
				rctx.RTAlloc( RENDER_NAME_TAG( "rayTracedTransparentReflection" ), desc );

				desc = rctx.GetDepthBufferDesc();
				desc.m_format = GpuWrapApi::TEXFMT_D32F;
				rctx.RTAlloc( RENDER_NAME_TAG( "rayTracedTransparentReflectionDepthBuffer" ), desc );
			}
		}
	}
#endif


	CParticleDPLTexture::DeclareFlowResources( rctx );
}

void CRenderNode_DeclareCommonResourceAllocs_SafeMode::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo& info = rctx.GetFrameInfo();
	const auto colorDesc = rctx.GetRegularPrecisionColorTargetDesc();

	const Uint32 surfacesWidth = colorDesc.m_sizeWidth;
	const Uint32 surfacesHeight = colorDesc.m_sizeHeight;

	RED_ASSERT( info.GetRenderingMode() == RM_SafeMode, "This node is intended to declare resources for shaded mode" );

	// Some stuff is only needed if we're doing the full scene render. If we aren't, then don't even declare them, to
	// make sure they aren't accidentally created.
	if ( rctx.HasScene() )
	{
		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;

			desc.m_format = GpuWrapApi::TEXFMT_R10G10B10A2_Unorm;
			RED_FATAL_ASSERT( nullptr == info.m_gBufferAlbedo8888Result, "10-bit albedo is rendered, but 8888 format result requested" );

			desc.m_isSamplable = true;
			desc.m_denyUnorderedAccess = true;
			desc.m_sizeWidth = surfacesWidth;
			desc.m_sizeHeight = surfacesHeight;
			desc.m_maxWidth = rctx.GetMaxWidth();
			desc.m_maxHeight = rctx.GetMaxHeight();

			desc.m_extraUsageExFlags |= GpuApi::TEXUSAGEEX_EnableColorFastClear;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 0 ) );

			{
				desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
				rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer0" ), desc );
				desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			}

			// higher precision for normals
			desc.m_format = GpuWrapApi::TEXFMT_R10G10B10A2_Unorm;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 1 ) );
			rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer1" ), desc );

			// material properties (roughness,metalness,translucency,emissive)
			desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
			desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 2 ) );

			{
				desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
				rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer2" ), desc );
				desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			}

			desc.m_extraUsageExFlags &= ~GpuApi::TEXUSAGEEX_EnableColorFastClear;
			desc.SetOptimalClearValue( Vector4::ZEROS() );

			// flattened normals with best fit compression, roughness on alpha
			desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
			desc.m_denyUnorderedAccess = false;

			{
				desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;
				rctx.RTAlloc( RENDER_NAME_TAG( "normals" ), desc );
				desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
			}

			desc.m_denyUnorderedAccess = true;
		}

#if SURFACE_CACHE_ENABLE_RUNTIME_BAKING
		{
			SRenderFlowTargetDesc desc;
			desc.m_extraUsageFlags = 0;
			desc.m_extraUsageExFlags = 0;
			desc.m_format = GpuWrapApi::TEXFMT_R32_Uint;
			desc.m_isSamplable = true;
			desc.m_sizeWidth = Max( 1u, surfacesWidth / 2 );
			desc.m_sizeHeight = Max( 1u, surfacesHeight / 2 );
			desc.m_maxWidth = Max( 1u, rctx.GetMaxWidth() / 2 );
			desc.m_maxHeight = Max( 1u, rctx.GetMaxHeight() / 2 );
			rctx.RTAlloc( RENDER_NAME_TAG( "surfaceRequests" ), desc );
		}
#endif

		{
			SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();
			rctx.RTAlloc( RENDER_NAME_TAG( "color" ), desc );
		}
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_extraUsageExFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Uint;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_CAR_MAP_RES;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_CAR_MAP_RES;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepth" ), rtDesc );
		rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_WATER_MAP_RES;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_WATER_MAP_RES;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc(RENDER_NAME_TAG( "topDownRainDepth" ), rtDesc);
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_A8_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_sizeWidth = surfacesWidth;
		rtDesc.m_sizeHeight = surfacesHeight;
		rtDesc.m_maxWidth = rctx.GetMaxWidth();
		rtDesc.m_maxHeight = rctx.GetMaxHeight();
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "transparencyTXAAMask" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rctx.GetMaxWidth() / 2;
		rtDesc.m_maxHeight = rctx.GetMaxHeight() / 2;
		rtDesc.m_sizeWidth = surfacesWidth / 2;
		rtDesc.m_sizeHeight = surfacesHeight / 2;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "temporalInvalidationMask" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16G16_Snorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_sizeWidth = surfacesWidth;
		rtDesc.m_sizeHeight = surfacesHeight;
		rtDesc.m_maxWidth = rctx.GetMaxWidth();
		rtDesc.m_maxHeight = rctx.GetMaxHeight();
		rtDesc.m_denyUnorderedAccess = true;
		rtDesc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

		rctx.RTAlloc( RENDER_NAME_TAG( "distortionMask" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_optimalClearValue[0] = 0.0f;
		desc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

		rctx.RTAlloc( RENDER_NAME_TAG( "depthBuffer" ), desc );
		desc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_format = GpuWrapApi::TEXFMT_D32F;
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ), desc );
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBufferHologram" ), desc );
	}

	if ( rctx.Test( RFF_HUD ) && rctx.GetFrameInfo().m_composition != nullptr )
	{
		SRenderFlowTargetDesc compTargetDesc = rctx.GetCompositionDesc();
		compTargetDesc.m_denyUnorderedAccess = c_compositionDenyUnorderedAccess;
		compTargetDesc.m_extraUsageFlags = GpuApi::TEXUSAGE_ESRAMResident|GpuApi::TEXUSAGE_ESRAMResident_LargeLane;

		rctx.RTAlloc( RENDER_NAME_TAG( "composition" ), compTargetDesc );
	}
}

void CRenderNode_DeclareCommonResourceAllocs_FinalOnly::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	SRenderFlowTargetDesc desc = rctx.GetRegularPrecisionColorTargetDesc();
	rctx.RTAlloc( RENDER_NAME_TAG( "color" ), desc );
	
	const Uint32 surfacesWidth = desc.m_sizeWidth;
	const Uint32 surfacesHeight = desc.m_sizeHeight;

	// TODO: Get rid of that here. Allocate it in some less "common" node.
	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_extraUsageExFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Uint;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_CAR_MAP_RES;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_CAR_MAP_RES;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepth" ), rtDesc );
		rctx.RTAlloc( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = TOP_DOWN_WATER_MAP_RES;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = TOP_DOWN_WATER_MAP_RES;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc(RENDER_NAME_TAG( "topDownRainDepth" ), rtDesc);
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_A8_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = surfacesWidth;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = surfacesHeight;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "transparencyTXAAMask" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R8_Unorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = surfacesWidth / 2;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = surfacesHeight / 2;
		rtDesc.m_denyUnorderedAccess = true;

		rctx.RTAlloc( RENDER_NAME_TAG( "temporalInvalidationMask" ), rtDesc );
	}

	{
		SRenderFlowTargetDesc rtDesc;
		rtDesc.m_type = FRT_Texture;
		rtDesc.m_extraUsageFlags = 0;
		rtDesc.m_format = GpuWrapApi::TEXFMT_R16G16_Snorm;
		rtDesc.m_isSamplable = true;
		rtDesc.m_maxWidth = rtDesc.m_sizeWidth = surfacesWidth;
		rtDesc.m_maxHeight = rtDesc.m_sizeHeight = surfacesHeight;
		rtDesc.m_denyUnorderedAccess = true;
		rtDesc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

		rctx.RTAlloc( RENDER_NAME_TAG( "distortionMask" ), rtDesc );
	}

	// DrawComposition is always added to this graph.
	if ( rctx.GetFrameInfo().m_composition != nullptr )
	{
		SRenderFlowTargetDesc compTargetDesc = rctx.GetCompositionDesc();
		compTargetDesc.m_denyUnorderedAccess = c_compositionDenyUnorderedAccess;
		compTargetDesc.m_extraUsageFlags = GpuApi::TEXUSAGE_ESRAMResident|GpuApi::TEXUSAGE_ESRAMResident_LargeLane;

		rctx.RTAlloc( RENDER_NAME_TAG( "composition" ), compTargetDesc );
	}
}

void CRenderNode_DeclareCommonResourceAllocs_HitProxy::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	{
		// TODO: change it to Uint, move away from RGB
		SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R8G8B8A8_Unorm );
		rctx.RTAlloc( RENDER_NAME_TAG( "hitProxyId" ), desc );
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_format = GpuWrapApi::TEXFMT_D32FS8;
		desc.m_optimalClearValue[0] = 0.0f;
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBuffer" ), desc );
	}
	
	if ( rctx.GetFrameInfo().GetFramePurpose() == FP_CascadesBaking )
	{
		SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R8G8B8A8_Unorm );
		rctx.RTAlloc( RENDER_NAME_TAG( "color" ), desc );
	}
	else if ( rctx.GetFrameInfo().m_displaySettings.m_displayMode == EMM_DebugHitProxies || 
			  rctx.GetFrameInfo().m_displaySettings.m_displayMode == EMM_TodvisBakePreview ||
			  rctx.GetFrameInfo().m_displaySettings.m_displayMode == EMM_TodvisRuntimePreview )
	{
		SRenderFlowTargetDesc desc = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R16G16B16A16_Float );
		rctx.RTAlloc( RENDER_NAME_TAG( "color" ), desc );
	}
}

void CRenderNode_DeclareCommonResourceAllocs_GBufferOnly::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo& info = rctx.GetFrameInfo();
	const auto colorDesc = rctx.GetRegularPrecisionColorTargetDesc();

	const Uint32 surfacesWidth = colorDesc.m_sizeWidth;
	const Uint32 surfacesHeight = colorDesc.m_sizeHeight;

	RED_ASSERT( info.GetRenderingMode() == RM_GBufferOnly, "This is resource declaration node is supposed to only be used with GBuffer_only render graph" );
	RED_ASSERT( rctx.HasScene(), "Rendering GBuffer only when not having a scene makes no sense!" );

	{
		SRenderFlowTargetDesc desc;
		desc.m_extraUsageFlags = 0;
		desc.m_extraUsageExFlags = 0;
		desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
		RED_FATAL_ASSERT( nullptr == info.m_gBufferAlbedo1010102Result, "8-bit albedo is rendered, but 1010102 format result requested" );

		desc.m_isSamplable = true;
		desc.m_denyUnorderedAccess = true;
		desc.m_sizeWidth = surfacesWidth;
		desc.m_sizeHeight = surfacesHeight;
		desc.m_maxWidth = rctx.GetMaxWidth();
		desc.m_maxHeight = rctx.GetMaxHeight();

		desc.m_extraUsageExFlags |= GpuApi::TEXUSAGEEX_EnableColorFastClear;

		desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 0 ) );
		rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer0" ), desc );

		// higher precision for normals
		desc.m_format = GpuWrapApi::TEXFMT_R10G10B10A2_Unorm;
		desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 1 ) );
		rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer1" ), desc );

		// material properties (roughness,metalness,translucency,emissive)
		desc.m_format = GpuWrapApi::TEXFMT_R8G8B8A8_Unorm;
		desc.SetOptimalClearValue( GetGBufferDefaultClearColor( 2 ) );
		rctx.RTAlloc( RENDER_NAME_TAG( "gbuffer2" ), desc );
	}

	{
		SRenderFlowTargetDesc desc = rctx.GetDepthBufferDesc();
		desc.m_optimalClearValue[0] = 0.0f;
		rctx.RTAlloc( RENDER_NAME_TAG( "depthBuffer" ), desc );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_DoCulling::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		#ifndef RED_CONFIGURATION_FINAL
			auto devConfig = GRender->GetDevConfig();
			const bool devConfigRenderSceneLayerOverride = (devConfig && devConfig->SceneLayers.IsEnabled());
		#else
			const bool devConfigRenderSceneLayerOverride = false;
		#endif

		//	if ( devConfig )
		//		if ( devConfig->SceneLayers.IsEnabled() )
		//			devConfigRenderSceneLayerOverride = true;
		//	if ( rctx.GetCameraData() && (rctx.GetCameraData()->GetInfo().m_sceneLayer != RenderSceneLayer::Default) && !devConfigRenderSceneLayerOverride )
		//#else
		//	if ( rctx.GetCameraData() && rctx.GetCameraData()->GetInfo().m_sceneLayer != RenderSceneLayer::Default )
		//#endif
		if ( rctx.GetCameraData() && (rctx.GetCameraData()->GetInfo().m_sceneLayer != RenderSceneLayer::Default) && !devConfigRenderSceneLayerOverride )
		{
			if ( m_cullMainScene )
			{
				builder->DispatchJob( "DoCull_SceneLayer", [rctx]( const job::RunContext& context )
				{
					Bool skipVisibleInDefaultLayer = false; // not rendering main layer, so don't skip those
					rctx.GetScene()->CollectNonDefaultSceneLayer( rctx, *rctx.GetCollector(), rctx.GetCameraData()->GetInfo().m_sceneLayer, skipVisibleInDefaultLayer );
				} );
			}
		}
		else
		{
			Bool needExplicitFence = false;

#ifndef RED_CONFIGURATION_FINAL
			Bool cullMainLayer = true;
			if ( rctx.GetCameraData() && rctx.GetCameraData()->IsOnscreen() )
			{
				if ( devConfigRenderSceneLayerOverride )
				{
					cullMainLayer = devConfig->SceneLayers.m_renderInMainView[Uint32(RenderSceneLayer::Default)];
					if ( m_cullMainScene )
					{
						needExplicitFence = true;
						builder->DispatchJob< job::Fence::None >( "DoCull_SceneLayer", [rctx, cullMainLayer]( const job::RunContext& context )
						{
							auto devConfig = GRender->GetDevConfig();
							RED_ASSERT(devConfig != nullptr, "How is devConfig null now, when it wasn't before?");

							static_assert(Uint32( RenderSceneLayer::Default ) == 0, "Expect Default scene layer to be 0.");
							for ( Uint32 i = 1; i < RenderSceneLayer_MAX; ++i )
							{
								if ( devConfig->SceneLayers.m_renderInMainView[i] )
								{
									Bool skipVisibleInDefaultLayer = cullMainLayer; // if rendering main layer, don't draw proxies twice
									rctx.GetScene()->CollectNonDefaultSceneLayer( rctx, *rctx.GetCollector(), RenderSceneLayer(i), skipVisibleInDefaultLayer );
								}
							}
						} );
					}
				}
			}
#else
			constexpr Bool cullMainLayer = true;
#endif

			// Parallel
			if ( cullMainLayer )
			{
				if ( m_cullCascades )
				{
					needExplicitFence = true;

					builder->DispatchJob< job::Fence::None >( "DoCull_Cascades", [rctx]( const job::RunContext& context )
					{
						job::Builder depBuilder { context };
						rctx.GetCollector()->DoCull_Cascades( rctx, rctx.GetVisibilityView(), depBuilder );
					} );
				}

				if ( m_cullMainScene )
				{
					needExplicitFence = true;

					builder->DispatchJob< job::Fence::None >( "DoCull_MainScene", [rctx]( const job::RunContext& context )
					{
						job::Builder depBuilder { context };
						rctx.GetCollector()->DoCull_MainScene( rctx, depBuilder );
					} );
				}

				if ( m_cullDynamicShadows )
				{
					if ( rctx.Test( RFF_LocalShadows ) )
					{
						needExplicitFence = true;

						builder->DispatchJob< job::Fence::None >( "DoCull_LocalShadowMaps", [rctx]( const job::RunContext& context )
						{
							job::Builder depBuilder { context };
							rctx.GetCollector()->DoCull_LocalShadowMaps( rctx, rctx.GetVisibilityView(), depBuilder );
						} );
					}
				}
#ifdef USE_RAY_TRACING
				if ( m_cullRayTracedObjects )
				{
					needExplicitFence = true;

					builder->DispatchJob< job::Fence::None >( "DoCull_RayTracedObjects", [rctx]( const job::RunContext& context )
					{
						job::Builder depBuilder { context };
						rctx.GetCollector()->DoCull_RayTracedObjects( rctx, rctx.GetVisibilityView(), depBuilder );
					} );
				}
#endif
				if ( m_cullQueryOnlyBoxes )
				{
					needExplicitFence = true;

					// #NOTE: See similar assert below
					RED_ASSERT( m_cullMainScene, "We're assuming all scene culling happens together. See code comment here for more details" );

					builder->DispatchJob< job::Fence::None >( "DoCull_QueryOnlyBoxes", [rctx]( const job::RunContext& context )
					{
						job::Builder depBuilder { context };
						rctx.GetCollector()->DoCull_QueryOnlyBoxes( rctx, depBuilder );
					} );
				}
			}

			if ( needExplicitFence )
			{
				builder->DispatchFenceExplicitly();
			}

#ifdef USE_PROFILER
			if ( m_cullMainScene )
			{
				builder->DispatchJob( "DoCull_CollectRenderingStats", [rctx]( const job::RunContext& context )
				{
					//Currently Occlusion buffer tests are done only in QuerySingleFrustum
					auto sceneQuery = red::StaticCast<vis::QueryBase>( rctx.GetCollector()->m_mainCameraQuery );
					if ( sceneQuery == nullptr )
					{
						return;
					}

					// accumulate rendering stats
					{
						VisibilitySystemStats stats;
						for ( VisibilitySystemStats& s : sceneQuery->m_visibilitySystemStats )
						{
							stats.Append( s );

							// HACK reset the stats so they are not doubled in one frame
							s = VisibilitySystemStats();
						}
						RED_SCOPE_LOCK( rctx.GetCollector()->m_renderingStatsLock );
						rctx.GetCollector()->m_renderingStats.m_visibilitySystemStats.Append( stats );
					}
				} );
			}
#endif
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_UpdateGlobalBindings::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	GetRenderer()->UpdateGlobalBindings( rctx );
}

static constexpr Uint32 fwdStart = rend::Binding::Textures::GLOBAL_FWD_BEGIN;
static constexpr Uint32 fwdCount = (rend::Binding::Textures::GLOBAL_FWD_END - rend::Binding::Textures::GLOBAL_FWD_BEGIN);

void CRenderNode_BindGlobalConstants::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const auto shaderTarget = m_shaderTypeMask;

	const auto& info = rctx.GetFrameInfo();
	const Bool isGBufferOnly = info.GetRenderingMode() == ERenderingMode::RM_GBufferOnly;
	const Bool isHitProxy = ( info.GetRenderingMode() == ERenderingMode::RM_HitProxies || info.m_displaySettings.m_displayMode == EMM_DebugHitProxies );

	if ( rctx.IsConsumePhase() )
	{
		GetRenderer()->GetStateManager().Reset();

		GpuApi::UnlockTexturesSlots( fwdStart, fwdCount );
	}

	if ( !isGBufferOnly && !isHitProxy )
	{
		rctx.RTUseBegin<RFUF_Read>( RENDER_NAME_TAG( "topDownCarProxiesDepth" ) );
		rctx.RTUseBegin<RFUF_Read>( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ) );
		rctx.RTUseBegin<RFUF_Read>( RENDER_NAME_TAG( "topDownRainDepth" ) );
	}

	GetRenderer()->BindGlobalSamplers( rctx, shaderTarget );

	if( rctx.IsConsumePhase() )
	{
		// BindSharedConstants
		GetRenderer()->BindSharedConstants( shaderTarget );
		// BindForwardConstBuffer
		GetRenderer()->BindComputeConstants( shaderTarget );

		RenderBindManager::BindGlobalConstants( rctx, shaderTarget );

		const GpuApi::TextureRef windImpulseTex = rctx.GetCameraStorage()->GetCustomData<CWindImpulseCollector>().GetImpulseVolumeTex();
		GpuApi::TextureRef rainMapTexture = rctx.GetCameraStorage()->GetCustomData<SceneCustomData_RainMap >().GetPersistentRainMap();
		const GpuApi::TextureRef terrainHeightmapTexArray = rctx.GetCameraStorage()->GetCustomData<CRenderTerrainCellCollector>().GetPrevTerrainHeightMap().m_terrainHeightMapTexArr;
		GpuApi::BindTexture( rend::Binding::Textures::WIND_IMPULSE, windImpulseTex, shaderTarget );
		GpuApi::BindTexture( rend::Binding::Textures::RAIN_MAP, rainMapTexture, shaderTarget );
		GpuApi::BindTexture( rend::Binding::Textures::TERRAIN_HEIGHTMAP, terrainHeightmapTexArray, shaderTarget );

		if ( !isGBufferOnly && !isHitProxy )
		{
			const GpuApi::TextureRef topDownCarProxyTex = rctx.RTGet<GpuApi::TextureRef>( RENDER_NAME_TAG( "topDownCarProxiesDepth" ) );
			const GpuApi::TextureRef topDownCarProxyBackFaceTex = rctx.RTGet<GpuApi::TextureRef>( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ) );
			const GpuApi::TextureRef topDownRainDepthTex = rctx.RTGet<GpuApi::TextureRef>( RENDER_NAME_TAG( "topDownRainDepth" ) );
			GpuApi::BindTexture( rend::Binding::Textures::TOP_DOWN_CAR_MAP, topDownCarProxyTex, shaderTarget );
			GpuApi::BindTexture( rend::Binding::Textures::TOP_DOWN_CAR_BACK_FACE_MAP, topDownCarProxyBackFaceTex, shaderTarget );
			GpuApi::BindTexture( rend::Binding::Textures::TOP_DOWN_RAIN_DEPTH, topDownRainDepthTex, shaderTarget );
		}

		GpuApi::TextureRef gradientTexture = GetRenderer()->GetGradientManager()->GetTexture();
		GpuApi::BindTexture( rend::Binding::Textures::GRADIENT_MAP, gradientTexture, shaderTarget );

		const GpuApi::BufferRef interiorMapBuffer = rctx.GetCameraStorage()->GetCustomData<InteriorMapDataUpdater>().m_interiorMapData;
		GpuApi::BindBufferSRV( interiorMapBuffer, rend::Binding::Textures::INTERIOR_MAP, shaderTarget );

		if ( auto blueNoiseTexRes = GetRenderer()->GetEngineHandler()->GetBlueNoiseTexArray() )
		{
			auto blueNoiseTexRef = static_cast<CRenderTextureArray*>( blueNoiseTexRes.Get() )->GetTextureRef();
			GpuApi::BindTexture( rend::Binding::Textures::BLUE_NOISE_ARRAY, blueNoiseTexRef, shaderTarget );
		}
		else
		{
			auto blueNoiseTexRef = GpuApi::TextureRef::Null();
			GpuApi::BindTexture( rend::Binding::Textures::BLUE_NOISE_ARRAY, gradientTexture, shaderTarget );
		}

		const auto& rainSettings = rctx.GetFrameInfo().m_worldRenderSettings.m_rain;

		if (rainSettings.m_raindropsMask)
		{
			CRenderTextureBase* raindropsMask = static_cast< CRenderTextureBase* >( rainSettings.m_raindropsMask.Get() );
			GpuApi::TextureRef raindropsMaskTexRef = raindropsMask->GetTextureRef();
			GpuApi::BindTexture( rend::Binding::Textures::RAIN_DROPLETS, raindropsMaskTexRef, shaderTarget );
		}
		if (rainSettings.m_rainleaksMask)
		{
			CRenderTextureBase* rainleaksMask = static_cast< CRenderTextureBase* >( rainSettings.m_rainleaksMask.Get() );
			GpuApi::TextureRef rainleaksMaskTexRef = rainleaksMask->GetTextureRef();
			GpuApi::BindTexture( rend::Binding::Textures::RAIN_LEAKS, rainleaksMaskTexRef, shaderTarget );
		}
		if (rainSettings.m_rainRipplesMask)
		{
			CRenderTextureBase* rainripplesNormal = static_cast< CRenderTextureBase* >( rainSettings.m_rainRipplesMask.Get() );
			GpuApi::TextureRef rainripplesNormalTexRef = rainripplesNormal->GetTextureRef();
			GpuApi::BindTexture( rend::Binding::Textures::RAIN_RIPPLES, rainripplesNormalTexRef, shaderTarget );
		}

		GpuApi::LockTexturesSlots( fwdStart, fwdCount );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_UnbindGlobalConstants::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const auto& info = rctx.GetFrameInfo();
	const Bool isGBufferOnly = info.GetRenderingMode() == ERenderingMode::RM_GBufferOnly;
	const Bool isHitProxy = ( info.GetRenderingMode() == ERenderingMode::RM_HitProxies || info.m_displaySettings.m_displayMode == EMM_DebugHitProxies );

	if ( !isGBufferOnly && !isHitProxy )
	{
		rctx.RTUseEnd( RENDER_NAME_TAG( "topDownCarProxiesDepth" ) );
		rctx.RTUseEnd( RENDER_NAME_TAG( "topDownCarProxiesDepthBackFace" ) );
		rctx.RTUseEnd( RENDER_NAME_TAG( "topDownRainDepth" ) );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_BindLightingGlobalConstants::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const auto& info = rctx.GetFrameInfo();

	// Some stuff is only needed if we're doing the full scene render. If we aren't, then don't even declare them, to
	// make sure they aren't accidentally created.
	if ( info.GetRenderingMode() <= RM_ShadedMax || info.GetRenderingMode() == RM_SafeMode )
	{
		if ( rctx.IsConsumePhase() )
		{
			GpuApi::UnlockTexturesSlots( fwdStart, fwdCount );
		}

		GetRenderer()->BindForwardShaderGlobals( rctx, m_shaderTypeMask, m_bindDeferredLights, m_bindLightChannels );

		if ( rctx.IsConsumePhase() )
		{
			GpuApi::LockTexturesSlots( fwdStart, fwdCount );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_UnbindLightingGlobalConstants::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const auto& info = rctx.GetFrameInfo();
	if ( info.GetRenderingMode() <= RM_ShadedMax || info.GetRenderingMode() == RM_SafeMode )
	{
		// Bind global constants
		if ( rctx.IsConsumePhase() )
		{
			GpuApi::UnlockTexturesSlots( fwdStart, fwdCount );
		}

		GetRenderer()->UnbindForwardShaderGlobals( rctx, m_shaderTypeMask, m_unbindDeferredLights, m_unbindLightChannels );

		if ( rctx.IsConsumePhase() )
		{
			GpuApi::LockTexturesSlots( fwdStart, fwdCount );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_BindEnvProbes::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		auto& dt = rctx.GetCameraData()->GetCustomData<CameraCustomData_ReflectionProbes>();
		GpuApi::BindConstantBuffer( rend::Binding::Constants::CUSTOM, dt.GetProbesData(), GpuWrapApi::ShaderTypeMask(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader ) );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_UnbindEnvProbes::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		GpuApi::BindConstantBuffer( rend::Binding::Constants::CUSTOM, GpuApi::BufferRef::Null(), GpuWrapApi::ShaderTypeMask(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader));
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_PrepareDummyLightingData::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	GetRenderer()->GetClustered()->PrepareDummyData( rctx );
	GetRenderer()->GetReflectionProbeManager()->PrepareDummyData( rctx );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_SetCameraMain::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		CRenderCamera renderCamera = rctx.GetRenderCamera( m_renderingPlane );
		if ( !m_allowTemporalJitter && ( 0 != renderCamera.GetSubpixelOffsetX() || 0 != renderCamera.GetSubpixelOffsetY() ) )
		{
			// We don't want the jittering currently, switch it off.
			renderCamera.SetSubpixelOffset( 0, 0, 1, 1 );
		}
		if( m_cameraSpace )
		{
			renderCamera.Set( WorldPosition::ZEROS(), Quaternion::IDENTITY() );
		}

		RenderBindManager::SetCamera(rctx, renderCamera, rctx.GetWidth(), rctx.GetHeight(), &rctx.GetCameraData()->GetLastFrameData( m_renderingPlane ) );

		if ( m_renderingPlane == RPl_Weapon )
		{
			rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight(), 0, 0, WEAPON_PLANE_MIN_DEPTH, WEAPON_PLANE_MAX_DEPTH ) );
		}
		else
		{ 
			rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight(), 0, 0 ) );
		}
	}	
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_SetCamera2D::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		RenderBindManager::SetCamera2D(rctx, rctx.GetWidth(), rctx.GetHeight() );
	}
}


void CRenderNode_CameraResourceDependencyScope::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameCameraStorage::SCameraData *cameraData = rctx.GetCameraData();
	RED_FATAL_ASSERT( cameraData, "No camera data - are we in unique node?" );

	// Iterate over parent cameras
	for( Uint32 dep_i=0; dep_i < cameraData->m_numSelectedDependencies; ++dep_i )
	{
		// Get parent name
		const Uint64 parentCameraName = cameraData->m_selectedDependency[dep_i];
		RED_FATAL_ASSERT( parentCameraName != cameraData->m_cameraHash, "" );

		// Get parent renderFlowSpace
		const Int32 parentCameraIndex = rctx.GetCameraStorage()->FindCameraData( parentCameraName );
		RED_FATAL_ASSERT( -1 != parentCameraIndex, "Parent camera name not found" );
		const Int32 parentRenderFlowSpace = rctx.GetCameraStorage()->GetCameraData( parentCameraIndex )->m_renderFlowSpace;
		RED_FATAL_ASSERT( -1 != parentRenderFlowSpace, "Invalid parent renderFlowSpace" );

		const Uint8 depFlags = cameraData->m_selectedDependencyFlags[dep_i];

		// Build nameTag for parent camera rendering result
		const RenderFlowNameTag rtParentColor( RENDER_NAME_TAG( "color" ), (TRenderFlowSpace)parentRenderFlowSpace );

		// Process dependency scope
		if( m_isScopeOpen )
		{
			if( depFlags & CRenderFrameCameraStorage::CDF_Output_Color )
			{
				rctx.RTUseBegin( rtParentColor, RFUF_Read );
			}
		}
		else
		{
			if( depFlags & CRenderFrameCameraStorage::CDF_Output_Color )
			{
				rctx.RTUseEnd( rtParentColor );
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_FlushTextureGrabs::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );

	if ( rctx.IsConsumePhase() )
	{
		const CRenderFrameInfo& info = rctx.GetFrameInfo();

		// Following conditions recreate scenarios from extractor nodes that lead to use GrabTexturePixels in immediate mode.
		const auto framePurpose = rctx.GetFrameInfo().GetFramePurpose();
		const Bool immediate = framePurpose != FP_GlobalIllumination && framePurpose != FP_GlobalIlluminationBackfaces && framePurpose != FP_DeferredScreenshot;
		const Bool colorExtraction = info.m_colorResultHDR || info.m_colorResult;
		const Bool depthExtractionImmediate = info.m_depthResult && immediate;
		const Bool somethingToSelect = !info.m_selectionCaptureData->Empty();
		const Bool background = static_cast< Bool >( info.m_backgroundDump );
		const Bool gBufferExtractionAtLeastOneLayerImmediate =
		(
				info.m_gBufferAlbedo8888Result || info.m_gBufferAlbedo1010102Result || info.m_gBufferMaterialResult 
		) && immediate;

		if ( somethingToSelect || gBufferExtractionAtLeastOneLayerImmediate || colorExtraction || depthExtractionImmediate || background )
		{
			if ( immediate )
			{
				GpuApi::FlushTextureGrabs();
			}
			else
			{
				GpuApi::TryFinalizeTextureGrabs();
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_FlushBufferGrabs::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	ScopedProfilerChannel forcedChannels( PBC_RENDER );
	PC_SCOPE( CRenderNode_FlushBufferGrabs );

	if ( rctx.IsConsumePhase() )
	{
		const CRenderFrameInfo& info = rctx.GetFrameInfo();

		GpuApi::FlushBufferGrabs();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef RED_ASSERTS_ENABLED
extern red::Atomic< Bool > GDebugRenderFrame;
#endif

void CRenderNode_CleanupBatchDataAllocator::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		PC_SCOPE( BatchDataCleanup );
		GetRenderer()->GetBatchDataAllocator().Cleanup();
	}
}

void CRenderNode_EndFrame::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
#ifdef USE_PROFILER
		if ( IRenderGpuProfiler* gpuProfiler = GetRenderer()->GetGpuProfiler() )
		{
		}
#endif	

		rctx.GetResourceAllocator()->SetPhase( RFP_Cleanup );

		{
			PC_SCOPE( SceneOnEndFrame );
			if ( IRenderFrame* frame = rctx.GetFrame() )
			{
				if ( CRenderSceneEx* renderScene = static_cast<CRenderSceneEx*>(frame->GetFrameInfo().GetScene()) )
				{
					renderScene->OnEndFrame();
				}
			}
		}

		RED_FATAL_ASSERT( nullptr != CRenderNodeJob::GetJobsRenderFrame(), "Expected frame to be there" );
		CRenderNodeJob::SetJobsRenderFrame( nullptr );

		RED_FATAL_ASSERT( GDebugRenderFrame.Exchange( false ) == true, "Wasn't rendering a frame?!" );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_BuildDepthChain::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	GetRenderer()->GetHiZ()->BuildDepthChain( rctx );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ClusteredLightsCull::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	auto cluster	= GetRenderer()->GetClustered();
	auto distant	= GetRenderer()->GetDistantLightBatcher();
	auto hiz		= GetRenderer()->GetHiZ();

	cluster->FillLights( rctx );

	distant->CullLights( rctx );	// distant lights can append to clustered lights list

	cluster->CullLights( rctx, hiz );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_VolumetricFog::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( !rctx.Test( RFF_VolumetricFog ) )
	{
		return;
	}

	CRenderVolumetricFogManager* fogMgr = GetRenderer()->GetVolumetricFogManager();

	fogMgr->Render( rctx );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_GlobalIllumination::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.GetCameraData()->IsOffscreen() )
		return;

	if( auto giMgr = GetRenderer()->GetGIManager() )
	{
		if( Config::GGlobalRenderingSettings.globalIllumination.ultraOn != giMgr->GetIsUltra() )
		{
			giMgr->Reset(); // Only when the quality is changed
			giMgr->SetIsUltra( Config::GGlobalRenderingSettings.globalIllumination.ultraOn );
		}

		if( rctx.Test( RFF_GlobalIlluminationCompute ) )
		{
			const auto& currentCam = rctx.GetRenderCamera();

			Vector4 camPos = currentCam.GetPosition().AsVector4();
			giMgr->Tick( rctx, &camPos, &currentCam.GetCameraForward(), 1 );
		}
	}

	if( auto dsMgr = GetRenderer()->GetDistantGIManager() )
	{
		dsMgr->Relit( rctx );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderSkyScattering::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	PC_SCOPE( CRenderNode_RenderSkyScattering );

	if( !rctx.Test( RFF_ReflectionProbes ) || rctx.GetCameraData()->IsOffscreen() )
		return;

	if( auto envMgr = GetRenderer()->GetReflectionProbeManager() )
	{
		envMgr->RenderSkyScattering( rctx );
	}
}

void CRenderNode_ReflectionProbes::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( auto envMgr = GetRenderer()->GetReflectionProbeManager() )
	{
		if( !rctx.Test( RFF_ReflectionProbes ) )
			return;

		envMgr->Tick( rctx );

		envMgr->CullProbes( rctx );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_SpawnDynamicDecals::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		if ( CRenderSceneEx* scene = rctx.GetScene() )
		{
			scene->SpawnQueuedDynamicDecals( rctx );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderElements::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		if ( m_renderConditionFunc && !(*m_renderConditionFunc)(rctx) )
		{
			return;
		}

		const CRenderCollector& collector = *rctx.GetCollector();
		const RenderStageCollector& stageCollector = collector.m_renderStageCollector;
		RenderBindManager& stateManager = GetRenderer()->GetStateManager();

		Int32 renderWidth = rctx.GetWidth();
		Int32 renderHeight = rctx.GetHeight();

#ifdef USE_DLSS
		if ( rctx.Test( RFF_DLSS ) )
		{
			const GpuApi::TextureRef colorTexture = rctx.RTGet<GpuApi::TextureRef>( RENDER_NAME_TAG( "color" ) );
			auto textureDesc = GpuApi::GetTextureDesc( colorTexture );
			renderWidth = (Int32)textureDesc.width;
			renderHeight = (Int32)textureDesc.height;
		}
#endif // USE_DLSS

		for ( Uint32 i = 0; i < m_batches.Size(); ++i )
		{
			const RenderElementsBatch& batch = m_batches[ i ];
			const RenderStageCollectorBucket& bucket = stageCollector.GetBucket( batch.m_renderStageId, batch.m_renderStageCluster );

			if ( !bucket.HasChunks() )
			{
				// no chunks - early exit
				continue;
			}

			GeometryBatcherStats localStats;
			CRenderGeometryBatcher::RenderContext renderCtx( rctx, localStats );
			renderCtx.renderStageId = batch.m_renderStageId;

			//
			// Set up camera
			// #TODO: Don't change it if it's the same as the previous batch
			//
			if ( batch.m_flags & (REBF_CAM_Main|REBF_CAM_CameraSpace) )
			{
				ERenderingPlane renderingPlane;
				if ( batch.m_flags & REBF_CAM_WeaponPlane )
				{
					if ( !rctx.Test(rend::DF_RenderMask_WeaponPlane) )
					{
						continue;
					}
					renderingPlane = RPl_Weapon;
				}
				else if ( batch.m_flags & REBF_CAM_ScenePlane )
				{
					renderingPlane = RPl_Scene;
				}
				else
				{
					// #TODO: Use default? or require it to be given?
					renderingPlane = RPl_Scene;
				}

				CRenderCamera renderCamera = rctx.GetRenderCamera( renderingPlane );

				Bool noJitter = (batch.m_flags & REBF_CAM_DisableJitter) != 0;
				if ( noJitter && ( 0 != renderCamera.GetSubpixelOffsetX() || 0 != renderCamera.GetSubpixelOffsetY() ) )
				{
					renderCamera.SetSubpixelOffset( 0, 0, 1, 1 );
				}

				if ( batch.m_flags & REBF_CAM_CameraSpace )
				{
					renderCamera.Set( WorldPosition::ZEROS(), Quaternion::IDENTITY() );
				}

				stateManager.SetCamera(rctx, renderCamera, renderWidth, renderHeight, &rctx.GetCameraData()->GetLastFrameData( renderingPlane ) );
				//For transparents on the weapon plane we do not want to squeeze depth because depth test would always pass and transparent objects would render in front of anything else.
				//That's because the squeezed depth is resolved after GBuffer pass into unsqueezed one and we must output unsquezzed depths to test against the weapon corrected depth buffer.
				if ( renderingPlane == RPl_Weapon && ( batch.m_flags & REBF_CAM_DisableSqueezedDepthInWeaponPlane ) == 0 )
				{
					rctx.SetViewport( GpuApi::ViewportDesc( renderWidth, renderHeight, 0, 0, WEAPON_PLANE_MIN_DEPTH, WEAPON_PLANE_MAX_DEPTH ) );
				}
				else
				{ 

					rctx.SetViewport( GpuApi::ViewportDesc( renderWidth, renderHeight, 0, 0 ) );
				}
			}
			else if ( batch.m_flags & REBF_CAM_2D )
			{
				RenderBindManager::SetCamera2D(rctx, renderWidth, renderHeight );
				rctx.SetViewport( GpuApi::ViewportDesc( renderWidth, renderHeight, 0, 0 ) );
			}
			else
			{
				RED_HALT("No camera specified for RenderElementsBatch");
			}

			//
			// Render the batch
			//

			renderCtx.sortingMode = batch.m_sortingMode;

			if ( renderCtx.sortingMode == SM_ByPriority )
			{
				renderCtx.allowInstancesSplit = true;
			}

#ifdef USE_PROFILER
			GpuApi::BeginProfilerBlock( GetRenderer()->GetRenderStageRegistry().GetRegisteredStageName( batch.m_renderStageId ).Data() );
#endif // USE_PROFILER

			GetRenderer()->GetGeometryBatcher()->RenderGeometry( renderCtx, bucket );

#ifdef USE_PROFILER
			GpuApi::EndProfilerBlock();
#endif // USE_PROFILER

#ifndef RED_CONFIGURATION_FINAL
			if ( ( batch.m_flags & REBF_DEBUG_NoStats ) == 0 )
			{
				RED_SCOPE_LOCK( collector.m_renderingStatsLock );
				collector.m_renderingStats.m_batcherStats.Append( localStats );
			}
#endif
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ComputeSkinningAndTangentUpdates::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		if ( !rctx.Test( RFF_RuntimeTangentUpdate ) )
		{
			return;
		}

		const CRenderCollector&	collector				= *rctx.GetCollector();
		const RenderStageCollector&	stageCollector		= collector.m_renderStageCollector;
		const CBatchDataAllocator& batchDataAllocator	= GetRenderer()->GetBatchDataAllocator();
		const Int32 frameIndex							= rctx.GetScene()->GetLastAllocatedFrame();

		Int32 maxIndex = stageCollector.GetTangentUpdateChunksCount();
		for ( Int32 i = 0; i < maxIndex; ++i )
		{
			const SRenderChunk& chunk = stageCollector.GetTangentUpdateChunk( i );

#if ENABLE_BATCHDATA_ALLOCATOR_OVERFLOW_CHECKS
			if ( !chunk.m_key.GetGeometry() || !chunk.m_data.GetProxy() )
			{
				continue;
			}
#endif // ENABLE_BATCHDATA_ALLOCATOR_OVERFLOW_CHECKS

			const SProxyBatchData& proxyBatchData = batchDataAllocator.GetProxyData().GetData( chunk.m_data.GetProxy() );
			if ( !proxyBatchData.m_gpuComputeSkinningBuffers )
			{
				continue;
			}
			const SGeometryBatchData& geoBatchData = batchDataAllocator.GetGeometryData().GetData( chunk.m_key.GetGeometry() );
			RED_ASSERT( geoBatchData.IsMesh() );

			if ( !proxyBatchData.m_gpuComputeSkinningBuffers->CheckAndUpdateSkinningComputeFrameIndex( geoBatchData.m_meshData.m_chunkIndex, frameIndex ) )
			{
				continue;
			}

			const CRenderMesh* mesh = geoBatchData.m_meshData.m_mesh;
			RED_ASSERT( mesh );
			RED_ASSERT( geoBatchData.m_meshData.m_hasTopologyDataForChunk );
			RED_ASSERT( proxyBatchData.m_skinningData );

            const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( proxyBatchData.GetProxy() );

            CRenderMesh::GpuComputeSkinningInput skinningInput;
            skinningInput.skinningGpuBuffers = proxyBatchData.m_gpuComputeSkinningBuffers;
            skinningInput.skinningData = proxyBatchData.m_skinningData;
            if( meshProxy->HasDismembermentData() )
            {
                skinningInput.dismembermentData = &meshProxy->GetDismembermentData();
            }
            skinningInput.runtimeQuantization = TangentUpdateUtils::CalculateMeshScaleAndBias_FromProxy( proxyBatchData.GetProxy(), geoBatchData.m_meshData.m_mesh, geoBatchData.m_meshData.m_chunkIndex );

			mesh->SkinAndUpdateTangents( rctx, geoBatchData.m_meshData.m_chunkIndex, skinningInput );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ClearFinalTarget::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	auto rtFinal = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );
	auto rtDepth = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "depthBuffer" ) );

	if( rctx.IsConsumePhase() )
	{
		const CRenderCameraFrameInfo& cameraInfo = rctx.GetCameraData()->m_info;

		// Final target needs to be premultiplied, in order to get proper blending.
		Vector4 clearColor = cameraInfo.m_clearColor.ToVector();
		Float alpha = clearColor.W;
		clearColor *= alpha;
		clearColor.W = alpha;

		GpuApi::ClearColorTarget( rtFinal, clearColor );
		GpuApi::ClearDepthStencilTarget( rtDepth, rctx.GetRenderCamera().GetDepthBufferClearValue(), 0 );
	}
}

//////////////////////////////////////////////////////////////////////////

void CRenderNode_GameplayPostFX::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.Test( rend::DF_RenderMask_GameplayPostProcess ) && rctx.GetCameraData()->IsOnscreen() )
	{
		GetRenderer()->GetPostProcess()->ApplyGameplayEffects( m_orderOfEffectsToExecute, rctx );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderBackground::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo &info = rctx.GetFrameInfo();

	if ( rctx.IsCameraDataAvailable() && rctx.GetCameraInfo().m_overrideClearColor )
	{
		const auto rtColor = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );
		if( rctx.IsConsumePhase() )
		{
			GpuApi::ClearColorTarget( rtColor, rctx.GetCameraInfo().m_clearColor.ToVector() );
		}
	}
	else
	{
		PC_SCOPE( RenderSkybox );

		const auto rtColor = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "color" ) );
		const auto rtDepth = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "depthBuffer" ) );

		if ( rctx.IsConsumePhase() )
		{
			// Setup targets
			rctx.BindPSO( ColorTarget( 0, rtColor ), DepthTarget( rtDepth.Get<GpuApi::TextureRef>() ) );
			rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );
		}

		RenderSkyUtils::Render( rctx, true );
	}
}


void CRenderNode_SaveDumpedBackground::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	const CRenderFrameInfo &info = rctx.GetFrameInfo();

	if( info.m_backgroundDump )
	{
		RenderFlowNameTag latLongSky = rctx.RTShared<RFUF_Read>( RENDER_NAME_TAG( "latLongSky" ) );

		if ( rctx.IsConsumePhase() )
		{
			GpuApi::TextureRef textureRef = rctx.RTGet<GpuApi::TextureRef>( latLongSky );
			GpuApi::TextureDesc textureDesc = GpuApi::GetTextureDesc( textureRef );

			info.m_backgroundDump->ReserveMemory( textureDesc.width, textureDesc.height );

			GpuApi::GrabTexturePixels( textureRef, 0, 0, textureDesc.width, textureDesc.height, info.m_backgroundDump->GetWriteData(), true );
		}
	}
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_Selection::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
#ifndef NO_EDITOR
	if( rctx.Test( RFF_Selection ) )
	{
		RenderSelection::Render( rctx );
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_PrepareCollector::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if( rctx.IsConsumePhase() )
	{
		RED_ASSERT( builder, "Job builder expected" );

		CRenderCollector &collector = *rctx.GetCollectorUnitialized();

		const CRenderFrameInfo &frameInfo = rctx.GetFrameInfo();
		CRenderSceneEx *scene = rctx.GetScene();

		// Extract collector data from camera data
		auto& collectorData   = rctx.GetCameraData()->GetCollectorData();

		// Collect things to render
		collector.Setup( frameInfo, m_baseFeatureFlag, rctx.GetRenderCamera(), scene, &collectorData, &rctx.GetCameraInfo() );

		// Prepare scene culling query
		builder->DispatchJob( "DoCull_PrepareSceneQuery", [rctx, collector = &collector] ( const job::RunContext& context )
		{
			job::Builder depBuilder{ context };
			collector->DoCull_PrepareSceneQuery( rctx, depBuilder );
		} );

		// Calculate cascades data
		collector.CalculateCascadesData( rctx.GetRenderCamera(), &rctx.GetCameraData()->m_cascadesStabilizer );

		RED_ASSERT( !rctx.GetCameraData()->IsAccesible( CRenderFrameCameraStorage::DAF_Collector ) );
		rctx.GetCameraData()->SetAccesible( CRenderFrameCameraStorage::DAF_Collector );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderFinal2D::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	GetRenderer()->RenderFinal2D( rctx, m_baseFeatureFlag );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_FullscreenVideo::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	GetRenderer()->RenderFullscreenVideo( rctx );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_SurfaceCacheAddGpuRequests::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
#if SURFACE_CACHE_ENABLE_RUNTIME_BAKING
	auto rtSurfaceRequests = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "surfaceRequests" ) );
	if( rctx.IsConsumePhase() )
	{
		GetRenderer()->GetSurfaceCache().AddGPUSurfaceRequests( rctx, rtSurfaceRequests );
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_GenerateAsyncDynamicTextures::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		if ( GetRenderer()->GetDynamicTextureManager() )
		{
			const Bool isOffscreen = !rctx.GetCameraData() || rctx.GetCameraData()->IsOffscreen();
			GetRenderer()->GetDynamicTextureManager()->GenerateAsync( isOffscreen );
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_UpdateGlyphCacheTexture::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		GetRenderer()->GetGlyphCache()->UpdateTexture();
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_RenderElementsTodvis::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		rctx.GetScene()->CollectTodvisGlobalShadowCastersAndDraw( rctx, *rctx.GetCollector() );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_AdvanceSpeedTreeWind::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( rctx.IsConsumePhase() )
	{
		const CRenderFrameCameraStorage* cameraStorage = rctx.GetCameraStorage();
		cameraStorage->GetCustomData<CSpeedTreeWindDataUpdater>().Update( rctx, *cameraStorage );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class SimpleTemporalCustomData : public ICameraStorageCustomData
{
	RTTI_DECLARE_TYPE( SimpleTemporalCustomData );
	DECLARE_CUSTOM_CAMERA_DATA( 15 );

public:
	red::FixedArray< rend::customData::AliasingTextureWrapper, 2 > accumTex;

	GpuApi::Texture				currAccum;
	GpuApi::Texture				prevAccum;

	Uint32						bufferIndex{ 0 };

	Uint32						accumSum{ 0 };

	RED_FORCE_INLINE Bool IsInvalid() const { return false; }

	// virtual const AnsiChar* IsRenderingReady() const override;

	virtual void Initialize() override
	{
	}

	virtual void Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage::SCameraData& cameraData ) override
	{
		//Prepare only for shaded mode
		if ( rctx.GetFrameInfo().GetRenderingMode() > RM_ShadedMax )
			return;

		GpuApi::Texture oldAccum = prevAccum;

		prevAccum = currAccum;

		GpuApi::TextureDesc desc;

		// make sure there's space for worst-case texture size
		{
			desc.width = cameraData.GetResolutionState().GetMaxInternalWidth();
			desc.height = cameraData.GetResolutionState().GetMaxInternalHeight();
			desc.numMips = 1;
			desc.usage = GpuApi::TEXUSAGE_Samplable | GpuApi::TEXUSAGE_UnorderedAccess;
			desc.group = GpuWrapApi::TEXG_System_PostProcess;

			desc.format = rctx.GetRegularPrecisionColorTargetFormat();
			accumTex[0].EnsureWorstCase( desc, "colorHistoryForBlur_0" );
			accumTex[1].EnsureWorstCase( desc, "colorHistoryForBlur_1" );

			desc.width = rctx.GetColorTargetWidth();
			desc.height = rctx.GetColorTargetHeight();
			currAccum = accumTex[bufferIndex].RequestTexture(desc);
		}

		bufferIndex ^= 1;

		if ( !currAccum.isNull() && currAccum != oldAccum )
		{
			GpuApi::BarrierTextureAliasing( false, currAccum, oldAccum );
		}
	}

	virtual void Evict() override
	{
		for ( auto& tex : accumTex )
		{
			tex.Reset();
		}
		currAccum = GpuApi::Texture();
		prevAccum = GpuApi::Texture();
	}
};

RTTI_BEGIN_TYPE( SimpleTemporalCustomData );
{
	RTTI_PARENT_TYPE( ICameraStorageCustomData );
}
RTTI_END_TYPE();

#include "../../../../engine/shaders/include/include_distortion.fx"
#include "../../../../engine/shaders/include/include_blurredGlass.fx"

CRenderNode_ResolveDistortion::CRenderNode_ResolveDistortion()
{
	m_blurHorizontalShader[0] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurHorizontal_5" ) )->AsCompute();
	m_blurVerticalShader[0] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurVertical_5" ) )->AsCompute();

	m_blurHorizontalShader[1] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurHorizontal_9" ) )->AsCompute();
	m_blurVerticalShader[1] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurVertical_9" ) )->AsCompute();

	m_blurHorizontalShader[2] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurHorizontal_15" ) )->AsCompute();
	m_blurVerticalShader[2] = GetRenderer()->GetShader( SHADER_NAME( "m_separateBlurVertical_15" ) )->AsCompute();

	m_simpleTemporal[0] = GetRenderer()->GetShader( SHADER_NAME( "m_simpleTemporal_Cheap" ) )->AsCompute();
	m_simpleTemporal[1] = GetRenderer()->GetShader( SHADER_NAME( "m_simpleTemporal" ) )->AsCompute();
}

void CRenderNode_ResolveDistortion::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	auto rtColor = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "color" ) );
	auto rtDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDistortionMask = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "distortionMask" ) );
	auto rtDistortedColor = rctx.RTTempAlloc<RFUF_Write>( RENDER_NAME_TAG( "distortedColor" ), rtColor );
	auto rtVelocityBuffer = rctx.Test( RFF_VelocityBuffer ) ? rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "velocityBuffer" ) ) : RenderScopeNameTag::CreateNull();
	auto rtGlassBlurRadius = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "glassBlurRadius" ) );

	auto rtDistortionMaskTiles = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "distortionMaskTiles" ) );
	auto rtFrostedGlassBlurMaskTiles = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "frostedGlassBlurMaskTiles" ) );

	SRenderFlowTargetDesc colorMipMapsDesc = rctx.GetRegularPrecisionColorTargetDesc();
	colorMipMapsDesc.m_maxWidth /= 2;
	colorMipMapsDesc.m_maxHeight /= 2;
	colorMipMapsDesc.m_sizeWidth /= 2;
	colorMipMapsDesc.m_sizeHeight /= 2;
	colorMipMapsDesc.m_maxNumMipLevels = FROSTED_GLASS_NUM_MIPS;
	colorMipMapsDesc.m_numMipLevels = FROSTED_GLASS_NUM_MIPS;
	colorMipMapsDesc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

	auto rtColorMipMapsH = rctx.RTTempAlloc<RFUF_Read | RFUF_Write>( RENDER_NAME_TAG( "colorMipMapsH" ), colorMipMapsDesc );
	auto rtColorMipMapsV = rctx.RTTempAlloc<RFUF_Read | RFUF_Write>( RENDER_NAME_TAG( "colorMipMapsV" ), colorMipMapsDesc );

	SRenderFlowTargetDesc descTileCount;
	descTileCount.m_type = FRT_Buffer;
	// 4 for various distortion/blur variations, and 1 for blur only
	descTileCount.m_maxElemCount = descTileCount.m_elemCount = 5 * 3; 
	descTileCount.m_elemStride	= sizeof( Uint32 );
	descTileCount.m_category	= GpuWrapApi::BCC_IndirectUAV;
	descTileCount.m_usage		= GpuApi::BUT_Default;
	auto bufTileCount			= rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "distortionTileCount" ), descTileCount );

	Uint32 numTilesX = red::DivideRoundingUp( rctx.GetRegularPrecisionColorTargetDesc().m_sizeWidth, RESOLVE_DISTORTION_TILE_SIZE );
	Uint32 numTilesY = red::DivideRoundingUp( rctx.GetRegularPrecisionColorTargetDesc().m_sizeHeight, RESOLVE_DISTORTION_TILE_SIZE);

	//colorMipMapsDesc.m_extraUsageFlags &= ~GpuApi::TEXUSAGE_ESRAMResident;
	Uint32 numTiles = numTilesX * numTilesY;

	SRenderFlowTargetDesc tilesListDesc;
	tilesListDesc.m_type = FRT_Buffer;
	tilesListDesc.m_maxElemCount = tilesListDesc.m_elemCount = numTiles * 5;
	tilesListDesc.m_elemStride = sizeof( Uint32 );
	tilesListDesc.m_category = GpuWrapApi::BCC_StructuredUAV;
	tilesListDesc.m_usage = GpuApi::BUT_Default;
	auto bufTilesList = rctx.RTAlloc<RFUF_Write>( RENDER_NAME_TAG( "distortionTilesList" ), tilesListDesc );

	if ( rctx.IsConsumePhase() )
	{
		const GpuApi::eSamplerStatePreset samplers[] =
		{
			GpuApi::SAMPSTATEPRESET_ClampLinearNoMip,
			GpuApi::SAMPSTATEPRESET_ClampLinearMip,
		};

		const GpuApi::TextureRef refs[] =
		{
			rtColor,
			rtDistortionMask,
			rtGlassBlurRadius,
			rtColorMipMapsV
		};

		const TexelArea renderArea( rctx.GetWidth(), rctx.GetHeight(), 0, 0 );
		const Int32 sampleFullWidth = GpuApi::GetTextureLevelDesc( rtColor, 0 ).width;
		const Int32 sampleFullHeight = GpuApi::GetTextureLevelDesc( rtColor, 0 ).height;

		// classification of tiles
		SDistortionBuildTileListConsts buildTileListConsts;
		buildTileListConsts.MaxListSize = numTiles;
		buildTileListConsts.NumTilesX = numTilesX;
		buildTileListConsts.NumTilesY = numTilesY;

		rctx.SetConstant<eCOMPUTE>( buildTileListConsts );
		GpuApi::ClearBufferUAV_Zero( bufTileCount );
		GpuApi::ClearBufferUAV_Zero( bufTilesList );

		rctx.BindTexture<0, eCOMPUTE>( rtDistortionMaskTiles );
		rctx.BindTexture<1, eCOMPUTE>( rtFrostedGlassBlurMaskTiles );
		rctx.BindBufferUAV<0>( bufTileCount );
		rctx.BindBufferUAV<1>( bufTilesList );
		GetRenderer()->GetShader( SHADER_NAME( "m_distortionBuildTilesList" ) )->Dispatch(
			red::DivideRoundingUp( numTilesX, DEFAULT_THREAD_GROUP_SIZE ),
			red::DivideRoundingUp( numTilesY, DEFAULT_THREAD_GROUP_SIZE )
		);

		auto& simpleTemporalData = rctx.GetCameraData()->GetCustomData<SimpleTemporalCustomData>();
		CRenderShaderCompute* simpleTemporal = m_simpleTemporal[Config::cvFrostedGlassAAQuality.Get()];
		CRenderShaderCompute* blurHorizontalShader = m_blurHorizontalShader[Config::cvFrostedGlassBlurQuality.Get()];
		CRenderShaderCompute* blurVerticalShader = m_blurVerticalShader[Config::cvFrostedGlassBlurQuality.Get()];
		
		SFrostedGlassTemporalConsts simpleTXAAConsts;
		simpleTXAAConsts.TexSize = Vector4( (Float) sampleFullWidth, (Float) sampleFullHeight, 0.0f, 0.0f );
		simpleTXAAConsts.TexSize.Z = 1.0f / simpleTXAAConsts.TexSize.X;
		simpleTXAAConsts.TexSize.W = 1.0f / simpleTXAAConsts.TexSize.Y;
		simpleTXAAConsts.IsAccumDirty = rctx.GetCameraData()->m_resetRequested;

		rctx.BindTexture<0, eCOMPUTE>( rtColor );
		rctx.BindTexture<1, eCOMPUTE>( simpleTemporalData.prevAccum );
		rctx.BindTexture<2, eCOMPUTE>( rtVelocityBuffer );
		rctx.BindTexture<3, eCOMPUTE>( rtDepth );
		rctx.BindTextureUAV<0>( simpleTemporalData.currAccum );
		rctx.SetConstant<eCOMPUTE>( simpleTXAAConsts );
		simpleTemporal->Dispatch( red::DivideRoundingUp( sampleFullWidth, FROSTED_GLASS_TEMPORAL_GROUP_SIZE ), red::DivideRoundingUp( sampleFullHeight, FROSTED_GLASS_TEMPORAL_GROUP_SIZE ) );

		GetRenderer()->GenerateMipmaps(simpleTemporalData.currAccum, rtColorMipMapsV);


		rctx.BindSamplers<0, eCOMPUTE>( samplers );

		Uint32 blurTexWidth = sampleFullWidth / 2;
		Uint32 blurTexHeight = sampleFullHeight / 2;

		SFrostedGlassBlurConsts blurConsts;
		for (Uint32 i = 0; i < 3; ++i)
		{
			blurConsts.TexSize[i] = Vector4( (Float)( blurTexWidth >> i ), (Float)( blurTexHeight >> i ), 0.0f, 0.0f );
			blurConsts.TexSize[i].Z = 1.0f / blurConsts.TexSize[i].X;
			blurConsts.TexSize[i].W = 1.0f / blurConsts.TexSize[i].Y;
		}
		blurConsts.AspectRatio = blurConsts.TexSize[0].Y * blurConsts.TexSize[0].Z;
		blurConsts.MaxListSize = numTiles;
		rctx.SetConstant<eCOMPUTE>( blurConsts );

		rctx.BindTexture<1, eCOMPUTE>( rtGlassBlurRadius );
		GpuApi::BindIndirectArgs( bufTileCount, sizeof( SDispatchIndirectArgs ) * 4 );
		rctx.BindBufferUAV<3>( bufTilesList );

		rctx.BindTexture<0, eCOMPUTE>( rtColorMipMapsV );
		rctx.BindTextureMipUAV<0>( rtColorMipMapsH, 0 );
		rctx.BindTextureMipUAV<1>( rtColorMipMapsH, 1 );
		rctx.BindTextureMipUAV<2>( rtColorMipMapsH, 2 );
		blurHorizontalShader->DispatchIndirect();

		rctx.BindTexture<0, eCOMPUTE>( rtColorMipMapsH );
		rctx.BindTextureMipUAV<0>( rtColorMipMapsV, 0 );
		rctx.BindTextureMipUAV<1>( rtColorMipMapsV, 1 );
		rctx.BindTextureMipUAV<2>( rtColorMipMapsV, 2 );
		blurVerticalShader->DispatchIndirect();
		GpuApi::BindIndirectArgs( GpuApi::BufferRef::Null() );

		GpuApi::BarrierBufferUAV( bufTileCount );
		GpuApi::BarrierBufferUAV( bufTilesList );

		SDistortionConsts distortionConsts;
		distortionConsts.TexSize = Vector4( (Float)sampleFullWidth, (Float)sampleFullHeight, 1.0f / ( (Float)sampleFullWidth ), 1.0f / ( (Float)sampleFullHeight ) );
		distortionConsts.MaxListSize = numTiles;
		rctx.SetConstant<eCOMPUTE>( distortionConsts );

#ifdef __EMSCRIPTEN__
		// CP2077 web [flash]: distortedColor is an RTTempAlloc (aliasable flow target) that is swapped into "color" below,
		// but only the indirect m_distortion_* dispatches write it, per classified tile. If a tile is not written, "color"
		// shows whatever the aliased texture last held: usually the previous frame (looks right), sometimes another
		// pass's target (floor/vehicles black, or the whole world black under an intact HUD). Seed it with the resolved
		// color first; tiles the dispatches write still overwrite it. Opt-in A/B only (?distortionCopy=1): measured no effect on the flash (49 vs 70 bad frames).
		{
			static const int cpCopy = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'distortionCopy' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
			if ( cpCopy )
			{
				const Bool cpCopied = GpuApi::CopyTextureData( rtDistortedColor, rtColor );
				static std::atomic< unsigned > cpCopyLogs{ 0 };
				const unsigned n = cpCopyLogs.fetch_add( 1 ) + 1;
				if ( n <= 4 || ( n & ( n - 1 ) ) == 0 ) std::fprintf( stderr, "CP2077 distortion seed copy: n=%u ok=%d\n", n, cpCopied ? 1 : 0 );
			}
		}
#endif
		rctx.BindTextures<0, eCOMPUTE>( refs );
		rctx.BindSamplers<0, eCOMPUTE>( samplers );
		rctx.BindTextureUAV<0>( rtDistortedColor );
		rctx.BindBufferUAV<1>( bufTilesList );

		GpuApi::BindIndirectArgs( bufTileCount, sizeof( SDispatchIndirectArgs ) * 0);
		GetRenderer()->GetShader( SHADER_NAME( "m_distortion_00" ) )->AsCompute()->DispatchIndirect();
		GpuApi::BindIndirectArgs( bufTileCount, sizeof( SDispatchIndirectArgs ) * 1);
		GetRenderer()->GetShader( SHADER_NAME( "m_distortion_01" ) )->AsCompute()->DispatchIndirect();
		GpuApi::BindIndirectArgs( bufTileCount, sizeof( SDispatchIndirectArgs ) * 2);
		GetRenderer()->GetShader( SHADER_NAME( "m_distortion_10" ) )->AsCompute()->DispatchIndirect();
		GpuApi::BindIndirectArgs( bufTileCount, sizeof( SDispatchIndirectArgs ) * 3);
		GetRenderer()->GetShader( SHADER_NAME( "m_distortion_11" ) )->AsCompute()->DispatchIndirect();
		GpuApi::BindIndirectArgs( GpuApi::BufferRef::Null() );

	}
	rctx.RTSwap( rtColor, rtDistortedColor );
}

#ifdef GPUAPI_EXTENDED_SYNC
void CRenderNode_InterCommandListSyncStartFrame::Execute(const SRenderNodeImplContext& rctx, job::Builder* builder) const
{
	if (!rctx.IsConsumePhase())
		return;

	GpuApi::InterCommandListSyncNewFrame();
}

void CRenderNode_SignalIntermediateSyncPoint::Execute(const SRenderNodeImplContext& rctx, job::Builder* builder) const
{
	if (!rctx.IsConsumePhase())
		return;

	GpuApi::SignalInterCommandListSyncPoint( m_syncPoint );
}

void CRenderNode_WaitIntermediateSyncPoint::Execute(const SRenderNodeImplContext& rctx, job::Builder* builder) const
{
	if ( rctx.IsConsumePhase() )
	{
		GpuApi::WaitOnInterCommandListSyncPoint( m_syncPoint );
	}

	m_epilogueFunc( rctx );
}
#endif

void CRenderNode_GlobalSystemsSetCommonUsage::Execute( const SRenderNodeImplContext& rctx, job::Builder* builder ) const
{
	if ( m_isBegin )
	{
		rctx.RTUseBegin<RFUF_Read>( RENDER_NAME_TAG( "AutoSpawnInstanceBuffer" ) );
	}
	else
	{
		rctx.RTUseEnd( RENDER_NAME_TAG( "AutoSpawnInstanceBuffer" ) );
	}
}
