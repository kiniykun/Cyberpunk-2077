export const T = {
  FRAME: 1, JOB_RUN: 2, JOB_DEP: 3, WAIT: 4, PROXY_DEQ: 5, PROXY_CALL: 6,
  SCOPE: 7, SETTLE: 8, SUBMIT: 9, SUBMIT_ENC: 10, GPU_PASS: 11,
  PRESENT: 12, PRESENT_DONE: 13, GCT: 14, JOB_SPAWN: 15,
  JOB_META: 16, COUNTER_ACTIVITY: 17, MUTEX_WAIT: 18, MUTEX_HOLD: 19, RENDER_PHASE: 20,
  CTR_ARM: 21, CTR_CONTRIB: 22, CTR_DEC: 23, CTR_WAIT: 24, CTR_WAKE: 25, GATE_RELEASE: 26,
};
// COUNTER is analyzer-internal: CTR_WAIT records become waits of this kind.
export const WK = { PROXY: 1, SCOPE: 2, SUBMIT_DONE: 3, JOB: 4, PRESENT: 5, OTHER: 6, COUNTER: 7 };
export const SK = { SCOPE: 1, SUBMIT: 2 };
const WAIT_NAME = { 1: 'proxy', 2: 'scope', 3: 'submit_done', 4: 'job', 5: 'present', 6: 'other', 7: 'counter' };
const EPS = 1e-3; // ms tolerance for nesting tests on the shared clock
const CTR_NEGATIVE = 0xFFFFFFFF;

const overlap = (a0, a1, b0, b1) => Math.max(0, Math.min(a1, b1) - Math.max(a0, b0));

const START = '__start';
const END = '__end';

export function indexRecords(records) {
  const ix = {
    frames: new Map(), jobs: new Map(), deps: new Map(), spawns: new Map(),
    proxyDeq: new Map(), proxyCall: new Map(), scopes: new Map(), scopeByReq: new Map(),
    settles: new Map(), submits: new Map(), submitEnc: new Map(), gpu: new Map(),
    present: new Map(), presentDone: new Map(), gct: new Map(), notes: [],
    // integration records 16..20, kept verbatim
    jobMeta: new Map(), counterActivity: [], mutexWait: [], mutexHold: [], renderPhase: [],
    // counter dependency records 21..25
    ctrArm: new Map(), ctrContrib: new Map(), ctrDec: new Map(), ctrWaits: [], ctrWake: new Map(),
    unknown: new Map(), jobsByTid: new Map(), gates: [], gatesByAccum: new Map(), decsByJob: new Map(),
  };
  const job = (id, frameId) => {
    let j = ix.jobs.get(id);
    if (!j) { j = { jobId: id, frameId, workerId: null, tid: null, start: Infinity, end: -Infinity, runs: 0, waits: [] }; ix.jobs.set(id, j); }
    return j;
  };
  const listPush = (m, k, v) => { const l = m.get(k); if (l) l.push(v); else m.set(k, [v]); };
  for (const r of records) {
    switch (r.type) {
      case T.FRAME: ix.frames.set(r.frameId, { frameId: r.frameId, start: r.t0, end: r.t1 }); break;
      case T.JOB_RUN: {
        const j = job(r.a, r.frameId);
        j.workerId = r.b; j.frameId = r.frameId; j.runs++;
        if (j.tid !== null && j.tid !== r.tid) ix.notes.push(`job ${r.a} ran on more than one thread (${j.tid}, ${r.tid}); cooperative nesting uses the first`);
        if (j.tid === null) j.tid = r.tid;
        j.start = Math.min(j.start, r.t0); j.end = Math.max(j.end, r.t1);
        break;
      }
      case T.JOB_META: ix.jobMeta.set(r.a, { jobId: r.a, nameId: r.c, counterAddr: r.b, tid: r.tid, t0: r.t0, t1: r.t1 }); break;
      case T.COUNTER_ACTIVITY: ix.counterActivity.push({ jobId: r.a, counterAddr: r.b, site: r.c, tid: r.tid, frameId: r.frameId, t0: r.t0, t1: r.t1 }); break;
      case T.MUTEX_WAIT: ix.mutexWait.push({ a: r.a, b: r.b, c: r.c, tid: r.tid, t0: r.t0, t1: r.t1 }); break;
      case T.MUTEX_HOLD: ix.mutexHold.push({ a: r.a, b: r.b, c: r.c, tid: r.tid, t0: r.t0, t1: r.t1 }); break;
      case T.RENDER_PHASE: ix.renderPhase.push({ a: r.a, b: r.b, c: r.c, tid: r.tid, frameId: r.frameId, t0: r.t0, t1: r.t1 }); break;
      case T.CTR_ARM: ix.ctrArm.set(r.a, { inst: r.a, addr: r.b + r.c * 2 ** 32, implicit: r.t1 === 1, t: r.t0 }); break;
      // Both carry [t0, t1] around the engine's atomic op; a record without an end stamp is a point interval.
      case T.CTR_CONTRIB: listPush(ix.ctrContrib, r.a, { inst: r.a, jobId: r.b, delta: r.c | 0, t: r.t0, t0: r.t0, t1: r.t1 > r.t0 ? r.t1 : r.t0, tid: r.tid }); break;
      case T.CTR_DEC: listPush(ix.ctrDec, r.a, { inst: r.a, jobId: r.b, after: r.c, t: r.t0, t0: r.t0, t1: r.t1 > r.t0 ? r.t1 : r.t0, tid: r.tid }); break;
      case T.CTR_WAIT: {
        const w = { jobId: r.b, kind: WK.COUNTER, target: r.a, site: r.c, t0: r.t0, t1: r.t1 };
        ix.ctrWaits.push(w);
        job(r.b, r.frameId).waits.push(w);
        break;
      }
      case T.CTR_WAKE: ix.ctrWake.set(`${r.a}:${r.b}:${r.t1}`, { inst: r.a, jobId: r.b, site: r.c, t: r.t0, resume: r.t1 }); break;
      case T.GATE_RELEASE: {
        const gte = { released: r.a, accum: r.b, jobId: r.c, t: r.t0, batch: r.t1, tid: r.tid };
        ix.gates.push(gte);
        if (r.b) listPush(ix.gatesByAccum, r.b, gte);
        break;
      }
      case T.JOB_DEP: { const l = ix.deps.get(r.a) || []; l.push(r.b); ix.deps.set(r.a, l); break; }
      // c === 1: inferred from a counter enqueue (ctr_add); the start may be gated by unrecorded conditions.
      case T.JOB_SPAWN: ix.spawns.set(r.a, { parent: r.b, t: r.t0, fromEnqueue: r.c === 1 }); break;
      case T.WAIT: job(r.a, r.frameId).waits.push({ jobId: r.a, kind: r.b, target: r.c, t0: r.t0, t1: r.t1 }); break;
      case T.PROXY_DEQ: ix.proxyDeq.set(r.a, { t: r.t1 }); break;
      case T.PROXY_CALL: ix.proxyCall.set(r.a, { reqId: r.a, kind: r.b, objId: r.c, t0: r.t0, t1: r.t1 }); break;
      case T.SCOPE: {
        const s = { scopeId: r.a, wrapped: r.b, reqId: r.c, push: r.t0, pop: r.t1 };
        ix.scopes.set(r.a, s);
        if (r.c) ix.scopeByReq.set(r.c, s);
        break;
      }
      case T.SETTLE: ix.settles.set(`${r.b}:${r.a}`, { id: r.a, kind: r.b, result: r.c, t: r.t0 }); break;
      case T.SUBMIT: ix.submits.set(r.a, { submitId: r.a, reqId: r.b, n: r.c, frameId: r.frameId, t0: r.t0, t1: r.t1 }); break;
      case T.SUBMIT_ENC: { const l = ix.submitEnc.get(r.a) || []; l.push(r.b); ix.submitEnc.set(r.a, l); break; }
      case T.GPU_PASS: { const l = ix.gpu.get(r.a) || []; l.push({ pass: r.b, ms: r.t0 }); ix.gpu.set(r.a, l); break; }
      case T.PRESENT: ix.present.set(r.a, { presentId: r.a, reqId: r.b, copyScopeId: r.c, sched: r.t0, fire: r.t1 }); break;
      case T.PRESENT_DONE: ix.presentDone.set(r.a, { copySubmitId: r.b, t: r.t0 }); break;
      case T.GCT: ix.gct.set(r.a, { t0: r.t0, t1: r.t1 }); break;
      default: listPush(ix.unknown, r.type, r);
    }
  }
  for (const [type, l] of ix.unknown) ix.notes.push(`unknown record type ${type}: ${l.length} records preserved, not interpreted`);
  for (const l of ix.ctrDec.values()) l.sort((a, b) => a.t - b.t);
  for (const l of ix.ctrDec.values()) for (const d of l) if (d.jobId) listPush(ix.decsByJob, d.jobId, d);
  for (const j of ix.jobs.values()) if (j.runs) listPush(ix.jobsByTid, j.tid, j);
  for (const l of ix.jobsByTid.values()) l.sort((a, b) => a.start - b.start);

  // A typed counter wait supersedes an untyped kind-6 wait recorded at the same blocking site.
  // Legacy kind-6 waits keep the counter address and site from a matching COUNTER_ACTIVITY record.
  let deduped = 0;
  const matchedActivity = new Set();
  for (const j of ix.jobs.values()) {
    const typed = j.waits.filter((w) => w.kind === WK.COUNTER);
    j.waits = j.waits.filter((w) => {
      if (w.kind !== WK.OTHER) return true;
      const shorter = (x) => Math.max(EPS, Math.min(w.t1 - w.t0, x.t1 - x.t0));
      if (typed.some((x) => overlap(w.t0, w.t1, x.t0, x.t1) > 0.5 * shorter(x))) { deduped++; return false; }
      const act = ix.counterActivity.find((x) => x.jobId === w.jobId && overlap(w.t0, w.t1, x.t0, x.t1) > 0.5 * shorter(x));
      if (act) { w.legacyCounter = { counterAddr: act.counterAddr, site: act.site }; matchedActivity.add(act); }
      return true;
    });
  }
  if (deduped) ix.notes.push(`${deduped} kind-6 WAIT records superseded by CTR_WAIT at the same site`);
  const loneActivity = ix.counterActivity.filter((a) => !matchedActivity.has(a) && !ix.ctrWaits.some((w) => w.jobId === a.jobId && overlap(a.t0, a.t1, w.t0, w.t1) > 0));
  if (loneActivity.length) ix.notes.push(`${loneActivity.length} COUNTER_ACTIVITY records have no WAIT or CTR_WAIT on the same job; preserved, not placed on any path`);
  if (ix.jobMeta.size) ix.notes.push(`JOB_META counter addresses are reported as address associations only; counters are reused at the same address, so they are not used as dependency edges`);

  for (const j of ix.jobs.values()) {
    j.waits.sort((a, b) => a.t0 - b.t0);
    if (j.runs > 1) ix.notes.push(`job ${j.jobId} has ${j.runs} JOB_RUN records; merged to [${j.start}, ${j.end}]`);
    if (j.runs === 0) ix.notes.push(`job ${j.jobId} has ${j.waits.length} WAIT records but no JOB_RUN; its waits are not placed on any path (wrap the waiting code in a JobScope)`);
  }
  return ix;
}

