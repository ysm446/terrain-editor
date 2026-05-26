#pragma once

#include <nlohmann/json.hpp>

#include "node_graph.h"

namespace terrain
{

nlohmann::json MakeProjectSettingsJson(const rock::GraphSettings& graphSettings);
bool ReadProjectSettingsJson(const nlohmann::json& root, rock::GraphSettings& graphSettings);

} // namespace terrain
