# dmview

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmview/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmview/actions/workflows/ci.yml)

UI view format of the dmod GUI stack: drawing and input-handling programs in
a text assembly (`.dmvs`) and its binary form (`.dmv`).

## Description

A view is a small program - drawing instructions with fixed binary
parameters, input handlers and variables - that `dmgui` executes directly,
without parsing at run time. HTML/CSS (`dmhtml`, `dmcss`) compiles to
`.dmvs`, `todmv` assembles it into `.dmv`.

The instruction set is described in [docs/assembly.md](docs/assembly.md)
(draft).

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmview-local.dmd -y
dmod_loader build/dmf/test_dmview.dmf
```

## Usage

<TBD>

This library module provides functions that can be used by other modules:

```c
#include "dmview.h"
```

## API

| Function | Description |
|----------|-------------|
| `dmview_create()` | Create a new `dmview_t` instance. |
| `dmview_destroy()` | Destroy an instance created by `_create()`. |
| `dmview_is_valid()` | Check whether a handle is a valid instance. |

See [include/dmview.h](include/dmview.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmview`.
## Project Structure

```
dmview/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmview.h
├── src/
│   └── dmview.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmview_test.c
├── CMakeLists.txt
├── Makefile
├── dmview.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
