"""Last session of a PakonIMAu_proxy.log, per roll: calls and geometry args."""
import re, sys
text = open(sys.argv[1], 'rb').read().decode('latin1')
calls = re.split(r'\n(?=#\d+ PI\w+ tid)', text)
sess = []
for c in calls:
    m = re.match(r'#(\d+) (PI\w+) tid', c)
    if not m: continue
    if m.group(2) == 'PIBegin': sess.append([])
    if sess: sess[-1].append((int(m.group(1)), m.group(2), c))
s = sess[int(sys.argv[2]) if len(sys.argv) > 2 else -1]
def args(c):
    m = re.search(r'args: ([0-9a-f ]+)', c); return [int(x, 16) for x in m.group(1).split()]
def dump(c, k):
    m = re.search(r'\n    a%d @[0-9a-f]+ \[\d+\]: ([0-9a-f ]+)' % k, c); return bytes.fromhex(m.group(1).replace(' ', '')) if m else b''
import struct
for n, name, c in s:
    a = args(c)
    if name == 'PIAnselStartNewRoll': print(f'#{n} StartNewRoll type {a[1]}')
    elif name == 'PIScaleAndRotatePlanar': print(f'#{n} ScaleRot in {a[2]}x{a[3]} out {a[4]}x{a[5]} planes {a[6]} angle {a[7]:#x} mode {a[8]}')
    elif name == 'PIAnselAddScene':
        d = dump(c, 2); w = struct.unpack_from('<6I', d, 0) if len(d) >= 24 else ()
        print(f'#{n} AddScene desc {w} dmin {struct.unpack_from("<3I", d, 0x54) if len(d) >= 0x60 else ()}')
    elif name == 'PIAnselColorSceneBalancePlanar':
        d = dump(c, 2); print(f'#{n} Balance desc {struct.unpack_from("<6I", d, 0) if len(d) >= 24 else ()}')
    elif name == 'PIColorAdjustPlanar':
        d = dump(c, 0); print(f'#{n} ColorAdjust desc {struct.unpack_from("<4I", d, 0) if len(d) >= 16 else ()}')
    elif name == 'PISaveFilePlanar_8':
        p = dump(c, 4).decode('utf-16le', 'ignore').split('\0')[0]
        print(f'#{n} Save {a[1]}x{a[2]} planes {a[3]} fmt {a[5]} q {a[6]} dpi {a[7]} {p}')
    elif name in ('PIRotatePlanar',): pass
    else: print(f'#{n} {name}')
