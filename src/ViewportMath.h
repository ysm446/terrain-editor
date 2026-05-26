#pragma once

#include <imgui.h>

#include "node_graph.h"

namespace terrain
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float xValue, float yValue, float zValue)
        : x(xValue), y(yValue), z(zValue)
    {
    }
};

struct ViewportCameraState
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovDegrees = 45.0f;
    float orbitDistance = 2044.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
};

struct CameraBasis
{
    Vec3 position;
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

struct ProjectedPoint
{
    ImVec2 screen;
    float depth = 0.0f;

    ProjectedPoint() = default;
    ProjectedPoint(const ImVec2& screenValue, float depthValue)
        : screen(screenValue), depth(depthValue)
    {
    }
};

Vec3 Subtract(Vec3 a, Vec3 b);
Vec3 Add(Vec3 a, Vec3 b);
Vec3 Scale(Vec3 value, float scalar);
float Dot(Vec3 a, Vec3 b);
float Length(Vec3 value);
Vec3 Cross(Vec3 a, Vec3 b);
Vec3 Normalize(Vec3 value, Vec3 fallback);

float DefaultViewportOrbitDistance(float terrainSizeMeters, float fovDegrees);
float CameraFocalLengthMmFromFovYDegrees(float fovYDegrees);
float CameraFovYDegreesFromFocalLengthMm(float focalLengthMm);

CameraBasis BuildCameraBasis(const ViewportCameraState& viewport);
ImVec2 ProjectWorldNormalized(const ViewportCameraState& viewport, float x, float y, float z);
ProjectedPoint ProjectWorldToScreen(const ViewportCameraState& viewport, float x, float y, float z, const ImVec2& center, float scale);
float SampleHeightAtWorld(const rock::HeightfieldGrid& grid, float worldX, float worldZ);
bool RayTerrainSquareRange(Vec3 origin, Vec3 dir, float halfSize, float farPlane, float* outEnter, float* outExit);
bool TryPickViewportFocusPoint(const ViewportCameraState& viewport,
                               const rock::HeightfieldGrid& grid,
                               const ImVec2& min,
                               const ImVec2& max,
                               const ImVec2& mouse,
                               float farPlane,
                               Vec3* outPoint,
                               float* outFocusDistance,
                               ImVec2* outScreenPoint);

} // namespace terrain
