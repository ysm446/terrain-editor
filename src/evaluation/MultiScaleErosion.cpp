#include "MultiScaleErosion.h"

#include "HeightfieldOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <execution>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace rock
{
namespace
{
MultiScaleErosionGpuEvaluator g_mseGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}

float SampleHeightValue(const std::vector<float>& values, int resolution, float u, float v)
{
    if (resolution < 2 || values.size() < static_cast<size_t>(resolution * resolution))
    {
        return 0.0f;
    }
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(resolution - 1);
    const float z = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(resolution - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, resolution - 1);
    const int z1 = std::min(z0 + 1, resolution - 1);
    const float tx = x - static_cast<float>(x0);
    const float tz = z - static_cast<float>(z0);
    const auto at = [&](int px, int pz) {
        return values[static_cast<size_t>(pz * resolution + px)];
    };
    const float a = std::lerp(at(x0, z0), at(x1, z0), tx);
    const float b = std::lerp(at(x0, z1), at(x1, z1), tx);
    return std::lerp(a, b, tz);
}

namespace mse
{
constexpr int kCoarsestPyramidLevel = 64;
constexpr float kRefCellSize = 4.0f;
constexpr float kRefCellArea = kRefCellSize * kRefCellSize;

constexpr std::array<std::pair<int, int>, 8> kNext8 = {{
    {0, 1}, {1, 1}, {1, 0}, {1, -1},
    {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
}};

inline int Index1D(int x, int z, int n)
{
    return z * n + x;
}

inline float Hash2(float x, float y)
{
    const float s = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453123f;
    return s - std::floor(s);
}

inline float ValueNoise2D(float x, float y)
{
    const float xi = std::floor(x);
    const float yi = std::floor(y);
    const float xf = x - xi;
    const float yf = y - yi;
    const float a = Hash2(xi, yi);
    const float b = Hash2(xi + 1.0f, yi);
    const float c = Hash2(xi, yi + 1.0f);
    const float d = Hash2(xi + 1.0f, yi + 1.0f);
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    return std::lerp(std::lerp(a, b, u), std::lerp(c, d, v), v) * 2.0f - 1.0f;
}

struct WeightedFlow
{
    std::array<float, 8> w{};
};

inline WeightedFlow GetFlowWeighted(const std::vector<float>& heights, int n, float cellSize, int x, int z, float flowP)
{
    WeightedFlow out{};
    float sum = 0.0f;
    const float h = heights[static_cast<size_t>(Index1D(x, z, n))];
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].first;
        const int qz = z + kNext8[i].second;
        if (qx < 0 || qx >= n || qz < 0 || qz >= n) { out.w[static_cast<size_t>(i)] = 0.0f; continue; }
        const float hq = heights[static_cast<size_t>(Index1D(qx, qz, n))];
        const float dx = static_cast<float>(kNext8[i].first) * cellSize;
        const float dz = static_cast<float>(kNext8[i].second) * cellSize;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float slope = (h - hq) / d;
        if (slope > 0.0f)
        {
            const float w = std::pow(slope, flowP);
            out.w[static_cast<size_t>(i)] = w;
            sum += w;
        }
        else
        {
            out.w[static_cast<size_t>(i)] = 0.0f;
        }
    }
    if (sum > 1e-6f)
    {
        for (float& w : out.w) { w /= sum; }
    }
    return out;
}

inline void GetSteepestDescent(const std::vector<float>& heights, int n, float cellSize, int x, int z,
                               int& dx, int& dz, float& slope)
{
    dx = 0;
    dz = 0;
    slope = 0.0f;
    const float h = heights[static_cast<size_t>(Index1D(x, z, n))];
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].first;
        const int qz = z + kNext8[i].second;
        if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
        const float hq = heights[static_cast<size_t>(Index1D(qx, qz, n))];
        const float ax = static_cast<float>(kNext8[i].first) * cellSize;
        const float az = static_cast<float>(kNext8[i].second) * cellSize;
        const float d = std::sqrt(ax * ax + az * az);
        const float s = (h - hq) / d;
        if (s > slope)
        {
            slope = s;
            dx = kNext8[i].first;
            dz = kNext8[i].second;
        }
    }
}

