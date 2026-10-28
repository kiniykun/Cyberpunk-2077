#include "skperf-api-timer-web.h" // [skperf] round 39
#include <map>
#include <atomic>
#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/eventloop.h> // [wcfix] emscripten_set_interval
#include <string> // [wcfix]
#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/gpuApi/include/gpuApiInterface.h"
#include "gpu-api-texture-web.h"
#include "gpu-api-command-web.h"
#include "gpu-api-shader-web.h"
#include "gpu-shader-web.h"
#include "gpu-texture-web.h"
#include "gpu-device-web.h"
#include "gpu-command-web.h"
#include "gpu-buffer-web.h"
#include "common/gpuApi/src/gpuApiTextureUtils.h"
#include "bc-decode-web.cpp" // [nobc] unity include (no separate compile job / link object)
#include <cstring>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <set>
#include <utility>
#include <algorithm>
#include <limits>
#include <vector>
#include <array>

namespace GpuApi {
struct BrowserInternalTextureState { TextureRef m_InternalTextures[INTERTEX_Max]{}; };
static BrowserInternalTextureState& GetBrowserInternalTextureData() {
    static BrowserInternalTextureState state;
    return state;
}
}
#define GetDeviceData GetBrowserInternalTextureData
#define SDeviceData BrowserInternalTextureState
#ifndef RED_CONFIGURATION_FINAL
#error "Internal texture assertion routing must be reviewed for non-Final builds"
#endif
#define GPUAPI_ASSERT(...) RED_ASSERT(__VA_ARGS__)
#include "gpu-internal-textures-native.inc"
#undef GPUAPI_ASSERT
#undef SDeviceData
#undef GetDeviceData
#include "common/gpuApi/src/gpuApiTextureCompressor.inl"
#include "gpu-api-compression-web.inl"

