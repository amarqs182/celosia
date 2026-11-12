// raylib subset for Linux (also AArch64): video on GLES3 (the context is
// SDL's SDL_GL_CreateContext), audio through SDL2, controls from the SDL2
// gamepad and keyboard. See raylib.h. Mirrors platform/vita/raylib
// (raylib_vita.cpp) with SDL2 + GLES in place of the SCE/vitaGL platform:
// the window, gamepad, audio and drawing structure is the same, only the
// platform calls differ.

#include "raylib.h"

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

// The boot's timeline (as ps2_stats::bootMark: the process clock).
static void bootMark(const char *what)
{
    std::fprintf(stderr, "[boot] %.3f s %s\n", GetTime(), what);
}

namespace
{
    SDL_Window *g_window = nullptr;
    SDL_GLContext g_context = nullptr;
    bool g_glReady = false;
    bool g_quit = false;
    int g_width = 960, g_height = 544;
    uint64_t g_startCounter = 0;
    double g_lastFrameTime = 0.0;
    double g_frameTime = 0.0;

    std::recursive_mutex g_glMutex;
    int g_glLockDepth = 0;
    std::mutex g_inputMutex;
    std::atomic<int> g_swapInterval{1};
    int g_appliedSwapInterval = 1;
}

void ps2xLinuxGlLock()
{
    g_glMutex.lock();
    if (++g_glLockDepth == 1 && g_window && g_context)
    {
        if (SDL_GL_MakeCurrent(g_window, g_context) != 0)
            std::fprintf(stderr, "[linux] SDL_GL_MakeCurrent(ctx) failed: %s\n", SDL_GetError());
    }
}

void ps2xLinuxGlUnlock()
{
    if (--g_glLockDepth == 0 && g_window && g_context)
    {
        if (SDL_GL_MakeCurrent(g_window, nullptr) != 0)
            std::fprintf(stderr, "[linux] SDL_GL_MakeCurrent(null) failed: %s\n", SDL_GetError());
    }
    g_glMutex.unlock();
}

namespace
{
    struct GlLock
    {
        GlLock() { ps2xLinuxGlLock(); }
        ~GlLock() { ps2xLinuxGlUnlock(); }
    };

    struct TextureSlot
    {
        GLuint texture = 0;
        std::vector<uint8_t> pixels; // last upload, for LoadImageFromTexture
    };
    std::map<unsigned int, TextureSlot> g_textures;
    unsigned int g_nextTexture = 1;

    // FPS over the last second of EndDrawing() calls.
    int g_fps = 0;
    int g_framesThisSecond = 0;
    double g_fpsWindowStart = 0.0;

    // Controls: up to two SDL game controllers merged (ports 0 and 1, like
    // the Vita, which merges its own pad with a PS TV pad). The bits and
    // axes keep the SceCtrlData layout of raylib_vita.cpp.
    enum : unsigned int
    {
        CTRL_UP = 1u << 0,
        CTRL_RIGHT = 1u << 1,
        CTRL_DOWN = 1u << 2,
        CTRL_LEFT = 1u << 3,
        CTRL_TRIANGLE = 1u << 4,
        CTRL_CIRCLE = 1u << 5,
        CTRL_CROSS = 1u << 6,
        CTRL_SQUARE = 1u << 7,
        CTRL_L1 = 1u << 8,
        CTRL_R1 = 1u << 9,
        CTRL_L2 = 1u << 10,
        CTRL_R2 = 1u << 11,
        CTRL_SELECT = 1u << 12,
        CTRL_START = 1u << 13,
        CTRL_L3 = 1u << 14,
        CTRL_R3 = 1u << 15,
    };

    struct PadState
    {
        unsigned int buttons = 0;
        unsigned char lx = 128, ly = 128, rx = 128, ry = 128;
    };

    SDL_GameController *g_controllers[2] = {nullptr, nullptr};
    PadState g_pad{};

