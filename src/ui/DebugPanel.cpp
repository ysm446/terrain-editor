#include "DebugPanel.h"

#include <algorithm>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
void DrawDebugPanel(DebugPanelState state)
{
    rock::GraphSettings& settings = state.settings;
    const rock::EvaluationSummary& evaluation = state.evaluation;
    const DebugPanelRenderStats& renderStats = state.renderStats;

    ImGui::SeparatorText("Viewport Debug");
    if (ImGui::BeginTable("DebugViewportRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        if (DrawPropertyBoolRow("Draw Calls", "DebugDrawCalls", &state.showDrawStats, "Draw stats visibility changed",
                "Shows the latest preview draw-call count in the viewport overlay.",
                false, true))
        {
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }
        if (DrawPropertyBoolRow("Frame Timing", "DebugFrameTiming", &state.showFrameStats, "Frame timing visibility changed",
                "Shows frame timing stats in the viewport overlay.",
                false, true))
        {
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }
        if (DrawPropertyBoolRow("Surface", "DebugSurface", &settings.preview.showSurface, "Surface visibility changed",
                "Shows the filled terrain surface. Turn it off when inspecting wireframe or grid-only debug views.",
                rock::PreviewSettings{}.showSurface, true))
        {
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }
        if (DrawPropertyBoolRow("Wireframe", "DebugWireframe", &settings.preview.showWireframe, "Wireframe visibility changed",
                "Shows mesh edges for topology debugging. High viewport resolutions can make this expensive.",
                rock::PreviewSettings{}.showWireframe, true))
        {
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("Graph Version: %llu", static_cast<unsigned long long>(evaluation.version));
    ImGui::Text("%s", state.lastEvaluationDuration.c_str());
    ImGui::TextColored(evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.dirty ? "Dirty" : "Evaluated");
    ImGui::TextWrapped("%s", evaluation.status.c_str());

    ImGui::SeparatorText("Preview");
    ImGui::Text("Stage: %s", rock::ToString(evaluation.previewStage).data());
    ImGui::Text("Backend: %s", renderStats.gpuDisplacement ? (renderStats.tessellation ? "GPU Displacement + Tessellation" : "GPU Displacement") : "CPU Mesh");
    ImGui::Text("Render Target: %d x %d", renderStats.renderTargetWidth, renderStats.renderTargetHeight);
    ImGui::Text("Draw Calls: %u (%u indexed)", renderStats.drawCalls, renderStats.indexedDrawCalls);
    ImGui::Text("Submitted: %llu verts / %llu tris / %llu lines",
        static_cast<unsigned long long>(renderStats.submittedVertices),
        static_cast<unsigned long long>(renderStats.submittedTriangles),
        static_cast<unsigned long long>(renderStats.submittedLines));
    if (renderStats.tessellation)
    {
        ImGui::Text("Patches: %u, Tess Max: %.1f", renderStats.submittedPatches, renderStats.tessellationMaxFactor);
    }
    ImGui::Text("Passes: %s%s%s%s%s%s",
        renderStats.shadowPass ? "Shadow " : "",
        renderStats.skyPass ? "Sky " : "",
        renderStats.surfacePass ? "Surface " : "",
        renderStats.gridPass ? "Grid " : "",
        renderStats.wireframePass ? "Wireframe " : "",
        renderStats.cloudsPass ? "Clouds " : "");

    ImGui::SeparatorText("Displayed Mesh");
    ImGui::Text("Mesh Resolution: %d", renderStats.displayMeshResolution);
    ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(renderStats.displayedVertices));
    ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(renderStats.displayedTriangles));
    ImGui::SeparatorText("Evaluated Mesh");
    ImGui::Text("Vertices: %zu", evaluation.previewMesh.vertices.size());
    ImGui::Text("Edges: %zu", evaluation.previewMesh.edges.size());
    ImGui::Text("Triangles: %zu", evaluation.previewMesh.triangles.size());
}
} // namespace terrain::ui
