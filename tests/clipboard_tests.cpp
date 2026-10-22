#include "clipboard.hpp"
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}

[[noreturn]] void Win32Failure(const char* operation) {
    // Сохраняем код до allocations, cleanup и любых следующих Win32 вызовов.
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(operation) + " (Win32 error " +
                             std::to_string(error) + ")");
}

[[nodiscard]] std::wstring ObjectName(HANDLE object) {
    wchar_t name[256]{};
    DWORD required = 0;
    if(!GetUserObjectInformationW(object, UOI_NAME, name,
                static_cast<DWORD>(sizeof(name)), &required)) Win32Failure("read USER object name");
    return name;
}

class PrivateStation {
public:
    PrivateStation() {
        original_station_ = GetProcessWindowStation();
        original_desktop_ = GetThreadDesktop(GetCurrentThreadId());
        const std::wstring previous_name = ObjectName(original_station_);
        try {
            // NULL даёт имя из logon session и конфликтует с CWF_CREATE_ONLY при повторном запуске.
            const std::wstring unique = L"LShift.Tests." + std::to_wstring(GetCurrentProcessId()) +
                                        L"." + std::to_wstring(GetTickCount64());
            constexpr ACCESS_MASK access = WINSTA_CREATEDESKTOP | WINSTA_ACCESSCLIPBOARD |
                                           WINSTA_ACCESSGLOBALATOMS | WINSTA_READATTRIBUTES;
            station_ = CreateWindowStationW(unique.c_str(), CWF_CREATE_ONLY, access, nullptr);
            if(station_ == nullptr) Win32Failure("CreateWindowStationW unique private station");
            if(!SetProcessWindowStation(station_)) Win32Failure("SetProcessWindowStation");
            const auto name = ObjectName(GetProcessWindowStation());
            Require(name != previous_name && name != L"WinSta0", "clipboard station is not isolated");
            USEROBJECTFLAGS flags{};
            DWORD needed = 0;
            if(!GetUserObjectInformationW(station_, UOI_FLAGS, &flags,
                        static_cast<DWORD>(sizeof(flags)), &needed)) Win32Failure("read private station flags");
            Require((flags.dwFlags & WSF_VISIBLE) == 0, "private station must be noninteractive");
            desktop_ = CreateDesktopW(L"LShiftClipboardTests", nullptr, nullptr, 0,
                DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS, nullptr);
            if(desktop_ == nullptr) Win32Failure("CreateDesktopW private station");
            if(!SetThreadDesktop(desktop_)) Win32Failure("SetThreadDesktop before clipboard windows");
        } catch(...) { Close(); throw; }
    }
    PrivateStation(const PrivateStation&) = delete;
    PrivateStation& operator=(const PrivateStation&) = delete;
    ~PrivateStation() { Close(); }
private:
    void Close() noexcept {
        if(original_station_ != nullptr) SetProcessWindowStation(original_station_);
        if(original_desktop_ != nullptr) SetThreadDesktop(original_desktop_);
        if(desktop_ != nullptr) { CloseDesktop(desktop_); desktop_ = nullptr; }
        if(station_ != nullptr) { CloseWindowStation(station_); station_ = nullptr; }
    }
    HWINSTA original_station_ = nullptr;
    HDESK original_desktop_ = nullptr;
    HWINSTA station_ = nullptr;
    HDESK desktop_ = nullptr;
};

class Owner {
public:
    Owner() {
        window_ = CreateWindowExW(0, L"STATIC", L"Private clipboard tests", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window_ != nullptr, "clipboard owner window creation");
    }
    ~Owner() { if(window_ != nullptr) DestroyWindow(window_); }
    Owner(const Owner&) = delete;
    Owner& operator=(const Owner&) = delete;
    [[nodiscard]] HWND Get() const noexcept { return window_; }
private:
    HWND window_ = nullptr;
};

class Opened {
public:
    explicit Opened(HWND owner) { Require(OpenClipboard(owner) != FALSE, "open private clipboard"); }
    ~Opened() { CloseClipboard(); }
    Opened(const Opened&) = delete;
    Opened& operator=(const Opened&) = delete;
};

