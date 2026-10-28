#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/bink2/include/bink.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <pthread.h>

static_assert(offsetof(BINK,FrameBuffers)==176,"Bink scalar-prefix ABI changed");
static_assert(sizeof(BINKFRAMEBUFFERS)==264,"Bink plane ABI changed");
static_assert(sizeof(BINKSUMMARY)==144,"Bink summary ABI changed");
namespace {
enum Operation {Open,Close,Register,Frame,Next,Pause,Wait,Skip,Goto,Sound,Summary,Offsets,Loop,FreeGlobals};
struct Request {
    em_proxying_ctx* context=nullptr;
    Operation operation=Open;
    U32 handle=0,flags=0;
    const void* input=nullptr;
    size_t inputBytes=0;
    void* header=nullptr;
    void* buffer=nullptr;
    size_t capacity=0;
    S32 first=0,second=0;
    std::int64_t value=0;
    bool succeeded=false;
    bool async=false; // [skperf] round 66 binkAsync: fire-and-forget request (CpAsyncRequest), freed on completion
};
struct Movie {
    BINK native{};
    BINKFRAMEBUFFERS info{};
    BINKFRAMEBUFFERS* registered=nullptr;
    U32 handle=0;
    std::unique_ptr<U64,decltype(&std::free)> offsets{nullptr,std::free};
    std::mutex mutex;
    std::mutex asyncMutex;
    std::condition_variable asyncFinished;
    bool asyncPending=false;
    bool closing=false;
    // [skperf] round 66 binkAsync: last SDK answers of BinkWait/BinkShouldSkip (-1 = none yet) and refresh-in-flight flags.
    std::atomic<int> cachedWait{-1}, cachedSkip{-1};
    std::atomic<bool> waitInFlight{false}, skipInFlight{false};
    // [skperf] round 67 binkAsync: latest SDK summary (ink VideoPlayersTick asks for it every tick per video).
    std::mutex summaryMutex; BINKSUMMARY cachedSummary{}; bool hasSummary=false; std::atomic<bool> summaryInFlight{false};
};
std::mutex registryMutex;
std::unordered_map<HBINK,std::shared_ptr<Movie>> movies;
struct FileCallbacks {BINKFILEOPEN open=nullptr;BINKFILEREAD read=nullptr;BINKFILESEEK seek=nullptr;BINKFILECLOSE close=nullptr;};
FileCallbacks fileCallbacks;
std::mutex fileMutex;
struct AsyncSlot {
    pthread_t thread{};
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Movie>> jobs;
    bool ready=false,stopping=false;
};
std::array<std::shared_ptr<AsyncSlot>,8> asyncSlots;
std::mutex slotsMutex;
std::atomic<U32> nextAsyncSlot{0};
em_proxying_queue* Queue() {static auto* queue=em_proxying_queue_create();return queue;}
std::shared_ptr<Movie> Find(HBINK movie) {
    std::lock_guard<std::mutex> guard(registryMutex);
    auto found=movies.find(movie);return found==movies.end()?nullptr:found->second;
}
}
extern "C" void StartBinkRequest(std::uintptr_t address,int operation,U32 handle,U32 flags,
    const void* input,size_t length,void* header,void* buffer,size_t capacity,S32 first,S32 second);
