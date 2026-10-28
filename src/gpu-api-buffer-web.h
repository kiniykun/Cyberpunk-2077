#pragma once
#include <webgpu/webgpu.h>
#include <vector>
#include <cstddef>
#include <cstdint>
namespace GpuApi { struct BufferRef; struct BufferDesc; }
namespace cp2077::web {
void ShutdownGpuBuffers();
// Acquires one original reference; caller releases it or transfers it to a command list.
bool AcquireGpuBuffer(const GpuApi::BufferRef&, GpuApi::BufferDesc&, WGPUBuffer&);
// [skperf] round 60 geoKeep: same as AcquireGpuBuffer but takes `count` references under one bufferMutex lock.
bool AcquireGpuBufferMulti(const GpuApi::BufferRef&, int count, GpuApi::BufferDesc&, WGPUBuffer&);
// [skperf] round 60 geoKeep: releases `count` references (null entries skipped, duplicates allowed) taking bufferMutex once;
// any release that would destroy its buffer goes through GpuApi::Release afterwards (identical end state).
void ReleaseGpuBuffersBatch(const GpuApi::BufferRef* refs, std::size_t count);
// [skperf] round 61 cbKeep (default ON since round 62, ?cbKeep=0 old): constant-buffer bind / unlock take bufferMutex once per buffer.
bool CpCbKeepOn();
// [skperf] round 61: descriptor + native of a live buffer under one lock, no reference taken (false like AcquireGpuBuffer).
bool PeekGpuBuffer(const GpuApi::BufferRef&, GpuApi::BufferDesc&, WGPUBuffer&);
// Acquires an original ref protecting its actual separate append counter.
bool AcquireGpuBufferCounter(const GpuApi::BufferRef&, WGPUBuffer&);
// Reflection-probe constants receive real GPU irradiance copies. Their native
// byte layout stays on the GPU and is repacked at each shader read.
bool UsesNativeGpuConstantBacking(const GpuApi::BufferRef&);
bool CopyMappedGpuConstantBuffer(const GpuApi::BufferRef&, std::vector<unsigned char>&);
// Real dynamic writes survive their recording command; bindings copy a stable
// version. Empty/unwritten buffers never supply synthetic constant values.
bool CopyCommittedGpuConstantBuffer(const GpuApi::BufferRef&, std::vector<unsigned char>&);
bool CommitGpuConstantBufferWrite(const GpuApi::BufferRef&, const void*, std::size_t);
// [flicker3] Submission-ordered constants. Native DX12 Dynamic_Legacy Lock/Unlock records a GPU copy into the one real
// buffer in command-list order, so another list reads the value of the last write SUBMITTED before it, not the last
// write recorded (Unlock) on some other CPU thread. IsOrderedGpuConstant: Dynamic_Legacy constant buffer with CPU
// snapshots (no alias backing, no native GPU backing). CopyOrderedGpuConstantBuffer returns the value as of the
// submissions committed so far; CommitOrderedGpuConstantBufferWrite is called in submission order.
bool IsOrderedGpuConstant(const GpuApi::BufferRef&);
bool CopyOrderedGpuConstantBuffer(const GpuApi::BufferRef&, std::vector<unsigned char>&);
bool CommitOrderedGpuConstantBufferWrite(const GpuApi::BufferRef&, const void*, std::size_t);
bool FlushOriginalMappedGpuBuffers();
// perf10: per-batch mapped flush. A submitter collects the mapped-upload buffer IDs its command lists reference
// (CopyMappedUploadBufferIds gives the current set), uploads only those, then marks this thread covered so the
// FlushOriginalMappedGpuBuffers() inside its SubmitGpuCommandEncoders call is a no-op. The caller clears the mark
// after that call returns (it stays set if the submit returned before flushing). Unreferenced buffers keep their
// dirty bytes against the committed shadow; the next submit that references them uploads them first.
void CopyMappedUploadBufferIds(std::vector<unsigned>& ids);
bool FlushOriginalMappedGpuBufferIds(const unsigned* ids, std::size_t count);
// perf12: same selective flush for the asynchronous submit. The dirty ranges are diffed and the committed shadow
// is updated now (in submitter order), but the bytes are returned as owned writes instead of being written through
// a synchronous main-thread handoff; the deferred submission job issues them with queue.writeBuffer before its
// queue.submit. Every listed buffer must stay referenced by the submission until it retires.
struct DeferredMappedGpuWrite { WGPUBuffer buffer; std::uint64_t offset; std::vector<unsigned char> bytes; };
bool FlushOriginalMappedGpuBufferIdsDeferred(const unsigned* ids, std::size_t count, std::vector<DeferredMappedGpuWrite>& writes);
void SetMappedGpuFlushCovered(bool covered);
// Live-range tracking for a persistently mapped buffer whose owner knows which byte ranges the GPU can read.
// The first call opts the buffer in; from then on submit-time dirty scans cover only live ranges of that
// buffer. Ranges are [offset, offset+size) in buffer bytes. Untracked buffers keep the full scan.
bool TrackMappedBufferRange(const GpuApi::BufferRef&, unsigned long long offset, unsigned long long size, bool live);
}
#include "mapped-write-track-web.h" // [skperf] round 23 mapTrack
