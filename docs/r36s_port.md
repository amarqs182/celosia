# R36S port notes

## Target
- SoC: Rockchip RK3326 (`rockchip,rk3326-odroidgo3-linux`)
- CPU: quad-core Cortex-A35 (`fp asimd`, no FMA/fp16/SVE; `1008–1512 MHz`)
- GPU: Mali-G31 (Bifrost r0p0, GPU ID `0x7093`, `400–600 MHz`) via ARM `libmali` (`OpenGL ES 3.2 v1.r13p0-01rel0`, `-gbm` variant)
- Memory: 895 MiB shared RAM (~708–726 MiB available) + 511 MiB ZRAM (`/dev/zram0`)
- Display: 640×480 KMSDRM scanout at 60 Hz (`card0`/`renderD128` via `rockchip-drm`) + RGA (`/dev/rga`, `librga 2.0.0`)
- Platform: aarch64 Debian 13.7 (trixie) Linux, glibc 2.41, systemd 257, kernel `4.4.189 #1 SMP` (plus `armhf` 32-bit `libMali.so` / linker)

The target is stable 60 FPS with headroom, not merely a momentary 60 FPS. Use a 16.67 ms frame budget and measure frame-time percentiles.

## Platform Capabilities & Kernel `4.4.189` Limitations (Measured 2026-11-15)

Do **not** target or debug technologies that are broken or absent on the R36S software/kernel stack:

| Subsystem / API | Status on R36S | Root Cause / Constraint |
| --- | --- | --- |
| **OpenGL ES 3.2 / ES 1.1 (`libMali.so`)** | ✅ Works | Only hardware-accelerated 3D graphics path (`libEGL.so.1`, `libGLESv2.so.2`, `libGLESv3.so.3` → `libMali.so`). |
| **SDL2 (`2.32.10`) KMSDRM + `librga` + ALSA** | ✅ Works | Built with `KMSDRM` (with `KMSDRM_InitRotateBuffer` via `/dev/rga`), `opengles2`, and `ALSA`. Note `SDL_GL_SetSwapInterval(0)` keeps `actual=1` (~60 Hz KMSDRM flip). |
| **Audio (`ALSA` `rockchip,rk817-codec`)** | ✅ Works | Direct ALSA `card 0, device 0`; no PulseAudio/PipeWire daemon runs by default. |
| **Vulkan (`mali_icd.json` → `libMali.so`)** | ⚠️ Offscreen compute/render only; **NO display WSI** | Exposes `Mali-G31` Vulkan `1.0.108` (`GRAPHICS\|COMPUTE\|TRANSFER`, `VK_KHR_external_memory_fd`), missing `vk_icdGetInstanceProcAddr` (falls back to `vkGetInstanceProcAddr`), **`present support = false`**, and **0 surface/swapchain extensions** (`VK_KHR_surface`, `VK_KHR_display`, `VK_KHR_swapchain` are absent in this `-gbm.so` blob). Normal Vulkan apps/emulators cannot create a swapchain without manual `dma_buf` + DRM/EGL composition. |
| **Vulkan (`panfrost_icd.json` / `lvp_icd.json`)** | ❌ Broken (Panfrost) / CPU-only (Lavapipe) | `libvulkan_panfrost.so` fails with `ERROR_INITIALIZATION_FAILED` (needs mainline `panfrost` DRM kernel ≥ 5.2); `libvulkan_lvp.so` (`1.4.305`) runs on CPU only (`llvmpipe`). |
| **OpenGL Desktop / Mesa DRI / Wayland / X11** | ❌ Broken / Absent | No X11 or Wayland server; Mesa `panfrost_dri.so` / `libEGL_mesa.so.0` have no `panfrost` kernel device (`renderD128` is `rockchip-drm` display-only); `libMali.so` does not implement desktop OpenGL (`gl4es` is not installed system-wide). |
| **OpenCL 2.0 (`libMali.so` vs `libOpenCL.so.1`)** | ⚠️ Broken via ICD loader | `/etc/OpenCL/vendors/` is empty, so `libOpenCL.so.1` fails with `-1001` (`CL_PLATFORM_NOT_FOUND_KHR`). Direct `dlopen("libMali.so")` (`libOpenCL.so -> libMali.so`) returns `1` OpenCL 2.0 platform. |
| **VPU / Hardware Video Decode (`FFmpeg`)** | ❌ Broken / Incomplete | `/dev/vpu_service` is `root`-only (`600`), `librockchip_mpp.so` is absent, `/dev/video*` V4L2 M2M nodes do not exist, and Debian FFmpeg lacks `rkmpp`. |
| **EGL/GBM directly on `/dev/dri/renderD128`** | 💥 Fatal Kernel Oops | Calling `eglInitialize` on `EGL_PLATFORM_GBM_KHR` (`renderD128`) NULL-dereferences in kernel (`Oops: 96000005`) and hangs the device until hard power-off; always use SDL2 KMSDRM. |
| **Bluetooth** | ❌ Absent | No Bluetooth controller detected (`wlan0` Wi-Fi works via `NetworkManager`/`wpa_supplicant`). |
| **Kernel `4.4.189` syscalls (`ENOSYS`)** | ❌ Missing (with glibc 2.41 header mismatch) | Probed `libc.syscall()` returns `errno 38 (ENOSYS)` for `io_uring_setup` (NR 425), `clone3` (NR 435), `pidfd_open` (NR 434), `statx` (NR 291), and `bpf` (NR 280); `memfd_create` (NR 279) works. Never gate code solely on compile-time `r36s-sysroot` 6.1 headers without runtime `ENOSYS` fallback. |
| **Kernel `4.4.189` cgroups / FS / sysfs** | ⚠️ Incomplete | `/proc/cgroups` only has `cpuset, cpu, cpuacct, devices, freezer, net_cls` (**no `memory`, `io`, `pids`**); **no `PSI`** (`/proc/pressure/*`), **no `overlayfs`**, **no `f2fs`**, **no `binfmt_misc`**, **no `/proc/config.gz`** (`ext2/3/4, squashfs, vfat, exfat, btrfs, xfs, fuse` exist). Scanning `/proc/[0-9]*/fd/*` freezes the device. |

