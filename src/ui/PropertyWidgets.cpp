#include "PropertyWidgets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <unordered_map>
#include <utility>

#include "Localization.h"

namespace terrain::ui
{
namespace
{
struct NumericTextInputState
{
    std::string text;
    bool active = false;
};

PropertyWidgetCallbacks g_callbacks;
std::unordered_map<ImGuiID, NumericTextInputState> g_numericTextInputs;

void MarkGraphChanged(const char* reason)
{
    if (g_callbacks.markGraphChanged)
    {
        g_callbacks.markGraphChanged(reason);
    }
}

void BeginPropertyUndoEdit()
{
    if (g_callbacks.beginPropertyUndoEdit)
    {
        g_callbacks.beginPropertyUndoEdit();
    }
}

void CommitPropertyUndoEdit()
{
    if (g_callbacks.commitPropertyUndoEdit)
    {
        g_callbacks.commitPropertyUndoEdit();
    }
}

void PushUndoSnapshot()
{
    if (g_callbacks.pushUndoSnapshot)
    {
        g_callbacks.pushUndoSnapshot();
    }
}

std::string FormatFloatInputText(float value, const char* format)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format ? format : "%.3f", value);
    return buffer;
}

bool ParseFloatInputText(const char* text, float* outValue)
{
    if (!text || !outValue)
    {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    if (*end != '\0')
    {
        return false;
    }
    *outValue = parsed;
    return true;
}

bool ParseIntInputText(const char* text, int* outValue)
{
    if (!text || !outValue)
    {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    if (*end != '\0')
    {
        return false;
    }
    *outValue = static_cast<int>(parsed);
    return true;
}
} // namespace

void SetPropertyWidgetCallbacks(PropertyWidgetCallbacks callbacks)
{
    g_callbacks = std::move(callbacks);
}

bool FloatDiffersFromDefault(float value, float defaultValue)
{
    return std::fabs(value - defaultValue) > 0.0001f;
}

bool ColorDiffersFromDefault(const std::array<float, 3>& value, const std::array<float, 3>& defaultValue)
{
    return FloatDiffersFromDefault(value[0], defaultValue[0]) ||
           FloatDiffersFromDefault(value[1], defaultValue[1]) ||
           FloatDiffersFromDefault(value[2], defaultValue[2]);
}

std::string FormatDefaultFloat(float value, const char* format)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format != nullptr ? format : "%.3f", value);
    return buffer;
}

void DrawPropertyLabel(const char* label, const char* tooltip, bool)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.22f, 0.22f, 0.22f, 0.97f));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

bool DrawPropertyComboRow(const char* label, const char* id, int* value, const char* items, const char* tooltip, int defaultValue)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(id);
    const float comboWidth = std::min(220.0f, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(comboWidth);
    const bool changed = ImGui::Combo("##value", value, items);
    ImGui::PopID();
    return changed;
}

void DrawReadOnlyFloatRow(const char* label, float value, const char* format, const char* tooltip)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, false);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(format, value);
}

