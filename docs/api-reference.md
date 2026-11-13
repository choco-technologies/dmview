# libdmview API Reference

`#include "libdmview.h"` - types in `libdmview_types.h`, the format in
`dmview_format.h`. Functions return 0 or a negative errno value unless noted.

## Views

| Function | Description |
|----------|-------------|
| `libdmview_t libdmview_open(const char* path, int* status)` | Load and validate a view; `.init` handlers run. `status`: `-ENOENT`, `-EBADMSG` (not a valid view), `-EIO`, `-ENOMEM` |
| `libdmview_t libdmview_open_input(const dmv_input_t* input, int* status)` | The same from any input (`read` at an offset) |
| `void libdmview_close(libdmview_t view)` | Release a view (NULL is fine) |
| `int libdmview_render(libdmview_t view, const libdmview_surface_t* s, libdmview_rect_t* changed)` | Draw what changed (everything the first time, after `libdmview_invalidate()` or a new surface size). 1 when something was drawn - `changed` is its bounding rectangle -, 0 when nothing changed, `-ENOTSUP` for a pixel format other than RGB565 / ARGB8888 |
| `void libdmview_invalidate(libdmview_t view)` | Draw everything next time |
| `int libdmview_get_size(libdmview_t view, uint16_t* width, uint16_t* height)` | The size the view was designed for (`.size`) - a service turns a portrait view on a landscape display by it |
| `int libdmview_input(libdmview_t view, const dmdrvi_input_state_t* state, uint32_t now_ms)` | Feed the input device's state; changes become events and run their handlers |
| `int libdmview_key(libdmview_t view, uint16_t code, uint32_t codepoint, bool down, uint8_t modifiers)` | A key of a keyboard, for the application of the claim: the events `mods` (modifier bits changed), `char` (a key that gives a character went down: its Unicode value) and `key` (one that gives none: its USB HID usage) |
| `uint32_t libdmview_update(libdmview_t view, uint32_t now_ms)` | Run what is due (`.timer`, `LONG`); returns the ms until the next deadline or `LIBDMVIEW_NO_DEADLINE` |
| `const char* libdmview_take_goto(libdmview_t view)` | The path a `GOTO` asked for, once; NULL when none |
| `int libdmview_get_int(view, name, int32_t* value)` | Read an integer variable (name without `$`) |
| `int libdmview_set_int(view, name, int32_t value)` | Set an integer variable - redraws what depends on it |
| `int libdmview_set_string(view, name, const char* value)` | Set a string variable (truncated to its size) |
| `dmv_status_t libdmview_validate(const dmv_input_t* input, uint32_t* error_offset)` | Check a view without loading it |

```c
typedef struct {
    void*    pixels;        /* top-left pixel */
    uint16_t width, height;
    uint32_t stride;        /* bytes per line */
    uint8_t  format;        /* dmdrvi_gfx_pixel_format_t */
} libdmview_surface_t;
```

Time (`now_ms`) is any monotonic millisecond counter; the first value a
view sees is its time 0 (`$time`, `.timer`).

## Displays and claims

| Function | Description |
|----------|-------------|
| `libdmview_display_t libdmview_display_register(const char* name)` | Register a display (its service); NULL when the name is taken |
| `void libdmview_display_unregister(libdmview_display_t d)` | Unregister; its claims are dropped |
| `int libdmview_display_view(d, uint32_t* generation, char* path, size_t size, size_t* length)` | The newest live claim's view; claims of processes that are gone are dropped. `-ENOENT` without a claim, `-ERANGE` when `path` is too small (NULL to ask for the length) |
| `uint32_t libdmview_display_generation(d)` | Changes whenever the answer of `libdmview_display_view()` may have |
| `int libdmview_claim(const char* display, const char* view_path, uint32_t* claim)` | Show a view on a display (NULL: the first registered) while the calling process runs; `-ENODEV` without such a display |
| `int libdmview_release(uint32_t claim)` | Drop a claim; `-ENOENT` when there is none |

A claim remembers the pid of the calling process - never a callback or a
pointer into it - so a process that ends without releasing leaves nothing
behind.

## The channel of a claim

