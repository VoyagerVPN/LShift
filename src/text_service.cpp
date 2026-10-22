#include "text_service.hpp"
#include "clipboard.hpp"
#include "text_context.hpp"

#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <algorithm>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

namespace lshift {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t ReadLimit = 4096;
constexpr std::size_t ReplaceLimit = 64 * 1024;
constexpr UINT CallTimeout = 200;
constexpr std::uint64_t LayoutWait = 150;

struct Guard {
    const TextRequest& request;
    const std::function<bool()>& predicate;

    [[nodiscard]] bool Current() const {
        const bool current = request.foreground != nullptr && IsWindow(request.foreground) &&
            GetForegroundWindow() == request.foreground &&
            GetTickCount64() <= request.deadline_ms && predicate && predicate();
        if (!current || request.focus == nullptr) return current;
        GUITHREADINFO info{sizeof(info)};
        const DWORD thread = GetWindowThreadProcessId(request.foreground, nullptr);
        return thread != 0 && GetGUIThreadInfo(thread, &info) && info.hwndFocus == request.focus;
    }
};

struct Bstr {
    BSTR value = nullptr;
    Bstr() = default;
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(value); }
};

struct Layouts {
    HKL english = nullptr;
    HKL russian = nullptr;
};

[[nodiscard]] TextResult Result(TextStatus status, const wchar_t* detail) {
    return {status, detail};
}

[[nodiscard]] bool ModifiersUp() noexcept {
    constexpr int keys[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL,
                            VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN};
    for (const int key : keys) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) return false;
    }
    return true;
}

[[nodiscard]] DWORD LayoutCode(HKL layout) noexcept {
    return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(layout) & 0xFFFFFFFFULL);
}

[[nodiscard]] Layouts InstalledLayouts() {
    Layouts result;
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0 || count > 128) return result;
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    const int copied = GetKeyboardLayoutList(count, layouts.data());
    for (int index = 0; index < copied; ++index) {
        const HKL layout = layouts[static_cast<std::size_t>(index)];
        if (LayoutCode(layout) == 0x04090409UL) result.english = layout;
        if (LayoutCode(layout) == 0x04190419UL) result.russian = layout;
    }
    return result;
}

[[nodiscard]] HWND FocusedWindow(const Guard& guard) {
    if (!guard.Current()) return nullptr;
    const DWORD thread = GetWindowThreadProcessId(guard.request.foreground, nullptr);
    if (thread == 0) return nullptr;
    GUITHREADINFO info{};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    if (!GetGUIThreadInfo(thread, &info)) return nullptr;
    return info.hwndFocus;
}

[[nodiscard]] bool SameNativeFocus(const Guard& guard, HWND focus) {
    return focus != nullptr && guard.Current() && IsWindow(focus) &&
        FocusedWindow(guard) == focus &&
        GetAncestor(focus, GA_ROOT) == guard.request.foreground;
}

[[nodiscard]] bool SameLayoutFocus(const Guard& guard, HWND focus) {
    if (!guard.Current()) return false;
    const HWND current = FocusedWindow(guard);
    return current == focus || (current == nullptr && focus == guard.request.foreground);
}

[[nodiscard]] bool NativeEditable(HWND window) noexcept {
    if (!IsWindowUnicode(window) || !IsWindowEnabled(window)) return false;
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (style == 0 && GetLastError() != ERROR_SUCCESS) return false;
    return (style & (ES_PASSWORD | ES_READONLY)) == 0;
}

