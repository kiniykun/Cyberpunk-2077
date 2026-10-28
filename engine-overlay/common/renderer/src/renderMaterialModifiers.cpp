/**
* Copyright (c) 2007 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "renderMesh.h"
#include "renderScene.h"
#include "../../renderData/include/materialModifier.h"
#include "renderMaterialModifiers.h"
#include "renderProxyParticles.h"
#include "renderParticleEmitter.h"
#include "particleDPLTexture.h"
#include "subframeInterpolation.h"
#include "renderProxyMesh.h"
#include "renderTexture.h"
#include "renderMaterialExtractedDataMgr.h"
#include "../../renderData/include/renderUtils.h"
#include "renderSkinningData.h"
#include "renderStructuredBuffer.h"
#include "gameplayFx_CPFocusMode.h"
#include "renderPostProcess.h"
#include "fontCache.h"
#include "renderMultilayerMask.h"
#include "renderSurfaceGenerator_Multilayer.h"
#include "renderSurfaceGenerator_MultilayerProxy.h"
#include "../../renderData/include/renderFrameInfo.h"

#include "renderDismembermentData.h"
#include "../../renderData/include/renderDismemberment.h"
#include "../../renderData/include/renderDrawBuffer.h"
#include "../../../common/renderData/include/meshEnum.h"
#include "../../renderData/include/rendererDevConfig.h"

#include "../../../common/renderer/src/renderProxyAutoSpawner.h"
#include "../../redReflection/include/resourceMetricsBank.h"
#include "renderSSRCustomData.h"

// [skperf] round 18 mirrorRate
#include <atomic>
#include <mutex>
#include <cstdio>
#if defined( RED_PLATFORM_WEB )
#include <emscripten.h>
#endif

// #pragma optimize("",off)

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

namespace Config
{
	TConfigVar< Bool > cvDLSSEnableMirrorScaling( "DLSS", "EnableMirrorScaling", true, eConsoleVarFlag_Developer );
	TConfigVar< Float > cvDLSSMirrorScaling( "DLSS", "MirrorScaling", -1.0f, eConsoleVarFlag_Developer );
}

struct SMatModApplyParams
{
	SMatModApplyParams ( const SRenderNodeImplContext *rctx, const CBatchDataAllocator &batchDataAllocator, const SRenderChunk &chunk )
		: m_renderNodeContext ( rctx )
		, m_batchDataAllocator ( batchDataAllocator )
		, m_chunk ( chunk )
	{}

	const SRenderNodeImplContext *m_renderNodeContext;
	const CBatchDataAllocator &m_batchDataAllocator;
	const SRenderChunk &m_chunk;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_HitProxy( const SMatModifierDataHitProxy &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );
	
	const CRenderFrameInfo& frameInfo = params.m_renderNodeContext->GetFrameInfo();

#ifndef NO_EDITOR

	if ( matModData.m_idSlot < 0 || matModData.m_selectionAreaSlot < 0 )
	{
		return;
	}

	Uint32 selectionId = 0;
	{
		const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
		if ( geoBatchData.IsDebug() || geoBatchData.IsText() )
		{
			selectionId = geoBatchData.m_debug.m_hitProxyId;
		}
		else
		{
			const IRenderProxyDrawable* proxy = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy();
			selectionId = proxy->GetRenderSelectionId();
		}
	}	
	
	Vector4 selectionArea = Vector4(
		(Float)frameInfo.m_selectionArea.m_left,
		(Float)frameInfo.m_selectionArea.m_right,
		(Float)frameInfo.m_selectionArea.m_top,
		(Float)frameInfo.m_selectionArea.m_bottom );
	
	mmConstants.SetValue( matModData.m_idSlot, IRenderSelectable::EncodeID( selectionId ).ToVector() );
	mmConstants.SetValue( matModData.m_selectionAreaSlot, selectionArea );
#else
	RED_FATAL( "HitProxy not supported - why was it even requested?" );
#endif
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_DrawBufferMask( const SMatModifierDataDrawBufferMask &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	if ( !matModData.IsValid() )
	{
		return;
	}

	const SGeometryBatchData& geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );

	const rend::DrawBufferCommand* command = nullptr;

	if ( geoBatchData.IsDrawBuffer() )
	{
		command = geoBatchData.m_drawBuffer.command;
		RED_FATAL_ASSERT( command, "Invalid draw command pointer" );
	}
	else if ( geoBatchData.IsText() )
	{
		command = geoBatchData.m_text.m_command;
	}

	RED_STATIC_ASSERT( rend::DrawBufferCommand::MaxMasks <= 4 );
	math::Vector4 maskParams = math::Vector4::ZEROS(); // rotation, opacity+invert

	RED_STATIC_ASSERT( rend::DrawBufferCommand::MaxMasks == RED_ARRAY_COUNT( matModData.m_idMaskTextures ) );
	RED_STATIC_ASSERT( rend::DrawBufferCommand::MaxMasks == RED_ARRAY_COUNT( matModData.m_idMaskRects ) );
	RED_STATIC_ASSERT( rend::DrawBufferCommand::MaxMasks == RED_ARRAY_COUNT( matModData.m_idMaskCoords ) );
	for ( Uint32 i = 0; i < rend::DrawBufferCommand::MaxMasks; ++i )
	{
		GpuApi::TextureRef maskTexRef = GpuApi::GetInternalTexture( GpuApi::INTERTEX_White );
		math::Vector4 maskRect( 1.0f, 0.0f, 0.0f, 0.0f ); // disable mask by default (by setting x > y)
		math::Vector4 maskCoords( 0.0f, 0.0f, 1.0f, 1.0f );

		if ( command )
		{
			const rend::DrawBufferMaskInfo& mask = command->masks[ i ];
			if ( const CRenderTextureBase* maskTexture = static_cast<CRenderTextureBase*>( mask.texture.Get() ) )
			{
				maskTexRef = maskTexture->GetTextureRef();
				maskRect = math::Vector4( mask.rect.m_left, mask.rect.m_right, mask.rect.m_top, mask.rect.m_bottom );
				maskCoords = math::Vector4( mask.texCoords.m_left, mask.texCoords.m_top,
					mask.texCoords.m_right - mask.texCoords.m_left, mask.texCoords.m_bottom - mask.texCoords.m_top );
				maskParams[ 2 * i ] = mask.angle;

				const float maskWeight = 1.0f - mask.opactity;
				maskParams[ 2 * i + 1 ] = mask.invert ? -maskWeight : maskWeight;
			}
		}

		if ( matModData.m_idMaskTextures[i] >= 0 )
		{
			GpuApi::BindTexture( matModData.m_idMaskTextures[ i ], maskTexRef, GpuWrapApi::PixelShader );
		}
		if ( matModData.m_idMaskRects[ i ] >= 0 )
		{
			mmConstants.SetValue( matModData.m_idMaskRects[ i ], maskRect );
		}
		if ( matModData.m_idMaskCoords[ i ] >= 0 )
		{
			mmConstants.SetValue( matModData.m_idMaskCoords[ i ], maskCoords );
		}
	}

	if ( matModData.m_idMaskParams >= 0 )
	{
		mmConstants.SetValue( matModData.m_idMaskParams, maskParams );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_EffectParams( const SMatModifierDataEffectParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	if( !matModData.IsValid() )
	{
		return;
	}

	const SGeometryBatchData& geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );

	const rend::DrawBufferCommand* command = nullptr;

	if( geoBatchData.IsDrawBuffer() )
	{
		command = geoBatchData.m_drawBuffer.command;
		RED_FATAL_ASSERT( command, "Invalid draw command pointer" );
	}
	else if( geoBatchData.IsText() )
	{
		command = geoBatchData.m_text.m_command;
	}

	GpuApi::TextureRef maskTexRef = GpuApi::GetInternalTexture( GpuApi::INTERTEX_White );
	math::Vector4 effectRect( 1.0f, 0.0f, 0.0f, 0.0f ); // disable mask by default (by setting x > y)
	math::Vector4 effectCoords( 0.0f, 0.0f, 1.0f, 1.0f );
	math::Vector4 effectParams = math::Vector4::ZEROS(); // rotation, opacity+invert

	if( command )
	{
		const rend::DrawBufferEffectInfo& effect = command->effectInfo;
		if( effect.enabled )
		{
			if( const CRenderTextureBase *effectTexture = static_cast< CRenderTextureBase* >( effect.texture.Get() ) )
			{
				maskTexRef = effectTexture->GetTextureRef();
			}

			effectRect = math::Vector4( effect.rect.m_left, effect.rect.m_right, effect.rect.m_top, effect.rect.m_bottom );
			effectCoords = math::Vector4( effect.texCoords.m_left, effect.texCoords.m_top,
				effect.texCoords.m_right - effect.texCoords.m_left, effect.texCoords.m_bottom - effect.texCoords.m_top );
			effectParams = effect.params;
		}
	}

	if( matModData.m_idTexture >= 0 )
	{
		GpuApi::BindTexture( matModData.m_idTexture, maskTexRef, GpuWrapApi::PixelShader );
	}
	if( matModData.m_idRect >= 0 )
	{
		mmConstants.SetValue( matModData.m_idRect, effectRect );
	}
	if( matModData.m_idTextureCoords >= 0 )
	{
		mmConstants.SetValue( matModData.m_idTextureCoords, effectCoords );
	}

	if( matModData.m_idParams >= 0 )
	{
		mmConstants.SetValue( matModData.m_idParams, effectParams );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_ColorAndTexture( const SMatModifierDataColorAndTexture &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	if ( !matModData.IsValid() )
	{
		return;
	}

	math::Vector4 color = math::Vector4::ONES();
	GpuApi::TextureRef texRef = GpuApi::GetInternalTexture( GpuApi::INTERTEX_White );
	GpuApi::TextureRef texRef2 = GpuApi::GetInternalTexture( GpuApi::INTERTEX_White );
	math::Vector4 customParams[ 3 ] = { math::Vector4::ZEROS(), math::Vector4::ZEROS(), math::Vector4::ZEROS() };

	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( geoBatchData.IsDebug() )
	{
		color = geoBatchData.m_debug.m_color.ToVectorLinearAccu();
		if ( geoBatchData.m_debug.m_texture )
		{
			texRef = geoBatchData.m_debug.m_texture->GetTextureRef();
		}
	}
	else if ( geoBatchData.IsDrawBuffer() )
	{
		const rend::DrawBufferCommand* command = geoBatchData.m_drawBuffer.command;
		RED_FATAL_ASSERT( command, "Invalid draw command pointer" );

		color = command->color;
		if ( const CRenderTextureBase* texture = static_cast<CRenderTextureBase*>( command->texture.Get() ) )
		{
			texRef = texture->GetTextureRef();
		}

		RED_STATIC_ASSERT( rend::DrawBufferCommand::MaxMasks == RED_ARRAY_COUNT( customParams ) );
		for ( Uint32 i = 0; i < rend::DrawBufferCommand::MaxCustomParams; ++i )
		{
			customParams[ i ] = command->customParams[ i ];
		}
	}
	else if ( geoBatchData.IsText() )
	{
		color = geoBatchData.m_text.m_color.ToVector4();
		texRef = GetRenderer()->GetGlyphCache()->GetFontTexture();
		customParams[ 0 ] = geoBatchData.m_text.m_outlineColor.ToVectorLinearAccu();
	}
	else
	{
		RED_FATAL_ASSERT( "Unsupported geometry batch data" );
	}

	if ( matModData.m_idColor >= 0 )
	{
		mmConstants.SetValue( matModData.m_idColor, color );
	}
	if ( matModData.m_customParams[ 0 ] >= 0 )
	{
		mmConstants.SetValue( matModData.m_customParams[ 0 ], customParams[ 0 ] );
	}
	if ( matModData.m_customParams[ 1 ] >= 0 )
	{
		mmConstants.SetValue( matModData.m_customParams[ 1 ], customParams[ 1 ] );
	}
	if ( matModData.m_customParams[ 2 ] >= 0 )
	{
		mmConstants.SetValue( matModData.m_customParams[ 2 ], customParams[ 2 ] );
	}
	if ( matModData.m_idTexture >= 0 )
	{
		GpuApi::BindTexture( matModData.m_idTexture, texRef, GpuWrapApi::PixelShader );
	}
	if (matModData.m_idTexture2 >= 0)
	{
		GpuApi::BindTexture(matModData.m_idTexture2, texRef2, GpuWrapApi::PixelShader);
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_WindData( const SMatModifierDataWindData &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( GT_Mesh != geoBatchData.m_type )
	{
		return;
	}

	const CRenderMesh *mesh = geoBatchData.m_meshData.m_mesh;	
	if ( mesh->GetWindData() )
	{
		const CRenderProxy_Mesh * meshProxy = static_cast<const CRenderProxy_Mesh*>( params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy() );
		// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
		//RED_FATAL_ASSERT( meshProxy->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );

		if ( matModData.m_windDataCBSlot >= 0 )
		{
			// Grab raw ptr so we don't have to addref the shared ptr
			const CSpeedTreeWindSimulation* windSimData = meshProxy->GetSpeedTreeWindSharedData();
			if ( !windSimData->GetWindCB().isNull() )
			{
				GpuApi::BindConstantBuffer( matModData.m_windDataCBSlot, windSimData->GetWindCB(), GpuWrapApi::VertexShader );
			}
		}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_AutoSpawnData( const SMatModifierDataAutoSpawnData &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const CRenderProxy_Mesh * meshProxy = static_cast<const CRenderProxy_Mesh*>( params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy() );

	if ( matModData.IsValid() )
	{
		CRenderTerrainCellCollector& terrainCellsCollector = params.m_renderNodeContext->GetCameraStorage()->GetCustomData<CRenderTerrainCellCollector>();

		GpuApi::BindConstantBuffer( matModData.m_autoSpawnCBSlot, terrainCellsCollector.m_autoSpawnRenderMatModCB, GpuWrapApi::ShaderTypeMask( GpuWrapApi::VertexShader ) );

		auto autoSpawnInstanceBuffer = params.m_renderNodeContext->RTGet<GpuApi::BufferRef>( RENDER_NAME_TAG("AutoSpawnInstanceBuffer") );
		GpuApi::BindBufferSRV( autoSpawnInstanceBuffer, matModData.m_autoSpawnInstanceBufferSlot, GpuWrapApi::VertexShader );
		GpuApi::BindBufferSRV( terrainCellsCollector.m_instanceBucketsGPUBuffer, matModData.m_autoSpawnInstanceBucketsSlot, GpuWrapApi::VertexShader );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_AutoHideDistance( const SMatModifierDataAutoHideDistance &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const IRenderProxyDrawable* drawable = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy();

	RED_ASSERT( drawable->GetType() == RPT_Mesh, "Expecting only CRenderProxy_Mesh instances to have AutoHideDistance material modifier." );
	const CRenderProxy_Mesh * meshProxy = static_cast<const CRenderProxy_Mesh*>( drawable );

	if ( matModData.IsValid() )
	{
		mmConstants.SetValue( matModData.m_autoHideDistanceSlot,
							  Vector4( meshProxy->GetAutoHideDistance(), meshProxy->CheckMeshProxyFlag( RMPF_WindImpulseEnabled ), 0.0f, 0.0f ) );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_Garments(const SMatModifierGarments& matModData, const SMatModApplyParams& params, MaterialModifierConstants& mmConstants)
{
	if ( !matModData.IsValid() )
	{
		return;
	}

	const SGeometryBatchData& geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( geoBatchData.m_type != GT_Mesh )
	{
		return;
	}

	const Uint32 proxyDataIndex = params.m_chunk.m_data.GetProxy();

	const IRenderProxyDrawable* drawableProxy = params.m_batchDataAllocator.GetProxyData().GetData( proxyDataIndex ).GetProxy();
	if ( !drawableProxy->IsMeshTypeProxy() )
	{
		return;
	}

	// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
	//RED_FATAL_ASSERT( drawableProxy->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );

	GpuApi::TextureRef morphOffsetScalesTexture = GpuApi::TextureRef::Null();
	if ( matModData.m_morphOffsetScalesTextureSlot != -1 )
	{
		const CRenderProxy_Mesh* garmentMeshProxy = static_cast<const CRenderProxy_Mesh* >( drawableProxy );
		if ( const RenderProxyCustomData_Garment* garmentData = garmentMeshProxy->GetCustomData< RenderProxyCustomData_Garment >() )
		{
			const red::DynArray< GpuApi::TextureRef >& morphOffsetScalesTextures = garmentData->m_morphOffsetScalesTextures;
			const Uint32 chunkIndex = geoBatchData.m_meshData.m_chunkIndex;
			if ( chunkIndex < morphOffsetScalesTextures.Size() )
			{
				morphOffsetScalesTexture = morphOffsetScalesTextures[ chunkIndex ];
			}
		}
		GpuApi::BindTexture( matModData.m_morphOffsetScalesTextureSlot, morphOffsetScalesTexture, GpuWrapApi::VertexShader );
	}

	if ( matModData.m_morphOffsetScalesTextureIsBoundSlot != -1 )
	{
		const Bool isBound = !morphOffsetScalesTexture.isNull();
		mmConstants.SetValue( matModData.m_morphOffsetScalesTextureIsBoundSlot, isBound );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_ParticleParams( const SMatModifierDataParticleParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );

	const CRenderFrameInfo& frameInfo = params.m_renderNodeContext->GetFrameInfo();

	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( GT_Emitter == geoBatchData.m_type || GT_TrailEmitter == geoBatchData.m_type )
	{
		const CRenderParticleEmitter* emitter = geoBatchData.m_emitterData.m_particleEmitter;
		const IParticleData& particleData = *geoBatchData.m_emitterData.m_particleData;

		// Calculate particle system fadeout alpha
		const CRenderProxy_Particles* proxy = static_cast< const CRenderProxy_Particles * >( params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy() );
		RED_ASSERT( proxy );
		Float currAlpha = 1.f;
		const Float autoHideDistance = proxy->GetAutoHideDistance();
		if ( autoHideDistance > 0 )
		{
			Float distanceSquare = geoBatchData.m_emitterData.m_distanceSquare;
			const Float autoHideInvRange = proxy->GetAutoHideInvRange();
			currAlpha = Min( 1.f, (autoHideDistance - MSqrt(distanceSquare)) * autoHideInvRange );
			RED_ASSERT( currAlpha >= 0.f );
		}

		// Update transparency params
		const EEnvColorGroup currEnvColorGroup = emitter->GetEnvColorGroup();
		Vector4 currEnvColor = Vector4::ONES();

		if( currEnvColorGroup >= ECG_Group0 )
		{
			currEnvColor = frameInfo.m_worldRenderSettings.m_colorGroups.m_colorGroup[currEnvColorGroup - ECG_Group0].GetHDR();
			if( currEnvColor.X < 0 || currEnvColor.Y < 0 || currEnvColor.Z < 0 )
			{
				currEnvColor = Vector4::ZEROS();
			}
		}


		Box bboxToAdd = particleData.m_box;
		const Vector4 bboxInfo( bboxToAdd.Min.Z, bboxToAdd.Max.Z, bboxToAdd.Max.Z - bboxToAdd.Min.Z, 0.0f );

		if ( matModData.m_transparencyParamsSlot >= 0 )
		{
			mmConstants.SetValue( matModData.m_transparencyParamsSlot, Vector4( currEnvColor.X, currEnvColor.Y, currEnvColor.Z, currAlpha ) );	
		}
		if ( matModData.m_bboxInfoSlot >= 0 )
		{
			mmConstants.SetValue( matModData.m_bboxInfoSlot, bboxInfo );	
		}	

		UDrawerData drawerData = emitter->GetDrawerData();
		Float stretchPerVelocity = 1.0f;
		Float pivotOffset = drawerData.simple.m_pivotOffset;
		if (emitter->GetVertexType() == PVDT_MotionBlurred)
		{
			stretchPerVelocity = drawerData.motionBlur.m_stretchPerVelocity;
			pivotOffset = drawerData.motionBlur.m_pivotOffset;
		}
		else if (emitter->GetVertexType() == PVDT_EmitterOrientation)
		{
			EulerAngles orientation(drawerData.orientation.m_orientation[0], drawerData.orientation.m_orientation[1], drawerData.orientation.m_orientation[2]);
			Matrix mat = orientation.ToMatrix() * proxy->GetInstanceMatrix();
			if ( matModData.m_orientationUSlot >= 0 )
			{
				mmConstants.SetValue(matModData.m_orientationUSlot, mat.GetRow(0).Normalized3());		
			}
			if ( matModData.m_orientationVSlot >= 0 )
			{
				mmConstants.SetValue(matModData.m_orientationVSlot, mat.GetRow(1).Normalized3());		
			}
		}

		if ( emitter->GetVertexType() != PVDT_Mesh &&
			 emitter->GetVertexType() != PVDT_Screen &&
			 emitter->GetVertexType() != PVDT_FacingTrail &&
			 emitter->GetVertexType() != PVDT_Trail &&
			 emitter->GetVertexType() != PVDT_FacingBeam &&
			 emitter->GetVertexType() != PVDT_Beam )
		{
			if ( matModData.m_dplParticleDataSlot >= 0 )
			{
				GpuApi::BindBufferSRV( CParticleDPLTexture::GetParticleData(), matModData.m_dplParticleDataSlot, GpuWrapApi::VertexShader );
			}
		}

		if ( matModData.m_motionBlurParamsSlot >= 0 )
		{
			mmConstants.SetValue(matModData.m_motionBlurParamsSlot, Vector4(pivotOffset, stretchPerVelocity, 0, 0));
		}
	}
	else if (GT_Mesh == geoBatchData.m_type)
	{
		if (matModData.m_transparencyParamsSlot >= 0)
		{
			mmConstants.SetValue(matModData.m_transparencyParamsSlot, Vector4::ONES());
		}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_ShadowsDebugParams(const SMatModifierDataShadowsDebugParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
#ifndef RED_CONFIGURATION_FINAL
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( !geoBatchData.IsMesh() )
	{
		return;
	}

	// Check render proxy shadows flags settings.
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	if ( !proxyBatchData.GetProxy() )
	{
		return;
	}
	const CRenderProxy_Mesh* renderProxyMesh = static_cast<const CRenderProxy_Mesh*> ( proxyBatchData.GetProxy() );

	// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
	//RED_FATAL_ASSERT( renderProxyMesh->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );
	
	if ( params.m_renderNodeContext->GetFrameInfo().m_displaySettings.m_displayMode == EMM_TodvisRuntimePreview )
	{
		if ( matModData.m_shadowsFlagsSlot >= 0 )
		{
			// Encode sun direction in rgb channels, and in w store 1.0 for visible in shadow and 0.5 for not visible in shadow.
			Vector4 matModParams = params.m_renderNodeContext->GetFrameInfo().m_baseLightingParameters.m_sunDirection;

			// Todvis check enabled globally.
			if( params.m_renderNodeContext->GetCameraData()->GetInfo().m_applyCascadesTODVis )
			{
				const Bool meshVisibleForCascades = renderProxyMesh->IsVisibleForCurrentTOD( params.m_renderNodeContext->GetCollector()->m_cascades.m_timeOfDay );
				matModParams.W = ( meshVisibleForCascades ) ? 1.0f : 0.5f;
			}

			mmConstants.SetValue( matModData.m_shadowsFlagsSlot, matModParams );
		}
	}
	else
	{
		const Bool isRenderProxyCastingShadows = renderProxyMesh->CanCastGlobalShadows();
		const Bool isRenderProxyCastingLocalShadows = renderProxyMesh->CanCastLocalShadows();

		if ( matModData.m_shadowsFlagsSlot >= 0 )
		{
			const Float r = isRenderProxyCastingShadows ? 1.0f : 0.0f;
			const Float g = isRenderProxyCastingLocalShadows ? 1.0f : 0.0f;
			mmConstants.SetValue( matModData.m_shadowsFlagsSlot, Vector4( r, g, 0.0f, 0.0f ) );
		}
	}
#endif // RED_CONFIGURATION_FINAL
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_MultilayeredDebug( const SMatModifierDataMultilayeredDebug &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
#ifndef RED_CONFIGURATION_FINAL
	const SGeometryBatchData& geoData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	if ( !proxyBatchData.GetProxy() )
	{
		return;
	}
	const CRenderProxy_Mesh* renderProxyMesh = static_cast< const CRenderProxy_Mesh* > ( proxyBatchData.GetProxy() );
	// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
	//RED_FATAL_ASSERT( renderProxyMesh->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );

	const EEnvManagerModifier envMode = params.m_renderNodeContext->GetFrameInfo().m_displaySettings.m_displayMode;

	// #TODO: If we add more modes, probably want to have some #define's in a shared header somewhere.
	Uint32 mode = 0;
	if ( envMode == EMM_MultilayeredProxy || params.m_renderNodeContext->Test( rend::DF_MultilayerProxy_Surfaces ) )
	{
		mode = 1;
	}

	RED_ASSERT( matModData.m_debugModeSlot >= 0, "Have no debugModeSlot. Material modifier should not have been valid." );
	mmConstants.SetValue( matModData.m_debugModeSlot, Vector4( Float(mode), 0.0f, 0.0f, 0.0f ) );

	const MaterialLodGroups::Chunk *lodGroupChunk = nullptr;
	if ( geoData.m_type == GT_Mesh )
	{
		const MaterialLodGroups& materialLodGroups = renderProxyMesh->GetMaterialLodGroups();
		lodGroupChunk = &materialLodGroups.GetChunkInfo( geoData.m_meshData.m_chunkIndex );
	}

	SurfaceID surfaceID = SurfaceID::Null;
	if ( lodGroupChunk != nullptr )
	{
		surfaceID = lodGroupChunk->id;
	}

	if ( mode == 0 )
	{
		Vector4 color = Vector4::ONES();

		if ( envMode == EMM_MultilayeredMode )
		{
			color = Vector4( 0.725f, 0.114f, 0.278f, 1.0f ); // no multilayer
			if ( lodGroupChunk != nullptr && lodGroupChunk->materialInfo.m_materialParameters != nullptr )
			{
				if ( lodGroupChunk->materialInfo.m_materialParameters->IsMultilayered() )
				{
					color = Vector4( 0.0f, 0.639f, 0.0f, 1.0f );
				}
			}
		}
		else if ( envMode == EMM_MultilayeredUniqueMasks )
		{
			TRenderPtr<RenderMultilayerMask> mask = GetRenderer()->GetSurfaceCache().GetGenerator_Multilayer().Debug_GetMultilayerMask( surfaceID );
			if ( mask )
			{
				const RenderMultilayerMask* maskPtr = mask.Get();
				const Uint32 hash = red::CalculateHash32( &maskPtr, sizeof( RenderMultilayerMask* ) );
				color = Vector4( math::HSVToRGB( (Float)hash / (Float)UINT32_MAX, 1.0f, 1.0f ), 1.0f );
			}
			else
			{
				color = Vector4::ONES();
			}
		}
		else if ( envMode == EMM_MultilayeredMaskWeight )
		{
#ifdef USE_RESOURCE_METRICS_BANK
			TRenderPtr<RenderMultilayerMask> mask = GetRenderer()->GetSurfaceCache().GetGenerator_Multilayer().Debug_GetMultilayerMask( surfaceID );
			if ( mask )
			{
				const res::SingleResourceMetrics metrics = GResourceLoader->GetResourceMetricsBank()->GetMaterialMetrics( mask->GetResourcePath() );

				// >= 2MB	- red
				// 1MB		- orange
				// 512KB	- yellow
				// <128KB	- green
				const float value = metrics.gpuBytesUsed > 0 ? ( 1.0f + log2f( (float)metrics.gpuBytesUsed / 2000000.0f ) / 4.0f ) : 0.0f;
				if ( value < 0.5f )
				{
					color = Vector4::Lerp( Vector4( 0.0f, 1.0f, 0.0f, 1.0f ), Vector4( 1.0f, 1.0f, 0.0f, 1.0f ), 2.0f * value );
				}
				else if ( value < 1.0f )
				{
					color = Vector4::Lerp( Vector4( 1.0f, 1.0f, 0.0f, 1.0f ), Vector4( 1.0f, 0.0f, 0.0f, 1.0f ), 2.0f * ( value - 0.5f ) );
				}
				else
				{
					color = Vector4( 1.0f, 0.0f, 0.0f, 1.0f );
				}
			}
#endif // USE_RESOURCE_METRICS_BANK
		}
		else
		{
			color = Vector4( 0.0f, 0.0f, 0.0f, 1.0f );
		}

		if ( matModData.m_colorSlot >= 0 )
		{
			mmConstants.SetValue( matModData.m_colorSlot, color );
		}
	}
	else if ( mode == 1 )
	{
		if ( matModData.m_colorSlot >= 0 )
		{
			const Vector4 c_debugColors[] = { Vector4( 1,0,0,1 ), Vector4( 1,1,0,1 ), Vector4( 0,1,0,1 ), Vector4( 0,1,1,1 ), Vector4( 0,0,1,1 ), Vector4( 1,0,1,1 ) };

			mmConstants.SetValue( matModData.m_colorSlot, c_debugColors[ Uint32(surfaceID) % 6 ] );
		}
		// #TODO: This will need to be refactored with the new material proxy chunking behavior
#if 0
		if ( matModData.m_proxyColorRoughnessSlot >= 0 )
		{
			TRenderPtr<CRenderTexture> colorRoughness = GetRenderer()->GetSurfaceCache().GetGenerator_MultilayerProxy().GetProxyColor( surfaceID );
			if ( colorRoughness != nullptr )
			{
				colorRoughness->Bind( matModData.m_proxyColorRoughnessSlot );
			}
			else
			{
				GpuApi::BindTextures( matModData.m_proxyColorRoughnessSlot, 1, nullptr, GpuWrapApi::PixelShader );
			}
		}
		if ( matModData.m_proxyNormalMetalnessSlot >= 0 )
		{
			TRenderPtr<CRenderTexture> normalMetalness = GetRenderer()->GetSurfaceCache().GetGenerator_MultilayerProxy().GetProxyNormal( surfaceID );
			if ( normalMetalness != nullptr )
			{
				normalMetalness->Bind( matModData.m_proxyNormalMetalnessSlot );
			}
			else
			{
				GpuApi::BindTextures( matModData.m_proxyNormalMetalnessSlot, 1, nullptr, GpuWrapApi::PixelShader );
			}
		}
#endif
	}
#endif // RED_CONFIGURATION_FINAL
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_HighlightsParams( const SMatModifierHighlightsParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	if ( !proxyBatchData.GetProxy() )
	{
		return;
	}

	CGameplayEffect_CPFocusMode* focusMode = GetRenderer()->GetPostProcess()->GetGameplayEffect<CGameplayEffect_CPFocusMode>();
	if ( !focusMode )
	{
		return;
	}

	Float opacityValue;
	if ( matModData.m_fadeValueSlot >= 0 && focusMode->GetProxyOpacity( proxyBatchData.GetProxy(), opacityValue ) )
	{
		const Vector4 fadeValuesVector( opacityValue, opacityValue, opacityValue, 1.0f );
		mmConstants.SetValue( matModData.m_fadeValueSlot, fadeValuesVector );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_ParallaxParams(const SMatModifierParallaxParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	if (!proxyBatchData.GetProxy() )
	{
		return;
	}
	const IRenderProxyBase* renderProxyMesh = static_cast<const IRenderProxyBase*>( proxyBatchData.GetProxy() );

	if (matModData.m_proxyTranslationSlot >= 0)
	{
		const Vector4 translation = renderProxyMesh->GetTranslation();
		mmConstants.SetValue(matModData.m_proxyTranslationSlot, translation);
	}

	if (matModData.m_localToWorldSlot >= 0)
	{
		const Matrix localToWorld = renderProxyMesh->GetLocalToWorld();
		mmConstants.SetValue(matModData.m_localToWorldSlot, localToWorld);
	}

	if( matModData.m_worldToLocalSlot >= 0 )
	{
		const Matrix worldToLocal = renderProxyMesh->GetLocalToWorld().FullInverted();
		mmConstants.SetValue( matModData.m_worldToLocalSlot, worldToLocal );
	}
}
// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_MaterialParams( const SMatModifierDataMaterialParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );

	if ( GT_Mesh != geoBatchData.m_type )
	{
		return;
	}

	const CRenderProxy_Mesh * meshProxy = static_cast< const CRenderProxy_Mesh* >( params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy() );
	// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
	//RED_FATAL_ASSERT( meshProxy->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );

	for ( Uint32 i = 0; i < 7; ++i )
	{
		if ( matModData.m_slot[ i ] >= 0 )
		{
			mmConstants.SetValue( matModData.m_slot[ i ], meshProxy->GetCustomMaterialParameter( i ) );
		}
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void MaterialModifierProcess_VehicleParams( const SMatModifierVehicleParams& matModData, const SMatModApplyParams& params, MaterialModifierConstants& mmConstants )
{
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const SGeometryBatchData& geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );

	if ( !proxyBatchData.GetProxy() )
	{
		return;
	}

	if( GT_Mesh != geoBatchData.m_type )
	{
		return;
	}

	const CRenderProxy_Mesh* renderProxyMesh = static_cast< const CRenderProxy_Mesh* >( proxyBatchData.GetProxy() );
	const RenderMeshProxyVehicleParams vehicleParams = renderProxyMesh->GetVehicleParams();

	if ( matModData.m_meshPivotInGridSpaceSlot >= 0 )
	{
		const Vector4 value = Vector4(
			vehicleParams.m_meshPivotInGridSpace.X,
			vehicleParams.m_meshPivotInGridSpace.Y,
			vehicleParams.m_meshPivotInGridSpace.Z, 
			0.f
		);
		mmConstants.SetValue( matModData.m_meshPivotInGridSpaceSlot, value );
	}

	if ( matModData.m_gridCornersSlot >= 0 )
	{
		const Vector4 value = Vector4(
			vehicleParams.m_dimensions.X,
			vehicleParams.m_dimensions.Y,
			vehicleParams.m_dimensions.Z,
			0.f
		);
		mmConstants.SetValue( matModData.m_gridCornersSlot, value );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

// Supersimple poly clipping algo (TODO: could be faster)
void ClipConvexPolygon( red::StaticArray<Vector4, 32> &verts, const Vector4 &plane )
{
	// find clip edges
	Uint32 numClips = 0;
	Float clipRatios[2];
	Uint32 clipIndices[2];
	Bool clipSides[2];
	{
		Bool hasAnyInFront = false;
		for ( Uint32 i=0; i<verts.Size() && numClips<2; ++i )
		{
			const Vector4 v0 = verts[i];
			const Vector4 v1 = verts[(i + 1) % verts.Size() ];
			Float d0 = plane.Dot4( v0 );
			Float d1 = plane.Dot4( v1 );
			hasAnyInFront |= d0 > 0;

			// Skip if on the same side
			if ( d0 * d1 > 0 )
			{
				continue;
			}

			// Skip if same distance from plane
			const Float div = d0 - d1;
			if ( std::abs( div ) < 1e-6 )
			{
				continue;
			}

			// Skip if going front-to-back, and front-most is on plane. whole edge should be removed, not clipped.
			if ( Min(d0, d1) >= 0 )
			{
				continue;
			}

			clipRatios[numClips] = d0 / div;
			clipIndices[numClips] = i;
			clipSides[numClips] = d0 < 0;
			++numClips;
		}

		if ( !hasAnyInFront )
		{
			verts.Clear();
			return;
		}
	}

	// exit if we didn't find edge pair to clip
	if ( numClips != 2 || clipSides[0] == clipSides[1] )
	{
		return;
	}

	// ensure proper order of clip edges
	if ( clipSides[0] )
	{
		Swap( clipRatios[0], clipRatios[1] );
		Swap( clipIndices[0], clipIndices[1] );
		Swap( clipSides[0], clipSides[1] );
	}

	// update vertices
	RED_ASSERT( clipIndices[1] != clipIndices[0] );
	const Uint32 numVertsToRemove = clipIndices[1] > clipIndices[0] ? clipIndices[1] - clipIndices[0] : verts.Size() - (clipIndices[0] - clipIndices[1]);
	const Vector4 newVertex0 = Vector4::Interpolate( verts[clipIndices[0]], verts[(clipIndices[0] + 1) % verts.Size()], clipRatios[0] );
	const Vector4 newVertex1 = Vector4::Interpolate( verts[clipIndices[1]], verts[(clipIndices[1] + 1) % verts.Size()], clipRatios[1] );
	Uint32 insertIndex;
	if ( clipIndices[1] > clipIndices[0] )
	{
		verts.RemoveAt( clipIndices[0] + 1, clipIndices[1] + 1 );
		insertIndex = clipIndices[0] + 1;
	}		
	else
	{
		const Uint32 origNumVerts = verts.Size();
		verts.RemoveAt( clipIndices[0] + 1, origNumVerts );
		verts.RemoveAt( 0, clipIndices[1] + 1 );
		insertIndex = verts.Size();
	}
	verts.InsertAt( insertIndex, newVertex1 );
	verts.InsertAt( insertIndex, newVertex0 );
}

Bool ResolutionScaleHelper( const CRenderCamera &camera, const Matrix& planeLocalToWorld, Box& outBbox )
{
	outBbox.Clear();
	
	// build world space verts
	red::StaticArray< Vector4, 32 > verts;
	{
		Vector4 frustumPlanes[6];
		camera.CalculateFrustumPlanes( frustumPlanes );

		verts.PushBack( planeLocalToWorld.TransformVectorWithW( Vector4(-1,-1,0,1) ) ); // with W not really needed (need to test)
		verts.PushBack( planeLocalToWorld.TransformVectorWithW( Vector4( 1,-1,0,1) ) ); // with W not really needed (need to test)
		verts.PushBack( planeLocalToWorld.TransformVectorWithW( Vector4( 1, 1,0,1) ) ); // with W not really needed (need to test)
		verts.PushBack( planeLocalToWorld.TransformVectorWithW( Vector4(-1, 1,0,1) ) ); // with W not really needed (need to test)
		for ( Uint32 i=0; i<RED_ARRAY_COUNT(frustumPlanes); ++i )
		{
			ClipConvexPolygon( verts, frustumPlanes[i] );
		}
	}

	// project clipped verts and add to bbox
	if ( verts.Size() >= 3 )
	{
		for ( Uint32 i=0; i<verts.Size(); ++i )
		{
			Vector4 v = camera.GetWorldToScreen().TransformVectorWithW( verts[i] );
			outBbox.AddPoint( v / v.W );
		}		
	}

	RED_ASSERT( outBbox.IsEmpty() || Box ( Vector4::ZERO_3D_POINT(), 1.05f ).Contains( outBbox ) );
	return !outBbox.IsEmpty();
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_RemoteCamera( const SMatModifierData_RemoteCamera &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );

	const CRenderFrameInfo& frameInfo = params.m_renderNodeContext->GetFrameInfo();
	const CRenderSceneEx* scene = static_cast<const CRenderSceneEx*>( frameInfo.GetScene() );
	RED_FATAL_ASSERT( scene, "Invalid render scene" );

	if( !params.m_renderNodeContext->IsCameraDataAvailable() )
		return;

	const CRenderFrameCameraStorage::SCameraData *cameraData = params.m_renderNodeContext->GetCameraData();
	if (cameraData->GetResolutionState().GetInternalWidth() <= 0 || cameraData->GetResolutionState().GetInternalHeight() <= 0)
		return;

	const Uint64 currCameraHash = params.m_renderNodeContext->GetCameraData()->m_cameraHash;

	Uint64 remoteCameraHash = 0;
	Bool haveRemoteCamera = false;

	// #TODO: Make this whole parameter fetch nicer.
	static const CName CameraNameParamName = RED_NAME_CONSTEXPR( "CameraName" );
	const SParamsBatchData& paramsData = params.m_batchDataAllocator.GetParamsData().GetData( params.m_chunk.m_key.GetParams() );
	if ( paramsData.m_cpuParamsId.IsValid() )
	{
		const Uint32 numCpuParams = g_ExtractedMaterialCpuParamsMgr.GetNum( paramsData.m_cpuParamsId );
		const SExtractedMaterialCpuParameter* cpuParams = g_ExtractedMaterialCpuParamsMgr.GetData( paramsData.m_cpuParamsId );
		for ( Uint32 i = 0; i < numCpuParams; ++i )
		{
			const SExtractedMaterialCpuParameter &param = cpuParams[i];
			switch ( param.m_type )
			{
			case IMaterialDataProviderDesc::PT_Cpu_NameU64:
				{
					if ( param.m_name == CameraNameParamName && param.m_data != currCameraHash )
					{
						remoteCameraHash = param.m_data;
						haveRemoteCamera = true;
					}
				}
				break;

			default:
				break;
			}

			if ( haveRemoteCamera )
			{
				break;
			}
		}
	}

	Int32 remoteCameraRenderFlowSpace = -1;
	const CRenderFrameCameraStorage::SCameraData *remoteCameraData = nullptr;

	if ( haveRemoteCamera )
	{
		RED_ASSERT(remoteCameraHash != currCameraHash, "Remote camera is same as current camera! Checked for this already...");

		CRenderFrameCameraStorage *cameraStorage = scene->GetCameraStorage();
		cameraStorage->AddDynamicCameraDependency( currCameraHash, remoteCameraHash, CRenderFrameCameraStorage::CDF_Output_Final );

		// check if we have a dependency to the remoteCameraHash in currently executed renderGraph.
		// otherwise we won't be able to obtain the result color from this camera.
		// i.e. such a camera may be already used for rendering, but the dependency is not there
		// so the renderTargets are not prepared to be used
		const Bool isCurrentlyDependent = cameraStorage->HasDependency( currCameraHash, remoteCameraHash, false );

		Int32 idx = isCurrentlyDependent ? cameraStorage->FindCameraData( remoteCameraHash ) : -1;
		if ( -1 != idx )
		{
			remoteCameraRenderFlowSpace = cameraStorage->GetCameraData(idx)->m_renderFlowSpace;
			remoteCameraData = cameraStorage->GetCameraData(idx);
		}
	}

	// bind texture
	// note that it's perfectly valid that we may not have a flowSpaceGroup (e.g. no result for the remote camera), since
	// this stuff is available with one frame delay	
	GpuApi::TextureRef texture = GpuApi::TextureRef::Null();
	Vector4 texSizeParams = Vector4::ONES();

	if ( -1 != remoteCameraRenderFlowSpace )
	{
		const RenderFlowNameTag rtColor( RENDER_NAME_TAG( "final" ), (TRenderFlowSpace)remoteCameraRenderFlowSpace );
		texture = params.m_renderNodeContext->RTGet< GpuApi::TextureRef >( rtColor );
		RED_FATAL_ASSERT( texture );
		GpuApi::TextureLevelDesc texColorDesc = GpuApi::GetTextureLevelDesc( texture, 0 );
		texSizeParams.X = (Float)remoteCameraData->GetResolutionState().GetInternalWidth();
		texSizeParams.Y = (Float)remoteCameraData->GetResolutionState().GetInternalHeight();
		texSizeParams.Z = (Float)texColorDesc.width;
		texSizeParams.W = (Float)texColorDesc.height;
	}

	if ( matModData.m_textureSlot >= 0 )
	{
		GpuApi::BindTexture( matModData.m_textureSlot, texture, GpuWrapApi::PixelShader );
	}
	if ( matModData.m_textureIsBoundSlot >= 0 )
	{
		mmConstants.SetValue( matModData.m_textureIsBoundSlot, !texture.isNull() && matModData.m_textureSlot >= 0 );
	}
	if ( matModData.m_texSizeSlot >= 0 )
	{
		mmConstants.SetValue( matModData.m_texSizeSlot, texSizeParams );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_CustomStructBuffer(const SMatModifierDataCustomStructBuffer &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if (GT_Mesh != geoBatchData.m_type)
	{
		return;
	}

	if(matModData.m_slot >= 0)
	{
		GpuApi::BufferRef buffer = geoBatchData.m_meshData.m_mesh->GetCustomDataBuffer();

		if ( buffer.isNull() )
		{
			GpuApi::BufferDesc desc;
			desc.size = 16;
			desc.structureStride = 16;
			desc.category = GpuWrapApi::BCC_Structured;
			desc.usage = GpuApi::BUT_Default;
			desc.group = GpuWrapApi::BufferGroup::MeshCustom;
			static GpuApi::BufferRef dummyData = GpuApi::CreateBuffer( desc, GpuApi::BufferInitData::ClearToZero() );
			buffer = dummyData;
		}

		GpuApi::BindBufferSRV( buffer, matModData.m_slot, GpuWrapApi::VertexShader );
	}
}

static void MaterialModifierProcess_DestructionRegions(const SMatModifierDestructionRegions &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const SProxyBatchData& proxyBatchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( !proxyBatchData.GetProxy() )
	{
		return;
	}

	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( proxyBatchData.GetProxy() );

	Int8 count = 0;

	if (matModData.m_slot >= 0)
	{
		GpuApi::BufferRef buffer = meshProxy->GetDestructionRegionsDataBuffer();
		if ( buffer.isNull() )
		{
			GpuApi::BufferDesc desc;
			desc.size = 16;
			desc.structureStride = 16;
			desc.category = GpuWrapApi::BCC_Structured;
			desc.usage = GpuApi::BUT_Default;
			desc.group = GpuWrapApi::BufferGroup::MeshCustom;
			static GpuApi::BufferRef dummyData = GpuApi::CreateBuffer( desc, GpuApi::BufferInitData::ClearToZero() );

			buffer = dummyData;
		}
		GpuApi::BindBufferSRV( buffer, matModData.m_slot, GpuWrapApi::VertexShader );

		const GpuApi::BufferDesc& bufferDesc = GpuApi::GetBufferDesc( buffer );
		Uint32 numElements = bufferDesc.size / bufferDesc.structureStride;
		count = (Int8)numElements / sizeof( Float );
	}

	if (matModData.m_count >= 0)
	{		
		mmConstants.SetValue(matModData.m_count, Vector4(count, 0, 0));
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_MotionMatrix(const SMatModifierDataMotionMatrixParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const IRenderProxyDrawable* proxy = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy();
	RED_FATAL_ASSERT( proxy );
	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( proxy );
	
	Matrix motionMatrix = proxy->GetPackedLocalToWorld();
	const Float multiplier = proxy->GetMotionBlurMultiplier();

	if ( multiplier > 0.0f )
	{
		const ProxyMotionData* motionData = meshProxy->GetMotionData();

		RenderProxyInstanceMatrix instanceMatrix;
		if ( motionData && motionData->GetInstanceMatrix( instanceMatrix ) )
		{
			motionMatrix = instanceMatrix.GetPackedMatrix();

			if ( multiplier < 1.0f )
			{
				// #TODO this is slow, but at least we do it only for proxies that have multiplier greater than 0 and smaller than 1

				const Matrix prevMatrix = instanceMatrix.GetUnpackedMatrix();
				const Matrix curMatrix = proxy->GetLocalToWorld();

				Vector4 prevTranslation, prevScale, curTranslation, curScale;
				Quaternion prevRotation, curRotation;
				Matrix::SLOW_Decompose( prevMatrix, prevTranslation, prevRotation, prevScale );
				Matrix::SLOW_Decompose( curMatrix, curTranslation, curRotation, curScale );

				const Quaternion rotation = Quaternion::Slerp( curRotation, prevRotation, multiplier );
				const Vector4 scale = Vector4::Lerp( curScale, prevScale, multiplier );

				const Matrix rotMatrix = rotation.ToMatrix();
				motionMatrix.X = rotMatrix.X * scale.X;
				motionMatrix.Y = rotMatrix.Y * scale.Y;
				motionMatrix.Z = rotMatrix.Z * scale.Z;
			}
		}
	}

	if ( matModData.m_slot >= 0 )
	{
		mmConstants.SetValue( matModData.m_slot, motionMatrix );	
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_Eye(const SMatModifierEyeParams &matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const IRenderProxyDrawable* proxy = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() ).GetProxy();
	
	CRenderSkinningData* skinData = nullptr;
	Matrix localToWorld = proxy ? proxy->GetLocalToWorld() : Matrix::IDENTITY();
	if ( const CRenderProxy_Mesh* meshProxy = static_cast< const CRenderProxy_Mesh* >( proxy ) )
	{
		skinData = meshProxy->GetSkinningData().Get();
	}

	if ( matModData.m_slotLeft >= 0 )
	{
		Matrix eyeMatrixLeft;
		if ( skinData && skinData->GetCustomMatrix( SDCM_EyeOrientationLeft, eyeMatrixLeft ) )
		{
			eyeMatrixLeft = eyeMatrixLeft * localToWorld;
		}
		else
		{
			eyeMatrixLeft = localToWorld;
		}
		mmConstants.SetValue( matModData.m_slotLeft, eyeMatrixLeft.Transposed() );
	}
	if ( matModData.m_slotRight >= 0 )
	{
		Matrix eyeMatrixRight;
		if ( skinData && skinData->GetCustomMatrix( SDCM_EyeOrientationRight, eyeMatrixRight ) )
		{
			eyeMatrixRight = eyeMatrixRight * localToWorld;
		}
		else
		{
			eyeMatrixRight = localToWorld;
		}
		mmConstants.SetValue( matModData.m_slotRight, eyeMatrixRight.Transposed() );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_Skin( const SMatModifierSkinParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );

	const SRenderNodeImplContext& rctx = *params.m_renderNodeContext;

#ifdef RED_PLATFORM_CONSOLE
	const Bool hasWrinkles = false; // TURN OFF until Wrinklemaps are fixed not to hang the GPU on PS4
#else
	const Bool hasWrinkles = rctx.Test( RFF_Wrinklemaps );
#endif // RED_PLATFORM_CONSOLE

	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( batchData.GetProxy() );
	RED_ASSERT( meshProxy );

	RenderBindManager& stateManager = GetRenderer()->GetStateManager();

	Uint32 floatTracksCount = 0;
	if ( matModData.m_floatTracksCBSlot >= 0 )
	{
		GpuApi::BufferRef buffer = stateManager.m_floatTracksConstantBuffer;

		SFloatTrackDataConstants dataToCopy;
		CRenderFloatTrackData* floatTrackData = meshProxy->GetFloatTrackData().Get();
		if ( hasWrinkles && floatTrackData )
		{
			const void* readData = floatTrackData->GetReadData();
			const Float* readDataFloat = static_cast< const Float* >( readData );
			floatTracksCount = Min( floatTrackData->GetFloatCount(), SFloatTrackDataConstants::NumFloatTracks );
			for ( Uint32 i = 0; i < floatTracksCount; ++i )
			{
				Float v = readDataFloat[ i ];
				dataToCopy.floatTrackData[ i ] = Vector4( v, v, v );
			}
		}
		GpuApi::UpdateBuffer( buffer, dataToCopy );
		GpuApi::BindConstantBuffer( matModData.m_floatTracksCBSlot, buffer, GpuWrapApi::VertexShader );
	}

	if ( matModData.m_regionsCBSlot >= 0 )
	{
		stateManager.BindTextureRegions( matModData.m_regionsCBSlot );
	}

	const Uint32 maxInfluence			= hasWrinkles ? 4 : 0;
	const Uint32 textureRegionsCount	= hasWrinkles ? stateManager.GetTextureRegionsCount() : 0;

	RED_ASSERT( matModData.m_wrinklesParamsSlot >= 0 );
	if ( matModData.m_wrinklesParamsSlot >= 0 )
	{
		Vector4 wrinklesInfo( (Float)textureRegionsCount, (Float)floatTracksCount, (Float)maxInfluence );
		mmConstants.SetValue( matModData.m_wrinklesParamsSlot, wrinklesInfo );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_Dismemberment(const SMatModifierDismembermentParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants)
{
	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( batchData.GetProxy() );
	
	RED_ASSERT(meshProxy);
	RED_ASSERT( matModData.m_dismParamsSlot >= 0 );
	
	Vector4 dismParams = Vector4::ZEROS();
	if( meshProxy->HasDismembermentData() )
	{
		const RenderDismembermentData& dismData = meshProxy->GetDismembermentData();
		if( dismData.m_numCullObjects && !dismData.m_cbuffer.isNull() )
		{
			dismData.UploadData();
			dismParams = RenderDismPackParams( dismData.m_cbufferData );
			GpuApi::BindConstantBuffer( matModData.m_cullObjectsCBSlot, dismData.m_cbuffer, GpuWrapApi::VertexShader );
		}
		else
		{
			GpuApi::BindConstantBuffer( matModData.m_cullObjectsCBSlot, RenderDismembermentData::s_dummyCBuffer, GpuWrapApi::VertexShader );
		}
	}
	else
	{
		GpuApi::BindConstantBuffer( matModData.m_cullObjectsCBSlot, RenderDismembermentData::s_dummyCBuffer, GpuWrapApi::VertexShader );
	}

	mmConstants.SetValue( matModData.m_dismParamsSlot, dismParams );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_FloatTracks( const SMatModifierFloatTracksParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );

	const SRenderNodeImplContext& rctx = *params.m_renderNodeContext;

	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( batchData.GetProxy() );
	RED_ASSERT( meshProxy );

	RenderBindManager& stateManager = GetRenderer()->GetStateManager();

	Uint32 floatTracksCount = 0;
	if ( matModData.m_floatTracksCBSlot >= 0 )
	{
		GpuApi::BufferRef buffer = stateManager.m_floatTracksConstantBuffer;

		SFloatTrackDataConstants dataToCopy;
		CRenderFloatTrackData* floatTrackData = meshProxy->GetFloatTrackData().Get();

		if ( floatTrackData )
		{
			const void* readData = floatTrackData->GetReadData();
			const Float* readDataFloat = static_cast<const Float*>(readData);
			floatTracksCount = Min( floatTrackData->GetFloatCount(), SFloatTrackDataConstants::NumFloatTracks );
			for ( Uint32 i = 0; i < floatTracksCount; ++i )
			{
				Float v = readDataFloat[i];
				dataToCopy.floatTrackData[i] = Vector4( v, v, v );
			}
		}
		GpuApi::UpdateBuffer( buffer, dataToCopy );
		GpuApi::BindConstantBuffer( matModData.m_floatTracksCBSlot, buffer, GpuWrapApi::VertexShader );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_Rain( const SMatModifierRainParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const CRenderProxy_Mesh* meshProxy = static_cast< const CRenderProxy_Mesh* >( batchData.GetProxy() );

	RED_ASSERT( meshProxy );

	if ( matModData.m_rainCBSlot >= 0 )
	{
		Vector4 rainParams = Vector4::ZEROS();
		if ( meshProxy->HasRainData() )
		{
			const RenderRainData& rainData = meshProxy->GetRainData();
			rainParams.X = rainData.m_rainIntensity;
			rainParams.Y = rainData.m_rainMoisture;
		}
		mmConstants.SetValue( matModData.m_rainCBSlot, rainParams );
	}
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_PlanarReflections( const SMatModifierPlanarReflectionsParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	if( params.m_renderNodeContext->Test( RFF_ScreenSpacePlanarReflections ) )
	{
		if( auto camData = params.m_renderNodeContext->GetCameraData() )
		{
			const auto& ssrData = camData->GetCustomData<SSRCustomData>();

			GpuApi::BindTexture( 5, ssrData.m_currPlanarResult, GpuWrapApi::PixelShader );
			GpuApi::BindTexture( 6, ssrData.m_currPlanarFade, GpuWrapApi::PixelShader );
		}
	}
	else
	{
		auto black = GpuApi::GetInternalTexture( GpuApi::INTERTEX_Zero );

		GpuApi::BindTexture( 5, black, GpuWrapApi::PixelShader );
		GpuApi::BindTexture( 6, black, GpuWrapApi::PixelShader );
	}

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

Uint32 NextPowerOfTwo( Uint32 x )
{
	x |= ( x >> 1 );
	x |= ( x >> 2 );
	x |= ( x >> 4 );
	x |= ( x >> 8 );
	x |= ( x >> 16 );
	return x + 1;
}

// [skperf] round 19 mirrorOccl (opt-in ?mirrorOccl=1, default 0 = original: the mirror view never uses occlusion).
// The mirror's reflected camera sits behind the mirror wall, so the wall's own occluders (between that camera and the
// mirror plane) would hide the whole reflection; that is why the engine turns occlusion off for mirrors. With the
// switch on, the mirror camera request asks for occlusion, and the collector's OCCLUSION camera (not the frustum or
// the render camera) gets its near plane pushed to beyond the farthest point of the mirror plane inside the view:
// every occluder nearer than that is clipped from the occlusion buffer. Any point deeper than that bound lies on the
// visible side of the mirror plane on every ray through the mirror, so only real reflected-room occluders remain.
// Bound = max view depth of the 4 mirror quad corners + the quad diagonal (the frustum is the screen bbox of the quad,
// whose plane footprint extends past the quad); skipped (occlusion stays off) when the view is grazing
// (|cos| < 0.5) or the bound passes the far plane. The collector looks the camera up by its exact position.
namespace cpMirrorOccl
{
	std::atomic<Int32> s_on{ -1 };
	Bool On()
	{
		Int32 v = s_on.load( std::memory_order_acquire );
		if ( v >= 0 ) return v != 0;
#if defined( RED_PLATFORM_WEB )
		v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'mirrorOccl' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
#else
		v = 0;
#endif
		Int32 expected = -1;
		if ( s_on.compare_exchange_strong( expected, v ) ) { std::fprintf( stderr, "CP2077 skperf switches: mirrorOccl=%d\n", v ); return v != 0; }
		return expected != 0;
	}
	struct Entry { WorldPosition pos; Float occlNear = 0; Bool ok = false; Bool used = false; };
	constexpr Uint32 kEntries = 8;
	Entry s_entries[ kEntries ];
	Uint32 s_next = 0;
	std::mutex s_lock;
	std::atomic<unsigned long long> s_published{ 0 }, s_grazing{ 0 }, s_applied{ 0 }, s_disabled{ 0 };
	void Publish( const WorldPosition& pos, Float occlNear, Bool ok )
	{
		std::lock_guard<std::mutex> guard( s_lock );
		for ( Entry& e : s_entries ) if ( e.used && e.pos == pos ) { e.occlNear = occlNear; e.ok = ok; return; }
		Entry& e = s_entries[ s_next++ % kEntries ];
		e.pos = pos; e.occlNear = occlNear; e.ok = ok; e.used = true;
	}
}
// Called by the collector (renderCollector.cpp overlay) for every camera's scene query. Returns false for cameras
// that are not a published mirror camera (no change). For a mirror camera: ok -> use outNear as the occlusion near
// plane; !ok -> the caller must turn occlusion off for this query.
Bool CpMirrorOcclusionNear( const WorldPosition& cameraPosition, Float& outNear, Bool& outOk )
{
	if ( cpMirrorOccl::s_on.load( std::memory_order_relaxed ) <= 0 ) return false;
	std::lock_guard<std::mutex> guard( cpMirrorOccl::s_lock );
	for ( const auto& e : cpMirrorOccl::s_entries )
		if ( e.used && e.pos == cameraPosition )
		{
			outNear = e.occlNear; outOk = e.ok;
			( e.ok ? cpMirrorOccl::s_applied : cpMirrorOccl::s_disabled ).fetch_add( 1, std::memory_order_relaxed );
			return true;
		}
	return false;
}

// [skperf] round 48 mirrorCull (opt-in ?mirrorCull=<milliradians>, default 0 = off): in the bar the mirror view's
// GBuffer/velocity/decal lists record ~5x the draws of the main view (tc62 timeline: ~26 ms each on mirror frames vs
// 4-6 ms), because the mirror camera has no occlusion and sees the whole reflected room. With the switch, objects whose
// angular radius seen from the MIRROR camera is below the threshold (bounding-box half-diagonal / distance < mrad/1000)
// are dropped from that camera's visibility query only (visibilityQuerySingleFrustum.cpp). Main view, shadows and
// every other camera are unchanged. Mirror cameras are recognised by their exact position, as for mirrorOccl.
namespace cpMirrorCull
{
	std::atomic<Int32> s_mrad{ -1 };
	Int32 Mrad()
	{
		Int32 v = s_mrad.load( std::memory_order_acquire );
		if ( v >= 0 ) return v;
#if defined( RED_PLATFORM_WEB )
		v = MAIN_THREAD_EM_ASM_INT( { try { const n = Math.floor( Number( new URLSearchParams( location.search ).get( 'mirrorCull' ) || 0 ) ); return ( Number.isFinite( n ) && n > 0 ) ? Math.min( n, 200 ) : 0; } catch ( e ) { return 0; } } );
#else
		v = 0;
#endif
		Int32 expected = -1;
		if ( s_mrad.compare_exchange_strong( expected, v, std::memory_order_acq_rel ) ) { std::fprintf( stderr, "CP2077 skperf switches: mirrorCull=%d (mrad)\n", v ); return v; }
		return expected;
	}
	// [skperf] round 49: mirror cameras are identified by their camera hash (the request's m_cameraName) and builder
	// pointer, both of which the collector sees in its SCameraData. Round 48 matched the camera POSITION and never hit
	// online (tc63: no 'mirrorCull' line in 87 s of mirror rendering): the dynamic camera the collector gets is not
	// positioned exactly at the builder's context.position.
	constexpr Uint32 kEntries = 8;
	Uint64 s_hash[ kEntries ] = {};
	const void* s_builder[ kEntries ] = {};
	Uint32 s_next = 0;
	std::mutex s_lock;
	std::atomic<unsigned long long> s_published{ 0 }, s_matched{ 0 };
	std::atomic<Int32> s_reported{ 0 };
	std::atomic<double> s_firstPublish{ 0.0 };
	void Publish( Uint64 hash, const void* builder )
	{
		s_published.fetch_add( 1, std::memory_order_relaxed );
		{
			double expected = 0.0;
			s_firstPublish.compare_exchange_strong( expected, emscripten_get_now() );
		}
		std::lock_guard<std::mutex> guard( s_lock );
		for ( Uint32 i = 0; i < kEntries; ++i ) if ( ( hash && s_hash[ i ] == hash ) || ( builder && s_builder[ i ] == builder ) ) { s_hash[ i ] = hash; s_builder[ i ] = builder; return; }
		const Uint32 i = s_next++ % kEntries;
		s_hash[ i ] = hash; s_builder[ i ] = builder;
	}
}
// [skperf] round 50 mirrorFrustum (opt-in ?mirrorFrustum=1; also implied by ?mirrorCull=N): tc65 showed why rounds 48/49
// never matched: the engine sets a mirror camera's m_cullingBox (renderFrameCamera.cpp, from the builder's authored
// mirror culling OBB -> a cube around it), and a camera with a culling box is collected by a BOX query (everything in
// the cube, no frustum test at all) - the frustum branch where the match was done never ran for the mirror. With the
// switch, such a mirror camera is collected by its own single-frustum query instead, with the culling box applied as an
// extra per-object filter (visibilityQuerySingleFrustum.cpp). The mirror camera's projection is an ordinary off-axis
// perspective (view along the mirror normal, zoom/shift framing the mirror quad, near just in front of the mirror, far
// +24 m), so its frustum is exactly the volume the mirror render can show: frustum AND box gives the same image as the
// box alone, without collecting the box contents outside the view.
namespace cpMirrorFrustum
{
	std::atomic<Int32> s_on{ -1 };
	Bool On()
	{
		Int32 v = s_on.load( std::memory_order_acquire );
		if ( v < 0 )
		{
#if defined( RED_PLATFORM_WEB )
			// [skperf] round 51: DEFAULT ON (tc66 Kabuki: 25 -> 35 fps, reflection identical); ?mirrorFrustum=0 = old box-only query.
			const Int32 read = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'mirrorFrustum' ) === '0' ? 0 : 1; } catch ( e ) { return 1; } } );
#else
			const Int32 read = 1;
#endif
			const Int32 value = ( read || cpMirrorCull::Mrad() > 0 ) ? 1 : 0;
			Int32 expected = -1;
			if ( s_on.compare_exchange_strong( expected, value, std::memory_order_acq_rel ) ) { std::fprintf( stderr, "CP2077 skperf switches: mirrorFrustum=%d\n", value ); v = value; }
			else v = expected;
		}
		return v > 0;
	}
	std::atomic<Int32> s_dumped{ 0 };
}
// renderCollector.cpp (culling-box branch): true when this collect is a published mirror camera (hash or builder) and
// mirrorFrustum/mirrorCull is on; outAngle = mirrorCull angle in radians (0 = no size cut). The first 6 culling-box
// collects with the switch on are dumped next to the published mirror ids (one-shot diagnostic).
Bool CpMirrorQueryFor( Uint64 cameraHash, const void* builder, Float& outAngle )
{
	outAngle = 0.f;
	if ( cpMirrorFrustum::s_on.load( std::memory_order_relaxed ) <= 0 ) return false;
	Bool match = false;
	Uint64 pubHash[ cpMirrorCull::kEntries ]; const void* pubBuilder[ cpMirrorCull::kEntries ];
	{
		std::lock_guard<std::mutex> guard( cpMirrorCull::s_lock );
		for ( Uint32 i = 0; i < cpMirrorCull::kEntries; ++i )
		{
			pubHash[ i ] = cpMirrorCull::s_hash[ i ]; pubBuilder[ i ] = cpMirrorCull::s_builder[ i ];
			match = match || ( cameraHash && cpMirrorCull::s_hash[ i ] == cameraHash ) || ( builder && cpMirrorCull::s_builder[ i ] == builder );
		}
	}
	const Int32 dumped = cpMirrorFrustum::s_dumped.fetch_add( 1, std::memory_order_relaxed );
	if ( dumped < 6 )
		std::fprintf( stderr, "CP2077 skperf mirrorFrustum collect %d: cameraHash=%llx builder=%p matched=%d | published mirrors: %llx/%p %llx/%p %llx/%p %llx/%p\n",
			dumped, static_cast<unsigned long long>( cameraHash ), builder, match ? 1 : 0,
			static_cast<unsigned long long>( pubHash[ 0 ] ), pubBuilder[ 0 ], static_cast<unsigned long long>( pubHash[ 1 ] ), pubBuilder[ 1 ],
			static_cast<unsigned long long>( pubHash[ 2 ] ), pubBuilder[ 2 ], static_cast<unsigned long long>( pubHash[ 3 ] ), pubBuilder[ 3 ] );
	if ( !match ) return false;
	if ( cpMirrorCull::s_matched.fetch_add( 1, std::memory_order_relaxed ) == 0 )
		std::fprintf( stderr, "CP2077 skperf mirrorFrustum: first mirror camera collect matched (hash=%llx), frustum query in use\n", static_cast<unsigned long long>( cameraHash ) );
	const Int32 mrad = cpMirrorCull::s_mrad.load( std::memory_order_relaxed );
	outAngle = mrad > 0 ? Float( mrad ) * 0.001f : 0.f;
	return true;
}

class CMirrorCameraBuilder : public CRenderFrameCameraStorage::IRenderCameraBuilder, red::NonCopyable
{
	Uint64							m_originCameraHash;
	TRenderPtr<IRenderProxyBase>	m_mirrorProxy;

public:
	Int32							m_lastRenderWidth = 0;
	Int32							m_lastRenderHeight = 0;

	Matrix							m_compensatedWorldToMirror;

private:

	static Matrix GetOriginCameraRelativeMirrorTransform( const CRenderCamera& originCamera, IRenderProxyBase* mirrorMeshProxy )
	{
		RED_ASSERT( mirrorMeshProxy, "Mirror mesh proxy is expected" );
		RED_ASSERT( RPT_Mesh == mirrorMeshProxy->GetType(), "Mirror is intended for use with meshes" );

		RenderProxyInstanceMatrix mirrorMtx = mirrorMeshProxy->m_instanceMatrix;
		mirrorMtx.m_03 -= originCamera.GetPosition().x;
		mirrorMtx.m_13 -= originCamera.GetPosition().y;
		mirrorMtx.m_23 -= originCamera.GetPosition().z;

		const Matrix result = mirrorMtx.GetUnpackedMatrix();
		return result;
	}


public:
	Bool BuildCamera( BuildResult& buildResult, const CRenderFrameCameraStorage::SCameraData& originCameraData )
	{
		if ( !CheckIfWorthBuilding( originCameraData ) )
		{
			// The mirror surface will not be visible so it is not worth to actually build this camera
			RED_LOG_WARNING("MirrorCameraBuilder: Building mirror camera, but origin camera is behind the mirror.");
			return false;
		}

		const CRenderCamera& originCamera = originCameraData.GetRenderCamera();
		const Matrix mirrorLocalToRelWorld = GetOriginCameraRelativeMirrorTransform( originCamera, m_mirrorProxy.Get() );
		const Vector4 planeNormal = mirrorLocalToRelWorld.TransformVector( Vector3( 0, 0, 1 ) ).Normalized3();

		// build new camera and calc resolution
		{
			const Vector4 camForward = originCamera.GetCameraForward();
			const Float d0 = planeNormal.Dot3(camForward);
			const Vector4 reflectionV = camForward - Vector4(planeNormal).Mul3(2.0f*d0);

			// Reflect camera position across mirror. We're already in originCamera-relative coordinates, so don't need
			// to factor in originCamera.GetPosition -- it'd be (0,0,0) here.
			const Vector4 v = /*originCamera.GetPosition()*/ - mirrorLocalToRelWorld.GetTranslation();
			const Float d1 = planeNormal.Dot3( v );
			const Vector4 relativeReflectionP = /*originCamera.GetPosition()*/ - Vector4(planeNormal).Mul3(2.0f*d1);

			const Quaternion rot = Quaternion::MakeFromDirectionVector( reflectionV );

			// Calculate view matrix
			const Vector4 mirrorWorldP = mirrorLocalToRelWorld.GetTranslation() + originCamera.GetPosition().AsVector4();
			const Vector4 worldSpaceClipPlane( planeNormal.X, planeNormal.Y, planeNormal.Z, -planeNormal.Dot3( mirrorWorldP ) );

			CRenderCamera tmpCamera = originCamera;
			// Clear any per-pixel jitter from main camera.
			tmpCamera.SetSubpixelOffset( 0, 0, 1, 1 );	
			tmpCamera.Set( WorldPosition(relativeReflectionP), rot );
			tmpCamera.SetShift( tmpCamera.GetShift() * Vector2( -1.f, 1.f ) );
			
			Box bbox;
			if ( !ResolutionScaleHelper( tmpCamera, mirrorLocalToRelWorld, bbox ) )
			{
				RED_LOG_WARNING( "MirrorCameraBuilder: Building mirror camera, but clipped mirror bbox was empty." );
				return false;
			}
			const Vector2 size = bbox.CalcSize();

			// Invert zoom in case of ortho (because it works there in opposite fashion:)
			Float _zoomOrigCam = originCamera.GetZoom();
			if ( originCamera.IsOrtho() )
				_zoomOrigCam = 1.f / _zoomOrigCam;

			Float newZoom = _zoomOrigCam * 2.f / size.Y;

			const Float newAspect = size.X / size.Y * originCamera.GetAspect();
			const Vector2 shift = -((bbox.CalcCenter() / _zoomOrigCam + originCamera.GetShift() * Vector2( 1.f, -1.f ) / _zoomOrigCam) * Vector2( originCamera.GetAspect() / newAspect, 1.f ) * newZoom);

			// We do not want to collect too many proxies so we restrict mirror's drawing distance to 20 meters for now.
			const Float fixedFarPlane{ 24.0f };
			const Float mirrorDist = v.Mag3();
			const Float newNear = mirrorDist - Max(size.X, size.Y); // make sure everything is inside the view even if it's close to the mirror
			const Float newFar = mirrorDist + fixedFarPlane; // all in all this is a little bit more than 20m but that shouldn't be an issue

			// Decide on the resolution we want the mirrors to render to. This is important choice for saving some GPU time.
			Float qualityLevelBasedResolutionScale = Config::GGlobalRenderingSettings.MirrorFinalResolutionScale;
			Float qualityLevelBasedInternalResolutionScale = Config::GGlobalRenderingSettings.MirrorInternalResolutionScale;
