/**
 * Copyright (c) 2019-2020 CD Projekt Red. All Rights Reserved.
 */

#include "build.h"
#include <emscripten.h>
#include <cstdio>
#include "baseEngine.h"
#include "screenshotSystem.h"
#include "scriptHelper.h"
#include "timeDilation.h"
#include "baseEngineCrashData.h"
#include "fadeOutSupervisorInterface.h"

#include "../../core/include/debugPageServer.h"
#include "../../redFileSystem/include/fileSys.h"
#include "../../redReflection/include/version.h"
#include "../../redReflection/include/resourceMonitor.h"
#include "../../redReflection/include/streamedResourceManager.h"
#include "../../redReflection/include/eventDebug.h"
#if defined( RED_FILE_SYNC_SERVICE_ENABLED )
	#include "../../redFileSystem/include/fileSyncService.h"
#endif

#include "../../input/include/inputSystem.h"
#include "../../input/include/inputContext.h"

#include "../../renderData/include/renderCommandHandler.h"
#include "../../renderData/include/viewport.h"

#include "../../localization/include/localizationManager.h"

#include "../../sound/include/soundSystem.h"

#include "../../telemetry/include/telemetryPublic.h"

#include "../../commChannel/include/networkConnection.h"

#include "../../resourceMaterial/include/materialCompilationManager.h"
#include "../../resourceMaterial/include/materialInstance.h"

#include "../../redConfig/include/inGameConfigSystem.h"

#include "../../inkCore/include/inkSystem.h"

#ifdef RED_PLATFORM_ORBIS
#include <system_service.h>
#endif
#include "../../redCore/include/debugServerManager.h"
#include "../../redReflection/include/scriptingSystemImpl.h"
#include "../../redReflection/include/scriptVersion.h"
#include "../../debugPages/include/debugPageRegisterer.h"
#include "inGameProfilerFrontEnd_CPUChromeTracing.h"

#include "../../redNetwork/include/ping.h"
#include "../../inkCore/include/gameNotificationsUtilities.h"

#if !defined( RED_CONFIGURATION_FINAL ) || defined( USE_PROFILER )
// We don't allow to have script path in crash data as it can show user folder name in dump reports from gamers' crash reports.
#define SCRIPT_BLOB_PATH_IN_CRASH_DATA
#endif

//////////////////////////////////////////////////////////////////////////
// configs
namespace Config
{
	TConfigVar< Int32 >										cvLimitFPS( "Engine", "LimitFPS", 60, 0, INT_MAX, eConsoleVarFlag_Save );

	TConfigVar< Bool >										cvDebugDumpNameList( "Debug", "DumpNameList", false );

	TConfigVar< String >									cvEngineFallbackCacheDirectory( "Engine", "FallbackCacheDirectory", "cache", eConsoleVarFlag_Developer );

	TConfigVar< Bool >										cvScriptsEnableCompilation( "Scripts", "EnableCompilation", false, eConsoleVarFlag_Developer );
	TConfigVar< String >									cvScriptsBlobFilename( "Scripts", "ScriptsBlobFilename", "master.redscripts", eConsoleVarFlag_Developer );
	TConfigVar< Bool >										cvScriptsSilentCompilation( "Scripts", "SilentCompilation", false );
	TConfigVar< Bool >										cvScriptsSilentValidation( "Scripts", "SilentValidation", false );

	RED_ENGINE_API TConfigVar< Bool >						cvDebugDumpMultiplayer( "Multiplayer", "DebugDump", false, eConsoleVarFlag_Developer );

	// profiler
	TConfigVar<String> cvActiveProfilers( "Profiler", "ActiveProfilers", "none", eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
	TConfigVar<Int32 > cvProfilingLevel( "Profiler", "ProfilingLevel", 4, 0, 4, eConsoleVarFlag_Developer );
	TConfigVar<String> cvProfilerServerName( "Profiler", "ProfilerServerName", "localhost", eConsoleVarFlag_Developer );
	TConfigVar<Float> cvProfilerBreakpointThresholdMs( "Profiler", "ProfilerBreakpointThresholdMs", 20.f, eConsoleVarFlag_Developer );

	TConfigVar< Bool > cvDebugUseStreamingFallback( "Editor/Streaming/Debug", "UseFallbackSlowmotion", false, eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
	TConfigVar< Int32 > cvDebugUseStreamingFallbackThreshold( "Editor/Streaming/Debug", "FallbackSlowmotionThreshold", 8000, eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
    TConfigVar< Int32 > cvDebugUseStreamingFallbackBucketBegin( "Editor/Streaming/Debug", "FallbackSlowmotionBucketBegin", 2, eConsoleVarFlag_Save | eConsoleVarFlag_Developer );
	TConfigVar< String > cvDebugUseStreamingEaseOutCurve("Editor/Streaming/Debug", "FallbackSlowmotionEaseOutCurve", "SandevistanEaseOut", eConsoleVarFlag_Save | eConsoleVarFlag_Developer);
}

namespace input
{
	class InputContextImpl : public IInputContext
	{
		RED_USE_MEMORY_POOL(input::PoolInputInternal);
	public:
		InputCollection	m_bufferedInputData;
		IInputSystem*	m_inputSystem;

		InputContextImpl( IInputSystem* deviceManager )
			: m_bufferedInputData(input::PoolInputInternal())
			, m_inputSystem( deviceManager )
		{}

		~InputContextImpl() override = default;

		InputCollection& GetBufferedInputData() override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_bufferedInputData;
		}

		void FlushConsumedEvents() override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			input::FlushConsumedEvents(m_bufferedInputData);
		}

		InputCollection GetSubDataByViewportID(ViewportID viewportID) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return input::GetSubDataByViewportID(m_bufferedInputData, viewportID);
		}

		InputCollection GetSubDataByDeviceID(DeviceID deviceID) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return input::GetSubDataByDeviceID(m_bufferedInputData, deviceID);
		}

		void RequestReset() override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->RequestReset();
		}

		void RefreshDevices() override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->RefreshDevices();
		}

		bool IsDeviceConnected(DeviceID deviceID) const override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->IsDeviceConnected(deviceID);
		}

		void SetRumble(DeviceID id, Float left, Float right) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->SetRumble(id, left,right);
		}

		void SetBacklightColor(DeviceID id, const Color& color) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->SetBacklightColor(id, color);
		}

		void ResetBacklightColor(DeviceID id) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->ResetBacklightColor(id);
		}

		Bool IsCursorForced() override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->IsCursorForced();
		}

		void ForceCursor( const CName reason, Bool show ) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->ForceCursor( reason, show );
		}

		Bool IsCursorVisible() const override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->IsCursorVisible();
		}

		void ShowCursor(const CName reason) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->ShowCursor(reason);
		}

		void HideCursor(const CName reason) override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->HideCursor(reason);
		}

		void GetCursorPos(const ViewportID id, Int32& x, Int32& y) const override final
		{
			RED_FATAL_ASSERT(::SIsMainThread(), "Calls to the input context may only be made on the main thread.");
			m_inputSystem->GetCursorPos(id, x, y);
		}

		void GetCursorPos(const ViewportID id, Float& x, Float& y) const override final
		{
			RED_FATAL_ASSERT(::SIsMainThread(), "Calls to the input context may only be made on the main thread.");
			m_inputSystem->GetCursorPos(id, x, y);
		}

		void SetCursorPos( const ViewportID id, Int32 x, Int32 y ) const override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			m_inputSystem->SetCursorPos( id, x, y );
		}

		CName GetDeviceName(DeviceID id) const override final
		{
			RED_FATAL_ASSERT(::SIsMainThread(), "Calls to the input context may only be made on the main thread.");
			return m_inputSystem->GetDeviceName(id);
		}

		EInputDevice GetDeviceType(DeviceID id) const override final
		{
			RED_FATAL_ASSERT(::SIsMainThread(), "Calls to the input context may only be made on the main thread.");
			return m_inputSystem->GetDeviceType(id);
		}

		Bool GetKeyState( EInputKey key ) const override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->GetKeyState( key );
		}

		Vector2 GetAxisState( Bool isLeftStick ) const override final
		{
			RED_FATAL_ASSERT( ::SIsMainThread(), "Calls to the input context may only be made on the main thread." );
			return m_inputSystem->GetAxisState( isLeftStick );
		}

		ViewportID GetLastInputViewportID() const override final
		{
			return m_inputSystem->GetLastInputViewportID();
		}
	};

	red::UniquePtr<input::InputContextImpl> GatherInput(IInputSystem* inputSystem, const Float timeDelta )
	{
		red::UniquePtr<input::InputContextImpl> ctx_ptr = red::CreateUniquePtr<input::InputContextImpl>(inputSystem);
		inputSystem->Update(ctx_ptr->m_bufferedInputData, timeDelta);
		return ctx_ptr;
	}
}

