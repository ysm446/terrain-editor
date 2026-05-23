#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include "../node_graph.h"

namespace terrain::ui
{
struct AssetExportPanelState
{
    const rock::EvaluationSummary& evaluation;
    std::string& exportStatus;
    int maxExportResolution = 512;
    std::function<bool(const std::filesystem::path&, int, std::string*)> exportTexture;
    std::function<void(const std::filesystem::path&)> openExportFolder;
};

void DrawAssetExportPanel(AssetExportPanelState state);
} // namespace terrain::ui
