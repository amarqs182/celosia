# Known limitations

Consolidated view of the limitations that are still open. Every row below is
taken from one of the repository's own status documents; nothing here is new.

| Source | What it holds |
|---|---|
| [`docs/STATUS.md`](STATUS.md) | the "Open issues" section and the R36S (Linux aarch64) status |
| [`docs/worklist.md`](worklist.md) | the pending list, items marked todo, doing or parked |
| [`docs/known_issues.md`](known_issues.md) | the older native-picture findings that `STATUS.md` points to |

Where a source is ambiguous, the row says *needs verification*.

## Limitations

| Limitation | Impact | Platform | Tracker |
|---|---|---|---|
| Bloom shape: bright openings saturate to a flat white with a narrow halo, while the PS2 shows a wide, soft glow | The sky and the shrine window blow out to white instead of glowing softly | macOS (native renderer) | [STATUS.md](STATUS.md) "Open issues"; [known_issues.md](known_issues.md) "bloom over the sky and windows" |
| Gaps on animated models such as Wander; skinning weights and strip assembly were checked and are correct | Holes in the character meshes | macOS (native renderer) | [worklist.md](worklist.md) item 19 (todo); [STATUS.md](STATUS.md) "Open issues" |
| Vita bloom and glow are off because the renderer has no depth textures | The PS2's bloom and the boss-death glow are missing on the Vita | PS Vita | [worklist.md](worklist.md) items 12 and 14; [STATUS.md](STATUS.md) "Open issues" |
| Long Vita runs past 80 minutes are unconfirmed; the 72-minute run died out of heap | Long-session stability on the Vita is unproven | PS Vita | [worklist.md](worklist.md) item 15 (doing); [STATUS.md](STATUS.md) "Open issues" |
| The native picture is brighter than the emulated one, most in the opening; mean luminance about 0.68 versus 0.84, which side owns which number needs verification | Brightness mismatch against the PS2: a factor in the opening, an offset in the shrine | macOS (native renderer) | [known_issues.md](known_issues.md) "Native: brighter than the emulated picture"; [STATUS.md](STATUS.md) "Open issues" points at it |
| Native zFog (opt-in) keeps layer 1 out of the main pass | Flat, blurred patches where layer 1 surfaces come close to the camera (shrine floor near the stairs), so zFog stays off | macOS (native renderer) | [known_issues.md](known_issues.md) "Native zFog (opt-in)"; the older list that [STATUS.md](STATUS.md) "Open issues" points at |
| Wander's soft ground shadow on the plain and Agro's shadow up close are missing natively; needs verification, since body shadows are listed as on by default | Characters look unanchored on the plain and up close | macOS (native renderer) | [known_issues.md](known_issues.md) "Wander's shadow on the plain, Agro up close"; the older list that [STATUS.md](STATUS.md) "Open issues" points at |
| EE syscalls 0x0d and 0x0e are refuted as the cause of the R36S stall, kept here as a note only | No impact by itself; it stops a dead end from being reopened | Linux aarch64 (R36S) | [worklist.md](worklist.md) item 33 (parked); [STATUS.md](STATUS.md) H3 |
| No CD data path after boot: 0 `sceCdRead` calls, and no `CDROM.IRX` or `FILEIO.IRX` in the IOP | The game stays at the loading screen, so the gameplay route cannot start | Linux aarch64 (R36S) | [worklist.md](worklist.md) items 34 and 46; [STATUS.md](STATUS.md) next steps 3 |
| The VU1 thread never advances after loading: `mscal=0`, `vu1Instr=0`, `xgkick=0` in all samples | No 3D geometry; only the loading screen's lines and sprites are drawn | Linux aarch64 (R36S) | [worklist.md](worklist.md) items 35 and 49; [STATUS.md](STATUS.md) "VU1 / GS (measured)" |
| GS to GLES presentation on Linux: `displayFbp=0`, `sourceFbp=224`, `shownFps` about 0 | The picture never reaches the swap, so the screen stays black | Linux aarch64 (R36S) | [worklist.md](worklist.md) items 50 and 55; [STATUS.md](STATUS.md) key diagnostics |
| The native GL backend uses `#version 330 core` with a GLES3 header, incompatible with SDL2's GLES3 context, and never calls `makeCurrent` | The native GL path cannot run on the device's context | Linux aarch64 (R36S) | [STATUS.md](STATUS.md) "Key diagnostics (recorded)" |
| No memory card with a save: `memcards/`, `mc0/` and `mc1/` are empty and the runtime creates a blank `Mcd001.ps2` | The gameplay route stays locked; the run script requires the save | Linux aarch64 (R36S) | [worklist.md](worklist.md) item 48; [STATUS.md](STATUS.md) "Memory / cards (measured)" |
| The Linux screenshot path (`ExportImage`) is a stub: `PS2X_SNAPSHOT_DIR` gets only `index.txt` | No screenshot can be taken on the device | Linux aarch64 (R36S) | [worklist.md](worklist.md) item 51; [STATUS.md](STATUS.md) next steps 6 |
| The device framebuffer `/dev/fb0` is 100% black while the GS draws real content | No image on the device; the real framebuffer screenshot is still pending | Linux aarch64 (R36S) | [worklist.md](worklist.md) item 32 (doing); [STATUS.md](STATUS.md) metrics table |
| Vita speed on real hardware is not measured yet; the recorded figures are from Vita3K | The 30 fps goal and the device's memory behaviour are unverified | PS Vita | [STATUS.md](STATUS.md) "Open issues" and status table |

## Notes

