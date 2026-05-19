#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>

#include <imgui.h>

namespace terrain::ui
{
struct PropertyWidgetCallbacks
{
    std::function<void(const char*)> markGraphChanged;
    std::function<void()> beginPropertyUndoEdit;
    std::function<void()> commitPropertyUndoEdit;
    std::function<void()> pushUndoSnapshot;
    std::function<std::optional<std::filesystem::path>(const std::string&)> showHeightmapFileDialog;
    std::function<std::string(const std::filesystem::path&)> pathToUtf8;
    std::function<std::string(const std::string&)> makeProjectAssetPathForJson;
};

void SetPropertyWidgetCallbacks(PropertyWidgetCallbacks callbacks);

bool FloatDiffersFromDefault(float value, float defaultValue);
bool ColorDiffersFromDefault(const std::array<float, 3>& value, const std::array<float, 3>& defaultValue);
std::string FormatDefaultFloat(float value, const char* format);
void DrawPropertyLabel(const char* label, const char* tooltip = nullptr, bool differsFromDefault = false);
bool DrawPropertyComboRow(const char* label, const char* id, int* value, const char* items, const char* tooltip = nullptr, int defaultValue = 0);
void DrawReadOnlyFloatRow(const char* label, float value, const char* format = "%.2f", const char* tooltip = nullptr);
bool DrawResetToDefaultButton(const char* id, bool isDefaultValue, const char* defaultValueText = nullptr);
bool DrawPresetIntRow(const char* label, const char* id, int* value, int defaultValue, std::span<const int> presets, int fallback, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr);
std::string FormatTimeHours(float hours);
bool DrawTimeOfDayRow(const char* label, const char* id, float* value, float defaultValue, const char* dirtyReason, const char* tooltip = nullptr);
bool DrawEnterCommitFloatInput(const char* id, float* value, const char* format);
bool DrawEnterCommitIntInput(const char* id, int* value);
bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr, const char* format = "%.3f", ImGuiSliderFlags sliderFlags = 0, float inputMinValue = std::numeric_limits<float>::quiet_NaN(), float inputMaxValue = std::numeric_limits<float>::quiet_NaN());
bool DrawPropertyFloatInputRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr, const char* format = "%.3f");
bool DrawPropertyPercentRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, const char* tooltip = nullptr);
bool DrawPropertyIntRow(const char* label, const char* id, int* value, int minValue, int maxValue, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr);
bool DrawPropertyBoolRow(const char* label, const char* id, bool* value, const char* dirtyReason, const char* tooltip = nullptr, bool defaultValue = false, bool compact = false);
bool DrawPropertyPathRow(const char* label, const char* id, std::string* value, const char* dirtyReason, const char* tooltip = nullptr);
bool DrawColorRgbRow(const char* label, const char* id, std::array<float, 3>& value, const std::array<float, 3>& defaultValue);
bool DrawCameraFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* format = "%.2f", const char* tooltip = nullptr);
} // namespace terrain::ui
