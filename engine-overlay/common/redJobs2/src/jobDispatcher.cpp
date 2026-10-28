/*
* Copyright (c) 2018 CD Projekt Red. All Rights Reserved.
*/

#include "build.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h> // [skperf] round 27 (ETIMEDOUT)
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
// [skperf] round 8 job-system probe (only after ?perfprobe=1 has been resolved by the GPU layer; heap-free, fixed
// tables). Every 2 s: per dispatcher-thread busy % (job execution time / wall), total busy, and the top jobs by time.
// Answers whether the render frame is limited by job throughput (threads busy) or by a serial chain (threads idle).
namespace cp2077::web { extern std::atomic<int> skperfEncodeProbe; }
extern "C" void cp2077_skperf_timeline_job(const char* name, unsigned worker, double start, double end); extern "C" void cp2077_skperf_crit_job(const char* name, double start, double end); // [skperf] round 23
extern "C" void cp2077_skperf_crit_zero(const char* name, int fromJob); // [skperf] round 24
extern "C" void cp2077_skperf_crit_flushloop(double loopExit, double iters, double sleeps, double sleepMs, double sleepMax, double ownJobsMs, double requeues, double lastJobEnd); // [skperf] round 25
extern "C" void cp2077_skperf_crit_flushloop2(double timeouts, double timedOutMaxMs, double wokenMaxMs, double missed); // [skperf] round 27/28
namespace { thread_local int tl_cpDecrementFromJob = 0; } // [skperf] round 24
// [skperf] round 58 ready probe (probeMask bit 2, heap-free): QueueJobAndSignal stamps the time a job became runnable
// (keyed by its job data pointer, 4096-slot direct-mapped table, last stamp wins); the worker that runs it exposes that
// time to the render node timers (timeline probe 'r' field) through cp2077_skperf_job_ready().
namespace cpSkperfReady
{
	struct Slot { std::atomic< const void* > key{ nullptr }; std::atomic< double > t{ 0 }; };
	Slot g_slots[ 4096 ];
	thread_local double tl_ready = 0;
	inline unsigned Index( const void* p ) { return unsigned( ( ( reinterpret_cast< std::uintptr_t >( p ) >> 3 ) * 2654435761u ) >> 20 ) & 4095u; }
	inline void Stamp( const void* p ) { Slot& s = g_slots[ Index( p ) ]; s.t.store( emscripten_get_now(), std::memory_order_relaxed ); s.key.store( p, std::memory_order_release ); }
	inline double Find( const void* p ) { const Slot& s = g_slots[ Index( p ) ]; if ( s.key.load( std::memory_order_acquire ) != p ) return 0; return s.t.load( std::memory_order_relaxed ); }
}
extern "C" double cp2077_skperf_job_ready() { return cpSkperfReady::tl_ready; }
// [skperf] round 26 flushWait: the game thread's frame wait (FlushCounter) used emscripten_thread_sleep(0.05) while
// idle, but every timed wait in the browser on Windows ends on the ~15.6 ms timer tick (tc13: sleepMaxMs 15.7,
// lastZeroToLoopExit 10.7 ms; ?flushSpin=1 gave Badlands 33 -> 45-48 fps). Now the idle waiter blocks on a wake
// word with emscripten_futex_wait and is woken (Atomics.notify, not a timer) whenever a counter reaches zero or a
// job is queued. The waker only notifies while someone is waiting (g_cpWaiters), so workers pay one atomic add.
// The 2 ms timeout is only a safety net for a missed wake. ?flushWait=0 = the old 0.05 ms sleep, ?flushSpin=1 = poll.
namespace
{
	std::atomic< Uint32 > g_cpWakeEpoch{ 0 };
	std::atomic< Int32 > g_cpWaiters{ 0 };
	// [flushwake] set once by FlushCounter. Default since 2026-10-23 A/B (Badlands, real-player priority: 31.5 -> 35.9 fps,
	// frames >50 ms 100 -> 23): no wake per queued job, no pre-futex spin. ?flushWakeJobs=1&flushSpinUs=150 = old.
	std::atomic< Int32 > g_cpWakeOnJobs{ 0 };
	inline void CpWakeWaiters()
	{
		g_cpWakeEpoch.fetch_add( 1, std::memory_order_seq_cst );
		if ( g_cpWaiters.load( std::memory_order_seq_cst ) > 0 ) emscripten_futex_wake( &g_cpWakeEpoch, INT32_MAX );
	}
} // [skperf] round 16 (librender)
namespace cpSkperfJobs {
constexpr unsigned kSlots = 512, kThreads = 64;
struct Slot { std::atomic<std::uintptr_t> fn{0}; std::atomic<const char*> name{nullptr}; std::atomic<unsigned long long> count{0}, us{0}, maxUs{0}; };
Slot slots[kSlots];
std::atomic<unsigned long long> threadBusyUs[kThreads];
std::atomic<double> windowStart{0};
void Print(double seconds) {
    static unsigned long long lastBusy[kThreads], lastUs[kSlots], lastCount[kSlots];
    char line[1400]; std::size_t used = 0;
    auto add = [&](const char* format, auto... args) {
        if (used + 1 >= sizeof(line)) return;
        const int n = std::snprintf(line + used, sizeof(line) - used, format, args...);
        if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1;
    };
    add("CP2077 skperf job probe: window=%.1fs busyPct:", seconds);
    double totalBusy = 0; unsigned active = 0;
    for (unsigned t = 0; t < kThreads; ++t) {
        const auto v = threadBusyUs[t].load(); const auto d = v - lastBusy[t]; lastBusy[t] = v;
        if (!d) continue;
        const double ms = d / 1000.0; totalBusy += ms; ++active;
        add(" t%u=%.0f", t, ms / (seconds * 10.0));
    }
    add(" threads=%u busyMsPerSec=%.0f top(ms/s,calls/s,maxMs):", active, totalBusy / seconds);
    unsigned long long d[kSlots]; bool taken[kSlots] = {};
    for (unsigned i = 0; i < kSlots; ++i) {
        const auto u = slots[i].us.load(); d[i] = u - lastUs[i]; lastUs[i] = u;
    }
    unsigned long long calls[kSlots];
    for (unsigned i = 0; i < kSlots; ++i) { const auto c = slots[i].count.load(); calls[i] = c - lastCount[i]; lastCount[i] = c; }
    for (unsigned n = 0; n < 12; ++n) {
        unsigned best = kSlots;
        for (unsigned i = 0; i < kSlots; ++i) if (!taken[i] && d[i] && (best == kSlots || d[i] > d[best])) best = i;
        if (best == kSlots) break;
        taken[best] = true;
        const char* name = slots[best].name.load();
        add(" [%.40s %.1f %.0f %.1f]", name ? name : "?", d[best] / 1000.0 / seconds, calls[best] / seconds, slots[best].maxUs.exchange(0) / 1000.0);
    }
    std::fprintf(stderr, "%s\n", line);
}
void Note(std::uintptr_t fn, const char* name, unsigned thread, double ms) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 1) == 0) return; // [skperf] probeMask bit 1
    const auto us = (unsigned long long)(ms * 1000.0);
    if (thread < kThreads) threadBusyUs[thread].fetch_add(us, std::memory_order_relaxed);
    // [skperf] round 55: slots are keyed by the job NAME (static instrumentation string), not the job function: every
    // parallel-for team job runs the same helper::ParallelForFunc, so keyed by function all parallel-for jobs of the
    // engine were summed under the first name seen (tc70: '43k Audio_EntitySystem_Update jobs/s' was all of them).
    fn = reinterpret_cast<std::uintptr_t>(name ? name : "<Unknown>");
    const unsigned h = unsigned(((fn >> 4) * 2654435761u) % kSlots);
    for (unsigned probe = 0; probe < 16; ++probe) {
        Slot& slot = slots[(h + probe) % kSlots];
        std::uintptr_t current = slot.fn.load(std::memory_order_acquire);
        if (!current) {
            std::uintptr_t expected = 0;
            if (slot.fn.compare_exchange_strong(expected, fn, std::memory_order_acq_rel)) { slot.name.store(name, std::memory_order_release); current = fn; }
            else current = expected;
        }
        if (current != fn) continue;
        slot.count.fetch_add(1, std::memory_order_relaxed); slot.us.fetch_add(us, std::memory_order_relaxed);
        auto previous = slot.maxUs.load(std::memory_order_relaxed);
        while (us > previous && !slot.maxUs.compare_exchange_weak(previous, us)) {}
        break;
    }
    const double now = emscripten_get_now();
    double start = windowStart.load(std::memory_order_relaxed);
    if (start == 0) { windowStart.compare_exchange_strong(start, now); return; }
    if (now - start >= 2000 && windowStart.compare_exchange_strong(start, now)) Print((now - start) / 1000.0);
}
}
#include "D:/2077/src/native-job-live-telemetry-web.h"
#include "../../redSystem/include/log.h"
#include "../../redCore/include/settings.h"
#include "../../redCore/include/profiler.h"

#if defined(RED_PLATFORM_DURANGO)
// Extra defines for new Console Types
#define CONSOLE_TYPE_XBOX_SERIES_S        ((CONSOLE_TYPE)5)
#define CONSOLE_TYPE_XBOX_SERIES_X        ((CONSOLE_TYPE)6)
#define CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT ((CONSOLE_TYPE)7)
#endif

#include "jobPriority.h"
#include "jobDispatcher.h"
#include "jobDispatcherThread.h"
#include "jobCounter.h"
#include "jobDecl.h"
#include "jobDebugger.h"
#include "jobCounterOwner.h"
#include "../../redSystem/include/unitTestMode.h"

// At least leave this in until the system is given a decent soak
#ifndef RED_CONFIGURATION_FINAL
# define USE_JOB_DEBUGGER
#endif

#ifdef RED_PLATFORM_WINPC
# define MUST_PUMP_WIN32_MESSAGES_FOR_DXGI
#endif

