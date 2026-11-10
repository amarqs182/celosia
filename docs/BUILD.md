# Building celosia

This page covers the three builds in this repository: the full game on macOS
(Apple Silicon), the PS2Recomp toolchain with its test suite (no game data
needed), and the PS Vita VPK.

Every command and option name below was copied from the sources listed in
[Sources](#sources).

## Prerequisites

### Tools

| Tool | Why it is needed | Install |
| --- | --- | --- |
| `cmake` | configures the build; `PS2Recomp/CMakeLists.txt:1` requires 3.21 or newer | macOS: `brew install cmake`; Debian/Ubuntu: `apt install cmake` |
| `ninja` | the generator used by every command on this page (`-G Ninja`) | `brew install ninja`; Debian/Ubuntu: `apt install ninja-build` |
| `python3` | 3.10 or newer; the capture and link tools under `tools/` | `brew install python`; Debian/Ubuntu: `apt install python3` |
| `bsdtar` | extracts the disc image during `./celosia setup --iso` | ships with macOS; Debian/Ubuntu: `apt install libarchive-tools` |
| `pkg-config` | locates the FFmpeg development libraries | `brew install pkg-config`; Debian/Ubuntu: `apt install pkg-config` |
| `ffmpeg` | MPEG video decoding in the runtime | `brew install ffmpeg`; Debian/Ubuntu: `apt install libavcodec-dev libavformat-dev libavutil-dev libswresample-dev libswscale-dev` |

The `celosia` script checks the first five by name and stops with a message
naming the install command when one is missing (`check_tools` in the
`celosia` script).

The FFmpeg module names are the ones `pkg-config` is asked for:
`libavcodec`, `libavformat`, `libavutil`, `libswresample` and `libswscale`
(`PS2Recomp/ps2xRuntime/CMakeLists.txt:392-399`). On Windows no system
FFmpeg is needed: the build downloads MSVC prebuilt import libraries and
stages the matching DLLs next to the executables instead
(`PS2Recomp/ps2xRuntime/CMakeLists.txt:306-426`).

### Compilers

C++20 is required (`PS2Recomp/ps2xIOP/CMakeLists.txt:36`,
`PS2Recomp/ps2xTest/CMakeLists.txt:5-6`). `PS2Recomp/README.md` lists "C++20
compiler (currently tested mainly with MSVC)". Because the Windows FFmpeg
path links MSVC prebuilt `.lib` files, treat MSVC as the expected Windows
toolchain; other Windows toolchains were not checked for this page.

### For the PS Vita only

`VITASDK` must point at a VitaSDK installation:

```sh
export VITASDK=~/vitasdk
```

`./celosia vita` defaults `VITASDK` to `$HOME/vitasdk` and fails with
"VitaSDK not found (set VITASDK)" when `$VITASDK/share/vita.toolchain.cmake`
is missing (`cmd_vita` in the `celosia` script).
`PS2Recomp/CMakeLists.txt:3-9` also picks the
toolchain file up automatically from `$ENV{VITASDK}` when no
`CMAKE_TOOLCHAIN_FILE` is given.

### Game data

You need **your own** disc image of *Shadow of the Colossus* (USA),
serial `SCUS-97472` (also written `SCUS-974.72`; the boot ELF inside the
disc is named `SCUS_974.72`), as an `.iso` or as a `.zip` containing one.
No game data is included in this repository. The pipeline looks for
`SCUS_974.72`, `KERNEL.XFF`, `MANAGER.XFF`
and `GAMECORE.XFF` in the extracted disc (`check_disc` in the `celosia`
script).

### Disk space and network

About 10 GB of free disk space and an internet connection are needed for the
first build, because CMake downloads raylib, imgui and sse2neon
(`README.md`, Requirements section).

## Three ways to build

### (a) Full game on macOS (Apple Silicon)

```sh
git clone git@github.com:HenryKun55/celosia.git
cd celosia
./celosia setup --iso "/path/to/Shadow of the Colossus (USA).iso"
./celosia run
```

`setup` extracts the image into `./disc`, configures CMake in `build/cmake`,
recompiles the boot ELF and each module, and compiles the VU1 microcode. It
takes a while the first time and briefly opens the game window a few times
while it captures each module. `run` plays the game (log: `logs/play.log`).

The `--iso` argument also accepts a `.zip` that contains one `.iso`.

### (b) Toolchain and tests, without the game (Linux or Windows)

```sh
cmake -S PS2Recomp -B build -G Ninja -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF
cmake --build build
```

This builds the PS2Recomp toolchain (`ps2xRecomp`, `ps2xRuntime`,
`ps2xIOP`, `ps2xAnalyzer`) and the test suite. No disc image is involved.

> **The `-DPS2X_BUILD_ANALYZER=OFF` above conflicts with `-DPS2X_BUILD_TEST=ON`.**
> The test library links `ps2_analyzer_lib`
> (`PS2Recomp/ps2xTest/CMakeLists.txt:50-54,80-85`), and that target only
> exists when `ps2xAnalyzer` is added to the build
> (`PS2Recomp/CMakeLists.txt:94-96`). With the analyzer off, the configure
> step fails with a "target was not found" error for `ps2_analyzer_lib`.
> Drop that one flag to get a configuring build:
>
> ```sh
> cmake -S PS2Recomp -B build -G Ninja -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_STUDIO=OFF
> cmake --build build
> ```

The build outputs follow the same layout as the `celosia` script expects:
the runner at `build/ps2xRuntime/ps2EntryRunner` and the recompiler at
`build/ps2xRecomp/ps2_recomp` (the `RUNNER` and `RECOMP` variables of the
`celosia` script).

Running the tests:

- `build/ps2xTest/ps2x_tests` is the MiniTest suite (`ps2xTest`).
- `ctest --test-dir build` runs the registered suites: the `gs_cache.*`
  tests (`PS2Recomp/ps2xTest/gs_cache/CMakeLists.txt:39`, added because
  `ps2xTest` includes CTest) and, with `-DPS2X_IOP_BUILD_TESTS=ON`, the
  `ps2_iop_emulator_tests`, `ps2_iop_import_tests`,
  `ps2_iop_compatibility_tests` and `ps2_iop_import_version_tests`
  (`PS2Recomp/ps2xIOP/CMakeLists.txt:48-67`).

On Windows the FFmpeg libraries are downloaded as MSVC prebuilts during the
build, so the first configure and build need the network too.

### (c) PS Vita VPK

```sh
export VITASDK=~/vitasdk
./celosia setup --iso "/path/to/Shadow of the Colossus (USA).iso"   # once, for the recompiled code
./celosia vita
```

The VPK is written to `build/vita/ps2xRuntime/ps2EntryRunner.vpk`
(`cmd_vita` in the `celosia` script). `./celosia vita` needs the recompiled code that
`./celosia setup` produces; without it it stops with "no recompiled code yet
(run: ./celosia setup)" (`cmd_vita`). The first Vita build also clones
vitaGL at a pinned revision and builds it with this port's patch
(`cmd_vita`).

Install the `.vpk` (VitaShell, or File > Install in Vita3K) and copy the
extracted disc to `ux0:data/celosia/disc/`. Saves go to
`ux0:data/celosia/memcards/`, logs to `ux0:data/celosia/log.txt` and
`err.txt`, and settings to `ux0:data/celosia/env.txt`.

## CMake options

The options below are the ones declared in the build files. The default
column is the value in the `option()` call; the Android column is the value
the top-level file forces on an Android build.

| Option | Default | Effect | Declared in |
| --- | --- | --- | --- |
| `PS2X_BUILD_RECOMP` | `ON` | Build `ps2xRecomp`, the MIPS R5900 to C++ recompiler. | `PS2Recomp/CMakeLists.txt:19` |
| `PS2X_BUILD_RUNTIME` | `ON` | Build `ps2xRuntime` and `ps2xIOP` (the EE runtime, the IOP emulator and the runner). | `PS2Recomp/CMakeLists.txt:20` |
| `PS2X_BUILD_ANALYZER` | `ON` | Build `ps2xAnalyzer`, the ELF/XFF scanner that writes the TOML config. | `PS2Recomp/CMakeLists.txt:21` |
| `PS2X_BUILD_TEST` | `ON` | Build `ps2xTest`, the MiniTest suite and the GS-cache tests. Needs the recomp, analyzer and runtime targets to be on. | `PS2Recomp/CMakeLists.txt:22` |
| `PS2X_BUILD_STUDIO` | `ON` | Build `ps2xStudio`, the imgui GUI. | `PS2Recomp/CMakeLists.txt:23` |
| `PS2X_IOP_BUILD_TESTS` | `OFF` | Build the IOP emulator smoke tests and register them with CTest. Only reachable with `PS2X_BUILD_RUNTIME=ON`, since `ps2xIOP` is added under that option. | `PS2Recomp/ps2xIOP/CMakeLists.txt:5` |
| `PRINT_GENERATED_CODE` | `OFF` | Print the generated code while the tests run; adds the `PRINT_GENERATED_CODE` define to `ps2x_tests` and `ps2_test_lib`. Only reachable with `PS2X_BUILD_TEST=ON`. | `PS2Recomp/ps2xTest/CMakeLists.txt:66` |
| Android forcing | n/a | On an Android target the top-level file sets `PS2X_BUILD_RECOMP`, `PS2X_BUILD_ANALYZER`, `PS2X_BUILD_TEST` and `PS2X_BUILD_STUDIO` to `OFF` and builds the runtime only. These are plain variables set for that configure, so they win over a `-D` value passed on the command line. | `PS2Recomp/CMakeLists.txt:25-31` |

`PS2X_BUILD_RUNTIME` is not forced on Android; only the four options above
are. Each option is read once, in the `if()` blocks that add the matching
subdirectory (`PS2Recomp/CMakeLists.txt:85-104`).

Other options worth knowing about:

- `PS2X_ENABLE_FFMPEG` (`ON` by default, `OFF` on Android) enables the
  FFmpeg-backed MPEG video decoding. With it off, the runtime prints
  "FFmpeg disabled; MPEG video decode falls back to stub frames" and builds
  without FFmpeg (`PS2Recomp/ps2xRuntime/CMakeLists.txt:295-305`).
- `PS2X_RUNNER_DIRS` is the list of directories with `ps2EntryRunner`
  sources. `./celosia` sets it to `build/runner`, `build/vu1rec` and
  `game/overrides` (`cmd_configure` in the `celosia` script); the default is
  `PS2Recomp/ps2xRuntime/src/runner`
  (`PS2Recomp/ps2xRuntime/CMakeLists.txt:515-517`).
- `CELOSIA_CMAKE_ARGS` and `CELOSIA_VITA_CMAKE_ARGS` add extra arguments to
  the two configure steps of the `celosia` script (`README.md`, Commands).

## Build directory layout

`./celosia` keeps everything it generates under `build/` (`CELOSIA_BUILD`,
default `./build`). The important paths are:

```
build/                       # CELOSIA_BUILD (default ./build)
|-- cmake/                   # the configured CMake build tree (Ninja)
|   |-- _deps/               # dependencies fetched by CMake FetchContent
|   |-- ps2xRecomp/          # ps2_recomp, the recompiler
|   |-- ps2xRuntime/         # ps2EntryRunner, the game runner
|   |-- ps2xIOP/             # the IOP emulator
|   |-- ps2xAnalyzer/        # the ELF/XFF analyzer
|   `-- ps2xTest/            # ps2x_tests, the test suite
|-- runner/                  # recompiled EE code (generated .cpp/.h)
|-- recomp/                  # recompiler inputs and outputs per stage
`-- vu1rec/                  # recompiled VU1 microcode (generated)
```

- `build/cmake` is where the `cmake -S PS2Recomp -B build/cmake` configure
  happens (`cmd_configure` in the `celosia` script).
- `build/cmake/_deps` is the FetchContent base directory. It sits under the
  binary directory because the tree does not override
  `FETCHCONTENT_BASE_DIR`; it holds the fetched raylib, imgui, SDL2,
  elfio, toml11, fmt, libdwarf, rabbitizer, nlohmann_json and sse2neon
  sources.
- `build/runner` and `build/vu1rec` are the generated-code directories: the
  recompiled EE code and the recompiled VU1 microcode
  (the `RUNNER_DIR` and `VU1REC_DIR` variables of the `celosia` script). Both are passed to CMake through `PS2X_RUNNER_DIRS`.
- `build/recomp` holds the recompiler inputs and outputs per stage
  (`scus/`, `game/`: the TOML config, the function map and the generated
  code of each stage) (the `WORK` variable of the `celosia` script).

Related paths the same script uses: `build/vu1` (the VU1 microcode images
dumped while playing), `build/captures` (the RAM dumps taken when each
module starts), `build/vita` (the Vita build tree),
`build/vitagl-src` (the vitaGL checkout) and `build/iso` (a temporary
folder used while unpacking a `.zip`).

## Troubleshooting

### "missing ffmpeg dev libs" / `pkg_check_modules` fails

On Linux and macOS the runtime asks `pkg-config` for `libavcodec`,
`libavformat`, `libavutil`, `libswresample` and `libswscale`
(`PS2Recomp/ps2xRuntime/CMakeLists.txt:392-399`). Install the matching
development packages (`libavcodec-dev` and friends on Debian/Ubuntu,
`brew install ffmpeg` on macOS) together with `pkg-config`, and make sure
`pkg-config --exists libavcodec` succeeds. To build without FFmpeg at all,
configure with `-DPS2X_ENABLE_FFMPEG=OFF`; the runtime then decodes MPEG
video into stub frames.

### `VITASDK` unset

Set `VITASDK` to the VitaSDK root, so that
`$VITASDK/share/vita.toolchain.cmake` exists:

```sh
export VITASDK=~/vitasdk
```

Without it `./celosia vita` stops with "VitaSDK not found (set VITASDK)"
(`cmd_vita` in the `celosia` script), and the Vita configure step has no
cross-compiler.

### The CMake cache points at a different source tree

CMake records the source directory in `CMAKE_HOME_DIRECTORY` inside
`CMakeCache.txt`. If the repository was moved or copied, or a build
directory is reused from another checkout or another machine, that cached
path no longer matches and CMake refuses to configure against it. The
repository checks for exactly this in `PS2Recomp/vita/build.sh:62-67` and
recreates the build directory when it finds a stale cache. The fix is the
same here: remove the build tree (`./celosia clean`, or delete
`build/cmake`) and configure again from the current checkout.

### The first build is big and needs the network

Expect roughly 10 GB of free disk space and an internet connection for the
first build: CMake downloads raylib, imgui and sse2neon
(`README.md`, Requirements). The downloaded sources land in
`build/cmake/_deps`, and the generated game code and the object files take
the rest. Later builds reuse the fetched sources and only need the network
again if a dependency's pinned revision changes.

## Cleaning up

```sh
./celosia clean
```

`clean` removes the whole build directory (`rm -rf "$BUILD"`, `cmd_clean` in
the `celosia` script),
which is `build/` by default and `CELOSIA_BUILD` otherwise. That deletes the
configured CMake tree, the fetched dependencies, the recompiled EE and VU1
code, the module captures and the Vita build.

It does **not** touch the extracted disc (`./disc`), the logs (`./logs`) or
the memory card images (`./memcards`). The next `./celosia setup` starts
again from the CMake configure step and downloads the dependencies anew.

## Sources

- `README.md` (Requirements, Quick start, PS Vita, Commands)
- `PS2Recomp/README.md` (Requirements, Build)
- `PS2Recomp/CMakeLists.txt` (top-level options, Android forcing, VitaSDK)
- `PS2Recomp/ps2xIOP/CMakeLists.txt` (`PS2X_IOP_BUILD_TESTS`)
- `PS2Recomp/ps2xTest/CMakeLists.txt` (`PRINT_GENERATED_CODE`)
- `PS2Recomp/ps2xRuntime/CMakeLists.txt` (FFmpeg, `PS2X_RUNNER_DIRS`)
- `PS2Recomp/vita/build.sh` (stale CMake cache check)
- `celosia` (subcommands, tool checks, directory layout, `clean`)
