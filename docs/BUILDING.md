# Building ps3recomp

Complete guide to building the ps3recomp runtime library and game projects.

---

## Table of Contents

1. [Prerequisites](#prerequisites)
2. [Building the Runtime Library](#building-the-runtime-library)
3. [Build Options](#build-options)
4. [Graphics Backends](#graphics-backends)
5. [Linux Notes](#linux-notes)
6. [Compiler Support](#compiler-support)
7. [CMake Build System Details](#cmake-build-system-details)
8. [Linking Against the Runtime](#linking-against-the-runtime)
9. [Building a Game Project](#building-a-game-project)
10. [Troubleshooting](#troubleshooting)

---

## Prerequisites

### Required

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| **CMake** | 3.20 | Build system generator |
| **C Compiler** | C17 support | Compile runtime and HLE modules |
| **C++ Compiler** | C++20 support | Compile templates, module registration |
| **Python** | 3.9+ | Recompiler tools |

### Recommended

| Tool | Purpose |
|------|---------|
| **Ninja** | Faster builds than Make (`pip install ninja`) |
| **ccache** | Compiler cache for faster rebuilds |

### Linux requirements

- GCC 12+ or Clang 15+
- Development headers: pthreads, `libm`
- SDL2 development package (`libsdl2-dev`) for input, audio and the Vulkan
  window
- Optional: Vulkan headers (`libvulkan-dev`) and glslang for the Vulkan backend

---

## Building the Runtime Library

### Quick Start

```bash
# Clone
git clone https://github.com/sp00nznet/ps3recomp.git
cd ps3recomp

# Configure and build
cmake -B build -G Ninja
cmake --build build
```

### Step by Step

#### 1. Configure

```bash
# Using Ninja (recommended)
cmake -B build -G Ninja

# Using Make
cmake -B build -G "Unix Makefiles"

# With specific build type
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
```

#### 2. Build

```bash
cmake --build build

# Parallel build (if not using Ninja which is parallel by default)
cmake --build build -j$(nproc)

# Build specific target
cmake --build build --target ps3recomp_runtime
```

#### 3. Verify

```bash
# Check that the library was built
ls build/libps3recomp_runtime.a
```

#### 4. Install (Optional)

```bash
cmake --install build --prefix /usr/local
# Installs:
#   /usr/local/lib/libps3recomp_runtime.a
#   /usr/local/include/ps3emu/*.h
```

---

## Build Options

| CMake Option | Default | Description |
|---|---|---|
| `PS3RECOMP_BUILD_TESTS` | `OFF` | Build and register runtime tests |
| `PS3RECOMP_RSX_VULKAN` | `OFF` | Linux: build the Vulkan RSX backend (early -- see [Graphics Backends](#graphics-backends)) |
| `PS3RECOMP_FFMPEG_RUNTIME` | `OFF` | Decode video (cellVdec: H.264, MPEG-2) through FFmpeg loaded at run time -- see [FFmpeg](#ffmpeg-run-time-loaded-opt-in) |
| `CMAKE_BUILD_TYPE` | — | `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | Installation directory |

### FFmpeg (run-time loaded, opt-in)

```bash
# Headers only -- Debian/Ubuntu: libavcodec-dev, Fedora: ffmpeg-free-devel
cmake -S . -B build -G Ninja -DPS3RECOMP_FFMPEG_RUNTIME=ON
```

FFmpeg is never linked. The first `cellVdecOpen` loads libavcodec and
libavutil with `dlopen` / `LoadLibrary` and resolves the few functions the
decoder calls. The library has to be the major version the headers describe
(`libavcodec.so.63` / `avcodec-63.dll` for FFmpeg 9.0), because the decoder
reads `AVFrame` fields and FFmpeg keeps their layout only within a major
version; another major is reported and not used. Ship the matching FFmpeg shared
libraries with the game, or rely on the system's. When the library is missing,
or `PS3RECOMP_FFMPEG=0` is set, the decoders keep the callback-only behaviour
every build had before. `PS3RECOMP_FFMPEG_AVCODEC` and
`PS3RECOMP_FFMPEG_AVUTIL` name the library files to load instead.

FFmpeg stays a separate library supplied by the user; nothing from it is
compiled into the runtime. If you redistribute FFmpeg libraries with a port,
use an LGPL-only build: distro packages are usually built with GPL parts
enabled (`--enable-gpl`), which changes what you may ship.

### Running the Tests

```bash
cmake -S . -B build-tests -G Ninja -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

The test build registers the deterministic synchronization stress suite, the
fiber switch test and cellVdec's guest-API test. With
`-DPS3RECOMP_FFMPEG_RUNTIME=ON` and an `ffmpeg` program on `PATH`, it also
generates small H.264 and MPEG-2 streams into the build tree and checks every
decoded picture against FFmpeg's own decode. It can still be configured directly from `tests/sync_stress` when
testing an alternate runtime tree with `PS3RECOMP_TREE_ROOT`.

### Checking the PPU boot scaffold

`runtime/ppu/` is the one part of the runtime no CMake target builds. Those
files `#include "ppu_recomp.h"`, which the lifter generates per game, so
`CMakeLists.txt` excludes them from the library and they are only ever
compiled inside somebody's game port.

```bash
python tools/check_ppu_scaffold.py            # check against the baseline
python tools/check_ppu_scaffold.py --verbose  # show the diagnostics
python tools/check_ppu_scaffold.py --update   # re-record after improving it
```

It synthesises a stub `ppu_recomp.h` from the lifter's own `HEADER_PREAMBLE`
— imported from `ppu_lifter.py`, not copied, so it cannot drift from what
games really get — and compiles the scaffold against it. It does not link and
runs nothing; it proves only that every declaration the scaffold reaches for
exists.

It is a **ratchet** rather than a pass/fail gate:
`tools/ppu_scaffold_baseline.json` records the current error count per
toolchain, and the check fails only if a number goes **up**. A toolchain with
no baseline entry reports its numbers and passes, so a new platform's first CI
run tells you what to record.

Every recorded toolchain is now at **zero** — the whole scaffold compiles under
GCC and Clang — so in practice this is a hard gate: any new error fails the
build.

Compiling is not the same as running. The scaffold is not linked or executed by
anything in this repository (that needs a lifted game). `boot_main.cpp`
starts its vblank ticker through the pthread `CreateThread` shim in
`win32_compat.h`. What remains between "builds" and "runs a title" is booting
one.

It compiles the four scaffold translation units. `boot_main.cpp` is excluded:
it also pulls in the per-game SPU recomp header and the generated function
table, which a stub cannot stand in for.

### Compile Definitions

| Definition | Default | Description |
|---|---|---|
| `PS3_MODULE_MAX_FUNCS` | `512` | Maximum functions per HLE module NID table |
| `PS3_MODULE_MAX_VARS` | `64` | Maximum variables per HLE module NID table |

Override at build time:

```bash
cmake -B build -DPS3_MODULE_MAX_FUNCS=1024
```

---

## Graphics Backends

Everything under `libs/video/` is compiled into `ps3recomp_runtime`. There are
two backends:

| Backend | Source | Status |
|---------|--------|--------|
| null / headless software | `libs/video/rsx_null_backend.c` | The default. The guest command stream is executed and validated, and triangles are filled in host memory. Nothing is presented to a window; this is the CI and fallback path. |
| Vulkan (opt-in) | `libs/video/rsx_vulkan_backend.c` | Built with `-DPS3RECOMP_RSX_VULKAN=ON`. With guest programs on (`PS3RECOMP_VK_GUEST_PROGRAMS=1`) it runs the title's shaders on the shared register-file draw engine; without them, the null backend's fixed-function contract. See below. |

### Exercising the backend without a game

The build also produces `ps3recomp_host`, which drives the
`cellGcm` -> RSX -> backend bridge with no lifted game at all. It is how a
backend gets brought up and regression-tested before any title exists:

```bash
cmake --build build --target ps3recomp_host
./build/ps3recomp_host
```

### The Vulkan backend (opt-in)

```bash
# Headers only -- Debian/Ubuntu: libvulkan-dev, Fedora: vulkan-headers
cmake -S . -B build -G Ninja -DPS3RECOMP_RSX_VULKAN=ON
cmake --build build --target ps3recomp_host
./build/ps3recomp_host --draw
```

`libvulkan` is loaded at run time, so building needs only the headers, which
also keeps cross-compiling for aarch64 simple (point
`-DPS3RECOMP_VULKAN_INCLUDE_DIR=` at them if the cross sysroot has none). Only
Vulkan 1.0 entry points are used; it has been run on Mesa's lavapipe and on
NVIDIA's L4T driver (Vulkan 1.2, Tegra X1).

| Environment variable | Effect |
|---|---|
| `PS3RECOMP_VK_DEVICE=<n>` | Use physical device *n* instead of the first non-CPU one |
| `PS3RECOMP_VK_DUMP=<path>` | Write each presented frame to *path* as a binary PPM |
| `PS3RECOMP_VK_DUMP_EVERY=<n>` | With a dump: write only every *n*th presented frame (default 1). A full-frame write per present slows a running title noticeably |
| `PS3RECOMP_VK_WINDOW=1` | Also show frames in an SDL2 window; if it cannot, the run continues headless and says why |
| `PS3RECOMP_VK_FULLSCREEN=1` | With a window: borderless full screen |
| `PS3RECOMP_VK_HOLD=<sec>` | With a window: keep the last frame up for that long before exiting |
| `PS3RECOMP_VK_GUEST_PROGRAMS=1` | Run the guest's own vertex/fragment programs (needs the translator, below); also makes the draw engine the default path |
| `PS3RECOMP_RSX_ENGINE=dispatch\|vtable` | Pick the draw path explicitly -- the engine's own switch |
| `RSX_ENGINE_COHERENCE_LOG=<n>` | Log the first *n* syncs between guest memory and the engine's surfaces around NV3089 transfers, with how many non-zero pixels moved |
| `RSX_ENGINE_PASS_DUMP=<dir>` | Capture draws pass by pass into an existing *dir*: each pass's colour targets as PPM, its zeta as PGM, and one line per pass in `passes.txt` (targets, depth range, clears, the first draw's state and textures). Starts after `RSX_ENGINE_PASS_DUMP_AFTER=<sec>` or on the frame after one of `RSX_ENGINE_PASS_DUMP_MINDRAWS=<n>` guest draws, and stops after `RSX_ENGINE_PASS_DUMP_MAX=<n>` passes (default 64). `RSX_ENGINE_PASS_DUMP_DRAWS=1` adds one line per draw to `draws.txt`. Every pass stalls the GPU for its readbacks |
| `RSX_ENGINE_SHADER_DUMP=<dir>` | Write each new pipeline's translated programs to an existing *dir* as `pipe<handle>.vs.hlsl` and `.ps.hlsl`, the handles `passes.txt` names |

Without a GPU, Mesa's lavapipe runs every check, e.g.
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./build/ps3recomp_host --tex`
(the ICD file name varies by distribution). Without guest programs, the modes
that need them (`--shader`, `--mip`, `--rtt`, `--depthtex`, `--mrt`,
`--mrt-a`, `--cube`) report that and are skipped, as on the null backend.
`--depthbounds` checks the depth bounds test (`SET_DEPTH_BOUNDS_TEST_ENABLE`,
`_MIN`, `_MAX`), which only the draw engine implements, and only on a device
with the `depthBounds` feature; run it with `PS3RECOMP_VK_GUEST_PROGRAMS=1`.
Anywhere else (the fixed-function path, the null backend, or a device without
the feature, such as lavapipe in Mesa 25) it reports
`skipped, no depth bounds test` and exits 0; the Vulkan workflow turns that
into a notice.

#### Guest programs and the draw engine

The guest's programs go through the shared decompilers to HLSL, then through
glslang to SPIR-V (`libs/video/rsx_shader_spirv.cpp`), so they need glslang's
CMake package at build time; CMake reports whether it found it
(`Vulkan: guest programs available`). On Debian/Ubuntu that is `glslang-dev`
plus `spirv-tools`. glslang must be built **with** SPIRV-Tools: the HLSL path
runs its legalization passes, and a glslang without them emits different --
possibly invalid -- SPIR-V, silently. That matters when cross-compiling glslang
yourself: its `update_glslang_sources.py` fetches the SPIRV-Tools revision it
expects.

```bash
PS3RECOMP_VK_GUEST_PROGRAMS=1 ./build/ps3recomp_host --rtt
```

With guest programs on, the backend runs on the register-file draw engine
(`rsx_draw_engine.h`): the engine owns surfaces,
render-to-texture, MRT, depth textures, cube maps, vertex compaction and pipeline keys,
and every `ps3recomp_host` scene passes. Not handled yet, and logged once when
a title reaches them: vertex-texture units, colour targets other
than RGBA8 and RGBA16F (the FP16 HDR target), depth-only passes, stencil on the vtable path, and depth snapshots on devices
whose depth format is neither `D32_SFLOAT` nor `D32_SFLOAT_S8_UINT`. Everything is submitted synchronously for now:
correct first, fast later.

### The RSX code that is *not* in the library

`runtime/ppu/` and `runtime/host/` are deliberately excluded from
`ps3recomp_runtime` (see the `list(FILTER ...)` in the root `CMakeLists.txt`).
They `#include` the lifter-generated `ppu_recomp.h`, which only exists once you
have lifted a specific title, so they are compiled **into your game target**
rather than into the shared library. `templates/project/CMakeLists.txt` shows the arrangement.

---

## Linux Notes

**Compiler flags (GCC/Clang):**
- `-Wall -Wextra` — Enable warnings
- `-Wno-unused-parameter` — Suppress for HLE stubs
- `-fno-strict-aliasing` — Required for `be_t<T>` endian conversion through unions

**Link libraries:**
- `-lpthread` — POSIX threads (linked automatically by most CMake configurations)
- `-lm` — Math library
- SDL2 — input, audio and the optional Vulkan window

---

## Compiler Support

### Tested Compilers

| Compiler | Version | Status |
|----------|---------|--------|
| GCC | 12+ | Full support |
| Clang | 15+ | Full support |

### Required C17 Features

- `_Static_assert` / `static_assert`
- Designated initializers
- `__attribute__((aligned))` (GCC/Clang) — used for VMX register alignment

### Required C++20 Features

- `std::is_enum_v`, `std::underlying_type_t` (type traits)
- `constexpr` if
- Designated initializers
- `thread_local` variables
- Lambdas in `constexpr` context

---

## CMake Build System Details

### Source Collection

The CMake build uses `GLOB_RECURSE` to automatically include all `.c` files:

```cmake
# HLE modules — all .c files under libs/
file(GLOB_RECURSE HLE_SOURCES
    "libs/system/*.c"
    "libs/filesystem/*.c"
    "libs/input/*.c"
    "libs/audio/*.c"
    "libs/video/*.c"
    "libs/network/*.c"
    "libs/spurs/*.c"
    "libs/sync/*.c"
    "libs/codec/*.c"
    "libs/font/*.c"
    "libs/misc/*.c"
    "libs/hardware/*.c"
)

# Runtime core
file(GLOB_RECURSE RUNTIME_SOURCES
    "runtime/*.c"
    "runtime/*.cpp"
)
```

**Adding a new module:** Just create a `.c` file in the appropriate `libs/` subdirectory. CMake will pick it up automatically on the next configure.

**Adding a new category:** Add a new `GLOB_RECURSE` pattern for `libs/newcategory/*.c`.

### Include Directories

```cmake
# Public headers (available to downstream projects)
target_include_directories(ps3recomp_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# Private headers (for internal HLE module use)
target_include_directories(ps3recomp_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/system"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem"
    # ... all libs/ subdirs ...
)
```

### Library Target

The build produces a single **static library**: `ps3recomp_runtime`.

This library contains:
- All HLE module implementations (77+ modules)
- Runtime core (memory, threading, syscalls)
- Module registration framework
- NID lookup tables

---

## Linking Against the Runtime

### From a Game Project

```cmake
# In your game's CMakeLists.txt

# Option 1: Add ps3recomp as a subdirectory
add_subdirectory(path/to/ps3recomp)
target_link_libraries(my_game PRIVATE ps3recomp_runtime)

# Option 2: Find an installed ps3recomp
find_library(PS3RECOMP_LIB ps3recomp_runtime PATHS /usr/local/lib)
find_path(PS3RECOMP_INCLUDE ps3emu/ps3types.h PATHS /usr/local/include)
target_link_libraries(my_game PRIVATE ${PS3RECOMP_LIB})
target_include_directories(my_game PRIVATE ${PS3RECOMP_INCLUDE})

# Option 3: Specify path directly
target_link_libraries(my_game PRIVATE
    /path/to/ps3recomp/build/libps3recomp_runtime.a
)
target_include_directories(my_game PRIVATE
    /path/to/ps3recomp/include
)
```

### Required Link Dependencies

`ps3recomp_runtime` links pthreads and libm itself. Your game project needs
SDL2 and, when the Vulkan backend is enabled, the Vulkan loader at run time:

```cmake
find_package(SDL2 REQUIRED)
target_link_libraries(my_game PRIVATE
    ps3recomp_runtime SDL2::SDL2 pthread m
)
```

**Vendored dependencies** (in-tree, no external install needed):
- `stb_image.h` v2.30 — PNG/JPG/GIF decoding for cellPngDec/cellJpgDec/cellGifDec
- `stb_truetype.h` v1.26 — TTF font rendering for cellFont

Both are single-header public domain libraries from [nothings/stb](https://github.com/nothings/stb).

---

## Building a Game Project

### Using the Template

```bash
# Copy the template
cp -r ps3recomp/templates/project/ my_game/

# Edit configuration
# - config.toml: set elf_path, module settings, graphics/audio backends
# - CMakeLists.txt: set project name, add ps3recomp path
# - stubs.cpp: add game-specific function overrides

# Run the recompiler (generates C source files)
python ps3recomp/tools/ppu_lifter.py disasm/ --output my_game/recompiled/

# Build
cd my_game
cmake -B build -G Ninja -DPS3RECOMP_DIR=/path/to/ps3recomp
cmake --build build

# Run
./build/my_game
```

### Template Files

| File | Purpose |
|------|---------|
| `main.cpp` | Entry point — initializes runtime subsystems, loads function table, creates main PPU thread |
| `stubs.cpp` | Game-specific NID overrides and custom function implementations |
| `config.toml` | Recompiler and runtime configuration |
| `CMakeLists.txt` | Build configuration — links against ps3recomp_runtime |

### Configuration Reference (config.toml)

See `templates/project/config.toml` for the full reference with comments. Key sections:

```toml
[input]
elf_path = "EBOOT.ELF"

[output]
output_dir = "recompiled/"
func_prefix = "recomp_"

[modules]
cellSysutil = "hle"    # Use HLE stub
cellGcmSys = "hle"
# Set to "lle" to recompile the actual PRX module code

[memory]
main_ram_mb = 256
vram_mb = 256

[threading]
max_ppu_threads = 64
max_spu_threads = 6

[graphics]
backend = "vulkan"      # "vulkan", "null"

[audio]
backend = "sdl"         # "sdl", "null"

[debug]
log_hle_calls = true
log_missing_nids = true
break_on_unimplemented = false
```

---

## Troubleshooting

### "fatal error: 'ps3emu/ps3types.h' file not found"

The include path is not set. Make sure your CMakeLists.txt includes:
```cmake
target_include_directories(my_target PRIVATE /path/to/ps3recomp/include)
```

### "undefined reference to `vm_base`"

`vm_base` is declared `extern` in `vm.h` but must be defined in exactly one translation unit. The runtime's `runtime/memory/vm.c` defines it. Make sure you're linking `ps3recomp_runtime`.

### "undefined reference to `g_ps3_module_registry`"

Same issue — link against `ps3recomp_runtime`.

### C: "'be_t': undeclared identifier"

You're compiling a `.c` file that includes `ps3types.h`. The `be_t<T>` template is C++ only. Either:
- Rename the file to `.cpp`
- Only use C-compatible types (`u32`, `u64`, etc.) in C files

### GCC/Clang: "warning: 'u128_t' attribute ignored [-Wattributes]"

The `__attribute__((aligned(16)))` on `u128` in a struct may warn if the alignment exceeds the struct's natural alignment. This is safe to ignore.

### "cannot find -lSDL2"

Install the SDL2 development package:
- Debian/Ubuntu: `sudo apt install libsdl2-dev`
- Fedora: `sudo dnf install SDL2-devel`
- Arch: `sudo pacman -S sdl2`

### Build takes too long

- Use Ninja (`-G Ninja`) instead of Make for better parallelism
- Use ccache: `cmake -B build -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
- Use a Release build type (fewer debug symbols): `-DCMAKE_BUILD_TYPE=Release`
