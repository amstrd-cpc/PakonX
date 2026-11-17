"""Refine line/pixel offset and R/B shift for the OEM density dump, then fit
3x4 and 3x10 (R G B R2 G2 B2 RG RB GB 1) models. Prints best alignment + matrices."""
import array, json, math, struct, sys
raw, side, dump, w, h, off0 = sys.argv[1], sys.argv[2], sys.argv[3], *map(int, sys.argv[4:7])
sc = json.load(open(side)); c = sc['corrections']
sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
f = open(raw, 'rb'); hd = f.read(44); spr = struct.unpack_from('<I', hd, 0x0C)[0]; px = spr // 3
o = array.array('H'); o.frombytes(open(dump, 'rb').read()); n = w * h
cache = {}
def row(r):
    if r not in cache:
        f.seek(44 + r * spr * 2); a = array.array('H'); a.frombytes(f.read(spr * 2)); cache[r] = a
    return cache[r]
dk = [c['dark']['planes'][ch] for ch in range(3)]; wh = [c['white']['planes'][ch] for ch in range(3)]
def D(ch, r, p):
    if p < 0 or p >= px: return None
    d = dk[ch][p + sh]; t = (row(r)[3 * p + ch] - d) / max(1.0, wh[ch][p + sh] - d)
    return -1000 * math.log10(max(1e-4, t))
def solve(X, y):
    k = len(X[0]); A = [[sum(xi[i] * xi[j] for xi in X) for j in range(k)] for i in range(k)]; b = [sum(xi[i] * yi for xi, yi in zip(X, y)) for i in range(k)]
    for i in range(k):
        piv = max(range(i, k), key=lambda r: abs(A[r][i])); A[i], A[piv] = A[piv], A[i]; b[i], b[piv] = b[piv], b[i]
        for r in range(k):
            if r != i:
                m = A[r][i] / A[i][i]; A[r] = [x - m * yv for x, yv in zip(A[r], A[i])]; b[r] -= m * b[i]
    return [b[i] / A[i][i] for i in range(k)]
def fit(feat, Y):
    m = solve(feat, Y); pred = [sum(a * b for a, b in zip(m, xi)) for xi in feat]
    my = sum(Y) / len(Y); ss = sum((a - b) ** 2 for a, b in zip(Y, pred)); st = sum((a - my) ** 2 for a in Y)
    return 1 - ss / st, math.sqrt(ss / len(Y)), m
samples = [(y, x) for y in range(30, h - 30, 19) for x in range(30, w - 30, 13)]
def build(off, poff, s, quad=False):
    X = []; Y = [[], [], []]
    for y, x in samples:
        r = off + y; p = x + poff
        v = (D(0, r - s, p), D(1, r, p), D(2, r + s, p))
        if None in v: continue
        R, G, B = v
        X.append([R, G, B, R * R / 1000, G * G / 1000, B * B / 1000, R * G / 1000, R * B / 1000, G * B / 1000, 1.0] if quad else [R, G, B, 1.0])
        for ch in range(3): Y[ch].append(o[ch * n + y * w + x])
    return X, Y
best = None
for off in range(off0 - 3, off0 + 4):
    for poff in range(-3, 4):
        X, Y = build(off, poff, 4)
        r2 = fit(X, Y[1])[0]
        if best is None or r2 > best[0]: best = (r2, off, poff)
print('best line offset %d pixel offset %d (G R2 %.5f)' % (best[1], best[2], best[0]))
for s in (3, 4, 5):
    X, Y = build(best[1], best[2], s)
    print('shift', s, ' '.join('%s R2 %.5f rms %.1f' % ("RGB"[ch], *fit(X, Y[ch])[:2]) for ch in range(3)))
for quad in (False, True):
    X, Y = build(best[1], best[2], 4, quad)
    print('3x10' if quad else '3x4')
    for ch in range(3):
        r2, rms, m = fit(X, Y[ch]); print('  %s R2 %.5f rms %.1f  %s' % ("RGB"[ch], r2, rms, ' '.join('%.4f' % v for v in m)))
