"""Convert original static-cache compute shaders (exact DXIL) to WGSL for the WebGPU runtime.

Names map to cache entries without guessing: FNV-1a32(VariableName) keys the static cache meta entry whose
compute slot holds the 64-bit program hash used in build/shaders/native-static-shaders.json.
DXIL -> dxil-spirv -> register remap (CB b#=#, SRV t#=64+#, sampler s#=256+#, UAV u#=288+#, set 0) -> naga WGSL.
The remap is cross-checked against the DXIL resource reflection. Output rows feed generate-web-shader-package.py
and generate-uniform-layouts.py via build/shaders/static-compute-conversion.json; the runtime matches the
original DXIL bytes exactly (no name or size substitution).

Usage: convert-static-compute.py m_computeSkinning_MeshSkinned [...]   |   --from-log full.log
"""
import fnmatch, hashlib, json, re, struct, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CACHE = Path(r'D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/engine/staticshader.cache')
SPIRV = ROOT / 'tools/dxil-spirv/build-win/Release/dxil-spirv.exe'
NAGA = ROOT / 'tools/naga-30.0.1/bin/naga.exe'
# naga with semantics-preserving SPIR-V frontend fixes (tools/naga-scope-fixed/scope-fix-provenance.json: OpUndef emit
# range, scoped switch cases, coordinate typing; validator unchanged). Used only when stock naga hits its scope bug.
NAGA_SCOPE_FIXED = ROOT / 'tools/naga-scope-fixed/target/release/naga.exe'
DXC = ROOT / 'tools/dxc-1.9.2609/bin/x64/dxc.exe'
OUT = ROOT / 'build/shaders'
WORK = ROOT / 'build/static-compute'
MANIFEST = OUT / 'static-compute-conversion.json'


def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def name_hash32(name):
    h = 2166136261
    for b in name.encode('ascii'): h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def parse(data):
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    assert words[0] == 0x07230203
    ops = []; pos = 5
    while pos < len(words):
        count, op = words[pos] >> 16, words[pos] & 65535
        ops.append([op, list(words[pos + 1:pos + count])]); pos += count
    return words[:5], ops


def emit(header, ops):
    words = list(header)
    for op, a in ops: words += [(len(a) + 1) << 16 | op, *a]
    return struct.pack('<%dI' % len(words), *words)


# Storage image formats the original engine creates for each compute UAV (dxil-spirv emits Unknown).
# Evidence: staticShaders.xml defines (PRECISION=32, CHANNELS=2 -> R32G32_Float) and renderHiZ.cpp UAV targets.
SPV_IMAGE_FORMAT = {'rgba32float': 1, 'rgba16float': 2, 'r32float': 3, 'rgba8unorm': 4, 'rgba8snorm': 5,
                    'rg32float': 6, 'rg11b10ufloat': 8, 'r16float': 9, 'r32sint': 24, 'rgba32uint': 30,
                    'rgba16uint': 31, 'r32uint': 33, 'rgb10a2unorm': 11, 'rg32uint': 35, 'r16uint': 38, 'r8uint': 39,
                    # spirv.h ImageFormat Rg16f=7, Rg16=12, Rg8=13, R16=14, R8=15 (PLAN P0.1)
                    'rg16float': 7, 'rg16unorm': 12, 'rg8unorm': 13, 'r16unorm': 14, 'r8unorm': 15}
# Storage formats outside core WebGPU: the package marks these rows requiresTier1Storage (runtime gate).
TIER1_STORAGE_FORMATS = {'r8unorm', 'r8snorm', 'r8uint', 'r8sint', 'rg8unorm', 'rg8snorm', 'rg8uint', 'rg8sint', 'r16unorm',
                         'r16snorm', 'r16uint', 'r16sint', 'r16float', 'rg16unorm', 'rg16snorm', 'rg16uint', 'rg16sint',
                         'rg16float', 'rgba16unorm', 'rgba16snorm', 'rgb10a2uint', 'rgb10a2unorm', 'rg11b10ufloat'}
