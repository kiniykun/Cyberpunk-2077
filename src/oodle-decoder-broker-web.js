// Browser-main-thread boundary. Scheduling and deadlines run in a separate
// pool manager, so synchronous graphics work cannot expire finished decodes.
(()=>{
    let manager=null,nextId=1,failure=null;
    const pending=new Map();
    Module.cp2077OodleStats={active:0,queued:0,finished:0,failed:0,peakActive:0,peakQueued:0};
    // [oodlefix] 2026-10-24: a pool failure was permanent (every later decode failed at once = engine abort). Now the
    // pool is discarded and the next request starts a fresh one; permanent only after 8 pool failures in a session.
    let poolFailures=0;
    function fail(error){
        manager?.terminate();manager=null;
        console.error('CP2077 Oodle pool failed ('+(poolFailures+1)+'): '+String(error&&error.message||error));
        if(++poolFailures>=8)failure=error;
        for(const request of pending.values())request.reject(error);pending.clear();
    }
    function initialize(){
        if(manager)return;
        manager=new Worker('/runtime/codec/oodle-decoder-pool.mjs',{type:'module'});
        manager.onerror=e=>fail(new Error(e.message||'Oodle pool manager failed'));
        manager.onmessageerror=()=>fail(new Error('Oodle pool reply could not be read'));
        manager.onmessage=({data})=>{
            if(data?.kind==='stats'){Object.assign(Module.cp2077OodleStats,data.stats);return;}
            if(data?.fatal){fail(new Error(data.error||'Oodle pool initialization failed'));return;}
            const request=pending.get(data?.id);
            if(!request){fail(new Error('Unexpected Oodle pool response'));return;}
            pending.delete(data.id);
            if(!data.ok){request.reject(new Error(data.error||'Oodle codec rejected its input'));return;}
            if(!(data.output instanceof ArrayBuffer)||(request.encoding?
                data.output.byteLength<=0||data.output.byteLength>request.capacity:data.output.byteLength!==request.capacity)){
                const invalid=new Error('Invalid Oodle pool output');fail(invalid);request.reject(invalid);return;
            }
            request.resolve(new Uint8Array(data.output));
        };
    }
    Module.cp2077OodleRequest=(compressed,rawLength,operation,level)=>new Promise((resolve,reject)=>{
        const encoding=operation==='compress';
        if(!(compressed instanceof Uint8Array)||compressed.byteLength<(encoding?1:2)||compressed.byteLength>128*1024*1024||
            !Number.isSafeInteger(rawLength)||rawLength<=0||rawLength>128*1024*1024){reject(new Error('Invalid Oodle request'));return;}
        if(failure){reject(failure);return;}
        const input=Uint8Array.from(compressed),id=nextId++;
        try{
            initialize();
            pending.set(id,{resolve,reject,encoding,capacity:encoding?rawLength+274*Math.ceil(rawLength/262144):rawLength});
            manager.postMessage(encoding?{id,operation,raw:input.buffer,level}:
                {id,operation:'decompress',compressed:input.buffer,rawLength},[input.buffer]);
        }catch(error){pending.delete(id);reject(error);}
    });
    // [poolsize] 2026-10-23: one retry. A transient pool failure (queue full, execution deadline, a crashed decoder
    // worker) used to reach the engine as a Kraken assertion = abort. Corrupt data simply fails twice. The requesting
    // engine thread waits synchronously, so the source bytes are still valid for the second attempt.
    // [oodlefix] two retries (the second after a short pause, e.g. while a fresh pool starts); reasons are logged with
    // "failed" in the text so the stall/fail reports keep them.
    Module.cp2077DecodeOodle=(compressed,rawLength)=>{
        const attempt=n=>Module.cp2077OodleRequest(compressed,rawLength,'decompress').catch(error=>{
            const why=String(error&&error.message||error);
            if(failure||n>=2){console.error('CP2077 Oodle decode failed after '+(n+1)+' tries: '+why);throw error;}
            console.error('CP2077 Oodle decode attempt '+(n+1)+' failed, retrying: '+why);
            return new Promise(r=>setTimeout(r,n?400:0)).then(()=>attempt(n+1));
        });
        return attempt(0);
    };
    Module.cp2077EncodeOodle=(raw,level)=>Module.cp2077OodleRequest(raw,raw.length,'compress',level);
})();
