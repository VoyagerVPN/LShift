#include "localization.hpp"
#include <windows.h>

namespace lshift {
namespace {
[[nodiscard]] WORD ResourceLanguage(UiLanguage language) noexcept {
    return language == UiLanguage::Russian ? MAKELANGID(LANG_RUSSIAN, SUBLANG_DEFAULT) :
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}
struct LanguageSearch { WORD wanted; bool found = false; };
BOOL CALLBACK FindLanguage(HMODULE, LPCWSTR, LPCWSTR, WORD language, LONG_PTR context) {
    auto& search = *reinterpret_cast<LanguageSearch*>(context);
    if (language == search.wanted) search.found = true;
    return TRUE;
}
[[nodiscard]] std::wstring ReadResource(StringId id, UiLanguage language, bool exact) {
    const unsigned number = static_cast<unsigned>(id);
    if (number == 0 || number > kLastStringId) return {};
    const auto module = GetModuleHandleW(nullptr);
    const auto block = MAKEINTRESOURCEW(number / 16 + 1);
    const WORD lang = ResourceLanguage(language);
    if (exact) {
        LanguageSearch search{lang};
        if (!EnumResourceLanguagesW(module, RT_STRING, block, FindLanguage,
                                    reinterpret_cast<LONG_PTR>(&search)) || !search.found) return {};
    }
    const HRSRC resource = FindResourceExW(module, RT_STRING, block, lang);
    if (!resource) return {};
    const DWORD bytes = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const auto* cursor = static_cast<const WORD*>(LockResource(loaded));
    if (!cursor || bytes % sizeof(WORD) != 0) return {};
    const auto* end = cursor + bytes / sizeof(WORD);
    for (unsigned index = 0; index <= number % 16; ++index) {
        if (cursor >= end) return {};
        const auto length = *cursor++;
        if (static_cast<std::size_t>(end - cursor) < length) return {};
        if (index == number % 16) return std::wstring(reinterpret_cast<const wchar_t*>(cursor), length);
        cursor += length;
    }
    return {};
}
}

UiLanguage SystemUiLanguage(unsigned short language_id) noexcept {
    return PRIMARYLANGID(language_id) == LANG_RUSSIAN ? UiLanguage::Russian : UiLanguage::English;
}
std::wstring LocalizedText(StringId id, UiLanguage language) {
    return ReadResource(id, language, false);
}
bool HasLocalizedText(StringId id, UiLanguage language) {
    return !ReadResource(id, language, true).empty();
}
std::wstring ContextFailure(int stage, long code, UiLanguage language) {
    std::wstring text = LocalizedText(StringId::FieldContextError, language);
    const auto substitute = [&text](std::wstring_view key, const std::wstring& value) {
        const auto position = text.find(key);
        if (position != std::wstring::npos) text.replace(position, key.size(), value);
    };
    substitute(L"{stage}", std::to_wstring(stage));
    substitute(L"{code}", std::to_wstring(code));
    return text;
}
}
