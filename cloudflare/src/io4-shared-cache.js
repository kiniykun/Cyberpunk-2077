// [cloudflare io4] Shared block cache (io3 + persistent-store presence table, persist/warm/widen counters) over SharedArrayBuffers, used by every pthread's CpResRead and by the
// async prefetch helper worker. One global lock (short critical sections: slot scan + memcpy of <= 1 block).
// Slot states: 0 empty, 1 in flight (claimed by a fetcher), 2 ready. Waiters block on the slot's state word.
// Layout: H = Int32Array(SBC_HDR + 5*S) control, Q = Int32Array(2*QC) prefetch ring, D = Uint8Array(S*B) data.
var SBC_HDR = 64, SBC_LOCK = 0, SBC_CLOCK = 1, SBC_QHEAD = 2, SBC_QTAIL = 3, SBC_BELL = 4, SBC_INFL = 5, SBC_INFLMAX = 6,
  SBC_HITS = 8, SBC_MISSES = 9, SBC_WAITS = 10, SBC_PF_ISSUED = 11, SBC_PF_DONE = 12, SBC_PF_USED = 13, SBC_EVICT = 14,
  SBC_PF_ERR = 15, SBC_KB_SYNC = 16, SBC_KB_PF = 17, SBC_PF_SKIP = 18, SBC_NOSLOT = 19, SBC_PF_INFL = 20,
  SBC_PF_BUDGET = 21, SBC_PF_OFFSKIP = 22, SBC_PF_WASTED = 23, SBC_FILES = 256,
  SBC_PS_LOADREQ = 24, SBC_PS_LOADHIT = 25, SBC_PS_LOADMISS = 26, SBC_PS_WRITES = 27, SBC_PS_KB = 28, SBC_WARM_PLAN = 29,
  SBC_WARM_DONE = 30, SBC_WARM_KB = 31, SBC_WARM_ERR = 32, SBC_PS_EVICT = 33, SBC_WIDEN = 34, SBC_PS_BLOCKS = 35, SBC_PS_READY = 36,
  SBC_PS_SKIPPED = 37;
