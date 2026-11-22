#include "runtime/native_renderer.h"
#include <raylib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#endif

namespace native_renderer
{
namespace
{
    struct BootConfig
    {
        int rendererIdx = 0; // 0 = mali, 1 = gl
        int fpsIdx = 0;      // 0 = 30, 1 = 60, 2 = 0 (Unlimited)
        int drsIdx = 0;      // 0 = 1 (Auto), 1 = 0 (Desligado)
        int tex16Idx = 0;    // 0 = 1 (Ligado), 1 = 0 (Desligado)
        int maliStatsIdx = 0;   // 0 = Off, 1 = On
        int maliAtPass2Idx = 2; // 0 = Off, 1 = Full, 2 = Smart
        int maliMergeIdx = 1;   // 0 = Off, 1 = On
    };

    std::string trimCopy(const std::string &s)
    {
        size_t start = 0;
        while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
            ++start;
        size_t end = s.size();
        while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
            --end;
        return s.substr(start, end - start);
    }

    std::string getConfigPath()
    {
        const char *mcDir = std::getenv("PS2X_MEMCARD_DIR");
        if (mcDir != nullptr && mcDir[0] != '\0')
        {
            return std::string(mcDir) + "/../r36s_boot.cfg";
        }
        return "./r36s_boot.cfg";
    }

    void loadBootConfig(BootConfig &cfg)
    {
        std::ifstream in(getConfigPath());
        if (!in.is_open())
            return;

        std::string line;
        while (std::getline(in, line))
        {
            line = trimCopy(line);
            if (line.empty() || line[0] == '#')
                continue;

            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;

            const std::string key = trimCopy(line.substr(0, eq));
            const std::string val = trimCopy(line.substr(eq + 1));

            if (key == "SOTC_RENDERER")
                cfg.rendererIdx = (val == "gl") ? 1 : 0;
            else if (key == "SOTC_FPS_CAP")
                cfg.fpsIdx = (val == "60") ? 1 : (val == "0" ? 2 : 0);
            else if (key == "SOTC_MALI_DRS")
                cfg.drsIdx = (val == "0") ? 1 : 0;
            else if (key == "SOTC_MALI_TEX16")
                cfg.tex16Idx = (val == "0") ? 1 : 0;
            else if (key == "SOTC_MALI_STATS")
                cfg.maliStatsIdx = (val == "1") ? 1 : 0;
            else if (key == "SOTC_MALI_AT_PASS2")
                cfg.maliAtPass2Idx = (val == "0") ? 0 : (val == "1" ? 1 : 2);
            else if (key == "SOTC_MALI_MERGE")
                cfg.maliMergeIdx = (val == "0") ? 0 : 1;
        }
    }

    void saveBootConfig(const BootConfig &cfg)
    {
        std::ofstream out(getConfigPath(), std::ios::trunc);
        if (!out.is_open())
            return;

        out << "SOTC_RENDERER=" << (cfg.rendererIdx == 0 ? "mali" : "gl") << "\n";
        out << "SOTC_FPS_CAP=" << (cfg.fpsIdx == 0 ? "30" : (cfg.fpsIdx == 1 ? "60") : "0") << "\n";
        out << "SOTC_MALI_DRS=" << (cfg.drsIdx == 0 ? "1" : "0") << "\n";
        out << "SOTC_MALI_TEX16=" << (cfg.tex16Idx == 0 ? "1" : "0") << "\n";
        out << "SOTC_MALI_STATS=" << (cfg.maliStatsIdx == 1 ? "1" : "0") << "\n";
        out << "SOTC_MALI_AT_PASS2=" << cfg.maliAtPass2Idx << "\n";
        out << "SOTC_MALI_MERGE=" << (cfg.maliMergeIdx == 0 ? "0" : "1") << "\n";
    }

    void setEnvVar(const char *name, const char *value, int overwrite)
    {
#if defined(_WIN32)
        if (overwrite == 0 && std::getenv(name) != nullptr)
            return;
        _putenv_s(name, value);
#else
        setenv(name, value, overwrite);
#endif
    }

