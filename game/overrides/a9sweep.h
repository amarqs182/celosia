#pragma once
// Round A9 sweep (SOTC_A9SWEEP, default on; A/B name a9sweep): A9 codegen costs in hot code, without a change of behaviour.
//
// 1. Function-local statics with a dynamic initialiser (`static const bool x = getenv(...)`, `static T y = init()`): GCC
//    guards each with a load of the guard word and a `dmb ish` on every pass on ARMv7 (`ldr; dmb ish; lsls; bpl`), and
//    the call to __cxa_guard_acquire the first time. A hot function with several pays them on every call. Here those
//    values are hoisted: a plain global per value (A9S_HOIST, at namespace scope), set once by a9sweep::init() when the
//    overrides install (after env.txt is in the environment: ps2_vita_runtime.cpp's LogToFile sets it before main), read
//    with one load. The original static stays as the switch's off path (A9S_VAL), so an interleaved A/B compares the two.
//    The same expression is evaluated both ways from the same environment (nothing sets these variables later: the only
//    setenv after start is SOTC_DIFFTEST's); SOTC_A9SWEEP_LOG=1 prints every hoisted value at init and, the first time
//    each original static is initialised (the off path), its value and whether it differs ("[a9sweep]" lines).
// 2. The runtime's side (PS2Recomp ps2_a9.h): 64-bit divisions and hash-table walks in the scheduler (the A9 has no
//    divide instruction). ps2_a9::g_flag points at this switch's byte once init() ran.
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include "sotc_decomp.h"
#include "runtime/ps2_a9.h"

namespace a9sweep
{
    // A part of the sweep (ps2_a9.h's parts): the switch, or SOTC_A9SWEEP's fixed value when SOTC_A9SWEEP_AB_PARTS leaves
    // the part out of the A/B. on() = the hoisted statics' part.
    inline bool onPart(unsigned part)
    {
        if (__builtin_expect((ps2_a9::g_abParts & part) != 0u, 1))
            return sotc_decomp::switchOn(sotc_decomp::kA9Sweep);
        return *ps2_a9::g_fixed > 0;
    }
    inline bool on() { return onPart(ps2_a9::kHoist); }

    // A registered initialiser of hoisted values (a namespace-scope object's constructor, before main: no guard at use).
    struct Hoist
    {
        void (*fn)();
        const Hoist *next;
        explicit Hoist(void (*f)());
    };
    inline const Hoist *g_head = nullptr;
    inline Hoist::Hoist(void (*f)()) : fn(f), next(g_head) { g_head = this; }
    inline bool g_log = false;
    inline bool g_ready = false;

    template <class T>
    inline void logNew(const char *name, T v)
    {
        if (!g_log)
            return;
        if constexpr (std::is_floating_point_v<T>)
            std::fprintf(stderr, "[a9sweep] new %s=%g\n", name, static_cast<double>(v));
        else
            std::fprintf(stderr, "[a9sweep] new %s=%lld\n", name, static_cast<long long>(v));
    }
    // The off path's static, the first time it is initialised: logged with the hoisted value it must equal.
    template <class T>
    inline T old(const char *name, T v, T hoisted)
    {
        if (g_log || (g_ready && !(v == hoisted)))
        {
            if constexpr (std::is_floating_point_v<T>)
                std::fprintf(stderr, "[a9sweep] old %s=%g%s\n", name, static_cast<double>(v), v == hoisted ? "" : " DIFFERS");
            else
                std::fprintf(stderr, "[a9sweep] old %s=%lld%s\n", name, static_cast<long long>(v), v == hoisted ? "" : " DIFFERS");
        }
        return v;
    }

    // Runs every registered initialiser once (sotc_overrides' install, before any native is registered).
    void init();
}

// A hoisted value at namespace scope: `A9S_HOIST(bool, g_x, std::getenv("X") != nullptr);`
#define A9S_HOIST(T, gv, ...)                                         \
    T gv{};                                                           \
    static const ::a9sweep::Hoist gv##_a9sHoist([] {                  \
        gv = static_cast<T>(__VA_ARGS__);                             \
        ::a9sweep::logNew<T>(#gv, gv);                                \
    })
// The same in a header (inline variables: one per program).
#define A9S_HOIST_INLINE(T, gv, ...)                                  \
    inline T gv{};                                                    \
    inline const ::a9sweep::Hoist gv##_a9sHoist{[] {                  \
        gv = static_cast<T>(__VA_ARGS__);                             \
        ::a9sweep::logNew<T>(#gv, gv);                                \
    }}
// The value in a function: on (sw), the hoisted global; off, the original function-local static with the same
// initialiser (evaluated at its first call as before). sw is a9sweep::on() read once by the caller.
#define A9S_VAL(sw, gv, T, ...)                                                                                       \
    (__builtin_expect((sw), 1) ? (gv) : ([]() -> T {                                                                   \
        static const T a9sOld_ = ::a9sweep::old<T>(#gv, static_cast<T>(__VA_ARGS__), gv);                              \
        return a9sOld_;                                                                                                \
    }()))
