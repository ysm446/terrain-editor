#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "node_graph.h"

namespace terrain
{

using AssetPathForJson = std::function<std::string(const std::string&)>;
using CanCreateLink = std::function<bool(rock::GraphId, rock::GraphId)>;

nlohmann::json MakeSerializedNodeJson(const rock::Node& node, const AssetPathForJson& assetPathForJson);
nlohmann::json MakeSerializedNodesJson(const std::vector<rock::Node>& nodes, const AssetPathForJson& assetPathForJson);
nlohmann::json MakeSerializedLinksJson(const std::vector<rock::Link>& links);
std::optional<rock::NodeKind> ReadSerializedNodeKind(const nlohmann::json& nodeJson);
std::optional<rock::PreviewStage> ReadSerializedPreviewStage(const nlohmann::json& root, rock::PreviewStage fallbackStage);
std::optional<rock::Node> ReadSerializedNodeJson(const nlohmann::json& nodeJson);
std::vector<rock::Node> ReadSerializedNodesJson(const nlohmann::json& root);
std::vector<rock::Link> ReadSerializedLinksJson(const nlohmann::json& root, const CanCreateLink& canCreateLink);
void MigrateRockUniqueMaskPins(std::vector<rock::Node>& nodes);

}
