#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace terrain
{
// RGBA8 のピクセル列 (行間隔 rowPitchBytes バイト) を PNG として directory 内へ保存する。
// スワップチェーンのバックバッファ読み戻しを想定しているため、ウィンドウ枠やタイトルバーは含まれない。
bool SaveRgbaScreenshot(const uint8_t* pixels,
                        int width,
                        int height,
                        int rowPitchBytes,
                        const std::filesystem::path& directory,
                        std::filesystem::path* savedPath,
                        std::string* error);
}
