#!/usr/bin/env python3
"""[w32-lean] mem=ios: no engine watchdog thread (its only job is killing a hung process; the browser does that)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
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
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\engine\src\engineWatchdog.cpp', [
 ('''void CEngineWatchdog::ForceKill(CName reason)
{
	dd::forceKillReason.Set( reason );
	m_thread->ForceKill();''', '''extern "C" int CpWebMemIos(); // [w32-lean]
void CEngineWatchdog::ForceKill(CName reason)
{
	dd::forceKillReason.Set( reason );
	if ( m_thread ) m_thread->ForceKill(); // [w32-lean] no thread under mem=ios'''),
 ('''	if ( Config::cvWatchdogEnabled.Get() )
	{
		if ( !m_thread )''', '''	if ( Config::cvWatchdogEnabled.Get() && !CpWebMemIos() ) // [w32-lean]
	{
		if ( !m_thread )'''),
], '[w32-lean]')

# [w32-lean] mem=ios: no network threads (PacketNetworkManager sender + RawTcp); sockets do not exist on the web.
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\redNetwork\src\packetNetworkManager.cpp', [
 ('''		Bool PacketNetworkManager::Initialize()
		{
			m_network->Initialize();''', '''		Bool PacketNetworkManager::Initialize()
		{
			if ( ::CpWebMemIos() ) return true; // [w32-lean] no sender/raw-tcp threads under mem=ios
			m_network->Initialize();'''),
 ('''		void PacketNetworkManager::Shutdown()
		{
			m_network->Shutdown();''', '''		void PacketNetworkManager::Shutdown()
		{
			if ( ::CpWebMemIos() ) { m_connections.Clear(); m_listeners.Clear(); return; } // [w32-lean]
			m_network->Shutdown();'''),
], '[w32-lean]')

# note: packetNetworkManager.cpp also needs  extern "C" int CpWebMemIos();  after #include "rawManager.h" (added to the w32 copy)

# [w32-lean2] mem=ios: Wwise audio processing on the engine thread (RenderAudio) instead of its own LEngine thread.
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\soundEngine\src\soundEngine.cpp', [
 ('''		// Initialize sound engine.
		res = AK::SoundEngine::Init(&initSettings, &platformInitSettings);''', '''		if ( ::CpWebMemIos() ) initSettings.bUseLEngineThread = false; // [w32-lean2] audio rendered in RenderAudio()
		// Initialize sound engine.
		res = AK::SoundEngine::Init(&initSettings, &platformInitSettings);'''),
 ('''#include "build.h"
''', '''#include "build.h"
extern "C" int CpWebMemIos(); // [w32-lean2]
'''),
], '[w32-lean2]')

# [w32-lean2] mem=ios: one Bink async decode thread; the engine's second slot aliases the first.
patch(r'D:\2077\src\bink-engine-web.cpp', [
 ('''S32 RADEXPLINK BinkStartAsyncThread(S32 number,const void* parameter) {
    if(number<0||number>=8||parameter||emscripten_is_main_runtime_thread())return 0;
    std::unique_lock<std::mutex> registry(slotsMutex);
    if(asyncSlots[number])return 0;''', '''extern "C" int CpWebMemIos(); // [w32-lean2]
S32 RADEXPLINK BinkStartAsyncThread(S32 number,const void* parameter) {
    if(number<0||number>=8||parameter||emscripten_is_main_runtime_thread())return 0;
    std::unique_lock<std::mutex> registry(slotsMutex);
    if(asyncSlots[number])return 0;
    if(CpWebMemIos())for(auto& existing:asyncSlots)if(existing){asyncSlots[number]=existing;return 1;} // [w32-lean2] share one thread'''),
], '[w32-lean2]')

# [w32-lean3] mem=ios: game-services requests run inline on the requesting thread (no GameServicesWebWorker pthread).
patch(r'D:\2077\src\game-services-worker-web.h', [
 ('''		virtual void ThreadFunc() override;''', '''		virtual void ThreadFunc() override;
		void RunPendingInline(); // [w32-lean3] mem=ios: execute queued requests on the caller'''),
], '[w32-lean3]')
patch(r'D:\2077\src\game-services-worker-web.cpp', [
 ('''	void GameServicesWebWorker::ThreadFunc()
	{''', '''	void GameServicesWebWorker::RunPendingInline() // [w32-lean3]
	{
		while ( true )
		{
			{
				RED_SCOPE_LOCK( m_requestsLock );
				if ( m_requests.Empty() ) return;
				auto& nextRequest = m_requests.Front();
				m_currentRequest = std::move( nextRequest );
				m_requests.RemoveAt( 0 );
			}
			if ( m_currentRequest )
			{
				m_currentRequest->Execute();
				while ( !m_currentRequest->IsFinished() )
				{
					red::SleepOnCurrentThread( m_currentRequest->GetSleepMs() );
					m_currentRequest->Update();
				}
				m_currentRequest = nullptr;
			}
		}
	}

	void GameServicesWebWorker::ThreadFunc()
	{'''),
], '[w32-lean3]')
def _svc():
    orig = r'D:\2077\src\game-services-web.cpp'
    p = copy(orig); raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if '[w32-lean3]' in s: print('already', p.name); return
    a = '''        m_worker->InitThread();
        m_worker->SetPriority(red::TP_Normal);'''
    assert s.count(a) == 1
    s = s.replace(a, '''        if (!CpWebMemIos()) { m_worker->InitThread(); m_worker->SetPriority(red::TP_Normal); } // [w32-lean3] inline under mem=ios''')
    a = '''            m_worker->JoinThread();'''
    assert s.count(a) == 1
    s = s.replace(a, '''            if (!CpWebMemIos()) m_worker->JoinThread(); // [w32-lean3]''')
    n = s.count('m_wakeUpWorker.WakeAll();')
    s = s.replace('            m_wakeUpWorker.WakeAll();\n            if (!CpWebMemIos()) m_worker->JoinThread();', '@@KEEP@@')
    s = s.replace('m_wakeUpWorker.WakeAll();', 'm_wakeUpWorker.WakeAll(); if (CpWebMemIos() && m_worker) m_worker->RunPendingInline(); /*[w32-lean3]*/')
    s = s.replace('@@KEEP@@', '            m_wakeUpWorker.WakeAll();\n            if (!CpWebMemIos()) m_worker->JoinThread();')
    first = s.find('namespace')
    s = s[:first] + 'extern "C" int CpWebMemIos(); // [w32-lean3]\n' + s[first:]
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name, 'wake sites', n)
_svc()

# [w32-lean3] mem=ios: no audio pacer thread (the LEngine runs inside RenderAudio there; output is best-effort)
patch(r'D:\2077\src\wwise-audio-web.cpp', [
 ('''        if (g_pacerStarted.compare_exchange_strong(expected, true)) {
            std::thread(PacerLoop).detach();''', '''        if (!::CpWebMemIos() && g_pacerStarted.compare_exchange_strong(expected, true)) { // [w32-lean3] no pacer under mem=ios
            std::thread(PacerLoop).detach();'''),
 # (the CpWebMemIos declaration is prepended at file scope of the w32 copy)
], '[w32-lean3]')

# [w32-noaudio] mem=ios (unless ?audio=1): Wwise is not initialised at all -> no Wwise threads (5) and no Wwise pools.
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\soundEngine\src\soundEngine.cpp', [
 ('''	Bool SoundEngine::Initialize()
	{''', '''	static Bool CpWebNoAudio() // [w32-noaudio]
	{
		static const int v = ::CpWebMemIos() && MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'audio' ) === '0' ? 1 : 0; } catch ( e ) { return 0; } } ); // [w32-noaudio] opt-in only: engine hangs without Wwise
		return v != 0;
	}

	Bool SoundEngine::Initialize()
	{
		if ( CpWebNoAudio() ) { std::fputs( "CP2077 w32 audio: Wwise disabled (?audio=0)", stderr ); std::fputc( 10, stderr ); return true; } // [w32-noaudio]'''),
 ('''extern "C" int CpWebMemIos(); // [w32-lean2]
''', '''extern "C" int CpWebMemIos(); // [w32-lean2]
#include <emscripten/emscripten.h> // [w32-noaudio]
#include <cstdio>
'''),
], '[w32-noaudio]')

# [w32-lean4] mem=ios: no Bink async thread at all (frames decode synchronously in BinkDoFrameAsyncMulti), and Wwise
# bank loading without the bank-manager thread.
patch(r'D:\2077\src\bink-engine-web.cpp', [
 ('''    if(CpWebMemIos())for(auto& existing:asyncSlots)if(existing){asyncSlots[number]=existing;return 1;} // [w32-lean2] share one thread''',
  '''    if(CpWebMemIos())return 1; // [w32-lean4] no async thread under mem=ios: BinkDoFrameAsyncMulti decodes synchronously'''),
 ('''S32 RADEXPLINK BinkDoFrameAsyncMulti(HBINK native,U32* threads,S32 count) {
    auto movie=Find(native);if(!movie||!threads||count<=0||count>8)return 0;''', '''S32 RADEXPLINK BinkDoFrameAsyncMulti(HBINK native,U32* threads,S32 count) {
    auto movie=Find(native);if(!movie||!threads||count<=0||count>8)return 0;
    if(CpWebMemIos()){ // [w32-lean4] synchronous decode on the caller (no async decode thread)
        {std::lock_guard<std::mutex> movieGuard(movie->asyncMutex);if(movie->asyncPending||movie->closing)return 0;}
        BinkDoFrame(&movie->native);return 1;
    }'''),
], '[w32-lean4]')
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\soundEngine\src\soundEngine.cpp', [
 ('''		if ( ::CpWebMemIos() ) initSettings.bUseLEngineThread = false; // [w32-lean2] audio rendered in RenderAudio()''',
  '''		if ( ::CpWebMemIos() ) initSettings.bUseLEngineThread = false; // [w32-lean2] audio rendered in RenderAudio()
		// [w32-lean4] bank-manager thread kept: without it the Initialization state waited ~40 s on bank loads'''),
], '[w32-lean4]')

# [w32-lean5] mem=ios: Bink frame decode is posted to the decoder asynchronously (no async pthread, no blocking wait):
# completion clears asyncPending, exactly what the async thread did after its blocking BinkDoFrame.
patch(r'D:\2077\src\bink-engine-web.cpp', [
 ('''    if(CpWebMemIos()){ // [w32-lean4] synchronous decode on the caller (no async decode thread)
        {std::lock_guard<std::mutex> movieGuard(movie->asyncMutex);if(movie->asyncPending||movie->closing)return 0;}
        BinkDoFrame(&movie->native);return 1;
    }''', '''    if(CpWebMemIos()){ // [w32-lean5] async decode request, no thread (completion in CpWebBinkComplete)
        {std::lock_guard<std::mutex> movieGuard(movie->asyncMutex);if(movie->asyncPending||movie->closing)return 0;movie->asyncPending=true;}
        if(CpPostFrameAsync(movie))return 1;
        {std::lock_guard<std::mutex> movieGuard(movie->asyncMutex);movie->asyncPending=false;}
        movie->asyncFinished.notify_all();return 0;
    }'''),
 ('''        else if(!success)r->movie->native.ReadError=1;
        delete r;return;''', '''        else if(r->operation==Frame){ // [w32-lean5]
            if(!success)r->movie->native.ReadError=1;
            {std::lock_guard<std::mutex> g(r->movie->asyncMutex);r->movie->asyncPending=false;}
            r->movie->asyncFinished.notify_all();
        }
        else if(!success)r->movie->native.ReadError=1;
        delete r;return;'''),
 ('''bool PostAsync(const std::shared_ptr<Movie>& movie,Operation operation,S32 first=0,S32 second=0) {''', '''bool CpPostFrameAsync(const std::shared_ptr<Movie>& movie) { // [w32-lean5] Frame request without blocking
    if(emscripten_is_main_runtime_thread()||!movie->registered)return false;
    auto* queue=Queue();if(!queue)return false;
    auto* request=new CpAsyncRequest();
    request->async=true;request->operation=Frame;request->handle=movie->handle;request->header=&movie->native;
    request->buffer=movie->registered;request->movie=movie;
    const bool posted=emscripten_proxy_async(queue,emscripten_main_runtime_thread_id(),[](void* argument){
        auto& r=*static_cast<CpAsyncRequest*>(argument);
        StartBinkRequest(reinterpret_cast<std::uintptr_t>(static_cast<Request*>(&r)),r.operation,r.handle,r.flags,
            r.input,r.inputBytes,r.header,r.buffer,r.capacity,r.first,r.second);
    },request);
    if(!posted){delete request;return false;}
    return true;
}
bool PostAsync(const std::shared_ptr<Movie>& movie,Operation operation,S32 first=0,S32 second=0) {'''),
], '[w32-lean5]')

# [w32-lean6] mem=ios: no GPU retirement pthread. Completed submissions are retired (in order, one drainer at a time)
# by the threads that submit or wait for retirement.
patch(r'D:\2077\src\gpu-api-command-web.cpp', [
 ('''bool EnsureRetirementWorker() {
    std::call_once(retirementStartup,[]{
        pthread_t thread;''', '''std::mutex cpRetireDrainMutex; // [w32-lean6]
bool CpRetireInline() { static const int v = ::CpWebMemIos(); return v != 0; }
void CpDrainRetirement() {
    if (!CpRetireInline()) return;
    std::unique_lock<std::mutex> drain(cpRetireDrainMutex, std::try_to_lock);
    if (!drain.owns_lock()) return;
    for (;;) {
        RetirementJob job;
        { std::lock_guard<std::mutex> lock(retirementMutex); if (retirementJobs.empty()) return; job = retirementJobs.front(); retirementJobs.pop_front(); }
        RetireSubmittedCommandsOnWorker(job.completed, job.userdata);
    }
}
template <class P> void CpWaitRetired(std::unique_lock<std::mutex>& lock, P pred) {
    if (!CpRetireInline()) { submissionsRetired.wait(lock, pred); return; }
    while (!pred()) {
        lock.unlock(); CpDrainRetirement(); lock.lock();
        if (pred()) break;
        submissionsRetired.wait_for(lock, std::chrono::milliseconds(2), pred);
    }
}
bool EnsureRetirementWorker() {
    if (CpRetireInline()) { retirementWorkerReady = true; return true; } // [w32-lean6] no thread under mem=ios
    std::call_once(retirementStartup,[]{
        pthread_t thread;'''),
 ('''    std::unique_lock<std::mutex> lock(fenceMutex);
    submissionsRetired.wait(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; });
    return true;''', '''    std::unique_lock<std::mutex> lock(fenceMutex);
    CpWaitRetired(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; }); // [w32-lean6]
    return true;'''),
 ('''        if (released) submissionsRetired.wait(lock, [released] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > released; });''',
  '''        if (released) CpWaitRetired(lock, [released] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > released; }); // [w32-lean6]'''),
 ('''    releaseTarget = 0;
    submissionsRetired.wait(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; });''',
  '''    releaseTarget = 0;
    CpWaitRetired(lock, [target] { return pendingSubmissions.empty() || *pendingSubmissions.begin() > target; }); // [w32-lean6]'''),
 ('''    if(!EnsureRetirementWorker()) {
        std::fputs("CP2077 WebGPU: GPU retirement worker creation failed\\n",stderr);''', '''    CpDrainRetirement(); // [w32-lean6] retire finished submissions on the submitting thread
    if(!EnsureRetirementWorker()) {
        std::fputs("CP2077 WebGPU: GPU retirement worker creation failed\\n",stderr);'''),
], '[w32-lean6]')
# note: gpu-api-command-web.cpp (w32 copy) also has  extern "C" int CpWebMemIos();  prepended at file scope for [w32-lean6]

# [w32-lean7] mem=ios: smaller Wwise container-stream IO memory (40 -> 12 MiB) and IO allocator budget (62 -> 40 MiB).
patch(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\soundEngine\src\wwiseContainerStreaming.cpp', [
 ('''	deviceSettings->uIOMemorySize = 40 * 1024 * 1024;''', '''	deviceSettings->uIOMemorySize = ::CpWebMemIos() ? 12 * 1024 * 1024 : 40 * 1024 * 1024; // [w32-lean7]'''),
 ('''void wwiseContainerStreamingDefaultDeviceSettings( AkDeviceSettings* deviceSettings )''', '''extern "C" int CpWebMemIos(); // [w32-lean7]
void wwiseContainerStreamingDefaultDeviceSettings( AkDeviceSettings* deviceSettings )'''),
], '[w32-lean7]')

# [w32-lean8] mem=ios: 20 MiB skinning buffer instead of 40 (mapped: GPU buffer + CPU bytes)
patch(r'D:\2077\engine-overlay\common\renderer\src\renderSkinManager.cxx', [
 ('''	Uint32 totalSkinBufferSize = c_skinBufferSizeInitial;''', '''	Uint32 totalSkinBufferSize = ::CpWebMemIos() ? RED_MEGA_BYTE( 20 ) : c_skinBufferSizeInitial; // [w32-lean8]'''),
 ('''static constexpr Uint32 c_defaultPin = UINT32_MAX;''', '''extern "C" int CpWebMemIos(); // [w32-lean8]
static constexpr Uint32 c_defaultPin = UINT32_MAX;'''),
], '[w32-lean8]')
