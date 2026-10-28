// Actual browser-side platform state for original SDK calls. Providers are
// implemented individually; unimplemented functions stay hard failures.
export function createBinkPlatform(vm,options={}) {
    const environment=new Map(Object.entries(options.environment||{}));
    const state={lastError:0,threadId:options.threadId||1};
    const handles=new Map(),namedMutexes=new Map();
    const kernelHandle=0x5000000,procedureCache=new Map();
    vm.engine.mem_map(kernelHandle,4096,vm.runtime.PROT_ALL);
    const write64=(pointer,value)=> {
        const bytes=new Uint8Array(8);new DataView(bytes.buffer).setBigUint64(0,BigInt.asUintN(64,value),true);vm.write(Number(pointer),bytes);
    };
    const cstring=pointer=> {
        const bytes=[];
        for(let i=0;i<4096;++i) {const value=vm.read(Number(pointer)+i,1)[0];if(!value)return new TextDecoder().decode(new Uint8Array(bytes));bytes.push(value);}
        throw new Error('Unterminated SDK environment variable name');
    };
    const providers={
        GetLastError:()=>state.lastError,
        SetLastError:args=>{state.lastError=Number(args[0])>>>0;return 0;},
        QueryPerformanceFrequency:args=> {
            if(!args[0]){state.lastError=87;return 0;}
            write64(args[0],1000000000n);return 1;
        },
        QueryPerformanceCounter:args=> {
            if(!args[0]){state.lastError=87;return 0;}
            write64(args[0],BigInt(Math.floor(performance.now()*1000000)));return 1;
        },
        GetTickCount:()=>Math.floor(performance.now())>>>0,
        timeGetTime:()=>Math.floor(performance.now())>>>0,
        GetCurrentThreadId:()=>state.threadId,
        CreateMutexA:args=> {
            if(args[0]){state.lastError=87;return 0;}
            const name=args[2]?cstring(args[2]):'';
            if(name&&namedMutexes.has(name)) {
                const handle=namedMutexes.get(name);handles.get(handle).references++;state.lastError=183;return handle;
            }
            const handle=vm.allocateNative(64);
            handles.set(handle,{kind:'mutex',name,owner:args[1]?state.threadId:null,depth:args[1]?1:0,references:1});
            if(name)namedMutexes.set(name,handle);state.lastError=0;return handle;
        },
        WaitForSingleObject:args=> {
            const handle=handles.get(Number(args[0]));
            if(!handle){state.lastError=6;return 0xffffffff;}
            if(handle.kind!=='mutex')throw new Error('SDK wait requires an unimplemented handle provider');
            if(handle.owner===null||handle.owner===state.threadId) {handle.owner=state.threadId;handle.depth++;return 0;}
            if(Number(args[1])===0)return 258;
            throw new Error('Contended SDK wait needs the guest thread scheduler');
        },
        ReleaseMutex:args=> {
            const handle=handles.get(Number(args[0]));
            if(!handle||handle.kind!=='mutex'){state.lastError=6;return 0;}
            if(handle.owner!==state.threadId||!handle.depth){state.lastError=288;return 0;}
            if(!--handle.depth)handle.owner=null;return 1;
        },
        CloseHandle:args=> {
            const pointer=Number(args[0]),handle=handles.get(pointer);
            if(!handle){state.lastError=6;return 0;}
            if(!--handle.references) {handles.delete(pointer);if(handle.name)namedMutexes.delete(handle.name);vm.freeNative(pointer);}
            return 1;
        },
        GetModuleHandleA:args=> {
            if(!args[0])return vm.base;
            const name=cstring(args[0]).toLowerCase();
            if(name==='bink2w64.dll'||name==='bink2w64')return vm.base;
            if(name==='kernel32.dll'||name==='kernel32')return kernelHandle;
            state.lastError=126;return 0;
        },
        GetProcAddress:args=> {
            if(args[1]<65536n){state.lastError=127;return 0;} // No ordinal provider is registered.
            const name=cstring(args[1]),handle=Number(args[0]);
            if(handle===vm.base) {const address=vm.exports.get(name);if(address)return address;}
            if(handle===kernelHandle&&providers[name]) {
                if(!procedureCache.has(name))procedureCache.set(name,vm.addCallback(callArgs=>providers[name](callArgs),'KERNEL32.dll!'+name));
                return procedureCache.get(name);
            }
            state.lastError=127;return 0;
        },
        GetEnvironmentVariableA:args=> {
            const name=cstring(args[0]),value=environment.get(name),pointer=Number(args[1]),capacity=Number(args[2]);
            if(value===undefined) {state.lastError=203;if(pointer&&capacity)vm.write(pointer,new Uint8Array([0]));return 0;}
            const bytes=new TextEncoder().encode(String(value));
            if(capacity<=bytes.length)return bytes.length+1;
            if(!pointer) {state.lastError=87;return 0;}
            const output=new Uint8Array(bytes.length+1);output.set(bytes);vm.write(pointer,output);state.lastError=0;return bytes.length;
        }
    };
    return {providers,state};
}
