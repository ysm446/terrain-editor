#pragma once

#include <functional>
#include <string>

#include "../node_graph.h"

namespace terrain::ui
{
struct AssetExportPanelState
{
    const rock::EvaluationSummary& evaluation;
    std::string& exportStatus;
    std::function<void()> ensurePreviewMesh;
};

void DrawAssetExportPanel(AssetExportPanelState state);
} // namespace terrain::ui
