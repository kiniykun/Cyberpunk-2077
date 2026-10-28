/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#include "renderInterface.h"
#include "renderGraphNodes.h"
#include "renderPostFx.h"
#include "renderPostProcess.h"

#ifdef USE_DLSS
#include "ngx.h"
#endif

#include "../../renderData/include/rendererDevConfig.h"
#include "renderInterface.h"
#include "renderHistogram.h"
#include "renderTXAACustomData.h"

// #pragma optimize("",off)

using namespace PostProcessUtilities;

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ApplyFXAA::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	if ( rctx.Test( RFF_DLSS ) )
	{
		return;
	}

	if( ( rctx.Test( RFF_AntialiasingFXAA ) && rctx.GetFrameInfo().m_displaySettings.m_displayMode != EMM_SceneReferredColor ) )
	{
		auto rtColor	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "color" ) );
		auto rtTarget	= rctx.RTTempAlloc<RFUF_Write>( RENDER_NAME_TAG( "FXAA_color" ), rtColor );

		if( rctx.IsConsumePhase() )
		{
			const CRenderFrameInfo &info = rctx.GetFrameInfo();

			// Setup targets
			rctx.BindPSO( ColorTarget( 0, rtTarget ) );
			rctx.SetViewport( GpuApi::ViewportDesc( rctx.GetWidth(), rctx.GetHeight() ) );

			// Set textures
			rctx.BindTexture<0>( rtColor );
			rctx.BindSampler<0>( GpuApi::SAMPSTATEPRESET_ClampLinearNoMip );
			rctx.BindSampler<1>( GpuApi::SAMPSTATEPRESET_ClampLinearNoMip );
			rctx.BindSampler<2>( GpuApi::SAMPSTATEPRESET_ClampLinearNoMip );

			// Build render area
			const TexelArea renderArea( rctx.GetWidth(), rctx.GetHeight(), 0, 0 );
			const Int32 sampleFullWidth  = GpuApi::GetTextureLevelDesc( rtColor, 0 ).width;
			const Int32 sampleFullHeight = GpuApi::GetTextureLevelDesc( rtColor, 0 ).height;
			const Float N = 0.5f;

			const Vector4 ver[] =
			{
				CalculateTexCoordTransform( renderArea, TexelArea( rctx.GetWidth(), rctx.GetHeight() ), sampleFullWidth, sampleFullHeight ),
				Vector4( 1.0f / ((Float)sampleFullWidth), 1.0f / ((Float)sampleFullHeight), 0.0f, 0.0f )
			};
			const Vector4 pix[] =
			{
				Vector4( 1.0f / ((Float)sampleFullWidth), 1.0f / ((Float)sampleFullHeight), 0.0f, 0.0f ),
				Vector4( -N / ((Float)sampleFullWidth), -N / ((Float)sampleFullHeight), N / ((Float)sampleFullWidth), N / ((Float)sampleFullHeight) ),
				Vector4( -2.0f / ((Float)sampleFullWidth), -2.0f / ((Float)sampleFullHeight), 2.0f / ((Float)sampleFullWidth), 2.0f / ((Float)sampleFullHeight) ),
				Vector4( 8.0f / ((Float)sampleFullWidth), 8.0f / ((Float)sampleFullHeight), -4.0f / ((Float)sampleFullWidth), -4.0f / ((Float)sampleFullHeight) )
			};

			rctx.SetConstant<eVERTEX>( ver );
			rctx.SetConstant<ePIXEL>( pix );

			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_postFXAA" ) );
		}

		rctx.RTSwap( rtColor, rtTarget );
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#if defined( __EMSCRIPTEN__ )
#include <emscripten/emscripten.h>
// [txaawait] 2026-10-24: "Waiting for systems" spent ~2.5 s on "TXAA stabilization" (temporal AA history built over
// slow loading-screen frames). No longer a loading gate (?loadWaitRender=1 = original wait); the history settles in
// the first few gameplay frames.
static bool CpTxaaLoadWait()
{
	static const int v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'loadWaitRender' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
	return v != 0;
}
#endif

const AnsiChar* TXAACustomData::IsRenderingReady() const
{
#if defined( __EMSCRIPTEN__ )
	if ( !CpTxaaLoadWait() ) return nullptr;
#endif
	return currAccum.isNull() || accumSum >= ( TEMPORAL_AA_NUM_SAMPLES - 1 ) ? nullptr : "TXAA stabilization";
}

