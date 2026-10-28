#pragma once
#include <webgpu/webgpu.h>
#include <cstddef>
#include <cstdint>
#include <vector>
namespace cp2077::web {
// GPU owner thread only. Resource destruction invalidates cached validation.
void ClearGpuBindGroupCache();
void InvalidateGpuBindGroupsForBuffer(WGPUBuffer);
void InvalidateGpuBindGroupsForTexture(WGPUTexture);
struct ComputeBindGroup { std::uint32_t group; WGPUBindGroup binding; };
// Caller retains the pipeline and entry resources until creation completes.
WGPUBindGroup CreateGpuComputeBindGroup(WGPUComputePipeline, std::uint32_t group,
                                        const WGPUBindGroupEntry*, std::size_t count,
                                        WGPUCommandEncoder validationOwner = nullptr);
// Worker-side exact bind-group cache lookup; never creates. Caller releases the returned group.
WGPUBindGroup FindCachedGpuGraphicsBindGroup(WGPURenderPipeline, const WGPUBindGroupEntry*, std::size_t count);
// Main thread only (batched draw encoding): creates or reuses a group-0 bind group with deferred validation
// attached to owner; returns nullptr when it cannot. Caller releases the returned group.
WGPUBindGroup CreateGpuGraphicsBindGroupOnMain(WGPURenderPipeline, const WGPUBindGroupEntry*, std::size_t count, WGPUCommandEncoder owner);
// Compute twins of the two functions above (group 0, layout:auto). The main-thread form never declines for a full
// validation backlog: it then creates the group without error scopes.
WGPUBindGroup FindCachedGpuComputeBindGroup(WGPUComputePipeline, const WGPUBindGroupEntry*, std::size_t count);
WGPUBindGroup CreateGpuComputeBindGroupOnMain(WGPUComputePipeline, const WGPUBindGroupEntry*, std::size_t count, WGPUCommandEncoder owner);
WGPUBindGroup CreateGpuGraphicsBindGroup(WGPURenderPipeline, std::uint32_t group,
                                        const WGPUBindGroupEntry*, std::size_t count,
                                        WGPUCommandEncoder validationOwner = nullptr);
// Drain every captured validation scope before submitting this encoder.
// Cancelled encoders abandon the waiter but retain callback-owned resources.
bool DrainGpuCommandValidation(WGPUCommandEncoder);
void AbandonGpuCommandValidation(WGPUCommandEncoder);
// perf12: main thread only, never waits. Forgets the encoder's validation record and reports its state:
// 0 = no record or every scope settled clean, 1 = a scope already failed, 2 = scopes still pending.
int PollGpuCommandValidation(WGPUCommandEncoder);
// perf12: deferred main-thread GPU work (asynchronous command-list submission). Workers append jobs; the main
// thread runs them strictly in append order. Every main-thread handler proxied from a worker that touches the
// queue timeline (writeBuffer, writeTexture, submit, mapAsync, onSubmittedWorkDone, present, destroy) calls
// DrainDeferredGpuWork first, so queue operations keep the program order of the threads that issued them.
// DrainDeferredGpuWork is a no-op off the main thread, when nothing is pending, and while a job is running.
bool EnqueueDeferredGpuWork(void (*work)(void*), void* argument);
void DrainDeferredGpuWork();
bool ReleaseGpuBindGroup(WGPUBindGroup);
// Releases every group (cached leases on the worker, the rest in one asynchronous main-thread handoff); clears groups.
void ReleaseGpuBindGroups(std::vector<WGPUBindGroup>&& groups);
// Encodes and ends a real pass; does not submit or wait for GPU completion.
bool EncodeGpuComputeDispatch(WGPUCommandEncoder, WGPUComputePipeline,
                              const ComputeBindGroup*, std::size_t count,
                              std::uint32_t x, std::uint32_t y, std::uint32_t z);
bool EncodeGpuComputeDispatchIndirect(WGPUCommandEncoder, WGPUComputePipeline,
    const ComputeBindGroup*, std::size_t count, WGPUBuffer arguments, std::uint64_t offset);
}
