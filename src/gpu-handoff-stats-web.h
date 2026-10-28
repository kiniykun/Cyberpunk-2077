#pragma once
#include <atomic>
#include <cstring>
#include <mutex>
#include <emscripten.h>
namespace cpGpuHandoffStats {
struct alignas(64) Row {
    std::atomic<unsigned> sequence{0};
    unsigned calls=0;
    double wallMs=0, executionMs=0, deliveryMs=0, returnMs=0, maxMs=0, validationMs=0;
    char name[72]{};
};
static_assert(sizeof(Row)==128);
inline Row rows[64*24];
inline std::atomic<unsigned> nextWorker{0};
inline std::once_flag registration;
inline void Register() {
    MAIN_THREAD_EM_ASM({
        const base=Number($0);
        Module['cp2077GpuHandoffSnapshot']=()=>{
            growMemViews();const result=[];
            for(let i=0;i<64*24;i++){
                const p=base+i*128;
                for(let attempt=0;attempt<3;attempt++){
                    const before=Atomics.load(HEAPU32,p/4);if(before&1)continue;
                    const calls=HEAPU32[(p+4)/4];if(!calls)break;
                    const row={};row.worker=Math.floor(i/24);row.calls=calls;
                    row.wallMs=HEAPF64[(p+8)/8];row.executionMs=HEAPF64[(p+16)/8];
                    row.deliveryMs=HEAPF64[(p+24)/8];row.returnMs=HEAPF64[(p+32)/8];
                    row.maxMs=HEAPF64[(p+40)/8];row.validationMs=HEAPF64[(p+48)/8];
                    row.operation=UTF8ArrayToString(HEAPU8,p+56,72);
                    if(before!==Atomics.load(HEAPU32,p/4))continue;
                    result.push(row);break;
                }
            }return result;
        };
    },rows);
}
inline void Record(const char* operation,double wall,double execution,double delivery,double returned,double validation=0) {
    // Each worker exclusively writes its own fixed table; no render-path mutex.
    static thread_local unsigned worker=nextWorker.fetch_add(1,std::memory_order_relaxed);
    if(worker>=64)return;
    Row* row=nullptr;
    for(unsigned i=0;i<24;i++) {
        auto& candidate=rows[worker*24+i];
        if(!candidate.calls || std::strcmp(candidate.name,operation)==0){row=&candidate;break;}
    }
    if(!row)return;
    row->sequence.fetch_add(1,std::memory_order_acq_rel);
    if(!row->calls)std::strncpy(row->name,operation,sizeof(row->name)-1);
    ++row->calls;row->wallMs+=wall;row->executionMs+=execution;
    row->deliveryMs+=delivery;row->returnMs+=returned;
    row->validationMs+=validation;
    if(wall>row->maxMs)row->maxMs=wall;
    row->sequence.fetch_add(1,std::memory_order_release);
}
}