    // Keyboard: the raylib key codes of raylib.h through SDL2.
    struct KeyMap
    {
        int key;
        SDL_Keycode code;
    };
        constexpr KeyMap kKeyMap[] = {
        {KEY_APOSTROPHE, SDLK_QUOTE},
        {KEY_COMMA, SDLK_COMMA},
        {KEY_MINUS, SDLK_MINUS},
        {KEY_PERIOD, SDLK_PERIOD},
        {KEY_SLASH, SDLK_SLASH},
        {KEY_ZERO, SDLK_0},
        {KEY_ONE, SDLK_1},
        {KEY_TWO, SDLK_2},
        {KEY_THREE, SDLK_3},
        {KEY_FOUR, SDLK_4},
        {KEY_FIVE, SDLK_5},
        {KEY_SIX, SDLK_6},
        {KEY_SEVEN, SDLK_7},
        {KEY_EIGHT, SDLK_8},
        {KEY_NINE, SDLK_9},
        {KEY_SEMICOLON, SDLK_SEMICOLON},
        {KEY_EQUAL, SDLK_EQUALS},
        {KEY_A, SDLK_a},
        {KEY_B, SDLK_b},
        {KEY_C, SDLK_c},
        {KEY_D, SDLK_d},
        {KEY_E, SDLK_e},
        {KEY_F, SDLK_f},
        {KEY_G, SDLK_g},
        {KEY_H, SDLK_h},
        {KEY_I, SDLK_i},
        {KEY_J, SDLK_j},
        {KEY_K, SDLK_k},
        {KEY_L, SDLK_l},
        {KEY_M, SDLK_m},
        {KEY_N, SDLK_n},
        {KEY_O, SDLK_o},
        {KEY_P, SDLK_p},
        {KEY_Q, SDLK_q},
        {KEY_R, SDLK_r},
        {KEY_S, SDLK_s},
        {KEY_T, SDLK_t},
        {KEY_U, SDLK_u},
        {KEY_V, SDLK_v},
        {KEY_W, SDLK_w},
        {KEY_X, SDLK_x},
        {KEY_Y, SDLK_y},
        {KEY_Z, SDLK_z},
        {KEY_LEFT_BRACKET, SDLK_LEFTBRACKET},
        {KEY_BACKSLASH, SDLK_BACKSLASH},
        {KEY_RIGHT_BRACKET, SDLK_RIGHTBRACKET},
        {KEY_GRAVE, SDLK_BACKQUOTE},
        {KEY_SPACE, SDLK_SPACE},
        {KEY_ESCAPE, SDLK_ESCAPE},
        {KEY_ENTER, SDLK_RETURN},
        {KEY_TAB, SDLK_TAB},
        {KEY_BACKSPACE, SDLK_BACKSPACE},
        {KEY_INSERT, SDLK_INSERT},
        {KEY_DELETE, SDLK_DELETE},
        {KEY_RIGHT, SDLK_RIGHT},
        {KEY_LEFT, SDLK_LEFT},
        {KEY_DOWN, SDLK_DOWN},
        {KEY_UP, SDLK_UP},
        {KEY_PAGE_UP, SDLK_PAGEUP},
        {KEY_PAGE_DOWN, SDLK_PAGEDOWN},
        {KEY_HOME, SDLK_HOME},
        {KEY_END, SDLK_END},
        {KEY_CAPS_LOCK, SDLK_CAPSLOCK},
        {KEY_SCROLL_LOCK, SDLK_SCROLLLOCK},
        {KEY_NUM_LOCK, SDLK_NUMLOCKCLEAR},
        {KEY_PRINT_SCREEN, SDLK_PRINTSCREEN},
        {KEY_PAUSE, SDLK_PAUSE},
        {KEY_F1, SDLK_F1},
        {KEY_F2, SDLK_F2},
        {KEY_F3, SDLK_F3},
        {KEY_F4, SDLK_F4},
        {KEY_F5, SDLK_F5},
        {KEY_F6, SDLK_F6},
        {KEY_F7, SDLK_F7},
        {KEY_F8, SDLK_F8},
        {KEY_F9, SDLK_F9},
        {KEY_F10, SDLK_F10},
        {KEY_F11, SDLK_F11},
        {KEY_F12, SDLK_F12},
        {KEY_LEFT_SHIFT, SDLK_LSHIFT},
        {KEY_LEFT_CONTROL, SDLK_LCTRL},
        {KEY_LEFT_ALT, SDLK_LALT},
        {KEY_LEFT_SUPER, SDLK_LGUI},
        {KEY_RIGHT_SHIFT, SDLK_RSHIFT},
        {KEY_RIGHT_CONTROL, SDLK_RCTRL},
        {KEY_RIGHT_ALT, SDLK_RALT},
        {KEY_RIGHT_SUPER, SDLK_RGUI},
        {KEY_KP_0, SDLK_KP_0},
        {KEY_KP_1, SDLK_KP_1},
        {KEY_KP_2, SDLK_KP_2},
        {KEY_KP_3, SDLK_KP_3},
        {KEY_KP_4, SDLK_KP_4},
        {KEY_KP_5, SDLK_KP_5},
        {KEY_KP_6, SDLK_KP_6},
        {KEY_KP_7, SDLK_KP_7},
        {KEY_KP_8, SDLK_KP_8},
        {KEY_KP_9, SDLK_KP_9},
        {KEY_KP_DECIMAL, SDLK_KP_DECIMAL},
        {KEY_KP_DIVIDE, SDLK_KP_DIVIDE},
        {KEY_KP_MULTIPLY, SDLK_KP_MULTIPLY},
        {KEY_KP_SUBTRACT, SDLK_KP_MINUS},
        {KEY_KP_ADD, SDLK_KP_PLUS},
        {KEY_KP_ENTER, SDLK_KP_ENTER},
        {KEY_KP_EQUAL, SDLK_KP_EQUALS},
    };
    constexpr size_t kKeyCount = sizeof(kKeyMap) / sizeof(kKeyMap[0]);
    std::array<bool, kKeyCount> g_keyDown{};
    std::array<bool, kKeyCount> g_keyPressed{};

    int keyIndex(int key)
    {
        for (size_t i = 0; i < kKeyCount; ++i)
            if (kKeyMap[i].key == key)
                return static_cast<int>(i);
        return -1;
    }

    unsigned char axisToByte(int16_t v)
    {
        const int b = (int(v) + 32768) * 255 / 65535;
        return static_cast<unsigned char>(std::clamp(b, 0, 255));
    }

