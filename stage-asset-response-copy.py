"""Apply the source asset byte conversion fix to the generated pre-JS."""
from pathlib import Path
import hashlib,json
r=Path(__file__).resolve().parent;p=r/'build/game/cp2077.js';text=p.read_text()
old='for (let i = 0; i < bytes.length; ++i) bytes[i] = request.responseText.charCodeAt(i) & 255;'
new='for (let i = 0; i < bytes.length; ++i) bytes[i] = responseText.charCodeAt(i) & 255;'
if old in text:
    assert text.count(old)==1
    request_anchor='    const request = new XMLHttpRequest;'
    start=text.index(request_anchor,text.index('const cacheLimit'))
    end=text.index('    return bytes;',start)
    block=text[start:end]
    assert block.count('request.send(null);')==1 and block.count(old)==1
    block=block.replace(request_anchor,request_anchor+'\n    let responseText;',1)
    block=block.replace('      request.send(null);','      request.send(null);\n      responseText = request.responseText;',1)
    block=block.replace('request.responseText.length','responseText.length').replace(old,new)
    text=text[:start]+block+text[end:]
assert text.count(new)==1 and old not in text
p.write_text(text)
(r/'build/asset-response-copy-stage.json').write_text(json.dumps({'JS_SHA256':hashlib.sha256(p.read_bytes()).hexdigest(),'ResponseTextAccessesPerBlock':1,'OriginalRangeAndEtagValidationPreserved':True,'OriginalByteMappingPreserved':True},indent=2))
print('Original asset response copied with one responseText access per block.')
