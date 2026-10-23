#include "core.hpp"
#include "shell.hpp"
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
        if(focus_hook_) UnhookWinEvent(focus_hook_);
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
        focus_hook_ = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS,
            nullptr, ContextEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        tray_added_ = lshift::AddTray(window_, Icon(), settings_.enabled);
        if(!raw_registered_ || !tray_added_ || !foreground_hook_ || !focus_hook_) return false;
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
    struct Job { lshift::TextRequest request; std::uint64_t epoch = 0; };
    void Trace(lshift::DiagnosticEvent event, lshift::DiagnosticReason reason,
               std::uint32_t data = 0, std::uint64_t source_tick = 0) noexcept {
#if LSHIFT_DIAGNOSTICS
        const auto recent = recent_.Snapshot();
        lshift::DiagnosticRecord record;
        record.event = event;
        record.reason = reason;
        record.tick = GetTickCount64();
        record.source_tick = source_tick;
        record.epoch = generation_.load();
        record.count = static_cast<std::uint32_t>(recent.count);
        record.complete = recent.complete;
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
        recent_.Reset((GetKeyState(VK_CAPITAL) & 1) != 0);
        for(std::uint16_t key = 8; key < 255; ++key) {
            if(!lshift::KeyboardChordKey(key)) continue;
            if(key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
            detector_.SetHeld(key, (GetAsyncKeyState(key) & 0x8000) != 0);
            if((key >= VK_LSHIFT && key <= VK_RMENU) || key == VK_LWIN || key == VK_RWIN) {
                recent_.Feed(key, (GetAsyncKeyState(key) & 0x8000) != 0);
            }
        }
    }
    void ClearRecent() noexcept {
#if LSHIFT_DIAGNOSTICS
        if(recent_.Snapshot().count != 0 || busy_.load())
            Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::Mouse);
#endif
        recent_.Clear();
        generation_.fetch_add(1);
    }
    static void CALLBACK ContextEvent(HWINEVENTHOOK, DWORD event, HWND window,
                                     LONG object, LONG child, DWORD, DWORD time) noexcept {
        auto* app = event_app_;
        if(!app || app->stopping_.load()) return;
        (void)child;
        const HWND foreground = GetForegroundWindow();
        if(event == EVENT_OBJECT_FOCUS) {
            if(!window || GetAncestor(window, GA_ROOT) != foreground) return;
            // Подключение accessibility тоже повторно объявляет прежний фокус.
            // Это подсказка для проверки, а не доказательство смены поля.
            // Native HWND и идентичность UIA-элемента проверяются перед записью;
            // клавиатура и кнопки мыши независимо отменяют устаревшую команду.
            app->Trace(lshift::DiagnosticEvent::FocusHint, lshift::DiagnosticReason::Focus,
                       static_cast<std::uint32_t>(object), time);
            app->recent_.MarkUncertain();
            return;
        }
        // WinEvent доставляется асинхронно: уведомление о прежнем foreground
        // не должно стирать набор, уже начавшийся в текущем окне.
        if(event != EVENT_SYSTEM_FOREGROUND || window != foreground ||
           static_cast<LONG>(time - app->last_edit_time_) < 0) return;
        app->generation_.fetch_add(1);
#if LSHIFT_DIAGNOSTICS
        if(app->recent_.Snapshot().count != 0 || app->busy_.load())
            app->Trace(lshift::DiagnosticEvent::Reset,lshift::DiagnosticReason::Foreground,
                       static_cast<std::uint32_t>(object), time);
#endif
        app->recent_.Clear();
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
            if((input.data.mouse.usButtonFlags & buttons) != 0) ClearRecent();
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
        const auto epoch = generation_.fetch_add(1) + 1;
        const auto now = GetTickCount64();
        const auto age = static_cast<DWORD>(now) - static_cast<DWORD>(GetMessageTime());
        if(age > 250) {
            Trace(lshift::DiagnosticEvent::Reset, lshift::DiagnosticReason::OldInput, age);
            SeedHeldKeys(); return;
        }
        const HWND foreground = GetForegroundWindow();
        const auto device = reinterpret_cast<std::uintptr_t>(input.header.hDevice);
        if(foreground != recent_foreground_ || (recent_device_ != 0 && recent_device_ != device)) {
#if LSHIFT_DIAGNOSTICS
            if(recent_.Snapshot().count != 0 || busy_.load())
                Trace(lshift::DiagnosticEvent::Reset, foreground != recent_foreground_ ?
                    lshift::DiagnosticReason::Foreground : lshift::DiagnosticReason::Device);
#endif
            recent_.Clear();
            recent_foreground_ = foreground;
            recent_device_ = device;
        }
        if((key.Flags & RI_KEY_BREAK) == 0 && vk != VK_LSHIFT && vk != VK_RSHIFT && vk != VK_CAPITAL) {
            last_edit_time_ = static_cast<DWORD>(now) - age;
        }
        if(settings_.enabled) recent_.Feed(vk, (key.Flags & RI_KEY_BREAK) == 0);
        else recent_.Clear();
        const bool triggered = detector_.Feed({vk, (key.Flags & RI_KEY_BREAK) == 0, now - age,
                                              reinterpret_cast<std::uintptr_t>(foreground),
                                              reinterpret_cast<std::uintptr_t>(input.header.hDevice)});
        if(!triggered || !foreground) return;
        if(busy_.load()) {
            Trace(lshift::DiagnosticEvent::Gesture, lshift::DiagnosticReason::Busy);
            return;
        }
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
        std::unique_lock lock(jobs_mutex_, std::try_to_lock);
        if(!lock || job_) {
            Trace(lshift::DiagnosticEvent::Gesture, lshift::DiagnosticReason::QueueBusy);
            return;
        }
        const DWORD thread = GetWindowThreadProcessId(foreground, nullptr);
        GUITHREADINFO info{sizeof(info)};
        const HWND focus = thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus ? info.hwndFocus : foreground;
        const auto layout = reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(GetWindowThreadProcessId(focus, nullptr)));
        job_ = Job{{foreground, now + 1500, window_, recent_.Snapshot(), layout, focus}, epoch};
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
            if(FAILED(*apartment)) result = {lshift::TextStatus::Failed, L"Не удалось подготовить текстовый доступ Windows."};
            else {
                try { result = lshift::ProcessText(current->request, valid); }
                catch(...) { result = {lshift::TextStatus::Failed, L"Текстовая операция отменена после ошибки."}; }
            }
