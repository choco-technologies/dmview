#!/usr/bin/env python3
"""ttf2dmvf - render a TrueType/OpenType font into a dmview font file (.dmvf).

Glyphs are rasterized once, here, at the pixel size the view asks for, with
antialiasing, and stored with 4 bits of coverage per pixel - libdmview only
blends them. See docs/font-format.md.

Each glyph is rendered SUPERSAMPLE times larger and reduced to the pixel
grid by averaging: the coverage follows the font's outlines, without what
small-size grid fitting does to them (a 't' growing into the ':' after it).

    tools/ttf2dmvf.py Roboto-Regular.ttf 16 sans-16.dmvf
    tools/ttf2dmvf.py --chars 0x20-0x7E Roboto-Medium.ttf 24 title.dmvf

Needs Pillow (python3-pil).
"""

import argparse
import math
import struct
import sys

from PIL import Image, ImageFont

HEADER = struct.Struct('<4sHHIHHhhIII')     # dmvf_header_t, 32 bytes
GLYPH = struct.Struct('<IHBBbbBx')          # dmvf_glyph_t, 12 bytes

# Printable ASCII, Latin-1 and Latin Extended-A (Polish, Czech, German, ...)
DEFAULT_CHARS = '0x20-0x7E,0xA0-0x17F'
MAX_CODEPOINT = 0xFFFF
SUPERSAMPLE = 8


def parse_chars(spec):
    codepoints = set()
    for part in spec.split(','):
        first, _, last = part.partition('-')
        first = int(first, 0)
        last = int(last, 0) if last else first
        if not 0 <= first <= last <= MAX_CODEPOINT:
            sys.exit('ttf2dmvf: codepoints are 0 ... 0xFFFF')
        codepoints.update(range(first, last + 1))
    return sorted(codepoints)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('font', help='TrueType / OpenType font')
    parser.add_argument('size', type=int, help='pixel size (em), 4 ... 127')
    parser.add_argument('output', help='.dmvf file to write')
    parser.add_argument('--chars', default=DEFAULT_CHARS,
                        help='codepoint ranges, e.g. 0x20-0x7E,0x104 (default: %(default)s)')
    args = parser.parse_args()

    if not 4 <= args.size <= 127:
        sys.exit('ttf2dmvf: the size must be 4 ... 127')
    font = ImageFont.truetype(args.font, args.size)
    big = ImageFont.truetype(args.font, args.size * SUPERSAMPLE)
    ascent, descent = font.getmetrics()
    q = SUPERSAMPLE

    glyphs, bitmaps = [], bytearray()
    missing = 0
    for cp in parse_chars(args.chars):
        ch = chr(cp)
        mask, (ox, oy) = big.getmask2(ch, mode='L', anchor='ls')
        image = Image.frombytes('L', mask.size, bytes(mask)) if mask.size[0] and mask.size[1] else None
        ink = image.getbbox() if image is not None else None
        if ink is None and cp not in (0x20, 0xA0):
            missing += 1            # Not in the font (or blank)
            continue
        advance = int(round(big.getlength(ch) / q))

        width = height = left = top = 0
        small = None
        if ink is not None:
            # The ink, relative to the pen at the baseline, in big pixels -
            # snapped out to whole target pixels and averaged down
            x0, y0, x1, y1 = ox + ink[0], oy + ink[1], ox + ink[2], oy + ink[3]
            left, upper = math.floor(x0 / q), math.floor(y0 / q)
            width, height = math.ceil(x1 / q) - left, math.ceil(y1 / q) - upper
            canvas = Image.new('L', (width * q, height * q), 0)
            canvas.paste(image.crop(ink), (x0 - left * q, y0 - upper * q))
            small = canvas.resize((width, height), Image.BOX)
            top = -upper
        if width > 255 or height > 255 or advance > 255 or not -128 <= left <= 127 or not -128 <= top <= 127:
            sys.exit('ttf2dmvf: U+%04X is too large for the format' % cp)

        offset = len(bitmaps)
        for y in range(height):
            row = [(small.getpixel((x, y)) * 15 + 127) // 255 for x in range(width)]
            if width % 2:
                row.append(0)
            bitmaps += bytes(row[i] | (row[i + 1] << 4) for i in range(0, len(row), 2))
        glyphs.append((offset, cp, width, height, left, top, advance))

    glyphs_at = HEADER.size
    bitmaps_at = glyphs_at + GLYPH.size * len(glyphs)
    size = bitmaps_at + len(bitmaps)
    out = bytearray(HEADER.pack(b'DMVF', 0, 1, size, args.size, ascent + descent, ascent, descent,
                                len(glyphs), glyphs_at, bitmaps_at))
    for g in glyphs:
        out += GLYPH.pack(*g)
    out += bitmaps
    with open(args.output, 'wb') as f:
        f.write(out)
    print('%s: %d glyphs, %d bytes%s' % (args.output, len(glyphs), size,
                                         ', %d not in the font' % missing if missing else ''))


if __name__ == '__main__':
    main()
