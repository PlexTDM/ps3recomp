# RSX Graphics Translation

How ps3recomp translates PS3 RSX GPU commands to host graphics APIs.

---

## Architecture Overview

The RSX graphics pipeline has three layers, each in its own source file:

```
Game code (recompiled C)
    │
    ▼
cellGcmSys HLE                    ← libs/video/cellGcmSys.c (33+ functions)
  │ State management: display buffers, IO mapping, flip/VBlank,
  │ tile/zcull config, command buffer control (put/get/ref),
  │ local memory allocator, offset tables, timestamps
  │
  ▼
RSX Command Processor             ← libs/video/rsx_commands.c
  │ Parses NV47xx FIFO command buffer
  │ Tracks all GPU state with dirty flags
  │ Dispatches to registered backend via callbacks
  │
  ├──▶ Null Backend               ← libs/video/rsx_null_backend.c
  │    Headless CPU rasteriser into host memory (CI and fallback)
  │
  └──▶ Vulkan Backend             ← libs/video/rsx_vulkan_backend.c
       Real GPU rendering (opt-in; PS3RECOMP_RSX_VULKAN)
       Guest shaders on the shared draw engine

Supporting files:
  libs/video/rsx_primitives.h      ← RSX → host topology mapping + expansion
  libs/video/rsx_vertex_formats.h  ← RSX vertex attribute decoding
  libs/video/rsx_shader_spirv.cpp  ← HLSL → SPIR-V through glslang
  libs/video/rsx_draw_engine.c     ← shared register-file draw engine
  libs/video/cellResc.c            ← Resolution scaling (16 functions)
  libs/video/cellVideoOut.c        ← Video output configuration
```

---

## Layer 1: cellGcmSys (HLE Module)

**File:** `libs/video/cellGcmSys.c` (~800 lines, 33+ functions)

The game interacts with the GPU through `cellGcmSys` — the PS3 SDK's RSX interface. Our HLE implementation provides:

### Initialization
- `cellGcmInit(cmdSize, ioSize, ioAddress)` — Sets up the RSX configuration struct (local memory address/size, IO region, clock frequencies), initializes the offset translation tables, and creates the command buffer control registers.
- `cellGcmGetConfiguration(config)` — Returns the `CellGcmConfig` struct with local/IO memory addresses and sizes. Games read this to know where GPU memory starts.
- `cellGcmGetControlRegister()` — Returns a pointer to the `CellGcmControl` struct (`put`, `get`, `ref` registers). The game writes the `put` pointer to advance the command buffer. **Important:** This must return a guest-accessible address (allocated in guest VM), not a host pointer.

### Display Buffers
- `cellGcmSetDisplayBuffer(id, offset, pitch, width, height)` — Registers a framebuffer region. Games typically register 2 buffers for double-buffering.
- `cellGcmSetFlipMode(mode)` — VSYNC or HSYNC.
- `cellGcmSetFlipHandler(handler)` / `cellGcmSetVBlankHandler(handler)` — Callback registration. In recomp, these store guest function pointers but don't fire automatically (no real VBlank interrupt).
- `cellGcmResetFlipStatus()` / `cellGcmGetFlipStatus()` — Flip state tracking.
- `cellGcmGetLastFlipTime()` — Returns a host timestamp from `clock_gettime`.

### Memory Management
- `cellGcmMapMainMemory(ea, size, offset)` — Maps main memory for GPU access. Uses a bump allocator with 1MB alignment and tracks mappings in an offset table.
- `cellGcmMapEaIoAddress(ea, io, size)` — Explicit EA→IO mapping with overlap detection.
- `cellGcmAddressToOffset(address, offset)` — Translates effective address to RSX-relative offset using the populated offset tables.
- `cellGcmGetOffsetTable(table)` — Returns pointers to the ioAddress/eaAddress translation arrays.

