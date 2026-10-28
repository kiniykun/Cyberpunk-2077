"""Stage the original static cache for the engine's existing final-build path."""
from pathlib import Path
import hashlib
import json
import shutil
import struct

root=Path(__file__).resolve().parent
source=Path('D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/engine/staticshader.cache')
data=source.read_bytes()
meta, shaders, date, time, magic, version, directory_hash=struct.unpack_from('<6IQ',data,len(data)-32)
if version != 7 or magic != 1397245010 or not meta or not shaders:
    raise ValueError('Original cache footer does not match the current loader')
destination=root/'runtime/engine/staticshader_final.cache'
destination.parent.mkdir(parents=True,exist_ok=True)
digest=hashlib.sha256(data).hexdigest()
if not destination.exists() or hashlib.sha256(destination.read_bytes()).hexdigest()!=digest:
    shutil.copyfile(source,destination)
assert hashlib.sha256(destination.read_bytes()).hexdigest()==digest
manifest={'Source':str(source),'Destination':str(destination),
          'BrowserPath':'/game/engine/staticshader_final.cache','SHA256':digest,
          'NativeCacheBytesUnmodified':True,'FilenameAliasForFinalConfiguration':True,
          'MetadataEntries':meta,'NativeShaderEntries':shaders,'Version':version,
          'ConvertedShaderCount':4,'CompleteBrowserShaderCoverage':False,
          'CacheLoadedInBrowser':False}
(root/'build/shaders/static-cache-staging.json').write_text(json.dumps(manifest,indent=2))
print(f'Staged original cache: {meta} metadata entries, {shaders} shaders; bytes unchanged')