RTTI_BEGIN_TYPE( CBaseEngine );
RTTI_END_TYPE();

// The engine
RED_ENGINE_API CBaseEngine* GEngine = nullptr;

CBaseEngine::CBaseEngine( Bool enableScriptReload )
	: m_rawEngineTime( 0.0f )
	, m_lastTimeDelta( 0.0f )
	, m_lastTimeDeltaUnclamped( 0.0f )
	, m_lastTickRate( 0.0f )
	, m_minTickRateOver1Sec( 0.0f )
	, m_lastFrameCPUTime( 0.0f )
	, m_minTickRateToShow( 0.0f )
	, m_exitRequestLockCount( 0 )
	, m_requestExit( false )
	, m_requestExitCache( false )
	, m_returnValue( 0 )
	, m_returnValueCache( 0 )
	, m_currentEngineTick( 0 )
	, m_timeSinceLastScriptFileCheck( 0.0f )
	, m_scriptVersion( script::ScriptVersion_Final )
	, m_blobFileModifiedDate( 0 )
	, m_state( BES_Uninitialized )
	, m_resourceMode( BERM_Full )
	, m_isGameSessionInitialized( false )
	, m_viewportManager( nullptr )
	, m_scriptsLoaded( false )
	, m_scriptsSilentCompilation( false )
	, m_scriptsSilentValidation( false )
	, m_gatheredResourceSilentValidation( false )
	, m_enableScriptsReload( enableScriptReload )
	, m_scriptsValidationErrors( red::PoolDebug() )
	, m_timeDilation( red::CreateUniquePtr< tick::TimeDilationController > () )
#ifdef RED_NETWORK_ENABLED
	, m_pingUtil( RED_NEW( red::Network::Ping ) )
#endif
#ifndef NO_DEBUG_PAGES
	, m_debugPageRegisterer( RED_NEW( DebugPageRegisterer ) )
#endif
#ifdef USE_RED_INGAME_PROFILER
	, m_inGameProfilerFrontEnd( RED_NEW( red::InGameCPUProfilerFrontEnd ) )
	, m_inGameChromeTracingProfiler( RED_NEW( red::InGameChromeTracingProfiler ) )
#endif

{
	RED_LOG( "PLM: BES_Initializing" );

	//InGameConfig::GetRefreshEventDispatcher().RegisterForEvent( RED_NAME_CONSTEXPR( refreshEngine ), &m_refreshListener );
	//InGameConfig::GetRefreshEventDispatcher().RegisterForEvent( RED_NAME_CONSTEXPR( refreshViewport ), &m_refreshListener );
	//InGameConfig::GetRefreshEventDispatcher().RegisterForEvent( RED_NAME_CONSTEXPR( RefreshOutputMonitor ), &m_refreshListener );


#ifndef NO_RUNTIME_MATERIAL_COMPILATION
	// ctremblay: Circular dependency fix betweem materials and texture. This need fixing
	SetRecompileMaterialsCallback( []( ITexture* texture ){ CMaterialInstance::RecompileMaterialsUsingTexture( texture ); } );
#endif
}

CBaseEngine::~CBaseEngine()
{
	//InGameConfig::GetRefreshEventDispatcher().UnregisterFromAllEvents( &m_refreshListener );
}

Float CBaseEngine::GetLastFrameCPUTime() const
{
	return m_lastFrameCPUTime;
}

Float CBaseEngine::GetLastFrameGPUTime() const
{
	return GRender->GetLastGPUFrameDuration();
}

Float CBaseEngine::GetLastFrameScriptsTime() const
{
	return CScriptingSystem::GetInstance().GetLastScriptFrameDuration();
}

Bool CBaseEngine::IsHeadless() const
{
	return false;
}

Bool CBaseEngine::IsBackend() const
{
	return false;
}

Bool CBaseEngine::IsTestEngine() const
{
	return false;
}

tick::ITimeDilation* CBaseEngine::GetTimeDilationController() const
{
	return m_timeDilation.Get();
}


void StreamingStats::Detail::Clear()
{
    red::Memzero( prioHistogram.Data(), prioHistogram.DataSize() );
}

void StreamingStats::Detail::AddNodeByPrio( Uint8 prio )
{
    // 16 because:
    // we have 16 buckets, for Uint8 (256 different values) in each bucket span to 16 values
    // so to compute bucket index we need to divide prio by 16 :)
    constexpr Uint32 bitshift = 4; // div by 16

    const Uint32 index = prio >> bitshift;
    RED_FATAL_ASSERT( index < prioHistogram.Size() );
    prioHistogram[index] += 1;
}

