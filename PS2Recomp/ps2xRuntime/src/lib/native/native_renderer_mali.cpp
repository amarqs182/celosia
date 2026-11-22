// Dedicated Mali-G31 (GLES 3.2 / GL 3.3 Core) native renderer backend for Celosia R36S.
// Plugs directly into native_renderer::detail::Backend (selected via SOTC_RENDERER=mali).
//
// Phase 3:
// - Phase 1+2 base: exact per-layer (state.order) rendering with intra-layer HSR
//   (Opaque Front-to-Back -> Alpha-Test 2-pass -> Transparent/Overlay -> Screen),
//   compact 28-byte static vertices, ASTC hardware cache loader, RGB565/RGB5_A1,
//   disk shader binary cache, glFenceSync ring buffers, TBDR invalidate, and DRS.
// - Phase 3A: GPU Skinning via UBO palette (canSkin(), uploadSkinnedMesh(), framePalette()).
// - Phase 3B: Hardware Instancing via UBO (up to 64 instances/call, glDraw*Instanced).
// - Phase 3C: Selective Z-Prepass for large opaque geometry (opt-in SOTC_MALI_ZPREPASS=1).
// - Phase 3D: Built-in per-120-frame telemetry (SOTC_MALI_STATS=1).

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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
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

constexpr uint32_t kMaxInstances = 64u;
constexpr uint32_t kMaxSkinQwords = 256u; // 64 bones * 4 vec4s (4 KiB UBO)

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

// World vertex shader with UBO instancing (uInstanceCount > 1 reads InstanceBlock).
static const char *kMaliWorldVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;

layout(std140) uniform InstanceBlock {
    highp vec4 uInstClips[256];
};

uniform vec4 uClip[4];
uniform int uInstanceCount;
uniform vec4 uDirections[4];
uniform vec4 uColours[4];
uniform vec4 uMisc;   // x: lit (>0.5 lit, <-0.5 fresnel), y: opacity, zw: uvOffset
uniform vec4 uFogB;   // z: vertexFog (>0.5), w: base
uniform vec4 uFogC;   // x: scale, y: lodK, z: lodScale, w: maxLod
uniform vec4 uGuard;  // x > 0: near plane cut at w < y

out highp vec4 vUV;
out mediump vec4 vColor;
out highp vec4 vScreen;
out highp float vLogW;

