"""Pixel fit: OEM planar density (w x h u16 planar) vs our density D_c = -1000 log10(t_c)
on lines [off, off+h). Tries pixel flip and R/B line shifts; least-squares 3x4 per channel."""
import array, json, math, struct, sys
raw, side, dump, w, h, off = sys.argv[1], sys.argv[2], sys.argv[3], *map(int, sys.argv[4:7])
sc = json.load(open(side)); c = sc['corrections']
sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
f = open(raw, 'rb'); hd = f.read(44); spr = struct.unpack_from('<I', hd, 0x0C)[0]; px = spr // 3
o = array.array('H'); o.frombytes(open(dump, 'rb').read()); n = w * h
cache = {}
def row(r):
    if r not in cache:
        f.seek(44 + r * spr * 2); a = array.array('H'); a.frombytes(f.read(spr * 2)); cache[r] = a
    return cache[r]
def D(ch, r, p):
    dk = c['dark']['planes'][ch][p + sh]; wt = max(1.0, c['white']['planes'][ch][p + sh] - dk)
    return -1000 * math.log10(max(1e-4, (row(r)[3 * p + ch] - dk) / wt))
def solve(X, y):  # normal equations 4x4
    k = len(X[0]); A = [[sum(xi[i] * xi[j] for xi in X) for j in range(k)] for i in range(k)]; b = [sum(xi[i] * yi for xi, yi in zip(X, y)) for i in range(k)]
    for i in range(k):
        piv = max(range(i, k), key=lambda r: abs(A[r][i])); A[i], A[piv] = A[piv], A[i]; b[i], b[piv] = b[piv], b[i]
        for r in range(k):
            if r != i:
                m = A[r][i] / A[i][i]; A[r] = [x - m * yv for x, yv in zip(A[r], A[i])]; b[r] -= m * b[i]
    return [b[i] / A[i][i] for i in range(k)]
samples = [(y, x) for y in range(20, h - 20, 23) for x in range(20, w - 20, 17)]
for flip in (False, True):
    for s in (0, 4):
        X = []; Y = [[], [], []]
        for y, x in samples:
            p = (w - 1 - x) if flip else x; r = off + y
            X.append([D(0, r - s, p), D(1, r, p), D(2, r + s, p), 1.0])
            for ch in range(3): Y[ch].append(o[ch * n + y * w + x])
        out = []
        for ch in range(3):
            m = solve(X, Y[ch]); pred = [sum(a * b for a, b in zip(m, xi)) for xi in X]
            my = sum(Y[ch]) / len(Y[ch]); ss = sum((a - b) ** 2 for a, b in zip(Y[ch], pred)); st = sum((a - my) ** 2 for a in Y[ch])
            rms = math.sqrt(ss / len(pred)); out.append((1 - ss / st, rms, m))
        print(f'pixel-flip {flip} shift {s}: ' + ' | '.join(f'{"RGB"[i]} R2 {r2:.5f} rms {rms:.1f} m {[round(v, 3) for v in m]}' for i, (r2, rms, m) in enumerate(out)))
