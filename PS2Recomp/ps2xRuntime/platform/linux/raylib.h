#pragma once

// The subset of the raylib 5.5 API the runtime uses, implemented on SDL2
// (window, input, audio) and GLES3 (drawing, textures) for Linux/AArch64
// (raylib_linux.cpp). Mirror of the Vita's SDL2 shim
// (platform/vita/raylib/raylib.h): same declarations, so the runtime sees
// the same raylib subset on both targets.

#include <cstdint>

#define PS2X_LINUX_SDL2_SHIM 1

typedef struct Vector2
{
    float x, y;
} Vector2;

typedef struct Rectangle
{
    float x, y, width, height;
} Rectangle;

typedef struct Color
{
    unsigned char r, g, b, a;
} Color;

typedef struct Image
{
    void *data;
    int width;
    int height;
    int mipmaps;
    int format;
} Image;

typedef struct Texture
{
    unsigned int id;
    int width;
    int height;
    int mipmaps;
    int format;
} Texture;
typedef Texture Texture2D;

typedef struct AudioStream
{
    void *buffer;
    void *processor;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
} AudioStream;

typedef struct Wave
{
    unsigned int frameCount;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
    void *data;
} Wave;

typedef struct Sound
{
    AudioStream stream;
    unsigned int frameCount;
} Sound;

typedef void (*AudioCallback)(void *bufferData, unsigned int frames);

#define CLITERAL(type) type
#define WHITE CLITERAL(Color){255, 255, 255, 255}
#define BLACK CLITERAL(Color){0, 0, 0, 255}
#define BLANK CLITERAL(Color){0, 0, 0, 0}
#define MAGENTA CLITERAL(Color){255, 0, 255, 255}
#define GREEN CLITERAL(Color){0, 228, 48, 255}
#define RED CLITERAL(Color){230, 41, 55, 255}

enum ConfigFlags
{
    FLAG_VSYNC_HINT                 = 0x00000040,
    FLAG_FULLSCREEN_MODE            = 0x00000002,
    FLAG_WINDOW_RESIZABLE           = 0x00000004,
    FLAG_WINDOW_UNDECORATED         = 0x00000008,
    FLAG_WINDOW_HIDDEN              = 0x00000080,
    FLAG_WINDOW_MINIMIZED           = 0x00000200,
    FLAG_WINDOW_MAXIMIZED           = 0x00000400,
    FLAG_WINDOW_UNFOCUSED           = 0x00000800,
    FLAG_WINDOW_TOPMOST             = 0x00001000,
    FLAG_WINDOW_ALWAYS_RUN          = 0x00000100,
    FLAG_WINDOW_TRANSPARENT         = 0x00000010,
    FLAG_WINDOW_HIGHDPI             = 0x00002000,
    FLAG_WINDOW_MOUSE_PASSTHROUGH   = 0x00004000,
    FLAG_BORDERLESS_WINDOWED_MODE   = 0x00008000,
    FLAG_MSAA_4X_HINT               = 0x00000020,
    FLAG_INTERLACED_HINT            = 0x00010000,
};

enum PixelFormat
{
    PIXELFORMAT_UNCOMPRESSED_GRAYSCALE     = 1,
    PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA    = 2,
    PIXELFORMAT_UNCOMPRESSED_R5G6B5        = 3,
    PIXELFORMAT_UNCOMPRESSED_R8G8B8        = 4,
    PIXELFORMAT_UNCOMPRESSED_R5G5B5A1      = 5,
    PIXELFORMAT_UNCOMPRESSED_R4G4B4A4      = 6,
    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8      = 7,
    PIXELFORMAT_UNCOMPRESSED_R32           = 8,
    PIXELFORMAT_UNCOMPRESSED_R32G32B32     = 9,
    PIXELFORMAT_UNCOMPRESSED_R32G32B32A32  = 10,
    PIXELFORMAT_UNCOMPRESSED_R16           = 11,
    PIXELFORMAT_UNCOMPRESSED_R16G16B16     = 12,
    PIXELFORMAT_UNCOMPRESSED_R16G16B16A16  = 13,
    PIXELFORMAT_COMPRESSED_DXT1_RGB        = 14,
    PIXELFORMAT_COMPRESSED_DXT1_RGBA       = 15,
    PIXELFORMAT_COMPRESSED_DXT3_RGBA       = 16,
    PIXELFORMAT_COMPRESSED_DXT5_RGBA       = 17,
    PIXELFORMAT_COMPRESSED_ETC1_RGB        = 18,
    PIXELFORMAT_COMPRESSED_ETC2_RGB        = 19,
    PIXELFORMAT_COMPRESSED_ETC2_EAC_RGBA   = 20,
    PIXELFORMAT_COMPRESSED_PVRT_RGB        = 21,
    PIXELFORMAT_COMPRESSED_PVRT_RGBA       = 22,
    PIXELFORMAT_COMPRESSED_ASTC_4x4_RGBA   = 23,
    PIXELFORMAT_COMPRESSED_ASTC_8x8_RGBA   = 24,
};

