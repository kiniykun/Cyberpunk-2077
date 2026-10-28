#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "common/renderData/include/pipelineStateObject.h"
#include "gpu-api-om-state-web.h"
#include "gpu-device-web.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdio>
#include <atomic>

namespace {
struct OMEntry { SOMState state; Int32 references = 1; };
std::mutex omMutex;
std::unordered_map<Uint32, std::unique_ptr<OMEntry>> states;
Uint32 nextOMState = 1;
}
namespace GpuApi {
OMStateRef CreateOMState(const SOMState& state) {
    if (!IsInit() || state.m_blendModeDesc.m_numTargets > PSODesc::MAX_RENDERTARGETS)
        return OMStateRef::Null();
    std::lock_guard<std::mutex> lock(omMutex);
    if (nextOMState > Uint32(INT32_MAX)) return OMStateRef::Null();
    const auto id = nextOMState++;
    states.emplace(id, std::make_unique<OMEntry>(OMEntry{state, 1}));
    return OMStateRef{id};
}
void AddRef(const OMStateRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(omMutex);
    auto entry = states.find(reference.ID());
    if (entry != states.end() && entry->second->references < INT32_MAX) ++entry->second->references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing output-merger state ref\n", stderr);
}
Int32 Release(const OMStateRef& reference) {
    if (reference.isNull()) return 0;
    std::lock_guard<std::mutex> lock(omMutex);
    auto entry = states.find(reference.ID());
    if (entry == states.end()) return 0;
    const auto count = --entry->second->references;
    if (!count) states.erase(entry);
    return count;
}
}
namespace cp2077::web {
bool AcquireGpuOMState(const GpuApi::OMStateRef& reference, SOMState& state) {
    std::lock_guard<std::mutex> lock(omMutex);
    auto entry = states.find(reference.ID());
    if (entry == states.end() || entry->second->references == INT32_MAX) return false;
    state = entry->second->state;
    ++entry->second->references;
    return true;
}
void ShutdownGpuOMStates() {
    std::lock_guard<std::mutex> lock(omMutex);
    states.clear();
}
}

