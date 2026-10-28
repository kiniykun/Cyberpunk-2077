"""Exact source ABI bridge for the original screen-presentation constant struct."""
from pathlib import Path
import hashlib,re
def present_cpu_source_map(root,shader,resource,dxil,fields,native,native_key):
    if shader['Hash']!='18217308226223964159' or shader['Stage']!='pixel' or resource['OriginalSlot']!=6:return None
    depot=root.parent/'cp2077SourceCode/extracted/root/R6.Root/Mainline'
    cpp=depot/'dev/src/common/renderer/src/renderPostProcess.cpp';hlsl=depot/'engine/shaders/postfx_present.fx'
    cpp_text=cpp.read_text(encoding='utf-8-sig');hlsl_text=hlsl.read_text(encoding='utf-8-sig')
    declaration=r'struct PixelShaderData\s*\{\s*Uint32 hdrMode;\s*float luminanceScale;\s*float luminanceBias;\s*float gamma;\s*Vector4 colorGamutTransform\[3\];'
    if not re.search(declaration,cpp_text):raise ValueError('Original presentation CPU struct changed')
    if 'rctx.SetConstant<ePIXEL>( pixelShaderData );' not in cpp_text:raise ValueError('Original presentation upload contract changed')
    if 'float3x4 cbColorGamutTransform;' not in hlsl_text:raise ValueError('Original presentation shader matrix changed')
    if '%dx.alignment.legacy.PIX = type { i32, float, float, float, [4 x <3 x float>] }' not in dxil:raise ValueError('Native presentation matrix storage changed')
    source_offsets={0:0,4:4,8:8,12:12}
    for column in range(4):
        for row in range(3):source_offsets[16+column*16+row*4]=16+row*16+column*4
    mapped={}
    for name in fields:
        offset,width=native[native_key][name]
        if offset in source_offsets:
            if width!=4:raise ValueError('Presentation matrix scalar width changed')
            mapped[name]=(source_offsets[offset],4)
    if len(mapped)!=16 or {v[0] for v in mapped.values()}!=set(range(0,64,4)):raise ValueError('Presentation source mapping must retain all 16 scalar values exactly once')
    proof={'Hash':shader['Hash'],'OriginalCPUBytes':64,'NativeDXILDeclaredBytes':76,'TranslatedUniformBytes':80,'MatrixRows':3,'MatrixColumns':4,'CPPSourceSHA256':hashlib.sha256(cpp.read_bytes()).hexdigest(),'HLSLSourceSHA256':hashlib.sha256(hlsl.read_bytes()).hexdigest(),'Mapping':'CPU Vector4 rows to native DXIL padded columns; preserve all 12 matrix scalar values and the four header values'}
    return mapped,proof
