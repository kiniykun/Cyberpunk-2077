// Client-side CPU execution boundary for original SDK bytes. No OS imports are
// fabricated here: unsupported calls/traps fail, and every call is bounded.
export class BinkSdkVm {
    constructor(runtime,image,options={}) {
        this.runtime=runtime;
        this.hostMemory=options.hostMemory||null;
        this.engine=new runtime.Unicorn(runtime.ARCH_X86,runtime.MODE_64);
        const bytes=image instanceof Uint8Array?image:new Uint8Array(image);
        const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);
        const u16=at=>view.getUint16(at,true),u32=at=>view.getUint32(at,true);
        const pe=u32(0x3c);
        if(u32(pe)!==0x4550||u16(pe+4)!==0x8664||u16(pe+24)!==0x20b)throw new Error('Expected original x64 PE SDK');
        const optional=pe+24,sectionTable=optional+u16(pe+20);
        this.base=Number(view.getBigUint64(optional+24,true));
        const size=u32(optional+56),headers=u32(optional+60);
        this.imageSize=Math.ceil(size/4096)*4096;
        if(!Number.isSafeInteger(this.base)||size>32*1024*1024||headers>bytes.length)throw new Error('Invalid SDK image span');
        this.engine.mem_map(this.base,Math.ceil(size/4096)*4096,runtime.PROT_ALL);
        this.engine.mem_write(this.base,bytes.subarray(0,headers));
        const sections=[];
        for(let i=0;i<u16(pe+6);++i) {
            const entry=sectionTable+i*40,rva=u32(entry+12),raw=u32(entry+16),offset=u32(entry+20);
            if(rva>size-raw||offset>bytes.length-raw)throw new Error('SDK section outside image');
            if(raw)this.engine.mem_write(this.base+rva,bytes.subarray(offset,offset+raw));
            sections.push({rva,offset,raw,virtual:u32(entry+8)});
        }
        const fileOffset=rva=> {
            if(rva<headers)return rva;
            const section=sections.find(s=>rva>=s.rva&&rva-s.rva<s.raw);
            if(!section)throw new Error('SDK table outside initialized sections');
            return section.offset+rva-section.rva;
        };
        const readString=rva=> {
            const start=fileOffset(rva);let end=start;
            while(end<bytes.length&&bytes[end]&&end-start<4096)++end;
            if(end===bytes.length||end-start===4096)throw new Error('Invalid SDK symbol name');
            return new TextDecoder().decode(bytes.subarray(start,end));
        };
        this.exports=new Map();
        this.imports=[];
        const importsRva=u32(optional+120);
        if(importsRva) {
            let descriptor=fileOffset(importsRva);
            while(u32(descriptor)||u32(descriptor+12)||u32(descriptor+16)) {
                const module=readString(u32(descriptor+12)),lookup=u32(descriptor)||u32(descriptor+16),iat=u32(descriptor+16);
                let index=0;
                for(;;++index) {
                    const entry=view.getBigUint64(fileOffset(lookup+index*8),true);if(!entry)break;
                    const name=entry&0x8000000000000000n?'#'+String(entry&0xffffn):readString(Number(entry)+2);
                    this.imports.push({module,name,iat:this.base+iat+index*8});
                }
                descriptor+=20;
            }
        }
        const exportRva=u32(optional+112),exportSize=u32(optional+116),directory=fileOffset(exportRva);
        const functions=u32(directory+20),names=u32(directory+24);
        const addresses=fileOffset(u32(directory+28)),nameTable=fileOffset(u32(directory+32)),ordinals=fileOffset(u32(directory+36));
        for(let i=0;i<names;++i) {
            const ordinal=u16(ordinals+i*2);if(ordinal>=functions)throw new Error('Invalid SDK export ordinal');
            const rva=u32(addresses+ordinal*4);
            if(rva>=exportRva&&rva<exportRva+exportSize)continue; // Forwarded OS exports need an actual provider.
            this.exports.set(readString(u32(nameTable+i*4)),this.base+rva);
        }
        this.data=options.dataBase||0x1000000;this.dataSize=options.dataBytes||16*1024*1024;
        if(!Number.isSafeInteger(this.data)||this.data%4096||!Number.isSafeInteger(this.dataSize)||
           this.dataSize%4096||this.dataSize<4*1024*1024||this.dataSize>256*1024*1024||
           this.data<0x1000000||this.data+this.dataSize>0x40000000||
           (this.data<0x6002000&&this.data+this.dataSize>0x4000000))throw new Error('Invalid private SDK memory layout');
        this.cursor=this.data;this.stack=this.data+this.dataSize-0x10008;
        this.returnAddress=this.data+this.dataSize-4096;
        // Windows x64 compiler stack probes read the guest NT_TIB via GS. Back
        // that descriptor with the actual mapped guest stack, including a real
        // inaccessible guard page, rather than patching out the probe routine.
        this.stackLimit=Math.floor((this.stack-0x100000)/4096)*4096;
        this.heapLimit=this.stackLimit-4096;
        // Map heap and stack separately, leaving the guard genuinely unmapped.
        // Avoid splitting a live RAM mapping through CPU-runtime mem_protect.
        this.engine.mem_map(this.data,this.heapLimit-this.data,runtime.PROT_ALL);
        this.engine.mem_map(this.stackLimit,this.data+this.dataSize-this.stackLimit,runtime.PROT_ALL);
        this.teb=0x6000000;this.engine.mem_map(this.teb,8192,runtime.PROT_ALL);
        const tib=new Uint8Array(80),tibView=new DataView(tib.buffer);
        tibView.setBigUint64(8,BigInt(this.stack+8),true);
        tibView.setBigUint64(16,BigInt(this.stackLimit),true);
        tibView.setBigUint64(0x30,BigInt(this.teb),true);
        tibView.setBigUint64(0x40,1n,true);tibView.setBigUint64(0x48,1n,true);
        this.engine.mem_write(this.teb,tib);this.engine.reg_write_i64(runtime.X86_REG_GS_BASE,BigInt(this.teb));
        this.nativeAllocations=new Map();this.freedBlocks=[];
        this.callbacks=new Map();this.callbackLabels=new Map();this.callbackBase=0x4000000;this.callbackNext=this.callbackBase;
        this.engine.mem_map(this.callbackBase,4096,runtime.PROT_ALL);
        this.lastFailure=null;
        this.lastMemoryFault=null;
        this.recentBlocks=[];
        if(options.traceBlocks !== false) this.engine.hook_add(runtime.HOOK_BLOCK,(engine,address,size)=> {
            const rsp=engine.reg_read_i64(runtime.X86_REG_RSP);
            let returnTarget;
            if(size===1&&engine.mem_read(Number(address),1)[0]===0xc3) {
                const bytes=new Uint8Array(engine.mem_read(Number(rsp),8));returnTarget=String(new DataView(bytes.buffer).getBigUint64(0,true));
            }
            this.recentBlocks.push({address:String(address),size,rsp:String(rsp),rax:String(engine.reg_read_i64(runtime.X86_REG_RAX)),returnTarget});if(this.recentBlocks.length>16)this.recentBlocks.shift();
        });
        this.engine.hook_add(runtime.HOOK_MEM_READ_UNMAPPED|runtime.HOOK_MEM_WRITE_UNMAPPED|runtime.HOOK_MEM_FETCH_UNMAPPED|
            runtime.HOOK_MEM_READ_PROT|runtime.HOOK_MEM_WRITE_PROT|runtime.HOOK_MEM_FETCH_PROT,
            (engine,type,address,size)=> {
                this.lastMemoryFault={type,address:String(address),size,pc:String(engine.reg_read_i64(runtime.X86_REG_RIP))};
                return false;
            });
        this.engine.hook_add(runtime.HOOK_CODE,(engine,address)=> {
            const callback=this.callbacks.get(Number(address));if(!callback)return;
            try {
                const registers=[runtime.X86_REG_RCX,runtime.X86_REG_RDX,runtime.X86_REG_R8,runtime.X86_REG_R9];
                const args=registers.map(reg=>BigInt(engine.reg_read_i64(reg)));
                const result=callback(args);
                const stack=Number(engine.reg_read_i64(runtime.X86_REG_RSP));
                const saved=new Uint8Array(engine.mem_read(stack,8));
                const target=new DataView(saved.buffer).getBigUint64(0,true);
                engine.reg_write_i64(runtime.X86_REG_RAX,BigInt(result||0));
                engine.reg_write_i64(runtime.X86_REG_RSP,BigInt(stack+8));
                engine.reg_write_i64(runtime.X86_REG_RIP,target);
            }catch(error){this.lastFailure=error;engine.emu_stop();}
        },{},this.callbackBase,this.callbackBase+4095);
    }
    addCallback(callback,label='unlabelled') {
        const address=this.callbackNext;if(address>this.callbackBase+4080)throw new Error('SDK callback table exhausted');
        this.callbackNext+=16;this.engine.mem_write(address,new Uint8Array([0xc3]));this.callbacks.set(address,callback);this.callbackLabels.set(address,label);return address;
    }
    bindImports(providers={}) {
        for(const imported of this.imports) {
            const callback=this.addCallback(args=> {
                const provider=providers[imported.module+'!'+imported.name]||providers[imported.name];
                if(!provider)throw new Error('Unimplemented SDK platform import: '+imported.module+'!'+imported.name);
                return provider(args,this);
            },imported.module+'!'+imported.name);
            const pointer=new Uint8Array(8);new DataView(pointer.buffer).setBigUint64(0,BigInt(callback),true);
            this.engine.mem_write(imported.iat,pointer);
        }
    }
    allocateNative(length,sdkCall=false) {
        const size=Number(length);if(!Number.isSafeInteger(size)||size<0)throw new Error('Invalid native SDK allocation size');
        const backing=this.hostMemory?this.hostMemory.reserve(Math.max(size,16)):null;
        if(this.hostMemory&&!backing) {
            if(sdkCall)return -1; // Original SDK allocator's explicit no-fallback failure sentinel.
            throw new Error('Engine Bink allocator exhausted');
        }
        let block;
        try {
            const span=Math.ceil(Math.max(size,16)/32)*32;
            block=this.takeFreeBlock(span)||{pointer:this.allocate(span),length:span};
            this.nativeAllocations.set(block.pointer,block.length);
            if(backing)this.hostMemory.attach(this,block.pointer,backing);
            return block.pointer;
        }catch(error) {
            if(backing)this.hostMemory.request('free',backing.root);
            if(block) {
                this.nativeAllocations.delete(block.pointer);this.releaseBlock(block);
                this.hostMemory?.records.delete(block.pointer);
            }
            if(sdkCall)return -1;
            throw error;
        }
    }
    freeNative(pointer) {
        const address=Number(pointer);if(!address)return;
        const size=this.nativeAllocations.get(address);if(size===undefined)throw new Error('SDK freed an unknown allocation');
        if(this.hostMemory)this.hostMemory.release(this,address);
        this.nativeAllocations.delete(address);this.releaseBlock({pointer:address,length:size});
    }
    // Free list kept sorted by address: best-fit with splitting, neighbours
    // coalesce on release, and a free tail rewinds the bump cursor. Without
    // this, movies with differing frame sizes fragment the private heap until
    // allocation fails ("Private SDK memory exhausted").
    takeFreeBlock(need) {
        const length=Math.ceil(need/32)*32,list=this.freedBlocks;let best=-1;
        for(let i=0;i<list.length;++i)
            if(list[i].length>=length&&(best<0||list[i].length<list[best].length)) {best=i;if(list[i].length===length)break;}
        if(best<0)return null;
        const block=list[best];
        if(block.length-length>=64) {
            list[best]={pointer:block.pointer+length,length:block.length-length};
            return {pointer:block.pointer,length};
        }
        list.splice(best,1);return block;
    }
    releaseBlock(block) {
        const list=this.freedBlocks;let lo=0,hi=list.length;
        while(lo<hi) {const mid=(lo+hi)>>1;if(list[mid].pointer<block.pointer)lo=mid+1;else hi=mid;}
        let merged={pointer:block.pointer,length:block.length},at=lo;
        if(at<list.length&&merged.pointer+merged.length===list[at].pointer) {merged.length+=list[at].length;list.splice(at,1);}
        if(at>0&&list[at-1].pointer+list[at-1].length===merged.pointer) {--at;merged={pointer:list[at].pointer,length:list[at].length+merged.length};list.splice(at,1);}
        if(merged.pointer+merged.length===this.cursor)this.cursor=merged.pointer;
        else list.splice(at,0,merged);
    }
    installMemoryCallbacks() {
        const allocate=this.addCallback(args=>this.allocateNative(args[0],true),'SdkAllocate');
        const release=this.addCallback(args=>{this.freeNative(args[0]);return 0;},'SdkFree');
        this.call('BinkSetMemory',[allocate,release]);
    }
    allocate(length) {
        if(!Number.isSafeInteger(length)||length<0)throw new Error('Invalid SDK allocation');
        const alignment=this.hostMemory?32:16;
        const pointer=Math.ceil(this.cursor/alignment)*alignment;if(pointer>this.cursor&&this.cursor>this.data)this.releaseBlock({pointer:this.cursor,length:pointer-this.cursor});
        if(pointer>this.heapLimit-length)throw new Error('Private SDK memory exhausted');
        this.cursor=pointer+length;return pointer;
    }
    write(pointer,bytes) {
        this.engine.mem_write(pointer,bytes);
        if(this.hostMemory)this.hostMemory.writeThrough(pointer,bytes instanceof Uint8Array?bytes:new Uint8Array(bytes));
    }
    read(pointer,length) {return this.engine.mem_read(pointer,length);}
    call(name,args=[],instructions=1000000,singleStep=false) {
        const entry=this.exports.get(name);if(!entry)throw new Error('Missing original SDK export '+name);
        if(args.length>12)throw new Error('SDK call exceeds bounded Microsoft x64 argument support');
        const r=this.runtime,e=this.engine;
        this.lastFailure=null;
        if(this.hostMemory)this.hostMemory.beforeCall(this);
        const savedReturn=new Uint8Array(8);new DataView(savedReturn.buffer).setBigUint64(0,BigInt(this.returnAddress),true);
        e.mem_write(this.stack,savedReturn);e.reg_write_i64(r.X86_REG_RSP,BigInt(this.stack));
        // Microsoft x64: return address, 32-byte caller shadow space, then
        // stack arguments in eight-byte slots. The guest stack stays separate
        // from engine memory and retains its guard page.
        if(args.length>4) {
            const extra=new Uint8Array((args.length-4)*8),view=new DataView(extra.buffer);
            for(let i=4;i<args.length;++i)view.setBigUint64((i-4)*8,BigInt.asUintN(64,BigInt(args[i]||0)),true);
            e.mem_write(this.stack+40,extra);
        }
        const registers=[r.X86_REG_RCX,r.X86_REG_RDX,r.X86_REG_R8,r.X86_REG_R9];
        for(let i=0;i<4;++i)e.reg_write_i64(registers[i],BigInt(args[i]||0));
        if(singleStep) {
            let pc=BigInt(entry),steps=0;
            while(pc!==BigInt(this.returnAddress)&&steps++<instructions) {
                e.emu_start(Number(pc),this.returnAddress,0,1);
                if(this.lastFailure)throw this.lastFailure;
                pc=BigInt(e.reg_read_i64(r.X86_REG_RIP));
            }
        } else e.emu_start(entry,this.returnAddress,0,instructions);
        if(this.lastFailure)throw this.lastFailure;
        if(BigInt(e.reg_read_i64(r.X86_REG_RIP))!==BigInt(this.returnAddress))throw new Error('Original SDK call exceeded instruction budget');
        if(this.hostMemory)this.hostMemory.afterCall(this);
        return BigInt(e.reg_read_i64(r.X86_REG_RAX));
    }
    close() {this.engine.close();}
}
