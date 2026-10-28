// Independent original TLSF instances back only real committed segments.
// DynamicTLSFAllocator's native class size and callers' locking remain intact.
namespace {
static_assert(sizeof(DynamicTLSFAllocator)==128,"Native dynamic TLSF ABI changed");
constexpr u64 CpTlsfStateTag=1ull<<63;
struct CpTlsfSegment {
    CpTlsfSegment* next=nullptr;
    VirtualRange range=NullVirtualRange();
    TLSFAllocator allocator;
};
struct CpTlsfState {
    DynamicTLSFAllocatorParameter parameter{};
    CpTlsfSegment* segments=nullptr;
    u64 backed=0;
};
CpTlsfState* CpTlsfStateOf(u64 value) {
    return value&CpTlsfStateTag ? reinterpret_cast<CpTlsfState*>(value&~CpTlsfStateTag) : nullptr;
}
[[noreturn]] void CpTlsfInvalid() {
    emscripten_log(EM_LOG_ERROR,"Invalid browser segmented TLSF operation");std::abort();
}
bool CpTlsfFrontendProfile(const DynamicTLSFAllocatorParameter& p) {
    // Only the four audited frontend pools opt in. Backend cache-bank arrays
    // need a contiguous GetAllocatedPages block and keep the original path.
    return (p.virtualRangeSize==RED_GIGA_BYTE(4) && p.defaultSize==RED_MEGA_BYTE(704) && p.chunkSize==RED_MEGA_BYTE(2)) ||
        (p.virtualRangeSize==RED_GIGA_BYTE(1) &&
         ((p.defaultSize==RED_MEGA_BYTE(16) && p.chunkSize==RED_MEGA_BYTE(2)) ||
          (p.defaultSize==RED_MEGA_BYTE(1) && p.chunkSize==RED_MEGA_BYTE(1))));
}
CpTlsfSegment* CpTlsfAdd(CpTlsfState& state,u64 minimum) {
    const u64 quantum=std::max<u64>(state.parameter.chunkSize,state.parameter.systemAllocator->GetPageSize());
    if(!minimum || minimum>std::numeric_limits<u32>::max() || !quantum || (quantum&(quantum-1)))return nullptr;
    const u64 bytes=(minimum+quantum-1)&~(quantum-1);
    if(bytes>std::numeric_limits<u32>::max() || state.backed>state.parameter.virtualRangeSize || bytes>state.parameter.virtualRangeSize-state.backed)return nullptr;
    void* storage=emscripten_builtin_memalign(64,sizeof(CpTlsfSegment));if(!storage)return nullptr;
    auto* segment=new(storage) CpTlsfSegment;
    auto* system=state.parameter.systemAllocator;
    segment->range=system->ReserveAlignedVirtualRange(bytes,state.parameter.flags,state.parameter.virtualRangeAlignment);
    if(segment->range==NullVirtualRange()){segment->~CpTlsfSegment();emscripten_builtin_free(segment);return nullptr;}
    const SystemBlock block=system->CommitAligned({segment->range.start,bytes},state.parameter.flags,state.parameter.virtualRangeAlignment);
    if(block==NullSystemBlock()){
        system->ReleaseVirtualRange(segment->range);segment->~CpTlsfSegment();emscripten_builtin_free(segment);return nullptr;
    }
    segment->allocator.Initialize({segment->range,block});
    segment->next=state.segments;state.segments=segment;state.backed+=bytes;
    return segment;
}
CpTlsfSegment* CpTlsfOwner(CpTlsfState& state,u64 address) {
    for(auto* s=state.segments;s;s=s->next)
        if(address>=s->range.start && address<s->range.end)return s;
    return nullptr;
}
bool CpTlsfRequestValid(u32 size,u32 alignment) {
    return alignment && !(alignment&(alignment-1)) && u64(size)+alignment+65536<std::numeric_limits<u32>::max();
}
Block CpTlsfAllocate(CpTlsfState& state,u32 size,u32 alignment) {
    if(!CpTlsfRequestValid(size,alignment))return NullBlock();
    for(auto* s=state.segments;s;s=s->next){
        const Block block=s->allocator.AllocateAligned(size,alignment);if(block.address)return block;
    }
    if(!state.segments)return NullBlock();
    const u64 area=state.segments->allocator.ComputeMinimumAreaSize(size,alignment);
    if(area<size || area>std::numeric_limits<u32>::max()-65536)return NullBlock();
    const u64 minimum=area+ComputeTLSFBookKeepingSize(static_cast<u32>(area))+65536;
    auto* segment=CpTlsfAdd(state,minimum);
    return segment ? segment->allocator.AllocateAligned(size,alignment) : NullBlock();
}
Block CpTlsfReallocate(CpTlsfState& state,Block& block,u32 size,u32 alignment) {
    if(!block.address)return size ? CpTlsfAllocate(state,size,alignment) : NullBlock();
    auto* owner=CpTlsfOwner(state,block.address);if(!owner)CpTlsfInvalid();
    if(!size){owner->allocator.Free(block);return NullBlock();}
    if(!CpTlsfRequestValid(size,alignment))return NullBlock();
    const Block result=block.address%alignment ? NullBlock() : owner->allocator.ReallocateAligned(block,size,alignment);
    if(result.address)return result;
    const Block moved=CpTlsfAllocate(state,size,alignment);
    if(moved.address){
        const u64 oldSize=GetTLSFBlockSize(block.address);
        std::memcpy(reinterpret_cast<void*>(moved.address),reinterpret_cast<const void*>(block.address),static_cast<size_t>(std::min<u64>(oldSize,size)));
        owner->allocator.Free(block);
    }
    return moved;
}
void CpTlsfDrop(CpTlsfState* state) {
    for(auto* s=state->segments;s;){auto* next=s->next;
        s->allocator.Unitialize();state->parameter.systemAllocator->ReleaseVirtualRange(s->range);
        s->~CpTlsfSegment();emscripten_builtin_free(s);s=next;
    }
    state->~CpTlsfState();emscripten_builtin_free(state);
}
void CpTlsfMetrics(CpTlsfState& state,TLSFAllocatorMetrics& out) {
    std::memset(&out,0,sizeof(out));out.metrics.bookKeepingBytes=sizeof(state);
    for(auto* s=state.segments;s;s=s->next){
        TLSFAllocatorMetrics part{};part.metrics.consumedSystemMemoryBytes=s->range.end-s->range.start;
        s->allocator.BuildMetrics(part);
        out.metrics.consumedSystemMemoryBytes+=part.metrics.consumedSystemMemoryBytes;
        out.metrics.consumedMemoryBytes+=part.metrics.consumedMemoryBytes;
        out.metrics.bookKeepingBytes+=part.metrics.bookKeepingBytes+sizeof(*s);
        out.metrics.largestBlockSize=std::max(out.metrics.largestBlockSize,part.metrics.largestBlockSize);
        if(part.metrics.smallestBlockSize && part.metrics.smallestBlockSize!=~0ull)
            out.metrics.smallestBlockSize=out.metrics.smallestBlockSize ? std::min(out.metrics.smallestBlockSize,part.metrics.smallestBlockSize) : part.metrics.smallestBlockSize;
        for(unsigned i=0;i<32;++i){
            auto& a=out.freeBlocksMetrics[i];const auto& b=part.freeBlocksMetrics[i];
            if(b.blockSize)a.blockSize=b.blockSize;a.totalCount+=b.totalCount;a.totalSize+=b.totalSize;
        }
    }
}
void CpTlsfChecks(SystemAllocator* system) {
    DynamicTLSFAllocator test;
    test.Initialize({system,RED_GIGA_BYTE(1),RED_MEGA_BYTE(1),RED_MEGA_BYTE(1),Flags_CPU_Read_Write|Flags_Skip_System_OOM,0});
    const Block firstPages=test.GetAllocatedPages();
    if(!firstPages.address || firstPages.size!=RED_MEGA_BYTE(1))CpTlsfInvalid();
    Block small=test.AllocateAligned(128,256);
    Block large=test.AllocateAligned(RED_MEGA_BYTE(2),4096);
    if(!small.address || small.address%256 || !large.address || large.address%4096 || !test.OwnBlock(small.address) || !test.OwnBlock(large.address) || test.OwnBlock(reinterpret_cast<u64>(&test)))CpTlsfInvalid();
    std::memset(reinterpret_cast<void*>(small.address),0xa5,128);
    std::memset(reinterpret_cast<void*>(large.address),0x5a,RED_MEGA_BYTE(2));
    Block moved=test.ReallocateAligned(small,RED_MEGA_BYTE(4),4096);
    if(!moved.address || moved.address%4096 || test.GetBlockSize(moved.address)<RED_MEGA_BYTE(4))CpTlsfInvalid();
    for(unsigned i=0;i<128;++i)if(reinterpret_cast<unsigned char*>(moved.address)[i]!=0xa5)CpTlsfInvalid();
    for(unsigned i=0;i<RED_MEGA_BYTE(2);++i)if(reinterpret_cast<unsigned char*>(large.address)[i]!=0x5a)CpTlsfInvalid();
    const auto oldAddress=moved.address;
    if(test.ReallocateAligned(moved,std::numeric_limits<u32>::max(),16).address || moved.address!=oldAddress)CpTlsfInvalid();
    for(unsigned i=0;i<128;++i)if(reinterpret_cast<unsigned char*>(moved.address)[i]!=0xa5)CpTlsfInvalid();
    if(test.AllocateAligned(RED_GIGA_BYTE(1)-65536,16).address || test.AllocateAligned(32,3).address)CpTlsfInvalid();
    TLSFAllocatorMetrics metrics{};test.BuildMetrics(metrics);
    if(metrics.metrics.consumedSystemMemoryBytes!=test.GetSystemMemoryConsumed() || metrics.metrics.consumedSystemMemoryBytes>=RED_MEGA_BYTE(16) || metrics.metrics.consumedMemoryBytes>metrics.metrics.consumedSystemMemoryBytes)CpTlsfInvalid();
    test.Free(large);test.ReallocateAligned(moved,0,16);test.Uninitialize();
    if(test.GetSystemMemoryConsumed()!=0)CpTlsfInvalid();
    std::fprintf(stderr,"CP2077 segmented TLSF checks passed: real segment growth, alignment, ownership, payload-preserving realloc, failed realloc preservation, budget exhaustion, metrics and cleanup\n");
}
}
