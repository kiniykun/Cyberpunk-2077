#pragma once
#include <atomic>
#include <cstdint>
#include <emscripten/emscripten.h>

namespace wgtrace {

enum Type : uint32_t {
  FRAME = 1, JOB_RUN = 2, JOB_DEP = 3, WAIT = 4, PROXY_DEQ = 5, PROXY_CALL = 6,
  SCOPE = 7, SETTLE = 8, SUBMIT = 9, SUBMIT_ENC = 10, GPU_PASS = 11,
  PRESENT = 12, PRESENT_DONE = 13, GCT = 14, JOB_SPAWN = 15,
  // 16..20 are emitted by the existing engine integration (job metadata, counter activity,
  // mutex wait, mutex hold, renderer phase). This header does not emit them; the analyzer preserves them.
  JOB_META = 16, COUNTER_ACTIVITY = 17, MUTEX_WAIT = 18, MUTEX_HOLD = 19, RENDER_PHASE = 20,
  // Counter dependency records. Every interval brackets the engine's own atomic operation; the analyzer
  // never places the transition at either stamp.
  CTR_ARM = 21,     // a=instance b=addr lo32 c=addr hi32  t0=stamp after the instance was published  t1=1 if implicit
  CTR_CONTRIB = 22, // a=instance b=contributing jobId c=delta  t0=before engine fetch_add  t1=after instance read
  CTR_DEC = 23,     // a=instance b=decrementing jobId c=value after (0 = zero, 0xFFFFFFFF = negative)  t0=before instance read and fetch_sub  t1=after fetch_sub
  CTR_WAIT = 24,    // a=instance b=waiting jobId c=siteId  t0=block start  t1=resume
  CTR_WAKE = 25,    // a=instance b=waiting jobId c=siteId  t0=zero observed by the waiter  t1=resume
  GATE_RELEASE = 26 // a=instance that reached zero b=instance the released job accumulates on (0 unknown)
                    // c=releasing jobId  t0=queue time  t1=entries released in this batch
};

enum WaitKind : uint32_t {
  W_PROXY = 1, W_SCOPE = 2, W_SUBMIT_DONE = 3, W_JOB = 4, W_PRESENT = 5, W_OTHER = 6
};

enum IdKind : uint32_t { ID_FRAME = 0, ID_JOB = 1, ID_REQ = 2, ID_SCOPE = 3, ID_SUBMIT = 4, ID_ENCODER = 5, ID_PRESENT = 6, ID_COUNTER = 7, ID_COUNT = 8 };

// Deterministic race tests define WGTRACE_RACE_POINT(point, counter) before including this header to park a
// thread at a named point inside the counter hooks. It compiles to nothing otherwise.
#ifndef WGTRACE_RACE_POINT
#define WGTRACE_RACE_POINT(point, counter) ((void)0)
#endif
enum RacePoint : uint32_t {
  RP_IMPLICIT_BEFORE_CAS = 1,   // instance slot read as empty, new id allocated, not yet published
  RP_ADD_BEFORE_PUBLISH = 2,    // engine fetch_add returned 0, new generation not yet published
  RP_DEC_BEFORE_END_STAMP = 3,  // engine fetch_sub done, end stamp not yet taken
};

struct Rec {
  uint32_t type, frameId, a, b, c, tid;
  double t0, t1;
};
static_assert(sizeof(Rec) == 40, "Rec layout must match wgtrace_main.js REC_BYTES");

struct Ring {
  std::atomic<uint32_t> head{0};
  std::atomic<uint32_t> dropped{0};
  std::atomic_flag writer = ATOMIC_FLAG_INIT;
  uint32_t capacity = 0;
  uint32_t tid = 0;
  Rec* recs = nullptr;
};

constexpr uint32_t kMaxThreads = 64;

inline std::atomic<Ring*> g_rings[kMaxThreads]{};
inline std::atomic<uint32_t> g_ringCount{0};
inline std::atomic<uint32_t> g_ids[7];
inline std::atomic<uint32_t> g_counterIds{0};
inline uint32_t g_capacity = 8192;
inline std::atomic<int> g_enabled{0};

inline thread_local Ring* tl_ring = nullptr;
inline thread_local uint32_t tl_frame = 0;
inline thread_local uint32_t tl_job = 0;

// The threaded SDK already returns timeOrigin + performance.now().
// Avoid defining a non-inline EM_JS import in every including translation unit.
inline double now() { return emscripten_get_now(); }

inline uint32_t next_id(IdKind k) { return k == ID_COUNTER ? g_counterIds.fetch_add(1, std::memory_order_relaxed) + 1 : g_ids[k].fetch_add(1, std::memory_order_relaxed) + 1; }

inline void set_capacity(uint32_t perThread) { g_capacity = perThread; }
inline void set_enabled(bool on) { g_enabled.store(on ? 1 : 0); }

inline void thread_register(uint32_t tid) {
  if (tl_ring) return;
  uint32_t slot = g_ringCount.fetch_add(1);
  if (slot >= kMaxThreads) return;
  Ring* r = new Ring();
  r->capacity = g_capacity;
  r->tid = tid;
  r->recs = new Rec[g_capacity]();
  g_rings[slot].store(r, std::memory_order_release);
  tl_ring = r;
}

// Records emitted while enabled from a thread with no ring (never registered, or past kMaxThreads) are lost;
// this count makes the loss visible in the export.
inline std::atomic<uint32_t> g_unregisteredEmits{0};

inline void emit(uint32_t type, uint32_t frameId, uint32_t a, uint32_t b, uint32_t c, double t0, double t1) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  Ring* r = tl_ring;
  if (!r) { g_unregisteredEmits.fetch_add(1, std::memory_order_relaxed); return; }
  while(r->writer.test_and_set(std::memory_order_acquire)) {}
  uint32_t h = r->head.load(std::memory_order_relaxed);
  if (h >= r->capacity) { r->dropped.fetch_add(1,std::memory_order_relaxed);r->writer.clear(std::memory_order_release);return; }
  Rec& rec = r->recs[h];
  rec.type = type; rec.frameId = frameId; rec.a = a; rec.b = b; rec.c = c; rec.tid = r->tid;
  rec.t0 = t0; rec.t1 = t1;
  r->head.store(h + 1, std::memory_order_release);
  r->writer.clear(std::memory_order_release);
}

