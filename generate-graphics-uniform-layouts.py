"""Publish original graphics uniform byte maps after rechecking shader artifacts."""
from pathlib import Path
import ast, hashlib, json, struct

root = Path(__file__).resolve().parent
tree = ast.parse((root/'compile-ui-shaders.py').read_text())
function = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'reflect')
namespace = {'Path':Path, 'struct':struct}
exec(compile(ast.Module(body=[function],type_ignores=[]),'<uniform-reflector>','exec'),namespace)
reflect = namespace['reflect']
manifest = json.loads((root/'build/shaders/native-ui-binding-preparation.json').read_text())
lines = ['// Generated original graphics uniform maps; artifact identity and scalar offsets rechecked.']
layouts = []
records = []
for shader in manifest['Shaders']:
    if not shader['RegisterMappingVerified'] or not shader['UniformRoundtripVerified']:
        continue
    for key, hashkey in [('Path','SHA256'),('SPIRV','SPIRVSHA256'),('WGSL','WGSLSHA256')]:
        assert hashlib.sha256(Path(shader[key]).read_bytes()).hexdigest() == shader[hashkey]
    native = reflect(Path(shader['SPIRV']), indexed=True)
    web = reflect(Path(shader['AuditSPIRV']), indexed=True)
    sizes = reflect(Path(shader['AuditSPIRV']), root_spans=True)
    assert native == web, 'Native/Web scalar layout changed'
    for resource in shader['Resources']:
        if resource['Kind'] != 'ConstantBuffer': continue
        key = (resource['Group'],resource['Binding'])
        fields = web[key]
        native_min = max(offset+width for offset,width in native[key].values())
        web_bytes = sizes[key]
        assert web_bytes == resource['WebGPUMinBytes']
        ident = f'kGraphicsUniformFields_{len(layouts)}'
        lines.append(f'static const UniformScalarCopy {ident}[] = {{')
        occupied = set()
        for name,(offset,width) in fields.items():
            source,size = native[key][name]
            assert size == width and source + size <= native_min and offset + width <= web_bytes
            positions = set(range(offset,offset+width))
            assert not occupied & positions
            occupied |= positions
            lines.append(f'    {{{source}, {offset}, {width}}},')
        lines.append('};')
        layouts.append('    {'+', '.join([json.dumps(shader['Hash']),str(key[0]),str(key[1]),
            str(native_min),str(web_bytes),ident,f'sizeof({ident})/sizeof({ident}[0])'])+'},')
        records.append({'Shader':shader['Hash'],'Group':key[0],'Binding':key[1],
            'NativeMinimumBytes':native_min,'WebBytes':web_bytes,'ScalarFields':len(fields)})
lines += ['static const UniformLayout kGraphicsUniformLayouts[] = {',*layouts,'};','']
(root/'src/gpu-graphics-uniform-layouts.inc').write_text('\n'.join(lines),encoding='utf-8')
(root/'build/shaders/graphics-uniform-layouts.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
print(f'Published {len(layouts)} verified original graphics uniform layouts')