#ifndef RED_CONFIGURATION_FINAL
			if ( auto devConfig = GetRenderer()->GetDevConfig() )
			{
				if ( devConfig->Mirror.IsEnabled() )
				{
					qualityLevelBasedResolutionScale = devConfig->Mirror.m_finalResolutionScale;
					qualityLevelBasedInternalResolutionScale = devConfig->Mirror.m_internalResolutionScale;
				}
			}
#endif
			
			// Evaluate output resolution.
			// We want to avoid frequent resolution changes - this helps maintaining temporal stability of the mirror image
			// If difference was small (less than 16 pixels) then ignore that change (use dimensions from previous frame)
			{
				// We need to base our calculation on the resolution of the parent camera.
				// Now, there is a twist: to really have control over how sharp the mirror image apperas, we need to take info account the internal resolution scaling of the main camera.
				// There are two possible scenarios:
				// 1. We upscale with DLSS. DLSS happens to upscale at the temporalAA stage, which means that mirror is rendered after upscaling already happens.
				//    Because of that, IF we are using DLSS, we need to based our calculations on the FINAL render dimensions (the ones that land on the final rendertarget/backbuffer.
				// 2. We upscale with CAS. CAS happens to upscale almost at the end of the frame - AFTER the mirror is already rendered.
				//	  In this case, we can base our calculations here on an INTERNAL render dimensions of the main camera. This is because the mirror contents will get upscaled together with the main camera contents.
				Uint32 originRenderHeight = originCameraData.GetFeatures().Get( RFF_DLSS ) ? originCameraData.GetResolutionState().GetTargetHeight() : originCameraData.GetResolutionState().GetInternalHeight();
				if ( originRenderHeight <= 0 )
				{
					RED_LOG_WARNING( "MirrorCameraBuilder: Building mirror camera, but origin camera's render height is invalid (%u).", originRenderHeight );
					return false;
				}

#ifdef USE_RAY_TRACING
				if (originCameraData.GetFeatures().Get( RFF_DLSS ))
				{
					// Debug override
					if (Config::cvDLSSMirrorScaling.Get() >= 0.0f)
					{
						originRenderHeight = static_cast<Uint32>(originRenderHeight * Config::cvDLSSMirrorScaling.Get());
					}
					else if (Config::cvDLSSEnableMirrorScaling.Get())
					{
						originRenderHeight = originCameraData.GetResolutionState().GetInternalHeight();
					}
				}
#endif

				Uint32 newRenderHeight = (Uint32)( ( originRenderHeight * _zoomOrigCam / newZoom + 0.01f ) * qualityLevelBasedResolutionScale );
				Uint32 newRenderWidth = (Uint32)( ( newRenderHeight * newAspect ) + 0.5f );

				// Let's not render to some insanely small resolution.
				newRenderWidth = Max( 128u, newRenderWidth );
				newRenderHeight = Max( 128u, newRenderHeight );

				const Int32 widthDifference = std::abs( (Int32)newRenderWidth - m_lastRenderWidth );
				const Int32 heightDifference = std::abs( (Int32)newRenderHeight - m_lastRenderHeight );
				if ( widthDifference > 16 || heightDifference > 16 )
				{
					m_lastRenderWidth = (Int32)newRenderWidth;
					m_lastRenderHeight = (Int32)newRenderHeight;
				}
				else
				{
					newRenderWidth = m_lastRenderWidth;
					newRenderHeight = m_lastRenderHeight;
				}

				buildResult.outRenderHeight = newRenderHeight;
				buildResult.outRenderWidth = newRenderWidth;

				buildResult.outInternalRenderWidth = Uint32( qualityLevelBasedInternalResolutionScale * buildResult.outRenderWidth );
				buildResult.outInternalRenderHeight = Uint32( qualityLevelBasedInternalResolutionScale * buildResult.outRenderHeight );
			}


			// Compute jitter offset for our newly created camera. We want it to get temporal AA like any other camera does.
			{
				Float temporalJitterX = 0;
				Float temporalJitterY = 0;
				Uint32 temporalJitterIndex = 0;

				temporalJitterIndex = originCameraData.m_temporalJitterGen.m_currIndex;
				originCameraData.m_temporalJitterGen.CalcJitter( temporalJitterX, temporalJitterY );

				buildResult.outCamera.SetSubpixelOffset( temporalJitterX, temporalJitterY, buildResult.outRenderWidth, buildResult.outRenderHeight, temporalJitterIndex % TEMPORAL_AA_NUM_SAMPLES );
			}

			// Invert zoom in case of ortho (because it works there in opposite fashion:)
			if ( originCamera.IsOrtho() )
				newZoom = 1.f / newZoom;

			// All parameters are ready. Create the camera!
			{
				CRenderCamera::CreationContext context;
				{
					const WorldPosition reflectionP = originCamera.GetPosition() + relativeReflectionP;

					context.position = reflectionP;
					context.rotation = rot;
					context.fov = originCamera.GetFOV();
					context.aspect = newAspect;
					context.nearPlane = newNear;
					context.farPlane = newFar;
					context.zoom = newZoom;
					context.shift = shift;
					context.clippingPlane = worldSpaceClipPlane;
					context.isReversedProjection = originCamera.IsReversedProjection();
					context.isCameraCut = originCamera.IsCameraCut() || originCameraData.m_resetRequested;
				}
				buildResult.outCamera = CRenderCamera( context );

				// [skperf] round 19 mirrorOccl: occlusion near bound for this mirror camera (see cpMirrorOccl)
				if ( cpMirrorOccl::On() )
				{
					const Vector4 fwd = reflectionV.Normalized3();
					const Float cosView = MAbs( planeNormal.Dot3( fwd ) );
					Float maxDepth = -RED_FLT_MAX;
					Vector4 corners[ 4 ];
					const Float cx[ 4 ] = { -1.f, 1.f, 1.f, -1.f }, cy[ 4 ] = { -1.f, -1.f, 1.f, 1.f };
					for ( Uint32 i = 0; i < 4; ++i )
					{
						corners[ i ] = mirrorLocalToRelWorld.TransformPoint( Vector4( cx[ i ], cy[ i ], 0.f, 1.f ) );
						maxDepth = Max( maxDepth, ( corners[ i ] - relativeReflectionP ).Dot3( fwd ) );
					}
					const Float diag = Max( ( corners[ 0 ] - corners[ 2 ] ).Mag3(), ( corners[ 1 ] - corners[ 3 ] ).Mag3() );
					const Float occlNear = maxDepth + diag;
					const Bool ok = cosView >= 0.5f && occlNear > newNear && occlNear < newFar - 1.0f;
					cpMirrorOccl::Publish( context.position, occlNear, ok );
					( ok ? cpMirrorOccl::s_published : cpMirrorOccl::s_grazing ).fetch_add( 1, std::memory_order_relaxed );
				}

				// Generate a world-to-screen that's using the originCamera-relative position. We'll use that in the
				// shader to get a higher precision UV (using camera-compensated world coord, instead of absolute world
				// coord).
				// #TODO: don't build the full camera, we only need a small part of it.
				{
					context.position = WorldPosition(relativeReflectionP);
					CRenderCamera compensatedCamera( context );
					m_compensatedWorldToMirror = compensatedCamera.GetWorldToScreen();
				}
			}

			// Last but not least, if it is available - set up a culling box. It can greatly help reduce the CPU and GPU workload of rendering the contents of mirror.
			{
				ERenderProxyType proxyType = m_mirrorProxy->GetType();
				buildResult.cullingBox = Box::EMPTY();
				if ( proxyType == RPT_Mesh )
				{
					const CRenderProxy_Mesh* meshProxyPtr = static_cast<const CRenderProxy_Mesh*>( m_mirrorProxy.Get() );
					RenderProxyCustomData_Mirror* mirrorParams = meshProxyPtr->GetCustomData<RenderProxyCustomData_Mirror>();
					//RED_FATAL_ASSERT(mirrorParams == nullptr);
					if ( mirrorParams && !mirrorParams->m_mirrorCullingBox.IsEmpty() )
					{
						//Convert OBB to AABB
						OrientedBox obb = mirrorParams->m_mirrorCullingBox;
						Vector4 obbCenter = obb.GetMassCenter();
						Vector3 obbMin = obb.m_position.AsVector3();
						Float obbRadius = ( obbCenter.AsVector3() - obbMin ).Mag();
						buildResult.cullingBox = Box( obbCenter, obbRadius );
					}
				}
			}
		}

		// Camera built successfully. We calculated that it makes sense to render it (it's visible etc.) and we know what it exactly needs to be.
		return true;
	}

	virtual Bool CheckIfWorthBuilding( const CRenderFrameCameraStorage::SCameraData& originCameraData ) const override final
	{
		const CRenderCamera& originCamera = originCameraData.GetRenderCamera();
		const Matrix mirrorLocalToRelWorld = GetOriginCameraRelativeMirrorTransform( originCamera, m_mirrorProxy.Get() );
		const Vector4 planeNormal = mirrorLocalToRelWorld.TransformVector( Vector3( 0, 0, 1 ) ).Normalized3();

		// Make sure we're at the correct side of the mirror
		// we're in originCamera-relative coordinates, so don't need to factor in originCamera.GetPosition -- it'd be 0.
		const Float epsilon = 0.001f;
		const Float a = planeNormal.Dot3( /*originCamera.GetPosition()*/ - mirrorLocalToRelWorld.GetTranslation() );
		return a > epsilon;
	}

