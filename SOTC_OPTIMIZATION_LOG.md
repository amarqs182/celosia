# SotC R36S Autonomous Optimization Log

Hardware target: R36S (`rockchip,rk3326-odroidgo3-linux`, 4× Cortex-A35 @ 1.512 GHz, Mali-G31 MP2 @ 600 MHz via ARM libmali GLES 3.2, 640×480 KMSDRM, 895 MiB RAM, Debian 13 trixie aarch64).

---

## Phase 0 — Initial Audit

- **Git baseline:** `main` at `d175254` (`PR #29` merged), clean working tree.
- **Installed binary (`/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl`):** MD5 `d54c4407a4a7195e042c88cb2f7e5987`, 77,784,632 bytes (stripped).
- **Pass 7 configuration verified:**
  - `SOTC_EE_TRUST=1` active (`1846 lean functions in place (13032 table slots); second set (SOTC_EE_TRUST) built`, `[lean] frame 2: lean copies, trusted floats (SOTC_EE_TRUST)`).
  - Cold Unity TUs + fallback `lean_part[0-9]*.cpp` at `-Os`; `ps2_runtime`, `overrides/*.cpp`, `lean_part_t*` at `-O3`; `vu1rec_*` at `-O2`; `-ffunction-sections` + `--section-ordering-file=game/overrides/linux_hot_order.ld` (`8657` ordered symbols); `PS2X_ENABLE_AGRESSIVE_LOGS=OFF`, `PS2X_ENABLE_IOP_RPC_TRACE=OFF`, `PS2X_LINUX_RELEASE=OFF`.
- **R36S automation & tooling:**
  - SSH/SCP operational (`ark@192.168.2.121`), `/dev/uinput` present, `python3-evdev` installed, `memcards/Mcd001.ps2` present (8.65 MB save), `PS2X_INPUT_SCRIPT` supported in `ps2_pad.cpp`.
  - CPU probe on real R36S (`SOTC_SUBSYS_PROF=1`): `timer 0.241 us/call`, `L1 16K 2.8 ns`, `RAM 16M 195.1 ns`, `hot call 32.7 ns`, `cold over 151 functions 4021.5 ns`.

---

## Phase 1 — Validate Pass 7 (Real Gameplay Comparison)

Compared the real user gameplay session on Pass 6 (`/tmp/sotc_pass6_play.log`, 356 buckets, `SOTC_EE_TRUST` not built) against the user's real gameplay session on Pass 7 (`/tmp/sotc.log` Nov 14 11:27 -> `scratch/sotc_pass7_play.log`, 1313 buckets):

- **Pass 6 gameplay (`f >= 2800`, `draws >= 300`, median `1582` draws):**
  - FPS: median `26.80`, avg `27.07` (min `22.00`, max `60.10`)
  - Frame: median `37.20 ms`, avg `37.06 ms`
  - `game`: median `36.00 ms`, avg `35.83 ms` (31.2–38.8 ms typical)
  - `hooks`: median `1.10 ms`
  - `render`: median `17.90 ms`, avg `19.18 ms`
  - `present`: median `17.00 ms`, avg `16.97 ms`
- **Pass 7 gameplay (`f >= 2800`, `draws >= 300`, median `1292` draws):**
  - FPS: median `35.50`, avg `38.42` (min `23.90`, max `60.70`) — **+32.5% median FPS**
  - Frame: median `28.10 ms`, avg `27.23 ms`
  - `game`: median `27.00 ms`, avg `26.14 ms` — **-9.0 ms median (-25% CPU time)**
  - `hooks`: median `1.00 ms`
  - `render`: median `21.10 ms`, avg `20.18 ms` (max `34.10 ms`)
  - `present`: median `17.20 ms`, avg `15.81 ms`