// Per-file arrays after the slot arrays: used[256], wasted[256], off[256] (prefetch disabled for that file).
function sbcCreate(slots, blockSize, queueCap, ptabCap) {
  // NOTE: may run on the main thread from PThread.initMainThread(), long before this file's top-level
  // var statements execute; make-io-bundle2.mjs therefore inlines every SBC_* constant as a literal.
  const hdr = new SharedArrayBuffer(4 * (SBC_HDR + 5 * slots + 3 * SBC_FILES)), q = new SharedArrayBuffer(4 * 2 * queueCap), d = new SharedArrayBuffer(slots * blockSize);
  // Presence table of blocks held by the persistent store (power of two, open addressing; helper is the only writer).
  const P = ptabCap > 0 ? 1 << Math.ceil(Math.log2(ptabCap)) : 0;
  return {hdr, q, d, S: slots, B: blockSize, QC: queueCap, P, p: P ? new SharedArrayBuffer(4 * P) : null};
}
// Returns null (caller falls back to the private per-thread cache) unless all three buffers are real
// SharedArrayBuffers of the expected sizes.
function sbcOpen(m) {
  if (!m || typeof SharedArrayBuffer === "undefined" || !(m.hdr instanceof SharedArrayBuffer) || !(m.q instanceof SharedArrayBuffer) || !(m.d instanceof SharedArrayBuffer)) return null;
  const S = m.S;
  if (!(S > 0) || !(m.B > 0) || !(m.QC > 0) || m.hdr.byteLength < 4 * (SBC_HDR + 5 * S + 3 * SBC_FILES) || m.q.byteLength < 8 * m.QC || m.d.byteLength < S * m.B) return null;
  if (m.P && (!(m.p instanceof SharedArrayBuffer) || m.p.byteLength < 4 * m.P || (m.P & (m.P - 1)))) return null;
  const H = new Int32Array(m.hdr);
  return {PT: m.P ? new Int32Array(m.p) : null, PM: m.P ? m.P - 1 : 0,S, B: m.B, QC: m.QC, H, Q: new Int32Array(m.q), D: new Uint8Array(m.d),
    key: SBC_HDR, state: SBC_HDR + S, use: SBC_HDR + 2 * S, len: SBC_HDR + 3 * S, flag: SBC_HDR + 4 * S,
    pfUsed: SBC_HDR + 5 * S, pfWasted: SBC_HDR + 5 * S + SBC_FILES, pfOff: SBC_HDR + 5 * S + 2 * SBC_FILES};
}
function sbcKey(file, block) { return file * 2097152 + block + 1; }
function sbcFileOf(k) { return Math.floor((k - 1) / 2097152); }
// Called under the lock when a prefetched block is first used (used=true) or evicted unused (used=false).
// Rolling window: halve both counts at 32 outcomes. A file whose used/(used+wasted) drops below 50%
// after at least 8 outcomes gets prefetch switched off for the rest of the session.
function sbcPfOutcome(c, k, used) {
  const H = c.H, f = sbcFileOf(k);
  if (used) ++H[SBC_PF_USED]; else ++H[SBC_PF_WASTED];
  if (f < 0 || f >= SBC_FILES) return;
  const u = c.pfUsed + f, w = c.pfWasted + f;
  if (used) ++H[u]; else ++H[w];
  const n = H[u] + H[w];
  if (n >= 8 && H[u] * 2 < n) H[c.pfOff + f] = 1;
  if (n >= 32) { H[u] >>= 1; H[w] >>= 1; }
}
// Persistent-store presence table: 0 empty, -1 tombstone, otherwise sbcKey. Readers are lock-free.
function sbcPtabSlot(c, k) { return Math.imul(k, 0x9E3779B1) >>> 0 & c.PM; }
function sbcPersisted(c, k) {
  const T = c.PT; if (!T) return false;
  for (let i = sbcPtabSlot(c, k), n = 0; n <= c.PM; ++n, i = (i + 1) & c.PM) { const v = Atomics.load(T, i); if (v === k) return true; if (v === 0) return false; }
  return false;
}
function sbcPtabAdd(c, k) {
  const T = c.PT; if (!T) return false;
  let tomb = -1;
  for (let i = sbcPtabSlot(c, k), n = 0; n <= c.PM; ++n, i = (i + 1) & c.PM) {
    const v = Atomics.load(T, i);
    if (v === k) return true;
    if (v === -1 && tomb < 0) tomb = i;
    if (v === 0) { Atomics.store(T, tomb >= 0 ? tomb : i, k); Atomics.add(c.H, SBC_PS_BLOCKS, 1); return true; }
  }
  if (tomb >= 0) { Atomics.store(T, tomb, k); Atomics.add(c.H, SBC_PS_BLOCKS, 1); return true; }
  return false;
}
function sbcPtabDel(c, k) {
  const T = c.PT; if (!T) return;
  for (let i = sbcPtabSlot(c, k), n = 0; n <= c.PM; ++n, i = (i + 1) & c.PM) { const v = Atomics.load(T, i); if (v === k) { Atomics.store(T, i, -1); Atomics.sub(c.H, SBC_PS_BLOCKS, 1); return; } if (v === 0) return; }
}
// Present in the shared cache (ready or in flight)?
function sbcHas(c, k) { return sbcRead(c, k, 0, 0, null, 0, false, {}) !== -2; }
function sbcPfAllowed(c, file) { return file >= 0 && file < SBC_FILES && Atomics.load(c.H, c.pfOff + file) === 0; }
function sbcLock(c) {
  let spins = 0;
  while (Atomics.compareExchange(c.H, SBC_LOCK, 0, 1) !== 0) {
    if (++spins > 64) { try { Atomics.wait(c.H, SBC_LOCK, 1, 1); } catch (e) {} }
  }
}
function sbcUnlock(c) { Atomics.store(c.H, SBC_LOCK, 0); Atomics.notify(c.H, SBC_LOCK, 1); }
function sbcFind(c, k) { const H = c.H; for (let i = 0; i < c.S; ++i) if (H[c.key + i] === k) return i; return -1; }
// Copy [within, within+take) of block k into out[outOff..] if ready. Returns copied count (>=0), or
// -1 in flight (slot index in r.slot), -2 absent and not claimed, or claims a slot: -3 with r.slot = claimed slot,
// -4 absent but no claimable slot (all in flight).
function sbcRead(c, k, within, take, out, outOff, claim, r) {
  const H = c.H;
  sbcLock(c);
  try {
  const i = sbcFind(c, k);
  if (i >= 0) {
    const st = H[c.state + i];
    if (st === 2) {
      const n = Math.max(0, Math.min(take, H[c.len + i] - within));
      if (n > 0 && out) out.set(c.D.subarray(i * c.B + within, i * c.B + within + n), outOff);
      if (out) {   // a probe (helper presence check) is not a use
        H[c.use + i] = ++H[SBC_CLOCK];
        if (H[c.flag + i]) { H[c.flag + i] = 0; sbcPfOutcome(c, k, true); }
        ++H[SBC_HITS];
      }
      return n;
    }
    if (st === 1) { r.slot = i; return -1; }
  }
  if (!claim) return -2;
  let v = -1, best = 2147483647;
  for (let j = 0; j < c.S; ++j) {
    const st = H[c.state + j];
    if (st === 0) { v = j; break; }
    if (st === 2 && H[c.use + j] < best) { best = H[c.use + j]; v = j; }
  }
  if (v < 0) { ++H[SBC_NOSLOT]; return -4; }
  if (H[c.state + v] === 2) { ++H[SBC_EVICT]; if (H[c.flag + v]) sbcPfOutcome(c, H[c.key + v], false); }
  H[c.key + v] = k; H[c.state + v] = 1; H[c.flag + v] = 0; H[c.len + v] = 0; H[c.use + v] = ++H[SBC_CLOCK];
  r.slot = v;
  return -3;
  } finally { sbcUnlock(c); }
}
function sbcFill(c, slot, k, bytes, prefetched) {
  const H = c.H;
  let ok = false;
  sbcLock(c);
  try {
    ok = H[c.key + slot] === k && H[c.state + slot] === 1;
    if (ok) {
      c.D.set(bytes.subarray(0, Math.min(bytes.length, c.B)), slot * c.B);
      H[c.len + slot] = Math.min(bytes.length, c.B); H[c.flag + slot] = prefetched ? 1 : 0;
      Atomics.store(H, c.state + slot, 2);
    }
  } finally { sbcUnlock(c); }
  Atomics.notify(H, c.state + slot);
  return ok;
}
function sbcAbort(c, slot, k) {
  const H = c.H;
  sbcLock(c);
  try { if (H[c.key + slot] === k && H[c.state + slot] === 1) { H[c.key + slot] = 0; Atomics.store(H, c.state + slot, 0); } }
  finally { sbcUnlock(c); }
  Atomics.notify(H, c.state + slot);
}
// Wait (pthread only) until an in-flight slot leaves state 1 or timeout ms passes.
function sbcWait(c, slot, ms) { ++c.H[SBC_WAITS]; try { Atomics.wait(c.H, c.state + slot, 1, ms); } catch (e) {} }
function sbcEnqueue(c, file, block) {
  const H = c.H;
  let ok = false;
  sbcLock(c);
  try {
    const head = H[SBC_QHEAD], tail = H[SBC_QTAIL];
    if (tail - head < c.QC) { const at = (tail % c.QC) * 2; c.Q[at] = file; c.Q[at + 1] = block; H[SBC_QTAIL] = tail + 1; ok = true; }
  } finally { sbcUnlock(c); }
  if (ok) { Atomics.add(H, SBC_BELL, 1); Atomics.notify(H, SBC_BELL); }
  return ok;
}
function sbcDequeue(c, max) {
  const H = c.H, out = [];
  sbcLock(c);
  try { while (H[SBC_QHEAD] < H[SBC_QTAIL] && out.length < max) { const at = (H[SBC_QHEAD] % c.QC) * 2; out.push([c.Q[at], c.Q[at + 1]]); ++H[SBC_QHEAD]; } }
  finally { sbcUnlock(c); }
  return out;
}
function sbcStats(c) {
  const H = c.H;
  let ready = 0, inflight = 0;
  for (let i = 0; i < c.S; ++i) { const st = H[c.state + i]; if (st === 2) ++ready; else if (st === 1) ++inflight; }
  return {slots: c.S, blockMB: c.B / 1048576, ready, inflight, hits: H[SBC_HITS], misses: H[SBC_MISSES], dedupWaits: H[SBC_WAITS],
    evictions: H[SBC_EVICT], noSlot: H[SBC_NOSLOT], syncInflightMax: H[SBC_INFLMAX], syncMb: Math.round(H[SBC_KB_SYNC] / 102.4) / 10,
    demandMb: Math.round(H[SBC_KB_SYNC] / 102.4) / 10, widenedRequests: H[SBC_WIDEN],
    persist: {ready: H[SBC_PS_READY], blocks: H[SBC_PS_BLOCKS], loadRequests: H[SBC_PS_LOADREQ], loadHits: H[SBC_PS_LOADHIT], loadMisses: H[SBC_PS_LOADMISS],
      writes: H[SBC_PS_WRITES], writtenMb: Math.round(H[SBC_PS_KB] / 102.4) / 10, evictedPacks: H[SBC_PS_EVICT], dropped: H[SBC_PS_SKIPPED]},
    warm: {plannedBlocks: H[SBC_WARM_PLAN], doneBlocks: H[SBC_WARM_DONE], mb: Math.round(H[SBC_WARM_KB] / 102.4) / 10, errors: H[SBC_WARM_ERR]},
    prefetch: {issued: H[SBC_PF_ISSUED], done: H[SBC_PF_DONE], used: H[SBC_PF_USED], wastedEvicted: H[SBC_PF_WASTED],
      usedPerIssued: H[SBC_PF_ISSUED] ? Math.round(100 * H[SBC_PF_USED] / H[SBC_PF_ISSUED]) / 100 : null,
      pctOfDemand: H[SBC_KB_SYNC] ? Math.round(1000 * H[SBC_KB_PF] / H[SBC_KB_SYNC]) / 10 : null,
      errors: H[SBC_PF_ERR], skipped: H[SBC_PF_SKIP], budgetSkips: H[SBC_PF_BUDGET], offSkips: H[SBC_PF_OFFSKIP],
      filesOff: (() => { const o = []; for (let f = 0; f < SBC_FILES; ++f) if (H[c.pfOff + f]) o.push(f); return o; })(),
      mb: Math.round(H[SBC_KB_PF] / 102.4) / 10, inflightNow: H[SBC_PF_INFL]}};
}
