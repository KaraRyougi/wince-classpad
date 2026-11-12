#!/usr/bin/env python3
"""mkcetuneicon.py - CEtune's icon (bsp/classpad50/SRC/APPS/CETUNE/cetune.ico) from Ptune's

    mkcetuneicon.py XTUNE/assets-cg/icon-uns.png OUT.ico [PREVIEW.png]

CEtune uses the icon of Ptune (Xtune's fx-CG add-in icon, 92x64: a crimson
badge with a speed dial, "beta" across it, "min" and "max" at its feet;
Xtune is MIT licensed, copyright Xtune contributors). An add-in icon is a
wide badge above a blank strip for the name; a Windows icon is a square. So
the dial, which is the picture, is cut out of the badge (64x36 pixels) and
set on a square of the badge's own colours with its dark edge:

    32x32   the dial at exactly half size (2x2 box filter), 32x18
    16x16   the dial at a quarter (4x4 box filter), 16x9

Both are stored as 8-bit images with their own palettes and 1-bit masks (the
corners of the square are transparent). The source PNG is not in this
repository: pass the path of an unpacked Xtune tree. The optional PNG shows
both sizes enlarged on the shell's two usual backgrounds.
"""
import struct
import sys
import zlib

DIAL = (14, 6, 78, 42)      # left, top, right, bottom of the dial in the 92x64 source
RED_AT = (8, 24)            # a pixel of the badge's face, left of the dial
EDGE_AT = (5, 20)           # a pixel of the badge's edge


def read_png(path):
    """8-bit RGBA, non-interlaced -> rows of (r, g, b, a)."""
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    p, idat = 8, b''
    while p < len(d):
        n, tag = struct.unpack('>I4s', d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        p += 12 + n
        if tag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert (depth, ctype, interlace) == (8, 6, 0), 'expected 8-bit RGBA, not interlaced'
        elif tag == b'IDAT':
            idat += body
    raw, stride, rows, prev, q = zlib.decompress(idat), w * 4, [], bytes(w * 4), 0
    for _ in range(h):
        f, line = raw[q], bytearray(raw[q + 1:q + 1 + stride])
        q += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * 4:x * 4 + 4]) for x in range(w)])
        prev = line
    return rows


def shrink(rows, box, k):
    """The box of the source reduced k times by averaging k x k pixels."""
    l, t, r, b = box
    out = []
    for y in range(t, b, k):
        line = []
        for x in range(l, r, k):
            px = [rows[yy][xx] for yy in range(y, y + k) for xx in range(x, x + k)]
            line.append(tuple((sum(p[c] for p in px) + len(px) // 2) // len(px) for c in range(3)))
        out.append(line)
    return out


def icon(rows, n):
    """n x n: the badge's face with its edge, corners cut, the dial across the middle."""
    face, edge = rows[RED_AT[1]][RED_AT[0]][:3], rows[EDGE_AT[1]][EDGE_AT[0]][:3]
    img = [[face] * n for _ in range(n)]
    for i in range(n):
        img[0][i] = img[n - 1][i] = img[i][0] = img[i][n - 1] = edge
    cut = 2 if n >= 32 else 1
    for y in range(n):
        for x in range(n):
            dx, dy = min(x, n - 1 - x), min(y, n - 1 - y)
            if dx + dy < cut:
                img[y][x] = None                    # transparent corner
            elif dx + dy == cut and (dx == 0 or dy == 0 or cut == 2):
                img[y][x] = edge
    # the badge's face is shaded from top to bottom; around the dial it becomes
    # the one colour of the square, or the cut-out would show as a band
    flat = [[face + (255,) if (p[0] > 150 and p[1] < 110 and p[2] < 140) else p for p in line] for line in rows]
    dial = shrink(flat, DIAL, (DIAL[2] - DIAL[0]) // n)
    top = (n - len(dial)) // 2
    for y, line in enumerate(dial):
        for x, c in enumerate(line):
            if 0 < x < n - 1:                       # the side edges stay
                img[top + y][x] = c
    return img


def palette_of(img):
    """At most 256 colours: the image's own, with the low bits dropped until they fit."""
    for bits in range(8, 2, -1):
        mask = (0xFF << (8 - bits)) & 0xFF
        def q(c, mask=mask, bits=bits):
            return tuple((v & mask) | ((v & mask) >> bits) for v in c)
        colours = sorted({q(c) for line in img for c in line if c is not None})
        if len(colours) <= 256:
            return colours, q
    raise SystemExit('too many colours')


def dib(img):
    n = len(img)
    colours, q = palette_of(img)
    index = {c: i for i, c in enumerate(colours)}
    xor_stride, and_stride = (n + 3) & ~3, ((n + 31) // 32) * 4
    out = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 8, 0, (xor_stride + and_stride) * n, 0, 0, 256, 0)
    for i in range(256):
        r, g, b = colours[i] if i < len(colours) else (0, 0, 0)
        out += bytes((b, g, r, 0))
    for line in reversed(img):
        out += bytes(index[q(c)] if c is not None else 0 for c in line).ljust(xor_stride, b'\0')
    for line in reversed(img):
        bits = 0
        for x, c in enumerate(line):
            if c is None:
                bits |= 0x80000000 >> x
        out += struct.pack('>I', bits)[:and_stride] if n <= 32 else b''
    return out


def ico(images):
    dibs = [dib(i) for i in images]
    out, offset = struct.pack('<HHH', 0, 1, len(images)), 6 + 16 * len(images)
    for img, d in zip(images, dibs):
        out += struct.pack('<BBBBHHII', len(img), len(img), 0, 0, 1, 8, len(d), offset)
        offset += len(d)
    return out + b''.join(dibs)


def png(path, images, scale=8):
    """Both sizes, enlarged, on the desktop's teal and on the menu's grey."""
    grounds, pad = [(0, 128, 128), (192, 192, 192)], 8
    w = sum(len(i) * scale + pad for i in images) + pad
    h = (max(len(i) for i in images) * scale + pad) * len(grounds) + pad
    canvas = [[(255, 255, 255)] * w for _ in range(h)]
    for gi, ground in enumerate(grounds):
        x0 = pad
        y0 = pad + gi * (max(len(i) for i in images) * scale + pad)
        for img in images:
            for y, line in enumerate(img):
                for x, c in enumerate(line):
                    for yy in range(scale):
                        for xx in range(scale):
                            canvas[y0 + y * scale + yy][x0 + x * scale + xx] = c if c is not None else ground
            x0 += len(img) * scale + pad
    raw = b''.join(b'\0' + bytes(v for px in line for v in px) for line in canvas)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    rows = read_png(sys.argv[1])
    assert (len(rows[0]), len(rows)) == (92, 64), 'expected the 92x64 add-in icon'
    images = [icon(rows, 32), icon(rows, 16)]
    open(sys.argv[2], 'wb').write(ico(images))
    if len(sys.argv) > 3:
        png(sys.argv[3], images)


if __name__ == '__main__':
    main()