void CBaseEngine::UpdateStreamingFallback( const tick::Info& tickInfo )
{
	if( !Config::cvDebugUseStreamingFallback.Get() )
		return;

    if( IsBackend() )
        return;

    const CName c_reason = RED_NAME_CONSTEXPR( "Engine_Streaming" );
    const Uint32 c_pendingThreshold = Max( 300u, (Uint32)Config::cvDebugUseStreamingFallbackThreshold.Get() );
	const Uint32 c_pendingTotalThreshold = 2 * c_pendingThreshold;
	const Uint32 c_releaseThreshold = Min( 300u, c_pendingThreshold / 2 );
    const Float c_cooldown = 25.f; // [s]
    const Float c_dilation = 0.15f;
	const Float timeDelta = tickInfo.m_realTimeDelta;

	red::Optional<Bool> isStreamingFallbackActive = red::MakeEmptyOptional<Bool>();
	Bool canActivate = !tickInfo.IsGameplayHidden();

	if( auto loadingScreen = m_inkSystem->GetLoadingScreen() )
	{
		canActivate &= !loadingScreen->IsLoadingScreenUp();
	}

	if(red::SharedPtr<ink::ViewportWrapper> viewportWrapper = m_inkSystem->GetGameViewportWrapper())
	{
		if( const THandle< ink::Layer >& menuLayer = viewportWrapper->GetLayerByType( ink::ELayerType::Menu ) )
		{
			canActivate &= !menuLayer->GetActive();
		}
	}

	if( canActivate )
	{
		const StreamingStats& stats = GetStreamingStats();
        const StreamingStats::Detail& statsDetail = stats.nodePendingDetails;

        StreamingTimeDilationState& dilationState = m_streamingTimeDilationState;
        
        const Uint32 bucketBegin = Config::cvDebugUseStreamingFallbackBucketBegin.Get();
        Uint32 pending = 0;

        for( Uint32 i = 0; i < bucketBegin; ++i )
        {
            if( statsDetail.prioHistogram[i] > 8000 ) // only if we are in a big trouble
                pending += statsDetail.prioHistogram[0];
        }

        for( Uint32 i = bucketBegin; i < stats.nodePendingDetails.prioHistogram.Size(); ++i )
        {
            pending += stats.nodePendingDetails.prioHistogram[i];
        }

		if( pending > c_pendingThreshold )
		{
			if( (dilationState.cooldown <= 0.f) || (pending > c_pendingTotalThreshold) )
			{
				dilationState.cooldown = c_cooldown;
				m_timeDilation->SetTimeDilation( c_reason, c_dilation, -1.f );
				dilationState.waitingForPending = true;
				isStreamingFallbackActive = true;

				//show loading spinner
				{
					THandle< ink::GameNotificationData > spinnerData = CreateHandle<ink::GameNotificationData>();

					spinnerData->m_notificationName = RED_NAME_CONSTEXPR( "base\\gameplay\\gui\\widgets\\streaming_spinner\\streaming_spinner.inkwidget" );
					spinnerData->m_queueName = RED_NAME_CONSTEXPR( "streaming_spinner" );

					m_spinnerHandle = m_inkSystem->ShowGameNotification( spinnerData );
				}
			}
		}

		if( (pending < c_releaseThreshold) && dilationState.waitingForPending )
		{
			const CName easeOutCurve = RED_NAME(Config::cvDebugUseStreamingEaseOutCurve.Get());
			m_timeDilation->UnsetTimeDilation( c_reason, easeOutCurve); // ease out for better control
			dilationState.waitingForPending = false;
			isStreamingFallbackActive = false;

			//hide loading spinner
			m_spinnerHandle = nullptr;
		}

		if( dilationState.cooldown > 0.f )
		{
			dilationState.cooldown -= timeDelta;
		}
	}
	else
	{
		m_timeDilation->UnsetTimeDilation( c_reason, CName::NONE() ); // unset instantly
		m_streamingTimeDilationState = {};
		isStreamingFallbackActive = false;
	}

	if( isStreamingFallbackActive.HasValue() && m_soundSystem )
	{
		m_soundSystem->SetStreamingLimitedMode( isStreamingFallbackActive.Value() );
	}
}

void CBaseEngine::OnSuspend()
{
	// NOTE: On PS4 we are not calling OnSuspend as we are not informed about that. Only OnResume is called.

	// TODO: Add Suspend mode to the GSM

#if defined( RED_PLATFORM_DURANGO )
	// Guard against GRender not existing yet (early in boot)
	if ( GRender )
	{
		GRender->GetCommands()->SuspendRendering();
		GRender->Flush();
	}
#endif

	// TODO: We don't set BES_Running for the engine so log fails because we are in BES_Initialized state. Fix it or remove Running and Suspended state as not used.
	const Uint32 oldState = m_state.CompareExchange( BES_Suspended, BES_Running );
	RED_LOG( "PLM: BES_Suspended %hs, oldState = %u", ( oldState == BES_Running || oldState == BES_Suspended ) ? "set" : "not set", oldState );
}

void CBaseEngine::OnResume()
{
	// TODO: We don't set BES_Running for the engine so log fails because we are in BES_Initialized state. Fix it or remove Running and Suspended state as not used.
	const Uint32 oldState = m_state.CompareExchange( BES_Running, BES_Suspended );
	RED_LOG( "PLM: BES_Running %hs, oldState = %u", ( oldState == BES_Suspended || oldState == BES_Running ) ? "set" : "not set", oldState );

	// Guard against GRender not existing yet (early in boot)
#if defined( RED_PLATFORM_DURANGO )
	if ( GRender )
	{
		GRender->GetCommands()->ResumeRendering();
		GRender->Flush();
	}
#endif

	telemetry::OnResume();
}

void CBaseEngine::OnResourceModeChanged( EBaseEngineResourceMode resourceMode )
{
	if( m_resourceMode.Exchange( resourceMode ) == resourceMode )
	{
		return;
	}

	// TODO: call handlers for jobs, rendering and other systems.

	// Constrained for PS4 == Background:
	// In the background state, use of the GPU (one of the resources shown in the "Resources That Can Be Used by Applications" chapter) by applications
	// will be restricted at most by 50% and the allocation of CPU#6 will be restricted to 10%.

	// Constrained for X1
	// The title has the following resources available:
	// - 4 CPU cores
	// - 45 % of the GPU processing power

	switch( resourceMode )
	{
	case BERM_Full:
	case BERM_FullWithSystem:
		/*if (GSoundSystem)
		{
			GSoundSystem->SoundEvent("system_resume");
		}*/
		break;

	case BERM_FullWithSystemNoInput:
	case BERM_Constrained:
		/*if (GSoundSystem)
		{
			GSoundSystem->SoundEvent("system_suspend");
		}*/
		break;
	}
}

void CBaseEngine::RequestExit( Int32 programReturnValue )
{
	RED_LOG_INFO( "Engine: Requesting exit: return value=%ld", programReturnValue );

	RED_SCOPE_LOCK( m_exitLock );

	if ( m_exitRequestLockCount > 0 )
	{
		if ( m_requestExitCache )
		{
			RED_LOG_WARNING( "Engine: Processing another exit request. Ignoring the current one." );
			return;
		}

		m_requestExitCache = true;
		m_returnValueCache = programReturnValue;
	}
	else if ( !m_requestExit )
	{
		dd::GetEngineCrashData().requestExit.Set( true );

		m_requestExit = true;
		m_returnValue = programReturnValue;
	}
	else
	{
		RED_LOG_WARNING( "Engine: Processing another exit request. Ignoring the current one." );
	}
}

Bool CBaseEngine::LockExitRequests()
{
	RED_SCOPE_LOCK( m_exitLock );

	if ( m_requestExit )
	{
		return false;
	}

	++m_exitRequestLockCount;
	return true;
}

void CBaseEngine::UnlockExitRequests()
{
	RED_SCOPE_LOCK( m_exitLock );

	if ( --m_exitRequestLockCount == 0 && m_requestExitCache )
	{
		dd::GetEngineCrashData().requestExit.Set( true );
		m_returnValue = m_returnValueCache;
		m_requestExit = true;
	}
}

