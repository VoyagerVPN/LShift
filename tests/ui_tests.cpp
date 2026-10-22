#include "shell.hpp"

#include <objbase.h>
#include <oleauto.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t OwnerClass[] = L"LShift.SettingsUiTests.Owner";
constexpr wchar_t SettingsClass[] = L"LShift.Settings.1";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr int EnabledControl = 2001;
constexpr int IntervalControl = 2002;
constexpr int HoldControl = 2003;
constexpr int AutostartControl = 2004;

void Require(bool condition, const char* detail) {
    if (!condition) throw std::runtime_error(detail);
}

void Check(HRESULT status, const char* detail) {
    if (FAILED(status)) throw std::runtime_error(std::string(detail) +
                                                " (HRESULT " + std::to_string(status) + ")");
}

class Apartment {
public:
    Apartment() { Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization"); }
    ~Apartment() { CoUninitialize(); }
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

void PumpMessages() {
    MSG message{};
    unsigned count = 0;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        Require(message.message != WM_QUIT, "unexpected WM_QUIT");
        TranslateMessage(&message);
        DispatchMessageW(&message);
        Require(++count < 1000, "unexpected message loop activity");
    }
}

[[nodiscard]] LRESULT CALLBACK OwnerProcedure(HWND window, UINT message,
                                              WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

struct FindSettings {
    HWND owner = nullptr;
    HWND found = nullptr;
};

[[nodiscard]] BOOL CALLBACK FindOwnedSettings(HWND window, LPARAM parameter) {
    auto& state = *reinterpret_cast<FindSettings*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(window, GW_OWNER) != state.owner) return TRUE;
    wchar_t name[128]{};
    if (GetClassNameW(window, name, static_cast<int>(std::size(name))) > 0 &&
        std::wstring_view(name) == SettingsClass) {
        state.found = window;
        return FALSE;
    }
    return TRUE;
}

class Fixture {
public:
    Fixture() : previous_(GetForegroundWindow()) {}
    ~Fixture() {
        Cancel();
        if (owner_ != nullptr) DestroyWindow(owner_);
        if (registered_) UnregisterClassW(OwnerClass, GetModuleHandleW(nullptr));
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void Open(const lshift::Settings& settings) {
        WNDCLASSW type{};
        type.lpfnWndProc = OwnerProcedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = OwnerClass;
        Require(RegisterClassW(&type) != 0, "register hidden test owner");
        registered_ = true;
        owner_ = CreateWindowExW(0, OwnerClass, L"LShift settings test owner", WS_OVERLAPPED,
            0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
        Require(owner_ != nullptr && !IsWindowVisible(owner_), "create hidden test owner");
        lshift::ShowSettings(owner_, settings);
        FindSettings state{owner_, nullptr};
        EnumThreadWindows(GetCurrentThreadId(), FindOwnedSettings, reinterpret_cast<LPARAM>(&state));
        settings_ = state.found;
        Require(settings_ != nullptr && IsWindowVisible(settings_), "find our visible settings window");
        RedrawWindow(settings_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        PumpMessages();
    }

    void Cancel() noexcept {
        if (settings_ == nullptr || !IsWindow(settings_)) return;
        // Возвращаем прежний foreground только пока фокус принадлежит нашему окну.
        if (GetForegroundWindow() == settings_ && previous_ != nullptr && IsWindow(previous_)) {
            SetForegroundWindow(previous_);
        }
        SendMessageW(settings_, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED),
                     reinterpret_cast<LPARAM>(GetDlgItem(settings_, IDCANCEL)));
    }

    [[nodiscard]] HWND Window() const noexcept { return settings_; }

private:
    HWND previous_ = nullptr;
    HWND owner_ = nullptr;
    HWND settings_ = nullptr;
    bool registered_ = false;
};

[[nodiscard]] std::wstring WindowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    Require(length >= 0 && length <= 4096, "reasonable control text length");
    std::vector<wchar_t> text(static_cast<std::size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    Require(copied == length, "read control text");
    return std::wstring(text.data(), static_cast<std::size_t>(copied));
}

[[nodiscard]] HWND Control(HWND parent, int id, const wchar_t* expected_class) {
    const HWND control = GetDlgItem(parent, id);
    Require(control != nullptr && IsWindowVisible(control) && IsWindowEnabled(control),
            "required control must be visible and enabled");
    Require(GetAncestor(control, GA_ROOT) == parent, "control must belong to our settings window");
    wchar_t name[64]{};
    Require(GetClassNameW(control, name, static_cast<int>(std::size(name))) != 0 &&
            CompareStringOrdinal(name, -1, expected_class, -1, TRUE) == CSTR_EQUAL,
            "required control has the expected native class");
    RECT client{};
    RECT rectangle{};
    Require(GetClientRect(parent, &client) && GetWindowRect(control, &rectangle), "get control bounds");
    POINT corners[] = {{rectangle.left, rectangle.top}, {rectangle.right, rectangle.bottom}};
    MapWindowPoints(nullptr, parent, corners, 2);
    Require(corners[0].x >= 0 && corners[0].y >= 0 && corners[1].x <= client.right &&
            corners[1].y <= client.bottom && corners[1].x > corners[0].x &&
            corners[1].y > corners[0].y, "control must fit inside the settings client area");
    return control;
}

void CheckControls(HWND window, const lshift::Settings& settings, bool autostart) {
    if (WindowText(window) != L"Настройки LShift") {
        std::cerr << "Actual title UTF-16:";
        for (const wchar_t code : WindowText(window)) std::cerr << ' ' << std::hex << static_cast<unsigned>(code);
        std::cerr << std::dec << '\n';
    }
    Require(WindowText(window) == L"Настройки LShift", "settings window title");
    Require(GetClassLongPtrW(window, GCLP_HICON) != 0 && GetClassLongPtrW(window, GCLP_HICONSM) != 0,
            "settings window exposes the application icons to Windows");
    const HWND enabled = Control(window, EnabledControl, L"BUTTON");
    const HWND interval = Control(window, IntervalControl, L"EDIT");
    const HWND hold = Control(window, HoldControl, L"EDIT");
    const HWND startup = Control(window, AutostartControl, L"BUTTON");
    const HWND save = Control(window, IDOK, L"BUTTON");
    const HWND cancel = Control(window, IDCANCEL, L"BUTTON");
    Require(WindowText(enabled) == L"Включить двойной левый Shift", "enabled checkbox label");
    Require(WindowText(startup) == L"Запускать LShift при входе в Windows", "autostart checkbox label");
    Require(WindowText(save) == L"Сохранить" && WindowText(cancel) == L"Отмена", "Save and Cancel buttons");
    Require(WindowText(interval) == std::to_wstring(settings.double_tap_ms), "double-tap interval value");
    Require(WindowText(hold) == std::to_wstring(settings.hold_ms), "hold duration value");
    Require(SendMessageW(enabled, BM_GETCHECK, 0, 0) == (settings.enabled ? BST_CHECKED : BST_UNCHECKED),
            "enabled checkbox value");
    Require(SendMessageW(startup, BM_GETCHECK, 0, 0) == (autostart ? BST_CHECKED : BST_UNCHECKED),
            "autostart checkbox reflects the read-only registry query");
}

struct Snapshot {
    bool exists = false;
    DWORD type = 0;
    std::vector<BYTE> bytes;
    bool operator==(const Snapshot&) const = default;
};

[[nodiscard]] Snapshot RunSnapshot() {
    Snapshot result;
    DWORD size = 0;
    constexpr DWORD flags = RRF_RT_ANY | RRF_NOEXPAND;
    const LSTATUS first = RegGetValueW(HKEY_CURRENT_USER, RunKey, L"LShift", flags,
                                       &result.type, nullptr, &size);
    if (first == ERROR_FILE_NOT_FOUND || first == ERROR_PATH_NOT_FOUND) return {};
    Require(first == ERROR_SUCCESS && size <= 65536, "read-only Run value query");
    result.exists = true;
    result.bytes.resize(size);
    Require(RegGetValueW(HKEY_CURRENT_USER, RunKey, L"LShift", flags, &result.type,
                         result.bytes.data(), &size) == ERROR_SUCCESS, "read-only Run value snapshot");
    result.bytes.resize(size);
    return result;
}

[[nodiscard]] Snapshot SettingsSnapshot(const std::filesystem::path& path) {
    Snapshot result;
    if (!std::filesystem::exists(path)) return result;
    Require(std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) <= 1048576,
            "reasonable settings fixture file");
    std::ifstream file(path, std::ios::binary);
    Require(static_cast<bool>(file), "open settings for read-only snapshot");
    const std::vector<char> data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Require(!file.bad(), "read settings snapshot");
    result.exists = true;
    result.bytes.assign(data.begin(), data.end());
    return result;
}

struct ClientDc {
    HWND window = nullptr;
    HDC value = nullptr;
    ~ClientDc() { if (value != nullptr) ReleaseDC(window, value); }
};
struct MemoryDc {
    HDC value = nullptr;
    ~MemoryDc() { if (value != nullptr) DeleteDC(value); }
};
struct Bitmap {
    HBITMAP value = nullptr;
    ~Bitmap() { if (value != nullptr) DeleteObject(value); }
};
struct SelectedBitmap {
    HDC dc = nullptr;
    HGDIOBJ original = nullptr;
    ~SelectedBitmap() { if (original != nullptr && original != HGDI_ERROR) SelectObject(dc, original); }
};

[[nodiscard]] bool HasImageContent(const void* data, std::size_t count) noexcept {
    const auto* pixels = static_cast<const std::uint32_t*>(data);
    if (count == 0) return false;
    const std::uint32_t first = pixels[0] & 0x00FFFFFFU;
    for (std::size_t index = 1; index < count; ++index) {
        if ((pixels[index] & 0x00FFFFFFU) != first) return true;
    }
    return false;
}

void SavePng(const std::filesystem::path& path, UINT width, UINT height, UINT stride, BYTE* data,
             UINT bytes, UINT dpi) {
    ComPtr<IWICImagingFactory> factory;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&factory)), "create WIC factory");
    ComPtr<IWICBitmap> source;
    Check(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR,
                                          stride, bytes, data, &source), "wrap captured bitmap");
    ComPtr<IWICFormatConverter> converted;
    Check(factory->CreateFormatConverter(&converted), "create native pixel format converter");
    // GDI не определяет alpha: сохраняем исходные RGB-пиксели как непрозрачный PNG.
    Check(converted->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeCustom), "convert capture storage format");
    ComPtr<IWICStream> stream;
    Check(factory->CreateStream(&stream), "create PNG stream");
    Check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open PNG output");
    ComPtr<IWICBitmapEncoder> encoder;
    Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "create PNG encoder");
    Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize PNG encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    Check(encoder->CreateNewFrame(&frame, nullptr), "create PNG frame");
    Check(frame->Initialize(nullptr), "initialize PNG frame");
    Check(frame->SetSize(width, height), "set PNG dimensions");
    Check(frame->SetResolution(static_cast<double>(dpi), static_cast<double>(dpi)), "set PNG DPI");
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    Check(frame->SetPixelFormat(&format), "set PNG pixel format");
    Require(IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) != FALSE, "preserve captured RGB format");
    Check(frame->WriteSource(converted.Get(), nullptr), "encode actual client capture");
    Check(frame->Commit(), "commit PNG frame");
    Check(encoder->Commit(), "commit PNG file");
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    ComPtr<IWICBitmapDecoder> decoder;
    Check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad, &decoder), "decode saved PNG");
    GUID container{};
    Check(decoder->GetContainerFormat(&container), "read saved container format");
    Require(IsEqualGUID(container, GUID_ContainerFormatPng) != FALSE, "saved image must be PNG");
    ComPtr<IWICBitmapFrameDecode> decoded;
    Check(decoder->GetFrame(0, &decoded), "read saved PNG frame");
    UINT saved_width = 0;
    UINT saved_height = 0;
    Check(decoded->GetSize(&saved_width, &saved_height), "verify saved PNG dimensions");
    Require(saved_width == width && saved_height == height, "PNG must match the captured client area");
}

