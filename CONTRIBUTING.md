# Contributing to celosia

Thanks for helping. This is the short version of how work happens here; the binding rules are spelled out in the sections below.

## 1. What celosia is

celosia is a native port of *Shadow of the Colossus* (PS2, USA, `SCUS-97472`) to macOS on Apple Silicon (Metal) and to the PS Vita (OpenGL ES / vitaGL).
The game's MIPS R5900 code is statically recompiled to C++ ahead of time and compiled natively, so no emulator runs the game's CPU code.
Everything else the game talks to — GS, DMA, VU1, SPU2, IOP, SIF — is reimplemented as host code under `PS2Recomp/ps2xRuntime` and `PS2Recomp/ps2xIOP`.

## 2. Development lifecycle

Every change walks these steps, in this order:

- **INSPECT** — read `README.md` and the sections below, look at the layout, the tests and `git status`; never overwrite someone else's uncommitted work.
- **PLAN** — say what changes, which files, how it will be validated and what "done" means.
- **BRANCH** — cut one dedicated branch from the right base; unrelated work goes on a different branch.
- **IMPLEMENT** — the smallest change that satisfies the requirement, in the existing architecture and style, with no drive-by refactors.
- **TEST** — unit tests, builds, static analysis, regressions; report what actually ran and what did not.
- **REVIEW** — `git status`, `git diff`, `git diff --check`; look for scope creep, duplicated code, leaks, error handling, secrets.
- **COMMIT** — small, coherent commits, staging only the intended files.
- **PUSH** — push the branch; never force-push a shared branch without authorization.
- **PULL REQUEST** — state the problem, the implementation, the tests run and the known limitations.
- **MERGE** — only after the required checks pass, using the project's merge strategy.
- **RELEASE** — a SemVer tag (`vMAJOR.MINOR.PATCH`) plus release notes, when one is due.
- **MONITOR** — watch CI and the progress page after the merge, and fix what breaks.

## 3. Branch naming

`main` is the stable, production-ready branch: land changes through a pull request, never by committing to it directly.

- `feat/<slug>` — a new feature.
- `fix/<slug>` — a bug fix.
- `refactor/<slug>` — internal restructuring, no behaviour change.
- `perf/<slug>` — a measured performance improvement.
- `docs/<slug>` — documentation only.
- `hotfix/<slug>` — an urgent production fix, branched from `main`.

Use a short kebab-case slug that says what the branch does.

## 4. Building

The bash script [`./celosia`](celosia) is the single entry point: `./celosia setup` extracts the disc and builds, while `build`, `recompile`, `run`, `vita` and `clean` do the obvious things; `./celosia help` lists every command and environment variable (`CELOSIA_DISC`, `CELOSIA_BUILD`, `CELOSIA_JOBS`, `CELOSIA_CMAKE_ARGS`, ...).

Prerequisites per platform, the CMake options and the Windows/Git Bash path handling are in [docs/BUILD.md](docs/BUILD.md). Never edit `build/` by hand: it is generated.

A typical first run looks like this:

```
./celosia setup --iso path/to/SCUS-974.72.iso
./celosia run
```

`setup` extracts the disc into `disc/`, configures CMake and builds every tool and
the generated runner; `run` launches it with the extracted disc. The C++ is C++20,
indented with four spaces, and the repository is LF-only — keep it that way.

## 5. Testing

The full matrix, plus the tools that compare captures and frames, is described in [docs/TESTING.md](docs/TESTING.md). In short:

- `ps2x_tests` — the MiniTest unit suite in `PS2Recomp/ps2xTest`, covering the R5900 decoder, the recompiler, ELF analysis, GS, VU/VU1, memory, kernel, interrupts, IOP and SIF; build it with the rest and run it before opening a pull request.
- **CTest / `gs_cache`** — the GS cache suite registers with CTest when `BUILD_TESTING` is on, so `ctest -R gs_cache` runs it on its own.
- **difftest** — `SOTC_DIFFTEST=name,name` (or `all`) runs each decompiled call twice on the same machine state and compares the results; a decompilation change is only proven when its difftest reports 0 mismatches (see [docs/decomp.md](docs/decomp.md)).

Report results honestly: passed, failed, or not executed with the reason.

## 6. Decompilation workflow

How to pick a target, inspect it, write the C++ and prove it is correct is in [docs/DECOMP_WORKFLOW.md](docs/DECOMP_WORKFLOW.md). Targets come from [docs/worklist.md](docs/worklist.md), the disassembly helpers from [`tools/`](tools) (`disasm.py`, `xref.py`, `ghidra.sh`, ...), and the running record of rounds from [docs/decomp.md](docs/decomp.md).

## 7. Commit style

This project uses Conventional Commits: `type(scope): description`, with `feat`, `fix`, `perf`, `refactor`, `test`, `docs`, `build`, `ci` and `chore` as the types and the affected area as the scope (`renderer`, `audio`, `gs`, `iop`, `sotc`, `docs`, ...). Use the imperative mood, lowercase, no trailing period, and keep one logical change per commit.

Historical note: older commits in this repository are titled `Round N: ...` or `Fix N: ...`, from the round-based decompilation and porting work. Those are left as they are; new contributions follow the commit style above.

## 8. Pull request checklist

The template lives in [.github/PULL_REQUEST_TEMPLATE.md](.github/PULL_REQUEST_TEMPLATE.md). A pull request is ready when:

- `PS2Recomp` builds with no new warnings, `ps2x_tests` passes and the `ctest` `gs_cache` suites pass;
- `git diff --check` is clean and `git status` shows nothing outside the task scope;
- no secrets, credentials, tokens, keys or game data are in the diff;
- every relative link in the changed docs resolves;
- the docs that describe the behaviour (`STATUS`, `worklist`, the plans) were updated;
- it does one thing, and the branch name says which.

## 9. What you need

- **Your own disc image** of *Shadow of the Colossus* (USA, `SCUS-97472`), as an `.iso` or a `.zip` containing one. It is never committed, and neither is anything derived from it: `disc/`, `iso/`, `*.iso`, `*.XFF`, captures and generated runner code are all ignored.
- **macOS on Apple Silicon** with the Xcode Command Line Tools for the full game: the Metal renderer, audio and input are the supported configuration there.
- **Linux and Windows** build the toolchain and run the unit tests, which is enough for recompiler, runtime and decompilation work; the full game is not supported there yet.

## 10. Docs map

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the pieces fit together.
- [docs/STATUS.md](docs/STATUS.md) — where the port stands today.
- [docs/worklist.md](docs/worklist.md) — what to work on next.
- [docs/TOOLS.md](docs/TOOLS.md) — the scripts under `tools/` and `PS2Recomp/`.
- [docs/FAQ.md](docs/FAQ.md) — common questions and pitfalls.
- [docs/GLOSSARY.md](docs/GLOSSARY.md) — EE, VU1, GS, GIF, XFF and the rest.

Also worth reading: [docs/BUILD.md](docs/BUILD.md), [docs/TESTING.md](docs/TESTING.md), [docs/DECOMP_WORKFLOW.md](docs/DECOMP_WORKFLOW.md), [docs/decomp.md](docs/decomp.md), [docs/known_issues.md](docs/known_issues.md) and the native port plans ([plan](docs/native_port_plan.md), [renderer](docs/native_renderer_map.md), [audio](docs/native_audio_plan.md), [log](docs/native_port_log.md)).

By contributing you agree that your changes are licensed under the terms of [LICENSE](LICENSE).