inline void frame_begin(uint32_t frameId) { tl_frame = frameId; }
inline void frame_end(uint32_t frameId, double t0, double t1) { emit(FRAME, frameId, 0, 0, 0, t0, t1); }

inline void job_dep(uint32_t frameId, uint32_t jobId, uint32_t depJobId) { emit(JOB_DEP, frameId, jobId, depJobId, 0, 0, 0); }
inline void job_spawn(uint32_t frameId, uint32_t jobId, uint32_t parentJobId) { emit(JOB_SPAWN, frameId, jobId, parentJobId, 0, now(), 0); }

struct JobScope {
  uint32_t frameId, jobId, workerId, prevJob, prevFrame;
  double t0;
  JobScope(uint32_t f, uint32_t j, uint32_t w) : frameId(f), jobId(j), workerId(w), prevJob(tl_job), prevFrame(tl_frame), t0(now()) {
    tl_job = j; tl_frame = f;
  }
  ~JobScope() {
    emit(JOB_RUN, frameId, jobId, workerId, 0, t0, now());
    tl_job = prevJob; tl_frame = prevFrame;
  }
};

struct WaitScope {
  uint32_t kind, target;
  double t0;
  WaitScope(uint32_t k, uint32_t t) : kind(k), target(t), t0(now()) {}
  ~WaitScope() { emit(WAIT, tl_frame, tl_job, kind, target, t0, now()); }
};

