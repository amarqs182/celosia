#include "native_backend.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#elif defined(__linux__)
#include <GLES3/gl3.h>
#define PS2X_MALI_GLES 1
#if defined(PS2X_LINUX_SDL2_SHIM)
void ps2xLinuxGlLock();
void ps2xLinuxGlUnlock();
#endif
#endif

namespace native_renderer::detail {
namespace {

struct GlLockGuard {
  GlLockGuard() {
#if defined(PS2X_LINUX_SDL2_SHIM)
    ps2xLinuxGlLock();
#endif
  }
  ~GlLockGuard() {
#if defined(PS2X_LINUX_SDL2_SHIM)
    ps2xLinuxGlUnlock();
#endif
  }
};

int envInt(const char *name, int defVal) {
  const char *v = std::getenv(name);
  return (v && *v) ? std::atoi(v) : defVal;
}

bool envBool(const char *name, bool defVal) {
  const char *v = std::getenv(name);
  if (!v || !*v) return defVal;
  return (*v != '0' && *v != 'f' && *v != 'F');
}

#if defined(PS2X_MALI_GLES)
#define GLSL_HDR "#version 310 es\nprecision highp float;\nprecision highp int;\n"
#define GLSL_MED_OUT "mediump "
#define GLSL_HIGH_OUT "highp "
#else
#define GLSL_HDR "#version 330 core\n"
#define GLSL_MED_OUT ""
#define GLSL_HIGH_OUT ""
#endif

const char *kWorldVS = GLSL_HDR R"(
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNrm;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aCol;
uniform mat4 uClip;
uniform vec2 uUVOff;
uniform float uFogA;
uniform float uFogC;
out )" GLSL_HIGH_OUT R"(vec2 vUV;
out )" GLSL_HIGH_OUT R"(float vLogW;
out )" GLSL_MED_OUT R"(vec4 vCol;
out )" GLSL_MED_OUT R"(float vFog;
void main() {
  vec4 p = uClip * vec4(aPos, 1.0);
  gl_Position = p;
  vUV = aUV + uUVOff;
  vLogW = p.w;
  vCol = clamp(aCol * 2.0, 0.0, 2.0);
  vFog = clamp(uFogA + uFogC / max(abs(p.w), 1e-4), 0.0, 1.0);
}
)";

const char *kOpaqueFS = GLSL_HDR R"(
uniform sampler2D uTex;
uniform )" GLSL_MED_OUT R"(vec3 uFogCol;
uniform float uOpacity;
uniform float uFogOn;
in )" GLSL_HIGH_OUT R"(vec2 vUV;
in )" GLSL_HIGH_OUT R"(float vLogW;
in )" GLSL_MED_OUT R"(vec4 vCol;
in )" GLSL_MED_OUT R"(float vFog;
out )" GLSL_MED_OUT R"(vec4 oCol;
void main() {
  )" GLSL_MED_OUT R"(vec4 t = texture(uTex, vUV);
  )" GLSL_MED_OUT R"(vec3 rgb = t.rgb * vCol.rgb;
  rgb = mix(rgb, uFogCol, vFog * uFogOn);
  oCol = vec4(rgb, clamp(t.a * vCol.a * uOpacity, 0.0, 1.0));
}
)";

const char *kCutFS = GLSL_HDR R"(
uniform sampler2D uTex;
uniform )" GLSL_MED_OUT R"(vec3 uFogCol;
uniform float uOpacity;
uniform float uFogOn;
uniform float uAlpha;
uniform float uGuard;
in )" GLSL_HIGH_OUT R"(vec2 vUV;
in )" GLSL_HIGH_OUT R"(float vLogW;
in )" GLSL_MED_OUT R"(vec4 vCol;
in )" GLSL_MED_OUT R"(float vFog;
out )" GLSL_MED_OUT R"(vec4 oCol;
void main() {
  )" GLSL_MED_OUT R"(vec4 t = texture(uTex, vUV);
  float a = clamp(t.a * vCol.a * uOpacity, 0.0, 1.0);
  if (uGuard > 0.0 ? (a < uAlpha) : (a >= uAlpha)) discard;
  )" GLSL_MED_OUT R"(vec3 rgb = t.rgb * vCol.rgb;
  rgb = mix(rgb, uFogCol, vFog * uFogOn);
  oCol = vec4(rgb, a);
}
)";

