#include "node_graph.h"

#include "sdf_preview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace rock
{
namespace
{
using Microsoft::WRL::ComPtr;

FluvialGpuEvaluator g_fluvialGpuEvaluator = nullptr;
constexpr int kFluvialSeed = 1;
// Strengths for the multi-level pyramid (resolutions 16, 32, 64, 128, 256, 512).
// The three coarsest levels are zeroed because their cellSize >> referenceDetailSize
// makes stepScale large enough that particles teleport across the terrain in one
// or two steps, leaving chunky cell-aligned blocks that show up as rectangular
// artifacts after bilinear upsampling. The active levels (128, 256, 512) keep
// stepScale around 8/4/2, which produces natural-looking flow paths.
constexpr std::array<float, 6> kFluvialLevelStrengths = {0.0f, 0.0f, 0.0f, 0.68f, 0.78f, 0.58f};

std::string NoisePipelineSummary(const SdfPipeline& pipeline)
{
    if (pipeline.noiseLayers.empty())
    {
        return "";
    }
    if (pipeline.noiseLayers.size() == 1)
    {
        const NoiseSettings& noise = pipeline.noiseLayers.front();
        return std::format(" -> noise {:.2f}/{:.2f}/{} seed {}", noise.amplitude, noise.frequency, noise.octaves, noise.seed);
    }
    return std::format(" -> noise x{}", pipeline.noiseLayers.size());
}

struct HeightmapImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::string precision;
    std::vector<float> values;
};

void AddEdge(MeshData& mesh, std::unordered_set<uint64_t>& edgeKeys, uint32_t a, uint32_t b);
void AccumulateNormal(MeshVertex& vertex, float nx, float ny, float nz);

void HashCombine(uint64_t& seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

uint64_t HashFloat(float value)
{
    return static_cast<uint64_t>(std::hash<float>{}(value));
}

uint64_t HashHeightmapSettings(const HeightmapLoadSettings& settings, int resolution)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, static_cast<uint64_t>(std::hash<std::string>{}(settings.path)));
    HashCombine(hash, HashFloat(settings.scaleMeters));
    HashCombine(hash, HashFloat(settings.relativeVerticalScalePercent));
    HashCombine(hash, HashFloat(settings.verticalOffsetMeters));
    HashCombine(hash, static_cast<uint64_t>(settings.simulationResolution));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashShapeSettings(const ShapeSettings& settings, int resolution)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, static_cast<uint64_t>(settings.kind));
    HashCombine(hash, HashFloat(settings.scaleMeters));
    HashCombine(hash, HashFloat(settings.relativeHeightPercent));
    HashCombine(hash, static_cast<uint64_t>(settings.simulationResolution));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashFluvialSettings(const FluvialErosionSettings& settings, int resolution)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, HashFloat(settings.featureSize));
    HashCombine(hash, HashFloat(settings.geologicalAge));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, HashFloat(settings.channelLength));
    HashCombine(hash, HashFloat(settings.erosionStrength));
    HashCombine(hash, HashFloat(settings.channeling));
    HashCombine(hash, HashFloat(settings.friction));
    HashCombine(hash, HashFloat(settings.wearAngleDegrees));
    HashCombine(hash, HashFloat(settings.depositAngleDegrees));
    HashCombine(hash, HashFloat(settings.maxErosionAngleDegrees));
    HashCombine(hash, HashFloat(settings.erosionGranularity));
    HashCombine(hash, HashFloat(settings.flowVolume));
    HashCombine(hash, HashFloat(settings.smallChannelInfluence));
    HashCombine(hash, HashFloat(settings.sedimentVelocity));
    HashCombine(hash, HashFloat(settings.forceVectorX));
    HashCombine(hash, HashFloat(settings.forceVectorY));
    HashCombine(hash, HashFloat(settings.forceVectorZ));
    HashCombine(hash, HashFloat(settings.forceStrength));
    HashCombine(hash, HashFloat(settings.shearX));
    HashCombine(hash, HashFloat(settings.shearY));
    HashCombine(hash, HashFloat(settings.referenceDetailSize));
    HashCombine(hash, HashFloat(settings.sourceTerrainDetailSmoothing));
    HashCombine(hash, static_cast<uint64_t>(settings.useMultigrid ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashHeightmapBlurSettings(const HeightmapBlurSettings& settings, int resolution)
{
    uint64_t hash = 2166136261ull;
    HashCombine(hash, HashFloat(settings.radius));
    HashCombine(hash, HashFloat(settings.strength));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

std::wstring Utf8ToWidePath(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const std::u8string utf8(value.begin(), value.end());
    return std::filesystem::path(utf8).wstring();
}

bool LoadHeightmapImage(const std::string& path, HeightmapImage& image, std::string* error)
{
    if (path.empty())
    {
        if (error != nullptr)
        {
            *error = "No heightmap file selected";
        }
        return false;
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE)
    {
        if (error != nullptr)
        {
            *error = std::format("COM initialization failed: 0x{:08X}", static_cast<unsigned int>(initHr));
        }
        return false;
    }

    const auto cleanup = [&]() {
        if (shouldUninitialize)
        {
            CoUninitialize();
        }
    };

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = std::format("WIC factory creation failed: 0x{:08X}", static_cast<unsigned int>(hr));
        }
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring widePath = Utf8ToWidePath(path);
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to open heightmap image";
        }
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to read heightmap image frame";
        }
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    if (width < 2 || height < 2)
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Heightmap image must be at least 2 x 2 pixels";
        }
        return false;
    }

    image.width = width;
    image.height = height;
    image.values.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

    WICPixelFormatGUID pixelFormat{};
    frame->GetPixelFormat(&pixelFormat);

    if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat8bppGray))
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
        hr = frame->CopyPixels(nullptr, width, static_cast<UINT>(pixels.size()), pixels.data());
        cleanup();
        if (FAILED(hr))
        {
            if (error != nullptr)
            {
                *error = "Failed to copy 8-bit heightmap pixels";
            }
            return false;
        }
        for (size_t i = 0; i < image.values.size(); ++i)
        {
            image.values[i] = static_cast<float>(pixels[i]) / 255.0f;
        }
        image.precision = "8-bit grayscale";
    }
    else if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat16bppGray))
    {
        std::vector<uint16_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
        hr = frame->CopyPixels(nullptr, width * sizeof(uint16_t), static_cast<UINT>(pixels.size() * sizeof(uint16_t)), reinterpret_cast<BYTE*>(pixels.data()));
        cleanup();
        if (FAILED(hr))
        {
            if (error != nullptr)
            {
                *error = "Failed to copy 16-bit heightmap pixels";
            }
            return false;
        }
        for (size_t i = 0; i < image.values.size(); ++i)
        {
            image.values[i] = static_cast<float>(pixels[i]) / 65535.0f;
        }
        image.precision = "16-bit grayscale";
    }
    else if (IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppGrayFloat))
    {
        std::vector<float> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
        hr = frame->CopyPixels(nullptr, width * sizeof(float), static_cast<UINT>(pixels.size() * sizeof(float)), reinterpret_cast<BYTE*>(pixels.data()));
        cleanup();
        if (FAILED(hr))
        {
            if (error != nullptr)
            {
                *error = "Failed to copy float heightmap pixels";
            }
            return false;
        }
        for (size_t i = 0; i < image.values.size(); ++i)
        {
            image.values[i] = std::clamp(pixels[i], 0.0f, 1.0f);
        }
        image.precision = "32-bit float grayscale";
    }
    else
    {
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(&converter);
        if (FAILED(hr))
        {
            cleanup();
            if (error != nullptr)
            {
                *error = "Failed to create heightmap image converter";
            }
            return false;
        }

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr))
        {
            std::vector<uint16_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            hr = converter->CopyPixels(nullptr, width * 4u * sizeof(uint16_t), static_cast<UINT>(pixels.size() * sizeof(uint16_t)), reinterpret_cast<BYTE*>(pixels.data()));
            cleanup();
            if (FAILED(hr))
            {
                if (error != nullptr)
                {
                    *error = "Failed to copy 16-bit color heightmap pixels";
                }
                return false;
            }
            for (size_t i = 0; i < image.values.size(); ++i)
            {
                const uint16_t r = pixels[i * 4u + 0u];
                const uint16_t g = pixels[i * 4u + 1u];
                const uint16_t b = pixels[i * 4u + 2u];
                image.values[i] = (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b)) / 65535.0f;
            }
            image.precision = "16-bit color";
        }
        else
        {
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            if (FAILED(hr))
            {
                cleanup();
                if (error != nullptr)
                {
                    *error = "Failed to convert heightmap image";
                }
                return false;
            }

            std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            hr = converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data());
            cleanup();
            if (FAILED(hr))
            {
                if (error != nullptr)
                {
                    *error = "Failed to copy heightmap pixels";
                }
                return false;
            }
            for (size_t i = 0; i < image.values.size(); ++i)
            {
                const uint8_t r = pixels[i * 4u + 0u];
                const uint8_t g = pixels[i * 4u + 1u];
                const uint8_t b = pixels[i * 4u + 2u];
                image.values[i] = (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b)) / 255.0f;
            }
            image.precision = "8-bit color";
        }
    }

    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

