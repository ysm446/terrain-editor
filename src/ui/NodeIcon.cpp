#include "NodeIcon.h"

#include <algorithm>

#include <imgui.h>

namespace terrain::ui
{

namespace
{

ImU32 ColorToU32(const ImVec4& color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

}

void DrawNodeIcon(const ImVec2& origin, const ImVec4& color)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto scaledColor = [](ImVec4 c, float scale, float alpha = 1.0f)
    {
        c.x = std::clamp(c.x * scale, 0.0f, 1.0f);
        c.y = std::clamp(c.y * scale, 0.0f, 1.0f);
        c.z = std::clamp(c.z * scale, 0.0f, 1.0f);
        c.w *= alpha;
        return ColorToU32(c);
    };

    const ImU32 backColor = scaledColor(color, 0.62f, 0.55f);
    const ImU32 faceColor = scaledColor(color, 0.95f, 0.96f);
    const ImU32 ridgeColor = scaledColor(color, 1.18f, 1.0f);
    const ImU32 shadeColor = scaledColor(color, 0.55f, 0.82f);
    const ImU32 snowColor = ColorToU32(ImVec4(0.92f, 0.96f, 0.90f, 0.90f));

    drawList->AddTriangleFilled(
        ImVec2(origin.x + 1.5f, origin.y + 17.0f),
        ImVec2(origin.x + 7.0f, origin.y + 7.5f),
        ImVec2(origin.x + 13.5f, origin.y + 17.0f),
        backColor);

    const ImVec2 mountain[] = {
        ImVec2(origin.x + 4.0f, origin.y + 18.0f),
        ImVec2(origin.x + 10.0f, origin.y + 7.0f),
        ImVec2(origin.x + 14.5f, origin.y + 13.0f),
        ImVec2(origin.x + 18.0f, origin.y + 5.0f),
        ImVec2(origin.x + 25.0f, origin.y + 18.0f),
    };
    drawList->AddConvexPolyFilled(mountain, 5, faceColor);
    drawList->AddTriangleFilled(
        ImVec2(origin.x + 18.0f, origin.y + 5.0f),
        ImVec2(origin.x + 25.0f, origin.y + 18.0f),
        ImVec2(origin.x + 15.5f, origin.y + 18.0f),
        shadeColor);

    drawList->AddLine(ImVec2(origin.x + 10.0f, origin.y + 7.0f), ImVec2(origin.x + 14.5f, origin.y + 13.0f), ridgeColor, 1.25f);
    drawList->AddLine(ImVec2(origin.x + 14.5f, origin.y + 13.0f), ImVec2(origin.x + 18.0f, origin.y + 5.0f), ridgeColor, 1.25f);
    drawList->AddPolyline(mountain, 5, ridgeColor, 0, 1.15f);

    drawList->AddLine(ImVec2(origin.x + 18.0f, origin.y + 6.2f), ImVec2(origin.x + 16.0f, origin.y + 10.0f), snowColor, 1.15f);
    drawList->AddLine(ImVec2(origin.x + 18.0f, origin.y + 6.2f), ImVec2(origin.x + 20.6f, origin.y + 11.2f), snowColor, 1.15f);
    drawList->AddLine(ImVec2(origin.x + 3.0f, origin.y + 18.0f), ImVec2(origin.x + 25.5f, origin.y + 18.0f), shadeColor, 1.1f);
}

}
