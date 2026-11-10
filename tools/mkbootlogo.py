#!/usr/bin/env python3
"""mkbootlogo.py - the two pictures on the loader's black start screen.

    mkbootlogo.py <xp-flag.jpg> <fontdir> <outdir> [flag width]

wordmark.png: "Microsoft(R) Windows(R) CE 5.0", white on black, set from text
the way the Windows CE 5.0 header art sets it: Microsoft and 5.0 in Franklin
Gothic Book, Windows and CE in Franklin Gothic Medium, all tracked -55/1000 em
(<fontdir> holds "Franklin Gothic Book.TTF" and "Franklin Gothic Medium.ttf").
Sizes and positions are the art's, in its pixels (SPEC, measured on the
207x155 original): Windows is fitted to the art's width, CE shares its size,
Microsoft is fitted to its width over Windows, 5.0 keeps the art's figure
height. Drawn at SS x and box-filtered down, so the antialiasing is the
fonts' own, on black.

flag.png: the XP flag from a picture of it on a checkerboard (fake
transparency). The panes are saturated and the checkerboard is grey, so the
mask is saturation > 50, closed to fill the pane highlights; the flag is
scaled down over black (default 36 px wide, the flag's width in the header
art, where it sits left of the lettering), which antialiases its edge against
black with no fringe. Needs Pillow.
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

SS = 8
TRACK = -55 / 1000                     # em
ORIGIN = (36, 61)                      # wordmark.png's top-left in the art
SIZE = (174, 32)
BOOK, MEDIUM = 'Franklin Gothic Book.TTF', 'Franklin Gothic Medium.ttf'

# (text, face, ink left, baseline, fit, value): fit 'width' sets the size so
# the ink is value px wide, 'height' so the first glyph's ink is value px tall,
# 'size' takes another entry's size (value = its index)
SPEC = [
    ('Windows',   MEDIUM,  39, 91, 'width', 103),
    ('CE',        MEDIUM, 148, 91, 'size', 0),
    ('Microsoft', BOOK,    38, 70, 'width', 39),
    ('5.0',       BOOK,   182, 91, 'height', 15),
]

def run(text, path, px):
    """text tracked by TRACK at px: (ink mask, its bbox, pen x, baseline y)"""
    f = ImageFont.truetype(path, max(1, round(px)))
    pad = round(px)
    im = Image.new('L', (round(px * (len(text) + 2)), round(px * 2)))
    d = ImageDraw.Draw(im)
    base = round(px * 1.5)
    for i, c in enumerate(text):
        d.text((pad + f.getlength(text[:i]) + i * TRACK * px, base), c, font=f, fill=255, anchor='ls')
    return im, im.getbbox(), pad, base

def wordmark(fontdir, scale=1.0):
    u = SS * scale
    out = Image.new('L', (round(SIZE[0] * u), round(SIZE[1] * u)))
    sizes, ends = [], []
    for text, face, left, base, fit, value in SPEC:
        path = f'{fontdir}/{face}'
        if fit == 'size':
            px = sizes[value]
        else:
            _, bb, _, b = run(text if fit == 'width' else text[0], path, 1000)
            px = 1000 * value * u / ((bb[2] - bb[0]) if fit == 'width' else (b - bb[1]))
        sizes.append(px)
        im, bb, _, b = run(text, path, px)
        x, y = round((left - ORIGIN[0]) * u) - bb[0], round((base - ORIGIN[1]) * u) - b
        out.paste(255, (x, y), im)
        ends.append((x + bb[2], y + b, px))
    # registered marks as in the art: 3 px tall, Book, just right of the word, the
    # Microsoft one at its cap top (row 62), the Windows one at its x-height (row 75)
    for i, top in ((2, 62), (0, 75)):
        x_end = ends[i][0]
        im, bb, _, _ = run('\u00ae', f'{fontdir}/{BOOK}', 1000)
        im = im.crop(bb)
        h = round(3.2 * u); im = im.resize((round(im.width * h / im.height), h), Image.LANCZOS)
        out.paste(255, (x_end + round(0.5 * u), round((top - ORIGIN[1]) * u)), im)
    return Image.merge('RGB', [out.resize((round(SIZE[0] * scale), round(SIZE[1] * scale)), Image.BOX)] * 3)

def flag(path, width):
    im = Image.open(path).convert('RGB')
    mask = im.convert('HSV').getchannel('S').point(lambda s: 255 if s > 50 else 0)
    mask = mask.filter(ImageFilter.MaxFilter(7)).filter(ImageFilter.MinFilter(7))
    box = mask.getbbox()
    im, mask = im.crop(box), mask.crop(box)
    size = (width, round(im.height * width / im.width))
    out = Image.new('RGB', im.size, 'black')
    out.paste(im, (0, 0), mask)
    return out.resize(size, Image.LANCZOS)

def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    flagsrc, fontdir, outdir = sys.argv[1:4]
    width = int(sys.argv[4]) if len(sys.argv) == 5 else 36
    wordmark(fontdir).save(f'{outdir}/wordmark.png')
    flag(flagsrc, width).save(f'{outdir}/flag.png')

if __name__ == '__main__':
    main()