    void pollInput()
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        // Events: quit, controller hot-plug.
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_QUIT)
                g_quit = true;
            else if (event.type == SDL_CONTROLLERDEVICEADDED)
            {
                for (int i = 0; i < 2; ++i)
                    if (!g_controllers[i])
                    {
                        g_controllers[i] = SDL_GameControllerOpen(event.cdevice.which);
                        break;
                    }
            }
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED)
            {
                // SDL2: SDL_GameControllerGetDeviceIndex is SDL3-only (no
                // SDL_JoystickDeviceIndex in SDL2 headers), and cdevice.which
                // for a REMOVED event is the joystick instance id (SDL_events.h),
                // so match on the opened controller's joystick instance id.
                for (int i = 0; i < 2; ++i)
                    if (g_controllers[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[i])) == event.cdevice.which)
                    {
                        SDL_GameControllerClose(g_controllers[i]);
                        g_controllers[i] = nullptr;
                    }
            }
        }

        // Keyboard edges (IsKeyPressed is "pressed since the last poll").
        const Uint8 *state = SDL_GetKeyboardState(nullptr);
        for (size_t i = 0; i < kKeyCount; ++i)
        {
            const bool down = state && state[SDL_GetScancodeFromKey(kKeyMap[i].code)];
            g_keyPressed[i] = down && !g_keyDown[i];
            g_keyDown[i] = down;
        }

        // Gamepads: ports 0 and 1 merged (.buttons OR, axes strongest).
        PadState merged{};
        for (SDL_GameController *ctrl : g_controllers)
        {
            if (!ctrl)
                continue;
            auto button = [ctrl](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(ctrl, b) == SDL_TRUE; };
            auto set = [&merged](unsigned int bit, bool v)
            {
                if (v)
                    merged.buttons |= bit;
            };
            set(CTRL_UP, button(SDL_CONTROLLER_BUTTON_DPAD_UP));
            set(CTRL_RIGHT, button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
            set(CTRL_DOWN, button(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
            set(CTRL_LEFT, button(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
            set(CTRL_TRIANGLE, button(SDL_CONTROLLER_BUTTON_Y));
            set(CTRL_CIRCLE, button(SDL_CONTROLLER_BUTTON_B));
            set(CTRL_CROSS, button(SDL_CONTROLLER_BUTTON_A));
            set(CTRL_SQUARE, button(SDL_CONTROLLER_BUTTON_X));
            set(CTRL_L1, button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
            set(CTRL_R1, button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            // Triggers for L2/R2 (SDL exposes them as axes 0..32767).
            if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 4096)
                merged.buttons |= CTRL_L2;
            if (SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 4096)
                merged.buttons |= CTRL_R2;
            set(CTRL_SELECT, button(SDL_CONTROLLER_BUTTON_BACK));
            set(CTRL_START, button(SDL_CONTROLLER_BUTTON_START));
            set(CTRL_L3, button(SDL_CONTROLLER_BUTTON_LEFTSTICK));
            set(CTRL_R3, button(SDL_CONTROLLER_BUTTON_RIGHTSTICK));
            auto pick = [](unsigned char current, int16_t value)
            {
                const unsigned char next = axisToByte(value);
                return std::abs(int(next) - 128) > std::abs(int(current) - 128) ? next : current;
            };
            merged.lx = pick(merged.lx, SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTX));
            merged.ly = pick(merged.ly, SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_LEFTY));
            merged.rx = pick(merged.rx, SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTX));
            merged.ry = pick(merged.ry, SDL_GameControllerGetAxis(ctrl, SDL_CONTROLLER_AXIS_RIGHTY));
        }
        g_pad = merged;
    }

    // Audio
    SDL_AudioDeviceID g_audioDevice = 0;
    int g_audioBufferFrames = 1024;
    std::atomic<AudioCallback> g_audioCallback{nullptr};
    unsigned int g_audioOutRate = 48000; // the device's actual rate (have.freq)

    // One-shot sounds (this file's PlaySound etc., used by ps2_audio.cpp):
    // each owns its decoded s16 PCM and rides on top of the SPU2 stream in
    // audioThunk, the way raylib's PlaySound mixes into its audio thread.
    struct SoundState
    {
        std::vector<int16_t> pcm; // interleaved s16, as decoded
        unsigned int sampleRate = 0;
        unsigned int channels = 0;
        unsigned int frameCount = 0;
        float pitch = 1.0f;
        float volume = 1.0f;
        bool playing = false;
        double position = 0.0; // fractional frame position (pitch steps it)
    };
    std::mutex g_soundMutex;
    std::vector<SoundState *> g_playingSounds; // guarded by g_soundMutex

    SoundState *soundStateOf(Sound sound)
    {
        return static_cast<SoundState *>(sound.stream.buffer);
    }

    // Mixes every playing sound (linear interpolation up to the device rate)
    // additively into the s16 stereo buffer the SPU2 callback just filled.
    // Runs on the audio thread; every SoundState it touches is meanwhile
    // mutated only under g_soundMutex by PlaySound/StopSound/UnloadSound/
    // SetSoundPitch/SetSoundVolume on the game thread.
    void mixSounds(int16_t *out, unsigned int frames)
    {
        std::lock_guard<std::mutex> lock(g_soundMutex);
        const double outRate = g_audioOutRate > 0 ? g_audioOutRate : 48000;
        for (auto it = g_playingSounds.begin(); it != g_playingSounds.end();)
        {
            SoundState *st = *it;
            if (!st->playing)
            {
                it = g_playingSounds.erase(it);
                continue;
            }
            const unsigned int channels = st->channels == 0 ? 1 : st->channels;
            const double frameCount = static_cast<double>(st->frameCount);
            if (st->position >= frameCount)
            {
                st->playing = false;
                it = g_playingSounds.erase(it);
                continue;
            }
            const double step = st->pitch * static_cast<double>(st->sampleRate) / outRate;
            for (unsigned int f = 0; f < frames; ++f)
            {
                if (st->position >= frameCount)
                    break;
                const size_t idx = static_cast<size_t>(st->position);
                const double frac = st->position - static_cast<double>(idx);
                const size_t base = idx * channels;
                const size_t baseNext = (idx + 1 < st->frameCount) ? (idx + 1) * channels : base;
                auto sampleAt = [&st, base, baseNext, frac](unsigned int c) -> double
                {
                    const double cur = st->pcm[base + c];
                    const double nxt = st->pcm[baseNext + c];
                    return cur + (nxt - cur) * frac;
                };
                // Mono duplicates to both outputs; >2 channels take the
                // first two (L/R).
                double l, r;
                if (channels == 1)
                    l = r = sampleAt(0);
                else
                {
                    l = sampleAt(0);
                    r = sampleAt(1);
                }
                const double vol = static_cast<double>(st->volume);
                const int32_t accL = static_cast<int32_t>(out[f * 2]) + static_cast<int32_t>(l * vol);
                const int32_t accR = static_cast<int32_t>(out[f * 2 + 1]) + static_cast<int32_t>(r * vol);
                out[f * 2] = static_cast<int16_t>(std::clamp(accL, -32768, 32767));
                out[f * 2 + 1] = static_cast<int16_t>(std::clamp(accR, -32768, 32767));
                st->position += step;
            }
            if (st->position >= frameCount)
            {
                st->playing = false;
                it = g_playingSounds.erase(it);
            }
            else
                ++it;
        }
    }

    void audioThunk(void *, Uint8 *stream, int bytes)
    {
        const unsigned int frames = static_cast<unsigned int>(bytes) / 4u; // s16 stereo
        if (AudioCallback callback = g_audioCallback.load(std::memory_order_acquire))
            callback(stream, frames);
        else
            std::memset(stream, 0, static_cast<size_t>(bytes));
        // One-shot sounds ride on top of the SPU2 stream.
        mixSounds(reinterpret_cast<int16_t *>(stream), frames);
    }

    // 2D quads on GLES (vitaGL's immediate-mode client arrays do not exist
    // in GLES2/3): one program, position + uv -> texture * color, with the
    // screen transform (glOrtho(0, w, h, 0)) in the vertex shader.
    GLuint g_quadProgram = 0;
    GLint g_quadPos = -1, g_quadUv = -1, g_quadSize = -1, g_quadTexture = -1, g_quadColor = -1, g_quadTonemap = -1;
    GLuint g_whiteTexture = 0; // 1x1 white texel: the texture-less draws

    GLuint compileShader(GLenum type, const char *source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[linux] shader failed: %s\n", log);
        }
        return shader;
    }

    bool initQuadProgram()
    {
        if (!g_glReady)
            return false;
        if (g_quadProgram)
            return true;
        static const char *kVertex =
            "attribute vec2 pos;"
            "attribute vec2 uv;"
            "uniform vec2 size;"
            "varying vec2 vuv;"
            "void main() { vuv = uv; gl_Position = vec4(vec2(pos.x / size.x, 1.0 - pos.y / size.y) * 2.0 - 1.0, 0.0, 1.0); }";
        static const char *kFragment =
            "precision mediump float;\n"
            "varying vec2 vuv;\n"
            "uniform sampler2D tex;\n"
            "uniform vec4 color;\n"
            "uniform int uTonemap;\n"
            "void main() {\n"
            "    vec4 c = texture2D(tex, vuv) * color;\n"
            "    if (uTonemap == 1) {\n"
            "        // Photographic Filmic Tonemapping (Extended Reinhard with Soft Highlight Shoulder)\n"
            "        // 1. Anchors pure black (0.0) with rich shadow contrast\n"
            "        vec3 x = c.rgb;\n"
            "        vec3 col = pow(x, vec3(1.12));\n"
            "        // 2. Luma-preserving filmic compression: smoothly rolls off bright sunbeams,\n"
            "        // specular highlights, and illuminated floors without washing out stone textures\n"
            "        float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));\n"
            "        float toneLuma = (luma * (1.0 + luma / 1.82)) / (1.0 + luma);\n"
            "        col = col * (toneLuma / max(luma, 0.0001));\n"
            "        gl_FragColor = vec4(clamp(col, 0.0, 1.0), c.a);\n"
            "    } else {\n"
            "        gl_FragColor = c;\n"
            "    }\n"
            "}\n";
        const GLuint vs = compileShader(GL_VERTEX_SHADER, kVertex);
        const GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragment);
        g_quadProgram = glCreateProgram();
        glAttachShader(g_quadProgram, vs);
        glAttachShader(g_quadProgram, fs);
        glLinkProgram(g_quadProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(g_quadProgram, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            char log[1024] = {};
            glGetProgramInfoLog(g_quadProgram, sizeof(log), nullptr, log);
            std::fprintf(stderr, "[linux] quad program failed: %s\n", log);
            glDeleteProgram(g_quadProgram);
            g_quadProgram = 0;
            return false;
        }
        g_quadPos = glGetAttribLocation(g_quadProgram, "pos");
        g_quadUv = glGetAttribLocation(g_quadProgram, "uv");
        g_quadSize = glGetUniformLocation(g_quadProgram, "size");
        g_quadTexture = glGetUniformLocation(g_quadProgram, "tex");
        g_quadColor = glGetUniformLocation(g_quadProgram, "color");
        g_quadTonemap = glGetUniformLocation(g_quadProgram, "uTonemap");
        // A 1x1 white texel for the draws without a texture (DrawRectangle).
        const uint8_t white[4] = {255, 255, 255, 255};
        glGenTextures(1, &g_whiteTexture);
        glBindTexture(GL_TEXTURE_2D, g_whiteTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        return true;
    }

    // Immediate quads with client arrays (the Vita's drawQuads, on a
    // GLES program instead of vitaGL's fixed-function path).
    void drawQuads(GLuint texture, const float *xy, const float *uv, int quads, Color color, int tonemap = 0)
    {
        if (!initQuadProgram() || quads <= 0)
            return;
        // Interleaved position + uv (uv-less draws use 0,0 with the white
        // texel, so the color comes out plain).
        std::vector<float> verts;
        verts.reserve(quads * 24);
        for (int q = 0; q < quads; ++q)
        {
            static const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int k : order)
            {
                verts.push_back(xy[q * 8 + k * 2]);
                verts.push_back(xy[q * 8 + k * 2 + 1]);
                verts.push_back(uv ? uv[q * 8 + k * 2] : 0.0f);
                verts.push_back(uv ? uv[q * 8 + k * 2 + 1] : 0.0f);
            }
        }
        GLuint vbo = 0;
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(g_quadPos);
        glVertexAttribPointer(g_quadPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(g_quadUv);
        glVertexAttribPointer(g_quadUv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<const void *>(2 * sizeof(float)));
        glUniform2f(g_quadSize, static_cast<float>(g_width), static_cast<float>(g_height));
        glUniform4f(g_quadColor, color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
        if (g_quadTonemap >= 0)
            glUniform1i(g_quadTonemap, tonemap);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture ? texture : g_whiteTexture);
        glUniform1i(g_quadTexture, 0);
        glDrawArrays(GL_TRIANGLES, 0, quads * 6);
        glDisableVertexAttribArray(g_quadPos);
        glDisableVertexAttribArray(g_quadUv);
        glDeleteBuffers(1, &vbo);
    }

    void rectQuad(float *xy, float x, float y, float w, float h)
    {
        const float v[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
        std::memcpy(xy, v, sizeof(v));
    }
}

void InitWindow(int width, int height, const char *title)
{
    g_width = width;
    g_height = height;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0)
        std::fprintf(stderr, "[linux] SDL_Init failed: %s\n", SDL_GetError());

    // Load external gamecontrollerdb.txt (e.g. PortMaster's database) and
    // hardcode the R36S / GO-Super Gamepad mapping so that the built-in
    // controls are recognized as a standard SDL GameController even when no
    // external db file is present.
    {
        const char *dbEnv = std::getenv("SDL_GAMECONTROLLERCONFIG_FILE");
        if (dbEnv && dbEnv[0])
            SDL_GameControllerAddMappingsFromFile(dbEnv);
        else
            SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
        // R36S / ArkOS built-in pad (Vendor=484b Product=1100).
        SDL_GameControllerAddMapping(
            "190000004b4800000011000000010000,GO-Super Gamepad,"
            "a:b0,b:b1,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,"
            "guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,"
            "leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,"
            "righttrigger:b7,rightx:a2,righty:a3,start:b13,"
            "x:b2,y:b3,platform:Linux,");
    }

    bootMark("window: SDL up");
    // The GLES3 context is SDL's (SDL_GL_CreateContext uses EGL underneath).
    // The GS renderer's shaders are GLSL ES 3.10 (texelFetch), so the context
    // is asked for as ES 3.2 first, then 3.1 and 3.0 when the driver refuses
    // it (the renderer then reports what it got and stays down below 3.10).
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window)
    {
        std::fprintf(stderr, "[linux] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return;
    }
    for (const int minor : {2, 1, 0})
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        g_context = SDL_GL_CreateContext(g_window);
        if (g_context)
            break;
    }
    if (!g_context)
    {
        std::fprintf(stderr, "[linux] SDL_GL_CreateContext (ES 3.2/3.1/3.0) failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return;
    }
    // The context that was granted (once): profile and version, as the
    // renderer's shader set depends on both.
    int glMajor = 0, glMinor = 0, glProfile = 0, glDepth = 0, glStencil = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &glMajor);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &glMinor);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &glProfile);
    SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &glDepth);
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &glStencil);
    std::fprintf(stderr, "[linux] GL context: %s %d.%d (%s), depth %d stencil %d\n", glProfile == SDL_GL_CONTEXT_PROFILE_ES ? "ES" : "compatibility",
                 glMajor, glMinor, reinterpret_cast<const char *>(glGetString(GL_VERSION)), glDepth, glStencil);
    g_glReady = true;
    initQuadProgram();
    glViewport(0, 0, width, height);
    // The screen transform (glOrtho(0, w, h, 0) on the Vita) lives in the
    // quad program's vertex shader; there is no fixed-function state here.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SDL_GL_MakeCurrent(g_window, nullptr);
    bootMark("window: GLES up");

    // Open any game controllers already connected at startup (the built-in
    // pad on handhelds like R36S is always present before SDL_Init).
    for (int i = 0; i < SDL_NumJoysticks() && i < 2; ++i)
    {
        if (SDL_IsGameController(i))
        {
            g_controllers[i] = SDL_GameControllerOpen(i);
            if (g_controllers[i])
                std::fprintf(stderr, "[linux] controller %d: %s\n", i,
                             SDL_GameControllerName(g_controllers[i]));
        }
    }

    g_startCounter = SDL_GetPerformanceCounter();
}

