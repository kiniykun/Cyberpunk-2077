#include "native-initialization-job-trace.h"
/**
* Copyright (c) 2007-16 CD Projekt Red. All Rights Reserved.
*/
#include "build.h"
#ifdef RED_PLATFORM_WEB
#include <cstdio>
#define CPWEB_INIT_BEGIN(name) std::fprintf(stderr, "CP2077 native initializer begin: %s\n", name)
#define CPWEB_INIT_FAILURE(name) std::fprintf(stderr, "CP2077 native initializer failed: %s\n", name)
#else
#define CPWEB_INIT_BEGIN(name) ((void)0)
#define CPWEB_INIT_FAILURE(name) ((void)0)
#endif
#include "baseEngine.h"

#include "profilerControlCenter.h"
#include "renderEngineHandler.h"
#include "baseEngineCrashData.h"
#include "scriptErrorDialog.h"

#include "../../commChannel/include/networkConnection.h"

#include "../../core/include/debugPageServer.h"

#ifndef NO_DEBUG_PAGES
#include "../../debugPages/include/debugPagesApi.h"
#endif // NO_DEBUG_PAGES

#include "../../inkCore/include/inkSystem.h"

#include "../../localization/include/localizationManager.h"

#include "../../physics/include/physicsConfigVars.h"
#include "../../physics/include/physXNatives.h"

#include "../../animation/include/animationSystemInit.h"

#include "../../redConfig/include/configVarSystem.h"

#include "../../redCore/include/commandline.h"
#include "../../redCore/include/debugServerManager.h"
#include "../../redCore/include/globalModeInfo.h"
#include "../../redContainers/include/string/stringUtils.h"
#include "../../redJobs2/include/jobBuilder.h"

#include "../../redReflection/include/gatheredResource.h"
#include "../../redReflection/include/dlcSystem.h"

#include "../../resourceMaterial/include/materialCompilationManager.h"

#include "../../shaderCache/include/shaderCacheManager.h"

#include "../../sound/include/soundPublic.h"
#include "../../sound/include/soundSystem.h"

#include "../../renderData/include/debugFilter.h"
#include "../../renderData/include/renderPublicFunctions.h"
#include "../../renderData/include/viewport.h"
#include "../../redSystem/include/stopWatch.h"

#ifdef RED_USE_IMGUI
#include "../../renderData/include/renderingMemory.h"
#include "../../imgui/include/redImGui.h"
#endif

#ifdef RED_PLATFORM_ORBIS
# include <kernel.h>
# include <libnetctl.h>
#include <system_service.h>
# ifdef RED_CONFIGURATION_ORBIS_USE_DBGLIB
# include <libdbg.h>
# endif
#endif

#include "../../redReflection/include/version.h"
#include "../../renderData/include/renderer.h"
#include "baseEngineInitParams.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../redNetwork/include/manager.h"
#include "../../redNetwork/include/ping.h"
#include "../../debugPages/include/debugPageRegisterer.h"
#include "inGameProfilerFrontEnd_CPU.h"
#include "inGameProfilerFrontEnd_CPUChromeTracing.h"
#include "../../renderData/include/captureParameters.h"
#include "../../redMemory/include/reportUtils.h"

using red::DynArray;

static const Uint16 EDITOR_PORT = 37000;
static const Uint16 GAME_START_PORT = 37001;

namespace Config
{
	TConfigVar< String > cvConsoleClassName( "Debug/Console", "ClassName", "CDebugConsole" );

