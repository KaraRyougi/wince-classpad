#!/usr/bin/env python3
"""mkloaderart.py - the loader's pictures as C (dbt/hhk/art_data.c).

    mkloaderart.py <ce50-header.png> <xp-flag.jpg> <out.c> [preview.png]

Draws the icons of docs/design/loader-luna.html with the mockup's geometry
and colours (iconChip, iconBox, iconBoth, iconGo, iconWarn, iconErr, radio,
stepIcon and the title bar's close button), 4x supersampled and box-filtered
down, and adds the pictures tools/mkbootlogo.py describes: the XP flag at
17 px (title bar) and 36 px (start screen) and the "Microsoft Windows CE 5.0"
lettering cut from the CE 5.0 header art. Each becomes RGB565 pixels plus a
4-bit alpha plane (hhk/gfx.h gfx_image); the lettering, white on black, is a
4-bit coverage mask only. Text inside icons uses hinted 1-bit Tahoma, like
the loader's other text. Needs Pillow; the optional preview.png shows every
picture at 4x.
"""
import os, sys
from PIL import Image, ImageDraw, ImageFont
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkbootlogo

S = 4
TAHOMA_B = '/System/Library/Fonts/Supplemental/Tahoma Bold.ttf'

def hexrgb(h):
    h = h.lstrip('#')
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))

def grad_color(stops, t):
    for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
        if t <= t1:
            return lerp(hexrgb(c0), hexrgb(c1), 0 if t1 == t0 else (t - t0) / (t1 - t0))
    return hexrgb(stops[-1][1])

class Icon:
    """an RGBA canvas in icon pixels, drawn at S x"""
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.im = Image.new('RGBA', (w * S, h * S), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
    def P(self, pts, ox=0, oy=0, k=1.0):
        return [((ox + x * k) * S, (oy + y * k) * S) for x, y in pts]
    def fill_mask(self, mask, fill):
        """fill = colour or function(y_in_icon_px) -> colour, for vertical gradients"""
        if callable(fill):
            layer = Image.new('RGBA', self.im.size)
            ld = ImageDraw.Draw(layer)
            for y in range(self.im.size[1]):
                ld.line([(0, y), (self.im.size[0], y)], fill=fill((y + 0.5) / S) + (255,))
        else:
            layer = Image.new('RGBA', self.im.size, hexrgb(fill) + (255,))
        self.im.paste(layer, (0, 0), mask)
    def mask(self):
        return Image.new('L', self.im.size, 0)
    def poly(self, pts, fill):
        m = self.mask(); ImageDraw.Draw(m).polygon(pts, fill=255); self.fill_mask(m, fill)
    def ellipse(self, cx, cy, r, fill):
        m = self.mask(); ImageDraw.Draw(m).ellipse([(cx - r) * S, (cy - r) * S, (cx + r) * S, (cy + r) * S], fill=255)
        self.fill_mask(m, fill)
    def rrect(self, x, y, w, h, r, fill):
        m = self.mask(); ImageDraw.Draw(m).rounded_rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], r * S, fill=255)
        self.fill_mask(m, fill)
    def stroke(self, pts, colour, width=1.0, closed=False):
        m = self.mask(); d = ImageDraw.Draw(m)
        p = list(pts) + ([pts[0]] if closed else [])
        d.line(p, fill=255, width=max(1, round(width * S)), joint='curve')
        self.fill_mask(m, colour)
    def circle_stroke(self, cx, cy, r, colour, width=1.0):
        m = self.mask()
        ImageDraw.Draw(m).ellipse([(cx - r - width / 2) * S, (cy - r - width / 2) * S, (cx + r + width / 2) * S, (cy + r + width / 2) * S], outline=255, width=round(width * S))
        self.fill_mask(m, colour)
    def rrect_stroke(self, x, y, w, h, r, colour):
        m = self.mask()
        ImageDraw.Draw(m).rounded_rectangle([x * S, y * S, (x + w) * S - 1, (y + h) * S - 1], r * S, outline=255, width=S)
        self.fill_mask(m, colour)
    def text(self, s, size, colour, cx, base):
        """1-bit hinted Tahoma Bold, centred on cx, baseline base (icon px), pasted 1:1 then scaled"""
        f = ImageFont.truetype(TAHOMA_B, size)
        t = Image.new('L', (size * 3, size * 3), 0)
        td = ImageDraw.Draw(t); td.fontmode = '1'
        w = td.textlength(s, font=f)
        td.text((size * 1.5 - w / 2, size * 2), s, font=f, fill=255, anchor='ls')
        t = t.resize((t.width * S, t.height * S), Image.NEAREST)
        self.fill_mask_at(t, colour, round((cx - size * 1.5) * S), round((base - size * 2) * S))
    def fill_mask_at(self, m, colour, x, y):
        full = self.mask(); full.paste(m, (x, y)); self.fill_mask(full, colour)
    def result(self):
        return self.im.resize((self.w, self.h), Image.BOX)

