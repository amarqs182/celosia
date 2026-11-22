// Dedicated Mali-G31 (GLES 3.2 / GL 3.3 Core) native renderer backend for Celosia R36S.
// Plugs directly into native_renderer::detail::Backend (selected via SOTC_RENDERER=mali).
//
// Phase 6 (Draw-Call Elimination — targeting 2,617 -> ~400-700 GL draws on R36S):
// - Preserves all Phase 5.1 fixes (6a20f0f): uBones[192] via glUniform4fv (no T-pose),
//   per-texture wrapClamp cache + pre-resolved MaliTex*, uClip shadow cache, m_postTarget
//   skip when bloom=0, mailbox handover preserving uploads, reusable FramePacket.
// - Phase 6A: Skip m_qBg -> m_bgTarget pass completely when !m_fog.enabled (ZFOG=0).
// - Phase 6B: Smart Alpha-Test Pass-2 elision (SOTC_MALI_AT_PASS2=0|1|2, default 1=auto:
//   skips Pass 2 when texture & vertices have only binary alpha — zero visual change).
// - Phase 6C: Safe Static Geometry Merging in GL_UNSIGNED_INT (SOTC_MALI_MERGE=1) over
//   28-byte CompactVertex with 1.5 ms/frame build budget and >=16-frame eviction grace.
// - Telemetry [mali:stats] reports draws, mergeSaved, pass2Skip, culled, texWrap, drop, drs, renderMs.

#include "native_backend.h"
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
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
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

namespace native_renderer::detail
{
namespace
{

constexpr uint32_t kMaxSkinQwords = native_renderer::kSkinPaletteQwords; // 48 bones * 4 vec4s via glUniform4fv (libMali r13p0 safe)
constexpr uint64_t kMaxMergeSrcBytes = 24ull * 1024ull * 1024ull;
constexpr uint64_t kMaxMergeRunBytes = 16ull * 1024ull * 1024ull;

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
    int8_t nx, ny, nz, pad;
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
        dst[i].pad = 0;
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
    vec4 c = uClip[0] * aPos.x + uClip[1] * aPos.y + uClip[2] * aPos.z + uClip[3];
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

// Phase 5.1 fix preserved: uniform highp vec4 uBones[192] via glUniform4fv (no T-pose on libMali r13p0).
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
    vec3 pos = vec3(0.0);
    vec3 nrm = vec3(0.0);
    float wSum = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        float w = aWeight[i];
        if (w > 1e-5)
        {
            int b = clamp(int(aBone[i] + 0.5), 0, 188);
            vec4 bp = uBones[b] * aPos.x + uBones[b + 1] * aPos.y + uBones[b + 2] * aPos.z + uBones[b + 3];
            vec3 bn = uBones[b].xyz * aNormal.x + uBones[b + 1].xyz * aNormal.y + uBones[b + 2].xyz * aNormal.z;
            pos += bp.xyz * w;
            nrm += bn * w;
            wSum += w;
        }
    }
    if (wSum < 1e-5)
    {
        pos = aPos;
        nrm = aNormal;
    }

    vec4 c = uClip[0] * pos.x + uClip[1] * pos.y + uClip[2] * pos.z + uClip[3];
    gl_Position = vec4(c.x, c.y, c.w - 0.2, c.w);

    float wSafe = max(c.w, 1e-3);
    float fog = uFogB.z > 0.5 ? clamp(uFogB.w + uFogC.x / wSafe, 0.0, 255.0) / 255.0 : 1.0;
    vUV = vec4(aUV + uMisc.zw, c.w, fog * c.w);
    vLogW = log2(wSafe);

