#include "diagnostics.hpp"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>

namespace lshift {
bool DiagnosticQueue::Push(const DiagnosticRecord& record) noexcept {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % records_.size();
    if(next == read_.load(std::memory_order_acquire)) { dropped_.fetch_add(1); return false; }
    records_[write] = record;
    write_.store(next, std::memory_order_release);
    return true;
}
bool DiagnosticQueue::Pop(DiagnosticRecord& record) noexcept {
    const auto read = read_.load(std::memory_order_relaxed);
    if(read == write_.load(std::memory_order_acquire)) return false;
    record = records_[read];
    read_.store((read + 1) % records_.size(), std::memory_order_release);
    return true;
}
DiagnosticLog::DiagnosticLog(std::filesystem::path path, std::uintmax_t limit)
    : path_(std::move(path)), limit_(limit) {}
bool DiagnosticLog::Append(const DiagnosticRecord& record, std::wstring_view message,
                           std::uint64_t dropped) noexcept {
    try {
        if(path_.empty()) return false;
        constexpr const char* events[]{"startup", "reset", "gesture", "result"};
        constexpr const char* reasons[]{"none", "foreground", "focus", "mouse", "device", "old_input",
            "settings", "journal", "accepted", "busy", "held_key", "own_window", "queue_busy"};
        const auto event = static_cast<std::size_t>(record.event);
        const auto reason = static_cast<std::size_t>(record.reason);
        if(event >= std::size(events) || reason >= std::size(reasons)) return false;
        std::string utf8;
        if(!message.empty()) {
            // Ограниченный служебный текст; содержимое редактора сюда не передаётся.
            if(message.size() > 512) message = message.substr(0, 512);
            const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message.data(),
                static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
            if(bytes <= 0) return false;
            utf8.resize(static_cast<std::size_t>(bytes));
            if(!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message.data(),
                static_cast<int>(message.size()), utf8.data(), bytes, nullptr, nullptr)) return false;
        }
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char timestamp[40]{};
        if(sprintf_s(timestamp, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth,
            utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds) < 0) return false;
        std::ostringstream line;
        line << "{\"utc\":\"" << timestamp << "\",\"version\":\"0.1.7\",\"event\":\"" << events[event]
            << "\",\"reason\":\"" << reasons[reason] << "\",\"tick\":" << record.tick
            << ",\"epoch\":" << record.epoch << ",\"recent_count\":" << record.count
            << ",\"complete\":" << (record.complete ? "true" : "false")
            << ",\"layout\":" << record.layout << ",\"elapsed_ms\":" << record.elapsed
            << ",\"status\":" << record.status << ",\"data\":" << record.data
            << ",\"temporary_executable\":" << (record.temporary_executable ? "true" : "false")
            << ",\"dropped\":" << dropped << ",\"detail\":\"";
        constexpr char hex[] = "0123456789abcdef";
        for(const unsigned char character : utf8) {
            if(character == '\n') line << "\\n";
            else if(character == '\r') line << "\\r";
            else if(character == '\t') line << "\\t";
            else if(character == '"' || character == '\\') line << '\\' << character;
            else if(character < 0x20) line << "\\u00" << hex[character >> 4] << hex[character & 15];
            else line << character;
        }
        line << "\"}\n";
        const auto contents = line.str();
        if(contents.size() > limit_) return false;
        std::filesystem::create_directories(path_.parent_path());
        if(std::filesystem::exists(path_) && std::filesystem::file_size(path_) + contents.size() > limit_) {
            auto backup = path_;
            backup += ".1";
            std::filesystem::remove(backup);
            std::filesystem::rename(path_, backup);
        }
        std::ofstream output(path_, std::ios::binary | std::ios::app);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        return output.good();
    } catch(...) { return false; }
}
}