enum TextureFilter
{
    TEXTURE_FILTER_POINT = 0,
    TEXTURE_FILTER_BILINEAR = 1,
    TEXTURE_FILTER_TRILINEAR = 2,
    TEXTURE_FILTER_ANISOTROPIC_4X = 3,
    TEXTURE_FILTER_ANISOTROPIC_8X = 4,
    TEXTURE_FILTER_ANISOTROPIC_16X = 5,
};

// Keyboard through SDL2: the codes the runtime's host menu and pad fallback
// read (host_menu.cpp, ps2_pad.cpp).
enum KeyboardKey {
    KEY_NULL            = 0,
    KEY_APOSTROPHE      = 39,
    KEY_COMMA           = 44,
    KEY_MINUS           = 45,
    KEY_PERIOD          = 46,
    KEY_SLASH           = 47,
    KEY_ZERO            = 48,
    KEY_ONE             = 49,
    KEY_TWO             = 50,
    KEY_THREE           = 51,
    KEY_FOUR            = 52,
    KEY_FIVE            = 53,
    KEY_SIX             = 54,
    KEY_SEVEN           = 55,
    KEY_EIGHT           = 56,
    KEY_NINE            = 57,
    KEY_SEMICOLON       = 59,
    KEY_EQUAL           = 61,
    KEY_A               = 65,
    KEY_B               = 66,
    KEY_C               = 67,
    KEY_D               = 68,
    KEY_E               = 69,
    KEY_F               = 70,
    KEY_G               = 71,
    KEY_H               = 72,
    KEY_I               = 73,
    KEY_J               = 74,
    KEY_K               = 75,
    KEY_L               = 76,
    KEY_M               = 77,
    KEY_N               = 78,
    KEY_O               = 79,
    KEY_P               = 80,
    KEY_Q               = 81,
    KEY_R               = 82,
    KEY_S               = 83,
    KEY_T               = 84,
    KEY_U               = 85,
    KEY_V               = 86,
    KEY_W               = 87,
    KEY_X               = 88,
    KEY_Y               = 89,
    KEY_Z               = 90,
    KEY_LEFT_BRACKET    = 91,
    KEY_BACKSLASH       = 92,
    KEY_RIGHT_BRACKET   = 93,
    KEY_GRAVE           = 96,
    KEY_SPACE           = 32,
    KEY_ESCAPE          = 256,
    KEY_ENTER           = 257,
    KEY_TAB             = 258,
    KEY_BACKSPACE       = 259,
    KEY_INSERT          = 260,
    KEY_DELETE          = 261,
    KEY_RIGHT           = 262,
    KEY_LEFT            = 263,
    KEY_DOWN            = 264,
    KEY_UP              = 265,
    KEY_PAGE_UP         = 266,
    KEY_PAGE_DOWN       = 267,
    KEY_HOME            = 268,
    KEY_END             = 269,
    KEY_CAPS_LOCK       = 280,
    KEY_SCROLL_LOCK     = 281,
    KEY_NUM_LOCK        = 282,
    KEY_PRINT_SCREEN    = 283,
    KEY_PAUSE           = 284,
    KEY_F1              = 290,
    KEY_F2              = 291,
    KEY_F3              = 292,
    KEY_F4              = 293,
    KEY_F5              = 294,
    KEY_F6              = 295,
    KEY_F7              = 296,
    KEY_F8              = 297,
    KEY_F9              = 298,
    KEY_F10             = 299,
    KEY_F11             = 300,
    KEY_F12             = 301,
    KEY_LEFT_SHIFT      = 340,
    KEY_LEFT_CONTROL    = 341,
    KEY_LEFT_ALT        = 342,
    KEY_LEFT_SUPER      = 343,
    KEY_RIGHT_SHIFT     = 344,
    KEY_RIGHT_CONTROL   = 345,
    KEY_RIGHT_ALT       = 346,
    KEY_RIGHT_SUPER     = 347,
    KEY_KB_MENU         = 348,
    KEY_KP_0            = 320,
    KEY_KP_1            = 321,
    KEY_KP_2            = 322,
    KEY_KP_3            = 323,
    KEY_KP_4            = 324,
    KEY_KP_5            = 325,
    KEY_KP_6            = 326,
    KEY_KP_7            = 327,
    KEY_KP_8            = 328,
    KEY_KP_9            = 329,
    KEY_KP_DECIMAL      = 330,
    KEY_KP_DIVIDE       = 331,
    KEY_KP_MULTIPLY     = 332,
    KEY_KP_SUBTRACT     = 333,
    KEY_KP_ADD          = 334,
    KEY_KP_ENTER        = 335,
    KEY_KP_EQUAL        = 336,
    KEY_BACK            = 4,
    KEY_MENU            = 5,
    KEY_VOLUME_UP       = 24,
    KEY_VOLUME_DOWN     = 25
};