void CloseWindow(void)
{
    {
        GlLock lock;
        for (auto &[id, slot] : g_textures)
            if (slot.texture)
                glDeleteTextures(1, &slot.texture);
        g_textures.clear();
        if (g_whiteTexture)
            glDeleteTextures(1, &g_whiteTexture);
        g_whiteTexture = 0;
        if (g_quadProgram)
            glDeleteProgram(g_quadProgram);
        g_quadProgram = 0;
        g_glReady = false;
    }
    if (g_audioDevice != 0)
    {
        SDL_CloseAudioDevice(g_audioDevice);
        g_audioDevice = 0;
    }
    for (SDL_GameController *&ctrl : g_controllers)
    {
        if (ctrl)
            SDL_GameControllerClose(ctrl);
        ctrl = nullptr;
    }
    if (g_context)
        SDL_GL_DeleteContext(g_context);
    g_context = nullptr;
    if (g_window)
        SDL_DestroyWindow(g_window);
    g_window = nullptr;
    SDL_Quit();
}

bool WindowShouldClose(void) { return g_quit; }
bool IsWindowReady(void) { return g_glReady; }
void SetConfigFlags(unsigned int) {}
// Presentation follows the display's vsync: 30 or less shows each picture
// for at least two vblanks (interval 2), else every vblank.
void SetTargetFPS(int fps) { g_swapInterval.store(fps > 0 && fps <= 30 ? 2 : 1, std::memory_order_relaxed); }
int GetFPS(void) { return g_fps; }

