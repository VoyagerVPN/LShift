#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace lshift {
enum class DiagnosticEvent { Startup, Reset, Gesture, Result, FocusHint };
enum class DiagnosticReason { None, Foreground, Focus, Mouse, Device, OldInput,
    Settings, Journal, Accepted, Busy, HeldKey, OwnWindow, QueueBusy };
// Только служебные числа. Символы, VK набираемых букв и содержимое clipboard не сохраняются.
struct DiagnosticRecord {
    DiagnosticEvent event = DiagnosticEvent::Startup;
    DiagnosticReason reason = DiagnosticReason::None;
    std::uint64_t tick = 0;
    std::uint64_t source_tick = 0;
    std::uint64_t epoch = 0;
    std::uint32_t count = 0;
    std::uint32_t layout = 0;
    std::uint32_t elapsed = 0;
    std::uint32_t status = 0;
    std::uint32_t data = 0;
    bool complete = true;
    bool temporary_executable = false;
};
class DiagnosticQueue {
public:
    // Один producer в UI-потоке, один consumer на worker. Нет ожиданий, heap и файлового I/O.
    [[nodiscard]] bool Push(const DiagnosticRecord& record) noexcept;
    [[nodiscard]] bool Pop(DiagnosticRecord& record) noexcept;
    [[nodiscard]] std::uint64_t Dropped() const noexcept { return dropped_.load(); }
    [[nodiscard]] bool Pending() const noexcept { return read_.load() != write_.load(); }
private:
    std::array<DiagnosticRecord, 128> records_{};
    std::atomic<std::size_t> write_{0};
    std::atomic<std::size_t> read_{0};
    std::atomic<std::uint64_t> dropped_{0};
};
class DiagnosticLog {
public:
    explicit DiagnosticLog(std::filesystem::path path, std::uintmax_t limit = 512 * 1024);
    // message — только фиксированная служебная detail из TextResult, никогда текст редактора.
    [[nodiscard]] bool Append(const DiagnosticRecord& record, std::wstring_view message = {},
                              std::uint64_t dropped = 0) noexcept;
private:
    std::filesystem::path path_;
    std::uintmax_t limit_;
};
}