[[nodiscard]] bool IsNativeClass(HWND window) noexcept {
    wchar_t buffer[256]{};
    const int length = GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
    if (length <= 0) return false;
    constexpr const wchar_t* names[] = {
        L"Edit", L"RichEdit", L"RichEdit20W", L"RichEdit20A", L"RichEdit50W",
        L"RICHEDIT50W", L"RichEditD2DPT"
    };
    for (const wchar_t* name : names) {
        if (CompareStringOrdinal(buffer, length, name, -1, TRUE) == CSTR_EQUAL) return true;
    }
    constexpr wchar_t prefix[] = L"WindowsForms10.EDIT";
    constexpr int prefix_length = static_cast<int>(std::size(prefix) - 1);
    return length >= prefix_length &&
        CompareStringOrdinal(buffer, prefix_length, prefix, prefix_length, TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool IsPlainEdit(HWND window) noexcept {
    wchar_t name[256]{};
    const int length = GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (length <= 0) return false;
    if (CompareStringOrdinal(name, length, L"Edit", -1, TRUE) == CSTR_EQUAL) return true;
    constexpr wchar_t prefix[] = L"WindowsForms10.EDIT";
    constexpr int prefix_length = static_cast<int>(std::size(prefix) - 1);
    return length >= prefix_length &&
        CompareStringOrdinal(name, prefix_length, prefix, prefix_length, TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool Message(const Guard& guard, HWND window, UINT message,
                           WPARAM wparam, LPARAM lparam, DWORD_PTR& result) {
    if (!guard.Current()) return false;
    const auto now = GetTickCount64();
    if (now >= guard.request.deadline_ms) return false;
    const auto remaining = guard.request.deadline_ms - now;
    const UINT timeout = static_cast<UINT>(std::min<std::uint64_t>(remaining, CallTimeout));
    return SendMessageTimeoutW(window, message, wparam, lparam,
        SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, timeout, &result) != 0;
}

struct NativeState {
    std::wstring text;
    std::size_t start = 0;
    std::size_t end = 0;
};

[[nodiscard]] bool ReadNative(const Guard& guard, HWND window, NativeState& state) {
    if (!SameNativeFocus(guard, window) || !NativeEditable(window)) return false;
    DWORD_PTR length = 0;
    if (!Message(guard, window, WM_GETTEXTLENGTH, 0, 0, length) || length > ReadLimit) return false;
    std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1, L'\0');
    DWORD_PTR copied = 0;
    if (!Message(guard, window, WM_GETTEXT, static_cast<WPARAM>(text.size()),
                 reinterpret_cast<LPARAM>(text.data()), copied) || copied > length) return false;
    state.text.assign(text.data(), static_cast<std::size_t>(copied));
    // Версии RichEdit по-разному представляют абзацы и объекты в WM_GETTEXT и cp-индексах.
    // Пока соответствие не проверено, не используем эти смещения для адресной записи.
    if (!IsPlainEdit(window) && state.text.find_first_of(L"\r\n\uFFFC") != std::wstring::npos) return false;
    DWORD_PTR selection = 0;
    if (!Message(guard, window, EM_GETSEL, 0, 0, selection) ||
        selection == std::numeric_limits<DWORD_PTR>::max()) return false;
    state.start = static_cast<std::size_t>(LOWORD(selection));
    state.end = static_cast<std::size_t>(HIWORD(selection));
    return state.start <= state.end && state.end <= state.text.size() &&
        SameNativeFocus(guard, window);
}

[[nodiscard]] bool SwitchLayout(const Guard& guard, HWND focus, HKL target) {
    if (!SameLayoutFocus(guard, focus) || !ModifiersUp()) return false;
    const DWORD thread = GetWindowThreadProcessId(focus, nullptr);
    if (thread == 0) return false;
    if (GetKeyboardLayout(thread) == target) return true;
    if (!PostMessageW(focus, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(target))) return false;
    const auto until = std::min<std::uint64_t>(guard.request.deadline_ms, GetTickCount64() + LayoutWait);
    while (SameLayoutFocus(guard, focus) && GetTickCount64() <= until) {
        if (GetKeyboardLayout(thread) == target) return true;
        Sleep(5);
    }
    return false;
}

[[nodiscard]] TextResult Toggle(const Guard& guard, HWND focus, const Layouts& layouts) {
    if (!guard.Current()) return Result(TextStatus::Stale, L"Команда устарела.");
    const DWORD thread = GetWindowThreadProcessId(focus, nullptr);
    if (thread == 0) return Result(TextStatus::Unsupported, L"Поток поля недоступен.");
    const HKL current = GetKeyboardLayout(thread);
    const HKL target = current == layouts.english ? layouts.russian :
                       current == layouts.russian ? layouts.english : nullptr;
    if (target == nullptr) return Result(TextStatus::Unsupported, L"Активна неподдерживаемая раскладка.");
    if (SwitchLayout(guard, focus, target)) return Result(TextStatus::Switched, L"Язык переключён.");
    return Result(guard.Current() ? TextStatus::Failed : TextStatus::Stale,
                  L"Переключение языка не подтверждено.");
}

[[nodiscard]] TextResult Converted(const Guard& guard, HWND focus, Direction direction,
                                   const Layouts& layouts) {
    const HKL target = direction == Direction::ToRussian ? layouts.russian : layouts.english;
    if (!SwitchLayout(guard, focus, target)) {
        return Result(TextStatus::Partial, L"Текст изменён; переключение языка не подтверждено.");
    }
    return Result(TextStatus::Converted, L"Текст исправлен, язык переключён.");
}

[[nodiscard]] TextResult ProcessRecent(const Guard& guard, HWND focus, const Layouts& layouts,
                                      std::wstring_view prefix, wchar_t following, const std::function<bool()>& confirm,
                                      bool& touched);

[[nodiscard]] TextResult ProcessNative(const Guard& guard, HWND focus,
                                       const Layouts& layouts, bool& touched) {
    NativeState before;
    if (!ReadNative(guard, focus, before)) {
        return Result(guard.Current() ? TextStatus::Unsupported : TextStatus::Stale,
                      L"Не удалось безопасно прочитать поле.");
    }
    std::optional<TextSpan> span;
    const bool has_selection = before.start != before.end;
    if (!has_selection) {
        const auto confirm = [&] {
            NativeState current;
            return ReadNative(guard, focus, current) && current.text == before.text &&
                current.start == before.start && current.end == before.end && NativeEditable(focus);
        };
        return ProcessRecent(guard, focus, layouts,
            std::wstring_view(before.text).substr(0, before.start),
            before.start < before.text.size() ? before.text[before.start] : L'\0', confirm, touched);
    }
    if (has_selection) span = TextSpan{before.start, before.end - before.start};
    else span = LastWord(before.text, before.start);
    if (!span) return Toggle(guard, focus, layouts);
    if (span->length > ReplaceLimit || span->start > before.text.size() ||
        span->length > before.text.size() - span->start ||
        (!has_selection && span->start + span->length > before.start)) {
        return Result(TextStatus::Unsupported, L"Диапазон текста слишком велик.");
    }
    const auto original = std::wstring_view(before.text).substr(span->start, span->length);
    const auto conversion = ConvertWords(original);
    const auto direction = conversion.last_direction;
    if (!direction) return Toggle(guard, focus, layouts);
    const std::wstring& replacement = conversion.text;
    if (replacement == original) return Toggle(guard, focus, layouts);
    std::wstring expected = before.text;
    expected.replace(span->start, span->length, replacement);
    if (replacement.size() > ReplaceLimit || expected.size() > ReadLimit) {
        return Result(TextStatus::Unsupported, L"Результат превышает ограничение размера.");
    }
    NativeState current;
    if (!ReadNative(guard, focus, current) || current.text != before.text ||
        current.start != before.start || current.end != before.end) {
        return Result(TextStatus::Stale, L"Текст или курсор изменились.");
    }
    DWORD_PTR ignored = 0;
    if (!SameNativeFocus(guard, focus) || !NativeEditable(focus) || !ModifiersUp()) {
        return Result(TextStatus::Stale, L"Условия ввода изменились.");
    }
    touched = true;
    if (!Message(guard, focus, EM_SETSEL, static_cast<WPARAM>(span->start),
                 static_cast<LPARAM>(span->start + span->length), ignored)) {
        return Result(TextStatus::Partial, L"Изменение выделения не подтверждено.");
    }
    NativeState selected;
    if (!ReadNative(guard, focus, selected) || selected.text != before.text ||
        selected.start != span->start || selected.end != span->start + span->length ||
        !NativeEditable(focus)) {
        return Result(TextStatus::Partial, L"Контекст изменился после выделения; замена отменена.");
    }
    if (!Message(guard, focus, EM_REPLACESEL, TRUE,
                 reinterpret_cast<LPARAM>(replacement.c_str()), ignored)) {
        return Result(TextStatus::Partial, L"Результат замены не подтверждён.");
    }
    NativeState after;
    const std::size_t expected_caret = span->start + replacement.size();
    if (!ReadNative(guard, focus, after) || after.text != expected ||
        after.start != expected_caret || after.end != expected_caret) {
        return Result(TextStatus::Partial, L"Поле не подтвердило ожидаемый текст.");
    }
    if (!has_selection) {
        const std::size_t restored_caret = expected_caret +
            (before.start - span->start - span->length);
        if (restored_caret != expected_caret) {
            if (!SameNativeFocus(guard, focus) || !NativeEditable(focus) ||
                !Message(guard, focus, EM_SETSEL, static_cast<WPARAM>(restored_caret),
                         static_cast<LPARAM>(restored_caret), ignored)) {
                return Result(TextStatus::Partial,
                              L"Текст исправлен; исходное смещение курсора после слова не восстановлено.");
            }
            NativeState restored;
            if (!ReadNative(guard, focus, restored) || restored.text != expected ||
                restored.start != restored_caret || restored.end != restored_caret) {
                return Result(TextStatus::Partial,
                              L"Текст исправлен; исходное смещение курсора после слова не подтверждено.");
            }
        }
    }
    return Converted(guard, focus, *direction, layouts);
}

[[nodiscard]] bool ReadRange(IUIAutomationTextRange* range, std::size_t limit,
                             std::wstring& text) {
    Bstr value;
    if (FAILED(range->GetText(static_cast<int>(limit + 1), &value.value))) return false;
    const auto length = static_cast<std::size_t>(SysStringLen(value.value));
    if (length > limit) return false;
    text.assign(value.value == nullptr ? L"" : value.value, length);
    return text.find(L'\0') == std::wstring::npos;
}

[[nodiscard]] bool WritableRange(IUIAutomationTextRange* range) noexcept {
    VARIANT value;
    VariantInit(&value);
    const HRESULT status = range->GetAttributeValue(UIA_IsReadOnlyAttributeId, &value);
    const bool writable = SUCCEEDED(status) && value.vt == VT_BOOL && value.boolVal == VARIANT_FALSE;
    VariantClear(&value);
    return writable;
}

[[nodiscard]] bool EditableElement(IUIAutomationElement* element, DWORD expected_pid) noexcept {
    BOOL enabled = FALSE, password = TRUE, focused = FALSE;
    int process = 0;
    CONTROLTYPEID type = 0;
    return SUCCEEDED(element->get_CurrentProcessId(&process)) &&
        process > 0 && static_cast<DWORD>(process) == expected_pid &&
        SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled &&
        SUCCEEDED(element->get_CurrentIsPassword(&password)) && !password &&
        SUCCEEDED(element->get_CurrentHasKeyboardFocus(&focused)) && focused &&
        SUCCEEDED(element->get_CurrentControlType(&type)) &&
        (type == UIA_EditControlTypeId || type == UIA_DocumentControlTypeId);
}

[[nodiscard]] bool SameElement(const Guard& guard, IUIAutomation* automation,
                               IUIAutomationElement* element, DWORD pid) {
    if (!guard.Current()) return false;
    ComPtr<IUIAutomationElement> current;
    BOOL same = FALSE;
    return SUCCEEDED(automation->GetFocusedElement(&current)) && current &&
        SUCCEEDED(automation->CompareElements(element, current.Get(), &same)) && same &&
        EditableElement(current.Get(), pid) && guard.Current();
}

[[nodiscard]] bool SingleSelection(IUIAutomationTextPattern* pattern,
                                   ComPtr<IUIAutomationTextRange>& selected) noexcept {
    ComPtr<IUIAutomationTextRangeArray> selections;
    int count = 0;
    if (FAILED(pattern->GetSelection(&selections)) || !selections ||
        FAILED(selections->get_Length(&count)) || count != 1) return false;
    return SUCCEEDED(selections->GetElement(0, &selected)) && selected;
}

[[nodiscard]] bool Collapsed(IUIAutomationTextRange* range, bool& collapsed) noexcept {
    int comparison = 0;
    if (FAILED(range->CompareEndpoints(TextPatternRangeEndpoint_Start, range,
                                      TextPatternRangeEndpoint_End, &comparison))) return false;
    collapsed = comparison == 0;
    return true;
}

[[nodiscard]] bool CreateAutomation(ComPtr<IUIAutomation>& automation) noexcept {
    HRESULT status = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation));
    if (FAILED(status)) status = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                                 IID_PPV_ARGS(&automation));
    if (FAILED(status)) return false;
    ComPtr<IUIAutomation2> timed;
    return SUCCEEDED(automation.As(&timed)) && SUCCEEDED(timed->put_AutoSetFocus(FALSE)) &&
        SUCCEEDED(timed->put_ConnectionTimeout(CallTimeout)) &&
        SUCCEEDED(timed->put_TransactionTimeout(CallTimeout));
}

