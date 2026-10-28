/**
* Copyright (c) 2016 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"

#ifdef RED_PLATFORM_ORBIS
# include <coredump.h>
# include <system_service.h>
#endif

#include "../../redSystem/include/errorHandler.h"
#include "../../redSystem/include/dbgUtils.h"
#include "../../redSystem/include/redThreadsThread.h"
#include "../../redSystem/include/redThreadsAtomic.h"
#include "../../redReflection/include/scriptingSystem.h"
#include "../../redJobs2/include/jobSystem.h"

#include "engineWatchdog.h"
#include "../../renderData/include/renderer.h"
#include "../../redSystem/include/platformUtilsWin32.h"

namespace dd
{
	red::CrashData<CName> forceKillReason{ "Engine/Watchdog", "ForceKillReason" };
}

namespace Config
{
	TConfigVar< Bool >												cvWatchdogEnabled( "Engine/Watchdog", "Enabled", true );
	TConfigVar< Bool >												cvWatchdogKillProcess( "Engine/Watchdog", "KillProcess", true );
	TConfigVar< Bool >												cvWatchdogActiveIfDebuggerPresent( "Engine/Watchdog", "ActiveIfDebuggerPresent", true );
	TConfigVar< Bool >												cvWatchdogActiveIfDialogBlocking( "Engine/Watchdog", "ActiveIfDialogBlocking", false );
	TConfigVar< Bool >												cvWatchdogActiveIfScriptBreakpointBlocking( "Engine/Watchdog", "ActiveIfScriptBreakpointBlocking", false );
	TConfigVar< Bool >												cvWatchdogDumpJobExecutionContext( "Engine/Watchdog", "DumpJobExecutionContext", true );
	TConfigVar< Int32 >												cvWatchdogThreadFrequencyHz( "Engine/Watchdog", "ThreadFrequencyHz", 15, 1, 60, eConsoleVarFlag_Save );
	
	// #tbd: stricter timeouts for various engines, or move config var into game engine cpp and pass in as argument
	// For Final game consider shorter watchdog. For Release and Debug we need longer time.
#if defined( RED_CONFIGURATION_FINAL )
	constexpr Uint32 c_defaultWatchdogTimeoutSeconds = 2 * 60;
#else
	constexpr Uint32 c_defaultWatchdogTimeoutSeconds = 5 * 60;
#endif
	TConfigVar< Int32 > cvWatchdogTimeoutSeconds( "Engine/Watchdog", "TimeoutSeconds", c_defaultWatchdogTimeoutSeconds, 1, 60*60*24, eConsoleVarFlag_Save );
}

class CWatchdogThread : public red::Thread
{
	RED_USE_MEMORY_POOL( red::PoolEngine );

public:
	explicit CWatchdogThread( const EngineWatchdogSetup& setup );

	virtual void ThreadFunc() override;

	void ForceKill();
	void Kick();
	void Shutdown();

public:
	void Suspend();
	void Resume();

private:
	void OnCheckStatus();
	void PrintTimeoutMessage();
	void KillProcess();

private:
	red::Atomic< Bool >				m_exit;
	red::Atomic< Bool >				m_forceKill;
	EngineWatchdogSetup				m_setup;
	Uint64							m_accumMsec;
};

static const Uint32 WatchDogThreadMemKB = 128;

CWatchdogThread::CWatchdogThread( const EngineWatchdogSetup& setup )
	: red::Thread( "WatchdogThread", red::ThreadMemParams( WatchDogThreadMemKB * 1024 ) )
	, m_exit( false )
	, m_setup( setup )
	, m_accumMsec( 0 )
{
}

void CWatchdogThread::ThreadFunc()
{
	// #tbd: especially on consoles, don't restrict priority if given a strict affinity
	// or else may never run and defeat the purpose of a watchdog thread
	m_accumMsec = 0;
	for(;;)
	{
		const red::TTimespec threadPeriod = 1000 / Config::cvWatchdogThreadFrequencyHz.Get();
		red::SleepOnCurrentThread( threadPeriod );
		if ( m_exit.GetValue() )
			break;

		// Should sleep for at *least* this long in practice, so shouldn't poll more much more frequently than the timeslice quantum
		m_accumMsec += threadPeriod;
		if ( m_accumMsec > 1000 * Config::cvWatchdogTimeoutSeconds.Get() )
		{
			OnCheckStatus();
			m_accumMsec = 0;
		}
	}
}

void CWatchdogThread::ForceKill()
{
	m_forceKill.SetValue( true );
	m_accumMsec = 1000 * Config::cvWatchdogTimeoutSeconds.Get( );//force check inside thread
}

void CWatchdogThread::Shutdown()
{
	m_exit.SetValue( true );
	JoinThread();
}

void CWatchdogThread::Kick()
{
	//force kill has higher priority
	if( m_forceKill.GetValue() == false )
		m_accumMsec = 0;
}

void CWatchdogThread::OnCheckStatus()
{
	if ( !Config::cvWatchdogEnabled.Get() )
		return;

	if ( IScriptingSystem::GetInstance().IsProcessingBreakpoint() )
	{
		if ( !Config::cvWatchdogActiveIfScriptBreakpointBlocking.Get() )
			return;
	}

	if ( dbgutils::IsDebuggerAttached() )
	{
		if ( !Config::cvWatchdogActiveIfDebuggerPresent.Get() )
			return;
	}

#ifdef RED_PLATFORM_WINPC
	if ( IsShowingEmbeddedWin32Dialog() )
	{
		if ( !Config::cvWatchdogActiveIfDialogBlocking.Get() )
			return;
	}
#endif

	PrintTimeoutMessage();

#ifdef __EMSCRIPTEN__
	// CP2077 web [player-fix]: on slow connections the main thread legitimately waits > 120 s for streamed game data
	// (2026-10-22 player reports: 110 of 134 "abort()" failures were this watchdog during loading). The page loader has
	// its own stall detection, so in the browser the watchdog only logs and never kills the game.
	return;
#endif

	// #tbd: dump on another prespawned thread and detach if doesn't return fast enough (maybe it got hung)
	if ( Config::cvWatchdogDumpJobExecutionContext.Get() )
	{
		RED_LOG_FLUSH();
		{
			job::DumpToLog();
			if ( m_setup.unsafeDumpRenderQueue && GRender )
			{
				GRender->Unsafe_DebugDumpRenderCommandQueue();
			}
		}
		RED_LOG_FLUSH();
	}

	if ( !Config::cvWatchdogKillProcess.Get() )
		return;

	KillProcess();
}

void CWatchdogThread::PrintTimeoutMessage()
{
	// #tbd: more robust logging system, crash handler should try ensuring this
	// Avoiding logging system since it could even be an error in the logging system for the reason we've hung.
#ifdef RED_PLATFORM_ORBIS
	// Try to help ensure there's a log or something interesting to look at
	const char* const msg = "!!! EngineWatchdog timeout!\n";
	::sceKernelWrite( STDERR_FILENO, msg, red::Strlen( msg ) );
#elif defined( RED_PLATFORM_WINPC ) || defined( RED_PLATFORM_DURANGO )
	::OutputDebugString( L"!!! EngineWatchdog timeout!" );
#elif ( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )
	const char* const msg = "!!! EngineWatchdog timeout!\n";
	::write( STDERR_FILENO, msg, red::Strlen( msg ) );
#else
# error Unsupported platform!
#endif
}

void CWatchdogThread::KillProcess()
{
#ifdef RED_PLATFORM_ORBIS
	::sceKernelSync();
#endif
	ALWAYSENABLED_RED_FATAL( "Watchdog timeout! (%d seconds)", Config::cvWatchdogTimeoutSeconds.Get() );
}

CEngineWatchdog::CEngineWatchdog( const EngineWatchdogSetup& setup /*= EngineWatchdogSetup()*/ )
	: m_thread( nullptr )
	, m_setup( setup )
{
	RED_FATAL_ASSERT( ::SIsMainThread() );
	Kick();
}