double GetTime(void)
{
    if (g_startCounter == 0)
        g_startCounter = SDL_GetPerformanceCounter();
    return static_cast<double>(SDL_GetPerformanceCounter() - g_startCounter) /
           static_cast<double>(SDL_GetPerformanceFrequency());
}

float GetFrameTime(void) { return static_cast<float>(g_frameTime); }

int GetScreenWidth(void) { return g_width; }
int GetScreenHeight(void) { return g_height; }

void PollInputEvents(void) { pollInput(); }

void BeginDrawing(void)
{
    pollInput();
    ps2xLinuxGlLock(); // released by EndDrawing
    const double now = GetTime();
    if (g_lastFrameTime > 0.0)
        g_frameTime = now - g_lastFrameTime;
    g_lastFrameTime = now;
    if (!initQuadProgram())
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(g_quadProgram);
    glDepthRangef(0.0f, 1.0f);
    glDepthMask(GL_TRUE);
    glViewport(0, 0, g_width, g_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
}

void EndDrawing(void)
{
    if (g_glReady && g_window)
    {
        int wantSwap = g_swapInterval.load(std::memory_order_relaxed);
        if (const char *envSwap = std::getenv("PS2X_SWAP_INTERVAL"))
        {
            wantSwap = std::atoi(envSwap);
        }
        if (wantSwap != g_appliedSwapInterval)
        {
            SDL_GL_SetSwapInterval(wantSwap);
            g_appliedSwapInterval = wantSwap;
        }
        SDL_GL_SwapWindow(g_window);
    }
    static bool firstShown = false;
    if (!firstShown)
    {
        firstShown = true;
        bootMark("first picture on screen");
    }
    ps2xLinuxGlUnlock();
    std::fflush(stdout);
    std::fflush(stderr);
    ++g_framesThisSecond;
    const double now = GetTime();
    if (now - g_fpsWindowStart >= 1.0)
    {
        g_fps = g_framesThisSecond;
        g_framesThisSecond = 0;
        g_fpsWindowStart = now;
    }
}

void ClearBackground(Color color)
{
    if (!g_glReady)
        return;
    glClearColor(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2, float, Color tint)
{
    // As raylib: a negative source height flips the picture vertically.
    if (source.height < 0.0f)
        source.y -= source.height;
    if (texture.id & 0x80000000u)
    {
        // A GL texture name (the native renderer's frame): opaque, drawn as is.
        if (texture.width <= 0 || texture.height <= 0)
            return;
        float xy[8], uv[8];
        rectQuad(xy, dest.x, dest.y, dest.width, dest.height);
        rectQuad(uv, source.x / texture.width, source.y / texture.height, source.width / texture.width, source.height / texture.height);
        glDisable(GL_BLEND);
        static const int tonemapMode = [] {
            const char *env = std::getenv("SOTC_TONEMAP");
            return env ? std::atoi(env) : 0; // Default to 0 (direct clean presentation), 1 = modern single-pass
        }();
        drawQuads(texture.id & 0x7FFFFFFFu, xy, uv, 1, tint, tonemapMode);
        glEnable(GL_BLEND);
        return;
    }
    const auto it = g_textures.find(texture.id);
    if (it == g_textures.end() || !it->second.texture || texture.width <= 0 || texture.height <= 0)
        return;
    float xy[8], uv[8];
    rectQuad(xy, dest.x, dest.y, dest.width, dest.height);
    rectQuad(uv, source.x / texture.width, source.y / texture.height, source.width / texture.width, source.height / texture.height);
    drawQuads(it->second.texture, xy, uv, 1, tint);
}

void DrawRectangle(int posX, int posY, int width, int height, Color color)
{
    float xy[8];
    rectQuad(xy, static_cast<float>(posX), static_cast<float>(posY), static_cast<float>(width), static_cast<float>(height));
    drawQuads(0, xy, nullptr, 1, color);
}

namespace
{
#include "default_font.inc"

    // raylib's default font as a 128x128 atlas, laid out the way LoadFontDefault does.
    struct GlyphRect
    {
        int x, y, w, h;
    };
    GLuint g_fontTexture = 0;
    GlyphRect g_glyphs[224];

    bool ensureFont()
    {
        if (g_fontTexture)
            return true;
        if (!g_glReady)
            return false;
        GlLock lock;
        if (g_fontTexture)
            return true;
        std::vector<uint32_t> pixels(128 * 128);
        for (int i = 0, counter = 0; i < 128 * 128; i += 32, ++counter)
            for (int j = 31; j >= 0; --j)
                pixels[i + j] = (kDefaultFontBits[counter] & (1u << j)) ? 0xFFFFFFFFu : 0x00FFFFFFu;
        glGenTextures(1, &g_fontTexture);
        glBindTexture(GL_TEXTURE_2D, g_fontTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 128, 128, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        constexpr int kHeight = 10, kDivisor = 1;
        int line = 0, posX = kDivisor, testX = kDivisor;
        for (int i = 0; i < 224; ++i)
        {
            const int w = kDefaultFontWidths[i];
            g_glyphs[i] = GlyphRect{posX, kDivisor + line * (kHeight + kDivisor), w, kHeight};
            testX += w + kDivisor;
            if (testX >= 128)
            {
                ++line;
                posX = 2 * kDivisor + w;
                testX = posX;
                g_glyphs[i].x = kDivisor;
                g_glyphs[i].y = kDivisor + line * (kHeight + kDivisor);
            }
            else
                posX = testX;
        }
        return true;
    }

    // UTF-8 -> code point (Latin-1 range is all the font has; others -> '?').
    int nextCodepoint(const char *&p)
    {
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c < 0x80)
            return c;
        if ((c & 0xE0) == 0xC0 && (static_cast<unsigned char>(*p) & 0xC0) == 0x80)
            return ((c & 0x1F) << 6) | (static_cast<unsigned char>(*p++) & 0x3F);
        while ((static_cast<unsigned char>(*p) & 0xC0) == 0x80)
            ++p;
        return '?';
    }

    int glyphIndex(int codepoint)
    {
        return (codepoint >= 32 && codepoint < 256) ? codepoint - 32 : '?' - 32;
    }
}

// raylib's DrawText: default font scaled to fontSize, fontSize / 10 spacing.
void DrawText(const char *text, int posX, int posY, int fontSize, Color color)
{
    if (!text || !ensureFont())
        return;
    fontSize = std::max(fontSize, 10);
    const float scale = fontSize / 10.0f;
    const int spacing = fontSize / 10;
    std::vector<float> xy, uv;
    float x = static_cast<float>(posX);
    int y = posY;
    for (const char *p = text; *p;)
    {
        const int cp = nextCodepoint(p);
        if (cp == '\n')
        {
            x = static_cast<float>(posX);
            y += fontSize + fontSize / 2;
            continue;
        }
        const GlyphRect &src = g_glyphs[glyphIndex(cp)];
        if (cp != ' ')
        {
            float q[8], t[8];
            rectQuad(q, x, static_cast<float>(y), src.w * scale, static_cast<float>(fontSize));
            rectQuad(t, src.x / 128.0f, src.y / 128.0f, src.w / 128.0f, src.h / 128.0f);
            xy.insert(xy.end(), q, q + 8);
            uv.insert(uv.end(), t, t + 8);
        }
        x += src.w * scale + spacing;
    }
    drawQuads(g_fontTexture, xy.data(), uv.data(), static_cast<int>(xy.size() / 8), color);
}

int MeasureText(const char *text, int fontSize)
{
    if (!text)
        return 0;
    ensureFont();
    fontSize = std::max(fontSize, 10);
    const float scale = fontSize / 10.0f;
    const int spacing = fontSize / 10;
    float width = 0.0f, best = 0.0f;
    int glyphs = 0;
    for (const char *p = text; *p;)
    {
        const int cp = nextCodepoint(p);
        if (cp == '\n')
        {
            best = std::max(best, width);
            width = 0.0f;
            glyphs = 0;
            continue;
        }
        width += kDefaultFontWidths[glyphIndex(cp)] * scale + (glyphs++ ? spacing : 0);
    }
    return static_cast<int>(std::max(best, width));
}

Image GenImageColor(int width, int height, Color color)
{
    Image image{};
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    auto *pixels = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(width) * height * 4u));
    for (int i = 0; pixels && i < width * height; ++i)
        std::memcpy(pixels + i * 4, &color, 4);
    image.data = pixels;
    return image;
}