float SampleHeightmap(const HeightmapImage& image, float u, float v)
{
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(image.width - 1u);
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(image.height - 1u);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1u, image.width - 1u);
    const uint32_t y1 = std::min(y0 + 1u, image.height - 1u);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto at = [&](uint32_t px, uint32_t py) {
        return image.values[static_cast<size_t>(py) * image.width + px];
    };
    const float a = std::lerp(at(x0, y0), at(x1, y0), tx);
    const float b = std::lerp(at(x0, y1), at(x1, y1), tx);
    return std::lerp(a, b, ty);
}

float SampleHeightfieldValue(const std::vector<float>& values, int resolution, float u, float v)
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

float Hash01(int x, int y, int seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u;
    h += static_cast<uint32_t>(y) * 668265263u;
    h ^= static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

float KttRandom2(float seedX, float seedY)
{
    const float value = std::abs(std::sin(seedX * 1.29898f + seedY * 7.8233f)) * 1228.543f;
    return value - std::floor(value);
}

HeightfieldGrid BuildHeightfieldFromHeightmap(const HeightmapLoadSettings& settings, int resolution, std::string* message)
{
    HeightfieldGrid grid;
    HeightmapImage image;
    std::string error;
    if (!LoadHeightmapImage(settings.path, image, &error))
    {
        if (message != nullptr)
        {
            *message = error;
        }
        return grid;
    }

    grid.resolution = std::clamp(resolution, 2, 2048);
    grid.terrainSizeMeters = std::max(1.0f, settings.scaleMeters);
    const float verticalRange = grid.terrainSizeMeters * std::max(0.0f, settings.relativeVerticalScalePercent) / 100.0f;
    grid.heights.reserve(static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution));
    grid.mask.assign(static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution), 0.0f);
    grid.deposits.assign(static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution), 0.0f);
    grid.flows.assign(static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution), 0.0f);
    grid.age.assign(static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution), 0.0f);
    for (int z = 0; z < grid.resolution; ++z)
    {
        const float v = grid.resolution > 1 ? static_cast<float>(z) / static_cast<float>(grid.resolution - 1) : 0.0f;
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float u = grid.resolution > 1 ? static_cast<float>(x) / static_cast<float>(grid.resolution - 1) : 0.0f;
            grid.heights.push_back(settings.verticalOffsetMeters + SampleHeightmap(image, u, v) * verticalRange);
        }
    }

    if (message != nullptr)
    {
        *message = std::format(
            "heightmap {}x{} {} -> terrain {}x{} ({:.1f} m)",
            image.width,
            image.height,
            image.precision,
            grid.resolution,
            grid.resolution,
            settings.scaleMeters);
    }
    return grid;
}

HeightfieldGrid BuildHeightfieldFromShape(const ShapeSettings& settings, int resolution, std::string* message)
{
    HeightfieldGrid grid;
    grid.resolution = std::clamp(resolution, 2, 2048);
    grid.terrainSizeMeters = std::max(1.0f, settings.scaleMeters);
    const float heightMeters = grid.terrainSizeMeters * std::max(0.0f, settings.relativeHeightPercent) / 100.0f;
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.reserve(cellCount);
    grid.mask.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    grid.flows.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);

    for (int z = 0; z < grid.resolution; ++z)
    {
        const float v = grid.resolution > 1 ? static_cast<float>(z) / static_cast<float>(grid.resolution - 1) : 0.0f;
        const float nz = v * 2.0f - 1.0f;
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float u = grid.resolution > 1 ? static_cast<float>(x) / static_cast<float>(grid.resolution - 1) : 0.0f;
            const float nx = u * 2.0f - 1.0f;
            float normalizedHeight = 0.0f;
            if (settings.kind == ShapeKind::Hemisphere)
            {
                const float radiusSq = nx * nx + nz * nz;
                normalizedHeight = radiusSq < 1.0f ? std::sqrt(1.0f - radiusSq) : 0.0f;
            }
            else
            {
                normalizedHeight = std::max(0.0f, 1.0f - std::max(std::abs(nx), std::abs(nz)));
            }
            grid.heights.push_back(normalizedHeight * heightMeters);
        }
    }

    if (message != nullptr)
    {
        *message = std::format(
            "{} shape {} x {}, scale {:.0f}m, height {:.0f}m",
            ToString(settings.kind),
            grid.resolution,
            grid.resolution,
            grid.terrainSizeMeters,
            heightMeters);
    }
    return grid;
}

HeightfieldGrid ResampleHeightfieldGrid(const HeightfieldGrid& source, int resolution)
{
    HeightfieldGrid result;
    result.resolution = std::clamp(resolution, 2, std::max(2, source.resolution));
    result.terrainSizeMeters = source.terrainSizeMeters;
    const size_t cellCount = static_cast<size_t>(result.resolution) * static_cast<size_t>(result.resolution);
    result.heights.reserve(cellCount);
    result.mask.assign(cellCount, 0.0f);
    result.deposits.assign(cellCount, 0.0f);
    result.flows.assign(cellCount, 0.0f);
    result.age.assign(cellCount, 0.0f);
    for (int z = 0; z < result.resolution; ++z)
    {
        const float v = result.resolution > 1 ? static_cast<float>(z) / static_cast<float>(result.resolution - 1) : 0.0f;
        for (int x = 0; x < result.resolution; ++x)
        {
            const float u = result.resolution > 1 ? static_cast<float>(x) / static_cast<float>(result.resolution - 1) : 0.0f;
            result.heights.push_back(SampleHeightfieldValue(source.heights, source.resolution, u, v));
        }
    }
    return result;
}

std::vector<float> SmoothHeightfieldHeights(const std::vector<float>& source, int resolution, int radius, int iterations)
{
    if (resolution < 2 || source.size() < static_cast<size_t>(resolution * resolution) || radius <= 0 || iterations <= 0)
    {
        return source;
    }

    const int n = resolution;
    const auto indexAt = [n](int x, int z) {
        return static_cast<size_t>(z * n + x);
    };
    std::vector<float> current = source;
    std::vector<float> next = source;
    const int clampedRadius = std::clamp(radius, 1, std::max(1, n / 8));
    const int clampedIterations = std::clamp(iterations, 1, 8);
    for (int iteration = 0; iteration < clampedIterations; ++iteration)
    {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                float weightedSum = 0.0f;
                float weightSum = 0.0f;
                for (int dz = -clampedRadius; dz <= clampedRadius; ++dz)
                {
                    for (int dx = -clampedRadius; dx <= clampedRadius; ++dx)
                    {
                        const int sx = std::clamp(x + dx, 0, n - 1);
                        const int sz = std::clamp(z + dz, 0, n - 1);
                        const float distance = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                        if (distance > static_cast<float>(clampedRadius) + 0.001f)
                        {
                            continue;
                        }
                        const float weight = 1.0f / (1.0f + distance);
                        weightedSum += current[indexAt(sx, sz)] * weight;
                        weightSum += weight;
                    }
                }
                next[indexAt(x, z)] = weightSum > 0.0f ? weightedSum / weightSum : current[indexAt(x, z)];
            }
        }
        current.swap(next);
    }
    return current;
}

void AddResampledHeightDelta(HeightfieldGrid& target, const HeightfieldGrid& base, const HeightfieldGrid& eroded, float strength)
{
    const int n = target.resolution;
    if (n < 2 || base.resolution != eroded.resolution || strength <= 0.0f)
    {
        return;
    }

    std::vector<float> delta(base.heights.size(), 0.0f);
    for (size_t i = 0; i < delta.size(); ++i)
    {
        delta[i] = eroded.heights[i] - base.heights[i];
    }

    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (target.mask.size() != cellCount)
    {
        target.mask.assign(cellCount, 0.0f);
    }
    if (target.deposits.size() != cellCount)
    {
        target.deposits.assign(cellCount, 0.0f);
    }
    if (target.flows.size() != cellCount)
    {
        target.flows.assign(cellCount, 0.0f);
    }
    if (target.age.size() != cellCount)
    {
        target.age.assign(cellCount, 0.0f);
    }

    for (int z = 0; z < n; ++z)
    {
        const float v = n > 1 ? static_cast<float>(z) / static_cast<float>(n - 1) : 0.0f;
        for (int x = 0; x < n; ++x)
        {
            const float u = n > 1 ? static_cast<float>(x) / static_cast<float>(n - 1) : 0.0f;
            const size_t index = static_cast<size_t>(z * n + x);
            const float heightDelta = SampleHeightfieldValue(delta, base.resolution, u, v) * strength;
            target.heights[index] += heightDelta;
            target.mask[index] += std::min(std::abs(heightDelta) / std::max(target.terrainSizeMeters * 0.015f, 0.0001f), 1.0f);
            if (!eroded.mask.empty())
            {
                target.mask[index] += SampleHeightfieldValue(eroded.mask, eroded.resolution, u, v) * strength;
            }
            if (!eroded.deposits.empty())
            {
                target.deposits[index] += SampleHeightfieldValue(eroded.deposits, eroded.resolution, u, v) * strength;
            }
            if (!eroded.flows.empty())
            {
                target.flows[index] += SampleHeightfieldValue(eroded.flows, eroded.resolution, u, v) * strength;
            }
            if (!eroded.age.empty())
            {
                target.age[index] += SampleHeightfieldValue(eroded.age, eroded.resolution, u, v) * strength;
            }
        }
    }
}

void NormalizeField(std::vector<float>& field)
{
    float maxValue = 0.0f;
    for (float value : field)
    {
        maxValue = std::max(maxValue, value);
    }
    if (maxValue > 0.000001f)
    {
        for (float& value : field)
        {
            value = std::clamp(value / maxValue, 0.0f, 1.0f);
        }
    }
}

