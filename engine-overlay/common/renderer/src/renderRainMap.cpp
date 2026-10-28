/**
* Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
* Originally written by Peter Sikachev
*/

#include "build.h"
#include "renderRainMap.h"
#include "../../redCore/include/globalModeInfo.h"
#include "../../renderData/include/materialFeatureFlags.h"
#include "../../visibility/include/visRasterizerSSEThreaded.h"
#if defined( __EMSCRIPTEN__ )
#include <emscripten/emscripten.h>
// [renderwait] 2026-10-24 warm-cache load: "Waiting for systems" spent ~2 s on "Rain map not ready" (cascades are
// built over many frames). The loading screen no longer waits for them (?loadWaitRender=1 = original wait); the
// cascades keep building after the session starts, so only rain occlusion can be incomplete for a moment.
static bool CpLoadWaitRender()
{
	static const int v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'loadWaitRender' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
	return v != 0;
}
#endif


RTTI_BEGIN_TYPE( SceneCustomData_RainMap );
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

SceneCustomData_RainMap::SceneCustomData_RainMap()
	: m_hasValidData( false )
	, m_levelToUpdate( 0 )
	, m_persistentRainMap( GpuApi::TextureRef::Null() )
	, m_levelsDirty( UINT8_MAX )
	, m_levelsValid( 0 )
	, m_tempRainMap( GpuApi::TextureRef::Null() )
	, m_forceFullDraw( false )
	, m_scheduleReset( Schedule::NONE )
{
}

SceneCustomData_RainMap::~SceneCustomData_RainMap()
{
	GpuApi::SafeRelease( m_persistentRainMap );
	GpuApi::SafeRelease( m_tempRainMap );
	m_collector.Reset();
}


void SceneCustomData_RainMap::Initialize()
{
}


void SceneCustomData_RainMap::Prepare( const SRenderNodeImplContext& rctx, const CRenderFrameCameraStorage& sceneData )
{
	PC_SCOPE( SceneCustomData_RainMap_Prepare );

	// The data need to be spawn for the whole frame, no matter how many camera's graphs we have.
	// Because of that we need to check whether there is at least one camera that needs it.
	Bool rainMapRequested = false;
	Bool hasOnscreenCamera = false;
	const auto* camStorage = rctx.GetCameraStorage();
	for ( Uint32 camIndex = 0; camIndex < camStorage->GetNumCameras(); camIndex++ )
	{
		const auto* camData = camStorage->GetCameraData( camIndex );
		if ( camData->IsOnscreen() )
		{
			hasOnscreenCamera = true;
			rainMapRequested |= camData->GetInfo().HasFeature( RFF_RainMap );
		}
	}

	// if no onscreen camera, ignore the rain map
	// for example, we don't want to clear it when in 3d map
	if ( !hasOnscreenCamera )
	{
		return;
	}

	if ( rainMapRequested )
	{
		const Uint32 resolution = Config::GGlobalRenderingSettings.RainMapShadowsResolution;
		RED_ASSERT( resolution > 0 );

		const GpuApi::TextureDesc* tempRainMapDesc = ( m_tempRainMap ? &GpuApi::GetTextureDesc( m_tempRainMap ) : nullptr );
		if ( tempRainMapDesc == nullptr || tempRainMapDesc->width != resolution )
		{
			m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );

			GpuApi::SafeRelease( m_tempRainMap );

			GpuApi::TextureDesc desc;
			desc.type = GpuWrapApi::TEXTYPE_2D;
			desc.width = resolution;
			desc.height = resolution;
			desc.numMips = 1;
			desc.format = GpuWrapApi::TEXFMT_D16U;
			desc.usage = GpuApi::TEXUSAGE_DepthStencil | GpuApi::TEXUSAGE_Samplable;
			desc.group = GpuWrapApi::TEXG_System_Environment;
			m_tempRainMap = GpuApi::CreateTexture( desc, GpuApi::TextureInitData::Uninitialized().OptimalClearDepth( 1.0f ) );

			GpuApi::SetResourceDebugName( m_tempRainMap, "TempRainMapShadowMap" );
		}

		const GpuApi::TextureDesc* persistentRainMapDesc = ( m_persistentRainMap ? &GpuApi::GetTextureDesc( m_persistentRainMap ) : nullptr );
		if ( persistentRainMapDesc == nullptr || persistentRainMapDesc->width != resolution )
		{
			m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );

			GpuApi::SafeRelease( m_persistentRainMap );

			GpuApi::TextureDesc desc;
			desc.type = GpuWrapApi::TEXTYPE_ARRAY;
			desc.width = resolution;
			desc.height = resolution;
			desc.depthOrSliceNum = NUM_RAIN_MAP_CASCADES;
			desc.numMips = 1;
			desc.format = GpuWrapApi::TEXFMT_D16U;
			desc.usage = GpuApi::TEXUSAGE_UnorderedAccess | GpuApi::TEXUSAGE_Samplable;
			desc.group = GpuWrapApi::TEXG_System_Environment;
			m_persistentRainMap = GpuApi::CreateTexture( desc );

			GpuApi::SetResourceDebugName( m_persistentRainMap, "PersistentRainMap" );
		}
	}
	else
	{
		m_scheduleReset.CompareExchange( Schedule::INVALIDATE, Schedule::NONE );

		GpuApi::SafeRelease( m_tempRainMap );
		GpuApi::SafeRelease( m_persistentRainMap );
	}

	if ( auto prev = m_scheduleReset.Exchange( Schedule::NONE ) )
	{
		Reset();

		if ( prev == Schedule::HARD_RESET )
		{
			m_phase = Phase::Waiting;
		}
	}
}