namespace {
// [skperf] round 66 binkAsync (opt-in ?binkAsync=1). Every Bink control call was a blocking round trip render worker ->
// browser main thread -> decoder worker -> back, and it also waited for the movie mutex that BinkDoFrame holds for a
// whole (emulated SDK) decode. With the switch on, BinkNextFrame/Pause/Goto/SetWillLoop/SetSoundOnOff are posted without
// waiting (same order: the same main-thread proxy queue, then the worker's serial request chain, ahead of the next
// decode request), and BinkWait/BinkShouldSkip return the SDK's latest answer while a refresh is in flight (the first
// call per movie is synchronous). Movie headers are refreshed by the next decode reply as before. No engine logic changes;
// only how the port talks to its decoder worker.
struct CpAsyncRequest : Request { std::shared_ptr<Movie> movie; unsigned char scratch[176]{}; BINKSUMMARY summary{}; };
bool CpBinkAsyncOn() {
    static const int v=[]{ const int r=MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('binkAsync')==='0'?0:1; } catch(e) { return 1; } }); /* [skperf] round 67: default ON (tc95 hitch gone); ?binkAsync=0 = old */
        std::fprintf(stderr,"CP2077 skperf switches: binkAsync=%d\n",r); return r; }();
    return v!=0;
}
bool PostAsync(const std::shared_ptr<Movie>& movie,Operation operation,S32 first=0,S32 second=0) {
    if(emscripten_is_main_runtime_thread())return false;
    auto* queue=Queue();if(!queue)return false;
    auto* request=new CpAsyncRequest();
    request->async=true;request->operation=operation;request->handle=movie->handle;request->header=request->scratch;
    request->first=first;request->second=second;request->movie=movie;
    if(operation==Summary){request->buffer=&request->summary;request->header=nullptr;} // same as the sync summary request
    const bool posted=emscripten_proxy_async(queue,emscripten_main_runtime_thread_id(),[](void* argument){
        auto& r=*static_cast<CpAsyncRequest*>(argument);
        StartBinkRequest(reinterpret_cast<std::uintptr_t>(static_cast<Request*>(&r)),r.operation,r.handle,r.flags,
            r.input,r.inputBytes,r.header,r.buffer,r.capacity,r.first,r.second);
    },request);
    if(!posted){delete request;return false;}
    return true;
}
}
extern "C" EMSCRIPTEN_KEEPALIVE void CpWebBinkComplete(std::uintptr_t address,int success,std::int64_t value) {
    auto& request=*reinterpret_cast<Request*>(address);
    if(request.async) { // [skperf] round 66 binkAsync
        auto* r=static_cast<CpAsyncRequest*>(&request);
        if(r->operation==Wait){r->movie->cachedWait.store(success?int(value):-1);r->movie->waitInFlight.store(false);}
        else if(r->operation==Skip){r->movie->cachedSkip.store(success?int(value):-1);r->movie->skipInFlight.store(false);}
        else if(r->operation==Summary){
            if(success){std::lock_guard<std::mutex> guard(r->movie->summaryMutex);r->movie->cachedSummary=r->summary;r->movie->hasSummary=true;}
            r->movie->summaryInFlight.store(false);
        }
        else if(!success)r->movie->native.ReadError=1;
        delete r;return;
    }
    request.succeeded=success!=0;request.value=value;emscripten_proxy_finish(request.context);
}
EM_JS(void, StartBinkRequest, (std::uintptr_t address,int operation,U32 handle,U32 flags,
    const void* input,size_t length,void* header,void* buffer,size_t capacity,S32 first,S32 second), {
    let finished=false;
    const finish=(success,value=0n)=> {
        if(finished)return;finished=true;Module['_CpWebBinkComplete'](address,success,value);
    };
    try {
        if(typeof Module['cp2077BinkRequest']!=='function') {finish(0);return;}
        const heap=()=>new Uint8Array(wasmMemory.buffer);
        const span=(pointer,size)=> {
            const start=Number(pointer),bytes=Number(size),memory=heap();
            if(!Number.isSafeInteger(start)||!Number.isSafeInteger(bytes)||start<0||bytes<0||start>memory.length-bytes)
                throw new Error('Bink engine buffer is outside shared memory');
            return memory.subarray(start,start+bytes);
        };
        const copy=(pointer,bytes,size)=> {
            if(!(bytes instanceof ArrayBuffer)||bytes.byteLength!==size)throw new Error('Bink SDK reply size mismatch');
            span(pointer,size).set(new Uint8Array(bytes));
        };
        let packet={handle:Number(handle)};
        if(operation===0)packet={type:'open',bytes:span(input,length),flags:Number(flags),callerFrameBuffers:true};
        else if(operation===1)packet.type='close';
        else if(operation===2) {
            packet.type='registerFrameBuffers';
            const bytes=span(input,264),view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
            const count=view.getInt32(0,true);if(count<1||count>3)throw new Error('Invalid engine Bink frame count');
            packet.pitches=Array.from({length:count},(_,slot)=>Array.from({length:5},(_,plane)=>view.getUint32(24+slot*80+plane*16+12,true)));
        } else if(operation===3)packet.type='frame';
        else if(operation===4)packet.type='next';
        else if(operation===10)packet.type='summary';
        else if(operation===11)packet.type='frameOffsets';
        else if(operation===13)packet.type='freeGlobals';
        else {
            packet.type='control';packet.control=({5:'pause',6:'wait',7:'shouldSkip',8:'goto',9:'sound',12:'loop'})[operation];
            packet.args=operation===6||operation===7?[]:operation===8?[Number(first),Number(second)]:[Number(first)];
            if(!packet.control)throw new Error('Unknown Bink engine operation');
        }
        Module['cp2077BinkRequest'](packet).then(result=> {
            if(result.header&&header)copy(header,result.header,176);
            if(operation===0) {
                const desc=result.frameBuffers;
                if(!Number.isSafeInteger(result.handle)||result.handle<=0||!desc||desc.frames<1||desc.frames>3)
                    throw new Error('Invalid Bink SDK open reply');
                const bytes=span(buffer,264);bytes.fill(0);const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
                [desc.frames,desc.yaWidth,desc.yaHeight,desc.chromaWidth,desc.chromaHeight,0].forEach((value,index)=>view.setUint32(index*4,value,true));
                desc.slots.forEach((planes,slot)=>planes.forEach((plane,index)=>view.setInt32(24+slot*80+index*16+8,plane.allocate?1:0,true)));
                finish(1,BigInt(result.handle));return;
            }
            if(operation===3) {
                const bytes=span(buffer,264),view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
                if(!Number.isSafeInteger(result.slot)||result.slot<0||result.slot>=view.getInt32(0,true)||!Array.isArray(result.planes))
                    throw new Error('Invalid Bink SDK frame slot');
                for(const plane of result.planes) {
                    if(!Number.isSafeInteger(plane.plane)||plane.plane<0||plane.plane>3)throw new Error('Unsupported Bink plane');
                    const at=24+result.slot*80+plane.plane*16,pitch=view.getUint32(at+12,true);
                    const height=view.getUint32(plane.plane===1||plane.plane===2?16:8,true);
                    if(!view.getInt32(at+8,true)||pitch!==plane.pitch)throw new Error('Bink registered plane changed during decode');
                    copy(view.getBigUint64(at,true),plane.bytes,pitch*height);
                }
                view.setUint32(20,result.slot,true);
            } else if(operation===10)copy(buffer,result.summary,144);
            else if(operation===11)copy(buffer,result.offsets,Number(capacity));
            finish(1,result.value===undefined?0n:BigInt.asIntN(64,BigInt(result.value)));
        }).catch(error=> {
            // [skperf] round 72: once the worker has failed every later request rejects at once; log 8, then powers of two
            const n=Module['cp2077BinkFailLogs']=(Module['cp2077BinkFailLogs']||0)+1;
            if(n<=8||(n&(n-1))===0)console.error('CP2077 Bink SDK request failed (count='+n+')',error);
            finish(0);});
    }catch(error) {console.error('CP2077 Bink engine boundary failed',error);finish(0);}
});
// [skperf] round 7 (?perfprobe=1 only; flag and counters are defined in gpu-api-command-web.cpp): every Bink SDK
// call (open/close/frame/next/wait/shouldSkip/...) is a blocking round trip render-worker -> main thread -> Bink
// worker -> main thread -> render worker. Per-operation call counts and wall time feed 'CP2077 skperf crit probe'.
namespace cp2077::web { extern std::atomic<int> skperfEncodeProbe; extern std::atomic<unsigned long long> skperfBinkCalls[16], skperfBinkUs[16]; }
namespace {
bool Run(Request& request) {
    if(emscripten_is_main_runtime_thread())return false;
    auto* queue=Queue();if(!queue)return false;
    const bool timed=cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed)!=0;
    const double started=timed?emscripten_get_now():0;
    struct BinkTimer{bool on;double start;unsigned op;~BinkTimer(){if(!on)return;const unsigned i=op<16?op:15;
        cp2077::web::skperfBinkCalls[i].fetch_add(1,std::memory_order_relaxed);
        cp2077::web::skperfBinkUs[i].fetch_add((unsigned long long)((emscripten_get_now()-start)*1000.0),std::memory_order_relaxed);}} binkTimer{timed,started,unsigned(request.operation)};
    const bool completed=emscripten_proxy_sync_with_ctx(queue,emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context,void* argument) {
            auto& request=*static_cast<Request*>(argument);request.context=context;
            StartBinkRequest(reinterpret_cast<std::uintptr_t>(&request),request.operation,request.handle,request.flags,
                request.input,request.inputBytes,request.header,request.buffer,request.capacity,request.first,request.second);
        },&request);
    return completed&&request.succeeded;
}
S32 Control(const std::shared_ptr<Movie>& movie,Operation operation,S32 first=0,S32 second=0) {
    Request request;request.operation=operation;request.handle=movie->handle;request.header=&movie->native;
    request.first=first;request.second=second;
    if(!Run(request)) {movie->native.ReadError=1;return -1;}
    return static_cast<S32>(request.value);
}
U32 Read32(const unsigned char* bytes) {return U32(bytes[0])|U32(bytes[1])<<8|U32(bytes[2])<<16|U32(bytes[3])<<24;}
struct InputFile {
    UINTa state[8]{};
    FileCallbacks callbacks;
    bool opened=false;
    ~InputFile() {if(opened)callbacks.close(state);}
};
}
void RADEXPLINK BinkSetOSFileCallbacks(BINKFILEOPEN open,BINKFILEREAD read,BINKFILESEEK seek,BINKFILECLOSE close) {
    std::lock_guard<std::mutex> guard(fileMutex);fileCallbacks={open,read,seek,close};
}
HBINK RADEXPLINK BinkOpenWithOptions(const char* name,const BINK_OPEN_OPTIONS* options,U32 flags) {
    if(!name||emscripten_is_main_runtime_thread())return nullptr;
    // The original renderer's I/O callbacks load the same resource-depot bytes.
    // Whole-file staging replaces the platform I/O ring for this memory SDK path.
    constexpr size_t maximum=64*1024*1024;
    constexpr U32 supported=BINKFROMMEMORY|BINKNOFRAMEBUFFERS|BINKNOFILLIOBUF|BINKIOSIZE|BINKALPHA|
        BINKNOSKIP|BINKWILLLOOP|BINKDONTCLOSETHREADS|BINKUSETRIPLEBUFFERING|BINK_SLICES_MASK;
    if(flags&~supported)return nullptr;
    if(options&&(options->FileOffset||options->ForceRate||options->ForceRateDiv||options->Simulate||options->TotTracks||options->TrackNums||options->UserOpen))return nullptr;
    unsigned char header[8];InputFile file;
    if(flags&BINKFROMMEMORY)std::memcpy(header,name,sizeof(header));
    else {
        {std::lock_guard<std::mutex> guard(fileMutex);file.callbacks=fileCallbacks;}
        if(!file.callbacks.open||!file.callbacks.read||!file.callbacks.seek||!file.callbacks.close||!file.callbacks.open(file.state,name))return nullptr;
        file.opened=true;
        if(file.callbacks.read(file.state,header,sizeof(header))!=sizeof(header))return nullptr;
    }
    if(header[0]!='K'||header[1]!='B'||header[2]!='2')return nullptr;
    const std::uint64_t size=std::uint64_t(Read32(header+4))+8;
    if(size<32||size>maximum)return nullptr;
    std::unique_ptr<unsigned char,decltype(&std::free)> input(static_cast<unsigned char*>(std::malloc(size)),std::free);
    if(!input)return nullptr;
    if(flags&BINKFROMMEMORY)std::memcpy(input.get(),name,size);
    else {
        std::memcpy(input.get(),header,sizeof(header));size_t position=sizeof(header);
        while(position<size) {
            const U64 remaining=size-position,amount=remaining>1024*1024?1024*1024:remaining;
            if(file.callbacks.read(file.state,input.get()+position,amount)!=amount)return nullptr;
            position+=amount;
        }
    }
    auto movie=std::make_shared<Movie>();Request request;
    request.input=input.get();request.inputBytes=size;request.header=&movie->native;request.buffer=&movie->info;
    request.flags=flags&~(BINKFROMMEMORY|BINKNOFRAMEBUFFERS|BINKNOFILLIOBUF|BINKIOSIZE);
    if(!Run(request))return nullptr;
    movie->handle=static_cast<U32>(request.value);
    const size_t tableSize=(size_t(movie->native.Frames)+1)*sizeof(U64);
    if(movie->native.Frames>1048576) {Request close;close.operation=Close;close.handle=movie->handle;Run(close);return nullptr;}
    movie->offsets.reset(static_cast<U64*>(std::malloc(tableSize)));
    Request offsets;offsets.operation=Offsets;offsets.handle=movie->handle;offsets.buffer=movie->offsets.get();offsets.capacity=tableSize;
    if(!movie->offsets||!Run(offsets)) {Request close;close.operation=Close;close.handle=movie->handle;Run(close);return nullptr;}
    movie->native.frameoffsets=movie->offsets.get();
    std::lock_guard<std::mutex> guard(registryMutex);movies.emplace(&movie->native,movie);return &movie->native;
}
void RADEXPLINK BinkGetFrameBuffersInfo(HBINK native,BINKFRAMEBUFFERS* info) {
    auto movie=Find(native);if(!movie||!info)return;
    std::lock_guard<std::mutex> guard(movie->mutex);*info=movie->info;
    if(movie->registered)info->FrameNum=movie->registered->FrameNum;
}
void RADEXPLINK BinkRegisterFrameBuffers(HBINK native,BINKFRAMEBUFFERS* buffers) {
    auto movie=Find(native);if(!movie||!buffers)return;
    std::lock_guard<std::mutex> guard(movie->mutex);
    if(buffers->TotalFrames!=movie->info.TotalFrames||buffers->YABufferWidth!=movie->info.YABufferWidth||
       buffers->YABufferHeight!=movie->info.YABufferHeight||buffers->cRcBBufferWidth!=movie->info.cRcBBufferWidth||
       buffers->cRcBBufferHeight!=movie->info.cRcBBufferHeight) {native->ReadError=1;return;}
    Request request;request.operation=Register;request.handle=movie->handle;request.input=buffers;
    if(Run(request)) {movie->registered=buffers;native->FrameBuffers=buffers;}
    else native->ReadError=1;
}
S32 RADEXPLINK BinkDoFrame(HBINK native) {
    auto movie=Find(native);if(!movie)return -1;std::lock_guard<std::mutex> guard(movie->mutex);
    if(!movie->registered)return -1;
    Request request;request.operation=Frame;request.handle=movie->handle;request.header=native;request.buffer=movie->registered;
    if(!Run(request)) {native->ReadError=1;return -1;}return 0;
}
void RADEXPLINK BinkNextFrame(HBINK native) {
    auto movie=Find(native);if(!movie)return;
    if(CpBinkAsyncOn()&&PostAsync(movie,Next)){movie->cachedWait.store(movie->cachedWait.load()<0?-1:1);movie->cachedSkip.store(movie->cachedSkip.load()<0?-1:0);return;} // [skperf] round 66: next frame not due yet / not behind until the SDK says so
    std::lock_guard<std::mutex> guard(movie->mutex);Control(movie,Next);
}
S32 RADEXPLINK BinkPause(HBINK native,S32 pause) {
    auto movie=Find(native);if(!movie)return -1;
    if(CpBinkAsyncOn()&&PostAsync(movie,Pause,pause))return pause?1:0; // [skperf] round 66
    std::lock_guard<std::mutex> guard(movie->mutex);return Control(movie,Pause,pause);
}
S32 RADEXPLINK BinkWait(HBINK native) {
    auto movie=Find(native);if(!movie)return 1;
    if(CpBinkAsyncOn()){ // [skperf] round 66
        const int cached=movie->cachedWait.load();
        if(cached>=0){ if(!movie->waitInFlight.exchange(true)&&!PostAsync(movie,Wait))movie->waitInFlight.store(false); return cached; }
        std::lock_guard<std::mutex> guard(movie->mutex);const S32 v=Control(movie,Wait);movie->cachedWait.store(v<0?-1:int(v));return v;
    }
    std::lock_guard<std::mutex> guard(movie->mutex);return Control(movie,Wait);
}
S32 RADEXPLINK BinkShouldSkip(HBINK native) {
    auto movie=Find(native);if(!movie)return -1;
    if(CpBinkAsyncOn()){ // [skperf] round 66
        const int cached=movie->cachedSkip.load();
        if(cached>=0){ if(!movie->skipInFlight.exchange(true)&&!PostAsync(movie,Skip))movie->skipInFlight.store(false); return cached; }
        std::lock_guard<std::mutex> guard(movie->mutex);const S32 v=Control(movie,Skip);movie->cachedSkip.store(v<0?-1:int(v));return v;
    }
    std::lock_guard<std::mutex> guard(movie->mutex);return Control(movie,Skip);
}
void RADEXPLINK BinkGoto(HBINK native,U32 frame,S32 flags) {
    auto movie=Find(native);if(!movie)return;
    if(CpBinkAsyncOn()&&PostAsync(movie,Goto,S32(frame),flags)){movie->cachedWait.store(movie->cachedWait.load()<0?-1:1);movie->cachedSkip.store(movie->cachedSkip.load()<0?-1:0);return;} // [skperf] round 66
    std::lock_guard<std::mutex> guard(movie->mutex);Control(movie,Goto,frame,flags);
}
S32 RADEXPLINK BinkSetSoundOnOff(HBINK native,S32 enabled) {
    auto movie=Find(native);if(!movie)return -1;std::lock_guard<std::mutex> guard(movie->mutex);return Control(movie,Sound,enabled);
}
bool CpWebBinkSyncLoop(HBINK native,S32 enabled) {
    auto movie=Find(native);if(!movie)return false;
    if(CpBinkAsyncOn()&&PostAsync(movie,Loop,enabled))return true; // [skperf] round 66
    std::lock_guard<std::mutex> guard(movie->mutex);
    Control(movie,Loop,enabled);return true;
}
// 1=actual SDK snapshot, 0=busy, -1=original SDK request failure.
extern "C" S32 CpWebBinkTryGetSummary(HBINK native,BINKSUMMARY* summary,U32* frame) {
    auto movie=Find(native);if(!movie||!summary||!frame)return -1;
    if(CpBinkAsyncOn()){ // [skperf] round 67: latest summary now, refresh in the background (first call per movie stays synchronous)
        bool got=false;
        {std::lock_guard<std::mutex> cache(movie->summaryMutex);if(movie->hasSummary){*summary=movie->cachedSummary;got=true;}}
        if(got){*frame=native->FrameNum;if(!movie->summaryInFlight.exchange(true)&&!PostAsync(movie,Summary))movie->summaryInFlight.store(false);return 1;}
    }
    std::unique_lock<std::mutex> guard(movie->mutex,std::try_to_lock);
    if(!guard.owns_lock()) {
        static std::atomic<unsigned> reports{0};const unsigned count=reports.fetch_add(1)+1;
        if(count<=8||(count<=1024&&!(count&(count-1))))std::fprintf(stderr,"CP2077 Bink summary probe: phase=busy-read-avoided count=%u\n",count);
        return 0;
    }
    Request request;request.operation=Summary;request.handle=movie->handle;request.buffer=summary;
    if(!Run(request)){native->ReadError=1;return -1;}
    if(CpBinkAsyncOn()){std::lock_guard<std::mutex> cache(movie->summaryMutex);movie->cachedSummary=*summary;movie->hasSummary=true;} // [skperf] round 67
    *frame=native->FrameNum;return 1;
}
void RADEXPLINK BinkGetSummary(HBINK native,BINKSUMMARY* summary) {
    auto movie=Find(native);if(!movie||!summary)return;std::lock_guard<std::mutex> guard(movie->mutex);
    Request request;request.operation=Summary;request.handle=movie->handle;request.buffer=summary;
    if(!Run(request))native->ReadError=1;
}
void RADEXPLINK BinkClose(HBINK native) {
    auto movie=Find(native);if(!movie)return;
    {std::unique_lock<std::mutex> guard(movie->asyncMutex);movie->closing=true;movie->asyncFinished.wait(guard,[&]{return !movie->asyncPending;});}
    std::lock_guard<std::mutex> guard(movie->mutex);
    Request request;request.operation=Close;request.handle=movie->handle;
    if(!Run(request)) {native->ReadError=1;return;}
    std::lock_guard<std::mutex> registryGuard(registryMutex);movies.erase(native);
}
void RADEXPLINK BinkFreeGlobals() {
    {std::lock_guard<std::mutex> guard(registryMutex);if(!movies.empty())return;}
    Request request;request.operation=FreeGlobals;Run(request);
}
namespace {
void* AsyncMain(void* argument) {
    auto& slot=*static_cast<AsyncSlot*>(argument);
    {std::lock_guard<std::mutex> guard(slot.mutex);slot.ready=true;slot.wake.notify_all();}
    for(;;) {
        std::shared_ptr<Movie> movie;
        {
            std::unique_lock<std::mutex> guard(slot.mutex);
            slot.wake.wait(guard,[&]{return slot.stopping||!slot.jobs.empty();});
            if(slot.jobs.empty()&&slot.stopping)break;
            movie=std::move(slot.jobs.front());slot.jobs.pop_front();
        }
        // Browser scheduling executes the original whole-frame decoder on its
        // private CPU worker. Requests may be serialized there, but completion
        // here is asynchronous and reflects actual decoded plane publication.
        BinkDoFrame(&movie->native);
        {std::lock_guard<std::mutex> guard(movie->asyncMutex);movie->asyncPending=false;}
        movie->asyncFinished.notify_all();
    }
    return nullptr;
}
}
S32 RADEXPLINK BinkStartAsyncThread(S32 number,const void* parameter) {
    if(number<0||number>=8||parameter||emscripten_is_main_runtime_thread())return 0;
    std::unique_lock<std::mutex> registry(slotsMutex);
    if(asyncSlots[number])return 0;
    auto slot=std::make_shared<AsyncSlot>();
    if(pthread_create(&slot->thread,nullptr,AsyncMain,slot.get())!=0)return 0;
    asyncSlots[number]=slot;registry.unlock();
    std::unique_lock<std::mutex> guard(slot->mutex);slot->wake.wait(guard,[&]{return slot->ready;});return 1;
}
S32 RADEXPLINK BinkDoFrameAsyncMulti(HBINK native,U32* threads,S32 count) {
    auto movie=Find(native);if(!movie||!threads||count<=0||count>8)return 0;
    std::lock_guard<std::mutex> registry(slotsMutex);
    for(S32 index=0;index<count;++index)if(threads[index]>=8||!asyncSlots[threads[index]])return 0;
    auto slot=asyncSlots[threads[nextAsyncSlot.fetch_add(1)%count]];
    std::lock_guard<std::mutex> slotGuard(slot->mutex);if(slot->stopping)return 0;
    std::lock_guard<std::mutex> movieGuard(movie->asyncMutex);if(movie->asyncPending||movie->closing)return 0;
    movie->asyncPending=true;slot->jobs.push_back(movie);slot->wake.notify_one();return 1;
}
S32 RADEXPLINK BinkDoFrameAsyncWait(HBINK native,S32 microseconds) {
    auto movie=Find(native);if(!movie)return 0;
    std::unique_lock<std::mutex> guard(movie->asyncMutex);
    if(microseconds<0)movie->asyncFinished.wait(guard,[&]{return !movie->asyncPending;});
    else if(microseconds>0)movie->asyncFinished.wait_for(guard,std::chrono::microseconds(microseconds),[&]{return !movie->asyncPending;});
    return movie->asyncPending?0:1;
}
S32 RADEXPLINK BinkRequestStopAsyncThreadsMulti(U32* threads,S32 count) {
    if(!threads||count<=0||count>8)return 0;
    std::lock_guard<std::mutex> registry(slotsMutex);
    for(S32 index=0;index<count;++index)if(threads[index]>=8||!asyncSlots[threads[index]])return 0;
    for(S32 index=0;index<count;++index) {
        auto slot=asyncSlots[threads[index]];std::lock_guard<std::mutex> guard(slot->mutex);
        slot->stopping=true;slot->wake.notify_all();
    }
    return 1;
}
S32 RADEXPLINK BinkWaitStopAsyncThreadsMulti(U32* threads,S32 count) {
    if(!threads||count<=0||count>8)return 0;
    std::array<std::shared_ptr<AsyncSlot>,8> slots;
    {
        std::lock_guard<std::mutex> registry(slotsMutex);
        for(S32 index=0;index<count;++index) {
            if(threads[index]>=8||!asyncSlots[threads[index]])return 0;
            for(S32 previous=0;previous<index;++previous)if(threads[index]==threads[previous])return 0;
            slots[index]=asyncSlots[threads[index]];
            std::lock_guard<std::mutex> guard(slots[index]->mutex);if(!slots[index]->stopping)return 0;
        }
    }
    for(S32 index=0;index<count;++index)if(pthread_join(slots[index]->thread,nullptr)!=0)return 0;
    std::lock_guard<std::mutex> registry(slotsMutex);
    for(S32 index=0;index<count;++index)if(asyncSlots[threads[index]]==slots[index])asyncSlots[threads[index]].reset();
    return 1;
}
