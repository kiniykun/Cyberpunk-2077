"""Use compiler-proved original HUD CPU offsets for the shader copy table."""
from pathlib import Path
import hashlib,json

def hud_cpu_layout(root,shader,layout):
    if shader['Name']!='hud' or layout['Group']!=0 or layout['Binding']!=6:return layout
    proof=json.loads((root/'build/shaders/hud-cpu-uniform-abi-proof.json').read_text())
    if not proof['NativeCompilerLayoutAssertionsPassed']:raise ValueError('Original HUD C++ layout not proved')
    for path,digest in proof['SourceHashes'].items():
        if hashlib.sha256(Path(path).read_bytes()).hexdigest()!=digest:
            raise ValueError('Original HUD source changed; recompute actual C++ layout')
    if proof['OriginalWGSLSHA256']!=shader['WGSLSHA256'] or proof['OriginalNativeSPIRVSHA256']!=shader['NativeSPIRVSHA256']:
        raise ValueError('Original HUD shader changed after C++ layout proof')
    if layout['NativeMinimumBytes']!=proof['OriginalDeclaredBytes'] or layout['WebBytes']!=proof['WebBytes']:
        raise ValueError('Original HUD declared layout changed')
    shape=lambda fs:[(f['Path'],f['IndexedPath'],f['WebOffset'],f['Bytes']) for f in fs]
    if shape(layout['Fields'])!=shape(proof['Fields']):raise ValueError('Original HUD scalar destination map changed')
    if len(proof['Fields'])!=76 or {f['NativeOffset'] for f in proof['Fields']}!=set(range(0,304,4)):
        raise ValueError('Original HUD CPU block must retain all76 scalar bits exactly once')
    return {**layout,'NativeMinimumBytes':proof['OriginalCPUBytes'],'Fields':proof['Fields']}
