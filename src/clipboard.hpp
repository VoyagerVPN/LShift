#pragma once
#include <windows.h>
#include <string>
#include <string_view>

namespace lshift {
// owner — собственное живое окно. Запись остаётся в буфере без восстановления.
[[nodiscard]] bool WriteClipboardText(HWND owner, std::wstring_view text) noexcept;
// Принимает только новую Unicode-копию от окна процесса expected_pid.
[[nodiscard]] bool ReadCopiedText(HWND owner, DWORD expected_pid, DWORD previous_sequence,
                                  std::wstring& text) noexcept;
}
