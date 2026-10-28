"""Read version-10 material cache using the original serialized schema.

Preserve bytecode and shader/material hashes; never infer UI identity from byte
patterns. Section boundaries and every referenced shader must be valid.
"""
from pathlib import Path
import collections
import hashlib
import json
import struct

root = Path(__file__).resolve().parent
source = Path('D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/engine/shader.cache')
data = source.read_bytes()
footer = struct.unpack_from('<3I2QI9Q2I', data, len(data)-112)
names = ['ShaderEntries','MaterialEntries','ReflectionEntries','IncludesCRC','Timestamp',
         'IncludeEntries','ShaderBytes','MaterialBytes','ReflectionBytes','IncludeBytes',
         'TimestampBytes','MaterialOffset','ReflectionOffset','TimestampOffset',
         'IncludeOffset','Magic','Version']
info = dict(zip(names, footer))
assert info['Magic'] == 1397245010 and info['Version'] == 10, info
assert info['ShaderBytes'] == info['MaterialOffset']
assert info['MaterialOffset'] + info['MaterialBytes'] == info['ReflectionOffset']
assert info['ReflectionOffset'] + info['ReflectionBytes'] == info['TimestampOffset']
assert info['TimestampOffset'] + info['TimestampBytes'] == info['IncludeOffset']
assert info['IncludeOffset'] + info['IncludeBytes'] == len(data)-112

class Reader:
    def __init__(self, start, size): self.pos, self.end = start, start+size
    def take(self, n):
        if n < 0 or self.pos+n > self.end: raise ValueError('Section read overflow')
        value = data[self.pos:self.pos+n]; self.pos += n
        return value
    def unpack(self, fmt): return struct.unpack(fmt, self.take(struct.calcsize(fmt)))
    def string(self):
        first = self.unpack('<B')[0]; value = first & 63
        more, shift = first & 64, 6
        for _ in range(4):
            if not more: break
            byte = self.unpack('<B')[0]; value |= (byte & 127) << shift
            more, shift = byte & 128, shift+7
        if more: raise ValueError('Invalid compressed string size')
        if not value: return ''
        return self.take(value * (1 if first & 128 else 2)).decode(
            'utf-8' if first & 128 else 'utf-16-le').rstrip('\0')
    def finish(self):
        if self.pos != self.end: raise ValueError(f'Unconsumed section: {self.end-self.pos}')

out = root/'build/shaders/native-material'
out.mkdir(exist_ok=True)
reader = Reader(0, info['ShaderBytes'])
shaders = []
stages = {0:'pixel',1:'vertex',2:'geometry',3:'hull',4:'domain',5:'compute',6:'library'}
for _ in range(info['ShaderEntries']):
    key, reflection, size = reader.unpack('<QQI')
    offset = reader.pos; blob = reader.take(size)
    assert blob[:4] == b'DXBC' and struct.unpack_from('<I',blob,24)[0] == size
    chunks = {}
    for i in range(struct.unpack_from('<I',blob,28)[0]):
        start = struct.unpack_from('<I',blob,32+4*i)[0]
        assert start+8 <= size
        length = struct.unpack_from('<I',blob,start+4)[0]
        assert start+8+length <= size
        chunks[blob[start:start+4].decode('ascii')] = (start+8,length)
    dxil, length = chunks['DXIL']; assert length >= 24
    version = struct.unpack_from('<I',blob,dxil)[0]
    stage = stages[version >> 16]
    path = out/f'{key}-{stage}.dxil'; path.write_bytes(blob)
    shaders.append({'Hash':str(key),'ReflectionHash':str(reflection),'Stage':stage,
                    'ShaderModel':f'{(version>>4)&15}.{version&15}',
                    'Path':str(path),'SourceOffset':offset,'Bytes':size,
                    'SHA256':hashlib.sha256(blob).hexdigest(),'Chunks':list(chunks)})
reader.finish()
keys = {s['Hash'] for s in shaders}
assert len(keys) == len(shaders)
reader = Reader(info['MaterialOffset'], info['MaterialBytes'])
materials = []
for _ in range(info['MaterialEntries']):
    key = reader.unpack('<Q')[0]; path = reader.string()
    technique, vs, ps, crcvs, crcps, timestamp, template = reader.unpack('<I5QI')
    samplers = []
    for stage in range(2):
        count = reader.unpack('<I')[0]
        assert count <= 16
        samplers.append([list(reader.take(8)) for _ in range(count)])
    assert str(vs) in keys and str(ps) in keys
    materials.append({'Hash':str(key),'Path':path,'TechniqueHash':technique,
                      'VertexShaderHash':str(vs),'PixelShaderHash':str(ps),
                      'TemplateHash':template,'Samplers':samplers})
reader.finish()
ui = [m for m in materials if m['Path'].lower().startswith('ui_')]
result = {'Source':str(source),'SourceSHA256':hashlib.sha256(data).hexdigest(),
          'Footer':info,'Extracted':len(shaders),'MaterialCount':len(materials),
          'CountsByStage':dict(collections.Counter(s['Stage'] for s in shaders)),
          'NativeBytesUnmodified':True,'UIPathMatches':len(ui),'UIMaterials':ui,
          'Shaders':shaders,'Materials':materials,'RuntimeIntegrated':False}
(root/'build/shaders/native-material-shaders.json').write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k not in ('Shaders','Materials','UIMaterials')},indent=2))
print('UI material paths:', sorted({m['Path'] for m in ui})[:25])
