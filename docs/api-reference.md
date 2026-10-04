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
