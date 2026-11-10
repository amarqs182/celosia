# tools/ — catalog

Every tracked file under `tools/`, one entry each, grouped by purpose. The list
below was taken from `git ls-files tools` (54 files: 38 `.py`, 7 `.sh`,
1 `.swift`, 8 `tools/ghidra/*.java`) and contains no invented entries — if a file
is not here, it is not in the repo.

Entry tags:

- **CLI (argparse)** — parsed options, run directly.
- **CLI (argv)** — positional arguments / hand-rolled `sys.argv` flags.
- **helper library** — imported by other tools (public API must stay stable).
- **internal** — no stable API yet, used from one place or by hand.

---

## a. Pipeline and capture — building the ELF, the config, the module dump, recordings

- `tools/build_elf.py` — combines the boot ELF with loaded XFF modules into one
  `SOTC.elf` plus `functions.csv`, `symbols.txt`, `dynamic_imm.txt` and
  `merged_entries.txt`. CLI (argv): `out_dir boot_elf boot_syms XFF@hdr@ram_dump...`.
  library consumer of `elfmini` / `modimage` / `funcmap_elf`.
- `tools/gen_config.py` — writes the PS2Recomp `config.toml` (stubs with a real
  runtime handler, plus hidden entry points). CLI (argv): `out_dir boot_cfg`.
- `tools/funcmap_elf.py` — MIPS function-start heuristics (jal targets, code
  pointers, prologues, split merging) and the CSV builder. **helper library**
  (`find_starts`, `merge_split_starts`, `hidden_entries`, `main`) with a CLI
  (argv): `elf_path [sym_path] out.csv`.
- `tools/modimage.py` — captures a loaded XFF module (sections, symbols,
  function map) out of a guest RAM dump. **helper library**
  (`find_header`, `LoadedModule`); the CLI lives in `build_elf.py`.
- `tools/rec.py` — reads a scene recording (`info.txt`, `assets_*.bin`,
  `frames_*.bin`, `full_*.bin`) and can re-render its frames. CLI (argparse)
  with subcommands `list`, `frames`, `draws`, `textures`, `render`, `profile`,
  `pad`.

## b. Decomp accounting and dashboards

- `tools/decomp_status.py` — how much of the game is native: per-function, by
  size and by call counts → `docs/decomp_status.json` / `.svg`. CLI (argparse):
  `--replaced file [--calls ...]`.
- `tools/decomp_tree.py` — the same accounting per game area → `docs/decomp_tree.json`;
  `--readme` rewrites the README's decomp block. CLI (argparse). internal consumer
  of `decomp_status` + `status_treemap`.
- `tools/decomp_history.py` — decomp progress over time (code / calls / time /
  generated curves) → `docs/decomp_history.svg`; `--add --label` appends a
  measurement. CLI (argparse).
- `tools/status_treemap.py` — draws `docs/port_status.json` as a treemap SVG
  (`squarify`, `wrap` live here). **helper library** + CLI (argv): `[src.json] [dst.svg]`.

## c. ELF / disassembly / xref helpers

- `tools/elfmini.py` — minimal ELF32 LE reader (`Elf.read32`, `exec_ranges`,
  `sections`). **helper library**, imported by `build_elf`, `gen_config`,
  `funcmap_elf`, `disasm`, `xref`, `callers`. No CLI.
- `tools/xff.py` — parser for the XFF2 relocatable module format (`Xff`,
  `Section`, `Symbol`, `Reloc`, `RelocTable`). **helper library**, imported by
  `modimage` and `xffinfo`. No CLI.
- `tools/xffinfo.py` — dumps an XFF module's header counts, sections, reloc
  tables and symbol map. CLI (argv): `xff [xff ...]`.
- `tools/disasm.py` — disassembles a function / range of the combined ELF
  (capstone). CLI (argv): `<name|0xstart> [0xend]`.
- `tools/xref.py` — finds code referencing a string or address in the combined
  ELF (lui/addiu|ori pairs). CLI (argv): `SOTC.elf functions.csv <string|0xaddr>`.
- `tools/callers.py` — lists static (jal/j) callers of a function, recursively
  to a depth. CLI (argv): `<name|0xaddr> [depth]`.
- `tools/addr2sym.py` — resolves addresses to `function+offset` from a functions.csv
  (in ADB-style output). CLI (argv): `functions.csv 0xaddr ...`.
- `tools/gsdecode.py` — decodes a GS VRAM dump to a texture PNG using the PSMT4 /
  PSMT8 swizzle. **helper library** (`decode_t4`, `addr_t4`) + CLI (argv):
  `vram.bin tbp tbw w h out.png`.
- `tools/dmawalk.py` — walks a VIF1 DMA chain in a RAM dump and decodes VIF codes
  and GIF packets. CLI (argv): `ram.bin 0xstart`.
- `tools/vudis.py` — minimal VU micro-mode disassembler. CLI (argv):
  `image.bin [start end]`.
