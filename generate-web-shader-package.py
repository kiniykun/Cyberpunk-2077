"""Embed validated converted original shaders for the WebGPU runtime."""
from pathlib import Path
import hashlib
import json
import re
import struct
import subprocess
from sampler_bias_lowering import lower_sampling_bias, directive_prefix_length
from texture_load_lowering import lower_unfilterable_srvs
from stencil_component_lowering import lower_stencil_reads
import os

def storage_layouts(path):
    words = struct.unpack('<'+'I'*(Path(path).stat().st_size//4), Path(path).read_bytes())
    types, decorations, members, variables = {}, {}, {}, []
    cursor = 5
    while cursor < len(words):
        word = words[cursor]
        count, opcode = word >> 16, word & 65535
        args = words[cursor+1:cursor+count]
        if not count: raise ValueError('Invalid SPIR-V instruction')
        if opcode in (21,22,23,29,30,32): types[args[0]] = (opcode,args[1:])
        elif opcode == 71: decorations.setdefault(args[0],{})[args[1]] = args[2:]
        elif opcode == 72: members.setdefault((args[0],args[1]),{})[args[2]] = args[3:]
        elif opcode == 59: variables.append(args)
        cursor += count
    result = {}
    for pointer, variable, *_ in variables:
        _, (_, root) = types[pointer]
        op, fields = types.get(root,(0,()))
        if op != 30 or len(fields) != 1: continue
        array_type = fields[0]
        op, array = types.get(array_type,(0,()))
        if op != 29: continue
        element = array[0]
        op, fields = types[element]
        if op == 23:
            # Vector element (e.g. uvec2/uvec4 raw word views): 32-bit components packed at 4-byte steps.
            scalar_op, scalar = types[fields[0]]
            if scalar_op not in (21,22) or scalar[0] != 32: raise ValueError('Vector storage element width unsupported')
            deco=decorations[variable]
            result[(deco[34][0],deco[33][0])]=(decorations[array_type][6][0],[(4*i,scalar_op,scalar) for i in range(fields[1])])
            continue
        if op in (21,22):
            if fields[0]!=32:raise ValueError('Raw storage scalar width unsupported')
            deco=decorations[variable]
            result[(deco[34][0],deco[33][0])]=(decorations[array_type][6][0],[(0,op,fields)])
            continue
        if op != 30: raise ValueError('Unimplemented storage element layout')
        layout = []
        for index, field in enumerate(fields):
            scalar_op, scalar = types[field]
            if scalar_op not in (21,22) or scalar[0] != 32:
                raise ValueError('Storage layout requires additional translation')
            layout.append((members[element,index][35][0], scalar_op, scalar))
        deco = decorations[variable]
        result[(deco[34][0],deco[33][0])] = (decorations[array_type][6][0], layout)
    return result

root = Path(__file__).resolve().parent
manifest = json.loads((root/'build/shaders/ui-shader-conversion.json').read_text(encoding='utf-8'))
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
def naga_input(path, text):
    # naga 30's WGSL frontend rejects 'enable subgroups' but accepts the subgroup builtins without it (the converter
    # validated these modules before adding the directive). Check a copy without the directive; ship the original.
    # The same holds for the Tint-only 'diagnostic(off, subgroup_uniformity)' directive added below.
    if not re.search(r'^\s*(?:enable\s+subgroups|diagnostic\s*\(\s*off\s*,\s*subgroup_uniformity\s*\))\s*;', text, re.M): return str(path)
    copy = path.with_name(path.stem + '-naga-input.wgsl')
    stripped = re.sub(r'^\s*enable\s+subgroups\s*;[ \t]*\r?\n', '', text, flags=re.M)
    stripped = re.sub(r'^[ \t]*diagnostic\s*\(\s*off\s*,\s*subgroup_uniformity\s*\)\s*;[ \t]*\r?\n', '', stripped, flags=re.M)
    copy.write_text(stripped, newline='')
    return str(copy)
def disable_subgroup_uniformity(text, sampling_calls):
    # Tint enforces subgroup_uniformity (quad*/subgroup* builtins only in subgroup-uniform control flow); the native
    # D3D shaders run these ops in divergent flow (e.g. a quad-aligned 'lane < 4' branch, per-light
    # WaveReadLaneFirst loops). Turning the diagnostic off keeps exactly the native semantics. Applied to modules
    # that call quad ops, and to subgroup modules whose 'enable subgroups' was previously buried below the sampler
    # bias declarations (Chrome never reached its uniformity analysis); modules Tint already accepts stay unchanged.
    if not re.search(r'^\s*enable\s+subgroups\s*;', text, re.M): return text
    if re.search(r'^[ \t]*diagnostic\s*\(\s*off\s*,\s*subgroup_uniformity\s*\)', text, re.M): return text
    if not (re.search(r'\bquad\w+\s*\(', text) or sampling_calls): return text
    at = directive_prefix_length(text)
    if not at: raise ValueError('subgroup module without a leading directive block')
    newline = '\r\n' if text[:at].endswith('\r\n') else '\n'
    return text[:at] + 'diagnostic(off, subgroup_uniformity);' + newline + text[at:]
def assert_directives_first(text):
    # Every enable/requires/diagnostic directive must come before the first global declaration.
    at = directive_prefix_length(text)
    if re.search(r'^[ \t]*(?:enable[ \t]+\w|requires[ \t]+\w|diagnostic[ \t]*\()', text[at:], re.M):
        raise ValueError('WGSL directive after a global declaration')
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
lines = ['// Generated from validated original shader conversions.']
entries = []
entry_point_audits = []
for index, shader in enumerate(manifest['Shaders']):
    if not shader['Complete'] or not shader['WGSLUniformLayoutRoundtripVerified']:
        raise ValueError('Incomplete original shader conversion')
    data = Path(shader['WGSL']).read_bytes()
    if hashlib.sha256(data).hexdigest() != shader['WGSLSHA256']:
        raise ValueError('WGSL hash differs from validated layout')
    text = data.decode('utf-8')
    native_path = root/'build/shaders'/f'{shader["Name"]}-native.spv'
    if hashlib.sha256(native_path.read_bytes()).hexdigest() != shader['NativeSPIRVSHA256']:
        raise ValueError('Native shader layout hash changed')
    native_storage = storage_layouts(native_path)
    web_storage = {}
    if native_storage:
        # Re-derive storage layout from the exact hash-verified WGSL being
        # packaged, rather than trusting an older audit file on disk.
        audit_path = root/'build/shaders'/f'{shader["Name"]}-storage-layout-audit.spv'
        subprocess.run([str(root/'tools/naga-30.0.1/bin/naga.exe'), '-g',
                        naga_input(Path(shader['WGSL']), text), str(audit_path)], check=True, capture_output=True)
        web_storage = storage_layouts(audit_path)
    resources = []
    bias_resources=[]
    for group, binding, address, name, typename in re.findall(
        r'@group\((\d+)\)\s+@binding\((\d+)\)\s+var(?:<([^>]+)>)?\s+(\w+)\s*:\s*([^;]+);', text):
        group, binding = int(group), int(binding)
        if address == 'uniform':
            kind, slot = ('RawUniform' if binding == 6 else 'OtherUniform'), binding
        elif address.startswith('storage'):
            if binding >= 448:
                # convert-static-compute.py COUNTER_BINDING_BASE: hidden UAV counter of RW buffer u(binding-448).
                if address != 'storage, read_write': raise ValueError('UAV counter must be read_write')
                kind, slot = 'StorageBufferCounter', binding - 448
                if slot >= 15: raise ValueError('UAV counter slot outside native binding range')
            elif binding >= 288:
                if address not in ('storage', 'storage, read', 'storage, read_write'):
                    raise ValueError('Unsupported storage-buffer access')
                kind, slot = 'StorageBufferUAV', binding - 288
                if slot >= 15: raise ValueError('UAV slot outside native binding range')
            else:
                if address not in ('storage', 'storage, read'):
                    raise ValueError('Writable buffer declared in an SRV register')
                kind, slot = 'StorageBuffer', binding - 64
        elif typename.startswith('texture_storage_'):
            kind, slot = 'StorageTexture', binding - 288
        elif typename.startswith('texture_'):
            kind, slot = 'SampledTexture', binding - 64
        elif typename in ('sampler', 'sampler_comparison'):
            kind, slot = 'Sampler', binding - 256
            bias_resources.append({'Group':group,'Binding':binding,'OriginalSlot':slot,'Kind':'Sampler'})
        else:
            raise ValueError(f'Unsupported resource declaration: {name}: {typename}')
        if group != 0 or slot < 0:
            raise ValueError('Unexpected register mapping')
        stride = 0
        if kind in ('StorageBuffer', 'StorageBufferUAV', 'StorageBufferCounter'):
            native = native_storage.get((group,binding))
            web = web_storage.get((group,binding))
            if not native or native != web:
                raise ValueError('Native and WGSL storage buffer layout differ')
            stride = native[0]
            if kind == 'StorageBufferCounter' and native != (4, [(0, 21, (32, 0))]):
                raise ValueError('UAV counter is not a u32 word array')
        resources.append(f'    {{{group}, {binding}, {slot}, ComputeResourceKind::{kind}, {stride}}},')
    if len(resources) != text.count('@binding('):
        raise ValueError('Incomplete shader resource reflection')
    if shader['Name'] == 'hud':
        # Runtime null-SRV emulation is valid only for this proven read shape.
        # Reject a future shader that observes buffer length or texture size.
        exposure_uses = re.findall(r'\bt_FrameExposure\b', text)
        if (len(exposure_uses) != 2 or
            't_FrameExposure.member[0u].uiLuminanceScale' not in text or
            'arrayLength(' in text or 'textureDimensions(' in text or
            '    {0, 73, 9, ComputeResourceKind::StorageBuffer, 28},' not in resources):
            raise ValueError('HUD null SRV compatibility read-shape proof no longer holds')
        (root/'build/shaders/hud-null-srv-read-shape-proof.json').write_text(json.dumps({
            'OriginalWGSLSHA256': shader['WGSLSHA256'],
            'ExposureBinding': 73, 'ExposureSlot': 9, 'ExposureStride': 28,
            'ExposureRead': 'member[0u].uiLuminanceScale',
            'BufferLengthQueries': False, 'TextureSizeQueries': False,
            'Scope': 'Null read compatibility only; never substitute non-null resources'
        }, indent=2))
    # Point-sampled depth/R32F SRVs that the engine binds with unfilterable views -> textureLoad (texture_load_lowering.py).
    if os.environ.get('CP2077_SKIP_UNFILTERABLE_LOWERING') != '1':
        text,_=lower_unfilterable_srvs(text,shader['RuntimeName'])
    # Stencil SRVs (DXIL Texture2D<uint2>, t# at binding 64+t#): WebGPU returns stencil in .x, HLSL reads .y.
    text,_=lower_stencil_reads(text,shader.get('OriginalBytecodePath'),64)
    text,sampling_calls=lower_sampling_bias(text,bias_resources,999)
    text=disable_subgroup_uniformity(text,sampling_calls)
    assert_directives_first(text)
    if sampling_calls:
        resources.append('    {0, 999, 0, ComputeResourceKind::SamplerBias, 0},')
    lowered_path=root/'build/shaders'/f'{shader["Name"]}-sampler-bias.wgsl'
    lowered_path.write_text(text)
    subprocess.run([str(root/'tools/naga-30.0.1/bin/naga.exe'),naga_input(lowered_path,text)],check=True,capture_output=True)
    # Naga may legalize the SPIR-V entry name (for example cs_main_ to
    # cs_main). The pipeline must use the entry in this exact validated WGSL,
    # while the original compiler manifest remains unchanged.
    compute_entries = re.findall(r'@compute\s+@workgroup_size\([^)]*\)\s+fn\s+([A-Za-z_]\w*)\s*\(', text)
    if len(compute_entries) != 1:
        raise ValueError('Expected exactly one validated WGSL compute entry')
    runtime_entry = compute_entries[0]
    data=text.encode('utf-8')
    lines.extend([f'static const ComputeResourceBinding kResources_{index}[] = {{', *resources, '};'])
    delimiter = 'CP2077WGSL'
    if ')'+delimiter+'"' in text: raise ValueError('Raw string delimiter collision')
    lines.append(f'static const char kWGSL_{index}[] = R"{delimiter}({text}){delimiter}";')
    lowered_hash=hashlib.sha256(data).hexdigest()
    if shader.get('OriginalBytecodePath'):
        # Static-cache programs: the engine passes the original DXIL container, so match those exact bytes.
        identity = Path(shader['OriginalBytecodePath']).read_bytes()
        if hashlib.sha256(identity).hexdigest() != shader['OriginalBytecodeSHA256']: raise ValueError('Original DXIL changed')
        lines.append(f'static const char kIdentity_{index}[] = {{'+','.join(str(b if b < 128 else b - 256) for b in identity)+',0};')
    else:
        identity = b'CPWGSL01' + lowered_hash.encode('ascii') + shader['RuntimeName'].encode('utf-8')
        lines.append(f'static const char kIdentity_{index}[] = '+json.dumps(identity.decode('ascii'))+';')
    (root/'build/shaders'/f'{shader["Name"]}-web.bin').write_bytes(identity)
    entry_point_audits.append({'Name':shader['Name'],'OriginalCompilerEntryPoint':shader['EntryPoint'],
                               'ValidatedWGSLEntryPoint':runtime_entry,'LoweredWGSLSHA256':lowered_hash})
    entries.append('    {'+', '.join([json.dumps(shader['Name']), json.dumps(shader['RuntimeName']), json.dumps(runtime_entry),
                                     json.dumps(lowered_hash), f'kWGSL_{index}',
                                     f'sizeof(kWGSL_{index}) - 1', f'kIdentity_{index}',
                                     f'sizeof(kIdentity_{index}) - 1',
                                     'true' if (shader.get('StorageOutputSpecialization') or {}).get('RequiresDeviceFeature') == 'texture-formats-tier1' else 'false',
                                     'true' if 'read_write>' in text else 'false',
                                     f'kResources_{index}', f'sizeof(kResources_{index}) / sizeof(kResources_{index}[0])', 'true',
                                     f'{shader.get("RequiredCaps", 0)}u', f'{shader.get("WorkgroupInvocations", 0)}u'])+'},')
lines.extend(['static const ConvertedShader kConvertedShaders[] = {', *entries, '};', ''])
(root/'src/gpu-shader-package.inc').write_text('\n'.join(lines), encoding='utf-8')
(root/'build/shaders/compute-wgsl-entry-point-audit.json').write_text(json.dumps(entry_point_audits,indent=2))
print(f'Packaged {len(entries)} original compute shaders')
