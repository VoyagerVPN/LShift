#include "shell.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
constexpr wchar_t kTestClass[] = L"LShift.TrayTest.1";
constexpr UINT_PTR kInspectMenu = 1;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

[[nodiscard]] BOOL CALLBACK FindMenu(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window)) return TRUE;
    std::array<wchar_t, 32> name{};
    if (GetClassNameW(window, name.data(), static_cast<int>(name.size())) != 0 &&
        std::wstring_view(name.data()) == L"#32768") {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

class TrayFixture {
public:
    TrayFixture() : previous_foreground_(GetForegroundWindow()) {
        Require(GetCursorPos(&previous_cursor_) != FALSE, "remember pointer position");
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        type.lpszClassName = kTestClass;
        Require(RegisterClassW(&type) != 0, "register tray test windows");
        registered_ = true;
    }

    ~TrayFixture() {
        if (owner_) KillTimer(owner_, kInspectMenu);
        const HWND foreground = GetForegroundWindow();
        if ((foreground == owner_ || foreground == overflow_ || foreground == outside_) &&
            previous_foreground_ && IsWindow(previous_foreground_)) {
            SetForegroundWindow(previous_foreground_);
        }
        if (overflow_) DestroyWindow(overflow_);
        if (outside_) DestroyWindow(outside_);
        if (owner_) DestroyWindow(owner_);
        SetCursorPos(previous_cursor_.x, previous_cursor_.y);
        if (registered_) UnregisterClassW(kTestClass, GetModuleHandleW(nullptr));
    }

    TrayFixture(const TrayFixture&) = delete;
    TrayFixture& operator=(const TrayFixture&) = delete;

    void Run() {
        RECT work{};
        Require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE, "get desktop work area");
        Require(work.right - work.left >= 900 && work.bottom - work.top >= 500,
                "tray test requires an interactive work area of at least 900 by 500");
        owner_ = CreateWindowExW(WS_EX_TOOLWINDOW, kTestClass, L"LShift hidden menu test owner",
            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        Require(owner_ && !IsWindowVisible(owner_), "create hidden tray owner");
        outside_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTestClass, L"LShift outside click target",
            WS_POPUP | WS_BORDER, work.left + 20, work.top + 20, 260, 180,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        overflow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kTestClass,
            L"LShift topmost overflow simulation", WS_POPUP | WS_BORDER,
            work.right - 440, work.bottom - 320, 420, 300,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        Require(outside_ && overflow_, "create our outside and topmost overflow windows");
        ShowWindow(outside_, SW_SHOWNOACTIVATE);
        ShowWindow(overflow_, SW_SHOW);
        RECT overflow_rectangle{};
        Require(GetWindowRect(overflow_, &overflow_rectangle) != FALSE, "get overflow bounds");
        const POINT activation{overflow_rectangle.left + 30, overflow_rectangle.top + 30};
        Require(WindowFromPoint(activation) == overflow_, "activation click must hit only our overflow window");
        Require(SetCursorPos(activation.x, activation.y) != FALSE, "position pointer on our overflow window");
        std::array<INPUT, 2> activation_click{};
        activation_click[0].type = INPUT_MOUSE;
        activation_click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        activation_click[1].type = INPUT_MOUSE;
        activation_click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        Require(SendInput(static_cast<UINT>(activation_click.size()), activation_click.data(), sizeof(INPUT)) ==
                activation_click.size(), "click only our overflow to establish genuine foreground ownership");
        const ULONGLONG activation_deadline = GetTickCount64() + 1000;
        while ((!activation_released_ || GetForegroundWindow() != overflow_) &&
               GetTickCount64() < activation_deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        Require(GetForegroundWindow() == overflow_, "test process must own foreground before opening menu");
        Require(activation_released_, "activation click must finish before the popup opens");
        const POINT anchor{overflow_rectangle.left + 50, overflow_rectangle.top + 55};
        deadline_ = GetTickCount64() + 2000;
        Require(SetTimer(owner_, kInspectMenu, 25, nullptr) != 0, "start bounded menu observation");

        // Публичное меню вызывается без регистрации иконки, поэтому живой LShift не затрагивается.
        lshift::ShowTrayMenu(owner_, true, anchor);

        menu_finished_ = true;
        KillTimer(owner_, kInspectMenu);
        if (failure_) std::rethrow_exception(failure_);
        Require(inspected_, "native popup menu must become observable");
        Require(outside_click_sent_, "menu must be dismissed by an outside click, not a timeout");
        Require(!IsWindowVisible(owner_), "temporary menu activation must leave the owner hidden");
        Require((GetWindowLongPtrW(owner_, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
                "temporary menu activation must restore the owner's non-topmost state");
        HWND remaining = nullptr;
        EnumThreadWindows(GetCurrentThreadId(), FindMenu, reinterpret_cast<LPARAM>(&remaining));
        Require(!remaining, "outside click must close the native popup");
        const ULONGLONG outside_deadline = GetTickCount64() + 1000;
        while ((GetForegroundWindow() == owner_ || (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) &&
               GetTickCount64() < outside_deadline) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        // Нативное меню вправе обработать отменяющий клик само; пересылка WM_LBUTTONUP не обещана API.
        MSG pending{};
        while (PeekMessageW(&pending, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&pending);
            DispatchMessageW(&pending);
        }
        const HWND foreground = GetForegroundWindow();
        Require(foreground != owner_, "dismissed menu must not retain foreground in its hidden owner");
        Require(foreground == outside_ || foreground == overflow_,
                "dismissal must leave foreground on the outside target or the preceding window");
        Require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0, "outside click must not leave the mouse pressed");
        Require(commands_ == 0, "outside dismissal must not invoke a tray menu command");
    }

private:
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* state = reinterpret_cast<TrayFixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            state = static_cast<TrayFixture*>(creation->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        if (state && message == WM_TIMER && wparam == kInspectMenu) {
            state->Inspect();
            return 0;
        }
        if (state && window == state->overflow_ && message == WM_LBUTTONUP) {
            state->activation_released_ = true;
        }
        if (state && window == state->owner_ && message == WM_COMMAND &&
            LOWORD(wparam) >= lshift::kToggleEnabled && LOWORD(wparam) <= lshift::kExit) {
            ++state->commands_;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void Inspect() noexcept {
        if (menu_finished_) return;
        try {
            if (outside_click_sent_) {
                if (GetTickCount64() >= deadline_) {
                    throw std::runtime_error("popup did not dismiss after the outside click");
                }
                return;
            }
            HWND menu = nullptr;
            EnumThreadWindows(GetCurrentThreadId(), FindMenu, reinterpret_cast<LPARAM>(&menu));
            if (!menu) {
                Require(GetTickCount64() < deadline_, "native menu did not appear before timeout");
                return;
            }
            Require(GetForegroundWindow() == owner_, "tray menu owner must actually have foreground");
            GUITHREADINFO information{sizeof(information)};
            Require(GetGUIThreadInfo(GetCurrentThreadId(), &information) != FALSE &&
                    information.hwndMenuOwner == owner_, "native menu loop belongs to the public menu owner");
            RECT menu_rectangle{};
            RECT overflow_rectangle{};
            RECT intersection{};
            Require(GetWindowRect(menu, &menu_rectangle) && GetWindowRect(overflow_, &overflow_rectangle) &&
                    IntersectRect(&intersection, &menu_rectangle, &overflow_rectangle),
                    "regression fixture must overlap the menu with the topmost overflow");
            bool menu_above_overflow = false;
            for (HWND candidate = GetTopWindow(nullptr); candidate;
                 candidate = GetWindow(candidate, GW_HWNDNEXT)) {
                if (candidate == menu) { menu_above_overflow = true; break; }
                if (candidate == overflow_) break;
            }
            Require(menu_above_overflow, "native menu must be above the simulated topmost overflow");
            const POINT visible{intersection.left + (intersection.right - intersection.left) / 2,
                                intersection.top + (intersection.bottom - intersection.top) / 2};
            Require(WindowFromPoint(visible) == menu,
                    "the overlapping menu area must receive pointer hits above overflow");
            inspected_ = true;
            RECT outside_rectangle{};
            Require(GetWindowRect(outside_, &outside_rectangle) != FALSE, "get outside click target");
            const POINT target{outside_rectangle.left + 30, outside_rectangle.top + 30};
            Require(!PtInRect(&menu_rectangle, target) && WindowFromPoint(target) == outside_,
                    "outside click must target our own visible test window");
            Require(SetCursorPos(target.x, target.y) != FALSE, "position pointer on our outside target");
            std::array<INPUT, 2> clicks{};
            clicks[0].type = INPUT_MOUSE;
            clicks[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            clicks[1].type = INPUT_MOUSE;
            clicks[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            Require(SendInput(static_cast<UINT>(clicks.size()), clicks.data(), sizeof(INPUT)) == clicks.size(),
                    "send an outside click only to our test target");
            outside_click_sent_ = true;
        } catch (...) {
            failure_ = std::current_exception();
            KillTimer(owner_, kInspectMenu);
            EndMenu();
        }
    }

    HWND owner_ = nullptr;
    HWND overflow_ = nullptr;
    HWND outside_ = nullptr;
    HWND previous_foreground_ = nullptr;
    POINT previous_cursor_{};
    std::exception_ptr failure_;
    ULONGLONG deadline_ = 0;
    bool registered_ = false;
    bool inspected_ = false;
    bool outside_click_sent_ = false;
    bool activation_released_ = false;
    bool menu_finished_ = false;
    unsigned commands_ = 0;
};
}

int main() {
    try {
        const BOOL changed = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Require(changed || AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "tray test requires PerMonitorV2 coordinates");
        TrayFixture fixture;
        fixture.Run();
        std::cout << "PASS: tray popup owns foreground, stays above topmost overflow and dismisses outside\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
