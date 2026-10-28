#!/usr/bin/env python3
"""[w32-lean9] mem=ios: three fewer Wwise pthreads (= three fewer wasm module instances on the phone).
- AK Suspended thread (CAkOutputMgr::Init): only wakes the event consumer while every sink is a dummy; with
  bUseLEngineThread=false (mem=ios) RenderAudio renders inline, so the thread is not created.
- wwise_container_streaming decode thread: the queued opus decodes run inline at the end of the post-render callback
  that queues them (same per-item body, no semaphore).
- AK::IOThread x2 (default + container stream devices): one hub thread serves every device (round robin over
  CanSchedule/PerformIO, woken by every device signal, 20 ms timed fallback).
Overlay copies are keyed by the ORIGINAL depot paths (the compile jobs' inputs)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
WW = r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\external\wwise\source'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag):
    p = copy(orig); assert 'build-w32' in str(p), p
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p)

# 1. AK Suspended thread
patch(WW + r'\SoundEngine\AkAudiolib\Common\AkOutputMgr.cpp', [
 ('''	AKPLATFORM::AkCreateThread(SuspendedThread, NULL, g_PDSettings.threadOutputMgr, &m_hSuspendedThread, "AK Suspended");
	if (!AkIsValidThread(&m_hSuspendedThread))
		return AK_Fail;''', '''	if (g_settings.bUseLEngineThread) // [w32-lean9] inline rendering (mem=ios): no suspended-pump thread
	{
	AKPLATFORM::AkCreateThread(SuspendedThread, NULL, g_PDSettings.threadOutputMgr, &m_hSuspendedThread, "AK Suspended");
	if (!AkIsValidThread(&m_hSuspendedThread))
		return AK_Fail;
	}'''),
], '[w32-lean9]')

# 2. container decode thread -> inline decode
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\soundEngine\src\wwiseContainerStreaming.cpp', [
 ('''bool wwiseContainerStreamingInit( AkThreadProperties* threadProps )
{
	AKPLATFORM::AkCreateThread( wwiseContainerStreamingThreadEntryPoint, nullptr, *threadProps, &s_containerStreamingDecodeThread, "wwise_container_streaming" );
	return true;
}

void wwiseContainerStreamingTerm()
{
	containerThreadIsShuttingDown = true;
	containerOpenFileDecodeSemaphore.Release();
	AKPLATFORM::AkWaitForSingleThread( &s_containerStreamingDecodeThread );
	AKPLATFORM::AkCloseThread( &s_containerStreamingDecodeThread );
}''', '''static bool s_cpInlineDecode = false; // [w32-lean9] mem=ios: decodes run inline in the post-render callback
static Uint64 s_cpInlineDecodeCount = 0;
bool wwiseContainerStreamingInit( AkThreadProperties* threadProps )
{
	if ( ::CpWebMemIos() ) { s_cpInlineDecode = true; return true; } // [w32-lean9] no decode thread
	AKPLATFORM::AkCreateThread( wwiseContainerStreamingThreadEntryPoint, nullptr, *threadProps, &s_containerStreamingDecodeThread, "wwise_container_streaming" );
	return true;
}

void wwiseContainerStreamingTerm()
{
	containerThreadIsShuttingDown = true;
	if ( s_cpInlineDecode ) return; // [w32-lean9]
	containerOpenFileDecodeSemaphore.Release();
	AKPLATFORM::AkWaitForSingleThread( &s_containerStreamingDecodeThread );
	AKPLATFORM::AkCloseThread( &s_containerStreamingDecodeThread );
}'''),
 ('''	if( openFileDecodesCreated )
	{
		containerOpenFileDecodeSemaphore.Release( openFileDecodesCreated );
	}
}''', '''	if( openFileDecodesCreated && s_cpInlineDecode ) // [w32-lean9] decode now, on this thread
	{
		wwiseContainerStreamingThreadEntryPoint( ( void* )1 );
	}
	else if( openFileDecodesCreated )
	{
		containerOpenFileDecodeSemaphore.Release( openFileDecodesCreated );
	}
}'''),
 ('''	PROFILER_InitThread( "WwiseDecode", 16384 );
	Uint64 currentOpenFileDecodeCount = 0;
	for ( ; ; )
	{
		containerOpenFileDecodeSemaphore.Acquire();
		if ( containerThreadIsShuttingDown )
			return 0;''', '''	const bool cpInline = param == ( void* )1; // [w32-lean9] inline pass: drain queued decodes, then return
	if ( !cpInline ) PROFILER_InitThread( "WwiseDecode", 16384 );
	Uint64 cpThreadDecodeCount = 0;
	Uint64& currentOpenFileDecodeCount = cpInline ? s_cpInlineDecodeCount : cpThreadDecodeCount;
	for ( ; ; )
	{
		if ( cpInline ) { if ( currentOpenFileDecodeCount >= containerOpenFileDecodeNum ) return 0; }
		else containerOpenFileDecodeSemaphore.Acquire();
		if ( containerThreadIsShuttingDown )
			return 0;'''),
], '[w32-lean9]')

# 3. one IO hub thread for every stream device
patch(WW + r'\StreamManager\POSIX\AkIOThread.cpp', [
 ('''using namespace AK::StreamMgr;''', '''using namespace AK::StreamMgr;

// [w32-lean9] mem=ios: one hub thread serves every stream device (each device used to own an AK::IOThread).
extern "C" int CpWebMemIos();
namespace
{
	const int c_cpHubMax = 8;
	CAkIOThread* s_cpHubDevs[ c_cpHubMax ];
	bool s_cpHubStarted[ c_cpHubMax ];
	int s_cpHubN = 0;
	bool s_cpHubMode = false;
	bool s_cpHubKick = false;
	AkThread s_cpHubThread = AK_NULL_THREAD;
	pthread_mutex_t s_cpHubMutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t s_cpHubCond = PTHREAD_COND_INITIALIZER;
	void CpHubKick()
	{
		if ( !s_cpHubMode ) return;
		pthread_mutex_lock( &s_cpHubMutex );
		s_cpHubKick = true;
		pthread_cond_broadcast( &s_cpHubCond );
		pthread_mutex_unlock( &s_cpHubMutex );
	}
}'''),
 ('''	m_threadProperties = in_threadProperties;
	m_bDoRun = true;
	AKPLATFORM::AkCreateThread(''', '''	m_threadProperties = in_threadProperties;
	m_bDoRun = true;
	if ( CpWebMemIos() ) // [w32-lean9] register with the hub; the first device starts the hub thread
	{
		pthread_mutex_lock( &s_cpHubMutex );
		if ( s_cpHubN >= c_cpHubMax ) { pthread_mutex_unlock( &s_cpHubMutex ); return AK_Fail; }
		s_cpHubMode = true;
		s_cpHubDevs[ s_cpHubN ] = this; s_cpHubStarted[ s_cpHubN ] = false; ++s_cpHubN;
		const bool start = !AKPLATFORM::AkIsValidThread( &s_cpHubThread );
		s_cpHubKick = true;
		pthread_cond_broadcast( &s_cpHubCond );
		pthread_mutex_unlock( &s_cpHubMutex );
		if ( start )
		{
			AKPLATFORM::AkCreateThread( CAkIOThread::IOSchedThread, this, in_threadProperties, &s_cpHubThread, "AK::IOThread" );
			if ( !AKPLATFORM::AkIsValidThread( &s_cpHubThread ) ) return AK_Fail;
		}
		return AK_Success;
	}
	AKPLATFORM::AkCreateThread('''),
 ('''	m_bDoRun = false;
	ret = pthread_cond_signal(&m_condAreTasksPending);
	AKASSERT( ret == 0 || ret == EINVAL );
	ret = pthread_mutex_unlock( &m_mutexPendingStmsSem );
	AKASSERT( ret == 0 || ret == EINVAL );
	''', '''	m_bDoRun = false;
	ret = pthread_cond_signal(&m_condAreTasksPending);
	AKASSERT( ret == 0 || ret == EINVAL );
	ret = pthread_mutex_unlock( &m_mutexPendingStmsSem );
	AKASSERT( ret == 0 || ret == EINVAL );
	if ( s_cpHubMode ) // [w32-lean9] wait until the hub has cleared and dropped this device; last one joins it
	{
		for ( ;; )
		{
			pthread_mutex_lock( &s_cpHubMutex );
			bool listed = false;
			for ( int i = 0; i < s_cpHubN; ++i ) listed = listed || s_cpHubDevs[ i ] == this;
			s_cpHubKick = true;
			pthread_cond_broadcast( &s_cpHubCond );
			const bool last = !listed && s_cpHubN == 0;
			pthread_mutex_unlock( &s_cpHubMutex );
			if ( !listed )
			{
				if ( last && AKPLATFORM::AkIsValidThread( &s_cpHubThread ) ) { AKPLATFORM::AkWaitForSingleThread( &s_cpHubThread ); s_cpHubThread = AK_NULL_THREAD; }
				break;
			}
			AKPLATFORM::AkSleep( 5 );
		}
	}
	'''),
 ('''	pDevice->OnThreadStart();
    
    while ( true )
    {''', '''	if ( s_cpHubMode ) // [w32-lean9] hub loop over every registered device
	{
		for ( ;; )
		{
			CAkIOThread* devs[ c_cpHubMax ]; int n = 0;
			pthread_mutex_lock( &s_cpHubMutex );
			for ( int i = 0; i < s_cpHubN; ++i )
			{
				devs[ n++ ] = s_cpHubDevs[ i ];
				if ( !s_cpHubStarted[ i ] ) { s_cpHubStarted[ i ] = true; pthread_mutex_unlock( &s_cpHubMutex ); s_cpHubDevs[ i ]->OnThreadStart(); pthread_mutex_lock( &s_cpHubMutex ); }
			}
			const bool none = s_cpHubN == 0;
			pthread_mutex_unlock( &s_cpHubMutex );
			if ( none )
			{
				AK::MemoryMgr::TermForThread();
				AkExitThread( AK_RETURN_THREAD_OK );
			}
			bool busy = false;
			for ( int i = 0; i < n; ++i )
			{
				CAkIOThread* d = devs[ i ];
				d->Lock();
				const bool stop = !d->m_bDoRun;
				const bool can = !stop && d->CanSchedule();
				d->Unlock();
				if ( stop )
				{
					if ( d->ClearStreams() )
					{
						pthread_mutex_lock( &s_cpHubMutex );
						for ( int k = 0; k < s_cpHubN; ++k )
							if ( s_cpHubDevs[ k ] == d ) { for ( int m = k + 1; m < s_cpHubN; ++m ) { s_cpHubDevs[ m - 1 ] = s_cpHubDevs[ m ]; s_cpHubStarted[ m - 1 ] = s_cpHubStarted[ m ]; } --s_cpHubN; break; }
						pthread_mutex_unlock( &s_cpHubMutex );
					}
					else AKPLATFORM::AkSleep( 10 );
					busy = true;
				}
				else if ( can )
				{
					d->PerformIO();
					busy = true;
				}
			}
			if ( !busy )
			{
				pthread_mutex_lock( &s_cpHubMutex );
				if ( !s_cpHubKick )
				{
					struct timeval now; gettimeofday( &now, NULL );
					struct timespec until; until.tv_sec = now.tv_sec; until.tv_nsec = now.tv_usec * 1000 + 20 * 1000000;
					if ( until.tv_nsec >= 1000000000 ) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
					pthread_cond_timedwait( &s_cpHubCond, &s_cpHubMutex, &until );
				}
				s_cpHubKick = false;
				pthread_mutex_unlock( &s_cpHubMutex );
			}
		}
	}

	pDevice->OnThreadStart();
    
    while ( true )
    {'''),
 ('''	if ( ++m_cPendingStdStms == 1 )
	{
		// We just incremented it from 0 to 1. Signal condition variable.
		AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
	}''', '''	if ( ++m_cPendingStdStms == 1 )
	{
		// We just incremented it from 0 to 1. Signal condition variable.
		AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
		CpHubKick(); // [w32-lean9]
	}'''),
 ('''#ifdef _DEBUG
        m_bIsAutoSemSignaled = true;
#endif
		AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
    }
}

// Decrement pending automatic streams count.''', '''#ifdef _DEBUG
        m_bIsAutoSemSignaled = true;
#endif
		AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
		CpHubKick(); // [w32-lean9]
    }
}

// Decrement pending automatic streams count.'''),
 ('''#ifdef _DEBUG
            m_bIsAutoSemSignaled = true;
#endif
			AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
        }
    }
}''', '''#ifdef _DEBUG
            m_bIsAutoSemSignaled = true;
#endif
			AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
			CpHubKick(); // [w32-lean9]
        }
    }
}'''),
 ('''	--m_uNumConcurrentIO;
	AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );''', '''	--m_uNumConcurrentIO;
	AKVERIFY( pthread_cond_signal( &m_condAreTasksPending ) == 0 );
	CpHubKick(); // [w32-lean9]'''),
], '[w32-lean9]')
