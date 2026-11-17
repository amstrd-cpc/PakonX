"""Relate an Ansel scene thumbnail (250x375 u16 planar) to its full frame (1000x1500):
4x4 box mean (and 4x subsample), 4 orientations; report mean abs diff per variant."""
import array, sys
th, full = sys.argv[1], sys.argv[2]
t = array.array('H'); t.frombytes(open(th, 'rb').read())
F = array.array('H'); F.frombytes(open(full, 'rb').read())
W, H, w, h = 1000, 1500, 250, 375
def box(c, x, y, mode):
    if mode == 'sub':
        return F[c * W * H + (4 * y) * W + 4 * x]
    s = 0
    for dy in range(4):
        base = c * W * H + (4 * y + dy) * W + 4 * x
        s += F[base] + F[base + 1] + F[base + 2] + F[base + 3]
    return s / 16
for mode in ('box', 'sub'):
    for fx in (False, True):
        for fy in (False, True):
            d = 0; n = 0
            for c in range(3):
                for y in range(0, h, 5):
                    for x in range(0, w, 5):
                        sx = w - 1 - x if fx else x; sy = h - 1 - y if fy else y
                        d += abs(t[c * w * h + y * w + x] - box(c, sx, sy, mode)); n += 1
            print(f'{mode} flipx {fx} flipy {fy}: mean abs diff {d / n:.2f}')
