# R36S status

## Current objective
Achieve stable 60 FPS with headroom on the R36S (16.67 ms frame budget; aim for roughly 14 ms P95 or better).

## Known project state
- Target: Rockchip RK3326, Cortex-A35, Mali-G31 MP2, 1 GB shared RAM.
- Display target: 640×480 at approximately 60 Hz.
- Runtime environment: aarch64 Linux, SDL2/KMSDRM, GLES.
- `meshoptimizer` is already integrated; do not repeat its integration.
- Previous renderer work reduced draw calls from approximately 2,175 to approximately 127 using `glSort87/glMergeDraws`.
- Fur shells were dynamically limited to four.
- GLES 3 shader work has been performed.

These are recorded project facts from earlier work. Re-measure the current branch before treating any number as a current benchmark.

## Active performance issue
The latest reported device behavior is that the game reaches a 3D scene and the camera can move, but camera movement drops performance below 10 FPS. Reported overall CPU use was around 50%, while GPU utilization appeared as 0%.

### Initial log capture (2026-11-13, R36S, `ps2sotc_aarch64_pie_sdl`, `/tmp/sotc.log` frames 10409–10430)
Confirmed facts from `[frame] slow` lines:
- Frame time: **51.8–70.2 ms** (~14–19 FPS).
- `render`: **23.5–37.6 ms** (`plan` 5.0–10.1 ms, `pass` 15.8–28.4 ms, `calls` 9.6–19.5 ms for **475–476 GL calls**, **2102–2103 draws**, **95k verts**).
- `present`: **22.7–40.1 ms**.
- `hooks`: **1.1–2.1 ms**; `jobs`: **3.8–16.7 ms**; `sync`: **0.1–17.5 ms**; `waits render`: up to **16.8 ms**.

### Moving-camera capture (2026-11-13, R36S, `ps2sotc_aarch64_pie_sdl`, `/tmp/sotc.log` frames 11117–11119)
Confirmed facts during camera movement:
- Frame time: **141.7–152.2 ms** (**6.7 fps** average over 300 frames, slowest **218.7 ms**).
- `render`: **70.4–89.2 ms**, dominated by **`plan`: 48.4–64.2 ms** (up from 5.0–10.1 ms stationary) while `runs` jumped from `0/0` to `41/2`–`66/1` and GL calls rose to `555–721`.
- `pass`: **19.3–26.6 ms** (`calls` 12.1–17.0 ms); `present`: **52.3–73.8 ms**.
- `GameThread` blocked waiting on `RenderThread`: `waits render` **81.5–90.8 ms**, `sync end` **60.9–90.9 ms**.
- Conclusion: The moving-camera collapse to < 10 FPS is **not** in `hooks` (1.4–3.9 ms) or `jobs` (3.9–5.3 ms); it is caused by `RenderThread` **`plan` (48–64 ms)** + **`present` (52–74 ms)** stalling `GameThread`.

Those readings do not establish the bottleneck. Investigate with matched stationary-camera and moving-camera captures:
- frame time and P50/P95/P99;
- per-core and per-thread CPU use;
- hot functions and guest PCs;
- draw calls, submitted vertices, state changes;
- GPU timing or validated driver telemetry;
- synchronization and presentation waits.

Do not assume GPU utilization of 0% means the GPU is idle. Do not assume 50% aggregate CPU means there is CPU headroom.

## Historical issues
Earlier loading-screen stalls, semaphore theories, IOP/CD hypotheses, and VU1 investigations are historical context only. Re-check current evidence before reopening them. Do not treat an old worklist status as proof of the present state.

## Update rule
When a measurement or diagnosis changes, update this file with date, device/build identity, reproduction steps, measurement method, result, and confidence. Separate confirmed facts from hypotheses.