public:
	CMirrorCameraBuilder ( Uint64 originCameraHash, const IRenderProxyBase* mirrorProxy )
	{
		m_originCameraHash	= originCameraHash;
		m_mirrorProxy		= TRenderPtr<IRenderProxyBase>::MakePtr( const_cast<IRenderProxyBase*>( mirrorProxy ) );
	}

	virtual ~CMirrorCameraBuilder ()
	{
	}

public:
	virtual Uint64 GetOriginCameraHash() const override
	{
		return m_originCameraHash;
	}

	virtual const RenderFeatureSet& GetAllowedFeatureSet() const override
	{
		static const RenderFeatureSet allowedFeatures = []()
		{
			RenderFeatureSet rff = AllowedRenderFeatureSet_DefaultOnScreen();

			rff.Set( RFF_HUD, false );
			rff.Set( RFF_Selection, false );
			rff.Set( RFF_AntialiasingTXAA, true );
			rff.Set( RFF_VelocityBuffer, true );
			rff.Set( RFF_AntialiasingFXAA, false );
			rff.Set( RFF_Tonemapping, false );
			rff.Set( RFF_ChromaticAberration, false );
			rff.Set( RFF_Vignette, false );
			rff.Set( RFF_Bloom, false );
			rff.Set( RFF_ImageBasedFlares, false );
			rff.Set( RFF_ScreenSpaceReflections, false );
			rff.Set( RFF_ScreenSpacePlanarReflections, false );
			rff.Set( RFF_DepthOfField, false );
			rff.Set( RFF_CascadeShadows, false );
			rff.Set( RFF_DistantShadows, false );
			rff.Set( RFF_ContactShadows_Global, false );
			rff.Set( RFF_DLSS, false );
			rff.Set( RFF_ContrastAdaptiveSharpening, true );

#ifdef USE_RAY_TRACING
			rff.Set( RFF_RayTracedAmbientOcclusion, false );
			rff.Set( RFF_RayTracedDiffuseIllumination, false );
			rff.Set( RFF_RayTracedReflection, false );
			rff.Set( RFF_RayTracedTranparentReflection, false );
			rff.Set( RFF_RayTracedDirectionalShadow, false );
			rff.Set( RFF_RayTracingDebug, false );
#endif
			// moradin - optimizations
			rff.Set( RFF_GlobalLight, false ); //no global lighting in mirrors
			rff.Set( RFF_VolumetricClouds, false );
			rff.Set( RFF_SimpleFog, true );
			rff.Set( RFF_VolumetricFog, false );
			rff.Set( RFF_GlobalIllumination, false );
			rff.Set( RFF_GlobalIlluminationCompute, false );
			rff.Set( RFF_Particles, false );
			rff.Set( RFF_DistantGI, false );
			rff.Set( RFF_LocalShadows, false );
			rff.Set( RFF_ContactShadows_Local, true );

			rff.Set( RFF_Simplified, true );

			// Disable Async Compute for mirrors.
			// #TODO: Do this in a nicer way. Don't really have any benefit from having async SSAO, since we generally have
			// no cascade rendering in the mirror. But maybe we should keep other stuff, like hair clears?

			// KN: enabling this as it produces some artifacts when async is disabled.
			rff.Set( RFF_EnableAsyncCompute, true );

			return rff;
		}();
		return allowedFeatures;
	}

};