constexpr ULONG_PTR OwnInputTag = 0x4C534846UL;

[[nodiscard]] INPUT Key(WORD key, bool up = false) noexcept {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC) & 0xFF);
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (key == VK_LEFT || key == VK_RIGHT) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    input.ki.dwExtraInfo = OwnInputTag;
    return input;
}

void ReleaseOwnCopyKeys() noexcept {
    INPUT release[]{Key(VK_LEFT, true), Key('C', true), Key(VK_LSHIFT, true), Key(VK_LCONTROL, true)};
    (void)SendInput(static_cast<UINT>(std::size(release)), release, sizeof(INPUT));
}

[[nodiscard]] bool CopySelection() noexcept {
    INPUT inputs[]{Key(VK_LCONTROL), Key('C'), Key('C', true), Key(VK_LCONTROL, true)};
    constexpr UINT count = static_cast<UINT>(std::size(inputs));
    const UINT sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent != count && sent != 0) ReleaseOwnCopyKeys();
    return sent == count;
}

[[nodiscard]] bool WaitModifiers(const Guard& guard, IUIAutomation* automation,
                                 IUIAutomationElement* element, DWORD pid) {
    const auto until = std::min<std::uint64_t>(guard.request.deadline_ms, GetTickCount64() + 50);
    do {
        if (!SameElement(guard, automation, element, pid)) return false;
        if (ModifiersUp()) return true;
        Sleep(2);
    } while (GetTickCount64() < until);
    return false;
}

