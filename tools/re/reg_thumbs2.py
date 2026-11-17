"""Place TLB's frames on our scan by their Ansel thumbnails (250x375, 180-rotated d x d box
of the frame). Args: raw side d thumb... ; prints the first line of each OEM frame."""
import array, json, math, struct, sys
raw, side, d = sys.argv[1], sys.argv[2], int(sys.argv[3]); thumbs = sys.argv[4:]
sc = json.load(open(side)); c = sc['corrections']; sh = sc['geometry']['film']['start'] - sc['geometry']['calibration']['start']
if sc['geometry']['film'].get('resample'): sh = sh * 3 // 4  # 3/4 horizontal resample (Base 8)
f = open(raw, 'rb'); hd = f.read(44); spr, rows = struct.unpack_from('<II', hd, 0x0C); px = spr // 3
idx = range(0, px, 6)
dk = [c['dark']['planes'][1][p + sh] for p in idx]; wt = [max(1.0, c['white']['planes'][1][p + sh] - dk[k]) for k, p in enumerate(idx)]
ours = []
for r in range(rows):
    a = array.array('H'); a.frombytes(f.read(spr * 2))
    ours.append(sum(-math.log10(max(1e-4, (a[3 * p + 1] - dk[k]) / wt[k])) for k, p in enumerate(idx)) / len(idx))
binned = [sum(ours[i:i + d]) / d for i in range(rows - d)]
def ncc(a, b):
    ma = sum(a) / len(a); mb = sum(b) / len(b)
    va = math.sqrt(sum((x - ma) ** 2 for x in a)); vb = math.sqrt(sum((x - mb) ** 2 for x in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (va * vb + 1e-12)
for t in thumbs:
    tb = array.array('H'); tb.frombytes(open(t, 'rb').read()); w = 250; h = len(tb) // (3 * w)
    prof = [sum(tb[w * h + y * w + x] for x in range(w)) / w for y in range(h)][::-1]
    best = max((ncc(prof, [binned[s + d * y] for y in range(h)]), s) for s in range(0, rows - d * h - d, 2))
    s = best[1]
    best = max((ncc(prof, [binned[q + d * y] for y in range(h)]), q) for q in range(max(0, s - 3), s + 4))
    print(f'{t.split("/")[-1]}: {h} rows, first line {best[1]} (ncc {best[0]:.4f})')
