"""Package audited original static graphics bytecode and uniform maps."""
from pathlib import Path
import argparse, ast, hashlib, json, struct
from native_cbv_padding import cbv_sizes, verify_scalar_reads
from present_cpu_abi import present_cpu_source_map
from material_modifier_cpu_abi import material_modifier_source_limit
from temporal_mask_cpu_abi import temporal_mask_source_limit
root=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--material-ui',action='store_true');options=parser.parse_args()
category='material' if options.material_ui else 'static'
prefix='kMaterialGraphics' if options.material_ui else 'kStaticGraphics'
generator=(root/'generate-ui-graphics-package.py').read_text()
generator=generator.replace('native-ui-binding-preparation.json',f'native-{category}-graphics-binding-preparation.json').replace('gpu-ui-graphics-package.inc',f'gpu-{category}-graphics-package.inc').replace('kGraphics',prefix).replace('kConvertedGraphicsShaders','kConvertedMaterialGraphicsShaders' if options.material_ui else 'kConvertedStaticGraphicsShaders')
exec(compile(generator,'static-graphics-package-generator','exec'),{'__file__':str(root/'generate-ui-graphics-package.py')})
tree=ast.parse((root/'compile-ui-shaders.py').read_text())
function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='reflect')
ns={'Path':Path,'struct':struct};exec(compile(ast.Module(body=[function],type_ignores=[]),'uniform-reflector','exec'),ns)
reflect=ns['reflect']
manifest=json.loads((root/f'build/shaders/native-{category}-graphics-binding-preparation.json').read_text())
lines=['// Audited original static graphics scalar byte maps.'];entries=[];padding_audits=[];cpu_abi_audits=[]
for shader in manifest['Shaders']:
    if not shader['RegisterMappingVerified'] or not shader['UniformRoundtripVerified']:continue
    for key,hashkey in [('Path','SHA256'),('SPIRV','SPIRVSHA256'),('WGSL','WGSLSHA256')]:assert hashlib.sha256(Path(shader[key]).read_bytes()).hexdigest()==shader[hashkey]
    native=reflect(Path(shader['SPIRV']),indexed=True);web=reflect(Path(shader['AuditSPIRV']),indexed=True);sizes=reflect(Path(shader['AuditSPIRV']),root_spans=True)
    for resource in shader['Resources']:
        if resource['Kind']!='ConstantBuffer':continue
        key=(resource['Group'],resource['Binding']);native_key=(resource['Group'],resource['OriginalSlot'])
        assert native[native_key]==web[key]
        fields=web[key];native_min=max(o+n for o,n in native[native_key].values());web_bytes=sizes[key]
        if resource['Kind']=='ConstantBuffer':
            dxil_path=Path(shader['WGSL']).with_name(Path(shader['WGSL']).stem+'-original-dxil.txt')
            dxil=dxil_path.read_text()
            descriptor=cbv_sizes(dxil).get(native_key)
            if descriptor and descriptor['Bytes']<native_min:
                try:
                    reads=verify_scalar_reads(dxil,descriptor['RangeID'],descriptor['Bytes'])
                except ValueError:
                    # Keep the stricter existing layout when native scalar
                    # liveness cannot be proved; no unused-byte assumption.
                    if resource['OriginalSlot']==6:raise
                    reads=None
                if reads is None:
                    reads=[]
                retained={name:value for name,value in fields.items() if native[native_key][name][0]+native[native_key][name][1]<=descriptor['Bytes']}
                if not retained and reads:raise ValueError('No native scalar fields retained')
                for offset,width in reads:
                    if not any(native[native_key][name]==(offset,width) for name in retained):raise ValueError('Actual native read missing from uniform map')
                if reads:
                    padding_audits.append({'Hash':shader['Hash'],'Stage':shader['Stage'],'Binding':key[1],'NativeDeclaredBytes':descriptor['Bytes'],'TranslatedNativeSpanBefore':native_min,'WebUniformBytes':web_bytes,'ActualNativeScalarReads':reads,'OmittedPaddingFields':len(fields)-len(retained),'DXILSHA256':hashlib.sha256(dxil_path.read_bytes()).hexdigest()})
                    fields=retained;native_min=descriptor['Bytes']
        cpu_map=None
        modifier_limit=material_modifier_source_limit(root,resource,dxil,descriptor) or temporal_mask_source_limit(root,shader,resource,dxil,descriptor)
        if modifier_limit:
            cpu_bytes,reads,proof=modifier_limit
            retained={name:value for name,value in fields.items() if native[native_key][name][0]+native[native_key][name][1]<=cpu_bytes}
            if any(not any(native[native_key][name]==(offset,width) for name in retained) for offset,width in reads):raise ValueError('Actual modifier scalar read missing from CPU map')
            fields=retained;native_min=cpu_bytes
            proof.update(Hash=shader['Hash'],Stage=shader['Stage'],Binding=key[1],WebUniformBytes=web_bytes)
            cpu_abi_audits.append(proof)
        if shader['Hash']=='18217308226223964159' and resource['OriginalSlot']==6:
            cpu_map,proof=present_cpu_source_map(root,shader,resource,dxil,fields,native,native_key)
            native_min=proof['OriginalCPUBytes'];cpu_abi_audits.append(proof)
        ident=prefix+'UniformFields_'+str(len(entries));lines.append('static const UniformScalarCopy '+ident+'[] = {')
        occupied=set()
        for name,(offset,width) in fields.items():
            if cpu_map is not None and name not in cpu_map:continue
            source,size=cpu_map[name] if cpu_map is not None else native[native_key][name];assert width==size
            positions=set(range(offset,offset+width));assert not occupied&positions;occupied|=positions
            lines.append('    {%d, %d, %d},'%(source,offset,width))
        lines.append('};')
        entries.append('    {'+', '.join([json.dumps(shader['Hash']),str(key[0]),str(key[1]),str(native_min),str(web_bytes),ident,'sizeof('+ident+')/sizeof('+ident+'[0])'])+'},')
lines+=['static const UniformLayout '+prefix+'UniformLayouts[] = {',*entries,'};']
(root/f'src/gpu-{category}-graphics-uniform-layouts.inc').write_text('\n'.join(lines))
(root/f'build/shaders/native-{category}-shared-uniform-padding-audit.json').write_text(json.dumps(padding_audits,indent=2))
(root/f'build/shaders/native-{category}-cpu-uniform-abi-audit.json').write_text(json.dumps(cpu_abi_audits,indent=2))
print('Audited shared uniform padding:',len(padding_audits))
print('Packaged %d audited %s graphics uniform maps'%(len(entries),category))
