namespace {
struct BrowserBufferGrab {
    WGPUBuffer staging;
    GpuApi::GpuFence fence;
    void* output;
    Uint32 bytes;
    Uint64 allocatedBytes;
};
std::mutex grabMutex;
std::vector<BrowserBufferGrab> bufferGrabs;
void ShutdownBrowserBufferGrabs() {
    std::vector<BrowserBufferGrab> retired;
    { std::lock_guard<std::mutex> lock(grabMutex); retired.swap(bufferGrabs); }
    for (auto& grab : retired) cp2077::web::DestroyGpuBuffer(grab.staging);
}
}
namespace GpuApi {
Bool GrabBuffer(const BufferRef& reference, Uint32 size, void* output) {
    if (!size || !output) return false;
    BufferDesc descriptor; WGPUBuffer native;
    if (!cp2077::web::AcquireGpuBuffer(reference, descriptor, native)) return false;
    // Constant buffers currently use per-draw translated snapshots; reading
    // their untouched backing would produce invented output.
    if (size > descriptor.size || descriptor.category == GpuWrapApi::BCC_Constant) { Release(reference); return false; }
    const auto fence = GetGpuFence();
    if (!fence) { Release(reference); return false; }
    const Uint64 allocated = (Uint64(size) + 3) & ~Uint64(3);
    const auto staging = cp2077::web::CreateGpuBuffer(allocated, WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst, nullptr, 0, "cp-grab-readback");
    if (!staging || !cp2077::web::RecordGpuBufferCopy(native, 0, staging, 0, allocated, reference, BufferRef::Null())) {
        if (staging) cp2077::web::DestroyGpuBuffer(staging);
        Release(reference); return false;
    }
    std::lock_guard<std::mutex> lock(grabMutex);
    bufferGrabs.push_back({staging, fence, output, size, allocated});
    return true;
}
Uint32 FlushBufferGrabs() {
    // Native contract requires the caller to submit commands before flushing.
    Flush(); // Wait for real queue completion and native-reference retirement.
    std::vector<BrowserBufferGrab> ready;
    {
        std::lock_guard<std::mutex> lock(grabMutex);
        for (auto it = bufferGrabs.begin(); it != bufferGrabs.end();) {
            if (IsGpuFenceComplete(it->fence)) { ready.push_back(*it); it = bufferGrabs.erase(it); }
            else ++it;
        }
        if (!bufferGrabs.empty()) std::fputs("CP2077 WebGPU: buffer grabs have unsubmitted or failed fences\n", stderr);
    }
    Uint32 completed = 0;
    for (auto& grab : ready) {
        if (cp2077::web::ReadGpuBuffer(grab.staging, grab.allocatedBytes, grab.output, grab.bytes)) ++completed;
        else std::fputs("CP2077 WebGPU: GPU buffer readback failed\n", stderr);
        cp2077::web::DestroyGpuBuffer(grab.staging);
    }
    return completed;
}
}