inline void gpu_pass(uint32_t frameId, uint32_t submitId, uint32_t passIndex, double durationMs) {
  emit(GPU_PASS, frameId, submitId, passIndex, 0, durationMs, 0);
}

// ---------------------------------------------------------------------------
// Call sites: static string literals registered once. The pointer is published with release ordering,
// so a reader that sees a non-null slot sees the whole literal; a null slot is "not yet published".
constexpr uint32_t kMaxSites = 1024;
inline std::atomic<const char*> g_siteNames[kMaxSites]{};
inline std::atomic<uint32_t> g_siteCount{0};

inline uint32_t register_site(const char* literal) {
  uint32_t id = g_siteCount.fetch_add(1, std::memory_order_relaxed);
  if (id >= kMaxSites) return 0;
  g_siteNames[id].store(literal, std::memory_order_release);
  return id + 1;  // 0 means "unknown site"
}
// Usage: wgtrace::CounterWaitScope w(&counter, WGTRACE_SITE("RenderFrame/WaitPreviousCommands"));
#define WGTRACE_SITE(literal) ([]() -> uint32_t { static const uint32_t id_ = ::wgtrace::register_site(literal); return id_; }())

// ---------------------------------------------------------------------------
// Counter generations. Engine counters are reused at one address, so each generation gets an instance id.
// The address -> current-instance table is maintained whether or not tracing is enabled, so generations are
// already correct when tracing is switched on; only emits are gated. wgtrace_reset never clears it, because
// that would split a generation in flight.
//
// Counters whose storage is freed (heap-allocated entries) must call ctr_retire(counter) before the memory is
// released; otherwise churned addresses fill the table and every later hook loses its records. A retired slot
// becomes a tombstone and is reused. Lookups are lock-free; insert and retire are serialized by a spinlock so
// tombstone reuse can never produce two slots for one address. Each happens once per counter lifetime.
struct CtrSlot {
  std::atomic<uint64_t> key{0};
  std::atomic<uint32_t> inst{0};
};
// 4096 slots: with ctr_retire on free, only live counters occupy slots. A larger table grows the static Wasm
// image past the engine link's fixed INITIAL_MEMORY (16K slots needed +192 KiB and failed the strict link).
constexpr uint32_t kCtrSlotsLog2 = 12;
constexpr uint32_t kCtrSlots = 1u << kCtrSlotsLog2;
constexpr uint64_t kCtrTombstone = 1;  // never a valid counter address (counters are at least 4-byte aligned)
inline CtrSlot g_ctr[kCtrSlots];
inline std::atomic<uint32_t> g_ctrTableFull{0};
inline std::atomic<uint32_t> g_ctrLive{0};
inline std::atomic<uint32_t> g_ctrMaxProbe{0};  // longest home->slot distance of any key ever inserted
inline std::atomic_flag g_ctrInsertLock = ATOMIC_FLAG_INIT;

inline uint64_t ctr_key(const void* counter) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(counter)); }
inline uint32_t ctr_home(uint64_t k) { return static_cast<uint32_t>((k * 0x9E3779B97F4A7C15ull) >> (64 - kCtrSlotsLog2)); }

// Lock-free probe, bounded by g_ctrMaxProbe so a table full of tombstones never costs a full scan.
// Tombstones do not end the chain; an empty slot does. A stale bound can only cause a miss, and every miss
// falls back to a rescan under the insert lock, so a miss is never final.
inline CtrSlot* ctr_find(uint64_t k) {
  const uint32_t limit = g_ctrMaxProbe.load(std::memory_order_acquire) + 1;
  uint32_t i = ctr_home(k);
  for (uint32_t probe = 0; probe < limit && probe < kCtrSlots; probe++, i = (i + 1) & (kCtrSlots - 1)) {
    uint64_t cur = g_ctr[i].key.load(std::memory_order_acquire);
    if (cur == k) return &g_ctr[i];
    if (cur == 0) return nullptr;
  }
  return nullptr;
}

