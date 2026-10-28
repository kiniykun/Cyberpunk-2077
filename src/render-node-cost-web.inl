#include "render-job-phase-web.h"
#include <atomic>
// [skperf] round 12 render-node probe (?perfprobe=1 only, heap-free): per render-graph node (by name pointer; the
// names are interned CNames) total and max time across all render job threads; every ~2 s prints the top nodes as
// 'CP2077 skperf node probe: [name ms/s calls/s maxMs] ...'. This splits the generic 'RunRenderJob' job.
namespace cp2077::web { extern std::atomic<int> skperfEncodeProbe; }
extern "C" double cp2077_skperf_job_ready(); // [skperf] round 58 (redJobs2 jobDispatcher.cpp)
namespace cpSkperfNodes {
constexpr unsigned kSlots = 256;
struct Slot { std::atomic<const char*> name{nullptr}; std::atomic<unsigned long long> count{0}, us{0}, maxUs{0}; };
inline Slot slots[kSlots];
inline std::atomic<double> windowStart{0};
inline void Print(double seconds) {
    static unsigned long long lastUs[kSlots], lastCount[kSlots];
    unsigned long long d[kSlots], c[kSlots]; bool taken[kSlots] = {};
    for (unsigned i = 0; i < kSlots; ++i) { const auto u = slots[i].us.load(), n = slots[i].count.load(); d[i] = u - lastUs[i]; c[i] = n - lastCount[i]; lastUs[i] = u; lastCount[i] = n; }
    char line[4000]; std::size_t used = 0; // [skperf] round 22: was 1400 (timeline lines were cut after ~1 frame)
    auto add = [&](const char* format, auto... args) {
        if (used + 1 >= sizeof(line)) return;
        const int n = std::snprintf(line + used, sizeof(line) - used, format, args...);
        if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1;
    };
    add("CP2077 skperf node probe: window=%.1fs top(ms/s,calls/s,maxMs):", seconds);
    for (unsigned k = 0; k < 14; ++k) {
        unsigned best = kSlots;
        for (unsigned i = 0; i < kSlots; ++i) if (!taken[i] && d[i] && (best == kSlots || d[i] > d[best])) best = i;
        if (best == kSlots) break;
        taken[best] = true;
        const char* name = slots[best].name.load();
        add(" [%.48s %.1f %.0f %.1f]", name ? name : "?", d[best] / 1000.0 / seconds, c[best] / seconds, slots[best].maxUs.exchange(0) / 1000.0);
    }
    std::fprintf(stderr, "%s\n", line);
}
// [skperf] round 15 R6 timeline (perfprobe only, heap-free): every node invocation goes into a 2048-entry ring
// (name, worker, start, end). Every ~5 s one line shows the node invocations that ended in the last 150 ms (>= 0.5 ms
// each, sorted by start, relative to the earliest), plus per-worker busy ms in that window: the render chain's shape.
constexpr unsigned kTimelineRing = 2048;
struct TimelineEntry { std::atomic<const char*> name{nullptr}; std::atomic<double> start{0}, end{0}; std::atomic<unsigned> worker{0}; std::atomic<double> ready{0}; }; // [skperf] round 58 ready
inline TimelineEntry timeline[kTimelineRing];
inline std::atomic<unsigned> timelineHead{0};
inline std::atomic<double> timelineLastPrint{0};
inline void TimelinePrint(double now) {
    struct Item { const char* name; double start, end; unsigned worker; double ready; };
    Item items[400]; unsigned count = 0;
    double busy[32] = {};
    const double from = now - 150.0;
    for (unsigned i = 0; i < kTimelineRing; ++i) {
        const double e = timeline[i].end.load(std::memory_order_relaxed), s = timeline[i].start.load(std::memory_order_relaxed);
        if (e < from || e > now || s <= 0 || e < s) continue;
        const unsigned w = timeline[i].worker.load(std::memory_order_relaxed);
        if (w < 32) busy[w] += e - (s > from ? s : from);
        if (e - s < 0.5 || count == 400) continue;
        items[count++] = {timeline[i].name.load(std::memory_order_relaxed), s, e, w, timeline[i].ready.load(std::memory_order_relaxed)};
    }
    for (unsigned i = 1; i < count; ++i) { Item x = items[i]; unsigned j = i; while (j && items[j - 1].start > x.start) { items[j] = items[j - 1]; --j; } items[j] = x; }
    char line[4000]; std::size_t used = 0; // [skperf] round 22: was 1400 (timeline lines were cut after ~1 frame)
    auto add = [&](const char* format, auto... args) {
        if (used + 1 >= sizeof(line)) return;
        const int n = std::snprintf(line + used, sizeof(line) - used, format, args...);
        if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1;
    };
    add("CP2077 skperf timeline probe: last150ms busyMs:");
    for (unsigned w = 0; w < 32; ++w) if (busy[w] > 0.05) add(" w%u=%.0f", w, busy[w]);
    add(" nodes(w +startMs durMs name [rReadyMs = when its job became runnable]):");
    const double base = count ? items[0].start : now;
    for (unsigned i = 0; i < count; ++i) {
        if (items[i].ready > 0 && items[i].ready <= items[i].start) add(" [w%u +%.1f %.1f %.22s r%+.1f]", items[i].worker, items[i].start - base, items[i].end - items[i].start, items[i].name ? items[i].name : "?", items[i].ready - base);
        else add(" [w%u +%.1f %.1f %.22s]", items[i].worker, items[i].start - base, items[i].end - items[i].start, items[i].name ? items[i].name : "?");
    }
    std::fprintf(stderr, "%s\n", line);
}
inline void TimelineAdd(const char* name, unsigned worker, double start, double end, double ready = 0) {
    TimelineEntry& entry = timeline[timelineHead.fetch_add(1, std::memory_order_relaxed) % kTimelineRing];
    entry.ready.store(ready, std::memory_order_relaxed); // [skperf] round 58
    entry.name.store(name, std::memory_order_relaxed); entry.worker.store(worker, std::memory_order_relaxed);
    entry.start.store(start, std::memory_order_relaxed); entry.end.store(end, std::memory_order_relaxed);
    double last = timelineLastPrint.load(std::memory_order_relaxed);
    if (last == 0) { timelineLastPrint.compare_exchange_strong(last, end); return; }
    if (end - last >= 5000 && timelineLastPrint.compare_exchange_strong(last, end)) TimelinePrint(end);
}
}
// [skperf] round 20 critical-path probe (probeMask bit 2, heap-free): the game thread marks when it dispatched a
// frame's render commands (FrameTick) and when its wait for them ended (FlushPreviousFrameCommandsProcessing); the
// node timers keep the first node start after the dispatch and the last node end. gameEngine.cpp prints the split.
namespace cpSkperfCrit {
inline std::atomic<double> dispatched{0}, firstStart{0}, lastEnd{0}, presentWait{0}, presentWaitSum{0};
// [skperf] round 22: command-list submissions (CloseAndSubmitCommandLists) of the frame: count, wall time, time to
// get the submit-order mutex, mapped-buffer diff time, and the end of the last submission after the dispatch.
inline std::atomic<double> submitWallSum{0}, submitOrderWaitSum{0}, submitScanSum{0}, lastSubmitEnd{0};
inline std::atomic<unsigned> submitCount{0};
inline std::atomic<double> scanIdsSum{0}, scanSearchSum{0}, scanFlushSum{0}; // [skperf] round 25
inline std::atomic<double> coreParts[7]; // [skperf] round 26: flush core lock/collect/diff/commit/release (+27: bytes, patches)
inline std::atomic<double> loopTimeouts{0}, loopTimedOutMax{0}, loopWokenMax{0}, loopMissed{0}; // [skperf] round 27/28
// [skperf] round 25: the game thread's FlushCounter loop (jobDispatcher.cpp) for the frame wait
inline std::atomic<double> flushLoopExit{0}, flushIters{0}, flushSleeps{0}, flushSleepMs{0}, flushSleepMax{0}, flushOwnJobsMs{0}, flushRequeues{0}, flushLastJobEnd{0};
inline void AddD(std::atomic<double>& a, double v) { double s = a.load(std::memory_order_relaxed); while (!a.compare_exchange_weak(s, s + v)) {} }
inline void Node(double start, double end) {
    const double d = dispatched.load(std::memory_order_relaxed);
    if (d > 0 && start >= d) {
        double f = firstStart.load(std::memory_order_relaxed);
        while ((f == 0 || start < f) && !firstStart.compare_exchange_weak(f, start)) {}
    }
    double l = lastEnd.load(std::memory_order_relaxed);
    while (end > l && !lastEnd.compare_exchange_weak(l, end)) {}
}
}
// [skperf] round 31 chain probe (probeMask bit 2, heap-free): every ~5 s, for the render frame dispatched last time
// (window [previous dispatch, this dispatch)), walk the timeline ring BACKWARDS from the entry that ended last:
// each step takes the entry that ended latest at or before the current entry's start (its likely predecessor on the
// critical path). Prints the chain (name, duration, idle gap before it, worker) plus the summed node time and the
// summed gaps: a chain of long nodes = serial work to split; large gaps = the chain waits on something that is not
// a node or a >=0.5 ms job (main-thread handoffs, small jobs, synchronization).
namespace cpSkperfChain {
inline std::atomic<double> prevDispatch{0}, lastPrint{0};
// [skperf] round 36 chain aggregate: the chain is computed for EVERY frame (probe only, game thread) and its links
// are summed per name, separately for long frames (window > 35 ms: bar mirror frames) and short ones. Every ~5 s:
// 'CP2077 skperf chainagg probe' with frames, average span and the top links (avg ms per frame on the chain).
struct AggSlot { const char* name = nullptr; double ms = 0; unsigned hits = 0; };
inline AggSlot agg[2][96];
inline unsigned aggFrames[2] = {0, 0};
inline double aggSpan[2] = {0, 0};
inline void AggAdd(unsigned cls, const char* name, double ms) {
    if (!name) name = "?";
    for (auto& slot : agg[cls]) {
        if (slot.name == name) { slot.ms += ms; ++slot.hits; return; }
        if (!slot.name) { slot.name = name; slot.ms = ms; slot.hits = 1; return; }
    }
}
inline void AggPrint() {
    for (unsigned cls = 0; cls < 2; ++cls) {
        if (!aggFrames[cls]) continue;
        char line[3000]; std::size_t used = 0;
        auto add = [&](const char* format, auto... args) {
            if (used + 1 >= sizeof(line)) return;
            const int n = std::snprintf(line + used, sizeof(line) - used, format, args...);
            if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1;
        };
        add("CP2077 skperf chainagg probe: %s frames=%u avgChainSpanMs=%.1f top(avg ms/frame on chain, frames):", cls ? "LONG(>35ms)" : "short", aggFrames[cls], aggSpan[cls] / aggFrames[cls]);
        bool taken[96] = {};
        for (unsigned k = 0; k < 18; ++k) {
            int best = -1;
            for (unsigned i = 0; i < 96; ++i) if (agg[cls][i].name && !taken[i] && (best < 0 || agg[cls][i].ms > agg[cls][best].ms)) best = int(i);
            if (best < 0) break;
            taken[best] = true;
            add(" [%.26s %.2f %u]", agg[cls][best].name, agg[cls][best].ms / aggFrames[cls], agg[cls][best].hits);
        }
        std::fprintf(stderr, "%s\n", line);
        for (auto& slot : agg[cls]) slot = AggSlot{};
        aggFrames[cls] = 0; aggSpan[cls] = 0;
    }
}
inline void Print(double from, double to, bool print = true) {
    struct Link { const char* name; double start, end; unsigned worker; };
    Link links[64]; unsigned count = 0;
    double cursor = to + 1e9; bool first = true;
    double nodeSum = 0, gapSum = 0;
    // [skperf] round 32: every timeline entry is used at most once, entries shorter than 0.05 ms are skipped (they
    // made the walk pick the same zero-length bind/unbind node over and over), and a predecessor must END at or
    // before the current link's START (strict).
    static std::uint64_t visited[cpSkperfNodes::kTimelineRing / 64];
    for (auto& v : visited) v = 0;
    for (; count < 64;) {
        int best = -1; double bestEnd = -1, bestStart = 0;
        for (unsigned i = 0; i < cpSkperfNodes::kTimelineRing; ++i) {
            if (visited[i >> 6] & (std::uint64_t(1) << (i & 63))) continue;
            const auto& e = cpSkperfNodes::timeline[i];
            const double s = e.start.load(std::memory_order_relaxed), en = e.end.load(std::memory_order_relaxed);
            if (en - s < 0.05 || s < from || en > to) continue;
            if (!first && en > cursor) continue;
            if (en > bestEnd || (en == bestEnd && s < bestStart)) { bestEnd = en; bestStart = s; best = int(i); }
        }
        if (best < 0) break;
        visited[unsigned(best) >> 6] |= std::uint64_t(1) << (unsigned(best) & 63);
        const auto& e = cpSkperfNodes::timeline[best];
        links[count++] = {e.name.load(std::memory_order_relaxed), bestStart, bestEnd, e.worker.load(std::memory_order_relaxed)};
        nodeSum += bestEnd - bestStart;
        if (!first) gapSum += cursor > bestEnd ? cursor - bestEnd : 0;
        first = false;
        cursor = bestStart;
        if (cursor <= from) break;
    }
    if (!count) return;
    {   // [skperf] round 36 aggregate
        const unsigned cls = (to - from) > 35.0 ? 1u : 0u;
        ++aggFrames[cls]; aggSpan[cls] += links[0].end - links[count - 1].start;
        for (unsigned i = 0; i < count; ++i) AggAdd(cls, links[i].name, links[i].end - links[i].start);
    }
    if (!print) return;
    char line[4000]; std::size_t used = 0;
    auto add = [&](const char* format, auto... args) {
        if (used + 1 >= sizeof(line)) return;
        const int n = std::snprintf(line + used, sizeof(line) - used, format, args...);
        if (n > 0) used = (used + std::size_t(n) < sizeof(line)) ? used + std::size_t(n) : sizeof(line) - 1;
    };
    add("CP2077 skperf chain probe: frameWindowMs=%.1f chainSpanMs=%.1f chainNodeMs=%.1f chainGapMs=%.1f links=%u (latest first: name durMs gapBeforeMs wN):",
        to - from, links[0].end - links[count - 1].start, nodeSum, gapSum, count);
    for (unsigned i = 0; i < count; ++i) {
        const double gap = (i + 1 < count) ? links[i].start - links[i + 1].end : links[i].start - from;
        add(" [%.26s %.1f %.1f w%u]", links[i].name ? links[i].name : "?", links[i].end - links[i].start, gap > 0 ? gap : 0.0, links[i].worker);
    }
    std::fprintf(stderr, "%s\n", line);
}
}
extern "C" void cp2077_skperf_crit_dispatched(double now) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) != 0) {
        const double prev = cpSkperfChain::prevDispatch.load(std::memory_order_relaxed);
        if (prev > 0) {
            const bool due = now - cpSkperfChain::lastPrint.load(std::memory_order_relaxed) >= 5000;
            if (due) cpSkperfChain::lastPrint.store(now, std::memory_order_relaxed);
            cpSkperfChain::Print(prev, now, due); // [skperf] round 36: every frame feeds the aggregate
            if (due) cpSkperfChain::AggPrint();
        }
        cpSkperfChain::prevDispatch.store(now, std::memory_order_relaxed);
    }
    cpSkperfCrit::dispatched.store(now, std::memory_order_relaxed);
    cpSkperfCrit::firstStart.store(0, std::memory_order_relaxed);
    cpSkperfCrit::lastEnd.store(0, std::memory_order_relaxed);
    cpSkperfCrit::lastSubmitEnd.store(0, std::memory_order_relaxed);
}
extern "C" int cp2077_skperf_crit_on() { return (cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) != 0; }
extern "C" void cp2077_skperf_crit_submit(double start, double end, double orderWaitMs, double scanMs) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    cpSkperfCrit::submitCount.fetch_add(1, std::memory_order_relaxed);
    cpSkperfCrit::AddD(cpSkperfCrit::submitWallSum, end - start);
    cpSkperfCrit::AddD(cpSkperfCrit::submitOrderWaitSum, orderWaitMs);
    cpSkperfCrit::AddD(cpSkperfCrit::submitScanSum, scanMs);
    double l = cpSkperfCrit::lastSubmitEnd.load(std::memory_order_relaxed);
    while (end > l && !cpSkperfCrit::lastSubmitEnd.compare_exchange_weak(l, end)) {}
}
// [skperf] round 23: every finished job (probeMask 1 and 2) goes into a 4096-entry ring, so the game thread can
// list what ran between the last submission and the end of its wait (the frame's tail).
namespace cpSkperfCrit {
constexpr unsigned kJobRing = 4096;
struct JobEntry { std::atomic<const char*> name{nullptr}; std::atomic<double> start{0}, end{0}; };
inline JobEntry jobRing[kJobRing];
inline std::atomic<unsigned> jobHead{0};
}
extern "C" void cp2077_skperf_crit_job(const char* name, double start, double end) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    auto& e = cpSkperfCrit::jobRing[cpSkperfCrit::jobHead.fetch_add(1, std::memory_order_relaxed) % cpSkperfCrit::kJobRing];
    e.name.store(name, std::memory_order_relaxed); e.start.store(start, std::memory_order_relaxed); e.end.store(end, std::memory_order_relaxed);
}
// Jobs overlapping [from, to]: out[0] count, out[1] busy ms inside the window, out[2] last job end (<= to),
// out[3] longest overlap ms; *longest = its name.
extern "C" void cp2077_skperf_crit_tail(double from, double to, double* out, const char** longest) {
    out[0] = out[1] = out[2] = out[3] = 0; *longest = "-";
    for (unsigned i = 0; i < cpSkperfCrit::kJobRing; ++i) {
        const auto& e = cpSkperfCrit::jobRing[i];
        const double s = e.start.load(std::memory_order_relaxed), en = e.end.load(std::memory_order_relaxed);
        if (en <= from || s >= to || en < s) continue;
        const double overlap = (en < to ? en : to) - (s > from ? s : from);
        out[0] += 1; out[1] += overlap;
        if (en <= to && en > out[2]) out[2] = en;
        if (overlap > out[3]) { out[3] = overlap; const char* n = e.name.load(std::memory_order_relaxed); *longest = n ? n : "?"; }
    }
}
// [skperf] round 24: every job counter that reaches ZERO (job finished or deferral finished) goes into a ring with
// its debug name and whether a job's own completion did it. The tail analysis then names what completed last
// before the game thread's wait ended, and whether that was a deferral (non-job completion).
namespace cpSkperfCrit {
struct ZeroEntry { std::atomic<const char*> name{nullptr}; std::atomic<double> t{0}; std::atomic<int> fromJob{0}; };
inline ZeroEntry zeroRing[kJobRing];
inline std::atomic<unsigned> zeroHead{0};
}
extern "C" void cp2077_skperf_crit_zero(const char* name, int fromJob) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    auto& e = cpSkperfCrit::zeroRing[cpSkperfCrit::zeroHead.fetch_add(1, std::memory_order_relaxed) % cpSkperfCrit::kJobRing];
    e.name.store(name, std::memory_order_relaxed); e.fromJob.store(fromJob, std::memory_order_relaxed); e.t.store(emscripten_get_now(), std::memory_order_relaxed);
}
// Counters that reached zero in (from, to]: out[0] count, out[1] count not caused by a job, out[2] time of the last one,
// out[3] whether the last one came from a job; *lastName its debug name.
extern "C" void cp2077_skperf_crit_zerotail(double from, double to, double* out, const char** lastName) {
    out[0] = out[1] = out[2] = out[3] = 0; *lastName = "-";
    for (unsigned i = 0; i < cpSkperfCrit::kJobRing; ++i) {
        const auto& e = cpSkperfCrit::zeroRing[i];
        const double t = e.t.load(std::memory_order_relaxed);
        if (t <= from || t > to) continue;
        const int fj = e.fromJob.load(std::memory_order_relaxed);
        out[0] += 1; if (!fj) out[1] += 1;
        if (t > out[2]) { out[2] = t; out[3] = fj; const char* n = e.name.load(std::memory_order_relaxed); *lastName = n ? n : "?"; }
    }
}
extern "C" void cp2077_skperf_crit_scan_parts(double idsMs, double searchMs, double flushMs) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    cpSkperfCrit::AddD(cpSkperfCrit::scanIdsSum, idsMs); cpSkperfCrit::AddD(cpSkperfCrit::scanSearchSum, searchMs); cpSkperfCrit::AddD(cpSkperfCrit::scanFlushSum, flushMs);
}
extern "C" void cp2077_skperf_crit_core_parts(double lockMs, double collectMs, double diffMs, double commitMs, double releaseMs, double bytes, double patches) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    const double v[7] = {lockMs, collectMs, diffMs, commitMs, releaseMs, bytes, patches};
    for (unsigned i = 0; i < 7; ++i) cpSkperfCrit::AddD(cpSkperfCrit::coreParts[i], v[i]);
}
extern "C" void cp2077_skperf_crit_read_core_parts(double* out) { for (unsigned i = 0; i < 7; ++i) out[i] = cpSkperfCrit::coreParts[i].exchange(0); }
extern "C" void cp2077_skperf_crit_flushloop2(double timeouts, double timedOutMaxMs, double wokenMaxMs, double missed) {
    cpSkperfCrit::loopTimeouts.store(timeouts); cpSkperfCrit::loopTimedOutMax.store(timedOutMaxMs); cpSkperfCrit::loopWokenMax.store(wokenMaxMs);
    cpSkperfCrit::loopMissed.store(missed);
}
extern "C" void cp2077_skperf_crit_read_flushloop2(double* out) {
    out[0] = cpSkperfCrit::loopTimeouts.load(); out[1] = cpSkperfCrit::loopTimedOutMax.load(); out[2] = cpSkperfCrit::loopWokenMax.load();
    out[3] = cpSkperfCrit::loopMissed.load();
}
extern "C" void cp2077_skperf_crit_read_scan_parts(double* out) {
    out[0] = cpSkperfCrit::scanIdsSum.exchange(0); out[1] = cpSkperfCrit::scanSearchSum.exchange(0); out[2] = cpSkperfCrit::scanFlushSum.exchange(0);
}
// Game-thread frame wait loop stats (written by jobDispatcher.cpp for the wait, read once per frame by gameEngine).
extern "C" void cp2077_skperf_crit_flushloop(double loopExit, double iters, double sleeps, double sleepMs, double sleepMax, double ownJobsMs, double requeues, double lastJobEnd) {
    cpSkperfCrit::flushLoopExit.store(loopExit); cpSkperfCrit::flushIters.store(iters); cpSkperfCrit::flushSleeps.store(sleeps);
    cpSkperfCrit::flushSleepMs.store(sleepMs); cpSkperfCrit::flushSleepMax.store(sleepMax); cpSkperfCrit::flushOwnJobsMs.store(ownJobsMs);
    cpSkperfCrit::flushRequeues.store(requeues); cpSkperfCrit::flushLastJobEnd.store(lastJobEnd);
}
extern "C" void cp2077_skperf_crit_read_flushloop(double* out) {
    out[0] = cpSkperfCrit::flushLoopExit.load(); out[1] = cpSkperfCrit::flushIters.load(); out[2] = cpSkperfCrit::flushSleeps.load();
    out[3] = cpSkperfCrit::flushSleepMs.load(); out[4] = cpSkperfCrit::flushSleepMax.load(); out[5] = cpSkperfCrit::flushOwnJobsMs.load();
    out[6] = cpSkperfCrit::flushRequeues.load(); out[7] = cpSkperfCrit::flushLastJobEnd.load();
}
// out[0..4]: submissions since the last read, their wall ms, order-wait ms, mapped-scan ms, last submission end.
extern "C" void cp2077_skperf_crit_read_submits(double* out) {
    out[0] = cpSkperfCrit::submitCount.exchange(0);
    out[1] = cpSkperfCrit::submitWallSum.exchange(0);
    out[2] = cpSkperfCrit::submitOrderWaitSum.exchange(0);
    out[3] = cpSkperfCrit::submitScanSum.exchange(0);
    out[4] = cpSkperfCrit::lastSubmitEnd.load(std::memory_order_relaxed);
}
extern "C" void cp2077_skperf_crit_present_wait(double ms) { // [skperf] round 21: render-side Present wait
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return;
    double s = cpSkperfCrit::presentWaitSum.load(std::memory_order_relaxed);
    while (!cpSkperfCrit::presentWaitSum.compare_exchange_weak(s, s + ms)) {}
}
// Returns 0 when the probe is off. out[0] dispatch time, out[1] first node start (0 = none), out[2] last node end,
// out[3] Present wait accumulated since the last read (ms).
extern "C" int cp2077_skperf_crit_read(double* out) {
    if ((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) == 0) return 0;
    out[0] = cpSkperfCrit::dispatched.load(std::memory_order_relaxed);
    out[1] = cpSkperfCrit::firstStart.load(std::memory_order_relaxed);
    out[2] = cpSkperfCrit::lastEnd.load(std::memory_order_relaxed);
    out[3] = cpSkperfCrit::presentWaitSum.exchange(0);
    return 1;
}
// [skperf] round 16: the job dispatcher (redJobs2) adds non-node jobs (culling, collection, ...) >= 0.5 ms to the same
// timeline, so the gaps between render nodes are attributed. Defined once here (this .inl is included by one unit).
extern "C" void cp2077_skperf_timeline_job(const char* name, unsigned worker, double start, double end) {
    cpSkperfNodes::TimelineAdd(name, worker, start, end);
}
namespace cpSkperfNodes {
inline void Note(const char* name, double ms) {
    const auto us = (unsigned long long)(ms * 1000.0);
    const unsigned h = unsigned(((reinterpret_cast<std::uintptr_t>(name) >> 3) * 2654435761u) % kSlots);
    for (unsigned probe = 0; probe < 16; ++probe) {
        Slot& slot = slots[(h + probe) % kSlots];
        const char* current = slot.name.load(std::memory_order_acquire);
        if (!current) { const char* expected = nullptr; if (slot.name.compare_exchange_strong(expected, name)) current = name; else current = expected; }
        if (current != name) continue;
        slot.count.fetch_add(1, std::memory_order_relaxed); slot.us.fetch_add(us, std::memory_order_relaxed);
        auto previous = slot.maxUs.load(std::memory_order_relaxed);
        while (us > previous && !slot.maxUs.compare_exchange_weak(previous, us)) {}
        break;
    }
    const double now = emscripten_get_now();
    double start = windowStart.load(std::memory_order_relaxed);
    if (start == 0) { windowStart.compare_exchange_strong(start, now); return; }
    if (now - start >= 2000 && windowStart.compare_exchange_strong(start, now)) Print((now - start) / 1000.0);
}
}
namespace {
struct CpRenderNodeTimer {
    // perf12: timing and the "render node cost probe" lines only with ?probes=1. Each stderr line from a render worker
    // is a synchronous main-thread handoff (fd_write), ~85 ms/s of render-thread wait on BODY TYPE.
    const char* name;bool on;double start;cpRenderJobPhase::Guard active;
    bool skperf; // [skperf] round 12 node probe
    // [skperf] round 32: RenderElements / RenderShadow* nodes are named after their command list (e.g.
    // GBufferOnly_Regular) in the skperf node/timeline/chain probes, so the long ones can be told apart.
    static const char* CpProbeName(const char* value, const char* listName) {
        if (!value || !listName) return value;
        return (std::strstr(value, "RenderElements") || std::strstr(value, "RenderShadow")) ? listName : value;
    }
    const char* probeName;
    explicit CpRenderNodeTimer(const char* value, const char* listName = nullptr):name(value?value:"<unnamed>"),on(cpEngineTrace::ProbesEnabled()),start(0),active(job::GetDispatcherThreadIndex(),name),
        skperf((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed)&2)!=0){ probeName=skperf?CpProbeName(name,listName):name; /* [skperf] probeMask bit 2 (round 41: strstr only while probing) */if(on||skperf)start=emscripten_get_now();active.Phase(6);}
    ~CpRenderNodeTimer(){
        if(!on&&!skperf)return;
        const double elapsed=emscripten_get_now()-start;
        if(skperf){cpSkperfNodes::Note(probeName,elapsed);cpSkperfNodes::TimelineAdd(probeName,job::GetDispatcherThreadIndex(),start,start+elapsed,cp2077_skperf_job_ready());cpSkperfCrit::Node(start,start+elapsed);} // [skperf] round 15 R6 / round 20 crit
        if(!on)return;
        struct Record {char name[128]{};unsigned count=0,slow=0;double total=0,max=0;};
        static thread_local Record records[64];static thread_local unsigned used=0;
        Record* record=nullptr;
        for(unsigned i=0;i<used;++i)if(std::strncmp(records[i].name,name,127)==0){record=&records[i];break;}
        if(!record && used<64){record=&records[used++];std::snprintf(record->name,sizeof(record->name),"%s",name);}
        if(!record)return;
        const unsigned n=++record->count;record->total+=elapsed;record->max=std::max(record->max,elapsed);
        if(n<=2 || (n<=2048 && (n&(n-1))==0) || (elapsed>=20 && ++record->slow<=8))
            std::fprintf(stderr,"CP2077 render node cost probe: worker=%u name=%s count=%u lastMs=%.3f totalMs=%.3f maxMs=%.3f\n",
                job::GetDispatcherThreadIndex(),record->name,n,elapsed,record->total,record->max);
    }
};
}
