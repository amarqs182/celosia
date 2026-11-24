// Dedicated Mali-G31 (GLES 3.2 / GL 3.3 Core) native renderer backend for Celosia R36S.
// Plugs directly into native_renderer::detail::Backend (selected via SOTC_RENDERER=mali).
//
// Phase 6.1 (Step 1 Polish + T-pose hardening — stabilizing Host FPS & native 640x480 DRS):
// - Preserves all Phase 5.1 fixes (6a20f0f): uBones[192] via glUniform4fv, per-texture
//   wrapClamp cache + pre-resolved MaliTex*, uClip shadow cache, m_postTarget skip when
//   bloom=0, mailbox handover preserving uploads, reusable FramePacket.
// - Camera-invariant Merge-First + Fixed-Chunk Sub-Runs + Group Union-AABB Culling:
//   merges compatible static chunks deterministically into <=24-mesh sub-runs (24 MiB cache
//   cap, stale srcVerts eviction, 2500 us/frame budget) and culls each sub-run as one AABB,
//   eliminating camera-pan merge collapse spikes (mergeSaved 820 -> 71 -> 65-78 ms).
// - DRS target FPS decoupled from SOTC_FPS_CAP (SOTC_MALI_DRS_TARGET_FPS=30, climbing back
//   to native 640x480 at <=34 ms) + pre-present drawMs vs post-vsync renderMs split.
// - Branchless unrolled 3-bone uBones[b].xyz vertex skinning + _v8.bin shader cache +
//   palette-grouped sort + GL skin error & [mali:stats] skin=gpu/cpu/maxQ telemetry.

#include "native_backend.h"
#include "runtime/host_menu.h"
#include "runtime/native_tuning.h"

#if defined(__linux__) || defined(__APPLE__)

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#if __has_include(<GLES3/gl3.h>)
#include <GLES3/gl3.h>
#define PS2X_MALI_GLES 1
#else
#include <GLES2/gl2.h>
#define PS2X_MALI_GLES 1
#endif
#if defined(PS2X_LINUX_SDL2_SHIM)
void ps2xLinuxGlLock();
void ps2xLinuxGlUnlock();
bool ps2xLinuxGlHasWaiter();
void ps2xLinuxGetDrawableSize(uint32_t &w, uint32_t &h);
double ps2xLinuxLastSwapMs();
double ps2xLinuxLastCapSleepMs();
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#endif

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifndef GL_COMPRESSED_RGBA_ASTC_4x4_KHR
#define GL_COMPRESSED_RGBA_ASTC_4x4_KHR 0x93B0
#define GL_COMPRESSED_RGBA_ASTC_5x4_KHR 0x93B1
#define GL_COMPRESSED_RGBA_ASTC_5x5_KHR 0x93B2
#define GL_COMPRESSED_RGBA_ASTC_6x5_KHR 0x93B3
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#define GL_COMPRESSED_RGBA_ASTC_8x5_KHR 0x93B5
#define GL_COMPRESSED_RGBA_ASTC_8x6_KHR 0x93B6
#define GL_COMPRESSED_RGBA_ASTC_8x8_KHR 0x93B7
#endif

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

namespace native_renderer::detail
{
namespace
{

constexpr uint32_t kMaxSkinQwords = native_renderer::kSkinPaletteQwords; // 48 bones * 4 vec4s via glUniform4fv (libMali r13p0 safe)
constexpr uint32_t kMaxInstances = 16u;
constexpr uint64_t kMaxMergeSrcBytes = 48ull * 1024ull * 1024ull;
constexpr uint64_t kMaxMergeRunBytes = 32ull * 1024ull * 1024ull;

inline uint64_t nowUs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline bool envBool(const char *name, bool fallback)
{
    const char *e = std::getenv(name);
    if (!e || !e[0])
        return fallback;
    return std::atoi(e) != 0;
}

inline int envInt(const char *name, int fallback)
{
    const char *e = std::getenv(name);
    if (!e || !e[0])
        return fallback;
    return std::atoi(e);
}

inline float envFloat(const char *name, float fallback)
{
    const char *e = std::getenv(name);
    if (!e || !e[0])
        return fallback;
    return std::strtof(e, nullptr);
}

inline uint64_t fnv1aHash(const void *data, size_t len, uint64_t h = 14695981039346656037ull)
{
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline int8_t packNormComponent(float f)
{
    const float clamped = std::clamp(f, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lround(clamped * 127.0f));
}

struct CompactVertex
{
    float x, y, z;
    int8_t nx, ny, nz;
    uint8_t texLayer;
    uint8_t r, g, b, a;
    float u, v;
};
static_assert(sizeof(CompactVertex) == 28, "CompactVertex must be 28 bytes");

inline void packCompactVertices(const Vertex *src, uint32_t count, std::vector<CompactVertex> &dst)
{
    dst.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        dst[i].x = src[i].x;
        dst[i].y = src[i].y;
        dst[i].z = src[i].z;
        dst[i].nx = packNormComponent(src[i].nx);
        dst[i].ny = packNormComponent(src[i].ny);
        dst[i].nz = packNormComponent(src[i].nz);
        dst[i].texLayer = 0u;
        dst[i].r = src[i].r;
        dst[i].g = src[i].g;
        dst[i].b = src[i].b;
        dst[i].a = src[i].a;
        dst[i].u = src[i].u;
        dst[i].v = src[i].v;
    }
}

inline GLenum astcBlockToFormat(uint8_t bx, uint8_t by)
{
    if (bx == 4 && by == 4) return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    if (bx == 5 && by == 4) return GL_COMPRESSED_RGBA_ASTC_5x4_KHR;
    if (bx == 5 && by == 5) return GL_COMPRESSED_RGBA_ASTC_5x5_KHR;
    if (bx == 6 && by == 5) return GL_COMPRESSED_RGBA_ASTC_6x5_KHR;
    if (bx == 6 && by == 6) return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
    if (bx == 8 && by == 5) return GL_COMPRESSED_RGBA_ASTC_8x5_KHR;
    if (bx == 8 && by == 6) return GL_COMPRESSED_RGBA_ASTC_8x6_KHR;
    if (bx == 8 && by == 8) return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
    return 0;
}

inline uint8_t astcBlockToFmtCode(uint8_t bx, uint8_t by)
{
    if (bx == 4 && by == 4) return 3u;
    if (bx == 5 && by == 4) return 4u;
    if (bx == 5 && by == 5) return 5u;
    if (bx == 6 && by == 5) return 6u;
    if (bx == 6 && by == 6) return 7u;
    if (bx == 8 && by == 5) return 8u;
    if (bx == 8 && by == 6) return 9u;
    if (bx == 8 && by == 8) return 10u;
    return 0u;
}

inline GLenum fmtCodeToGlInternal(uint8_t fmtCode, uint8_t &outBx, uint8_t &outBy)
{
    outBx = outBy = 0;
    switch (fmtCode)
    {
    case 0u: return GL_RGB565;
    case 1u: return GL_RGB5_A1;
    case 2u: return GL_RGBA8;
    case 3u: outBx = 4; outBy = 4; return GL_COMPRESSED_RGBA_ASTC_4x4_KHR;
    case 4u: outBx = 5; outBy = 4; return GL_COMPRESSED_RGBA_ASTC_5x4_KHR;
    case 5u: outBx = 5; outBy = 5; return GL_COMPRESSED_RGBA_ASTC_5x5_KHR;
    case 6u: outBx = 6; outBy = 5; return GL_COMPRESSED_RGBA_ASTC_6x5_KHR;
    case 7u: outBx = 6; outBy = 6; return GL_COMPRESSED_RGBA_ASTC_6x6_KHR;
    case 8u: outBx = 8; outBy = 5; return GL_COMPRESSED_RGBA_ASTC_8x5_KHR;
    case 9u: outBx = 8; outBy = 6; return GL_COMPRESSED_RGBA_ASTC_8x6_KHR;
    case 10u: outBx = 8; outBy = 8; return GL_COMPRESSED_RGBA_ASTC_8x8_KHR;
    default: return GL_RGBA8;
    }
}

inline uint64_t computeTexContentHash(const uint8_t *rgba, size_t len, uint32_t w, uint32_t h, uint32_t levels)
{
    const uint32_t meta[3] = {w, h, levels};
    uint64_t hval = fnv1aHash(meta, sizeof(meta));
    return fnv1aHash(rgba, len, hval);
}

#if defined(PS2X_MALI_GLES)
#define MALI_VERT_HDR "#version 310 es\nprecision highp float;\nprecision highp int;\n"
#define MALI_FRAG_HDR "#version 310 es\nprecision mediump float;\nprecision highp int;\nprecision mediump sampler2D;\n"
#else
#define MALI_VERT_HDR "#version 330 core\n"
#define MALI_FRAG_HDR "#version 330 core\n"
#endif

static const char *kMaliWorldVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;

uniform vec4 uClip[4];
uniform vec4 uInstanceClips[64];
uniform int uInstanceCount;
uniform vec4 uDirections[4];
uniform vec4 uColours[4];
uniform vec4 uMisc;
uniform vec4 uFogB;
uniform vec4 uFogC;
uniform vec4 uGuard;

out highp vec4 vUV;
out mediump vec4 vColor;
out highp vec4 vScreen;
out highp float vLogW;

void main()
{
    vec4 c0 = uClip[0], c1 = uClip[1], c2 = uClip[2], c3 = uClip[3];
    if (uInstanceCount > 1)
    {
        int b = gl_InstanceID * 4;
        c0 = uInstanceClips[b + 0];
        c1 = uInstanceClips[b + 1];
        c2 = uInstanceClips[b + 2];
        c3 = uInstanceClips[b + 3];
    }
    vec4 c = c0 * aPos.x + c1 * aPos.y + c2 * aPos.z + c3;
    gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);

    float wSafe = max(c.w, 1e-3);
    float fog = uFogB.z > 0.5 ? clamp(uFogB.w + uFogC.x / wSafe, 0.0, 255.0) / 255.0 : 1.0;
    vUV = vec4(aUV + uMisc.zw, c.w, fog * c.w);
    vLogW = log2(wSafe);

    vec4 col = aColor * (1.0 / 128.0);
    col.a *= uMisc.y;
    if (uMisc.x < -0.5)
    {
        vec4 p = uDirections[0] * aPos.x + uDirections[1] * aPos.y + uDirections[2] * aPos.z + uDirections[3];
        float ndv = dot(normalize(p.xyz), uDirections[1].xyz);
        col.a *= 0.2 + 0.8 * (1.0 - abs(ndv));
    }
    else if (uMisc.x > 0.5)
    {
        vec4 d = max(uDirections[0] * aNormal.x + uDirections[1] * aNormal.y + uDirections[2] * aNormal.z + uDirections[3], 0.0);
        d.w = 1.0;
        vec4 light = uColours[0] * d.x + uColours[1] * d.y + uColours[2] * d.z + uColours[3];
        col.rgb = min(col.rgb * light.rgb, vec3(255.0 / 128.0));
    }
    vColor = col;
    vScreen = vec4(c.x, c.y, c.w, uGuard.x > 0.0 ? c.w - uGuard.y : 1.0);
}
)";

// Phase 6.1: Branchless unrolled 3-bone uBones[b].xyz skinning matching GL backend + clamp bounds.
static const char *kMaliSkinVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec3 aBone;
layout(location = 5) in vec3 aWeight;

uniform highp vec4 uBones[192];
uniform vec4 uClip[4];
uniform vec4 uDirections[4];
uniform vec4 uColours[4];
uniform vec4 uMisc;
uniform vec4 uFogB;
uniform vec4 uFogC;
uniform vec4 uGuard;

out highp vec4 vUV;
out mediump vec4 vColor;
out highp vec4 vScreen;
out highp float vLogW;

void main()
{
    int b0 = clamp(int(aBone.x + 0.5), 0, 188);
    int b1 = clamp(int(aBone.y + 0.5), 0, 188);
    int b2 = clamp(int(aBone.z + 0.5), 0, 188);
    vec3 pos = aWeight.x * (uBones[b0].xyz * aPos.x + uBones[b0 + 1].xyz * aPos.y + uBones[b0 + 2].xyz * aPos.z + uBones[b0 + 3].xyz)
             + aWeight.y * (uBones[b1].xyz * aPos.x + uBones[b1 + 1].xyz * aPos.y + uBones[b1 + 2].xyz * aPos.z + uBones[b1 + 3].xyz)
             + aWeight.z * (uBones[b2].xyz * aPos.x + uBones[b2 + 1].xyz * aPos.y + uBones[b2 + 2].xyz * aPos.z + uBones[b2 + 3].xyz);
    vec3 nrm = aWeight.x * (uBones[b0].xyz * aNormal.x + uBones[b0 + 1].xyz * aNormal.y + uBones[b0 + 2].xyz * aNormal.z)
             + aWeight.y * (uBones[b1].xyz * aNormal.x + uBones[b1 + 1].xyz * aNormal.y + uBones[b1 + 2].xyz * aNormal.z)
             + aWeight.z * (uBones[b2].xyz * aNormal.x + uBones[b2 + 1].xyz * aNormal.y + uBones[b2 + 2].xyz * aNormal.z);

    vec4 c = uClip[0] * pos.x + uClip[1] * pos.y + uClip[2] * pos.z + uClip[3];
    gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);

    float wSafe = max(c.w, 1e-3);
    float fog = uFogB.z > 0.5 ? clamp(uFogB.w + uFogC.x / wSafe, 0.0, 255.0) / 255.0 : 1.0;
    vUV = vec4(aUV + uMisc.zw, c.w, fog * c.w);
    vLogW = log2(wSafe);

    vec4 col = aColor * (1.0 / 128.0);
    col.a *= uMisc.y;
    if (uMisc.x < -0.5)
    {
        vec4 p = uDirections[0] * pos.x + uDirections[1] * pos.y + uDirections[2] * pos.z + uDirections[3];
        float ndv = dot(normalize(p.xyz), uDirections[1].xyz);
        col.a *= 0.2 + 0.8 * (1.0 - abs(ndv));
    }
    else if (uMisc.x > 0.5)
    {
        vec4 d = max(uDirections[0] * nrm.x + uDirections[1] * nrm.y + uDirections[2] * nrm.z + uDirections[3], 0.0);
        d.w = 1.0;
        vec4 light = uColours[0] * d.x + uColours[1] * d.y + uColours[2] * d.z + uColours[3];
        col.rgb = min(col.rgb * light.rgb, vec3(255.0 / 128.0));
    }
    vColor = col;
    vScreen = vec4(c.x, c.y, c.w, uGuard.x > 0.0 ? c.w - uGuard.y : 1.0);
}
)";

static const char *kMaliDepthVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
uniform vec4 uClip[4];
void main()
{
    vec4 c = uClip[0] * aPos.x + uClip[1] * aPos.y + uClip[2] * aPos.z + uClip[3];
    gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);
}
)";

static const char *kMaliDepthFrag = MALI_FRAG_HDR R"(
void main() {}
)";

static const char *kMaliOpaqueFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;

uniform sampler2D uTex;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;

out mediump vec4 oColor;

void main()
{
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = uFogCol.w > 0.5 ? textureLod(uTex, vUV.xy, lod) : vec4(vec3(128.0 / 255.0), 1.0);
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, 1.0);
}
)";

static const char *kMaliCutFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;

uniform sampler2D uTex;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    if (vScreen.w < 0.0)
        discard;
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = uFogCol.w > 0.5 ? textureLod(uTex, vUV.xy, lod) : vec4(vec3(128.0 / 255.0), 1.0);
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, alpha);
}
)";

// Phase 7B: Lean fragment shaders for Mali-G31 when background/vertex fog are inactive,
// plus discard-free Soft variants that preserve Early-Z / Forward Pixel Kill (FPK).
static const char *kMaliOpaqueFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp float vLogW;

uniform sampler2D uTex;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;

out mediump vec4 oColor;

void main()
{
    mediump vec4 t;
    if (uFogCol.w > 0.5)
    {
        if (uFogC.w > 0.0)
        {
            highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
            t = textureLod(uTex, vUV.xy, lod);
        }
        else
        {
            t = texture(uTex, vUV.xy);
        }
    }
    else
    {
        t = vec4(vec3(128.0 / 255.0), 1.0);
    }
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), 1.0);
}
)";

static const char *kMaliCutFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;

uniform sampler2D uTex;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    if (vScreen.w < 0.0)
        discard;
    mediump vec4 t;
    if (uFogCol.w > 0.5)
    {
        if (uFogC.w > 0.0)
        {
            highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
            t = textureLod(uTex, vUV.xy, lod);
        }
        else
        {
            t = texture(uTex, vUV.xy);
        }
    }
    else
    {
        t = vec4(vec3(128.0 / 255.0), 1.0);
    }
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), alpha);
}
)";

static const char *kMaliSoftFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp float vLogW;

uniform sampler2D uTex;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    mediump vec4 t;
    if (uFogCol.w > 0.5)
    {
        if (uFogC.w > 0.0)
        {
            highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
            t = textureLod(uTex, vUV.xy, lod);
        }
        else
        {
            t = texture(uTex, vUV.xy);
        }
    }
    else
    {
        t = vec4(vec3(128.0 / 255.0), 1.0);
    }
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        alpha = 0.0;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), alpha);
}
)";

static const char *kMaliSoftFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;

uniform sampler2D uTex;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = uFogCol.w > 0.5 ? textureLod(uTex, vUV.xy, lod) : vec4(vec3(128.0 / 255.0), 1.0);
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        alpha = 0.0;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, alpha);
}
)";

static const char *kMaliScreenVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;

uniform vec4 uClip[4];
uniform vec4 uMisc;
uniform vec4 uGuard;

out highp vec2 vUV;
out mediump vec4 vColor;

void main()
{
    vec4 c = uClip[0] * aPos.x + uClip[1] * aPos.y + uClip[2] * aPos.z + uClip[3];
    if (uGuard.z > 0.5)
        gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);
    else
        gl_Position = vec4(c.x, c.y, 0.0, 1.0);
    vUV = aUV + uMisc.zw;
    vColor = vec4(aColor.rgb * (1.0 / 128.0), aColor.a * (1.0 / 128.0) * uMisc.y);
}
)";

static const char *kMaliScreenFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
in mediump vec4 vColor;

uniform sampler2D uTex;
uniform mediump vec4 uFogCol;

out mediump vec4 oColor;

void main()
{
    mediump vec4 t = uFogCol.w > 0.5 ? texture(uTex, vUV) : vec4(vec3(128.0 / 255.0), 1.0);
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < 0.01)
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), alpha);
}
)";

static const char *kMaliPostVert = MALI_VERT_HDR R"(
out highp vec2 vUV;
void main()
{
    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0, (gl_VertexID == 2) ? 3.0 : -1.0);
    vUV = pos * 0.5 + 0.5;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

static const char *kMaliPostFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uBackground;
uniform sampler2D uDofTex;
uniform sampler2D uAoTex;
uniform sampler2D uPrevTex;
uniform highp sampler2D uDepth;
uniform highp vec4 uMisc;  // invW, invH, caStrength, lensFlareStrength
uniform highp vec4 uPostA; // bloomAddR, bloomAddG, bloomAddB, bloomAddA
uniform highp vec4 uPostB; // expStrength, aoEnabled, prevBlurWeight, dofEnabled
uniform highp vec4 uPostC; // dofFocusZ, dofZA, dofZB, (bit0=fxaa, bit1=rcas)
uniform highp vec4 uPostD; // sblurScale, sblurAngle, sblurDx, sblurDy
out mediump vec4 oColor;
mediump float lumaFast(mediump vec3 rgb) { return rgb.g * 1.963 + rgb.r; }
void main()
{
    highp vec2 tx = uMisc.xy;
    mediump vec3 cN = vec3(0.0), cS = vec3(0.0), cW = vec3(0.0), cE = vec3(0.0);
    bool haveCross = false;
    mediump vec3 c;
    if (uMisc.z > 0.0001)
    {
        highp vec2 dir = vUV - vec2(0.5);
        highp vec2 off = dir * dot(dir, dir) * uMisc.z;
        c.r = texture(uTex, vUV + off).r;
        c.g = texture(uTex, vUV).g;
        c.b = texture(uTex, vUV - off).b;
    }
    else
    {
        c = texture(uTex, vUV).rgb;
    }
    if (uPostC.w > 0.5)
    {
        cN = texture(uTex, vUV + vec2(0.0, -tx.y)).rgb;
        cS = texture(uTex, vUV + vec2(0.0,  tx.y)).rgb;
        cW = texture(uTex, vUV + vec2(-tx.x, 0.0)).rgb;
        cE = texture(uTex, vUV + vec2( tx.x, 0.0)).rgb;
        haveCross = true;
    }
    if (uPostC.w == 1.0 || uPostC.w == 3.0)
    {
        mediump float lM = lumaFast(c);
        mediump float lN = lumaFast(cN);
        mediump float lS = lumaFast(cS);
        mediump float lW = lumaFast(cW);
        mediump float lE = lumaFast(cE);
        mediump float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
        mediump float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
        mediump float lRange = lMax - lMin;
        if (lRange > max(0.0625, lMax * 0.125))
        {
            highp vec2 dir = vec2(-(lN - lS), lW - lE);
            highp float dirReduce = max((lN + lS + lW + lE) * 0.03125, 0.0078125);
            highp float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
            dir = clamp(dir * rcpDirMin, vec2(-4.0), vec2(4.0)) * tx;
            mediump vec3 rgbA = 0.5 * (
                texture(uTex, vUV + dir * (-0.166667)).rgb +
                texture(uTex, vUV + dir * ( 0.166667)).rgb);
            mediump vec3 rgbB = rgbA * 0.5 + 0.25 * (
                texture(uTex, vUV + dir * (-0.5)).rgb +
                texture(uTex, vUV + dir * ( 0.5)).rgb);
            mediump float lB = lumaFast(rgbB);
            c = (lB < lMin || lB > lMax) ? rgbA : rgbB;
        }
    }
    if (uPostC.w >= 1.5)
    {
        if (!haveCross)
        {
            cN = texture(uTex, vUV + vec2(0.0, -tx.y)).rgb;
            cS = texture(uTex, vUV + vec2(0.0,  tx.y)).rgb;
            cW = texture(uTex, vUV + vec2(-tx.x, 0.0)).rgb;
            cE = texture(uTex, vUV + vec2( tx.x, 0.0)).rgb;
        }
        mediump float lM = lumaFast(c);
        mediump float lN = lumaFast(cN);
        mediump float lS = lumaFast(cS);
        mediump float lW = lumaFast(cW);
        mediump float lE = lumaFast(cE);
        mediump float mn4 = min(min(lN, lS), min(lW, lE));
        mediump float mx4 = max(max(lN, lS), max(lW, lE));
        mediump float mnL = min(mn4, lM);
        mediump float mxL = max(mx4, lM);
        mediump float hitMin = mnL / max(4.0 * mxL, 1e-4);
        mediump float hitMax = (2.963 - mxL) / max(4.0 * mnL - 11.852, -11.852);
        mediump float lobe = max(-hitMin, hitMax);
        mediump float w = clamp(lobe, -0.18, 0.0) * 0.85;
        mediump float rcpW = 1.0 / (4.0 * w + 1.0);
        c = clamp((c + (cN + cS + cW + cE) * w) * rcpW, 0.0, 1.0);
    }
    if (abs(uPostD.x) > 0.0001 || abs(uPostD.y) > 0.0001 || abs(uPostD.z) > 0.0001 || abs(uPostD.w) > 0.0001)
    {
        highp vec2 p = vUV - vec2(0.5);
        highp float s = sin(uPostD.y * 0.5);
        highp float k = cos(uPostD.y * 0.5);
        highp vec2 rot = vec2(p.x * k - p.y * s, p.x * s + p.y * k) * (1.0 + uPostD.x * 0.5) + vec2(0.5) + uPostD.zw * 0.5;
        highp vec2 stepUv = (rot - vUV) * 0.3333;
        c = c * 0.34
          + texture(uTex, vUV + stepUv).rgb * 0.26
          + texture(uTex, vUV + stepUv * 2.0).rgb * 0.22
          + texture(uTex, vUV + stepUv * 3.0).rgb * 0.18;
    }
    if (uPostB.w > 0.5)
    {
        highp float d = texture(uDepth, vUV).r;
        highp float z = d >= 0.99999 ? 0.0 : uPostC.y + uPostC.z * (1.0 - d) / 0.1;
        if (z <= uPostC.x)
        {
            mediump float coc = clamp((uPostC.x - z) / max(uPostC.x, 1.0), 0.0, 1.0) * 0.85;
            c = mix(c, texture(uDofTex, vUV).rgb, coc);
        }
    }
    if (uPostB.y > 0.5)
    {
        c *= texture(uAoTex, vUV).r;
    }
    if (uPostB.x > 0.001)
    {
        mediump float avgLuma = 0.25 * (
            texture(uBackground, vec2(0.25, 0.25)).a +
            texture(uBackground, vec2(0.75, 0.25)).a +
            texture(uBackground, vec2(0.25, 0.75)).a +
            texture(uBackground, vec2(0.75, 0.75)).a);
        mediump float expGain = clamp(1.0 + (0.42 - avgLuma) * uPostB.x, 0.82, 1.22);
        c *= expGain;
    }
    if (uPostA.a > 0.001)
    {
        mediump vec3 b = texture(uBackground, vUV).rgb * uPostA.rgb * uPostA.a;
        c = c + b * (vec3(1.0) - clamp(c, 0.0, 1.0) * 0.35);
    }
    if (uMisc.w > 0.001)
    {
        highp vec2 dv = vUV - vec2(0.5);
        mediump float vig = 1.0 - clamp(length(dv) * 1.35, 0.0, 1.0);
        mediump vec3 f1 = texture(uBackground, clamp(vec2(0.5) - dv * 0.55, vec2(0.02), vec2(0.98))).rgb * vec3(1.0, 0.78, 0.48);
        mediump vec3 f2 = texture(uBackground, clamp(vec2(0.5) - dv * 0.25, vec2(0.02), vec2(0.98))).rgb * vec3(0.45, 0.78, 1.0);
        mediump vec3 f3 = texture(uBackground, clamp(vec2(0.5) + dv * 0.35, vec2(0.02), vec2(0.98))).rgb * vec3(0.95, 0.65, 0.35);
        c += (f1 * 0.45 + f2 * 0.30 + f3 * 0.25) * vig * uMisc.w;
    }
    if (uPostB.z > 0.001)
    {
        c = mix(c, texture(uPrevTex, vUV).rgb, uPostB.z);
    }
    oColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

// Phase 9: Team ICO Pseudo-HDR Bloom Mask + Bright Extract + Luma (128x96)
static const char *kMaliBloomMaskFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform sampler2D uTex;
uniform highp sampler2D uDepth;
uniform highp vec4 uMisc;  // texelX, texelY, 0, 0
uniform highp vec4 uPostA; // threshold, zA, zB, bloomEnabled
uniform mediump vec4 uPostB; // maskColor.rgb, brightWeight
out mediump vec4 oColor;
highp float gsZ(highp float d, highp float zA, highp float zB) { return d >= 0.99999 ? 0.0 : zA + zB * (1.0 - d) / 0.1; }
void main()
{
    highp vec2 o = uMisc.xy * 0.5;
    highp float d0 = texture(uDepth, vUV + vec2(-o.x, -o.y)).r;
    highp float d1 = texture(uDepth, vUV + vec2( o.x, -o.y)).r;
    highp float d2 = texture(uDepth, vUV + vec2(-o.x,  o.y)).r;
    highp float d3 = texture(uDepth, vUV + vec2( o.x,  o.y)).r;
    mediump float mask = 0.0;
    if (uPostA.w > 0.5)
    {
        highp float thr = uPostA.x + 1.0;
        mask = 0.25 * (
            (gsZ(d0, uPostA.y, uPostA.z) <= thr ? 1.0 : 0.0) +
            (gsZ(d1, uPostA.y, uPostA.z) <= thr ? 1.0 : 0.0) +
            (gsZ(d2, uPostA.y, uPostA.z) <= thr ? 1.0 : 0.0) +
            (gsZ(d3, uPostA.y, uPostA.z) <= thr ? 1.0 : 0.0));
    }
    mediump vec3 c = texture(uTex, vUV).rgb;
    mediump float luma = dot(c, vec3(0.299, 0.587, 0.114));
    mediump vec3 hi = max(c - vec3(0.75), vec3(0.0)) * uPostB.w;
    oColor = vec4(clamp(uPostB.rgb * mask + hi, 0.0, 1.5), clamp(luma, 0.0, 1.0));
}
)";

