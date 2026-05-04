#pragma once

#include <filesystem>
#include <string>

#include <windows.h>

namespace terrain
{
bool CaptureWindowScreenshot(HWND hwnd, const std::filesystem::path& directory, std::filesystem::path* savedPath, std::string* error);
}
