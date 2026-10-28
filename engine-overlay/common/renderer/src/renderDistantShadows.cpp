/**
* Copyright (c) 2017 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include "renderDistantShadows.h"
#include "../../redCore/include/globalModeInfo.h"
#include "../../renderData/include/materialFeatureFlags.h"
#include "../../visibility/include/visRasterizerSSEThreaded.h"
#if defined( __EMSCRIPTEN__ )
#include <emscripten/emscripten.h>
// [renderwait] see renderRainMap.cpp: the loading screen no longer waits ~2 s for all distant-shadow cascades
// (?loadWaitRender=1 = original wait); they finish building right after the session starts.
static bool CpLoadWaitRenderDS()
{
	static const int v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'loadWaitRender' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
	return v != 0;
}
#endif

RTTI_BEGIN_ABSTRACT_TYPE( SceneCustomData_DistantShadowsCommon );
{
	RTTI_PARENT_TYPE( ISceneStorageCustomData );
}
RTTI_END_TYPE();

RTTI_BEGIN_TYPE( SceneCustomData_DistantShadows );
{
	RTTI_PARENT_TYPE( SceneCustomData_DistantShadowsCommon );
}
RTTI_END_TYPE();


namespace Schedule
{
	enum
	{
		NONE,
		INVALIDATE,	// Just some data changed, need to resolve shadows.
		HARD_RESET	// When we want to bring up loading screen again
	};
}

SceneCustomData_DistantShadowsCommon::SceneCustomData_DistantShadowsCommon()
	: m_phase( IsGameMode() ? Phase::Waiting : Phase::Prepare )
	, m_shadowmapBatchStart( 0 )
	, m_shadowmapBatchIndex( 0 )
{
}

SceneCustomData_DistantShadowsCommon::~SceneCustomData_DistantShadowsCommon()
{
}

SceneCustomData_DistantShadows::SceneCustomData_DistantShadows()
	: m_hasValidData( false )
	, m_numLevels( -1 )
	, m_levelsValid( 0 )
	, m_updateIteration( 0 )
	, m_levelToUpdate( 0 )
	, m_forceFullDraw( false )
	, m_scheduleReset( Schedule::NONE )
	, m_doClearDepth( false )
	, m_lastUpdatedLevel( 0 )
	, m_isBlendInProgress( false )
{
	for ( Uint32 i = 0; i < MAX_DISTANT_SHADOWS_LEVELS; ++i )
	{
		m_activeDistShadowBlendFactor[i] = 1.f;
	}
}

SceneCustomData_DistantShadows ::~SceneCustomData_DistantShadows()
{
	m_distantShadowsCollector.Reset();
}

void SceneCustomData_DistantShadows::Initialize()
{
}

void SceneCustomData_DistantShadows::Prepare(const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData )
{
	PC_SCOPE( SceneCustomData_DistantShadows_Prepare );

	// distant shadows are used only in normal scene rendering
	if ( rctx.GetFrameInfo().GetFramePurpose() != FP_Normal )
	{
		return;
	}

	// The data need to be spawn for the whole frame, no matter how many camera's graphs we have.
	// Because of that we need to check whether there is at least one camera that needs it.
	Bool distantShadowsRequested = false;
	Bool hasOnscreenCamera = false;
	const auto* camStorage = rctx.GetCameraStorage();
	for ( Uint32 camIndex = 0; camIndex < camStorage->GetNumCameras(); camIndex++ )
	{
		const auto* camData = camStorage->GetCameraData( camIndex );
		if ( camData->IsOnscreen() )
		{
			hasOnscreenCamera = true;
			distantShadowsRequested |= camData->GetInfo().HasFeature( RFF_DistantShadows );
		}
	}

	// if no onscreen camera, ignore distant shadows
	if ( !hasOnscreenCamera )
	{
		return;
	}

	// if( rctx.GetCameraData()->m_timeOfDayCut )
	// {
	// 	m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );
	// }

	if ( distantShadowsRequested )
	{
		const Uint32 shadowmapResolution = Config::GetDistantShadowsResolution();
		const Uint32 maxLevels = Config::GetDistantShadowsMaxLevels() + DistShadowsBlendLevelsCount;

		RED_ASSERT( shadowmapResolution > 0 );

		const GpuApi::TextureDesc* shadowsTextureDesc = (m_shadowsTexture ? &GpuApi::GetTextureDesc(m_shadowsTexture) : nullptr);
		if ( shadowsTextureDesc == nullptr || shadowsTextureDesc->width != shadowmapResolution || shadowsTextureDesc->depthOrSliceNum != maxLevels )
		{
			m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );
			m_levelsValid = 0; // invalidate all levels

			GpuApi::TextureDesc desc;
			desc.type = GpuWrapApi::TEXTYPE_ARRAY;
			desc.width = shadowmapResolution;
			desc.height = shadowmapResolution;
			desc.depthOrSliceNum = maxLevels;
			desc.numMips = 1;
			desc.format = GpuWrapApi::TEXFMT_D16U;
			desc.usage = GpuApi::TEXUSAGE_Samplable;
			desc.group = GpuWrapApi::TEXG_System_Shadows;
			m_shadowsTexture = GpuApi::Texture::Create( desc );
			
			GpuApi::SetResourceDebugName( m_shadowsTexture, "DistantShadows" );
		}

		{
			const GpuApi::TextureDesc* shadowmapDesc = ( m_updatedShadowmap ? &GpuApi::GetTextureDesc( m_updatedShadowmap ) : nullptr );
			if(shadowmapDesc == nullptr || shadowmapDesc->width != shadowmapResolution)
			{
				m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );

				GpuApi::TextureDesc desc;
				desc.type = GpuWrapApi::TEXTYPE_2D;
				desc.width = shadowmapResolution;
				desc.height = shadowmapResolution;
				desc.numMips = 1;
				desc.format = GpuWrapApi::TEXFMT_D16U;
				desc.usage = GpuApi::TEXUSAGE_DepthStencil | GpuApi::TEXUSAGE_Samplable;
				desc.group = GpuWrapApi::TEXG_System_Shadows;
				m_updatedShadowmap = GpuApi::Texture::Create( desc, GpuApi::TextureInitData::Uninitialized().OptimalClearDepth(1.0f) );

				GpuApi::SetResourceDebugName( m_updatedShadowmap, "DistantShadows_Updated" );
			}
		}

		{
			const Uint32 readbackResolution = shadowmapResolution/4;
			const Uint32 readbackLowResolution = readbackResolution/32;
			const Uint32 readbackSize = (readbackResolution*readbackResolution + readbackLowResolution*readbackLowResolution)*sizeof( vis::rast::DepthType );
			if ( m_globalOcclusionReadbackBuffer.isNull() || GpuApi::GetBufferDesc( m_globalOcclusionReadbackBuffer ).size != readbackSize )
			{
				m_globalOcclusionRasterizer = nullptr;

				GpuApi::BufferDesc bdesc;
				bdesc.size = readbackSize;
				bdesc.category = GpuWrapApi::BCC_Staging;
				bdesc.group = GpuWrapApi::BufferGroup::Misc;
				bdesc.usage = GpuApi::BUT_Readback;
				m_globalOcclusionReadbackBuffer = GpuApi::Buffer::Create( bdesc );
				m_globalOcclusionReadbackFence = GpuApi::GpuFence::Null();

				m_globalOcclusionResolution = readbackResolution;
				m_globalOcclusionLowResolution = readbackLowResolution;
			}
		}
	}
	else
	{
		m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );

		m_shadowsTexture = GpuApi::Texture();
		m_updatedShadowmap = GpuApi::Texture();
	}

	if( auto prev = m_scheduleReset.Exchange( Schedule::NONE ) )
	{
		Reset();

		if( prev == Schedule::HARD_RESET )
		{
			m_phase = Phase::Waiting;
		}
	}

	if( m_phase == Phase::Waiting || m_phase == Phase::Prepare )
	{
		const Uint32 numLevels = Min( rctx.GetFrameInfo().m_distantShadowsRequestedNumLevels, Config::GetDistantShadowsMaxLevels() );

		// Invalidate entire set.
		// #TODO: In this case, either want to regenerate all levels, or maybe generate the largest first, so that we
		// have valid data for the full world, and just set degenerate scale/bias for the rest.
		if( m_numLevels != numLevels )
		{
			m_numLevels			= numLevels;
			m_levelsValid		= 0;
			m_updateIteration	= 0;
			m_levelToUpdate		= 0;
		}
	}
}

void SceneCustomData_DistantShadows::Evict()
{
	const Uint32 shadowmapResolution = Config::GetDistantShadowsResolution();
	const Uint32 maxLevels = Config::GetDistantShadowsMaxLevels() + DistShadowsBlendLevelsCount;

	if ( m_shadowsTexture )
	{
		const GpuApi::TextureDesc& shadowsTextureDesc = GpuApi::GetTextureDesc( m_shadowsTexture );
		if ( shadowsTextureDesc.width != shadowmapResolution || shadowsTextureDesc.depthOrSliceNum != maxLevels )
		{
			m_shadowsTexture = GpuApi::Texture();
		}
	}
	if ( m_updatedShadowmap )
	{
		const GpuApi::TextureDesc& shadowmapDesc = GpuApi::GetTextureDesc( m_updatedShadowmap );
		if ( shadowmapDesc.width != shadowmapResolution )
		{
			m_updatedShadowmap = GpuApi::Texture();
		}
	}
	if ( m_globalOcclusionReadbackBuffer )
	{
		const Uint32 readbackResolution = shadowmapResolution/4;
		const Uint32 readbackLowResolution = readbackResolution/32;
		const Uint32 readbackSize = (readbackResolution*readbackResolution + readbackLowResolution*readbackLowResolution)*sizeof( vis::rast::DepthType );
		if ( GpuApi::GetBufferDesc( m_globalOcclusionReadbackBuffer ).size != readbackSize )
		{
			m_globalOcclusionReadbackBuffer = GpuApi::Buffer();
		}
	}
}

const AnsiChar* SceneCustomData_DistantShadows::IsRenderingReady() const
{
	// Texture was never created. Camera will probably never use distance shadows. Pretend ready
	if( m_shadowsTexture.isNull() )
		return nullptr;

	if( m_scheduleReset.GetValue() )
		return "Distant shadow reset pending";

	if( m_phase == Phase::Waiting )
	{
		m_phase = Phase::Prepare;
	}

#if defined( __EMSCRIPTEN__ )
	if( !CpLoadWaitRenderDS() ) return nullptr; // [renderwait]
#endif
	// Not went through render node yet. Not ready
	if( -1 == m_numLevels )
		return "Distant shadows not ready";

	// All cascades were generated
	if( m_numLevels != 0 && m_levelsValid != ( RED_FLAG( (Uint32)m_numLevels ) - 1 ) )
		return "Distant shadows not ready";

	return nullptr;
}

void SceneCustomData_DistantShadows::Reset()
{
	if( m_phase != Phase::Waiting )
	{
		m_phase				= Phase::Prepare;
	}

	m_numLevels				= -1;
	m_levelsValid			= 0;
	m_hasValidData			= false;

	m_shadowmapBatchStart = 0;
	m_shadowmapBatchIndex = 0;
	m_collectedProxies.Clear();

#ifndef RED_CONFIGURATION_FINAL
	m_stats = DistantShadowsStats();
	m_currentCascadeStats = DistantShadowsCascadeStats();
#endif // RED_CONFIGURATION_FINAL
}

void SceneCustomData_DistantShadows::SetupCollector( const CRenderFrameCameraStorage::SCameraData* camData, const CRenderFrameInfo* rFrameInfo )
{
	m_distantShadowsCollector.Setup( *rFrameInfo, FEATFLAG_Default, camData->GetRenderCamera(), GetScene(), &m_distantShadowsCollectorData, &camData->GetInfo() );
}

void SceneCustomData_DistantShadows::OnProxiesCollected()
{
#ifndef RED_CONFIGURATION_FINAL
	// update stats
	m_currentCascadeStats.m_meshesRendered = m_collectedProxies.Size();
#endif
}

void SceneCustomData_DistantShadows::TickWhileLoading( Bool isFirstFrame )
{
	if( isFirstFrame )
	{
		m_scheduleReset.Exchange( Schedule::HARD_RESET );
	}
}
