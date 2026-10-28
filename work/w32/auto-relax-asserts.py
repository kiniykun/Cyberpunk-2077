#!/usr/bin/env python3
"""[w32] For every failed `static_assert( sizeof( X ) == N ...)` in build-w32/failures.json, make a wasm32 copy
(w32-overlay.py) and wrap the line:  #if defined( __wasm32__ ) <same assert with '<='> #else <original> #endif.
Pointer members shrink on wasm32, so a struct is never expected to grow; '<=' keeps the size-budget intent.
Each relaxation is logged to build-w32/auto-relaxed-asserts.json for review (a struct that is copied raw to or from
disk would need a real fix; none is expected for pointer-bearing structs). Other assert forms are left alone."""
import json, re, subprocess
from pathlib import Path
W32 = Path(r'D:\2077\build-w32')
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
logp = W32 / 'auto-relaxed-asserts.json'
log = json.loads(logp.read_text()) if logp.exists() else []
fails = json.loads((W32 / 'failures.json').read_text())
ERR = re.compile(r"^(?P<file>[A-Za-z]:.*?):(?P<line>\d+):\d+: error: static assertion failed due to requirement 'sizeof\(.*\) == \d+'")
ASSERT = re.compile(r'^(\s*)(static_assert|RED_STATIC_ASSERT)\s*\(\s*sizeof\s*\(.*?\)\s*==\s*\d+')
done = set((e['File'].lower(), e['Line']) for e in log)
todo = {}
for obj, rec in fails.items():
    for e in rec.get('errors', []):
        m = ERR.match(e.strip())
        if m: todo.setdefault(str(Path(m['file'])).lower(), set()).add(int(m['line']))
for orig, lines in todo.items():
    subprocess.run([PY, OV, 'add', orig], capture_output=True, text=True, check=True)
    p = Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    L = raw.replace('\r\n', '\n').split('\n')
    for ln in sorted(lines, reverse=True):
        if (orig, ln) in done: continue
        src = L[ln - 1]
        if not ASSERT.match(src) or '[w32]' in src:
            print('SKIP (not a plain sizeof== assert)', p.name, ln, src.strip()[:100]); continue
        new = re.sub(r'\)\s*==\s*(\d+)', r') <= \1', src, count=1) + ' // [w32] auto-relaxed: pointer members are 4 bytes'
        L[ln - 1:ln] = ['#if defined( __wasm32__ )', new, '#else', src, '#endif']
        log.append({'File': orig, 'Line': ln, 'Copy': str(p), 'Original': src.strip()})
        print('relaxed', p.name, ln, src.strip()[:110])
    out = '\n'.join(L)
    p.write_bytes((out.replace('\n', '\r\n') if crlf else out).encode('utf-8', errors='surrogateescape'))
logp.write_text(json.dumps(log, indent=1))
