#include "node_graph.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
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

MultiScaleErosionGpuEvaluator g_mseGpuEvaluator = nullptr;
MaskNoiseGpuEvaluator g_maskNoiseGpuEvaluator = nullptr;
std::atomic<GraphId> g_currentlyEvaluatingNodeId{0};

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

uint64_t HashHeightmapBlurSettings(const HeightmapBlurSettings& settings, int resolution)
{
    uint64_t hash = 2166136261ull;
    HashCombine(hash, HashFloat(settings.radius));
    HashCombine(hash, HashFloat(settings.strength));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashErosionNoiseSettings(const ErosionNoiseSettings& settings, int resolution)
{
    uint64_t hash = 14695981039346656037ull;
    HashCombine(hash, HashFloat(settings.frequency));
    HashCombine(hash, static_cast<uint64_t>(settings.octaves));
    HashCombine(hash, HashFloat(settings.erosionStrength));
    HashCombine(hash, HashFloat(settings.directionInfluence));
    HashCombine(hash, HashFloat(settings.valleyLow));
    HashCombine(hash, HashFloat(settings.valleyHigh));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMaskNoiseSettings(const MaskNoiseSettings& settings)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, static_cast<uint64_t>(settings.octaves));
    HashCombine(hash, HashFloat(settings.frequency));
    HashCombine(hash, HashFloat(settings.lacunarity));
    HashCombine(hash, HashFloat(settings.persistence));
    HashCombine(hash, static_cast<uint64_t>(settings.simulationResolution));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    return hash;
}

uint64_t HashMaskBlendSettings(const MaskBlendSettings& settings)
{
    uint64_t hash = 16777619ull;
    HashCombine(hash, static_cast<uint64_t>(settings.mode));
    HashCombine(hash, HashFloat(settings.intensity));
    return hash;
}

uint64_t HashMaskFluvialSettings(const MaskFluvialSettings& settings, int resolution)
{
    uint64_t hash = 8589869056ull;
    HashCombine(hash, static_cast<uint64_t>(settings.algorithm));
    HashCombine(hash, static_cast<uint64_t>(settings.outputCurve));
    HashCombine(hash, HashFloat(settings.accumulationThreshold));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, HashFloat(settings.softness));
    HashCombine(hash, HashFloat(settings.power));
    HashCombine(hash, static_cast<uint64_t>(settings.pitFillIterations));
    HashCombine(hash, HashFloat(settings.mfdExponent));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMultiScaleErosionSettings(const MultiScaleErosionSettings& settings, int resolution)
{
    uint64_t hash = 11400714819323198485ull;
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(settings.enableStreamPower ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.enableThermal ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.enableDeposition ? 1 : 0));
    HashCombine(hash, HashFloat(settings.speStrength));
    HashCombine(hash, HashFloat(settings.streamExponent));
    HashCombine(hash, HashFloat(settings.slopeExponent));
    HashCombine(hash, HashFloat(settings.maxStreamPower));
    HashCombine(hash, HashFloat(settings.flowExponent));
    HashCombine(hash, HashFloat(settings.speTimeStep));
    HashCombine(hash, HashFloat(settings.thermalAngleDegrees));
    HashCombine(hash, HashFloat(settings.thermalStrength));
    HashCombine(hash, static_cast<uint64_t>(settings.thermalNoisifyAngle ? 1 : 0));
    HashCombine(hash, HashFloat(settings.thermalNoiseMin));
    HashCombine(hash, HashFloat(settings.thermalNoiseMax));
    HashCombine(hash, HashFloat(settings.thermalNoiseWavelength));
    HashCombine(hash, HashFloat(settings.depositionStrength));
    HashCombine(hash, HashFloat(settings.rain));
    HashCombine(hash, static_cast<uint64_t>(settings.useMultigrid ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
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

// Run `fn(z)` for each row z in [0, n). Each iteration must be independent
// (writing only to its own row). Used by the heightfield ops that follow.
template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
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

    // Each row writes only to its own indices in temp / blurred (separable
    // Gaussian: horizontal pass then vertical pass), so both passes are safely
    // parallelized across z.
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const std::vector<float>& source = grid.heights;
        ParallelForRows(n, [&](int z) {
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
        });

        ParallelForRows(n, [&](int z) {
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
        });

        for (size_t i = 0; i < grid.heights.size(); ++i)
        {
            grid.heights[i] = std::lerp(source[i], blurred[i], strength);
        }
    }
}

// Procedural "Erosion Noise" — port of clayjohn's eroded terrain noise (Shadertoy MtGcWh).
// Not a simulation: it stacks a directional Gavoronoise-style FBM whose direction follows
// the input heightfield's slope (90deg-rotated gradient) and adds the result to the input.
namespace erosion_noise
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kKx = 0.3183099f;
constexpr float kKy = 0.3678794f;

inline float Fract(float v)
{
    return v - std::floor(v);
}

inline void Hash2(float px, float py, float seedOffset, float& outX, float& outY)
{
    float x = px * kKx + kKy + seedOffset;
    float y = py * kKy + kKx + seedOffset;
    const float s = Fract(x * y * (x + y));
    outX = -1.0f + 2.0f * Fract(16.0f * kKx * s);
    outY = -1.0f + 2.0f * Fract(16.0f * kKy * s);
}

// Returns (value, dValue/dpx, dValue/dpy) for a single layer of directional noise.
// The "derivative" here matches the shadertoy code exactly — it favours direction
// rather than being a strict analytic gradient.
inline void ErosionLayer(float px, float py, float dirX, float dirY, float seedOffset,
                         float& outVal, float& outDx, float& outDy)
{
    const float ipx = std::floor(px);
    const float ipy = std::floor(py);
    const float fpx = px - ipx;
    const float fpy = py - ipy;
    constexpr float f = 2.0f * kPi;

    float vaX = 0.0f;
    float vaY = 0.0f;
    float vaZ = 0.0f;
    float wt = 0.0f;
    for (int j = -2; j <= 1; ++j)
    {
        for (int i = -2; i <= 1; ++i)
        {
            float hx, hy;
            Hash2(ipx - static_cast<float>(i), ipy - static_cast<float>(j), seedOffset, hx, hy);
            hx *= 0.5f;
            hy *= 0.5f;
            const float ppx = fpx + static_cast<float>(i) - hx;
            const float ppy = fpy + static_cast<float>(j) - hy;
            const float d = ppx * ppx + ppy * ppy;
            const float w = std::exp(-d * 2.0f);
            wt += w;
            const float mag = ppx * dirX + ppy * dirY;
            const float c = std::cos(mag * f);
            const float s = -std::sin(mag * f);
            vaX += c * w;
            vaY += s * (ppx + dirX) * w;
            vaZ += s * (ppy + dirY) * w;
        }
    }
    const float invWt = wt > 0.0f ? 1.0f / wt : 0.0f;
    outVal = vaX * invWt;
    outDx = vaY * invWt;
    outDy = vaZ * invWt;
}

