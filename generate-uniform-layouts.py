"""Generate immutable runtime copy tables from validated original shader maps."""
from pathlib import Path
import json
import hashlib
import re
from hud_cpu_abi import hud_cpu_layout

root = Path(__file__).resolve().parent
manifest_path = root / 'build/shaders/ui-shader-conversion.json'
manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
compression_manifest = root/'build/shaders/compression-shader-conversion.json'
if compression_manifest.exists():
    compression = json.loads(compression_manifest.read_text(encoding='utf-8'))
    if compression['DxcSHA256'] != manifest['DxcSHA256'] or compression['NagaSHA256'] != manifest['NagaSHA256']:
        raise ValueError('Compression shader toolchain differs from the validated HUD toolchain')
    manifest['Shaders'].extend(compression['Shaders'])
glyph_manifest=root/'build/shaders/glyph-shader-conversion.json'
if glyph_manifest.exists():
    glyph=json.loads(glyph_manifest.read_text())
    if glyph['DxcSHA256']!=manifest['DxcSHA256'] or glyph['NagaSHA256']!=manifest['NagaSHA256']:
        raise ValueError('Glyph shader toolchain differs')
    manifest['Shaders'].extend(glyph['Shaders'])
# Original static-cache compute programs converted from their exact DXIL (convert-static-compute.py). A row is published
# when it passed the headless GPU compute-pipeline check, or when it passed the proxy check (same WGSL hash) and every
# proxy substitution is covered by a runtime requirement bit (requiredCaps) that BeginCreation checks on the real device.
# Keep this block identical in generate-web-shader-package.py and generate-uniform-layouts.py.
CAP_SUBGROUPS, CAP_FORMATS_TIER1, CAP_LARGE_WORKGROUP = 1, 2, 4
CORE_STORAGE_FORMATS = {'rgba8unorm','rgba8snorm','rgba8uint','rgba8sint','rgba16uint','rgba16sint','rgba16float',
                        'r32uint','r32sint','r32float','rg32uint','rg32sint','rg32float','rgba32uint','rgba32sint','rgba32float'}
TIER1_STORAGE_FORMATS = {'r8unorm','r8snorm','r8uint','r8sint','rg8unorm','rg8snorm','rg8uint','rg8sint','r16unorm',
                         'r16snorm','r16uint','r16sint','r16float','rg16unorm','rg16snorm','rg16uint','rg16sint','rg16float',
                         'rgba16unorm','rgba16snorm','rgb10a2uint','rgb10a2unorm','rg11b10ufloat'}
def compute_requirements(text):
    caps = 0
    for extension in re.findall(r'^\s*enable\s+([\w\s,]+);', text, re.M):
        for name in (e.strip() for e in extension.split(',') if e.strip()):
            if name != 'subgroups': raise ValueError(f'Unsupported WGSL extension: {name}')
            caps |= CAP_SUBGROUPS
    for fmt, access in re.findall(r'texture_storage_\w+<\s*(\w+)\s*,\s*(\w+)\s*>', text):
        if access == 'read_write' and fmt not in ('r32float','r32uint','r32sint'):
            raise ValueError(f'read_write storage texture format needs tier2: {fmt}')
        if fmt in TIER1_STORAGE_FORMATS: caps |= CAP_FORMATS_TIER1
        elif fmt not in CORE_STORAGE_FORMATS: raise ValueError(f'Unsupported storage texture format: {fmt}')
    sizes = re.findall(r'@workgroup_size\(([^)]*)\)', text)
    if len(sizes) != 1: raise ValueError('Expected exactly one compute workgroup size')
    invocations = 1
    for dimension in (d.strip() for d in sizes[0].split(',') if d.strip()):
        invocations *= int(dimension.rstrip('iu'))
    if invocations > 256: caps |= CAP_LARGE_WORKGROUP
    return caps, invocations