namespace {
struct Format {
    WGPUTextureFormat native;
    Uint32 blockSize;
    Uint32 blockBytes;
};
bool IsDepthFormat(GpuWrapApi::eTextureFormat format) {
    return format == GpuWrapApi::TEXFMT_D16U || format == GpuWrapApi::TEXFMT_D32F ||
        format == GpuWrapApi::TEXFMT_D24S8 || format == GpuWrapApi::TEXFMT_D32FS8;
}
bool HasStencilFormat(GpuWrapApi::eTextureFormat format) {
    return format == GpuWrapApi::TEXFMT_D24S8 || format == GpuWrapApi::TEXFMT_D32FS8;
}
// [nobc] TranslateSource = the engine's (cooked) data layout. Translate = what the GPU stores: identical, except that
// BC formats become the nearest uncompressed format when the device has no texture-compression-bc.
Format TranslateSource(GpuWrapApi::eTextureFormat format) {
    using namespace GpuWrapApi;
    switch (format) {
    case TEXFMT_R8_Unorm: return {WGPUTextureFormat_R8Unorm, 1, 1};
    // Original DX12 maps legacy L8 to R8_UNORM with the default component
    // mapping. Preserve that exact one-byte storage and sampled channel.
    case TEXFMT_L8_Unorm: return {WGPUTextureFormat_R8Unorm, 1, 1};
    case TEXFMT_R8_Uint: return {WGPUTextureFormat_R8Uint, 1, 1};
    case TEXFMT_R8G8_Unorm: return {WGPUTextureFormat_RG8Unorm, 1, 2};
    case TEXFMT_R16G16_Unorm: return {WGPUTextureFormat_RG16Unorm, 1, 4};
    // [nailfix] distortionMask (glass/water distortion pass colour target 0). rg16snorm is a render/sampled
    // format with texture-formats-tier1 (requested by gpu-device-web.cpp); without it creation fails as before.
    case TEXFMT_R16G16_Snorm: return {WGPUTextureFormat_RG16Snorm, 1, 4};
    case TEXFMT_R16G16B16A16_Unorm: return {WGPUTextureFormat_RGBA16Unorm, 1, 8};
    case TEXFMT_R8G8B8A8_Unorm: return {WGPUTextureFormat_RGBA8Unorm, 1, 4};
    case TEXFMT_R8G8B8A8_Unorm_SRGB: return {WGPUTextureFormat_RGBA8UnormSrgb, 1, 4};
    case TEXFMT_R8G8B8A8_Snorm: return {WGPUTextureFormat_RGBA8Snorm, 1, 4};
    case TEXFMT_R16_Uint: return {WGPUTextureFormat_R16Uint, 1, 2};
    case TEXFMT_R16_Unorm: return {WGPUTextureFormat_R16Unorm, 1, 2};
    case TEXFMT_R32_Uint: return {WGPUTextureFormat_R32Uint, 1, 4};
    // [gamerender] Morph working texture (renderProxyMorphTargetMesh.cpp). DX12: R8G8B8A8_TYPELESS resource,
    // R32_UINT UAV (what the converted m_morphTargetTexture* kernels write), R8G8B8A8_UNORM SRV/RTV. WebGPU
    // cannot alias r32uint with rgba8unorm views, so the resource is r32uint (same 4 bytes per texel) and the
    // only UNORM reader, CompressTexture, reads a byte-identical RGBA8 copy (CreateMorphUnormCopy).
    case TEXFMT_R8G8B8A8_Uint_Typeless: return {WGPUTextureFormat_R32Uint, 1, 4};
    case TEXFMT_R32G32_Uint: return {WGPUTextureFormat_RG32Uint, 1, 8};
    case TEXFMT_R32G32B32A32_Uint: return {WGPUTextureFormat_RGBA32Uint, 1, 16};
    case TEXFMT_R16G16_Uint: return {WGPUTextureFormat_RG16Uint, 1, 4};
    case TEXFMT_R16G16B16A16_Uint: return {WGPUTextureFormat_RGBA16Uint, 1, 8};
    case TEXFMT_R10G10B10A2_Unorm: return {WGPUTextureFormat_RGB10A2Unorm, 1, 4};
    case TEXFMT_R16_Float: return {WGPUTextureFormat_R16Float, 1, 2};
    case TEXFMT_R16G16_Float: return {WGPUTextureFormat_RG16Float, 1, 4};
    case TEXFMT_R16G16B16A16_Float: return {WGPUTextureFormat_RGBA16Float, 1, 8};
    case TEXFMT_R32_Float: return {WGPUTextureFormat_R32Float, 1, 4};
    case TEXFMT_R32G32_Float: return {WGPUTextureFormat_RG32Float, 1, 8};
    case TEXFMT_R32G32B32A32_Float: return {WGPUTextureFormat_RGBA32Float, 1, 16};
    case TEXFMT_R11G11B10_Float: return {WGPUTextureFormat_RG11B10Ufloat, 1, 4};
    case TEXFMT_D32F: return {WGPUTextureFormat_Depth32Float, 1, 4};
    case TEXFMT_D16U: return {WGPUTextureFormat_Depth16Unorm, 1, 2};
    case TEXFMT_D24S8: return {WGPUTextureFormat_Depth24PlusStencil8, 1, 4};
    case TEXFMT_D32FS8: return {WGPUTextureFormat_Depth32FloatStencil8, 1, 8};
    case TEXFMT_BC1: return {WGPUTextureFormat_BC1RGBAUnorm, 4, 8};
    case TEXFMT_BC1_SRGB: return {WGPUTextureFormat_BC1RGBAUnormSrgb, 4, 8};
    case TEXFMT_BC2: return {WGPUTextureFormat_BC2RGBAUnorm, 4, 16};
    case TEXFMT_BC2_SRGB: return {WGPUTextureFormat_BC2RGBAUnormSrgb, 4, 16};
    case TEXFMT_BC3: return {WGPUTextureFormat_BC3RGBAUnorm, 4, 16};
    case TEXFMT_BC3_SRGB: return {WGPUTextureFormat_BC3RGBAUnormSrgb, 4, 16};
    case TEXFMT_BC4: return {WGPUTextureFormat_BC4RUnorm, 4, 8};
    case TEXFMT_BC5: return {WGPUTextureFormat_BC5RGUnorm, 4, 16};
    case TEXFMT_BC6H_UNSIGNED: return {WGPUTextureFormat_BC6HRGBUfloat, 4, 16};
    case TEXFMT_BC6H_SIGNED: return {WGPUTextureFormat_BC6HRGBFloat, 4, 16};
    case TEXFMT_BC7: return {WGPUTextureFormat_BC7RGBAUnorm, 4, 16};
    case TEXFMT_BC7_SRGB: return {WGPUTextureFormat_BC7RGBAUnormSrgb, 4, 16};
    default: return {WGPUTextureFormat_Undefined, 0, 0};
    }
}
bool DecodesBC(const Format& source) { return source.blockSize == 4 && cp2077::web::GpuDecodesBC(); }
// [notier1] Formats that need texture-formats-tier1 to exist, stored as core float formats without it.
Format Tier1Fallback(const Format& source) {
    if (!cp2077::web::GpuLacksTier1()) return source;
    switch (source.native) {
    case WGPUTextureFormat_R16Unorm: return {WGPUTextureFormat_R32Float, 1, 4};
    case WGPUTextureFormat_RG16Unorm: case WGPUTextureFormat_RG16Snorm: case WGPUTextureFormat_RGBA16Unorm:
    case WGPUTextureFormat_RG11B10Ufloat: return {WGPUTextureFormat_RGBA16Float, 1, 8};
    default: return source;
    }
}
bool ConvertsTier1(const Format& source) { return Tier1Fallback(source).native != source.native; }
Format Translate(GpuWrapApi::eTextureFormat format) {
    const auto source = TranslateSource(format);
    if (ConvertsTier1(source)) return Tier1Fallback(source);
    if (!DecodesBC(source)) return source;
    switch (source.native) {
    case WGPUTextureFormat_BC1RGBAUnormSrgb: case WGPUTextureFormat_BC2RGBAUnormSrgb:
    case WGPUTextureFormat_BC3RGBAUnormSrgb: case WGPUTextureFormat_BC7RGBAUnormSrgb: return {WGPUTextureFormat_RGBA8UnormSrgb, 1, 4};
    case WGPUTextureFormat_BC4RUnorm: return {WGPUTextureFormat_R8Unorm, 1, 1};
    case WGPUTextureFormat_BC4RSnorm: return {WGPUTextureFormat_R8Snorm, 1, 1};
    case WGPUTextureFormat_BC5RGUnorm: return {WGPUTextureFormat_RG8Unorm, 1, 2};
    case WGPUTextureFormat_BC5RGSnorm: return {WGPUTextureFormat_RG8Snorm, 1, 2};
    case WGPUTextureFormat_BC6HRGBUfloat: case WGPUTextureFormat_BC6HRGBFloat: return {WGPUTextureFormat_RGBA16Float, 1, 8};
    default: return {WGPUTextureFormat_RGBA8Unorm, 1, 4};
    }
}
cp2077::bc::Kind BCKind(WGPUTextureFormat source) {
    using K = cp2077::bc::Kind;
    switch (source) {
    case WGPUTextureFormat_BC1RGBAUnorm: case WGPUTextureFormat_BC1RGBAUnormSrgb: return K::BC1;
    case WGPUTextureFormat_BC2RGBAUnorm: case WGPUTextureFormat_BC2RGBAUnormSrgb: return K::BC2;
    case WGPUTextureFormat_BC3RGBAUnorm: case WGPUTextureFormat_BC3RGBAUnormSrgb: return K::BC3;
    case WGPUTextureFormat_BC4RUnorm: return K::BC4U;
    case WGPUTextureFormat_BC4RSnorm: return K::BC4S;
    case WGPUTextureFormat_BC5RGUnorm: return K::BC5U;
    case WGPUTextureFormat_BC5RGSnorm: return K::BC5S;
    case WGPUTextureFormat_BC6HRGBUfloat: return K::BC6HU;
    case WGPUTextureFormat_BC6HRGBFloat: return K::BC6HS;
    default: return K::BC7;
    }
}
// [nobc] Decodes one subresource (depth planes of srcRows block rows, srcRowBytes apart) into tightly packed texels.
// Runs on the uploading engine worker, never in a main-thread proxy.
std::atomic<Uint64> nobcTexels{0}, nobcMicros{0}; std::atomic<Uint32> nobcRegions{0};
bool DecodeBCSubresource(const Format& source, const void* data, Uint32 srcRowBytes, Uint32 srcRows, Uint32 width,
                         Uint32 height, Uint32 depth, std::vector<unsigned char>& out, Uint32& outRowBytes) {
    const auto kind = BCKind(source.native);
    const Uint32 bpp = cp2077::bc::DecodedBytesPerTexel(kind), blocksWide = (width + 3) / 4, blockRows = (height + 3) / 4;
    if (!data || srcRowBytes < blocksWide * source.blockBytes || srcRows < blockRows) return false;
    const double t0 = emscripten_get_now();
    outRowBytes = width * bpp;
    out.resize(std::size_t(outRowBytes) * height * depth);
    for (Uint32 z = 0; z < depth; ++z)
        cp2077::bc::DecodeRegion(kind, static_cast<const unsigned char*>(data) + std::size_t(z) * srcRowBytes * srcRows, srcRowBytes,
            blocksWide, blockRows, out.data() + std::size_t(z) * outRowBytes * height, outRowBytes, width, height);
    const Uint64 texels = nobcTexels.fetch_add(Uint64(width) * height * depth) + Uint64(width) * height * depth;
    const Uint64 micros = nobcMicros.fetch_add(Uint64((emscripten_get_now() - t0) * 1000)) ;
    const Uint32 n = nobcRegions.fetch_add(1) + 1;
    if (n <= 2 || (n & (n - 1)) == 0)
        std::fprintf(stderr, "CP2077 nobc decode probe: regions=%u mtexels=%.1f ms=%.0f\n", n, texels / 1e6, micros / 1000.0);
    return true;
}
// [notier1] Converts one subresource of a tier1-only format (rows srcRowBytes apart) into its float fallback.
std::uint16_t F32ToF16(float f) {
    std::uint32_t x; std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u; const int e = int((x >> 23) & 0xFF) - 127 + 15; std::uint32_t m = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFF) == 0xFF) return std::uint16_t(sign | 0x7C00u | (m ? 0x200u : 0u));
    if (e >= 31) return std::uint16_t(sign | 0x7C00u);
    if (e <= 0) { if (e < -10) return std::uint16_t(sign); m |= 0x800000u; return std::uint16_t(sign | (m >> (14 - e))); }
    return std::uint16_t(sign | (std::uint32_t(e) << 10) | (m >> 13));
}
bool ConvertTier1Subresource(const Format& source, const void* data, Uint32 srcRowBytes, Uint32 srcRows, Uint32 width,
                             Uint32 height, Uint32 depth, std::vector<unsigned char>& out, Uint32& outRowBytes) {
    const Format target = Tier1Fallback(source);
    if (!data || srcRowBytes < width * source.blockBytes || srcRows < height) return false;
    outRowBytes = width * target.blockBytes;
    out.resize(std::size_t(outRowBytes) * height * depth);
    const auto* in = static_cast<const unsigned char*>(data);
    for (Uint32 z = 0; z < depth; ++z) for (Uint32 y = 0; y < height; ++y) {
        const unsigned char* s = in + (std::size_t(z) * srcRows + y) * srcRowBytes;
        unsigned char* d = out.data() + (std::size_t(z) * height + y) * outRowBytes;
        for (Uint32 x = 0; x < width; ++x) {
            std::uint16_t h[4] = {0, 0, 0, 0x3C00u};
            switch (source.native) {
            case WGPUTextureFormat_R16Unorm: { std::uint16_t v; std::memcpy(&v, s + x * 2, 2); const float f = v / 65535.0f; std::memcpy(d + x * 4, &f, 4); continue; }
            case WGPUTextureFormat_RG16Unorm: for (int c = 0; c < 2; ++c) { std::uint16_t v; std::memcpy(&v, s + x * 4 + c * 2, 2); h[c] = F32ToF16(v / 65535.0f); } break;
            case WGPUTextureFormat_RG16Snorm: for (int c = 0; c < 2; ++c) { std::int16_t v; std::memcpy(&v, s + x * 4 + c * 2, 2); h[c] = F32ToF16(std::max(-1.0f, v / 32767.0f)); } break;
            case WGPUTextureFormat_RGBA16Unorm: for (int c = 0; c < 4; ++c) { std::uint16_t v; std::memcpy(&v, s + x * 8 + c * 2, 2); h[c] = F32ToF16(v / 65535.0f); } break;
            case WGPUTextureFormat_RG11B10Ufloat: { // same 5-bit exponent bias as half: shift the mantissas up
                std::uint32_t p; std::memcpy(&p, s + x * 4, 4);
                h[0] = std::uint16_t(((p >> 6) & 0x1F) << 10 | (p & 0x3F) << 4);
                h[1] = std::uint16_t(((p >> 17) & 0x1F) << 10 | ((p >> 11) & 0x3F) << 4);
                h[2] = std::uint16_t(((p >> 27) & 0x1F) << 10 | ((p >> 22) & 0x1F) << 5); break; }
            default: return false;
            }
            std::memcpy(d + x * 8, h, 8);
        }
    }
    return true;
}
// Device mip extent: decoded BC levels are not rounded up to whole 4x4 blocks.
Uint32 DeviceMipDimension(Uint32 base, Uint8 mip, GpuWrapApi::eTextureFormat format) {
    return DecodesBC(TranslateSource(format)) ? std::max(1u, base >> mip) : GpuApi::CalculateTextureMipDimension(base, mip, format);
}
struct TextureEntry {
    GpuApi::TextureDesc descriptor; WGPUTexture texture; Int32 references = 1;
    WGPUTextureFormat renderFormat = WGPUTextureFormat_Undefined;
    GpuApi::TextureRef backing = GpuApi::TextureRef::Null();
    std::unordered_map<Uint32, std::vector<unsigned char>> uploadCopies;
    GpuApi::STextureIndirection indirection{};
};
bool HasDepthPlane(const GpuApi::TextureDesc& descriptor) {
    // DX12's D16U resource is R16_TYPELESS. Without a depth attachment its
    // UAV is R16_UNORM (gpuApiDX12Mapping.h). Read-only shadow caches still
    // require WebGPU depth sampling and retain the original depth backing.
    return IsDepthFormat(descriptor.format) &&
        !(descriptor.format==GpuWrapApi::TEXFMT_D16U &&
          descriptor.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess) &&
          !descriptor.HasUsage(GpuApi::TEXUSAGE_DepthStencil));
}
Format TranslateResource(const GpuApi::TextureDesc& descriptor) {
    auto format=Translate(descriptor.format);
    if(descriptor.format==GpuWrapApi::TEXFMT_D16U && !HasDepthPlane(descriptor))
        format.native=WGPUTextureFormat_R16Unorm;
    return format;
}
WGPUTextureFormat StorageFormat(GpuWrapApi::eTextureFormat format) {
    if(format==GpuWrapApi::TEXFMT_D16U)return WGPUTextureFormat_R16Unorm;
    // Match the desktop UAV interpretation: sRGB is a sampled/render view,
    // while storage writes address the same RGBA8 bytes without sRGB encoding.
    return format == GpuWrapApi::TEXFMT_R8G8B8A8_Unorm_SRGB
        ? WGPUTextureFormat_RGBA8Unorm : Translate(format).native;
}
std::mutex textureMutex;
std::unordered_map<Uint32, std::unique_ptr<TextureEntry>> textures;
Uint32 nextTextureId = 1;
std::array<Uint32, UINT16_MAX> indirectTextures{};
std::vector<Uint16> freeIndirectTextures;
// [textures] Texture id whose bindless index was read through GetTextureIndirection and offered to the multilayer
// pools (gpu-texture-web.cpp); cleared when that texture is destroyed. Guarded by textureMutex.
std::array<Uint32, UINT16_MAX + 1> multilayerPooledIds{};
Uint32 nextIndirectTexture=0;
bool RegisterIndirectTexture(Uint32 id,TextureEntry& entry) {
    if(!(entry.descriptor.usage&GpuApi::TEXUSAGE_Bindless))return true;
    Uint16 index;
    if(!freeIndirectTextures.empty()) {index=freeIndirectTextures.back();freeIndirectTextures.pop_back();}
    else {
        if(nextIndirectTexture>=UINT16_MAX)return false;
        index=static_cast<Uint16>(nextIndirectTexture++);
    }
    indirectTextures[index]=id;entry.indirection=GpuApi::STextureIndirection(index);
    {
        // Census of bindless texture classes (format, size, mips) for multilayer pool sizing.
        static std::map<std::string,unsigned> classes;static unsigned total=0;
        char key[64];
        std::snprintf(key,sizeof(key),"%u:%ux%u:m%u",unsigned(entry.descriptor.format),entry.descriptor.width,entry.descriptor.height,
            unsigned(entry.descriptor.Internal_CalcTargetNumMips()));
        ++classes[key];
        if((++total&255)==0){
            std::string line;for(const auto& c:classes)line+=" "+c.first+"="+std::to_string(c.second);
            std::fprintf(stderr,"CP2077 bindless census: total=%u classes=%u%s\n",total,unsigned(classes.size()),line.c_str());
        }
    }
    return true;
}
void UnregisterIndirectTexture(const TextureEntry& entry) {
    if(!entry.indirection.IsValid())return;
    const auto index=entry.indirection.m_index;indirectTextures[index]=0;freeIndirectTextures.push_back(index);
}
std::mutex internalTextureMutex;
bool internalTexturesReady = false;
Uint64 TextureStorageBytes(const GpuApi::TextureDesc& descriptor) {
    const auto format = Translate(descriptor.format);
    if (!format.blockBytes || !descriptor.width || !descriptor.height || !descriptor.depthOrSliceNum) return 0;
    Uint64 bytes = 0;
    const bool volume = descriptor.type == GpuWrapApi::TEXTYPE_3D;
    for (Uint32 mip = 0; mip < descriptor.Internal_CalcTargetNumMips(); ++mip) {
        const Uint64 width = std::max(1u, Uint32(descriptor.width) >> mip);
        const Uint64 height = std::max(1u, Uint32(descriptor.height) >> mip);
        const Uint64 layers = volume ? std::max(1u, Uint32(descriptor.depthOrSliceNum) >> mip) : descriptor.CalcTargetSlices();
        bytes += ((width + format.blockSize - 1) / format.blockSize) *
            ((height + format.blockSize - 1) / format.blockSize) * layers * format.blockBytes;
    }
    // WebGPU does not expose driver allocation padding. This is the texture's
    // physical pixel/block storage estimate, never a claim about adapter VRAM.
    return bytes * (1u << Uint32(descriptor.sampleCount));
}
bool TextureAliasCompatible(const GpuApi::TextureDesc& source, const GpuApi::TextureDesc& destination) {
    return !destination.IsInPlace() && source.type == destination.type &&
        source.width == destination.width && source.height == destination.height &&
        source.depthOrSliceNum == destination.depthOrSliceNum && source.format == destination.format &&
        TranslateResource(source).native == TranslateResource(destination).native &&
        source.sampleCount == destination.sampleCount && source.group == destination.group &&
        source.Internal_CalcTargetNumMips() == destination.Internal_CalcTargetNumMips() &&
        (destination.usage & ~source.usage) == 0 && (destination.usageEx & ~source.usageEx) == 0;
}
bool InitializeImageImpl(WGPUTexture texture, const GpuApi::TextureDesc& descriptor,
                     const GpuApi::TextureInitData& initial, Format format) {
    using Mode = GpuApi::TextureInitData::Mode;
    if (initial.m_mode == Mode::Uninitialized || initial.m_mode == Mode::ClearToZero) return true;
    if (initial.m_mode != Mode::Uncooked && initial.m_mode != Mode::Cooked) return false;
    if (descriptor.sampleCount != GpuWrapApi::eTextureSampleCount::SampleCount1 ||
        HasDepthPlane(descriptor)) return false;
    const bool volume = descriptor.type == GpuWrapApi::TEXTYPE_3D;
    // Native DX12's single-mip volume uses one subresource at data offset 0,
    // with RowPitch/SlicePitch describing all depth planes. Multi-mip cooked
    // volume offsets still require a separate layout proof.
    if (initial.m_mode == Mode::Cooked &&
        (!initial.m_cooked.m_data || (volume && descriptor.Internal_CalcTargetNumMips()!=1))) return false;
    if (initial.m_mode == Mode::Uncooked && !initial.m_uncooked.m_getFunc) return false;
    auto* cooked = initial.m_mode == Mode::Cooked
        ? static_cast<const unsigned char*>(initial.m_cooked.m_data) : nullptr;
    const Uint32 slices = volume ? 1 : descriptor.CalcTargetSlices();
    const Uint32 mips = descriptor.Internal_CalcTargetNumMips();
    // [compress] Upload every region of the image in one main-thread proxy. Cooked data is one caller-owned
    // block; uncooked per-mip buffers are kept alive until the batch is written (the API contract only requires
    // them until CreateTexture returns). ?texBatch=0 keeps the per-mip proxies.
    static const int batchEnabled = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('texBatch') === '0' ? 0 : 1; });
    std::vector<cp2077::web::GpuTextureUploadRegion> batch;
    const bool batching = batchEnabled != 0;
    std::vector<std::unique_ptr<GpuApi::TextureInitData::Mip>> held;
    if (batching) batch.reserve(std::size_t(slices) * mips);
    const Format gpuFormat = format; format = TranslateSource(descriptor.format); // [nobc] cooked block layout
    const bool decode = format.blockSize != gpuFormat.blockSize || format.native != gpuFormat.native; // [nobc] / [notier1]
    std::vector<std::vector<unsigned char>> decoded; // [nobc] kept alive until the batch is written
    for (Uint32 slice = 0; slice < slices; ++slice) {
        for (Uint32 mip = 0; mip < mips; ++mip) {
            Uint32 width = std::max(1u, Uint32(descriptor.width) >> mip);
            Uint32 height = std::max(1u, Uint32(descriptor.height) >> mip);
            Uint32 depth = volume ? std::max(1u, Uint32(descriptor.depthOrSliceNum) >> mip) : 1;
            const Uint32 blocksWide = (width + format.blockSize - 1) / format.blockSize;
            const Uint32 rows = (height + format.blockSize - 1) / format.blockSize;
            const Uint32 rowBytes = blocksWide * format.blockBytes;
            const Uint64 byteCount = Uint64(rowBytes) * rows * depth;
            if (byteCount > std::numeric_limits<std::size_t>::max()) return false;
            const void* source = cooked;
            auto owned = std::unique_ptr<GpuApi::TextureInitData::Mip>(new GpuApi::TextureInitData::Mip{mip, slice, {}, nullptr});
            auto& provided = *owned;
            if (initial.m_mode == Mode::Uncooked) {
                initial.m_uncooked.m_getFunc(provided);
                if (provided.rawPtr && provided.buffer.Get()) return false;
                source = provided.rawPtr ? provided.rawPtr : provided.buffer.Get();
                if (!source || (provided.buffer.Get() && provided.buffer.GetSize() < byteCount)) return false;
            }
            WGPUTexelCopyTextureInfo destination = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            destination.texture = texture; destination.mipLevel = mip;
            destination.origin.z = volume ? 0 : slice;
            destination.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
            layout.bytesPerRow = rowBytes; layout.rowsPerImage = rows;
            // Compressed mip copies use the rounded physical block extent.
            WGPUExtent3D extent{blocksWide * format.blockSize, rows * format.blockSize, depth};
            std::size_t uploadBytes = static_cast<std::size_t>(byteCount);
            if (decode) {
                Uint32 outRow = 0; decoded.emplace_back();
                if (!(ConvertsTier1(format) ? ConvertTier1Subresource(format, source, rowBytes, rows, width, height, depth, decoded.back(), outRow)
                                            : DecodeBCSubresource(format, source, rowBytes, rows, width, height, depth, decoded.back(), outRow))) return false;
                source = decoded.back().data(); uploadBytes = decoded.back().size();
                layout.bytesPerRow = outRow; layout.rowsPerImage = height; extent = {width, height, depth};
            }
            if (batching) {
                batch.push_back({destination, source, uploadBytes, layout, extent});
                if (initial.m_mode == Mode::Uncooked && provided.buffer.Get()) held.push_back(std::move(owned));
            }
            else if (!cp2077::web::UploadInitialGpuTexture(destination, source,
                    uploadBytes, layout, extent)) return false;
            if (initial.m_mode == Mode::Cooked) cooked += static_cast<std::size_t>(byteCount);
        }
    }
    if (batching && !batch.empty())
        return cp2077::web::UploadInitialGpuTextureRegions(batch.data(), batch.size());
    return true;
}
// [compress] Initial-image upload cost probe (wall time the creating worker spends, incl. main-thread proxies).
bool InitializeImage(WGPUTexture texture, const GpuApi::TextureDesc& descriptor,
                     const GpuApi::TextureInitData& initial, Format format) {
    using Mode = GpuApi::TextureInitData::Mode;
    if (initial.m_mode != Mode::Uncooked && initial.m_mode != Mode::Cooked) return InitializeImageImpl(texture, descriptor, initial, format);
    static std::atomic<unsigned> probeTextures{0}; static std::atomic<std::uint64_t> micros{0}, cookedMicros{0}; static std::atomic<unsigned> cooked{0};
    const double began = emscripten_get_now();
    const bool result = InitializeImageImpl(texture, descriptor, initial, format);
    const auto spent = std::uint64_t((emscripten_get_now() - began) * 1000.0);
    micros += spent; if (initial.m_mode == Mode::Cooked) { cookedMicros += spent; ++cooked; }
    const unsigned n = ++probeTextures;
    if ((n & 255) == 0) std::fprintf(stderr, "CP2077 initial upload probe: textures=%u cooked=%u totalMs=%.1f cookedMs=%.1f\n",
        n, cooked.load(), micros.load() / 1000.0, cookedMicros.load() / 1000.0);
    return result;
}
}
namespace GpuApi {
namespace {
// The internal zero texture binds as a single-sample 2D float view. It can
// stand in only for a 2D single-slice float texture; any other dimension or
// sample type would invalidate the bind group (and so the whole encoder).
bool ZeroTextureCanStandIn(const TextureDesc& d) {
    if (d.type != GpuWrapApi::TEXTYPE_2D || d.CalcTargetSlices() != 1 ||
        Uint32(d.sampleCount) != 0 || IsDepthFormat(d.format)) return false;
    switch (Translate(d.format).native) {
    case WGPUTextureFormat_R8Uint: case WGPUTextureFormat_R16Uint: case WGPUTextureFormat_R32Uint:
    case WGPUTextureFormat_RG16Uint: case WGPUTextureFormat_RG32Uint:
    case WGPUTextureFormat_RGBA16Uint: case WGPUTextureFormat_RGBA32Uint: return false;
    default: return true;
    }
}
// Copies a live texture's descriptor without GetTextureDesc's per-call stale log.
bool FindTextureDescQuiet(const TextureRef& reference, TextureDesc& descriptor) {
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry == textures.end()) return false;
    descriptor = entry->second->descriptor; return true;
}
// A per-slot view failure affects only that slot; the command list survives.
// Logs the first failure per (site, stale, format, type, usage, usageEx).
void ReportTextureSlotFailure(const char* site, const TextureRef& reference, Uint32 slot, unsigned stage,
                              const char* substitute) {
    TextureDesc d;
    const bool live = FindTextureDescQuiet(reference, d);
    const std::uint64_t key = (std::uint64_t(!live) << 63) | (std::uint64_t(d.format) << 48) | (std::uint64_t(d.type) << 40) |
        ((std::uint64_t(d.usageEx) & 0xFF) << 32) | std::uint64_t(std::uint32_t(d.usage));
    static std::mutex reportMutex;
    static std::set<std::pair<const void*, std::uint64_t>> reported;
    {
        std::lock_guard<std::mutex> lock(reportMutex);
        if (reported.size() >= 256 || !reported.insert({site, key}).second) return;
    }
    std::fprintf(stderr, "CP2077 WebGPU: %s slot view failed, bound %s: ref=%u stale=%u stage=%u slot=%u w=%u h=%u slices=%u type=%u fmt=%u mips=%u samples=%u usage=0x%x usageEx=0x%x\n",
        site, substitute, reference.ID(), unsigned(!live), stage, slot, unsigned(d.width), unsigned(d.height), unsigned(d.depthOrSliceNum),
        unsigned(d.type), unsigned(d.format), unsigned(d.numMips), unsigned(d.sampleCount), unsigned(d.usage), unsigned(d.usageEx));
}
// Replaces a failed slot with an acquired zero-texture view when that is
// type-compatible, else with null (ref and view both null). Returns the label.
const char* SubstituteFailedTextureSlot(TextureRef& reference, WGPUTextureView& view) {
    const TextureRef failed = reference;
    const TextureRef zero = GetInternalTexture(INTERTEX_Zero);
    reference = TextureRef::Null(); view = nullptr;
    TextureDesc descriptor;
    if (zero.isNull() || failed.ID() == zero.ID() || !FindTextureDescQuiet(failed, descriptor) ||
        !ZeroTextureCanStandIn(descriptor) ||
        !cp2077::web::AcquireSampledGpuTextureView(zero, view)) {
        view = nullptr; return "null";
    }
    reference = zero; return "zero";
}
void BindComputeTextureRange(Uint32 start, Uint32 count, const TextureRef* references,
                             const Uint32* mips, bool storage) {
    if (!count) return;
    const Uint32 limit = storage ? MAX_UAV_BINDINGS : MAX_SRV_BINDINGS;
    if (start > limit || count > limit - start) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: invalid compute texture range\n", stderr); return;
    }
    std::vector<TextureRef> boundReferences(count, TextureRef::Null());
    if (references) boundReferences.assign(references, references + count);
    std::vector<WGPUTextureView> views(count, nullptr);
    for (Uint32 index = 0; index < count; ++index) {
        auto& reference = boundReferences[index];
        if (reference.isNull()) continue;
        const bool success = storage
            ? cp2077::web::AcquireStorageGpuTextureView(reference, mips ? mips[index] : 0, views[index])
            : cp2077::web::AcquireSampledGpuTextureView(reference, views[index]);
        if (success) continue;
        // The failing view factory released its own hold. A storage slot has
        // no stand-in; it stays null and only a dispatch reading it is affected.
        const TextureRef failed = reference;
        const char* substitute = "null";
        if (storage) { reference = TextureRef::Null(); views[index] = nullptr; }
        else substitute = SubstituteFailedTextureSlot(reference, views[index]);
        ReportTextureSlotFailure(storage ? "compute storage texture" : "compute sampled texture",
            failed, start + index, 2, substitute);
    }
    if (cp2077::web::SetBoundGpuComputeTextures(start, count, boundReferences.data(), views.data(), storage)) return;
    // The commit rejects only an unbound, foreign-thread or already-failed
    // list (or marks it failed itself), so release the holds without re-failing.
    for (Uint32 index = 0; index < count; ++index) {
        if (views[index]) {
            cp2077::web::ReleaseGpuTextureView(views[index]); Release(boundReferences[index]);
        }
    }
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 4)
        std::fputs("CP2077 WebGPU: compute texture binding not committed (list unbound or already failed)\n", stderr);
}
}
void BindTextures(Uint32 start, Uint32 count, const TextureRef* references, GpuWrapApi::eShaderType stage) {
    if (stage != GpuWrapApi::VertexShader && stage != GpuWrapApi::PixelShader &&
        stage != GpuWrapApi::ComputeShader) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: unsupported sampled texture stage\n", stderr); return;
    }
    BindTextures(start, count, references, GpuWrapApi::ShaderTypeMask(stage));
}
void BindTextures(Uint32 start, Uint32 count, const TextureRef* references, GpuWrapApi::ShaderTypeMask mask) {
    CpApiTimer cpApiTimer(5); // [skperf] round 39
    if (!count) return;
    const GpuWrapApi::ShaderTypeMask allowed(GpuWrapApi::VertexShader, GpuWrapApi::PixelShader, GpuWrapApi::ComputeShader);
    if ((mask.Get() & ~allowed.Get()) || start > MAX_SRV_BINDINGS || count > MAX_SRV_BINDINGS - start) {
        cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: invalid sampled texture stage mask or range\n", stderr); return;
    }
    const bool selected[3] = {mask.Has(GpuWrapApi::VertexShader), mask.Has(GpuWrapApi::PixelShader),
                              mask.Has(GpuWrapApi::ComputeShader)};
    if (!selected[0] && !selected[1] && !selected[2]) return;
    // [cmdlock] count <= MAX_SRV_BINDINGS (checked above): fixed stack storage instead of 8 heap vectors per call
    // (the shared allocator lock was a top contention point on the render workers).
    std::array<TextureRef, MAX_SRV_BINDINGS> clearReferences;
    if (!references) {
        clearReferences.fill(TextureRef::Null()); references = clearReferences.data();
    }
    const TextureRef* logicalReferences = references;
    std::array<TextureRef, MAX_SRV_BINDINGS> resolvedReferences;
    for (Uint32 i = 0; i < count; ++i)
        resolvedReferences[i] = references[i].isNull() ? GetInternalTexture(INTERTEX_Zero) : references[i];
    // [cmdlock] Rebinding the references already bound is a no-op: skip the view acquisition and the commit.
    // ?bindEarly=0 disables (A/B).
    static const int bindEarly = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('bindEarly') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 cmdlock switches: bindEarly=%d\n", v); return v; }();
    if (bindEarly && cp2077::web::BoundGpuSampledTexturesMatch(start, count, resolvedReferences.data(), selected, references)) {
        cp2077::web::CountCmdlockEvent(0);
        return;
    }
    // Each stage owns a separate retained reference and view, so a failed slot
    // is replaced (zero texture or null) in that stage only.
    cp2077::web::CountCmdlockEvent(1);
    // [skperf] round 43 bindKeep (default ON, ?bindKeep=0 old): slots that already hold exactly the requested
    // reference keep their binding (no view acquire here, no release in the commit). See gpu-api-command-web.cpp.
    static const int bindKeep = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ try { return new URLSearchParams(location.search).get('bindKeep') === '0' ? 0 : 1; } catch (e) { return 1; } });
        std::fprintf(stderr, "CP2077 skperf switches: bindKeep=%d\n", v); return v; }();
    bool keep[3 * MAX_SRV_BINDINGS];
    const bool useKeep = bindKeep && bindEarly &&
        cp2077::web::BoundGpuSampledTexturesKeep(start, count, resolvedReferences.data(), selected, references, keep);
    std::array<TextureRef, MAX_SRV_BINDINGS> stageRefVectors[3];
    std::array<WGPUTextureView, MAX_SRV_BINDINGS> stageViews[3];
    const TextureRef* stageRefs[3] = {};
    const WGPUTextureView* views[3] = {};
    for (unsigned stage = 0; stage < 3; ++stage) {
        if (!selected[stage]) continue;
        for (Uint32 i = 0; i < count; ++i) { stageRefVectors[stage][i] = resolvedReferences[i]; stageViews[stage][i] = nullptr; }
        stageRefs[stage] = stageRefVectors[stage].data();
        views[stage] = stageViews[stage].data();
        for (Uint32 i = 0; i < count; ++i) {
            auto& reference = stageRefVectors[stage][i];
            if (reference.isNull()) continue;
            if (useKeep && keep[stage * MAX_SRV_BINDINGS + i]) continue; // [skperf] round 43 bindKeep (view stays null: no hold)
            if (cp2077::web::AcquireSampledGpuTextureView(reference, stageViews[stage][i])) continue;
            const TextureRef failed = reference;
            const char* substitute = SubstituteFailedTextureSlot(reference, stageViews[stage][i]);
            ReportTextureSlotFailure("sampled texture", failed, start + i, stage, substitute);
        }
    }
    if (useKeep ? cp2077::web::SetBoundGpuSampledTexturesKeep(start, count, stageRefs, views, selected, logicalReferences, keep)
                : cp2077::web::SetBoundGpuSampledTextures(start, count, stageRefs, views, selected, logicalReferences)) return;
    // The commit rejects only an unbound, foreign-thread or already-failed
    // list (or marks it failed itself), so release the holds without re-failing.
    for (unsigned stage = 0; stage < 3; ++stage)
        for (std::size_t i = 0; selected[stage] && i < count; ++i)
            if (stageViews[stage][i]) {
                cp2077::web::ReleaseGpuTextureView(stageViews[stage][i]); Release(stageRefVectors[stage][i]);
            }
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 4)
        std::fputs("CP2077 WebGPU: sampled texture binding not committed (list unbound or already failed)\n", stderr);
}
void BindTextureUAVs(Uint32 start, Uint32 count, const TextureRef* references, const Uint32* mips) {
    BindComputeTextureRange(start, count, references, mips, true);
}
void BindTextureMipLevelUAV(Uint32 slot, const TextureRef& reference, Uint32 mip) {
    BindComputeTextureRange(slot, 1, &reference, &mip, true);
}
namespace {
void BindBrowserSelectedTexture(Uint32 slot, const TextureRef& reference, GpuWrapApi::eShaderType shaderStage,
                                bool stencil, Uint32 mip = UINT32_MAX, bool mipGeneration = false, Uint32 slice = UINT32_MAX) {
    if (slot >= MAX_SRV_BINDINGS || (shaderStage != GpuWrapApi::VertexShader &&
        shaderStage != GpuWrapApi::PixelShader && shaderStage != GpuWrapApi::ComputeShader)) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 4)
            std::fprintf(stderr, "CP2077 WebGPU: skipped selected texture bind: slot=%u stage=%u ref=%u\n",
                unsigned(slot), unsigned(shaderStage), unsigned(reference.ID()));
        return;
    }
    if (reference.isNull()) return; // Original selected-view bindings leave null inputs unchanged.
    TextureRef bound = reference;
    WGPUTextureView view = nullptr;
    if (!cp2077::web::AcquireSampledGpuTextureView(reference, view, stencil, mip, mipGeneration, slice)) {
        // Substitute zero or null for this slot only. A full-chain default
        // view is not used: it could overlap a mip being written in the same
        // pass. A stencil view (uint) has no float stand-in.
        const char* substitute = "null";
        view = nullptr;
        if (stencil) bound = TextureRef::Null();
        else substitute = SubstituteFailedTextureSlot(bound, view);
        ReportTextureSlotFailure(stencil ? "stencil texture" : mipGeneration ? "mip-generation texture" : "selected mip texture",
            reference, slot, unsigned(shaderStage), substitute);
    }
    const bool selected[3] = {shaderStage == GpuWrapApi::VertexShader, shaderStage == GpuWrapApi::PixelShader,
                              shaderStage == GpuWrapApi::ComputeShader};
    const TextureRef* refs[3] = {&bound, &bound, &bound};
    const WGPUTextureView* views[3] = {&view, &view, &view};
    // A rejected commit means the list is unbound or already failed; only
    // release this call's hold.
    if (!cp2077::web::SetBoundGpuSampledTextures(slot, 1, refs, views, selected) && view) {
        cp2077::web::ReleaseGpuTextureView(view); Release(bound);
    }
}
}
void BindTextureStencil(Uint32 slot, const TextureRef& texture, GpuWrapApi::eShaderType stage) {
    BindBrowserSelectedTexture(slot, texture, stage, true);
}
void BindTextureMipLevel(Uint32 slot, const TextureRef& texture, Uint32 slice, Uint32 mip, GpuWrapApi::eShaderType stage) {
    // Original DX12 asserts slice==0 but selects the view by mip alone.
    if (slice != 0) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1) < 4)
            std::fprintf(stderr, "CP2077 WebGPU: BindTextureMipLevel slice=%u ignored: ref=%u slot=%u mip=%u\n",
                unsigned(slice), unsigned(texture.ID()), unsigned(slot), unsigned(mip));
    }
    BindBrowserSelectedTexture(slot, texture, stage, false, mip);
}
void BindTextureMipLevelForMipGeneration(Uint32 slot, const TextureRef& texture, Uint32 slice, Uint32 mip,
                                       GpuWrapApi::eShaderType stage) {
    BindBrowserSelectedTexture(slot, texture, stage, false, mip, true, slice);
}
void BindTextureMipLevelForMipGeneration(Uint32 slot, const TextureRef& texture, Uint32 mip, GpuWrapApi::eShaderType stage) {
    BindBrowserSelectedTexture(slot, texture, stage, false, mip, true);
}
TextureRef CreateTexture(const TextureDesc& descriptor, const TextureInitData& initial) {
    auto format = TranslateResource(descriptor);
    if (!descriptor.width || !descriptor.height || !descriptor.depthOrSliceNum ||
        descriptor.IsInPlace() || !format.blockBytes ||
        (initial.m_mode != TextureInitData::Mode::Uninitialized && initial.m_mode != TextureInitData::Mode::ClearToZero &&
         initial.m_mode != TextureInitData::Mode::Uncooked && initial.m_mode != TextureInitData::Mode::Cooked)) {
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1)<16) std::fprintf(stderr,"CP2077 WebGPU: unsupported texture descriptor: format=%u dimensions=%ux%ux%u inPlace=%u blockBytes=%u initialMode=%u\n",
            unsigned(descriptor.format),descriptor.width,descriptor.height,descriptor.depthOrSliceNum,
            unsigned(descriptor.IsInPlace()),format.blockBytes,unsigned(initial.m_mode));
        return TextureRef::Null();
    }
    WGPUTextureDescriptor native = WGPU_TEXTURE_DESCRIPTOR_INIT;
    native.format = format.native;
    WGPUTextureFormat sampledViewFormat = WGPUTextureFormat_Undefined;
    if (descriptor.HasUsage(TEXUSAGEEX_Internal_BackBuffer) &&
        descriptor.format == GpuWrapApi::TEXFMT_R8G8B8A8_Unorm) {
        sampledViewFormat = WGPUTextureFormat_RGBA8UnormSrgb;
        native.viewFormatCount = 1; native.viewFormats = &sampledViewFormat;
    }
    if (descriptor.HasUsage(TEXUSAGE_UnorderedAccess) &&
        descriptor.format == GpuWrapApi::TEXFMT_R8G8B8A8_Unorm_SRGB) {
        native.format = StorageFormat(descriptor.format);
        sampledViewFormat = format.native;
        native.viewFormatCount = 1;
        native.viewFormats = &sampledViewFormat;
    }
    if (cp2077::web::GpuLacksTier1() && descriptor.HasUsage(TEXUSAGE_RenderTarget) && !descriptor.HasUsage(TEXUSAGE_UnorderedAccess) &&
        (initial.m_mode == TextureInitData::Mode::Uninitialized || initial.m_mode == TextureInitData::Mode::ClearToZero)) { // [notier1] RG16 targets: rg16float (gpu-texture-web Tier1CoreFormat)
        const auto source = TranslateSource(descriptor.format);
        if (source.native == WGPUTextureFormat_RG16Unorm || source.native == WGPUTextureFormat_RG16Snorm ||
            (source.native == WGPUTextureFormat_RG11B10Ufloat && cp2077::web::GpuRendersRG11B10())) native.format = source.native;
    }
    native.dimension = descriptor.type == GpuWrapApi::TEXTYPE_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    native.size = {descriptor.width, descriptor.height,
        descriptor.type == GpuWrapApi::TEXTYPE_3D ? descriptor.depthOrSliceNum : descriptor.CalcTargetSlices()};
    native.mipLevelCount = descriptor.Internal_CalcTargetNumMips();
    native.sampleCount = 1u << Uint32(descriptor.sampleCount);
    if (native.sampleCount != 1 && native.sampleCount != 4) return TextureRef::Null();
    if (native.sampleCount == 1) native.usage = WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst;
    if (descriptor.HasUsage(TEXUSAGE_Samplable)||descriptor.HasUsage(TEXUSAGE_Bindless)) native.usage |= WGPUTextureUsage_TextureBinding;
    if (descriptor.HasUsage(TEXUSAGE_RenderTarget) || descriptor.HasUsage(TEXUSAGE_DepthStencil)) native.usage |= WGPUTextureUsage_RenderAttachment;
    if (descriptor.HasUsage(TEXUSAGE_UnorderedAccess)) native.usage |= WGPUTextureUsage_StorageBinding;
    if (descriptor.type == GpuWrapApi::TEXTYPE_CUBE && descriptor.width != descriptor.height) return TextureRef::Null();
    auto texture = cp2077::web::CreateGpuTexture(native);
    if (!texture) return TextureRef::Null();
    if (!InitializeImage(texture, descriptor, initial, format)) {
        static std::atomic<unsigned> uploadFailureReports{0};
        if(uploadFailureReports.fetch_add(1)<32)std::fprintf(stderr,
            "CP2077 WebGPU: texture initial image upload failed: mode=%u type=%u format=%u width=%u height=%u depthOrSlices=%u mips=%u samples=%u\n",
            unsigned(initial.m_mode),unsigned(descriptor.type),unsigned(descriptor.format),
            unsigned(descriptor.width),unsigned(descriptor.height),unsigned(descriptor.depthOrSliceNum),
            unsigned(descriptor.Internal_CalcTargetNumMips()),unsigned(descriptor.sampleCount));
        cp2077::web::DestroyGpuTexture(texture); return TextureRef::Null();
    }
    Uint32 id = 0;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        if (nextTextureId <= Uint32(INT32_MAX)) {
            const auto candidate=nextTextureId++;
            auto entry=std::make_unique<TextureEntry>(TextureEntry{descriptor, texture, 1});
            if(RegisterIndirectTexture(candidate,*entry)) {id=candidate;textures.emplace(id,std::move(entry));}
        }
    }
    if (!id) cp2077::web::DestroyGpuTexture(texture);
    else if(descriptor.format==GpuWrapApi::TEXFMT_D16U && !HasDepthPlane(descriptor)) {
        static std::atomic<unsigned> reports{0};
        if(reports.fetch_add(1)<16)
            std::fprintf(stderr,"CP2077 texture format probe: ref=%u nativeFormat=%u browserFormat=%u usage=%u size=%ux%ux%u\n",
                id,unsigned(descriptor.format),unsigned(native.format),unsigned(descriptor.usage),
                descriptor.width,descriptor.height,descriptor.depthOrSliceNum);
    }
    return TextureRef{id};
}
TextureAliasingDesc GetAliasingDesc(const TextureDesc& descriptor) {
    TextureAliasingDesc result{};
    const auto bytes = TextureStorageBytes(descriptor);
    if (bytes && bytes <= UINT32_MAX && !descriptor.IsInPlace()) result.size = Uint32(bytes);
    return result;
}
Bool CanTexturesAlias(TextureRef existing, const TextureDesc& descriptor, const TextureAliasingDesc& aliasing) {
    const auto expected = GetAliasingDesc(descriptor);
    if (!expected.size || aliasing.size != expected.size) return false;
    if (existing.isNull()) return true;
    std::lock_guard<std::mutex> lock(textureMutex);
    const auto entry = textures.find(existing.ID());
    return entry != textures.end() && TextureAliasCompatible(entry->second->descriptor, descriptor);
}
TextureRef CreateAliasingTexture(TextureRef existing, const TextureDesc& descriptor, const TextureInitData& initial) {
    if (initial.m_mode != TextureInitData::Mode::Uninitialized || !GetAliasingDesc(descriptor).size)
        return TextureRef::Null();
    if (existing.isNull()) return CreateTexture(descriptor, initial);
    std::lock_guard<std::mutex> lock(textureMutex);
    const auto found = textures.find(existing.ID());
    if (found == textures.end() || nextTextureId > Uint32(INT32_MAX)) return TextureRef::Null();
    auto& entry = *found->second;
    if (entry.references == INT32_MAX || !TextureAliasCompatible(entry.descriptor, descriptor)) return TextureRef::Null();
    const auto id = nextTextureId++;
    auto alias = std::make_unique<TextureEntry>(TextureEntry{descriptor, entry.texture, 1, entry.renderFormat, existing});
    if(!RegisterIndirectTexture(id,*alias))return TextureRef::Null();
    textures.emplace(id, std::move(alias));
    ++entry.references;
    return TextureRef{id};
}
Uint32 GetTextureUsedVideoMemory(const TextureRef& reference) {
    std::lock_guard<std::mutex> lock(textureMutex);
    const auto found = textures.find(reference.ID());
    return found == textures.end() ? 0 : Uint32(std::min<Uint64>(UINT32_MAX, TextureStorageBytes(found->second->descriptor)));
}
void AddRef(const TextureRef& reference) {
    if (reference.isNull()) return;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry != textures.end() && entry->second->references < INT32_MAX) ++entry->second->references;
    else std::fputs("CP2077 WebGPU: invalid or overflowing texture reference\n", stderr);
}
Int32 Release(const TextureRef& reference) {
    if (reference.isNull()) return 0;
    WGPUTexture texture = nullptr;
    Int32 count = 0;
    TextureRef backing = TextureRef::Null();
    std::uint16_t pooledIndex = 0; WGPUTexture pooledTexture = nullptr; bool pooled = false;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto entry = textures.find(reference.ID());
        if (entry == textures.end()) return 0;
        count = --entry->second->references;
        if (!count) { backing = entry->second->backing;
            const auto& indirection = entry->second->indirection;
            if (indirection.IsValid() && multilayerPooledIds[indirection.m_index] == reference.ID()) {
                multilayerPooledIds[indirection.m_index] = 0;
                pooledIndex = indirection.m_index; pooledTexture = entry->second->texture; pooled = true;
            }
            UnregisterIndirectTexture(*entry->second);
            if (backing.isNull()) texture = entry->second->texture;
            textures.erase(entry); }
    }
    if (pooled) cp2077::web::UnpoolGpuMultilayerIndex(pooledIndex, pooledTexture);
    if (texture && !cp2077::web::DestroyGpuTexture(texture)) std::fputs("CP2077 WebGPU: texture destruction proxy failed\n", stderr);
    if (!backing.isNull()) Release(backing);
    return count;
}
const TextureDesc& GetTextureDesc(const TextureRef& reference) {
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry != textures.end()) return entry->second->descriptor;
    std::fputs("CP2077 WebGPU: stale texture descriptor reference\n", stderr);
    static const TextureDesc empty; return empty;
}
STextureIndirection GetTextureIndirection(const TextureRef& reference) {
    if(reference.isNull())return {};
    STextureIndirection result; WGPUTexture poolSource=nullptr; TextureDesc poolDesc;
    {
        std::lock_guard<std::mutex> guard(textureMutex);
        auto found=textures.find(reference.ID());
        if(found==textures.end())return STextureIndirection();
        result=found->second->indirection;
        // [textures] The engine reads a bindless index only to hand it to t130 shaders (multilayer layer
        // textures, ML proxies, light projections): mirror that texture into the emulated bindless pools once.
        if(result.IsValid() && multilayerPooledIds[result.m_index]!=reference.ID() && found->second->texture &&
           found->second->references<INT32_MAX){
            multilayerPooledIds[result.m_index]=reference.ID();
            poolSource=found->second->texture; poolDesc=found->second->descriptor; ++found->second->references;
        }
    }
    if(poolSource){
        if(poolDesc.type==GpuWrapApi::TEXTYPE_2D && poolDesc.CalcTargetSlices()==1 && Uint32(poolDesc.sampleCount)==0 &&
           !IsDepthFormat(poolDesc.format)){
            const auto format=TranslateResource(poolDesc);
            cp2077::web::PoolGpuMultilayerTexture(result.m_index,poolSource,format.native,poolDesc.width,poolDesc.height,
                poolDesc.Internal_CalcTargetNumMips(),format.blockSize,format.blockSize,format.blockBytes);
        }
        Release(reference);
    }
    return result;
}
TextureLevelDesc GetTextureLevelDesc(const TextureRef& reference, Uint16 level) {
    TextureLevelDesc result;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry == textures.end() || level >= entry->second->descriptor.Internal_CalcTargetNumMips()) {
        std::fputs("CP2077 WebGPU: invalid texture level descriptor request\n", stderr);
        return result;
    }
    const auto& original = entry->second->descriptor;
    // Match the original common implementation, including format-specific mip sizing.
    result.width = CalculateTextureMipDimension(original.width, Uint8(level), original.format);
    result.height = CalculateTextureMipDimension(original.height, Uint8(level), original.format);
    return result;
}

