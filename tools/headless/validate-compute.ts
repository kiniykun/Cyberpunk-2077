// Headless WebGPU (Deno) validation of converted compute WGSL: module + compute pipeline (layout 'auto').
// Usage: deno run -A validate-compute.ts <static-compute-conversion.json>
const manifest = JSON.parse(await Deno.readTextFile(Deno.args[0]));
const adapter = await navigator.gpu.requestAdapter();
if (!adapter) throw new Error('no adapter');
// Mirror the runtime device (gpu-device-web.cpp): optional features when the adapter has them, and the adapter's compute
// limits (maxComputeInvocationsPerWorkgroup 1024, workgroup storage 32768 on the target GPU) instead of WebGPU defaults.
const want = ['texture-compression-bc', 'float32-filterable', 'float32-blendable', 'shader-f16', 'subgroups',
  'texture-formats-tier1', 'texture-formats-tier2', 'rg11b10ufloat-renderable', 'bgra8unorm-storage'];
const limitNames = ['maxComputeInvocationsPerWorkgroup', 'maxComputeWorkgroupSizeX', 'maxComputeWorkgroupSizeY',
  'maxComputeWorkgroupSizeZ', 'maxComputeWorkgroupStorageSize', 'maxStorageTexturesPerShaderStage',
  'maxStorageBuffersPerShaderStage'];
const requiredLimits: Record<string, number> = {};
// Never exceed the browser target (RTX 5060 in Chrome: 1024 invocations, 1024x1024x64, 32768 B workgroup storage), so a
// shader that only fits this host's larger limits (Deno here reports 49152 B) is not accepted.
const browserCaps: Record<string, number> = { maxComputeInvocationsPerWorkgroup: 1024, maxComputeWorkgroupSizeX: 1024,
  maxComputeWorkgroupSizeY: 1024, maxComputeWorkgroupSizeZ: 64, maxComputeWorkgroupStorageSize: 32768 };
for (const name of limitNames) requiredLimits[name] = Math.min((adapter.limits as any)[name], browserCaps[name] ?? Infinity);
const device = await adapter.requestDevice({ requiredFeatures: want.filter(f => adapter.features.has(f)) as GPUFeatureName[],
  requiredLimits });
console.log('features', [...device.features].join(','), 'limits', JSON.stringify(requiredLimits));
// Optional trailing names restrict validation to those rows (others keep their previous result in the output file).
const only = new Set(Deno.args.slice(1));
const outPath = Deno.args[0].replace(/\.json$/, '-gpu-validation.json');
let results: Record<string, string> = {};
if (only.size) { try { results = JSON.parse(await Deno.readTextFile(outPath)); } catch { results = {}; } }
let failures = 0;
for (const s of manifest.Shaders) {
  if (!s.Complete || (only.size && !only.has(s.RuntimeName))) continue;
  const code = await Deno.readTextFile(s.WGSL);
  device.pushErrorScope('validation');
  const module = device.createShaderModule({ code });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter(m => m.type === 'error').map(m => `${m.lineNum}:${m.linePos} ${m.message}`);
  let pipelineError = '';
  if (!errors.length) {
    const entry = code.match(/@compute\s+@workgroup_size\([^)]*\)\s+fn\s+(\w+)/)?.[1] ?? 'main';
    try { await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: entry } }); }
    catch (e) { pipelineError = String(e); }
  }
  const scope = await device.popErrorScope();
  const ok = !errors.length && !pipelineError && !scope;
  results[s.RuntimeName] = ok ? 'OK' : (errors.join(' | ') + ' ' + pipelineError + ' ' + (scope?.message ?? '')).slice(0, 300);
  if (!ok) { failures++; console.log(s.RuntimeName, 'FAIL', results[s.RuntimeName]); }
}
await Deno.writeTextFile(outPath, JSON.stringify(results, null, 2));
console.log('validated', Object.keys(results).length, 'failures', failures);