def proxy_covered(row, proxy, caps, invocations):
    entry = proxy.get(row['RuntimeName']) or {}
    if entry.get('Status') != 'PROXY-OK' or entry.get('WGSLSHA256') != row.get('WGSLSHA256') or not caps: return False
    tier1 = (row.get('StorageOutputSpecialization') or {}).get('RequiresDeviceFeature') == 'texture-formats-tier1'
    if tier1 != bool(caps & CAP_FORMATS_TIER1): return False
    for substitution in entry.get('Substitutions', []):
        if substitution == 'maxComputeInvocationsPerWorkgroup=adapter': continue  # covered by workgroupInvocations
        # tools/headless/proxy-validate-compute.py: subgroup/quad builtins and entry builtins (subgroupBroadcastFirst,
        # quadBroadcast, subgroupOr/Max/Ballot..., subgroup_invocation_id/size) need the runtime subgroups capability.
        if substitution.startswith(('subgroup', 'quad')) and caps & CAP_SUBGROUPS: continue
        if substitution.split('->')[0] in TIER1_STORAGE_FORMATS and caps & CAP_FORMATS_TIER1: continue
        return False
    return True
# Rows that must be published (and runnable) together or not at all; requirements are the union of the group.
STATIC_COMPUTE_UNITS = [('m_histogramDownscale','m_histogramGather','m_histogramSumUp','m_histogramAverage','m_histogramFrameExposure'),
                        # CHierarchicalDepthBuffer::BuildDepthChain (renderHiZ.cpp:99/108/109): grab, then both mip chains.
                        ('m_hierarchicalZGrab','m_downsampleDepth','m_downsampleDepthForceMax'),
                        # Clustered light culling (clusteredDeferred.cpp:279-304): InitCull then BinPack, frustum variants.
                        ('m_clusteredInitCull','m_clusteredInitCullNoFrustum','m_clusteredBinPack','m_clusteredBinPackNoFrustum'),
                        # Volumetric fog (renderVolumetricFogManager.cpp:364-461): lighting then scattering, near and distant.
                        ('m_volumetricFogLighting','m_volumetricFogLighting_NoParticles','m_volumetricFogLighting_Distant',
                         'm_volumetricFogScattering','m_volumetricFogScattering_Distant'),
                        # GI tetra acceleration volume (renderGIManager.cpp:1378-1469): clear then build.
                        ('m_giTetraClearVolume','m_giTetraBuildVolume'),
                        # --- Round 6 (build/research/convert/PLAN.md F1/F2). Mixed requirements inside a chain must not
                        # leave a consumer running on unwritten input on devices without tier1 / subgroups.
                        # Cone AO (renderPostFxConeAO.cpp): downsample (tier1 r8uint/rg16float) feeds HalfRes/Filter/Upsample.
                        ('m_coneAODownsampleCS','m_coneAOBlank','m_coneAOHalfResCS_Quality0','m_coneAOHalfResCS_Quality1',
                         'm_coneAOHalfResCS_Quality2','m_coneAOFilterCS','m_coneAOFilterHalfResCS','m_coneAOFilterTemporalHalfRes',
                         'm_coneAOUpsampleCS'),
                        # Screen-space reflections (renderNode_ScreenSpaceReflections.cpp): depth/colour downsample, then trace.
                        ('m_postfxSSR_DownsampleDepth','m_postfxSSRDownsample','m_postfxSSR_LOW','m_postfxSSR_LOW_EnforceUberQuality',
                         'm_postfxSSR_MEDIUM','m_postfxSSR_MEDIUM_EnforceUberQuality','m_postfxSSR_HIGH','m_postfxSSR_HIGH_EnforceUberQuality',
                         'm_postfxSSR_ULTRA','m_postfxSSR_ULTRA_EnforceUberQuality','m_postfxSSR_CINEMATIC',
                         'm_postfxSSR_CINEMATIC_EnforceUberQuality','m_postfxSSR_Planar_LOW','m_postfxSSR_Planar_Filter'),
                        # Focus/vision mode (gameplayFx_CPFocusMode.cpp:414-627): luminance, edges, pixelization, final apply.
                        ('m_postfx_CPVisionMode_SceneToLuminance','m_postfx_CPVisionMode_EdgeDetectR','m_postfx_CPVisionMode_Highlights',
                         'm_postfx_CPVisionMode_UpdateResolution','m_postfx_CPVisionMode_DownsampleResolution',
                         'm_postfx_CPVisionMode_UpsampleResolution','m_postfx_CPVisionMode_FinalizeResolution',
                         'm_postfx_CPVisionMode_ApplyFullscreen'),
                        # Water FFT (renderSimWaterFFT.cpp:147/159/188): the three fused passes.
                        ('m_shaderFFTGPU_Phillips_Animate_Butterfly_Transpose','m_shaderFFTGPU_Butterfly','m_shaderFFTGPU_Finalize'),
                        # GI lighting (renderGIManager.cpp:451/1302/1342): bricks -> probes; m_sumGIVolumes has no caller.
                        ('m_giProbesFeature','m_giLitBrick','m_giLitBrickLights','m_giLitProbes','m_giLitProbes_separateSky'),
                        # F3 interim (regular-precision target baked as R11G11B10; browser runs -qualityLevel=Low).
                        # Combined blurs (renderNode_CombinedBlurs.cpp): downsample -> MB / WPB -> upsample (MBCS: subgroups).
                        ('m_postfx_combinedBlursDownsampleCS_Quality0','m_postfx_combinedBlursDownsampleCS_Quality1',
                         'm_postfx_combinedBlursDownsampleNoMBCS','m_postfx_combinedBlursMBCS_Quality0','m_postfx_combinedBlursMBCS_Quality1',
                         'm_postfx_combinedBlursWPBCS','m_postfx_combinedBlursUpsampleCS'),
                        # DOF custom CoC (renderNode_DepthOfField.cpp:667-702): horizontal then vertical combine.
                        ('m_postfxDofSeparableHorizontalPassCustomCOC','m_postfxDofSeparableVerticalCombinePassCustomCOC'),
                        # DOF radius4 (renderNode_DepthOfField.cpp:576-593, 679): downsample, horizontal and vertical near/far.
                        ('m_postfxDofSeparable_downsample','m_postfxDofSeparableHorizontalPass_radius4_nearplane',
                         'm_postfxDofSeparableHorizontalPass_radius4_farplane','m_postfxDofSeparableVerticalCombinePass_radius4_nearplane',
                         'm_postfxDofSeparableVerticalCombinePass_radius4_farplane'),
                        # --- Round 7 (PLAN F4/F5/F6): typed R32 UAV buffers lowered to array<u32>; hidden UAV counters at 448+u#.
                        # Auto-spawn on terrain (renderNode_PrepareAutoSpawnOnTerrain.cpp:113-127): offsets -> prefix sum -> instances.
                        ('m_onTerrainAutoSpawnInstanceBufferOffsets','m_onTerrainAutoSpawnInstanceBufferOffsets_multipleInstanceBuffers',
                         'm_onTerrainAutoSpawnInstanceBufferOffsetsPrefixSum','m_onTerrainAutoSpawnInstanceBufferOffsetsPrefixSum_multipleInstanceBuffers',
                         'm_onTerrainAutoSpawnInstanceBuffer','m_onTerrainAutoSpawnInstanceBuffer_multipleInstanceBuffers'),
                        # Decoupled particle lighting (particleDPLTexture.cpp:331-446): tiles count/sum/write, then lighting.
                        ('m_countParticleTiles','m_sumParticleTiles','m_writeParticleTiles','m_countLightsPerPixel','m_decoupledParticleLighting'),
                        # Dynamic decals (renderDecalMesh.cpp:553/570/635): vertex generation then the append-counter index pass.
                        ('m_shaderDynamicDecalGen','m_shaderDynamicDecalGen_Dismemberment','m_shaderDynamicDecalGen_ExtraData',
                         'm_shaderDynamicDecalGen_Dismemberment_ExtraData','m_shaderDynamicDecalGen_Skinned','m_shaderDynamicDecalGen_Skinned_Dismemberment',
                         'm_shaderDynamicDecalGen_Skinned_ExtraData','m_shaderDynamicDecalGen_Skinned_Dismemberment_ExtraData',
                         'm_shaderDynamicDecalGen_SkinnedExt','m_shaderDynamicDecalGen_SkinnedExt_Dismemberment',
                         'm_shaderDynamicDecalGen_SkinnedExt_ExtraData','m_shaderDynamicDecalGen_SkinnedExt_Dismemberment_ExtraData',
                         'm_shaderDynamicDecalGenIdxCS'),
                        # Volumetric clouds (renderNode_VolumetricClouds.cpp): visible tiles (append counter) -> integrate / shadowmap.
                        ('m_volumetricClouds_calcVisibleTiles','m_volumetricClouds_calcVisibleTiles_noDownsample','m_volumetricClouds_integrate',
                         'm_volumetricClouds_integrate_noDownsample','m_volumetricClouds_shadowmap','m_volumetricClouds_shadowmap_noDownsample'),
                        # --- Round 8: resolve distortion (renderGraphNodes.cpp:2238-2403): tile lists -> per-class distortedColor writes.
                        ('m_distortionBuildTilesList','m_distortion_00','m_distortion_01','m_distortion_10','m_distortion_11'),
                        # --- Round 9: U-probes (renderReflectionProbeManager.cpp): relight -> cube mips -> prefilter -> SH/luminance -> blend SH.
                        ('m_reflectionProbeRelight','m_reflectionProbeRelight_Fill','m_downsample_SquarePow2_Array','m_reflectionProbePrefilter',
                         'm_reflectionProbePrefilterConvolution','m_reflectionProbeSH','m_reflectionProbeSH_TopOnly','m_reflectionProbeNoSkySH',
                         'm_reflectionProbeNoSkySH_TopOnly','m_reflectionProbeLuminance','m_reflectionProbeBlendOverSH')]
