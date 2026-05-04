#pragma once

#include "node_graph.h"

namespace rock
{
float EvaluateSdfAt(const GraphSettings& settings, const SdfPipeline& pipeline, float x, float y, float z);
SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, const SdfPipeline& pipeline, int resolution);
}