inline float Smoothstep(float lo, float hi, float x)
{
    if (hi <= lo) { return x < lo ? 0.0f : 1.0f; }
    const float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace erosion_noise

void ApplyErosionNoise(HeightfieldGrid& grid, const ErosionNoiseSettings& settings)
{
    using namespace erosion_noise;

    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount)
    {
        return;
    }
    const int octaves = std::clamp(settings.octaves, 0, 8);
    const float erosionStrength = std::max(settings.erosionStrength, 0.0f);
    if (octaves <= 0 || erosionStrength <= 0.0f)
    {
        return;
    }
    const float frequency = std::max(settings.frequency, 0.0f);
    const float directionInfluence = std::max(settings.directionInfluence, 0.0f);
    const float valleyLow = std::clamp(settings.valleyLow, 0.0f, 1.0f);
    const float valleyHigh = std::clamp(settings.valleyHigh, 0.0f, 1.0f);
    const float seedOffset = static_cast<float>(settings.seed) * 12.9898f;

    float hMin = std::numeric_limits<float>::infinity();
    float hMax = -std::numeric_limits<float>::infinity();
    for (float v : grid.heights)
    {
        if (v < hMin) { hMin = v; }
        if (v > hMax) { hMax = v; }
    }
    const float heightRange = std::max(hMax - hMin, 1e-4f);
    const float invRange = 1.0f / heightRange;
    const float invDenom = 1.0f / static_cast<float>(n - 1);
    // Match the shader's derivative scale closely enough for existing heightfields:
    // normalize height to the terrain range and measure the slope over the full domain.
    const float gradScale = invRange * static_cast<float>(n - 1);

    const auto sampleHeight = [&](int x, int z) {
        const int xc = std::clamp(x, 0, n - 1);
        const int zc = std::clamp(z, 0, n - 1);
        return grid.heights[static_cast<size_t>(zc) * static_cast<size_t>(n) + static_cast<size_t>(xc)];
    };

    std::vector<float> output(cellCount, 0.0f);
    for (int z = 0; z < n; ++z)
    {
        for (int x = 0; x < n; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            const float v = static_cast<float>(z) * invDenom;
            const float px = u * frequency;
            const float py = v * frequency;

            const float dhdx = (sampleHeight(x + 1, z) - sampleHeight(x - 1, z)) * 0.5f * gradScale;
            const float dhdz = (sampleHeight(x, z + 1) - sampleHeight(x, z - 1)) * 0.5f * gradScale;
            const float dirX = dhdz * directionInfluence;
            const float dirY = -dhdx * directionInfluence;

            const float h0 = sampleHeight(x, z);
            const float hNorm = std::clamp((h0 - hMin) * invRange, 0.0f, 1.0f);
            float a = 0.7f * Smoothstep(valleyLow, valleyHigh, hNorm);

            float accVal = 0.0f;
            float accDx = 0.0f;
            float accDy = 0.0f;
            float f = 1.0f;
            for (int i = 0; i < octaves; ++i)
            {
                const float octaveDirX = dirX + accDy;       // h.z * 1
                const float octaveDirY = dirY - accDx;       // h.y * -1
                float val, dx, dy;
                ErosionLayer(px * f, py * f, octaveDirX, octaveDirY, seedOffset, val, dx, dy);
                accVal += val * a;
                accDx += dx * a * f;
                accDy += dy * a * f;
                a *= 0.4f;
                f *= 2.0f;
            }

            output[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)] =
                h0 + accVal * erosionStrength * heightRange;
        }
    }
    grid.heights = std::move(output);
}

// 2D Perlin / fBM noise used by the Mask Noise node. Unlike the directional
// noise in the erosion-noise namespace above (which warps gradients to follow
// existing terrain), this is a plain isotropic Perlin field driving fBM, so we
// keep the implementation self-contained: gradient hashes derived from a
// triple-mix of (x, y, seed), 5th-order fade, lerp, then summed across octaves
// with Lacunarity / Persistence and remapped to [0, 1].
namespace mask_noise
{
inline uint32_t Hash3(int32_t x, int32_t y, int32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x27d4eb2du;
    h ^= static_cast<uint32_t>(y) * 0x165667b1u;
    h ^= static_cast<uint32_t>(seed) * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

inline std::array<float, 2> Gradient2(int32_t x, int32_t y, int32_t seed)
{
    const uint32_t h = Hash3(x, y, seed);
    constexpr float twoPi = 6.28318530717958647692f;
    const float angle = (static_cast<float>(h & 0xFFFFu) / 65535.0f) * twoPi;
    return {std::cos(angle), std::sin(angle)};
}

inline float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float Perlin2D(float x, float y, int32_t seed)
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int32_t x0 = static_cast<int32_t>(fx);
    const int32_t y0 = static_cast<int32_t>(fy);
    const int32_t x1 = x0 + 1;
    const int32_t y1 = y0 + 1;
    const float dx = x - fx;
    const float dy = y - fy;

    const auto g00 = Gradient2(x0, y0, seed);
    const auto g10 = Gradient2(x1, y0, seed);
    const auto g01 = Gradient2(x0, y1, seed);
    const auto g11 = Gradient2(x1, y1, seed);

    const float v00 = g00[0] * dx + g00[1] * dy;
    const float v10 = g10[0] * (dx - 1.0f) + g10[1] * dy;
    const float v01 = g01[0] * dx + g01[1] * (dy - 1.0f);
    const float v11 = g11[0] * (dx - 1.0f) + g11[1] * (dy - 1.0f);

    const float u = Fade(dx);
    const float v = Fade(dy);
    return std::lerp(std::lerp(v00, v10, u), std::lerp(v01, v11, u), v);
}

inline float Fbm2D(float x, float y, int octaves, float lacunarity, float persistence, int32_t seed)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxAmplitude = 0.0f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i)
    {
        total += Perlin2D(x * freq, y * freq, seed + i * 1013) * amplitude;
        maxAmplitude += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return maxAmplitude > 0.0f ? total / maxAmplitude : 0.0f;
}
} // namespace mask_noise

