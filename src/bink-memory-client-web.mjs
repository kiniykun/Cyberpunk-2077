// Synchronous allocator callback channel, used only by the private CPU worker.
// The game's allocator owns the backing bytes; the emulated CPU keeps a cache
// of those bytes in its separate address space, synchronized at SDK boundaries.
export class BinkHostMemory {
    constructor(memory,channel) {
        if(!(memory?.buffer instanceof SharedArrayBuffer)||!(channel instanceof SharedArrayBuffer)||channel.byteLength!==32)
            throw new Error('Bink allocator requires shared engine memory and a bounded reply channel');
        this.memory=memory;this.control=new Int32Array(channel);this.reply=new BigUint64Array(channel);
        this.records=new Map();
    }
    request(operation,value) {
        Atomics.store(this.control,0,0);
        self.postMessage({kind:'binkMemory',operation,value:String(value)});
        if(Atomics.wait(this.control,0,0,30000)==='timed-out')throw new Error('Engine Bink allocator deadline exceeded');
        if(Atomics.load(this.control,1)!==1)throw new Error('Engine Bink allocator service failed');
        return this.reply[1];
    }
    reserve(size) {
        const root=this.request('allocate',size);
        if(root===0xffffffffffffffffn||root===0n)return null;
        const address=Number(root),heap=new Uint8Array(this.memory.buffer);
        if(!Number.isSafeInteger(address)||address<0||address>heap.length-size) {
            this.request('free',root);throw new Error('Engine Bink allocator returned an invalid backing span');
        }
        return {root,address,size};
    }
    attach(vm,pointer,record) {
        this.records.set(pointer,record);this.readIntoGuest(vm,pointer,record);
    }
    readIntoGuest(vm,pointer,record) {
        const heap=new Uint8Array(this.memory.buffer);
        for(let offset=0;offset<record.size;offset+=65536)
            vm.write(pointer+offset,heap.subarray(record.address+offset,record.address+Math.min(record.size,offset+65536)));
    }
    publish(vm,pointer) {
        const record=this.records.get(pointer);if(!record)return;
        const heap=new Uint8Array(this.memory.buffer);
        for(let offset=0;offset<record.size;offset+=65536)
            heap.set(vm.read(pointer+offset,Math.min(65536,record.size-offset)),record.address+offset);
    }
    release(vm,pointer) {
        const record=this.records.get(pointer);if(!record)return;
        this.publish(vm,pointer);this.request('free',record.root);this.records.delete(pointer);
    }
    beforeCall(vm) {for(const [pointer,record] of this.records)this.readIntoGuest(vm,pointer,record);}
    afterCall(vm) {for(const pointer of this.records.keys())this.publish(vm,pointer);}
    writeThrough(pointer,bytes) {
        const heap=new Uint8Array(this.memory.buffer);
        for(const [base,record] of this.records) {
            const first=Math.max(pointer,base),last=Math.min(pointer+bytes.length,base+record.size);
            if(first<last)heap.set(bytes.subarray(first-pointer,last-pointer),record.address+first-base);
        }
    }
}