void ImageCrop(Image *image, Rectangle crop)
{
    if (!image || !image->data)
        return;
    const int x = std::clamp(static_cast<int>(crop.x), 0, image->width);
    const int y = std::clamp(static_cast<int>(crop.y), 0, image->height);
    const int w = std::clamp(static_cast<int>(crop.width), 0, image->width - x);
    const int h = std::clamp(static_cast<int>(crop.height), 0, image->height - y);
    auto *out = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(w) * h * 4u));
    const auto *in = static_cast<const uint8_t *>(image->data);
    for (int row = 0; out && row < h; ++row)
        std::memcpy(out + row * w * 4, in + ((y + row) * image->width + x) * 4, static_cast<size_t>(w) * 4u);
    std::free(image->data);
    image->data = out;
    image->width = w;
    image->height = h;
}

void UnloadImage(Image image) { std::free(image.data); }
bool ExportImage(Image, const char *) { return false; }

Texture2D LoadTextureFromImage(Image image)
{
    Texture2D texture{};
    if (!g_glReady)
        return texture;
    GlLock lock;
    TextureSlot slot;
    glGenTextures(1, &slot.texture);
    glBindTexture(GL_TEXTURE_2D, slot.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texture.id = g_nextTexture++;
    texture.width = image.width;
    texture.height = image.height;
    texture.mipmaps = 1;
    texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    g_textures[texture.id] = std::move(slot);
    if (image.data)
        UpdateTexture(texture, image.data);
    return texture;
}

Image LoadImageFromTexture(Texture2D texture)
{
    Image image = GenImageColor(texture.width, texture.height, BLANK);
    const auto it = g_textures.find(texture.id);
    if (it != g_textures.end() && image.data && it->second.pixels.size() == static_cast<size_t>(texture.width) * texture.height * 4u)
        std::memcpy(image.data, it->second.pixels.data(), it->second.pixels.size());
    return image;
}

void UpdateTexture(Texture2D texture, const void *pixels)
{
    const auto it = g_textures.find(texture.id);
    if (it == g_textures.end() || !it->second.texture || !pixels)
        return;
    GlLock lock;
    glBindTexture(GL_TEXTURE_2D, it->second.texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture.width, texture.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    // Kept for LoadImageFromTexture (the Vita's UpdateTexture drops it,
    // so its copies always come out BLANK).
    it->second.pixels.assign(static_cast<const uint8_t *>(pixels), static_cast<const uint8_t *>(pixels) + static_cast<size_t>(texture.width) * texture.height * 4u);
}

void UnloadTexture(Texture2D texture)
{
    const auto it = g_textures.find(texture.id);
    if (it == g_textures.end())
        return;
    GlLock lock;
    if (it->second.texture)
        glDeleteTextures(1, &it->second.texture);
    g_textures.erase(it);
}

void SetTextureFilter(Texture2D texture, int filter)
{
    if (!g_glReady)
        return;
    GlLock lock;
    // GLES3 here has no trilinear or anisotropic filtering: both degrade to GL_LINEAR.
    const GLint glFilter = (filter == TEXTURE_FILTER_POINT) ? GL_NEAREST : GL_LINEAR;
    GLuint glTexture = 0;
    if (texture.id & 0x80000000u)
    {
        // A raw GL texture name (the native renderer's frame), as DrawTexturePro takes it.
        glTexture = texture.id & 0x7FFFFFFFu;
    }
    else
    {
        const auto it = g_textures.find(texture.id);
        if (it == g_textures.end() || !it->second.texture)
            return;
        glTexture = it->second.texture;
    }
    glBindTexture(GL_TEXTURE_2D, glTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, glFilter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, glFilter);
}

bool IsKeyDown(int key)
{
    const int i = keyIndex(key);
    return i >= 0 && g_keyDown[static_cast<size_t>(i)];
}

bool IsKeyPressed(int key)
{
    const int i = keyIndex(key);
    return i >= 0 && g_keyPressed[static_cast<size_t>(i)];
}

bool IsGamepadAvailable(int gamepad) { return gamepad == 0; }

bool IsGamepadButtonDown(int gamepad, int button)
{
    if (gamepad != 0)
        return false;
    const unsigned int b = g_pad.buttons;
    switch (button)
    {
    case GAMEPAD_BUTTON_LEFT_FACE_UP: return b & CTRL_UP;
    case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return b & CTRL_RIGHT;
    case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return b & CTRL_DOWN;
    case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return b & CTRL_LEFT;
    case GAMEPAD_BUTTON_RIGHT_FACE_UP: return b & CTRL_TRIANGLE;
    case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return b & CTRL_CIRCLE;
    case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return b & CTRL_CROSS;
    case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return b & CTRL_SQUARE;
    case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return b & CTRL_L1;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return b & CTRL_R1;
    case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return b & CTRL_L2;
    case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return b & CTRL_R2;
    case GAMEPAD_BUTTON_MIDDLE_LEFT: return b & CTRL_SELECT;
    case GAMEPAD_BUTTON_MIDDLE_RIGHT: return b & CTRL_START;
    case GAMEPAD_BUTTON_LEFT_THUMB: return b & CTRL_L3;
    case GAMEPAD_BUTTON_RIGHT_THUMB: return b & CTRL_R3;
    default: return false;
    }
}

float GetGamepadAxisMovement(int gamepad, int axis)
{
    if (gamepad != 0)
        return 0.0f;
    auto norm = [](unsigned char v)
    {
        const float f = (static_cast<float>(v) - 128.0f) / 127.0f;
        return std::clamp(f, -1.0f, 1.0f);
    };
    switch (axis)
    {
    case GAMEPAD_AXIS_LEFT_X: return norm(g_pad.lx);
    case GAMEPAD_AXIS_LEFT_Y: return norm(g_pad.ly);
    case GAMEPAD_AXIS_RIGHT_X: return norm(g_pad.rx);
    case GAMEPAD_AXIS_RIGHT_Y: return norm(g_pad.ry);
    default: return 0.0f;
    }
}

void InitAudioDevice(void)
{
    if (g_audioDevice != 0)
        return;
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = static_cast<Uint16>(g_audioBufferFrames);
    want.callback = audioThunk;
    g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_audioDevice != 0 && have.freq > 0)
        g_audioOutRate = static_cast<unsigned int>(have.freq);
    if (g_audioDevice == 0)
        std::fprintf(stderr, "[linux] SDL audio failed: %s\n", SDL_GetError());
}

void CloseAudioDevice(void)
{
    if (g_audioDevice != 0)
        SDL_CloseAudioDevice(g_audioDevice);
    g_audioDevice = 0;
}

bool IsAudioDeviceReady(void) { return g_audioDevice != 0; }
void SetAudioStreamBufferSizeDefault(int size) { g_audioBufferFrames = std::max(256, size); }

AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels)
{
    AudioStream stream{};
    stream.sampleRate = sampleRate;
    stream.sampleSize = sampleSize;
    stream.channels = channels;
    return stream;
}