bool DrawResetToDefaultButton(const char* id, bool isDefaultValue, const char* defaultValueText)
{
    ImGui::SameLine();
    ImGui::PushID(id);
    const float buttonSize = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button("##resetDefault", ImVec2(buttonSize, buttonSize));
    const char* resetIcon = "↺";
    ImFont* font = ImGui::GetFont();
    const float iconFontSize = ImGui::GetFontSize() * 1.25f;
    const ImVec2 iconSize = font->CalcTextSizeA(iconFontSize, FLT_MAX, 0.0f, resetIcon);
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const ImVec2 iconPos = {
        buttonMin.x + ((buttonMax.x - buttonMin.x) - iconSize.x) * 0.5f,
        buttonMin.y + ((buttonMax.y - buttonMin.y) - iconSize.y) * 0.5f,
    };
    const ImU32 iconColor = ImGui::GetColorU32(isDefaultValue ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddText(font, iconFontSize, iconPos, iconColor, resetIcon);
    if (ImGui::IsItemHovered())
    {
        if (defaultValueText != nullptr && defaultValueText[0] != '\0')
        {
            ImGui::SetTooltip(Tr("Reset to default\nDefault: %s", "既定値に戻す\n既定値: %s"), defaultValueText);
        }
        else
        {
            ImGui::SetTooltip("%s", Tr("Reset to default", "既定値に戻す"));
        }
    }
    ImGui::PopID();
    return pressed;
}

bool DrawPresetIntRow(const char* label,
                      const char* id,
                      int* value,
                      int defaultValue,
                      std::span<const int> presets,
                      int fallback,
                      const char* dirtyReason,
                      bool recordUndo,
                      const char* tooltip)
{
    auto nearestPreset = [presets, fallback](int rawValue) {
        const auto nearest = std::ranges::min_element(presets, [rawValue](int lhs, int rhs) {
            return std::abs(lhs - rawValue) < std::abs(rhs - rawValue);
        });
        return nearest != presets.end() ? *nearest : fallback;
    };

    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const int normalizedValue = nearestPreset(*value);
    if (*value != normalizedValue)
    {
        *value = normalizedValue;
    }
    const int normalizedDefault = nearestPreset(defaultValue);
    DrawPropertyLabel(label, tooltip, *value != normalizedDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    constexpr float comboWidth = 110.0f;
    const std::string previewValue = std::to_string(*value);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##preset", previewValue.c_str()))
    {
        for (int preset : presets)
        {
            const bool selected = *value == preset;
            const std::string presetText = std::to_string(preset);
            if (ImGui::Selectable(presetText.c_str(), selected))
            {
                if (*value != preset)
                {
                    if (recordUndo)
                    {
                        PushUndoSnapshot();
                    }
                    *value = preset;
                    MarkGraphChanged(dirtyReason);
                    changed = true;
                }
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const std::string defaultValueText = std::to_string(normalizedDefault);
    if (DrawResetToDefaultButton("reset", *value == normalizedDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = normalizedDefault;
        MarkGraphChanged(dirtyReason);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

std::string FormatTimeHours(float hours)
{
    int totalMinutes = static_cast<int>(std::round(std::clamp(hours, 0.0f, 24.0f) * 60.0f));
    totalMinutes = std::clamp(totalMinutes, 0, 24 * 60);
    const int hour = totalMinutes / 60;
    const int minute = totalMinutes % 60;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return buffer;
}

bool DrawTimeOfDayRow(const char* label, const char* id, float* value, float defaultValue, const char* dirtyReason, const char* tooltip)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, FloatDiffersFromDefault(*value, defaultValue));
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float valueWidth = 48.0f;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - valueWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x * 2.0f,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat("##slider", value, 0.0f, 24.0f, ""))
    {
        *value = std::clamp(*value, 0.0f, 24.0f);
        MarkGraphChanged(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    const std::string timeText = FormatTimeHours(*value);
    ImGui::TextUnformatted(timeText.c_str());

    const std::string defaultText = FormatTimeHours(defaultValue);
    if (DrawResetToDefaultButton("reset", !FloatDiffersFromDefault(*value, defaultValue), defaultText.c_str()))
    {
        *value = std::clamp(defaultValue, 0.0f, 24.0f);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    return editEnded;
}

bool DrawEnterCommitFloatInput(const char* id, float* value, const char* format)
{
    const ImGuiID inputId = ImGui::GetID(id);
    NumericTextInputState& state = g_numericTextInputs[inputId];
    if (!state.active)
    {
        state.text = FormatFloatInputText(*value, format);
    }

    char buffer[64]{};
    strncpy_s(buffer, state.text.c_str(), _TRUNCATE);
    const bool enterPressed = ImGui::InputText(id, buffer, IM_ARRAYSIZE(buffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsScientific);
    state.text = buffer;
    state.active = ImGui::IsItemActive();

    if (!enterPressed)
    {
        return false;
    }
    float parsed = *value;
    if (!ParseFloatInputText(buffer, &parsed))
    {
        return false;
    }
    *value = parsed;
    state.text = FormatFloatInputText(*value, format);
    return true;
}

bool DrawEnterCommitIntInput(const char* id, int* value)
{
    const ImGuiID inputId = ImGui::GetID(id);
    NumericTextInputState& state = g_numericTextInputs[inputId];
    if (!state.active)
    {
        state.text = std::to_string(*value);
    }

    char buffer[32]{};
    strncpy_s(buffer, state.text.c_str(), _TRUNCATE);
    const bool enterPressed = ImGui::InputText(id, buffer, IM_ARRAYSIZE(buffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
    state.text = buffer;
    state.active = ImGui::IsItemActive();

    if (!enterPressed)
    {
        return false;
    }
    int parsed = *value;
    if (!ParseIntInputText(buffer, &parsed))
    {
        return false;
    }
    *value = parsed;
    state.text = std::to_string(*value);
    return true;
}

bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo, const char* tooltip, const char* format, ImGuiSliderFlags sliderFlags, float inputMinValue, float inputMaxValue)
{
    bool editEnded = false;
    const float inputMin = std::isnan(inputMinValue) ? minValue : inputMinValue;
    const float inputMax = std::isnan(inputMaxValue) ? maxValue : inputMaxValue;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 76.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat("##slider", value, minValue, maxValue, format, sliderFlags))
    {
        MarkGraphChanged(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, inputMin, inputMax);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, inputMin, inputMax);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawPropertyFloatInputRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo, const char* tooltip, const char* format)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float inputWidth = std::max(80.0f, availableWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }

    ImGui::SameLine();
    differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, minValue, maxValue);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawPropertyPercentRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, const char* tooltip)
{
    float percentValue = *value * 100.0f;
    const float minPercent = minValue * 100.0f;
    const float maxPercent = maxValue * 100.0f;
    const float defaultPercent = defaultValue * 100.0f;
    const bool editEnded = DrawPropertyFloatRow(label, id, &percentValue, minPercent, maxPercent, defaultPercent, dirtyReason, true, tooltip);
    const float nextValue = std::clamp(percentValue / 100.0f, minValue, maxValue);
    if (nextValue != *value)
    {
        *value = nextValue;
    }
    return editEnded;
}

bool DrawPropertyIntRow(const char* label, const char* id, int* value, int minValue, int maxValue, int defaultValue, const char* dirtyReason, bool recordUndo, const char* tooltip)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = *value != defaultValue;
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 58.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderInt("##slider", value, minValue, maxValue))
    {
        MarkGraphChanged(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitIntInput("##number", value))
    {
        *value = std::clamp(*value, minValue, maxValue);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    differsFromDefault = *value != defaultValue;
    const std::string defaultValueText = std::to_string(defaultValue);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, minValue, maxValue);
        MarkGraphChanged(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawPropertyBoolRow(const char* label, const char* id, bool* value, const char* dirtyReason, const char* tooltip, bool defaultValue, bool compact)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    if (compact)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    }
    const bool changed = ImGui::Checkbox("##value", value);
    if (compact)
    {
        ImGui::PopStyleVar();
    }
    if (changed)
    {
        MarkGraphChanged(dirtyReason);
    }
    ImGui::PopID();
    return changed;
}

bool DrawPropertyPathRow(const char* label, const char* id, std::string* value, const char* dirtyReason, const char* tooltip)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, !value->empty());
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    char buffer[1024]{};
    strncpy_s(buffer, value->c_str(), _TRUNCATE);
    const float buttonWidth = 82.0f;
    const float inputWidth = std::clamp(
        ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        120.0f,
        260.0f);
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputText("##path", buffer, IM_ARRAYSIZE(buffer)))
    {
        *value = buffer;
        MarkGraphChanged(dirtyReason);
    }
    if (ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    if (ImGui::Button(Tr("Browse", "参照"), ImVec2(buttonWidth, 0.0f)))
    {
        if (g_callbacks.showHeightmapFileDialog && g_callbacks.pathToUtf8 && g_callbacks.makeProjectAssetPathForJson)
        {
            if (const std::optional<std::filesystem::path> path = g_callbacks.showHeightmapFileDialog(*value))
            {
                PushUndoSnapshot();
                *value = g_callbacks.makeProjectAssetPathForJson(g_callbacks.pathToUtf8(*path));
                MarkGraphChanged(dirtyReason);
                editEnded = true;
            }
        }
    }
    ImGui::PopID();
    if (editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawColorRgbRow(const char* label, const char* id, std::array<float, 3>& value, const std::array<float, 3>& defaultValue)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = ColorDiffersFromDefault(value, defaultValue);
    DrawPropertyLabel(label, nullptr, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float colorWidth = 356.0f;
    ImGui::SetNextItemWidth(colorWidth);
    bool changed = ImGui::ColorEdit3(
        "##rgb",
        value.data(),
        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoAlpha);
    value[0] = std::clamp(value[0], 0.0f, 1.0f);
    value[1] = std::clamp(value[1], 0.0f, 1.0f);
    value[2] = std::clamp(value[2], 0.0f, 1.0f);
    differsFromDefault = ColorDiffersFromDefault(value, defaultValue);
    const std::string defaultValueText = std::format("{:.3f}, {:.3f}, {:.3f}", defaultValue[0], defaultValue[1], defaultValue[2]);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        value = defaultValue;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool DrawCameraFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* format, const char* tooltip)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const bool differsFromDefaultBeforeEdit = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefaultBeforeEdit);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 76.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    changed = ImGui::SliderFloat("##slider", value, minValue, maxValue, format) || changed;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        changed = true;
    }
    const bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        *value = std::clamp(defaultValue, minValue, maxValue);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}
} // namespace terrain::ui
