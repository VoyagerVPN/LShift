#include "text_service.hpp"

#include <objbase.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <string_view>
#include <optional>
#include <atomic>
#include <commctrl.h>

namespace {
constexpr int Unavailable = 77;
constexpr wchar_t WindowClass[] = L"LShift.NativeTests.Isolated";

[[nodiscard]] std::wstring DesktopName(HDESK desktop) {
    wchar_t name[256]{};
    DWORD needed = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name,
                                   static_cast<DWORD>(sizeof(name)), &needed)) {
        throw std::runtime_error("GetUserObjectInformationW failed");
    }
    return name;
}

[[nodiscard]] std::wstring InputDesktopName() {
    const HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (input == nullptr) throw std::runtime_error("OpenInputDesktop failed");
    try {
        const auto name = DesktopName(input);
        CloseDesktop(input);
        return name;
    } catch (...) {
        CloseDesktop(input);
        throw;
    }
}

class PrivateDesktop {
public:
    explicit PrivateDesktop(bool own_window = false) : original_(GetThreadDesktop(GetCurrentThreadId())),
                       input_name_(InputDesktopName()) {
        if(own_window) { desktop_ = original_; owns_ = false; return; }
        const std::wstring name = L"LShift.NativeTests." +
            std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        // Права переключения desktop намеренно не запрашиваются.
        desktop_ = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
            DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
        if (desktop_ == nullptr) throw std::runtime_error("CreateDesktopW failed");
        if (!SetThreadDesktop(desktop_)) {
            CloseDesktop(desktop_);
            desktop_ = nullptr;
            throw std::runtime_error("SetThreadDesktop failed before window creation");
        }
    }
    PrivateDesktop(const PrivateDesktop&) = delete;
    PrivateDesktop& operator=(const PrivateDesktop&) = delete;
    ~PrivateDesktop() {
        if (desktop_ != nullptr && owns_) {
            SetThreadDesktop(original_);
            CloseDesktop(desktop_);
        }
    }
    [[nodiscard]] HDESK Get() const noexcept { return desktop_; }
    [[nodiscard]] bool InputUnchanged() const { return InputDesktopName() == input_name_; }
private:
    HDESK original_ = nullptr;
    HDESK desktop_ = nullptr;
    bool owns_ = true;
    std::wstring input_name_;
};

