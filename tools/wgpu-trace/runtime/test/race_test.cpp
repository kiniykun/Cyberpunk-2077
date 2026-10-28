// Deterministic interleaving tests for the counter-generation hooks.
// Each scenario parks a thread at a named race point, runs the competing thread to completion, then
// releases the parked one, so the interleaving is forced rather than hoped for.
#define WGTRACE_IMPLEMENTATION
#include "race_hooks.h"
#include "../wgtrace.h"

#include <cstdio>
#include <string_view>
#include <thread>
#include <vector>

uint32_t tu2_dec_begin_inst(const void* counter);
uint32_t tu2_site();

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::vector<wgtrace::Rec> collect() {
  std::vector<wgtrace::Rec> out;
  uint32_t n = wgtrace_ring_count();
  for (uint32_t i = 0; i < n; i++) {
    auto* r = wgtrace::g_rings[i].load(std::memory_order_acquire);
    if (!r) continue;
    uint32_t len = r->head.load(std::memory_order_acquire);
    for (uint32_t k = 0; k < len; k++) out.push_back(r->recs[k]);
  }
  return out;
}

static uint64_t addrOf(const wgtrace::Rec& r) { return static_cast<uint64_t>(r.b) | (static_cast<uint64_t>(r.c) << 32); }
static uint64_t key(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }

static void startThread(std::vector<std::thread>& ts, int index, uint32_t tid, void (*body)()) {
  ts.emplace_back([=] { racetest::testThread = index; wgtrace::thread_register(tid); body(); });
}

// --- scenario 1: two first sightings race to create the implicit instance -----------------------------
static std::atomic<int64_t> c1{0};
static std::atomic<uint32_t> s1inst[2];
static void s1body() { s1inst[racetest::testThread].store(wgtrace::ctr_dec_begin(&c1).inst); }

// --- scenario 2: a contributor reads the previous instance before the new generation is published ------
static std::atomic<int64_t> c2{0};
static std::atomic<int> s2step{0};
static std::atomic<int> s2go{0};
static void s2threadA() {
  // generation X: job 101 adds and brings the counter back to zero
  { auto t = wgtrace::ctr_add_begin(); int64_t b = c2.fetch_add(1); wgtrace::ctr_add_end(t, &c2, 101, 1, b); }
  { wgtrace::JobScope js(1, 101, 0); auto t = wgtrace::ctr_dec_begin(&c2); int64_t a = c2.fetch_sub(1) - 1; wgtrace::ctr_dec_end(t, 101, a); }
  s2step.store(1);
  while (!s2go.load()) {}  // main arms the park point only now, so generation X is not parked
  // generation G: job 102's fetch_add returns 0; this thread parks before publishing G
  { auto t = wgtrace::ctr_add_begin(); int64_t b = c2.fetch_add(1); wgtrace::ctr_add_end(t, &c2, 102, 1, b); }
  while (s2step.load() < 2) {}
  { wgtrace::JobScope js(1, 102, 0); auto t = wgtrace::ctr_dec_begin(&c2); int64_t a = c2.fetch_sub(1) - 1; wgtrace::ctr_dec_end(t, 102, a); }
}
static void s2threadB() {
  // job 103 joins generation G (fetch_add returns 1) but reads the stale instance X
  { auto t = wgtrace::ctr_add_begin(); int64_t b = c2.fetch_add(1); wgtrace::ctr_add_end(t, &c2, 103, 1, b); }
  { wgtrace::JobScope js(1, 103, 1); auto t = wgtrace::ctr_dec_begin(&c2); int64_t a = c2.fetch_sub(1) - 1; wgtrace::ctr_dec_end(t, 103, a); }
}

// --- scenario 4: the decrementer is preempted after its fetch_sub, before its end stamp ---------------------
static std::atomic<int64_t> c4{0};
static std::atomic<int> s4go{0};
static double s4frameT0 = 0;
static void s4waiter() {
  s4frameT0 = wgtrace::now();
  {
    wgtrace::JobScope js(4, 400, 0);
    { auto t = wgtrace::ctr_add_begin(); int64_t b = c4.fetch_add(1); wgtrace::ctr_add_end(t, &c4, 401, 1, b); }
    s4go.store(1);
    wgtrace::CounterWaitScope w(&c4, WGTRACE_SITE("RaceTest/WaitC4"));
    while (c4.load() != 0) {}
    w.woke();
  }
  wgtrace::frame_end(4, s4frameT0, wgtrace::now());
}
static void s4decrementer() {
  while (!s4go.load()) {}
  { wgtrace::JobScope js(4, 401, 1); }   // job body; the engine decrements in its epilogue, outside the scope
  auto d = wgtrace::ctr_dec_begin(&c4);
  int64_t after = c4.fetch_sub(1) - 1;
  wgtrace::ctr_dec_end(d, 401, after);  // parks before taking the end stamp
}