**Phase 1 conclusion:**
- `game` thread (`27.0 ms` median, `25.7–39.9 ms` in active gameplay) remains above the `20–25 ms` threshold, and `render` (`21.1 ms` median) + `present` (`17.2 ms` median) also exceed the `16.67 ms` (60 FPS) budget.
- **Scripted Deterministic Pass 7 Baseline (120s route, open-world Save Shrine `draws=1482`, `f>=2700`, 20 buckets = 1200 frames, `SOTC_GL_PHASE_LOG=1`, `SOTC_GL_DRAW_LOG=1`):**
  - FPS: min `24.20`, med `25.95`, avg `25.92`, max `27.00`
  - Frame: med `38.40 ms`, avg `38.49 ms`
  - `game`: med `37.25 ms`, avg `37.31 ms`
  - `hooks`: med `1.10 ms`
  - `render`: med `18.10 ms`, avg `18.44 ms` (`plan 4.01-4.35 ms`, `passes 10.66-11.99 ms`, `draw calls 197-281` / `6.09-7.05 ms`, `rest 2.24-2.28 ms`)
  - `present`: med `14.60 ms`, avg `14.76 ms`
- **Fresh Pass 7 R36S `PS2X_PROFILE` (43,975 samples, 44s, EE busy 93.8%):**
  1. `ClipCollisionDirectCallBack @0112d590`: `3.23%`
  2. `GetLayoutClass @010f1a20`: `2.26%`
  3. `manager_sub_010F3A08 @010f3a08`: `2.11%`
  4. `DispDispObjGroupModel @010f52c8`: `2.04%`
  5. `ExecCharaGeometry @010bf8f0`: `1.77%`
  6. `ClipCollisionFinderCallBack @01125cc0`: `1.53%`
  7. `gsReg @001c5b00`: `1.46%`
  8. `gamecore_sub_013CFBE0 @013cfbe0`: `1.42%`
  9. `GetCollisionDBCallBack @01124c88`: `1.20%`
  10. `spriteDrawSprite @010a32b8`: `1.04%`
- **Decision:** Proceed through **Phase 2 (Pass 8: Release build -> Real R36S PGO -> Hot functions / `SOTC_CHAR104`)** and then **Phase 3 (Pass 9: Present pipeline, Mali-G31 render, VSync-off + 60 FPS cap, and Horizontal Camera fix)**.


---

## Phase 2A — Release Build (`PS2X_LINUX_RELEASE=ON` → `SOTC_RELEASE=1`)

- **Configuration:** `-DPS2X_LINUX_RELEASE=ON` in `PS2Recomp/ps2xRuntime/CMakeLists.txt` (`-DSOTC_RELEASE=1`), stripping `SOTC_GL_PHASE_LOG`, `SOTC_GL_DRAW_LOG`, and timing/diag hook overhead while keeping `SOTC_FRAME_LOG=1` active on R36S (`523` frame lines recorded cleanly).
- **Binary:** `ps2sotc_aarch64_pie_sdl` (`77,391,256` bytes stripped, MD5 `5e3c915fb5f5d230b01bd9a15e880f92`).
- **Deterministic 125s R36S Benchmark (`bench_pass8a_det125s.log` vs `bench_pass7_120s.log`, matching Save-Shrine `f>=4458` / `draws=1209..1332` vs `1482`):**
  - Wait-free hooks: `hooks` dropped **`1.10 ms → 0.00 ms`**.
  - On the exact pure-frame deterministic route (`f=4458..5298`, 15 buckets = 900 frames, `draws=853..1129`, `bench_pass8a_125s.log`):
    - FPS: min `23.80`, med **`25.60`**, avg `25.60`, max `26.60`
    - Frame: med **`38.90 ms`**, avg `38.97 ms`
    - `game`: med **`38.90 ms`**, avg `38.93 ms` (`hooks 0.00 ms`)
    - `render`: med **`16.80 ms`**, avg `17.63 ms`
    - `present`: med **`18.40 ms`**, avg `18.34 ms`
- **Status:** Kept (`feat/r36s-pass8-pass9`).

