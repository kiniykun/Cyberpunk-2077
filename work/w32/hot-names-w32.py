#!/usr/bin/env python3
"""Hot function names from wasm-split --in-secondary-memory profiles (one byte per defined function) of a named module.
usage: hot-names-w32.py module.wasm out.txt prof1.bin [prof2.bin ...]"""
import sys
src, dst, *profs = sys.argv[1:]
b = open(src, 'rb').read()
def leb(p):
    r = s = 0
    while True:
        x = b[p]; p += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, p
p = 8; names = {}; nimp = 0
while p < len(b):
    sid = b[p]; n, q = leb(p + 1)
    if sid == 2:
        cnt, r = leb(q)
        for _ in range(cnt):
            l, r = leb(r); r += l; l, r = leb(r); r += l; k = b[r]; r += 1
            if k == 0: _, r = leb(r); nimp += 1
            elif k == 1: r += 1; fl = b[r]; r += 1; _, r = leb(r); r = leb(r)[1] if fl & 1 else r
            elif k == 2:
                fl = b[r]; r += 1; _, r = leb(r)
                if fl & 1: _, r = leb(r)
            elif k == 3: r += 2
    if sid == 0:
        l, r = leb(q)
        if b[r:r + l] == b'name':
            r += l
            while r < q + n:
                sub = b[r]; sz, r2 = leb(r + 1)
                if sub == 1:
                    cnt, r3 = leb(r2)
                    for _ in range(cnt):
                        idx, r3 = leb(r3); l2, r3 = leb(r3); names[idx] = b[r3:r3 + l2].decode('utf-8', 'surrogateescape'); r3 += l2
                r = r2 + sz
    p = q + n
hot = set()
for f in profs:
    d = open(f, 'rb').read()
    for i, x in enumerate(d):
        if x and (i + nimp) in names: hot.add(names[i + nimp])
open(dst, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(sorted(hot)) + '\n')
print('hot names', len(hot))
