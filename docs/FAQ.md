# FAQ

Short answers to the questions that come up most often. Each answer names the
source it comes from; the long form lives in [STATUS.md](STATUS.md),
[worklist.md](worklist.md) and [CONTRIBUTING.md](../CONTRIBUTING.md).

## Do I need to own the game, and where does the ISO come from?

Yes — the repository ships no game data, and you need your own copy of *Shadow
of the Colossus* (USA, serial SCUS-97472). `setup` takes a disc image as an
`.iso` or a `.zip` containing one (`./celosia setup --iso "<path>"`) and
extracts it into `disc/`; the README documents only the format the image must
have, not a place to get one, so it comes from your own disc.

*Source: README.md ("No game data is included", "Requirements", "Quick start"),
.recomp.json (`original.serial`).*

## Which platforms run the game today?

Two: macOS on Apple Silicon (Metal) and the PS Vita (OpenGL ES / vitaGL) — the
two targets the project declares. On the Mac the game runs from boot to
gameplay at 60 fps and 1536×1344 on an M4 Pro; on the Vita it builds and runs at
18–25 fps in Vita3K's intro, with fidelity and speed still in progress and real
hardware not measured yet.

*Source: README.md (intro, "Status"), .recomp.json (`targets`), STATUS.md
("At a glance").*

## Why is this not an emulator?

No emulator runs the game's CPU code: the EE code from the boot ELF and the
game's three XFF modules is translated into C++ ahead of time and compiled
natively. The picture is drawn by a native renderer (Metal on the Mac, OpenGL
ES on the Vita) that reads the game's model data directly, while the remaining
hardware (DMA, IOP, SPU2) is reimplemented as host code — today the game no
longer needs GS, VU1 or VIF emulation to draw a frame. The project began as a
static recompilation on a runtime that emulated the PS2 hardware and replaces
that hardware piece by piece.

*Source: README.md (intro), STATUS.md (intro).*

## Why is there no CI for the game itself?

Because the game cannot be built without the disc image the user owns: the
repository ships no game data, and the first build compiles the runtime and
around 16k recompiled functions, needing about 10 GB of disk and an internet
connection for the dependencies CMake downloads. What CI does cover is the
toolchain and its unit tests — the `PS2Recomp` subtree, the `ps2x_tests` suite
and the CTest `gs_cache` suite — so a runner would need the copyrighted disc
image to go further. [CONTRIBUTING.md](../CONTRIBUTING.md) lists what that
matrix checks.

*Source: README.md ("Requirements", "Quick start"), CONTRIBUTING.md (§5
Testing, §8 Pull request checklist), .recomp.json (`toolchain`).*

## How do I run the unit tests without the game?

The MiniTest suite `ps2x_tests` lives in `PS2Recomp/ps2xTest` and covers the
R5900 decoder, the recompiler, ELF analysis, GS, VU/VU1, memory, kernel,
interrupts, IOP and SIF; it needs no disc image, because it exercises the
runtime and the toolchain rather than the game. Configure the tree with
`-DPS2X_BUILD_TEST=ON`, run the binary (`build/ps2xTest/ps2x_tests`), and run
the GS cache suite on its own with `ctest -R gs_cache`.
[TESTING.md](TESTING.md) has the full matrix and the tools that compare
captures and frames.

*Source: CONTRIBUTING.md (§5 Testing), TESTING.md, README.md ("Requirements" —
the image is needed for the game build only).*

## How do I report a bug or request a feature?

Open an issue on the GitHub repository: it ships issue templates for a bug
report and for a feature request, and the bug template asks for the platform,
the exact steps and command line, every `SOTC_*` and `PS2X_*` switch you set,
and the logs. Never attach copyrighted game data (the ISO, the extracted
`SCUS_974.72`, XFF dumps, textures, videos) — describe what happened instead and
the maintainers reproduce it with their own copy. Before filing, check
[STATUS.md](STATUS.md) for the open issues and [worklist.md](worklist.md) for
the queue, since the problem may already be known or already scheduled.

*Source: CONTRIBUTING.md (§8 Pull request checklist, §9 What you need),
STATUS.md ("Open issues"), the repository's bug and feature-request issue
templates.*

## Where is the progress and status?

[STATUS.md](STATUS.md) is the in-repo record: it opens with the date it was
updated, an "At a glance" table for both platforms, what is native today, the
open issues and the ordered [worklist.md](worklist.md) of what is being fixed
right now. The public progress page (henrykun55.github.io/celosia) shows the
treemaps per area, the decompilation figures and the history of rounds. For the
decompilation itself, [DECOMP_WORKFLOW.md](DECOMP_WORKFLOW.md) explains the
loop and [worklist.md](worklist.md) lists the targets.

*Source: README.md (intro), STATUS.md (head, "Open issues").*

## What is the `PS2Recomp` folder versus `game/overrides`?

`PS2Recomp/` is the recompiler and the runtime — a git subtree of
ran-j/PS2Recomp with our changes, kept generic (SPU2, VU1 AOT, asynchronous GS,
VU0 fixes) and meant to go back upstream. `game/` is Shadow of the Colossus
specific: the boot ELF config, the stub bindings and the runtime overrides,
with `game/overrides/` holding the game-specific native code (the hand-written
decompiled functions and the renderer hooks). The rule of thumb: a change that
would also help another PS2 game belongs in `PS2Recomp/`, a change that only
makes sense for this game belongs in `game/` — and everything generated
(recompiled code, captures, build files) goes to `build/` and is never
committed.

