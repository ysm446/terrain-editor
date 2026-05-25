#pragma once

#include <string_view>

namespace terrain::ui
{
enum class UiLanguage
{
    English,
    Japanese,
};

UiLanguage CurrentLanguage();
void SetCurrentLanguage(UiLanguage language);
const char* UiLanguageCode(UiLanguage language);
UiLanguage UiLanguageFromCode(std::string_view code);
const char* Tr(const char* english, const char* japanese);
} // namespace terrain::ui
