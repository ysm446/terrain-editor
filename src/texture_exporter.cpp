#include "texture_exporter.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace terrain
{
namespace
{
constexpr int kMaxExportResolution = 2048;

std::filesystem::path EnsurePngExtension(std::filesystem::path path)
{
    if (path.extension().empty())
    {
        path.replace_extension(".png");
    }
    return path;
}

bool PrepareOutputPath(const std::filesystem::path& path, std::string* error)
{
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty())
    {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec)
    {
        if (error != nullptr) *error = "Failed to create export directory";
        return false;
    }
    return true;
}

size_t SampleIndex(int srcResolution, int dstResolution, int x, int y)
{
    const int srcX = std::clamp((x * srcResolution) / dstResolution, 0, srcResolution - 1);
    const int srcY = std::clamp(((dstResolution - 1 - y) * srcResolution) / dstResolution, 0, srcResolution - 1);
    return static_cast<size_t>(srcY) * static_cast<size_t>(srcResolution) + static_cast<size_t>(srcX);
}

const std::vector<float>* SelectMaskField(const rock::HeightfieldGrid& grid, rock::HeightfieldPreviewField field)
{
    switch (field)
    {
    case rock::HeightfieldPreviewField::Deposits:
        return &grid.deposits;
    case rock::HeightfieldPreviewField::Flows:
        return &grid.flows;
    case rock::HeightfieldPreviewField::Age:
        return &grid.age;
    case rock::HeightfieldPreviewField::UniqueMask:
        return &grid.uniqueMask;
    case rock::HeightfieldPreviewField::Mask:
    case rock::HeightfieldPreviewField::Heightmap:
    default:
        return &grid.mask;
    }
}

std::vector<uint8_t> BuildMaskPixels(const rock::EvaluationSummary& evaluation, int resolution)
{
    const rock::HeightfieldGrid& grid = evaluation.previewHeightfield;
    const std::vector<float>* source = SelectMaskField(grid, evaluation.previewField);
    std::vector<uint8_t> pixels(static_cast<size_t>(resolution) * static_cast<size_t>(resolution), 0);
    const int srcResolution = grid.resolution;
    const size_t expected = static_cast<size_t>(srcResolution) * static_cast<size_t>(srcResolution);
    if (srcResolution < 2 || source == nullptr || source->size() < expected)
    {
        return pixels;
    }

    for (int y = 0; y < resolution; ++y)
    {
        for (int x = 0; x < resolution; ++x)
        {
            const float value = std::clamp((*source)[SampleIndex(srcResolution, resolution, x, y)], 0.0f, 1.0f);
            pixels[static_cast<size_t>(y) * static_cast<size_t>(resolution) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(std::lround(value * 255.0f));
        }
    }
    return pixels;
}

std::vector<uint8_t> BuildColorPixelsBgra(const rock::EvaluationSummary& evaluation, int resolution)
{
    const rock::ColorGrid& grid = evaluation.previewColorGrid;
    std::vector<uint8_t> pixels(static_cast<size_t>(resolution) * static_cast<size_t>(resolution) * 4u, 255);
    const int srcResolution = grid.resolution;
    const size_t expected = static_cast<size_t>(srcResolution) * static_cast<size_t>(srcResolution) * 4u;
    if (srcResolution < 2 || grid.pixels.size() < expected)
    {
        return pixels;
    }

    for (int y = 0; y < resolution; ++y)
    {
        for (int x = 0; x < resolution; ++x)
        {
            const size_t src = SampleIndex(srcResolution, resolution, x, y) * 4u;
            const size_t dst = (static_cast<size_t>(y) * static_cast<size_t>(resolution) + static_cast<size_t>(x)) * 4u;
            pixels[dst + 0] = grid.pixels[src + 2];
            pixels[dst + 1] = grid.pixels[src + 1];
            pixels[dst + 2] = grid.pixels[src + 0];
            pixels[dst + 3] = grid.pixels[src + 3];
        }
    }
    return pixels;
}

std::vector<uint16_t> BuildHeightPixels(const rock::EvaluationSummary& evaluation, int resolution)
{
    const rock::HeightfieldGrid& grid = evaluation.previewHeightfield;
    std::vector<uint16_t> pixels(static_cast<size_t>(resolution) * static_cast<size_t>(resolution), 0);
    const int srcResolution = grid.resolution;
    const size_t expected = static_cast<size_t>(srcResolution) * static_cast<size_t>(srcResolution);
    if (srcResolution < 2 || grid.heights.size() < expected)
    {
        return pixels;
    }

    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();
    for (float h : grid.heights)
    {
        if (!std::isfinite(h))
        {
            continue;
        }
        minHeight = std::min(minHeight, h);
        maxHeight = std::max(maxHeight, h);
    }

    const float range = maxHeight - minHeight;
    if (range <= 1e-6f || !std::isfinite(range))
    {
        return pixels;
    }

    for (int y = 0; y < resolution; ++y)
    {
        for (int x = 0; x < resolution; ++x)
        {
            const float height = grid.heights[SampleIndex(srcResolution, resolution, x, y)];
            const float normalized = std::clamp((height - minHeight) / range, 0.0f, 1.0f);
            pixels[static_cast<size_t>(y) * static_cast<size_t>(resolution) + static_cast<size_t>(x)] =
                static_cast<uint16_t>(std::lround(normalized * 65535.0f));
        }
    }
    return pixels;
}

bool WritePngPixels(
    const std::filesystem::path& path,
    int width,
    int height,
    WICPixelFormatGUID pixelFormat,
    UINT stride,
    UINT bufferSize,
    BYTE* pixels,
    std::string* error)
{
    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE)
    {
        if (error != nullptr) *error = "COM initialization failed";
        return false;
    }

    const auto finish = [&]() {
        if (shouldUninitialize)
        {
            CoUninitialize();
        }
    };

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC factory";
        finish();
        return false;
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC stream";
        finish();
        return false;
    }
    hr = stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to open export file";
        finish();
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create PNG encoder";
        finish();
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to initialize PNG encoder";
        finish();
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> propertyBag;
    hr = encoder->CreateNewFrame(&frame, &propertyBag);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create PNG frame";
        finish();
        return false;
    }
    hr = frame->Initialize(propertyBag.Get());
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to initialize PNG frame";
        finish();
        return false;
    }
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to set PNG size";
        finish();
        return false;
    }

    WICPixelFormatGUID actualFormat = pixelFormat;
    hr = frame->SetPixelFormat(&actualFormat);
    if (FAILED(hr) || !IsEqualGUID(actualFormat, pixelFormat))
    {
        if (error != nullptr) *error = "PNG pixel format is not supported";
        finish();
        return false;
    }

    hr = frame->WritePixels(static_cast<UINT>(height), stride, bufferSize, pixels);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to write PNG pixels";
        finish();
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to commit PNG frame";
        finish();
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to commit PNG file";
        finish();
        return false;
    }

    finish();
    return true;
}
} // namespace

