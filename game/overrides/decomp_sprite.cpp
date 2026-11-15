// spriteDrawSprite (0x010A32B8): 2D sprite emitter on top of gsReg / feed2D.
// Eliminates per-sprite iosDlPushLayer, texTransRequest, gsRegBegin/gsReg/gsRegEnd
// traffic and recompiled clip/matrix overhead by computing the 4 corners, clipping,
// and feeding pairs straight to sotc_native::spriteFeed when nativeDraws() is on.
#include "sotc_decomp.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace sotc_native
{
    bool nativeDraws();
    void spriteFeed(uint8_t *rdram, uint32_t texResource, uint32_t layer, const uint64_t *pairs, uint32_t count);
}

namespace decomp_sprite
{
    using sotc_decomp::returnTo;

    constexpr uint32_t kSpriteDrawSprite = 0x010A32B8u;
    constexpr uint32_t kScreenWidthAddr = 0x00213818u;
    constexpr uint32_t kScreenHeightAddr = 0x0021381Cu;

    inline uint32_t ru32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (addr & 0x01FFFFFFu), 4);
        return v;
    }

    inline float rf32(const uint8_t *rdram, uint32_t addr)
    {
        float v;
        std::memcpy(&v, rdram + (addr & 0x01FFFFFFu), 4);
        return v;
    }

    inline void spriteSinCos(float x, float &s, float &c)
    {
        if (x == 0.0f)
        {
            s = 0.0f;
            c = 1.0f;
            return;
        }
        s = std::sinf(x);
        c = std::cosf(x);
    }

    void spriteDrawSprite(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (__builtin_expect(!sotc_native::nativeDraws(), 0))
        {
            if (const auto fb = sotc_decomp::recompiled(kSpriteDrawSprite))
            {
                fb(rdram, ctx, runtime);
                return;
            }
        }

        const uint32_t s = GPR_U32(ctx, 4);
        const uint64_t zShift = static_cast<uint64_t>(GPR_U32(ctx, 5)) << 32;
        const int32_t layerArg = static_cast<int32_t>(GPR_U32(ctx, 6));
        uint32_t alignMode = GPR_U32(ctx, 7);

        const float posX = ctx->f[12];
        const float posY = ctx->f[13];
        const float scaleX = ctx->f[14];
        const float scaleY = ctx->f[15];
        const float rot2 = ctx->f[16];
        const float alphaMul = ctx->f[17];

        const uint8_t *sp = rdram + (s & 0x01FFFFFFu);
        const float alphaF = static_cast<float>(sp[7]) * rf32(rdram, s + 8u) * alphaMul;
        if (alphaF == 0.0f)
        {
            returnTo(ctx);
            return;
        }

        // manager_sub_010A1F10: 4 anchor-aligned rectangle corners around local origin.
        if (alignMode == 0xFFFFFFFFu)
            alignMode = 4u;
        const int32_t q = static_cast<int32_t>(alignMode) / 3;
        const int32_t r = static_cast<int32_t>(alignMode) % 3;
        static constexpr float kMinAnchor[3] = {0.0f, -0.5f, -1.0f};
        static constexpr float kMaxAnchor[3] = {1.0f, 0.5f, 0.0f};
        const float xMinLocal = (r >= 0 && r < 3) ? kMinAnchor[r] : rf32(rdram, 0x0118C158u + static_cast<uint32_t>(r) * 4u);
        const float xMaxLocal = (r >= 0 && r < 3) ? kMaxAnchor[r] : rf32(rdram, 0x0118C168u + static_cast<uint32_t>(r) * 4u);
        const float yMinLocal = (q >= 0 && q < 3) ? kMinAnchor[q] : rf32(rdram, 0x0118C158u + static_cast<uint32_t>(q) * 4u);
        const float yMaxLocal = (q >= 0 && q < 3) ? kMaxAnchor[q] : rf32(rdram, 0x0118C168u + static_cast<uint32_t>(q) * 4u);

        const float w = rf32(rdram, s + 0x38u) * rf32(rdram, s + 0x40u);
        const float h = rf32(rdram, s + 0x3Cu) * rf32(rdram, s + 0x44u);
        const float lx[4] = {w * xMinLocal, w * xMaxLocal, w * xMinLocal, w * xMaxLocal};
        const float ly[4] = {h * yMinLocal, h * yMinLocal, h * yMaxLocal, h * yMaxLocal};

        float s1, c1, s2, c2;
        spriteSinCos(rf32(rdram, s + 0x48u), s1, c1);
        spriteSinCos(rot2, s2, c2);
        const float ox = rf32(rdram, s + 0x30u);
        const float oy = rf32(rdram, s + 0x34u);

        float vx[4], vy[4];
        for (int i = 0; i < 4; ++i)
        {
            const float x1 = c1 * lx[i] + s1 * ly[i] + ox;
            const float y1 = -s1 * lx[i] + c1 * ly[i] + oy;
            const float x2 = c2 * x1 + s2 * y1 + posX;
            const float y2 = -s2 * x1 + c2 * y1 + posY;
            vx[i] = scaleX * x2 + 2048.0f;
            vy[i] = scaleY * y2 + 2048.0f;
        }

        // manager_sub_010A21F8 + manager_sub_010A2040: screen & GS 12.4 guardband clip.
        const int32_t sw = static_cast<int32_t>(ru32(rdram, kScreenWidthAddr));
        const int32_t sh = static_cast<int32_t>(ru32(rdram, kScreenHeightAddr));
        const float sxMin = static_cast<float>(2048 - (sw >> 1));
        const float syMin = static_cast<float>(2048 - (sh >> 1));
        const float sxMax = static_cast<float>((sw >> 1) + 2048);
        const float syMax = static_cast<float>((sh >> 1) + 2048);

        if ((vx[0] < sxMin && vx[1] < sxMin && vx[2] < sxMin && vx[3] < sxMin) ||
            (vy[0] < syMin && vy[1] < syMin && vy[2] < syMin && vy[3] < syMin) ||
            (sxMax < vx[0] && sxMax < vx[1] && sxMax < vx[2] && sxMax < vx[3]) ||
            (syMax < vy[0] && syMax < vy[1] && syMax < vy[2] && syMax < vy[3]))
        {
            returnTo(ctx);
            return;
        }
        for (int i = 0; i < 4; ++i)
        {
            if (vx[i] < 0.0f || vx[i] >= 4096.0f || vy[i] < 0.0f || vy[i] >= 4096.0f)
            {
                returnTo(ctx);
                return;
            }
        }

        auto xyzf = [zShift](float x, float y) -> uint64_t
        {
            const int32_t iy = static_cast<int32_t>(y);
            const int32_t ix = static_cast<int32_t>(x);
            const int32_t fy = static_cast<int32_t>((y - static_cast<float>(iy)) * 16.0f);
            const int32_t fx = static_cast<int32_t>((x - static_cast<float>(ix)) * 16.0f);
            const uint32_t y16 = static_cast<uint32_t>((iy << 4) + fy);
            const uint32_t x16 = static_cast<uint32_t>((ix << 4) + fx);
            return zShift | (0xFF00ull << 48) | (static_cast<uint64_t>(y16) << 16) | static_cast<uint64_t>(x16);
        };

        uint64_t pairs[40];
        uint32_t n = 0;
        auto emit = [&](uint32_t reg, uint64_t val)
        {
            pairs[2u * n] = val;
            pairs[2u * n + 1u] = reg;
            ++n;
        };

        const uint32_t flags = ru32(rdram, s + 0x4Cu);
        if (((flags >> 12) & 3u) == 2u)
        {
            emit(0x42u, 0x0000008000000044ull);
            emit(0x00u, 0x54ull);
            emit(0x01u, 0xFE00ull << 46);
            for (int i = 0; i < 4; ++i)
                emit(0x04u, xyzf(vx[i], vy[i]));
        }

        uint64_t s4 = 1ull;
        switch ((flags >> 8) & 0xFu)
        {
        case 0u:
            s4 = 0ull;
            break;
        case 1u:
            emit(0x42u, 0x0000008000000044ull);
            break;
        case 2u:
            emit(0x42u, 0x0000008000000048ull);
            break;
        case 3u:
            emit(0x42u, 0x0000008000000042ull);
            break;
        case 4u:
        case 5u:
        case 6u:
            emit(0x42u, 0x0000008000000054ull);
            break;
        default:
            s4 = 0ull;
            break;
        }

        const bool useSt = ((flags >> 16) & 1u) != 0u;
        emit(0x00u, (s4 << 6) | (useSt ? 0x1Cull : 0x11Cull));

        const int32_t aByte = static_cast<int32_t>(alphaF);
        const uint64_t rgbaq = (0xFE00ull << 46) |
                               static_cast<uint64_t>(sp[4]) |
                               (static_cast<uint64_t>(sp[5]) << 8) |
                               (static_cast<uint64_t>(sp[6]) << 16) |
                               (static_cast<uint64_t>(static_cast<int64_t>(aByte)) << 24);
        emit(0x01u, rgbaq);

        if (useSt)
        {
            const uint64_t s0 = ru32(rdram, s + 0x20u);
            const uint64_t t0 = ru32(rdram, s + 0x24u);
            const uint64_t s1 = ru32(rdram, s + 0x28u);
            const uint64_t t1 = ru32(rdram, s + 0x2Cu);
            emit(0x02u, s0 | (t0 << 32));
            emit(0x04u, xyzf(vx[0], vy[0]));
            emit(0x02u, s1 | (t0 << 32));
            emit(0x04u, xyzf(vx[1], vy[1]));
            emit(0x02u, s0 | (t1 << 32));
            emit(0x04u, xyzf(vx[2], vy[2]));
            emit(0x02u, s1 | (t1 << 32));
            emit(0x04u, xyzf(vx[3], vy[3]));
        }
        else
        {
            const uint64_t u0 = (ru32(rdram, s + 0x10u) << 4) + 8u;
            const uint64_t v0 = (ru32(rdram, s + 0x14u) << 4) + 8u;
            const uint64_t u1 = (ru32(rdram, s + 0x18u) << 4) + 8u;
            const uint64_t v1raw = ru32(rdram, s + 0x1Cu) << 4;
            emit(0x03u, u0 | (v0 << 16));
            emit(0x04u, xyzf(vx[0], vy[0]));
            emit(0x03u, u1 | (v0 << 16));
            emit(0x04u, xyzf(vx[1], vy[1]));
            emit(0x03u, u0 | (v1raw << 16));
            emit(0x04u, xyzf(vx[2], vy[2]));
            emit(0x03u, u1 | ((v1raw + 8u) << 16));
            emit(0x04u, xyzf(vx[3], vy[3]));
        }

        const int32_t layerSum = layerArg + 25;
        const uint32_t layer = static_cast<uint32_t>((layerSum < 31) ? layerSum : 55);
        sotc_native::spriteFeed(rdram, ru32(rdram, s), layer, pairs, n);
        returnTo(ctx);
    }
}

SOTC_DECOMP_FUNCTION(0x010A32B8u, spriteDrawSprite, decomp_sprite::spriteDrawSprite);
