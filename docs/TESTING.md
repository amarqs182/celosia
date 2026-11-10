# Testing

Three layers: the unit suites under `PS2Recomp/ps2xTest`, the CTest suites
(`gs_cache`, optionally `ps2xIOP`), and the game level checks that run the
recompiled game. Build layout lives in [BUILD.md](BUILD.md), the
decompilation flow the difftest proves in [decomp.md](decomp.md).

## 1. `ps2x_tests`, the MiniTest suite

`PS2Recomp/ps2xTest` builds one binary, `ps2x_tests`, over the static library
`ps2_test_lib` (`PS2Recomp/ps2xTest/CMakeLists.txt:23`). Its `main()`
(`src/main.cpp`) registers the suites in a fixed order and calls
`MiniTest::Run()`, which returns the number of failed tests; `_Exit(res)` makes
that the process exit status. CTest does not know it: the root `CMakeLists.txt`
has no `add_test` for it, so run it by hand.

```sh
cmake -S PS2Recomp -B build -G Ninja -DPS2X_BUILD_TEST=ON -DPS2X_BUILD_STUDIO=OFF
cmake --build build
./build/ps2xTest/ps2x_tests            # exit status = failed test count
```

`-DPS2X_BUILD_ANALYZER=OFF` cannot be combined with `-DPS2X_BUILD_TEST=ON`,
because `ps2_test_lib` links `ps2_analyzer_lib`, which only exists when the
analyzer subdirectory is added (see [BUILD.md](BUILD.md)).
`-DPRINT_GENERATED_CODE=ON` prints the generated code while the tests run. On
Windows the target raises the stack to 8 MiB (`/STACK:8388608`,
`--stack,8388608` on MinGW) for the large VU/GS interpreter frames.

### Test files (`git ls-files PS2Recomp/ps2xTest/src`)

| File | What it covers |
| --- | --- |
| `code_generator_tests.cpp` | Recompiler code generator: MMIO/RDRAM fast paths, SYSCALL continuation, control flow and internal labels, branch likely/JALR/JR forms, COP0/FCR/VU2 registers, MMI/QFSRV/PCPYLD, VU0 macro mapping and masks |
| `r5900_decoder_tests.cpp` | Decoder field and flag classification: J/JAL targets, BEQ/REGIMM, load/store flags, JR/JALR, R5900 MULT and MMI MULT1 `rd` writes, COP2, LL/SC, ERET |
| `elf_analyzer_tests.cpp` | ELF analysis heuristics: symbol classification, reliable-symbol filter, entry point mapping, risk signals (MMIO/self-modifying), canonical jump table pattern |
| `pad_input_tests.cpp` | `scePad*` state machine: port/mode init, active-low button bits, EXECCMD before STABLE, pressure bytes, info/mode helpers, frame count and string helpers |
| `ps2_gs_tests.cpp` | GS and GIF: privileged registers, CSR/IMR, display environment and swap, primitive and raster behavior (fog, PRMODE, PABE, FBA, scissor), texture addressing (PSMT4/8, CSM, CLUT, TEXA/TEX1), path transfers, GIF packets, `sceGs*` helpers |
| `ps2_memory_tests.cpp` | EE memory map and DMA: uncached/aliased addresses, timers, scratchpad and VU0 windows, VIF MPG/UNPACK/control commands, GIF arbiter and DMA chains, DMAC channels, unaligned access |
| `ps2_vu_tests.cpp` | VU0 math library: vector, matrix, rotation, transform, clip and shadow helpers |
| `ps2_vu1_tests.cpp` | VU1 micro interpreter: upper/lower opcodes and masks, branch and flag pipeline latencies, ACC forwarding, EFU/WAITP/RNG, MPG upload, XGKICK through PATH1, FSSET/FCGET, reserved opcodes |
| `ps2_iop_tests.cpp` | IOP subsystem as seen from the runtime: PS2 path normalization, HLE service activation after a recognized module load, unknown SID handling |
| `ps2_sif_rpc_tests.cpp` | SIF RPC: bind/register order, remote call fallback to the stack ABI, queue roundtrip, MCSERV/DBCMAN/LIBSD services, module load and HLE activation |
| `ps2_sif_dma_tests.cpp` | SIF DMA: payload copy and `sceSifDmaStat`, private IOP heap DMA, descriptor validation and limits, DMAC cause 5 dispatch, boot-ready registers, `sceSifGetOtherData` |
| `ps2_recompiler_tests.cpp` | Recompiler driver: stub classification, entry discovery and reslicing, jump tables and entry hints, ELF parsing including DWARF and stripped ELFs, runtime call resolution, initializer fallbacks |
| `ps2_runtime_kernel_tests.cpp` | EE kernel: thread/sema layout and lifecycle, IRQ-safe syscall ids, priority and FIFO scheduling, heap and allocator, `SetSyscall` overrides and mirrors, `Copy` / `GetEntryAddress` |
| `ps2_runtime_interrupt_tests.cpp` | Scheduler and interrupts: event flag clear/transfer order, VBlank deadline and field parity, isolated IRQ and async callback stacks, semaphore wake, idle VSync stop |
| `ps2_runtime_io_tests.cpp` | IO: ROM0/ROMVER pseudo-files, memory card directory and file operations, `sceMc*` roundtrip, `sceIoctl`, CD file search and read |
| `ps2_runtime_expansion_tests.cpp` | Runtime expansion: `lookupFunction` and dispatcher entry rules, MPEG/IPU/SD stubs, `sprintf` varargs from a2/a3/t0, exception signaling, VU0 micro execution, `sceVu*` matrix math, VIF packet helpers, alpha blend and sprite XYOFFSET |
| `test_function_table.cpp` | `reset_ps2_test_function_table()`, linked as the object library `ps2_test_function_table` and installed as the global `MiniTest::BeforeEach` hook by `main.cpp` |

