// Headless WebGPU (Deno) validation of converted WGSL: shader module + render pipeline with layout 'auto'.
// Device limits stay at WebGPU defaults (e.g. 16 sampled textures per stage) to match Chrome's baseline.
// Usage: deno run -A validate-wgsl.ts <prep.json> [hash...]
const [prepPath, ...only] = Deno.args;
const prep = JSON.parse(await Deno.readTextFile(prepPath));
const adapter = await navigator.gpu.requestAdapter();
if (!adapter) throw new Error('no adapter');
const want = ['clip-distances', 'shader-f16', 'texture-compression-bc', 'float32-filterable', 'depth-clip-control', 'dual-source-blending', 'texture-formats-tier1'];
// CP2077_VALIDATE_ADAPTER_TEXTURES=1: request the adapter's sampled-texture limit, as gpu-device-web.cpp does
// (emulated t130 bindless pools bind 24 extra texture arrays in multilayered pixel shaders).
const adapterTextures = Deno.env.get('CP2077_VALIDATE_ADAPTER_TEXTURES') === '1';
const device = await adapter.requestDevice({ requiredFeatures: want.filter(f => adapter.features.has(f)) as GPUFeatureName[],
  requiredLimits: adapterTextures ? { maxSampledTexturesPerShaderStage: adapter.limits.maxSampledTexturesPerShaderStage } : {} });

function structBody(code: string, name: string): string {
  const m = code.match(new RegExp('struct\\s+' + name + '\\s*\\{([\\s\\S]*?)\\}'));
  return m ? m[1] : '';
}

function stubVertex(code: string): { vs: string; targets: GPUColorTargetState[] } {
  const entry = code.match(/@fragment\s+fn\s+\w+\s*\(([\s\S]*?)\)\s*(?:->\s*(\w+|@location\(\d+\)\s*[\w<>]+))?\s*\{/);
  let params = entry?.[1] ?? '';
  for (const t of [...params.matchAll(/:\s*(\w+)\s*(?:,|$)/g)]) params += structBody(code, t[1]);
  const varyings = [...params.matchAll(/@location\((\d+)\)\s*(@interpolate\([^)]*\)\s*)?\w+\s*:\s*([\w<>]+)/g)];
  const fields = varyings.map(v => `@location(${v[1]}) ${v[2] ?? ''}o${v[1]}: ${v[3]},`).join('\n');
  const init = varyings.map(v => `o.o${v[1]} = ${v[3]}();`).join(' ');
  const vs = `struct VSOut { @builtin(position) p: vec4<f32>, ${fields} };\n` +
    `@vertex fn main() -> VSOut { var o: VSOut; o.p = vec4<f32>(0.0); ${init} return o; }`;
  const ret = entry?.[2] ?? '';
  const outs = ret.startsWith('@location') ? [ret] :
    [...structBody(code, ret).matchAll(/@location\((\d+)\)\s*\w+\s*:\s*([\w<>]+)/g)].map(m => m[0]);
  const targets: GPUColorTargetState[] = [];
  for (const o of outs) {
    const m = o.match(/@location\((\d+)\)[\s\S]*?(?:vec\d<(\w+)>|(f32|u32|i32))/);
    const kind = m?.[2] ?? m?.[3] ?? 'f32';
    targets[Number(m?.[1] ?? 0)] = { format: kind === 'u32' ? 'rgba32uint' : kind === 'i32' ? 'rgba32sint' : 'rgba16float' };
  }
  return { vs, targets };
}

let failures = 0;
for (const s of prep.Shaders) {
  if (only.length && !only.includes(s.Hash)) continue;
  if (!s.WGSL) continue;
  const code = await Deno.readTextFile(s.WGSL);
  if (/^\s*enable\s+wgpu_/m.test(code)) { console.log(s.Hash, 'FAIL non-standard enable'); failures++; continue; }
  device.pushErrorScope('validation');
  const module = device.createShaderModule({ code });
  const info = await module.getCompilationInfo();
  const errors = info.messages.filter(m => m.type === 'error').map(m => `${m.lineNum}:${m.linePos} ${m.message}`);
  let pipelineError = '';
  if (!errors.length && s.Stage === 'pixel') {
    const { vs, targets } = stubVertex(code);
    try {
      await device.createRenderPipelineAsync({ layout: 'auto',
        vertex: { module: device.createShaderModule({ code: vs }), entryPoint: 'main' },
        fragment: { module, entryPoint: 'main', targets }, primitive: { topology: 'triangle-list' },
        depthStencil: { format: 'depth32float', depthWriteEnabled: true, depthCompare: 'less' } });
    } catch (e) { pipelineError = String(e); }
  }
  const scope = await device.popErrorScope();
  const ok = !errors.length && !pipelineError && !scope;
  if (!ok) failures++;
  console.log(s.Hash, s.Stage, ok ? 'OK' : 'FAIL', errors.join(' | '), pipelineError, scope ? scope.message : '');
}
console.log('limits sampledTextures', device.limits.maxSampledTexturesPerShaderStage, 'failures', failures);
Deno.exit(failures ? 1 : 0);
