#include "text_context.hpp"
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <limits>

namespace lshift {
bool WritableAutomationTarget(IUIAutomationTextRange* document,
    IUIAutomationTextRange* selection, std::wstring_view before_caret, bool logical_eof) {
    if (!document || !selection || before_caret.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    int start = 0, end = 0;
    if (FAILED(selection->CompareEndpoints(TextPatternRangeEndpoint_Start,
            document, TextPatternRangeEndpoint_Start, &start)) || start < 0 ||
        FAILED(selection->CompareEndpoints(TextPatternRangeEndpoint_End,
            document, TextPatternRangeEndpoint_End, &end))) return false;

    Microsoft::WRL::ComPtr<IUIAutomationTextRange> target;
    if (end > 0 && !logical_eof) {
        int collapsed = 0;
        if (!before_caret.empty() ||
            FAILED(selection->CompareEndpoints(TextPatternRangeEndpoint_Start,
                selection, TextPatternRangeEndpoint_End, &collapsed)) || collapsed == 0 ||
            FAILED(selection->Clone(&target)) || !target ||
            FAILED(target->MoveEndpointByRange(TextPatternRangeEndpoint_End,
                document, TextPatternRangeEndpoint_End)) ||
            FAILED(target->CompareEndpoints(TextPatternRangeEndpoint_Start,
                selection, TextPatternRangeEndpoint_Start, &start)) || start != 0 ||
            FAILED(target->CompareEndpoints(TextPatternRangeEndpoint_End,
                document, TextPatternRangeEndpoint_End, &collapsed)) || collapsed != 0) return false;
        // Выделение до логического EOF может заканчиваться за последним текстовым endpoint.
        // Допускаем его только при точном совпадении текста с ограниченным диапазоном.
        constexpr int limit = 64 * 1024;
        BSTR selected_text = nullptr, bounded_text = nullptr;
        const HRESULT selected_status = selection->GetText(limit + 1, &selected_text);
        const HRESULT bounded_status = target->GetText(limit + 1, &bounded_text);
        const auto length = SysStringLen(selected_text);
        const bool same = SUCCEEDED(selected_status) && SUCCEEDED(bounded_status) &&
            length > 0 && length <= limit &&
            std::wstring_view(selected_text, length) ==
                std::wstring_view(bounded_text ? bounded_text : L"", SysStringLen(bounded_text));
        SysFreeString(selected_text);
        SysFreeString(bounded_text);
        if (!same) return false;
        selection = target.Get();
        end = 0;
    }
    if (!before_caret.empty() || end > 0) {
        int collapsed = 0;
        if (FAILED(selection->CompareEndpoints(TextPatternRangeEndpoint_Start,
                selection, TextPatternRangeEndpoint_End, &collapsed)) || collapsed != 0) return false;
        // После подтверждения логического EOF используем физический конец редактора.
        if (end > 0) {
            if (FAILED(document->Clone(&target)) || !target ||
                FAILED(target->MoveEndpointByRange(TextPatternRangeEndpoint_Start,
                    document, TextPatternRangeEndpoint_End))) return false;
        } else if (FAILED(selection->Clone(&target)) || !target) return false;
        if (!before_caret.empty()) {
            const int length = static_cast<int>(before_caret.size());
            int moved = 0;
            if (FAILED(target->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character,
                    -length, &moved)) || moved != -length ||
                FAILED(target->CompareEndpoints(TextPatternRangeEndpoint_Start,
                    document, TextPatternRangeEndpoint_Start, &start)) || start < 0 ||
                FAILED(target->CompareEndpoints(TextPatternRangeEndpoint_End,
                    end > 0 ? document : selection, TextPatternRangeEndpoint_End, &collapsed)) ||
                collapsed != 0) return false;
            BSTR text = nullptr;
            const HRESULT read = target->GetText(length + 1, &text);
            const bool matches = SUCCEEDED(read) && text &&
                std::wstring_view(text, SysStringLen(text)) == before_caret;
            SysFreeString(text);
            if (!matches) return false;
        }
        selection = target.Get();
    }
    VARIANT value{};
    const HRESULT status = selection->GetAttributeValue(UIA_IsReadOnlyAttributeId, &value);
    const bool writable = SUCCEEDED(status) && value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE;
    VariantClear(&value);
    return writable;
}

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
        context.prefix_complete = skip == 0;
    }
    SysFreeString(value);
    if(FAILED(context.status)) return false;
    if(length > limit || context.prefix.find(L'\0') != std::wstring::npos) {
        context.status = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); return false;
    }
    if(end > 0) {
        context.logical_eof = true;
        context.stage = ContextStage::Complete;
        return true;
    }
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