### Tile and Zcull
- `cellGcmSetTile(index, ...)` — Configures one of 15 tile regions with compression parameters.
- `cellGcmSetZcull(index, ...)` — Configures one of 8 zcull (hierarchical depth) regions.
- `cellGcmBindTile(index)` / `cellGcmUnbindTile(index)` — Activate/deactivate tile regions.
- `cellGcmSetTileInfo(...)` — Alternative tile configuration (same as SetTile, needed by Tokyo Jungle).

### Labels and Reports
- `cellGcmGetLabelAddress(index)` — Returns a guest pointer to one of 256 label slots (4 bytes each). **Must be in guest VM memory** for the game to read/write via vm_read/vm_write.
- `cellGcmGetReportDataAddress(index)` — Returns pointer to report data (16 bytes: timestamp + value).
- `cellGcmGetNotifyDataAddress(index)` — Notify data area (256 slots, used by Tokyo Jungle).

### Reference
- Based on RPCS3's `rpcs3/Emu/RSX/rsx_methods.cpp` and `cellGcmSys.cpp`
- PS3 SDK documentation: `cell/target/ppu/include/cell/gcm/` headers

---

## Layer 2: RSX Command Processor

**File:** `libs/video/rsx_commands.c` (~400 lines) + `rsx_commands.h` (~300 lines)

### How It Works

On the real PS3, the game writes NV47xx GPU method commands to a FIFO ring buffer. The RSX hardware reads these commands and executes them. In ps3recomp, we intercept the command buffer after the game writes to it and process the methods ourselves.

### Command Buffer Format (NV47xx FIFO)

Each command is a 32-bit header followed by N data words:

```
Header: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
```

| Type | Name | Behavior |
|------|------|----------|
| 0 | Increasing | Writes data to method, method+4, method+8... |
| 2 | Non-increasing | Writes all data to the same method address |
| 1 | Jump | Changes command buffer read position |
| 3 | Call/Return | Subroutine call/return |

### State Tracking

The `rsx_state` struct tracks ALL GPU state that methods modify:

| Category | Fields | Method Range |
|----------|--------|--------------|
| **Surfaces** | color/depth offsets, pitches, format, clip rect, color target | 0x200-0x23C |
| **Viewport** | x, y, width, height, depth range (clip_min/max) | 0x300-0x398 |
| **Scissor** | x, y, width, height | 0x8C0-0x8C4 |
| **Clear** | color value (ARGB), zstencil value | 0x1D0, 0x1D8, 0x1D94 |
| **Blend** | enable, src/dst factors, equation, color | 0x310-0x350 |
| **Depth** | test enable, function, write mask | 0x304-0x30C |
| **Stencil** | enable, func, ref, mask, fail/zfail/zpass ops | 0x360-0x378 |
| **Cull** | enable, face (front/back), winding (CW/CCW) | 0x2BC-0x2C4 |
| **Color mask** | ARGB channel write enables | 0x028 |
| **Alpha test** | enable, function, reference value | 0x104-0x10C |
| **Textures** | 16 units × 8 registers (offset, format, address, control, filter, rect, border) | 0x1A00-0x1BFF |
| **Vertices** | 16 attribs × format + offset (type, size, stride, enabled) | 0x1680-0x177F |
| **Shaders** | fragment program addr, vertex load slot, constants, output mask | 0x8E4, 0x1E9C, 0x1EFC |
| **Draw** | primitive type, draw_arrays (first+count), draw_indexed | 0x1808, 0x1814, 0x1820 |

Each category has a **dirty flag**. When a method modifies state, the dirty flag is set. Before a draw call (`SET_BEGIN_END`), all dirty state is flushed to the backend via callbacks. This ensures the backend only processes actual changes, not redundant updates.

### Processing Flow

```c
// Game writes methods to command buffer via put pointer
// We process them when the game calls flip or when we detect put > get

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size);
// Parses headers, extracts method+data, calls rsx_process_method for each

int rsx_process_method(rsx_state* state, u32 method, u32 data);
// Updates rsx_state and sets dirty flags
// For draw commands, dispatches to backend
```