enum GamepadButton
{
    GAMEPAD_BUTTON_UNKNOWN = 0,
    GAMEPAD_BUTTON_LEFT_FACE_UP,
    GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
    GAMEPAD_BUTTON_LEFT_FACE_DOWN,
    GAMEPAD_BUTTON_LEFT_FACE_LEFT,
    GAMEPAD_BUTTON_RIGHT_FACE_UP,
    GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
    GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
    GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
    GAMEPAD_BUTTON_LEFT_TRIGGER_1,
    GAMEPAD_BUTTON_LEFT_TRIGGER_2,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
    GAMEPAD_BUTTON_MIDDLE_LEFT,
    GAMEPAD_BUTTON_MIDDLE,
    GAMEPAD_BUTTON_MIDDLE_RIGHT,
    GAMEPAD_BUTTON_LEFT_THUMB,
    GAMEPAD_BUTTON_RIGHT_THUMB,
};

enum MouseButton
{
    MOUSE_BUTTON_LEFT    = 0,
    MOUSE_BUTTON_RIGHT   = 1,
    MOUSE_BUTTON_MIDDLE  = 2,
    MOUSE_BUTTON_SIDE    = 3,
    MOUSE_BUTTON_EXTRA   = 4,
    MOUSE_BUTTON_FORWARD = 5,
    MOUSE_BUTTON_BACK    = 6,
};

typedef enum {
    GAMEPAD_AXIS_LEFT_X = 0,
    GAMEPAD_AXIS_LEFT_Y = 1,
    GAMEPAD_AXIS_RIGHT_X = 2,
    GAMEPAD_AXIS_RIGHT_Y = 3,
    GAMEPAD_AXIS_LEFT_TRIGGER = 4,
    GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
} GamepadAxis;

// Window and drawing
void InitWindow(int width, int height, const char *title);
void CloseWindow(void);
bool WindowShouldClose(void);
bool IsWindowReady(void);
void SetConfigFlags(unsigned int flags);
void SetTargetFPS(int fps);
int GetFPS(void);
double GetTime(void);
float GetFrameTime(void);
int GetScreenWidth(void);
int GetScreenHeight(void);
void BeginDrawing(void);
void PollInputEvents(void);
void EndDrawing(void);
void ClearBackground(Color color);
void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin, float rotation, Color tint);
void DrawRectangle(int posX, int posY, int width, int height, Color color);
void DrawText(const char *text, int posX, int posY, int fontSize, Color color);
int MeasureText(const char *text, int fontSize);

// Images and textures (RGBA8 only)
Image GenImageColor(int width, int height, Color color);
void ImageCrop(Image *image, Rectangle crop);
void UnloadImage(Image image);
bool ExportImage(Image image, const char *fileName);
Texture2D LoadTextureFromImage(Image image);
Image LoadImageFromTexture(Texture2D texture);
void UpdateTexture(Texture2D texture, const void *pixels);
void UnloadTexture(Texture2D texture);
void SetTextureFilter(Texture2D texture, int filter);

// Input
bool IsKeyDown(int key);
bool IsKeyPressed(int key);
bool IsGamepadAvailable(int gamepad);
bool IsGamepadButtonDown(int gamepad, int button);
float GetGamepadAxisMovement(int gamepad, int axis);

// Input (extended)
int GetKeyPressed(void);
int GetCharPressed(void);
Vector2 GetMousePosition(void);
Vector2 GetMouseDelta(void);
float GetMouseWheelMove(void);
void SetMousePosition(int x, int y);
bool IsMouseButtonDown(int button);
bool IsMouseButtonPressed(int button);
bool IsGamepadButtonPressed(int gamepad, int button);

// Misc
void TraceLog(int msgType, const char *text, ...);
int GetRandomValue(int min, int max);
void SetRandomSeed(unsigned int seed);
Color ColorFromHSV(float hue, float saturation, float value);
Color ColorAlpha(Color color, float alpha);
bool CheckCollisionRecs(Rectangle rec1, Rectangle rec2);
void TakeScreenshot(const char *fileName);
const char *GetClipboardText(void);
void SetClipboardText(const char *text);
float Fade(Color color, float alpha);

// Audio (streams with a callback; one-shot sounds for ps2_audio.cpp)// Audio (streams with a callback; one-shot sounds for ps2_audio.cpp)
void InitAudioDevice(void);
void CloseAudioDevice(void);
bool IsAudioDeviceReady(void);
void SetAudioStreamBufferSizeDefault(int size);
AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels);
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback);
void PlayAudioStream(AudioStream stream);
Wave LoadWaveFromMemory(const char *fileType, const void *fileData, int dataSize);
void UnloadWave(Wave wave);
Sound LoadSoundFromWave(Wave wave);
void UnloadSound(Sound sound);
void PlaySound(Sound sound);
void StopSound(Sound sound);
void SetSoundVolume(Sound sound, float volume);
void SetSoundPitch(Sound sound, float pitch);
bool IsSoundPlaying(Sound sound);

// Separate shared SDL GL context for the native renderer's worker thread.
void *CreateNativeGLContext(void);
void MakeGLContextCurrent(void *context);