MaskGrid GenerateMaskNoise(const MaskNoiseSettings& settings)
{
    MaskGrid grid;
    grid.resolution = std::clamp(settings.simulationResolution, 2, 2048);
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.values.assign(cellCount, 0.0f);

    // GPU compute path. Falls back to the CPU branch below if the evaluator
    // hasn't been registered (no D3D12 device) or returns failure.
    if (settings.backend == MaskNoiseBackend::GpuCompute && g_maskNoiseGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_maskNoiseGpuEvaluator(grid, settings, &ignoredError))
        {
            return grid;
        }
        // Fall through on GPU failure; clear any partial writes first.
        grid.values.assign(cellCount, 0.0f);
    }

    const int octaves = std::clamp(settings.octaves, 1, 12);
    const float lacunarity = std::max(settings.lacunarity, 0.0f);
    const float persistence = std::clamp(settings.persistence, 0.0f, 1.0f);
    const float frequency = std::max(settings.frequency, 0.0f);
    const int32_t seed = settings.seed;
    const float invDenom = grid.resolution > 1 ? 1.0f / static_cast<float>(grid.resolution - 1) : 0.0f;

    const int n = grid.resolution;
    ParallelForRows(n, [&](int z) {
        const float v = static_cast<float>(z) * invDenom;
        for (int x = 0; x < n; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            // Perlin / fBM output is roughly in [-1, 1] after normalisation, so
            // remap to [0, 1] for use as a Mask channel.
            const float ns = mask_noise::Fbm2D(u * frequency, v * frequency, octaves, lacunarity, persistence, seed);
            grid.values[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)] =
                std::clamp(ns * 0.5f + 0.5f, 0.0f, 1.0f);
        }
    });
    return grid;
}

float SampleMaskBilinear(const MaskGrid& grid, float u, float v)
{
    if (grid.resolution <= 0)
    {
        return 0.0f;
    }
    const float fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(grid.resolution - 1);
    const float fy = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(grid.resolution - 1);
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, grid.resolution - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, grid.resolution - 1);
    const int x1 = std::min(x0 + 1, grid.resolution - 1);
    const int y1 = std::min(y0 + 1, grid.resolution - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float a = grid.values[static_cast<size_t>(y0) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x0)];
    const float b = grid.values[static_cast<size_t>(y0) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x1)];
    const float c = grid.values[static_cast<size_t>(y1) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x0)];
    const float d = grid.values[static_cast<size_t>(y1) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x1)];
    return std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), ty);
}

MaskGrid ResampleMaskGrid(const MaskGrid& source, int targetResolution)
{
    if (source.resolution == targetResolution || source.resolution <= 0)
    {
        return source;
    }
    MaskGrid resampled;
    resampled.resolution = targetResolution;
    resampled.values.assign(static_cast<size_t>(targetResolution) * static_cast<size_t>(targetResolution), 0.0f);
    const float invDenom = targetResolution > 1 ? 1.0f / static_cast<float>(targetResolution - 1) : 0.0f;
    for (int z = 0; z < targetResolution; ++z)
    {
        const float v = static_cast<float>(z) * invDenom;
        for (int x = 0; x < targetResolution; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            resampled.values[static_cast<size_t>(z) * static_cast<size_t>(targetResolution) + static_cast<size_t>(x)] =
                SampleMaskBilinear(source, u, v);
        }
    }
    return resampled;
}

MaskGrid BlendMaskGrids(const MaskGrid& a, const MaskGrid& b, MaskBlendMode mode, float intensity)
{
    if (a.resolution <= 0 && b.resolution <= 0) { return {}; }
    if (a.resolution <= 0) { return b; }
    if (b.resolution <= 0) { return a; }

    const int n = std::max(a.resolution, b.resolution);
    const MaskGrid& gridA = a.resolution == n ? a : ResampleMaskGrid(a, n);
    const MaskGrid& gridB = b.resolution == n ? b : ResampleMaskGrid(b, n);

    MaskGrid result;
    result.resolution = n;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    result.values.resize(cellCount);
    const float t = std::clamp(intensity, 0.0f, 1.0f);
    for (size_t i = 0; i < cellCount; ++i)
    {
        const float va = gridA.values[i];
        const float vb = gridB.values[i];
        float blended = va;
        switch (mode)
        {
        case MaskBlendMode::Add:
            blended = va + vb;
            break;
        case MaskBlendMode::Multiply:
            blended = va * vb;
            break;
        case MaskBlendMode::Min:
            blended = std::min(va, vb);
            break;
        case MaskBlendMode::Max:
            blended = std::max(va, vb);
            break;
        }
        result.values[i] = std::clamp(std::lerp(va, blended, t), 0.0f, 1.0f);
    }
    return result;
}

// Multi-Scale Erosion — CPU port of Schott et al. (SIGGRAPH 2024) shaders:
//   data/shaders/erosion.glsl      (Stream Power Erosion)
//   data/shaders/thermal.glsl      (Thermal / talus)
//   data/shaders/deposition.glsl   (Sediment deposition)
// Source repo: https://github.com/H-Schott/MultiScaleErosion (MIT)
//
// Each pass uses ping-pong buffers and reads upstream contributions through
// the previous iteration's stream/sediment field. SPE/Deposition use the same
// D8 weighted-flow direction (`flow_p` exponent on slope). Thermal is a 3x3
// stencil with wraparound boundary, matching the shader.
namespace mse
{
// Coarsest resolution in the multi-grid pyramid. Anything coarser than this
// has too few cells to be meaningful; anything finer is reached by repeated
// x2 bilinear upsampling.
constexpr int kCoarsestPyramidLevel = 64;

// Reference cell size for resolution-invariant scaling of per-iter strengths.
// The shader's `eps * cellArea` (and `rain * cellArea`) factors make the per-
// iter effect scale with cellSize², which causes the visual outcome to drift
// noticeably as the user changes Simulation Resolution. We anchor those
// `cellArea` factors to a fixed reference so the same parameter set produces
// roughly the same look across resolutions. 4 m matches the typical 512 grid
// over a 2048 m terrain — existing tunings stay accurate there, and other
// resolutions get the compensation.
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

// 2D value-noise approximation used to perturb the thermal threshold angle.
// The reference shader samples 3D simplex noise on (x, y, height); we keep a
// cheaper 2D variant since the spatial variation is what matters visually.
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

// Each row of the (x, z) grid is independent in the SPE / Thermal / Deposition
// passes (outputs are written only to the row's own indices), so we parallelize
// across rows using the outer-namespace `ParallelForRows` helper. par (not
// par_unseq) — the bodies have heavy branching that wouldn't vectorize cleanly.

struct WeightedFlow
{
    std::array<float, 8> w{};
};

// Compute the weighted D8 outflow distribution for cell p. `pow(slope, flow_p)`
// over downhill neighbours, normalized; uphill neighbours get 0.
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

// Steepest descent direction (i, j) at p, plus its slope magnitude.
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

            // Incoming flow from neighbours' weighted outflow towards p.
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

            int dx = 0, dz = 0;
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
    // Resolution-invariant: anchor matter to refCellArea instead of the
    // current cellArea (see kRefCellSize comment above).
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
                    // Wraparound (matches thermal.glsl).
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
    // Match deposition.glsl, but anchor cellArea to refCellArea for resolution
    // invariance (see kRefCellSize comment near the top of the namespace).
    // Otherwise rain * cellArea (the per-cell rain volume contribution) would
    // scale with cellSize², shifting deposition behavior across resolutions.
    constexpr float cellArea = kRefCellArea * 0.00001f;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            const float h = heightsIn[static_cast<size_t>(id)];
            float sed = sedIn[static_cast<size_t>(id)];

