"""Preserve audited original conditional implicit-gradient fragment shaders."""
from pathlib import Path
import hashlib,json

def preserve_conditional_sampling(shader,text,root):
    policy=json.loads((root/'build/shaders/static-derivative-compat-policy.json').read_text())
    # [whitewall] A few engine hashes exist in two packages with the same bytecode but differently prepared WGSL (e.g. 9437428442297698678
    # in ui and material). Key the policy on hash + exact original bytecode + manifest WGSL hash (ui and material prepare that hash
    # differently); the on-disk WGSL file and DXIL reflection are still verified below.
    candidates=[p for p in policy if p['Hash']==shader['Hash']]
    match=next((p for p in candidates if p['OriginalBytecodeSHA256']==shader['SHA256'] and p['OriginalWGSLSHA256']==shader.get('WGSLSHA256')),None)
    if not match:return text
    if (shader['Stage']!='pixel' or shader['SHA256']!=match['OriginalBytecodeSHA256'] or
        shader['WGSLSHA256']!=match['OriginalWGSLSHA256'] or
        hashlib.sha256(Path(shader['WGSL']).read_bytes()).hexdigest()!=match['OriginalWGSLSHA256'] or
        hashlib.sha256(Path(match['DXILReflection']).read_text().encode()).hexdigest()!=match['DXILReflectionSHA256']):
        raise ValueError('Conditional-gradient source evidence changed')
    marker='fn main_1() {'
    if text.count(marker)!=1:raise ValueError('Original sampling helper changed')
    directive='@diagnostic(warning, derivative_uniformity)\n'
    translated=text.replace(marker,directive+marker)
    if translated.replace(directive,'',1)!=text:raise ValueError('Shader instructions changed')
    return translated