	// [potato] defined in the renderData overlay renderInGameSettings.cpp
	void CpPotatoApply( const AnsiChar* where );
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

#if !defined( RED_CONFIGURATION_FINAL ) && defined( RED_PLATFORM_ORBIS )
static Uint8 GBlinkenLichten;
#define DEBUG_BLINK() do { ::sceKernelSetGPO( ++GBlinkenLichten ); } while( false )
#define DEBUG_BLINKOFF() do { ::sceKernelSetGPO( 0 ); } while( false )
#define DEBUG_BLINKALL() do { ::sceKernelSetGPO( 0xFF ); } while( false )
#else
#define DEBUG_BLINK()
#define DEBUG_BLINKOFF()
#define DEBUG_BLINKALL()
#endif

#ifndef RED_PLATFORM_CONSOLE

#define SAFE_ASYNC_INIT( func, name )\
 CPWEB_INIT_BEGIN(name);\
 if ( !(func) )\
 { \
	CPWEB_INIT_FAILURE(name);\
	::dd::GetEngineCrashData().safeInitName.Set( "FAILED - " name );\
	RED_LOG_ERROR( "CBaseEngine: error initializing: %hs", name );\
	m_state.SetValue( BES_Uninitialized ); \
	return;\
 }\
 else\
 {\
	 ::dd::GetEngineCrashData().safeInitName.Set( name );\
	 DEBUG_BLINK();\
 }

#define SAFE_INIT( func, name )\
 CPWEB_INIT_BEGIN(name);\
 if ( !(func) )\
 { \
	CPWEB_INIT_FAILURE(name);\
	::dd::GetEngineCrashData().safeInitName.Set( "FAILED - " name );\
	RED_LOG_ERROR( "CBaseEngine: error initializing: %hs", name );\
	return false;\
 }\
 else\
 {\
	 ::dd::GetEngineCrashData().safeInitName.Set( name );\
	 DEBUG_BLINK();\
 }

#else

#define SAFE_ASYNC_INIT( func, name )\
 CPWEB_INIT_BEGIN(name);\
 if ( !(func) )\
 { \
	CPWEB_INIT_FAILURE(name);\
	::dd::GetEngineCrashData().safeInitName.Set( "FAILED - " name );\
	ALWAYSENABLED_RED_FATAL( "CBaseEngine: error initializing: %hs", name );\
	m_state.SetValue( BES_Uninitialized ); \
	return;\
 }\
 else\
 {\
	 ::dd::GetEngineCrashData().safeInitName.Set( name );\
	 DEBUG_BLINK();\
 }

#define SAFE_INIT( func, name )\
 CPWEB_INIT_BEGIN(name);\
 if ( !(func) )\
 { \
	CPWEB_INIT_FAILURE(name);\
	::dd::GetEngineCrashData().safeInitName.Set( "FAILED - " name );\
	ALWAYSENABLED_RED_FATAL( "CBaseEngine: error initializing: %hs", name );\
	return false;\
 }\
 else\
 {\
	 ::dd::GetEngineCrashData().safeInitName.Set( name );\
	 DEBUG_BLINK();\
 }

#endif

namespace helper
{
	static const char* GetWatermarkExeInfo()
	{
		static char exeInfo[1024] = "";
		static Bool doOnce = false;
		if (doOnce)
		{
			return exeInfo;
		}
		doOnce = true;

#if defined( RED_CONFIGURATION_FINAL )
		const char* configName = " (FINAL)";
#elif defined( RED_CONFIGURATION_RELEASE )
		const char* configName = " (RELEASE)";
#elif defined( RED_CONFIGURATION_NOPTS )
		const char* configName = " (NOPTS)";
#elif defined( RED_CONFIGURATION_DEBUG )
		const char* configName = " (DEBUGFULL)";
#else
# error Unexpected configuration;
#endif

#ifdef RED_CONFIGURATION_ORBIS_USE_DBGLIB
		{
			char exePath[SCE_KERNEL_PATH_MAX] = {};
			const Int32 numWritten = ::sceDbgGetExecutablePath(exePath, RED_ARRAY_COUNT_U32(exePath));
			RED_FATAL_ASSERT(numWritten != SCE_SYSTEM_SERVICE_ERROR_PARAMETER);
			if (numWritten == SCE_SYSTEM_SERVICE_ERROR_REJECTED)
			{
				red::Strcpy(exeInfo, "<Launched by system>", RED_ARRAY_COUNT_U32(exeInfo));
			}
			else
			{
				const char* hostPrefix = "/host/";
				const Uint32 hostPrefixLen = red::Strlen(hostPrefix);
				if (red::Strcmp(hostPrefix, exePath, hostPrefixLen) == 0)
				{
					red::Strcpy(exeInfo, "<Host>", RED_ARRAY_COUNT_U32(exeInfo));
				}
				else
				{
					red::Strcpy(exeInfo, exePath, RED_ARRAY_COUNT_U32(exeInfo));
				}
			}
			red::Strcat(exeInfo, " ", RED_ARRAY_COUNT_U32(exeInfo));
		}
#endif

		red::Strcat(exeInfo, configName, RED_ARRAY_COUNT_U32(exeInfo));

		char sdkBuf[64] = "";
		RED_TOUCH(sdkBuf);
#if defined( RED_PLATFORM_ORBIS )
		red::SNPrintFUnsafe(sdkBuf, RED_ARRAY_COUNT_U32(sdkBuf), " SCE_ORBIS_SDK_VERSION=0x%08X", SCE_ORBIS_SDK_VERSION);
		red::Strcat(exeInfo, sdkBuf, RED_ARRAY_COUNT_U32(exeInfo));
#elif defined( RED_PLATFORM_DURANGO )
		red::Strcat(exeInfo, " ", RED_ARRAY_COUNT_U32(exeInfo));
		red::Strcat(exeInfo, _XDK_VER_STRING, RED_ARRAY_COUNT_U32(exeInfo));
#endif

		red::Strcat(exeInfo, " ", RED_ARRAY_COUNT_U32(exeInfo));

		red::memory::RuntimeMetrics metrics = {};
		red::memory::GetRuntimeMetrics(metrics);
		char metricsBuf[64] = "";
		red::SNPrintFUnsafe(metricsBuf, RED_ARRAY_COUNT_U32(metricsBuf), "Total memory: %llu bytes", metrics.totalPhysicalMemory);

		red::Strcat(exeInfo, metricsBuf, RED_ARRAY_COUNT_U32(exeInfo));
		red::Strcat(exeInfo, " ", RED_ARRAY_COUNT_U32(exeInfo));

		return exeInfo;
	}

	static String PrintVersionWatermark()
	{
		const Bool isInternal = red::Strcmp(APP_LAST_P4_CHANGE, "CL_INTERNAL") == 0;
		// Internal can use a shorter debug string without a bunch of useless placeholders
		const char* const appVer = isInternal ? APP_LAST_P4_CHANGE : APP_VERSION_NUMBER;
		return String::Printf( "%s %s %s %s", appVer, APP_DATE, __TIME__, GetWatermarkExeInfo() );
	}
}

namespace helper
{
	// Copy-pasted everywhere, should really put in some utility lib...
#if defined( RED_PLATFORM_WINPC )
	static red::AbsolutePath GetExecutableDirectory()
	{
		UniChar moduleFileName[ MAX_PATH ];
		UniChar moduleFullPath[ MAX_PATH ];
		::GetModuleFileNameW( nullptr, moduleFileName, MAX_PATH );
		::GetFullPathNameW( moduleFileName, MAX_PATH, moduleFullPath, nullptr );

		// Strip EXE name
		UniChar* pEnd = red::StrchrR( moduleFullPath, '\\' );
		if ( !pEnd )
			return red::AbsolutePath();
		*pEnd = '\0';

		// returns the directory path above the bin
		return red::AbsolutePath::CreateDirPath( red::Utf16String( moduleFullPath ) );
	}
#endif

