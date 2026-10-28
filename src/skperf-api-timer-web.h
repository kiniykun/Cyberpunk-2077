#pragma once
// [skperf] round 39: per-thread time spent in GpuApi state calls OUTSIDE draws (buffer lock/unlock, constant-buffer
// and texture binds, vertex/index binds, VSC/PSC constants, shader and sampler binds). Only with ?perfprobe=1
// (probeMask bit 2); the engine's geometry probe reads and resets the totals around each big RenderGeometry call to
// split its 'engine side' time. Categories: 0 lock/unlock buffer, 1 bind CB, 2 VB/IB, 3 VSC/PSC, 4 shader,
// 5 textures, 6 samplers.
#include <atomic>
#include <emscripten/emscripten.h>
namespace cp2077 { namespace web { extern std::atomic<int> skperfEncodeProbe; } }
extern "C" void cp2077_skperf_tl_api_note(int category, double ms);
struct CpApiTimer {
    int category; double start;
    explicit CpApiTimer(int c) : category(c), start((cp2077::web::skperfEncodeProbe.load(std::memory_order_relaxed) & 2) ? emscripten_get_now() : 0) {}
    ~CpApiTimer() { if (start > 0) cp2077_skperf_tl_api_note(category, emscripten_get_now() - start); }
};