void StepStreamPower(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                     std::vector<float>& streamIn, std::vector<float>& streamOut,
                     int n, float cellSize, const MultiScaleErosionSettings& s)
{
    const float cellDiag = cellSize * std::sqrt(2.0f);
    const float baseStream = cellDiag;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            float incoming = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const int qx = x + kNext8[i].first;
                const int qz = z + kNext8[i].second;
                if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
                const WeightedFlow wf = GetFlowWeighted(heightsIn, n, cellSize, qx, qz, s.flowExponent);
                const float w = wf.w[static_cast<size_t>((i + 4) % 8)];
                if (w > 0.0f)
                {
                    incoming += w * streamIn[static_cast<size_t>(Index1D(qx, qz, n))];
                }
            }
            const float stream = baseStream + incoming;

            int dx = 0;
            int dz = 0;
            float steepest = 0.0f;
            GetSteepestDescent(heightsIn, n, cellSize, x, z, dx, dz, steepest);
            const float receiverHeight = (dx == 0 && dz == 0)
                ? heightsIn[static_cast<size_t>(id)]
                : heightsIn[static_cast<size_t>(Index1D(x + dx, z + dz, n))];

            float spe = std::pow(stream, s.streamExponent) * std::clamp(std::pow(steepest, s.slopeExponent), 0.0f, 1.0f);
            spe = std::clamp(spe, 0.0f, s.maxStreamPower) * s.speStrength;

            const float oldHeight = heightsIn[static_cast<size_t>(id)];
            float newHeight = oldHeight - s.speTimeStep * spe;
            newHeight = std::max(newHeight, receiverHeight);
            heightsOut[static_cast<size_t>(id)] = newHeight;
            streamOut[static_cast<size_t>(id)] = stream;
        }
    });
}

void StepThermal(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                 int n, float cellSize, const MultiScaleErosionSettings& s)
{
    const float baseTan = std::tan(s.thermalAngleDegrees * 3.14159265358979323846f / 180.0f);
    const float matter = s.thermalStrength * kRefCellArea;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            const float h = heightsIn[static_cast<size_t>(id)];

            float tanAngle = baseTan;
            if (s.thermalNoisifyAngle)
            {
                const float t = ValueNoise2D(static_cast<float>(x) * s.thermalNoiseWavelength * static_cast<float>(n),
                                             static_cast<float>(z) * s.thermalNoiseWavelength * static_cast<float>(n)) * 0.5f + 0.5f;
                tanAngle = baseTan * std::lerp(s.thermalNoiseMin, s.thermalNoiseMax, t);
            }

            float receiveMul = 0.0f;
            float distributeMul = 0.0f;
            for (int j = -1; j <= 1; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    if (i == 0 && j == 0) { continue; }
                    const int qx = ((x + i) % n + n) % n;
                    const int qz = ((z + j) % n + n) % n;
                    const float ax = static_cast<float>(i) * cellSize;
                    const float az = static_cast<float>(j) * cellSize;
                    const float d = std::sqrt(ax * ax + az * az);
                    const float hq = heightsIn[static_cast<size_t>(Index1D(qx, qz, n))];
                    if ((hq - h) / d > tanAngle) { receiveMul += 1.0f; }
                    if ((h - hq) / d > tanAngle) { distributeMul += 1.0f; }
                }
            }
            heightsOut[static_cast<size_t>(id)] = h + matter * (receiveMul - distributeMul);
        }
    });
}

void StepDeposition(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                    std::vector<float>& streamIn, std::vector<float>& streamOut,
                    std::vector<float>& sedIn, std::vector<float>& sedOut,
                    int n, float cellSize, const MultiScaleErosionSettings& s)
{
    constexpr float cellArea = kRefCellArea * 0.00001f;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            const float h = heightsIn[static_cast<size_t>(id)];
            float sed = sedIn[static_cast<size_t>(id)];

            int dx = 0;
            int dz = 0;
            float steepest = 0.0f;
            GetSteepestDescent(heightsIn, n, cellSize, x, z, dx, dz, steepest);
            const bool isPit = (dx == 0 && dz == 0);
            if (!isPit) { sed = 0.0f; }

            float incomingStream = 0.0f;
            float incomingSed = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const int qx = x + kNext8[i].first;
                const int qz = z + kNext8[i].second;
                if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
                const WeightedFlow wf = GetFlowWeighted(heightsIn, n, cellSize, qx, qz, s.flowExponent);
                const float w = wf.w[static_cast<size_t>((i + 4) % 8)];
                if (w > 0.0f)
                {
                    incomingStream += w * streamIn[static_cast<size_t>(Index1D(qx, qz, n))];
                    incomingSed += w * sedIn[static_cast<size_t>(Index1D(qx, qz, n))];
                }
            }
            const float stream = s.rain * cellArea + incomingStream;
            sed += incomingSed;

            const float speed = std::clamp(std::pow(steepest, 2.0f), 0.0f, 1.0f);
            const float streamPower = std::pow(std::max(stream, 1e-12f), 0.3f) * speed;

            float newHeight = h;
            if (s.depositionStrength * sed > streamPower)
            {
                const float deposit = std::min(sed, (s.depositionStrength * sed - streamPower) * 0.1f);
                newHeight += deposit;
                sed = std::max(0.0f, sed - deposit);
            }
            sed += 0.1f * streamPower;

            heightsOut[static_cast<size_t>(id)] = newHeight;
            streamOut[static_cast<size_t>(id)] = stream;
            sedOut[static_cast<size_t>(id)] = sed;
        }
    });
}

