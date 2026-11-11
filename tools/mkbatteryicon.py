#!/usr/bin/env python3
"""mkbatteryicon.py - the Battery program's icon (bsp/classpad50/SRC/APPS/BATTERY/battery.ico)

    mkbatteryicon.py OUT.ico [PREVIEW.png]

A Windows CE 5.0 era icon: pixel art in the 16 standard VGA colours, drawn
at 32x32 and again at 16x16 (not scaled down), one-pixel black outlines, the
light from the upper left. An upright AA cell, green jacket with a white
gleam, bright metal cap and contact, with a yellow lightning bolt in front of
its lower right. The file holds both sizes as 4-bit images with 1-bit masks,
which is what the CE shell and its 16-colour icons of the time used.
The optional PNG shows both sizes enlarged on the shell's two usual
backgrounds, for looking at the result.
"""
import struct
import sys
import zlib

# the 16 standard colours, in the order of the Windows 4-bit palette
PALETTE = [
    (0x00, 0x00, 0x00), (0x80, 0x00, 0x00), (0x00, 0x80, 0x00), (0x80, 0x80, 0x00),
    (0x00, 0x00, 0x80), (0x80, 0x00, 0x80), (0x00, 0x80, 0x80), (0xC0, 0xC0, 0xC0),
    (0x80, 0x80, 0x80), (0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0xFF, 0xFF, 0x00),
    (0x00, 0x00, 0xFF), (0xFF, 0x00, 0xFF), (0x00, 0xFF, 0xFF), (0xFF, 0xFF, 0xFF),
]
K, DKGREEN, OLIVE, LTGREY, DKGREY, GREEN, YELLOW, WHITE = 0, 2, 3, 7, 8, 10, 11, 15
T = None    # transparent


def blank(n):
    return [[T] * n for _ in range(n)]


def column_shade(colours, x0, x1, x):
    """The colour of column x of a cylinder spanning x0..x1, from a list given left to right."""
    return colours[(x - x0) * len(colours) // (x1 - x0 + 1)]


def cell(img, x0, x1, nub0, nub1, y_nub, y_top, y_cap, y_foot, y_bottom):
    """An upright cell: outline columns x0 and x1, contact nub0..nub1 from row y_nub,
    metal cap rows y_top+1..y_cap-1, jacket to y_foot-1, metal foot to y_bottom-1."""
    jacket = [DKGREEN, GREEN, WHITE, GREEN, GREEN, GREEN, DKGREEN, DKGREEN, DKGREEN, DKGREEN, DKGREEN, DKGREEN]
    metal = [DKGREY, LTGREY, WHITE, WHITE, LTGREY, LTGREY, LTGREY, DKGREY, DKGREY, DKGREY, DKGREY, DKGREY]
    # the contact
    for y in range(y_nub, y_top):
        for x in range(nub0, nub1 + 1):
            edge = x in (nub0, nub1) or y == y_nub
            img[y][x] = K if edge else column_shade([LTGREY, WHITE, LTGREY, DKGREY], nub0 + 1, nub1 - 1, x)
    # the body
    for y in range(y_top, y_bottom + 1):
        for x in range(x0, x1 + 1):
            if y in (y_top, y_bottom) or x in (x0, x1):
                corner = (y in (y_top, y_bottom)) and x in (x0, x1)
                img[y][x] = T if corner else K
            elif y in (y_cap, y_foot):
                img[y][x] = K                                   # the seams
            elif y < y_cap or y > y_foot:
                img[y][x] = column_shade(metal, x0 + 1, x1 - 1, x)
            else:
                img[y][x] = column_shade(jacket, x0 + 1, x1 - 1, x)


def bolt(img, points):
    """A filled yellow polygon with a black outline one pixel outside it."""
    n = len(img)
    inside = [[False] * n for _ in range(n)]
    for y in range(n):
        yc = y + 0.5
        xs = []
        for i in range(len(points)):
            (xa, ya), (xb, yb) = points[i], points[(i + 1) % len(points)]
            if (ya <= yc < yb) or (yb <= yc < ya):
                xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
        xs.sort()
        for i in range(0, len(xs) - 1, 2):
            for x in range(n):
                if xs[i] <= x + 0.5 < xs[i + 1]:
                    inside[y][x] = True
    for y in range(n):
        for x in range(n):
            if inside[y][x]:
                # the lit edge: the first pixel of each run on the left is white, olive on the lower right
                left = x == 0 or not inside[y][x - 1]
                right = x == n - 1 or not inside[y][x + 1]
                img[y][x] = WHITE if left and not right else OLIVE if right and not left and n > 16 else YELLOW
            else:
                near = any(0 <= y + dy < n and 0 <= x + dx < n and inside[y + dy][x + dx]
                           for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (-1, -1), (1, -1), (-1, 1)))
                if near:
                    img[y][x] = K