void CaptureClient(HWND window, const std::filesystem::path& output) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    Require(pid == GetCurrentProcessId(), "capture is restricted to our own window");
    RECT client{};
    Require(GetClientRect(window, &client) && client.left == 0 && client.top == 0 &&
            client.right > 0 && client.bottom > 0 && client.right <= 4096 && client.bottom <= 4096,
            "valid own client capture dimensions");
    const UINT width = static_cast<UINT>(client.right);
    const UINT height = static_cast<UINT>(client.bottom);
    const UINT stride = width * 4;
    const UINT bytes = stride * height;
    ClientDc source{window, GetDC(window)};
    Require(source.value != nullptr, "get own client DC");
    MemoryDc memory{CreateCompatibleDC(source.value)};
    Require(memory.value != nullptr, "create capture memory DC");
    BITMAPINFO info{};
    info.bmiHeader.biSize = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* data = nullptr;
    Bitmap bitmap{CreateDIBSection(source.value, &info, DIB_RGB_COLORS, &data, nullptr, 0)};
    Require(bitmap.value != nullptr && data != nullptr, "create actual capture bitmap");
    SelectedBitmap selected{memory.value, SelectObject(memory.value, bitmap.value)};
    Require(selected.original != nullptr && selected.original != HGDI_ERROR, "select capture bitmap");
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::fill_n(static_cast<std::uint32_t*>(data), pixels, 0U);
    const char* method = "PrintWindow";
    bool captured = PrintWindow(window, memory.value, PW_CLIENTONLY) != FALSE;
    GdiFlush();
    if (!captured || !HasImageContent(data, pixels)) {
        Require(GetForegroundWindow() == window, "client BitBlt requires our own foreground window");
        method = "BitBlt";
        captured = BitBlt(memory.value, 0, 0, static_cast<int>(width), static_cast<int>(height),
                          source.value, 0, 0, SRCCOPY) != FALSE;
        GdiFlush();
    }
    Require(captured && HasImageContent(data, pixels), "own client capture must contain rendered controls");
    const UINT dpi = GetDpiForWindow(window);
    Require(dpi != 0, "read capture window DPI");
    SavePng(output, width, height, stride, static_cast<BYTE*>(data), bytes, dpi);
    std::cout << "PASS: " << method << " captured own settings client " << width << 'x' << height
              << " at " << dpi << " DPI\n";
}