inline std::vector<float> ResampleHeightsBilinear(const std::vector<float>& source, int sourceN, int targetN)
{
    std::vector<float> result(static_cast<size_t>(targetN) * static_cast<size_t>(targetN), 0.0f);
    for (int z = 0; z < targetN; ++z)
    {
        const float v = targetN > 1 ? static_cast<float>(z) / static_cast<float>(targetN - 1) : 0.0f;
        for (int x = 0; x < targetN; ++x)
        {
            const float u = targetN > 1 ? static_cast<float>(x) / static_cast<float>(targetN - 1) : 0.0f;
            result[static_cast<size_t>(z * targetN + x)] = SampleHeightValue(source, sourceN, u, v);
        }
    }
    return result;
}

void ApplyMultiScaleErosionSingleLevel(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount || settings.iterations <= 0)
    {
        return;
    }

    if (settings.backend == MultiScaleErosionBackend::GpuCompute && g_mseGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_mseGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
    }

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));

    std::vector<float> heightsA = grid.heights;
    std::vector<float> heightsB(cellCount, 0.0f);
    std::vector<float> streamA(cellCount, 0.0f);
    std::vector<float> streamB(cellCount, 0.0f);
    std::vector<float> sedA(cellCount, 0.0f);
    std::vector<float> sedB(cellCount, 0.0f);

    const int iterations = std::clamp(settings.iterations, 0, 500);
    for (int it = 0; it < iterations; ++it)
    {
        if (settings.enableStreamPower)
        {
            StepStreamPower(heightsA, heightsB, streamA, streamB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
            std::swap(streamA, streamB);
        }
        if (settings.enableThermal)
        {
            StepThermal(heightsA, heightsB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
        }
        if (settings.enableDeposition)
        {
            StepDeposition(heightsA, heightsB, streamA, streamB, sedA, sedB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
            std::swap(streamA, streamB);
            std::swap(sedA, sedB);
        }
    }

    grid.heights = std::move(heightsA);
    grid.flows = std::move(streamA);
    grid.deposits = std::move(sedA);
    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);
    NormalizeHeightfieldFields(grid);
}
} // namespace mse
} // namespace

void ApplyMultiScaleErosion(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings)
{
    const int targetN = grid.resolution;
    const size_t targetCellCount = static_cast<size_t>(targetN) * static_cast<size_t>(targetN);
    if (targetN < 3 || grid.heights.size() < targetCellCount || settings.iterations <= 0)
    {
        return;
    }

    if (!settings.useMultigrid)
    {
        mse::ApplyMultiScaleErosionSingleLevel(grid, settings);
        return;
    }

    std::vector<int> levels;
    for (int r = mse::kCoarsestPyramidLevel; r < targetN; r *= 2)
    {
        levels.push_back(r);
    }
    levels.push_back(targetN);

    if (levels.size() <= 1)
    {
        mse::ApplyMultiScaleErosionSingleLevel(grid, settings);
        return;
    }

    HeightfieldGrid working;
    working.terrainSizeMeters = grid.terrainSizeMeters;
    working.resolution = levels[0];
    working.heights = mse::ResampleHeightsBilinear(grid.heights, targetN, levels[0]);

    for (size_t i = 0; i < levels.size(); ++i)
    {
        if (i > 0)
        {
            std::vector<float> upsampled = mse::ResampleHeightsBilinear(working.heights, levels[i - 1], levels[i]);
            working.heights = std::move(upsampled);
            working.resolution = levels[i];
        }
        mse::ApplyMultiScaleErosionSingleLevel(working, settings);
    }

    grid.heights = std::move(working.heights);
    grid.flows = std::move(working.flows);
    grid.deposits = std::move(working.deposits);
    grid.mask.assign(targetCellCount, 0.0f);
    grid.age.assign(targetCellCount, 0.0f);
}

void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator)
{
    g_mseGpuEvaluator = evaluator;
}
} // namespace rock