#ifdef USE_JOB_DEBUGGER
static const constexpr Bool c_useJobDebugger = true;
#else
static const constexpr Bool c_useJobDebugger = false;
#endif

// #todo: warn once vs spam
#ifndef NO_EDITOR
static const constexpr Bool c_warnOnPriorityInversion = false;
#else
static const constexpr Bool c_warnOnPriorityInversion = true;
#endif

#ifdef USE_RESOURCE_THROTTLER_THREADS
	static const Uint32	numAdditionalResourceThrottlerThreads = 2;
#else
	static const Uint32	numAdditionalResourceThrottlerThreads = 0;
#endif

namespace dd
{
	static red::CrashData< Uint32 > numDispatcherThreads{ "Jobs/Dispatcher", "NumDispatcherThreads" };
	static red::CrashData< Uint32 > maxHWConcurrency{ "Jobs/Dispatcher", "MaxHWConcurrency" };
}

namespace job { namespace prv
{

thread_local Bool g_tls_IsDispatcherThread = false;
thread_local Uint32 g_tls_DispatcherThreadIndex = UINT32_MAX;

namespace hacks
{
	REDJOBS2_API Bool IsDispatcherThread()
	{
		return g_tls_IsDispatcherThread;
	}
}

prv::Dispatcher* gDispatcher;

class DispatcherHelper
{
public:
	static CounterEntry* AllocCounterEntry( const char* debugName, const ScheduleParam& param, const void* debugUserData )
	{
		auto* counter = RED_NEW_WITHOUT_HOOKS( CounterEntry,  red::memory::HookType_Memory_Marking )();
		counter->debugName = debugName;
		counter->debugUserData = debugUserData;
		counter->param = param;
		if ( c_useJobDebugger && gDispatcher->GetDebugger() )
		{
			gDispatcher->GetDebugger()->RegisterCounter( *counter );
		}
		return counter;
	}

	static void FreeEntry( const CounterEntry* entry )
	{
		RED_FATAL_ASSERT( entry );
		RED_FATAL_ASSERT( entry->refCountMask.IsZero_Snapshot() );
		RED_FATAL_ASSERT( entry->counterValue.IsZero_Snapshot() );

		// Since a counter can be used and drop to zero multiple times, the waitingList should have been flushed
		// each and every time before finally being freed
		RED_FATAL_ASSERT( entry->waitingJobsHead == nullptr );

		if ( c_useJobDebugger && gDispatcher->GetDebugger() )
		{
			gDispatcher->GetDebugger()->UnregisterCounter( *entry );
		}

		// Counter entries are heap allocated, so their addresses churn. Release the tracer's per-address
		// generation slot before the memory goes back; without this the table fills and every later counter
		// hook loses its records. Returns immediately when nothing is tracked.
		wgtrace::ctr_retire( &entry->counterValue );

		RED_DELETE( entry );
	}

	static WaitingListEntry* FlushWaitingList( CounterEntry& counterEntry )
	{
		auto& lock = counterEntry.waitingListLock;
		RED_SCOPE_LOCK( lock );
		
		// Have to check counter value again under lock.
		// Otherwise you can have this (rare) scenario where you violate the sequentially-consistent ordering of events.
		// 1) J1 finishes running on thread T1, and T1 decrements counter W to zero. But does not call FlushWaitingList() yet.
		// 2) Then a new job J2 is created on thread T2, and increments counter W to 1. We reused counter W because it was zero (that's how CounterChain operates to avoid breaking the chain of dependencies).
		// 3) Then yet another new job J3 is created (on any thread except T1; doesn't matter) and increments new counter A, waiting for counter W.
		// 4) J3 goes on counter W's waiting list because W > 0.
		// 5) Thread T1 now finally calls FlushWaitingList() without a lock. We flush J3 from counter W's waiting list and queue it to run before J2 finishes running (or even starts).
		//
		// NOTE: if T1 checked the counter value again without a lock, then it's wrong because T1 could check in step (1.5) and then have the same situation again.
		// If T1 is holding this lock, then step (4) can't complete until we release the lock, at which point step (2) has already completed, incrementing W > 0.
		//
		// NOTE: it's always possible some counter *already* has waiting entries, then hits zero, and then gets incremented again, preventing it from flushing the waiting list.
		// However, the waiting list will eventually be flushed -- but not prematurely. And you can't deadlock unless your accumulate and wait counters are the same, or create some other cycle.
		//
		// CounterChain indirectly prevents this "denial of flushing" by not accumulating new jobs on the waitForZero counter entry.
		// (The only exception being the continuation, but that counter cannot have dropped to zero yet anyway).
		if ( !counterEntry.counterValue.IsZero_Snapshot() )
		{
			return nullptr;
		}

		auto* ret = counterEntry.waitingJobsHead;
		counterEntry.waitingJobsHead = nullptr;
		return ret;
	}
};

namespace helper
{
	static void FreeWaitingListEntryLinkedList( const WaitingListEntry* entryHead )
	{
		auto* entryIter = entryHead;
		while ( entryIter )
		{
			auto* next = entryIter->next;
			RED_FATAL_ASSERT( entryIter != next );
			RED_DELETE( entryIter );
			entryIter = next;
		}
	}

	RED_FORCE_INLINE void FreeEntry( const WaitingListEntry* entry )
	{
		FreeWaitingListEntryLinkedList( entry );
	}

	RED_FORCE_INLINE void FreeEntry( const ParallelForSharedCounterEntry* entry )
	{
		RED_DELETE( entry );
	}

	RED_FORCE_INLINE void FreeEntry( const ParallelForJobEntry* entry )
	{
		RED_DELETE( entry );
	}

	template <typename T>
	T* AllocEntry()
	{
		return RED_NEW_WITHOUT_HOOKS( T, red::memory::HookType_Memory_Marking )();
	}
	   
	static void EmptyJob( void* jobData, const RunContext& )
	{
		// Do nothing
	}
	
	static red::InstrumentationObject s_emptyJobInstrumentationObject{ "EmptyJob" };

	static Uint32 GetNumDispatcherThreads( const InitParam& setup )
	{
#if defined( RED_PLATFORM_WINPC )
		RED_FATAL_ASSERT( red::GetMaxHardwareConcurrency() > 0 );
		const Uint32 numThreads = std::max< Uint32 >( 1, (Int32)red::GetMaxHardwareConcurrency() - 1 );
#elif defined( RED_PLATFORM_WEB )
		// Browser port: leave cores for the browser main/compositor/GPU threads; idle workers only add wake churn.
		RED_FATAL_ASSERT( red::GetMaxHardwareConcurrency() > 0 );
		const Uint32 hwThreads = ( Uint32 )red::GetMaxHardwareConcurrency();
		const Uint32 cpDefaultThreads = std::max< Uint32 >( 1, std::min< Uint32 >( hwThreads > 2 ? hwThreads - 2 : 1, 4 ) ); // [skperf] round 59: default cap 4 (was 8; tc80 Badlands workers=4+transSplit=2 50 -> 56 fps, Kabuki unchanged); ?workers=8 = old
		// [skperf] round 40: ?workers=N (1..24) sets the dispatcher (job worker) thread count; ?workersAuto=1 uses
		// min(hardwareConcurrency-4, 12). Default unchanged = min(hardwareConcurrency-2, 8). Banner 'workers=N'.
		const int cpReq = MAIN_THREAD_EM_ASM_INT( { try { const q = new URLSearchParams( location.search ); const w = q.get( 'workers' );
			if ( w !== null && /^[0-9]+$/.test( w ) ) return Number( w ); if ( q.get( 'workersAuto' ) === '1' ) return -1; } catch ( e ) {} return 0; } );
		Uint32 numThreads = cpDefaultThreads;
		if ( cpReq > 0 ) numThreads = std::min< Uint32 >( ( Uint32 )cpReq, 24 );
		else if ( cpReq == -1 ) numThreads = std::max< Uint32 >( 1, std::min< Uint32 >( hwThreads > 4 ? hwThreads - 4 : 1, 12 ) );
		// [skperf] round 41: tc28 workers=12 broke the bar (maxThreads 11). Main thread (index 0) + dispatchers +
		// resource-throttler threads must fit in maxThreads, so a request is clamped to that (default untouched).
		{
			const Uint32 cpMax = ( Uint32 )setup.maxThreads, cpExtra = 1 + numAdditionalResourceThrottlerThreads;
			const Uint32 cpCap = cpMax > cpExtra ? cpMax - cpExtra : 1;
			if ( cpReq != 0 && numThreads > cpCap ) numThreads = cpCap;
		}
		std::fprintf( stderr, "CP2077 skperf switches: workers=%u (default %u, hardwareConcurrency %u, maxThreads %u)\n", numThreads, cpDefaultThreads, hwThreads, ( Uint32 )setup.maxThreads );
#elif defined( RED_PLATFORM_LINUX )
		// #tbd: this should be constrained depending on how many instances per machine we want to run
		RED_FATAL_ASSERT( red::GetMaxHardwareConcurrency() > 0 );
		const Uint32 numThreads = std::max< Uint32 >( 1, ( Int32 )red::GetMaxHardwareConcurrency() - 1 );
#elif defined( RED_PLATFORM_ORBIS )
		RED_FATAL_ASSERT( red::GetMaxHardwareConcurrency() == 7, "7CPU mode not enabled in PS4 param.sfo/Xbox manifest!" );
	#ifdef RED_CONSOLE_CORE_7_SUPPORT
			const Uint32 numThreads = 6;
	#else
			const Uint32 numThreads = 5;
	#endif
#elif defined( RED_PLATFORM_DURANGO )

	#ifdef RED_CONSOLE_CORE_7_SUPPORT
		Uint32 numThreads = 0;

		// We need this value to be flexible based on the console its being run on (only called once during setup)
		CONSOLE_TYPE consoleType = ::GetConsoleType();
		if ( consoleType == CONSOLE_TYPE_XBOX_SERIES_X || consoleType == CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT )
		{
			numThreads = 7;
		}
		else
		{
			numThreads = 6;
		}
	#else
		const Uint32 numThreads = 5;
	#endif
#else
#error Unsupported platform!
#endif

#ifdef RED_PLATFORM_CONSOLE
		if( red::UnitTestMode()  )
		{
			return std::min<Uint32>( setup.maxThreads, numThreads ); 
		}

		return numThreads;
#else 
		return std::min<Uint32>( setup.maxThreads, numThreads ); 
#endif
	}

