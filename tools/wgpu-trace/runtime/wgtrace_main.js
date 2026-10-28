export const T = {
  FRAME: 1, JOB_RUN: 2, JOB_DEP: 3, WAIT: 4, PROXY_DEQ: 5, PROXY_CALL: 6,
  SCOPE: 7, SETTLE: 8, SUBMIT: 9, SUBMIT_ENC: 10, GPU_PASS: 11,
  PRESENT: 12, PRESENT_DONE: 13, GCT: 14, JOB_SPAWN: 15,
  JOB_META: 16, COUNTER_ACTIVITY: 17, MUTEX_WAIT: 18, MUTEX_HOLD: 19, RENDER_PHASE: 20,
  CTR_ARM: 21, CTR_CONTRIB: 22, CTR_DEC: 23, CTR_WAIT: 24, CTR_WAKE: 25,
};
export const WK = { PROXY: 1, SCOPE: 2, SUBMIT_DONE: 3, JOB: 4, PRESENT: 5, OTHER: 6 };
export const SK = { SCOPE: 1, SUBMIT: 2 };
export const ID = { FRAME: 0, JOB: 1, REQ: 2, SCOPE: 3, SUBMIT: 4, ENCODER: 5, PRESENT: 6, COUNTER: 7 };

const REC_BYTES = 40;
const SITE_NAME_MAX = 256;

// Decodes a NUL-terminated site literal from the current heap. The pointer is validated like a ring pointer.
// Bytes are copied out first: TextDecoder rejects views on a SharedArrayBuffer.
function readCString(heap, address) {
  const ptr = Number(address);
  if (!ptr) return '';
  if (!Number.isSafeInteger(ptr) || ptr < 0 || ptr >= heap.byteLength) throw new RangeError('Invalid site name pointer');
  const end = Math.min(heap.byteLength, ptr + SITE_NAME_MAX);
  let n = ptr;
  while (n < end && heap[n] !== 0) n++;
  return new TextDecoder().decode(heap.slice(ptr, n));
}
const MAIN_TID = 0xffff;