[[nodiscard]] TextResult SubmitEdit(const Guard& guard, HWND focus, Direction direction,
                                  const Layouts& layouts, std::size_t erase_count = 0,
                                  std::wstring_view replacement = {}) {
    std::vector<INPUT> keys;
    keys.reserve(erase_count * 2 + replacement.size() * 2 + 4);
    for (std::size_t index = 0; index < erase_count; ++index) {
        keys.push_back(Key(VK_BACK));
        keys.push_back(Key(VK_BACK, true));
    }
    if (erase_count != 0) {
        const HKL output = direction == Direction::ToRussian ? layouts.russian : layouts.english;
        bool shift_held = false;
        for (const wchar_t character : replacement) {
            const SHORT mapping = VkKeyScanExW(character, output);
            if (mapping == -1 || (HIBYTE(mapping) & ~1) != 0) {
                return Result(TextStatus::Unsupported, L"Клавиша для исправленного текста недоступна.");
            }
            const WORD key = LOBYTE(mapping);
            std::optional<bool> required_shift;
            for (const bool shifted : {false, true}) {
                BYTE state[256]{};
                state[VK_CAPITAL] = guard.request.recent_keys.caps_active ? 1 : 0;
                if (shifted) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
                wchar_t decoded[8]{};
                const int count = ToUnicodeEx(key, MapVirtualKeyExW(key, MAPVK_VK_TO_VSC, output),
                    state, decoded, static_cast<int>(std::size(decoded)), 4, output);
                if (count == 1 && decoded[0] == character) { required_shift = shifted; break; }
            }
            if (!required_shift) return Result(TextStatus::Unsupported, L"Регистр исправленного текста не подтверждён.");
            if (*required_shift != shift_held) {
                keys.push_back(Key(VK_LSHIFT, !*required_shift));
                shift_held = *required_shift;
            }
            keys.push_back(Key(key));
            keys.push_back(Key(key, true));
        }
        if (shift_held) keys.push_back(Key(VK_LSHIFT, true));
    } else {
        keys.push_back(Key(VK_LCONTROL));
        keys.push_back(Key('V'));
        keys.push_back(Key('V', true));
        keys.push_back(Key(VK_LCONTROL, true));
    }
    // Сначала подтверждается раскладка, затем обычные клавиши поступают одним пакетом.
    // Caps Lock не переключается: нужный Shift заранее проверен через ToUnicodeEx.
    if (erase_count != 0) {
        const HKL next = direction == Direction::ToRussian ? layouts.russian : layouts.english;
        if (!SwitchLayout(guard, focus, next) || !SameLayoutFocus(guard, focus) || !ModifiersUp()) {
            return Result(TextStatus::Stale, L"Условия ввода изменились до исправления слова.");
        }
    }
    const UINT sent = SendInput(static_cast<UINT>(keys.size()), keys.data(), sizeof(INPUT));
    if (sent != keys.size()) {
        if (sent != 0) {
            if (erase_count != 0) {
                INPUT release = keys[sent - 1];
                if ((release.ki.dwFlags & KEYEVENTF_KEYUP) == 0) {
                    release.ki.dwFlags |= KEYEVENTF_KEYUP;
                    (void)SendInput(1, &release, sizeof(INPUT));
                }
                // Наш Shift мог быть нажат для группы символов до места прерывания пакета.
                bool own_shift = false;
                for (UINT index = 0; index < sent; ++index) {
                    if (keys[index].ki.wVk == VK_LSHIFT) own_shift = (keys[index].ki.dwFlags & KEYEVENTF_KEYUP) == 0;
                }
                if (own_shift) {
                    INPUT release_shift = Key(VK_LSHIFT, true);
                    (void)SendInput(1, &release_shift, sizeof(INPUT));
                }
            } else {
                INPUT release[]{Key('V', true), Key(VK_LCONTROL, true)};
                (void)SendInput(static_cast<UINT>(std::size(release)), release, sizeof(INPUT));
            }
        }
        return Result(TextStatus::Partial, L"Система приняла не всю команду вставки.");
    }
    // Вставку уже нельзя отменить. Новый набор в том же окне не отменяет смену языка;
    // смена foreground, HWND фокуса, модификаторы и общий deadline всё ещё проверяются.
    const std::function<bool()> keep_layout = [] { return true; };
    const Guard layout_guard{guard.request, keep_layout};
    const auto until = std::min<std::uint64_t>(guard.request.deadline_ms, GetTickCount64() + 50);
    while (SameLayoutFocus(layout_guard, focus) && !ModifiersUp() && GetTickCount64() < until) Sleep(2);
    const HKL target = direction == Direction::ToRussian ? layouts.russian : layouts.english;
    if (!SwitchLayout(layout_guard, focus, target)) {
        return Result(TextStatus::Partial, L"Вставка отправлена; переключение языка не подтверждено.");
    }
    // SendInput подтверждает приём команды системой, но не изменение внутреннего текста редактора.
    return Result(TextStatus::Submitted, erase_count != 0 ? L"Команда исправления отправлена, язык переключён." :
                  L"Вставка отправлена, язык переключён.");
}

