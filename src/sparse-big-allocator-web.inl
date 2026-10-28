// Browser big allocations reserve only their own actual backing. Native ABI,
// page granularity, locking, payloads and genuine allocation failures remain.
namespace {
constexpr u64 CpBigTag=1ull<<63;
struct CpBigAllocation {CpBigAllocation* next=nullptr;VirtualRange range=NullVirtualRange();u64 committed=0;};
struct CpBigState {
    SystemAllocator* system=nullptr;u64 limit=0,used=0;u32 flags=0,page=0,count=0;
    bool full=false;CpBigAllocation* allocations=nullptr;
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
};
struct CpBigLock {pthread_mutex_t* mutex;explicit CpBigLock(CpBigState& s):mutex(&s.mutex){pthread_mutex_lock(mutex);}~CpBigLock(){pthread_mutex_unlock(mutex);}};
CpBigState* CpBigStateOf(u64 value){return value&CpBigTag ? reinterpret_cast<CpBigState*>(value&~CpBigTag) : nullptr;}
[[noreturn]] void CpBigInvalid(){emscripten_log(EM_LOG_ERROR,"Invalid browser sparse big allocator operation");std::abort();}
bool CpBigRounded(CpBigState& s,u32 size,u32 alignment,u64& bytes){
    if(!size || !alignment || (alignment&(alignment-1)))return false;
    const u64 quantum=std::max<u64>(s.page,alignment);
    bytes=(u64(size)+quantum-1)&~(quantum-1);
    return bytes && bytes<=std::numeric_limits<u32>::max();
}
CpBigAllocation* CpBigExact(CpBigState& s,u64 address){
    for(auto* n=s.allocations;n;n=n->next)if(n->range.start==address)return n;
    return nullptr;
}
Block CpBigAllocateLocked(CpBigState& s,u32 size,u32 alignment,u64 credit=0){
    u64 bytes=0;if(s.full || !CpBigRounded(s,size,alignment,bytes) || credit>s.used || s.used-credit>s.limit || bytes>s.limit-(s.used-credit))return NullBlock();
    void* storage=emscripten_builtin_malloc(sizeof(CpBigAllocation));if(!storage)return NullBlock();
    auto* node=new(storage) CpBigAllocation;
    node->range=s.system->ReserveAlignedVirtualRange(bytes,s.flags,std::max(alignment,s.page));
    if(node->range==NullVirtualRange()){node->~CpBigAllocation();emscripten_builtin_free(node);return NullBlock();}
    const SystemBlock block=s.system->CommitAligned({node->range.start,bytes},s.flags,std::max(alignment,s.page));
    if(block==NullSystemBlock()){s.system->ReleaseVirtualRange(node->range);node->~CpBigAllocation();emscripten_builtin_free(node);return NullBlock();}
    node->committed=bytes;node->next=s.allocations;s.allocations=node;s.used+=bytes;++s.count;
    return {block.address,block.size};
}
void CpBigFreeLocked(CpBigState& s,Block& block){
    if(!block.address)return;
    CpBigAllocation** link=&s.allocations;while(*link && (*link)->range.start!=block.address)link=&(*link)->next;
    if(!*link)CpBigInvalid();auto* node=*link;block.size=node->committed;
    s.system->ReleaseVirtualRange(node->range);*link=node->next;
    s.used-=node->committed;--s.count;node->~CpBigAllocation();emscripten_builtin_free(node);
}
Block CpBigAllocate(CpBigState& s,u32 size,u32 alignment){CpBigLock lock(s);return CpBigAllocateLocked(s,size,alignment);}
void CpBigFree(CpBigState& s,Block& block){CpBigLock lock(s);CpBigFreeLocked(s,block);}
Block CpBigReallocate(CpBigState& s,Block& block,u32 size,u32 alignment){
    CpBigLock lock(s);
    if(!block.address)return CpBigAllocateLocked(s,size,alignment);
    auto* node=CpBigExact(s,block.address);if(!node)CpBigInvalid();
    block.size=node->committed;
    if(!size){CpBigFreeLocked(s,block);return NullBlock();}
    u64 bytes=0;if(!CpBigRounded(s,size,alignment,bytes))return NullBlock();
    if(block.address%alignment==0 && bytes<=node->committed){
        if(bytes<node->committed){
            const u64 released=node->committed-bytes;
            s.system->PartialDecommit({node->range.start,node->committed},{node->range.start+bytes,released});
            node->committed=bytes;s.used-=released;
        }
        return {node->range.start,node->committed};
    }
    if(block.address%alignment==0 && bytes<=node->range.end-node->range.start && s.used-node->committed<=s.limit && bytes<=s.limit-(s.used-node->committed)){
        const u64 extra=bytes-node->committed;
        const auto committed=s.system->Commit({node->range.start+node->committed,extra},s.flags);
        if(committed!=NullSystemBlock()){node->committed=bytes;s.used+=extra;return {node->range.start,bytes};}
    }
    // Credit the old block when checking the logical budget, while retaining
    // it until the new allocation succeeds and the payload is copied.
    const Block replacement=CpBigAllocateLocked(s,size,alignment,node->committed);
    if(replacement.address){
        std::memcpy(reinterpret_cast<void*>(replacement.address),reinterpret_cast<const void*>(block.address),static_cast<size_t>(std::min<u64>(block.size,replacement.size)));
        CpBigFreeLocked(s,block);
    }
    return replacement;
}
bool CpBigOwn(CpBigState& s,u64 address){
    CpBigLock lock(s);for(auto* n=s.allocations;n;n=n->next)
        if(address>=n->range.start && address<n->range.start+n->committed)return true;
    return false;
}
u64 CpBigSize(CpBigState& s,u64 address){CpBigLock lock(s);auto* node=CpBigExact(s,address);if(!node)CpBigInvalid();return node->committed;}
void CpBigMetrics(CpBigState& s,BigSizeAllocatorMetrics& out){
    CpBigLock lock(s);std::memset(&out,0,sizeof(out));
    out.metrics.consumedMemoryBytes=s.used;
    out.metrics.bookKeepingBytes=sizeof(s)+u64(s.count)*sizeof(CpBigAllocation);
    out.metrics.consumedSystemMemoryBytes=s.used+out.metrics.bookKeepingBytes;
    out.virtualRangeSize=s.limit;out.allocationListCount=out.allocationListMaxCount=s.count;
    // Unused budget can back a new contiguous allocation on demand. Report
    // that capacity separately from actual backing, not as resident memory.
    out.freeListSize=s.limit-s.used;
    out.freeListCount=out.freeListMaxCount=out.freeListSize ? 1 : 0;
    out.metrics.largestBlockSize=out.freeListSize;out.metrics.smallestBlockSize=out.freeListSize;
}
void CpBigDrop(CpBigState* s){
    for(auto* n=s->allocations;n;){auto* next=n->next;s->system->ReleaseVirtualRange(n->range);n->~CpBigAllocation();emscripten_builtin_free(n);n=next;}
    pthread_mutex_destroy(&s->mutex);s->~CpBigState();emscripten_builtin_free(s);
}
void CpBigChecks(SystemAllocator* system){
    BigSizeAllocator test;test.Initialize({system,RED_GIGA_BYTE(2),Flags_CPU_Read_Write|Flags_Skip_System_OOM});
    Block a=test.AllocateAligned(131071,131072);
    if(!a.address || a.address%131072 || a.size!=131072 || !test.OwnBlock(a.address) || test.OwnBlock(reinterpret_cast<u64>(&test)))CpBigInvalid();
    std::memset(reinterpret_cast<void*>(a.address),0xa5,131071);
    Block b=test.ReallocateAligned(a,1048576,131072);
    if(!b.address || b.address%131072 || test.GetBlockSize(b.address)!=1048576)CpBigInvalid();
    for(unsigned i=0;i<131071;++i)if(reinterpret_cast<unsigned char*>(b.address)[i]!=0xa5)CpBigInvalid();
    Block c=test.ReallocateAligned(b,65536,65536);
    if(!c.address || c.size!=65536)CpBigInvalid();
    Block d=test.ReallocateAligned(c,131072,65536);
    if(d.address!=c.address || d.size!=131072)CpBigInvalid();
    for(unsigned i=0;i<65536;++i)if(reinterpret_cast<unsigned char*>(d.address)[i]!=0xa5)CpBigInvalid();
    if(test.AllocateAligned(std::numeric_limits<u32>::max(),65536).address || test.AllocateAligned(128,3).address)CpBigInvalid();
    test.InternalMarkBigSizeAllocatorAsFull();
    const u64 old=d.address;
    if(test.ReallocateAligned(d,RED_MEGA_BYTE(2),65536).address || d.address!=old)CpBigInvalid();
    for(unsigned i=0;i<65536;++i)if(reinterpret_cast<unsigned char*>(d.address)[i]!=0xa5)CpBigInvalid();
    BigSizeAllocatorMetrics metrics;test.BuildMetrics(metrics);
    if(metrics.metrics.consumedMemoryBytes!=131072 || metrics.virtualRangeSize!=RED_GIGA_BYTE(2))CpBigInvalid();
    test.ReallocateAligned(d,0,65536);test.BuildMetrics(metrics);
    if(metrics.metrics.consumedMemoryBytes || metrics.allocationListCount)CpBigInvalid();
    test.Uninitialize();
    std::fprintf(stderr,"CP2077 sparse big allocator probe: phase=checks-passed alignment=1 reallocPayload=1 shrinkRegrow=1 ownership=1 failurePreservation=1 metrics=1 cleanup=1\n");
}
}