	static red::TAffinityMask GetDispatcherThreadAffinityMask( Uint32 threadIndex )
	{
		// #tbd: let's explore hard affinities once we don't have to fight on the second cluster against things like I/O and other threads
		RED_UNUSED( threadIndex );

#if defined( RED_PLATFORM_WINPC ) || ( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )

		RED_FATAL_ASSERT( red::GetMaxHardwareConcurrency() > 0 );
		const red::TAffinityMask affinityMask{ 0 };

#elif defined( RED_PLATFORM_ORBIS )

		red::TAffinityMask affinityMask = 0;

		switch (threadIndex)
		{
		case 0:
			affinityMask = RED_FLAG64( 0 ) | RED_FLAG64( 1 );
			break;
		case 1:
			affinityMask = RED_FLAG64( 2 ) | RED_FLAG64( 3 );
			break;
		case 2:
			affinityMask = RED_FLAG64( 2 ) | RED_FLAG64( 3 );
			break;
		case 3:
			affinityMask = RED_FLAG64( 4 ) | RED_FLAG64( 5 );
			break;
		case 4:
			affinityMask = RED_FLAG64( 4 ) | RED_FLAG64( 5 );
			break;
#ifdef RED_CONSOLE_CORE_7_SUPPORT
		case 5:
			affinityMask = RED_FLAG64( 6 );
			break;
#endif 
		default:
			RED_FATAL( "Out of Bound Thread Index" );
			break;
		}
#elif defined( RED_PLATFORM_DURANGO )

		red::TAffinityMask affinityMask = 0;

		// This is just to allow us to dynamically assert based on the #define
	#ifdef USE_RESOURCE_THROTTLER_THREADS
		const bool usingResourceThrottlerThreads = true;
	#else
		const bool usingResourceThrottlerThreads = false ;
	#endif

		switch (threadIndex)
		{
		case 0:
			affinityMask = RED_FLAG64( 0 ) | RED_FLAG64( 1 );
			break;
		case 1:
			affinityMask = RED_FLAG64( 2 ) | RED_FLAG64( 3 );
			break;
		case 2:
			affinityMask = RED_FLAG64( 2 ) | RED_FLAG64( 3 );
			break;
		case 3:
			affinityMask = RED_FLAG64( 4 ) | RED_FLAG64( 5 );
			break;
		case 4:
			affinityMask = RED_FLAG64( 4 ) | RED_FLAG64( 5 );
			break;
	#ifdef RED_CONSOLE_CORE_7_SUPPORT
		case 5:
			affinityMask = RED_FLAG64( 6 );
			break;
		case 6:
			{
				// Detect which console we are running on
				CONSOLE_TYPE consoleType = ::GetConsoleType();

				// If we are running on SERIES_X then we have an additional redDispatcher thread, so set the affinity for that
				if ( consoleType == CONSOLE_TYPE_XBOX_SERIES_X || consoleType == CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT )
				{
					// Allow it to float a bit, just in case it starts, gets bumped by Audio and other threads are waiting on it
					affinityMask = RED_FLAG64( 5 ) | RED_FLAG64( 6 ); 
				}
				else
				{
					// Check we should be able to get here !
					RED_ASSERT( usingResourceThrottlerThreads );
					// Otherwise, this is an additional ResourceThrottler Thread, so set Affinity based on that
					affinityMask = 0x7F; // Resource Throttler : All cores, but at a background priority
				}
			}
			break;
		case 7:
			{
				// Check we should be able to get here !
				RED_ASSERT( usingResourceThrottlerThreads );
				affinityMask = 0x7F; // Resource Throttler : All cores, but at a background priority
			}
			break;
		case 8:
			{
				// Detect which console we are running on for the Assert
				CONSOLE_TYPE consoleType = ::GetConsoleType();

				// Check we should be able to get here !
				RED_ASSERT( usingResourceThrottlerThreads && ( consoleType == CONSOLE_TYPE_XBOX_SERIES_X || consoleType == CONSOLE_TYPE_XBOX_SERIES_X_DEVKIT ) );
				affinityMask = 0x7F; // Resource Throttler : All cores, but at a background priority
			}
			break;
	#endif
		default:
			RED_FATAL( "Out of Bound Thread Index" );
			break;
		}
#else

	#error Unsupported platform!

#endif
		return affinityMask;
	}

	static red::TAffinityMask GetMainThreadAffinityMask()
	{
#if defined( RED_PLATFORM_WINPC ) || ( defined( RED_PLATFORM_LINUX ) || defined( RED_PLATFORM_WEB ) )
		const red::TAffinityMask affinityMask{ 0 };
#elif defined( RED_PLATFORM_CONSOLE )
		const red::TAffinityMask affinityMask = RED_FLAG64( 0 ) | RED_FLAG64( 1 );
#else
#error Unsupported platform!
# endif
		return affinityMask;
	}
}

Dispatcher::Dispatcher( const InitParam& setup )
	: m_setup( setup )
	, m_priorityMap()
	, m_jobScopeAllocator( nullptr )
	, m_isFlushingCounter( false )
{
	InitParam::SetCrashData(setup);

	RED_FATAL_ASSERT( m_setup.maxThreads > 0 && m_setup.maxThreads <= 64 );
	Init();
}

Dispatcher::~Dispatcher()
{
	Shutdown();
}

void Dispatcher::Init()
{
    cp2077JobTelemetry::Register();
	if ( c_useJobDebugger && m_setup.useJobDebugger )
	{
		m_debugger = red::CreateUniquePtr< prv::Debugger >();
	}

	InitJobQueue( m_setup );

	const Uint32 numThreads = helper::GetNumDispatcherThreads( m_setup );
	dd::numDispatcherThreads.Set( numThreads + numAdditionalResourceThrottlerThreads );
	dd::maxHWConcurrency.Set( red::GetMaxHardwareConcurrency() );
	m_dispatcherThreads.Reserve( numThreads + numAdditionalResourceThrottlerThreads );

	RED_FATAL_ASSERT( ::SIsMainThread() );
	red::SetCurrentThreadAffinity( helper::GetMainThreadAffinityMask() );
	red::SetCurrentThreadName( "GameThread" ); //#tbd: only because "MainThread" on Xbox is already taken by default

	m_jobScopeAllocator = &job::PoolJobScope::GetAllocator();
	RED_FATAL_ASSERT( m_jobScopeAllocator, "Job scope allocator does not exist" );
#if !defined( RED_MEMORY_FORCE_DEBUG_ALLOCATOR )
	m_jobScopeAllocator->RegisterCurrentThread();
#endif

	// Tad: Add additional ResourceThrottler job threads
	for ( Uint32 i = 0; i < numThreads + numAdditionalResourceThrottlerThreads; ++i )
	{
		const Uint32 dispatcherThreadIndex = i + 1; // 0 index reserved for the main thread
		char threadName[ 64 ];
		if ( i < numThreads )
		{
			red::SNPrintFUnsafe( threadName, RED_ARRAY_COUNT_U32( threadName ), "redDispatcher%u", dispatcherThreadIndex );
		}
#ifdef USE_RESOURCE_THROTTLER_THREADS
		else
		{
			red::SNPrintFUnsafe( threadName, RED_ARRAY_COUNT_U32( threadName ), "resourceThrottler%u", (i + 1) - numThreads );
		}
#endif
		DispatcherThreadSetup setup;
		setup.stackSizeKB = m_setup.workerThreadStackSizeKB;
		setup.affinityMask = helper::GetDispatcherThreadAffinityMask( i );
		setup.dispatcherThreadIndex = dispatcherThreadIndex;
		m_dispatcherThreads.EmplaceBack( red::CreateUniquePtr< DispatcherThread >( threadName, *this, setup ) );
	}

	RED_FATAL_ASSERT( !m_dispatcherThreads.Empty() );

	for ( auto& thread : m_dispatcherThreads )
	{
		thread->InitThread();
	}

#ifdef USE_RESOURCE_THROTTLER_THREADS
	for ( Uint32 rtThreads = m_dispatcherThreads.Size() - 1; rtThreads >= ( m_dispatcherThreads.Size() - numAdditionalResourceThrottlerThreads ); rtThreads-- )
	{
		// Set the resourceThrottler Threads to low priority
		m_dispatcherThreads[rtThreads]->SetPriority( red::TP_Lowest );
	
#if defined(RED_PLATFORM_DURANGO)
		// Setting this flag means that the threads never get a priority boost, which is what we want as
		// these are designed to be background threads and should never interrupt higher priority threads
		m_dispatcherThreads[rtThreads]->DisablePriorityBoost( true );
#endif
	}
#endif // USE_RESOURCE_THROTTLER_THREADS

	// Apparently needed in unit tests to give the dispatcher threads time to actually finish initializing!
	// Or else we take too long doing this test, because we're using maybe one or two threads and the system doesn't
	// finish initializing the threads in time.
	red::Timer initTimer;
	for ( ;; )
	{
		Uint32 numThreadsReady = 0;
		for ( auto& thread : m_dispatcherThreads )
		{
			if ( thread->IsReady() )
			{
				numThreadsReady += 1;
			}
		}
		if ( numThreadsReady == m_dispatcherThreads.Size() )
		{
			break;
		}

		red::SleepOnCurrentThread( 100 );
		RED_LOG_WARNING( "Waiting for DispatcherThreads: %u/%u ready...", numThreadsReady, m_dispatcherThreads.Size() );
		if ( initTimer.GetSeconds() > 1. )
		{
			RED_LOG_WARNING( "Skipped waiting for DispatcherThreads: %u/%u ready...", numThreadsReady, m_dispatcherThreads.Size() );
			break;
		}
	}
}

void Dispatcher::InitJobQueue( const InitParam& setup )
{
	RED_FATAL_ASSERT( setup.maxLatentJobs > 0 );
	RED_FATAL_ASSERT( setup.maxCriticalPathJobs > 0 );
	RED_FATAL_ASSERT( setup.maxImmediateJobs > 0 );

	DispatcherQueueSetup queueSetup;
	queueSetup.numLowPriorityJobs = m_setup.maxLatentJobs;
	queueSetup.numNormalPriorityJobs = m_setup.maxCriticalPathJobs;
	queueSetup.numHighPriorityJobs = m_setup.maxImmediateJobs;
#ifdef RED_CONSOLE_CORE_7_SUPPORT
	RED_FATAL_ASSERT( setup.maxCore7Jobs > 0 );
	queueSetup.numCore7Jobs = m_setup.maxCore7Jobs;
#endif

	if ( setup.allJobsCriticalPath )
	{
		queueSetup.numLowPriorityJobs = m_setup.maxCriticalPathJobs;
		std::fill( std::begin( m_priorityMap ), std::end( m_priorityMap ), Priority::CriticalPath );
	}
	else
	{
		static_assert( RED_ARRAY_COUNT_U32( m_priorityMap ) == static_cast< Uint32 >( Priority::COUNT ), "" );
		for ( Uint32 i = 0; i < RED_ARRAY_COUNT_U32( m_priorityMap ); ++i )
		{
			m_priorityMap[ i ] = static_cast<Priority>( i );
		}
	}

	m_dispatcherQueue.Initialize( queueSetup );
}

// Note: Shutdown() can be called multiple times, since it will have no further effect
// subsequent times.
void Dispatcher::Shutdown()
{
	m_isExitRequested.SetValue( true );
    m_dispatcherQueue.Shutdown( numAdditionalResourceThrottlerThreads );

	auto* lowPriCounter = InitJobCounter( "shutdownLowPri", Priority::Latent, nullptr );
	auto* renderPriCounter = InitJobCounter( "shutdownLowPri", Priority::RenderPath, nullptr );
	auto* normalPriCounter = InitJobCounter( "shutdownNormalPri", Priority::CriticalPath, nullptr );
	auto* highPriCounter = InitJobCounter( "shutdownHighPri", Priority::Immediate, nullptr );

	const JobDecl nullJob;
	for ( int i : m_dispatcherThreads.Indices() )
	{
		QueueJobAndSignal( nullJob, lowPriCounter, nullptr );
		QueueJobAndSignal( nullJob, renderPriCounter, nullptr );
		QueueJobAndSignal( nullJob, normalPriCounter, nullptr );
		QueueJobAndSignal( nullJob, highPriCounter, nullptr );
	}

	for ( auto& thread : m_dispatcherThreads )
	{
		thread->JoinThread();
	}

	ReleaseJobCounterInternal( lowPriCounter );
	ReleaseJobCounterInternal( renderPriCounter );
	ReleaseJobCounterInternal( normalPriCounter );
	ReleaseJobCounterInternal( highPriCounter );
	m_dispatcherThreads.Clear();
}

void Dispatcher::AddRefJobCounterInternal(CounterEntry* entry)
{
	RED_FATAL_ASSERT(entry);
	entry->refCountMask.AddRef();
}

void Dispatcher::ReleaseJobCounterInternal( CounterEntry* entry )
{
	RED_FATAL_ASSERT( entry );
	const auto result = entry->refCountMask.Release();
	if ( result.isZero )
	{
		DispatcherHelper::FreeEntry( entry );
	}
}

namespace helper
{
	static const StackTraceCacheEntryPath* GetDebugStackTraces( const JobQueueEntry& entry )
	{
		return nullptr; // TO REDO
	}

