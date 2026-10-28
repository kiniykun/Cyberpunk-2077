const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const block=1024*1024,total=8*block,nodes=new Map();
const entry={path:'/game/archive/pc/basegame_1.archive',size:total,mtime:0,etag:'"original"'};
const data=Buffer.alloc(total);for(let i=0;i<total;i++)data[i]=i%251;
let syncRequests=0,corrupt=false;
const Module={},context=vm.createContext({Module,performance,AbortController,console,
 addRunDependency(){},removeRunDependency(){},abort(message){throw Error(message)},
 FS:{ErrnoError:class extends Error{},mkdirTree(){},symlink(){},createDataFile(dir,name){nodes.set(dir+'/'+name,{node_ops:{},stream_ops:{}})},lookupPath(path){return {node:nodes.get(path)}}},
 fetch:async(path,options)=>{
   if(path==='/runtime/game-files.json')return {ok:true,json:async()=>({version:1,files:[entry]})};
   const match=options.headers.Range.match(/bytes=(\d+)-(\d+)/),start=Number(match[1]),end=Number(match[2]);
   return {status:206,headers:{get:name=>name==='ETag'?(corrupt?'"wrong"':entry.etag):name==='Content-Range'?`bytes ${start}-${end}/${total}`:null},arrayBuffer:async()=>data.subarray(start,end+1).buffer.slice(data.byteOffset+start,data.byteOffset+end+1)};
 },XMLHttpRequest:class {
   constructor(){this.headers={};this.status=206;}
   open(){}overrideMimeType(){}setRequestHeader(name,value){this.headers[name]=value;}
   send(){syncRequests++;const m=this.headers.Range.match(/bytes=(\d+)-(\d+)/);this.start=+m[1];this.end=+m[2];this.responseText=data.subarray(this.start,this.end+1).toString('latin1');}
   getResponseHeader(name){return name==='ETag'?entry.etag:name==='Content-Range'?`bytes ${this.start}-${this.end}/${total}`:null;}
 }});
vm.runInContext(fs.readFileSync('D:/2077/src/resource-filesystem-web.js','utf8'),context);
const tick=()=>new Promise(resolve=>setImmediate(resolve));
async function run(){
 Module.preRun[0]();await tick();
 const read=number=>{const out=new Uint8Array(block);const got=nodes.get(entry.path).stream_ops.read(null,out,0,block,number*block);assert.equal(got,block);assert.deepEqual(Buffer.from(out),data.subarray(number*block,(number+1)*block));};
 read(0);read(1);await tick();read(2);
 assert.equal(Module.cp2077ResourceFileMetrics.asyncHits,1);assert.equal(syncRequests,2);
 corrupt=true;read(3);await tick();read(4);await tick();
 assert.ok(Module.cp2077ResourceFileMetrics.asyncErrors>=1);
 assert.ok(Module.cp2077ResourceFileMetrics.cacheBytes<=64*block);
 assert.equal(Module.cp2077ResourceFileMetrics.pendingAsyncBytes,0);
 const result={originalByteParity:true,asynchronousCacheHit:true,invalidEtagFallback:true,boundedMemory:true,passed:true};
 fs.writeFileSync('D:/2077/build/resource-prefetch-validation.json',JSON.stringify(result,null,2));console.log(JSON.stringify(result));
}run().catch(error=>{console.error(error);process.exitCode=1});
