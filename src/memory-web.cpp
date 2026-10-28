#include "build.h"
#include "memory-web.h"
#include "callstack-web.h"
#include "callstackCollectorConstant.h"
#include "flags.h"
#include <emscripten/heap.h>
#include <emscripten/emscripten.h>
#include <pthread.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <atomic>

namespace red { namespace memory {
namespace {
constexpr u64 Page=65536;
std::atomic<unsigned> reservationSequence{0};
struct Range {
    Range* next;
    SystemPageAllocatorWeb* owner;
    u64 begin,bytes,committedBytes,peakCommittedBytes;
    unsigned sequence;
    // A page is either logically uncommitted or committed by one allocator.
    SystemAllocatorWeb** committed;
};
Range* ranges=nullptr;
pthread_mutex_t rangeMutex=PTHREAD_MUTEX_INITIALIZER;
struct Lock { Lock(){pthread_mutex_lock(&rangeMutex);} ~Lock(){pthread_mutex_unlock(&rangeMutex);} };
[[noreturn]] void invalidRange(){emscripten_log(EM_LOG_ERROR,"Invalid browser allocator range or commit transition");std::abort();}
Range* find(u64 address,u64 size) {
    if(!size || address>std::numeric_limits<u64>::max()-size)return nullptr;
    for(Range* r=ranges;r;r=r->next)if(address>=r->begin && address+size<=r->begin+r->bytes)return r;
    return nullptr;
}
bool rounded(u64 bytes,u64& result) {
    if(!bytes || bytes>std::numeric_limits<u64>::max()-(Page-1))return false;
    result=(bytes+Page-1)&~(Page-1);return result<=std::numeric_limits<size_t>::max();
}
bool gpuFlags(u32 flags){return (flags&(Flags_GPU_Read_Write|Flags_GPU_Coherent|Flags_Garlic_Bus))!=0;}
}
void SystemPageAllocatorWeb::OnInitialize(){SetPageSize(Page);}
VirtualRange SystemPageAllocatorWeb::OnReserveRange(u64 bytes,u32,u32 flags){return OnReserveAlignedRange(bytes,Page,flags,Page);}
VirtualRange SystemPageAllocatorWeb::OnReserveAlignedRange(u64 bytes,u32,u32 flags,u32 alignment){
    const unsigned request=reservationSequence.fetch_add(1,std::memory_order_relaxed);
    if(request<128)emscripten_log(EM_LOG_CONSOLE,"CP2077 reserve #%u: bytes=%llu alignment=%u flags=%u heap=%llu max=%llu",request,(unsigned long long)bytes,alignment,flags,(unsigned long long)emscripten_get_heap_size(),(unsigned long long)emscripten_get_heap_max());
    u64 count;if(!rounded(bytes,count) || alignment<Page || (alignment&(alignment-1)) || gpuFlags(flags))return NullVirtualRange();
    auto* record=static_cast<Range*>(emscripten_builtin_calloc(1,sizeof(Range)));
    if(!record)return NullVirtualRange();
    record->committed=static_cast<SystemAllocatorWeb**>(emscripten_builtin_calloc(count/Page,sizeof(SystemAllocatorWeb*)));
    void* backing=record->committed?emscripten_builtin_memalign(alignment,size_t(count)):nullptr;
    if(!backing){emscripten_log(EM_LOG_ERROR,"CP2077 reserve #%u failed: bytes=%llu",request,(unsigned long long)count);emscripten_builtin_free(record->committed);emscripten_builtin_free(record);return NullVirtualRange();}
    record->owner=this;record->begin=reinterpret_cast<u64>(backing);record->bytes=count;record->sequence=request;
    {Lock lock;record->next=ranges;ranges=record;}
    if(request<128)emscripten_log(EM_LOG_CONSOLE,"CP2077 reserve #%u backed: address=%llu bytes=%llu heap=%llu",request,(unsigned long long)record->begin,(unsigned long long)count,(unsigned long long)emscripten_get_heap_size());
    return {record->begin,record->begin+count};
}
void SystemPageAllocatorWeb::OnReleaseRange(const VirtualRange& range){
    Lock lock;Range** link=&ranges;
    while(*link && (*link)->begin!=range.start)link=&(*link)->next;
    if(!*link || (*link)->owner!=this || (*link)->begin+(*link)->bytes!=range.end)invalidRange();
    Range* r=*link;
    for(u64 i=0;i<r->bytes/Page;++i)if(r->committed[i])invalidRange();
    *link=r->next;emscripten_builtin_free(reinterpret_cast<void*>(r->begin));
    emscripten_builtin_free(r->committed);emscripten_builtin_free(r);
}
void SystemAllocatorWeb::OnInitialize(){if(!m_pageAllocator || m_pageAllocator->GetPageSize()!=Page)invalidRange();}
SystemBlock SystemAllocatorWeb::OnCommit(const SystemBlock& block,u32 flags){
    u64 bytes;if(!rounded(block.size,bytes) || block.address%Page || gpuFlags(flags))return NullSystemBlock();
    Lock lock;Range* r=find(block.address,bytes);if(!r)return NullSystemBlock();
    u64 first=(block.address-r->begin)/Page,count=bytes/Page;
    for(u64 i=first;i<first+count;++i)if(r->committed[i])return NullSystemBlock();
    // Wasm has already backed reserved linear memory. Zero newly committed
    // pages and track actual ownership rather than pretending mprotect works.
    std::memset(reinterpret_cast<void*>(block.address),0,size_t(bytes));
    for(u64 i=first;i<first+count;++i)r->committed[i]=this;
    r->committedBytes+=bytes;
    r->peakCommittedBytes=std::max(r->peakCommittedBytes,r->committedBytes);
    return {block.address,bytes};
}
SystemBlock SystemAllocatorWeb::OnCommitAligned(const SystemBlock& b,u32 flags,u32 alignment){
    if(!alignment || (alignment&(alignment-1)) || b.address%alignment)return NullSystemBlock();
    return OnCommit(b,flags);
}
void SystemAllocatorWeb::OnDecommit(const SystemBlock& b){
    if(!b.size || b.address%Page || b.size%Page)invalidRange();
    Lock lock;Range* r=find(b.address,b.size);if(!r)invalidRange();
    u64 first=(b.address-r->begin)/Page,count=b.size/Page;
    for(u64 i=first;i<first+count;++i)if(r->committed[i]!=this)invalidRange();
    std::memset(reinterpret_cast<void*>(b.address),0,size_t(b.size));
    for(u64 i=first;i<first+count;++i)r->committed[i]=nullptr;
    if(r->committedBytes<b.size)invalidRange();
    r->committedBytes-=b.size;
}
void SystemAllocatorWeb::OnPartialDecommit(const SystemBlock& whole,const SystemBlock& part){
    if(part.address<whole.address || part.size>whole.size || part.address-whole.address>whole.size-part.size)invalidRange();
    OnDecommit(part);
}
u64 SystemAllocatorWeb::OnReleaseVirtualRange(const VirtualRange& range){
    Lock lock;Range* r=find(range.start,range.end-range.start);
    if(!r || r->begin!=range.start || r->begin+r->bytes!=range.end)invalidRange();
    u64 total=0;
    for(u64 i=0;i<r->bytes/Page;++i){if(r->committed[i] && r->committed[i]!=this)invalidRange();if(r->committed[i]){total+=Page;r->committed[i]=nullptr;}}
    if(r->committedBytes<total)invalidRange();
    r->committedBytes-=total;
    return total;
}
extern "C" void CpWebReportAllocatorMemory() {
    struct Snapshot {unsigned sequence;u64 reserved,committed,peak;};
    Snapshot entries[64];unsigned count=0,totalRanges=0;u64 reserved=0,committed=0;
    {
        Lock lock;
        for(Range* r=ranges;r;r=r->next){
            ++totalRanges;reserved+=r->bytes;committed+=r->committedBytes;
            if(count<64)entries[count++]={r->sequence,r->bytes,r->committedBytes,r->peakCommittedBytes};
        }
    }
    std::fprintf(stderr,"CP2077 allocator memory probe: ranges=%u reservedBytes=%llu committedBytes=%llu heapCapacityBytes=%llu\n",totalRanges,(unsigned long long)reserved,(unsigned long long)committed,(unsigned long long)emscripten_get_heap_size());
    for(unsigned i=0;i<count;++i){const auto& e=entries[i];
        std::fprintf(stderr,"CP2077 allocator range probe: id=%u reservedBytes=%llu committedBytes=%llu peakCommittedBytes=%llu\n",e.sequence,(unsigned long long)e.reserved,(unsigned long long)e.committed,(unsigned long long)e.peak);
    }
}
u64 SystemAllocatorWeb::OnGetTotalPhysicalMemoryAvailable()const{return emscripten_get_heap_max();}
u64 SystemAllocatorWeb::OnGetCurrentPageMemoryAvailable()const{
    // Conservative growth headroom; does not claim the browser's physical RAM.
    u64 max=emscripten_get_heap_max(),used=emscripten_get_heap_size();return max>used?max-used:0;
}
u64 SystemAllocatorWeb::OnGetPageSize()const{return Page;}
void SystemAllocatorWeb::OnWriteReportToLog()const{emscripten_log(EM_LOG_CONSOLE,"Browser linear heap: %llu / %llu bytes",(unsigned long long)emscripten_get_heap_size(),(unsigned long long)emscripten_get_heap_max());}
void SystemAllocatorWeb::OnWriteReportToJson(FILE* f)const{std::fprintf(f,",\"wasm_heap_bytes\":%llu,\"wasm_heap_max\":%llu",(unsigned long long)emscripten_get_heap_size(),(unsigned long long)emscripten_get_heap_max());}
void CallstackCollectorWeb::GetCallstack(u64* frames,u32& depth,u32& size,u16& mask,u32& hash){
    char stack[16384]{};emscripten_get_callstack(EM_LOG_JS_STACK,stack,sizeof(stack));
    depth=0;size=0;mask=0;hash=2166136261u;
    // Store actual Wasm code offsets from browser stack entries. These are
    // module-relative PCs, not native addresses. Some browsers omit offsets;
    // such frames remain unavailable rather than receiving invented addresses.
    for(char* p=stack;depth<c_callstackMaxDepth && (p=std::strstr(p,"wasm-function["));){
        char* end=std::strchr(p,']');if(!end)break;
        p=end+1;if(p[0]!=':' || p[1]!='0' || p[2]!='x')continue;
        char* tail=nullptr;u64 pc=std::strtoull(p+1,&tail,16);if(tail==p+1)continue;p=tail;
        frames[depth]=pc;mask|=u16(1u<<depth);++depth;size+=sizeof(u64);
        for(unsigned b=0;b<8;++b){hash^=u32((pc>>(b*8))&255);hash*=16777619u;}
    }
    if(!depth)hash=0;
}
}}
