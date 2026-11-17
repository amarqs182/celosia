# Known limitations

Consolidated view of the limitations that are still open. Every row below is
taken from one of the repository's own status documents; nothing here is new.

| Source | What it holds |
|---|---|
| [`docs/STATUS.md`](STATUS.md) | the "Open issues" section and the platform status table |
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
| Vita speed on real hardware is not measured yet; the recorded figures are from Vita3K | The 30 fps goal and the device's memory behaviour are unverified | PS Vita | [STATUS.md](STATUS.md) "Open issues" and status table |

## Notes

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

1. **Bloom shape** — `docs/STATUS.md:272-274` ("Open issues"); `docs/known_issues.md:114-126` ("Native vs emulated: bloom over the sky and windows (reference doubtful)").
2. **Gaps on animated models** — `docs/STATUS.md:275-277` ("Open issues", "Character meshes"); `docs/worklist.md:57` (item 19, `W-06`, todo: "not seen recently; check with an F8 capture when it shows").
3. **Vita bloom and glow off** — `docs/STATUS.md:278-280` ("Open issues", Vita); `docs/worklist.md:45` (item 12, done: "narrow glow, one frame late; not the Mac's GS chain yet") and `docs/worklist.md:47` (item 14, `W-03`, todo).
4. **Long Vita runs** — `docs/STATUS.md:281-282` ("Open issues"); `docs/worklist.md:48` (item 15, `W-04`, doing: the 72-minute run died out of heap, `new(512 KB)` at 156 of 160 MB).
5. **Native brighter than emulated** — `docs/known_issues.md:62-98` ("Native: brighter than the emulated picture, most in the opening"); pointed at by `docs/STATUS.md:284-285`.
6. **Native zFog, layer 1 out of the main pass** — `docs/known_issues.md:140-148` (opt-in behind `SOTC_NATIVE_ZFOG=1`).
7. **Wander's and Agro's missing shadows** — `docs/known_issues.md:158-164` (seen 2026-10-16; route picture 33, game frame ~2040); `docs/STATUS.md:142` lists body shadows (Wander, Agro, colossi) as on for Mac and Vita, so the current state needs verification.
8. **Vita real-hardware speed** — `docs/STATUS.md:280` ("Open issues") and `docs/STATUS.md:57` (status table: 18-25 fps in Vita3K's intro, real hardware not measured yet).