namespace {
WGPUBlendOperation BlendOperation(PSODesc::BlendMode::Op value) {
    using namespace PSODesc::BlendMode;
    switch (value) {
    case OP_Add: return WGPUBlendOperation_Add;
    case OP_Subtract: return WGPUBlendOperation_Subtract;
    case OP_RevSub: return WGPUBlendOperation_ReverseSubtract;
    case OP_Min: return WGPUBlendOperation_Min;
    case OP_Max: return WGPUBlendOperation_Max;
    default: return static_cast<WGPUBlendOperation>(0);
    }
}
WGPUBlendFactor BlendFactor(PSODesc::BlendMode::Factor value) {
    using namespace PSODesc::BlendMode;
    switch (value) {
    case FAC_Zero: return WGPUBlendFactor_Zero;
    case FAC_One: return WGPUBlendFactor_One;
    case FAC_SrcColor: return WGPUBlendFactor_Src;
    case FAC_InvSrcColor: return WGPUBlendFactor_OneMinusSrc;
    case FAC_SrcAlpha: return WGPUBlendFactor_SrcAlpha;
    case FAC_InvSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case FAC_DestColor: return WGPUBlendFactor_Dst;
    case FAC_InvDestColor: return WGPUBlendFactor_OneMinusDst;
    case FAC_DestAlpha: return WGPUBlendFactor_DstAlpha;
    case FAC_InvDestAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
    case FAC_BlendFactor: return WGPUBlendFactor_Constant;
    case FAC_InvBlendFactor: return WGPUBlendFactor_OneMinusConstant;
    // [whitewall] Dual-source factors; BuildGpuOMPipelineState only keeps them on a dual-source device with one target.
    case FAC_Src1Color: return WGPUBlendFactor_Src1;
    case FAC_InvSrc1Color: return WGPUBlendFactor_OneMinusSrc1;
    case FAC_Src1Alpha: return WGPUBlendFactor_Src1Alpha;
    case FAC_InvSrc1Alpha: return WGPUBlendFactor_OneMinusSrc1Alpha;
    default: return static_cast<WGPUBlendFactor>(0);
    }
}
bool FillBlend(WGPUBlendComponent& result, PSODesc::BlendMode::Op operation,
               PSODesc::BlendMode::Factor source, PSODesc::BlendMode::Factor destination) {
    result.operation = BlendOperation(operation);
    if (!result.operation) return false;
    if (operation == PSODesc::BlendMode::OP_Min || operation == PSODesc::BlendMode::OP_Max) {
        // Native MIN/MAX ignore factors; WebGPU requires these to be One.
        result.srcFactor = result.dstFactor = WGPUBlendFactor_One;
    } else {
        result.srcFactor = BlendFactor(source); result.dstFactor = BlendFactor(destination);
    }
    return result.srcFactor && result.dstFactor;
}
WGPUCompareFunction Comparison(PSODesc::DepthStencilMode::ComparisonMode value) {
    using namespace PSODesc::DepthStencilMode;
    switch (value) {
    case COMPARISON_Never: return WGPUCompareFunction_Never;
    case COMPARISON_Less: return WGPUCompareFunction_Less;
    case COMPARISON_Equal: return WGPUCompareFunction_Equal;
    case COMPARISON_LessEqual: return WGPUCompareFunction_LessEqual;
    case COMPARISON_Greater: return WGPUCompareFunction_Greater;
    case COMPARISON_NotEqual: return WGPUCompareFunction_NotEqual;
    case COMPARISON_GreaterEqual: return WGPUCompareFunction_GreaterEqual;
    case COMPARISON_Always: return WGPUCompareFunction_Always;
    default: return WGPUCompareFunction_Undefined;
    }
}
WGPUStencilOperation StencilOperation(PSODesc::DepthStencilMode::StencilOpMode value) {
    using namespace PSODesc::DepthStencilMode;
    switch (value) {
    case STENCILOP_Keep: return WGPUStencilOperation_Keep;
    case STENCILOP_Zero: return WGPUStencilOperation_Zero;
    case STENCILOP_Replace: return WGPUStencilOperation_Replace;
    case STENCILOP_IncreaseSaturate: return WGPUStencilOperation_IncrementClamp;
    case STENCILOP_DecreaseSaturate: return WGPUStencilOperation_DecrementClamp;
    case STENCILOP_Invert: return WGPUStencilOperation_Invert;
    case STENCILOP_Increase: return WGPUStencilOperation_IncrementWrap;
    case STENCILOP_Decrease: return WGPUStencilOperation_DecrementWrap;
    default: return static_cast<WGPUStencilOperation>(0);
    }
}
}
namespace cp2077::web {
bool SupportsGpuDualSourceBlending(); // [whitewall] defined in gpu-device-web.cpp (declared here to keep gpu-device-web.h unchanged)
bool BuildGpuOMPipelineState(const SOMState& state, bool depthClip, unsigned targets,
                            WGPUTextureFormat depthFormat, unsigned samples, GpuOMPipelineState& output) {
    output = GpuOMPipelineState{};
    GpuOMPipelineState result;
    const auto& raster = state.m_rasterizerModeDesc;
    const auto& blend = state.m_blendModeDesc;
    const auto& depth = state.m_depthStencilModeDesc;
    auto reject=[&](int line){
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1)<16)std::fprintf(stderr,"CP2077 OM state rejection probe: line=%d depthClip=%u wireframe=%u conservative=%u targets=%u depthFormat=%u samples=%u stencil=%u stencilPass=%u stencilWriteMask=%u\n",line,unsigned(depthClip),unsigned(raster.m_wireframe),unsigned(raster.m_conservativeRasterization),targets,unsigned(depthFormat),samples,unsigned(depth.m_stencilEnable),unsigned(depth.m_frontFace.m_stencilPassOp),unsigned(state.m_stencilWriteMask));
        return false;
    };
    if (targets > 8 || blend.m_numTargets > 8 || (samples != 1 && samples != 4) ||
        raster.m_wireframe || raster.m_conservativeRasterization ||
        (samples > 1 && !raster.m_allowMSAA)) return reject(__LINE__);
    if(!depthClip){if(!SupportsGpuDepthClipControl())return reject(__LINE__);result.primitive.unclippedDepth=true;}
    switch (raster.m_frontWinding) {
    case PSODesc::RasterizerMode::FRONTFACE_CCW: result.primitive.frontFace = WGPUFrontFace_CCW; break;
    case PSODesc::RasterizerMode::FRONTFACE_CW: result.primitive.frontFace = WGPUFrontFace_CW; break;
    default: return reject(__LINE__);
    }
    switch (raster.m_cullMode) {
    case PSODesc::RasterizerMode::CULL_None: result.primitive.cullMode = WGPUCullMode_None; break;
    case PSODesc::RasterizerMode::CULL_Front: result.primitive.cullMode = WGPUCullMode_Front; break;
    case PSODesc::RasterizerMode::CULL_Back: result.primitive.cullMode = WGPUCullMode_Back; break;
    default: return reject(__LINE__);
    }
    // [gamerender] D3D12 accepts alpha-to-coverage on single-sample targets (one coverage bit: at most a
    // cutout); WebGPU requires multisampling for it. Drop it at 1 sample instead of rejecting the draw.
    result.alphaToCoverage = blend.m_alphaToCoverage && samples > 1;
    for (unsigned i=0; i<targets; ++i) {
        if (blend.m_independent && i >= blend.m_numTargets) continue; // no writes
        const auto& native = blend.m_renderTarget[blend.m_independent ? i : 0];
        if (unsigned(native.m_writeMask) & ~15u) return reject(__LINE__);
        result.writeMasks[i] = static_cast<WGPUColorWriteMask>(native.m_writeMask);
        if(native.m_colorOp==PSODesc::BlendMode::OP_Xor){
            if(targets!=1 || samples!=1 || result.alphaToCoverage)return reject(__LINE__);
            result.logicXor=true;continue;
        }
        if (!native.m_blendEnable) {
            if (native.m_colorOp >= PSODesc::BlendMode::OP_Or) return reject(__LINE__);
            continue;
        }
        result.blendEnabled[i] = true;
        if (!FillBlend(result.blends[i].color, native.m_colorOp, native.m_srcFactor, native.m_destFactor) ||
            !FillBlend(result.blends[i].alpha, native.m_alphaOp, native.m_srcAlphaFactor, native.m_destAlphaFactor)) return reject(__LINE__);
        // [whitewall] WebGPU allows Src1 factors only with dual-source-blending and a single color target.
        const auto src1 = [](WGPUBlendFactor f) { return f == WGPUBlendFactor_Src1 || f == WGPUBlendFactor_OneMinusSrc1 ||
            f == WGPUBlendFactor_Src1Alpha || f == WGPUBlendFactor_OneMinusSrc1Alpha; };
        const auto& b = result.blends[i];
        if (src1(b.color.srcFactor) || src1(b.color.dstFactor) || src1(b.alpha.srcFactor) || src1(b.alpha.dstFactor)) {
            // [whitewall] r4: a non-independent blend desc repeats RT0's Src1 factors on every target; the split
            // writes only target 0 (others are write-masked), so Src1 on targets > 0 is not a reason to reject.
            if (targets != 1) result.dualSourceMultiTarget = true; // [whitewall] split into two draws, no feature needed
            else if (!SupportsGpuDualSourceBlending()) return reject(__LINE__);
            result.dualSource = true;
        }
    }
    result.hasDepthStencil = depthFormat != WGPUTextureFormat_Undefined;
    if (result.hasDepthStencil) {
        auto& web = result.depthStencil;
        web.format = depthFormat;
        web.depthWriteEnabled = depth.m_depthTestEnable && depth.m_depthWriteEnable ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        web.depthCompare = depth.m_depthTestEnable ? Comparison(depth.m_depthFunc) : WGPUCompareFunction_Always;
        if (web.depthCompare == WGPUCompareFunction_Undefined) return reject(__LINE__);
        WGPUStencilFaceState face = WGPU_STENCIL_FACE_STATE_INIT;
        face.compare = WGPUCompareFunction_Always;
        face.failOp = face.depthFailOp = face.passOp = WGPUStencilOperation_Keep;
        if (depth.m_stencilEnable) {
            if (depthFormat != WGPUTextureFormat_Depth24PlusStencil8 &&
                depthFormat != WGPUTextureFormat_Depth32FloatStencil8) return reject(__LINE__);
            face.compare = Comparison(depth.m_frontFace.m_stencilFunc);
            face.passOp = StencilOperation(depth.m_frontFace.m_stencilPassOp);
            if (face.compare == WGPUCompareFunction_Undefined || !face.passOp) return reject(__LINE__);
            web.stencilReadMask = depth.m_stencilReadMask ? state.m_stencilReadMask : 255;
            web.stencilWriteMask = depth.m_stencilWriteMask ? state.m_stencilWriteMask : 255;
        } else { web.stencilReadMask = web.stencilWriteMask = 0; }
        web.stencilFront = web.stencilBack = face;
    }
    switch (raster.m_offsetMode) {
    case PSODesc::RasterizerMode::OFFSET_None: break;
    case PSODesc::RasterizerMode::OFFSET_NormalBias:
    case PSODesc::RasterizerMode::OFFSET_DecalBias: result.depthStencil.depthBias = 838; break;
    case PSODesc::RasterizerMode::OFFSET_ShadowBias: result.depthStencil.depthBiasSlopeScale = 1.0f; break;
    default: return reject(__LINE__);
    }
    output = result;
    return true;
}
}
