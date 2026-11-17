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

- **Root cause fixed for R36S PGO-gen:** `PS2Recomp/ps2xRuntime/CMakeLists.txt` (`PS2X_LINUX_PGO=gen`) now passes `-Wl,-u,__gcov_dump` so strong `__gcov_dump` is linked from `libgcov.a` and triggered cleanly at `SOTC_GCOV_DUMP_FRAME=5050` via `/tmp/sotc_pgo/` + `-fprofile-prefix-path` (`213/213` `.gcda` files collected on real R36S gameplay, `933K` archive).
- **Build configuration:** Built with `-DPS2X_LINUX_PGO=.../pgo_pass8b` with `-Wmissing-profile` and `-Wcoverage-mismatch` active (`0` missing/mismatched profile warnings across all 213 TUs). Regenerated `game/overrides/linux_hot_order.ld` from `guest_pass8a_det125s.prof` (`8,866` ordered symbols, `34.61 MB`).
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
  1. **Game thread CPU (`33.80 ms` median in Shrine):** still the primary limiter on 4× Cortex-A35 @ 1.512 GHz (~55% of samples remain in recompiled MIPS/game logic, top remaining native hotspots include `ClipCollisionDirectCallBack`, `GetLayoutClass` non-plain paths, `manager_sub_010F3A08`, `DispDispObjGroupModel`, `ExecCharaGeometry`, `ClipCollisionFinderCallBack`). Pushing significantly past ~30 FPS in heavy scenes requires further deep MIPS-to-C++ decompilation or algorithmic collision/culling reduction.
  2. **Mali-G31 + KMSDRM scanout (`render` `15.50 ms` med / `max 25.60 ms`, `present` `18.50 ms` med):** runs asynchronously (`wait 0.0`) so it does not stall `game`, though `SDL_GL_SetSwapInterval(0)` is overridden to `actual=1` by the RK3326 KMSDRM/libmali flip path (`18.5 ms`), capping peak single-flip cadence to the 60 Hz display scanout.

---

## Phase 5 — Pass 9.5: Direct Native Chaining, `spriteDrawSprite`, Collision/Pose Cleanups & Fresh PGO (`pgo_pass95`)

Executed after Pass 9 + PGO showed `game` (`33.80 ms` med) still gating 3D Shrine frame rate on Cortex-A35 (`wait 0.0`).

- **Implemented optimizations across Batches 1–4 (`feat/r36s-pass95-cpu`):**
  - **Batch 1 (`SOTC_GLEND_ASYNC` & sync/fallback bypasses):** Added `SOTC_GLEND_ASYNC` (default `1`, async `gsRegEnd` / end-of-frame hook path) in `game/overrides/sotc_native.cpp`; bypassed table-dispatch `callGuest` overhead when direct native targets apply; tuned `PS2Recomp/ps2xRuntime/CMakeLists.txt`.
  - **Batch 2 (`decomp_sprite.cpp` — `spriteDrawSprite`):** Added native `spriteDrawSprite` (`game/overrides/decomp_sprite.cpp`, raising `[sotc:decomp]` from `1195` to `1196` functions), verified `+0.40 FPS` over `SOTC_DECOMP_SKIP=spriteDrawSprite` (`29.20` vs `28.80 FPS`) on the deterministic route.
  - **Off-by-default switch audits (rejected):** A/B tested `SOTC_FIND84` and `SOTC_TEXR128` on real R36S hardware; both measured slower/non-beneficial and remain off by default.
  - **Batch 3 (Direct native chaining & `coll125` micro-optimizations):**
    - Added native forward declarations and direct C++ call wiring (`sotc_decomp::callNative` / `callDirect` / `g.callNative`) in `game/overrides/sotc_decomp.h`, `decomp_scriptvm3.cpp` (`execCharaGeometry`), `decomp_charageo.cpp` (`rest81::rootDynamics`, `CalcNodeInterp`, `GetCharaRootDynamicsAfter`, `GetMotionMatrix`, `UpdateLayoutClassGeometry`, `childNodes`, `GetClothDynamics`), `decomp_collision.cpp` (`invMatrix`, `isysGroupForFirst`, `isysGroupForNext`, `isysGroupForExit`), `decomp_isys.cpp` (`signalSema`), and `decomp_walk.cpp` (`sotc_native::modelDispHook`, plus early `!native_renderer::tuning().shadows` short-circuit in `solidDisp` before two guest RAM reads).
    - `game/overrides/coll125.h` (`validate138`): removed redundant same-iteration `prefetch138` (keeping `pf141::noteGuest` and lookahead `k+1` prefetch in `run()`) and removed the redundant second `layoutClass` lookup (keeping the `g.u8(m.w0 + 8u) == m.type` check).
  - **Batch 4 (`game/overrides/decomp_skinpal.cpp`):** Wired direct `sotc_decomp::callNative(..., &sotc_native::modelDispHook, ...)` when `dispTarget == 0x01097118u`.
