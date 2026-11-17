# Changelog

All notable changes to **DiaTweaks (Dia Tune-ups) for Windows** will be documented in this file.

## [1.1.1] - 2026-11-17

### Added
- **Material Translucency Slider**: New slider under *Window → Window material* in the Tune-ups panel. It sets how far the space colour fades while the sidebar is open (0% keeps the full colour, 100% leaves only the Mica/Acrylic backdrop). Saved as `materialTransparency` in `state.json` and `xaml.cfg`.
- **Silent Launcher**: `diatweaks.vbs` applies/launches the tweaks with no CMD window. Point a shortcut at it (`wscript.exe "...\diatweaks.vbs" launch`) for a completely silent launch.

### Changed
- With *Translucent sidebar* on and the sidebar open, the window now uses an Acrylic backdrop even when *Mica* is selected, so the desktop shows through the way the Settings window does.
- The sidebar space colour no longer keeps a fixed tint while the sidebar is open; the new slider controls it instead.
- **One-click updates**: The update card is now a single *Update Tune-ups* button. One click checks GitHub, tells you what was found, then pulls and rebuilds automatically.
- **Smarter update detection**: New builds are detected by comparing the checked-out git commit with GitHub's latest commit on `master`, so an update is found even when no GitHub release was published or the hard-coded version was not bumped. Falls back to the changelog version when there is no local git checkout.
- **No console windows**: The in-panel updater and the `update.ps1`/`update-dll.ps1` scripts now start hidden child processes, so applying an update never flashes an open CMD window.
- **Patch Dia however it's opened** (`normalLaunch`) is now on by default, so a startup watcher keeps the tweaks active for any shortcut, pinned icon, or link.

### Fixed
- The developer-mode striped tab mark and toolbar no longer jump to a tab the user just switched to. The mark waits until the page tracker has described the new tab instead of reusing the previous tab's origin for a second.
- **Developer-mode toolbar icons no longer appear only on hover.** Dia's trailing toolbar stack has a hover-only opacity binding; it was being restored whenever the page tracker momentarily disagreed about the active tab. The developer state and toolbar now have a short stable latch, and the stack is kept opaque while the toolbar is actually present.
- Developer-mode toolbar icons now choose light or dark from the real on-screen toolbar colour (including Mica/Acrylic and custom site toolbars) instead of the inherited app theme, and hover no longer rebuilds or flickers the strip.
- Toolbar palette changes now recolour the existing developer toolbar in place instead of rebuilding it.

## [1.1.0] - 2026-11-15

### Added
- **Tune-ups Settings Panel**: Full settings page integrated directly into Dia's native Settings sidebar (with live presets: *Arc*, *Arc for Windows*, *None*, and *Custom*).
- **Developer Mode Toolbar**: Developer tools toggle in tab and site menus with Arc-inspired toolbar: Responsive device mode, element inspector, console, network panel, and extension tools.
- **Peek Link Preview**: Preview links in an animated floating card before navigating (`Shift+click`, `Hold`, `Ctrl+Shift+click`, or `Alt+click`) with *Genie*, *Ease In/Out*, or *Zoom* animations.
- **Tactile Sidebar Grain**: Procedural texture generator supporting *Sand*, *Fine Grain*, *Denim weave*, and *Paper fiber* textures with interactive Amount and Softness sliders.
- **Window Backdrop Materials**: Support for Windows 11 *Mica*, *Mica Alt*, and *Acrylic* backdrops combined with edge-to-edge frameless styling.
- **Daily Painting Wallpaper**: Displays curated fine art from Dia's Morning Brief as high-resolution wallpaper on the New Tab page.
- **Segoe UI Variable Font**: Option to force Windows 11's modern Segoe UI Variable typography instead of system defaults.
- **Clean Search Box**: Minimalist single-line New Tab and Command Bar (`Ctrl+T` / `Ctrl+L`).
- **Profile Emojis**: Custom emoji icons replacing color dots in the space switcher.
- **In-Panel Updates & Changelog**: Built-in update checker card and expandable changelog viewer directly within the Tune-ups panel.
- **Support the Developer**: Integrated Buy Me a Coffee link (`https://buymeacoffee.com/jinansh_`) to support ongoing open-source development.
