#!/usr/bin/env python3
# ppm2png.py <in.ppm> <out.png> - convert casio-emu HLE_FBDUMP frames (binary P6) to PNG, no dependencies.
import sys, zlib, struct
def png(w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
data = open(sys.argv[1], 'rb').read()
parts = data.split(b'\n', 3)
w, h = map(int, parts[1].split()); rgb = parts[3]
open(sys.argv[2], 'wb').write(png(w, h, rgb))
print("wrote", sys.argv[2], w, h)