UAV_FORMATS = {
    'm_downsampleDepth': {u: 'rg32float' for u in range(1, 7)},
    'm_downsampleDepthForceMax': {u: 'r32float' for u in range(1, 7)},
    'm_hierarchicalZGrab': {0: 'rg32float', 1: 'r32float'},
    # renderNode_Lighting.cpp: u0/u1 = lightBufferDiffuse/Specular, allocated with GetRegularPrecisionColorTargetDesc
    # (renderGraphNodes.cpp:478) = R11G11B10_Float unless UseHighPrecisionRenderTargets (default false).
    'm_shaderLightsComputeGlobalLocal*_Clustered_*': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat'},
    # renderColorGradingLUT.cpp:712-740 picks *_R11G11B10 when the LUT is R11G11B10_Float, else RGBA16F
    # (staticShaders.xml RenderTargets ColorFormat=TEXFMT_R16G16B16A16_Float).
    'm_LUTGenerate*_R11G11B10': {0: 'rg11b10ufloat'},
    'm_LUTGenerate*': {0: 'rgba16float'},
    # renderNode_Histogram.cpp: exposureTemp/GlobalHistogram = R16G16B16A16_Uint, exposureSceneLum = R16_Float.
    'm_histogramGather': {0: 'rgba16uint'},
    'm_histogramSumUp': {0: 'rgba16uint'},
    'm_histogramDownscale': {0: 'r16float'},
    # --- Round 5: engine allocation of the texture bound at every dispatch site (paths relative to
    # dev/src/common/renderer/src), mapped through the port's StorageFormat() (gpu-api-texture-web.cpp:130:
    # R8G8B8A8_Unorm_SRGB -> rgba8unorm). Only fixed formats; shaders whose UAV is the regular-precision colour target
    # (R11G11B10 or RGBA16F by UseHighPrecisionRenderTargets/screenshot: CAS, SSS/frosted-glass blur, simpleTemporal,
    # DOF near/far, combinedBlursUpsample) or a caller-chosen format (m_downsample*, m_simpleCopyCS*) are NOT listed.
    # renderNode_Prepare.cpp:376 rtNormals <- renderGraphNodes.cpp:446 / :787 R8G8B8A8_Unorm.
    'm_flattenNormals': {0: 'rgba8unorm'},
    # renderNode_Lighting.cpp:1170 descMaterialClass R8_Uint, bound :1220-1234.
    'm_classifyMaterials': {0: 'r8uint'},
    # renderNode_Lighting.cpp:1280 lightChannelMip R16_Uint, bound :1321.
    'm_lightChannelVolumesMip': {0: 'r16uint'},
    # particleDPLTexture.cpp:386 dplTexture R11G11B10_Float, bound :407.
    'm_countLightsPerPixel': {0: 'rg11b10ufloat'},
    'm_decoupledParticleLighting': {0: 'rg11b10ufloat'},
    # clusteredDeferred.cpp:897-936 cascade arrays R32_Uint; DispatchClusterGrid binds u4/u5 at :119.
    'm_clusteredInitCull*': {4: 'r32uint', 5: 'r32uint'},
    'm_clusteredBinPack*': {4: 'r32uint', 5: 'r32uint'},
    # renderGraphNodes.cpp:497-502 hair_depths R32_Uint x4 slices, bound renderNode_Hair.cpp:192.
    'm_hair_ClearDepths_CopySceneDepth': {0: 'r32uint'},
    # renderNode_Composition.cpp:158-168 blurredComposition (GetCompositionDesc R8G8B8A8_Unorm_SRGB), mip UAVs :194/:200/:238.
    'm_GaussianBlurSeparable_*': {0: 'rgba8unorm'},
    # renderNode_Composition.cpp:372-377 filmGrain R10G10B10A2_Unorm on PC, bound :405.
    'm_filmGrainGen': {0: 'rgb10a2unorm'},
    # renderNode_CombinedBlurs.cpp:143-145 motionBlurResult / weaponPlaneBlurResult R16G16B16A16_Float.
    'm_postfx_combinedBlursMBCS_*': {0: 'rgba16float'},
    'm_postfx_combinedBlursDownsampleNoMBCS': {0: 'rgba16float'},
    'm_postfx_combinedBlursWPBCS': {0: 'rgba16float'},
    # renderNode_DepthOfField.cpp:667-669 intermediateRe/Im GetHighPrecisionColorTargetFormat (R16G16B16A16_Float), :701-702.
    'm_postfxDofSeparableHorizontalPassCustomCOC': {0: 'rgba16float', 1: 'rgba16float'},
    # renderNode_VolumetricClouds.cpp:112-133 rtClouds high precision RGBA16F (u0), rtDownsampledDepthBuffer R32_Float (u2);
    # cloudsShadow renderGraphNodes.cpp:569-578 R8G8B8A8_Unorm.
    'm_volumetricClouds_integrate*': {0: 'rgba16float', 2: 'r32float'},
    'm_volumetricClouds_shadowmap*': {0: 'rgba8unorm'},
    # renderVolumetricFogManager.cpp:166-180 volume descriptor R16G16B16A16_Float (lighting u1 :378/:428, scattering u2 :407/:459).
    'm_volumetricFogLighting*': {1: 'rgba16float'},
    'm_volumetricFogScattering*': {2: 'rgba16float'},
    # windImpulseCollector.cpp:39 GetVolumeTextureDesc R16G16B16A16_Float (renderNode_WindImpulseVolumeUpdate.cpp:138/:185).
    'm_windImpulseVolume*': {0: 'rgba16float'},
    # renderGIManager.cpp:1824-1834 m_irradiance R11G11B10_Float array, bound :1961.
    'm_distantIrradiance': {0: 'rg11b10ufloat'},
    # renderGIManager.cpp:347/:370 StartingTetra R32G32_Uint (u0), StartingSlot R8_Uint (u1), bound :1385-1386 / :1405-1406.
    'm_giTetra*Volume': {0: 'rg32uint', 1: 'r8uint'},
    # --- Round 6 (PLAN F2, build/research/convert/formats.json fix=F2): fixed engine allocation at every dispatch
    # site (paths relative to dev/src/common/renderer/src unless noted). Exact names; formats keyed by DXIL u#.
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOBlank': {0: 'rgba8unorm'},
    # depth R32F, stencil R8_Uint (:347), velocity R16G16F, normals RGBA8 (:473-476) [high]
    'm_coneAODownsampleCS': {0: 'r32float', 1: 'r8uint', 2: 'rg16float', 3: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOFilterCS': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOFilterHalfResCS': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOFilterTemporalHalfRes': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOHalfResCS_Quality0': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOHalfResCS_Quality1': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOHalfResCS_Quality2': {0: 'rgba8unorm'},
    # cone AO targets R8G8B8A8_Unorm (renderPostFxConeAO.cpp:197,214,311,342,352) [high]
    'm_coneAOUpsampleCS': {0: 'rgba8unorm'},
    # only sRGB textures select it; StorageFormat() maps R8G8B8A8_Unorm_SRGB -> rgba8unorm (gpu-api-texture-web.cpp:130); shader encodes sRGB itself [high]
    'm_downsample_SRGB': {1: 'rgba8unorm', 2: 'rgba8unorm', 3: 'rgba8unorm', 4: 'rgba8unorm', 5: 'rgba8unorm', 6: 'rgba8unorm'},
    # only sRGB textures select it; StorageFormat() maps R8G8B8A8_Unorm_SRGB -> rgba8unorm (gpu-api-texture-web.cpp:130); shader encodes sRGB itself [high]
    'm_downsample_SRGB_Array': {1: 'rgba8unorm', 2: 'rgba8unorm', 3: 'rgba8unorm', 4: 'rgba8unorm', 5: 'rgba8unorm', 6: 'rgba8unorm'},
    # only sRGB textures select it; StorageFormat() maps R8G8B8A8_Unorm_SRGB -> rgba8unorm (gpu-api-texture-web.cpp:130); shader encodes sRGB itself [high]
    'm_downsample_SquarePow2_SRGB': {1: 'rgba8unorm', 2: 'rgba8unorm', 3: 'rgba8unorm', 4: 'rgba8unorm', 5: 'rgba8unorm', 6: 'rgba8unorm'},
    # only sRGB textures select it; StorageFormat() maps R8G8B8A8_Unorm_SRGB -> rgba8unorm (gpu-api-texture-web.cpp:130); shader encodes sRGB itself [high]
    'm_downsample_SquarePow2_SRGB_Array': {1: 'rgba8unorm', 2: 'rgba8unorm', 3: 'rgba8unorm', 4: 'rgba8unorm', 5: 'rgba8unorm', 6: 'rgba8unorm'},
    # exposureDebug R32_Float (renderNode_Histogram.cpp:410,419) [high]
    'm_histogramDebug': {0: 'r32float'},
    # u32 scalar UAV; m_modifiedTexture format not fully traced (renderProxyMorphTargetMesh.cpp:232-240) [medium]
    'm_morphTargetTextureSet': {0: 'r32uint'},
    # m_colorFeedback mips R11G11B10 (renderNode_ScreenSpaceReflections.cpp:72) [high]
    'm_postfxSSRDownsample': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat', 3: 'rg11b10ufloat'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_CINEMATIC': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_CINEMATIC_EnforceUberQuality': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # rtDownSampledDepth R16_Unorm (:343, bound :375); needs P0 r16unorm=14 [high]
    'm_postfxSSR_DownsampleDepth': {0: 'r16unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_HIGH': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_HIGH_EnforceUberQuality': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_LOW': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_LOW_EnforceUberQuality': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_MEDIUM': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_MEDIUM_EnforceUberQuality': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # planar result R11G11B10 (:89) / fade R8_Unorm (:96), bound :537-538 [high]
    'm_postfxSSR_Planar_Filter': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # rtSSROut_temp R11G11B10 / rtSSRFade_temp R8_Unorm (renderNode_ScreenSpaceReflections.cpp:449,452,522-523) [high]
    'm_postfxSSR_Planar_LOW': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_ULTRA': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # SSR_Result R11G11B10 + SSR_Fade R8_Unorm (renderGraphNodes.cpp:521-524). Batch guesses r32float/rg11b10/rgba16float for u1 are wrong [high]
    'm_postfxSSR_ULTRA_EnforceUberQuality': {0: 'rg11b10ufloat', 1: 'r8unorm'},
    # rtSorted pixelSortedBuf16 R11G11B10 (gameplayFx_CPCyberspacePixelsort.cpp:117,126, bound :263); fmt17 rgba16float guess wrong [high]
    'm_postfx_CPCyberspacePixelsort': {3: 'rg11b10ufloat'},
    # gmpl_visionmodeFinalEffect R8G8B8A8_Unorm (:599, bound :627); fmt18 rgba16float guess wrong [high]
    'm_postfx_CPVisionMode_ApplyFullscreen': {0: 'rgba8unorm'},
    # rtPixelization R8_Unorm mips (:792, bound :836/:868/:884) [high]
    'm_postfx_CPVisionMode_DownsampleResolution': {0: 'r8unorm'},
    # FOCUSMODE_lumaEdge uses sceneLumDesc R16_Float (:476, bound :492); fmt00 r32float guess wrong [high]
    'm_postfx_CPVisionMode_EdgeDetectR': {0: 'r16float'},
    # rtResolution R8_Uint (:414, bound :904) [high]
    'm_postfx_CPVisionMode_FinalizeResolution': {0: 'r8uint'},
    # effectResults R8G8B8A8_Unorm (:421), currHistory (:90), bound :533-534 [high]
    'm_postfx_CPVisionMode_Highlights': {0: 'rgba8unorm', 1: 'rgba8unorm'},
    # sceneLumDesc R16_Float (:437, bound :462) [high]
    'm_postfx_CPVisionMode_SceneToLuminance': {0: 'r16float'},
    # rtPixelization R8_Unorm mips (:792, bound :836/:868/:884) [high]
    'm_postfx_CPVisionMode_UpdateResolution': {0: 'r8unorm'},
    # rtPixelization R8_Unorm mips (:792, bound :836/:868/:884) [high]
    'm_postfx_CPVisionMode_UpsampleResolution': {0: 'r8unorm'},
    # downsampleOutput RGBA16F, intensities R16G16F; split_images path (shared OpTypeImage) [high]
    'm_postfx_combinedBlursDownsampleCS_Quality0': {0: 'rgba16float', 1: 'rg16float'},
    # same pass as Quality0 [medium]
    'm_postfx_combinedBlursDownsampleCS_Quality1': {0: 'rgba16float', 1: 'rg16float'},
    # probe atlas Map(eRGB)=R11G11B10 (renderReflectionProbeManager.cpp:511-515) [medium-high]
    'm_reflectionProbePrefilter': {1: 'rg11b10ufloat'},
    # intermediate Map(eRGB) R11G11B10 (:515, :3690-3708) [high]
    'm_reflectionProbePrefilterConvolution': {1: 'rg11b10ufloat'},
    # R16G16_Float ping-pong (:101,:157) [high]
    'm_shaderFFTGPU_Butterfly': {1: 'rg16float'},
    # ForceFormatAndSize R16G16B16A16_Float (:56), bound u1 :186 [medium-high]
    'm_shaderFFTGPU_Finalize': {1: 'rgba16float'},
    # ping-pong m_tex R16G16_Float (:101), UAV1 :145-147 [high]
    'm_shaderFFTGPU_Phillips_Animate_Butterfly_Transpose': {1: 'rg16float'},
    # m_skyLUT R16G16B16A16_Float array (renderReflectionProbeManager.cpp:855-866); no SHADER_NAME dispatch found [medium]
    'm_skyGenerateLUT': {0: 'rgba16float'},
    # atlases R8G8B8A8_Unorm (renderSurfaceGenerator_BakedMultilayer.cpp:169,172); debug [high]
    'm_surfaceCache_DebugClearFreePages': {0: 'rgba8unorm', 1: 'rgba8unorm'},
    # --- Round 6 F3 interim (PLAN F3RP): the regular-precision colour target (R/renderNodeImplContext.cpp:28) is
    # R11G11B10 unless UseHighPrecisionRenderTargets or FP_ScreenShot (then RGBA16F). The browser build runs
    # -qualityLevel=Low (native-browser.html:77 -> GetRenderSettings_LOW, UseHighPrecisionRenderTargets=false), so only
    # the R11G11B10 variant is baked; a frame whose bound texture differs is skipped at dispatch by the storage format
    # guard in gpu-api-command-web.cpp ("storage texture format mismatch"). reflectionProbeRelight(_Fill) are listed in
    # the Round 9 U-probes block below: their u0 follows the probe sky-channel flag, not the preset.
    'm_clusteredDebugLightsCountPreview': {3: 'rg11b10ufloat'},
    'm_clusteredDebugLightsWithContactShadowCountPreview': {3: 'rg11b10ufloat'},
    'm_postfxCas': {0: 'rg11b10ufloat'},
    'm_postfxCas_ClearUAV1': {0: 'rg11b10ufloat', 1: 'rgba8unorm'},
    'm_postfxCas_NoScaling': {0: 'rg11b10ufloat'},
    'm_postfxCas_NoScaling_ClearUAV1': {0: 'rg11b10ufloat', 1: 'rgba8unorm'},
    'm_postfxDofSeparableHorizontalPass_radius4_farplane': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat'},
    'm_postfxDofSeparableHorizontalPass_radius4_nearplane': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat'},
    'm_postfxDofSeparableVerticalCombinePassCustomCOC': {0: 'rg11b10ufloat'},
    'm_postfxDofSeparableVerticalCombinePass_radius4_farplane': {0: 'rg11b10ufloat'},
    'm_postfxDofSeparableVerticalCombinePass_radius4_nearplane': {0: 'rg11b10ufloat'},
    'm_postfxDofSeparable_downsample': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg16float'},
    'm_postfx_combinedBlursUpsampleCS': {0: 'rg11b10ufloat'},
    'm_separateBlurHorizontal_15': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_separateBlurHorizontal_5': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_separateBlurHorizontal_9': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_separateBlurVertical_15': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_separateBlurVertical_5': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_separateBlurVertical_9': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat', 2: 'rg11b10ufloat'},
    'm_shaderCascadesShadowsRenderedMeshes': {0: 'rg11b10ufloat'},
    'm_shaderLightsContactShadowOverflow': {0: 'rg11b10ufloat', 1: 'rg11b10ufloat'},
    'm_shaderLightsOverdraw': {0: 'rg11b10ufloat'},
    'm_shaderLocalShadowsRenderedMeshes': {0: 'rg11b10ufloat'},
    'm_shaderVSMDebugRangesDynamicsOnly': {3: 'rg11b10ufloat'},
    'm_shaderVSMDebugRangesStaticsOnly': {3: 'rg11b10ufloat'},
    'm_shaderVSMDebugVariance': {3: 'rg11b10ufloat'},
    'm_simpleTemporal': {0: 'rg11b10ufloat'},
    'm_simpleTemporal_Cheap': {0: 'rg11b10ufloat'},
    # renderGraphNodes.cpp:2243 distortedColor = RTTempAlloc(..., rtColor) = the regular-precision colour target
    # (R11G11B10 at -qualityLevel=Low, same F3 interim as simpleTemporal); u0 of postfx_distortion.fx. Written only
    # (lower_storage_rmw_to_local drops the APPLY_BLUR read-back), so write-only rg11b10ufloat storage (tier1).
    'm_distortion_*': {0: 'rg11b10ufloat'},
    # --- Round 9 U-probes (PLAN F6). CEnvProbeRelighter::PrepareLiTexture (renderReflectionProbeManager.cpp:899-921) picks
    # RGBA16F for m_litCubemap only under #ifdef USE_RAY_TRACING && allowRaytracing; USE_RAY_TRACING is defined only for
    # RED_PLATFORM_WINPC (gpuApiInterface.h:31-33) and the web build is RED_PLATFORM_WEB (overlay architecture.h:51, WINPC
    # needs _WIN32/_WIN64 :92), so the lit cube is always R11G11B10 (TEXTYPE_CUBE 128, 6 mips, written as a 2d_array mip-0
    # UAV :1065). u1 m_luminance R16_Float 16x16x6 array (:819-829, bound :1053). Both store-only in reflectionProbe.fx
    # PROBE_RELIT (:654/:723) [high].
    'm_reflectionProbeRelight': {0: 'rg11b10ufloat', 1: 'r16float'},
    'm_reflectionProbeRelight_Fill': {0: 'rg11b10ufloat', 1: 'r16float'},
    # GenerateMipmaps(m_litCubemap) (:1127) and of the sky cube (:1911, R11G11B10 TEXTYPE_CUBE 128, :833-843) resolve to this
    # variant (overlay renderInterface.cpp:223-290: square pow2, not sRGB/1ch/2ch, CUBE -> _Array). Probe SH samples mip 3
    # and the convolution samples brdf mip levels, so the U-probes chain needs it. Caller-chosen format (PLAN F3CC): only the
    # R11G11B10 bake is published; other-format callers are skipped by the dispatch storage-format guard. downsample.fx
    # outMip[6] (u1..u6) is store-only (:54-61) [high for the probe cubes].
    'm_downsample_SquarePow2_Array': {u: 'rg11b10ufloat' for u in range(1, 7)},
}

# Storage textures whose only reads are the invocation re-reading the texel it just stored (HLSL RW read-modify-write
# on its own pixel, e.g. postfx_distortion.fx APPLY_BLUR `lerp( rtOutColor[pixelId], ... )`). WebGPU has no read_write
# storage for these formats, so the read becomes the stored value and the texture becomes write-only.
STORAGE_RMW_TO_LOCAL = {'m_distortion_00', 'm_distortion_01', 'm_distortion_10', 'm_distortion_11'}


def lower_storage_rmw_to_local(text, name):
    """WGSL post-pass for STORAGE_RMW_TO_LOCAL rows: textureLoad(V, C) after a top-level textureStore(V, C, X) with the
    identical coordinate expression C reads X back (same invocation, same texel; no other invocation writes C), so it
    becomes a let. Every read_write storage texture of the row must end up with no loads (then access = write)."""
    if name not in STORAGE_RMW_TO_LOCAL: return text
    decls = re.findall(r'var (\w+): texture_storage_2d<(\w+),read_write>;', text)
    if not decls: raise ValueError('rmw lowering: no read_write storage texture in %s' % name)
    for var, fmt in decls:
        loads = re.findall(r'textureLoad\(%s, ([^;]*?)\);' % re.escape(var), text)
        if len(loads) > 1: raise ValueError('rmw lowering: %d loads of %s in %s' % (len(loads), var, name))
        if loads:
            coord = loads[0]
            stores = list(re.finditer(r'^    textureStore\(%s, (vec2<u32>\([^;]*?\)), (vec4<f32>\([^;]*\))\);$' % re.escape(var), text, re.M))
            load_at = text.index('textureLoad(%s, %s);' % (var, coord))
            before = [m for m in stores if m.start() < load_at]
            # exactly one store before the load, at main_1 top level (4-space indent), with the identical coordinate
            if len(before) != 1 or before[0].group(1) != coord:
                raise ValueError('rmw lowering: store/load coordinates of %s in %s do not match' % (var, name))
            if text.count('fn ') != 2 or text.index('fn main_1()') > before[0].start():
                raise ValueError('rmw lowering: unexpected function layout in %s' % name)
            local = 'cp_rmw_' + var
            store = before[0]
            text = (text[:store.start()] + '    let %s = %s;\n    textureStore(%s, %s, %s);' % (local, store.group(2), var, coord, local)
                    + text[store.end():])
            text, n = re.subn(r'let (\w+) = textureLoad\(%s, %s\);' % (re.escape(var), re.escape(coord)), r'let \1 = %s;' % local, text)
            if n != 1: raise ValueError('rmw lowering: load rewrite failed for %s in %s' % (var, name))
        if 'textureLoad(%s,' % var in text: raise ValueError('rmw lowering: %s still read in %s' % (var, name))
        text = text.replace('var %s: texture_storage_2d<%s,read_write>;' % (var, fmt), 'var %s: texture_storage_2d<%s,write>;' % (var, fmt))
    return text


def uav_formats(name):
    """Exact names first, then the first matching fnmatch pattern in table order."""
    if name in UAV_FORMATS: return UAV_FORMATS[name]
    return next((v for k, v in UAV_FORMATS.items() if fnmatch.fnmatchcase(name, k)), {})


def lower_naga_unsupported(data):
    """Rewrite dxil-spirv output that naga's SPIR-V frontend rejects, without changing results.

    - SSBO aliases: dxil-spirv declares one buffer twice (u32 words and uvecN) at one binding when the DXIL mixes
      scalar and vector raw accesses; naga rejects duplicate bindings. Each vector load becomes N word loads through
      the u32 view plus OpCompositeConstruct, each vector store N word stores of OpCompositeExtract (same bytes, same
      order). Identical read-only u32 views are merged into one variable.
    - OpSNegate on uint (two's complement) becomes OpISub(0, x).
    - Scalar OpIsInf(f32) becomes (bitcast<u32>(x) & 0x7fffffff) == 0x7f800000 (exact IEEE test; no inf literal).
    """
    header, ops = parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    variables = {a[1]: (a[0], a[2]) for op, a in ops if op == 59}
    deco = {}
    for op, a in ops:
        if op == 71: deco.setdefault(a[0], {})[a[1]] = a[2:]
    bound = header[3]; new_globals = []; consts = {}

    def new_id():
        nonlocal bound
        bound += 1; return bound - 1

    def u32_type():
        found = next((t for t, (op, a) in types.items() if op == 21 and a == [32, 0]), None)
        if found is None:
            found = new_id(); types[found] = (21, [32, 0]); new_globals.append([21, [found, 32, 0]])
        return found

    def u32_const(value):
        if value not in consts:
            consts[value] = new_id(); new_globals.append([43, [u32_type(), consts[value], value]])
        return consts[value]

    def ssbo_view(var):
        pointer, storage = variables[var]
        if storage != 12: return None
        top, members = types[types[pointer][1][1]]
        if top != 30 or len(members) != 1 or types[members[0]][0] != 29: return None
        element = types[members[0]][1][0]; eop, eargs = types[element]
        stride = deco.get(members[0], {}).get(6, [0])[0]
        if eop == 21 and eargs == [32, 0] and stride == 4: return 1, 1
        if eop == 23 and types[eargs[0]] == (21, [32, 0]) and stride % 4 == 0 and stride >= 4 * eargs[1]:
            return eargs[1], stride // 4
        return None

    member_nonwritable = {a[0] for op, a in ops if op == 72 and a[2] == 24}
    groups = {}
    for var, (pointer, storage) in variables.items():
        d = deco.get(var, {})
        if storage == 12 and 33 in d:
            readonly = 24 in d or types[pointer][1][1] in member_nonwritable
            groups.setdefault((d.get(34, [0])[0], d[33][0], readonly), []).append(var)
    vector_alias = {}; same_alias = {}
    for key, members in groups.items():
        if len(members) < 2: continue
        widths = {v: ssbo_view(v) for v in members}
        if key[2] and all(w == (1, 1) for w in widths.values()):
            # Identical read-only u32 views of one binding: use the first one everywhere (same bytes, same type).
            for v in sorted(members)[1:]: same_alias[v] = sorted(members)[0]
            continue
        scalar = [v for v, w in widths.items() if w == (1, 1)]
        if len(scalar) != 1 or None in widths.values():
            raise ValueError('aliased SSBO binding %s needs lowering (views %s)' % (key, sorted(widths.values())))
        for v, w in widths.items():
            if v != scalar[0]: vector_alias[v] = (scalar[0], w)
    scalar_ptr = {}
    for op, a in ops:
        if op == 32 and a[1] == 12 and types.get(a[2]) == (21, [32, 0]): scalar_ptr.setdefault(12, a[0])
    # A uvecN SSBO view whose array stride WGSL cannot express (e.g. vec3<u32> with stride 12; WGSL needs 16) gets a
    # synthesized u32 view at the same binding and is lowered through the same word-load/store path as an alias.
    new_annotations = []; interface_add = []
    alias_targets = {s for s, _ in vector_alias.values()}
    for var, (pointer, storage) in list(variables.items()):
        if storage != 12 or var in vector_alias or var in same_alias or var in alias_targets: continue
        view = ssbo_view(var)
        if not view or view == (1, 1): continue
        width, words = view
        if (words * 4) % (4 * (4 if width == 3 else width)) == 0: continue
        d = deco.get(var, {})
        if 33 not in d: continue
        u32 = u32_type(); array, block, pointer_type, scalar_var = new_id(), new_id(), new_id(), new_id()
        new_globals += [[29, [array, u32]], [30, [block, array]], [32, [pointer_type, 12, block]], [59, [pointer_type, scalar_var, 12]]]
        new_annotations += [[71, [array, 6, 4]], [72, [block, 0, 35, 0]], [71, [block, 2]],
                            [71, [scalar_var, 34, d.get(34, [0])[0]]], [71, [scalar_var, 33, d[33][0]]]]
        if 24 in d: new_annotations.append([71, [scalar_var, 24]])
        if types[pointer][1][1] in member_nonwritable: new_annotations.append([72, [block, 0, 24]])
        if 12 not in scalar_ptr:
            scalar_ptr[12] = new_id(); new_globals.append([32, [scalar_ptr[12], 12, u32]])
        vector_alias[var] = (scalar_var, view); interface_add.append(scalar_var)
    chains = {}; out = []; changed = bool(vector_alias or same_alias)
    for op, a in ops:
        if op in (5, 71) and (a[0] in vector_alias or a[0] in same_alias): continue
        if op == 59 and (a[1] in vector_alias or a[1] in same_alias): continue
        if op == 15 and (vector_alias or same_alias):
            end = 2
            while not any(b == 0 for b in struct.pack('<I', a[end])): end += 1
            end += 1
            a = a[:end] + [x for x in a[end:] if x not in vector_alias and x not in same_alias]
            if header[1] >= 0x10400: a = a + interface_add  # SPIR-V 1.4+: every referenced global is in the interface
        if same_alias and op != 15:
            if op in (65, 66, 68) and a[2] in same_alias:
                a = [a[0], a[1], same_alias[a[2]], *a[3:]]
            elif op >= 20 and op not in (71, 72) and any(x in same_alias for x in a[2:]):
                raise ValueError('aliased SSBO variable used by opcode %d' % op)
        if op in (65, 66) and a[2] in vector_alias:
            if len(a) != 5: raise ValueError('aliased SSBO access chain shape requires lowering')
            chains[a[1]] = (a[2], a[3], a[4]); continue
        if op == 61 and a[2] in chains:
            var, zero, index = chains[a[2]]; scalar, (width, words_per_element) = vector_alias[var]
            if 12 not in scalar_ptr: raise ValueError('aliased SSBO without u32 pointer type')
            u32 = u32_type(); base = new_id()
            out.append([132, [u32, base, index, u32_const(words_per_element)]])
            words = []
            for k in range(width):
                at = base
                if k: at = new_id(); out.append([128, [u32, at, base, u32_const(k)]])
                ptr = new_id(); out.append([65, [scalar_ptr[12], ptr, scalar, zero, at]])
                word = new_id(); out.append([61, [u32, word, ptr]]); words.append(word)
            out.append([80, [a[0], a[1], *words]]); continue
        if op == 62 and a[0] in chains:
            # Store of a uvecN through a vector view of a writable alias: N word stores (same bytes, same order).
            var, zero, index = chains[a[0]]; scalar, (width, words_per_element) = vector_alias[var]
            if 12 not in scalar_ptr or len(a) != 2: raise ValueError('aliased SSBO store shape requires lowering')
            u32 = u32_type(); base = new_id()
            out.append([132, [u32, base, index, u32_const(words_per_element)]])
            for k in range(width):
                at = base
                if k: at = new_id(); out.append([128, [u32, at, base, u32_const(k)]])
                ptr = new_id(); out.append([65, [scalar_ptr[12], ptr, scalar, zero, at]])
                word = new_id(); out.append([81, [u32, word, a[1], k]])
                out.append([62, [ptr, word]])
            continue
        if (any(x in chains for x in a[2:]) or (op == 62 and a[0] in chains)) and op not in (5, 71):
            raise ValueError('aliased SSBO pointer used by opcode %d' % op)
        if op == 126 and types.get(a[0]) == (21, [32, 0]):
            out.append([130, [a[0], a[1], u32_const(0), a[2]]]); changed = True; continue
        if op == 156 and types.get(a[0], (0,))[0] == 20:
            # Scalar OpIsNan(f32) -> (bitcast<u32>(x) & 0x7fffffff) > 0x7f800000 (exact IEEE test).
            u32 = u32_type(); bits = new_id(); mag = new_id()
            out += [[124, [u32, bits, a[2]]], [199, [u32, mag, bits, u32_const(0x7fffffff)]],
                    [172, [a[0], a[1], mag, u32_const(0x7f800000)]]]
            changed = True; continue
        if op == 157:
            if types.get(a[0], (0,))[0] != 20: raise ValueError('vector OpIsInf requires lowering')
            u32 = u32_type(); bits = new_id(); mag = new_id()
            out += [[124, [u32, bits, a[2]]], [199, [u32, mag, bits, u32_const(0x7fffffff)]],
                    [170, [a[0], a[1], mag, u32_const(0x7f800000)]]]
            changed = True; continue
        out.append([op, a])
    if not changed and not new_globals: return data
    first_function = next(i for i, (op, _) in enumerate(out) if op == 54)
    out[first_function:first_function] = new_globals
    if new_annotations:
        first_type = next(i for i, (op, _) in enumerate(out) if 19 <= op <= 39)
        out[first_type:first_type] = new_annotations
    header[3] = bound
    return emit(header, out)


SPIRV_GRAMMAR = ROOT / 'tools/dxil-spirv/third_party/spirv-headers/include/spirv/unified1/spirv.core.grammar.json'
_RESULT_TYPED_OPS = None


def result_typed_ops():
    """Opcodes whose first two operands are (IdResultType, IdResult), from the SPIR-V core grammar."""
    global _RESULT_TYPED_OPS
    if _RESULT_TYPED_OPS is None:
        grammar = json.loads(SPIRV_GRAMMAR.read_text())
        _RESULT_TYPED_OPS = {x['opcode'] for x in grammar['instructions']
                             if [o['kind'] for o in x.get('operands', [])[:2]] == ['IdResultType', 'IdResult']}
    return _RESULT_TYPED_OPS


def lower_signed_uint_ops(data):
    """Make signed SPIR-V operations on 32-bit uint-typed values explicit, without changing results.

    dxil-spirv types every DXIL i32 as uint and relies on the signed opcode (OpConvertFToS, OpConvertSToF, OpSRem,
    GLSL SMin, ...) to carry signedness. naga's SPIR-V frontend uses the operand/result type instead, so it prints
    u32(x) for FToS (negative floats clamp to 0), f32(u32) for SToF (negative ints become ~4e9), and unsigned
    min/max/abs/%/>> for the signed forms. Each such instruction is rewritten to compute in the matching i32 type
    with OpBitcast in and out (bit pattern semantics identical to DXIL two's complement):
      FToS -> FToS(i32) + Bitcast;  SToF(x) -> SToF(Bitcast_i32(x));  SDiv/SRem/SMod/ShiftRightArithmetic and GLSL
      SAbs/SSign/SMin/SMax/SClamp/FindSMsb and GroupNonUniformSMin/SMax -> operands bitcast to i32, result computed
      as i32, bitcast back.
    Signed comparisons are left alone (naga already bitcasts their operands). OpSNegate is handled by
    lower_naga_unsupported. Other signed opcodes on uint (SConvert, SMulExtended, BitFieldSExtract, AtomicSMin/SMax,
    SDot/SUDot, clustered GroupNonUniformSMin/SMax) raise.
    """
    header, ops = parse(data)
    typed = result_typed_ops()
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    value_type = {a[1]: a[0] for op, a in ops if op in typed and len(a) >= 2}
    glsl = {a[0] for op, a in ops if op == 11 and
            b''.join(struct.pack('<I', x) for x in a[1:]).rstrip(b'\0') == b'GLSL.std.450'}
    bound = header[3]; new_globals = []; signed_of = {}; audits = 0

    def new_id():
        nonlocal bound
        bound += 1; return bound - 1

    def lanes_of(t):
        """(lanes, is_uint32) for a 32-bit integer scalar/vector type id, else None."""
        if t not in types: return None
        op, args = types[t]; lanes = 1
        if op == 23: t, lanes = args[0], args[1]; op, args = types[t]
        if op != 21: return None
        if args[0] != 32: raise ValueError('signed op on %d-bit integer requires lowering' % args[0])
        return lanes, args[1] == 0

    def is_uint(t):
        info = lanes_of(t)
        return bool(info and info[1])

    def signed_type(t):
        lanes = lanes_of(t)[0]
        if lanes in signed_of: return signed_of[lanes]
        scalar = next((i for i, (op, a) in types.items() if op == 21 and a == [32, 1]), None)
        if scalar is None:
            scalar = new_id(); types[scalar] = (21, [32, 1]); new_globals.append([21, [scalar, 32, 1]])
        result = scalar
        if lanes > 1:
            result = next((i for i, (op, a) in types.items() if op == 23 and a == [scalar, lanes]), None)
            if result is None:
                result = new_id(); types[result] = (23, [scalar, lanes]); new_globals.append([23, [result, scalar, lanes]])
        signed_of[lanes] = result
        return result

    out = []

    def as_signed(value):
        t = value_type.get(value)
        if not is_uint(t): return value
        cast = new_id(); out.append([124, [signed_type(t), cast, value]]); return cast

    def signed_result(op, a, operands):
        """Emit op computing in i32; bitcast back to the original uint result type when needed."""
        nonlocal audits
        audits += 1
        if is_uint(a[0]):
            tmp = new_id(); out.append([op, [signed_type(a[0]), tmp, *operands]]); out.append([124, [a[0], a[1], tmp]])
        else:
            out.append([op, [a[0], a[1], *operands]])

    for op, a in ops:
        if op == 110 and is_uint(a[0]):  # OpConvertFToS -> uint
            signed_result(op, a, a[2:]); continue
        if op == 111 and is_uint(value_type.get(a[2])):  # OpConvertSToF(uint)
            audits += 1; operand = as_signed(a[2]); out.append([op, [a[0], a[1], operand]]); continue
        if op in (135, 138, 139) and (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[2:4])):
            signed_result(op, a, [as_signed(a[2]), as_signed(a[3])]); continue
        if op == 195 and (is_uint(a[0]) or is_uint(value_type.get(a[2]))):  # shift amount keeps its type
            signed_result(op, a, [as_signed(a[2]), a[3]]); continue
        if op in (353, 356) and len(a) == 5 and (is_uint(a[0]) or is_uint(value_type.get(a[4]))):
            # OpGroupNonUniformSMin/SMax(scope, Reduce|Scan, x): signed subgroup min/max computed in i32.
            signed_result(op, a, [a[2], a[3], as_signed(a[4])]); continue
        if op in (114, 152, 202, 236, 238, 353, 356, 4450, 4452) and                 (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[2:])):
            raise ValueError('signed opcode %d on uint requires lowering' % op)
        if op == 12 and a[2] in glsl and a[3] in (5, 7, 39, 42, 45, 74) and \
                (is_uint(a[0]) or any(is_uint(value_type.get(x)) for x in a[4:])):
            operands = [as_signed(x) for x in a[4:]]
            signed_result(op, a, [a[2], a[3], *operands]); continue
        out.append([op, a])
    if not audits: return data
    first_function = next(i for i, (op, _) in enumerate(out) if op == 54)
    out[first_function:first_function] = new_globals
    header[3] = bound
    return emit(header, out)