void SetAudioStreamCallback(AudioStream, AudioCallback callback) { g_audioCallback.store(callback, std::memory_order_release); }

void PlayAudioStream(AudioStream)
{
    if (g_audioDevice != 0)
        SDL_PauseAudioDevice(g_audioDevice, 0);
}

// One-shot sounds (this shim's counterpart of raylib's PlaySound family,
// used by ps2_audio.cpp on the non-Vita path). A Sound holds a SoundState* in
// stream.buffer; LoadSoundFromWave deep-copies the wave's PCM, so UnloadWave
// may run right after. Every function is null-safe on a failed load.

Wave LoadWaveFromMemory(const char *fileType, const void *fileData, int dataSize)
{
    Wave wave{};
    if (!fileType || !fileData || dataSize < 44)
        return wave;
    // Only .wav is decoded here (the sole caller, ps2_audio.cpp, builds WAV
    // bytes in memory itself): TODO(r36s) .ogg/.mp3/.qoa support.
    if (std::strcmp(fileType, ".wav") != 0 && std::strcmp(fileType, "wav") != 0)
        return wave;
    const auto *data = static_cast<const uint8_t *>(fileData);
    auto u32 = [&data](int off) -> uint32_t
    {
        return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) | (static_cast<uint32_t>(data[off + 3]) << 24);
    };
    auto u16 = [&data](int off) -> unsigned int
    {
        return static_cast<unsigned int>(data[off]) | (static_cast<unsigned int>(data[off + 1]) << 8);
    };
    if (std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return wave;
    unsigned int fmtFormat = 0, fmtChannels = 0, fmtRate = 0, fmtBits = 0;
    int dataOff = -1;
    unsigned int dataLen = 0;
    for (int off = 12; off + 8 <= dataSize;)
    {
        const unsigned int chunkSize = u32(off + 4);
        if (std::memcmp(data + off, "fmt ", 4) == 0 && chunkSize >= 16 && off + 8 + 16 <= dataSize)
        {
            fmtFormat = u16(off + 8);
            fmtChannels = u16(off + 10);
            fmtRate = u32(off + 12);
            fmtBits = u16(off + 22);
        }
        else if (std::memcmp(data + off, "data", 4) == 0)
        {
            dataOff = off + 8;
            dataLen = chunkSize;
            break;
        }
        off += 8 + static_cast<int>(chunkSize) + static_cast<int>(chunkSize & 1u);
    }
    if (dataOff < 0 || fmtFormat != 1 || fmtChannels == 0 || fmtRate == 0 || (fmtBits != 8 && fmtBits != 16))
        return wave;
    if (static_cast<unsigned int>(dataOff) + dataLen > static_cast<unsigned int>(dataSize))
        dataLen = static_cast<unsigned int>(dataSize) - static_cast<unsigned int>(dataOff);
    const unsigned int bytesPerFrame = fmtChannels * (fmtBits / 8);
    if (bytesPerFrame == 0 || dataLen < bytesPerFrame)
        return wave;
    wave.data = std::malloc(static_cast<size_t>(dataLen));
    if (!wave.data)
        return wave;
    std::memcpy(wave.data, data + dataOff, static_cast<size_t>(dataLen));
    wave.frameCount = dataLen / bytesPerFrame;
    wave.sampleRate = fmtRate;
    wave.sampleSize = fmtBits;
    wave.channels = fmtChannels;
    return wave;
}

