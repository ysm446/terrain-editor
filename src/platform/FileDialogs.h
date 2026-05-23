#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <windows.h>

namespace terrain::platform
{

std::optional<std::filesystem::path> ShowProjectFileDialog(
    HWND owner,
    const std::filesystem::path& currentProjectPath,
    bool save);

std::optional<std::filesystem::path> ShowHeightmapFileDialog(
    HWND owner,
    const std::filesystem::path& projectPath,
    const std::string& currentPath);

}