	static const char* debugHelperMessage[] = { "Run with '-jobDebugger' on the commandline to get stacktraces" };

	static void InitRunContext( const JobQueueEntry& entry, const Uint32 dispatcherThreadIndex, ScheduleParam param, job::Counter& continuationCounter, RunContext& outRunContext )
	{
		const char* debugName = "<Unknown>";
		if ( entry.jobDecl.instrumentationObject && entry.jobDecl.instrumentationObject->m_name )
		{
			debugName = entry.jobDecl.instrumentationObject->m_name;
		}
		outRunContext.debugName = debugName;

		if ( GetDebugStackTraces( entry ) )
		{
			outRunContext.debugStackTraces = GetDebugStackTraces( entry )->debugStringView;
		}
		else
		{
			if ( c_useJobDebugger )
			{
				outRunContext.debugStackTraces = debugHelperMessage;
			}
		}

		outRunContext.dispatcherThreadIndex = dispatcherThreadIndex;

		//FIXME:
		outRunContext.continuationContext.counter = &continuationCounter;
		outRunContext.continuationContext.instrumentationObject = entry.jobDecl.instrumentationObject;
		outRunContext.continuationContext.param = param;
	}

	static void VerifyJobDecl( const JobDecl& jobDecl )
	{
		RED_FATAL_ASSERT( jobDecl.jobFunc );

#ifdef USE_PROFILER
		auto* const instrumentationObject = jobDecl.instrumentationObject;
		RED_FATAL_ASSERT( instrumentationObject && instrumentationObject->m_name && instrumentationObject->m_name[ 0 ], "Must provide a proper instrumentation object" );
#endif
	}

	struct ScopedDebugFunction : red::NonCopyable
	{
		explicit ScopedDebugFunction( Debugger* debugger, const JobQueueEntry& entry )
			: m_debugger( debugger )
			, m_entry( entry )
		{
			if ( c_useJobDebugger && m_debugger )
			{
				auto* stackTrace = GetDebugStackTraces( entry );
//				m_debugger->BeginJobFunction( m_entry.jobDecl, stackTrace );
			}
		}

		~ScopedDebugFunction()
		{
			if ( c_useJobDebugger && m_debugger )
			{
	//			m_debugger->EndJobFunction( m_entry.jobDecl );			
			}
		}

		Debugger* m_debugger;
		const JobQueueEntry& m_entry;
	};

	static void RunJobFunctionWithInstrumentation( const JobQueueEntry& entry, const RunContext& runContext )
	{
		PC_SCOPE_INST_OBJ( *entry.jobDecl.instrumentationObject, entry.jobDecl.instrumentationObject->m_name );
        // [skperf] round 10: job timing/bookkeeping (two clock reads, name compare, worker-table scan per job) only
        // with ?perfprobe=1; default builds just run the job.
        // [skperf] round 58 ready probe: this job's runnable time for the node timers it runs (restored afterwards: jobs can nest).
        struct CpReadyScope { double prev; ~CpReadyScope() { cpSkperfReady::tl_ready = prev; } } cpReadyScope{ cpSkperfReady::tl_ready };
        cpSkperfReady::tl_ready = ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 2 ) ? cpSkperfReady::Find( entry.jobDecl.jobData ) : 0;
        if ( ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 1 ) == 0 ) { entry.jobDecl.jobFunc( entry.jobDecl.jobData, runContext ); return; }
        // [skperf] round 18: everything the probe needs is read BEFORE the job runs. The old code re-read
        // entry.jobDecl.jobFunc (and the debug name) after the job had finished, when the queue entry / job data may
        // already be recycled by another worker.
        const auto cpJobFunction=entry.jobDecl.jobFunc;
        const char* cpName=runContext.debugName ? runContext.debugName : "<Unknown>";
        const double cpBegin=emscripten_get_now();
		cpJobFunction( entry.jobDecl.jobData, runContext );
        const double cpElapsed=emscripten_get_now()-cpBegin;
        cpSkperfJobs::Note(reinterpret_cast<std::uintptr_t>(cpJobFunction),cpName,runContext.dispatcherThreadIndex,cpElapsed); // [skperf] round 8
        if(cpElapsed>=0.5 && std::strncmp(cpName,"RunRenderJob",12)!=0) cp2077_skperf_timeline_job(cpName,runContext.dispatcherThreadIndex,cpBegin,cpBegin+cpElapsed); // [skperf] round 16
        cp2077_skperf_crit_job(cpName,cpBegin,cpBegin+cpElapsed); // [skperf] round 23 (probeMask bit 2 too)
        // Keep bounded worker-local records, avoiding profiler allocations in
        // the scheduler and preserving all original job/counter operations.
        struct CpJobCost {std::uintptr_t function=0;unsigned count=0,slow=0;double total=0,max=0;};
        static thread_local CpJobCost cpRecords[64];
        static thread_local unsigned cpUsed=0;
        if(std::strncmp(cpName,"Render",6)==0 || cpElapsed>=20 || (std::strcmp(cpName,"<Unknown>")==0 && cpElapsed>=1)){
            const auto cpFunction=reinterpret_cast<std::uintptr_t>(cpJobFunction);
            CpJobCost* cpRecord=nullptr;
            for(unsigned i=0;i<cpUsed;++i)if(cpRecords[i].function==cpFunction){cpRecord=&cpRecords[i];break;}
            if(!cpRecord && cpUsed<64){cpRecord=&cpRecords[cpUsed++];cpRecord->function=cpFunction;}
            if(cpRecord){
                const unsigned n=++cpRecord->count;cpRecord->total+=cpElapsed;cpRecord->max=std::max(cpRecord->max,cpElapsed);
                const bool slowSample=cpElapsed>=20 && ++cpRecord->slow<=8;
                if(n<=2 || (n<=4096 && (n&(n-1))==0) || slowSample)
                    std::fprintf(stderr,"CP2077 render job cost probe: worker=%u function=%llu name=%s count=%u lastMs=%.3f totalMs=%.3f maxMs=%.3f\n",
                        runContext.dispatcherThreadIndex,static_cast<unsigned long long>(cpFunction),cpName,n,cpElapsed,cpRecord->total,cpRecord->max);
            }
        }
	}
}

