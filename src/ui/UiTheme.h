#pragma once

#include <imgui.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rock
{
struct UiThemeInfo
{
    std::string id;
    std::string name;
};

struct UiTheme
{
    UiThemeInfo info;
    std::string base = "dark";
    float scaleAllSizes = 1.0f;
    std::optional<float> alpha;
    std::optional<float> windowRounding;
    std::optional<float> frameRounding;
    std::optional<float> grabRounding;
    std::optional<float> childRounding;
    std::optional<float> popupRounding;
    std::optional<float> scrollbarRounding;
    std::optional<float> windowBorderSize;
    std::optional<float> frameBorderSize;
    std::optional<float> childBorderSize;
    std::optional<ImVec2> windowPadding;
    std::optional<ImVec2> framePadding;
    std::optional<ImVec2> itemSpacing;
    std::optional<ImVec2> itemInnerSpacing;
    std::vector<std::pair<ImGuiCol, ImVec4>> colors;
    std::unordered_map<std::string, ImVec4> namedColors;
};

class UiThemeManager
{
public:
    void LoadThemes(const std::filesystem::path& themeDirectory);
    bool ApplyTheme(const std::string& id);

    const std::vector<UiThemeInfo>& ThemeInfos() const { return themeInfos_; }
    const std::string& CurrentThemeId() const { return currentThemeId_; }
    ImVec4 AppColor(const std::string& name, const ImVec4& fallback) const;

private:
    void ApplyThemeData(const UiTheme& theme);
    void RebuildThemeInfos();

    std::vector<UiTheme> themes_;
    std::vector<UiThemeInfo> themeInfos_;
    std::string currentThemeId_ = "road_editor_dark";
    const UiTheme* currentTheme_ = nullptr;
};

} // namespace rock