- **EE syscalls 0x0d and 0x0e.** Refuted as the cause of the R36S stall, so
  this row is a note and not an open defect: only 3 calls with `v1=0xd`
  (PC=0x106c98) and 9 with `v1=0xe` (PC=0x106ca8) happen at boot, all of them
  the kernel's syscall-table enumeration, and no syscall is traced during the
  stall. `Dispatcher.cpp` has no `case` 0x07-0x0f and the 37 `sceCd*` stubs
  have no call site (dead code).
- **The 0.68 and 0.84 luminances.** The source gives "emulated / native mean
  brightness about 0.68 in the opening, 0.84 in the shrine, 0.95 on the plain"
  and, from the paired route of 2026-10-16, "native about 15 brighter in mean
  luminance through the route". Which side owns 0.68 and which owns 0.84 needs
  verification.
- **The bloom reference.** The game's own chain says the native bloom is
  right, but a real reference (PCSX2 software or the console) is still wanted
  before either side is tuned, so the bloom row is open on the native side.
- **The emulated picture has no GS fog.** For fidelity work, compare the
  native picture against `PS2X_GS_GPU=0` (CPU raster, exact GS fog), not
  against the default emulated view.

## Sources

1. **Bloom shape** — `docs/STATUS.md:214-216` ("Open issues"); `docs/known_issues.md:78-90` ("Native vs emulated: bloom over the sky and windows (reference doubtful)").
2. **Gaps on animated models** — `docs/STATUS.md:217-219` ("Open issues", "Character meshes"); `docs/worklist.md:43` (item 19, todo: "not seen recently; check with an F8 capture when it shows").
3. **Vita bloom and glow off** — `docs/STATUS.md:220-222` ("Open issues", Vita); `docs/worklist.md:31` (item 12, done: "narrow glow, one frame late; not the Mac's GS chain yet") and `docs/worklist.md:33` (item 14, todo).
4. **Long Vita runs** — `docs/STATUS.md:223-224` ("Open issues"); `docs/worklist.md:34` (item 15, doing: the 72-minute run died out of heap, `new(512 KB)` at 156 of 160 MB).
5. **Native brighter than emulated** — `docs/known_issues.md:39-53` (mean brightness about 0.68 in the opening, 0.84 in the shrine, 0.95 on the plain) and `docs/known_issues.md:55-69` ("Native about 15 brighter in mean luminance through the route"); pointed at by `docs/STATUS.md:226-227`.
6. **Native zFog, layer 1 out of the main pass** — `docs/known_issues.md:92-100` (opt-in behind `SOTC_NATIVE_ZFOG=1`).
7. **Wander's and Agro's missing shadows** — `docs/known_issues.md:102-108` (seen 2026-10-16; route picture 33, game frame ~2040); `docs/STATUS.md:112` lists body shadows (Wander, Agro, colossi) as on for Mac and Vita since round 38, so the current state needs verification.
8. **EE syscalls 0x0d and 0x0e** — `docs/STATUS.md:434-445` and `docs/STATUS.md:479-492` (H3, refuted); `docs/worklist.md:72` (item 33, parked).
9. **CD data path** — `docs/worklist.md:73` (item 34, doing) and `docs/worklist.md:85` (item 46, todo); `docs/STATUS.md:300-301` (0 `sceCdRead` post-boot, files read through `fioOpen`/`fioRead`), `docs/STATUS.md:426-433` (`CDREAD`/`CDROM`/`FILEIO` absent in the native IOP) and `docs/STATUS.md:529-532` (no `CDROM.IRX`/`FILEIO.IRX`, and no code loads `IOPRP300.IMG`).
10. **VU1 thread blocked** — `docs/worklist.md:74` (item 35, todo, blocked by item 43) and `docs/worklist.md:88` (item 49, todo); `docs/STATUS.md:321-322` (`mscal=0`, `vu1Instr=0`, `xgkick=0` in all 14 samples of A2 and 35 of A3), `docs/STATUS.md:422-425` and `docs/STATUS.md:511-512`.
11. **GS to GLES presentation** — `docs/worklist.md:89` (item 50, todo) and `docs/worklist.md:94` (item 55, done: the present-path analysis); `docs/STATUS.md:412-416` (`shownFps` counts `presentedFrames`, not real swaps; `hostFps` about 59), `docs/STATUS.md:513-515` and `docs/STATUS.md:537-538`.
12. **GLES3 shader profile** — `docs/STATUS.md:417-421` ("Key diagnostics (recorded)"): `native_renderer_gl.cpp:78` uses `#version 330 core` with a GLES3 header.
13. **Memory card save path** — `docs/worklist.md:87` (item 48, todo); `docs/STATUS.md:332-338` ("Memory / cards (measured)") and `docs/STATUS.md:539-540` (next step 7).
14. **Screenshot path on Linux** — `docs/worklist.md:90` (item 51, todo); `docs/STATUS.md:328-330` (`ExportImage` is a stub at `raylib_linux.cpp:894`, so `PS2X_SNAPSHOT_DIR` writes only `index.txt`) and `docs/STATUS.md:537-538`.
15. **`/dev/fb0` all black** — `docs/worklist.md:71` (item 32, doing); `docs/STATUS.md:258` (100% black, 1.228.800 zero bytes), `docs/STATUS.md:325-330` and `docs/STATUS.md:461-466` (the GS draws 20.640 / 81.965 / 20.570 non-black pixels that never reach the screen).
16. **Vita real-hardware speed** — `docs/STATUS.md:222` ("Open issues") and `docs/STATUS.md:25` (status table: 18-25 fps in Vita3K's intro, real hardware not measured yet).
