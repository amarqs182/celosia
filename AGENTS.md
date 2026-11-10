# Celosia — AGENTS.md

Celosia is a native port of *Shadow of the Colossus* (PS2, USA, SCUS-97472):
static recompilation with PS2Recomp, a native Metal/OpenGL renderer, and an
ongoing decompilation of the game's hot code into host C++.

- Upstream (read-only reference): `https://github.com/HenryKun55/celosia`
- This fork (where work happens): `https://github.com/amarqs182/celosia`
  - `origin` = this fork, `upstream` = HenryKun55/celosia
- Local clone: `/home/amarqs/Projects/celosia`

## Hard rules

1. **Never push to `upstream`.** Only `origin` is writable. Push with
   `git push origin <branch>`. Do not run `git push upstream` under any
   circumstances.
2. **Never force-push `main`.** `main` on `origin` tracks upstream's `main`.
   Work on feature branches: `git switch -c feat/<slug>`.
3. **Sync before starting:** `git fetch upstream && git rebase upstream/main`
   (or merge) so the branch is based on current upstream.
4. **No game assets in the repo.** The game ISO/ELF/XFF files are copyrighted
   and stay out of git. Never commit them, never upload them anywhere.
5. **No fabricated measurements.** This machine has no game disc and no
   Vita3K. Never write "measured X ms" or "0 mismatches" unless the number
   came from an actual run you performed here. If a claim needs the game,
   say so explicitly and leave the measurement to the maintainer.
6. **Do not commit generated build output.** `build/`, `disc/`, `memcards/`,
   `logs/` are gitignored.

## The loop for a decompiled (native) function

Per `docs/decomp.md`:

1. Find what matters — `tools/guest_profile.py`, `tools/prof129.py`.
2. Read it — `tools/ghidra.sh` (Ghidra project) or `tools/disasm.py`.
3. Write it — `game/overrides/decomp_<system>.cpp`, registered with
   `SOTC_DECOMP_FUNCTION(address, name, fn)` (or `SOTC_DECOMP_FLOAT_FUNCTION`
   when the result is in f0). Arguments in a0-a3 (64-bit GPRs), f12/f13;
   result in v0 or f0; end with `returnTo(ctx)`.
4. Prove it — `SOTC_DIFFTEST=name` compares the native against the
   recompiled code (registers + every RAM byte).
5. Measure it — profiler, on the Mac and the Vita.

