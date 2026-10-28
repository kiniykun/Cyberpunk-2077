import {PredecodedBinkFrames} from './predecoded-bink-frames-web.mjs';
import {LosslessBrowserBinkFrames} from './lossless-browser-bink-frames-web.mjs';
const losslessAssets=new Set(["6b512aa130f035502cfa01ed0be5ef2b28fffe376a801595be4f2b7fd753ec4b","d45496cc518f731379b6835202d3203ad181e52d475497818f50b4e2888921bb",
    'f746e8be9baec8c5839f02c25f213b139f540551b212d7f86a59d275a9e7f066',
    'f395ddcabb51e5085c25af8ebba97b88a37f00085c112f5d6a784858c8e168b5',
    '2f4d1ca4110e3f82f7668e3fe11461ae3b9b59ecbcedb2a4d5eba8c64d6dc9c6'
]);
function unhex(text){if(!/^(?:[0-9a-f]{2})+$/.test(text))throw new Error('Invalid original metadata');return Uint8Array.from(text.match(/../g),x=>parseInt(x,16));}
// Browser video asset backend only. Original engine/widgets/animations stay callers.
// Playback time is real elapsed time; no game readiness flags or clocks change.
export class CachedBinkMovie {
    static async open(hash){
        const frames=losslessAssets.has(hash)
            ?await LosslessBrowserBinkFrames.open(hash):await PredecodedBinkFrames.open(hash);
        return new CachedBinkMovie(frames);
    }
    constructor(frames){
        this.frames=frames;this.manifest=frames.manifest;this.header=unhex(this.manifest.openHeaderHex);
        this.view=new DataView(this.header.buffer);
        if(this.header.length!==176 || this.view.getInt32(56,true)!==0)throw new Error('Cached backend requires original silent clip');
        this.rate=this.view.getUint32(20,true)/this.view.getUint32(24,true);
        if(!Number.isFinite(this.rate)||this.rate<=0)throw new Error('Invalid original video rate');
        this.startedAt=null;this.pausedAt=null;this.elapsedBeforeStart=0;this.decoded=0;this.decodeMs=0;this.pitches=null;this.closed=false;this.cycles=0;this.scheduleOrigin=null;
    }
    elapsed(){return this.startedAt===null?this.elapsedBeforeStart:this.elapsedBeforeStart+(this.pausedAt??performance.now())-this.startedAt;}
    begin(){if(this.startedAt===null){this.startedAt=performance.now();this.scheduleOrigin=this.startedAt;}}
    current(){return this.view.getUint32(12,true);}
    description(){
        if(!this.manifest.frameBufferDescription)throw new Error("Missing original frame buffer description");
        return structuredClone(this.manifest.frameBufferDescription);
    }
    register(pitches){
        if(this.pitches)throw new Error('Frame buffers already registered');
        const desc=this.description();if(pitches.length!==desc.frames)throw new Error('Original frame slot count mismatch');
        this.pitches=pitches.map((row,slot)=>row.map((pitch,plane)=>{
            const width=plane===1||plane===2?desc.chromaWidth:desc.yaWidth;
            if(row.length!==5 || !Number.isSafeInteger(pitch) || pitch<0 || (desc.slots[slot][plane].allocate && pitch<width))throw new Error('Invalid original plane pitch');return pitch;
        }));
    }
    async decode(){
        if(this.closed||!this.pitches)throw new Error('Original movie closed or buffers not registered');
        this.begin();const started=performance.now(),frame=this.current();
        const result=await this.frames.read(frame),row=this.manifest.frames[frame-1];
        const desc=this.description();
        for(const plane of result.planes){
            const targetPitch=this.pitches[result.slot][plane.plane];if(targetPitch===plane.pitch)continue;
            const height=plane.plane===1||plane.plane===2?desc.chromaHeight:desc.yaHeight;
            const width=plane.plane===1||plane.plane===2?desc.chromaWidth:desc.yaWidth;
            if(targetPitch*height>8*1024*1024)throw new Error('Original plane exceeds budget');
            const source=new Uint8Array(plane.bytes),target=new Uint8Array(targetPitch*height);
            for(let y=0;y<height;y++)target.set(source.subarray(y*plane.pitch,y*plane.pitch+width),y*targetPitch);
            plane.bytes=target.buffer;plane.pitch=targetPitch;
        }
        // Restore actual asset-frame output fields. Runtime time/controls are this
        // playback backend's state, rather than offline SDK timing statistics.
        const native=new DataView(unhex(row.headerHex).buffer);
        for(const offset of [16,28,40,44,48,52,60])this.view.setUint32(offset,native.getUint32(offset,true),true);
        this.decoded++;this.decodeMs+=performance.now()-started;
        if(this.decoded===1||this.decoded%60===0)console.log('CP2077 video decode progress: asset='+String(this.manifest.assetSHA256).slice(0,8)+' frame='+frame+' decoded='+this.decoded+' avgDecodeMs='+(this.decodeMs/this.decoded).toFixed(1));
        return {...result,header:this.header.slice().buffer,backend:this.frames.backend||'predecoded-original',decodedCacheBytes:this.frames.bytes};
    }
    next(){if(this.closed)throw new Error('Closed original movie');if(this.current()===this.manifest.totalFrames)++this.cycles;this.view.setUint32(12,this.current()%this.manifest.totalFrames+1,true);return {advanced:true,header:this.header.slice().buffer};}
    control(name,args){
        if(this.closed)throw new Error('Closed original movie');
        let value=0;
        if(name==='wait'){this.begin();value=this.pausedAt!==null || (this.decoded>0 && performance.now()-this.scheduleOrigin<(this.cycles*this.manifest.totalFrames+this.current()-(this.view.getUint32(16,true)===this.current()?0:1))*1000/this.rate)?1:0;}
        else if(name==='pause'){
            this.begin();const pause=!!args[0];if(pause&&this.pausedAt===null)this.pausedAt=performance.now();
            if(!pause&&this.pausedAt!==null){const pausedMs=performance.now()-this.pausedAt;this.startedAt+=pausedMs;this.scheduleOrigin+=pausedMs;this.pausedAt=null;}value=pause?1:0;
        }else if(name==='goto'){
            // Original renderBinkVideo loop-skip arithmetic (frame % Frames) can request frame 0; frames are 1-based, so
            // clamp to the movie like the SDK does instead of failing playback. Non-integers are still rejected.
            const requested=args[0];if(!Number.isSafeInteger(requested))throw new Error('Invalid original seek');
            const frame=Math.min(Math.max(requested,1),this.manifest.totalFrames);
            this.begin();this.cycles=0;this.view.setUint32(16,this.current(),true);this.view.setUint32(12,frame,true);this.scheduleOrigin=(this.pausedAt??performance.now())-(frame-1)*1000/this.rate;
        }else if(name==='shouldSkip'){
            // Original SDK BinkShouldSkip tests FrameNum+2 >= Frames and returns
            // zero for the tail. Preserve that boundary so the native engine
            // reaches its completion/loop callback rather than wrapping while late.
            value=this.current()+2<this.manifest.totalFrames && this.pausedAt===null && this.decoded>0 &&
                performance.now()-this.scheduleOrigin>(this.cycles*this.manifest.totalFrames+this.current())*1000/this.rate?1:0;
        }
        else if(name==='sound'){if(args[0]!==0&&args[0]!==1)throw new Error('Invalid sound state');value=0;}
        else if(name==='loop'){this.loop=!!args[0];}
        else if(name==='keyFrame'){
            let frame=args[0]||1;const flags=args[1];
            if(!Number.isSafeInteger(frame)||frame<1||frame>this.manifest.totalFrames||!Number.isSafeInteger(flags))throw new Error('Invalid original keyframe lookup');
            const table=new DataView(unhex(this.manifest.frameOffsetsHex).buffer);
            const key=n=>(table.getBigUint64((n-1)*8,true)&1n)!==0n;
            if(!(flags&128)&&key(frame))value=frame;
            else switch(flags&127){
                case 0:if(frame>1){value=frame-1;while(value>1&&!key(value))--value;}break;
                case 1:for(let n=frame+1;n<=this.manifest.totalFrames;n++)if(key(n)){value=n;break;}break;
                case 2:for(let distance=1;distance<this.manifest.totalFrames;distance++){
                    if(distance<frame&&key(frame-distance)){value=frame-distance;break;}
                    if(distance<=this.manifest.totalFrames-frame&&key(frame+distance)){value=frame+distance;break;}
                }break;
            }
        }
        else throw new Error('Cached original control not implemented: '+name);
        return {value:String(value),header:this.header.slice().buffer};
    }
    summary(){
        const data=new Uint8Array(144),view=new DataView(data.buffer);
        for(const [offset,value] of [[40,this.view.getUint32(0,true)],[44,this.view.getUint32(4,true)],[48,Math.floor(this.elapsed())],
            [52,this.view.getUint32(20,true)],[56,this.view.getUint32(24,true)],[60,this.view.getUint32(20,true)],
            [64,this.view.getUint32(24,true)],[72,this.manifest.totalFrames],[76,this.decoded],[100,Math.floor(this.decodeMs)]])view.setUint32(offset,value>>>0,true);
        return {summary:data.buffer};
    }
    close(){this.closed=true;this.frames.close?.();this.frames.cache.clear();this.frames.bytes=0;return {closed:true};}
}