// [skperf] round 18 mirrorRate (?mirrorRate=N, N = 1..3; DEFAULT 2 since t18 (16 vs 12 FPS in the bar, identical
// screenshot, 0 errors); ?mirrorRate=1 = original every-frame mirror).
// A mirror re-renders the scene from a dynamic camera every frame (a second full pipeline: in the bar ~2/3 of the
// frame). With N > 1 the mirror camera is only requested every Nth frame. The request made in frame F renders in F+1;
// on that render frame the result is copied (outside the engine's per-frame render flow) into a persistent texture of
// our own. On the following N-1 frames the camera is not in the frame (its temporary dependency is dropped) and the
// mirror material binds that copy instead of the null texture, so there is no black frame. The reflection matrix is
// the one built on the render frame, re-based by the origin camera's motion since then (the shader works in
// origin-camera-relative coordinates), so the reflected image stays registered while the player moves; only the
// reflected content updates at 1/N rate. N <= 3 keeps the engine's temporal data (PrepareCamera usedRecently needs
// usageTick + 2 >= tick; temporal cameras live until updateTick + 3). The mirror's builder is reused across the
// skipped frames, so its resolution hysteresis and matrix persist exactly as in the every-frame path.
// perfprobe=1: 'CP2077 skperf mirror probe' every ~2 s (frames rendered / from copy / null, copies, requests).
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
namespace cpMirrorRate
{
	std::atomic<Int32> s_rate{ -1 };
	Int32 Rate()
	{
		Int32 v = s_rate.load( std::memory_order_acquire );
		if ( v >= 0 ) return v;
#if defined( RED_PLATFORM_WEB )
		v = MAIN_THREAD_EM_ASM_INT( {
			try {
				const q = new URLSearchParams( location.search );
				const m = q.get( 'mirrorRate' );
				if ( m !== null ) { const n = Math.floor( Number( m ) ); return ( Number.isFinite( n ) && n >= 1 ) ? Math.min( n, 3 ) : 2; }
				return 2;
			} catch ( e ) { return 2; }
		} );
#else
		v = 1;
#endif
		if ( v < 1 ) v = 1;
		if ( v > 3 ) v = 3;
		Int32 expected = -1;
		if ( s_rate.compare_exchange_strong( expected, v, std::memory_order_acq_rel ) )
		{
			std::fprintf( stderr, "CP2077 skperf switches: mirrorRate=%d\n", v );
			return v;
		}
		return expected;
	}

