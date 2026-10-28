// [cloudflare io2] Shared block cache over SharedArrayBuffers, used by every pthread's CpResRead and by the
// async prefetch helper worker. One global lock (short critical sections: slot scan + memcpy of <= 1 block).
// Slot states: 0 empty, 1 in flight (claimed by a fetcher), 2 ready. Waiters block on the slot's state word.
// Layout: H = Int32Array(SBC_HDR + 5*S) control, Q = Int32Array(2*QC) prefetch ring, D = Uint8Array(S*B) data.
var SBC_HDR = 64, SBC_LOCK = 0, SBC_CLOCK = 1, SBC_QHEAD = 2, SBC_QTAIL = 3, SBC_BELL = 4, SBC_INFL = 5, SBC_INFLMAX = 6,
  SBC_HITS = 8, SBC_MISSES = 9, SBC_WAITS = 10, SBC_PF_ISSUED = 11, SBC_PF_DONE = 12, SBC_PF_USED = 13, SBC_EVICT = 14,
  SBC_PF_ERR = 15, SBC_KB_SYNC = 16, SBC_KB_PF = 17, SBC_PF_SKIP = 18, SBC_NOSLOT = 19, SBC_PF_INFL = 20;
function sbcCreate(slots, blockSize, queueCap) {
  // NOTE: may run on the main thread from PThread.initMainThread(), long before this file's top-level
  // var statements execute; make-io-bundle2.mjs therefore inlines every SBC_* constant as a literal.
  const hdr = new SharedArrayBuffer(4 * (SBC_HDR + 5 * slots)), q = new SharedArrayBuffer(4 * 2 * queueCap), d = new SharedArrayBuffer(slots * blockSize);
  return {hdr, q, d, S: slots, B: blockSize, QC: queueCap};
}
// Returns null (caller falls back to the private per-thread cache) unless all three buffers are real
// SharedArrayBuffers of the expected sizes.
function sbcOpen(m) {
  if (!m || typeof SharedArrayBuffer === "undefined" || !(m.hdr instanceof SharedArrayBuffer) || !(m.q instanceof SharedArrayBuffer) || !(m.d instanceof SharedArrayBuffer)) return null;
  const S = m.S;
  if (!(S > 0) || !(m.B > 0) || !(m.QC > 0) || m.hdr.byteLength < 4 * (64 + 5 * S) || m.q.byteLength < 8 * m.QC || m.d.byteLength < S * m.B) return null;
  const H = new Int32Array(m.hdr);
  return {S, B: m.B, QC: m.QC, H, Q: new Int32Array(m.q), D: new Uint8Array(m.d),
    key: SBC_HDR, state: SBC_HDR + S, use: SBC_HDR + 2 * S, len: SBC_HDR + 3 * S, flag: SBC_HDR + 4 * S};
}
function sbcKey(file, block) { return file * 2097152 + block + 1; }
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
        if (H[c.flag + i]) { H[c.flag + i] = 0; ++H[SBC_PF_USED]; }
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
  if (H[c.state + v] === 2) ++H[SBC_EVICT];
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
    prefetch: {issued: H[SBC_PF_ISSUED], done: H[SBC_PF_DONE], used: H[SBC_PF_USED], errors: H[SBC_PF_ERR], skipped: H[SBC_PF_SKIP], mb: Math.round(H[SBC_KB_PF] / 102.4) / 10, inflightNow: H[SBC_PF_INFL]}};
}
