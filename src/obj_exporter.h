#pragma once

#include "node_graph.h"

#include <filesystem>

namespace rock
{
bool ExportMeshObj(const MeshData& mesh, const std::filesystem::path& path, std::string* errorMessage);
}