def chip(ic, ox=0, oy=0, k=1.0):
    q = lambda x, y: ((ox + x * k), (oy + y * k))
    for i in range(5):
        for x, y, w, h in ((7 + i * 4, 2, 2, 4), (7 + i * 4, 26, 2, 4), (2, 7 + i * 4, 4, 2), (26, 7 + i * 4, 4, 2)):
            x0, y0 = q(x, y)
            ic.rrect(x0, y0, w * k, h * k, 0, '#8A8A8A')
    x0, y0 = q(5, 5)
    ic.rrect(x0, y0, 22 * k, 22 * k, 3 * k, lambda y: grad_color([(0, '#6F9BF5'), (1, '#1F4FC4')], (y - y0) / (22 * k)))
    ic.rrect_stroke(x0, y0, 22 * k, 22 * k, 3 * k, '#123A8F')
    if k == 1.0:
        ic.text('CE', 9, '#FFFFFF', ox + 16, oy + 20)

def box(ic, ox=0, oy=0, k=1.0):
    P = lambda pts: ic.P(pts, ox, oy, k)
    ic.poly(P([(4, 10), (16, 4), (28, 10), (16, 16)]), '#F4D58D')
    ic.poly(P([(4, 10), (16, 16), (16, 30), (4, 24)]), '#D9A441')
    ic.poly(P([(28, 10), (16, 16), (16, 30), (28, 24)]), '#B98020')
    ic.stroke(P([(4, 10), (16, 4), (28, 10), (28, 24), (16, 30), (4, 24)]), '#7A5310', k, closed=True)
    ic.stroke(P([(4, 10), (16, 16), (28, 10)]), '#7A5310', k)
    ic.stroke(P([(16, 16), (16, 30)]), '#7A5310', k)

def go(ic):
    ic.ellipse(16, 16, 13, lambda y: grad_color([(0, '#8BE07F'), (1, '#1E8F1E')], (y - 3) / 26))
    ic.circle_stroke(16, 16, 13, '#13691A')
    ic.poly(ic.P([(12, 9), (23, 16), (12, 23)]), '#FFFFFF')

def warn(ic):
    tri = ic.P([(16, 3), (30, 28), (2, 28)])
    ic.poly(tri, lambda y: grad_color([(0, '#FFE680'), (1, '#F2B800')], (y - 3) / 25))
    ic.stroke(tri, '#A07400', 1, closed=True)
    ic.text('!', 15, '#000000', 16, 25)

def err(ic):
    ic.ellipse(16, 16, 14, lambda y: grad_color([(0, '#F7907B'), (1, '#C4200C')], (y - 2) / 28))
    ic.circle_stroke(16, 16, 14, '#8E1606')
    ic.stroke(ic.P([(10, 10), (22, 22)]), '#FFFFFF', 3)
    ic.stroke(ic.P([(22, 10), (10, 22)]), '#FFFFFF', 3)

def radio(on, dis):
    ic = Icon(13, 13)
    ic.ellipse(6.5, 6.5, 6, '#F5F4EA' if dis else '#FFFFFF')
    ic.circle_stroke(6.5, 6.5, 6, '#C9C7BA' if dis else '#1C5180')
    if on:
        a, b = hexrgb('#9CF08F'), hexrgb('#A7A6AA' if dis else '#1E9B1E')
        m = ic.mask(); ImageDraw.Draw(m).ellipse([3 * S, 3 * S, 10 * S, 10 * S], fill=255)
        layer = Image.new('RGBA', ic.im.size)
        px = layer.load()
        for y in range(layer.size[1]):
            for x in range(layer.size[0]):
                t = min(1.0, (((x + .5) / S - 5.5) ** 2 + ((y + .5) / S - 5.5) ** 2) ** .5 / 3.5)
                px[x, y] = lerp(a, b, t) + (255,)
        ic.im.paste(layer, (0, 0), m)
    return ic.result()

def step(kind):
    ic = Icon(12, 12)
    if kind == 'done':
        ic.ellipse(6, 6, 6, '#2DC12B')
        ic.stroke(ic.P([(3, 6), (5.5, 8.5), (9.5, 3.8)]), '#FFFFFF', 1.8)
    elif kind == 'now':
        ic.poly(ic.P([(2, 1), (11, 6), (2, 11)]), '#316AC5')
    elif kind == 'fail':
        ic.ellipse(6, 6, 6, '#D8331F')
        ic.stroke(ic.P([(3.5, 3.5), (8.5, 8.5)]), '#FFFFFF', 1.6)
        ic.stroke(ic.P([(8.5, 3.5), (3.5, 8.5)]), '#FFFFFF', 1.6)
    else:
        ic.ellipse(6, 6, 2.5, '#B9B7AA')
    return ic.result()

