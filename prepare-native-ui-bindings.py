"""Prepare deterministic original-register bindings and audit uniform byte maps.

Only WGSL binding attributes change. Algorithms and interface locations remain
unchanged. Original DXIL, translator SPIR-V and validated WGSL are hash checked.
This does not claim renderer integration or native execution parity.
"""
from pathlib import Path
import ast
import hashlib
import json
import re
import struct
import subprocess

root=Path(__file__).resolve().parent
source=json.loads((root/'build/shaders/native-ui-conversion.json').read_text())
naga=root/'tools/naga-30.0.1/bin/naga.exe'
dxc=root/'tools/dxc-1.9.2609/bin/x64/dxc.exe'
# Reuse the uniform reflector without executing the compilation script.
tree=ast.parse((root/'compile-ui-shaders.py').read_text())
function=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='reflect')
namespace={'Path':Path,'struct':struct}
exec(compile(ast.Module(body=[function],type_ignores=[]),'<uniform-reflector>','exec'),namespace)
reflect=namespace['reflect']
out=root/'build/shaders/native-ui-ready'; out.mkdir(exist_ok=True)
pattern=re.compile(r'@group\((\d+)\)\s+@binding\((\d+)\)(\s+var(?:<([^>]+)>)?\s+\w+\s*:\s*([^;]+);)')
rows=[]
for original in source['Shaders']:
    if not original['WGSLValidated']: continue
    row=dict(original,RegisterMappingVerified=False,UniformRoundtripVerified=False)
    try:
        for key,hashkey in [('Path','SHA256'),('SPIRV','SPIRVSHA256'),('WGSL','WGSLSHA256')]:
            assert hashlib.sha256(Path(original[key]).read_bytes()).hexdigest()==original[hashkey]
        text=Path(original['WGSL']).read_text()
        resources=[]
        def remap(match):
            group,slot=int(match[1]),int(match[2]); address,kind=match[4] or '',match[5]
            if group: raise ValueError('Original register spaces beyond zero need explicit handling')
            if address=='uniform': name,shift,limit='ConstantBuffer',0,14
            elif kind=='sampler': name,shift,limit='Sampler',256,16
            elif kind.startswith('texture_') and not kind.startswith('texture_storage_'):
                name,shift,limit='TextureSRV',64,64
            else: raise ValueError(f'Unsupported UI resource {address}: {kind}')
            assert slot<limit
            resources.append({'Group':group,'OriginalSlot':slot,'Binding':slot+shift,
                              'Kind':name,'Type':kind})
            return f'@group({group}) @binding({slot+shift})'+match[3]
        mapped=pattern.sub(remap,text)
        assert len(resources)==text.count('@binding(')
        assert len({(r['Group'],r['Binding']) for r in resources})==len(resources)
        wgsl=out/(Path(original['WGSL']).stem+'.wgsl'); wgsl.write_text(mapped)
        audit=wgsl.with_suffix('.spv')
        process=subprocess.run([str(naga),'--keep-coordinate-space','-g',str(wgsl),str(audit)],capture_output=True,text=True)
        assert process.returncode==0,process.stderr
        native=reflect(Path(original['SPIRV']),indexed=True)
        web=reflect(audit,indexed=True)
        # Uniform slots retain native b-register indices. Both representations
        # must have identical scalar coordinates, widths and byte offsets.
        assert native==web,'Uniform byte offsets changed during WGSL roundtrip'
        sizes=reflect(audit,root_spans=True)
        dump=subprocess.run([str(dxc),'-dumpbin',original['Path']],capture_output=True,text=True)
        assert dump.returncode==0,dump.stderr
        dump_path=out/(wgsl.stem+'-original-dxil.txt'); dump_path.write_text(dump.stdout)
        vertex_inputs=[]
        if original['Stage']=='vertex':
            signature=dump.stdout.split('; Input signature:',1)[1].split('; Output signature:',1)[0]
            entry=re.search(r'@vertex\s+fn main\((.*?)\)\s*(?:->|\{)',mapped,re.S)
            assert entry,'Converted vertex entry point not found'
            locations={int(value) for value in re.findall(r'@location\((\d+)\)',entry[1])}
            for line in signature.splitlines():
                match=re.match(r';\s+(\w+)\s+(\d+)\s+([xyzw]+)\s+(\d+)\s+(\w+)\s+(float|uint|sint)\b',line)
                if not match: continue
                name,index,mask,register,system,component=match.groups()
                if system!='NONE' or int(register) not in locations: continue
                vertex_inputs.append({'Semantic':name,'Index':int(index),'Location':int(register),
                                      'Components':len(mask),'ComponentType':component})
            assert len({v['Location'] for v in vertex_inputs})==len(vertex_inputs)
            assert {v['Location'] for v in vertex_inputs}==locations,'Native input rows do not match WGSL locations'
        section=dump.stdout.split('; Resource Bindings:',1)[1].split('; ViewId',1)[0]
        expected=set()
        for line in section.splitlines():
            match=re.search(r'\b(cbuffer|texture|sampler)\s+.*?\b(?:CB|T|S)\d+\s+(cb|t|s)(\d+)\s+(\d+)\s*$',line)
            if not match: continue
            typename,kind,slot,count=match.groups(); assert int(count)==1
            expected.add(({'cb':'ConstantBuffer','t':'TextureSRV','s':'Sampler'}[kind],int(slot)))
        assert expected=={(r['Kind'],r['OriginalSlot']) for r in resources},'DXIL register reflection mismatch'
        for resource in resources:
            if resource['Kind']=='ConstantBuffer':
                resource['WebGPUMinBytes']=sizes[(resource['Group'],resource['Binding'])]
        row.update(RegisterMappingVerified=True,UniformRoundtripVerified=True,
                   VertexInputs=vertex_inputs,VertexInputLocationsVerified=True,
                   Resources=resources,WGSL=str(wgsl),WGSLSHA256=hashlib.sha256(wgsl.read_bytes()).hexdigest(),
                   AuditSPIRV=str(audit),OriginalDXILReflection=str(dump_path))
    except Exception as error: row['PreparationError']=str(error)
    rows.append(row)
result={'Prepared':sum(r['RegisterMappingVerified'] and r['UniformRoundtripVerified'] for r in rows),
        'Candidates':len(rows),'OriginalCacheSHA256':source['OriginalCacheSHA256'],
        'RegisterScheme':'b:0, t:64, s:256; original slots retained',
        'NativeDXILResourceReflectionChecked':True,'RuntimeIntegrated':False,
        'NativeExecutionParityVerified':False,'Shaders':rows}
(root/'build/shaders/native-ui-binding-preparation.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!='Shaders'},indent=2))
for row in rows:
    if 'PreparationError' in row: print(row['Hash'],row['PreparationError'][:250])