void UnloadWave(Wave wave) { std::free(wave.data); }

Sound LoadSoundFromWave(Wave wave)
{
    Sound sound{};
    if (!wave.data || wave.frameCount == 0 || wave.sampleRate == 0 || wave.channels == 0)
        return sound;
    if (wave.sampleSize != 8 && wave.sampleSize != 16)
        return sound;
    auto *st = new SoundState();
    st->sampleRate = wave.sampleRate;
    st->channels = wave.channels;
    st->frameCount = wave.frameCount;
    st->pcm.resize(static_cast<size_t>(wave.frameCount) * wave.channels);
    const auto *in = static_cast<const uint8_t *>(wave.data);
    if (wave.sampleSize == 16)
        std::memcpy(st->pcm.data(), in, st->pcm.size() * sizeof(int16_t));
    else // 8-bit unsigned -> s16
        for (size_t i = 0; i < st->pcm.size(); ++i)
            st->pcm[i] = static_cast<int16_t>((static_cast<int>(in[i]) - 128) << 8);
    sound.stream.buffer = st;
    sound.stream.processor = nullptr;
    sound.stream.sampleRate = wave.sampleRate;
    sound.stream.sampleSize = 16;
    sound.stream.channels = wave.channels;
    sound.frameCount = wave.frameCount;
    return sound;
}

void UnloadSound(Sound sound)
{
    SoundState *st = soundStateOf(sound);
    if (!st)
        return;
    std::lock_guard<std::mutex> lock(g_soundMutex);
    g_playingSounds.erase(std::remove(g_playingSounds.begin(), g_playingSounds.end(), st), g_playingSounds.end());
    delete st;
}

void PlaySound(Sound sound)
{
    SoundState *st = soundStateOf(sound);
    if (!st)
        return;
    {
        std::lock_guard<std::mutex> lock(g_soundMutex);
        st->position = 0.0;
        st->playing = true;
        g_playingSounds.erase(std::remove(g_playingSounds.begin(), g_playingSounds.end(), st), g_playingSounds.end());
        g_playingSounds.push_back(st);
    }
    if (g_audioDevice != 0)
        SDL_PauseAudioDevice(g_audioDevice, 0);
}

void StopSound(Sound sound)
{
    SoundState *st = soundStateOf(sound);
    if (!st)
        return;
    std::lock_guard<std::mutex> lock(g_soundMutex);
    st->playing = false;
    st->position = 0.0;
    g_playingSounds.erase(std::remove(g_playingSounds.begin(), g_playingSounds.end(), st), g_playingSounds.end());
}

void SetSoundVolume(Sound sound, float volume)
{
    if (SoundState *st = soundStateOf(sound))
    {
        std::lock_guard<std::mutex> lock(g_soundMutex);
        st->volume = std::clamp(volume, 0.0f, 1.0f);
    }
}

void SetSoundPitch(Sound sound, float pitch)
{
    if (SoundState *st = soundStateOf(sound))
    {
        std::lock_guard<std::mutex> lock(g_soundMutex);
        st->pitch = std::clamp(pitch, 0.0f, 8.0f);
    }
}

bool IsSoundPlaying(Sound sound)
{
    SoundState *st = soundStateOf(sound);
    if (!st)
        return false;
    std::lock_guard<std::mutex> lock(g_soundMutex);
    return st->playing;
}

// ---------------------------------------------------------------------------
// Input / misc functions declared in the shim but not yet implemented on the
// Linux SDL2 shim. Minimal functional stubs with a safe return value, so no
// symbol is left undefined. Upgrade each to a real implementation as needed.
// TODO(r36s): wire these to SDL2/GLES.
int GetKeyPressed(void) { return 0; }
int GetCharPressed(void) { return 0; }
Vector2 GetMousePosition(void) { return Vector2{0.0f, 0.0f}; }
Vector2 GetMouseDelta(void) { return Vector2{0.0f, 0.0f}; }
float GetMouseWheelMove(void) { return 0.0f; }
void SetMousePosition(int x, int y) { (void)x; (void)y; }
bool IsMouseButtonDown(int button) { (void)button; return false; }
bool IsMouseButtonPressed(int button) { (void)button; return false; }
bool IsGamepadButtonPressed(int gamepad, int button) { (void)gamepad; (void)button; return false; }

// Misc
void TraceLog(int msgType, const char *text, ...) { (void)msgType; (void)text; }
int GetRandomValue(int min, int max) { (void)max; return min; }
void SetRandomSeed(unsigned int seed) { (void)seed; }
Color ColorFromHSV(float hue, float saturation, float value) { (void)hue; (void)saturation; (void)value; return WHITE; }
Color ColorAlpha(Color color, float alpha) { color.a = static_cast<unsigned char>(alpha * 255.0f); return color; }
bool CheckCollisionRecs(Rectangle rec1, Rectangle rec2) { (void)rec1; (void)rec2; return false; }
void TakeScreenshot(const char *fileName) { (void)fileName; }
const char *GetClipboardText(void) { return nullptr; }
void SetClipboardText(const char *text) { (void)text; }
float Fade(Color color, float alpha) { (void)color; (void)alpha; return 1.0f; }