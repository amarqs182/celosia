# R36S port: first plan

Goal of this fork: run the Vita build's design on the R36S (RK3326: quad
Cortex-A35, Mali-G31) using the device's **Linux AArch64 userland** on
dArkOS. The hardware and driver facts below were **measured on the device
over SSH on 2026-11-09**; the source review is static. Items still marked
**(check)** are assumptions that remain to be verified.

## Measured target facts

Read off the R36S itself, not from a datasheet:

| Item | Measured value |
| --- | --- |
| Board / SoC | `rockchip,rk3326-odroidgo3-linux` (RK3326) |
| CPU | 4× **Cortex-A35** — `CPU implementer 0x41`, `CPU part 0xd04` |
| CPU features | `fp asimd evtstrm aes pmull sha1 sha2 crc32` — **no FMA**, no fp16, no SVE |
| Architecture | **aarch64** (`uname -m`), Linux `4.4.189` |
| RAM | **895 MiB** total, ~726 MiB available |
| GPU | **Mali-G31**, 1 core / 2 execution engines, `r0p0`, GPU ID `0x7093` |
| GPU driver | **ARM libmali** — `libmali-bifrost-g31-rxp0-gbm.so`, Bifrost r13p0, over the ARM kbase kernel driver on `/dev/mali0` |
| GL API | **OpenGL ES 3.2** (`OpenGL ES 3.2 v1.r13p0-01rel0`) |
| EGL | `EGL_KHR_platform_gbm`, `EGL_EXT_platform_base`, `EGL_KHR_client_get_all_proc_addresses` |
| Display | 640×480 framebuffer; DRM `card0` / `renderD128` via `rockchip-drm` |
| OS / libc | Debian 13 (trixie), **glibc 2.41-12+deb13u4** |

Two corrections to earlier revisions of this document, both of which changed
the build plan rather than just the wording:

- **The userland is aarch64, not 32-bit ARM.** `uname -m` returns
  `aarch64` and the loader is `aarch64-linux-gnu`. The 32-bit ARM path
  described in older revisions of this document is not the target.
- **The GL stack is ARM's libmali, not Mesa.** Mesa 25.0.7 is installed and
  `panfrost_dri.so` exists in the DRI directory, but every `libEGL.so.1`,
  `libGLESv2.so.2`, `libGLESv3.so.3`, `libgbm.so.1` and `libwayland-egl.so`
  symlink resolves to `libMali.so`, and the kernel driver is the ARM kbase
  one behind `/dev/mali0`, not Panfrost. `renderD128` is the Rockchip display
  controller, not a GPU render node. Treat the device as libmali plus
  OpenGL ES 3.2.

## What the tree already gives

- **Recompiled game code is plain C++.** `./celosia vita` reuses
  `build/runner` from `setup` with another toolchain; an R36S target can do
  the same (`cmd_r36s`, modelled on `cmd_vita`).
- **The AArch64 + NEON path exists and is not Vita-only.**
  `ps2_runtime_macros.h` (about line 178) is gated on
  `__arm__ && !__aarch64__ && __ARM_NEON && USE_SSE2NEON`, so the AArch64
  target takes the other side of that gate; the top-level
  `PS2Recomp/CMakeLists.txt` already recognises `aarch64`/`arm64`/`ARM64`,
  enables `USE_SSE2NEON` and adds `-march=armv8-a+fp+simd`. The Vita-specific
  tuning is under `__vita__` (16 blocks in `native_tuning.h`, plus
  `ps2_runtime_macros.h` around lines 626-722 and `EeScheduler.cpp`).
- **The GL backend is small.** `native_renderer_gl.cpp` (10 051 lines) uses
  about 70 distinct GL calls. No geometry shaders, no compute.
- **The platform layer is a small raylib subset.**
  `platform/vita/raylib/raylib.h` is 202 lines (window, textures, keys and
  gamepad, an audio stream callback); `raylib_vita.cpp` implements it on SDL2
  and vitaGL in 1 428 lines.
