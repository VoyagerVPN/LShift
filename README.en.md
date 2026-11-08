# LShift

<img src="resources/logo.png" alt="LShift logo" width="64">

**English** · [Русский](README.md)

Press left Shift twice to fix a word or selection typed in the wrong keyboard layout. LShift converts between US QWERTY and Russian ЙЦУКЕН.

**Windows 10/11 x64 · Portable EXE**

[Download LShift.exe](https://github.com/VoyagerVPN/LShift/releases/latest/download/LShift.exe) · [Discussions](https://github.com/VoyagerVPN/LShift/discussions)

## Quick start

1. Add **English (US)** and **Russian** keyboard layouts in Windows and run `LShift.exe`.
2. Type `ghbdtn` and press **left Shift twice** to turn it into `привет`. The shortcut works on selected text or the last word before the cursor.
3. Double-click the tray icon to change the interface language, adjust the shortcut interval or enable startup with Windows.

When there is no eligible text, the shortcut switches the layout. Words that mix Cyrillic and Latin letters stay as typed.

Symbols follow the same physical keys as letters: `ghbdtn# → привет№`, `@ → "`, `$ → ;`. For symbols without letters, the field's current keyboard layout determines the direction.

## Behavior and troubleshooting

- Keep both keyboard layouts installed. If another layout switcher uses the same shortcut, close it before testing. Hover over the tray icon to see the last result.
- LShift skips password fields, read-only fields and text that no longer matches what you typed. Some editors can briefly show intermediate text or require more than one Undo.
- Replacing a selection through UI Automation overwrites the clipboard. Keep the EXE in a permanent folder before enabling startup with Windows.

## Build from source

Use Node.js 24 and Visual Studio 2022 or 2026 with Desktop development with C++, Windows SDK, CMake and Ninja.

```powershell
node scripts/build.mjs
```

The EXE is written to `dist/`. See [validation notes](docs/VALIDATION.md) and the [changelog](CHANGELOG.md).

## License

[MIT with Commons Clause 1.0](LICENSE). See [NOTICE](NOTICE) for attribution and [LICENSE](LICENSE) for restrictions on sale.
