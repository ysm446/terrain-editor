#pragma once

#include "node_graph.h"

#include <filesystem>

namespace rock
{
bool ExportDebugTrianglesObj(const SdfPreviewStats& sdf, const std::filesystem::path& path, std::string* errorMessage);
bool ExportMeshObj(const MeshData& mesh, const std::filesystem::path& path, std::string* errorMessage);
}
