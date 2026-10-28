#pragma once
#include <webgpu/webgpu.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string> // [skperf] round 45

namespace cp2077::web {
// GPU thread only: drop cache ownership, retaining outstanding caller leases.
void ClearGpuImmutableUniformCache();
// [nomadperf] Main thread: run queued worker buffer creations now (called between lists of a long deferred submit).
void ServiceGpuAllocationRequests();
// Copied read-only uniform payload; caller releases its lease via DestroyGpuBuffer.
WGPUBuffer CreateGpuImmutableUniformBuffer(std::size_t bytes,const void* data=nullptr);
// Worker-facing allocation. The returned handle must only be used by platform
// code that marshals operations to the browser main runtime thread.
WGPUBuffer CreateGpuBuffer(std::uint64_t size, WGPUBufferUsage usage,
                          const void* initialData = nullptr, std::size_t initialSize = 0, const char* label = nullptr);
bool DestroyGpuBuffer(WGPUBuffer buffer);
// Actual asynchronous MAP_READ, proxied to the main thread; returns only after
// bytes have been copied or mapping has failed. Caller must submit copies first.
bool ReadGpuBuffer(WGPUBuffer, std::uint64_t mappedBytes, void* output, std::size_t outputBytes);
bool WriteGpuBuffer(WGPUBuffer, const void*, std::size_t);
bool WriteGpuBufferRange(WGPUBuffer, std::uint64_t offset, const void*, std::size_t);
// Many queue writes in one main-thread handoff; issued[i] reports each write. Returns false if the handoff failed.
struct GpuBufferWrite { WGPUBuffer buffer; std::uint64_t offset; const void* bytes; std::size_t size; };
bool WriteGpuBufferRanges(const GpuBufferWrite* writes, std::size_t count, bool* issued);
bool FlushMappedGpuBuffers();
using GpuMappedBufferFlusher = bool (*)();
void SetGpuMappedBufferFlusher(GpuMappedBufferFlusher);
// Original CPU constant data is repacked using the converted shader's exact
// layout before allocating a real uniform buffer. Unknown mappings fail.
WGPUBuffer CreateGpuUniformBuffer(const char* shader, std::uint32_t group,
                                 std::uint32_t binding, const void* nativeData,
                                 std::size_t nativeBytes);
// Worker-only exact lookup in the immutable uniform cache; never allocates. A hit is a lease released via
// DestroyGpuBuffer. Misses return nullptr so callers can suballocate varying payloads instead.
WGPUBuffer FindGpuImmutableUniformBuffer(std::size_t bytes, const void* data);
// [skperf] round 45 uniMemo: the same lookup with a caller-built key (BuildGpuImmutableUniformKey into a reused
// buffer), so a lookup needs no heap allocation. Identical cache semantics (lease, LRU touch, counters).
void BuildGpuImmutableUniformKey(std::string& key, std::size_t bytes, const void* data);
WGPUBuffer FindGpuImmutableUniformBufferKey(const std::string& key, std::size_t bytes);
// Content admission for the immutable cache: true once the same payload has been seen before, so stable
// payloads get a cached buffer while per-frame varying payloads stay out of it.
bool ShouldCacheGpuUniform(std::size_t bytes, const void* data);
// Repacks original constant data into the converted shader's web layout without allocating.
bool RepackGpuUniformBytes(const char* shader, std::uint32_t group, std::uint32_t binding,
                           const void* nativeData, std::size_t nativeBytes, std::vector<unsigned char>& packed);
}
