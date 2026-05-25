#include "AssetExportPanel.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>

#include <imgui.h>

namespace terrain::ui
{
namespace
{
const char* TextureKindLabel(const rock::EvaluationSummary& evaluation)
{
    if (evaluation.previewIsColor)
    {
        return "Color texture";
    }
    if (evaluation.previewShowsMask)
    {
        return "Mask";
    }
    return "Heightmap";
}

const char* TextureFormatLabel(const rock::EvaluationSummary& evaluation)
{
    if (evaluation.previewIsColor)
    {
        return "PNG RGBA 8-bit";
    }
    if (evaluation.previewShowsMask)
    {
        return "PNG grayscale 8-bit";
    }
    return "PNG grayscale 16-bit";
}

std::filesystem::path EnsureDisplayedPngExtension(std::filesystem::path path)
{
    if (path.extension().empty())
    {
        path.replace_extension(".png");
    }
    return path;
}

bool DrawFolderIconButton(const char* id)
{
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button(id, ImVec2(size, size));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float unit = size / 18.0f;
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 outline = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
    const ImVec2 tabMin(min.x + 4.0f * unit, min.y + 5.0f * unit);
    const ImVec2 tabMax(min.x + 8.0f * unit, min.y + 8.0f * unit);
    const ImVec2 bodyMin(min.x + 3.0f * unit, min.y + 7.0f * unit);
    const ImVec2 bodyMax(max.x - 3.0f * unit, max.y - 4.0f * unit);
    drawList->AddRectFilled(tabMin, tabMax, fill, 1.5f * unit);
    drawList->AddRectFilled(bodyMin, bodyMax, fill, 2.0f * unit);
    drawList->AddRect(bodyMin, bodyMax, outline, 2.0f * unit);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("フォルダを開く");
    }
    return pressed;
}
} // namespace

void DrawAssetExportPanel(AssetExportPanelState state)
{
    const rock::EvaluationSummary& evaluation = state.evaluation;
    static int exportResolution = 512;
    static char exportPathBuffer[260] = "exports/node_texture.png";
    constexpr std::array<int, 5> kExportResolutions = {128, 256, 512, 1024, 2048};
    const int maxExportResolution = std::clamp(state.maxExportResolution, 2, 2048);
    exportResolution = std::clamp(exportResolution, 2, maxExportResolution);

    if (ImGui::BeginTable("TextureExportTable", 2, ImGuiTableFlags_SizingFixedFit))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Preview output");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s", evaluation.dirty ? "needs evaluation" : "ready");

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Type");
        ImGui::TableSetColumnIndex(1);
        ImGui::Text("%s / %s", TextureKindLabel(evaluation), TextureFormatLabel(evaluation));

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Resolution");
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(120.0f);
        const std::string currentResolution = std::to_string(exportResolution) + " px";
        if (ImGui::BeginCombo("##ExportResolution", currentResolution.c_str()))
        {
            for (int resolution : kExportResolutions)
            {
                if (resolution > maxExportResolution)
                {
                    continue;
                }
                const bool selected = exportResolution == resolution;
                const std::string label = std::to_string(resolution) + " px";
                if (ImGui::Selectable(label.c_str(), selected))
                {
                    exportResolution = resolution;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("File");
        ImGui::TableSetColumnIndex(1);
        const float folderButtonWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(std::min(320.0f, std::max(140.0f, ImGui::GetContentRegionAvail().x - folderButtonWidth)));
        ImGui::InputText("##ExportFile", exportPathBuffer, sizeof(exportPathBuffer));
        ImGui::SameLine();
        if (DrawFolderIconButton("##OpenExportFolder") && state.openExportFolder)
        {
            state.openExportFolder(EnsureDisplayedPngExtension(exportPathBuffer));
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button("Export Texture"))
        {
            std::string error;
            const std::filesystem::path exportPath = EnsureDisplayedPngExtension(exportPathBuffer);
            if (state.exportTexture && state.exportTexture(exportPath, exportResolution, &error))
            {
                state.exportStatus = "Exported " + exportPath.string();
            }
            else
            {
                state.exportStatus = "Export failed: " + error;
            }
        }

        if (!state.exportStatus.empty())
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Status");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", state.exportStatus.c_str());
        }

        ImGui::EndTable();
    }
}
} // namespace terrain::ui