void TXAACustomData::Initialize()
{
}

void TXAACustomData::Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage::SCameraData& cameraData )
{
	PC_SCOPE( TXAACustomData_Prepare );
		
	// Prepare only for shaded mode
	if( rctx.GetFrameInfo().GetRenderingMode() > RM_ShadedMax )
		return;

	if ( cameraData.Test( RFF_AntialiasingTXAA ) )
	{
		GpuApi::Texture oldAccum = prevAccum;
		GpuApi::Texture oldObjectIDHistory = prevObjectIDHistory;
		GpuApi::Texture oldDepth = prevDepth;

		prevAccum = currAccum;
		prevObjectIDHistory = currObjectIDHistory;
		prevDepth = currDepth;

		GpuApi::TextureDesc desc;

		// make sure there's space for worst-case texture size
		{
			desc.width = cameraData.GetResolutionState().GetMaxInternalWidth();
			desc.height = cameraData.GetResolutionState().GetMaxInternalHeight();
			desc.numMips = 1;
			desc.usage = GpuApi::TEXUSAGE_Samplable | GpuApi::TEXUSAGE_RenderTarget;
			desc.group = GpuWrapApi::TEXG_System_PostProcess;

			desc.format = rctx.GetRegularPrecisionColorTargetFormat();
			accumTex[ 0 ].EnsureWorstCase( desc, "TXAA_Accum_0" );
			accumTex[ 1 ].EnsureWorstCase( desc, "TXAA_Accum_1" );

			desc.width /= 2;
			desc.height /= 2;
			
			desc.format = GpuWrapApi::TEXFMT_R8_Uint;
			objectIDHistory[ 0 ].EnsureWorstCase( desc, "TXAA_ObjectID_0" );
			objectIDHistory[ 1 ].EnsureWorstCase( desc, "TXAA_ObjectID_1" );

			desc.format = GpuWrapApi::TEXFMT_R16_Float;
			depthTex[ 0 ].EnsureWorstCase( desc, "TXAA_Depth_0" );
			depthTex[ 1 ].EnsureWorstCase( desc, "TXAA_Depth_1" );
		}

		{
			desc.width = cameraData.GetResolutionState().GetInternalWidth();
			desc.height = cameraData.GetResolutionState().GetInternalHeight();

			desc.format = rctx.GetRegularPrecisionColorTargetFormat();
			currAccum = accumTex[ bufferIndex ].RequestTexture( desc );

			desc.width = cameraData.GetResolutionState().GetInternalWidth() / 2;
			desc.height = cameraData.GetResolutionState().GetInternalHeight() / 2;

			desc.format = GpuWrapApi::TEXFMT_R16_Float;
			currDepth = depthTex[ bufferIndex ].RequestTexture( desc );
			
			desc.format = GpuWrapApi::TEXFMT_R8_Uint;
			currObjectIDHistory = objectIDHistory[ bufferIndex ].RequestTexture( desc );

			bufferIndex ^= 1;
		}

		if ( !currAccum.isNull() && currAccum != oldAccum )
		{
			GpuApi::BarrierTextureAliasing( false, currAccum, oldAccum );
		}
		if ( !currObjectIDHistory.isNull() && currObjectIDHistory != oldObjectIDHistory )
		{
			GpuApi::BarrierTextureAliasing( false, currObjectIDHistory, oldObjectIDHistory );
		}
		if ( !currDepth.isNull() && currDepth != oldDepth )
		{
			GpuApi::BarrierTextureAliasing( false, currDepth, oldDepth );
		}

		if( cameraData.m_resetRequested )
		{
			accumSum = 0;
		}
	}
	else
	{
		Evict();
	}
}

void TXAACustomData::Evict()
{
	if ( !currAccum.isNull() || !prevAccum.isNull() )
	{
		accumSum = 0;
	}

	currAccum			= GpuApi::Texture();
	currObjectIDHistory	= GpuApi::Texture();
	currDepth			= GpuApi::Texture();
	prevAccum			= GpuApi::Texture();
	prevObjectIDHistory	= GpuApi::Texture();
	prevDepth			= GpuApi::Texture();

	accumTex[0].Reset();
	accumTex[1].Reset();

	objectIDHistory[0].Reset();
	objectIDHistory[1].Reset();

	depthTex[0].Reset();
	depthTex[1].Reset();
}

