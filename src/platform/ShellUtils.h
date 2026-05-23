#pragma once

#include <filesystem>

namespace terrain::platform
{

void RevealFileInExplorer(const std::filesystem::path& path);
void OpenFolderInExplorer(const std::filesystem::path& folder);

}
