#include "NodePins.h"

#include <imgui-node-editor/imgui_node_editor.h>

namespace ed = ax::NodeEditor;

namespace terrain::ui
{

namespace
{

ImU32 ColorToU32(const ImVec4& color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

}

ImVec4 PinTypeColor(rock::ValueType valueType)
{
    switch (valueType)
    {
    case rock::ValueType::HeightField:
        return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    case rock::ValueType::Mask:
        return ImVec4(0.82f, 0.64f, 0.36f, 1.0f);
    case rock::ValueType::ColorTexture:
        return ImVec4(0.54f, 0.60f, 1.0f, 1.0f);
    case rock::ValueType::Path:
        return ImVec4(0.42f, 0.78f, 0.92f, 1.0f);
    case rock::ValueType::Mesh:
    default:
        return ImVec4(0.52f, 0.58f, 0.56f, 1.0f);
    }
}

ImVec4 PinColor(const rock::Pin& pin)
{
    return PinTypeColor(pin.valueType);
}

ImVec4 PinLabelColor(const rock::Pin& pin, bool hovered, bool selected)
{
    if (selected)
    {
        return PinColor(pin);
    }
    if (hovered)
    {
        return ImVec4(0.94f, 0.94f, 0.92f, 1.0f);
    }
    return ImVec4(0.62f, 0.64f, 0.62f, 1.0f);
}

void DrawRoundPin(const rock::Pin& pin)
{
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImVec2 pivotMin(center.x - 6.0f, center.y - 6.0f);
    const ImVec2 pivotMax(center.x + 6.0f, center.y + 6.0f);
    ed::PinRect(min, max);
    ed::PinPivotRect(pivotMin, pivotMax);
    const ImVec4 color = PinColor(pin);
    ImGui::GetWindowDrawList()->AddCircle(center, 4.3f, ColorToU32(color), 16, 1.6f);
}

}
