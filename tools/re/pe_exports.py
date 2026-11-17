"""List PE imports (dll!name) and exports of a 32-bit DLL. Pure stdlib."""
import struct, sys
b = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', b, 0x3c)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]; opt = pe + 24; optsz = struct.unpack_from('<H', b, pe + 20)[0]
dd = opt + 96  # PE32 data directories
secs = []
for i in range(nsec):
    s = pe + 24 + optsz + 40 * i
    va, vs, raw, rs = struct.unpack_from('<IIII', b, s + 8 + 4)[0:1] + struct.unpack_from('<I', b, s + 8)[0:1] + struct.unpack_from('<II', b, s + 16)
    secs.append((struct.unpack_from('<I', b, s + 12)[0], struct.unpack_from('<I', b, s + 8)[0], struct.unpack_from('<I', b, s + 20)[0]))
def off(rva):
    for va, vs, raw in secs:
        if va <= rva < va + max(vs, 1): return raw + rva - va
def cstr(rva):
    o = off(rva); e = b.index(b'\0', o); return b[o:e].decode('latin1')
erva = struct.unpack_from('<I', b, dd)[0]
if erva:
    o = off(erva); n_names, af, an, ao = struct.unpack_from('<IIII', b, o + 24)[0], *struct.unpack_from('<III', b, o + 28)
    for i in range(n_names):
        name = cstr(struct.unpack_from('<I', b, off(an) + 4 * i)[0]); ordn = struct.unpack_from('<H', b, off(ao) + 2 * i)[0]
        print('EXPORT', hex(struct.unpack_from('<I', b, off(af) + 4 * ordn)[0] + struct.unpack_from('<I', b, opt + 28)[0]), name)
irva = struct.unpack_from('<I', b, dd + 8)[0]
o = off(irva)
while True:
    oft, _, _, nm, ft = struct.unpack_from('<IIIII', b, o)
    if nm == 0: break
    dll = cstr(nm); t = off(oft or ft); names = []
    while True:
        v = struct.unpack_from('<I', b, t)[0]
        if v == 0: break
        names.append(f'#{v & 0xffff}' if v & 0x80000000 else cstr(v + 2)); t += 4
    print('IMPORT', dll, len(names), ' '.join(names[:40]))
    o += 20
