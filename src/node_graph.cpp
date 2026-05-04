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

uint64_t HashFluvialSettings(const FluvialErosionSettings& settings, int resolution)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(settings.useAdvancedParameters));
    HashCombine(hash, HashFloat(settings.featureSize));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, HashFloat(settings.channelLength));
    HashCombine(hash, HashFloat(settings.erosionStrength));
    HashCombine(hash, HashFloat(settings.channeling));
    HashCombine(hash, HashFloat(settings.friction));
    HashCombine(hash, HashFloat(settings.wearAngleDegrees));
    HashCombine(hash, HashFloat(settings.depositAngleDegrees));
    HashCombine(hash, HashFloat(settings.maxErosionAngleDegrees));
    HashCombine(hash, HashFloat(settings.erosionGranularity));
    HashCombine(hash, HashFloat(settings.sedimentVelocity));
    HashCombine(hash, HashFloat(settings.sedimentCapacity));
    HashCombine(hash, HashFloat(settings.depositionRate));
    for (float levelStrength : settings.levelStrengths)
    {
        HashCombine(hash, HashFloat(levelStrength));
    }
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
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

HeightfieldGrid ResampleHeightfieldGrid(const HeightfieldGrid& source, int resolution)
{
    HeightfieldGrid result;
    result.resolution = std::clamp(resolution, 2, std::max(2, source.resolution));
    result.terrainSizeMeters = source.terrainSizeMeters;
    const size_t cellCount = static_cast<size_t>(result.resolution) * static_cast<size_t>(result.resolution);
    result.heights.reserve(cellCount);
    result.mask.assign(cellCount, 0.0f);
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
        }
    }
}

void NormalizeHeightfieldMask(HeightfieldGrid& grid)
{
    float maxMask = 0.0f;
    for (float value : grid.mask)
    {
        maxMask = std::max(maxMask, value);
    }
    if (maxMask > 0.000001f)
    {
        for (float& value : grid.mask)
        {
            value = std::clamp(value / maxMask, 0.0f, 1.0f);
        }
    }
}

