# Decomp workflow: a newcomer's guide

`docs/decomp.md` is the log of what has been decompiled so far (the table of
modules, functions and check counts, plus the notes on the hard cases).
This page is the how: the five steps that turn a hot recompiled function
into a hand-written native C++ function that replaces it, proven equal.

Nothing here rebuilds a PS2 binary. The game runs as recompiled C++ and a
native function takes the place of the recompiled one, by address.

## The five steps

1. Profile the running game and find the hot functions.
2. Read the function in Ghidra, the decompiler and the disassembly.
3. Write the native in `game/overrides/decomp_<system>.cpp` and register it.
4. Prove it with the differential test: 0 mismatches.
5. Re-profile on the Mac and on the Vita and check for a regression.

## Before you start

- `game/overrides/sotc_decomp.h`: the registry (`Function`, `registry()`,
  `Registrar`), the `SOTC_DECOMP_*` macros, `returnTo` and the guest-call
  helpers.
- `game/overrides/decomp_<system>.cpp`: your file. One namespace per system:
  the build is a unity build.
- `game/pinned_starts.txt` and `game/lean_hot.txt` guide the target
  selection. The first holds the function starts the ELF build keeps (read
  only: editing a line changes the function split and needs a recompile),
  the second the hot recompiled functions the lean copies keep, hottest
  first.
- `game/extra_stubs.txt` holds the extra stubs: the `handler@address`
  bindings applied on top of the generated config.

Start with a cold, small file. `decomp_time.cpp` (newlib's `mktime`),
`decomp_math.cpp` (`iosGetSinCosf`) and `decomp_sema.cpp` (the semaphore's
uncontended case) are small and barely hot, and they show the shape of a
native: read the state, do the work, leave the context as the original
leaves it, return. Cold, small files are the right place to validate the
pattern first, before you touch a hot one.

## Step 1: profile, find what is worth decompiling

`PS2X_PROFILE=<file>` turns on a sampling profiler of the guest code: 1000
times a second it reads the running EE context's pc and ra (an idle line
while the EE waits) and every 5 s rewrites the file with "pc ra samples"
lines. `PS2X_PROFILE_START=<s>` starts sampling that many seconds in, so
the profile holds the game and not the boot.

    PS2X_PROFILE=profile.txt tools/native_run.sh <out dir>
    tools/guest_profile.py profile.txt --module

`guest_profile.py --module` names the functions the EE context was sampled
in and their callers, hottest first. Pick a function that is hot and
self-contained: one or two callers, no context switch, no host state the
test cannot put back. A function called 30 000 times a frame is worth more
than one called twice a second, but a function that cannot be checked is
worth nothing at all.

## Step 2: read it in Ghidra

The project is built once: `tools/ghidra.sh import` imports
`recomp/game/SOTC.elf` and runs `tools/ghidra/ApplyRecompNames.java` as its
post-script, naming every function from `recomp/game/functions.csv` and
`recomp/game/symbols.txt`. Ghidra's names are then the recompiled code's
names, so a name in the profile is a name in Ghidra.

    tools/ghidra.sh DecompileTo.java $PWD/build/ghidra/out <name|0xaddr> ...
    tools/ghidra.sh Disasm.java 0xaddr
    tools/ghidra.sh Callers.java <name>
    tools/ghidra.sh RefsTo.java 0xaddr
    tools/ghidra.sh Containing.java 0xaddr

`DecompileTo.java` writes the decompiler's C-like output to
`build/ghidra/out/<name>.c`; `Disasm.java` prints the instructions of the
function containing an address; `Callers.java` prints every call site of a
named function; `RefsTo.java` prints every reference to an address;
`Containing.java` prints the function containing an address (the profiler
gives addresses, not names: this turns one into a name).

The decompiler is a guide, not the truth: it splits functions at stray
branches, so where it matters, read the disassembly. Two traps:

- the code may be relocated: the XFF modules' references to the boot ELF are
  patched in when the game links them, so the `lui`/`lw` pairs and the `jal`
  targets of the disassembly differ from the ones the game runs. The
  recompiled code reads them at run time (`_dynInsn`,
  `jumpTarget = READ32(site)`), and so do you:
  `sotc_decomp::relocated(rdram, luiSite, loSite)` and
  `sotc_decomp::jalTarget(rdram, site)`.
