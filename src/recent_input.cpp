#include "recent_input.hpp"

namespace lshift {
namespace {
bool Printable(std::uint16_t key) noexcept {
    if((key >= 0x30 && key <= 0x39) || (key >= 0x41 && key <= 0x5A) || key == 0x20)
        return true;
    return (key >= 0xBA && key <= 0xC0) || (key >= 0xDB && key <= 0xDE) || key == 0xE2;
}
bool ShortcutModifier(std::uint16_t key) noexcept {
    return key == 0x11 || key == 0x12 || key == 0x5B || key == 0x5C ||
           (key >= 0xA2 && key <= 0xA5);
}
}
void RecentTyping::Feed(std::uint16_t key, bool down) noexcept {
    // Потерянное начало не восстанавливается от навигации или hotkey; нужна явная граница.
    const auto discard = [this] {
        const bool complete = recent_.complete;
        recent_ = {};
        recent_.complete = complete;
    };
    if(key >= held_.size()) { replace_all_ = false; discard(); return; }
    const bool was_held = held_[key];
    held_[key] = down;
    if(key == 0x14) {
        if(down && !was_held) caps_ = !caps_;
        return;
    }
    if(key == 0x10 || key == 0xA0 || key == 0xA1) return;
    if(ShortcutModifier(key)) {
        if(down) {
            discard();
            if(key != 0x11 && key != 0xA2 && key != 0xA3) replace_all_ = false;
        }
        return;
    }
    if(!down) return;
    const bool control = held_[0x11] || held_[0xA2] || held_[0xA3];
    const bool other_shortcut = held_[0x12] || held_[0x5B] || held_[0x5C] || held_[0xA4] || held_[0xA5];
    const bool shift = held_[0x10] || held_[0xA0] || held_[0xA1];
    if(key == 0x41 && control && !other_shortcut && !shift) {
        discard();
        replace_all_ = true;
        return;
    }
    if(replace_all_ && !other_shortcut && (key == 0x08 || key == 0x2E)) {
        Clear();
        // Повторные Backspace/Delete в уже очищенном поле не теряют новую границу.
        replace_all_ = true;
        return;
    }
    if(control || other_shortcut) {
        replace_all_ = false;
        discard();
        return;
    }
    if(key == 0x0D) { Clear(); return; }
    if(replace_all_) {
        if(Printable(key)) Clear();
        else replace_all_ = false;
    }
    if(key == 0x08) {
        if(recent_.complete && recent_.count != 0) {
            recent_.keys[--recent_.count] = {};
        } else {
            recent_ = {};
            recent_.complete = false;
        }
        return;
    }
    if(!Printable(key)) { discard(); return; }
    if(key == 0x20 && (!recent_.complete || recent_.count == recent_.keys.size())) Clear();
    if(!recent_.complete) return;
    if(recent_.count == recent_.keys.size()) {
        recent_ = {};
        recent_.complete = false;
        return;
    }
    recent_.keys[recent_.count++] = {key, shift, caps_};
}
void RecentTyping::Reset(bool caps) noexcept {
    recent_ = {};
    held_.fill(false);
    caps_ = caps;
    replace_all_ = false;
}
RecentKeys RecentTyping::Snapshot() const noexcept {
    RecentKeys result = recent_;
    result.caps_active = caps_;
    return result;
}
void RecentTyping::Clear() noexcept { recent_ = {}; replace_all_ = false; }
}