def lower_scalar_cbuffer_arrays(data):
    """Rewrite word-indexed constant buffers into the vec4 rows WGSL uniform buffers require (same bytes).

    dxil-spirv declares a cbuffer it must index per 32-bit word as array<f32|u32, N> with ArrayStride 4, which is
    illegal in the WGSL uniform address space (naga: array stride must be a multiple of 16). The array becomes
    array<vec4<T>, ceil(N/4)> (stride 16) and every word load ptr = AccessChain(var, 0, i); Load(ptr) becomes
    Load(AccessChain(var, 0, i >> 2)) followed by VectorExtractDynamic(row, i & 3): the same 4 bytes at offset 4*i.
    """
    header, ops = parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    consts = {a[1]: a[2] for op, a in ops if op == 43}
    stride = {a[0]: a[2] for op, a in ops if op == 71 and a[1] == 6}
    bound = header[3]; new_globals = []; targets = {}

    def new_id():
        nonlocal bound
        bound += 1; return bound - 1

    def find_or_add(op, args):
        found = next((i for i, (o, a) in types.items() if o == op and a == args), None)
        if found is None:
            found = new_id(); types[found] = (op, args); new_globals.append([op, [found, *args]])
        return found

    u32 = next((i for i, (o, a) in types.items() if o == 21 and a == [32, 0]), None)
    for op, a in ops:
        if op != 59 or a[2] != 2: continue
        struct_id = types[a[0]][1][1]; sop, members = types[struct_id]
        if sop != 30 or len(members) != 1: continue
        aop, aargs = types[members[0]]
        if aop != 28 or stride.get(members[0]) != 4: continue
        element = aargs[0]; eop, eargs = types[element]
        if eop not in (21, 22) or eargs[0] != 32: raise ValueError('scalar cbuffer array of unsupported element')
        targets[a[1]] = (members[0], element, consts[aargs[1]])
    if not targets: return data
    if u32 is None: raise ValueError('scalar cbuffer lowering needs a u32 type')
    binding_of = {}
    for op, a in ops:
        if op == 71 and a[1] in (33, 34): binding_of.setdefault(a[0], {})[a[1]] = a[2]
    rewrites = {}; merged = {}
    for var, (array, element, count) in list(targets.items()):
        rows = (count + 3) // 4
        # dxil-spirv may also declare the same cbuffer as array<vec4, M> at the same binding: read through that one.
        twin = next((v for op, a in ops if op == 59 and a[2] == 2 and (v := a[1]) != var and v not in targets
                     and binding_of.get(v) == binding_of.get(var)), None)
        if twin is not None:
            tstruct = types[types[next(a[0] for op, a in ops if op == 59 and a[1] == twin)][1][1]]
            tarray = types[tstruct[1][0]]
            tvec = types[tarray[1][0]]
            if tarray[0] != 28 or tvec[0] != 23 or tvec[1][1] != 4 or consts[tarray[1][1]] < rows:
                raise ValueError('cbuffer twin view has an unexpected layout')
            merged[var] = twin; targets[var] = (tarray[1][0], element, twin); continue
        vec4 = find_or_add(23, [element, 4])
        length = new_id(); new_globals.append([43, [u32, length, rows]])
        rewrites[array] = (vec4, length)
        targets[var] = (vec4, element, var)
    two = new_id(); three = new_id(); new_globals += [[43, [u32, two, 2]], [43, [u32, three, 3]]]
    out = []; chains = {}; row_ptr = {}
    for op, a in ops:
        if op == 28 and a[0] in rewrites:
            out.append([28, [a[0], rewrites[a[0]][0], rewrites[a[0]][1]]]); continue
        if op == 71 and a[1] == 6 and a[0] in rewrites:
            out.append([71, [a[0], 6, 16]]); continue
        if op in (5, 71) and a[0] in merged: continue
        if op == 59 and a[1] in merged: continue
        if op == 15 and merged:
            end = 2
            while not any(b == 0 for b in struct.pack('<I', a[end])): end += 1
            a = a[:end + 1] + [x for x in a[end + 1:] if x not in merged]
        if op in (65, 66) and a[2] in targets:
            if len(a) != 5: raise ValueError('scalar cbuffer access chain shape requires lowering')
            chains[a[1]] = (a[2], a[3], a[4]); continue
        if op == 61 and a[2] in chains:
            var, zero, index = chains[a[2]]; vec4, element, source = targets[var]
            if vec4 not in row_ptr: row_ptr[vec4] = find_or_add(32, [2, vec4])
            row = new_id(); lane = new_id(); ptr = new_id(); value = new_id()
            out += [[194, [u32, row, index, two]], [199, [u32, lane, index, three]],
                    [65, [row_ptr[vec4], ptr, source, zero, row]], [61, [vec4, value, ptr]]]
            lane_type = types[vec4][1][0]
            if lane_type == a[0]: out.append([77, [a[0], a[1], value, lane]])
            else:
                word = new_id(); out += [[77, [lane_type, word, value, lane]], [124, [a[0], a[1], word]]]
            continue
        if op >= 54 and (any(x in chains for x in a[1:]) or any(x in targets for x in a[2:])):
            raise ValueError('scalar cbuffer pointer used by opcode %d' % op)
        out.append([op, a])
    # The rewritten OpTypeArray now references the new row-count constant: declare the additions before it.
    first_array = next((i for i, (op, a) in enumerate(out) if op == 28 and a[0] in rewrites),
                       next(i for i, (op, _) in enumerate(out) if op == 54))
    out[first_array:first_array] = new_globals
    header[3] = bound
    return emit(header, out)


