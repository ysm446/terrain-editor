#include "PathUtils.h"

#include <system_error>

namespace terrain
{

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path PathFromUtf8(const std::string& value)
{
    const std::u8string utf8(value.begin(), value.end());
    return std::filesystem::path(utf8);
}

std::filesystem::path ProjectFolderForPath(const std::filesystem::path& projectPath)
{
    if (projectPath.empty())
    {
        return {};
    }
    const std::filesystem::path parent = projectPath.parent_path();
    return parent.empty() ? std::filesystem::current_path() : parent;
}

std::filesystem::path ScreenshotDirectoryForProject(const std::filesystem::path& projectPath)
{
    if (!projectPath.empty())
    {
        const std::filesystem::path parent = projectPath.parent_path();
        if (!parent.empty())
        {
            return parent / "screenshots";
        }
    }
    return std::filesystem::current_path() / "screenshots";
}

std::filesystem::path NormalizedProjectPath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

std::string ResolveProjectAssetPath(std::string_view value, const std::filesystem::path& projectFolder)
{
    if (value.empty())
    {
        return {};
    }
    std::filesystem::path path = PathFromUtf8(std::string(value));
    if (path.is_absolute())
    {
        return PathToUtf8(path.lexically_normal());
    }
    if (projectFolder.empty())
    {
        return PathToUtf8(path.lexically_normal());
    }
    return PathToUtf8((projectFolder / path).lexically_normal());
}

std::string MakeProjectAssetPathForJson(const std::string& value, const std::filesystem::path& activeProjectFolder)
{
    if (value.empty())
    {
        return {};
    }
    std::filesystem::path path = PathFromUtf8(value);
    if (path.is_relative())
    {
        return PathToUtf8(path.lexically_normal());
    }

    if (activeProjectFolder.empty())
    {
        return PathToUtf8(path.lexically_normal());
    }

    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(path, activeProjectFolder, error);
    if (error || relative.empty())
    {
        return PathToUtf8(path.lexically_normal());
    }
    return PathToUtf8(relative.lexically_normal());
}

}