// Phase 9: 5-tap Dual-Kawase blur (used for 64x48 Bloom and 160x120 DoF)
static const char *kMaliKawaseFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform sampler2D uTex;
uniform highp vec4 uMisc; // offsetX, offsetY, 0, 0
out mediump vec4 oColor;
void main()
{
    highp vec2 o = uMisc.xy;
    mediump vec4 c = texture(uTex, vUV) * 0.40;
    c += texture(uTex, vUV + vec2(-o.x, -o.y)) * 0.15;
    c += texture(uTex, vUV + vec2( o.x, -o.y)) * 0.15;
    c += texture(uTex, vUV + vec2(-o.x,  o.y)) * 0.15;
    c += texture(uTex, vUV + vec2( o.x,  o.y)) * 0.15;
    oColor = c;
}
)";

// Phase 9: Team ICO Temporal Persistence / Eye-Adaptation Blend (64x48)
static const char *kMaliTemporalFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform sampler2D uTex;
uniform sampler2D uBackground;
uniform highp vec4 uMisc; // blendFactor, 0, 0, 0
out mediump vec4 oColor;
void main()
{
    oColor = mix(texture(uBackground, vUV), texture(uTex, vUV), uMisc.x);
}
)";

// Phase 9: Quarter-Res 4-Tap Contact SSAO (160x120)
static const char *kMaliAoFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform highp sampler2D uDepth;
uniform highp vec4 uMisc; // texelX, texelY, radiusPx, strength
out mediump vec4 oColor;
highp float linW(highp float d) { return 0.1 / max(1.0 - d, 1e-5); }
void main()
{
    highp float d0 = texture(uDepth, vUV).r;
    if (d0 >= 0.99995)
    {
        oColor = vec4(1.0);
        return;
    }
    highp float w0 = linW(d0);
    highp float rad = clamp(uMisc.z / max(w0 * 0.08, 1.0), 1.5, 6.0);
    highp vec2 stepUv = uMisc.xy * rad;
    highp vec2 par = mod(floor(gl_FragCoord.xy), 2.0);
    bool rot = abs(par.x - par.y) > 0.5;
    highp vec2 o0 = rot ? vec2( 0.707,  0.707) : vec2( 1.0,  0.0);
    highp vec2 o1 = rot ? vec2(-0.707,  0.707) : vec2( 0.0,  1.0);
    highp vec2 o2 = rot ? vec2(-0.707, -0.707) : vec2(-1.0,  0.0);
    highp vec2 o3 = rot ? vec2( 0.707, -0.707) : vec2( 0.0, -1.0);
    highp float maxRange = max(w0 * 0.22, 1.8);
    mediump float occ = 0.0;
    highp float ws;
    highp float diff;
    ws = linW(texture(uDepth, vUV + o0 * stepUv).r); diff = w0 - ws;
    if (diff > 0.15 && diff < maxRange) occ += smoothstep(0.15, maxRange * 0.5, diff) * (1.0 - smoothstep(maxRange * 0.5, maxRange, diff));
    ws = linW(texture(uDepth, vUV + o1 * stepUv).r); diff = w0 - ws;
    if (diff > 0.15 && diff < maxRange) occ += smoothstep(0.15, maxRange * 0.5, diff) * (1.0 - smoothstep(maxRange * 0.5, maxRange, diff));
    ws = linW(texture(uDepth, vUV + o2 * stepUv).r); diff = w0 - ws;
    if (diff > 0.15 && diff < maxRange) occ += smoothstep(0.15, maxRange * 0.5, diff) * (1.0 - smoothstep(maxRange * 0.5, maxRange, diff));
    ws = linW(texture(uDepth, vUV + o3 * stepUv).r); diff = w0 - ws;
    if (diff > 0.15 && diff < maxRange) occ += smoothstep(0.15, maxRange * 0.5, diff) * (1.0 - smoothstep(maxRange * 0.5, maxRange, diff));
    oColor = vec4(vec3(clamp(1.0 - occ * uMisc.w, 0.35, 1.0)), 1.0);
}
)";

// Phase 9: 4-Tap 2x2 Dither-Canceling AO Blur (160x120)
static const char *kMaliAoBlurFrag = MALI_FRAG_HDR R"(
in highp vec2 vUV;
uniform sampler2D uTex;
uniform highp vec4 uMisc; // texelX, texelY, 0, 0
out mediump vec4 oColor;
void main()
{
    highp vec2 o = uMisc.xy * 0.5;
    mediump float a = 0.25 * (
        texture(uTex, vUV + vec2(-o.x, -o.y)).r +
        texture(uTex, vUV + vec2( o.x, -o.y)).r +
        texture(uTex, vUV + vec2(-o.x,  o.y)).r +
        texture(uTex, vUV + vec2( o.x,  o.y)).r);
    oColor = vec4(vec3(a), 1.0);
}
)";

// Phase 7C/7G: GL_TEXTURE_2D_ARRAY shaders for cross-texture static MergeRuns, singletons, and instanced draws.
static const char *kMaliWorldArrVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;
layout(location = 4) in float aTexLayer;

uniform vec4 uClip[4];
uniform vec4 uInstanceClips[64];
uniform int uInstanceCount;
uniform float uTexLayerBias;
uniform vec4 uDirections[4];
uniform vec4 uColours[4];
uniform vec4 uMisc;
uniform vec4 uFogB;
uniform vec4 uFogC;
uniform vec4 uGuard;

out highp vec4 vUV;
out mediump vec4 vColor;
out highp vec4 vScreen;
out highp float vLogW;
flat out highp float vTexLayer;

void main()
{
    vec4 c0 = uClip[0], c1 = uClip[1], c2 = uClip[2], c3 = uClip[3];
    if (uInstanceCount > 1)
    {
        int b = gl_InstanceID * 4;
        c0 = uInstanceClips[b + 0];
        c1 = uInstanceClips[b + 1];
        c2 = uInstanceClips[b + 2];
        c3 = uInstanceClips[b + 3];
    }
    vec4 c = c0 * aPos.x + c1 * aPos.y + c2 * aPos.z + c3;
    gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);

    float wSafe = max(c.w, 1e-3);
    float fog = uFogB.z > 0.5 ? clamp(uFogB.w + uFogC.x / wSafe, 0.0, 255.0) / 255.0 : 1.0;
    vUV = vec4(aUV + uMisc.zw, c.w, fog * c.w);
    vLogW = log2(wSafe);
    vTexLayer = aTexLayer + uTexLayerBias;

    vec4 col = aColor * (1.0 / 128.0);
    col.a *= uMisc.y;
    if (uMisc.x < -0.5)
    {
        vec4 p = uDirections[0] * aPos.x + uDirections[1] * aPos.y + uDirections[2] * aPos.z + uDirections[3];
        float ndv = dot(normalize(p.xyz), uDirections[1].xyz);
        col.a *= 0.2 + 0.8 * (1.0 - abs(ndv));
    }
    else if (uMisc.x > 0.5)
    {
        vec4 d = max(uDirections[0] * aNormal.x + uDirections[1] * aNormal.y + uDirections[2] * aNormal.z + uDirections[3], 0.0);
        d.w = 1.0;
        vec4 light = uColours[0] * d.x + uColours[1] * d.y + uColours[2] * d.z + uColours[3];
        col.rgb = min(col.rgb * light.rgb, vec3(255.0 / 128.0));
    }
    vColor = col;
    vScreen = vec4(c.x, c.y, c.w, uGuard.x > 0.0 ? c.w - uGuard.y : 1.0);
}
)";

static const char *kMaliArrOpaqueFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;

out mediump vec4 oColor;

void main()
{
    mediump vec4 t;
    if (uFogC.w > 0.0)
    {
        highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
        t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    }
    else
    {
        t = texture(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)));
    }
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), 1.0);
}
)";

static const char *kMaliArrCutFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    if (vScreen.w < 0.0)
        discard;
    mediump vec4 t;
    if (uFogC.w > 0.0)
    {
        highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
        t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    }
    else
    {
        t = texture(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)));
    }
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), alpha);
}
)";

static const char *kMaliArrSoftFastFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    mediump vec4 t;
    if (uFogC.w > 0.0)
    {
        highp float lod = clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w);
        t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    }
    else
    {
        t = texture(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)));
    }
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        alpha = 0.0;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(clamp(raw, 0.0, 1.0), alpha);
}
)";

static const char *kMaliArrOpaqueFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;

out mediump vec4 oColor;

void main()
{
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, 1.0);
}
)";

static const char *kMaliArrCutFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    if (vScreen.w < 0.0)
        discard;
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, alpha);
}
)";

static const char *kMaliArrSoftFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;
flat in highp float vTexLayer;

uniform mediump sampler2DArray uTexArray;
uniform sampler2D uBackground;
uniform highp vec4 uFogA;
uniform highp vec4 uFogC;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    highp float depth = vUV.z;
    highp float lod = uFogC.w > 0.0 ? clamp(vLogW * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    mediump vec4 t = textureLod(uTexArray, vec3(vUV.xy, floor(vTexLayer + 0.5)), lod);
    mediump float alpha = clamp(t.a * max(uFogCol.w, 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        alpha = 0.0;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    rgb = mix(uFogCol.rgb, rgb, clamp(vUV.w / max(depth, 1e-3), 0.0, 1.0));
    if (uFogA.w > 0.5)
    {
        highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3) * 0.5 + 0.5;
        mediump float f = clamp((depth - uFogA.x) / max(uFogA.y, 1e-3), 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, scr).rgb, f);
    }
    oColor = vec4(rgb, alpha);
}
)";

// Phase 8B: Lightweight Fresnel-modulated water tint (zero extra reflection FBO pass).
static const char *kMaliWaterFrag = MALI_FRAG_HDR R"(
in highp vec4 vUV;
in mediump vec4 vColor;
in highp vec4 vScreen;
in highp float vLogW;

uniform highp vec4 uFogA;
uniform mediump vec4 uFogCol;
uniform mediump vec4 uAlpha;

out mediump vec4 oColor;

void main()
{
    mediump float fres = clamp(vColor.a, 0.0, 1.0);
    highp vec2 scr = vScreen.xy / max(vScreen.z, 1e-3);
    mediump float wave = 0.5 + 0.5 * sin(scr.x * 22.0 + scr.y * 14.0 + uFogA.x * 2.5);
    mediump vec3 deepCol = mix(vec3(0.14, 0.22, 0.26), uFogCol.rgb * 0.55, 0.45);
    mediump vec3 skyCol = mix(vec3(0.52, 0.64, 0.72), uFogCol.rgb * 1.15, 0.65);
    mediump vec3 raw = mix(deepCol, skyCol, clamp(fres * 0.85 + wave * 0.12, 0.0, 1.0));
    mediump vec3 rgb = clamp(raw, 0.0, 1.0);
    mediump float a = clamp((0.20 + fres * 0.55) * max(uAlpha.z, 0.35), 0.12, 0.78);
    oColor = vec4(rgb, a);
}
)";

struct UniformShadow
{
    bool valid = false;
    bool clipValid = false;
    bool lightValid = false;
    uint32_t lastSkinPalette = 0u;
    int instCount = -1;
    float texLayerBias = -1.0f;
    float clip[16] = {};
    float misc[4] = {};
    float fogA[4] = {};
    float fogB[4] = {};
    float fogC[4] = {};
    float fogCol[4] = {};
    float guard[4] = {};
    float alpha[4] = {};
    float light[32] = {};

    void reset()
    {
        valid = false;
        clipValid = false;
        lightValid = false;
        lastSkinPalette = 0u;
        instCount = -1;
        texLayerBias = -1.0f;
    }
};

struct ShaderProg
{
    GLuint id = 0;
    GLint uClip = -1, uInstanceClips = -1, uInstanceCount = -1, uTexLayerBias = -1;
    GLint uBones = -1, uDirections = -1, uColours = -1, uMisc = -1;
    GLint uFogA = -1, uFogB = -1, uFogC = -1, uFogCol = -1, uGuard = -1, uAlpha = -1;
    GLint uPostA = -1, uPostB = -1, uPostC = -1, uPostD = -1;
    GLint uTex = -1, uTexArray = -1, uBackground = -1, uDofTex = -1, uAoTex = -1, uPrevTex = -1, uDepth = -1;
    mutable UniformShadow shadow{};
};

inline bool same4(const float a[4], float x, float y, float z, float w)
{
    return a[0] == x && a[1] == y && a[2] == z && a[3] == w;
}

inline void setCached4f(GLint loc, float cached[4], bool valid, float x, float y, float z, float w)
{
    if (loc < 0)
        return;
    if (valid && same4(cached, x, y, z, w))
        return;
    cached[0] = x; cached[1] = y; cached[2] = z; cached[3] = w;
    glUniform4f(loc, x, y, z, w);
}

GLuint compileStage(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[mali] shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

void bindProgUniforms(ShaderProg &out, GLuint p)
{
    out.id = p;
    out.uClip = glGetUniformLocation(p, "uClip");
    out.uInstanceClips = glGetUniformLocation(p, "uInstanceClips");
    out.uInstanceCount = glGetUniformLocation(p, "uInstanceCount");
    out.uTexLayerBias = glGetUniformLocation(p, "uTexLayerBias");
    out.uBones = glGetUniformLocation(p, "uBones");
    out.uDirections = glGetUniformLocation(p, "uDirections");
    out.uColours = glGetUniformLocation(p, "uColours");
    out.uMisc = glGetUniformLocation(p, "uMisc");
    out.uFogA = glGetUniformLocation(p, "uFogA");
    out.uFogB = glGetUniformLocation(p, "uFogB");
    out.uFogC = glGetUniformLocation(p, "uFogC");
    out.uFogCol = glGetUniformLocation(p, "uFogCol");
    out.uGuard = glGetUniformLocation(p, "uGuard");
    out.uAlpha = glGetUniformLocation(p, "uAlpha");
    out.uPostA = glGetUniformLocation(p, "uPostA");
    out.uPostB = glGetUniformLocation(p, "uPostB");
    out.uPostC = glGetUniformLocation(p, "uPostC");
    out.uPostD = glGetUniformLocation(p, "uPostD");
    out.uTex = glGetUniformLocation(p, "uTex");
    out.uTexArray = glGetUniformLocation(p, "uTexArray");
    out.uBackground = glGetUniformLocation(p, "uBackground");
    out.uDofTex = glGetUniformLocation(p, "uDofTex");
    out.uAoTex = glGetUniformLocation(p, "uAoTex");
    out.uPrevTex = glGetUniformLocation(p, "uPrevTex");
    out.uDepth = glGetUniformLocation(p, "uDepth");

    glUseProgram(p);
    if (out.uTex >= 0) glUniform1i(out.uTex, 0);
    if (out.uTexArray >= 0) glUniform1i(out.uTexArray, 0);
    if (out.uBackground >= 0) glUniform1i(out.uBackground, 1);
    if (out.uDofTex >= 0) glUniform1i(out.uDofTex, 2);
    if (out.uAoTex >= 0) glUniform1i(out.uAoTex, 3);
    if (out.uPrevTex >= 0) glUniform1i(out.uPrevTex, 4);
    if (out.uDepth >= 0) glUniform1i(out.uDepth, 5);
    out.shadow.reset();
}

bool linkProgCached(ShaderProg &out, const char *name, const char *vsSrc, const char *fsSrc,
                    const std::string &cacheDir, bool useCache, bool *outFromCache = nullptr)
{
    if (outFromCache)
        *outFromCache = false;
#if defined(PS2X_MALI_GLES)
    std::string binPath;
    if (useCache && !cacheDir.empty())
    {
        binPath = cacheDir + "/" + name + "_v14.bin";
        if (FILE *fp = std::fopen(binPath.c_str(), "rb"))
        {
            GLenum fmt = 0;
            uint32_t len = 0;
            if (std::fread(&fmt, sizeof(fmt), 1, fp) == 1 &&
                std::fread(&len, sizeof(len), 1, fp) == 1 && len > 0 && len < 4u * 1024u * 1024u)
            {
                std::vector<uint8_t> blob(len);
                if (std::fread(blob.data(), 1, len, fp) == len)
                {
                    GLuint p = glCreateProgram();
                    glProgramBinary(p, fmt, blob.data(), static_cast<GLsizei>(len));
                    GLint ok = 0;
                    glGetProgramiv(p, GL_LINK_STATUS, &ok);
                    if (ok)
                    {
                        std::fclose(fp);
                        bindProgUniforms(out, p);
                        if (outFromCache)
                            *outFromCache = true;
                        return true;
                    }
                    glDeleteProgram(p);
                }
            }
            std::fclose(fp);
        }
    }
#else
    (void)name;
    (void)cacheDir;
    (void)useCache;
#endif

    GLuint vs = compileStage(GL_VERTEX_SHADER, vsSrc);
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, fsSrc);
    if (!vs || !fs)
    {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512] = {};
        glGetProgramInfoLog(p, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[mali] program link error (%s): %s\n", name, log);
        glDeleteProgram(p);
        return false;
    }

#if defined(PS2X_MALI_GLES)
    if (useCache && !binPath.empty())
    {
        GLint binLen = 0;
        glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &binLen);
        if (binLen > 0 && binLen < 4 * 1024 * 1024)
        {
            std::vector<uint8_t> blob(static_cast<size_t>(binLen));
            GLenum fmt = 0;
            GLsizei written = 0;
            glGetProgramBinary(p, binLen, &written, &fmt, blob.data());
            if (written > 0)
            {
                if (FILE *fp = std::fopen(binPath.c_str(), "wb"))
                {
                    const uint32_t ulen = static_cast<uint32_t>(written);
                    std::fwrite(&fmt, sizeof(fmt), 1, fp);
                    std::fwrite(&ulen, sizeof(ulen), 1, fp);
                    std::fwrite(blob.data(), 1, ulen, fp);
                    std::fclose(fp);
                }
            }
        }
    }
#endif

    bindProgUniforms(out, p);
    return true;
}

struct GlContextGuard
{
    explicit GlContextGuard(bool alreadyHeld = false)
        : m_locked(!alreadyHeld)
    {
#if defined(PS2X_LINUX_SDL2_SHIM)
        if (m_locked)
            ps2xLinuxGlLock();
#endif
    }
    ~GlContextGuard()
    {
#if defined(PS2X_LINUX_SDL2_SHIM)
        if (m_locked)
            ps2xLinuxGlUnlock();
#endif
    }
    GlContextGuard(const GlContextGuard &) = delete;
    GlContextGuard &operator=(const GlContextGuard &) = delete;
    bool m_locked = false;
};

struct MaliMesh
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t firstVertex = 0;
    bool indexed = false;
    bool dynamic = false;
    bool skinned = false;
    bool hasBox = false;
    bool hasIntermediateAlpha = false; // Phase 6B: any vertex alpha in (4, 124)
    float lo[3] = {};
    float hi[3] = {};
    uint8_t minAlpha = 0;
    uint8_t maxAlpha = 0xFF;
    bool pendingEvict = false;
    uint64_t gpuBytes = 0;
    uint64_t lastUsed = 0;
    // Phase 6C: Optional CPU source copy for safe GL_UNSIGNED_INT static run merging.
    std::vector<CompactVertex> srcVerts;
    std::vector<uint32_t> srcIndices;
    uint64_t srcBytes = 0;
};

struct MaliTex
{
    GLuint tex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    uint8_t minAlpha = 0;
    uint8_t maxAlpha = 0xFF;
    uint8_t wrapClamp = 0xFF; // Phase 5.1: cached GL_TEXTURE_WRAP_S/T state
    int16_t arrayId = -1;     // Phase 7C: GL_TEXTURE_2D_ARRAY bucket index
    uint8_t arrayLayer = 0;   // Phase 7C: layer within m_texArrays[arrayId]
    bool binaryAlpha = false; // Phase 6B: true when every level-0 texel is <=4 or >=124
    bool pendingEvict = false;
    bool astc = false;
    bool tex16 = false;
    bool has2D = false;       // Phase 7G: true only when standalone GL_TEXTURE_2D is allocated
    uint8_t fmtCode = 2;      // 0 = RGB565, 1 = RGB5_A1, 2 = RGBA8, 3..10 = ASTC
    uint64_t gpuBytes = 0;
    uint64_t lastUsed = 0;
    std::vector<uint8_t> rawData; // Phase 7G: kept for lazy 2D upload if a non-array draw needs tx.tex
    std::vector<uint32_t> astcLevelSizes;
};

struct MaliTexArray
{
    GLuint tex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    uint32_t capacity = 0;
    uint32_t used = 0;
    uint8_t fmtCode = 0;     // 0 = RGB565, 1 = RGB5_A1, 2 = RGBA8
    uint8_t wrapClamp = 0xFF;
    bool tex16 = false;
    uint64_t gpuBytes = 0;
    std::vector<uint8_t> freeLayers;
};

struct MaliTarget
{
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasDepth = true;
    bool depthAsTex = false;
};

struct SkinPaletteEntry
{
    uint32_t offsetFloats = 0;
    uint32_t qwordCount = 0;
};

// Phase 6C/7C: Cached merged static run (strictly GL_UNSIGNED_INT + 28B CompactVertex).
struct MergeRun
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    uint32_t indexCount = 0;
    int16_t texArrayId = -1;
    uint64_t bytes = 0;
    uint64_t lastUsed = 0;
    uint32_t frames = 0;
};

struct UploadCmd
{
    enum Kind : uint8_t { StaticMesh, IndexedMesh, SkinnedMesh, Texture };
    Kind kind = StaticMesh;
    uint64_t id = 0;
    uint32_t width = 0, height = 0, levels = 1;
    bool hasBox = false;
    bool hasIntermediateAlpha = false;
    float lo[3] = {}, hi[3] = {};
    uint8_t minAlpha = 0xFF;
    uint8_t maxAlpha = 0xFF;
    std::vector<CompactVertex> compactVerts;
    std::vector<SkinVertex> skinVerts;
    std::vector<uint32_t> indices;
    std::vector<uint8_t> rgba;
};

struct DynMeshRec
{
    uint64_t id = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint8_t minAlpha = 0;
    uint8_t maxAlpha = 0xFF;
    bool hasIntermediateAlpha = false;
};

struct RawDraw
{
    uint64_t meshId = 0;
    float clip[16] = {};
    DrawState state{};
    uint32_t seq = 0;
};

struct QueuedDraw
{
    uint64_t meshId = 0;
    const MaliMesh *mesh = nullptr;
    MaliTex *tex = nullptr; // Phase 5.1: pre-resolved once during queue build
    float clip[16] = {};
    uint64_t clipHash = 0;
    uint64_t stateHash = 0;
    DrawState state{};
    float minW = 0.0f;
    uint32_t seq = 0;
    uint32_t psoKey = 0;
    int16_t texArrayId = -1;
    uint8_t texArrayLayer = 0;
    uint8_t bucket = 0;
    bool skinned = false;
    bool deferredCull = false;
    bool b2Opaque = false;
};

struct FramePacket
{
    uint64_t frameNum = 0;
    uint32_t width = 0, height = 0;
    bool hold = false;
    Fog fog{};
    Bloom bloom{};
    DepthOfField dof{};
    std::vector<ScreenBlur> screenBlurs;
    std::vector<UploadCmd> uploads;
    std::vector<DynMeshRec> dynMeshes;
    std::vector<Vertex> dynVerts;
    std::vector<SkinPaletteEntry> skinPalettes;
    std::vector<float> skinPaletteData;
    std::vector<RawDraw> draws;
    std::vector<uint64_t> confirmEvictMeshes;
    std::vector<uint64_t> cancelEvictMeshes;
    std::vector<uint64_t> confirmEvictTex;
    std::vector<uint64_t> cancelEvictTex;

    void clearPerFrame()
    {
        // Note: uploads and eviction decisions are intentionally preserved across beginFrame()
        // so asyncBuilds().takeUp() uploads and 2-phase evictions are never dropped.
        dynMeshes.clear();
        dynVerts.clear();
        skinPalettes.clear();
        skinPaletteData.clear();
        draws.clear();
        screenBlurs.clear();
        dof = DepthOfField{};
        hold = false;
    }
};

inline uint32_t makePsoKey(uint32_t prog, Blend blend, bool depthTest, bool depthWrite, uint32_t depthFunc)
{
    return (prog & 31u)
         | ((static_cast<uint32_t>(blend) & 3u) << 5)
         | ((depthTest ? 1u : 0u) << 7)
         | ((depthWrite ? 1u : 0u) << 8)
         | ((depthFunc & 3u) << 9);
}

inline uint32_t toArrayProg(uint32_t progIdx)
{
    switch (progIdx)
    {
    case 0u: return 16u;
    case 1u: return 17u;
    case 6u: return 13u;
    case 7u: return 14u;
    case 8u: return 15u;
    case 12u: return 18u;
    default: return progIdx;
    }
}

inline bool cullAabb(const float *c, const float *lo, const float *hi, float &outMinW)
{
    auto maxPlane = [&](float a0, float a1, float a2, float a3) noexcept -> float
    {
        return a3 + (a0 >= 0.0f ? a0 * hi[0] : a0 * lo[0])
                  + (a1 >= 0.0f ? a1 * hi[1] : a1 * lo[1])
                  + (a2 >= 0.0f ? a2 * hi[2] : a2 * lo[2]);
    };
    const float maxW = maxPlane(c[3], c[7], c[11], c[15]);
    if (!std::isfinite(maxW))
        return false;
    if (maxW < 0.05f ||
        maxPlane(c[0] + c[3], c[4] + c[7], c[8] + c[11], c[12] + c[15]) < -1e-3f ||
        maxPlane(c[3] - c[0], c[7] - c[4], c[11] - c[8], c[15] - c[12]) < -1e-3f ||
        maxPlane(c[1] + c[3], c[5] + c[7], c[9] + c[11], c[13] + c[15]) < -1e-3f ||
        maxPlane(c[3] - c[1], c[7] - c[5], c[11] - c[9], c[15] - c[13]) < -1e-3f)
    {
        return true;
    }
    const float minW = c[15]
        + (c[3] >= 0.0f ? c[3] * lo[0] : c[3] * hi[0])
        + (c[7] >= 0.0f ? c[7] * lo[1] : c[7] * hi[1])
        + (c[11] >= 0.0f ? c[11] * lo[2] : c[11] * hi[2]);
    outMinW = std::isfinite(minW) ? minW : 0.0f;
    return false;
}

inline bool canMergeStatic(const QueuedDraw &a, const QueuedDraw &b)
{
    return a.bucket == b.bucket &&
           a.bucket <= 2u &&
           a.mesh && b.mesh &&
           !a.mesh->dynamic && !b.mesh->dynamic &&
           !a.skinned && !b.skinned &&
           !a.state.reflectionSample && !b.state.reflectionSample &&
           !a.mesh->srcVerts.empty() && !b.mesh->srcVerts.empty() &&
           a.state.order == b.state.order &&
           a.psoKey == b.psoKey &&
           ((a.texArrayId >= 0 && a.texArrayId == b.texArrayId) ||
            (a.texArrayId < 0 && b.texArrayId < 0 && a.state.texture == b.state.texture)) &&
           a.state.clamp == b.state.clamp &&
           a.clipHash == b.clipHash &&
           std::memcmp(a.clip, b.clip, 16u * sizeof(float)) == 0 &&
           a.state.alphaTest == b.state.alphaTest &&
           a.state.lit == b.state.lit &&
           a.state.fresnel == b.state.fresnel &&
           a.state.fogged == b.state.fogged &&
           a.state.background == b.state.background &&
           a.state.backgroundFge == b.state.backgroundFge &&
           a.state.opacity == b.state.opacity &&
           a.state.uvOffset[0] == b.state.uvOffset[0] &&
           a.state.uvOffset[1] == b.state.uvOffset[1] &&
           a.state.lodK == b.state.lodK &&
           a.state.lodScale == b.state.lodScale &&
           a.state.maxLod == b.state.maxLod &&
           ((!a.state.lit && !a.state.fresnel) || std::memcmp(a.state.light, b.state.light, sizeof(a.state.light)) == 0);
}

