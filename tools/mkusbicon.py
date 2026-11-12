#!/usr/bin/env python3
"""mkusbicon.py - the "USB Connection" Control Panel icon (bsp/classpad50/SRC/APPS/USBCPL/usb.ico)

    mkusbicon.py OUT.ico [PREVIEW.png]

In the manner of Windows XP's icons, to sit next to the ones CE 5.0's Control
Panel has: saturated colour, soft gradients lit from the upper left, outlines
in a darker shade of the fill instead of black, a gloss on the plastic. A USB
A plug pointing up: the silver shell with its two holes, a blue moulded grip
carrying the white USB trident, the start of the cable.

The picture is described once, in a 32-unit square, as shapes with gradients,
and rendered at 32x32 and 16x16 by supersampling (8x8 samples per pixel), so
the inside is smooth at both sizes. CE's icons have no alpha channel: a pixel
is opaque when half of it is covered, and its colour is that of the covered
part. Each size is stored as an 8-bit image with its own palette (at most 256
colours) and a 1-bit mask.
"""
import struct
import sys
import zlib

SS = 8      # samples per pixel edge


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def rgb(h):
    return ((h >> 16) & 255, (h >> 8) & 255, h & 255)


def rrect(x0, y0, x1, y1, r):
    """inside test of a rounded rectangle"""
    def inside(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = x0 + r if x < x0 + r else x1 - r if x > x1 - r else x
        cy = y0 + r if y < y0 + r else y1 - r if y > y1 - r else y
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return inside


def circle(cx, cy, r):
    return lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def segment(ax, ay, bx, by, w):
    """inside test of a thick line with round ends"""
    def inside(x, y):
        dx, dy = bx - ax, by - ay
        t = ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)
        t = max(0.0, min(1.0, t))
        px, py = ax + t * dx, ay + t * dy
        return (x - px) ** 2 + (y - py) ** 2 <= w * w
    return inside


def triangle(a, b, c):
    def side(p, q, x, y):
        return (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0])
    def inside(x, y):
        d = (side(a, b, x, y), side(b, c, x, y), side(c, a, x, y))
        return all(v >= 0 for v in d) or all(v <= 0 for v in d)
    return inside


def scene():
    """(inside, colour) pairs, painted in order; coordinates in a 32-unit square"""
    white = rgb(0xFFFFFF)
    shapes = []

    # the cable, behind the grip
    shapes.append((rrect(12.6, 23.0, 19.4, 31.6, 1.2), lambda x, y: rgb(0x4A5361)))
    shapes.append((rrect(13.5, 23.0, 18.5, 30.7, 0.8),
                   lambda x, y: mix(rgb(0xD5DAE2), rgb(0x79828F), (x - 13.5) / 5.0)))

    # the metal shell
    shapes.append((rrect(9.2, 0.6, 22.8, 13.5, 1.2), lambda x, y: rgb(0x5E6876)))
    shapes.append((rrect(10.2, 1.6, 21.8, 13.5, 0.7),
                   lambda x, y: mix(mix(rgb(0xFFFFFF), rgb(0xAEB7C4), (x - 10.2) / 11.6), rgb(0xE9EDF3), 0.35 * (1 - (y - 1.6) / 12))))
    for sx in (12.3, 17.1):                             # its two holes, a light lower lip under each
        shapes.append((rrect(sx, 4.0, sx + 2.6, 7.9, 0.3), lambda x, y: rgb(0xF7F9FC)))
        shapes.append((rrect(sx, 4.0, sx + 2.6, 7.5, 0.3), lambda x, y: mix(rgb(0x242B36), rgb(0x4B5565), (y - 4.0) / 3.5)))

    # the grip: outline, body gradient, gloss
    shapes.append((rrect(5.4, 10.6, 26.6, 25.6, 3.4), lambda x, y: rgb(0x113A8C)))
    def body(x, y):
        c = mix(rgb(0x6FB0FF), rgb(0x1A53C2), (y - 11.6) / 13.0)
        return mix(c, rgb(0x0F3F9E), 0.55 * max(0.0, (x - 19.0) / 6.6))       # the side away from the light
    shapes.append((rrect(6.4, 11.6, 25.6, 24.6, 2.5), body))
    shapes.append((rrect(7.4, 12.4, 24.6, 16.2, 1.8),
                   lambda x, y: mix(mix(rgb(0xC9E2FF), rgb(0x86BBFF), (y - 12.4) / 3.8), body(x, y), 0.25)))

    # the trident
    for inside in (segment(16.0, 14.6, 16.0, 22.6, 0.55), triangle((16.0, 12.9), (14.5, 15.2), (17.5, 15.2)),
                   circle(16.0, 22.6, 1.25),
                   segment(16.0, 20.6, 12.8, 18.6, 0.5), segment(12.8, 18.6, 12.8, 17.6, 0.5), circle(12.8, 17.2, 1.0),
                   segment(16.0, 21.6, 19.2, 19.6, 0.5), segment(19.2, 19.6, 19.2, 18.8, 0.5), rrect(18.3, 16.9, 20.1, 18.7, 0.1)):
        shapes.append((inside, lambda x, y: white))
    return shapes


