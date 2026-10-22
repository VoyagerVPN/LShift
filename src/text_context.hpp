#pragma once
#include <windows.h>
#include <string>

struct IUIAutomationTextRange;
namespace lshift {
enum class ContextStage { None, Bounds, PrefixClone, PrefixEnd, PrefixRead,
    SuffixClone, SuffixStart, SuffixRead, Complete };
struct AutomationContext {
    std::wstring prefix;
    wchar_t following = L'\0';
    ContextStage stage = ContextStage::None;
    HRESULT status = S_OK;
};
// Чтение подтверждённого поля без изменения выделения, фокуса и содержимого.
[[nodiscard]] bool ReadAutomationContext(IUIAutomationTextRange* document,
    IUIAutomationTextRange* selection, AutomationContext& context);
}
