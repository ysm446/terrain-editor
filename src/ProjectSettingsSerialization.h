#pragma once

#include <nlohmann/json.hpp>

#include "node_graph.h"

namespace terrain
{

struct ProjectDisplaySettings
{
    bool showFps = false;
};

nlohmann::json MakeProjectSettingsJson(const rock::GraphSettings& graphSettings, const ProjectDisplaySettings& display);
bool ReadProjectSettingsJson(const nlohmann::json& root, rock::GraphSettings& graphSettings, ProjectDisplaySettings& display);

} // namespace terrain