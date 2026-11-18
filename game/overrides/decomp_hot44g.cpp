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

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include <ps2_recompiled_functions.h>

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

namespace decomp_hot44g_1c1538
{
    static void gsSyncRegs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        auto &mem = runtime->memory();
        constexpr uint32_t kBase = 0x00213810u;
        const uint32_t t4 = mem.read32(kBase + 0x8Cu);
        const uint32_t v0 = t4 & 1u;
        const uint32_t nextIdx = t4 ^ 1u;
        const uint32_t ptr = kBase + v0 * 56u;

        const uint64_t r00 = mem.read64(ptr + 0x18u);
        mem.write64(0x12000000u, r00);
        const uint64_t r20 = mem.read64(ptr + 0x20u);
        mem.write64(0x12000020u, r20);
        const uint64_t r90 = mem.read64(ptr + 0x28u);
        mem.write64(0x12000090u, r90);
        const uint64_t r70 = mem.read64(ptr + 0x38u);
        mem.write64(0x12000070u, r70);
        const uint64_t ra0 = mem.read64(ptr + 0x30u);
        mem.write64(0x120000A0u, ra0);
        const uint64_t r80 = mem.read64(ptr + 0x40u);
        mem.write64(0x12000080u, r80);
        const uint64_t re0 = mem.read64(ptr + 0x48u);
        mem.write64(0x120000E0u, re0);
        mem.write32(kBase + 0x8Cu, nextIdx);