// --- scenario 3: re-arm after the first generation uses exchange, not CAS from 0 ------------------------
static std::atomic<int64_t> c3{0};

static void writeJson(const char* path, const std::vector<wgtrace::Rec>& recs) {
  FILE* f = std::fopen(path, "wb");
  if (!f) { std::printf("FAIL cannot open %s\n", path); failures++; return; }
  std::fprintf(f, "{\"meta\":{\"droppedWasm\":0,\"unregisteredEmits\":%u,\"siteNames\":{", wgtrace_unregistered_emits());
  for (uint32_t i = 0; i < wgtrace_site_count(); i++) {
    const char* s = reinterpret_cast<const char*>(wgtrace_site_name(i));
    std::fprintf(f, "%s\"%u\":\"%s\"", i ? "," : "", i + 1, s ? s : "");
  }
  std::fprintf(f, "}},\"records\":[");
  for (size_t i = 0; i < recs.size(); i++) {
    const auto& r = recs[i];
    std::fprintf(f, "%s{\"type\":%u,\"frameId\":%u,\"a\":%u,\"b\":%u,\"c\":%u,\"tid\":%u,\"t0\":%.6f,\"t1\":%.6f}",
                 i ? "," : "", r.type, r.frameId, r.a, r.b, r.c, r.tid, r.t0, r.t1);
  }
  std::fprintf(f, "]}\n");
  std::fclose(f);
}