CEngineWatchdog::~CEngineWatchdog()
{
	RED_FATAL_ASSERT( ::SIsMainThread() );
	ShutdownThread();
}

void CEngineWatchdog::ForceKill(CName reason)
{
	dd::forceKillReason.Set( reason );
	m_thread->ForceKill();
}

void CEngineWatchdog::Kick()
{
	// #tbd: config to allow from other threads
	//RED_FATAL_ASSERT( ::SIsMainThread(), "Don't cheat by kicking from some other thread!!!" );

	if ( Config::cvWatchdogEnabled.Get() )
	{
		if ( !m_thread )
		{
			InitThread();
		}
		m_thread->Kick();
	}
}

void CEngineWatchdog::SetTimeoutSecondsOverride( Uint32 seconds )
{
	// #tbd: don't set cvar, better is if we could just specify temp cvar values on the commandline, old-school style.
	RED_LOG( "Engine: override >>> TimeoutSeconds=%d", seconds);
	Config::cvWatchdogTimeoutSeconds.Set( seconds );
}

void CEngineWatchdog::InitThread()
{
	m_thread.Reset( RED_NEW(CWatchdogThread)( m_setup ) );
	m_thread->InitThread();

#ifdef RED_PLATFORM_CONSOLE
	m_thread->SetAffinityMask( 0x7F );
	m_thread->SetPriority( red::TP_TimeCritical );
#else
	m_thread->SetPriority( red::TP_AboveNormal );
#endif

	RED_LOG( "Engine: CWatchdogThread created:" );
	RED_LOG( "Engine: >>> Enabled=%d", Config::cvWatchdogEnabled.Get());
	RED_LOG( "Engine: >>> KillProcess=%d", Config::cvWatchdogKillProcess.Get());
	RED_LOG( "Engine: >>> ActiveIfDebuggerPresent=%d", Config::cvWatchdogActiveIfDebuggerPresent.Get());
	RED_LOG( "Engine: >>> WatchdogActiveIfScriptBreakpointBlocking=%d", Config::cvWatchdogActiveIfScriptBreakpointBlocking.Get());
	RED_LOG( "Engine: >>> ThreadFrequencyHz=%d", Config::cvWatchdogThreadFrequencyHz.Get());
	RED_LOG( "Engine: >>> TimeoutSeconds=%d", Config::cvWatchdogTimeoutSeconds.Get());
}

void CEngineWatchdog::ShutdownThread()
{
	// #tbd: suspend thread vs killing
	if ( m_thread )
	{
		m_thread->Shutdown();
		m_thread.Reset();
	}
}