	static void InitAutomatorRepoSteps(const String& automatorID )
	{
		// reprosteps_<uniqeuid>.txt then have the crashreporter copy it to reprosteps.txt in root crashfolder
		// Can't use a timestamp on this side, since I need to ensure it's the correct file and WER on Durango
		// can attach files from previous runs...
#if defined( RED_PLATFORM_WINPC )
		const red::AbsolutePath reproStepsDir = red::paths::ParentAbsolutePath( GetExecutableDirectory() );
#elif defined( RED_PLATFORM_DURANGO )
		const red::AbsolutePath reproStepsDir = red::AbsolutePath::CreateDirPath( "d:\\" );
#else
		const red::AbsolutePath reproStepsDir = red::AbsolutePath::CreateDirPath( "/data/" );
#endif

		auto fileName = String( "reprosteps_automator" ) + automatorID + ".txt";
		fileName.ToLower(); // ensure lower regardles of GUID formatting
		red::AbsolutePath reproStepsFile = reproStepsDir.AddFilePath( fileName );
		auto file = GFileManager->CreateFileWriter( reproStepsFile, FOF_AbsolutePath );
		if ( file )
		{
			String cmdLine = red::CommandLine().Get().Internal_GetRawCommandLine();
			auto repro = String::Printf("Cmdline: '%hs'\r\n%hs\r\n", cmdLine.AsChar(), GEngine->GetVersionWatermark().AsChar());
			file->Serialize( repro.AsChar(), repro.Length() );
			red::RegisterAttachmentForErrorReport( reproStepsFile.AsChar() );
			RED_LOG_INFO( "Registered automator file: %hs", reproStepsFile.AsChar() );
		}
		else
		{
			RED_LOG_WARNING( "FAILED to register automator file: %hs", reproStepsFile.AsChar() );
		}
	}
}

void CBaseEngine::InitializeInitial( const EngineInitParams& params )
{
	dd::InitEngineInitParamsCrashData( params );

#if defined( RED_PLATFORM_WINPC )
	dd::InitDesktopCrashDumpData();
#elif defined( RED_PLATFORM_ORBIS )
	dd::InitOrbisCrashDumpData();
#elif defined( RED_PLATFORM_DURANGO )
	dd::InitDurangoCrashDumpData();
#endif

	m_versionWatermark = helper::PrintVersionWatermark();

	dd::GetEngineCrashData().versionWatermark.Set(m_versionWatermark);
	dd::GetEngineCrashData().isHeadless.Set(IsHeadless());

	RED_LOG_INFO("AutomatorID=%hs", params.m_automatorID.AsChar());
	if ( !params.m_automatorID.Empty() )
	{
		helper::InitAutomatorRepoSteps(params.m_automatorID);
	}

	if (red::CommandLine().Get().HasOption("autoMemDump"))
	{
		Float autoDumpTime = 0.f;
		red::CommandLine().Get().GetFirstParam("autoMemDump", autoDumpTime);
		red::memory::SetAutoDumpInterval(autoDumpTime);
	}

	red::String autoMemDumpFormat;
	if ( red::CommandLine().Get().GetFirstParam( "autoMemDumpFormat", autoMemDumpFormat ) )
	{
		red::memory::SetAutoDumpFormat( autoMemDumpFormat.AsChar() );
	}

	FTInitCrashDumpData();

	m_scriptsSilentCompilation = params.m_scriptsSilentCompilation;
	m_scriptsSilentValidation = params.m_scriptsSilentValidation;
	m_interopStartingPort = params.m_interopStartingPort;
	m_gatheredResourceSilentValidation = params.m_gatheredResourceSilentValidation;

	DEBUG_BLINKOFF();

	{
		Config::MemoryInfo memInfo;

		memInfo.MemoryBudget_RAM = red::memory::AcquireSystemAllocator().GetCurrentPhysicalMemoryAvailable();
		memInfo.MemoryBudget_GPU = GetGpuTotalMemory();

		Config::SelectTextureQualityLevel( (Uint32)params.m_textureQualityLevel, memInfo );
		if (params.m_disableRayTracing)
		{
			Config::cvRayTracingEnable.Set( false );
		}
		if (params.m_disableRayTracedReflection)
		{
			Config::cvRayTracingEnableReflection.Set( false );
		}
		if (params.m_disableRayTracedTransparentReflection)
		{
			Config::cvRayTracingEnableTransparentReflection.Set( false );
		}
		if (params.m_rayTracingAmbientOcclusionRayNumber >= 0)
		{
			Config::cvRayTracingAmbientOcclusionRayNumber.Set( params.m_rayTracingAmbientOcclusionRayNumber );
		}
		Config::cvDLSSEnable.Set( params.m_dlss );
		if ( params.m_dlssQuality >= 0 )
		{
			Config::cvDLSSQuality.Set( params.m_dlssQuality );
		}
	}
}

// asynchronous engine initialization
job::Counter CBaseEngine::InitializeAsync( const EngineInitParams& params )
{
	m_state.SetValue( BES_InitializingRootSystems );

	red::StopWatch timer;

	if ( !OnRootSystemInitialization( params ) )
	{
		RED_LOG_ERROR( "Engine: Failed to initialize root systems" );
		m_state.SetValue( BES_Uninitialized );
		return job::Counter{};
	}

	RED_LOG_INFO( "Engine: RootSystem initialization took %lf ms", timer.GetDeltaMS() );

	timer.Reset();

	job::Builder builder;
	builder.DispatchJob( "Engine/RegularSystemInitialization", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"Engine/RegularSystemInitialization"};
		m_state.SetValue( BES_InitializingRegularSystems );
		OnRegularSystemInitialization( params, context, m_state );
	} );

	builder.DispatchJob( "Engine/InitializationFinished", [this, timer=std::move( timer )]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"Engine/InitializationFinished"};
		const EBaseEngineState state = static_cast< EBaseEngineState >( m_state.GetValue() );
		switch ( state )
		{
		case BES_InitializingRegularSystems:
			m_state.SetValue( BES_Initialized );
			RED_LOG_INFO( "Engine: RegularSystem initialization took %lf ms", timer.GetDeltaMS() );
			break;

		case BES_Uninitialized:
			break;

		default:
			RED_FATAL_ASSERT( 0, "Invalid engine state '%d'", static_cast< Uint32 >( state ) );
		}
	} );

	return builder.ExtractWaitCounter();
}

Bool CBaseEngine::OnRootSystemInitialization( const EngineInitParams& params )
{
	InitializeInitial( params );

	//////////////////////////////////////////////////////////////////////////
	// Do not move anything requiring attached content above this
	//////////////////////////////////////////////////////////////////////////

	SAFE_INIT( InitOle(), "Ole" );
	SAFE_INIT( InitializeProfilers(), "Profiles" );
	SAFE_INIT( InitializeNetwork(), "Network" );
	SAFE_INIT( InitializeViewportManager(), "ViewportManager" );
	SAFE_INIT( InitializeDebugFilters( params ), "DebugFilters" );

#ifdef RED_USE_IMGUI
	SAFE_INIT( InitializeImGui(), "ImGui" );
#endif

	// have to load scripts synchronously, because there are static resources which depend on script classes
	SAFE_INIT( InitializeScripts( params.m_scriptsBlobPath, params.m_scriptVersion, params.m_scriptsProfilerMode ), "Scripts" );

	SAFE_INIT( InitializeLocalizationManager( red::CommandLine::Get() ), "LocalizationManager" );

	SAFE_INIT( InitializeServices(), "GameServices" );

	if ( !IsHeadless() )
	{
		SAFE_INIT( InitializeContentManager(), "ContentManager" );
		SAFE_INIT( InitializeAsyncMaterialCompiler(), "AsyncMaterialCompiler" );
		SAFE_INIT( CreateRenderer(), "Renderer" );
		SAFE_INIT( InitializeRenderer( ( Uint32 ) params.m_qualityLevel ), "RendererInit" );
		SAFE_INIT( LoadEngineGatheredResources(), "GatheredResources" );
		SAFE_INIT( InitializeInkSystem(), "InkSystem" );
	}
	else
	{
		SAFE_INIT( LoadEngineGatheredResources(), "GatheredResources" );
	}

	SAFE_INIT( LoadDlcManifests(), "DlcManifests" );

	return true;
}