## Optimization State (Passes 7–9.5, Phases 6 / A / B / C & Fastmem)

Full phase-by-phase notes, A/B comparisons, and raw log findings are recorded in [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md).

### Summary of Implemented Optimizations

For the active R36S-only backlog and the full waste audit (PS2-inherited vs. generic runtime choices), see [`worklist.md`](worklist.md) and [`r36s_native_audit.md`](r36s_native_audit.md).
- **Build & Code Layout (Pass 7 + Pass 8A + Pass 8B + Phase 4, [`CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt)):**
  - `PS2X_LINUX_RELEASE=ON` defines `SOTC_RELEASE=1` across `ps2_runtime`, `ps2_iop`, and `overrides/*.cpp`, compiling out diagnostic timers/logging/counts while retaining `SOTC_FRAME_LOG=1` (drop in `hooks` from `1.10 ms` to `0.00 ms`).
  - Cold recompiled batches and non-trust fallback `lean_part[0-9]*.cpp` compile at `-Os`, `vu1rec_*` at `-O2`, and `ps2_runtime`, `overrides/*.cpp`, and `lean_part_t*.cpp` at `-O3`.
  - `-ffunction-sections` + `--section-ordering-file` using [`linux_hot_order.ld`](../game/overrides/linux_hot_order.ld) (`8,919` ordered symbols covering `34.72 MB` across Zones A/B/C), regenerated from R36S `PS2X_PROFILE` telemetry via [`linux_hot_order.py`](../tools/linux_hot_order.py).
  - Portable cross-compile PGO (`-DPS2X_LINUX_PGO=gen` / `<collected_dir>`) emits counters to `/tmp/sotc_pgo/` with `-fprofile-prefix-path` and links `__gcov_dump` (`-Wl,-u,__gcov_dump`). Setting `SOTC_GCOV_DUMP_FRAME` in [`sotc_native.cpp`](../game/overrides/sotc_native.cpp) skips premature `frame % 600 == 0` dumps so profiles capture intro + open-world gameplay across all `213/213` compilation units with zero missing/mismatch warnings.
- **Hot CPU Paths (Pass 8C, [`sotc_decomp.h`](../game/overrides/sotc_decomp.h), [`sotc_native.cpp`](../game/overrides/sotc_native.cpp), [`decomp_isys.cpp`](../game/overrides/decomp_isys.cpp)):**
  - Inline plain-type lookups (`characterClassFast`, `layoutClassPlainOnly`, `layoutClassFast`) remove cross-unit `callGuest` overhead at hot call sites in `decomp_hot43b.cpp`, `decomp_hot46j.cpp`, `decomp_scriptvm3.cpp`, and `decomp_cam126.cpp`.
  - `addGroup` switched to `callNative`; `NativeCount::hit()` and `gsRegCensus` gated under `#if !SOTC_RELEASE`; early return on disabled `feedbackBlur` avoids redundant `syncFrame`/`hookJoin`.
  - `SOTC_CHAR104=1` was tested via A/B benchmark and **rejected** (`SOTC_CHAR104=0` kept): on 4× Cortex-A35 the game thread waited `3.426 ms/frame` on worker synchronization and stalled route progress.
- **Mali-G31 TBDR & Presentation (Pass 9, [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp), [`raylib_linux.cpp`](../PS2Recomp/ps2xRuntime/platform/linux/raylib_linux.cpp)):**
  - Enabled `glFewScenes` (`colorAlt`), `glFewCopies` (`previousIsShown`), `syncDepthStencil` tracking (`shadowMask`/`glowLayer`/`blurOut`), and `glInvalidateFramebuffer` on overwritten targets / swap boundaries under `PS2X_NATIVE_GLES` (non-PGO `render` median `17.30 ms → 14.90 ms`).
  - Measured KMSDRM + ARM `libMali` swap behavior (`[linux] swap interval req=0 setRc=0 actual=1`): `SDL_GL_SetSwapInterval(0)` succeeds (`setRc=0`) while the KMSDRM/libMali flip path keeps `actual=1` (`~16.3–18.5 ms` asynchronous `present`, `wait 0.0`). Added `SOTC_FPS_CAP` (default `60`) using `CLOCK_MONOTONIC` + `clock_nanosleep(..., TIMER_ABSTIME, ...)` in `EndDrawing()`, gated per-frame `fflush` behind `SOTC_RELEASE`, and kept `PS2X_SWAP_INTERVAL=0` default.
  - Added `SOTC_CAM_INVERT_X` (default `1`) in `raylib_linux.cpp` before PS2 pad packing so right-stick right pans right during gameplay (verified live on R36S via virtual `uinput` `ABS_RX` injection + `PS2X_LOG_PAD=1`).
- **GLES 3.2 Submission & Texture-Array Batching (Phase C / PR #42, [`native_renderer_gl.cpp`](../PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp), [`r36s_render_merge_audit.md`](r36s_render_merge_audit.md)):**
  - Opt-in `SOTC_GL_TEXARRAY=1` (`GL_TEXTURE_2D_ARRAY` buckets preserving `GL_REPEAT` and mipmaps), `SOTC_GL_PROMOTE_128=1` (promoting `32..128` PoT textures into the `128x128` bucket with Q8 alpha-weighted resampling, `mipOffset`, and `[minLod, maxLod]` floor clamping), `SOTC_GL_STATIC_VAO=1` (persistent per-mesh/run VAOs), `SOTC_GL_DRAW_RANGE=1` (`glDrawRangeElements` for `mesh.pool < 0` and run copies), and `SOTC_GL_SORT_PROG=1` (program/bucket-first safe opaque sorting in `sortFree()`).
  - Measured on real R36S (`build/linux-aarch64` PGO, Shrine/intro windows): `TEXARRAY=1 + STATIC_VAO=1` cut GL draw calls by `-40.0%` to `-42.6%` and CPU `render` by `-35.7%` to `-39.6%` (`18.62 -> 11.97 ms` heavy peak); adding `PROMOTE_128=1 + DRAW_RANGE=1` cut per-draw cost (`35.3–37.5 -> 30.9–34.1 µs/call`); stacking `SORT87=1 + SORT_OPAQUE=1 + SORT_PROG=1` further reduced Shrine GL draw calls `183 -> 124`, `rsd 117.4 -> 81.2`, `prog 92.4 -> 62.2` (`ALL render p50 9.50 ms`). `SOTC_GL_UBO=1` (`+15 µs/call` on `libmali r13p0`) and `SOTC_GL_NODISCARD=1` (`+3.20..+4.08 ms` / `+1.21 ms` CPU regression despite `-1.86 ms` GPU `present` drop) were evaluated and kept **off (`0`) by default**.
- **Selective LTO, 4 GiB Fastmem, & Selective Upstream Ports (PRs #42–#50, [`CMakeLists.txt`](../PS2Recomp/ps2xRuntime/CMakeLists.txt), [`ps2_memory.cpp`](../PS2Recomp/ps2xRuntime/src/lib/ps2_memory.cpp), [`game/overrides/`](../game/overrides/)):**
  - Opt-in `-DPS2X_LINUX_SELECTIVE_LTO=ON` (`-flto -ffat-lto-objects -fno-plt` on `ps2_runtime`, `ps2_iop`, and `/overrides/*.cpp` while preserving non-LTO `lean_part*` and `linux_hot_order.ld`) and `-DPS2X_LINUX_FASTMEM=ON` (4 GiB `PROT_NONE` `mmap` with `memfd_create` RDRAM/Scratchpad mirrors, 64 KiB circular overhang guards, ARM64 `SA_SIGINFO` trap handler restored after `SDL_Init`, `SOTC_FASTMEM_STAT=1` diagnostics, and native `GS_CSR` overrides for `kernel_sub_001C1538`, `kernel_sub_001C37F0`, and `iosSyncV`).
  - Selectively ported upstream correctness and redesign packages (omitting Vita/ARMv7-only paths such as `V1`/`V2`, `S7`, `S8`, `kDiv`, `kMemcmp`/`A9_MEMEQ`, `normalizeA9`, and `a9cmp`): PR #46 (correctness fixes + `keys122` per-lane scratch), PR #47 (`SOTC_S9RT`, `SOTC_S10DRAW`), PR #48 (`SOTC_S4DISP`, `SOTC_LAYER_MIRROR`, `SOTC_S11RT`), PR #49 (`SOTC_S2COLL` replacing `SOTC_S2WATER`, `SOTC_S3CHARA`, ARMv8 `SOTC_A9SWEEP` subset `kHoist|kCache|kZone`), and PR #50 (`s4disp::reflectionEffects` `0x3DCCCCCCu` PS2 FPU + `NOREFLECT` guard).
  - Verified directly on real R36S hardware + disc: `CHECK=1` passed with **`0` differences** across `1,357` `s2coll` view clips, `12,984` under-end / `20,819` crossing `s2water` probes, `60,001` `s3chara` poses (`2,244,519` bones), `9,456` `s9rt` camera copies, `54,626` `s10draw` cloth records, `83,313` `s11rt` checks (`2,191` unwound), and `78` `s4disp` `reflectionBegin` calls. Deployed stripped `Release+PGO+LTO+Fastmem` binary (`50.34 MB` vs `64.09 MB`; `16` one-time boot fastmem traps, `0` gameplay traps) + enabled all 5 Phase C GLES flags (`TEXARRAY`, `PROMOTE_128`, `STATIC_VAO`, `DRAW_RANGE`, `SORT_PROG`) in `SotC.sh`: Shrine gameplay game thread reached **`36.15–36.42 FPS`** (`27.39–27.55 ms` mean/med at `1967` draws, peak `38.90 FPS`) while host display rate remains constrained by `draw + present` (`[wait143]` `draw 33.14 ms + present 39.48 ms` at `1967` draws under KMSDRM `actual=1` VSync).

### Measured 1:1 Deterministic R36S Progression (`gp 4458..5298`, 15 buckets = 900 frames, `draws 838..1130`)

| Stage | FPS (min / med / avg / max) | Frame med | `game` med | `hooks` med | `render` med | `present` med |
| --- | --- | --- | --- | --- | --- | --- |
| Pass 8A (Release, no PGO) | `23.80 / 25.60 / 25.60 / 26.60` | `38.90 ms` | `38.90 ms` | `0.00 ms` | `16.80 ms` | `18.40 ms` |
| Pass 8B (Release + PGO v1) | `25.50 / 27.00 / 26.98 / 28.00` | `37.00 ms` | `36.90 ms` | `0.00 ms` | `15.90 ms` | `18.50 ms` |
| Pass 8C-1 (Hot inlining, no PGO) | `25.10 / 26.40 / 26.60 / 28.00` | `37.70 ms` | `37.70 ms` | `0.00 ms` | `17.30 ms` | `18.50 ms` |
| Pass 9 (Mali-G31 + Cap, no PGO) | `24.90 / 27.30 / 26.97 / 28.60` | `36.60 ms` | `36.50 ms` | `0.00 ms` | `14.90 ms` | `18.60 ms` |
| Pass 9 + PGO (`0d0f7ab8b766d879fc9d18d0a966166a`) | `27.50 / 29.40 / 29.25 / 30.30` | `33.90 ms` | `33.80 ms` | `0.00 ms` | `15.50 ms` | `18.50 ms` |
| Pass 9.5 + `pgo_pass95` (`8f1377e4b1b40d1e34a546921cd0971a`, `f=4458..5418`, `453..3221 draws`) | `27.30 / 29.20 / 29.90 / 34.70` (`3041..3221` ext: `29.60` med / `31.50` max) | `34.10 ms` | `34.10 ms` (`33.65 ms` ext) | `0.00 ms` | `19.60 ms` (`wait 0.0`) | `16.10 ms` |
| **Phase 6 + `pgo_merged` + `SOTC_MESHOPT=1`** (`063ba18e41672b0dd04ce85bcec62695`, `draws 828..1238`) | **`27.00 / 30.10 / 29.87 / 31.00`** | **`33.10 ms`** | **`33.10 ms`** (`33.37 avg`) | `0.00 ms` | `29.40 ms` (`wait 0.0`) | `14.90 ms` |
| **Phase 6B + `decomp_char122` / GL cutout fix** (`f97edf3dcb9fb0de9a499165c2adcd10`, active Shrine/courtyard `15` samples, `draws 1148..2346`) | **`29.10 / 31.60 / 31.65 / 34.30`** (`f=5118..5358`: `32.00` med / `32.40` avg) | `31.65 ms` | `31.60 ms` | `0.00 ms` | `29.70 ms` (`wait 0.0`) | `14.30 ms` |
| **Phase A (`READ_WATCH=0`, `WRITE_TRACE=0`, `RUNTIME_LOGS=OFF`, `-mtune=cortex-a35 +crypto+crc` + `pgo_phase_a`)** (`f8ee05adb7dfdc287b3f13df0c835f98`, matched `f=4500..5900` `n=1401`, `draws 853..1000`) | **`32.34` avg / `33.00` med** *(vs `A0` baseline `30.38 / 31.06`)* | **`30.30 ms` p50** (`34.20` p90 / `40.80` p99) | **`30.30 ms`** (`30.91 avg`) | `0.00 ms` | `20.30 ms` (`25.65 avg`, `wait 0.0`) | `31.20 ms` (`18.19 avg`) |

Light/menu scenes run locked at **`59.8–60.1 FPS`** (`16.6–16.7 ms`, `wait 0.0`).

### Reproducible R36S Release + PGO Cross-Build

1. Configure PGO-generation build:
   ```sh
   cmake -S PS2Recomp -B build/linux-aarch64-pgogen -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/r36s-toolchain.cmake \
     -DPLATFORM=SDL -DPS2X_ENABLE_FFMPEG=OFF \
     -DPS2X_LINUX_RELEASE=ON -DPS2X_LINUX_PGO=gen
   ninja -C build/linux-aarch64-pgogen ps2EntryRunner
   ```
2. Run on R36S with `SOTC_GCOV_DUMP_FRAME=4200` (and `/tmp/sotc_pgo/` writable) through intro + open-world gameplay, then pull `/tmp/sotc_pgo/*.gcda` into e.g. `build/linux-aarch64/pgo_pass9` (optionally regenerate `game/overrides/linux_hot_order.ld` with `tools/linux_hot_order.py`).
3. Configure and link final Release + PGO binary (`0` `-Wmissing-profile`/`-Wcoverage-mismatch` warnings across all 213 TUs; optionally add `-DPS2X_LINUX_SELECTIVE_LTO=ON -DPS2X_LINUX_FASTMEM=ON`; strip `ps2EntryRunner` and deploy as `/roms/ports/sotc_data/ps2sotc_aarch64_pie_sdl`):
   ```sh
   cmake -S PS2Recomp -B build/linux-aarch64 -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=cmake/r36s-toolchain.cmake \
     -DPLATFORM=SDL -DPS2X_ENABLE_FFMPEG=OFF \
     -DPS2X_LINUX_RELEASE=ON -DPS2X_LINUX_PGO=build/linux-aarch64/pgo_pass9
   ninja -C build/linux-aarch64 ps2EntryRunner
   ```
4. Deploy the launcher files from [`ports/r36s/`](../ports/r36s/) to the R36S (alongside the user-supplied game files in `/roms/ports/sotc_data/`):
   - `ports/r36s/sotc.sh` -> `/roms/ports/sotc.sh` (`chmod +x`)
   - `ports/r36s/control.txt` -> `/roms/ports/sotc_data/`
   - `ports/r36s/gamelist.xml` -> merge the `<game>` entry into `/roms/ports/gamelist.xml`

### Remaining Bottlenecks & Phase 6 Results
- **Phase 6 & 6B visual correctness, cutscene freeze fix, & indexed `meshoptimizer` (`fix/r36s-visuals-and-meshopt`, [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md)):** Fixed `SOTC_NATIVE_NO*` `"0"` parsing and added `"SOTC_NATIVE_NOADDITIVE"` env hook to `dbg_additive` in `native_renderer.cpp`; unified `isSafeOpaqueDraw()` (removing `opacity >= 0.95f`/`Blend::Normal` shortcut) across `sortFree()` and instancing in `native_renderer_gl.cpp`; added indexed `meshoptimizer` (`SOTC_MESHOPT=1`, `VCACHE=0`, `OVERDRAW=0`, `SORT_OPAQUE=0`) + indexed `glMergeDraws` / `glDrawElementsInstanced` + `vao` guard; added `todoUrgent` terrain priority queue and atomic last-complete static packet retention (dropping `f=5058` `held` packets from `1284 (terrain 1212)` to `0 (terrain 0)`); fixed `char122::cloth::dynamics()` `$sp`/`$ra` corruption across `layoutClassFast()` fallback `callGuest()` calls (eliminating the bird-past-Wander cutscene hang) and updated `SotC.sh` with `SOTC_GL_AT98=0`, `SOTC_GL_OPAQUE88=0`, `SOTC_GL_NODISCARD=0`, `SOTC_GL_PSOFT98=0`. Verified via R36S cutscene/gameplay captures (`ab_vis_fix`, `gp_vis`), deterministic 125 s benchmark (`31.60 FPS` med in Shrine/courtyard gameplay), and on-device `ps2x_tests` (`93 Passed / 57 Failed` across `150` cases, matching `AGENTS.md`).
- **Pass 9.5 updates (`feat/r36s-pass95-cpu`, [`SOTC_OPTIMIZATION_LOG.md`](../SOTC_OPTIMIZATION_LOG.md)):** Added `SOTC_GLEND_ASYNC` (default `1`), native `spriteDrawSprite` (`game/overrides/decomp_sprite.cpp`, `1196` natives active), direct `callNative`/`callDirect` chaining (`execCharaGeometry`, `GetMotionMatrix`, `CalcNodeInterp`, `UpdateLayoutClassGeometry`, `GetClothDynamics`, `invMatrix`, `isysGroupFor*`, `signalSema`, `modelDispHook`), `!shadows` early-out in `solidDisp`, `coll125::validate138` dedup, rebuilt with fresh R36S `pgo_pass95` (`214` `.gcda` TUs, `0` warnings) and regenerated `linux_hot_order.ld` (`8,883` symbols, `35.04 MB`).
- **Game thread CPU (`33.10 ms` median, `29.87 FPS` avg):** Still the sole frame limiter on Cortex-A35 (`wait 0.0`). Fresh R36S `PS2X_PROFILE` (`gp_b2_game.prof`, `26,375` samples / 26 s, EE busy 99.7%) shows a flat distribution across `ClipCollisionDirectCallBack` (`2.19%`), `ClipCollisionFinderCallBack` (`1.97%`), `GetCollisionDBCallBack` (`1.95%`), `DispDispObjGroupModel` (`1.75%`), `manager_sub_0111C810` (`1.72%`), `ExecCharaGeometry` (`1.69%`), `manager_sub_010F3A08` (`1.63%`), `gamecore_sub_013CFBE0` (`1.61%`), `manager_sub_010F3CF0` (`1.35%`), `ClipCollisionFinder` (`1.25%`), `GetMotionMatrix` (`1.14%`), `gamecore_sub_013E3840` (`1.08%`), and `GetLayoutClass` (`1.08%`).
- **KMSDRM 60 Hz page-flip (`present 16.10–18.50 ms` med):** runs asynchronously (`wait 0.0`) so it does not stall the game thread below `16.67 ms`, though `actual=1` caps single-flip cadence to 60 Hz scanout.

## Runtime and profiling
Separate CPU emulation/recompilation, VU work, renderer submission, GPU execution, synchronization, and presentation. Aggregate CPU utilization can hide a saturated main thread. Validate GPU telemetry before drawing conclusions from a reported 0%.

For the current moving-camera slowdown, compare stationary and moving camera captures in the same scene and configuration. Record per-thread CPU, hot functions/guest PCs, frame times, draw calls, and GPU timings where available.

## Reverse engineering
Prefer local IDA Pro via IDA MCP, with REA MCP configured to use its IDA provider. Ghidra is fallback only. Confirm module format, MIPS/R5900 mode, endianness, relocations, and runtime address mapping. See [reverse_engineering_tools.md](reverse_engineering_tools.md).

## Device safety
- Avoid broad enumeration of `/proc/[0-9]*/fd/*`; it has previously frozen the device.
- Do not capture with `ffmpeg -f fbdev` or `ffmpeg -f kmsgrab` without an isolated test; these can interfere with KMS.
- Do not use broad `pkill` commands. Only use a reviewed atomic recovery script that restores EmulationStation.
- Keep temporary writes small and under `/tmp` where possible.
- Do not alter firmware/system-wide files or write outside the game directory and `/tmp` without explicit authorization.
- Prefer reviewing a script locally and then sending it over SSH with `bash -s`.
