import sys
def leb(b, p):
    r = s = 0
    while True:
        x = b[p]; p += 1; r |= (x & 127) << s; s += 7
        if x < 128: return r, p
b = open(sys.argv[1], 'rb').read(); p = 8
while p < len(b):
    sid = b[p]; n, q = leb(b, p + 1)
    if sid == 10:
        cnt, r = leb(b, q); sizes = []
        for i in range(cnt):
            sz, r2 = leb(b, r); sizes.append(sz); r = r2 + sz
        sizes.sort(reverse=True)
        print('functions', cnt, 'max', sizes[:8], 'over1MB', sum(1 for s in sizes if s > 1 << 20))
    p = q + n
