(() => {
  'use strict';
  if (globalThis.cp2077PerformanceOverlay) return;
  const panel = document.createElement('pre');
  panel.id = 'cp2077-performance-overlay';
  panel.style.cssText = 'position:fixed;left:12px;top:12px;z-index:10000;margin:0;padding:8px 10px;background:rgba(0,0,0,.8);color:#b8ffad;font:13px/1.4 monospace;white-space:pre-wrap;max-width:calc(100vw - 44px);pointer-events:none;text-shadow:0 1px #000';
  document.body.append(panel);
  const tracking = { buffers: 0, bufferBytes: 0, textures: 0, textureBytes: 0, unknownTextures: 0 };
  const buffers = new WeakMap(), textures = new WeakMap();
  const bytes = n => Number.isFinite(n) ? (n / 1048576).toFixed(1) + ' MiB' : 'unavailable';
  const num = n => Number.isFinite(n) ? n.toFixed(2) : 'unavailable';
  const textureSize = d => {
    const f = d.format;
    let block = 1, b;
    if (/^(r8)/.test(f)) b = 1;
    else if (/^(rg8|r16|depth16)/.test(f)) b = 2;
    else if (/^(rgba8|bgra8|rg16|r32|rgb10a2|rg11b10|depth32float$)/.test(f)) b = 4;
    else if (/^(rgba16|rg32)/.test(f)) b = 8;
    else if (/^rgba32/.test(f)) b = 16;
    else if (/^bc[1-7]/.test(f)) { block = 4; b = /^bc[14]-/.test(f) ? 8 : 16; }
    else return null; // Implementation-dependent depth/stencil sizes are not guessed.
    const s = d.size, w = Number(s.width ?? s[0]), h = Number(s.height ?? s[1] ?? 1), z = Number(s.depthOrArrayLayers ?? s[2] ?? 1);
    let total = 0;
    for (let mip = 0; mip < (d.mipLevelCount || 1); ++mip) {
      total += Math.ceil(Math.max(1, Math.floor(w / 2 ** mip)) / block) * Math.ceil(Math.max(1, Math.floor(h / 2 ** mip)) / block) * (d.dimension === '3d' ? Math.max(1, Math.floor(z / 2 ** mip)) : z) * b * (d.sampleCount || 1);
    }
    return total;
  };
  // Track logical allocations only: driver overhead and canvas backing are excluded.
  if (globalThis.GPUDevice && globalThis.GPUBuffer && globalThis.GPUTexture) {
    const createBuffer = GPUDevice.prototype.createBuffer, createTexture = GPUDevice.prototype.createTexture;
    const destroyBuffer = GPUBuffer.prototype.destroy, destroyTexture = GPUTexture.prototype.destroy;
    GPUDevice.prototype.createBuffer = function(d) {
      const result = createBuffer.call(this, d), size = Number(d.size);
      buffers.set(result, size); tracking.buffers++; tracking.bufferBytes += size; return result;
    };
    GPUBuffer.prototype.destroy = function() {
      const result = destroyBuffer.call(this);
      if (buffers.has(this)) { tracking.bufferBytes -= buffers.get(this); tracking.buffers--; buffers.delete(this); }
      return result;
    };
    // [compress] Live texture bytes by bucket: compression class (bc/raw/depth) x role
    // (mlpool = multilayer pool arrays, sampled = sampled/copy-only, target = render/storage) and by format.
    tracking.buckets = {}; tracking.formats = {};
    const bucketOf = d => {
      const f = String(d.format), u = Number(d.usage) || 0, label = String(d.label || '');
      const cls = /^bc/.test(f) ? 'bc' : /^(depth|stencil)/.test(f) ? 'depth' : 'raw';
      const role = /multilayer pool/.test(label) ? 'mlpool' : (u & 0x18) ? 'target' : 'sampled';
      return [cls + '|' + role, f + '|' + role];
    };
    const account = (keys, size, sign) => {
      const [bucket, format] = keys;
      for (const [table, key] of [[tracking.buckets, bucket], [tracking.formats, format]]) {
        const row = table[key] || (table[key] = {count: 0, bytes: 0});
        row.count += sign; row.bytes += sign * (size || 0);
      }
    };
    GPUDevice.prototype.createTexture = function(d) {
      const result = createTexture.call(this, d), size = textureSize(d), keys = bucketOf(d);
      textures.set(result, {size, keys}); tracking.textures++;
      if (size === null) tracking.unknownTextures++; else tracking.textureBytes += size;
      account(keys, size, 1);
      return result;
    };
    GPUTexture.prototype.destroy = function() {
      const result = destroyTexture.call(this);
      if (textures.has(this)) {
        const {size, keys} = textures.get(this); tracking.textures--;
        if (size === null) tracking.unknownTextures--; else tracking.textureBytes -= size;
        account(keys, size, -1);
        textures.delete(this);
      }
      return result;
    };
    // [compress] queue.writeTexture traffic (initial texture uploads + unlock uploads): calls, bytes, JS-side ms.
    if (globalThis.GPUQueue) {
      const writeTexture = GPUQueue.prototype.writeTexture;
      tracking.writeTexture = {calls: 0, bytes: 0, ms: 0};
      GPUQueue.prototype.writeTexture = function(destination, data, layout, size) {
        const began = performance.now();
        try { return writeTexture.call(this, destination, data, layout, size); }
        finally {
          const w = tracking.writeTexture; w.calls++; w.ms += performance.now() - began;
          w.bytes += Number(data?.byteLength ?? 0) - Number(layout?.offset ?? 0);
        }
      };
    }
  }
  let lastFrames = 0, lastAt = performance.now(), windowFps = 0;
  let uploadLastAt=performance.now(),uploadLastBytes=0,uploadLastCalls=0,uploadRate=0,uploadCallsRate=0;
  let ioLastAt=performance.now(),ioLastBytes=0,ioLastSyncMs=0,ioRate=0,ioBlockingMsPerSecond=0;
  function update() {
    const m = globalThis.cp2077EngineModule, now = performance.now();
    const p = m?.cp2077PresentationStats || {}, frames = p.validatedFrames || 0;
    if (now - lastAt >= 1000) { windowFps = (frames - lastFrames) * 1000 / (now - lastAt); lastFrames = frames; lastAt = now; }
    const memory = m?.cp2077AllocatorMemoryDiagnostics || [];
    const record = [...memory].reverse().find(r => r.message.startsWith('CP2077 allocator memory probe:'));
    const field = key => { const match = record?.message.match(new RegExp(key + '=(\\d+)')); return match ? Number(match[1]) : NaN; };
    const pipeline = m?.cp2077PipelineCacheStats || {}, binding = m?.cp2077BindGroupCacheStats || {}, view = m?.cp2077TextureViewCacheStats || {};
    const upload=globalThis.cp2077GpuUploadStats;
    if(upload && now-uploadLastAt>=1000){const dt=(now-uploadLastAt)/1000;uploadRate=(upload.bytes-uploadLastBytes)/dt;uploadCallsRate=(upload.calls-uploadLastCalls)/dt;uploadLastBytes=upload.bytes;uploadLastCalls=upload.calls;uploadLastAt=now;}
    const cache = c => `${c.entries ?? 0} entries | hit ${c.hits ?? 0} / miss ${c.misses ?? 0} / evict ${c.evictions ?? 0}`;
    const stage = m?.cp2077NativeLoadingStageDiagnostics?.at(-1)?.message || 'initializing engine';
    const phaseCandidate = m?.cp2077EngineFramePhaseDiagnostics?.at(-1);
    const phase = phaseCandidate && Date.now()-Date.parse(phaseCandidate.timestamp)<=10000 ? phaseCandidate : undefined;
    const phaseRows=m?.cp2077EngineFramePhaseDiagnostics || [];
    const latestPhase=prefix=>[...phaseRows].reverse().find(r=>r.message.startsWith(prefix));
    const gamePhase=latestPhase('CP2077 game engine phases probe:');
    const basePhase=latestPhase('CP2077 engine frame phases probe:');
    const diagnosticValue=(record,key)=>{
      if(!record || Date.now()-Date.parse(record.timestamp)>10000)return NaN;
      const match=record.message.match(new RegExp(key+'=([\\d.]+)'));
      return match ? Number(match[1]) : NaN;
    };
    const asset=m?.cp2077ResourceFileMetrics;
    const commandLocks=typeof m?.cp2077CommandLockSnapshot==='function'?m.cp2077CommandLockSnapshot():[];
    const lockOwner=commandLocks.find(r=>r.phase==='holding');
    if(asset && now-ioLastAt>=1000){
      const seconds=(now-ioLastAt)/1000;
      ioRate=(asset.bytesFetched-ioLastBytes)/seconds;
      ioBlockingMsPerSecond=((asset.syncReadMs || 0)-ioLastSyncMs)/seconds;
      ioLastAt=now;ioLastBytes=asset.bytesFetched;ioLastSyncMs=asset.syncReadMs || 0;
    }
    const allocation=[...(m?.cp2077RenderWorkerDiagnostics || [])].reverse().find(r=>r.message.startsWith('CP2077 GPU allocation wait probe:'));
    const scheduling=m?.cp2077BrowserSchedulingDiagnostics;
    const timer=scheduling?.slowTimers?.at(-1);
    const scope=m?.cp2077NativeScopeBridgeDiagnostics?.at(-1);
    const scopeFresh=scope && now-scope.at<=10000;
    const mapped=m?.cp2077MappedUploadDiagnostics?.at(-1);
    const mappedValue=key=>{const match=mapped?.message.match(new RegExp(key+'=([\\d.]+)'));return match ? Number(match[1]) : NaN;};
    const phaseValue = key => { const match = phase?.message.match(new RegExp(key + '=([\\d.]+)')); return match ? Number(match[1]) : NaN; };
    const jobs = Object.values(m?.cp2077InitializationJobs || {});
    const hasNativeJobs=typeof m?.cp2077NativeJobSnapshot==='function';
    const liveJobs=hasNativeJobs ? m.cp2077NativeJobSnapshot() : [];
    const nativeWorkers=m?.cp2077NativeWorkerSnapshot?.();
    const oldestJob=liveJobs.reduce((oldest,row)=>!oldest||row.ageMs>oldest.ageMs ? row : oldest,null);
    panel.textContent = [
      `FPS: ${num(windowFps)} | recent: ${num(p.recentFps)} | target: 60`,
      `Frame interval: ${num(p.lastIntervalMs)} ms | presented: ${frames}`,
      `Last frame age: ${p.lastPresentedAt ? num(Math.max(0, (p.lastPresentedAt > 1e12 ? Date.now() : now) - p.lastPresentedAt)) + ' ms' : 'no frame yet'}`,
      `Engine wall time sampled: ${num(phaseValue('totalMs'))} ms | sample age ${phase ? num((Date.now() - Date.parse(phase.timestamp)) / 1000) + ' s' : 'unavailable'}`,
      `Frame phases sampled (ms): previous commands ${num(diagnosticValue(gamePhase,'previousCommandsMs'))} | frame tick ${num(diagnosticValue(gamePhase,'frameTickMs'))} | UI ${num(diagnosticValue(gamePhase,'renderUiMs'))}`,
      `Base phases sampled (ms): resources ${num(diagnosticValue(basePhase,'resourcesMs'))} | streaming ${num(diagnosticValue(basePhase,'streamingMs'))} | simulation ${num(diagnosticValue(basePhase,'simulationMs'))}`,
      `GPU buffer wait sampled (ms): total ${num(diagnosticValue(allocation,'wallMs'))} | dispatch ${num(diagnosticValue(allocation,'deliveryMs'))} | validation ${num(diagnosticValue(allocation,'validationAndCacheMs'))}`,
      `GPU scope callback sampled: ${num(scopeFresh ? scope.promiseMs : NaN)} ms | browser timer lag last ${num(timer?.lagMs)} ms / age ${timer ? num((now-timer.at)/1000)+' s' : 'unavailable'}`,
      `Render: ${document.getElementById('cp2077-canvas')?.width ?? 'unavailable'} x ${document.getElementById('cp2077-canvas')?.height ?? 'unavailable'} | GPU time: unavailable (no timestamp measurement)`,
      `JS heap: ${bytes(performance.memory?.usedJSHeapSize)} (main thread only)`,
      `Wasm capacity: ${bytes(field('heapCapacityBytes'))} | engine committed: ${bytes(field('committedBytes'))}`,
      `WebGPU buffer sizes tracked: ${bytes(tracking.bufferBytes)} (${tracking.buffers})`,
      `WebGPU texture sizes estimated: ${bytes(tracking.textureBytes)} (${tracking.textures}, unknown size ${tracking.unknownTextures})`,
      `GPU buffer uploads: ${upload ? bytes(uploadRate) + '/s | ' + num(uploadCallsRate) + ' calls/s | largest ' + bytes(upload.largestWriteBytes) : 'unavailable'}`,
      `Asset reads: ${asset ? bytes(ioRate)+'/s | sync blocking '+num(ioBlockingMsPerSecond)+' ms/s | cache '+bytes(asset.cacheBytes) : 'unavailable'}`,
      `Async read-ahead: ${asset && 'asyncRequests' in asset ? 'hits '+asset.asyncHits+' / requests '+asset.asyncRequests+' | pending '+bytes(asset.pendingAsyncBytes)+' | errors '+asset.asyncErrors : 'not in this build'}`,
      `Mapped scan sampled: ${num(mappedValue('scanMs'))} ms / ${bytes(mappedValue('scanBytes'))} | age ${mapped ? num((Date.now()-Date.parse(mapped.timestamp))/1000)+' s' : 'unavailable'}`,
      `Total tab RAM / VRAM: unavailable; figures above are not totals`,
      `Pipelines: ${pipeline.entries ?? 0} | hits ${pipeline.hits ?? 0} / misses ${pipeline.misses ?? 0} | compile ${num(pipeline.compileMs)} ms`,
      `Bindings: ${cache(binding)} | retained buffers ${bytes(binding.retainedBufferBytes || 0)}`,
      `Texture views: ${cache(view)}`,
      `View evictions: capacity ${view.capacityEvictions ?? 'unavailable'} / destroyed ${view.textureInvalidations ?? 'unavailable'} | limit ${view.entryLimit ?? 'unavailable'}`,
      `Init jobs pending: ${jobs.filter(j => j.entered > j.returned).length} | WebGPU errors: ${m?.cp2077NativeWebGpuErrors?.length ?? 0}`,
      `Recorded command failures: ${m?.cp2077GpuFailureTraces?.length ?? 0} (bounded samples) | missing shader samples: ${m?.cp2077NativeMaterialShaderDiagnostics?.length ?? 0}`,
      `Native workers: ${nativeWorkers?.active?.length ?? 'unavailable'} | jobs running: ${hasNativeJobs ? liveJobs.filter(j=>j.phase===1).length : 'unavailable'} | counter waits: ${hasNativeJobs ? liveJobs.filter(j=>j.phase===2).length : 'unavailable'}`,
      `Longest native job/wait: ${oldestJob ? 'worker '+oldestJob.worker+' '+(oldestJob.label || (oldestJob.phase===2 ? 'unnamed counter' : 'unnamed job'))+' | '+num(oldestJob.ageMs)+' ms' : 'unavailable'}`,
      // The lock probe runs only during trace capture; outside it the plain lock path publishes nothing.
      typeof m?.cp2077CommandLockSnapshot==='function'
        ? `Command lock (sampled during traces): ${commandLocks.filter(r=>r.phase==='waiting').length} waiting | owner ${lockOwner ? lockOwner.function+':'+lockOwner.line+' | '+num(lockOwner.ageMs)+' ms' : 'none'}`
        : 'Command lock: not sampled (probe runs only during trace capture)',
      `Stage: ${stage.replace(/^CP2077 /, '').slice(0, 200)}`,
    ].join('\n');
  }
  globalThis.cp2077PerformanceOverlay = { tracking, update };
  update(); setInterval(update, 500);
})();
