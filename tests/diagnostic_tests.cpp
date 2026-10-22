#include "diagnostics.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
    if(!value) throw std::runtime_error(message);
}
}
int main() {
    try {
        lshift::DiagnosticQueue queue;
        lshift::DiagnosticRecord event;
        event.event = lshift::DiagnosticEvent::Gesture;
        event.reason = lshift::DiagnosticReason::Busy;
        event.tick = 123;
        event.count = 6;
        Require(queue.Push(event), "a rejected gesture can be queued without blocking");
        lshift::DiagnosticRecord observed;
        Require(queue.Pop(observed) && observed.tick == 123 && observed.count == 6 &&
            observed.reason == lshift::DiagnosticReason::Busy, "diagnostic retains reason and bounded count");
        Require(!queue.Pop(observed), "empty queue does not duplicate an event");
        std::cout << "PASS diagnostic event queue\n";

        for(unsigned index = 0; index < 127; ++index) {
            event.tick = index;
            Require(queue.Push(event), "bounded queue accepts its capacity");
        }
        Require(!queue.Push(event) && queue.Dropped() == 1, "overflow is counted instead of waiting");
        for(unsigned index = 0; index < 127; ++index)
            Require(queue.Pop(observed) && observed.tick == index, "queue drains in original order");
        Require(queue.Push(event) && queue.Pop(observed), "queue recovers after overflow");
        std::cout << "PASS bounded diagnostic queue and overflow recovery\n";

        const auto path = std::filesystem::current_path() / ".cache/diagnostic-test/events.jsonl";
        auto backup = path;
        backup += ".1";
        std::filesystem::remove(path);
        std::filesystem::remove(backup);
        lshift::DiagnosticLog log(path, 1024);
        event.event = lshift::DiagnosticEvent::Result;
        event.status = 3;
        Require(log.Append(event, L"Проверка \"фокуса\"\nотменена", 1), "worker persists a diagnostic result");
        std::ifstream input(path, std::ios::binary);
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        input.close();
        Require(contents.find("\"event\":\"result\"") != std::string::npos &&
            contents.find("\\\"фокуса\\\"\\n") != std::string::npos &&
            contents.find("\"dropped\":1") != std::string::npos,
            "JSONL preserves service reason with escaped quotes and Unicode");
        for(unsigned index = 0; index < 20; ++index)
            Require(log.Append(event), "log remains writable across rotations");
        Require(std::filesystem::exists(backup) && std::filesystem::file_size(path) <= 1024 &&
            std::filesystem::file_size(backup) <= 1024, "current and previous log remain bounded");
        lshift::DiagnosticLog invalid({});
        Require(!invalid.Append(event), "unavailable log never throws into text processing");
        std::cout << "PASS UTF-8 JSONL, bounded rotation and unavailable log\n";
    } catch(const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
