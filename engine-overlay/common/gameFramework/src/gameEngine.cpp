#include "native-initialization-job-trace.h"
/**
 * Copyright (c) 2007-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include <emscripten.h>
#include <cstdio>
#include "gameEngine.h"
#include "fadeOutSupervisor.h"

#include "../../app/include/logging.h"
#include "../../redCore/include/commandline.h"
#include "../../redReflection/include/scriptingSystem.h"
#include "../../redSystem/include/loggerSink.h"
#include "../../redSystem/include/logMessage.h"
#include "../../engine/include/renderEngineViewport.h"
#include "../../gameServices/include/gameServices.h"
#include "../../gameServices/include/gameLocalStorage.h"
#include "../../videoPlayer/include/videoPlayer.h"
#include "../../input/include/inputSystem.h"
#include "../../input/include/inputContext.h"
#include "../../inkCore/include/inkSystem.h"
#include "../../renderData/include/renderFrameInfo.h"
#include "../../renderData/include/renderCommandHandler.h"
#include "../../game/include/gameFreeCamera.h"
#include "../../sound/include/soundSystem.h"
#include "../../commChannel/include/networkConnection.h"
#include "../../localization/include/localizationManager.h"
#include "../../redIO/include/redIOAsyncIO.h"
#include "../../redJobs2/include/jobBuilder.h"
#include "../../renderData/include/screenshotParameters.h"
#include "../../redMemory/include/reportUtils.h"
#include "../../engine/include/baseEngineInitParams.h"
#include "gameDefEnumeration.h"


#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
#include "../../../internal/loopbackCapture/include/loopbackRecorder.h"
#include "../../renderData/include/captureParameters.h"
#include "../../redConfig/include/configVarSystem.h"
#endif
#include "../../renderData/include/renderer.h"

#if defined( RED_SAVE_SERVER_ENABLED )
#include "saveServer.h"
#endif

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
#include <string>
#endif

using red::DynArray;

RTTI_BEGIN_TYPE( CGameEngine );
	RTTI_PARENT_TYPE( BaseGameEngine );
RTTI_END_TYPE();

#if !defined( NO_DEBUG_CONSOLE )
namespace helper
{
	static Bool JobDebuggerDump( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		job::DumpToLog();
		return true;
	}

	static Bool IOProfilerStart( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		io::GAsyncIO.ProfileStartTrace();
		return true;
	}

	static Bool IOProfilerStop( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		io::GAsyncIO.ProfileStopTrace();
		return true;
	}

	static Bool ForceCrash( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		int *p = nullptr;
		*p = 0xDEADBEEF;
		return true;
	}

	static Bool ForceAssert( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		ALWAYSENABLED_RED_FATAL_ASSERT( false, "Forced assert" );
		return true;
	}

	//
	// I've tried so hard to make it not optimize the recursion, this is the only way that seemed to work.
	// I hate myself for doing that, but if someone smarter comes by and figure out better way please change it.
	// ~eryk.dwornicki
	//
	RED_NOINLINE static void DoCrashRecur( int recurId );
	static std::function<void( int )> s_CallRecur = [](int recurId)
	{
		DoCrashRecur( recurId + 1 );
	};

	RED_NOINLINE static void DoCrashRecur( int recurId )
	{
		if( recurId >= 128 )
		{
			int *p = nullptr;
			*p = 0xDEADBEEF;
		}
		else
		{
			s_CallRecur( recurId );
		}
	}

	static Bool ForceCrashRecur( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		DoCrashRecur( 0 );
		return true;
	}

	static Bool ForceOom( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		for( Uint32 i = 0; i < 1024; ++i )
		{
			RED_ALLOCATE( red::PoolDebug, RED_GIGA_BYTE( 1 ) );
		}

		return true;
	}

	static Bool ForceHang( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		while( true )
		{
			red::SleepOnCurrentThread( 1000 );
		}

		return true;
	}

	static Bool ForceGpuHang( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		if ( GRender != nullptr )
		{
			GRender->RequestGpuHang();
		}

		return true;
	}

	static const char* GetResourcesReportPath()
	{
		red::DateTime time;
		red::Clock::GetInstance().GetLocalTime( time );

		constexpr Uint32 moduleFullPathSize = 260;
		static char moduleFullPath[ moduleFullPathSize ];

#if defined( RED_PLATFORM_WINPC )

		char moduleFileName[ MAX_PATH ];
		::GetModuleFileNameA( NULL, moduleFileName, MAX_PATH );
		::GetFullPathNameA( moduleFileName, MAX_PATH, moduleFullPath, NULL );

		// Strip EXE name
		char* pEnd = red::StrchrR( moduleFullPath, '\\' );
		if ( !pEnd )
		{
			return nullptr;
		}
		*pEnd = '\0';

		if ( !std::snprintf( pEnd, moduleFullPathSize - (Uint32)red::Strlen( moduleFullPath ), "\\resources_report_%04d-%02d-%02d_%02d.%02d.%02d.%03d.txt",
			time.GetYear(), time.GetMonth() + 1, time.GetDay() + 1,
			time.GetHour(), time.GetMinute(), time.GetSecond(), time.GetMilliSeconds() ) )
		{
			return nullptr;
		}

		return moduleFullPath;

#elif defined( RED_PLATFORM_DURANGO )
		if ( !std::snprintf( moduleFullPath, moduleFullPathSize, "d:\\resources_report_%04d-%02d-%02d_%02d.%02d.%02d.%03d.txt",
			time.GetYear(), time.GetMonth() + 1, time.GetDay() + 1,
			time.GetHour(), time.GetMinute(), time.GetSecond(), time.GetMilliSeconds() ) )
		{
			return nullptr;
		}

		return moduleFullPath;
#elif defined( RED_PLATFORM_ORBIS )
		if ( !std::snprintf( moduleFullPath, moduleFullPathSize, "/data/resources_report_%04d-%02d-%02d_%02d.%02d.%02d.%03d.txt",
			time.GetYear(), time.GetMonth() + 1, time.GetDay() + 1,
			time.GetHour(), time.GetMinute(), time.GetSecond(), time.GetMilliSeconds() ) )
		{
			return nullptr;
		}

		return moduleFullPath;
#elif defined( RED_PLATFORM_LINUX )
		if ( !std::snprintf( moduleFullPath, moduleFullPathSize, "/var/log/resources_report_%04d-%02d-%02d_%02d.%02d.%02d.%03d.txt",
			time.GetYear(), time.GetMonth() + 1, time.GetDay() + 1,
			time.GetHour(), time.GetMinute(), time.GetSecond(), time.GetMilliSeconds() ) )
		{
			return nullptr;
		}

		return moduleFullPath;
#else
#	error Unsupported platform
#endif
	}

	static Bool ReportResources( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		red::DynArray< res::ResourcePath > loadedResources{ red::PoolDebug() };
		GResourceLoader->EnumLoadedResources( loadedResources );
		std::sort( loadedResources.Begin(), loadedResources.End() );

		const char* outputFilePath = GetResourcesReportPath();
		if ( FILE* file = fopen( outputFilePath, "w" ) )
		{
			for ( const res::ResourcePath& path : loadedResources )
			{
				fprintf( file, "%s\n", path.ToDebugString() );
			}
			fclose( file );
		}

		return true;
	}

	static Bool MemoryReport( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		red::memory::LogFullReport();
		return true;
	}

	static Bool MemoryReportTxt( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		red::memory::LogFullReportToTxt();
		return true;
	}

	static Bool MemoryReportJson( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		red::memory::LogFullReportToJson();
		return true;
	}

	static Bool MemoryReportAutoInterval( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
#if !defined(RED_CONFIGURATION_FINAL) || defined( USE_PROFILER )
		if (parameters.Size() != 1)
			return false;
		red::memory::SetAutoDumpInterval( std::stof( parameters[0].AsChar() ));
#endif
		return true;
	}
	static Bool MemorySlabStomp( IDebugConsole* callingConsole, const DynArray< String >& parameters, SCommandInvokeResult& invokeResult, void* userData )
	{
		struct MemoryPointers
		{
			void* pointers[ 100 ];
		};

		auto memoryPointers = red::CreateSharedPtr< MemoryPointers, red::PoolDebug >();


		job::Builder builder{ job::Priority::Immediate };

		builder.DispatchParallelForJob( "memorySlabStompAllocate", { RED_ARRAY_COUNT_U32( MemoryPointers::pointers ) },
			[ memoryPointers ]( Uint32 index, const job::RunContext &context )
		{
			memoryPointers->pointers[ index ] = RED_ALLOCATE( red::memory::AcquireLocklessSlabAllocator(), sizeof( THandle< ISerializable > ) );
			red::SleepOnCurrentThread( 100 );
		} );

		builder.DispatchParallelForJob( "memorySlabStompFree", { RED_ARRAY_COUNT_U32( MemoryPointers::pointers ) },
			[ memoryPointers ]( Uint32 index, const job::RunContext &context )
		{
			RED_FREE( red::memory::AcquireLocklessSlabAllocator(), memoryPointers->pointers[ index ] );
			red::SleepOnCurrentThread( 100 );
		} );

		builder.DispatchJob( "memorySlabStomp",
			[ memoryPointers ]( const job::RunContext &context )
		{
			red::SleepOnCurrentThread( 100 );
			for( Uint32 index = 0; index < RED_ARRAY_COUNT_U32( MemoryPointers::pointers ); ++index )
			{
				*reinterpret_cast< void** >( memoryPointers->pointers[ index ] ) = nullptr;
			}
		} );

		auto counter = builder.ExtractWaitCounter();
		job::FlushCounter( counter );

		return true;
	}
}
#endif

CGameEngine::CGameEngine()
	: m_freeCameraEnabled( false )
	, m_fadeOutSupervisor( red::CreateUniquePtr< gsm::FadeOutSupervisor > () )
{
}

CGameEngine::~CGameEngine()
{
}

#if !defined( NO_DEBUG_CONSOLE )
const CGameEngine::NativeFunctionEntry CGameEngine::s_nativeFunctions[] = {
	{ "job.debugger.dump", "Dump job execution", &helper::JobDebuggerDump},
	{ "io.profiler.start", "Start IO profiler", &helper::IOProfilerStart },
	{ "io.profiler.stop", "Stop IO profiler", &helper::IOProfilerStop },
	{ "engine.forceCrashRecur", "Forces crash that is at least 128 stack entries long", &helper::ForceCrashRecur },
	{ "engine.forceCrash", "Forces nullptr access crash", &helper::ForceCrash },
	{ "engine.forceAssert", "Forces assertion crash", &helper::ForceAssert },
	{ "engine.forceOom", "Forces OOM crash", &helper::ForceOom },
	{ "engine.forceHang", "Forces hang (watchdog crash)", &helper::ForceHang },
	{ "red.memory.report.log", "Log memory report", &helper::MemoryReport },
	{ "red.memory.report.txt", "Write memory report to text file", &helper::MemoryReportTxt }, 
	{ "red.memory.report.json", "Write memory report to json file", &helper::MemoryReportJson },
	{ "red.memory.report.auto", "Write memory report to json at intervals", &helper::MemoryReportAutoInterval },
	{ "red.memory.slab.stomp", "Forces nullptr stomp in Slab allocator", &helper::MemorySlabStomp },
	{ "engine.reportResources", "Report all loaded resources", &helper::ReportResources },
	{ "render.forceGpuHang", "Tries to get the GPU to crash", &helper::ForceGpuHang },
};
#endif

Bool CGameEngine::OnRootSystemInitialization( const EngineInitParams& params )
{
	if ( !BaseGameEngine::OnRootSystemInitialization( params ) )
	{
		return false;
	}

	audio::SoundSystem::PreInit( SoundSystemConfig::GameConfig() );

#ifdef RED_MEMORY_ENABLE_REPORT_EXTRAS
	red::memory::DebugSetCmdLine(red::CommandLine::Get().Internal_GetRawCommandLine().AsChar());
#endif

	if( !InitializeSoundSystem() )
	{
		ALWAYSENABLED_RED_FATAL( "CGameEngine: Failed to initialize sound system" );
		return false;
	}

#ifdef RED_PLATFORM_WINPC
	// On WinPC load the local storage asap so we have it ready for example when starting
	// game from gamedef. It will be loaded again if main menu is open but thats not a big problem.
	// This solution is not perfect as the loading is asynchronous and it may in theory need better
	// solution if we will have steam/gog cloud support for these. With Null game services loading
	// should take few frames at most (request processing) and loading is synchronous fopen+fread pretty much
	// so hopefully it will not be a big issue.
	//
	LoadGameLocalStorage();
#endif

	if( !InitializeInput() )
	{
		ALWAYSENABLED_RED_FATAL( "CGameEngine: Failed to initialize input" );
		return false;
	}

	if ( GetLocalizationManager() )
	{
		auto& locManager = *GetLocalizationManager();
		locManager.Init();

		const red::StringView languageCodeName = GGameServices->GetLanguageCodeName();
		if ( !languageCodeName.Empty() )
		{
			locManager.SetLanguageGlobal( locManager.ResolveClosestLanguage( languageCodeName.ToString() ) );
		}

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
		auto commandLine = red::CommandLine::Get();
		String param;
		if ( commandLine.GetFirstOption( "language", param ) )
		{
			locManager.SetLanguageGlobal( locManager.ResolveClosestLanguage( param ) );
		}
		if ( commandLine.GetFirstOption( "voLanguage", param ) )
		{
			locManager.SetVoiceLanguage( locManager.ResolveClosestLanguage( param ) );
		}
		if ( commandLine.GetFirstOption( "subtitleLanguage", param ) )
		{
			locManager.SetSubtitleLanguage( locManager.ResolveClosestLanguage( param ) );
		}
		if ( commandLine.GetFirstOption( "textLanguage", param ) )
		{
			locManager.SetTextLanguage( locManager.ResolveClosestLanguage( param ) );
		}
#endif
	}

	if( m_soundSystem )
	{
		m_soundSystem->FinalizeInit( GetLocalizationManager() );
		m_inkSystem->SetSoundSystem( GetSoundSystem() );
	}

	// create game viewport
	if ( !CreateViewport( params ) )
	{
		ALWAYSENABLED_RED_FATAL( "CGameEngine: Failed to initialize viewport" );
		return false;
	}

	if( !InitializeGOGRewardsSystem() )
	{
		ALWAYSENABLED_RED_FATAL( "CGameEngine: Failed to initialize GOG rewards system" );
		return false;
	}

	m_viewport->AttachViewportSink( m_inputSystem.Get() );
	m_inputSystem->OnAttach( reinterpret_cast<void*>( m_viewport->GetViewportID() ) );
#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
	m_viewport->SetCaptureParameters( m_captureParameters );
#endif

	m_videoPlayer = VideoPlayer::CreateVideoPlayerInstance();
	m_fadeOutSupervisor->SetTimeDilationController( GetTimeDilationController() );
	m_fadeOutSupervisor->SetInkSystem( &GetInkSystem() );
	m_fadeOutSupervisor->SetSoundSystem( GetSoundSystem() );

#ifdef RED_USE_IMGUI
	m_viewport->ImGuiInitialize();
#ifndef RED_CONFIGURATION_FINAL
	m_viewport->ImGuiSetDefaultQAPreset( params.m_defaultQAPreset );
	m_viewport->ImGuiRequestDebugOverlay( params.m_defaultDebugOverlay );
	m_viewport->ImGuiRequestDefaultWindows( { params.m_defaultRequestedWindows } );
#endif
#endif

	return true;
}

void CGameEngine::OnRegularSystemInitialization( const EngineInitParams& params, const job::RunContext& context, red::Atomic< Uint32 >& keepRunning )
{
	job::Builder builder{ context };
	builder.DispatchJob( "CGameEngine/Initialization/BaseEngineRegularSystemInitialization", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/BaseEngineRegularSystemInitialization"};
		BaseGameEngine::OnRegularSystemInitialization( params, context, keepRunning );
	} );

	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/FreeCamera", [this, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/FreeCamera"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// create free camera
		m_freeCamera = red::CreateUniquePtr< game::GameFreeCamera >();
		m_freeCameraEnabled = false;
	} );

	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/RegisterNativeFunctions", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/RegisterNativeFunctions"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

#if !defined( NO_DEBUG_CONSOLE )
		for ( Uint32 i = 0; i < RED_ARRAY_COUNT_U32( s_nativeFunctions ); ++i )
		{
			const auto& entry = s_nativeFunctions[i];
			RED_FATAL_ASSERT( entry.functionName );
			RED_FATAL_ASSERT( entry.description );
			RED_FATAL_ASSERT( entry.funcPtr );
			GetNativeFunRegistry().RegisterNativeFunction( entry.functionName, entry.description, entry.funcPtr, nullptr );
		}
#endif
	} );

	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/ScriptsDebugger", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/ScriptsDebugger"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		IScriptingSystem::GetInstance().InitializeChannel( m_networkConnection->GetChannelFactory() );
	} );

	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/GameFramework", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/GameFramework"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// create game framework
		gsm::InitParams gsmParams{ IsTestEngine(), false, !params.m_automatorID.Empty() };
		m_gameFramework.Reset( gsm::IGameFramework::Create( red::CommandLine::Get(), gsmParams, context, keepRunning ) );
	} );

	builder.DispatchJob< job::Fence::None >( "GameEngine/Initialization/EnumerateGameDefs", [this, &keepRunning, params]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"GameEngine/Initialization/EnumerateGameDefs"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		job::Builder builder{ context };
		// make sure all worlds are enumerated for network and other users
		gsm::GameDefEnumerator::GetInstance().EnumerateGameDefs( builder );
	} );

	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/InkSystem", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/InkSystem"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		if ( m_inkSystem )
		{
			m_inkSystem->SetMode( ink::InkSystemMode::Game );
		}
	} );

#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/CinamaticScreenshotCapture", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/CinamaticScreenshotCapture"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		if ( red::SharedPtr< interop::utils::Object > interopRoot = GetInteropRoot() )
		{
			interopRoot->RegisterObject( "recorder", m_recorderInterop.CreateInterop() );
		}
	} );
#endif

	builder.DispatchFenceExplicitly();

#if defined( RED_SAVE_SERVER_ENABLED )
	builder.DispatchJob( "CGameEngine/Initialization/SaveServer", [this, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/SaveServer"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		if ( !red::SaveServer::GetInstance().Initialize() )
		{
			RED_LOG_ERROR( "CGameEngine: Failed to initialize save server" );
		}
	} );
#endif

#ifdef USE_RESOURCE_METRICS_BANK
	builder.DispatchJob< job::Fence::None >( "CGameEngine/Initialization/CreateSnaphot", [this, params, &keepRunning]( const job::RunContext& context )
	{
		cp2077::web::NativeInitializationJobTrace cp2077Trace{"CGameEngine/Initialization/CreateSnaphot"};
		if ( !keepRunning.GetValue() )
		{
			return;
		}

		// Snapshot of what was just loaded
		if ( GResourceLoader->GetResourceMetricsBank() )
		{
			GResourceLoader->GetResourceMetricsBank()->NextLoadingRequestSnapshot( "GameEngine" );
		}
	} );
#endif

	builder.DispatchFenceExplicitly();
}

void CGameEngine::Shutdown()
{
#if defined( RED_SAVE_SERVER_ENABLED )
	red::SaveServer::GetInstance().Shutdown();
#endif

	// close the game state machine
	m_gameFramework.Reset();

	// delete video player
	m_videoPlayer.Reset();

	// close the rendering viewport
	m_viewport.Reset();

	// close generic free camera
	m_freeCamera.Reset();

	m_fadeOutSupervisor->SetTimeDilationController( nullptr );
	m_fadeOutSupervisor->SetInkSystem( nullptr );
	m_fadeOutSupervisor->SetSoundSystem( nullptr );

	IScriptingSystem::GetInstance().DeinitializeChannel();

	ShutdownGOGRewardsSystem();

#if !defined( NO_DEBUG_CONSOLE )
	// --- CR4GameApp::Deinit()
	for ( Uint32 i = 0; i < RED_ARRAY_COUNT_U32( s_nativeFunctions ); ++i )
	{
		const auto& entry = s_nativeFunctions[ i ];
		GetNativeFunRegistry().UnregisterNativeFunction( entry.functionName );
	}
	// --- CR4GameApp::Deinit()
#endif

	// continue with basic shutdown
	BaseGameEngine::Shutdown();
}

void CGameEngine::OnSuspend()
{
	BaseGameEngine::OnSuspend();
}

void CGameEngine::OnResume()
{
	BaseGameEngine::OnResume();

	GGameServices->OnExitSuspendedState();
}

void CGameEngine::SubmitStalledFrameForDebug(const rend::StalledFrameParams& params)
{
	// start rendering
	CRenderFrameInfo frameInfo;
	CRenderCameraFrameInfo cameraInfo;
	m_viewport->BeginFrame(frameInfo, cameraInfo);
	{
		const Float engineTime = GetRawEngineTime();
		frameInfo.SetEngineTime(engineTime);
		frameInfo.SetCleanEngineTime(engineTime);
		frameInfo.m_stalledFrameForDebug = true;
	}

	GRender->GetCommands()->FlushPreviousFrameCommandsProcessing();

	auto& dd = cameraInfo.GetDebugDrawer();

	rend::helper::DrawLoadingMetrics(dd, params);

	m_viewport->SubmitFrame(frameInfo, cameraInfo);

	GRender->GetCommands()->FlushPreviousFrameCommandsProcessing();
}

void CGameEngine::ProcessInputInternal( input::IInputContext& context )
{
	m_viewport->ProcessInputInternal( context );
}

Bool CGameEngine::ProcessFreeCamera( gsm::Frame& frame )
{
#ifndef RED_CONFIGURATION_FINAL
	// enable/disable free camera
	for( auto& it : frame.m_input.GetBufferedInputData() )
	{
		if ( it.m_type == input::BufferedInputEvent::Type::Raw && it.m_raw.m_key == IK_F1 && it.m_raw.m_action == IACT_Press )
		{
			m_freeCameraEnabled = !m_freeCameraEnabled;

			if ( m_freeCameraEnabled ) // just enabled
			{
				// if we have valid game POVs use them to reset the free camerae
				const Vector3* initialPosition = &m_lastGamePOVPosition;
				const EulerAngles* initialRotation = &m_lastGamePOVRotation;

				// place the camera on top of the last valid game POV
				m_freeCamera->Reset( initialPosition, initialRotation );
			}

			it.Consume();
		}
	}

	// send rest of the input to camera (it may eat some)
	if ( m_freeCameraEnabled )
	{
		// eat additional input
		m_freeCamera->ProcessInput( frame.m_input );

		// update the free camera
		m_freeCamera->Tick( frame.m_timeDelta );

		// compute the camera position
		game::GameCamera gameCamera;
		m_freeCamera->CalculateCamera( gameCamera );

		// convert the setup to rendering camera
		const Float fov = (gameCamera.m_requestedFov > 0.0f) ? gameCamera.m_requestedFov : 75.0f; // engine fov
		const Float aspectRatio = (gameCamera.m_requestedAspect > 0.0f) ? gameCamera.m_requestedAspect : m_viewport->GetAspectRatio();

		CRenderCamera::CreationContext context;
		{
			context.position = gameCamera.m_position;
			context.rotation = gameCamera.m_rotation;
			context.fov = fov;
			context.aspect = aspectRatio;
			context.nearPlane = 0.1f;
			context.farPlane = 2000.f;
		}
		frame.m_freeCamera.Reset( RED_NEW( CRenderCamera )( context ) );

		// render free camera overlay
		static rend::DebugFilter DF_FreeCamera( "Overlay/FreeCamera" );

		rend::IDebugDrawer& dd = frame.m_cameraFrameInfo->GetDebugDrawer();
		if ( dd.GetMask().Test( DF_FreeCamera ) )
		{
			const Int32 x = 50;
			const Int32 y = 100;
			m_freeCamera->RenderDebug( dd, x, y );
		}
	}
#endif
	return m_freeCameraEnabled;
}

Bool CGameEngine::InitializeServices()
{
#if defined( RED_PLATFORM_WEB )
    CName servicesClassName = RED_NAME_CONSTEXPR("servicesGameServicesWeb");
#elif defined( RED_PLATFORM_LINUX )
	return true;
#elif defined( RED_PLATFORM_WINPC )
	CName servicesClassName = RED_NAME_CONSTEXPR( "servicesGameServicesWin" );
#elif defined( RED_PLATFORM_ORBIS )
	CName servicesClassName = RED_NAME_CONSTEXPR( "servicesGameServicesOrbis" );
#elif defined( RED_PLATFORM_DURANGO )
	CName servicesClassName = RED_NAME_CONSTEXPR( "servicesGameServicesDurango" );
#else
#error "Game services are not available on current platform"
#endif

#ifndef RED_PLATFORM_LINUX
	RED_LOG_INFO( "Core: Game services class: '%hs'", servicesClassName.AsChar() );

	const auto* servicesClass = GetRttiSystem().FindClass( servicesClassName );
	if ( servicesClass )
	{
		m_services = red::UniquePtr< services::GameServices >( servicesClass->CreateObject< services::GameServices >() );
		if ( m_services->Initialize( red::AppType::Game ) )
		{
			GGameServices = m_services.Get();
			return true;
		}
	}

	RED_LOG_ERROR( "Core: Unable to create game services" );
	return false;
#endif
}

void CGameEngine::LoadResourcesForGameServices( const job::RunContext& context )
{
#if defined( RED_PLATFORM_ORBIS )
		if ( GGameServices )
		{
			GGameServices->LoadResourcesForGameServices();
		}
#endif
}

void CGameEngine::ShutdownServices()
{
	if ( m_services )
	{
		m_services->Shutdown();
		GGameServices = nullptr;
	}
}

void CGameEngine::LoadGameLocalStorage()
{
	services::GGameLocalStorage->RequestLoad( []( const services::GameLocalStorageLoadResult result )
	{
		if( result == services::GameLocalStorageLoadResult::Success )
		{
			RED_LOG_TRACE( "[CGameEngine] Local storage has been loaded!" );
		}
		else
		{
			RED_LOG_WARNING( "[CGameEngine] Local storage failed to load!" );
		}
	} );
}


Bool CGameEngine::InitializeInput()
{
#if defined RED_PLATFORM_WEB
    CName class_name = RED_NAME_CONSTEXPR("inputInputSystemWeb");
#elif defined RED_PLATFORM_WINPC
	CName class_name = RED_NAME_CONSTEXPR("inputInputSystemWin32Game");
#elif defined RED_PLATFORM_ORBIS
	CName class_name = RED_NAME_CONSTEXPR("inputInputSystemOrbis");
#elif defined RED_PLATFORM_DURANGO
	CName class_name = RED_NAME_CONSTEXPR("inputInputSystemDurango");
#elif defined RED_PLATFORM_LINUX
	// TODO this is the engine for clients, no need for input right now
	CName class_name = RED_NAME_CONSTEXPR( "inputInputSystemNullInterface" );
#endif
	return TBaseClass::InitializeInput( class_name );
}

Bool CGameEngine::ProcessBaseFrame( const Float scaledEngineTimeDelta, const tick::Info& tickInfo, input::IInputContext& input )
{
	Bool result = true;

	Float frameDeltaTime = scaledEngineTimeDelta;

#ifndef RED_CONFIGURATION_FINAL
	if (IsGameMode())
	{
		res::SetDebugWaitUntilLoadedRenderCallback([this](const res::ResourcePath& waitingForResource, Float waitingSeconds) {	
			rend::StalledFrameParams params;
			params.waitingForResource = waitingForResource;
			params.waitingSeconds = waitingSeconds;
			SubmitStalledFrameForDebug(params);
		});
	}
#endif

	result = TBaseClass::ProcessFrame( frameDeltaTime, tickInfo, input );

#ifdef RED_USE_IMGUI
	m_viewport->ImGuiNewFrame( tickInfo.m_realTimeDelta );
#endif

	// start rendering
	CRenderFrameInfo frameInfo;
	CRenderCameraFrameInfo cameraInfo;
	m_viewport->BeginFrame( frameInfo, cameraInfo );
	{
		const Float engineTime = GetRawEngineTime();
		frameInfo.SetEngineTime( engineTime );
		frameInfo.SetCleanEngineTime( engineTime );
	}

#ifdef RED_USE_IMGUI
	m_viewport->ImGuizmoUpdate( cameraInfo.GetRenderCamera() );
#endif

	// handle debug update of the master viewport
	m_viewport->ProcessDebugInput( input );
	m_viewport->UpdateDebug( tickInfo.m_realTimeDelta ); // don't cheat the UI with scaled timeDelta

	m_services->Update();
	UpdateGOGRewardsSystem( tickInfo.m_realTimeDelta );

	// setup game frame
	gsm::Frame frame =
	{
		frameDeltaTime,
		tickInfo,
		input,
		&frameInfo,
		&cameraInfo
	};

	// get the frame messages from input
	m_inputSystem->GetMessages( frame.m_messages );

	// push all messages from services into the frame and clear services message queue
	m_services->GetMessages( frame.m_messages );


	// process the free camera
	ProcessFreeCamera( frame );

	// process the video player
	m_videoPlayer->Tick();

	GRender->GetCommands()->FlushPreviousFrameCommandsProcessing();

	red::DynArray<TRenderPtr<IRenderScene>> scenesForUpdate{ red::PoolFrame() };
	GatherScenesForUpdate( &scenesForUpdate, frameInfo );
	GRender->GetCommands()->FrameTick( scenesForUpdate );

	GetInkSystem().SimpleRender( frameInfo );

#ifdef RED_USE_IMGUI
	m_viewport->DrawImGui( frameInfo.m_systemOnRenderDebug, cameraInfo.GetDebugDrawer() );
	m_viewport->ImGuiEndFrame( frameDeltaTime );
#endif

	// Render collected frame into the master viewport
	// #tbd: not ideal above that we wait for the tick to finish completely before submitting the frame
	m_viewport->SubmitFrame( frameInfo, cameraInfo );

	return result;
}

red::DynArray<red::UniquePtr<services::GameServicesGalaxy>> x( red::PoolEngine() );

// [skperf] round 20 critical-path probe (src/render-node-cost-web.inl; ?perfprobe=1 with probeMask bit 2).
extern "C" void cp2077_skperf_crit_dispatched( double now );
extern "C" int cp2077_skperf_crit_read( double* out );
extern "C" void cp2077_skperf_crit_read_submits( double* out );
extern "C" void cp2077_skperf_crit_tail( double from, double to, double* out, const char** longest );
extern "C" void cp2077_skperf_crit_zerotail( double from, double to, double* out, const char** lastName );
extern "C" void cp2077_skperf_crit_read_scan_parts( double* out );
extern "C" void cp2077_skperf_crit_read_flushloop( double* out );
extern "C" void cp2077_skperf_crit_read_core_parts( double* out ); // [skperf] round 26
extern "C" void cp2077_skperf_crit_read_flushloop2( double* out ); // [skperf] round 27
namespace cpSkperfCritPrint
{
	// Sums over ~2 s (game thread only): wait = FlushPreviousFrameCommandsProcessing; dispatch->first node,
	// first->last node (render-graph node span), last node->wait end (render work after the last node: submit,
	// small jobs, handoffs), dispatch->wait end (whole previous-frame render as seen by the game thread).
	double s_wait = 0, s_toFirst = 0, s_span = 0, s_tail = 0, s_total = 0, s_gsm = 0, s_windowStart = 0, s_present = 0;
	double s_subN = 0, s_subWall = 0, s_subOrder = 0, s_subScan = 0, s_nodeToSub = 0, s_subToWait = 0; unsigned s_subFrames = 0;
	double s_tailJobs = 0, s_tailBusy = 0, s_lastJobToWait = 0, s_tailLongest = 0; const char* s_tailLongestName = "-"; unsigned s_tailFrames = 0;
	double s_zeroCount = 0, s_zeroNonJob = 0, s_lastZeroToWait = 0, s_lastZeroFromJob = 0; const char* s_lastZeroName = "-"; unsigned s_zeroFrames = 0;
	double s_scanIds = 0, s_scanSearch = 0, s_scanFlush = 0;
	double s_core[ 7 ] = {};
	double s_loopTimeouts = 0, s_loopTimedOutMax = 0, s_loopWokenMax = 0, s_loopMissed = 0;
	double s_loopIters = 0, s_loopSleeps = 0, s_loopSleepMs = 0, s_loopSleepMax = 0, s_loopOwnJobs = 0, s_loopRequeues = 0, s_loopExitToReturn = 0, s_lastZeroToLoopExit = 0; unsigned s_loopFrames = 0;
	unsigned s_frames = 0, s_noNode = 0;
	void Note( double gsmMs, double waitStart, double waitEnd )
	{
		double v[ 4 ];
		if ( !cp2077_skperf_crit_read( v ) || v[ 0 ] <= 0 ) return;
		s_present += v[ 3 ];
		{
			double sp[ 3 ]; cp2077_skperf_crit_read_scan_parts( sp ); s_scanIds += sp[ 0 ]; s_scanSearch += sp[ 1 ]; s_scanFlush += sp[ 2 ];
			double cp[ 7 ]; cp2077_skperf_crit_read_core_parts( cp ); for ( int i = 0; i < 7; ++i ) s_core[ i ] += cp[ i ];
			double fl[ 8 ]; cp2077_skperf_crit_read_flushloop( fl );
			if ( fl[ 0 ] >= waitStart && fl[ 0 ] <= waitEnd + 1.0 )
			{
				s_loopIters += fl[ 1 ]; s_loopSleeps += fl[ 2 ]; s_loopSleepMs += fl[ 3 ]; if ( fl[ 4 ] > s_loopSleepMax ) s_loopSleepMax = fl[ 4 ];
				s_loopOwnJobs += fl[ 5 ]; s_loopRequeues += fl[ 6 ]; s_loopExitToReturn += waitEnd - fl[ 0 ]; ++s_loopFrames;
				double f2[ 4 ]; cp2077_skperf_crit_read_flushloop2( f2 );
				s_loopMissed += f2[ 3 ];
				s_loopTimeouts += f2[ 0 ]; if ( f2[ 1 ] > s_loopTimedOutMax ) s_loopTimedOutMax = f2[ 1 ]; if ( f2[ 2 ] > s_loopWokenMax ) s_loopWokenMax = f2[ 2 ];
			}
		}
		{
			double sb[ 5 ];
			cp2077_skperf_crit_read_submits( sb );
			s_subN += sb[ 0 ]; s_subWall += sb[ 1 ]; s_subOrder += sb[ 2 ]; s_subScan += sb[ 3 ];
			if ( sb[ 4 ] > v[ 0 ] && v[ 2 ] > 0 ) { s_nodeToSub += sb[ 4 ] - v[ 2 ]; s_subToWait += waitEnd - sb[ 4 ]; ++s_subFrames; }
			if ( sb[ 4 ] > v[ 0 ] )
			{
				// [skperf] round 23: jobs that ran between the last submission and the end of the wait
				double t[ 4 ]; const char* longest = "-";
				cp2077_skperf_crit_tail( sb[ 4 ], waitEnd, t, &longest );
				s_tailJobs += t[ 0 ]; s_tailBusy += t[ 1 ]; if ( t[ 2 ] > 0 ) s_lastJobToWait += waitEnd - t[ 2 ];
				if ( t[ 3 ] > s_tailLongest ) { s_tailLongest = t[ 3 ]; s_tailLongestName = longest; }
				++s_tailFrames;
				// [skperf] round 24: counters that reached zero in the tail, and the last one before the wait ended
				double z[ 4 ]; const char* lastName = "-";
				cp2077_skperf_crit_zerotail( sb[ 4 ], waitEnd, z, &lastName );
				s_zeroCount += z[ 0 ]; s_zeroNonJob += z[ 1 ];
				if ( z[ 2 ] > 0 ) { s_lastZeroToWait += waitEnd - z[ 2 ]; s_lastZeroFromJob += z[ 3 ]; s_lastZeroName = lastName; ++s_zeroFrames; }
				{ double fl[ 8 ]; cp2077_skperf_crit_read_flushloop( fl ); if ( z[ 2 ] > 0 && fl[ 0 ] >= z[ 2 ] ) s_lastZeroToLoopExit += fl[ 0 ] - z[ 2 ]; }
			}
		}
		s_gsm += gsmMs; s_wait += waitEnd - waitStart; s_total += waitEnd - v[ 0 ]; ++s_frames;
		if ( v[ 1 ] > 0 && v[ 2 ] >= v[ 1 ] ) { s_toFirst += v[ 1 ] - v[ 0 ]; s_span += v[ 2 ] - v[ 1 ]; s_tail += waitEnd - v[ 2 ]; }
		else ++s_noNode;
		if ( s_windowStart == 0 ) s_windowStart = waitEnd;
		if ( waitEnd - s_windowStart < 2000 || !s_frames ) return;
		const double n = s_frames, m = ( s_frames > s_noNode ) ? double( s_frames - s_noNode ) : 1.0;
		std::fprintf( stderr, "CP2077 skperf critpath probe: frames=%u gsmMs=%.1f waitMs=%.1f renderSinceDispatchMs=%.1f dispatchToFirstNodeMs=%.1f nodeSpanMs=%.1f lastNodeToWaitEndMs=%.1f presentWaitMs=%.1f noNodeFrames=%u | submitsPerFrame=%.1f submitWallMs=%.1f submitOrderWaitMs=%.1f mappedScanMs=%.1f lastNodeToLastSubmitEndMs=%.1f lastSubmitEndToWaitEndMs=%.1f | tail: jobsPerFrame=%.1f busyMs=%.1f lastJobEndToWaitEndMs=%.1f longestJob=%.40s %.1fms | zero: countersPerFrame=%.1f nonJobPerFrame=%.1f lastZeroToWaitEndMs=%.2f lastZeroFromJob=%.2f lastZero=%.48s | scanParts: idsMs=%.2f searchMs=%.2f flushMs=%.2f (core lock=%.2f collect=%.2f diff=%.2f commit=%.2f release=%.2f bytesKB=%.1f patches=%.1f) | waitLoop: frames=%u iters=%.0f sleeps=%.0f sleepMs=%.2f sleepMaxMs=%.2f ownJobsMs=%.2f requeues=%.1f lastZeroToLoopExitMs=%.2f loopExitToReturnMs=%.2f timeoutsPerFrame=%.2f timedOutMaxMs=%.2f wokenMaxMs=%.2f missedNotifyPerFrame=%.2f\n",
			s_frames, s_gsm / n, s_wait / n, s_total / n, s_toFirst / m, s_span / m, s_tail / m, s_present / n, s_noNode,
			s_subN / n, s_subWall / n, s_subOrder / n, s_subScan / n, s_subFrames ? s_nodeToSub / s_subFrames : 0.0, s_subFrames ? s_subToWait / s_subFrames : 0.0,
			s_tailFrames ? s_tailJobs / s_tailFrames : 0.0, s_tailFrames ? s_tailBusy / s_tailFrames : 0.0, s_tailFrames ? s_lastJobToWait / s_tailFrames : 0.0, s_tailLongestName, s_tailLongest,
			s_tailFrames ? s_zeroCount / s_tailFrames : 0.0, s_tailFrames ? s_zeroNonJob / s_tailFrames : 0.0,
			s_zeroFrames ? s_lastZeroToWait / s_zeroFrames : 0.0, s_zeroFrames ? s_lastZeroFromJob / s_zeroFrames : 0.0, s_lastZeroName,
			s_scanIds / n, s_scanSearch / n, s_scanFlush / n, s_core[ 0 ] / n, s_core[ 1 ] / n, s_core[ 2 ] / n, s_core[ 3 ] / n, s_core[ 4 ] / n, s_core[ 5 ] / 1024.0 / n, s_core[ 6 ] / n,
			s_loopFrames, s_loopFrames ? s_loopIters / s_loopFrames : 0.0, s_loopFrames ? s_loopSleeps / s_loopFrames : 0.0, s_loopFrames ? s_loopSleepMs / s_loopFrames : 0.0, s_loopSleepMax,
			s_loopFrames ? s_loopOwnJobs / s_loopFrames : 0.0, s_loopFrames ? s_loopRequeues / s_loopFrames : 0.0,
			s_zeroFrames ? s_lastZeroToLoopExit / s_zeroFrames : 0.0, s_loopFrames ? s_loopExitToReturn / s_loopFrames : 0.0,
			s_loopFrames ? s_loopTimeouts / s_loopFrames : 0.0, s_loopTimedOutMax, s_loopWokenMax, s_loopFrames ? s_loopMissed / s_loopFrames : 0.0 );
		s_wait = s_toFirst = s_span = s_tail = s_total = s_gsm = s_present = 0;
		s_subN = s_subWall = s_subOrder = s_subScan = s_nodeToSub = s_subToWait = 0; s_subFrames = 0;
		s_tailJobs = s_tailBusy = s_lastJobToWait = s_tailLongest = 0; s_tailLongestName = "-"; s_tailFrames = 0;
		s_zeroCount = s_zeroNonJob = s_lastZeroToWait = s_lastZeroFromJob = 0; s_lastZeroName = "-"; s_zeroFrames = 0;
		s_scanIds = s_scanSearch = s_scanFlush = 0; for ( int i = 0; i < 7; ++i ) s_core[ i ] = 0;
		s_loopTimeouts = s_loopTimedOutMax = s_loopWokenMax = s_loopMissed = 0;
		s_loopIters = s_loopSleeps = s_loopSleepMs = s_loopSleepMax = s_loopOwnJobs = s_loopRequeues = s_loopExitToReturn = s_lastZeroToLoopExit = 0; s_loopFrames = 0; s_frames = s_noNode = 0; s_windowStart = waitEnd;
	}
}

Bool CGameEngine::ProcessFrame( const Float scaledEngineTimeDelta, const tick::Info& tickInfo, input::IInputContext& input )
{
	PC_SCOPE( CGameEngine_ProcessFrame );
        static unsigned long long cpFrame=0;
        const bool cpTrace=++cpFrame<=4 || (cpFrame%16)==0;
        const double cpStart=emscripten_get_now();

	Float frameDeltaTime = scaledEngineTimeDelta;
	Bool result = true;

	result = TBaseClass::ProcessFrame( frameDeltaTime, tickInfo, input );
    const double cpAfterBase=emscripten_get_now();

#ifdef RED_USE_IMGUI
	m_viewport->ImGuiNewFrame( tickInfo.m_realTimeDelta );
#endif

#if defined( RED_SAVE_SERVER_ENABLED )
	red::SaveServer::GetInstance().Update( frameDeltaTime, input );
#endif

	// start rendering
	CRenderFrameInfo frameInfo;
	CRenderCameraFrameInfo cameraInfo;
	m_viewport->BeginFrame( frameInfo, cameraInfo );
    const double cpAfterBegin=emscripten_get_now();
	{
		const Float engineTime = GetRawEngineTime();
#ifdef RED_MEMORY_ENABLE_REPORT_EXTRAS
		red::memory::Debug_SetPlayTIme( engineTime );
#endif 
		frameInfo.SetEngineTime( engineTime );
		frameInfo.SetCleanEngineTime( engineTime );
	}

	// handle debug update of the master viewport
	m_viewport->ProcessDebugInput( input );
	m_viewport->UpdateDebug( tickInfo.m_realTimeDelta ); // don't cheat the UI with scaled timeDelta

	m_services->Update();
	UpdateGOGRewardsSystem( tickInfo.m_realTimeDelta );

	// setup game frame
	gsm::Frame frame =
	{
		frameDeltaTime,
		tickInfo,
		input,
		&frameInfo,
		&cameraInfo
	};

	// get the frame messages from input
	m_inputSystem->GetMessages(frame.m_messages);

	// push all messages from services into the frame and clear services message queue
	m_services->GetMessages(frame.m_messages);

	// Force pause for limited resource mode
	const EBaseEngineResourceMode resourceMode = GetResourceMode();
	if( resourceMode == BERM_FullWithSystemNoInput || resourceMode == BERM_Constrained )
	{
		frame.m_messages.PushBack( { RED_NAME_CONSTEXPR( "PauseBackground" ), {} } );
	}

	// process the free camera
	ProcessFreeCamera( frame );

	// process the video player
	const double cpBeforeVideo=emscripten_get_now();
    m_videoPlayer->Tick();
    const double cpAfterVideo=emscripten_get_now();

	m_fadeOutSupervisor->Tick( tickInfo.m_realTimeDelta );
#ifdef RED_USE_IMGUI
	m_fadeOutSupervisor->RenderDebug( cameraInfo.GetDebugDrawer() );
#endif

	// Tick the game state machine
	if ( !( result = m_gameFramework->ProcessFrame( frame ) ) )
	{
		// normal exit
		RED_LOG_INFO( "CGameEngine: ProcessFrame failed. Requesting for a game exit" );
		RequestExit( 0 );
	}

#ifdef RED_USE_IMGUI
	m_viewport->ImGuizmoUpdate( cameraInfo.GetRenderCamera() );
#endif

const double cpAfterGsm=emscripten_get_now();
	GRender->GetCommands()->FlushPreviousFrameCommandsProcessing();
    const double cpAfterFlush=emscripten_get_now();

	red::DynArray<TRenderPtr<IRenderScene>> scenesForUpdate{ red::PoolFrame() };
	GatherScenesForUpdate( &scenesForUpdate, frameInfo );
	cpSkperfCritPrint::Note( cpAfterGsm - cpAfterVideo, cpAfterGsm, cpAfterFlush ); // [skperf] round 20 (probe off: no-op)
	GRender->GetCommands()->FrameTick( scenesForUpdate );
    const double cpAfterFrameTick=emscripten_get_now();
	cp2077_skperf_crit_dispatched( cpAfterFrameTick ); // [skperf] round 20

	//we have no scene so we need to ask UI to render by itself
	if (scenesForUpdate.Size() == 0)
	{
		GetInkSystem().SimpleRender( frameInfo );
	}

#ifdef RED_USE_IMGUI
	m_viewport->DrawImGui( frameInfo.m_systemOnRenderDebug, cameraInfo.GetDebugDrawer() );
	m_viewport->ImGuiEndFrame( frameDeltaTime );
#endif

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
	FT_CollectStats( frameInfo, cameraInfo );
#endif

#ifdef RED_MEMORY_ENABLE_REPORT_EXTRAS
	red::memory::TryDoAutoMemReport(frameDeltaTime);
#endif

const double cpBeforeSubmit=emscripten_get_now();
	// Render collected frame into the master viewport
	// #tbd: not ideal above that we wait for the tick to finish completely before submitting the frame
	m_viewport->SubmitFrame( frameInfo, cameraInfo );
    const double cpAfterSubmit=emscripten_get_now();
    if(cpTrace || (cpAfterSubmit-cpStart)>50.0)std::fprintf(stderr,"CP2077 game engine phases probe: frame=%llu baseMs=%.3f beginFrameMs=%.3f servicesInputMs=%.3f videoMs=%.3f gsmFadeMs=%.3f previousCommandsMs=%.3f frameTickMs=%.3f renderUiMs=%.3f submitMs=%.3f totalMs=%.3f\n",
        cpFrame,cpAfterBase-cpStart,cpAfterBegin-cpAfterBase,cpBeforeVideo-cpAfterBegin,
        cpAfterVideo-cpBeforeVideo,cpAfterGsm-cpAfterVideo,cpAfterFlush-cpAfterGsm,
        cpAfterFrameTick-cpAfterFlush,cpBeforeSubmit-cpAfterFrameTick,
        cpAfterSubmit-cpBeforeSubmit,cpAfterSubmit-cpStart);

	return result;
}

IVideoPlayer* CGameEngine::GetVideoPlayer() const
{
	return m_videoPlayer.Get();
}

gsm::IFadeOutSupervisor* CGameEngine::GetFadeOutSupervisor() const
{
	return m_fadeOutSupervisor.Get();
}

void CGameEngine::TakeFullScreenshot( rend::ScreenshotMode mode, const String& fileName, rend::OnScreenshotCreatedCallback callback )
{
	if ( m_viewport )
	{
		// TODO: this can take much more parameters
		rend::SingleScreenShotData params;
		rend::dim::GetPreset( m_viewport->GetWidth(), m_viewport->GetHeight(), params.m_resolution );
		params.m_mode = mode;
		params.m_onCreatedCallback = callback;

		if (red::paths::IsAbsolutePath(fileName))
			params.m_outputPath = red::AbsolutePath::CreateFilePath(fileName);
		else
			params.m_outputPath.AppendFilePath( fileName );

		if (mode == rend::ScreenshotMode::HIGH_RESOLUTION || mode == rend::ScreenshotMode::HIGH_RESOLUTION_LAYERED)
			params.m_saveFormat = SF_EXR;
		else
			params.m_saveFormat = SF_PNG;

		m_viewport->TakeFullScreenshot( params );
	}
}

void CGameEngine::TakeFullScreenshot( const rend::SingleScreenShotData& screenshotParams, Bool useCustomResolution )
{
	if (m_viewport)
	{
		if (!useCustomResolution)
		{
			rend::SingleScreenShotData params = screenshotParams;
			rend::dim::GetPreset( m_viewport->GetWidth( ), m_viewport->GetHeight( ), params.m_resolution );

			m_viewport->TakeFullScreenshot( params );
		}
		else
		{
			m_viewport->TakeFullScreenshot( screenshotParams );
		}

	}
}

Bool CGameEngine::CanTakeFullScreenshot() const
{
	if( m_viewport )
	{
		return m_viewport->CanTakeFullScreenshot();
	}
	return false;
}

rend::EngineViewportPtr CGameEngine::GetEngineViewport()
{
	return m_viewport;
}

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
void CGameEngine::FT_CollectStats( CRenderFrameInfo& frameInfo, const CRenderCameraFrameInfo& cameraInfo )
{
}
#endif

#ifdef RED_CINEMATIC_SCREENSHOT_CAPTURE
void CGameEngine::ToggleScreenshotCapture()
{
	if ( m_viewport )
	{
		m_viewport->ToggleScreenshotCapture();
	}
}

void CGameEngine::SetCaptureParameters( const rend::CaptureParameters& parameters )
{
	m_captureParameters = parameters;

	if ( m_viewport )
	{
		m_viewport->SetCaptureParameters( parameters );
	}
}

Bool CGameEngine::GetCaptureParameters( rend::CaptureParameters& parameters ) const
{
	if ( m_viewport )
	{
		parameters = m_viewport->GetCaptureParameters();
		return true;
	}
	return false;
}

Bool CGameEngine::IsScreenshotCaptureOn() const
{
	if ( m_viewport )
	{
		return m_viewport->IsScreenshotCaptureOn();
	}

	return false;
}

void CGameEngine::StartScreenshotCapture( const Bool paused )
{
	Config::GetConfigSystem().SetValue( "AnimationSystem", "UseCulling", "0" );
	Config::GetConfigSystem().SetValue( "Developer/FeatureToggles", "MotionBlur", "0" );

	if ( m_viewport )
	{
		m_viewport->StartScreenshotCapture( paused );
		m_recorderInterop.NotifyCaptureStarted();
	}
}

void CGameEngine::StopScreenshotCapture()
{
	if ( m_viewport )
	{
		m_viewport->StopScreenshotCapture();
		m_recorderInterop.NotifyCaptureFinished();
	}
}
void CGameEngine::PauseScreenshotCapture()
{
	if ( m_viewport )
	{
		m_viewport->PauseScreenshotCapture();
	}
}

void CGameEngine::ResumeScreenshotCapture()
{
	if ( m_viewport )
	{
		m_viewport->ResumeScreenshotCapture();
	}
}

void CGameEngine::StartAudioCapture()
{
	if ( m_audioRecorder )
	{
		RED_LOG_ERROR( "" );
		return;
	}

	rend::CaptureParameters params;
	GetCaptureParameters( params );

	const red::AbsolutePath filePath = params.m_outputPath.AddFilePath( String::Printf( "%hs_%hs.wav", params.m_outputDirectoryName.AsChar(), params.m_outputDirectoryNameSuffix.AsChar() ) );
	m_audioRecorder = red::CreateUniquePtr< LoopbackAPI::LoopbackRecorder, red::PoolEngine >( filePath.AsChar(), filePath.Length() );
	m_audioRecorder->StartRecording();

	m_recorderInterop.NotifyCaptureStarted();
}

void CGameEngine::StopAudioCapture()
{
	if ( !m_audioRecorder )
	{
		RED_LOG_ERROR( "" );
		return;
	}

	m_audioRecorder->StopRecording();
	m_audioRecorder.Reset();

	m_recorderInterop.NotifyCaptureFinished();
}

void CGameEngine::RecorderInterop::SetupInterop( RecorderInterop* ptr, ::interop::utils::Object& op )
{
	op.RegisterNotifier( "onCaptureStarted", ptr->m_onCaptureStarted );
	op.RegisterNotifier( "onCaptureFinished", ptr->m_onCaptureFinished );
}

void CGameEngine::RecorderInterop::NotifyCaptureStarted()
{
	m_onCaptureStarted.Notify();
}

void CGameEngine::RecorderInterop::NotifyCaptureFinished()
{
	m_onCaptureFinished.Notify();
}

#endif