// ---------------------------------------------------------------------------
// Counter-generation audit. A contributor or decrementer can read the previous instance id before the
// zero-to-positive contributor publishes the next one, so an instance tag is a claim, not a fact. Ordering
// against the zero window [dec begin, dec end] decides it where the stamps allow, and otherwise the item is
// reported as ambiguous with its candidate instances and never becomes a dependency edge. These rules are sound
// only if every add and decrement on the counter is instrumented; the count check catches the cases where not.
export function auditCounters(ix) {
  const listPush = (m, k, v) => { const l = m.get(k); if (l) l.push(v); else m.set(k, [v]); };
  const byAddr = new Map();
  for (const a of ix.ctrArm.values()) listPush(byAddr, a.addr, a);
  for (const l of byAddr.values()) l.sort((x, y) => x.t - y.t); // publication order, not id order

  const insts = new Set([...ix.ctrArm.keys(), ...ix.ctrContrib.keys(), ...ix.ctrDec.keys(), ...ix.ctrWaits.map((w) => w.target)]);
  insts.delete(0);
  // Later generations at the same address that an item spanning [t0, t1] could really belong to. A generation's
  // publication can lag the item (the publisher may be preempted after its fetch_add), so the bound is the BEGIN
  // stamp of its arming add, which precedes that fetch_add. A generation already closed before the item began
  // is excluded.
  const firstZero = (inst) => (ix.ctrDec.get(inst) || []).filter((d) => d.after === 0).sort((a, b) => a.t0 - b.t0)[0] || null;
  const armBegin = (a) => Math.min(a.t, ...(ix.ctrContrib.get(a.inst) || []).map((c) => c.t0));
  const laterArmed = (inst, t1, t0 = -Infinity) => {
    const arm = ix.ctrArm.get(inst);
    if (!arm) return [];
    return (byAddr.get(arm.addr) || []).filter((a) => {
      if (a.inst === inst || a.t <= arm.t) return false;
      if (armBegin(a) > t1 + EPS) return false;
      const z = firstZero(a.inst);
      return !z || z.t1 >= t0 - EPS;
    }).map((a) => a.inst);
  };

  const byInst = new Map();
  const ambiguities = [];
  for (const inst of insts) {
    const arm = ix.ctrArm.get(inst) || null;
    const contribs = [...(ix.ctrContrib.get(inst) || [])].sort((a, b) => a.t0 - b.t0);
    const decs = [...(ix.ctrDec.get(inst) || [])].sort((a, b) => a.t0 - b.t0);
    const zero = decs.find((d) => d.after === 0) || null;
    const ambiguous = new Map(); // record -> reason
    const flag = (rec, reason, kind) => {
      ambiguous.set(rec, reason);
      ambiguities.push({ inst, kind, reason, jobId: rec.jobId, tid: rec.tid, t0: rec.t0, t1: rec.t1, candidates: [inst, ...laterArmed(inst, rec.t1, rec.t0)] });
    };
    if (zero) {
      for (const c of contribs) {
        if (c.t0 > zero.t1 + EPS) flag(c, 'add_after_zero', 'contribution');
        else if (c.t1 > zero.t0 + EPS && c.t0 < zero.t1 - EPS) flag(c, 'add_overlaps_zero', 'contribution');
      }
      for (const d of decs) {
        if (d === zero) continue;
        if (d.t0 > zero.t1 + EPS) flag(d, 'dec_after_zero', 'decrement');
        else if (d.t1 > zero.t0 + EPS && d.t0 < zero.t1 - EPS) flag(d, 'dec_overlaps_zero', 'decrement');
      }
    }

    // A generation armed before the previous one at this address could have reached zero is impossible under
    // counter semantics: competing creators (pre-CAS runtime) or an uninstrumented path.
    let overlapWith = null;
    if (arm) {
      const peers = byAddr.get(arm.addr) || [];
      // A predecessor that only ever saw a wait (an implicit instance on a never-incremented counter) has no
      // generation to overlap; only one with recorded contributions or decrements can block the next arm.
      const live = (a) => (ix.ctrContrib.get(a.inst) || []).length + (ix.ctrDec.get(a.inst) || []).length > 0;
      const prev = peers.slice(0, peers.indexOf(arm)).reverse().find(live);
      if (prev) {
        const pz = (ix.ctrDec.get(prev.inst) || []).filter((d) => d.after === 0).sort((a, b) => a.t0 - b.t0)[0];
        if (!pz || pz.t0 > arm.t + EPS) overlapWith = prev.inst;
      }
    }

    let zeroStatus = 'none';
    const sumDelta = (l) => l.reduce((a, c) => a + c.delta, 0);
    const certainContrib = sumDelta(contribs.filter((c) => !ambiguous.has(c)));
    const overlapContrib = sumDelta(contribs.filter((c) => ambiguous.get(c) === 'add_overlaps_zero'));
    const certainDecs = zero ? 1 + decs.filter((d) => d !== zero && !ambiguous.has(d) && d.t0 <= zero.t1 + EPS).length : 0;
    const overlapDecs = decs.filter((d) => ambiguous.get(d) === 'dec_overlaps_zero').length;
    if (zero) {
      if (!arm) zeroStatus = 'generation_started_before_capture';
      else if (overlapWith) zeroStatus = 'generation_overlap';
      else if (certainContrib + overlapContrib < certainDecs || certainDecs + overlapDecs < certainContrib) zeroStatus = 'count_mismatch';
      else zeroStatus = 'verified';
    }
    if (overlapWith) ambiguities.push({ inst, kind: 'generation', reason: 'generation_overlap', previous: overlapWith, t0: arm.t, t1: arm.t, candidates: [overlapWith, inst] });
    if (zero && zeroStatus !== 'verified') ambiguities.push({ inst, kind: 'zero', reason: zeroStatus, jobId: zero.jobId, tid: zero.tid, t0: zero.t0, t1: zero.t1, candidates: [inst, ...laterArmed(inst, zero.t1, zero.t0)] });

    byInst.set(inst, {
      inst, addr: arm ? arm.addr : null, arm, zero, zeroStatus,
      certainDecs: decs.filter((d) => !ambiguous.has(d) && (!zero || d.t0 <= zero.t1 + EPS)),
      ambiguous,
      count: { contributedCertain: certainContrib, contributedOverlapping: overlapContrib, decrementedCertain: certainDecs, decrementedOverlapping: overlapDecs },
    });
  }
  return { byInst, ambiguities, laterArmed };
}