const char *kScreenVS = GLSL_HDR R"(
layout(location=0) in vec3 aPos;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aCol;
uniform mat4 uClip;
uniform vec2 uUVOff;
out )" GLSL_HIGH_OUT R"(vec2 vUV;
out )" GLSL_HIGH_OUT R"(vec2 vScreen;
out )" GLSL_MED_OUT R"(vec4 vCol;
void main() {
  vec4 p = uClip * vec4(aPos, 1.0);
  gl_Position = p;
  vUV = aUV + uUVOff;
  vScreen = p.xy * 0.5 + 0.5;
  vCol = clamp(aCol * 2.0, 0.0, 2.0);
}
)";

const char *kScreenFS = GLSL_HDR R"(
uniform sampler2D uTex;
uniform float uOpacity;
in )" GLSL_HIGH_OUT R"(vec2 vUV;
in )" GLSL_HIGH_OUT R"(vec2 vScreen;
in )" GLSL_MED_OUT R"(vec4 vCol;
out )" GLSL_MED_OUT R"(vec4 oCol;
void main() {
  )" GLSL_MED_OUT R"(vec4 t = texture(uTex, vUV);
  oCol = vec4(t.rgb * vCol.rgb, clamp(t.a * vCol.a * uOpacity, 0.0, 1.0));
}
)";
GLuint compileShader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

struct Prog {
  GLuint id = 0;
  GLint uClip = -1, uTex = -1, uUVOff = -1, uOpacity = -1;
  GLint uFogA = -1, uFogC = -1, uFogCol = -1, uFogOn = -1;
  GLint uAlpha = -1, uGuard = -1;
  void init(const char *vs, const char *fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { glDeleteShader(v); glDeleteShader(f); return; }
    id = glCreateProgram();
    glAttachShader(id, v); glAttachShader(id, f);
    glLinkProgram(id);
    glDeleteShader(v); glDeleteShader(f);
    uClip = glGetUniformLocation(id, "uClip");
    uTex = glGetUniformLocation(id, "uTex");
    uUVOff = glGetUniformLocation(id, "uUVOff");
    uOpacity = glGetUniformLocation(id, "uOpacity");
    uFogA = glGetUniformLocation(id, "uFogA");
    uFogC = glGetUniformLocation(id, "uFogC");
    uFogCol = glGetUniformLocation(id, "uFogCol");
    uFogOn = glGetUniformLocation(id, "uFogOn");
    uAlpha = glGetUniformLocation(id, "uAlpha");
    uGuard = glGetUniformLocation(id, "uGuard");
  }
};

struct Mesh {
  GLuint vbo = 0, ibo = 0;
  uint32_t vCount = 0, iCount = 0;
  float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
  bool perFrame = false, indexed = false;
};

struct Tex {
  GLuint glId = 0;
  uint32_t w = 0, h = 0;
};

struct PendingTex {
  uint64_t id = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

struct Cmd {
  uint64_t mesh = 0;
  float clip[16]{};
  DrawState st;
  float minW = 0.0f;
};

struct RingSlot {
  GLuint vbo = 0, ibo = 0;
  size_t vCap = 0, iCap = 0, vOff = 0, iOff = 0;
  GLsync fence = nullptr;
};

class MaliBackend final : public Backend {
public:
  explicit MaliBackend(std::function<void()> makeCurrent)
      : makeCurrent_(std::move(makeCurrent)) {
    cullEnabled_ = envBool("SOTC_MALI_CULL", true);
    tex16_ = envBool("SOTC_MALI_TEX16", true);
    uploadBudgetUs_ = envInt("SOTC_MALI_UPLOAD_BUDGET_US", 2000);
    bgRate_ = std::max(1, envInt("SOTC_MALI_BG_RATE", 1));
    drsEnabled_ = envBool("SOTC_MALI_DRS", false);
  }
  ~MaliBackend() override { shutdown(); }