int main(int argc, char** argv) {
  wgtrace::thread_register(1000);
  wgtrace::set_enabled(true);

  // scenario 1
  {
    racetest::arm(wgtrace::RP_IMPLICIT_BEFORE_CAS, racetest::kAnyThread);
    std::vector<std::thread> ts;
    startThread(ts, 0, 1, s1body);
    startThread(ts, 1, 2, s1body);
    racetest::waitParked(2);  // both read an empty slot and allocated competing ids
    racetest::releaseAll();
    for (auto& t : ts) t.join();
    CHECK(s1inst[0].load() != 0);
    CHECK(s1inst[0].load() == s1inst[1].load());
    int arms = 0; uint32_t armInst = 0;
    for (auto& r : collect()) if (r.type == wgtrace::CTR_ARM && addrOf(r) == key(&c1)) { arms++; armInst = r.a; }
    CHECK(arms == 1);
    CHECK(armInst == s1inst[0].load());
    CHECK(tu2_dec_begin_inst(&c1) == s1inst[0].load());  // the other TU sees the same table
  }

  // scenario 2
  {
    std::vector<std::thread> ts;
    startThread(ts, 10, 10, s2threadA);
    while (s2step.load() < 1) {}
    racetest::arm(wgtrace::RP_ADD_BEFORE_PUBLISH, 10);
    s2go.store(1);
    racetest::waitParked(1);       // A's fetch_add returned 0; G not yet published
    startThread(ts, 11, 11, s2threadB);
    ts.back().join();              // B completes add + decrement against the stale instance
    racetest::releaseAll();
    s2step.store(2);
    ts.front().join();
    CHECK(c2.load() == 0);

    uint32_t instX = 0, instG = 0, contrib103 = 0, dec103 = 0, dec102 = 0;
    double zeroXEnd = 0, add103Begin = 0, dec103Begin = 0;
    for (auto& r : collect()) {
      if (r.type == wgtrace::CTR_CONTRIB && r.b == 101) instX = r.a;
      if (r.type == wgtrace::CTR_CONTRIB && r.b == 102) instG = r.a;
      if (r.type == wgtrace::CTR_CONTRIB && r.b == 103) { contrib103 = r.a; add103Begin = r.t0; }
      if (r.type == wgtrace::CTR_DEC && r.b == 101 && r.c == 0) zeroXEnd = r.t1;
      if (r.type == wgtrace::CTR_DEC && r.b == 103) { dec103 = r.a; dec103Begin = r.t0; }
      if (r.type == wgtrace::CTR_DEC && r.b == 102) dec102 = r.a;
    }
    CHECK(instX != 0 && instG != 0 && instX != instG);
    CHECK(contrib103 == instX);   // the race happened: stale tag
    CHECK(dec103 == instX);
    CHECK(dec102 == instG);
    CHECK(add103Begin > zeroXEnd); // ... and the stamps prove it, which is what the analyzer relies on
    CHECK(dec103Begin > zeroXEnd);
  }

  // scenario 3: three generations on one address, each publishes a distinct explicit instance
  {
    uint32_t seen[3] = {0, 0, 0};
    for (int g = 0; g < 3; g++) {
      auto t = wgtrace::ctr_add_begin(); int64_t b = c3.fetch_add(1); wgtrace::ctr_add_end(t, &c3, 300 + g, 1, b);
      auto d = wgtrace::ctr_dec_begin(&c3);
      seen[g] = d.inst;
      int64_t after = c3.fetch_sub(1) - 1;
      wgtrace::ctr_dec_end(d, 300 + g, after);
    }
    CHECK(seen[0] && seen[1] && seen[2]);
    CHECK(seen[0] != seen[1] && seen[1] != seen[2]);
    int explicitArms = 0;
    for (auto& r : collect()) if (r.type == wgtrace::CTR_ARM && addrOf(r) == key(&c3) && r.t1 == 0.0) explicitArms++;
    CHECK(explicitArms == 3);
  }

  // scenario 4
  {
    racetest::arm(wgtrace::RP_DEC_BEFORE_END_STAMP, 21);
    std::vector<std::thread> ts;
    startThread(ts, 21, 21, s4decrementer);
    startThread(ts, 20, 20, s4waiter);
    ts.back().join();          // the waiter observes zero and exits while the decrementer is parked
    racetest::waitParked(1);
    racetest::releaseAll();
    ts.front().join();
    double wake = 0, decEnd = 0, decBegin = 0;
    for (auto& r : collect()) {
      if (r.type == wgtrace::CTR_WAKE && r.b == 400) wake = r.t0;
      if (r.type == wgtrace::CTR_DEC && r.b == 401) { decBegin = r.t0; decEnd = r.t1; }
    }
    CHECK(wake > 0 && decEnd > 0);
    CHECK(decBegin <= wake);
    CHECK(wake < decEnd);      // the end stamp alone would place the zero after the wake
  }

  // site names publish across TUs; unregistered emits are counted, not written
  {
    uint32_t a = tu2_site(), b = tu2_site();
    CHECK(a != 0 && a == b);
    CHECK(std::string_view(reinterpret_cast<const char*>(wgtrace_site_name(a - 1))) == "RaceTest/SiteFromTU2");
    std::thread([] { wgtrace::job_spawn(1, 1, 1); }).join();
    CHECK(wgtrace_unregistered_emits() == 1);
  }

  // scenario 5: heap churn like CounterEntry (RED_NEW / RED_DELETE). Without retire this filled the table
  // in the engine capture (548,239 overflows); with retire the table must never fill and probes stay short.
  {
    wgtrace::set_enabled(false);  // table maintenance runs regardless; keep the rings for the analyzer trace
    const uint32_t liveBefore = wgtrace::g_ctrLive.load();
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++) ts.emplace_back([] {
      for (int n = 0; n < 50000; n++) {
        auto* c = new std::atomic<int64_t>(0);
        { auto a = wgtrace::ctr_add_begin(); int64_t b = c->fetch_add(1); wgtrace::ctr_add_end(a, c, 0, 1, b); }
        { auto d = wgtrace::ctr_dec_begin(c); int64_t after = c->fetch_sub(1) - 1; wgtrace::ctr_dec_end(d, 0, after); }
        wgtrace::ctr_retire(c);
        delete c;
      }
    });
    for (auto& t : ts) t.join();
    wgtrace::set_enabled(true);
    CHECK(wgtrace_ctr_table_full() == 0);
    CHECK(wgtrace::g_ctrLive.load() == liveBefore);
    CHECK(wgtrace::g_ctrMaxProbe.load() < 64);
    std::printf("churn: 200000 counters, max probe %u, live %u\n", wgtrace::g_ctrMaxProbe.load(), wgtrace::g_ctrLive.load());
  }

  if (argc > 1) writeJson(argv[1], collect());
  std::printf(failures ? "race tests: %d FAILED\n" : "race tests: all passed\n", failures);
  return failures ? 1 : 0;
}
