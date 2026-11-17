// gsReg (0x1c5b00, round 44): one GS register write of the 2D code, a
// (register, value) pair appended to the current packet. ~0.2% of the game
// thread recompiled in round 43's Vita3K profile (21.6 k instructions a
// frame: thousands of calls, eighteen MIPS instructions each).
//
// a0 the register (its 64-bit A+D address word), a1 the value. The packet
// state is *0x1e11a4 (a record: +0xc the write pointer, +0x1c a "closed"
// word). Closed: nothing written. Otherwise the value goes at the pointer
// and the register after it (an A+D pair is data, then address), each
// 8 bytes, the pointer moved past each; the record is read again from
// 0x1e11a4 before the second store, as the code does.
//
// Registers as the code leaves them: a3 0x1e11a4, v1 the record (or its
// +0xc on the write path), a2 the record's +0xc, v0 the closed word (or the
// pointer after the pair).
//
// sotc_native hooks gsReg (the native renderer reads the 2D writes by their
// return address) and calls what behindHook gives it: this native, its
// difftest checker or the recompiled code (SOTC_HOTLIST44=0,
// SOTC_DECOMP_SKIP=gsReg). Without the native renderer it is the table's.
#include "sotc_decomp.h"

#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

namespace decomp_hot44g
{
    constexpr uint32_t kGsReg = 0x001C5B00u;

    void gsReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!sotc_decomp::hotList44On())
            return sotc_decomp::recompiled(kGsReg)(rdram, ctx, runtime);
        sotc_decomp::gsRegWrite(rdram, ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x001C5B00u, gsReg, decomp_hot44g::gsReg);
