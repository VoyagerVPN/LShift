#include "text_context.hpp"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace lshift {
bool ReadAutomationContext(IUIAutomationTextRange* document,
    IUIAutomationTextRange* selection, AutomationContext& context) {
    context = {};
    context.stage = ContextStage::Bounds;
    if(!document || !selection) { context.status = E_POINTER; return false; }
    int start = 0, end = 0, collapsed = 0;
    context.status = selection->CompareEndpoints(TextPatternRangeEndpoint_Start,
        document, TextPatternRangeEndpoint_Start, &start);
    if(FAILED(context.status)) return false;
    context.status = selection->CompareEndpoints(TextPatternRangeEndpoint_End,
        document, TextPatternRangeEndpoint_End, &end);
    if(FAILED(context.status)) return false;
    context.status = selection->CompareEndpoints(TextPatternRangeEndpoint_Start,
        selection, TextPatternRangeEndpoint_End, &collapsed);
    if(FAILED(context.status)) return false;
    if(start < 0) { context.status = HRESULT_FROM_WIN32(ERROR_INVALID_INDEX); return false; }
    if(collapsed != 0) { context.status = E_UNEXPECTED; return false; }
    Microsoft::WRL::ComPtr<IUIAutomationTextRange> prefix, suffix;
    // Character-перемещение у некоторых UIA-провайдеров покидает редактор.
    // Клонируем только DocumentRange подтверждённого поля и ограничиваем его курсором.
    context.stage = ContextStage::PrefixClone;
    context.status = document->Clone(&prefix);
    if(FAILED(context.status) || !prefix) return false;
    context.stage = ContextStage::PrefixEnd;
    context.status = prefix->MoveEndpointByRange(TextPatternRangeEndpoint_End,
        selection, TextPatternRangeEndpoint_Start);
    if(FAILED(context.status)) return false;
    BSTR value = nullptr;
    context.stage = ContextStage::PrefixRead;
    constexpr UINT limit = 64 * 1024;
    context.status = prefix->GetText(static_cast<int>(limit + 1), &value);
    const auto length = SysStringLen(value);
    if(SUCCEEDED(context.status) && length <= limit && end > 0) {
        // Логический EOF может сравниваться после последнего текстового endpoint.
        // Допускаем его только при точном равенстве всего префикса всему тексту поля.
        BSTR document_text = nullptr;
        context.status = document->GetText(static_cast<int>(limit + 1), &document_text);
        const bool same_text = SUCCEEDED(context.status) && length == SysStringLen(document_text) &&
            std::wstring_view(value ? value : L"", length) ==
                std::wstring_view(document_text ? document_text : L"", SysStringLen(document_text));
        SysFreeString(document_text);
        if(!same_text) {
            SysFreeString(value);
            if(SUCCEEDED(context.status)) context.status = E_BOUNDS;
            return false;
        }
    }
    if(SUCCEEDED(context.status) && length <= limit) {
        constexpr UINT kept = 4096;
        const auto skip = length > kept ? length - kept : 0;
        context.prefix.assign(value ? value + skip : L"", length - skip);
    }
    SysFreeString(value);
    if(FAILED(context.status)) return false;
    if(length > limit || context.prefix.find(L'\0') != std::wstring::npos) {
        context.status = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); return false;
    }
    if(end > 0) { context.stage = ContextStage::Complete; return true; }
    context.stage = ContextStage::SuffixClone;
    context.status = document->Clone(&suffix);
    if(FAILED(context.status) || !suffix) return false;
    context.stage = ContextStage::SuffixStart;
    context.status = suffix->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
        selection, TextPatternRangeEndpoint_End);
    if(FAILED(context.status)) return false;
    value = nullptr;
    context.stage = ContextStage::SuffixRead;
    context.status = suffix->GetText(1, &value);
    const auto suffix_length = SysStringLen(value);
    if(SUCCEEDED(context.status) && suffix_length != 0) context.following = value[0];
    SysFreeString(value);
    if(FAILED(context.status)) return false;
    context.stage = ContextStage::Complete;
    return true;
}
}
