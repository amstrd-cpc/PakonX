"""Print dumped struct words from PakonIMAu_proxy.bin: seq, arg, phase, count."""
import struct, sys
path, seq, arg, phase, count = sys.argv[1], *map(int, sys.argv[2:6])
b = open(path, 'rb').read(); o = 0
while o + 24 <= len(b):
    magic, s, a, ph, addr, n = struct.unpack_from('<6I', b, o)
    if magic != 0x58504B50: print('bad magic at', o); break
    if s == seq and a == arg and ph == phase:
        words = struct.unpack_from('<%dI' % min(count, n // 4), b, o + 24)
        for i in range(0, len(words), 8): print(f'  +{4*i:#05x}', ' '.join(f'{w:08x}' for w in words[i:i + 8]))
        break
    o += 24 + n