- the original's constants are the bits it used: write `0x3a83126e`, not
  `0.001f`.

## Step 3: write the native function

Create `game/overrides/decomp_<system>.cpp`. A native has the recompiled
code's signature (`PS2Runtime::RecompiledFunction`):

    void fn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)

The three arguments are the whole world it has:

- **the guest registers**: `GPR_U32(ctx, n)` and `GPR_S32(ctx, n)`, with the
  MIPS numbering (0 = zero, 1 = at, 2 = v0, 4..7 = a0..a3, 8..15 = t0..t7,
  16..23 = s0..s7, 28 = gp, 29 = sp, 30 = fp, 31 = ra). Write them back with
  `SET_GPR_U32`, `SET_GPR_S32`, `SET_GPR_U64`; `setReturnS32(ctx, value)`
  puts an integer result in v0.
- **the FPU and VU0 state**: `ctx->f[n]` (the 32 float registers, f12 is the
  first float argument), `ctx->fcr31` (the FPU's control and status word,
  whose condition bits the comparisons set) and `ctx->vu0_vf[n]` (the 32 VU0
  vector-float registers).
- **memory**: `rdram` is the 32 MB of RAM, so a raw read is
  `std::memcpy(&value, rdram + (addr & PS2_RAM_MASK), 4)`; the guest helpers
  `READ32` / `WRITE32` read and write a guest address (RAM or scratchpad).

Leave the context as the original leaves it, and finish with
`sotc_decomp::returnTo(ctx)` (`sotc_decomp.h:412`): it sets `ctx->pc` to
`ra`, the `jr ra` the recompiled code ends with. Nothing else returns from
a native.

Register it at file scope:

    SOTC_DECOMP_FUNCTION(0x00123456u, myFunction, decomp_mysystem::myFunction);

