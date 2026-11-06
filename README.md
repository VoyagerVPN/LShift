# LShift

<img src="resources/logo.png" alt="LShift logo" width="64">

**English** · [Русский](README.ru.md)

Press left Shift twice to fix a word or selection typed in the wrong keyboard layout. LShift converts between US QWERTY and Russian ЙЦУКЕН.

**Windows 10/11 x64 · Portable EXE**

[Download LShift.exe](https://github.com/VoyagerVPN/LShift/releases/latest/download/LShift.exe) · [Discussions](https://github.com/VoyagerVPN/LShift/discussions)

## Quick start

1. Install the **English (US)** and **Russian** keyboard layouts in Windows.
2. Download and run `LShift.exe`. Look for its tray icon, including under the hidden icons arrow.
3. Type `ghbdtn` in the wrong layout and press **left Shift twice** to turn it into `привет`. You can also select text before using the shortcut.
4. Double-click the tray icon to open Settings. You can change the interface language, adjust the shortcut interval or enable startup with Windows. Right-click for the tray menu.

A fresh profile uses Russian on Russian Windows and English on other systems. Existing profiles without a language choice keep Russian. The interface language is independent of the layouts that LShift converts between.

## Features

- Corrects a selection or the recently typed word before the caret. When there is no eligible text, the shortcut only switches the layout.
- Converts each word in a selection separately. Words already mixing Latin and Cyrillic letters are preserved.
- Runs from the tray and can pause or start with Windows for the current user.
- The EXE runs without an installer, administrator rights or a separate runtime.

| Before | After |
| --- | --- |
| `ghbdtn` | `привет` |
| Selected `привет ghbdtn` | `ghbdtn привет` |
| Selected `привет приvet ghbdtn` | `ghbdtn приvet привет` |

For startup, keep the EXE in a permanent location. LShift saves its settings in `%LOCALAPPDATA%/LShift/settings.ini`. Close other layout switchers that use the same shortcut when testing. [Detailed behavior and checks](README.ru.md#поведение-и-ограничения).

## Build from source

Use Windows 10/11 x64, Node.js 24 and Visual Studio 2022 or 2026 with Desktop development with C++, Windows SDK, CMake and Ninja.

```powershell
node scripts/build.mjs
```

The release EXE is written to `dist/`.

## License

[MIT with Commons Clause 1.0](LICENSE). The source is available under those terms. See [NOTICE](NOTICE) for attribution and [LICENSE](LICENSE) for the restrictions on sale. The EXE includes both notices.
