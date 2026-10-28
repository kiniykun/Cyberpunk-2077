// [warmup] Low-allocation replacements for the hottest emdawnwebgpu glue entry points (--js-library).
// Port JS libraries are merged AFTER user --js-library files (ports' linker_setup runs late), so a same-name
// definition here would be overridden; instead these are cp2077_* functions (own __sig, so memory64 pointer
// conversion is generated) and the $cp2077Glue postset rebinds the original import variables to them before
// assignWasmImports() runs. Pulled in by -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$cp2077Glue (relink script).
// Main-thread allocation sampling at 32 vehicles (S/warmup/r32.heap.json): ~21 MB/s, of which
// wgpuDeviceCreateBindGroup ~10 MB/s (two closures per call, one extension-handler object + closure per entry,
// fresh descriptor/entry/resource objects), wgpuCommandEncoderBeginRenderPass ~2.8 MB/s,
// wgpuRenderPassEncoderSetBlendConstant ~0.6 MB/s. That drives a V8 GC every ~1.3 s.
// WebGPU reads descriptor dictionaries synchronously during the call, so the objects here are reused (pooled by
// entry/attachment count) and their object references cleared right after the call (no GPU object is retained).
// Same semantics as the originals. ?glueLowAlloc=0 = original glue code paths.
// Struct offsets / SType value are the wasm64 values from emdawnwebgpu's generated struct info + enum tables
// (the port's macros are not defined yet when this file is preprocessed). wasm64 only:
{{{ (() => { if (!MEMORY64) throw new Error('webgpu-lowalloc-web.js: wasm64 struct offsets only'); return ''; })() }}}
addToLibrary({
  $cp2077Glue__deps: ['cp2077_wgpuDeviceCreateBindGroup', 'cp2077_wgpuRenderPassEncoderSetBlendConstant', 'cp2077_wgpuCommandEncoderBeginRenderPass'],
  $cp2077Glue__postset: '_wgpuDeviceCreateBindGroup = _cp2077_wgpuDeviceCreateBindGroup; ' +
    '_wgpuRenderPassEncoderSetBlendConstant = _cp2077_wgpuRenderPassEncoderSetBlendConstant; ' +
    '_wgpuCommandEncoderBeginRenderPass = _cp2077_wgpuCommandEncoderBeginRenderPass;',
  $cp2077Glue: {
    on: undefined,
    bgDesc: {label: undefined, layout: null, entries: null},
    bgLists: [],      // bgLists[count] = array of count entry objects
    bgBuffers: [],    // bgBuffers[count][i] = reusable buffer binding object
    rpDesc: {label: undefined, colorAttachments: null, depthStencilAttachment: undefined,
             occlusionQuerySet: undefined, timestampWrites: undefined, maxDrawCount: undefined},
    rpLists: [],      // rpLists[count] = array of count color attachment objects
    rpColors: [],
    rpDepth: {view: null, depthClearValue: 0, depthLoadOp: undefined, depthStoreOp: undefined, depthReadOnly: false,
              stencilClearValue: 0, stencilLoadOp: undefined, stencilStoreOp: undefined, stencilReadOnly: false},
    blend: {r: 0, g: 0, b: 0, a: 0},
    enabled() {
      if (this.on === undefined) {
        try { this.on = !(typeof location != 'undefined' && new URLSearchParams(location.search).get('glueLowAlloc') === '0'); }
        catch (e) { this.on = true; }
      }
      return this.on;
    },
  },

  cp2077_wgpuDeviceCreateBindGroup__sig: 'ppp',
  cp2077_wgpuDeviceCreateBindGroup__deps: ['emwgpuCreateBindGroup'],
  cp2077_wgpuDeviceCreateBindGroup: (devicePtr, descriptor) => {
    var G = cp2077Glue, low = G.enabled();
    var count = {{{ makeGetValue('descriptor', 32, '*') }}};
    var entriesPtr = {{{ makeGetValue('descriptor', 40, '*') }}};
    var list, bufs;
    if (low) {
      list = G.bgLists[count];
      if (!list) {
        list = G.bgLists[count] = [];
        bufs = G.bgBuffers[count] = [];
        for (var k = 0; k < count; ++k) { list.push({binding: 0, resource: null}); bufs.push({buffer: null, offset: 0, size: undefined}); }
      }
      bufs = G.bgBuffers[count];
    } else list = [];
    for (var i = 0; i < count; ++i) {
      var entryPtr = entriesPtr + {{{ 56 }}} * i;
      var bufferPtr = {{{ makeGetValue('entryPtr', 16, '*') }}};
      var samplerPtr = {{{ makeGetValue('entryPtr', 40, '*') }}};
      var textureViewPtr = {{{ makeGetValue('entryPtr', 48, '*') }}};
      var externalTexturePtr = 0;
      for (var ext = {{{ makeGetValue('entryPtr', 0, '*') }}}; ext;
           ext = {{{ makeGetValue('ext', 0, '*') }}}) {
        if ({{{ makeGetValue('ext', 8, 'i32') }}} === 14)
          externalTexturePtr = {{{ makeGetValue('ext', 16, '*') }}};
      }
      var resource;
      if (bufferPtr) {
        var size = {{{ makeGetValue('entryPtr', 32, 'i53') }}};
        if (size == -1) size = undefined;
        resource = low ? bufs[i] : {};
        resource["buffer"] = WebGPU.getJsObject(bufferPtr);
        resource["offset"] = {{{ makeGetValue('entryPtr', 24, 'i53') }}};
        resource["size"] = size;
      } else {
        resource = WebGPU.getJsObject(samplerPtr || textureViewPtr || externalTexturePtr);
      }
      var binding = {{{ makeGetValue('entryPtr', 8, 'u32') }}};
      if (low) { list[i]["binding"] = binding; list[i]["resource"] = resource; }
      else list.push({"binding": binding, "resource": resource});
    }
    var desc = low ? G.bgDesc : {};
    desc["label"] = WebGPU.makeStringFromOptionalStringView(descriptor + {{{ 8 }}});
    desc["layout"] = WebGPU.getJsObject({{{ makeGetValue('descriptor', 24, '*') }}});
    desc["entries"] = list;
    var device = WebGPU.getJsObject(devicePtr);
    var ptr = _emwgpuCreateBindGroup(0n);
    try {
      WebGPU.Internals.jsObjectInsert(ptr, device.createBindGroup(desc));
    } finally {
      if (low) {
        desc["layout"] = null; desc["entries"] = null;
        for (var j = 0; j < count; ++j) { list[j]["resource"] = null; bufs[j]["buffer"] = null; }
      }
    }
    return ptr;
  },

  cp2077_wgpuRenderPassEncoderSetBlendConstant__sig: 'vpp',
  cp2077_wgpuRenderPassEncoderSetBlendConstant__deps: [],
  cp2077_wgpuRenderPassEncoderSetBlendConstant: (passPtr, colorPtr) => {
    var pass = WebGPU.getJsObject(passPtr);
    var c = cp2077Glue.enabled() ? cp2077Glue.blend : {};
    c["r"] = {{{ makeGetValue('colorPtr', 0, 'double') }}};
    c["g"] = {{{ makeGetValue('colorPtr', 8, 'double') }}};
    c["b"] = {{{ makeGetValue('colorPtr', 16, 'double') }}};
    c["a"] = {{{ makeGetValue('colorPtr', 24, 'double') }}};
    pass.setBlendConstant(c);
  },

  cp2077_wgpuCommandEncoderBeginRenderPass__sig: 'ppp',
  cp2077_wgpuCommandEncoderBeginRenderPass__deps: ['emwgpuCreateRenderPassEncoder'],
  cp2077_wgpuCommandEncoderBeginRenderPass: (encoderPtr, descriptor) => {
    var G = cp2077Glue, low = G.enabled();
    var nextInChainPtr = {{{ makeGetValue('descriptor', 0, '*') }}};
    var maxDrawCount = undefined;
    if (nextInChainPtr !== 0) {
      maxDrawCount = {{{ makeGetValue('nextInChainPtr', 16, 'i53') }}};
    }
    var count = {{{ makeGetValue('descriptor', 24, '*') }}};
    var caBase = {{{ makeGetValue('descriptor', 32, '*') }}};
    var list, colors;
    if (low) {
      list = G.rpLists[count];
      if (!list) {
        list = G.rpLists[count] = []; colors = G.rpColors[count] = [];
        for (var k = 0; k < count; ++k) {
          colors.push({r: 0, g: 0, b: 0, a: 0});
          list.push({view: null, depthSlice: undefined, resolveTarget: undefined, clearValue: null, loadOp: undefined, storeOp: undefined});
        }
      }
      colors = G.rpColors[count];
    } else list = [];
    var attachments = low ? G.rpAttach || (G.rpAttach = []) : list;
    if (low) attachments.length = 0;
    for (var i = 0; i < count; ++i) {
      var caPtr = caBase + {{{ 72 }}} * i;
      var viewPtr = {{{ makeGetValue('caPtr', 8, '*') }}};
      if (viewPtr === 0) { if (low) attachments.push(undefined); else list.push(undefined); continue; }
      var depthSlice = {{{ makeGetValue('caPtr', 16, 'u32') }}};
      if (depthSlice == 0xFFFFFFFF) depthSlice = undefined;
      var a = low ? list[i] : {}, cv = low ? colors[i] : {};
      var cvPtr = caPtr + {{{ 40 }}};
      cv["r"] = {{{ makeGetValue('cvPtr', 0, 'double') }}};
      cv["g"] = {{{ makeGetValue('cvPtr', 8, 'double') }}};
      cv["b"] = {{{ makeGetValue('cvPtr', 16, 'double') }}};
      cv["a"] = {{{ makeGetValue('cvPtr', 24, 'double') }}};
      a["view"] = WebGPU.getJsObject(viewPtr);
      a["depthSlice"] = depthSlice;
      a["resolveTarget"] = WebGPU.getJsObject({{{ makeGetValue('caPtr', 24, '*') }}});
      a["clearValue"] = cv;
      a["loadOp"] = WebGPU.LoadOp[{{{ makeGetValue('caPtr', 32, 'i32') }}}];
      a["storeOp"] = WebGPU.StoreOp[{{{ makeGetValue('caPtr', 36, 'i32') }}}];
      if (low) attachments.push(a); else list.push(a);
    }
    var dsaPtr = {{{ makeGetValue('descriptor', 40, '*') }}};
    var ds = undefined;
    if (dsaPtr !== 0) {
      ds = low ? G.rpDepth : {};
      ds["view"] = WebGPU.getJsObject({{{ makeGetValue('dsaPtr', 8, '*') }}});
      ds["depthClearValue"] = {{{ makeGetValue('dsaPtr', 24, 'float') }}};
      ds["depthLoadOp"] = WebGPU.LoadOp[{{{ makeGetValue('dsaPtr', 16, 'i32') }}}];
      ds["depthStoreOp"] = WebGPU.StoreOp[{{{ makeGetValue('dsaPtr', 20, 'i32') }}}];
      ds["depthReadOnly"] = !!({{{ makeGetValue('dsaPtr', 28, 'u32') }}});
      ds["stencilClearValue"] = {{{ makeGetValue('dsaPtr', 40, 'u32') }}};
      ds["stencilLoadOp"] = WebGPU.LoadOp[{{{ makeGetValue('dsaPtr', 32, 'i32') }}}];
      ds["stencilStoreOp"] = WebGPU.StoreOp[{{{ makeGetValue('dsaPtr', 36, 'i32') }}}];
      ds["stencilReadOnly"] = !!({{{ makeGetValue('dsaPtr', 44, 'u32') }}});
    }
    var desc = low ? G.rpDesc : {};
    desc["label"] = WebGPU.makeStringFromOptionalStringView(descriptor + {{{ 8 }}});
    desc["colorAttachments"] = attachments;
    desc["depthStencilAttachment"] = ds;
    desc["occlusionQuerySet"] = WebGPU.getJsObject({{{ makeGetValue('descriptor', 48, '*') }}});
    desc["timestampWrites"] = WebGPU.makePassTimestampWrites({{{ makeGetValue('descriptor', 56, '*') }}});
    desc["maxDrawCount"] = maxDrawCount;
    var commandEncoder = WebGPU.getJsObject(encoderPtr);
    var ptr = _emwgpuCreateRenderPassEncoder(0n);
    try {
      WebGPU.Internals.jsObjectInsert(ptr, commandEncoder.beginRenderPass(desc));
    } finally {
      if (low) {
        for (var j = 0; j < count; ++j) { list[j]["view"] = null; list[j]["resolveTarget"] = undefined; }
        attachments.length = 0;
        if (ds) ds["view"] = null;
        desc["colorAttachments"] = null; desc["depthStencilAttachment"] = undefined;
        desc["occlusionQuerySet"] = undefined; desc["timestampWrites"] = undefined;
      }
    }
    return ptr;
  },
  // [nomadperf] One wasm->JS crossing per draw: gpu-command-web.cpp writes the draw's (already deduplicated) pass
  // state calls as u32 words and this replays them on the GPURenderPassEncoder. Avoids ~5-8 glue calls per draw
  // (BigInt pointer conversion, asserting getJsObject, size sentinel handling). Pointers/u64 are lo,hi word pairs.
  // Ops: 1 pipeline(p) 2 viewport(6 f32) 3 scissor(4) 4 blend(4 f32) 5 stencil(1) 6 vertex(slot,p,off,size)
  //      7 bindgroup(index,p,n,offsets[n]) 8 index(fmt,p,off,size) 9 draw(4) 10 drawIndexed(count,inst,first,base(i32),firstInst)
  //      11 drawIndirect(p,off) 12 drawIndexedIndirect(p,off). size hi=lo=0xffffffff = whole size (undefined).
  cp2077_replayPassStream__sig: 'vppi',
  cp2077_replayPassStream: (passPtr, streamPtr, words) => {
    var objs = WebGPU.Internals.jsObjects;
    var pass = objs[passPtr];
    growMemViews();
    var u = HEAPU32, f = HEAPF32, i = streamPtr / 4, end = i + words;
    var p = (k) => u[k] + u[k + 1] * 4294967296;
    var sz = (k) => (u[k] === 0xffffffff && u[k + 1] === 0xffffffff) ? undefined : u[k] + u[k + 1] * 4294967296;
    while (i < end) {
      switch (u[i]) {
        case 1: pass.setPipeline(objs[p(i + 1)]); i += 3; break;
        case 2: pass.setViewport(f[i + 1], f[i + 2], f[i + 3], f[i + 4], f[i + 5], f[i + 6]); i += 7; break;
        case 3: pass.setScissorRect(u[i + 1], u[i + 2], u[i + 3], u[i + 4]); i += 5; break;
        case 4: { var c = cp2077Glue.blend; c["r"] = f[i + 1]; c["g"] = f[i + 2]; c["b"] = f[i + 3]; c["a"] = f[i + 4]; pass.setBlendConstant(c); i += 5; break; }
        case 5: pass.setStencilReference(u[i + 1]); i += 2; break;
        case 6: pass.setVertexBuffer(u[i + 1], objs[p(i + 2)], p(i + 4), sz(i + 6)); i += 8; break;
        case 7: { var n = u[i + 4]; if (n) pass.setBindGroup(u[i + 1], objs[p(i + 2)], u, i + 5, n); else pass.setBindGroup(u[i + 1], objs[p(i + 2)]); i += 5 + n; break; }
        case 8: pass.setIndexBuffer(objs[p(i + 2)], u[i + 1] === 1 ? 'uint16' : 'uint32', p(i + 4), sz(i + 6)); i += 8; break;
        case 9: pass.draw(u[i + 1], u[i + 2], u[i + 3], u[i + 4]); i += 5; break;
        case 10: pass.drawIndexed(u[i + 1], u[i + 2], u[i + 3], u[i + 4] | 0, u[i + 5]); i += 6; break;
        case 11: pass.drawIndirect(objs[p(i + 1)], p(i + 3)); i += 5; break;
        case 12: pass.drawIndexedIndirect(objs[p(i + 1)], p(i + 3)); i += 5; break;
        default: throw new Error('cp2077_replayPassStream: bad op ' + u[i] + ' at ' + (i - streamPtr / 4));
      }
    }
  },
});
