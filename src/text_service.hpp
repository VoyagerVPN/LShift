#pragma once
#include "core.hpp"
#include "recent_input.hpp"
#include <windows.h>
#include <functional>
#include <string>

namespace lshift {
struct TextRequest {
    HWND foreground = nullptr;
    std::uint64_t deadline_ms = 0;
    HWND clipboard_owner = nullptr;
    RecentKeys recent_keys{};
    std::uintptr_t recent_layout = 0;
    HWND focus = nullptr;
};
enum class TextStatus { Converted, Switched, Unsupported, Stale, Failed, Partial, Submitted };
struct TextResult {
    TextStatus status = TextStatus::Unsupported;
    std::wstring detail;
};
// Вызывается только на COM MTA worker; предикат запрещает устаревшие операции.
[[nodiscard]] TextResult ProcessText(const TextRequest& request,
                                     const std::function<bool()>& is_current);
}