RTTI_BEGIN_TYPE( TXAACustomData );
{
	RTTI_PARENT_TYPE( ICameraStorageCustomData );
}
RTTI_END_TYPE();

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_BuildTemporalInvalidationMask::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	const TXAACustomData& txaaData = rctx.GetCameraData()->GetCustomData<TXAACustomData>();

	if ( !rctx.Test( RFF_AntialiasingTXAA ) )
	{
		return;
	}

	auto rtTemporalInvalidationMask = rctx.RT<RFUF_Write>( RENDER_NAME_TAG( "temporalInvalidationMask" ) );
	auto rtVelocityBuffer = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "velocityBuffer" ) );
	auto rtDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDepthWeaponCorrected = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );

	SRenderFlowTargetDesc downsampledVelocityDesc = rctx.GetColorTargetDesc( GpuWrapApi::TEXFMT_R16G16_Float );
	downsampledVelocityDesc.m_format = GpuWrapApi::TEXFMT_R16G16_Float;
	downsampledVelocityDesc.m_denyUnorderedAccess = true;
	downsampledVelocityDesc.m_maxWidth = Max( 1, downsampledVelocityDesc.m_maxWidth / 2 );
	downsampledVelocityDesc.m_maxHeight = Max( 1, downsampledVelocityDesc.m_maxHeight / 2 );
	downsampledVelocityDesc.m_sizeWidth = Max( 1, downsampledVelocityDesc.m_sizeWidth / 2 );
	downsampledVelocityDesc.m_sizeHeight = Max( 1, downsampledVelocityDesc.m_sizeHeight / 2 );

	auto rtVelocityDownsampled = rctx.RTAlloc<RFUF_Read|RFUF_Write>( RENDER_NAME_TAG( "velocityDownsampled" ), downsampledVelocityDesc );
	auto rtDilatedVelocity = rctx.RTAlloc<RFUF_Read|RFUF_Write>( RENDER_NAME_TAG( "dilatedVelocity" ), downsampledVelocityDesc );
	auto rtDilatedTemporalInvalidationMask = rctx.RTTempAlloc<RFUF_Read|RFUF_Write>( RENDER_NAME_TAG( "dilatedTemporalInvalidationMask" ), rtTemporalInvalidationMask );

	if ( rctx.IsConsumePhase() )
	{
		const GpuApi::eSamplerStatePreset samplers[] =
		{
			GpuApi::SAMPSTATEPRESET_ClampLinearNoMip,
			GpuApi::SAMPSTATEPRESET_ClampPointNoMip
		};

		const GpuApi::TextureDesc& texDesc = GpuApi::GetTextureDesc( rtTemporalInvalidationMask );
		const GpuApi::TextureDesc& texAccumDesc = GpuApi::GetTextureDesc( txaaData.currAccum );
		const GpuApi::TextureDesc prevTexDesc = !txaaData.prevAccum.isNull() ? GpuApi::GetTextureDesc( txaaData.prevAccum ) : texDesc;

		const SRenderCameraLastFrameData &lastCamera = rctx.GetCameraData()->GetLastFrameData( RPl_Scene );

		GpuApi::ViewportDesc viewportDesc( rctx.GetWidth() / 2, rctx.GetHeight() / 2 );
		rctx.SetViewport( viewportDesc );

		rctx.BindSamplers<0, ePIXEL>( samplers );

		// downsample depth and velocity
		{
			struct PIX
			{
				math::Vector4	currResolution;
			} pix;
			pix.currResolution = math::Vector4( texDesc.width, texDesc.height, 1.0f / texDesc.width, 1.0f / texDesc.height );
			rctx.SetConstant<ePIXEL>( pix );

			rctx.BindPSO(
				ColorTarget( 0, txaaData.currDepth ),
				ColorTarget( 1, txaaData.currObjectIDHistory ),
				ColorTarget( 2, rtVelocityDownsampled ) );

			rctx.BindTexture<0, ePIXEL>( rtDepthWeaponCorrected );
			rctx.BindTexture<1, ePIXEL>( rtVelocityBuffer );
			rctx.BindTextureStencil<2, ePIXEL>( rtDepth );

			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_downsampleDepthVelocity" ) );
		}

		// build invalidation mask
		{
			struct PIX
			{
				math::Vector4	currResolution;
				math::Vector4	prevResolution;
				Matrix			currCameraView;
				Matrix			prevCameraView;
				Matrix			viewToScreen;
				GpuApi::cbBool	isDirty;
			} pix;

			const CRenderCamera& camera = rctx.GetRenderCamera();

			pix.currCameraView = camera.GetWorldToView().Transposed();
			pix.viewToScreen = camera.GetViewToScreenRevProjAware().Transposed();
			pix.prevCameraView = lastCamera.m_worldToView.Transposed();
			pix.currResolution = math::Vector4( texDesc.width, texDesc.height, 1.0f / texDesc.width, 1.0f / texDesc.height );
			pix.prevResolution = !txaaData.IsInvalid() ? math::Vector4( prevTexDesc.width, prevTexDesc.height, 1.0f / prevTexDesc.width, 1.0f / prevTexDesc.height ) : math::Vector4::ZEROS();
			pix.isDirty = txaaData.IsInvalid() || !lastCamera.m_isValid || ( camera.IsCameraCut() && !rctx.GetCameraData()->m_timeOfDayCut );

			rctx.SetConstant<ePIXEL>( pix );

			rctx.BindPSO(
				ColorTarget( 0, rtTemporalInvalidationMask ),
				ColorTarget( 1, rtDilatedVelocity ) );

			rctx.BindTexture<0, ePIXEL>( rtVelocityDownsampled );
			rctx.BindTexture<1, ePIXEL>( txaaData.currObjectIDHistory );
			rctx.BindTexture<2, ePIXEL>( txaaData.prevObjectIDHistory );
			rctx.BindTexture<3, ePIXEL>( txaaData.currDepth );
			rctx.BindTexture<4, ePIXEL>( txaaData.prevDepth );

			TStaticMetaEntryHash shaderHash =
				txaaData.isPlayerDrivingVehicle ?
				SHADER_NAME( "m_buildTemporalInvalidationMask_DepthVelHeuristic" ) :
				SHADER_NAME( "m_buildTemporalInvalidationMask" );

			GetRenderer()->DrawTriangleNoBuffers( shaderHash );
		}

		// dilate invalidation mask
		{
			struct PIX
			{
				math::Vector4	currResolution;
			} pix;
			pix.currResolution = math::Vector4( texDesc.width, texDesc.height, 1.0f / texDesc.width, 1.0f / texDesc.height );
			rctx.SetConstant<ePIXEL>( pix );

			rctx.BindPSO( ColorTarget( 0, rtDilatedTemporalInvalidationMask ) );

			rctx.BindTexture<0, ePIXEL>( rtTemporalInvalidationMask );

			GetRenderer()->DrawTriangleNoBuffers( SHADER_NAME( "m_dilateTemporalInvalidationMask" ) );
		}
	}

	rctx.RTSwap( rtTemporalInvalidationMask, rtDilatedTemporalInvalidationMask );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void CRenderNode_ApplyTXAA::Execute( const SRenderNodeImplContext& rctx, job::Builder* ) const
{
	TXAACustomData& txaaData = rctx.GetCameraData()->GetCustomData<TXAACustomData>();
	if ( txaaData.currAccum.isNull() )
	{
		return;
	}

	if ( !rctx.Test( RFF_AntialiasingTXAA ) )
	{
		return;
	}

	auto rtColor = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "color" ) );
	auto rtVelocityBuffer = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "velocityBuffer" ) );
	auto rtDilatedVelocity = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "dilatedVelocity" ) );
	auto rtDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtDepthWeaponCorrected = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBufferWeaponCorrected" ) );
	auto rtTemporalInvalidationMask = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "temporalInvalidationMask" ) );
	auto rtTransparencyMask = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "transparencyTXAAMask" ) );

	SRenderFlowTargetDesc esramColorDesc = rctx.GetRegularPrecisionColorTargetDesc();
	esramColorDesc.m_extraUsageFlags |= GpuApi::TEXUSAGE_ESRAMResident;

	const GpuWrapApi::eTextureFormat renderTargetFormat = esramColorDesc.m_format;
	RED_FATAL_ASSERT( renderTargetFormat == GpuWrapApi::TEXFMT_R16G16B16A16_Float || renderTargetFormat == GpuWrapApi::TEXFMT_R11G11B10_Float, "Unexpected render target format" );

	auto rtTargetColor = rctx.RTTempAlloc<RFUF_Read | RFUF_Write>( RENDER_NAME_TAG( "TXAA_color" ), esramColorDesc );

	if( rctx.IsConsumePhase() )
	{
		struct PIX
		{
			math::Vector4	currResolution;
			math::Vector4	prevResolution;
			Matrix			currCameraView;
			Matrix			prevCameraView;
			Matrix			viewToScreen;
			Float			minBlendFactor;
			GpuApi::cbBool	isDirty;
			Uint32			m_debugMode{ (Uint32)renderDev::TXAA_NoDebug };
		} pix;

#ifndef RED_CONFIGURATION_FINAL
		if ( auto devConfig = GetRenderer()->GetDevConfig() )
		{
			pix.m_debugMode = (Uint32)devConfig->TXAA.m_debugMode;
		}
#endif

		const GpuApi::TextureDesc& texDesc = GpuApi::GetTextureDesc( rtTargetColor );
		const GpuApi::TextureDesc& texAccumDesc = GpuApi::GetTextureDesc( txaaData.currAccum );
		RED_FATAL_ASSERT( texAccumDesc.format == texDesc.format, "Both render target formats must match" );
		const GpuApi::TextureDesc prevTexDesc = !txaaData.prevAccum.isNull() ? GpuApi::GetTextureDesc( txaaData.prevAccum ) : texDesc;

		const SRenderCameraLastFrameData &lastCamera = rctx.GetCameraData()->GetLastFrameData( RPl_Scene );

		{
			const GpuApi::TextureRef refs[] =
			{
				/* 0 */ rtColor,
				/* 1 */ txaaData.prevAccum,
				/* 2 */ rtDilatedVelocity,
				/* 3 */ rtDepthWeaponCorrected,
				/* 4 */ rtTransparencyMask
			};

			const GpuApi::eSamplerStatePreset samplers[] =
			{
				GpuApi::SAMPSTATEPRESET_ClampLinearNoMip,
				GpuApi::SAMPSTATEPRESET_ClampPointNoMip
			};

			rctx.BindPSO(
				ColorTarget( 0, rtTargetColor ),
				ColorTarget( 1, txaaData.currAccum ) );

			GpuApi::ViewportDesc viewportDesc( rctx.GetWidth(), rctx.GetHeight() );
			rctx.SetViewport( viewportDesc );

			const CRenderCamera& camera = rctx.GetRenderCamera();

			GetRenderer()->GetStateManager().SetCamera( rctx, camera, rctx.GetWidth(), rctx.GetHeight(), nullptr, false );

			pix.currCameraView = camera.GetWorldToView().Transposed();
			pix.viewToScreen = camera.GetViewToScreenRevProjAware().Transposed();
			pix.prevCameraView = lastCamera.m_worldToView.Transposed();
			pix.currResolution = math::Vector4( texDesc.width, texDesc.height, 1.0f / texDesc.width, 1.0f / texDesc.height );
			pix.prevResolution = math::Vector4( prevTexDesc.width, prevTexDesc.height, 1.0f / prevTexDesc.width, 1.0f / prevTexDesc.height );
			pix.isDirty = txaaData.IsInvalid() || !lastCamera.m_isValid || ( camera.IsCameraCut() && !rctx.GetCameraData()->m_timeOfDayCut );

			// compute min blend factor
			{
				const Float dt = Clamp( GetRenderer()->GetLastEngineTickDelta(), 1.0f/60.0f, 1.0f/20.0f );
				const Float blendAfterOneSec = 0.2f; // aim for 20% of original image after one second of blending
				pix.minBlendFactor = 1.0f - Clamp( powf( blendAfterOneSec, dt ), 0.0f, 1.0f );
			}

			rctx.SetConstant<ePIXEL>( pix );

			auto exposure = CameraCustomData_Histogram::GetFrameExposure( rctx );

			rctx.BindTextures<0, ePIXEL>( refs );
			rctx.BindSamplers<0, ePIXEL>( samplers );
			rctx.BindTextureStencil<6, ePIXEL>( rtDepth );
			rctx.BindBufferSRV<7>( exposure );
			rctx.BindBufferSRV<0,eVERTEX>( exposure );

			rctx.BindTexture<8, ePIXEL>( rtTemporalInvalidationMask );
			rctx.BindTexture<9, ePIXEL>( rtDepth );

			const Bool isF11_11_10 = renderTargetFormat == GpuWrapApi::TEXFMT_R11G11B10_Float;

			TStaticMetaEntryHash shaderHash = isF11_11_10 ? SHADER_NAME( "m_postTXAA_R11G11B10" ) : SHADER_NAME( "m_postTXAA" );

			GetRenderer()->DrawTriangleNoBuffers( shaderHash );
		}

		txaaData.accumSum = Min<Uint32>( txaaData.accumSum + 1, TEMPORAL_AA_NUM_SAMPLES );
	}

	rctx.RTSwap( rtColor, rtTargetColor );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#ifdef USE_DLSS

