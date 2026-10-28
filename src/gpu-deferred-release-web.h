#pragma once
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <atomic>
#include <new>

namespace cp2077::web {
inline em_proxying_queue* GpuReferenceReleaseQueue(){
    static auto* queue=em_proxying_queue_create();return queue;
}
inline std::atomic<unsigned>& GpuPendingReferenceReleases(){
    static std::atomic<unsigned> pending{0};return pending;
}
// Ownership of one existing reference transfers to the queued callback.
// Object destruction stays on the browser thread that owns its JS mapping.
// Explicit GPU resource destruction and command ordering are unchanged.
inline bool QueueGpuReferenceRelease(void* object,void(*release)(void*)){
    if(!object)return true;
    if(emscripten_is_main_runtime_thread()){release(object);return true;}
    auto* queue=GpuReferenceReleaseQueue();if(!queue)return false;
    auto& pending=GpuPendingReferenceReleases();
    if(pending.fetch_add(1,std::memory_order_acq_rel)>=256){
        pending.fetch_sub(1,std::memory_order_acq_rel);
        return emscripten_proxy_sync(queue,emscripten_main_runtime_thread_id(),release,object);
    }
    struct Release {void* object;void(*function)(void*);};
    auto* request=new(std::nothrow) Release{object,release};
    if(!request){pending.fetch_sub(1,std::memory_order_acq_rel);return false;}
    if(!emscripten_proxy_async(queue,emscripten_main_runtime_thread_id(),[](void* data){
        auto* request=static_cast<Release*>(data);
        request->function(request->object);
        delete request;
        GpuPendingReferenceReleases().fetch_sub(1,std::memory_order_acq_rel);
    },request)){
        delete request;pending.fetch_sub(1,std::memory_order_acq_rel);return false;
    }
    return true;
}
// A FIFO barrier for quiescent shutdown and lifecycle verification.
inline bool FlushGpuReferenceReleases(){
    auto* queue=GpuReferenceReleaseQueue();if(!queue)return false;
    if(emscripten_is_main_runtime_thread()){emscripten_proxy_execute_queue(queue);return true;}
    return emscripten_proxy_sync(queue,emscripten_main_runtime_thread_id(),[](void*){},nullptr);
}
}