        SET_GPR_U64(ctx, 2, re0);
        SET_GPR_U64(ctx, 3, r80);
        SET_GPR_U64(ctx, 4, ptr);
        SET_GPR_U64(ctx, 5, kBase);
        SET_GPR_U64(ctx, 6, 0x120000E0u);
        SET_GPR_U64(ctx, 7, 0x12000080u);
        SET_GPR_U64(ctx, 8, 0x120000A0u);
        SET_GPR_U64(ctx, 9, 0x12000020u);
        SET_GPR_U64(ctx, 10, 0x12000090u);
        SET_GPR_U64(ctx, 11, 0x12000070u);
        SET_GPR_U64(ctx, 12, nextIdx);
        SET_GPR_U64(ctx, 13, ptr);
        SET_GPR_U64(ctx, 14, ptr);
        SET_GPR_U64(ctx, 15, ptr);
        sotc_decomp::returnTo(ctx);
    }

    // kernel_sub_001C37F0 (0x001C37F0, VBlank/CSR handler) & iosSyncV (0x001C2858):
    // route GS_CSR (0x12001000) directly through runtime->memory() instead of
    // triggering >14,000 fastmem SIGSEGV traps across steady-state gameplay.
    static void vblankCsrHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint64_t rc_2 = 0, rc_3 = 0, rc_4 = 0, rc_5 = 0, rc_16 = 0, rc_29 = 0, rc_31 = 0;
        if (__builtin_expect(ctx->pc != 0x1C37F0u, 0))
        {
            switch (ctx->pc)
            {
            case 0x1C3804u: goto label_1c3804;
            case 0x1C3834u: goto label_1c3834;
            case 0x1C3858u: goto label_1c3858;
            default: ctx->pc = 0x1C37F0u; break;
            }
        }
        rc_29 = Ps2GprU64(ctx, 29);
        RC_SET_S32(rc_29, static_cast<int32_t>(ADD32(RC_U32(rc_29), 4294967280u)));
        Ps2SetGpr64(ctx, 29, RC_U64X(rc_29));
        rc_16 = Ps2GprU64(ctx, 16);
        WRITE64(ADD32(RC_U32(rc_29), 0), RC_U64(rc_16));
        rc_31 = Ps2GprU64(ctx, 31);
        WRITE64(ADD32(RC_U32(rc_29), 8), RC_U64(rc_31));
        RC_SET_U32(rc_31, 0x1C3804u);
        Ps2SetGpr64(ctx, 31, RC_U64X(rc_31));
        RC_SET_U64(rc_4, RC_U64(0) | static_cast<uint64_t>(static_cast<uint16_t>(65280u)));
        Ps2SetGpr64(ctx, 4, RC_U64X(rc_4));
        ctx->pc = 0x1E05B8u;
        if (!PS2_CALL_GUEST_KNOWN(0x1E05B8u, isceGsPutIMR_0x1e05b8, 0x1C3804u, 0x1C37FCu, DirectCall, "JAL"))
            return;
    label_1c3804:
        rc_2 = Ps2GprU64(ctx, 2);
        RC_SET_U64(rc_16, static_cast<uint64_t>(RC_U64(rc_2)) + static_cast<uint64_t>(RC_U64(0)));
        Ps2SetGpr64(ctx, 16, rc_16);
        RC_SET_S32(rc_2, static_cast<int32_t>(30u << 16));
        RC_SET_S32(rc_2, static_cast<int32_t>(ADD32(RC_U32(rc_2), 4440)));
        Ps2SetGpr64(ctx, 2, RC_U64X(rc_2));
        RC_SET_S32(rc_4, static_cast<int32_t>(30u << 16));
        RC_SET_U64(rc_3, READ64(ADD32(RC_U32(rc_2), 0)));
        RC_SET_S32(rc_4, static_cast<int32_t>(READ32(ADD32(RC_U32(rc_4), 4460))));
        Ps2SetGpr64(ctx, 4, RC_U64X(rc_4));
        RC_SET_S64(rc_3, static_cast<int64_t>(RC_S64(rc_3)) + static_cast<int64_t>(1));
        Ps2SetGpr64(ctx, 3, rc_3);
        WRITE64(ADD32(RC_U32(rc_2), 0), RC_U64(rc_3));
        if (RC_U64X(rc_4) == RC_U64(0))
        {
            RC_SET_S32(rc_2, static_cast<int32_t>(4608u << 16));
            Ps2SetGpr64(ctx, 2, RC_U64X(rc_2));
            goto label_1c3838;
        }
        {
            const uint32_t jumpTarget = RC_U32(rc_4);
            RC_SET_U32(rc_31, 0x1C3834u);
            Ps2SetGpr64(ctx, 31, RC_U64X(rc_31));
            ctx->pc = jumpTarget;
            if (!PS2_CALL_GUEST(jumpTarget, 0x1C3834u, 0x1C382Cu, IndirectCall, "JALR"))
                return;
        }
    label_1c3834:
        RC_SET_S32(rc_2, static_cast<int32_t>(4608u << 16));
        Ps2SetGpr64(ctx, 2, RC_U64X(rc_2));
    label_1c3838:
        RC_SET_S32(rc_5, static_cast<int32_t>(ADD32(RC_U32(0), 2)));
        Ps2SetGpr64(ctx, 5, RC_U64X(rc_5));
        rc_2 = Ps2GprU64(ctx, 2);
        RC_SET_U64(rc_2, RC_U64(rc_2) | static_cast<uint64_t>(static_cast<uint16_t>(4096u)));
        Ps2SetGpr64(ctx, 2, rc_2);
        rc_16 = Ps2GprU64(ctx, 16);
        RC_SET_U64(rc_4, static_cast<uint64_t>(RC_U64(rc_16)) + static_cast<uint64_t>(RC_U64(0)));
        Ps2SetGpr64(ctx, 4, rc_4);
        RC_SET_U64(rc_3, runtime->memory().read64(0x12001000u));
        RC_SET_U64(rc_3, RC_U64(rc_3) | RC_U64X(rc_5));
        Ps2SetGpr64(ctx, 3, rc_3);
        runtime->memory().write64(0x12001000u, RC_U64(rc_3));
        RC_SET_U32(rc_31, 0x1C3858u);
        Ps2SetGpr64(ctx, 31, RC_U64X(rc_31));
        ctx->pc = 0x1E05B8u;
        if (!PS2_CALL_GUEST_KNOWN(0x1E05B8u, isceGsPutIMR_0x1e05b8, 0x1C3858u, 0x1C3850u, DirectCall, "JAL"))
            return;
    label_1c3858:
        ctx->cop0_status |= 0x10000;
        rc_29 = Ps2GprU64(ctx, 29);
        RC_SET_U64(rc_16, READ64(ADD32(RC_U32(rc_29), 0)));
        Ps2SetGpr64(ctx, 16, rc_16);
        RC_SET_U64(rc_2, static_cast<uint64_t>(RC_U64(0)) + static_cast<uint64_t>(RC_U64(0)));
        Ps2SetGpr64(ctx, 2, RC_U64X(rc_2));
        RC_SET_U64(rc_31, READ64(ADD32(RC_U32(rc_29), 8)));
        Ps2SetGpr64(ctx, 31, rc_31);
        {
            const uint32_t jumpTarget = RC_U32(rc_31);
            RC_SET_S32(rc_29, static_cast<int32_t>(ADD32(RC_U32(rc_29), 16)));
            Ps2SetGpr64(ctx, 29, RC_U64X(rc_29));
            ctx->pc = jumpTarget;
            return;
        }
    }

    static void iosSyncV(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint64_t rc_2 = 0, rc_3 = 0, rc_4 = 0, rc_16 = 0, rc_29 = 0, rc_31 = 0;
        if (__builtin_expect(ctx->pc != 0x1C2858u, 0))
        {
            switch (ctx->pc)
            {
            case 0x1C2870u: goto label_1c2870;
            case 0x1C2878u: goto label_1c2878;
            default: ctx->pc = 0x1C2858u; break;
            }
        }
        rc_29 = Ps2GprU64(ctx, 29);
        RC_SET_S32(rc_29, static_cast<int32_t>(ADD32(RC_U32(rc_29), 4294967280u)));
        Ps2SetGpr64(ctx, 29, RC_U64X(rc_29));
        rc_4 = Ps2GprU64(ctx, 4);
        RC_SET_S32(rc_2, static_cast<int32_t>(ADD32(RC_U32(rc_4), 1)));
        Ps2SetGpr64(ctx, 2, RC_U64X(rc_2));
        rc_16 = Ps2GprU64(ctx, 16);
        WRITE64(ADD32(RC_U32(rc_29), 0), RC_U64(rc_16));
        {
            const bool branch_taken = (RC_S32(rc_2) <= 0);
            rc_31 = Ps2GprU64(ctx, 31);
            WRITE64(ADD32(RC_U32(rc_29), 8), RC_U64(rc_31));
            if (branch_taken)
                goto label_1c2880;
        }
        RC_SET_U64(rc_16, static_cast<uint64_t>(RC_U64X(rc_2)) + static_cast<uint64_t>(RC_U64(0)));
        Ps2SetGpr64(ctx, 16, RC_U64X(rc_16));
    label_1c2870:
        RC_SET_U32(rc_31, 0x1C2878u);
        Ps2SetGpr64(ctx, 31, RC_U64X(rc_31));
        rc_16 = Ps2GprU64(ctx, 16);
        RC_SET_S32(rc_16, static_cast<int32_t>(ADD32(RC_U32(rc_16), 4294967295u)));
        Ps2SetGpr64(ctx, 16, RC_U64X(rc_16));
        ctx->pc = 0x1C7E98u;
        if (!PS2_CALL_GUEST_KNOWN(0x1C7E98u, waitVscheduler_0x1c7e98, 0x1C2878u, 0x1C2870u, DirectCall, "JAL"))
            return;
    label_1c2878:
        rc_16 = Ps2GprU64(ctx, 16);
        if (RC_U64(rc_16) != RC_U64(0))
        {
            if (PS2_CHECKPOINT_DUE_AT(0x1C2870u))
                return;
            goto label_1c2870;
        }
    label_1c2880:
        RC_SET_S32(rc_3, static_cast<int32_t>(4608u << 16));
        rc_29 = Ps2GprU64(ctx, 29);
        RC_SET_U64(rc_16, READ64(ADD32(RC_U32(rc_29), 0)));
        Ps2SetGpr64(ctx, 16, rc_16);
        RC_SET_U64(rc_3, RC_U64X(rc_3) | static_cast<uint64_t>(static_cast<uint16_t>(4096u)));
        Ps2SetGpr64(ctx, 3, RC_U64X(rc_3));
        RC_SET_U64(rc_31, READ64(ADD32(RC_U32(rc_29), 8)));
        Ps2SetGpr64(ctx, 31, rc_31);
        RC_SET_U64(rc_2, runtime->memory().read64(0x12001000u));
        RC_SET_U64(rc_2, RC_U64(rc_2) >> 13);
        RC_SET_U64(rc_2, RC_U64(rc_2) & static_cast<uint64_t>(static_cast<uint16_t>(1u)));
        RC_SET_U64(rc_2, RC_U64X(rc_2) << 32);
        RC_SET_S64(rc_2, RC_S64(rc_2) >> 32);
        Ps2SetGpr64(ctx, 2, rc_2);
        {
            const uint32_t jumpTarget = RC_U32(rc_31);
            RC_SET_S32(rc_29, static_cast<int32_t>(ADD32(RC_U32(rc_29), 16)));
            Ps2SetGpr64(ctx, 29, RC_U64X(rc_29));
            ctx->pc = jumpTarget;
            return;
        }
    }
}

