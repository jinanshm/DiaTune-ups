// devmode.h - the Developer Mode tweak.
//
// Arc's Developer Mode turns a site into a development site: the tab is marked
// in the sidebar, and the command bar swaps its extension buttons for a set of
// development tools (console, network, inspector, capture, extensions).
//
// On Windows the same idea is built on two things Dia already has:
//
//   * Its XAML tree. The site settings menu, the tab menu, the sidebar rows and
//     the command bar are all WinUI elements, so the tweak adds its menu items
//     and its toolbar to them directly, the way the other tweaks here do.
//
//   * Chromium's DevTools. Dia is a Chromium browser, so the console, the
//     network panel and the element inspector are Chromium's own. The developer
//     toolbar opens them over the DevTools protocol (see cdp.h).

#pragma once

#include <string>

namespace DevMode {

// Turns the tweak on or off. When it is off, everything the tweak added to
// Dia is taken away again.
void SetEnabled(bool enabled);

// Whether localhost and 127.0.0.1 count as developer sites without being
// switched on by hand.
void SetLocalhost(bool enabled);

// Whether the tweak is on.
bool Enabled();

// Called on the UI thread on every timer tick. Keeps the menu items, the
// sidebar marks and the command bar in step with the page in front.
void Tick();

// The origin (scheme://host:port) of the page the user is looking at, or an
// empty string when it cannot be told.
std::wstring CurrentOrigin();

// Whether the currently active tab has developer mode turned on.
bool ActiveTabInDevMode();

}  // namespace DevMode

// Returns whether the current tab in Dia is the New Tab page
bool IsDiaNewTab();
