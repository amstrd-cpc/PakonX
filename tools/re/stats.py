"""Per-plane stats of a planar u16 dump (w h planes) or u8 dump."""
import array, sys
path, w, h, kind = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
a = array.array('H' if kind == 'u16' else 'B'); a.frombytes(open(path, 'rb').read())
n = w * h
for c in range(3):
    pl = a[c * n:(c + 1) * n]
    s = sorted(pl[::97])
    q = lambda f: s[int(f * (len(s) - 1))]
    print(f'  plane {c}: min {s[0]} p1 {q(0.01)} p50 {q(0.5)} p99 {q(0.99)} max {s[-1]}')