class Graph {
  constructor(frame) {
    this.frame = frame;
    this.nodes = new Map();
    this.inconsistencies = [];
    this.unresolved = [];
    this.addNode(START, 'start', frame.start, frame.start);
    this.addNode(END, 'end', frame.end, frame.end);
  }
  addNode(id, cat, start, end, meta = {}) {
    if (this.nodes.has(id)) return this.nodes.get(id);
    if (end < start) { this.inconsistencies.push({ node: id, kind: 'negative_duration', ms: start - end }); end = start; }
    const n = { id, cat, start, end, dur: end - start, edges: [], succ: [], meta };
    this.nodes.set(id, n);
    return n;
  }
  addEdge(toId, fromId, at) {
    const to = this.nodes.get(toId), from = this.nodes.get(fromId);
    if (!to || !from || toId === fromId) return;
    if (to.edges.some((e) => e.from === fromId && e.at === at)) return;
    to.edges.push({ from: fromId, at });
    from.succ.push(toId);
  }
  has(id) { return this.nodes.has(id); }
  get(id) { return this.nodes.get(id); }
}

function clamp(x, lo, hi) { return Math.max(lo, Math.min(hi, x)); }

function buildGraph(ix, frame, opts) {
  const g = new Graph(frame);
  const F = frame.frameId;
  const jobs = [...ix.jobs.values()].filter((j) => j.frameId === F && j.end >= j.start);
  const jobNodes = new Map();
  const pendingJobEdges = [];
  const pending = []; // edge closures that need every job node to exist first
  g.counterWaits = [];
  const jobName = (id) => opts.jobNames?.[ix.jobMeta.get(id)?.nameId] || '';
  const siteName = (id) => opts.siteNames?.[id] || '';
  const rel = (t) => (t === null || t === undefined ? null : t - frame.start);

  // Jobs the waiting thread ran inside a blocking wait. Same-thread intervals are disjoint at top level,
  // so their durations can be summed without double counting.
  const helpsFor = (w, waiter) => {
    if (waiter.tid === null) return [];
    const c = (ix.jobsByTid.get(waiter.tid) || []).filter((h) => h !== waiter && h.end >= h.start && h.start >= w.t0 - EPS && h.end <= w.t1 + EPS);
    return c.filter((h) => !c.some((o) => o !== h && o.start <= h.start + EPS && h.end <= o.end + EPS && (o.end - o.start) > (h.end - h.start)));
  };

  // Gate attribution: a job that decrements counter generation X and started after exactly one release of a
  // waiting job accumulating on X was that released job. Several candidate releases are reported, not chosen.
  const gateMemo = new Map();
  const gateFor = (j) => {
    if (gateMemo.has(j.jobId)) return gateMemo.get(j.jobId);
    let out = { edge: null, candidates: [] };
    const insts = [...new Set((ix.decsByJob.get(j.jobId) || []).map((d) => d.inst).filter(Boolean))];
    for (const inst of insts) {
      const before = (ix.gatesByAccum.get(inst) || []).filter((gt) => gt.t <= j.start + EPS);
      out.candidates.push(...before);
    }
    if (out.candidates.length === 1 && out.candidates[0].jobId) out.edge = out.candidates[0];
    gateMemo.set(j.jobId, out);
    return out;
  };

  // Pull in jobs from any frame that this frame's counter waits depend on or ran cooperatively.
  const inFrame = new Set(jobs);
  const include = (j) => { if (j && j.runs && j.end >= j.start && !inFrame.has(j)) { inFrame.add(j); jobs.push(j); } };
  for (let k = 0; k < jobs.length; k++) {
    for (const w of jobs[k].waits) {
      if (w.kind === WK.COUNTER || w.kind === WK.OTHER) for (const h of helpsFor(w, jobs[k])) include(h);
      // Instance zero means attribution failed. It must never join unrelated counters.
      if (w.kind === WK.COUNTER && w.target) for (const d of ix.ctrDec.get(w.target) || []) include(ix.jobs.get(d.jobId));
    }
    // A job released from a waiting list depends on the job that drove the releasing counter to zero.
    const gate = gateFor(jobs[k]);
    if (gate.edge) include(ix.jobs.get(gate.edge.jobId));
  }

  const nodeAt = (jobId, t) => {
    const jn = jobNodes.get(jobId);
    if (!jn) return null;
    let host = null;
    for (const s of jn.seq) for (const n of [s.first, s.last]) if (n.start - EPS <= t && t <= n.end + EPS) host = n;
    return host || jn.last;
  };

  const mainCall = (reqId) => {
    const id = `call:${reqId}`;
    if (g.has(id)) return g.get(id);
    const c = ix.proxyCall.get(reqId);
    if (!c) return null;
    const name = opts.callKinds?.[c.kind] || `kind${c.kind}`;
    return g.addNode(id, `main_call:${name}`, c.t0, c.t1, { reqId, objId: c.objId });
  };

  const scopeSettle = (scopeId) => {
    const id = `settle:scope:${scopeId}`;
    if (g.has(id)) return g.get(id);
    const settle = ix.settles.get(`${SK.SCOPE}:${scopeId}`);
    if (!settle) return null;
    const s = ix.scopes.get(scopeId);
    if (!s) {
      g.unresolved.push({ what: 'scope_without_push_pop', id: scopeId });
      return g.addNode(id, 'unresolved:scope_no_pushpop', settle.t, settle.t, { scopeId });
    }
    const n = g.addNode(id, 'scope_settle', s.pop, settle.t, { scopeId, wrapped: s.wrapped, reqId: s.reqId, result: settle.result });
    const call = s.reqId ? mainCall(s.reqId) : null;
    if (call) g.addEdge(id, call.id, clamp(s.pop, call.start, call.end));
    return n;
  };

  const submitNode = (submitId) => {
    const id = `submit:${submitId}`;
    if (g.has(id)) return g.get(id);
    const s = ix.submits.get(submitId);
    if (!s) return null;
    const n = g.addNode(id, 'main_submit', s.t0, s.t1, { submitId, encoders: ix.submitEnc.get(submitId) || [], gpuMs: (ix.gpu.get(submitId) || []).reduce((a, p) => a + p.ms, 0) });
    const call = s.reqId ? mainCall(s.reqId) : null;
    if (call) g.addEdge(id, call.id, clamp(s.t0, call.start, call.end));
    return n;
  };

  const submitDone = (submitId) => {
    const id = `done:submit:${submitId}`;
    if (g.has(id)) return g.get(id);
    const settle = ix.settles.get(`${SK.SUBMIT}:${submitId}`);
    if (!settle) return null;
    const sub = submitNode(submitId);
    const gpuMs = (ix.gpu.get(submitId) || []).reduce((a, p) => a + p.ms, 0);
    if (!sub) {
      g.unresolved.push({ what: 'submit_settle_without_submit', id: submitId });
      return g.addNode(id, 'unresolved:submit_no_record', settle.t, settle.t, { submitId });
    }
    const n = g.addNode(id, 'gpu_completion', sub.end, settle.t, { submitId, gpuMs });
    g.addEdge(id, sub.id, sub.end);
    return n;
  };

  const presentChain = (presentId) => {
    const id = `present:${presentId}`;
    if (g.has(id)) return g.get(id);
    const p = ix.present.get(presentId);
    if (!p) return null;
    const raf = g.addNode(`raf:${presentId}`, 'raf_wait', p.sched, p.fire, { presentId });
    const call = p.reqId ? mainCall(p.reqId) : null;
    if (call) g.addEdge(raf.id, call.id, clamp(p.sched, call.start, call.end));
    let last = raf;
    const gct = ix.gct.get(presentId);
    if (gct) {
      const n = g.addNode(`gct:${presentId}`, 'get_current_texture', gct.t0, gct.t1, { presentId });
      g.addEdge(n.id, last.id, last.end); last = n;
    }
    const done = ix.presentDone.get(presentId);
    if (done) {
      const n = g.addNode(`copy:${presentId}`, 'present_copy', last.end, done.t, { presentId, copySubmitId: done.copySubmitId });
      g.addEdge(n.id, last.id, last.end); last = n;
    }
    const fin = g.addNode(id, 'present_done', last.end, last.end, { presentId });
    g.addEdge(id, last.id, last.end);
    return fin;
  };

  const unresolvedWait = (w, i, why) => {
    g.unresolved.push({ what: why, jobId: w.jobId, kind: WAIT_NAME[w.kind], target: w.target, ms: w.t1 - w.t0 });
    return g.addNode(`wait:${w.jobId}:${i}`, `unresolved:wait:${WAIT_NAME[w.kind]}`, w.t0, w.t1, { target: w.target });
  };

  const chain = (prefix, cats, times, meta) => {
    const out = [];
    let cursor = times[0];
    for (let k = 0; k < cats.length; k++) {
      let end = times[k + 1];
      if (end < cursor) { g.inconsistencies.push({ node: `${prefix}:${cats[k]}`, kind: 'non_monotonic', ms: cursor - end }); end = cursor; }
      const n = g.addNode(`${prefix}:${cats[k]}`, cats[k], cursor, end, meta);
      if (out.length) g.addEdge(n.id, out[out.length - 1].id, out[out.length - 1].end);
      out.push(n);
      cursor = end;
    }
    return out;
  };

  // Splits a wait whose release cause is unknown into blocked segments and the jobs the waiter ran
  // cooperatively in between. Only the blocked segments are reported as unresolved.
  const interleaveUnresolved = (w, i, waiter, prevNode, cat, why, meta) => {
    const prefix = `wait:${w.jobId}:${i}`;
    const helps = helpsFor(w, waiter);
    const blocked = [];
    let cursor = w.t0;
    for (let k = 0; k <= helps.length; k++) {
      const end = k < helps.length ? Math.max(cursor, helps[k].start) : Math.max(cursor, w.t1);
      blocked.push(g.addNode(`${prefix}:blocked${k}`, cat, cursor, end, meta));
      if (k < helps.length) cursor = Math.max(cursor, helps[k].end);
    }
    pending.push(() => {
      for (let k = 0; k < helps.length; k++) {
        const hn = jobNodes.get(helps[k].jobId);
        if (hn) {
          g.addEdge(hn.first.id, blocked[k].id, blocked[k].end);
          g.addEdge(blocked[k + 1].id, hn.last.id, hn.last.end);
        } else g.addEdge(blocked[k + 1].id, blocked[k].id, blocked[k].end);
      }
    });
    const blockedMs = blocked.reduce((a, n) => a + n.dur, 0);
    g.unresolved.push({ what: why, jobId: w.jobId, kind: WAIT_NAME[w.kind], target: w.target, ms: blockedMs, waitMs: w.t1 - w.t0, cooperativeMs: (w.t1 - w.t0) - blockedMs, ...meta });
    return { first: blocked[0], last: blocked[blocked.length - 1], helps, blockedMs };
  };

  const reportWait = (w, waiter, helps, extra) => {
    const helpMs = helps.reduce((a, h) => a + overlap(h.start, h.end, w.t0, w.t1), 0);
    g.counterWaits.push({
      waiterJobId: w.jobId, waiterTid: waiter.tid, kind: WAIT_NAME[w.kind],
      at: rel(w.t0), resumeAt: rel(w.t1), waitMs: w.t1 - w.t0,
      cooperativeMs: helpMs, notCooperativeMs: (w.t1 - w.t0) - helpMs,
      helps: helps.map((h) => ({ jobId: h.jobId, name: jobName(h.jobId), at: rel(h.start), ms: h.end - h.start })),
      ...extra,
    });
  };

  // A decrement with no job context came from a thread without a JobScope; name the thread so the
  // integration can wrap it, instead of implying a job with a missing run record.
  const decMissing = (d, w, inst, ms) => d.jobId
    ? { what: 'counter_contributor_without_run', jobId: w.jobId, inst, target: d.jobId, tid: d.tid, ms }
    : { what: 'counter_decrement_outside_job', jobId: w.jobId, inst, tid: d.tid, ms };

  const waitNodes = (w, i, prevNode, waiter) => {
    const prefix = `wait:${w.jobId}:${i}`;
    const wake = (from, u) => {
      u = clamp(u, w.t0, w.t1);
      const n = g.addNode(`${prefix}:wake`, `wake:${WAIT_NAME[w.kind]}`, u, w.t1, { target: w.target });
      if (from) g.addEdge(n.id, from.id, clamp(u, from.start, from.end));
      return n;
    };
    switch (w.kind) {
      case WK.PROXY: {
        const call = mainCall(w.target);
        if (!call) { const n = unresolvedWait(w, i, 'proxy_without_main_records'); return { first: n, last: n }; }
        const deq = ix.proxyDeq.get(w.target);
        const parts = deq
          ? chain(prefix, ['proxy_delivery', 'proxy_dispatch'], [w.t0, deq.t, call.start], { reqId: w.target })
          : chain(prefix, ['proxy_delivery'], [w.t0, call.start], { reqId: w.target });
        g.addEdge(call.id, parts[parts.length - 1].id, parts[parts.length - 1].end);
        const s = ix.scopeByReq.get(w.target);
        const settle = s ? ix.settles.get(`${SK.SCOPE}:${s.scopeId}`) : null;
        let from = call, u = call.end;
        if (settle && settle.t > call.end && settle.t <= w.t1) { from = scopeSettle(s.scopeId); u = settle.t; }
        return { first: parts[0], last: wake(from, u) };
      }
      case WK.SCOPE: {
        const sn = scopeSettle(w.target);
        if (!sn) { const n = unresolvedWait(w, i, 'scope_without_settle'); return { first: n, last: n }; }
        const n = wake(sn, sn.end);
        return { first: n, last: n };
      }
      case WK.SUBMIT_DONE: {
        const dn = submitDone(w.target);
        if (!dn) { const n = unresolvedWait(w, i, 'submit_without_completion_settle'); return { first: n, last: n }; }
        const n = wake(dn, dn.end);
        return { first: n, last: n };
      }
      case WK.JOB: {
        const tj = ix.jobs.get(w.target);
        if (!tj || tj.end < tj.start) { const n = unresolvedWait(w, i, 'job_without_run_record'); return { first: n, last: n }; }
        const n = wake(null, tj.end);
        pendingJobEdges.push({ node: n, target: w.target, w, i });
        return { first: n, last: n };
      }
      case WK.PRESENT: {
        const pn = presentChain(w.target);
        if (!pn) { const n = unresolvedWait(w, i, 'present_without_records'); return { first: n, last: n }; }
        const p = ix.present.get(w.target);
        const settle = p.copyScopeId ? ix.settles.get(`${SK.SCOPE}:${p.copyScopeId}`) : null;
        let from = pn, u = pn.end;
        if (settle && settle.t > pn.end && settle.t <= w.t1) {
          const sn = scopeSettle(p.copyScopeId);
          if (sn) {
            if (!sn.edges.length) {
              const host = g.has(`copy:${w.target}`) ? g.get(`copy:${w.target}`) : pn;
              g.addEdge(sn.id, host.id, clamp(sn.start, host.start, host.end));
            }
            from = sn; u = sn.end;
          }
        }
        const n = wake(from, u);
        return { first: n, last: n };
      }
      case WK.COUNTER: {
        const inst = w.target;
        const arm = ix.ctrArm.get(inst);
        const meta = { inst, site: w.site, siteName: siteName(w.site), counterAddr: arm ? arm.addr : null };
        const decs = ix.ctrDec.get(inst) || [];
        const audit = ix.counterAudit?.byInst.get(inst) || null;
        const wakeRec = ix.ctrWake.get(`${inst}:${w.jobId}:${w.t1}`);
        const tw = wakeRec ? clamp(wakeRec.t, w.t0, w.t1) : null;
        const limit = (tw ?? w.t1) + EPS;
        const amb = (rec) => audit?.ambiguous.get(rec) || null;
        const contributions = (ix.ctrContrib.get(inst) || []).map((c) => {
          const j = ix.jobs.get(c.jobId);
          const dec = decs.find((d) => d.jobId === c.jobId);
          return {
            jobId: c.jobId, name: jobName(c.jobId), delta: c.delta, addedAt: rel(c.t0), addedEndAt: rel(c.t1),
            runAt: j && j.runs ? rel(j.start) : null, runEndAt: j && j.runs ? rel(j.end) : null,
            decAt: dec ? rel(dec.t0) : null, decEndAt: dec ? rel(dec.t1) : null, valueAfter: dec ? (dec.after === CTR_NEGATIVE ? -1 : dec.after) : null,
            ambiguous: amb(c), decAmbiguous: dec ? amb(dec) : null,
          };
        });
        const uncontributedDecs = decs.filter((d) => !(ix.ctrContrib.get(inst) || []).some((c) => c.jobId === d.jobId))
          .map((d) => ({ jobId: d.jobId, name: jobName(d.jobId), decAt: rel(d.t0), decEndAt: rel(d.t1), valueAfter: d.after === CTR_NEGATIVE ? -1 : d.after, ambiguous: amb(d) }));
        for (const d of decs) if (d.after === CTR_NEGATIVE) g.inconsistencies.push({ kind: 'counter_negative', inst, jobId: d.jobId, ms: 0 });
        const base = {
          ...meta, implicitArm: arm ? arm.implicit : null, contributions, uncontributedDecs,
          zeroStatus: audit ? audit.zeroStatus : 'none', countCheck: audit ? audit.count : null,
          ambiguities: (ix.counterAudit?.ambiguities || []).filter((a) => a.inst === inst).map((a) => ({ ...a, t0: rel(a.t0), t1: rel(a.t1) })),
        };

        // Only a verified zero becomes a dependency. A wait that began after its tagged instance reached zero,
        // while a later generation at the same address was armed before it resumed, may be waiting on that
        // later generation: report the candidates, do not pick one.
        const z = audit && audit.zeroStatus === 'verified' && audit.zero.t0 <= limit ? audit.zero : null;
        const stale = z && w.t0 > z.t1 + EPS ? ix.counterAudit.laterArmed(inst, w.t1, w.t0) : [];
        let why = null;
        if (!inst) why = 'counter_wait_without_instance';
        else if (stale.length) why = 'counter_wait_generation_ambiguous';
        else if (!z) why = audit?.zero ? `counter_zero_${audit.zeroStatus}` : 'counter_zero_not_recorded';
        if (why) {
          const r = interleaveUnresolved(w, i, waiter, prevNode, 'unresolved:wait:counter', why, meta);
          reportWait(w, waiter, r.helps, { ...base, zeroAt: audit?.zero ? rel(audit.zero.t0) : null, zeroBy: null, candidateInstances: stale.length ? [inst, ...stale] : undefined, wakeAt: rel(tw), unresolved: why, blockedUnattributedMs: r.blockedMs });
          return r;
        }
        const zero = z;

        const helps = helpsFor(w, waiter);
        let lastHelp = null;
        for (const h of helps) if (h.end <= limit) lastHelp = h;
        // The zero transition happened somewhere in [dec begin, dec end], and no later than the waiter's wake.
        // Preemption before the atomic op is inside this window, so it is reported as one unattributed interval.
        const windowEnd = Math.max(zero.t0, Math.min(zero.t1, tw ?? w.t1));
        const zeroWin = g.addNode(`${prefix}:zero_window`, 'unresolved:counter_zero_window', zero.t0, windowEnd, { inst, jobId: zero.jobId, tid: zero.tid });
        const u = clamp(Math.max(zeroWin.end, lastHelp ? lastHelp.end : -Infinity), w.t0, tw ?? w.t1);
        const wakeN = g.addNode(`${prefix}:wake`, 'wake:counter', u, tw ?? w.t1, { ...meta, wakeStamped: tw !== null });
        g.addEdge(wakeN.id, zeroWin.id, zeroWin.end);
        let last = wakeN;
        if (tw !== null) {
          last = g.addNode(`${prefix}:resume`, 'counter_resume', tw, w.t1, meta);
          g.addEdge(last.id, wakeN.id, tw);
        }
        if (tw === null) g.unresolved.push({ what: 'counter_wake_not_stamped', jobId: w.jobId, inst, ms: 0, note: 'wake and resume are merged in wake:counter' });
        const zeroAfterWaitStart = zero.t0 > w.t0;
        pending.push(() => {
          // Zero is the AND of every certain decrement in this generation: each is a prerequisite of the wake.
          // Ambiguous decrements are reported by the audit and get no edge.
          for (const d of audit.certainDecs) {
            if (d === zero) continue;
            const n = d.jobId ? nodeAt(d.jobId, d.t0) : null;
            if (n) g.addEdge(wakeN.id, n.id, clamp(d.t0, n.start, n.end));
            else g.unresolved.push(decMissing(d, w, inst, 0));
          }
          const src = zero.jobId ? nodeAt(zero.jobId, zero.t0) : null;
          if (src) g.addEdge(zeroWin.id, src.id, clamp(zero.t0, src.start, src.end));
          else if (zeroAfterWaitStart) {
            const cat = zero.jobId ? 'unresolved:counter_contributor' : 'unresolved:counter_decrement_outside_job';
            const n = g.addNode(`${prefix}:zero_src`, cat, w.t0, Math.max(w.t0, zero.t0), { inst, jobId: zero.jobId, tid: zero.tid });
            if (prevNode) g.addEdge(n.id, prevNode.id, prevNode.end);
            g.addEdge(zeroWin.id, n.id, n.end);
            g.unresolved.push(decMissing(zero, w, inst, n.dur));
          }
          if (lastHelp) { const hn = jobNodes.get(lastHelp.jobId); if (hn) g.addEdge(wakeN.id, hn.last.id, hn.last.end); }
          if (prevNode) for (const h of helps) { const hn = jobNodes.get(h.jobId); if (hn) g.addEdge(hn.first.id, prevNode.id, prevNode.end); }
        });
        // Prerequisites of the zero-reaching contributor are reported so the next step is a lookup, not a guess.
        const zj = ix.jobs.get(zero.jobId);
        reportWait(w, waiter, helps, {
          ...base,
          zeroAt: rel(zero.t0), zeroWindowEndAt: rel(zero.t1), zeroWindowMs: zero.t1 - zero.t0, zeroBy: { jobId: zero.jobId, name: jobName(zero.jobId), runAt: zj && zj.runs ? rel(zj.start) : null, runEndAt: zj && zj.runs ? rel(zj.end) : null, spawnedBy: ix.spawns.get(zero.jobId)?.parent ?? null, deps: ix.deps.get(zero.jobId) || [] },
          completedBeforeWait: !zeroAfterWaitStart,
          wakeAt: rel(tw),
          wakeAfterReleaseMs: tw !== null ? tw - u : null,
          resumeAfterWakeMs: tw !== null ? w.t1 - tw : null,
          lastHelpEndAt: lastHelp ? rel(lastHelp.end) : null,
          unresolved: null,
        });
        return { first: wakeN, last };
      }
      default: {
        // Untyped wait. If a COUNTER_ACTIVITY record matched it, the counter address and site are known but
        // the release cause is not: no contributor or zero record exists for an address-only counter.
        const lc = w.legacyCounter;
        const meta = lc ? { counterAddr: lc.counterAddr, site: lc.site, siteName: siteName(lc.site), target: w.target } : { target: w.target };
        const why = lc ? 'counter_wait_untyped_release_unknown' : 'wait_kind_other_untyped';
        const r = interleaveUnresolved(w, i, waiter, prevNode, lc ? 'unresolved:wait:counter' : 'unresolved:wait:other', why, meta);
        const assoc = lc ? [...ix.jobMeta.values()].filter((m) => m.counterAddr === lc.counterAddr && m.t1 >= w.t0 - EPS && m.t0 <= w.t1 + EPS)
          .map((m) => ({ jobId: m.jobId, name: jobName(m.jobId), at: rel(m.t0), endAt: rel(m.t1) })) : [];
        reportWait(w, waiter, r.helps, { ...meta, unresolved: why, blockedUnattributedMs: r.blockedMs, addressAssociatedJobs: assoc, addressAssociationNote: lc ? 'JOB_META address match only: role (signal vs wait) and counter generation are unverified' : undefined });
        return r;
      }
    }
  };

  for (const j of jobs) {
    const seq = [];
    let cursor = j.start;
    let k = 0;
    for (let i = 0; i < j.waits.length; i++) {
      const w = j.waits[i];
      if (w.t0 < cursor) { g.inconsistencies.push({ node: `wait:${j.jobId}:${i}`, kind: 'wait_overlaps_previous', ms: cursor - w.t0 }); }
      if (w.t0 > cursor) seq.push({ first: g.addNode(`run:${j.jobId}:${k++}`, `run:w${j.workerId}`, cursor, w.t0, { jobId: j.jobId }) });
      const prev = seq.length ? (seq[seq.length - 1].last || seq[seq.length - 1].first) : null;
      seq.push(waitNodes(w, i, prev, j));
      cursor = Math.max(cursor, w.t1);
    }
    if (j.end > cursor || seq.length === 0) seq.push({ first: g.addNode(`run:${j.jobId}:${k++}`, `run:w${j.workerId}`, cursor, Math.max(j.end, cursor), { jobId: j.jobId }) });
    for (const s of seq) if (!s.last) s.last = s.first;
    for (let i = 1; i < seq.length; i++) g.addEdge(seq[i].first.id, seq[i - 1].last.id, seq[i - 1].last.end);
    jobNodes.set(j.jobId, { first: seq[0].first, last: seq[seq.length - 1].last, seq });
  }

  for (const j of jobs) {
    const jn = jobNodes.get(j.jobId);
    for (const d of ix.deps.get(j.jobId) || []) {
      const dn = jobNodes.get(d);
      if (dn) g.addEdge(jn.first.id, dn.last.id, dn.last.end);
      else g.unresolved.push({ what: 'dep_job_not_in_frame', jobId: j.jobId, target: d });
    }
    const sp = ix.spawns.get(j.jobId);
    if (sp) {
      const pn = jobNodes.get(sp.parent);
      if (pn) {
        let host = null;
        for (const s of pn.seq) for (const n of [s.first, s.last]) if (n.start <= sp.t && sp.t <= n.end) host = n;
        if (!host) host = pn.seq.find((s) => s.first.start > sp.t)?.first || pn.last;
        g.addEdge(jn.first.id, host.id, clamp(sp.t, host.start, host.end));
        if (sp.fromEnqueue) jn.first.meta.enqueueHost = host.id;
      } else g.unresolved.push({ what: 'spawn_parent_not_in_frame', jobId: j.jobId, target: sp.parent });
    }
    const gate = gateFor(j);
    if (gate.edge) {
      const src = nodeAt(gate.edge.jobId, gate.edge.t);
      if (src) {
        g.addEdge(jn.first.id, src.id, clamp(gate.edge.t, src.start, src.end));
        jn.first.meta.gateHost = src.id;
        jn.first.meta.gate = { releasedInst: gate.edge.released, by: gate.edge.jobId };
      }
    } else if (gate.candidates.length > 1) {
      g.unresolved.push({ what: 'gate_release_ambiguous', jobId: j.jobId, ms: 0, candidates: gate.candidates.map((c) => ({ released: c.released, by: c.jobId })) });
    }
  }

  for (const fn of pending) fn();

  for (const p of pendingJobEdges) {
    const tj = jobNodes.get(p.target);
    if (tj) g.addEdge(p.node.id, tj.last.id, tj.last.end);
    else {
      g.unresolved.push({ what: 'job_target_not_in_frame', jobId: p.w.jobId, target: p.target, ms: p.w.t1 - p.w.t0 });
      p.node.cat = 'unresolved:wait:job';
      p.node.start = p.w.t0; p.node.dur = p.node.end - p.node.start;
    }
  }

  for (const n of g.nodes.values()) {
    if (n.id === START || n.id === END) continue;
    if (n.edges.length === 0) g.addEdge(n.id, START, frame.start);
  }
  for (const n of g.nodes.values()) {
    if (n.id === START || n.id === END) continue;
    if (n.succ.length === 0) g.addEdge(END, n.id, n.end);
  }
  return g;
}