void PutRaw(HWND owner, std::size_t bytes, bool terminated) {
    Opened lock(owner);
    Require(EmptyClipboard() != FALSE, "empty isolated clipboard");
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    Require(memory != nullptr, "allocate raw clipboard payload");
    void* data = GlobalLock(memory);
    if(data == nullptr) { GlobalFree(memory); throw std::runtime_error("lock raw clipboard payload"); }
    const SIZE_T allocated = GlobalSize(memory);
    std::memset(data, 0x41, allocated);
    // Заполняем и проверяем также padding: иначе fixture случайно публикует корректный NUL.
    Require(allocated >= bytes && allocated % sizeof(wchar_t) == 0, "raw allocation size is valid");
    Require(std::wmemchr(static_cast<const wchar_t*>(data), L'\0', allocated / sizeof(wchar_t)) == nullptr,
            "unterminated fixture contains no NUL before publication");
    if(terminated && allocated >= sizeof(wchar_t)) {
        auto* characters = static_cast<wchar_t*>(data);
        characters[allocated / sizeof(wchar_t) - 1] = L'\0';
    }
    GlobalUnlock(memory);
    if(SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        throw std::runtime_error("publish raw clipboard payload");
    }
}

[[nodiscard]] std::vector<std::byte> ReadBytes(HWND owner, UINT format) {
    Opened lock(owner);
    const HANDLE data = GetClipboardData(format);
    Require(data != nullptr, "read clipboard format");
    const SIZE_T size = GlobalSize(data);
    const void* bytes = GlobalLock(data);
    Require(bytes != nullptr && size != 0, "read bounded format bytes");
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), bytes, size);
    GlobalUnlock(data);
    return result;
}

void CheckExclusions(HWND owner) {
    for(const wchar_t* name : {L"ExcludeClipboardContentFromMonitorProcessing",
                              L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
        const UINT format = RegisterClipboardFormatW(name);
        Require(format != 0, "register exclusion format");
        const auto bytes = ReadBytes(owner, format);
        DWORD value = 1;
        Require(bytes.size() >= sizeof(value), "exclusion DWORD present");
        std::memcpy(&value, bytes.data(), sizeof(value));
        Require(value == 0, "clipboard history/cloud exclusion enabled");
    }
}
} // namespace

