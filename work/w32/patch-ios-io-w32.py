#!/usr/bin/env python3
"""[w32-iosio] mem=ios: bounded JS-side IO memory.
- CpResRead (pthread engine reads): per-thread block LRU 16 -> 2 MiB; large reads fetched in 1 MiB pieces (no single
  huge transient ArrayBuffer per read).
- main-thread libc FS (glue resource-filesystem-web.js, w32 copy): block LRU 64 -> 8 MiB.
- IO allocator budget back to 62 MiB (40 stalled the nomadfree world load)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def edit(p, edits, tag):
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:80], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

IOS = '(typeof location == "object" && /[?&]mem=ios(&|$)/.test(location.search))'
p = copy(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src\common\redIO\src\redIOSystemFileLinuxAPI.cpp')  # original depot key (the compile jobs' input); assert 'build-w32' in str(p)
edit(p, [
 ('const blockSize = 1048576, cacheLimit = 16 * 1048576;', 'const blockSize = 1048576, cacheLimit = (' + IOS + ' ? 8 : 16) * 1048576; /*[w32-iosio]*/'),
 ('''        const bytes = fetchRange(offset, offset + count - 1);
        if (!bytes) { ++R.stats.failures; return -1; }
        growMemViews(); HEAPU8.set(bytes, target); copied = count;''', '''        const piece = ''' + IOS + ''' ? 8388608 : count; /*[w32-iosio] 8 MiB pieces under mem=ios*/
        while (copied < count) {
            const n = Math.min(piece, count - copied);
            const bytes = fetchRange(offset + copied, offset + copied + n - 1);
            if (!bytes) { ++R.stats.failures; return -1; }
            growMemViews(); HEAPU8.set(bytes, target + copied); copied += n;
        }'''),
], '[w32-iosio]')
g = Path(r'D:\2077\build-w32\glue\resource-filesystem-web.js')
edit(g, [('  const cacheLimit = 64 * 1024 * 1024;', '  const cacheLimit = (' + IOS + ' ? 8 : 64) * 1024 * 1024; /*[w32-iosio]*/')], '[w32-iosio]')
s = Path(r'D:\2077\build-w32\src-w32\dev\src\common\redio\src\serializationmemoryallocator.cpp')
edit(s, [('CpWebMemIos() ? RED_MEGA_BYTE( 40 ) : RED_MEGA_BYTE( 126 ) /*[w32-lean7] was 62 under ios*/',
          'CpWebMemIos() ? RED_MEGA_BYTE( 62 ) : RED_MEGA_BYTE( 126 ) /*[w32-iosio] back to 62 (40 stalled nomadfree)*/')], '[w32-iosio]')