  bool hasMesh(uint64_t id) override { return meshes_.count(id) != 0; }

  void uploadMesh(uint64_t id, const Vertex *v, uint32_t count, bool perFrame) override {
    if (!v || !count) return;
    GlLockGuard lk;
    ensureGl();
    auto &m = meshes_[id];
    if (!m.vbo) glGenBuffers(1, &m.vbo);
    m.vCount = count; m.iCount = 0; m.perFrame = perFrame; m.indexed = false;
    computeBounds(v, count, m);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(Vertex), v,
                 perFrame ? GL_STREAM_DRAW : GL_STATIC_DRAW);
  }

  void uploadIndexedMesh(uint64_t id, const Vertex *v, uint32_t vc,
                         const uint32_t *idx, uint32_t ic) override {
    if (!v || !vc || !idx || !ic) return;
    GlLockGuard lk;
    ensureGl();
    auto &m = meshes_[id];
    if (!m.vbo) glGenBuffers(1, &m.vbo);
    if (!m.ibo) glGenBuffers(1, &m.ibo);
    m.vCount = vc; m.iCount = ic; m.perFrame = false; m.indexed = true;
    computeBounds(v, vc, m);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, vc * sizeof(Vertex), v, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ic * sizeof(uint32_t), idx, GL_STATIC_DRAW);
  }

  bool hasTexture(uint64_t id) override {
    if (textures_.count(id)) return true;
    for (const auto &p : pendingTex_) if (p.id == id) return true;
    return false;
  }

  void uploadTexture(uint64_t id, uint32_t w, uint32_t h,
                     const uint8_t *rgba, uint32_t) override {
    if (!rgba || !w || !h) return;
    PendingTex pt{id, w, h, std::vector<uint8_t>(rgba, rgba + size_t(w) * h * 4)};
    pendingTex_.push_back(std::move(pt));
  }

  void beginFrame() override {
    opaqueQ_.clear(); cutQ_.clear(); transQ_.clear();
  }

  void draw(uint64_t mesh, const float clip[16], const DrawState &st) override {
    auto it = meshes_.find(mesh);
    if (it == meshes_.end()) return;
    if (cullEnabled_ && !st.screen && !aabbVisible(it->second, clip)) return;
    Cmd c{mesh, {}, st, std::fabs(clip[15])};
    std::memcpy(c.clip, clip, sizeof(c.clip));
    if (st.screen || st.overlay || st.blend != Blend::Opaque || st.opacity < 0.999f)
      transQ_.push_back(c);
    else if (st.alphaTest != 0)
      cutQ_.push_back(c);
    else
      opaqueQ_.push_back(c);
  }

  void setFog(const Fog &fog) override { fog_ = fog; }
  void setBloom(const Bloom &) override {}
  void addScreenBlur(const ScreenBlur &) override {}
  void setGlowComposite(const GlowComposite &) override {}
  void setDepthOfField(const DepthOfField &) override {}

  void endFrame(uint32_t width, uint32_t height) override {
    if (!width || !height) return;
    GlLockGuard lk;
    ensureGl();
    auto t0 = std::chrono::steady_clock::now();
    advanceRing();
    flushTextureBudget();
    if (drsEnabled_ && lastFrameUs_ > 0) {
      if (lastFrameUs_ > 18000 && drsScale_ > 0.55f) drsScale_ -= 0.05f;
      else if (lastFrameUs_ < 13000 && drsScale_ < 1.0f) drsScale_ = std::min(1.0f, drsScale_ + 0.05f);
    }
    uint32_t rw = std::max(64u, uint32_t(width * (drsEnabled_ ? drsScale_ : 1.0f)));
    uint32_t rh = std::max(64u, uint32_t(height * (drsEnabled_ ? drsScale_ : 1.0f)));
    ensureFbo(rw, rh);
    if (bgRate_ > 1 && (frameIdx_ % uint32_t(bgRate_)) == 0) renderBgPass();
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, GLsizei(rw), GLsizei(rh));
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(fog_.colour[0], fog_.colour[1], fog_.colour[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    std::sort(opaqueQ_.begin(), opaqueQ_.end(),
              [](const Cmd &a, const Cmd &b) { return a.minW < b.minW; });
    for (const auto &c : opaqueQ_) drawCmd(c, progOpaque_, GL_LESS, 0.0f);
    for (const auto &c : cutQ_) drawCmd(c, progCut_, GL_GEQUAL, 1.0f);
    for (const auto &c : cutQ_) drawCmd(c, progCut_, GL_LESS, 0.0f);
    for (const auto &c : transQ_)
      drawCmd(c, c.st.screen ? progScreen_ : progOpaque_, GL_LESS, 0.0f);
#if defined(PS2X_MALI_GLES)
    const GLenum inv[1] = {GL_DEPTH_ATTACHMENT};
    glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, inv);
#endif
    ring_[ringIdx_].fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (const auto &hook = presentHook()) hook(colorTex_, rw, rh);
    auto t1 = std::chrono::steady_clock::now();
    lastFrameUs_ = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    ++frameIdx_;
  }

  bool latestFrame(std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) override {
    if (!fbo_ || !renderW_ || !renderH_) return false;
    GlLockGuard lk;
    w = renderW_; h = renderH_;
    rgba.resize(size_t(w) * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
  }

  void shutdown() override {
    GlLockGuard lk;
    for (auto &kv : meshes_) {
      if (kv.second.vbo) glDeleteBuffers(1, &kv.second.vbo);
      if (kv.second.ibo) glDeleteBuffers(1, &kv.second.ibo);
    }
    for (auto &kv : textures_) if (kv.second.glId) glDeleteTextures(1, &kv.second.glId);
    for (auto &s : ring_) {
      if (s.fence) glDeleteSync(s.fence);
      if (s.vbo) glDeleteBuffers(1, &s.vbo);
      if (s.ibo) glDeleteBuffers(1, &s.ibo);
    }
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (depthRb_) glDeleteRenderbuffers(1, &depthRb_);
    if (bgFbo_) glDeleteFramebuffers(1, &bgFbo_);
    if (bgTex_) glDeleteTextures(1, &bgTex_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    meshes_.clear(); textures_.clear(); pendingTex_.clear();
  }

private:
  void ensureGl() {
    if (glInit_) return;
    if (makeCurrent_) makeCurrent_();
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    progOpaque_.init(kWorldVS, kOpaqueFS);
    progCut_.init(kWorldVS, kCutFS);
    progScreen_.init(kScreenVS, kScreenFS);
    for (auto &s : ring_) {
      glGenBuffers(1, &s.vbo);
      glGenBuffers(1, &s.ibo);
    }
    glGenTextures(1, &whiteTex_);
    glBindTexture(GL_TEXTURE_2D, whiteTex_);
    const uint32_t wpx = 0xffffffffu;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &wpx);
    glInit_ = true;
  }

  void ensureFbo(uint32_t w, uint32_t h) {
    if (fbo_ && renderW_ == w && renderH_ == h) return;
    renderW_ = w; renderH_ = h;
    if (!fbo_) glGenFramebuffers(1, &fbo_);
    if (!colorTex_) glGenTextures(1, &colorTex_);
    if (!depthRb_) glGenRenderbuffers(1, &depthRb_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GLsizei(w), GLsizei(h));
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb_);
  }

  void renderBgPass() {
    if (!bgFbo_) {
      glGenFramebuffers(1, &bgFbo_);
      glGenTextures(1, &bgTex_);
      glBindTexture(GL_TEXTURE_2D, bgTex_);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, bgFbo_);
    glViewport(0, 0, 256, 256);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  void advanceRing() {
    ringIdx_ = (ringIdx_ + 1) % 3;
    auto &s = ring_[ringIdx_];
    if (s.fence) {
      glClientWaitSync(s.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 5000000);
      glDeleteSync(s.fence);
      s.fence = nullptr;
    }
    glBindBuffer(GL_ARRAY_BUFFER, s.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(std::max<size_t>(s.vCap, 65536)), nullptr, GL_STREAM_DRAW);
    s.vOff = 0; s.iOff = 0;
  }

  void flushTextureBudget() {
    auto start = std::chrono::steady_clock::now();
    while (!pendingTex_.empty()) {
      auto pt = std::move(pendingTex_.front());
      pendingTex_.erase(pendingTex_.begin());
      auto &t = textures_[pt.id];
      if (!t.glId) glGenTextures(1, &t.glId);
      t.w = pt.w; t.h = pt.h;
      glBindTexture(GL_TEXTURE_2D, t.glId);
      if (tex16_) {
        bool hasAlpha = false;
        size_t n = size_t(pt.w) * pt.h;
        for (size_t i = 3; i < n * 4; i += 4) if (pt.rgba[i] < 250) { hasAlpha = true; break; }
        std::vector<uint16_t> buf(n);
        if (hasAlpha) {
          for (size_t i = 0; i < n; ++i) {
            const uint8_t *p = &pt.rgba[i * 4];
            buf[i] = uint16_t(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) | (p[3] >= 128 ? 1 : 0));
          }
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, GLsizei(pt.w), GLsizei(pt.h), 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, buf.data());
        } else {
          for (size_t i = 0; i < n; ++i) {
            const uint8_t *p = &pt.rgba[i * 4];
            buf[i] = uint16_t(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
          }
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB565, GLsizei(pt.w), GLsizei(pt.h), 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, buf.data());
        }
      } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(pt.w), GLsizei(pt.h), 0, GL_RGBA, GL_UNSIGNED_BYTE, pt.rgba.data());
      }
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
      if (uploadBudgetUs_ > 0 && us >= uploadBudgetUs_) break;
    }
  }

  void applyPso(const DrawState &st, GLenum depthFunc, uint32_t progId) {
    uint32_t key = (uint32_t(st.blend) & 3u) | ((st.screen ? 0u : 1u) << 2) |
                   ((!st.noDepthWrite && !st.overlay && !st.screen ? 1u : 0u) << 3) |
                   ((depthFunc == GL_GEQUAL ? 1u : 0u) << 4) | ((progId & 0xffu) << 8);
    if (key == lastPso_) return;
    lastPso_ = key;
    if (st.screen) glDisable(GL_DEPTH_TEST); else { glEnable(GL_DEPTH_TEST); glDepthFunc(depthFunc); }
    glDepthMask((!st.noDepthWrite && !st.overlay && !st.screen) ? GL_TRUE : GL_FALSE);
    if (st.blend == Blend::Opaque) glDisable(GL_BLEND);
    else {
      glEnable(GL_BLEND);
      if (st.blend == Blend::Additive) glBlendFunc(GL_SRC_ALPHA, GL_ONE);
      else glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    glUseProgram(progId);
  }

  void drawCmd(const Cmd &c, const Prog &p, GLenum depthFunc, float guard) {
    auto mit = meshes_.find(c.mesh);
    if (mit == meshes_.end() || !p.id) return;
    applyPso(c.st, depthFunc, p.id);
    if (p.uClip >= 0) glUniformMatrix4fv(p.uClip, 1, GL_FALSE, c.clip);
    if (p.uUVOff >= 0) glUniform2fv(p.uUVOff, 1, c.st.uvOffset);
    if (p.uOpacity >= 0) glUniform1f(p.uOpacity, c.st.opacity);
    if (p.uFogOn >= 0) glUniform1f(p.uFogOn, fog_.enabled ? 1.0f : 0.0f);
    if (p.uFogA >= 0) glUniform1f(p.uFogA, fog_.base / 255.0f);
    if (p.uFogC >= 0) glUniform1f(p.uFogC, fog_.scale);
    if (p.uFogCol >= 0) glUniform3fv(p.uFogCol, 1, fog_.colour);
    if (p.uAlpha >= 0) glUniform1f(p.uAlpha, float((c.st.alphaTest >> 4) & 0xff) / 255.0f);
    if (p.uGuard >= 0) glUniform1f(p.uGuard, guard);
    auto tit = textures_.find(c.st.texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tit != textures_.end() ? tit->second.glId : whiteTex_);
    if (p.uTex >= 0) glUniform1i(p.uTex, 0);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, mit->second.vbo);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, nx)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, u)));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, r)));
    if (mit->second.indexed) {
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mit->second.ibo);
      glDrawElements(GL_TRIANGLES, GLsizei(mit->second.iCount), GL_UNSIGNED_INT, nullptr);
    } else {
      glDrawArrays(GL_TRIANGLES, 0, GLsizei(mit->second.vCount));
    }
  }

  static void computeBounds(const Vertex *v, uint32_t n, Mesh &m) {
    for (int i = 0; i < 3; ++i) { m.bmin[i] = 1e30f; m.bmax[i] = -1e30f; }
    for (uint32_t i = 0; i < n; ++i) {
      m.bmin[0] = std::min(m.bmin[0], v[i].x); m.bmax[0] = std::max(m.bmax[0], v[i].x);
      m.bmin[1] = std::min(m.bmin[1], v[i].y); m.bmax[1] = std::max(m.bmax[1], v[i].y);
      m.bmin[2] = std::min(m.bmin[2], v[i].z); m.bmax[2] = std::max(m.bmax[2], v[i].z);
    }
  }

  static bool aabbVisible(const Mesh &m, const float c[16]) {
    uint8_t out = 0x3f;
    for (int i = 0; i < 8; ++i) {
      float x = (i & 1) ? m.bmax[0] : m.bmin[0];
      float y = (i & 2) ? m.bmax[1] : m.bmin[1];
      float z = (i & 4) ? m.bmax[2] : m.bmin[2];
      float cx = c[0]*x + c[4]*y + c[8]*z + c[12];
      float cy = c[1]*x + c[5]*y + c[9]*z + c[13];
      float cw = c[3]*x + c[7]*y + c[11]*z + c[15];
      uint8_t f = (cx < -cw ? 1 : 0) | (cx > cw ? 2 : 0) | (cy < -cw ? 4 : 0) | (cy > cw ? 8 : 0) | (cw <= 0 ? 16 : 0);
      out &= f;
    }
    return out == 0;
  }

  std::function<void()> makeCurrent_;
  std::unordered_map<uint64_t, Mesh> meshes_;
  std::unordered_map<uint64_t, Tex> textures_;
  std::vector<PendingTex> pendingTex_;
  std::vector<Cmd> opaqueQ_, cutQ_, transQ_;
  Fog fog_;
  Prog progOpaque_, progCut_, progScreen_;
  RingSlot ring_[3]{};
  uint32_t ringIdx_ = 0, lastPso_ = ~0u, frameIdx_ = 0, renderW_ = 0, renderH_ = 0;
  GLuint vao_ = 0, fbo_ = 0, colorTex_ = 0, depthRb_ = 0, bgFbo_ = 0, bgTex_ = 0, whiteTex_ = 0;
  bool glInit_ = false, cullEnabled_ = true, tex16_ = true, drsEnabled_ = false;
  int uploadBudgetUs_ = 2000, bgRate_ = 1;
  int64_t lastFrameUs_ = 0;
  float drsScale_ = 1.0f;
};

} // namespace

Backend *createMaliBackend(std::function<void()> makeCurrent) {
  return new MaliBackend(std::move(makeCurrent));
}

} // namespace native_renderer::detail

#else
namespace native_renderer::detail {
Backend *createMaliBackend(std::function<void()>) { return nullptr; }
} // namespace native_renderer::detail
#endif


