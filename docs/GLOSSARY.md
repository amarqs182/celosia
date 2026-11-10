# Glossary

Terms used by the celosia code base and its documentation. Each entry names
where in the tree the term lives, so a reader can go and read the source
instead of guessing. Terms that name one thing together share a row.

| Term | Meaning | Where |
|---|---|---|
| EE | Emotion Engine, the PS2's main 294 MHz MIPS IV (R5900) CPU that runs guest code, the VU0 vector coprocessor, the DMA controller and 32 MB of RAM. | `PS2Recomp/ps2xRuntime/include/ps2_runtime.h` |
| IOP | Input Output Processor, the PS2's secondary R3000A CPU with its own 2 MB RAM, servicing the disc, sound and RPC calls the EE makes over the SIF bus. | `PS2Recomp/ps2xIOP/src/emulator/core/iop_cpu.cpp` |
| GS | Graphics Synthesizer, the PS2's 4 MB VRAM frame buffer and rasteriser unit, fed by the GIF packets VU1 builds. | `PS2Recomp/ps2xRuntime/include/runtime/gs/gs_types.h` |
| VU0 | Vector Unit 0, the vector coprocessor embedded in the EE branch that computes the game's transforms, matrices and skinning. | `PS2Recomp/ps2xRecomp/src/lib/vu_translator.cpp` |
| VU1 | Vector Unit 1, the free-running vector CPU that builds the GIF display lists the GS draws, the last stage of the 3D pipeline. | `PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_core.cpp` |
| GIF | Graphics Interface, the packet based link that carries register writes and drawing commands from VU1 into the GS. | `PS2Recomp/ps2xRuntime/src/lib/gs/ps2_gif_arbiter.cpp` |
| DMA | Direct Memory Access, the EE's dedicated copy engine that moves data between RDRAM, scratchpad and the hardware units without the CPU. | `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/DMA.cpp` |
| SIF, SIF RPC | SIF is the Sub-system Interface, the DMA based bus between the EE and the IOP that the SDK exposes as the sceSif library; SIF RPC is the request and reply protocol over it that lets EE code run functions inside IOP modules. | `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/SIF.cpp`, `PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/RPC.cpp` |
| VIF | Vector Interface, the unit that unpacks VIF tags from a DMA chain and fills the VU micro memory and data memory before VU1 runs. | `PS2Recomp/ps2xRuntime/src/lib/ps2_vif1_interpreter.cpp` |
| XFF | eXecutable File Format, the relocatable module format the game ships its own code in; the port merges the boot ELF and the XFF modules into one image. | `tools/xff.py` |
| IRX | I/O RebooTable Xecutable, a relocatable IOP module that the IOP emulator loads and starts through its module loader. | `PS2Recomp/ps2xIOP/src/emulator/services/iop_module_loader.cpp` |
| ELF | Executable and Linkable Format, the object file format of the game's boot stub and of the merged image the recompiler reads. | `tools/elfmini.py` |
| static recompilation versus emulation | Static recompilation translates the guest's MIPS code into C++ ahead of time and then runs it natively, while emulation interprets the guest instructions at run time; celosia does the first. | `docs/STATUS.md` |
| native override | Game specific C++ that replaces one recompiled guest function with an equivalent native implementation, registered by address through SOTC_DECOMP_FUNCTION. | `game/overrides/sotc_decomp.cpp` |
| decompilation | Rewriting a guest function as readable C++ that keeps the original's observable behaviour, as opposed to the mechanical translation the recompiler emits. | `docs/decomp.md` |
| difftest | A differential test harness: a selected native function is run twice, once through the native code and once through the recompiled code, and their side effects are compared. | `game/overrides/sotc_decomp.h` |
| hot path | The code the profiler shows as most executed, the first place to look when optimising a frame; celosia keeps its hot lists in the lean and A/B work. | `game/lean_hot.txt` |
| lean function | A hand rewritten native copy of a hot recompiled function, built leaner than the mechanical translation so it runs faster with the same observable behaviour. | `game/overrides/lean_hot.cpp` |
| round | One profiling and optimisation iteration: profile, pick targets, rewrite or tune, difftest, then A/B measure and record the findings. | `docs/rounds.json` |
| KERNEL.XFF, MANAGER.XFF, GAMECORE.XFF | The game's three relocatable code modules, linked in memory by its own dynamic linker: start-up and imports, the manager layer, and the core game loop. | `docs/STATUS.md` |
| NICO.DAT | The multi gigabyte data file on the game disc that holds the game's assets; it is never committed and never recompiled. | `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Helpers/Support.h` |
| IOPRP300.IMG | The IOP reset image that loads the IOP's kernel modules; the port does not load it because those modules are emulated instead. | `PS2Recomp/ps2xIOP/include/ps2x/iop/iop_subsystem.h` |
| SG2IOPM1.IRX | The sound driver IOP module the game loads over SIF RPC, reimplemented natively here as the sound service. | `docs/native_audio_plan.md` |
| SCUS-974.72 | The serial number of the USA disc of Shadow of the Colossus, also the file name the port gives the extracted boot executable. | `docs/STATUS.md`, `game/scus.toml` |
| syscall | A request from guest code into the emulated kernel, dispatched by the EE runtime's syscall layer. | `PS2Recomp/ps2xRuntime/src/lib/Kernel/Syscalls/Dispatcher.cpp` |
| stub | An empty or simplified implementation of a guest function that keeps the ABI when a full implementation is missing, so the caller still runs to completion. | `PS2Recomp/ps2xRuntime/include/ps2_stubs.h` |
| TODO_NAMED | The macro used inside a stub body: it records the guest function name so the runtime can log, or later recompile, that function instead of failing silently. | `PS2Recomp/ps2xRuntime/src/lib/Kernel/Stubs/Unimplemented.cpp` |
| HLE | High level emulation, running a module's semantics as native host code rather than interpreting its machine code. | `PS2Recomp/ps2xIOP/README.md` |
| RDRAM | The PS2's 32 MB main RAM, mapped at 0x00000000 and masked with PS2_RAM_MASK in this port. | `PS2Recomp/ps2xRuntime/src/lib/ps2_memory.cpp` |
| scratchpad | The 16 KB fast memory at 0x70000000 that the VUs and DMA use as a low latency work area. | `PS2Recomp/ps2xRuntime/include/runtime/ps2_memory.h` |
| SPU2 | Sound Processor Unit 2, the PS2's sound chip with 2 MB of sample RAM, driven by the IOP's sound driver. | `PS2Recomp/ps2xIOP/src/emulator/core/iop_spu2.cpp` |
| VAG | The PS2's compressed audio sample format, a form of ADPCM used for all voice playback. | `PS2Recomp/ps2xRuntime/src/lib/ps2_audio_vag.cpp` |
| CLUT | Colour lookup table, the palette an indexed texture (PSMT4 or PSMT8) is sampled through, cached by the renderer. | `PS2Recomp/ps2xTest/gs_cache/gs_clut_cache_tests.cpp` |
| PSMCT32, PSMT8, PSMT4 | The GS texture storage modes the game uses: 32 bit colour, and 8 bit and 4 bit indices into a CLUT. | `PS2Recomp/ps2xRuntime/include/runtime/gs/` |
| MSCAL | The VIF command that starts a micro program on VU1, the way the EE hands a fresh display list to the vector unit. | `tools/model_view.py` |
| XGkick | The VU1 instruction that sends a finished GIF packet to the GS, the hand-off that ends one VU1 program's work. | `PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_lower.cpp` |
| vitaGL | The Vita implementation of OpenGL ES over the console's GXM API, the graphics layer the port renders through on PS Vita. | `PS2Recomp/ps2xRuntime/platform/vita/vitagl/celosia.patch` |
| raylib | The small open source game library that hosts the renderer, window and input work on the desktop and Vita builds. | `PS2Recomp/ps2xRuntime/platform/vita/raylib/raylib_vita.cpp` |
| MiniTest | The header only test framework in the toolchain, which carries each suite's expectations. | `PS2Recomp/ps2xTest/include/MiniTest.h` |
| CTest | The CMake test driver that runs the registered test binaries; `ctest --test-dir build` drives it here. | `PS2Recomp/ps2xTest/CMakeLists.txt` |
| FetchContent | The CMake module that downloads and builds the third-party dependencies this project fetches at configure time. | `PS2Recomp/CMakeLists.txt` |
| RAM and frame capture | The debugging practice of dumping guest RAM and rendered frames from one run so two builds can be compared offline by the tools. | `PS2Recomp/ps2xRuntime/src/lib/native/native_recorder.cpp` |
| Wander | The player character of Shadow of the Colossus, the object the camera and the player controls follow. | `game/overrides/decomp_cam126.cpp` |
| Agro | Wander's horse, a first class character whose body, bridle and cloth simulation have their own natives. | `game/overrides/decomp_bridle.cpp` |
| colossus | Each boss creature Wander must climb; the colossus LOD, geometry and collision work is named after them. | `game/overrides/decomp_charlod78.cpp` |
| NICO | The code name the game gives its own engine, seen in the `nico_` symbol prefix and in the NICO.DAT asset file. | `tools/prof129.py` |
| R36S | The handheld Linux device the port targets as its aarch64 build, tracked in its own plan document. | `docs/r36s_port_plan.md` |
| PS2X_ prefix | The prefix of the build options and environment switches that configure the toolchain and the runtime itself. | `PS2Recomp/CMakeLists.txt` |
| SOTC_ prefix | The prefix of the environment switches that configure the game specific port and its overrides at run time. | `game/overrides/sotc_native.cpp` |
| A/B flag | A switch that alternates a feature on and off inside one run, so two settings can be measured in the same session. | `game/overrides/sotc_decomp.cpp` |
| lean_hot.txt | The address list, hottest first, of the recompiled functions the lean copies are generated from. | `game/lean_hot.txt` |
| pinned_starts.txt | Function start addresses the ELF build keeps whatever its own merge of false split guesses. | `game/pinned_starts.txt` |
| extra_stubs.txt | Extra `handler@address` bindings layered on top of the generated configuration, mostly fatal error paths. | `game/extra_stubs.txt` |
| scus.toml | The generated recompiler configuration: input ELF, function map, sections, stubs and jump table settings. | `game/scus.toml` |
| vu1rec | The ahead of time recompilation of VU1 micro programs, the fast path that replaces the VU1 interpreter. | `PS2Recomp/ps2xRuntime/src/lib/vu/ps2_vu1_rec.cpp` |
| ps2x_tests | The test binary that runs the MiniTest suites over the analyzer, the recompiler and the runtime. | `PS2Recomp/ps2xTest/src/main.cpp` |
| gs_cache | The GS memory, texture and CLUT cache test suite, registered with CTest alongside the main suite. | `PS2Recomp/ps2xTest/gs_cache/` |
| VPK | The Vita package format portable installs are built into, the artefact `./celosia vita` produces for the port. | `PS2Recomp/vita/build.sh` |