- **Fresh R36S `pgo_pass95` & `linux_hot_order.ld` Regeneration:**
  - Built `PS2X_LINUX_PGO=gen` (stripped upload `99,120,976` bytes, MD5 `911b04d4e330cc091a082889d560a275`) and ran real Save-Shrine gameplay on R36S with `SOTC_GCOV_DUMP_FRAME=4900` (`[gcov] frame 4900: __gcov_dump() complete`, `draws=1208..1255`), collecting `214/214` `.gcda` files (`6.0M`).
  - Rebuilt `build/linux-aarch64` with `PS2X_LINUX_PGO=build/linux-aarch64/pgo_pass95` with **zero** missing-profile / coverage-mismatch warnings across all 214 PGO units.
  - Regenerated `game/overrides/linux_hot_order.ld` via `tools/linux_hot_order.py` against fresh R36S gameplay profile `gp_b2_game.prof` (`8,883` ordered symbols, `35.04 MB`: `7.70 MB` Zone A critical + `20.68 MB` Zone B warm + `6.65 MB` Zone C `lift_d1`).
- **Final Pass 9.5 + PGO Binary:** `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl` (`78,177,360` bytes stripped, MD5 `8f1377e4b1b40d1e34a546921cd0971a`).

### Pass 9.5 Deterministic R36S Benchmarks (125s Save-Shrine Route, `r36s_run_bench.py`)

| Stage | Window / Scene | `draws` | FPS (min / med / avg / max) | `game` med | `render` med |
| --- | --- | --- | --- | --- | --- |
| **Pass 9 + PGO** | `f=4458..4818` / `4458..5418` (Shrine) | `852..1128` | `27.50 / 29.30 / 29.04 / 30.30` (`4458..5418`: `29.40 / 29.26`) | `34.00 ms` (`33.80 ms`) | `16.80 ms` |
| **Pass 9.5 B1** | `f=4458..4818` / `4458..5418` (Shrine) | `790..1129` | `27.60 / 29.00 / 29.04 / 30.50` (`4458..5418`: `29.70 / 29.54`) | `34.30 ms` (`33.50 ms`) | `16.50 ms` (`14.50 ms`) |
| **Pass 9.5 B2** (`+spriteDrawSprite`) | `f=4458..4818` / `4458..5418` (Shrine) | `790..1130` | `27.40 / 28.90 / 28.84 / 30.40` (`4458..5418`: `29.20 / 29.18`) | `34.50 ms` (`34.10 ms`) | `13.90 ms` (`14.50 ms`) |
| **Pass 9.5 B3** (clean, stale PGO) | `f=3378..4278` (Shrine before early pause) | `932..933` | `29.15` med (`16` buckets) | `34.15 ms` | — |
| **Final Pass 9.5 + `pgo_pass95`** (`8f1377e4`) | `f=4458..4818` (matched Shrine draws) | `775..1146` | `28.20 / 29.20 / 29.17 / 30.70` | `34.10 ms` | `18.70 ms` |
| **Final Pass 9.5 + `pgo_pass95`** (`8f1377e4`) | `f=4458..5418` (Shrine exit → Forbidden Lands) | `453..3221` | `27.30 / 29.20 / 29.90 / 34.70` | `34.10 ms` | `19.60 ms` |
| **Final Pass 9.5 + `pgo_pass95`** (`8f1377e4`) | `f=5118..5418` (exterior plain) | **`3041..3221`** | **`27.50 / 29.60 / 29.68 / 31.50`** | **`33.65 ms`** | `28.10 ms` (`wait 0.0`) |

- **Pass 9.5 Evaluation & Conclusion:**
  - Micro-optimizing call dispatch (`callNative`/`callDirect`), `coll125` validation, and `spriteDrawSprite` lifted peak/avg throughput across transitions and held **`29.60 FPS` med (`33.65 ms` game, `wait 0.0`)** even when Wander exited the Shrine into **3,041–3,221 draws** outdoors, while steady interior Shrine gameplay remains in the **`29.0–30.1 FPS` (`~33.1–34.1 ms` game)** band (within run-to-run variance under the `ondemand` SSH governor).
  - **Fresh gameplay `PS2X_PROFILE` (`gp_b2_game.prof`, `26,375` samples / 26 s, EE busy 99.7%):** confirms that single-function micro-shaving has hit diminishing returns (~1–2% per native) because CPU time is spread flatly across collision/geometry traversal and remaining recompiled logic: `ClipCollisionDirectCallBack` (`2.19%`), `ClipCollisionFinderCallBack` (`1.97%`), `GetCollisionDBCallBack` (`1.95%`), `DispDispObjGroupModel` (`1.75%`), `manager_sub_0111C810` (`1.72%`), `ExecCharaGeometry` (`1.69%`), `manager_sub_010F3A08` (`1.63%`), `gamecore_sub_013CFBE0` (`1.61%`), `manager_sub_010F3CF0` (`1.35%`), `ClipCollisionFinder` (`1.25%`), `GetMotionMatrix` (`1.14%`), `gamecore_sub_013E3840` (`1.08%`), and `GetLayoutClass` (`1.08%`).
  - **Next optimization frontier:** Meaningful gains beyond ~30 FPS require either algorithmic scene/collision work reduction (broadphase/skip logic for distant collision & object draws) or full C++ system-level rewrites of the remaining recompiled hot subsystems rather than incremental per-call micro-optimizations.