    void applyBootEnv(const BootConfig &cfg, int overwrite)
    {
        setEnvVar("SOTC_RENDERER", cfg.rendererIdx == 0 ? "mali" : "gl", overwrite);
        setEnvVar("SOTC_FPS_CAP", cfg.fpsIdx == 0 ? "30" :(cfg.fpsIdx == 1 ?  "60" : "0"), overwrite);
        setEnvVar("SOTC_MALI_DRS", cfg.drsIdx == 0 ? "1" : "0", overwrite);
        setEnvVar("SOTC_MALI_TEX16", cfg.tex16Idx == 0 ? "1" : "0", overwrite);
        setEnvVar("SOTC_NATIVE", "1", overwrite);
        setEnvVar("SOTC_NATIVE_GL", "1", overwrite);
        setEnvVar("SOTC_MALI_STATS", cfg.maliStatsIdx == 1 ? "1" : "0", overwrite);
      setEnvVar("SOTC_MALI_AT_PASS2", cfg.maliAtPass2Idx == 0 ? "0" : (cfg.maliAtPass2Idx == 1 ? "1" : "2"), overwrite);
    setEnvVar("SOTC_MALI_MERGE", cfg.maliMergeIdx == 0 ? "0" : "1", overwrite);
}

#if defined(__linux__)
    void drawBoxOutline(int x, int y, int w, int h, Color c)
{
    DrawRectangle(x, y, w, 2, c);
    DrawRectangle(x, y + h - 2, w, 2, c);
    DrawRectangle(x, y, 2, h, c);
    DrawRectangle(x + w - 2, y, 2, h, c);
}
#endif
} // namespace

void runR36sBootMenu()
{
#if !defined(__linux__)
    return;
#endif
    BootConfig cfg;
    loadBootConfig(cfg);

    const char *skipEnv = std::getenv("SOTC_SKIP_BOOT_MENU");
    const bool skipMenu = (skipEnv != nullptr && std::string(skipEnv) == "1");
    if (skipMenu)
    {
        applyBootEnv(cfg, 0);
        return;
    }

#if defined(__linux__)
    if (!skipMenu && IsWindowReady())
    {
        int selectedRow = 0;
        constexpr int kRowCount = 7;
        bool countdownActive = true;
        double remainingSeconds = 5.0;
        bool done = false;

        auto getOptionValue = [&](int row) -> int &
        {
            switch (row)
            {
            case 0:
                return cfg.rendererIdx;
            case 1:
                return cfg.fpsIdx;
            case 2:
                return cfg.drsIdx;
            case 3:
                return cfg.tex16Idx;
        case 4:
            return cfg.maliStatsIdx;
        case 5:
            return cfg.maliAtPass2Idx;
        default:
            return cfg.maliMergeIdx;
            }
        };

        const char *labels[kRowCount] = {
            "Renderer",
            "FPS Cap",
            "Dynamic Res",
            "16-bit Tex",
            "Mali Stats",
            "Mali AT Pass 2",
            "Mali Merge",
        };
        const int choiceCounts[kRowCount] = {2, 3, 2, 2, 2, 3, 2};
        const char *choices[kRowCount][3] = {
            {"Mali GPU", "OpenGL", nullptr},
            {"30", "60", "Unlimited"},
            {"Auto (0.75-1.00)", "Off (1.00)", nullptr},
            {"On", "Off (RGBA8)", nullptr},
            {"Off", "On", nullptr},
            {"Off", "Full", "Smart"},
            {"Off", "On", nullptr},
        };

        int prevAxisDirX[4] = {0, 0, 0, 0};
        int prevAxisDirY[4] = {0, 0, 0, 0};

#ifdef __linux__
        std::vector<int> evdevFds;
        for (int i = 0; i < 8; ++i)
        {
            char devPath[32];
            std::snprintf(devPath, sizeof(devPath), "/dev/input/event%d", i);
            const int fd = ::open(devPath, O_RDONLY | O_NONBLOCK);
            if (fd >= 0)
                evdevFds.push_back(fd);
        }
        bool evSelectDown = false;
        bool evStartDown = false;
        bool evComboTriggered = false;
        int evHatX = 0;
        int evHatY = 0;
#endif

        while (!done && !WindowShouldClose())
        {
            const double dt = static_cast<double>(GetFrameTime());
            if (countdownActive)
            {
                remainingSeconds -= dt;
                if (remainingSeconds <= 0.0)
                {
                    done = true;
                }
            }

            bool upPressed = IsKeyPressed(KEY_UP);
            bool downPressed = IsKeyPressed(KEY_DOWN);
            bool leftPressed = IsKeyPressed(KEY_LEFT);
            bool rightPressed = IsKeyPressed(KEY_RIGHT);
            bool startOnlyPressed = IsKeyPressed(KEY_ENTER);
            bool saveAndStartPressed = false;
            bool fallbackGlPressed = IsKeyPressed(KEY_ESCAPE);
        bool raylibGamepadConnected = false;

            for (int gp = 0; gp < 4; ++gp)
            {
                if (!IsGamepadAvailable(gp))
                    continue;
            raylibGamepadConnected = true;

                // Standard Raylib GAMEPAD_BUTTON_* + raw evdev indices for GO-Super Gamepad (R36S DRM)
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_UP) || IsGamepadButtonPressed(gp, 8))
                    upPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_DOWN) || IsGamepadButtonPressed(gp, 9))
                    downPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_LEFT) || IsGamepadButtonPressed(gp, 10))
                    leftPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_RIGHT) || IsGamepadButtonPressed(gp, 11))
                    rightPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_FACE_DOWN) || IsGamepadButtonPressed(gp, 0))
                    startOnlyPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_RIGHT) || IsGamepadButtonPressed(gp, 13))
                    saveAndStartPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT) || IsGamepadButtonPressed(gp, 1))
                    fallbackGlPressed = true;

                if ((IsGamepadButtonDown(gp, 12) && IsGamepadButtonDown(gp, 13)) ||
                    (IsGamepadButtonDown(gp, GAMEPAD_BUTTON_MIDDLE_LEFT) && IsGamepadButtonDown(gp, GAMEPAD_BUTTON_MIDDLE_RIGHT)))
                {
#ifdef __linux__
                    for (int fd : evdevFds)
                        ::close(fd);
#endif
                    CloseWindow();
                    std::exit(0);
                }

                const float axX = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_LEFT_X);
                const float axY = GetGamepadAxisMovement(gp, GAMEPAD_AXIS_LEFT_Y);
                const int dirX = (axX > 0.5f) ? 1 : ((axX < -0.5f) ? -1 : 0);
                const int dirY = (axY > 0.5f) ? 1 : ((axY < -0.5f) ? -1 : 0);
                if (dirY != prevAxisDirY[gp])
                {
                    if (dirY < 0)
                        upPressed = true;
                    else if (dirY > 0)
                        downPressed = true;
                    prevAxisDirY[gp] = dirY;
                }
                if (dirX != prevAxisDirX[gp])
                {
                    if (dirX < 0)
                        leftPressed = true;
                    else if (dirX > 0)
                        rightPressed = true;
                    prevAxisDirX[gp] = dirX;
                }
            }

