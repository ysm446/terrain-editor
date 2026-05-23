#include "ViewportSerialization.h"

#include <algorithm>

namespace terrain
{
namespace
{

constexpr float kMaxViewportOrbitDistance = 100000.0f;

} // namespace

nlohmann::json MakeViewportJson(const ViewportCameraState& viewport)
{
    return {
        {"yaw", viewport.yaw},
        {"pitch", viewport.pitch},
        {"fovDegrees", viewport.fovDegrees},
        {"orbitDistance", viewport.orbitDistance},
        {"pan", {viewport.pan.x, viewport.pan.y}},
    };
}

void NormalizeViewportCameraState(ViewportCameraState& viewport,
                                  bool migrateCloseOrbitDistance,
                                  float defaultPitch,
                                  float defaultOrbitDistance)
{
    viewport.pitch = std::clamp(viewport.pitch, -1.25f, 1.25f);
    viewport.fovDegrees = std::clamp(viewport.fovDegrees, 15.0f, 90.0f);
    viewport.orbitDistance = std::clamp(viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance);
    if (migrateCloseOrbitDistance && viewport.orbitDistance <= 40.0f)
    {
        viewport.pitch = defaultPitch;
        viewport.orbitDistance = defaultOrbitDistance;
    }
}

void ReadViewportJson(const nlohmann::json& root,
                      ViewportCameraState& viewport,
                      bool migrateCloseOrbitDistance,
                      float defaultPitch,
                      float defaultOrbitDistance)
{
    const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
    viewport.yaw = viewportJson.value("yaw", viewport.yaw);
    viewport.pitch = viewportJson.value("pitch", viewport.pitch);
    viewport.fovDegrees = viewportJson.value("fovDegrees", viewport.fovDegrees);
    viewport.orbitDistance = viewportJson.value("orbitDistance", viewport.orbitDistance);

    NormalizeViewportCameraState(viewport, migrateCloseOrbitDistance, defaultPitch, defaultOrbitDistance);

    if (viewportJson.contains("pan") && viewportJson["pan"].is_array() && viewportJson["pan"].size() == 2)
    {
        viewport.pan = ImVec2(viewportJson["pan"][0].get<float>(), viewportJson["pan"][1].get<float>());
    }
}

} // namespace terrain
