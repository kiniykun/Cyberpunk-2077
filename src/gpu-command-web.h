#include <string>
#include <memory>
#include <vector>
#include <functional>
#pragma once
#include <webgpu/webgpu.h>
#include <cstddef>
#include <cstdint>
namespace cp2077::web {
WGPUCommandEncoder CreateGpuCommandEncoder();
// [opt] Main thread only (no-op elsewhere): tops the worker encoder pool up and services waiting worker refills.
void ServiceGpuEncoderPool();
// Finishes, submits in array order, and releases all encoders. Caller transfers ownership.
using GpuSubmissionComplete = void (*)(bool succeeded, void* userdata);
// Success transfers userdata to the asynchronous callback; pre-submit failure
// leaves it with the caller. Completion does not prove rendered pixels.
bool SubmitGpuCommandEncoders(WGPUCommandEncoder* encoders, std::size_t count,
    GpuSubmissionComplete complete = nullptr, void* userdata = nullptr);
// perf12: main thread only, no validation drain and no mapped flush (the caller did both). isolated[i] != 0 submits
// member i on its own. See gpu-command-web.cpp.
bool SubmitGpuCommandEncodersOnMain(WGPUCommandEncoder* encoders, const unsigned char* isolated, std::size_t count,
    GpuSubmissionComplete complete, void* userdata);
bool WaitGpuQueueIdle();
bool ReleaseGpuCommandEncoder(WGPUCommandEncoder encoder);
enum class GpuDebugCommand { PushGroup, PopGroup, Marker };
bool EncodeGpuDebugCommand(WGPUCommandEncoder, GpuDebugCommand, const char* label);
bool EncodeGpuBufferZero(WGPUCommandEncoder, WGPUBuffer, std::uint64_t bytes);
bool EncodeGpuBufferCopy(WGPUCommandEncoder encoder, WGPUBuffer source,
                         std::uint64_t sourceOffset, WGPUBuffer destination,
                         std::uint64_t destinationOffset, std::uint64_t size);
struct GpuBufferTransfer {
    WGPUBuffer source, destination;
    std::uint64_t sourceOffset, destinationOffset, bytes;
};
bool EncodeGpuBufferTransfers(WGPUCommandEncoder, const GpuBufferTransfer*, std::size_t count);
struct GpuQueuedDraw;
struct GpuOrderedCommand {
    bool isDebug=false;
    // When set, a graphics draw encoded in order with the surrounding copies (see QueueGpuGraphicsDraw).
    std::shared_ptr<GpuQueuedDraw> draw;
    // Any other encoder work (clears, timestamps, query resolves) run in order on the main thread inside the batch.
    // Returns false to fail the batch. Runs exactly once when the batch is encoded.
    std::function<bool(WGPUCommandEncoder)> custom;
    GpuBufferTransfer copy{};
    GpuDebugCommand debug=GpuDebugCommand::Marker;
    std::string label;
};
// Ordered copies and owned debug labels in one synchronous browser handoff.
bool EncodeGpuOrderedCommands(WGPUCommandEncoder,const GpuOrderedCommand*,std::size_t count);

// Same ordered GPU copies, issued together in one browser handoff.
struct GpuBufferCopyRegion {
    std::uint64_t sourceOffset, destinationOffset, bytes;
};
bool EncodeGpuBufferCopyRegions(WGPUCommandEncoder, WGPUBuffer source, WGPUBuffer destination,
    const GpuBufferCopyRegion* regions, std::size_t count);
// Encodes a full selected-subresource clear using an actual render pass.
bool EncodeGpuColorClear(WGPUCommandEncoder, WGPUTextureView, const WGPUColor&);
bool EncodeGpuColorRectClear(WGPUCommandEncoder, WGPUTextureView, WGPUTextureFormat,
    std::uint32_t width, std::uint32_t height, std::uint32_t samples,
    std::uint32_t x, std::uint32_t y, std::uint32_t rectWidth, std::uint32_t rectHeight, const WGPUColor&);
bool EncodeGpuDepthClear(WGPUCommandEncoder, WGPUTextureView, float depth,
    bool hasStencil = false, bool clearStencil = false, std::uint8_t stencil = 0);
// Preserves depth/stencil outside the native clear rectangle.
bool EncodeGpuDepthRectClear(WGPUCommandEncoder, WGPUTextureView, WGPUTextureFormat,
    std::uint32_t width, std::uint32_t height, std::uint32_t samples,
    std::uint32_t x, std::uint32_t y, std::uint32_t rectWidth, std::uint32_t rectHeight,
    float depth, bool hasStencil, bool clearStencil, std::uint8_t stencil);
bool EncodeGpuColorResolve(WGPUCommandEncoder, WGPUTextureView source, WGPUTextureView destination);
// Copy actual single-sample depth values into an R32Float compatibility view.
// Caller retains both views/textures through submission. No sampling, resizing,
// or substitute data: dimensions must match the selected source subresource.
bool EncodeGpuDepthFloatCopy(WGPUCommandEncoder, WGPUTextureView source,
    WGPUTextureView destination, std::uint32_t width, std::uint32_t height);
// [npcblack] Copy the 16-bit codes of one R16Unorm layer (the backing of a D16U UAV texture) into one Depth16Unorm
// layer, so the original depth/comparison SRV declaration can read the same values. Both views are single 2D layers.
bool EncodeGpuR16ToDepth16Copy(WGPUCommandEncoder, WGPUTextureView sourceR16,
    WGPUTextureView destination, std::uint32_t width, std::uint32_t height,
    WGPUTextureFormat target = WGPUTextureFormat_Depth16Unorm); // or R32Float/R16Float colour
struct GpuTextureCopy {
    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    WGPUExtent3D extent{};
};
bool EncodeGpuDepthUnorm16Copy(WGPUCommandEncoder,WGPUTextureView source,WGPUTextureView destination,
    WGPUBuffer region,std::uint32_t width,std::uint32_t height);
// Same copy with the 32-byte region read at regionOffset (a 256-aligned uniform ring/arena slot).
bool EncodeGpuDepthUnorm16CopyAt(WGPUCommandEncoder,WGPUTextureView source,WGPUTextureView destination,
    WGPUBuffer region,std::uint64_t regionOffset,std::uint32_t width,std::uint32_t height);
bool EncodeGpuTextureCopies(WGPUCommandEncoder, const GpuTextureCopy*, std::size_t count);
// The argument checks EncodeGpuTextureCopies applies (any thread), for copies queued and encoded later.
bool ValidateGpuTextureCopies(const GpuTextureCopy*, std::size_t count);
// Main thread only (queued compute dispatch, perf7): one compute pass with this pipeline and group 0, then a
// direct (indirect == nullptr) or indirect dispatch. False when a limit is exceeded or the pass cannot begin.
bool EncodeGpuComputeDispatchOnMain(WGPUCommandEncoder, WGPUComputePipeline, WGPUBindGroup group,
    std::uint32_t x, std::uint32_t y, std::uint32_t z, WGPUBuffer indirect, std::uint64_t indirectOffset);
// Releases one reference on every handle of entries on the main thread (asynchronously from a worker).
void ReleaseGpuBindEntriesOnMain(std::vector<WGPUBindGroupEntry>&& entries);
bool EncodeGpuTextureBufferCopy(WGPUCommandEncoder, const WGPUTexelCopyTextureInfo&,
    const WGPUTexelCopyBufferInfo&, const WGPUExtent3D&, bool upload);
WGPUQuerySet CreateGpuTimestampQuerySet(std::uint32_t count);
bool ReleaseGpuQuerySet(WGPUQuerySet);
bool EncodeGpuTimestamp(WGPUCommandEncoder, WGPUQuerySet, std::uint32_t index);
bool EncodeGpuQueryResolve(WGPUCommandEncoder, WGPUQuerySet, std::uint32_t start,
    std::uint32_t count, WGPUBuffer resolve, WGPUBuffer readback);
struct GraphicsDrawBuffer {
    WGPUBuffer buffer = nullptr;
    std::uint64_t offset = 0, bytes = 0;
    // Explicit hole in the original shader's sparse vertex-slot layout.
    bool unused = false;
};
constexpr std::size_t kMaxGraphicsDrawDynamicOffsets = 16;
struct GraphicsDrawGroup {
    std::uint32_t index = 0;
    WGPUBindGroup group = nullptr;
    // Dynamic uniform offsets in ascending binding order (explicit pipeline layouts only). Stored inline so
    // queued copies of the draw need no deep copy.
    std::uint32_t dynamicOffsetCount = 0;
    std::uint32_t dynamicOffsets[kMaxGraphicsDrawDynamicOffsets]{};
};
// Uniform arena slot bytes written on the main thread immediately before this draw is encoded.
struct GraphicsUniformWrite { WGPUBuffer buffer = nullptr; std::uint64_t offset = 0; const void* bytes = nullptr; std::size_t size = 0; };
struct GraphicsDraw {
    WGPURenderPipeline pipeline = nullptr;
    const WGPUTextureView* colors = nullptr;
    std::size_t colorCount = 0;
    WGPUTextureView depth = nullptr;
    bool depthReadOnly = false;
    bool hasStencil = false;
    std::uint32_t targetWidth = 0, targetHeight = 0;
    const GraphicsDrawBuffer* vertices = nullptr;
    std::size_t vertexCount = 0;
    GraphicsDrawBuffer indices;
    WGPUIndexFormat indexFormat = WGPUIndexFormat_Undefined;
    const GraphicsDrawGroup* groups = nullptr;
    std::size_t groupCount = 0;
    float viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
    float minDepth = 0, maxDepth = 1;
    std::uint32_t scissorX = 0, scissorY = 0, scissorWidth = 0, scissorHeight = 0;
    WGPUColor blendFactor{1, 1, 1, 1};
    std::uint32_t stencilReference = 0;
    bool indexed = false;
    std::uint32_t count = 0, instances = 0, first = 0, firstInstance = 0;
    std::int32_t baseVertex = 0;
    GraphicsDrawBuffer indirectArguments;
    std::uint32_t indirectCount = 0;
    const GraphicsUniformWrite* uniformWrites = nullptr;
    std::size_t uniformWriteCount = 0;
    // True when the draw binds no writable storage, so it may share a render pass with neighbouring draws that
    // target the same attachments without creating a read/write usage conflict inside one pass.
    bool mergeablePass = false;
};
// Synchronously issues an actual pass/draw on the browser main thread.
// All handles are borrowed: caller retains resources through submission.
// Success means commands were issued, not validated or rendered on the GPU.
bool EncodeGpuGraphicsDraw(WGPUCommandEncoder, const GraphicsDraw&);
// A validated draw with owned copies of every array it points to, so it can be encoded later on the main
// thread together with other queued commands (one browser handoff for many draws instead of one per draw).
struct GpuQueuedDraw {
    GraphicsDraw draw;
    std::vector<WGPUTextureView> colors;
    std::vector<GraphicsDrawBuffer> vertices;
    std::vector<GraphicsDrawGroup> groups;
    std::vector<GraphicsUniformWrite> writes;
    std::vector<unsigned char> writeBytes;
    // Group-0 bind group created on the main thread at encode time when bindPipeline is set (cache miss on the
    // worker); createdGroup then holds the reference the command retains until GPU completion.
    WGPURenderPipeline bindPipeline = nullptr;
    std::vector<WGPUBindGroupEntry> bindEntries;
    WGPUCommandEncoder bindOwner = nullptr;
    WGPUBindGroup createdGroup = nullptr;
    // References taken on every bindEntries handle when queued, so views/samplers rebound before the flush stay
    // alive until the main thread has created the group; released right after creation.
    bool bindRefsHeld = false;
    // References on the color/depth target views taken when queued: the worker's view cache may evict or a
    // texture invalidation may release a view before the batch is encoded on the main thread. Released when
    // the queued draw is destroyed (after encoding; the render pass keeps its own references).
    bool targetRefsHeld = false;
    // [skperf] round 13 copyHoist: buffer ranges this draw's bind group reads (real offsets, captured before the
    // dynamic-offset conversion). readOverflow = more than kMaxReadRanges (the pass then never hoists copies).
    static constexpr unsigned kMaxReadRanges = 24;
    struct ReadRange { WGPUBuffer buffer; std::uint64_t begin, end; };
    ReadRange readRanges[kMaxReadRanges];
    unsigned readRangeCount = 0;
    bool readOverflow = true; // unknown until the recorder fills it
    void HoldTargetViews() {
        if (targetRefsHeld) return;
        for (auto view : colors) if (view) wgpuTextureViewAddRef(view);
        if (draw.depth) wgpuTextureViewAddRef(draw.depth);
        targetRefsHeld = true;
    }
    ~GpuQueuedDraw() {
        if (!targetRefsHeld) return;
        for (auto view : colors) if (view) wgpuTextureViewRelease(view);
        if (draw.depth) wgpuTextureViewRelease(draw.depth);
    }
    // [skperf] round 14 drawPool: return to the freshly constructed state (releasing exactly what the destructor
    // releases), keeping the vectors' capacity so a recycled object needs no heap allocation for typical draws.
    void Recycle() {
        if (targetRefsHeld) {
            for (auto view : colors) if (view) wgpuTextureViewRelease(view);
            if (draw.depth) wgpuTextureViewRelease(draw.depth);
        }
        draw = GraphicsDraw{};
        colors.clear(); vertices.clear(); groups.clear(); writes.clear(); writeBytes.clear(); bindEntries.clear();
        bindPipeline = nullptr; bindOwner = nullptr; createdGroup = nullptr; bindRefsHeld = false; targetRefsHeld = false;
        readRangeCount = 0; readOverflow = true;
    }
};
// Validates exactly like EncodeGpuGraphicsDraw. Returns false if invalid; on success sets queued to nullptr
// when the draw produces no fragments (nothing to encode), otherwise to an owned copy.
// [skperf] round 15 passHold: holdTargets=false when the command list itself keeps every attachment view alive until
// retirement (SetupRenderTargets parks swapped-out attachments), so the per-draw AddRef/Release is not needed.
bool PrepareQueuedGpuGraphicsDraw(const GraphicsDraw&, std::shared_ptr<GpuQueuedDraw>& queued, bool holdTargets = true);
}
