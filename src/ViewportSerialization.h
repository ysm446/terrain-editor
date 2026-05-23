#pragma once

#include <nlohmann/json.hpp>

#include "ViewportMath.h"

namespace terrain
{

nlohmann::json MakeViewportJson(const ViewportCameraState& viewport);
void NormalizeViewportCameraState(ViewportCameraState& viewport,
                                  bool migrateCloseOrbitDistance,
                                  float defaultPitch,
                                  float defaultOrbitDistance);
void ReadViewportJson(const nlohmann::json& root,
                      ViewportCameraState& viewport,
                      bool migrateCloseOrbitDistance,
                      float defaultPitch,
                      float defaultOrbitDistance);

} // namespace terrain