inline bool canInstanceStatic(const QueuedDraw &a, const QueuedDraw &b)
{
    return a.bucket == b.bucket &&
           a.bucket <= 2u &&
           a.mesh && b.mesh &&
           a.mesh == b.mesh &&
           !a.mesh->dynamic && !b.mesh->dynamic &&
           !a.skinned && !b.skinned &&
           !a.state.reflectionSample && !b.state.reflectionSample &&
           a.stateHash == b.stateHash &&
           a.state.order == b.state.order &&
           a.psoKey == b.psoKey &&
           a.texArrayId == b.texArrayId &&
           a.texArrayLayer == b.texArrayLayer &&
           a.state.texture == b.state.texture &&
           a.state.clamp == b.state.clamp &&
           a.state.alphaTest == b.state.alphaTest &&
           a.state.lit == b.state.lit &&
           a.state.fresnel == b.state.fresnel &&
           a.state.fogged == b.state.fogged &&
           a.state.background == b.state.background &&
           a.state.backgroundFge == b.state.backgroundFge &&
           a.state.opacity == b.state.opacity &&
           a.state.uvOffset[0] == b.state.uvOffset[0] &&
           a.state.uvOffset[1] == b.state.uvOffset[1] &&
           a.state.lodK == b.state.lodK &&
           a.state.lodScale == b.state.lodScale &&
           a.state.maxLod == b.state.maxLod &&
           ((!a.state.lit && !a.state.fresnel) || std::memcmp(a.state.light, b.state.light, sizeof(a.state.light)) == 0);
}

class MaliBackend final : public Backend
{
public:
    explicit MaliBackend(std::function<void()> makeCurrent)
        : m_makeCurrent(std::move(makeCurrent))
    {
        m_asyncEnabled = envBool("SOTC_MALI_ASYNC", true);
        m_mailboxEnabled = envBool("SOTC_MALI_MAILBOX", true);
        m_cullEnabled = envBool("SOTC_MALI_CULL", true);
        m_tex16Enabled = envBool("SOTC_MALI_TEX16", true);
        m_tex5551Enabled = envBool("SOTC_MALI_TEX5551", false);
        m_waterEnabled = envBool("SOTC_MALI_WATER", true);
        m_drsEnabled = envBool("SOTC_MALI_DRS", true);
        m_shaderCacheEnabled = envBool("SOTC_MALI_SHADER_CACHE", true);
        m_gpuSkinEnabled = envBool("SOTC_MALI_GPU_SKIN", true);
        m_mergeEnabled = envBool("SOTC_MALI_MERGE", true);
        m_texArrayEnabled = tuning().maliTexArray;
        m_instancingEnabled = envBool("SOTC_MALI_INSTANCING", true);
        m_keepCurrentEnabled = envBool("SOTC_MALI_KEEP_CURRENT", true);
        m_directScanoutEnabled = envBool("SOTC_MALI_DIRECT_SCANOUT", true);
        m_fastShadersEnabled = envBool("SOTC_MALI_FAST_SHADERS", true);
        m_drsDrawOnly = envBool("SOTC_MALI_DRS_DRAW_ONLY", false);
        m_drsMin = std::clamp(envFloat("SOTC_MALI_DRS_MIN", 0.60f), 0.45f, 1.0f);
        m_drsMax = std::clamp(envFloat("SOTC_MALI_DRS_MAX", 1.0f), m_drsMin, 1.0f);
        m_drsScale = m_drsMax;
        if (const char *sm = std::getenv("SOTC_SCREEN_MODE"); sm && sm[0])
            m_screenMode = std::atoi(sm);
        else if (std::getenv("SOTC_STRETCH"))
            m_screenMode = 1;
        {
            float cropRatio = 0.0f;
            if (const char *cr = std::getenv("SOTC_CROP_LETTERBOX"); cr && cr[0])
                cropRatio = std::clamp(std::strtof(cr, nullptr), 0.0f, 0.25f);
            else if (m_screenMode == 1 || std::getenv("SOTC_STRETCH"))
                cropRatio = 0.115f;
            m_hasLetterboxCrop = (cropRatio > 0.001f);
        }
        m_mergeBudgetUs = static_cast<uint64_t>(std::max(0, envInt("SOTC_MALI_MERGE_BUDGET_US", 2500)));
        m_mergeChunkMax = static_cast<uint32_t>(std::clamp(envInt("SOTC_MALI_MERGE_CHUNK", 64), 4, 128));
        m_cacheBudgetBytes = static_cast<uint64_t>(std::max(16, envInt("SOTC_MALI_CACHE_BUDGET_MB", 160))) * 1024ull * 1024ull;
        m_texArrayBudgetBytes = std::min(100ull * 1024ull * 1024ull, (m_cacheBudgetBytes * 5ull) / 8ull);
        m_atPass2Mode = std::clamp(envInt("SOTC_MALI_AT_PASS2", 1), 0, 2); // 0=off, 1=smart auto, 2=always
        m_zPrepassEnabled = envBool("SOTC_MALI_ZPREPASS", false);
        m_postEnabled = envBool("SOTC_MALI_POST", true);
        m_postBloomEnabled = envBool("SOTC_MALI_BLOOM", envBool("SOTC_MALI_POST_BLOOM", true));
        m_exposureEnabled = envBool("SOTC_MALI_EXPOSURE", true);
        m_dofEnabled = envBool("SOTC_MALI_DOF", false);
        m_motionBlurMode = std::clamp(envInt("SOTC_MALI_MOTION_BLUR", 0), 0, 2);
        m_caEnabled = envBool("SOTC_MALI_CA", false);
        m_lensFlareEnabled = envBool("SOTC_MALI_LENS_FLARE", false);
        m_aoEnabled = envBool("SOTC_MALI_AO", false);
        m_trilinearEnabled = envBool("SOTC_MALI_TRILINEAR", false);
        {
            const int an = envInt("SOTC_MALI_ANISO", 0);
            m_anisoLevel = an >= 4 ? 4 : (an >= 2 ? 2 : 0);
        }
        m_fxaaEnabled = envBool("SOTC_MALI_FXAA", false);
        m_rcasEnabled = envBool("SOTC_MALI_RCAS", envBool("SOTC_MALI_FSR_RCAS", false));
        m_postUpscaleEnabled = envBool("SOTC_MALI_POST_UPSCALE", false);
        m_dumpTexEnabled = envBool("SOTC_MALI_DUMP_TEX", false);
        m_paceEnabled = envBool("SOTC_MALI_PACE", false);
        m_statsEnabled = envBool("SOTC_MALI_STATS", false);
        m_uploadBudgetUs = static_cast<uint64_t>(std::max(0, envInt("SOTC_MALI_UPLOAD_BUDGET_US", 4000)));
        m_bgRate = static_cast<uint32_t>(std::max(1, envInt("SOTC_MALI_BG_RATE", 2)));
        {
            const int cap = envInt("SOTC_FPS_CAP", 30);
            m_targetFps = cap <= 0 ? 0 : std::max(20, cap);
        }
        m_drsTargetFps = std::max(15, envInt("SOTC_MALI_DRS_TARGET_FPS", 30));

        if (const char *tc = std::getenv("SOTC_MALI_TEX_CACHE"); tc && tc[0])
            m_astcCacheDir = tc;
        else if (const char *mc = std::getenv("PS2X_MEMCARD_DIR"); mc && mc[0])
            m_astcCacheDir = std::string(mc) + "/../tex_cache";

        if (const char *td = std::getenv("SOTC_MALI_TEX_DUMP_DIR"); td && td[0])
            m_texDumpDir = td;
        else
            m_texDumpDir = "/tmp/celosia_tex_dump";

        if (const char *sc = std::getenv("SOTC_MALI_SHADER_CACHE_DIR"); sc && sc[0])
            m_shaderCacheDir = sc;
        else if (const char *mc = std::getenv("PS2X_MEMCARD_DIR"); mc && mc[0])
            m_shaderCacheDir = std::string(mc) + "/../shader_cache";
        else
            m_shaderCacheDir = "/tmp/celosia_mali_shaders";
    }

    ~MaliBackend() override { shutdown(); }

    bool init()
    {
        if (m_asyncEnabled)
        {
            m_renderThread = std::thread(&MaliBackend::renderThreadMain, this);
            std::unique_lock<std::mutex> lk(m_mtx);
            m_cvDone.wait(lk, [this] { return m_initDone; });
            return m_initialized;
        }
        return initGlOnCurrentThread();
    }

    bool hasMesh(uint64_t id) override { return m_knownMeshes.find(id) != m_knownMeshes.end(); }
    uint64_t meshEvictions() override { return m_evictions.load(std::memory_order_relaxed); }
    bool canSkin() override { return m_gpuSkinEnabled; }

    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame) override
    {
        if (!vertices || !count)
            return;

        if (perFrame)
        {
            DynMeshRec rec{};
            rec.id = id;
            rec.firstVertex = static_cast<uint32_t>(m_rec.dynVerts.size());
            rec.vertexCount = count;
            uint8_t minA = 0xFF;
            bool interA = false;
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t a = vertices[i].a;
                if (a < minA) minA = a;
                if (a > 4u && a < 124u) interA = true;
            }
            rec.minAlpha = minA;
            rec.hasIntermediateAlpha = interA;
            m_rec.dynVerts.insert(m_rec.dynVerts.end(), vertices, vertices + count);
            m_rec.dynMeshes.push_back(rec);
            return;
        }

        m_knownMeshes.insert(id);
        UploadCmd cmd{};
        cmd.kind = UploadCmd::StaticMesh;
        cmd.id = id;
        computeBoundsAndAlpha(vertices, count, cmd.lo, cmd.hi, cmd.hasBox, cmd.minAlpha, cmd.hasIntermediateAlpha);
        packCompactVertices(vertices, count, cmd.compactVerts);
        m_rec.uploads.push_back(std::move(cmd));
    }

    void uploadIndexedMesh(uint64_t id, const Vertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount) override
    {
        if (!vertices || !count || !indices || !indexCount)
            return;
        m_knownMeshes.insert(id);
        UploadCmd cmd{};
        cmd.kind = UploadCmd::IndexedMesh;
        cmd.id = id;
        computeBoundsAndAlpha(vertices, count, cmd.lo, cmd.hi, cmd.hasBox, cmd.minAlpha, cmd.hasIntermediateAlpha);
        packCompactVertices(vertices, count, cmd.compactVerts);
        cmd.indices.assign(indices, indices + indexCount);
        m_rec.uploads.push_back(std::move(cmd));
    }

    void uploadSkinnedMesh(uint64_t id, const SkinVertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount) override
    {
        if (!vertices || !count)
            return;
        m_knownMeshes.insert(id);
        UploadCmd cmd{};
        cmd.kind = UploadCmd::SkinnedMesh;
        cmd.id = id;
        uint8_t minA = 0xFF;
        bool interA = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t a = vertices[i].a;
            if (a < minA) minA = a;
            if (a > 4u && a < 124u) interA = true;
        }
        cmd.minAlpha = minA;
        cmd.hasIntermediateAlpha = interA;
        cmd.skinVerts.assign(vertices, vertices + count);
        if (indices && indexCount)
            cmd.indices.assign(indices, indices + indexCount);
        m_rec.uploads.push_back(std::move(cmd));
    }

    uint32_t framePalette(const float *qwords, uint32_t qwordCount) override
    {
        if (!qwords || !qwordCount)
            return 0u;
        const uint32_t clamped = std::min(qwordCount, kMaxSkinQwords);
        SkinPaletteEntry entry{};
        entry.offsetFloats = static_cast<uint32_t>(m_rec.skinPaletteData.size());
        entry.qwordCount = clamped;
        m_rec.skinPaletteData.insert(m_rec.skinPaletteData.end(), qwords, qwords + clamped * 4u);
        m_rec.skinPalettes.push_back(entry);
        return static_cast<uint32_t>(m_rec.skinPalettes.size());
    }

    void rewriteFramePalette(uint32_t handle, const float *qwords, uint32_t qwordCount) override
    {
        if (!handle || handle > m_rec.skinPalettes.size() || !qwords || !qwordCount)
            return;
        SkinPaletteEntry &entry = m_rec.skinPalettes[handle - 1u];
        const uint32_t copyQwords = std::min(qwordCount, entry.qwordCount);
        std::memcpy(m_rec.skinPaletteData.data() + entry.offsetFloats, qwords, copyQwords * 4u * sizeof(float));
    }

    bool hasTexture(uint64_t id) override { return m_knownTextures.find(id) != m_knownTextures.end(); }

    void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels) override
    {
        if (!rgba || !width || !height)
            return;
        m_knownTextures.insert(id);
        UploadCmd cmd{};
        cmd.kind = UploadCmd::Texture;
        cmd.id = id;
        cmd.width = width;
        cmd.height = height;
        cmd.levels = std::max(1u, levels);
        size_t totalBytes = 0;
        uint32_t lw = width, lh = height;
        for (uint32_t lv = 0; lv < cmd.levels; ++lv)
        {
            totalBytes += static_cast<size_t>(lw) * lh * 4u;
            lw = std::max(1u, lw >> 1);
            lh = std::max(1u, lh >> 1);
        }
        cmd.rgba.assign(rgba, rgba + totalBytes);
        m_rec.uploads.push_back(std::move(cmd));
    }

    void beginFrame() override
    {
        ++m_recFrameNum;
        m_rec.clearPerFrame();
        m_rec.frameNum = m_recFrameNum;
        m_recDrawSeq = 0;
    }

    void draw(uint64_t meshId, const float clip[16], const DrawState &state) override
    {
        if (state.shadowVolume || state.reflectionSource)
            return;
        RawDraw rd{};
        rd.meshId = meshId;
        std::memcpy(rd.clip, clip, 16 * sizeof(float));
        rd.state = state;
        rd.seq = m_recDrawSeq++;
        m_rec.draws.push_back(rd);
    }

    void setFog(const Fog &fog) override { m_rec.fog = fog; }
    void setBloom(const Bloom &bloom) override { m_rec.bloom = bloom; }
    void addScreenBlur(const ScreenBlur &b) override { m_rec.screenBlurs.push_back(b); }
    void setGlowComposite(const GlowComposite &) override {}
    void setDepthOfField(const DepthOfField &d) override { m_rec.dof = d; }
    void holdFrame() override { m_rec.hold = true; }

    void endFrame(uint32_t width, uint32_t height) override
    {
        if (!m_initialized || !width || !height)
            return;
        m_rec.width = width;
        m_rec.height = height;

        if (!m_asyncEnabled)
        {
            executeFrame(m_rec);
            m_rec.uploads.clear();
            m_rec.confirmEvictMeshes.clear();
            m_rec.cancelEvictMeshes.clear();
            m_rec.confirmEvictTex.clear();
            m_rec.cancelEvictTex.clear();
            return;
        }

        // Phase 5.1 mailbox handover: never stalls game thread and never drops static uploads!
        std::unique_lock<std::mutex> lk(m_mtx);
        if (!m_mailboxEnabled)
            m_cvDone.wait(lk, [this] { return !m_hasPendingFrame || m_stopRequested; });
        if (m_stopRequested)
            return;

        if (!m_pendingEvictMeshes.empty() || !m_pendingEvictTex.empty())
        {
            auto isMeshInUse = [&](uint64_t id) {
                for (const auto &d : m_rec.draws)
                    if (d.meshId == id) return true;
                for (const auto &u : m_rec.uploads)
                    if (u.kind != UploadCmd::Texture && u.id == id) return true;
                if (m_hasPendingFrame)
                {
                    for (const auto &d : m_pending.draws)
                        if (d.meshId == id) return true;
                    for (const auto &u : m_pending.uploads)
                        if (u.kind != UploadCmd::Texture && u.id == id) return true;
                }
                return false;
            };
            auto isTexInUse = [&](uint64_t id) {
                for (const auto &d : m_rec.draws)
                    if (d.state.texture == id) return true;
                for (const auto &u : m_rec.uploads)
                    if (u.kind == UploadCmd::Texture && u.id == id) return true;
                if (m_hasPendingFrame)
                {
                    for (const auto &d : m_pending.draws)
                        if (d.state.texture == id) return true;
                    for (const auto &u : m_pending.uploads)
                        if (u.kind == UploadCmd::Texture && u.id == id) return true;
                }
                return false;
            };
            for (uint64_t id : m_pendingEvictMeshes)
            {
                if (isMeshInUse(id))
                    m_rec.cancelEvictMeshes.push_back(id);
                else
                {
                    m_knownMeshes.erase(id);
                    m_rec.confirmEvictMeshes.push_back(id);
                }
            }
            for (uint64_t id : m_pendingEvictTex)
            {
                if (isTexInUse(id))
                    m_rec.cancelEvictTex.push_back(id);
                else
                {
                    m_knownTextures.erase(id);
                    m_rec.confirmEvictTex.push_back(id);
                }
            }
            m_pendingEvictMeshes.clear();
            m_pendingEvictTex.clear();
        }

        if (m_hasPendingFrame)
        {
            // Merge unconsumed uploads and eviction decisions from m_pending ahead of m_rec before dropping old frame.
            if (!m_pending.uploads.empty())
            {
                m_pending.uploads.insert(m_pending.uploads.end(),
                                         std::make_move_iterator(m_rec.uploads.begin()),
                                         std::make_move_iterator(m_rec.uploads.end()));
                m_rec.uploads.swap(m_pending.uploads);
                m_pending.uploads.clear();
            }
            auto prependIds = [](std::vector<uint64_t> &pending, std::vector<uint64_t> &rec) {
                if (!pending.empty())
                {
                    pending.insert(pending.end(), rec.begin(), rec.end());
                    rec.swap(pending);
                    pending.clear();
                }
            };
            prependIds(m_pending.confirmEvictMeshes, m_rec.confirmEvictMeshes);
            prependIds(m_pending.cancelEvictMeshes, m_rec.cancelEvictMeshes);
            prependIds(m_pending.confirmEvictTex, m_rec.confirmEvictTex);
            prependIds(m_pending.cancelEvictTex, m_rec.cancelEvictTex);
            m_droppedFrames.fetch_add(1u, std::memory_order_relaxed);
        }
        std::swap(m_pending, m_rec);
        m_rec.uploads.clear();
        m_rec.confirmEvictMeshes.clear();
        m_rec.cancelEvictMeshes.clear();
        m_rec.confirmEvictTex.clear();
        m_rec.cancelEvictTex.clear();
        m_hasPendingFrame = true;
        lk.unlock();
        m_cvWork.notify_one();
    }

    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
    {
        finishFrames();
        if (!m_initialized)
            return false;
        if (m_asyncEnabled)
        {
            std::unique_lock<std::mutex> lk(m_mtx);
            m_readbackReq = true;
            m_readbackDone = false;
            m_cvWork.notify_one();
            m_cvDone.wait(lk, [this] { return m_readbackDone || m_stopRequested; });
            if (m_readbackRgba.empty())
                return false;
            rgba = m_readbackRgba;
            width = m_readbackW;
            height = m_readbackH;
            return true;
        }
        return readbackOnGlThread(rgba, width, height);
    }

    void finishFrames() override
    {
        if (!m_asyncEnabled || !m_renderThread.joinable())
            return;
        std::unique_lock<std::mutex> lk(m_mtx);
        m_cvDone.wait(lk, [this] { return (!m_hasPendingFrame && !m_renderingActive) || m_stopRequested; });
    }

    void shutdown() override
    {
        if (m_asyncEnabled && m_renderThread.joinable())
        {
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_stopRequested = true;
            }
            m_cvWork.notify_one();
            m_renderThread.join();
            return;
        }
        shutdownGlOnCurrentThread();
    }