## Phase 6 — Visual Correctness, Opaque Classification, Indexed `meshoptimizer`, & Atomic Async Terrain (`fix/r36s-visuals-and-meshopt`, 2026-11-15)

- **Root causes fixed:**
  - `SOTC_NATIVE_NO*` boolean parsing (`PS2Recomp/ps2xRuntime/src/lib/native/native_renderer.cpp`): `presetEnv()` and `tuningFirst()` previously checked string non-emptiness via `on(...)`, causing `SOTC_NATIVE_NOADDITIVE=0` in `SotC.sh` to disable additive blending anyway. Switched to numeric non-zero checks (`std::atoi`).
  - Opaque classification & draw sorting (`PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp`): removed the unsafe `opacity >= 0.95f` / `Blend::Normal` shortcut; added `x.alphaTest != y.alphaTest` to `sameState`; unified sorting and instancing behind `isSafeOpaqueDraw()` (checking alpha-test `AREF`/`minAlpha`, `<1.0f` opacity, blend mode, depth, overlay, reflection, and skin flags); defaulted `sortOpaqueEnabled()` (`SOTC_SORT_OPAQUE`) to `0`.
  - Indexed `meshoptimizer` + GL batching (`game/overrides/sotc_native.cpp`, `PS2Recomp/ps2xRuntime/src/lib/native/native_renderer_gl.cpp`): defaulted `SOTC_MESHOPT=0`, added `SOTC_MESHOPT_VCACHE=0` and `SOTC_MESHOPT_OVERDRAW=0`, restricted triangle reordering strictly to `reorderSafe` (`!twoPasses && Opaque && layer 2..11 && no v.a < 128 && flags`), kept `SOTC_MESHOPT=1` indexed via `uploadIndexedMesh` instead of expanding triangles, added a `vao` bind guard in `uploadIndexedMesh`, and repaired `glMergeDraws` / `copyIndexedRun` + `glDrawElementsInstanced` / `glDrawElements` for indexed static meshes.
  - Atomic async terrain publication (`native_renderer.h`, `native_renderer.cpp`, `sotc_native.cpp`): added `todoUrgent` terrain priority queue (`priority = isTerrainPkt` for layers 1–3) and non-Vita `wake.wait(lock)` in `AsyncBuilds`; retained last complete static packet (`completeId`, `hasComplete`, `completeTwoPasses`, `completeLit`) across async replacement; gated `e->resolved = e->ready && e->readyTwo` and held 2-pass pairs atomically. On the deterministic `f=5058` frame, `held` packets dropped from `held 1284 (terrain 1212) pictures held 2` (`bench_phase1_baseline.log`) to `held 0 (terrain 0) pictures held 0` (`bench_phase5_meshopt0.log` and `bench_final_pgo_merged.log`).
- **Visual & unit-test validation on R36S:**
  - Inspected `SOTC_GL_SHOTS=500` framebuffer captures converted to PNG across `SOTC_MESHOPT=0` (`shots_phase5`: frames `3000`, `3500`, `4000`), `SOTC_MESHOPT=1` (`shots_runA`: `3500`, `4000`), `SOTC_MESHOPT=1, VCACHE=1` (`shots_runB`: `3000`, `3500`), and `SOTC_MESHOPT=1, VCACHE=1, SORT_OPAQUE=1, GL_SORT87=1` (`shots_runC`: `3500`): Agro, Wander, Mono, Shrine dome/columns, foliage alpha-test, and Forbidden Lands valley rendered solid with zero transparency holes or Z-fighting/flicker.
  - Cross-compiled arm64 `ps2x_tests` and ran `/tmp/ps2x_tests` on R36S: reproduced the pre-existing failure pattern documented in `AGENTS.md` (GS primitive/presentation cases, DMAC `D_CTRL DMAE`, recompiler test cases, ending `EXIT=139`), with no regressions in runtime or native renderer tests.
