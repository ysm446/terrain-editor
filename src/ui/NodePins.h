#pragma once

#include <imgui.h>

#include "../node_graph.h"

namespace terrain::ui
{

ImVec4 PinTypeColor(rock::ValueType valueType);
ImVec4 PinColor(const rock::Pin& pin);
ImVec4 PinLabelColor(const rock::Pin& pin, bool hovered, bool selected);
void DrawRoundPin(const rock::Pin& pin);

}
