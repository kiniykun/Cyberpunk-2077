"""Extract unchanged original cached shader containers with provenance."""
from pathlib import Path
import collections
import hashlib
import json
import struct

root=Path(__file__).resolve().parent
inventory=json.loads((root/'build/shaders/native-static-cache-inventory.json').read_text())
source=Path(inventory['source'])
data=source.read_bytes()
assert hashlib.sha256(data).hexdigest()==inventory['sha256']
out=root/'build/shaders/native-static'
out.mkdir(exist_ok=True)
rows=[]
stages={0:'pixel',1:'vertex',2:'geometry',3:'hull',4:'domain',5:'compute',6:'library'}
for entry in inventory['containers']:
    offset,size=entry['offset'],entry['bytes']
    blob=data[offset:offset+size]
    if blob[:4]!=b'DXBC' or struct.unpack_from('<I',blob,24)[0]!=len(blob):
        raise ValueError('Invalid original container size')
    chunks={}
    for i in range(struct.unpack_from('<I',blob,28)[0]):
        start=struct.unpack_from('<I',blob,32+4*i)[0]
        if start+8>len(blob): raise ValueError('Invalid chunk pointer')
        kind=blob[start:start+4].decode('ascii')
        length=struct.unpack_from('<I',blob,start+4)[0]
        if start+8+length>len(blob): raise ValueError('Invalid chunk size')
        chunks[kind]=(start+8,length)
    dxil,length=chunks['DXIL']
    if length<24: raise ValueError('Incomplete DXIL program')
    program=struct.unpack_from('<I',blob,dxil)[0]
    stage=stages.get(program>>16, f'unknown-{program>>16}')
    path=out/f'{entry["hash"]}-{stage}.dxil'
    path.write_bytes(blob)
    rows.append({'Hash':entry['hash'],'Stage':stage,'ShaderModel':f'{(program>>4)&15}.{program&15}',
                 'Path':str(path),'SourceOffset':offset,'Bytes':size,
                 'SHA256':hashlib.sha256(blob).hexdigest(),'Chunks':list(chunks)})
result={'Source':str(source),'SourceSHA256':inventory['sha256'],'Extracted':len(rows),
        'CountsByStage':dict(collections.Counter(s['Stage'] for s in rows)),
        'NativeBytesUnmodified':True,'BrowserConversionVerified':False,'Shaders':rows}
(root/'build/shaders/native-static-shaders.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!='Shaders'},indent=2))