- **Controlled 125s deterministic R36S A/B benchmarks (`SOTC_GL_SHOTS=0`, `gp 4458..5298`, 15 buckets, `performance` governor):**

| Build / Flags | `draws` (min..max, med) | FPS (min / med / avg / max) | `game` med (avg) | `render` med (avg) | `present` med (avg) |
| --- | --- | --- | --- | --- | --- |
| `pgo_pass95` + `SOTC_MESHOPT=0` (`bench_clean0_meshopt0.log`) | `830..1261` (`967`) | `26.70 / 28.90 / 28.91 / 30.30` | `34.50 ms` (`34.51`) | `27.70 ms` (`28.33`) | `16.30 ms` (`15.81`) |
| `pgo_pass95` + `SOTC_MESHOPT=1, VCACHE=0` (`bench_clean1_meshopt1.log`) | `829..2011` (`1550`) | `27.10 / 29.60 / 29.59 / 31.60` | `33.70 ms` (`33.76`) | `31.90 ms` (`30.65`) | `10.50 ms` (`11.39`) |
| `pgo_pass95` + `SOTC_MESHOPT=1, VCACHE=1` (`bench_clean2_vcache1.log`) | `832..4066` (`1130`) | `25.60 / 28.80 / 28.55 / 31.00` | `34.60 ms` (`35.01`) | `30.80 ms` (`29.71`) | `10.90 ms` (`11.85`) |
| `pgo_pass96` (skewed slow PGO-gen run) + `SOTC_MESHOPT=1` (`bench_final_pgo_pass96.log`) | `829..1245` (`978`) | `25.40 / 27.60 / 27.23 / 28.00` | `36.10 ms` (`36.61`) | `30.40 ms` (`30.71`) | `16.40 ms` (`15.64`) |
| **`pgo_merged` + `SOTC_MESHOPT=1, VCACHE=0` (`bench_final_pgo_merged.log`, MD5 `063ba18e41672b0dd04ce85bcec62695`)** | `828..1238` (`977`) | **`27.00 / 30.10 / 29.87 / 31.00`** | **`33.10 ms` (`33.37`)** | `29.40 ms` (`29.13`) | `14.90 ms` (`14.11`) |

- **Switch evaluation & deployed production config (`/roms/ports/.SotC_Celosia/SotC.sh`):**
  - `SOTC_MESHOPT=1` (indexed dedup + vertex-fetch optimization without triangle reordering) improved clean throughput (`29.60` med / `29.59` avg vs `28.90` med / `28.91` avg at `MESHOPT=0`, `-4.42 ms` avg `present`), while `SOTC_MESHOPT_VCACHE=1` (`28.80` med), `SOTC_MESHOPT_OVERDRAW=1` (`30.16 ms` render under `SHOTS=500`), and `SOTC_SORT_OPAQUE=1` + `SOTC_GL_SORT87=1` (`28.01 ms` render under `SHOTS=500`) regressed and remain `0` by default.
  - Final `pgo_merged` (`211` `.gcda` TUs from `pgo_pass95` merged with the 3 updated `.gcda` units from `pgo_pass96`, `78,046,288` bytes stripped) reached **`30.10 FPS` med (`29.87` avg, `33.10 ms` `game` med)** across `gp 4458..5298` (reaching `f=5478` in `125 s` with `0` held terrain packets at `f=5058`/`5298`/`5478`). Production `SotC.sh` ships `SOTC_ALPHA_TEST=1`, `SOTC_NATIVE_NOADDITIVE=0`, `SOTC_SAC_LOD_SCALE=1.6`, `SOTC_SAC_MIN_PROJECTED=0`, `SOTC_GL_SORT87=0`, `SOTC_SORT_OPAQUE=0`, `SOTC_MESHOPT=1`, `SOTC_MESHOPT_VCACHE=0`, `SOTC_MESHOPT_OVERDRAW=0`, `SOTC_DETAIL_TEX=1`, `SOTC_HOLD_INCOMPLETE=1`, `SOTC_HOLD_TERRAIN_MS=300`, `SOTC_GL_SHOTS=0`.

### Phase 6B — Cutscene Freeze (`decomp_char122`) & GL Alpha-Cutout / Additive Follow-up

