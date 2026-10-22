#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lshift {
struct Settings {
    bool enabled = true;
    std::uint32_t double_tap_ms = 350;
    std::uint32_t hold_ms = 250;
};
struct KeyEvent {
    std::uint16_t key = 0;
    bool down = false;
    std::uint64_t time_ms = 0;
    std::uintptr_t context = 0;
    std::uintptr_t device = 0;
};
class Gesture {
public:
    explicit Gesture(Settings settings = {}) noexcept;
    [[nodiscard]] bool Feed(const KeyEvent& event) noexcept;
    void Reset() noexcept;
    void SetSettings(Settings settings) noexcept;
    void SetHeld(std::uint16_t key, bool held) noexcept;
private:
    Settings settings_;
    std::array<bool, 256> held_{};
    struct DeviceKeys {
        std::uintptr_t id = 0;
        bool used = false;
        std::array<bool, 256> held{};
    };
    std::array<DeviceKeys, 8> devices_{};
    std::array<std::uint8_t, 256> held_counts_{};
    unsigned stage_ = 0;
    std::uint64_t pressed_at_ = 0;
    std::uint64_t released_at_ = 0;
    std::uintptr_t context_ = 0;
    std::uintptr_t device_ = 0;
};
enum class Direction { ToRussian, ToEnglish };
struct WordConversion {
    std::wstring text;
    std::optional<Direction> last_direction;
};
struct TextSpan { std::size_t start = 0; std::size_t length = 0; };
[[nodiscard]] std::wstring Convert(std::wstring_view text, Direction direction);
[[nodiscard]] WordConversion ConvertWords(std::wstring_view text);
[[nodiscard]] std::optional<Direction> DetectDirection(std::wstring_view text) noexcept;
[[nodiscard]] std::optional<TextSpan> LastWord(std::wstring_view text, std::size_t caret) noexcept;
}