Bool CBaseEngine::LoopSingleTick( Bool ( CBaseEngine::* singleTickFun )( const float ) )
{
	Bool result = false;

	{
		PC_SCOPE( Frame );

#if defined( RED_LOGGING_ENABLED )
		UpdateLoggingDebugFilters();
#endif

#ifdef USE_RED_INGAME_PROFILER
	if ( !IsHeadless() )
	{
		m_inGameProfilerFrontEnd->Update();
	}
#endif

#ifdef CONTINUOUS_SCREENSHOT_HACK
		red::Clock::GetInstance().GetTimer().NextFrameGameTimeHack();
#endif

		// Calculate time delta
		Double curTime = red::Clock::GetInstance().GetTimer().GetSeconds();
		m_lastTimeDeltaUnclamped = (Float)( curTime - m_oldTickTime );
		m_oldTickTime = curTime;

#ifdef RED_CONFIGURATION_FINAL
		float timeDelta = Min( m_lastTimeDeltaUnclamped, 0.066f );
#else
		float timeDelta = Min( m_lastTimeDeltaUnclamped, 0.1f ); // so it is "playable" with low framerate - although issues related to physics may appear
#endif

		// Advance internal timers
		m_rawEngineTime += timeDelta;
		m_lastTimeDelta = timeDelta;

		Float currentTickRate = 1.f / timeDelta;

		if (m_minTickRateOver1Sec > currentTickRate || m_minTickRateOver1Sec == 0.f)
		{
			m_minTickRateOver1Sec = currentTickRate;
		}

		// Tick rate calculation
		if ( curTime > (m_prevTickRateTime + 1.0) )
		{
			m_lastTickRate = m_numTicks / (Float)( curTime - m_prevTickRateTime );
			m_minTickRateToShow = m_minTickRateOver1Sec;
			m_minTickRateOver1Sec = 0.f;
			m_prevTickRateTime = curTime;
			m_numTicks = 0;
		}

		// Tick the engine
		if ( timeDelta > 0.0f )
		{
			result = ( this->*singleTickFun )( timeDelta );
		}
#ifdef __EMSCRIPTEN__
		else
		{
			// Browser frames can land on the same clock reading; skip this tick instead of
			// reporting failure (false makes GameAppRunningState request exit -> Shutdown).
			result = true;
		}
#endif

		// Count ticks
		m_numTicks += 1;
	}

	// Notify redMemory about new frame and reset frame-based allocators
	red::memory::ProcessNextFrame();

	// right out of scope, so the main frame scope is closed when the frame ticks in the profiler
	PROFILER_NextFrame( red::ProfilerFrameType::PFT_ENGINE );

	return result;
}

Bool CBaseEngine::BaseLoopSingleTick()
{
	return LoopSingleTick( &CBaseEngine::ProcessBaseLoopFrame );
}

Bool CBaseEngine::MainLoopSingleTick()
{
	return LoopSingleTick( &CBaseEngine::ProcessMainLoopFrame );
}

Bool CBaseEngine::ProcessBaseLoopFrame( const Float timeDelta )
{
	red::Timer timer;
	const Uint64 beginFrameTick = timer.GetTicks();

	Bool result = true;

#if defined( USE_PROFILER )
	CScriptingSystem::GetInstance().ResetLastScriptFrameDuration();
#endif

	if ( Config::cvDebugDumpMultiplayer.Get() )
	{
		static Uint32 frameIndex = 0;
		RED_LOG( "=== Next engine frame %u ===", frameIndex++ );
	}

#if 0 // disabling scripts reload, because it is not properly handled (yet)
#ifndef RED_PLATFORM_CONSOLE
	if ( m_enableScriptsReload )
	{
		PC_SCOPE( CheckForUpdatedScriptsBlob );
		CheckForUpdatedScriptsBlob( timeDelta );
	}
#endif
#endif

	telemetry::Update();

	if ( !IsHeadless() )
	{
		// If renderer is not ready eg. has lost device, do not tick game
		PC_SCOPE( PrepareRenderer );
		if ( !GRender->PrepareRenderer() )
		{
			PC_SCOPE( RenderNotPrepared );
			red::SleepOnCurrentThread( 100 );
			return result;
		}
	}

#ifdef RED_NETWORK_ENABLED
	// update debug server
	DBGSRV_CALL( Update() );
#endif

	// Update debug page service
#ifdef RED_NETWORK_ENABLED
	{
		PC_SCOPE( DebugPageServer );
		CDebugPageServer::GetInstance().ProcessServiceCommands( timeDelta );
	}
#endif

	// Advance the screenshot system
	{
		PC_SCOPE( SScreenshotSystem_Tick );
		GetScreenshotSystem().Tick();
	}

	// Process time dilation
	const Uint32 flags = GetFadeOutSupervisor() ? GetFadeOutSupervisor()->GetTickFlags() : 0;
	const tick::Info tickInfo( timeDelta, m_timeDilation->ComputeGlobalTimeDilation(), *m_timeDilation, flags );
	const Float scaledTimeDelta = timeDelta * tickInfo.m_globalTimeDilation;

	{
		UpdateStreamingFallback( tickInfo );
	}

	// Tick renderer
	{
		PC_SCOPE( RenderTick );
		if ( GRender )
		{
			GRender->Tick( scaledTimeDelta, tickInfo.m_globalTimeDilation, m_lastFrameCPUTime );
		}
	}

	red::UniquePtr<input::InputContextImpl> inputContext;

	// Update inputs
	{
		PC_SCOPE( ProcessInput );
		inputContext = input::GatherInput(m_inputSystem.Get(), scaledTimeDelta); // TODO: should we cheat the input? pass scaled timeDelta?

		ProcessInputInternal( *inputContext );
	}

	// Process UI framework
	if ( m_inkSystem )
	{
		m_inkSystem->SimpleTick( tickInfo, *inputContext.Get() );
	}

	// Process simulation frame
	{
		PC_SCOPE( ProcessBaseFrame );
		result = ProcessBaseFrame( scaledTimeDelta, tickInfo, *inputContext.Get() );
	}

#if defined( RED_FILE_SYNC_SERVICE_ENABLED )
	if ( FileSyncService::DidCriticalErrorOccur() )
	{
		RequestExit( -33 );
	}
#endif

	// Process resource monitoring events
	{
		PC_SCOPE(ResourceMonitor);
		res::MonitorRouter::GetInstance().DispatchChanges();
	}

	// Update last frame CPU time
	const Uint64 endFrameTick = timer.GetTicks();
	m_lastFrameCPUTime = static_cast< Float >( ( endFrameTick - beginFrameTick ) / timer.GetFrequency() * 1000.f );

	// Update current engine tick
	++m_currentEngineTick;

	return result;
}

