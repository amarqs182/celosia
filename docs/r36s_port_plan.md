# R36S port: initial engineering plan

Goal: adapt Celosia's existing PS2 recompilation/runtime and native renderer to the
R36S, reusing the Vita and macOS work where the abstractions are portable. This
document reviews the current `main` branch. The older experimental `r36s`
branch records a Debian 13/AArch64 target and a successful start of an ELF64
binary on the device, but its run log still stops before gameplay. Those
results do not mean the current `main` branch contains the Linux port or that
the game is playable on the R36S.

## Measured target facts

The following values were recorded on the R36S over SSH on 2026-11-09:

| Item | Measured value |
| --- | --- |
| Board / SoC | `rockchip,rk3326-odroidgo3-linux` (RK3326) |
| CPU | 4× Cortex-A35 (ARM implementer `0x41`, part `0xd04`), AArch64 |
| CPU features | `fp asimd evtstrm aes pmull sha1 sha2 crc32`; no FMA, FP16 or SVE |
| RAM | 895 MiB total; approximately 726 MiB available |
| GPU | Mali-G31, one core / two execution engines, GPU ID `0x7093` |
| GPU driver | ARM libmali Bifrost r13p0, using the kbase driver via `/dev/mali0` |
| Graphics API | OpenGL ES 3.2 |
| Display | 640×480; DRM `card0` / `renderD128` via `rockchip-drm` |
| OS / libc | Debian 13 (trixie), glibc 2.41; kernel 4.4.189 |

The target is Linux AArch64, not 32-bit ARM. The active graphics stack is
ARM libmali, not Panfrost. Budget memory against the measured 895 MiB rather
than the nominal 1 GB.

The device's EGL/GBM path must not be initialized manually on
`/dev/dri/renderD128`: a previous attempt caused a kernel NULL dereference
and hung the device until a power cycle. Let SDL2 own display and context
creation through its KMSDRM video driver.

## Validation status recorded on 2026-11-09

The AArch64 cross-build compiled the runtime and unit-test target, and the
`ps2x_tests` binary ran on the R36S. It reproduced the same two known test
failures and final assertion observed in the host run; these were recorded as
pre-existing failures, not as an AArch64 regression.

The game itself has not yet been built or run on the R36S. The setup/capture
pipeline, gameplay rendering, frame timing and image output therefore remain
unverified. Do not describe the port as playable or the renderer as validated
until those checks have been performed.

## What can be reused

- The game code emitted by PS2Recomp is C++. The setup pipeline generates the
  runner sources; a separate target build can compile those sources for the
  R36S without recompiling the game's MIPS code on the device.
- The top-level `PS2Recomp/CMakeLists.txt` already recognizes `aarch64`,
  `arm64` and `ARM64`, and enables the AArch64 NEON/SSE2NEON path. Use an
  AArch64 toolchain and firmware-matched sysroot for cross-compilation; do not
  use the 32-bit ARMHF toolchain or the `__arm__ && !__aarch64__` code path.
- The runtime, PS2 subsystem emulation, game overrides and most of the native
  renderer's draw logic are valuable starting points. Vita-specific kernel,
  allocator, display and GXM calls are not portable platform abstractions.
- The renderer already has a backend interface in
  `PS2Recomp/ps2xRuntime/src/lib/native/native_backend.h`. Keep that seam.
- The old `r36s` branch contains an experimental Linux GLES renderer path:
  `native_renderer_gl.cpp` uses GLSL ES 3.10, GLES depth-stencil targets and
  checks the runtime ES version. This is useful implementation reference,
  but it is based on an older upstream snapshot and must be reviewed against
  current `main` before any code is copied.
- The Vita raylib shim documents the small host API Celosia uses (window,
  input, texture and audio functions). Reuse the API contract, not its
  Vita-specific implementation.

## Main blockers found in the source

1. **Native GL compilation is gated to macOS and Vita.**
   `native_renderer_gl.cpp` is compiled only for those paths, and the GL
   backend factory does not select it on generic Linux. Add an explicit Linux
   target path; do not define `PS2X_NATIVE_VITA` for the R36S because that
   enables vitaGL/GXM and Vita kernel code.
2. **The current shaders target desktop GLSL.**
   The native GL shaders use `#version 330 core`. Mali-G31 is a GLES GPU, so
   the target needs a GLES 3.x shader variant (or a controlled source
   translation) with the required precision declarations. Keep desktop GL
   shaders unchanged.