def select_static_compute(static_manifest):
    import fnmatch
    static = json.loads(static_manifest.read_text())
    gpu_path = static_manifest.with_name('static-compute-conversion-gpu-validation.json')
    gpu = json.loads(gpu_path.read_text()) if gpu_path.exists() else {}
    proxy_path = static_manifest.with_name('static-compute-conversion-proxy-validation.json')
    proxy = json.loads(proxy_path.read_text()).get('Shaders', {}) if proxy_path.exists() else {}
    allow_path = static_manifest.with_name('static-compute-allowlist.txt')
    allow = [l.strip() for l in allow_path.read_text().splitlines() if l.strip() and not l.startswith('#')] if allow_path.exists() else ['*']
    selected = []
    for row in static['Shaders']:
        if not row.get('Complete') or not any(fnmatch.fnmatch(row['RuntimeName'], a) for a in allow): continue
        caps, invocations = compute_requirements(Path(row['WGSL']).read_text(encoding='utf-8'))
        if gpu.get(row['RuntimeName']) == 'OK' or proxy_covered(row, proxy, caps, invocations):
            selected.append(dict(row, RequiredCaps=caps, WorkgroupInvocations=invocations))
    for unit in STATIC_COMPUTE_UNITS:
        members = [row for row in selected if row['RuntimeName'] in unit]
        if not members: continue
        if len(members) != len(unit):
            selected = [row for row in selected if row['RuntimeName'] not in unit]
            continue
        caps = 0
        for row in members: caps |= row['RequiredCaps']
        for row in members: row['RequiredCaps'] = caps
    return selected