- `tools/filter_stubs.py` — drops TOML stub bindings whose runtime handler is only
  a `TODO_NAMED` placeholder, so the SDK code is recompiled instead. CLI (argv):
  `runtime_dir config.toml`.
- `tools/gen_lean.py` — generates `game/overrides/lean_hot.cpp`'s include
  fragments (decls, renames, table, partN) from the hot set; `--from` first
  rewrites `game/lean_hot.txt` from call-count dumps. CLI (argv), internal.

## d. Measurement and comparison

- `tools/measure.sh` — the measurement gate: a scripted Mac route driven by
  VBlank entries, one summary line, shots under `shots/<label>/`. CLI: `<label> [seconds]`.
- `tools/bench.sh` — scripted New Game run (`./celosia autotest`) printing the
  average game fps per section. CLI: `[label]`.
- `tools/gameplay_fps.sh` — scripted New Game → shrine walk, real game frames per
  second per 5 s window. CLI: `[seconds]`.
- `tools/framediff.py` — pixel-compares the route frames of two `measure.sh` runs,
  flagging frames with > 1.5% changed pixels. CLI (argv): `<dir before> <dir after> [threshold]`.
- `tools/native_compare.py` — ranks native vs emulated frame pairs from a paired
  run and writes contact sheets of the worst. CLI (argv): `run [--out dir] [--worst N]`.
- `tools/native_sbs.py` — side-by-side emulated | native | signed difference at
  2x per frame, plus `gaps.txt`. CLI (argv): `run [--out dir] [--scale N] [--only i,j]`.
- `tools/guest_profile.py` — names the samples of `PS2X_PROFILE` (the guest
  sampling profiler) per recompiled function, with callers. CLI (argv):
  `profile [--top N] [--callers N] [--module]`.
- `tools/prof129.py` — round 129's console sampler (`SOTC_PROF129=1`) as
  function-level shares of the game thread, grouped by subsystem. CLI (argparse):
  `err.txt --elf ELF [--windows|--t|--frames|--top|--group|--host|--base|--raw ...]`.
- `tools/vita_profile.py` — names a Vita3K profile (`VITA3K_PROFILE`): top
  functions, main callers, split by thread; `--minus` gives a window. CLI (argv):
  `profile [--top|--elf|--thread|--busiest|--minus ...]`.
- `tools/vita_hot_order.py` — writes the Vita build's hot-code `.ld` ordering
  from Vita3K profiles. CLI (argv): `out.ld profile:elf ... [--size-elf ELF]`.
- `tools/replay_compare.py` — the input replay report: one HTML page with the
  determinism check and one row per recording MARK. CLI (argv):
  `<recording> <out dir> <label>=<run dir> ...`.
- `tools/replay_compare.sh` — replays an input recording on the Mac's GS path,
  the native renderer and in Vita3K, then calls `replay_compare.py`. CLI:
  `<recording> [name]` (env: RUNNER, V3, VITA3K, VPK, MHZ, SKIP, SHOTS, SECS).
- `tools/sample_inclusive.py` — inclusive sample counts per function inside one
  thread of a macOS `sample` report. CLI (argv): `report.txt ThreadName [pattern ...]`.
