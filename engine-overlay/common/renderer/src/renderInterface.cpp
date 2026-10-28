/**
* Copyright @ 2018 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#include <cstdio>
#include "renderInterface.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <atomic>
// CP2077 web [drs]: render-height percentage (50..100) for the engine static resolution scaling path; 0 = off.
// The page sets it through the exported cp2077_set_res_percent (?srs=N fixed, ?drs=1 frame-time controller in the
// loader). Read every frame by GetDynamicResolutionSetup without any thread hop.
static std::atomic< int > g_cpWebResPercent{ 0 };
extern "C" EMSCRIPTEN_KEEPALIVE void cp2077_set_res_percent( int percent )
{
	g_cpWebResPercent.store( percent <= 0 ? 0 : ( percent < 50 ? 50 : ( percent > 100 ? 100 : percent ) ), std::memory_order_relaxed );
}
extern "C" EMSCRIPTEN_KEEPALIVE int cp2077_get_res_percent() { return g_cpWebResPercent.load( std::memory_order_relaxed ); }
#endif
#include "../../renderData/include/renderVideo.h"
#include "../../renderData/include/rendererDevConfig.h"
#include "../../redFileSystem/include/fileSys.h"

#include "renderViewport.h"
#include "gameplayFx.h"
#include "particleSimulationController.h"
#include "particleDPLTexture.h"
#include "rainSystemController.h"
#include "renderLodBudgetSystem.h"
#include "renderSkinningData.h"
#include "renderScene.h"
#include "renderPostProcess.h"
#include "renderGeometryBatcher.h"
#include "renderDistantLightBatcher.h"
#include "renderSkinManager.h"
#include "renderVolumetricFogManager.h"
#include "renderGpuNoiseManager.h"
#include "renderTexture.h"
#include "renderMaterial.h"
#include "renderMesh.h"
#include "renderCommandHandler.h"
#include "renderBatchData.h"
#include "renderVideo.h"
#include "renderSkinProfile.h"
#include "renderHairProfile.h"
#include "renderGradient.h"
#include "renderFoliageProfile.h"
#include "renderMeshCloth.h"
#include "renderIES.h"
#include "renderSurfaceCache.h"
#include "renderSurfaceGenerator_MultilayerProxy.h"

#include "renderViewportWindowPC.h"
#ifdef __EMSCRIPTEN__
#include "viewport-window-web.h"
#endif
#include "renderViewportWindowOrbis.h"
#include "renderViewportWindowDurango.h"

#include "renderShaderPair.h"

#include "renderCollector.h"

#include "../../shaderCache/include/shaderCacheCompilationUtils.h"
#include "../../shaderCache/include/shaderCacheManager.h"

#ifdef RED_PLATFORM_WINPC
#	include "../../redCore/include/processRunner.h"	// requesting runtime shader recompilation
#endif // RED_PLATFORM_WINPC

#include "renderHiZ.h"
#include "clusteredDeferred.h"
#include "renderGraphCache.h"
#include "renderShader.h"
#include "renderGIManager.h"
#include "renderSurfaceCache.h"
#include "renderVideoPlayer.h"
#include "renderBRDF.h"
#include "renderReflectionProbeManager.h"
#include "renderDismembermentData.h"

#include "../../core/include/cacheFilenames.h"
#include "../../redSystem/include/stopWatch.h"
#include "../../redCore/include/globalModeInfo.h"
#include "../../redCore/include/commandline.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../redContainers/include/fixedArray.h"
#include "../../redReflection/include/gatheredResource.h"

#include "../../renderData/include/renderEngineHandler.h"
#include "renderDynamicTextureManager.h"
#include "renderDecalMesh.h"

#include "fontCache.h"

#ifdef USE_RAY_TRACING
#include "renderStateObjectManager.h"
#include "renderAccelerationStructure.h"
#endif

#ifdef USE_DLSS
#include "ngx.h"
#endif
#include "../../resource/include/2darray.h"
#include "renderProxyLightVolume.h"
#include "../../redConfig/include/configVarStorage.h"

#include "../../../../engine/shaders/include/include_downsample.fx"
#define A_CPU
#define A_HLSL
#include "../../../../engine/shaders/include/FidelityFX/ffx_a.h"
#include "../../../../engine/shaders/include/FidelityFX/ffx_spd.h"
#include "renderHistogram.h"
#include "persistentCameraData.h"
#include "../../redConfig/include/inGameConfigUtils.h"
#include "../../redConfig/include/inGameConfigSystem.h"
#include "../../redConfig/include/inGameConfigGroup.h"
#include "../../redConfig/include/inGameConfigVar.h"
#include "../../renderData/include/renderSimpleTypes.h"
#include "../../renderData/include/renderInGameSettings.h"

using red::DynArray;

red::RWSpinLock CRenderInterface::m_pendingScenesLock;

#ifdef USE_DLSS
// AppId for Cyberpunk 2077
#define NGX_APP_ID 100152211ULL
#endif


#define GAME_PIPELINE_LIBRARY_CACHE_FILENAME   "GamePipelineLibrary.cache"
#define EDITOR_PIPELINE_LIBRARY_CACHE_FILENAME "EditorPipelineLibrary.cache"

namespace Config
{
	TConfigVar<Int32> cvOutputMonitor			( "Viewport",	 "OutputMonitor",		0,										eConsoleVarFlag_Save );		// Index of output monitor enumerated by GpuApi

#ifdef USE_PROFILER
	renderDev::SConfig			developerConfig;
#endif

}

#ifdef USE_PROFILER
namespace renderDev
{
	CCameraSpyImpl		cameraSpy;
}
#endif

GPUAPI_API extern Float GRenderSettingsMipBias;

void RenderFrameCommandLists::PrepareForFrame( Uint32 numCommandLists )
{
#ifdef RED_ASSERTS_ENABLED
	for ( const GpuApi::CommandListRef& cl : m_commandLists )
	{
		RED_ASSERT(cl.isNull(), "Frame command lists were not cleared");
	}
#endif

	m_commandLists.Resize( numCommandLists );
	m_nextFlushStart = 0;


	for ( Uint32 i = 0; i < numCommandLists; ++i )
	{
		m_commandLists[i] = GpuApi::CommandListRef::Null();
	}
}

void RenderFrameCommandLists::SetCommandList( Uint32 index, GpuApi::CommandListRef commandList )
{
	RED_ASSERT(index < m_commandLists.Size(), "Invalid frame command list index %u >= %u", index, m_commandLists.Size());
	RED_ASSERT(commandList.isNull() || m_commandLists[index].isNull(), "Frame command list %u already set", index);
	m_commandLists[index] = commandList;
}

GpuApi::CommandListRef RenderFrameCommandLists::GetCommandList( Uint32 index ) const
{
	RED_ASSERT(index < m_commandLists.Size(), "Invalid frame command list index %u >= %u", index, m_commandLists.Size());
	return m_commandLists[index];
}

void RenderFrameCommandLists::Submit( const char* scopeName, Uint32 upToIndex, GpuApi::CommandListSyncType sync, job::Builder& builder )
{
	RED_ASSERT(upToIndex < m_commandLists.Size(), "Invalid flush range");

	Uint32 flushCount = upToIndex - m_nextFlushStart + 1;
	GpuApi::CloseAndSubmitCommandLists( scopeName, red::ArraySpan<GpuApi::CommandListRef>( &m_commandLists[m_nextFlushStart], flushCount ), sync, builder );

	builder.DispatchFenceExplicitly();

	builder.DispatchJob("RenderFrameCommandLists_FlushGraphics_Epilogue", [this,upToIndex]( const job::RunContext& context )
	{
		for(Uint32 i = m_nextFlushStart; i <= upToIndex; ++i)
		{
			m_commandLists[i] = GpuApi::CommandListRef::Null();
		}

		m_nextFlushStart = upToIndex + 1;
	} );
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const Uint32 DOWNSAMPLING_GLOBAL_ATOMIC_BUFFER_SIZE = ( Uint32 )64_KB;
static const Uint32 DOWNSAMPLING_GLOBAL_ATOMIC_COUNT = DOWNSAMPLING_GLOBAL_ATOMIC_BUFFER_SIZE / ( 8 * sizeof( Uint32 ) );

CMipmapGenerator::CMipmapGenerator( CRenderInterface* renderInterface ) :
	m_renderInterface( renderInterface ),
	m_nextDownsamplingGlobalAtomicIndex( 0 )
{
}

bool CMipmapGenerator::Init()
{
	GpuApi::BufferDesc desc;
	desc.size = DOWNSAMPLING_GLOBAL_ATOMIC_BUFFER_SIZE;
	desc.structureStride = 8 * sizeof( Uint32 );
	desc.category = GpuWrapApi::BCC_StructuredUAV;
	desc.usage = GpuApi::BUT_Default;
	desc.group = GpuWrapApi::BufferGroup::System;
	m_downsamplingGlobalAtomic = GpuApi::CreateBuffer( desc, GpuApi::BufferInitData::ClearToZero() );
	GpuApi::SetResourceDebugName( m_downsamplingGlobalAtomic, "Downsampling global atomic" );

	return true;
}

CMipmapGenerator::~CMipmapGenerator()
{
	GpuApi::SafeRelease( m_downsamplingGlobalAtomic );
}

CRenderShaderCompute* CMipmapGenerator::GetComputeShader( const GpuApi::TextureDesc& textureDesc ) const
{
	static constexpr TStaticMetaEntryHash shaderNames[] =
	{
		SHADER_NAME( "m_downsample" ), 
		SHADER_NAME( "m_downsample_SquarePow2" ), 
		SHADER_NAME( "m_downsample_Array" ), 
		SHADER_NAME( "m_downsample_SquarePow2_Array" ), 

		// SRGB
		SHADER_NAME( "m_downsample_SRGB" ), 
		SHADER_NAME( "m_downsample_SquarePow2_SRGB" ), 
		SHADER_NAME( "m_downsample_SRGB_Array" ), 
		SHADER_NAME( "m_downsample_SquarePow2_SRGB_Array" ), 

		// 1channel
		SHADER_NAME( "m_downsample_1channel" ), 
		SHADER_NAME( "m_downsample_SquarePow2_1channel" ), 
		SHADER_NAME( "m_downsample_1channel_Array" ), 
		SHADER_NAME( "m_downsample_SquarePow2_1channel_Array" ), 
		
		// 2channels
		SHADER_NAME( "m_downsample_2channels" ), 
		SHADER_NAME( "m_downsample_SquarePow2_2channels" ), 
		SHADER_NAME( "m_downsample_2channels_Array" ), 
		SHADER_NAME( "m_downsample_SquarePow2_2channels_Array" ), 
	};

	Uint8 index = 0;

	if( textureDesc.width == textureDesc.height && red::IsPowerOf2( textureDesc.width ) )
	{
		// _SquarePow2
		index |= RED_FLAGS( 0 );
	}
	if( GpuWrapApi::IsTextureFormatSRGB( textureDesc.format ) )
	{
		// _SRGB
		index |= 4;
	}
	else if( 
		textureDesc.format == GpuWrapApi::TEXFMT_A8_Unorm ||
		textureDesc.format == GpuWrapApi::TEXFMT_R8_Unorm ||
		textureDesc.format == GpuWrapApi::TEXFMT_L8_Unorm ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16_Unorm ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16_Uint ||
		textureDesc.format == GpuWrapApi::TEXFMT_R32_Uint ||
		textureDesc.format == GpuWrapApi::TEXFMT_R32_Float ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16_Float ||
		textureDesc.format == GpuWrapApi::TEXFMT_R8_Uint )
	{
		// _1channel
		index |= 8;
	}
	else if( 
		textureDesc.format == GpuWrapApi::TEXFMT_R8G8_Unorm ||
		textureDesc.format == GpuWrapApi::TEXFMT_R32G32_Uint ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16G16_Uint ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16G16_Sint ||
		textureDesc.format == GpuWrapApi::TEXFMT_R16G16_Float ||
		textureDesc.format == GpuWrapApi::TEXFMT_R32G32_Float ||
		textureDesc.format == GpuWrapApi::TEXFMT_D24S8 ||
		textureDesc.format == GpuWrapApi::TEXFMT_D32FS8 )
	{
		// _2channels
		index |= 12;
	}
	if ( textureDesc.type == GpuWrapApi::TEXTYPE_ARRAY || textureDesc.type == GpuWrapApi::TEXTYPE_CUBE )
	{
		// _Array
		index |= RED_FLAGS( 1 );
	}

	RED_ASSERT( index < RED_ARRAY_COUNT( shaderNames ), "Invalid downsample shader name index [%u]", index );
	const TStaticMetaEntryHash shaderName = shaderNames[index];
	return m_renderInterface->GetShader( shaderName )->AsCompute();
}

//----

CRenderInterface::CRenderInterface()
	: m_deviceInitialized( false )
	, m_gpuDeviceInitialized( false )
	, m_lastTickDelta( 0.0f )
	, m_timeScale( 1.0f )
	, m_engineTickDelta( 0.0f )
	, m_lastCpuFrameTime( 0.0f )
	, m_currentDeviceState( ERDS_Operational )
	, m_lastTextureEvictionTime( 0.0 )
	, m_motionBlurScale( 1.0f )
	, m_cachedExposureFadeout( 1.0f )
	, m_isAsyncMaterialCompilationEnabled( true )
#ifndef RED_PLATFORM_CONSOLE
	, m_canUseCache( true )
#endif
	, m_imGuiVertexBufferSize( 0 )
	, m_imGuiIndexBufferSize( 0 )
	, m_clothsPendingUpdate( rend::PoolRenderMesh() )
	, m_tickCounter( 0 )
	, m_lastShaderRecompilation( 0 )
	, m_enquedShaderRecompilation( false )
	, m_destructionBuffers( rend::PoolRenderMesh() )
	, m_pendingSceneIndex( 0 )
{
}

CRenderInterface::~CRenderInterface()
{
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGroupDisplay );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGropuBasic );

#ifdef RED_PLATFORM_WINPC
	InGameConfig::System::GetInstance().UnregisterNotifier( m_igcNotifierAcceptOrSave );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierQuickPresets );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGropuAdvanced );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGropuRayTracing );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGropuDynamicResolution );
	InGameConfig::System::GetInstance().UnregisterListener( m_igcNotifierGropuDisplay );
	InGameConfig::System::GetInstance().UnregisterValidator( m_igcValidatorRayTracingToggle );
	InGameConfig::System::GetInstance().UnregisterValidator( m_igcValidatorDLSS );
	InGameConfig::System::GetInstance().UnregisterValidator( m_igcValidatorDRSMinimalRes );
	InGameConfig::System::GetInstance().UnregisterValidator( m_igcValidatorDRSMaximalRes );
#endif

	GpuApi::SafeRelease( m_computeConstantBuffer );
	GpuApi::SafeRelease( m_sharedConstantBuffer );

	m_gameSaveThumbnails.Clear();

	// Delete Direct3D interface
	CloseDevice();
}

Bool CRenderInterface::CanUseResourceCache() const
{
#ifdef RED_PLATFORM_CONSOLE
	return true;
#else
	return m_canUseCache;
#endif
}

void CRenderInterface::Register( red::UniquePtr< IRenderEngineHandler >&& engineHandler )
{
	m_resourceEngineHandler = std::move( engineHandler );

	RED_FATAL_ASSERT(m_resourceEngineHandler, "Engine handler required");
}

#ifdef RED_PLATFORM_WINPC

void ProcessInGameEntryChangeListener_Display( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason changeReason )
{
	if ( varName == RED_NAME_CONSTEXPR_NOREG( "MaximumFPS_OnOff" ) )
	{
		rend::settings::callbacks::OnMaximumFPSOnOffChanged();
	}
}

void ProcessInGameEntryChangeListener_Graphics( CName groupPath, CName varName, const InGameConfig::VarType varType, const InGameConfig::ChangeReason changeReason )
{
	if ( varName == RED_NAME_CONSTEXPR_NOREG( "LODPreset" ) )
	{
		return;
	}

	if ( rend::settings::Valid() )
	{
		// If for some reason it come to this. Should not have cause the listener is set to lower group than QuickPresets
		// option resides in and InGameConfig seems to not spread listeners from parents to children.
		if ( varName == RED_NAME_CONSTEXPR_NOREG( "QuickPresets" ) )
		{
			return;
		}

		using namespace InGameConfig;

		System &system = System::GetInstance();
		Var &var = system.GetVar( groupPath, varName );

		//
		// Someone changed a variable manually, not via QuickPresets - set QuickPresets to 'Custom'.
		//
		const Source c_source = var.SourceOfChange();
		const Bool c_fromPreset = c_source == Source::PresetCode || c_source == Source::PresetSettings;
		if ( !c_fromPreset )
		{
			system.GetVarListString( RED_NAME_CONSTEXPR( "/graphics" ), RED_NAME_CONSTEXPR( "QuickPresets" ) ).SetIndex( 0, c_source );
		}

		//
		// Hide/Show RTX variables.
		//
		if ( varName == RED_NAME_CONSTEXPR_NOREG( "RayTracing" ) )
		{
			rend::settings::callbacks::OnProcessRayTracingTabVisibility();
		}

		//
		// Hide/Show DRS/DLSS variables.
		//
		if ( varName == RED_NAME_CONSTEXPR_NOREG( "DynamicResolutionScaling" ) )
		{
			rend::settings::callbacks::OnDRSChanged();
		}

		if ( varName == RED_NAME_CONSTEXPR_NOREG( "DLSS" ) )
		{
			rend::settings::callbacks::OnDLSSChanged();
		}

		if( varName == RED_NAME_CONSTEXPR_NOREG( "StaticResolutionScaling" ) )
		{
			rend::settings::callbacks::OnSRSChanged();
		}
	}
}

#endif
Bool CRenderInterface::Init()
{
	// Create rendering GpuApi environment
	if ( !GpuApi::InitEnv() )
	{
		RED_LOG_WARNING( "Renderer: Unable to initialize graphics environment. Make sure driver is intalled." );
		return false;
	}

	// Register device notify
	GpuApi::RegisterDeviceNotify( this );

	RED_FATAL_ASSERT( !m_gpuDeviceInitialized );

	GpuApi::DeviceParams gpuDeviceParams;
	gpuDeviceParams.isEditor = !IsGameMode();
#ifdef RED_PLATFORM_WINPC // Note: Would want to use USE_PIPELINE_LIBRARY but can't
	// IsGameMode() is used in order to establish whether renderer was launched from editor or not.
	// Pipeline library cache files must be generated separately for each of the case.
	const char* libraryName = IsGameMode() ? GAME_PIPELINE_LIBRARY_CACHE_FILENAME : EDITOR_PIPELINE_LIBRARY_CACHE_FILENAME;
	gpuDeviceParams.pipelineLibraryPath = red::paths::GetUserCacheDirectory().AddFilePath( libraryName );
#endif
	if ( !GpuApi::InitDevice( gpuDeviceParams ) )
	{
		RED_LOG_WARNING( "Renderer: Failed to initialize render device." );
		return false;
	}
	m_gpuDeviceInitialized = true;

	m_skinProfilesManager		= red::CreateUniquePtr<CSkinProfilesManager>();
	m_hairProfilesManager		= red::CreateUniquePtr<CHairProfilesManager>();
	m_foliageProfilesManager	= red::CreateUniquePtr<CFoliageProfilesManager>();
	m_gradientManager			= red::CreateUniquePtr<CGradientManager>();

	m_hierarchicalDepthBuffer	= red::CreateUniquePtr<CHierarchicalDepthBuffer>();
	m_clusteredRenderer			= red::CreateUniquePtr<CClusteredDeferred>();
	m_giManager					= red::CreateUniquePtr<CRenderGIManager>();
	m_distantGiManager			= red::CreateUniquePtr<CRenderDistantGIManager>();
	m_reflectionProbeManager	= red::CreateUniquePtr<CReflectionProbeManager>();
	m_dynamicTextureManager		= red::CreateUniquePtr<CDynamicTextureManager>();
	m_iesProfile				= red::CreateUniquePtr<CRenderIES>();
	m_surfaceCache				= red::CreateUniquePtr<SurfaceCache>();
	m_glyphCache				= red::CreateUniquePtr<RenderGlyphCache>();
	m_glyphCache->Initialize( 2048, 2048 );

	m_mipmapGenerator			= red::CreateUniquePtr< CMipmapGenerator >( this );
	if( !m_mipmapGenerator->Init() )
	{
		return false;
	}

	// Decal buffers initialize
	if ( !CRenderDecalMesh::CreateDecalBuffers() )
	{
		return false;
	}

	// Register callbacks for in-game settings.
	{
		auto& inGameConfigSys = InGameConfig::System::GetInstance();

#ifdef RED_PLATFORM_WINPC
		//
		// [PC] Register 'Accept' button clicked and Saving notifier.
		//
		{
			const auto notifier = [ this ] ( InGameConfig::NotificationType notificationType )
			{
				if ( !m_viewports.Empty() && rend::settings::Valid() )
				{
					if ( notificationType == InGameConfig::NotificationType::Saved )
					{
						rend::settings::callbacks::OnSave( m_viewports[ 0 ] );
					}

					if ( notificationType == InGameConfig::NotificationType::ChangesApplied )
					{
						rend::settings::callbacks::OnApply( m_viewports[ 0 ] );
					}
				}
			};

			m_igcNotifierAcceptOrSave = inGameConfigSys.RegisterNotifier( notifier );
		}

		//
		// [PC] Register 'Ray Tracing, 'Dynamic Resolution', 'Basic' and 'Advanced' group notifier.
		//
		{
			const auto notifier = [this]( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( rend::settings::Valid() )
				{
					::ProcessInGameEntryChangeListener_Graphics( groupPath, varName, varType, changeReason );
				}
			};

			m_igcNotifierGropuBasic = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics/basic" ), notifier );
			m_igcNotifierGropuAdvanced = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics/advanced" ), notifier );
			m_igcNotifierGropuRayTracing = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics/raytracing" ), notifier );
			m_igcNotifierGropuDynamicResolution = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics/dynamicresolution" ), notifier );
		}

		//
		// [PC] Register 'Display' group/tab notifier.
		//
		{
			const auto notifier = [ this ] ( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( rend::settings::Valid() )
				{
					::ProcessInGameEntryChangeListener_Display( groupPath, varName, varType, changeReason );
				}
			};

			m_igcNotifierGropuDisplay = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/video/display" ), notifier );
		}

		//
		// [PC] Register 'Video' group notifier.
		//
		{
			const auto notifier = [this]( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( rend::settings::Valid() )
				{
					if ( varName == RED_NAME_CONSTEXPR( "OutputMonitor" ) )
					{
						rend::settings::callbacks::OnOutputMonitorChanged();
					}

					if ( varName == RED_NAME_CONSTEXPR( "TonemappingMidpoint" ) || varName == RED_NAME_CONSTEXPR( "MaxMonitorBrightness" ) ||
						varName == RED_NAME_CONSTEXPR( "PaperWhiteLevel" ) || varName == RED_NAME_CONSTEXPR( "Gamma" ) )
					{
						if ( !m_viewports.Empty() )
						{
							rend::settings::callbacks::OnDisplayCalibrationValuesChanged( m_viewports[ 0 ] );
						}
					}
				}
			};

			m_igcNotifierGroupDisplay = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/video/display" ), notifier );
		}

		//
		// [PC] Register 'QuickPresets' notifier.
		//
		{
			const auto notifier = [this]( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( rend::settings::Valid() && varName == RED_NAME_CONSTEXPR( "QuickPresets" ) )
				{
					rend::settings::callbacks::OnPresetChanged();
				}
			};

			m_igcNotifierQuickPresets = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics" ), notifier );
		}

		//
		// [PC] Register 'RayTracing' validator. In case someone wants to turn it On when hardware do not support it.
		//
		{
			const auto validator = [ this ] ( CName groupName, CName varName, InGameConfig::VarType type, InGameConfig::Source source )
			{
				return rend::settings::callbacks::OnRayTracingToggleValidation();
			};
			m_igcValidatorRayTracingToggle = inGameConfigSys.RegisterValidator( RED_NAME_CONSTEXPR( "/graphics/raytracing" ), RED_NAME_CONSTEXPR( "RayTracing" ), validator );
		}

		//
		// [PC] Register 'DLSS' validator. In case someone wants to turn it on when hardware do not support it
		//
		{
			const auto validator = [ this ] ( CName groupName, CName varName, InGameConfig::VarType type, InGameConfig::Source source )
			{
				return rend::settings::callbacks::OnDLSSToggleValidation();
			};
			m_igcValidatorDLSS = inGameConfigSys.RegisterValidator( RED_NAME_CONSTEXPR( "/graphics/dynamicresolution" ), RED_NAME_CONSTEXPR( "DLSS" ), validator );
		}

		//
		// [PC] Register 'DRS_MinimalResolution' validator. In order to make sure value is always <= than 'DRS_MaximalResolution'
		//
		{
			const auto validator = [ this ] ( CName groupName, CName varName, InGameConfig::VarType type, InGameConfig::Source source )
			{
				return rend::settings::callbacks::OnDRSMinimalResolutionValidation();
			};
			m_igcValidatorDRSMinimalRes = inGameConfigSys.RegisterValidator( RED_NAME_CONSTEXPR( "/graphics/dynamicresolution" ), RED_NAME_CONSTEXPR( "DRS_MinimalResolution" ), validator );
		}

		//
		// [PC] Register 'DRS_MinimalResolution' validator. In order to make sure value is always <= than 'DRS_MaximalResolution'
		//
		{
			const auto validator = [ this ] ( CName groupName, CName varName, InGameConfig::VarType type, InGameConfig::Source source )
			{
				return rend::settings::callbacks::OnDRSMaximalResolutionValidation();
			};
			m_igcValidatorDRSMaximalRes = inGameConfigSys.RegisterValidator( RED_NAME_CONSTEXPR( "/graphics/dynamicresolution" ), RED_NAME_CONSTEXPR( "DRS_MaximalResolution" ), validator );
		}
#else
		//
		// [XBOX/PS4] Register 'Display' group notifier.
		//
		{
			const auto entryChangeListenerDisplay = [ this ] ( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( !m_viewports.Empty() && IsGameMode() )
				{
					rend::settings::callbacks::OnConsoleSettingsChanged( groupPath, varName, m_viewports[ 0 ], changeReason );
				}
			};
			m_igcNotifierGroupDisplay = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/video/display" ), entryChangeListenerDisplay );
		}
		
		//
		// [XBOX/PS4] Register 'Basic' group notifier.
		//
		{
			const auto entryChangeListenerGraphics_Consoles = [this]( CName groupPath, CName varName, InGameConfig::VarType varType, InGameConfig::ChangeReason changeReason )
			{
				if ( !m_viewports.Empty() && IsGameMode() )
				{
					rend::settings::callbacks::OnConsoleSettingsChanged( groupPath, varName, m_viewports[ 0 ], changeReason );
				}
			};
			m_igcNotifierGropuBasic = inGameConfigSys.RegisterListener( RED_NAME_CONSTEXPR( "/graphics/basic" ), entryChangeListenerGraphics_Consoles );
		}
#endif
	}

	// Initialized
	return true;
}

void CRenderInterface::Flush()
{
	// if you add any behaviour to Flush(), make sure it does not interfere with the rendering systems when doing shutdown (in CloseDevice() )

	if ( GetCommands() )
	{
		static_cast<CRenderCommandHandler*>( GetCommands() )->SyncWithRenderCommands();
	}
}

void CRenderInterface::Sync( job::Builder& builder )
{
	// if you add any behaviour to Flush(), make sure it does not interfere with the rendering systems when doing shutdown (in CloseDevice() )

	if ( GetCommands() )
	{
		static_cast<CRenderCommandHandler*>(GetCommands())->SyncWithRenderCommands( builder );
	}
}


#ifdef RENDER_ENABLE_RESOURCE_EVICT

void CRenderInterface::RequestEvictOnNextSceneRender()
{
	m_evictOnRenderRequested.SetValue( true );
}

void CRenderInterface::ProcessEvictOnRender( CRenderSceneEx* scene )
{
	m_renderFlowAllocator->ClearAllCaches();
	if ( scene != nullptr )
	{
		scene->GetCameraStorage()->EvictCustomData();
	}
}

#endif


void CRenderInterface::Unsafe_DebugDumpRenderCommandQueue()
{
}

void CRenderInterface::Tick( Float timeDelta, Float timeScale, Float lastCpuFrameTime )
{
	// Update last tick delta
	m_engineTickDelta = Max( 0.f, timeDelta );
	m_lastTickDelta = Max( 0.f, timeDelta * timeScale );
	m_timeScale = timeScale;
	m_lastCpuFrameTime = Max( 0.f, lastCpuFrameTime );

	for ( Uint32 i=0; i<m_viewports.Size(); i++ )
	{
		m_viewports[i]->UpdateViewportProperties();
	}

	if( m_reflectionProbeManager )
	{
		m_reflectionProbeManager->ResolveStreaming();
	}

	if( m_renderShaderMap )
	{
		m_renderShaderMap->Tick( timeDelta );
	}

	if ( m_debugDrawer )
	{
		m_debugDrawer->SetOverflow( false );
	}

#ifdef USE_RAY_TRACING
	if ( m_accelerationStructureManager )
	{
		m_accelerationStructureManager->SetOverflow( false );
	}
#endif

	{
		SetDynamicCameraOverflow( false );
	}
}

void CRenderInterface::RecalculateTextureStreamingSettings()
{
	// TODO
}

void CRenderInterface::ReloadEngineShaders()
{
#ifdef RED_PLATFORM_WINPC
	RED_LOG( "Renderer: Reloading main shaders" );

	// Reload materials
	if ( m_resourceEngineHandler )
	{
		m_resourceEngineHandler->ReloadEngineShaders();
	}

#endif // RED_PLATFORM_WINPC
}

void CRenderInterface::EnqueueReloadSimpleShaders()
{
	if (!m_enquedShaderRecompilation)
	{
		// If the difference is greater than the threshold ( 5 frames ) or initial bake at startup
		constexpr Uint64 tThreshold = 5;
		if ((m_tickCounter - m_lastShaderRecompilation) > tThreshold || m_lastShaderRecompilation == 0)
		{
			m_enquedShaderRecompilation = true;
		}
	}
}

void CRenderInterface::FlushReloadSimpleShaders()
{
#ifdef RED_PLATFORM_WINPC

	// Is the recompilation scheduled ?
	if ( !m_enquedShaderRecompilation )
	{
		return;
	}
	else
	{
		m_enquedShaderRecompilation = false;
	}

	RED_LOG( "Renderer: Reloading simple shaders" );

	// clean up the log a bit, so you can actually see errors from last shader recompilation
	RED_LOG( "...Reloading simple shaders started..." );

	// Flush render thread
	Flush();

	const red::AbsolutePath& shaderCachePath = GStaticShaderCache->GetShaderCache()->GetPath();
	const red::AbsolutePath& engineRootPath = GFileManager->GetEngineRoot();
	const red::AbsolutePath& rootPath = engineRootPath.AddDirPath( ".." );

	const Bool useLocalCompiler = Config::cvShaderCompilerUseLocalCompiler.Get();

	red::AbsolutePath appPath;
	if ( !GetShaderCompilerToolPath( useLocalCompiler, rootPath, engineRootPath, STATIC_SHADER_COMPILER_FILE_NAME, appPath ) )
	{
		m_lastShaderRecompilation = m_tickCounter;
		return;
	}

	// PC. The staticShaderCompiler is simplified and only requires engine root for proper work
	// if you want to override some of the parameters, please see staticShaderCompiler project for instructions
	// or run staticShaderCompiler.exe with no parameters to see usage

	String arguments = String::Printf( "-platform=pc -engineRoot=%hs -dumpAssembly=%hs -optimize=%hs %hs",
						engineRootPath.AsChar(),
						Config::cvShaderCompilerDumpAssembly.Get() ? "true" : "false",
						Config::cvShaderCompilerOptimize.Get() ? "true" : "false",
						Config::cvShaderCompilerForceRecompile.Get() ? "-force" : "" );

	Uint32 timeout = 1000 * 60 * 4; // 4 min

	CProcessRunner runner;
	String compilerOutput;

	runner.Run( appPath, Utf16String( arguments.AsChar() ), red::utils::GetCurrentWorkingDirectory() );
	runner.WaitForFinish( timeout );
	runner.LogOutput( CProcessRunner::ELT_String, String::EMPTY(), &compilerOutput );
	red::DynArray< String > compilerOutputLines{ rend::PoolShaderCompilation() };
	compilerOutput.Slice( compilerOutputLines, "\r\n" );
	for ( const auto& line : compilerOutputLines )
	{
		if ( line.Empty() )
		{
			continue;
		}

		if ( line.BeginsWith("error:") )
		{
			RED_LOG_ERROR( "%s", line.AsChar() );
		}
		else
		{
			RED_LOG( "%s", line.AsChar() );
		}
	}
	runner.Terminate();

	RED_VERIFY( GStaticShaderCache->Reload( shaderCachePath ) );
	if ( m_renderShaderMap )
	{
		RED_VERIFY( m_renderShaderMap->Reload( GStaticShaderCache->GetShaderCache() ) );
	}

	m_lastShaderRecompilation = m_tickCounter;

	RED_LOG( "...Reloading simple shaders finished..." );
#endif // RED_PLATFORM_WINPC
}

void CRenderInterface::RelinkSimpleShaders()
{
	// Setup texture compression on GPU side
	{
		CRenderShaderBase* shader = GetShader( SHADER_NAME( "m_textureCompressBC1" ) );
		RED_FATAL_ASSERT( nullptr != shader, "Should be here" );
		CRenderShader* renderShader = shader->GetRenderShader( CRenderShaderBase::eCOMPUTE ).Get();
		GpuApi::SetupTextureCompressionShaders( renderShader->GetShader(), 0 );
	}
	{
		CRenderShaderBase* shader = GetShader( SHADER_NAME( "m_textureCompressBC3" ) );
		RED_FATAL_ASSERT( nullptr != shader, "Should be here" );
		CRenderShader* renderShader = shader->GetRenderShader( CRenderShaderBase::eCOMPUTE ).Get();
		GpuApi::SetupTextureCompressionShaders( renderShader->GetShader(), 1 );
	}
	{
		CRenderShaderBase* shader = GetShader( SHADER_NAME( "m_textureCompressBC4" ) );
		RED_FATAL_ASSERT( nullptr != shader, "Should be here" );
		CRenderShader* renderShader = shader->GetRenderShader( CRenderShaderBase::eCOMPUTE ).Get();
		GpuApi::SetupTextureCompressionShaders( renderShader->GetShader(), 2 );
	}
	{
		CRenderShaderBase* shader = GetShader( SHADER_NAME( "m_textureCompressBC5" ) );
		RED_FATAL_ASSERT( nullptr != shader, "Should be here" );
		CRenderShader* renderShader = shader->GetRenderShader( CRenderShaderBase::eCOMPUTE ).Get();
		GpuApi::SetupTextureCompressionShaders( renderShader->GetShader(), 3 );
	}

}

void CRenderInterface::ReloadTextures()
{
	RED_LOG( "Renderer: Reloading textures" );

	// Cancel pending streaming before reloading textures :)
	{
		// Flush renderer
		GRender->Flush();
	}

	// Reload
	if ( m_resourceEngineHandler )
	{
		m_resourceEngineHandler->ReloadTextures();
	}
}

void CRenderInterface::RecreatePlatformResources()
{
	// Flush render thread
	Flush();

	// recreate volumetric fog manager
	if ( m_volumetricFogManager )
	{
		m_volumetricFogManager = nullptr;
		m_volumetricFogManager = red::CreateUniquePtr<CRenderVolumetricFogManager>();
	}

	if ( m_gpuNoiseManager )
	{
		m_gpuNoiseManager = nullptr;
		m_gpuNoiseManager = red::CreateUniquePtr<CRenderGpuNoiseManager>();
	}

	if ( m_renderFlowAllocator )
	{
		m_renderFlowAllocator = nullptr;
		m_renderFlowAllocator = red::CreateUniquePtr<CRenderFlowResourceAllocator>();
	}

#ifdef USE_RAY_TRACING
	if ( m_renderStateObjectManager )
	{
		m_renderStateObjectManager = nullptr;
		m_renderStateObjectManager = red::CreateUniquePtr<CRenderStateObjectManager>();
	}

	if ( m_accelerationStructureManager )
	{
		m_accelerationStructureManager = nullptr;
		m_accelerationStructureManager = red::CreateUniquePtr<CRenderAccelerationStructure>();
	}
#endif

#ifdef USE_DLSS
	if ( m_ngx )
	{
		m_ngx = nullptr;
		m_ngx = red::CreateUniquePtr<NGX>();
		m_ngx->Init( NGX_APP_ID );
	}
#endif
}

Bool CRenderInterface::InitWithoutViewport( Uint32 width, Uint32 height, EViewportWindowMode windowMode, Bool vsync )
{
	if ( m_deviceInitialized == false )
	{
		if ( !InitDevice( width, height, windowMode == VWM_Fullscreen, vsync ) )
		{
			// Failed to initialize D3D
			return false;
		}

		m_deviceInitialized = true;
	}
	return true;
}

ViewportHandle CRenderInterface::CreateViewport( void* TopLevelWindow, void* ParentWindow, const String& title, Uint32 width, Uint32 height, EViewportWindowMode windowMode, Bool vsync /*=false*/, Int32 outputMonitorIdx /*= 0*/, Int32 hdrMode /*= 0*/ )
{
	RED_FATAL_ASSERT( ::SIsMainThread() );

	if ( windowMode == VWM_EditorPreview )
	{
		RED_FATAL_ASSERT( hdrMode == GpuWrapApi::eHDRMode::HDRMODE_None, "We do not handle HDR in the editor viewports." );
	}

	RenderViewport::AdjustPlatformResolution( width, height );

	RED_FATAL_ASSERT( m_gpuDeviceInitialized );

	ViewportWindowPtr window = nullptr;

#if defined __EMSCRIPTEN__
	{
		auto browserWindow = red::CreateUniquePtr<ViewportWindowWeb>(width, height);
		if (!browserWindow->IsReady()) return ViewportHandle();
		window = std::move(browserWindow);
	}
#elif defined RED_PLATFORM_WINPC
	window = red::CreateUniquePtr< ViewportWindowPC >( (HWND)ParentWindow, title, width, height, (EViewportWindowMode)windowMode );
#elif defined RED_PLATFORM_DURANGO
	window = red::CreateUniquePtr< ViewportWindowDurango >( width, height );
#elif defined RED_PLATFORM_ORBIS
	window = red::CreateUniquePtr< ViewportWindowOrbis >( width, height );
#endif

	RED_FATAL_ASSERT( window, "Window wasn't created");

	// Initialize device if this is the first viewport created
	if ( m_viewports.Empty() )
	{
		if ( !InitWithoutViewport( width, height, windowMode, vsync ) )
		{
			return ViewportHandle();
		}
	}

	RenderViewportInitParams params;
	params.width = width;
	params.height = height;
	params.windowMode = windowMode;
	params.window = std::move( window );
	params.renderWidth = width;
	params.renderHeight = height;
	params.hdrMode = ( GpuWrapApi::eHDRMode )hdrMode;
	params.outputMonitor = ( outputMonitorIdx == -1 ) ? 0 : outputMonitorIdx;
#ifdef RED_PLATFORM_CONSOLE
	// On Consoles we never ever wanna allow no vsync, so let's default to 2 (30hz). Only strong next gens (XSX, PS5) in quality mode are using 60hz.
	params.vsyncInterval = 2;
#endif
	if ( rend::settings::Valid() )
	{
		params.gamma = rend::settings::video_tab::GetGamma();
		params.hdrMaxMonitorBrightness = rend::settings::video_tab::GetMaxMonitorBrightness( outputMonitorIdx );
		params.hdrTonemappingMidpoint = rend::settings::video_tab::GetTonemappingMidpoint();
		params.hdrPaperWhite = rend::settings::video_tab::GetPaperWhiteLevel();
	}


#ifdef RED_PLATFORM_WINPC
	//
	// On PC we load DRS & DLLS setup from local user settings.
	//
	if ( rend::settings::Valid() )
	{
		params.dlssEnabled = rend::settings::graphics_tab::GetDLSSEnabled();
		params.dlssQualityPresetIdx = rend::settings::graphics_tab::GetDLSSQuality();
		params.drsEnabled = rend::settings::graphics_tab::GetDRSEnabled();
		params.drsMaximalResolutionPercentage = rend::settings::graphics_tab::GetDRSMaximalResolutionPercentage();
		params.drsMinimalResolutionPercentage = rend::settings::graphics_tab::GetDRSMinimalResolutionPercentage();
		params.drsTargetFPS = rend::settings::graphics_tab::GetDRSTargetFPS();
		params.srsEnabled = rend::settings::graphics_tab::GetSRSEnabled();
		params.srsResolutionPercentage = rend::settings::graphics_tab::GetSRSResolutionPercentage();
		params.vsyncInterval = rend::settings::video_tab::GetVSyncInterval( params.outputMonitor );
		params.fpsClamp = rend::settings::video_tab::GetFPSClamp();
	}
#endif

	// Create viewport wrapper
	ViewportHandle viewport = red::CreateSharedPtr< RenderViewport >( params );
	m_viewports.PushBack( static_cast< RenderViewport* >( viewport.Get() ) );

	return viewport->IsNull() ? ViewportHandle() : viewport;
}