function gapCategory(from, to) {
  if (from.id === START) return 'unresolved:root_gap';
  // Enqueue-to-start of a counter contributor: queueing, or a gate the trace does not record. Not proven scheduling.
  if (to.meta?.enqueueHost === from.id) return 'unresolved:enqueue_to_start';
  // Released from a waiting list at the edge time; the gap is queueing until a worker picked it up.
  if (to.meta?.gateHost === from.id) return 'gate_queue';
  if (to.id === END) return 'unresolved:frame_end_gap';
  if (to.cat.startsWith('run:') && from.cat.startsWith('run:')) return 'sched';
  if (to.cat.startsWith('run:') && from.cat.startsWith('wake:')) return 'sched';
  if (to.cat.startsWith('run:') && from.cat === 'counter_resume') return 'sched';
  // Between the contributor's last recorded run segment and its decrement stamp (job-system epilogue).
  if (to.cat === 'wake:counter' && from.cat.startsWith('run:')) return 'counter_signal';
  if (to.cat === 'unresolved:counter_zero_window' && from.cat.startsWith('run:')) return 'counter_signal';
  return `unresolved:gap:${from.cat}->${to.cat}`;
}

export function walkCriticalPath(g) {
  const { frame } = g;
  const path = [];
  const seen = new Set();
  let cur = g.get(END), at = frame.end;
  while (true) {
    if (seen.has(cur.id)) { g.inconsistencies.push({ node: cur.id, kind: 'cycle' }); break; }
    seen.add(cur.id);
    if (cur.start < frame.start && cur.id !== START) {
      if (at > frame.start) path.push({ cat: 'prev_frame_carry', ms: at - frame.start, id: cur.id, meta: cur.meta });
      break;
    }
    const portion = at - cur.start;
    if (portion > 0) path.push({ cat: cur.cat, ms: portion, id: cur.id, meta: cur.meta });
    if (cur.id === START || cur.edges.length === 0) break;
    let e = cur.edges[0];
    for (const x of cur.edges) if (x.at > e.at) e = x;
    const from = g.get(e.from);
    let a = e.at;
    if (a > cur.start) { g.inconsistencies.push({ node: cur.id, pred: e.from, kind: 'pred_after_start', ms: a - cur.start }); a = cur.start; }
    else if (a < cur.start) {
      const cat = gapCategory(from, cur);
      path.push({ cat, ms: cur.start - a, id: `gap:${from.id}->${cur.id}`, meta: { from: from.id, to: cur.id } });
    }
    cur = from; at = a;
    if (cur.id === START) break;
  }
  path.reverse();
  return path;
}

