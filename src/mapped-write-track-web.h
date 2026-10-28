#pragma once
// [skperf] round 23 mapTrack (default ON, ?mapTrack=0 = full diff of every referenced mapped buffer at each submit).
// A persistently mapped buffer whose owner reports EVERY CPU write (NoteMappedGpuWrite after the bytes are stored)
// keeps a 4 KB page bitmap; the submit-time diff then covers only the marked pages (bits are cleared before the pages
// are read; a write that lands after the clear marks its page again for the next submit, so nothing is lost; a failed
// flush re-marks its pages). The first flush after enabling still diffs the whole buffer once.
// No WebGPU types here: engine units (renderGeometryBatcher.cpp overlay) include it too.
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace GpuApi { struct BufferRef; }
namespace cp2077::web {
struct MappedWriteTracker { std::atomic<std::uint64_t>* words; std::size_t wordCount; std::size_t bytes; };
const MappedWriteTracker* EnableMappedGpuWriteTracking(const GpuApi::BufferRef&); // nullptr: switch off / not mapped
inline void NoteMappedGpuWrite(const MappedWriteTracker* tracker, std::size_t offset, std::size_t bytes) {
    if (!tracker || !bytes || offset >= tracker->bytes) return;
    std::size_t page = offset >> 12, last = (offset + bytes - 1) >> 12;
    const std::size_t maxPage = (tracker->bytes - 1) >> 12;
    if (last > maxPage) last = maxPage;
    while (page <= last) {
        const std::size_t word = page >> 6, bit = page & 63;
        const std::size_t left = last - page + 1, room = 64 - bit, n = left < room ? left : room;
        const std::uint64_t mask = (n == 64 ? ~std::uint64_t(0) : ((std::uint64_t(1) << n) - 1)) << bit;
        if (word < tracker->wordCount) tracker->words[word].fetch_or(mask, std::memory_order_release);
        page += n;
    }
}
}