// [textures] A later write into a texture mirrored in the multilayer pools makes its pool copy stale.
void NoteMultilayerTextureWrite(const GpuApi::TextureRef& reference) {
    if (reference.isNull()) return;
    WGPUTexture texture = nullptr;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto found = textures.find(reference.ID());
        if (found == textures.end() || !found->second->indirection.IsValid() ||
            multilayerPooledIds[found->second->indirection.m_index] != reference.ID()) return;
        texture = found->second->texture;
    }
    cp2077::web::NoteGpuMultilayerSourceWrite(texture);
}
}

// ---------------------------------------------------------------------------------------------------------------
// [wcfix] Multilayer fallback audit + on-demand re-pool.
// The original DX12 GpuApi writes every bindless texture's descriptor into the t130 table when the texture is
// created (gpuApiDX12Texture.cpp: AllocateBindless(srv, texId - 1)), so a layer index in an mlsetup always reads
// whatever live texture holds that index. The port mirrors a texture into the emulated pools only when the engine
// calls GetTextureIndirection on that exact texture; any other live texture that holds an index referenced by a
// live mlsetup stays unmapped and its layer reads the neutral (0.5, 0.5, 1, 1) default. Multiplied by the layer
// tint (colorScale * 2) that is flat white/grey. Once per second (main thread) this audit:
//   - counts live mlsetups that reference an unmapped/rejected index, by reason, with their resource names;
//   - (default, ?mlRepool=0 = old) offers the live texture of every unmapped referenced index to the pools,
//     which restores the original table semantics for exactly the indices the multilayer shaders read.
namespace cp2077::web { void GetGpuMultilayerPoolCounters(unsigned (&out)[8]); } // [whitewall] r6, gpu-texture-web.cpp
namespace {
struct CpMlSetupRecord { std::string name; std::vector<Uint16> ids; };
std::mutex cpMlAuditMutex;
std::unordered_map<const void*, CpMlSetupRecord> cpMlSetups;
unsigned cpMlRegistered = 0, cpMlUnregistered = 0;
// [skperf] round 5: re-pool now OFF by default (it repooled 0 in q001; the 1 s main-thread audit walked ~13.7k setups).
// ?mlRepool=1 = wcfix re-pool (and its audit); ?perfprobe=1 = audit/probe lines only.
int CpMlRepoolEnabled() {
    static const int value = MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('mlRepool') === '1' ? 1 : 0; } catch (e) { return 0; } });
    return value;
}
int CpMlAuditEnabled() {
    static const int value = CpMlRepoolEnabled() || MAIN_THREAD_EM_ASM_INT({
        try { return new URLSearchParams(location.search).get('perfprobe') === '1' ? 1 : 0; } catch (e) { return 0; } });
    return value;
}
// Main thread. Offers the live texture that holds index to the pools (as GetTextureIndirection does).
// Returns 1 when an offer was made, 0 when there is nothing to offer, -1 when the texture cannot be pooled.
int CpMlOfferLiveIndex(Uint16 index, bool clearStaleMark) {
    WGPUTexture source = nullptr; GpuApi::TextureDesc desc; Uint32 id = 0;
    {
        std::lock_guard<std::mutex> guard(textureMutex);
        id = indirectTextures[index];
        if (!id) return 0;
        auto found = textures.find(id);
        if (found == textures.end() || !found->second->texture || found->second->references >= INT32_MAX) return 0;
        if (multilayerPooledIds[index] == id && !clearStaleMark) return 0;
        desc = found->second->descriptor;
        if (!(desc.type == GpuWrapApi::TEXTYPE_2D && desc.CalcTargetSlices() == 1 && Uint32(desc.sampleCount) == 0 &&
              !IsDepthFormat(desc.format))) return -1;
        multilayerPooledIds[index] = id;
        source = found->second->texture; ++found->second->references;
    }
    const auto format = TranslateResource(desc);
    cp2077::web::PoolGpuMultilayerTexture(index, source, format.native, desc.width, desc.height,
        desc.Internal_CalcTargetNumMips(), format.blockSize, format.blockSize, format.blockBytes);
    GpuApi::Release(GpuApi::TextureRef{id});
    return 1;
}
void CpMlAuditTick(void*) {
    static unsigned ticks = 0, lines = 0, cleanTicks = 0;
    ++ticks;
    // 1. unique referenced indices
    static std::vector<std::uint8_t> referenced(65536);
    std::fill(referenced.begin(), referenced.end(), 0);
    std::vector<Uint16> unique;
    unsigned setups = 0;
    {
        std::lock_guard<std::mutex> lock(cpMlAuditMutex);
        setups = unsigned(cpMlSetups.size());
        for (const auto& item : cpMlSetups)
            for (Uint16 id : item.second.ids)
                if (!referenced[id]) { referenced[id] = 1; unique.push_back(id); }
    }
    if (!setups) return;
    std::vector<std::uint8_t> states(unique.size());
    std::vector<const char*> reasons(unique.size());
    unsigned chunks = cp2077::web::GetGpuMultilayerIndexStates(unique.data(), unsigned(unique.size()), states.data(), reasons.data());
    // 2. classify unmapped indices (and repair them)
    const int repool = CpMlRepoolEnabled();
    unsigned dead = 0, notOffered = 0, lost = 0, unpoolable = 0, rejected = 0, repooled = 0, high = 0;
    std::map<std::string, unsigned> rejectHistogram;
    static std::vector<std::uint8_t> bad(65536);
    std::fill(bad.begin(), bad.end(), 0);
    for (size_t i = 0; i < unique.size(); ++i) {
        const Uint16 index = unique[i];
        if (states[i] == 1) continue;
        if (states[i] == 2) { ++high; bad[index] = 2; continue; }
        if (states[i] == 3) {
            ++rejected; ++rejectHistogram[reasons[i] ? reasons[i] : "?"]; bad[index] = 1;
            // transient reasons are retried
            if (repool && reasons[i] && (!std::strcmp(reasons[i], "proxy") || !std::strcmp(reasons[i], "device")) &&
                repooled < 256 && CpMlOfferLiveIndex(index, true) > 0) ++repooled;
            continue;
        }
        bad[index] = 1;
        Uint32 live = 0, marked = 0;
        {
            std::lock_guard<std::mutex> guard(textureMutex);
            live = indirectTextures[index]; marked = multilayerPooledIds[index];
        }
        if (!live) { ++dead; continue; }
        if (marked != live) ++notOffered; else ++lost;
        if (repool && repooled < 256) {
            const int offered = CpMlOfferLiveIndex(index, marked == live);
            if (offered > 0) ++repooled; else if (offered < 0) { ++unpoolable; }
        }
    }
    // 3. per setup
    unsigned fallbackSetups = 0, highSetups = 0;
    std::string names;
    {
        std::lock_guard<std::mutex> lock(cpMlAuditMutex);
        for (const auto& item : cpMlSetups) {
            unsigned badIds = 0, highIds = 0;
            for (Uint16 id : item.second.ids) { if (bad[id] == 1) ++badIds; else if (bad[id] == 2) ++highIds; }
            if (highIds) ++highSetups;
            if (!badIds) continue;
            ++fallbackSetups;
            if (names.size() < 900) {
                char part[320];
                std::snprintf(part, sizeof(part), "%s%s[%u/%u]", names.empty() ? "" : ",",
                    item.second.name.empty() ? "<unnamed>" : item.second.name.c_str(), badIds, unsigned(item.second.ids.size()));
                names += part;
            }
        }
    }
    std::string rejectText;
    for (const auto& r : rejectHistogram) rejectText += (rejectText.empty() ? "" : ",") + r.first + ":" + std::to_string(r.second);
    const bool problem = fallbackSetups || high || repooled;
    if (!problem) ++cleanTicks;
    // budget: every problem second for the first 300 lines, then every 10th; a clean summary every 30 s
    if ((problem && (lines < 300 || ticks % 10 == 0)) || (!problem && cleanTicks % 30 == 1)) {
        ++lines;
        std::fprintf(stderr, "CP2077 multilayer fallback probe: t=%u setups=%u (reg=%u unreg=%u) fallbackSetups=%u highSlotSetups=%u "
            "uniqueIds=%u dead=%u notOffered=%u lost=%u unpoolable=%u rejected=%u(%s) highSlotIds=%u repooled=%u chunks=%u mlRepool=%d names=%s\n",
            ticks, setups, cpMlRegistered, cpMlUnregistered, fallbackSetups, highSetups, unsigned(unique.size()), dead, notOffered,
            lost, unpoolable, rejected, rejectText.c_str(), high, repooled, chunks, repool, names.c_str());
    }
    // [whitewall] r6 pool counters (cumulative, heap-free): every 5 s while the audit runs.
    if (ticks % 5 == 0) {
        unsigned c[8]; cp2077::web::GetGpuMultilayerPoolCounters(c);
        std::fprintf(stderr, "CP2077 multilayer pool counters: pooled=%u tinyPooled=%u sameSourceReoffers(stale-refreshed)=%u "
            "replaced=%u sourceRewrites=%u refreshCopies=%u texdebugZeroed=%u dirtyNow=%u\n", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
    }
}
void CpMlArmAudit() {
    std::fprintf(stderr, "CP2077 multilayer fallback probe armed (1 s, main thread; default no re-pool; ?mlRepool=1 = re-pool)\n");
    emscripten_set_interval(CpMlAuditTick, 1000.0, nullptr);
}
}
namespace cp2077::web {
// Called by the renderMultilayerSetup.cpp overlay (RenderMultilayerSetup::Create / destructor).
void MultilayerSetupRegister(const void* key, const char* name, const std::uint16_t* ids, unsigned count) {
    // [whitewall] r9 probe (always on, capped at 96 lines): which multilayer setups use a tiny (<= 4x4) layer texture,
    // in which slot. ids = per layer {microblend, diffuse, normal, roughness, metalness} bindless indices.
    {
        static std::atomic<unsigned> lines{0};
        if (key && ids && lines.load(std::memory_order_relaxed) < 96) {
            static const char* const slotNames[5] = {"microblend", "diffuse", "normal", "roughness", "metalness"};
            for (unsigned i = 0; i < count; ++i) {
                Uint32 w = 0, h = 0, mips = 0, fmt = 0, id = 0;
                {
                    std::lock_guard<std::mutex> guard(textureMutex);
                    id = indirectTextures[ids[i]];
                    auto found = id ? textures.find(id) : textures.end();
                    if (found == textures.end()) continue;
                    const auto& d = found->second->descriptor;
                    w = d.width; h = d.height; mips = d.Internal_CalcTargetNumMips(); fmt = Uint32(d.format);
                }
                if (w > 4 || h > 4) continue;
                // First 4 setups per (index, slot), so one shared default does not use up the budget.
                static std::atomic<unsigned char> seen[65536][5];
                if (seen[ids[i]][i % 5].load(std::memory_order_relaxed) >= 4) continue;
                seen[ids[i]][i % 5].fetch_add(1, std::memory_order_relaxed);
                if (lines.fetch_add(1) >= 96) break;
                std::fprintf(stderr, "CP2077 whitewall: tiny multilayer layer texture: setup=%s layer=%u slot=%s index=%u texId=%u engineFormat=%u size=%ux%u mips=%u\n",
                    name ? name : "?", i / 5, slotNames[i % 5], unsigned(ids[i]), unsigned(id), fmt, w, h, mips);
            }
        }
    }
    if (!key || !CpMlAuditEnabled()) return; // [skperf] round 5: no registry/audit unless mlRepool=1 or perfprobe=1
    {
        std::lock_guard<std::mutex> lock(cpMlAuditMutex);
        auto& record = cpMlSetups[key];
        record.name = name ? name : "";
        record.ids.assign(ids, ids + count);
        ++cpMlRegistered;
    }
    static std::atomic<bool> armed{false};
    if (!armed.exchange(true)) {
        if (emscripten_is_main_runtime_thread()) CpMlArmAudit();
        else emscripten_async_run_in_main_runtime_thread(EM_FUNC_SIG_V, reinterpret_cast<void*>(&CpMlArmAudit));
    }
}
void MultilayerSetupUnregister(const void* key) {
    std::lock_guard<std::mutex> lock(cpMlAuditMutex);
    if (cpMlSetups.erase(key)) ++cpMlUnregistered;
}
}
#include "gpu-api-texture-transfer-web.inl"
namespace cp2077::web {
WGPUTextureFormat TranslateGpuTextureFormat(std::uint32_t format) {
    const auto engineFormat = static_cast<GpuWrapApi::eTextureFormat>(format);
    if (GpuLacksTier1()) { // [notier1] render-target formats (pipeline precache): RG16 targets are rg16float
        const auto source = TranslateSource(engineFormat).native;
        if (source == WGPUTextureFormat_RG16Unorm || source == WGPUTextureFormat_RG16Snorm) return WGPUTextureFormat_RG16Float;
        if (source == WGPUTextureFormat_RG11B10Ufloat && GpuRendersRG11B10()) return source;
    }
    return Translate(engineFormat).native;
}
bool InitializeGpuInternalTextures() {
    std::lock_guard<std::mutex> lock(internalTextureMutex);
    if (internalTexturesReady) return true;
    GpuApi::InitInternalTextures();
    const auto& state = GpuApi::GetBrowserInternalTextureData();
    for (const auto& ref : state.m_InternalTextures) {
        if (ref.isNull()) { GpuApi::ShutInternalTextures(); return false; }
    }
    internalTexturesReady=true;
    return true;
}
bool RetainGpuTexture(const GpuApi::TextureRef& reference) {
    if (reference.isNull()) return false;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry == textures.end() || entry->second->references >= INT32_MAX) return false;
    ++entry->second->references;
    return true;
}
const GpuApi::TextureDesc* RetainGpuTextureDesc(const GpuApi::TextureRef& reference) { // [skperf] round 18 texRetain
    if (reference.isNull()) return nullptr;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry == textures.end() || entry->second->references >= INT32_MAX) return nullptr;
    ++entry->second->references;
    return &entry->second->descriptor;
}
bool AcquireGpuTextureForCopy(const GpuApi::TextureRef& reference,WGPUTexture& texture) {
    texture=nullptr;
    if(reference.isNull())return false;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry=textures.find(reference.ID());
    if(entry==textures.end() || !entry->second->texture || entry->second->references>=INT32_MAX)return false;
    ++entry->second->references;texture=entry->second->texture;return true;
}
bool AcquireStorageGpuTextureView(const GpuApi::TextureRef& reference, std::uint32_t mip, WGPUTextureView& view) {
    view = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto entry = textures.find(reference.ID());
        if (entry == textures.end() || entry->second->references == INT32_MAX) return false;
        const auto& original = entry->second->descriptor;
        if (!original.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess) ||
            mip >= original.Internal_CalcTargetNumMips() || Uint32(original.sampleCount) != 0) return false;
        descriptor.format = StorageFormat(original.format);
        descriptor.baseMipLevel = mip; descriptor.mipLevelCount = 1;
        descriptor.baseArrayLayer = 0;
        descriptor.arrayLayerCount = original.type == GpuWrapApi::TEXTYPE_3D ? 1 : original.CalcTargetSlices();
        descriptor.usage = WGPUTextureUsage_StorageBinding;
        switch (original.type) {
        case GpuWrapApi::TEXTYPE_2D: descriptor.dimension = WGPUTextureViewDimension_2D; break;
        case GpuWrapApi::TEXTYPE_ARRAY:
        case GpuWrapApi::TEXTYPE_CUBE: descriptor.dimension = WGPUTextureViewDimension_2DArray; break;
        case GpuWrapApi::TEXTYPE_3D: descriptor.dimension = WGPUTextureViewDimension_3D; break;
        default: return false;
        }
        ++entry->second->references; texture = entry->second->texture;
    }
    view = CreateGpuTextureView(texture, descriptor);
    if (!view) GpuApi::Release(reference);
    return view != nullptr;
}
bool GetGpuStorageTextureFormat(const GpuApi::TextureRef& reference, WGPUTextureFormat& format) {
    format = WGPUTextureFormat_Undefined;
    std::lock_guard<std::mutex> lock(textureMutex);
    auto entry = textures.find(reference.ID());
    if (entry == textures.end() || entry->second->references == INT32_MAX) return false;
    format = StorageFormat(entry->second->descriptor.format);
    if (GpuLacksTier1() && format != WGPUTextureFormat_Undefined) format = Tier1CoreFormat(format, WGPUTextureUsage_StorageBinding); // [notier1]
    return format != WGPUTextureFormat_Undefined;
}
bool AcquireSampledGpuTextureView(const GpuApi::TextureRef& reference, WGPUTextureView& view, bool stencil,
                                 std::uint32_t mip, bool mipGeneration, std::uint32_t slice) {
    view = nullptr;
    WGPUTexture texture = nullptr;
    WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto entry = textures.find(reference.ID());
        if (entry == textures.end() || entry->second->references == INT32_MAX ||
            !(entry->second->descriptor.HasUsage(GpuApi::TEXUSAGE_Samplable)||
              entry->second->descriptor.HasUsage(GpuApi::TEXUSAGE_Bindless))) return false;
        const auto& original = entry->second->descriptor;
        if (stencil && !HasStencilFormat(original.format)) return false;
        descriptor.format = TranslateResource(original).native;
        descriptor.mipLevelCount = original.Internal_CalcTargetNumMips();
        descriptor.arrayLayerCount = original.type == GpuWrapApi::TEXTYPE_3D ? 1 : original.CalcTargetSlices();
        descriptor.aspect = stencil ? WGPUTextureAspect_StencilOnly : HasDepthPlane(original)
            ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
        if (stencil) descriptor.format = WGPUTextureFormat_Stencil8;
        else if (original.format == GpuWrapApi::TEXFMT_D24S8) descriptor.format = WGPUTextureFormat_Depth24Plus;
        else if (original.format == GpuWrapApi::TEXFMT_D32FS8) descriptor.format = WGPUTextureFormat_Depth32Float;
        descriptor.usage = WGPUTextureUsage_TextureBinding;
        switch (original.type) {
        case GpuWrapApi::TEXTYPE_2D: descriptor.dimension = WGPUTextureViewDimension_2D; break;
        case GpuWrapApi::TEXTYPE_ARRAY: descriptor.dimension = WGPUTextureViewDimension_2DArray; break;
        case GpuWrapApi::TEXTYPE_CUBE: descriptor.dimension = original.depthOrSliceNum == 1
            ? WGPUTextureViewDimension_Cube : WGPUTextureViewDimension_CubeArray; break;
        case GpuWrapApi::TEXTYPE_3D: descriptor.dimension = WGPUTextureViewDimension_3D; break;
        default: return false;
        }
        if (mip != UINT32_MAX) {
            if (stencil || mip >= original.Internal_CalcTargetNumMips()) return false;
            if (mipGeneration) {
                // Match the original on-demand mip-generation SRVs: cube faces
                // are exposed as a 2D array, with one selected face or all faces.
                if (original.type == GpuWrapApi::TEXTYPE_3D) return false;
                if (slice != UINT32_MAX && slice >= original.CalcTargetSlices()) return false;
                if (original.type == GpuWrapApi::TEXTYPE_2D && slice != UINT32_MAX && slice != 0) return false;
                if (original.type == GpuWrapApi::TEXTYPE_CUBE || original.type == GpuWrapApi::TEXTYPE_ARRAY)
                    descriptor.dimension = WGPUTextureViewDimension_2DArray;
                descriptor.baseArrayLayer = slice == UINT32_MAX ? 0 : slice;
                descriptor.arrayLayerCount = slice == UINT32_MAX ? original.CalcTargetSlices() : 1;
                descriptor.baseMipLevel = mip; descriptor.mipLevelCount = 1;
            } else {
                const bool perMip = (original.type == GpuWrapApi::TEXTYPE_2D &&
                    (original.HasUsage(GpuApi::TEXUSAGE_Tex2DSamplablePerMipLevel) || original.HasUsage(GpuApi::TEXUSAGE_Tex2DSamplableMipRanges))) ||
                    (original.type == GpuWrapApi::TEXTYPE_3D && original.HasUsage(GpuApi::TEXUSAGE_Tex3DSamplablePerMipLevel));
                if (perMip) {
                    if (original.type == GpuWrapApi::TEXTYPE_2D && original.CalcTargetSlices() != 1) return false;
                    descriptor.baseMipLevel = mip; descriptor.mipLevelCount = 1;
                } else if (mip != 0) return false; // Original fallback is the full default SRV at mip zero.
            }
        }
        ++entry->second->references; texture = entry->second->texture;
    }
    // [cmdlock] Per-worker direct-mapped front for the sampled-view hit path (same rules as the indirect front in
    // AcquireIndirectGpuTextureView): texture ids are never reused and a live id's descriptor never changes, so
    // (id, stencil, mip, mipGeneration, slice) always maps to the same view descriptor. The front keeps its own
    // view reference (no handle reuse) and a hit requires the view to still be in the shared view cache (a texture
    // destroy or device change evicts it there, so the hit fails and we fall through). Skips the label snprintf,
    // the string key build and the std::map lookup per draw. The texture hold taken above is the returned hold.
    // ?viewFront=0 disables (A/B).
    struct SampledFront { Uint32 id=0; WGPUTexture texture=nullptr; std::uint32_t mip=0, slice=0; std::uint8_t flags=0; std::uint64_t generation=0;
        WGPUTextureView view=nullptr; WGPUTextureViewDescriptor recorded=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT; };
    static const int frontEnabled = [] {
        const int v = MAIN_THREAD_EM_ASM_INT({ return new URLSearchParams(location.search).get('viewFront') === '0' ? 0 : 1; });
        std::fprintf(stderr, "CP2077 cmdlock switches: viewFront=%d\n", v); return v; }();
    const bool worker = !emscripten_is_main_runtime_thread();
    SampledFront* front = nullptr;
    const std::uint8_t frontFlags = std::uint8_t((stencil ? 1 : 0) | (mipGeneration ? 2 : 0));
    const auto generation = cp2077::web::GpuDeviceGeneration();
    if (frontEnabled && worker) {
        static thread_local SampledFront fronts[2048];
        std::uint32_t h = reference.ID() * 2654435761u ^ (mip * 40503u) ^ (slice * 9973u) ^ frontFlags;
        front = &fronts[(h ^ (h >> 15)) & 2047];
        if (front->view && front->id == reference.ID() && front->texture == texture && front->mip == mip && front->slice == slice &&
            front->flags == frontFlags && front->generation == generation &&
            cp2077::web::RetainCachedGpuTextureViewOnWorker(front->view, front->recorded)) {
            view = front->view;
            cp2077::web::CountCmdlockEvent(2);
            return true;
        }
    }
    if (worker) cp2077::web::CountCmdlockEvent(3);
    // Keep original view identity in browser validation errors. TextureRef and
    // native view format can differ for depth/stencil and aliasing resources.
    char label[160];
    std::snprintf(label,sizeof(label),"CP2077 SRV ref=%u stencil=%u format=%u dimension=%u mip=%u layer=%u",
        reference.ID(),unsigned(stencil),unsigned(descriptor.format),unsigned(descriptor.dimension),
        descriptor.baseMipLevel,descriptor.baseArrayLayer);
    descriptor.label={label,std::strlen(label)};
    view = CreateGpuTextureView(texture, descriptor);
    if (!view) { GpuApi::Release(reference); return false; }
    if (front) {
        // Only views owned by the shared cache enter the front (Retain fails for uncached ones).
        WGPUTextureViewDescriptor recorded = descriptor; recorded.label = {nullptr, 0};
        if (cp2077::web::RetainCachedGpuTextureViewOnWorker(view, recorded)) {
            if (front->view) cp2077::web::ReleaseGpuTextureView(front->view);
            front->view = view; front->id = reference.ID(); front->texture = texture; front->mip = mip; front->slice = slice;
            front->flags = frontFlags; front->generation = generation; front->recorded = recorded;
        }
    }
    return true;
}
bool AcquireIndirectGpuTextureView(std::uint16_t index,GpuApi::TextureRef& reference,WGPUTextureView& view) {
    reference=GpuApi::TextureRef::Null();view=nullptr;
    if(index==UINT16_MAX)return false;
    Uint32 id=0;
    {
        std::lock_guard<std::mutex> guard(textureMutex);
        id=indirectTextures[index];auto found=textures.find(id);
        if(!id||found==textures.end()||found->second->references==INT32_MAX)return false;
        ++found->second->references;reference=GpuApi::TextureRef{id};
    }
    // Per-worker front (perf7): (slot, texture id) -> the default SRV acquired last time. Texture ids are never
    // reused and a texture's descriptor never changes, so the view is the one AcquireSampledGpuTextureView would
    // return again; the slot keeps its own reference (no handle reuse) and a hit requires the view to still be in
    // the shared view cache (so the caller's release stays worker-side). Skips the label snprintf, the string-keyed
    // cache lookup and two of the three textureMutex round trips. The lookup hold becomes the returned hold.
    struct Front { Uint32 id=0; std::uint16_t index=0; std::uint64_t generation=0; WGPUTextureView view=nullptr; WGPUTextureViewDescriptor recorded=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT; };
    static thread_local Front fronts[1024];
    static thread_local unsigned hits=0,misses=0;
    auto& front=fronts[index&1023];
    const auto generation=cp2077::web::GpuDeviceGeneration();
    if(front.view&&front.id==id&&front.index==index&&front.generation==generation&&
       cp2077::web::RetainCachedGpuTextureViewOnWorker(front.view,front.recorded)){
        view=front.view;
        const unsigned h=++hits;
        if(h>=1024&&!(h&(h-1)))
            std::fprintf(stderr,"CP2077 indirect SRV front probe: hits=%u misses=%u\n",h,misses);
        return true;
    }
    ++misses;
    const bool acquired=AcquireSampledGpuTextureView(reference,view);
    GpuApi::Release(reference); // Drop lookup hold; successful view acquisition owns its own hold.
    if(!acquired){reference=GpuApi::TextureRef::Null();return false;}
    if(!emscripten_is_main_runtime_thread()){
        if(front.view){cp2077::web::ReleaseGpuTextureView(front.view);front.view=nullptr;}
        WGPUTextureViewDescriptor recorded=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
        if(cp2077::web::GetGpuTextureViewInfo(view,recorded.format,recorded.dimension)&&
           cp2077::web::GetGpuTextureViewRange(view,recorded.baseMipLevel,recorded.mipLevelCount,recorded.baseArrayLayer,recorded.arrayLayerCount)&&
           cp2077::web::RetainCachedGpuTextureViewOnWorker(view,recorded)){ // the front's own reference; only cached views
            front.view=view;front.id=id;front.index=index;front.generation=generation;front.recorded=recorded;
        }
    }
    return true;
}
bool AcquireRenderGpuTextureView(const GpuApi::TextureRef& reference, std::uint32_t mip,
    std::uint32_t layer, bool depth, WGPUTextureView& view, WGPUTextureFormat* format, bool nativeBaseFormat) {
    view = nullptr;
    if (format) *format = WGPUTextureFormat_Undefined;
    WGPUTexture texture = nullptr;
    WGPUTextureViewDescriptor descriptor = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    {
        std::lock_guard<std::mutex> lock(textureMutex);
        auto entry = textures.find(reference.ID());
        if (entry == textures.end() || entry->second->references == INT32_MAX) return false;
        const auto& original = entry->second->descriptor;
        if (original.type == GpuWrapApi::TEXTYPE_3D ||
            mip >= original.Internal_CalcTargetNumMips() || layer >= original.CalcTargetSlices()) return false;
        if (depth) {
            if (!original.HasUsage(GpuApi::TEXUSAGE_DepthStencil) ||
                !IsDepthFormat(original.format)) return false;
        } else if (!original.HasUsage(GpuApi::TEXUSAGE_RenderTarget) ||
                   HasDepthPlane(original)) return false;
        descriptor.format = nativeBaseFormat
            ? (original.HasUsage(GpuApi::TEXUSAGE_UnorderedAccess) ? StorageFormat(original.format) : TranslateResource(original).native)
            : entry->second->renderFormat != WGPUTextureFormat_Undefined
                ? entry->second->renderFormat : TranslateResource(original).native;
        descriptor.dimension = WGPUTextureViewDimension_2D;
        descriptor.baseMipLevel = mip; descriptor.mipLevelCount = 1;
        descriptor.baseArrayLayer = layer; descriptor.arrayLayerCount = 1;
        descriptor.usage = WGPUTextureUsage_RenderAttachment;
        descriptor.aspect = WGPUTextureAspect_All;
        ++entry->second->references; texture = entry->second->texture;
    }
    view = CreateGpuTextureView(texture, descriptor);
    if (!view) GpuApi::Release(reference);
    else if (format) { // [notier1] the view's actual format (remapped textures), which pipelines must target
        WGPUTextureFormat actual; WGPUTextureViewDimension dimension;
        *format = GetGpuTextureViewInfo(view, actual, dimension) ? actual : descriptor.format;
    }
    return view != nullptr;
}
void ShutdownGpuTextures() {
    ShutdownGpuTextureCompressor();
    {
        std::lock_guard<std::mutex> lock(internalTextureMutex);
        GpuApi::ShutInternalTextures(); internalTexturesReady=false;
    }
    ShutdownBrowserTextureGrabs();
    ShutdownBrowserTextureLocks();
    std::unordered_map<Uint32, std::unique_ptr<TextureEntry>> retired;
    { std::lock_guard<std::mutex> lock(textureMutex); retired.swap(textures);
      indirectTextures.fill(0);freeIndirectTextures.clear();nextIndirectTexture=0; }
    for (const auto& item : retired)
        if (item.second->backing.isNull()) DestroyGpuTexture(item.second->texture);
}
}
#include "gpu-api-swapchain-web.inl"
#include "gpu-api-texture-copy-web.inl"