ViewportHandle CRenderInterface::CreateGameViewport( const String& title, Uint32 width, Uint32 height, EViewportWindowMode windowMode, Int32 hdrMode, Int32 monitorIdx )
{
	ViewportHandle viewport = CreateViewport( nullptr, nullptr, title, width, height, windowMode, false, monitorIdx, hdrMode );

	//
	// [PC/XBOX/PS4] Update in game video settings with newly created game viewport.
	//
	if ( rend::settings::Valid() )
	{
		// Not pretty that switch, not pretty ...
		// But can't link the solution if I make that check inside that callback below, and no time for refactor it is.
		const Bool isNextGenQuality = ( GpuApi::GetDeviceType() == GpuApi::eDeviceType::eXBOX_SERIES_X );
		rend::settings::callbacks::OnGameViewportCreated( viewport.Get(), isNextGenQuality );
	}

	return viewport;
}

void CRenderInterface::RequestResizeRenderSurfaces( Uint32 width, Uint32 height )
{
	m_renderCommandHandler->ResizeRenderSurfaces( width, height );
	Flush();
}

void CRenderInterface::RequestUpdateMultilayerMask( TRenderPtr<RenderMultilayerMask>& mlMask, const rend::RenderMultilayerMaskBlobHeader& header, const GpuApi::BufferRef& tiles, GpuApi::TextureRef& atlas )
{
	if ( m_renderCommandHandler )
	{
		m_renderCommandHandler->UpdateMultilayerMask( mlMask, header, tiles, atlas );
	}
}

