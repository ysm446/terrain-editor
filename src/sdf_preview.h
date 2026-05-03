#pragma once

#include "node_graph.h"

#include <vector>

namespace rock
{
float EvaluateSdfAt(const GraphSettings& settings, const SdfPipeline& pipeline, float x, float y, float z);
SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, const SdfPipeline& pipeline, int resolution);
SdfPreviewStats BuildDenseSdfPreviewFromValues(const GraphSettings& settings, int resolution, const std::vector<float>& sdfValues);
}
