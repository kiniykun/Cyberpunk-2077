#!/usr/bin/env python3
"""[w32-lean11] mem=ios: the Wwise bank manager has no thread of its own. Its queue is pumped by the AK IO hub thread
(patch-lean9). A blocking stream read issued from the hub thread (bank loading) services the stream devices itself
while it waits, so the hub never deadlocks on its own IO. 5 -> 4 pthreads. Falls back to the real bank thread if the
hub is not running yet. Run after patch-lean9-w32.py. Keyed by ORIGINAL depot paths."""
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

patch(WW + r'\StreamManager\POSIX\AkIOThread.cpp', [
 ('''	void CpHubKick()
	{
		if ( !s_cpHubMode ) return;
		pthread_mutex_lock( &s_cpHubMutex );
		s_cpHubKick = true;
		pthread_cond_broadcast( &s_cpHubCond );
		pthread_mutex_unlock( &s_cpHubMutex );
	}
}''', '''	void CpHubKick()
	{
		if ( !s_cpHubMode ) return;
		pthread_mutex_lock( &s_cpHubMutex );
		s_cpHubKick = true;
		pthread_cond_broadcast( &s_cpHubCond );
		pthread_mutex_unlock( &s_cpHubMutex );
	}
	void ( *volatile s_cpHubPump )() = nullptr; // [w32-lean11] bank-manager queue pump run by the hub thread
	pthread_t s_cpHubSelf;
	volatile bool s_cpHubSelfValid = false;
}
extern "C" void CpWwiseHubSetPump( void ( *pump )() ) { s_cpHubPump = pump; CpHubKick(); } // [w32-lean11]
extern "C" void CpWwiseHubKick() { CpHubKick(); }
extern "C" int CpWwiseHubActive() { return s_cpHubMode && AKPLATFORM::AkIsValidThread( &s_cpHubThread ) ? 1 : 0; }'''),
 ('''	if ( s_cpHubMode ) // [w32-lean9] hub loop over every registered device
	{
		for ( ;; )
		{''', '''	if ( s_cpHubMode ) // [w32-lean9] hub loop over every registered device
	{
		s_cpHubSelf = pthread_self(); s_cpHubSelfValid = true; // [w32-lean11]
		for ( ;; )
		{
			if ( void ( *pump )() = s_cpHubPump ) pump(); // [w32-lean11] bank-manager commands'''),
 ('''	AKVERIFY( pthread_mutex_lock( &m_mutexBlockingIO ) == 0 );

	// Now that we aquired the mutex, check if the task is still waiting for I/O (its thread ID would be NULL).''', '''	if ( s_cpHubMode && s_cpHubSelfValid && pthread_equal( pthread_self(), s_cpHubSelf ) ) // [w32-lean11] the hub waits on its own IO:
	{                                                                                     // service the devices until the transfer completes
		for ( ;; )
		{
			AKVERIFY( pthread_mutex_lock( &m_mutexBlockingIO ) == 0 );
			const bool waiting = in_pWaitingTask->BlockedThreadID() != AK_NULL_THREAD;
			AKVERIFY( pthread_mutex_unlock( &m_mutexBlockingIO ) == 0 );
			if ( !waiting ) return;
			CAkIOThread* devs[ c_cpHubMax ]; int n = 0;
			pthread_mutex_lock( &s_cpHubMutex );
			for ( int i = 0; i < s_cpHubN; ++i ) devs[ n++ ] = s_cpHubDevs[ i ];
			pthread_mutex_unlock( &s_cpHubMutex );
			bool busy = false;
			for ( int i = 0; i < n; ++i )
			{
				CAkIOThread* d = devs[ i ];
				d->Lock();
				const bool can = d->m_bDoRun && d->CanSchedule();
				d->Unlock();
				if ( can ) { d->PerformIO(); busy = true; }
			}
			if ( !busy ) AKPLATFORM::AkSleep( 1 );
		}
	}
	AKVERIFY( pthread_mutex_lock( &m_mutexBlockingIO ) == 0 );

	// Now that we aquired the mutex, check if the task is still waiting for I/O (its thread ID would be NULL).'''),
], '[w32-lean11]')

patch(WW + r'\SoundEngine\AkAudiolib\Common\AkThreadedBankMgr.cpp', [
 ('''AKRESULT CAkThreadedBankMgr::StartThread()
{''', '''extern "C" int CpWebMemIos(); // [w32-lean11]
extern "C" void CpWwiseHubSetPump( void ( *pump )() );
extern "C" void CpWwiseHubKick();
extern "C" int CpWwiseHubActive();
static CAkThreadedBankMgr* s_cpHubBankMgr = nullptr;
AKRESULT CAkThreadedBankMgr::StartThread()
{
	if ( CpWebMemIos() && CpWwiseHubActive() && !AkIsValidThread( &m_BankMgrThread ) ) // [w32-lean11] pumped by the AK IO hub
	{
		m_bStopThread = false;
		if ( AkCreateEvent( m_eventQueue ) != AK_Success ) return AK_Fail;
		s_cpHubBankMgr = this;
		CpWwiseHubSetPump( []()
		{
			CAkThreadedBankMgr* mgr = s_cpHubBankMgr;
			if ( !mgr || mgr->m_bStopThread ) return;
			m_idThread = AKPLATFORM::CurrentThread();
			mgr->ExecuteCommand();
		} );
		return AK_Success;
	}'''),
 ('''void CAkThreadedBankMgr::StopThread()
{
	m_bStopThread = true;''', '''void CAkThreadedBankMgr::StopThread()
{
	m_bStopThread = true;
	if ( s_cpHubBankMgr == this ) { CpWwiseHubSetPump( nullptr ); s_cpHubBankMgr = nullptr; } // [w32-lean11]'''),
 ('''		BankMonitorNotification( in_Item );

		AkSignalEvent( m_eventQueue );''', '''		BankMonitorNotification( in_Item );

		AkSignalEvent( m_eventQueue );
		if ( s_cpHubBankMgr == this ) CpWwiseHubKick(); // [w32-lean11]'''),
], '[w32-lean11]')
