#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/threading.h>
#include "engine-wgtrace-web.h"

namespace cp2077JobTelemetry {
struct alignas(64) Slot {
    std::atomic<std::uint32_t> sequence{0};
    std::uint32_t phase=0;
    double beganAt=0;
    std::uint64_t function=0,counter=0,thread=0;
    char label[88]{};
};
static_assert(sizeof(Slot)==128 && offsetof(Slot,label)==40);
static Slot slots[64];
inline void Register() {
    MAIN_THREAD_EM_ASM({
        const base=Number($0);
        Module['cp2077NativeJobSnapshot']=function(){
            growMemViews();
            const rows=[];
            for(let worker=0;worker<64;worker++){
                const p=base+worker*128;
                for(let attempt=0;attempt<3;attempt++){
                    const before=Atomics.load(HEAPU32,p/4);
                    if(before&1)continue;
                    const phase=HEAPU32[(p+4)/4];
                    if(!phase)break;
                    const row={};row.worker=worker;row.phase=phase;
                    row.beganAt=HEAPF64[(p+8)/8];
                    row.function=String(HEAPU64[(p+16)/8]);
                    row.counter=String(HEAPU64[(p+24)/8]);
                    row.thread=String(HEAPU64[(p+32)/8]);
                    row.label=UTF8ArrayToString(HEAPU8,p+40,88);
                    if(before!==Atomics.load(HEAPU32,p/4))continue;
                    row.ageMs=Date.now()-row.beganAt;rows.push(row);break;
                }
            }
            return rows;
        };
    },slots);
}
struct Guard {
    unsigned tracePhase=0,traceFrame=0,traceJob=0,previousJob=0,traceWorker=0,traceCounter=0,traceName=0;double traceBegan=0;
    Slot* slot=nullptr;
    std::uint32_t phase=0;
    double beganAt=0;
    std::uint64_t function=0,counter=0,thread=0;
    char label[88]{};
    Guard(unsigned worker,unsigned nextPhase,const char* name,std::uintptr_t jobFunction,std::uintptr_t waitingCounter) {
        if(!wgtrace::tl_ring)wgtrace::thread_register(unsigned(std::uintptr_t(pthread_self())));
        if(cpEngineTrace::Enabled()){
            cpEngineTrace::Register();tracePhase=nextPhase;traceFrame=wgtrace::tl_frame;traceBegan=wgtrace::now();previousJob=wgtrace::tl_job;
            traceWorker=worker;traceCounter=waitingCounter<=UINT32_MAX?unsigned(waitingCounter):0;traceName=cpEngineTrace::NameId(name);
            if(nextPhase==1){traceJob=wgtrace::next_id(wgtrace::ID_JOB);wgtrace::tl_job=traceJob;}
            else traceJob=wgtrace::tl_job;
        }
        if(worker>=64)return;
        slot=&slots[worker];phase=slot->phase;beganAt=slot->beganAt;
        function=slot->function;counter=slot->counter;thread=slot->thread;
        std::memcpy(label,slot->label,sizeof(label));
        slot->sequence.fetch_add(1,std::memory_order_acq_rel);
        slot->phase=nextPhase;slot->beganAt=emscripten_get_now();
        slot->function=jobFunction;slot->counter=waitingCounter;slot->thread=(std::uintptr_t)pthread_self();
        std::memset(slot->label,0,sizeof(slot->label));
        if(name)std::strncpy(slot->label,name,sizeof(slot->label)-1);
        slot->sequence.fetch_add(1,std::memory_order_release);
    }
    ~Guard(){
        if(tracePhase){const auto end=wgtrace::now();
            if(tracePhase==1){cpEngineTrace::Emit(16,traceFrame,traceJob,traceCounter,traceName,traceBegan,end);cpEngineTrace::Emit(wgtrace::JOB_RUN,traceFrame,traceJob,traceWorker,0,traceBegan,end);wgtrace::tl_job=previousJob;}
            else {cpEngineTrace::Emit(17,traceFrame,traceJob,traceCounter,traceName,traceBegan,end);cpEngineTrace::Emit(wgtrace::WAIT,traceFrame,traceJob,wgtrace::W_OTHER,traceCounter,traceBegan,end);}
        }
        if(!slot)return;
        slot->sequence.fetch_add(1,std::memory_order_acq_rel);
        slot->phase=phase;slot->beganAt=beganAt;slot->function=function;slot->counter=counter;slot->thread=thread;
        std::memcpy(slot->label,label,sizeof(label));
        slot->sequence.fetch_add(1,std::memory_order_release);
    }
    Guard(const Guard&)=delete;
    Guard& operator=(const Guard&)=delete;
};
}