/*
namespace helper
{
	// #tbd: should also ban in IO callbacks, but this should catch most real frame allocator misuse
	// #tbd: should also ban if have no wait counter and not a continuation... 
	// ... which could be tricker to do correctly: the accum counter could get flushed so it's safe.
	// For the continuation case, would need to know if the job was pushed into the queue after waiting for another non-latent
	// counter.
	// And "immediate" jobs might also be non-frame sync'd.
	struct ScopedBanFrameAllocator : red::NonCopyable
	{
		explicit ScopedBanFrameAllocator( const JobQueueEntry& entry )
			: m_isBanned( false )
		{
			Init( entry );
		}

		~ScopedBanFrameAllocator()
		{
			if ( m_isBanned )
			{
				red::memory::DebugBanFrameAllocator( false );
			}
		}

	private:
		void Init( const JobQueueEntry& entry )
		{
			if ( red::memory::c_checkDebugBanFrameAllocator )
			{
				if ( !entry.accumulateCounterEntry || entry.accumulateCounterEntry->GetPriority() == Priority::Latent )
				{
					if ( (entry.jobDecl.debugFlags & static_cast<Uint8>( JobDebugFlags::AllowFrameAllocator ) ) == 0 )
					{
						red::memory::DebugBanFrameAllocator( true );
						m_isBanned = true;
					}
				}
			}
		}

		Bool m_isBanned;
	};
}
*/

void Dispatcher::DoRunJobQueueEntry( const JobQueueEntry& entry, const Uint32 dispatcherThreadIndex, Priority priority, prv::JobScopeMemoryAllocator& jobScopeAllocator, TLocalQueue* optLocalQueue )
{
	RunContext runContext;
	
	// FIXME:
	//////////////////////////////////////////////////////////////////////////
	//helper::ScopedBanFrameAllocator scopedBanFrameAllocator{ entry };
	
	RED_FATAL_ASSERT(entry.accumulateCounterEntry);
	job::Counter continuationCounter{ *entry.accumulateCounterEntry };
	helper::InitRunContext( entry, dispatcherThreadIndex, { priority, Affinity::All, continuationCounter.Internal_GetIOPriority() }, continuationCounter, runContext );
        cp2077JobTelemetry::Guard cpLiveJob(runContext.dispatcherThreadIndex,1,runContext.debugName,
            reinterpret_cast<std::uintptr_t>(entry.jobDecl.jobFunc),reinterpret_cast<std::uintptr_t>(entry.accumulateCounterEntry));
	helper::RunJobFunctionWithInstrumentation( entry, runContext );

	if ( entry.accumulateCounterEntry )
	{
		tl_cpDecrementFromJob = 1; // [skperf] round 24
		DecrementCounterEntryInternal( entry.accumulateCounterEntry, optLocalQueue );
		tl_cpDecrementFromJob = 0;
	}

#if !defined( RED_MEMORY_FORCE_DEBUG_ALLOCATOR )
	jobScopeAllocator.Reset();
#endif
}

Bool Dispatcher::TryPopJobQueueEntry( JobQueueEntry& outEntry, Priority& outPriority )
{
	return m_dispatcherQueue.TryPop( outEntry, outPriority );
}

void Dispatcher::PopJobQueueEntry( JobQueueEntry& outEntry, Priority& outPriority )
{
	m_dispatcherQueue.Pop( outEntry, outPriority );
}

void Dispatcher::PopJobQueueEntry( JobQueueEntry& outEntry, Priority& outPriority, Affinity affinity )
{
	m_dispatcherQueue.Pop( outEntry, outPriority, affinity );
}

Priority Dispatcher::MapPriority( Priority priority ) const
{
	return m_priorityMap[ static_cast<Uint32>( priority ) ];
}

CounterEntry* Dispatcher::InitJobCounter( const char* debugName, ScheduleParam param, const void* debugUserData )
{
#ifdef USE_RESOURCE_THROTTLER_THREADS
	RED_FATAL_ASSERT( param.affinity == Affinity::All || ( (param.affinity == Affinity::ConsoleCore7 || param.affinity == Affinity::ResourceThrottler) && param.priority == Priority::Latent ), "Invalid Priority & Affinity combination." );
#else
	RED_FATAL_ASSERT( param.affinity == Affinity::All || (param.affinity == Affinity::ConsoleCore7 && param.priority == Priority::Latent ), "Invalid Priority & Affinity combination." );
#endif
	auto mappedPriority = MapPriority( param.priority );
	return DispatcherHelper::AllocCounterEntry( debugName, { mappedPriority, param.affinity, param.ioPriority }, debugUserData );
}

CompletionDeferral Dispatcher::CreateDeferral( const char* debugName, const void* debugUserData, CounterEntry& counterEntry )
{
	const Uint32 numFakeJobs = 1;
	const auto result = counterEntry.counterValue.ExchangeAdd( numFakeJobs );
	if ( result.wasZero )
	{
		counterEntry.refCountMask.AddRef();
	}
	return CompletionDeferral( debugName, debugUserData, counterEntry );
}

Bool Dispatcher::IsZero_Snapshot( const CounterEntry* counterEntry )
{
	RED_FATAL_ASSERT( counterEntry );
	return counterEntry->counterValue.IsZero_Snapshot();
}

Debugger* Dispatcher::GetDebugger() const
{
	return m_debugger.Get();
}

void Dispatcher::AnalyzeCounter(const CounterEntry& counter)
{
	if ( !c_useJobDebugger )
	{
		RED_LOG_ERROR( "Job debugger not compiled. Recompile with c_useJobDebugger true" );
		return;
	}

	if ( !m_debugger )
	{
		RED_LOG_ERROR( "Job debugger not enabled. Run with '-jobDebugger'" );
		return;
	}

	m_debugger->AnalyzeCounter(counter);
}

namespace helper
{
	static void CopyJobToWaitingListEntry( const JobDecl& job, CounterEntry* accumulateCounterEntry, const StackTraceCacheEntryPath* debugTrace, WaitingListEntry* entry )
	{
		RED_FATAL_ASSERT( entry );
		entry->job = job;
		entry->accumulateCounterEntry = accumulateCounterEntry;
		entry->debugTrace = debugTrace;
	}
}

Bool Dispatcher::TryPutOnWaitingList( const JobDecl& job, const CounterEntry& waitForZeroCounterEntry, CounterEntry* accumulateCounterEntry, const StackTraceCacheEntryPath* debugTrace )
{
	// check if we should try to lock the waitForZeroCounter to avoid writing/allocating waint list entries unnecesarily.
	// If this counter is bumped back above zero asynchronously during this function call, then it's a race condition anyway
	// whether this job would have to wait or not
	if ( waitForZeroCounterEntry.counterValue.IsZero_Snapshot() )
	{
		return false;
	}

	// #tbd: should be quick anyway, but allocate/free without holding lock; keep the upcoming locked section as quick as possible
	// to avoid tying up other threads needlessly and potentially have them start backing off/sleeping on the spinlock
	WaitingListEntry* const entry = helper::AllocEntry< WaitingListEntry >();
	helper::CopyJobToWaitingListEntry( job, accumulateCounterEntry, debugTrace, entry );
		
	Bool isOnList = false;

	// locked section
	{
		auto& lock = waitForZeroCounterEntry.waitingListLock;
		RED_SCOPE_LOCK( lock );

		// Must check value again under lock, or might never get run
		if ( !waitForZeroCounterEntry.counterValue.IsZero_Snapshot() )
		{
			entry->next = waitForZeroCounterEntry.waitingJobsHead;
			waitForZeroCounterEntry.waitingJobsHead = entry;
			isOnList = true;
		}
	}

	if ( !isOnList )
	{
		helper::FreeEntry( entry );
		return false;
	}

	return true;
}

void Dispatcher::DecrementCounterEntryInternal( CounterEntry* counterEntry, TLocalQueue* optLocalQueue )
{
	RED_FATAL_ASSERT( counterEntry );
	
	if ( !counterEntry->counterValue.Decrement().isZero )
	{
		return;
	}

	// We hit zero counter value, time to release any waiting entries
	if ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 1 ) cp2077_skperf_crit_zero( counterEntry->debugName, tl_cpDecrementFromJob ); // [skperf] round 24
	CpWakeWaiters(); // [skperf] round 26 flushWait
	auto* const entriesHead = DispatcherHelper::FlushWaitingList( *counterEntry );
	auto* entryIter = entriesHead;
	// Trace which counter generation gated each released job. The instance must be read before FreeEntry below
	// retires the counter's tracer slot. Read-only; queueing order and counter lifetimes are unchanged.
	const bool cpGateTrace = entriesHead && cpEngineTrace::Enabled();
	const Uint32 cpReleasedInst = cpGateTrace ? wgtrace::ctr_peek( &counterEntry->counterValue ) : 0;
	Uint32 cpBatch = 0;
	if ( cpGateTrace ) for ( auto* e = entriesHead; e; e = e->next ) ++cpBatch;

	// See RunJob() for AddRef() logic.
	if ( counterEntry->refCountMask.Release().isZero )
	{
		// Free now before resuming any suspended jobs. Make it easier to hit any issues that might arise from accessing the counter.
		// If any dependencies or suspended jobs were still going to use the counter again, it would have been kept alive by an AddRef().
		DispatcherHelper::FreeEntry( counterEntry );
	}

	// Queue up the waiting jobs
	if ( entryIter )
	{
		for ( ;; )
		{
			if ( cpGateTrace ) wgtrace::gate_release( cpReleasedInst, entryIter->accumulateCounterEntry ? &entryIter->accumulateCounterEntry->counterValue : nullptr, cpBatch );
			QueueJobAndSignal( entryIter->job, entryIter->accumulateCounterEntry, optLocalQueue );
			RED_FATAL_ASSERT( entryIter != entryIter->next );
			if ( !entryIter->next )
			{
				break;
			}
			entryIter = entryIter->next;
		}
	}

	helper::FreeEntry( entriesHead );
}