static_manifest=root/'build/shaders/static-compute-conversion.json'
if static_manifest.exists():
    manifest['Shaders'].extend(select_static_compute(static_manifest))
out = ['// Generated by generate-uniform-layouts.py; do not hand-edit.']
layouts = []
for shader in manifest['Shaders']:
    if not shader['Complete'] or not shader['WGSLUniformLayoutRoundtripVerified']:
        raise ValueError('Cannot publish an incomplete shader layout')
    if hashlib.sha256(Path(shader['WGSL']).read_bytes()).hexdigest() != shader['WGSLSHA256']:
        raise ValueError('Converted shader changed after layout validation')
    for layout in shader['UniformLayouts']:
        layout = hud_cpu_layout(root,shader,layout)
        ident = f"kFields_{shader['Name']}_{layout['Group']}_{layout['Binding']}"
        out.append(f'static const UniformScalarCopy {ident}[] = {{')
        occupied = set()
        for field in layout['Fields']:
            source, target, size = field['NativeOffset'], field['WebOffset'], field['Bytes']
            if source < 0 or target < 0 or size <= 0 or source+size > layout['NativeMinimumBytes'] or target+size > layout['WebBytes']:
                raise ValueError('Copy map exceeds buffer span')
            positions = set(range(target, target+size))
            if occupied & positions: raise ValueError('Overlapping destination fields')
            occupied |= positions
            out.append(f'    {{{source}, {target}, {size}}},')
        out.append('};')
        layouts.append(f'    {{{json.dumps(shader["Name"])}, {layout["Group"]}, {layout["Binding"]}, {layout["NativeMinimumBytes"]}, {layout["WebBytes"]}, {ident}, sizeof({ident}) / sizeof({ident}[0])}},')
out.extend(['static const UniformLayout kUniformLayouts[] = {', *layouts, '};', ''])
output = root / 'src/gpu-uniform-layouts.inc'
output.write_text('\n'.join(out), encoding='utf-8')
print(f'Generated {len(layouts)} original uniform layouts')