def lower_image_arrays(data):
    """Split constant-indexed arrays of images (e.g. RWTexture2D out[6] at u1) into one variable per element."""
    header, ops = parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    consts = {a[1]: a[2] for op, a in ops if op == 43}
    deco = {}
    for op, a in ops:
        if op == 71: deco.setdefault(a[0], []).append(a[1:])
    ptr_for = {a[2]: a[0] for op, a in ops if op == 32 and a[1] == 0}
    bound = header[3]; added_types = []; elems = {}; new_vars = []; new_decos = []
    for op, a in ops:
        if op != 59 or a[2] != 0: continue
        aop, aargs = types[types[a[0]][1][1]]
        if aop != 28 or types[aargs[0]][0] != 25: continue
        elem, n = aargs[0], consts[aargs[1]]
        d = deco.get(a[1], [])
        base = next(x[1] for x in d if x[0] == 33)
        if elem not in ptr_for:
            ptr_for[elem] = bound; added_types.append([32, [bound, 0, elem]]); bound += 1
        ids = []
        for i in range(n):
            vid = bound; bound += 1; ids.append(vid)
            new_vars.append([59, [ptr_for[elem], vid, 0]])
            new_decos += [[71, [vid, 34, 0]], [71, [vid, 33, base + i]]] + [[71, [vid] + x] for x in d if x[0] not in (33, 34)]
        elems[a[1]] = ids
    if not elems: return data
    out = []; alias = {}
    for op, a in ops:
        if op in (71, 5) and a[0] in elems: continue
        if op == 59 and a[1] in elems: continue
        if op == 65 and a[2] in elems:
            if len(a) != 4 or a[3] not in consts: raise ValueError('dynamically indexed image array requires switch lowering')
            alias[a[1]] = elems[a[2]][consts[a[3]]]; continue
        if op == 61 and a[2] in alias: a = [a[0], a[1], alias[a[2]], *a[3:]]
        if op == 15:
            a = [x for x in a] ; end = 2
            while not any(b == 0 for b in struct.pack('<I', a[end])): end += 1
            end += 1
            iface = [x for x in a[end:] if x not in elems] + [v for ids in elems.values() for v in ids]
            a = a[:end] + iface
        out.append([op, a])
    first_type = next(i for i, (op, _) in enumerate(out) if 19 <= op <= 39)
    out[first_type:first_type] = new_decos
    last_var = max(i for i, (op, _) in enumerate(out) if op == 59 or (19 <= op <= 39))
    out[last_var + 1:last_var + 1] = added_types + new_vars
    header[3] = bound
    return emit(header, out)