void Dispatcher::QueueJobAndSignal( const JobDecl& job, CounterEntry* accumulateCounterEntry, TLocalQueue* optLocalQueue )
{
	Priority priority = Priority::RenderPath;
	Affinity affinity = Affinity::All;

	// all physics job will have normal priority so they are not blocked by loading jobs
	if ( job.hint != JobHint::PhysX )
	{
		priority = MapPriority( Priority::Latent );

		if ( accumulateCounterEntry )
		{
			priority = accumulateCounterEntry->param.priority;
			affinity = accumulateCounterEntry->param.affinity;
		}

		// If no accumulate counter, then assume low priority because no dependencies can sync on it anyway
		// Always possible that could be sync'd on in some ad-hoc way though.
		// #fixme: priority and path are separate now.
#ifndef NO_EDITOR
		if ( job.hint == JobHint::Large && m_setup.allJobsCriticalPath )
		{
			priority = Priority::Latent;
		}
#endif
	}

	JobQueueEntry entry;
	entry.jobDecl = job;
	entry.accumulateCounterEntry = accumulateCounterEntry;
	if ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 2 ) cpSkperfReady::Stamp( job.jobData ); // [skperf] round 58

	const Bool useLocalQueue = entry.jobDecl.hint == JobHint::Trivial && optLocalQueue && optLocalQueue->Size() < c_localQueueDefaultCapacity;
	if (false)//useLocalQueue)
	{
		optLocalQueue->PushBack(std::make_pair(entry, priority));
	}
	else
	{
		for (;;)
		{
			if (m_dispatcherQueue.TryPush(entry, priority, affinity))
			{
				break;
			}
			else if (optLocalQueue)
			{
				// Emergency push back to try and avoid deadlocks
				optLocalQueue->PushBack(std::make_pair(entry, priority));
				break;
			}
		}
	}
	if ( g_cpWakeOnJobs.load( std::memory_order_relaxed ) ) CpWakeWaiters(); // [skperf] round 26 flushWait: a new job may be one the frame waiter can run ([flushwake] ?flushWakeJobs=0 = counter-zero wakes only)
}

void Dispatcher::InitWithEmptyJob( JobDecl& outJob )
{
	outJob.jobFunc = &helper::EmptyJob;
	outJob.hint = JobHint::Trivial;
	outJob.instrumentationObject = &helper::s_emptyJobInstrumentationObject;
}

Uint32 Dispatcher::GetNumDispatcherThreads(Bool includeCore7) const
{
#ifdef RED_CONSOLE_CORE_7_SUPPORT
	if (includeCore7)
	{
		// Minus one for the 7th Core Latent job queue which isn't used for Parallel Jobs
		return m_dispatcherThreads.Size() - 1 - numAdditionalResourceThrottlerThreads;
	}
#endif

	return m_dispatcherThreads.Size() - numAdditionalResourceThrottlerThreads;
}

void Dispatcher::RunJob( const JobDecl& job, const CounterEntry* waitForZeroCounterEntry, CounterEntry* accumulateCounterEntry )
{
	helper::VerifyJobDecl( job );
	RED_FATAL_ASSERT( ( waitForZeroCounterEntry != accumulateCounterEntry ) || !waitForZeroCounterEntry, "This will create a deadlock - how can jobs run after themselves?" );

	// Bump value here and NOT in QueueJobsOrWait. If we suspend a fiber and queue a fake job to resume it later, we don't want to bump its accumulateCounterEntry again
	if ( accumulateCounterEntry )
	{
		// #fixme: if a continuation counter and useContinuationPriorityBoost true, then no real priority inversion exists since our wait counter is already zero
		// but need to know at this level
		if ( c_warnOnPriorityInversion && waitForZeroCounterEntry && waitForZeroCounterEntry->param.priority < accumulateCounterEntry->param.priority )
		{
		RED_LOG_CATEGORY_WARNING( red::LoggerCategory_Jobs, "Priority inversion for job '%hs'. priority %u < priority %u waitForZeroCounterEntry:\n%hs\naccumulateCounterEntry:\n%hs",
				( job.instrumentationObject ? job.instrumentationObject->m_name : "<No instrumentation object>" ),
				(Uint32)waitForZeroCounterEntry->param.priority, accumulateCounterEntry->param.priority,
				waitForZeroCounterEntry->debugName, accumulateCounterEntry->debugName );			
		}

		const Int32 numJobs = 1; // NOTE: if go back to array of jobs, then DO NOT AddRefForDispatcher() if numJobs == 0
		const auto result = accumulateCounterEntry->counterValue.ExchangeAdd( numJobs );

		// Increment for every expected future "return to zero" - when the dispatcher will see if it can free the counter. But we haven't actually run these jobs yet, so
		// no race condition having to AddRef() next.
		//
		// We must add to the flag in order to set it, and subtract to try and clear it. This avoids a race where oldValue is zero,
		// but zero because another thread just finished decrementing counterValue and wants to clear c_inUseDispatcherMask from refCountMask.
		if ( result.wasZero )
		{
			accumulateCounterEntry->refCountMask.AddRef();
		}
	}

	QueueJobOrWait( job, waitForZeroCounterEntry, accumulateCounterEntry );
}

namespace helper
{
	static void ParallelForFunc( void* jobData, const RunContext& runContext )
	{
		auto* entry = static_cast<ParallelForJobEntry*>( jobData );
		RED_FATAL_ASSERT( entry );
		auto* sharedCounter = entry->sharedCounterEntry;
		RED_FATAL_ASSERT( sharedCounter );

		auto* const sharedData = entry->sharedData;
		void* const elements = entry->elements;
		const Uint32 numElements = entry->numElements;
		const Uint32 realTeamSize = entry->teamSize;
		const Uint32 teamSize = entry->maxBatchSize == 0 ? entry->teamSize : math::Max( numElements / entry->maxBatchSize, 1u );
		const Uint32 batchSize = ( numElements + teamSize - 1 ) / teamSize;

		// Not used in the epilogue, since should be agnostic which job finished
		RunContext parallelForRunContext = runContext;
		parallelForRunContext.parallelForTeamIndex = entry->teamIndex;
		for ( ;; )
		{
			const auto index = sharedCounter->counter.Increment() - 1;
			if ( index < teamSize )
			{
				const Uint32 elementStartInd = index * batchSize;
				const Uint32 elementEndIndex = Min( elementStartInd + batchSize, numElements );
				entry->jobFunc( sharedData, elements, elementStartInd, elementEndIndex, parallelForRunContext );
			}
			else
			{
				// Can release the sharedCounter after every job in the team has incremented the counter
				// and seen that it's been exceeded, so won't touch the counter again
				// One thread will see index as "numElements" and stop, then "teamSize - 1" more threads will increment it past that value, then stop too.
				const Bool mustReleaseCounterNow = ( index == teamSize + realTeamSize - 1 );
				if ( mustReleaseCounterNow )
				{
					helper::FreeEntry( sharedCounter );

					// #tbd: could even copy then release entry here before calling function to free the entry even sooner
					if ( entry->epilogueFunc )
					{
						entry->epilogueFunc( sharedData, elements, numElements, runContext );
					}
				}

				break;
			}
		}

		helper::FreeEntry( entry );
	}

	static void ParallelForFuncEpilogueOnly( void* jobData, const RunContext& runContext )
	{
		auto* entry = static_cast<ParallelForJobEntry*>( jobData );
		RED_FATAL_ASSERT( entry );
		RED_FATAL_ASSERT( !entry->sharedCounterEntry );
		RED_FATAL_ASSERT( entry->epilogueFunc );

		auto* const sharedData = entry->sharedData;
		void* const elements = entry->elements;
		const Uint32 numElements = entry->numElements;
		entry->epilogueFunc( sharedData, elements, numElements, runContext );

		helper::FreeEntry( entry );
	}

	static void ParallelForFuncSingleTeam( void* jobData, const RunContext& runContext )
	{
		auto* entry = static_cast<ParallelForJobEntry*>( jobData );
		RED_FATAL_ASSERT( entry );
		RED_FATAL_ASSERT( !entry->sharedCounterEntry );

		auto* const sharedData = entry->sharedData;
		void* const elements = entry->elements;
		const Uint32 numElements = entry->numElements;
		// Not used in the epilogue, since should be agnostic which job finished
		RunContext parallelForRunContext = runContext;
		parallelForRunContext.parallelForTeamIndex = entry->teamIndex;
		entry->jobFunc( sharedData, elements, 0, numElements, parallelForRunContext );

		// #tbd: could even copy then release entry here before calling function to free the entry even sooner
		if ( entry->epilogueFunc )
		{
			entry->epilogueFunc( sharedData, elements, numElements, runContext );
		}

		helper::FreeEntry( entry );
	}

	static Uint32 CalcParallelForTeamSize( Uint32 numElements, Uint32 numElementsPerBatch, Uint32 numWorkers )
	{
		// If each paralell-for job will exclusively process a chunk, don't create more jobs than there are chunks
		// Otherwise it's just extra thread contention
	 	const Uint32 numBatches = ( numElements + numElementsPerBatch - 1 ) / numElementsPerBatch;
		return std::min( numBatches, numWorkers );	
	}
}

