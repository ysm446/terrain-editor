#include "ShellUtils.h"

#include <windows.h>
#include <shellapi.h>

namespace terrain::platform
{

void RevealFileInExplorer(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    const std::wstring args = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenFolderInExplorer(const std::filesystem::path& folder)
{
    if (folder.empty())
    {
        return;
    }

    ShellExecuteW(nullptr, L"open", std::filesystem::absolute(folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}