#ifdef __linux__
            if (!raylibGamepadConnected)
        {
        for (int fd : evdevFds)
        {
            struct input_event ev;
                while (::read(fd, &ev, sizeof(ev)) == static_cast<ssize_t>(sizeof(ev)))
                {
                    if (ev.type == EV_KEY)
                    {
                        if (ev.code == BTN_SELECT || ev.code == BTN_TRIGGER_HAPPY5)
                            evSelectDown = (ev.value != 0);
                        else if (ev.code == BTN_START || ev.code == BTN_TRIGGER_HAPPY6)
                            evStartDown = (ev.value != 0);

                        if (evSelectDown && evStartDown)
                {
                    evComboTriggered = true;
                            for (int cfd : evdevFds)
                                ::close(cfd);
                            CloseWindow();
                            std::exit(0);
                        }

                        if (ev.value == 1)
                        {
                            if (ev.code == BTN_DPAD_UP || ev.code == BTN_TRIGGER_HAPPY1)
                                upPressed = true;
                            else if (ev.code == BTN_DPAD_DOWN || ev.code == BTN_TRIGGER_HAPPY2)
                                downPressed = true;
                            else if (ev.code == BTN_DPAD_LEFT || ev.code == BTN_TRIGGER_HAPPY3)
                                leftPressed = true;
                            else if (ev.code == BTN_DPAD_RIGHT || ev.code == BTN_TRIGGER_HAPPY4)
                                rightPressed = true;
                            else if (ev.code == BTN_SOUTH)
                                startOnlyPressed = true;
                        else if (ev.code == BTN_EAST)
                            fallbackGlPressed = true;
                            }
                else if (ev.value == 0)
                {
                    if ((ev.code == BTN_START || ev.code == BTN_TRIGGER_HAPPY6) && !evSelectDown && !evComboTriggered)
                        saveAndStartPressed = true;
                    if (!evSelectDown && !evStartDown)
                        evComboTriggered = false;
                        }
                    }
                    else if (ev.type == EV_ABS)
                    {
                        if (ev.code == ABS_HAT0X)
                        {
                            const int d = (ev.value > 0) ? 1 : ((ev.value < 0) ? -1 : 0);
                            if (d != evHatX)
                            {
                                if (d < 0)
                                    leftPressed = true;
                                else if (d > 0)
                                    rightPressed = true;
                                evHatX = d;
                            }
                        }
                        else if (ev.code == ABS_HAT0Y)
                        {
                            const int d = (ev.value > 0) ? 1 : ((ev.value < 0) ? -1 : 0);
                            if (d != evHatY)
                            {
                                if (d < 0)
                                    upPressed = true;
                                else if (d > 0)
                                    downPressed = true;
                                evHatY = d;
                            }
                        }
                    }
                }
            }
            }
