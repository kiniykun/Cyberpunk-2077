#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/oodle/include/oodle2.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "oodle-hooks-web.h"
namespace {
struct DecodeRequest {
    em_proxying_ctx* context=nullptr;
    const void* source=nullptr;
    void* output=nullptr;
    SINTa compressed=0,raw=0;
    bool succeeded=false;
};
em_proxying_queue* DecodeQueue() {static auto* queue=em_proxying_queue_create();return queue;}
}
extern "C" EMSCRIPTEN_KEEPALIVE void CpWebOodleDecodeComplete(std::uintptr_t address,int success) {
    auto& request=*reinterpret_cast<DecodeRequest*>(address);
    request.succeeded=success!=0;
    emscripten_proxy_finish(request.context);
}
EM_JS(void, StartOodleDecode, (std::uintptr_t request,const void* source,void* output,SINTa compressed,SINTa raw), {
    let finished=false;
    const finish=success=> {if(finished)return;finished=true;Module['_CpWebOodleDecodeComplete'](request,success);};
    try {
        const begin=Number(source),destination=Number(output),size=Number(compressed),expected=Number(raw);
        const heap=HEAPU8;
        if(!Number.isSafeInteger(begin)||!Number.isSafeInteger(destination)||begin<0||destination<0||
           !Number.isSafeInteger(size)||!Number.isSafeInteger(expected)||size<2||expected<=0||
           begin>heap.byteLength-size||destination>heap.byteLength-expected||
           typeof Module['cp2077DecodeOodle']!=='function') {finish(0);return;}
        Module['cp2077DecodeOodle'](heap.subarray(begin,begin+size),expected).then(decoded=> {
            // Reacquire after asynchronous decode: the engine's shared memory
            // can grow while this request is outstanding.
            if(!(decoded instanceof Uint8Array)||decoded.length!==expected||destination>HEAPU8.length-expected) {finish(0);return;}
            HEAPU8.set(decoded,destination);finish(1);
        },error=> {console.error('CP2077 Oodle decode failed',error);finish(0);}).catch(error=> {
            console.error('CP2077 Oodle output copy failed',error);finish(0);
        });
    } catch(error) {console.error('CP2077 Oodle decoder boundary failed',error);finish(0);}
});
SINTa OodleLZ_Decompress(const void* compressed,SINTa compressedSize,void* output,SINTa rawLength,
    OodleLZ_FuzzSafe fuzzSafe,OodleLZ_CheckCRC checkCRC,OodleLZ_Verbosity verbosity,
    void* dictionaryBase,SINTa dictionarySize,OodleDecompressCallback* callback,void* callbackData,
    void* decoderMemory,SINTa decoderMemorySize,OodleLZ_Decode_ThreadPhase phase) {
    // Original engine wrapper uses exactly this non-dictionary, unthreaded,
    // callback-free path. Reject unsupported modes instead of silently dropping
    // dictionary state, CRC requests, cancellation callbacks or phased decoding.
    constexpr SINTa maximumBytes=128*1024*1024;
    if(!compressed||!output||compressedSize<2||compressedSize>maximumBytes||rawLength<=0||rawLength>maximumBytes||
        fuzzSafe!=OodleLZ_FuzzSafe_Yes||checkCRC!=OodleLZ_CheckCRC_Yes||verbosity!=OodleLZ_Verbosity_None||
        dictionaryBase||dictionarySize||callback||callbackData||phase!=OodleLZ_Decode_Unthreaded||
        decoderMemorySize<0||(!decoderMemory&&decoderMemorySize)||emscripten_is_main_runtime_thread()) return 0;
    const SINTa requiredWorkspace=OodleLZDecoder_MemorySizeNeeded(OodleLZ_Compressor_Kraken,rawLength);
    if(decoderMemory && decoderMemorySize<requiredWorkspace)return 0;
    CpWebOodleWorkspace ownedWorkspace;
    if(!decoderMemory) {
        const SINTa allocationSize=compressedSize>requiredWorkspace?compressedSize:requiredWorkspace;
        if(!CpWebOodleAllocateWorkspace(allocationSize,ownedWorkspace))return 0;
        decoderMemory=ownedWorkspace.data;decoderMemorySize=allocationSize;
    }
    const void* input=compressed;
    // Use supplied caller-owned scratch for staging when it fits. Decoder
    // internals live in a separate private Wasm memory and never see this pointer.
    if(decoderMemory&&decoderMemorySize>=compressedSize) {
        const auto staging=reinterpret_cast<std::uintptr_t>(decoderMemory);
        const auto target=reinterpret_cast<std::uintptr_t>(output);
        if(staging<=UINTPTR_MAX-static_cast<std::uintptr_t>(compressedSize)&&
            target<=UINTPTR_MAX-static_cast<std::uintptr_t>(rawLength)&&
            (staging+compressedSize<=target||target+rawLength<=staging)) {
            std::memmove(decoderMemory,compressed,static_cast<size_t>(compressedSize));input=decoderMemory;
        }
    }
    auto* queue=DecodeQueue();if(!queue) {
        CpWebOodleReportAssertion(__FILE__,__LINE__,__func__,"Unable to create the decoder proxy queue");return 0;
    }
    DecodeRequest request;request.source=input;request.output=output;request.compressed=compressedSize;request.raw=rawLength;
    const bool completed=emscripten_proxy_sync_with_ctx(queue,emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context,void* argument) {
            auto& request=*static_cast<DecodeRequest*>(argument);request.context=context;
            StartOodleDecode(reinterpret_cast<std::uintptr_t>(&request),request.source,request.output,request.compressed,request.raw);
        },&request);
    return completed&&request.succeeded ? rawLength : 0;
}