	struct Entry
	{
		Uint64 hash = 0;
		Bool used = false;
		Bool valid = false;
		red::SharedPtr<CRenderFrameCameraStorage::IRenderCameraBuilder> builder;
		GpuApi::TextureRef tex = GpuApi::TextureRef::Null();
		Uint32 texW = 0, texH = 0;
		Uint32 texFormat = 0;
		Vector4 texSize = Vector4::ONES();
		Matrix worldToMirror;
		WorldPosition camPos;
		Int32 renderFrame = 0;		// frame whose mirror render the copy holds
		Int32 seenFrame = 0;		// last frame this mirror was drawn (for the per-frame counters / eviction)
		Bool seenOnce = false;
	};
	constexpr Uint32 kEntries = 8;
	Entry s_entries[ kEntries ];
	std::mutex s_lock;
	std::atomic<unsigned long long> s_framesRendered{ 0 }, s_framesCached{ 0 }, s_framesNull{ 0 }, s_copies{ 0 }, s_copyFails{ 0 },
		s_requests{ 0 }, s_requestsSkipped{ 0 };
	std::atomic<double> s_windowStart{ 0 };

	Entry& Find( Uint64 hash, Int32 frame )
	{
		Entry* freeSlot = nullptr;
		Entry* oldest = nullptr;
		for ( Entry& e : s_entries )
		{
			if ( e.used && e.hash == hash ) return e;
			if ( !e.used ) { if ( !freeSlot ) freeSlot = &e; continue; }
			if ( !oldest || ( frame - e.seenFrame ) > ( frame - oldest->seenFrame ) ) oldest = &e;
		}
		Entry& e = freeSlot ? *freeSlot : *oldest;
		if ( !e.tex.isNull() ) GpuApi::Release( e.tex );
		e = Entry();
		e.used = true;
		e.hash = hash;
		return e;
	}

