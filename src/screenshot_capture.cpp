#include "screenshot_capture.h"

#include <chrono>
#include <ctime>

#include <dwmapi.h>
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

bool SaveBitmapAsPng(HBITMAP bitmap, const std::filesystem::path& path, std::string* error)
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

    ComPtr<IWICBitmap> sourceBitmap;
    hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &sourceBitmap);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to create WIC bitmap";
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
        if (error != nullptr) *error = "Failed to open screenshot file";
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
    hr = frame->WriteSource(sourceBitmap.Get(), nullptr);
    if (FAILED(hr))
    {
        if (error != nullptr) *error = "Failed to write screenshot pixels";
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

bool CaptureWindowScreenshot(HWND hwnd, const std::filesystem::path& directory, std::filesystem::path* savedPath, std::string* error)
{
    if (hwnd == nullptr)
    {
        if (error != nullptr) *error = "No window to capture";
        return false;
    }

    RECT rect{};
    HRESULT rectHr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
    if (FAILED(rectHr) && !GetWindowRect(hwnd, &rect))
    {
        if (error != nullptr) *error = "Failed to get window bounds";
        return false;
    }
    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    if (width <= 0 || height <= 0)
    {
        if (error != nullptr) *error = "Window has invalid size";
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    if (screenDc == nullptr)
    {
        if (error != nullptr) *error = "Failed to get screen device context";
        return false;
    }
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HBITMAP bitmap = CreateCompatibleBitmap(screenDc, width, height);
    if (memoryDc == nullptr || bitmap == nullptr)
    {
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (memoryDc != nullptr) DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        if (error != nullptr) *error = "Failed to create screenshot bitmap";
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    const BOOL copied = BitBlt(memoryDc, 0, 0, width, height, screenDc, rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);

    if (!copied)
    {
        DeleteObject(bitmap);
        if (error != nullptr) *error = "Failed to capture window";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec)
    {
        DeleteObject(bitmap);
        if (error != nullptr) *error = "Failed to create screenshot directory";
        return false;
    }

    const std::filesystem::path path = directory / ("terrain_editor_screenshot_" + ScreenshotTimestamp() + ".png");
    const bool saved = SaveBitmapAsPng(bitmap, path, error);
    DeleteObject(bitmap);
    if (!saved)
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