    vec4 col = aColor * (1.0 / 128.0);
    col.a *= uMisc.y;
    if (uMisc.x > 0.5)
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
    mediump vec3 rgb = raw / (vec3(1.0) + raw * 0.35);
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
    mediump float alpha = clamp(t.a * (uFogCol.w > 0.5 ? 2.0 : 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < max(0.02, uAlpha.x) || (uAlpha.y > 0.0 && alpha >= uAlpha.y))
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    mediump vec3 rgb = raw / (vec3(1.0) + raw * 0.35);
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
    mediump float alpha = clamp(t.a * (uFogCol.w > 0.5 ? 2.0 : 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < 0.01)
        discard;
    mediump vec3 raw = max(t.rgb * vColor.rgb, vec3(0.0));
    oColor = vec4(raw / (vec3(1.0) + raw * 0.35), alpha);
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
uniform highp vec4 uMisc;
out mediump vec4 oColor;
void main()
{
    mediump vec3 c = texture(uTex, vUV).rgb;
    if (uMisc.z > 0.001)
    {
        highp vec2 o = uMisc.xy * 1.75;
        mediump vec3 b0 = texture(uTex, vUV + vec2(-o.x, -o.y)).rgb;
        mediump vec3 b1 = texture(uTex, vUV + vec2( o.x, -o.y)).rgb;
        mediump vec3 b2 = texture(uTex, vUV + vec2(-o.x,  o.y)).rgb;
        mediump vec3 b3 = texture(uTex, vUV + vec2( o.x,  o.y)).rgb;
        mediump vec3 avg = (b0 + b1 + b2 + b3) * 0.25;
        mediump float lum = dot(avg, vec3(0.299, 0.587, 0.114));
        mediump float soft = clamp((lum - uMisc.w) / max(1.0 - uMisc.w, 0.1), 0.0, 1.0);
        c += avg * soft * uMisc.z;
    }
    oColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
)";

struct UniformShadow
{
    bool valid = false;
    bool clipValid = false;
    bool lightValid = false;
    uint32_t lastSkinPalette = 0u;
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
    }
};

struct ShaderProg
{
    GLuint id = 0;
    GLint uClip = -1, uBones = -1, uDirections = -1, uColours = -1, uMisc = -1;
    GLint uFogA = -1, uFogB = -1, uFogC = -1, uFogCol = -1, uGuard = -1, uAlpha = -1;
    GLint uTex = -1, uBackground = -1;
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
    out.uTex = glGetUniformLocation(p, "uTex");
    out.uBackground = glGetUniformLocation(p, "uBackground");

    glUseProgram(p);
    if (out.uTex >= 0) glUniform1i(out.uTex, 0);
    if (out.uBackground >= 0) glUniform1i(out.uBackground, 1);
    out.shadow.reset();
}

bool linkProgCached(ShaderProg &out, const char *name, const char *vsSrc, const char *fsSrc,
                    const std::string &cacheDir, bool useCache)
{
#if defined(PS2X_MALI_GLES)
    std::string binPath;
    if (useCache && !cacheDir.empty())
    {
        binPath = cacheDir + "/" + name + "_v7.bin";
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
    GlContextGuard()
    {
#if defined(PS2X_LINUX_SDL2_SHIM)
        ps2xLinuxGlLock();
#endif
    }
    ~GlContextGuard()
    {
#if defined(PS2X_LINUX_SDL2_SHIM)
        ps2xLinuxGlUnlock();
#endif
    }
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
    uint8_t wrapClamp = 0xFF; // Phase 5.1: cached GL_TEXTURE_WRAP_S/T state
    bool binaryAlpha = false; // Phase 6B: true when every level-0 texel is <=4 or >=124
    bool astc = false;
    bool tex16 = false;
    uint64_t lastUsed = 0;
};

struct MaliTarget
{
    GLuint fbo = 0;
    GLuint color = 0;
    GLuint depth = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hasDepth = true;
};

struct SkinPaletteEntry
{
    uint32_t offsetFloats = 0;
    uint32_t qwordCount = 0;
};

// Phase 6C: Cached merged static run (strictly GL_UNSIGNED_INT + 28B CompactVertex).
struct MergeRun
{
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    uint32_t indexCount = 0;
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
    DrawState state{};
    float minW = 0.0f;
    uint32_t seq = 0;
    uint32_t psoKey = 0;
    uint8_t bucket = 0;
    bool skinned = false;
};

struct FramePacket
{
    uint64_t frameNum = 0;
    uint32_t width = 0, height = 0;
    bool hold = false;
    Fog fog{};
    Bloom bloom{};
    std::vector<UploadCmd> uploads;
    std::vector<DynMeshRec> dynMeshes;
    std::vector<Vertex> dynVerts;
    std::vector<SkinPaletteEntry> skinPalettes;
    std::vector<float> skinPaletteData;
    std::vector<RawDraw> draws;

    void clearPerFrame()
    {
        // Note: uploads are intentionally preserved across beginFrame() so asyncBuilds().takeUp()
        // uploads queued before beginFrame() are never dropped!
        dynMeshes.clear();
        dynVerts.clear();
        skinPalettes.clear();
        skinPaletteData.clear();
        draws.clear();
        hold = false;
    }
};

inline uint32_t makePsoKey(uint32_t prog, Blend blend, bool depthTest, bool depthWrite, uint32_t depthFunc)
{
    return (prog & 7u)
         | ((static_cast<uint32_t>(blend) & 3u) << 3)
         | ((depthTest ? 1u : 0u) << 5)
         | ((depthWrite ? 1u : 0u) << 6)
         | ((depthFunc & 3u) << 7);
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
           (a.bucket == 0u || a.bucket == 1u) &&
           a.mesh && b.mesh &&
           !a.mesh->dynamic && !b.mesh->dynamic &&
           !a.skinned && !b.skinned &&
           !a.mesh->srcVerts.empty() && !b.mesh->srcVerts.empty() &&
           a.state.order == b.state.order &&
           a.psoKey == b.psoKey &&
           a.state.texture == b.state.texture &&
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
           (!a.state.lit || std::memcmp(a.state.light, b.state.light, sizeof(a.state.light)) == 0);
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
        m_drsEnabled = envBool("SOTC_MALI_DRS", true);
        m_shaderCacheEnabled = envBool("SOTC_MALI_SHADER_CACHE", true);
        m_gpuSkinEnabled = envBool("SOTC_MALI_GPU_SKIN", true);
        m_mergeEnabled = envBool("SOTC_MALI_MERGE", true);
        m_mergeBudgetUs = static_cast<uint64_t>(std::max(0, envInt("SOTC_MALI_MERGE_BUDGET_US", 1500)));
        m_atPass2Mode = std::clamp(envInt("SOTC_MALI_AT_PASS2", 1), 0, 2); // 0=off, 1=smart auto, 2=always
        m_zPrepassEnabled = envBool("SOTC_MALI_ZPREPASS", false);
        m_postEnabled = envBool("SOTC_MALI_POST", true);
        m_postBloomEnabled = envBool("SOTC_MALI_POST_BLOOM", false);
        m_postUpscaleEnabled = envBool("SOTC_MALI_POST_UPSCALE", false);
        m_dumpTexEnabled = envBool("SOTC_MALI_DUMP_TEX", false);
        m_paceEnabled = envBool("SOTC_MALI_PACE", false);
        m_statsEnabled = envBool("SOTC_MALI_STATS", false);
        m_uploadBudgetUs = static_cast<uint64_t>(std::max(0, envInt("SOTC_MALI_UPLOAD_BUDGET_US", 1500)));
        m_bgRate = static_cast<uint32_t>(std::max(1, envInt("SOTC_MALI_BG_RATE", 2)));
        m_targetFps = std::max(20, envInt("SOTC_FPS_CAP", 30));

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
    void addScreenBlur(const ScreenBlur &) override {}
    void setGlowComposite(const GlowComposite &) override {}
    void setDepthOfField(const DepthOfField &) override {}
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
            return;
        }

        // Phase 5.1 mailbox handover: never stalls game thread and never drops static uploads!
        std::unique_lock<std::mutex> lk(m_mtx);
        if (!m_mailboxEnabled)
            m_cvDone.wait(lk, [this] { return !m_hasPendingFrame || m_stopRequested; });
        if (m_stopRequested)
            return;
        if (m_hasPendingFrame)
        {
            // Merge unconsumed uploads from m_pending ahead of m_rec.uploads before dropping old frame.
            if (!m_pending.uploads.empty())
            {
                m_pending.uploads.insert(m_pending.uploads.end(),
                                         std::make_move_iterator(m_rec.uploads.begin()),
                                         std::make_move_iterator(m_rec.uploads.end()));
                m_rec.uploads.swap(m_pending.uploads);
                m_pending.uploads.clear();
            }
            m_droppedFrames.fetch_add(1u, std::memory_order_relaxed);
        }
        std::swap(m_pending, m_rec);
        m_rec.uploads.clear();
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
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(2, &cpuset);
        CPU_SET(3, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
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

        FramePacket local; // Reused across frames to avoid vector reallocations
        for (;;)
        {
            bool doFrame = false;
            bool doReadback = false;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cvWork.wait(lk, [this] { return m_hasPendingFrame || m_readbackReq || m_stopRequested; });
                if (m_stopRequested && !m_hasPendingFrame)
                    break;
                if (m_hasPendingFrame)
                {
                    local.uploads.clear();
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
                executeFrame(local);
                local.uploads.clear();
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
                readbackOnGlThread(tmp, rw, rh);
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
            if (ext && std::strcmp(ext, "GL_KHR_texture_compression_astc_ldr") == 0)
            {
                m_hasAstc = true;
                break;
            }
        }

        if (!linkProgCached(m_progOpaque, "opaque", kMaliWorldVert, kMaliOpaqueFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progCut, "cut", kMaliWorldVert, kMaliCutFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progScreen, "screen", kMaliScreenVert, kMaliScreenFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progSkinOpaque, "skin_opaque", kMaliSkinVert, kMaliOpaqueFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progSkinCut, "skin_cut", kMaliSkinVert, kMaliCutFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progDepth, "depth", kMaliDepthVert, kMaliDepthFrag, m_shaderCacheDir, m_shaderCacheEnabled) ||
            !linkProgCached(m_progPost, "post", kMaliPostVert, kMaliPostFrag, m_shaderCacheDir, m_shaderCacheEnabled))
        {
            return false;
        }

        glGenVertexArrays(1, &m_emptyVao);

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
        std::fprintf(stderr, "[mali] Phase 6 GLES 3.2 backend up (async=%d mailbox=%d merge=%d atPass2=%d cull=%d skin=%d astcHw=%d drs=%d)\n",
                     m_asyncEnabled ? 1 : 0, m_mailboxEnabled ? 1 : 0, m_mergeEnabled ? 1 : 0, m_atPass2Mode,
                     m_cullEnabled ? 1 : 0, m_gpuSkinEnabled ? 1 : 0, m_hasAstc ? 1 : 0, m_drsEnabled ? 1 : 0);
        return true;
    }

    void shutdownGlOnCurrentThread()
    {
        if (!m_initialized)
            return;
        GlContextGuard guard;
        glDeleteVertexArrays(3, m_arenaVao);
        glDeleteBuffers(3, m_arenaVbo);
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
        for (auto &[id, t] : m_textures)
            if (t.tex) glDeleteTextures(1, &t.tex);
        m_textures.clear();
        destroyTarget(m_mainTarget);
        destroyTarget(m_postTarget);
        destroyTarget(m_bgTarget);
        if (m_whiteTex) { glDeleteTextures(1, &m_whiteTex); m_whiteTex = 0; }
        auto delProg = [](ShaderProg &p) { if (p.id) { glDeleteProgram(p.id); p.id = 0; } };
        delProg(m_progOpaque);
        delProg(m_progCut);
        delProg(m_progScreen);
        delProg(m_progSkinOpaque);
        delProg(m_progSkinCut);
        delProg(m_progDepth);
        delProg(m_progPost);
        m_initialized = false;
    }

    bool readbackOnGlThread(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        const MaliTarget &src = (m_postTarget.fbo && m_lastPresentedPost) ? m_postTarget : m_mainTarget;
        if (!m_initialized || !src.fbo)
            return false;
        GlContextGuard guard;
        width = src.width;
        height = src.height;
        rgba.resize(static_cast<size_t>(width) * height * 4u);
        glBindFramebuffer(GL_FRAMEBUFFER, src.fbo);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void processUploads(const FramePacket &pkt)
    {
        m_uploadUsThisFrame = 0;
        for (const UploadCmd &cmd : pkt.uploads)
        {
            if (cmd.kind == UploadCmd::StaticMesh || cmd.kind == UploadCmd::IndexedMesh)
            {
                MaliMesh &mesh = m_meshes[cmd.id];
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
                    if (m_mergeSrcBytes + needBytes <= kMaxMergeSrcBytes)
                    {
                        mesh.srcVerts = cmd.compactVerts;
                        if (mesh.indexed)
                        {
                            mesh.srcIndices = cmd.indices;
                        }
                        else
                        {
                            mesh.srcIndices.resize(cmd.compactVerts.size());
                            for (uint32_t i = 0; i < cmd.compactVerts.size(); ++i)
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
                mesh.lastUsed = pkt.frameNum;
                mesh.dynamic = false;
                mesh.skinned = true;
                mesh.indexed = !cmd.indices.empty();
                mesh.vertexCount = static_cast<uint32_t>(cmd.skinVerts.size());
                mesh.indexCount = static_cast<uint32_t>(cmd.indices.size());
                mesh.hasBox = false;
                mesh.hasIntermediateAlpha = cmd.hasIntermediateAlpha;
                mesh.minAlpha = cmd.minAlpha;
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
        }

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

    void uploadTextureOnGl(const UploadCmd &cmd, uint64_t frameNum)
    {
        if (m_dumpTexEnabled && m_dumpedTexIds.insert(cmd.id).second)
        {
            char fname[96];
            std::snprintf(fname, sizeof(fname), "%s/%016llx_%ux%u.rgba",
                          m_texDumpDir.c_str(), static_cast<unsigned long long>(cmd.id), cmd.width, cmd.height);
            if (FILE *fp = std::fopen(fname, "wb"))
            {
                std::fwrite(cmd.rgba.data(), 1, static_cast<size_t>(cmd.width) * cmd.height * 4u, fp);
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
        if (!tx.tex)
            glGenTextures(1, &tx.tex);
        glBindTexture(GL_TEXTURE_2D, tx.tex);

        const size_t pxCount = static_cast<size_t>(cmd.width) * cmd.height;
        uint8_t minA = 0xFF;
        bool allBinaryAlpha = true;
        const uint8_t *rgba = cmd.rgba.data();
        for (size_t i = 0; i < pxCount; ++i)
        {
            const uint8_t a = rgba[i * 4 + 3];
            if (a < minA) minA = a;
            if (a > 4u && a < 124u) allBinaryAlpha = false;
        }
        tx.minAlpha = minA;
        tx.binaryAlpha = allBinaryAlpha;

        if (m_hasAstc && !m_astcCacheDir.empty() && tryUploadAstcFromCache(cmd.id, tx))
        {
            ++m_astcHits;
            if (m_uploadBudgetUs)
                m_uploadUsThisFrame += nowUs() - t0;
            return;
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, tx.levels > 1u ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        const bool use565 = m_tex16Enabled && minA >= 124u;
        const bool use5551 = m_tex16Enabled && !use565 && allBinaryAlpha;
        tx.tex16 = use565 || use5551;

        const uint8_t *ptr = rgba;
        uint32_t lw = cmd.width, lh = cmd.height;
        for (uint32_t lv = 0; lv < tx.levels; ++lv)
        {
            const size_t n = static_cast<size_t>(lw) * lh;
            if (use565)
            {
                m_tex16Scratch.resize(n);
                for (size_t i = 0; i < n; ++i)
                {
                    const uint16_t r = static_cast<uint16_t>(ptr[i * 4 + 0] >> 3);
                    const uint16_t g = static_cast<uint16_t>(ptr[i * 4 + 1] >> 2);
                    const uint16_t b = static_cast<uint16_t>(ptr[i * 4 + 2] >> 3);
                    m_tex16Scratch[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
                }
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB565,
                             static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                             GL_RGB, GL_UNSIGNED_SHORT_5_6_5, m_tex16Scratch.data());
            }
            else if (use5551)
            {
                m_tex16Scratch.resize(n);
                for (size_t i = 0; i < n; ++i)
                {
                    const uint16_t r = static_cast<uint16_t>(ptr[i * 4 + 0] >> 3);
                    const uint16_t g = static_cast<uint16_t>(ptr[i * 4 + 1] >> 3);
                    const uint16_t b = static_cast<uint16_t>(ptr[i * 4 + 2] >> 3);
                    const uint16_t a = ptr[i * 4 + 3] >= 64u ? 1u : 0u;
                    m_tex16Scratch[i] = static_cast<uint16_t>((r << 11) | (g << 6) | (b << 1) | a);
                }
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGB5_A1,
                             static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                             GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, m_tex16Scratch.data());
            }
            else
            {
                glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(lv), GL_RGBA8,
                             static_cast<GLsizei>(lw), static_cast<GLsizei>(lh), 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, ptr);
            }
            ptr += n * 4u;
            lw = std::max(1u, lw >> 1);
            lh = std::max(1u, lh >> 1);
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(tx.levels - 1u));
        if (m_uploadBudgetUs)
            m_uploadUsThisFrame += nowUs() - t0;
    }

    // Phase 6C: Build or fetch a cached merged static VBO/IBO (strictly GL_UNSIGNED_INT).
    MergeRun *getOrBuildMergeRun(size_t from, size_t to, uint64_t &mergeUsThisFrame)
    {
        uint64_t key = 14695981039346656037ull;
        uint32_t totalVerts = 0;
        uint32_t totalIndices = 0;
        for (size_t j = from; j < to; ++j)
        {
            const MaliMesh *m = m_qAll[j].mesh;
            key = fnv1aHash(&m_qAll[j].meshId, sizeof(uint64_t), key);
            totalVerts += static_cast<uint32_t>(m->srcVerts.size());
            totalIndices += static_cast<uint32_t>(m->srcIndices.size());
        }
        if (!totalVerts || !totalIndices)
            return nullptr;
        MergeRun &run = m_mergeRuns[key];
        run.lastUsed = m_frameNum;
        if (run.vao && run.indexCount == totalIndices)
            return &run;

        ++run.frames;
        if (run.frames < 2u)
            return nullptr;
        if (m_mergeBudgetUs && mergeUsThisFrame >= m_mergeBudgetUs)
            return nullptr;

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
                if (rr.vao && rr.lastUsed + 16u < m_frameNum)
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
                er.vao = er.vbo = er.ibo = 0;
                er.indexCount = 0;
                er.bytes = 0;
                er.frames = 0;
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

    void executeFrame(FramePacket &pkt)
    {
        const uint64_t frameStartUs = nowUs();
        GlContextGuard guard;

        m_frameNum = pkt.frameNum;
        m_fog = pkt.fog;
        m_bloom = pkt.bloom;
        m_culledThisFrame = 0;
        m_drawCallsThisFrame = 0;
        m_mergeSavedThisFrame = 0;
        m_pass2SkipThisFrame = 0;
        m_texWrapThisFrame = 0;
        uint64_t mergeUsThisFrame = 0;

        processUploads(pkt);

        if (!pkt.dynVerts.empty())
        {
            glBindBuffer(GL_ARRAY_BUFFER, m_arenaVbo[m_arenaSlot]);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pkt.dynVerts.size() * sizeof(Vertex)),
                         pkt.dynVerts.data(), GL_STREAM_DRAW);
        }

        // Build draw queues, pre-resolving MaliMesh* and MaliTex* once per draw.
        m_qBg.clear();
        m_qAll.clear();
        m_qAll.reserve(pkt.draws.size());
        for (const RawDraw &rd : pkt.draws)
        {
            auto it = m_meshes.find(rd.meshId);
            if (it == m_meshes.end())
                continue;
            MaliMesh &mesh = it->second;
            mesh.lastUsed = m_frameNum;

            float minW = 0.0f;
            if (m_cullEnabled && mesh.hasBox && !mesh.dynamic && !mesh.skinned && !rd.state.screen && !rd.state.overlay)
            {
                if (cullAabb(rd.clip, mesh.lo, mesh.hi, minW))
                {
                    ++m_culledThisFrame;
                    continue;
                }
            }

            MaliTex *texPtr = nullptr;
            if (rd.state.texture)
            {
                auto tit = m_textures.find(rd.state.texture);
                if (tit != m_textures.end() && tit->second.tex)
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
            q.minW = minW;
            q.seq = rd.seq;
            q.skinned = mesh.skinned && rd.state.skinPalette != 0u;

            if (rd.state.screen || rd.state.screenDepthTest)
            {
                q.bucket = 3u;
                q.psoKey = makePsoKey(2u, rd.state.blend, rd.state.screenDepthTest, false, 0u);
            }
            else
            {
                const bool hasAlphaTest = (rd.state.alphaTest & 1u) != 0u;
                const bool needsGuard = rd.state.background && m_fog.skyNear > 0.0f;
                const bool texOpaque = !texPtr || texPtr->minAlpha >= 124u;
                const bool isOpaque = !hasAlphaTest && !needsGuard && !rd.state.overlay && !rd.state.noDepthWrite &&
                                      (rd.state.blend == Blend::Opaque ||
                                       (rd.state.blend == Blend::Normal && rd.state.opacity >= 0.99f && mesh.minAlpha >= 124u && texOpaque));
                const uint32_t opProg = q.skinned ? 3u : 0u;
                const uint32_t cutProg = q.skinned ? 4u : 1u;
                if (isOpaque)
                {
                    q.bucket = 0u;
                    q.psoKey = makePsoKey(opProg, Blend::Opaque, true, true, 0u);
                }
                else if (hasAlphaTest && !rd.state.overlay)
                {
                    q.bucket = 1u;
                    q.psoKey = makePsoKey(cutProg, rd.state.blend, true, !rd.state.noDepthWrite, 0u);
                }
                else
                {
                    q.bucket = 2u;
                    const uint32_t df = rd.state.overlay ? 2u : 0u;
                    const bool dw = rd.state.writeDepth && !rd.state.noDepthWrite && !rd.state.overlay;
                    q.psoKey = makePsoKey(cutProg, rd.state.blend, true, dw, df);
                }
            }

            if (rd.state.background && m_fog.enabled)
                m_qBg.push_back(q);
            m_qAll.push_back(q);
        }

        // Sort per-layer; within Opaque and Alpha-Test buckets, group by state+texture+clipHash
        // so Phase 6C merges contiguous static chunks cleanly while keeping Front-to-Back order.
        std::sort(m_qAll.begin(), m_qAll.end(), [](const QueuedDraw &a, const QueuedDraw &b)
        {
            if (a.state.order != b.state.order)
                return a.state.order < b.state.order;
            if (a.bucket != b.bucket)
                return a.bucket < b.bucket;
            if (a.bucket == 0u || a.bucket == 1u)
            {
                if (a.psoKey != b.psoKey) return a.psoKey < b.psoKey;
                if (a.state.texture != b.state.texture) return a.state.texture < b.state.texture;
                if (a.state.clamp != b.state.clamp) return a.state.clamp < b.state.clamp;
                if (a.clipHash != b.clipHash) return a.clipHash < b.clipHash;
                if (a.state.alphaTest != b.state.alphaTest) return a.state.alphaTest < b.state.alphaTest;
                if (a.meshId != b.meshId) return a.meshId < b.meshId;
            }
            return a.seq < b.seq;
        });

        const float scale = m_drsEnabled ? m_drsScale : 1.0f;
        const uint32_t rw = std::max(320u, static_cast<uint32_t>(std::lround(pkt.width * scale)) & ~3u);
        const uint32_t rh = std::max(240u, static_cast<uint32_t>(std::lround(pkt.height * scale)) & ~3u);
        if (m_mainTarget.width != rw || m_mainTarget.height != rh)
            makeTarget(m_mainTarget, rw, rh, true);

        // Phase 5.1 fix preserved: skip m_postTarget FBO unless bloom or explicit post-upscale is active.
        const bool wantBloom = m_postEnabled && m_postBloomEnabled && m_bloom.enabled;
        const bool runPost = wantBloom || (m_postEnabled && m_postUpscaleEnabled && (rw != pkt.width || rh != pkt.height));
        if (runPost && (m_postTarget.width != pkt.width || m_postTarget.height != pkt.height))
            makeTarget(m_postTarget, pkt.width, pkt.height, false);

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

        glBindFramebuffer(GL_FRAMEBUFFER, m_mainTarget.fbo);
        glViewport(0, 0, static_cast<GLsizei>(m_mainTarget.width), static_cast<GLsizei>(m_mainTarget.height));
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
                const uint32_t tris = q.mesh->indexed ? q.mesh->indexCount : q.mesh->vertexCount;
                if (tris < 300u)
                    continue;
                QueuedDraw zq = q;
                zq.psoKey = zPso;
                emitDrawSingle(zq, pkt, false, 0.0f, 0.0f);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }

        for (size_t i = 0; i < m_qAll.size();)
        {
            const QueuedDraw &q = m_qAll[i];

            // Phase 6C: Try safe GL_UNSIGNED_INT static merge for contiguous compatible draws.
            if (m_mergeEnabled && (q.bucket == 0u || q.bucket == 1u) && q.mesh && !q.mesh->dynamic && !q.skinned && !q.mesh->srcVerts.empty())
            {
                size_t end = i + 1u;
                while (end < m_qAll.size() && canMergeStatic(q, m_qAll[end]))
                    ++end;
                if (end - i >= 2u)
                {
                    if (MergeRun *run = getOrBuildMergeRun(i, end, mergeUsThisFrame))
                    {
                        if (q.bucket == 0u)
                        {
                            emitMergedRun(q, *run, pkt, true, 0.0f, 0.0f);
                        }
                        else
                        {
                            const float aref = static_cast<float>((q.state.alphaTest >> 4) & 0xFFu) / 128.0f;
                            emitMergedRun(q, *run, pkt, true, std::max(0.02f, aref), 0.0f);
                            bool anyNeedPass2 = false;
                            for (size_t j = i; j < end; ++j)
                                if (needsAlphaTestPass2(m_qAll[j], aref)) { anyNeedPass2 = true; break; }
                            if (anyNeedPass2)
                            {
                                QueuedDraw p2 = q;
                                p2.psoKey = makePsoKey(1u, q.state.blend, true, false, 1u);
                                emitMergedRun(p2, *run, pkt, true, 0.02f, aref);
                            }
                            else if (((q.state.alphaTest >> 12) & 3u) == 1u || ((q.state.alphaTest >> 12) & 3u) == 3u)
                            {
                                m_pass2SkipThisFrame += static_cast<uint32_t>(end - i);
                            }
                        }
                        m_mergeSavedThisFrame += static_cast<uint32_t>(end - i - 1u);
                        i = end;
                        continue;
                    }
                }
            }

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
                    pass2.psoKey = makePsoKey(q.skinned ? 4u : 1u, q.state.blend, true, false, 1u);
                    emitDrawSingle(pass2, pkt, true, 0.02f, aref);
                }
                else if (((q.state.alphaTest >> 12) & 3u) == 1u || ((q.state.alphaTest >> 12) & 3u) == 3u)
                {
                    ++m_pass2SkipThisFrame;
                }
            }
            else if (q.bucket == 2u)
            {
                emitDrawSingle(q, pkt, true, 0.02f, 0.0f);
            }
            else
            {
                emitDrawSingle(q, pkt, false, 0.01f, 0.0f);
            }
            ++i;
        }

        invalidateDepth();

        GLuint outTex = m_mainTarget.color;
        uint32_t outW = m_mainTarget.width;
        uint32_t outH = m_mainTarget.height;
        m_lastPresentedPost = false;
        if (runPost && m_postTarget.fbo)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_postTarget.fbo);
            glViewport(0, 0, static_cast<GLsizei>(m_postTarget.width), static_cast<GLsizei>(m_postTarget.height));
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glUseProgram(m_progPost.id);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_mainTarget.color);
            const float bloomAmt = wantBloom ? 0.25f : 0.0f;
            const float bloomThr = m_bloom.threshold > 0.05f ? std::min(m_bloom.threshold, 0.9f) : 0.65f;
            if (m_progPost.uMisc >= 0)
                glUniform4f(m_progPost.uMisc, 1.0f / static_cast<float>(m_mainTarget.width),
                            1.0f / static_cast<float>(m_mainTarget.height), bloomAmt, bloomThr);
            glBindVertexArray(m_emptyVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            outTex = m_postTarget.color;
            outW = m_postTarget.width;
            outH = m_postTarget.height;
            m_lastPresentedPost = true;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_arenaSlot = (m_arenaSlot + 1) % 3;

        if (m_paceEnabled && !pkt.hold)
        {
            const auto period = std::chrono::microseconds(1000000 / m_targetFps);
            const auto target = m_lastPresentTime + period;
            const auto now = std::chrono::steady_clock::now();
            if (target > now + std::chrono::microseconds(600))
                std::this_thread::sleep_until(target - std::chrono::microseconds(400));
            m_lastPresentTime = std::chrono::steady_clock::now();
        }

        if (!pkt.hold)
        {
            if (const auto &hook = presentHook())
                hook(outTex, outW, outH);
        }

        const double frameMs = static_cast<double>(nowUs() - frameStartUs) * 0.001;
        m_emaFrameMs = m_emaFrameMs > 0.0 ? (m_emaFrameMs * 0.85 + frameMs * 0.15) : frameMs;
        if (m_drsEnabled && (m_frameNum % 12u == 0u))
        {
            const double budgetMs = 1000.0 / static_cast<double>(m_targetFps);
            if (m_emaFrameMs > budgetMs * 0.90)
            {
                ++m_drsOverStreak;
                m_drsUnderStreak = 0;
                if (m_drsOverStreak >= 2 && m_drsScale > 0.75f)
                {
                    m_drsScale = std::max(0.75f, m_drsScale - 0.05f);
                    m_drsOverStreak = 0;
                }
            }
            else if (m_emaFrameMs < budgetMs * 0.70)
            {
                ++m_drsUnderStreak;
                m_drsOverStreak = 0;
                if (m_drsUnderStreak >= 4 && m_drsScale < 1.0f)
                {
                    m_drsScale = std::min(1.0f, m_drsScale + 0.05f);
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
            std::fprintf(stderr, "[mali:stats] frame=%llu draws=%u mergeSaved=%u pass2Skip=%u culled=%u texWrap=%u drop=%llu drs=%.2f renderMs=%.2f\n",
                         static_cast<unsigned long long>(m_frameNum), m_drawCallsThisFrame,
                         m_mergeSavedThisFrame, m_pass2SkipThisFrame, m_culledThisFrame,
                         m_texWrapThisFrame, static_cast<unsigned long long>(m_droppedFrames.load(std::memory_order_relaxed)),
                         m_drsScale, m_emaFrameMs);
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

    bool tryUploadAstcFromCache(uint64_t id, MaliTex &tx)
    {
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%016llx.astc", static_cast<unsigned long long>(id));
        const std::string path = m_astcCacheDir + "/" + hex;
        FILE *fp = std::fopen(path.c_str(), "rb");
        if (!fp)
            return false;

        uint8_t hdr[16] = {};
        if (std::fread(hdr, 1, 16, fp) != 16 ||
            hdr[0] != 0x13u || hdr[1] != 0xABu || hdr[2] != 0xA1u || hdr[3] != 0x5Cu)
        {
            std::fclose(fp);
            return false;
        }
        const uint8_t bx = hdr[4], by = hdr[5];
        const GLenum fmt = astcBlockToFormat(bx, by);
        const uint32_t w = static_cast<uint32_t>(hdr[7]) | (static_cast<uint32_t>(hdr[8]) << 8) | (static_cast<uint32_t>(hdr[9]) << 16);
        const uint32_t h = static_cast<uint32_t>(hdr[10]) | (static_cast<uint32_t>(hdr[11]) << 8) | (static_cast<uint32_t>(hdr[12]) << 16);
        if (!fmt || !w || !h)
        {
            std::fclose(fp);
            return false;
        }
        const uint32_t blocksX = (w + bx - 1u) / bx;
        const uint32_t blocksY = (h + by - 1u) / by;
        const size_t dataBytes = static_cast<size_t>(blocksX) * blocksY * 16u;
        m_astcScratch.resize(dataBytes);
        if (std::fread(m_astcScratch.data(), 1, dataBytes, fp) != dataBytes)
        {
            std::fclose(fp);
            return false;
        }
        std::fclose(fp);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, fmt, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                               static_cast<GLsizei>(dataBytes), m_astcScratch.data());
        tx.width = w;
        tx.height = h;
        tx.levels = 1;
        tx.astc = true;
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

    static void makeTarget(MaliTarget &t, uint32_t w, uint32_t h, bool withDepth)
    {
        destroyTarget(t);
        t.width = w;
        t.height = h;
        t.hasDepth = withDepth;
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
            glGenRenderbuffers(1, &t.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    static void destroyTarget(MaliTarget &t)
    {
        if (t.fbo) { glDeleteFramebuffers(1, &t.fbo); t.fbo = 0; }
        if (t.color) { glDeleteTextures(1, &t.color); t.color = 0; }
        if (t.depth) { glDeleteRenderbuffers(1, &t.depth); t.depth = 0; }
        t.width = t.height = 0;
    }

    static void invalidateDepth()
    {
#if defined(PS2X_MALI_GLES)
        const GLenum attachments[2] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        glInvalidateFramebuffer(GL_FRAMEBUFFER, 2, attachments);
#endif
    }

    void resetPsoAndUniformCaches()
    {
        m_lastPso = ~0u;
        m_lastTex = ~0u;
        m_lastVao = ~0u;
        m_progOpaque.shadow.reset();
        m_progCut.shadow.reset();
        m_progScreen.shadow.reset();
        m_progSkinOpaque.shadow.reset();
        m_progSkinCut.shadow.reset();
        m_progDepth.shadow.reset();
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
        default: return m_progDepth;
        }
    }

    const ShaderProg &applyPso(uint32_t psoKey)
    {
        const uint32_t progIdx = psoKey & 7u;
        const ShaderProg &prog = progForIdx(progIdx);
        if (psoKey == m_lastPso)
            return prog;

        if ((m_lastPso == ~0u) || ((m_lastPso & 7u) != progIdx))
            glUseProgram(prog.id);

        const Blend blend = static_cast<Blend>((psoKey >> 3) & 3u);
        if ((m_lastPso == ~0u) || (((m_lastPso >> 3) & 3u) != static_cast<uint32_t>(blend)))
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

        const bool depthTest = ((psoKey >> 5) & 1u) != 0u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 5) & 1u) != (depthTest ? 1u : 0u)))
        {
            if (depthTest) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
        }

        const bool depthWrite = ((psoKey >> 6) & 1u) != 0u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 6) & 1u) != (depthWrite ? 1u : 0u)))
            glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);

        const uint32_t df = (psoKey >> 7) & 3u;
        if ((m_lastPso == ~0u) || (((m_lastPso >> 7) & 3u) != df))
        {
            const GLenum fn = (df == 1u) ? GL_LESS : (df == 2u ? GL_EQUAL : (df == 3u ? GL_ALWAYS : GL_LEQUAL));
            glDepthFunc(fn);
        }

        m_lastPso = psoKey;
        return prog;
    }

    void bindCommonUniforms(const ShaderProg &prog, const QueuedDraw &q, const FramePacket &pkt,
                            bool allowBgFog, float minAlpha, float maxAlpha)
    {
        // Phase 5.1: Pre-resolved MaliTex* + per-texture wrapClamp cache (eliminates glTexParameteri storm).
        const GLuint glTex = q.tex ? q.tex->tex : m_whiteTex;
        const bool hasTex = (q.tex != nullptr);
        if (glTex != m_lastTex)
        {
            glBindTexture(GL_TEXTURE_2D, glTex);
            m_lastTex = glTex;
        }
        if (q.tex && q.tex->wrapClamp != q.state.clamp)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (q.state.clamp & 1u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (q.state.clamp & 2u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            q.tex->wrapClamp = q.state.clamp;
            ++m_texWrapThisFrame;
        }

        UniformShadow &sh = prog.shadow;
        if (prog.uClip >= 0 && (!sh.clipValid || std::memcmp(sh.clip, q.clip, sizeof(sh.clip)) != 0))
        {
            std::memcpy(sh.clip, q.clip, sizeof(sh.clip));
            sh.clipValid = true;
            glUniform4fv(prog.uClip, 4, q.clip);
        }

        if (q.skinned && prog.uBones >= 0 && q.state.skinPalette &&
            q.state.skinPalette != sh.lastSkinPalette && q.state.skinPalette <= pkt.skinPalettes.size())
        {
            const SkinPaletteEntry &pe = pkt.skinPalettes[q.state.skinPalette - 1u];
            const uint32_t qwords = std::min(pe.qwordCount, kMaxSkinQwords);
            if (qwords > 0u && static_cast<size_t>(pe.offsetFloats) + static_cast<size_t>(qwords) * 4u <= pkt.skinPaletteData.size())
            {
                glUniform4fv(prog.uBones, static_cast<GLsizei>(qwords), pkt.skinPaletteData.data() + pe.offsetFloats);
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
        setCached4f(prog.uFogCol, sh.fogCol, v, m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], hasTex ? 1.0f : 0.0f);

        const bool bgFog = allowBgFog && m_fog.enabled && q.state.fogged && !q.state.background;
        setCached4f(prog.uFogA, sh.fogA, v, m_fog.start, m_fog.width, static_cast<float>(m_fog.steps), bgFog ? 1.0f : 0.0f);

        const bool useGuard = q.state.background && m_fog.skyNear > 0.0f;
        setCached4f(prog.uGuard, sh.guard, v, useGuard ? 1.0f : 0.0f, m_fog.skyNear, q.state.screenDepthTest ? 1.0f : 0.0f, 0.0f);
        setCached4f(prog.uAlpha, sh.alpha, v, minAlpha, maxAlpha, 0.0f, 0.0f);
        sh.valid = true;
    }

    void emitDrawSingle(const QueuedDraw &q, const FramePacket &pkt,
                        bool allowBgFog, float minAlpha, float maxAlpha)
    {
        if (!q.mesh)
            return;
        const MaliMesh &mesh = *q.mesh;

        const ShaderProg &prog = applyPso(q.psoKey);
        bindCommonUniforms(prog, q, pkt, allowBgFog, minAlpha, maxAlpha);

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
        const ShaderProg &prog = applyPso(q.psoKey);
        bindCommonUniforms(prog, q, pkt, allowBgFog, minAlpha, maxAlpha);
        if (run.vao != m_lastVao)
        {
            glBindVertexArray(run.vao);
            m_lastVao = run.vao;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(run.indexCount), GL_UNSIGNED_INT, nullptr);
        ++m_drawCallsThisFrame;
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
        for (auto it = m_mergeRuns.begin(); it != m_mergeRuns.end();)
        {
            if (it->second.lastUsed + 120u < m_frameNum)
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
    }

    std::function<void()> m_makeCurrent;
    bool m_initialized = false;
    bool m_asyncEnabled = true;
    bool m_mailboxEnabled = true;
    bool m_hasAstc = false;
    bool m_cullEnabled = true;
    bool m_tex16Enabled = true;
    bool m_drsEnabled = true;
    bool m_shaderCacheEnabled = true;
    bool m_gpuSkinEnabled = true;
    bool m_mergeEnabled = true;
    int m_atPass2Mode = 1;
    bool m_zPrepassEnabled = false;
    bool m_postEnabled = true;
    bool m_postBloomEnabled = false;
    bool m_postUpscaleEnabled = false;
    bool m_lastPresentedPost = false;
    bool m_dumpTexEnabled = false;
    bool m_paceEnabled = false;
    bool m_statsEnabled = false;
    uint64_t m_uploadBudgetUs = 1500;
    uint64_t m_uploadUsThisFrame = 0;
    uint64_t m_mergeBudgetUs = 1500;
    uint32_t m_bgRate = 2;
    int m_targetFps = 30;
    float m_drsScale = 1.0f;
    double m_emaFrameMs = 0.0;
    int m_drsOverStreak = 0;
    int m_drsUnderStreak = 0;
    std::chrono::steady_clock::time_point m_lastPresentTime{};

    std::string m_astcCacheDir;
    std::string m_texDumpDir;
    std::string m_shaderCacheDir;

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
    uint32_t m_pass2SkipThisFrame = 0;
    uint32_t m_texWrapThisFrame = 0;
    uint32_t m_astcHits = 0;
    uint64_t m_mergeSrcBytes = 0;
    uint64_t m_mergeRunBytes = 0;

    ShaderProg m_progOpaque{}, m_progCut{}, m_progScreen{};
    ShaderProg m_progSkinOpaque{}, m_progSkinCut{}, m_progDepth{}, m_progPost{};
    GLuint m_emptyVao = 0;
    GLuint m_whiteTex = 0;
    MaliTarget m_mainTarget{}, m_postTarget{}, m_bgTarget{};

    GLuint m_arenaVao[3] = {};
    GLuint m_arenaVbo[3] = {};
    int m_arenaSlot = 0;

    uint32_t m_lastPso = ~0u;
    GLuint m_lastTex = ~0u;
    GLuint m_lastVao = ~0u;

    Fog m_fog{};
    Bloom m_bloom{};
    std::vector<uint16_t> m_tex16Scratch;
    std::vector<uint8_t> m_astcScratch;
    std::vector<CompactVertex> m_mergeVertsScratch;
    std::vector<uint32_t> m_mergeIndicesScratch;
    std::vector<std::pair<uint64_t, uint64_t>> m_evictRunsScratch;
    std::vector<QueuedDraw> m_qBg, m_qAll;
    std::unordered_set<uint64_t> m_dumpedTexIds;
    std::unordered_map<uint64_t, MaliMesh> m_meshes;
    std::unordered_map<uint64_t, MaliTex> m_textures;
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