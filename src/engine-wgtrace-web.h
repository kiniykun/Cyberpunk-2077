#pragma once
#include "../tools/wgpu-trace/runtime/wgtrace.h"
#include <mutex>
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <emscripten/em_asm.h>
namespace cpEngineTrace {
inline std::once_flag initialized;
inline std::atomic<unsigned> activeFrame{0},frameCounter{0};
inline unsigned startFrame=300,captureFrames=64;
inline bool requested=false;
struct Name {std::atomic<unsigned> hash{0};char text[124]{};};
static_assert(sizeof(Name)==128);
inline Name names[1024];inline std::mutex namesMutex;
inline unsigned NameId(const char* text){
 if(!text)text="<unnamed>";unsigned hash=2166136261u;
 for(const char* p=text;*p;++p)hash=(hash^unsigned(static_cast<unsigned char>(*p)))*16777619u;
 if(!hash)hash=1;
 for(unsigned probe=0;probe<1024;probe++){const unsigned i=(hash+probe)&1023;auto& entry=names[i];const auto stored=entry.hash.load(std::memory_order_acquire);
  if(stored==hash&&std::strncmp(entry.text,text,123)==0)return i+1;
  if(!stored){std::lock_guard<std::mutex> lock(namesMutex);if(entry.hash.load())continue;std::strncpy(entry.text,text,123);entry.hash.store(hash,std::memory_order_release);return i+1;}
 }return 0;
}
inline void Initialize(){
    wgtrace::set_capacity(8192);wgtrace::set_enabled(false);
    MAIN_THREAD_EM_ASM({
        Module['cp2077TraceExports']={};
        const e=Module['cp2077TraceExports'];
        e._wgtrace_ring_count=()=>_wgtrace_ring_count();
        e._wgtrace_ring_ptr=i=>_wgtrace_ring_ptr(i);
        e._wgtrace_ring_len=i=>_wgtrace_ring_len(i);
        e._wgtrace_ring_dropped=i=>_wgtrace_ring_dropped(i);
        e._wgtrace_reset=()=>_wgtrace_reset();
        e._wgtrace_next_id=k=>_wgtrace_next_id(k);
        e._wgtrace_set_enabled=on=>_wgtrace_set_enabled(on);
        e._wgtrace_site_count=()=>_wgtrace_site_count();
        e._wgtrace_site_name=i=>_wgtrace_site_name(i);
        e._wgtrace_ctr_table_full=()=>_wgtrace_ctr_table_full();
        e._wgtrace_unregistered_emits=()=>_wgtrace_unregistered_emits();
        Module['cp2077TraceHeapProvider']=()=>{growMemViews();return HEAPU8;};
        const base=Number($0);Module['cp2077TraceReadNames']=()=>{growMemViews();const result={};for(let i=0;i<1024;i++){const p=base+i*128;if(Atomics.load(HEAPU32,p/4))result[i+1]=UTF8ArrayToString(HEAPU8,p+4,124);}return result;};
    },names);
    requested=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('gpuTrace')==='1';});
    startFrame=MAIN_THREAD_EM_ASM_INT({const n=Number(new URLSearchParams(location.search).get('traceStartFrame')||300);return Number.isFinite(n)?Math.max(1,Math.min(100000,n)):300;});
    captureFrames=MAIN_THREAD_EM_ASM_INT({const n=Number(new URLSearchParams(location.search).get('traceFrames')||64);return Number.isFinite(n)?Math.max(1,Math.min(512,n)):64;});
}
inline void Ensure(){std::call_once(initialized,Initialize);}
inline bool Enabled(){return wgtrace::g_enabled.load(std::memory_order_relaxed)!=0;}
// perf12: per-call timing probes (draw stage cost, handoff/binding/allocation wait timers, job and render-node
// timers) are off by default; ?probes=1 turns them on. Published once on the main thread at WebGPU device creation
// (InitProbesOnMain); until then probes are off. Low-frequency summary and count logs stay on either way.
inline std::atomic<int> probesFlag{0};
inline bool ProbesEnabled(){return probesFlag.load(std::memory_order_relaxed)!=0;}
inline void InitProbesOnMain(){
    const int value=EM_ASM_INT({return new URLSearchParams(location.search).get('probes')==='1'?1:0;});
    probesFlag.store(value,std::memory_order_relaxed);
    std::fprintf(stderr,"CP2077 perf12 switches: probes=%d\n",value);
}
inline void Register(){if(Enabled()&&!wgtrace::tl_ring)wgtrace::thread_register(unsigned(std::uintptr_t(pthread_self())));}
inline unsigned FrameId(){return wgtrace::tl_frame?wgtrace::tl_frame:activeFrame.load(std::memory_order_relaxed);}
inline void Emit(unsigned type,unsigned frame,unsigned a,unsigned b,unsigned c,double begin,double end){Register();wgtrace::emit(type,frame,a,b,c,begin,end);}
struct Request {
 unsigned frame=0,job=0,req=0,scope=0,operation=0;double queued=0,began=0,encoded=0;
 void Queue(const char* name=nullptr){if(!Enabled())return;frame=FrameId();job=wgtrace::tl_job;req=wgtrace::next_id(wgtrace::ID_REQ);operation=NameId(name);queued=wgtrace::now();}
 void Begin(){if(!req)return;began=wgtrace::now();Emit(wgtrace::PROXY_DEQ,frame,req,0,0,0,began);}
 void CallEnd(){if(!req)return;encoded=wgtrace::now();Emit(wgtrace::PROXY_CALL,frame,req,operation,0,began,encoded);}
 void Scoped(bool deferred=false){if(!req)return;if(!scope)scope=wgtrace::next_id(wgtrace::ID_SCOPE);Emit(wgtrace::SCOPE,frame,scope,0,deferred?0:req,began,wgtrace::now());}
 void Settled(bool failed){if(scope)Emit(wgtrace::SETTLE,frame,scope,1,failed?1:0,wgtrace::now(),0);}
 void WaitEnd(){if(req&&began)Emit(wgtrace::WAIT,frame,job,wgtrace::W_PROXY,req,queued,wgtrace::now());}
};
struct Frame {
 unsigned id=0,job=0,previousJob=0,previousFrame=0;double began=0;
 Frame(){Ensure();const auto n=frameCounter.fetch_add(1)+1;if(!requested||n<startFrame||n>=startFrame+captureFrames)return;
    wgtrace::set_enabled(true);Register();id=wgtrace::next_id(wgtrace::ID_FRAME);job=wgtrace::next_id(wgtrace::ID_JOB);
    previousJob=wgtrace::tl_job;previousFrame=wgtrace::tl_frame;wgtrace::tl_job=job;wgtrace::tl_frame=id;activeFrame.store(id);began=wgtrace::now();}
 ~Frame(){if(!id)return;const double ended=wgtrace::now();Emit(wgtrace::JOB_RUN,id,job,0,0,began,ended);Emit(wgtrace::FRAME,id,0,0,0,began,ended);
    wgtrace::tl_job=previousJob;wgtrace::tl_frame=previousFrame;
    if(frameCounter.load()>=startFrame+captureFrames-1){wgtrace::set_enabled(false);MAIN_THREAD_EM_ASM({Module['cp2077TraceCaptureComplete']=true;});}}
};
}
