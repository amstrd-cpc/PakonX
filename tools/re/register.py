"""Register an OEM planar density dump (w x h, u16 planar R,G,B; w = CCD pixels)
against our .pakraw: per-line mean green density, correlated along the film
for both line directions. Prints best offset/orientation."""
import array, json, math, struct, sys
raw, side, dump, w, h = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
sc = json.load(open(side)); c = sc['corrections']
sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
f = open(raw, 'rb'); hd = f.read(44); spr, rows = struct.unpack_from('<II', hd, 0x0C); px = spr // 3
idx = range(0, px, 4)
d = [c['dark']['planes'][1][p + sh] for p in idx]; wt = [max(1.0, c['white']['planes'][1][p + sh] - d[k]) for k, p in enumerate(idx)]
ours = []
for r in range(rows):
    a = array.array('H'); a.frombytes(f.read(spr * 2))
    s = 0.0
    for k, p in enumerate(idx):
        s += -math.log10(max(1e-4, (a[3 * p + 1] - d[k]) / wt[k]))
    ours.append(1000 * s / len(idx))
o = array.array('H'); o.frombytes(open(dump, 'rb').read())
n = w * h
# OEM line means: plane G, assume rows = h lines of w pixels (row-major w fastest)
oem = [sum(o[n + y * w + x] for x in range(0, w, 4)) / len(range(0, w, 4)) for y in range(h)]
def ncc(a, b):
    ma = sum(a) / len(a); mb = sum(b) / len(b)
    va = math.sqrt(sum((x - ma) ** 2 for x in a)); vb = math.sqrt(sum((x - mb) ** 2 for x in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (va * vb + 1e-9)
best = []
for flip in (False, True):
    ref = oem[::-1] if flip else oem
    for off in range(0, rows - h, 2):
        best.append((ncc(ref[::3], ours[off:off + h:3]), off, flip))
best.sort(reverse=True)
for b in best[:5]: print('ncc %.4f offset %d line-flip %s' % b)
print('oem G line-mean range %.0f..%.0f, ours %.0f..%.0f' % (min(oem), max(oem), min(ours[2700:12700]), max(ours[2700:12700])))