void NormalizeHeightfieldFields(HeightfieldGrid& grid)
{
    NormalizeField(grid.mask);
    NormalizeField(grid.deposits);
    NormalizeField(grid.flows);
    NormalizeField(grid.age);
}

void SelectHeightfieldPreviewField(HeightfieldGrid& grid, HeightfieldPreviewField previewField)
{
    if (previewField == HeightfieldPreviewField::Deposits && !grid.deposits.empty())
    {
        grid.mask = grid.deposits;
    }
    else if (previewField == HeightfieldPreviewField::Flows && !grid.flows.empty())
    {
        grid.mask = grid.flows;
    }
    else if (previewField == HeightfieldPreviewField::Age && !grid.age.empty())
    {
        grid.mask = grid.age;
    }
}

void ApplyFluvialErosionSingleLevel(HeightfieldGrid& grid, const FluvialErosionSettings& settings)
{
    const int n = grid.resolution;
    if (n < 3 || grid.heights.size() < static_cast<size_t>(n * n) || settings.iterations <= 0 || settings.erosionStrength <= 0.0f)
    {
        return;
    }

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));
    const float detailScale = std::max(settings.referenceDetailSize, cellSize * 0.001f);
    const float stepScale = cellSize / detailScale;
    const float flowCutting = (cellSize <= detailScale ? 1.0f : 0.0f) * std::clamp(settings.flowVolume, 0.0f, 2.0f);

    const float wearSlope = std::tan(std::clamp(settings.wearAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float depositSlope = std::tan(std::clamp(settings.depositAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float maxSlope = std::tan(std::clamp(settings.maxErosionAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float flowStrength = std::clamp(settings.erosionStrength, 0.0f, 1.0f);
    const float channeling = std::clamp(settings.channeling, 0.0f, 1.0f);
    const float friction = std::clamp(settings.friction, 0.0f, 1.0f);
    const float velocityScale = std::clamp(settings.sedimentVelocity, 0.0f, 2.0f);
    const float granularity = std::clamp(settings.erosionGranularity + 1.0f, 1.0f, 101.0f);
    const float smallChannelScale = std::clamp(settings.smallChannelInfluence, 0.0f, 1.0f);
    const float forceScale = std::clamp(settings.forceStrength, 0.0f, 1.0f);
    const float forceX0 = settings.forceVectorX * forceScale;
    const float forceZ0 = settings.forceVectorZ * forceScale;
    const float shearXVal = std::clamp(settings.shearX, -0.5f, 0.5f);
    const float shearZVal = std::clamp(settings.shearY, -0.5f, 0.5f);

    const int maxSteps = std::clamp(static_cast<int>(std::round(settings.channelLength / std::max(stepScale, 0.0001f))), 1, 4096);
    const float featureCells = std::max(settings.featureSize, cellSize) / cellSize;
    const int particleStride = std::max(1, static_cast<int>(std::round(featureCells * 0.18f)));

    const float ageScale = std::clamp(settings.geologicalAge / 20.0f, 0.1f, 4.0f);
    const int iterations = std::max(1, static_cast<int>(std::round(settings.iterations * std::clamp(ageScale, 0.35f, 2.5f))));

    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    const float bedrockFloor = *std::ranges::min_element(grid.heights);

    const auto indexAt = [n](int x, int z) { return static_cast<size_t>(z * n + x); };
    const auto clampCoord = [n](int v) { return std::clamp(v, 0, n - 1); };
    const auto sampleHeight = [&](float x, float z) {
        const float fx = std::clamp(x, 0.0f, static_cast<float>(n - 1));
        const float fz = std::clamp(z, 0.0f, static_cast<float>(n - 1));
        const int x0 = static_cast<int>(std::floor(fx));
        const int z0 = static_cast<int>(std::floor(fz));
        const int x1 = std::min(x0 + 1, n - 1);
        const int z1 = std::min(z0 + 1, n - 1);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        return std::lerp(
            std::lerp(grid.heights[indexAt(x0, z0)], grid.heights[indexAt(x1, z0)], tx),
            std::lerp(grid.heights[indexAt(x0, z1)], grid.heights[indexAt(x1, z1)], tx),
            tz);
    };

    struct BilinearWeights { std::array<size_t, 4> idx; std::array<float, 4> w; };
    const auto bilinearAt = [&](float x, float z) {
        const float fx = std::clamp(x, 0.0f, static_cast<float>(n - 1));
        const float fz = std::clamp(z, 0.0f, static_cast<float>(n - 1));
        const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, n - 1);
        const int z0 = std::clamp(static_cast<int>(std::floor(fz)), 0, n - 1);
        const int x1 = std::min(x0 + 1, n - 1);
        const int z1 = std::min(z0 + 1, n - 1);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        return BilinearWeights{
            {indexAt(x0, z0), indexAt(x1, z0), indexAt(x0, z1), indexAt(x1, z1)},
            {(1.0f - tx) * (1.0f - tz), tx * (1.0f - tz), (1.0f - tx) * tz, tx * tz}};
    };
    const auto sampleField = [&](const std::vector<float>& field, float x, float z) {
        const auto bw = bilinearAt(x, z);
        return field[bw.idx[0]] * bw.w[0] + field[bw.idx[1]] * bw.w[1]
             + field[bw.idx[2]] * bw.w[2] + field[bw.idx[3]] * bw.w[3];
    };
    const auto splatField = [&](std::vector<float>& field, float x, float z, float amount) {
        if (std::abs(amount) < 1e-7f) { return; }
        const auto bw = bilinearAt(x, z);
        for (int i = 0; i < 4; ++i) { field[bw.idx[i]] += amount * bw.w[i]; }
    };

    std::vector<float> forceX(cellCount, 0.0f);
    std::vector<float> forceZ(cellCount, 0.0f);
    std::vector<float> erosionField(cellCount, 0.0f);
    std::vector<float> depositField(cellCount, 0.0f);
    std::vector<float> flowField(cellCount, 0.0f);
    std::vector<float> maskField(cellCount, 0.0f);
    std::vector<float> visitMask(cellCount, 0.0f);
    std::vector<float> ageField(cellCount, 0.0f);

    const auto updateForces = [&]() {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = indexAt(x, z);
                const float baseH = grid.heights[idx] + erosionField[idx] * flowCutting;
                float gx = 0.0f, gz = 0.0f;
                for (int dz = -1; dz <= 1; ++dz)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dz == 0) { continue; }
                        const int nx = clampCoord(x + dx);
                        const int nz = clampCoord(z + dz);
                        const float sampleH = grid.heights[indexAt(nx, nz)] + erosionField[indexAt(nx, nz)] * flowCutting;
                        const float dist = std::sqrt(static_cast<float>(dx * dx + dz * dz)) + 0.0001f;
                        gx += static_cast<float>(dx) * (sampleH - baseH) / dist;
                        gz += static_cast<float>(dz) * (sampleH - baseH) / dist;
                    }
                }
                forceX[idx] = -gx / (cellSize * 6.0f) + forceX0;
                forceZ[idx] = -gz / (cellSize * 6.0f) + forceZ0;
                ageField[idx] += 0.1f * cellSize;
            }
        }
    };

    updateForces();

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        updateForces();
        std::fill(visitMask.begin(), visitMask.end(), 0.0f);

        const float iterValRaw = static_cast<float>(iteration + kFluvialSeed);
        const float clampX = static_cast<float>(n - 1);
        const float clampZ = static_cast<float>(n - 1);
        const float iterOffsetX = KttRandom2(iterValRaw * 11.137f + 7.31f, 17.93f) * clampX;
        const float iterOffsetZ = KttRandom2(iterValRaw * 23.719f + 41.51f, 53.17f) * clampZ;

        for (int z = 1; z < n - 1; z += particleStride)
        {
            for (int x = 1; x < n - 1; x += particleStride)
            {
                float px = iterOffsetX + KttRandom2(static_cast<float>(x) * 1.8f * cellSize, static_cast<float>(z) / 49.2f * cellSize) * clampX;
                px = (px / clampX - std::floor(px / clampX)) * clampX;
                float pz = iterOffsetZ + KttRandom2(static_cast<float>(x) / 1.345f * cellSize + 203.12f, static_cast<float>(z) * cellSize + 502.23f) * clampZ;
                pz = (pz / clampZ - std::floor(pz / clampZ)) * clampZ;

                const int sx = clampCoord(static_cast<int>(px));
                const int sz = clampCoord(static_cast<int>(pz));
                const size_t startIdx = indexAt(sx, sz);

                const float startSlope = std::sqrt(forceX[startIdx] * forceX[startIdx] + forceZ[startIdx] * forceZ[startIdx]);
                const float centerH = grid.heights[startIdx];
                float curvature = 0.0f;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        curvature += (grid.heights[indexAt(clampCoord(sx + dx), clampCoord(sz + dz))] - centerH) / (cellSize * 8.0f);
                if (std::max(startSlope, -curvature) <= wearSlope) { continue; }

                const float exponent = cellSize > detailScale ? 2.0f : 2.0f - 2.0f * smallChannelScale;
                const float scaleRatio = std::max(cellSize / detailScale, 0.0001f);
                const float denom = std::max(granularity / std::pow(scaleRatio, exponent), 0.0001f);
                const float granularityThreshold = std::clamp(1.0f - 1.0f / denom, 0.0f, 0.999f);
                if (KttRandom2(static_cast<float>(x) + 1042.1f, static_cast<float>(z) + iterValRaw) <= granularityThreshold) { continue; }

                if (visitMask[startIdx] > 0.1f) { continue; }
                visitMask[startIdx] = 1.0f;

                const float startFx = forceX[startIdx];
                const float startFz = forceZ[startIdx];
                const float startForceLen = std::sqrt(startFx * startFx + startFz * startFz);

                float vx = 0.0f, vz = 0.0f, carry = 0.0f;

                for (int step = 0; step < maxSteps; ++step)
                {
                    px += velocityScale * vx / (1.0f + startForceLen);
                    pz += velocityScale * vz / (1.0f + startForceLen);
                    if (px < 0.0f || pz < 0.0f || px > clampX || pz > clampZ) { break; }

                    const float fx = sampleField(forceX, px, pz);
                    const float fz = sampleField(forceZ, px, pz);

                    const float frictionDecay = std::pow(1.0f - friction, stepScale);
                    vx = vx * frictionDecay + fx * stepScale;
                    vz = vz * frictionDecay + fz * stepScale;

                    const float vLen = std::sqrt(vx * vx + vz * vz);
                    if (vLen < 0.00001f) { break; }

                    const float dirXn = vx / vLen;
                    const float dirZn = vz / vLen;
                    const float l1 = std::abs(dirXn) + std::abs(dirZn);
                    const float dirX = dirXn * l1;
                    const float dirZ = dirZn * l1;

                    // Bilinear read of h1 at the particle's fractional position so that
                    // mirror particles read mirror values regardless of sub-cell offset.
                    // Writing back via splatField (4-tap bilinear) preserves the mirror
                    // symmetry that floor(px) / floor(pz) breaks when the dome center
                    // sits at a half-integer cell boundary.
                    const float h1 = sampleHeight(px, pz);
                    const float h2 = sampleHeight(px + dirX + shearXVal, pz + dirZ + shearZVal);
                    const float h3 = sampleHeight(px - dirX + shearXVal, pz - dirZ + shearZVal);

                    const float newSlope = std::sqrt(fx * fx + fz * fz);
                    const bool doErosion = newSlope >= depositSlope && newSlope < maxSlope;
                    if (!doErosion) { break; }

                    float newH = h1 + flowStrength * ((h2 + h3) * 0.5f - h1);
                    newH -= channeling * std::max(newH - h1, 0.0f);
                    carry += h1 - newH;
                    if (carry < 0.0f) { newH = h1 - carry; carry = 0.0f; }
                    newH = std::max(newH, std::min(h2, h3));
                    newH = std::min(newH, std::max(h2, h3));

                    const float eroded = std::max(h1 - newH, 0.0f);
                    const float deposited = std::max(newH - h1, 0.0f);
                    splatField(erosionField, px, pz, eroded);
                    splatField(depositField, px, pz, deposited);
                    splatField(flowField, px, pz, 1.0f);
                    splatField(maskField, px, pz, eroded + deposited * 0.5f);
                    splatField(grid.heights, px, pz, newH - h1);

                    // Age decay: cells that get eroded/deposited become "younger".
                    // Mirrors the kernel's Age[sampleidx] *= pow(0.5, |dh|*10/dx).
                    const float ageDecay = std::pow(0.5f, std::abs(newH - h1) * 10.0f / std::max(cellSize, 0.0001f));
                    const auto bw = bilinearAt(px, pz);
                    for (int i = 0; i < 4; ++i)
                    {
                        ageField[bw.idx[i]] *= std::lerp(1.0f, ageDecay, bw.w[i]);
                    }
                }
            }
        }

        for (float& h : grid.heights) { h = std::max(h, bedrockFloor); }
    }

    grid.deposits = depositField;
    grid.flows = flowField;
    grid.age = ageField;
    grid.mask.resize(cellCount, 0.0f);
    float maxMask = 0.0f;
    for (float v : maskField) { maxMask = std::max(maxMask, v); }
    if (maxMask > 1e-6f)
    {
        for (size_t i = 0; i < cellCount; ++i)
        {
            grid.mask[i] = std::clamp(maskField[i] / maxMask, 0.0f, 1.0f);
        }
    }
}