struct CtrInsertLock {
  CtrInsertLock() { while (g_ctrInsertLock.test_and_set(std::memory_order_acquire)) {} }
  ~CtrInsertLock() { g_ctrInsertLock.clear(std::memory_order_release); }
};

inline CtrSlot* ctr_slot(const void* counter) {
  const uint64_t k = ctr_key(counter);
  if (k <= kCtrTombstone) return nullptr;
  if (CtrSlot* s = ctr_find(k)) return s;
  CtrInsertLock lock;
  const uint32_t home = ctr_home(k);
  uint32_t i = home, reuseAt = 0;
  CtrSlot* reuse = nullptr;
  for (uint32_t probe = 0; probe < kCtrSlots; probe++, i = (i + 1) & (kCtrSlots - 1)) {
    uint64_t cur = g_ctr[i].key.load(std::memory_order_acquire);
    if (cur == k) return &g_ctr[i];
    if (cur == kCtrTombstone && !reuse) { reuse = &g_ctr[i]; reuseAt = probe; }
    if (cur == 0) { if (!reuse) { reuse = &g_ctr[i]; reuseAt = probe; } break; }
  }
  if (!reuse) { g_ctrTableFull.fetch_add(1, std::memory_order_relaxed); return nullptr; }
  if (reuseAt > g_ctrMaxProbe.load(std::memory_order_relaxed)) g_ctrMaxProbe.store(reuseAt, std::memory_order_release);
  reuse->inst.store(0, std::memory_order_relaxed);
  reuse->key.store(k, std::memory_order_release);
  g_ctrLive.fetch_add(1, std::memory_order_relaxed);
  return reuse;
}

// Call when the counter's storage is about to be freed. No hook may run on this address concurrently (the
// engine asserts the counter is zero and unreferenced at that point). Safe to call for untracked addresses.
inline void ctr_retire(const void* counter) {
  const uint64_t k = ctr_key(counter);
  if (k <= kCtrTombstone || g_ctrLive.load(std::memory_order_relaxed) == 0) return;
  CtrInsertLock lock;  // under the lock the bounded probe sees the final bound, so a miss is final
  if (CtrSlot* s = ctr_find(k)) {
    s->inst.store(0, std::memory_order_relaxed);
    s->key.store(kCtrTombstone, std::memory_order_release);
    g_ctrLive.fetch_sub(1, std::memory_order_relaxed);
  }
}

inline void ctr_emit_arm(uint32_t inst, const void* counter, bool implicit) {
  const uint64_t k = ctr_key(counter);
  emit(CTR_ARM, tl_frame, inst, static_cast<uint32_t>(k), static_cast<uint32_t>(k >> 32), now(), implicit ? 1.0 : 0.0);
}

// Current instance for an address. If none exists yet (first sighting), exactly one caller publishes a new
// implicit instance by CAS from 0; a loser adopts the winner's id and emits nothing under its own id.
inline uint32_t ctr_current(const void* counter) {
  CtrSlot* s = ctr_slot(counter);
  if (!s) return 0;
  uint32_t inst = s->inst.load(std::memory_order_acquire);
  if (inst) return inst;
  uint32_t mine = next_id(ID_COUNTER);
  WGTRACE_RACE_POINT(RP_IMPLICIT_BEFORE_CAS, counter);
  uint32_t expected = 0;
  if (s->inst.compare_exchange_strong(expected, mine, std::memory_order_acq_rel, std::memory_order_acquire)) {
    ctr_emit_arm(mine, counter, true);
    return mine;
  }
  return expected;
}

// JOB_SPAWN.c flag: the spawn was inferred from a counter enqueue, not an explicit spawn. The analyzer does
// not treat enqueue-to-start time as scheduling, because the engine may hold the job on a gate.
constexpr uint32_t SPAWN_FROM_COUNTER_ENQUEUE = 1;

