#pragma once
// Test-only park/release mechanism compiled into the counter hooks through WGTRACE_RACE_POINT.
#include <atomic>
#include <cstdint>

namespace racetest {
constexpr int kAnyThread = -2;
inline std::atomic<uint32_t> parkPoint{0};
inline std::atomic<int> parkThread{-1};
inline std::atomic<int> parked{0};
inline std::atomic<int> release{0};
inline thread_local int testThread = -1;

inline void point(uint32_t p) {
  if (p != parkPoint.load(std::memory_order_acquire)) return;
  int who = parkThread.load(std::memory_order_acquire);
  if (who != kAnyThread && who != testThread) return;
  parked.fetch_add(1, std::memory_order_acq_rel);
  while (!release.load(std::memory_order_acquire)) {}
}

inline void arm(uint32_t p, int thread) {
  release.store(0); parked.store(0);
  parkThread.store(thread); parkPoint.store(p, std::memory_order_release);
}
inline void waitParked(int n) { while (parked.load(std::memory_order_acquire) < n) {} }
inline void releaseAll() { parkPoint.store(0); release.store(1, std::memory_order_release); }
}

#define WGTRACE_RACE_POINT(p, counter) ::racetest::point(p)
