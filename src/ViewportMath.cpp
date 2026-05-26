#include "ViewportMath.h"

#include <algorithm>
#include <cmath>

namespace terrain
{
namespace
{

constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;
constexpr float kFullFrameSensorHeightMm = 24.0f;
constexpr float kMaxViewportOrbitDistance = 100000.0f;

} // namespace

Vec3 Subtract(Vec3 a, Vec3 b)
{
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

Vec3 Add(Vec3 a, Vec3 b)
{
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

Vec3 Scale(Vec3 value, float scalar)
{
    return Vec3(value.x * scalar, value.y * scalar, value.z * scalar);
}

float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Length(Vec3 value)
{
    return std::sqrt(Dot(value, value));
}

Vec3 Cross(Vec3 a, Vec3 b)
{
    return Vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

Vec3 Normalize(Vec3 value, Vec3 fallback)
{
    const float lengthSq = Dot(value, value);
    if (lengthSq <= 0.000001f)
    {
        return fallback;
    }
    return Scale(value, 1.0f / std::sqrt(lengthSq));
}

float DefaultViewportOrbitDistance(float terrainSizeMeters, float fovDegrees)
{
    const float terrainSize = std::max(1.0f, terrainSizeMeters);
    const float fovRadians = std::clamp(fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float terrainBoundingRadius = terrainSize * 0.70710678f;
    const float distance = terrainBoundingRadius / std::sin(fovRadians * 0.5f);
    return std::clamp(distance * 1.32f, 1.0f, kMaxViewportOrbitDistance);
}

float CameraFocalLengthMmFromFovYDegrees(float fovYDegrees)
{
    const float fovRadians = std::clamp(fovYDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    return (kFullFrameSensorHeightMm * 0.5f) / std::tan(fovRadians * 0.5f);
}

float CameraFovYDegreesFromFocalLengthMm(float focalLengthMm)
{
    const float defaultFocalLengthMm = CameraFocalLengthMmFromFovYDegrees(45.0f);
    const float safeFocalLength = std::max(0.1f, std::isfinite(focalLengthMm) ? focalLengthMm : defaultFocalLengthMm);
    const float fovRadians = 2.0f * std::atan((kFullFrameSensorHeightMm * 0.5f) / std::max(0.001f, safeFocalLength));
    const float fovDegrees = fovRadians / kDegreesToRadians;
    return std::clamp(std::isfinite(fovDegrees) ? fovDegrees : 45.0f, 15.0f, 90.0f);
}

CameraBasis BuildCameraBasis(const ViewportCameraState& viewport)
{
    const float distance = std::clamp(viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance);
    const float cosPitch = std::cos(viewport.pitch);
    const float sinPitch = std::sin(viewport.pitch);
    const float cosYaw = std::cos(viewport.yaw);
    const float sinYaw = std::sin(viewport.yaw);
    const Vec3 worldUp(0.0f, 1.0f, 0.0f);

    CameraBasis basis;
    basis.position = Vec3(sinYaw * cosPitch * distance, sinPitch * distance, cosYaw * cosPitch * distance);
    basis.forward = Normalize(Scale(basis.position, -1.0f), Vec3(0.0f, 0.0f, -1.0f));
    basis.right = Normalize(Cross(basis.forward, worldUp), Vec3(1.0f, 0.0f, 0.0f));
    basis.up = Normalize(Cross(basis.right, basis.forward), worldUp);
    return basis;
}

ImVec2 ProjectWorldNormalized(const ViewportCameraState& viewport, float x, float y, float z)
{
    const CameraBasis basis = BuildCameraBasis(viewport);
    const Vec3 world(x, y, z);
    const Vec3 view = Subtract(world, basis.position);
    const float cameraX = Dot(view, basis.right);
    const float cameraY = Dot(view, basis.up);
    const float depth = std::max(0.05f, Dot(view, basis.forward));
    const float fovRadians = std::clamp(viewport.fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float perspective = focalLength / depth;
    return ImVec2(cameraX * perspective, -cameraY * perspective);
}

ProjectedPoint ProjectWorldToScreen(const ViewportCameraState& viewport, float x, float y, float z, const ImVec2& center, float scale)
{
    const CameraBasis basis = BuildCameraBasis(viewport);
    const Vec3 world(x, y, z);
    const Vec3 view = Subtract(world, basis.position);
    const float cameraX = Dot(view, basis.right);
    const float cameraY = Dot(view, basis.up);
    const float depth = std::max(0.05f, Dot(view, basis.forward));
    const float fovRadians = std::clamp(viewport.fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float perspective = focalLength / depth;
    return ProjectedPoint(ImVec2(center.x + cameraX * perspective * scale, center.y - cameraY * perspective * scale), depth);
}

float SampleHeightAtWorld(const rock::HeightfieldGrid& grid, float worldX, float worldZ)
{
    const int n = grid.resolution;
    if (n < 2 || grid.heights.size() < static_cast<size_t>(n) * static_cast<size_t>(n))
    {
        return 0.0f;
    }

    const float terrainSize = std::max(1.0f, grid.terrainSizeMeters);
    const float halfSize = terrainSize * 0.5f;
    const float u = std::clamp((worldX + halfSize) / terrainSize, 0.0f, 1.0f);
    const float v = std::clamp((halfSize - worldZ) / terrainSize, 0.0f, 1.0f);
    const float gx = u * static_cast<float>(n - 1);
    const float gz = v * static_cast<float>(n - 1);
    const int x0 = std::clamp(static_cast<int>(std::floor(gx)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(gz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float tx = gx - static_cast<float>(x0);
    const float tz = gz - static_cast<float>(z0);
    const auto h = [&](int x, int z) {
        return grid.heights[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)];
    };
    const float h0 = std::lerp(h(x0, z0), h(x1, z0), tx);
    const float h1 = std::lerp(h(x0, z1), h(x1, z1), tx);
    return std::lerp(h0, h1, tz);
}

bool RayTerrainSquareRange(Vec3 origin, Vec3 dir, float halfSize, float farPlane, float* outEnter, float* outExit)
{
    float tEnter = 0.0f;
    float tExit = farPlane;
    const auto clipAxis = [&](float originAxis, float dirAxis) {
        if (std::abs(dirAxis) < 1e-6f)
        {
            return originAxis >= -halfSize && originAxis <= halfSize;
        }
        float t0 = (-halfSize - originAxis) / dirAxis;
        float t1 = ( halfSize - originAxis) / dirAxis;
        if (t0 > t1)
        {
            std::swap(t0, t1);
        }
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        return tEnter <= tExit;
    };
    if (!clipAxis(origin.x, dir.x) || !clipAxis(origin.z, dir.z))
    {
        return false;
    }
    *outEnter = std::max(0.0f, tEnter);
    *outExit = std::max(*outEnter, tExit);
    return *outEnter <= *outExit;
}

bool TryPickViewportFocusPoint(const ViewportCameraState& viewport,
                               const rock::HeightfieldGrid& grid,
                               const ImVec2& min,
                               const ImVec2& max,
                               const ImVec2& mouse,
                               float farPlane,
                               Vec3* outPoint,
                               float* outFocusDistance,
                               ImVec2* outScreenPoint)
{
    const int n = grid.resolution;
    if (n < 2 || grid.heights.size() < static_cast<size_t>(n) * static_cast<size_t>(n))
    {
        return false;
    }

    const float viewportWidth = std::max(1.0f, max.x - min.x);
    const float viewportHeight = std::max(1.0f, max.y - min.y);
    const float viewportSize = std::min(viewportWidth, viewportHeight);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + viewport.pan.x, (min.y + max.y) * 0.5f + viewport.pan.y);
    const float fovRadians = std::clamp(viewport.fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float cameraX = (mouse.x - center.x) / (focalLength * scale);
    const float cameraY = -(mouse.y - center.y) / (focalLength * scale);

    const CameraBasis basis = BuildCameraBasis(viewport);
    const Vec3 rayDir = Normalize(
        Add(Add(basis.forward, Scale(basis.right, cameraX)), Scale(basis.up, cameraY)),
        basis.forward);

    const float terrainSize = std::max(1.0f, grid.terrainSizeMeters);
    const float halfSize = terrainSize * 0.5f;
    float tStart = 0.0f;
    float tEnd = 0.0f;
    if (!RayTerrainSquareRange(basis.position, rayDir, halfSize, farPlane, &tStart, &tEnd))
    {
        return false;
    }
    tEnd = std::min(tEnd, farPlane);
    if (tEnd <= tStart)
    {
        return false;
    }

    const auto pointAt = [&](float t) {
        return Add(basis.position, Scale(rayDir, t));
    };
    const auto heightDelta = [&](float t) {
        const Vec3 p = pointAt(t);
        return p.y - SampleHeightAtWorld(grid, p.x, p.z);
    };

    float prevT = tStart;
    float prevDelta = heightDelta(prevT);
    if (prevDelta <= 0.0f)
    {
        Vec3 p = pointAt(prevT);
        p.y = SampleHeightAtWorld(grid, p.x, p.z);
        if (outPoint) *outPoint = p;
        if (outFocusDistance) *outFocusDistance = std::max(0.1f, Dot(Subtract(p, basis.position), basis.forward));
        if (outScreenPoint) *outScreenPoint = ProjectWorldToScreen(viewport, p.x, p.y, p.z, center, scale).screen;
        return true;
    }

    constexpr int kRaySteps = 256;
    for (int i = 1; i <= kRaySteps; ++i)
    {
        const float t = std::lerp(tStart, tEnd, static_cast<float>(i) / static_cast<float>(kRaySteps));
        const float delta = heightDelta(t);
        if (delta <= 0.0f || (prevDelta < 0.0f && delta >= 0.0f))
        {
            float lo = prevT;
            float hi = t;
            for (int refine = 0; refine < 16; ++refine)
            {
                const float mid = (lo + hi) * 0.5f;
                if (heightDelta(mid) > 0.0f)
                {
                    lo = mid;
                }
                else
                {
                    hi = mid;
                }
            }
            Vec3 p = pointAt(hi);
            p.y = SampleHeightAtWorld(grid, p.x, p.z);
            if (outPoint) *outPoint = p;
            if (outFocusDistance) *outFocusDistance = std::max(0.1f, Dot(Subtract(p, basis.position), basis.forward));
            if (outScreenPoint) *outScreenPoint = ProjectWorldToScreen(viewport, p.x, p.y, p.z, center, scale).screen;
            return true;
        }
        prevT = t;
        prevDelta = delta;
    }
    return false;
}

} // namespace terrain