- `tools/heapsym.py` — names the call sites of the `SOTC_HEAP_PROF [heap]` lines
  (operator new's load delta from `nm`). CLI (argv): `<elf> <err.txt> [last-n-reports]`.
- `tools/lift126.py` — lifts the camera's / characters' guest functions to plain
  host C++ (`*_lift` files) from the recompiler's listings; refuses anything it
  cannot express. CLI (argv): `out.cpp switch-name addr|@file ...`.
- `tools/model_view.py` — draws a SotC model's vertices straight from a guest RAM
  dump (`SOTC_MODEL_DUMP`), to check the model format. CLI (argv):
  `logs/model_dump [--dump N] [--out file.png]`.
- `tools/pgo_fix.py` — merges Vita PGO `.gcda` dumps of several runs into one
  usable profile (arc counters summed, object summary fixed, paths made
  relocatable with `--root`). CLI (argv): `[--root tree] out-dir run-dir ...`.
- `tools/ab100.py` — reads a console / Vita3K err.txt per A/B state
  (`SOTC_AB_ROTATE=1`): mean, p90, spike share and a paired delta per state.
  CLI (argv): `err.txt [--min-draws|--k|--skip|--max-ms|--causes ...]`. Internal
  consumer of `spike100.attribute`.
- `tools/abframes.py` — per-frame or draw-matched 60-frame summary A/B table
  from a rotating A/B `err.txt` (`SOTC_AB_ROTATE=1`), leaving out host-stall
  frames (`>= --stall` ms). CLI (argv):
  `err.txt [--summary] [--bin DRAWS] [--from F] [--to F] [--stall MS] [--min N]`.
- `tools/spike100.py` — classifies 3D frame spikes in a console err.txt by cause
  (render thread, sched, joins, hooks, frame end, own CPU). **helper library**
  (`parse`, `attribute`, `classify`) + CLI (argv):
  `err.txt [--min-draws N] [--k K] [-v] [--local N]`.
- `tools/fmean129.py` — mean frame ms and sched ms of `[frame] each|slow` lines
  over a game-frame range. CLI (argv): `err.txt from to`.

## e. Shell entry points

- `tools/ghidra.sh` — headless Ghidra over the game: `import` builds the project
  from `recomp/game/SOTC.elf`, otherwise it runs a `tools/ghidra/*.java` script
  and prints only its lines. CLI: `import | <Script.java> args...`.
- `tools/native_run.sh` — paired native/emulated run along a quick route for
  `native_compare.py`. CLI: `<out dir> [last game frame]`.
- `tools/vita3k.sh` — runs the Vita build in its own Vita3K tree (install, timed
  run, `PROF=`, `PROF_SNAPS=`, `SHOTS=` window capture, shader-cache seeding).
  CLI: `[seconds]` (env: MHZ, CORES, PROF, NOINSTALL, SHOTS, SEED, ...).
- `tools/winid.swift` — prints the `CGWindowID` of a process's largest on-screen
  window for `screencapture -l` (used by `vita3k.sh SHOTS=`). CLI:
  `<pid> [dump]`. Not a Python tool; it needs `swiftc` at hand.

## f. Ghidra Java scripts (run through `tools/ghidra.sh`)

- `tools/ghidra/ApplyRecompNames.java` — names every function from `functions.csv`
  (and `symbols.txt`), creating the ones Ghidra did not find. Script args:
  `<functions.csv> [symbols.txt]`.
- `tools/ghidra/Callers.java` — prints every call site of the named functions.
  Script args: `name ...`.
- `tools/ghidra/Containing.java` — prints the function containing each address.
  Script args: `0xaddr ...`.
- `tools/ghidra/DecompileAt.java` — like `DecompileTo`, but disassembles and
  creates the functions first (raw binaries). Script args: `outdir 0xaddr ...`.
- `tools/ghidra/DecompileTo.java` — decompiles named functions into
  `<outdir>/<name>.c`. Script args: `outdir name|0xaddr ...`.
- `tools/ghidra/Disasm.java` — prints the instructions of the functions holding
  the given addresses. Script args: `0xaddr ...`.
- `tools/ghidra/Range.java` — prints the instructions in `[start, end)`.
  Script args: `0xstart 0xend`.
- `tools/ghidra/RefsTo.java` — prints every reference to the given addresses
  (from, containing function, type). Script args: `0xaddr ...`.

---

## Common patterns

- **Repo root.** Never hardcoded: it is computed from `__file__` in one of two
  house idioms — `pathlib.Path(__file__).resolve().parent.parent`
  (`build_elf.py`, `gen_config.py`) or
  `os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))` and the
  equivalent. Shell scripts use `cd "$(dirname "$0")/.."` and set `ROOT=$PWD`.
  Keep computing it; do not assume the repository is checked out at a fixed path.
- **Style.** f-strings are the house style; `%` formatting survives only in
  `heapsym.py`. No `import typing` appears anywhere in `tools/` (checked).
  Python target is 3.10+ (`X | None` annotations are therefore fine, as
  `modimage.py` already uses them).
- **Tests.** There is no pytest suite and no test runner for `tools/`: scripts are
  self-contained one-offs driven by hand or from the `./celosia` pipeline, and
  validation is "does the output look right". Do not add a test framework here.
- **Imports between tools.** Scripts add their own directory to `sys.path` and
  import each other rather than duplicating logic. `elfmini.py` (`Elf`), `xff.py`
  (`Xff`), `modimage.py` (`find_header`, `LoadedModule`), `funcmap_elf.py`
  (`find_starts`, `merge_split_starts`, `hidden_entries`, `main`),
  `status_treemap.py` (`squarify`, `wrap`) and `spike100.py` (`parse`,
  `attribute`, `classify`) are the modules other tools import.
- **Public API stability.** The modules above must keep the names, signatures and
  output formats they export, because other tools (and the `./celosia` pipeline:
  `build_elf.py` → `gen_config.py` → recompile) call them. The set named as
  stability-sensitive is `elfmini.py`, `xff.py`, `modimage.py`,
  `status_treemap.py`, `gsdecode.py` and `heapsym.py`: the first four are already
  shared, `gsdecode.py` exposes `decode_t4` / `addr_t4` for hand use, and
  `heapsym.py` is a standalone CLI today — so nothing currently breaks, but keep
  those entry points intact (no renaming, no extra required arguments, no change
  to written file names or printed columns).
- **Environment assumptions.** Several scripts shell out to macOS-only tools
  (`sysctl`, `screencapture`, `sample`) or to a local VitaSDK
  (`~/vitasdk/bin/arm-vita-eabi-nm`), and `lift126.py` / `prof129.py` still carry
  absolute `/Users/...` paths for the recompiler's output dir and the VitaSDK
  binary. These are work-in-progress constants, not API.
