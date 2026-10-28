"""Translate the original static-cache DXIL programs without recreating shaders."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import subprocess
import argparse
import re

root=Path(__file__).resolve().parent
translator=root/'tools/dxil-spirv/build-win/Release/dxil-spirv.exe'
naga=root/'tools/naga-30.0.1/bin/naga.exe'
parser=argparse.ArgumentParser()
parser.add_argument('--material-ui',action='store_true')
parser.add_argument('--material-observed',type=Path)
# [textures] Runtime-captured exact bytecode (globalThis.cp2077UnknownGraphicsBytecode dump) for shaders that the
# extracted engine shader.cache does not contain; converted like --material-observed rows.
parser.add_argument('--captured',type=Path)
options=parser.parse_args()
if options.captured and not options.material_observed: raise SystemExit('--captured requires --material-observed')
is_material=options.material_ui or bool(options.material_observed)
source=json.loads((root/('build/shaders/native-material-shaders.json' if is_material
                         else 'build/shaders/native-static-shaders.json')).read_text())
selected=source['Shaders']
if options.material_observed:
    messages=json.loads(options.material_observed.read_text())['errors']
    observed=set()
    for row in messages:
        match=re.search(r'no exact original-bytecode conversion: stage=(\d+) bytes=(\d+) name=(.+)',row['message'])
        if match:observed.add(('vertex' if match[1]=='0' else 'pixel',int(match[2]),match[3]))
    assert observed,'No observed original material shader failures'
    families={name for _,_,name in observed}
    selected_materials=[m for m in source['Materials'] if m['Path'].split(' ',1)[0] in families]
    hashes={m[k] for m in selected_materials for k in ('VertexShaderHash','PixelShaderHash')}
    pairs={(stage,size) for stage,size,_ in observed}
    selected=[s for s in selected if s['Hash'] in hashes and (s['Stage'],s['Bytes']) in pairs]
    if options.captured:
        import base64
        capdir=root/'build/shaders/native-captured';capdir.mkdir(exist_ok=True)
        selected=[]
        for record in json.loads(options.captured.read_text()):
            blob=base64.b64decode(record['base64']);digest=hashlib.sha256(blob).hexdigest()
            stage='vertex' if record['stage']==0 else 'pixel'
            ident=str(int(digest[:15],16))
            path=capdir/(ident+'-'+stage+'.dxil');path.write_bytes(blob)
            selected.append({'Hash':ident,'Stage':stage,'Path':str(path),'Bytes':len(blob),'SHA256':digest,
                             'CapturedName':record.get('name',''),'Captured':True})
    assert selected,'Observed shader byte sizes not found in original material cache'
if options.material_ui:
    selected_materials=[m for m in source['Materials'] if m['Path'].lower().startswith('ui_') or m['Path'].lower().startswith('bink_simple ')]
    hashes={m[k] for m in selected_materials for k in ('VertexShaderHash','PixelShaderHash')}
    selected=[s for s in selected if s['Hash'] in hashes]
    assert len(selected)==len(hashes)
out=root/('build/shaders/native-observed-material-converted' if options.material_observed else 'build/shaders/native-ui-converted' if options.material_ui else 'build/shaders/native-converted')
out.mkdir(exist_ok=True)
def convert(shader):
    row=dict(shader,SPIRVConverted=False,WGSLConverted=False,WGSLValidated=False)
    blob=Path(shader['Path']).read_bytes()
    if hashlib.sha256(blob).hexdigest()!=shader['SHA256']:
        raise ValueError('Original extracted shader bytes changed')
    stem=Path(shader['Path']).stem
    spv=out/(stem+'.spv')
    wgsl=out/(stem+'.wgsl')
    commands=[('DXIL', [str(translator),shader['Path'],'--ssbo-srv','--ssbo-uav','--output',str(spv)]),
              # DXIL already uses the Direct3D/WebGPU clip-space convention.
              # This SPIR-V bridge does not introduce a Vulkan viewport flip;
              # Naga's default SPIR-V import adjustment would invert native Y.
              ('WGSL', [str(naga),'--keep-coordinate-space',str(spv),str(wgsl)]),
              ('Validation', [str(naga),str(wgsl)])]
    for step,args in commands:
        process=subprocess.run(args,capture_output=True,text=True,errors='replace',timeout=60)
        log=out/(stem+'-'+step.lower()+'.log')
        log.write_text(process.stdout+process.stderr,encoding='utf-8')
        row[step+'Log']=str(log)
        if process.returncode:
            row.update(FailedStage=step,ExitCode=process.returncode)
            break
        row[{'DXIL':'SPIRVConverted','WGSL':'WGSLConverted','Validation':'WGSLValidated'}[step]]=True
    if row['SPIRVConverted']: row.update(SPIRV=str(spv),SPIRVSHA256=hashlib.sha256(spv.read_bytes()).hexdigest())
    if row['WGSLValidated']: row.update(WGSL=str(wgsl),WGSLSHA256=hashlib.sha256(wgsl.read_bytes()).hexdigest())
    return row
rows=[]
with ThreadPoolExecutor(max_workers=4) as pool:
    futures=[pool.submit(convert,s) for s in selected]
    for future in as_completed(futures):
        rows.append(future.result())
result={'OriginalCacheSHA256':source['SourceSHA256'],'OriginalPrograms':len(rows),
        'TranslatorSHA256':hashlib.sha256(translator.read_bytes()).hexdigest(),
        'NagaSHA256':hashlib.sha256(naga.read_bytes()).hexdigest(),
        'SPIRVConverted':sum(r['SPIRVConverted'] for r in rows),
        'WGSLValidated':sum(r['WGSLValidated'] for r in rows),
        'ConvertedFromOriginalBytecode':True,'NativeResourceLayoutParityVerified':False,
        'NativeClipCoordinatesPreserved':True,'NagaCoordinateAdjustmentDisabled':True,
        'RuntimeIntegrated':False,'BrowserRenderingVerified':False,
        'Shaders':sorted(rows,key=lambda r:int(r['Hash']))}
if options.material_ui:
    result.update(Materials=selected_materials,MaterialCachePrograms=source['Extracted'],
                  Selection='Original material path prefix ui_ plus exact bink_simple family')
(root/('build/shaders/native-observed-material-conversion.json' if options.material_observed else 'build/shaders/native-ui-conversion.json' if options.material_ui else
       'build/shaders/native-cache-conversion.json')).write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k not in ('Shaders','Materials')},indent=2))