	// The shader gets world positions relative to the CURRENT origin camera; the matrix was built relative to the
	// origin camera of the render frame. Row-vector convention (TransformPoint = X*x + Y*y + Z*z + W):
	// M'(p) = M(p + d) with d = camNow - camThen  ->  W' = W + X*d.x + Y*d.y + Z*d.z.
	Matrix Rebase( const Matrix& m, const Vector3& d )
	{
		Matrix r = m;
		r.SetRow( 3, m.GetRow( 3 ) + m.GetRow( 0 ) * d.X + m.GetRow( 1 ) * d.Y + m.GetRow( 2 ) * d.Z );
		return r;
	}

	// Copies mip 0 of the mirror camera's color result into the entry's own texture (recreated when size/format change).
	Bool Store( Entry& e, const GpuApi::TextureRef& source )
	{
		const GpuApi::TextureDesc srcDesc = GpuApi::GetTextureDesc( source );
		const GpuApi::TextureLevelDesc level = GpuApi::GetTextureLevelDesc( source, 0 );
		if ( e.tex.isNull() || e.texW != level.width || e.texH != level.height || e.texFormat != ( Uint32 )srcDesc.format )
		{
			if ( !e.tex.isNull() ) { GpuApi::Release( e.tex ); e.tex = GpuApi::TextureRef::Null(); }
			GpuApi::TextureDesc desc;
			desc.width = level.width;
			desc.height = level.height;
			desc.format = srcDesc.format;
			desc.type = GpuWrapApi::TEXTYPE_2D;
			desc.usage = GpuApi::TEXUSAGE_Samplable;
			desc.numMips = 1;
			desc.group = GpuWrapApi::TEXG_System_Generic;
			e.tex = GpuApi::CreateTexture( desc );
			if ( e.tex.isNull() ) return false;
			GpuApi::SetResourceDebugName( e.tex, "skperfMirrorCopy" );
			e.texW = level.width; e.texH = level.height; e.texFormat = ( Uint32 )srcDesc.format;
		}
		return GpuApi::CopyTextureData( e.tex, 0, 0, source, 0, 0 );
	}