def lower_typed_buffers(data):
    """Lower 32-bit integer typed UAV buffers (RWBuffer<uint|int>, R32_UINT views) to storage-buffer word arrays.

    WebGPU has no texel buffers. Each storage image with Dim=Buffer, Sampled=2, a 32-bit int sampled type and format
    R32ui/R32i/Unknown keeps its variable id, Binding and DescriptorSet, and becomes a StorageBuffer Block holding a
    runtime array of that int (ArrayStride 4: element i is bytes 4i..4i+3, the D3D R32 typed element i):
      OpImageWrite(img, i, texel)     -> Store(AccessChain(var, 0, i), texel.x)   (R32 typed store keeps .x only)
      OpImageRead(img, i)             -> (Load(AccessChain(var, 0, i)), 0, 0, 1)  (D3D R32 typed load fills g,b=0 a=1)
      OpImageTexelPointer(var, i, 0)  -> AccessChain(var, 0, i)                    (atomics then act on that word)
      OpImageQuerySize(img)           -> OpArrayLength(var, 0)                     (element count)
    When a lowered buffer also has atomics, its plain loads/stores become OpAtomicLoad/OpAtomicStore (relaxed, device
    scope) so the WGSL atomic<T> array stays valid; the values are the same as the native typed accesses.
    Float/unorm typed buffers raise (they need the engine buffer format and packing).
    """
    header, ops = parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    targets = {}
    for op, a in ops:
        if op != 59 or a[2] != 0: continue
        pointee = types[a[0]][1][1]; top, targs = types[pointee]
        if top != 25 or targs[1] != 5: continue
        sop, sargs = types[targs[0]]
        if targs[5] != 2: raise ValueError('typed buffer SRV (texel buffer) requires lowering')
        if sop != 21 or sargs[0] != 32 or targs[6] not in (0, 33, 24):
            raise ValueError('non-R32 integer typed buffer (texel buffer) requires lowering')
        targets[a[1]] = (pointee, targs[0])
    if not targets: return data
    typed = result_typed_ops()
    value_type = {a[1]: a[0] for op, a in ops if op in typed and len(a) >= 2}
    bound = header[3]; new_types = []; new_annotations = []

    def new_id():
        nonlocal bound
        bound += 1; return bound - 1

    def find_or_add(op, args):
        found = next((i for i, (o, x) in types.items() if o == op and x == args), None)
        if found is None:
            found = new_id(); types[found] = (op, args); new_types.append([op, [found, *args]])
        return found

    consts = {(a[0], a[2]): a[1] for op, a in ops if op == 43 and len(a) == 3}

    def const(t, value):
        if (t, value) not in consts:
            consts[(t, value)] = new_id(); new_types.append([43, [t, consts[(t, value)], value]])
        return consts[(t, value)]

    u32 = find_or_add(21, [32, 0])
    zero = const(u32, 0)
    block_ptr = {}; element_ptr = {}
    for var, (image, element) in targets.items():
        array = new_id(); block = new_id()
        new_types += [[29, [array, element]], [30, [block, array]]]
        types[array] = (29, [element]); types[block] = (30, [array])
        new_annotations += [[71, [array, 6, 4]], [72, [block, 0, 35, 0]], [71, [block, 2]]]
        block_ptr[var] = find_or_add(32, [12, block])
        element_ptr[var] = find_or_add(32, [12, element])
    atomic_ops = {227, 228, 229, 230, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242}
    texel_ptr_var = {a[1]: a[2] for op, a in ops if op == 60 and a[2] in targets}
    atomic_vars = {texel_ptr_var[a[0] if op == 228 else a[2]] for op, a in ops
                   if op in atomic_ops and (a[0] if op == 228 else a[2]) in texel_ptr_var}
    loaded = {}; out = []; old_types = {image for image, _ in targets.values()}; in_function = False; moved_vars = []
    for op, a in ops:
        in_function = in_function or op == 54
        if op == 59 and a[1] in targets:
            old_types.add(a[0]); moved_vars.append([59, [block_ptr[a[1]], a[1], 12]]); continue
        if op == 71 and a[0] in targets and a[1] == 25: continue  # NonReadable: WGSL storage buffers are readable
        if op == 61 and a[2] in targets:
            loaded[a[1]] = a[2]; continue
        if op == 99 and a[0] in loaded:
            var = loaded[a[0]]; element = targets[var][1]
            if len(a) != 3: raise ValueError('typed buffer write with image operands requires lowering')
            ptr = new_id(); out.append([65, [element_ptr[var], ptr, var, zero, a[1]]])
            texel = a[2]; ttype = value_type.get(texel)
            if ttype != element:
                if types.get(ttype, (0,))[0] != 23: raise ValueError('typed buffer write of unexpected texel type')
                x = new_id(); out.append([81, [element, x, texel, 0]]); texel = x
            out.append([228, [ptr, const(u32, 1), const(u32, 0), texel]] if var in atomic_vars else [62, [ptr, texel]])
            continue
        if op == 98 and a[2] in loaded:
            var = loaded[a[2]]; element = targets[var][1]
            if len(a) != 4: raise ValueError('typed buffer read with image operands requires lowering')
            ptr = new_id(); x = new_id()
            out.append([65, [element_ptr[var], ptr, var, zero, a[3]]])
            out.append([227, [element, x, ptr, const(u32, 1), const(u32, 0)]] if var in atomic_vars else [61, [element, x, ptr]])
            rop, rargs = types[a[0]]
            if a[0] == element: out.append([83, [element, a[1], x]])  # OpCopyObject
            elif rop == 23 and rargs == [element, 4]:
                out.append([80, [a[0], a[1], x, const(element, 0), const(element, 0), const(element, 1)]])
            else: raise ValueError('typed buffer read of unexpected result type')
            continue
        if op == 104 and a[2] in loaded:
            if a[0] != u32: raise ValueError('typed buffer size query of unexpected result type')
            out.append([68, [a[0], a[1], loaded[a[2]], 0]]); continue
        if op == 60 and a[2] in targets:
            if types[types[a[0]][1][1]] != types[targets[a[2]][1]]: raise ValueError('typed buffer texel pointer type')
            old_types.add(a[0]); out.append([65, [element_ptr[a[2]], a[1], a[2], zero, a[3]]]); continue
        image_use = op in (57, 61, 62, 65, 66, 68, 83, 169, 245) or 60 <= op <= 107
        if in_function and image_use and any(x in loaded or x in targets for x in a):
            raise ValueError('typed buffer used by opcode %d' % op)
        out.append([op, a])
    # Drop the now-unreferenced image, image-pointer and texel-pointer types (and ImageBuffer when no buffer image is left).
    # References are matched by operand position (literal operands such as ArrayStride 16 must not count as uses).
    removed = set()
    while True:
        live = [(op, a) for op, a in out if not (19 <= op <= 39 and a[0] in removed)]
        refs = {a[0] for op, a in live if (op in typed or op in (43, 59)) and len(a) > 1}
        refs |= {a[2] for op, a in live if op == 32} | {a[1] for op, a in live if op in (27, 28, 29)}
        free = {t for t in old_types - removed if t not in refs}
        if not free: break
        removed |= free
    out = [[op, a] for op, a in out if not ((19 <= op <= 39 or op in (5, 71)) and a[0] in removed)]
    if not any(op == 25 and a[2] == 5 for op, a in out):
        out = [[op, a] for op, a in out if not (op == 17 and a[0] == 47)]
    # Globals may interleave types and variables: declare the new types, then the retyped variables, before the code.
    first_function = next(i for i, (op, _) in enumerate(out) if op == 54)
    out[first_function:first_function] = new_types + moved_vars
    first_type = next(i for i, (op, _) in enumerate(out) if 19 <= op <= 39)
    out[first_type:first_type] = new_annotations
    header[3] = bound
    return emit(header, out)