Every native must keep: relocated code in the XFF modules
(`sotc_decomp::relocated`, `sotc_decomp::jalTarget`), guest calls
(`sotc_decomp::callGuest` with the original's ra), the callee-saved
registers, and the EE cycle accounting.

## Style

- C++20, 4-space indent, braces on their own line, `namespace` blocks per
  system (`decomp_<system>`), `constexpr uint32_t kGuestAddr = 0x...u` for
  guest addresses.
- Comments are dense and explain *why* and *how it was verified* — the
  upstream style. Match the surrounding file's tone.
- Every new switch gets a `SOTC_<NAME>` env var, is documented in
  `README.md`'s switch table, and defaults either on or off with the reason
  stated in the commit message.
- Commit messages are long, single-paragraph and factual, in upstream's
  style: what changed, what was measured, what was proven. Start with
  `Round N:` only when continuing an existing round; otherwise use a plain
  imperative subject line.

## Target platform: R36S (arm64 / aarch64)

**This fork targets the R36S handheld (Rockchip RK3326, aarch64, Mali-G31
GPU, ARM libmali, SDL2 + EGL/GLES 3.2).** It is *not* a macOS, PS Vita, x86
or Android port. The upstream (`HenryKun55/celosia`) targets macOS (Apple
Silicon) and the PS Vita; this fork ports that work to the R36S.

### Hardware facts measured on the device (2026-11-09)

Every fact below was read off the real R36S over SSH, not taken from a
datasheet or from another project:

| Item | Measured value |
| --- | --- |
| SoC / board | `rockchip,rk3326-odroidgo3-linux` (RK3326) |
| CPU | 4× **Cortex-A35** (`CPU implementer 0x41`, `CPU part 0xd04`), aarch64, ARMv8-A |
| CPU features | `fp asimd evtstrm aes pmull sha1 sha2 crc32` — **no FMA**, no fp16, no SVE |
| RAM | 895 MiB total (~726 MiB available) |
| GPU | **Mali-G31**, 1 core / 2 EE, `r0p0`, GPU ID `0x7093` (Bifrost) |
| GPU driver | **ARM libmali** (`libmali-bifrost-g31-rxp0-gbm.so`, Bifrost r13p0) — *not* Mesa |
| GL API | **OpenGL ES 3.2** (`OpenGL ES 3.2 v1.r13p0-01rel0`), EGL 1.5-style with `EGL_KHR_platform_gbm` |
| Display | 640×480 framebuffer, DRM `card0`/`renderD128` via `rockchip-drm` |
| OS | Debian 13 (trixie), **glibc 2.41-12+deb13u4**, kernel `4.4.189` |

Two claims that were previously wrong in this repo's docs, now corrected
here:

- The userland is **aarch64**, not 32-bit ARM. `uname -m` returns `aarch64`.
- The GL stack is **ARM's proprietary libmali over the Bifrost kbase kernel
  driver**, *not* Mesa/Panfrost. Mesa 25.0.7 is installed, and
  `panfrost_dri.so` exists, but every `libEGL.so.1`, `libGLESv2.so.2`,
  `libGLESv3.so.3`, `libgbm.so.1` and `libwayland-egl.so` symlink resolves to
  `libMali.so`. The EGL vendor JSON points at Mesa, but the soname symlinks
  win at link time. Panfrost is not usable here: the GPU is driven by the
  ARM kbase driver through `/dev/mali0`, and `renderD128` is the Rockchip
  display controller, not a GPU render node.

Consequences for your work:

- The GPU path is **GLES 3.2 via libmali's EGL**, not desktop GL 3.3 and not
  Metal. `native_renderer_gl.cpp` currently ships `#version 330 core` shaders
  — those do **not** run on the R36S. Porting them to `#version 300 es` (or
  `310 es`) is part of this fork's job.
- The windowing path is **SDL2** (`-DPLATFORM=SDL`), not GLFW/X11. The R36S
  has no X server; the sysroot has no `libX11`.
- The compiler flags for arm64 are `-march=armv8-a+fp+simd+crypto+crc`
  (already set by `PS2Recomp/CMakeLists.txt` for arm64 hosts). Because the
  A35 has **no FMA**, do not add `-mfpu=neon-fp-armv8` (a 32-bit-only
  option) and do not assume FMA is available at runtime.
- The R36S runs **Debian trixie userland, glibc 2.41**. Never link against
  anything requiring a newer glibc.
- Shared memory is **895 MiB**, not 1 GB. Budget renderer pools against the
  measured figure.

### The device is reachable over SSH

Use the maintainer's local SSH configuration or an interactive credential
prompt. Do not store passwords, askpass scripts containing passwords, private
keys, or other credentials in the repository or in committed agent instructions.

Example (replace the placeholders with local values):

```sh
ssh <user>@<device-ip> 'uname -m'
```

Device rules — read `tools/r36s_smoke.sh` on the `r36s` branch before
touching the hardware:

- Write only under `/roms/ports/SotC_Celosia/` and `/tmp` (`/tmp` is tmpfs,
  448 MiB).
- **Never** scan `/proc/[0-9]*/fd/*` — it freezes the R36S.
- **Never** run `ffmpeg -f kmsgrab` or `fbdev` — it freezes the display.
- Restore EmulationStation after every run.
- **Never** create an EGL display through `EGL_PLATFORM_GBM_KHR` on
  `/dev/dri/renderD128` and then initialise it: doing so NULL-derefs in the
  kernel (`Oops: 96000005`, `Unable to handle kernel NULL pointer
  dereference at virtual address 00000010`) and hangs the device. The
  process ends up in uninterruptible `D` state and the machine stops
  answering SSH; it only recovers on a physical power cycle. Use the
  SDL2 KMSDRM path (what EmulationStation itself uses) instead of
  hand-rolling an EGL/GBM display.

### Toolchain (this machine)

`PS2Recomp/cmake/r36s-toolchain.cmake` cross-compiles for the R36S using the
sysroot at `~/tools/r36s-sysroot` by default (override with `-DR36S_SYSROOT=/path/to/r36s-sysroot`); it contains glibc 2.41 + kernel headers 6.1 from
Debian trixie arm64; gcc/binutils 16.2.0/2.47 from Arch
`aarch64-linux-gnu-*`; the libstdc++ headers and runtime come from the
matching Arch gcc package).

The path passed to `-DCMAKE_TOOLCHAIN_FILE` is relative to the CMake source
directory (`PS2Recomp/`), so `cmake/r36s-toolchain.cmake` is correct when
configuring with `-S PS2Recomp`.

### Verification available on this machine (Linux arm64 cross-build + real R36S)

