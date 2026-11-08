#include "core.hpp"
#include "shell.hpp"
#include "localization.hpp"
#include "text_service.hpp"
#include "diagnostics.hpp"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objbase.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace {
constexpr wchar_t kWindowClass[] = L"LShift.Main.1";
constexpr UINT kResultMessage = WM_APP + 20;
constexpr UINT kShowTrayMenu = WM_APP + 21;
constexpr UINT kWorkerStopped = WM_APP + 22;
constexpr UINT_PTR kSmokeTimer = 90;
constexpr UINT_PTR kTrayRetryTimer = 91;

struct OwnedIcon {
    HICON value = nullptr;
    ~OwnedIcon() { if(value) DestroyIcon(value); }
};
class App {
public:
    explicit App(std::filesystem::path smoke_path) : smoke_path_(std::move(smoke_path)) {
        settings_ = lshift::LoadSettings();
        if(!smoke_path_.empty()) settings_.enabled = false;
        detector_.SetSettings(settings_);
    }
    ~App() {
        stopping_.store(true);
        event_app_ = nullptr;
        if(foreground_hook_) UnhookWinEvent(foreground_hook_);

        worker_.request_stop();
        wake_.notify_all();
        if(worker_.joinable()) worker_.join();
        lshift::CloseSettings();
        if(window_) lshift::RemoveTray(window_);
    }
    [[nodiscard]] bool Initialize(HINSTANCE instance) {
        enabled_icon_.value = lshift::CreateTrayIcon(true);
        disabled_icon_.value = lshift::CreateTrayIcon(false);
        if(!enabled_icon_.value || !disabled_icon_.value) return false;
        WNDCLASSEXW cls{sizeof(cls)};
        cls.hInstance = instance;
        cls.lpfnWndProc = WindowProc;
        cls.lpszClassName = kWindowClass;
        cls.hIcon = enabled_icon_.value;
        cls.hIconSm = enabled_icon_.value;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if(!RegisterClassExW(&cls)) return false;
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"LShift", WS_OVERLAPPED,
                                  0, 0, 0, 0, nullptr, nullptr, instance, this);
        if(!window_) return false;
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        const RAWINPUTDEVICE devices[]{
            {0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_},
            {0x01, 0x02, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_}
        };
        raw_registered_ = RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE)) != FALSE;
        event_app_ = this;
        foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, ContextEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if(!raw_registered_ || !foreground_hook_ || !taskbar_created_) return false;
        if(!RegisterTray()) return false;
        SeedHeldKeys();
        worker_ = std::jthread([this](std::stop_token stop) { Worker(stop); });
        if(!smoke_path_.empty()) SetTimer(window_, kSmokeTimer, 1000, nullptr);
        return true;
    }
    [[nodiscard]] HWND window() const noexcept { return window_; }
    [[nodiscard]] int Run() {
        MSG message{};
        BOOL status = 0;
        while((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            const HWND active = GetActiveWindow();
            wchar_t name[64]{};
            if(active && GetClassNameW(active, name, 64) &&
               std::wstring_view(name) == L"LShift.Settings.1" && IsDialogMessageW(active, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return status == -1 ? 1 : 0;
    }
private:
    [[nodiscard]] bool RegisterTray() {
        tray_added_ = lshift::AddTray(window_, Icon(), settings_.enabled);
        if(tray_added_) {
            KillTimer(window_, kTrayRetryTimer);
            return true;
        }
        // Explorer при входе может ещё не принимать значки. Работа ввода
        // продолжается; таймер нужен только до успешной регистрации трея.
        return SetTimer(window_, kTrayRetryTimer, 1000, nullptr) != 0;
    }
    struct Job { lshift::TextRequest request; std::uint64_t epoch = 0; };
    void Trace(lshift::DiagnosticEvent event, lshift::DiagnosticReason reason,
               std::uint32_t data = 0, std::uint64_t source_tick = 0) noexcept {
#if LSHIFT_DIAGNOSTICS

        lshift::DiagnosticRecord record;
        record.event = event;
        record.reason = reason;
        record.tick = GetTickCount64();
        record.source_tick = source_tick;
        record.epoch = generation_.load();

        record.data = data;
        (void)diagnostics_.Push(record);
        wake_.notify_one();
#else
        (void)event;
        (void)reason;
        (void)data;
        (void)source_tick;
#endif
    }
#if LSHIFT_DIAGNOSTICS
    void DrainDiagnostics(lshift::DiagnosticLog& log) noexcept {
        lshift::DiagnosticRecord record;
        while(diagnostics_.Pop(record)) (void)log.Append(record, {}, diagnostics_.Dropped());
    }
#endif
    [[nodiscard]] HICON Icon() const noexcept { return settings_.enabled ? enabled_icon_.value : disabled_icon_.value; }
    void SeedHeldKeys() noexcept {
        detector_.Reset();
        foreground_ = GetForegroundWindow();
        caps_active_ = (GetKeyState(VK_CAPITAL) & 1) != 0;
        caps_held_ = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
        for(std::uint16_t key = 8; key < 255; ++key) {
            if(!lshift::KeyboardChordKey(key) || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
            detector_.SetHeld(key, (GetAsyncKeyState(key) & 0x8000) != 0);
        }
    }
    void InvalidateInput() noexcept {
        generation_.fetch_add(1);
        if(busy_.load()) Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::Mouse);
    }
    static void CALLBACK ContextEvent(HWINEVENTHOOK, DWORD event, HWND window,
                                     LONG, LONG, DWORD, DWORD) noexcept {
        auto* app = event_app_;
        if(!app || app->stopping_.load() || event != EVENT_SYSTEM_FOREGROUND ||
           !window || window != GetForegroundWindow() || window == app->foreground_) return;
        app->foreground_ = window;
        app->generation_.fetch_add(1);
        app->Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::Foreground);
    }
    void Invalidate() noexcept {
        Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::Settings);
        generation_.fetch_add(1);
        SeedHeldKeys();
    }
    void RawInput(HRAWINPUT handle) {
        if(stopping_.load()) return;
        RAWINPUT input{};
        UINT size = sizeof(input);
        const UINT count = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
        if(count == static_cast<UINT>(-1) || count < sizeof(RAWINPUTHEADER)) return;
        if(input.header.dwType == RIM_TYPEMOUSE) {
            constexpr USHORT buttons = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN |
                RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
            if((input.data.mouse.usButtonFlags & buttons) != 0) InvalidateInput();
            return;
        }
        if(input.header.dwType != RIM_TYPEKEYBOARD) return;
        const auto& key = input.data.keyboard;
        if(key.ExtraInformation == 0x4C534846UL) return;
        std::uint16_t vk = key.VKey;
        if(vk == 255 || key.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) { Invalidate(); return; }
        if(vk == VK_SHIFT) vk = key.MakeCode == 0x2A ? VK_LSHIFT : VK_RSHIFT;
        if(vk == VK_CONTROL) vk = (key.Flags & RI_KEY_E0) != 0 ? VK_RCONTROL : VK_LCONTROL;
        if(vk == VK_MENU) vk = (key.Flags & RI_KEY_E0) != 0 ? VK_RMENU : VK_LMENU;
        const bool down = (key.Flags & RI_KEY_BREAK) == 0;
        if(lshift::ChangesTextOrPosition(vk, down)) generation_.fetch_add(1);
        if(vk == VK_CAPITAL) {
            if(down && !caps_held_) caps_active_ = !caps_active_;
            caps_held_ = down;
        }
        const auto now = GetTickCount64();
        const auto age = static_cast<DWORD>(now) - static_cast<DWORD>(GetMessageTime());
        if(age > 250) {
            Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::OldInput, age);
            SeedHeldKeys(); return;
        }
        const HWND foreground = GetForegroundWindow();
        if(foreground != foreground_) {
            foreground_ = foreground;
            generation_.fetch_add(1);
        }
        const auto epoch = generation_.load();
        const bool triggered = detector_.Feed({vk, (key.Flags & RI_KEY_BREAK) == 0, now - age,
                                              reinterpret_cast<std::uintptr_t>(foreground),
                                              reinterpret_cast<std::uintptr_t>(input.header.hDevice)});
        if(!triggered || !foreground) return;
        // Проверка только готового жеста дополняет Raw Input состоянием клавиш до запуска.
        for(int key_code = 8; key_code < 255; ++key_code) {
            if(!lshift::KeyboardChordKey(static_cast<std::uint16_t>(key_code))) continue;
            if((GetAsyncKeyState(key_code) & 0x8000) != 0) {
                Trace(lshift::DiagnosticEvent::Gesture, lshift::DiagnosticReason::HeldKey);
                return;
            }
        }
        DWORD process = 0;
        GetWindowThreadProcessId(foreground, &process);
        if(process == GetCurrentProcessId()) {
            Trace(lshift::DiagnosticEvent::Gesture, lshift::DiagnosticReason::OwnWindow);
            return;
        }
        std::unique_lock lock(jobs_mutex_);
        const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
        GUITHREADINFO info{sizeof(info)};
        const HWND focus = thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus ? info.hwndFocus : foreground;
        // Храним не более одного ожидающего запроса; более новый заменяет ожидающий.
        job_ = Job{{foreground, now + 1500, window_, caps_active_, focus, settings_.language}, epoch};
        Trace(lshift::DiagnosticEvent::Gesture, lshift::DiagnosticReason::Accepted);
        lock.unlock();
        wake_.notify_one();
    }
    void Worker(std::stop_token stop) {
        std::optional<HRESULT> apartment;
        worker_started_.store(true);
#if LSHIFT_DIAGNOSTICS
        const auto settings = lshift::SettingsPath();
        lshift::DiagnosticLog log(settings.empty() ? std::filesystem::path{} :
            settings.parent_path() / L"diagnostics.jsonl");
        lshift::DiagnosticRecord startup;
        startup.event = lshift::DiagnosticEvent::Startup;
        startup.tick = GetTickCount64();
        startup.data = GetCurrentProcessId();
        wchar_t executable[32768]{}, temporary[32768]{};
        const auto executable_size = GetModuleFileNameW(nullptr, executable, 32768);
        const auto temporary_size = GetTempPathW(32768, temporary);
        startup.temporary_executable = executable_size != 0 && temporary_size != 0 &&
            temporary_size < executable_size && CompareStringOrdinal(executable,
                static_cast<int>(temporary_size), temporary, static_cast<int>(temporary_size), TRUE) == CSTR_EQUAL;
        (void)log.Append(startup);
#endif
        while(!stop.stop_requested()) {
            std::optional<Job> current;
            {
                std::unique_lock lock(jobs_mutex_);
                wake_.wait(lock, stop, [this] {
                    return job_.has_value()
#if LSHIFT_DIAGNOSTICS
                        || diagnostics_.Pending()
#endif
                        ;
                });
                if(stop.stop_requested()) break;
#if LSHIFT_DIAGNOSTICS
                if(!job_) {
                    lock.unlock();
                    DrainDiagnostics(log);
                    continue;
                }
#endif
                current = std::move(job_);
                job_.reset();
                busy_.store(true);
            }
            const auto valid = [this, &stop, &current] {
                return !stop.stop_requested() && !stopping_.load() &&
                       generation_.load() == current->epoch &&
                       GetTickCount64() <= current->request.deadline_ms &&
                       GetForegroundWindow() == current->request.foreground;
            };
#if LSHIFT_DIAGNOSTICS
            DrainDiagnostics(log);
            const auto started = GetTickCount64();
#endif
            // COM нужен только для текстовой команды; при простом запуске поток спит без apartment.
            if(!apartment) {
                apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                com_initialized_.store(SUCCEEDED(*apartment));
            }
            lshift::TextResult result;
            if(FAILED(*apartment)) result = {lshift::TextStatus::Failed, lshift::LocalizedText(lshift::StringId::StatusCouldNotInitializeWindowsTextAccess, current->request.language)};
            else {
                try { result = lshift::ProcessText(current->request, valid); }
                catch(...) { result = {lshift::TextStatus::Failed, lshift::LocalizedText(lshift::StringId::StatusTextOperationCancelledAfterAnError, current->request.language)}; }
            }
#if LSHIFT_DIAGNOSTICS
            lshift::DiagnosticRecord recorded;
            recorded.event = lshift::DiagnosticEvent::Result;
            recorded.tick = GetTickCount64();
            recorded.epoch = current->epoch;

            recorded.layout = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(
                GetKeyboardLayout(GetWindowThreadProcessId(current->request.foreground, nullptr))));
            recorded.status = static_cast<std::uint32_t>(result.status);
            recorded.elapsed = static_cast<std::uint32_t>(recorded.tick - started);
            recorded.data = static_cast<std::uint32_t>(generation_.load());
            DrainDiagnostics(log);
            (void)log.Append(recorded, result.detail, diagnostics_.Dropped());
#endif
            {
                std::lock_guard lock(result_mutex_);
                result_ = std::move(result);
                result_language_ = current->request.language;
            }
            busy_.store(false);
            PostMessageW(window_, kResultMessage, 0, 0);
        }
#if LSHIFT_DIAGNOSTICS
        DrainDiagnostics(log);
#endif
        if(apartment && SUCCEEDED(*apartment)) CoUninitialize();
        PostMessageW(window_, kWorkerStopped, 0, 0);
    }
    void BeginStop() {
        if(stopping_.exchange(true)) return;
        KillTimer(window_, kTrayRetryTimer);
        generation_.fetch_add(1);
        worker_.request_stop();
        wake_.notify_all();
        // Окно-владелец clipboard остаётся живым до завершения рабочего потока.
        if(!worker_.joinable()) DestroyWindow(window_);
    }
    void Command(UINT command) {
        if(stopping_.load()) return;
        if(command == lshift::kExit) { BeginStop(); return; }
#if LSHIFT_DIAGNOSTICS
        if(command == lshift::kOpenDiagnostics) {
            const auto settings = lshift::SettingsPath();
            if(!settings.empty()) {
                const auto path = settings.parent_path() / L"diagnostics.jsonl";
                if(reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                    MessageBoxW(window_, lshift::LocalizedText(lshift::StringId::DiagnosticsError, settings_.language).c_str(),
                                L"LShift", MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
#endif
        if(command == lshift::kOpenSettings) { Invalidate(); lshift::ShowSettings(window_, settings_); return; }
        if(command == lshift::kToggleAutostart) {
            if(!lshift::SetAutostart(!lshift::AutostartEnabled()))
                MessageBoxW(window_, lshift::LocalizedText(lshift::StringId::AutostartChangeError, settings_.language).c_str(), L"LShift", MB_OK | MB_ICONERROR);
            return;
        }
        if(command == lshift::kToggleEnabled) {
            auto next = settings_;
            next.enabled = !next.enabled;
            if(!lshift::SaveSettings(next)) {
                MessageBoxW(window_, lshift::LocalizedText(lshift::StringId::SaveShortError, settings_.language).c_str(), L"LShift", MB_OK | MB_ICONERROR);
                return;
            }
            Reload();
        }
    }
    void Reload() {
        settings_ = lshift::LoadSettings();
        detector_.SetSettings(settings_);
        Invalidate();
        lshift::UpdateTray(window_, Icon(), settings_.enabled);
    }
    void SmokeReport() {
        std::ofstream out(smoke_path_);
        out << "{\"raw_input_registered\":" << (raw_registered_ ? "true" : "false")
            << ",\"tray_registered\":" << (tray_added_ ? "true" : "false")
            << ",\"tray_retry_pending\":" << (!tray_added_ ? "true" : "false")
            << ",\"worker_initialized\":" << (worker_started_.load() ? "true" : "false")
            << ",\"com_initialized\":" << (com_initialized_.load() ? "true" : "false")
            << ",\"diagnostics_enabled\":" << (LSHIFT_DIAGNOSTICS ? "true" : "false")
            << ",\"gesture_actions_disabled\":true}\n";
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if(taskbar_created_ && message == taskbar_created_) {
            if(!stopping_.load()) (void)RegisterTray();
            return 0;
        }
        switch(message) {
        case WM_INPUT:
            RawInput(reinterpret_cast<HRAWINPUT>(lparam));
            return DefWindowProcW(window_, message, wparam, lparam);
        case WM_INPUT_DEVICE_CHANGE:
        case WM_POWERBROADCAST:
            Invalidate();
            return TRUE;
        case lshift::kTrayMessage: {
            const UINT event = LOWORD(lparam);
            if(event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
                Invalidate();
                PostMessageW(window_, kShowTrayMenu, wparam, 0);
            } else if(event == WM_LBUTTONDBLCLK || event == NIN_KEYSELECT) {
                Invalidate();
                lshift::ShowSettings(window_, settings_);
            }
            return 0;
        }
        case kShowTrayMenu: {
            const POINT point{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
            lshift::ShowTrayMenu(window_, settings_.enabled, point);
            return 0;
        }
        case WM_COMMAND: Command(LOWORD(wparam)); return 0;
        case lshift::kSettingsChanged: Reload(); return 0;
        case lshift::kShowSettings: lshift::ShowSettings(window_, settings_); return 0;
        case kResultMessage: {
            std::lock_guard lock(result_mutex_);
            if(result_) {
                if(result_language_ == settings_.language) lshift::SetTrayStatus(window_, result_->detail);
                result_.reset();
            }
            return 0;
        }
        case WM_TIMER:
            if(wparam == kSmokeTimer) { KillTimer(window_, kSmokeTimer); SmokeReport(); BeginStop(); }
            else if(wparam == kTrayRetryTimer && !stopping_.load()) (void)RegisterTray();
            return 0;
        case WM_CLOSE: BeginStop(); return 0;
        case kWorkerStopped:
            if(stopping_.load()) DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            stopping_.store(true);
            worker_.request_stop();
            wake_.notify_all();
            lshift::CloseSettings();
            lshift::RemoveTray(window_);
            PostQuitMessage(0);
            return 0;
        default: return DefWindowProcW(window_, message, wparam, lparam);
        }
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if(message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        return app ? app->Handle(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }
    HWND window_ = nullptr;
    OwnedIcon enabled_icon_;
    OwnedIcon disabled_icon_;
    lshift::Settings settings_;
    lshift::Gesture detector_;
    bool caps_active_ = false;
    bool caps_held_ = false;
#if LSHIFT_DIAGNOSTICS
    lshift::DiagnosticQueue diagnostics_;
#endif
    HWND foreground_ = nullptr;

    HWINEVENTHOOK foreground_hook_ = nullptr;

    static inline App* event_app_ = nullptr;
    UINT taskbar_created_ = 0;
    bool raw_registered_ = false;
    bool tray_added_ = false;
    std::filesystem::path smoke_path_;
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> worker_started_{false};
    std::atomic<bool> com_initialized_{false};
    std::mutex jobs_mutex_;
    std::condition_variable_any wake_;
    std::optional<Job> job_;
    std::mutex result_mutex_;
    std::optional<lshift::TextResult> result_;
    lshift::UiLanguage result_language_ = lshift::UiLanguage::English;
    std::jthread worker_;
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    std::filesystem::path smoke;
    bool settings = false;
    bool quit = false;
    bool autostart = false;
    if(arguments) {
        for(int i = 1; i < count; ++i) {
            if(std::wstring_view(arguments[i]) == L"--smoke-test" && i + 1 < count) smoke = arguments[++i];
            else if(std::wstring_view(arguments[i]) == L"--settings") settings = true;
            else if(std::wstring_view(arguments[i]) == L"--quit") quit = true;
            else if(std::wstring_view(arguments[i]) == L"--autostart") autostart = true;
        }
        LocalFree(arguments);
    }
    const HWND existing = FindWindowW(kWindowClass, nullptr);
    if(quit) { if(existing) PostMessageW(existing, WM_CLOSE, 0, 0); return 0; }
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\LShift.App.1");
    if(!mutex) return 1;
    const bool already_running = GetLastError() == ERROR_ALREADY_EXISTS;
    if(already_running) {
        if(existing && smoke.empty() && !autostart) PostMessageW(existing, lshift::kShowSettings, 0, 0);
        CloseHandle(mutex);
        return smoke.empty() ? 0 : 2;
    }
    int result = 1;
    try {
        App app(std::move(smoke));
        if(app.Initialize(instance)) {
            if(settings) PostMessageW(app.window(), lshift::kShowSettings, 0, 0);
            result = app.Run();
        } else MessageBoxW(nullptr, lshift::LocalizedText(lshift::StringId::StartupError, lshift::LoadSettings().language).c_str(), L"LShift", MB_OK | MB_ICONERROR);
    } catch(...) { MessageBoxW(nullptr, lshift::LocalizedText(lshift::StringId::StartupShortError, lshift::LoadSettings().language).c_str(), L"LShift", MB_OK | MB_ICONERROR); }
    CloseHandle(mutex);
    return result;
}