CRenderNode_ApplyDLSS::CRenderNode_ApplyDLSS()
{
	m_convertColorBiasMask = GetRenderer()->GetShader( SHADER_NAME( "m_dlssConvertColorBiasMask" ) )->AsCompute();
}

void CRenderNode_ApplyDLSS::Execute(const SRenderNodeImplContext& rctx, job::Builder*) const
{
	if ( !rctx.Test( RFF_DLSS ) )
		return;

	auto rtColor = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "color" ) );
	auto rtDepth = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "depthBuffer" ) );
	auto rtVelocityBuffer = rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "velocityBuffer" ) );

	SRenderFlowTargetDesc rtColorDescUpscaled = rctx.GetRegularPrecisionColorTargetDesc();
	rtColorDescUpscaled.m_maxWidth		= rctx.GetPostAAMaxWidth();
	rtColorDescUpscaled.m_maxHeight		= rctx.GetPostAAMaxHeight();
	rtColorDescUpscaled.m_sizeWidth		= rctx.GetPostAAWidth();
	rtColorDescUpscaled.m_sizeHeight	= rctx.GetPostAAHeight();

	auto rtColorUpscaled = rctx.RTTempAlloc<RFUF_Write>( RENDER_NAME_TAG( "colorUpscaled" ), rtColorDescUpscaled );

	SRenderFlowTargetDesc rtColorBiasMaskDesc = rctx.GetRegularPrecisionColorTargetDesc();
	rtColorBiasMaskDesc.m_format = GpuWrapApi::eTextureFormat::TEXFMT_R8_Unorm;
	auto rtTransparencyMask	= rctx.RT<RFUF_Read>( RENDER_NAME_TAG( "transparencyTXAAMask" ) );
	auto rtColorBiasMask = rctx.RTTempAlloc<RFUF_Write>( RENDER_NAME_TAG( "colorBiasMask" ), rtColorBiasMaskDesc );

	if ( rctx.IsConsumePhase() )
	{
		// this conversion pass is gonna be removed in further, as it should become redundant
		Vector4 imageSize;
		imageSize.X = (Float)rtColorBiasMaskDesc.m_sizeWidth;
		imageSize.Y = (Float)rtColorBiasMaskDesc.m_sizeHeight;
		imageSize.Z = 1.0f / imageSize.X;
		imageSize.W = 1.0f / imageSize.Y;

		rctx.SetConstant<eCOMPUTE>( imageSize );

		rctx.BindTexture<0, eCOMPUTE>(rtTransparencyMask);
		rctx.BindTextureUAV<0>(rtColorBiasMask);
		constexpr Uint32 kTileSize = 16;

		m_convertColorBiasMask->Dispatch( red::DivideRoundingUp(rtColorBiasMaskDesc.m_sizeWidth, kTileSize), red::DivideRoundingUp(rtColorBiasMaskDesc.m_sizeHeight, kTileSize));

		GetRenderer()->GetNGX()->ApplyDLSS( rctx, rtColor, rtVelocityBuffer, rtDepth, rtColorBiasMask, rtColorUpscaled );
	}

	// WARNING!!!!!
	// THIS POTENTIALLY SWAPS TARGETS OF DIFFERENT RESOLUTIONS.
	rctx.RTSwap( rtColor, rtColorUpscaled );
}
#endif // USE_DLSS