function topoOrder(g) {
  const indeg = new Map();
  for (const n of g.nodes.values()) indeg.set(n.id, n.edges.length);
  const q = [...g.nodes.values()].filter((n) => n.edges.length === 0).map((n) => n.id);
  const order = [];
  while (q.length) {
    const id = q.shift();
    order.push(id);
    for (const s of g.get(id).succ) {
      indeg.set(s, indeg.get(s) - 1);
      if (indeg.get(s) === 0) q.push(s);
    }
  }
  if (order.length !== g.nodes.size) g.inconsistencies.push({ kind: 'cycle_in_graph', ms: 0 });
  return order;
}

function annotateLags(g) {
  for (const n of g.nodes.values()) {
    if (!n.edges.length) continue;
    let best = n.edges[0];
    for (const e of n.edges) if (e.at > best.at) best = e;
    for (const e of n.edges) { e.off = Math.max(0, e.at - g.get(e.from).start); e.lag = 0; }
    best.lag = Math.max(0, n.start - best.at);
  }
}

export function schedule(g, durOverride = new Map()) {
  const order = g.order || (g.order = topoOrder(g));
  const es = new Map();
  const dur = (id) => durOverride.has(id) ? durOverride.get(id) : g.get(id).dur;
  const lag = (e, id) => { const k = `gap:${e.from}->${id}`; return durOverride.has(k) ? durOverride.get(k) : e.lag; };
  for (const id of order) {
    const n = g.get(id);
    let v = 0;
    for (const e of n.edges) v = Math.max(v, es.get(e.from) + Math.min(e.off, dur(e.from)) + lag(e, id));
    es.set(id, v);
  }
  const end = es.get(END);
  const ls = new Map();
  for (let i = order.length - 1; i >= 0; i--) {
    const id = order[i];
    const n = g.get(id);
    let v = id === END ? end : Infinity;
    for (const sid of n.succ) {
      const s = g.get(sid);
      for (const e of s.edges) if (e.from === id) v = Math.min(v, ls.get(sid) - Math.min(e.off, dur(id)) - lag(e, sid));
    }
    if (v === Infinity) v = end - dur(id);
    ls.set(id, v);
  }
  return { es, ls, end };
}

