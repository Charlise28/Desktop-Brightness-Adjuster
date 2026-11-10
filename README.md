# Desktop Brightness Adjuster

Hotkey brightness control for **any number of monitors** on Windows 10 and 11 — kept in sync, so every screen looks the same at every step.

![Language](https://img.shields.io/badge/Language-C-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6.svg)
![Size](https://img.shields.io/badge/Size-32%20KB-brightgreen.svg)
![Startup Impact](https://img.shields.io/badge/Startup%20Impact-Low-success.svg)

Windows only exposes a brightness slider for laptop panels. Plug a monitor into a desktop and you are left with its physical buttons. This sits in the tray and gives you `Alt`+`PageUp` / `Alt`+`PageDown` instead, for every monitor at once.

---

## Why this exists

Controlling an external monitor's brightness means talking DDC/CI over the I²C lines in the video cable. Two things make that hard to do across several monitors:

1. **Panels disagree.** Two monitors set to the same DDC percentage do not look the same — their backlight curves have different shapes, so they agree at 0% and 100% and drift apart everywhere in between.
2. **Some panels lie.** A lot of monitors **accept the brightness command, report success, and then ignore it.** They stay frozen at whatever their on-screen menu is set to, and no software can move them.

So the app has two strategies:

| Mode | What happens |
| :--- | :--- |
| **Match all monitors** | Every backlight is held at one common level — the highest every panel can actually reach — and a dimming overlay with **identical** strength on every screen does the dialling. Identical overlay = identical step on every panel, so they track together at *every* position. |
| **Hardware backlight** | Each backlight is driven across its own range. Real dimming (less backlight power, full contrast); below 20% the overlay carries on past the panel's own minimum. A monitor with no working DDC/CI gets the overlay across the whole dial. |

**Automatic** (the default) uses *Match all monitors* whenever more than one screen is connected and *Hardware backlight* when there is only one, and re-decides whenever your monitor layout changes. You can force either from the tray menu; the choice is remembered.

### Balance monitors

Matching makes every screen dim by the same amount, but it cannot make a 400-nit panel as dim as a 250-nit one at full brightness. If one monitor still looks brighter than its neighbour, right-click the tray icon → **Balance monitors** → pick that monitor → **5–40% dimmer** until they look alike. It is remembered per monitor (by its connection, so it survives the monitor being renumbered).

---

## Features

- **Works on any setup.** One monitor or sixteen, any resolution, refresh rate, orientation or DPI, mixed brands, monitors with or without DDC/CI, cloned displays, laptops with external screens.
- **Stays on top.** Image viewers, video players, the taskbar and other "always on top" windows can push themselves over a dimming overlay; the app notices within milliseconds and puts the overlay back, so a monitor never jumps to full brightness when you open something on it. If another app keeps fighting for the top, it backs off instead of burning CPU.
- **No lag on a held key.** DDC/CI round-trips take ~60 ms and run on a separate thread that never blocks the hotkeys. Repeats are folded together, so letting go of the key stops the change immediately.
- **Detects lying monitors.** A brightness write is read back to confirm it took effect (with a tolerance for panels that round the value), retried before the panel is written off, and remembered so it is not re-tested on every start. You get a tray notification explaining what happened, and **Re-detect monitors** in the tray menu tests again after you enable DDC/CI.
- **Wide DDC/CI support.** Talks raw MCCS VCP `0x10` first, which works on monitors whose capability string Windows' high-level API rejects, and falls back to the high-level API.
- **Screen capture excluded.** Uses `SetWindowDisplayAffinity`, so screenshots and screen sharing (Discord, Zoom, Teams, OBS) stay at full brightness while your eyes get the dimmed version.
- **Survives display changes and sleep.** Hotplug, resolution, arrangement changes and resume from sleep rebuild cleanly.
- **Per-monitor DPI aware**, so the overlay covers high-DPI screens fully.
- **~32 KB, no dependencies.** Links only against DLLs Windows has already loaded. No runtime, no installer, no background polling, no idle CPU.

---

## Install

1. Grab `DesktopBrightness.exe` from a release, or build it (below).
2. Run it. It lives in the tray.
3. Right-click the tray icon → **Start with Windows** if you want it persistent.

Adjust with `Alt` + `PageUp` / `PageDown`.

---

## Requirements

- Windows 10 (2004+) or Windows 11
- Any monitor. DDC/CI (over DisplayPort, HDMI or DVI) gives real backlight dimming; without it the monitor is dimmed in software and still stays in step with the rest.

DDC/CI is often **disabled by default** in a monitor's own menu. To get real backlight control, look for a `DDC/CI` toggle under `Settings`, `OSD Setup`, `System` or `Other`, switch it on, then use **Re-detect monitors** in the tray menu.

A laptop's **internal** panel is dimmed in software only (its backlight is controlled through WMI, and its own brightness keys still work). External monitors attached to a laptop work exactly as on a desktop.

---

## Configuration

Everything below is optional and lives under `HKEY_CURRENT_USER\Software\DesktopBrightness`. Restart the app after changing a value.

| Value | Type | Default | Meaning |
| :--- | :--- | :--- | :--- |
| `Step` | DWORD | `5` | Percent per keypress (1–50) |
| `HotkeyMod` | DWORD | `1` | Modifier bitmask |
| `HotkeyVkUp` | DWORD | `0x21` | Virtual-key code to brighten |
| `HotkeyVkDown` | DWORD | `0x22` | Virtual-key code to dim |
| `Mode` | DWORD | `0` | `0` automatic, `1` hardware, `2` match all monitors |
| `Level` | DWORD | — | Current level; managed by the app |
| `Balance\<monitor>` | DWORD | — | Per-monitor balance (percent dimmer); set from the tray menu |
| `IgnoresWrites\<monitor>` | DWORD | — | Monitors caught ignoring DDC/CI; cleared by **Re-detect monitors** |

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
- **Windows' own shell layers** — the Start menu, Alt+Tab, notification and Action Center flyouts — sit above every app's windows by design, so they show undimmed while open. Ordinary apps, image viewers, video players and the taskbar are all covered.
- **Secure desktop** (UAC prompts, Ctrl+Alt+Del) is not covered by the overlay, by design.
- **Software dimming cannot brighten.** If a monitor is frozen at 50% by its own menu, nothing can push it past that — turn it up with the monitor's buttons, or enable DDC/CI on it. In *Match all monitors* the other screens are held to that same level so they still match.
- **First detection of a lying monitor** costs a brief jump of the other monitors' backlight while the write is tested. It happens once; the result is remembered.
- **USB display adapters and some KVMs** do not pass DDC/CI through; those monitors fall back to overlay dimming.

---

## License

MIT — see [LICENSE](LICENSE).