void CRenderInterface::RequestUpdateFoliageProfileSurfaceCache( TRenderPtr<CRenderFoliageProfile>& foliageProfile )
{
	if ( m_renderCommandHandler )
	{
		m_renderCommandHandler->UpdateFoliageProfileSurfaceCache( foliageProfile );
	}
}

void CRenderInterface::ResizeRenderSurfaces( Uint32 width, Uint32 height )
{
	m_resolution.m_width 	= width;
	m_resolution.m_height	= height;
}

void CRenderInterface::SetAutoFoliageRuntimeMap( red::UniquePtr<rend::AutoFoliageRuntimeMap>& runtimeMap )
{
	m_autoFoliageRuntimeMap = std::move( runtimeMap );
}

void CRenderInterface::SPendingProxyOpPair::AddOperation( SceneStaticBVHOperationType op )
{
	if ( m_op == BVH_OP_NONE )
	{
		m_op = op;
		return;
	}

	if ( op == BVH_OP_ADDITION )
	{
		if ( m_op == BVH_OP_REMOVAL ) //if we want to add a new proxy and remove in the same frame then don't do anything
		{
			m_op = BVH_OP_NONE;
			return;
		}

		RED_ASSERT( m_op != BVH_OP_MOVE, "Move operation must never happen before proxy is Added to the scene" );

	}
	else if ( op == BVH_OP_REMOVAL )
	{
		if ( m_op == BVH_OP_ADDITION )
		{
			m_op = BVH_OP_NONE;
			return;
		}
		if ( m_op == BVH_OP_MOVE )
		{
			m_op = op;
			return;
		}
	}
}

CRenderInterface::SPendingSceneMap CRenderInterface::GetPendingProxiesAndAdvanceFrame()
{
	//RVO should return the result nicely
	//Could we return reference and walk over this without a lock in FlushPendingJobs?
	RED_SCOPE_LOCK( m_pendingScenesLock );
	CRenderInterface::SPendingSceneMap pendingProxiesPingPong = std::move( m_pendingScenes );
	m_pendingScenes = SPendingSceneMap();
	return pendingProxiesPingPong;
}

CRenderInterface::SPendingSceneMap& CRenderInterface::GetPendingProxiesNoLock()
{
	return m_pendingScenes;
}

void CRenderInterface::AddPendingProxy( const TRenderPtr<IRenderScene>& scene, const TRenderPtr<IRenderProxy>& proxy, const SceneStaticBVHOperationType pendingOp )
{
	RED_SCOPE_LOCK( m_pendingScenesLock );

	PendingProxiesHashMapKey key = { scene };
	TPendingScenesMap& pendingProxies = GetPendingProxiesNoLock().m_pendingSceneMap;
	const auto& it = pendingProxies.Find( key );
	IRenderProxyBase* proxyBase = static_cast<IRenderProxyBase*>( proxy.Get() );
	if ( it == pendingProxies.End() ) //if given scene was not found in the hash map
	{
		PendingSceneProxiesOperationsData proxyOperationsMap;
		proxyOperationsMap.m_sortedProxyOperations.Reserve( 256 );
		proxyOperationsMap.m_sortedProxyOperations.PushBack( SPendingProxyOpPair( proxy, static_cast<Uint8>( pendingOp ) ) );
		pendingProxies.Insert( key, std::move( proxyOperationsMap ) );
	}
	else
	{
		PendingSceneProxiesOperationsData& proxyOperationsMap = it.Value();
		proxyOperationsMap.m_sortedProxyOperations.PushBack( SPendingProxyOpPair( proxy, static_cast<Uint8>( pendingOp ) ) );
	}
}

void CRenderInterface::AddPendingProxies( const TRenderPtr<IRenderScene>& scene, const red::ArraySpan < TRenderPtr<IRenderProxy> >& proxies, const SceneStaticBVHOperationType pendingOp )
{
	RED_SCOPE_LOCK( m_pendingScenesLock );

	PendingProxiesHashMapKey key = { scene };
	TPendingScenesMap& pendingProxies = GetPendingProxiesNoLock().m_pendingSceneMap;
	const auto& it = pendingProxies.Find( key );
	if ( it == pendingProxies.End() ) //if given scene was not found in the hash map
	{
		PendingSceneProxiesOperationsData proxyOperationsMap;
		proxyOperationsMap.m_sortedProxyOperations.Reserve( 256 );
		for ( auto& proxy : proxies )
		{
			proxyOperationsMap.m_sortedProxyOperations.PushBack( SPendingProxyOpPair( proxy, static_cast<Uint8>( pendingOp ) ) );
		}

		pendingProxies.Insert( key, std::move( proxyOperationsMap ) );
	}
	else
	{
		PendingSceneProxiesOperationsData& proxyOperationsMap = it.Value();
		for ( auto& proxy : proxies )
		{
			proxyOperationsMap.m_sortedProxyOperations.PushBack( SPendingProxyOpPair( proxy, static_cast<Uint8>( pendingOp ) ) );
		}
	}
}

#ifdef RED_PLATFORM_WINPC
static void GetRenderSurfacesResolution( Uint32 &outWidth, Uint32 &outHeight )
{
	struct SData
	{
		static BOOL MonitorEnumProc( HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM param )
		{
			if ( rect )
			{
				SData *data = (SData*)param;
				data->AddRect( *rect );
			}
			return true;
		}

		SData()
			: maxWidth(0)
			, maxHeight(0)
			, left(0)
			, right(0)
			, top(0)
			, bottom(0)
		{}

		void AddRect( const RECT &rect )
		{
			if ( 0 == maxWidth && 0 == maxHeight )
			{
				maxWidth = (Uint32)(rect.right - rect.left);
				maxHeight = (Uint32)(rect.bottom - rect.top);
				left = rect.left;
				right = rect.right;
				top = rect.top;
				bottom = rect.bottom;
			}
			else
			{
				maxWidth = Max( maxWidth, (Uint32)(rect.right - rect.left) );
				maxHeight = Max( maxHeight, (Uint32)(rect.bottom - rect.top) );
				left = Min( left, rect.left );
				right = Max( right, rect.right );
				top = Min( top, rect.top );
				bottom = Max( bottom, rect.bottom );
			}
		}

		void GetSize( Uint32 &outWidth, Uint32 &outHeight )
		{
			#if 0
			outWidth = (Uint32)(right - left);
			outHeight = (Uint32)(bottom - top);
			#else
			outWidth = maxWidth;
			outHeight = maxHeight;
			#endif
			RED_FATAL_ASSERT( outWidth && outHeight, "Invalid size" );
		}

		Uint32 maxWidth, maxHeight;
		Int32 left, right, top, bottom;
	};

	SData data;

	// Get main display (just in case)
	Uint32 desktopWidth = 0;
	Uint32 desktopHeight = 0;
	{
		RECT desktop;
		const HWND hDesktop = GetDesktopWindow();
		if ( GetWindowRect( hDesktop, &desktop ) )
		{
			data.AddRect( desktop );
			desktopWidth = (Uint32)(desktop.right - desktop.left);
			desktopHeight = (Uint32)(desktop.bottom - desktop.top);
		}
	}

	// Enumerate all displays
	{
		EnumDisplayMonitors( nullptr, nullptr, SData::MonitorEnumProc, reinterpret_cast<LPARAM>(&data) );
	}

	// Get the result
	data.GetSize( outWidth, outHeight );

	// Limit the result to some (in)sane size
	//outWidth = Max( desktopWidth, Min( outWidth, (Uint32)(8 * 1024) ) );
	//outHeight = Max( desktopHeight, Min( outHeight, (Uint32)(8 * 1024) ) );
}
#endif