	void Probe()
	{
#if defined( RED_PLATFORM_WEB )
		if ( ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 8 ) == 0 ) return; // probeMask bit 8
		const double now = emscripten_get_now();
		double start = s_windowStart.load( std::memory_order_relaxed );
		if ( start == 0 ) { s_windowStart.compare_exchange_strong( start, now ); return; }
		if ( now - start < 2000 || !s_windowStart.compare_exchange_strong( start, now ) ) return;
		static std::atomic<unsigned long long> last[ 7 ];
		const unsigned long long v[ 7 ] = { s_framesRendered.load(), s_framesCached.load(), s_framesNull.load(), s_copies.load(),
			s_copyFails.load(), s_requests.load(), s_requestsSkipped.load() };
		unsigned long long d[ 7 ];
		for ( Uint32 i = 0; i < 7; ++i ) d[ i ] = v[ i ] - last[ i ].exchange( v[ i ] );
		const double sec = ( now - start ) / 1000.0;
		std::fprintf( stderr, "CP2077 skperf mirror probe: window=%.1fs rate=%d per-second: mirrorRendered=%.1f fromCopy=%.1f nullTex=%.1f copies=%.1f copyFails=%.1f requests=%.1f requestsSkipped=%.1f | occl=%d published=%llu grazing=%llu applied=%llu disabled=%llu (totals)\n",
			sec, s_rate.load(), d[ 0 ] / sec, d[ 1 ] / sec, d[ 2 ] / sec, d[ 3 ] / sec, d[ 4 ] / sec, d[ 5 ] / sec, d[ 6 ] / sec,
			cpMirrorOccl::s_on.load(), cpMirrorOccl::s_published.load(), cpMirrorOccl::s_grazing.load(), cpMirrorOccl::s_applied.load(), cpMirrorOccl::s_disabled.load() );
#endif
	}
}


static void MaterialModifierProcess_Mirror( const SMatModifierData_Mirror& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
	RED_FATAL_ASSERT( params.m_renderNodeContext, "This material modifier must be used in a render node" );

	if ( !params.m_renderNodeContext->Test( RFF_MirrorMaterial ) // <- Mirrors are simply disabled
		 || ( Config::cvRayTracingEnableMirrorMaterialReflection.Get() && params.m_renderNodeContext->Test( RFF_RayTracedReflection ) ) // <- Mirrors are handled through RayTracing
	   )
	{
		// Nothing to do here. Early out.
		return;
	}

	// Fetch a bunch of stuff we gonna need
	const CRenderFrameInfo& frameInfo = params.m_renderNodeContext->GetFrameInfo();
	const CRenderSceneEx* scene = static_cast<const CRenderSceneEx*>( frameInfo.GetScene() );
	RED_FATAL_ASSERT( scene, "Invalid render scene" );
	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	if ( GT_Mesh != geoBatchData.m_type ) return;
	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const IRenderProxyDrawable* proxyPtr = batchData.GetProxy();
	CRenderFrameCameraStorage* cameraStorage = scene->GetCameraStorage();

	
#ifndef RED_CONFIGURATION_FINAL
	// For debug purposes: when requested show a sphere at every proxy with mirror material attached.
	const auto& cameraInfo = params.m_renderNodeContext->GetCameraData()->GetInfo();
	if ( cameraInfo.GetRenderingMask().Test( rend::DF_MirrorMaterialLocation_Debug ) )
	{
		params.m_renderNodeContext->GetCollector()->m_mirrorDebugCollector.PushLocationMarker( proxyPtr->GetLocalToWorld() );
	}
#endif

	// build new mirror camera hash
	const Uint64 originCameraHash = params.m_renderNodeContext->GetCameraData()->m_cameraHash;
	const Uint64 mirrorCameraHash = red::CalculateHash64( &proxyPtr, sizeof( proxyPtr ), originCameraHash );
	RED_ASSERT( mirrorCameraHash != originCameraHash, "Somehow the camera points at itself as dependency." );

	// We gonna need:
	// 1. Mirror Camera Data - obviously to be able to render the contents of mirror.
	// 2. Flow Space - to be able to fetch results (texture) of last frame mirror camera render
	// 3. Mirror Camera Builder - essentially just a helper object, but it helps preserving temporal stability of mirror between frames.
	// In the next section further below we are gonna try to salvage previous frame results. For now just declare these three.
	Int32 remoteCameraRenderFlowSpace = -1;
	const CRenderFrameCameraStorage::SCameraData *remoteCameraData = nullptr;
	red::SharedPtr<CRenderFrameCameraStorage::IRenderCameraBuilder> cameraBuilder = nullptr;

	// So, check if we already have these in the storage (again - we might have requested this specific mirror in previous frames already, meaning we have the data, the builder, and the flow space for it)
	// - If we have the camera AND we have a dependency between it and the main camera - we should be able to just prolong it's life and keep using it.
	// - If we DON'T have the camera OR the dependency wasn't there - no problem - we will create new camera data, new flow space, and new camera builder.
	// [skperf] round 18 mirrorRate: with N > 1 the per-mirror entry below (under s_lock for the whole call) keeps the
	// builder across the skipped frames and holds the copy of the last render.
	const Int32 mirrorRate = cpMirrorRate::Rate();
	const Int32 frame = ( Int32 )params.m_renderNodeContext->GetCollector()->m_frameIndex; // per-frame snapshot (collector init)
	std::unique_lock<std::mutex> rateLock( cpMirrorRate::s_lock, std::defer_lock );
	cpMirrorRate::Entry* rateEntry = nullptr;
	Bool firstCallThisFrame = true;
	if ( mirrorRate > 1 )
	{
		rateLock.lock();
		rateEntry = &cpMirrorRate::Find( mirrorCameraHash, frame );
		firstCallThisFrame = !rateEntry->seenOnce || rateEntry->seenFrame != frame;
		rateEntry->seenOnce = true;
		rateEntry->seenFrame = frame;
	}
	{
		const Bool isCurrentlyDependent = cameraStorage->HasDependency( originCameraHash, mirrorCameraHash, false );
		const Int32 idx = isCurrentlyDependent ? cameraStorage->FindCameraData( mirrorCameraHash ) : -1;
		if ( -1 != idx )
		{
			// Camera already exists (which means mirror was being requested in previous frames), so let's keep using it.
			remoteCameraRenderFlowSpace = cameraStorage->GetCameraData( idx )->m_renderFlowSpace;
			remoteCameraData = cameraStorage->GetCameraData( idx );
			cameraBuilder = remoteCameraData->m_cameraBuilder;
		}

		// [skperf] skipped frame: keep using this mirror's builder instead of a fresh one
		if ( cameraBuilder == nullptr && rateEntry && rateEntry->builder != nullptr )
		{
			cameraBuilder = rateEntry->builder;
		}

		if ( cameraBuilder == nullptr )
		{
			cameraBuilder.Reset( RED_NEW( CMirrorCameraBuilder )( originCameraHash, proxyPtr ) );
		}
		if ( rateEntry ) rateEntry->builder = cameraBuilder;
	}

	// At this point we either already have camera data, builder, and flow space from previous frame - or we just have a brand new builder.
	// This way or another, now we want to actually set up what's the mirror camera going to be (it's placement, resolution, etc.)
	const CRenderFrameCameraStorage::SCameraData *originCameraData = params.m_renderNodeContext->GetCameraData();
	RED_ASSERT( originCameraData, "camera data is expected" );

	// Last but not least, it's time to actually help rendering the mirror contents that were requested last frame
	// (unless this is the very first frame when mirror is visible - in which case the flow space will probably not exist and we gonna early out)
	// [skperf] (moved ahead of the request so the request decision knows whether the copy is valid; the request only
	// queues data for the next frame's camera allocation, so the order of the two steps does not matter otherwise)
	{
		GpuApi::TextureRef mirrorTexture = GpuApi::TextureRef::Null();
		Vector4 texSizeParams = Vector4::ONES();
		Bool fromCopy = false;
		Matrix copyWorldToMirror;

		if ( -1 != remoteCameraRenderFlowSpace )
		{
			// Flow space exists - great! This means that last frame we alredy requetsed this mirror camera.
			// Let's pull the render target with the contents of the mirror, together with it's dimensions
			RenderFlowNameTag rtColor( RENDER_NAME_TAG( "color" ), (TRenderFlowSpace)remoteCameraRenderFlowSpace );
			mirrorTexture = params.m_renderNodeContext->RTGet< GpuApi::TextureRef >( rtColor );
			RED_FATAL_ASSERT( mirrorTexture, "If flow space exists, the result of rendering mirror should be available!" );
			GpuApi::TextureLevelDesc texColorDesc = GpuApi::GetTextureLevelDesc( mirrorTexture, 0 );
			texSizeParams.X = (Float)remoteCameraData->GetResolutionState().GetTargetWidth();
			texSizeParams.Y = (Float)remoteCameraData->GetResolutionState().GetTargetHeight();
			texSizeParams.Z = (Float)texColorDesc.width;
			texSizeParams.W = (Float)texColorDesc.height;

			// [skperf] render frame: copy the result (once per frame) for the skipped frames that follow
			if ( rateEntry && ( firstCallThisFrame || !rateEntry->valid || rateEntry->renderFrame != frame ) )
			{
				const Bool stored = cpMirrorRate::Store( *rateEntry, mirrorTexture );
				rateEntry->valid = stored;
				rateEntry->renderFrame = frame;
				if ( stored )
				{
					rateEntry->texSize = texSizeParams;
					rateEntry->worldToMirror = ( (CMirrorCameraBuilder*)cameraBuilder.Get() )->m_compensatedWorldToMirror;
					rateEntry->camPos = originCameraData->GetRenderCamera().GetPosition();
					cpMirrorRate::s_copies.fetch_add( 1, std::memory_order_relaxed );
				}
				else
				{
					cpMirrorRate::s_copyFails.fetch_add( 1, std::memory_order_relaxed );
				}
			}
			if ( firstCallThisFrame ) cpMirrorRate::s_framesRendered.fetch_add( 1, std::memory_order_relaxed );

#ifndef RED_CONFIGURATION_FINAL
			// Since we have a mirror texture in our hands, let's provide a convenient way to preview it's contents
			if ( auto camSpy = ( renderDev::CCameraSpyImpl* )GRender->GetCameraSpy() )
			{
				camSpy->SpyCameraOnce( mirrorTexture );
			}

			const auto& cameraInfo = params.m_renderNodeContext->GetCameraData()->GetInfo();
			if ( cameraInfo.GetRenderingMask().Test( rend::DF_MirrorMaterialTexture_Debug ) )
			{
				GetRenderer()->PushTexturePreview( SDynamicPreviewTexture( mirrorTexture ).Tonemap().Rgb() );
			}

			if ( cameraInfo.GetRenderingMask().Test( rend::DF_MirrorMaterialCamera_Debug ) && remoteCameraData )
			{
				params.m_renderNodeContext->GetCollector()->m_mirrorDebugCollector.PushCamera( remoteCameraData->m_renderCamera[ 0 ] );
			}

			if ( cameraInfo.GetRenderingMask().Test( rend::DF_MirrorMaterialReflectionPlane_Debug ) )
			{
				params.m_renderNodeContext->GetCollector()->m_mirrorDebugCollector.PushPlane( proxyPtr->GetLocalToWorld() );
			}
#endif
		}
		else if ( rateEntry && rateEntry->valid && !rateEntry->tex.isNull() && frame != rateEntry->renderFrame &&
				  ( frame - rateEntry->renderFrame ) < mirrorRate )
		{
			// [skperf] skipped frame: the copy of the last render, re-based to the current origin camera
			mirrorTexture = rateEntry->tex;
			texSizeParams = rateEntry->texSize;
			const Vector3 moved = originCameraData->GetRenderCamera().GetPosition() - rateEntry->camPos;
			copyWorldToMirror = cpMirrorRate::Rebase( rateEntry->worldToMirror, moved );
			fromCopy = true;
			if ( firstCallThisFrame ) cpMirrorRate::s_framesCached.fetch_add( 1, std::memory_order_relaxed );
		}
		else if ( firstCallThisFrame && cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) != 0 )
		{
			cpMirrorRate::s_framesNull.fetch_add( 1, std::memory_order_relaxed );
		}

		// Now, regardless of whether we did request the mirror last frame (in which case we have a textures and it's dimensions)
		// OR we didn't - in which case we have a null texture and 0 dimensions...
		// Bind one or another. We are in the process of issuing a draw-call for the mirror mesh after all, too late to change our mind.
		if ( matModData.m_textureSlot >= 0 )
		{
			GpuApi::BindTextures( matModData.m_textureSlot, 1, &mirrorTexture, GpuWrapApi::PixelShader );
		}
		if ( matModData.m_textureIsBoundSlot >= 0 )
		{
			mmConstants.SetValue( matModData.m_textureIsBoundSlot, !mirrorTexture.isNull() && matModData.m_textureSlot >= 0 );
		}
		if ( matModData.m_texSizeSlot >= 0 )
		{
			mmConstants.SetValue( matModData.m_texSizeSlot, texSizeParams );
		}
		if ( matModData.m_reflectionViewToScreenSlot >= 0 )
		{
			CMirrorCameraBuilder* builder = (CMirrorCameraBuilder*)cameraBuilder.Get();
			Matrix worldToMirror = fromCopy ? copyWorldToMirror : builder->m_compensatedWorldToMirror;
			mmConstants.SetValue( matModData.m_reflectionViewToScreenSlot, worldToMirror.Transposed() );
		}
	}

	// [skperf] mirrorRate: request the camera only on the frame before a render frame (N = 1: every frame = original)
	Bool requestThisFrame = true;
	if ( rateEntry && rateEntry->valid && ( frame - rateEntry->renderFrame ) < mirrorRate - 1 )
	{
		requestThisFrame = false;
	}

	if ( requestThisFrame && cameraBuilder->CheckIfWorthBuilding( *originCameraData ) )
	{
		const CRenderFrameCameraStorage::SCameraData *cameraData = params.m_renderNodeContext->GetCameraData();
		const Bool shouldUseOcclusionCulling { false };	// E.g. mirror that is hanging on a wall might end up with its camera located behind the wall and when this wall has occluders the mirror sees nothing.

		CRenderFrameCameraStorage::SDynamicCameraRequest mirrorCameraRequest;
		mirrorCameraRequest.m_cameraBuilder = cameraBuilder;
		//mirrorCameraRequest.m_renderCamera = <- deliberately leave that unset. This request is gonna use the camera builder instead.
		mirrorCameraRequest.m_featureSet = cameraBuilder->GetAllowedFeatureSet();
		mirrorCameraRequest.m_cameraName = mirrorCameraHash;
		mirrorCameraRequest.m_originCameraName = originCameraHash;
		mirrorCameraRequest.m_outputFlags = CRenderFrameCameraStorage::CDF_Output_Color;
		mirrorCameraRequest.m_sceneToRender = cameraData->m_info.m_renderScene;
		mirrorCameraRequest.m_shouldUseOcclusionCulling = shouldUseOcclusionCulling || cpMirrorOccl::On(); // [skperf] mirrorOccl (opt-in)

		cameraStorage->AddDynamicCameraRequest( mirrorCameraRequest );
		if ( cpMirrorFrustum::On() ) cpMirrorCull::Publish( mirrorCameraHash, cameraBuilder.Get() ); // [skperf] round 49/50 mirror id (mirrorFrustum / mirrorCull)
		if ( firstCallThisFrame ) cpMirrorRate::s_requests.fetch_add( 1, std::memory_order_relaxed );
	}
	else if ( !requestThisFrame && firstCallThisFrame )
	{
		cpMirrorRate::s_requestsSkipped.fetch_add( 1, std::memory_order_relaxed );
	}
	cpMirrorRate::Probe();

}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 