const WAITISH = /^(proxy_delivery|proxy_dispatch|scope_settle|gpu_completion|wake:|raf_wait|sched|wait:other|unresolved:|prev_frame_carry|main_call:|main_submit|get_current_texture|present_copy|counter_resume|counter_signal|gate_queue)/;

export function analyzeFrame(ix, frame, opts = {}) {
  const g = buildGraph(ix, frame, opts);
  annotateLags(g);
  const path = walkCriticalPath(g);
  const frameMs = frame.end - frame.start;
  const byCat = {};
  for (const p of path) byCat[p.cat] = (byCat[p.cat] || 0) + p.ms;
  const pathMs = path.reduce((a, p) => a + p.ms, 0);
  const base = schedule(g);
  const critical = [];
  for (const p of path) {
    if (!WAITISH.test(p.cat)) continue;
    const isGap = p.id.startsWith('gap:');
    if (!isGap && !g.has(p.id)) continue;
    const anchor = isGap ? p.meta.to : p.id;
    const what = schedule(g, new Map([[p.id, 0]]));
    critical.push({ id: p.id, cat: p.cat, ms: p.ms, slack: base.ls.get(anchor) - base.es.get(anchor), improvementIfZero: base.end - what.end, meta: isGap ? p.meta : g.get(p.id).meta });
  }
  const unresolvedMs = Object.entries(byCat).filter(([c]) => c.startsWith('unresolved:') || c === 'prev_frame_carry').reduce((a, [, v]) => a + v, 0);
  const gpuExecMs = [...ix.submits.values()].filter((s) => s.frameId === frame.frameId).reduce((a, s) => a + (ix.gpu.get(s.submitId) || []).reduce((x, p) => x + p.ms, 0), 0);
  const mutexOnPath = mutexOnCriticalRuns(ix, g, path);
  return { frameId: frame.frameId, frameMs, pathMs, path, byCat, critical, unresolvedMs, unresolved: g.unresolved, inconsistencies: g.inconsistencies, gpuExecMs, modelEndMs: base.end, counterWaits: g.counterWaits, mutexOnPath, graph: g };
}

