// Original frame counters and profiler scopes mapped to browser command state.
void BeginRender() {
    bool valid = false;
    Perf10Switches(); // perf10: read the URL switches once, outside any lock
    ReportCmdlockCounters(); // [cmdlock]
    {
        std::lock_guard<std::mutex> lock(renderFrameMutex);
        if (!insideRenderFrame && cp2077::web::IsGpuDeviceReady()) {
            insideRenderFrame = true;
            ++renderFrameIndex; valid = true;
            publishedRenderFrame.store(renderFrameIndex, std::memory_order_relaxed);
        }
    }
    if (!valid) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: nested frame or unavailable device\n", stderr);
    }
    // WebGPU owns queued GPU resource lifetimes. Native desktop retirement
    // rings and vendor hooks do not exist in this browser allocator.
}
void EndRender() {
    bool valid = false;
    {
        std::lock_guard<std::mutex> lock(renderFrameMutex);
        if (insideRenderFrame) {
            insideRenderFrame = false; valid = true;
        }
    }
    if (!valid) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: unmatched frame end\n", stderr);
    }
}
Int32 FrameIndex() {
    std::lock_guard<std::mutex> lock(renderFrameMutex);
    Int32 result; std::memcpy(&result, &renderFrameIndex, sizeof(result));
    return result;
}
namespace {
void RecordNativeGpuDebugCommand(cp2077::web::GpuDebugCommand operation, const AnsiChar* label) {
    CpCommandLock lock(commandMutex,__builtin_FUNCTION(),__FILE__,__LINE__);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) {
        std::fputs("CP2077 WebGPU: profiler marker needs an owned command list\n", stderr); return;
    }
    auto& command = entry->second;
    if (command.failed) return;
    if ((operation == cp2077::web::GpuDebugCommand::PopGroup && !command.debugGroupDepth) ||
        (operation == cp2077::web::GpuDebugCommand::PushGroup && command.debugGroupDepth == 0xffffffffu) ||
        !QueuePendingDebugCommand(command, operation, label)) {
        MarkBrowserCommandFailed(command, __func__, __LINE__);
        std::fputs("CP2077 WebGPU: invalid or failed GPU profiler scope\n", stderr); return;
    }
    if (operation == cp2077::web::GpuDebugCommand::PushGroup) ++command.debugGroupDepth;
    if (operation == cp2077::web::GpuDebugCommand::PopGroup) --command.debugGroupDepth;
}
}
void BeginProfilerBlock(const AnsiChar* label) { RecordNativeGpuDebugCommand(cp2077::web::GpuDebugCommand::PushGroup, label); }
void EndProfilerBlock() { RecordNativeGpuDebugCommand(cp2077::web::GpuDebugCommand::PopGroup, nullptr); }
void SetMarker(const AnsiChar* label) { RecordNativeGpuDebugCommand(cp2077::web::GpuDebugCommand::Marker, label); }