static void MaterialModifierProcess_DebugColoring( const SMatModifierDebugColoringParams& matModData, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants )
{
#ifndef RED_CONFIGURATION_FINAL
	RED_FATAL_ASSERT( matModData.m_debugColorSlot >= 0 );

	const CRenderFrameInfo& frameInfo = params.m_renderNodeContext->GetFrameInfo();
	const CRenderSceneEx* scene = static_cast<const CRenderSceneEx*>( frameInfo.GetScene() );
	RED_FATAL_ASSERT( scene, "Invalid render scene" );

	const SGeometryBatchData &geoBatchData = params.m_batchDataAllocator.GetGeometryData().GetData( params.m_chunk.m_key.GetGeometry() );
	RED_FATAL_ASSERT( GT_Mesh == geoBatchData.m_type, "Mesh chunk expected" );

	const SProxyBatchData& batchData = params.m_batchDataAllocator.GetProxyData().GetData( params.m_chunk.m_data.GetProxy() );
	const CRenderProxy_Mesh* meshProxy = static_cast<const CRenderProxy_Mesh*>( batchData.GetProxy() );

	RED_FATAL_ASSERT( meshProxy );

	// TODO uncomment when TBatchDataAllocator::DataInitRequest is fixed (thread safety issue)
	//RED_FATAL_ASSERT( meshProxy->GetProxyBatchDataId().GetValue() == params.m_chunk.m_data.GetProxy() );

	math::Color color = meshProxy->GetDebugColor();

	if ( color == c_meshDebugColoringMode_LOD )
	{
		if ( geoBatchData.m_meshData.m_lodIndex == 0 )
		{
			color = math::Color::RED();
		}
		else if ( geoBatchData.m_meshData.m_lodIndex == 1 )
		{
			color = math::Color::YELLOW();
		}
		else if ( geoBatchData.m_meshData.m_lodIndex == 2 )
		{
			color = math::Color::LIGHT_GREEN();
		}
		else
		{
			color = math::Color::GREEN();
		}
	}

	mmConstants.SetValue( matModData.m_debugColorSlot, color.ToVector() );
#endif // RED_CONFIGURATION_FINAL
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -


template<typename T>
inline void MatModDispatch(void (*Func)(const T&, const SMatModApplyParams &params, MaterialModifierConstants& mmConstants ), const SMaterialModifierEntryHeader* modDataHeader, const SMatModApplyParams& params, MaterialModifierConstants& mmConstants )
{
	RED_ASSERT( modDataHeader->m_size == sizeof(T), "modData has unexpected size (is %u, expected %u)", modDataHeader->m_size, sizeof(T) );
	Func( *(const T*)(modDataHeader + 1), params, mmConstants );
}


static void MaterialModifierProcessForType( EMaterialModifier modType, const SMaterialModifierEntryHeader* modDataHeader, const SMatModApplyParams& params, MaterialModifierConstants& mmConstants )
{
	static_assert(EMATMOD_MAX == 27, "Material Modifiers changed");
	switch ( modType)
	{
	case EMATMOD_HitProxy:			MatModDispatch( MaterialModifierProcess_HitProxy,			modDataHeader, params, mmConstants ); break;
	case EMATMOD_WindData:			MatModDispatch( MaterialModifierProcess_WindData,			modDataHeader, params, mmConstants ); break;
	case EMATMOD_AutoSpawnData:		MatModDispatch( MaterialModifierProcess_AutoSpawnData,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_ParticleParams:	MatModDispatch( MaterialModifierProcess_ParticleParams,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_RemoteCamera:		MatModDispatch( MaterialModifierProcess_RemoteCamera,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_Mirror:			MatModDispatch( MaterialModifierProcess_Mirror,				modDataHeader, params, mmConstants ); break;
	case EMATMOD_CustomStructBuffer:MatModDispatch( MaterialModifierProcess_CustomStructBuffer,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_DestructionRegions:MatModDispatch( MaterialModifierProcess_DestructionRegions,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_MotionMatrix:		MatModDispatch( MaterialModifierProcess_MotionMatrix,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_ColorAndTexture:	MatModDispatch( MaterialModifierProcess_ColorAndTexture,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_EffectParams:		MatModDispatch( MaterialModifierProcess_EffectParams,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_DrawBufferMask:	MatModDispatch( MaterialModifierProcess_DrawBufferMask,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_Garments:			MatModDispatch( MaterialModifierProcess_Garments,			modDataHeader, params, mmConstants ); break;
	case EMATMOD_MaterialParams:	MatModDispatch( MaterialModifierProcess_MaterialParams,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_Eye:				MatModDispatch( MaterialModifierProcess_Eye,				modDataHeader, params, mmConstants ); break;
	case EMATMOD_Skin:				MatModDispatch( MaterialModifierProcess_Skin,				modDataHeader, params, mmConstants ); break;
	case EMATMOD_Dismemberment:		MatModDispatch( MaterialModifierProcess_Dismemberment,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_ShadowsDebugParams:MatModDispatch( MaterialModifierProcess_ShadowsDebugParams,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_MultilayeredDebug:	MatModDispatch( MaterialModifierProcess_MultilayeredDebug,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_ParallaxParams:	MatModDispatch( MaterialModifierProcess_ParallaxParams,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_HighlightsParams:	MatModDispatch( MaterialModifierProcess_HighlightsParams,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_DebugColoring:		MatModDispatch( MaterialModifierProcess_DebugColoring,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_VehicleParams:		MatModDispatch( MaterialModifierProcess_VehicleParams,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_FloatTracks:		MatModDispatch( MaterialModifierProcess_FloatTracks,		modDataHeader, params, mmConstants ); break;
	case EMATMOD_AutoHideDistance:	MatModDispatch( MaterialModifierProcess_AutoHideDistance,	modDataHeader, params, mmConstants ); break;
	case EMATMOD_Rain:				MatModDispatch( MaterialModifierProcess_Rain,				modDataHeader, params, mmConstants ); break;
	case EMATMOD_PlanarReflections:	MatModDispatch( MaterialModifierProcess_PlanarReflections,	modDataHeader, params, mmConstants ); break;
	default:						RED_FATAL( "Invalid" );
	}
}

MaterialModifierSetupFlags GetMaterialModifierUpdateFrequency( EMaterialModifier modType )
{
	// A material modifier should only have the flags it needs, based on where it pulls data from. E.g. if the modifier
	// is getting some constant stored in a render proxy, then it should have PerProxy. If it takes something from a
	// particular mesh, then it should be PerGeometry. Putting more flags than necessary can break up batching and
	// instancing, reducing performance if it's used frequently.

	// #TODO: Go through these, make sure they're sensible.
	// #TODO: Could be good to have another frequency for things that don't really need to change, so they can be
	// applied just when the material itself changes.

	static_assert(EMATMOD_MAX == 27, "Material Modifiers changed");
	switch ( modType )
	{
	case EMATMOD_HitProxy:			return MMSF_PerChunk;
	case EMATMOD_WindData:			return					MMSF_PerGeometry;
	case EMATMOD_AutoSpawnData:		return					MMSF_PerGeometry;
	case EMATMOD_ParticleParams:	return					MMSF_PerGeometry;
	case EMATMOD_RemoteCamera:		return MMSF_PerChunk;
	case EMATMOD_Mirror:			return										MMSF_PerProxy;
	case EMATMOD_CustomStructBuffer:return					MMSF_PerGeometry;
	case EMATMOD_DestructionRegions:return										MMSF_PerProxy;
	case EMATMOD_MotionMatrix:		return										MMSF_PerProxy;
	case EMATMOD_ColorAndTexture:	return					MMSF_PerGeometry;
	case EMATMOD_EffectParams:		return					MMSF_PerGeometry;
	case EMATMOD_DrawBufferMask:	return					MMSF_PerGeometry;
	case EMATMOD_Garments:			return MMSF_PerChunk;
	case EMATMOD_MaterialParams:	return										MMSF_PerProxy;
	case EMATMOD_Eye:				return										MMSF_PerProxy;
	case EMATMOD_Skin:				return										MMSF_PerProxy;
	case EMATMOD_Dismemberment:		return MMSF_PerChunk|	MMSF_PerGeometry|	MMSF_PerProxy;
	case EMATMOD_ShadowsDebugParams:return										MMSF_PerProxy;
	case EMATMOD_MultilayeredDebug:	return MMSF_PerChunk;
	case EMATMOD_ParallaxParams:	return										MMSF_PerProxy;
	case EMATMOD_HighlightsParams:	return										MMSF_PerProxy;
	case EMATMOD_DebugColoring:		return										MMSF_PerProxy;
	case EMATMOD_VehicleParams:		return										MMSF_PerProxy;
	case EMATMOD_FloatTracks:		return										MMSF_PerProxy;
	case EMATMOD_AutoHideDistance:	return										MMSF_PerProxy;
	case EMATMOD_Rain:				return										MMSF_PerProxy;
	case EMATMOD_PlanarReflections:	return					MMSF_PerGeometry;

	default: RED_FATAL( "Invalid" ); return (MaterialModifierSetupFlags)0;
	}
}


void MaterialModifierProcess( const SMaterialModifierSpanBuffer &matModBuffer, MaterialModifierSetupFlags setupFlags, const SRenderNodeImplContext *rctx, const CBatchDataAllocator &batchDataAllocator, const SRenderChunk &chunk, GeometryBatcherStats& stats, MaterialModifierConstants& mmConstants )
{
	const SMatModApplyParams params ( rctx, batchDataAllocator, chunk );
	for ( MaterialModifierBufferIterator iter ( matModBuffer ); iter.IsValid(); iter.Next() )
	{
		const EMaterialModifier modType = iter.GetType();

		const MaterialModifierSetupFlags updateFrequency = GetMaterialModifierUpdateFrequency( modType );
		if ( ( updateFrequency & setupFlags ) == 0 )
		{
			continue;
		}

#ifndef RED_CONFIGURATION_FINAL
		stats.m_materialModifierChanges[ static_cast<Uint32>( modType ) ]++;
#endif

		MaterialModifierProcessForType( modType, iter.GetHeader(), params, mmConstants );
	}
}