def icon32():
    img = blank(32)
    cell(img, x0=4, x1=17, nub0=8, nub1=13, y_nub=1, y_top=4, y_cap=9, y_foot=26, y_bottom=30)
    bolt(img, [(25, 9), (17, 20), (22, 20), (18, 31), (30, 17), (24, 17), (28, 9)])
    return img


def icon16():
    img = blank(16)
    cell(img, x0=2, x1=8, nub0=4, nub1=6, y_nub=0, y_top=2, y_cap=5, y_foot=12, y_bottom=15)
    bolt(img, [(12, 4), (8, 10), (11, 10), (9, 16), (15, 8), (12, 8), (14, 4)])
    return img


def dib(img):
    """BITMAPINFOHEADER, palette, 4-bit XOR rows and 1-bit AND rows, bottom-up."""
    n = len(img)
    xor_stride = ((n * 4 + 31) // 32) * 4
    and_stride = ((n + 31) // 32) * 4
    out = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 4, 0, (xor_stride + and_stride) * n, 0, 0, 16, 0)
    for r, g, b in PALETTE:
        out += bytes((b, g, r, 0))
    for row in reversed(img):
        line = bytearray(xor_stride)
        for x, c in enumerate(row):
            line[x // 2] |= (c or 0) << (4 if x % 2 == 0 else 0)
        out += bytes(line)
    for row in reversed(img):
        line = bytearray(and_stride)
        for x, c in enumerate(row):
            if c is None:
                line[x // 8] |= 0x80 >> (x % 8)
        out += bytes(line)
    return out


def ico(images):
    dibs = [dib(i) for i in images]
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, d in zip(images, dibs):
        n = len(img)
        out += struct.pack('<BBBBHHII', n, n, 16, 0, 1, 4, len(d), offset)
        offset += len(d)
    return out + b''.join(dibs)


def png(path, images, scale=8):
    """Both sizes enlarged, on the desktop's blue and on the menu's grey."""
    backgrounds = [(0x3A, 0x6E, 0xA5), (0xC0, 0xC0, 0xC0), (0xFF, 0xFF, 0xFF)]
    pad = 8
    tiles = []
    for bg in backgrounds:
        for img in images:
            tiles.append((img, bg, scale))
        tiles.append((images[0], bg, 1))
        tiles.append((images[1], bg, 1))
    w = sum(len(i) * s + 2 * pad for i, _, s in tiles)
    h = max(len(i) * s for i, _, s in tiles) + 2 * pad
    px = [[(0x20, 0x20, 0x20)] * w for _ in range(h)]
    x0 = 0
    for img, bg, s in tiles:
        n = len(img)
        for y in range(h):
            for x in range(x0, x0 + n * s + 2 * pad):
                px[y][x] = bg
        for y in range(n):
            for x in range(n):
                c = img[y][x]
                if c is None:
                    continue
                for dy in range(s):
                    for dx in range(s):
                        px[pad + y * s + dy][x0 + pad + x * s + dx] = PALETTE[c]
        x0 += n * s + 2 * pad
    raw = b''.join(b'\x00' + bytes(v for p in row for v in p) for row in px)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    images = [icon32(), icon16()]
    open(sys.argv[1], 'wb').write(ico(images))
    if len(sys.argv) > 2:
        png(sys.argv[2], images)


if __name__ == '__main__':
    main()