[[nodiscard]] bool DecodeRecent(const RecentKeys& recent, HKL layout, std::wstring& text) {
    if (!recent.complete || recent.count == 0 || recent.count > recent.keys.size()) return false;
    text.reserve(recent.count);
    for (std::size_t index = 0; index < recent.count; ++index) {
        const auto& key = recent.keys[index];
        if (key.key >= 256) return false;
        BYTE state[256]{};
        if (key.shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
        state[VK_CAPITAL] = key.caps ? 1 : 0;
        wchar_t decoded[8]{};
        const UINT scan = MapVirtualKeyExW(key.key, MAPVK_VK_TO_VSC, layout);
        // Флаг 4 не изменяет системное состояние dead keys; перевод только на worker по жесту.
        const int count = ToUnicodeEx(key.key, scan, state, decoded, static_cast<int>(std::size(decoded)), 4, layout);
        if (count != 1 || !((decoded[0] >= L' ' && decoded[0] <= L'~') ||
            (decoded[0] >= L'А' && decoded[0] <= L'я') || decoded[0] == L'ё' || decoded[0] == L'Ё')) return false;
        text.push_back(decoded[0]);
    }
    return true;
}

[[nodiscard]] TextResult ProcessRecent(const Guard& guard, HWND focus, const Layouts& layouts,
                                      std::wstring_view prefix, wchar_t following, const std::function<bool()>& confirm,
                                      bool& touched) {
    const auto word = LastWord(prefix, prefix.size());
    if (!word) return Toggle(guard, focus, layouts);
    const auto& recent = guard.request.recent_keys;
    if (!recent.complete || recent.count == 0) {
        TextResult toggled = Toggle(guard, focus, layouts);
        if (toggled.status == TextStatus::Switched) toggled.detail = L"Язык переключён; недавно набранное слово недоступно.";
        return toggled;
    }
    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(focus, nullptr));
    if (reinterpret_cast<std::uintptr_t>(layout) != guard.request.recent_layout) {
        return Result(TextStatus::Stale, L"Раскладка набранного слова изменилась.");
    }
    std::wstring typed;
    if (!DecodeRecent(recent, layout, typed)) {
        return Result(TextStatus::Unsupported, L"Последовательность набора не поддерживается.");
    }
    // После исправления HKL меняется только для текущего слова; предыдущие слова
    // остаются в прежней раскладке. Сверяем весь удаляемый фрагмент, включая хвост,
    // и требуем наличие начала целого слова в журнале, не переоценивая старые слова.
    const std::size_t erase_count = prefix.size() - word->start;
    if (erase_count > typed.size() ||
        std::wstring_view(typed).substr(typed.size() - erase_count) != prefix.substr(word->start)) {
        return Result(TextStatus::Stale, L"Набранное слово не совпало с текстом перед курсором.");
    }
    const auto original = prefix.substr(word->start, word->length);
    const auto direction = DetectDirection(original);
    if (!direction) return Toggle(guard, focus, layouts);
    // Парные символы могут удаляться редактором вместе с символом справа от курсора.
    // В таком контексте количество Backspace не доказывает безопасный диапазон удаления.
    constexpr std::wstring_view opening = L"([{\"'";
    constexpr std::wstring_view closing = L")]}\"'";
    const auto pair = closing.find(following);
    if (following != L'\0' && pair != std::wstring_view::npos) {
        const wchar_t opener = following == L')' ? L'(' : following == L']' ? L'[' :
            following == L'}' ? L'{' : following;
        if (opening.find(opener) != std::wstring_view::npos &&
            prefix.substr(word->start).find(opener) != std::wstring_view::npos) {
            return Result(TextStatus::Unsupported, L"Парные символы рядом с курсором: исправление отменено.");
        }
    }
    std::wstring replacement = Convert(original, *direction);
    replacement.append(prefix.substr(word->start + word->length));
    if (erase_count == 0 || erase_count > recent.keys.size() || !confirm() || !ModifiersUp()) {
        return Result(TextStatus::Stale, L"Текст или позиция ввода изменились.");
    }
    touched = true;
    return SubmitEdit(guard, focus, *direction, layouts, erase_count, replacement);
}

