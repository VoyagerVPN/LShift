#include "recent_input.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
void Tap(lshift::RecentTyping& typing, std::uint16_t key) {
    typing.Feed(key, true);
    typing.Feed(key, false);
}
}

int main() {
    try {
        lshift::RecentTyping typing;
        for(const auto key : {0x47, 0x48, 0x42, 0x44, 0x54, 0x4E})
            Tap(typing, static_cast<std::uint16_t>(key));
        const auto word = typing.Snapshot();
        Require(word.complete && word.count == 6, "recent ghbdtn is a complete six-key word");
        Require(word.keys[0].key == 0x47 && word.keys[5].key == 0x4E &&
                !word.keys[0].shift && !word.keys[0].caps,
                "physical G through N survive without Unicode translation");
        std::cout << "PASS recent ghbdtn key snapshot\n";

        typing.Feed(0xA2, true);
        Tap(typing, 0x41);
        typing.Feed(0xA2, false);
        Tap(typing, 0x08);
        for(const auto key : {0x47, 0x48, 0x42, 0x44, 0x54, 0x4E})
            Tap(typing, static_cast<std::uint16_t>(key));
        const auto after_select_all_delete = typing.Snapshot();
        Require(after_select_all_delete.complete && after_select_all_delete.count == 6 &&
            after_select_all_delete.keys[0].key == 0x47 && after_select_all_delete.keys[5].key == 0x4E,
            "Ctrl+A, Backspace and retyping retain a complete new word without requiring a space");
        std::cout << "PASS retyping after select-all deletion\n";
        for(const auto deletion : {0x08, 0x2E}) {
            typing.Reset();
            Tap(typing, 0x08);
            typing.Feed(0xA3, true);
            Tap(typing, 0x41);
            // Удаление при ещё удерживаемом Ctrl и повторные delete в пустом поле.
            Tap(typing, static_cast<std::uint16_t>(deletion));
            typing.Feed(0xA3, false);
            Tap(typing, static_cast<std::uint16_t>(deletion));
            Tap(typing, 0x47);
            Require(typing.Snapshot().complete && typing.Snapshot().count == 1,
                "select-all deletion recovers missing history and tolerates repeated deletion");
        }
        typing.Reset();
        Tap(typing, 0x08);
        typing.Feed(0xA2, true);
        Tap(typing, 0x41);
        typing.Feed(0xA2, false);
        Tap(typing, 0x47);
        Require(typing.Snapshot().complete && typing.Snapshot().count == 1,
            "typing directly over select-all starts a complete candidate history");
        typing.Reset();
        Tap(typing, 0x08);
        typing.Feed(0xA2, true);
        Tap(typing, 0x41);
        typing.Feed(0xA2, false);
        Tap(typing, 0x1B);
        Tap(typing, 0x08);
        Tap(typing, 0x47);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 1,
            "Escape cancels select-all certainty but preserves a later candidate for field validation");
        std::cout << "PASS select-all replacement, repeated delete and cancelled selection\n";
        typing.Reset();
        for(const auto key : {0x47, 0x48, 0x42, 0x44, 0x54, 0x4E})
            Tap(typing, static_cast<std::uint16_t>(key));

        Require(!lshift::RecentKeys{}.caps_active, "default snapshot starts with Caps off");
        Tap(typing, 0x14);
        const auto caps_after_word = typing.Snapshot();
        Require(caps_after_word.caps_active && caps_after_word.count == 6 &&
                !caps_after_word.keys[5].caps,
                "snapshot reports current Caps changed after the last printable key");
        std::cout << "PASS current Caps state is distinct from recorded character Caps\n";

        typing.Reset();
        typing.Feed(0xA0, true);
        Tap(typing, 0x51);
        typing.Feed(0xA1, true);
        typing.Feed(0xA0, false);
        Tap(typing, 0x57);
        typing.Feed(0xA1, false);
        Tap(typing, 0x45);
        typing.Feed(0x14, true);
        typing.Feed(0x14, true);
        Tap(typing, 0x52);
        typing.Feed(0x14, false);
        Tap(typing, 0x54);
        Tap(typing, 0x14);
        Tap(typing, 0x59);
        const auto cases = typing.Snapshot();
        Require(cases.count == 6 && cases.keys[0].shift && cases.keys[1].shift &&
                !cases.keys[2].shift, "left and right Shift overlap without losing the held state");
        Require(!cases.keys[2].caps && cases.keys[3].caps && cases.keys[4].caps && !cases.keys[5].caps,
                "Caps toggles once per physical press and is stored for each printed key");
        typing.Reset(true);
        typing.Feed(0xA0, true);
        typing.Clear();
        Tap(typing, 0x41);
        const auto after_clear = typing.Snapshot();
        Require(after_clear.count == 1 && after_clear.keys[0].shift && after_clear.keys[0].caps,
                "Clear preserves held modifiers and seeded Caps state");
        typing.Reset();
        Tap(typing, 0x42);
        const auto after_reset = typing.Snapshot();
        Require(after_reset.count == 1 && !after_reset.keys[0].shift && !after_reset.keys[0].caps,
                "Reset clears held modifiers and resets Caps seed");
        std::cout << "PASS per-key Shift, Caps repeat, Clear and Reset\n";

        for(const auto invalidating_key : {0x09, 0x0D, 0x21, 0x22, 0x23, 0x24,
                                           0x25, 0x26, 0x27, 0x28, 0x2D, 0x2E,
                                           0xDF, 0xE5, 0xE7, 0xFFFF}) {
            typing.Reset();
            Tap(typing, 0x41);
            Tap(typing, static_cast<std::uint16_t>(invalidating_key));
            Require(typing.Snapshot().count == 0 && typing.Snapshot().complete,
                    "navigation, unsupported input and IME start a fresh guarded context");
            Tap(typing, 0x42);
            Require(typing.Snapshot().count == 1 && typing.Snapshot().keys[0].key == 0x42,
                    "a new context can record a new word immediately");
        }
        for(const auto modifier : {0x11, 0x12, 0xA2, 0xA3, 0xA4, 0xA5, 0x5B, 0x5C}) {
            typing.Reset();
            Tap(typing, 0x41);
            typing.Feed(static_cast<std::uint16_t>(modifier), true);
            Tap(typing, 0x56);
            Tap(typing, 0x20);
            Require(typing.Snapshot().count == 0, "hotkeys and held Ctrl, Alt or Win are not recorded as text");
            typing.Feed(static_cast<std::uint16_t>(modifier), false);
            Tap(typing, 0x42);
            Require(typing.Snapshot().count == 1, "recording resumes after a shortcut modifier is released");
        }
        typing.Reset();
        typing.Feed(0xA2, true);
        typing.Feed(0xA3, true);
        typing.Feed(0xA2, false);
        Tap(typing, 0x41);
        Require(typing.Snapshot().count == 0, "right Ctrl remains held when left Ctrl is released");
        typing.Feed(0xA3, false);
        Tap(typing, 0x42);
        Require(typing.Snapshot().count == 1, "both Ctrl keys must be released before recording");
        std::cout << "PASS context invalidation and shortcut guards\n";

        typing.Reset();
        Tap(typing,0x47);
        typing.MarkUncertain();
        Tap(typing,0x48);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 2 &&
                typing.Snapshot().keys[0].key == 0x47,
                "an accessibility focus hint preserves a candidate until actual field validation");

        typing.Reset();
        Tap(typing, 0x47);
        Tap(typing, 0xA2);
        Require(typing.Snapshot().count == 1,
                "a modifier tap without a shortcut does not discard the typed word");
        Tap(typing, 0x1B);
        Require(typing.Snapshot().count == 1,
                "a non-editing Escape command leaves the candidate word for field validation");

        typing.Reset();
        for(const auto printable : {0x30, 0x39, 0x41, 0x5A, 0x20, 0xBA, 0xBB, 0xBC,
                                    0xBD, 0xBE, 0xBF, 0xC0, 0xDB, 0xDC, 0xDD, 0xDE, 0xE2})
            Tap(typing, static_cast<std::uint16_t>(printable));
        const auto oem = typing.Snapshot();
        Require(oem.complete && oem.count == 17 && oem.keys[4].key == 0x20 &&
                oem.keys[5].key == 0xBA && oem.keys[16].key == 0xE2,
                "digits, letters, space and conservative US/RU OEM keys are recorded");
        typing.Reset();
        typing.Feed(0x41, true);
        typing.Feed(0x41, true);
        typing.Feed(0x41, true);
        typing.Feed(0x41, false);
        Require(typing.Snapshot().count == 3, "printable make repeats are retained instead of deduplicated");
        std::cout << "PASS printable OEM range and key repeat\n";

        typing.Reset();
        for(unsigned index=0;index<25;++index) {
            for(const auto key:{0x57,0x4F,0x52,0x44,0x20})Tap(typing,static_cast<std::uint16_t>(key));
        }
        for(const auto key:{0x47,0x48,0x42,0x44,0x54,0x4E})Tap(typing,static_cast<std::uint16_t>(key));
        Require(typing.Snapshot().count == 128 && !typing.Snapshot().complete &&
                typing.Snapshot().keys[122].key == 0x47 && typing.Snapshot().keys[127].key == 0x4E,
                "bounded history retains the whole last word when a long paragraph crosses capacity");
        Tap(typing,0x20);
        Require(typing.Snapshot().count == 128 && typing.Snapshot().keys[121].key == 0x47 &&
                typing.Snapshot().keys[127].key == 0x20,
                "a trailing space does not discard a word at the history capacity boundary");

        typing.Reset();
        Tap(typing, 0x08);
        Tap(typing, 0x47);
        Tap(typing, 0x48);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 2,
                "typing after an unknown deletion remains available for actual-field validation");

        typing.Reset();
        Tap(typing, 0x47);
        Tap(typing, 0x61);
        Require(typing.Snapshot().count == 2 && typing.Snapshot().keys[1].key == 0x61,
                "a numeric keypad digit preserves the preceding typed word");

        typing.Reset();
        for(const auto key : {0x41, 0x42, 0x20, 0x20}) Tap(typing, static_cast<std::uint16_t>(key));
        Tap(typing, 0x08);
        const auto trailing = typing.Snapshot();
        Require(trailing.complete && trailing.count == 3 && trailing.keys[2].key == 0x20,
                "Backspace removes one key while preserving the other trailing space");
        Tap(typing, 0x08);
        Tap(typing, 0x08);
        Tap(typing, 0x08);
        Require(typing.Snapshot().complete && typing.Snapshot().count == 0,
                "deleting exactly the recorded input leaves a complete empty snapshot");
        Tap(typing, 0x08);
        Tap(typing, 0x43);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 1,
                "Backspace before the recorded start marks a later candidate as uncertain");
        Tap(typing, 0x25);
        Tap(typing, 0x44);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 1,
                "navigation does not make a new candidate a complete history");
        Tap(typing, 0x20);
        Tap(typing, 0x45);
        const auto recovered = typing.Snapshot();
        Require(!recovered.complete && recovered.count == 3 && recovered.keys[1].key == 0x20 &&
                recovered.keys[2].key == 0x45, "a word after a space is retained for whole-word validation");
        std::cout << "PASS Backspace, trailing spaces and incomplete history recovery\n";

        typing.Reset();
        for(unsigned index = 0; index < 128; ++index) Tap(typing, 0x41);
        Require(typing.Snapshot().complete && typing.Snapshot().count == 128,
                "the full 128-key capacity is usable");
        Tap(typing, 0x42);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 128 &&
                typing.Snapshot().keys[127].key == 0x42,
                "overflow retains a bounded uncertain suffix requiring whole-word validation");
        Tap(typing, 0xA2);
        Tap(typing, 0x70);
        Tap(typing, 0x08);
        Tap(typing, 0x43);
        Require(!typing.Snapshot().complete && typing.Snapshot().count == 128,
                "overflow remains uncertain while a later candidate can be validated by the field");
        Tap(typing, 0x0D);
        Tap(typing, 0x44);
        Require(typing.Snapshot().complete && typing.Snapshot().count == 1 &&
                typing.Snapshot().keys[0].key == 0x44, "Enter restores a fresh context after overflow");
        for(unsigned index = 0; index < 128; ++index) Tap(typing, 0x45);
        Require(!typing.Snapshot().complete, "the next overflow is also detected");
        typing.Clear();
        Tap(typing, 0x46);
        Require(typing.Snapshot().complete && typing.Snapshot().count == 1,
                "explicit Clear starts a caller-validated new context after overflow");
        std::cout << "PASS bounded capacity and sticky overflow\n";
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