// Mutex waits (record 18) recorded on the same thread inside a critical-path run segment. This is an
// annotation: the run segment's time is not reduced, because the field layout of 18 is integration-defined
// and a run segment may legitimately contain lock waits. Same-thread waits are disjoint, so the sum is valid.
function mutexOnCriticalRuns(ix, g, path) {
  const byTid = new Map();
  for (const m of ix.mutexWait) { const l = byTid.get(m.tid); if (l) l.push(m); else byTid.set(m.tid, [m]); }
  let ms = 0;
  const bySite = {};
  for (const p of path) {
    if (!p.cat.startsWith('run:') || !g.has(p.id)) continue;
    const n = g.get(p.id);
    const j = ix.jobs.get(n.meta.jobId);
    if (!j || j.tid === null) continue;
    for (const m of byTid.get(j.tid) || []) {
      const o = overlap(n.start, n.start + p.ms, m.t0, m.t1);
      if (o <= 0) continue;
      ms += o;
      const k = `${m.b}:${m.c}`;
      bySite[k] = (bySite[k] || 0) + o;
    }
  }
  return { ms, bySite };
}

export function analyze(trace, opts = {}) {
  const records = Array.isArray(trace) ? trace : trace.records;
  const callKinds = opts.callKinds || trace.meta?.callKinds || {};
  const jobNames = opts.jobNames || trace.meta?.jobNames || {};
  const siteNames = opts.siteNames || trace.meta?.siteNames || {};
  const ix = indexRecords(records);
  const m = trace.meta || {};
  if (m.ctrTableFull) ix.notes.push(`counter table overflowed ${m.ctrTableFull} times; affected counters have no instance records`);
  if (m.droppedWasm) ix.notes.push(`${m.droppedWasm} worker records dropped (ring full); a missing zero or contributor may be a dropped record, not a missing dependency`);
  if (m.droppedMain) ix.notes.push(`${m.droppedMain} main-thread records dropped`);
  if (m.unregisteredEmits) ix.notes.push(`${m.unregisteredEmits} records emitted from threads that never called thread_register were lost`);
  // Kept from the port integration copy.
  const dropped = (m.droppedWasm || 0) + (m.droppedMain || 0) + (m.unregisteredEmits || 0);
  if (dropped) ix.notes.push(`INCOMPLETE TRACE: ${dropped} records were dropped; category totals are partial and critical-path attribution is not verified.`);
  if (m.integration?.includes('not mapped')) ix.notes.push('Job dependencies are not mapped. Counter-activity intervals include cooperative work and cannot be treated as pure blocked time.');
  ix.counterAudit = auditCounters(ix);
  const frames = [...ix.frames.values()].sort((a, b) => a.start - b.start);
  const results = frames.map((f) => analyzeFrame(ix, f, { callKinds, jobNames, siteNames }));
  const preserved = {
    jobMeta: ix.jobMeta.size, counterActivity: ix.counterActivity.length, mutexWait: ix.mutexWait.length,
    mutexHold: ix.mutexHold.length, renderPhase: ix.renderPhase.length,
    ctrArm: ix.ctrArm.size, ctrContrib: [...ix.ctrContrib.values()].reduce((a, l) => a + l.length, 0),
    ctrDec: [...ix.ctrDec.values()].reduce((a, l) => a + l.length, 0), ctrWait: ix.ctrWaits.length, ctrWake: ix.ctrWake.size,
    unknown: Object.fromEntries([...ix.unknown].map(([t, l]) => [t, l.length])),
  };
  const summary = summarize(results);
  summary.gpuDurationSamples = records.filter((r) => r.type === T.GPU_PASS).length;
  return { frames: results, notes: ix.notes, preserved, counterAudit: ix.counterAudit, index: ix, summary };
}