void main()
{
    vec4 c;
    if (uInstanceCount > 1)
    {
        int base = gl_InstanceID * 4;
        c = uInstClips[base] * aPos.x + uInstClips[base + 1] * aPos.y + uInstClips[base + 2] * aPos.z + uInstClips[base + 3];
    }
    else
    {
        c = uClip[0] * aPos.x + uClip[1] * aPos.y + uClip[2] * aPos.z + uClip[3];
    }
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

// Phase 3A: GPU Skinning vertex shader reading bone qwords from SkinBlock UBO.
static const char *kMaliSkinVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec3 aBone;
layout(location = 5) in vec3 aWeight;

layout(std140) uniform SkinBlock {
    highp vec4 uBones[256];
};

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
            int b = clamp(int(aBone[i] + 0.5), 0, 252);
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

// Phase 3C: Minimal depth-only Z-prepass shader (zero fragment work).
static const char *kMaliDepthVert = MALI_VERT_HDR R"(
layout(location = 0) in vec3 aPos;
layout(std140) uniform InstanceBlock {
    highp vec4 uInstClips[256];
};
uniform vec4 uClip[4];
uniform int uInstanceCount;
void main()
{
    vec4 c;
    if (uInstanceCount > 1)
    {
        int base = gl_InstanceID * 4;
        c = uInstClips[base] * aPos.x + uInstClips[base + 1] * aPos.y + uInstClips[base + 2] * aPos.z + uInstClips[base + 3];
    }
    else
    {
        c = uClip[0] * aPos.x + uClip[1] * aPos.y + uClip[2] * aPos.z + uClip[3];
    }
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

struct ShaderProg
{
    GLuint id = 0;
    GLint uClip = -1, uInstanceCount = -1, uDirections = -1, uColours = -1, uMisc = -1;
    GLint uFogA = -1, uFogB = -1, uFogC = -1, uFogCol = -1, uGuard = -1, uAlpha = -1;
    GLint uTex = -1, uBackground = -1;
};

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
    out.uInstanceCount = glGetUniformLocation(p, "uInstanceCount");
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

    const GLuint instIdx = glGetUniformBlockIndex(p, "InstanceBlock");
    if (instIdx != GL_INVALID_INDEX)
        glUniformBlockBinding(p, instIdx, 0u);
    const GLuint skinIdx = glGetUniformBlockIndex(p, "SkinBlock");
    if (skinIdx != GL_INVALID_INDEX)
        glUniformBlockBinding(p, skinIdx, 1u);

    glUseProgram(p);
    if (out.uTex >= 0) glUniform1i(out.uTex, 0);
    if (out.uBackground >= 0) glUniform1i(out.uBackground, 1);
    if (out.uInstanceCount >= 0) glUniform1i(out.uInstanceCount, 1);
}

bool linkProgCached(ShaderProg &out, const char *name, const char *vsSrc, const char *fsSrc,
                    const std::string &cacheDir, bool useCache)
{
#if defined(PS2X_MALI_GLES)
    std::string binPath;
    if (useCache && !cacheDir.empty())
    {
        binPath = cacheDir + "/" + name + "_v3.bin";
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
    float lo[3] = {};
    float hi[3] = {};
    uint8_t minAlpha = 0;
    uint64_t lastUsed = 0;
};

struct MaliTex
{
    GLuint tex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    uint8_t minAlpha = 0;
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
};

struct SkinPaletteEntry
{
    uint32_t offsetFloats = 0;
    uint32_t qwordCount = 0;
};

// Bucket within each state.order layer: 0=Opaque (Front-to-Back), 1=AlphaTest, 2=Transparent/Overlay, 3=Screen
struct QueuedDraw
{
    uint64_t meshId = 0;
    float clip[16] = {};
    DrawState state{};
    float minW = 0.0f;
    uint32_t seq = 0;
    uint32_t psoKey = 0;
    uint8_t bucket = 0;
    bool skinned = false;
};

// prog bits (0..2): 0=opaque, 1=cut, 2=screen, 3=skinOpaque, 4=skinCut, 5=depthOnly
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

inline bool canInstanceTogether(const QueuedDraw &a, const QueuedDraw &b)
{
    return a.meshId == b.meshId &&
           a.psoKey == b.psoKey &&
           a.bucket == 0u && b.bucket == 0u &&
           !a.skinned && !b.skinned &&
           a.state.order == b.state.order &&
           a.state.texture == b.state.texture &&
           a.state.clamp == b.state.clamp &&
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
        m_cullEnabled = envBool("SOTC_MALI_CULL", true);
        m_tex16Enabled = envBool("SOTC_MALI_TEX16", true);
        m_drsEnabled = envBool("SOTC_MALI_DRS", true);
        m_shaderCacheEnabled = envBool("SOTC_MALI_SHADER_CACHE", true);
        m_gpuSkinEnabled = envBool("SOTC_MALI_GPU_SKIN", true);
        m_instancingEnabled = envBool("SOTC_MALI_INSTANCING", true);
        m_zPrepassEnabled = envBool("SOTC_MALI_ZPREPASS", false);
        m_statsEnabled = envBool("SOTC_MALI_STATS", false);
        m_uploadBudgetUs = static_cast<uint64_t>(std::max(0, envInt("SOTC_MALI_UPLOAD_BUDGET_US", 1500)));
        m_bgRate = static_cast<uint32_t>(std::max(1, envInt("SOTC_MALI_BG_RATE", 2)));
        m_targetFps = std::max(20, envInt("SOTC_FPS_CAP", 30));

        if (const char *tc = std::getenv("SOTC_MALI_TEX_CACHE"); tc && tc[0])
            m_astcCacheDir = tc;
        else if (const char *mc = std::getenv("PS2X_MEMCARD_DIR"); mc && mc[0])
            m_astcCacheDir = std::string(mc) + "/../tex_cache";

        if (const char *sc = std::getenv("SOTC_MALI_SHADER_CACHE_DIR"); sc && sc[0])
            m_shaderCacheDir = sc;
        else
            m_shaderCacheDir = "/tmp/celosia_mali_shaders";
    }

    ~MaliBackend() override { shutdown(); }

    bool init()
    {
        GlContextGuard guard;
        if (m_makeCurrent)
            m_makeCurrent();

#if defined(__linux__)
        if (m_shaderCacheEnabled && !m_shaderCacheDir.empty())
            ::mkdir(m_shaderCacheDir.c_str(), 0755);
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
            !linkProgCached(m_progDepth, "depth", kMaliDepthVert, kMaliDepthFrag, m_shaderCacheDir, m_shaderCacheEnabled))
        {
            return false;
        }

        // UBO binding 0: InstanceBlock (64 * 16 floats = 4 KiB)
        glGenBuffers(1, &m_instUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_instUbo);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(kMaxInstances * 16u * sizeof(float)), nullptr, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0u, m_instUbo);

        // UBO binding 1: SkinBlock (256 * 4 floats = 4 KiB)
        glGenBuffers(1, &m_skinUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, m_skinUbo);
        glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(kMaxSkinQwords * 4u * sizeof(float)), nullptr, GL_STREAM_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, 1u, m_skinUbo);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);

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

        makeTarget(m_bgTarget, 256, 256);
        m_initialized = true;
        std::fprintf(stderr, "[mali] Phase 3 GLES 3.2 backend up (cull=%d inst=%d skin=%d zpre=%d tex16=%d astcHw=%d drs=%d)\n",
                     m_cullEnabled ? 1 : 0, m_instancingEnabled ? 1 : 0, m_gpuSkinEnabled ? 1 : 0,
                     m_zPrepassEnabled ? 1 : 0, m_tex16Enabled ? 1 : 0, m_hasAstc ? 1 : 0, m_drsEnabled ? 1 : 0);
        return true;
    }

    bool hasMesh(uint64_t id) override { return m_meshes.find(id) != m_meshes.end(); }
    uint64_t meshEvictions() override { return m_evictions; }
    bool canSkin() override { return m_gpuSkinEnabled; }

    void uploadMesh(uint64_t id, const Vertex *vertices, uint32_t count, bool perFrame) override
    {
        if (!vertices || !count)
            return;

        MaliMesh &mesh = m_meshes[id];
        mesh.lastUsed = m_frameNum;
        mesh.indexed = false;
        mesh.skinned = false;
        mesh.vertexCount = count;
        mesh.indexCount = 0;
        computeBoundsAndAlpha(vertices, count, mesh);

        if (perFrame)
        {
            mesh.dynamic = true;
            mesh.firstVertex = static_cast<uint32_t>(m_dynVerts.size());
            m_dynVerts.insert(m_dynVerts.end(), vertices, vertices + count);
            return;
        }

        mesh.dynamic = false;
        packCompactVertices(vertices, count, m_compactScratch);
        GlContextGuard guard;
        if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
        if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(CompactVertex)), m_compactScratch.data(), GL_STATIC_DRAW);
        setupCompactVertexAttribs();
        glBindVertexArray(0);
    }

    void uploadIndexedMesh(uint64_t id, const Vertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount) override
    {
        if (!vertices || !count || !indices || !indexCount)
            return;

        MaliMesh &mesh = m_meshes[id];
        mesh.lastUsed = m_frameNum;
        mesh.indexed = true;
        mesh.dynamic = false;
        mesh.skinned = false;
        mesh.vertexCount = count;
        mesh.indexCount = indexCount;
        computeBoundsAndAlpha(vertices, count, mesh);

        packCompactVertices(vertices, count, m_compactScratch);
        GlContextGuard guard;
        if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
        if (!mesh.ibo) glGenBuffers(1, &mesh.ibo);
        if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(CompactVertex)), m_compactScratch.data(), GL_STATIC_DRAW);
        setupCompactVertexAttribs();
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexCount * sizeof(uint32_t)), indices, GL_STATIC_DRAW);
        glBindVertexArray(0);
    }

    // Phase 3A: Static GPU-skinned mesh upload (poses via SkinBlock UBO).
    void uploadSkinnedMesh(uint64_t id, const SkinVertex *vertices, uint32_t count, const uint32_t *indices, uint32_t indexCount) override
    {
        if (!vertices || !count)
            return;

        MaliMesh &mesh = m_meshes[id];
        mesh.lastUsed = m_frameNum;
        mesh.dynamic = false;
        mesh.skinned = true;
        mesh.indexed = (indices != nullptr && indexCount > 0u);
        mesh.vertexCount = count;
        mesh.indexCount = mesh.indexed ? indexCount : 0u;
        mesh.hasBox = false;
        uint8_t minA = 0xFF;
        for (uint32_t i = 0; i < count; ++i)
            if (vertices[i].a < minA) minA = vertices[i].a;
        mesh.minAlpha = minA;

        GlContextGuard guard;
        if (!mesh.vbo) glGenBuffers(1, &mesh.vbo);
        if (mesh.indexed && !mesh.ibo) glGenBuffers(1, &mesh.ibo);
        if (!mesh.vao) glGenVertexArrays(1, &mesh.vao);
        glBindVertexArray(mesh.vao);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(count * sizeof(SkinVertex)), vertices, GL_STATIC_DRAW);
        setupSkinVertexAttribs();
        if (mesh.indexed)
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexCount * sizeof(uint32_t)), indices, GL_STATIC_DRAW);
        }
        glBindVertexArray(0);
    }

    uint32_t framePalette(const float *qwords, uint32_t qwordCount) override
    {
        if (!qwords || !qwordCount)
            return 0u;
        const uint32_t clamped = std::min(qwordCount, kMaxSkinQwords);
        SkinPaletteEntry entry{};
        entry.offsetFloats = static_cast<uint32_t>(m_skinPaletteData.size());
        entry.qwordCount = clamped;
        m_skinPaletteData.insert(m_skinPaletteData.end(), qwords, qwords + clamped * 4u);
        m_skinPalettes.push_back(entry);
        return static_cast<uint32_t>(m_skinPalettes.size()); // 1-based handle
    }

    void rewriteFramePalette(uint32_t handle, const float *qwords, uint32_t qwordCount) override
    {
        if (!handle || handle > m_skinPalettes.size() || !qwords || !qwordCount)
            return;
        SkinPaletteEntry &entry = m_skinPalettes[handle - 1u];
        const uint32_t copyQwords = std::min(qwordCount, entry.qwordCount);
        std::memcpy(m_skinPaletteData.data() + entry.offsetFloats, qwords, copyQwords * 4u * sizeof(float));
    }

    bool hasTexture(uint64_t id) override { return m_textures.find(id) != m_textures.end(); }

    void uploadTexture(uint64_t id, uint32_t width, uint32_t height, const uint8_t *rgba, uint32_t levels) override
    {
        if (!rgba || !width || !height)
            return;

        const uint64_t t0 = m_uploadBudgetUs ? nowUs() : 0u;
        if (m_uploadBudgetUs && m_uploadUsThisFrame >= m_uploadBudgetUs && m_textures.find(id) != m_textures.end())
            return;

        GlContextGuard guard;
        MaliTex &tx = m_textures[id];
        tx.width = width;
        tx.height = height;
        tx.levels = std::max(1u, levels);
        tx.lastUsed = m_frameNum;
        tx.astc = false;
        tx.tex16 = false;
        if (!tx.tex)
            glGenTextures(1, &tx.tex);

        glBindTexture(GL_TEXTURE_2D, tx.tex);

        const size_t pxCount = static_cast<size_t>(width) * height;
        uint8_t minA = 0xFF;
        bool allBinaryAlpha = true;
        for (size_t i = 0; i < pxCount; ++i)
        {
            const uint8_t a = rgba[i * 4 + 3];
            if (a < minA) minA = a;
            if (a > 4u && a < 124u) allBinaryAlpha = false;
        }
        tx.minAlpha = minA;

        if (m_hasAstc && !m_astcCacheDir.empty() && tryUploadAstcFromCache(id, tx))
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
        uint32_t lw = width, lh = height;
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

    void beginFrame() override
    {
        ++m_frameNum;
        m_uploadUsThisFrame = 0;
        m_drawSeq = 0;
        m_culledThisFrame = 0;
        m_drawCallsThisFrame = 0;
        m_instSavedThisFrame = 0;
        m_holdCurrent = false;
        m_dynVerts.clear();
        m_skinPalettes.clear();
        m_skinPaletteData.clear();
        m_qBg.clear();
        m_qAll.clear();
    }

    void draw(uint64_t meshId, const float clip[16], const DrawState &state) override
    {
        if (state.shadowVolume || state.reflectionSource)
            return;
        auto it = m_meshes.find(meshId);
        if (it == m_meshes.end())
            return;
        MaliMesh &mesh = it->second;
        mesh.lastUsed = m_frameNum;

        float minW = 0.0f;
        if (m_cullEnabled && mesh.hasBox && !mesh.dynamic && !mesh.skinned && !state.screen && !state.overlay)
        {
            if (cullAabb(clip, mesh.lo, mesh.hi, minW))
            {
                ++m_culledThisFrame;
                return;
            }
        }

        QueuedDraw q{};
        q.meshId = meshId;
        std::memcpy(q.clip, clip, 16 * sizeof(float));
        q.state = state;
        q.minW = minW;
        q.seq = m_drawSeq++;
        q.skinned = mesh.skinned && state.skinPalette != 0u;

        if (state.screen || state.screenDepthTest)
        {
            q.bucket = 3u;
            q.psoKey = makePsoKey(2u, state.blend, state.screenDepthTest, false, 0u);
        }
        else
        {
            const bool hasAlphaTest = (state.alphaTest & 1u) != 0u;
            const bool needsGuard = state.background && m_fog.skyNear > 0.0f;
            const bool isOpaque = !hasAlphaTest && !needsGuard && !state.overlay && !state.noDepthWrite &&
                                  (state.blend == Blend::Opaque ||
                                   (state.blend == Blend::Normal && state.opacity >= 0.99f && mesh.minAlpha >= 124u && textureIsOpaque(state.texture)));
            const uint32_t opProg = q.skinned ? 3u : 0u;
            const uint32_t cutProg = q.skinned ? 4u : 1u;
            if (isOpaque)
            {
                q.bucket = 0u;
                q.psoKey = makePsoKey(opProg, Blend::Opaque, true, true, 0u);
            }
            else if (hasAlphaTest && !state.overlay)
            {
                q.bucket = 1u;
                q.psoKey = makePsoKey(cutProg, state.blend, true, !state.noDepthWrite, 0u);
            }
            else
            {
                q.bucket = 2u;
                const uint32_t df = state.overlay ? 2u : 0u;
                const bool dw = state.writeDepth && !state.noDepthWrite && !state.overlay;
                q.psoKey = makePsoKey(cutProg, state.blend, true, dw, df);
            }
        }

        if (state.background)
            m_qBg.push_back(q);

        m_qAll.push_back(q);
    }

    void setFog(const Fog &fog) override { m_fog = fog; }
    void setBloom(const Bloom &) override {}
    void addScreenBlur(const ScreenBlur &) override {}
    void setGlowComposite(const GlowComposite &) override {}
    void setDepthOfField(const DepthOfField &) override {}
    void holdFrame() override { m_holdCurrent = true; }

    void endFrame(uint32_t width, uint32_t height) override
    {
        if (!m_initialized || !width || !height)
            return;

        const uint64_t frameStartUs = nowUs();
        GlContextGuard guard;

        const float scale = m_drsEnabled ? m_drsScale : 1.0f;
        const uint32_t rw = std::max(320u, static_cast<uint32_t>(std::lround(width * scale)) & ~3u);
        const uint32_t rh = std::max(240u, static_cast<uint32_t>(std::lround(height * scale)) & ~3u);
        if (m_mainTarget.width != rw || m_mainTarget.height != rh)
            makeTarget(m_mainTarget, rw, rh);

        uploadDynamicArena();

        // Preserve exact PS2 layer order (state.order); within each layer run
        // Opaque (Front-to-Back, grouped for instancing) -> Alpha-Test -> Transparent -> Screen.
        std::sort(m_qAll.begin(), m_qAll.end(), [this](const QueuedDraw &a, const QueuedDraw &b)
        {
            if (a.state.order != b.state.order)
                return a.state.order < b.state.order;
            if (a.bucket != b.bucket)
                return a.bucket < b.bucket;
            if (a.bucket == 0u)
            {
                if (m_instancingEnabled)
                {
                    if (a.psoKey != b.psoKey) return a.psoKey < b.psoKey;
                    if (a.state.texture != b.state.texture) return a.state.texture < b.state.texture;
                    if (a.meshId != b.meshId) return a.meshId < b.meshId;
                }
                if (a.minW != b.minW)
                    return a.minW < b.minW;
            }
            return a.seq < b.seq;
        });

        resetPsoCache();

        // Background 256x256 pass with temporal amortization.
        if (!m_qBg.empty() && m_bgTarget.fbo && (m_frameNum % m_bgRate == 0u || m_frameNum <= 2u))
        {
            glBindFramebuffer(GL_FRAMEBUFFER, m_bgTarget.fbo);
            glViewport(0, 0, static_cast<GLsizei>(m_bgTarget.width), static_cast<GLsizei>(m_bgTarget.height));
            glDepthMask(GL_TRUE);
            glClearColor(m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            for (const QueuedDraw &q : m_qBg)
                emitDrawSingle(q, false, 0.02f, 0.0f);
            invalidateDepth();
        }

        // Main pass (TBDR: always glClear at start, glInvalidateFramebuffer depth at end).
        glBindFramebuffer(GL_FRAMEBUFFER, m_mainTarget.fbo);
        glViewport(0, 0, static_cast<GLsizei>(m_mainTarget.width), static_cast<GLsizei>(m_mainTarget.height));
        glDepthMask(GL_TRUE);
        glClearColor(m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Phase 3C: Optional selective Z-prepass for large static opaque meshes.
        if (m_zPrepassEnabled)
        {
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
            const uint32_t zPso = makePsoKey(5u, Blend::Opaque, true, true, 0u);
            for (const QueuedDraw &q : m_qAll)
            {
                if (q.bucket != 0u || q.skinned || q.state.background)
                    continue;
                auto mit = m_meshes.find(q.meshId);
                if (mit == m_meshes.end() || mit->second.dynamic)
                    continue;
                const uint32_t tris = mit->second.indexed ? mit->second.indexCount : mit->second.vertexCount;
                if (tris < 300u)
                    continue;
                QueuedDraw zq = q;
                zq.psoKey = zPso;
                emitDrawSingle(zq, false, 0.0f, 0.0f);
            }
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }

        // Main layer-ordered pass with Phase 3B hardware instancing on opaque runs.
        for (size_t i = 0; i < m_qAll.size();)
        {
            const QueuedDraw &q = m_qAll[i];
            if (q.bucket == 0u)
            {
                if (m_instancingEnabled && !q.skinned)
                {
                    size_t end = i + 1u;
                    while (end < m_qAll.size() && (end - i) < kMaxInstances && canInstanceTogether(q, m_qAll[end]))
                        ++end;
                    const uint32_t count = static_cast<uint32_t>(end - i);
                    if (count > 1u)
                    {
                        emitDrawInstanced(i, count);
                        m_instSavedThisFrame += (count - 1u);
                        i = end;
                        continue;
                    }
                }
                emitDrawSingle(q, true, 0.0f, 0.0f);
                ++i;
            }
            else if (q.bucket == 1u)
            {
                const uint32_t at = q.state.alphaTest;
                const float aref = static_cast<float>((at >> 4) & 0xFFu) / 128.0f;
                const uint32_t afail = (at >> 12) & 3u;
                emitDrawSingle(q, true, std::max(0.02f, aref), 0.0f);
                if ((afail == 1u || afail == 3u) && aref > 0.02f)
                {
                    QueuedDraw pass2 = q;
                    pass2.psoKey = makePsoKey(q.skinned ? 4u : 1u, q.state.blend, true, false, 1u);
                    emitDrawSingle(pass2, true, 0.02f, aref);
                }
                ++i;
            }
            else if (q.bucket == 2u)
            {
                emitDrawSingle(q, true, 0.02f, 0.0f);
                ++i;
            }
            else
            {
                emitDrawSingle(q, false, 0.01f, 0.0f);
                ++i;
            }
        }

        invalidateDepth();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (m_arenaSync[m_arenaSlot])
            glDeleteSync(m_arenaSync[m_arenaSlot]);
        m_arenaSync[m_arenaSlot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        m_arenaSlot = (m_arenaSlot + 1) % 3;

        if (!m_holdCurrent)
        {
            if (const auto &hook = presentHook())
                hook(m_mainTarget.color, m_mainTarget.width, m_mainTarget.height);
        }

        const double frameMs = static_cast<double>(nowUs() - frameStartUs) * 0.001;
        m_emaFrameMs = m_emaFrameMs > 0.0 ? (m_emaFrameMs * 0.85 + frameMs * 0.15) : frameMs;
        if (m_drsEnabled && (m_frameNum % 15u == 0u))
        {
            const double budgetMs = 1000.0 / static_cast<double>(m_targetFps);
            if (m_emaFrameMs > budgetMs * 0.92 && m_drsScale > 0.75f)
                m_drsScale = std::max(0.75f, m_drsScale - 0.05f);
            else if (m_emaFrameMs < budgetMs * 0.72 && m_drsScale < 1.0f)
                m_drsScale = std::min(1.0f, m_drsScale + 0.05f);
        }

        if (m_statsEnabled && (m_frameNum % 120u == 0u))
        {
            std::fprintf(stderr, "[mali:stats] frame=%llu draws=%u instSaved=%u culled=%u astc=%u drs=%.2f emaMs=%.2f\n",
                         static_cast<unsigned long long>(m_frameNum), m_drawCallsThisFrame,
                         m_instSavedThisFrame, m_culledThisFrame, m_astcHits, m_drsScale, m_emaFrameMs);
        }

        if (m_frameNum % 300u == 0u)
            evictStale();
    }

    bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override
    {
        if (!m_initialized || !m_mainTarget.fbo)
            return false;
        GlContextGuard guard;
        width = m_mainTarget.width;
        height = m_mainTarget.height;
        rgba.resize(static_cast<size_t>(width) * height * 4u);
        glBindFramebuffer(GL_FRAMEBUFFER, m_mainTarget.fbo);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void finishFrames() override {}

    void shutdown() override
    {
        if (!m_initialized)
            return;
        GlContextGuard guard;
        for (int i = 0; i < 3; ++i)
        {
            if (m_arenaSync[i]) { glDeleteSync(m_arenaSync[i]); m_arenaSync[i] = nullptr; }
        }
        glDeleteVertexArrays(3, m_arenaVao);
        glDeleteBuffers(3, m_arenaVbo);
        if (m_instUbo) { glDeleteBuffers(1, &m_instUbo); m_instUbo = 0; }
        if (m_skinUbo) { glDeleteBuffers(1, &m_skinUbo); m_skinUbo = 0; }
        for (auto &[id, m] : m_meshes)
        {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.ibo) glDeleteBuffers(1, &m.ibo);
        }
        m_meshes.clear();
        for (auto &[id, t] : m_textures)
            if (t.tex) glDeleteTextures(1, &t.tex);
        m_textures.clear();
        destroyTarget(m_mainTarget);
        destroyTarget(m_bgTarget);
        if (m_whiteTex) { glDeleteTextures(1, &m_whiteTex); m_whiteTex = 0; }
        auto delProg = [](ShaderProg &p) { if (p.id) { glDeleteProgram(p.id); p.id = 0; } };
        delProg(m_progOpaque);
        delProg(m_progCut);
        delProg(m_progScreen);
        delProg(m_progSkinOpaque);
        delProg(m_progSkinCut);
        delProg(m_progDepth);
        m_initialized = false;
    }

private:
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

    static void computeBoundsAndAlpha(const Vertex *v, uint32_t count, MaliMesh &mesh)
    {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        uint8_t minA = 0xFF;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (v[i].a < minA) minA = v[i].a;
            if (v[i].x < lo[0]) lo[0] = v[i].x; if (v[i].x > hi[0]) hi[0] = v[i].x;
            if (v[i].y < lo[1]) lo[1] = v[i].y; if (v[i].y > hi[1]) hi[1] = v[i].y;
            if (v[i].z < lo[2]) lo[2] = v[i].z; if (v[i].z > hi[2]) hi[2] = v[i].z;
        }
        mesh.minAlpha = minA;
        mesh.hasBox = std::isfinite(lo[0] + lo[1] + lo[2] + hi[0] + hi[1] + hi[2]);
        std::memcpy(mesh.lo, lo, sizeof(lo));
        std::memcpy(mesh.hi, hi, sizeof(hi));
    }

    bool textureIsOpaque(uint64_t texId) const
    {
        if (!texId)
            return true;
        auto it = m_textures.find(texId);
        return it != m_textures.end() && it->second.minAlpha >= 124u;
    }

    static void makeTarget(MaliTarget &t, uint32_t w, uint32_t h)
    {
        destroyTarget(t);
        t.width = w;
        t.height = h;
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

        glGenRenderbuffers(1, &t.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t.depth);
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

    void uploadDynamicArena()
    {
        if (m_dynVerts.empty())
            return;
        const int slot = m_arenaSlot;
        bool gpuBusy = false;
        if (m_arenaSync[slot])
        {
            const GLenum res = glClientWaitSync(m_arenaSync[slot], 0, 0);
            gpuBusy = (res == GL_TIMEOUT_EXPIRED);
            if (!gpuBusy)
            {
                glDeleteSync(m_arenaSync[slot]);
                m_arenaSync[slot] = nullptr;
            }
        }
        const size_t bytes = m_dynVerts.size() * sizeof(Vertex);
        glBindBuffer(GL_ARRAY_BUFFER, m_arenaVbo[slot]);
        if (gpuBusy || bytes > m_arenaCap[slot])
        {
            const size_t cap = std::max(bytes, m_arenaCap[slot]);
            m_arenaCap[slot] = (cap + 65535u) & ~size_t(65535u);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_arenaCap[slot]), nullptr, GL_STREAM_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), m_dynVerts.data());
    }

    void resetPsoCache()
    {
        m_lastPso = ~0u;
        m_lastTex = ~0u;
        m_lastClamp = 0xFF;
        m_lastVao = ~0u;
        m_lastSkinPalette = 0u;
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

    void bindCommonUniforms(const ShaderProg &prog, const QueuedDraw &q, bool allowBgFog, float minAlpha, float maxAlpha)
    {
        GLuint glTex = m_whiteTex;
        bool hasTex = false;
        if (q.state.texture)
        {
            auto tit = m_textures.find(q.state.texture);
            if (tit != m_textures.end() && tit->second.tex)
            {
                glTex = tit->second.tex;
                tit->second.lastUsed = m_frameNum;
                hasTex = true;
            }
        }
        if (glTex != m_lastTex || q.state.clamp != m_lastClamp)
        {
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (q.state.clamp & 1u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (q.state.clamp & 2u) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            m_lastTex = glTex;
            m_lastClamp = q.state.clamp;
        }

        if (q.skinned && q.state.skinPalette && q.state.skinPalette != m_lastSkinPalette && q.state.skinPalette <= m_skinPalettes.size())
        {
            const SkinPaletteEntry &pe = m_skinPalettes[q.state.skinPalette - 1u];
            const uint32_t qwords = std::min(pe.qwordCount, kMaxSkinQwords);
            glBindBuffer(GL_UNIFORM_BUFFER, m_skinUbo);
            glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(qwords * 4u * sizeof(float)),
                            m_skinPaletteData.data() + pe.offsetFloats);
            m_lastSkinPalette = q.state.skinPalette;
        }

        const float litMode = q.state.fresnel ? -1.0f : (q.state.lit ? 1.0f : 0.0f);
        if (prog.uMisc >= 0)
            glUniform4f(prog.uMisc, litMode, q.state.opacity, q.state.uvOffset[0], q.state.uvOffset[1]);
        if (q.state.lit || q.state.fresnel)
        {
            if (prog.uDirections >= 0) glUniform4fv(prog.uDirections, 4, q.state.light);
            if (prog.uColours >= 0) glUniform4fv(prog.uColours, 4, q.state.light + 16);
        }
        const bool vfog = m_fog.vertexFog && (!q.state.background || q.state.backgroundFge);
        if (prog.uFogB >= 0)
            glUniform4f(prog.uFogB, 0.0f, 0.0f, vfog ? 1.0f : 0.0f, m_fog.base);
        if (prog.uFogC >= 0)
            glUniform4f(prog.uFogC, m_fog.scale, q.state.lodK, q.state.lodScale, q.state.maxLod);
        if (prog.uFogCol >= 0)
            glUniform4f(prog.uFogCol, m_fog.colour[0], m_fog.colour[1], m_fog.colour[2], hasTex ? 1.0f : 0.0f);
        if (prog.uFogA >= 0)
        {
            const bool bgFog = allowBgFog && m_fog.enabled && q.state.fogged && !q.state.background;
            glUniform4f(prog.uFogA, m_fog.start, m_fog.width, static_cast<float>(m_fog.steps), bgFog ? 1.0f : 0.0f);
        }
        if (prog.uGuard >= 0)
        {
            const bool useGuard = q.state.background && m_fog.skyNear > 0.0f;
            glUniform4f(prog.uGuard, useGuard ? 1.0f : 0.0f, m_fog.skyNear, q.state.screenDepthTest ? 1.0f : 0.0f, 0.0f);
        }
        if (prog.uAlpha >= 0)
            glUniform4f(prog.uAlpha, minAlpha, maxAlpha, 0.0f, 0.0f);
    }

    void emitDrawSingle(const QueuedDraw &q, bool allowBgFog, float minAlpha, float maxAlpha)
    {
        auto mit = m_meshes.find(q.meshId);
        if (mit == m_meshes.end())
            return;
        const MaliMesh &mesh = mit->second;

        const ShaderProg &prog = applyPso(q.psoKey);
        if (prog.uInstanceCount >= 0)
            glUniform1i(prog.uInstanceCount, 1);
        if (prog.uClip >= 0)
            glUniform4fv(prog.uClip, 4, q.clip);
        bindCommonUniforms(prog, q, allowBgFog, minAlpha, maxAlpha);

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

    // Phase 3B: Hardware instancing batch (2..64 identical static opaque draws in 1 GL call).
    void emitDrawInstanced(size_t startIdx, uint32_t count)
    {
        const QueuedDraw &q = m_qAll[startIdx];
        auto mit = m_meshes.find(q.meshId);
        if (mit == m_meshes.end())
            return;
        const MaliMesh &mesh = mit->second;

        m_instClipsScratch.resize(count * 16u);
        for (uint32_t k = 0; k < count; ++k)
            std::memcpy(m_instClipsScratch.data() + k * 16u, m_qAll[startIdx + k].clip, 16u * sizeof(float));

        glBindBuffer(GL_UNIFORM_BUFFER, m_instUbo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(count * 16u * sizeof(float)), m_instClipsScratch.data());

        const ShaderProg &prog = applyPso(q.psoKey);
        if (prog.uInstanceCount >= 0)
            glUniform1i(prog.uInstanceCount, static_cast<GLint>(count));
        bindCommonUniforms(prog, q, true, 0.0f, 0.0f);

        const GLuint targetVao = mesh.dynamic ? m_arenaVao[m_arenaSlot] : mesh.vao;
        if (targetVao != m_lastVao)
        {
            glBindVertexArray(targetVao);
            m_lastVao = targetVao;
        }
        if (mesh.indexed)
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr, static_cast<GLsizei>(count));
        else
            glDrawArraysInstanced(GL_TRIANGLES, static_cast<GLint>(mesh.dynamic ? mesh.firstVertex : 0u),
                                  static_cast<GLsizei>(mesh.vertexCount), static_cast<GLsizei>(count));
        ++m_drawCallsThisFrame;
    }

    void evictStale()
    {
        for (auto it = m_meshes.begin(); it != m_meshes.end();)
        {
            if (it->second.dynamic && it->second.lastUsed + 30u < m_frameNum)
            {
                it = m_meshes.erase(it);
                ++m_evictions;
            }
            else
            {
                ++it;
            }
        }
    }

    std::function<void()> m_makeCurrent;
    bool m_initialized = false;
    bool m_hasAstc = false;
    bool m_cullEnabled = true;
    bool m_tex16Enabled = true;
    bool m_drsEnabled = true;
    bool m_shaderCacheEnabled = true;
    bool m_gpuSkinEnabled = true;
    bool m_instancingEnabled = true;
    bool m_zPrepassEnabled = false;
    bool m_statsEnabled = false;
    bool m_holdCurrent = false;
    uint64_t m_uploadBudgetUs = 1500;
    uint64_t m_uploadUsThisFrame = 0;
    uint32_t m_bgRate = 2;
    int m_targetFps = 30;
    float m_drsScale = 1.0f;
    double m_emaFrameMs = 0.0;

    std::string m_astcCacheDir;
    std::string m_shaderCacheDir;

    uint64_t m_frameNum = 0;
    uint64_t m_evictions = 0;
    uint32_t m_drawSeq = 0;
    uint32_t m_culledThisFrame = 0;
    uint32_t m_drawCallsThisFrame = 0;
    uint32_t m_instSavedThisFrame = 0;
    uint32_t m_astcHits = 0;

    ShaderProg m_progOpaque{}, m_progCut{}, m_progScreen{};
    ShaderProg m_progSkinOpaque{}, m_progSkinCut{}, m_progDepth{};
    GLuint m_instUbo = 0;
    GLuint m_skinUbo = 0;
    GLuint m_whiteTex = 0;
    MaliTarget m_mainTarget{}, m_bgTarget{};

    GLuint m_arenaVao[3] = {};
    GLuint m_arenaVbo[3] = {};
    size_t m_arenaCap[3] = {};
    GLsync m_arenaSync[3] = {};
    int m_arenaSlot = 0;

    uint32_t m_lastPso = ~0u;
    GLuint m_lastTex = ~0u;
    uint8_t m_lastClamp = 0xFF;
    GLuint m_lastVao = ~0u;
    uint32_t m_lastSkinPalette = 0u;

    Fog m_fog{};
    std::vector<Vertex> m_dynVerts;
    std::vector<CompactVertex> m_compactScratch;
    std::vector<uint16_t> m_tex16Scratch;
    std::vector<uint8_t> m_astcScratch;
    std::vector<float> m_instClipsScratch;
    std::vector<SkinPaletteEntry> m_skinPalettes;
    std::vector<float> m_skinPaletteData;
    std::vector<QueuedDraw> m_qBg, m_qAll;
    std::unordered_map<uint64_t, MaliMesh> m_meshes;
    std::unordered_map<uint64_t, MaliTex> m_textures;
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