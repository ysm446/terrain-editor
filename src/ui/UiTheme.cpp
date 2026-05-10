#include "UiTheme.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace rock
{
namespace
{
struct ColorName
{
    const char* name;
    ImGuiCol color;
};

constexpr ColorName kColorNames[] = {
    {"Text", ImGuiCol_Text},
    {"TextDisabled", ImGuiCol_TextDisabled},
    {"WindowBg", ImGuiCol_WindowBg},
    {"ChildBg", ImGuiCol_ChildBg},
    {"PopupBg", ImGuiCol_PopupBg},
    {"Border", ImGuiCol_Border},
    {"BorderShadow", ImGuiCol_BorderShadow},
    {"FrameBg", ImGuiCol_FrameBg},
    {"FrameBgHovered", ImGuiCol_FrameBgHovered},
    {"FrameBgActive", ImGuiCol_FrameBgActive},
    {"TitleBg", ImGuiCol_TitleBg},
    {"TitleBgActive", ImGuiCol_TitleBgActive},
    {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
    {"MenuBarBg", ImGuiCol_MenuBarBg},
    {"ScrollbarBg", ImGuiCol_ScrollbarBg},
    {"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
    {"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered},
    {"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
    {"CheckMark", ImGuiCol_CheckMark},
    {"SliderGrab", ImGuiCol_SliderGrab},
    {"SliderGrabActive", ImGuiCol_SliderGrabActive},
    {"Button", ImGuiCol_Button},
    {"ButtonHovered", ImGuiCol_ButtonHovered},
    {"ButtonActive", ImGuiCol_ButtonActive},
    {"Header", ImGuiCol_Header},
    {"HeaderHovered", ImGuiCol_HeaderHovered},
    {"HeaderActive", ImGuiCol_HeaderActive},
    {"Separator", ImGuiCol_Separator},
    {"SeparatorHovered", ImGuiCol_SeparatorHovered},
    {"SeparatorActive", ImGuiCol_SeparatorActive},
    {"ResizeGrip", ImGuiCol_ResizeGrip},
    {"ResizeGripHovered", ImGuiCol_ResizeGripHovered},
    {"ResizeGripActive", ImGuiCol_ResizeGripActive},
    {"Tab", ImGuiCol_Tab},
    {"TabHovered", ImGuiCol_TabHovered},
    {"TabSelected", ImGuiCol_TabSelected},
    {"TabDimmed", ImGuiCol_TabDimmed},
    {"TabDimmedSelected", ImGuiCol_TabDimmedSelected},
    {"PlotLines", ImGuiCol_PlotLines},
    {"PlotLinesHovered", ImGuiCol_PlotLinesHovered},
    {"PlotHistogram", ImGuiCol_PlotHistogram},
    {"PlotHistogramHovered", ImGuiCol_PlotHistogramHovered},
    {"TextSelectedBg", ImGuiCol_TextSelectedBg},
    {"NavCursor", ImGuiCol_NavCursor},
    {"ModalWindowDimBg", ImGuiCol_ModalWindowDimBg},
};

bool TryGetColorName(const std::string& name, ImGuiCol& outColor)
{
    if (name == "TabActive")
    {
        outColor = ImGuiCol_TabSelected;
        return true;
    }
    if (name == "TabUnfocused")
    {
        outColor = ImGuiCol_TabDimmed;
        return true;
    }
    if (name == "TabUnfocusedActive")
    {
        outColor = ImGuiCol_TabDimmedSelected;
        return true;
    }

    for (const ColorName& colorName : kColorNames)
    {
        if (name == colorName.name)
        {
            outColor = colorName.color;
            return true;
        }
    }
    return false;
}

bool TryReadColor(const nlohmann::json& value, ImVec4& outColor)
{
    if (!value.is_array() || value.size() != 4)
    {
        return false;
    }

    outColor = ImVec4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
    return true;
}

std::optional<float> TryReadFloat(const nlohmann::json& root, const char* key)
{
    if (!root.contains(key) || !root[key].is_number())
    {
        return std::nullopt;
    }
    return root[key].get<float>();
}

std::optional<ImVec2> TryReadVec2(const nlohmann::json& root, const char* key)
{
    if (!root.contains(key) || !root[key].is_array() || root[key].size() != 2)
    {
        return std::nullopt;
    }
    return ImVec2(root[key][0].get<float>(), root[key][1].get<float>());
}

UiTheme ThemeFromJson(const nlohmann::json& root)
{
    UiTheme theme;
    theme.info.id = root.value("id", std::string());
    theme.info.name = root.value("name", theme.info.id);
    theme.base = root.value("base", std::string("dark"));

    if (root.contains("style") && root["style"].is_object())
    {
        const nlohmann::json& style = root["style"];
        theme.scaleAllSizes = style.value("scaleAllSizes", theme.scaleAllSizes);
        theme.alpha = TryReadFloat(style, "alpha");
        theme.windowRounding = TryReadFloat(style, "windowRounding");
        theme.frameRounding = TryReadFloat(style, "frameRounding");
        theme.grabRounding = TryReadFloat(style, "grabRounding");
        theme.childRounding = TryReadFloat(style, "childRounding");
        theme.popupRounding = TryReadFloat(style, "popupRounding");
        theme.scrollbarRounding = TryReadFloat(style, "scrollbarRounding");
        theme.windowBorderSize = TryReadFloat(style, "windowBorderSize");
        theme.frameBorderSize = TryReadFloat(style, "frameBorderSize");
        theme.childBorderSize = TryReadFloat(style, "childBorderSize");
        theme.windowPadding = TryReadVec2(style, "windowPadding");
        theme.framePadding = TryReadVec2(style, "framePadding");
        theme.itemSpacing = TryReadVec2(style, "itemSpacing");
        theme.itemInnerSpacing = TryReadVec2(style, "itemInnerSpacing");
    }

    if (root.contains("colors") && root["colors"].is_object())
    {
        for (auto it = root["colors"].begin(); it != root["colors"].end(); ++it)
        {
            ImGuiCol colorIndex = ImGuiCol_COUNT;
            ImVec4 colorValue;
            if (!TryReadColor(it.value(), colorValue))
            {
                continue;
            }
            if (TryGetColorName(it.key(), colorIndex))
            {
                theme.colors.push_back({colorIndex, colorValue});
            }
            else
            {
                theme.namedColors[it.key()] = colorValue;
            }
        }
    }

    return theme;
}
} // namespace

void UiThemeManager::LoadThemes(const std::filesystem::path& themeDirectory)
{
    themes_.clear();

    try
    {
        if (std::filesystem::exists(themeDirectory) && std::filesystem::is_directory(themeDirectory))
        {
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(themeDirectory))
            {
                if (!entry.is_regular_file() || entry.path().extension() != ".json")
                {
                    continue;
                }

                std::ifstream stream(entry.path());
                if (!stream)
                {
                    continue;
                }

                nlohmann::json root;
                stream >> root;
                UiTheme theme = ThemeFromJson(root);
                if (theme.info.id.empty())
                {
                    continue;
                }

                auto existing = std::ranges::find_if(themes_, [&](const UiTheme& item) { return item.info.id == theme.info.id; });
                if (existing != themes_.end())
                {
                    *existing = std::move(theme);
                }
                else
                {
                    themes_.push_back(std::move(theme));
                }
            }
        }
    }
    catch (...)
    {
    }

    RebuildThemeInfos();
}

bool UiThemeManager::ApplyTheme(const std::string& id)
{
    auto it = std::ranges::find_if(themes_, [&](const UiTheme& theme) { return theme.info.id == id; });
    if (it == themes_.end())
    {
        it = std::ranges::find_if(themes_, [](const UiTheme& theme) { return theme.info.id == "road_editor_dark"; });
    }
    if (it == themes_.end() && !themes_.empty())
    {
        it = themes_.begin();
    }
    if (it == themes_.end())
    {
        return false;
    }

    ApplyThemeData(*it);
    currentThemeId_ = it->info.id;
    currentTheme_ = &(*it);
    return true;
}

ImVec4 UiThemeManager::AppColor(const std::string& name, const ImVec4& fallback) const
{
    if (currentTheme_ == nullptr)
    {
        return fallback;
    }
    const auto it = currentTheme_->namedColors.find(name);
    return it != currentTheme_->namedColors.end() ? it->second : fallback;
}

void UiThemeManager::ApplyThemeData(const UiTheme& theme)
{
    ImGui::GetStyle() = ImGuiStyle();
    if (theme.base == "light")
    {
        ImGui::StyleColorsLight();
    }
    else if (theme.base == "classic")
    {
        ImGui::StyleColorsClassic();
    }
    else
    {
        ImGui::StyleColorsDark();
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(theme.scaleAllSizes);
    if (theme.alpha) style.Alpha = *theme.alpha;
    if (theme.windowRounding) style.WindowRounding = *theme.windowRounding;
    if (theme.frameRounding) style.FrameRounding = *theme.frameRounding;
    if (theme.grabRounding) style.GrabRounding = *theme.grabRounding;
    if (theme.childRounding) style.ChildRounding = *theme.childRounding;
    if (theme.popupRounding) style.PopupRounding = *theme.popupRounding;
    if (theme.scrollbarRounding) style.ScrollbarRounding = *theme.scrollbarRounding;
    if (theme.windowBorderSize) style.WindowBorderSize = *theme.windowBorderSize;
    if (theme.frameBorderSize) style.FrameBorderSize = *theme.frameBorderSize;
    if (theme.childBorderSize) style.ChildBorderSize = *theme.childBorderSize;
    if (theme.windowPadding) style.WindowPadding = *theme.windowPadding;
    if (theme.framePadding) style.FramePadding = *theme.framePadding;
    if (theme.itemSpacing) style.ItemSpacing = *theme.itemSpacing;
    if (theme.itemInnerSpacing) style.ItemInnerSpacing = *theme.itemInnerSpacing;

    for (const auto& [colorIndex, colorValue] : theme.colors)
    {
        style.Colors[colorIndex] = colorValue;
    }
}

void UiThemeManager::RebuildThemeInfos()
{
    themeInfos_.clear();
    for (const UiTheme& theme : themes_)
    {
        if (theme.info.id == "road_editor_dark")
        {
            themeInfos_.push_back(theme.info);
            break;
        }
    }
    for (const UiTheme& theme : themes_)
    {
        if (theme.info.id == "road_editor_dark")
        {
            continue;
        }
        themeInfos_.push_back(theme.info);
    }
}

} // namespace rock
