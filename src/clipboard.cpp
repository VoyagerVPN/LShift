#include "clipboard.hpp"
#include <array>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <new>

namespace lshift {
namespace {
constexpr std::size_t MaxCharacters = 64U * 1024U;
// GlobalSize может включать округление allocation; длина до NUL ограничена отдельно.
constexpr SIZE_T MaxCopiedBytes = (MaxCharacters + 1U) * sizeof(wchar_t) + 64U;

[[nodiscard]] bool Fail(DWORD error) noexcept { SetLastError(error); return false; }

[[nodiscard]] bool OwnWindow(HWND owner) noexcept {
    DWORD pid = 0;
    return owner != nullptr && IsWindow(owner) &&
           GetWindowThreadProcessId(owner, &pid) != 0 && pid == GetCurrentProcessId();
}

[[nodiscard]] bool CopiedBy(DWORD expected_pid) noexcept {
    DWORD pid = 0;
    const HWND source = GetClipboardOwner();
    return source != nullptr && GetWindowThreadProcessId(source, &pid) != 0 &&
           pid == expected_pid;
}

class ClipboardLock {
public:
    explicit ClipboardLock(HWND owner) noexcept {
        for(unsigned attempt = 0; attempt < 5; ++attempt) {
            if(OpenClipboard(owner)) { open_ = true; break; }
            if(attempt != 4) Sleep(5);
        }
    }
    ~ClipboardLock() { (void)Close(); }
    ClipboardLock(const ClipboardLock&) = delete;
    ClipboardLock& operator=(const ClipboardLock&) = delete;
    [[nodiscard]] bool Open() const noexcept { return open_; }
    [[nodiscard]] bool Close() noexcept {
        if(!open_) return true;
        if(!CloseClipboard()) return false;
        open_ = false;
        return true;
    }
private:
    bool open_ = false;
};

class Memory {
public:
    ~Memory() { if(handle_ != nullptr) GlobalFree(handle_); }
    Memory() = default;
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    [[nodiscard]] bool Assign(const void* bytes, SIZE_T count) noexcept {
        handle_ = GlobalAlloc(GMEM_MOVEABLE, count);
        if(handle_ == nullptr) return false;
        void* target = GlobalLock(handle_);
        if(target == nullptr) return false;
        std::memcpy(target, bytes, count);
        GlobalUnlock(handle_);
        return true;
    }
    [[nodiscard]] bool AssignText(std::wstring_view text) noexcept {
        handle_ = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1U) * sizeof(wchar_t));
        if(handle_ == nullptr) return false;
        auto* target = static_cast<wchar_t*>(GlobalLock(handle_));
        if(target == nullptr) return false;
        if(!text.empty()) std::memcpy(target, text.data(), text.size() * sizeof(wchar_t));
        target[text.size()] = L'\0';
        GlobalUnlock(handle_);
        return true;
    }
    [[nodiscard]] bool Publish(UINT format) noexcept {
        if(SetClipboardData(format, handle_) == nullptr) return false;
        handle_ = nullptr;
        return true;
    }
private:
    HGLOBAL handle_ = nullptr;
};

class LockedText {
public:
    explicit LockedText(HGLOBAL handle) noexcept
        : handle_(handle), data_(static_cast<const wchar_t*>(GlobalLock(handle))) {}
    ~LockedText() { if(data_ != nullptr) GlobalUnlock(handle_); }
    LockedText(const LockedText&) = delete;
    LockedText& operator=(const LockedText&) = delete;
    [[nodiscard]] const wchar_t* Data() const noexcept { return data_; }
private:
    HGLOBAL handle_;
    const wchar_t* data_;
};
} // namespace

bool WriteClipboardText(HWND owner, std::wstring_view text) noexcept {
    if(!OwnWindow(owner)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
    if(text.size() > MaxCharacters || text.find(L'\0') != std::wstring_view::npos)
        return Fail(ERROR_INVALID_DATA);

    constexpr std::array names{L"ExcludeClipboardContentFromMonitorProcessing",
                               L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"};
    std::array<UINT, names.size()> formats{};
    std::array<Memory, names.size()> policies;
    const DWORD disabled = 0;
    for(std::size_t index = 0; index < names.size(); ++index) {
        formats[index] = RegisterClipboardFormatW(names[index]);
        if(formats[index] == 0 || !policies[index].Assign(&disabled, sizeof(disabled))) return false;
    }
    Memory payload;
    if(!payload.AssignText(text)) return false;

    // Все блоки готовы до очистки. Повторные открытия ограничены и выполняются только worker-потоком.
    ClipboardLock clipboard(owner);
    if(!clipboard.Open() || !EmptyClipboard()) return false;
    for(std::size_t index = 0; index < policies.size(); ++index)
        if(!policies[index].Publish(formats[index])) return false;
    if(!payload.Publish(CF_UNICODETEXT)) return false;
    // Sequence number проверяется вызывающим кодом после CloseClipboard.
    return clipboard.Close();
}

bool ReadCopiedText(HWND owner, DWORD expected_pid, DWORD previous_sequence,
                    std::wstring& text) noexcept {
    text.clear();
    if(!OwnWindow(owner)) return Fail(ERROR_INVALID_WINDOW_HANDLE);
    if(expected_pid == 0) return Fail(ERROR_INVALID_PARAMETER);
    ClipboardLock clipboard(owner);
    if(!clipboard.Open()) return false;
    const DWORD sequence = GetClipboardSequenceNumber();
    if(sequence == 0 || sequence == previous_sequence || !CopiedBy(expected_pid))
        return Fail(ERROR_RETRY);
    const HGLOBAL handle = GetClipboardData(CF_UNICODETEXT);
    if(handle == nullptr) return false;
    const SIZE_T bytes = GlobalSize(handle);
    if(bytes < sizeof(wchar_t) || bytes > MaxCopiedBytes || bytes % sizeof(wchar_t) != 0)
        return Fail(ERROR_INVALID_DATA);
    try {
        std::wstring copied;
        {
            LockedText memory(handle);
            if(memory.Data() == nullptr) return false;
            const wchar_t* end = std::wmemchr(memory.Data(), L'\0', bytes / sizeof(wchar_t));
            if(end == nullptr || static_cast<std::size_t>(end - memory.Data()) > MaxCharacters)
                return Fail(ERROR_INVALID_DATA);
            copied.assign(memory.Data(), static_cast<std::size_t>(end - memory.Data()));
        }
        // Delayed rendering может изменить sequence; источник и свежесть проверяем повторно под lock.
        const DWORD rendered_sequence = GetClipboardSequenceNumber();
        if(rendered_sequence == 0 || rendered_sequence == previous_sequence || !CopiedBy(expected_pid))
            return Fail(ERROR_RETRY);
        if(!clipboard.Close()) return false;
        text.swap(copied);
        return true;
    } catch(const std::bad_alloc&) {
        return Fail(ERROR_NOT_ENOUGH_MEMORY);
    } catch(...) {
        return Fail(ERROR_INVALID_DATA);
    }
}
} // namespace lshift
