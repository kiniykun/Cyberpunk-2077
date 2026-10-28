// Observe an unfinished lock wait as well as completed timings. No GPU work
// or locking policy changes; workers publish to a bounded shared-memory table.
struct alignas(64) CpCommandLockSlot {
    std::atomic<std::uint32_t> sequence{0};
    std::uint32_t phase=0;
    double beganAt=0;
    std::uint64_t thread=0;
    std::uint32_t line=0;
    char function[88]{};
};
static_assert(sizeof(CpCommandLockSlot)==128);
static CpCommandLockSlot cpCommandLockSlots[64];
static std::atomic<unsigned> cpCommandLockNextSlot{0};
static std::once_flag cpCommandLockRegistration;
static void CpRegisterCommandLockSnapshot() {
    MAIN_THREAD_EM_ASM({
        const base=Number($0);
        Module['cp2077CommandLockSnapshot']=()=>{
            growMemViews();const rows=[];
            for(let slot=0;slot<64;slot++){
                const p=base+slot*128;
                for(let attempt=0;attempt<3;attempt++){
                    const before=Atomics.load(HEAPU32,p/4);if(before&1)continue;
                    const phase=HEAPU32[(p+4)/4];if(!phase)break;
                    const row={};row.slot=slot;row.phase=phase===1?'waiting':'holding';
                    row.beganAt=HEAPF64[(p+8)/8];row.thread=String(HEAPU64[(p+16)/8]);
                    row.line=HEAPU32[(p+24)/4];row.function=UTF8ArrayToString(HEAPU8,p+28,88);
                    if(before!==Atomics.load(HEAPU32,p/4))continue;
                    row.ageMs=Date.now()-row.beganAt;rows.push(row);break;
                }
            }return rows;
        };
    },cpCommandLockSlots);
}
// Outside an active trace capture this is a plain lock/unlock. The diagnostic path below costs several JS
// clock calls, two telemetry publishes and a stats search per acquisition; at ~3,400 acquisitions per menu
// frame that overhead landed on every render worker. Mutual exclusion is identical on both paths.
class CpCommandLock {
    unsigned traceFrame=0,traceJob=0,traceName=0;bool trace=false;bool fast=false;
    std::unique_lock<std::mutex> lock;
    const char* function;const char* file;unsigned line;
    double begin,acquired;
    double releasedAt=0;
    CpCommandLockSlot* telemetry=nullptr;
    void Publish(unsigned phase,double at) {
        if(!telemetry)return;
        telemetry->sequence.fetch_add(1,std::memory_order_acq_rel);
        telemetry->phase=phase;telemetry->beganAt=at;
        telemetry->thread=std::uintptr_t(pthread_self());telemetry->line=line;
        std::memset(telemetry->function,0,sizeof(telemetry->function));
        std::strncpy(telemetry->function,function,sizeof(telemetry->function)-1);
        telemetry->sequence.fetch_add(1,std::memory_order_release);
    }
public:
    CpCommandLock(std::mutex& mutex,const char* name,const char* source,unsigned location)
        :lock(mutex,std::defer_lock),function(name),file(source),line(location),begin(0),acquired(0) {
        if(!cpEngineTrace::Enabled()){fast=true;
            // [cmdlock] uncapped contention telemetry: a clock read only when the lock is actually contended.
            if(!lock.try_lock()){const double t0=emscripten_get_now();lock.lock();
                cmdlockCounters[5].fetch_add(1,std::memory_order_relaxed);
                cmdlockCounters[6].fetch_add((unsigned long long)((emscripten_get_now()-t0)*1000.0),std::memory_order_relaxed);}
            return;}
        begin=emscripten_get_now();
        std::call_once(cpCommandLockRegistration,CpRegisterCommandLockSnapshot);
        static thread_local unsigned slot=cpCommandLockNextSlot.fetch_add(1,std::memory_order_relaxed);
        if(slot<64)telemetry=&cpCommandLockSlots[slot];
        Publish(1,begin);
        trace=cpEngineTrace::Enabled();if(trace){traceFrame=cpEngineTrace::FrameId();traceJob=wgtrace::tl_job;traceName=cpEngineTrace::NameId(name);}
        lock.lock();acquired=emscripten_get_now();Publish(2,acquired);
        if(trace){cpEngineTrace::Emit(18,traceFrame,traceJob,traceName,line,begin,acquired);cpEngineTrace::Emit(wgtrace::WAIT,traceFrame,traceJob,wgtrace::W_OTHER,0,begin,acquired);}
    }
    ~CpCommandLock(){
        if(fast){if(lock.owns_lock())lock.unlock();return;}
        const double end=releasedAt?releasedAt:emscripten_get_now();if(lock.owns_lock()){Publish(0,end);lock.unlock();}
        if(trace)cpEngineTrace::Emit(19,traceFrame,traceJob,traceName,line,acquired,end);
        const double waited=acquired-begin,held=end-acquired;
        struct Record {const char* file=nullptr;unsigned line=0,count=0,slow=0;double wait=0,hold=0,maxWait=0,maxHold=0;};
        static thread_local Record records[96];static thread_local unsigned used=0;
        Record* record=nullptr;
        for(unsigned i=0;i<used;++i)if(records[i].file==file && records[i].line==line){record=&records[i];break;}
        if(!record && used<96){record=&records[used++];record->file=file;record->line=line;}
        if(!record)return;
        const unsigned n=++record->count;record->wait+=waited;record->hold+=held;
        record->maxWait=std::max(record->maxWait,waited);record->maxHold=std::max(record->maxHold,held);
        if(n<=2 || (n<=4096 && (n&(n-1))==0) || ((waited>=10 || held>=10) && ++record->slow<=6))
            std::fprintf(stderr,"CP2077 command lock cost probe: function=%s line=%u count=%u waitMs=%.3f holdMs=%.3f totalWaitMs=%.3f totalHoldMs=%.3f maxWaitMs=%.3f maxHoldMs=%.3f\n",
                function,line,n,waited,held,record->wait,record->hold,record->maxWait,record->maxHold);
    }
    void UnlockForOwnedOperation(){if(fast){lock.unlock();return;}releasedAt=emscripten_get_now();Publish(0,releasedAt);lock.unlock();}
};