void ApplyHeightmapBlur(HeightfieldGrid& grid, const HeightmapBlurSettings& settings);

void ApplyFluvialErosion(HeightfieldGrid& grid, const FluvialErosionSettings& settings)
{
    const int n = grid.resolution;
    if (n < 3 || grid.heights.size() < static_cast<size_t>(n * n) || settings.iterations <= 0 || settings.erosionStrength <= 0.0f)
    {
        return;
    }

    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);

    // Source Terrain Detail Smoothing: optional Gaussian pre-smooth on the input
    // so noisy heightmaps yield cleaner flow paths. Mirrors the HDA parameter.
    const float smoothing = std::clamp(settings.sourceTerrainDetailSmoothing, 0.0f, 10.0f);
    if (smoothing > 0.001f)
    {
        HeightmapBlurSettings blur;
        blur.radius = 1.5f;
        blur.strength = 1.0f;
        blur.iterations = std::max(1, static_cast<int>(std::round(smoothing)));
        ApplyHeightmapBlur(grid, blur);
    }

    if (!settings.useMultigrid)
    {
        grid.mask.assign(cellCount, 0.0f);
        grid.deposits.assign(cellCount, 0.0f);
        grid.flows.assign(cellCount, 0.0f);
        grid.age.assign(cellCount, 0.0f);
        ApplyFluvialErosionSingleLevel(grid, settings);
        NormalizeHeightfieldFields(grid);
        return;
    }

    const std::array<int, kFluvialLevelStrengths.size()> levelResolutions = {16, 32, 64, 128, 256, 512};

    grid.mask.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    grid.flows.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);

    int previousResolution = 0;
    for (size_t level = 0; level < kFluvialLevelStrengths.size(); ++level)
    {
        const float levelStrength = std::clamp(kFluvialLevelStrengths[level], 0.0f, 2.0f);
        if (levelStrength <= 0.0001f) { continue; }

        const int levelResolution = std::clamp(levelResolutions[level], 16, n);
        if (levelResolution == previousResolution) { continue; }
        previousResolution = levelResolution;

        HeightfieldGrid base = ResampleHeightfieldGrid(grid, levelResolution);
        HeightfieldGrid eroded = base;

        FluvialErosionSettings levelSettings = settings;
        const float detailT = static_cast<float>(level) / static_cast<float>(std::max<size_t>(1, kFluvialLevelStrengths.size() - 1));
        const float levelFade = detailT < 0.2f ? std::lerp(0.35f, 1.0f, detailT / 0.2f) : 1.0f;
        levelSettings.erosionStrength = std::clamp(settings.erosionStrength * levelStrength, 0.0f, 1.0f);
        levelSettings.iterations = std::clamp(static_cast<int>(std::round(static_cast<float>(settings.iterations) * levelFade)), 1, 180);
        levelSettings.channelLength = settings.channelLength * std::lerp(2.8f, 0.75f, detailT);
        levelSettings.featureSize = settings.featureSize * std::lerp(4.0f, 0.75f, detailT);
        levelSettings.channeling = std::clamp(settings.channeling * std::lerp(1.65f, 0.85f, detailT), 0.0f, 1.0f);
        levelSettings.erosionGranularity = std::clamp(settings.erosionGranularity * std::lerp(0.45f, 1.2f, detailT), 1.0f, 100.0f);

        ApplyFluvialErosionSingleLevel(eroded, levelSettings);
        AddResampledHeightDelta(grid, base, eroded, 1.0f);

        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const float u = n > 1 ? static_cast<float>(x) / static_cast<float>(n - 1) : 0.0f;
                const float v = n > 1 ? static_cast<float>(z) / static_cast<float>(n - 1) : 0.0f;
                const size_t idx = static_cast<size_t>(z * n + x);
                grid.mask[idx] += SampleHeightfieldValue(eroded.mask, eroded.resolution, u, v);
                grid.deposits[idx] += SampleHeightfieldValue(eroded.deposits, eroded.resolution, u, v);
                grid.flows[idx] += SampleHeightfieldValue(eroded.flows, eroded.resolution, u, v);
                grid.age[idx] += SampleHeightfieldValue(eroded.age, eroded.resolution, u, v);
            }
        }
    }

    NormalizeHeightfieldFields(grid);
}