- **Root causes fixed:**
  - **Bird-past-Wander cutscene freeze (`game/overrides/decomp_char122.cpp`):** In `char122::cloth::dynamics()`, `sotc_decomp::layoutClassFast()` falls back to `sotc_decomp::callGuest()` for non-plain objects such as scripted layout entities, overwriting guest `$sp` (`GPR 29`) and `$ra` (`GPR 31`, return site `0x01137AACu`). Saving and restoring `$sp` and `$ra` around `layoutClassFast()` fixed the cutscene hang (`shot_1080..1440` pixel delta rose from `<1%` frozen to `27.72%–51.85%` `right>12` across `900..2160` in `ab_vis_fix`), alongside a `std::isnan` guard in `cloth::check()`.
  - **`SOTC_NATIVE_NOADDITIVE` env hook (`PS2Recomp/ps2xRuntime/src/lib/native/native_renderer.cpp`):** Wired `"SOTC_NATIVE_NOADDITIVE"` directly to the `dbg_additive` `debugToggle` item so additive Shrine light shafts obey `SOTC_NATIVE_NOADDITIVE=0`.
  - **R36S GL cutout / blend overrides (`/roms/ports/.SotC_Celosia/SotC.sh`):** Updated `SOTC_GL_AT98=0`, `SOTC_GL_OPAQUE88=0`, `SOTC_GL_NODISCARD=0`, and `SOTC_GL_PSOFT98=0` (with `SOTC_NATIVE_NOADDITIVE=0`) and deployed stripped `ps2EntryRunner` (`78,046,288` bytes, MD5 `f97edf3dcb9fb0de9a499165c2adcd10`).
- **Hardware validation (R36S):**
  - **Visual captures (`ab_vis_fix` `125 s` `SOTC_GL_SHOTS=180`; `gp_vis` `135 s` `SOTC_GL_SHOTS=300`):** Verified solid hawk wing cutout (`shot_720`), smooth un-frozen canyon bird flyby (`shot_1080..1440`), solid Wander/Agro/forest foliage (`shot_1800`, `shot_3420`, `shot_3780`), rain streaks (`shot_3960`), additive Shrine light beams + solid interior (`gp_vis/shot_2700`, `3000`), and Shrine stairs / Forbidden Lands terrain (`gp_vis/shot_3300`). (Note: a thin dark dashed horizontal line across the distant sky in `shot_720` / `shot_3780` is pre-existing in base GL sky/cloud rendering, unchanged from `MESHOPT=0, GL_INSTANCING=0, GL_MERGE=0`.)
  - **Deterministic `125 s` `r36s_run_bench.py` (`SOTC_GL_SHOTS=0`):** Across `28` 3D scene samples (`draws >= 500`), measured `31.60` med / `37.41` avg FPS (`29.10` min, `60.10` max); across active Shrine/courtyard gameplay (`f=3738..4278` and `5118..5358`, `15` samples, `draws 1148..2346`), measured **`29.10–34.30 FPS`** (median `31.60`, mean `31.65`; `f=5118..5358`: `32.00` med / `32.40` avg FPS).
  - **`ps2x_tests` (`aarch64` cross-build, executed on R36S `/tmp/ps2x_tests`):** Executed all `150` test lines (`93 Passed`, `57 Failed`, matching the pre-existing GS primitive/presentation, DMAC `D_CTRL DMAE`, and recompiler test failures noted in `AGENTS.md`).

## Phase A (R36S Native Audit) — Release Instrumentation Removal, Compiler Flags, & Matched PGO (`2026-11-16`)

- **Build & runtime findings verified (`PS2Recomp/CMakeLists.txt`, `PS2Recomp/ps2xRuntime/CMakeLists.txt`):**
  - Previously, `-DPS2X_LINUX_RELEASE=ON` left `PS2X_GUEST_READ_WATCH=1`, `PS2X_GUEST_WRITE_TRACE=1`, and `PS2_RUNTIME_LOGS=1` active in `ps2_runtime` / `ps2EntryRunner`, and `PS2Recomp/CMakeLists.txt` added `-march=armv8-a+fp+simd` after `CMAKE_CXX_FLAGS` (overwriting `+crypto+crc` on manual toolchains without emitting `-mtune=cortex-a35` by default).
  - Updated `PS2Recomp/ps2xRuntime/CMakeLists.txt` (`PS2X_ENABLE_GUEST_READ_WATCH`, `PS2X_ENABLE_GUEST_WRITE_TRACE`) so `PS2X_LINUX_RELEASE=ON` disables `READ_WATCH`, `WRITE_TRACE`, `PS2_RUNTIME_LOGS`, `AGRESSIVE_LOGS`, and `PS2X_ENABLE_IOP_RPC_TRACE` unconditionally; added `PS2X_ARM64_MTUNE` (default `cortex-a35`) to `PS2Recomp/CMakeLists.txt`.
  - **Root cause of stale-PGO cancellation in `A1`/`A2`:** Compiling `A1` (`READ_WATCH=0, WRITE_TRACE=0`) with the old `pgo_merged` corpus (recorded under `READ_WATCH=1, WRITE_TRACE=1`) changed function CFG arc counts (`-Werror=coverage-mismatch`, e.g., `decomp_ai106::leaderNative` `141 → 91` arcs), silently dropping PGO profiles across modified functions. Regenerated a fresh `214`-TU `.gcda` corpus (`pgo_phase_a`) on R36S with `SOTC_GCOV_DUMP_FRAME=5200` (`0` `-Wmissing-profile` / `-Wcoverage-mismatch` warnings on `decomp_ai106.cpp`).