int main() {
    bool started = false;
    try {
        // Clipboard принадлежит window station. До подтверждённой изоляции эти API не вызываются.
        PrivateStation station;
        Owner sentinel_owner;
        Owner copied_owner;
        started = true;
        const HWND reader = sentinel_owner.Get();
        const HWND target = copied_owner.Get();
        const DWORD pid = GetCurrentProcessId();
        const DWORD wrong_pid = pid == 0xFFFFFFFFUL ? 0xFFFFFFFEUL : 0xFFFFFFFFUL;
        std::wstring actual;

        DWORD before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(reader, L"our sentinel"), "write own sentinel");
        Require(!lshift::ReadCopiedText(reader, wrong_pid, before, actual) && actual.empty(),
                "own sentinel is rejected for a different target PID");
        const DWORD sentinel_sequence = GetClipboardSequenceNumber();
        Require(!lshift::ReadCopiedText(reader, pid, sentinel_sequence, actual),
                "unchanged sequence is not a fresh copy");
        CheckExclusions(reader);
        std::cout << "PASS sentinel source PID, old sequence and exclusion markers\n";

        Require(lshift::WriteClipboardText(target, L"привет\nмир  \t"), "emulate target copy");
        Require(lshift::ReadCopiedText(reader, pid, sentinel_sequence, actual), "read actual target copy");
        Require(actual == L"привет\nмир  \t", "multiline text and trailing whitespace remain exact");
        Require(GetClipboardOwner() == target, "read does not take clipboard ownership");
        const DWORD copied_sequence = GetClipboardSequenceNumber();
        Require(!lshift::ReadCopiedText(reader, wrong_pid, sentinel_sequence, actual), "wrong target PID rejected");
        Require(!lshift::ReadCopiedText(reader, pid, copied_sequence, actual), "already observed copy rejected");
        std::cout << "PASS second HWND copy and exact Unicode content\n";

        const std::wstring long_text = L"начало " + std::wstring(4096, L'я') + L"\nконец  ";
        before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(target, long_text), "write text beyond former 4096 context cap");
        Require(lshift::ReadCopiedText(reader, pid, before, actual) && actual == long_text,
                "long copied selection is returned intact");
        before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(target, L""), "empty Unicode payload is valid");
        Require(lshift::ReadCopiedText(reader, pid, before, actual) && actual.empty(), "empty copy returned exactly");
        const std::wstring maximum_text(65536, L'я');
        before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(target, maximum_text), "write exact 64 Ki character limit");
        Require(lshift::ReadCopiedText(reader, pid, before, actual) && actual == maximum_text,
                "exact 64 Ki character limit survives allocation rounding");
        std::cout << "PASS long, empty and exact 64 Ki copied text\n";

        const DWORD unchanged = GetClipboardSequenceNumber();
        const std::wstring embedded(L"ab\0cd", 5);
        Require(!lshift::WriteClipboardText(reader, embedded), "write rejects embedded NUL");
        Require(!lshift::WriteClipboardText(reader, std::wstring(65537, L'a')), "write rejects more than 64 Ki characters");
        Require(!lshift::WriteClipboardText(nullptr, L"invalid owner"), "write requires own live window");
        Require(GetClipboardSequenceNumber() == unchanged, "invalid writes leave clipboard untouched");
        std::cout << "PASS invalid write inputs preserve clipboard\n";

        before = GetClipboardSequenceNumber();
        PutRaw(target, 64, false);
        bool normalized_terminator = false;
        std::size_t published_length = 0;
        {
            const auto bytes = ReadBytes(reader, CF_UNICODETEXT);
            std::size_t first_nul = bytes.size() / sizeof(wchar_t);
            for(std::size_t index = 0; index < bytes.size() / sizeof(wchar_t); ++index) {
                wchar_t value = 0;
                std::memcpy(&value, bytes.data() + index * sizeof(wchar_t), sizeof(value));
                if(value == L'\0') { first_nul = index; break; }
            }
            std::cout << "DIAG published unterminated fixture bytes=" << bytes.size()
                      << " first_nul=" << first_nul << '\n';
            normalized_terminator = first_nul < bytes.size() / sizeof(wchar_t);
            published_length = first_nul;
        }
        if(normalized_terminator) {
            // Если Windows добавила NUL, проверяем фактический валидный формат, а не исходный блок.
            Require(lshift::ReadCopiedText(reader, pid, before, actual) &&
                    actual == std::wstring(published_length, static_cast<wchar_t>(0x4141)),
                    "read returns the actual Unicode payload normalized by Windows");
            std::cout << "PASS Windows-normalized terminator; missing-NUL branch is not reachable through this fixture\n";
        } else {
            Require(!lshift::ReadCopiedText(reader, pid, before, actual), "read rejects missing bounded terminator");
        }
        before = GetClipboardSequenceNumber();
        PutRaw(target, (65536U + 1U) * sizeof(wchar_t) + 128U, true);
        Require(!lshift::ReadCopiedText(reader, pid, before, actual), "read rejects oversized global block");
        before = GetClipboardSequenceNumber();
        PutRaw(target, (65536U + 2U) * sizeof(wchar_t), true);
        Require(!lshift::ReadCopiedText(reader, pid, before, actual), "read rejects NUL after character limit within allocation slack");
        std::cout << "PASS copied-memory bounds and terminator validation\n";

        before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(reader, L"persistent converted payload"), "write bulk paste payload");
        Require(lshift::ReadCopiedText(target, pid, before, actual) && actual == L"persistent converted payload",
                "stateless write is not automatically restored");
        before = GetClipboardSequenceNumber();
        Require(lshift::WriteClipboardText(target, L"later user copy"), "emulate later copy");
        Require(lshift::ReadCopiedText(reader, pid, before, actual) && actual == L"later user copy",
                "no destructor can overwrite later clipboard content");
        std::cout << "PASS no automatic restoration and later copy preserved\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr << (started ? "FAIL: " : "NOT EXECUTED: isolated window station unavailable: ")
                  << error.what() << '\n';
        return started ? 1 : 77;
    }
}