def render(n):
    """n x n pixels: rows of (r, g, b) or None for transparent"""
    shapes = scene()
    scale = 32.0 / n
    out = []
    for py in range(n):
        row = []
        for px in range(n):
            acc = [0.0, 0.0, 0.0]
            cover = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = (px + (sx + 0.5) / SS) * scale
                    y = (py + (sy + 0.5) / SS) * scale
                    colour = None
                    for inside, paint in shapes:
                        if inside(x, y):
                            colour = paint(x, y)
                    if colour is not None:
                        cover += 1
                        for i in range(3):
                            acc[i] += colour[i]
            if cover * 2 >= SS * SS:
                row.append(tuple(int(max(0, min(255, acc[i] / cover)) + 0.5) for i in range(3)))
            else:
                row.append(None)
        out.append(row)
    return out


def quantise(img):
    """the image with at most 256 colours: low bits dropped until it fits"""
    for bits in range(8, 2, -1):
        m = 0xFF & ~((1 << (8 - bits)) - 1)
        q = [[None if p is None else tuple((c & m) | ((c & m) >> bits) for c in p) for p in row] for row in img]
        palette = sorted({p for row in q for p in row if p is not None})
        if len(palette) <= 256:
            return q, palette
    raise SystemExit('cannot quantise')


def dib(img):
    """BITMAPINFOHEADER, 256-entry palette, 8-bit XOR rows and 1-bit AND rows, bottom-up"""
    n = len(img)
    q, palette = quantise(img)
    index = {c: i for i, c in enumerate(palette)}
    xor_stride = (n + 3) // 4 * 4
    and_stride = ((n + 31) // 32) * 4
    out = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 8, 0, (xor_stride + and_stride) * n, 0, 0, 256, 0)
    for i in range(256):
        r, g, b = palette[i] if i < len(palette) else (0, 0, 0)
        out += bytes((b, g, r, 0))
    for row in reversed(q):
        out += bytes(index[p] if p is not None else 0 for p in row).ljust(xor_stride, b'\0')
    for row in reversed(q):
        line = bytearray(and_stride)
        for x, p in enumerate(row):
            if p is None:
                line[x // 8] |= 0x80 >> (x % 8)
        out += bytes(line)
    return out


def ico(images):
    dibs = [dib(i) for i in images]
    out = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for img, d in zip(images, dibs):
        n = len(img)
        out += struct.pack('<BBBBHHII', n, n, 0, 0, 1, 8, len(d), offset)
        offset += len(d)
    return out + b''.join(dibs)


def png(path, images, scale=6):
    """both sizes, enlarged and at their own size, on the Control Panel's white and on two other backgrounds"""
    backgrounds = [(255, 255, 255), (192, 192, 192), (58, 110, 165)]
    pane_w = images[0] and (len(images[0]) * scale + len(images[1]) * scale + len(images[0]) + len(images[1]) + 50)
    pane_h = len(images[0]) * scale + 20
    w, h = pane_w * len(backgrounds), pane_h
    px = [[backgrounds[x // pane_w] for x in range(w)] for _ in range(h)]

    def blit(img, ox, oy, s):
        q, _ = quantise(img)
        for y, row in enumerate(q):
            for x, p in enumerate(row):
                if p is None:
                    continue
                for dy in range(s):
                    for dx in range(s):
                        px[oy + y * s + dy][ox + x * s + dx] = p

    for k in range(len(backgrounds)):
        ox = k * pane_w + 10
        blit(images[0], ox, 10, scale)
        ox += len(images[0]) * scale + 10
        blit(images[1], ox, 10, scale)
        ox += len(images[1]) * scale + 10
        blit(images[0], ox, 10, 1)
        blit(images[1], ox + len(images[0]) + 10, 10, 1)

    raw = b''.join(b'\0' + bytes(c for p in row for c in p) for row in px)

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    images = [render(32), render(16)]
    open(sys.argv[1], 'wb').write(ico(images))
    if len(sys.argv) > 2:
        png(sys.argv[2], images)


if __name__ == '__main__':
    main()
