#include "runtime/native_renderer.h"
#include <raylib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace native_renderer
{
namespace
{
    struct BootConfig
    {
        int rendererIdx = 0; // 0 = mali, 1 = gl
        int fpsIdx = 0;      // 0 = 30, 1 = 60
        int drsIdx = 0;      // 0 = 1 (Auto), 1 = 0 (Desligado)
        int tex16Idx = 0;    // 0 = 1 (Ligado), 1 = 0 (Desligado)
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
                cfg.fpsIdx = (val == "60") ? 1 : 0;
            else if (key == "SOTC_MALI_DRS")
                cfg.drsIdx = (val == "0") ? 1 : 0;
            else if (key == "SOTC_MALI_TEX16")
                cfg.tex16Idx = (val == "0") ? 1 : 0;
        }
    }

    void saveBootConfig(const BootConfig &cfg)
    {
        std::ofstream out(getConfigPath(), std::ios::trunc);
        if (!out.is_open())
            return;

        out << "SOTC_RENDERER=" << (cfg.rendererIdx == 0 ? "mali" : "gl") << "\n";
        out << "SOTC_FPS_CAP=" << (cfg.fpsIdx == 0 ? "30" : "60") << "\n";
        out << "SOTC_MALI_DRS=" << (cfg.drsIdx == 0 ? "1" : "0") << "\n";
        out << "SOTC_MALI_TEX16=" << (cfg.tex16Idx == 0 ? "1" : "0") << "\n";
    }

    void setEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    void applyBootEnv(const BootConfig &cfg)
    {
        setEnvVar("SOTC_RENDERER", cfg.rendererIdx == 0 ? "mali" : "gl");
        setEnvVar("SOTC_FPS_CAP", cfg.fpsIdx == 0 ? "30" : "60");
        setEnvVar("SOTC_MALI_DRS", cfg.drsIdx == 0 ? "1" : "0");
        setEnvVar("SOTC_MALI_TEX16", cfg.tex16Idx == 0 ? "1" : "0");
        setEnvVar("SOTC_NATIVE", "1");
        setEnvVar("SOTC_NATIVE_GL", "1");
    }
} // namespace

void runR36sBootMenu()
{
    BootConfig cfg;
    loadBootConfig(cfg);

    const char *skipEnv = std::getenv("SOTC_SKIP_BOOT_MENU");
    const bool skipMenu = (skipEnv != nullptr && std::string(skipEnv) == "1");

#if defined(__linux__)
    if (!skipMenu && IsWindowReady())
    {
        int selectedRow = 0;
        constexpr int kRowCount = 4;
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
            default:
                return cfg.tex16Idx;
            }
        };

        const char *labels[kRowCount] = {
            "Renderizador",
            "Perfil FPS",
            "Resolucao Dinamica (DRS)",
            "Texturas 16-bit (RGB565)",
        };

        const char *choices[kRowCount][2] = {
            {"Mali-G31 Engine (Novo)", "OpenGL Classico"},
            {"30 FPS Cravados", "60 FPS Livre"},
            {"Auto (0.75-1.00)", "Desligado (1.00)"},
            {"Ligado", "Desligado (RGBA8)"},
        };

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

            for (int gp = 0; gp < 4; ++gp)
            {
                if (!IsGamepadAvailable(gp))
                    continue;

                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_UP))
                    upPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
                    downPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
                    leftPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
                    rightPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
                    startOnlyPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_MIDDLE_RIGHT))
                    saveAndStartPressed = true;
                if (IsGamepadButtonPressed(gp, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
                    fallbackGlPressed = true;
            }

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
                val = 1 - val;
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
            DrawRectangleLines(panelX, panelY, panelW, panelH, Color{70, 130, 180, 255});

            DrawText("CELOSIA / SOTC - R36S BOOT MENU", panelX + 24, panelY + 22, 24, RAYWHITE);
            DrawText("640x480 Pre-Boot Renderer Selector", panelX + 24, panelY + 54, 16, Color{160, 185, 215, 255});
            DrawLine(panelX + 20, panelY + 82, panelX + panelW - 20, panelY + 82, Color{58, 72, 96, 255});

            for (int i = 0; i < kRowCount; ++i)
            {
                const int rowY = panelY + 104 + i * 56;
                const bool isSel = (i == selectedRow);
                if (isSel)
                {
                    DrawRectangle(panelX + 20, rowY - 6, panelW - 40, 44, Color{42, 64, 96, 255});
                    DrawRectangleLines(panelX + 20, rowY - 6, panelW - 40, 44, Color{110, 180, 255, 255});
                }

                DrawText(labels[i], panelX + 32, rowY + 6, 18, isSel ? YELLOW : LIGHTGRAY);
                const int valIdx = getOptionValue(i);
                const char *valText = choices[i][valIdx];
                char buf[128];
                std::snprintf(buf, sizeof(buf), "< %s >", valText);
                const int textW = MeasureText(buf, 18);
                DrawText(buf, panelX + panelW - 32 - textW, rowY + 6, 18, isSel ? RAYWHITE : Color{180, 210, 240, 255});
            }

            DrawLine(panelX + 20, panelY + 340, panelX + panelW - 20, panelY + 340, Color{58, 72, 96, 255});

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

            DrawText("A / Enter: Iniciar   |   Start: Salvar padrao e iniciar", panelX + 24, panelY + 384, 16, LIGHTGRAY);
            DrawText("B / Esc: Forcar fallback OpenGL (gl) e iniciar", panelX + 24, panelY + 408, 16, Color{255, 160, 160, 255});

            EndDrawing();
        }
    }
#else
    (void)skipMenu;
#endif

    applyBootEnv(cfg);
}

} // namespace native_renderer
