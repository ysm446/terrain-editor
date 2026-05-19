#include "AssetExportPanel.h"

#include <filesystem>

#include <imgui.h>

#include "../obj_exporter.h"

namespace terrain::ui
{
void DrawAssetExportPanel(AssetExportPanelState state)
{
    const rock::EvaluationSummary& evaluation = state.evaluation;

    ImGui::Columns(3, nullptr, false);
    ImGui::TextUnformatted("Preview mesh");
    ImGui::Text("%s", evaluation.dirty ? "needs evaluation" : "ready");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Topology");
    ImGui::Text("%zu verts / %zu tris",
                evaluation.previewMesh.vertices.size(),
                evaluation.previewMesh.triangles.size());
    ImGui::NextColumn();
    ImGui::TextUnformatted("Export");
    if (ImGui::Button("Export OBJ"))
    {
        if (state.ensurePreviewMesh)
        {
            state.ensurePreviewMesh();
        }

        std::string error;
        const std::filesystem::path exportPath = std::filesystem::path("exports") / "terrain_mesh.obj";
        if (rock::ExportMeshObj(evaluation.previewMesh, exportPath, &error))
        {
            state.exportStatus = "Exported " + exportPath.string();
        }
        else
        {
            state.exportStatus = "Export failed: " + error;
        }
    }
    ImGui::TextWrapped("%s", state.exportStatus.c_str());
    ImGui::Columns(1);
}
} // namespace terrain::ui