Bool CRenderInterface::InitDevice( Uint32 width, Uint32 height, Bool fullscreen, Bool vsync )
{
	RED_LOG( "Status: Initializing render device..." );

#ifdef RED_LOGGING_ENABLED
	red::StopWatch initTimer;
#endif

	RenderViewport::AdjustPlatformResolution( width, height );

	//GRenderSettingsMipBias = Config::cvTextureMipBias.Get(); // ctremblay need to be fix. DLL issue.

	RED_LOG( "Status: Initializing shaders..." );

	RED_FATAL_ASSERT( GStaticShaderCache, "No static shader cache!" );
	m_renderShaderMap = red::CreateUniquePtr<CRenderShaderMap>();
	if ( m_renderShaderMap )
	{
		Bool res = false;
		if ( GStaticShaderCache->IsInit() )
		{
			res = m_renderShaderMap->Init( GStaticShaderCache->GetShaderCache() );
		}
#if defined( RED_PLATFORM_WEB )
		std::fprintf( stderr, "CP2077 render shader map init: staticCacheInit=%u cache=%p result=%u\n",
			Uint32( GStaticShaderCache->IsInit() ), static_cast< const void* >( GStaticShaderCache->IsInit() ? GStaticShaderCache->GetShaderCache().Get() : nullptr ), Uint32( res ) );
#endif

		// Receive some shaders
		RelinkSimpleShaders();

#ifdef RED_PLATFORM_WIN64
		while ( !res )
		{
			const red::AbsolutePath& shaderCachePath =
				GStaticShaderCache->IsInit() ? GStaticShaderCache->GetShaderCache()->GetPath() :
				GFileManager->GetEngineRoot().AddFilePath( STATIC_SHADER_CACHE_FILENAME );

			const red::AbsolutePath& engineRootPath = GFileManager->GetEngineRoot();
			const red::AbsolutePath& rootPath = engineRootPath.AddDirPath( ".." );

			const Bool useLocalCompiler = Config::cvShaderCompilerUseLocalCompiler.Get();

			red::AbsolutePath appPath;
			if ( !GetShaderCompilerToolPath( useLocalCompiler, rootPath, engineRootPath, STATIC_SHADER_COMPILER_FILE_NAME, appPath ) )
			{
				RED_LOG_WARNING( "Renderer: Unable to find staticShaderCompiler.exe under path '%hs'", appPath.AsChar() );
				return false;
			}

			// PC. The staticShaderCompiler is simplified and only requires engine root for proper work
			// if you want to override some of the parameters, please see staticShaderCompiler project for instructions
			// or run staticShaderCompiler.exe with no parameters to see usage

//#define USE_CUSTOM_ARGUMENTS_FOR_STATIC_SHADER_COMPILATION
#if USE_CUSTOM_ARGUMENTS_FOR_STATIC_SHADER_COMPILATION
			const red::AbsolutePath metaPath = engineRootPath.AddFilePath( "shaders\\staticShaders.xml" );
			const red::AbsolutePath srcPath = engineRootPath.AddDirPath( "shaders\\" );
			const String arguments = String::Printf( ("-platform=pc -breakOnError -engineRoot=%ls -cache=%ls -meta=%ls -src=%ls")
				, engineRootPath.AsChar()
				, shaderCachePath.AsChar()
				, metaPath.AsChar()
				, srcPath.AsChar()
				);
#else
			UniChar buffer[512];
			red::SNPrintFUnsafe( buffer, 512, TXT("-platform=pc -breakOnError -engineRoot=%ls"), engineRootPath.AsChar() );
#endif // USE_CUSTOM_ARGUMENTS_FOR_STATIC_SHADER_COMPILATION

			Uint32 timeout = 1000 * 60 * 4; // 4 min
			CProcessRunner runner;
			if ( !runner.Run( appPath, Utf16String( buffer ), red::utils::GetCurrentWorkingDirectory()) || !runner.WaitForFinish( timeout ) || runner.GetExitCode() != 0 )
			{
				RED_LOG_ERROR( "Renderer: Shader compiler error: %hs", runner.GetFullCommandLine().AsChar() );
				break;
			}
			runner.Terminate();

			if ( GStaticShaderCache->IsInit() )
			{
				res = m_renderShaderMap->Init( GStaticShaderCache->GetShaderCache() );
			}
		}
#endif // RED_PLATFORM_WIN64
	}

	// Create normal render surfaces
	{
		Uint32 rendererWidth;
		Uint32 rendererHeight;
#ifdef RED_PLATFORM_WINPC
		GetRenderSurfacesResolution( rendererWidth, rendererHeight );
#else
		rendererWidth  = width;
		rendererHeight = height;
#endif

		m_resolution.m_width		= rendererWidth;
		m_resolution.m_height		= rendererHeight;
	}

	RED_FATAL_ASSERT( !m_renderFlowAllocator, "Expected no allocator" );
	RED_FATAL_ASSERT( !m_renderStageChunksAllocator, "Already created?" );

	m_renderFlowAllocator = red::CreateUniquePtr<CRenderFlowResourceAllocator>();
	m_renderStageChunksAllocator =	red::CreateUniquePtr<RenderStageChunkPageAllocator>();
	m_stateManager = red::CreateUniquePtr<RenderBindManager>();
	m_debugDrawer = red::CreateUniquePtr<CRenderDrawer>();
	m_renderCommandHandler = red::CreateUniquePtr<CRenderCommandHandler>();
	m_postProcess = red::CreateUniquePtr<CRenderPostProcess>();
	m_geometryBatcher = red::CreateUniquePtr<CRenderGeometryBatcher>();
	m_particleSimulationController = red::CreateUniquePtr<CParticleSimulationController>();
	m_rainSystemController = red::CreateUniquePtr<CRainSystemController>();
	m_distantLightBatcher = red::CreateUniquePtr<CRenderDistantLightBatcher>();
	m_graphCache = red::CreateUniquePtr<CRenderGraphCache>();
	m_videoPlayer = red::CreateUniquePtr<RenderVideoPlayer>();
	m_volumetricFogManager = red::CreateUniquePtr<CRenderVolumetricFogManager>();
	m_gpuNoiseManager = red::CreateUniquePtr<CRenderGpuNoiseManager>();
	m_batchDataAllocator = red::CreateUniquePtr<CBatchDataAllocator>();
	m_brdf = red::CreateUniquePtr<CRenderBRDF>();
	m_skinningManager = red::CreateUniquePtr<RenderSkinningManager>();

	m_skinningManager->Initialize();

	InitTiledDeferredConstants();
	InitSharedConstants();

	// initialize static shared data for dismemberment
	// it's needed for PS4 platform because there is no option to left not binded buffer
	RenderDismembermentData::StaticInit();
	CameraCustomData_Histogram::CreateDefaultBuffer();
	CameraCustomData_Persistent::CreateDefaultBuffer();

	if( m_particleSimulationController )
	{
		m_particleSimulationController->InitGPUResources();
	}

	RED_LOG( "Renderer: Device initialized in %1.2f sec", initTimer.GetDelta() );

#ifdef USE_PROFILER

	Bool useGpuProfiler = true;

	if ( red::CommandLine::Get().HasOption( "gpuProfiler" ) )
	{
		red::String gpuProfilerParam;
		if ( red::CommandLine::Get().GetFirstOption( "gpuProfiler", gpuProfilerParam ) && gpuProfilerParam == "0" )
		{
			useGpuProfiler = false;
		}
	}

	if ( useGpuProfiler )
	{
		m_gpuProfiler = red::CreateUniquePtr<CRenderGpuProfiler>();
	}

#endif // USE_PROFILER

	// Initialize the frame timer we need for Dynamic Resolution Scaling to get previous frame time data.
	m_GpuFrameTimer.Initialize();

	// Preload some basic materials
	if ( GetEngineHandler() )
	{
		GetEngineHandler()->GetFallbackMaterial();
		GetEngineHandler()->GetDebugMaterial();
		GetEngineHandler()->GetFontMaterial();
	}

#ifdef USE_RAY_TRACING
	m_renderStateObjectManager = red::CreateUniquePtr<CRenderStateObjectManager>();
	m_accelerationStructureManager = red::CreateUniquePtr<CRenderAccelerationStructure>();
#endif

#ifdef USE_DLSS
	{
		m_ngx = red::CreateUniquePtr<NGX>();
		m_ngx->Init( NGX_APP_ID );
	}
#endif

	CRenderFrameCameraStorage::StaticInit();

	m_gameSaveThumbnails.Initialize();

	return true;
}

void CRenderInterface::CloseDevice()
{
	// flush commands before closing various rendering systems
	Flush();

#ifdef USE_PROFILER
	m_gpuProfiler.Reset();
#endif
	m_GpuFrameTimer.Reset();

	m_videoPlayer.Reset();
	m_surfaceCache.Reset();
	m_iesProfile.Reset();
	m_mipmapGenerator.Reset();
	m_glyphCache.Reset();

	// Release Output-Merger States
	{
		RED_SCOPE_LOCK( m_OMStateMapLock );
		for ( auto pair : m_OMStateMap )
		{
			GpuApi::Release( pair.Value() );
		}
		m_OMStateMap.Clear();
	}

	RenderDismembermentData::StaticDeinit();
	CameraCustomData_Histogram::ReleaseDefaultBuffer();
	CRenderDecalMesh::ReleaseDecalBuffers();

	if ( m_skinningManager )
	{
		m_skinningManager->Uninitialize();
	}

	{
		// must clear proxies in pending scenes before batcher is destroyed
		RED_SCOPE_LOCK( m_pendingScenesLock );
		m_pendingScenes.m_pendingSceneMap.Clear();
	}

	m_stateManager.Reset();
	m_debugDrawer.Reset();
	m_renderCommandHandler.Reset();
	m_postProcess.Reset();
	m_hierarchicalDepthBuffer.Reset();
	m_clusteredRenderer.Reset();
	m_particleSimulationController.Reset();
	m_rainSystemController.Reset();
	m_geometryBatcher.Reset();
	m_distantLightBatcher.Reset();
	m_renderFlowAllocator.Reset();
	m_renderStageChunksAllocator.Reset();
	m_skinningManager.Reset();

	//this has to be released before releasing of foliageProfilesManager
	m_autoFoliageRuntimeMap.Reset();

	m_skinProfilesManager.Reset();
	m_hairProfilesManager.Reset();
	m_foliageProfilesManager.Reset();
	m_gradientManager.Reset();
	m_graphCache.Reset();
	m_renderShaderMap.Reset();
	m_brdf.Reset();

	if ( m_resourceEngineHandler )
	{
		m_resourceEngineHandler->ReleaseTexturesOnCloseDevice();
	}

	m_volumetricFogManager.Reset();
	m_gpuNoiseManager.Reset();
	m_batchDataAllocator.Reset();

	m_giManager.Reset();
	m_distantGiManager.Reset();

	m_reflectionProbeManager.Reset();
	m_dynamicTextureManager.Reset();

#ifdef USE_RAY_TRACING
	m_renderStateObjectManager.Reset();
	m_accelerationStructureManager.Reset();
#endif

	RED_LOG( "RenderShaders: %d", CRenderShader::GetNumberOfCreatedObjects() );
}

GpuApi::OMStateRef CRenderInterface::RequestOutputMergerState( const SOMState& omState )
{
	const red::THash64 hash = omState.CalcHash();

	const red::ScopedLock< red::Mutex > lock( m_OMStateMapLock );
	auto findResult = m_OMStateMap.Find( hash );
	if ( findResult != m_OMStateMap.End() )
	{
		RED_FATAL_ASSERT( findResult.Value(), "Expected Output-Merger State to be created" );
		return findResult.Value();
	}

	// Create new one
	GpuApi::OMStateRef omStateRef = GpuApi::CreateOMState( omState );
	RED_FATAL_ASSERT( omStateRef, "Failed to create Output-Merger State" );
	if ( omStateRef )
	{
		m_OMStateMap.Insert( hash, omStateRef );
		RED_FATAL_ASSERT( m_OMStateMap.Find( hash ) != m_OMStateMap.End(), "Internal error" );
	}
	return omStateRef;
}


void CRenderInterface::ViewportClosed( RenderViewport* viewport )
{
	// Remove viewport from the list
	RED_ASSERT( m_viewports.Exist( viewport ) );
	m_viewports.Remove( viewport );

	//// If this was the last viewport close the device
	//if ( !m_viewports.Size() )
	//{
	//	RED_LOG( "Renderer: Deleted last DX9 viewport, closing device") );
	//	CloseDevice();
	//}
}

void CRenderInterface::OnDeviceLost()
{
	// Release all resources

	m_currentDeviceState = ERDS_Lost_ResourcesNotReleased;

	// #TODO: Reupload blobs somehow
	// ReleaseRenderingResources();

	m_currentDeviceState = ERDS_NotReset;
}

void CRenderInterface::OnDeviceRestored()
{
	// Recreate all resources

	// Recreate all dynamic rendering resources
	for (CRenderResourceIterator i; i; ++i)
	{
		i->OnDeviceReset();
	}

	m_currentDeviceState = ERDS_Operational;

	if (m_resourceEngineHandler)
	{
		// #TODO: Reupload blobs somehow
		// m_resourceEngineHandler->ResetDevice();
	}
}

void CRenderInterface::Suspend()
{
#ifdef RED_PLATFORM_DURANGO
	GpuApi::SuspendDevice();
#endif
}

void CRenderInterface::Resume()
{
#ifdef RED_PLATFORM_DURANGO
	GpuApi::ResumeDevice();
#endif
}

void CRenderInterface::GetSkinningStats( SkinningStats& stats ) const
{
	if( m_skinningManager )
	{
		m_skinningManager->DEBUG_GetStats( &stats );
	}
}

#ifdef USE_PROFILER
renderDev::SConfig* CRenderInterface::GetDevConfig() const
{
	return &Config::developerConfig;
}

renderDev::ICameraSpy* CRenderInterface::GetCameraSpy() const
{
	return &renderDev::cameraSpy;
}
#endif



void CRenderInterface::GetPreviousFrameState( PreviousFrameState& outState ) const
{
	RED_SCOPE_SHARED_LOCK( m_previousFrameStateLock );
	outState = m_previousFrameState;
}

