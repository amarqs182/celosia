# R36S port notes

## Target
- SoC: Rockchip RK3326
- CPU: quad-core Cortex-A35
- GPU: Mali-G31 MP2
- Memory: 1 GB shared RAM
- Display: 640×480 at approximately 60 Hz
- Platform: aarch64 Linux, SDL2/KMSDRM, GLES

The target is stable 60 FPS with headroom, not merely a momentary 60 FPS. Use a 16.67 ms frame budget and measure frame-time percentiles.

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
