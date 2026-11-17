"""TLB density model (16000 open gate, -3500 log10 LUT, NegMatrix from an EEPROM dump) vs an
OEM PakonIMAu input dump: search line offset, pixel offset and R/B line shift.
Args: raw side eeprom dump w h line_off"""
import array, json, math, struct, sys
raw, side, eep, dump = sys.argv[1:5]; w, h, off0 = map(int, sys.argv[5:8])
nm = struct.unpack_from('<30f', open(eep, 'rb').read(), 0x26); NM = [nm[10 * r:10 * r + 10] for r in range(3)]
LUT = [16383] + [int(-3500 * math.log10(i / 16383)) for i in range(1, 16384)]
sc = json.load(open(side)); c = sc['corrections']; g = sc['geometry']
sh = g['film']['start'] - g['calibration']['start']
if g['film'].get('resample'): sh = sh * 3 // 4
f = open(raw, 'rb'); spr = struct.unpack_from('<I', f.read(44), 0x0C)[0]; px = spr // 3
o = array.array('H'); o.frombytes(open(dump, 'rb').read()); n = w * h
dk = [c['dark']['planes'][ch] for ch in range(3)]; wh = [c['white']['planes'][ch] for ch in range(3)]
cache = {}
def row(r):
    if r not in cache:
        f.seek(44 + r * spr * 2); a = array.array('H'); a.frombytes(f.read(spr * 2)); cache[r] = a
    return cache[r]
def D(ch, r, p):
    d = dk[ch][p + sh]; t = (row(r)[3 * p + ch] - d) / max(1.0, wh[ch][p + sh] - d)
    return LUT[min(16383, max(0, int(16000 * t)))]
def model(R, G, B):
    T = (R, G, B, R * R, G * G, B * B, R * G, R * B, G * B, 1.0)
    return [sum(m * t for m, t in zip(NM[ch], T)) for ch in range(3)]
samples = [(y, x) for y in range(40, h - 40, 37) for x in range(40, w - 40, 29)]
def score(off, poff, s, chans=(0, 1, 2)):
    e = [[], [], []]
    for y, x in samples:
        p = x + poff
        if not 0 <= p < px: continue
        pr = model(D(0, off + y - s, p), D(1, off + y, p), D(2, off + y + s, p))
        for ch in chans: e[ch].append(pr[ch] - o[ch * n + y * w + x])
    return [(math.sqrt(sum(v * v for v in q) / len(q)), sum(q) / len(q)) if q else (0, 0) for q in e]
best = min(((score(off, poff, 0, (1,))[1][0], off, poff) for off in range(off0 - 4, off0 + 5) for poff in range(-4, 5)))
print('G best line %d pixel %d rms %.1f' % (best[1], best[2], best[0]))
for s in range(max(0, int(sys.argv[8]) - 2) if len(sys.argv) > 8 else 3, (int(sys.argv[8]) + 3) if len(sys.argv) > 8 else 10):
    r = score(best[1], best[2], s); print('shift %d  R rms %.1f bias %.1f  B rms %.1f bias %.1f  G rms %.1f bias %.1f' % (s, r[0][0], r[0][1], r[2][0], r[2][1], r[1][0], r[1][1]))
