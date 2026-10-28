"""Generate preset descriptors from the original platform-independent table."""
from pathlib import Path
import re, hashlib, json

root=Path(__file__).resolve().parent
source=Path('D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/src/common/gpuApi/src/gpuApiSamplerCommon.h')
data=source.read_bytes()
try: text=data.decode('utf-8-sig')
except UnicodeDecodeError: text=data.decode('cp1252')
rows=[]
for match in re.finditer(r'\bINIT_PRESET\(\s*(SAMPSTATEPRESET_\w+)\s*,([^;]+?)\)\s*;',text):
    rows.append((match[1],match[2].strip()))
for match in re.finditer(r'dd\.m_SamplerStatePresets\[(SAMPSTATEPRESET_\w+)\]\s*=\s*RequestSamplerState\(\s*Utils::BuildSamplerStateDesc\(([^;]+?)\)\s*\)\s*;',text):
    rows.append((match[1],match[2].strip()))
assert len({name for name,args in rows}) == len(rows) and len(rows)>20
lines=['// Generated from original gpuApiSamplerCommon.h; no preset remapping.',
       f'static_assert(SAMPSTATEPRESET_Max == {len(rows)}, "Original sampler table coverage changed");',
       'static bool BuildBrowserSamplerPreset(eSamplerStatePreset preset, SamplerStateDesc& out) {',
       '    switch (preset) {']
for name,args in rows:
    values=[v.strip() for v in args.split(',')]
    assert 4<=len(values)<=8
    values += ['COMP_None','false','0.f','false'][len(values)-4:]
    address,mini,mag,mip,comparison,point_z,bias,settings_bias=values
    lines += [f'    case {name}:',
        f'        out.addressU = out.addressV = out.addressW = {address};',
        f'        out.filterMin = {mini}; out.filterMag = {mag}; out.filterMip = {mip};',
        f'        out.comparisonFunction = {comparison}; out.pointZFilter = {point_z};',
        f'        out.mipLODBias = {bias}; out.allowSettingsBias = {settings_bias}; return true;']
lines += ['    default: return false;','    }','}','']
(root/'src/gpu-sampler-presets.inc').write_text('\n'.join(lines),encoding='utf-8')
(root/'build/webgpu/sampler-preset-provenance.json').write_text(json.dumps({'Source':str(source),
    'SourceSHA256':hashlib.sha256(data).hexdigest(),'Presets':len(rows),
    'Descriptors':[{'Name':name,'OriginalArguments':args} for name,args in rows]},indent=2),encoding='utf-8')
print(f'Generated {len(rows)} original sampler preset descriptors')