---

## Phase 2B — Real R36S PGO + `linux_hot_order.ld` Regeneration

- **Root cause fixed for R36S PGO-gen:** `PS2Recomp/ps2xRuntime/CMakeLists.txt` (`PS2X_LINUX_PGO_GEN`) now passes `-Wl,-u,__gcov_dump` so strong `__gcov_dump` is linked from `libgcov.a` and triggered cleanly at `SOTC_GCOV_DUMP_FRAME=5050` via `GCOV_PREFIX=/tmp/sotc_pgo GCOV_PREFIX_STRIP=1` (`213/213` `.gcda` files collected on real R36S gameplay, `933K` archive).
- **Build configuration:** Built with `-DPS2X_LINUX_PGO=ON -DPS2X_LINUX_PGO_USE_DIR=.../pgo_pass8b` with `-Wmissing-profile` and `-Wcoverage-mismatch` active (`0` missing/mismatched profile warnings across all 213 TUs). Regenerated `game/overrides/linux_hot_order.ld` from `guest_pass8a_det125s.prof` (`8,866` ordered symbols, `34.61 MB`).
- **Binary:** `ps2sotc_aarch64_pie_sdl` (`77,718,984` bytes stripped, MD5 `3b47a44712f8e22f881dcf4f1ea15b88`).
- **1:1 Deterministic R36S Benchmark (`bench_pass8b_125s.log` vs `bench_pass8a_125s.log`, identical `f=4458..5298`, 15 buckets = 900 frames, `draws=853..1130`):**
  - FPS: min `25.50` (+1.70), med **`27.00`** (**+1.40 FPS / +5.5%**), avg `26.98` (+1.38), max `28.00` (+1.40)
  - Frame: med **`37.00 ms`** (**-1.90 ms / -4.9%**), avg `36.99 ms`
  - `game`: med **`36.90 ms`** (**-2.00 ms / -5.1%**), avg `36.97 ms`
  - `render`: med **`15.90 ms`** (-0.90 ms), avg `17.19 ms` (-0.44 ms)
  - `present`: med **`18.50 ms`**, avg `18.50 ms`
  - Boot-to-GameCore: `11.359 s → 10.978 s`
- **Status:** Kept.


---

## Phase 2C — Hot Function Optimization & `SOTC_CHAR104` A/B Evaluation

