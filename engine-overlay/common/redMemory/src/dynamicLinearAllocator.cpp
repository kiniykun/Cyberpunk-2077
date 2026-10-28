// Browser implementation of the native 128-byte DynamicLinearAllocator ABI.
// Individual blocks remain contiguous and stable; unused 4-GiB VA is not backed.
#include "build.h"
#include "dynamicLinearAllocator.h"
#include "systemAllocator.h"
#include "utils.h"
#include "flags.h"
#include <emscripten/heap.h>
#include <emscripten/emscripten.h>
#include <pthread.h>
#include <new>
#include <cstring>
#include <cstdlib>
#include <limits>
namespace red { namespace memory {
namespace {
constexpr u64 Limit=4ull*1024*1024*1024;
struct Segment {
    Segment* next=nullptr;
    VirtualRange range=NullVirtualRange();
    LinearAllocator linear;
};
struct State {
    State* next=nullptr;
    const DynamicLinearAllocator* owner=nullptr;
    SystemAllocator* system=nullptr;
    Segment* segments=nullptr;
    Segment* current=nullptr;
    u64 backed=0;
    u32 chunk=0,flags=0;
    pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
};
State* states=nullptr;
#ifdef CPWEB_ALLOCATOR_DIAGNOSTICS
void TestDynamic(SystemAllocator* system);
#endif
pthread_mutex_t registryMutex=PTHREAD_MUTEX_INITIALIZER;
struct Lock {pthread_mutex_t* mutex;explicit Lock(pthread_mutex_t& m):mutex(&m){pthread_mutex_lock(mutex);}~Lock(){pthread_mutex_unlock(mutex);}};
[[noreturn]] void Invalid(){emscripten_log(EM_LOG_ERROR,"Invalid browser dynamic-linear allocator operation");std::abort();}
bool Alignment(u32 value){return value && !(value&(value-1));}
State* Find(const DynamicLinearAllocator* owner){Lock lock(registryMutex);for(State* s=states;s;s=s->next)if(s->owner==owner)return s;return nullptr;}
Segment* Owned(State& state,u64 address){for(Segment* s=state.segments;s;s=s->next)if(s->linear.OwnBlock(address))return s;return nullptr;}
void Drop(State& state,Segment* segment){
    state.system->ReleaseVirtualRange(segment->range);
    state.backed-=segment->range.end-segment->range.start;
    segment->~Segment();emscripten_builtin_free(segment);
}
Segment* Add(State& state,u64 requested){
    const u64 quantum=std::max<u64>(state.chunk,state.system->GetPageSize());
    if(!quantum || (quantum&(quantum-1)) || !requested || requested>Limit || requested>Limit-(quantum-1))return nullptr;
    const u64 size=(requested+quantum-1)&~(quantum-1);
    if(size>Limit-state.backed)return nullptr;
    void* metadata=emscripten_builtin_memalign(64,sizeof(Segment));if(!metadata)return nullptr;
    Segment* segment=new(metadata) Segment;
    segment->range=state.system->ReserveVirtualRange(size,state.flags);
    if(segment->range==NullVirtualRange()){segment->~Segment();emscripten_builtin_free(segment);return nullptr;}
    const SystemBlock block=state.system->Commit({segment->range.start,size},state.flags);
    if(!block.address){state.system->ReleaseVirtualRange(segment->range);segment->~Segment();emscripten_builtin_free(segment);return nullptr;}
    segment->linear.Initialize({block});segment->next=state.segments;state.segments=segment;state.current=segment;state.backed+=segment->range.end-segment->range.start;
    return segment;
}
Block AllocateFromState(State& state,u32 size,u32 alignment){
    if(!Alignment(alignment))return NullBlock();
    alignment=std::max(alignment,4u);
    const u64 rounded=(u64(size)+alignment-1)&~u64(alignment-1);
    if(rounded>std::numeric_limits<u32>::max())return NullBlock();
    if(state.current){Block block=state.current->linear.AllocateAligned(size,alignment);if(block.address)return block;}
    for(Segment* s=state.segments;s;s=s->next){if(s==state.current)continue;Block block=s->linear.AllocateAligned(size,alignment);if(block.address)return block;}
    Segment* segment=Add(state,rounded+alignment+sizeof(u32));
    return segment?segment->linear.AllocateAligned(size,alignment):NullBlock();
}
void Metrics(State& state,LinearAllocatorMetrics& result){
    std::memset(&result,0,sizeof(result));
    result.metrics.bookKeepingBytes=sizeof(State);
    for(Segment* s=state.segments;s;s=s->next){
        LinearAllocatorMetrics part;s->linear.BuildMetrics(part);
        result.metrics.consumedSystemMemoryBytes+=part.metrics.consumedSystemMemoryBytes;
        result.metrics.consumedMemoryBytes+=part.metrics.consumedMemoryBytes;
        result.metrics.bookKeepingBytes+=sizeof(Segment)+part.metrics.bookKeepingBytes;
        result.metrics.largestBlockSize=std::max(result.metrics.largestBlockSize,part.metrics.largestBlockSize);
        if(part.metrics.smallestBlockSize)result.metrics.smallestBlockSize=4;
    }
    // Report actual free contiguous backing and remaining logical growth budget.
    result.metrics.largestBlockSize=std::max(result.metrics.largestBlockSize,Limit-state.backed);
}
}
DynamicLinearAllocator::DynamicLinearAllocator():m_virtualRange(NullVirtualRange()),m_nextVirtualAddress(0),m_minimalAllocationSize(0),m_systemAllocator(nullptr),m_chunkSize(0),m_flags(Flags_CPU_Read_Write){}
DynamicLinearAllocator::~DynamicLinearAllocator(){if(IsInitialized())Uninitialize();}
void DynamicLinearAllocator::Initialize(const DynamicLinearAllocatorParameter& parameter){
    if(Find(this)||!parameter.systemAllocator||!Alignment(parameter.chunkSize))Invalid();
    void* metadata=emscripten_builtin_memalign(64,sizeof(State));if(!metadata)Invalid();
    State* state=new(metadata) State;state->owner=this;state->system=parameter.systemAllocator;state->chunk=parameter.chunkSize;state->flags=parameter.flags;
    if(!Add(*state,parameter.chunkSize)){state->~State();emscripten_builtin_free(state);Invalid();}
    m_systemAllocator=parameter.systemAllocator;m_chunkSize=parameter.chunkSize;m_flags=parameter.flags;m_virtualRange=state->current->range;m_nextVirtualAddress=m_virtualRange.end;
    {Lock lock(registryMutex);state->next=states;states=state;}
#ifdef CPWEB_ALLOCATOR_DIAGNOSTICS
    static unsigned checked=0;
    if(!__atomic_exchange_n(&checked,1,__ATOMIC_ACQ_REL))TestDynamic(state->system);
#endif
}
void DynamicLinearAllocator::Uninitialize(){
    State* state;
    {Lock lock(registryMutex);State** at=&states;while(*at&&(*at)->owner!=this)at=&(*at)->next;if(!*at)Invalid();state=*at;*at=state->next;}
    pthread_mutex_lock(&state->mutex);
    while(state->segments){Segment* segment=state->segments;state->segments=segment->next;Drop(*state,segment);}
    pthread_mutex_unlock(&state->mutex);pthread_mutex_destroy(&state->mutex);state->~State();emscripten_builtin_free(state);
    m_virtualRange=NullVirtualRange();m_nextVirtualAddress=0;m_minimalAllocationSize=0;
}
Block DynamicLinearAllocator::Allocate(u32 size){return AllocateAligned(size,DefaultAlignmentType::value);}
Block DynamicLinearAllocator::AllocateAligned(u32 size,u32 alignment){State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);return AllocateFromState(*state,size,alignment);}
void DynamicLinearAllocator::Free(Block& block){State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);Segment* segment=Owned(*state,block.address);if(!segment)Invalid();segment->linear.Free(block);}
Block DynamicLinearAllocator::Reallocate(Block& block,u32 size){return ReallocateAligned(block,size,DefaultAlignmentType::value);}
Block DynamicLinearAllocator::ReallocateAligned(Block& block,u32 size,u32 alignment){
    State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);
    if(!Alignment(alignment))return NullBlock();
    alignment=std::max(alignment,4u);
    if(!block.address)return size?AllocateFromState(*state,size,alignment):NullBlock();
    Segment* segment=Owned(*state,block.address);if(!segment)Invalid();
    if(!size){segment->linear.Free(block);return NullBlock();}
    if((u64(size)+alignment-1)>std::numeric_limits<u32>::max())return NullBlock();
    Block resized=segment->linear.ReallocateAligned(block,size,alignment);if(resized.address)return resized;
    resized=AllocateFromState(*state,size,alignment);
    if(resized.address){std::memcpy(reinterpret_cast<void*>(resized.address),reinterpret_cast<void*>(block.address),size_t(block.size));segment->linear.Free(block);}
    return resized;
}
Bool DynamicLinearAllocator::OwnBlock(u64 address)const{State* state=Find(this);if(!state)return false;Lock lock(state->mutex);return Owned(*state,address)!=nullptr;}
u64 DynamicLinearAllocator::GetBlockSize(u64 address)const{State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);Segment* segment=Owned(*state,address);if(!segment)Invalid();return segment->linear.GetBlockSize(address);}
u32 DynamicLinearAllocator::GetChunkSize()const{return m_chunkSize;}
void DynamicLinearAllocator::Reset(){
    State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);
    // Reset invalidates all previous allocations; release every old owned extent.
    while(state->segments){Segment* segment=state->segments;state->segments=segment->next;Drop(*state,segment);}
    state->current=nullptr;
    if(!Add(*state,state->chunk))Invalid();
    m_virtualRange=state->current->range;m_nextVirtualAddress=m_virtualRange.end;
}
void DynamicLinearAllocator::BuildMetrics(LinearAllocatorMetrics& metrics){State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);Metrics(*state,metrics);}
void DynamicLinearAllocator::SerializeMetrics(Serializer& serializer){SerializeAllocatorIdentifiers(this,serializer);LinearAllocatorMetrics metrics;BuildMetrics(metrics);serializer.Serialize(static_cast<u32>(sizeof(metrics)));serializer.Serialize(&metrics,sizeof(metrics));}
bool DynamicLinearAllocator::IsInitialized()const{return Find(this)!=nullptr;}
SystemBlock DynamicLinearAllocator::AllocateBlock(u32 size){State* state=Find(this);if(!state)return NullSystemBlock();Lock lock(state->mutex);Segment* segment=Add(*state,size);return segment?SystemBlock{segment->range.start,segment->range.end-segment->range.start}:NullSystemBlock();}
bool DynamicLinearAllocator::CanCreateMoreBlock()const{State* state=Find(this);if(!state)return false;Lock lock(state->mutex);return state->backed<Limit;}
void DynamicLinearAllocator::Reserve(u32 size){
    State* state=Find(this);if(!state)Invalid();Lock lock(state->mutex);if(!size)return;
    // Preserve actual additional backing requested by Reserve, not a fake acknowledgement.
    if(!Add(*state,size))emscripten_log(EM_LOG_ERROR,"Browser dynamic-linear Reserve failed: %u bytes",size);
}
#ifdef CPWEB_ALLOCATOR_DIAGNOSTICS
namespace {
void TestDynamic(SystemAllocator* system){
    DynamicLinearAllocator test;
    test.Initialize({system,65536,Flags_CPU_Read_Write|Flags_Skip_System_OOM});
    Block first=test.AllocateAligned(128,64);
    if(!first.address||first.address%64||!test.OwnBlock(first.address)||test.OwnBlock(0))Invalid();
    std::memset(reinterpret_cast<void*>(first.address),0xa5,128);
    Block large=test.AllocateAligned(66000,16);
    if(!large.address||large.address%16)Invalid();
    std::memset(reinterpret_cast<void*>(large.address),0x5a,66000);
    Block grown=test.ReallocateAligned(first,131072,64);
    if(!grown.address||grown.address%64||test.GetBlockSize(grown.address)<131072)Invalid();
    if(test.ReallocateAligned(first,std::numeric_limits<u32>::max(),1).address)Invalid();
    for(unsigned i=0;i<128;++i)if(reinterpret_cast<const unsigned char*>(grown.address)[i]!=0xa5)Invalid();
    for(unsigned i=0;i<66000;++i)if(reinterpret_cast<const unsigned char*>(large.address)[i]!=0x5a)Invalid();
    test.Free(large);
    if(!test.OwnBlock(large.address)||large.size<66000)Invalid();
    test.Reserve(131072);
    LinearAllocatorMetrics metrics;test.BuildMetrics(metrics);
    if(metrics.metrics.consumedSystemMemoryBytes<metrics.metrics.consumedMemoryBytes||metrics.metrics.consumedSystemMemoryBytes>=Limit)Invalid();
    test.Reset();test.BuildMetrics(metrics);
    if(metrics.metrics.consumedMemoryBytes!=0)Invalid();
    Block after=test.AllocateAligned(4096,256);
    if(!after.address||after.address%256||!test.OwnBlock(after.address))Invalid();
    test.Uninitialize();
    if(test.OwnBlock(after.address))Invalid();
    DynamicLinearAllocator smallChunks;
    smallChunks.Initialize({system,4096,Flags_CPU_Read_Write|Flags_Skip_System_OOM});
    Block small=smallChunks.AllocateAligned(8192,16);
    if(!small.address||small.address%16)Invalid();
    smallChunks.Reset();smallChunks.Uninitialize();
    emscripten_log(EM_LOG_CONSOLE,"CP2077 dynamic-linear checks passed: chunk growth, payload-preserving realloc, alignment, ownership, free, reset, metrics and cleanup");
}
}
#endif
}}
