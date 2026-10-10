# dmview Binary Format (`.dmv`)

Status: **version 0.5** - follows the [assembly draft](assembly.md). Version
0.2 added gradients, 0.3 the `OPACITY` instruction, 0.4 `ICON`, 0.5 `APPEND`.

A `.dmv` file is what `libtodmv` produces from `.dmvs` assembly and what
`libdmview` executes. Everything is **little-endian**; every table starts at a
4-byte aligned offset, so 16- and 32-bit fields can be read in place.

```
┌──────────────┐ 0
│ header       │ 96 bytes (80 in version 0.1)
├──────────────┤
│ code         │ instructions, 4-byte words
├──────────────┤
│ strings      │ offsets + zero-terminated text
├──────────────┤
│ variables    │ 12 bytes each
│ fonts        │  4 bytes each
│ boxes        │  8 bytes each
│ items        │  8 bytes each
│ symbols      │  4 bytes each
│ gradients    │ 16 bytes each
│ stops        │  8 bytes each
└──────────────┘ file_size
```

The order of the tables after the header is the one `libtodmv` writes; a
reader finds them only through the header, never by position.

## Header

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | magic | `'D' 'M' 'V' 0` |
| 4 | 2 | version_major | 0 - a reader rejects any other |
| 6 | 2 | version_minor | 4 - a reader accepts this or lower |
| 8 | 4 | file_size | Size of the whole file |
| 12 | 2 | width | `.size` width, 0 = not given |
| 14 | 2 | height | `.size` height, 0 = not given |
| 16 | 2 | name | String index of `.view` |
| 18 | 2 | entry | Code word offset of `.entry` |
| 20 | 2 | longpress_ms | `.longpress` (default 600) |
| 22 | 2 | scrollslop | `.scrollslop` (default 8) |
| 24 | 8 | code | Table: offset, count of 4-byte words |
| 32 | 8 | strings | Table: offset, count of strings |
| 40 | 8 | vars | Table: offset, count of variables |
| 48 | 8 | fonts | Table: offset, count of fonts |
| 56 | 8 | boxes | Table: offset, count of boxes |
| 64 | 8 | items | Table: offset, count of view-level items |
| 72 | 8 | symbols | Table: offset, count of symbols |
| 80 | 8 | gradients | Table: offset, count of gradients - version 0.2 |
| 88 | 8 | stops | Table: offset, count of gradient stops - version 0.2 |

Each table is `{ uint32 offset; uint32 count; }`, the offset counted from the
start of the file. An empty table has count 0.

A version 0.1 header ends at 80, without the gradient tables. `libtodmv`
always writes the 96-byte header, but marks a view with the oldest version
that has what it uses - 0.5 with `APPEND`, 0.4 with `ICON`, 0.3 with `OPACITY`, 0.2 with gradients, else 0.1: a
reader that knows only 0.1 runs a 0.1 view, it finds every table through
the header and never looks at bytes 80 ... 95.

## Code

