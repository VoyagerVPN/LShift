#include "shell.hpp"

#include <filesystem>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

class SettingsSandbox {
public:
    SettingsSandbox() {
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
        if (length != 0) {
            std::vector<wchar_t> buffer(length);
            const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(), length);
            Require(copied != 0 && copied < length, "read original LOCALAPPDATA");
            original_ = std::wstring(buffer.data(), copied);
        }
        const auto root = std::filesystem::absolute(L".cache");
        static unsigned sequence = 0;
        path_ = root / (L"shell-tests-" + std::to_wstring(GetCurrentProcessId()) +
                        L"-" + std::to_wstring(GetTickCount64()) + L"-" +
                        std::to_wstring(++sequence));
        Require(SetEnvironmentVariableW(L"LOCALAPPDATA", path_.c_str()) != FALSE,
                "isolate settings in test process");
    }

    ~SettingsSandbox() {
        SetEnvironmentVariableW(L"LOCALAPPDATA", original_ ? original_->c_str() : nullptr);
        std::error_code ignored;
        if (path_.parent_path() == std::filesystem::absolute(L".cache") &&
            path_.filename().wstring().starts_with(L"shell-tests-")) {
            std::filesystem::remove_all(path_, ignored);
        }
    }

    SettingsSandbox(const SettingsSandbox&) = delete;
    SettingsSandbox& operator=(const SettingsSandbox&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::optional<std::wstring> original_;
    std::filesystem::path path_;
};

class RegistrySandbox {
public:
    RegistrySandbox() {
        name_ = L"LShift.Tests." + std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(GetTickCount64());
        try {
            Require(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software", 0,
                                  KEY_READ | KEY_WRITE | DELETE, &software_) == ERROR_SUCCESS,
                    "open HKCU Software for a unique registry sandbox");
            DWORD disposition = 0;
            Require(RegCreateKeyExW(software_, name_.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                    KEY_ALL_ACCESS, nullptr, &sandbox_, &disposition) == ERROR_SUCCESS,
                    "create isolated registry sandbox");
            created_ = disposition == REG_CREATED_NEW_KEY;
            Require(created_, "registry sandbox must be newly created, never an existing key");
            const LSTATUS result = RegOverridePredefKey(HKEY_CURRENT_USER, sandbox_);
            if (result != ERROR_SUCCESS) {
                throw std::runtime_error("process-local HKCU override is unavailable (error " +
                                         std::to_string(result) + "); real Run was not touched");
            }
            overridden_ = true;
        } catch (...) {
            if (!Cleanup()) std::cerr << "FAIL: registry sandbox cleanup after setup failure\n";
            throw;
        }
    }

    ~RegistrySandbox() {
        if (!Cleanup()) std::cerr << "FAIL: registry sandbox cleanup\n";
    }

    RegistrySandbox(const RegistrySandbox&) = delete;
    RegistrySandbox& operator=(const RegistrySandbox&) = delete;
    [[nodiscard]] bool Finish() noexcept { return Cleanup(); }

private:
    [[nodiscard]] bool Cleanup() noexcept {
        // Сначала возвращаем HKCU; до успеха не закрываем и не удаляем отображённый ключ.
        if (overridden_) {
            if (RegOverridePredefKey(HKEY_CURRENT_USER, nullptr) != ERROR_SUCCESS) return false;
            overridden_ = false;
        }
        if (sandbox_) {
            if (RegCloseKey(sandbox_) != ERROR_SUCCESS) return false;
            sandbox_ = nullptr;
        }
        if (created_) {
            if (!software_ || !name_.starts_with(L"LShift.Tests.") ||
                name_.find(L'\\') != std::wstring::npos) return false;
            // Открытый до override родитель указывает на настоящий Software; удаляем только свой ключ.
            const LSTATUS removed = RegDeleteTreeW(software_, name_.c_str());
            if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND) return false;
            created_ = false;
        }
        if (software_) {
            if (RegCloseKey(software_) != ERROR_SUCCESS) return false;
            software_ = nullptr;
        }
        return true;
    }

    std::wstring name_;
    HKEY software_ = nullptr;
    HKEY sandbox_ = nullptr;
    bool created_ = false;
    bool overridden_ = false;
};

