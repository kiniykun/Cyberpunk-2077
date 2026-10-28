"""Normalize native string-view addresses in the installed SDK's JS helpers.

Some Emdawn debug-marker imports pass Wasm64 BigInt addresses straight to
helpers whose generated heap indexing uses Number arithmetic. Keep the original
string view, label and GPU operation; only normalize a representable address.
"""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent
path=root/'build/game/cp2077.js'
text=path.read_text()
for name in ('makeStringFromStringView','makeStringFromOptionalStringView'):
    marker=f'  {name}: stringViewPtr => {{\n'
    normalization='    if (typeof stringViewPtr === "bigint") {\n      if (stringViewPtr < 0n || stringViewPtr > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError("WebGPU string-view address is not representable");\n      stringViewPtr = Number(stringViewPtr);\n    }\n'
    if marker+normalization in text: continue
    if text.count(marker)!=1: raise RuntimeError('Installed SDK string-view helper changed: '+name)
    text=text.replace(marker,marker+normalization,1)
timestamp_marker='  makePassTimestampWrites: ptr => {\n'
start=text.index(timestamp_marker)
end=text.index('  makePipelineConstants:',start)
helper=text[start:end]
sentinel_fix='    if (writes.beginningOfPassWriteIndex === 4294967295) delete writes.beginningOfPassWriteIndex;\n    if (writes.endOfPassWriteIndex === 4294967295) delete writes.endOfPassWriteIndex;\n    return writes;\n'
if sentinel_fix not in helper:
    if helper.count('    return {\n')!=1 or helper.count('    };\n')!=1:
        raise RuntimeError('Installed SDK timestamp descriptor helper changed')
    helper=helper.replace('    return {\n','    const writes = {\n',1)
    helper=helper.replace('    };\n','    };\n'+sentinel_fix,1)
    text=text[:start]+helper+text[end:]
path.write_text(text)
(root/'build/webgpu-memory64-string-view-result.json').write_text(json.dumps({'GeneratedJavaScriptSHA256':hashlib.sha256(path.read_bytes()).hexdigest(),'Scope':'Actual WGPU string-view addresses normalized; native undefined timestamp indices omitted from optional browser descriptor fields; actual requested indices, labels and GPU calls preserved.','OriginalMenusVerified':False},indent=2))
print('Normalized actual Wasm64 WGPU string-view helper addresses; original GPU labels/calls preserved.')