def split_loop_merge_headers(data):
    """Give a loop whose merge block is itself the next loop's header a merge block of its own (same CFG paths).

    dxil-spirv can emit loop A's merge block M as loop B's header (M holds an OpLoopMerge). naga's SPIR-V frontend
    then resolves A's exits inside B's scope and rejects them ("break is used outside of a loop"). An empty block N
    is inserted before M: A's OpLoopMerge names N, every branch to M from a block laid out before M (A's construct;
    B's back edges come after M, which dominates them) goes to N, N branches to M, and M's OpPhi pairs from the
    redirected blocks become OpPhis in N whose results flow into M from N. Unchanged data when the pattern is absent.
    Used only as a fallback when naga rejects the default output with that error.
    """
    header, ops = parse(data)
    labels = {}; block_of = []; current = None
    for i, (op, a) in enumerate(ops):
        if op == 248: current = a[0]; labels[current] = i
        block_of.append(current)
    loop_headers = {block_of[i] for i, (op, a) in enumerate(ops) if op == 246}
    merges = {a[0] for op, a in ops if op == 246 and a[0] in loop_headers}
    if not merges: return data
    bound = header[3]; new_block = {}
    for m in sorted(merges): new_block[m] = bound; bound += 1

    def label_operands(op, a):
        if op == 249: return [0]
        if op == 250: return [1, 2]
        if op == 251: return [1] + list(range(3, len(a), 2))
        return []

    redirected = {m: set() for m in merges}; out = []
    for i, (op, a) in enumerate(ops):
        b = block_of[i]
        if op == 246 and a[0] in merges and labels[a[0]] > labels[b]:
            a = [new_block[a[0]], *a[1:]]
        elif op in (249, 250, 251):
            a = list(a)
            for k in label_operands(op, a):
                if a[k] in merges and labels[a[k]] > labels[b]:
                    redirected[a[k]].add(b); a[k] = new_block[a[k]]
        out.append([op, a])
    result = []
    for j, (op, a) in enumerate(out):
        if op == 248 and a[0] in merges:
            m = a[0]; k = j + 1; phis = []
            while out[k][0] == 245:
                ptype, pid, *flat = out[k][1]
                pairs = list(zip(flat[0::2], flat[1::2]))
                moved = [p for p in pairs if p[1] in redirected[m]]
                kept = [p for p in pairs if p[1] not in redirected[m]]
                if moved:
                    nid = bound; bound += 1
                    phis.append([245, [ptype, nid, *[v for p in moved for v in p]]])
                    kept.append((nid, new_block[m]))
                out[k] = [245, [ptype, pid, *[v for p in kept for v in p]]]
                k += 1
            result += [[248, [new_block[m]]], *phis, [249, [m]]]
        result.append([op, a])
    header[3] = bound
    return emit(header, result)