void ApplyHeightmapBlur(HeightfieldGrid& grid, const HeightmapBlurSettings& settings)
{
    const int n = grid.resolution;
    if (n < 2 || grid.heights.size() < static_cast<size_t>(n * n) || settings.radius <= 0.0f || settings.strength <= 0.0f || settings.iterations <= 0)
    {
        return;
    }

    const float radius = std::clamp(settings.radius, 0.0f, 128.0f);
    const float strength = std::clamp(settings.strength, 0.0f, 1.0f);
    const int iterations = std::clamp(settings.iterations, 0, 64);
    const int kernelRadius = std::clamp(static_cast<int>(std::ceil(radius)), 1, std::max(1, n - 1));
    const float sigma = std::max(radius * 0.5f, 0.5f);

    std::vector<float> weights(static_cast<size_t>(kernelRadius) + 1u);
    float weightSum = 0.0f;
    for (int offset = 0; offset <= kernelRadius; ++offset)
    {
        const float x = static_cast<float>(offset) / sigma;
        const float weight = std::exp(-0.5f * x * x);
        weights[static_cast<size_t>(offset)] = weight;
        weightSum += offset == 0 ? weight : weight * 2.0f;
    }
    for (float& weight : weights)
    {
        weight /= weightSum;
    }

    std::vector<float> temp(grid.heights.size(), 0.0f);
    std::vector<float> blurred(grid.heights.size(), 0.0f);
    const auto indexAt = [n](int x, int z) {
        return static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
    };

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const std::vector<float>& source = grid.heights;
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                float value = source[indexAt(x, z)] * weights[0];
                for (int offset = 1; offset <= kernelRadius; ++offset)
                {
                    const int left = std::clamp(x - offset, 0, n - 1);
                    const int right = std::clamp(x + offset, 0, n - 1);
                    const float weight = weights[static_cast<size_t>(offset)];
                    value += (source[indexAt(left, z)] + source[indexAt(right, z)]) * weight;
                }
                temp[indexAt(x, z)] = value;
            }
        }

        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                float value = temp[indexAt(x, z)] * weights[0];
                for (int offset = 1; offset <= kernelRadius; ++offset)
                {
                    const int up = std::clamp(z - offset, 0, n - 1);
                    const int down = std::clamp(z + offset, 0, n - 1);
                    const float weight = weights[static_cast<size_t>(offset)];
                    value += (temp[indexAt(x, up)] + temp[indexAt(x, down)]) * weight;
                }
                blurred[indexAt(x, z)] = value;
            }
        }

        for (size_t i = 0; i < grid.heights.size(); ++i)
        {
            grid.heights[i] = std::lerp(source[i], blurred[i], strength);
        }
    }
}

MeshData BuildMeshFromHeightfield(const HeightfieldGrid& grid, int meshResolution)
{
    MeshData mesh;
    const int gridResolution = grid.resolution;
    if (gridResolution < 2 || grid.heights.size() < static_cast<size_t>(gridResolution * gridResolution))
    {
        return mesh;
    }
    meshResolution = std::clamp(meshResolution, 2, 512);

    const float halfSize = grid.terrainSizeMeters * 0.5f;
    const size_t surfaceVertexCount = static_cast<size_t>(meshResolution) * static_cast<size_t>(meshResolution);
    mesh.vertices.reserve(surfaceVertexCount * 2u + static_cast<size_t>(meshResolution) * 8u);
    mesh.triangles.reserve(static_cast<size_t>(meshResolution - 1) * static_cast<size_t>(meshResolution - 1) * 4u + static_cast<size_t>(meshResolution - 1) * 8u);
    mesh.edges.reserve(mesh.triangles.capacity() * 3u);

    for (int z = 0; z < meshResolution; ++z)
    {
        const float v = meshResolution > 1 ? static_cast<float>(z) / static_cast<float>(meshResolution - 1) : 0.0f;
        for (int x = 0; x < meshResolution; ++x)
        {
            const float u = meshResolution > 1 ? static_cast<float>(x) / static_cast<float>(meshResolution - 1) : 0.0f;
            const float height = SampleHeightfieldValue(grid.heights, gridResolution, u, v);
            mesh.vertices.push_back({
                std::lerp(-halfSize, halfSize, u),
                height,
                std::lerp(halfSize, -halfSize, v),
                0.0f,
                0.0f,
                0.0f,
                SampleHeightfieldValue(grid.mask, gridResolution, u, v),
            });
        }
    }

    std::unordered_set<uint64_t> edgeKeys;
    const auto indexAt = [meshResolution](int x, int z) {
        return static_cast<uint32_t>(z * meshResolution + x);
    };
    const auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c, bool keepWinding = false) {
        const MeshVertex& va = mesh.vertices[a];
        const MeshVertex& vb = mesh.vertices[b];
        const MeshVertex& vc = mesh.vertices[c];
        const float ux = vb.x - va.x;
        const float uy = vb.y - va.y;
        const float uz = vb.z - va.z;
        const float vx = vc.x - va.x;
        const float vy = vc.y - va.y;
        const float vz = vc.z - va.z;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        if (!keepWinding && ny < 0.0f)
        {
            std::swap(b, c);
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }
        AccumulateNormal(mesh.vertices[a], nx, ny, nz);
        AccumulateNormal(mesh.vertices[b], nx, ny, nz);
        AccumulateNormal(mesh.vertices[c], nx, ny, nz);
        mesh.triangles.push_back({a, b, c});
        AddEdge(mesh, edgeKeys, a, b);
        AddEdge(mesh, edgeKeys, b, c);
        AddEdge(mesh, edgeKeys, c, a);
    };

    for (int z = 0; z < meshResolution - 1; ++z)
    {
        for (int x = 0; x < meshResolution - 1; ++x)
        {
            const uint32_t a = indexAt(x, z);
            const uint32_t b = indexAt(x + 1, z);
            const uint32_t c = indexAt(x + 1, z + 1);
            const uint32_t d = indexAt(x, z + 1);
            addTriangle(a, b, c);
            addTriangle(a, c, d);
        }
    }

    const float baseY = 0.0f;
    const auto addVertex = [&](float x, float y, float z, float mask = 0.0f) {
        const uint32_t index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({x, y, z, 0.0f, 0.0f, 0.0f, mask});
        return index;
    };
    const auto addWallSegment = [&](uint32_t topA, uint32_t topB) {
        const MeshVertex& a = mesh.vertices[topA];
        const MeshVertex& b = mesh.vertices[topB];
        const uint32_t sideTopA = addVertex(a.x, a.y, a.z, a.mask);
        const uint32_t sideTopB = addVertex(b.x, b.y, b.z, b.mask);
        const uint32_t sideBottomA = addVertex(a.x, baseY, a.z, a.mask);
        const uint32_t sideBottomB = addVertex(b.x, baseY, b.z, b.mask);
        addTriangle(sideTopA, sideBottomA, sideBottomB, true);
        addTriangle(sideTopA, sideBottomB, sideTopB, true);
    };

    for (int x = 0; x < meshResolution - 1; ++x)
    {
        addWallSegment(indexAt(x, 0), indexAt(x + 1, 0));
        addWallSegment(indexAt(x + 1, meshResolution - 1), indexAt(x, meshResolution - 1));
    }
    for (int z = 0; z < meshResolution - 1; ++z)
    {
        addWallSegment(indexAt(0, z + 1), indexAt(0, z));
        addWallSegment(indexAt(meshResolution - 1, z), indexAt(meshResolution - 1, z + 1));
    }

    const uint32_t bottomStart = static_cast<uint32_t>(mesh.vertices.size());
    for (int z = 0; z < meshResolution; ++z)
    {
        const float v = meshResolution > 1 ? static_cast<float>(z) / static_cast<float>(meshResolution - 1) : 0.0f;
        for (int x = 0; x < meshResolution; ++x)
        {
            const float u = meshResolution > 1 ? static_cast<float>(x) / static_cast<float>(meshResolution - 1) : 0.0f;
            addVertex(std::lerp(-halfSize, halfSize, u), baseY, std::lerp(halfSize, -halfSize, v));
        }
    }
    const auto bottomIndexAt = [bottomStart, meshResolution](int x, int z) {
        return bottomStart + static_cast<uint32_t>(z * meshResolution + x);
    };
    for (int z = 0; z < meshResolution - 1; ++z)
    {
        for (int x = 0; x < meshResolution - 1; ++x)
        {
            const uint32_t a = bottomIndexAt(x, z);
            const uint32_t b = bottomIndexAt(x + 1, z);
            const uint32_t c = bottomIndexAt(x + 1, z + 1);
            const uint32_t d = bottomIndexAt(x, z + 1);
            addTriangle(a, c, b, true);
            addTriangle(a, d, c, true);
        }
    }

    for (MeshVertex& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        if (length > 0.000001f)
        {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
        else
        {
            vertex.nx = 0.0f;
            vertex.ny = 1.0f;
            vertex.nz = 0.0f;
        }
    }
    return mesh;
}

std::string OperationPipelineSummary(const SdfPipeline& pipeline)
{
    if (pipeline.operations.empty())
    {
        return "";
    }
    return std::format(" -> {} op{}", pipeline.operations.size(), pipeline.operations.size() == 1 ? "" : "s");
}

template <typename Settings>
int EffectiveMeshResolution(const Settings& settings)
{
    const int divisor = 1 << std::clamp(settings.lod, 0, 4);
    return std::clamp(settings.resolution / divisor, 16, 512);
}

struct QuantizedVertex
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const QuantizedVertex& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedVertexHash
{
    size_t operator()(const QuantizedVertex& value) const
    {
        size_t h = static_cast<size_t>(value.x) * 73856093u;
        h ^= static_cast<size_t>(value.y) * 19349663u;
        h ^= static_cast<size_t>(value.z) * 83492791u;
        return h;
    }
};

uint64_t EdgeKey(uint32_t a, uint32_t b)
{
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

void AddEdge(MeshData& mesh, std::unordered_set<uint64_t>& edgeKeys, uint32_t a, uint32_t b)
{
    const uint64_t key = EdgeKey(a, b);
    if (edgeKeys.insert(key).second)
    {
        mesh.edges.push_back({std::min(a, b), std::max(a, b)});
    }
}

void AccumulateNormal(MeshVertex& vertex, float nx, float ny, float nz)
{
    vertex.nx += nx;
    vertex.ny += ny;
    vertex.nz += nz;
}

void ApplySdfGradientNormals(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf, MeshData& mesh)
{
    const float e = std::max(sdf.voxelSize * 0.35f, 0.001f);
    for (MeshVertex& vertex : mesh.vertices)
    {
        const float dx = EvaluateSdfAt(settings, pipeline, vertex.x + e, vertex.y, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x - e, vertex.y, vertex.z);
        const float dy = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y + e, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y - e, vertex.z);
        const float dz = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z + e) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z - e);
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length > 0.000001f)
        {
            vertex.nx = dx / length;
            vertex.ny = dy / length;
            vertex.nz = dz / length;
        }
    }
}

