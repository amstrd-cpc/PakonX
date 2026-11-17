"""Place TLB's frames on our scan: each Ansel thumbnail (250x375, 180-rotated 4x reduction
of a 1000x1500 frame) is matched by per-row mean green density against our per-line mean
(4-line bins). Prints the first line of each OEM frame in our .pakraw."""
import array, json, math, struct, sys
raw, side = sys.argv[1], sys.argv[2]; thumbs = sys.argv[3:]
sc = json.load(open(side)); c = sc['corrections']; sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
f = open(raw, 'rb'); hd = f.read(44); spr, rows = struct.unpack_from('<II', hd, 0x0C); px = spr // 3
idx = range(0, px, 4)
d = [c['dark']['planes'][1][p + sh] for p in idx]; wt = [max(1.0, c['white']['planes'][1][p + sh] - d[k]) for k, p in enumerate(idx)]
ours = []
for r in range(rows):
    a = array.array('H'); a.frombytes(f.read(spr * 2))
    ours.append(sum(-math.log10(max(1e-4, (a[3 * p + 1] - d[k]) / wt[k])) for k, p in enumerate(idx)) / len(idx))
def ncc(a, b):
    ma = sum(a) / len(a); mb = sum(b) / len(b)
    va = math.sqrt(sum((x - ma) ** 2 for x in a)); vb = math.sqrt(sum((x - mb) ** 2 for x in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (va * vb + 1e-12)
for t in thumbs:
    tb = array.array('H'); tb.frombytes(open(t, 'rb').read()); w, h = 250, 375
    prof = [sum(tb[w * h + y * w + x] for x in range(w)) / w for y in range(h)][::-1]  # undo 180
    best = max(((ncc(prof, [sum(ours[s + 4 * y:s + 4 * y + 4]) / 4 for y in range(h)]), s) for s in range(0, rows - 1500, 2)))
    print(f'{t.split("/")[-1]}: first line {best[1]} (ncc {best[0]:.4f})')