#endif

            if (upPressed || downPressed || leftPressed || rightPressed ||
                startOnlyPressed || saveAndStartPressed || fallbackGlPressed)
            {
                countdownActive = false;
            }

            if (upPressed)
            {
                selectedRow = (selectedRow + kRowCount - 1) % kRowCount;
            }
            if (downPressed)
            {
                selectedRow = (selectedRow + 1) % kRowCount;
            }
            if (leftPressed || rightPressed)
            {
                int &val = getOptionValue(selectedRow);
                const int count = choiceCounts[selectedRow]; val = leftPressed ? (val + count - 1) % count : (val + 1) % count;
            }

            if (fallbackGlPressed)
            {
                cfg.rendererIdx = 1; // gl
                done = true;
            }
            else if (saveAndStartPressed)
            {
                saveBootConfig(cfg);
                done = true;
            }
            else if (startOnlyPressed)
            {
                done = true;
            }

            BeginDrawing();
            ClearBackground(Color{14, 18, 26, 255});

            const int screenW = GetScreenWidth();
            const int screenH = GetScreenHeight();
            const int panelW = 600;
            const int panelH = 440;
            const int panelX = (screenW - panelW) / 2;
            const int panelY = (screenH - panelH) / 2;

            DrawRectangle(panelX, panelY, panelW, panelH, Color{24, 30, 42, 255});
            drawBoxOutline(panelX, panelY, panelW, panelH, Color{70, 130, 180, 255});

            DrawText("CELOSIA / SOTC - R36S BOOT MENU", panelX + 24, panelY + 22, 24, Color{245, 245, 245, 255});
            DrawText("640x480 Pre-Boot Renderer Selector", panelX + 24, panelY + 54, 16, Color{160, 185, 215, 255});
            DrawRectangle(panelX + 20, panelY + 82, panelW - 40, 1, Color{58, 72, 96, 255});

            for (int i = 0; i < kRowCount; ++i)
            {
                const int rowY = panelY + 92 + i * 35;
                const bool isSel = (i == selectedRow);
                if (isSel)
                {
                    DrawRectangle(panelX + 20, rowY - 4, panelW - 40, 30, Color{42, 64, 96, 255});
                    drawBoxOutline(panelX + 20, rowY - 4, panelW - 40, 30, Color{110, 180, 255, 255});
                }

                DrawText(labels[i], panelX + 32, rowY + 3, 16, isSel ? Color{253, 249, 0, 255} : Color{200, 200, 200, 255});
                const int valIdx = getOptionValue(i);
                const char *valText = choices[i][valIdx];
                char buf[128];
                std::snprintf(buf, sizeof(buf), "< %s >", valText);
                const int textW = MeasureText(buf, 16);
                DrawText(buf, panelX + panelW - 32 - textW, rowY + 3, 16, isSel ? Color{245, 245, 245, 255} : Color{180, 210, 240, 255});
            }

            DrawRectangle(panelX + 20, panelY + 340, panelW - 40, 1, Color{58, 72, 96, 255});

            if (countdownActive)
            {
                char cdBuf[96];
                const int secs = std::max(1, static_cast<int>(remainingSeconds + 0.99));
                std::snprintf(cdBuf, sizeof(cdBuf), "Iniciando automaticamente em %ds (mova D-Pad para cancelar)", secs);
                DrawText(cdBuf, panelX + 24, panelY + 354, 16, Color{255, 210, 110, 255});
            }
            else
            {
                DrawText("Contagem cancelada. Ajuste as opcoes e inicie.", panelX + 24, panelY + 354, 16, Color{140, 230, 170, 255});
            }

            DrawText("A / Enter: Iniciar   |   Start: Salvar padrao e iniciar", panelX + 24, panelY + 384, 16, Color{200, 200, 200, 255});
            DrawText("B / Esc: Forcar fallback OpenGL (gl) e iniciar", panelX + 24, panelY + 408, 16, Color{255, 160, 160, 255});

            EndDrawing();
        }
#ifdef __linux__
        for (int fd : evdevFds)
            ::close(fd);
#endif
    }
#else
    (void)skipMenu;
#endif

    applyBootEnv(cfg, 1);
}

} // namespace native_renderer