void CRenderInterface::UpdatePreviousFrameState()
{
	RED_SCOPE_LOCK( m_previousFrameStateLock );

	m_previousFrameState = PreviousFrameState();

	if ( m_glyphCache )
	{
		m_previousFrameState.m_wasGlyphCacheOverflow = m_glyphCache->WasOverflowLastFrame() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

#ifndef RED_CONFIGURATION_FINAL
	m_previousFrameState.m_wasBatchDataGeometryOverflow = CBatchDataAllocator::s_wasGeometryDataOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;
	m_previousFrameState.m_wasBatchDataPsoOverflow = CBatchDataAllocator::s_wasPsoDataOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;
	m_previousFrameState.m_wasBatchDataParamsOverflow = CBatchDataAllocator::s_wasParamsOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;
	m_previousFrameState.m_wasBatchDataProxyOverflow = CBatchDataAllocator::s_wasProxyDataOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;
	m_previousFrameState.m_wasBatchDataInstanceOverflow = CBatchDataAllocator::s_wasInstanceOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;

	m_previousFrameState.m_wasLocalShadowsOverflow = CRenderCollector::s_wasLocalShadowsOverflow.GetValue() ? PreviousFrameState::LOW : PreviousFrameState::NONE;
	m_previousFrameState.m_wasLocalShadowsQueueOverflow = CRenderCollector::s_wasLocalShadowsQueueOverflow.GetValue() ? PreviousFrameState::LOW : PreviousFrameState::NONE;
	m_previousFrameState.m_wasLightsAllocationOverflow = CRenderCollector::s_wasLightAllocationOverflow.GetValue() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	m_previousFrameState.m_wasRenderStageChunkAllocatorOverflow = CRenderCollector::s_wasRenderStageChunkAllocatorOverflow.GetValue() ? PreviousFrameState::CRITICAL : PreviousFrameState::NONE;
	m_previousFrameState.m_wasParticleEmitterOverflow = CRenderCollector::s_wasParticleEmitterOverflow.GetValue() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	m_previousFrameState.m_wasParticleAmountOverflow = CRenderCollector::s_wasParticleAmountOverflow.GetValue() > 0 ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
#endif // RED_CONFIGURATION_FINAL

	{
		rend::InstanceBufferStats instanceBufferStats;
		GetGeometryBatcher()->GetInstanceBufferStats( instanceBufferStats );
		m_previousFrameState.m_wasDynamicInstanceBufferOverflow = instanceBufferStats.m_dynamicBufferStats.m_wasOverflow ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
		m_previousFrameState.m_wasDynamicSkinnedInstanceBufferOverflow = instanceBufferStats.m_dynamicSkinnedBufferStats.m_wasOverflow ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
		m_previousFrameState.m_wasDecalOverflow = instanceBufferStats.m_decalParamsBufferStats.m_wasOverflow ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
		m_previousFrameState.m_wasStaticInstanceBufferOverflow = instanceBufferStats.m_staticBufferFreeSize == 0 ? PreviousFrameState::LOW : PreviousFrameState::NONE;
	}

	if( m_reflectionProbeManager )
	{
		const Int32 numProbesAboveBudget = m_reflectionProbeManager->WasOverflowLastFrame();

		if( numProbesAboveBudget > 4 )
		{
			m_previousFrameState.m_wasEnvProbeOverflow = PreviousFrameState::CRITICAL;
		}
		else if( numProbesAboveBudget > 2 )
		{
			m_previousFrameState.m_wasEnvProbeOverflow = PreviousFrameState::HIGH;
		}
		else if( numProbesAboveBudget > 0 )
		{
			m_previousFrameState.m_wasEnvProbeOverflow = PreviousFrameState::LOW;
		}
		else
		{
			m_previousFrameState.m_wasEnvProbeOverflow = PreviousFrameState::NONE;
		}
	}

	if( m_volumetricFogManager )
	{
		m_previousFrameState.m_wasFogVolumeOverflown = m_volumetricFogManager->WasOverflownLastFrame() ? PreviousFrameState::LOW : PreviousFrameState::NONE;
	}

	if( m_clusteredRenderer )
	{
		m_previousFrameState.m_wasLocalLightContactShadowsOverflow	= m_clusteredRenderer->WasContactShadowsOverflow() ? PreviousFrameState::LOW : PreviousFrameState::NONE;
	}

	if ( m_debugDrawer )
	{
		m_previousFrameState.m_wasDebugOverflow = m_debugDrawer->IsOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

	if ( m_videoPlayer )
	{
		m_previousFrameState.m_wasVideoUploadOverflow = m_videoPlayer->WasUploadOverflow() ? PreviousFrameState::LOW : PreviousFrameState::NONE;
		m_previousFrameState.m_wasSingleVideoUploadOverflow = m_videoPlayer->WasSingleUploadOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

	if ( m_giManager )
	{
		m_previousFrameState.m_wasGITetrahedronBufferOverflow = m_giManager->IsOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

#ifdef USE_RAY_TRACING
	if ( m_accelerationStructureManager )
	{
		m_previousFrameState.m_wasRaytraceOverflow = m_accelerationStructureManager->IsOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}
#endif

	if ( m_surfaceCache )
	{
		m_previousFrameState.m_wasMultilayerProxyMaterialOverflow = m_surfaceCache->GetGenerator_MultilayerProxy().IsOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

	{
		m_previousFrameState.m_wasDynamicCameraOverflow = WasDynamicCameraOverflow() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

	{
		m_previousFrameState.m_wasLightChannelsOverflow = lightVolume::WasLastFrameOverflowed() ? PreviousFrameState::HIGH : PreviousFrameState::NONE;
	}

	m_previousFrameState.m_wasReflectionsUberQualityEnabled = Config::GGlobalRenderingSettings.allowSSR_UberQuality ? PreviousFrameState::LOW : PreviousFrameState::NONE;

#ifndef RED_CONFIGURATION_FINAL
	{
		rend::DebugStats gpuStats;
		GpuApi::GetDebugStats( gpuStats );
		if ( gpuStats.GetSupport(rend::DebugStats::DSS_FRAME_INFO) )
		{
			m_previousFrameState.m_wasMissingPSO = gpuStats.m_frameInfo.m_wasMissingPSO;
			m_previousFrameState.m_didImmediateCompilePSO = gpuStats.m_frameInfo.m_didImmediateCompilePSO;
			m_previousFrameState.m_wasUploadOverflow = gpuStats.m_frameInfo.m_didOverflowUploadRing;
		}
	}
#endif
}



Uint8 CRenderInterface::AllocSceneID( IRenderScene* scene )
{
	RED_SCOPE_LOCK(m_usedSceneIDsLock);

	// Don't use sceneId == 0.
	Uint32 id = m_usedSceneIDs.FindNextClear( 1 );
	RED_ASSERT( id < 256, "Out of Scene IDs" );
	m_usedSceneIDs.Set( id );

	return Uint8(id);
}

void CRenderInterface::FreeSceneID( Uint8 sceneID, IRenderScene* scene )
{
	RED_SCOPE_LOCK( m_usedSceneIDsLock );

	RED_ASSERT( sceneID < m_usedSceneIDs.Size(), "Invalid scene ID %u", sceneID );
	m_usedSceneIDs.Clear( sceneID );
}

TRenderPtr<IRenderScene> CRenderInterface::CreateScene( IRenderScene::Type type )
{
	return TRenderPtr<IRenderScene>( RED_NEW( CRenderSceneEx )( type ) );
}

TRenderPtr<IRenderSkinningData> CRenderInterface::CreateSkinningBuffer( const CName& category )
{
	return TRenderPtr<IRenderSkinningData>( RED_NEW( CRenderSkinningData )( category ) );
}

TRenderPtr<IRenderSkinningData> CRenderInterface::CreateNonRenderSkinningBuffer( Uint32 numMatrices )
{
	return TRenderPtr<IRenderSkinningData>( RED_NEW( CRenderSkinningDataEngineBuffer )( numMatrices ) );
}

TRenderPtr<IRenderFloatTrackData> CRenderInterface::CreateFloatTrackBuffer( Uint32 numFloats, const red::DynArray< CName >& floatTrackNames )
{
	RED_ASSERT( numFloats > 0, "Empty float track data buffer" );
	return numFloats ? TRenderPtr<IRenderFloatTrackData>( RED_NEW( CRenderFloatTrackData )( numFloats, floatTrackNames ) ) : nullptr;
}

Bool CRenderInterface::IsStreamingTextures() const
{
	return false;
}

void CRenderInterface::GetPendingStreamingTextures( Uint32& outNumTextures, Uint32& outDataSize ) const
{
	outNumTextures = 0;
	outDataSize = 0;
}

Bool CRenderInterface::IsStreamingMeshes() const
{
	return m_meshStreamingStats.IsLoading();
}

void CRenderInterface::GetPendingStreamingMeshes( Uint32& outNumMeshes, Uint32& outDataSize ) const
{
	m_meshStreamingStats.GetStats( outNumMeshes, outDataSize );
}

namespace DeviceStateStateMachineHelpers
{
	ERenderingDeviceState OnLost( ERenderingDeviceState currentDeviceState )
	{
		switch ( currentDeviceState )
		{
		case ERDS_Operational:
			{
				RED_LOG( "Renderer: Device lost");
				return ERDS_Lost_ResourcesNotReleased;
			}
			break;
		case ERDS_Lost_ResourcesNotReleased:
		case ERDS_Lost_ResourcesReleased:
			{
				return currentDeviceState;
			}
			break;
		case ERDS_NotReset:
			{
				return ERDS_Lost_ResourcesReleased;
			}
			break;
		default:
			{
				RED_ASSERT( false, "Error in device state state machine or some device fuckup" );
				return currentDeviceState;
			}
		}
	}

	ERenderingDeviceState OnNotReset( ERenderingDeviceState currentDeviceState )
	{
		switch ( currentDeviceState )
		{
		case ERDS_Operational:
			{
				RED_LOG( "Renderer: Device lost");
				return ERDS_Lost_ResourcesNotReleased;
			}
			break;
		case ERDS_Lost_ResourcesNotReleased:
			{
				return currentDeviceState;
			}
			break;
		case ERDS_Lost_ResourcesReleased:
			{
				RED_LOG( "Renderer: Device reset!");
				return ERDS_NotReset;
			}
			break;
		case ERDS_NotReset:
			{
				return ERDS_NotReset;
			}
			break;
		default:
			{
				RED_ASSERT( false, "Error in device state state machine or some device fuckup" );
				return currentDeviceState;
			}
		}
	}

}

void CRenderInterface::RefreshDeviceState()
{
	// Test device state, if we are lost then do not render
	switch ( GpuApi::TestDeviceCooperativeLevel() )
	{
	case GpuApi::DEVICECOOPLVL_Operational:
		{
			RED_ASSERT( m_currentDeviceState == ERDS_Operational );
		}
		break;


		//HACK DX10 TBD device lost handling

	//case GpuApi::DEVICECOOPLVL_Lost:
	//	{
	//		m_currentDeviceState = DeviceStateStateMachineHelpers::OnLost( m_currentDeviceState );
	//	}
	//	break;

	//case GpuApi::DEVICECOOPLVL_NotReset:
	//	{
	//		m_currentDeviceState = DeviceStateStateMachineHelpers::OnNotReset( m_currentDeviceState );
	//	}
	//	break;

	//case GpuApi::DEVICECOOPLVL_InternalError:
	//	{
	//		RED_ASSERT( false, "internal error - wtf should we do?" );
	//		RED_LOG( "Renderer: Internal error"));
	//		m_currentDeviceState = (m_currentDeviceState == ERDS_Operational) ? ERDS_Lost_ResourcesNotReleased : m_currentDeviceState;
	//	}
	//	break;

	default:
		RED_ASSERT( false, "invalid device state" );
		break;
	}
}


Bool CRenderInterface::PrepareRenderer()
{
	// At the beginning of each frame, check for rendering device state - if it is lost, game should not tick!
	// If it is, perform needed tasks eg. releasing device or resetting it.
	// RefreshDeviceState();

	switch( m_currentDeviceState )
	{
	case ERDS_Lost_ResourcesNotReleased:
		{
			// Flush shit, then release device
			RED_LOG( "Renderer: Flushing all pending loading" );
			GetEngineHandler()->FlushAllLoading();
			RED_LOG( "Renderer: Loading flushed" );
			GRender->Flush();

			ReleaseRenderingResources();

			return false;
		}
	case ERDS_Lost_ResourcesReleased:
		{
#ifdef RED_PLATFORM_WINPC
			// Sleep to wait for events causing device to be in state for reset
			Sleep( 200 );
#endif
			return false;
		}
	case ERDS_NotReset:
		{
			// GRender->Flush();

			// Try to reset device
			if (!GpuApi::ResetDevice())
			{
				RED_LOG("Renderer: Unable to reset device");
			}

			// Delete all swapchains in all viewports, needed to preform reset
			for (Uint32 i = 0; i < m_viewports.Size(); i++)
			{
				m_viewports[i]->Reset();
				m_viewports[i]->CreateSwapChain();
			}

			return false;
		}
	case ERDS_Operational:
		{
			// That's what we are hoping for :)
			return true;
		}
	default:
		{
			RED_ASSERT( false, "Some problematic renderer device state in main tick" );
			return false;
		}
	}
}


Bool CRenderInterface::IsRaytracingAvaible()
{
#ifdef USE_RAY_TRACING

	// Also add some extra capabilities here, like graphic card checking, some hardware feature set and so on...
	return GpuApi::GetCapabilities().rayTracingSupport && Config::GGlobalRenderingSettings.allowRaytracing;

#endif

	return false;
}

void CRenderInterface::ReleaseRenderingResources()
{
#ifdef RED_COMPILER_MSC
	RED_LOG( "Renderer: Releasing device..." );

	if ( m_postProcess )
	{
		// Release additional postprocess resources (eg. nvidia's ssao stuff)
		m_postProcess->OnLostDevice();
	}

	// Release all dynamic rendering resources
	for ( CRenderResourceIterator i; i; ++i )
	{
		i->OnDeviceLost();
	}

	//
	if ( m_resourceEngineHandler )
	{
		m_resourceEngineHandler->ReleaseRenderingResources();
	}

	// Flush rendering thread so we have up to date data
	// GRender->Flush();

	RED_LOG( "Renderer: Device released" );

#endif

	m_currentDeviceState = ERDS_Lost_ResourcesReleased;
}

Bool CRenderInterface::ResetDevice()
{

#ifdef RED_COMPILER_MSC
	RED_LOG( "Renderer: Resetting device..." );

	// Delete all swapchains in all viewports, needed to preform reset
	for ( Uint32 i=0; i<m_viewports.Size(); i++ )
	{
		m_viewports[i]->Reset();
	}

	{
		//CGuiRenderCommand<THandleDeviceLost>::Send();
		GRender->Flush(); // Making sure it's handled before trying to reset the device
	}

	// Try to reset device
	if ( !GpuApi::ResetDevice() )
	{
		RED_LOG( "Renderer: Unable to reset device" );
		return false;
	}

	// Reset state manager
	m_stateManager->Reset();

	// Device was reset
	RED_LOG( "Renderer: Device reset !" );

	// Recreate all dynamic rendering resources
	for ( CRenderResourceIterator i; i; ++i )
	{
		i->OnDeviceReset();
	}

	//RED_MESSAGE( "Rendering TODO: UI package cleanup" )
	//CGuiRenderCommand<THandleDeviceReset>::Send();

	// Flush rendering thread so we have up to date data
	GRender->Flush();

	m_currentDeviceState = ERDS_Operational;

	// Flush rendering thread so we have up to date data
	GRender->Flush();

	//
	if ( m_resourceEngineHandler )
	{
		m_resourceEngineHandler->ResetDevice();
	}

	// Flush rendering thread so we have up to date data
	GRender->Flush();


	// Info
	RED_LOG( "Renderer: Dynamic resources recreated !" );
#endif

	m_currentDeviceState = ERDS_Operational;

	return true;
}

void CRenderInterface::ForceFakeDeviceReset()
{
	// Flush rendering thread so we have up to date data
	GRender->Flush();

	m_currentDeviceState = ERDS_Lost_ResourcesNotReleased;

	ReleaseRenderingResources();

	// Flush rendering thread so we have up to date data
	GRender->Flush();

	m_currentDeviceState = ERDS_NotReset;

	ResetDevice();

	// Flush rendering thread so we have up to date data
	GRender->Flush();
}

void CRenderInterface::ForceFakeDeviceLost()
{
	m_currentDeviceState = ERDS_Lost_ResourcesReleased;
}

void CRenderInterface::ForceFakeDeviceUnlost()
{
	m_currentDeviceState = ERDS_Operational;
}

void CRenderInterface::LoadSmallTextureData2D( const GpuApi::BufferRef& srcBuffer, const GpuApi::TextureRef& destTex, Uint32 mipLevel, const Rect& destRect, Uint32 srcPitch )
{
	PC_SCOPE( LoadSmallTextureData2D );

	RED_FATAL_ASSERT( srcPitch % 4 == 0, "Row pitch must be multiple of 4, due to raw buffer access restrictions" );

	const GpuApi::TextureDesc& toDesc = GpuApi::GetTextureDesc( destTex );

	RED_FATAL_ASSERT( destRect.m_right <= toDesc.width );
	RED_FATAL_ASSERT( destRect.m_bottom <= toDesc.height );

	GpuApi::BindBufferSRV( srcBuffer, 0, GpuWrapApi::ComputeShader );

	if ( mipLevel == 0 )
	{
		GpuApi::BindTextureUAVs( 0, 1, &destTex );
	}
	else
	{
		GpuApi::BindTextureMipLevelUAV( 0, destTex, mipLevel );
	}

	struct ShaderData
	{
		Int32 x;
		Int32 y;
		Uint32 width;
		Uint32 height;
		Uint32 rowPitch;
	};
	const ShaderData data =
	{
		destRect.m_left, destRect.m_top, (Uint32)destRect.Width(), (Uint32)destRect.Height(), srcPitch
	};
	GpuApi::SetComputeShaderConsts( data );

	// Must match ThreadCount in smallTextureUpdate.fx
	const Uint32 threadCount = 16;
	Uint32 pixelsPerThread = 1;

	switch ( toDesc.format )
	{
	case GpuWrapApi::TEXFMT_R8_Unorm:
		GetShader( SHADER_NAME( "m_smallTextureUpdate_R8_Unorm" ) )->Bind();
		pixelsPerThread = 4;
		break;
		// TODO more formats
	default:
		RED_FATAL( "Unsupported texture format" );
	}

	GpuApi::DispatchCompute( red::DivideRoundingUp( (Uint32)destRect.Width(), threadCount * pixelsPerThread ), red::DivideRoundingUp( (Uint32)destRect.Height(), threadCount ), 1 );
}

void CRenderInterface::StretchRect( const GpuApi::TextureRef& from, const Rect& fromArea, const GpuApi::TextureRef& to, const Rect& toArea, Uint32 srcMip /*= 0*/, Uint32 dstMip /*= 0*/, Uint32 srcSlice /*= 0*/, Uint32 dstSlice /*= 0*/ )
{
	PC_SCOPE( StretchRect );

	RED_ASSERT( from != to, "Cannot copy a texture to itself");

	// If no scaling is needed, try letting GpuApi do it directly first.
	if ( fromArea.Width() == toArea.Width() && fromArea.Height() == toArea.Height() )
	{
		if ( IsCopyRectValid( to, toArea.m_left, toArea.m_top, dstMip, dstSlice, from, fromArea, srcMip, srcSlice ) )
		{
			GpuApi::CopyRect( to, toArea.m_left, toArea.m_top, dstMip, dstSlice, from, fromArea, srcMip, srcSlice );
			return;
		}
	}

	const GpuApi::TextureDesc& fromDesc	= GpuApi::GetTextureDesc( from );
	const GpuApi::TextureDesc& toDesc	= GpuApi::GetTextureDesc( to );

	Bool isDestFormatUnorm = false;
	switch (toDesc.format)
	{
	case GpuWrapApi::TEXFMT_A8_Unorm:
	case GpuWrapApi::TEXFMT_R8_Unorm:
	case GpuWrapApi::TEXFMT_L8_Unorm:
	case GpuWrapApi::TEXFMT_R8G8B8X8_Unorm:
	case GpuWrapApi::TEXFMT_R8G8B8A8_Unorm:
	case GpuWrapApi::TEXFMT_R8G8_Unorm:
		isDestFormatUnorm = true;
	}

	// Can't use backbuffer as UAV
	// Can only use compute for non-MSAA targets. For simplicity, also non-MSAA source.
	// Current copy CS assumes a unorm output. Could write a variation for float, but this is simple too.
	if ( ( toDesc.usage & GpuApi::TEXUSAGE_UnorderedAccess )
		&& toDesc.sampleCount == GpuWrapApi::eTextureSampleCount::SampleCount1
		&& fromDesc.sampleCount == GpuWrapApi::eTextureSampleCount::SampleCount1
		&& isDestFormatUnorm )
	{
		//////////////////////////////////////////////////////////////////////////
		// Copy with compute shader
		struct ComputeData
		{
			Uint32	destRect[4];
			Float	destToSourceParams[4];
			Uint32	DestSlice;
			Uint32	SrcSlice;
			Uint32	SrcMipLvl;

		};

		// Can't have a texture bound as output and UAV at the same time, so we'll clear the current render target.
		GpuApi::SetupBlankRenderTargets();

		if( dstMip == 0 )
		{
			GpuApi::BindTextureUAVs( 0, 1, &to );
		}
		else
		{
			GpuApi::BindTextureMipLevelUAV( 0, to, dstMip );
		}

		const Float widthRatio = (Float)fromArea.Width() / (Float)toArea.Width();
		const Float heightRatio = (Float)fromArea.Height() / (Float)toArea.Height();

		const Uint32 srcMipWidth = fromDesc.width >> srcMip;
		const Uint32 srcMipHeight = fromDesc.height >> srcMip;

		const ComputeData data =
		{
			{ (Uint32)toArea.m_left, (Uint32)toArea.m_top, (Uint32)toArea.Width(), (Uint32)toArea.Height() },
			{
				// Scale
				widthRatio / (Float)( srcMipWidth ),
				heightRatio / (Float)( srcMipHeight ),
				// Offset. 0.5 to account for texel center
				( ( (Float)fromArea.m_left + 0.5f ) * widthRatio ) / (Float)srcMipWidth,
				( ( (Float)fromArea.m_top + 0.5f ) * heightRatio ) / (Float)srcMipHeight,
			},
			dstSlice,
			srcSlice,
			srcMip
		};
		GpuApi::SetComputeShaderConsts( data );

		GpuApi::BindTextures( 0, 1, &from, GpuWrapApi::ComputeShader );
		GpuApi::SetSamplerStatePreset( 0, GpuApi::SAMPSTATEPRESET_ClampLinearMip, GpuWrapApi::ComputeShader );

		{
			// Must match ThreadCount in simpleCopy.fx
			static Uint32 ThreadCount = 16;
			const TStaticMetaEntryHash shaderName = toDesc.type == GpuWrapApi::TEXTYPE_ARRAY ? SHADER_NAME( "m_simpleCopyCSArray" ) : SHADER_NAME( "m_simpleCopyCS" );
			GetShader( shaderName )->Dispatch( red::DivideRoundingUp( (Uint32)toArea.Width(), ThreadCount), red::DivideRoundingUp( (Uint32)toArea.Height(), ThreadCount ), 1);
		}

		GpuApi::BarrierTextureUAV( to );

		GpuApi::BindTextureUAVs( 0, 1, nullptr );
		GpuApi::BindTextures( 0, 1, nullptr, GpuWrapApi::ComputeShader );
	}
	else
	{
		//////////////////////////////////////////////////////////////////////////
		// Copy with fullscreen quad

		GpuApi::RenderTargetSetup rtSetup;
		rtSetup.SetColorTarget( 0, to );
		GpuApi::SetupRenderTargets( rtSetup );

		GpuApi::SetViewport( GpuApi::ViewportDesc( toArea.Width(), toArea.Height(), toArea.m_left, toArea.m_top ) );

		// Set textures
		GpuApi::BindTextures( 0, 1, &from, GpuWrapApi::PixelShader );
		GpuApi::SetSamplerStatePreset( 0, GpuApi::SAMPSTATEPRESET_ClampLinearNoMip, GpuWrapApi::PixelShader );

		// Render
		const Float fromWidth  = static_cast< Float >( fromDesc.width );
		const Float fromHeight = static_cast< Float >( fromDesc.height );
		const Vector4 rectVector(
			static_cast< Float >( fromArea.m_left ),
			static_cast< Float >( fromArea.m_top ),
			static_cast< Float >( fromArea.m_right ),
			static_cast< Float >( fromArea.m_bottom ) );

		GpuApi::SetVSC( VSC_Custom_0,	rectVector / Vector4( fromWidth, fromHeight, fromWidth, fromHeight ) );

		switch (toDesc.format)
		{
		case GpuWrapApi::TEXFMT_R32G32B32A32_Float:
			GetShader( SHADER_NAME( "m_simpleCopy32_RGBA" ) )->Bind();
			break;
		case GpuWrapApi::TEXFMT_R32G32_Float:
			GetShader( SHADER_NAME( "m_simpleCopy32_RG" ) )->Bind();
			break;
		case GpuWrapApi::TEXFMT_R32_Float:
			GetShader( SHADER_NAME( "m_simpleCopy32_R" ) )->Bind();
			break;
		default:
			GetShader( SHADER_NAME( "m_simpleCopy" ) )->Bind();
			break;
		}

		GpuApi::DrawPrimitiveNoBuffers( PRIMTYPE_TriangleStrip, 3 );

		GpuApi::BindTextures( 0, 1, nullptr, GpuWrapApi::PixelShader );
	}
}


void CRenderInterface::StretchRect( const GpuApi::TextureRef& from, const GpuApi::TextureRef& to )
{
	const GpuApi::TextureDesc fromDesc	= GpuApi::GetTextureDesc( from );
	const GpuApi::TextureDesc toDesc	= GpuApi::GetTextureDesc( to );

	Rect fromArea( 0, fromDesc.width, 0, fromDesc.height );
	Rect toArea( 0, toDesc.width, 0, toDesc.height );
	StretchRect( from, fromArea, to, toArea );
}

void CMipmapGenerator::GenerateMipmaps( const GpuApi::TextureRef& texture, const Rect* srcRegionToUpdate )
{
	const GpuApi::TextureDesc& desc	= GpuApi::GetTextureDesc( texture );

	if( !desc.HasMips() )
	{
		return;
	}

	if( ( desc.usage & GpuApi::TEXUSAGE_UnorderedAccess ) != 0 &&
		( desc.type == GpuWrapApi::TEXTYPE_2D || desc.type == GpuWrapApi::TEXTYPE_CUBE ) )
	{
		const Rect wholeRect = Rect( 0, ( Int32 )desc.width, 0, ( Int32 )desc.height );
		GenerateMipmaps_CS( texture, srcRegionToUpdate ? *srcRegionToUpdate : wholeRect );
	}
	else
	{
		// regionToUpdate not supported, always updates whole texture.
		GenerateMipmaps_PS( texture );
	}
}

void CMipmapGenerator::GenerateMipmapsWithGfx( const GpuApi::TextureRef& texture )
{
	const GpuApi::TextureDesc& desc	= GpuApi::GetTextureDesc( texture );

	if ( !desc.HasMips() )
	{
		return;
	}

	GenerateMipmaps_PS( texture );
}


void CMipmapGenerator::GenerateMipmaps( const GpuApi::TextureRef& sourceTexture, const GpuApi::TextureRef& destTexture, const Rect* srcRegionToUpdate )
{
	PC_GPU_SCOPE( GenerateMipmaps_CS_SrcDst );
	PC_SCOPE( GenerateMipmaps_CS_SrcDst );

	const GpuApi::TextureDesc& srcDesc = GpuApi::GetTextureDesc( sourceTexture );
	const GpuApi::TextureDesc& destDesc = GpuApi::GetTextureDesc( destTexture );

	RED_ASSERT( srcDesc.usage & GpuApi::TEXUSAGE_UnorderedAccess, "Source texture must have UAV usage" );
	RED_ASSERT( destDesc.usage & GpuApi::TEXUSAGE_UnorderedAccess, "Destination texture must have UAV usage" );
	RED_ASSERT( destDesc.format == srcDesc.format, "Source and destination texture formats must be the same" );

	const Rect wholeRect = Rect( 0, ( Int32 )srcDesc.width, 0, ( Int32 )srcDesc.height );
	if( srcRegionToUpdate == nullptr )
		srcRegionToUpdate = &wholeRect;

	const Uint32 numSlices = CalculateNumPhysicalSlices( destDesc ); // Already multiplied by 6 for cube texture.
	RED_ASSERT( numSlices == CalculateNumPhysicalSlices( srcDesc ) );

	const Uint32 totalMipLevels = 1+destDesc.numMips;
	const Uint32 maxMipsInBatch = 6;

	CRenderShaderCompute* const computeShader = GetComputeShader( srcDesc );
	RED_ASSERT( computeShader );
#ifdef __EMSCRIPTEN__
	// Browser port: a compute program without a WebGPU conversion yet resolves to null; skip the mip chain
	// rather than dereferencing it (the original asserts only in debug builds).
	if ( !computeShader ) return;
#endif

	// Optional transition of the whole texture to UAV state so the barrier is not issued for every subresource separately.
	GpuApi::TransitionTextureUAV( destTexture );

	GpuApi::BindBufferUAV( m_downsamplingGlobalAtomic, 0 );

	for ( Uint32 baseMip = 0; baseMip < totalMipLevels-1; baseMip += maxMipsInBatch )
	{
		const Uint32 baseTexWidth = Max<Uint32>( srcDesc.width >> baseMip, 1u );
		const Uint32 baseTexHeight = Max<Uint32>( srcDesc.height >> baseMip, 1u );

		const Uint8 numMipLevelsToGenerate = Min( maxMipsInBatch, totalMipLevels - baseMip - 1 );

		Uint32 rectInfo[] = {
			(Uint32)(srcRegionToUpdate->m_left >> baseMip),
			(Uint32)(srcRegionToUpdate->m_top >> baseMip),
			(Uint32)(((srcRegionToUpdate->m_right + ((1<<baseMip)-1)) >> baseMip) - (srcRegionToUpdate->m_left >> baseMip)),
			(Uint32)(((srcRegionToUpdate->m_bottom + ((1<<baseMip)-1)) >> baseMip) - (srcRegionToUpdate->m_top >> baseMip))
		};

		Uint32 dispatchThreadGroupCount[2] = {};
		Uint32 workGroupOffset[2] = {};
		Uint32 numWorkGroupsAndMips[2] = {};
		SpdSetup(
			dispatchThreadGroupCount, workGroupOffset, numWorkGroupsAndMips,
			rectInfo,
			numMipLevelsToGenerate );

		SDownsampleConsts cb = {};
		cb.mips = numWorkGroupsAndMips[1];
		cb.numWorkGroups = numWorkGroupsAndMips[0];
		cb.srcExtentMax.x = baseTexWidth - 1;
		cb.srcExtentMax.y = baseTexHeight - 1;
		cb.workGroupOffset.x = workGroupOffset[0];
		cb.workGroupOffset.y = workGroupOffset[1];
		cb.globalAtomicIndex = m_nextDownsamplingGlobalAtomicIndex.PostIncrement() % DOWNSAMPLING_GLOBAL_ATOMIC_COUNT;

		GpuApi::SetComputeShaderConsts( cb );

		if ( baseMip == 0 )
		{
			GpuApi::BindTextureMipLevelForMipGeneration( 0, sourceTexture, baseMip, GpuWrapApi::ComputeShader );
		}
		else
		{
			GpuApi::BindTextureMipLevelForMipGeneration( 0, destTexture, baseMip-1, GpuWrapApi::ComputeShader );
		}

		// destTexture is already half resolution, so we start writing out to mip 0 instead of 1.
		const Uint32 baseOutMip = baseMip;
		for ( Uint32 mip = 0; mip < numMipLevelsToGenerate; ++mip )
		{
			GpuApi::BindTextureMipLevelUAV( 1+mip, destTexture, baseOutMip+mip );
		}

		computeShader->Dispatch( dispatchThreadGroupCount[0], dispatchThreadGroupCount[1], numSlices );

		GpuApi::BarrierTextureUAV( destTexture );
	}
	GpuApi::BarrierBufferUAV( m_downsamplingGlobalAtomic );
}

void CMipmapGenerator::GenerateDepthMipmaps( const GpuApi::TextureRef& texture, Uint32 width, Uint32 height, bool forceMax )
{
	PC_SCOPE( HiZ_MipChain );
	PC_GPU_SCOPE( HiZ_MipChain );

	const GpuApi::TextureDesc& desc	= GpuApi::GetTextureDesc( texture );
	RED_ASSERT( desc.HasMips() );
	RED_ASSERT( width > 0 && height > 0 && width <= desc.width && height <= desc.height );
	const Uint32 numSlices = CalculateNumPhysicalSlices( desc ); // Already multiplied by 6 for cube texture.
	const Uint32 totalMipLevels = desc.numMips;
	const Uint32 maxMipsInBatch = 6;

	RED_ASSERT( numSlices == 1, "Shader assumes non-array. If you need an array here, add array-enabled versions to static shaders." );

	CRenderShaderBase* const depthShader = forceMax 
		? m_renderInterface->GetShader( SHADER_NAME( "m_downsampleDepthForceMax" ) ) 
		: m_renderInterface->GetShader( SHADER_NAME( "m_downsampleDepth" ) );
	CRenderShaderCompute* const computeShader = depthShader ? depthShader->AsCompute() : nullptr;
	RED_ASSERT( computeShader );
#ifdef __EMSCRIPTEN__
	// Browser port: a compute program without a WebGPU conversion yet resolves to null; skip the mip chain
	// rather than dereferencing it (the original asserts only in debug builds).
	if ( !computeShader ) return;
#endif

	// Optional transition of the whole texture to UAV state so the barrier is not issued for every subresource separately.
	GpuApi::TransitionTextureUAV( texture );

	GpuApi::BindBufferUAV( m_downsamplingGlobalAtomic, 0 );

	for ( Uint32 baseMip = 0; baseMip < totalMipLevels-1; baseMip += maxMipsInBatch )
	{
		const Uint32 mipWidth = Max( width >> baseMip, 1u );
		const Uint32 mipHeight = Max( height >> baseMip, 1u );

		const Uint32 baseTexWidth = Max<Uint32>( desc.width >> baseMip, 1u );
		const Uint32 baseTexHeight = Max<Uint32>( desc.height >> baseMip, 1u );

		const Uint32 numMipLevelsToGenerate = Min( maxMipsInBatch, totalMipLevels - baseMip - 1 );
		const Uint32 numWorkgroupsX = red::DivideRoundingUp( mipWidth, 64u );
		const Uint32 numWorkgroupsY = red::DivideRoundingUp( mipHeight, 64u );

		SDownsampleConsts cb = {};
		cb.mips = numMipLevelsToGenerate;
		cb.numWorkGroups = numWorkgroupsX * numWorkgroupsY;
		cb.srcExtentMax.x = (Int32)baseTexWidth  - 1;
		cb.srcExtentMax.y = (Int32)baseTexHeight - 1;
		cb.workGroupOffset.x = 0;
		cb.workGroupOffset.y = 0;
		cb.globalAtomicIndex = m_nextDownsamplingGlobalAtomicIndex.PostIncrement() % DOWNSAMPLING_GLOBAL_ATOMIC_COUNT;

		GpuApi::SetComputeShaderConsts( cb );

		GpuApi::BindTextureMipLevelForMipGeneration( 0, texture, baseMip, GpuWrapApi::ComputeShader );

		const Uint32 baseOutMip = baseMip+1;
		for ( Uint32 mip = 0; mip < numMipLevelsToGenerate; ++mip )
		{
			GpuApi::BindTextureMipLevelUAV( 1+mip, texture, baseOutMip+mip );
		}

		computeShader->Dispatch( numWorkgroupsX, numWorkgroupsY, numSlices );

		GpuApi::BarrierTextureUAV( texture );
	}

	GpuApi::BarrierBufferUAV( m_downsamplingGlobalAtomic );
}

void CMipmapGenerator::GenerateMipmaps_CS( const GpuApi::TextureRef& texture, const Rect& srcRegionToUpdate )
{
	PC_SCOPE( GenerateMipmaps_CS );
	PC_GPU_SCOPE( GenerateMipmaps_CS );

	const GpuApi::TextureDesc& desc = GpuApi::GetTextureDesc( texture );
	const Uint32 numSlices = CalculateNumPhysicalSlices( desc ); // Already multiplied by 6 for cube texture.
	const Uint32 totalMipLevels = desc.numMips;
	const Uint32 maxMipsInBatch = 6;

	CRenderShaderCompute* const computeShader = GetComputeShader( desc );
	RED_ASSERT( computeShader );
#ifdef __EMSCRIPTEN__
	// Browser port: a compute program without a WebGPU conversion yet resolves to null; skip the mip chain
	// rather than dereferencing it (the original asserts only in debug builds).
	if ( !computeShader ) return;
#endif

	// Optional transition of the whole texture to UAV state so the barrier is not issued for every subresource separately.
	GpuApi::TransitionTextureUAV( texture );

	GpuApi::BindBufferUAV( m_downsamplingGlobalAtomic, 0 );

	for ( Uint32 baseMip = 0; baseMip < totalMipLevels-1; baseMip += maxMipsInBatch )
	{
		const Uint32 baseTexWidth = Max<Uint32>( desc.width >> baseMip, 1u );
		const Uint32 baseTexHeight = Max<Uint32>( desc.height >> baseMip, 1u );

		const Uint32 numMipLevelsToGenerate = Min( maxMipsInBatch, totalMipLevels - baseMip - 1 );

		Uint32 rectInfo[] = {
			(Uint32)(srcRegionToUpdate.m_left >> baseMip),
			(Uint32)(srcRegionToUpdate.m_top >> baseMip),
			(Uint32)(((srcRegionToUpdate.m_right + ((1<<baseMip)-1)) >> baseMip) - (srcRegionToUpdate.m_left >> baseMip)),
			(Uint32)(((srcRegionToUpdate.m_bottom + ((1<<baseMip)-1)) >> baseMip) - (srcRegionToUpdate.m_top >> baseMip))
		};

		Uint32 dispatchThreadGroupCount[2] = {};
		Uint32 workGroupOffset[2] = {};
		Uint32 numWorkGroupsAndMips[2] = {};
		SpdSetup(
			dispatchThreadGroupCount, workGroupOffset, numWorkGroupsAndMips,
			rectInfo,
			numMipLevelsToGenerate );

		SDownsampleConsts cb = {};
		cb.mips = numWorkGroupsAndMips[1];
		cb.numWorkGroups = numWorkGroupsAndMips[0];
		cb.srcExtentMax.x = (Int32)baseTexWidth  - 1;
		cb.srcExtentMax.y = (Int32)baseTexHeight - 1;
		cb.workGroupOffset.x = workGroupOffset[0];
		cb.workGroupOffset.y = workGroupOffset[1];
		cb.globalAtomicIndex = m_nextDownsamplingGlobalAtomicIndex.PostIncrement() % DOWNSAMPLING_GLOBAL_ATOMIC_COUNT;

		GpuApi::SetComputeShaderConsts( cb );

		GpuApi::BindTextureMipLevelForMipGeneration( 0, texture, baseMip, GpuWrapApi::ComputeShader );

		const Uint32 baseOutMip = baseMip+1;
		for ( Uint32 mip = 0; mip < numMipLevelsToGenerate; ++mip )
		{
			GpuApi::BindTextureMipLevelUAV( 1+mip, texture, baseOutMip+mip );
		}

		computeShader->Dispatch( dispatchThreadGroupCount[0], dispatchThreadGroupCount[1], numSlices );

		GpuApi::BarrierTextureUAV( texture );
	}

	GpuApi::BarrierBufferUAV( m_downsamplingGlobalAtomic );
}

void CMipmapGenerator::GenerateMipmaps_PS(const GpuApi::TextureRef& texture)
{
	// fallback to manual mip map generation method
	PC_SCOPE( GenerateMipmaps );
	PC_GPU_SCOPE( GenerateMipmaps );

	const GpuApi::TextureDesc& desc	= GpuApi::GetTextureDesc( texture );

	GpuApi::SetSamplerStatePreset( 0, GpuApi::SAMPSTATEPRESET_ClampLinearNoMip, GpuWrapApi::PixelShader );
	GpuApi::SetVSC( VSC_Custom_0, Vector4(0.0f, 0.0f, 1.0f, 1.0f));

	switch (desc.format)
	{
	case GpuWrapApi::TEXFMT_R32G32B32A32_Float:
		m_renderInterface->GetShader( SHADER_NAME( "m_simpleCopy32_RGBA" ) )->Bind();
		break;
	case GpuWrapApi::TEXFMT_R32G32_Float:
		m_renderInterface->GetShader( SHADER_NAME( "m_simpleCopy32_RG" ) )->Bind();
		break;
	case GpuWrapApi::TEXFMT_R32_Float:
		m_renderInterface->GetShader( SHADER_NAME( "m_simpleCopy32_R" ) )->Bind();
		break;
	default:
		m_renderInterface->GetShader( SHADER_NAME( "m_simpleCopy" ) )->Bind();
		break;
	}

	const Int16 numSlices = GpuApi::CalculateNumPhysicalSlices( desc );

	for (Int16 mip_i = 1; mip_i < desc.numMips; ++mip_i)
	{
		for( Int16 slice_i = 0; slice_i < numSlices; ++slice_i )
		{
			GpuApi::RenderTargetSetup rtSetup;
			rtSetup.SetColorTarget( 0, texture, GpuApi::CalculateTextureSliceMipIndex( desc, slice_i, mip_i ) );
			GpuApi::SetupRenderTargets( rtSetup );

			GpuApi::SetViewport( GpuApi::ViewportDesc( desc.width >> mip_i, desc.height >> mip_i ) );

			GpuApi::BindTextureMipLevelForMipGeneration( 0, texture, slice_i, mip_i - 1, GpuWrapApi::PixelShader );

			GpuApi::DrawPrimitiveNoBuffers( PRIMTYPE_TriangleStrip, 3 );
		}
	}
}


Float CRenderInterface::GetLastGPUFrameDuration( Float* outMainScene, Float* outSubscene, Float* outDrawBuffers ) const
{
	if ( outMainScene )
	{
		*outMainScene = m_lastMainSceneTime;
	}

	if ( outSubscene )
	{
		*outSubscene = m_lastSubsceneTime;
	}

	if ( outDrawBuffers )
	{
		*outDrawBuffers = m_lastDrawBuffersTime;
	}

	return m_lastMainSceneTime + m_lastSubsceneTime + m_lastDrawBuffersTime;
}

Uint32 CRenderInterface::GetRayTracingInstanceNum() const
{
#ifdef USE_RAY_TRACING
	return m_accelerationStructureManager ? m_accelerationStructureManager->GetLastFrameInstanceNum() : 0;
#else
	return 0;
#endif
}

Int32 CRenderInterface::GetMonitorCount() const
{
	return GpuApi::GetMonitorCount();
}


Uint32 CRenderInterface::GetSupportedHDRModesMask( Int32 monitorIndex, Bool fullscreen ) const
{
	return GpuApi::GetSupportedHDRModesMask( monitorIndex, fullscreen );
}


Uint32 CRenderInterface::GetCurrentOutputMonitorConfig() const
{
	Int32 result = 0;
	Int32 monitorCount = GetMonitorCount();
	if( monitorCount > 0 )
	{
		result= Clamp( Config::cvOutputMonitor.Get(), 0, monitorCount - 1 );
	}
	return (Uint32)result;
}

void CRenderInterface::GetNativeResolution( Uint32& width, Uint32& height ) const
{
	GpuApi::GetNativeResolution( GetCurrentOutputMonitorConfig(), width, height );
}

static Bool IsResolutionSupported( const Uint32 width, const Uint32 height )
{
	const Int32 minimumResolutionWidth = 1024;
	const Int32 minimumResolutionHeight = 720;
	return (width >= minimumResolutionWidth && height >= minimumResolutionHeight);
}

static Bool GetDisplayModes( Int32 monitorIndex, Uint32& outModesNum, GpuApi::DisplayModeDesc** outModes )
{
	Uint32 modesNum = 0;
	if( GpuApi::EnumerateDisplayModes( monitorIndex, &modesNum ) == true )
	{
		if( modesNum != 0 )
		{
			(*outModes) = RED_NEW_ARRAY( GpuApi::DisplayModeDesc, modesNum );
			outModesNum = modesNum;

			auto sortPredicate = []( const GpuApi::DisplayModeDesc& left, const GpuApi::DisplayModeDesc& right )
			{
				if( left.width == right.width )
				{
					return left.height < right.height;
				}

				return left.width < right.width;
			};

			if( GpuApi::EnumerateDisplayModes( monitorIndex, &modesNum, outModes ) == true )
			{
				std::sort( &(*outModes)[0], &(*outModes)[modesNum], sortPredicate );
				return true;
			}
			else
			{
				RED_DELETE_ARRAY( *outModes, modesNum );
				return false;
			}
		}
	}

	return false;
}

void CRenderInterface::ListAvailableDisplayModes( DynArray<String>& outScreenModes, Int32 requestedMonitorIdx /*=-1*/ ) const
{
	Uint32 modesNum = 0;
	GpuApi::DisplayModeDesc* modes = nullptr;

	// If no monitor index was explicitly requested, use current setup from render interface, otherwise ask for required display.
	if( GetDisplayModes( (requestedMonitorIdx != -1) ? requestedMonitorIdx : GetCurrentOutputMonitorConfig(), modesNum, &modes ) == false )
	{
		return;
	}

	outScreenModes.Reserve( modesNum );

	for( Uint32 i=0; i<modesNum; ++i )
	{
		Int32 width = modes[i].width;
		Int32 height = modes[i].height;
		if( IsResolutionSupported( width, height ) == true )
		{
			outScreenModes.PushBack( Config::Helper::PackDisplayModeValues( width, height ) );
		}
	}

	outScreenModes.Remove( std::unique( outScreenModes.Begin(), outScreenModes.End() ), outScreenModes.End() );

	RED_DELETE_ARRAY( modes, modesNum );
}


void CRenderInterface::ListAvailableVSyncFPSModes( red::DynArray<String>& outVSyncFPSModes, Int32 requestedMonitorIdx /*= -1 */ ) const
{
	outVSyncFPSModes.Clear();

	const Int32 refreshRate = GpuApi::GetCurrentDisplayRefreshRate( requestedMonitorIdx );
	if ( refreshRate > 0 )
	{
		outVSyncFPSModes.PushBack( "Off" );
		outVSyncFPSModes.PushBack( String::Printf( job::PoolJobScope(), "%i", refreshRate ) );
		outVSyncFPSModes.PushBack( String::Printf( job::PoolJobScope(), "%i", refreshRate / 2 ) );
		outVSyncFPSModes.PushBack( String::Printf( job::PoolJobScope(), "%i", refreshRate / 3 ) );
		outVSyncFPSModes.PushBack( String::Printf( job::PoolJobScope(), "%i", refreshRate / 4 ) );
		return;
	}

	//
	// Default to off/60/30/20/15 just not to crash.
	// 
	outVSyncFPSModes = { "Off", "60", "30", "20", "15" };
}



Bool CRenderInterface::GetBestMatchingResolution( const Uint32 width, const Uint32 height, Uint32& outWidth, Uint32& outHeight, Uint32 monitorIdx /* = 0 */ ) const
{
	Uint32 modesNum = 0;
	GpuApi::DisplayModeDesc* modes = nullptr;

	if( GetDisplayModes( monitorIdx, modesNum, &modes ) == false )
	{
		return false;
	}

	if( modesNum == 0 )
	{
		return false;
	}

	// Find first supported resolution
	Bool supportedResolutionFound = false;		// If we won't find any supported resolution, then this means GetBestMatchingResolution failed
	for( Uint32 i=0; i<modesNum; ++i )
	{
		if( IsResolutionSupported( modes[i].width, modes[i].height ) == true )
		{
			outWidth = modes[i].width;
			outHeight = modes[i].height;
			supportedResolutionFound = true;
			break;
		}
	}

	// Find best match
	if( supportedResolutionFound == true )
	{
		Int32 matchDelta = Abs( (Int32)modes[0].width - (Int32)width ) + Abs( (Int32)modes[0].height - (Int32)height );

		for( Uint32 i=0; i<modesNum; ++i )
		{
			if( IsResolutionSupported( modes[i].width, modes[i].height ) == true )
			{
				Int32 currentMatchDelta = Abs( (Int32)modes[i].width - (Int32)width ) + Abs( (Int32)modes[i].height - (Int32)height );
				if( currentMatchDelta < matchDelta )
				{
					outWidth = modes[i].width;
					outHeight = modes[i].height;
					matchDelta = currentMatchDelta;
				}
			}
		}
	}

	RED_DELETE_ARRAY( modes, modesNum );

	return supportedResolutionFound;
}

void CRenderInterface::GetResolutionScale( float& x, float& y ) const
{
	////// TODO: Plug this to in-game options

	// default: no scaling
	x = y = 1.0f;

#ifdef USE_DLSS
	if ( m_ngx && m_ngx->IsDLSSEnabled() )
	{
		// DLSS enabled, set scaling to a value decided internally by DLSS quality level
		x = y = m_ngx->GetScaleFactor();
	}
	else
#endif
	{
		if ( Config::GGlobalRenderingSettings.ForceInternalResolutionScaling )
		{
			// Resolution scaling is enforced by quality level. Choose scale based on platform.
			// On consoles (in Console quality actually - we can use it on PC for development convenience), we want specific CAS setups.
			// NOTE: The setups are not final.
			switch ( GpuApi::GetDeviceType() )
			{
				// TODO: rethink those scale factors.
				case GpuApi::eDeviceType::ePC:					// Same as base PS4 (why PC? This is for "Console mode")
				case GpuApi::eDeviceType::ePLAY_STATION:		x = y = 900.5f / 1080.0f;	break;	// 900p upscaled to 1080p

				case GpuApi::eDeviceType::eXBOX_SERIES_S:		// Same as PS4 Pro (similar power in flops)
				case GpuApi::eDeviceType::ePLAY_STATION_PRO:
				case GpuApi::eDeviceType::ePLAY_STATION_5:		x = y = 1200.5f / 1440.0f;	break; // 1200p upscaled to 1440p, sadly - no boost on PS5 because of it's shit memory in b/c mode
															

				case GpuApi::eDeviceType::eXBOX_ONE:			// same as S (pretty much the same console, save for a tiny bit of more power to accommodate for HDR->SDR conversion)
				case GpuApi::eDeviceType::eXBOX_ONE_S:			x = y = 720.5f / 1080.0f;	break; // 720p upscaled to 1080p

				case GpuApi::eDeviceType::eXBOX_ONE_X:			// same as SeriesX (on SeriesX we aim for 60 FPS). TODO: revisit after DRS is ready
				case GpuApi::eDeviceType::eXBOX_SERIES_X:		x = y = 1600.5f / 2160.0f;	break; // 1600p upscaled to 2160p (our strongest platforms)
				default: RED_FATAL( "Unknown platform" );
			}
		}

#ifndef RED_CONFIGURATION_FINAL
		if ( auto devConfig = GetRenderer()->GetDevConfig() )
		{
			if ( devConfig->ResolutionUpscaling.IsEnabled() )
			{
				x = y = devConfig->ResolutionUpscaling.m_casScalingFactor;
			}
		}
#endif
	}
}

void CRenderInterface::GetDynamicResolutionSetup( SDynamicResolutionSetup& outSetup ) const
{
#ifdef __EMSCRIPTEN__
	{
		const int percent = g_cpWebResPercent.load( std::memory_order_relaxed );
		if ( percent > 0 && percent < 100 && !m_viewports.Empty() )
		{
			const Uint32 height = m_viewports[ 0 ]->GetHeight();
			// multiples of 8 keep the half/quarter-res chains even
			const Uint32 scaled = std::max< Uint32 >( 8, ( Uint32 )( height * percent / 100 ) & ~7u );
			outSetup.targetHeight = height;
			outSetup.heightMin = scaled;
			outSetup.heightMax = scaled;
			outSetup.targetFPS = 60; // ignored by the static path
			outSetup.enabled = true;
			return;
		}
	}
#endif
#ifdef RED_PLATFORM_WINPC
	if ( !Config::GGlobalRenderingSettings.ForceInternalResolutionScaling )
	{
		if ( !m_viewports.Empty() && rend::settings::Valid() )
		{
			const auto* gameViewport = m_viewports[ 0 ];
			const Uint32 height = gameViewport->GetHeight();
			outSetup.targetHeight = height;
			if( gameViewport->GetSRS() )
			{
				const Float resolutionPercentage = ( Float ) gameViewport->GetSRSResolution() / 100.f;
				outSetup.heightMin = ( Uint32 ) ( height * resolutionPercentage );
				outSetup.heightMax = outSetup.heightMin;
				outSetup.targetFPS = 60; // Ignored.
				outSetup.enabled = true;
			}
			else
			{
				const Float minResolutionPercentage = ( ( Float ) gameViewport->GetDRSMinimalResolution() / 100.0f );
				const Float maxResolutionPercentage = ( ( Float ) gameViewport->GetDRSMaximalResolution() / 100.0f );
				const Uint32 targetFPS = gameViewport->GetDRSTargetFPS();
				const Bool drsEnabled = gameViewport->GetDRS();

				outSetup.heightMin = ( Uint32 ) ( height * minResolutionPercentage );
				outSetup.heightMax = ( Uint32 ) ( height * maxResolutionPercentage );
				outSetup.targetFPS = targetFPS;
				outSetup.enabled = drsEnabled;
			}
			return;
		}
	}
#endif
	// #NOTE: DO NOT change Xbox settings without also changing SDeviceData::SDeviceData in gpuApiDX12.h
	// targetHeight and targetFPS affect m_XboxPresentationInterval and m_XboxPresentScaleFactorVert/m_XboxPresentScaleFactorHorz.
	// #TODO: this needs automating
	if ( Config::GGlobalRenderingSettings.ForceInternalResolutionScaling )
	{
		// Resolution scaling is enforced by quality level. Choose scale based on platform.
		// On consoles (in Console quality actually - we can use it on PC for development convenience), we want specific CAS setups.
		switch ( GpuApi::GetDeviceType() )
		{
		case GpuApi::eDeviceType::ePC:					// Same as base PS4 (why PC? This is for "Console mode")
		case GpuApi::eDeviceType::ePLAY_STATION:
			outSetup.heightMin = 720;
			outSetup.heightMax = 900;
			outSetup.targetHeight = 1080;
			outSetup.targetFPS = 30;
			outSetup.enabled = true;
			break;

		case GpuApi::eDeviceType::ePLAY_STATION_PRO:
			outSetup.heightMin = 900;
			outSetup.heightMax = 1200;
			outSetup.targetHeight = 1440;
			outSetup.targetFPS = 30;
			outSetup.enabled = true;
			break;

		case GpuApi::eDeviceType::ePLAY_STATION_5:
			outSetup.heightMin = 900;
			outSetup.heightMax = 1200;
			outSetup.targetHeight = 1440;
			outSetup.targetFPS = 60;
			outSetup.enabled = true;
			break;

		case GpuApi::eDeviceType::eXBOX_ONE:			// same as S (pretty much the same console, save for a tiny bit of more power to accommodate for HDR->SDR conversion)
		case GpuApi::eDeviceType::eXBOX_ONE_S:
			outSetup.heightMin = 688;
			outSetup.heightMax = 864;				// not 900, because we want to fit in ESRAM
			outSetup.targetHeight = 900;
			outSetup.targetFPS = 30;
			outSetup.enabled = true;
			break;

		case GpuApi::eDeviceType::eXBOX_SERIES_S:
			outSetup.heightMin = 900;
			outSetup.heightMax = 1440;
			outSetup.targetHeight = 1440;
			outSetup.targetFPS = 30;
			outSetup.enabled = true;
			break;

		case GpuApi::eDeviceType::eXBOX_ONE_X:
			outSetup.heightMin		= 1080;
			outSetup.heightMax		= 2160;
			outSetup.targetHeight	= 2160;
			outSetup.targetFPS		= 30;
			outSetup.enabled		= true;
			break;

		case GpuApi::eDeviceType::eXBOX_SERIES_X:
			outSetup.heightMin		= 1080;
			outSetup.heightMax		= 1800;
			outSetup.targetHeight	= 1800;
			outSetup.targetFPS		= 60;
			outSetup.enabled		= true;

			// Ehh, ok, just patch the target FPS according to current vsyncInterval. This is so we can support Quality and Performance modes
			if ( !m_viewports.Empty() && rend::settings::Valid() )
			{
				const auto* gameViewport = m_viewports[0];
				outSetup.targetFPS = gameViewport->GetVSyncInterval() == 1 ? 60 : 30;
			}
			break;

		default: RED_FATAL( "Unknown platform" );
		}
	}
}

#ifdef USE_PROFILER

void CRenderInterface::GetResolutionHistory( red::DynArray< ResolutionHistoryEntry >& outHistory ) const
{
	RED_SCOPE_SHARED_LOCK( m_resolutionHistoryLock );

	outHistory.Clear();
	outHistory.Reserve( MaxResolutionHistoryDepth );

	for ( Uint32 i = 0; i < MaxResolutionHistoryDepth; ++i )
	{
		const ResolutionHistoryEntry& entry = m_resolutionHistoryData[ ( m_resolutionHistoryWriteOffset + i ) % MaxResolutionHistoryDepth ];
		outHistory.PushBack( entry );
	}
}

void CRenderInterface::PushResolutionHistoryEntry( const ResolutionHistoryEntry& entry )
{
	RED_SCOPE_LOCK( m_resolutionHistoryLock );

	m_resolutionHistoryData[ m_resolutionHistoryWriteOffset ] = entry;
	m_resolutionHistoryWriteOffset = ( m_resolutionHistoryWriteOffset + 1 ) % MaxResolutionHistoryDepth;
}

#endif // USE_PROFILER


IRenderCommandHandler* CRenderInterface::GetCommands()
{
	return m_renderCommandHandler.Get();
}

Uint16 CRenderInterface::UploadIESData( const Uint8* data )
{
	RED_FATAL_ASSERT( m_iesProfile );

	return m_iesProfile->UploadIESData( data );
}

void CRenderInterface::FlushTextureGrabs()
{
	const Uint32 numGrabsFlushed = GpuApi::FlushTextureGrabs();
	RED_LOG( "Num texture grabs flushed: %u", numGrabsFlushed );
}

void CRenderInterface::FlushBufferGrabs()
{
	const Uint32 numGrabsFlushed = GpuApi::FlushBufferGrabs();
	RED_LOG_SPAM( "Num buffer grabs flushed: %u", numGrabsFlushed );
}

TRenderPtr<IRenderVideo> CRenderInterface::CreateVideo( const VideoParams& videoParams ) const
{
	return CreateRenderVideo( videoParams );
}

void CRenderInterface::ProcessRenderVideoDestroyQueue() const
{
	::ProcessRenderVideoDestroyQueue();
}


void CRenderInterface::SetVideoToRender( Uint64 canvasName, const TRenderPtr<IRenderVideo>& video )
{
	m_videoPlayer->PlayVideo( canvasName, video );
}

void CRenderInterface::GetDebugStats( rend::DebugStats& debugStats ) const
{
#ifndef RED_CONFIGURATION_FINAL
	GpuApi::GetDebugStats( debugStats );
	if ( m_geometryBatcher != nullptr )
	{
		m_geometryBatcher->GetInstanceBufferStats( debugStats.m_instanceBufferStats );
	}
#endif
}

#define READ_CONFIG_VALUE( typeName, groupName, varName, configVarName )																												\
	{																																													\
		String temp;																																									\
		if ( configStorage.GetEntry( groupName, varName, temp ) )																														\
		{																																												\
			Config::configVarName.Set( FromStringDirect< typeName >( temp.AsChar() ) );																									\
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Rendering, "CRenderInterface::LoadConfig(): Override value '" varName "' in group '" groupName "' to value '%hs'.", temp.AsChar() );	\
		}																																												\
		else																																											\
		{																																												\
			RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Rendering, "CRenderInterface::LoadConfig(): Could not load config override value '" varName "' in group '" groupName "'." );	\
		}																																												\
	}

void CRenderInterface::LoadConfig()
{
	red::AbsolutePath path = GFileManager->GetGameRoot().AddDirPath( "depot" ).AddFilePath( "base\\materials\\config\\config_override.ini" );

	Config::CConfigVarStorage configStorage;
	if ( !configStorage.Load( path ) )
	{
		RED_LOG_CATEGORY_ERROR( red::LoggerCategory_Rendering, "CRenderInterface::LoadConfig(), Could not load config override from '%hs'", path.ToDebugString() );
		return;
	}

	// Load Hair override
	{
		READ_CONFIG_VALUE( Float, "Hair", "cvHairRoughnessFactor", cvHairRoughnessFactor );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairAlbedoMultiplier", cvHairAlbedoMultiplier );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairAdditionalAreaRoughness", cvHair_AdditionalAreaRoughness );
		READ_CONFIG_VALUE( Bool, "Hair", "cvHair_useReferenceImpl", cvHair_useReferenceImpl );
		READ_CONFIG_VALUE( Bool, "Hair", "cvHair_useGlobalContactShadows", cvHair_useGlobalContactShadows );
		READ_CONFIG_VALUE( Bool, "Hair", "cvHair_useLocalContactShadows", cvHair_useLocalContactShadows );
		READ_CONFIG_VALUE( Float, "Hair", "cvHair_contactShadowsClamp", cvHair_contactShadowsClamp );
		READ_CONFIG_VALUE( Bool, "Hair", "cvHair_debugSwitch1", cvHair_debugSwitch1 );
		READ_CONFIG_VALUE( Bool, "Hair", "cvHair_debugSwitch2", cvHair_debugSwitch2 );
		READ_CONFIG_VALUE( Float, "Hair", "cvHair_SpecRandomMin", cvHair_SpecRandomMin );
		READ_CONFIG_VALUE( Float, "Hair", "cvHair_SpecRandomMax", cvHair_SpecRandomMax );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairR_Local", cvHairR_Local );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTT_Local", cvHairTT_Local );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTRT_Local", cvHairTRT_Local );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_Local", cvHairScatter_Local );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatterDepth_Local", cvHairScatterDepth_Local );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairR_Global", cvHairR_Global );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTT_Global", cvHairTT_Global );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTRT_Global", cvHairTRT_Global );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_Global", cvHairScatter_Global );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatterDepth_Global", cvHairScatterDepth_Global );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairR_EnvProbe", cvHairR_EnvProbe );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTT_EnvProbe", cvHairTT_EnvProbe );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairTRT_EnvProbe", cvHairTRT_EnvProbe );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_EnvProbe", cvHairScatter_EnvProbe );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatterDepth_EnvProbe", cvHairScatterDepth_EnvProbe );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_Wrap", cvHairScatter_Wrap );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_DiffuseScatterFactor", cvHairScatter_DiffuseScatterFactor );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairScatter_ShadowFactorExp", cvHairScatter_ShadowFactorExp );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairSpec_Wrap", cvHairSpec_Wrap);
		READ_CONFIG_VALUE( Float, "Hair", "cvHairSpec_MaskIntensity", cvHairSpec_MaskIntensity);
		READ_CONFIG_VALUE( Float, "Hair", "cvHairAlphaShift_R", cvHairAlphaShift_R );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairAlphaShift_TT", cvHairAlphaShift_TT );
		READ_CONFIG_VALUE( Float, "Hair", "cvHairAlphaShift_TRT", cvHairAlphaShift_TRT );
		READ_CONFIG_VALUE( Float, "Hair", "cvHair_TRT_EXPSCALE", cvHair_TRT_EXPSCALE );
		READ_CONFIG_VALUE( Float, "Hair", "cvHair_TRT_EXPBIAS", cvHair_TRT_EXPBIAS );

		// HACKS
		READ_CONFIG_VALUE( Float, "Hair", "AAAA_HACK_hairModifiedLocalLightIntensity",	cvHACK_hairModifiedLocalLightIntensity );
		READ_CONFIG_VALUE( Float, "Hair", "cvHACK_hairFactor0",							cvHACK_hairFactor0 );
		READ_CONFIG_VALUE( Float, "Hair", "cvHACK_hairFactor1",							cvHACK_hairFactor1 );
		READ_CONFIG_VALUE( Float, "Hair", "cvHACK_hairFactor2",							cvHACK_hairFactor2 );
		READ_CONFIG_VALUE( Float, "Hair", "cvHACK_hairFactor3",							cvHACK_hairFactor3 );
	}

	// Load CharacterRimEnhancement override
	{
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvSkinSpecular",			cvCharacterRim_Skin_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvSkinFresnel",			cvCharacterRim_Skin_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvSkinConstOffset",		cvCharacterRim_Skin_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvFoliageSpecular",		cvCharacterRim_Foliage_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvFoliageFresnel",		cvCharacterRim_Foliage_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvFoliageConstOffset",	cvCharacterRim_Foliage_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvWeaponSpecular",		cvCharacterRim_Weapon_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvWeaponFresnel",			cvCharacterRim_Weapon_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvWeaponConstOffset",		cvCharacterRim_Weapon_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvStandardSpecular",		cvCharacterRim_Standard_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvStandardFresnel",		cvCharacterRim_Standard_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvStandardConstOffset",	cvCharacterRim_Standard_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvCharacterFresnel",		cvCharacterRim_Character_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvRoughnessFactor_Scale",	cvCharacterRim_RoughnessFactor_Scale );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvRoughnessFactor_Bias",	cvCharacterRim_RoughnessFactor_Bias );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement", "cvLightBlockerInfluence",	cvCharacterRim_LightBlockerInfluence );

		// RayTracing
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvSkinSpecular",			cvCharacterRim_RayTracing_Skin_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvSkinFresnel",			cvCharacterRim_RayTracing_Skin_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvSkinConstOffset",		cvCharacterRim_RayTracing_Skin_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvFoliageSpecular",		cvCharacterRim_RayTracing_Foliage_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvFoliageFresnel",			cvCharacterRim_RayTracing_Foliage_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvFoliageConstOffset",		cvCharacterRim_RayTracing_Foliage_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvWeaponSpecular",			cvCharacterRim_RayTracing_Weapon_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvWeaponFresnel",			cvCharacterRim_RayTracing_Weapon_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvWeaponConstOffset",		cvCharacterRim_RayTracing_Weapon_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvStandardSpecular",		cvCharacterRim_RayTracing_Standard_Specular );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvStandardFresnel",		cvCharacterRim_RayTracing_Standard_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvStandardConstOffset",	cvCharacterRim_RayTracing_Standard_ConstOffset );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvCharacterFresnel",		cvCharacterRim_RayTracing_Character_Fresnel );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvRoughnessFactor_Scale",	cvCharacterRim_RayTracing_RoughnessFactor_Scale );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvRoughnessFactor_Bias",	cvCharacterRim_RayTracing_RoughnessFactor_Bias );
		READ_CONFIG_VALUE( Float, "CharacterRimEnhancement_RayTracing", "cvLightBlockerInfluence",	cvCharacterRim_RayTracing_LightBlockerInfluence );
	}

	// Load Skin override
	{
		READ_CONFIG_VALUE( Float, "Skin", "cvSkin_SpecularTint_R", cvSkin_SpecularTint_R );
		READ_CONFIG_VALUE( Float, "Skin", "cvSkin_SpecularTint_G", cvSkin_SpecularTint_G );
		READ_CONFIG_VALUE( Float, "Skin", "cvSkin_SpecularTint_B", cvSkin_SpecularTint_B );
		READ_CONFIG_VALUE( Float, "Skin", "cvSkin_SpecularTint_Weight", cvSkin_SpecularTint_Weight );
	}

	// Load Volumetric Fog override
	{
		READ_CONFIG_VALUE( Float, "VolumetricFog", "cvVolFog_Exponent", cvVolFog_Exponent );
	}

	// Load Dismemberment Decals override
	//{
	//	READ_CONFIG_VALUE( Float, "DismembermentDecals", "cvDismembermentDecalsFadeOrigin", cvDismembermentDecalFadeOrigin );
	//	READ_CONFIG_VALUE( Float, "DismembermentDecals", "cvDismembermentDecalsFadePower", cvDismembermentDecalFadePower );
	//}
}