The macro (`sotc_decomp.h:883`) expands to a file-scope
`static const ::sotc_decomp::Registrar` whose constructor pushes a
`sotc_decomp::Function` into `sotc_decomp::registry()` (`sotc_decomp.h:37`,
`:62`, `:232`): the static initializer self-registers the function before
`sotc_decomp::install()` puts the registered natives in place. The `name` is
the function's name in the profile and in `SOTC_DECOMP_SKIP`, and it must be
a plain identifier (it becomes part of the registrar's variable name).

A function that returns a float uses the other macro,
`SOTC_DECOMP_FLOAT_FUNCTION(0x..., name, ns::fn)` (`sotc_decomp.h:888`): the
same registration with `returnsFloat` set. Its result goes in f0
(`ctx->f[0]`) and it reads and writes back `ctx->fcr31`, the FPU status
word as the original leaves it. For a float function v0 is scratch, so the
test does not compare it.

Two exceptions to that pattern are worth knowing:
`decomp_collcensus.cpp`'s `installAll` and `decomp_sema.cpp`'s
`installSemaFast` install their functions straight into the runtime's table
(`runtime.registerFunction`), because they wrap the table's entry or keep it
as a fallback (a test leg would signal the semaphore twice). Everything
else goes through `SOTC_DECOMP_FUNCTION`.

Rare cases go back to the recompiled code, before anything is written:
`sotc_decomp::recompiled(address)` gives the recompiled function the native
replaced, called with the state the original had there. A guest function the
native calls is `sotc_decomp::callGuest(rdram, ctx, runtime, target, ra,
sp)`.

### A minimal template

The plain pattern, in the style of `decomp_sema.cpp`:

```cpp
// game/overrides/decomp_<system>.cpp
// What the function does, its address, the round.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstring>

namespace decomp_mysystem
{
    using sotc_decomp::returnTo;

    void myFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t a0 = GPR_U32(ctx, 4);   // the first argument
        const int32_t count = GPR_S32(ctx, 5); // the second one, signed

        uint32_t value;                       // the data it works on
        std::memcpy(&value, rdram + (a0 & PS2_RAM_MASK), 4);

        SET_GPR_S32(ctx, 2, static_cast<int32_t>(value + count)); // v0

        returnTo(ctx);                        // leave it as the original does
    }
}

SOTC_DECOMP_FUNCTION(0x00123456u, myFunction, decomp_mysystem::myFunction);
```

And the float variant:

```cpp
namespace decomp_mysystem
{
    void myFloatFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        uint32_t fcr31 = ctx->fcr31;        // the FPU status word
        const float x = ctx->f[12];          // f12: the first float argument
        const float r = FPU_ADD_S(x, x);     // the same operation, in order

        ctx->f[0] = r;                      // the result: f0
        ctx->fcr31 = fcr31;                 // fcr31 as the original leaves it
        returnTo(ctx);
    }
}

SOTC_DECOMP_FLOAT_FUNCTION(0x00123478u, myFloatFunction, decomp_mysystem::myFloatFunction);
```

## Step 4: prove it, the differential test

    SOTC_DIFFTEST=myFunction tools/native_run.sh <out dir>
    SOTC_DIFFTEST=all tools/native_run.sh <out dir>

Each call of a named function runs twice on the same state: the native
first, on the live state; its result and everything it changed are kept
aside; the state is put back and the recompiled one runs (the game goes on
with its result). The two are compared:

- the registers: **v0**, **f0**, the callee-saved **s0-s7**, **fp**, **gp**,
  **sp** and **ra** (the caller-saved scratch registers may differ);
- **every byte of RAM and of the scratchpad** (the dead stack below sp
  excepted).

0 mismatches over the route is the bar: a function goes in only then.
`SOTC_DIFFTEST_FIRST` (default 200) sets how many calls are checked,
`SOTC_DIFFTEST_EVERY` (default 1000) then one in N, and `SOTC_DIFFTEST_MAX`
doubles the stride after each n checks. `SOTC_DIFFTEST_ALLREGS=1`,
`SOTC_DIFFTEST_VU0=1` and `SOTC_DIFFTEST_DRAWS=1` widen the comparison
(every register, the VU0 registers, the native renderer's 3D draws of the
two legs).

Two warnings: each check copies the memory, so the game stutters (the
scripted route's inputs, timed in VBlanks, land elsewhere: compare the
pictures from a run without the test), and the test is re-entrant (a
checked function reached from another check's leg runs recompiled,
unchecked). To put the recompiled code back for a run: `SOTC_DECOMP=0` (all
of them) or `SOTC_DECOMP_SKIP=name,name`.

## Step 5: re-profile and compare

Profile the same route again, on the Mac first and then on the Vita
(`PS2X_PROFILE=after.txt tools/native_run.sh <out dir>`, then
`tools/guest_profile.py after.txt --module`), and compare both profiles
with the "before" one:

- the function you replaced is gone from the recompiled list, or much
  smaller in it;
- the share it took of the EE's busy time is now the native's share;
- nothing else grew. A native that pushes the work into its caller, adds a
  guest call per iteration or allocates on every call can be slower than
  the recompiled code it replaced: that is a regression, and it shows up as
  the caller's share going up.

The Vita's game thread is the bottleneck, so measure there too: a native
that wins on the Mac can lose on the Vita, and the call overhead is usually
what tips the balance. Then write the round down in the table of
`docs/decomp.md` (module, functions, checks on the route, notes): that is
the log the next newcomer reads.

## Checklist

- [ ] the profile says the function is hot, and the route reaches it
- [ ] the disassembly read; the decompiler's output only a guide
- [ ] the native in `game/overrides/decomp_<system>.cpp`, in its own namespace
- [ ] `SOTC_DECOMP_FUNCTION(0x..., name, ns::fn)` at file scope (the float
      one: `SOTC_DECOMP_FLOAT_FUNCTION`)
- [ ] the context left as the original leaves it, `returnTo(ctx)` at the end
- [ ] `SOTC_DIFFTEST=name`: 0 mismatches, over the route
- [ ] re-profiled on the Mac and on the Vita: no regression
- [ ] the round written down in `docs/decomp.md`