`test_function_table.cpp` is the one special case: its own target, not part of
`ps2_test_lib`, because it links the runtime without the generated runner
sources.

## 2. CTest

`ps2xTest/CMakeLists.txt:8` calls `include(CTest)`, so the subdirectories
registered under `BUILD_TESTING` register their suites with CTest:

```sh
ctest --test-dir build -R gs_cache --output-on-failure
ctest --test-dir build --output-on-failure                 # every registered suite
```

The `gs_cache` suites (`PS2Recomp/ps2xTest/gs_cache/CMakeLists.txt:34`) build
the runtime's GS sources into `ps2_gs_cache_backend_under_test` and register
one `add_test` per case, with the labels `gs;cache` and a 60 s timeout:

| Suite binary | Prefix | Cases |
| --- | --- | --- |
| `ps2_gs_texture_cache_tests` | `texture` | `unaligned_texture`, `unaligned_wrap`, `stale_mirror`, `page_alternation`, `flush_visibility`, `upload_visibility`, `local_copy_visibility`, `raster_visibility`, `reset_and_rebind`, `invalid_vram_size`, `reserved_psm` |
| `ps2_gs_clut_cache_tests` | `clut` | `unaligned_csm1_ct32`, `unaligned_csm1_ct16`, `unaligned_csm1_ct16s`, `wrapped_clut`, `unaligned_csm2`, `retained_palette`, `clut_uses_page_cache`, `cbp0_conditional`, `cbp1_conditional`, `reserved_cld`, `nonindexed_cld`, `tex2_reload`, `shared_contexts`, `csa_ct32`, `csa_ct16`, `csa_ct16s`, `texa_without_reload`, `palette_before_filtering`, `high_planes` |
| `ps2_gs_memory_cache_tests` | `memory` | `ct32`, `ct24`, `ct16`, `ct16s`, `t8`, `t4`, `t8h`, `t4hl`, `t4hh`, `z32`, `z24`, `z16`, `z16s`, `alias_lanes`, `physical_tag_aliases` |

The memory suite needs `PS2_GS_CACHE_BUILD_MEMORY_TESTS=ON` (the default); the
texture and clut suites are unconditional.

### The IOP suite (opt in)

`PS2X_IOP_BUILD_TESTS` (default `OFF`, `PS2Recomp/ps2xIOP/CMakeLists.txt:5`)
registers four CTest suites for the IOP emulator. `ps2xIOP` is also a
standalone project, so configure it on its own, without the shared build dir:

```sh
cmake -S PS2Recomp/ps2xIOP -B out/iop-tests -G Ninja -DPS2X_IOP_BUILD_TESTS=ON
cmake --build out/iop-tests
ctest --test-dir out/iop-tests --output-on-failure
```

That registers `ps2_iop_emulator_tests`, `ps2_iop_import_tests`,
`ps2_iop_compatibility_tests` and `ps2_iop_import_version_tests`. The same flag
on the main configure (`cmake -S PS2Recomp -B build -G Ninja
-DPS2X_IOP_BUILD_TESTS=ON`) adds them to the shared build dir, where
`ctest --test-dir build` picks them up; there it is only reachable with
`PS2X_BUILD_RUNTIME=ON`, since `ps2xIOP` is added under that option.

