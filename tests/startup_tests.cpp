#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
void Require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
struct Desktop {
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK own = nullptr;
    std::wstring name;
    explicit Desktop(bool isolated) {
        if(!isolated) return;
        name = L"LShift.Startup.Tests." + std::to_wstring(GetCurrentProcessId());
        own = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        Require(own != nullptr, "create private desktop");
        Require(SetThreadDesktop(own) != FALSE, "select private test desktop without switching the display");
    }
    ~Desktop() { if(own) { SetThreadDesktop(original); CloseDesktop(own); } }
};
struct Child {
    PROCESS_INFORMATION info{};
    ~Child() {
        if(info.hProcess) {
            // Только дочерний тестовый EXE по собственному handle; ввод в нём отключён.
            if(WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 99);
                WaitForSingleObject(info.hProcess, 2000);
            }
            CloseHandle(info.hProcess);
        }
        if(info.hThread) CloseHandle(info.hThread);
    }
};
struct WindowSearch { DWORD pid; HWND result = nullptr; };
BOOL CALLBACK FindMain(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t name[64]{};
    if(pid == search.pid && GetClassNameW(window, name, 64) &&
       std::wstring_view(name) == L"LShift.Main.1") search.result = window;
    return TRUE;
}
bool NotificationServiceAvailable() {
    // Окно панели может существовать и в неинтерактивной CI-сессии.
    // Проверяем независимый значок до запуска приложения, иначе пропуск
    // мог бы скрыть дефект самого LShift.
    const wchar_t* name = L"LShift.Startup.NotificationProbe";
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = name;
    Require(RegisterClassW(&type) != 0, "register own notification probe class");
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, name, name, WS_OVERLAPPED,
        0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
    Require(window != nullptr, "create hidden notification probe window");
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = window;
    icon.uFlags = NIF_GUID | NIF_ICON | NIF_MESSAGE;
    icon.guidItem = {0x8d4c2871, 0xa113, 0x4d91, {0x82, 0x31, 0x75, 0x01, 0, 0, 0, 0}};
    const DWORD pid = GetCurrentProcessId();
    for(unsigned i = 0; i < 4; ++i) icon.guidItem.Data4[4 + i] = static_cast<BYTE>(pid >> (i * 8));
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    icon.uCallbackMessage = WM_APP + 1;
    bool available = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
    if(available) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        available = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
        Shell_NotifyIconW(NIM_DELETE, &icon);
    }
    DestroyWindow(window);
    UnregisterClassW(name, type.hInstance);
    return available;
}
}

int wmain(int count, wchar_t** arguments) {
    try {
        Require(count == 3, "expected app executable and --no-shell or --reannounce");
        const bool no_shell = std::wstring_view(arguments[2]) == L"--no-shell";
        Require(no_shell || std::wstring_view(arguments[2]) == L"--reannounce", "unknown startup scenario");
        const HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\LShift.App.1");
        if(existing) { CloseHandle(existing); std::cout << "SKIP: preserve running LShift\n"; return 77; }
        Require(GetLastError() == ERROR_FILE_NOT_FOUND, "inspect existing instance mutex");
        const Desktop desktop(no_shell);
        const HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr);
        if(no_shell) Require(shell == nullptr, "private desktop must have no Explorer taskbar");
        else if(!shell) { std::cout << "SKIP: no interactive shell for reannouncement\n"; return 77; }
        else if(!NotificationServiceAvailable()) {
            std::cout << "SKIP: independent notification probe is unavailable in this session\n";
            return 77;
        }

        const auto directory = std::filesystem::absolute(L".cache") /
            (L"startup-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
        const auto report = directory / L"smoke.json";
        const auto executable = std::filesystem::absolute(arguments[1]);
        std::wstring command = L"\"" + executable.wstring() + L"\" --autostart --smoke-test \"" + report.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        startup.lpDesktop = no_shell ? const_cast<wchar_t*>(desktop.name.c_str()) : nullptr;
        Child child;
        Require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &child.info) != FALSE, "launch own smoke child");
        if(!no_shell) {
            WindowSearch search{child.info.dwProcessId};
            const auto until = GetTickCount64() + 700;
            do {
                EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), FindMain, reinterpret_cast<LPARAM>(&search));
                if(search.result) break;
                Sleep(5);
            } while(GetTickCount64() < until);
            Require(search.result != nullptr, "find own child message window");
            Sleep(100);
            const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
            Require(message != 0 && PostMessageW(search.result, message, 0, 0), "reannounce taskbar only to test child");
        }
        const DWORD waited = WaitForSingleObject(child.info.hProcess, 3500);
        Require(waited == WAIT_OBJECT_0, "startup must reach the smoke report even without Explorer");
        DWORD exit_code = 99;
        Require(GetExitCodeProcess(child.info.hProcess, &exit_code) && exit_code == 0, "smoke child exits normally");
        std::ifstream input(report);
        const std::string result((std::istreambuf_iterator<char>(input)), {});
        Require(result.find("\"raw_input_registered\":true") != std::string::npos, "Raw Input initialized");
        Require(result.find("\"worker_initialized\":true") != std::string::npos, "worker initialized");
        Require(result.find("\"gesture_actions_disabled\":true") != std::string::npos, "test child cannot edit user text");
        Require(result.find(no_shell ? "\"tray_registered\":false" : "\"tray_registered\":true") != std::string::npos,
            "tray state matches shell availability");
        Require(result.find(no_shell ? "\"tray_retry_pending\":true" : "\"tray_retry_pending\":false") != std::string::npos,
            "retry waits only while the tray is unavailable");
        std::cout << "PASS: " << (no_shell ? "startup survives missing Explorer" : "tray survives repeated taskbar notification") << '\n';
        return 0;
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