*Source: README.md ("How it works", "Layout"), STATUS.md ("What is native").*

## Can this be reused for Ico?

The toolchain is reusable — PS2Recomp is upstream, and the changes kept in
`PS2Recomp/` are meant to go back to it — but the port is not: `.recomp.json`
describes this project as a recompilation of *Shadow of the Colossus* (USA,
SCUS-97472), and the game-specific parts (boot ELF config, stub bindings,
overrides) are that game's own. Porting Ico would mean its own disc, its own
boot ELF and module capture, its own stub bindings and overrides, and its own
renderer work; nothing in the repository supports Ico today. Team Ico appears
in the credits only as the game's copyright holder.

*Source: .recomp.json (`game`, `original`), README.md ("Layout", "Credits").*

## Is this legal, and what about copyrighted data?

The repository never ships game data and expects every user to supply their own
copy of the disc; nothing derived from it (the extracted disc, the ISO, the XFF
dumps, captures, generated runner code) is committed or asked for in issues.
The project's own code is GPL-3.0, the same license as PS2Recomp, and it states
that it is not affiliated with or endorsed by Sony Interactive Entertainment /
Team Ico. This is not legal advice: read the license and check your own
situation before you redistribute anything.

*Source: README.md ("No game data is included", "License", "Credits"),
CONTRIBUTING.md (§9 What you need).*

## How do I investigate a rendering bug?

Compare the two pictures of the same moment: F9 flips between the native
renderer and the emulated GS, F8 saves both plus the frame's native draws into
`shots/` (`f8_<n>_emulated.png`, `f8_<n>_native.png`, `f8_<n>_draws.txt`), F10
opens the renderer's panel and F1's Debug tab shows the draw layers and the
timing. For fidelity work, compare against `PS2X_GS_GPU=0` (CPU raster, exact
GS fog) and not against the default emulated view, whose GPU rasterizer has no
GS fog. For a frame-level record, the Debug tab's "Record scene" writes the
draw lists and captures to `logs/rec/`, and `tools/rec.py` lists a recording
and draws its frames again ([TOOLS.md](TOOLS.md)); then check the open issues
in [STATUS.md](STATUS.md) and the queue in [worklist.md](worklist.md).

*Source: README.md (F1, F8, F9, F10, "Record scene"), STATUS.md ("Open issues"
and its note on `PS2X_GS_GPU=0`), TOOLS.md.*

## Where do I start contributing?

Read [CONTRIBUTING.md](../CONTRIBUTING.md) first: it lays out the lifecycle
(INSPECT → PLAN → BRANCH → IMPLEMENT → TEST → REVIEW → COMMIT → PUSH → PULL
REQUEST → MERGE → RELEASE), the branch naming, the Conventional Commit style
and the pull-request checklist. Then pick a target from [worklist.md](worklist.md),
use [ARCHITECTURE.md](ARCHITECTURE.md) to see how the pieces fit together and
[DECOMP_WORKFLOW.md](DECOMP_WORKFLOW.md) for the decompilation loop. The
toolchain and its unit tests are the part you can work on without owning the
disc; the full game needs macOS on Apple Silicon.

*Source: CONTRIBUTING.md (§2 Development lifecycle, §5 Testing, §6
Decompilation workflow, §9 What you need), worklist.md, ARCHITECTURE.md,
DECOMP_WORKFLOW.md.*

## What is `NICO.DAT` and what are the XFF files?

The XFF files are the game's relocatable code modules: *Shadow of the Colossus*
ships one small boot ELF and loads the rest of its code at run time —
`KERNEL.XFF`, `MANAGER.XFF` and `GAMECORE.XFF` — which the game links in
memory with its own dynamic linker, and `setup` captures each from guest RAM
and merges them into one `SOTC.elf` for the recompiler. `NICO.DAT` is the
largest file in the disc's game directory (2,667,340,678 bytes), with a custom
header instead of an ISO9660 one, sitting next to the small `NICO.ELF` boot ELF
(242,940 bytes) and the three XFF modules. The port runs that boot ELF under
the name `SCUS_974.72` so that the game-specific overrides apply.

*Source: README.md ("How it works"), STATUS.md ("How the recompilation was
done").*

## Where do logs and captures go?

On the Mac, `./celosia run` writes `logs/play.log`, the game folder holds
`shots/` (the F8 captures; `./celosia autotest` writes `shots/run`) and
`memcards/` for the saves, and the scene and input recordings go to
`logs/rec/`. On the Vita, the logs are `ux0:data/celosia/log.txt` and
`ux0:data/celosia/err.txt`, the saves go to `ux0:data/celosia/memcards/`, the
scene recordings to `ux0:data/celosia/rec/<date_time>/` and the renderer's own
shots to `ux0:data/celosia/shots`. The frame, sound and loader logs are
switched on with `SOTC_FRAME_LOG`, `SOTC_FRAME_LOG_ALL` and `SOTC_AUDIO_LOG`,
and the player's menu button "Mark this moment" writes a `[mark] <n> frame <f>`
line so you can find the moment later.

*Source: README.md ("Commands", "PS Vita", F1 settings, F8), STATUS.md.*