void SceneCustomData_RainMap::Evict()
{
	const Uint32 resolution = Config::GGlobalRenderingSettings.RainMapShadowsResolution;

	if ( m_tempRainMap )
	{
		const GpuApi::TextureDesc& tempRainMapDesc = GpuApi::GetTextureDesc( m_tempRainMap );
		if ( tempRainMapDesc.width != resolution )
		{
			GpuApi::SafeRelease( m_tempRainMap );
		}
	}
	if ( m_persistentRainMap )
	{
		const GpuApi::TextureDesc& persistentRainMapDesc = GpuApi::GetTextureDesc( m_persistentRainMap );
		if ( persistentRainMapDesc.width != resolution )
		{
			GpuApi::SafeRelease( m_persistentRainMap );
		}
	}
}


void SceneCustomData_RainMap::Reset()
{
	if ( m_phase != Phase::Waiting )
	{
		m_phase = Phase::Prepare;
	}

	m_levelsValid = 0;
	m_hasValidData = false;

	m_shadowmapBatchStart = 0;
	m_shadowmapBatchIndex = 0;
	m_collectedProxies.Clear();
}

void SceneCustomData_RainMap::TickWhileLoading( Bool isFirstFrame )
{
	if ( isFirstFrame )
	{
		m_scheduleReset.Exchange( Schedule::HARD_RESET );
	}
}

const AnsiChar* SceneCustomData_RainMap::IsRenderingReady() const
{
	// Texture was never created. Camera will probably never use distance shadows. Pretend ready
	if ( m_tempRainMap.isNull() )
		return nullptr;

	if ( m_scheduleReset.GetValue() )
		return "Rain map reset pending";

	if ( m_phase == Phase::Waiting )
	{
		m_phase = Phase::Prepare;
	}

	// All cascades were generated
	if ( m_levelsValid != ( RED_FLAG( (Uint32) NUM_RAIN_MAP_CASCADES ) - 1 ) )
	{
#if defined( __EMSCRIPTEN__ )
		if ( !CpLoadWaitRender() ) return nullptr; // [renderwait]
#endif
		return "Rain map not ready";
	}

	return nullptr;
}

void SceneCustomData_RainMap::SetupCollector( const CRenderFrameCameraStorage::SCameraData* camData, const CRenderFrameInfo* rFrameInfo )
{
	m_collector.Setup( *rFrameInfo, FEATFLAG_Default, camData->GetRenderCamera(), GetScene(), &m_collectorData, &camData->GetInfo() );
}