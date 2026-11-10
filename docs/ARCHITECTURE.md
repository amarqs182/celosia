# Celosia Architecture

## Pipeline Overview

```mermaid
flowchart TD
    A[Disc ISO Extraction] --> B[tools/build_elf.py]
    B --> C[Combined ELF: boot + XFF modules]
    C --> D[tools/funcmap_elf.py]
    D --> E[Function map CSV]
    E --> F[tools/gen_config.py]
    F --> G[game/scus.toml config]
    G --> H[PS2Recomp ps2xRecomp]
    H --> I[Generated C++ code]
    I --> J[PS2Recomp ps2xRuntime]
    J --> K[Native C++ execution]
    K --> L[Native renderer: Metal/macOS or OpenGL ES/Vita]
    L --> M[Playable game]

    subgraph PS2Recomp internals
        H --> H1[ps2xRecomp: MIPS R5900 -> C++ translators]
        H1 --> H2[decoder, FPU/COP0/MMI/VU translators]
        H2 --> H3[jump tables, control flow emitters]
        H3 --> H4[code_generator: portable C++ output]
    end

    subgraph Runtime
        J --> J1[ps2xRuntime: memory model, syscalls]
        J1 --> J2[ps2_call_list.h dispatch]
        J1 --> J3[GS backends: hw/cpu/async]
        J1 --> J4[VU0/VU1 interpreters]
        J1 --> J5[GIF/DMA arbiter, pad, audio, VFS]
        J1 --> J6[ROM device, IOP host transport]
    end

    subgraph Game native overrides
        G --> G1[game/overrides/decomp_*.cpp: 115 files, 332 SOTC_DECOMP_FUNCTION]
        G1 --> G2[sotc_decomp.cpp registry + install()]
    end
```

## Subsystem → Directory Mapping

| Subsystem | Directory | Key Files |
|---|---|---|
| ELF/build pipeline | `tools/` | `build_elf.py`, `funcmap_elf.py`, `gen_config.py` |
| Recompiler (MIPS→C++) | `PS2Recomp/ps2xRecomp/` | `code_generator.cpp`, translators (fpu, cop0, mmi, vu), emitters |
| Runtime (EE emulation) | `PS2Recomp/ps2xRuntime/` | `ps2_call_list.h`, `ps2_memory.h`, `ps2_gif_arbiter.cpp`, VU interpreters, GS backends |
| IOP (R3000A emulator) | `PS2Recomp/ps2xIOP/` | `iop_cpu.cpp`, `iop_kernel.cpp`, modules (libsd, dbcman, mcserv), native audio |
| Test suite | `PS2Recomp/ps2xTest/` | MiniTest harness, compiler/runtime tests |
| GUI studio | `PS2Recomp/ps2xStudio/` | imgui-based inspection and control |
| Game native overrides | `game/overrides/` | 115 `decomp_*.cpp` files registered via `SOTC_DECOMP_FUNCTION` in `sotc_decomp.h` |
| Config & symbols | `game/` | `scus.toml`, `pinned_starts.txt`, `extra_stubs.txt` |

## Memory Model

The PS2 runtime emulates the following physical memory regions (defined in `PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h`):

| Region | Base Address | Size | Notes |
|---|---|---|---|
| RDRAM | `0x00000000` | 32 MB | Main RAM; masked with `PS2_RAM_MASK` |
| Scratchpad | `0x70000000` | 16 KB | VU scratchpad; alias at `0xF0000000` |
| I/O registers | `0x10000000` | 64 KB | Timers, DMAC, INTC |
| BIOS | `0x1FC00000` | 4 MB | Firmware code/data |
| VU0 code | `0x11000000` | 4 KB | Microcode memory |
| VU0 data | `0x11004000` | 4 KB | VU0 data memory |
| VU1 code | `0x11008000` | 16 KB | VU1 microcode |
| VU1 data | `0x1100C000` | 16 KB | VU1 data memory |
| GS VRAM | — | 4 MB | Graphics synthesiser video RAM |

Access validation functions (`ps2IsScratchpadAddress`, `ps2IsRDRAMAddress`, etc.) route emulated accesses to the correct handler.

## Platform Selection

Compilation targets are selected via preprocessor defines in source files under `PS2Recomp/ps2xRuntime/src/lib/native/`. The relevant defines are:

| Define | Used in | Purpose |
|---|---|---|
| `_WIN32` | Various stubs (`CD.cpp`, `MemoryCard.cpp`, `Common.h`, `Runtime.h`) | Windows-specific code paths |
| `__APPLE__` | `native_renderer.cpp`, `native_renderer_gl.cpp` | macOS: Metal backend (default) or OpenGL via `SOTC_NATIVE_GL=1` |
| `__vita__` | `native_renderer.cpp`, `native_runtime.cpp`, `recorder.cpp` | PS Vita: OpenGL ES via vitaGL; raylib Vita subset |
| `__linux__` | `native_renderer_gl.cpp` | Linux: OpenGL ES (GLES3) via raylib |
| `PS2X_HAVE_METAL` | `gs_cpu_backend.cpp`, `metal/gs_metal_raster.mm` | Metal enabled only on Apple platforms; guards Metal-only code paths |

The native renderer entry point (`native_renderer.cpp:1266`) first checks `wantGl()` which returns `true` on Vita, and on macOS checks the `SOTC_NATIVE_GL` environment variable (Metal by default, OpenGL when `SOTC_NATIVE_GL=1`). On Windows and Linux, Metal is unavailable and the appropriate GL backend is selected.

## No x86/ARM Codegen Backend

The MIPS R5900 output from `ps2xRecomp` is **pure portable C++**. The translators (instruction decoder, FPU/COP0/MMI/VU), control flow emitters, jump table switch emitter, and function emitter all generate C++ source code that is architecture-agnostic. No x86 or ARM-specific instruction selection, register allocation, or code emission occurs. The generated C++ compiles identically on macOS (Apple Silicon), Linux (x86_64 / ARM), and PS Vita (ARM). This portability is what enables the project to run on both Apple Silicon Macs and PS Vita without recompiling the recomplier frontend.

## Build System

The project uses **CMake** (minimum 3.21) with **Ninja** generator. Third-party libraries are fetched via **FetchContent** at configure time:

| Library | CMake Module | FetchContent Tag |
|---|---|---|
| `elfio` | `ps2xRecomp/CMakeLists.txt` | `Release_3.12` |
| `toml11` | `ps2xRecomp/CMakeLists.txt` | `v4.4.0` |
| `fmt` | `ps2xRecomp/CMakeLists.txt` | `12.1.0` |
| `libdwarf` | `ps2xRecomp/CMakeLists.txt` | `v2.2.0` |
| `rabbitizer` | `ps2xRecomp/CMakeLists.txt` | `1.14.3` |
| `raylib` | `ps2xRuntime/CMakeLists.txt` | `5.5` |
| `imgui` | `ps2xStudio/CMakeLists.txt` | `v1.92.7-docking` |
| `SDL2` | `ps2xStudio/CMakeLists.txt` | `release-2.32.10` |
| `sse2neon` | `PS2Recomp/CMakeLists.txt` (ARM only) | `v1.9.1` |

CMake configure defines platform-specific options (`PS2X_BUILD_RECOMP`, `PS2X_BUILD_RUNTIME`, `PS2X_BUILD_ANALYZER`, `PS2X_BUILD_TEST`, `PS2X_BUILD_STUDIO`, `PS2X_VITA_SDL2_SHIM`) and conditionally enables ARM (`sse2neon`) NEON crypto/CRC extensions when cross-compiling for ARM64 targets.
