#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/bink2/include/bink.h"
#include <emscripten.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace {
std::mutex mutex;
BINKMEMALLOC allocateHook=nullptr;
BINKMEMFREE releaseHook=nullptr;
struct Allocation {void* pointer;BINKMEMFREE release;};
std::unordered_map<std::uintptr_t,Allocation> allocations;
void RADLINK DefaultFree(void* pointer) {std::free(pointer);}
void* DefaultAllocate(U64 bytes) {
    void* pointer=nullptr;
    if(bytes>128u*1024u*1024u||posix_memalign(&pointer,32,bytes?static_cast<size_t>(bytes):16)!=0)
        return reinterpret_cast<void*>(UINTPTR_MAX);
    return pointer;
}
}
void RADEXPLINK BinkSetMemory(BINKMEMALLOC allocate,BINKMEMFREE release) {
    if((allocate==nullptr)!=(release==nullptr)) {
        std::fprintf(stderr,"Bink requires a matching allocator and free callback\n");return;
    }
    std::lock_guard<std::mutex> guard(mutex);allocateHook=allocate;releaseHook=release;
}
extern "C" EMSCRIPTEN_KEEPALIVE std::uintptr_t CpWebBinkAllocateSdk(U64 bytes) {
    BINKMEMALLOC allocate;BINKMEMFREE release;
    {std::lock_guard<std::mutex> guard(mutex);allocate=allocateHook;release=releaseHook;}
    void* pointer=allocate?allocate(bytes):DefaultAllocate(bytes);
    // The original game uses -1 to forbid SDK fallback. A null callback result
    // permits real default allocation, with its own matching free operation.
    if(pointer==reinterpret_cast<void*>(UINTPTR_MAX))return UINTPTR_MAX;
    if(!pointer) {pointer=DefaultAllocate(bytes);release=DefaultFree;}
    if(pointer==reinterpret_cast<void*>(UINTPTR_MAX))return UINTPTR_MAX;
    if(!release)release=DefaultFree;
    const auto address=reinterpret_cast<std::uintptr_t>(pointer);
    std::lock_guard<std::mutex> guard(mutex);
    if(allocations.count(address)) {
        std::fprintf(stderr,"Bink allocator returned an already owned backing pointer\n");return UINTPTR_MAX;
    }
    allocations.emplace(address,Allocation{pointer,release});return address;
}
extern "C" EMSCRIPTEN_KEEPALIVE int CpWebBinkFreeSdk(std::uintptr_t address) {
    Allocation allocation;
    {
        std::lock_guard<std::mutex> guard(mutex);
        auto found=allocations.find(address);if(found==allocations.end())return 0;
        allocation=found->second;allocations.erase(found);
    }
    allocation.release(allocation.pointer);return 1;
}
extern "C" EMSCRIPTEN_KEEPALIVE void CpWebBinkReleaseSdkMemory() {
    // Only after the CPU worker explicitly reports a terminal idle fault.
    // A timeout alone does not prove it has stopped touching shared backing.
    std::vector<Allocation> retired;
    {
        std::lock_guard<std::mutex> guard(mutex);
        retired.reserve(allocations.size());for(const auto& pair:allocations)retired.push_back(pair.second);
        allocations.clear();
    }
    for(const auto& allocation:retired)allocation.release(allocation.pointer);
}
