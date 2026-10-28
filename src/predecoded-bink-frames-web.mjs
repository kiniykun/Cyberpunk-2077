// [vidcache] user 2026-10-22: lifepath-card videos froze the game for 0.5-2.6 s per frame and ~7 s per open on a
// first visit (every gzip frame was a fresh no-store range request while the engine waited). Now:
//  - compressed frames are kept in Cache Storage ('cp2077-video-v1', absolute keys: workers run from blob: URLs);
//    gzip's CRC/length checks run on every decode, so a damaged cached frame is dropped and fetched again;
//  - while a movie plays, the next READAHEAD frames are fetched in the background;
//  - the manifest is cached too; the page may pre-warm the same keys (support.js, main menu).
const READAHEAD=12;
let VIDEO_CACHE=null;
const videoCache=()=>VIDEO_CACHE||(VIDEO_CACHE=(typeof caches==='object'&&caches?caches.open('cp2077-video-v1').catch(()=>null):Promise.resolve(null)));
const cacheKey=(hash,what)=>'https://video-cache.cyberpunk2077.lol/pre/'+hash+'/'+what;
async function cacheGet(key){try{const c=await videoCache();const r=c&&await c.match(key);return r?new Uint8Array(await r.arrayBuffer()):null;}catch(e){return null;}}
function cachePut(key,bytes){videoCache().then(c=>c&&c.put(key,new Response(bytes.slice(0),{headers:{'Content-Type':'application/octet-stream'}}))).catch(()=>{});}
async function gunzip(bytes,size){
    const reader=new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip')).getReader();
    const data=new Uint8Array(size);let position=0;
    try{for(;;){const {done,value}=await reader.read();if(done)break;
        if(position+value.length>data.length)throw new Error('Decoded original frame exceeds declared size');
        data.set(value,position);position+=value.length;
    }}catch(error){await reader.cancel(error).catch(()=>{});throw error;}finally{reader.releaseLock();}
    if(position!==data.length)throw new Error('Truncated original frame');
    return data;
}
export class PredecodedBinkFrames {
    constructor(base,manifest,budget=16*1024*1024,hash=''){
        if(manifest.totalFrames!==manifest.frames.length || budget<8*1024*1024)throw new Error('Invalid original frame manifest/budget');
        this.base=base;this.manifest=manifest;this.budget=budget;this.cache=new Map();this.bytes=0;this.hash=hash;
        this.packed=new Map();this.ahead=null;
    }
    static async open(hash){
        if(!/^[0-9a-f]{64}$/.test(hash))throw new Error('Invalid asset identity');
        const base='/runtime/codec/bink/predecoded/'+hash+'/';
        let manifest=null;const cached=await cacheGet(cacheKey(hash,'manifest.json'));
        if(cached){try{manifest=JSON.parse(new TextDecoder().decode(cached));if(manifest.assetSHA256!==hash)manifest=null;}catch(e){manifest=null;}}
        if(!manifest){
            const response=await fetch(base+'manifest.json');if(!response.ok)throw new Error('Original frame manifest missing');
            const bytes=new Uint8Array(await response.arrayBuffer());manifest=JSON.parse(new TextDecoder().decode(bytes));
            if(manifest.assetSHA256!==hash)throw new Error('Original frame identity mismatch');
            cachePut(cacheKey(hash,'manifest.json'),bytes);
        }
        return new PredecodedBinkFrames(base,manifest,16*1024*1024,hash);
    }
    span(frame){
        const {packOffset,compressedBytes,uncompressedBytes}=this.manifest.frames[frame-1];
        if(![packOffset,compressedBytes,uncompressedBytes].every(Number.isSafeInteger)||packOffset<0||compressedBytes<=0||uncompressedBytes<=0||uncompressedBytes>8*1024*1024)throw new Error('Invalid original frame span');
        return {packOffset,compressedBytes,uncompressedBytes};
    }
    // compressed bytes of one frame: memory (readahead) -> Cache Storage -> network (then stored)
    async packedFrame(frame,fromCache=true){
        const {packOffset,compressedBytes}=this.span(frame);
        const pending=this.packed.get(frame);if(pending){this.packed.delete(frame);const b=await pending.catch(()=>null);if(b&&b.length===compressedBytes)return {bytes:b,cached:false};}
        const key=cacheKey(this.hash,'f'+frame);
        if(fromCache&&this.hash){const b=await cacheGet(key);if(b&&b.length===compressedBytes)return {bytes:b,cached:true};}
        const response=await fetch(this.base+'frames.gzpack',{cache:'no-store',headers:{Range:`bytes=${packOffset}-${packOffset+compressedBytes-1}`}});
        if(response.status!==206 || response.headers.get('Content-Length')!==String(compressedBytes))throw new Error('Frame server did not honor exact range');
        const bytes=new Uint8Array(await response.arrayBuffer());
        if(bytes.length!==compressedBytes)throw new Error('Truncated original frame');
        if(this.hash)cachePut(key,bytes);
        return {bytes,cached:false};
    }
    readAhead(frame){
        if(this.ahead)return;
        this.ahead=(async()=>{
            for(let f=frame+1;f<=Math.min(this.manifest.totalFrames,frame+READAHEAD);f++){
                if(this.cache.has(f)||this.packed.has(f))continue;
                const key=cacheKey(this.hash,'f'+f);
                const c=await videoCache();const hit=c&&await c.match(key).catch(()=>null);if(hit)continue;
                const p=this.packedFrame(f,false).then(r=>r.bytes);p.catch(()=>{});this.packed.set(f,p);await p.catch(()=>{});
                while(this.packed.size>READAHEAD)this.packed.delete(this.packed.keys().next().value);
            }
        })().catch(()=>{}).finally(()=>{this.ahead=null;});
    }
    async read(frame){
        if(!Number.isSafeInteger(frame)||frame<1||frame>this.manifest.totalFrames)throw new Error('Invalid original frame');
        let data=this.cache.get(frame);
        const row=this.manifest.frames[frame-1];
        if(data){this.cache.delete(frame);this.cache.set(frame,data);}
        else{
            const {uncompressedBytes}=this.span(frame);
            let got=await this.packedFrame(frame);
            try{data=await gunzip(got.bytes,uncompressedBytes);}
            catch(error){if(!got.cached)throw error;got=await this.packedFrame(frame,false);data=await gunzip(got.bytes,uncompressedBytes);}   // damaged cached copy: refetch
            while(this.bytes+data.length>this.budget && this.cache.size){const key=this.cache.keys().next().value;this.bytes-=this.cache.get(key).length;this.cache.delete(key);}
            this.cache.set(frame,data);this.bytes+=data.length;
        }
        if(this.hash)this.readAhead(frame);
        return {frame,slot:row.slot,planes:row.planes.map(p=>{
            if(p.offset<0||p.bytes<=0||p.offset+p.bytes>data.length)throw new Error('Invalid original plane range');
            return {plane:p.plane,pitch:p.pitch,bytes:data.slice(p.offset,p.offset+p.bytes).buffer};
        }),frameSHA256:row.frameSHA256};
    }
}