export function createMainTrace({ module, submitCompletion = false, maxRecords = 65536, heapProvider } = {}) {
  if(!Number.isSafeInteger(maxRecords)||maxRecords<1)throw new RangeError('Invalid trace capacity');
  const recs = [];
  let droppedMain=0;
  const callKinds = new Map();
  const presents = new Map();
  let enabled = true;

  const now = () => performance.timeOrigin + performance.now();
  const push = (type, frameId, a, b, c, t0, t1) => {
    if (enabled) {
      if(recs.length>=maxRecords){droppedMain++;return;}
      recs.push({ type, frameId, a, b, c, tid: MAIN_TID, t0, t1 });
    }
  };
  const kindId = (name) => {
    let k = callKinds.get(name);
    if (k === undefined) { k = callKinds.size + 1; callKinds.set(name, k); }
    return k;
  };
  const nextId = (k) => module ? module._wgtrace_next_id(k) : 0;

  function proxyDeq(reqId, frameId) {
    push(T.PROXY_DEQ, frameId, reqId, 0, 0, 0, now());
  }

  function proxyCall(reqId, frameId, kindName, objId, fn) {
    const t0 = now();
    try { return fn(); }
    finally { push(T.PROXY_CALL, frameId, reqId, kindId(kindName), objId >>> 0, t0, now()); }
  }

  function scope(device, { scopeId, wrappedId = 0, reqId = 0, frameId = 0, filter = 'validation' }, fn) {
    device.pushErrorScope(filter);
    const t0 = now();
    let result, settled;
    try { result = fn(); }
    finally {
      settled = device.popErrorScope();
      const t1 = now();
      push(T.SCOPE, frameId, scopeId, wrappedId >>> 0, reqId, t0, t1);
      settled.then(
        (err) => push(T.SETTLE, frameId, scopeId, SK.SCOPE, err ? 1 : 0, now(), 0),
        () => push(T.SETTLE, frameId, scopeId, SK.SCOPE, 2, now(), 0),
      );
    }
    return { result, settled };
  }

  function submit(queue, { submitId, reqId = 0, frameId = 0, encoderIds = [] }, commandBuffers) {
    const t0 = now();
    queue.submit(commandBuffers);
    const t1 = now();
    push(T.SUBMIT, frameId, submitId, reqId, commandBuffers.length, t0, t1);
    for (const e of encoderIds) push(T.SUBMIT_ENC, frameId, submitId, e, 0, 0, 0);
    if (submitCompletion) {
      queue.onSubmittedWorkDone().then(
        () => push(T.SETTLE, frameId, submitId, SK.SUBMIT, 0, now(), 0),
        () => push(T.SETTLE, frameId, submitId, SK.SUBMIT, 2, now(), 0),
      );
    }
  }

  function gpuPass(frameId, submitId, passIndex, durationMs) {
    push(T.GPU_PASS, frameId, submitId, passIndex, 0, durationMs, 0);
  }

  function presentScheduled(presentId, { frameId = 0, reqId = 0, slot = 0 } = {}) {
    presents.set(presentId, { frameId, reqId, slot, sched: now() });
  }

  function presentRafFired(presentId, copyScopeId = 0) {
    const p = presents.get(presentId);
    if (!p) return;
    push(T.PRESENT, p.frameId, presentId, p.reqId, copyScopeId, p.sched, now());
    p.copyScopeId = copyScopeId;
  }

  function getCurrentTexture(presentId, context) {
    const p = presents.get(presentId);
    const t0 = now();
    try { return context.getCurrentTexture(); }
    finally { push(T.GCT, p ? p.frameId : 0, presentId, 0, 0, t0, now()); }
  }

  function presentDone(presentId, copySubmitId) {
    const p = presents.get(presentId);
    push(T.PRESENT_DONE, p ? p.frameId : 0, presentId, copySubmitId, 0, now(), 0);
    presents.delete(presentId);
  }

  function readWasmRings() {
    if (!module) return { records: [], dropped: 0 };
    const out = [];
    let dropped = 0;
    const n = module._wgtrace_ring_count();
    for (let i = 0; i < n; i++) {
      const address = module._wgtrace_ring_ptr(i);
      if(!address)continue; // A worker reserved the slot but has not published it.
      const ptr=Number(address);
      if(!Number.isSafeInteger(ptr)||ptr<0||ptr%8)throw new RangeError('Invalid trace pointer');
      const len = module._wgtrace_ring_len(i);
      dropped += module._wgtrace_ring_dropped(i);
      const heap=heapProvider?heapProvider():module.HEAPU8;
      if(!Number.isSafeInteger(len)||len<0||len*REC_BYTES>heap.byteLength-ptr)throw new RangeError('Trace exceeds current memory');
      const u32 = new Uint32Array(heap.buffer, ptr, len * (REC_BYTES / 4));
      const f64 = new Float64Array(heap.buffer, ptr, len * (REC_BYTES / 8));
      for (let k = 0; k < len; k++) {
        const u = k * 10, f = k * 5;
        out.push({
          type: u32[u], frameId: u32[u + 1], a: u32[u + 2], b: u32[u + 3], c: u32[u + 4], tid: u32[u + 5],
          t0: f64[f + 3], t1: f64[f + 4],
        });
      }
    }
    return { records: out, dropped };
  }

  function readSiteNames() {
    const out = {};
    if (!module?._wgtrace_site_count) return out;
    const heap = heapProvider ? heapProvider() : module.HEAPU8;
    const n = module._wgtrace_site_count();
    for (let i = 0; i < n; i++) out[i + 1] = readCString(heap, module._wgtrace_site_name(i)); // unpublished slot -> ''
    return out;
  }

  function exportTrace() {
    const w = readWasmRings();
    const records = w.records.concat(recs);
    records.sort((x, y) => (x.t0 || x.t1) - (y.t0 || y.t1));
    return {
      meta: {
        exportedAt: now(),
        droppedWasm: w.dropped,
        droppedMain,
        callKinds: Object.fromEntries([...callKinds].map(([k, v]) => [v, k])),
        siteNames: readSiteNames(),
        ctrTableFull: module?._wgtrace_ctr_table_full ? module._wgtrace_ctr_table_full() : 0,
        unregisteredEmits: module?._wgtrace_unregistered_emits ? module._wgtrace_unregistered_emits() : 0,
        submitCompletion,
      },
      records,
    };
  }

  function download(filename = 'wgtrace.json') {
    const blob = new Blob([JSON.stringify(exportTrace())], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = filename;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  }

  function reset() {
    recs.length = 0;
    droppedMain=0;
    if (module) module._wgtrace_reset();
  }

  return {
    now, nextId, proxyDeq, proxyCall, scope, submit, gpuPass,
    presentScheduled, presentRafFired, getCurrentTexture, presentDone,
    exportTrace, download, reset,
    setEnabled(on) { enabled = Boolean(on);module?._wgtrace_set_enabled?.(enabled?1:0); },
  };
}