MeshData BuildMeshFromSdf(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf)
{
    MeshData mesh;
    mesh.vertices.reserve(sdf.surfaceTriangles.size());
    mesh.triangles.reserve(sdf.surfaceTriangles.size());
    mesh.edges.reserve(sdf.surfaceTriangles.size() * 3);

    std::unordered_map<QuantizedVertex, uint32_t, QuantizedVertexHash> vertexMap;
    std::unordered_set<uint64_t> edgeKeys;
    constexpr float kQuantizeScale = 10000.0f;

    const auto vertexIndex = [&](float x, float y, float z) -> uint32_t {
        const QuantizedVertex key{
            static_cast<int>(std::lround(x * kQuantizeScale)),
            static_cast<int>(std::lround(y * kQuantizeScale)),
            static_cast<int>(std::lround(z * kQuantizeScale)),
        };
        if (const auto it = vertexMap.find(key); it != vertexMap.end())
        {
            return it->second;
        }

        const uint32_t index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({x, y, z, 0.0f, 0.0f, 0.0f});
        vertexMap.emplace(key, index);
        return index;
    };

    for (const SurfaceTriangle& triangle : sdf.surfaceTriangles)
    {
        const uint32_t a = vertexIndex(triangle.ax, triangle.ay, triangle.az);
        const uint32_t b = vertexIndex(triangle.bx, triangle.by, triangle.bz);
        const uint32_t c = vertexIndex(triangle.cx, triangle.cy, triangle.cz);
        if (a == b || b == c || c == a)
        {
            continue;
        }

        const float ux = triangle.bx - triangle.ax;
        const float uy = triangle.by - triangle.ay;
        const float uz = triangle.bz - triangle.az;
        const float vx = triangle.cx - triangle.ax;
        const float vy = triangle.cy - triangle.ay;
        const float vz = triangle.cz - triangle.az;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;

        AccumulateNormal(mesh.vertices[a], nx, ny, nz);
        AccumulateNormal(mesh.vertices[b], nx, ny, nz);
        AccumulateNormal(mesh.vertices[c], nx, ny, nz);
        mesh.triangles.push_back({a, b, c});
        AddEdge(mesh, edgeKeys, a, b);
        AddEdge(mesh, edgeKeys, b, c);
        AddEdge(mesh, edgeKeys, c, a);
    }

    for (MeshVertex& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        if (length > 0.000001f)
        {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
        else
        {
            vertex.nx = 0.0f;
            vertex.ny = 1.0f;
            vertex.nz = 0.0f;
        }
    }

    ApplySdfGradientNormals(settings, pipeline, sdf, mesh);

    return mesh;
}

MeshData BuildMeshFromHeightPipeline(const SdfPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap)
{
    HeightfieldGrid grid = pipeline.useShape
        ? BuildHeightfieldFromShape(pipeline.shape, std::clamp(pipeline.shape.simulationResolution, 2, 2048), message)
        : BuildHeightfieldFromHeightmap(pipeline.heightmap, resolution, message);
    if (grid.resolution <= 0)
    {
        return {};
    }
    for (const SdfPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::FluvialErosion)
        {
            ApplyFluvialErosion(grid, operation.fluvialErosion);
        }
        else if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::HeightmapBlur)
        {
            ApplyHeightmapBlur(grid, operation.heightmapBlur);
        }
    }
    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    SelectHeightfieldPreviewField(grid, previewField);
    return BuildMeshFromHeightfield(grid, resolution);
}
} // namespace

MeshData NodeGraph::BuildMeshFromHeightPipelineCached(const SdfPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField)
{
    const GraphId sourceNodeId = pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId;
    if (sourceNodeId == 0)
    {
        return BuildMeshFromHeightPipeline(pipeline, resolution, message, previewField);
    }

    const int simulationResolution = pipeline.useShape
        ? std::clamp(pipeline.shape.simulationResolution, 2, 2048)
        : std::clamp(pipeline.heightmap.simulationResolution, 2, 2048);
    uint64_t inputHash = 0;
    const uint64_t sourceHash = pipeline.useShape
        ? HashShapeSettings(pipeline.shape, simulationResolution)
        : HashHeightmapSettings(pipeline.heightmap, simulationResolution);
    HeightfieldNodeCache& sourceCache = heightfieldCache_[sourceNodeId];
    if (!sourceCache.valid ||
        sourceCache.resolution != simulationResolution ||
        sourceCache.inputHash != inputHash ||
        sourceCache.parameterHash != sourceHash)
    {
        std::string sourceMessage;
        sourceCache.grid = pipeline.useShape
            ? BuildHeightfieldFromShape(pipeline.shape, simulationResolution, &sourceMessage)
            : BuildHeightfieldFromHeightmap(pipeline.heightmap, simulationResolution, &sourceMessage);
        sourceCache.message = sourceMessage;
        sourceCache.valid = true;
        sourceCache.resolution = simulationResolution;
        sourceCache.inputHash = inputHash;
        sourceCache.parameterHash = sourceHash;
        sourceCache.outputHash = sourceHash;
    }

    HeightfieldGrid grid = sourceCache.grid;
    inputHash = sourceCache.outputHash;
    if (message != nullptr)
    {
        *message = sourceCache.message;
    }
    if (grid.resolution <= 0)
    {
        return {};
    }

    for (const SdfPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.nodeId == 0)
        {
            if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::FluvialErosion)
            {
                ApplyFluvialErosion(grid, operation.fluvialErosion);
            }
            else if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::HeightmapBlur)
            {
                ApplyHeightmapBlur(grid, operation.heightmapBlur);
            }
            continue;
        }

        const uint64_t parameterHash = operation.kind == SdfPipeline::HeightfieldOperation::Kind::FluvialErosion
            ? HashFluvialSettings(operation.fluvialErosion, simulationResolution)
            : HashHeightmapBlurSettings(operation.heightmapBlur, simulationResolution);
        HeightfieldNodeCache& operationCache = heightfieldCache_[operation.nodeId];
        if (!operationCache.valid ||
            operationCache.resolution != simulationResolution ||
            operationCache.inputHash != inputHash ||
            operationCache.parameterHash != parameterHash)
        {
            HeightfieldGrid operationGrid = grid;
            if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::FluvialErosion)
            {
                ApplyFluvialErosion(operationGrid, operation.fluvialErosion);
            }
            else if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::HeightmapBlur)
            {
                ApplyHeightmapBlur(operationGrid, operation.heightmapBlur);
            }
            operationCache.grid = std::move(operationGrid);
            operationCache.message.clear();
            operationCache.valid = true;
            operationCache.resolution = simulationResolution;
            operationCache.inputHash = inputHash;
            operationCache.parameterHash = parameterHash;
            operationCache.outputHash = inputHash;
            HashCombine(operationCache.outputHash, parameterHash);
            HashCombine(operationCache.outputHash, static_cast<uint64_t>(operation.nodeId));
        }

        grid = operationCache.grid;
        inputHash = operationCache.outputHash;
    }

    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    SelectHeightfieldPreviewField(grid, previewField);
    return BuildMeshFromHeightfield(grid, resolution);
}

NodeGraph NodeGraph::CreateDefaultTerrainGraph()
{
    NodeGraph graph;
    graph.Evaluate();
    return graph;
}

const std::vector<Node>& NodeGraph::Nodes() const
{
    return nodes_;
}

const std::vector<Link>& NodeGraph::Links() const
{
    return links_;
}

GraphSettings& NodeGraph::Settings()
{
    return settings_;
}

const GraphSettings& NodeGraph::Settings() const
{
    return settings_;
}

const EvaluationSummary& NodeGraph::Evaluation() const
{
    return evaluation_;
}

OutputMeshSettings* NodeGraph::FindOutputMeshSettings(GraphId nodeId)
{
    Node* node = FindMutableNode(nodeId);
    return node != nullptr && node->kind == NodeKind::OutputMesh ? &node->outputMesh : nullptr;
}

const OutputMeshSettings* NodeGraph::FindOutputMeshSettings(GraphId nodeId) const
{
    const Node* node = FindNode(nodeId);
    return node != nullptr && node->kind == NodeKind::OutputMesh ? &node->outputMesh : nullptr;
}

const OutputMeshSettings& NodeGraph::OutputMeshSettingsFor(GraphId nodeId) const
{
    if (const OutputMeshSettings* settings = FindOutputMeshSettings(nodeId))
    {
        return *settings;
    }
    if (const Node* outputNode = FindFirstNode(NodeKind::OutputMesh))
    {
        return outputNode->outputMesh;
    }

    static constexpr OutputMeshSettings fallback{};
    return fallback;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        const auto input = std::ranges::find_if(node.inputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (input != node.inputs.end())
        {
            return &*input;
        }

        const auto output = std::ranges::find_if(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (output != node.outputs.end())
        {
            return &*output;
        }
    }

    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) { return node.id == nodeId; });
    return it == nodes_.end() ? nullptr : &*it;
}

bool NodeGraph::IsInputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Input;
}

bool NodeGraph::IsOutputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Output;
}