[[nodiscard]] TextResult ProcessAutomation(const Guard& guard, const Layouts& layouts,
                                           bool& touched) {
    if (!guard.Current()) return Result(TextStatus::Stale, L"Команда устарела.");
    ComPtr<IUIAutomation> automation;
    if (!CreateAutomation(automation)) return Result(TextStatus::Unsupported, L"UI Automation недоступен.");
    DWORD pid = 0;
    if (GetWindowThreadProcessId(guard.request.foreground, &pid) == 0 || pid == 0) {
        return Result(TextStatus::Stale, L"Целевое окно недоступно.");
    }
    ComPtr<IUIAutomationElement> element;
    const auto focus_until = std::min<std::uint64_t>(guard.request.deadline_ms, GetTickCount64() + 300);
    for (;;) {
        element.Reset();
        if (!guard.Current()) return Result(TextStatus::Stale, L"Команда устарела.");
        if (SUCCEEDED(automation->GetFocusedElement(&element)) && element && EditableElement(element.Get(), pid)) break;
        if (GetTickCount64() >= focus_until) {
            return Result(TextStatus::Unsupported, L"Фокус не подтверждён как текстовое поле.");
        }
        Sleep(15);
    }
    ComPtr<IUIAutomationTextPattern> pattern;
    ComPtr<IUIAutomationTextRange> document, selection;
    if (FAILED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) || !pattern ||
        FAILED(pattern->get_DocumentRange(&document)) || !document || !WritableRange(document.Get())) {
        return Result(TextStatus::Unsupported, L"Редактируемость текста не подтверждена.");
    }
    bool collapsed = false;
    if (!SingleSelection(pattern.Get(), selection) || !Collapsed(selection.Get(), collapsed)) {
        return Result(TextStatus::Unsupported, L"Выделение или позиция ввода недоступны.");
    }
    const bool own_selection = collapsed;
    const HWND native_focus = FocusedWindow(guard);
    const HWND focus = native_focus != nullptr ? native_focus : guard.request.foreground;
    if (own_selection) {
        // GetSelection может актуализировать accessibility-дерево после набора.
        // Берём границы поля заново, чтобы не сравнивать новый курсор со старым диапазоном.
        document.Reset();
        if (FAILED(pattern->get_DocumentRange(&document)) || !document || !WritableRange(document.Get())) {
            return Result(TextStatus::Unsupported, L"Границы редактируемого поля недоступны.");
        }
        AutomationContext context;
        if (!ReadAutomationContext(document.Get(), selection.Get(), context)) {
            return {TextStatus::Unsupported, L"Контекст поля недоступен: этап " +
                std::to_wstring(static_cast<int>(context.stage)) + L", код " +
                std::to_wstring(context.status) + L"."};
        }
        const auto confirm = [&] {
            ComPtr<IUIAutomationTextRange> current, current_document;
            bool is_collapsed = false;
            AutomationContext after;
            return SameElement(guard, automation.Get(), element.Get(), pid) &&
                SingleSelection(pattern.Get(), current) && Collapsed(current.Get(), is_collapsed) &&
                is_collapsed && SUCCEEDED(pattern->get_DocumentRange(&current_document)) &&
                ReadAutomationContext(current_document.Get(), current.Get(), after) &&
                after.prefix == context.prefix && after.following == context.following;
        };
        return ProcessRecent(guard, focus, layouts, context.prefix, context.following, confirm, touched);
    }
    if (!SameElement(guard, automation.Get(), element.Get(), pid) || !ModifiersUp()) {
        return Result(TextStatus::Stale, L"Условия копирования изменились.");
    }
    if (!WriteClipboardText(guard.request.clipboard_owner, L"")) {
        return Result(TextStatus::Failed, L"Не удалось подготовить буфер обмена; текст не изменён.");
    }
    const DWORD sentinel_sequence = GetClipboardSequenceNumber();
    if (!SameElement(guard, automation.Get(), element.Get(), pid) || !ModifiersUp()) {
        return Result(TextStatus::Stale, L"Условия копирования изменились.");
    }
    touched = true;
    if (!CopySelection()) return Result(TextStatus::Partial, L"Система приняла не всю команду копирования.");
    std::wstring original;
    bool copied = false;
    const auto copy_until = std::min<std::uint64_t>(guard.request.deadline_ms, GetTickCount64() + 300);
    do {
        if (!SameElement(guard, automation.Get(), element.Get(), pid)) {
            return Result(TextStatus::Partial, L"Фокус изменился после копирования; дальнейшие действия отменены.");
        }
        // Владение target PID и новая sequence подтверждают обработку Ctrl+C целевым процессом.
        if (ReadCopiedText(guard.request.clipboard_owner, pid, sentinel_sequence, original)) { copied = true; break; }
        Sleep(5);
    } while (GetTickCount64() < copy_until);
    if (!copied) {
        // Отсутствие ACK после известного слова — ошибка копирования, а не команда смены языка.
        return Result(TextStatus::Partial, L"Копирование не подтверждено; вставка и смена языка не отправлены.");
    }
    const auto conversion = ConvertWords(original);
    const auto direction = conversion.last_direction;
    if (!direction) return Toggle(guard, focus, layouts);
    const std::wstring& replacement = conversion.text;
    if (replacement == original) return Toggle(guard, focus, layouts);
    if (replacement.empty() || replacement.size() > ReplaceLimit) {
        return Result(TextStatus::Partial, L"Скопированный фрагмент превышает допустимый размер.");
    }
    if (!WaitModifiers(guard, automation.Get(), element.Get(), pid) ||
        !WriteClipboardText(guard.request.clipboard_owner, replacement)) {
        return Result(TextStatus::Partial, L"Не удалось подготовить вставку; текст не изменён.");
    }
    const DWORD paste_sequence = GetClipboardSequenceNumber();
    if (!SameElement(guard, automation.Get(), element.Get(), pid) || !ModifiersUp() ||
        GetClipboardOwner() != guard.request.clipboard_owner || GetClipboardSequenceNumber() != paste_sequence) {
        return Result(TextStatus::Partial, L"Условия вставки изменились; текст не изменён.");
    }
    return SubmitEdit(guard, focus, *direction, layouts);
}
} // namespace