Float CRenderInterface::GetMonitorMaxLuminanceNits( Int32 monitorIndex ) const
{
	return GpuApi::GetMonitorMaxLuminanceNits( monitorIndex );
}

Bool CRenderInterface::GetHardwareRayTracingSupport() const
{
	return GpuApi::GetCapabilities().rayTracingSupport;
}

#ifdef RED_PLATFORM_WINPC
Bool CRenderInterface::GetHardwareDLSSSupport() const
{
	return GetNGX()->CheckHardwareDLSSSupport();
}
#endif

#if defined( RED_USE_IMGUI ) && defined( RED_PLATFORM_WINPC )
void CRenderInterface::GetDLSSDebugSettings( Bool& enabledInNGX, Int32& ngxQuality ) const
{
	const auto* ngx = GetNGX();
	enabledInNGX = ngx->IsDLSSEnabled();
	ngxQuality = ngx->GetDLSSQuality();
}
#endif

namespace Config
{
	TConfigVar< Int32 >		cvTreesLateAllocVSLimit			( "Rendering/LateVSAlloc", "Trees", 50, 0, 63, eConsoleVarFlag_Save );
	TConfigVar< Int32 >		cvGrassLateAllocVSLimit			( "Rendering/LateVSAlloc", "Grass", 28, 0, 63, eConsoleVarFlag_Save );
	TConfigVar< Int32 >		cvTerrainLateAllocVSLimit		( "Rendering/LateVSAlloc", "Terrain", 0, 0, 63, eConsoleVarFlag_Save );
	TConfigVar< Int32 >		cvHairLateAllocVSLimit			( "Rendering/LateVSAlloc", "Hair", 28, 0, 63, eConsoleVarFlag_Save );
	TConfigVar< Int32 >		cvParticlesLateAllocVSLimit		( "Rendering/LateVSAlloc", "Particles", 28, 0, 63, eConsoleVarFlag_Save );
}

