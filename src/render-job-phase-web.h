#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <emscripten.h>
#include "engine-wgtrace-web.h"
namespace cpRenderJobPhase {
struct alignas(64) Slot {
    std::atomic<std::uint32_t> sequence{0};
    std::uint32_t phase=0;
    double beganAt=0;
    std::uint64_t thread=0;
    char name[88]{};
};
static_assert(sizeof(Slot)==128);
inline Slot slots[64];
inline std::once_flag registration;
inline void Register() {
    MAIN_THREAD_EM_ASM({
        const base=Number($0);
        Module['cp2077RenderJobPhaseSnapshot']=()=>{
            growMemViews();const rows=[];
            for(let worker=0;worker<64;worker++){
                const p=base+worker*128;
                for(let attempt=0;attempt<3;attempt++){
                    const before=Atomics.load(HEAPU32,p/4);if(before&1)continue;
                    const phase=HEAPU32[(p+4)/4];if(!phase)break;
                    const row={};row.worker=worker;row.phase=phase;
                    row.beganAt=HEAPF64[(p+8)/8];row.thread=String(HEAPU64[(p+16)/8]);
                    row.node=UTF8ArrayToString(HEAPU8,p+24,88);
                    if(before!==Atomics.load(HEAPU32,p/4))continue;
                    row.ageMs=Math.max(0,Date.now()-row.beganAt);rows.push(row);break;
                }
            }return rows;
        };
    },slots);
}
struct Guard {
    unsigned traceName=0,traceJob=0,traceFrame=0,tracePhase=1;double traceAt=0;
    Slot* slot=nullptr;
    std::uint32_t savedPhase=0;
    double savedAt=0;
    std::uint64_t savedThread=0;
    char savedName[88]{};
    Guard(unsigned worker,const char* name) {
        if(cpEngineTrace::Enabled()){traceName=cpEngineTrace::NameId(name);traceJob=wgtrace::tl_job;traceFrame=cpEngineTrace::FrameId();traceAt=wgtrace::now();}
        std::call_once(registration,Register);
        if(worker>=64||!cpEngineTrace::ProbesEnabled())return; // perf12: live phase slots only with ?probes=1
        slot=&slots[worker];savedPhase=slot->phase;savedAt=slot->beganAt;savedThread=slot->thread;
        std::memcpy(savedName,slot->name,sizeof(savedName));
        slot->sequence.fetch_add(1,std::memory_order_acq_rel);
        slot->thread=std::uintptr_t(pthread_self());
        std::memset(slot->name,0,sizeof(slot->name));
        if(name)std::strncpy(slot->name,name,sizeof(slot->name)-1);
        slot->phase=1;slot->beganAt=emscripten_get_now();
        slot->sequence.fetch_add(1,std::memory_order_release);
    }
    void Phase(unsigned phase) {
        if(traceAt){const double now=wgtrace::now();cpEngineTrace::Emit(20,traceFrame,traceJob,traceName,tracePhase,traceAt,now);traceAt=now;tracePhase=phase;}
        if(!slot)return;
        slot->sequence.fetch_add(1,std::memory_order_acq_rel);
        slot->phase=phase;slot->beganAt=emscripten_get_now();
        slot->sequence.fetch_add(1,std::memory_order_release);
    }
    ~Guard() {
        if(traceAt)cpEngineTrace::Emit(20,traceFrame,traceJob,traceName,tracePhase,traceAt,wgtrace::now());
        if(!slot)return;
        slot->sequence.fetch_add(1,std::memory_order_acq_rel);
        slot->phase=savedPhase;slot->beganAt=savedAt;slot->thread=savedThread;
        std::memcpy(slot->name,savedName,sizeof(savedName));
        slot->sequence.fetch_add(1,std::memory_order_release);
    }
};
}
