#include "gpu-sampler-web.h"
#include "gpu-device-web.h"
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>

namespace {
em_proxying_queue* SamplerQueue() {
    static auto* queue = em_proxying_queue_create(); return queue;
}
struct Request {
    WGPUSamplerDescriptor descriptor;
    WGPUSampler sampler = nullptr;
    em_proxying_ctx* context = nullptr;
    unsigned remaining = 3;
    bool failed = false;
};
void Checked(WGPUPopErrorScopeStatus status, WGPUErrorType type,
             WGPUStringView message, void* argument, void*) {
    auto* request = static_cast<Request*>(argument);
    if (status != WGPUPopErrorScopeStatus_Success || type != WGPUErrorType_NoError) {
        request->failed = true;
        std::fputs("CP2077 WebGPU sampler creation failed: ", stderr);
        if (message.data) {
            if (message.length == WGPU_STRLEN) std::fputs(message.data, stderr);
            else std::fwrite(message.data, 1, message.length, stderr);
        }
        std::fputc('\n', stderr);
    }
    if (--request->remaining) return;
    if (!cp2077::web::IsGpuDeviceReady()) request->failed = true;
    if (request->failed && request->sampler) { wgpuSamplerRelease(request->sampler); request->sampler = nullptr; }
    emscripten_proxy_finish(request->context);
}
}
namespace cp2077::web {
WGPUSampler CreateGpuSampler(const WGPUSamplerDescriptor& descriptor) {
    if (emscripten_is_main_runtime_thread() || !IsGpuDeviceReady()) return nullptr;
    auto* queue = SamplerQueue();
    if (!queue) return nullptr;
    Request request{descriptor};
    if (!emscripten_proxy_sync_with_ctx(queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            auto* request = static_cast<Request*>(argument);
            request->context = context;
            auto device = MainThreadGpuDevice();
            if (!device) { emscripten_proxy_finish(context); return; }
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Internal);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_OutOfMemory);
            wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);
            request->sampler = wgpuDeviceCreateSampler(device, &request->descriptor);
            if (!request->sampler) request->failed = true;
            WGPUPopErrorScopeCallbackInfo callback = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
            callback.mode = WGPUCallbackMode_AllowSpontaneous;
            callback.callback = Checked; callback.userdata1 = request;
            for (unsigned i = 0; i < 3; ++i) wgpuDevicePopErrorScope(device, callback);
        }, &request)) return nullptr;
    return request.sampler;
}
bool ReleaseGpuSampler(WGPUSampler sampler) {
    if (!sampler) return true;
    if (emscripten_is_main_runtime_thread()) { wgpuSamplerRelease(sampler); return true; }
    auto* queue = SamplerQueue();
    return queue && emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(),
        [](void* argument) { wgpuSamplerRelease(static_cast<WGPUSampler>(argument)); }, sampler);
}
}
