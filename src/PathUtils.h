#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace terrain
{

std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(const std::string& value);
std::filesystem::path ProjectFolderForPath(const std::filesystem::path& projectPath);
std::filesystem::path ScreenshotDirectoryForProject(const std::filesystem::path& projectPath);
std::filesystem::path NormalizedProjectPath(const std::filesystem::path& path);
std::string ResolveProjectAssetPath(std::string_view value, const std::filesystem::path& projectFolder);
std::string MakeProjectAssetPathForJson(const std::string& value, const std::filesystem::path& activeProjectFolder);

}
