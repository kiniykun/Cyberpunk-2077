#include "oodle-hooks-web.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
namespace {
std::mutex allocatorMutex;
t_fp_OodleCore_Plugin_MallocAligned* allocateHook=nullptr;
t_fp_OodleCore_Plugin_Free* freeHook=nullptr;
bool configured=false;
std::atomic<t_fp_OodleCore_Plugin_DisplayAssertion*> assertionHook{nullptr};
void DefaultFree(void* pointer) {std::free(pointer);}
}
void CpWebOodleReportAssertion(const char* file,int line,const char* function,const char* message) {
    auto* callback=assertionHook.load(std::memory_order_acquire);
    if(callback) {
        if(callback(file,line,function,message))std::abort();
    } else std::fprintf(stderr,"Oodle assertion %s:%d (%s): %s\n",file,line,function,message);
}
void OodleCore_Plugins_SetAllocators(t_fp_OodleCore_Plugin_MallocAligned* allocate,t_fp_OodleCore_Plugin_Free* release) {
    if((allocate==nullptr)!=(release==nullptr)) {
        CpWebOodleReportAssertion(__FILE__,__LINE__,__func__,"An allocator and matching free callback must be supplied together");return;
    }
    bool changed=false;
    {
        std::lock_guard<std::mutex> lock(allocatorMutex);
        changed=configured&&(allocateHook!=allocate||freeHook!=release);
        if(!changed) {allocateHook=allocate;freeHook=release;configured=true;}
    }
    // The original SDK requires that the allocator pair never be replaced after
    // registration. Release the lock before calling external diagnostic code.
    if(changed)CpWebOodleReportAssertion(__FILE__,__LINE__,__func__,"Allocator callbacks cannot be changed after registration");
}
t_fp_OodleCore_Plugin_DisplayAssertion* OodleCore_Plugins_SetAssertion(t_fp_OodleCore_Plugin_DisplayAssertion* callback) {
    return assertionHook.exchange(callback,std::memory_order_acq_rel);
}
bool CpWebOodleAllocateWorkspace(SINTa bytes,CpWebOodleWorkspace& workspace) {
    if(bytes<=0||workspace.data)return false;
    t_fp_OodleCore_Plugin_MallocAligned* allocate;
    t_fp_OodleCore_Plugin_Free* release;
    {std::lock_guard<std::mutex> lock(allocatorMutex);allocate=allocateHook;release=freeHook;}
    void* memory=nullptr;
    if(allocate)memory=allocate(bytes,16);
    else if(posix_memalign(&memory,16,static_cast<size_t>(bytes))!=0)memory=nullptr;
    if(!memory)return false;
    if(reinterpret_cast<std::uintptr_t>(memory)%16) {
        (release?release:DefaultFree)(memory);
        CpWebOodleReportAssertion(__FILE__,__LINE__,__func__,"Allocator returned a misaligned pointer");return false;
    }
    workspace.data=memory;workspace.release=release?release:DefaultFree;
    return true;
}
