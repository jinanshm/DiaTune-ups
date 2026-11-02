# DiaTweaks for Windows

Small tweaks for [Dia](https://www.diabrowser.com) on Windows, ported from
[JaceThings/DiaTweaks](https://github.com/JaceThings/DiaTweaks).

This is the clean native subset. Everything here uses documented Windows
APIs, and Dia's signed MSIX package is never modified. The macOS add-on cannot
be reused on Windows, and the parts of Dia it changes are compiled XAML that
Windows does not let another process reach. Those tweaks are listed below as
unavailable, with the reason, instead of being faked.

Unofficial and not affiliated with The Browser Company. Windows 11 (build
22000 or later). Tested with the packaged Dia 1.5.0.588
(`TheBrowserCompany.Dia_1.5.0.588_x64__ttt1ap7aakyb4`).

## Tweaks

| Tweak | On Windows | What it does |
|---|---|---|
| `edgeToEdge` | Works | Removes the window's 1px frame border, so the page meets the window edges. |
| `squareSidebarCorners` | Works | Asks Windows not to round the window's corners. Needs `edgeToEdge`. |
| `keepWindowsActive` | Works, adapted | Keeps the display awake while Dia is open. |
| `hideToolbar` | Not available | Dia's toolbar is compiled XAML in a detached content island. |
| `revealToolbarOnHover` | Not available | Builds on `hideToolbar`. |
| `showToolbarWithSidebar` | Not available | Builds on `hideToolbar`. |
| `showToolbarOnNewTab` | Not available | Builds on `hideToolbar`. |
| `showAddressField` | Not available | The address field is compiled XAML with no reachable runtime tree. |
| `hideSidebarButton` | Not available | The sidebar toggle is a XAML element in Dia's navigation bar. |
| `hideChatButton` | Not available | The Chat button is a XAML element in Dia's navigation bar. |
| `hideChatsButton` | Not available | The Chats button is drawn by Dia's own XAML sidebar. |
| `hideRecentTabsButton` | Not available | The recent tabs button is drawn by Dia's own XAML sidebar. |
| `passkeys` | Not available | Relays to Apple Passwords, which only exists on Apple platforms. |
| `icloudPasswords` | Not available | Bridges to Apple's iCloud Passwords extension and native host, which are macOS-only. |

Run `diatweaks list` for the same list, with what is currently on.

## Why the rest can't be ported

The macOS version loads a dylib into Dia and swizzles AppKit classes such as
`NSWindow` and `NSView`. That is possible because AppKit is an Objective-C
framework, and the same runtime is present whether or not the app expects it.

Dia on Windows is a WinUI 3 app. Its chrome is compiled XAML (`.xbf` files such
as `NavigationBar/NavigationBar.xbf` and `ARCUI/MinMaxCloseControl.xbf`) hosted
inside a `Microsoft.UI.Content.DesktopChildSiteBridge`. There is no Windows
equivalent of the AppKit hook. The approaches that were tried and did not work:

- **XAML runtime access.** `Window::Current()` returns null from outside Dia's
  XAML island, and there is no bridge to a XAML island. Registering activation
  factories with `RoRegisterActivationFactories` fails with `0x80040154`
  (class not registered), and `Microsoft.UI.Xaml.dll` exports only eight
  symbols, so there are no diagnostics entry points to borrow.
- **Activation interception.** IAT hooks on `RoGetActivationFactory` and
  `RoActivateInstance` never observe a XAML object being created, because Dia
  activates XAML through the generated `Microsoft.UI.Xaml` projection rather
  than the raw WinRT API surface.
- **UI Automation.** Dia's controls are visible to UIA (`SidebarToggleButton`,
  `ChatButton`, `MinMaxCloseControl`, and so on), but UIA automation peers are
  read-only for these elements: they can report a bounding rectangle, not hide
  or move the element.
- **Preferences.** `isChatButtonHidden` and `sidebarToggleButtonShown` exist in
  `Dia.dll`, but setting them in Dia's plist has no effect; the XAML does not
  read them for visibility.
- **WebView / CDP.** The remote debugging port exposes Dia's web tabs only.
  Dia's chrome is not a web page, so CDP cannot reach it.

The passkey tweaks are a different problem: they hand requests to Apple's
Passwords app through a macOS native messaging host. Windows uses Windows
Hello and has no Apple Passwords app to relay to, so there is nothing to port.

## Requirements

- Windows 11 build 22000 or later. The DWM window attributes used here do not
  exist on Windows 10.
- Dia installed from its MSIX package for the current user.
- PowerShell 5.1 (included with Windows) or PowerShell 7.
- Optional: Visual Studio Build Tools with "Desktop development with C++" to
  build the keep-awake helper. The two window tweaks work without it.

## Build

The CLI is PowerShell and needs no build. Build the keep-awake helper once:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```

This writes `bin\diatweaks-agent.exe`. It uses `vswhere` to find the Visual
Studio toolchain and falls back to `g++` if that is what is installed. Run
`scripts\build.ps1` again after changing the helper.

## Use

Run `diatweaks` from this folder (or put the folder on your `PATH`):

| Command | Does |
|---|---|
| `diatweaks list` | Shows every tweak and whether it's on |
| `diatweaks enable edgeToEdge` | Turns a tweak on and applies it to any open Dia window |
| `diatweaks disable edgeToEdge` | Turns it off and puts the window back |
| `diatweaks status` | Shows Dia's version, its windows, and which tweaks are on |
| `diatweaks apply` | Applies the saved tweaks to Dia's open windows once |
| `diatweaks watch` | Keeps applying them every two seconds until Ctrl+C |
| `diatweaks launch` | Opens Dia, waits for its window, then applies the tweaks |
| `diatweaks restore` | Puts Dia's windows back and turns every tweak off |
| `diatweaks restore --purge` | The same, and deletes DiaTweaks' saved settings |

Tweak names work as keys or titles, like `squareSidebarCorners` or
`"Square window corners"`, ignoring case, spaces and punctuation. Add `-y` or
`--yes` to answer yes to every question, which is useful in scripts.

`squareSidebarCorners` needs `edgeToEdge`. Turning it on offers to turn
`edgeToEdge` on too; `-y` accepts that automatically.

### Keeping the tweaks on

DWM window attributes belong to a window, not to Dia's profile. They survive
until that window closes, and they are lost when Dia recreates it (restarting
Dia, for example). There are two supported ways to keep them applied:

```powershell
diatweaks launch        # open Dia and apply them once
diatweaks watch         # leave this running; it applies them as windows appear
```

`enable` and `disable` apply immediately when a Dia window is open. They do not
start a background watcher. If you want the tweaks every time without thinking
about it, put a shortcut to `diatweaks launch` in your Start menu, or run
`diatweaks watch` at login.

Stop a running `watch` with Ctrl+C before `restore --purge`; otherwise it sees
the defaults again and puts the tweaks straight back on.

## How it works

- **`edgeToEdge`** sets `DWMWA_BORDER_COLOR` (attribute 34) to
  `DWMWA_COLOR_NONE` on Dia's top-level window. This removes the frame border
  DWM draws around the window.
- **`squareSidebarCorners`** sets `DWMWA_WINDOW_CORNER_PREFERENCE`
  (attribute 33) to `DWMWCP_DONOTROUND`. Both calls are ordinary
  `DwmSetWindowAttribute` calls made from the CLI, not from inside Dia.
- **`keepWindowsActive`** starts `diatweaks-agent.exe`, a separate helper that
  holds `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
  ES_DISPLAY_REQUIRED)` while any `Dia.exe` process exists. The helper exits
  when Dia does and holds a named mutex so only one copy runs per session.

Nothing is injected into Dia, no package files are changed, and no DLL is
loaded into the browser. `diatweaks restore` removes the window attributes and
stops the helper.

State is kept in `%LOCALAPPDATA%\DiaTweaks\state.json`. `patches.json` in this
folder is the source of truth for tweak names, order, defaults and
availability.

## Files

```
diatweaks.cmd                    Command shim
cli\diatweaks.ps1                Command line interface
cli\lib\windows.ps1              Win32 and DWM interop
cli\lib\dia.ps1                  Package, process and window discovery
cli\lib\state.ps1                Manifest, saved state, applying
src\agent\diatweaks-agent.cpp    Keep-awake helper
scripts\build.ps1                Builds the helper
tests\smoke.ps1                  Manifest and CLI checks
patches.json                     Tweak list
```

Run the checks with:

```powershell
powershell -ExecutionPolicy Bypass -File tests\smoke.ps1
```

## Limitations

- The window attributes need Windows 11 and are applied per window. Use
  `launch` or `watch` if you want them every time.
- `squareSidebarCorners` squares the whole window, not only the sidebar. The
  macOS tweak could reshape the sidebar's own corners because it had AppKit
  views to work with; DWM only offers the window-level preference.
- `edgeToEdge` removes the border DWM draws around the window. It cannot remove
  padding inside Dia's chrome; the macOS tweak could, because it could move
  AppKit views. That needs the same XAML access as the toolbar tweaks.
- `keepWindowsActive` is an adaptation, not the macOS behaviour. macOS kept an
  unfocused window looking active; Windows instead keeps the display and
  system awake while Dia is open. It does not stop Dia from being throttled in
  the background.
- Hiding the toolbar and its buttons needs access to Dia's XAML, which is not
  available. See "Why the rest can't be ported".
- Dia updates replace the app package, but they do not touch DiaTweaks. Because
  the window class is matched by name with a size-and-title fallback, minor Dia
  updates should keep working; a major UI rewrite may need the class name in
  `cli\lib\dia.ps1` updated.

## Uninstall

```powershell
diatweaks restore --purge
```

Then delete this folder. Dia's own files and profile are untouched, so there is
nothing else to undo.

## Credits

Based on [JaceThings/DiaTweaks](https://github.com/JaceThings/DiaTweaks). The
Windows port only reimplements the tweaks that map to documented Windows APIs.
