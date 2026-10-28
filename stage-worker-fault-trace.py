"""Relay genuine pthread exception stacks to the owning browser console."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parent
path=root/'build/game/cp2077.js'
text=path.read_text()
worker='err(`worker: onmessage() captured an uncaught exception: ${ex}`);'
parent='var cmd = d.cmd;'
if 'cp2077NativeFault' not in text:
    if text.count(worker)!=1 or text.count(parent)!=1:raise RuntimeError('Unexpected SDK worker fault handler shape')
    text=text.replace(worker,'self.postMessage({cmd: "cp2077NativeFault", stack: ex?.stack || String(ex)});\n      '+worker)
    text=text.replace(parent,parent+'\n      if (cmd === "cp2077NativeFault") { err(d.stack); return; }')
if 'cmd === "cp2077GpuFailureTrace"' not in text:
    marker='var cmd = d.cmd;'
    if text.count(marker)!=1: raise RuntimeError('Unexpected SDK parent worker message handler shape')
    text=text.replace(marker,marker+'\n      if (cmd === "cp2077GpuFailureTrace") { var traces = Module["cp2077GpuFailureTraces"] ||= []; if (traces.length < 8) traces.push(d.record); return; }')
fault_handler='if (cmd === "cp2077NativeFault") { err(d.stack); return; }'
if fault_handler in text:
    text=text.replace(fault_handler,'if (cmd === "cp2077NativeFault") { var faults = Module["cp2077NativeFaults"] ||= []; if (faults.length < 8) faults.push({timestamp:new Date().toISOString(),stack:d.stack}); err(d.stack); return; }',1)
if "e.reason?.stack" not in text:
    marker='  function handleMessage(e) {'
    if text.count(marker)!=1:raise RuntimeError('Unexpected SDK worker event setup')
    text=text.replace(marker,"  self.addEventListener('error', e => self.postMessage({cmd:'cp2077NativeFault', stack:e.error?.stack || e.message}));\n  self.addEventListener('unhandledrejection', e => self.postMessage({cmd:'cp2077NativeFault', stack:e.reason?.stack || String(e.reason)}));\n"+marker)
path.write_text(text)
(root/'build/worker-fault-trace-result.json').write_text(json.dumps({'GeneratedJavaScriptSHA256':hashlib.sha256(path.read_bytes()).hexdigest(),'Scope':'Original worker error still propagates; genuine native stack additionally relayed to parent console.'},indent=2))
print('Native worker fault trace relayed to parent console.')
