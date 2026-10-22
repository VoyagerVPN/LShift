#include "text_service.hpp"
#include "recent_input.hpp"

#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t FixtureTitle[] = L"LShift Automation Fixture";
constexpr wchar_t OwnerClass[] = L"LShift.AutomationProbe.ClipboardOwner";

struct Search {
    HWND window = nullptr;
    unsigned matches = 0;
};

[[nodiscard]] BOOL CALLBACK FindFixture(HWND window, LPARAM parameter) {
    wchar_t title[256]{};
    if (GetWindowTextW(window, title, static_cast<int>(std::size(title))) > 0 &&
        std::wstring_view(title) == FixtureTitle && IsWindowVisible(window)) {
        auto& search = *reinterpret_cast<Search*>(parameter);
        search.window = window;
        ++search.matches;
    }
    return TRUE;
}

[[nodiscard]] LRESULT CALLBACK OwnerProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

class ClipboardOwner {
public:
    ClipboardOwner() {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpfnWndProc = OwnerProcedure;
        type.lpszClassName = OwnerClass;
        if (RegisterClassW(&type) == 0) throw std::runtime_error("register hidden clipboard owner");
        window_ = CreateWindowExW(0, OwnerClass, L"LShift Automation Probe", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
        if (window_ == nullptr) {
            UnregisterClassW(OwnerClass, type.hInstance);
            throw std::runtime_error("create hidden clipboard owner");
        }
    }
    ~ClipboardOwner() {
        DestroyWindow(window_);
        UnregisterClassW(OwnerClass, GetModuleHandleW(nullptr));
    }
    ClipboardOwner(const ClipboardOwner&) = delete;
    ClipboardOwner& operator=(const ClipboardOwner&) = delete;
    [[nodiscard]] HWND Window() const noexcept { return window_; }
private:
    HWND window_ = nullptr;
};

[[nodiscard]] DWORD TargetLayout(HWND window) noexcept {
    const DWORD thread = GetWindowThreadProcessId(window, nullptr);
    GUITHREADINFO info{sizeof(info)};
    const HWND focus = thread != 0 && GetGUIThreadInfo(thread, &info) && info.hwndFocus
        ? info.hwndFocus : window;
    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(focus, nullptr));
    return static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(layout) & 0xFFFFFFFFULL);
}

void PrepareLayout(HWND window, DWORD expected_pid, std::uintptr_t requested) {
    if (requested != 0x04090409UL && requested != 0x04190419UL) throw std::runtime_error("only installed standard US/RU fixture layouts are allowed");
    DWORD actual_pid = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &actual_pid);
    GUITHREADINFO info{sizeof(info)};
    if (GetForegroundWindow() != window || actual_pid != expected_pid || thread == 0 ||
        !GetGUIThreadInfo(thread, &info) || !info.hwndFocus || GetAncestor(info.hwndFocus, GA_ROOT) != window) {
        throw std::runtime_error("own fixture focus is unavailable for layout preparation");
    }
    DWORD focus_pid = 0;
    const DWORD focus_thread = GetWindowThreadProcessId(info.hwndFocus, &focus_pid);
    if (focus_pid != expected_pid || focus_thread == 0) throw std::runtime_error("own fixture focus PID mismatch");
    HKL installed[128]{};
    const int count = GetKeyboardLayoutList(static_cast<int>(std::size(installed)), installed);
    HKL target = nullptr;
    for (int index = 0; index < count; ++index) {
        if ((reinterpret_cast<ULONG_PTR>(installed[index]) & 0xFFFFFFFFULL) == requested) target = installed[index];
    }
    if (target == nullptr) throw std::runtime_error("requested fixture layout is not installed");
    if (!PostMessageW(info.hwndFocus, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(target))) {
        throw std::runtime_error("own fixture layout request failed");
    }
    const auto until = GetTickCount64() + 300;
    do {
        GUITHREADINFO current{sizeof(current)};
        if (GetForegroundWindow() != window || !GetGUIThreadInfo(thread, &current) || current.hwndFocus != info.hwndFocus) {
            throw std::runtime_error("own fixture focus changed during layout preparation");
        }
        if (GetKeyboardLayout(focus_thread) == target) return;
        Sleep(5);
    } while (GetTickCount64() < until);
    throw std::runtime_error("own fixture layout preparation was not confirmed");
}

template<class Request>
void SetClipboardOwner(Request& request, HWND owner) {
    // Старый baseline-код сравнивается тем же probe; новое поле добавляется без смены ABI функции.
    if constexpr (requires { request.clipboard_owner = owner; }) request.clipboard_owner = owner;
}

