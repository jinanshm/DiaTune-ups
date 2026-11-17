# DiaTweaks (Dia Tune-ups) for Windows

[![Version 1.1.1](https://img.shields.io/badge/version-1.1.1-blue.svg)](https://github.com/jinanshm/DiaTweaks-Win/releases)
[![Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078d4.svg)](https://microsoft.com/windows)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-Support%20Developer-orange.svg)](https://buymeacoffee.com/jinansh_)

A comprehensive suite of tune-ups and enhancements for [Dia](https://www.diabrowser.com) on Windows, ported and extended from [JaceThings/DiaTweaks](https://github.com/JaceThings/DiaTweaks).

Dia on Windows is built with WinUI 3 XAML. DiaTweaks applies tweaks at runtime through live XAML element management, DWM backdrop hints, and non-invasive process helpers—**without modifying Dia's signed MSIX package**.

---

## What's New in Version 1.1.1

- **Hover-free Developer Toolbar**: The dev toolbar icons and striped tab mark now stay visible without hovering the toolbar.
- **One-click, build-aware Updates**: A single button checks GitHub and installs the newest build, detected by git commit rather than published releases.
- **Silent Launch & Apply**: `diatweaks.vbs` and the updater run with no CMD window.
- **Settings Tune-ups Panel**: Full native settings page directly inside Dia's settings window with real-time toggle switches, presets, and sliders.
- **Developer Mode Toolbar**: Arc-style developer tools toolbar (device inspection, console, network panel, element inspector, screenshot captures, and extensions).
- **Peek Link Previews**: Preview links in floating cards before opening (`Shift+click`, `Hold`, `Ctrl+Shift`, `Alt`) with *Genie*, *Ease In/Out*, or *Zoom* animations.
- **Tactile Sidebar Textures**: Procedural grain textures (*Sand*, *Fine Grain*, *Denim weave*, and *Paper fiber*) with customizable intensity and softness sliders.
- **Window Backdrop Materials**: Windows 11 *Mica*, *Mica Alt*, and *Acrylic* materials combined with edge-to-edge frameless geometry.
- **Daily Painting Wallpaper**: Automatic high-resolution fine-art wallpaper on the New Tab page from Dia's Morning Brief catalogue.
- **Segoe UI Variable Typography**: Force Windows 11's Segoe UI Variable font for crisp, legible UI and web fonts.
- **Clean Search Box**: Minimalist single-line New Tab and Command Bar (`Ctrl+T` / `Ctrl+L`).
- **In-Panel Updates & Changelog**: Built-in release update checker and expandable changelog viewer directly within the Tune-ups panel.
- **Support the Developer**: Integrated [Buy Me a Coffee](https://buymeacoffee.com/jinansh_) link.

---

## Installation

### Install from a release (recommended)

1. Download `DiaTuneups-Windows-v1.1.1.zip` from the [Releases page](https://github.com/jinanshm/DiaTweaks-Win/releases).
2. Extract it to a folder of your choice, for example `C:\DiaTuneups`.
3. Double-click `diatweaks.vbs` in that folder to apply the tweaks, or use a shortcut to `wscript.exe "C:\DiaTuneups\diatweaks.vbs" launch` to open Dia and apply them without a console window.
4. To update later, open Dia's Settings, go to the Tune-ups tab, and click **Update Tune-ups**.

### Install from source

Requires Windows 11, [Dia](https://www.diabrowser.com), PowerShell, and (for building) the Visual Studio C++ Build Tools with the Desktop development with C++ workload.

```powershell
git clone https://github.com/jinanshm/DiaTweaks-Win.git
cd DiaTweaks-Win
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
diatweaks.cmd launch
```

---

## Settings &rarr; Tune-ups Panel

You can configure every tweak directly inside Dia's native Settings window:

1. Launch Dia with DiaTweaks (`diatweaks launch` or run `diatweaks apply` while Dia is open).
2. Open **Settings** (`Ctrl+,` or from the App Menu).
3. Click the **Tune-ups** tab in the navigation sidebar (located right after *Advanced*, marked with a wrench & screwdriver icon).
4. From the Tune-ups page, you can:
   - **Switch Presets**: Instant one-click presets for **Arc**, **Arc (Windows)**, **None** (original Dia), or **Custom**.
   - **Toggle any tweak**: Window geometry, toolbar visibility, toolbar icons, sidebar textures, link previews, typography, and wallpaper.
   - **Choose Materials**: Switch backdrop materials (*Mica*, *Acrylic*, *Mica Alt*) and textures (*Sand*, *Fine grain*, *Denim*, *Paper*) with instant live feedback.
   - **Tune Material Translucency**: A **Translucency** slider under *Window material* sets how much the space colour fades out while the sidebar is open, from the full colour (0%) to just the backdrop (100%).
   - **Update Dia Tune-ups**: One click checks GitHub and installs the newest build. It compares the local git commit with the latest commit on `master`, so it catches up to every build — even ones with no published release.
   - **View Changelog**: Expand the in-panel changelog card to inspect version release notes loaded from `changelog.json`.
   - **Support Developer**: One-click button to support continued open-source development via Buy Me a Coffee.
   - **Turn All Off / Open Folder**: Quick reset and instant access to the `%LOCALAPPDATA%\DiaTweaks` data directory in File Explorer.

All changes made in the Settings tab update Dia instantly in real-time and stay synchronized with the CLI (`state.json` and `xaml.cfg`).

---

## Tweaks List

### 1. Window & Frame
| Tweak Key | Title | Windows Status | Description |
|---|---|---|---|
| `edgeToEdge` | Edge-to-edge window | Supported | Removes the window's 1px border and margins so the page meets the window edges. |
| `squareSidebarCorners` | Square window corners | Supported | Requests Windows not to round the window corners (`DWMWCP_DONOTROUND`). Requires `edgeToEdge`. |
| `windowMaterial` | Window material | Supported | Applies backdrop materials behind tabs and sidebar: `default`, `mica`, `acrylic`, `micaalt`, or `none`. |
| `materialTransparency` | Window material translucency | Supported | 0–100%: how much the space colour fades out while the sidebar is open. 100% leaves only the backdrop; hidden sidebar keeps the full colour. Set from the Tune-ups slider. |
| `keepWindowsActive` | Keep display awake | Supported | Keeps the display and system awake while Dia is running. |
| `normalLaunch` | Patch Dia however it's opened | Supported | **On by default.** Keeps tweaks active whenever Dia opens from any shortcut, pinned icon, or link. |

### 2. Toolbar & Visibility
| Tweak Key | Title | Windows Status | Description |
|---|---|---|---|
| `hideToolbar` | Hide toolbar | Supported | Hides the address field, navigation buttons, and extensions. Window controls remain accessible. |
| `toolbarColor` | Toolbar color | Supported | Makes the toolbar transparent so the sidebar color or window backdrop shines through. |
| `revealToolbarOnHover` | Reveal toolbar on hover | Supported | Reveals the toolbar when moving the pointer to the top edge of the window. |
| `showToolbarWithSidebar` | Show toolbar with sidebar | Supported | Keeps the toolbar showing whenever the sidebar is open. |
| `showToolbarOnNewTab` | Show toolbar on new tab | Supported | Keeps the toolbar showing when viewing a new tab or start page. |
| `showAddressField` | Always show address field | Supported | Keeps the address field bookmark and site buttons showing without hovering. |

### 3. Toolbar Items & Controls
| Tweak Key | Title | Windows Status | Description |
|---|---|---|---|
| `hideSidebarButton` | Hide sidebar button | Supported | Hides the sidebar toggle button at the top-left of the toolbar. |
| `hideChatButton` | Hide Chat button | Supported | Hides the Chat button at the top-right of the toolbar. |
| `hidePersonalizeButton` | Hide Personalize button | Supported | Hides the Personalize button in the toolbar. |
| `hideTabSearchButton` | Hide tab search button | Supported | Hides the tab search / overflow button in the window drag header. |
| `hideTrafficLights` | Hide window controls | Supported | Hides the minimize, maximize, and close buttons in the title bar. |
| `hideLoadingAnimation` | Hide loading animation | Supported | Hides the loading line indicator in the address bar. |
| `pinnedExtensions` | Pinned extensions shown | Supported | Shows up to 6, 8, 10, 12 or 16 pinned extensions by the address bar instead of 4. |
| `developerMode` | Developer Mode Toolbar | Supported | Adds Developer Mode toggle to tab/site menus with an Arc-style dev toolbar (console, network, inspector, responsive device, extensions). The striped tab mark and toolbar only appear on tabs that are actually in developer mode, and toolbar icons pick light or dark from the real on-screen toolbar colour. |
| `developerModeLocalhost` | Developer mode for localhost | Supported | Automatically enables developer mode for `localhost` and `127.0.0.1`. |

### 4. Sidebar & Tab Bar
| Tweak Key | Title | Windows Status | Description |
|---|---|---|---|
| `translucentSidebar` | Translucent sidebar | Supported | Lets the sidebar show the window material: with it on, an open sidebar uses Acrylic and the material translucency slider instead of the solid space colour. |
| `sidebarGrain` | Sidebar grain | Supported | Adds procedural tactile texture behind the sidebar and tabs. |
| `sidebarGrainMaterial` | Grain texture material | Supported | Texture choice: `sand`, `grain` (Fine grain), `denim` (Denim weave), or `paper` (Paper fiber). |
| `sidebarGrainAmount` | Sidebar grain amount | Supported | Density slider (0 to 100%) for procedural texture generation. |
| `sidebarGrainSoftness` | Sidebar grain softness | Supported | Softness slider (0 to 100%) for procedural texture blur and blend. |
| `hidePinnedTabBorder` | Hide pinned tab border | Supported | Removes the border outline around pinned tabs in the sidebar. |
| `hideTabHoverCard` | Hide tab hover card | Supported | Stops the preview card from popping up when hovering over open or pinned tabs. |
| `profileEmoji` | Profile emoji | Supported | Displays custom emoji in place of space dots in the space switcher. |

### 5. Page & Search
| Tweak Key | Title | Windows Status | Description |
|---|---|---|---|
| `peek` | Peek link preview | Supported | Preview links in floating cards without navigating away from the current tab. |
| `peekTrigger` | Peek trigger shortcut | Supported | How Peek opens: `shift` (Shift+click), `hold` (Hold on link), `ctrlShift`, or `alt`. |
| `peekAnimation` | Peek animation style | Supported | How the card flies in and out: `genie`, `easeInOut`, or `zoom`. |
| `hideLinkAddress` | Hide link address | Supported | Hides the hover URL preview bubble at the bottom corner of the window. |
| `cleanSearchBox` | Clean search box | Supported | Streamlines the New Tab and Command Bar (`Ctrl+T` / `Ctrl+L`) into a clean single-line input. |
| `newTabPainting` | Daily painting wallpaper | Supported | Displays the Daily Brief's curated fine art of the day as New Tab wallpaper with caption metadata. |
| `dontPopUpDownloads` | Don't pop up downloads | Supported | Stops the downloads list from automatically opening on every download. |
| `hideUpgradeBanner` | Hide upgrade banner | Supported | Hides the Chat usage and upgrade upsell banner. |
| `segoeUiVariable` | Force Segoe UI Variable font | Supported | Forces browser default sans-serif font to Windows 11 Segoe UI Variable instead of system-ui. |
| `notoEmoji` | Noto emoji font | Supported | Draws emoji in Dia's interface with Noto instead of Segoe UI Emoji, on the elements whose text carries an emoji. Requires Noto Color Emoji or Noto Emoji to be installed. |
| `notoEmojiFont` | Emoji font family | Supported | Which Noto family is stacked: `notoColor` (Noto Color Emoji) or `notoMono` (Noto Emoji, monochrome outlines). |

### 6. macOS-Specific / Unavailable Tweaks
The following tweaks from the original macOS DiaTweaks are not available on Windows:

| Tweak Key | Title | Windows Status | Description / Reason |
|---|---|---|---|
| `hideChatsButton` | Hide Chats button | Not available | Dia for Windows does not have a separate sidebar Chats button (macOS specific layout). |
| `compactSidebarHeader` | Compact sidebar header | Not available | The Windows sidebar header uses a different layout and does not expose a separate row to trim. |
| `hideRecentTabsButton` | Hide recent tabs button | Not available | Dia for Windows does not have a separate recent tabs button in the sidebar. |
| `hideNewTabLogo` | Hide new tab logo | Not available | The new-tab logo is drawn inside Dia's page canvas rather than as an individual XAML element. |
| `turnOffAI` | Turn off AI | Not available | Dia's AI features are controlled inside the app rather than by one XAML surface. Hide Chat button removes the main AI entry point. |
| `passkeys` | Allow iCloud passkeys | Not available | Relays passkey requests to Apple Passwords, which only exists on Apple platforms. |
| `icloudPasswords` | Allow iCloud Passwords | Not available | Bridges to Apple's iCloud Passwords extension and native host, which are macOS-only. |

---

## CLI Usage

Run `diatweaks` via `diatweaks.cmd` in the repository root (or add the folder to your `PATH`):

```powershell
diatweaks <command> [tweak...] [-y]
```

> **No CMD window:** For a silent launch/apply (no console window flashing), use
> `diatweaks.vbs` instead, or point a shortcut at it:
> `wscript.exe "C:\path\to\DiaTweaks-Windows\diatweaks.vbs" launch`. The in-panel
> updater also runs entirely hidden.

### Available Commands

| Command | Usage Example | Description |
|---|---|---|
| `list` | `diatweaks list` | Displays every tweak, its current status (`on`, `off`, `n/a`), and description. |
| `enable` | `diatweaks enable hideToolbar edgeToEdge` | Turns one or more tweaks on and immediately applies them to running Dia windows. |
| `disable` | `diatweaks disable hideToolbar` | Turns one or more tweaks off and restores default appearance. |
| `set` | `diatweaks set windowMaterial acrylic`<br>`diatweaks set sidebarGrain denim`<br>`diatweaks set peekTrigger hold`<br>`diatweaks set pinnedExtensions 8` | Configures options for multi-value tweaks (materials, textures, triggers, extensions). |
| `status` | `diatweaks status` | Shows Dia installation info, version, running windows, helper status, and enabled tweaks. |
| `apply` | `diatweaks apply` | Applies current saved configuration to Dia's open windows immediately. |
| `watch` | `diatweaks watch` | Runs continuous watcher service that applies tweaks to any newly opened Dia windows. |
| `launch` | `diatweaks launch` | Starts Dia (if not already running), waits for its window, and applies all active tweaks. |
| `dump` / `tree` | `diatweaks dump` | Dumps Dia's live XAML visual tree element names to `xaml.tree.txt` (for inspection and research). |
| `restore` | `diatweaks restore` | Resets Dia's windows to stock appearance and disables all tweaks. |
| `restore --purge`| `diatweaks restore --purge` | Resets Dia and deletes DiaTweaks saved state (`%LOCALAPPDATA%\DiaTweaks`). |
| `help` | `diatweaks help` | Displays full CLI usage instructions and command reference. |

### CLI Options & Flags
- **`-y` or `--yes`**: Automatically accept dependency prompts (e.g., automatically enabling `edgeToEdge` when enabling `squareSidebarCorners`).
- **Flexible Names**: Tweaks can be referenced by camelCase key (`squareSidebarCorners`) or friendly title (`"Square window corners"`), case-insensitively.

---

## Requirements

- **Operating System**: Windows 11 (build 22000 or later).
- **Browser**: [Dia for Windows](https://www.diabrowser.com) installed via its MSIX package.
- **Shell**: PowerShell 5.1 (built into Windows) or PowerShell 7+.
- **Build Tools** (only needed if compiling from source): Visual Studio Build Tools with "Desktop development with C++" (`vswhere` / `vcvars64.bat`).

---

## Building from Source

To build all three binaries (`diatweaks-agent.exe`, `diatweaks-xaml.dll`, `diatweaks-inject.exe`):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

The compiled binaries are placed into `bin\`:
- `bin\diatweaks-xaml.dll`: Live WinUI 3 XAML helper DLL.
- `bin\diatweaks-agent.exe`: Thread execution state / keep-awake agent.
- `bin\diatweaks-inject.exe`: Low-overhead injector for Dia's browser process.

---

## Project Structure

```
├── .gitignore                     # Git ignore rules for build artifacts and scratch files
├── assets/                        # SVG icons and visual assets (developer toolbar, watermark)
│   └── icons/                     # DevMode icons (camera, console, inspect, network, phone, etc.)
├── bin/                           # Compiled binaries (ignored in git)
├── changelog.json                 # Machine-readable version changelog data for in-panel viewer
├── CHANGELOG.md                   # Human-readable release notes for repository
├── cli/                           # PowerShell CLI implementation
│   ├── diatweaks.ps1              # CLI entry point
│   └── lib/                       # Support modules (dia, injector, state, windows, agent)
├── diatweaks.cmd                  # Batch shim for running diatweaks from Command Prompt / PATH
├── paintings.json                 # Morning Brief fine-art catalogue for New Tab wallpapers
├── patches.json                   # Manifest of all supported tweaks, defaults, and mechanisms
├── README.md                      # Documentation and usage guide
├── scripts/                       # Automation scripts (build.ps1, update-dll.ps1)
├── src/                           # C++ source code
│   ├── agent/                     # Keep-awake background agent
│   ├── injector/                  # Process injector
│   └── xaml/                      # XAML tree injection, DevMode, Peek, and Tune-ups UI
└── tests/                         # Test suites (smoke.ps1)
```

---

## Support the Developer

If you find DiaTweaks helpful and enjoy using Dia with tune-ups on Windows, consider buying me a coffee to support maintenance and future updates:

**[buymeacoffee.com/jinansh_](https://buymeacoffee.com/jinansh_)**

---

## License & Credits

- Unofficial and not affiliated with The Browser Company.
- Based on [JaceThings/DiaTweaks](https://github.com/JaceThings/DiaTweaks).
- Windows WinUI 3 implementation by [jinansh](https://github.com/jinanshm).