No game disc and no macOS or Vita3K, but **the R36S itself is reachable over
SSH**, so device-side measurements are possible — with the rules listed
above.

Cross-build (host):

```sh
# Configure the arm64 cross-build (R36S)
cmake -S PS2Recomp -B /tmp/opencode/celosia-r36s -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/r36s-toolchain.cmake \
  -DPLATFORM=SDL \
  -DPS2X_ENABLE_FFMPEG=OFF \
  -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=ON \
  -DPS2X_BUILD_ANALYZER=ON -DPS2X_BUILD_TEST=ON \
  -DPS2X_BUILD_STUDIO=OFF -DCMAKE_BUILD_TYPE=Release

# Build the unit tests for arm64
ninja -C /tmp/opencode/celosia-r36s ps2x_tests

# Run them under the emulator
qemu-aarch64-static -L ~/tools/r36s-sysroot/usr \
  /tmp/opencode/celosia-r36s/ps2xTest/ps2x_tests
```

On the real device (copy the binary over, then run it there):

```sh
SSH_OPTS="-o StrictHostKeyChecking=no -o PreferredAuthentications=password -o PubkeyAuthentication=no"

scp $SSH_OPTS /tmp/opencode/celosia-r36s/ps2xTest/ps2x_tests \
    <user>@<device-ip>:/tmp/ps2x_tests
ssh $SSH_OPTS <user>@<device-ip> 'cd /tmp && ./ps2x_tests'
```

The arm64 `ps2x_tests` binary **does run on the real device** (verified
2026-11-09). Its results match the qemu run: the same two known failures
("callback invocation should use the reserved async stack pool" and
"DMAE=1 should allow GIF DMA transfer") and the same final assert in
`stl_vector.h`. Those are pre-existing failures on `main`, not arm64
regressions — record them as such.

Notes:

- `-DPLATFORM=SDL` is mandatory: the raylib FetchContent defaults to
  GLFW/X11 and the configure fails with `Could NOT find X11`.
- `-DPS2X_ENABLE_FFMPEG=OFF` is mandatory on Linux: the FFmpeg
  `pkg_check_modules` call is not optional there and the sysroot has no
  FFmpeg.
- The x86-64 host build is still useful as a **fast syntax/compile check**
  and it needs `-DCMAKE_CXX_FLAGS="-msse4.1"` (the runtime uses
  `_mm_min_epu32`, which is SSE4.1):

  ```sh
  cmake -S PS2Recomp -B /tmp/opencode/celosia-linux -G Ninja \
    -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=ON \
    -DPS2X_BUILD_ANALYZER=ON -DPS2X_BUILD_TEST=ON \
    -DPS2X_BUILD_STUDIO=OFF -DCMAKE_CXX_FLAGS="-msse4.1"
  ninja -C /tmp/opencode/celosia-linux ps2x_tests
  /tmp/opencode/celosia-linux/ps2xTest/ps2x_tests
  ```

Also usable: `python3 tools/*.py` (pure-Python analysis tools),
`tools/ghidra.sh` (needs a Ghidra install), the GitHub API through `gh`.

What still does **not** work here: running the game itself (no disc),
`SOTC_DIFFTEST` runs, and picture comparisons. Never fabricate these — write
the command and leave the measurement to the maintainer.

## Docs to keep in sync

- `README.md` — the switch table and the status block.
- `docs/decomp.md` — what is native, per system.
- `docs/worklist.md` — the open items and their status.
- `docs/rounds.json`, `docs/decomp_status.json` — generated by
  `tools/decomp_status.py` / `tools/decomp_tree.py`; regenerate, don't hand-edit.
- `docs/STATUS.md`, `docs/known_issues.md` — the project's own status page.

## Style of the upstream commit history

Upstream commits are single long paragraphs describing the change, the
measurement and the proof. When rebasing onto upstream, keep that density:
one commit = one idea, fully explained.


## Development workflow

For each task:

1. Inspect the current branch, working tree, relevant instructions and affected code before editing.
2. State assumptions and acceptance criteria; resolve ambiguities that could change the implementation.
3. Make the smallest focused change. Preserve unrelated user modifications and avoid speculative refactoring.
4. Validate with the narrowest relevant tests, then run the appropriate build or regression checks.
5. Review `git diff --check`, the complete diff and repository status before committing.
6. Report what changed and distinguish passed, failed, blocked and unexecuted checks.

Use KISS, DRY and YAGNI. Prefer tests that reproduce the reported problem. Never claim validation that was not actually run.
