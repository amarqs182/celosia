// Round P2 (SOTC_P2PTCL, A/B p2ptcl; off by default): doit_particle_manager's five layer passes as one host pass
// (decomp_hot47e.cpp; upstream commit 4752b4f, without the 32-byte Vita prefetch worker).
//
// SOTC_P2PTCL_CHECK=1: every layers pass runs both ways from the same state (the original first, its writes kept, the
// state put back, the host pass, compared node by node and the budget counter; every SOTC_P2PTCL_RAMCHECK frames (default
// 120) all 32 MB of main RAM compared, the dead stack below sp apart), and the registers the original leaves that the host
// pass does not are counted by register. SOTC_P2PTCL_LOG=<frames>: [p2ptcl] lines. SOTC_P2PTCL_LAYERS=0: the original
// layers pass.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "sotc_decomp.h"

namespace p2ptcl
{
    inline int envI(const char *name, int def)
    {
        const char *e = std::getenv(name);
        return e ? std::atoi(e) : def;
    }
    inline bool on()
    {
        return sotc_decomp::p2ptclOn();
    }
    inline int checkOn()
    {
        static const int v = envI("SOTC_P2PTCL_CHECK", 0);
        return v;
    }
    inline int logEvery()
    {
        static const int v = envI("SOTC_P2PTCL_LOG", 0);
        return v;
    }
    inline bool layersOn()
    {
        static const bool v = envI("SOTC_P2PTCL_LAYERS", 1) != 0;
        return v;
    }

    // Counters (the game thread's), printed by frameEnd.
    struct Stats
    {
        uint64_t passes = 0, fallbacks = 0, items = 0, gds = 0, us = 0;
        uint64_t checks = 0, checkNodes = 0, checkDiffer = 0, counterDiffer = 0, ramChecks = 0, ramDiffer = 0, ramDead = 0, ramSkipped = 0;
        uint64_t regDiffer[40] = {}; // the 32 GPRs, then f0, fcr31, VU0 (any vf), ACC, Q, other
        uint64_t checkFallback = 0;
    };
    inline Stats &stats()
    {
        static Stats s;
        return s;
    }
    inline void frameEnd()
    {
        const int every = logEvery();
        if (every <= 0)
            return;
        static uint64_t frames = 0;
        if (++frames % static_cast<uint64_t>(every) != 0u)
            return;
        const Stats &st = stats();
        std::fprintf(stderr,
                     "[p2ptcl] frame %llu: on %d; passes %llu (fallbacks %llu), gds %llu, items %llu, host %.2f ms; "
                     "checks %llu (nodes %llu, differ %llu, countDiffer %llu, fallback %llu, ram %llu differ %llu dead %llu skipped %llu)\n",
                     (unsigned long long)frames, on() ? 1 : 0, (unsigned long long)st.passes, (unsigned long long)st.fallbacks,
                     (unsigned long long)st.gds, (unsigned long long)st.items, st.us / 1000.0,
                     (unsigned long long)st.checks, (unsigned long long)st.checkNodes, (unsigned long long)st.checkDiffer,
                     (unsigned long long)st.counterDiffer, (unsigned long long)st.checkFallback,
                     (unsigned long long)st.ramChecks, (unsigned long long)st.ramDiffer, (unsigned long long)st.ramDead,
                     (unsigned long long)st.ramSkipped);
    }
}
