#pragma once
// Round A9 sweep (SOTC_A9SWEEP, default on; A/B name a9sweep): the runtime's side,
// adapted for R36S aarch64 (keeps static-guard hoisting kHoist, scheduler caches
// kCache, and idle-zone skip kZone; omits ARMv7 software division kDiv and kMemcmp).
//
// The switch itself is the game's (sotc_decomp::g_switch[kA9Sweep], flipped frame by frame by an interleaved A/B):
// the game points g_flag at that byte when it installs (a9sweep::init). Until then the runtime reads a constant 0
// (the original code).
#include <cstdint>

namespace ps2_a9
{
    inline const signed char kOff = 0;
    inline const signed char kOn = 1;
    inline const signed char *g_flag = &kOff;
    // The sweep's parts, for an A/B of one part (SOTC_A9SWEEP_AB_PARTS, a mask, default all: the parts that follow the
    // switch; the others follow g_fixed, SOTC_A9SWEEP's own value, set by a9sweep::init; off until then like g_flag).
    enum : unsigned
    {
        kHoist = 1u,  // function-local static guards hoisted (a9sweep.h A9S_VAL, members instead of statics)
        kCache = 8u,  // invocationStackTop's direct-mapped answers, thread()'s 8-way cache
        kZone = 16u,  // GameZone / dispatch zones not stored while nothing reads them (no TLS lookup)
        kAllParts = kHoist | kCache | kZone,
    };
    inline unsigned g_abParts = kAllParts;
    inline const signed char *g_fixed = &kOff;
    inline bool on() { return *g_flag > 0; }
    inline bool on(unsigned part) { return *((g_abParts & part) ? g_flag : g_fixed) > 0; }
}
