#include "core.hpp"
namespace lshift {
Gesture::Gesture(Settings settings) noexcept : settings_(settings) {}
bool Gesture::Feed(const KeyEvent& event) noexcept {
    if(event.key >= held_.size()) { stage_ = 0; return false; }
    DeviceKeys* device = nullptr;
    for(auto& candidate : devices_) {
        if(candidate.used && candidate.id == event.device) { device = &candidate; break; }
    }
    if(!device) {
        for(auto& candidate : devices_) {
            bool pressed = false;
            for(const bool held : candidate.held) pressed = pressed || held;
            if(!candidate.used || !pressed) {
                candidate = {};
                candidate.used = true;
                candidate.id = event.device;
                device = &candidate;
                break;
            }
        }
    }
    if(!device) { stage_ = 0; return false; }
    const bool was_held = device->held[event.key];
    device->held[event.key] = event.down;
    if(event.down && !was_held) ++held_counts_[event.key];
    else if(!event.down && was_held) --held_counts_[event.key];
    if(!event.down) held_[event.key] = false;
    if(!settings_.enabled || event.key != 0xA0) { stage_ = 0; return false; }
    if(stage_ != 0 && (event.context != context_ || event.device != device_)) stage_ = 0;
    if(event.down) {
        if(was_held) { stage_ = 0; return false; }
        for(std::size_t key = 0; key < held_.size(); ++key) {
            if(key != 0xA0 && (held_[key] || held_counts_[key] != 0)) { stage_ = 0; return false; }
        }
        if(stage_ == 2 && event.time_ms - released_at_ > settings_.double_tap_ms) stage_ = 0;
        pressed_at_ = event.time_ms;
        context_ = event.context;
        device_ = event.device;
        if(stage_ == 0 || stage_ == 2) ++stage_;
        else stage_ = 0;
        return false;
    }
    if(event.time_ms < pressed_at_ || event.time_ms - pressed_at_ > settings_.hold_ms) {
        stage_ = 0;
        return false;
    }
    if(stage_ == 1) { stage_ = 2; released_at_ = event.time_ms; return false; }
    if(stage_ == 3) { stage_ = 0; return true; }
    return false;
}
void Gesture::Reset() noexcept {
    stage_ = 0;
    pressed_at_ = released_at_ = 0;
    context_ = device_ = 0;
    held_.fill(false);
    held_counts_.fill(0);
    devices_ = {};
}
void Gesture::SetSettings(Settings settings) noexcept { settings_ = settings; Reset(); }
void Gesture::SetHeld(std::uint16_t key, bool held) noexcept { if(key < held_.size()) held_[key] = held; }
namespace {
constexpr std::wstring_view kUs = L"`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./";
constexpr std::wstring_view kRu = L"ё1234567890-=йцукенгшщзхъ\\фывапролджэячсмитьбю.";
constexpr std::wstring_view kUsShift = L"~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
constexpr std::wstring_view kRuShift = L"Ё!\"№;%:?*()_+ЙЦУКЕНГШЩЗХЪ/ФЫВАПРОЛДЖЭЯЧСМИТЬБЮ,";
static_assert(kUs.size() == kRu.size());
static_assert(kUsShift.size() == kRuShift.size());
bool Latin(wchar_t c) noexcept { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }
bool Russian(wchar_t c) noexcept { return (c >= L'а' && c <= L'я') || (c >= L'А' && c <= L'Я') || c == L'ё' || c == L'Ё'; }
bool WordCharacter(wchar_t c) noexcept {
    return Latin(c) || Russian(c) || (c >= L'0' && c <= L'9') ||
           std::wstring_view(L"_`[]{}\\|;'\",./~<>:").find(c) != std::wstring_view::npos;
}
}
std::wstring Convert(std::wstring_view text, Direction direction) {
    const auto source = direction == Direction::ToRussian ? kUs : kRu;
    const auto target = direction == Direction::ToRussian ? kRu : kUs;
    const auto source_shift = direction == Direction::ToRussian ? kUsShift : kRuShift;
    const auto target_shift = direction == Direction::ToRussian ? kRuShift : kUsShift;
    std::wstring result(text);
    for(auto& c : result) {
        const auto lower = source.find(c);
        if(lower != std::wstring_view::npos) c = target[lower];
        else if(const auto upper = source_shift.find(c); upper != std::wstring_view::npos) c = target_shift[upper];
    }
    return result;
}
std::optional<Direction> DetectDirection(std::wstring_view text) noexcept {
    bool latin = false;
    bool russian = false;
    for(const auto c : text) { latin = latin || Latin(c); russian = russian || Russian(c); }
    if(latin == russian) return std::nullopt;
    return latin ? Direction::ToRussian : Direction::ToEnglish;
}
WordConversion ConvertWords(std::wstring_view text) {
    WordConversion result{std::wstring(text), std::nullopt};
    std::size_t start = 0;
    while(start < text.size()) {
        if(!WordCharacter(text[start])) { ++start; continue; }
        std::size_t end = start + 1;
        while(end < text.size() && WordCharacter(text[end])) ++end;
        const auto word = text.substr(start, end - start);
        // Смешанное слово сохраняется целиком; соседние слова от него не зависят.
        if(const auto direction = DetectDirection(word)) {
            result.text.replace(start, word.size(), Convert(word, *direction));
            result.last_direction = direction;
        }
        start = end;
    }
    return result;
}
std::optional<TextSpan> LastWord(std::wstring_view text, std::size_t caret) noexcept {
    if(caret > text.size()) return std::nullopt;
    std::size_t end = caret;
    while(end > 0 && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                     std::wstring_view(L"!?()").find(text[end - 1]) != std::wstring_view::npos)) --end;
    std::size_t start = end;
    while(start > 0 && WordCharacter(text[start - 1])) --start;
    if(start == end) return std::nullopt;
    return TextSpan{start, end - start};
}
}