[[nodiscard]] std::string JsonString(std::wstring_view value) {
    std::string utf8;
    if (!value.empty()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "\"encoding-error\"";
        utf8.resize(static_cast<std::size_t>(size));
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                             utf8.data(), size, nullptr, nullptr);
    }
    std::string escaped = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char byte : utf8) {
        if (byte == '"' || byte == '\\') { escaped += '\\'; escaped += static_cast<char>(byte); }
        else if (byte < 32) {
            escaped += "\\u00";
            escaped += hex[byte >> 4];
            escaped += hex[byte & 15];
        } else escaped += static_cast<char>(byte);
    }
    escaped += '"';
    return escaped;
}

[[nodiscard]] const char* StatusName(lshift::TextStatus status) noexcept {
    switch (status) {
    case lshift::TextStatus::Converted: return "Converted";
    case lshift::TextStatus::Switched: return "Switched";
    case lshift::TextStatus::Unsupported: return "Unsupported";
    case lshift::TextStatus::Stale: return "Stale";
    case lshift::TextStatus::Failed: return "Failed";
    case lshift::TextStatus::Partial: return "Partial";
    case lshift::TextStatus::Submitted: return "Submitted";
    }
    return "Unknown";
}

[[nodiscard]] std::string TextStatistics(IUIAutomationTextRange* range) {
    constexpr int limit = 64 * 1024;
    BSTR value = nullptr;
    const HRESULT status = range != nullptr ? range->GetText(limit + 1, &value) : E_NOINTERFACE;
    const UINT length = SUCCEEDED(status) ? SysStringLen(value) : 0;
    UINT latin = 0, russian = 0;
    if (SUCCEEDED(status)) {
        for (UINT index = 0; index < length; ++index) {
            const wchar_t character = value[index];
            if ((character >= L'a' && character <= L'z') || (character >= L'A' && character <= L'Z')) ++latin;
            if ((character >= L'а' && character <= L'я') || (character >= L'А' && character <= L'Я') ||
                character == L'ё' || character == L'Ё') ++russian;
        }
    }
    SysFreeString(value);
    std::ostringstream json;
    // Только агрегаты собственной fixture; сам текст никогда не попадает в отчёт.
    json << "{\"get_text_hresult\":" << status << ",\"len\":";
    if (SUCCEEDED(status)) json << length; else json << "null";
    json << ",\"latinChars\":" << latin << ",\"russianChars\":" << russian
         << ",\"truncated\":" << (length > static_cast<UINT>(limit) ? "true" : "false") << '}';
    return json.str();
}

[[nodiscard]] std::string Capabilities(HWND foreground) {
    // Диагностика выполняется только после ProcessText, чтобы не прогревать UIA до проверки.
    ComPtr<IUIAutomation> automation;
    HRESULT created = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(created)) created = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    ComPtr<IUIAutomation2> bounded;
    if (automation && SUCCEEDED(automation.As(&bounded))) {
        bounded->put_ConnectionTimeout(200);
        bounded->put_TransactionTimeout(200);
    }
    ComPtr<IUIAutomationElement> focused;
    const HRESULT focus_hr = automation ? automation->GetFocusedElement(&focused) : created;
    CONTROLTYPEID type = 0;
    int pid = 0;
    BOOL keyboard_focus = FALSE, enabled = FALSE, password = FALSE;
    UIA_HWND native = nullptr;
    DWORD foreground_pid = 0;
    GetWindowThreadProcessId(foreground, &foreground_pid);
    ComPtr<IUIAutomationTextPattern> text;
    ComPtr<IUIAutomationTextPattern2> text2;
    int selection_count = -1, endpoint_comparison = -1;
    HRESULT selection_hr = E_NOINTERFACE, caret_hr = E_NOINTERFACE;
    std::string document_statistics = "null", selection_statistics = "null";
    BOOL active = FALSE;
    if (focused) {
        focused->get_CurrentControlType(&type);
        focused->get_CurrentProcessId(&pid);
        focused->get_CurrentHasKeyboardFocus(&keyboard_focus);
        focused->get_CurrentIsEnabled(&enabled);
        focused->get_CurrentIsPassword(&password);
        focused->get_CurrentNativeWindowHandle(&native);
        focused->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&text));
        focused->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&text2));
    }
    if (text) {
        const bool own_text = GetForegroundWindow() == foreground && pid > 0 &&
            static_cast<DWORD>(pid) == foreground_pid && keyboard_focus && !password;
        if (own_text) {
            ComPtr<IUIAutomationTextRange> document;
            if (SUCCEEDED(text->get_DocumentRange(&document))) document_statistics = TextStatistics(document.Get());
        }
        ComPtr<IUIAutomationTextRangeArray> selections;
        selection_hr = text->GetSelection(&selections);
        if (SUCCEEDED(selection_hr) && selections) {
            selections->get_Length(&selection_count);
            if (selection_count == 1) {
                ComPtr<IUIAutomationTextRange> selected;
                if (SUCCEEDED(selections->GetElement(0, &selected)) && selected) {
                    selected->CompareEndpoints(TextPatternRangeEndpoint_Start, selected.Get(),
                        TextPatternRangeEndpoint_End, &endpoint_comparison);
                    if (own_text) selection_statistics = TextStatistics(selected.Get());
                }
            }
        }
    }
    if (text2) {
        ComPtr<IUIAutomationTextRange> caret;
        caret_hr = text2->GetCaretRange(&active, &caret);
    }
    std::ostringstream json;
    json << "{\"focus_hresult\":" << focus_hr << ",\"control_type\":" << type << ",\"pid_matches\":"
         << (pid > 0 && static_cast<DWORD>(pid) == foreground_pid ? "true" : "false")
         << ",\"has_keyboard_focus\":" << (keyboard_focus ? "true" : "false")
         << ",\"enabled\":" << (enabled ? "true" : "false") << ",\"password\":" << (password ? "true" : "false")
         << ",\"native_hwnd\":" << reinterpret_cast<std::uintptr_t>(native)
         << ",\"text_pattern\":" << (text ? "true" : "false") << ",\"text_pattern2\":" << (text2 ? "true" : "false")
         << ",\"selection_hresult\":" << selection_hr << ",\"selection_count\":" << selection_count
         << ",\"selection_collapsed\":" << (selection_count == 1 && endpoint_comparison == 0 ? "true" : "false")
         << ",\"caret_hresult\":" << caret_hr << ",\"caret_active\":" << (active ? "true" : "false")
         << ",\"document_text\":" << document_statistics << ",\"selection_text\":" << selection_statistics << '}';
    return json.str();
}

