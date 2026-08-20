# Desktop Brightness Adjuster

Hotkey brightness control for **desktop monitors** on Windows 10 and 11 — including the ones Windows gives you no slider for.

![Language](https://img.shields.io/badge/Language-C-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6.svg)
![Size](https://img.shields.io/badge/Size-24%20KB-brightgreen.svg)
![Startup Impact](https://img.shields.io/badge/Startup%20Impact-Low-success.svg)

Windows only exposes a brightness slider for laptop panels. Plug a monitor into a desktop and you are left with its physical buttons. This sits in the tray and gives you `Alt`+`PageUp` / `Alt`+`PageDown` instead.

---

## Why this exists

Controlling an external monitor's brightness means talking DDC/CI over the I²C lines in the video cable. That mostly works — except a lot of panels **accept the command, report success, and then ignore it.** They stay frozen at whatever their on-screen menu is set to, and no software can move them.

That single fact drives the whole design. If one monitor is frozen at 50% and another happily obeys, turning the dial down pulls them apart instead of together.

So the app picks its strategy from what your hardware actually does:

| Mode | When | What happens |
| :--- | :--- | :--- |
| **Hardware backlight** | Every monitor accepts brightness writes | Each backlight is driven across its own reported range. Real dimming — less backlight power, full contrast. |
| **Match all monitors** | Some monitors obey, at least one is frozen | Controllable monitors are pinned to the frozen one's level, and a dimming overlay drives the whole range on every screen — identical on each, so mismatched panels track together at *every* step. |

**Automatic** picks between them and re-picks whenever your monitor layout changes. You can force either from the tray menu; the choice is remembered.

> **Why matching needs the overlay:** two panels' backlight PWM curves are not the same shape. Drive both by DDC percentage and they agree at 0% and 100% and drift everywhere in between. One uniform software overlay is the only way to make genuinely different panels step together.

---

## Features

- **No lag on a held key.** DDC/CI round-trips take ~60 ms. Repeats are folded together and slow work is deferred, so letting go of the key stops the change immediately instead of drifting on for a second afterwards.
- **Real backlight control where possible**, software dimming only where it isn't.
- **Screen capture excluded.** Uses `SetWindowDisplayAffinity`, so screenshots and screen sharing (Discord, Zoom, Teams, OBS) stay at full brightness while your eyes get the dimmed version.
- **Detects lying monitors.** A write is read back to confirm it took effect, retried before the panel is written off, and you get a tray notification explaining what happened.
- **Survives display changes.** Hotplug, resolution and arrangement changes rebuild cleanly.
- **Per-monitor DPI aware**, so the overlay covers high-DPI screens fully.
- **~24 KB, no dependencies.** Links only against DLLs Windows has already loaded. No runtime, no installer, no background polling, no idle CPU.

---

## Install

1. Grab `DesktopBrightness.exe` from a release, or build it (below).
2. Run it. It lives in the tray.
3. Right-click the tray icon → **Start with Windows** if you want it persistent.

Adjust with `Alt` + `PageUp` / `PageDown`.

---

## Requirements

- Windows 10 (2004+) or Windows 11
- A **desktop** monitor connected over DisplayPort, HDMI or DVI

DDC/CI is often **disabled by default** in a monitor's own menu. If brightness does nothing on one screen, look for a `DDC/CI` toggle under `Settings`, `OSD Setup`, `System` or `Other` and switch it on.

**Laptops are out of scope.** Internal laptop panels use a different mechanism (WMI) and already have working brightness keys. External monitors attached to a laptop work fine.

---

## Configuration

Everything below is optional and lives under `HKEY_CURRENT_USER\Software\DesktopBrightness`. Restart the app after changing a value.

| Value | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `Step` | DWORD | `5` | Percent per keypress (1–50) |
| `HotkeyMod` | DWORD | `1` | Modifier bitmask |
| `HotkeyVkUp` | DWORD | `0x21` | Virtual-key code to brighten |
| `HotkeyVkDown` | DWORD | `0x22` | Virtual-key code to dim |
| `Mode` | DWORD | `0` | `0` automatic, `1` hardware, `2` matched |
| `Level` | DWORD | — | Current level; managed by the app |

**Modifiers** (add them together): `1` Alt · `2` Ctrl · `4` Shift · `8` Win — e.g. `3` is Ctrl+Alt.

**Common keys:** `0x21` PgUp · `0x22` PgDn · `0x26` Up · `0x28` Down · `0x7A` F11 · `0x7B` F12 · `0xAF` Vol Up · `0xAE` Vol Down

Set Ctrl + Up/Down:

```powershell
$k = 'HKCU:\Software\DesktopBrightness'
New-Item -Path $k -Force | Out-Null
Set-ItemProperty $k -Name HotkeyMod    -Value 2    -Type DWord
Set-ItemProperty $k -Name HotkeyVkUp   -Value 0x26 -Type DWord
Set-ItemProperty $k -Name HotkeyVkDown -Value 0x28 -Type DWord
```

If the combination is already taken by another app, you get a tray warning at startup rather than silent dead keys.

---

## Building

Needs a C compiler targeting Windows. [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) is what the build script looks for, falling back to any `gcc` on `PATH`.

```powershell
powershell -ExecutionPolicy Bypass -File build_brightness_app.ps1
```

Or directly:

```
gcc -Os -mwindows -municode -ffunction-sections -fdata-sections -Wall -Wextra ^
    DesktopBrightness.c -o DesktopBrightness.exe ^
    -Wl,--gc-sections -s -ldxva2 -luser32 -lgdi32 -lshell32 -ladvapi32
```

---

## Known limits

- **Exclusive-fullscreen games** bypass the desktop compositor, so the software overlay cannot cover them. Borderless windowed works. Monitors in hardware mode are unaffected — the backlight is genuinely dimmer.
- **Secure desktop** (UAC prompts, Ctrl+Alt+Del) is not covered by the overlay, by design.
- **Software dimming cannot brighten.** If a monitor is frozen at 50% by its own menu, nothing can push it past that — turn it up with the monitor's buttons, or enable DDC/CI on it.
- **USB display adapters and some KVMs** do not pass DDC/CI through; those monitors fall back to overlay dimming.

---

## License

MIT — see [LICENSE](LICENSE).
