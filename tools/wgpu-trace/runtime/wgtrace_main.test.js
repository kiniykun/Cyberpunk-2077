import test from 'node:test';
import assert from 'node:assert/strict';
import {createMainTrace,T} from './wgtrace_main.js';
test('main records are bounded and dropped records are visible',()=>{
 const trace=createMainTrace({maxRecords:2});
 for(let i=0;i<5;i++)trace.proxyDeq(i,1);
 const result=trace.exportTrace();assert.equal(result.records.length,2);assert.equal(result.meta.droppedMain,3);
 trace.reset();assert.equal(trace.exportTrace().meta.droppedMain,0);
});
test('memory64 pointers and unpublished ring slots use explicit current heap provider',()=>{
 const heap=new Uint8Array(new SharedArrayBuffer(4096));const view=new DataView(heap.buffer);
 view.setUint32(1024,T.FRAME,true);view.setUint32(1028,7,true);view.setFloat64(1048,100,true);view.setFloat64(1056,125,true);
 const module={_wgtrace_ring_count:()=>2,_wgtrace_ring_ptr:i=>i===0?0n:1024n,_wgtrace_ring_len:()=>1,_wgtrace_ring_dropped:()=>0};
 Object.defineProperty(module,'HEAPU8',{get(){throw Error('Unexported heap getter touched');}});
 const trace=createMainTrace({module,heapProvider:()=>heap});
 assert.deepEqual(trace.exportTrace().records,[{type:T.FRAME,frameId:7,a:0,b:0,c:0,tid:0,t0:100,t1:125}]);
 module._wgtrace_ring_ptr=()=>9007199254740993n;assert.throws(()=>trace.exportTrace(),RangeError);
});
test('scope wrappers preserve original promise identity and attribute out-of-order outcomes',async()=>{
 const trace=createMainTrace();const resolves=[];const promises=[];
 const device={pushErrorScope(){},popErrorScope(){const p=new Promise(resolve=>resolves.push(resolve));promises.push(p);return p;}};
 const a=trace.scope(device,{scopeId:11,frameId:1},()=>111);
 const b=trace.scope(device,{scopeId:12,frameId:1},()=>222);
 assert.equal(a.settled,promises[0]);assert.equal(b.settled,promises[1]);assert.equal(a.result,111);
 resolves[1](null);await b.settled;resolves[0]({message:'original failure'});await a.settled;
 const records=trace.exportTrace().records.filter(r=>r.type===T.SETTLE);
 assert.deepEqual(records.map(r=>[r.a,r.c]),[[12,0],[11,1]]);
});
test('site names decode from the heap provider, not UTF8ToString; unpublished slots are empty; loss counters export',()=>{
 const heap=new Uint8Array(new SharedArrayBuffer(4096));
 const name='RenderFrame/WaitPreviousCommands';heap.set(new TextEncoder().encode(name),2048);heap[2048+name.length]=0;
 const module={_wgtrace_ring_count:()=>0,_wgtrace_site_count:()=>2,_wgtrace_site_name:i=>i===0?2048n:0n,
  _wgtrace_ctr_table_full:()=>3,_wgtrace_unregistered_emits:()=>4};
 Object.defineProperty(module,'HEAPU8',{get(){throw Error('Unexported heap getter touched');}});
 const meta=createMainTrace({module,heapProvider:()=>heap}).exportTrace().meta;
 assert.deepEqual(meta.siteNames,{1:name,2:''});assert.equal(meta.ctrTableFull,3);assert.equal(meta.unregisteredEmits,4);
 module._wgtrace_site_name=()=>9007199254740993n;assert.throws(()=>createMainTrace({module,heapProvider:()=>heap}).exportTrace(),RangeError);
});
