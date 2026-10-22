#pragma once
#include "core.hpp"
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace lshift {
inline constexpr UINT kTrayMessage = WM_APP + 10;
inline constexpr UINT kSettingsChanged = WM_APP + 11;
inline constexpr UINT kShowSettings = WM_APP + 12;
inline constexpr UINT kToggleEnabled = 1001;
inline constexpr UINT kOpenSettings = 1002;
inline constexpr UINT kToggleAutostart = 1003;
inline constexpr UINT kExit = 1004;
inline constexpr UINT kOpenDiagnostics = 1005;
[[nodiscard]] std::filesystem::path SettingsPath();
[[nodiscard]] Settings LoadSettings();
[[nodiscard]] bool SaveSettings(const Settings& settings);
[[nodiscard]] bool AutostartEnabled();
[[nodiscard]] bool SetAutostart(bool enabled);
[[nodiscard]] HICON CreateTrayIcon(bool enabled);
[[nodiscard]] bool AddTray(HWND window, HICON icon, bool enabled);
void UpdateTray(HWND window, HICON icon, bool enabled);
void SetTrayStatus(HWND window, std::wstring_view detail);
void RemoveTray(HWND window);
void ShowTrayMenu(HWND window, bool enabled, POINT point);
void ShowSettings(HWND owner, const Settings& settings);
void CloseSettings();
}
