#include "build.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/internal/RecordingToolCore/src/common/profiling/scopedProfiler.h"
#include <chrono>
#include <cstdio>
static Uint64 RecordingClockNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
recCore::ScopedProfiler::ScopedProfiler(const AnsiChar* message):m_msg(message),m_startCounter(RecordingClockNanoseconds()),m_frequency(1e9) {}
recCore::ScopedProfiler::~ScopedProfiler() {
    const Double elapsed=static_cast<Double>(RecordingClockNanoseconds()-m_startCounter)/m_frequency;
    std::fprintf(stderr,"[recCore::ScopedProfiler] [%s] finished in %f seconds\n",m_msg,elapsed);
}
