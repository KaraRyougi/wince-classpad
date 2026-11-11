#!/usr/bin/env python3
"""mkbootlogo.py - the two pictures on the loader's black start screen.

    mkbootlogo.py <ce50-header.png> <xp-flag.jpg> <outdir> [flag width]

wordmark.png: the "Microsoft Windows CE 5.0" lettering cut from the Windows
CE 5.0 header art (207x155) at its original resolution, without the art's
flag. The lettering is grey on white, so white on black is 255 - luminance
per pixel; its antialiasing carries over unchanged.

flag.png: the XP flag from a picture of it on a checkerboard (fake
transparency). The panes are saturated and the checkerboard is grey, so the
mask is saturation > 50, closed to fill the pane highlights; the flag is
scaled down over black (default 36 px wide, the flag's width in the header
art, where it sits left of the lettering), which antialiases its edge against
black with no fringe. Needs Pillow.
"""
import sys
from PIL import Image, ImageFilter, ImageOps

TEXT_BOX = (36, 61, 207, 93)          # the lettering inside the header art

def wordmark(path):
    art = Image.open(path).convert('L')
    return ImageOps.invert(art.crop(TEXT_BOX)).convert('RGB')

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
    art, flagsrc, outdir = sys.argv[1:4]
    width = int(sys.argv[4]) if len(sys.argv) == 5 else 36
    wordmark(art).save(f'{outdir}/wordmark.png')
    flag(flagsrc, width).save(f'{outdir}/flag.png')

if __name__ == '__main__':
    main()