bool NodeGraph::PinHasLink(GraphId pinId) const
{
    return std::ranges::any_of(links_, [pinId](const Link& link) {
        return link.startPin == pinId || link.endPin == pinId;
    });
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const
{
    if (startPin == 0 || endPin == 0 || startPin == endPin)
    {
        return false;
    }

    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId || start->valueType != end->valueType)
    {
        return false;
    }

    return (start->kind == PinKind::Output && end->kind == PinKind::Input) ||
           (start->kind == PinKind::Input && end->kind == PinKind::Output);
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin)
{
    if (!CanCreateLink(startPin, endPin))
    {
        return false;
    }

    if (IsInputPin(startPin))
    {
        std::swap(startPin, endPin);
    }

    std::erase_if(links_, [endPin](const Link& link) {
        return link.endPin == endPin;
    });
    links_.push_back({nextLinkId_++, startPin, endPin});
    MarkDirty("Link changed");
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId)
{
    const auto oldSize = links_.size();
    std::erase_if(links_, [linkId](const Link& link) { return link.id == linkId; });
    if (links_.size() == oldSize)
    {
        return false;
    }

    MarkDirty("Link deleted");
    return true;
}

bool NodeGraph::DeleteNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs)
    {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs)
    {
        pinIds.push_back(pin.id);
    }

    std::erase_if(links_, [&](const Link& link) {
        return std::ranges::find(pinIds, link.startPin) != pinIds.end() ||
               std::ranges::find(pinIds, link.endPin) != pinIds.end();
    });
    std::erase_if(nodes_, [nodeId](const Node& candidate) {
        return candidate.id == nodeId;
    });
    if (evaluation_.previewNodeId == nodeId)
    {
        evaluation_.previewNodeId = 0;
        evaluation_.previewPinId = 0;
        evaluation_.previewShowsMask = false;
    }
    MarkDirty("Node deleted");
    return true;
}

GraphId NodeGraph::CreateNode(NodeKind kind)
{
    const GraphId nodeId = AddNode(kind, std::string(ToString(kind)));
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        AddPin(nodeId, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
        break;
    case NodeKind::NoiseWarp:
    case NodeKind::CrackField:
        AddPin(nodeId, PinKind::Input, ValueType::SdfGrid, "SDFGrid");
        AddPin(nodeId, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
        break;
    case NodeKind::OutputMesh:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        break;
    case NodeKind::HeightmapLoad:
    case NodeKind::Shape:
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::FluvialErosion:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Deposits");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Flows");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Age");
        break;
    case NodeKind::HeightmapBlur:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    default:
        break;
    }
    MarkDirty("Node added");
    return nodeId;
}

void NodeGraph::ReplaceNodes(std::vector<Node> nodes)
{
    nodes_ = std::move(nodes);
    nextNodeId_ = 1;
    nextPinId_ = 11;
    for (const Node& node : nodes_)
    {
        nextNodeId_ = std::max(nextNodeId_, node.id + 1);
        for (const Pin& pin : node.inputs)
        {
            nextPinId_ = std::max(nextPinId_, pin.id + 1);
        }
        for (const Pin& pin : node.outputs)
        {
            nextPinId_ = std::max(nextPinId_, pin.id + 1);
        }
    }
    MarkDirty("Project nodes loaded");
}

void NodeGraph::ReplaceLinks(std::vector<Link> links)
{
    links_ = std::move(links);
    nextLinkId_ = 101;
    for (const Link& link : links_)
    {
        nextLinkId_ = std::max(nextLinkId_, link.id + 1);
    }
    MarkDirty("Project links loaded");
}

bool NodeGraph::SetPreviewStage(PreviewStage stage)
{
    if (evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview stage changed to {}", ToString(stage));
    return true;
}

bool NodeGraph::SetPreviewNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == nodeId && evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewNodeId = nodeId;
    evaluation_.previewPinId = 0;
    evaluation_.previewShowsMask = false;
    evaluation_.previewField = HeightfieldPreviewField::Heightmap;
    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview node changed to {}", node->title);
    return true;
}

bool NodeGraph::SetPreviewPin(GraphId pinId)
{
    const Pin* pin = FindPin(pinId);
    if (pin == nullptr || pin->kind != PinKind::Output)
    {
        return false;
    }
    const Node* node = FindNode(pin->nodeId);
    if (node == nullptr)
    {
        return false;
    }

    const bool showsMask = pin->valueType == ValueType::Mask;
    HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
    if (showsMask)
    {
        if (pin->label == "Deposits")
        {
            previewField = HeightfieldPreviewField::Deposits;
        }
        else if (pin->label == "Flows")
        {
            previewField = HeightfieldPreviewField::Flows;
        }
        else if (pin->label == "Age")
        {
            previewField = HeightfieldPreviewField::Age;
        }
    }
    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == node->id &&
        evaluation_.previewPinId == pinId &&
        evaluation_.previewStage == stage &&
        evaluation_.previewShowsMask == showsMask &&
        evaluation_.previewField == previewField)
    {
        return false;
    }

    const bool pipelineChanged = evaluation_.previewNodeId != node->id || evaluation_.previewStage != stage;
    evaluation_.previewNodeId = node->id;
    evaluation_.previewPinId = pinId;
    evaluation_.previewShowsMask = showsMask;
    evaluation_.previewField = previewField;
    evaluation_.previewStage = stage;
    evaluation_.dirty = evaluation_.dirty || pipelineChanged;
    evaluation_.status = std::format("Preview output changed to {}", pin->label);
    return true;
}

PreviewStage NodeGraph::Preview() const
{
    return evaluation_.previewStage;
}

SdfPipeline NodeGraph::PipelineFor(PreviewStage stage) const
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return PipelineTo(NodeKind::PrimitiveSdf);
    case PreviewStage::Noise:
        return PipelineTo(NodeKind::NoiseWarp);
    case PreviewStage::Crack:
        return PipelineTo(NodeKind::CrackField);
    case PreviewStage::Fluvial:
        return PipelineTo(NodeKind::FluvialErosion);
    case PreviewStage::HeightmapBlur:
        return PipelineTo(NodeKind::HeightmapBlur);
    case PreviewStage::Shape:
        return PipelineTo(NodeKind::Shape);
    case PreviewStage::Output:
    default:
        return PipelineTo(NodeKind::OutputMesh);
    }
}

SdfPipeline NodeGraph::PreviewPipeline() const
{
    if (const Node* previewNode = FindNode(evaluation_.previewNodeId))
    {
        return PipelineToNode(*previewNode);
    }
    return PipelineFor(evaluation_.previewStage);
}

SdfPipeline NodeGraph::FinalPipeline() const
{
    return PipelineFor(PreviewStage::Output);
}

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
    evaluation_.finalDirty = true;
    evaluation_.status = std::string(reason);
}

void NodeGraph::SetEvaluationPending(std::string_view status)
{
    evaluation_.dirty = true;
    evaluation_.status = std::string(status);
}

void NodeGraph::ApplyEvaluationResultFrom(const NodeGraph& evaluatedGraph)
{
    heightfieldCache_ = evaluatedGraph.heightfieldCache_;
    evaluation_ = evaluatedGraph.evaluation_;
}

const Node* NodeGraph::FindFirstNode(NodeKind kind) const
{
    const auto it = std::ranges::find_if(nodes_, [kind](const Node& node) {
        return node.kind == kind;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId)
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) {
        return node.id == nodeId;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

const Node* NodeGraph::FindNodeByOutputPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        if (std::ranges::any_of(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; }))
        {
            return &node;
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindUpstreamNode(const Node& node) const
{
    if (node.inputs.empty())
    {
        return nullptr;
    }

    const GraphId inputPin = node.inputs.front().id;
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [inputPin](const Link& link) {
        return link.endPin == inputPin;
    });
    if (linkIt == links_.rend())
    {
        return nullptr;
    }

    return FindNodeByOutputPin(linkIt->startPin);
}

SdfPipeline NodeGraph::PipelineTo(NodeKind targetKind) const
{
    const Node* node = FindFirstNode(targetKind);
    return node != nullptr ? PipelineToNode(*node) : SdfPipeline{};
}

SdfPipeline NodeGraph::PipelineToNode(const Node& targetNode) const
{
    SdfPipeline pipeline;
    const Node* node = &targetNode;
    int guard = 0;
    while (node != nullptr && guard++ < 16)
    {
        if (node->kind == NodeKind::NoiseWarp)
        {
            pipeline.noiseLayers.push_back(node->noise);
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::NoiseWarp,
                node->id,
                node->noise,
                {},
                0.0f,
            });
        }
        else if (node->kind == NodeKind::CrackField)
        {
            pipeline.useCrack = true;
            pipeline.crack = node->crack;
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::CrackField,
                node->id,
                {},
                node->crack,
                0.0f,
            });
        }
        else if (node->kind == NodeKind::OutputMesh)
        {
            pipeline.applyOutputIso = true;
            pipeline.outputIsoValue = node->outputMesh.isoValue;
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::OutputIso,
                node->id,
                {},
                {},
                node->outputMesh.isoValue,
            });
        }
        else if (node->kind == NodeKind::FluvialErosion)
        {
            pipeline.heightfieldOperations.push_back({
                SdfPipeline::HeightfieldOperation::Kind::FluvialErosion,
                node->id,
                node->fluvialErosion,
                {},
            });
        }
        else if (node->kind == NodeKind::HeightmapBlur)
        {
            pipeline.heightfieldOperations.push_back({
                SdfPipeline::HeightfieldOperation::Kind::HeightmapBlur,
                node->id,
                {},
                node->heightmapBlur,
            });
        }
        else if (node->kind == NodeKind::PrimitiveSdf)
        {
            pipeline.hasSource = true;
            pipeline.primitiveKind = node->primitive.kind;
            break;
        }
        else if (node->kind == NodeKind::HeightmapLoad)
        {
            pipeline.hasSource = true;
            pipeline.useHeightmap = true;
            pipeline.heightmapNodeId = node->id;
            pipeline.heightmap = node->heightmap;
            break;
        }
        else if (node->kind == NodeKind::Shape)
        {
            pipeline.hasSource = true;
            pipeline.useHeightmap = true;
            pipeline.useShape = true;
            pipeline.shapeNodeId = node->id;
            pipeline.shape = node->shape;
            break;
        }

        node = FindUpstreamNode(*node);
    }
    std::ranges::reverse(pipeline.noiseLayers);
    std::ranges::reverse(pipeline.operations);
    std::ranges::reverse(pipeline.heightfieldOperations);
    pipeline.useNoise = !pipeline.noiseLayers.empty();
    if (pipeline.useNoise)
    {
        pipeline.noise = pipeline.noiseLayers.back();
    }
    return pipeline;
}

