#pragma once
#include <emscripten/emscripten.h>

namespace red
{
    // Browser monotonic milliseconds become integer microsecond ticks. All
    // conversions use the matching frequency rather than an invented CPU rate.
    class Timer
    {
    public:
        Timer() : m_ticksPerSecond(1000000), m_secondsPerTick(0.000001),
                  m_startSecondsNegated(0.0) { Reset(); }
        RED_FORCE_INLINE static Uint64 GetTicks()
        {
            return static_cast<Uint64>(emscripten_get_now() * 1000.0);
        }
        RED_FORCE_INLINE static void GetTicks(Uint64& time) { time = GetTicks(); }
        RED_FORCE_INLINE Double GetFrequency() const { return static_cast<Double>(m_ticksPerSecond); }
        RED_FORCE_INLINE void GetFrequency(Uint64& frequency) const { frequency = m_ticksPerSecond; }
        RED_INLINE Double GetSeconds() const { return GetTicks() * m_secondsPerTick + m_startSecondsNegated; }
        RED_FORCE_INLINE void GetSeconds(Double& seconds) const { seconds = GetSeconds(); }
        // Convert before negating: -Uint64 wraps to ~2^64, which put the offset near 1.8e13 s and
        // quantized GetSeconds() to 1/256 s (zero frame deltas -> LoopSingleTick exit -> Shutdown).
        RED_FORCE_INLINE void Reset() { m_startSecondsNegated = -( static_cast<Double>(GetTicks()) * m_secondsPerTick ); }
    private:
        Uint64 m_ticksPerSecond;
        Double m_secondsPerTick;
        Double m_startSecondsNegated;
    };
}