private:
    void renderThreadMain()
    {
#if defined(__linux__)
        native_renderer::threads::place(native_renderer::threads::kRender);
#endif
        const bool ok = initGlOnCurrentThread();
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_initDone = true;
            if (!ok)
                m_stopRequested = true;
        }
        m_cvDone.notify_all();
        if (!ok)
            return;

        bool glHeld = false;
        auto releaseHeld = [&]
        {
            if (glHeld)
            {
#if defined(PS2X_LINUX_SDL2_SHIM)
                ps2xLinuxGlUnlock();
#endif
                glHeld = false;
            }
        };

        FramePacket local; // Reused across frames to avoid vector reallocations
        for (;;)
        {
            bool doFrame = false;
            bool doReadback = false;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
#if defined(PS2X_LINUX_SDL2_SHIM)
                if (glHeld && (m_stopRequested || (!m_hasPendingFrame && !m_readbackReq) || ps2xLinuxGlHasWaiter()))
                {
                    lk.unlock();
                    releaseHeld();
                    lk.lock();
                }
#endif
                m_cvWork.wait(lk, [this] { return m_hasPendingFrame || m_readbackReq || m_stopRequested; });
                if (m_stopRequested && !m_hasPendingFrame)
                {
                    lk.unlock();
                    releaseHeld();
                    break;
                }
#if defined(PS2X_LINUX_SDL2_SHIM)
                if (m_keepCurrentEnabled && !glHeld)
                {
                    lk.unlock();
                    ps2xLinuxGlLock();
                    glHeld = true;
                    lk.lock();
                    if (m_stopRequested && !m_hasPendingFrame)
                    {
                        lk.unlock();
                        releaseHeld();
                        break;
                    }
                }
#endif
                if (m_hasPendingFrame)
                {
                    local.uploads.clear();
                    local.confirmEvictMeshes.clear();
                    local.cancelEvictMeshes.clear();
                    local.confirmEvictTex.clear();
                    local.cancelEvictTex.clear();
                    std::swap(local, m_pending);
                    m_hasPendingFrame = false;
                    m_renderingActive = true;
                    doFrame = true;
                    m_cvDone.notify_all();
                }
                else if (m_readbackReq)
                {
                    doReadback = true;
                }
            }

            if (doFrame)
            {
                executeFrame(local, glHeld);
                local.uploads.clear();
                local.confirmEvictMeshes.clear();
                local.cancelEvictMeshes.clear();
                local.confirmEvictTex.clear();
                local.cancelEvictTex.clear();
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_renderingActive = false;
                }
                m_cvDone.notify_all();
            }
            else if (doReadback)
            {
                std::vector<uint8_t> tmp;
                uint32_t rw = 0, rh = 0;
                readbackOnGlThread(tmp, rw, rh, glHeld);
                {
                    std::lock_guard<std::mutex> lk(m_mtx);
                    m_readbackRgba.swap(tmp);
                    m_readbackW = rw;
                    m_readbackH = rh;
                    m_readbackReq = false;
                    m_readbackDone = true;
                }
                m_cvDone.notify_all();
            }
        }
        releaseHeld();
        shutdownGlOnCurrentThread();
    }

    bool initGlOnCurrentThread()
    {
        GlContextGuard guard;
        if (m_makeCurrent)
            m_makeCurrent();

#if defined(__linux__)
        if (m_shaderCacheEnabled && !m_shaderCacheDir.empty())
            ::mkdir(m_shaderCacheDir.c_str(), 0755);
        if (m_dumpTexEnabled && !m_texDumpDir.empty())
            ::mkdir(m_texDumpDir.c_str(), 0755);
#endif

        GLint numExt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
        for (GLint i = 0; i < numExt; ++i)
        {
            const char *ext = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (!ext)
                continue;
            if (std::strcmp(ext, "GL_KHR_texture_compression_astc_ldr") == 0)
                m_hasAstc = true;
            else if (std::strcmp(ext, "GL_EXT_texture_filter_anisotropic") == 0)
                m_hasAniso = true;
        }
        if (m_hasAniso)
        {
            GLfloat maxAniso = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
            m_maxAniso = std::clamp(maxAniso, 1.0f, 16.0f);
        }

        GLint maxLayers = 64;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
        m_maxArrayLayers = static_cast<uint32_t>(std::clamp(maxLayers, 16, 256));

#if defined(__linux__)
        if (m_shaderCacheEnabled && !m_shaderCacheDir.empty())
            ::mkdir(m_shaderCacheDir.c_str(), 0755);
        if (m_dumpTexEnabled && !m_texDumpDir.empty())
            ::mkdir(m_texDumpDir.c_str(), 0755);
        m_astcFiles.clear();
        if (m_hasAstc && !m_astcCacheDir.empty())
        {
            if (DIR *dp = ::opendir(m_astcCacheDir.c_str()))
            {
                while (struct dirent *de = ::readdir(dp))
                {
                    if (de->d_name[0] == '.')
                        continue;
                    const std::string fn(de->d_name);
                    if ((fn.size() > 6 && fn.compare(fn.size() - 6, 6, ".astcm") == 0) ||
                        (fn.size() > 5 && fn.compare(fn.size() - 5, 5, ".astc") == 0))
                    {
                        m_astcFiles.insert(fn);
                    }
                }
                ::closedir(dp);
            }
        }
#endif

        uint32_t shHits = 0u, shMisses = 0u;
        auto linkTracked = [&](ShaderProg &out, const char *name, const char *vsSrc, const char *fsSrc) -> bool {
            bool fromCache = false;
            if (!linkProgCached(out, name, vsSrc, fsSrc, m_shaderCacheDir, m_shaderCacheEnabled, &fromCache))
                return false;
            if (fromCache) ++shHits;
            else ++shMisses;
            return true;
        };

        if (!linkTracked(m_progOpaque, "opaque", kMaliWorldVert, kMaliOpaqueFrag) ||
            !linkTracked(m_progCut, "cut", kMaliWorldVert, kMaliCutFrag) ||
            !linkTracked(m_progScreen, "screen", kMaliScreenVert, kMaliScreenFrag) ||
            !linkTracked(m_progSkinOpaque, "skin_opaque", kMaliSkinVert, kMaliOpaqueFrag) ||
            !linkTracked(m_progSkinCut, "skin_cut", kMaliSkinVert, kMaliCutFrag) ||
            !linkTracked(m_progDepth, "depth", kMaliDepthVert, kMaliDepthFrag) ||
            !linkTracked(m_progPost, "post", kMaliPostVert, kMaliPostFrag) ||
            !linkTracked(m_progOpaqueFast, "opaque_fast", kMaliWorldVert, kMaliOpaqueFastFrag) ||
            !linkTracked(m_progCutFast, "cut_fast", kMaliWorldVert, kMaliCutFastFrag) ||
            !linkTracked(m_progSoftFast, "soft_fast", kMaliWorldVert, kMaliSoftFastFrag) ||
            !linkTracked(m_progSoft, "soft", kMaliWorldVert, kMaliSoftFrag) ||
            !linkTracked(m_progSkinOpaqueFast, "skin_opaque_fast", kMaliSkinVert, kMaliOpaqueFastFrag) ||
            !linkTracked(m_progSkinCutFast, "skin_cut_fast", kMaliSkinVert, kMaliCutFastFrag) ||
            !linkTracked(m_progSkinSoftFast, "skin_soft_fast", kMaliSkinVert, kMaliSoftFastFrag) ||
            !linkTracked(m_progArrOpaqueFast, "arr_opaque_fast", kMaliWorldArrVert, kMaliArrOpaqueFastFrag) ||
            !linkTracked(m_progArrCutFast, "arr_cut_fast", kMaliWorldArrVert, kMaliArrCutFastFrag) ||
            !linkTracked(m_progArrSoftFast, "arr_soft_fast", kMaliWorldArrVert, kMaliArrSoftFastFrag) ||
            !linkTracked(m_progArrOpaque, "arr_opaque", kMaliWorldArrVert, kMaliArrOpaqueFrag) ||
            !linkTracked(m_progArrCut, "arr_cut", kMaliWorldArrVert, kMaliArrCutFrag) ||
            !linkTracked(m_progArrSoft, "arr_soft", kMaliWorldArrVert, kMaliArrSoftFrag) ||
            !linkTracked(m_progWater, "water", kMaliWorldVert, kMaliWaterFrag) ||
            !linkTracked(m_progBloomMask, "bloom_mask", kMaliPostVert, kMaliBloomMaskFrag) ||
            !linkTracked(m_progKawase, "kawase", kMaliPostVert, kMaliKawaseFrag) ||
            !linkTracked(m_progTemporal, "temporal", kMaliPostVert, kMaliTemporalFrag) ||
            !linkTracked(m_progAo, "ao", kMaliPostVert, kMaliAoFrag) ||
            !linkTracked(m_progAoBlur, "ao_blur", kMaliPostVert, kMaliAoBlurFrag))
        {
            return false;
        }

        glGenVertexArrays(1, &m_emptyVao);
        glGenSamplers(8, m_samplers);
        updateSamplersOnGl();

        glGenTextures(1, &m_whiteTex);
        glBindTexture(GL_TEXTURE_2D, m_whiteTex);
        const uint8_t white[4] = {128, 128, 128, 128};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        glGenVertexArrays(3, m_arenaVao);
        glGenBuffers(3, m_arenaVbo);
        for (int i = 0; i < 3; ++i)
        {
            glBindVertexArray(m_arenaVao[i]);
            glBindBuffer(GL_ARRAY_BUFFER, m_arenaVbo[i]);
            setupDynamicVertexAttribs();
        }
        glBindVertexArray(0);

        makeTarget(m_bgTarget, 256, 256, true);
        m_lastPresentTime = std::chrono::steady_clock::now();
        m_initialized = true;
        std::fprintf(stderr, "[mali] Phase 7G+E GLES 3.2 backend up (async=%d mailbox=%d keepCur=%d direct=%d fastSh=%d shCache=%u/%u astcIdx=%zu texArr=%d(maxL=%u,bud=%lluM) merge=%d(chunk=%u) inst=%d atPass2=%d cull=%d skin=%d tex5551=%d water=%d trilin=%d aniso=%d(max=%.0f) fxaa=%d rcas=%d bloom=%d exp=%d dof=%d mb=%d ca=%d flare=%d ao=%d upBud=%llu drs=%d@%dfps[%.2f..%.2f] drawOnly=%d budget=%lluM)\n",
                     m_asyncEnabled ? 1 : 0, m_mailboxEnabled ? 1 : 0, m_keepCurrentEnabled ? 1 : 0,
                     m_directScanoutEnabled ? 1 : 0, m_fastShadersEnabled ? 1 : 0,
                     shHits, shMisses, m_astcFiles.size(),
                     m_texArrayEnabled ? 1 : 0, m_maxArrayLayers,
                     static_cast<unsigned long long>(m_texArrayBudgetBytes >> 20),
                     m_mergeEnabled ? 1 : 0, m_mergeChunkMax, m_instancingEnabled ? 1 : 0,
                     m_atPass2Mode, m_cullEnabled ? 1 : 0, m_gpuSkinEnabled ? 1 : 0,
                     m_tex5551Enabled ? 1 : 0, m_waterEnabled ? 1 : 0,
                     m_trilinearEnabled ? 1 : 0, m_anisoLevel, m_maxAniso,
                     m_fxaaEnabled ? 1 : 0, m_rcasEnabled ? 1 : 0,
                     m_postBloomEnabled ? 1 : 0, m_exposureEnabled ? 1 : 0,
                     m_dofEnabled ? 1 : 0, m_motionBlurMode, m_caEnabled ? 1 : 0,
                     m_lensFlareEnabled ? 1 : 0, m_aoEnabled ? 1 : 0,
                     static_cast<unsigned long long>(m_uploadBudgetUs),
                     m_drsEnabled ? 1 : 0, m_drsTargetFps, m_drsMin, m_drsMax, m_drsDrawOnly ? 1 : 0,
                     static_cast<unsigned long long>(m_cacheBudgetBytes >> 20));
        return true;
    }

    void shutdownGlOnCurrentThread()
    {
        if (!m_initialized)
            return;
        GlContextGuard guard;
        glDeleteVertexArrays(3, m_arenaVao);
        glDeleteBuffers(3, m_arenaVbo);
        if (m_samplers[0]) { glBindSampler(0, 0); glDeleteSamplers(8, m_samplers); std::memset(m_samplers, 0, sizeof(m_samplers)); }
        if (m_emptyVao) { glDeleteVertexArrays(1, &m_emptyVao); m_emptyVao = 0; }
        for (auto &[k, r] : m_mergeRuns)
        {
            if (r.vao) glDeleteVertexArrays(1, &r.vao);
            if (r.vbo) glDeleteBuffers(1, &r.vbo);
            if (r.ibo) glDeleteBuffers(1, &r.ibo);
        }
        m_mergeRuns.clear();
        m_mergeRunBytes = 0;
        for (auto &[id, m] : m_meshes)
        {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.ibo) glDeleteBuffers(1, &m.ibo);
        }
        m_meshes.clear();
        m_mergeSrcBytes = 0;
        m_gpuMeshBytes = 0;
        for (auto &[id, t] : m_textures)
            if (t.tex) glDeleteTextures(1, &t.tex);
        m_textures.clear();
        m_gpuTexBytes = 0;
        for (MaliTexArray &arr : m_texArrays)
            if (arr.tex) glDeleteTextures(1, &arr.tex);
        m_texArrays.clear();
        m_gpuTexArrayBytes = 0;
        destroyTarget(m_mainTarget[0]);
        destroyTarget(m_mainTarget[1]);
        destroyTarget(m_postTarget);
        destroyTarget(m_bgTarget);
        destroyTarget(m_bloomQuarter);
        destroyTarget(m_bloomEighth);
        destroyTarget(m_bloomHist[0]);
        destroyTarget(m_bloomHist[1]);
        destroyTarget(m_dofHalf);
        destroyTarget(m_aoTarget);
        destroyTarget(m_aoBlur);
        if (m_whiteTex) { glDeleteTextures(1, &m_whiteTex); m_whiteTex = 0; }
        auto delProg = [](ShaderProg &p) { if (p.id) { glDeleteProgram(p.id); p.id = 0; } };
        delProg(m_progOpaque);
        delProg(m_progCut);
        delProg(m_progScreen);
        delProg(m_progSkinOpaque);
        delProg(m_progSkinCut);
        delProg(m_progDepth);
        delProg(m_progPost);
        delProg(m_progOpaqueFast);
        delProg(m_progCutFast);
        delProg(m_progSoftFast);
        delProg(m_progSoft);
        delProg(m_progSkinOpaqueFast);
        delProg(m_progSkinCutFast);
        delProg(m_progSkinSoftFast);
        delProg(m_progArrOpaqueFast);
        delProg(m_progArrCutFast);
        delProg(m_progArrSoftFast);
        delProg(m_progArrOpaque);
        delProg(m_progArrCut);
        delProg(m_progArrSoft);
        delProg(m_progWater);
        delProg(m_progBloomMask);
        delProg(m_progKawase);
        delProg(m_progTemporal);
        delProg(m_progAo);
        delProg(m_progAoBlur);
        m_deferredUploads.clear();
        m_bloomHistValid = false;
        m_prevMainValid = false;
        m_initialized = false;
    }

    bool readbackOnGlThread(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, bool glAlreadyHeld = false)
    {
        if (!m_initialized)
            return false;
        if (m_lastPresentedDirect)
        {
            if (!m_lastDirectW || !m_lastDirectH)
                return false;
            GlContextGuard guard(glAlreadyHeld);
            width = m_lastDirectW;
            height = m_lastDirectH;
            rgba.resize(static_cast<size_t>(width) * height * 4u);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            return true;
        }
        const MaliTarget &src = (m_postTarget.fbo && m_lastPresentedPost) ? m_postTarget : m_mainTarget[m_lastPresentedMain];
        if (!src.fbo)
            return false;
        GlContextGuard guard(glAlreadyHeld);
        width = src.width;
        height = src.height;
        rgba.resize(static_cast<size_t>(width) * height * 4u);
        glBindFramebuffer(GL_FRAMEBUFFER, src.fbo);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void deleteMeshGpu(MaliMesh &m)
    {
        if (m_lastVao == m.vao && m.vao != 0u)
            m_lastVao = ~0u;
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.ibo) glDeleteBuffers(1, &m.ibo);
        if (m_gpuMeshBytes >= m.gpuBytes)
            m_gpuMeshBytes -= m.gpuBytes;
        else
            m_gpuMeshBytes = 0;
        if (m_mergeSrcBytes >= m.srcBytes)
            m_mergeSrcBytes -= m.srcBytes;
        else
            m_mergeSrcBytes = 0;
        m.vao = m.vbo = m.ibo = 0;
        m.gpuBytes = 0;
        m.srcBytes = 0;
    }

    void processUploads(FramePacket &pkt)
    {
        m_uploadUsThisFrame = 0;

        for (uint64_t id : pkt.cancelEvictMeshes)
        {
            if (auto it = m_meshes.find(id); it != m_meshes.end())
            {
                it->second.pendingEvict = false;
                it->second.lastUsed = pkt.frameNum;
            }
        }
        for (uint64_t id : pkt.confirmEvictMeshes)
        {
            if (auto it = m_meshes.find(id); it != m_meshes.end())
            {
                deleteMeshGpu(it->second);
                m_meshes.erase(it);
                m_evictions.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        for (uint64_t id : pkt.cancelEvictTex)
        {
            if (auto it = m_textures.find(id); it != m_textures.end())
            {
                it->second.pendingEvict = false;
                it->second.lastUsed = pkt.frameNum;
            }
        }
        for (uint64_t id : pkt.confirmEvictTex)
        {
            if (auto it = m_textures.find(id); it != m_textures.end())
            {
                releaseTexArrayLayer(it->second.arrayId, it->second.arrayLayer);
                if (m_lastTex == it->second.tex && it->second.tex != 0u)
                    m_lastTex = ~0u;
                if (it->second.tex) glDeleteTextures(1, &it->second.tex);
                if (m_gpuTexBytes >= it->second.gpuBytes)
                    m_gpuTexBytes -= it->second.gpuBytes;
                else
                    m_gpuTexBytes = 0;
                m_textures.erase(it);
                m_evictions.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        if (!pkt.confirmEvictTex.empty())
            reclaimEmptyTexArrays();

        auto uploadCmdOnGl = [&](UploadCmd &cmd)
        {
            if (cmd.kind == UploadCmd::StaticMesh || cmd.kind == UploadCmd::IndexedMesh)
            {
                MaliMesh &mesh = m_meshes[cmd.id];
                mesh.pendingEvict = false;
                mesh.lastUsed = pkt.frameNum;
                mesh.indexed = (cmd.kind == UploadCmd::IndexedMesh);
                mesh.dynamic = false;
                mesh.skinned = false;
                mesh.vertexCount = static_cast<uint32_t>(cmd.compactVerts.size());
                mesh.indexCount = mesh.indexed ? static_cast<uint32_t>(cmd.indices.size()) : 0u;
                mesh.hasBox = cmd.hasBox;
                mesh.hasIntermediateAlpha = cmd.hasIntermediateAlpha;
                std::memcpy(mesh.lo, cmd.lo, sizeof(mesh.lo));
                std::memcpy(mesh.hi, cmd.hi, sizeof(mesh.hi));
                mesh.minAlpha = cmd.minAlpha;

                const uint64_t newGpuBytes = static_cast<uint64_t>(cmd.compactVerts.size()) * sizeof(CompactVertex) +
                                             (mesh.indexed ? static_cast<uint64_t>(cmd.indices.size()) * sizeof(uint32_t) : 0ull);
                if (m_gpuMeshBytes >= mesh.gpuBytes)
                    m_gpuMeshBytes -= mesh.gpuBytes;
                mesh.gpuBytes = newGpuBytes;
                m_gpuMeshBytes += newGpuBytes;

                if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
                if (mesh.indexed && !mesh.ibo) glGenBuffers(1, &mesh.ibo);
                if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
                glBindVertexArray(mesh.vao);
                glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(cmd.compactVerts.size() * sizeof(CompactVertex)),
                             cmd.compactVerts.data(), GL_STATIC_DRAW);
                setupCompactVertexAttribs();
                if (mesh.indexed)
                {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(cmd.indices.size() * sizeof(uint32_t)),
                                 cmd.indices.data(), GL_STATIC_DRAW);
                }
                glBindVertexArray(0);

                // Phase 6C: Keep CPU source copy for safe GL_UNSIGNED_INT static run merging within budget.
                if (m_mergeEnabled && !cmd.compactVerts.empty())
                {
                    const uint64_t needBytes = cmd.compactVerts.size() * sizeof(CompactVertex) +
                                               (mesh.indexed ? cmd.indices.size() : cmd.compactVerts.size()) * sizeof(uint32_t);
                    if (m_mergeSrcBytes >= mesh.srcBytes)
                        m_mergeSrcBytes -= mesh.srcBytes;
                    mesh.srcVerts.clear();
                    mesh.srcIndices.clear();
                    mesh.srcBytes = 0;
                    if (m_mergeSrcBytes + needBytes > kMaxMergeSrcBytes)
                        evictOldMergeSrc(pkt.frameNum, 30u, needBytes);
                    if (m_mergeSrcBytes + needBytes <= kMaxMergeSrcBytes)
                    {
                        mesh.srcVerts = std::move(cmd.compactVerts);
                        if (mesh.indexed)
                        {
                            mesh.srcIndices = std::move(cmd.indices);
                        }
                        else
                        {
                            mesh.srcIndices.resize(mesh.srcVerts.size());
                            for (uint32_t i = 0; i < mesh.srcVerts.size(); ++i)
                                mesh.srcIndices[i] = i;
                        }
                        mesh.srcBytes = needBytes;
                        m_mergeSrcBytes += needBytes;
                    }
                }
            }
            else if (cmd.kind == UploadCmd::SkinnedMesh)
            {
                MaliMesh &mesh = m_meshes[cmd.id];
                mesh.pendingEvict = false;
                mesh.lastUsed = pkt.frameNum;
                mesh.dynamic = false;
                mesh.skinned = true;
                mesh.indexed = !cmd.indices.empty();
                mesh.vertexCount = static_cast<uint32_t>(cmd.skinVerts.size());
                mesh.indexCount = static_cast<uint32_t>(cmd.indices.size());
                mesh.hasBox = false;
                mesh.hasIntermediateAlpha = cmd.hasIntermediateAlpha;
                mesh.minAlpha = cmd.minAlpha;
                const uint64_t newGpuBytes = static_cast<uint64_t>(cmd.skinVerts.size()) * sizeof(SkinVertex) +
                                             (mesh.indexed ? static_cast<uint64_t>(cmd.indices.size()) * sizeof(uint32_t) : 0ull);
                if (m_gpuMeshBytes >= mesh.gpuBytes)
                    m_gpuMeshBytes -= mesh.gpuBytes;
                mesh.gpuBytes = newGpuBytes;
                m_gpuMeshBytes += newGpuBytes;
                if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
                if (mesh.indexed && !mesh.ibo) glGenBuffers(1, &mesh.ibo);
                if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
                glBindVertexArray(mesh.vao);
                glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
                glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(cmd.skinVerts.size() * sizeof(SkinVertex)),
                             cmd.skinVerts.data(), GL_STATIC_DRAW);
                setupSkinVertexAttribs();
                if (mesh.indexed)
                {
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
                    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(cmd.indices.size() * sizeof(uint32_t)),
                                 cmd.indices.data(), GL_STATIC_DRAW);
                }
                glBindVertexArray(0);
            }
            else if (cmd.kind == UploadCmd::Texture)
            {
                uploadTextureOnGl(cmd, pkt.frameNum);
            }
        };

        for (UploadCmd &u : pkt.uploads)
        {
            if (u.kind == UploadCmd::SkinnedMesh)
                m_deferredUploads.push_front(std::move(u));
            else
                m_deferredUploads.push_back(std::move(u));
        }
        pkt.uploads.clear();

        const uint64_t uploadStartUs = (m_uploadBudgetUs || m_mergeBudgetUs) ? nowUs() : 0u;
        uint32_t uploadedCount = 0u;
        while (!m_deferredUploads.empty())
        {
            const bool isSkinned = (m_deferredUploads.front().kind == UploadCmd::SkinnedMesh);
            if (!isSkinned && m_uploadBudgetUs > 0u && uploadedCount >= 8u &&
                (nowUs() - uploadStartUs) >= m_uploadBudgetUs)
            {
                break;
            }
            UploadCmd cmd = std::move(m_deferredUploads.front());
            m_deferredUploads.pop_front();
            ++uploadedCount;
            uploadCmdOnGl(cmd);
        }

        // Never leave a mesh or texture in m_deferredUploads if it is actually drawn in pkt.draws
        // this frame (since m_knownMeshes/m_knownTextures already marked it ready on the game thread).
        // Index the remaining deferred items first (typically a few entries) instead of probing
        // m_meshes/m_textures for every draw before executeFrame does the same.
        if (!m_deferredUploads.empty() && !pkt.draws.empty())
        {
            m_drawnMeshIdsScratch.clear();
            m_drawnTexIdsScratch.clear();
            for (const UploadCmd &u : m_deferredUploads)
            {
                if (u.kind == UploadCmd::Texture)
                    m_drawnTexIdsScratch.insert(u.id);
                else
                    m_drawnMeshIdsScratch.insert(u.id);
            }
            for (const RawDraw &rd : pkt.draws)
            {
                if (!m_drawnMeshIdsScratch.empty())
                    m_drawnMeshIdsScratch.erase(rd.meshId);
                if (rd.state.texture && !m_drawnTexIdsScratch.empty())
                    m_drawnTexIdsScratch.erase(rd.state.texture);
                if (m_drawnMeshIdsScratch.empty() && m_drawnTexIdsScratch.empty())
                    break;
            }
            for (auto it = m_deferredUploads.begin(); it != m_deferredUploads.end();)
            {
                const bool stillDeferred = (it->kind == UploadCmd::Texture)
                    ? (m_drawnTexIdsScratch.count(it->id) != 0u)
                    : (m_drawnMeshIdsScratch.count(it->id) != 0u);
                if (!stillDeferred)
                {
                    UploadCmd cmd = std::move(*it);
                    it = m_deferredUploads.erase(it);
                    uploadCmdOnGl(cmd);
                }
                else
                {
                    ++it;
                }
            }
        }
        if (uploadStartUs)
            m_uploadUsThisFrame = nowUs() - uploadStartUs;

        for (const DynMeshRec &rec : pkt.dynMeshes)
        {
            MaliMesh &mesh = m_meshes[rec.id];
            mesh.lastUsed = pkt.frameNum;
            mesh.indexed = false;
            mesh.dynamic = true;
            mesh.skinned = false;
            mesh.hasBox = false;
            mesh.hasIntermediateAlpha = rec.hasIntermediateAlpha;
            mesh.firstVertex = rec.firstVertex;
            mesh.vertexCount = rec.vertexCount;
            mesh.indexCount = 0;
            mesh.minAlpha = rec.minAlpha;
        }
    }

    void updateSamplersOnGl()
    {
        if (!m_samplers[0])
            return;
        const GLfloat aniso = (m_hasAniso && m_anisoLevel > 0)
            ? std::min(m_maxAniso, static_cast<GLfloat>(m_anisoLevel))
            : 1.0f;
        for (uint32_t i = 0; i < 8u; ++i)
        {
            const bool hasMips = (i >= 4u);
            const uint32_t clamp = i & 3u;
            const GLint minFilter = hasMips
                ? (m_trilinearEnabled ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                : GL_LINEAR;
            glSamplerParameteri(m_samplers[i], GL_TEXTURE_MIN_FILTER, minFilter);
            glSamplerParameteri(m_samplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_S, (clamp & 1u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glSamplerParameteri(m_samplers[i], GL_TEXTURE_WRAP_T, (clamp & 2u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            if (m_hasAniso)
                glSamplerParameterf(m_samplers[i], GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
        }
        m_lastSamplerIdx = ~0u;
    }

    void releaseTexArrayLayer(int16_t arrayId, uint8_t layer)
    {
        if (arrayId >= 0 && static_cast<size_t>(arrayId) < m_texArrays.size() && m_texArrays[arrayId].tex != 0u)
            m_texArrays[arrayId].freeLayers.push_back(layer);
    }

    bool reclaimEmptyTexArrays()
    {
        bool reclaimed = false;
        for (size_t i = 0; i < m_texArrays.size(); ++i)
        {
            MaliTexArray &arr = m_texArrays[i];
            if (arr.tex == 0u || arr.freeLayers.size() < arr.used)
                continue;
            if (m_lastTexArr == arr.tex)
                m_lastTexArr = ~0u;
            glDeleteTextures(1, &arr.tex);
            if (m_gpuTexArrayBytes >= arr.gpuBytes)
                m_gpuTexArrayBytes -= arr.gpuBytes;
            else
                m_gpuTexArrayBytes = 0;
            arr = MaliTexArray{};
            const int16_t deadId = static_cast<int16_t>(i);
            for (auto it = m_mergeRuns.begin(); it != m_mergeRuns.end();)
            {
                if (it->second.texArrayId == deadId)
                {
                    MergeRun &er = it->second;
                    if (m_lastVao == er.vao)
                        m_lastVao = ~0u;
                    if (er.vao) glDeleteVertexArrays(1, &er.vao);
                    if (er.vbo) glDeleteBuffers(1, &er.vbo);
                    if (er.ibo) glDeleteBuffers(1, &er.ibo);
                    if (m_mergeRunBytes >= er.bytes)
                        m_mergeRunBytes -= er.bytes;
                    else
                        m_mergeRunBytes = 0;
                    it = m_mergeRuns.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            reclaimed = true;
        }
        return reclaimed;
    }

    bool allocTexArraySlot(uint32_t w, uint32_t h, uint32_t levels, uint8_t fmtCode, int16_t &outArrId, uint8_t &outLayer)
    {
        outArrId = -1;
        outLayer = 0;
        if (!m_texArrayEnabled || w < 8u || w > 256u || h < 8u || h > 256u || levels == 0u)
            return false;

        for (size_t i = 0; i < m_texArrays.size(); ++i)
        {
            MaliTexArray &arr = m_texArrays[i];
            if (!arr.tex || arr.width != w || arr.height != h || arr.levels != levels || arr.fmtCode != fmtCode)
                continue;
            if (!arr.freeLayers.empty())
            {
                outArrId = static_cast<int16_t>(i);
                outLayer = arr.freeLayers.back();
                arr.freeLayers.pop_back();
                return true;
            }
            if (arr.used < arr.capacity)
            {
                outArrId = static_cast<int16_t>(i);
                outLayer = static_cast<uint8_t>(arr.used++);
                return true;
            }
        }

        auto findDeadSlot = [this]() -> size_t {
            for (size_t i = 0; i < m_texArrays.size(); ++i)
            {
                if (m_texArrays[i].tex == 0u)
                    return i;
            }
            return SIZE_MAX;
        };

        size_t reuseIdx = findDeadSlot();
        if (reuseIdx == SIZE_MAX && m_texArrays.size() >= 256u)
        {
            if (reclaimEmptyTexArrays())
                reuseIdx = findDeadSlot();
            if (reuseIdx == SIZE_MAX)
                return false;
        }

        uint8_t bx = 0, by = 0;
        const GLenum internalFmt = fmtCodeToGlInternal(fmtCode, bx, by);
#if !defined(PS2X_MALI_GLES)
        if (bx > 0u)
            return false;
#endif
        const uint32_t maxDim = std::max(w, h);
        const uint32_t tierCap = (maxDim >= 256u) ? 16u : ((maxDim >= 128u) ? 32u : 64u);
        const uint32_t cap = std::min(m_maxArrayLayers, tierCap);
        uint64_t mipBytes = 0;
        {
            uint32_t bw = w, bh = h;
            const uint64_t bpp = (fmtCode < 2u) ? 2ull : 4ull;
            for (uint32_t lv = 0; lv < levels; ++lv)
            {
                if (bx > 0u)
                    mipBytes += static_cast<uint64_t>((bw + bx - 1u) / bx) * ((bh + by - 1u) / by) * 16ull;
                else
                    mipBytes += static_cast<uint64_t>(bw) * bh * bpp;
                bw = std::max(1u, bw >> 1);
                bh = std::max(1u, bh >> 1);
            }
        }
        const uint64_t arrBytes = mipBytes * cap;
        if (m_gpuTexArrayBytes + arrBytes > m_texArrayBudgetBytes)
        {
            if (reclaimEmptyTexArrays() && reuseIdx == SIZE_MAX)
                reuseIdx = findDeadSlot();
            if (m_gpuTexArrayBytes + arrBytes > m_texArrayBudgetBytes)
                return false;
        }

        MaliTexArray arr{};
        arr.width = w;
        arr.height = h;
        arr.levels = levels;
        arr.capacity = cap;
        arr.used = 1u;
        arr.fmtCode = fmtCode;
        arr.tex16 = (fmtCode < 2u);
        arr.gpuBytes = arrBytes;

        glGenTextures(1, &arr.tex);
        glBindTexture(GL_TEXTURE_2D_ARRAY, arr.tex);
        m_lastTexArr = arr.tex;
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, levels > 1u ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1u));
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        arr.wrapClamp = 0u;

#if defined(PS2X_MALI_GLES)
        while (glGetError() != GL_NO_ERROR) {}
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLsizei>(levels), internalFmt,
                       static_cast<GLsizei>(w), static_cast<GLsizei>(h), static_cast<GLsizei>(cap));
        if (glGetError() != GL_NO_ERROR)
        {
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            m_lastTexArr = ~0u;
            glDeleteTextures(1, &arr.tex);
            return false;
        }
#else
        while (glGetError() != GL_NO_ERROR) {}
        const GLenum extFmt = (fmtCode == 0u) ? GL_RGB : GL_RGBA;
        const GLenum extType = (fmtCode == 0u) ? GL_UNSIGNED_SHORT_5_6_5 : (fmtCode == 1u ? GL_UNSIGNED_SHORT_5_5_5_1 : GL_UNSIGNED_BYTE);
        uint32_t lw = w, lh = h;
        for (uint32_t lv = 0; lv < levels; ++lv)
        {
            glTexImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(lv), static_cast<GLint>(internalFmt),
                         static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), static_cast<GLsizei>(cap),
                         0, extFmt, extType, nullptr);
            lw = std::max(1u, lw >> 1);
            lh = std::max(1u, lh >> 1);
        }
        if (glGetError() != GL_NO_ERROR)
        {
            glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
            m_lastTexArr = ~0u;
            glDeleteTextures(1, &arr.tex);
            return false;
        }
#endif
        m_gpuTexArrayBytes += arrBytes;
        outLayer = 0u;
        if (reuseIdx != SIZE_MAX)
        {
            outArrId = static_cast<int16_t>(reuseIdx);
            m_texArrays[reuseIdx] = std::move(arr);
        }
        else
        {
            outArrId = static_cast<int16_t>(m_texArrays.size());
            m_texArrays.push_back(std::move(arr));
        }
        return true;
    }

    void ensureStandalone2D(MaliTex &tx)
    {
        if (tx.has2D || tx.rawData.empty() || !tx.width || !tx.height)
            return;
        if (!tx.tex)
            glGenTextures(1, &tx.tex);
        glBindTexture(GL_TEXTURE_2D, tx.tex);
        m_lastTex = tx.tex;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        tx.levels > 1u ? (m_trilinearEnabled ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(tx.levels - 1u));

        uint64_t newTexBytes = 0;
        if (tx.astc)
        {
            uint8_t bx = 0, by = 0;
            const GLenum fmt = fmtCodeToGlInternal(tx.fmtCode, bx, by);
            size_t off = 0;
            uint32_t lw = tx.width, lh = tx.height;
            for (uint32_t lv = 0; lv < tx.levels && lv < tx.astcLevelSizes.size(); ++lv)
            {
                const uint32_t lvBytes = tx.astcLevelSizes[lv];
                if (off + lvBytes <= tx.rawData.size())
                {
                    glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), fmt,
                                           static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                           static_cast<GLsizei>(lvBytes), tx.rawData.data() + off);
                }
                off += lvBytes;
                lw = std::max(1u, lw >> 1);
                lh = std::max(1u, lh >> 1);
            }
            newTexBytes = static_cast<uint64_t>(tx.rawData.size());
        }
        else
        {
            const uint64_t bpp = tx.tex16 ? 2ull : 4ull;
            const uint8_t *ptr = tx.rawData.data();
            uint32_t lw = tx.width, lh = tx.height;
            for (uint32_t lv = 0; lv < tx.levels; ++lv)
            {
                const size_t nBytes = static_cast<size_t>(lw) * lh * static_cast<size_t>(bpp);
                if (tx.fmtCode == 0u)
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB565,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, ptr);
                }
                else if (tx.fmtCode == 1u)
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB5_A1,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, ptr);
                }
                else
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGBA8,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, ptr);
                }
                newTexBytes += nBytes;
                ptr += nBytes;
                lw = std::max(1u, lw >> 1);
                lh = std::max(1u, lh >> 1);
            }
        }
        tx.has2D = true;
        tx.gpuBytes = newTexBytes;
        m_gpuTexBytes += newTexBytes;
        std::vector<uint8_t>().swap(tx.rawData);
        std::vector<uint32_t>().swap(tx.astcLevelSizes);
    }

    void uploadTextureOnGl(UploadCmd &cmd, uint64_t frameNum)
    {
        const uint64_t contentHash = (m_dumpTexEnabled || (m_hasAstc && !m_astcFiles.empty()))
            ? computeTexContentHash(cmd.rgba.data(), cmd.rgba.size(), cmd.width, cmd.height, cmd.levels)
            : 0ull;
        if (m_dumpTexEnabled && m_dumpedTexIds.insert(contentHash).second)
        {
            char fname[128];
            std::snprintf(fname, sizeof(fname), "%s/%016llx_%ux%u_L%u.rgba",
                          m_texDumpDir.c_str(), static_cast<unsigned long long>(contentHash),
                          cmd.width, cmd.height, cmd.levels);
            if (FILE *fp = std::fopen(fname, "wb"))
            {
                std::fwrite(cmd.rgba.data(), 1, cmd.rgba.size(), fp);
                std::fclose(fp);
            }
        }

        const uint64_t t0 = m_uploadBudgetUs ? nowUs() : 0u;
        MaliTex &tx = m_textures[cmd.id];
        tx.width = cmd.width;
        tx.height = cmd.height;
        tx.levels = cmd.levels;
        tx.lastUsed = frameNum;
        tx.wrapClamp = 0xFF;
        tx.astc = false;
        tx.tex16 = false;
        if (tx.has2D && tx.tex)
        {
            if (m_lastTex == tx.tex)
                m_lastTex = ~0u;
            glDeleteTextures(1, &tx.tex);
            tx.tex = 0;
        }
        if (m_gpuTexBytes >= tx.gpuBytes)
            m_gpuTexBytes -= tx.gpuBytes;
        else
            m_gpuTexBytes = 0;
        tx.gpuBytes = 0;
        tx.has2D = false;
        tx.rawData.clear();
        tx.astcLevelSizes.clear();

        const size_t pxCount = static_cast<size_t>(cmd.width) * cmd.height;
        uint8_t minA = 0xFF;
        uint8_t maxA = 0;
        bool allBinaryAlpha = true;
        const uint8_t *rgba = cmd.rgba.data();
        size_t aIdx = 0;
#if defined(__aarch64__)
        uint8x16_t vMinA = vdupq_n_u8(0xFFu);
        uint8x16_t vMaxA = vdupq_n_u8(0u);
        uint8x16_t vInterMask = vdupq_n_u8(0u);
        const uint8x16_t vFour = vdupq_n_u8(4u);
        const uint8x16_t v124 = vdupq_n_u8(124u);
        for (; aIdx + 16u <= pxCount; aIdx += 16u)
        {
            const uint8x16x4_t px = vld4q_u8(rgba + aIdx * 4u);
            const uint8x16_t a = px.val[3];
            vMinA = vminq_u8(vMinA, a);
            vMaxA = vmaxq_u8(vMaxA, a);
            const uint8x16_t inter = vandq_u8(vcgtq_u8(a, vFour), vcltq_u8(a, v124));
            vInterMask = vorrq_u8(vInterMask, inter);
        }
        minA = vminvq_u8(vMinA);
        maxA = vmaxvq_u8(vMaxA);
        if (vmaxvq_u8(vInterMask) != 0u)
            allBinaryAlpha = false;
#endif
        for (; aIdx < pxCount; ++aIdx)
        {
            const uint8_t a = rgba[aIdx * 4 + 3];
            if (a < minA) minA = a;
            if (a > maxA) maxA = a;
            if (a > 4u && a < 124u) allBinaryAlpha = false;
        }
        tx.minAlpha = minA;
        tx.maxAlpha = maxA;
        tx.binaryAlpha = allBinaryAlpha;

        if (m_hasAstc && !m_astcFiles.empty() && tryUploadAstcFromCache(contentHash, cmd.id, tx))
        {
            ++m_astcHits;
            if (m_uploadBudgetUs)
                m_uploadUsThisFrame += nowUs() - t0;
            return;
        }

        const bool use565 = m_tex16Enabled && minA >= 124u;
        const bool use5551 = m_tex16Enabled && m_tex5551Enabled && !use565 && allBinaryAlpha;
        tx.tex16 = use565 || use5551;
        tx.pendingEvict = false;
        releaseTexArrayLayer(tx.arrayId, tx.arrayLayer);
        tx.arrayId = -1;
        tx.arrayLayer = 0;
        const uint8_t fmtCode = use565 ? 0u : (use5551 ? 1u : 2u);
        tx.fmtCode = fmtCode;
        const bool arrEligible = m_texArrayEnabled && cmd.width >= 8u && cmd.width <= 256u &&
                                 cmd.height >= 8u && cmd.height <= 256u && tx.levels > 0u;
        if (arrEligible && !allocTexArraySlot(cmd.width, cmd.height, tx.levels, fmtCode, tx.arrayId, tx.arrayLayer))
            ++m_arrMissUploads;

        uint64_t newTexBytes = 0;
        {
            uint32_t bw = cmd.width, bh = cmd.height;
            const uint64_t bpp = tx.tex16 ? 2ull : 4ull;
            for (uint32_t lv = 0; lv < tx.levels; ++lv)
            {
                newTexBytes += static_cast<uint64_t>(bw) * bh * bpp;
                bw = std::max(1u, bw >> 1);
                bh = std::max(1u, bh >> 1);
            }
        }

        if (tx.arrayId < 0)
        {
            if (!tx.tex)
                glGenTextures(1, &tx.tex);
            glBindTexture(GL_TEXTURE_2D, tx.tex);
            m_lastTex = tx.tex;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            tx.levels > 1u ? (m_trilinearEnabled ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(tx.levels - 1u));
            tx.has2D = true;
            tx.gpuBytes = newTexBytes;
            m_gpuTexBytes += newTexBytes;
        }
        else
        {
            glBindTexture(GL_TEXTURE_2D_ARRAY, m_texArrays[tx.arrayId].tex);
            m_lastTexArr = m_texArrays[tx.arrayId].tex;
            if (use565 || use5551)
                tx.rawData.resize(static_cast<size_t>(newTexBytes));
        }

        const uint8_t *ptr = rgba;
        uint32_t lw = cmd.width, lh = cmd.height;
        size_t rawOffset = 0u;
        for (uint32_t lv = 0; lv < tx.levels; ++lv)
        {
            const size_t n = static_cast<size_t>(lw) * lh;
            if (use565)
            {
                uint16_t *dst16 = nullptr;
                if (tx.arrayId >= 0)
                {
                    dst16 = reinterpret_cast<uint16_t *>(tx.rawData.data() + rawOffset);
                    rawOffset += n * sizeof(uint16_t);
                }
                else
                {
                    m_tex16Scratch.resize(n);
                    dst16 = m_tex16Scratch.data();
                }
                size_t i = 0;
#if defined(__aarch64__)
                for (; i + 8u <= n; i += 8u)
                {
                    const uint8x8x4_t px = vld4_u8(ptr + i * 4u);
                    const uint16x8_t r = vshlq_n_u16(vmovl_u8(vshr_n_u8(px.val[0], 3)), 11);
                    const uint16x8_t g = vshll_n_u8(vshr_n_u8(px.val[1], 2), 5);
                    const uint16x8_t b = vmovl_u8(vshr_n_u8(px.val[2], 3));
                    vst1q_u16(dst16 + i, vorrq_u16(vorrq_u16(r, g), b));
                }
#endif
                for (; i < n; ++i)
                {
                    const uint16_t r = static_cast<uint16_t>(ptr[i * 4 + 0] >> 3);
                    const uint16_t g = static_cast<uint16_t>(ptr[i * 4 + 1] >> 2);
                    const uint16_t b = static_cast<uint16_t>(ptr[i * 4 + 2] >> 3);
                    dst16[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
                }
                if (tx.arrayId >= 0)
                {
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(lv), 0, 0, static_cast<GLint>(tx.arrayLayer),
                                    static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 1,
                                    GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dst16);
                }
                else
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB565,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, dst16);
                }
            }
            else if (use5551)
            {
                uint16_t *dst16 = nullptr;
                if (tx.arrayId >= 0)
                {
                    dst16 = reinterpret_cast<uint16_t *>(tx.rawData.data() + rawOffset);
                    rawOffset += n * sizeof(uint16_t);
                }
                else
                {
                    m_tex16Scratch.resize(n);
                    dst16 = m_tex16Scratch.data();
                }
                for (size_t i = 0; i < n; ++i)
                {
                    const uint16_t r = static_cast<uint16_t>(ptr[i * 4 + 0] >> 3);
                    const uint16_t g = static_cast<uint16_t>(ptr[i * 4 + 1] >> 3);
                    const uint16_t b = static_cast<uint16_t>(ptr[i * 4 + 2] >> 3);
                    const uint16_t a = ptr[i * 4 + 3] >= 64u ? 1u : 0u;
                    dst16[i] = static_cast<uint16_t>((r << 11) | (g << 6) | (b << 1) | a);
                }
                if (tx.arrayId >= 0)
                {
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(lv), 0, 0, static_cast<GLint>(tx.arrayLayer),
                                    static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 1,
                                    GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dst16);
                }
                else
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB5_A1,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, dst16);
                }
            }
            else
            {
                if (tx.arrayId >= 0)
                {
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(lv), 0, 0, static_cast<GLint>(tx.arrayLayer),
                                    static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 1,
                                    GL_RGBA, GL_UNSIGNED_BYTE, ptr);
                }
                else
                {
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGBA8,
                                 static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, ptr);
                }
            }
            ptr += n * 4u;
            lw = std::max(1u, lw >> 1);
            lh = std::max(1u, lh >> 1);
        }
        if (tx.arrayId >= 0 && !use565 && !use5551)
            tx.rawData = std::move(cmd.rgba);
        if (m_uploadBudgetUs)
            m_uploadUsThisFrame += nowUs() - t0;
    }

    // Phase 6C/7C: Build or fetch a cached merged static VBO/IBO (strictly GL_UNSIGNED_INT).
    MergeRun *getOrBuildMergeRun(size_t from, size_t to, uint64_t &mergeUsThisFrame, int16_t texArrayId)
    {
        uint64_t key = 14695981039346656037ull;
        key = fnv1aHash(&texArrayId, sizeof(texArrayId), key);
        uint32_t totalVerts = 0;
        uint32_t totalIndices = 0;
        for (size_t j = from; j < to; ++j)
        {
            const MaliMesh *m = m_qAll[j].mesh;
            key = fnv1aHash(&m_qAll[j].meshId, sizeof(uint64_t), key);
            if (texArrayId >= 0)
                key = fnv1aHash(&m_qAll[j].texArrayLayer, sizeof(uint8_t), key);
            totalVerts += static_cast<uint32_t>(m->srcVerts.size());
            totalIndices += static_cast<uint32_t>(m->srcIndices.size());
        }
        if (!totalVerts || !totalIndices)
            return nullptr;
        auto [runIt, inserted] = m_mergeRuns.try_emplace(key);
        (void)inserted;
        MergeRun &run = runIt->second;
        run.lastUsed = m_frameNum;
        run.texArrayId = texArrayId;
        if (run.vao && run.indexCount == totalIndices)
            return &run;

        ++run.frames;
        if (run.frames < 2u)
            return nullptr;
        if (m_mergeBudgetUs)
        {
            if (mergeUsThisFrame >= m_mergeBudgetUs)
                return nullptr;
            if (m_uploadBudgetUs && (m_uploadUsThisFrame + mergeUsThisFrame) >= std::max(m_uploadBudgetUs, m_mergeBudgetUs))
                return nullptr;
        }

        const uint64_t bytes = static_cast<uint64_t>(totalVerts) * sizeof(CompactVertex) +
                               static_cast<uint64_t>(totalIndices) * sizeof(uint32_t);
        if (bytes > kMaxMergeRunBytes)
            return nullptr;

        const uint64_t t0 = m_mergeBudgetUs ? nowUs() : 0u;
        if (m_mergeRunBytes + bytes > kMaxMergeRunBytes)
        {
            // Evict only runs untouched for >= 16 frames (safe on triple-buffered Mali-G31).
            m_evictRunsScratch.clear();
            for (const auto &[rk, rr] : m_mergeRuns)
            {
                if (rk != key && rr.vao && rr.lastUsed + 16u < m_frameNum)
                    m_evictRunsScratch.push_back({rr.lastUsed, rk});
            }
            std::sort(m_evictRunsScratch.begin(), m_evictRunsScratch.end());
            for (const auto &[used, rk] : m_evictRunsScratch)
            {
                if (m_mergeRunBytes + bytes <= kMaxMergeRunBytes)
                    break;
                auto it = m_mergeRuns.find(rk);
                if (it == m_mergeRuns.end())
                    continue;
                MergeRun &er = it->second;
                if (m_lastVao == er.vao)
                    m_lastVao = ~0u;
                if (er.vao) glDeleteVertexArrays(1, &er.vao);
                if (er.vbo) glDeleteBuffers(1, &er.vbo);
                if (er.ibo) glDeleteBuffers(1, &er.ibo);
                if (m_mergeRunBytes >= er.bytes)
                    m_mergeRunBytes -= er.bytes;
                else
                    m_mergeRunBytes = 0;
                m_mergeRuns.erase(it);
            }
            if (m_mergeRunBytes + bytes > kMaxMergeRunBytes)
                return nullptr;
        }

        m_mergeVertsScratch.clear();
        m_mergeIndicesScratch.clear();
        m_mergeVertsScratch.reserve(totalVerts);
        m_mergeIndicesScratch.reserve(totalIndices);
        for (size_t j = from; j < to; ++j)
        {
            const MaliMesh *m = m_qAll[j].mesh;
            const uint32_t baseV = static_cast<uint32_t>(m_mergeVertsScratch.size());
            m_mergeVertsScratch.insert(m_mergeVertsScratch.end(), m->srcVerts.begin(), m->srcVerts.end());
            if (texArrayId >= 0)
            {
                const uint8_t layer = m_qAll[j].texArrayLayer;
                for (size_t v = baseV; v < m_mergeVertsScratch.size(); ++v)
                    m_mergeVertsScratch[v].texLayer = layer;
            }
            for (uint32_t idx : m->srcIndices)
                m_mergeIndicesScratch.push_back(baseV + idx);
        }

        if (!run.vao) glGenVertexArrays(1, &run.vao);
        if (!run.vbo) glGenBuffers(1, &run.vbo);
        if (!run.ibo) glGenBuffers(1, &run.ibo);
        glBindVertexArray(run.vao);
        m_lastVao = run.vao;
        glBindBuffer(GL_ARRAY_BUFFER, run.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_mergeVertsScratch.size() * sizeof(CompactVertex)),
                     m_mergeVertsScratch.data(), GL_STATIC_DRAW);
        setupCompactVertexAttribs();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, run.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_mergeIndicesScratch.size() * sizeof(uint32_t)),
                     m_mergeIndicesScratch.data(), GL_STATIC_DRAW);

        if (m_mergeRunBytes >= run.bytes)
            m_mergeRunBytes -= run.bytes;
        run.indexCount = totalIndices;
        run.bytes = bytes;
        m_mergeRunBytes += bytes;
        if (m_mergeBudgetUs)
            mergeUsThisFrame += nowUs() - t0;
        return &run;
    }

    bool needsAlphaTestPass2(const QueuedDraw &q, float aref) const
    {
        if (m_atPass2Mode == 0 || aref <= 0.02f)
            return false;
        const uint32_t atst = (q.state.alphaTest >> 1) & 7u;
        if (atst != 5u && atst != 6u)
            return false;
        const uint32_t afail = (q.state.alphaTest >> 12) & 3u;
        if (afail != 1u && afail != 3u)
            return false;
        if (m_atPass2Mode == 2)
            return true;
        // Mode 1 (smart auto): skip Pass 2 when neither vertex nor texture has intermediate alpha in (4, 124).
        if (q.state.opacity < 0.99f)
            return true;
        if (q.mesh && q.mesh->hasIntermediateAlpha)
            return true;
        if (q.tex && !q.tex->binaryAlpha)
            return true;
        return false;
    }

    void executeFrame(FramePacket &pkt, bool glAlreadyHeld = false)
    {
        const uint64_t frameStartUs = nowUs();
        GlContextGuard guard(glAlreadyHeld);

        m_frameNum = pkt.frameNum;
        m_fog = pkt.fog;
        m_bloom = pkt.bloom;
        {
            const Tuning &liveTune = tuning();
            m_postBloomEnabled = liveTune.maliBloom;
            m_exposureEnabled = liveTune.maliExposure;
            m_dofEnabled = liveTune.maliDof;
            m_motionBlurMode = std::clamp(static_cast<int>(liveTune.maliMotionBlur + 0.5f), 0, 2);
            m_caEnabled = liveTune.maliCa;
            m_lensFlareEnabled = liveTune.maliLensFlare;
            m_aoEnabled = liveTune.maliAo;
            m_waterEnabled = liveTune.maliWater;
            m_fxaaEnabled = liveTune.maliFxaa;
            m_rcasEnabled = liveTune.maliRcas;
            m_drsEnabled = liveTune.maliDrs;
            m_atPass2Mode = !liveTune.alphaTest ? 0 : std::clamp(static_cast<int>(liveTune.maliAtPass2 + 0.5f), 0, 2);
            m_fastShadersEnabled = liveTune.maliFastShaders;
            m_cullEnabled = liveTune.maliCull;
            m_mergeEnabled = liveTune.maliMerge;
            m_instancingEnabled = liveTune.maliInstancing;
            m_tex16Enabled = liveTune.maliTex16;
            m_directScanoutEnabled = liveTune.maliDirectScanout;
            m_zPrepassEnabled = liveTune.maliZPrepass;
            m_bgRate = static_cast<uint32_t>(std::clamp(static_cast<int>(liveTune.maliBgRate + 0.5f), 1, 4));
            m_statsEnabled = liveTune.maliStats;
            m_furShells = std::clamp(static_cast<int>(liveTune.furShells + 0.5f), 0, 16);
            {
                const uint64_t wantCacheBytes = static_cast<uint64_t>(std::clamp(static_cast<int>(liveTune.maliCacheMb + 0.5f), 64, 224)) * 1024ull * 1024ull;
                if (wantCacheBytes != m_cacheBudgetBytes)
                {
                    m_cacheBudgetBytes = wantCacheBytes;
                    m_texArrayBudgetBytes = std::min(140ull * 1024ull * 1024ull, (m_cacheBudgetBytes * 5ull) / 8ull);
                }
            }
            m_screenMode = hostmenu::settings().screenMode.load(std::memory_order_relaxed);
            {
                const float userScale = std::clamp(liveTune.renderScale, 0.50f, 2.00f);
                const float userDrsMin = std::clamp(liveTune.maliDrsMin, 0.50f, userScale);
                if (std::fabs(userScale - m_drsMax) > 1e-3f || std::fabs(userDrsMin - m_drsMin) > 1e-3f)
                {
                    m_drsMax = userScale;
                    m_drsMin = userDrsMin;
                    m_drsScale = std::clamp(m_drsScale, m_drsMin, m_drsMax);
                }
            }
            const bool wantTrilinear = liveTune.maliTrilinear;
            const int wantAnisoIdx = std::clamp(static_cast<int>(liveTune.maliAniso + 0.5f), 0, 2);
            const int wantAniso = wantAnisoIdx == 2 ? 4 : (wantAnisoIdx == 1 ? 2 : 0);
            if (wantTrilinear != m_trilinearEnabled || wantAniso != m_anisoLevel)
            {
                m_trilinearEnabled = wantTrilinear;
                m_anisoLevel = wantAniso;
                updateSamplersOnGl();
            }
            if (!m_postBloomEnabled && !m_lensFlareEnabled)
                m_bloomHistValid = false;
        }
        m_culledThisFrame = 0;
        m_drawCallsThisFrame = 0;
        m_mergeSavedThisFrame = 0;
        m_b2SavedThisFrame = 0;
        m_instSavedThisFrame = 0;
        m_pass2SkipThisFrame = 0;
        m_texWrapThisFrame = 0;
        m_arrMissDrawsThisFrame = 0;
        m_skinGpuDrawsThisFrame = 0;
        m_skinCpuDrawsThisFrame = 0;
        m_skinMaxQwordsThisFrame = 0;
        m_skinInvalidThisFrame = 0;
        uint64_t mergeUsThisFrame = 0;

        processUploads(pkt);

        if (!pkt.dynVerts.empty())
        {
            glBindBuffer(GL_ARRAY_BUFFER, m_arenaVbo[m_arenaSlot]);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pkt.dynVerts.size() * sizeof(Vertex)),
                         pkt.dynVerts.data(), GL_STREAM_DRAW);
        }

        // Build draw queues, pre-resolving MaliMesh* and MaliTex* once per draw.
        // Merge-eligible static draws defer culling so MergeRun keys stay camera-invariant across pans.
        m_qBg.clear();
        m_qAll.clear();
        m_qAll.reserve(pkt.draws.size());
        uint64_t prevFurMeshId = 0ull;
        int furShellCount = 0;
        for (const RawDraw &rd : pkt.draws)
        {
            auto it = m_meshes.find(rd.meshId);
            if (it == m_meshes.end())
                continue;
            MaliMesh &mesh = it->second;
            mesh.lastUsed = m_frameNum;

            MaliTex *texPtr = nullptr;
            if (!rd.state.reflectionSample && rd.state.texture)
            {
                auto tit = m_textures.find(rd.state.texture);
                if (tit != m_textures.end() && (tit->second.tex != 0u || tit->second.arrayId >= 0))
                {
                    texPtr = &tit->second;
                    texPtr->lastUsed = m_frameNum;
                }
            }

            QueuedDraw q{};
            q.meshId = rd.meshId;
            q.mesh = &mesh;
            q.tex = texPtr;
            std::memcpy(q.clip, rd.clip, 16 * sizeof(float));
            q.clipHash = fnv1aHash(rd.clip, 16u * sizeof(float));
            q.state = rd.state;
            q.minW = 0.0f;
            q.seq = rd.seq;
            if (mesh.skinned && (rd.state.skinPalette == 0u || rd.state.skinPalette > pkt.skinPalettes.size()))
            {
                ++m_skinInvalidThisFrame;
                if (m_statsEnabled && !m_skinInvalidLogged)
                {
                    std::fprintf(stderr, "[mali:skin:invalid] frame=%llu mesh=%016llx pal=%u pals=%zu\n",
                                 static_cast<unsigned long long>(m_frameNum),
                                 static_cast<unsigned long long>(rd.meshId),
                                 rd.state.skinPalette, pkt.skinPalettes.size());
                    m_skinInvalidLogged = true;
                }
                continue;
            }
            q.skinned = mesh.skinned && rd.state.skinPalette != 0u;
            if (m_furShells > 0 && q.skinned && rd.state.alphaTest && rd.state.blend != Blend::Opaque)
            {
                if (rd.meshId == prevFurMeshId)
                {
                    ++furShellCount;
                    if (furShellCount > m_furShells)
                        continue;
                }
                else
                {
                    prevFurMeshId = rd.meshId;
                    furShellCount = 1;
                }
            }
            else
            {
                prevFurMeshId = 0ull;
                furShellCount = 0;
            }

            if (q.skinned)
                ++m_skinGpuDrawsThisFrame;
            else if (mesh.dynamic && ((rd.meshId >> 63) & 1ull))
                ++m_skinCpuDrawsThisFrame;

            if (rd.state.reflectionSample)
            {
                if (!m_waterEnabled)
                    continue;
                q.bucket = 2u;
                q.psoKey = makePsoKey(19u, Blend::Normal, true, false, 0u);
            }
            else if (rd.state.screen || rd.state.screenDepthTest)
            {
                q.bucket = 3u;
                q.psoKey = makePsoKey(2u, rd.state.blend, rd.state.screenDepthTest, false, 0u);
            }
            else
            {
                const bool ate = (rd.state.alphaTest & 1u) != 0u;
                const uint32_t atst = (rd.state.alphaTest >> 1) & 7u;
                const uint32_t afail = (rd.state.alphaTest >> 12) & 3u;
                if (ate && atst == 0u && afail == 0u)
                    continue;
                const uint32_t aref8 = (rd.state.alphaTest >> 4) & 0xFFu;
                const bool canAlphaTestBucket = ate && (atst == 5u || atst == 6u) &&
                                                !rd.state.background && !rd.state.overlay && !rd.state.noDepthWrite &&
                                                rd.state.blend != Blend::Additive && rd.state.blend != Blend::Subtract;
                const bool texOpaque = (texPtr != nullptr && texPtr->minAlpha >= 124u);
                const bool fullAlpha = !rd.state.fresnel && rd.state.opacity >= 0.99f && mesh.minAlpha >= 124u && texOpaque;
                const bool alphaTestAlwaysPasses = !ate || (atst == 1u) ||
                                                   (fullAlpha && ((atst == 5u && aref8 <= 120u) ||
                                                                  (atst == 6u && aref8 <= 116u)));
                const bool isOpaque = alphaTestAlwaysPasses && !rd.state.background && !rd.state.overlay && !rd.state.noDepthWrite &&
                                      (rd.state.blend == Blend::Opaque ||
                                       (rd.state.blend == Blend::Normal && fullAlpha));
                const bool needBgFog = m_fog.enabled && rd.state.fogged && !rd.state.background;
                const bool needVFog = m_fog.vertexFog && (!rd.state.background || rd.state.backgroundFge);
                const bool fastFog = m_fastShadersEnabled && !needBgFog && !needVFog;
                const uint32_t opProg = q.skinned ? (fastFog ? 9u : 3u) : (fastFog ? 6u : 0u);
                const uint32_t cutProg = q.skinned ? (fastFog ? 10u : 4u) : (fastFog ? 7u : 1u);
                if (isOpaque)
                {
                    q.bucket = 0u;
                    q.psoKey = makePsoKey(opProg, Blend::Opaque, true, true, 0u);
                }
                else if (canAlphaTestBucket)
                {
                    q.bucket = 1u;
                    q.psoKey = makePsoKey(cutProg, rd.state.blend, true, !rd.state.noDepthWrite, 0u);
                }
                else
                {
                    q.bucket = 2u;
                    const uint32_t df = rd.state.overlay ? 2u : 0u;
                    const bool dw = !rd.state.overlay && !rd.state.noDepthWrite &&
                                    (rd.state.blend != Blend::Additive || rd.state.writeDepth);
                    const bool needsGuard = rd.state.background && m_fog.skyNear > 0.0f;
                    const bool hasHardCut = ate && (atst == 5u || atst == 6u) && afail == 0u;
                    const bool canSoft = m_fastShadersEnabled && !dw && !needsGuard && !hasHardCut &&
                                         (rd.state.blend == Blend::Normal || rd.state.blend == Blend::Additive || rd.state.blend == Blend::Subtract);
                    const uint32_t b2Prog = canSoft ? (q.skinned ? (fastFog ? 11u : 4u) : (fastFog ? 8u : 12u)) : cutProg;
                    q.psoKey = makePsoKey(b2Prog, rd.state.blend, true, dw, df);
                    q.b2Opaque = !rd.state.overlay && dw && rd.state.blend == Blend::Opaque;
                }
            }

            if (q.bucket <= 2u && !rd.state.reflectionSample)
            {
                q.texArrayId = (m_texArrayEnabled && texPtr && texPtr->arrayId >= 0) ? texPtr->arrayId : -1;
                q.texArrayLayer = (q.texArrayId >= 0) ? texPtr->arrayLayer : 0u;
                if (m_texArrayEnabled && !mesh.dynamic && !q.skinned && texPtr && q.texArrayId < 0 &&
                    texPtr->width >= 8u && texPtr->width <= 256u && texPtr->height >= 8u && texPtr->height <= 256u && texPtr->levels > 0u)
                {
                    ++m_arrMissDrawsThisFrame;
                }
                const uint64_t mergeTexKey = (q.texArrayId >= 0)
                    ? (0x8000000000000000ull | static_cast<uint64_t>(static_cast<uint16_t>(q.texArrayId)))
                    : q.state.texture;
                uint64_t sh = fnv1aHash(&q.psoKey, sizeof(q.psoKey));
                sh = fnv1aHash(&mergeTexKey, sizeof(mergeTexKey), sh);
                sh = fnv1aHash(&q.state.clamp, sizeof(q.state.clamp), sh);
                sh = fnv1aHash(&q.state.alphaTest, sizeof(q.state.alphaTest), sh);
                const uint8_t flags = (q.state.lit ? 1u : 0u) | (q.state.fresnel ? 2u : 0u) |
                                      (q.state.fogged ? 4u : 0u) | (q.state.background ? 8u : 0u) |
                                      (q.state.backgroundFge ? 16u : 0u);
                sh = fnv1aHash(&flags, sizeof(flags), sh);
                sh = fnv1aHash(&q.state.opacity, sizeof(q.state.opacity), sh);
                sh = fnv1aHash(q.state.uvOffset, sizeof(q.state.uvOffset), sh);
                sh = fnv1aHash(&q.state.lodK, sizeof(q.state.lodK), sh);
                sh = fnv1aHash(&q.state.lodScale, sizeof(q.state.lodScale), sh);
                sh = fnv1aHash(&q.state.maxLod, sizeof(q.state.maxLod), sh);
                if (q.state.lit || q.state.fresnel)
                    sh = fnv1aHash(q.state.light, sizeof(q.state.light), sh);
                q.stateHash = sh;
            }

            const bool mergeCandidate = m_mergeEnabled && q.bucket <= 2u && !rd.state.reflectionSample && !mesh.dynamic && !q.skinned && !mesh.srcVerts.empty();
            if (m_cullEnabled && mesh.hasBox && !mesh.dynamic && !mesh.skinned && !rd.state.screen && !rd.state.overlay && !rd.state.background)
            {
                if (mergeCandidate)
                {
                    q.deferredCull = true;
                }
                else
                {
                    float minW = 0.0f;
                    if (cullAabb(rd.clip, mesh.lo, mesh.hi, minW))
                    {
                        ++m_culledThisFrame;
                        continue;
                    }
                    q.minW = minW;
                }
            }

            if (rd.state.background && m_fog.enabled)
            {
                float bgMinW = 0.0f;
                if (!q.deferredCull || !cullAabb(rd.clip, mesh.lo, mesh.hi, bgMinW))
                    m_qBg.push_back(q);
            }
            m_qAll.push_back(q);
        }

        // Sort per-layer; within Opaque and Alpha-Test buckets, group skinned draws by palette
        // and static draws deterministically by state+texture+clamp+clipHash+alphaTest+meshId.
        std::sort(m_qAll.begin(), m_qAll.end(), [](const QueuedDraw &a, const QueuedDraw &b)
        {
            if (a.state.order != b.state.order)
                return a.state.order < b.state.order;
            if (a.bucket != b.bucket)
                return a.bucket < b.bucket;
            if (a.bucket == 0u || a.bucket == 1u)
            {
                if (a.psoKey != b.psoKey) return a.psoKey < b.psoKey;
                const uint32_t aPal = a.skinned ? a.state.skinPalette : 0u;
                const uint32_t bPal = b.skinned ? b.state.skinPalette : 0u;
                if (aPal != bPal) return aPal < bPal;
                if (a.stateHash != b.stateHash) return a.stateHash < b.stateHash;
                if (a.texArrayId != b.texArrayId) return a.texArrayId < b.texArrayId;
                if (a.state.clamp != b.state.clamp) return a.state.clamp < b.state.clamp;
                if (a.state.alphaTest != b.state.alphaTest) return a.state.alphaTest < b.state.alphaTest;
                if (a.clipHash != b.clipHash) return a.clipHash < b.clipHash;
                if (a.texArrayId < 0 && a.state.texture != b.state.texture) return a.state.texture < b.state.texture;
                if (a.meshId != b.meshId) return a.meshId < b.meshId;
            }
            if (a.bucket == 2u)
            {
                if (a.b2Opaque != b.b2Opaque)
                    return a.b2Opaque > b.b2Opaque;
                if (a.b2Opaque)
                {
                    if (a.psoKey != b.psoKey) return a.psoKey < b.psoKey;
                    if (a.stateHash != b.stateHash) return a.stateHash < b.stateHash;
                    if (a.texArrayId != b.texArrayId) return a.texArrayId < b.texArrayId;
                    if (a.state.clamp != b.state.clamp) return a.state.clamp < b.state.clamp;
                    if (a.state.alphaTest != b.state.alphaTest) return a.state.alphaTest < b.state.alphaTest;
                    if (a.clipHash != b.clipHash) return a.clipHash < b.clipHash;
                    if (a.texArrayId < 0 && a.state.texture != b.state.texture) return a.state.texture < b.state.texture;
                    if (a.meshId != b.meshId) return a.meshId < b.meshId;
                }
            }
            return a.seq < b.seq;
        });

        const float scale = m_drsEnabled ? m_drsScale : m_drsMax;
        const uint32_t rw = std::max(320u, static_cast<uint32_t>(std::lround(pkt.width * scale)) & ~3u);
        const uint32_t rh = std::max(240u, static_cast<uint32_t>(std::lround(pkt.height * scale)) & ~3u);

        bool has3D = false;
        for (const QueuedDraw &q : m_qAll)
        {
            if (q.bucket <= 2u)
            {
                has3D = true;
                break;
            }
        }
        const bool wantTeamIcoBloom = has3D && m_postEnabled && m_postBloomEnabled && m_bloom.enabled;
        const bool wantExposure = wantTeamIcoBloom && m_exposureEnabled;
        const bool wantDof = has3D && m_postEnabled && m_dofEnabled && pkt.dof.enabled && pkt.dof.slices > 0;
        const bool wantScreenBlur = has3D && m_postEnabled && m_motionBlurMode >= 1 && !pkt.screenBlurs.empty();
        const bool wantPrevBlur = has3D && m_postEnabled && m_motionBlurMode >= 2 && m_prevMainValid;
        const bool wantCa = has3D && m_postEnabled && m_caEnabled;
        const bool wantLensFlare = has3D && m_postEnabled && m_lensFlareEnabled;
        const bool wantAo = has3D && m_postEnabled && m_aoEnabled;
        const bool wantFxaa = has3D && m_postEnabled && m_fxaaEnabled;
        const bool wantRcas = has3D && m_postEnabled && m_rcasEnabled;
        const bool needBloomChain = wantTeamIcoBloom || wantLensFlare;
        const bool needDepthTex = needBloomChain || wantDof || wantAo;
        const bool wantAnyFx = wantTeamIcoBloom || wantDof || wantScreenBlur || wantPrevBlur || wantCa || wantLensFlare || wantAo || wantFxaa || wantRcas;
        const bool runPost = wantAnyFx || (m_postEnabled && m_postUpscaleEnabled && (rw != pkt.width || rh != pkt.height));

        uint32_t drawW = 0u, drawH = 0u;
