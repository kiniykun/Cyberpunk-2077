#pragma once
#include <webgpu/webgpu.h>
#include <cstddef>
#include <cstdint>

namespace cp2077::web {
struct ConvertedGraphicsShader;
enum class ComputeResourceKind { RawUniform, OtherUniform, SampledTexture, StorageTexture, Sampler, StorageBuffer, StorageBufferUAV, StorageBufferCounter, SamplerBias };
struct ComputeResourceBinding {
    std::uint32_t group;
    std::uint32_t binding;
    std::uint32_t slot;
    ComputeResourceKind kind;
    std::uint32_t storageStride;
};
struct ConvertedShader {
    const char* name;
    const char* runtimeName;
    const char* entryPoint;
    const char* wgslSHA256;
    const char* wgsl;
    std::size_t wgslBytes;
    const char* bytecodeIdentity;
    std::size_t bytecodeIdentityBytes;
    bool requiresTier1Storage;
    bool requiresReadWriteStorage;
    const ComputeResourceBinding* resources;
    std::size_t resourceCount;
    bool samplerBiasLowered = false;
    // Device capabilities checked before pipeline creation (ConvertedShaderCap bits); a
    // shader whose requirements the device lacks gets no pipeline and its dispatch skips.
    std::uint32_t requiredCaps = 0;
    std::uint32_t workgroupInvocations = 0;
};
enum ConvertedShaderCap : std::uint32_t {
    ConvertedShaderCap_Subgroups = 1u,
    ConvertedShaderCap_FormatsTier1 = 2u,
    ConvertedShaderCap_LargeWorkgroup = 4u, // workgroupInvocations > 256
};
const ConvertedShader* FindConvertedShader(const char* name);
const ConvertedShader* FindConvertedShaderBytecode(const void* data, std::size_t bytes);
// Worker-facing. Creates the actual module and waits for asynchronous pipeline
// creation and validation; no error pipeline or stand-in is returned.
WGPUComputePipeline CreateConvertedComputePipeline(const char* shader);
WGPUComputePipeline CreateGpuComputePipelineFromSource(const ConvertedShader*);
const ConvertedGraphicsShader* FindConvertedGraphicsLogicXorVariant(const ConvertedGraphicsShader*);
bool ReleaseGpuComputePipeline(WGPUComputePipeline pipeline);
enum class GraphicsShaderStage { Vertex, Pixel };
enum class GraphicsResourceKind { ConstantBuffer, TextureSRV, Sampler, TextureIndirection, DrawParameters, SamplerBias, StorageBuffer, StorageTextureUAV, StorageTextureAtomicBuffer, TextureAtomicDimensions,
    // [textures] Emulated t130 bindless: slot = pool chunk index (texture_2d_array), and the index table.
    MultilayerPool, MultilayerTable };
struct GraphicsResourceBinding {
    std::uint32_t group, binding, slot;
    // TextureIndirection uses slot as a constant native descriptor index.
    // Varying GPU indices require a separate bounded-table shader lowering.
    GraphicsResourceKind kind;
    std::uint32_t minimumUniformBytes;
    // Verified static float4-array reads for legacy b3/b5 constants.
    std::uint64_t legacyReadMask = 0;
    bool legacyReadMaskVerified = false;
    std::uint32_t storageStride = 0;
};
struct GraphicsVertexInput {
    const char* semantic;
    std::uint32_t semanticIndex, location, components;
};
struct ConvertedGraphicsShader {
    const char* originalHash;
    GraphicsShaderStage stage;
    const char* wgslSHA256;
    const unsigned char* nativeBytecode;
    std::size_t nativeBytecodeBytes;
    const char* wgsl;
    std::size_t wgslBytes;
    const GraphicsResourceBinding* resources;
    std::size_t resourceCount;
    const GraphicsVertexInput* vertexInputs;
    std::size_t vertexInputCount;
    bool samplerBiasLowered = false;
};
const ConvertedGraphicsShader* FindConvertedGraphicsShaderBytecode(const void*, std::size_t);
const ConvertedGraphicsShader* FindConvertedGraphicsDepthGatherVariant(const ConvertedGraphicsShader*);
const ConvertedGraphicsShader* FindConvertedGraphicsGiRGBOutputVariant(const ConvertedGraphicsShader*);
// True when shader's WGSL SHA is the pinned one or the pin's known regenerated equivalent (signedness-only
// lowering changes from the graphics package regen); exact-shader workarounds keep matching after the regen.
bool GraphicsWgslHashMatches(const ConvertedGraphicsShader* shader, const char* pinnedSHA256);
// Bit n set when the converted fragment entry point declares @location(n) output; ~0u when it cannot be parsed.
std::uint32_t GraphicsFragmentOutputMask(const ConvertedGraphicsShader* shader);
// [tankview] explicit layout made this sampler binding NonFiltering (integer-texture-only use).
bool GraphicsSamplerIsNonFiltering(const ConvertedGraphicsShader* shader, std::uint32_t binding);
// Worker-facing, validated browser module; graphics pipeline creation is separate.
WGPUShaderModule CreateConvertedGraphicsModule(const ConvertedGraphicsShader*);
bool ReleaseGpuGraphicsModule(WGPUShaderModule);
// Descriptor state is explicit. The worker keeps borrowed module/layout handles
// alive for this synchronous call; all descriptor arrays are copied internally.
WGPURenderPipeline CreateGpuRenderPipeline(const WGPURenderPipelineDescriptor&);
// [hitch] Non-blocking variant for per-draw use: a cache miss starts creation on the main runtime thread and
// returns nullptr with pending=true (skip only this draw; the pipeline is cached when ready). ?asyncPso=0 = sync.
// blockWhenBusy (precache): over the in-flight cap, wait for creation instead of returning pending.
WGPURenderPipeline CreateGpuRenderPipelineOrPending(const WGPURenderPipelineDescriptor&, bool& pending, bool blockWhenBusy = false);
// [hitch] Draws skipped because their pipeline was still being created (monotonic).
std::uint64_t GpuRenderPipelinePendingSkips();
bool ReleaseGpuRenderPipeline(WGPURenderPipeline);
// Explicit group-0 pipeline layout for a converted (vertex, pixel) pair whose uniform buffers use dynamic
// offsets, so per-draw uniform arena offsets stay out of bind-group cache keys. Entries are derived from the
// reflected resources plus the WGSL declarations, mirroring the default (auto) layout rules.
constexpr std::size_t kMaxGraphicsLayoutDynamicOffsets = 16;
struct GpuGraphicsPipelineLayout {
    WGPUPipelineLayout layout = nullptr; // borrowed; owned by the layout cache for the device generation
    std::uint32_t dynamicBindings[kMaxGraphicsLayoutDynamicOffsets]{}; // ascending binding numbers
    std::uint32_t dynamicCount = 0;
    // [nomadperf] When uniforms beyond the device's dynamic-uniform limit were turned into dynamic read-only storage
    // buffers, these are the converted-WGSL modules the pipeline must use (borrowed; owned by the layout cache).
    WGPUShaderModule vertexModule = nullptr, fragmentModule = nullptr;
    // [giexplicit] the GI-RGB-output variant of fragmentModule (same storage conversion), when the pixel shader has one.
    WGPUShaderModule giFragmentModule = nullptr;
};
// Worker-facing. False selects the auto layout (variant, unsupported reflection, no uniforms, rejected pair).
// pixel is nullptr for depth-only draws. The first call per pair creates the layout on the main thread.
// [varexplicit] Storage-converted module of a fragment variant (same bindings as pixel, different outputs) for the
// explicit layout of (vertex, pixel); nullptr when that layout converted no fragment uniforms or creation failed.
// Cached for the device generation (borrowed). Worker threads only.
WGPUShaderModule AcquireGpuFragmentStorageVariant(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                                                  const ConvertedGraphicsShader* variant);
bool AcquireGpuGraphicsPipelineLayout(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel,
                                      GpuGraphicsPipelineLayout& out);
// Called when a pipeline failed with the explicit layout but succeeded with auto layout: the pair uses auto.
void RejectGpuGraphicsPipelineLayout(const ConvertedGraphicsShader* vertex, const ConvertedGraphicsShader* pixel);
}