Bool CBaseEngine::ProcessMainLoopFrame( const Float timeDelta )
{
	red::Timer timer;
	const Uint64 beginFrameTick = timer.GetTicks();
    static unsigned long long cpSample=0;
    const bool cpTrace=++cpSample<=4 || (cpSample%16)==0;
    const double cpStart=emscripten_get_now();

	Bool result = true;

#if defined( RED_ENABLE_EVENT_DEBUG )
	red::eventDebug::OnBeginFrame();
#endif

#if defined( USE_PROFILER )
	CScriptingSystem::GetInstance().ResetLastScriptFrameDuration();
#endif

	if ( Config::cvDebugDumpMultiplayer.Get() )
	{
		static Uint32 frameIndex = 0;
		RED_LOG( "=== Next engine frame %u ===", frameIndex++ );
	}

#if 0 // disabling scripts reload, because it is not properly handled (yet)
#ifndef RED_PLATFORM_CONSOLE
	if ( m_enableScriptsReload )
	{
		PC_SCOPE( CheckForUpdatedScriptsBlob );
		CheckForUpdatedScriptsBlob( timeDelta );
	}
#endif
#endif

	telemetry::Update();
    const double cpAfterTelemetry=emscripten_get_now();

	if ( !IsHeadless() )
	{
		// If renderer is not ready eg. has lost device, do not tick game
		PC_SCOPE( PrepareRenderer );
		if ( !GRender->PrepareRenderer() )
		{
			PC_SCOPE( RenderNotPrepared );
			red::SleepOnCurrentThread( 100 );
			return result;
		}
	}

const double cpAfterPrepare=emscripten_get_now();
#ifdef RED_NETWORK_ENABLED
	// update debug server
	DBGSRV_CALL( Update() );
#endif

	// Update debug page service
#ifdef RED_NETWORK_ENABLED
	{
		PC_SCOPE( DebugPageServer );
		CDebugPageServer::GetInstance().ProcessServiceCommands( timeDelta );
	}
#endif

	if (GResourceLoader)
	{
		GResourceLoader->Update();
	}

const double cpAfterResources=emscripten_get_now();
	// Advance the screenshot system
	{
		PC_SCOPE( SScreenshotSystem_Tick );
		GetScreenshotSystem().Tick();
	}

	// Process time dilation
	const Uint32 flags = GetFadeOutSupervisor() ? GetFadeOutSupervisor()->GetTickFlags() : 0;
	const tick::Info tickInfo( timeDelta, m_timeDilation->ComputeGlobalTimeDilation(), *m_timeDilation, flags );
	const Float scaledTimeDelta = timeDelta * tickInfo.m_globalTimeDilation;

	{
		UpdateStreamingFallback( tickInfo );
	}

const double cpAfterStreaming=emscripten_get_now();
	// Tick renderer
	{
		PC_SCOPE( RenderTick );
		if ( GRender )
		{
			GRender->Tick( timeDelta, tickInfo.m_globalTimeDilation, m_lastFrameCPUTime );
		}
	}

const double cpAfterRenderer=emscripten_get_now();
	red::UniquePtr<input::InputContextImpl> inputContext;

	// Update inputs
	{
		PC_SCOPE( ProcessInput );
		if ( m_inputSystem )
		{
			inputContext = input::GatherInput(m_inputSystem.Get(), scaledTimeDelta); // TODO: should we cheat the input? pass scaled timeDelta?

			ProcessInputInternal( *inputContext );
		}
	}

const double cpAfterInput=emscripten_get_now();
    double cpAfterUi=cpAfterInput,cpAfterSimulation=cpAfterInput;
	if ( m_inputSystem )
	{
		if( m_inkSystem )
		{
#ifdef INK_ENABLE_PROFILING
			m_inkSystem->SetCurrentFrameNumber( GetCurrentEngineTick() );
#endif
			m_inkSystem->BeginFrame();
			m_inkSystem->Tick( tickInfo, *inputContext.Get() );
            cpAfterUi=emscripten_get_now();
		}

		if ( m_soundSystem && inputContext->GetBufferedInputData().Size() > 0 )
		{
			m_soundSystem->SetUsingKeyboardAndMouse(
				inputContext->GetDeviceType( inputContext->GetBufferedInputData().Back().m_deviceID ) == input::KBD_MOUSE );
		}

		// Process simulation frame
		{
			result = ProcessFrame( scaledTimeDelta, tickInfo, *inputContext.Get() );
            cpAfterSimulation=emscripten_get_now();
		}
	}

#ifndef NO_RUNTIME_MATERIAL_COMPILATION
	if ( m_asyncMaterialCompiler )
	{
		m_asyncMaterialCompiler->Tick( scaledTimeDelta, tickInfo );
	}
#endif // NO_RUNTIME_MATERIAL_COMPILATION

#if defined( RED_FILE_SYNC_SERVICE_ENABLED )
	if ( FileSyncService::DidCriticalErrorOccur() )
	{
		RequestExit( -33 );
	}
#endif

	// Process resource monitoring events
	{
		PC_SCOPE(ResourceMonitor);
		res::MonitorRouter::GetInstance().DispatchChanges();
	}

	{
		PC_SCOPE( InGameConfigSystem );
		InGameConfig::System::GetInstance().Update();
	}

    if(cpTrace) std::fprintf(stderr,
        "CP2077 engine frame phases probe: frame=%llu telemetryMs=%.3f prepareMs=%.3f resourcesMs=%.3f streamingMs=%.3f rendererMs=%.3f inputMs=%.3f uiMs=%.3f simulationMs=%.3f tailMs=%.3f totalMs=%.3f\n",
        cpSample,cpAfterTelemetry-cpStart,cpAfterPrepare-cpAfterTelemetry,
        cpAfterResources-cpAfterPrepare,cpAfterStreaming-cpAfterResources,
        cpAfterRenderer-cpAfterStreaming,cpAfterInput-cpAfterRenderer,
        cpAfterUi-cpAfterInput,cpAfterSimulation-cpAfterUi,
        emscripten_get_now()-cpAfterSimulation,emscripten_get_now()-cpStart);
	// Update last frame CPU time
	const Uint64 endFrameTick = timer.GetTicks();
	m_lastFrameCPUTime = static_cast< Float >( ( endFrameTick - beginFrameTick ) / timer.GetFrequency() * 1000.f );

	// Update current engine tick
	++m_currentEngineTick;

	return result;
}

Bool CBaseEngine::InitializeScriptsData( script::ScriptVersion scriptVersion, script::ProfilerMode profilerMode )
{
	RED_LOG_INFO( "[Scripts] Initializng Scripts Data" );

	CScriptingSystem::GetInstance().SetProfilerMode( profilerMode );

	if ( !m_scriptBlobPath.Empty() )
	{
		return InitializeSciptsDataBasedOnCustomBlob();
	}

	return InitializeSciptsDataBasedOnDefaultBlobs( scriptVersion );
}

Bool CBaseEngine::InitializeSciptsDataBasedOnCustomBlob()
{
	RED_LOG_INFO( "[Scripts] Using custom script blob file path %s.", m_scriptBlobPath.ToDebugString() );

	auto& crashData = dd::GetScriptsCrashData();
#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.primaryBlobPath.Set( m_scriptBlobPath );
#endif

	Uint64 primaryBlobModifiedDate = GFileManager->GetFileTimeStamp( m_scriptBlobPath );
	crashData.primaryBlobModifiedDate.Set( primaryBlobModifiedDate );

	if ( !LoadScripts( m_scriptBlobPath, primaryBlobModifiedDate, crashData ) )
	{
		crashData.blobPathUsed.Set( "None" );
		crashData.scriptsLoaded.Set( false );
	}

	return m_scriptsLoaded;
}

Bool CBaseEngine::InitializeSciptsDataBasedOnDefaultBlobs( script::ScriptVersion scriptVersion )
{
	const char* blobName = GetBlobName( scriptVersion );
	red::AbsolutePath primaryBlobPath = ScriptsHelper::GetCompileBlobPath().AddFilePath( blobName );
	red::AbsolutePath fallbackBlobPath = GFileManager->GetEngineRoot()
		.AddDirPath( Config::cvEngineFallbackCacheDirectory.Get().AsChar() )
		.AddFilePath( blobName );

	auto& crashData = dd::GetScriptsCrashData();

#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.primaryBlobPath.Set( primaryBlobPath );
	crashData.fallbackBlobPath.Set( fallbackBlobPath );
#endif

	CompileScripts( primaryBlobPath, crashData, scriptVersion );

	Uint64 primaryBlobModifiedDate = GFileManager->GetFileTimeStamp( primaryBlobPath );

#if defined( RED_PLATFORM_WINPC )
	if ( !primaryBlobModifiedDate )
	{
		RED_LOG_WARNING( "[Scripts] Failed to get time stamp for %s due to 0x%X", primaryBlobPath.ToDebugString(), GetLastError() );
	}
#endif

	Uint64 fallbackBlobModifiedDate = GFileManager->GetFileTimeStamp( fallbackBlobPath );

#if defined( RED_PLATFORM_WINPC )
	if ( !fallbackBlobModifiedDate )
	{
		RED_LOG_WARNING( "[Scripts] Failed to get time stamp for %s due to 0x%X", fallbackBlobPath.ToDebugString(), GetLastError() );
	}
#endif

	crashData.primaryBlobModifiedDate.Set( primaryBlobModifiedDate );
	crashData.fallbackBlobModifiedDate.Set( fallbackBlobModifiedDate );

	if ( !LoadScripts( primaryBlobPath, primaryBlobModifiedDate, crashData ) )
	{
		if ( !LoadScripts( fallbackBlobPath, fallbackBlobModifiedDate, crashData ) )
		{
			crashData.blobPathUsed.Set( "None" );
			crashData.scriptsLoaded.Set( false );
		}
	}

	return m_scriptsLoaded;
}