TextResult ProcessText(const TextRequest& request, const std::function<bool()>& is_current) {
    bool touched = false;
    try {
        const Guard guard{request, is_current};
        if (!guard.Current()) return Result(TextStatus::Stale, L"Команда устарела.");
        const Layouts layouts = InstalledLayouts();
        if (layouts.english == nullptr || layouts.russian == nullptr) {
            return Result(TextStatus::Unsupported, L"Нужны стандартные раскладки English US и Русская.");
        }
        const HWND focus = FocusedWindow(guard);
        const HWND layout_window = focus != nullptr ? focus : request.foreground;
        const DWORD thread = GetWindowThreadProcessId(layout_window, nullptr);
        const HKL current = thread != 0 ? GetKeyboardLayout(thread) : nullptr;
        if (current != layouts.english && current != layouts.russian) {
            return Result(TextStatus::Unsupported, L"Активна неподдерживаемая раскладка.");
        }
        if (focus != nullptr && IsNativeClass(focus)) {
            TextResult native = ProcessNative(guard, focus, layouts, touched);
            if (native.status != TextStatus::Unsupported || touched ||
                !SameNativeFocus(guard, focus) || !NativeEditable(focus)) return native;
            // Неподтверждённые native-смещения допускают проверку UIA, запрет редактирования — нет.
        }
        return ProcessAutomation(guard, layouts, touched);
    } catch (...) {
        return Result(touched ? TextStatus::Partial : TextStatus::Failed,
                      L"Операция прервана; результат не подтверждён.");
    }
}
} // namespace lshift
