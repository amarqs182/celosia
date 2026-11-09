# R36S port: first plan

Goal of this fork: run the Celosia game/runtime on the R36S (RK3326: quad
Cortex-A35, Mali-G31) using the device's **Linux AArch64 userspace**. The
AArch64 userspace architecture was confirmed by the project owner on
2026-11-09. This is a static source review; nothing here has been built or
measured on the device. Items marked **(check)** are assumptions to verify.

## What the tree already gives

- **Recompiled game code is plain C++.** The setup pipeline generates runner
  sources; those sources can be compiled for the target separately from the
  host-side capture/build steps.
- **AArch64 and NEON compiler support already exists.** The top-level
  `PS2Recomp/CMakeLists.txt` recognizes `aarch64`, `arm64` and `ARM64`,
  enables `USE_SSE2NEON`, and adds `-march=armv8-a+fp+simd` for non-Apple,
  non-MSVC AArch64 builds. The 32-bit-only `__arm__ && !__aarch64__` path in
  `ps2_runtime_macros.h` is not the target ABI for this device.
- **The GL backend is small relative to the runtime.**
  `native_renderer_gl.cpp` uses desktop OpenGL 3.3 on macOS and vitaGL on
  Vita. It is currently excluded on ordinary Linux builds.
- **The platform layer is a small raylib subset.**
  `platform/vita/raylib/raylib.h` defines the API surface used by the
  runtime; `raylib_vita.cpp` implements it with SDL2 plus Vita-specific
  GXM/system APIs. Reuse the API contract, not the Vita implementation.

## What is in the way

1. **The GL backend only compiles on macOS and on the Vita in `main`.**
   Its platform guard excludes Linux, so a Linux build currently gets no
   native GL backend.
2. **Shaders use `#version 330 core`.** Mali-G31 exposes an OpenGL ES
   implementation through the installed driver. The exact GLES version and
   shader compiler behavior on this R36S image still need to be checked.
   The GLES path requires `#version 300 es` and appropriate precision
   declarations.
3. **Desktop-only calls:** `glClearDepth(1.0)` must become
   `glClearDepthf(1.0f)` for GLES. Other desktop GL calls and framebuffer /
   texture formats still need a GLES audit against the target driver.
4. **Vita-only extensions** (`vglCelo*`, `vgl_celo_scenes`, `blend_info`,
   `gxm_context`) must remain behind Vita-specific guards. The R36S must not
   be compiled as `PS2X_NATIVE_VITA`.
5. **The runtime host backend is not yet an R36S host.** Linux currently
   follows the generic raylib path. The project needs an SDL2/EGL/GLES host
   integration (or another verified host path) that creates a context,
   handles input/audio, and supplies the renderer's make-current/present
   hooks. The exact SDL video driver and display path are **(check)**.
6. **The 1 GB shared memory budget is a constraint.** CPU allocations,
   textures, render targets, the operating system and GPU driver all compete
   for memory. Effect presets and renderer pools must be measured on-device.

## Steps, in order

1. **Confirm target ABI and libraries.** The device uses AArch64 userspace.
   For cross-compilation, use an `aarch64-linux-gnu` toolchain and a sysroot
   from the actual firmware image. Alternatively, build natively on the R36S
   if its compiler and available memory permit it. The sysroot must match the
   target's libc and provide the SDL2, EGL and GLES headers/libraries.
2. **Host bootstrap.** Run the existing setup/capture pipeline on the
   development host and record what works. Keep host-generated runner sources
   separate from the target runner build; do not assume the host executable
   can run on AArch64.
3. **Platform shim.** Implement the raylib subset for Linux using SDL2, with
   an EGL/GLES context. Select KMSDRM or another SDL video driver only after
   confirming what the installed firmware supports. Use SDL game-controller
   input and the SDL audio callback.
4. **GLES renderer.** Enable the native GL backend for the Linux GLES target,
   convert shaders to GLSL ES 3.00, replace unsupported GL calls, and audit
   texture formats, renderbuffer formats, framebuffer completeness and
   extensions on the actual Mali driver.
5. **First picture.** Reach the boot logos with optional effects disabled;
   then test the title screen and intro. Record shader compile and GL errors.
6. **Memory and performance defaults.** Start conservatively: 640x480 output,
   a reduced internal render scale, limited post-processing and measured
   texture/render-target budgets. Do not copy Vita heap sizes or GPU pool
   assumptions directly.
7. **Measure.** Use the existing frame logging and memory diagnostics where
   available. Change performance settings only from measured bottlenecks.

## Validation status

This document records the target architecture and a bring-up sequence. It
does not claim that the R36S executable, SDL2 host, EGL context, GLES renderer
or gameplay has been built or tested yet.