# Hidden UAV counters (dxil-spirv DescriptorSet 7, Binding = DXIL UAV range ID) bind at 448 + the owning UAV register.
COUNTER_BINDING_BASE = 448


def remap_bindings(data, reflected, name='', counters=None):
    """Rebind every descriptor to the port's register convention; return (spirv, resources).

    counters maps a DXIL UAV range ID to the register of the RW structured buffer that owns the hidden counter
    (reflect_dxil_counters). After lower_typed_buffers the counter is an array<u32> storage buffer; word 0 is the count.
    """
    header, ops = parse(data)
    types = {a[0]: (op, a[1:]) for op, a in ops if 19 <= op <= 33}
    variables = {a[1]: (a[0], a[2]) for op, a in ops if op == 59}
    deco = {}
    for op, a in ops:
        if op == 71: deco.setdefault(a[0], {})[a[1]] = a[2:]
    member_nonwritable = {a[0] for op, a in ops if op == 72 and a[2] == 24}
    new_binding = {}; found = set(); image_format = {}; split_images = {}; bound = header[3]
    image_registers = set()  # t# registers held by sampled images (never by a buffer)
    for var, (pointer, storage) in variables.items():
        d = deco.get(var, {})
        if 33 in d and storage == 0 and types[types[pointer][1][1]][0] == 25 and types[types[pointer][1][1]][1][5] == 1:
            image_registers.add(('t', d[33][0]))
    for var, (pointer, storage) in variables.items():
        d = deco.get(var, {})
        if 33 not in d: continue
        slot = d[33][0]
        if d.get(34, [0])[0] == 7 and storage == 12 and counters and slot in counters:
            new_binding[var] = COUNTER_BINDING_BASE + counters[slot]; continue
        if d.get(34, [0])[0]: raise ValueError('register space requires lowering')
        pointee = types[pointer][1][1]; top, targs = types[pointee]
        if storage == 2: kind, binding = 'cb', slot
        elif storage == 12:
            readonly = 24 in d or pointee in member_nonwritable
            if readonly and ('u', slot) in reflected and (('t', slot) not in reflected or ('t', slot) in image_registers):
                # RW buffer the program only reads: dxil-spirv marks it NonWritable, but the engine binds it as a UAV
                # (the DXIL has no buffer SRV at t#, or t# is a texture). Keep the UAV register (binding 288+slot,
                # WGSL var<storage, read>).
                readonly = False
            kind, binding = ('t', 64 + slot) if readonly else ('u', 288 + slot)
        elif storage == 0 and top == 26: kind, binding = 's', 256 + slot
        elif storage == 0 and top == 25:
            if targs[1] == 5: raise ValueError('typed buffer (texel buffer) requires lowering')
            kind, binding = ('t', 64 + slot) if targs[5] == 1 else ('u', 288 + slot)
            # An explicit table slot wins over dxil-spirv's own guess (it writes r32f on read UAVs, dropping .yzw).
            if kind == 'u' and slot in uav_formats(name):
                fmt = SPV_IMAGE_FORMAT[uav_formats(name)[slot]]
                if image_format.setdefault(pointee, fmt) != fmt:
                    # UAVs sharing one OpTypeImage need different formats: give this one its own image/pointer type.
                    split_images[var] = (pointer, bound, bound + 1, [bound] + targs[:6] + [fmt] + targs[7:]); bound += 2
        else: raise ValueError('unsupported resource storage %d type %d' % (storage, top))
        new_binding[var] = binding; found.add((kind, slot))
    # A register the DXIL declares but never accesses has no SPIR-V variable (dxil-spirv emits only used handles);
    # the runtime simply never binds it. Anything else (extra or reclassified registers) is a mismatch.
    if not found <= reflected:
        raise ValueError('register classification %s differs from DXIL reflection %s' % (sorted(found), sorted(reflected)))
    for op, a in ops:
        if op == 71 and a[1] == 33 and a[0] in new_binding: a[2] = new_binding[a[0]]
        if op == 71 and a[1] == 34 and a[0] in new_binding: a[2] = 0
        if op == 25 and a[0] in image_format: a[7] = image_format[a[0]]
        if op == 59 and a[1] in split_images: a[0] = split_images[a[1]][2]
        if op == 61 and a[2] in split_images: a[0] = split_images[a[2]][1]
    for var, (pointer, image, image_pointer, image_args) in split_images.items():
        at = next(i for i, (op, a) in enumerate(ops) if op == 32 and a[0] == pointer) + 1
        ops[at:at] = [[25, image_args], [32, [image_pointer, 0, image]]]
    header[3] = bound
    # A storage image (Sampled=2) still in Unknown format would only fail later in naga with a generic message.
    final_types = {a[0]: a for op, a in ops if op == 25}
    for var, (pointer, storage) in variables.items():
        d = deco.get(var, {})
        if 33 not in d or storage != 0: continue
        image = split_images[var][1] if var in split_images else types[pointer][1][1]
        t = final_types.get(image)
        if t is not None and t[6] == 2 and t[7] == 0:
            raise ValueError('UAV u%d of %s has no storage format (add UAV_FORMATS entry)' % (d[33][0], name))
    # WGSL storage buffers are read or read_write only: drop NonReadable (25) from buffer blocks/members.
    # Write-only images keep it (texture_storage write access exists).
    ssbo_blocks = {types[types[p][1][1]][0] and types[p][1][1] for v, (p, s) in variables.items() if s == 12}
    ops = [[op, a] for op, a in ops if not ((op == 72 and a[0] in ssbo_blocks and a[2] == 25) or
                                            (op == 71 and a[1] == 25 and (a[0] in ssbo_blocks or variables.get(a[0], (0, 0))[1] == 12)))]
    return emit(header, ops)