void CheckIsolatedProfile() {
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    Require(size > 0 && size < 32768, "test LOCALAPPDATA must be explicitly set");
    std::vector<wchar_t> text(size);
    const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", text.data(), size);
    Require(copied != 0 && copied < size, "read test LOCALAPPDATA");
    const auto expected = std::filesystem::absolute(L".cache/ui-test-profile").lexically_normal();
    const auto actual = std::filesystem::path(text.data()).lexically_normal();
    Require(actual.is_absolute() && CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL,
            "GUI test must use the isolated .cache/ui-test-profile");
    Require(lshift::SettingsPath() == actual / L"LShift" / L"settings.ini",
            "settings must stay inside the test profile");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        Require(argc == 2, "usage: ui_tests.exe <output.png>");
        // DPI задаётся до COM и создания любых тестовых HWND.
        const BOOL changed = SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        Require(changed || AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(),
                    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), "PerMonitorV2 DPI awareness");
        CheckIsolatedProfile();
        const auto output = std::filesystem::absolute(argv[1]).lexically_normal();
        Require(output.extension() == L".png", "capture output must be a PNG file");
        std::filesystem::create_directories(output.parent_path());
        const Snapshot run_before = RunSnapshot();
        const Snapshot settings_before = SettingsSnapshot(lshift::SettingsPath());
        const bool autostart = lshift::AutostartEnabled();
        Apartment apartment;
        Fixture fixture;
        const lshift::Settings values{true, 475, 200};
        fixture.Open(values);
        CheckControls(fixture.Window(), values, autostart);
        std::cout << "PASS: settings controls, supplied values, Save and Cancel are present\n";
        CaptureClient(fixture.Window(), output);
        const HWND closed = fixture.Window();
        fixture.Cancel();
        PumpMessages();
        Require(!IsWindow(closed), "Cancel must close our settings window");
        Require(RunSnapshot() == run_before, "Cancel must preserve the existing Run value");
        Require(SettingsSnapshot(lshift::SettingsPath()) == settings_before,
                "Cancel must preserve settings without creating or saving a file");
        std::cout << "PASS: Cancel closed the window; settings and Run are unchanged\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
