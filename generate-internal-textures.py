from pathlib import Path
import json,hashlib
root=Path(r'D:\cp2077 port')
p=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\gpuApi\src\gpuApiImplInternalTextures.inl')
s=p.read_text()
assert s.count('const Uint32 baseIdx = z + slicePitch + y * pitch + x * bytesPerPixel;')==1
s=s.replace('#include "gpuApiTextureUtils.h"','#include "common/gpuApi/src/gpuApiTextureUtils.h"')
s=s.replace('const Uint32 baseIdx = z + slicePitch + y * pitch + x * bytesPerPixel;','const Uint32 baseIdx = z * slicePitch + y * pitch + x * bytesPerPixel; // Repair original volume slice offset.')
(root/'src/gpu-internal-textures-native.inc').write_text('// Original texture generators; only include routing and volume slice-offset repair differ.\n'+s)
(root/'build/internal-texture-generator-provenance.json').write_text(json.dumps({'Original':str(p),'OriginalSHA256':hashlib.sha256(p.read_bytes()).hexdigest(),'GeneratedSHA256':hashlib.sha256((root/'src/gpu-internal-textures-native.inc').read_bytes()).hexdigest(),'Adaptations':['Route relative header include','Map original device storage accessor/type through scoped macros','Repair original 3D baseIdx from z + slicePitch to z * slicePitch; preserves declared blank RGBA value across all slices']},indent=2))
