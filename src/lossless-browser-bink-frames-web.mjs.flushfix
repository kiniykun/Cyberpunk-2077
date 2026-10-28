// Decode offline-prepared original video planes using the browser codec.
// Native Bink headers, frames, pitches and caller controls remain authoritative.
export class LosslessBrowserBinkFrames {
    static async open(hash,{requireVerified=true}={}){
        if(!/^[0-9a-f]{64}$/.test(hash))throw new Error('Invalid original asset identity');
        const base='/runtime/codec/bink/lossless/'+hash+'/';
        const response=await fetch(base+'manifest.json');if(!response.ok)throw new Error('Prepared browser video missing');
        const video=await response.json();
        if(video.sourceAssetSHA256!==hash || !video.lossless || (requireVerified&&!video.browserVerified))throw new Error('Original browser video identity/verification mismatch');
        const nativeResponse=await fetch('/runtime/codec/bink/predecoded/'+hash+'/manifest.json');if(!nativeResponse.ok)throw new Error('Original video metadata missing');
        const manifest=await nativeResponse.json();
        if(manifest.assetSHA256!==hash || video.packets.length!==manifest.totalFrames || video.rows.length!==manifest.totalFrames)throw new Error('Original video frame count mismatch');
        const config={codec:'vp09.00.10.08',codedWidth:manifest.dimensions.yaWidth,codedHeight:manifest.dimensions.yaHeight,hardwareAcceleration:'prefer-software'};
        if(typeof VideoDecoder==='undefined'||!(await VideoDecoder.isConfigSupported(config)).supported)throw new Error('Browser lossless video decoder unavailable');
        return new LosslessBrowserBinkFrames(base,manifest,video,config);
    }
    constructor(base,manifest,video,config){
        this.base=base;this.manifest=manifest;this.video=video;this.config=config;
        this.backend='browser-lossless-original';
        this.cache=new Map();this.bytes=0;this.budget=16*1024*1024;this.next=0;this.pending=new Map();this.closed=false;this.failure=null;
        this.serial=Promise.resolve();this.timestampFrames=new Map(video.packets.map((p,i)=>[p.timestamp,i+1]));
        this.decoder=new VideoDecoder({output:frame=>this.receive(frame),error:error=>this.fail(error)});
        this.decoder.configure(config);
    }
    fail(error){this.failure=error;for(const p of this.pending.values())p.reject(new Error(String(error)+' at original frame '+(this.next+1)+' ('+this.config.codedWidth+'x'+this.config.codedHeight+')'));this.pending.clear();}
    async receive(frame){
        const number=this.timestampFrames.get(frame.timestamp),waiter=this.pending.get(number);
        try{
            if(!number||!waiter)throw new Error('Unexpected original video output');
            if(frame.format!=='I420')throw new Error('Original video planes require I420');
            const tight=new Uint8Array(frame.allocationSize()),layout=await frame.copyTo(tight);
            const row=this.manifest.frames[number-1],planes=[];let bytes=0;
            for(const p of row.planes){
                if(p.plane>2)throw new Error('Prepared video alpha plane requires explicit support');
                const width=p.plane?this.manifest.dimensions.chromaWidth:this.manifest.dimensions.yaWidth;
                const height=p.plane?this.manifest.dimensions.chromaHeight:this.manifest.dimensions.yaHeight;
                if(!layout[p.plane]||p.pitch<width||p.bytes!==p.pitch*height)throw new Error('Original plane layout mismatch');
                const output=new Uint8Array(p.bytes);
                for(let y=0;y<height;y++){
                    const at=layout[p.plane].offset+y*layout[p.plane].stride;
                    output.set(tight.subarray(at,at+width),y*p.pitch);
                }
                bytes+=output.length;planes.push({plane:p.plane,pitch:p.pitch,bytes:output.buffer});
            }
            if(bytes>this.budget)throw new Error('Original frame exceeds bounded decoder cache');
            while(this.bytes+bytes>this.budget&&this.cache.size){const key=this.cache.keys().next().value;this.bytes-=this.cache.get(key).size;this.cache.delete(key);}
            this.cache.set(number,{planes,size:bytes});this.bytes+=bytes;
            this.pending.delete(number);waiter.resolve();
        }catch(error){this.fail(error);}finally{frame.close();}
    }
    async packet(index){
        const p=this.video.packets[index];
        if(!p||!Number.isSafeInteger(p.offset)||!Number.isSafeInteger(p.bytes)||p.bytes<=0||p.bytes>8*1024*1024)throw new Error('Invalid browser video packet');
        // no-store: Chrome's sparse range cache can mix stale ranges of an earlier frames.ivf into later reads,
        // which surfaced as a deterministic VP9 decode error at original frame 331 of the intro.
        const response=await fetch(this.base+'frames.ivf',{cache:'no-store',headers:{Range:`bytes=${p.offset}-${p.offset+p.bytes-1}`}});
        if(response.status!==206||response.headers.get('Content-Length')!==String(p.bytes))throw new Error('Browser video packet range mismatch');
        return {p,data:await response.arrayBuffer()};
    }
    read(frame){
        const operation=this.serial.then(()=>this.readSerial(frame));this.serial=operation.catch(()=>{});return operation;
    }
    async readSerial(frame){
        if(this.closed||this.failure)throw this.failure||new Error('Original video closed');
        if(!Number.isSafeInteger(frame)||frame<1||frame>this.manifest.totalFrames)throw new Error('Invalid original video frame');
        if(!this.cache.has(frame)){
            const target=frame-1;
            if(this.next>target||this.next===0||this.needKey){this.needKey=false;
                let key=target;while(key>0&&!this.video.packets[key].key)key--;
                if(!this.video.packets[key].key)throw new Error('Original browser video keyframe missing');
                this.decoder.reset();this.decoder.configure(this.config);this.next=key;
            }
            let flushes=0;
            while(this.next<=target){
                const index=this.next,{p,data}=await this.packet(index);
                const ready=new Promise((resolve,reject)=>this.pending.set(index+1,{resolve,reject}));
                this.decoder.decode(new EncodedVideoChunk({type:p.key?'key':'delta',timestamp:p.timestamp,data}));
                // Completion is actual decoded output, never an invented frame.
                // [flushfix] user 2026-10-22: new players' videos froze 30 s (Bink watchdog) at a fixed frame: the decoder
                // held that frame's output back. If it is not out within 1.5 s, flush() (forces every pending output);
                // after a flush the decoder needs a key frame, so the next miss re-decodes from the previous key frame.
                const got=await Promise.race([ready.then(()=>true),new Promise(r=>setTimeout(()=>r(false),1500))]);
                if(!got){
                    if(++flushes>2)throw new Error('Original video frame stalled repeatedly');   // let the broker recycle
                    this.needKey=true;
                    try{await this.decoder.flush();}catch(e){}
                    await Promise.race([ready,new Promise((_,j)=>setTimeout(()=>j(new Error('Original video frame stalled after flush')),3000))]);
                }
                this.next=index+1;
                if(this.needKey&&this.next<=target){let key=target;while(key>0&&!this.video.packets[key].key)key--;this.decoder.reset();this.decoder.configure(this.config);this.next=key;this.needKey=false;}   // flushed mid-run: restart from a key frame
            }
        }
        const entry=this.cache.get(frame);if(!entry)throw new Error('Original decoded frame unavailable');
        this.cache.delete(frame);this.cache.set(frame,entry);
        return {frame,slot:this.manifest.frames[frame-1].slot,planes:entry.planes.map(p=>({...p,bytes:p.bytes.slice(0)})),frameSHA256:this.manifest.frames[frame-1].frameSHA256};
    }
    close(){this.closed=true;if(this.decoder.state!=='closed')this.decoder.close();this.fail(new Error('Original video closed'));this.cache.clear();this.bytes=0;}
}