// Enqueue, in two halves around the engine's own fetch_add. Call end BEFORE the job is visible to workers.
//   auto tok = wgtrace::ctr_add_begin();
//   int64_t before = counter.value.fetch_add(1);          // unchanged engine code
//   wgtrace::ctr_add_end(tok, &counter, job->id, 1, before);
//   PushJob(job);                                          // unchanged engine code
// The caller whose fetch_add returned 0 starts a new generation: it allocates an id and publishes it with an
// exchange (never a CAS from 0, which would fail on every re-arm after the first). Another contributor may
// still read the previous instance before that publish; the begin stamp lets the analyzer detect that.
struct CtrAddToken { double t0; };
inline CtrAddToken ctr_add_begin() { return { now() }; }
inline void ctr_add_end(const CtrAddToken& tok, const void* counter, uint32_t jobId, int32_t delta, int64_t valueBefore) {
  CtrSlot* s = ctr_slot(counter);
  if (!s) return;
  uint32_t inst;
  if (valueBefore == 0) {
    inst = next_id(ID_COUNTER);
    WGTRACE_RACE_POINT(RP_ADD_BEFORE_PUBLISH, counter);
    s->inst.exchange(inst, std::memory_order_acq_rel);
    ctr_emit_arm(inst, counter, false);
  } else {
    inst = ctr_current(counter);
  }
  emit(CTR_CONTRIB, tl_frame, inst, jobId, static_cast<uint32_t>(delta), tok.t0, now());
  if (jobId && tl_job && jobId != tl_job) emit(JOB_SPAWN, tl_frame, jobId, tl_job, SPAWN_FROM_COUNTER_ENQUEUE, tok.t0, 0);
}

// Decrement, in two halves around the engine's own fetch_sub:
//   auto tok = wgtrace::ctr_dec_begin(&counter);
//   int64_t after = counter.value.fetch_sub(1) - 1;        // unchanged engine code
//   wgtrace::ctr_dec_end(tok, job->id, after);
// The zero transition happened somewhere inside [begin, end]. Preemption between the two stamps widens the
// interval; the analyzer reports the interval, never a point inside it.
struct CtrDecToken { uint32_t inst; double t0; };
inline CtrDecToken ctr_dec_begin(const void* counter) {
  const double t0 = now();
  return { ctr_current(counter), t0 };
}
inline void ctr_dec_end(const CtrDecToken& tok, uint32_t jobId, int64_t valueAfter) {
  WGTRACE_RACE_POINT(RP_DEC_BEFORE_END_STAMP, nullptr);
  const double t1 = now();
  uint32_t v = valueAfter < 0 ? 0xFFFFFFFFu : (valueAfter > 0xFFFFFFFEll ? 0xFFFFFFFEu : static_cast<uint32_t>(valueAfter));
  // jobId 0 with no enclosing JobScope stays 0; the analyzer reports it as a decrement outside any job.
  emit(CTR_DEC, tl_frame, tok.inst, jobId ? jobId : tl_job, v, tok.t0, t1);
}

// Lookup only: never creates an instance. 0 when the address is untracked.
inline uint32_t ctr_peek(const void* counter) {
  const uint64_t k = ctr_key(counter);
  if (k <= kCtrTombstone) return 0;
  CtrSlot* s = ctr_find(k);
  return s ? s->inst.load(std::memory_order_acquire) : 0;
}

// A counter reached zero and its waiting list released jobs that accumulate on another counter. Call with the
// released counter's instance captured BEFORE that counter can be freed, then once per released job.
inline void gate_release(uint32_t releasedInst, const void* accumulateCounter, uint32_t batchSize) {
  if (!g_enabled.load(std::memory_order_relaxed)) return;
  emit(GATE_RELEASE, tl_frame, releasedInst, ctr_peek(accumulateCounter), tl_job, now(), static_cast<double>(batchSize));
}