- **Hot-path inline specialization (kept):**
  - Added `sotc_decomp::characterClassFast`, `sotc_decomp::layoutClassPlainOnly`, and `sotc_decomp::layoutClassFast` in `game/overrides/sotc_decomp.h` (matching `decomp_walk.cpp`'s plain-type table while preserving worker-thread checks and fallback guest calls), eliminating cross-unit `callGuest` overhead in hot call sites inside `decomp_hot43b.cpp`, `decomp_hot46j.cpp`, `decomp_scriptvm3.cpp`, and `decomp_cam126.cpp`.
  - Switched `decomp_isys.cpp` (`addGroup`) to `sotc_decomp::callNative`, guarded `NativeCount::hit()` registration with `#if !SOTC_RELEASE`, and gated `gsRegCensus` / `SOTC_LOG_GSREG` under `#if !SOTC_RELEASE` in `game/overrides/sotc_native.cpp` with an early return when `!native_renderer::tuning().feedback` (avoiding `syncFrame`/`hookJoin` on disabled `feedbackBlur` calls).
  - Non-PGO Pass 8C-1 A/B benchmark (`bench_pass8c1_125s.log` vs `bench_pass8a_125s.log`, `gp 4458..5298`, 15 buckets): FPS med **`26.40`** (+0.80 FPS vs `25.60`), `game` med **`37.70 ms`** (-1.20 ms vs `38.90 ms`).
- **`SOTC_CHAR104=1` A/B evaluation (rejected, kept `SOTC_CHAR104=0`):**
  - Tested `SOTC_CHAR104=1` on R36S (`bench_pass9_char104_125s.log`): stalled progress (`f range: 78..4278` vs `5298` baseline) and logged `[char104] frame 4218: 12.32 characters in 4.21 batches a frame on the worker, worker 5.267 ms a frame ..., the game thread waited 3.426 ms a frame`. Worker-queue waiting outweighed parallelism on 4× Cortex-A35, so `SOTC_CHAR104=0` remains the default in `SotC.sh`.

---

## Phase 3 — Pass 9: Mali-G31 TBDR, Presentation Pacing & Camera Control

- **Present pipeline & KMSDRM findings (`PS2Recomp/ps2xRuntime/platform/linux/raylib_linux.cpp`):**
  - Verified actual EGL/SDL swap behavior with runtime diagnostic `[linux] swap interval req=0 setRc=0 actual=1`: although `PS2X_SWAP_INTERVAL=0` requests swap interval `0` via `SDL_GL_SetSwapInterval(0)` (and `SetSwapInterval` succeeds with `setRc=0`), SDL's KMSDRM/GBM backend + ARM `libMali` enforces `actual=1` (`~18.5 ms` asynchronous `present` on 60 Hz scanout, `wait 0.0`).
  - Implemented `SOTC_FPS_CAP` (default `60`, `0` uncapped) using `clock_gettime(CLOCK_MONOTONIC)` / `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` in `EndDrawing()`, locking light/menu scenes cleanly to `16.6–16.7 ms` (`59.8–60.1 FPS`, `wait 0.0`) without busy-waiting or queue runaway, and gated per-frame `fflush` behind `SOTC_RELEASE`.
  - Added `glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, {GL_DEPTH, GL_STENCIL})` before `SDL_GL_SwapWindow`.
  - **Controlled Phase 3C A/B benchmarks on Final Pass 9+PGO (`gp 4458..5298`, 15 buckets = 900 frames):**
    - `PS2X_SWAP_INTERVAL=1, SOTC_FPS_CAP=0` (`bench_pass9_final_swap1_cap0_125s.log`, `actual=1`, `draws 838..1129` med `932`): FPS `27.00 / 29.10 / 28.81 / 30.10`, `game` med `34.30 ms`, `present` med `18.30 ms`, light frames `59.8–60.0 FPS`.
    - `PS2X_SWAP_INTERVAL=0, SOTC_FPS_CAP=0` (`bench_pass9_final_swap0_cap0_125s.log`, `actual=1`, `draws 454..3091` med `1134`): FPS `24.70 / 29.10 / 28.85 / 30.50`, `game` med `34.30 ms`, `present` med `16.30 ms`, light frames `59.9–60.0 FPS`.
    - `PS2X_SWAP_INTERVAL=0, SOTC_FPS_CAP=60` (**kept default**, `bench_pass9_final_pgo_125s.log`, `actual=1`, `draws 852..1128` med `933`): FPS **`27.50 / 29.40 / 29.25 / 30.30`**, `game` med **`33.80 ms`**, `present` med `18.50 ms` (`wait 0.0`), light frames `59.8–60.1 FPS` — zero penalty below 60 FPS plus portable `CLOCK_MONOTONIC` pacing if KMSDRM unlocks swap interval `0` on another kernel/mesa driver.
- **Mali-G31 TBDR optimizations (`PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp`):**
  - Enabled `glFewScenes` (`colorAlt`) and `glFewCopies` (`previousIsShown`) under `PS2X_NATIVE_GLES`, eliminating redundant full-frame scene copies.
  - Added `invalidateTarget()` (`glInvalidateFramebuffer` when `glDepthKeep` is enabled) on quad color overwrites (`!corners && blend == QuadBlend::None && !target.depth`), `reflection` / `background` depth+stencil attachments, and final `color` depth+stencil.
  - Added `syncDepthStencil()` for `shadowMask`, `glowLayer`, and `blurOut` to track and avoid redundant FBO depth/stencil rebinds.
- **Camera right-stick horizontal inversion (`SOTC_CAM_INVERT_X=1`):**
  - Inverted `SDL_CONTROLLER_AXIS_RIGHTX` once before PS2 pad packing in `raylib_linux.cpp` when `SOTC_CAM_INVERT_X != 0` (default `1`), leaving menus and vertical `ry` untouched; radial deadzone (`PS2X_STICK_DEADZONE=0.18`) and curve (`PS2X_STICK_CURVE=1.25`) in `ps2_pad.cpp::shape()` remain symmetric.
  - Verified live on R36S hardware via virtual `uinput` `GO-Super Gamepad` `ABS_RX` step injection (`PS2X_LOG_PAD=1`): `ABS_RX = +1800` (right) → `[pad] raw L +0.000 +0.000 R -1.000 +0.000`; `ABS_RX = 0` → `R +0.000 +0.000`; `ABS_RX = -900` (half-left) → `R +0.503 +0.000`; `ABS_RX = -1800` (full-left) → `R +1.000 +0.000`; `ABS_RX = 0` → `R +0.000 +0.000`.
- **Visual & Pass 9 non-PGO validation on R36S:**
  - Verified clean PNG captures converted from `SOTC_GL_SHOTS=25` PPM dumps (`shot_1500`, `shot_2805`, `shot_3285`, `shot_3775`, `shot_3850`, `shot_3925`) across intro cliffs, Shrine of Worship interior, Options menu, and Forbidden Lands plains with zero rendering corruption.
  - Clean Pass 9 non-PGO benchmark (`bench_pass9_s0_125s.log`, `gp 4458..5298`, 15 buckets): `render` med dropped **`17.30 ms → 14.90 ms`** (-2.40 ms / -13.9%), FPS med rose **`26.40 → 27.30`**, `game` med `36.50 ms`, `present` med `18.60 ms` (`wait 0.0`).

---

## Phase 4 — Final Reproducible Release + PGO Build & R36S Validation

- **Portable Linux/aarch64 PGO pipeline (`PS2Recomp/ps2xRuntime/CMakeLists.txt`, `game/overrides/sotc_native.cpp`):**
  - Fixed Windows-host cross-compiled `-fprofile-generate` path mangling on Linux R36S (`D:\celosia_upstream\build\...` + `GCOV_PREFIX` failed in `libgcov` with `Cannot create directory` and `free(): invalid pointer`) by configuring `-fprofile-generate=/tmp/sotc_pgo/` paired with `-fprofile-prefix-path=${PS2X_LINUX_PGO_PREFIX}`.
  - Fixed premature `frame % 600 == 0` `__gcov_dump()` in `sotc_native.cpp` (which set `libgcov`'s `dump_done` flag at intro frame 600 and turned later `SOTC_GCOV_DUMP_FRAME` calls into no-ops) so timed `SOTC_GCOV_DUMP_FRAME=4200` captures cumulative intro + open-world/Shrine (`draws=907..3097`) gameplay counters (`213/213` `.gcda` files).
  - Regenerated `game/overrides/linux_hot_order.ld` using `tools/linux_hot_order.py` against `scratch/pass9.prof` (`8,919` ordered symbols, `34.72 MB`: `7.59 MB` Zone A critical + `20.49 MB` Zone B warm + `6.64 MB` Zone C `lift_d1`).
- **Final Release+PGO build configuration:**
  - `cmake -S PS2Recomp -B build/linux-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/r36s-toolchain.cmake -DPLATFORM=SDL -DPS2X_ENABLE_FFMPEG=OFF -DPS2X_LINUX_RELEASE=ON -DPS2X_LINUX_PGO=build/linux-aarch64/pgo_pass9`
  - `0` `-Wmissing-profile` / `-Wcoverage-mismatch` warnings across all 213 TUs.
  - Stripped binary `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl`: `77,849,680` bytes, MD5 `0d0f7ab8b766d879fc9d18d0a966166a`.
  - Updated `/roms/ports/.SotC_Celosia/SotC.sh`: `PS2X_SWAP_INTERVAL=0`, `SOTC_FPS_CAP=60`, `SOTC_CAM_INVERT_X=1`, `SOTC_CHAR104=0`, `SOTC_GL_FEW_COPIES=1`, `SOTC_GL_FEW_SCENES=1`, `SOTC_GL_OPAQUE88=1`, `SOTC_GL_MERGE_STATS=0`, `SOTC_GL_PHASE_LOG=0`, `SOTC_GL_DRAW_LOG=0`. `Mcd001.ps2` (`8.3M`) preserved.

### 1:1 Deterministic R36S Progression (`gp 4458..5298`, 15 buckets = 900 frames, `draws 838..1130`)

| Stage | FPS (min / med / avg / max) | Frame med | `game` med | `hooks` med | `render` med | `present` med |
| --- | --- | --- | --- | --- | --- | --- |
| **Pass 8A** (Release, no PGO) | `23.80 / 25.60 / 25.60 / 26.60` | `38.90 ms` | `38.90 ms` | `0.00 ms` | `16.80 ms` | `18.40 ms` |
| **Pass 8B** (Release + PGO v1) | `25.50 / 27.00 / 26.98 / 28.00` | `37.00 ms` | `36.90 ms` | `0.00 ms` | `15.90 ms` | `18.50 ms` |
| **Pass 8C-1** (Hot inlining, no PGO) | `25.10 / 26.40 / 26.60 / 28.00` | `37.70 ms` | `37.70 ms` | `0.00 ms` | `17.30 ms` | `18.50 ms` |
| **Pass 9** (Mali-G31 + Cap, no PGO) | `24.90 / 27.30 / 26.97 / 28.60` | `36.60 ms` | `36.50 ms` | `0.00 ms` | `14.90 ms` | `18.60 ms` |
| **Final Pass 9 + PGO** (`bench_pass9_final_pgo_125s.log`) | **`27.50 / 29.40 / 29.25 / 30.30`** | **`33.90 ms`** | **`33.80 ms`** | `0.00 ms` | `15.50 ms` | `18.50 ms` |

- **Net measured gain across Phases 2–4 (identical 125s deterministic route):** **`25.60 → 29.40 FPS`** (**+3.80 FPS / +14.8% median**, **`23.80 → 27.50` minimum FPS**), **`game` `38.90 → 33.80 ms`** (**-5.10 ms / -13.1%**), light/menu scenes locked at **`59.8–60.1 FPS`**, and `SOTC_RELEASE=1` + `PS2X_SWAP_INTERVAL=0` + `SOTC_FPS_CAP=60` + `SOTC_CAM_INVERT_X=1` verified active on real R36S hardware.
- **Remaining bottlenecks & next-frontier constraints:**
  1. **Game thread CPU (`33.80 ms` median in Shrine):** still the primary frame-rate limiter on 4× Cortex-A35 @ 1.512 GHz (~55% of samples remain in recompiled MIPS/game logic, top remaining native hotspots include `ClipCollisionDirectCallBack`, `GetLayoutClass` non-plain paths, `manager_sub_010F3A08`, `DispDispObjGroupModel`, `ExecCharaGeometry`, `ClipCollisionFinderCallBack`). Pushing significantly past ~30 FPS in heavy scenes requires further deep MIPS-to-C++ decompilation or algorithmic collision/culling reduction.
  2. **Mali-G31 + KMSDRM scanout (`render` `15.50 ms` med / `max 25.60 ms`, `present` `18.50 ms` med):** runs asynchronously (`wait 0.0`) so it does not stall `game`, though `SDL_GL_SetSwapInterval(0)` is overridden to `actual=1` by the RK3326 KMSDRM/libmali flip path (`18.5 ms`), capping peak single-flip cadence to the 60 Hz display scanout.