namespace helper
{
#ifdef MUST_PUMP_WIN32_MESSAGES_FOR_DXGI
	static void PumpWindowsMessagesForDXGI()
	{
		// Not bothering to filter as experimentally makes no difference and supported by technical reference. Quit message is posted, so won't miss processing 
		// it here. Doubtful PM_NOYIELD makes any practical difference here if some other process is waiting for the launcher to be initialized with WaitForInputIdle,
		// but using it as we're not processing all messages in the main loop.

		// See "GetMessage and PeekMessage Internals" by Bob Gunderson.
		/*
		Applications that do not desire window-handle filtering may pass a NULL value to GetMessageand PeekMessage in the hwnd parameter.
		Similarly, passing NULL values in bothuMsgFilterMin and uMsgFilterMax parameters disables message-range filtering.

		It is important to realize that only mouse and keyboard hardware messages, posted messages, WM_PAINT messages, and timer messages can be filtered.
		Most messages a window procedure receives are sent directly to the window using SendMessage.
		*/

		// mainthread check mainly for the sake of not making GPumpingMessagesOutsideMainLoop TLS, since PeekMessage would be a no-op anyway
		if ( ::SIsMainThread() )
		{
			MSG msg;
			PeekMessage( &msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE | PM_NOYIELD );
		}
	}
#endif

#ifdef MUST_PUMP_WIN32_MESSAGES_FOR_DXGI
	static void PumpWindowsMessagesForDXGIReminderJob( void* jobData, const RunContext& )
	{
		auto* const flag = static_cast< red::Atomic< Bool >* >( jobData );
		flag->SetValue( true );
	}
#endif

	class MessagePumper : red::NonCopyable
	{
	public:
		MessagePumper( Dispatcher& dispatcher, red::Atomic< Bool >& shouldPumpMessagesForDXGI )
			: m_dispatcher( dispatcher )
			, m_nextUpdateTick( 0 )
			, m_tickFrequency( 0 )
			, m_shouldPumpMessagesForDXGI( shouldPumpMessagesForDXGI )
			, m_sentPumpMessagesReminder( false )
		{
			RED_TOUCH( m_dispatcher );
			RED_TOUCH2( m_nextUpdateTick, m_tickFrequency );
			RED_TOUCH3( m_shouldPumpMessagesForDXGI, m_sentPumpMessagesReminder, m_isMainThread );

#ifdef MUST_PUMP_WIN32_MESSAGES_FOR_DXGI
			m_isMainThread = ::SIsMainThread();

			// Reset any previously abandoned value
			if ( m_isMainThread )
			{
				m_shouldPumpMessagesForDXGI.SetValue( false );
			}
			red::Clock::GetInstance().GetTimer().GetFrequency( m_tickFrequency );
#endif
		}

		void Update()
		{
#ifdef MUST_PUMP_WIN32_MESSAGES_FOR_DXGI
			if ( !m_isMainThread )
				return;

			// If we couldn't pop any more jobs, maybe the other threads are hung in Win32 API calls where messages are posted the HWND
			// So we need to pump messages. We queue this job because where this occurs on PC we should be able to tolerate a momentary FPS hitch
			// where the main thread has to drain the job queue to get to this: e.g., going from windowed to fullscreen mode etc.
			// And then we don't pump messages too often if it's simply the case that other threads are beating us to job entries.
			// If we kick the reminder job and finish, it doesn't matter if we abandon it. We reset the flag before entrering this loop.
			const Uint64 tickNow = red::Timer::GetTicks();
			if ( !m_sentPumpMessagesReminder && tickNow >= m_nextUpdateTick )
			{
				// One second until next pump
				m_nextUpdateTick = tickNow + m_tickFrequency;

				m_sentPumpMessagesReminder = true;
				JobDecl jobDecl;
				jobDecl.instrumentationObject = &s_instrumentationObject;
				jobDecl.jobFunc = &PumpWindowsMessagesForDXGIReminderJob;
				jobDecl.jobData = &m_shouldPumpMessagesForDXGI;
				jobDecl.hint = JobHint::Trivial;
				
				auto* dummyWaitCounter = m_dispatcher.InitJobCounter("PumpWindowsMessagesForDXGI", Priority::Latent, nullptr);
				auto* dummyAccumCounter = m_dispatcher.InitJobCounter("PumpWindowsMessagesForDXGI", Priority::Latent, nullptr);
				m_dispatcher.RunJob( jobDecl, dummyWaitCounter, dummyAccumCounter );

				m_dispatcher.ReleaseJobCounterInternal(dummyWaitCounter);
				m_dispatcher.ReleaseJobCounterInternal(dummyAccumCounter);
			}
			else if ( m_shouldPumpMessagesForDXGI.GetValue() )
			{
				m_sentPumpMessagesReminder = false;
				PumpWindowsMessagesForDXGI();
			}
#endif
		}

	private:
		static red::InstrumentationObject s_instrumentationObject;
		Dispatcher& m_dispatcher;
		Uint64 m_nextUpdateTick;
		Uint64 m_tickFrequency;
		red::Atomic< Bool >& m_shouldPumpMessagesForDXGI;
		Bool m_sentPumpMessagesReminder;
		Bool m_isMainThread;
	};

	red::InstrumentationObject MessagePumper::s_instrumentationObject{ "MessagePumper" };
}

Bool Dispatcher::FlushCounter( const CounterEntry* counterEntry, Priority processPriority, Int32 timeoutMillseconds, Bool processLarge )
{
	ALWAYSENABLED_RED_FATAL_ASSERT( ::SIsMainThread() && !g_tls_IsDispatcherThread, 
		"Can only FlushCounter() from the main thread "
		"BUT NOT from any job (regardless of it coincidentally or cleverly running on the main thread)" );

	RED_FATAL_ASSERT( counterEntry );

	red::ScopedFlag< Bool > scopedFlag { g_tls_IsDispatcherThread = true, false };

	// #tbd: if try to shutdown dispatcher while still has jobs waiting
	// generally shouldn't be supported, and shutdown should happen at a stable time
	// Can't infinitely nest waits or mitigation like stack too deep.
	RED_FATAL_ASSERT( !m_isFlushingCounter, "Reentrantly flushing counter!" );
	red::ScopedFlag< Bool > flushFlag( m_isFlushingCounter = true, false );

	red::ProfileTimer timeoutTimer;

	helper::MessagePumper pumper { *this, m_shouldPumpMessagesForDXGI };
    const double cpFlushStart=emscripten_get_now();
    cp2077JobTelemetry::Guard cpLiveWait(0,2,counterEntry->debugName,0,reinterpret_cast<std::uintptr_t>(counterEntry));
    wgtrace::CounterWaitScope cpCounterWait(&counterEntry->counterValue, WGTRACE_SITE("Dispatcher::FlushCounter"));
    double cpRunMs=0;unsigned cpRunCount=0,cpEmptyCount=0,cpRequeueCount=0;
    // [skperf] round 25: loop statistics for the critpath probe (probeMask bit 2), and opt-in ?flushSpin=1 (no
    // 0.05 ms sleeps while the frame counter is not zero: pure poll; default = the sleeps as before).
    static const int cpFlushSpin = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'flushSpin' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
    static const int cpFlushWait = [] { const int v = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'flushWait' ) === '0' ? 0 : 1; } catch ( e ) { return 1; } } );
        std::fprintf( stderr, "CP2077 skperf switches: flushWait=%d flushSpin=%d\n", v, cpFlushSpin ); return v; }();
    static const double cpSpinMs = [] {
        const int us = MAIN_THREAD_EM_ASM_INT( { try { const q = new URLSearchParams( location.search ); const v = q.get( 'flushSpinUs' ); return v !== null && /^[0-9]+$/.test( v ) ? Math.min( 2000, Number( v ) ) : 0; } catch ( e ) { return 0; } } );
        const int wake = MAIN_THREAD_EM_ASM_INT( { try { return new URLSearchParams( location.search ).get( 'flushWakeJobs' ) === '1' ? 1 : 0; } catch ( e ) { return 0; } } );
        g_cpWakeOnJobs.store( wake, std::memory_order_relaxed );
        std::fprintf( stderr, "CP2077 flushwake switches: flushSpinUs=%d flushWakeJobs=%d\n", us, wake );
        return us / 1000.0; }();
    const bool cpLoopProbe = ( cp2077::web::skperfEncodeProbe.load( std::memory_order_relaxed ) & 2 ) != 0;
    double cpIters=0,cpSleeps=0,cpSleepMs=0,cpSleepMax=0,cpLastJobEnd=0,cpTimeouts=0,cpTimedOutMax=0,cpWokenMax=0,cpMissed=0;
	while ( !counterEntry->counterValue.IsZero_Snapshot() )
	{
        cpIters+=1;
        const Uint32 cpEpoch0=g_cpWakeEpoch.load(std::memory_order_seq_cst); // [skperf] round 26
		if ( timeoutMillseconds > -1 && timeoutTimer.GetDeltaMsec() >= timeoutMillseconds )
		{		
			if (c_useJobDebugger && m_debugger)
			{
				AnalyzeCounter(*counterEntry);
			}
			return false;
		}

		JobQueueEntry entry;
		Priority priority = Priority::RenderPath;
		if ( TryPopJobQueueEntry( entry, priority ) )
		{
			priority = MapPriority( priority );
			const Bool isTrivialJob = entry.jobDecl.hint == JobHint::Trivial;
			const Bool isLargeJob = ( entry.jobDecl.hint == JobHint::Large || entry.jobDecl.hint == JobHint::AudioEvent );
			const Bool canRunJob = ( processLarge || !isLargeJob );
			if ( entry.accumulateCounterEntry == counterEntry || ( priority >= processPriority && canRunJob ) || isTrivialJob )
			{
				// Main thread is excluded from dispatcher threads but at the beginning of this function there's RED_FATAL_ASSERT( ::SIsMainThread() );
				// Therefore 0 index is reserved for the Main Thread 
const double cpRunStart=emscripten_get_now();
				DoRunJobQueueEntry( entry, 0, priority, *m_jobScopeAllocator, nullptr );
                cpLastJobEnd=emscripten_get_now();
                cpRunMs+=cpLastJobEnd-cpRunStart;++cpRunCount;
			}
			else
			{
				++cpRequeueCount;
                // Throw back onto the queue
				QueueJobAndSignal( entry.jobDecl, entry.accumulateCounterEntry, nullptr );
			}
		}
		else
		{
			// #tbd: if no more jobs to run and the dispatcher threads are finishing up the remaining jobs, we may pump messages a bit too frequently?
++cpEmptyCount;
			pumper.Update();
            // Short spin, then brief sleeps instead of burning a core while waiting on the counter.
            if(cpEmptyCount>64 && !emscripten_is_main_runtime_thread() && !cpFlushSpin) {
                const double cpS=cpLoopProbe?emscripten_get_now():0;
                if(cpFlushWait) {
                    // [skperf] round 26: cpEpoch0 was read at the top of this iteration, BEFORE the queue pop and the
                    // counter test, so any job queued / counter zeroed since then changed the word: no lost wake.
                    // [skperf] round 28: spin up to 150 us first (short gaps then cost no futex round trip at all)
                    {
                        const double cpSpinStart=emscripten_get_now();
                        while(g_cpWakeEpoch.load(std::memory_order_seq_cst)==cpEpoch0 && !counterEntry->counterValue.IsZero_Snapshot() &&
                              emscripten_get_now()-cpSpinStart<cpSpinMs) {}
                    }
                    g_cpWaiters.fetch_add(1,std::memory_order_seq_cst);
                    if(g_cpWakeEpoch.load(std::memory_order_seq_cst)==cpEpoch0 && !counterEntry->counterValue.IsZero_Snapshot()) {
                        const double cpW=cpLoopProbe?emscripten_get_now():0;
                        const int cpRc=emscripten_futex_wait(&g_cpWakeEpoch,cpEpoch0,2.0);
                        if(cpLoopProbe){ // [skperf] round 27: a timed-out wait = no notify arrived (expected ~2 ms, timer-rounded)
                            const double d=emscripten_get_now()-cpW;
                            if(cpRc==-ETIMEDOUT){cpTimeouts+=1;if(d>cpTimedOutMax)cpTimedOutMax=d;
                                // missed = the condition changed while we slept but no notify woke us
                                if(g_cpWakeEpoch.load(std::memory_order_seq_cst)!=cpEpoch0 || counterEntry->counterValue.IsZero_Snapshot()) cpMissed+=1;}
                            else if(d>cpWokenMax)cpWokenMax=d;
                        }
                    }
                    g_cpWaiters.fetch_sub(1,std::memory_order_seq_cst);
                } else emscripten_thread_sleep(0.05);
                if(cpLoopProbe){const double d=emscripten_get_now()-cpS;cpSleeps+=1;cpSleepMs+=d;if(d>cpSleepMax)cpSleepMax=d;}
            }
		}
	}
    if(cpLoopProbe) cp2077_skperf_crit_flushloop(emscripten_get_now(),cpIters,cpSleeps,cpSleepMs,cpSleepMax,cpRunMs,cpRequeueCount,cpLastJobEnd);
    if(cpLoopProbe) cp2077_skperf_crit_flushloop2(cpTimeouts,cpTimedOutMax,cpWokenMax,cpMissed);
    cpCounterWait.woke();
    static unsigned cpFlushCount=0;static unsigned cpSlowSamples=0;
    const unsigned cpN=++cpFlushCount;
    const double cpTotal=emscripten_get_now()-cpFlushStart;
    if(cpN<=4 || (cpN<=2048 && (cpN&(cpN-1))==0) || (cpTotal>=20 && ++cpSlowSamples<=8))
        std::fprintf(stderr,"CP2077 frame job wait probe: count=%u totalMs=%.3f ownJobsMs=%.3f otherWaitMs=%.3f jobs=%u emptyPolls=%u requeues=%u\n",cpN,cpTotal,cpRunMs,cpTotal-cpRunMs,cpRunCount,cpEmptyCount,cpRequeueCount);
	return true;
}


