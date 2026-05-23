#include "FileDialogs.h"

#include <commdlg.h>

#include "../PathUtils.h"

namespace terrain::platform
{

std::optional<std::filesystem::path> ShowProjectFileDialog(
    HWND owner,
    const std::filesystem::path& currentProjectPath,
    bool save)
{
    wchar_t fileName[MAX_PATH]{};
    if (!currentProjectPath.empty())
    {
        const std::wstring current = currentProjectPath.wstring();
        wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Terrain Editor Project (*.terrainproj)\0*.terrainproj\0Legacy Rock Generator Project (*.rockproj)\0*.rockproj\0JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"terrainproj";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (save)
    {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&ofn))
        {
            return std::nullopt;
        }
    }
    else
    {
        ofn.Flags |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn))
        {
            return std::nullopt;
        }
    }

    return std::filesystem::path(fileName);
}

std::optional<std::filesystem::path> ShowHeightmapFileDialog(
    HWND owner,
    const std::filesystem::path& projectPath,
    const std::string& currentPath)
{
    wchar_t fileName[MAX_PATH]{};
    if (!currentPath.empty())
    {
        std::filesystem::path current = terrain::PathFromUtf8(currentPath);
        if (current.is_relative() && !projectPath.empty())
        {
            const std::filesystem::path parent = projectPath.parent_path();
            current = (parent.empty() ? std::filesystem::current_path() : parent) / current;
        }
        const std::wstring currentWide = current.wstring();
        wcsncpy_s(fileName, currentWide.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Heightmap Images (*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp)\0*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
    {
        return std::nullopt;
    }

    return std::filesystem::path(fileName);
}

}