- **Controlled 130s deterministic R36S A/B results (`SOTC_GL_SHOTS=0`, `SOTC_FRAME_LOG_ALL=1`, matched Shrine/courtyard walk `f=4500..5900` = `1401` frames, `draws 853..1000`, `performance` governor):**

| Build | Stripped Size (MD5) | FPS mean / med (`4500..5900`) | Frame p50 / p90 / p99 / max | `game` med (mean) | `render` med (mean) | CPU (`t=80..120s`) / RSS |
| --- | --- | --- | --- | --- | --- | --- |
| **`A0` Baseline** (`READ_WATCH=1`, `WRITE_TRACE=1`, `RUNTIME_LOGS=1`, `pgo_merged`) | `78,046,288 B` (`f97edf3d...`) | `30.38 / 31.06` | `32.20 / 36.10 / 44.40 / 130.4 ms` | `32.20 ms` (`32.91`) | `19.70 ms` (`25.95`) | `50.1%` (`cpu0 97.9%`) / `342.3 MiB` |
| **`A1`** (`READ_WATCH=0`, `WRITE_TRACE=0`, `RUNTIME_LOGS=1`, stale `pgo_merged`) | `64,283,728 B` (`10c89962...`) | `30.20 / 30.86` | `32.40 / 36.50 / 45.10 / 64.4 ms` | `32.40 ms` (`33.09`) | `20.10 ms` (`26.46`) | `50.7%` (`cpu0 98.2%`) / `343.1 MiB` |
| **`A2` / `A3`** (`+ RUNTIME_LOGS=OFF`; `A3` also `+crypto+crc -mtune=cortex-a35`, stale `pgo_merged`) | `64,283,728 B` (`330a444e...`) | `30.24 / 30.96` | `32.30 / 36.60 / 43.81 / 62.6 ms` | `32.30 ms` (`33.05`) | `20.20 ms` (`26.43`) | `50.7%` (`cpu0 98.0%`) / `339.7 MiB` |
| **`A3_PGO` (final Phase A)** (`A3` + matched `pgo_phase_a`, run `A3_PGO_r2`) | **`63,431,760 B` (`f8ee05ad...`)** | **`32.34 / 33.00`** *(all `f>=4500`, `n=1503`: `32.20 / 32.89`)* | **`30.30 / 34.20 / 40.80 / 71.6 ms`** | **`30.30 ms` (`30.91`)** | `20.30 ms` (`25.65`) | `51.2%` (`cpu0 97.9%`) / `337.2 MiB` |

- **Notes & Visual Integrity (`A3_VIS`):**
  - On the first `A3_PGO` run (`A3_PGO` run 1), the frame-based `PS2X_INPUT_SCRIPT` drifted at frame `2940` and hit pause at `4458+` (`draws 12`); `A3_PGO_r2` matched the exact Shrine/courtyard route (`draws 991 -> 988 -> 989 -> 999`, `1401` matched frames in `4500..5900`, reaching `max_frame=6002` in `130 s` vs `5900` on `A0`).
  - Net Phase A improvement (`A3_PGO` vs `A0`, matched `4500..5900`): **`+1.96 FPS` mean (`+6.45%`)**, **`+1.94 FPS` median**, **`-2.00 ms` `game` thread (`-6.08%`)**, **`-14.61 MB` stripped binary (`-18.7%`)**, **`-5.1 MiB` RSS**.
  - Inspected `SOTC_GL_SHOTS=300` PNG conversions (`shot_2400`, `shot_3000`, `shot_3600`, `shot_3900` at `512×448`) verifying memory card UI, additive Shrine rays, cutscenes, and Wander/terrain outdoors with zero artifacts. Deployed `A3_PGO` (`f8ee05adb7dfdc287b3f13df0c835f98`) to `/roms/ports/.SotC_Celosia/ps2sotc_aarch64_pie_sdl`.

---

## Phase B (`B1`, `B3`, `B4`) — Diagnostic Integrity, Collision Evaluation & L2 Cache Findings (`2026-11-16`)

- **Baseline Release+PGO post-PR #37 (`PR37`, `9ea397c3bc7bdf2a2a861bb6f3f12cb4`, matched `4500..5900` = `1401` frames):**
  - FPS **`32.74` mean / `33.44` med** (`p50 29.90 / p90 34.00 / p99 39.40 ms`), `game` `29.90 / 30.53 ms`, `render` `19.60 / 25.75 ms`, `wait 0.00 / 0.01 ms` (`f>=4500`, `n=1524`: `32.62` mean / `33.33` med FPS, `cpu0 98.1%`, `RSS 340.9 MiB`).
