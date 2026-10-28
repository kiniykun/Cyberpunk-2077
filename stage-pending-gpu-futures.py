"""Record pending actual WebGPU callback phases without changing delivery."""
from pathlib import Path
import hashlib,json
r=Path(__file__).resolve().parent;p=r/'build/game/cp2077.js';text=p.read_text()
if 'cp2077PendingGpuFutureSnapshot' in text:raise SystemExit('Already instrumented')
helpers='''var cp2077GpuFutureRecords = new Map();
var cp2077GpuFutureBegin = (operation, id) => {
  if (cp2077GpuFutureRecords.size < 512) cp2077GpuFutureRecords.set(String(id),
    {id:String(id),operation,stage:"browser-promise",startedAt:performance.now()});
};
var cp2077GpuFutureStage = (id, stage) => {
  const record=cp2077GpuFutureRecords.get(String(id));
  if(record){record.stage=stage;record.stageAt=performance.now();}
};
var cp2077GpuFutureFinish = id => cp2077GpuFutureRecords.delete(String(id));
Module["cp2077PendingGpuFutureSnapshot"] = () => {
  const now=performance.now();
  return Array.from(cp2077GpuFutureRecords.values(),r=>({...r,ageMs:now-r.startedAt}));
};

'''
targets={
 'BufferMapAsync':('buffer-map','OnMapAsyncCompleted'),
 'DevicePopErrorScope':('error-scope','OnPopErrorScopeCompleted'),
 'QueueOnSubmittedWorkDone':('queue-completion','OnWorkDoneCompleted'),
 'DeviceCreateRenderPipelineAsync':('render-pipeline','OnCreateRenderPipelineCompleted'),
 'DeviceCreateComputePipelineAsync':('compute-pipeline','OnCreateComputePipelineCompleted'),
 'ShaderModuleGetCompilationInfo':('shader-compilation-info','OnCompilationInfoCompleted'),
}
promise_names={'BufferMapAsync':'mapAsync','DevicePopErrorScope':'popErrorScope','QueueOnSubmittedWorkDone':'onSubmittedWorkDone','DeviceCreateRenderPipelineAsync':'createRenderPipelineAsync','DeviceCreateComputePipelineAsync':'createComputePipelineAsync','ShaderModuleGetCompilationInfo':'getCompilationInfo'}
proof=[]
for name,(operation,completion) in targets.items():
    marker='var _emwgpu'+name+' = function('
    assert text.count(marker)==1,name
    start=text.index(marker);end=text.index('\n};',start)+len('\n};')
    block=text[start:end]
    anchor='  runtimeKeepalivePush();'
    assert block.count(anchor)==1,name
    block=block.replace(anchor,anchor+'\n  cp2077GpuFutureBegin("'+operation+'",futureId);',1)
    lines=block.splitlines();out=[];count=0
    for line in lines:
        if '_emwgpu'+completion+'(futureId,' in line:
            out.append('      cp2077GpuFutureStage(futureId,"native-callback");')
            out.append(line)
            out.append('      cp2077GpuFutureFinish(futureId);');count+=1
        else:
            out.append(line)
            if '// '+promise_names[name]+' fulfilled' in line or '// '+promise_names[name]+' rejected' in line:
                out.append('    cp2077GpuFutureStage(futureId,"'+('promise-rejected' if 'rejected' in line else 'promise-resolved')+'");')
    assert count>=1,(name,completion)
    text=text[:start]+'\n'.join(out)+text[end:]
    proof.append({'Operation':operation,'OriginalCompletionCallsPreserved':count})
anchor='var _emwgpuBufferMapAsync = function('
text=text.replace(anchor,helpers+anchor,1)
p.write_text(text)
(r/'build/pending-gpu-futures-stage.json').write_text(json.dumps({'JS_SHA256':hashlib.sha256(p.read_bytes()).hexdigest(),'RecordsBound':512,'Operations':proof,'Scope':'Read-only callback phase observations. Original promises and native completion calls unchanged.'},indent=2))
print('Pending actual GPU callback phases recorded.')
