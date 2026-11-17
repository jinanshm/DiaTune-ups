# Changelog

All notable changes to **DiaTweaks (Dia Tune-ups) for Windows** will be documented in this file.

## [Unreleased]

### Added
- **Material Translucency Slider**: New slider under *Window → Window material* in the Tune-ups panel. It sets how far the space colour fades while the sidebar is open (0% keeps the full colour, 100% leaves only the Mica/Acrylic backdrop). Saved as `materialTransparency` in `state.json` and `xaml.cfg`.

### Changed
- With *Translucent sidebar* on and the sidebar open, the window now uses an Acrylic backdrop even when *Mica* is selected, so the desktop shows through the way the Settings window does.
- The sidebar space colour no longer keeps a fixed tint while the sidebar is open; the new slider controls it instead.

### Fixed
- The developer-mode striped tab mark and toolbar no longer jump to a tab the user just switched to. The mark waits until the page tracker has described the new tab instead of reusing the previous tab's origin for a second.
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
