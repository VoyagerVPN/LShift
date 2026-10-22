#include "core.hpp"
#include <iostream>
#include <stdexcept>
using namespace lshift;
void Require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        Gesture gesture;
        Require(!gesture.Feed({0xA0, true, 100, 1, 1}), "first press must pass through");
        Require(!gesture.Feed({0xA0, false, 150, 1, 1}), "one tap is not a command");
        Require(!gesture.Feed({0xA0, true, 250, 1, 1}), "second press must wait for release");
        Require(gesture.Feed({0xA0, false, 300, 1, 1}), "two short left Shift taps trigger a command");
        std::cout << "PASS: double left Shift\n";
        Gesture delayed;
        (void)delayed.Feed({0xA0, true, 100, 1, 1});
        (void)delayed.Feed({0xA0, false, 150, 1, 1});
        (void)delayed.Feed({0xA0, true, 800, 1, 1});
        Require(!delayed.Feed({0xA0, false, 850, 1, 1}), "late second tap must not trigger");
        std::cout << "PASS: late taps are ignored\n";
        auto Pair = [](Gesture& detector, std::uint16_t key = 0xA0,
                       std::uintptr_t second_context = 1, std::uintptr_t second_device = 1,
                       std::uint64_t first_release = 150) {
            (void)detector.Feed({key, true, 100, 1, 1});
            (void)detector.Feed({key, false, first_release, 1, 1});
            (void)detector.Feed({key, true, first_release + 50, second_context, second_device});
            return detector.Feed({key, false, first_release + 100, second_context, second_device});
        };
        Gesture walking;
        walking.SetHeld(0x57, true);
        Require(!Pair(walking), "held W prevents a gesture even before the first tap");
        Gesture changed_window;
        Require(!Pair(changed_window, 0xA0, 2), "focus switch between taps cancels");
        Gesture changed_device;
        Require(!Pair(changed_device, 0xA0, 1, 2), "taps on different keyboards cannot form one gesture");
        Gesture held_shift;
        Require(!Pair(held_shift, 0xA0, 1, 1, 600), "long Shift hold is not a tap");
        Gesture right;
        Require(!Pair(right, 0xA1), "right Shift must not trigger");
        Gesture paused({false, 350, 250});
        Require(!Pair(paused), "paused application ignores gestures");
        std::cout << "PASS: gameplay, focus, device and pause guards\n";
        Gesture two_keyboards;
        (void)two_keyboards.Feed({0x57, true, 10, 1, 1});
        (void)two_keyboards.Feed({0x57, true, 20, 1, 2});
        (void)two_keyboards.Feed({0x57, false, 30, 1, 1});
        (void)two_keyboards.Feed({0xA0, true, 100, 1, 2});
        (void)two_keyboards.Feed({0xA0, false, 150, 1, 2});
        (void)two_keyboards.Feed({0xA0, true, 200, 1, 2});
        Require(!two_keyboards.Feed({0xA0, false, 250, 1, 2}),
                "releasing W on keyboard A must not lose held W on keyboard B");
        Require(Convert(L"Ghbdtn vbh!", Direction::ToRussian) == L"Привет мир!",
                "wrong-layout Latin text converts to Russian with case preserved");
        Require(Convert(L"Привет мир!", Direction::ToEnglish) == L"Ghbdtn vbh!",
                "conversion works in the opposite direction");
        Require(DetectDirection(L"ghbdtn") == Direction::ToRussian, "infer Latin source");
        Require(DetectDirection(L"привет") == Direction::ToEnglish, "infer Russian source");
        Require(!DetectDirection(L"hello привет"), "one direction cannot represent both alphabets");
        Require(!DetectDirection(L"12345"), "numbers do not imply a text conversion direction");
        std::cout << "PASS: RU-US conversion\n";
        const auto bilingual = ConvertWords(L"привет ghbdtn");
        Require(bilingual.text == L"ghbdtn привет" &&
                bilingual.last_direction == Direction::ToRussian,
                "selection inverts each single-alphabet word and follows the last converted word");
        Require(ConvertWords(L"привет🙂ghbdtn\u00A0мир").text == L"ghbdtn🙂привет\u00A0vbh",
                "emoji and nonbreaking spaces separate words without changing themselves");
        const auto mixed_word = ConvertWords(L"привет приvet ghbdtn");
        Require(mixed_word.text == L"ghbdtn приvet привет",
                "mixed-alphabet word remains whole while neighboring words convert");
        Require(ConvertWords(mixed_word.text).text == L"привет приvet ghbdtn",
                "a second conversion restores both alphabets and preserves the mixed word");
        Require(ConvertWords(L"Ghbdtn-МИР\tтест123\r\n;bdjq").text ==
                L"Привет-VBH\tntcn123\r\nживой",
                "case, digits, hyphen boundaries and physical semicolon key are supported");
        const auto untouched = ConvertWords(L"приvet 12345 ; 🙂");
        Require(untouched.text == L"приvet 12345 ; 🙂" && !untouched.last_direction,
                "mixed words, numbers and standalone punctuation do not cause text replacement");
        const auto last_english = ConvertWords(L"hello мир приvet");
        Require(last_english.text == L"руддщ vbh приvet" &&
                last_english.last_direction == Direction::ToEnglish,
                "skipped mixed word does not override the last converted direction");
        Require(ConvertWords(L"[jhjij ;bdjq").text == L"хорошо живой",
                "punctuation on Russian letter keys stays attached to a word");
        Require(ConvertWords(L"люблю жук").text == L"k.,k. ;er" &&
                ConvertWords(L"k.,k. ;er").text == L"люблю жук",
                "physical punctuation inside a word survives a round trip");
        Require(ConvertWords(L"").text.empty() && !ConvertWords(L"").last_direction,
                "empty selection has no direction");
        std::cout << "PASS: word-by-word bilingual selection\n";
        const auto word = LastWord(L"one ghbdtn  ", 12);
        Require(word && word->start == 4 && word->length == 6,
                "last word excludes trailing spaces and the previous word");
        const auto before_newline = LastWord(L"ghbdtn\n", 7);
        Require(!before_newline, "word lookup must not cross a new line");
        const auto physical = LastWord(L"[jhjij", 6);
        Require(physical && physical->start == 0 && physical->length == 6,
                "punctuation on Russian letter keys belongs to the mistyped word");
        Require(Convert(L"[jhjij", Direction::ToRussian) == L"хорошо", "physical key punctuation converts");
        Require(!LastWord(L"", 0), "empty field has no word");
        Require(!LastWord(L"hello", 9), "invalid caret is rejected");
        std::cout << "PASS: last word ranges\n";
    } catch(const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