### Reference
- NV47xx method register documentation: [envytools rnndb](https://envytools.readthedocs.io/)
- RPCS3's method handling: `rpcs3/Emu/RSX/rsx_methods.cpp`

---

## Layer 3: Graphics Backends

### Backend Interface

**File:** `rsx_commands.h` (the `rsx_backend` struct)

Every backend implements these callbacks:

```c
typedef struct rsx_backend {
    void* userdata;              // backend-specific state pointer

    // Lifecycle
    int  (*init)(void*, u32 w, u32 h);
    void (*shutdown)(void*);
    void (*begin_frame)(void*);
    void (*end_frame)(void*);
    void (*present)(void*, u32 buffer_id);

    // State changes (called when dirty flags set, before draw)
    void (*set_render_target)(void*, const rsx_state*);
    void (*set_viewport)(void*, const rsx_state*);
    void (*set_blend)(void*, const rsx_state*);
    void (*set_depth_stencil)(void*, const rsx_state*);
    void (*set_color_mask)(void*, const rsx_state*);
    void (*set_alpha_test)(void*, const rsx_state*);
    void (*set_shader)(void*, const rsx_state*);
    void (*set_vertex_attribs)(void*, const rsx_state*);

    // Rendering
    void (*clear)(void*, u32 flags, u32 color, float depth, u8 stencil);
    void (*draw_arrays)(void*, u32 primitive, u32 first, u32 count);
    void (*draw_indexed)(void*, u32 primitive, u32 offset, u32 count);
    void (*bind_texture)(void*, u32 unit, const rsx_texture_state*);
} rsx_backend;
```

Register a backend with `rsx_set_backend(&my_backend)`. Only one backend can be active at a time.

### Null Backend

**File:** `libs/video/rsx_null_backend.c`

The default backend and the CI path. It keeps a `u32` framebuffer and a depth
buffer in host memory, executes the guest command stream, and flat-shades
triangles on the CPU. Nothing is presented to a window: `present()` leaves the
centre pixel for the host harness to read back. Build `ps3recomp_host` and its
scene checks to verify the command pipeline without a GPU or display.

```c
rsx_null_backend_init(1280, 720, "My Game");
// ... game runs ...
rsx_null_backend_present();                    // finish the frame
```

### Vulkan Backend (opt-in)

**File:** `libs/video/rsx_vulkan_backend.c`

Real GPU rendering, built with `-DPS3RECOMP_RSX_VULKAN=ON`. Vulkan is loaded
at run time with `dlopen("libvulkan.so.1")`, so the build needs only the
headers and a machine without a loader fails init cleanly. Only Vulkan 1.0
core entry points are used.

There are two draw paths, selected by `PS3RECOMP_RSX_ENGINE`:

| Path | What it does |
|------|--------------|
| `dispatch` (default with guest programs) | The shared register-file draw engine (`rsx_draw_engine.c`) owns surfaces, render-to-texture, MRT, depth textures, vertex compaction and pipeline keys; the backend builds Vulkan pipelines from the guest's own vertex and fragment programs. |
| `vtable` | The null backend's fixed-function contract: position from attribute 0, flat colour from attribute 3, texture unit 0, depth test. Used to compare the two renderers pixel for pixel. |

**Pipeline:**

```
1. Record the frame's clears, draws, texture uploads and optional window
   blit into one command buffer
2. Submit once; two frames are in flight, each with its own command buffer,
   fence and staging arena
3. Present is ordered by a semaphore, so the FIFO walker records frame N
   while the GPU draws N-1
4. Read the frame back for the host's pixel checks (and PS3RECOMP_VK_DUMP)
```

Guest programs are decompiled to HLSL by `rsx_vp_decompiler.c` /
`rsx_fp_decompiler.c` and translated to SPIR-V by glslang
(`rsx_shader_spirv.cpp`). Textures cover the RSX layouts and formats the
draw engine decodes, including mip chains and per-unit samplers. Images stay
in `VK_IMAGE_LAYOUT_GENERAL` for their whole life, so there is no layout
bookkeeping.

Environment:

| Variable | |
|---|---|
| `PS3RECOMP_VK_DEVICE=<n>` | pick physical device n instead of the first non-CPU one |
| `PS3RECOMP_VK_DUMP=<path>` | write every presented frame as a binary PPM |
| `PS3RECOMP_VK_WINDOW=1` | show frames in an SDL2 window (rendering stays offscreen) |
| `PS3RECOMP_VK_FULLSCREEN=1` | borderless full screen with a window |
| `PS3RECOMP_VK_HOLD=<sec>` | keep the last frame on screen before shutdown |
| `PS3RECOMP_VK_GUEST_PROGRAMS=1` | run the guest's own shader programs |
| `PS3RECOMP_RSX_ENGINE=dispatch\|vtable` | pick the draw path explicitly |

---

## Utility Headers

### Primitive Type Mapping (`rsx_primitives.h`)

Maps RSX primitive types to the topologies every modern host API has. Quads,
quad strips, triangle fans and polygons have no direct equivalent, so their
vertices are expanded on the CPU into a triangle list. `LINE_LOOP` is drawn as
a plain strip (the closing edge is not emitted yet).

| RSX Primitive | Value | Drawn as | Expansion |
|--------------|-------|----------|-----------|
| Points | 1 | POINTS | Direct |
| Lines | 2 | LINES | Direct |
| Line Loop | 3 | LINE_STRIP | Approximated |
| Line Strip | 4 | LINE_STRIP | Direct |
| Triangles | 5 | TRIANGLES | Direct |
| Triangle Strip | 6 | TRIANGLE_STRIP | Direct |
| Triangle Fan | 7 | TRIANGLES | CPU expansion |
| Quads | 8 | TRIANGLES | CPU expansion |
| Quad Strip | 9 | TRIANGLES | CPU expansion |

### Vertex Format Mapping (`rsx_vertex_formats.h`)

Decodes RSX vertex attribute types (from `NV4097_SET_VERTEX_DATA_ARRAY_FORMAT`)
and the frequency-divider rules. The draw engine fetches attributes into a
compact host-side vertex buffer (`rsx_vertex_fetch.c`), so the backend does not
see DXGI or Vulkan format enums at all.

| RSX Type | ID | Component Size | Meaning |
|----------|-----|---------------|---------|
| None | 0 | - | Disabled |
| S1 (snorm16) | 1 | 2 bytes | Signed normalized |
| F (float32) | 2 | 4 bytes | Float |
| SF (float16) | 3 | 2 bytes | Half float |
| UB (unorm8) | 4 | 1 byte | Unsigned normalized |
| S32K (s16) | 5 | 2 bytes | Signed 16-bit |
| CMP (packed) | 6 | 4 bytes | 11-11-10 packed |
| UB256 (uint8) | 7 | 1 byte | Unsigned, not normalized |

## RSX Shader Translation

The RSX uses a proprietary shader ISA derived from NVIDIA's NV40/NV47
architecture. Both stages are translated at run time:

- **Vertex programs** live in transform program memory, loaded through
  `NV4097_SET_TRANSFORM_PROGRAM_LOAD` (method 0x1E9C); up to 512 instructions.
- **Fragment programs** live in VRAM or main memory at
  `NV4097_SET_SHADER_PROGRAM` (method 0x08E4), variable length, terminated by
  an end instruction.

### Translation Pipeline

1. **Parse** the NV40 ISA encoding.
2. **Decompile** to HLSL (`rsx_vp_decompiler.c`, `rsx_fp_decompiler.c`).
3. **Translate** HLSL to SPIR-V with glslang (`rsx_shader_spirv.cpp`).
4. **Cache** pipelines by their register-file and program keys, so a title's
   shaders are built once (asynchronously where possible).

The fragment path handles the RSX's texture, colour-combine and alpha-test
state that the games in this tree exercise; see
[RSX_FRAGMENT_PROGRAM.md](RSX_FRAGMENT_PROGRAM.md) for the supported
instruction set and the remaining gaps.

**Reference implementations:**
- RPCS3: `rpcs3/Emu/RSX/Program/RSXVertexProgram.cpp` and `RSXFragmentProgram.cpp`
- Mesa NV40: `src/gallium/drivers/nouveau/nv30/` (open-source NV40 shader compiler)
- envytools: NV40 ISA documentation at [envytools.readthedocs.io](https://envytools.readthedocs.io/)

---

## Memory Architecture

The RSX has two memory regions:

| Region | Size | Guest Address | Purpose |
|--------|------|---------------|---------|
| **Local (VRAM)** | 256 MB | Configured at init | Framebuffers, textures, vertex data |
| **Main (mapped)** | Variable | Game-specified | GPU-accessible system memory |

`cellGcmMapMainMemory()` makes a region of main memory visible to the RSX by mapping it into the IO address space. The mapping creates entries in the offset table that `cellGcmAddressToOffset()` uses for translation.

**In ps3recomp**, both regions are in the same flat guest VM address space. The distinction matters for the RSX command processor (which needs to know whether a texture/buffer address is in local or main memory) but not for the host CPU (which can access both via `vm_base + addr`).

---

## Integration Guide (For Game Porters)

### Step 1: Null Backend (Prove Command Flow)

```c
// In your game's main.cpp, before entering the game loop:
rsx_null_backend_init(1280, 720, "My Game");
```

Verify that:
- `cellGcmInit` succeeds (log shows RSX configuration)
- Display buffers are registered
- The game loop calls flip functions

### Step 2: Vulkan Backend (Real Rendering)

Build the runtime with `-DPS3RECOMP_RSX_VULKAN=ON`, then register the Vulkan
backend instead of the null one. With `PS3RECOMP_VK_WINDOW=1` the frame is
shown in an SDL2 window; without it rendering stays offscreen and the host
harness can read the frame back.

```c
rsx_vulkan_backend_init(1280, 720, "My Game");
```

### Step 3: Identify Draw Patterns

Set `RSX_DRAW_STATS=1` and `RSX_PIPE_LOG=1` to see what reaches the backend:
primitive type, vertex count, attributes and pipelines. Small counts suggest
UI elements; large counts suggest 3D geometry.

### Step 4: Guest Programs

Set `PS3RECOMP_VK_GUEST_PROGRAMS=1` to run the title's own vertex and fragment
programs on the draw engine. `RSX_HLSL_DUMP_DIR=<dir>` writes the translated
HLSL for inspection; `RSX_ENGINE_SLOW=1` selects the engine's older per-draw
paths for comparison.

---

## File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `cellGcmSys.c` | 830 | RSX state management HLE (33+ functions) |
| `cellGcmSys.h` | 310 | GCM struct definitions and constants |
| `cellVideoOut.c` | 100 | Video output resolution/mode config |
| `cellResc.c` | 254 | Resolution scaling (16 functions) |
| `rsx_commands.c` | 400 | NV47xx FIFO parser + state tracker |
| `rsx_commands.h` | 300 | Method defines, rsx_state, rsx_backend |
| `rsx_draw_engine.c` | 4000 | shared register-file draw engine (surfaces, pipelines, draws) |
| `rsx_vulkan_backend.c` | 4500 | Vulkan device, pipelines, textures, optional window |
| `rsx_null_backend.c` | 500 | headless CPU rasteriser |
| `rsx_primitives.h` | 120 | topology mapping + expansion rules |
| `rsx_vertex_formats.h` | 120 | RSX vertex attribute decoding |
| `rsx_shader_spirv.cpp` | 200 | HLSL → SPIR-V through glslang |