#if LSHIFT_DIAGNOSTICS
            lshift::DiagnosticRecord recorded;
            recorded.event = lshift::DiagnosticEvent::Result;
            recorded.tick = GetTickCount64();
            recorded.epoch = current->epoch;
            recorded.count = static_cast<std::uint32_t>(current->request.recent_keys.count);
            recorded.complete = current->request.recent_keys.complete;
            recorded.layout = static_cast<std::uint32_t>(current->request.recent_layout);
            recorded.status = static_cast<std::uint32_t>(result.status);
            recorded.elapsed = static_cast<std::uint32_t>(recorded.tick - started);
            recorded.data = static_cast<std::uint32_t>(generation_.load());
            DrainDiagnostics(log);
            (void)log.Append(recorded, result.detail, diagnostics_.Dropped());
#endif
            {
                std::lock_guard lock(result_mutex_);
                result_ = std::move(result);
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
                    MessageBoxW(window_, L"Журнал пока недоступен. Путь: LOCALAPPDATA\\LShift\\diagnostics.jsonl",
                                L"LShift", MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
#endif
        if(command == lshift::kOpenSettings) { Invalidate(); lshift::ShowSettings(window_, settings_); return; }
        if(command == lshift::kToggleAutostart) {
            if(!lshift::SetAutostart(!lshift::AutostartEnabled()))
                MessageBoxW(window_, L"Не удалось изменить автозапуск текущего пользователя.", L"LShift", MB_OK | MB_ICONERROR);
            return;
        }
        if(command == lshift::kToggleEnabled) {
            auto next = settings_;
            next.enabled = !next.enabled;
            if(!lshift::SaveSettings(next)) {
                MessageBoxW(window_, L"Не удалось сохранить настройки.", L"LShift", MB_OK | MB_ICONERROR);
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
            << ",\"worker_initialized\":" << (worker_started_.load() ? "true" : "false")
            << ",\"com_initialized\":" << (com_initialized_.load() ? "true" : "false")
            << ",\"diagnostics_enabled\":" << (LSHIFT_DIAGNOSTICS ? "true" : "false")
            << ",\"gesture_actions_disabled\":true}\n";
    }
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        if(taskbar_created_ && message == taskbar_created_) {
            tray_added_ = lshift::AddTray(window_, Icon(), settings_.enabled);
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
                lshift::SetTrayStatus(window_, result_->detail);
                result_.reset();
            }
            return 0;
        }
        case WM_TIMER:
            if(wparam == kSmokeTimer) { KillTimer(window_, kSmokeTimer); SmokeReport(); BeginStop(); }
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
    lshift::RecentTyping recent_;
#if LSHIFT_DIAGNOSTICS
    lshift::DiagnosticQueue diagnostics_;
#endif
    HWND recent_foreground_ = nullptr;
    std::uintptr_t recent_device_ = 0;
    DWORD last_edit_time_ = 0;
    HWINEVENTHOOK foreground_hook_ = nullptr;
    HWINEVENTHOOK focus_hook_ = nullptr;
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
        } else MessageBoxW(nullptr, L"Не удалось запустить LShift: проверьте доступ к трею и устройствам ввода.", L"LShift", MB_OK | MB_ICONERROR);
    } catch(...) { MessageBoxW(nullptr, L"Не удалось запустить LShift.", L"LShift", MB_OK | MB_ICONERROR); }
    CloseHandle(mutex);
    return result;
}