void CBaseEngine::OnRegularSystemInitialization( const EngineInitParams& params, const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
{
	job::Builder builder{ context };

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/GameServicesAsync", [ this, params ]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/GameServicesAsync"};
		if ( m_state.GetValue() != BES_Uninitialized )
		{
			LoadResourcesForGameServices( context );
		}
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/Physics", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/Physics"};
		if ( m_state.GetValue() != BES_Uninitialized )
		{
			InitializePhysicsAsync( params.m_pvdDumpToFile, context );
		}
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/LoadRenderResources",[this]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/LoadRenderResources"};
		if ( m_state.GetValue() != BES_Uninitialized )
		{
			InitializeRenderResourceAsync( context );
		}
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/Recorder", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/Recorder"};
		if ( !m_state.GetValue() )
		{
			return;
		}

		SAFE_ASYNC_INIT( InitializeRecorder(), "Recorder" );
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/Animation", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/Animation"};
		if ( !m_state.GetValue() )
		{
			return;
		}

		SAFE_ASYNC_INIT( InitializeAnimation(), "Animation" );
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/DebugServer", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/DebugServer"};
		if ( !m_state.GetValue() )
		{
			return;
		}

		SAFE_ASYNC_INIT( InitializeDebugServer(), "DebugServer" );
	} );

	builder.DispatchJob< job::Fence::None >( "BaseEngine/Initialization/DebugNetwork", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/DebugNetwork"};
		if ( !m_state.GetValue() )
		{
			return;
		}

		SAFE_ASYNC_INIT( InitializeDebugNetwork(), "DebugNetwork" );
	} );

	builder.DispatchFenceExplicitly();

#if !defined( RED_CONFIGURATION_FINAL ) && !defined( RED_PLATFORM_CONSOLE )
	builder.DispatchJob( "BaseEngine/Initialization/StreamingManager", [this, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/StreamingManager"};
		if ( !m_state.GetValue() )
		{
			return;
		}

		SAFE_ASYNC_INIT( InitializeStreamingManager( params.m_cmdlet ), "StreamingManager" );
	} );
#endif

	builder.DispatchJob("BaseEngine/Initialization/Finialization", [this, params](const job::RunContext& context)
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"BaseEngine/Initialization/Finialization"};
		dd::GetEngineCrashData().safeInitName.Set( "Finished" );
	} );
}

Bool CBaseEngine::InitializeContentManager()
{
	// HACK for shaders
	GpuWrapApi::SetShadersEnginePath( GFileManager->GetEngineRoot() );

	// TODO: add initialization of GStaticShaderCache to CPlatform::Init() for Durango and Orbis
	// skipping for now, because of R6.Rendering having platform project imported from Main
	GStaticShaderCache = RED_NEW( CStaticShaderManager );
	if ( !GStaticShaderCache->Init() )
	{
		RED_LOG_ERROR( "Engine: Failed to initialize static shader cache!" );
		return false;
	}

	GShaderCache = RED_NEW( CShaderCacheResolver );
	if ( !GShaderCache->Init() )
	{
		RED_LOG_ERROR( "Engine: Failed to initialize shader cache!" );
		return false;
	}

	return true;
}

Bool CBaseEngine::InitializeNetwork()
{
	// Initialise Network
	RED_LOG( "Status: Initializing Network..." );

#ifdef RED_NETWORK_ENABLED
	m_network = red::CreateUniquePtr< red::Network::Manager >();
	m_network->Initialize( );

#ifdef RED_LOG_VIA_NETWORK
	m_networkedLogger.Initialise( m_network.Get() );
#endif

#endif // RED_NETWORK_ENABLED

	RED_LOG( "Engine: Initializing network connection..." );

	RED_ASSERT( !m_networkConnection, "Network was already initialized" );

	// initialize this application handle (and it's channels)
	m_networkConnection = red::CreateUniquePtr< comm::NetworkConnection >( "EngineLauncher", "Engine Launcher" );

	// initialize network
	return m_networkConnection->Initialize();
}

Bool CBaseEngine::CreateRenderer()
{
	// Create renderer
	RED_LOG( "Status: Creating Renderer..." );
	InitializeGpuMemoryPools();
	CreateRender();

	if ( !GRender )
	{
		RED_LOG_ERROR( "Engine: Unable to create renderer" );
		return false;
	}

	if ( !GRender->Init() )
	{
		RED_LOG_ERROR( "Engine: Unable to initialize renderer" );
		return false;
	}

	IMaterial::InitDependenciesMemoryPools();

	return true;
}

Bool CBaseEngine::InitializeRenderer( Uint32 qualityLevel )
{
	// Initialize renderer. Needs to be after RTTI is up, so resource loading works proper (e.g. CGameplayEffects needs to load some CSV files)
	RED_LOG( "Status: Initializing Renderer..." );

	GRender->Register( red::CreateUniquePtr< RenderEngineHandler >() );

	// Initialize GGlobalRenderingSettings - we need it to do it here so that GRender is up and running
	// and InGameConfig system can pick up ray tracing capabilities.
	{
		Config::MemoryInfo memInfo;
		memInfo.MemoryBudget_RAM = red::memory::AcquireSystemAllocator().GetCurrentPhysicalMemoryAvailable();
		memInfo.MemoryBudget_GPU = GetGpuTotalMemory();

		Config::SelectQualityLevel( qualityLevel, memInfo );
	}

	// [potato] opt-in low-end preset (?potato=1|2|3 / -potato=N; default OFF = no-op). Lowers the settings just selected
	// and sets the engine's own config vars; implementation in the renderData overlay renderInGameSettings.cpp.
	Config::CpPotatoApply( "init" );

#ifndef RED_CONFIGURATION_FINAL
	// Load the config override values for shaders
	GRender->LoadConfig();
#endif // RED_CONFIGURATION_FINAL

	return true;
}

#ifdef RED_USE_IMGUI
static void* ImGuiAllocate( size_t sz, void* user_data )
{
	return RED_ALLOCATE( rend::PoolImGui, sz );
}

static void ImGuiFree( void* ptr, void* user_data )
{
	RED_FREE( rend::PoolImGui, ptr );
}

