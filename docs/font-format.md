# dmview Font Format (`.dmvf`)

Status: **version 0.1**.

A `.dmvf` file is a font rendered for one pixel size: every glyph is an
antialiased bitmap with 4 bits of coverage per pixel, so `libdmview` only
blends it - no outlines, no rasterizer on the device. The structures are in
`include/dmview_format.h` (`dmvf_header_t`, `dmvf_glyph_t`). Everything is
little-endian.

```
┌──────────────┐ 0
│ header       │ 32 bytes
├──────────────┤ glyphs
│ glyphs       │ 12 bytes each, codepoints ascending
├──────────────┤ bitmaps
│ bitmaps      │ 4 bits per pixel
└──────────────┘ file_size
```

## Making a font

```bash
tools/ttf2dmvf.py Roboto-Regular.ttf 16 sans-16.dmvf
tools/ttf2dmvf.py --chars 0x20-0x7E Roboto-Bold.ttf 24 title.dmvf    # ASCII only - smaller
```

`--chars` takes codepoint ranges; the default is printable ASCII, Latin-1
and Latin Extended-A (Polish, Czech, German, ... - about 320 glyphs; Roboto
16 px: 21 KiB, ASCII only: 5.4 KiB). Each glyph is rendered 8 times larger and
averaged down to the pixel grid, so the coverage follows the font's outlines
rather than what grid fitting at small sizes makes of them. Needs Pillow.

dmview's [fonts/](../fonts) are Roboto (Apache 2.0) made this way:
`sans-N.dmvf` (Regular) and `sans-bold-N.dmvf` (Bold) for N = 12, 16, 20,
24, 32.

## Header

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | magic | `'D' 'M' 'V' 'F'` |
| 4 | 2 | version_major | 0 - a reader rejects any other |
| 6 | 2 | version_minor | 1 |
| 8 | 4 | file_size | Size of the whole file |
| 12 | 2 | size | Pixel size the font was rendered for (em) |
| 14 | 2 | line_height | Distance of two baselines |
| 16 | 2 | ascent | Baseline below the top of a line |
| 18 | 2 | descent | Below the baseline |
| 20 | 4 | glyph_count | Glyphs |
| 24 | 4 | glyphs | Offset of the glyph table (4-aligned) |
| 28 | 4 | bitmaps | Offset of the bitmaps |

## Glyph (12 bytes)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | bitmap | Offset of its bitmap from `bitmaps` |
| 4 | 2 | codepoint | Unicode (BMP), strictly ascending through the table |
| 6 | 1 | width | Bitmap width |
| 7 | 1 | height | Bitmap height |
| 8 | 1 | left | Bitmap's left edge right of the pen (signed) |
| 9 | 1 | top | Bitmap's top edge above the baseline (signed) |
| 10 | 1 | advance | Pen movement after the glyph |
| 11 | 1 | reserved | 0 |

A blank glyph (the space) has width and height 0 and only an advance.

## Bitmaps

`height` rows of `(width + 1) / 2` bytes, two pixels per byte - the left
one in the low nibble - each the coverage 0 (nothing) ... 15 (all of the
pixel). Drawing a pixel blends the text's color (or gradient) with alpha
coverage x 17.

## Reading a font

`libdmview` loads a font file into memory once and shares it between the
views that use it. Before using it, it checks:

- the magic, the major version and `file_size`;
- the glyph table and every glyph's bitmap lie inside the file;
- the codepoints are strictly ascending, reserved bytes are 0.

A file that fails is not used: the view gets the built-in font. Text finds
ASCII glyphs through a table built when the font is loaded and the others by
a binary search; a character the font does not have is drawn as its `?`.