Bool CBaseEngine::LoadScripts( const red::AbsolutePath& blobPath, Uint64 blobModifiedDate, dd::ScriptsCrashData& crashData )
{
#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.blobPathUsed.Set( blobPath.AsString() );
#else
	crashData.blobPathUsed.Set( "Loaded" );
#endif

	if ( !blobModifiedDate )
	{
		RED_LOG_ERROR( "[Scripts] Failed to load script blob from %s. File does not exist.", blobPath.ToDebugString() );
		crashData.scriptsLoaded.Set( m_scriptsLoaded );
		return m_scriptsLoaded;
	}

	if ( m_blobFileModifiedDate < blobModifiedDate )
	{
		RED_LOG_INFO( "[Scripts] Trying to load script blob from %s (new blob modified date=%llu, old blob modified date=%llu)",
			blobPath.ToDebugString(), blobModifiedDate, m_blobFileModifiedDate );
		m_scriptsLoaded = ScriptsHelper::Load( blobPath, ScriptsSilentValidation(), m_scriptsValidationErrors, 10 );
		m_blobFileModifiedDate = blobModifiedDate;
	}

	crashData.scriptsLoaded.Set( m_scriptsLoaded );
	return m_scriptsLoaded;
}

Bool CBaseEngine::ReloadScripts( const red::AbsolutePath& blobPath, Uint64 blobModifiedDate, dd::ScriptsCrashData& crashData )
{
#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.blobPathUsed.Set( blobPath.AsString() );
#else
	crashData.blobPathUsed.Set( "Reloaded" );
#endif

	if ( !blobModifiedDate )
	{
		crashData.scriptsLoaded.Set( m_scriptsLoaded );
		return m_scriptsLoaded;
	}

	if ( m_blobFileModifiedDate < blobModifiedDate )
	{
		RED_LOG_INFO( "[Scripts] Trying to reload script blob from %s (new blob modified date=%llu, old blob modified date=%llu)",
			blobPath.ToDebugString(), blobModifiedDate, m_blobFileModifiedDate );
		m_scriptsLoaded = ScriptsHelper::Load( blobPath, ScriptsSilentValidation(), m_scriptsValidationErrors, 10 );
		m_blobFileModifiedDate = blobModifiedDate;
	}

	crashData.scriptsLoaded.Set( m_scriptsLoaded );
	return m_scriptsLoaded;
}

void CBaseEngine::CompileScripts( const red::AbsolutePath& blobPath, dd::ScriptsCrashData& crashData, script::ScriptVersion scriptVersion )
{
	RED_LOG_INFO( "[Scripts] Trying to compile script blob (%s)", blobPath.ToDebugString() );

	Bool shouldCompileScripts = true;

#if defined( RED_FILE_SYNC_SERVICE_ENABLED )
	if ( FileSyncService::IsClient() )
	{
		shouldCompileScripts = false; // Clients don't compile scripts, they download compiled scripts from the server
	}
#endif

	crashData.shouldCompileScripts.Set( shouldCompileScripts );

	if ( !shouldCompileScripts )
	{
		RED_LOG_INFO( "[Scripts] Script blob compilation is disabled" );
		return;
	}

	// #tmp redIsInternalVersion is broken on the BM
	const char* lastChange = APP_LAST_P4_CHANGE;
	const Bool isInternal = red::Strcmp( lastChange, "CL_INTERNAL" ) == 0;
	if ( !Config::cvScriptsEnableCompilation.Get() && !isInternal )
	{
		RED_LOG_INFO( "[Scripts] Script blob compilation is disabled" );
		return;
	}

	red::AbsolutePath source = ScriptsHelper::GetScriptSourceDirectory();
	if ( !ScriptsHelper::Compile( source, blobPath, scriptVersion, ScriptsSilentCompilation(), m_scriptsCompilationErrors ) )
	{
		RED_LOG_INFO( "[Scripts] Failed to compile script blob (%s)", blobPath.ToDebugString() );
		crashData.compileScriptsSuccess.Set( false );
		return;
	}

	RED_LOG_INFO( "[Scripts] Compiled script blob (%s)", blobPath.ToDebugString() );
	crashData.compileScriptsSuccess.Set( true );
}

void CBaseEngine::CheckForUpdatedScriptsBlob( const Float timeDelta, Bool forceUpdate /*= false*/ )
{
	m_timeSinceLastScriptFileCheck += timeDelta;

	// Since we're polling a file's modification date, let's make sure we don't do it more than once a second
	if ( m_timeSinceLastScriptFileCheck >= 1.0f || forceUpdate )
	{
		m_timeSinceLastScriptFileCheck = 0.0f;

		if ( !m_scriptBlobPath.Empty() )
		{
			ReloadCustomScriptsBlob();
		}
		else
		{
			ReloadDefaultScriptsBlob();
		}
	}
}

void CBaseEngine::ReloadCustomScriptsBlob()
{
	auto& crashData = dd::GetScriptsCrashData();
#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.primaryBlobPath.Set( m_scriptBlobPath );
#endif

	Uint64 primaryBlobModifiedDate = GFileManager->GetFileTimeStamp( m_scriptBlobPath );
	crashData.primaryBlobModifiedDate.Set( primaryBlobModifiedDate );

	if ( !ReloadScripts( m_scriptBlobPath, primaryBlobModifiedDate, crashData ) )
	{
		crashData.blobPathUsed.Set( "None" );
		crashData.scriptsLoaded.Set( false );
	}
}

void CBaseEngine::ReloadDefaultScriptsBlob()
{
	const char* blobName = GetBlobName( m_scriptVersion );
	red::AbsolutePath primaryBlobPath = ScriptsHelper::GetCompileBlobPath().AddFilePath( blobName );
	red::AbsolutePath fallbackBlobPath = GFileManager->GetEngineRoot().AddDirPath( Config::cvEngineFallbackCacheDirectory.Get() ).AddFilePath( blobName );

	auto& crashData = dd::GetScriptsCrashData();
#ifdef SCRIPT_BLOB_PATH_IN_CRASH_DATA
	crashData.primaryBlobPath.Set( primaryBlobPath );
	crashData.fallbackBlobPath.Set( fallbackBlobPath );
#endif

	Uint64 primaryBlobModifiedDate = GFileManager->GetFileTimeStamp( primaryBlobPath );
	Uint64 fallbackBlobModifiedDate = GFileManager->GetFileTimeStamp( fallbackBlobPath );

	crashData.primaryBlobModifiedDate.Set( primaryBlobModifiedDate );
	crashData.fallbackBlobModifiedDate.Set( fallbackBlobModifiedDate );

	if ( !ReloadScripts( primaryBlobPath, primaryBlobModifiedDate, crashData ) )
	{
		if ( !ReloadScripts( fallbackBlobPath, fallbackBlobModifiedDate, crashData ) )
		{
			crashData.blobPathUsed.Set( "None" );
			crashData.scriptsLoaded.Set( false );
		}
	}
}