#if defined(PS2X_LINUX_SDL2_SHIM)
        ps2xLinuxGetDrawableSize(drawW, drawH);
#endif
        const bool fullViewportMatch = (m_screenMode == 1) || (m_screenMode == 0 && drawW * 3u == drawH * 4u);
        const bool directScanout =
#if defined(PS2X_LINUX_SDL2_SHIM)
            m_directScanoutEnabled && !m_hasLetterboxCrop && !runPost && !pkt.hold &&
            std::fabs(scale - 1.0f) <= 0.01f && fullViewportMatch &&
            static_cast<bool>(presentHook()) && drawW > 0u && drawH > 0u;
#else
            false;
#endif
        const bool directPostToFbo0 =
#if defined(PS2X_LINUX_SDL2_SHIM)
            runPost && m_directScanoutEnabled && !m_hasLetterboxCrop && !pkt.hold &&
            fullViewportMatch && static_cast<bool>(presentHook()) && drawW > 0u && drawH > 0u;
#else
            false;
#endif

        MaliTarget &mt = m_mainTarget[m_mainIdx];
        if (!directScanout)
        {
            if (mt.width != rw || mt.height != rh || mt.depthAsTex != needDepthTex)
            {
                makeTarget(mt, rw, rh, true, needDepthTex);
                m_prevMainValid = false;
            }
            if (runPost && !directPostToFbo0 && (m_postTarget.width != pkt.width || m_postTarget.height != pkt.height))
                makeTarget(m_postTarget, pkt.width, pkt.height, false);
            if (needBloomChain)
            {
                if (!m_bloomQuarter.fbo) makeTarget(m_bloomQuarter, 128u, 96u, false);
                if (!m_bloomEighth.fbo) makeTarget(m_bloomEighth, 64u, 48u, false);
                if (!m_bloomHist[0].fbo) makeTarget(m_bloomHist[0], 64u, 48u, false);
                if (!m_bloomHist[1].fbo) makeTarget(m_bloomHist[1], 64u, 48u, false);
            }
            if (wantDof && !m_dofHalf.fbo)
                makeTarget(m_dofHalf, 160u, 120u, false);
            if (wantAo)
            {
                if (!m_aoTarget.fbo) makeTarget(m_aoTarget, 160u, 120u, false);
                if (!m_aoBlur.fbo) makeTarget(m_aoBlur, 160u, 120u, false);
            }
        }

        resetPsoAndUniformCaches();

        // Phase 6A: Skip m_qBg -> m_bgTarget completely when !m_fog.enabled (ZFOG=0)!
        if (m_fog.enabled && !m_qBg.empty() && m_bgTarget.fbo && (m_frameNum % m_bgRate == 0u || m_frameNum <= 2u))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_bgTarget.fbo);
            glViewport(0, 0, static_cast<GLsizei>(m_bgTarget.width), static_cast<GLsizei>(m_bgTarget.height));
            glDepthMask(GL_TRUE);
            glClearColor(m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            for (const QueuedDraw &q : m_qBg)
                emitDrawSingle(q, pkt, false, 0.02f, 0.0f);
            invalidateDepth();
        }

        if (directScanout)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, static_cast<GLsizei>(drawW), static_cast<GLsizei>(drawH));
        }
        else
        {
            glBindFramebuffer(GL_FRAMEBUFFER, mt.fbo);
            glViewport(0, 0, static_cast<GLsizei>(mt.width), static_cast<GLsizei>(mt.height));
        }
        glDepthMask(GL_TRUE);
        glClearColor(m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        if (m_zPrepassEnabled)
        {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            const uint32_t zPso = makePsoKey(5u, Blend::Opaque, true, true, 0u);
            for (const QueuedDraw &q : m_qAll)
            {
                if (q.bucket != 0u || q.skinned || q.state.background || !q.mesh || q.mesh->dynamic)
                    continue;
                float dummyW = 0.0f;
                if (q.deferredCull && m_cullEnabled && cullAabb(q.clip, q.mesh->lo, q.mesh->hi, dummyW))
                    continue;
                const uint32_t tris = q.mesh->indexed ? q.mesh->indexCount : q.mesh->vertexCount;
                if (tris < 300u)
                    continue;
                QueuedDraw zq = q;
                zq.psoKey = zPso;
                emitDrawSingle(zq, pkt, false, 0.0f, 0.0f);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }

        auto b2MinAlpha = [](const QueuedDraw &dq) noexcept -> float {
            float minA = 0.02f;
            const uint32_t at = dq.state.alphaTest;
            const uint32_t atst = (at >> 1) & 7u;
            const uint32_t afail = (at >> 12) & 3u;
            if ((at & 1u) && (atst == 5u || atst == 6u) && afail == 0u)
                minA = std::max(0.02f, static_cast<float>((at >> 4) & 0xFFu) / 128.0f);
            return minA;
        };

        std::vector<size_t> deferredHud;
        for (size_t i = 0; i < m_qAll.size();)
        {
            const QueuedDraw &q = m_qAll[i];
            if (runPost && q.bucket == 3u && q.state.order >= 20u && q.state.screen && !q.state.screenDepthTest)
            {
                deferredHud.push_back(i);
                ++i;
                continue;
            }

            // Phase 6.1/7C/8: Camera-invariant static merge across GL_TEXTURE_2D_ARRAY layers + leftover singleton instancing.
            // Bucket 2 stays strictly in original seq order (painter's order); only consecutive compatible draws merge/instance.
            if (m_mergeEnabled && q.bucket <= 2u && q.mesh && !q.mesh->dynamic && !q.skinned && !q.mesh->srcVerts.empty() &&
                i + 1u < m_qAll.size() && canMergeStatic(q, m_qAll[i + 1u]))
            {
                size_t groupEnd = i + 2u;
                while (groupEnd < m_qAll.size() && canMergeStatic(q, m_qAll[groupEnd]))
                    ++groupEnd;

                m_instScratch.clear();
                for (size_t s = i; s < groupEnd;)
                {
                    const size_t e = std::min(s + static_cast<size_t>(m_mergeChunkMax), groupEnd);
                    const QueuedDraw &sq = m_qAll[s];
                    if (e - s >= 2u)
                    {
                        float runLo[3] = {1e30f, 1e30f, 1e30f};
                        float runHi[3] = {-1e30f, -1e30f, -1e30f};
                        bool allBox = true;
                        for (size_t j = s; j < e; ++j)
                        {
                            const MaliMesh *jm = m_qAll[j].mesh;
                            if (!jm || !jm->hasBox)
                            {
                                allBox = false;
                                break;
                            }
                            for (int a = 0; a < 3; ++a)
                            {
                                if (jm->lo[a] < runLo[a]) runLo[a] = jm->lo[a];
                                if (jm->hi[a] > runHi[a]) runHi[a] = jm->hi[a];
                            }
                        }
                        float dummyW = 0.0f;
                        if (m_cullEnabled && allBox && !sq.state.background && !sq.state.overlay && cullAabb(sq.clip, runLo, runHi, dummyW))
                        {
                            m_culledThisFrame += static_cast<uint32_t>(e - s);
                            s = e;
                            continue;
                        }
                        if (MergeRun *run = getOrBuildMergeRun(s, e, mergeUsThisFrame, sq.texArrayId))
                        {
                            if (sq.bucket == 0u)
                            {
                                emitMergedRun(sq, *run, pkt, true, 0.0f, 0.0f);
                            }
                            else if (sq.bucket == 2u)
                            {
                                emitMergedRun(sq, *run, pkt, true, b2MinAlpha(sq), 0.0f);
                            }
                            else
                            {
                                const float aref = static_cast<float>((sq.state.alphaTest >> 4) & 0xFFu) / 128.0f;
                                emitMergedRun(sq, *run, pkt, true, std::max(0.02f, aref), 0.0f);
                                bool anyNeedPass2 = false;
                                for (size_t j = s; j < e; ++j)
                                    if (needsAlphaTestPass2(m_qAll[j], aref)) { anyNeedPass2 = true; break; }
                                if (anyNeedPass2)
                                {
                                    QueuedDraw p2 = sq;
                                    const bool fastFog = ((sq.psoKey & 31u) == 7u || (sq.psoKey & 31u) == 10u);
                                    const uint32_t p2ProgBase = m_fastShadersEnabled ? (fastFog ? 8u : 12u) : 1u; const uint32_t p2Prog = (sq.texArrayId >= 0) ? toArrayProg(p2ProgBase) : p2ProgBase;
                                    p2.psoKey = makePsoKey(p2Prog, sq.state.blend, true, false, 1u);
                                    emitMergedRun(p2, *run, pkt, true, 0.02f, aref);
                                }
                                else if (((sq.state.alphaTest >> 12) & 3u) == 1u || ((sq.state.alphaTest >> 12) & 3u) == 3u)
                                {
                                    m_pass2SkipThisFrame += static_cast<uint32_t>(e - s);
                                }
                            }
                            const uint32_t saved = static_cast<uint32_t>(e - s - 1u);
                            m_mergeSavedThisFrame += saved;
                            if (sq.bucket == 2u)
                                m_b2SavedThisFrame += saved;
                            s = e;
                            continue;
                        }
                    }

                    for (size_t j = s; j < e; ++j)
                    {
                        const QueuedDraw &jq = m_qAll[j];
                        float dummyW = 0.0f;
                        if (m_cullEnabled && jq.deferredCull && jq.mesh && cullAabb(jq.clip, jq.mesh->lo, jq.mesh->hi, dummyW))
                        {
                            ++m_culledThisFrame;
                            continue;
                        }
                        m_instScratch.push_back(&jq);
                    }
                    s = e;
                }

                if (m_instancingEnabled && m_instScratch.size() >= 2u)
                {
                    if (q.bucket != 2u)
                    {
                        std::sort(m_instScratch.begin(), m_instScratch.end(), [](const QueuedDraw *a, const QueuedDraw *b) {
                            if (a->meshId != b->meshId) return a->meshId < b->meshId;
                            if (a->state.texture != b->state.texture) return a->state.texture < b->state.texture;
                            return a->seq < b->seq;
                        });
                    }
                    for (size_t k = 0; k < m_instScratch.size();)
                    {
                        const QueuedDraw *iq0 = m_instScratch[k];
                        size_t kEnd = k + 1u;
                        while (kEnd < m_instScratch.size() && (kEnd - k) < kMaxInstances && canInstanceStatic(*iq0, *m_instScratch[kEnd]))
                            ++kEnd;
                        const uint32_t cnt = static_cast<uint32_t>(kEnd - k);
                        if (cnt >= 2u)
                        {
                            if (iq0->bucket == 0u)
                            {
                                emitInstanced(&m_instScratch[k], cnt, pkt, true, 0.0f, 0.0f, iq0->psoKey);
                            }
                            else if (iq0->bucket == 2u)
                            {
                                emitInstanced(&m_instScratch[k], cnt, pkt, true, b2MinAlpha(*iq0), 0.0f, iq0->psoKey);
                            }
                            else
                            {
                                const float aref = static_cast<float>((iq0->state.alphaTest >> 4) & 0xFFu) / 128.0f;
                                emitInstanced(&m_instScratch[k], cnt, pkt, true, std::max(0.02f, aref), 0.0f, iq0->psoKey);
                                bool anyPass2 = false;
                                for (uint32_t u = 0; u < cnt; ++u)
                                    if (needsAlphaTestPass2(*m_instScratch[k + u], aref)) { anyPass2 = true; break; }
                                if (anyPass2)
                                {
                                    const bool fastFog = ((iq0->psoKey & 31u) == 7u || (iq0->psoKey & 31u) == 10u);
                                    const uint32_t p2Prog = m_fastShadersEnabled ? (fastFog ? 8u : 12u) : 1u;
                                    const uint32_t p2Pso = makePsoKey(p2Prog, iq0->state.blend, true, false, 1u);
                                    emitInstanced(&m_instScratch[k], cnt, pkt, true, 0.02f, aref, p2Pso);
                                }
                                else if (((iq0->state.alphaTest >> 12) & 3u) == 1u || ((iq0->state.alphaTest >> 12) & 3u) == 3u)
                                {
                                    m_pass2SkipThisFrame += cnt;
                                }
                            }
                            m_instSavedThisFrame += cnt - 1u;
                            if (iq0->bucket == 2u)
                                m_b2SavedThisFrame += cnt - 1u;
                        }
                        else
                        {
                            emitBucketSingle(*iq0, pkt);
                        }
                        k = kEnd;
                    }
                }
                else
                {
                    for (const QueuedDraw *jq : m_instScratch)
                        emitBucketSingle(*jq, pkt);
                }

                i = groupEnd;
                continue;
            }

            if (m_instancingEnabled && q.bucket <= 2u && q.mesh && !q.mesh->dynamic && !q.skinned)
            {
                size_t instEnd = i + 1u;
                while (instEnd < m_qAll.size() && (instEnd - i) < kMaxInstances && canInstanceStatic(q, m_qAll[instEnd]))
                    ++instEnd;
                if (instEnd - i >= 2u)
                {
                    const QueuedDraw *inst[kMaxInstances];
                    uint32_t count = 0u;
                    for (size_t j = i; j < instEnd; ++j)
                    {
                        const QueuedDraw &jq = m_qAll[j];
                        float dummyW = 0.0f;
                        if (m_cullEnabled && jq.deferredCull && jq.mesh && cullAabb(jq.clip, jq.mesh->lo, jq.mesh->hi, dummyW))
                        {
                            ++m_culledThisFrame;
                            continue;
                        }
                        inst[count++] = &jq;
                    }
                    if (count >= 2u)
                    {
                        const QueuedDraw &iq = *inst[0];
                        if (iq.bucket == 0u)
                        {
                            emitInstanced(inst, count, pkt, true, 0.0f, 0.0f, iq.psoKey);
                        }
                        else if (iq.bucket == 2u)
                        {
                            emitInstanced(inst, count, pkt, true, b2MinAlpha(iq), 0.0f, iq.psoKey);
                        }
                        else
                        {
                            const float aref = static_cast<float>((iq.state.alphaTest >> 4) & 0xFFu) / 128.0f;
                            emitInstanced(inst, count, pkt, true, std::max(0.02f, aref), 0.0f, iq.psoKey);
                            bool anyPass2 = false;
                            for (uint32_t k = 0; k < count; ++k)
                                if (needsAlphaTestPass2(*inst[k], aref)) { anyPass2 = true; break; }
                            if (anyPass2)
                            {
                                const bool fastFog = ((iq.psoKey & 31u) == 7u || (iq.psoKey & 31u) == 10u);
                                const uint32_t p2Prog = m_fastShadersEnabled ? (fastFog ? 8u : 12u) : 1u;
                                const uint32_t p2Pso = makePsoKey(p2Prog, iq.state.blend, true, false, 1u);
                                emitInstanced(inst, count, pkt, true, 0.02f, aref, p2Pso);
                            }
                            else if (((iq.state.alphaTest >> 12) & 3u) == 1u || ((iq.state.alphaTest >> 12) & 3u) == 3u)
                            {
                                m_pass2SkipThisFrame += count;
                            }
                        }
                        m_instSavedThisFrame += count - 1u;
                        if (iq.bucket == 2u)
                            m_b2SavedThisFrame += count - 1u;
                    }
                    else if (count == 1u)
                    {
                        emitBucketSingle(*inst[0], pkt);
                    }
                    i = instEnd;
                    continue;
                }
            }

            if (m_cullEnabled && q.deferredCull && q.mesh)
            {
                float dummyW = 0.0f;
                if (cullAabb(q.clip, q.mesh->lo, q.mesh->hi, dummyW))
                {
                    ++m_culledThisFrame;
                    ++i;
                    continue;
                }
            }
            emitBucketSingle(q, pkt);
            ++i;
        }

        if (!needDepthTex)
            invalidateDepth(directScanout);

        GLuint outTex = 0u;
        uint32_t outW = 0u;
        uint32_t outH = 0u;
        m_lastPresentedPost = false;
        double postMs = 0.0;
        if (directScanout)
        {
            outTex = 0u;
            outW = drawW;
            outH = drawH;
            m_lastPresentedDirect = true;
            m_lastDirectW = drawW;
            m_lastDirectH = drawH;
            m_prevMainValid = false;
        }
        else
        {
            outTex = mt.color;
            outW = mt.width;
            outH = mt.height;
            if (runPost)
            {
                const uint64_t postStartUs = nowUs();
                if (m_lastSamplerIdx != 0u)
                {
                    glBindSampler(0, 0);
                    m_lastSamplerIdx = 0u;
                }
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glDisable(GL_BLEND);
                glBindVertexArray(m_emptyVao);

                if (needBloomChain)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomQuarter.fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_bloomQuarter.width), static_cast<GLsizei>(m_bloomQuarter.height));
                    glUseProgram(m_progBloomMask.id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, mt.color);
                    glActiveTexture(GL_TEXTURE5);
                    glBindTexture(GL_TEXTURE_2D, mt.depth);
                    if (m_progBloomMask.uMisc >= 0)
                        glUniform4f(m_progBloomMask.uMisc, 1.0f / static_cast<float>(mt.width), 1.0f / static_cast<float>(mt.height), 0.0f, 0.0f);
                    if (m_progBloomMask.uPostA >= 0)
                        glUniform4f(m_progBloomMask.uPostA, m_bloom.threshold, m_bloom.zA, m_bloom.zB, wantTeamIcoBloom ? 1.0f : 0.0f);
                    if (m_progBloomMask.uPostB >= 0)
                        glUniform4f(m_progBloomMask.uPostB, m_bloom.maskColor[0], m_bloom.maskColor[1], m_bloom.maskColor[2], wantLensFlare ? 0.55f : 0.25f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);

                    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomEighth.fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_bloomEighth.width), static_cast<GLsizei>(m_bloomEighth.height));
                    glUseProgram(m_progKawase.id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_bloomQuarter.color);
                    if (m_progKawase.uMisc >= 0)
                        glUniform4f(m_progKawase.uMisc, 1.5f / static_cast<float>(m_bloomQuarter.width), 1.5f / static_cast<float>(m_bloomQuarter.height), 0.0f, 0.0f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);

                    const int curHist = m_bloomHistIdx;
                    const int prevHist = curHist ^ 1;
                    glBindFramebuffer(GL_FRAMEBUFFER, m_bloomHist[curHist].fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_bloomHist[curHist].width), static_cast<GLsizei>(m_bloomHist[curHist].height));
                    glUseProgram(m_progTemporal.id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_bloomEighth.color);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, m_bloomHist[prevHist].color);
                    const float blendFactor = m_bloomHistValid ? std::clamp(1.0f - m_bloom.temporal, 0.08f, 0.92f) : 1.0f;
                    if (m_progTemporal.uMisc >= 0)
                        glUniform4f(m_progTemporal.uMisc, blendFactor, 0.0f, 0.0f, 0.0f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                    m_bloomHistValid = true;
                }

                if (wantDof)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, m_dofHalf.fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_dofHalf.width), static_cast<GLsizei>(m_dofHalf.height));
                    glUseProgram(m_progKawase.id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, mt.color);
                    if (m_progKawase.uMisc >= 0)
                        glUniform4f(m_progKawase.uMisc, 2.0f / static_cast<float>(mt.width), 2.0f / static_cast<float>(mt.height), 0.0f, 0.0f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }

                if (wantAo)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, m_aoTarget.fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_aoTarget.width), static_cast<GLsizei>(m_aoTarget.height));
                    glUseProgram(m_progAo.id);
                    glActiveTexture(GL_TEXTURE5);
                    glBindTexture(GL_TEXTURE_2D, mt.depth);
                    if (m_progAo.uMisc >= 0)
                        glUniform4f(m_progAo.uMisc, 1.0f / static_cast<float>(mt.width), 1.0f / static_cast<float>(mt.height), 4.0f, 0.42f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);

                    glBindFramebuffer(GL_FRAMEBUFFER, m_aoBlur.fbo);
                    glViewport(0, 0, static_cast<GLsizei>(m_aoBlur.width), static_cast<GLsizei>(m_aoBlur.height));
                    glUseProgram(m_progAoBlur.id);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_aoTarget.color);
                    if (m_progAoBlur.uMisc >= 0)
                        glUniform4f(m_progAoBlur.uMisc, 1.0f / static_cast<float>(m_aoTarget.width), 1.0f / static_cast<float>(m_aoTarget.height), 0.0f, 0.0f);
                    glDrawArrays(GL_TRIANGLES, 0, 3);
                }

                const GLuint dstFbo = directPostToFbo0 ? 0u : m_postTarget.fbo;
                const uint32_t dstW = directPostToFbo0 ? drawW : m_postTarget.width;
                const uint32_t dstH = directPostToFbo0 ? drawH : m_postTarget.height;
                glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
                glViewport(0, 0, static_cast<GLsizei>(dstW), static_cast<GLsizei>(dstH));
                glUseProgram(m_progPost.id);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, mt.color);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, needBloomChain ? m_bloomHist[m_bloomHistIdx].color : m_whiteTex);
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, wantDof ? m_dofHalf.color : m_whiteTex);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, wantAo ? m_aoBlur.color : m_whiteTex);
                glActiveTexture(GL_TEXTURE4);
                glBindTexture(GL_TEXTURE_2D, wantPrevBlur ? m_mainTarget[m_mainIdx ^ 1].color : mt.color);
                glActiveTexture(GL_TEXTURE5);
                glBindTexture(GL_TEXTURE_2D, mt.depthAsTex ? mt.depth : m_whiteTex);
                glActiveTexture(GL_TEXTURE0);

                if (m_progPost.uMisc >= 0)
                    glUniform4f(m_progPost.uMisc, 1.0f / static_cast<float>(mt.width), 1.0f / static_cast<float>(mt.height),
                                wantCa ? 0.008f : 0.0f, wantLensFlare ? 0.22f : 0.0f);
                if (m_progPost.uPostA >= 0)
                    glUniform4f(m_progPost.uPostA, m_bloom.color[0], m_bloom.color[1], m_bloom.color[2], wantTeamIcoBloom ? m_bloom.color[3] : 0.0f);
                if (m_progPost.uPostB >= 0)
                    glUniform4f(m_progPost.uPostB, wantExposure ? 0.35f : 0.0f, wantAo ? 1.0f : 0.0f,
                                wantPrevBlur ? 0.22f : 0.0f, wantDof ? 1.0f : 0.0f);
                const float fxaaRcasMode = (wantFxaa ? 1.0f : 0.0f) + (wantRcas ? 2.0f : 0.0f);
                if (m_progPost.uPostC >= 0)
                    glUniform4f(m_progPost.uPostC, static_cast<float>(pkt.dof.focusZ), pkt.dof.zA, pkt.dof.zB, fxaaRcasMode);
                float sbScale = 0.0f, sbAngle = 0.0f, sbDx = 0.0f, sbDy = 0.0f;
                if (wantScreenBlur)
                {
                    const ScreenBlur &sb = pkt.screenBlurs.back();
                    sbScale = sb.scale - 1.0f;
                    sbAngle = sb.angle;
                    sbDx = sb.dx / static_cast<float>(std::max(1u, pkt.width));
                    sbDy = sb.dy / static_cast<float>(std::max(1u, pkt.height));
                }
                if (m_progPost.uPostD >= 0)
                    glUniform4f(m_progPost.uPostD, sbScale, sbAngle, sbDx, sbDy);
                glDrawArrays(GL_TRIANGLES, 0, 3);

                if (needDepthTex)
                {
                    glBindFramebuffer(GL_FRAMEBUFFER, mt.fbo);
                    invalidateDepth(false);
                    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
                }

                if (!deferredHud.empty())
                {
                    resetPsoAndUniformCaches();
                    for (size_t hi : deferredHud)
                        emitBucketSingle(m_qAll[hi], pkt);
                }

                if (needBloomChain)
                    m_bloomHistIdx ^= 1;

                postMs = static_cast<double>(nowUs() - postStartUs) * 0.001;
                if (directPostToFbo0)
                {
                    outTex = 0u;
                    outW = drawW;
                    outH = drawH;
                    m_lastPresentedDirect = true;
                    m_lastDirectW = drawW;
                    m_lastDirectH = drawH;
                }
                else
                {
                    outTex = m_postTarget.color;
                    outW = m_postTarget.width;
                    outH = m_postTarget.height;
                    m_lastPresentedDirect = false;
                    m_lastPresentedPost = true;
                }
            }
            else
            {
                m_lastPresentedDirect = false;
            }
            if (!directPostToFbo0)
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
            m_lastPresentedMain = m_mainIdx;
            m_prevMainValid = true;
            m_mainIdx ^= 1;
        }
        m_emaPostMs = m_emaPostMs > 0.0 ? (m_emaPostMs * 0.85 + postMs * 0.15) : postMs;
        m_arenaSlot = (m_arenaSlot + 1) % 3;

        const double drawMs = static_cast<double>(nowUs() - frameStartUs) * 0.001;
        m_emaDrawMs = m_emaDrawMs > 0.0 ? (m_emaDrawMs * 0.85 + drawMs * 0.15) : drawMs;
        double pacerSleepMs = 0.0;
        if (m_paceEnabled && !pkt.hold && m_targetFps > 0)
        {
            const auto period = std::chrono::microseconds(1000000 / m_targetFps);
            const auto target = m_lastPresentTime + period;
            const auto now = std::chrono::steady_clock::now();
            if (target > now + std::chrono::microseconds(600))
            {
                std::this_thread::sleep_until(target - std::chrono::microseconds(400));
                pacerSleepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
            }
        }

        double lastCapMs = 0.0;
        if (!pkt.hold)
        {
            if (const auto &hook = presentHook())
                hook(outTex, outW, outH);
            m_lastPresentTime = std::chrono::steady_clock::now();
#if defined(PS2X_LINUX_SDL2_SHIM)
            const double swapMs = ps2xLinuxLastSwapMs();
            lastCapMs = ps2xLinuxLastCapSleepMs();
            m_emaSwapMs = m_emaSwapMs > 0.0 ? (m_emaSwapMs * 0.85 + swapMs * 0.15) : swapMs;
            m_emaCapMs = m_emaCapMs > 0.0 ? (m_emaCapMs * 0.85 + lastCapMs * 0.15) : lastCapMs;
#endif
        }

        const double frameMs = static_cast<double>(nowUs() - frameStartUs) * 0.001;
        const double gpuFrameMs = std::max(0.0, frameMs - lastCapMs - pacerSleepMs);
        m_emaFrameMs = m_emaFrameMs > 0.0 ? (m_emaFrameMs * 0.85 + frameMs * 0.15) : frameMs;
        m_emaGpuFrameMs = m_emaGpuFrameMs > 0.0 ? (m_emaGpuFrameMs * 0.85 + gpuFrameMs * 0.15) : gpuFrameMs;
        if (!pkt.hold && m_drsEnabled && (m_frameNum % 12u == 0u))
        {
            const double metricMs = m_drsDrawOnly ? m_emaDrawMs : m_emaGpuFrameMs;
            const double budgetMs = (1000.0 / static_cast<double>(m_drsTargetFps)) * (m_drsDrawOnly ? 0.85 : 1.0);
            if (metricMs > budgetMs * 1.12)
            {
                ++m_drsOverStreak;
                m_drsUnderStreak = 0;
                if (m_drsOverStreak >= 2 && m_drsScale > m_drsMin)
                {
                    m_drsScale = std::max(m_drsMin, m_drsScale - 0.05f);
                    m_drsOverStreak = 0;
                }
            }
            else if (metricMs <= budgetMs * 1.04 && m_emaDrawMs < budgetMs * 0.70)
            {
                ++m_drsUnderStreak;
                m_drsOverStreak = 0;
                if (m_drsUnderStreak >= 6 && m_drsScale < m_drsMax)
                {
                    m_drsScale = std::min(m_drsMax, m_drsScale + 0.05f);
                    m_drsUnderStreak = 0;
                }
            }
            else
            {
                m_drsOverStreak = 0;
                m_drsUnderStreak = 0;
            }
        }

        if (m_statsEnabled && (m_frameNum % 120u == 0u))
        {
            const double gpuMb = static_cast<double>(m_gpuMeshBytes + m_gpuTexBytes + m_gpuTexArrayBytes) / (1024.0 * 1024.0);
            const double budMb = static_cast<double>(m_cacheBudgetBytes) / (1024.0 * 1024.0);
            uint32_t liveArrays = 0;
            for (const MaliTexArray &arr : m_texArrays)
            {
                if (arr.tex != 0u)
                    ++liveArrays;
            }
            uint32_t bCnt[4] = {0, 0, 0, 0};
            uint32_t brkClip = 0, brkArr = 0, brkState = 0, brkDyn = 0, brkSkin = 0, brkNoSrc = 0;
            uint32_t b2BrkClip = 0, b2BrkArr = 0, b2BrkState = 0, b2BrkDyn = 0, b2BrkSkin = 0, b2BrkNoSrc = 0;
            const QueuedDraw *prevStatic = nullptr;
            const QueuedDraw *prevStaticB2 = nullptr;
            for (const QueuedDraw &q : m_qAll)
            {
                if (q.bucket < 4u)
                    ++bCnt[q.bucket];
                if (q.bucket == 2u)
                {
                    if (q.skinned)
                    {
                        ++b2BrkSkin;
                        prevStaticB2 = nullptr;
                    }
                    else if (!q.mesh || q.mesh->dynamic)
                    {
                        ++b2BrkDyn;
                        prevStaticB2 = nullptr;
                    }
                    else if (q.mesh->srcVerts.empty())
                    {
                        ++b2BrkNoSrc;
                        prevStaticB2 = nullptr;
                    }
                    else
                    {
                        if (prevStaticB2 && !canMergeStatic(*prevStaticB2, q))
                        {
                            if (prevStaticB2->state.order != q.state.order || prevStaticB2->psoKey != q.psoKey)
                                ++b2BrkState;
                            else if (prevStaticB2->texArrayId != q.texArrayId || (prevStaticB2->texArrayId < 0 && prevStaticB2->state.texture != q.state.texture))
                                ++b2BrkArr;
                            else if (prevStaticB2->state.clamp != q.state.clamp || prevStaticB2->state.alphaTest != q.state.alphaTest || prevStaticB2->stateHash != q.stateHash)
                                ++b2BrkState;
                            else
                                ++b2BrkClip;
                        }
                        prevStaticB2 = &q;
                    }
                    continue;
                }
                if (q.bucket != 0u && q.bucket != 1u)
                    continue;
                if (q.skinned)
                {
                    ++brkSkin;
                    prevStatic = nullptr;
                    continue;
                }
                if (!q.mesh || q.mesh->dynamic)
                {
                    ++brkDyn;
                    prevStatic = nullptr;
                    continue;
                }
                if (q.mesh->srcVerts.empty())
                {
                    ++brkNoSrc;
                    prevStatic = nullptr;
                    continue;
                }
                if (prevStatic && !canMergeStatic(*prevStatic, q))
                {
                    if (prevStatic->state.order != q.state.order || prevStatic->bucket != q.bucket || prevStatic->psoKey != q.psoKey)
                        ++brkState;
                    else if (prevStatic->texArrayId != q.texArrayId || (prevStatic->texArrayId < 0 && prevStatic->state.texture != q.state.texture))
                        ++brkArr;
                    else if (prevStatic->state.clamp != q.state.clamp || prevStatic->state.alphaTest != q.state.alphaTest || prevStatic->stateHash != q.stateHash)
                        ++brkState;
                    else
                        ++brkClip;
                }
                prevStatic = &q;
            }
            std::fprintf(stderr, "[mali:stats] frame=%llu draws=%u mergeSaved=%u instSaved=%u pass2Skip=%u culled=%u direct=%d drop=%llu drs=%.2f drawMs=%.2f postMs=%.2f swapMs=%.2f capMs=%.2f renderMs=%.2f mem=%.1f/%.1fM arr=%u arrMiss=%u/%llu astc=%u b=%u/%u/%u/%u b2s=%u brk=%u/%u/%u/%u/%u/%u b2brk=%u/%u/%u/%u/%u/%u skin=%u/%u/%u upQ=%zu\n",
                         static_cast<unsigned long long>(m_frameNum), m_drawCallsThisFrame,
                         m_mergeSavedThisFrame, m_instSavedThisFrame, m_pass2SkipThisFrame, m_culledThisFrame,
                         m_lastPresentedDirect ? 1 : 0,
                         static_cast<unsigned long long>(m_droppedFrames.load(std::memory_order_relaxed)),
                         m_drsScale, m_emaDrawMs, m_emaPostMs, m_emaSwapMs, m_emaCapMs, m_emaFrameMs, gpuMb, budMb, liveArrays,
                         m_arrMissDrawsThisFrame, static_cast<unsigned long long>(m_arrMissUploads), m_astcHits,
                         bCnt[0], bCnt[1], bCnt[2], bCnt[3], m_b2SavedThisFrame,
                         brkClip, brkArr, brkState, brkDyn, brkSkin, brkNoSrc,
                         b2BrkClip, b2BrkArr, b2BrkState, b2BrkDyn, b2BrkSkin, b2BrkNoSrc,
                         m_skinGpuDrawsThisFrame, m_skinCpuDrawsThisFrame, m_skinMaxQwordsThisFrame,
                         m_deferredUploads.size());
        }

        if (m_frameNum % 300u == 0u)
            evictStale();
    }

    static void setupDynamicVertexAttribs()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, r)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void *>(offsetof(Vertex, u)));
    }

    static void setupCompactVertexAttribs()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CompactVertex), reinterpret_cast<const void *>(offsetof(CompactVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_BYTE, GL_TRUE, sizeof(CompactVertex), reinterpret_cast<const void *>(offsetof(CompactVertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(CompactVertex), reinterpret_cast<const void *>(offsetof(CompactVertex, r)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(CompactVertex), reinterpret_cast<const void *>(offsetof(CompactVertex, u)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 1, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(CompactVertex), reinterpret_cast<const void *>(offsetof(CompactVertex, texLayer)));
    }

    static void setupSkinVertexAttribs()
    {
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, nx)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, r)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, u)));
        glEnableVertexAttribArray(4);
        glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, bone)));
        glEnableVertexAttribArray(5);
        glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(SkinVertex), reinterpret_cast<const void *>(offsetof(SkinVertex, weight)));
    }

    bool tryUploadAstcFromCache(uint64_t contentHash, uint64_t legacyId, MaliTex &tx)
    {
        char nameBuf[48];
        std::snprintf(nameBuf, sizeof(nameBuf), "%016llx.astcm", static_cast<unsigned long long>(contentHash));
        bool isMulti = m_astcFiles.count(nameBuf) != 0u;
        if (!isMulti)
        {
            std::snprintf(nameBuf, sizeof(nameBuf), "%016llx.astc", static_cast<unsigned long long>(contentHash));
            if (m_astcFiles.count(nameBuf) == 0u)
            {
                std::snprintf(nameBuf, sizeof(nameBuf), "%016llx.astc", static_cast<unsigned long long>(legacyId));
                if (m_astcFiles.count(nameBuf) == 0u)
                    return false;
            }
        }

        const std::string path = m_astcCacheDir + "/" + nameBuf;
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp)
            return false;

        uint8_t bx = 0, by = 0;
        uint32_t w = 0, h = 0, levels = 1u;
        GLenum fmt = 0;
        std::vector<uint32_t> levelSizes;
        if (isMulti)
        {
            uint8_t hdr[12] = {};
            if (std::fread(hdr, 1, 12, fp) != 12 ||
                hdr[0] != 'M' || hdr[1] != 'T' || hdr[2] != 'C' || hdr[3] != '1')
            {
                std::fclose(fp);
                return false;
            }
            bx = hdr[4];
            by = hdr[5];
            levels = hdr[6];
            w = static_cast<uint32_t>(hdr[8]) | (static_cast<uint32_t>(hdr[9]) << 8);
            h = static_cast<uint32_t>(hdr[10]) | (static_cast<uint32_t>(hdr[11]) << 8);
            fmt = astcBlockToFormat(bx, by);
            if (!fmt || !w || !h || levels == 0u || levels > 12u)
            {
                std::fclose(fp);
                return false;
            }
            levelSizes.resize(levels);
            m_astcScratch.clear();
            uint32_t lw = w, lh = h;
            for (uint32_t lv = 0; lv < levels; ++lv)
            {
                uint32_t lvBytes = 0;
                if (std::fread(&lvBytes, sizeof(lvBytes), 1, fp) != 1)
                {
                    std::fclose(fp);
                    return false;
                }
                const uint32_t expBytes = ((lw + bx - 1u) / bx) * ((lh + by - 1u) / by) * 16u;
                if (lvBytes != expBytes)
                {
                    std::fclose(fp);
                    return false;
                }
                const size_t off = m_astcScratch.size();
                m_astcScratch.resize(off + lvBytes);
                if (std::fread(m_astcScratch.data() + off, 1, lvBytes, fp) != lvBytes)
                {
                    std::fclose(fp);
                    return false;
                }
                levelSizes[lv] = lvBytes;
                lw = std::max(1u, lw >> 1);
                lh = std::max(1u, lh >> 1);
            }
        }
        else
        {
            uint8_t hdr[16] = {};
            if (std::fread(hdr, 1, 16, fp) != 16 ||
                hdr[0] != 0x13u || hdr[1] != 0xABu || hdr[2] != 0xA1u || hdr[3] != 0x5Cu)
            {
                std::fclose(fp);
                return false;
            }
            bx = hdr[4];
            by = hdr[5];
            fmt = astcBlockToFormat(bx, by);
            w = static_cast<uint32_t>(hdr[7]) | (static_cast<uint32_t>(hdr[8]) << 8) | (static_cast<uint32_t>(hdr[9]) << 16);
            h = static_cast<uint32_t>(hdr[10]) | (static_cast<uint32_t>(hdr[11]) << 8) | (static_cast<uint32_t>(hdr[12]) << 16);
            if (!fmt || !w || !h)
            {
                std::fclose(fp);
                return false;
            }
            const uint32_t blocksX = (w + bx - 1u) / bx;
            const uint32_t blocksY = (h + by - 1u) / by;
            const uint32_t dataBytes = blocksX * blocksY * 16u;
            m_astcScratch.resize(dataBytes);
            if (std::fread(m_astcScratch.data(), 1, dataBytes, fp) != dataBytes)
            {
                std::fclose(fp);
                return false;
            }
            levelSizes.push_back(dataBytes);
        }
        std::fclose(fp);

        const uint8_t fmtCode = astcBlockToFmtCode(bx, by);
        tx.fmtCode = fmtCode;
        tx.pendingEvict = false;
        releaseTexArrayLayer(tx.arrayId, tx.arrayLayer);
        tx.arrayId = -1;
        tx.arrayLayer = 0;
        const bool arrEligible = m_texArrayEnabled && fmtCode >= 3u && w >= 8u && w <= 256u && h >= 8u && h <= 256u;
        if (arrEligible && !allocTexArraySlot(w, h, levels, fmtCode, tx.arrayId, tx.arrayLayer))
            ++m_arrMissUploads;

        while (glGetError() != GL_NO_ERROR) {}
        if (tx.arrayId >= 0)
        {
            glBindTexture(GL_TEXTURE_2D_ARRAY, m_texArrays[tx.arrayId].tex);
            m_lastTexArr = m_texArrays[tx.arrayId].tex;
            size_t off = 0;
            uint32_t lw = w, lh = h;
            for (uint32_t lv = 0; lv < levels; ++lv)
            {
                const uint32_t lvBytes = levelSizes[lv];
                glCompressedTexSubImage3D(GL_TEXTURE_2D_ARRAY, static_cast<GLint>(lv),
                                          0, 0, static_cast<GLint>(tx.arrayLayer),
                                          static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 1,
                                          fmt, static_cast<GLsizei>(lvBytes), m_astcScratch.data() + off);
                off += lvBytes;
                lw = std::max(1u, lw >> 1);
                lh = std::max(1u, lh >> 1);
            }
            if (glGetError() != GL_NO_ERROR)
            {
                releaseTexArrayLayer(tx.arrayId, tx.arrayLayer);
                tx.arrayId = -1;
                tx.arrayLayer = 0;
                return false;
            }
            tx.has2D = false;
            tx.gpuBytes = 0;
            tx.rawData = m_astcScratch;
            tx.astcLevelSizes = std::move(levelSizes);
        }
        else
        {
            if (!tx.tex)
                glGenTextures(1, &tx.tex);
            glBindTexture(GL_TEXTURE_2D, tx.tex);
            m_lastTex = tx.tex;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            levels > 1u ? (m_trilinearEnabled ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1u));
            size_t off = 0;
            uint32_t lw = w, lh = h;
            for (uint32_t lv = 0; lv < levels; ++lv)
            {
                const uint32_t lvBytes = levelSizes[lv];
                glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), fmt,
                                       static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                                       static_cast<GLsizei>(lvBytes), m_astcScratch.data() + off);
                off += lvBytes;
                lw = std::max(1u, lw >> 1);
                lh = std::max(1u, lh >> 1);
            }
            if (glGetError() != GL_NO_ERROR)
                return false;
            tx.has2D = true;
            tx.gpuBytes = static_cast<uint64_t>(m_astcScratch.size());
            m_gpuTexBytes += tx.gpuBytes;
        }
        tx.width = w;
        tx.height = h;
        tx.levels = levels;
        tx.astc = true;
        tx.tex16 = false;
        return true;
    }

    static void computeBoundsAndAlpha(const Vertex *v, uint32_t count, float lo[3], float hi[3], bool &hasBox, uint8_t &minA, bool &interA)
    {
        lo[0] = lo[1] = lo[2] = 1e30f;
        hi[0] = hi[1] = hi[2] = -1e30f;
        minA = 0xFF;
        interA = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t a = v[i].a;
            if (a < minA) minA = a;
            if (a > 4u && a < 124u) interA = true;
            if (v[i].x < lo[0]) lo[0] = v[i].x; if (v[i].x > hi[0]) hi[0] = v[i].x;
            if (v[i].y < lo[1]) lo[1] = v[i].y; if (v[i].y > hi[1]) hi[1] = v[i].y;
            if (v[i].z < lo[2]) lo[2] = v[i].z; if (v[i].z > hi[2]) hi[2] = v[i].z;
        }
        hasBox = std::isfinite(lo[0] + lo[1] + lo[2] + hi[0] + hi[1] + hi[2]);
    }

    static void makeTarget(MaliTarget &t, uint32_t w, uint32_t h, bool withDepth, bool depthAsTex = false)
    {
        destroyTarget(t);
        t.width = w;
        t.height = h;
        t.hasDepth = withDepth;
        t.depthAsTex = withDepth && depthAsTex;
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);

        glGenTextures(1, &t.color);
        glBindTexture(GL_TEXTURE_2D, t.color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);

        if (withDepth)
        {
            if (depthAsTex)
            {
                glGenTextures(1, &t.depth);
                glBindTexture(GL_TEXTURE_2D, t.depth);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
            }
            else
            {
                glGenRenderbuffers(1, &t.depth);
                glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    static void destroyTarget(MaliTarget &t)
    {
        if (t.fbo) { glDeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
        if (t.color) { glDeleteTextures(1, &t.color); t.color = 0; }
        if (t.depth)
        {
            if (t.depthAsTex) glDeleteTextures(1, &t.depth);
            else glDeleteRenderbuffers(1, &t.depth);
            t.depth = 0;
        }
        t.width = t.height = 0;
        t.depthAsTex = false;
    }

    static void invalidateDepth(bool defaultFbo = false)
    {
#if defined(PS2X_MALI_GLES)
        static constexpr GLenum kDefaultDiscard[2] = {GL_DEPTH, GL_STENCIL};
        static constexpr GLenum kFboDiscard[2] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, defaultFbo ? kDefaultDiscard : kFboDiscard);
#else
        (void)defaultFbo;
#endif
    }

    void resetPsoAndUniformCaches()
    {
        m_lastPso = ~0u;
        m_lastTex = ~0u;
        m_lastTexArr = ~0u;
        m_lastSamplerIdx = ~0u;
        m_lastVao = ~0u;
        m_progOpaque.shadow.reset();
        m_progCut.shadow.reset();
        m_progScreen.shadow.reset();
        m_progSkinOpaque.shadow.reset();
        m_progSkinCut.shadow.reset();
        m_progDepth.shadow.reset();
        m_progOpaqueFast.shadow.reset();
        m_progCutFast.shadow.reset();
        m_progSoftFast.shadow.reset();
        m_progSoft.shadow.reset();
        m_progSkinOpaqueFast.shadow.reset();
        m_progSkinCutFast.shadow.reset();
        m_progSkinSoftFast.shadow.reset();
        m_progArrOpaqueFast.shadow.reset();
        m_progArrCutFast.shadow.reset();
        m_progArrSoftFast.shadow.reset();
        m_progArrOpaque.shadow.reset();
        m_progArrCut.shadow.reset();
        m_progArrSoft.shadow.reset();
        m_progWater.shadow.reset();
        glDisable(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_bgTarget.color ? m_bgTarget.color : m_whiteTex);
        glActiveTexture(GL_TEXTURE0);
    }

    const ShaderProg &progForIdx(uint32_t idx) const
    {
        switch (idx)
        {
        case 0u: return m_progOpaque;
        case 1u: return m_progCut;
        case 2u: return m_progScreen;
        case 3u: return m_progSkinOpaque;
        case 4u: return m_progSkinCut;
        case 6u: return m_progOpaqueFast;
        case 7u: return m_progCutFast;
        case 8u: return m_progSoftFast;
        case 9u: return m_progSkinOpaqueFast;
        case 10u: return m_progSkinCutFast;
        case 11u: return m_progSkinSoftFast;
        case 12u: return m_progSoft;
        case 13u: return m_progArrOpaqueFast;
        case 14u: return m_progArrCutFast;
        case 15u: return m_progArrSoftFast;
        case 16u: return m_progArrOpaque;
        case 17u: return m_progArrCut;
        case 18u: return m_progArrSoft;
        case 19u: return m_progWater;
        default: return m_progDepth;
        }
    }

    const ShaderProg &applyPso(uint32_t psoKey)
    {
        const uint32_t progIdx = psoKey & 31u;
        const ShaderProg &prog = progForIdx(progIdx);
        if (psoKey == m_lastPso)
            return prog;

        if ((m_lastPso == ~0u) || ((m_lastPso & 31u) != progIdx))
            glUseProgram(prog.id);

        const Blend blend = static_cast<Blend>((psoKey >> 5) & 3u);
        if ((m_lastPso == ~0u) || (((m_lastPso >> 5) & 3u) != static_cast<uint32_t>(blend)))
        {
            if (blend == Blend::Opaque)
            {
                glDisable(GL_BLEND);
            }
            else
            {
                glEnable(GL_BLEND);
                if (blend == Blend::Additive)
                {
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                }
                else if (blend == Blend::Subtract)
                {
                    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                }
                else
                {
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            }
        }

        const bool depthTest = ((psoKey >> 7) & 1u) != 0u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 7) & 1u) != (depthTest ? 1u : 0u)))
        {
            if (depthTest) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
        }

        const bool depthWrite = ((psoKey >> 8) & 1u) != 0u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 8) & 1u) != (depthWrite ? 1u : 0u)))
            glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);

        const uint32_t df = (psoKey >> 9) & 3u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 9) & 3u) != df))
        {
            const GLenum fn = (df == 1u) ? GL_LESS : (df == 2u ? GL_EQUAL : (df == 3u ? GL_ALWAYS : GL_LEQUAL));
            glDepthFunc(fn);
        }

        m_lastPso = psoKey;
        return prog;
    }

    void bindCommonUniforms(const ShaderProg &prog, const QueuedDraw &q, const FramePacket &pkt,
                            bool allowBgFog, float minAlpha, float maxAlpha, int instCount = 1,
                            int16_t texArrayId = -1, float texLayerBias = 0.0f)
    {
        float texAlphaScale = 0.0f;
        uint32_t texLevels = 1u;
        if (texArrayId >= 0 && static_cast<size_t>(texArrayId) < m_texArrays.size())
        {
            MaliTexArray &arr = m_texArrays[texArrayId];
            if (arr.tex != m_lastTexArr)
            {
                glBindTexture(GL_TEXTURE_2D_ARRAY, arr.tex);
                m_lastTexArr = arr.tex;
            }
            texLevels = arr.levels;
            texAlphaScale = arr.tex16 ? 1.0f : 2.0f;
        }
        else
        {
            if (q.tex && !q.tex->has2D)
                ensureStandalone2D(*q.tex);
            const bool hasTex = (q.tex != nullptr && q.tex->tex != 0u);
            const GLuint glTex = hasTex ? q.tex->tex : m_whiteTex;
            if (glTex != m_lastTex)
            {
                glBindTexture(GL_TEXTURE_2D, glTex);
                m_lastTex = glTex;
            }
            texLevels = hasTex ? q.tex->levels : 1u;
            texAlphaScale = !hasTex ? 0.0f : (q.tex->tex16 ? 1.0f : 2.0f);
        }
        if (m_samplers[0])
        {
            const uint32_t sampIdx = ((texLevels > 1u) ? 4u : 0u) | (q.state.clamp & 3u);
            if (sampIdx != m_lastSamplerIdx)
            {
                glBindSampler(0, m_samplers[sampIdx]);
                m_lastSamplerIdx = sampIdx;
            }
        }

        UniformShadow &sh = prog.shadow;
        if (prog.uTexLayerBias >= 0 && sh.texLayerBias != texLayerBias)
        {
            sh.texLayerBias = texLayerBias;
            glUniform1f(prog.uTexLayerBias, texLayerBias);
        }
        if (prog.uClip >= 0 && (!sh.clipValid || std::memcmp(sh.clip, q.clip, sizeof(sh.clip)) != 0))
        {
            std::memcpy(sh.clip, q.clip, sizeof(sh.clip));
            sh.clipValid = true;
            glUniform4fv(prog.uClip, 4, q.clip);
        }
        if (instCount == 1 && prog.uInstanceCount >= 0 && sh.instCount != 1)
        {
            sh.instCount = 1;
            glUniform1i(prog.uInstanceCount, 1);
        }

        if (q.skinned && prog.uBones >= 0 && q.state.skinPalette &&
            q.state.skinPalette != sh.lastSkinPalette && q.state.skinPalette <= pkt.skinPalettes.size())
        {
            const SkinPaletteEntry &pe = pkt.skinPalettes[q.state.skinPalette - 1u];
            const uint32_t qwords = std::min(pe.qwordCount, kMaxSkinQwords);
            if (qwords > 0u && static_cast<size_t>(pe.offsetFloats) + static_cast<size_t>(qwords) * 4u <= pkt.skinPaletteData.size())
            {
                if (qwords > m_skinMaxQwordsThisFrame)
                    m_skinMaxQwordsThisFrame = qwords;
                glUniform4fv(prog.uBones, static_cast<GLsizei>(qwords), pkt.skinPaletteData.data() + pe.offsetFloats);
                if (m_statsEnabled && !m_skinErrLogged)
                {
                    const GLenum err = glGetError();
                    if (err != GL_NO_ERROR)
                    {
                        std::fprintf(stderr, "[mali:skin:err] frame=%llu glErr=0x%x uBonesLoc=%d qwords=%u pal=%u\n",
                                     static_cast<unsigned long long>(m_frameNum), static_cast<unsigned>(err),
                                     prog.uBones, qwords, q.state.skinPalette);
                        m_skinErrLogged = true;
                    }
                }
                sh.lastSkinPalette = q.state.skinPalette;
            }
        }

        const bool v = sh.valid;
        const float litMode = q.state.fresnel ? -1.0f : (q.state.lit ? 1.0f : 0.0f);
        setCached4f(prog.uMisc, sh.misc, v, litMode, q.state.opacity, q.state.uvOffset[0], q.state.uvOffset[1]);

        if (q.state.lit || q.state.fresnel)
        {
            if (!sh.lightValid || std::memcmp(sh.light, q.state.light, sizeof(sh.light)) != 0)
            {
                std::memcpy(sh.light, q.state.light, sizeof(sh.light));
                sh.lightValid = true;
                if (prog.uDirections >= 0) glUniform4fv(prog.uDirections, 4, q.state.light);
                if (prog.uColours >= 0) glUniform4fv(prog.uColours, 4, q.state.light + 16);
            }
        }

        const bool vfog = m_fog.vertexFog && (!q.state.background || q.state.backgroundFge);
        setCached4f(prog.uFogB, sh.fogB, v, 0.0f, 0.0f, vfog ? 1.0f : 0.0f, m_fog.base);
        setCached4f(prog.uFogC, sh.fogC, v, m_fog.scale, q.state.lodK, q.state.lodScale, q.state.maxLod);
        setCached4f(prog.uFogCol, sh.fogCol, v, m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], texAlphaScale);

        if (q.state.reflectionSample)
        {
            setCached4f(prog.uFogA, sh.fogA, v, q.state.mirrorRipple[0], q.state.mirrorRipple[1], 0.0f, 0.0f);
            setCached4f(prog.uGuard, sh.guard, v, 0.0f, 0.0f, 0.0f, 0.0f);
            setCached4f(prog.uAlpha, sh.alpha, v, minAlpha, maxAlpha, q.state.reflectionTint[3], 0.0f);
        }
        else
        {
            const bool bgFog = allowBgFog && m_fog.enabled && q.state.fogged && !q.state.background;
            setCached4f(prog.uFogA, sh.fogA, v, m_fog.start, m_fog.width, static_cast<float>(m_fog.steps), bgFog ? 1.0f : 0.0f);

            const bool useGuard = q.state.background && m_fog.skyNear > 0.0f;
            setCached4f(prog.uGuard, sh.guard, v, useGuard ? 1.0f : 0.0f, m_fog.skyNear, q.state.screenDepthTest ? 1.0f : 0.0f, 0.0f);
            setCached4f(prog.uAlpha, sh.alpha, v, minAlpha, maxAlpha, 0.0f, 0.0f);
        }
        sh.valid = true;
    }

    void emitDrawSingle(const QueuedDraw &q, const FramePacket &pkt,
                        bool allowBgFog, float minAlpha, float maxAlpha)
    {
        if (!q.mesh)
            return;
        const MaliMesh &mesh = *q.mesh;

        uint32_t psoKey = q.psoKey;
        int16_t useArrId = -1;
        float layerBias = 0.0f;
        if (q.texArrayId >= 0 && !mesh.dynamic && !q.skinned)
        {
            const uint32_t baseProg = psoKey & 31u;
            const uint32_t arrProg = toArrayProg(baseProg);
            if (arrProg != baseProg)
            {
                psoKey = (psoKey & ~31u) | (arrProg & 31u);
                useArrId = q.texArrayId;
                layerBias = static_cast<float>(q.texArrayLayer);
            }
        }
        const ShaderProg &prog = applyPso(psoKey);
        bindCommonUniforms(prog, q, pkt, allowBgFog, minAlpha, maxAlpha, 1, useArrId, layerBias);

        const GLuint targetVao = mesh.dynamic ? m_arenaVao[m_arenaSlot] : mesh.vao;
        if (targetVao != m_lastVao)
        {
            glBindVertexArray(targetVao);
            m_lastVao = targetVao;
        }
        if (mesh.indexed)
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(mesh.dynamic ? mesh.firstVertex : 0u), static_cast<GLsizei>(mesh.vertexCount));
        ++m_drawCallsThisFrame;
    }

    void emitMergedRun(const QueuedDraw &q, const MergeRun &run, const FramePacket &pkt,
                       bool allowBgFog, float minAlpha, float maxAlpha)
    {
        uint32_t psoKey = q.psoKey;
        if (run.texArrayId >= 0)
            psoKey = (psoKey & ~31u) | (toArrayProg(psoKey & 31u) & 31u);
        const ShaderProg &prog = applyPso(psoKey);
        bindCommonUniforms(prog, q, pkt, allowBgFog, minAlpha, maxAlpha, 1, run.texArrayId, 0.0f);
        if (run.vao != m_lastVao)
        {
            glBindVertexArray(run.vao);
            m_lastVao = run.vao;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(run.indexCount), GL_UNSIGNED_INT, nullptr);
        ++m_drawCallsThisFrame;
    }

    void emitInstanced(const QueuedDraw *const *draws, uint32_t count, const FramePacket &pkt,
                       bool allowBgFog, float minAlpha, float maxAlpha, uint32_t psoKey)
    {
        if (!count || !draws[0]->mesh)
            return;
        const QueuedDraw &q0 = *draws[0];
        const MaliMesh &mesh = *q0.mesh;
        int16_t useArrId = -1;
        float layerBias = 0.0f;
        if (q0.texArrayId >= 0 && !mesh.dynamic && !q0.skinned)
        {
            const uint32_t baseProg = psoKey & 31u;
            const uint32_t arrProg = toArrayProg(baseProg);
            if (arrProg != baseProg)
            {
                psoKey = (psoKey & ~31u) | (arrProg & 31u);
                useArrId = q0.texArrayId;
                layerBias = static_cast<float>(q0.texArrayLayer);
            }
        }
        const ShaderProg &prog = applyPso(psoKey);
        bindCommonUniforms(prog, q0, pkt, allowBgFog, minAlpha, maxAlpha, static_cast<int>(count), useArrId, layerBias);

        if (prog.uInstanceClips >= 0 && prog.uInstanceCount >= 0)
        {
            float clips[kMaxInstances * 16];
            for (uint32_t k = 0; k < count; ++k)
                std::memcpy(clips + k * 16u, draws[k]->clip, 16u * sizeof(float));
            glUniform4fv(prog.uInstanceClips, static_cast<GLsizei>(count * 4u), clips);
            if (prog.shadow.instCount != static_cast<int>(count))
            {
                prog.shadow.instCount = static_cast<int>(count);
                glUniform1i(prog.uInstanceCount, static_cast<GLint>(count));
            }
        }

        const GLuint targetVao = mesh.vao;
        if (targetVao != m_lastVao)
        {
            glBindVertexArray(targetVao);
            m_lastVao = targetVao;
        }
        if (mesh.indexed)
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(count));
        else
            glDrawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh.vertexCount), static_cast<GLsizei>(count));
        ++m_drawCallsThisFrame;
    }

    void emitBucketSingle(const QueuedDraw &q, const FramePacket &pkt)
    {
        if (q.bucket == 0u)
        {
            emitDrawSingle(q, pkt, true, 0.0f, 0.0f);
        }
        else if (q.bucket == 1u)
        {
            const float aref = static_cast<float>((q.state.alphaTest >> 4) & 0xFFu) / 128.0f;
            emitDrawSingle(q, pkt, true, std::max(0.02f, aref), 0.0f);
            if (needsAlphaTestPass2(q, aref))
            {
                QueuedDraw pass2 = q;
                const bool fastFog = ((q.psoKey & 31u) == 7u || (q.psoKey & 31u) == 10u);
                const uint32_t p2Prog = m_fastShadersEnabled ? (q.skinned ? (fastFog ? 11u : 4u) : (fastFog ? 8u : 12u))
                                                             : (q.skinned ? 4u : 1u);
                pass2.psoKey = makePsoKey(p2Prog, q.state.blend, true, false, 1u);
                emitDrawSingle(pass2, pkt, true, 0.02f, aref);
            }
            else if (((q.state.alphaTest >> 12) & 3u) == 1u || ((q.state.alphaTest >> 12) & 3u) == 3u)
            {
                ++m_pass2SkipThisFrame;
            }
        }
        else if (q.bucket == 2u)
        {
            float minA = 0.02f;
            const uint32_t at = q.state.alphaTest;
            const uint32_t atst = (at >> 1) & 7u;
            const uint32_t afail = (at >> 12) & 3u;
            if ((at & 1u) && (atst == 5u || atst == 6u) && afail == 0u)
                minA = std::max(0.02f, static_cast<float>((at >> 4) & 0xFFu) / 128.0f);
            emitDrawSingle(q, pkt, true, minA, 0.0f);
        }
        else
        {
            emitDrawSingle(q, pkt, false, 0.01f, 0.0f);
        }
    }

    void evictOldMergeSrc(uint64_t frameNum, uint64_t ageThreshold, uint64_t needBytes)
    {
        m_evictRunsScratch.clear();
        for (const auto &[id, m] : m_meshes)
        {
            if (!m.dynamic && !m.skinned && m.srcBytes > 0u && m.lastUsed + ageThreshold < frameNum)
                m_evictRunsScratch.push_back({m.lastUsed, id});
        }
        std::sort(m_evictRunsScratch.begin(), m_evictRunsScratch.end());
        for (const auto &[used, id] : m_evictRunsScratch)
        {
            (void)used;
            auto it = m_meshes.find(id);
            if (it == m_meshes.end())
                continue;
            MaliMesh &m = it->second;
            if (!m.dynamic && !m.skinned && m.srcBytes > 0u)
            {
                std::vector<CompactVertex>().swap(m.srcVerts);
                std::vector<uint32_t>().swap(m.srcIndices);
                if (m_mergeSrcBytes >= m.srcBytes)
                    m_mergeSrcBytes -= m.srcBytes;
                else
                    m_mergeSrcBytes = 0;
                m.srcBytes = 0;
            }
            if (needBytes > 0u && m_mergeSrcBytes + needBytes <= kMaxMergeSrcBytes)
                break;
        }
    }

    void evictStale()
    {
        for (auto it = m_meshes.begin(); it != m_meshes.end();)
        {
            if (it->second.dynamic && it->second.lastUsed + 30u < m_frameNum)
            {
                it = m_meshes.erase(it);
                m_evictions.fetch_add(1u, std::memory_order_relaxed);
            }
            else
            {
                ++it;
            }
        }
        if (m_mergeSrcBytes > (kMaxMergeSrcBytes * 7ull) / 8ull)
            evictOldMergeSrc(m_frameNum, 1200u, kMaxMergeSrcBytes / 4ull);
        for (auto it = m_mergeRuns.begin(); it != m_mergeRuns.end();)
        {
            if (it->second.lastUsed + 1800u < m_frameNum)
            {
                MergeRun &er = it->second;
                if (m_lastVao == er.vao)
                    m_lastVao = ~0u;
                if (er.vao) glDeleteVertexArrays(1, &er.vao);
                if (er.vbo) glDeleteBuffers(1, &er.vbo);
                if (er.ibo) glDeleteBuffers(1, &er.ibo);
                if (m_mergeRunBytes >= er.bytes)
                    m_mergeRunBytes -= er.bytes;
                it = m_mergeRuns.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Reclaim CPU rawData from array textures unused for > 1800 frames (~30-45s) without evicting the GPU array texture.
        for (auto &[id, t] : m_textures)
        {
            if (t.arrayId >= 0 && !t.rawData.empty() && t.lastUsed + 1800u < m_frameNum)
            {
                std::vector<uint8_t>().swap(t.rawData);
                std::vector<uint32_t>().swap(t.astcLevelSizes);
            }
        }

        // Phase 1 of 2-phase tombstone eviction: reclaim empty texture arrays before computing projectedBytes.
        // Never evict static meshes or textures by TTL when total GPU memory is within m_cacheBudgetBytes,
        // so turning the camera never unloads or rebuilds previously visited scenery or textures.
        reclaimEmptyTexArrays();
        std::vector<uint64_t> candMeshes;
        std::vector<uint64_t> candTex;
        uint64_t projectedBytes = m_gpuMeshBytes + m_gpuTexBytes + m_gpuTexArrayBytes;

        if (projectedBytes > m_cacheBudgetBytes)
        {
            auto texEffectiveBytes = [this](const MaliTex &t) -> uint64_t {
                if (t.gpuBytes > 0u)
                    return t.gpuBytes;
                if (t.arrayId >= 0 && static_cast<size_t>(t.arrayId) < m_texArrays.size())
                {
                    const MaliTexArray &arr = m_texArrays[t.arrayId];
                    if (arr.capacity > 0u && arr.gpuBytes > 0u)
                        return std::max<uint64_t>(256ull, arr.gpuBytes / arr.capacity);
                }
                return std::max<uint64_t>(256ull, static_cast<uint64_t>(t.width) * t.height * (t.tex16 ? 2ull : 4ull));
            };

            struct BudgetCand
            {
                uint64_t lastUsed;
                uint64_t id;
                uint64_t bytes;
                bool isMesh;
            };
            std::vector<BudgetCand> cands;
            for (const auto &[id, m] : m_meshes)
            {
                if (!m.dynamic && !m.pendingEvict && m.lastUsed + 600u < m_frameNum)
                    cands.push_back({m.lastUsed, id, std::max<uint64_t>(256ull, m.gpuBytes), true});
            }
            for (const auto &[id, t] : m_textures)
            {
                if (!t.pendingEvict && t.lastUsed + 600u < m_frameNum)
                    cands.push_back({t.lastUsed, id, texEffectiveBytes(t), false});
            }
            std::sort(cands.begin(), cands.end(), [](const BudgetCand &a, const BudgetCand &b) {
                return a.lastUsed < b.lastUsed;
            });
            const uint64_t targetBytes = (m_cacheBudgetBytes * 7ull) / 8ull;
            for (const BudgetCand &c : cands)
            {
                if (projectedBytes <= targetBytes)
                    break;
                if (c.isMesh)
                {
                    if (auto it = m_meshes.find(c.id); it != m_meshes.end())
                    {
                        it->second.pendingEvict = true;
                        candMeshes.push_back(c.id);
                    }
                }
                else
                {
                    if (auto it = m_textures.find(c.id); it != m_textures.end())
                    {
                        it->second.pendingEvict = true;
                        candTex.push_back(c.id);
                    }
                }
                projectedBytes = (projectedBytes >= c.bytes) ? (projectedBytes - c.bytes) : 0ull;
            }
        }

        if (!candMeshes.empty() || !candTex.empty())
        {
            if (m_asyncEnabled)
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                m_pendingEvictMeshes.insert(m_pendingEvictMeshes.end(), candMeshes.begin(), candMeshes.end());
                m_pendingEvictTex.insert(m_pendingEvictTex.end(), candTex.begin(), candTex.end());
            }
            else
            {
                for (uint64_t id : candMeshes)
                {
                    m_knownMeshes.erase(id);
                    if (auto it = m_meshes.find(id); it != m_meshes.end())
                    {
                        deleteMeshGpu(it->second);
                        m_meshes.erase(it);
                        m_evictions.fetch_add(1u, std::memory_order_relaxed);
                    }
                }
                for (uint64_t id : candTex)
                {
                    m_knownTextures.erase(id);
                    if (auto it = m_textures.find(id); it != m_textures.end())
                    {
                        releaseTexArrayLayer(it->second.arrayId, it->second.arrayLayer);
                        if (m_lastTex == it->second.tex && it->second.tex != 0u)
                            m_lastTex = ~0u;
                        if (it->second.tex) glDeleteTextures(1, &it->second.tex);
                        if (m_gpuTexBytes >= it->second.gpuBytes)
                            m_gpuTexBytes -= it->second.gpuBytes;
                        else
                            m_gpuTexBytes = 0;
                        m_textures.erase(it);
                        m_evictions.fetch_add(1u, std::memory_order_relaxed);
                    }
                }
            }
        }
        reclaimEmptyTexArrays();
    }

    std::function<void()> m_makeCurrent;
    bool m_initialized = false;
    bool m_asyncEnabled = true;
    bool m_mailboxEnabled = true;
    bool m_hasAstc = false;
    bool m_cullEnabled = true;
    bool m_tex16Enabled = true;
    bool m_tex5551Enabled = false;
    bool m_waterEnabled = true;
    bool m_drsEnabled = true;
    bool m_shaderCacheEnabled = true;
    bool m_gpuSkinEnabled = true;
    bool m_mergeEnabled = true;
    bool m_texArrayEnabled = true;
    bool m_instancingEnabled = true;
    bool m_keepCurrentEnabled = true;
    bool m_directScanoutEnabled = true;
    bool m_fastShadersEnabled = true;
    bool m_hasLetterboxCrop = false;
    int m_screenMode = -1;
    bool m_drsDrawOnly = false;
    int m_atPass2Mode = 1;
    int m_furShells = 4;
    bool m_zPrepassEnabled = false;
    bool m_postEnabled = true;
    bool m_postBloomEnabled = true;
    bool m_exposureEnabled = true;
    bool m_dofEnabled = false;
    int m_motionBlurMode = 0;
    bool m_caEnabled = false;
    bool m_lensFlareEnabled = false;
    bool m_aoEnabled = false;
    bool m_trilinearEnabled = false;
    bool m_hasAniso = false;
    int m_anisoLevel = 0;
    float m_maxAniso = 1.0f;
    bool m_fxaaEnabled = false;
    bool m_rcasEnabled = false;
    bool m_postUpscaleEnabled = false;
    bool m_lastPresentedPost = false;
    bool m_lastPresentedDirect = false;
    uint32_t m_lastDirectW = 0;
    uint32_t m_lastDirectH = 0;
    bool m_dumpTexEnabled = false;
    bool m_paceEnabled = false;
    bool m_statsEnabled = false;
    uint64_t m_uploadBudgetUs = 1500;
    uint64_t m_uploadUsThisFrame = 0;
    uint64_t m_mergeBudgetUs = 2500;
    uint32_t m_mergeChunkMax = 64u;
    uint32_t m_maxArrayLayers = 64u;
    uint64_t m_cacheBudgetBytes = 160ull * 1024ull * 1024ull;
    uint64_t m_texArrayBudgetBytes = 100ull * 1024ull * 1024ull;
    uint32_t m_bgRate = 2;
    int m_targetFps = 30;
    int m_drsTargetFps = 30;
    float m_drsMin = 0.60f;
    float m_drsMax = 1.0f;
    float m_drsScale = 1.0f;
    double m_emaDrawMs = 0.0;
    double m_emaPostMs = 0.0;
    double m_emaSwapMs = 0.0;
    double m_emaCapMs = 0.0;
    double m_emaFrameMs = 0.0;
    double m_emaGpuFrameMs = 0.0;
    int m_drsOverStreak = 0;
    int m_drsUnderStreak = 0;
    std::chrono::steady_clock::time_point m_lastPresentTime{};

    std::string m_astcCacheDir;
    std::string m_texDumpDir;
    std::string m_shaderCacheDir;
    std::unordered_set<std::string> m_astcFiles;

    // Game-thread state
    uint64_t m_recFrameNum = 0;
    uint32_t m_recDrawSeq = 0;
    FramePacket m_rec;
    std::unordered_set<uint64_t> m_knownMeshes;
    std::unordered_set<uint64_t> m_knownTextures;

    // Async render-thread synchronization
    std::thread m_renderThread;
    std::mutex m_mtx;
    std::condition_variable m_cvWork;
    std::condition_variable m_cvDone;
    bool m_initDone = false;
    bool m_stopRequested = false;
    bool m_hasPendingFrame = false;
    bool m_renderingActive = false;
    FramePacket m_pending;
    bool m_readbackReq = false;
    bool m_readbackDone = false;
    std::vector<uint8_t> m_readbackRgba;
    uint32_t m_readbackW = 0, m_readbackH = 0;

    // Render-thread GL state
    uint64_t m_frameNum = 0;
    std::atomic<uint64_t> m_evictions{0};
    std::atomic<uint64_t> m_droppedFrames{0};
    uint32_t m_culledThisFrame = 0;
    uint32_t m_drawCallsThisFrame = 0;
    uint32_t m_mergeSavedThisFrame = 0;
    uint32_t m_b2SavedThisFrame = 0;
    uint32_t m_instSavedThisFrame = 0;
    uint32_t m_pass2SkipThisFrame = 0;
    uint32_t m_texWrapThisFrame = 0;
    uint32_t m_arrMissDrawsThisFrame = 0;
    uint64_t m_arrMissUploads = 0;
    uint32_t m_astcHits = 0;
    uint32_t m_skinGpuDrawsThisFrame = 0;
    uint32_t m_skinCpuDrawsThisFrame = 0;
    uint32_t m_skinMaxQwordsThisFrame = 0;
    uint32_t m_skinInvalidThisFrame = 0;
    bool m_skinErrLogged = false;
    bool m_skinInvalidLogged = false;
    std::deque<UploadCmd> m_deferredUploads;
    uint64_t m_mergeSrcBytes = 0;
    uint64_t m_mergeRunBytes = 0;
    uint64_t m_gpuMeshBytes = 0;
    uint64_t m_gpuTexBytes = 0;
    uint64_t m_gpuTexArrayBytes = 0;
    std::vector<uint64_t> m_pendingEvictMeshes;
    std::vector<uint64_t> m_pendingEvictTex;

    ShaderProg m_progOpaque{}, m_progCut{}, m_progScreen{};
    ShaderProg m_progSkinOpaque{}, m_progSkinCut{}, m_progDepth{}, m_progPost{};
    ShaderProg m_progOpaqueFast{}, m_progCutFast{}, m_progSoftFast{}, m_progSoft{};
    ShaderProg m_progSkinOpaqueFast{}, m_progSkinCutFast{}, m_progSkinSoftFast{};
    ShaderProg m_progArrOpaqueFast{}, m_progArrCutFast{}, m_progArrSoftFast{};
    ShaderProg m_progArrOpaque{}, m_progArrCut{}, m_progArrSoft{};
    ShaderProg m_progWater{};
    ShaderProg m_progBloomMask{}, m_progKawase{}, m_progTemporal{}, m_progAo{}, m_progAoBlur{};
    GLuint m_emptyVao = 0;
    GLuint m_whiteTex = 0;
    MaliTarget m_mainTarget[2]{}, m_postTarget{}, m_bgTarget{};
    MaliTarget m_bloomQuarter{}, m_bloomEighth{}, m_bloomHist[2]{}, m_dofHalf{}, m_aoTarget{}, m_aoBlur{};
    int m_mainIdx = 0;
    int m_lastPresentedMain = 0;
    int m_bloomHistIdx = 0;
    bool m_bloomHistValid = false;
    bool m_prevMainValid = false;

    GLuint m_arenaVao[3] = {};
    GLuint m_arenaVbo[3] = {};
    GLuint m_samplers[8] = {};
    int m_arenaSlot = 0;

    uint32_t m_lastPso = ~0u;
    GLuint m_lastTex = ~0u;
    GLuint m_lastTexArr = ~0u;
    uint32_t m_lastSamplerIdx = ~0u;
    GLuint m_lastVao = ~0u;

    Fog m_fog{};
    Bloom m_bloom{};
    std::vector<uint16_t> m_tex16Scratch;
    std::vector<uint8_t> m_astcScratch;
    std::vector<CompactVertex> m_mergeVertsScratch;
    std::vector<uint32_t> m_mergeIndicesScratch;
    std::vector<std::pair<uint64_t, uint64_t>> m_evictRunsScratch;
    std::vector<const QueuedDraw *> m_instScratch;
    std::vector<QueuedDraw> m_qBg, m_qAll;
    std::unordered_set<uint64_t> m_drawnMeshIdsScratch;
    std::unordered_set<uint64_t> m_drawnTexIdsScratch;
    std::unordered_set<uint64_t> m_dumpedTexIds;
    std::unordered_map<uint64_t, MaliMesh> m_meshes;
    std::unordered_map<uint64_t, MaliTex> m_textures;
    std::vector<MaliTexArray> m_texArrays;
    std::unordered_map<uint64_t, MergeRun> m_mergeRuns;
};

} // namespace

Backend *createMaliBackend(std::function<void()> makeCurrent)
{
    auto *b = new MaliBackend(std::move(makeCurrent));
    if (!b->init())
    {
        delete b;
        return nullptr;
    }
    return b;
}

} // namespace native_renderer::detail

#else

namespace native_renderer::detail
{
Backend *createMaliBackend(std::function<void()>) { return nullptr; }
} // namespace native_renderer::detail

#endif
