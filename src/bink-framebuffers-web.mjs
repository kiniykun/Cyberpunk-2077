// Mirrors caller-owned BINKFRAMEBUFFERS in the private SDK address space.
// Engine pointers never cross into the emulated CPU. Plane bytes are copied
// back to the original engine's registered buffers by the engine boundary.
export class BinkFrameBuffers {
    constructor(vm,movie) {
        this.vm=vm;this.movie=movie;this.allocations=[];this.description=null;
        this.structure=vm.allocateNative(264);
        vm.write(this.structure,new Uint8Array(264));
        vm.call('BinkGetFrameBuffersInfo',[movie,this.structure]);
        const bytes=new Uint8Array(vm.read(this.structure,264)),view=new DataView(bytes.buffer);
        const frames=view.getInt32(0,true);
        if(frames<1||frames>3) {this.dispose();throw new Error('Invalid original Bink frame-buffer count');}
        this.description={frames,yaWidth:view.getUint32(4,true),yaHeight:view.getUint32(8,true),
            chromaWidth:view.getUint32(12,true),chromaHeight:view.getUint32(16,true),slots:[]};
        for(let slot=0;slot<frames;++slot) {
            const planes=[];
            for(let plane=0;plane<5;++plane) {
                const at=24+slot*80+plane*16;
                planes.push({allocate:view.getInt32(at+8,true)!==0});
            }
            if(planes[4].allocate) {this.dispose();throw new Error('Original renderer H-plane integration is not implemented');}
            this.description.slots.push(planes);
        }
    }
    register(pitches) {
        if(this.allocations.length)throw new Error('Bink frame buffers already registered');
        if(!Array.isArray(pitches)||pitches.length!==this.description.frames)throw new Error('Bink frame-buffer count mismatch');
        const bytes=new Uint8Array(this.vm.read(this.structure,264)),view=new DataView(bytes.buffer),staged=[];
        try {
            for(let slot=0;slot<this.description.frames;++slot) {
                if(pitches[slot].length!==5)throw new Error('Bink plane pitch count mismatch');
                for(let plane=0;plane<5;++plane) {
                    if(!this.description.slots[slot][plane].allocate)continue;
                    const chroma=plane===1||plane===2;
                    const width=chroma?this.description.chromaWidth:this.description.yaWidth;
                    const height=chroma?this.description.chromaHeight:this.description.yaHeight;
                    const pitch=pitches[slot][plane],size=pitch*height;
                    if(!Number.isSafeInteger(pitch)||pitch<width||!Number.isSafeInteger(size)||size<=0||size>8*1024*1024)
                        throw new Error('Invalid registered Bink plane dimensions');
                    const owned=this.vm.allocateNative(size+255),pointer=Math.ceil(owned/256)*256;
                    staged.push({owned,pointer,size,slot,plane,pitch});
                    this.vm.write(pointer,new Uint8Array(size));
                    const at=24+slot*80+plane*16;
                    view.setBigUint64(at,BigInt(pointer),true);view.setUint32(at+12,pitch,true);
                }
            }
            this.vm.write(this.structure,bytes);
            this.vm.call('BinkRegisterFrameBuffers',[this.movie,this.structure]);
            this.allocations=staged;
        }catch(error) {for(const plane of staged)this.vm.freeNative(plane.owned);throw error;}
    }
    readCurrent() {
        if(!this.allocations.length)throw new Error('Bink frame buffers have not been registered');
        const metadata=new DataView(new Uint8Array(this.vm.read(this.structure,24)).buffer);
        const slot=metadata.getUint32(20,true);
        if(slot>=this.description.frames)throw new Error('Original SDK returned an invalid current frame slot');
        const planes=this.allocations.filter(plane=>plane.slot===slot).map(plane=>({
            plane:plane.plane,pitch:plane.pitch,bytes:new Uint8Array(this.vm.read(plane.pointer,plane.size)).buffer
        }));
        return {slot,planes};
    }
    dispose() {
        // Call after BinkClose: original SDK may retain these buffers until close.
        for(const plane of this.allocations)this.vm.freeNative(plane.owned);
        this.allocations=[];
        if(this.structure) {this.vm.freeNative(this.structure);this.structure=0;}
    }
}