[[nodiscard]] double Number(const wchar_t* value) {
    wchar_t* end = nullptr;
    const double number = std::wcstod(value, &end);
    if (end == value || *end != L'\0' || !std::isfinite(number)) throw std::runtime_error("invalid numeric option");
    return number;
}

void FeedRecordedEvents(lshift::RecentTyping& recent, const wchar_t* serialized) {
    const wchar_t* cursor = serialized;
    unsigned count = 0;
    while (*cursor != L'\0') {
        if (++count > 1024 || *cursor < L'0' || *cursor > L'9') throw std::runtime_error("invalid recent key stream");
        wchar_t* end = nullptr;
        const unsigned long key = std::wcstoul(cursor, &end, 10);
        if (key > 255 || *end != L':' || (end[1] != L'0' && end[1] != L'1')) {
            throw std::runtime_error("recent stream must contain decimal-VK:0-or-1");
        }
        recent.Feed(static_cast<std::uint16_t>(key), end[1] == L'1');
        cursor = end + 2;
        if (*cursor == L'\0') break;
        if (*cursor++ != L',' || *cursor == L'\0') throw std::runtime_error("invalid recent key separator");
    }
}

[[nodiscard]] std::uintptr_t RecordedLayout(const wchar_t* value) {
    wchar_t* end = nullptr;
    errno = 0;
    const auto layout = std::wcstoull(value, &end, 0);
    if (end == value || *end != L'\0' || errno == ERANGE || layout == 0 ||
        layout > std::numeric_limits<std::uintptr_t>::max()) throw std::runtime_error("invalid recent layout");
    return static_cast<std::uintptr_t>(layout);
}

[[nodiscard]] std::runtime_error HitFailure(HWND window, POINT point, const RECT& client) {
    const HWND hit = WindowFromPoint(point);
    const HWND root = GetAncestor(hit, GA_ROOT);
    DWORD hit_pid = 0;
    GetWindowThreadProcessId(hit, &hit_pid);
    wchar_t hit_class[128]{};
    GetClassNameW(hit, hit_class, static_cast<int>(std::size(hit_class)));
    RECT outer{}, hit_rectangle{};
    GetWindowRect(window, &outer);
    GetWindowRect(root, &hit_rectangle);
    std::ostringstream message;
    message << "own field is obscured; no click performed; hit_pid=" << hit_pid
            << " hit_class=" << JsonString(hit_class) << " point=" << point.x << ',' << point.y
            << " own_rect=" << outer.left << ',' << outer.top << ',' << outer.right << ',' << outer.bottom
            << " client=" << client.right << ',' << client.bottom
            << " hit_root_rect=" << hit_rectangle.left << ',' << hit_rectangle.top << ','
            << hit_rectangle.right << ',' << hit_rectangle.bottom;
    return std::runtime_error(message.str());
}

