# dmview Binary Format (`.dmv`)

Status: **version 0.1** - follows the [assembly draft](assembly.md).

A `.dmv` file is what `libtodmv` produces from `.dmvs` assembly and what
`libdmview` executes. Everything is **little-endian**; every table starts at a
4-byte aligned offset, so 16- and 32-bit fields can be read in place.

```
┌──────────────┐ 0
│ header       │ 80 bytes
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
└──────────────┘ file_size
```

The order of the tables after the header is the one `libtodmv` writes; a
reader finds them only through the header, never by position.

## Header

| Offset | Size | Field | Meaning |
|--------|------|-------|---------|
| 0 | 4 | magic | `'D' 'M' 'V' 0` |
| 4 | 2 | version_major | 0 - a reader rejects any other |
| 6 | 2 | version_minor | 1 - a reader accepts this or lower |
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

Each table is `{ uint32 offset; uint32 count; }`, the offset counted from the
start of the file. An empty table has count 0.

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
| color | 4 | 0xAARRGGBB, or a variable index (varmask) |
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
  own flag names, every index (string, variable, font, box, event) in range,
  and variable types matching the instruction;
- every code offset (labels, `entry`, items, boxes, symbols) points at the
  start of an instruction;
- `BOX` / `END` nest, match the box table (begin, end, parent), and `ON`
  only appears inside a box.
