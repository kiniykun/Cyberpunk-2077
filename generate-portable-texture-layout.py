"""Select the unchanged PC cooked-mip layout helper for original PC assets."""
from pathlib import Path
import hashlib
import json
import re

root = Path(__file__).resolve().parent
original = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\gpuApi\src')
texture = original / 'dx12/gpuApiDX12Texture.cpp'
common = original / 'gpuApiCommon.cpp'
buffer = original / 'dx12/gpuApiDX12Buffer.cpp'
match = re.search(r'(CookedTextureMipPlacement CalculateCookedTextureMipChainOffsetAndSize\([^\n]+\)\s*\{)\s*#ifdef RED_PLATFORM_WINPC\s*(.*?)#endif',
                  texture.read_text(), re.S)
if not match or 'const Uint32 mipCount' not in match[2] or 'result.size = totalSize - mipOffset[mip]' not in match[2]:
    raise RuntimeError('Original PC cooked mip implementation was not identified')
bias = re.findall(r'^GPUAPI_API Float GRenderSettingsMipBias = 0\.f;', common.read_text(), re.M)
if len(bias) != 1:
    raise RuntimeError('Original renderer bias declaration was not identified')
body = bias[0] + '\nnamespace GpuApi {\n' + match[1] + '\n' + match[2] + '\n}\n}\n'
slot_bodies = []
for source, names in [(buffer, ['LockBuffersSlots', 'UnlockBuffersSlots']),
                      (texture, ['LockTexturesSlots', 'UnlockTexturesSlots'])]:
    for name in names:
        definitions = re.findall(r'\tvoid ' + name + r'\([^\n]*\)\s*\n\t\{.*?\n\t\}', source.read_text(), re.S)
        if len(definitions) != 1 or '#ifndef RED_CONFIGURATION_FINAL' not in definitions[0]:
            raise RuntimeError('Original Final/debug slot-lock definition differs: ' + name)
        slot_bodies.append(definitions[0])
body += '\n#ifndef RED_CONFIGURATION_FINAL\n#error Original slot-lock selection requires the Final configuration\n#endif\n'
body += 'namespace GpuApi {\n' + '\n'.join(slot_bodies) + '\n}\n'
output = root / 'src/gpu-portable-texture-layout.inc'
output.write_text('// Generated from unchanged native PC layout and common renderer declarations.\n' + body)
report = dict(OriginalSources={str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in [texture, common, buffer]},
              OutputSHA256=hashlib.sha256(output.read_bytes()).hexdigest(),
              AssetLayout='Original PC linear cooked texture mip-chain placement',
              OriginalBodyRetained=True, OriginalFinalSlotLockDefinitionsRetained=True, RuntimeVerified=False)
(root / 'build/webgpu/portable-texture-layout-provenance.json').write_text(json.dumps(report, indent=2))
print('Original PC cooked mip placement and renderer bias selected unchanged')
