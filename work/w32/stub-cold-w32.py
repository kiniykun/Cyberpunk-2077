#!/usr/bin/env python3
"""[w32-cold] Diagnostic: replace the bodies of functions that never ran in the desktop profile (boot -> menu ->
in-world drive; wasm-split --instrument --in-secondary-memory bytes) with a single `unreachable`. Function indices,
types, tables, exports, data and names are unchanged, so a cold call traps with its own name in the stack.
usage: stub-cold-w32.py in.wasm out.wasm prof1.bin [prof2.bin ...]
"""
import sys

def leb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, p

def enc(x):
    o = bytearray()
    while True:
        b = x & 127; x >>= 7
        if x: o.append(b | 128)
        else: o.append(b); return bytes(o)

src, dst, *profs = sys.argv[1:]
b = open(src, 'rb').read()
hot = None
for pf in profs:
    d = open(pf, 'rb').read()
    hot = bytearray(d) if hot is None else bytearray(x | y for x, y in zip(hot, d))
# exported functions always stay
p = 8; nimp = 0; exported = set(); start = None
while p < len(b):
    sid = b[p]; n, q = leb(b, p + 1)
    if sid == 2:
        cnt, r = leb(b, q)
        for _ in range(cnt):
            l, r = leb(b, r); r += l; l, r = leb(b, r); r += l; k = b[r]; r += 1
            if k == 0: _, r = leb(b, r); nimp += 1
            elif k == 1: r += 1; fl = b[r]; r += 1; _, r = leb(b, r); r = leb(b, r)[1] if fl & 1 else r
            elif k == 2:
                fl = b[r]; r += 1; _, r = leb(b, r)
                if fl & 1: _, r = leb(b, r)
            elif k == 3: r += 2
    elif sid == 7:
        cnt, r = leb(b, q)
        for _ in range(cnt):
            l, r = leb(b, r); r += l; k = b[r]; r += 1; idx, r = leb(b, r)
            if k == 0: exported.add(idx)
    elif sid == 8:
        start, _ = leb(b, q)
    p = q + n
out = bytearray(b[:8]); p = 8; stubbed = kept = 0; saved = 0
STUB = bytes([0, 0x00, 0x0b])   # no locals; unreachable; end
while p < len(b):
    sid = b[p]; n, q = leb(b, p + 1)
    if sid != 10:
        out += b[p:q + n]; p = q + n; continue
    cnt, r = leb(b, q); body = bytearray(enc(cnt))
    for i in range(cnt):
        sz, r2 = leb(b, r); fn = b[r2:r2 + sz]; r = r2 + sz
        gidx = i + nimp
        if (hot is not None and i < len(hot) and hot[i]) or gidx in exported or gidx == start:
            body += enc(sz) + fn; kept += 1
        else:
            body += enc(len(STUB)) + STUB; stubbed += 1; saved += sz - len(STUB)
    out += bytes([10]) + enc(len(body)) + body; p = q + n
open(dst, 'wb').write(out)
print(f'kept {kept} stubbed {stubbed} saved {saved / 1048576:.1f} MiB -> {len(out) / 1048576:.1f} MiB')
