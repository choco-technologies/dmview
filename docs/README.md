# dmview Documentation

dmview defines the UI view format: a `.dmvs` assembly and its binary form
`.dmv` - drawing instructions with parameters, input handlers and the
variables they work on, executed by `dmgui`.

## Contents

- **[assembly.md](assembly.md)** - `.dmvs` assembly: execution model, syntax, instruction set (drawing and input)
- **[api-reference.md](api-reference.md)** - Complete API documentation

## Quick Reference

```c
#include "dmview.h"
```

View documentation using `dmf-man`:

```bash
dmf-man dmview          # Main documentation
dmf-man dmview api      # API reference
```
