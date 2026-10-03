# dmview Assembly (`.dmvs`)

Status: **draft 0.1** - the instruction set is open for changes until the
first release of the binary format.

A dmview view is a small program: drawing instructions with their parameters,
input handlers and the variables they work on. It exists in two forms:

| Form | Extension | Used for |
|------|-----------|----------|
| Assembly | `.dmvs` | Text, written by hand or generated (`dmhtml` + `dmcss`), read in tests and by the disassembler |
| Binary | `.dmv` | What `libdmview` executes - opcode + fixed binary parameters, no parsing at run time |

Both forms map 1:1: every line of assembly is one binary instruction, and a
`.dmv` disassembles back into equivalent assembly.

```
.html + .css ──(dmhtml, dmcss)──► .dmvs ──(libtodmv / todmv)──► .dmv ──► libdmview ──► GFX / INPUT
```

### Modules

Every step is its own dmf module, so it runs on a PC and on the device, and
a firmware contains only what it needs:

| Module | Kind | Role |
|--------|------|------|
| `dmview_format.h` | Header (in `libdmview`) | The format itself: opcodes, operand layouts, file structures - shared by everything below, header only |
| `libtodmv` | Library | Assembler and disassembler as an API working on streams, never on whole files: `.dmvs` read line by line -> `.dmv` written as it is assembled, every error with line and column; `.dmv` -> `.dmvs`; validation of a `.dmv` |
| `todmv` | Application | Small command-line tool on top of `libtodmv`: `todmv view.dmvs -o view.dmv`, `todmv -d view.dmv` to disassemble |
| `libdmview` | Library | Runtime: loads a `.dmv`, draws it into a framebuffer, turns `DMDRVI_IOCTL_INPUT_*` state into events; the display registry for application claims |
| `dmview` | Service | One instance per display (`dmview@<name>`, started by a device rule): opens the display and its touch input, shows the right view |
| `dmhtml`, `dmcss` | Libraries | HTML / CSS front end; produce `.dmvs` and hand it to `libtodmv` line by line through its source stream, no intermediate file needed |