class OwnTopmost {
public:
    explicit OwnTopmost(HWND window) : window_(window), was_topmost_((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        if (!SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
            throw std::runtime_error("could not temporarily raise own fixture");
        }
    }
    ~OwnTopmost() {
        if (!was_topmost_) SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    OwnTopmost(const OwnTopmost&) = delete;
    OwnTopmost& operator=(const OwnTopmost&) = delete;
private:
    HWND window_;
    bool was_topmost_;
};

void ActivateOwnFixture(HWND window, double viewport_width, double viewport_height, double x, double y) {
    if (viewport_width <= 0 || viewport_height <= 0 || x < 0 || y < 0 ||
        x >= viewport_width || y >= viewport_height) throw std::runtime_error("invalid own field geometry");
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0) throw std::runtime_error("mouse button already held");
    ShowWindow(window, SW_RESTORE);
    const OwnTopmost topmost(window);
    Sleep(40);
    RECT client{};
    if (!GetClientRect(window, &client)) throw std::runtime_error("own client rectangle unavailable");
    const double scale = static_cast<double>(client.right) / viewport_width;
    const double top = static_cast<double>(client.bottom) - viewport_height * scale;
    if (scale <= 0 || top < -2 || top > 150 * scale) throw std::runtime_error("own viewport geometry mismatch");
    POINT point{static_cast<LONG>(std::lround(x * scale)), static_cast<LONG>(std::lround(top + y * scale))};
    if (!PtInRect(&client, point) || !ClientToScreen(window, &point) ||
        GetAncestor(WindowFromPoint(point), GA_ROOT) != window) {
        throw HitFailure(window, point, client);
    }
    POINT original{};
    if (!GetCursorPos(&original) || !SetCursorPos(point.x, point.y)) throw std::runtime_error("own click positioning failed");
    if (GetAncestor(WindowFromPoint(point), GA_ROOT) != window) {
        SetCursorPos(original.x, original.y);
        throw HitFailure(window, point, client);
    }
    INPUT click[2]{};
    click[0].type = INPUT_MOUSE; click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].type = INPUT_MOUSE; click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(2, click, sizeof(INPUT));
    if (sent == 1) SendInput(1, &click[1], sizeof(INPUT));
    const auto until = GetTickCount64() + 800;
    while (GetForegroundWindow() != window && GetTickCount64() < until) Sleep(10);
    POINT current{};
    if (GetCursorPos(&current) && current.x == point.x && current.y == point.y) SetCursorPos(original.x, original.y);
    if (sent != 2 || GetForegroundWindow() != window) throw std::runtime_error("own fixture click did not activate foreground");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::uint64_t timeout = 4000;
        DWORD fixture_pid = 0;
        bool activate = false;
        bool diagnostic_capabilities = false;
        lshift::RecentTyping recent;
        recent.Reset();
        std::uintptr_t recent_layout = 0;
        std::uintptr_t set_layout = 0;
        double viewport_width = 0, viewport_height = 0, x = -1, y = -1;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--title" && index + 1 < argc) {
                if (std::wstring_view(argv[++index]) != FixtureTitle) {
                    throw std::runtime_error("only the fixed own-fixture title is allowed");
                }
            } else if (argument == L"--deadline-ms" && index + 1 < argc) {
                const std::wstring_view value(argv[++index]);
                timeout = 0;
                for (const wchar_t digit : value) {
                    if (digit < L'0' || digit > L'9') throw std::runtime_error("invalid deadline");
                    timeout = timeout * 10 + static_cast<std::uint64_t>(digit - L'0');
                    if (timeout > 8000) throw std::runtime_error("deadline must not exceed 8000ms");
                }
                if (timeout < 500) throw std::runtime_error("deadline must be at least 500ms");
            } else if (argument == L"--activate-own-fixture") activate = true;
            else if (argument == L"--diagnostic-capabilities") diagnostic_capabilities = true;
            else if (argument == L"--recent-events" && index + 1 < argc) FeedRecordedEvents(recent, argv[++index]);
            else if (argument == L"--recent-layout" && index + 1 < argc) recent_layout = RecordedLayout(argv[++index]);
            else if (argument == L"--set-layout" && index + 1 < argc) set_layout = RecordedLayout(argv[++index]);
            else if (argument == L"--fixture-pid" && index + 1 < argc) {
                const double pid = Number(argv[++index]);
                if (pid < 1 || pid > MAXDWORD || std::floor(pid) != pid) throw std::runtime_error("invalid fixture PID");
                fixture_pid = static_cast<DWORD>(pid);
            } else if (argument == L"--viewport-width" && index + 1 < argc) viewport_width = Number(argv[++index]);
            else if (argument == L"--viewport-height" && index + 1 < argc) viewport_height = Number(argv[++index]);
            else if (argument == L"--point-x" && index + 1 < argc) x = Number(argv[++index]);
            else if (argument == L"--point-y" && index + 1 < argc) y = Number(argv[++index]);
            else throw std::runtime_error("invalid automation probe option");
        }
        const BOOL dpi = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!dpi && !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                                                  DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
            throw std::runtime_error("PerMonitorV2 initialization failed");
        }
        Search search;
        EnumWindows(FindFixture, reinterpret_cast<LPARAM>(&search));
        DWORD actual_pid = 0;
        GetWindowThreadProcessId(search.window, &actual_pid);
        if (search.matches != 1 || fixture_pid == 0 || actual_pid != fixture_pid) {
            throw std::runtime_error("unique fixed-title fixture does not match dedicated browser PID");
        }
        if (activate) {
            ActivateOwnFixture(search.window, viewport_width, viewport_height, x, y);
            std::cout << "{\"status\":\"Activated\",\"foreground_ok\":true}\n";
            return 0;
        }
        if (GetForegroundWindow() != search.window) {
            throw std::runtime_error("unique own fixture must already be foreground; no foreign window was focused");
        }
        if (set_layout != 0) {
            PrepareLayout(search.window, fixture_pid, set_layout);
            std::cout << "{\"status\":\"Prepared\",\"layout_after\":" << TargetLayout(search.window) << "}\n";
            return 0;
        }
        ClipboardOwner owner;
        const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (done == nullptr) throw std::runtime_error("create worker completion event");
        lshift::TextRequest request{search.window, GetTickCount64() + timeout};
        SetClipboardOwner(request, owner.Window());
        request.recent_keys = recent.Snapshot();
        // CDP задаёт регистр исходного текста явно; глобальный Caps для замены берём отдельно.
        request.recent_keys.caps_active = (GetKeyState(VK_CAPITAL) & 1) != 0;
        request.recent_layout = recent_layout != 0 ? recent_layout : static_cast<std::uintptr_t>(TargetLayout(search.window));
        lshift::TextResult result{lshift::TextStatus::Failed, L"COM MTA не инициализирован."};
        HRESULT com_status = E_FAIL;
        std::string capabilities = "null";
        std::uint64_t elapsed = 0;
        const DWORD layout_before = TargetLayout(search.window);
        const auto started = GetTickCount64();
        std::thread worker([&] {
            com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(com_status)) {
                result = lshift::ProcessText(request, [&] { return GetForegroundWindow() == search.window; });
                elapsed = GetTickCount64() - started;
                // Избыточные межпроцессные UIA-запросы после отправки событий могут
                // задерживать их обработку. В обычной проверке результат читает только DOM oracle.
                if (diagnostic_capabilities) capabilities = Capabilities(search.window);
                CoUninitialize();
            }
            SetEvent(done);
        });
        bool complete = false;
        while (GetTickCount64() - started < timeout + 5000) {
            const DWORD state = MsgWaitForMultipleObjectsEx(1, &done, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (state == WAIT_OBJECT_0) { complete = true; break; }
            if (state == WAIT_FAILED) break;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        if (!complete) {
            std::cout << "{\"status\":\"HarnessTimeout\",\"detail\":\"worker did not finish\"}\n" << std::flush;
            // Завершается только этот тестовый процесс; HWND браузера ему не принадлежат.
            ExitProcess(3);
        }
        worker.join();
        CloseHandle(done);
        std::cout << "{\"status\":\"" << StatusName(result.status) << "\",\"detail\":"
                  << JsonString(result.detail) << ",\"elapsed_ms\":" << elapsed
                  << ",\"com_hresult\":" << com_status << ",\"foreground_ok\":"
                  << (GetForegroundWindow() == search.window ? "true" : "false")
                  << ",\"layout_before\":" << layout_before
                  << ",\"layout_after\":" << TargetLayout(search.window)
                  << ",\"recent_count\":" << request.recent_keys.count
                  << ",\"recent_complete\":" << (request.recent_keys.complete ? "true" : "false")
                  << ",\"capabilities_after\":" << capabilities << "}\n";
        return 0;
    } catch (const std::exception& error) {
        // Ошибки harness содержат только фиксированные сообщения, без текста редактора.
        std::cerr << "automation probe: " << error.what() << '\n';
        return 2;
    }
}
