#!/usr/bin/env python3
"""[w32-coldtable] iPhone: JSC materialises every funcref table entry per WebAssembly.Instance (one wrapper per entry,
per pthread). The engine table has ~275k entries (every address-taken function). This rewrites the active element
segment into expression form: entries whose function never ran in the desktop profiles (wasm-split
--instrument --in-secondary-memory bytes, one per defined function) become ref.null; all function bodies stay.
An indirect call through a nulled entry traps (null function), like the cold-stub diagnostic.
usage: null-cold-table-w32.py in.wasm out.wasm prof1.bin [prof2.bin ...]
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
        c = x & 127; x >>= 7
        if x: o.append(c | 128)
        else: o.append(c); return bytes(o)

src, dst, *profs = sys.argv[1:]
b = open(src, 'rb').read()
hot = None
for pf in profs:
    d = open(pf, 'rb').read()
    hot = bytearray(d) if hot is None else bytearray(x | y for x, y in zip(hot, d))
p = 8; nimp = 0
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
    p = q + n
out = bytearray(b[:8]); p = 8; total = kept = 0
while p < len(b):
    sid = b[p]; n, q = leb(b, p + 1)
    if sid != 9:
        out += b[p:q + n]; p = q + n; continue
    cnt, r = leb(b, q); body = bytearray(enc(cnt))
    for _ in range(cnt):
        fl, r = leb(b, r)
        assert fl == 0, f'unexpected element segment flag {fl}'
        e = r
        while b[r] != 0x0b: r += 1
        r += 1
        offset_expr = b[e:r]
        k, r = leb(b, r)
        exprs = bytearray()
        for _ in range(k):
            idx, r = leb(b, r); total += 1
            defined = idx - nimp
            if idx < nimp or (0 <= defined < len(hot) and hot[defined]):
                exprs += b'\xd2' + enc(idx) + b'\x0b'; kept += 1           # ref.func idx
            else:
                exprs += b'\xd0\x70\x0b'                                    # ref.null func
        # flag 4: active, table 0, offset expr, vec(expr) of funcref
        body += enc(4) + offset_expr + enc(k) + exprs
    out += bytes([9]) + enc(len(body)) + body; p = q + n
open(dst, 'wb').write(out)
print(f'table entries {total}: kept {kept}, nulled {total - kept}; {len(b) / 1048576:.1f} -> {len(out) / 1048576:.1f} MiB')