SOTC_DECOMP_FUNCTION(0x001C1538u, kernel_sub_001C1538, decomp_hot44g_1c1538::gsSyncRegs);
SOTC_DECOMP_COROUTINE(0x001C37F0u, kernel_sub_001C37F0, decomp_hot44g_1c1538::vblankCsrHandler);
SOTC_DECOMP_RESUME(0x001C3804u, kernel_sub_001C37F0, r804, decomp_hot44g_1c1538::vblankCsrHandler);
SOTC_DECOMP_RESUME(0x001C3834u, kernel_sub_001C37F0, r834, decomp_hot44g_1c1538::vblankCsrHandler);
SOTC_DECOMP_RESUME(0x001C3858u, kernel_sub_001C37F0, r858, decomp_hot44g_1c1538::vblankCsrHandler);
SOTC_DECOMP_COROUTINE(0x001C2858u, iosSyncV, decomp_hot44g_1c1538::iosSyncV);
SOTC_DECOMP_RESUME(0x001C2870u, iosSyncV, r870, decomp_hot44g_1c1538::iosSyncV);
SOTC_DECOMP_RESUME(0x001C2878u, iosSyncV, r878, decomp_hot44g_1c1538::iosSyncV);
SOTC_DECOMP_FUNCTION(0x001C5B00u, gsReg, decomp_hot44g::gsReg);
