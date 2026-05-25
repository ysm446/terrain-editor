#include "Localization.h"

namespace terrain::ui
{
namespace
{
UiLanguage g_language = UiLanguage::Japanese;
}

UiLanguage CurrentLanguage()
{
    return g_language;
}

void SetCurrentLanguage(UiLanguage language)
{
    g_language = language;
}

const char* UiLanguageCode(UiLanguage language)
{
    switch (language)
    {
    case UiLanguage::English:
        return "en";
    case UiLanguage::Japanese:
    default:
        return "ja";
    }
}

UiLanguage UiLanguageFromCode(std::string_view code)
{
    if (code == "en" || code == "English")
    {
        return UiLanguage::English;
    }
    return UiLanguage::Japanese;
}

const char* Tr(const char* english, const char* japanese)
{
    return g_language == UiLanguage::Japanese ? japanese : english;
}
} // namespace terrain::ui