Bool CBaseEngine::InitializeImGui()
{
	ImGui::SetAllocatorFunctions( ImGuiAllocate, ImGuiFree, nullptr );
	return true;
}
#endif

Bool CBaseEngine::InitializeAsyncMaterialCompiler()
{
#ifndef NO_RUNTIME_MATERIAL_COMPILATION
	m_asyncMaterialCompiler = red::CreateUniquePtr< MaterialCompilationManager >();
	if ( !m_asyncMaterialCompiler )
	{
		return false;
	}
	m_asyncMaterialCompiler->Initialize();
#endif // NO_RUNTIME_MATERIAL_COMPILATION
	return true;
}

Bool CBaseEngine::InitializeInkSystem()
{
	m_inkSystem = red::CreateUniquePtr<ink::InkSystem>();
	if( !m_inkSystem )
	{
		return false;
	}

	return m_inkSystem->Initialize(m_viewportManager.Get());
}

Bool CBaseEngine::InitializeDebugNetwork()
{
#ifdef RED_NETWORK_ENABLED
	if( !m_network )
		return true;

	// Enable listening to incoming connections
	RED_LOG( "Status: Listen for incoming connections..." );

	while( !m_network->IsInitialized() )
	{
		red::SleepOnCurrentThread( 10 );
	}

	const Uint16 numPortsToTry = 32;

	const Uint16 port = ( IsGameMode() )? GAME_START_PORT : EDITOR_PORT;

	m_network->ListenForIncomingConnections( port, numPortsToTry );

	m_pingUtil->Initialize();

	// #tbd: keep the server for anything with NO_DEBUG_PAGES defined?
	CDebugPageServer::GetInstance().Initialize();

# ifndef NO_DEBUG_PAGES
	extern DEBUGPAGES_API void InitStatsDebugPages( DebugPageRegisterer&, Float ( *lastTickRateProvider )() );
	InitStatsDebugPages( *m_debugPageRegisterer, []() { return GEngine->GetLastTickRate(); } );

	extern DEBUGPAGES_API void InitDebugFiltersDebugPages( DebugPageRegisterer& );
	InitDebugFiltersDebugPages( *m_debugPageRegisterer );

	extern DEBUGPAGES_API void InitMemoryMetricsDebugPages( DebugPageRegisterer& );
	InitMemoryMetricsDebugPages( *m_debugPageRegisterer );
# endif
#endif
	return true;
}

Bool CBaseEngine::InitializePhysics( Bool pvdDumpToFile )
{
	RED_LOG( "Status: Initializing Physics..." );

	physics::Config::InitializeConfigVars();

	if ( !physics::CreateNatives() )
	{
		return false;
	}

	if ( pvdDumpToFile )
	{
		physics::FileDumpVisualDebugger();
	}
	else
	{
		physics::ConnectVisualDebugger();
	}

#ifdef PHYSICS_DUMP_PXMATERIAL_COLLECTION
	physics::DumpMaterialCollection("..\\materials.xml", true);
#endif

	return true;
}

void CBaseEngine::InitializePhysicsAsync( Bool pvdDumpToFile, const job::RunContext& context )
{
	job::Builder builder{ context };
	builder.DispatchJob( "Engine/PhysicsInitialization", [this, pvdDumpToFile]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"Engine/PhysicsInitialization"};
		if ( m_state.GetValue() != BES_Uninitialized )
		{
			RED_LOG( "Status: Initializing Physics..." );

			physics::Config::InitializeConfigVars();
			physics::CreateNativesAsync( context, m_state );
		}
	} );

	builder.DispatchJob( "Engine/PhysicsInitialization/Finalization", [this, pvdDumpToFile]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"Engine/PhysicsInitialization/Finalization"};
		if ( m_state.GetValue() != BES_Uninitialized )
		{
			if ( pvdDumpToFile )
			{
				physics::FileDumpVisualDebugger();
			}
			else
			{
				physics::ConnectVisualDebugger();
			}
		}
	} );
}

void CBaseEngine::InitializeRenderResourceAsync( const job::RunContext& context )
{
	IRenderEngineHandler * handler = GRender->GetEngineHandler();
	if( handler )
	{
		job::Builder builder( context );
		handler->LoadRenderResources( builder );
	}
}

Bool CBaseEngine::InitializeAnimation()
{
	anim::Initialize();
	return true;
}

Bool CBaseEngine::InitializeDebugServer()
{
	DBGSRV_CALL( Init( m_networkConnection->GetChannelFactory() ) );
	return true;
}

Bool CBaseEngine::InitializeSoundSystem()
{
	m_soundSystem = red::CreateUniquePtr<audio::SoundSystem>();

	if ( m_soundSystem->Init() )
	{
		return true;
	}

	m_soundSystem.Reset();
	return false;
}

Bool CBaseEngine::InitializeLocalizationManager( const red::CommandLine& commandLine )
{
	m_localizationManager = red::CreateUniquePtr< loc::LocalizationManager >();

	return true;
}

Bool CBaseEngine::InitializeRecorder()
{
#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
	const red::CommandLine& commandLine = red::CommandLine::Get();
	if ( commandLine.HasOption( "recorderParams" ) )
	{
		red::DynArray< String > parts = commandLine.GetValues( "recorderParams" );
		String paramsStr = String::Join( parts, " " );

		// Hahahack
		{
			Uint32 startIndex;
			Uint32 endIndex;

			const String pathParamStr = "{Path=";
			if ( paramsStr.IndexOf( pathParamStr, startIndex ) )
			{
				if ( paramsStr.IndexOf( "}", endIndex, startIndex ) )
				{
					paramsStr.Insert( startIndex + pathParamStr.Length(), '\"' );
					paramsStr.Insert( endIndex + 1, '\"' );

					paramsStr.ReplaceAll( "\\", "\\\\" );
				}
			}
		}

		rtti::ValuePtr value = rtti::ValueHolder::Parse( paramsStr.AsChar() );
		rend::CaptureParameters params;
		if ( GetTypeObject< rend::CaptureParameters >()->WriteValue( IRTTIContext::GetDefault(), &params, rtti::AccessPath(), *value ) )
		{
			SetCaptureParameters( params );
		}
	}
#endif // RED_CINEMATIC_SCREENSHOT_CAPTURE
	return true;
}

Bool CBaseEngine::InitializeServices()
{
	return true;
}

void CBaseEngine::LoadResourcesForGameServices( const job::RunContext& context )
{
}

void CBaseEngine::ShutdownServices()
{
}

