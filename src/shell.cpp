#include "shell.hpp"

#include <shellapi.h>
#include <array>
#include <memory>
#include <string_view>
#include <system_error>
#include <vector>

namespace lshift {
namespace {
constexpr GUID kTrayGuid{0x573b37d2, 0xd8a3, 0x466d,
                         {0x98, 0xdc, 0xb8, 0xc6, 0x3c, 0x70, 0xd2, 0x13}};
constexpr wchar_t kSettingsClass[] = L"LShift.Settings.1";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int kEnabledControl = 2001;
constexpr int kIntervalControl = 2002;
constexpr int kHoldControl = 2003;
constexpr int kAutostartControl = 2004;
constexpr UINT kMinInterval = 150;
constexpr UINT kMaxInterval = 800;
constexpr UINT kMinHold = 100;
constexpr UINT kMaxHold = 500;
HWND settings_window = nullptr;
HWND registered_tray_window = nullptr;
bool tray_menu_active = false;

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void Close() noexcept {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
struct Bitmap {
    HBITMAP value = nullptr;
    ~Bitmap() { if (value) DeleteObject(value); }
};
struct Menu {
    HMENU value = nullptr;
    ~Menu() { if (value) DestroyMenu(value); }
};

class MenuActivation {
public:
    explicit MenuActivation(HWND window) noexcept : window_(window) {}
    ~MenuActivation() {
        if (!changed_ || !IsWindow(window_)) return;
        SetWindowPos(window_, was_topmost_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                     original_.left, original_.top, original_.right - original_.left,
                     original_.bottom - original_.top,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | (was_visible_ ? 0 : SWP_HIDEWINDOW));
    }

    MenuActivation(const MenuActivation&) = delete;
    MenuActivation& operator=(const MenuActivation&) = delete;

    [[nodiscard]] bool Activate(POINT point) noexcept {
        if (!IsWindow(window_) || !GetWindowRect(window_, &original_)) return false;
        was_visible_ = IsWindowVisible(window_) != FALSE;
        was_topmost_ = (GetWindowLongPtrW(window_, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
        // Нулевой служебный HWND становится допустимым foreground без видимого окна или taskbar-кнопки.
        const UINT flags = SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                           (was_visible_ ? SWP_NOMOVE | SWP_NOSIZE : SWP_SHOWWINDOW);
        if (!SetWindowPos(window_, HWND_TOPMOST, point.x, point.y, 0, 0, flags)) return false;
        changed_ = true;
        SetForegroundWindow(window_);
        return GetForegroundWindow() == window_;
    }

private:
    HWND window_ = nullptr;
    RECT original_{};
    bool was_visible_ = false;
    bool was_topmost_ = false;
    bool changed_ = false;
};

struct MenuSession {
    MenuSession() { tray_menu_active = true; }
    ~MenuSession() { tray_menu_active = false; }
};

[[nodiscard]] bool ValidSettings(const Settings& settings) noexcept {
    return settings.double_tap_ms >= kMinInterval && settings.double_tap_ms <= kMaxInterval &&
           settings.hold_ms >= kMinHold && settings.hold_ms <= kMaxHold;
}

[[nodiscard]] std::optional<std::uint32_t> ParseNumber(std::wstring_view text,
                                                      std::uint32_t minimum,
                                                      std::uint32_t maximum) noexcept {
    if (text.empty()) return std::nullopt;
    std::uint32_t result = 0;
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') return std::nullopt;
        const auto digit = static_cast<std::uint32_t>(character - L'0');
        if (result > maximum / 10 || (result == maximum / 10 && digit > maximum % 10)) {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    if (result < minimum) return std::nullopt;
    return result;
}

[[nodiscard]] std::uint32_t ReadNumber(const std::filesystem::path& path, const wchar_t* key,
                                      std::uint32_t fallback, std::uint32_t minimum,
                                      std::uint32_t maximum) {
    std::array<wchar_t, 32> buffer{};
    const DWORD count = GetPrivateProfileStringW(L"Settings", key, L"", buffer.data(),
                                                static_cast<DWORD>(buffer.size()), path.c_str());
    if (count == 0 || count >= buffer.size() - 1) return fallback;
    return ParseNumber(std::wstring_view(buffer.data(), count), minimum, maximum).value_or(fallback);
}

[[nodiscard]] std::wstring AutostartCommand() {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0 || count >= buffer.size()) return {};
    return L"\"" + std::wstring(buffer.data(), count) + L"\" --autostart";
}

[[nodiscard]] NOTIFYICONDATAW TrayData(HWND window) noexcept {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_GUID;
    data.guidItem = kTrayGuid;
    return data;
}

void SetTrayContents(NOTIFYICONDATAW& data, HICON icon, bool enabled) {
    data.uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = icon;
    const wchar_t* tip = enabled ? L"LShift — двойной левый Shift включён"
                                 : L"LShift — работа приостановлена";
    lstrcpynW(data.szTip, tip, static_cast<int>(std::size(data.szTip)));
}

struct SettingsWindow {
    HWND owner = nullptr;
    bool owned_by_window = false;
    UINT dpi = USER_DEFAULT_SCREEN_DPI;
    HFONT font = nullptr;
    std::array<HWND, 9> controls{};
    ~SettingsWindow() { if (font) DeleteObject(font); }
};

[[nodiscard]] int Scaled(const SettingsWindow& state, int value) noexcept {
    return MulDiv(value, static_cast<int>(state.dpi), USER_DEFAULT_SCREEN_DPI);
}

void SetWindowFont(SettingsWindow& state) {
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    HFONT next = nullptr;
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, state.dpi)) {
        next = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!next) return;
    for (const HWND control : state.controls) {
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    }
    if (state.font) DeleteObject(state.font);
    state.font = next;
}

void PositionControls(SettingsWindow& state) {
    constexpr std::array<std::array<int, 4>, 9> rectangles{{
        {20, 18, 410, 24}, {20, 61, 315, 22}, {350, 57, 80, 27},
        {20, 105, 320, 38}, {350, 106, 80, 27}, {20, 158, 410, 24},
        {20, 197, 410, 38}, {240, 252, 90, 29}, {340, 252, 90, 29}
    }};
    for (std::size_t index = 0; index < state.controls.size(); ++index) {
        const auto& rectangle = rectangles[index];
        MoveWindow(state.controls[index], Scaled(state, rectangle[0]), Scaled(state, rectangle[1]),
                   Scaled(state, rectangle[2]), Scaled(state, rectangle[3]), TRUE);
    }
}

[[nodiscard]] HWND MakeControl(HWND parent, DWORD extended_style, const wchar_t* type,
                               const wchar_t* text, DWORD style, int id) {
    return CreateWindowExW(extended_style, type, text, WS_CHILD | WS_VISIBLE | style,
                           0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void FillSettings(HWND window, const Settings& settings) {
    CheckDlgButton(window, kEnabledControl, settings.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemInt(window, kIntervalControl, settings.double_tap_ms, FALSE);
    SetDlgItemInt(window, kHoldControl, settings.hold_ms, FALSE);
    CheckDlgButton(window, kAutostartControl, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
}

[[nodiscard]] bool CreateControls(HWND window, SettingsWindow& state) {
    state.controls = {
        MakeControl(window, 0, L"BUTTON", L"Включить двойной левый Shift",
                    BS_AUTOCHECKBOX | WS_TABSTOP, kEnabledControl),
        MakeControl(window, 0, L"STATIC", L"Интервал между нажатиями, мс (150–800)", 0, -1),
        MakeControl(window, WS_EX_CLIENTEDGE, L"EDIT", L"",
                    ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, kIntervalControl),
        MakeControl(window, 0, L"STATIC", L"Максимальная длительность нажатия, мс\n(100–500)", 0, -1),
        MakeControl(window, WS_EX_CLIENTEDGE, L"EDIT", L"",
                    ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, kHoldControl),
        MakeControl(window, 0, L"BUTTON", L"Запускать LShift при входе в Windows",
                    BS_AUTOCHECKBOX | WS_TABSTOP, kAutostartControl),
        MakeControl(window, 0, L"STATIC",
                    L"Исправляет последнее слово или выделение\nв доступном текстовом поле.", 0, -1),
        MakeControl(window, 0, L"BUTTON", L"Сохранить", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK),
        MakeControl(window, 0, L"BUTTON", L"Отмена", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL)
    };
    for (const HWND control : state.controls) {
        if (!control) return false;
    }
    SendDlgItemMessageW(window, kIntervalControl, EM_SETLIMITTEXT, 4, 0);
    SendDlgItemMessageW(window, kHoldControl, EM_SETLIMITTEXT, 4, 0);
    SetWindowFont(state);
    PositionControls(state);
    return true;
}

[[nodiscard]] std::optional<std::uint32_t> ReadControlNumber(HWND window, int control,
                                                           std::uint32_t minimum,
                                                           std::uint32_t maximum) {
    std::array<wchar_t, 32> buffer{};
    const int count = GetDlgItemTextW(window, control, buffer.data(), static_cast<int>(buffer.size()));
    if (count <= 0 || count >= static_cast<int>(buffer.size() - 1)) return std::nullopt;
    return ParseNumber(std::wstring_view(buffer.data(), static_cast<std::size_t>(count)),
                       minimum, maximum);
}

void SaveFromWindow(HWND window, const SettingsWindow& state) {
    const auto interval = ReadControlNumber(window, kIntervalControl, kMinInterval, kMaxInterval);
    const auto hold = ReadControlNumber(window, kHoldControl, kMinHold, kMaxHold);
    if (!interval || !hold) {
        MessageBoxW(window, L"Интервал должен быть от 150 до 800 мс, а длительность нажатия — "
                            L"от 100 до 500 мс.", L"Настройки LShift", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(window, interval ? kHoldControl : kIntervalControl));
        return;
    }
    const Settings selected{IsDlgButtonChecked(window, kEnabledControl) == BST_CHECKED,
                            *interval, *hold};
    if (!SaveSettings(selected)) {
        MessageBoxW(window, L"Не удалось сохранить настройки в папке LShift внутри LOCALAPPDATA. "
                            L"Проверьте доступ к папке и свободное место.",
                    L"Настройки LShift", MB_OK | MB_ICONERROR);
        return;
    }
    PostMessageW(state.owner, kSettingsChanged, 0, 0);
    const bool autostart = IsDlgButtonChecked(window, kAutostartControl) == BST_CHECKED;
    if (autostart != AutostartEnabled() && !SetAutostart(autostart)) {
        MessageBoxW(window, L"Настройки сохранены, но изменить автозапуск не удалось. "
                            L"Проверьте доступ к настройкам текущего пользователя. "
                            L"Для автозапуска путь программы вместе с параметром должен быть "
                            L"не длиннее 260 символов.",
                    L"Автозапуск LShift", MB_OK | MB_ICONERROR);
        CheckDlgButton(window, kAutostartControl, AutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
        return;
    }
    DestroyWindow(window);
}

LRESULT CALLBACK SettingsProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        auto* incoming = static_cast<SettingsWindow*>(creation->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(incoming));
        // Стандартная обработка сохраняет lpWindowName для заголовка и WM_GETTEXT.
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_CREATE:
        if (!state) return -1;
        state->dpi = GetDpiForWindow(window);
        return CreateControls(window, *state) ? 0 : -1;
    case WM_COMMAND:
        if (!state || HIWORD(wparam) != BN_CLICKED) break;
        if (LOWORD(wparam) == IDOK) {
            SaveFromWindow(window, *state);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_DPICHANGED:
        if (state) {
            state->dpi = HIWORD(wparam);
            const auto* rectangle = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, rectangle->left, rectangle->top,
                         rectangle->right - rectangle->left, rectangle->bottom - rectangle->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            SetWindowFont(*state);
            PositionControls(*state);
        }
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (settings_window == window) settings_window = nullptr;
        // До успеха CreateWindowEx состояние принадлежит вызывающей функции.
        if (state && state->owned_by_window) delete state;
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

std::filesystem::path SettingsPath() {
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (count == 0 || count > 32768) return {};
    std::vector<wchar_t> buffer(count);
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), count);
    if (copied == 0 || copied >= count) return {};
    const std::filesystem::path root(std::wstring(buffer.data(), copied));
    if (!root.is_absolute()) return {};
    return root / L"LShift" / L"settings.ini";
}

Settings LoadSettings() {
    Settings settings;
    try {
        const auto path = SettingsPath();
        if (path.empty()) return settings;
        settings.enabled = ReadNumber(path, L"Enabled", settings.enabled ? 1U : 0U, 0, 1) != 0;
        settings.double_tap_ms = ReadNumber(path, L"DoubleTapMs", settings.double_tap_ms,
                                             kMinInterval, kMaxInterval);
        settings.hold_ms = ReadNumber(path, L"HoldMs", settings.hold_ms, kMinHold, kMaxHold);
    } catch (const std::exception&) {
        return Settings{};
    }
    return settings;
}

bool SaveSettings(const Settings& settings) {
    if (!ValidSettings(settings)) return false;
    std::filesystem::path temporary;
    try {
        const auto path = SettingsPath();
        if (path.empty()) return false;
        std::filesystem::create_directories(path.parent_path());
        temporary = path.parent_path() /
            (L"settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L".tmp");
        const std::wstring contents = std::wstring(L"\xFEFF[Settings]\r\nEnabled=") +
            (settings.enabled ? L"1" : L"0") + L"\r\nDoubleTapMs=" +
            std::to_wstring(settings.double_tap_ms) + L"\r\nHoldMs=" +
            std::to_wstring(settings.hold_ms) + L"\r\n";
        FileHandle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.value == INVALID_HANDLE_VALUE) return false;
        const DWORD bytes = static_cast<DWORD>(contents.size() * sizeof(wchar_t));
        DWORD written = 0;
        const bool success = WriteFile(file.value, contents.data(), bytes, &written, nullptr) != FALSE &&
                             written == bytes && FlushFileBuffers(file.value) != FALSE;
        file.Close();
        // Полный файл заменяет прежний только после успешной записи всех настроек.
        if (success && MoveFileExW(temporary.c_str(), path.c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    } catch (const std::exception&) {
        // Ошибку отображает вызвавший интерфейс; публичная функция пригодна для фонового вызова.
    }
    if (!temporary.empty()) DeleteFileW(temporary.c_str());
    return false;
}

bool AutostartEnabled() {
    const std::wstring expected = AutostartCommand();
    if (expected.empty()) return false;
    std::array<wchar_t, 1024> buffer{};
    DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"LShift", RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
                     nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return false;
    if (bytes != (expected.size() + 1) * sizeof(wchar_t)) return false;
    return CompareStringOrdinal(buffer.data(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool SetAutostart(bool enabled) {
    RegistryKey key;
    if (!enabled) {
        const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                             KEY_SET_VALUE | KEY_QUERY_VALUE, &key.value);
        if (opened == ERROR_FILE_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) return false;
        const LSTATUS removed = RegDeleteValueW(key.value, L"LShift");
        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) return false;
        return RegQueryValueExW(key.value, L"LShift", nullptr, nullptr, nullptr, nullptr) ==
               ERROR_FILE_NOT_FOUND;
    }
    const std::wstring command = AutostartCommand();
    if (command.empty() || command.size() > 260) return false;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key.value, nullptr) != ERROR_SUCCESS) return false;
    const LSTATUS written = RegSetValueExW(key.value, L"LShift", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(command.c_str()),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    return written == ERROR_SUCCESS && AutostartEnabled();
}

HICON CreateTrayIcon(bool enabled) {
    constexpr int size = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    Bitmap image{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &data, nullptr, 0)};
    if (!image.value || !data) return nullptr;
    auto* pixels = static_cast<std::uint32_t*>(data);
    const std::uint32_t background = enabled ? 0xFF245E73U : 0xFF62676CU;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int corner_x = x < 6 ? 6 - x : (x > 25 ? x - 25 : 0);
            const int corner_y = y < 6 ? 6 - y : (y > 25 ? y - 25 : 0);
            const bool inside = corner_x * corner_x + corner_y * corner_y <= 36;
            const bool letter = (x >= 7 && x <= 10 && y >= 7 && y <= 24) ||
                                (x >= 7 && x <= 19 && y >= 21 && y <= 24);
            const bool arrow = (x >= 16 && x <= 26 && y >= 12 && y <= 14) ||
                               (x >= 22 && x <= 26 && (y == 13 - (26 - x) ||
                                                       y == 13 + (26 - x)));
            pixels[y * size + x] = inside ? (letter || arrow ? 0xFFFFFFFFU : background) : 0U;
        }
    }
    std::array<BYTE, size * size / 8> mask_bits{};
    Bitmap mask{CreateBitmap(size, size, 1, 1, mask_bits.data())};
    if (!mask.value) return nullptr;
    ICONINFO icon{};
    icon.fIcon = TRUE;
    icon.hbmColor = image.value;
    icon.hbmMask = mask.value;
    return CreateIconIndirect(&icon);
}

bool AddTray(HWND window, HICON icon, bool enabled) {
    if (!window || !icon) return false;
    auto data = TrayData(window);
    SetTrayContents(data, icon, enabled);
    // Повторное TaskbarCreated не всегда означает удаление прежнего значка.
    // Если он уже существует, обновляем тот же GUID вместо вечных повторов ADD.
    if (!Shell_NotifyIconW(NIM_ADD, &data) && !Shell_NotifyIconW(NIM_MODIFY, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &data)) {
        registered_tray_window = window;
        return true;
    }
    Shell_NotifyIconW(NIM_DELETE, &data);
    return false;
}

void UpdateTray(HWND window, HICON icon, bool enabled) {
    auto data = TrayData(window);
    SetTrayContents(data, icon, enabled);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void SetTrayStatus(HWND window, std::wstring_view detail) {
    auto data = TrayData(window);
    data.uFlags |= NIF_TIP | NIF_SHOWTIP;
    constexpr std::wstring_view prefix = L"LShift — ";
    constexpr std::size_t capacity = std::size(data.szTip) - 1;
    std::size_t position = 0;
    for (const wchar_t character : prefix) data.szTip[position++] = character;
    // Вызывающий код передаёт только статус команды, без содержимого текстового поля.
    for (const wchar_t character : detail) {
        if (position == capacity) break;
        data.szTip[position++] = character < L' ' ? L' ' : character;
    }
    if (position > prefix.size() && data.szTip[position - 1] >= L'\xD800' &&
        data.szTip[position - 1] <= L'\xDBFF') {
        --position;
    }
    data.szTip[position] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void RemoveTray(HWND window) {
    auto data = TrayData(window);
    Shell_NotifyIconW(NIM_DELETE, &data);
    if (registered_tray_window == window) registered_tray_window = nullptr;
}

void ShowTrayMenu(HWND window, bool enabled, POINT point) {
    if (tray_menu_active || !IsWindow(window)) return;
    MenuSession session;
    Menu menu{CreatePopupMenu()};
    if (!menu.value) return;
    AppendMenuW(menu.value, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED),
                 kToggleEnabled, L"Включено");
    AppendMenuW(menu.value, MF_STRING, kOpenSettings, L"Настройки…");
#if LSHIFT_DIAGNOSTICS
    AppendMenuW(menu.value, MF_STRING, kOpenDiagnostics, L"Открыть журнал диагностики");
#endif
    AppendMenuW(menu.value, MF_STRING | (AutostartEnabled() ? MF_CHECKED : MF_UNCHECKED),
                 kToggleAutostart, L"Запускать при входе в Windows");
    AppendMenuW(menu.value, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.value, MF_STRING, kExit, L"Выход");
    TPMPARAMS position{};
    position.cbSize = sizeof(position);
    bool exclude_icon = false;
    if (registered_tray_window == window) {
        NOTIFYICONIDENTIFIER identifier{};
        identifier.cbSize = sizeof(identifier);
        identifier.hWnd = window;
        identifier.uID = 1;
        identifier.guidItem = kTrayGuid;
        exclude_icon = SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &position.rcExclude));
    }
    if (point.x == -1 && point.y == -1) {
        if (exclude_icon) {
            point = {position.rcExclude.left, position.rcExclude.bottom};
        } else if (!GetCursorPos(&point)) {
            return;
        }
    }
    UINT selected = 0;
    {
        MenuActivation activation(window);
        if (!activation.Activate(point)) {
            if (registered_tray_window == window) {
                SetTrayStatus(window, L"Не удалось открыть меню. Повторите щелчок.");
            }
            return;
        }
        const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY | TPM_WORKAREA |
            (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
        selected = static_cast<UINT>(TrackPopupMenuEx(menu.value, flags, point.x, point.y,
                                                     window, exclude_icon ? &position : nullptr));
        // После внешнего клика не возвращаем фокус в Explorer поверх выбранного пользователем окна.
        if (selected == 0 && GetForegroundWindow() == window && registered_tray_window == window) {
            auto data = TrayData(window);
            Shell_NotifyIconW(NIM_SETFOCUS, &data);
        }
    }
    if (selected != 0) PostMessageW(window, WM_COMMAND, selected, 0);
    PostMessageW(window, WM_NULL, 0, 0);
}

void ShowSettings(HWND owner, const Settings& settings) {
    if (settings_window && IsWindow(settings_window)) {
        ShowWindow(settings_window, SW_RESTORE);
        SetForegroundWindow(settings_window);
        return;
    }
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = SettingsProcedure;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hIcon = LoadIconW(type.hInstance, MAKEINTRESOURCEW(101));
    type.hIconSm = static_cast<HICON>(LoadImageW(type.hInstance, MAKEINTRESOURCEW(101),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    type.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    type.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        MessageBoxW(owner, L"Не удалось подготовить окно настроек.", L"LShift", MB_OK | MB_ICONERROR);
        return;
    }
    auto state = std::make_unique<SettingsWindow>();
    state->dpi = GetDpiForWindow(owner);
    if (state->dpi == 0) state->dpi = USER_DEFAULT_SCREEN_DPI;
    RECT rectangle{0, 0, Scaled(*state, 450), Scaled(*state, 302)};
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    constexpr DWORD extended = WS_EX_CONTROLPARENT;
    AdjustWindowRectExForDpi(&rectangle, style, FALSE, extended, state->dpi);
    const HWND window = CreateWindowExW(extended, kSettingsClass, L"Настройки LShift", style,
        CW_USEDEFAULT, CW_USEDEFAULT, rectangle.right - rectangle.left, rectangle.bottom - rectangle.top,
        owner, nullptr, type.hInstance, state.get());
    if (!window) {
        MessageBoxW(owner, L"Не удалось открыть настройки.", L"LShift", MB_OK | MB_ICONERROR);
        return;
    }
    state->owner = owner;
    state->owned_by_window = true;
    state.release();
    settings_window = window;
    FillSettings(window, settings);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(GetDlgItem(window, kEnabledControl));
}

void CloseSettings() {
    if (settings_window && IsWindow(settings_window)) DestroyWindow(settings_window);
    settings_window = nullptr;
}
}