export function summarize(results) {
  const n = results.length;
  if (!n) return { frames: 0 };
  const ms = results.map((r) => r.frameMs).sort((a, b) => a - b);
  const q = (p) => ms[Math.min(n - 1, Math.floor(p * n))];
  const byCat = {};
  for (const r of results) for (const [c, v] of Object.entries(r.byCat)) byCat[c] = (byCat[c] || 0) + v;
  const cats = Object.entries(byCat).map(([cat, total]) => ({ cat, meanMsPerFrame: total / n, share: total / ms.reduce((a, b) => a + b, 0) })).sort((a, b) => b.meanMsPerFrame - a.meanMsPerFrame);
  const imp = {};
  for (const r of results) for (const c of r.critical) {
    const key = c.cat;
    const e = imp[key] || (imp[key] = { cat: key, frames: 0, meanMs: 0, meanImprovementIfZero: 0, meanSlack: 0 });
    e.frames++; e.meanMs += c.ms; e.meanImprovementIfZero += c.improvementIfZero; e.meanSlack += c.slack;
  }
  const improvements = Object.values(imp).map((e) => ({ ...e, meanMs: e.meanMs / n, meanImprovementIfZero: e.meanImprovementIfZero / n, meanSlack: e.meanSlack / e.frames })).sort((a, b) => b.meanImprovementIfZero - a.meanImprovementIfZero);
  return {
    frames: n,
    frameMs: { mean: ms.reduce((a, b) => a + b, 0) / n, p50: q(0.5), p95: q(0.95), max: ms[n - 1] },
    unresolvedMsMean: results.reduce((a, r) => a + r.unresolvedMs, 0) / n,
    pathCoverage: results.reduce((a, r) => a + r.pathMs, 0) / results.reduce((a, r) => a + r.frameMs, 0),
    inconsistencies: results.reduce((a, r) => a + r.inconsistencies.length, 0),
    gpuExecMsMean: results.reduce((a, r) => a + r.gpuExecMs, 0) / n,
    categories: cats,
    improvements,
  };
}

export function formatSummary(s, notes = []) {
  const f = (x) => (Math.round(x * 100) / 100).toFixed(2).padStart(9);
  const lines = [];
  lines.push(`frames ${s.frames}  mean ${f(s.frameMs.mean)} ms  p50 ${f(s.frameMs.p50)}  p95 ${f(s.frameMs.p95)}  max ${f(s.frameMs.max)}`);
  lines.push(`critical path covers ${(s.pathCoverage * 100).toFixed(1)}% of frame time; unresolved mean ${f(s.unresolvedMsMean)} ms/frame; inconsistencies ${s.inconsistencies}`);
  lines.push(s.gpuDurationSamples ? `gpu pass durations (GPU clock, not on CPU timeline): mean ${f(s.gpuExecMsMean)} ms/frame` : 'gpu pass durations: not measured');
  lines.push('');
  lines.push('critical path by category (mean ms/frame, share)');
  for (const c of s.categories) lines.push(`  ${c.cat.padEnd(44)} ${f(c.meanMsPerFrame)}  ${(c.share * 100).toFixed(1).padStart(5)}%`);
  lines.push('');
  lines.push('critical waits: mean ms/frame, conventional slack (0 on path), predicted frame reduction if the wait were zero');
  for (const i of s.improvements) {
    // Unattributed time has no known prerequisite chain, so a "what if zero" number would be a fabrication.
    const unattributed = i.cat.startsWith('unresolved:') || i.cat === 'prev_frame_carry';
    lines.push(`  ${i.cat.padEnd(44)} ${f(i.meanMs)}  slack ${f(i.meanSlack)}  improve ${unattributed ? '      n/a' : f(i.meanImprovementIfZero)}`);
  }
  for (const nt of notes) lines.push(`note: ${nt}`);
  return lines.join('\n');
}

if (import.meta.url === `file:///${process.argv[1]?.replace(/\\/g, '/')}` || process.argv[1]?.endsWith('critical_path.js')) {
  const fs = await import('node:fs');
  const file = process.argv[2];
  if (!file) { console.error('usage: node critical_path.js trace.json [--frames]'); process.exit(2); }
  const trace = JSON.parse(fs.readFileSync(file, 'utf8'));
  const out = analyze(trace);
  console.log(formatSummary(out.summary, out.notes));
  console.log(`preserved records: ${JSON.stringify(out.preserved)}`);
  if (process.argv.includes('--frames')) {
    for (const r of out.frames) {
      console.log(`\nframe ${r.frameId}: ${r.frameMs.toFixed(2)} ms (path ${r.pathMs.toFixed(2)}, unresolved ${r.unresolvedMs.toFixed(2)})`);
      for (const p of r.path) console.log(`  ${p.cat.padEnd(40)} ${p.ms.toFixed(3).padStart(9)}  ${p.id}`);
      for (const u of r.unresolved) console.log(`  unresolved: ${JSON.stringify(u)}`);
      for (const i of r.inconsistencies) console.log(`  inconsistency: ${JSON.stringify(i)}`);
      if (r.mutexOnPath.ms > 0) console.log(`  mutex wait inside critical run segments (annotation, not subtracted): ${r.mutexOnPath.ms.toFixed(3)} ms ${JSON.stringify(r.mutexOnPath.bySite)}`);
    }
  }
  if (process.argv.includes('--counters')) {
    console.log(`\ncounter generation ambiguities: ${out.counterAudit.ambiguities.length} (no dependency edges are created for these)`);
    for (const a of out.counterAudit.ambiguities) console.log(`  ${JSON.stringify(a)}`);
    for (const r of out.frames) {
      console.log(`\nframe ${r.frameId} counter / untyped waits (times relative to frame start, ms)`);
      for (const c of r.counterWaits) console.log(`  ${JSON.stringify(c)}`);
    }
  }
}