Bool CBaseEngine::InitializeProfilers()
{
#ifdef USE_PROFILER
	const red::CommandLine& commandLine = red::CommandLine::Get();

	if ( commandLine.HasOption( "noprofilers" ) )
	{
		return true;
	}

	// dUd: temp disabled - WIP
	String activeProfilers = Config::cvActiveProfilers.Get();

	// Profiler overrides
	if( commandLine.GetFirstParam( "profiler", activeProfilers ) == true )
	{
		RED_LOG( "Engine: Custom profiler override: '%hs'", activeProfilers.AsChar() );
	}

	String profilerServer;
	if( commandLine.GetFirstParam( "profilerServer", profilerServer ) == true )
	{
		RED_LOG( "Engine: Custom profiler server override: '%hs'", profilerServer.AsChar() );
		Config::cvProfilerServerName.Set( profilerServer );
	}

	String breakpointThresholdMs;
	Bool hasBreakpointThreshold = false;
	if ( commandLine.GetFirstParam( "breakpointThresholdMs", breakpointThresholdMs ) == true )
	{
		Config::cvProfilerBreakpointThresholdMs.Set( static_cast< Float >( std::atof( breakpointThresholdMs.AsChar() ) ) );
		RED_LOG( "Engine: Setting breakpointThresholdMs to '%s' for Chrome Tracing profiler", breakpointThresholdMs.AsChar() );
		hasBreakpointThreshold = true;
	}

	// initialized control center
	red::ProfilerControlCenter::RegisterConsoleCommands();

	// dUd: todo - enable all tools
	PROFILER_SetEditor( false );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_REDPROFILER, true );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_PIX, true );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_VTUNE, true );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_RAZOR, true );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_NVIDIA, true );
	PROFILER_EnableTool( red::ProfilerToolSlot::PTS_TRACY, true );

	// dUd: todo - enable all tools
	PROFILER_EnableExtension( red::ProfilerExtensionSlot::PES_MEMORY, true );

	// register remote control of profiler
	DBGSRV_REG_PLUGIN( RED_NEW( red::ProfilerControlCenter )(), true );

#ifdef USE_RED_INGAME_PROFILER
	if ( !IsHeadless() )
	{
		m_inGameProfilerFrontEnd->Init();
		if ( hasBreakpointThreshold )
		{
			m_inGameChromeTracingProfiler->Init( m_inGameProfilerFrontEnd.Get(), Config::cvProfilerBreakpointThresholdMs.Get() );
		}
		else
		{
			m_inGameChromeTracingProfiler->Init( m_inGameProfilerFrontEnd.Get() );
		}
	}
#endif

#endif
	return true;
}

Bool CBaseEngine::InitializeScripts( const red::AbsolutePath& blobPath, script::ScriptVersion scriptVersion, script::ProfilerMode profilerMode )
{
	m_scriptBlobPath = blobPath;
	m_scriptVersion = scriptVersion;

	return InitializeScriptsData( scriptVersion, profilerMode );
}

bool CBaseEngine::LoadEngineGatheredResources()
{
	const Uint32 flags = IsHeadless() ? res::prv::GRLF_OnlyHeadless : 0;
	if ( !res::prv::LoadEngineGatheredResources( flags ) )
	{
		const auto& gatheredResourceErrors = res::prv::GetGatheredResourceErrors();
#ifdef RED_PLATFORM_WINPC
		if ( !m_gatheredResourceSilentValidation )
		{
			ScriptErrorDialogData errorDialog;
			errorDialog.m_caption = "Errors loading Gathered Resources!";
			errorDialog.m_errors.PushBack( "Not all gathered resources could be loaded, please check the errors below and get any missing files before trying this operation again." );
			errorDialog.m_errors.PushBack( "The engine will now crash." );
			errorDialog.m_errors.PushBack( gatheredResourceErrors );
			ShowScriptErrorDialogBox( errorDialog );
		}
#endif
		const auto combinedErrorString = red::StrJoin( gatheredResourceErrors, "\r\n" );
		ALWAYSENABLED_RED_FATAL( "Not all Gathered Resources were loaded!\r\n%hs", combinedErrorString.AsChar() );
		return false;
	}
	return true;
}

bool CBaseEngine::LoadDlcManifests()
{
	// NOTE: Yes I know this isn't ideal but there shouldn't be that many manifests (up to 20 or so)
	// and they are very minimal to load. 
	// NOTE: They also should be loaded before we try to initialise any other systems because they might
	// need to know what DLCs we are running and load data from the manifests
	auto counter = res::GDlcSystem.LoadManifests();
	job::FlushCounter( counter );
	return true;
}

Bool CBaseEngine::InitializeViewportManager()
{
	// initialize the preview window system, on consoles use the main viewport as the master
	// there's a slight difference how the preview system is managed on PC and on the console
	m_platformRenderingViewport = ViewportHandle(); // for now (PC only)

	// create the viewport manager
	m_viewportManager = red::CreateUniquePtr< rend::EngineViewportManager >( m_platformRenderingViewport );
	return true;
}

Bool CBaseEngine::InitializeDebugFilters( const EngineInitParams& params )
{
	auto& defaultGameFilters = rend::GetDefaultGameFilters();

	// TODO: a different setup of render filters should be loaded based on the engine configuration
	InitializeCommonDefaultDebugFilters( defaultGameFilters, params.m_renderPreset );
	InitializeDefaultGameDebugFilters();

#if defined( RED_CONFIGURATION_FINAL ) && defined( USE_PROFILER )

	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay", true );
	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/InGameProfiler", true );
	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/FPS", true );
	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/CPUTime", true );
	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/GPUTime", true );
	rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/MemoryMetrics", true );

#else
	if ( params.m_profilerEnabled )
	{

		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/FPS", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/CPUTime", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/GPUTime", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/GPU_PSOs", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/MemoryMetrics", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/TriangleCount", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/TriangleCount/MainCamera", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/TriangleCount/Shadows", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/MeshChunkCount", true );
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "Overlay/Stats/InGameProfiler", true );
	}
#endif

	if ( params.m_showVersionWatermark)
	{
		rend::DebugFilter::ToggleFilter( defaultGameFilters, "VersionWatermark", true );
	}


	const char* gameFilterName = rend::GetGameFilterName();
	auto& gameFilters = rend::DebugFilterMask::GetFilterMask( gameFilterName );
	gameFilters = defaultGameFilters;

	// override game filters based on filters defined in *.ini
	LoadGameDebugFilters();

	return true;
}