            int dx = 0, dz = 0;
            float steepest = 0.0f;
            GetSteepestDescent(heightsIn, n, cellSize, x, z, dx, dz, steepest);
            // Match deposition.glsl `if (!CheckPit(p)) sed = 0;`: only local minima
            // (no lower neighbour) retain sediment between iterations. Non-pit cells
            // flush their sediment downstream and start fresh from incoming + pickup.
            const bool isPit = (dx == 0 && dz == 0);
            if (!isPit) { sed = 0.0f; }

            // Add rain and incoming weighted flow / sediment.
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
// Bilinear resample of a height array between arbitrary source/target
// resolutions. Reuses SampleHeightfieldValue's bilinear sampler so the same
// formula is used as elsewhere in the project.
inline std::vector<float> ResampleHeightsBilinear(const std::vector<float>& source, int sourceN, int targetN)
{
    std::vector<float> result(static_cast<size_t>(targetN) * static_cast<size_t>(targetN), 0.0f);
    for (int z = 0; z < targetN; ++z)
    {
        const float v = targetN > 1 ? static_cast<float>(z) / static_cast<float>(targetN - 1) : 0.0f;
        for (int x = 0; x < targetN; ++x)
        {
            const float u = targetN > 1 ? static_cast<float>(x) / static_cast<float>(targetN - 1) : 0.0f;
            result[static_cast<size_t>(z * targetN + x)] = SampleHeightfieldValue(source, sourceN, u, v);
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

    // GPU compute path. Falls back to CPU if the evaluator hasn't been
    // registered (no D3D12 device) or returns failure (e.g. shader compile
    // error, runtime issue). The evaluator is responsible for filling
    // grid.heights / flows / deposits and for normalizing the auxiliary
    // fields just like the CPU branch below.
    if (settings.backend == MultiScaleErosionBackend::GpuCompute && g_mseGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_mseGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Fall through to CPU path on GPU failure.
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
    grid.age.assign(cellCount, 0.0f);
    NormalizeHeightfieldFields(grid);
}
} // namespace mse

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

    // Build pyramid: start at kCoarsestPyramidLevel and double up to the target
    // resolution. The coarse pass establishes the drainage network quickly
    // (stream propagation cost is O(path_length / cellSize), which is small at
    // coarse cellSize). Each subsequent level only refines the existing
    // structure with bilinear upsample + more iterations.
    std::vector<int> levels;
    for (int r = mse::kCoarsestPyramidLevel; r < targetN; r *= 2)
    {
        levels.push_back(r);
    }
    levels.push_back(targetN);

    if (levels.size() <= 1)
    {
        // Target is already <= the coarsest level, no benefit from pyramid.
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

    // Final level's output is already at target resolution.
    grid.heights = std::move(working.heights);
    grid.flows = std::move(working.flows);
    grid.deposits = std::move(working.deposits);
    grid.mask.assign(targetCellCount, 0.0f);
    grid.age.assign(targetCellCount, 0.0f);
}

// Mask Fluvial: D8 / MFD flow accumulation -> river-stream mask.
// Heights pass through. Fills grid.mask with a normalized 0..1 mask
// where the upstream-cell count exceeds accumulationThreshold.
void ApplyMaskFluvial(HeightfieldGrid& grid, const MaskFluvialSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount)
    {
        return;
    }

    // 1. Iterative pit fill (Jacobi, double-buffered). Any interior cell
    // whose 8 neighbours are all >= itself gets raised to (min_neighbour +
    // epsilon). Boundary cells act as outlets. We use Jacobi (read from
    // `filled`, write to `next`, swap) instead of Gauss-Seidel so the
    // sweep parallelises cleanly across rows. Jacobi propagates fills one
    // cell per iteration just like GS, so iteration count is the practical
    // tunable for "how deep a pit can be filled".
    std::vector<float> filled = grid.heights;
    std::vector<float> next = filled;
    const int pitIters = std::clamp(settings.pitFillIterations, 0, 64);
    constexpr float kPitEpsilon = 1e-4f;
    for (int iter = 0; iter < pitIters; ++iter)
    {
        ParallelForRows(n, [&](int z) {
            if (z == 0 || z >= n - 1)
            {
                return;
            }
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            const size_t rowAbove = static_cast<size_t>(z - 1) * static_cast<size_t>(n);
            const size_t rowBelow = static_cast<size_t>(z + 1) * static_cast<size_t>(n);
            for (int x = 1; x < n - 1; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float h = filled[idx];
                const float n00 = filled[rowAbove + static_cast<size_t>(x - 1)];
                const float n01 = filled[rowAbove + static_cast<size_t>(x)];
                const float n02 = filled[rowAbove + static_cast<size_t>(x + 1)];
                const float n10 = filled[rowBase + static_cast<size_t>(x - 1)];
                const float n12 = filled[rowBase + static_cast<size_t>(x + 1)];
                const float n20 = filled[rowBelow + static_cast<size_t>(x - 1)];
                const float n21 = filled[rowBelow + static_cast<size_t>(x)];
                const float n22 = filled[rowBelow + static_cast<size_t>(x + 1)];
                const float minNeighbor = std::min({n00, n01, n02, n10, n12, n20, n21, n22});
                next[idx] = (h <= minNeighbor) ? (minNeighbor + kPitEpsilon) : h;
            }
        });
        std::swap(filled, next);
    }

    // 2. Sort cell indices by height descending. Accumulation must process
    // each cell after every higher cell upstream of it has already pushed
    // its flow downhill, so a topological sort by elevation works.
    std::vector<int> indices(cellCount);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(std::execution::par, indices.begin(), indices.end(),
              [&filled](int a, int b) { return filled[a] > filled[b]; });

    // 3. Flow accumulation. Each cell starts with weight 1 and pushes its
    // accumulator to one (D8) or several (MFD) downhill neighbours.
    std::vector<float> accum(cellCount, 1.0f);
    static const int kDx[8]    = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int kDz[8]    = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const float kDist[8] = {
        1.41421356f, 1.0f, 1.41421356f,
        1.0f,              1.0f,
        1.41421356f, 1.0f, 1.41421356f,
    };

    if (settings.algorithm == FlowAccumulationAlgorithm::D8)
    {
        for (int idx : indices)
        {
            const int x = idx % n;
            const int z = idx / n;
            const float h = filled[static_cast<size_t>(idx)];
            int bestK = -1;
            float bestSlope = 0.0f;
            for (int k = 0; k < 8; ++k)
            {
                const int nx = x + kDx[k];
                const int nz = z + kDz[k];
                if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                const float nh = filled[static_cast<size_t>(nz) * n + nx];
                const float slope = (h - nh) / kDist[k];
                if (slope > bestSlope) { bestSlope = slope; bestK = k; }
            }
            if (bestK >= 0)
            {
                const int nx = x + kDx[bestK];
                const int nz = z + kDz[bestK];
                accum[static_cast<size_t>(nz) * n + nx] += accum[static_cast<size_t>(idx)];
            }
        }
    }
    else
    {
        const float p = std::clamp(settings.mfdExponent, 0.1f, 16.0f);
        for (int idx : indices)
        {
            const int x = idx % n;
            const int z = idx / n;
            const float h = filled[static_cast<size_t>(idx)];
            float weights[8] = {0};
            float weightSum = 0.0f;
            for (int k = 0; k < 8; ++k)
            {
                const int nx = x + kDx[k];
                const int nz = z + kDz[k];
                if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                const float nh = filled[static_cast<size_t>(nz) * n + nx];
                const float slope = (h - nh) / kDist[k];
                if (slope > 0.0f)
                {
                    weights[k] = std::pow(slope, p);
                    weightSum += weights[k];
                }
            }
            if (weightSum > 0.0f)
            {
                const float inv = 1.0f / weightSum;
                const float a = accum[static_cast<size_t>(idx)];
                for (int k = 0; k < 8; ++k)
                {
                    if (weights[k] > 0.0f)
                    {
                        const int nx = x + kDx[k];
                        const int nz = z + kDz[k];
                        accum[static_cast<size_t>(nz) * n + nx] += a * weights[k] * inv;
                    }
                }
            }
        }
    }

    // 4. Convert accumulation to mask. Threshold is interpreted as a
    // fraction of grid cells so it stays meaningful across resolutions.
    // The per-cell math is heavy (std::log / std::pow), so the row sweep
    // here parallelises cleanly and is a big win at higher resolutions.
    const float thresholdCells = std::clamp(settings.accumulationThreshold, 0.0f, 1.0f) * static_cast<float>(cellCount);
    grid.mask.assign(cellCount, 0.0f);

    if (settings.outputCurve == MaskFluvialOutputCurve::Threshold)
    {
        const float thresholdLow = std::max(1.0f, thresholdCells);
        const float softness = std::clamp(settings.softness, 0.001f, 4.0f);
        const float thresholdHigh = thresholdLow * (1.0f + 4.0f * softness);
        const float power = std::clamp(settings.power, 0.1f, 8.0f);
        const float invRange = 1.0f / std::max(thresholdHigh - thresholdLow, 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                float t = (accum[idx] - thresholdLow) * invRange;
                t = std::clamp(t, 0.0f, 1.0f);
                const float smooth = t * t * (3.0f - 2.0f * t);
                grid.mask[idx] = std::pow(smooth, power);
            }
        });
        return;
    }