## 3. Sanitizers for `gs_cache`

`PS2_GS_CACHE_SANITIZERS` (`PS2Recomp/ps2xTest/gs_cache/CMakeLists.txt:10`)
builds `ps2_gs_cache_backend_under_test` and every suite linking it with
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`,
so a report is a hard failure instead of a warning:

```sh
cmake -S PS2Recomp/ps2xTest/gs_cache -B out/gs-cache-asan -G Ninja -DPS2_GS_CACHE_SANITIZERS=ON
cmake --build out/gs-cache-asan
ctest --test-dir out/gs-cache-asan --output-on-failure
```

It only applies to the `gs_cache` suites; the runtime GS sources are compiled
into that single static library, so the option covers exactly the code those
suites exercise. The configure fails with
`PS2_GS_CACHE_SANITIZERS requires a GCC/Clang sanitizer toolchain` when the
compiler is not GCC or Clang (MSVC included), so use a build tree of its own.

## 4. Runtime macro selftest

`PS2X_MACRO_SELFTEST=<millions>[x]` compares the generated code's VU0 / FPU
macros (`ps2_runtime_macros.h`) with their previous definitions, bit for bit,
on the machine that runs the game. It is read on the game thread before the
game starts (`ps2_runtime.cpp:3136` -> `ps2_macro_selftest.cpp`), so the ARMv7
forms are checked under the Vita's own float environment:

```sh
PS2X_MACRO_SELFTEST=4 ./celosia run        # 4 million random pairs per op
PS2X_MACRO_SELFTEST=4x ./celosia run       # ... plus every 32-bit pattern through the normalisation
```

The count is millions of random pairs per operation; the optional `x` suffix
adds the exhaustive 2^32 pass of the compare key, minutes instead of seconds.
Results and a timing of old / new go to stderr as `[selftest]` lines: one
`[selftest] <name> N checked, M mismatches` per operation, a
`[selftest] PASSED ...` / `FAILED ...` summary, and the rounding note when the
VU0 lanes disagree with the scalar FPU on random normal operands.

## 5. Game level checks

These run the real game and are where a runtime change is judged; the routes
and input scripts live in the scripts' own headers.

| Command | What it does |
| --- | --- |
| `./celosia autotest [seconds] [interval]` | Scripted run saving screenshots every `interval` seconds into `shots/run` and the log into `logs/autotest.log`; `PS2X_INPUT_SCRIPT` and `PS2X_SNAPSHOT_SECONDS` drive it. `./celosia play` is the same run without the capture |
| `tools/measure.sh <label> [seconds]` | The Mac measurement gate: a scripted route (title -> Load Game -> shrine -> walk out) with inputs timed in VBlanks, one summary line, log and pictures in `shots/<label>/` |
| `tools/bench.sh [label]` | Scripted New Game run; prints the average game fps per section (title / bridge / shrine) from the `[stats]` lines of `logs/autotest.log` |
| `tools/framediff.py <dir before> <dir after> [threshold]` | Compares the `gameframe_*.ppm` / `snap_*.ppm` of two `tools/measure.sh` runs; a pixel counts as changed when nothing within one pixel of the other frame is inside `threshold` (default 24) in every channel. Needs Pillow; exits 1 when a frame is flagged and writes `diff_<name>.png` |
| `tools/native_compare.py shots/run [--out dir] [--worst 24]` | Ranks the `frame_*`/`native_*` pairs of a `SOTC_NATIVE=2` run with `PS2X_SNAPSHOT_FRAMES=<n>` (emulated GS picture against the native renderer) and writes contact sheets of the worst frames with a per screen region break down |

A frame difference between two runs of the same binary is noise; the threshold
and the 1.5% flag exist for that reason (see [decomp.md](decomp.md)).

## 6. Difftest

`SOTC_DIFFTEST=<name,name|all>` runs each recompiled call of the named native
function twice on the same machine state, the native leg and the recompiled
leg, each on its own RAM and scratchpad snapshot. It is the proof that a hand
written replacement (`game/overrides/`) does what the generated code did.

```sh
SOTC_DIFFTEST=name,name ./celosia autotest 40
SOTC_DIFFTEST=all SOTC_DIFFTEST_EVERY=200 ./celosia autotest 190
```

"0 mismatches" means the native leg came out with:

- the same return registers: `v0`, `f0`, `s0` to `s7`, `fp`, `gp`, `sp`, `ra`,
  plus `pc` on the return address (a different one is "returns elsewhere");
- the same memory: every byte of RDRAM and of the scratchpad. The 64 KiB of
  dead stack below `sp` is left out by default, `SOTC_DIFFTEST_DEADSTACK=<bytes>`
  brings that much of it back, and `SOTC_DIFFTEST_IGNORE=lo-hi,...` excludes RAM
  ranges that two runs of the same code never agree on.

Reports go to stderr (`game/overrides/sotc_decomp.cpp`): one line per mismatch
with the function, its address, what differed and the values from both legs,
then the per function counter on the first check and every hundred after that:

```
[difftest] name@00123456 MISMATCH (ram 003da010 recompiled 41 native 40) a0 ... a1 ... a2 ... a3 ... f12 ... f13 ...
[difftest] name@00123456 MISMATCH (f0 recompiled 3f800000 native 3f800001) a0 ... a1 ... a2 ... a3 ... f12 ... f13 ...
[difftest] name: 100 checks, 0 mismatches (137 calls)
```

Diagnostic knobs: `SOTC_DIFFTEST_DUMP=1` prints the next differing bytes of RAM,
`SOTC_DIFFTEST_WATCH=1` reruns the native leg and logs the guest calls that
change the differing byte, `SOTC_DIFFTEST_ALLREGS=1` / `_VU0=1` widen the
register set, `SOTC_DIFFTEST_CONTROL=1` runs the same code in both legs (a
native that matches itself is not evidence). Anything other than 0 mismatches
is not proven; compare pictures from a run without it, because the checks are
slow enough to move the timed inputs.

## 7. How to add a test

MiniTest is two classes in `PS2Recomp/ps2xTest/include/MiniTest.h`:

- `MiniTest::Case(name, fn)` registers a suite; `fn` takes a `TestCase&`.
- `tc.Run(name, fn)` registers one test inside a suite, `fn` taking a
  `TestCase&`. Register tests only; `MiniTest::Run()` (called once by `main.cpp`)
  executes them in registration order.
- `MiniTest::BeforeEach(fn)` runs before every test. `main.cpp` already sets one
  to `reset_ps2_test_function_table`, so it runs without you touching it;
  `BeforeEach` is also a `TestCase` method for a per suite hook, next to
  `Before`, `After` and `AfterEach`.
- Assertions are methods on `TestCase`: `t.Equals(a, b, message)` compares with
  `==`, `t.IsTrue(condition, message)`, `t.IsFalse(condition, message)`,
  `t.IsNull(ptr, message)` (expects `nullptr`), `t.IsNotNull(ptr, message)`,
  and `t.Fail(message)`. A test with any reason prints `[Failed]` and the
  reasons, so say what was expected rather than "assertion failed".

Steps for a new test file:

1. Put it in `PS2Recomp/ps2xTest/src/` and wrap the registration in a free
   function `register_<name>_tests()` (see `r5900_decoder_tests.cpp`).
2. List the file in `ps2_test_lib` in `PS2Recomp/ps2xTest/CMakeLists.txt:23`;
   `test_function_table.cpp` is the model only when the code has to link the
   runtime without the generated runner sources.
3. Declare and call `register_<name>_tests()` in
   `PS2Recomp/ps2xTest/src/main.cpp`, keeping the order of that block.
4. Build and run `./build/ps2xTest/ps2x_tests`. Keep the test in `ps2_test_lib`
   (not `ps2_test_function_table`); CTest will not run it, since `ps2x_tests`
   has no `add_test` entry, so the suite is verified by its exit status.

## 8. Known gaps

- No golden render test: nothing starts the renderer, draws a fixed picture and
  compares against a checked in reference. The [game level checks](#5-game-level-checks)
  compare run against run, so a regression stable in every run still passes.
  Ground truth there is the emulated GS picture of a `SOTC_NATIVE=2` run.
- No end to end smoke test: `autotest` needs a built runner and a disc derived
  tree (`./celosia run`, `disc/`), so nothing boots the game headless and
  asserts a frame. `ps2x_tests` covers the pieces, never their wiring into the
  runner.
- No pytest for `tools/`: the Python scripts are hand run and checked by their
  output, with no unit test for them in `tools/`.
- No sanitizer build for the main suite: `PS2_GS_CACHE_SANITIZERS` exists only
  for `gs_cache`. `ps2x_tests`, `ps2xRuntime` and `ps2xIOP` have no ASan or
  UBSan build, so memory bugs in the runtime surface as crashes or as a failed
  difftest, not as a sanitizer report.
