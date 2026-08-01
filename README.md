# Desktop Brightness Adjuster

An ultra-lightweight, zero-latency desktop brightness controller for Windows 11 and 10.

![Language](https://img.shields.io/badge/Language-C%23-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011-0078D6.svg)
![RAM Footprint](https://img.shields.io/badge/RAM-~6%20MB-brightgreen.svg)
![Startup Impact](https://img.shields.io/badge/Startup%20Impact-Low%20(%3C1ms)-success.svg)

---

## Features

- **0ms Latency**: Instant response via dual-layer architecture — seamless UI overlay combined with background DDC/CI hardware I2C synchronization.
- **1% Fine Increments**: Fine-grained brightness adjustments for precise light tuning.
- **Ultra-Minimalist Center OSD**: Sleek matte-black pill badge HUD showing exact brightness percentage.
- **True Multi-Monitor and OLED Screen Dimmer**: Seamless click-through dark overlay that works on hardware monitors and laptop screens alike.
- **Low Startup Impact (< 1ms CPU)**: Deferred hardware initialization ensures Windows boots in milliseconds without background bloat.
- **Single-File C# / Zero External Dependencies**: Compiles out-of-the-box using standard Windows `.NET csc.exe`.

---

## Default Hotkeys

| Action | Hotkey |
| :--- | :--- |
| **Increase Brightness (+1%)** | `Alt` + `Page Up` |
| **Decrease Brightness (-1%)** | `Alt` + `Page Down` |

---

## How to Customize Your Own Hotkeys

You can easily change the hotkey combination to whatever suits your setup (e.g., `Ctrl` + `Up/Down`, `Shift` + `F11/F12`, etc.).

### 1. Open `DesktopBrightnessHotkeys.cs` in any text editor.
### 2. Locate lines 22-24:
```csharp
private const uint MOD_ALT = 0x0001;
private const uint VK_PRIOR = 0x21; // Page Up
private const uint VK_NEXT = 0x22;  // Page Down
```

### 3. Replace the Modifier & Key values:

#### Modifier Keys (`fsModifiers`):
| Modifier | Hex Code |
| :--- | :--- |
| `Alt` | `0x0001` |
| `Ctrl` | `0x0002` |
| `Shift` | `0x0004` |
| `Win Key` | `0x0008` |
| `Ctrl + Alt` | `0x0003` |

#### Popular Virtual-Key Codes (`vk`):
| Key | Hex Code | Key | Hex Code |
| :--- | :--- | :--- | :--- |
| `Page Up` | `0x21` | `Up Arrow` | `0x26` |
| `Page Down` | `0x22` | `Down Arrow` | `0x28` |
| `F11` | `0x7A` | `F12` | `0x7B` |
| `Volume Up` | `0xAF` | `Volume Down` | `0xAE` |

*Example for `Ctrl` + `Up Arrow` / `Down Arrow`:*
```csharp
private const uint MOD_ALT = 0x0002; // Ctrl
private const uint VK_PRIOR = 0x26; // Up Arrow
private const uint VK_NEXT = 0x28;  // Down Arrow
```

---

## Building from Source

You can compile the executable in 1 second using native Windows `.NET Framework`:

```powershell
powershell -ExecutionPolicy Bypass -File build_brightness_app.ps1
```

Or directly via `csc.exe`:
```cmd
C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /target:winexe /out:DesktopBrightness.exe /optimize+ /r:System.dll /r:System.Windows.Forms.dll /r:System.Drawing.dll DesktopBrightnessHotkeys.cs
```

---

## Author

Developed by **[Charlise28](https://github.com/Charlise28)** 