SRenderFrameContext::SRenderFrameContext( const job::RunContext& runCtx, const TRenderPtr<IRenderFrame>& frame )
	: m_dispatcherThreadIndex( runCtx.dispatcherThreadIndex )
	, m_builder( runCtx )
	, m_frame( frame )
{

}



void CRenderInterface::RequestGpuHang()
{
	// Don't want to accidentally allow a crash in Final :)
#if !defined(RED_CONFIGURATION_FINAL) || defined(USE_PROFILER)

	m_renderCommandHandler->RunSyncedWithCommands( "CrashTheGpu", [this]( const job::RunContext& runContext )
	{
		job::Builder depBuilder { runContext };

		GpuApi::CommandListRef commandList = GpuApi::CreateCommandList( GpuApi::CommandListType::Default, 0 );
		GpuApi::SetResourceDebugName( commandList, "GpuCrasher" );
		GpuApi::BindCommandList( commandList );

		// Create a buffer to give to the shader. Doesn't really matter what it is, mainly just want to have a UAV to
		// add barriers on.
		GpuApi::BufferDesc bufferDesc;
		bufferDesc.size = 64*sizeof(Uint32);
		bufferDesc.usage = GpuApi::BUT_Default;
		bufferDesc.category = GpuWrapApi::BCC_Raw;
		bufferDesc.group = GpuWrapApi::BufferGroup::Debug;
		GpuApi::Buffer buffer = GpuApi::Buffer::Create( bufferDesc, GpuApi::BufferInitData::Uninitialized() );

		GpuApi::BindBufferUAV( buffer, 0 );

		GetShader( SHADER_NAME("m_gpuCrasherCS") )->Bind();

		struct CB
		{
			Uint32 step;
			Uint32 value;
			Int32 value2;
		} cb = {};

		// (A) Something to run and finish before.
		GpuApi::BeginProfilerBlock("FinishedBefore");
		{
			cb.step = 0;
			cb.value = 0;
			GpuApi::SetComputeShaderConsts( cb );
			GpuApi::DispatchCompute( 3, 1, 1 );
		}
		GpuApi::EndProfilerBlock();

		// Sync to finish (A)
		GpuApi::BarrierBufferUAV( buffer );

		// (B) Something to run before (might be in progress?).
		GpuApi::BeginProfilerBlock("RunningBefore");
		{
			cb.step = 0;
			cb.value = 0;
			GpuApi::SetComputeShaderConsts( cb );
			GpuApi::DispatchCompute( 1, 1, 1 );
		}
		GpuApi::EndProfilerBlock();

		// (C) Something to crash (might overlap).
		GpuApi::BeginProfilerBlock("Crashing");
		{
			cb.step = 1;
			cb.value = 16;
			cb.value2 = UINT32_MAX;
			GpuApi::SetComputeShaderConsts( cb );
			GpuApi::DispatchCompute( 1, 1, 1 );
		}
		GpuApi::EndProfilerBlock();

		// (D) Something to run after (might be in progress?).
		GpuApi::BeginProfilerBlock("RunningAfter");
		{
			cb.step = 0;
			cb.value = 32;
			GpuApi::SetComputeShaderConsts( cb );
			GpuApi::DispatchCompute( 1, 1, 1 );
		}
		GpuApi::EndProfilerBlock();

		// Sync to finish everything above
		GpuApi::BarrierBufferUAV( buffer );

		// (E) Something to run after.
		GpuApi::BeginProfilerBlock("NotRunningYet");
		{
			cb.step = 0;
			cb.value = 0;
			GpuApi::SetComputeShaderConsts( cb );
			GpuApi::DispatchCompute( 3, 1, 1 );
		}
		GpuApi::EndProfilerBlock();


		GpuApi::BindCommandList( {} );
		GpuApi::CloseAndSubmitCommandLists( "GonnaCrash", red::MakeArraySpan( &commandList, 1 ), GpuApi::CommandListSyncType::None, depBuilder );
	} );

#endif
}