Like `libsystemd` and `systemd` in dmsystem, the library and the program
share a repository each: `libdmview` and the `dmview` service live here
(`libdmview` at the root, `apps/dmview`), `libtodmv` and `todmv` in
[todmv](https://github.com/choco-technologies/todmv); a firmware that only
shows prebuilt views needs neither of the latter.

This document defines the assembly language and the instruction set. The
exact binary layout (file header, section tables) is defined in
[binary-format.md](binary-format.md); the
[encoding summary](#binary-encoding-summary) below fixes what the instruction
set depends on.

## Execution model

### Boxes

The screen is built from **boxes** - rectangles that correspond to HTML
elements. `BOX` opens a box, `END` closes it; boxes nest.

Inside a box:

- coordinates are relative to the box's top-left corner,
- drawing is clipped to the box (and to all its parents),
- the box's size is readable as `$box.w` / `$box.h`.

Every box has an identifier (`@name`). The assembler collects all boxes into a
box table, so `libdmview` knows each box's code, bounds and parent without running
the program first.

### Drawing is stateless

Drawing instructions carry all their parameters - position, size, color,
font. There is no "current color" or "current font". The only state is the
one `BOX` sets up (origin, clip, box variables). Because of that, **any box
can be redrawn on its own**: `libdmview` re-runs the code between its `BOX` and
`END` with the box's origin and clip, and nothing else is needed.

### Draw pass

When a view is shown, `libdmview` runs the code from the entry label (`.entry`)
until its top-level `RET` and draws the whole screen. After that, only
invalidated boxes are drawn again:

- a box is invalidated when a variable it **read** during its last draw
  changes (this includes `$box.pressed` - pressing a button redraws it),
- or explicitly with `REDRAW`.

Writes to variables during a draw pass never invalidate anything: a draw must
depend only on the state, temporary results (e.g. `FORMAT` into a buffer) are
allowed.

A box that is jumped over is neither drawn nor hit-tested - this is how
elements are shown and hidden. A box whose position or size comes from a
variable invalidates both its old and its new area.

**Opaque boxes.** Redrawing a box that does not paint all of its area itself
needs what lies beneath it. `BOX ... OPAQUE` promises that the box covers its
whole area with opaque pixels (typically `FILL` or a `RECT` over it) - not
a button drawn as an `RRECT`: its corners, and the antialiased pixels along
them, show what lies beneath. For a box that is not
opaque, `libdmview` redraws its parents up to the nearest opaque one, clipped to
the invalidated area. The view's root is always treated as opaque.

#### Translucent boxes

`OPACITY a` makes a box translucent, like CSS `opacity`:

```
        BOX     @toast, 40, 200, 400, 40
        OPACITY $toast_alpha            ; fade in and out with a .timer
        RRECT   0, 0, $box.w, $box.h, 8, #303A48
        TEXT    0, 0, $box.w, $box.h, "Saved", body, #FFFFFF, CENTER|MIDDLE
        END
```

- Everything drawn in the box - colors, gradients, text, its boxes - has its
  alpha multiplied by `a / 255`. A box inside a translucent box multiplies
  its own `OPACITY` with it.
- What the box draws is blended with what lies beneath it shape by shape,
  without an offscreen buffer: where shapes of the box overlap, the lower
  one shows through the upper one (CSS composes the box first). For the
  usual fades, disabled controls and overlays this looks the same and costs
  no memory.
- `a` can be a variable: changing it redraws the box. A translucent box and
  everything in it is never opaque - a redraw starts beneath it, so the
  blend never adds up. `OPAQUE` on such a box is ignored.
- `OPACITY 0` draws nothing; the box is still there for input.

### Scrolling

`SCROLL cw, ch [, flags]` right after `BOX` makes the box a viewport onto
content of `cw` x `ch` pixels. The box's children are drawn shifted by the
scroll offset, which `libdmview` keeps per box:

- **Dragging** the content scrolls it - `libdmview` handles that itself, no
  handler needed. A press on a child inside starts as usual; once the contact
  moves more than the scroll threshold (`.scrollslop`, default 8 px), the
  scroll box takes the contact over and the child gets `RELEASE` without
  `CLICK`. `WHEEL` over the box scrolls it as well.
- `SCROLLTO @id, x, y` sets the offset from a handler (clamped to the
  content), `$box.sx` / `$box.sy` read it.
- **Why an instruction**: `libdmview` knows the box is a viewport, so scrolling
  needs no full redraw - it moves the pixels already on the screen and draws
  only the strip that became visible, and skips children whose bounds (from
  the box table) are outside the visible area.

The content must not depend on its own box's scroll offset - a scroll
indicator is drawn by `libdmview` with the `BAR` flag. To show something
elsewhere based on the offset (e.g. a "back to top" button), a handler of
the scroll box copies `$box.sy` into a variable that other boxes read.

### Input

A box becomes interactive with `ON event, label` inside it. The handler runs
**in the context of that box**: `$box.*` refers to it and `$ev.x` / `$ev.y`
are relative to its top-left corner.

The language assumes **several contacts at once** (fingers on a touch panel,
the mouse pointer) and **focus navigation** from the start. A runtime may
implement less - follow only one contact, compute no gestures, have no
focus - it then simply never sends the corresponding events, and every view
stays valid. See [runtime capabilities](#runtime-capabilities).

#### Contacts

Every contact is followed on its own and identified by `$ev.contact` (the
contact ID the input device reports; the mouse pointer and focus activation
have their own IDs, see below).

- **Hit test** - when a contact touches, its target is the topmost box under
  it (the last one drawn) that has a handler for the event. A box without
  one passes it to its parent (bubbling, as in the DOM).
- **Capture** - the box that received a contact's `PRESS` receives that
  contact's `DRAG`, `LONG`, `RELEASE` and `CLICK` until it is lifted, even
  outside its bounds. Two fingers on two sliders move both, independently.
- **One or many per box** - a box takes one contact at a time: a further
  contact landing on a box that already holds one is passed to its parent.
  A box with the `MULTI` flag takes any number of contacts; `$box.contacts`
  says how many it holds.
- **Gestures** - while a `MULTI` box holds two or more contacts, it also
  receives `PINCH` and `ROTATE`, computed from the first two of them
  relative to where they were when the second one touched. A box that wants
  the gestures and not the single contacts handles only those events.
- **Scroll boxes** are `MULTI` implicitly: one contact scrolls, two contacts
  scroll as well (two-finger scroll), `PINCH` is available to their handlers.

#### Focus

For devices without a pointer - a few buttons, a rotary encoder, a keyboard
- one box at a time has the **focus**, and keys move it and activate it.

- `FOCUS order` right after `BOX` makes the box focusable. Boxes are visited
  in increasing `order`, equal orders in drawing order (`FOCUS 0` everywhere
  = drawing order, like `tabindex="0"`).
- `.navkeys` maps device buttons to navigation: `NEXT`/`PREV` move through
  the focus order, `UP`/`DOWN`/`LEFT`/`RIGHT` move to the nearest focusable
  box in that direction (a runtime without spatial navigation treats them as
  `PREV`/`NEXT`), `OK` activates the focused box.
- **Activation** sends the focused box `PRESS` when `OK` goes down and
  `RELEASE` + `CLICK` when it goes up, with `$ev.contact` = `FOCUS_CONTACT`
  and `$ev.x`/`$ev.y` at the box's center - a button written for touch works
  with keys unchanged.
- `$box.focused` is 1 for the focused box (redrawn when it changes, like
  `$box.pressed` - this is CSS `:focus`); `FOCUS` / `BLUR` events go to the
  box that gains / loses the focus; `SETFOCUS @id` moves it from a handler.
- A key that is not a navigation key goes to the focused box as `KEY`
  (`$ev.key`), bubbling to its parents; `.key` handlers of the view get the
  keys nobody handled.
- Scroll boxes follow the focus: moving the focus into a child outside the
  visible area scrolls it into view.

#### Events

| Event | When | Event variables |
|-------|------|-----------------|
| `PRESS` | A contact touches the box (mouse: a button goes down; focus: `OK` goes down) | `$ev.contact`, `$ev.x`, `$ev.y`, `$ev.button` |
| `DRAG` | A captured contact moves | `$ev.contact`, `$ev.x`, `$ev.y`, `$ev.dx`, `$ev.dy` |
| `LONG` | A captured contact stays down for the long-press time (`.longpress`, default 600 ms) | `$ev.contact`, `$ev.x`, `$ev.y` |
| `RELEASE` | A captured contact is lifted, wherever it is | `$ev.contact`, `$ev.x`, `$ev.y`, `$ev.button` |
| `CLICK` | A captured contact is lifted inside the box, after `RELEASE` | `$ev.contact`, `$ev.x`, `$ev.y`, `$ev.button` |
| `PINCH` | Two contacts of a `MULTI` box move apart or together | `$ev.scale`, `$ev.cx`, `$ev.cy` |
| `ROTATE` | Two contacts of a `MULTI` box turn around each other | `$ev.angle`, `$ev.cx`, `$ev.cy` |
| `ENTER` / `LEAVE` | A mouse pointer enters / leaves the box (no button down) | `$ev.x`, `$ev.y` |
| `WHEEL` | Wheel steps over the box | `$ev.wheel` |
| `SCROLLED` | The box's scroll offset changed (drag, wheel, `SCROLLTO`, focus) | `$box.sx`, `$box.sy` |
| `FOCUS` / `BLUR` | The box gains / loses the focus | - |
| `KEY` | A non-navigation key goes down while the box (or a child) has the focus | `$ev.key` |

`PRESS`, `DRAG`, `RELEASE` and `CLICK` work the same for a touch panel, a
mouse and focus activation; `ENTER`, `LEAVE` and `WHEEL` only come from
devices that report motion or a wheel (`DMDRVI_INPUT_CAP_MOTION` / `_WHEEL`).

**Handlers** run until `RET`. They change variables and perform actions;
drawing instructions are not allowed in a handler (the assembler rejects
them when it can prove it, the runtime ignores them). When the handler
returns, `libdmview` redraws what became invalid. Handlers that belong to the
view rather than to a box are declared with directives: `.init` (when the
view is shown), `.timer` (periodically), `.key` (a key nobody handled). They
run in the context of the root box.

#### Runtime capabilities

What a runtime implements is a superset of these levels; a view written for
a higher level works on a lower one, with the events of the higher level
never arriving:

| Level | Adds |
|-------|------|
| 1 | One contact (further contacts are ignored until it is lifted), `PRESS` ... `CLICK`, `ENTER`/`LEAVE`, `WHEEL`, scrolling with one contact |
| 2 | Independent contacts, `MULTI`, `$box.contacts` |
| 3 | `PINCH`, `ROTATE`, two-finger scroll |
| F | Focus: `FOCUS`, `.navkeys`, activation, `FOCUS`/`BLUR`/`KEY` events, `SETFOCUS` |

The first `libdmview` implements level 1; F and 2-3 come later without changing
the format.

## Syntax

```
; comment - to the end of the line
.directive  operand, operand        ; directives start with '.'
label:                              ; global label
.local:                             ; local label, scoped to the last global label
            MNEMONIC operand, operand, ...
```

- One instruction or directive per line. Mnemonics, event names and flags are
  case-insensitive; labels, variables and box names are case-sensitive.
- Operands are separated by commas.
- A label is `[A-Za-z_][A-Za-z0-9_]*` followed by `:`; a local label starts
  with `.` and is unique only within its global label.
- A whole file is one view. `.include` inserts another file (e.g. a library
  of widget subroutines).

### Operand syntax

| Syntax | Meaning | Examples |
|--------|---------|----------|
| integer | Decimal, `0x` hexadecimal, `0b` binary, optional `-` | `16`, `-4`, `0x1F` |
| color | `#RRGGBB` (opaque) or `#AARRGGBB`, the same 0xAARRGGBB as `DMDRVI_IOCTL_GFX_FILL_RECT` | `#3D85F5`, `#803D85F5` |
| string | `"..."` with `\n`, `\t`, `\"`, `\\`, `\xHH` | `"Count: %d"` |
| variable | `$name`, or a built-in `$box.w`, `$ev.x`, ... | `$count` |
| label | A label name, local labels with their `.` | `inc`, `.up` |
| box | `@name` | `@plus` |
| font | Name declared with `.font` | `body` |
| flags | Names joined with `\|` | `CENTER\|MIDDLE` |
| constant | Name declared with `.define`, usable wherever a number or color is | `BTN_H`, `BLUE` |
| gradient | Name declared with `.gradient`, usable wherever a drawing instruction takes a color | `sky` |

Where an operand is marked **value** below, it can be an immediate or a
variable - `RECT 0, 0, $width, 8, #3D85F5` takes its width from `$width`.

## Directives

| Directive | Meaning |
|-----------|---------|
| `.view name` | Name of the view (required, once) |
| `.size w, h` | Size the view was designed for; `libdmview` refuses a smaller screen |
| `.entry label` | Start of the draw pass (required, once) |
| `.var $name, int, init [, env:NAME]` | 32-bit signed integer variable |
| `.var $name, str[N], "init" [, env:NAME]` | String variable holding up to N bytes |
| `.font name, "spec"` | Font used by `TEXT`; `spec` is resolved by `libdmview` (e.g. `"sans-16"`) - see [Fonts](#fonts) |
| `.gradient name, LINEAR [, angle], stop, stop [, ...]` | Linear gradient - see [Gradients](#gradients) |
| `.gradient name, RADIAL [, cx, cy, rx, ry], stop, stop [, ...]` | Radial gradient - see [Gradients](#gradients) |
| `.define NAME, value` | Assembly-time constant |
| `.include "file.dmvs"` | Insert another assembly file |
| `.init label` | Handler run once when the view is shown, before the first draw |
| `.timer ms, label` | Handler run every `ms` milliseconds while the view is shown |
| `.key button, label` | Handler run when button `button` (bit of `dmdrvi_input_state_t::buttons`) goes down and no box handled it as `KEY` |
| `.navkeys role, button [, role, button ...]` | Map device buttons to focus navigation; roles `NEXT`, `PREV`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `OK` |
| `.longpress ms` | Long-press time of `LONG` (default 600) |
| `.scrollslop px` | Movement after which a scroll box takes a contact over (default 8) |

`env:NAME` binds a variable to the dmenv variable `NAME`:

- it is read when the view is shown and written back on every change, so the
  rest of the system sees it;
- a change made by anyone else (a C module, a dmell `set`) reaches the view
  through a dmenv listener: `libdmview` registers for `NAME` when the view is
  shown, and the change invalidates the boxes that read the variable, like
  any other change. See [dmenv change listener](#dmenv-change-listener).

### Built-in variables

Read-only, always available:

| Variable | Meaning |
|----------|---------|
| `$box.w`, `$box.h` | Size of the current box |
| `$box.pressed` | 1 while the current box holds a captured contact, else 0 |
| `$box.sx`, `$box.sy` | Scroll offset of the current box (0 without `SCROLL`) |
| `$box.contacts` | Number of contacts the current box holds |
| `$box.focused` | 1 while the current box has the focus, else 0 |
| `$ev.contact` | Contact of the event: the input device's contact ID (0...), `POINTER_CONTACT` for the mouse, `FOCUS_CONTACT` for focus activation |
| `$ev.x`, `$ev.y` | Event position, relative to the handler's box |
| `$ev.dx`, `$ev.dy` | Motion since the previous `DRAG` |
| `$ev.wheel` | Wheel steps of `WHEEL` |
| `$ev.scale` | `PINCH`: distance of the two contacts relative to the gesture's start, in 1/1000 (1000 = unchanged, 2000 = twice as far apart) |
| `$ev.angle` | `ROTATE`: rotation since the gesture's start, in 1/10 degree, clockwise positive |
| `$ev.cx`, `$ev.cy` | Center between the two contacts of a gesture, relative to the box |
| `$ev.key` | Key code of `KEY` (button index of `dmdrvi_input_state_t::buttons` for now) |
| `$ev.button` | Mouse/device button of the event (`DMDRVI_INPUT_BUTTON_*` bit) |
| `$view.w`, `$view.h` | Size of the screen |
| `$time` | Milliseconds since the view was shown - never a draw dependency, use `.timer` to animate |

Event variables are 0 outside a handler. `POINTER_CONTACT` and
`FOCUS_CONTACT` are predefined constants (-1 and -2).

## Instruction set

Operand types: **x/y/w/h/r/t** - value, 16-bit signed (pixels); **color** -
value, 32-bit 0xAARRGGBB; **str** - string literal or string variable;
**n** - value, 32-bit signed; **$d** - a writable variable.

### Structure and flow

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x00 | `NOP` | - | Nothing |
| 0x01 | `BOX` | `@id, x, y, w, h [, flags]` | Open a box at x/y of the current one. Flags: `OPAQUE`, `MULTI` (takes several contacts, receives gestures) |
| 0x02 | `END` | - | Close the innermost box |
| 0x03 | `JMP` | `label` | Jump |
| 0x04 | `CALL` | `label` | Call a subroutine (stack of 8) |
| 0x05 | `RET` | - | Return; at the top level it ends the pass or the handler |
| 0x06 | `JEQ` | `a, b, label` | Jump if a == b (a, b: n) |
| 0x07 | `JNE` | `a, b, label` | Jump if a != b |
| 0x08 | `JLT` | `a, b, label` | Jump if a < b |
| 0x09 | `JLE` | `a, b, label` | Jump if a <= b |
| 0x0A | `JGT` | `a, b, label` | Jump if a > b |
| 0x0B | `JGE` | `a, b, label` | Jump if a >= b |
| 0x0C | `SCROLL` | `cw, ch [, flags]` | Right after `BOX`: the box shows a scrollable area of cw x ch. Flags: `HORIZONTAL`, `VERTICAL` (default: both where the content is larger), `BAR` (draw a scroll indicator) |
| 0x0D | `FOCUS` | `order` | Right after `BOX` (and `SCROLL`): the box is focusable, visited in increasing `order` |
| 0x0E | `OPACITY` | `a` | Right after `BOX` (and `SCROLL`, `FOCUS`): the box and everything in it are drawn with opacity `a`, 0 (invisible) ... 255 (opaque) - see [Translucent boxes](#translucent-boxes) |

A subroutine called inside a box draws in that box, so widgets are reusable:
`CALL button_bg` draws whatever background the current box is.

### Drawing

Coordinates are relative to the current box, everything is clipped to it.
Colors with alpha below 0xFF are blended. Curves (`RRECT`, `RFRAME`,
`CIRCLE`, `RING`) are antialiased: the pixels their edges cross get the part
of them the shape covers as alpha. Straight edges at whole pixels stay
sharp; `LINE` is not antialiased yet.

### Fonts

`TEXT` draws in the font its `.font` names. `libdmview` resolves the spec
when the view is shown:

| Spec | Font |
|------|------|
| `"builtin-N"` | The built-in 8x8 font, magnified N / 8 times (`builtin-8`, `builtin-16`) - fixed width, never a file: a console's font |
| has a `/` | The font file at that path |
| anything else | `$DMVIEW_FONTS/<spec>.dmvf` |

Font files ([font-format.md](font-format.md)) hold antialiased glyphs -
proportional, any Unicode character of the BMP the file has, text is UTF-8.
dmview ships Roboto as `sans-12` ... `sans-32` and `sans-bold-12` ...
`sans-bold-32`; `tools/ttf2dmvf.py` makes further sizes and fonts from
TrueType files. When the file is missing or is not a valid font, the
built-in font is used, magnified by the spec's size / 8 - a view always
shows its text. A file is loaded into memory once and shared by every view
that uses it.

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x10 | `FILL` | `color` | Fill the whole current box |
| 0x11 | `RECT` | `x, y, w, h, color` | Filled rectangle |
| 0x12 | `RRECT` | `x, y, w, h, r, color` | Filled rectangle with corner radius r |
| 0x13 | `FRAME` | `x, y, w, h, t, color` | Rectangle outline, t pixels thick, drawn inside x/y/w/h |
| 0x14 | `RFRAME` | `x, y, w, h, r, t, color` | Rounded rectangle outline |
| 0x15 | `LINE` | `x1, y1, x2, y2, t, color` | Line t pixels thick |
| 0x16 | `CIRCLE` | `x, y, r, color` | Filled circle centered at x/y |
| 0x17 | `RING` | `x, y, r, t, color` | Circle outline |
| 0x18 | `TEXT` | `x, y, w, h, str, font, color, align` | Text laid out in the rectangle x/y/w/h |
| 0x19 | `IMAGE` | `x, y, w, h, str, align` | Image from the file `str` (path, literal or string variable) placed in the rectangle x/y/w/h - see [Images](#images) |

`TEXT` alignment flags: horizontal `LEFT` (default), `CENTER`, `RIGHT`;
vertical `TOP` (default), `MIDDLE`, `BOTTOM`; `WRAP` breaks lines at spaces
to fit `w`. Text that does not fit is clipped.

Every `color` operand of the drawing instructions can be a gradient instead
- `RRECT 0, 0, $box.w, $box.h, 8, sky`.

### Gradients

A gradient is declared once and painted by name wherever a drawing
instruction takes a color. Its geometry is relative to the shape it paints,
so one gradient fits buttons of every size:

| Instruction | The gradient spans |
|-------------|--------------------|
| `FILL` | The whole current box (also when only a part of it is redrawn) |
| `RECT`, `RRECT`, `FRAME`, `RFRAME`, `TEXT` | Its `x, y, w, h` |
| `CIRCLE`, `RING` | The square around the circle |
| `LINE` | The line's bounding box, its thickness included |

```
.gradient sky,  LINEAR, #3D85F5, #2A6FDB            ; top to bottom
.gradient bar,  LINEAR, 90, #FF5F6D, #FFC371 70     ; left to right
.gradient glow, RADIAL, #FFFFFF, #00FFFFFF          ; white center fading out
.gradient spot, RADIAL, 30, 30, 80, 80, #FFFFFF 0, #3D85F5 40, #101820

        FILL    sky
        RRECT   0, 0, $box.w, $box.h, 8, bar
        CIRCLE  40, 40, 24, glow
        TEXT    0, 0, 200, 32, "Title", title, bar, CENTER|MIDDLE
```

- **`LINEAR [, angle]`** - the angle in degrees points where the gradient goes,
  as in CSS: 0 up, 90 right, 180 down (default), 270 left, anything between.
  The gradient line is as long as the shape's corners need (`|w sin| +
  |h cos|`), so both end colors reach the corners.
- **`RADIAL [, cx, cy, rx, ry]`** - an ellipse around `cx, cy` with radii
  `rx, ry`, in percent of the shape's width and height (default `50, 50, 50,
  50`: the ellipse inscribed in the shape; `50, 50, 71, 71` reaches its
  corners). Outside the ellipse the last color.
- **Stops** - 2 to 16 of `color [position]`, the position in percent
  (`50`, `50%`, `33.3`), not decreasing. Left-out positions follow CSS: the
  first stop at 0, the last at 100, the others evenly between their
  neighbors. Two stops at the same position make a hard edge. Before the
  first stop the first color, after the last one the last color.
- Colors with alpha are blended over what is beneath, per pixel; a gradient
  of opaque colors only is written without blending - on an RGB565 screen
  with ordered dithering (4x4 Bayer), so a gradient between close colors
  does not show as bands of RGB565's 32 / 64 levels.
- A gradient is a constant: an operand names it, never a variable. Choose
  between gradients with a jump, as `button_bg` in the example does with
  colors.

`libdmview` turns a gradient into a palette of 256 colors, in the screen's
pixel format, when it is first drawn. Drawing then costs one addition and a
palette lookup per pixel for a linear gradient - a vertical one is a single
color per line, as fast as a plain color - and a table lookup for the
square root of a radial one.

### Variables

Integer arithmetic is 32-bit signed and wraps around. `$d` must be a
variable declared with `.var`; built-in variables are read-only.

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x40 | `SET` | `$d, n` or `$d, str` | d = n; for a string variable copies the string (truncated to its size) |
| 0x41 | `ADD` | `$d, n` | d = d + n |
| 0x42 | `SUB` | `$d, n` | d = d - n |
| 0x43 | `MUL` | `$d, n` | d = d * n |
| 0x44 | `DIV` | `$d, n` | d = d / n, rounded toward zero; d = 0 when n = 0 |
| 0x45 | `MOD` | `$d, n` | d = d % n; d = 0 when n = 0 |
| 0x46 | `MIN` | `$d, n` | d = min(d, n) |
| 0x47 | `MAX` | `$d, n` | d = max(d, n) |
| 0x48 | `CLAMP` | `$d, lo, hi` | d = min(max(d, lo), hi) |
| 0x49 | `TOGGLE` | `$d` | d = (d == 0) ? 1 : 0 |
| 0x4A | `FORMAT` | `$d, str, n` | Format n into string variable d: `str` with one `%d`, `%x` or `%%` |

A variable is "changed" only when its value differs from the previous one -
`SET $v, 5` on a `$v` that already holds 5 invalidates nothing.

### Input

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x60 | `ON` | `event, label` | Make the current box receive `event` and run `label` for it |

`ON` is valid only inside a box. Several `ON` for different events may follow
each other; one box has at most one handler per event.

### Actions

| Opcode | Mnemonic | Operands | Description |
|--------|----------|----------|-------------|
| 0x80 | `REDRAW` | `[@id]` | Invalidate a box (default: the current one) |
| 0x81 | `EXEC` | `str` | Run a dmell command line, e.g. `"ifconfig eth0 up"`, without waiting for it |
| 0x82 | `SIGNAL` | `str` | Call the dmhaman handler with this name - lets a C module react to the UI |
| 0x83 | `GOTO` | `str` | Show another view (path of a `.dmv`) after the current handler returns |
| 0x84 | `SCROLLTO` | `@id, x, y` | Set the scroll offset of a scroll box (clamped to its content) |
| 0x85 | `RELOAD` | `str` | Load the image file `str` again (it changed on disk) and redraw where it is shown |
| 0x86 | `SETFOCUS` | `@id` | Move the focus to a focusable box |

Opcodes 0xF0-0xFF are reserved for extensions (e.g. `dmjs` script calls).

## Images

An image is never part of an instruction. `IMAGE` carries only a rectangle
and the **path** of the file; the path is a string like any other - a literal
in the string table or a string variable:

```
        IMAGE   8, 8, 64, 64, "/flash/icons/wifi.dmvi", CENTER|MIDDLE
        IMAGE   0, 40, 480, 200, $photo, CENTER|MIDDLE     ; dynamic source
```

- **The rectangle is fixed in the instruction**, so the layout never depends
  on the image: nothing moves when it arrives. The image is placed in the
  rectangle by the alignment flags (`LEFT`/`CENTER`/`RIGHT`,
  `TOP`/`MIDDLE`/`BOTTOM`) and clipped to it; scaling is left for later.
- **Loading is asynchronous.** The first time a path is drawn, `libdmview` starts
  loading it in the background and draws nothing in its place; when the
  image is ready, the boxes that show it are invalidated and redrawn. A
  slow file (an SD card, the network) never blocks drawing or input. A file
  that cannot be loaded or decoded stays empty and is logged.
- **Dynamic sources**: with a string variable as the source, setting the
  variable to another path loads the new image and redraws - from a
  handler, or from outside through an `env:` binding. `RELOAD` reads a file
  again when its content changed under the same path (e.g. a camera
  snapshot).
- **Memory**: decoded images are kept (in SDRAM where there is one) per path
  as long as a shown view uses them, and shared between boxes.

### Where the file comes from

`libdmview` opens images only through the dmod file system (`Dmod_FileOpen`),
so the source is whatever is mounted: `/flash`, an SD card, a ramfs. An image
from a **link** therefore needs no support in `libdmview` itself - it needs a
file system that maps URLs to paths, e.g. a dmfsi module mounted at `/http`
that fetches `/http/example.com/cam.jpg` from the network. Every other module
gets network files the same way. (Alternatively a downloader module stores
the file in a ramfs and sets the variable with its path.)

### Formats

- **Built-in: raw images** (`.dmvi`) - a small header (width, height,
  `dmdrvi_gfx_pixel_format_t`, stride) followed by the pixels. `libdmview` copies
  them as they are, with no decoding, so this is the fastest format. For
  images known at compile time (literal paths), `todmv` can convert the
  source files (PNG, BMP, ...) into `.dmvi` files next to the `.dmv` - they
  stay separate files, not embedded in the view.
- **Other formats are plugins**: separate dmf modules implementing an image
  decoder DIF. `libdmview` asks the loaded decoders in turn
  (`Dmod_GetNextDifModule()`) which of them recognizes the file, and lets it
  decode the image into the framebuffer's pixel format. The firmware
  contains the decoders the user puts into it (e.g. `dmbmp`, `dmpng`) - none,
  one or several. A file no loaded decoder knows stays empty and is logged.

The decoder DIF (draft):

| Function | Meaning |
|----------|---------|
| `_probe(const void* head, size_t size)` | True if the decoder recognizes the file from its first bytes |
| `_get_info(file, uint16_t* width, uint16_t* height)` | Size of the image |
| `_decode(file, void* dst, uint32_t stride, dmdrvi_gfx_pixel_format_t format)` | Decode into a buffer in the given pixel format |

The DIF belongs to a small interface module (like dmdrvi for drivers or dmfsi
for file systems), so decoders and `libdmview` depend only on it, not on each
other.

## dmenv change listener

dmenv itself needs an addition for `env:` variables (draft, to be added to
dmenv):

- a **DIF** that a listener module implements, called when a variable it
  listens to changes, e.g. `_changed(dmenv_ctx_t ctx, const char* name, const char* value)`;
- a function to **register** a listener by the listening **module's name**
  and the variable name (or a prefix, like `dmenv_find()`), and its
  counterpart to unregister.

dmenv stores the module name, not a callback pointer. On a change it looks
the module up (`Dmod_GetModuleContext()`) and calls its DIF implementation
only if the module is still loaded - a module unloaded without
unregistering is simply skipped, there is no dangling callback. The call
happens in the context of whoever changed the variable, so a listener only
records the change; `libdmview` marks the variable and redraws on its own
thread.

## Example

A counter button, a slider and a label bound to the system's IP address:

```
.view   demo
.size   480, 272
.entry  draw

.font   title, "sans-24"
.font   body,  "sans-16"

.define BLUE,      #3D85F5
.define BLUE_DOWN, #2A6FDB

.var    $count, int, 0
.var    $level, int, 50
.var    $ip,    str[16], "-", env:ETH0_IP
.var    $label, str[24], ""
.var    $fill,  int, 0

draw:
        FILL    #101820
        TEXT    16, 12, 448, 32, "dmview demo", title, #FFFFFF, LEFT|MIDDLE

        BOX     @counter, 16, 60, 160, 48
        ON      CLICK, increment
        CALL    button_bg
        FORMAT  $label, "Count: %d", $count
        TEXT    0, 0, $box.w, $box.h, $label, body, #FFFFFF, CENTER|MIDDLE
        END

        BOX     @slider, 16, 130, 300, 32
        ON      PRESS, slide
        ON      DRAG,  slide
        RRECT   0, 12, $box.w, 8, 4, #303A48
        SET     $fill, $level           ; temporary: fill = level * width / 100
        MUL     $fill, $box.w
        DIV     $fill, 100
        RRECT   0, 12, $fill, 8, 4, BLUE
        END

        BOX     @ip, 16, 190, 448, 32, OPAQUE
        FILL    #101820
        TEXT    0, 0, $box.w, $box.h, $ip, body, #A0A8B0, LEFT|MIDDLE
        END
        RET

; Background of a button-like box, darker while pressed
button_bg:
        JNE     $box.pressed, 0, .down
        RRECT   0, 0, $box.w, $box.h, 8, BLUE
        RET
.down:
        RRECT   0, 0, $box.w, $box.h, 8, BLUE_DOWN
        RET

increment:
        ADD     $count, 1
        RET

slide:
        SET     $level, $ev.x
        MUL     $level, 100
        DIV     $level, $box.w
        CLAMP   $level, 0, 100
        RET
```

What happens at run time:

- Pressing `@counter` changes `$box.pressed`, which `button_bg` read, so only
  the button is redrawn (darker); releasing it inside runs `increment`, and
  the new `$count` redraws it again with the new text.
- Touching or dragging `@slider` sets `$level`; only the slider is redrawn,
  together with the background beneath it (it is not opaque, so its parent -
  the root - is redrawn within the slider's area).
- When `ETH0_IP` is reported through `$ip`, only `@ip` is redrawn.

## Binary encoding summary

Little-endian. Every instruction is a 4-byte header followed by its operands:

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | opcode | Instruction (tables above) |
| 1 | size | Total size of the instruction in bytes, a multiple of 4 |
| 2 | varmask | Bit n set: operand n holds a variable index instead of an immediate |
| 3 | flags | Instruction flags (`BOX` flags, `TEXT` alignment, ...); bit 7 on a drawing instruction: its color operand is a gradient index |

- Operands follow in the order of the assembly operands: 16-bit values
  (coordinates, sizes, variable/string/font/image/box indices, labels) take 2
  bytes, colors and `n` values take 4 bytes; the instruction is padded with
  zeros to a multiple of 4. Each opcode has one fixed layout, so `libdmview`
  reads parameters without decoding anything.
- A variable operand stores the variable's index in the operand's own slot;
  built-in variables use indices 0xFF00-0xFFFF.
- Labels are code offsets in 4-byte words (code up to 256 KiB).
- `size` lets an interpreter skip an instruction it does not know.
- Strings, fonts, gradients, variables, boxes and view-level handlers live in
  tables next to the code; their layout is described in
  [binary-format.md](binary-format.md).

## Open questions

- **Fonts**: kerning, and fonts beyond the BMP.
- **Embedded resources**: should small icons optionally be embedded in the
  `.dmv` (a resource section), so a simple view is one file?
- **Image scaling** (`object-fit: contain/cover`) in `IMAGE`.
- **Key codes**: `$ev.key` is a button index for now; a keyboard (text
  input) needs key codes and characters - an addition to the dmdrvi input
  state.