void CBaseEngine::InitializeDefaultGameDebugFilters()
{
	auto& gameFilters = rend::GetDefaultGameFilters();

	// #tbd: not in overlay since it seems like all over overlay gets disabled in final
	// but this shouldn't be visible in a "shipping" config
	rend::DebugFilter::ToggleFilter(gameFilters, "VersionWatermark", false);

	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay", true );

#if defined( USE_PROFILER )
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Console", true );
#else
	rend::DebugFilter::ToggleFilter(gameFilters, "Overlay/Console", false );
#endif

	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/DebugHUD", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/DebugVisualizer", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/DebugPageServer", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Visualizations", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/LoadingScreen", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/FadeOut", false );

	 // Don't clutter up the editor, game engine will enable
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/FPS", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/CPUTime", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/GPUTime", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/ScriptsTime", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/MemoryMetrics", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/TriangleCount/MainCamera", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/TriangleCount/Shadows", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/MeshChunkCount", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/LoadingMetrics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/IO", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/GPUMemoryMetrics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/ResourceGPUMemoryMetrics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/GPUPerformance", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/CPUPerformance", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/InGameProfiler", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/Particles", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/CrowdDensity", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/Entities", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/DynEntities", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/Nodes", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/Nodes/ObserverDetails", false );

#if !defined( RED_CONFIGURATION_FINAL)
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/GPU_PSOs", true );
#else
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/GPU_PSOs", false );
#endif

	rend::DebugFilter::ToggleFilter( gameFilters, "Physics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Inspector", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation/Generation/InputGeometry", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation/Generation/RawHeightfield", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation/Generation/FilteredHeightfield", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation/Generation/RawCompactHeightfield", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Navigation/Generation/FilteredCompactHeightfield", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Pivot", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Snap", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Snap/AllPoints", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Snap/PotentialPoints", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Snap/ActivePoints", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/SelectedName2d", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/Sockets", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/SocketsNamesAlwaysVisible", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes/SecondaryReferencePointInfo", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Selection", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Selection/NodeDepth", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Selection/NodeName", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Proxy/SpawnInfo", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Proxy/ResourceProxyMeshOnly", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Proxy/NodeProxyMeshOnly", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/AsMarker", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Area", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Building", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Decoration", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Generated", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Quest", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Regular", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Road", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Prefabs/Type/Terrain", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/PerformanceNotification", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/Debug", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/DummyObject", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/DummyPlayerFPP", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/PlayerCharacter", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Multiplayer/Weapon", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Ink", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/Draw", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/HUD", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/Menu", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/WorldWidgets", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/InteractionPlanes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Ink/GameUI", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "GameSystem/GPS/DrawInWorld", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "GameSystem/GPS/Debug", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity/Transforms", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity/Animation", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity/Physics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/GeometryDescription", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity/Workspots", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "TimeDilation", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "TimeDilation/Controller", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "TimeDilation/Player", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Traffic", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Traffic/SlotsThatFinishedMovement", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Traffic/Lanes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Traffic/ParkingSpaces", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/AutoSelectEnabled", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/DisplayAdditionalInfo", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/PerformPhysicsCheck", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/PerformNavMeshCheck", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/AlwaysDrawErrorEntries", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "AI/Workspots/ActionSpotsDebugger/TagLists", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Interactions", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Interactions/HotSpots", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Log", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/AI", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Animation", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Audio", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Core", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Default", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Effects", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Engine", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/GameStateMachine", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Gameplay", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/GameplayError", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Input", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Interop", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/ItemFactory", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Math", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Physics", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Rendering", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Scripts", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Tools", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Localization", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/UserInterface", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/PlayerLocomotion", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Resources", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Scanning", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Interactions", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/PlayerBreadcrumbs", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/FunctionalTests", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Population", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Log/Chatter", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Audio", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/MuteAll", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/MuteSfx", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/MuteVo", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/MuteMusic", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/PlaceholderVO", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/DrawReverbDistances", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/DrawReverbDebug", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/DrawOcclusion", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/DrawMusicSync", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/UseOcclusion", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/UseDynamicReverb", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/RenderSelection", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Triggers", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Triggers/Sfx", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Triggers/Music", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Triggers/SoloSelected", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Triggers/OuterShape", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Mix", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Mix/RedConf", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Metadata/WeaponWielding/LeftHand", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Audio/Metadata/WeaponWielding/RightHand", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Localization", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Localization/LogLocalizationQueries", false );

#if defined( RED_CONFIGURATION_FINAL )
	rend::DebugFilter::ToggleFilter( gameFilters, "Physics", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Nodes", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "TimeDilation", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Interactions", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Vehicle/Suspension", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Vehicle/Spawner", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Vehicle/Physics", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/Stats/FPS", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/DebugPageServer", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Overlay/LoadingScreen", false );
#endif

	rend::DebugFilter::ToggleFilter( gameFilters, "Components", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Game", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Sprites", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/LightBounds", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaLightShape", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ShadowAngleAreaLightShape", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/FogVolumes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/FogVolumes/Sprite", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/FogVolumes/Box", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Markers", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/QuestMarkers", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/QuestMarkers/MainQuest", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/QuestMarkers/SideQuest", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/QuestMarkers/StreetStory", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Debug_Shapes", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Debug_Shapes_InGame", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/MovingPlatformPreview", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/MovingPlatform", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines/Selectable", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines/RotationVisualization", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines/RotationVisualization/X", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines/RotationVisualization/Y", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Splines/RotationVisualization/Z", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/CurvePaths", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes/Outline", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes/OutlineSeethrough", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes/Volume", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes/Selectable", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/AreaShapes/Interiors", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Stickers", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Locomotion", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Editor", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Skeletons", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Skeletons/BonesLines", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Skeletons/BonesAxes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Skeletons/BonesNames", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/EntityName", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/PlayedAnimations", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/FiredEvents", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/DiscardedEvents", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/TrackErrors", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/BoneErrors", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/AnimationErrors", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/ExternalInputsErrors", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/Profiler", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/Animation/LOD", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/GISpaces", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VirtualCamera", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/WorkspotOffsets", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/FreeCamera", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "RuntimeSystem/Entity/Workspots", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ColliderComponent", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ColliderComponent/Axes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ColliderComponent/Outline", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ColliderComponent/Volume", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/ColliderComponent/BoundingBox", true );

	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent/Axes", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent/BoundingBox", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent/Outline", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent/Volume", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Components/VehicleChassisComponent/ShowPlayerCollider", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Triggers", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Triggers/Shapes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Triggers/Activators", false );

	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Source", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Source/MeshNode", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Source/OccluderNode", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Source/InstancedOccluderNode", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Source/OccluderComponent", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Occluder", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Occluder/OnlyFlatMeshes", false );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Occluder/Outline", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Occluder/Volume", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "Occlusion/Occluder/XRayMode", false );

	rend::DebugFilter::ToggleFilter(gameFilters, "RecordDebugDrawHistory", false);
	rend::DebugFilter::ToggleFilter(gameFilters, "RecordDebugDrawHistory/Default", false);
	rend::DebugFilter::ToggleFilter(gameFilters, "RecordDebugDrawHistory/Player", false);
	rend::DebugFilter::ToggleFilter(gameFilters, "RecordDebugDrawHistory/AI", false);

	rend::DebugFilter::ToggleFilter( gameFilters, "MuppetResimulationWarning", true );
	
#ifndef RED_CONFIGURATION_FINAL
	rend::DebugFilter::ToggleFilter( gameFilters, "Rendering/Debug/GI/MissingTetraIndicator", true );
#endif // RED_CONFIGURATION_FINAL

	rend::DebugFilter::ToggleFilter( gameFilters, "GameSystem", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "GameSystem/QuestsSystem", true );
	rend::DebugFilter::ToggleFilter( gameFilters, "GameSystem/QuestsSystem/PrefabLoading", false );
}

