#include "screenshot_capture.h"

#include <chrono>
#include <ctime>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace terrain
{
namespace
{
std::string ScreenshotTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y%m%d_%H%M%S", &localTime);
    return buffer;
}

// COM の初期化をスコープで管理する。別モードで初期化済みのスレッドでも動作させる。
class ComScope
{
public:
    ComScope()
        : hr_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }
    ~ComScope()
    {
        if (SUCCEEDED(hr_))
        {
            CoUninitialize();
        }
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

    bool Usable() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

bool EncodePngFile(IWICImagingFactory* factory,
                   IWICBitmapSource* source,
                   const std::filesystem::path& path,
                   std::string* error)
{
    ComPtr<IWICStream> stream;
    HRESULT hr = factory->CreateStream(&stream);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC stream";
        return false;
    }
    hr = stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to open screenshot file";
        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create PNG encoder";
        return false;
    }
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to initialize PNG encoder";
        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> propertyBag;
    hr = encoder->CreateNewFrame(&frame, &propertyBag);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create PNG frame";
        return false;
    }
    hr = frame->Initialize(propertyBag.Get());
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to initialize PNG frame";
        return false;
    }
    hr = frame->WriteSource(source, nullptr);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to write screenshot pixels";
        return false;
    }
    hr = frame->Commit();
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to commit PNG frame";
        return false;
    }
    hr = encoder->Commit();
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to commit PNG file";
        return false;
    }
    return true;
}
} // namespace

bool SaveRgbaScreenshot(const uint8_t* pixels,
                        int width,
                        int height,
                        int rowPitchBytes,
                        const std::filesystem::path& directory,
                        std::filesystem::path* savedPath,
                        std::string* error)
{
    if (pixels == nullptr || width <= 0 || height <= 0 || rowPitchBytes < width * 4)
    {
        if (error != nullptr) *error = "Invalid screenshot pixels";
        return false;
    }

    // WIC へ渡すため RGBA を BGRA に詰め直す。読み戻しの行パディングもここで落とす。
    // アルファは常に不透明にして、UI のブレンド結果が透過 PNG にならないようにする。
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* src = pixels + static_cast<size_t>(y) * static_cast<size_t>(rowPitchBytes);
        uint8_t* dst = bgra.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        for (int x = 0; x < width; ++x)
        {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 0xFF;
            src += 4;
            dst += 4;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        if (error != nullptr) *error = "Failed to create screenshot directory";
        return false;
    }

    ComScope com;
    if (!com.Usable())
    {
        if (error != nullptr) *error = "COM initialization failed";
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC factory";
        return false;
    }

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmapFromMemory(
        static_cast<UINT>(width),
        static_cast<UINT>(height),
        GUID_WICPixelFormat32bppBGRA,
        static_cast<UINT>(width) * 4,
        static_cast<UINT>(bgra.size()),
        bgra.data(),
        &bitmap);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC bitmap";
        return false;
    }

    const std::filesystem::path path = directory / ("terrain_editor_screenshot_" + ScreenshotTimestamp() + ".png");
    if (!EncodePngFile(factory.Get(), bitmap.Get(), path, error))
    {
        return false;
    }
    if (savedPath != nullptr)
    {
        *savedPath = path;
    }
    return true;
}
} // namespace terrain