Instructions as described in [assembly.md](assembly.md#binary-encoding-summary):
a 4-byte header (opcode, size, varmask, flags), then the operands, each
aligned to its own size (2 or 4 bytes), padded to a multiple of 4. Code
offsets (labels, `entry`, box and item offsets) count 4-byte words, so code
is limited to 65535 words (256 KiB).

| Operand kind | Size | Holds |
|--------------|------|-------|
| 16-bit value (x, y, w, h, r, t, order) | 2 | int16, or a variable index (varmask) |
| 32-bit value (n) | 4 | int32, or a variable index (varmask); `SET` into a string variable: a string index |
| color | 4 | 0xAARRGGBB, or a variable index (varmask), or a gradient index (flags bit 7, `DMV_PAINT_GRADIENT`) |
| string | 2 | String index, or a string variable index (varmask) |
| variable (destination) | 2 | Index of a declared variable |
| label | 2 | Code word offset |
| box | 2 | Box index; 0xFFFF = the current box (`REDRAW` without operand) |
| font | 2 | Font index |
| event | 2 | Event number (`PRESS` = 0 ... `KEY` = 13, in the order of assembly.md) |

Variable indices below 0xFF00 are entries of the variable table, 0xFF00 and
above are the built-in variables (`$box.w` = 0xFF00 ... `$box.focused` =
0xFF06, `$ev.contact` = 0xFF10 ... `$ev.key` = 0xFF1B, `$view.w` = 0xFF20,
`$view.h` = 0xFF21, `$time` = 0xFF22).

## Strings

`count` 32-bit offsets, relative to the start of the table, followed by the
strings themselves, each zero-terminated (UTF-8). Every string the view uses
is stored once: texts, paths, format strings, and the names of the view,
variables, fonts, boxes, labels and dmenv bindings.

## Records

**Variable** (12 bytes)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 1 | type | 0 = integer, 1 = string |
| 1 | 1 | flags | bit 0: bound to dmenv (`env:`) |
| 2 | 2 | capacity | Bytes of a string variable, 0 for an integer |
| 4 | 2 | name | String index of the name, without `$` |
| 6 | 2 | env | String index of the dmenv name, 0xFFFF without binding |
| 8 | 4 | init | Initial integer, or string index of the initial string |

**Font** (4 bytes): name (string index), spec (string index, e.g. `sans-16`).

**Box** (8 bytes): name (string index, without `@`), parent (box index,
0xFFFF for a top-level box), begin (code word offset of its `BOX`), end
(code word offset of its `END`). Every box appears exactly once between its
`BOX` and `END`, so a runtime can redraw it by running `begin`..`end`.

**Item** (8 bytes) - the view-level directives:

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 1 | kind | 0 `.init`, 1 `.timer`, 2 `.key`, 3 `.navkeys` |
| 1 | 1 | arg | `.navkeys`: role (`NEXT` = 0 ... `OK` = 6) |
| 2 | 2 | label | Handler code word offset, 0xFFFF for `.navkeys` |
| 4 | 4 | value | `.timer`: milliseconds, `.key` / `.navkeys`: button index |

**Gradient** (16 bytes)

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 2 | name | String index |
| 2 | 1 | kind | 0 linear, 1 radial |
| 3 | 1 | count | Stops, 2 ... 16 |
| 4 | 2 | first | Index of its first stop in the stop table |
| 6 | 8 | param | 4 x int16 - linear: angle (0 ... 359 degrees, 0 = up, 90 = right), 0, 0, 0; radial: cx, cy, rx, ry in percent of the shape's width and height, rx and ry > 0 |
| 14 | 2 | reserved | 0 |

**Stop** (8 bytes): color (0xAARRGGBB, 4 bytes), position (0 ... 1000, in
1/1000 of the gradient, 2 bytes), reserved (0, 2 bytes). The stops of a
gradient are consecutive, in order.

**Symbol** (4 bytes): name (string index, local labels with their leading
`.`), offset (code word offset). Symbols are in the order the labels were
defined; they are needed only for disassembly and debugging.

## Validation

`libtodmv_validate()` checks a view, reading it in small pieces;
`libtodmv_disassemble()` validates every view before reading it. A valid
file satisfies:

- the header magic and version, `file_size` equal to the file's size;
- every table lies inside the file at a 4-byte aligned offset, every string
  is terminated inside the file;
- every instruction has a known opcode and exactly its opcode's size, the
  last one ends at the end of the code;
- varmask bits only for value operands, flags only with the instruction's
  own flag names (and `DMV_PAINT_GRADIENT` on a drawing instruction with a
  color), every index (string, variable, font, gradient, box, event) in
  range, and variable types matching the instruction; a gradient operand is
  never a variable;
- every gradient's kind and parameters valid, its stops inside the stop
  table, positions 0 ... 1000 and not decreasing;
- every code offset (labels, `entry`, items, boxes, symbols) points at the
  start of an instruction;
- `BOX` / `END` nest, match the box table (begin, end, parent), and `ON`
  only appears inside a box; `OPACITY` directly follows `BOX`, `SCROLL` or
  `FOCUS`.