    // Log / Linear: parallel max reduction (each row computes its local
    // adjusted-max, then a quick serial fold across rows), followed by a
    // parallel mask-conversion sweep. We don't materialise the adjusted
    // vector — recomputing accum[i] - threshold inline is cheaper than the
    // extra allocation + pass.
    std::vector<float> rowMax(static_cast<size_t>(n), 0.0f);
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        float local = 0.0f;
        for (int x = 0; x < n; ++x)
        {
            const float v = accum[rowBase + static_cast<size_t>(x)] - thresholdCells;
            if (v > local) local = v;
        }
        rowMax[static_cast<size_t>(z)] = local;
    });
    float maxAdjusted = 0.0f;
    for (float v : rowMax) if (v > maxAdjusted) maxAdjusted = v;

    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    if (settings.outputCurve == MaskFluvialOutputCurve::Log)
    {
        const float invLogMax = 1.0f / std::max(std::log(1.0f + maxAdjusted), 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float a = std::max(0.0f, accum[idx] - thresholdCells);
                const float t = std::log(1.0f + a) * invLogMax;
                grid.mask[idx] = std::pow(std::clamp(t, 0.0f, 1.0f), gamma);
            }
        });
    }
    else  // Linear
    {
        const float invMax = 1.0f / std::max(maxAdjusted, 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float a = std::max(0.0f, accum[idx] - thresholdCells);
                const float t = a * invMax;
                grid.mask[idx] = std::pow(std::clamp(t, 0.0f, 1.0f), gamma);
            }
        });
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
    meshResolution = std::clamp(meshResolution, 2, 2048);

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

template <typename Settings>
int EffectiveMeshResolution(const Settings& settings, int maxResolution = 512)
{
    const int divisor = 1 << std::clamp(settings.lod, 0, 4);
    return std::clamp(settings.resolution / divisor, 16, maxResolution);
}

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

// Lightweight mesh builder for Mask Noise / Mask Blend previews. The
// heightfield is flat (y = 0), so we skip the wall, bottom, normal-accumulation
// and edge-dedup work that BuildMeshFromHeightfield needs for terrain. All
// surface normals are (0, 1, 0) and the grid topology is regular, so triangles
// and edges can be written by index in parallel rows without hash sets.
MeshData BuildFlatMaskMesh(const HeightfieldGrid& grid, int meshResolution)
{
    MeshData mesh;
    if (grid.resolution < 2 || grid.mask.empty())
    {
        return mesh;
    }
    meshResolution = std::clamp(meshResolution, 2, 2048);

    const int M = meshResolution;
    const int gridResolution = grid.resolution;
    const float halfSize = grid.terrainSizeMeters * 0.5f;
    const size_t vertexCount = static_cast<size_t>(M) * static_cast<size_t>(M);
    const size_t triangleCount = static_cast<size_t>(M - 1) * static_cast<size_t>(M - 1) * 2u;
    const size_t horizEdges = static_cast<size_t>(M) * static_cast<size_t>(M - 1);
    const size_t vertEdges = static_cast<size_t>(M - 1) * static_cast<size_t>(M);
    const size_t diagEdges = static_cast<size_t>(M - 1) * static_cast<size_t>(M - 1);

    mesh.vertices.resize(vertexCount);
    mesh.triangles.resize(triangleCount);
    mesh.edges.resize(horizEdges + vertEdges + diagEdges);

    const float invDenom = 1.0f / static_cast<float>(M - 1);

    ParallelForRows(M, [&](int z) {
        const float v = static_cast<float>(z) * invDenom;
        const float zPos = std::lerp(halfSize, -halfSize, v);
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M);
        for (int x = 0; x < M; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            mesh.vertices[rowStart + static_cast<size_t>(x)] = MeshVertex{
                std::lerp(-halfSize, halfSize, u),
                0.0f,
                zPos,
                0.0f,
                1.0f,
                0.0f,
                SampleHeightfieldValue(grid.mask, gridResolution, u, v),
            };
        }
    });

    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M - 1) * 2u;
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        const uint32_t nextRowOffset = rowOffset + static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            const uint32_t b = a + 1u;
            const uint32_t c = nextRowOffset + static_cast<uint32_t>(x) + 1u;
            const uint32_t d = nextRowOffset + static_cast<uint32_t>(x);
            mesh.triangles[rowStart + static_cast<size_t>(x) * 2u + 0u] = {a, b, c};
            mesh.triangles[rowStart + static_cast<size_t>(x) * 2u + 1u] = {a, c, d};
        }
    });

    // Horizontal edges: M rows × (M-1) edges per row.
    ParallelForRows(M, [&](int z) {
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M - 1);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, a + 1u};
        }
    });

    // Vertical edges: (M-1) rows × M edges per row.
    const size_t vertOffset = horizEdges;
    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = vertOffset + static_cast<size_t>(z) * static_cast<size_t>(M);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, a + static_cast<uint32_t>(M)};
        }
    });

    // Diagonal edges (one per quad): (M-1) × (M-1).
    const size_t diagOffset = horizEdges + vertEdges;
    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = diagOffset + static_cast<size_t>(z) * static_cast<size_t>(M - 1);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            const uint32_t b = a + static_cast<uint32_t>(M) + 1u;
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, b};
        }
    });

    return mesh;
}

