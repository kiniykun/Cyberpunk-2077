#pragma once
#include "common/redMemory/include/systemAllocator.h"
#include "common/redMemory/src/systemPageAllocator.h"

namespace red { namespace memory {
class SystemPageAllocatorWeb final : public SystemPageAllocator {
    void OnInitialize() override;
    VirtualRange OnReserveRange(u64,u32,u32) override;
    VirtualRange OnReserveAlignedRange(u64,u32,u32,u32) override;
    void OnReleaseRange(const VirtualRange&) override;
};
class SystemAllocatorWeb final : public SystemAllocator {
    void OnInitialize() override;
    u64 OnReleaseVirtualRange(const VirtualRange&) override;
    SystemBlock OnCommit(const SystemBlock&,u32) override;
    SystemBlock OnCommitAligned(const SystemBlock&,u32,u32) override;
    void OnDecommit(const SystemBlock&) override;
    void OnPartialDecommit(const SystemBlock&,const SystemBlock&) override;
    u64 OnGetTotalPhysicalMemoryAvailable() const override;
    u64 OnGetCurrentPageMemoryAvailable() const override;
    u64 OnGetPageSize() const override;
    void OnWriteReportToLog() const override;
    void OnWriteReportToJson(FILE*) const override;
};
}}
