# Security Policy

Celosia statically recompiles *Shadow of the Colossus* (PS2, SCUS-97472) to
native C++ for macOS (Apple Silicon) and the PS Vita. This policy covers the
source tree - `celosia`, `PS2Recomp/`, `tools/`, `game/overrides/` - not the
game or the game data you supply locally.

## Supported versions

Security fixes land on `main` and ship in the builds produced from it:

- `main` - macOS / Apple Silicon build: **supported**
- `main` - PS Vita build: **supported**
- Any other branch, fork, or older snapshot: **not supported**

If you run a fork or a diverged branch, reproduce against `main` first.

## Reporting a vulnerability

Report vulnerabilities **privately**, through GitHub, using one of:

1. The repository's **Security** tab, then **Report a vulnerability** (GitHub
   private vulnerability reporting) - preferred: it opens a private advisory
   where we discuss, patch, and publish together.
2. A **private GitHub issue** shared only with the maintainers, if private
   vulnerability reporting is unavailable for your account.

Do **not** open a public issue, pull request, or discussion describing the
vulnerability before a fix has been released.

A good report contains:

- **Affected component** - e.g. `celosia`, `ps2xAnalyzer`, `ps2xRecomp`,
  `ps2xRuntime`, `ps2xIOP`, a script under `tools/`, or `game/overrides/`.
- **Reproduction steps** - exact commands, arguments, inputs, and host (macOS
  version / chip, or Vita firmware and toolchain); minimal repros beat long.
- **Impact** - what an attacker gains (code execution, path traversal, file
  read/write, crash or denial of service, memory disclosure) and the
  conditions required.
- **Suggested fix**, if you have one - a patch, the offending lines, or the
  mitigation you would apply; optionally a minimal proof of concept.

Keep secrets, credentials, and personal data out of the report.

## Response expectations

- **Acknowledgement:** within **72 hours**.
- **Updates:** we keep you informed while the issue is triaged and fixed.
  Celosia is maintained by volunteers, so fix timelines depend on severity
  and complexity; we will say so if a report will take longer.
- **Credit:** ask in the report and we will name you in the release notes for
  the version containing the fix. Anonymous reports are welcome too.

## Scope

**In scope:** the build and run pipeline (the `celosia` script - `setup`,
`run`, `build`, `recompile`, `vita`, `clean` - and its CMake configuration);
the scripts under `tools/` and their handling of untrusted ELF/XFF input; the
PS2Recomp toolchain (`ps2xAnalyzer`, `ps2xRecomp`, `ps2xRuntime`, `ps2xIOP`,
`ps2xTest`, `ps2xStudio`), including EE/IOP emulation, syscalls, and file /
memory-card I/O; and `game/overrides/` with the configuration it reads.

**Out of scope:** the copyrighted game data (disc image, `NICO.DAT`, XFF
modules, extracted assets); third-party vendored or downloaded code (raylib,
imgui, sse2neon, upstream PS2Recomp dependencies - report those upstream);
social engineering, physical access, and issues requiring a compromised host;
and issues that only reproduce with a modified build, a patched dependency, or
a game image the maintainers cannot legally obtain.

## Never send game data

**Do not attach, link, or transmit a game ISO, `NICO.DAT`, XFF dumps, or any
other extracted game content** in a report, issue, or advisory - that material
is copyrighted and sharing it is not acceptable. Describe the input instead:
file name, size, hashes, the command that consumed it, and where it failed.
Maintainers reproduce with their own legally obtained copy of the game.

## Disclosure policy

We follow coordinated disclosure: keep details private until a fix is
available. We target **90 days** from acknowledgement as the public disclosure
date, and publish the advisory with the fix if it ships sooner. If the issue is
still unresolved at 90 days we agree a revised timeline with you rather than
publish unilaterally. Once the advisory is public, you are welcome to write
about the issue.
