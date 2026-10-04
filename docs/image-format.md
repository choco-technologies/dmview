# dmview Image Format (`.dmvi`)

Status: **version 0.1**.

A `.dmvi` file is an image in the form `libdmview` draws it: a header and
the pixels in one of a few raw formats - nothing to decode on the device,
at most to unpack (see [Compression](#compression)).
Other formats (PNG, JPEG, ...) are converted into `.dmvi` by **todmvi**
(see [assembly.md](assembly.md#formats)). The structures are in
`include/dmview_format.h` (`dmvi_header_t`, `dmvi_format_t`). Everything is
little-endian.

```
┌──────────────┐ 0
│ header       │ 56 bytes
├──────────────┤ pixels
│ pixels       │ height rows, stride bytes apart
├──────────────┤ alpha        (RGB565A8)
│ alpha plane  │ height rows, alpha_stride bytes apart
├──────────────┤ palette      (I8)
│ palette      │ palette_count x 4 bytes
└──────────────┘ 56 + unpacked_size
```

A compressed file holds the header and the packed rest (`file_size` bytes
in all); the picture above is what it unpacks into.

## Header

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | magic | `'D' 'M' 'V' 'I'` |
| 4 | 2 | version_major | 0 - a reader rejects any other |
| 6 | 2 | version_minor | 1 |
| 8 | 4 | file_size | Size of the whole file |
| 12 | 2 | width | Pixels per row, > 0 |
| 14 | 2 | height | Rows, > 0 |
| 16 | 1 | format | Pixel format, see below |
| 17 | 1 | reserved | 0 |
| 18 | 2 | palette_count | I8: colors in the palette, 1 ... 256; else 0 |
| 20 | 4 | stride | Bytes from one row of pixels to the next |
| 24 | 4 | pixels | Offset of the pixels, 4-aligned |
| 28 | 4 | alpha_stride | RGB565A8: bytes from one alpha row to the next; else 0 |
| 32 | 4 | alpha | RGB565A8: offset of the alpha plane; else 0 |
| 36 | 4 | palette | I8: offset of the palette, 4-aligned; else 0 |
| 40 | 12 | compression | Compression of everything after the header - a dmod compression name (`"fastlz"`), zero-terminated and zero-padded; empty: not compressed |
| 52 | 4 | unpacked_size | Bytes after the header once unpacked (not compressed: `file_size` - 56) |

The offsets count from the start of the unpacked image - the same whether
the file is compressed or not.

## Pixel formats

| Value | Name | Bytes per pixel | Pixel |
|-------|------|-----------------|-------|
| 1 | `RGB565` | 2 | `RRRRRGGG GGGBBBBB` (16-bit), opaque |
| 2 | `ARGB8888` | 4 | `0xAARRGGBB` (32-bit) |
| 3 | `RGB565A8` | 2 + 1 | RGB565 pixels, and the alpha of each in the alpha plane (0 transparent ... 255 opaque) |
| 4 | `I8` | 1 | Index into the palette of `0xAARRGGBB` colors; an index past `palette_count` is transparent |
| 5 | `A8` | 1 | Coverage 0 ... 255 - a mask |
| 6 | `A4` | 1/2 | Coverage 0 ... 15, two pixels per byte, the left one in the low nibble - a mask |

`stride` is at least the bytes of a row (`A4`: `(width + 1) / 2`) and a
multiple of the pixel size (2 for `RGB565` / `RGB565A8`, 4 for `ARGB8888`),
so rows can be padded to 4 bytes. `alpha_stride` is at least `width`.

Which to choose:

- **`RGB565`** for opaque images on an RGB565 screen (photos, backgrounds):
  drawn by copying rows, the fastest;
- **`RGB565A8`** for images with transparency on an RGB565 screen - 3 bytes
  per pixel, the colors copied as they are, only the edges blended;
- **`ARGB8888`** for an ARGB8888 screen, or where 5:6:5 colors are not good
  enough;
- **`I8`** for images with few colors (up to 256, with alpha) - 1 byte per
  pixel;
- **`A8` / `A4`** for icons drawn with `ICON` in any color or gradient -
  `A4` takes half the memory and is usually enough.

## Compression

Everything after the header - pixels, alpha plane, palette - may be packed
as one block with dmod's compression (`Dmod_Compression_Pack()` - the one
compressed modules, `.dmfc`, use; FastLZ where `DMOD_USE_FASTLZ` is on).
`libdmview` unpacks it with `Dmod_Compression_Unpack()` when the image is
loaded and draws from the unpacked pixels, so drawing costs the same. A
file whose compression this dmod does not have is not shown (and logged).

It saves storage and load time, not memory: an image is kept unpacked
while it is shown. Flat graphics, gradients and masks pack well; photos
hardly do - todmvi stores an image unpacked when packing gains nothing.

## Drawing

`IMAGE` draws the colors of an image (formats 1 ... 4), `ICON` its coverage:
the alpha of every pixel (formats 5 and 6, or the alpha of the others;
`RGB565` covers everything) painted with a color or a gradient. An opaque
image in the screen's own pixel format (`RGB565` on RGB565, `ARGB8888`
without transparent pixels on ARGB8888) is copied row by row; everything
else is converted and blended pixel by pixel.

## Reading an image

`libdmview` loads an image file into memory once and shares it between the
views that show it. Before using it, it checks:

- the magic, the major version and `file_size`; a compressed image unpacks
  into exactly `unpacked_size` bytes;
- the format, a non-zero size, the reserved byte;
- the pixels, the alpha plane and the palette lie inside the file, with the
  alignment above; the fields the format does not use are 0.

A file that fails is not shown (and logged). While loading, `libdmview`
also finds out whether every pixel is opaque, so an opaque `ARGB8888`,
`RGB565A8` or `I8` image is drawn without blending.
