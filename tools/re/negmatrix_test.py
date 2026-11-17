"""Test TLB's density model on an OEM PakonIMAu input dump: TLB = NegMatrix (3x10, registry
TLB\\ColorKodak, terms R G B R2 G2 B2 RG RB GB 1) applied to X_c = a * D_c + b_c, D_c = ours
-log10(t_c). Gauss-Newton over (a, b_R, b_G, b_B). Args: raw side dump w h line_off pixel_off [fixed a [fixed b]]"""
import array, json, math, struct, sys
raw, side, dump = sys.argv[1:4]; w, h, off, poff = map(int, sys.argv[4:8])
NM = [[0.291037, -0.010605, 0.009786, -0.000003, -0.000001, -0.000005, 0.000002, -0.000003, 0.000007, 173.227005],
      [0.008807, 0.305601, -0.022396, 0.000000, -0.000004, -0.000005, -0.000002, -0.000002, 0.000013, 447.127625],
      [0.002900, -0.049366, 0.346690, -0.000000, 0.000030, 0.000037, -0.000002, 0.000001, -0.000070, 623.031494]]
sc = json.load(open(side)); c = sc['corrections']
sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
if sc['geometry']['film'].get('resample'): sh = sh * 3 // 4  # 3/4 horizontal resample (Base 8)
f = open(raw, 'rb'); spr = struct.unpack_from('<I', f.read(44), 0x0C)[0]; px = spr // 3
o = array.array('H'); o.frombytes(open(dump, 'rb').read()); n = w * h
dk = [c['dark']['planes'][ch] for ch in range(3)]; wh = [c['white']['planes'][ch] for ch in range(3)]
def row(r):
    f.seek(44 + r * spr * 2); a = array.array('H'); a.frombytes(f.read(spr * 2)); return a
S = []
for y in range(30, h - 30, 23):
    rr, rg, rb = row(off + y - 4), row(off + y), row(off + y + 4)
    for x in range(30, w - 30, 17):
        p = x + poff; D = []
        for ch, rw in enumerate((rr, rg, rb)):
            d = dk[ch][p + sh]; t = (rw[3 * p + ch] - d) / max(1.0, wh[ch][p + sh] - d)
            D.append(-math.log10(max(1e-4, t)))
        S.append((D, [o[ch * n + y * w + x] for ch in range(3)]))
def model(par, D):
    a, b = par[0], par[1:]
    X = [a * D[k] + b[k] for k in range(3)]; R, G, B = X
    T = [R, G, B, R * R, G * G, B * B, R * G, R * B, G * B, 1.0]
    return [sum(m * t for m, t in zip(NM[ch], T)) for ch in range(3)]
def resid(par):
    return [mo - y for D, Y in S for mo, y in zip(model(par, D), Y)]
par = [float(sys.argv[8]) if len(sys.argv) > 8 else 3500.0, 0.0, 0.0, 0.0]
free = [] if len(sys.argv) > 9 else [1, 2, 3] if len(sys.argv) > 8 else [0, 1, 2, 3]
if len(sys.argv) > 9: par[1:] = [float(sys.argv[9])] * 3
nf = len(free)
for it in range(12 if free else 0):
    r0 = resid(par); J = []
    for k in free:
        q = list(par); q[k] += 1e-3 * (abs(q[k]) + 1); rk = resid(q)
        J.append([(x - y) / (q[k] - par[k]) for x, y in zip(rk, r0)])
    M = [[sum(J[i][m] * J[j][m] for m in range(len(r0))) for j in range(nf)] +
         [-sum(J[i][m] * r0[m] for m in range(len(r0)))] for i in range(nf)]
    for i in range(nf):
        pv = max(range(i, nf), key=lambda r: abs(M[r][i])); M[i], M[pv] = M[pv], M[i]
        for r in range(nf):
            if r != i:
                fct = M[r][i] / M[i][i]; M[r] = [x - fct * y for x, y in zip(M[r], M[i])]
    for i, k in enumerate(free):
        par[k] += M[i][nf] / M[i][i]
r = resid(par)
for ch in range(3):
    e = r[ch::3]; Y = [Yv[ch] for _, Yv in S]; my = sum(Y) / len(Y)
    print('RGB'[ch], 'rms %.1f' % math.sqrt(sum(v * v for v in e) / len(e)), 'R2 %.5f' % (1 - sum(v * v for v in e) / sum((y - my) ** 2 for y in Y)), 'mean %.1f' % (sum(e) / len(e)))
print('a %.2f b %s' % (par[0], ' '.join('%.1f' % v for v in par[1:])))