void ApplyGridErosionPass(HeightfieldGrid& grid, const FluvialErosionSettings& settings, int iterationCount, float strengthScale, std::vector<float>& maskField)
{
    const int n = grid.resolution;
    if (n < 3 || iterationCount <= 0)
    {
        return;
    }

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));
    const size_t cellCount = static_cast<size_t>(n * n);
    const auto indexAt = [n](int x, int z) {
        return static_cast<size_t>(z * n + x);
    };
    const auto clampCoord = [n](int value) {
        return std::clamp(value, 0, n - 1);
    };

    std::vector<int> receiver(cellCount, -1);
    std::vector<float> receiverDistance(cellCount, 1.0f);
    std::vector<float> flow(cellCount, 1.0f);
    std::vector<float> delta(cellCount, 0.0f);
    std::vector<size_t> sortedCells(cellCount, 0);
    std::iota(sortedCells.begin(), sortedCells.end(), 0);

    const float wearSlope = std::tan(std::clamp(settings.wearAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float maxSlope = std::tan(std::clamp(settings.maxErosionAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float strength = std::clamp(settings.erosionStrength, 0.0f, 2.0f) * strengthScale;
    const float capacityScale = std::clamp(settings.sedimentCapacity, 0.0f, 2.0f);
    const float depositionRate = std::clamp(settings.depositionRate, 0.0f, 1.0f);
    const float channeling = std::clamp(settings.channeling, 0.0f, 1.0f);

    for (int iteration = 0; iteration < iterationCount; ++iteration)
    {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const size_t index = indexAt(x, z);
                float steepestDrop = 0.0f;
                int bestReceiver = -1;
                float bestDistance = 1.0f;
                const float currentHeight = grid.heights[index];
                for (int dz = -1; dz <= 1; ++dz)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dz == 0)
                        {
                            continue;
                        }
                        const int nx = clampCoord(x + dx);
                        const int nz = clampCoord(z + dz);
                        if (nx == x && nz == z)
                        {
                            continue;
                        }
                        const float distance = (dx != 0 && dz != 0) ? 1.41421356f : 1.0f;
                        const size_t neighborIndex = indexAt(nx, nz);
                        const float drop = (currentHeight - grid.heights[neighborIndex]) / distance;
                        if (drop > steepestDrop)
                        {
                            steepestDrop = drop;
                            bestReceiver = static_cast<int>(neighborIndex);
                            bestDistance = distance;
                        }
                    }
                }
                receiver[index] = bestReceiver;
                receiverDistance[index] = bestDistance;
            }
        }

        std::fill(flow.begin(), flow.end(), 1.0f);
        std::ranges::sort(sortedCells, [&](size_t a, size_t b) {
            return grid.heights[a] > grid.heights[b];
        });
        for (const size_t index : sortedCells)
        {
            const int next = receiver[index];
            if (next >= 0)
            {
                flow[static_cast<size_t>(next)] += flow[index];
            }
        }

        float maxFlow = 1.0f;
        for (float value : flow)
        {
            maxFlow = std::max(maxFlow, value);
        }
        const float logMaxFlow = std::log1p(maxFlow);
        for (float& value : flow)
        {
            value = logMaxFlow > 0.0001f ? std::log1p(value) / logMaxFlow : 0.0f;
        }

        std::fill(delta.begin(), delta.end(), 0.0f);
        for (int z = 1; z < n - 1; ++z)
        {
            for (int x = 1; x < n - 1; ++x)
            {
                const size_t index = indexAt(x, z);
                const int next = receiver[index];
                if (next < 0)
                {
                    continue;
                }
                const size_t receiverIndex = static_cast<size_t>(next);
                const float distanceMeters = std::max(cellSize * receiverDistance[index], 0.0001f);
                const float drop = std::max(grid.heights[index] - grid.heights[receiverIndex], 0.0f);
                const float slope = drop / distanceMeters;
                if (slope > maxSlope)
                {
                    continue;
                }

                const float flowWeight = std::pow(std::clamp(flow[index], 0.0f, 1.0f), 1.35f);
                const float wearGate = std::clamp((slope + flowWeight * 0.10f) / std::max(wearSlope + 0.10f, 0.0001f), 0.0f, 1.0f);
                const float capacity = (drop * 0.16f + slope * cellSize * 0.08f) * (0.25f + flowWeight * 2.4f) * capacityScale;
                const float erodeAmount = std::min(drop * 0.72f, capacity * strength * wearGate * (0.65f + channeling));
                const float depositAmount = erodeAmount * depositionRate * std::lerp(0.18f, 0.42f, flowWeight);
                delta[index] -= erodeAmount;
                delta[receiverIndex] += depositAmount;
                maskField[index] += erodeAmount;
                maskField[receiverIndex] += depositAmount * 0.5f;
            }
        }

        for (size_t i = 0; i < cellCount; ++i)
        {
            grid.heights[i] += delta[i];
        }
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
    const auto indexAt = [n](int x, int z) {
        return static_cast<size_t>(z * n + x);
    };
    const auto clampCoord = [n](int value) {
        return std::clamp(value, 0, n - 1);
    };
    const auto sampleHeight = [&](float x, float z) {
        const float fx = std::clamp(x, 0.0f, static_cast<float>(n - 1));
        const float fz = std::clamp(z, 0.0f, static_cast<float>(n - 1));
        const int x0 = static_cast<int>(std::floor(fx));
        const int z0 = static_cast<int>(std::floor(fz));
        const int x1 = std::min(x0 + 1, n - 1);
        const int z1 = std::min(z0 + 1, n - 1);
        const float tx = fx - static_cast<float>(x0);
        const float tz = fz - static_cast<float>(z0);
        const float a = std::lerp(grid.heights[indexAt(x0, z0)], grid.heights[indexAt(x1, z0)], tx);
        const float b = std::lerp(grid.heights[indexAt(x0, z1)], grid.heights[indexAt(x1, z1)], tx);
        return std::lerp(a, b, tz);
    };

    const size_t cellCount = static_cast<size_t>(n * n);
    std::vector<float> forceX(cellCount, 0.0f);
    std::vector<float> forceZ(cellCount, 0.0f);
    std::vector<float> flow(cellCount, 1.0f);
    std::vector<float> maskField(cellCount, 0.0f);
    std::vector<int> receiver(cellCount, -1);
    std::vector<size_t> sortedCells(cellCount, 0);
    std::iota(sortedCells.begin(), sortedCells.end(), 0);

    const auto updateFlowFields = [&]() {
        for (int z = 0; z < n; ++z)
        {
            for (int x = 0; x < n; ++x)
            {
                const float left = grid.heights[indexAt(clampCoord(x - 1), z)];
                const float right = grid.heights[indexAt(clampCoord(x + 1), z)];
                const float up = grid.heights[indexAt(x, clampCoord(z - 1))];
                const float down = grid.heights[indexAt(x, clampCoord(z + 1))];
                const size_t index = indexAt(x, z);
                forceX[index] = -(right - left) / std::max(cellSize * 2.0f, 0.0001f);
                forceZ[index] = -(down - up) / std::max(cellSize * 2.0f, 0.0001f);

                float steepestDrop = 0.0f;
                int bestReceiver = -1;
                const float currentHeight = grid.heights[index];
                for (int dz = -1; dz <= 1; ++dz)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (dx == 0 && dz == 0)
                        {
                            continue;
                        }
                        const int nx = clampCoord(x + dx);
                        const int nz = clampCoord(z + dz);
                        if (nx == x && nz == z)
                        {
                            continue;
                        }
                        const float distance = (dx != 0 && dz != 0) ? 1.41421356f : 1.0f;
                        const size_t neighborIndex = indexAt(nx, nz);
                        const float drop = (currentHeight - grid.heights[neighborIndex]) / distance;
                        if (drop > steepestDrop)
                        {
                            steepestDrop = drop;
                            bestReceiver = static_cast<int>(neighborIndex);
                        }
                    }
                }
                receiver[index] = bestReceiver;
            }
        }

        std::fill(flow.begin(), flow.end(), 1.0f);
        std::ranges::sort(sortedCells, [&](size_t a, size_t b) {
            return grid.heights[a] > grid.heights[b];
        });
        for (const size_t index : sortedCells)
        {
            const int next = receiver[index];
            if (next >= 0)
            {
                flow[static_cast<size_t>(next)] += flow[index];
            }
        }

        float maxFlow = 1.0f;
        for (float value : flow)
        {
            maxFlow = std::max(maxFlow, value);
        }
        const float logMaxFlow = std::log1p(maxFlow);
        for (float& value : flow)
        {
            value = logMaxFlow > 0.0001f ? std::log1p(value) / logMaxFlow : 0.0f;
        }
    };

    updateFlowFields();
    const float wearSlope = std::tan(std::clamp(settings.wearAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float depositSlope = std::tan(std::clamp(settings.depositAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float maxSlope = std::tan(std::clamp(settings.maxErosionAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f);
    const float strength = std::clamp(settings.erosionStrength, 0.0f, 1.0f);
    const float channeling = std::clamp(settings.channeling, 0.0f, 1.0f);
    const float friction = std::clamp(settings.friction, 0.0f, 1.0f);
    const float velocityScale = std::clamp(settings.sedimentVelocity, 0.0f, 2.0f);
    const float capacityScale = std::clamp(settings.sedimentCapacity, 0.0f, 2.0f);
    const float depositionRate = std::clamp(settings.depositionRate, 0.0f, 1.0f);
    const float featureCells = std::max(settings.featureSize, cellSize) / std::max(cellSize, 0.0001f);
    const float granularity = std::clamp(settings.erosionGranularity, 1.0f, 100.0f) / 100.0f;

    const auto runErosionPass = [&](float featureScale, float lengthScale, float strengthScale, int iterationCount, int seedOffset) {
        const float passFeatureCells = std::max(1.0f, featureCells * featureScale);
        const float passFeatureMeters = std::max(cellSize, settings.featureSize * featureScale);
        const int maxSteps = std::clamp(static_cast<int>((settings.channelLength * lengthScale) / std::max(cellSize, passFeatureMeters * 0.25f)), 1, 512);
        const int particleStride = std::max(1, static_cast<int>(std::round(std::lerp(passFeatureCells * 0.22f, 1.0f, granularity))));
        const float spawnBase = std::lerp(0.42f, 0.82f, granularity);
        const float featureStrength = std::clamp(passFeatureCells * 0.2f, 0.35f, 2.4f) * strengthScale;

        for (int iteration = 0; iteration < iterationCount; ++iteration)
        {
            if ((iteration % 4) == 0)
            {
                updateFlowFields();
            }
            for (int z = 1; z < n - 1; z += particleStride)
            {
                for (int x = 1; x < n - 1; x += particleStride)
                {
                    const float sourceFlow = flow[indexAt(x, z)];
                    const float spawnChance = std::clamp(spawnBase + sourceFlow * 0.45f, 0.0f, 0.98f);
                    if (Hash01(x, z, settings.seed + seedOffset + iteration * 977) > spawnChance)
                    {
                        continue;
                    }

                    float px = static_cast<float>(x) + Hash01(x, z, settings.seed + seedOffset + 11) - 0.5f;
                    float pz = static_cast<float>(z) + Hash01(x, z, settings.seed + seedOffset + 23) - 0.5f;
                    float vx = 0.0f;
                    float vz = 0.0f;
                    float sediment = 0.0f;
                    for (int step = 0; step < maxSteps; ++step)
                    {
                        const int ix = clampCoord(static_cast<int>(px));
                        const int iz = clampCoord(static_cast<int>(pz));
                        const size_t currentIndex = indexAt(ix, iz);
                        float fx = forceX[currentIndex];
                        float fz = forceZ[currentIndex];
                        const float slope = std::sqrt(fx * fx + fz * fz);
                        if (slope > maxSlope)
                        {
                            break;
                        }

                        const float flowWeight = std::clamp(flow[currentIndex], 0.0f, 1.0f);
                        const float flowBoost = 0.45f + flowWeight * 1.35f;
                        vx = vx * (1.0f - friction) + fx * flowBoost;
                        vz = vz * (1.0f - friction) + fz * flowBoost;
                        const float vLength = std::sqrt(vx * vx + vz * vz);
                        if (vLength < 0.00001f)
                        {
                            break;
                        }

                        const float dirX = vx / vLength;
                        const float dirZ = vz / vLength;
                        const float before = grid.heights[currentIndex];
                        const float ahead = sampleHeight(px + dirX, pz + dirZ);
                        const float behind = sampleHeight(px - dirX, pz - dirZ);
                        const float flowStrength = std::lerp(0.45f, 1.8f, flowWeight);
                        const float downhill = std::max(before - ahead, 0.0f);
                        const float slopeCapacity = slope * passFeatureMeters * 0.12f + downhill * 0.85f;
                        const float capacity = std::max(0.0f, slopeCapacity * flowStrength * capacityScale * strengthScale);
                        const float lowSlopeDeposit = slope <= depositSlope ? 1.0f : 0.35f;
                        const float depositAmount = std::max(sediment - capacity, 0.0f) * depositionRate * lowSlopeDeposit;

                        float erodeAmount = 0.0f;
                        if (slope >= wearSlope)
                        {
                            const float capacityDeficit = std::max(capacity - sediment, 0.0f);
                            const float valleyCut = std::max(before - ahead, 0.0f) * channeling * 0.14f * featureStrength * flowStrength;
                            const float smoothCut = std::max(before - ((ahead + behind) * 0.5f), 0.0f) * 0.10f * featureStrength;
                            erodeAmount = (capacityDeficit * 0.18f + valleyCut + smoothCut) * strength;
                        }
                        erodeAmount = std::min(erodeAmount, std::max(before - std::min(ahead, behind), 0.0f));

                        const float oldHeight = grid.heights[currentIndex];
                        const float nextHeight = std::clamp(oldHeight - erodeAmount + depositAmount, std::min(ahead, behind), std::max(ahead, behind) + passFeatureMeters * 0.1f);
                        grid.heights[currentIndex] = nextHeight;
                        sediment = std::max(0.0f, sediment + (oldHeight - nextHeight));
                        maskField[currentIndex] += erodeAmount + std::max(nextHeight - (oldHeight - erodeAmount), 0.0f);

                        px += dirX * velocityScale;
                        pz += dirZ * velocityScale;
                        if (px < 1.0f || pz < 1.0f || px > static_cast<float>(n - 2) || pz > static_cast<float>(n - 2))
                        {
                            break;
                        }
                    }
                }
            }
        }
    };

    const int gridIterations = std::max(1, settings.iterations / 2);
    const int particleIterations = std::max(0, settings.iterations - gridIterations);
    ApplyGridErosionPass(grid, settings, gridIterations, 0.68f, maskField);
    updateFlowFields();

    const int coarseIterations = particleIterations / 2;
    const int detailIterations = std::max(0, particleIterations - coarseIterations);
    if (coarseIterations > 0)
    {
        runErosionPass(2.75f, 1.6f, 0.45f, coarseIterations, 13007);
    }
    if (detailIterations > 0)
    {
        runErosionPass(0.75f, 0.85f, 0.65f, detailIterations, 29017);
    }

    float maxMask = 0.0f;
    for (float value : maskField)
    {
        maxMask = std::max(maxMask, value);
    }
    grid.mask.resize(cellCount, 0.0f);
    if (maxMask > 0.000001f)
    {
        for (size_t i = 0; i < cellCount; ++i)
        {
            grid.mask[i] = std::clamp(maskField[i] / maxMask, 0.0f, 1.0f);
        }
    }
}

void ApplyFluvialErosion(HeightfieldGrid& grid, const FluvialErosionSettings& settings)
{
    const int n = grid.resolution;
    if (n < 3 || grid.heights.size() < static_cast<size_t>(n * n) || settings.iterations <= 0 || settings.erosionStrength <= 0.0f)
    {
        return;
    }

    const std::array<int, FluvialErosionSettings::LevelStrengthCount> levelResolutions = {16, 32, 64, 128, 256, 512};
    FluvialErosionSettings effectiveSettings = settings;
    if (!settings.useAdvancedParameters)
    {
        const FluvialErosionSettings defaults;
        effectiveSettings.featureSize = defaults.featureSize;
        effectiveSettings.channeling = defaults.channeling;
        effectiveSettings.friction = defaults.friction;
        effectiveSettings.wearAngleDegrees = 4.0f;
        effectiveSettings.depositAngleDegrees = defaults.depositAngleDegrees;
        effectiveSettings.maxErosionAngleDegrees = 80.0f;
        effectiveSettings.erosionGranularity = defaults.erosionGranularity;
        effectiveSettings.sedimentVelocity = defaults.sedimentVelocity;
    }

    grid.mask.assign(static_cast<size_t>(n) * static_cast<size_t>(n), 0.0f);
    int previousResolution = 0;
    for (size_t level = 0; level < effectiveSettings.levelStrengths.size(); ++level)
    {
        const float levelStrength = std::clamp(effectiveSettings.levelStrengths[level], 0.0f, 2.0f);
        if (levelStrength <= 0.0001f)
        {
            continue;
        }

        const int levelResolution = std::clamp(levelResolutions[level], 16, n);
        if (levelResolution == previousResolution && levelResolution != n)
        {
            continue;
        }
        previousResolution = levelResolution;

        HeightfieldGrid base = ResampleHeightfieldGrid(grid, levelResolution);
        HeightfieldGrid eroded = base;
        FluvialErosionSettings levelSettings = effectiveSettings;
        const float detailT = static_cast<float>(level) / static_cast<float>(std::max<size_t>(1, effectiveSettings.levelStrengths.size() - 1));
        levelSettings.erosionStrength = std::clamp(effectiveSettings.erosionStrength * levelStrength, 0.0f, 2.0f);
        levelSettings.iterations = std::max(1, static_cast<int>(std::round(static_cast<float>(effectiveSettings.iterations) * std::lerp(0.55f, 0.9f, detailT))));
        levelSettings.channelLength = effectiveSettings.channelLength * std::lerp(2.8f, 0.75f, detailT);
        levelSettings.featureSize = effectiveSettings.featureSize * std::lerp(4.0f, 0.75f, detailT);
        levelSettings.channeling = std::clamp(effectiveSettings.channeling * std::lerp(1.65f, 0.85f, detailT), 0.0f, 1.0f);
        levelSettings.erosionGranularity = std::clamp(effectiveSettings.erosionGranularity * std::lerp(0.45f, 1.2f, detailT), 1.0f, 100.0f);
        levelSettings.seed = effectiveSettings.seed + static_cast<int>(level) * 1009;
        bool usedGpu = false;
        if (effectiveSettings.backend == FluvialBackend::GpuCompute && g_fluvialGpuEvaluator != nullptr)
        {
            std::string gpuError;
            usedGpu = g_fluvialGpuEvaluator(eroded, levelSettings, &gpuError);
        }
        if (!usedGpu)
        {
            ApplyFluvialErosionSingleLevel(eroded, levelSettings);
        }
        AddResampledHeightDelta(grid, base, eroded, 1.0f);
    }
    NormalizeHeightfieldMask(grid);
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

    float minHeight = FLT_MAX;
    float maxHeight = -FLT_MAX;
    for (int z = 0; z < meshResolution; ++z)
    {
        const float v = meshResolution > 1 ? static_cast<float>(z) / static_cast<float>(meshResolution - 1) : 0.0f;
        for (int x = 0; x < meshResolution; ++x)
        {
            const float u = meshResolution > 1 ? static_cast<float>(x) / static_cast<float>(meshResolution - 1) : 0.0f;
            const float height = SampleHeightfieldValue(grid.heights, gridResolution, u, v);
            minHeight = std::min(minHeight, height);
            maxHeight = std::max(maxHeight, height);
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

    const float heightRange = std::max(0.0f, maxHeight - minHeight);
    const float baseDepth = std::max({grid.terrainSizeMeters * 0.08f, heightRange * 0.15f, 32.0f});
    const float baseY = minHeight - baseDepth;
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

MeshData BuildMeshFromHeightPipeline(const SdfPipeline& pipeline, int resolution, std::string* message)
{
    HeightfieldGrid grid = BuildHeightfieldFromHeightmap(pipeline.heightmap, resolution, message);
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
    }
    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    return BuildMeshFromHeightfield(grid, resolution);
}
} // namespace

MeshData NodeGraph::BuildMeshFromHeightPipelineCached(const SdfPipeline& pipeline, int resolution, std::string* message)
{
    if (pipeline.heightmapNodeId == 0)
    {
        return BuildMeshFromHeightPipeline(pipeline, resolution, message);
    }

    const int simulationResolution = std::clamp(pipeline.heightmap.simulationResolution, 2, 2048);
    uint64_t inputHash = 0;
    const uint64_t heightmapHash = HashHeightmapSettings(pipeline.heightmap, simulationResolution);
    HeightfieldNodeCache& heightmapCache = heightfieldCache_[pipeline.heightmapNodeId];
    if (!heightmapCache.valid ||
        heightmapCache.resolution != simulationResolution ||
        heightmapCache.inputHash != inputHash ||
        heightmapCache.parameterHash != heightmapHash)
    {
        std::string heightmapMessage;
        heightmapCache.grid = BuildHeightfieldFromHeightmap(pipeline.heightmap, simulationResolution, &heightmapMessage);
        heightmapCache.message = heightmapMessage;
        heightmapCache.valid = true;
        heightmapCache.resolution = simulationResolution;
        heightmapCache.inputHash = inputHash;
        heightmapCache.parameterHash = heightmapHash;
        heightmapCache.outputHash = heightmapHash;
    }

    HeightfieldGrid grid = heightmapCache.grid;
    inputHash = heightmapCache.outputHash;
    if (message != nullptr)
    {
        *message = heightmapCache.message;
    }
    if (grid.resolution <= 0)
    {
        return {};
    }

    for (const SdfPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.kind != SdfPipeline::HeightfieldOperation::Kind::FluvialErosion || operation.nodeId == 0)
        {
            if (operation.kind == SdfPipeline::HeightfieldOperation::Kind::FluvialErosion)
            {
                ApplyFluvialErosion(grid, operation.fluvialErosion);
            }
            continue;
        }

        const uint64_t parameterHash = HashFluvialSettings(operation.fluvialErosion, simulationResolution);
        HeightfieldNodeCache& operationCache = heightfieldCache_[operation.nodeId];
        if (!operationCache.valid ||
            operationCache.resolution != simulationResolution ||
            operationCache.inputHash != inputHash ||
            operationCache.parameterHash != parameterHash)
        {
            HeightfieldGrid operationGrid = grid;
            ApplyFluvialErosion(operationGrid, operation.fluvialErosion);
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
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::FluvialErosion:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Fluvial Mask");
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
    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == node->id &&
        evaluation_.previewPinId == pinId &&
        evaluation_.previewStage == stage &&
        evaluation_.previewShowsMask == showsMask)
    {
        return false;
    }

    const bool pipelineChanged = evaluation_.previewNodeId != node->id || evaluation_.previewStage != stage;
    evaluation_.previewNodeId = node->id;
    evaluation_.previewPinId = pinId;
    evaluation_.previewShowsMask = showsMask;
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
        evaluation_.previewMesh = BuildMeshFromHeightPipelineCached(previewPipeline, previewMeshResolution, &evaluation_.previewMessage);
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
    case NodeKind::HeightmapLoad:
        return PreviewStage::Primitive;
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