- **A cross-build toolchain now exists.**
  `PS2Recomp/cmake/r36s-toolchain.cmake` builds the tree for aarch64 against
  a sysroot whose libc is glibc 2.41, matching the device.

## What is in the way

1. **The GL backend only compiles on macOS and on the Vita.** Its first
   `#if` is `defined(__APPLE__) || (defined(__vita__) && defined(PS2X_VITA_SDL2_SHIM))`;
   on any other platform the file is empty.
2. **Shaders are `#version 330 core`** (`kMainVertex`, `kMainFragment`,
   `kQuadVertex`, the world fragment, and others). The device provides
   **OpenGL ES 3.2** through libmali, so they need `#version 300 es` (or
   `310 es`) plus precision qualifiers. The GLES 3.2 level means the ES 3.1
   features the Vita path already relies on are available, but ES still
   lacks the desktop-only calls listed next.
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
6. **The display path needs the SDL2 KMSDRM route, not a hand-built EGL
   display.** The device's EGL advertises `EGL_KHR_platform_gbm`, but
   creating a GBM device on `/dev/dri/renderD128` and initialising a display
   on it NULL-derefs in the kernel (`Oops: 96000005`, `Unable to handle
   kernel NULL pointer dereference at virtual address 00000010`) and hangs
   the machine in uninterruptible `D` state, so it needs a physical power
   cycle. Let SDL2 own the display and the context.
7. **GPU, not CPU, may be the limit.** The A35 cores at ~1.3 GHz should beat
   the Vita's A9s at 444 MHz, but the Mali-G31's single core with two
   execution engines and the RK3326's memory bandwidth are probably weaker
   than the Vita's SGX543MP4 **(check by measuring)**. Bloom, depth of
   field, reflections and refraction are the multi-pass cost.
8. **Memory is tighter than the 1 GB usually quoted.** The unit has
   **895 MiB**, shared with the OS, the display controller and the GL driver.
   Effect presets and renderer pools must be measured on-device.

## Steps, in order

1. **Host bootstrap.** Run `./celosia setup` on the Linux machine; record
   what breaks. Fallback: use the runner sources from a Mac.
2. **Toolchain.** Use `aarch64-linux-gnu-gcc` with a sysroot whose glibc is
   2.41 or older, matching the device; `PS2Recomp/cmake/r36s-toolchain.cmake`
   already does this against `~/tools/r36s-sysroot`. Add `cmd_r36s` next to
   `cmd_vita`. Do **not** use `arm-linux-gnueabihf`, and do not add
   `-mfpu=neon-fp-armv8` or `-mfloat-abi=hard` — those are 32-bit ARM
   options that AArch64 rejects.
3. **Platform shim.** `platform/r36s/raylib`: the same 202-line API on SDL2
   with KMSDRM + EGL (GLES), gamepad through `SDL_GameController`, audio
   through the SDL callback. Start by copying `raylib_vita.cpp` and removing
   GXM. The video driver is chosen by SDL2's KMSDRM backend; the project
   must not create its own EGL display (see item 6 above).
4. **GLES shaders.** Add a `#version 300 es` prologue chosen at compile time;
   replace the two `glClearDepth` calls with `glClearDepthf`; widen the first
   `#if` to the new target and settle the `PS2X_NATIVE_VITA` question (item 3
   above).
5. **First picture.** Boot to the logos with every effect off; then the
   title screen; then the intro.
6. **Defaults.** Start from the Vita's: render scale 1 (512x448 on a 640x480
   screen), fewer particles, 30 fps cap in scenes (`SOTC_CAP30`), and the
   `[gl:mem]` budget lowered for the measured **895 MiB** of RAM.
7. **Measure** with the existing frame log and `[gl:mem]`, before deciding
   what else to cut.

## Validation status

The cross-build for aarch64 compiles the runtime and the unit tests, and the
resulting `ps2x_tests` binary runs on the real device, where it reproduces
the same two pre-existing failures and the same final assert as the host
run. The game itself has not been built or run on the R36S: there is no
game disc here, so the setup and capture pipeline has not been exercised
for this target, and no frame timing or picture comparison has been
measured.
