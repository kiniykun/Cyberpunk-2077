#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/oodle/include/oodle2.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdint>
namespace {
struct EncodeRequest {
    em_proxying_ctx* context=nullptr;
    const void* source=nullptr;
    void* output=nullptr;
    SINTa raw=0,capacity=0;
    int level=0;
    unsigned result=0;
};
em_proxying_queue* EncodeQueue() {static auto* queue=em_proxying_queue_create();return queue;}
}
extern "C" EMSCRIPTEN_KEEPALIVE void CpWebOodleEncodeComplete(std::uintptr_t pointer,unsigned result) {
    auto& request=*reinterpret_cast<EncodeRequest*>(pointer);request.result=result;
    emscripten_proxy_finish(request.context);
}
EM_JS(void,StartOodleEncode,(std::uintptr_t request,const void* source,void* output,SINTa raw,SINTa capacity,int level), {
    let finished=false;
    const finish=bytes=>{if(finished)return;finished=true;Module['_CpWebOodleEncodeComplete'](request,bytes);};
    try {
        const begin=Number(source),destination=Number(output),size=Number(raw),bound=Number(capacity);
        if(!Number.isSafeInteger(begin)||!Number.isSafeInteger(destination)||begin<0||destination<0||
            !Number.isSafeInteger(size)||!Number.isSafeInteger(bound)||size<=0||bound<=0||
            begin>HEAPU8.length-size||destination>HEAPU8.length-bound||typeof Module['cp2077EncodeOodle']!=='function') {finish(0);return;}
        Module['cp2077EncodeOodle'](HEAPU8.subarray(begin,begin+size),level).then(encoded=> {
            if(!(encoded instanceof Uint8Array)||encoded.length<=0||encoded.length>bound||destination>HEAPU8.length-bound) {finish(0);return;}
            HEAPU8.set(encoded,destination);finish(encoded.length);
        },error=>{console.error('CP2077 Kraken encoding failed',error);finish(0);}).catch(error=> {
            console.error('CP2077 Kraken output copy failed',error);finish(0);
        });
    }catch(error){console.error('CP2077 Kraken encoder boundary failed',error);finish(0);}
});
SINTa OodleLZ_Compress(OodleLZ_Compressor compressor,const void* raw,SINTa rawLength,void* output,
    OodleLZ_CompressionLevel level,const OodleLZ_CompressOptions* options,const void* dictionary,
    const void* lrm,void* scratch,SINTa scratchSize) {
    // Preserve the original engine wrapper's real Normal/Optimal Kraken path.
    // Other option sets require explicit translation; do not silently discard.
    if(compressor!=OodleLZ_Compressor_Kraken||!raw||!output||rawLength<=0||rawLength>128*1024*1024||
        (level!=OodleLZ_CompressionLevel_Normal&&level!=OodleLZ_CompressionLevel_Optimal)||
        options||dictionary||lrm||scratch||scratchSize||emscripten_is_main_runtime_thread())return 0;
    const SINTa capacity=OodleLZ_GetCompressedBufferSizeNeeded(rawLength);if(capacity<=0)return 0;
    auto* queue=EncodeQueue();if(!queue)return 0;
    EncodeRequest request;request.source=raw;request.output=output;request.raw=rawLength;request.capacity=capacity;request.level=level;
    const bool completed=emscripten_proxy_sync_with_ctx(queue,emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context,void* argument) {
            auto& request=*static_cast<EncodeRequest*>(argument);request.context=context;
            StartOodleEncode(reinterpret_cast<std::uintptr_t>(&request),request.source,request.output,request.raw,request.capacity,request.level);
        },&request);
    return completed?static_cast<SINTa>(request.result):0;
}