3. **GL API differences need a branch-by-branch audit.**
   GLES uses `glClearDepthf`; desktop GL uses `glClearDepth`. Buffer mapping
   and other desktop-only calls must be checked in the non-Vita branch that
   Linux will compile. Do not globally replace calls without checking the
   Vita and macOS paths.
4. **Linux host integration is missing from `main`.**
   The old `r36s` branch contains an experimental SDL2/GLES raylib subset and
   a Linux CMake path, but those changes are not in `main` and should not be
   copied wholesale: the shim still contains unimplemented input/misc
   functions and TODOs. Its `BeginDrawing()` also has an explicit TODO for
   coordinating the host GL state with the renderer thread. Rebase useful
   parts onto current `main` in small changes. Select SDL's video driver only
   after checking which drivers and DRM/KMS access the installed firmware
   actually provides.
5. **The Linux GL context is not connected to the native renderer.**
   Current `ps2_runtime.cpp` calls `native_renderer::setGlContext` for
   macOS and Vita, but has no Linux path. The old branch also lacks a Linux
   `setGlContext` hookup even though its renderer has an experimental
   `__linux__` GLES implementation. That old renderer requires OpenGL ES
   3.1 (GLSL ES 3.10) and refuses to initialize below it. Design SDL context
   ownership/sharing between the main and render threads before enabling
   `SOTC_NATIVE_GL`; a single SDL GL context cannot be current on two threads
   at once.
6. **The setup pipeline needs a host runner.**
   `./celosia setup` captures module state by running the generated host
   runner. First validate this flow on the Linux development machine. The
   R36S target runner should then be cross-compiled using a sysroot that
   matches the device ABI and its SDL2/EGL/GLES libraries.
7. **Memory and performance must be measured on the device.**
   The R36S has 1 GB shared memory. Revisit render-target and texture budgets
   before enabling all effects. Do not infer CPU/GPU performance from clock
   rates or Vita results alone; record frame time, GPU behavior and memory
   use on the R36S.

## First target checks

Repeat these on the exact R36S image that will launch the game. The previous
work log already identifies the device as Debian 13/AArch64; these commands
make that environment reproducible:

```sh
uname -m
getconf LONG_BIT
dpkg --print-architecture 2>/dev/null || true
file /bin/sh
```

Then record the actual SDL2 video drivers, EGL version, GLES version and GLSL
version exposed by the image. The old R36S renderer implementation explicitly
requires OpenGL ES 3.1 / GLSL ES 3.10 and stays disabled below that version;
do not treat an ES 3.0 context as sufficient. Previous device experiments
with libmali and Vulkan are useful evidence that the GPU driver works, but
they do not prove that SDL2 can create the required EGL/GLES context.

## Work sequence

1. **Freeze the build environment.** Reconfirm the exact AArch64 compiler
   triple, libc and SDL2/EGL/GLES libraries. For cross-compilation, use an
   `aarch64-linux-gnu` toolchain with a sysroot copied from the target
   firmware; a native AArch64 build on the R36S is an alternative only if its
   compiler and available memory are sufficient. Build a tiny SDL2/EGL/GLES
   smoke test that requests ES 3.1 and reports the actual EGL, GLES and GLSL
   versions before changing Celosia.
2. **Validate host setup.** Run the existing setup pipeline on Linux x86_64,
   including module capture and generation of runner sources.
3. **Add the target build.** Add an explicit R36S/Linux AArch64 build option
   and toolchain configuration matching the recorded device sysroot. Keep it
   separate from Vita options.
4. **Add the platform adapter.** Implement only the raylib API surface Celosia
   uses, backed by SDL2 and EGL/GLES. Keep Vita kernel/GXM code isolated.
5. **Enable the native renderer on GLES.** Add the Linux backend selection,
   GLES shader variants and the minimal API compatibility changes. Preserve
   macOS and Vita implementations.
6. **Reach the first picture.** Test boot logos, title screen and intro with
   expensive effects disabled; then enable effects incrementally.
7. **Tune against measurements.** Set conservative render-target and texture
   budgets for 1 GB shared memory, then profile frame time and memory before
   adding CPU-specific optimizations.

Each step should land as a small change with a build or device test attached.