MeshData BuildMeshFromHeightPipeline(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap, HeightfieldGrid* previewGrid = nullptr)
{
    HeightfieldGrid grid = pipeline.useShape
        ? BuildHeightfieldFromShape(pipeline.shape, std::clamp(pipeline.shape.simulationResolution, 2, 2048), message)
        : BuildHeightfieldFromHeightmap(pipeline.heightmap, std::clamp(pipeline.heightmap.simulationResolution, 2, 2048), message);
    if (grid.resolution <= 0)
    {
        return {};
    }
    for (const HeightfieldPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur)
        {
            ApplyHeightmapBlur(grid, operation.heightmapBlur);
        }
        else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::ErosionNoise)
        {
            ApplyErosionNoise(grid, operation.erosionNoise);
        }
        else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion)
        {
            ApplyMultiScaleErosion(grid, operation.multiScaleErosion);
        }
        else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial)
        {
            ApplyMaskFluvial(grid, operation.maskFluvial);
        }
    }
    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    SelectHeightfieldPreviewField(grid, previewField);
    if (previewGrid != nullptr)
    {
        *previewGrid = grid;
    }
    return BuildMeshFromHeightfield(grid, resolution);
}
} // namespace

MeshData NodeGraph::BuildMeshFromHeightPipelineCached(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField, HeightfieldGrid* previewGrid)
{
    const GraphId sourceNodeId = pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId;
    if (sourceNodeId == 0)
    {
        return BuildMeshFromHeightPipeline(pipeline, resolution, message, previewField, previewGrid);
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
        g_currentlyEvaluatingNodeId.store(sourceNodeId, std::memory_order_relaxed);
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

    for (const HeightfieldPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.nodeId == 0)
        {
            if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur)
            {
                ApplyHeightmapBlur(grid, operation.heightmapBlur);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::ErosionNoise)
            {
                ApplyErosionNoise(grid, operation.erosionNoise);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion)
            {
                ApplyMultiScaleErosion(grid, operation.multiScaleErosion);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial)
            {
                ApplyMaskFluvial(grid, operation.maskFluvial);
            }
            continue;
        }

        uint64_t parameterHash = 0;
        switch (operation.kind)
        {
        case HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur:
            parameterHash = HashHeightmapBlurSettings(operation.heightmapBlur, simulationResolution);
            break;
        case HeightfieldPipeline::HeightfieldOperation::Kind::ErosionNoise:
            parameterHash = HashErosionNoiseSettings(operation.erosionNoise, simulationResolution);
            break;
        case HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion:
            parameterHash = HashMultiScaleErosionSettings(operation.multiScaleErosion, simulationResolution);
            break;
        case HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial:
            parameterHash = HashMaskFluvialSettings(operation.maskFluvial, simulationResolution);
            break;
        }
        HeightfieldNodeCache& operationCache = heightfieldCache_[operation.nodeId];
        if (!operationCache.valid ||
            operationCache.resolution != simulationResolution ||
            operationCache.inputHash != inputHash ||
            operationCache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(operation.nodeId, std::memory_order_relaxed);
            HeightfieldGrid operationGrid = grid;
            if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur)
            {
                ApplyHeightmapBlur(operationGrid, operation.heightmapBlur);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::ErosionNoise)
            {
                ApplyErosionNoise(operationGrid, operation.erosionNoise);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion)
            {
                ApplyMultiScaleErosion(operationGrid, operation.multiScaleErosion);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial)
            {
                ApplyMaskFluvial(operationGrid, operation.maskFluvial);
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
    if (previewGrid != nullptr)
    {
        *previewGrid = grid;
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
    links_.push_back({AllocateGraphId(), startPin, endPin});
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
    case NodeKind::HeightmapLoad:
    case NodeKind::Shape:
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::HeightmapBlur:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::ErosionNoise:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::MultiScaleErosion:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Flows");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Deposits");
        break;
    case NodeKind::MaskFluvial:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "HeightField");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskNoise:
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskBlend:
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "A");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "B");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
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
    RebuildNextGraphId();
    MarkDirty("Project nodes loaded");
}

void NodeGraph::ReplaceLinks(std::vector<Link> links)
{
    links_ = std::move(links);
    RebuildNextGraphId();
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

    // If the node has no HeightField output but does have a Mask output,
    // selecting the node body should default to the mask view — otherwise
    // the user sees terrain when the node produces a mask, which is
    // confusing. (Pin clicks still override via SetPreviewPin.)
    bool hasHeightOutput = false;
    bool hasMaskOutput = false;
    for (const Pin& pin : node->outputs)
    {
        if (pin.valueType == ValueType::HeightField) hasHeightOutput = true;
        else if (pin.valueType == ValueType::Mask) hasMaskOutput = true;
    }
    const bool defaultToMask = !hasHeightOutput && hasMaskOutput;

    evaluation_.previewNodeId = nodeId;
    evaluation_.previewPinId = 0;
    evaluation_.previewShowsMask = defaultToMask;
    evaluation_.previewField = defaultToMask ? HeightfieldPreviewField::Mask : HeightfieldPreviewField::Heightmap;
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
        else
        {
            previewField = HeightfieldPreviewField::Mask;
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

    const bool pipelineChanged = evaluation_.previewNodeId != node->id ||
                                 evaluation_.previewStage != stage ||
                                 evaluation_.previewField != previewField ||
                                 evaluation_.previewShowsMask != showsMask;
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

HeightfieldPipeline NodeGraph::PipelineFor(PreviewStage stage) const
{
    switch (stage)
    {
    case PreviewStage::HeightmapBlur:
        return PipelineTo(NodeKind::HeightmapBlur);
    case PreviewStage::Shape:
        return PipelineTo(NodeKind::Shape);
    case PreviewStage::ErosionNoise:
        return PipelineTo(NodeKind::ErosionNoise);
    case PreviewStage::MultiScaleErosion:
        return PipelineTo(NodeKind::MultiScaleErosion);
    case PreviewStage::MaskFluvial:
        return PipelineTo(NodeKind::MaskFluvial);
    case PreviewStage::Graph:
    default:
        if (const Node* node = FindFirstNode(NodeKind::MaskFluvial)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MultiScaleErosion)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::ErosionNoise)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::HeightmapBlur)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Shape)) { return PipelineToNode(*node); }
        return PipelineTo(NodeKind::HeightmapLoad);
    }
}

