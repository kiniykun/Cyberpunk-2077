/* Private original-SDK execution worker. Frames are consumed by the engine;
 * this is not an alternate player, scene viewer, or replacement game UI. */
let vm, runtime, FrameBuffers, CachedMovie, faulted=false;
let initialized=false,initialization;
const preparedSilentAssets=new Set(["073a32b10641d5c9e74cfdbeff0d006724f88b7d2474f0b52e6cfac4af38af24","6b512aa130f035502cfa01ed0be5ef2b28fffe376a801595be4f2b7fd753ec4b","b04fc828038e4bbe1a9b269bed1b8eea533b93519bf670367ded62186e046d6f","d45496cc518f731379b6835202d3203ad181e52d475497818f50b4e2888921bb","e2d45bc245a1f1fb62e62f61674da70250c3fdb34338d47b076bed823190e17b","354a22c09643bc38e2e469c67c94deb475a9f26e21812805d8109f2460fb5776","440d4599167aabec4b8ecca517a990694ec8b3a4c616f6f95f2dbf27d86651d7","f746e8be9baec8c5839f02c25f213b139f540551b212d7f86a59d275a9e7f066","f395ddcabb51e5085c25af8ebba97b88a37f00085c112f5d6a784858c8e168b5","2f4d1ca4110e3f82f7668e3fe11461ae3b9b59ecbcedb2a4d5eba8c64d6dc9c6"]);
const movies=new Map();
let nextHandle=1;
let serial=Promise.resolve();
async function ensureSdkVm() {
    if(vm)return;
    const request=initialization;
    importScripts(request.runtimeUrl);
    runtime=await MUnicorn();
    const [{BinkSdkVm},{createBinkPlatform},{BinkFrameBuffers},{BinkHostMemory}]=await Promise.all([
        import(request.vmUrl),import(request.platformUrl),import(request.frameBuffersUrl),import(request.memoryClientUrl)
    ]);
    FrameBuffers=BinkFrameBuffers;
    const hostMemory=new BinkHostMemory(request.engineMemory,request.memoryChannel);
    const nextVm=new BinkSdkVm(runtime,new Uint8Array(request.sdkBytes),{dataBase:0x10000000,dataBytes:128*1024*1024,hostMemory,traceBlocks:false});
    nextVm.bindImports(createBinkPlatform(nextVm).providers);
    nextVm.installMemoryCallbacks();
    vm=nextVm;
    // The original image is now owned by the SDK VM; release the input copy.
    initialization=null;
}
async function dispatch(request) {
    if(faulted)throw new Error('Bink SDK worker is terminal and cannot accept more operations');
    if(request.type==='initialize') {
        if(initialized)throw new Error('Bink worker already initialized');
        CachedMovie=(await import("./cached-bink-movie-web.mjs")).CachedBinkMovie;
        initialization=request;initialized=true;
        return {ready:true};
    }
    if(!initialized)throw new Error('Bink worker is not initialized');
    if(request.type==='freeGlobals') {
        if(movies.size)throw new Error('Cannot free Bink globals while movies are open');
        if(vm)vm.call('BinkFreeGlobals');return {released:true};
    }
    if(request.type==='open') {
        const bytes=new Uint8Array(request.bytes);
        if(bytes.length===0||bytes.length>64*1024*1024)throw new Error('Clip exceeds current private SDK heap budget');
        const flags=request.flags||0;
        const supportedFlags=0x00100000|0x00080000|0x80|0x40|0x08|0x03;
        if(!Number.isSafeInteger(flags)||flags<0||(flags&~supportedFlags)!==0)
            throw new Error('Bink open flags require an unsupported SDK integration path');
        const digest=await crypto.subtle.digest('SHA-256',bytes);
        const hash=Array.from(new Uint8Array(digest),x=>x.toString(16).padStart(2,'0')).join('');
        if(request.callerFrameBuffers){
            if(preparedSilentAssets.has(hash)){
                const cached=await CachedMovie.open(hash),handle=nextHandle++;
                movies.set(handle,{cached,assetSHA256:hash});
                const info=cached.view;
                console.info('CP2077 original video cache probe: asset='+hash+' frames='+cached.manifest.totalFrames+' budgetBytes='+cached.frames.budget);
                return {handle,width:info.getUint32(0,true),height:info.getUint32(4,true),frames:info.getUint32(8,true),
                    rate:info.getUint32(20,true),rateDiv:info.getUint32(24,true),header:cached.header.slice().buffer,frameBuffers:cached.description(),backend:cached.frames.backend||'predecoded-original',assetSHA256:hash};
            }
        }
        await ensureSdkVm();
        const buffer=vm.allocateNative(bytes.length+64);
        vm.write(buffer,bytes);
        let movie;
        try { movie=vm.call('BinkOpenWithOptions',[buffer,0,0x04000000|flags|(request.callerFrameBuffers?0x400:0)],10000000); }
        catch(error) {vm.freeNative(buffer);throw error;}
        if(!movie) {vm.freeNative(buffer);throw new Error('Original SDK rejected the clip');}
        const header=new DataView(new Uint8Array(vm.read(Number(movie),32)).buffer);
        const width=header.getUint32(0,true),height=header.getUint32(4,true);
        const length=width*height*4;
        if(!width||!height||!Number.isSafeInteger(length)||length>8*1024*1024) {
            vm.call('BinkClose',[movie]);vm.freeNative(buffer);throw new Error('Frame exceeds current private SDK heap budget');
        }
        let output=0,frameBuffers=null;
        try {
            if(request.callerFrameBuffers)frameBuffers=new FrameBuffers(vm,movie);
            else output=vm.allocateNative(length);
        }catch(error) {vm.call('BinkClose',[movie]);vm.freeNative(buffer);throw error;}
        const handle=nextHandle++;
        movies.set(handle,{movie,buffer,output,frameBuffers,width,height,length,assetSHA256:hash});
        console.info('CP2077 original SDK video probe: asset='+hash+' handle='+handle+' bytes='+bytes.length+' width='+width+' height='+height);
        return {handle,width,height,frames:header.getUint32(8,true),rate:header.getUint32(20,true),rateDiv:header.getUint32(24,true),
            header:new Uint8Array(vm.read(Number(movie),176)).buffer,
            frameBuffers:frameBuffers?.description,backend:'original-sdk-vm',assetSHA256:hash};
    }
    const movie=movies.get(request.handle);
    if(!movie)throw new Error('Unknown Bink movie handle');
    if(movie.cached){
        const cached=movie.cached;
        if(request.type==='frameOffsets')return {offsets:Uint8Array.from(cached.manifest.frameOffsetsHex.match(/../g),x=>parseInt(x,16)).buffer};
        if(request.type==='control')return cached.control(request.control,request.args||[]);
        if(request.type==='summary')return cached.summary();
        if(request.type==='registerFrameBuffers'){cached.register(request.pitches);return {registered:true};}
        if(request.type==='frame')return {...await cached.decode(),assetSHA256:movie.assetSHA256};
        if(request.type==='next')return cached.next();
        if(request.type==='close'){const result=cached.close();movies.delete(request.handle);return result;}
        throw new Error('Unsupported prepared original movie operation');
    }
    if(request.type==='frameOffsets') {
        const pointer=new DataView(new Uint8Array(vm.read(Number(movie.movie)+0x1c8,8)).buffer).getBigUint64(0,true);
        const frames=new DataView(new Uint8Array(vm.read(Number(movie.movie)+8,4)).buffer).getUint32(0,true);
        if(!pointer||frames>1048576)throw new Error('Invalid SDK frame-offset table');
        return {offsets:new Uint8Array(vm.read(Number(pointer),(frames+1)*8)).buffer};
    }
    if(request.type==='control') {
        const controls={pause:['BinkPause',1],wait:['BinkWait',0],shouldSkip:['BinkShouldSkip',0],
            goto:['BinkGoto',2],sound:['BinkSetSoundOnOff',1],keyFrame:['BinkGetKeyFrame',2],loop:['BinkSetWillLoop',1]};
        const command=controls[request.control],args=request.args||[];
        if(!command||args.length!==command[1]||args.some(value=>!Number.isSafeInteger(value)))
            throw new Error('Invalid Bink SDK control request');
        const result=vm.call(command[0],[movie.movie,...args],50000000);
        const header=new Uint8Array(vm.read(Number(movie.movie),176));
        return {value:String(result),header:header.buffer};
    }
    if(request.type==='summary') {
        const output=vm.allocateNative(144);
        try {
            vm.write(output,new Uint8Array(144));vm.call('BinkGetSummary',[movie.movie,output]);
            return {summary:new Uint8Array(vm.read(output,144)).buffer};
        }finally {vm.freeNative(output);}
    }
    if(request.type==='registerFrameBuffers') {
        if(!movie.frameBuffers)throw new Error('Movie does not use caller-owned frame buffers');
        movie.frameBuffers.register(request.pitches);return {registered:true};
    }
    if(request.type==='frame') {
        vm.call('BinkDoFrame',[movie.movie],50000000);
        const header=new DataView(new Uint8Array(vm.read(Number(movie.movie),32)).buffer);
        if(header.getUint32(28,true))throw new Error('Original SDK reported a movie read error');
        if(movie.frameBuffers) return {frame:header.getUint32(16,true),backend:'original-sdk-vm',assetSHA256:movie.assetSHA256,
            header:new Uint8Array(vm.read(Number(movie.movie),176)).buffer,...movie.frameBuffers.readCurrent()};
        vm.call('BinkCopyToBuffer',[movie.movie,movie.output,movie.width*4,movie.height,0,0,6],50000000);
        const pixels=new Uint8Array(vm.read(movie.output,movie.length));
        return {frame:header.getUint32(16,true),width:movie.width,height:movie.height,pixels:pixels.buffer,backend:'original-sdk-vm',assetSHA256:movie.assetSHA256};
    }
    if(request.type==='next') {
        vm.call('BinkNextFrame',[movie.movie]);return {advanced:true,header:new Uint8Array(vm.read(Number(movie.movie),176)).buffer};
    }
    if(request.type==='close') {
        vm.call('BinkClose',[movie.movie]);
        if(movie.frameBuffers)movie.frameBuffers.dispose();
        if(movie.output)vm.freeNative(movie.output);
        vm.freeNative(movie.buffer);
        movies.delete(request.handle);return {closed:true};
    }
    throw new Error('Unsupported Bink worker request');
}
self.onmessage=({data})=> {
    serial=serial.then(async()=> {
        try {
            const result=await dispatch(data);
            const transfers=result.pixels?[result.pixels]:(result.planes||[]).map(plane=>plane.bytes);
    self.postMessage({id:data.id,result,sdkInitialized:!!vm},transfers);
        }catch(error) {
            const fatal=error instanceof WebAssembly.RuntimeError||Boolean(vm?.lastMemoryFault)||Boolean(vm?.lastFailure);
            if(fatal)faulted=true;
            self.postMessage({id:data.id,error:String(error?.message||error),fatal});
        }
    });
};