def close_button():
    ic = Icon(21, 21)
    ic.rrect(0, 0, 21, 21, 3, lambda y: grad_color([(0, '#F6A28C'), (.5, '#E0533B'), (1, '#C1361E')], y / 21))
    ic.rrect_stroke(0, 0, 21, 21, 3, '#FFFFFF')
    ic.stroke(ic.P([(6, 6), (15, 15)]), '#FFFFFF', 2)
    ic.stroke(ic.P([(15, 6), (6, 15)]), '#FFFFFF', 2)
    return ic.result()

def big(fn):
    ic = Icon(32, 32); fn(ic); return ic.result()

def both():
    ic = Icon(34, 34)
    chip(ic, 0, 0, .78)
    box(ic, 13, 12, .7)
    return ic.result()

def flag_rgba(src, width):
    """the flag on transparency: its RGB over black divided out by its own alpha"""
    im = Image.open(src).convert('RGB')
    mask = im.convert('HSV').getchannel('S').point(lambda s: 255 if s > 50 else 0)
    from PIL import ImageFilter
    mask = mask.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(7))
    box_ = mask.getbbox()
    im, mask = im.crop(box_), mask.crop(box_)
    h = round(im.height * width / im.width)
    rgba = im.convert('RGBA'); rgba.putalpha(mask)
    return rgba.resize((width, h), Image.LANCZOS)

# --- C output ---------------------------------------------------------------

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def emit_image(o, name, im, alpha=True):
    im = im.convert('RGBA')
    w, h = im.size
    px = list(im.getdata())
    o.append(f'static const u16 {name}_px[{w * h}] = {{')
    vals = [rgb565(*p[:3]) for p in px]
    for i in range(0, len(vals), 12):
        o.append('    ' + ','.join(f'0x{v:04x}' for v in vals[i:i + 12]) + ',')
    o.append('};')
    if alpha:
        a = [round(p[3] * 15 / 255) for p in px] + [0]
        packed = [(a[i] << 4) | a[i + 1] for i in range(0, w * h, 2)]
        o.append(f'static const u8 {name}_a[{len(packed)}] = {{')
        for i in range(0, len(packed), 16):
            o.append('    ' + ','.join(f'0x{v:02x}' for v in packed[i:i + 16]) + ',')
        o.append('};')
    o.append(f'const gfx_image {name} = {{ {w}, {h}, {name}_px, {name + "_a" if alpha else "0"} }};')
    o.append('')

def emit_mask(o, name, im):
    g = im.convert('L')
    w, h = g.size
    a = [round(v * 15 / 255) for v in g.getdata()] + [0]
    packed = [(a[i] << 4) | a[i + 1] for i in range(0, w * h, 2)]
    o.append(f'static const u8 {name}_a[{len(packed)}] = {{')
    for i in range(0, len(packed), 16):
        o.append('    ' + ','.join(f'0x{v:02x}' for v in packed[i:i + 16]) + ',')
    o.append('};')
    o.append(f'const gfx_image {name} = {{ {w}, {h}, 0, {name}_a }};')
    o.append('')

def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    art, flag_src, out = sys.argv[1:4]
    images = {
        'art_chip': big(chip), 'art_box': big(box), 'art_both': both(), 'art_go': big(go),
        'art_warn': big(warn), 'art_err': big(err),
        'art_radio_on': radio(True, False), 'art_radio_off': radio(False, False),
        'art_radio_dis_on': radio(True, True), 'art_radio_dis_off': radio(False, True),
        'art_step_done': step('done'), 'art_step_now': step('now'), 'art_step_todo': step('todo'),
        'art_step_fail': step('fail'), 'art_close': close_button(),
        'art_flag17': flag_rgba(flag_src, 17), 'art_flag36': flag_rgba(flag_src, 36),
    }
    lettering = mkbootlogo.wordmark(art).convert('L')
    o = ['/* art_data.c - generated by tools/mkloaderart.py from the loader mockup',
         ' * (docs/design/loader-luna.html) and docs/design/art: RGB565 pixels with a 4-bit',
         ' * alpha plane, two pixels per byte, high nibble first (hhk/gfx.h). Do not edit. */',
         '#include "gfx.h"', '']
    for name, im in images.items():
        emit_image(o, name, im)
    emit_mask(o, 'art_lettering', lettering)
    open(out, 'w').write('\n'.join(o))
    if len(sys.argv) == 5:
        tiles = list(images.values()) + [lettering.convert('RGBA')]
        W = sum(t.width + 4 for t in tiles) + 4
        H = max(t.height for t in tiles) + 8
        sheet = Image.new('RGBA', (W, H), (236, 233, 216, 255))
        x = 4
        for t in tiles:
            sheet.alpha_composite(t.convert('RGBA'), (x, 4)); x += t.width + 4
        sheet.resize((W * 4, H * 4), Image.NEAREST).save(sys.argv[4])

if __name__ == '__main__':
    main()
