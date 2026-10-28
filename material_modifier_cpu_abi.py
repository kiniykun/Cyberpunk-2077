"""Audit the native material-modifier CPU/shader array-size mismatch."""
from pathlib import Path
import hashlib,re
from native_cbv_padding import verify_scalar_reads

def material_modifier_source_limit(root,resource,dxil,descriptor):
    if resource['OriginalSlot']!=7 or not descriptor:return None
    if '%MaterialModifiersConsts = type { [26 x <4 x float>] }' not in dxil:return None
    depot=root.parent/'Cyber punk/extracted/root/R6.Root/Mainline'
    cpp=depot/'dev/src/common/renderer/src/renderMaterialModifiers.h'
    state=depot/'dev/src/common/renderer/src/renderStateManager.cpp'
    hlsl=depot/'engine/shaders/include/materialModifiersConstantBuffers.fx'
    cpp_text=cpp.read_text(encoding='utf-8-sig')
    state_text=state.read_text(encoding='utf-8-sig')
    hlsl_text=hlsl.read_text(encoding='utf-8-sig')
    if not re.search(r'NumVectors\s*=\s*18\s*\+\s*3\s*;',cpp_text):raise ValueError('Original modifier CPU array changed')
    if 'return NumVectors*sizeof(math::Vector4);' not in cpp_text or 'GpuApi::UpdateBuffer( m_targetBuffer, m_data.DataSize(), m_data.Data() );' not in cpp_text:raise ValueError('Original modifier upload changed')
    if 'desc.size = MaterialModifierConstants::BufferSize();' not in state_text:raise ValueError('Original modifier buffer allocation changed')
    if 'START_CB( MaterialModifiersConsts, 7 )' not in hlsl_text or '#define MATERIAL_MODIFIER_CONSTANTS_LAST_INDEX 26' not in hlsl_text:raise ValueError('Original modifier shader array changed')
    if descriptor['Bytes']!=416:raise ValueError('Native modifier declaration changed')
    try:reads=verify_scalar_reads(dxil,descriptor['RangeID'],descriptor['Bytes'])
    except ValueError:return None
    cpu_bytes=21*16
    if any(offset+width>cpu_bytes for offset,width in reads):return None
    return cpu_bytes,reads,{'OriginalCPUBytes':cpu_bytes,'NativeDXILDeclaredBytes':416,'ActualNativeScalarReads':reads,'CPPSourceSHA256':hashlib.sha256(cpp.read_bytes()).hexdigest(),'AllocationSourceSHA256':hashlib.sha256(state.read_bytes()).hexdigest(),'HLSLSourceSHA256':hashlib.sha256(hlsl.read_bytes()).hexdigest(),'Scope':'Original CPU uses 21 float4s; only shaders whose complete static native reads remain inside that array may omit the five unread trailing shader vectors'}