void CBaseEngine::InitializeCommonDefaultDebugFilters( rend::DebugFilterMask& mask, rend::RenderFilterPreset preset )
{
	rend::ApplyRenderFilterMask( preset, mask );
}

void CBaseEngine::LoadGameDebugFilters()
{
	String gameMaskAsString;
	const char* gameFilterName = rend::GetGameFilterName();
	if ( Config::GetConfigSystem().GetValue( "DebugFilters", gameFilterName, gameMaskAsString ) && !gameMaskAsString.Empty() )
	{
		rend::DebugFilterMask::SetFilterMaskFromHierarchyString( gameFilterName, gameMaskAsString );
	}
}

//------

void CBaseEngine::Shutdown()
{
	ShutdownServices();

	if (m_soundSystem)
	{
		m_soundSystem->BeginShutdown();
	}

	if( m_inkSystem )
	{
		m_inkSystem->Shutdown();
		m_inkSystem.Reset();
	}

	// close connection
	if ( m_networkConnection )
	{
		m_networkConnection->Shutdown();
	}

	// Set the closing flag - this is a HACK to prevent some crap from happening during unload
	SetClosingMode();

	RED_LOG( "Engine: Shutting down engine" );

	if( m_soundSystem)
	{
		m_soundSystem->Shutdown();
		m_soundSystem = nullptr;
	}

	if ( m_localizationManager )
	{
		m_localizationManager->Shutdown();
		m_localizationManager = nullptr;
	}



#ifdef RED_NETWORK_ENABLED
	if( m_network )
	{
		// debug server
		RED_LOG( "Shutting down debug server.." );
		DBGSRV_CALL( ShutDown() );
		m_network->QueueShutdown();
	}
	// Stop Network communications

#endif // RED_NETWORK_ENABLED

	if (GResourceLoader)
	{
		GResourceLoader->ShutdownGameCacheManuallyBecauesRenderingCrashesOtherwise();
	}

	// Flush any render jobs that were added to the jobChain but not yet executed.
	if ( GRender )
	{
		GRender->Flush();
	}

	// Delete Bink videos. Must be after GRender Flush, so we know we aren't currently trying to update or release
	// anything already.
	if ( GRender )
	{
		GRender->ProcessRenderVideoDestroyQueue();
	}

	// Close the viewport manager
	m_viewportManager.Reset();

#ifndef NO_ASYNCHRONOUS_MATERIALS
	if ( GRender )
	{
		GRender->Flush();
	}
#endif // NO_ASYNCHRONOUS_MATERIALS

#ifndef NO_RUNTIME_MATERIAL_COMPILATION
	if ( m_asyncMaterialCompiler )
	{
		m_asyncMaterialCompiler->Shutdown();
	}
#endif // NO_RUNTIME_MATERIAL_COMPILATION

	if ( GRender )
	{
		GRender->Flush();
	}

	// Release references to gathered resources
	res::prv::ReleaseAllGatheredResources();

	anim::Uninitialize();

	physics::DestroyNatives();

	// #fixme: eventually need a proper flush instead, but will need some refactoring
	io::Shutdown();

#ifdef RED_NETWORK_ENABLED
	// Unregister debug pages
	m_debugPageRegisterer->UnregisterAllDebugPages();

	// Close debug page server
	CDebugPageServer::GetInstance().Shutdown();

	if( m_network )
	{
		if(m_network->IsInitialized())
		{
			// Ensure the network thread has shut down by the time this function has finished
			m_network->WaitForShutdown();
		}
		else
		{
			m_network->ImmediateShutdown();
		}
	}

#endif // RED_NETWORK_ENABLED

	ShutOle();

	// Shut down rendering
	DeleteRender();

#ifdef USE_SHADER_CACHE
	if ( GShaderCache )
	{
		GShaderCache->Shutdown();
	}
#endif // USE_SHADER_CACHE

	if ( GStaticShaderCache )
	{
#ifndef USE_SHADER_CACHE
# error How does GStaticShaderCache correspond to this macro?
#endif
		GStaticShaderCache->Shutdown();
	}

	// Cleanup final GPU resources
	UninitializeGpuMemoryPools();
}

#if defined( RED_PLATFORM_WINPC )
#include <combaseapi.h>
#endif

Bool CBaseEngine::InitOle()
{
#if defined( RED_PLATFORM_WINPC )
#if defined( NO_EDITOR )
	// Initialize COM for use by the current thread.
	// Needed for XAudio
	HRESULT hr = ::CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
	if( FAILED( hr ) )
	{
		HRESULT hr = ::CoInitializeEx( nullptr, COINIT_MULTITHREADED );
		if ( FAILED( hr ) )
		{
			return false;
		}
	}
#else
	::OleInitialize( nullptr );
#endif
#endif
	return true;
}

void CBaseEngine::ShutOle()
{
#if defined( RED_PLATFORM_WINPC )
#if defined( NO_EDITOR )
	::CoUninitialize();
#else
	::OleUninitialize();
#endif
#endif
}