bool ExportPreviewTexturePng(
    const rock::EvaluationSummary& evaluation,
    const std::filesystem::path& rawPath,
    int requestedResolution,
    std::string* error)
{
    if (evaluation.dirty)
    {
        if (error != nullptr) *error = "Preview has not been evaluated";
        return false;
    }

    const std::filesystem::path path = EnsurePngExtension(rawPath);
    if (!PrepareOutputPath(path, error))
    {
        return false;
    }

    const int resolution = std::clamp(requestedResolution, 2, kMaxExportResolution);
    if (evaluation.previewIsColor)
    {
        std::vector<uint8_t> pixels = BuildColorPixelsBgra(evaluation, resolution);
        return WritePngPixels(
            path,
            resolution,
            resolution,
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(resolution * 4),
            static_cast<UINT>(pixels.size()),
            pixels.data(),
            error);
    }

    if (evaluation.previewShowsMask)
    {
        std::vector<uint8_t> pixels = BuildMaskPixels(evaluation, resolution);
        return WritePngPixels(
            path,
            resolution,
            resolution,
            GUID_WICPixelFormat8bppGray,
            static_cast<UINT>(resolution),
            static_cast<UINT>(pixels.size()),
            pixels.data(),
            error);
    }

    std::vector<uint16_t> pixels = BuildHeightPixels(evaluation, resolution);
    return WritePngPixels(
        path,
        resolution,
        resolution,
        GUID_WICPixelFormat16bppGray,
        static_cast<UINT>(resolution * sizeof(uint16_t)),
        static_cast<UINT>(pixels.size() * sizeof(uint16_t)),
        reinterpret_cast<BYTE*>(pixels.data()),
        error);
}
} // namespace terrain
