"""Prove the exact native dilation shader's short CPU constant upload."""
import hashlib,re
from native_cbv_padding import verify_scalar_reads

def temporal_mask_source_limit(root,shader,resource,dxil,descriptor):
    if shader['Hash']!='16380013616743894990' or resource['OriginalSlot']!=6:
        return None
    if shader['Stage']!='pixel' or shader['SHA256']!='f0e76588a359fd5e260a538eddd79000a0bea942f13e2311080a4c9b4cceabd5':
        raise ValueError('Original temporal dilation shader identity changed')
    if not descriptor or descriptor['Bytes']!=228:
        raise ValueError('Original temporal dilation CBV declaration changed')
    cpp=root.parent/'Cyber punk/extracted/root/R6.Root/Mainline/dev/src/common/renderer/src/renderNode_AntyAliasing.cpp'
    text=cpp.read_text(encoding='utf-8-sig')
    start=text.index('// dilate invalidation mask')
    end=text.index('rctx.RTSwap( rtTemporalInvalidationMask',start)
    block=text[start:end]
    if not re.search(r'struct PIX\s*\{\s*math::Vector4\s+currResolution;\s*\}\s*pix;',block):
        raise ValueError('Original dilation CPU upload no longer one Vector4')
    if 'rctx.SetConstant<ePIXEL>( pix );' not in block or 'SHADER_NAME( "m_dilateTemporalInvalidationMask" )' not in block:
        raise ValueError('Original dilation shader/upload pairing changed')
    reads=verify_scalar_reads(dxil,descriptor['RangeID'],16)
    return 16,reads,{'OriginalCPUBytes':16,'NativeDXILDeclaredBytes':228,
        'ActualNativeScalarReads':reads,'CPPSourceSHA256':hashlib.sha256(cpp.read_bytes()).hexdigest(),
        'CPUBlockSHA256':hashlib.sha256(block.encode()).hexdigest(),
        'DXILSHA256':hashlib.sha256(dxil.encode()).hexdigest(),
        'Scope':'Exact original shader statically reads only the supplied Vector4; omit unread trailing fields from CPU repacking, preserve original WGSL and native bytecode.'}
