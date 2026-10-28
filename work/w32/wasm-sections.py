import sys
def leb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, p
for f in sys.argv[1:]:
    b = open(f, 'rb').read(); p = 8; out = []
    while p < len(b):
        sid = b[p]; n, q = leb(b, p + 1); name = ''
        if sid == 0: ln, r = leb(b, q); name = b[r:r + ln].decode(errors='replace')
        out.append(f"{sid}{'/' + name if name else ''}={n / 1048576:.1f}"); p = q + n
    print(f, len(b) / 1048576, ' '.join(out))
