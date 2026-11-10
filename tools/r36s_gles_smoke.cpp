// R36S SDL2/GLES smoke test.
//
// Build:
//   c++ -std=c++20 tools/r36s_gles_smoke.cpp -o r36s_gles_smoke \
//       $(pkg-config --cflags --libs sdl2) -lGLESv2
//
// Run on the device:
//   SDL_VIDEODRIVER=kmsdrm ./r36s_gles_smoke
//
// Tests the same independent hidden-window/context arrangement used by the
// Linux native renderer. It does not start Celosia or require game assets.

#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include <cstdio>
#include <initializer_list>

static GLuint compileShader(GLenum type, const char *source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE)
    {
        char log[2048] = {};
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        std::fprintf(stderr, "[smoke] GLSL compile failed: %.*s\n",
                     static_cast<int>(length), log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool linkProgram()
{
    static constexpr char vertex[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "void main() { gl_Position = vec4(0.0, 0.0, 0.0, 1.0); }\n";
    static constexpr char fragment[] =
        "#version 310 es\n"
        "precision highp float;\n"
        "out vec4 color;\n"
        "void main() { color = vec4(1.0); }\n";

    const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertex);
    const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragment);
    if (!vertexShader || !fragmentShader)
    {
        if (vertexShader)
            glDeleteShader(vertexShader);
        if (fragmentShader)
            glDeleteShader(fragmentShader);
        return false;
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
    {
        char log[2048] = {};
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        std::fprintf(stderr, "[smoke] GLSL link failed: %.*s\n",
                     static_cast<int>(length), log);
    }

    glDeleteProgram(program);
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    return linked == GL_TRUE;
}

static bool testRendererContext()
{
    if (!linkProgram())
        return false;

    GLuint fbo = 0, color = 0, depth = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, color, 0);

    glGenTextures(1, &depth);
    glBindTexture(GL_TEXTURE_2D, depth);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, 32, 32, 0,
                 GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_TEXTURE_2D, depth, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_TEXTURE_2D, depth, 0);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE)
        std::fprintf(stderr, "[smoke] GLES depth/stencil FBO incomplete: 0x%04x\n", status);

    glViewport(0, 0, 32, 32);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepthf(1.0f);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glFinish();

    const GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        std::fprintf(stderr, "[smoke] GLES error after FBO clear: 0x%04x\n", error);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteTextures(1, &depth);
    glDeleteTextures(1, &color);
    glDeleteFramebuffers(1, &fbo);
    return status == GL_FRAMEBUFFER_COMPLETE && error == GL_NO_ERROR;
}

int main()
{
    SDL_Window *hostWindow = nullptr;
    SDL_Window *rendererWindow = nullptr;
    SDL_GLContext hostContext = nullptr;
    SDL_GLContext rendererContext = nullptr;
    int result = 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "[smoke] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    hostWindow = SDL_CreateWindow("Celosia GLES smoke",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  640, 480, SDL_WINDOW_OPENGL);
    if (!hostWindow)
    {
        std::fprintf(stderr, "[smoke] host window creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    for (const int minor : {2, 1})
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
        hostContext = SDL_GL_CreateContext(hostWindow);
        if (hostContext)
            break;
    }
    if (!hostContext)
    {
        std::fprintf(stderr, "[smoke] ES 3.1 host context creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    {
        const char *driver = SDL_GetCurrentVideoDriver();
        const auto *version = glGetString(GL_VERSION);
        const auto *glsl = glGetString(GL_SHADING_LANGUAGE_VERSION);
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        std::printf("[smoke] SDL video driver: %s\n", driver ? driver : "unknown");
        std::printf("[smoke] host context: ES %d.%d | %s | GLSL %s\n",
                    major, minor,
                    version ? reinterpret_cast<const char *>(version) : "unknown",
                    glsl ? reinterpret_cast<const char *>(glsl) : "unknown");
        if (major < 3 || (major == 3 && minor < 1))
        {
            std::fprintf(stderr, "[smoke] native renderer requires GLES 3.1 or newer\n");
            goto cleanup;
        }
    }

    rendererWindow = SDL_CreateWindow("Celosia renderer context",
                                      SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                      16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!rendererWindow)
    {
        std::fprintf(stderr, "[smoke] hidden renderer window creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    if (SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1) != 0)
    {
        std::fprintf(stderr, "[smoke] could not enable context sharing: %s\n", SDL_GetError());
        goto cleanup;
    }
    rendererContext = SDL_GL_CreateContext(rendererWindow);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!rendererContext)
    {
        std::fprintf(stderr, "[smoke] renderer context creation failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    // A texture created in the renderer context must be visible to the host
    // context, because DrawTexturePro presents renderer-owned GL textures.
    GLuint sharedTexture = 0;
    glGenTextures(1, &sharedTexture);
    glBindTexture(GL_TEXTURE_2D, sharedTexture);
    if (SDL_GL_MakeCurrent(hostWindow, hostContext) != 0 ||
        glIsTexture(sharedTexture) != GL_TRUE)
    {
        std::fprintf(stderr, "[smoke] renderer and host contexts do not share GL textures\n");
        goto cleanup;
    }
    glDeleteTextures(1, &sharedTexture);
    if (SDL_GL_MakeCurrent(rendererWindow, rendererContext) != 0)
    {
        std::fprintf(stderr, "[smoke] could not restore renderer context: %s\n", SDL_GetError());
        goto cleanup;
    }
    {
        GLint major = 0, minor = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        std::printf("[smoke] renderer context: ES %d.%d\n", major, minor);
        if (major < 3 || (major == 3 && minor < 1))
            goto cleanup;
    }

    if (!testRendererContext())
        goto cleanup;

    if (SDL_GL_MakeCurrent(hostWindow, hostContext) != 0 ||
        SDL_GL_GetCurrentContext() != hostContext)
    {
        std::fprintf(stderr, "[smoke] could not restore the host context: %s\n", SDL_GetError());
        goto cleanup;
    }

    std::puts("[smoke] PASS: GLES 3.1, shared SDL contexts, GLSL 3.10, depth/stencil FBO");
    result = 0;

cleanup:
    if (hostWindow && hostContext)
        SDL_GL_MakeCurrent(hostWindow, hostContext);
    if (rendererContext)
        SDL_GL_DeleteContext(rendererContext);
    if (rendererWindow)
        SDL_DestroyWindow(rendererWindow);
    if (hostContext)
        SDL_GL_DeleteContext(hostContext);
    if (hostWindow)
        SDL_DestroyWindow(hostWindow);
    SDL_Quit();
    return result;
}