Bool CBaseEngine::InitializeStreamingManager( bool isCmdlet )
{
	auto streamingManager = GetStreamingResourceManager();
	if ( streamingManager )
	{
		if ( !streamingManager->Load( isCmdlet ) )
		{
			RED_LOG_WARNING( "Engine: failed to load streaming cache." );
		}
	}

	return true;
}

Bool CBaseEngine::ProcessBaseFrame( const Float engineTimeDelta, const tick::Info& tickInfo, input::IInputContext& input )
{
	return false;
}

Bool CBaseEngine::ProcessFrame( const Float scaledEngineTimeDelta, const tick::Info& tickInfo, input::IInputContext& input )
{
	return false;
}

void CBaseEngine::UpdateLoggingDebugFilters()
{
	static rend::DebugFilter DF_LogCategory_AI( "Log/AI" );
	static rend::DebugFilter DF_LogCategory_Animation( "Log/Animation" );
	static rend::DebugFilter DF_LogCategory_Audio( "Log/Audio" );
	static rend::DebugFilter DF_LogCategory_Core( "Log/Core" );
	static rend::DebugFilter DF_LogCategory_Default( "Log/Default" );
	static rend::DebugFilter DF_LogCategory_Engine( "Log/Engine" );
	static rend::DebugFilter DF_LogCategory_GameStateMachine( "Log/GameStateMachine" );
	static rend::DebugFilter DF_LogCategory_Gameplay( "Log/Gameplay" );
	static rend::DebugFilter DF_LogCategory_GameplayError( "Log/GameplayError" );
	static rend::DebugFilter DF_LogCategory_Input( "Log/Input" );
	static rend::DebugFilter DF_LogCategory_Interop( "Log/Interop" );
	static rend::DebugFilter DF_LogCategory_ItemFactory( "Log/ItemFactory" );
	static rend::DebugFilter DF_LogCategory_Localization( "Log/Localization" );
	static rend::DebugFilter DF_LogCategory_Math( "Log/Math" );
	static rend::DebugFilter DF_LogCategory_ObjectPool( "Log/ObjectPool" );
	static rend::DebugFilter DF_LogCategory_Physics( "Log/Physics" );
	static rend::DebugFilter DF_LogCategory_Rendering( "Log/Rendering" );
	static rend::DebugFilter DF_LogCategory_Scripts( "Log/Scripts" );
	static rend::DebugFilter DF_LogCategory_ScriptRuntimeErrors( "Log/ScriptRuntimeErrors" );
	static rend::DebugFilter DF_LogCategory_Tools( "Log/Tools" );
	static rend::DebugFilter DF_LogCategory_UserInterface( "Log/UserInterface" );
	static rend::DebugFilter DF_LogCategory_PlayerLocomotion( "Log/PlayerLocomotion" );
	static rend::DebugFilter DF_LogCategory_Navigation( "Log/Navigation" );
	static rend::DebugFilter DF_LogCategory_Multiplayer( "Log/Multiplayer" );
	static rend::DebugFilter DF_LogCategory_Effects( "Log/Effects" );
	static rend::DebugFilter DF_LogCategory_Resources( "Log/Resources" );
	static rend::DebugFilter DF_LogCategory_Scanning( "Log/Scanning" );
	static rend::DebugFilter DF_LogCategory_Interactions( "Log/Interactions" );
	static rend::DebugFilter DF_LogCategory_Muppet( "Log/Muppet" );
	static rend::DebugFilter DF_LogCategory_PingSystem( "Log/PingSystem" );
	static rend::DebugFilter DF_LogCategory_Chatter( "Log/Chatter" );
	static rend::DebugFilter DF_LogCategory_AIDirector( "Log/AIDirector" );
	static rend::DebugFilter DF_LogCategory_PlayerBreadcrumbs( "Log/PlayerBreadcrumbs" );
	static rend::DebugFilter DF_LogCategory_FunctionalTests("Log/FunctionalTests");
	static rend::DebugFilter DF_LogCategory_GPS( "Log/GPS" );
	static rend::DebugFilter DF_LogCategory_PlayerManager( "Log/PlayerManager" );
	static rend::DebugFilter DF_LogCategory_Population( "Log/Population" );
	static rend::DebugFilter DF_LogCategory_GameplayProfile( "Log/GameplayProfile" );
	static rend::DebugFilter DF_LogCategory_Mounting( "Log/Mounting" );
	static rend::DebugFilter DF_LogCategory_Projectiles( "Log/Projectiles" );
	static rend::DebugFilter DF_LogCategory_Telemetry( "Log/Telemetry" );
	static rend::DebugFilter DF_LogCategory_Garment( "Log/Garment" );

	const auto game_mask = rend::DebugFilterMask::GetFilterMask( rend::GetGameFilterName() );

	red::EnableLogCategory( red::LoggerCategory_AI, game_mask.Test( DF_LogCategory_AI ));
	red::EnableLogCategory( red::LoggerCategory_Animation, game_mask.Test( DF_LogCategory_Animation ));
	red::EnableLogCategory( red::LoggerCategory_Audio, game_mask.Test( DF_LogCategory_Audio ));
	red::EnableLogCategory( red::LoggerCategory_Core, game_mask.Test( DF_LogCategory_Core ));
	red::EnableLogCategory( red::LoggerCategory_Default, game_mask.Test( DF_LogCategory_Default ));
	red::EnableLogCategory( red::LoggerCategory_Engine, game_mask.Test( DF_LogCategory_Engine ));
	red::EnableLogCategory( red::LoggerCategory_GameStateMachine, game_mask.Test( DF_LogCategory_GameStateMachine ));
	red::EnableLogCategory( red::LoggerCategory_Gameplay, game_mask.Test( DF_LogCategory_Gameplay ));
	red::EnableLogCategory( red::LoggerCategory_TweakDB, game_mask.Test( DF_LogCategory_GameplayError ));
	red::EnableLogCategory( red::LoggerCategory_Input, game_mask.Test( DF_LogCategory_Input ));
	red::EnableLogCategory( red::LoggerCategory_ItemFactory, game_mask.Test( DF_LogCategory_ItemFactory ));
	red::EnableLogCategory( red::LoggerCategory_Localization, game_mask.Test( DF_LogCategory_Localization ));
	red::EnableLogCategory( red::LoggerCategory_Math, game_mask.Test( DF_LogCategory_Math ));
	red::EnableLogCategory( red::LoggerCategory_ObjectPool, game_mask.Test( DF_LogCategory_ObjectPool ));
	red::EnableLogCategory( red::LoggerCategory_Physics, game_mask.Test( DF_LogCategory_Physics ));
	red::EnableLogCategory( red::LoggerCategory_Rendering, game_mask.Test( DF_LogCategory_Rendering ));
	red::EnableLogCategory( red::LoggerCategory_Scripts, game_mask.Test( DF_LogCategory_Scripts ));
	red::EnableLogCategory( red::LoggerCategory_ScriptRuntimeErrors, game_mask.Test( DF_LogCategory_ScriptRuntimeErrors ));
	red::EnableLogCategory( red::LoggerCategory_Tools, game_mask.Test( DF_LogCategory_Tools ) );
	red::EnableLogCategory( red::LoggerCategory_UserInterface, game_mask.Test( DF_LogCategory_UserInterface ) );
	red::EnableLogCategory( red::LoggerCategory_PlayerLocomotion, game_mask.Test( DF_LogCategory_PlayerLocomotion ) );
	red::EnableLogCategory( red::LoggerCategory_Navigation, game_mask.Test( DF_LogCategory_Navigation ) );
	red::EnableLogCategory( red::LoggerCategory_Multiplayer, game_mask.Test( DF_LogCategory_Multiplayer ) );
	red::EnableLogCategory( red::LoggerCategory_Effects, game_mask.Test( DF_LogCategory_Effects ) );
	red::EnableLogCategory( red::LoggerCategory_Resources, game_mask.Test( DF_LogCategory_Resources ) );
	red::EnableLogCategory( red::LoggerCategory_Scanning, game_mask.Test( DF_LogCategory_Scanning ) );
	red::EnableLogCategory( red::LoggerCategory_Interactions, game_mask.Test( DF_LogCategory_Interactions ) );
	red::EnableLogCategory( red::LoggerCategory_Muppet, game_mask.Test( DF_LogCategory_Muppet ) );
	red::EnableLogCategory( red::LoggerCategory_PingSystem, game_mask.Test( DF_LogCategory_PingSystem ) );
	red::EnableLogCategory( red::LoggerCategory_Chatter, game_mask.Test( DF_LogCategory_Chatter ) );
	red::EnableLogCategory( red::LoggerCategory_AIDirector, game_mask.Test( DF_LogCategory_AIDirector ) );
	red::EnableLogCategory( red::LoggerCategory_PlayerBreadcrumbs, game_mask.Test( DF_LogCategory_PlayerBreadcrumbs ) );
	red::EnableLogCategory( red::LoggerCategory_FunctionalTests, game_mask.Test( DF_LogCategory_FunctionalTests ));
	red::EnableLogCategory( red::LoggerCategory_GPS, game_mask.Test( DF_LogCategory_GPS ) );
	red::EnableLogCategory( red::LoggerCategory_PlayerManager, game_mask.Test( DF_LogCategory_PlayerManager ) );
	red::EnableLogCategory( red::LoggerCategory_Population, game_mask.Test( DF_LogCategory_Population ));
	red::EnableLogCategory( red::LoggerCategory_GameplayProfile, game_mask.Test( DF_LogCategory_GameplayProfile ));
	red::EnableLogCategory( red::LoggerCategory_Mounting, game_mask.Test( DF_LogCategory_Mounting ) );
	red::EnableLogCategory( red::LoggerCategory_Projectiles, game_mask.Test( DF_LogCategory_Projectiles ) );
	red::EnableLogCategory( red::LoggerCategory_Telemetry, game_mask.Test( DF_LogCategory_Telemetry) );
	red::EnableLogCategory( red::LoggerCategory_Garment, game_mask.Test( DF_LogCategory_Garment ) );

	static rend::DebugFilter DF_Localization_logQueries( "Localization/LogLocalizationQueries" );
	if ( m_localizationManager != nullptr )
	{
		m_localizationManager->LogQueries( game_mask.Test( DF_Localization_logQueries ) );
	}
}