HeightfieldPipeline NodeGraph::PreviewPipeline() const
{
    if (const Node* previewNode = FindNode(evaluation_.previewNodeId))
    {
        return PipelineToNode(*previewNode);
    }
    return PipelineFor(evaluation_.previewStage);
}

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
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
    maskCache_ = evaluatedGraph.maskCache_;
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

const Node* NodeGraph::FindUpstreamForPin(GraphId pinId) const
{
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [pinId](const Link& link) {
        return link.endPin == pinId;
    });
    if (linkIt == links_.rend())
    {
        return nullptr;
    }
    return FindNodeByOutputPin(linkIt->startPin);
}

// Recursive descent through Mask Noise / Mask Blend nodes. Mask Blend follows
// both inputs (FindUpstreamNode only walks the first input), so we use
// FindUpstreamForPin per pin and merge with BlendMaskGrids. Each node's output
// is cached by (input hash, parameter hash) so unrelated edits do not re-run
// upstream noise generation.
MaskGrid NodeGraph::EvaluateMaskGridForNodeCached(const Node& node, int depth, uint64_t* outputHash)
{
    if (depth > 16)
    {
        if (outputHash != nullptr) { *outputHash = 0; }
        return {};
    }
    if (node.kind == NodeKind::MaskNoise)
    {
        const uint64_t parameterHash = HashMaskNoiseSettings(node.maskNoise);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != 0 || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = GenerateMaskNoise(node.maskNoise);
            cache.valid = true;
            cache.inputHash = 0;
            cache.parameterHash = parameterHash;
            cache.outputHash = parameterHash;
            HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
        }
        if (outputHash != nullptr) { *outputHash = cache.outputHash; }
        return cache.grid;
    }
    if (node.kind == NodeKind::MaskBlend)
    {
        uint64_t aHash = 0;
        uint64_t bHash = 0;
        MaskGrid a;
        MaskGrid b;
        if (node.inputs.size() >= 1)
        {
            if (const Node* upstream = FindUpstreamForPin(node.inputs[0].id))
            {
                if (IsMaskOnlyNodeKind(upstream->kind))
                {
                    a = EvaluateMaskGridForNodeCached(*upstream, depth + 1, &aHash);
                }
            }
        }
        if (node.inputs.size() >= 2)
        {
            if (const Node* upstream = FindUpstreamForPin(node.inputs[1].id))
            {
                if (IsMaskOnlyNodeKind(upstream->kind))
                {
                    b = EvaluateMaskGridForNodeCached(*upstream, depth + 1, &bHash);
                }
            }
        }
        uint64_t inputHash = 0;
        HashCombine(inputHash, aHash);
        HashCombine(inputHash, bHash);
        const uint64_t parameterHash = HashMaskBlendSettings(node.maskBlend);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = BlendMaskGrids(a, b, node.maskBlend.mode, node.maskBlend.intensity);
            cache.valid = true;
            cache.inputHash = inputHash;
            cache.parameterHash = parameterHash;
            cache.outputHash = inputHash;
            HashCombine(cache.outputHash, parameterHash);
            HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
        }
        if (outputHash != nullptr) { *outputHash = cache.outputHash; }
        return cache.grid;
    }
    if (outputHash != nullptr) { *outputHash = 0; }
    return {};
}

HeightfieldGrid NodeGraph::EvaluateMaskAsHeightfield(const Node& node, std::string* message)
{
    const MaskGrid mask = EvaluateMaskGridForNodeCached(node, 0, nullptr);
    HeightfieldGrid grid;
    if (mask.resolution <= 0)
    {
        if (message != nullptr)
        {
            *message = "Mask preview (no input)";
        }
        // Provide a default flat grid so the viewport still has something to draw.
        grid.resolution = 64;
        grid.terrainSizeMeters = 1024.0f;
        const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
        grid.heights.assign(cellCount, 0.0f);
        grid.mask.assign(cellCount, 0.0f);
        return grid;
    }
    grid.resolution = mask.resolution;
    grid.terrainSizeMeters = 1024.0f;
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.assign(cellCount, 0.0f);
    grid.mask = mask.values;
    if (message != nullptr)
    {
        *message = std::format("Mask preview {} x {}", grid.resolution, grid.resolution);
    }
    return grid;
}

HeightfieldPipeline NodeGraph::PipelineTo(NodeKind targetKind) const
{
    const Node* node = FindFirstNode(targetKind);
    return node != nullptr ? PipelineToNode(*node) : HeightfieldPipeline{};
}

HeightfieldPipeline NodeGraph::PipelineToNode(const Node& targetNode) const
{
    HeightfieldPipeline pipeline;
    const Node* node = &targetNode;
    int guard = 0;
    while (node != nullptr && guard++ < 16)
    {
        if (node->kind == NodeKind::HeightmapBlur)
        {
            pipeline.heightfieldOperations.push_back({
                HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur,
                node->id,
                node->heightmapBlur,
                {},
                {},
                {},
            });
        }
        else if (node->kind == NodeKind::ErosionNoise)
        {
            pipeline.heightfieldOperations.push_back({
                HeightfieldPipeline::HeightfieldOperation::Kind::ErosionNoise,
                node->id,
                {},
                node->erosionNoise,
                {},
                {},
            });
        }
        else if (node->kind == NodeKind::MultiScaleErosion)
        {
            pipeline.heightfieldOperations.push_back({
                HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion,
                node->id,
                {},
                {},
                node->multiScaleErosion,
                {},
            });
        }
        else if (node->kind == NodeKind::MaskFluvial)
        {
            pipeline.heightfieldOperations.push_back({
                HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial,
                node->id,
                {},
                {},
                {},
                node->maskFluvial,
            });
        }
        else if (node->kind == NodeKind::HeightmapLoad)
        {
            pipeline.hasSource = true;
            pipeline.heightmapNodeId = node->id;
            pipeline.heightmap = node->heightmap;
            break;
        }
        else if (node->kind == NodeKind::Shape)
        {
            pipeline.hasSource = true;
            pipeline.useShape = true;
            pipeline.shapeNodeId = node->id;
            pipeline.shape = node->shape;
            break;
        }

        node = FindUpstreamNode(*node);
    }
    std::ranges::reverse(pipeline.heightfieldOperations);
    return pipeline;
}

