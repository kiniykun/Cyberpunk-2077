"""Embed the matching original ICU 65 locale package, preserving its bytes."""
from pathlib import Path
import hashlib
import json
import struct
import subprocess

ROOT = Path(__file__).resolve().parent
SDK = Path(r'D:\cp2077SourceCode\tools\emsdk')
DATA = Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\engine\depot\i18n\icudt65l.dat')
PYTHON = SDK / 'python/3.13.3_64bit/python.exe'
raw = DATA.read_bytes()
header_size = struct.unpack_from('<H', raw)[0]
if raw[2:4] != b'\xda\x27' or raw[8:11] != bytes([0, 0, 2]) or raw[12:16] != b'CmnD':
    raise RuntimeError('Expected original little-endian UTF-16 ICU common-data package')
count = struct.unpack_from('<I', raw, header_size)[0]
if not 0 < count < 100000 or header_size + 4 + count * 8 > len(raw):
    raise RuntimeError('Invalid ICU package table')
names = []
for index in range(count):
    name_offset, data_offset = struct.unpack_from('<II', raw, header_size + 4 + index * 8)
    name_start = header_size + name_offset
    item_start = header_size + data_offset
    if not 0 <= name_start < len(raw) or not 0 <= item_start < len(raw) - 24:
        raise RuntimeError('Out-of-range ICU item')
    name_end = raw.find(b'\0', name_start)
    if name_end < 0:
        raise RuntimeError('Unterminated ICU item name')
    name = raw[name_start:name_end].decode('ascii')
    if not name.startswith('icudt65l/') or raw[item_start + 2:item_start + 4] != b'\xda\x27':
        raise RuntimeError('Mismatched ICU package or invalid item header')
    names.append(name)
out = ROOT / 'build/modules/icudata'
out.mkdir(parents=True, exist_ok=True)
source = out / 'icu-data.c'
source.write_text('#include <stddef.h>\n'
                  '_Alignas(16) const unsigned char icudt65_dat[] = {\n'
                  '#embed "' + DATA.as_posix() + '"\n};\n'
                  'const size_t cpweb_icu_data_size = sizeof(icudt65_dat);\n')
obj = out / 'icu-data.o'
log = out / 'compile.log'
with log.open('w') as stream:
    process = subprocess.run([str(PYTHON), str(SDK / 'upstream/emscripten/em++.py'),
                              '-x', 'c', '-std=c23', '-pthread', '-sMEMORY64=1',
                              '-O0', '-c', str(source), '-o', str(obj)],
                             stdout=stream, stderr=subprocess.STDOUT)
complete = process.returncode == 0
library = ROOT / 'build/lib/libicudata.a'
if complete:
    temporary = library.with_suffix('.building.a')
    temporary.unlink(missing_ok=True)
    subprocess.run([str(PYTHON), str(SDK / 'upstream/emscripten/emar.py'),
                    'rcs', str(temporary), str(obj)], check=True)
    temporary.replace(library)
status = dict(Module='icudata', Sources=1, Compiled=int(complete), Complete=complete,
              Library=str(library) if complete else None, OriginalData=str(DATA),
              DataSHA256=hashlib.sha256(raw).hexdigest(), DataBytes=len(raw),
              ItemCount=count, WcharBytes=4,
              Units=[dict(Source=str(source), Object=str(obj), Log=str(log), Compiled=complete)])
(out / 'result.json').write_text(json.dumps(status, indent=2))
(out / 'package-items.json').write_text(json.dumps(names, indent=2))
print(f'icudata: {len(raw)} original bytes, {count} validated items; complete={complete}')
if not complete:
    print(log.read_text())
    raise SystemExit(process.returncode)
