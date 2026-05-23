#include "AppFonts.h"

#include <filesystem>

#include <imgui.h>

namespace terrain::ui
{

void LoadJapaneseFont(ImGuiIO& io)
{
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };

    for (const char* fontPath : fontPaths)
    {
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
        if (font != nullptr)
        {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}

}
