# R36S port: initial engineering plan

Goal: adapt Celosia's existing PS2 recompilation/runtime and native renderer to the
R36S, reusing the Vita and macOS work where the abstractions are portable. This
document is a static source review, not a claim that a Linux/ARM64 build or the
game has already run on the device.

Target hardware: RK3326, four Cortex-A35 cores, Mali-G31 MP2 and 1 GB shared
system memory. The CPU supports AArch64, but the target image's **userspace ABI
must be checked** before choosing a compiler or sysroot. A 64-bit kernel does
not by itself prove that the installed userspace is 64-bit.

## What can be reused

- The game code emitted by PS2Recomp is C++. The setup pipeline generates the
  runner sources; a separate target build can compile those sources for the
  R36S without recompiling the game's MIPS code on the device.
- The runtime, PS2 subsystem emulation, game overrides and most of the native
  renderer's draw logic are valuable starting points. Vita-specific kernel,
  allocator, display and GXM calls are not portable platform abstractions.
- The renderer already has a backend interface in
  `PS2Recomp/ps2xRuntime/src/lib/native/native_backend.h`. Keep that seam:
  add a Linux/GLES implementation rather than pretending the R36S is a Vita.
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
4. **Linux host integration is missing.**
   The existing Linux CMake path is not the same as the Vita SDL2/vitaGL
   shim. Add a host adapter for the target's SDL2 + EGL/GLES stack. Select
   SDL's video driver only after checking which drivers and DRM/KMS access
   the installed firmware actually provides; KMSDRM is a candidate, not an
   assumption.
5. **The setup pipeline needs a host runner.**
   `./celosia setup` captures module state by running the generated host
   runner. First validate this flow on the Linux development machine. The
   R36S target runner should then be cross-compiled using a sysroot that
   matches the device ABI and its SDL2/EGL/GLES libraries.
6. **Memory and performance must be measured on the device.**
   The R36S has 1 GB shared memory. Revisit render-target and texture budgets
   before enabling all effects. Do not infer CPU/GPU performance from clock
   rates or Vita results alone; record frame time, GPU behavior and memory
   use on the R36S.

## First target checks

Run these on the exact R36S image that will launch the game:

```sh
uname -m
getconf LONG_BIT
dpkg --print-architecture 2>/dev/null || true
file /bin/sh
```

Then record the actual SDL2 video drivers, EGL version, GLES version and GLSL
version exposed by the image. Previous device experiments with libmali and
Vulkan are useful evidence that the GPU driver works, but they do not prove
that SDL2 can create the required EGL/GLES context.

## Work sequence

1. **Freeze the target ABI.** Record kernel architecture, userspace ABI,
   compiler triple, libc and the exact SDL2/EGL/GLES libraries. Build a tiny
   SDL2/EGL/GLES smoke test before changing Celosia.
2. **Validate host setup.** Run the existing setup pipeline on Linux x86_64,
   including module capture and generation of runner sources.
3. **Add the target build.** Add an explicit R36S/Linux ARM build option and
   toolchain configuration selected from the ABI check. Keep it separate from
   Vita options and do not hard-code ARMHF or AArch64 until the device result
   is recorded.
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
