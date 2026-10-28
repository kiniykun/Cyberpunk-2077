// Actual decoder runs in a private, non-shared Wasm memory, owned by this worker and reused
// until any failure; malformed streams cannot corrupt the engine, and a failure discards the instance.
import createDecoder from './oodle-decoder.mjs';
const maximumBytes=128*1024*1024;
const guard=64,decoderPadding=64;
// [oodlefix] retry the decoder module download (a dropped fetch here used to kill the whole decode pool)
async function loadDecoderModule(){
    for(let attempt=0;;++attempt){
        try{
            const response=await fetch(new URL('./oodle-decoder.wasm',import.meta.url));
            if(!response.ok)throw new Error('Private Oodle decoder module could not be loaded: '+response.status);
            return await WebAssembly.compile(await response.arrayBuffer());
        }catch(error){if(attempt>=3)throw error;await new Promise(r=>setTimeout(r,500*(attempt+1)));}
    }
}
const compiledDecoder=await loadDecoderModule();
function freshDecoder(){
    return createDecoder({instantiateWasm(imports,receive){
        const instance=new WebAssembly.Instance(compiledDecoder,imports);
        receive(instance,compiledDecoder);return instance.exports;
    }});
}
let busy=false;
// [compress] Decoder instance reuse: instantiating a private instance per job was ~45% of worker time
// (0.9 ms/job, 18k jobs per sandbox load). The instance stays private to this worker, guards and input checks
// still run every job, and ANY failure discards it (next job gets a fresh one). Buffers are reused and only grow;
// the instance is also retired after maxJobs or once grown-away buffers exceed maxLeakBytes.
const maxJobs=4096,maxLeakBytes=64*1024*1024;
let cached=null;
async function reusableDecoder(sourceBytes,destinationBytes){
    if(!cached)cached={codec:await freshDecoder(),jobs:0,leak:0,src:0,srcCap:0,dst:0,dstCap:0};
    const c=cached;
    if(c.srcCap<sourceBytes){c.leak+=c.srcCap;c.srcCap=Math.max(sourceBytes,65536);c.src=c.codec._malloc(c.srcCap);}
    if(c.dstCap<destinationBytes){c.leak+=c.dstCap;c.dstCap=Math.max(destinationBytes,262144);c.dst=c.codec._malloc(c.dstCap);}
    if(!c.src||!c.dst){cached=null;throw new Error('Decoder allocation failed');}
    return c;
}
self.onmessage=async ({data:request}) => {
    const id=request?.id;
    if(busy) {self.postMessage({id,ok:false,error:'Decoder worker is busy'});return;}
    busy=true;
    try {
        if(request.operation==='compress') {
            const input=request.raw;
            if(!(input instanceof ArrayBuffer)||input.byteLength<1||input.byteLength>maximumBytes||
                (request.level!==4&&request.level!==6))throw new Error('Unsupported encoder input or level');
            const codec=await freshDecoder(),original=new Uint8Array(input);
            const capacity=input.byteLength+274*Math.ceil(input.byteLength/262144);
            const source=codec._malloc(input.byteLength+guard+decoderPadding+guard);
            const destination=codec._malloc(capacity+guard+decoderPadding+guard);
            if(!source||!destination)throw new Error('Encoder allocation failed');
            codec.HEAPU8.fill(0xa5,source,source+guard);
            codec.HEAPU8.set(original,source+guard);
            codec.HEAPU8.fill(0,source+guard+input.byteLength,source+guard+input.byteLength+decoderPadding);
            codec.HEAPU8.fill(0x5a,source+guard+input.byteLength+decoderPadding,source+input.byteLength+guard+decoderPadding+guard);
            codec.HEAPU8.fill(0xa5,destination,destination+guard);
            codec.HEAPU8.fill(0,destination+guard,destination+guard+capacity+decoderPadding);
            codec.HEAPU8.fill(0x5a,destination+guard+capacity+decoderPadding,destination+capacity+guard+decoderPadding+guard);
            const size=codec._CpWebKrakenEncode(source+guard,input.byteLength,destination+guard,capacity,request.level);
            if(size<=0||size>capacity)throw new Error('Kraken encoding failed');
            const heap=codec.HEAPU8;
            for(const [pointer,expected] of [[source,0xa5],[destination,0xa5],
                [source+guard+input.byteLength+decoderPadding,0x5a],[destination+guard+capacity+decoderPadding,0x5a]]) {
                for(let i=0;i<guard;++i)if(heap[pointer+i]!==expected)throw new Error('Encoder buffer boundary was violated');
            }
            for(let i=0;i<original.length;++i)if(heap[source+guard+i]!==original[i])throw new Error('Encoder modified its input');
            const output=heap.slice(destination+guard,destination+guard+size);
            self.postMessage({id,ok:true,output:output.buffer},[output.buffer]);return;
        }
        if(request.operation && request.operation!=='decompress')throw new Error('Unknown codec operation');
        const input=request.compressed,rawLength=request.rawLength;
        if(!(input instanceof ArrayBuffer) || input.byteLength<2 || input.byteLength>maximumBytes ||
           !Number.isSafeInteger(rawLength) || rawLength<=0 || rawLength>maximumBytes)
            throw new Error('Invalid decoder input or unsupported allocation size');
        const began=performance.now(); // [compress] timing only
        const reused=await reusableDecoder(input.byteLength+guard+decoderPadding+guard,rawLength+guard+decoderPadding+guard);
        const decoder=reused.codec,source=reused.src,destination=reused.dst;
        cached=null; // re-armed only after this job fully succeeds
        const instantiated=performance.now();
        if(!source || !destination) throw new Error('Decoder allocation failed');
        const original=new Uint8Array(input);
        decoder.HEAPU8.fill(0xa5,source,source+guard);
        decoder.HEAPU8.set(original,source+guard);
        decoder.HEAPU8.fill(0,source+guard+input.byteLength,source+guard+input.byteLength+decoderPadding);
        decoder.HEAPU8.fill(0x5a,source+guard+input.byteLength+decoderPadding,source+input.byteLength+guard+decoderPadding+guard);
        decoder.HEAPU8.fill(0xa5,destination,destination+guard);
        decoder.HEAPU8.fill(0,destination+guard,destination+guard+rawLength+decoderPadding);
        decoder.HEAPU8.fill(0x5a,destination+guard+rawLength+decoderPadding,destination+rawLength+guard+decoderPadding+guard);
        const decodeBegan=performance.now();
        const size=decoder._Kraken_Decompress(source+guard,input.byteLength,destination+guard,rawLength);
        const decoded=performance.now();
        if(size!==rawLength) throw new Error('Compressed stream was rejected');
        const heap=decoder.HEAPU8; // Decoder allocations may grow its private memory.
        for(const [pointer,expected] of [[source,0xa5],[destination,0xa5],
            [source+guard+input.byteLength+decoderPadding,0x5a],[destination+guard+rawLength+decoderPadding,0x5a]]) {
            for(let i=0;i<guard;++i) if(heap[pointer+i]!==expected) throw new Error('Decoder buffer boundary was violated');
        }
        for(let i=0;i<original.length;++i) if(heap[source+guard+i]!==original[i]) throw new Error('Decoder modified its input');
        const output=heap.slice(destination+guard,destination+guard+rawLength);
        // Discard the whole instance rather than trusting heap metadata after
        // processing an untrusted stream. Only exact output bytes cross back.
        if(++reused.jobs<maxJobs&&reused.leak<maxLeakBytes)cached=reused;
        self.postMessage({id,ok:true,output:output.buffer,timing:{instMs:instantiated-began,decodeMs:decoded-decodeBegan,totalMs:performance.now()-began}},[output.buffer]);
    } catch(error) {
        self.postMessage({id,ok:false,error:String(error?.message || error)});
    } finally {busy=false;}
};
self.postMessage({kind:'ready'});
