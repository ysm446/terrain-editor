#pragma once

#include <filesystem>
#include <string>

#include "node_graph.h"

namespace terrain
{
bool ExportPreviewTexturePng(
    const rock::EvaluationSummary& evaluation,
    const std::filesystem::path& path,
    int resolution,
    std::string* error);
} // namespace terrain
