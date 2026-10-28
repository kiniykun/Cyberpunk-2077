// No shared engine memory enters this worker. At most two private decoder
// workers execute; each owns a private Wasm instance, reused until any failure (see the worker).
const queue=[],slots=[];
// [poolsize] 2026-10-23: the 1024-entry queue overflowed in on-demand load bursts (about 1000 multilayer setups at once
// since the cooked mlsetup blob is skipped) and each overflow failed a decode = engine Kraken assertion. Bigger queue,
// and up to 4 decoders on machines with the cores for it (all Kraken decompression of the game runs through here).
const MAX_QUEUE=8192,MAX_WORKERS=Math.max(2,Math.min(4,Math.floor((self.navigator?.hardwareConcurrency||4)/4)));
const stats={active:0,queued:0,finished:0,failed:0,peakActive:0,peakQueued:0,workersStarted:0,
    // [compress] decode timing (ms) and bytes; waitMs = enqueue -> reply (includes queueing).
    decodes:0,bytesIn:0,bytesOut:0,instMs:0,decodeMs:0,workerMs:0,waitMs:0,maxWaitMs:0};
let stopped=false;
function report(){stats.queued=queue.length;stats.peakQueued=Math.max(stats.peakQueued,queue.length);self.postMessage({kind:'stats',stats:{...stats}});}
function fatal(error){
    if(stopped)return;stopped=true;
    for(const slot of slots){clearTimeout(slot.timer);slot.worker.terminate();}
    self.postMessage({fatal:true,error:String(error?.message||error)});
}
let initFailures=0;
function retire(slot,error){
    if(stopped||slot.dead)return;
    clearTimeout(slot.timer);slot.worker.terminate();slot.dead=true;
    // [oodlefix] 2026-10-24: a decoder worker that failed to START (its module/wasm fetch dropped) used to be fatal for
    // the whole session: every later decode failed -> engine Kraken assertion (~1500 reports/day). Respawn with backoff.
    if(!slot.ready){if(++initFailures>6){fatal(error);return;}setTimeout(pump,500*initFailures);return;}
    if(slot.job){stats.active--;stats.failed++;self.postMessage({id:slot.job.id,ok:false,error:String(error?.message||error)});slot.job=null;}
    report();pump();
}
function spawn(){
    const worker=new Worker(new URL('./oodle-decoder-pool-worker.mjs',import.meta.url),{type:'module'});
    const slot={worker,ready:false,job:null,dead:false,timer:null};slots.push(slot);stats.workersStarted++;
    slot.timer=setTimeout(()=>retire(slot,new Error('Oodle worker initialization deadline exceeded')),60000);
    worker.onerror=e=>retire(slot,new Error(e.message||'Oodle decoder worker failed'));
    worker.onmessageerror=()=>retire(slot,new Error('Oodle decoder reply could not be read'));
    worker.onmessage=({data})=>{
        if(stopped||slot.dead)return;
        if(data?.kind==='ready'&&!slot.ready&&!slot.job){clearTimeout(slot.timer);slot.ready=true;pump();return;}
        if(!slot.job||data?.id!==slot.job.id){retire(slot,new Error('Unexpected Oodle decoder response'));return;}
        clearTimeout(slot.timer);slot.lastJob=slot.job;slot.job=null;stats.active--;
        if(data.ok)stats.finished++;else stats.failed++;
        if(data.ok&&data.timing&&slot.lastJob?.operation==='decompress'){
            const t=data.timing,wait=performance.now()-slot.lastJob.queuedAt;
            stats.decodes++;stats.bytesIn+=slot.lastJob.compressedBytes;stats.bytesOut+=data.output.byteLength;
            stats.instMs+=t.instMs;stats.decodeMs+=t.decodeMs;stats.workerMs+=t.totalMs;stats.waitMs+=wait;stats.maxWaitMs=Math.max(stats.maxWaitMs,wait);
        }
        delete data.timing;
        self.postMessage(data,data.output instanceof ArrayBuffer?[data.output]:[]);report();pump();
    };
    return slot;
}
function pump(){
    if(stopped)return;
    try{
        while(queue.length&&slots.filter(s=>!s.dead).length<MAX_WORKERS)spawn();
        for(const slot of slots){
            if(slot.dead||!slot.ready||slot.job||!queue.length)continue;
            const job=queue.shift();slot.job=job;stats.active++;stats.peakActive=Math.max(stats.peakActive,stats.active);
            slot.timer=setTimeout(()=>retire(slot,new Error('Oodle codec execution deadline exceeded')),job.operation==='compress'?120000:15000);
            const input=job.operation==='compress'?job.raw:job.compressed;
            job.compressedBytes=input?.byteLength||0;
            try{slot.worker.postMessage(job,[input]);}catch(error){retire(slot,error);}
        }
        report();
    }catch(error){fatal(error);}
}
self.onmessage=({data})=>{
    if(stopped)return;
    if(!Number.isSafeInteger(data?.id)||data.id<=0||!['compress','decompress'].includes(data.operation)){
        self.postMessage({id:data?.id,ok:false,error:'Invalid Oodle pool request'});return;
    }
    if(queue.length>=MAX_QUEUE){self.postMessage({id:data.id,ok:false,error:'Oodle queue capacity exceeded'});return;}
    data.queuedAt=performance.now();queue.push(data);pump();
};
