# dmview Documentation

dmview defines the UI view format - a `.dmvs` assembly and its binary form
`.dmv`: drawing instructions with parameters, input handlers and the
variables they work on - and executes views: `libdmview` and the `dmview`
display service.

## Contents

- **[assembly.md](assembly.md)** - `.dmvs` assembly: execution model, syntax, instruction set (drawing and input)
- **[binary-format.md](binary-format.md)** - `.dmv` file layout: header, code, tables, validation
- **[font-format.md](font-format.md)** - `.dmvf` font files: antialiased glyphs, made by `tools/ttf2dmvf.py`
- **[image-format.md](image-format.md)** - `.dmvi` image files: raw pixels and masks, made by todmvi
- **[api-reference.md](api-reference.md)** - libdmview: views, displays and claims

## Quick Reference

```c
#include "libdmview.h"

libdmview_t view = libdmview_open("/flash/views/main.dmv", NULL);
libdmview_render(view, &surface, NULL);
```

View documentation using `dmf-man`:

```bash
dmf-man libdmview          # Main documentation
dmf-man libdmview api      # API reference
```