The view's properties (`.prop`) and events (`EMIT`) - see
[assembly.md](assembly.md#the-channel-of-a-claim).

| Function | Description |
|----------|-------------|
| `int libdmview_prop_set_int(uint32_t claim, const char* name, int32_t value)` | Set an integer property; the view takes it in (and runs its `.change` handlers) on the service's thread. `-ENOENT`: no such claim or property, `-EINVAL`: not an integer |
| `int libdmview_prop_set_string(claim, name, const char* value)` | Set a string property, cut to its size |
| `int libdmview_prop_get_int(claim, name, int32_t* value)` | As the view or the application last set it |
| `int libdmview_prop_get_string(claim, name, char* value, size_t size)` | `-ERANGE`: cut to `size` |
| `int libdmview_wait_event(claim, libdmview_event_t* event, int32_t timeout_ms)` | The next event (`name`, `value`, `dropped` before it); 0: no wait, -1: no limit. `-ETIMEDOUT`, `-ENOENT` (released meanwhile) |
| `int libdmview_attach(libdmview_t view, libdmview_display_t display)` | For the service: connect the view it shows to the display's newest claim; `-ENOENT` without a claim |

## Screen rent

The application that owns a display can rent rectangles of its view to other applications - see [rent.md](rent.md) for how it works.
Rects are in the coordinates of the owner's view.

The owner (its claim, its process):

| Function | Description |
|----------|-------------|
| `int libdmview_rent_create(uint32_t claim, const libdmview_rect_t* rect, uint32_t* rent)` | Rent `rect`. `-ENOENT` (no such claim), `-EPERM` (not the process of the claim), `-EINVAL` (empty rectangle, or the claim is an application's in a rent), `-EEXIST` (overlaps another rent of the claim) |
| `int libdmview_rent_name(uint32_t rent, char* name, size_t size)` | `"<display>/<rent>"`, what the application claims; `-ERANGE`: cut |
| `int libdmview_rent_bind(uint32_t rent, uint32_t pid)` | Only that process may take the rent (`-EACCES` for the others); 0: any |
| `int libdmview_rent_set_rect(uint32_t rent, const libdmview_rect_t* rect)` | Move or resize; the application is told the size (`"rent.size"`) |
| `int libdmview_rent_set_visible(uint32_t rent, bool visible)` | Show or hide (`"rent.visible"`): a hidden rent is not drawn, takes no input and runs no timers |
| `int libdmview_rent_set_focus(uint32_t rent, bool focus)` | The keys go to this rent, and to no other of the claim (`"rent.focus"`) |
| `int libdmview_rent_set_escape(uint32_t rent, uint16_t key)` | The key (USB HID usage) that is the owner's though the rent has the focus; 0: none |
| `int libdmview_rent_release(uint32_t rent)` | Take the rent back; the application's claim gets `-ENOENT` |

The application:

| Function | Description |
|----------|-------------|
| `libdmview_claim(name, view, &claim)` | `name` is the one of the rent instead of a display's: `-EACCES` (bound to another process), `-EBUSY` (taken), `-ENODEV` (no such rent) |
| `int libdmview_rent_get_size(uint32_t claim, uint16_t* width, uint16_t* height)` | The size of its rent; `-ENOENT` when the claim is not in one |

Events of the channel of a claim: the application gets `"rent.size"` (value: `width << 16 | height`), `"rent.visible"` and `"rent.focus"` (0 or
1); the owner gets `"rent.open"` and `"rent.closed"` (value: the id of the rent).

The service of a display:

| Function | Description |
|----------|-------------|
| `int libdmview_display_rents(d, uint32_t* generation, libdmview_rent_info_t* out, uint32_t max, uint32_t* count)` | The rents of the claim on top (the one shown): id, the claim in it, `rect`, `visible`, `focus`, `escape`, the view's `path`. Applications that have gone are dropped here |
| `uint32_t libdmview_display_rent_generation(d)` | Changes whenever `libdmview_display_rents()` may answer another |
| `int libdmview_rent_attach(libdmview_t view, uint32_t claim)` | `libdmview_attach()` for the view of an application in a rent |
| `void libdmview_invalidate_rect(libdmview_t view, const libdmview_rect_t* area)` | Draw `area` on the next `libdmview_render()` |
| `libdmview_rents_t libdmview_rents_create(d)` / `libdmview_rents_destroy(r)` | The composition of a display's rents |
| `bool libdmview_rents_sync(r, libdmview_rect_t* vacated)` | Open the views of the applications that came, close those that went; `vacated` is what the owner draws again |
| `uint32_t libdmview_rents_update(r, now_ms)` | `libdmview_update()` of the shown rents; the ms until the next deadline |
| `void libdmview_rents_damage(r, const libdmview_rect_t* area)` | The owner drew over `area`: the rents draw again what is theirs of it |
| `int libdmview_rents_render(r, const libdmview_surface_t* surface, libdmview_rect_t* changed)` | Draw what changed in the shown rents, each into its rectangle |
| `bool libdmview_rents_input(r, dmdrvi_input_state_t* state, uint32_t now_ms)` | Route the contacts that began in a rent to its view (relative coordinates) and take them out of `state` |
| `bool libdmview_rents_key(r, code, codepoint, down, modifiers)` | A key for the rent with the focus; false: it is the owner's |
