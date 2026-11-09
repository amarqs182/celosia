# R36S port: first plan

Goal of this fork: run the Vita build's design on the R36S (RK3326: quad
Cortex-A35, Mali-G31, ARM 32-bit userland on dArkOS). Written from a static
read of the tree on 2026-11-09; nothing here has been built or measured on
the device. Items marked **(check)** are assumptions to verify first.

## What the tree already gives

- **Recompiled game code is plain C++.** `./celosia vita` reuses
  `build/runner` from `setup` with another toolchain; an R36S target can do
  the same (`cmd_r36s`, modelled on `cmd_vita`).
- **A 32-bit ARM + NEON path exists and is not Vita-only.**
  `ps2_runtime_macros.h` (about line 178) is gated on
  `__arm__ && !__aarch64__ && __ARM_NEON && USE_SSE2NEON`. The Vita-specific
  tuning is under `__vita__` (16 blocks in `native_tuning.h`, plus
  `ps2_runtime_macros.h` around lines 626-722 and `EeScheduler.cpp`).
- **The GL backend is small.** `native_renderer_gl.cpp` (10 051 lines) uses
  about 70 distinct GL calls. No geometry shaders, no compute.
- **The platform layer is a small raylib subset.**
  `platform/vita/raylib/raylib.h` is 202 lines (window, textures, keys and
  gamepad, an audio stream callback); `raylib_vita.cpp` implements it on SDL2
  and vitaGL in 1 428 lines.

## What is in the way

1. **The GL backend only compiles on macOS and on the Vita.** Its first
   `#if` is `defined(__APPLE__) || (defined(__vita__) && defined(PS2X_VITA_SDL2_SHIM))`;
   on any other platform the file is empty.
2. **Shaders are `#version 330 core`** (`kMainVertex`, `kMainFragment`,
   `kQuadVertex`, the world fragment, and others). Mali-G31 is a GLES part
   **(check: which driver dArkOS ships, libmali or Panfrost, and the
   GLES version it reports)**. They need `#version 300 es` plus precision
   qualifiers.
3. **Desktop-only calls:** `glClearDepth(1.0)` (2 sites, around lines 6329 and
   6491 of `native_renderer_gl.cpp`; GLES has `glClearDepthf`, which the Vita
   branches already use). `glMapBuffer(GL_WRITE_ONLY)` (around lines 3944 and
   4234) is not a problem: both sites are inside
   `#if defined(PS2X_NATIVE_VITA)` blocks (vitaGL buffer memory), so a
   non-Vita target does not compile them. `texelFetch` on depth textures is
   fine in GLES 3.0.
   Because of that, decide how the R36S relates to `PS2X_NATIVE_VITA`: if it
   does not define it, every such block takes its desktop `#else` branch,
   and those branches must be read for GLES problems **(check)**; if it
   defines it, the vitaGL-only code under it will not compile.
4. **Vita-only extensions** (`vglCelo*`, `vgl_celo_scenes`, `blend_info`,
   `gxm_context`) are declared under `PS2X_NATIVE_VITA`; they must stay behind
   that guard.
5. **The pipeline is documented for macOS only.** `./celosia setup` captures
   each module by running the game on the host. Whether that works on
   Linux x86_64 is untested **(check)**: the Metal path is Apple-only and
   other hosts fall back to the emulated GS (`gs_metal_raster_stub.cpp`).
   If it does not, the generated `build/runner` from a Mac can be copied.
6. **GPU, not CPU, may be the limit.** The A35 cores at ~1.3 GHz should beat
   the Vita's A9s at 444 MHz, but Mali-G31 MP2 and the RK3326's memory
   bandwidth are probably weaker than the Vita's SGX543MP4 **(check by
   measuring)**. Bloom, depth of field, reflections and refraction are the
   multi-pass cost.

## Steps, in order

1. **Host bootstrap.** Run `./celosia setup` on the Linux machine; record
   what breaks. Fallback: use the runner sources from a Mac.
2. **Toolchain.** `arm-linux-gnueabihf` cross compiler and a sysroot taken
   from the device (SDL2, EGL, GLESv2). Add `cmd_r36s` next to `cmd_vita`.
3. **Platform shim.** `platform/r36s/raylib`: the same 202-line API on SDL2
   with KMSDRM + EGL (GLES), gamepad through `SDL_GameController`, audio
   through the SDL callback. Start by copying `raylib_vita.cpp` and removing
   GXM.
4. **GLES shaders.** Add a `#version 300 es` prologue chosen at compile time;
   replace the two `glClearDepth` calls with `glClearDepthf`; widen the first
   `#if` to the new target and settle the `PS2X_NATIVE_VITA` question (item 3
   above).
5. **First picture.** Boot to the logos with every effect off; then the
   title screen; then the intro.
6. **Defaults.** Start from the Vita's: render scale 1 (512x448 on a 640x480
   screen), fewer particles, 30 fps cap in scenes (`SOTC_CAP30`), and the
   `[gl:mem]` budget lowered for ~1 GB total RAM **(check the RAM of this
   unit)**.
7. **Measure** with the existing frame log and `[gl:mem]`, before deciding
   what else to cut.
