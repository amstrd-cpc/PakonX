"""Our framing runs (metric 255 - mean 8-bit RGB transmission, d-line bins) at several
thresholds vs the OEM's DxCode runs mapped to our lines (line = off + d * bin)."""
import array, json, struct, sys
raw, side, d, off = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
oem = [tuple(map(int, p.split('-'))) for p in sys.argv[5].split(',')]
sc = json.load(open(side)); c = sc['corrections']; g = sc['geometry']
sh = g['film']['start'] - g['calibration']['start']
if g['film'].get('resample'): sh = sh * 3 // 4
f = open(raw, 'rb'); spr, rows = struct.unpack_from('<II', f.read(44), 0x0C); px = spr // 3
CENTRAL = float(sys.argv[8]) if len(sys.argv) > 8 else 1.0
SCALE = float(sys.argv[9]) if len(sys.argv) > 9 else 255.0
lo_ = int(px * (1 - CENTRAL) / 2); STRIDE = int(sys.argv[10]) if len(sys.argv) > 10 else 8
idx = range(lo_, px - lo_, STRIDE)
dk = [[c['dark']['planes'][ch][p + sh] for p in idx] for ch in range(3)]
wt = [[max(1.0, c['white']['planes'][ch][p + sh] - dk[ch][k]) for k, p in enumerate(idx)] for ch in range(3)]
line = []
for r in range(rows):
    a = array.array('H'); a.frombytes(f.read(spr * 2)); s = 0.0
    for ch in range(3):
        s += sum(min(16383.0, max(0.0, 16000 * (a[3 * p + ch] - dk[ch][k]) / wt[ch][k])) // 64 for k, p in enumerate(idx)) / len(idx)
    line.append(s / 3)
start = off // d
m = [255 - int(SCALE * sum(line[i * d:(i + 1) * d]) / d) for i in range(rows // d)]
print('OEM runs (bins rel. OEM):', ' '.join(f'{a}-{b}({b-a+1})' for a, b in oem))
for t in range(int(sys.argv[6]), int(sys.argv[7]), 2):
    runs = []; i = start
    while i < len(m):
        if m[i] > t:
            j = i
            while j < len(m) and m[j] > t: j += 1
            if j - i > 3: runs.append((i - start, j - 1 - start))
            i = j
        else: i += 1
    print(f'thr {t}:', ' '.join(f'{a}-{b}({b-a+1})' for a, b in runs if a >= -5))