- **Diagnostic fixes kept (working tree, zero Release binary impact):**
  - [`game/overrides/decomp_collcensus.cpp`](game/overrides/decomp_collcensus.cpp#L240-L248): parsed `SOTC_COLL_CENSUS` and `SOTC_COLLISION_STATS` numerically (`std::atoi(env) != 0`) instead of `!= nullptr`, preventing `"0"` in `SotC.sh` from enabling all 55 `collcensus` wrappers in `DIAG` builds (which broke `sotc_decomp::isNative()` pointer equality on 6 collision natives including `0x0111C078` and disabled `obj125`/`fin125` during `DIAG` profiling).
  - [`game/overrides/decomp_collision.cpp`](game/overrides/decomp_collision.cpp#L86-L154) and [`game/overrides/decomp_hot44a.cpp`](game/overrides/decomp_hot44a.cpp#L232-L238): split `[collision] keep` telemetry into `native test` (`decomp_hot44a::clipKeepTest`) vs `recompiled` (`noteKeepFallback()` only when `!hotList44On()`).
  - Verified on real R36S (`D5_DIAG`, `D9_COLL_LOG`): `SOTC_CALLGUEST_STATS=600` (`f=4200..4800`, `8006.5 calls/frame`, `5772.1 to natives`) restored `"native"` tagging on `0111c078` (`157.1`), `0112a250` (`96.9`), `01118b88` (`94.6`), etc. (`00107000` `SignalSema` omits `"native"` only because `installSemaFast` registers outside `registry()`), and confirmed `coll115` (`fallback 0.00, original 0.00`), `fin125` (`guest entries 0.00`), and `seg123` (`20–25% kept`) already handle gameplay collision natively.
- **Controlled B4 Collision A/B Experiments on Real R36S (`matched 4500..5900` + `SOTC_DIFFTEST`):**

| Build / Run | Verification / Equivalence | `4500..5900` FPS mean / med | `game` med / mean | `render` med / mean | Decision |
| --- | --- | --- | --- | --- | --- |
| **`PR37` Baseline** | Deployed telemetry fix | **`32.74 / 33.44`** | `29.90 / 30.53 ms` | `19.60 / 25.75 ms` | **Baseline** |
| **`B4_NOSEG123`** (`SOTC_SEG123=0`) | Runtime switch test | `32.57 / 33.33` | `30.00 / 30.69 ms` | `19.60 / 25.25 ms` | Keep `SOTC_SEG123=1` |
| **`B4_COLLDB`** (`decomp_hot47j.cpp`: skip `g_view`, `insertDirect`, `groupCall` + PGO) | `D6`/`D7`/`D8` difftest (`0` diffs in gameplay/RAM) | `32.35 / 33.33` | `30.00 / 30.90 ms` | `19.80 / 25.19 ms` | **Discarded** (`-0.39 FPS` mean) |
| **`B4_COLL125`** (`coll125.h`: 4-slot `ListSlot` cache + PGO) | `D10_FIN125_CHECK`: `check 22078 differ 0` (`point max 0.00 mm`) | `32.30 / 33.00` | `30.30 / 30.96 ms` | `20.20 / 26.07 ms` | **Discarded** (`-0.44 FPS` mean) |

- **B4 Architectural Takeaway:** Adding extra per-slot state/lookup tables (`+120 KB` `ListSlot` in `B4_COLL125`) or larger inlined dispatch (`B4_COLLDB`) regresses both `game` and `render` on the RK3326's `4× Cortex-A35` (`256 KB` shared L2 cache), even with `0` physics diffs and matched `.gcda` PGO. With `D0_PROF` showing no single collision function above `2.80%`, B4 is closed cleanly in favor of **B5** (`DispDispObjGroupModel` `1.94%`, `010F3A08` `1.93%`, `ExecCharaGeometry` `1.69%`, `gamecore_sub_013CFBE0` `1.97%`, `SOTC_CHAR104=1` re-test on `PR37`) and **Phase C** (Mali-G31 `render 19.60 med / 25.75 mean ms`).

---

## Phase B5 — `SOTC_CHAR104` A/B, Display Walk, & `pf141=0` L2 Cache Win (`2026-11-16`)

- **B5-A (`SOTC_CHAR104=1` A/B on `PR37`):**
  - Tested `SOTC_CHAR104=1` (`B5_CHAR104`) on the exact `PR37` binary (`matched 4500..5900`, `n=1394`): **`30.05` mean / `30.86` med FPS** (`game 32.40 / 33.26 ms`, `render 20.90 / 27.12 ms`, `p99 50.21 ms`, `cpu0 89.8% cpu1..3 35.0/41.1/41.4%`) vs **`PR37`** (`32.74 / 33.44 FPS`) — **`-2.69 FPS` mean / `-2.58 FPS` med**.
  - `[char104]`/`[char111]`/`[char114]` telemetry at `frame 5418`: offloading `13.86 chars/frame` (`5.657 ms` worker time) cost `4.315 ms/frame` in game-thread joins (`chan 2960`, `group 142`) + `1.82 ms/frame` in handover/merge (`1.210 + 0.611 ms`), plus `+1.37 ms` mean render regression from shared L2 contention. **`SOTC_CHAR104=0` kept OFF.**
- **B5-B (Display Walk & `pf141` Shared-L2 Contention on Cortex-A35):**
  - In `D0_PROF` (`a3_pgo_game.prof`), `013cfc68` inside `gamecore_sub_013CFBE0` (`pf141::segmentEnd` mutex + condvar wake + speculative prefetch on every character segment) accounted for `675` samples (`1.54%`), while `DispDispObjGroupModel` held `844` samples (`1.94%`).
  - **Controlled R36S A/B tests (`matched 4500..5900`, `n=1401`):**

| Build / Flags | Equivalence / PGO | `4500..5900` FPS mean / med | `game` med / mean | `render` med / mean (`60f win med`) | Decision |
| --- | --- | --- | --- | --- | --- |
| **`PR37` Baseline** (`PF141=1`) | `pgo_phase_a` | `32.74 / 33.44` | `29.90 / 30.53 ms` | `19.60 / 25.75 ms` (`25.75`) | Baseline |
| **`B5_NOCLIP140`** (`SOTC_CLIP140=0`) | `PR37` binary | `32.67 / 33.33` | `30.00 / 30.61 ms` | `19.80 / 25.28 ms` (`26.00`) | Keep `SOTC_CLIP140=1` |
| **`B5_NOPF141`** (`SOTC_PF141=0, PFC141=0`) | `PR37` binary | `33.54 / 34.48` | `29.00 / 29.81 ms` | `19.20 / 24.30 ms` (`23.45`) | **Win (`+0.80 / +1.04 FPS`)** |
| **`B5_NOPF141_r2`** (`SOTC_PF141=0, PFC141=0`) | `PR37` binary | `33.71 / 34.48` | `29.00 / 29.65 ms` | `19.10 / 25.65 ms` (`25.80`) | **Reproducible** |
| **`B5_NOPF_NOW67`** (`PF141=0, WALK67=0`) | `PR37` binary | `33.54 / 34.36` | `29.10 / 29.81 ms` | `19.00 / 24.45 ms` (`25.00`) | Keep `SOTC_WALK67=1` |
| **`B5_WALK_PGO`** (`decomp_walk.cpp` `callNative` + `pf141=0`) | `D12` difftest `0` diffs on `ClipLayoutObjModel`/`AddSortObj`/`DispLayoutObjModel`; `pgo_phase_b5` | `32.56 / 33.22` | `30.10 / 30.71 ms` | `20.20 / 25.25 ms` (`24.65`) | **Discarded** (`-0.98 FPS` I-cache bloat) |
| **`B5_FINAL`** (`sotc_decomp.h`: `kPf141`/`kPfc141` default `0` on `linux && aarch64`) | `pgo_phase_a`, `63,366,224 B` stripped (`3a35c26e...`) | **`33.89 / 34.72`** | **`28.80 / 29.49 ms`** | `7.80 / 24.38 ms` (`24.15`) | **Kept & Deployed (`+1.15 / +1.28 FPS`)** |

- **B5 Architectural Takeaway & Deployment:**
  - Inlining 9 `g.callNative` guards inside `DispDispObjGroupModel` / `DispLayoutObjModel` bloated hot loop code size past Cortex-A35's 32 KB L1I and lost `-0.98 FPS` even with fresh `.gcda` PGO (`pgo_phase_b5`), so `decomp_walk.cpp` was reverted cleanly.
  - Defaulting `kPf141` / `kPfc141` OFF on Linux `aarch64` ([`game/overrides/sotc_decomp.h`](game/overrides/sotc_decomp.h#L705-L718)) + updating `/roms/ports/.SotC_Celosia/SotC.sh` (`SOTC_PF141=0`, `SOTC_PFC141=0`) eliminated both mutex/condvar overhead at `0x013cfc68` and speculative L2 thrashing on the RK3326's 256 KB shared L2, delivering **`33.89` mean / `34.72` med FPS** (`f>=4500`, `n=1577`: `33.70 / 34.48 FPS`, `max_frame 6076`, `RSS 336.7 MiB`).
