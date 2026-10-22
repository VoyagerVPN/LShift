#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace lshift {
struct RecordedKey {
    std::uint16_t key = 0;
    bool shift = false;
    bool caps = false;
};
struct RecentKeys {
    std::array<RecordedKey, 128> keys{};
    std::size_t count = 0;
    // false после потери начала: такой snapshot нельзя использовать для удаления текста.
    bool complete = true;
    // Текущее состояние для отправки замены; может отличаться от Caps у последней буквы.
    bool caps_active = false;
};
class RecentTyping {
public:
    // Физические VK, включая повторные make. Unicode восстанавливается только после жеста.
    void Feed(std::uint16_t key, bool down) noexcept;
    void Reset(bool caps = false) noexcept;
    [[nodiscard]] RecentKeys Snapshot() const noexcept;
    // Новый контекст задаёт вызывающий код; состояние удерживаемых клавиш и Caps сохраняется.
    void Clear() noexcept;
private:
    RecentKeys recent_{};
    std::array<bool, 256> held_{};
    bool caps_ = false;
    // Ожидание замены Ctrl+A либо нового набора после очистки этого выделения.
    bool replace_all_ = false;
};
}
