"""Embed verified original UI bytecode identities and prepared WGSL modules."""
from pathlib import Path
import hashlib
import json
import re
root=Path(__file__).resolve().parent
d=json.loads((root/'build/shaders/native-ui-binding-preparation.json').read_text())
lines=['// Generated from original DXIL and validated register/layout audits.']
entries=[]
for shader in d['Shaders']:
    if not shader['RegisterMappingVerified'] or not shader['UniformRoundtripVerified']:
        continue
    blob=Path(shader['Path']).read_bytes()
    wgsl=Path(shader['WGSL']).read_bytes()
    assert hashlib.sha256(blob).hexdigest()==shader['SHA256']
    assert hashlib.sha256(wgsl).hexdigest()==shader['WGSLSHA256']
    assert shader['Stage'] in ('vertex','pixel')
    i=len(entries)
    lines.append(f'static const unsigned char kGraphicsDXIL_{i}[] = {{')
    for start in range(0,len(blob),32):
        lines.append(','.join(str(v) for v in blob[start:start+32])+',')
    lines.append('};')
    assert shader['VertexInputLocationsVerified']
    inputs=shader['VertexInputs']
    if inputs:
        lines.append(f'static const GraphicsVertexInput kGraphicsInputs_{i}[] = {{')
        for value in inputs:
            lines.append('    {'+', '.join([json.dumps(value['Semantic']),str(value['Index']),
                         str(value['Location']),str(value['Components'])])+'},')
        lines.append('};')
    text=wgsl.decode('utf-8'); assert ')CPUIWGSL"' not in text
    from static_derivative_compat import preserve_conditional_sampling
    text=preserve_conditional_sampling(shader,text,root)
    # Stencil SRVs (DXIL Texture2D<uint2>, t# at binding 576+t#): WebGPU returns stencil in .x, HLSL reads .y.
    from stencil_component_lowering import lower_stencil_reads
    text,_=lower_stencil_reads(text,shader['Path'],576)
    runtime_wgsl_hash=hashlib.sha256(text.encode('utf-8')).hexdigest()
    lines.append(f'static const char kGraphicsWGSL_{i}[] = R"CPUIWGSL({text})CPUIWGSL";')
    lines.append(f'static const GraphicsResourceBinding kGraphicsResources_{i}[] = {{')
    for resource in shader['Resources']:
        if resource.get('UnusedAfterLowering'):
            assert resource['Kind']=='StorageBuffer'
            assert any(a['Binding']==resource['Binding'] and a.get('UnusedAfterLowering') and not a['Accesses'] for a in shader['StorageWordAddressAudits'])
            continue
        legacy_mask = 0
        legacy_verified = False
        if resource['Kind'] == 'ConstantBuffer' and resource['OriginalSlot'] in (3,5):
            declaration = re.search(r'@group\('+str(resource['Group'])+r'\)\s+@binding\('+str(resource['Binding'])+r'\)\s+var<uniform>\s+(\w+)\s*:\s*(\w+)\s*;', text)
            if declaration:
                variable, typename = declaration.groups()
                structure = re.search(r'\bstruct\s+'+re.escape(typename)+r'\s*\{\s*(\w+)\s*:\s*array<vec4<f32>,\s*(\d+)>\s*,?\s*\}', text)
                if structure:
                    member, count = structure.groups()
                    accesses = re.findall(r'\b'+re.escape(variable)+r'\.'+re.escape(member)+r'\[(\d+)u\]', text)
                    # Any alias, whole-block use or dynamic access invalidates
                    # this proof and retains the conservative runtime check.
                    if len(re.findall(r'\b'+re.escape(variable)+r'\b',text)) == len(accesses)+1:
                        assert int(count) <= 64
                        for index in accesses:
                            assert int(index) < int(count)
                            legacy_mask |= 1 << int(index)
                        legacy_verified = True
        lines.append('    {'+', '.join([str(resource['Group']),str(resource['Binding']),
                    str(resource['OriginalSlot']), 'GraphicsResourceKind::'+resource.get('RuntimeKind',resource['Kind']),
                    str(resource.get('WebGPUMinBytes',0)),str(legacy_mask)+'ull',
                    'true' if legacy_verified else 'false',str(resource.get('StorageStride',0))])+'},')
    lines.append('};')
    entries.append('    {'+', '.join([json.dumps(shader['Hash']),
                   'GraphicsShaderStage::'+('Vertex' if shader['Stage']=='vertex' else 'Pixel'),
                   json.dumps(runtime_wgsl_hash),f'kGraphicsDXIL_{i}',f'sizeof(kGraphicsDXIL_{i})',
                   f'kGraphicsWGSL_{i}',f'sizeof(kGraphicsWGSL_{i})-1',
                   f'kGraphicsResources_{i}',f'sizeof(kGraphicsResources_{i})/sizeof(kGraphicsResources_{i}[0])',
                   f'kGraphicsInputs_{i}' if inputs else 'nullptr', str(len(inputs)),
                   'true' if shader.get('SamplerBiasLowered') else 'false'])+'},')
lines+=['static const ConvertedGraphicsShader kConvertedGraphicsShaders[] = {',*entries,'};']
(root/'src/gpu-ui-graphics-package.inc').write_text('\n'.join(lines),encoding='utf-8')
print(f'Packaged {len(entries)} original UI graphics shaders with exact native bytecode lookup')
