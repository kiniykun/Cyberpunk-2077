#ifdef RED_PLATFORM_WEB
#include <cstdio>
#endif
/**
* Copyright (c) 2019 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include "gameAppBaseInitializationState.h"
#include "gameApp.h"
#include "baseEngine.h"
#include "../../app/include/logging.h"
#include "../../core/include/memoryBudgets.h"
#include "../../launcher/include/splashScreen.h"
#include "../../redCompression/include/compression.h"
#include "../../redConfig/include/configVar.h"
#include "../../redConfig/include/redConfigPool.h"
#include "../../redCore/include/commandline.h"
#include "../../redCore/include/profilerTypes.h"
#include "../../redFileSystem/include/fileSys.h"
#include "../../redIO/include/redIOAsyncIO.h"
#include "../../redImage/src/imageMemory.h"
#include "../../redJobs2/include/jobDispatcherInitParam.h"
#include "../../redJobs2/include/jobSystem.h"
#include "../../redMemory/include/memoryAnalyzerUtils.h"
#include "../../redNetwork/include/network.h"
#include "../../redReflection/include/util.h"
#include "../../renderData/include/renderSettingsVars.h"
#include "../../renderData/include/renderer.h"
#include "../../redHTTP/include/redHTTPPublic.h" // Only used in that file, no need to put it to PCH
#include "../../redHTTP/include/httpSystem.h"
#include "../../redConfig/include/inGameConfigSystem.h"

#if defined(RED_PLATFORM_DURANGO)
	#define CONSOLE_TYPE_XBOX_SERIES_S        ((CONSOLE_TYPE)5)
	#define CONSOLE_TYPE_XBOX_SERIES_X        ((CONSOLE_TYPE)6)
	#define CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT ((CONSOLE_TYPE)7)
#endif

namespace Config
{
	TConfigVar<String> cvDefaultEngine("General", "DefaultEngine", "");
}

namespace
{
	red::String GetLogDescription( const red::CommandLine& commandLine )
	{
		String logDescription;
		String engine = "DefaultClass";
		commandLine.GetFirstOption( "engine", engine );
		logDescription = String::Printf( "engine-%hs", engine.AsChar() );
		return logDescription;
	}

	red::String GetLogPrettyName( const red::CommandLine& commandLine )
	{
		String prettyName;
		String engine = "DefaultClass";
		commandLine.GetFirstOption( "engine", engine );
		if ( engine == "BackendGameEngine" )
		{
			prettyName = "Backend";
		}
		else
		{
#if defined( RED_PLATFORM_WINPC )
			prettyName = "Game(PC)";
#elif defined( RED_PLATFORM_DURANGO )
			prettyName = "Game(XboxOne)";
#elif defined( RED_PLATFORM_ORBIS )
			prettyName = "Game(PS4)";
#elif defined( RED_PLATFORM_LINUX )
			prettyName = "Game(Linux)";
#endif
		}
		return prettyName;
	}

	Uint64 GetCurrentFrameNumber()
	{
		return GEngine ? GEngine->GetCurrentEngineTick() : 0;
	}
}

namespace red
{
	GameAppBaseInitializationState::GameAppBaseInitializationState()
	{}

	GameAppBaseInitializationState::~GameAppBaseInitializationState()
	{}

	const char* GameAppBaseInitializationState::GetName() const
	{
		return "BaseInitialization";
	}

	Int32 GameAppBaseInitializationState::GetId() const
	{
		return GameAppState::GameAppState_BaseInitialization;
	}

	bool GameAppBaseInitializationState::OnEnterState( GameAppInstance& gameApp )
	{
		const red::CommandLine& commandLine = red::CommandLine::Get();

		// Initialize memory pools for modules without initialization
		InGameConfig::InitializeMemoryPools();
		img::memory::InitializeMemoryPools();
		compression::InitializeMemoryPools();

#if defined( RED_NETWORK_ENABLED )
		red::Network::InitializeMemoryPools();
		red::Network::Base::Initialize();
#endif
		//-----------------------

		const auto& processPrettyName = GetLogPrettyName( commandLine );
		app::logging::Initialize( commandLine, processPrettyName, gameApp.GetName(), GetLogDescription( commandLine ) );
		app::logging::SetFrameNumberRetriever( GetCurrentFrameNumber );

		//-----------------------

#if !defined( RED_CONFIGURATION_FINAL )
		{
			red::String commandLineToPrint = String_CreateExternal_OnStack( 4096 );
			commandLineToPrint = red::CommandLine::Internal_GetRawCommandLine();
			Uint32 GOGPasswordStartIndex = -1;
			const char GOGPasswordArgStart[] = "-GOGPassword \"";
			if ( commandLineToPrint.IndexOf( GOGPasswordArgStart, GOGPasswordStartIndex ) )
			{
				GOGPasswordStartIndex += sizeof( GOGPasswordArgStart ) - 1;
				Uint32 GOGPasswordEndIndex = -1;
				if ( commandLineToPrint.IndexOf( '\"', GOGPasswordEndIndex, GOGPasswordStartIndex ) )
				{
					red::Memset( &commandLineToPrint[ GOGPasswordStartIndex ], '*', GOGPasswordEndIndex - GOGPasswordStartIndex );
				}
			}
			RED_LOG_INFO( "cmd: %s", commandLineToPrint.AsChar() );
		}
#endif

		//-----------------------

		io::InitSetup ioSetup;
		if ( commandLine.HasOption( "profileio" ) )
		{
			ioSetup.enableProfiler = true;
		}

		if ( !io::Initialize( ioSetup ) )
		{
			RED_FATAL( "Failed to initialize redIO" );
			return true;
		}

		if ( ioSetup.enableProfiler )
		{
			io::GAsyncIO.ProfileStartTrace();
		}

		//-----------------------

		http::InitParams initParams;
		initParams.maxWorkers = 1;
		if( !http::Initialize( initParams ) )
		{
			RED_FATAL( "Failed to initialize redHTTP" );
			return false;
		}

		//-----------------------

		#ifndef RED_PLATFORM_WEB
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
#endif
        // WebAssembly has no mutable x86 floating-point control register.
		#ifndef RED_PLATFORM_WEB
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
        // WebAssembly has no mutable x86 floating-point control register.

		//-----------------------

		app::InitializationSettings appSettings;
		appSettings.ParseFromCommandline( commandLine );

		if ( appSettings.m_waitForDebugger )
		{
			while ( !dbgutils::IsDebuggerAttached() )
			{
				continue;
			}
		}

		// Don't care if debugger attached - maybe on purpose so get Windows dialog so you can attach a debugger this way
		// But of course check after the above prompt/wait for debugger setting.
		if ( appSettings.m_debugBreak )
		{
			RED_BREAKPOINT();
		}

		//-----------------------

#ifdef USE_PROFILER
		red::profiler::InitInGameProfiler();
#endif
 		PROFILER_InitThread( "Main", 16384 );

		job::InitParam jobInitParam;
		if( appSettings.m_jobThreadCount != -1 )
		{
			jobInitParam.maxThreads = appSettings.m_jobThreadCount;
		}
		job::Initialize( jobInitParam );

		//-----------------------

		#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: app initialization begin\n", stderr);
#endif
        if ( !gameApp.Initialize( appSettings ) )
		{
			return false;
		}

		#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: app initialization returned; setting memory budgets\n", stderr);
#endif
        red::SetMemoryBudgets();

		#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: base engine systems begin\n", stderr);
#endif
        InitializeBaseEngineSystems( gameApp, commandLine );
#ifdef RED_PLATFORM_WEB
        std::fputs("CP2077 native startup: base engine systems returned\n", stderr);
#endif
		return true;
	}

	bool GameAppBaseInitializationState::OnTick( GameAppInstance& gameApp )
	{
		if ( !GEngine )
		{
			// pre initialization failed
			gameApp.RequestNextStateTransition( GameAppState_Shutdown );
			return true;
		}

		Bool result = false;

		const EBaseEngineState state = GEngine->GetState();
#ifdef RED_PLATFORM_WEB
        static Int32 lastReportedState = -1;
        if (lastReportedState != static_cast<Int32>(state)) {
            lastReportedState = static_cast<Int32>(state);
            std::fprintf(stderr, "CP2077 native base engine state: %d\n", lastReportedState);
        }
#endif
		switch ( state )
		{
		case BES_Uninitialized:
			RED_LOG( "GameAppBaseInitializationState: Failed to initalize engine %hs", GEngine->GetClass()->GetName().AsChar() );
			gameApp.RequestNextStateTransition( GameAppState_Shutdown );
			result = true;
			break;

		case BES_InitializingRootSystems:
			red::YieldCurrentThread();
			break;

		case BES_InitializingRegularSystems:
			// finished initialization of the root systems, so we can start ticking simplified frame (renderer, audio, input and few others)
			gameApp.RequestNextStateTransition( GameAppState_Initialization );
			result = true;
			break;

		case BES_Initialized:
			gameApp.RequestNextStateTransition( GameAppState_GameRunning );
			break;

		default:
			RED_FATAL_ASSERT( 0 , "GameAppBaseInitializationState: Invalid engine state '%u'", state );
			break;
		}

		return result;
	}

	void GameAppBaseInitializationState::CreateEngineInitParams( const red::CommandLine& commandLine, EngineInitParams& params ) const
	{
		bool unattended = commandLine.HasOption( "unattended" );
		params.m_scriptsSilentCompilation = unattended;
		params.m_scriptsSilentValidation = unattended;
		params.m_tweakDBSilentValidation = unattended;
		params.m_gatheredResourceSilentValidation = unattended;
		params.m_tweakDBForceRaw = commandLine.HasOption( "forceRawTweakDB" );
		params.m_profilerEnabled = commandLine.HasOption( "profiler" );
		params.m_disableRayTracing = commandLine.HasOption( "disableRayTracing" );
		params.m_disableRayTracedReflection = commandLine.HasOption( "disableRayTracedReflection" );
		params.m_disableRayTracedTransparentReflection = commandLine.HasOption( "disableRayTracedTransparentReflection" );
		params.m_dlss = commandLine.HasOption( "dlss" );
		commandLine.GetFirstParam( "windowCaption", params.m_windowCaption );

		String scriptVersion;
		if ( commandLine.GetFirstOption( "scriptVersion", scriptVersion ) )
		{
			params.m_scriptVersion = script::ParseScriptVersion( scriptVersion );
		}

		params.m_scriptsProfilerMode = script::ParseProfilerMode( scriptVersion );

		String tweakdbBlobPath;
		if ( commandLine.GetFirstOption( "tweakdbBlobPath", tweakdbBlobPath ) )
		{
			params.m_tweakdbBlobPath = red::AbsolutePath::ParseDirPath( tweakdbBlobPath );
		}

		String scriptsBlobPath;
		if ( commandLine.GetFirstOption( "scriptsBlobPath", scriptsBlobPath ) )
		{
			params.m_scriptsBlobPath = red::AbsolutePath::ParseFilePath( scriptsBlobPath );
		}

		String pvdDumpToFile;
		if ( commandLine.GetFirstOption( "pvdDumpToFile", pvdDumpToFile ) )
		{
			if ( !pvdDumpToFile.Empty() )
			{
				params.m_pvdDumpToFile = (pvdDumpToFile == red::String( "true" ));
			}
		}

		String renderPreset;
		if( commandLine.GetFirstOption( "renderPreset", renderPreset ) )
		{
			params.m_renderPreset = rend::GetRenderFilterPreset( renderPreset );
		}

		String graphicsQualityLevel;
#if defined(RED_PLATFORM_DURANGO)
		CONSOLE_TYPE consoleType = ::GetConsoleType();
		switch ( consoleType )
		{
			case CONSOLE_TYPE_UNKNOWN:				RED_FATAL_ASSERT( false, "Unknown Xbox Platform" ); graphicsQualityLevel = "Console"; break;
			case CONSOLE_TYPE_XBOX_ONE:				graphicsQualityLevel = "Console"; break;
			case CONSOLE_TYPE_XBOX_ONE_S:			graphicsQualityLevel = "Console"; break;
			case CONSOLE_TYPE_XBOX_ONE_X:			graphicsQualityLevel = "ConsolePro"; break;				// Xbox One X (Scorpio)
			case CONSOLE_TYPE_XBOX_ONE_X_DEVKIT:	graphicsQualityLevel = "ConsolePro"; break;
			case CONSOLE_TYPE_XBOX_SERIES_S:		graphicsQualityLevel = "ConsolePro"; break;				// Currently we set Series S (Lockhart) to be the same as Xbox One X (Scorpio)
			case CONSOLE_TYPE_XBOX_SERIES_X:		graphicsQualityLevel = "ConsoleEarlyNextGen"; break;	// Anaconda gets the highest settings, although by default we want "performance" mode.
			case CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT:	graphicsQualityLevel = "ConsoleEarlyNextGen"; break;
			default:								RED_FATAL_ASSERT( false, "Unknown Xbox Platform" ); graphicsQualityLevel = "Console"; break;
		};

		// Then set the enum based on the level
		params.m_qualityLevel = Config::QualityLevelNameToEnum( graphicsQualityLevel );
#else
#ifdef RED_PLATFORM_WINPC
		if ( commandLine.HasOption( "forceLowSettings" ) )
		{
			params.m_qualityLevel = Config::GraphicsQualityLevel::Low;
		}
		else
#endif
		if ( commandLine.GetFirstOption( "qualityLevel", graphicsQualityLevel ) )
		{
			if ( graphicsQualityLevel == "RTX" )
			{
				params.m_qualityLevel = Config::GraphicsQualityLevel::RTXUltra;
			}
			else
			{
				params.m_qualityLevel = Config::QualityLevelNameToEnum( graphicsQualityLevel );
			}
		}
#	ifdef RED_PLATFORM_WINPC
		//
		// Main path for PC platform when one did not pass -qualityLevel flag. Quality level is
		// set to GraphicsQualityLevel::FromUserSettings in order to pull all relevant settigs
		// from UserSettings.json.
		//
		else
		{
			params.m_qualityLevel = Config::GraphicsQualityLevel::FromUserSettings;
		}
#	endif
#endif

		String textureQualityLevel;
		if ( commandLine.GetFirstOption( "texQualityLevel", textureQualityLevel ) )
		{
			params.m_textureQualityLevel = Config::TextureQualityLevelNameToEnum( textureQualityLevel );
		}
#ifdef RED_PLATFORM_WINPC
		//
		// Main path for PC platform when one did not pass -texQualityLevel flag.
		// Texture quality level is pulled straight from UserSettings.json
		//
		else
		{
			params.m_textureQualityLevel = Config::TextureQualityFromInGameSettings();
		}
#endif

		String rayTracingAmbientOcclusionRayNumber;
		if (commandLine.GetFirstOption( "rayTracingAmbientOcclusionRayNumber", rayTracingAmbientOcclusionRayNumber ))
		{
			params.m_rayTracingAmbientOcclusionRayNumber = ::FromStringDirect< Int32 >( rayTracingAmbientOcclusionRayNumber );
		}

		String dlssQuality;
		if ( commandLine.GetFirstOption( "dlssQuality", dlssQuality ) )
		{
			params.m_dlssQuality = ::FromStringDirect< Int32 >( dlssQuality );
		}

		String engine;
		if ( commandLine.GetFirstOption( "engine", engine ) && engine == "BackendGameEngine" )
		{
			params.m_isBackendEngine = true;
		}

		commandLine.GetFirstParam( "interopstartingport", params.m_interopStartingPort );

		String watchdogTimeout;
		if ( commandLine.GetFirstOption( "watchdogTimeout", watchdogTimeout ) )
		{
			params.m_watchdogTimeoutSecondsOverride = ::FromStringDirect<Uint32>( watchdogTimeout );
		}

		commandLine.GetFirstOption( "automator", params.m_automatorID );

#ifndef RED_CONFIGURATION_FINAL
		commandLine.GetFirstOption( "qaPreset", params.m_defaultQAPreset );
		commandLine.GetFirstOption( "debugOverlay", params.m_defaultDebugOverlay );
		{
			red::DynArray< String > temp{ red::PoolDebug() };
			commandLine.GetAllParams< String >( "debugWindow", temp );

			for ( const String& raw : temp )
			{
				params.m_defaultRequestedWindows.PushBack( RED_NAME( raw ) );
			}
		}

		// s.ortiz TEMP
		commandLine.GetAllParams< String >( "tweakDBDLC", params.m_tweakDBdlcsTEMP );
#endif
	}

	CName GameAppBaseInitializationState::GetEngineTypeName( const red::CommandLine& commandLine ) const
	{
		String engineTypeName = Config::cvDefaultEngine.Get().AsChar();
		if( !commandLine.GetFirstParam( ( "engine" ), engineTypeName ) && engineTypeName.Empty() )
		{
			RED_LOG( "Engine: No engine specified on the command line or in the config files" );
			return CName::NONE();
		}

		return RED_NAME( engineTypeName );
	}

	void GameAppBaseInitializationState::InitializeBaseEngineSystems( GameAppInstance& gameApp, const red::CommandLine& commandLineParser )
	{
		RED_MEMORY_MA_WRITE_BOOKMARK( "Initialize Engine", "Started initializing engine" );

		CName engineTypeName = GetEngineTypeName( commandLineParser );
		const auto* engineClass = GetRttiSystem().FindClass( engineTypeName );
		if( engineClass == nullptr )
		{
			RED_LOG( "Engine: Could not find specified engine type: %hs", engineTypeName.AsChar() );
			return;
		}

		if( !engineClass->IsA( CBaseEngine::GetStaticClass() ) )
		{
			RED_LOG( "Engine: Specified engine is not a valid engine type: %hs", engineTypeName.AsChar() );
			return;
		}

		if ( !engineClass )
		{
			RED_LOG( "Engine: Cannot find class type for engine" );
			return;
		}

		GEngine = static_cast< CBaseEngine* >( engineClass->CreateObject( engineClass->GetSize() ) );
		RED_ASSERT( GEngine->IsHeadless() == IsHeadlessMode(), "Engine headless mode mismatch." );
		CreateEngineInitParams( commandLineParser, gameApp.m_engineInitParams );
		GEngine->InitializeAsync( gameApp.m_engineInitParams );
	}
}