void RequireSettings(const lshift::Settings& actual, const lshift::Settings& expected,
                     const char* message) {
    Require(actual.enabled == expected.enabled && actual.double_tap_ms == expected.double_tap_ms &&
            actual.hold_ms == expected.hold_ms, message);
}

void WriteIni(std::wstring_view contents) {
    const auto path = lshift::SettingsPath();
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(file), "create isolated INI fixture");
    constexpr wchar_t bom = L'\xFEFF';
    file.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
    file.write(reinterpret_cast<const char*>(contents.data()),
               static_cast<std::streamsize>(contents.size() * sizeof(wchar_t)));
    file.close();
    Require(static_cast<bool>(file), "write isolated INI fixture");
}
}

int main() {
    try {
        SettingsSandbox sandbox;
        Require(lshift::SettingsPath() == sandbox.path() / L"LShift" / L"settings.ini",
                "settings must use the current user's LOCALAPPDATA");
        const lshift::Settings selected{false, 575, 180};
        Require(lshift::SaveSettings(selected), "user can save chosen settings");
        const auto loaded = lshift::LoadSettings();
        Require(!loaded.enabled && loaded.double_tap_ms == 575 && loaded.hold_ms == 180,
                "settings survive reload with the values chosen by the user");
        std::cout << "PASS: saved settings survive reload in an isolated profile\n";

        constexpr std::array<lshift::Settings, 5> invalid{{
            {true, 0, 250}, {true, 149, 250}, {true, 801, 250},
            {true, 350, 99}, {true, 350, 501}
        }};
        for (const auto& candidate : invalid) {
            Require(!lshift::SaveSettings(candidate), "out-of-range settings must be rejected");
            RequireSettings(lshift::LoadSettings(), selected,
                            "rejected settings must preserve the previously saved values");
        }
        std::cout << "PASS: invalid saves preserve the user's settings\n";

        for (const lshift::Settings boundary : std::array<lshift::Settings, 2>{{
                 {true, 150, 100}, {false, 800, 500}}}) {
            SettingsSandbox boundary_sandbox;
            Require(lshift::SaveSettings(boundary), "documented boundary values can be saved");
            RequireSettings(lshift::LoadSettings(), boundary, "boundary values survive reload");
        }
        std::cout << "PASS: interval and hold boundaries are inclusive\n";

        struct IniCase {
            std::wstring_view contents;
            lshift::Settings expected;
        };
        const std::array<IniCase, 5> damaged{{
            {L"[Settings]\r\nEnabled=wrong\r\nDoubleTapMs=350ms\r\n"
             L"HoldMs=99999999999999999999999999999999999999\r\n", {true, 350, 250}},
            {L"[Settings]\r\nEnabled=2\r\nDoubleTapMs=149\r\nHoldMs=501\r\n", {true, 350, 250}},
            {L"[Settings]\r\nEnabled=-1\r\nDoubleTapMs=\r\nHoldMs=-100\r\n", {true, 350, 250}},
            {L"[Other]\r\nDoubleTapMs=500\r\n", {true, 350, 250}},
            {L"[Settings]\r\nEnabled=0\r\nDoubleTapMs=800\r\nHoldMs=garbage\r\n", {false, 800, 250}}
        }};
        for (const auto& fixture : damaged) {
            SettingsSandbox damaged_sandbox;
            WriteIni(fixture.contents);
            RequireSettings(lshift::LoadSettings(), fixture.expected,
                            "damaged INI values fall back independently to safe defaults");
        }
        std::cout << "PASS: malformed and out-of-range INI values use safe defaults\n";

        {
            RegistrySandbox registry;
            Require(!lshift::AutostartEnabled(), "autostart is off in a new isolated registry");
            Require(lshift::SetAutostart(true), "user can register autostart in the isolated registry");
            Require(lshift::AutostartEnabled(), "autostart registration is reported as enabled");
            Require(lshift::SetAutostart(false), "user can remove autostart from the isolated registry");
            Require(!lshift::AutostartEnabled(), "removed autostart registration is reported as disabled");
            Require(registry.Finish(), "restore HKCU mapping and remove only the test sandbox");
        }
        std::cout << "PASS: autostart toggles through a process-local HKCU sandbox\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
