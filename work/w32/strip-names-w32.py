#!/usr/bin/env python3
"""[w32-nonames] Drop the custom "name" section (function names) from a wasm file: smaller download and no name
table for JSC to parse/keep. usage: strip-names-w32.py in.wasm out.wasm"""
import sys
src, dst = sys.argv[1:3]
b = open(src, 'rb').read()
def leb(p):
    r = s = 0
    while True:
        x = b[p]; p += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, p
out = bytearray(b[:8]); p = 8; dropped = 0
while p < len(b):
    sid = b[p]; n, q = leb(p + 1)
    if sid == 0:
        l, r = leb(q)
        if b[r:r + l] == b'name': dropped += n; p = q + n; continue
    out += b[p:q + n]; p = q + n
open(dst, 'wb').write(out)
print(f'dropped name section {dropped / 1048576:.1f} MiB -> {len(out) / 1048576:.1f} MiB')