void CRenderInterface::SetObserverVelocity( Float velocity )
{
	m_giStreamingTracker.m_currentObserverVelocity = velocity;
}

Float CRenderInterface::GetGIStrength() const
{
	return m_giStreamingTracker.m_currentGIStrength;
}

void CRenderInterface::GIStreamingRegistration( Uint64 nodeId, Bool unregister /*= false */ )
{
	unregister ? m_giStreamingTracker.Unregister( nodeId ) : m_giStreamingTracker.Register( nodeId );
}

Uint32 CRenderInterface::GetNumSectorsWaiting() const
{
	return m_giStreamingTracker.m_numCurrentlyWaiting;
}

Float CRenderInterface::GetAvgWaitingTime() const
{
	return m_giStreamingTracker.m_currentWaitingAvg;
}

Float CRenderInterface::GetGIObserverVelocity() const
{
	return m_giStreamingTracker.m_currentObserverVelocity;
}

GIStreamingTracker::GIStreamingTracker()
	: m_currentGIStrength( 1.0f )
	, m_currentObserverVelocity( 0.0f )
	, m_currentPhase( Phase::GION )
	, m_currentWaitingAvg( 0.0 )
	, m_numCurrentlyWaiting( 0 )
	, m_lastStateToggleTime( EngineTime::ZERO )
	, m_waitingNodes( rend::PoolGI() )
{

}

//////////////////////////////////////////////////////////////////////////

void GIStreamingTracker::Register( Uint64 nodeId )
{
	RED_SCOPE_LOCK( m_lock );

	//for ( auto& entry : m_waitingNodes )
	//{
	//	ALWAYSENABLED_RED_FATAL_ASSERT( entry.nodeId != nodeId, "Already in the tracker!" );
	//}

	m_waitingNodes.Grow( 1 );
	m_waitingNodes.Back().nodeId = nodeId;
	m_waitingNodes.Back().timeOfIssue = EngineTime::GetNow();
}

void GIStreamingTracker::Unregister( Uint64 nodeId )
{
	RED_SCOPE_LOCK( m_lock );
	for ( Entry entry : m_waitingNodes )
	{
		if ( entry.nodeId == nodeId )
		{
			m_waitingNodes.RemoveReorder( entry );
			return;
		}
	}

	ALWAYSENABLED_RED_FATAL( "Wasn't in the tracker!" );
}

void GIStreamingTracker::Tick()
{
	RED_SCOPE_LOCK( m_lock );

	// This is the threshold for saying "it's fucked, need to react (hide GI)"
	static volatile Uint32 NumWaitingUpperThreshold = 10;
	static volatile Float WaitTimeAvgUpperThreshold = 2.0f;
	static volatile Float VelocityUpperThreshold = 12.0f; // 11.5 is the fastest sprint with all upgrades

	// This is the threshold for saying "it's good, can show GI"
	static volatile Uint32 NumWaitingLowerThreshold = 2;
	static volatile Float WaitTimeAvgLowerThreshold = 1.0f;
	static volatile Float VelocityLowerThreshold = 3.0f;

	// Time to blend out / blend in fully
	static volatile Float GIBlendOverTime = 3.0f;
	const EngineTime now = EngineTime::GetNow();
	if ( m_currentPhase == Phase::GION || m_currentPhase == Phase::GIOFF )
	{
		// Collect info needed to perform heuristic
		// Check how many waiting
		m_numCurrentlyWaiting = m_waitingNodes.Size();

		// Check what's the average waited time
		if ( m_numCurrentlyWaiting > 0 )
		{
			Float timeWaitedSum = 0.0f;
			for ( const auto& entry : m_waitingNodes )
			{
				const Float timeWaitedByNode = (Float)( now - entry.timeOfIssue );
				timeWaitedSum += timeWaitedByNode;
			}
			m_currentWaitingAvg = timeWaitedSum / m_numCurrentlyWaiting;
		}
		else
		{
			m_currentWaitingAvg = 0.0f;
		}

		if ( m_currentPhase == Phase::GION )
		{
			// Apply heuristic to potentially HIDE GI
			if ( m_currentObserverVelocity >= VelocityUpperThreshold && m_numCurrentlyWaiting >= NumWaitingUpperThreshold && m_currentWaitingAvg >= WaitTimeAvgUpperThreshold )
			{
				// Switch phase to BlendOut
				m_currentPhase = Phase::BlendingOut;
				m_lastStateToggleTime = now;
			}
		}

		if ( m_currentPhase == Phase::GIOFF )
		{
			// Apply heuristic to potentially SHOW GI
			if ( m_currentObserverVelocity <= VelocityLowerThreshold && m_numCurrentlyWaiting <= NumWaitingLowerThreshold && m_currentWaitingAvg <= WaitTimeAvgLowerThreshold )
			{
				// Switch phase to BlendOut
				m_currentPhase = Phase::BlendingIn;
				m_lastStateToggleTime = now;
			}
		}
	}
	else if ( m_currentPhase == Phase::BlendingOut )
	{
		const Float timeSinceToggle = (Float)( EngineTime::GetNow() - m_lastStateToggleTime );
		m_currentGIStrength = 1.0f - Clamp( timeSinceToggle / GIBlendOverTime, 0.0f, 1.0f );

		if ( m_currentGIStrength == 0.0f )
		{
			m_currentPhase = Phase::GIOFF;
			m_lastStateToggleTime = now;
		}
	}
	else if ( m_currentPhase == Phase::BlendingIn )
	{
		const Float timeSinceToggle = (Float)( EngineTime::GetNow() - m_lastStateToggleTime );
		m_currentGIStrength = Clamp( timeSinceToggle / GIBlendOverTime, 0.0f, 1.0f );

		if ( m_currentGIStrength == 1.0f )
		{
			m_currentPhase = Phase::GION;
			m_lastStateToggleTime = now;
		}
	}
	else
	{
		ALWAYSENABLED_RED_FATAL( "Shouldn't reach this path!" );
	}

}