const StreamingTimeDilationState& CBaseEngine::GetStreamingTimeDilationState() const
{
    return m_streamingTimeDilationState;
}

const StreamingStats& CBaseEngine::GetStreamingStats() const
{
	return m_streamingStats;
}

void CBaseEngine::SetStreamingStats( const StreamingStats& stats )
{
	m_streamingStats = stats;
}

SceneEntityStats& CBaseEngine::GetSceneEntityStats()
{
	return m_sceneEntityStats;
}

const DynEntityStats& CBaseEngine::GetDynEntityStats() const
{
	return m_dynEntityStats;
}

void CBaseEngine::SetDynEntityStats( const DynEntityStats& stats )
{
	m_dynEntityStats = stats;
}

void CBaseEngine::FinalizeSoundSystemInit()
{
	return m_soundSystem->FinalizeInit(GetLocalizationManager());
}

red::SharedPtr< interop::utils::Object > CBaseEngine::GetInteropRoot() const
{
	return red::SharedPtr< interop::utils::Object >();
}

void CBaseEngine::OnKeyDown(Uint32 key)
{
#ifdef USE_RED_INGAME_PROFILER
	m_inputSystem->OnKeyDown(key);
#endif
}

void CBaseEngine::OnKeyUp(Uint32 key)
{
#ifdef USE_RED_INGAME_PROFILER
	m_inputSystem->OnKeyUp(key);
#endif
}

#ifdef USE_RED_INGAME_PROFILER
red::InGameCPUProfilerFrontEnd* CBaseEngine::GetInGameProfilerFrontEnd()
{
	return m_inGameProfilerFrontEnd.Get();
}
red::InGameChromeTracingProfiler* CBaseEngine::GetInGameChromeTracingProfiler()
{
	return m_inGameChromeTracingProfiler.Get();
}
#endif

#if 0
void CBaseEngine::RefreshEngine()
{
	RED_FATAL_ASSERT( ::SIsMainThread(), "Engine refresh should be called from main thread." );

	// Reload rendering shit that may depend on performance profile
	if ( GRender )
	{
		GRender->ReloadTextures();
		GRender->RecreateShadowmapResources();
	}

	// Recalculate max mip for streaming, so render textures already streamed in are refreshed
	GRender->RecalculateTextureStreamingSettings();
}

void CBaseEngine::RequestRefreshViewport()
{
#ifdef RED_PLATFORM_WINPC
	Uint32 width;
	Uint32 height;
	EViewportWindowMode windowMode = (EViewportWindowMode)Config::cvFullScreenMode.Get();

	RED_FATAL_ASSERT( GRender, "Renderer expected for 'ExtractDisplayModeValues' fallback" );
	if ( !Config::Helper::ExtractDisplayModeValues( Config::cvResolution.Get(), width, height ) )
	{
		GRender->GetNativeResolution( width, height );
	}

	Int32 bestWidth = width;
	Int32 bestHeight = height;

	RED_FATAL_ASSERT( GRender, "Renderer expected in case of fullscreen mode" );
	if( windowMode == VWM_Fullscreen )
	{
		GRender->GetBestMatchingResolution( width, height, bestWidth, bestHeight );

		// If best resolution is different than set resolution, this means we have to update config
		// this happens in situation like switching to monitor with different resolutions support
		if( bestWidth != width || bestHeight != height )
		{
			Config::cvResolution.Set( Config::Helper::PackDisplayModeValues( bestWidth, bestHeight ) );
		}
	}

	RED_MESSAGE( "Hacky viewport refresh" );
	/*
	{
		legacy_viewport->RequestWindowSize( bestWidth, bestHeight );
		legacy_viewport->RequestWindowMode( windowMode );
		legacy_viewport->RequestOutputMonitor( GRender->GetCurrentOutputMonitorConfig() );
	}*/

	if( windowMode == VWM_Fullscreen )
	{
		InGameConfig::GetInGameConfig().ActivateTag(RED_NAME_CONSTEXPR(fullscreen));
	}
	else
	{
		InGameConfig::GetInGameConfig().DeactivateTag(RED_NAME_CONSTEXPR(fullscreen));
	}
#endif
}
#endif