DXIL_ROW = re.compile(r'^;\s+(?:cbuffer|texture|sampler|UAV)\w*.*?\s(?:CB|T|S|U)\d+\s+(cb|t|s|u)(\d+)\s+(\d+)\s*$')


def reflect_dxil(path):
    dump = subprocess.run([str(DXC), '-dumpbin', str(path)], capture_output=True, text=True, check=True).stdout
    section = dump.split('; Resource Bindings:', 1)[1].split('; ViewId', 1)[0]
    result = set()
    for line in section.splitlines():
        m = DXIL_ROW.search(line)
        if m:
            base, count = int(m.group(2)), int(m.group(3))
            if count < 1 or count > 64: raise ValueError('unbounded descriptor array requires lowering')
            result |= {(m.group(1), base + i) for i in range(count)}
        elif re.search(r'\s(?:CB|T|S|U)\d+\s+(?:cb|t|s|u)\d+\s+\d+\s*$', line):
            raise ValueError('unparsed DXIL resource row: ' + line.strip())
    return result


def reflect_dxil_counters(path):
    """{UAV range ID: u# register} for every UAV the DXIL declares with a hidden counter (r/w+cnt)."""
    dump = subprocess.run([str(DXC), '-dumpbin', str(path)], capture_output=True, text=True, check=True).stdout
    section = dump.split('; Resource Bindings:', 1)[1].split('; ViewId', 1)[0]
    counters = {}
    for line in section.splitlines():
        m = re.search(r'\+cnt\s+U(\d+)\s+u(\d+)\s+1\s*$', line)
        if m: counters[int(m.group(1))] = int(m.group(2))
        elif '+cnt' in line: raise ValueError('unparsed DXIL counter row: ' + line.strip())
    return counters


def uniform_layouts(wgsl):
    """dxil-spirv keeps each cbuffer as array<vec4<f32>, N>: native and web byte layouts are identical."""
    layouts = []
    structs = dict(re.findall(r'struct\s+(\w+)\s*\{\s*\w+\s*:\s*array<vec4<f32>,\s*(\d+)u?>\s*,?\s*\}', wgsl))
    for binding, typename in re.findall(r'@group\(0\)\s*@binding\((\d+)\)\s*var<uniform>\s+\w+\s*:\s*(\w+)\s*;', wgsl):
        if typename not in structs: raise ValueError('uniform block is not a vec4 array')
        size = int(structs[typename]) * 16
        layouts.append({'Group': 0, 'Binding': int(binding), 'NativeMinimumBytes': size, 'WebBytes': size,
                        'Fields': [{'Path': 'vec4 array', 'NativeOffset': 0, 'WebOffset': 0, 'Bytes': size}]})
    return layouts


def convert(name, inventory, cache):
    record = {'Name': name.removeprefix('m_'), 'RuntimeName': name, 'Complete': False}
    key = struct.pack('<I', name_hash32(name)); at = cache.find(key)
    if at < 0 or cache.find(key, at + 1) >= 0: raise ValueError('static cache meta entry not unique')
    program = struct.unpack_from('<4Q', cache, at + 4)[2]
    entry = inventory.get(str(program))
    if not entry or entry['Stage'] != 'compute': raise ValueError('compute program %d not in inventory' % program)
    dxil = Path(entry['Path'].replace('D:\\cp2077 port', str(ROOT)))
    if sha(dxil) != entry['SHA256']: raise ValueError('extracted DXIL changed')
    record.update(ProgramHash=str(program), OriginalBytecodePath=str(dxil), OriginalBytecodeSHA256=entry['SHA256'],
                  OriginalBytes=entry['Bytes'])
    stem = record['Name']
    raw = WORK / f'{stem}-dxil.spv'
    p = subprocess.run([str(SPIRV), str(dxil), '--ssbo-srv', '--ssbo-uav', '--output', str(raw)], capture_output=True, text=True)
    if p.returncode: raise ValueError('dxil-spirv: ' + (p.stdout + p.stderr)[-400:])
    native = OUT / f'{stem}-native.spv'

    def lowered(spirv):
        return remap_bindings(lower_typed_buffers(lower_image_arrays(lower_scalar_cbuffer_arrays(lower_naga_unsupported(
            lower_signed_uint_ops(spirv))))), reflect_dxil(dxil), name, reflect_dxil_counters(dxil))
    wgsl = OUT / f'{stem}-static.wgsl'

    def translate():
        p = subprocess.run([str(NAGA), '--keep-coordinate-space', str(native), str(wgsl)], capture_output=True, text=True)
        # Stock naga 30 also panics on every arrayed storage image ("Required coordinate type should have been set up by
        # parse_type_image"); the scope-fixed frontend handles both, and stock naga revalidates its WGSL below.
        if p.returncode and ("can't be introduced - it's already in scope" in p.stdout + p.stderr or
                             "Required coordinate type" in p.stdout + p.stderr) and NAGA_SCOPE_FIXED.exists():
            p = subprocess.run([str(NAGA_SCOPE_FIXED), '--keep-coordinate-space', str(native), str(wgsl)], capture_output=True, text=True)
            if not p.returncode:
                record['NagaFrontend'] = 'naga-scope-fixed'
                check = subprocess.run([str(NAGA), str(wgsl)], capture_output=True, text=True)  # stock naga must accept the WGSL
                if check.returncode: raise ValueError('naga revalidation of scope-fixed output: ' + (check.stdout + check.stderr)[-400:])
        return p
    native.write_bytes(lowered(raw.read_bytes()))
    p = translate()
    if p.returncode and 'is used outside of a `loop` or `switch` context' in p.stdout + p.stderr:
        # naga misreads a loop merge block that is also the next loop's header; retry with an explicit merge block.
        split = split_loop_merge_headers(raw.read_bytes())
        if split != raw.read_bytes():
            native.write_bytes(lowered(split)); record['LoopMergeSplit'] = True
            p = translate()
    if p.returncode: raise ValueError('naga: ' + '\n'.join(l for l in (p.stdout + p.stderr).splitlines() if 'WARN' not in l)[-600:])
    text = wgsl.read_text()
    if name in STORAGE_RMW_TO_LOCAL:
        text = lower_storage_rmw_to_local(text, name)
        wgsl.write_bytes(text.encode("utf-8"))
        p = subprocess.run([str(NAGA), str(wgsl)], capture_output=True, text=True)
        if p.returncode: raise ValueError('naga revalidation of rmw lowering: ' + (p.stdout + p.stderr)[-400:])
        record['StorageRMWToLocal'] = True
    if 'binding_array<' not in text and 'wgpu_binding_array' in text:
        # Leftover from a split image array (declared capability only): not a WebGPU extension.
        text = re.sub(r'^\s*enable\s+wgpu_binding_array\s*;\s*\n', '', text, flags=re.M)
        wgsl.write_text(text)
        p = subprocess.run([str(NAGA), str(wgsl)], capture_output=True, text=True)
        if p.returncode: raise ValueError('naga revalidation: ' + (p.stdout + p.stderr)[-400:])
    if re.search(r'(quad\w+|subgroup\w+)\s*\(', text) and not re.search(r'^\s*enable\s+subgroups\s*;', text, re.M):
        # WebGPU requires the explicit directive (the port requests the 'subgroups' device feature).
        text = 'enable subgroups;\n' + text
        wgsl.write_text(text)
        # naga 30's CLI does not accept the subgroups enable; the headless Deno GPU check validates these.
    storage_formats = set(re.findall(r'texture_storage_\w+<(\w+),', text))
    if storage_formats & TIER1_STORAGE_FORMATS:
        record['StorageOutputSpecialization'] = {'RequiresDeviceFeature': 'texture-formats-tier1',
                                                 'Formats': sorted(storage_formats & TIER1_STORAGE_FORMATS)}
    record.update(WGSL=str(wgsl), WGSLSHA256=sha(wgsl), NativeSPIRVSHA256=sha(native), EntryPoint='main',
                  UniformLayouts=uniform_layouts(text), WGSLUniformLayoutRoundtripVerified=True, Complete=True)
    return record


def main():
    args = sys.argv[1:]
    names = []
    if args and args[0] == '--from-log':
        text = Path(args[1]).read_text(errors='replace')
        names = sorted(set(re.findall(r'exact converted bytecode record: stage=2 bytes=\d+ name=(m_\w+)', text)))
    else: names = args
    WORK.mkdir(parents=True, exist_ok=True)
    cache = CACHE.read_bytes()
    inventory = {s['Hash']: s for s in json.loads((OUT / 'native-static-shaders.json').read_text())['Shaders']}
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {
        'Toolchain': 'dxil-spirv+naga', 'TranslatorSHA256': sha(SPIRV), 'NagaSHA256': sha(NAGA), 'Shaders': []}
    rows = {r['RuntimeName']: r for r in manifest['Shaders']}
    for name in names:
        try: rows[name] = convert(name, inventory, cache); status = 'converted'
        except Exception as error: rows[name] = {'Name': name.removeprefix('m_'), 'RuntimeName': name, 'Complete': False, 'Error': str(error)}; status = 'FAILED ' + str(error)[:160]
        print(name, status, flush=True)
    manifest['Shaders'] = sorted(rows.values(), key=lambda r: r['RuntimeName'])
    MANIFEST.write_text(json.dumps(manifest, indent=2))
    print('complete', sum(r['Complete'] for r in rows.values()), 'of', len(rows))


if __name__ == '__main__':
    main()
