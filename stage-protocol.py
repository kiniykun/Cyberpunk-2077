from pathlib import Path
import hashlib
import json

root = Path(__file__).resolve().parent
source = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\internal\CommLibProtocol\src')
destination = root / 'build-tools/comm-protocol/src'
destination.mkdir(parents=True, exist_ok=True)
records = []
for original in source.iterdir():
    if original.suffix not in ('.h', '.cpp'):
        continue
    data = original.read_bytes()
    try:
        text = data.decode('utf-8-sig')
    except UnicodeDecodeError:
        text = data.decode('cp1252')
    text = text.replace('\r\n', '\n')
    text = text.replace('defined( __linux__ )', '(defined( __linux__ ) || defined( __EMSCRIPTEN__ ))')
    text = text.replace('#ifdef __linux__', '#if defined(__linux__) || defined(__EMSCRIPTEN__)')
    text = text.replace('vsprintf( buf, message, args );', 'vsnprintf( buf, sizeof(buf), message, args );')
    text = text.replace('vsprintf( buf, txt, args );', 'vsnprintf( buf, sizeof(buf), txt, args );')
    if original.name == 'stdafx.h':
        text += '\n#include <limits>\n#include <cstdlib>\n#include <cstdint>\n'
    if original.name == 'proto.cpp':
        text = text.replace('int main( int argc, const char* argv[] )', 'int main( int argc, char* argv[] )')
    output = destination / original.name
    output.write_text(text, encoding='utf-8')
    records.append({'source': str(original), 'sourceSHA256': hashlib.sha256(data).hexdigest(),
                    'adaptedSHA256': hashlib.sha256(output.read_bytes()).hexdigest()})
(destination.parent / 'source-manifest.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('Staged original CommLibProtocol generator source')