void NodeGraph::Evaluate(int previewMeshResolution)
{
    if (previewMeshResolution <= 0)
    {
        previewMeshResolution = EffectiveMeshResolution(settings_.preview);
    }
    const SdfPipeline previewPipeline = PreviewPipeline();
    const SdfPipeline finalPipeline = FinalPipeline();
    evaluation_.previewIsHeightmap = previewPipeline.useHeightmap;
    evaluation_.previewShowsMask = evaluation_.previewShowsMask && previewPipeline.useHeightmap;
    if (!evaluation_.previewShowsMask)
    {
        evaluation_.previewField = HeightfieldPreviewField::Heightmap;
    }
    evaluation_.previewMessage.clear();
    if (!previewPipeline.hasSource)
    {
        evaluation_.previewSdf = {};
        evaluation_.previewMesh = {};
        evaluation_.previewMessage = "No source node";
    }
    else if (previewPipeline.useHeightmap)
    {
        evaluation_.previewSdf = {};
        evaluation_.previewMesh = BuildMeshFromHeightPipelineCached(previewPipeline, previewMeshResolution, &evaluation_.previewMessage, evaluation_.previewField);
    }
    else
    {
        evaluation_.previewSdf = BuildDenseSdfPreview(settings_, previewPipeline, previewMeshResolution);
        evaluation_.previewMesh = BuildMeshFromSdf(settings_, previewPipeline, evaluation_.previewSdf);
    }
    ++evaluation_.version;
    evaluation_.dirty = false;
    if (!previewPipeline.hasSource)
    {
        evaluation_.status = "No source node";
    }
    else if (previewPipeline.useHeightmap)
    {
        evaluation_.status = std::format(
            "Heightmap preview [{}] -> {} verts / {} tris{}{}",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.finalDirty ? " / output mesh pending" : "",
            evaluation_.previewMesh.vertices.empty() ? " / no mesh" : "");
    }
    else
    {
        evaluation_.status = std::format(
            "{} preview -> {}{}{} -> preview LOD {} / output LOD {} / iso {:.3f} -> {} verts / {} tris{}",
            ToString(evaluation_.previewStage),
            ToString(previewPipeline.primitiveKind),
            OperationPipelineSummary(finalPipeline),
            "",
            settings_.preview.lod,
            OutputMeshSettingsFor().lod,
            OutputMeshSettingsFor().isoValue,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.finalDirty ? " / output mesh pending" : "");
    }
}

void NodeGraph::EvaluateFinal(GraphId outputNodeId)
{
    const OutputMeshSettings& outputMesh = OutputMeshSettingsFor(outputNodeId);
    const int outputMeshResolution = EffectiveMeshResolution(outputMesh);
    const Node* outputNode = FindNode(outputNodeId);
    SdfPipeline finalPipeline = outputNode != nullptr && outputNode->kind == NodeKind::OutputMesh
        ? PipelineToNode(*outputNode)
        : FinalPipeline();
    if (finalPipeline.applyOutputIso)
    {
        finalPipeline.outputIsoValue = outputMesh.isoValue;
        for (SdfPipeline::Operation& operation : finalPipeline.operations)
        {
            if (operation.kind == SdfPipeline::OperationKind::OutputIso)
            {
                operation.isoValue = outputMesh.isoValue;
            }
        }
    }
    GraphSettings finalSettings = settings_;
    if (!finalPipeline.hasSource)
    {
        evaluation_.finalSdf = {};
        evaluation_.finalMesh = {};
    }
    else if (finalPipeline.useHeightmap)
    {
        evaluation_.finalSdf = {};
        evaluation_.finalMesh = BuildMeshFromHeightPipelineCached(finalPipeline, outputMeshResolution, nullptr);
    }
    else
    {
        evaluation_.finalSdf = BuildDenseSdfPreview(finalSettings, finalPipeline, outputMeshResolution);
        evaluation_.finalMesh = BuildMeshFromSdf(finalSettings, finalPipeline, evaluation_.finalSdf);
    }
    ++evaluation_.finalVersion;
    evaluation_.finalDirty = false;
    evaluation_.status = std::format(
        "{} preview / output mesh {}^3 LOD {} iso {:.3f} -> {} verts / {} tris",
        ToString(evaluation_.previewStage),
        evaluation_.finalSdf.resolution,
        outputMesh.lod,
        outputMesh.isoValue,
        evaluation_.finalMesh.vertices.size(),
        evaluation_.finalMesh.triangles.size());
}

GraphId NodeGraph::AddNode(NodeKind kind, std::string title)
{
    const GraphId id = nextNodeId_++;
    nodes_.push_back({id, kind, std::move(title), {}, {}});
    return id;
}

GraphId NodeGraph::AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label)
{
    Node* node = nullptr;
    for (Node& candidate : nodes_)
    {
        if (candidate.id == nodeId)
        {
            node = &candidate;
            break;
        }
    }

    if (node == nullptr)
    {
        return 0;
    }

    const GraphId id = nextPinId_++;
    Pin pin{id, nodeId, kind, valueType, std::move(label)};
    if (kind == PinKind::Input)
    {
        node->inputs.push_back(std::move(pin));
    }
    else
    {
        node->outputs.push_back(std::move(pin));
    }
    return id;
}

void NodeGraph::AddInitialLink(GraphId startPin, GraphId endPin)
{
    links_.push_back({nextLinkId_++, startPin, endPin});
}

std::string_view ToString(PrimitiveKind kind)
{
    switch (kind)
    {
    case PrimitiveKind::Sphere:
        return "Sphere";
    case PrimitiveKind::Box:
        return "Box";
    case PrimitiveKind::Capsule:
        return "Capsule";
    case PrimitiveKind::Ellipsoid:
        return "Ellipsoid";
    case PrimitiveKind::RockBlob:
        return "Rock Blob";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ShapeKind kind)
{
    switch (kind)
    {
    case ShapeKind::Hemisphere:
        return "Hemisphere";
    case ShapeKind::Pyramid:
        return "Pyramid";
    default:
        return "Unknown";
    }
}

std::string_view ToString(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return "Primitive SDF";
    case NodeKind::NoiseWarp:
        return "Noise Warp";
    case NodeKind::CrackField:
        return "Crack Field";
    case NodeKind::OutputMesh:
        return "Output Mesh";
    case NodeKind::HeightmapLoad:
        return "Import Heightmap";
    case NodeKind::FluvialErosion:
        return "Fluvial Erosion";
    case NodeKind::HeightmapBlur:
        return "Heightmap Blur";
    case NodeKind::Shape:
        return "Shape";
    default:
        return "Unknown";
    }
}

std::string_view ToString(PreviewStage stage)
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return "Primitive";
    case PreviewStage::Noise:
        return "Noise Warp";
    case PreviewStage::Crack:
        return "Crack Field";
    case PreviewStage::Fluvial:
        return "Fluvial Erosion";
    case PreviewStage::Output:
        return "Output Mesh";
    case PreviewStage::HeightmapBlur:
        return "Heightmap Blur";
    case PreviewStage::Shape:
        return "Shape";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ValueType type)
{
    switch (type)
    {
    case ValueType::SdfGrid:
        return "SDFGrid";
    case ValueType::Mesh:
        return "Mesh";
    case ValueType::HeightField:
        return "HeightField";
    case ValueType::Mask:
        return "Mask";
    default:
        return "Unknown";
    }
}

PreviewStage PreviewStageFor(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return PreviewStage::Primitive;
    case NodeKind::NoiseWarp:
        return PreviewStage::Noise;
    case NodeKind::CrackField:
        return PreviewStage::Crack;
    case NodeKind::FluvialErosion:
        return PreviewStage::Fluvial;
    case NodeKind::HeightmapBlur:
        return PreviewStage::HeightmapBlur;
    case NodeKind::HeightmapLoad:
        return PreviewStage::Primitive;
    case NodeKind::Shape:
        return PreviewStage::Shape;
    case NodeKind::OutputMesh:
    default:
        return PreviewStage::Output;
    }
}

void SetFluvialGpuEvaluator(FluvialGpuEvaluator evaluator)
{
    g_fluvialGpuEvaluator = evaluator;
}

} // namespace rock