[[nodiscard]] LRESULT CALLBACK HostProcedure(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

class Fixture {
public:
    explicit Fixture(bool own_window = false) : own_window_(own_window),
        original_layout_(GetKeyboardLayout(GetCurrentThreadId())) {
        WNDCLASSW type{};
        type.lpfnWndProc = HostProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = WindowClass;
        if (RegisterClassW(&type) == 0) throw std::runtime_error("RegisterClassW failed");
        host_ = CreateWindowExW(0, WindowClass, L"LShift — временная проверка собственного поля",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 500, 180, nullptr, nullptr, type.hInstance, nullptr);
        if (host_ == nullptr) {
            UnregisterClassW(WindowClass, type.hInstance);
            throw std::runtime_error("CreateWindowExW host failed");
        }
        // В обычном режиме окно находится на неактивном тестовом desktop.
        ShowWindow(host_, SW_SHOWNOACTIVATE);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    ~Fixture() {
        if(own_window_ && edit_) {
            SendMessageW(edit_, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(original_layout_));
        }
        if (host_ != nullptr) DestroyWindow(host_);
        UnregisterClassW(WindowClass, GetModuleHandleW(nullptr));
    }
    void Field(DWORD extra_style, const wchar_t* text, std::size_t start, std::size_t end) {
        if (edit_ != nullptr) DestroyWindow(edit_);
        edit_ = CreateWindowExW(0, L"Edit", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style,
            10, 10, 450, 40, host_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (edit_ == nullptr) throw std::runtime_error("CreateWindowExW Edit failed");
        SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
        SetFocus(edit_);
    }
    [[nodiscard]] bool EstablishForeground() const {
        SetActiveWindow(host_);
        (void)SetForegroundWindow(host_);
        SetFocus(edit_);
        if(FocusMatches()) return true;
        if(!own_window_) return false;
        // Только явный --own-window. Клик проверяется по реальному hit-test собственного HWND.
        for(int key = 1; key < 256; ++key)
            if((GetAsyncKeyState(key) & 0x8000) != 0) return false;
        const bool was_topmost = (GetWindowLongPtrW(host_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        if(!SetWindowPos(host_, HWND_TOPMOST, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) return false;
        struct RestoreTopmost {
            HWND window;
            bool previous;
            ~RestoreTopmost() {
                if(!previous) SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0,
                                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        } topmost{host_, was_topmost};
        ShowWindow(host_, SW_RESTORE);
        Sleep(40);
        RECT client{};
        POINT point{20, 75};
        if(!GetClientRect(host_, &client) || !PtInRect(&client, point) || !ClientToScreen(host_, &point)) return false;
        const auto own_hit = [&] {
            const HWND hit = WindowFromPoint(point);
            DWORD pid = 0;
            return hit && GetWindowThreadProcessId(hit, &pid) != 0 &&
                   pid == GetCurrentProcessId() && GetAncestor(hit, GA_ROOT) == host_;
        };
        if(!own_hit()) return false;
        POINT original{};
        if(!GetCursorPos(&original) || !SetCursorPos(point.x, point.y)) return false;
        struct RestoreCursor {
            POINT original;
            POINT clicked;
            ~RestoreCursor() {
                POINT current{};
                if(GetCursorPos(&current) && current.x == clicked.x && current.y == clicked.y)
                    SetCursorPos(original.x, original.y);
            }
        } cursor{original, point};
        if(!own_hit()) return false;
        INPUT click[2]{};
        click[0].type = INPUT_MOUSE; click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].type = INPUT_MOUSE; click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        const UINT sent = SendInput(2, click, sizeof(INPUT));
        if(sent == 1) (void)SendInput(1, &click[1], sizeof(INPUT));
        const auto until = GetTickCount64() + 800;
        while(GetForegroundWindow() != host_ && GetTickCount64() < until) {
            MSG message{};
            while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(5);
        }
        SetFocus(edit_);
        return sent == 2 && FocusMatches();
    }
    [[nodiscard]] bool SourceLayout(HKL layout) const {
        return ActivateKeyboardLayout(layout, 0) != nullptr &&
               GetKeyboardLayout(GetCurrentThreadId()) == layout;
    }
    [[nodiscard]] bool FocusMatches() const {
        GUITHREADINFO info{};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        return GetForegroundWindow() == host_ &&
            GetGUIThreadInfo(GetCurrentThreadId(), &info) && info.hwndFocus == edit_;
    }
    [[nodiscard]] std::wstring Text() const {
        const int length = GetWindowTextLengthW(edit_);
        std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1, L'\0');
        const int copied = GetWindowTextW(edit_, text.data(), static_cast<int>(text.size()));
        return {text.data(), static_cast<std::size_t>(copied)};
    }
    [[nodiscard]] HWND Host() const noexcept { return host_; }
    [[nodiscard]] std::pair<std::size_t,std::size_t> Selection() const noexcept {
        const auto selection = static_cast<DWORD>(SendMessageW(edit_, EM_GETSEL, 0, 0));
        return {LOWORD(selection), HIWORD(selection)};
    }
private:
    HWND host_ = nullptr;
    HWND edit_ = nullptr;
    bool own_window_ = false;
    HKL original_layout_ = nullptr;
};

struct WorkerResult {
    lshift::TextResult text;
    bool desktop_ready = false;
    HRESULT com_status = E_FAIL;
};

[[nodiscard]] WorkerResult Process(const Fixture& fixture, HDESK desktop, bool current,
                                    bool caps_active) {
    const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr) throw std::runtime_error("CreateEventW failed");
    WorkerResult result;
    const lshift::TextRequest request{fixture.Host(), GetTickCount64() + 2500, fixture.Host(),
                                    caps_active};
    std::thread worker([&] {
        // Desktop назначается до COM: COM может создать вспомогательные объекты USER.
        result.desktop_ready = SetThreadDesktop(desktop) != FALSE;
        if (result.desktop_ready) {
            result.com_status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(result.com_status)) {
                result.text = lshift::ProcessText(request, [current] { return current; });
                CoUninitialize();
            }
        }
        SetEvent(done);
    });
    const auto until = GetTickCount64() + 8000;
    bool wait_failed = false;
    for (;;) {
        const DWORD state = MsgWaitForMultipleObjectsEx(1, &done, 50, QS_ALLINPUT,
                                                       MWMO_INPUTAVAILABLE);
        if (state == WAIT_OBJECT_0) break;
        if (state == WAIT_FAILED) wait_failed = true;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (wait_failed || GetTickCount64() > until) {
            // Нельзя уничтожать fixture или зависший worker; завершается только тестовый процесс.
            std::cerr << "FAIL: isolated ProcessText worker did not finish; E2E incomplete\n";
            std::cerr.flush();
            ExitProcess(1);
        }
    }
    worker.join();
    CloseHandle(done);
    return result;
}

[[nodiscard]] bool LayoutsAvailable() {
    const int count = GetKeyboardLayoutList(0, nullptr);
    if (count <= 0) return false;
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    const int copied = GetKeyboardLayoutList(count, layouts.data());
    bool english = false;
    bool russian = false;
    for (int index = 0; index < copied; ++index) {
        const auto code = reinterpret_cast<ULONG_PTR>(layouts[static_cast<std::size_t>(index)]) &
                          0xFFFFFFFFULL;
        english = english || code == 0x04090409ULL;
        russian = russian || code == 0x04190419ULL;
    }
    const auto current = reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(GetCurrentThreadId())) &
                         0xFFFFFFFFULL;
    return english && russian && (current == 0x04090409ULL || current == 0x04190419ULL);
}

[[nodiscard]] HKL InstalledLayout(ULONG_PTR code) {
    const int count = GetKeyboardLayoutList(0, nullptr);
    if(count <= 0) return nullptr;
    std::vector<HKL> layouts(static_cast<std::size_t>(count));
    const int copied = GetKeyboardLayoutList(count, layouts.data());
    for(int index = 0; index < copied; ++index) {
        const HKL layout = layouts[static_cast<std::size_t>(index)];
        if((reinterpret_cast<ULONG_PTR>(layout) & 0xFFFFFFFFULL) == code) return layout;
    }
    return nullptr;
}

[[nodiscard]] bool CheckCase(const char* name, Fixture& fixture, const PrivateDesktop& desktop,
                            DWORD style, const wchar_t* input, std::size_t start,
                            std::size_t end, const wchar_t* expected,
                            lshift::TextStatus status, bool current = true,
                            std::optional<std::size_t> caret = std::nullopt,
                            ULONG_PTR expected_layout = 0x04190419ULL) {
    fixture.Field(style, input, start, end);
    if (!fixture.FocusMatches()) {
        std::cerr << "FAIL " << name << ": isolated foreground/focus was lost\n";
        return false;
    }
    const HKL source_layout = InstalledLayout(expected_layout == 0x04090409ULL ? 0x04190419ULL : 0x04090409ULL);
    if(source_layout == nullptr || !fixture.SourceLayout(source_layout)) {
        std::cerr << "FAIL " << name << ": could not establish the own test thread source layout\n";
        return false;
    }
    const HKL previous_layout = GetKeyboardLayout(GetCurrentThreadId());
    const WorkerResult result = Process(fixture, desktop.Get(), current,
                                       (GetKeyState(VK_CAPITAL) & 1) != 0);
    if(result.text.status == lshift::TextStatus::Submitted) {
        const auto until = GetTickCount64() + 500;
        while(fixture.FocusMatches() && fixture.Text() != expected && GetTickCount64() < until) {
            MSG message{};
            while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(2);
        }
    }
    const bool unchanged_case = status == lshift::TextStatus::Unsupported ||
                                status == lshift::TextStatus::Stale;
    const HKL observed_layout = GetKeyboardLayout(GetCurrentThreadId());
    const auto observed_code = reinterpret_cast<ULONG_PTR>(observed_layout) & 0xFFFFFFFFULL;
    const bool layout_ok = unchanged_case ? observed_layout == previous_layout :
        status == lshift::TextStatus::Switched ? observed_layout != previous_layout :
        (status != lshift::TextStatus::Converted && status != lshift::TextStatus::Submitted) ||
            observed_code == expected_layout;
    const auto selection = fixture.Selection();
    const bool caret_ok = !caret || (selection.first == *caret && selection.second == *caret);
    const bool passed = result.desktop_ready && SUCCEEDED(result.com_status) &&
        result.text.status == status && fixture.Text() == expected && layout_ok &&
        desktop.InputUnchanged() && fixture.FocusMatches() && caret_ok;
    std::cerr << (passed ? "PASS " : "FAIL ") << name << '\n';
    if (!passed) {
        std::wcerr << L"  ProcessText detail: " << result.text.detail << L'\n';
    }
    return passed;
}

[[nodiscard]] bool CheckRoundTrips(Fixture& fixture, const PrivateDesktop& desktop) {
    constexpr std::wstring_view original = L"hello ghbdtn  ";
    constexpr std::wstring_view converted = L"hello привет  ";
    const HKL english = InstalledLayout(0x04090409ULL);
    const HKL russian = InstalledLayout(0x04190419ULL);
    fixture.Field(0, original.data(), original.size(), original.size());
    if(!fixture.SourceLayout(english) || !fixture.FocusMatches()) return false;
    for(unsigned cycle = 0; cycle < 12; ++cycle) {
        const auto expected = cycle % 2 == 0 ? converted : original;
        const HKL target = cycle % 2 == 0 ? russian : english;
        const auto result = Process(fixture, desktop.Get(), true, (GetKeyState(VK_CAPITAL) & 1) != 0);
        const auto until = GetTickCount64() + 500;
        while(fixture.FocusMatches() && fixture.Text() != expected && GetTickCount64() < until) {
            MSG message{};
            while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            Sleep(2);
        }
        const auto selection = fixture.Selection();
        if(result.text.status != lshift::TextStatus::Submitted || fixture.Text() != expected ||
            GetKeyboardLayout(GetCurrentThreadId()) != target ||
            selection.first != original.size() || selection.second != original.size()) {
            std::cerr << "FAIL multiword roundtrip cycle " << cycle + 1 << '\n';
            std::wcerr << result.text.detail << '\n';
            return false;
        }
    }
    std::cerr << "PASS 12 multiword roundtrips using actual editor text\n";
    return true;
}
struct QueuedGestureCheck {
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> armed{false};
    std::atomic<bool> interrupted{false};
    bool russian = false;
    bool latin = false;
    ~QueuedGestureCheck() { CloseHandle(entered); CloseHandle(release); }
};
LRESULT CALLBACK QueueFieldProc(HWND window, UINT message, WPARAM wp, LPARAM lp,
                               UINT_PTR, DWORD_PTR data) {
    auto& check = *reinterpret_cast<QueuedGestureCheck*>(data);
    if(message == WM_GETTEXT && InSendMessage() && check.armed.exchange(false)) {
        // Задерживается только чтение собственного поля через внешнюю Win32-границу.
        SetEvent(check.entered);
        WaitForSingleObject(check.release, 150);
    }
    if(message == WM_CHAR) {
        check.russian = check.russian || (wp >= L'А' && wp <= L'я');
        check.latin = check.latin || (wp >= L'A' && wp <= L'Z') || (wp >= L'a' && wp <= L'z');
    }
    return DefSubclassProc(window, message, wp, lp);
}
int CheckQueuedGesture(Fixture& fixture) {
    if(!FindWindowW(L"LShift.Main.1", nullptr)) {
        std::cerr << "E2E NOT EXECUTED: live queue check requires a running LShift\n";
        return Unavailable;
    }
    fixture.Field(0, L"", 0, 0);
    if(!fixture.EstablishForeground() || !fixture.SourceLayout(InstalledLayout(0x04090409ULL))) return Unavailable;
    GUITHREADINFO info{sizeof(info)};
    if(!GetGUIThreadInfo(GetCurrentThreadId(), &info) || !info.hwndFocus) return Unavailable;
    QueuedGestureCheck check;
    if(!check.entered || !check.release ||
       !SetWindowSubclass(info.hwndFocus, QueueFieldProc, 117, reinterpret_cast<DWORD_PTR>(&check))) return Unavailable;
    struct SubclassScope {
        HWND window;
        ~SubclassScope() { RemoveWindowSubclass(window, QueueFieldProc, 117); }
    } subclass_scope{info.hwndFocus};
    const auto send = [&](const std::vector<WORD>& keys) {
        if(GetForegroundWindow() != fixture.Host()) { check.interrupted.store(true); return false; }
        std::vector<INPUT> events;
        for(const auto key : keys) for(const bool up : {false, true}) {
            INPUT event{};
            event.type = INPUT_KEYBOARD;
            event.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
            event.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
            event.ki.dwExtraInfo = 0x5453544B;
            events.push_back(event);
        }
        return SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) == events.size();
    };
    const auto pump = [&](unsigned ms) {
        const auto until = GetTickCount64() + ms;
        do {
            if(GetForegroundWindow() != fixture.Host()) check.interrupted.store(true);
            MSG message{};
            while(PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            Sleep(2);
        } while(GetTickCount64() < until);
    };
    if(!send({'G','H','B','D','T','N'})) return Unavailable;
    pump(80);
    const auto original = fixture.Text();
    check.russian = check.latin = false;
    check.armed.store(true);
    std::atomic<bool> second_sent{false};
    std::thread second([&] {
        if(WaitForSingleObject(check.entered, 1200) == WAIT_OBJECT_0) {
            second_sent.store(send({VK_LSHIFT, VK_LSHIFT}));
            Sleep(25);
        }
        SetEvent(check.release);
    });
    const bool first_sent = send({VK_LSHIFT, VK_LSHIFT});
    pump(1400);
    second.join();
    const bool passed = first_sent && second_sent.load() && check.russian && check.latin &&
        fixture.Text() == original && GetKeyboardLayout(GetCurrentThreadId()) == InstalledLayout(0x04090409ULL);
    if(check.interrupted.load()) {
        std::cerr << "E2E NOT EXECUTED: focus left the own fixture; run interrupted\n";
        return Unavailable;
    }
    std::cerr << (passed ? "PASS" : "FAIL") << " a gesture during a pending read is processed after the first correction\n";
    if(!passed) std::cerr << "  first=" << first_sent << " second=" << second_sent.load()
        << " russian=" << check.russian << " latin=" << check.latin
        << " restored=" << (fixture.Text() == original) << '\n';
    return passed ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    bool tests_started = false;
    try {
        bool own_window = false;
        bool stability_only = false;
        bool live_queue = false;
        for(int index = 1; index < argc; ++index) {
            if(std::string_view(argv[index]) == "--own-window") own_window = true;
            else if(std::string_view(argv[index]) == "--stability-only") stability_only = true;
            else if(std::string_view(argv[index]) == "--live-queue") live_queue = true;
        }
        if(!own_window) {
            std::cerr << "E2E NOT EXECUTED: the Backspace/paste input suite requires explicit --own-window; "
                         "no input was sent and the input desktop was not switched.\n";
            return Unavailable;
        }
        PrivateDesktop desktop(own_window);
        Fixture fixture(own_window);
        fixture.Field(0, L"ghbdtn", 6, 6);
        if (!desktop.InputUnchanged() || !fixture.EstablishForeground()) {
            std::cerr << "E2E NOT EXECUTED: Windows did not provide foreground and Edit focus "
                         "to the own-window fixture; no test text input was sent and the input desktop was not switched.\n";
            return Unavailable;
        }
        if (!LayoutsAvailable()) {
            std::cerr << "E2E NOT EXECUTED: installed standard EN-US/RU layouts and a supported "
                         "test-thread layout are required; no layouts were installed or loaded.\n";
            return Unavailable;
        }
        tests_started = true;
        if(live_queue) return CheckQueuedGesture(fixture);
        if(stability_only) return CheckRoundTrips(fixture, desktop) ? 0 : 1;
        bool passed = true;
        passed &= CheckRoundTrips(fixture, desktop);
        passed &= CheckCase("last word", fixture, desktop, 0, L"ghbdtn", 6, 6,
                            L"привет", lshift::TextStatus::Submitted, true, 6, 0x04190419ULL);
        passed &= CheckCase("number-row word", fixture, desktop, 0, L"ghbdtn#", 7, 7,
                            L"привет№", lshift::TextStatus::Submitted, true, 7, 0x04190419ULL);
        passed &= CheckCase("reverse number sign", fixture, desktop, 0, L"привет№", 7, 7,
                            L"ghbdtn#", lshift::TextStatus::Submitted, true, 7, 0x04090409ULL);
        passed &= CheckCase("standalone number sign", fixture, desktop, 0, L"#", 1, 1,
                            L"№", lshift::TextStatus::Submitted, true, 1, 0x04190419ULL);
        passed &= CheckCase("standalone punctuation", fixture, desktop, 0, L"?", 1, 1,
                            L",", lshift::TextStatus::Submitted, true, 1, 0x04190419ULL);
        passed &= CheckCase("symbols-only selection", fixture, desktop, 0, L"# @ $ ^ & .", 0, 11,
                            L"№ \" ; : ? ю", lshift::TextStatus::Converted, true, 11);
        passed &= CheckCase("existing word without history", fixture, desktop, 0, L"ghbdtn", 6, 6,
                            L"привет", lshift::TextStatus::Submitted, true, 6);
        passed &= CheckCase("selection takes priority", fixture, desktop, 0,
                            L"ghbdtn vbh", 0, 6, L"привет vbh", lshift::TextStatus::Converted);
        passed &= CheckCase("bilingual selection", fixture, desktop, 0,
                            L"привет ghbdtn", 0, 13, L"ghbdtn привет", lshift::TextStatus::Converted);
        passed &= CheckCase("mixed word preserved inside selection", fixture, desktop, 0,
                            L"привет приvet ghbdtn", 0, 20, L"ghbdtn приvet привет",
                            lshift::TextStatus::Converted);
        passed &= CheckCase("selection follows last converted word", fixture, desktop, 0,
                            L"hello мир приvet", 0, 16, L"руддщ vbh приvet",
                            lshift::TextStatus::Converted, true, 16, 0x04090409ULL);
        passed &= CheckCase("only mixed word unchanged", fixture, desktop, 0,
                            L"приvet", 0, 6, L"приvet", lshift::TextStatus::Switched);
        passed &= CheckCase("physical punctuation selection", fixture, desktop, 0,
                            L";bdjq [jhjij", 0, 12, L"живой хорошо", lshift::TextStatus::Converted);
        passed &= CheckCase("selected repeated occurrence", fixture, desktop, 0,
                            L"ghbdtn ghbdtn ghbdtn", 7, 13, L"ghbdtn привет ghbdtn",
                            lshift::TextStatus::Converted);
        passed &= CheckCase("caret after trailing spaces", fixture, desktop, 0,
                            L"ghbdtn  ", 8, 8, L"привет  ", lshift::TextStatus::Submitted,
                            true, 8, 0x04190419ULL);
        passed &= CheckCase("multiline Edit", fixture, desktop, ES_MULTILINE,
                            L"first\r\nghbdtn", 13, 13, L"first\r\nпривет", lshift::TextStatus::Submitted,
                            true, 13, 0x04190419ULL);
        passed &= CheckCase("reverse conversion with case", fixture, desktop, 0,
                            L"Привет", 6, 6, L"Ghbdtn", lshift::TextStatus::Submitted,
                            true, 6, 0x04090409ULL);
        passed &= CheckCase("password unchanged", fixture, desktop, ES_PASSWORD,
                            L"ghbdtn", 6, 6, L"ghbdtn", lshift::TextStatus::Unsupported);
        passed &= CheckCase("read-only unchanged", fixture, desktop, ES_READONLY,
                            L"ghbdtn", 6, 6, L"ghbdtn", lshift::TextStatus::Unsupported);
        passed &= CheckCase("stale predicate unchanged", fixture, desktop, 0,
                            L"ghbdtn", 6, 6, L"ghbdtn", lshift::TextStatus::Stale, false);
        const HKL before_empty = GetKeyboardLayout(GetCurrentThreadId());
        const bool empty = CheckCase("empty field toggles layout", fixture, desktop, 0,
            L"", 0, 0, L"", lshift::TextStatus::Switched);
        const auto before_code = reinterpret_cast<ULONG_PTR>(before_empty) & 0xFFFFFFFFULL;
        const auto after_code = reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(GetCurrentThreadId())) &
                                0xFFFFFFFFULL;
        const auto expected_code = before_code == 0x04090409ULL ? 0x04190419ULL : 0x04090409ULL;
        const bool switched = after_code == expected_code;
        if (empty && !switched) std::cerr << "FAIL empty field: target thread layout unchanged\n";
        passed &= empty && switched;
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << (tests_started ? "FAIL: isolated test interrupted: " :
                      "E2E NOT EXECUTED: isolated fixture unavailable: ") << error.what() << '\n';
        return tests_started ? 1 : Unavailable;
    }
}
