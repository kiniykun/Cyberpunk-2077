// Browser-main-thread transport for the engine's original Bink boundary.
// The private SDK worker owns its CPU memory; only metadata and real decoded
// plane copies are returned. A failed worker invalidates all of its handles.
(() => {
    let worker=null,ready=null,nextId=1,failure=null,memoryChannel=null;
    const pending=new Map();
    const query=(name)=>{try{return new URLSearchParams(location.search).get(name);}catch(e){return null;}};
    // [skperf] round 72 binkWatch (opt-in ?binkWatch=1): the worker runs requests one at a time, so the 30 s deadline
    // applies to the request at the head of its queue (armed when it becomes the head) instead of to every request from
    // the moment it was posted. [skperf] round 73: the head deadline counts only time this main thread was itself
    // responsive (1 s watchdog ticks; a tick that arrives late because the main thread was pegged counts as 1 s), since
    // the worker's SDK allocations wait on this thread. When it fires it logs the head request and its movie's history.
    const headWatch=query('binkWatch')==='1'||query('binkRecycle')==='1';
    // [skperf] round 73 binkRecycle (opt-in ?binkRecycle=1): a failed worker is replaced instead of failing every later
    // request. Movie handles seen by the engine are the broker's own; each maps to a handle of the current worker. After
    // a failure the broker starts a new worker and re-opens every live movie from the same file bytes (kept by the
    // broker), re-registers its frame buffers and goes back to its last decoded frame (loop/pause restored). Requests in
    // flight at the failure fail (as before); later ones go to the re-opened movies.
    const recycle=query('binkRecycle')==='1';
    const order=[];let headTimer=null,headCounted=0,headLastTick=0;
    function headInfo() {
        const head=pending.get(order[0]);if(!head)return 'none';
        const m=timings.movies[head.handle];
        return 'type='+head.type+' handle='+head.handle+' ageMs='+Math.round(performance.now()-head.started)+' countedMs='+Math.round(headCounted)+
            ' pending='+pending.size+(m?' backend='+(m.backend||'?')+' frames='+m.frames+' lastFrame='+m.lastFrame+' lastFrameMs='+Math.round(m.lastFrameMs||0)+' maxFrameMs='+Math.round(m.maxFrameMs||0):'');
    }
    function headTick() {
        headTimer=null;
        if(!order.length)return;
        const now=performance.now();
        headCounted+=Math.min(now-headLastTick,1000);headLastTick=now;
        if(headCounted>=30000){
            const info=headInfo();
            console.error('CP2077 Bink watchdog: head request over 30 s of responsive main-thread time: '+info);
            fail(new Error('Bink worker request deadline exceeded ('+info+')'));return;
        }
        headTimer=setTimeout(headTick,1000);
    }
    function armHead() {
        if(!headWatch)return;
        clearTimeout(headTimer);headTimer=null;
        while(order.length&&!pending.has(order[0]))order.shift();
        if(order.length){headCounted=0;headLastTick=performance.now();headTimer=setTimeout(headTick,1000);}
    }
    const timings=Module.cp2077BinkTimings={requests:0,pending:0,byType:{},movies:{},recycles:0};
    // [skperf] round 73 binkRecycle state
    const movies=new Map(); // broker handle -> {bytes,flags,callerFrameBuffers,pitches,lastFrame,loop,paused,workerHandle,dead}
    let nextBrokerHandle=1;
    function fail(error) {
        if(failure)return;
        failure=error;worker?.terminate();worker=null;clearTimeout(headTimer);headTimer=null;order.length=0;
        for(const request of pending.values()) {clearTimeout(request.timer);request.reject(error);}
        pending.clear();
        if(recycle&&timings.recycles<4){ // at most 4 restarts per page; after that it fails as before
            // The terminated worker's SDK allocations live in engine memory; release them before a new worker starts.
            Module['_CpWebBinkReleaseSdkMemory']?.();
            timings.recycles++;
            console.error('CP2077 Bink recycle: worker failed ('+(error?.message||error)+'); restarting it and re-opening '+movies.size+' movie(s)');
            failure=null;ready=null;
            for(const movie of movies.values())movie.workerHandle=0;
        }
    }
    function send(request,transfers=[]) {
        if(failure)return Promise.reject(failure);
        if(!worker)return Promise.reject(new Error('Bink worker has not been created'));
        const id=nextId++;
        return new Promise((resolve,reject)=> {
            const timer=headWatch?null:setTimeout(()=>fail(new Error('Bink worker request deadline exceeded')),30000);
            pending.set(id,{resolve,reject,timer,type:request.type,handle:request.handle,started:performance.now()});
            if(headWatch){order.push(id);if(order.length===1)armHead();}
            timings.pending=pending.size;
            try {worker.postMessage({id,...request},transfers);}
            catch(error) {clearTimeout(timer);pending.delete(id);if(headWatch)armHead();reject(error);}
        });
    }
    async function initialize() {
        const response=await fetch('/sdk/bink2w64.dll');
        if(!response.ok)throw new Error('Original local Bink SDK could not be loaded');
        const sdkBytes=await response.arrayBuffer();
        const digest=await crypto.subtle.digest('SHA-256',sdkBytes);
        const hash=Array.from(new Uint8Array(digest),byte=>byte.toString(16).padStart(2,'0')).join('');
        if(hash!=='4f60fb1acd3cc03da501fadc378b093f5ec99f5c99a7bb9d1013603fb8e74334')
            throw new Error('Local Bink SDK differs from the verified ABI build');
        worker=new Worker('/runtime/codec/bink/bink-decoder-worker.js');
        memoryChannel=new SharedArrayBuffer(32);
        worker.onerror=event=>fail(new Error(event.message||'Bink CPU worker failed'));
        worker.onmessageerror=()=>fail(new Error('Bink worker reply could not be read'));
        worker.onmessage=({data})=> {
            if(data?.kind==='binkMemory') {
                const control=new Int32Array(memoryChannel),reply=new BigUint64Array(memoryChannel);
                try {
                    if(data.operation==='allocate') {
                        const size=BigInt(data.value);
                        if(size<0n||size>128n*1024n*1024n)throw new Error('Invalid SDK allocation size');
                        reply[1]=BigInt.asUintN(64,Module['_CpWebBinkAllocateSdk'](size));
                    }else if(data.operation==='free') {
                        if(!Module['_CpWebBinkFreeSdk'](BigInt(data.value)))throw new Error('Unknown SDK backing allocation');
                        reply[1]=0n;
                    }else throw new Error('Unknown SDK memory operation');
                    Atomics.store(control,1,1);
                }catch(error) {console.error('CP2077 Bink allocator service failed',error);Atomics.store(control,1,0);}
                Atomics.store(control,0,1);Atomics.notify(control,0,1);return;
            }
            const request=pending.get(data?.id);
            if(!request) {fail(new Error('Unexpected Bink worker reply'));return;}
            if(data.fatal) {
                if(!recycle||timings.recycles>=4)Module['_CpWebBinkReleaseSdkMemory']?.(); // a recycle releases it in fail()
                fail(new Error(data.error||'Bink SDK worker state became invalid'));return;
            }
            pending.delete(data.id);clearTimeout(request.timer);
            if(headWatch)armHead();
            if(typeof data.sdkInitialized==='boolean')timings.sdkInitialized=data.sdkInitialized;
            timings.pending=pending.size; timings.requests++;
            const elapsed=performance.now()-request.started;
            const entry=timings.byType[request.type] ||= {count:0,totalMs:0,maxMs:0,errors:0};
            entry.count++;entry.totalMs+=elapsed;entry.maxMs=Math.max(entry.maxMs,elapsed);
            const movieHandle=request.type==='open'?data.result?.handle:request.handle;
            if(movieHandle!==undefined){
                const movie=timings.movies[movieHandle] ||= {frames:0,totalFrameMs:0,maxFrameMs:0};
                if(data.result?.backend)movie.backend=data.result.backend;
                if(data.result?.assetSHA256)movie.assetSHA256=data.result.assetSHA256;
                if(request.type==='frame'&&!data.error){movie.frames++;movie.totalFrameMs+=elapsed;movie.maxFrameMs=Math.max(movie.maxFrameMs,elapsed);movie.lastFrameMs=elapsed;movie.lastFrame=data.result.frame;}
                if(request.type==='close'&&!data.error)movie.closed=true;
            }
            if(data.result?.backend){timings.backend=data.result.backend;if(Number.isSafeInteger(data.result.decodedCacheBytes))timings.decodedCacheBytes=data.result.decodedCacheBytes;}
            if(data.error)entry.errors++;
            data.error?request.reject(new Error(data.error)):request.resolve(data.result);
        };
        return send({type:'initialize',sdkBytes,engineMemory:wasmMemory,memoryChannel,runtimeUrl:'/runtime/codec/bink/unicorn_x86.js',
            vmUrl:'/runtime/codec/bink/bink-sdk-vm.mjs',platformUrl:'/runtime/codec/bink/bink-platform-web.mjs',
            frameBuffersUrl:'/runtime/codec/bink/bink-framebuffers-web.mjs',memoryClientUrl:'/runtime/codec/bink/bink-memory-client-web.mjs'},[sdkBytes]);
    }
    // [skperf] round 73 binkRecycle: after a new worker is up, re-open every live movie in it.
    async function reopenAll() {
        for(const [brokerHandle,movie] of movies) {
            if(movie.dead||movie.workerHandle)continue;
            try {
                const bytes=movie.bytes.slice(0);
                const opened=await send({type:'open',bytes,flags:movie.flags,callerFrameBuffers:movie.callerFrameBuffers},[bytes]);
                movie.workerHandle=opened.handle;
                if(movie.pitches)await send({type:'registerFrameBuffers',handle:movie.workerHandle,pitches:movie.pitches});
                if(movie.lastFrame>1)await send({type:'control',handle:movie.workerHandle,control:'goto',args:[movie.lastFrame,0]});
                if(movie.loop)await send({type:'control',handle:movie.workerHandle,control:'loop',args:[movie.loop]});
                if(movie.paused)await send({type:'control',handle:movie.workerHandle,control:'pause',args:[1]});
                console.info('CP2077 Bink recycle: movie '+brokerHandle+' re-opened as '+movie.workerHandle+' at frame '+movie.lastFrame);
            }catch(error){
                movie.dead=true;console.error('CP2077 Bink recycle: movie '+brokerHandle+' could not be re-opened',error);
            }
        }
    }
    async function ensureReady() {
        if(!ready)ready=(async()=>{await initialize();if(recycle&&movies.size)await reopenAll();})().catch(error=>{fail(error);throw error;});
        await ready;
    }
    Module['cp2077BinkRequest']=async request=> {
        await ensureReady();
        const payload={...request};
        let transfers=[];
        if(payload.type==='open') {
            // Never transfer the engine's shared memory to the SDK worker.
            const bytes=Uint8Array.from(payload.bytes);
            if(recycle){
                const keep=bytes.slice(0); // broker copy for re-opening after a recycle
                payload.bytes=bytes.buffer;transfers=[bytes.buffer];
                const result=await send(payload,transfers);
                const brokerHandle=nextBrokerHandle++;
                movies.set(brokerHandle,{bytes:keep.buffer,flags:payload.flags,callerFrameBuffers:payload.callerFrameBuffers,pitches:null,lastFrame:0,loop:0,paused:0,workerHandle:result.handle,dead:false});
                return {...result,handle:brokerHandle};
            }
            payload.bytes=bytes.buffer;transfers=[bytes.buffer];
            return send(payload,transfers);
        }
        if(!recycle||payload.handle===undefined||payload.type==='freeGlobals')return send(payload,transfers);
        const brokerHandle=payload.handle,movie=movies.get(brokerHandle);
        if(!movie||movie.dead||!movie.workerHandle)throw new Error('Bink movie '+brokerHandle+' is not available');
        payload.handle=movie.workerHandle;
        if(payload.type==='registerFrameBuffers')movie.pitches=payload.pitches;
        if(payload.type==='control'&&payload.control==='loop')movie.loop=payload.args?.[0]||0;
        if(payload.type==='control'&&payload.control==='pause')movie.paused=payload.args?.[0]?1:0;
        if(payload.type==='control'&&payload.control==='goto')movie.lastFrame=payload.args?.[0]||movie.lastFrame;
        const result=await send(payload,transfers);
        if(payload.type==='frame'&&Number.isSafeInteger(result?.frame))movie.lastFrame=result.frame;
        if(payload.type==='close')movies.delete(brokerHandle);
        return result;
    };
})();