void NodeGraph::Evaluate(int previewMeshResolution)
{
    if (previewMeshResolution <= 0)
    {
        previewMeshResolution = EffectiveMeshResolution(settings_.preview, 2048);
    }

    // Track which node's kernel is running so the UI can paint a "計算中"
    // badge that walks the upstream chain. Cleared on exit so the badge
    // disappears when no kernel is active. Cache hits don't store —
    // they're instantaneous and the flicker would just be noise.
    struct ProgressGuard
    {
        ~ProgressGuard() { g_currentlyEvaluatingNodeId.store(0, std::memory_order_relaxed); }
    } progressGuard;
    g_currentlyEvaluatingNodeId.store(0, std::memory_order_relaxed);

    // Mask-only preview: Mask Noise / Mask Blend live in their own pipeline
    // (no upstream heightfield), so render them on a flat plane with the mask
    // channel populated from the mask graph.
    const Node* previewNode = FindNode(evaluation_.previewNodeId);
    if (previewNode != nullptr && IsMaskOnlyNodeKind(previewNode->kind))
    {
        evaluation_.previewMessage.clear();
        evaluation_.previewHeightfield = EvaluateMaskAsHeightfield(*previewNode, &evaluation_.previewMessage);
        evaluation_.previewShowsMask = true;
        evaluation_.previewField = HeightfieldPreviewField::Mask;
        evaluation_.previewMesh = evaluation_.previewHeightfield.resolution > 0
            ? BuildFlatMaskMesh(evaluation_.previewHeightfield, previewMeshResolution)
            : MeshData{};
        ++evaluation_.version;
        evaluation_.dirty = false;
        evaluation_.status = std::format(
            "Mask preview [{}] -> {} verts / {} tris",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size());
        return;
    }

    const HeightfieldPipeline previewPipeline = PreviewPipeline();
    evaluation_.previewShowsMask = evaluation_.previewShowsMask && previewPipeline.hasSource;
    if (!evaluation_.previewShowsMask)
    {
        evaluation_.previewField = HeightfieldPreviewField::Heightmap;
    }
    evaluation_.previewMessage.clear();
    if (!previewPipeline.hasSource)
    {
        evaluation_.previewHeightfield = {};
        evaluation_.previewMesh = {};
        evaluation_.previewMessage = "No source node";
    }
    else
    {
        evaluation_.previewMesh = BuildMeshFromHeightPipelineCached(previewPipeline, previewMeshResolution, &evaluation_.previewMessage, evaluation_.previewField, &evaluation_.previewHeightfield);
    }
    ++evaluation_.version;
    evaluation_.dirty = false;
    if (!previewPipeline.hasSource)
    {
        evaluation_.status = "No source node";
    }
    else
    {
        evaluation_.status = std::format(
            "Heightmap preview [{}] -> {} verts / {} tris{}",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.previewMesh.vertices.empty() ? " / no mesh" : "");
    }
}

GraphId NodeGraph::AddNode(NodeKind kind, std::string title)
{
    const GraphId id = AllocateGraphId();
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

    const GraphId id = AllocateGraphId();
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
    links_.push_back({AllocateGraphId(), startPin, endPin});
}

GraphId NodeGraph::AllocateGraphId()
{
    return nextGraphId_++;
}

void NodeGraph::RebuildNextGraphId()
{
    GraphId maxId = 0;
    for (const Node& node : nodes_)
    {
        maxId = std::max(maxId, node.id);
        for (const Pin& pin : node.inputs)
        {
            maxId = std::max(maxId, pin.id);
        }
        for (const Pin& pin : node.outputs)
        {
            maxId = std::max(maxId, pin.id);
        }
    }
    for (const Link& link : links_)
    {
        maxId = std::max(maxId, link.id);
    }
    nextGraphId_ = maxId + 1;
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

std::string_view ToString(MaskBlendMode mode)
{
    switch (mode)
    {
    case MaskBlendMode::Add:
        return "Add";
    case MaskBlendMode::Multiply:
        return "Multiply";
    case MaskBlendMode::Min:
        return "Min";
    case MaskBlendMode::Max:
        return "Max";
    default:
        return "Unknown";
    }
}

std::string_view ToString(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::HeightmapLoad:
        return "Import Heightmap";
    case NodeKind::HeightmapBlur:
        return "Heightmap Blur";
    case NodeKind::Shape:
        return "Shape";
    case NodeKind::ErosionNoise:
        return "Erosion Noise";
    case NodeKind::MultiScaleErosion:
        return "Multi-Scale Erosion";
    case NodeKind::MaskNoise:
        return "Mask Noise";
    case NodeKind::MaskBlend:
        return "Mask Blend";
    case NodeKind::MaskFluvial:
        return "Mask Fluvial";
    default:
        return "Unknown";
    }
}

std::string_view ToString(PreviewStage stage)
{
    switch (stage)
    {
    case PreviewStage::Graph:
        return "Graph";
    case PreviewStage::HeightmapBlur:
        return "Heightmap Blur";
    case PreviewStage::Shape:
        return "Shape";
    case PreviewStage::ErosionNoise:
        return "Erosion Noise";
    case PreviewStage::MultiScaleErosion:
        return "Multi-Scale Erosion";
    case PreviewStage::MaskNoise:
        return "Mask Noise";
    case PreviewStage::MaskBlend:
        return "Mask Blend";
    case PreviewStage::MaskFluvial:
        return "Mask Fluvial";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ValueType type)
{
    switch (type)
    {
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
    case NodeKind::HeightmapBlur:
        return PreviewStage::HeightmapBlur;
    case NodeKind::ErosionNoise:
        return PreviewStage::ErosionNoise;
    case NodeKind::MultiScaleErosion:
        return PreviewStage::MultiScaleErosion;
    case NodeKind::HeightmapLoad:
        return PreviewStage::Graph;
    case NodeKind::Shape:
        return PreviewStage::Shape;
    case NodeKind::MaskNoise:
        return PreviewStage::MaskNoise;
    case NodeKind::MaskBlend:
        return PreviewStage::MaskBlend;
    case NodeKind::MaskFluvial:
        return PreviewStage::MaskFluvial;
    default:
        return PreviewStage::Graph;
    }
}

bool IsMaskOnlyNodeKind(NodeKind kind)
{
    return kind == NodeKind::MaskNoise || kind == NodeKind::MaskBlend;
}

void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator)
{
    g_mseGpuEvaluator = evaluator;
}

void SetMaskNoiseGpuEvaluator(MaskNoiseGpuEvaluator evaluator)
{
    g_maskNoiseGpuEvaluator = evaluator;
}

std::atomic<GraphId>& CurrentlyEvaluatingNodeId()
{
    return g_currentlyEvaluatingNodeId;
}

} // namespace rock