//#tbd: could assert that counterEntry value never increases while flushing.
//then could also stop trying to pop job entries once gone through the entire queue
Bool Dispatcher::FlushCounter( const CounterEntry* counterEntry, Bool processLatent, Int32 timeoutMillseconds )
{
	return FlushCounter( counterEntry, processLatent ? Priority::Latent : counterEntry->param.priority, timeoutMillseconds, true );
}

namespace helper
{
	static void InitParallelForEntry( prv::ParallelForJobEntry& entry, const JobDeclParallelFor& jobDeclParallelFor, void* resolvedSharedData, Uint32 teamSize, Uint32 teamIndex, ParallelForSharedCounterEntry* sharedCounterEntry )
	{
		entry.jobFunc = jobDeclParallelFor.jobFunc;
		entry.epilogueFunc = jobDeclParallelFor.epilogueFunc;
		entry.sharedData = resolvedSharedData;
		entry.elements = jobDeclParallelFor.elements;
		entry.sharedCounterEntry = sharedCounterEntry;
		entry.numElements = jobDeclParallelFor.numElements;
		entry.teamSize = teamSize;
		entry.teamIndex = teamIndex;
		entry.maxBatchSize = jobDeclParallelFor.maxBatchSize;
	}

	static void InitParallelForJobDecl( JobDecl& jobDecl, const JobDeclParallelFor& jobDeclParallelFor, prv::ParallelForJobEntry& entry, JobDecl::TJobFunc* parallelForFunc  )
	{
		jobDecl.jobFunc = parallelForFunc;
		jobDecl.jobData = &entry;
		jobDecl.instrumentationObject = jobDeclParallelFor.instrumentationObject;
		jobDecl.debugFlags = jobDeclParallelFor.debugFlags;
	}
}

void Dispatcher::RunParallelForJob( const JobDeclParallelFor& job, const CounterEntry* waitForZeroCounterEntry, CounterEntry* accumulateCounterEntry )
{
	RED_FATAL_ASSERT( job.jobFunc );

	Uint32 maxParallelForTeamSize = GetNumDispatcherThreads(false) + 1; // +1 for main thread as worker

#ifdef RED_CONSOLE_CORE_7_SUPPORT
	if (accumulateCounterEntry && accumulateCounterEntry->GetAffinity() == job::Affinity::ConsoleCore7)
	{
		// Only one core7, so no point splitting into further jobs
		maxParallelForTeamSize = 1;
	}
#endif

	const Uint32 teamSize = helper::CalcParallelForTeamSize( job.numElements, 1, maxParallelForTeamSize );
	RED_FATAL_ASSERT( teamSize > 0 || job.numElements == 0, "Sanity check failed: invalid zero teamSize calculation!" );

	ParallelForSharedCounterEntry* sharedCounterEntry = nullptr;
	JobDecl::TJobFunc* parallelForFunc = nullptr;
	Uint32 numJobs = 0;

	// Choose how to run the parallel-for function
	if ( teamSize == 0 )
	{
		if ( job.epilogueFunc )
		{
			// Always run epilogueFunc, regardless of whether any elements. Run it in a job too, to make sure it's in the proper order relative to other jobs.
			numJobs = 1;
			parallelForFunc = &helper::ParallelForFuncEpilogueOnly;
		}
	}
	else
	{
		numJobs = teamSize;
		if ( teamSize == 1 )
		{
			parallelForFunc = &helper::ParallelForFuncSingleTeam;
		}
		else
		{
			parallelForFunc = &helper::ParallelForFunc;
			sharedCounterEntry = helper::AllocEntry< ParallelForSharedCounterEntry>();
			RED_FATAL_ASSERT( sharedCounterEntry );
			sharedCounterEntry->counter.SetValue( 0 ); // can be atomic relaxed here
		}
	}
	
	RED_FATAL_ASSERT( parallelForFunc || numJobs == 0, "Did not select a parallel-for function to run!" );

	void* resolvedSharedData = job.sharedData;
	if ( job.initSharedDataCallback )
	{
		resolvedSharedData = job.initSharedDataCallback( teamSize, job.sharedData );
	}

	// Init the jobDecl for each parallel for worker
	for ( Uint32 i = 0; i < numJobs; ++i )
	{
		auto* parallelForEntry = helper::AllocEntry<ParallelForJobEntry>();
		RED_FATAL_ASSERT( parallelForEntry );
		helper::InitParallelForEntry( *parallelForEntry, job, resolvedSharedData, teamSize, i, sharedCounterEntry );

		JobDecl jobDecl;
		helper::InitParallelForJobDecl( jobDecl, job, *parallelForEntry, parallelForFunc );
		RunJob( jobDecl, waitForZeroCounterEntry, accumulateCounterEntry );
	}

	// Avoid any surprises breaking a dependency chain if the parallel-for was empty.
	// That is, always increment the counter even if nothing to do.
	if (numJobs == 0)
	{
		JobDecl jobDecl;
		InitWithEmptyJob(jobDecl);
		RunJob(jobDecl, waitForZeroCounterEntry, accumulateCounterEntry);
	}
}

void Dispatcher::QueueJobOrWait( const JobDecl& job, const CounterEntry* waitForZeroCounterEntry, CounterEntry* accumulateCounterEntry )
{
	const StackTraceCacheEntryPath* debugTrace = nullptr;

	if ( c_useJobDebugger && m_debugger )
	{
		debugTrace = m_debugger->TraceCall();
	}

	if ( waitForZeroCounterEntry )
	{
		if ( TryPutOnWaitingList( job, *waitForZeroCounterEntry, accumulateCounterEntry, debugTrace ) )
		{
			return;
		}
	}

	QueueJobAndSignal( job, accumulateCounterEntry, nullptr );
}

} } // prv/jobs
