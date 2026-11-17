"""Write the dumped bytes of (seq, arg, phase) from PakonIMAu_proxy.bin to a file; print addr/len."""
import struct, sys
path, seq, arg, phase, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
b = open(path, 'rb').read(); o = 0
while o + 24 <= len(b):
    magic, s, a, ph, addr, n = struct.unpack_from('<6I', b, o)
    if s == seq and a == arg and ph == phase:
        open(out, 'wb').write(b[o + 24:o + 24 + n]); print(f'{out}: addr {addr:#010x} len {n}'); break
    o += 24 + n
else:
    print('not found', seq, arg, phase)
