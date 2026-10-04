# dmview

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmview/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmview/actions/workflows/ci.yml)

Views of the dmod GUI stack: the view format and the runtime that shows
views on displays.

## Description

A view is a small program - drawing instructions with fixed binary
parameters, input handlers and variables - in a text assembly (`.dmvs`,
[docs/assembly.md](docs/assembly.md)) and its binary form (`.dmv`,
[docs/binary-format.md](docs/binary-format.md)). HTML/CSS (`dmhtml`,
`dmcss`) compiles to `.dmvs`, which `libtodmv` / `todmv` (in
[todmv](https://github.com/choco-technologies/todmv)) assemble into `.dmv`.
This repository executes `.dmv` files:

| Module | Kind | What it does |
|--------|------|--------------|
| `libdmview` | Library | Loads a view, draws it into a framebuffer, runs its input handlers; the display registry |
| `dmview` | Service ([apps/dmview](apps/dmview)) | One instance per display: shows the right view on it |

`include/dmview_format.h` is the format itself (opcodes, operand layouts,
file structures), header only - libtodmv can use it as well.

## The display service

dmdevfs reports every node that answers `DMDRVI_IOCTL_GFX_GET_INFO` to
libsystemd as a `display`, and [configs/dmview.rules](configs/dmview.rules)
starts `dmview@<name>` ([configs/dmview@.ini](configs/dmview@.ini)) for it -
whatever the display's driver. The service:

1. opens the display node and its framebuffer;
2. takes its input from the member of the display's `friends_group` that is
   an input device with contacts of the display's resolution
   (`DMDRVI_IOCTL_DEVFS_GET_FRIEND`, `DMDRVI_IOCTL_INPUT_GET_INFO`);
3. shows, in this order:
   - the view the newest running application claimed the display for
     (`libdmview_claim()`, see below),
   - `$DMVIEW_VIEWS/<display name>.dmv`, if that file exists,
   - `$DMVIEW_DEFAULT`;
4. waits for input (at most 100 ms, or until the next timer), runs the
   events and draws what changed, then presents it
   (`DMDRVI_IOCTL_GFX_PRESENT` with the area drawn). On a double buffered
   display the frame is drawn off the screen and shown whole at the next
   vertical blank - no drawing is ever visible; the driver copies the area
   into the other buffer, so only what changed is ever drawn. A driver
   without `PRESENT` gets the drawing flushed.

The display's name is its node name - the section name of its
configuration: `[lcd]` is `/dev/lcd`, served by `dmview@lcd`. With two
displays, name them after their purpose (`[main]`, `[status]`) and put
`main.dmv` and `status.dmv` into `$DMVIEW_VIEWS`.

In a firmware's `flash.dmd`:

```
dmview service=dmview@.ini
dmview rules=dmview.rules
```

and in the board configuration, the display and its touch panel in one
`friends_group`.

### Applications on a display

An application shows its own view while it runs:

```c
#include "libdmview.h"

uint32_t claim;
libdmview_claim(NULL, "/app/ui.dmv", &claim);   /* NULL: the first display */
...
libdmview_release(claim);
```

The service draws the view and handles its input; the application talks to
it through the view's `SIGNAL` (a dmhaman handler it registered) and `env:`
variables. Claims stack: the newest one is shown, a release - or the
application ending without one, noticed through its pid - returns the
display to the previous claim and finally to the service's own view.

## libdmview

```c
#include "libdmview.h"

int status;
libdmview_t view = libdmview_open("/flash/views/main.dmv", &status);

libdmview_surface_t s = { framebuffer, 480, 272, 480 * 2, DMDRVI_GFX_PIXEL_FORMAT_RGB565 };
for (;;)
{
    /* input state from DMDRVI_IOCTL_INPUT_WAIT_EVENT / read() of the input node */
    libdmview_input(view, &state, now_ms);
    uint32_t next = libdmview_update(view, now_ms);      /* timers, long press */
    libdmview_rect_t changed;
    if (libdmview_render(view, &s, &changed) > 0)
        /* present `changed` - DMDRVI_IOCTL_GFX_PRESENT - and draw into
           the buffer DMDRVI_IOCTL_GFX_GET_FRAMEBUFFER gives next */;
}
libdmview_close(view);
```

Several views - on several displays, in several services - run at once:
everything of a view lives in its `libdmview_t`.

### Speed

Drawing is what a view does all the time, so:

- the view is validated once, when it is loaded, and its code and tables
  are kept in memory - execution checks nothing and reads every operand at
  a constant offset (`src/layout.h`, checked against the format by the
  tests);
- only what changed is drawn: while a box draws, every variable it reads is
  recorded; a change redraws exactly the boxes that read the variable - an
  opaque box (`OPAQUE`) alone, a transparent one together with what is
  beneath it, clipped to its area;
- drawing goes straight into the framebuffer, as horizontal spans: RGB565
  stores two pixels at a time, only colors with alpha below 0xFF are
  blended, curves use an integer square root per sub-row and are
  antialiased - only the pixels their edges cross (one or two per side and
  row) are blended by coverage, the inside stays plain spans;
- a gradient becomes a 256-color palette in the screen's pixel format once:
  a linear one then costs an addition and a lookup per pixel (a vertical
  one is one color - on RGB565 one 4-pixel dither pattern - per line), a
  radial one takes its square root from a table; on RGB565 opaque gradients
  are dithered, without bands.

### What is implemented

Runtime level 1 of [docs/assembly.md](docs/assembly.md#runtime-capabilities):
every instruction except `IMAGE` and `RELOAD` (no images yet), one contact
(`PRESS`, `DRAG`, `LONG`, `RELEASE`, `CLICK`), `.init`, `.timer`, `.key`,
`env:` variables, `GOTO`, `SIGNAL`, `EXEC`, `SCROLLTO`, translucent boxes
(`OPACITY`). Text: antialiased,
proportional fonts from `.dmvf` files in `$DMVIEW_FONTS` - Roboto
`sans-N` / `sans-bold-N` in [fonts/](fonts), more made with
[tools/ttf2dmvf.py](tools/ttf2dmvf.py) - UTF-8; the built-in 8x8 font
(public domain font8x8, `"builtin-N"`) for consoles and when a font file is
missing ([docs/assembly.md](docs/assembly.md#fonts)). Linear and radial gradients paint any shape and text
([docs/assembly.md](docs/assembly.md#gradients)). Not yet: images, scrolling by dragging, focus, several
contacts and gestures. Pixel formats: RGB565, ARGB8888.

## Building

```bash
cmake -S . -B build -DDMOD_TOOLS_NAME=arch/x86_64
cmake --build build
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout,
and e.g. `-DDMOD_TOOLS_NAME=arch/armv7/cortex-m7` for a target.

## Testing

```bash
cd build
ctest --output-on-failure
```

[tests/run_tests.sh](tests/run_tests.sh) installs `todmv`, assembles the
views of `tests/fixtures/` and runs `test_libdmview` through `dmod_loader`;
the tests draw them into framebuffers in memory and check pixels, what is
redrawn, events, timers, `env:` variables and the claims.

## Documentation

- **[docs/assembly.md](docs/assembly.md)** - the `.dmvs` language and instruction set
- **[docs/binary-format.md](docs/binary-format.md)** - the `.dmv` file
- **[docs/api-reference.md](docs/api-reference.md)** - libdmview

## Project Structure

```
dmview/
├── include/
│   ├── dmview_format.h     # The format: opcodes, layouts, file structures
│   ├── libdmview.h         # libdmview API
│   └── libdmview_types.h
├── src/                    # libdmview: loading, interpreter, rasterizer, font, input, claims
├── fonts/                  # Roboto as .dmvf font files (Apache 2.0)
├── tools/ttf2dmvf.py       # TrueType -> .dmvf
├── tests/                  # libdmview host tests and their views
├── apps/dmview/            # The display service
├── configs/                # dmview@.ini, dmview.rules
├── docs/
├── CMakeLists.txt
├── libdmview.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