// Wraps the engine's existing counter wait without changing it. Jobs run cooperatively inside it keep their
// own JobScope. Call woke() at the exact point the wait loop has observed zero, before post-wait bookkeeping.
struct CounterWaitScope {
  uint32_t inst, site;
  double t0, tWake = -1.0;
  CounterWaitScope(const void* counter, uint32_t siteId) : site(siteId), t0(now()) { inst = ctr_current(counter); }
  void woke() { if (tWake < 0) tWake = now(); }
  ~CounterWaitScope() {
    const double t1 = now();
    emit(CTR_WAIT, tl_frame, inst, tl_job, site, t0, t1);
    if (tWake >= 0) emit(CTR_WAKE, tl_frame, inst, tl_job, site, tWake, t1);
  }
  CounterWaitScope(const CounterWaitScope&) = delete;
  CounterWaitScope& operator=(const CounterWaitScope&) = delete;
};

}

#ifdef WGTRACE_IMPLEMENTATION
extern "C" {
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_ring_count() { auto n=wgtrace::g_ringCount.load();return n<wgtrace::kMaxThreads?n:wgtrace::kMaxThreads; }
EMSCRIPTEN_KEEPALIVE uintptr_t wgtrace_ring_ptr(uint32_t i) { auto* r=i<wgtrace::kMaxThreads?wgtrace::g_rings[i].load(std::memory_order_acquire):nullptr;return r?reinterpret_cast<uintptr_t>(r->recs):0; }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_ring_len(uint32_t i) { auto* r=i<wgtrace::kMaxThreads?wgtrace::g_rings[i].load(std::memory_order_acquire):nullptr;return r?r->head.load(std::memory_order_acquire):0; }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_ring_dropped(uint32_t i) { auto* r=i<wgtrace::kMaxThreads?wgtrace::g_rings[i].load(std::memory_order_acquire):nullptr;return r?r->dropped.load():0; }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_ring_tid(uint32_t i) { auto* r=i<wgtrace::kMaxThreads?wgtrace::g_rings[i].load(std::memory_order_acquire):nullptr;return r?r->tid:0; }
EMSCRIPTEN_KEEPALIVE void wgtrace_reset() {
  uint32_t n = wgtrace_ring_count();
  for (uint32_t i = 0; i < n; i++) {
    auto* r=wgtrace::g_rings[i].load(std::memory_order_acquire);if(!r)continue;
    while(r->writer.test_and_set(std::memory_order_acquire)) {}
    r->head.store(0);r->dropped.store(0);r->writer.clear(std::memory_order_release);
  }
  // Loss counters restart with the capture. The counter-instance table is deliberately kept.
  wgtrace::g_unregisteredEmits.store(0);
  wgtrace::g_ctrTableFull.store(0);
}
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_site_count() { auto n=wgtrace::g_siteCount.load(std::memory_order_acquire);return n<wgtrace::kMaxSites?n:wgtrace::kMaxSites; }
EMSCRIPTEN_KEEPALIVE uintptr_t wgtrace_site_name(uint32_t i) { const char* p=i<wgtrace::kMaxSites?wgtrace::g_siteNames[i].load(std::memory_order_acquire):nullptr;return reinterpret_cast<uintptr_t>(p); }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_ctr_table_full() { return wgtrace::g_ctrTableFull.load(); }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_unregistered_emits() { return wgtrace::g_unregisteredEmits.load(); }
EMSCRIPTEN_KEEPALIVE uint32_t wgtrace_next_id(uint32_t k) { return k<wgtrace::ID_COUNT?wgtrace::next_id(static_cast<wgtrace::IdKind>(k)):0; }
EMSCRIPTEN_KEEPALIVE void wgtrace_set_enabled(uint32_t on) { wgtrace::set_enabled(on!=0); }
}
#endif
