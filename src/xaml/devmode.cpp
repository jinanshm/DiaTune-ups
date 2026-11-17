// devmode.cpp - the Developer Mode tweak (see devmode.h).
//
// Arc's Developer Mode turns a site into a development site. The tab in the
// sidebar is marked, and the command bar swaps the extension buttons for a
// row of development tools.

#include "devmode.h"

#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <wincodec.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Text.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

#include "cdp.h"
#include "engine.h"

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxc_primitives = winrt::Microsoft::UI::Xaml::Controls::Primitives;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxa_peers = winrt::Microsoft::UI::Xaml::Automation::Peers;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxm_imaging = winrt::Microsoft::UI::Xaml::Media::Imaging;
namespace muxsh = winrt::Microsoft::UI::Xaml::Shapes;
using Inspectable = winrt::Windows::Foundation::IInspectable;

#define Log Engine_Log

namespace DevMode {
namespace {

constexpr wchar_t kSiteItemName[] = L"DiaTweaksDevModeSiteItem";
constexpr wchar_t kTabItemName[] = L"DiaTweaksDevModeTabItem";
constexpr wchar_t kToolbarName[] = L"DiaTweaksDevModeToolbar";
constexpr wchar_t kMarkName[] = L"DiaTweaksDevModeMark";

constexpr wchar_t kDevModeText[] = L"Developer Mode";

constexpr int kDevToolsPort = 9222;

// How long the developer UI stays off after the selected tab changed, while
// the page tracker and the address bar still describe the previous tab.
constexpr unsigned long long kTabSwitchGraceMs = 600;

constexpr uint32_t kMarkYellow = 0xFFD700;
constexpr uint32_t kMarkBlack = 0x000000;

std::mutex g_lock;
bool g_enabled = false;
bool g_localhost = true;
std::set<std::wstring> g_sites;
std::set<std::wstring> g_devTabs;
std::set<std::wstring> g_disabledTabs;

InstanceHandle g_markedRowHandle = 0;
winrt::weak_ref<mux::FrameworkElement> g_toolbar;

void ShowToolbar();
void HideToolbar();
void UpdateToolbar();
void MarkSidebarTab();
std::wstring GetSelectedTabTitle();
HWND GetDiaMainWindow();
HWND FindChromiumChild(HWND parent);

InstanceHandle TreeRoot(InstanceHandle handle) {
    InstanceHandle root = handle;
    InstanceHandle parent = Engine_Parent(handle);
    int guard = 0;
    while (parent && guard++ < 40) {
        root = parent;
        parent = Engine_Parent(parent);
    }
    return root;
}

bool SameWindow(InstanceHandle a, InstanceHandle b) {
    if (!a || !b) return true;
    InstanceHandle ra = TreeRoot(a);
    InstanceHandle rb = TreeRoot(b);
    if (ra == a || rb == b) return true;
    return ra == rb;
}

std::atomic<bool> g_workerRunning{false};
std::atomic<bool> g_stopWorker{false};
// Set when the selected tab changed: the worker wakes up early instead of
// waiting for the end of its poll interval.
std::atomic<bool> g_wakeWorker{false};

struct Page {
    std::wstring url;
    std::wstring origin;
    std::wstring title;
    std::wstring targetId;
    std::wstring socket;
    bool devTools = false;
};

Page g_page;
// Dia's tab row switches instantly while the page tracker only learns about the
// new tab on its next pass. Until this time the tracker's origin (and the
// address bar, which lags the same way) still describe the previous tab, so the
// developer UI must not trust either of them.
std::atomic<ULONGLONG> g_pageStaleUntil{0};
// The page tracker and tab title can briefly disagree about the active tab.
// Keep the developer state "active" for a short grace after a real detection so
// those misses don't tear down the toolbar and restore Dia's hover-only opacity
// (which makes the icons appear only while the pointer is over the toolbar).
constexpr unsigned long long kDevActiveLatchMs = 1200;
std::atomic<ULONGLONG> g_devActiveUntil{0};
std::wstring g_iniPath;

// Gradient presets for Portrait Card capture
struct GradientPreset {
    const wchar_t* name;
    uint32_t color1; // RGB
    uint32_t color2; // RGB
};

const GradientPreset kGradients[] = {
    { L"Sunset",  0xFF5E62, 0xFFB95E },
    { L"Sunrise", 0xF857A6, 0xFF5858 },
    { L"Ocean",   0x2193B0, 0x6DD5ED },
    { L"Aurora",  0x11998E, 0x38EF7D },
    { L"Dusk",    0x8E2DE2, 0x4A00E0 },
};
int g_selectedGradient = 0;

std::wstring ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(),
                        size);
    return wide;
}

std::string ToNarrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                    nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), narrow.data(),
                        size, nullptr, nullptr);
    return narrow;
}

winrt::Windows::UI::Color ColorFrom(uint32_t bgr) {
    return winrt::Windows::UI::ColorHelper::FromArgb(
        0xFF, static_cast<uint8_t>((bgr >> 16) & 0xFF),
        static_cast<uint8_t>((bgr >> 8) & 0xFF), static_cast<uint8_t>(bgr & 0xFF));
}

winrt::Windows::UI::Color ColorFrom(uint32_t bgr, uint8_t alpha) {
    return winrt::Windows::UI::ColorHelper::FromArgb(
        alpha, static_cast<uint8_t>((bgr >> 16) & 0xFF),
        static_cast<uint8_t>((bgr >> 8) & 0xFF), static_cast<uint8_t>(bgr & 0xFF));
}

muxm::LinearGradientBrush MakeGradientBrush(uint32_t c1, uint32_t c2) {
    muxm::LinearGradientBrush brush;
    brush.StartPoint(winrt::Windows::Foundation::Point{0.0f, 0.0f});
    brush.EndPoint(winrt::Windows::Foundation::Point{1.0f, 1.0f});

    muxm::GradientStop stop1;
    stop1.Color(ColorFrom(c1));
    stop1.Offset(0.0);
    brush.GradientStops().Append(stop1);

    muxm::GradientStop stop2;
    stop2.Color(ColorFrom(c2));
    stop2.Offset(1.0);
    brush.GradientStops().Append(stop2);

    return brush;
}

HWND FindChromiumChild(HWND parent) {
    // Find the PAGE renderer (left-/top-most), not DevTools renderer.
    // When DevTools is docked, there are 2 Chrome_RenderWidgetHostHWND children;
    // the page one is further left (or further up if docked bottom).
    std::vector<HWND> list;
    EnumChildWindows(parent, [](HWND hwnd, LPARAM lp) -> BOOL {
        wchar_t cls[128]{};
        GetClassNameW(hwnd, cls, 128);
        if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0) {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));

    if (list.empty()) return nullptr;
    if (list.size() == 1) return list[0];

    // Pick the leftmost (page content) — DevTools panel is to the right or below.
    HWND best = list[0];
    RECT bestRect{};
    GetWindowRect(best, &bestRect);
    for (size_t i = 1; i < list.size(); ++i) {
        RECT r{};
        GetWindowRect(list[i], &r);
        if (r.left < bestRect.left || (r.left == bestRect.left && r.top < bestRect.top)) {
            best = list[i];
            bestRect = r;
        }
    }
    return best;
}

// Cleans up any stuck "Copied current URL" text or popup in Dia's address bar
void ClearStuckCopiedText(bool force = false) {
    static ULONGLONG lastRun = 0;
    ULONGLONG now = GetTickCount64();
    if (!force && now - lastRun < 2000) return;
    lastRun = now;

    Engine_ForEachNode([](const NodeRef& node) {
        if (node.type == L"Microsoft.UI.Xaml.Controls.TextBlock") {
            IInspectable* raw = Engine_Object(node.handle);
            if (!raw) return;
            Inspectable object{nullptr};
            winrt::copy_from_abi(object, raw);
            raw->Release();

            if (auto tb = object.try_as<muxc::TextBlock>()) {
                try {
                    std::wstring_view txt = tb.Text();
                    if (txt.find(L"Copied") != std::wstring_view::npos) {
                        tb.Visibility(mux::Visibility::Collapsed);
                        tb.Text(L"");
                        mux::DependencyObject p = tb.Parent();
                        for (int i = 0; i < 6 && p; ++i) {
                            if (auto popup = p.try_as<muxc_primitives::Popup>()) {
                                popup.IsOpen(false);
                                break;
                            }
                            p = muxm::VisualTreeHelper::GetParent(p);
                        }
                    }
                } catch (...) {}
            }
        } else if (node.type == L"Microsoft.UI.Xaml.Controls.Primitives.Popup") {
            IInspectable* raw = Engine_Object(node.handle);
            if (!raw) return;
            Inspectable object{nullptr};
            winrt::copy_from_abi(object, raw);
            raw->Release();

            if (auto popup = object.try_as<muxc_primitives::Popup>()) {
                try {
                    if (popup.IsOpen()) {
                        if (auto child = popup.Child()) {
                            std::vector<mux::DependencyObject> stack;
                            stack.push_back(child);
                            int guard = 0;
                            while (!stack.empty() && guard++ < 30) {
                                auto cur = stack.back();
                                stack.pop_back();
                                if (auto tb = cur.try_as<muxc::TextBlock>()) {
                                    if (std::wstring_view(tb.Text()).find(L"Copied") != std::wstring_view::npos) {
                                        popup.IsOpen(false);
                                        break;
                                    }
                                }
                                int cnt = muxm::VisualTreeHelper::GetChildrenCount(cur);
                                for (int c = 0; c < cnt; ++c) {
                                    stack.push_back(muxm::VisualTreeHelper::GetChild(cur, c));
                                }
                            }
                        }
                    }
                } catch (...) {}
            }
        }
    });
}

// ---- the state file -------------------------------------------------------

std::wstring IniPath() {
    if (!g_iniPath.empty()) return g_iniPath;
    wchar_t local[MAX_PATH]{};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (size == 0 || size > MAX_PATH) return std::wstring();
    std::wstring dir(local);
    g_iniPath = dir + L"\\DiaTweaks\\devmode.ini";
    return g_iniPath;
}

void LoadSites() {
    g_sites.clear();
    g_devTabs.erase(L"page");
    std::wstring path = IniPath();
    if (path.empty()) return;

    std::vector<unsigned char> data;
    if (!Cdp::ReadWholeFile(path.c_str(), data)) return;

    std::string text(data.begin(), data.end());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;

        size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string origin = line.substr(0, equals);
        std::string value = line.substr(equals + 1);

        while (!origin.empty() && (origin.front() == ' ' || origin.front() == '\t')) origin.erase(origin.begin());
        while (!origin.empty() && (origin.back() == '\r' || origin.back() == ' ' || origin.back() == '\t')) origin.pop_back();
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) value.pop_back();

        if (value != "1" || origin.empty() || origin == "active_tab") continue;
        g_sites.insert(ToWide(origin));
    }
}

void SaveSites() {
    std::wstring path = IniPath();
    if (path.empty()) return;

    std::string text = "[sites]\r\n";
    for (const std::wstring& site : g_sites) {
        std::wstring trimmed = site;
        while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L' ' || trimmed.back() == L'\t')) trimmed.pop_back();
        if (!trimmed.empty()) {
            text += ToNarrow(trimmed) + "=1\r\n";
        }
    }

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

// ---- sites ----------------------------------------------------------------

std::wstring OriginOf(const std::wstring& url) {
    if (url.empty()) return std::wstring();
    if (url.compare(0, 6, L"dia://") == 0 ||
        url.compare(0, 9, L"chrome://") == 0 ||
        url.compare(0, 6, L"about:") == 0 ||
        url.compare(0, 11, L"devtools://") == 0) {
        return std::wstring();
    }

    size_t scheme = url.find(L"://");
    if (scheme == std::wstring::npos) return std::wstring();
    size_t host = scheme + 3;
    size_t end = url.find_first_of(L"/?#", host);
    std::wstring authority =
        url.substr(host, end == std::wstring::npos ? std::wstring::npos : end - host);
    if (authority.empty()) return std::wstring();

    std::wstring schemePart = url.substr(0, scheme);
    size_t colon = authority.rfind(L':');
    if (colon != std::wstring::npos && authority.find(L']') == std::wstring::npos) {
        std::wstring port = authority.substr(colon + 1);
        bool digits = !port.empty();
        for (wchar_t c : port) {
            if (c < L'0' || c > L'9') digits = false;
        }
        if (digits) {
            bool defaultPort = (schemePart == L"https" && port == L"443") ||
                               (schemePart == L"http" && port == L"80");
            if (!defaultPort) return schemePart + L"://" + authority;
        }
    }
    return schemePart + L"://" + authority;
}

bool IsLocalhost(const std::wstring& origin) {
    if (origin.empty()) return false;
    size_t scheme = origin.find(L"://");
    if (scheme == std::wstring::npos) return false;
    std::wstring host = origin.substr(scheme + 3);
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host = host.substr(0, colon);
    if (host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]" || host == L"::1")
        return true;
    if (host.compare(0, 4, L"127.") == 0) return true;
    return false;
}

std::wstring NormalizeUrl(const std::wstring& raw) {
    if (raw.empty()) return L"";
    if (raw.find(L"://") != std::wstring::npos) return raw;
    if (raw.starts_with(L"localhost") || raw.starts_with(L"127.0.0.1") || raw.starts_with(L"[::1]")) {
        return L"http://" + raw;
    }
    return L"https://" + raw;
}

std::wstring GetAddressBarUrl() {
    std::wstring result;
    for (const wchar_t* boxName : { L"QueryTextBox", L"AddressRichEditBox" }) {
        for (auto handle : Engine_FindNodes(boxName)) {
            IInspectable* raw = Engine_Object(handle);
            if (!raw) continue;
            Inspectable object{nullptr};
            winrt::copy_from_abi(object, raw);
            raw->Release();

            if (auto reb = object.try_as<muxc::RichEditBox>()) {
                try {
                    winrt::hstring text;
                    reb.Document().GetText(winrt::Microsoft::UI::Text::TextGetOptions::None, text);
                    std::wstring s(text);
                    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' '))
                        s.pop_back();
                    if (!s.empty() && s != L"Search or enter URL" && s != L"Type to search") {
                        result = s;
                        return result;
                    }
                } catch (...) {}
            }
        }
    }
    return result;
}

bool SiteInDevMode(const std::wstring& origin) {
    if (origin.empty()) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_sites.count(origin)) return true;
    return g_localhost && IsLocalhost(origin);
}

std::wstring PageOrigin() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_page.origin;
}

bool IsNewTabPage() {
    if (IsDiaNewTab()) return true;
    std::wstring title = GetSelectedTabTitle();
    if (!title.empty() && title != L"New Tab" && title != L"New tab") {
        return false;
    }
    std::wstring addr = GetAddressBarUrl();
    if (!addr.empty() && addr.find(L"newtab") == std::wstring::npos) {
        return false;
    }
    if (title == L"New Tab" || title == L"New tab") return true;
    if (!addr.empty() && addr.find(L"newtab") != std::wstring::npos) return true;

    // Fallback: check if Chromium render window is present and visible
    HWND win = GetDiaMainWindow();
    if (win) {
        HWND chrom = FindChromiumChild(win);
        if (chrom && IsWindowVisible(chrom)) {
            RECT rc{};
            GetClientRect(chrom, &rc);
            if (rc.right - rc.left > 100 && rc.bottom - rc.top > 100) {
                return false;
            }
        }
    }
    return true;
}

bool TabInDevMode(const Page& page) {
    if (IsNewTabPage()) return false;
    if (page.origin.empty() || page.origin == L"active_tab") return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (!page.targetId.empty() && page.targetId != L"page" && g_disabledTabs.count(page.targetId)) return false;
    if (!page.targetId.empty() && page.targetId != L"page" && g_devTabs.count(page.targetId)) return true;
    if (g_sites.count(page.origin)) return true;
    if (g_localhost && IsLocalhost(page.origin)) return true;
    return false;
}

Page CurrentPage() {
    std::lock_guard<std::mutex> lock(g_lock);
    Page page = g_page;
    if (IsNewTabPage()) {
        page.url.clear();
        page.origin.clear();
        page.targetId.clear();
        page.title = L"New Tab";
        return page;
    }
    if (page.origin.empty() && GetTickCount64() >= g_pageStaleUntil.load()) {
        // Only trust the address bar once it has caught up with the tab switch:
        // right after switching it still shows the tab the user just left.
        std::wstring addr = NormalizeUrl(GetAddressBarUrl());
        if (!addr.empty() && addr.find(L"newtab") == std::wstring::npos) {
            page.url = addr;
            page.origin = OriginOf(addr);
            if (page.targetId.empty()) page.targetId = L"page";
        }
    }
    return page;
}

bool ActiveTabInDevMode() {
    if (IsNewTabPage()) return false;
    // The tab row changed but the page tracker has not described the new tab
    // yet: report "not in developer mode" instead of applying the previous
    // tab's developer mark and toolbar to this one for a moment.
    // A real tab switch clears the latch below, so the toolbar still tears down
    // promptly when the user leaves a developer tab.
    if (GetTickCount64() < g_pageStaleUntil.load()) return false;
    if (TabInDevMode(CurrentPage())) {
        g_devActiveUntil.store(GetTickCount64() + kDevActiveLatchMs);
        return true;
    }
    // Momentary tracker misses (reload, title rewrite, slow CDP response) must
    // not restore the hover-only opacity on the toolbar that holds our icons.
    return GetTickCount64() < g_devActiveUntil.load();
}

void ToggleCurrentTabDevMode() {
    if (IsNewTabPage()) {
        Log("developer mode: cannot toggle on New Tab");
        return;
    }
    Page page = CurrentPage();
    if (page.origin.empty()) {
        std::wstring addr = NormalizeUrl(GetAddressBarUrl());
        page.origin = OriginOf(addr);
        if (page.origin.empty()) {
            page.origin = OriginOf(NormalizeUrl(GetSelectedTabTitle()));
        }
    }
    if (page.origin.empty() || page.origin == L"active_tab") {
        Log("developer mode: no active valid origin to toggle");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_lock);
        bool currentlyOn = false;
        if (!page.targetId.empty() && page.targetId != L"page" && g_disabledTabs.count(page.targetId)) {
            currentlyOn = false;
        } else if (!page.targetId.empty() && page.targetId != L"page" && g_devTabs.count(page.targetId)) {
            currentlyOn = true;
        } else if (g_sites.count(page.origin) || (g_localhost && IsLocalhost(page.origin))) {
            currentlyOn = true;
        }

        if (currentlyOn) {
            if (!page.targetId.empty() && page.targetId != L"page") {
                g_devTabs.erase(page.targetId);
                g_disabledTabs.insert(page.targetId);
            }
            g_sites.erase(page.origin);
            for (auto it = g_sites.begin(); it != g_sites.end(); ) {
                std::wstring s = *it;
                while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.pop_back();
                while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
                if (s == page.origin) {
                    it = g_sites.erase(it);
                } else {
                    ++it;
                }
            }
            SaveSites();
            Log("developer mode: turned OFF for tab %ls (%ls)", page.targetId.c_str(), page.origin.c_str());
        } else {
            if (!page.targetId.empty() && page.targetId != L"page") {
                g_disabledTabs.erase(page.targetId);
                g_devTabs.insert(page.targetId);
            }
            if (!page.origin.empty() && page.origin != L"active_tab") {
                std::wstring clean = page.origin;
                while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'\t' || clean.back() == L'\r')) clean.pop_back();
                while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'\t')) clean.erase(clean.begin());
                g_sites.insert(clean);
                SaveSites();
            }
            Log("developer mode: turned ON for tab %ls (%ls)", page.targetId.c_str(), page.origin.c_str());
        }
        g_page = page;
    }

    UpdateToolbar();
    MarkSidebarTab();
}

void ToggleSite(const std::wstring& origin) {
    if (origin.empty()) return;

    {
        std::lock_guard<std::mutex> lock(g_lock);
        auto it = g_sites.find(origin);
        if (it != g_sites.end()) {
            g_sites.erase(it);
        } else {
            g_sites.insert(origin);
        }
        SaveSites();
    }
    Log("developer mode: %ls is now %s", origin.c_str(),
        SiteInDevMode(origin) ? "a developer site" : "an ordinary site");
}

// Reads the title or accessible name of the currently selected tab in RowsPanel
HWND GetDiaMainWindow() {
    DWORD myPid = GetCurrentProcessId();
    HWND top = GetForegroundWindow();
    if (top) {
        DWORD pid = 0;
        GetWindowThreadProcessId(top, &pid);
        if (pid == myPid) {
            wchar_t cls[128]{};
            GetClassNameW(top, cls, 128);
            if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0) return top;
        }
    }
    HWND found = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t cls[128]{};
        GetClassNameW(h, cls, 128);
        if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0) {
            wchar_t title[128]{};
            GetWindowTextW(h, title, 128);
            if (title[0] != L'\0') {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// Reads the title or accessible name of the currently selected tab in Dia
std::wstring GetSelectedTabTitle() {
    // Primary & most accurate source: Dia's main window caption, which
    // is always "<Profile>: <Tab Title>" (e.g. "Personal: GitHub", "Personal: Elon Musk on X: ...")
    HWND win = GetDiaMainWindow();
    if (win) {
        wchar_t title[512]{};
        GetWindowTextW(win, title, ARRAYSIZE(title));
        std::wstring t(title);
        if (!t.empty()) {
            // Strip any " (Not Responding)" or " - Dia" suffix
            const wchar_t kNotResp[] = L" (Not Responding)";
            size_t nr = t.rfind(kNotResp);
            if (nr != std::wstring::npos) t.erase(nr);
            const wchar_t kDia[] = L" - Dia";
            size_t d = t.rfind(kDia);
            if (d != std::wstring::npos) t.erase(d);

            // Strip profile prefix if present (e.g. "Personal: " or "Work: ")
            size_t colon = t.find(L": ");
            if (colon != std::wstring::npos && colon < 30) {
                t = t.substr(colon + 2);
            }
            while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
            if (!t.empty()) return t;
        }
    }

    // Fallback: search visual tree
    for (auto handle : Engine_FindNodes(L"RowsPanel")) {
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto element = object.try_as<mux::UIElement>();
        if (!element) continue;

        try {
            auto peer = muxa_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
            if (!peer) continue;

            auto children = peer.GetChildren();
            for (uint32_t i = 0; i < children.Size(); ++i) {
                auto child = children.GetAt(i);
                if (!child) continue;

                auto pattern = child.GetPattern(muxa_peers::PatternInterface::SelectionItem);
                if (!pattern) continue;
                auto provider = pattern.try_as<winrt::Microsoft::UI::Xaml::Automation::Provider::ISelectionItemProvider>();
                if (!provider || !provider.IsSelected()) continue;

                try {
                    std::wstring name = std::wstring(child.GetName());
                    if (!name.empty()) return name;
                } catch (...) {}
            }
        } catch (...) {}
    }
    return L"";
}

// ---- the DevTools endpoint ------------------------------------------------

bool DevToolsPort(int& port) {
    static ULONGLONG checkedAt = 0;
    static bool found = false;
    static int foundPort = 0;

    ULONGLONG now = GetTickCount64();
    if (found && now - checkedAt < 30000) {
        port = foundPort;
        return true;
    }

    checkedAt = now;
    Cdp::Connection connection = Cdp::Connect(kDevToolsPort);
    if (connection.connected) {
        found = true;
        foundPort = connection.port;
        Log("developer mode: DevTools endpoint found on port %d", foundPort);
    }

    if (found) {
        port = foundPort;
        return true;
    }
    return false;
}

Page ReadPage(const Page& last) {
    Page page = last;
    int port = 0;
    if (!DevToolsPort(port) || port <= 0) {
        return last;
    }

    std::wstring activeTitle;
    std::wstring activeUrl;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        activeTitle = g_page.title;
        activeUrl = g_page.url;
    }
    if (activeUrl.empty()) activeUrl = GetAddressBarUrl();
    if (activeTitle.empty()) activeTitle = GetSelectedTabTitle();
    if (!activeUrl.empty() && activeTitle.empty()) activeTitle = activeUrl;
    if (activeTitle == L"New Tab" || activeTitle == L"New tab") {
        page.url.clear();
        page.origin.clear();
        page.socket.clear();
        page.title = activeTitle;
        page.targetId.clear();
        page.devTools = false;
        return page;
    }

    std::wstring url;
    std::wstring targetId;

    if (Cdp::FocusedPageUrl(port, activeTitle, url, targetId) && !url.empty()) {
        if (url.find(L"newtab") != std::wstring::npos ||
            url.find(L"about:blank") != std::wstring::npos) {
            if (activeTitle.empty() || activeTitle == L"New Tab" || activeTitle == L"New tab" || IsNewTabPage()) {
                page.url.clear();
                page.origin.clear();
                page.socket.clear();
                page.title.clear();
                page.targetId.clear();
                page.devTools = false;
                return page;
            }
            return last;
        }

        page.url = url;
        page.origin = OriginOf(url);
        page.targetId = targetId;
        page.devTools = true;

        std::vector<Cdp::Target> targets;
        if (Cdp::ListTargets(port, targets)) {
            for (const Cdp::Target& target : targets) {
                if (target.id == targetId) {
                    page.socket = target.websocketDebuggerUrl;
                    if (page.title.empty()) page.title = target.title;
                    break;
                }
            }
        }
        return page;
    }

    return last;
}

// ---- the menu items -------------------------------------------------------

enum MenuKind {
    kMenuNone,
    kMenuSite,
    kMenuTab,
};

using ItemVector = winrt::Windows::Foundation::Collections::IVector<muxc::MenuFlyoutItemBase>;

ItemVector MenuItems(muxc::ItemsControl& presenter) {
    try {
        auto source = presenter.ItemsSource();
        if (source) {
            auto vector = source.try_as<ItemVector>();
            if (vector) return vector;
        }
    } catch (...) {}

    try {
        auto items = presenter.Items();
        if (items.Size() > 0) {
            auto vector = items.try_as<ItemVector>();
            if (vector) return vector;
        }
    } catch (...) {}

    return nullptr;
}

std::wstring MenuSignature(ItemVector& items) {
    try {
        std::wstring text;
        for (uint32_t i = 0; i < items.Size() && i < 24; ++i) {
            std::wstring line;
            if (auto item = items.GetAt(i).try_as<muxc::MenuFlyoutItem>()) {
                line = item.Text();
            }
            if (line.empty()) continue;
            if (!text.empty()) text += L"|";
            text += line;
        }
        return text;
    } catch (...) {
        return std::wstring();
    }
}

MenuKind KindOfMenu(ItemVector& items) {
    std::wstring signature = MenuSignature(items);
    if (signature.find(L"Open as split") != std::wstring::npos ||
        signature.find(L"Duplicate") != std::wstring::npos ||
        signature.find(L"Pin") != std::wstring::npos)
        return kMenuTab;
    if (signature.find(L"Copy link") != std::wstring::npos)
        return kMenuSite;
    return kMenuNone;
}

muxc::ToggleMenuFlyoutItem FindOurItem(ItemVector& items, const wchar_t* name) {
    try {
        for (uint32_t i = 0; i < items.Size(); ++i) {
            if (auto item = items.GetAt(i).try_as<muxc::ToggleMenuFlyoutItem>()) {
                if (item.Name() == name) return item;
            }
        }
    } catch (...) {}
    return nullptr;
}

bool AddToggleToMenu(ItemVector& items, const wchar_t* name, const std::wstring& origin) {
    try {
        bool wanted = ActiveTabInDevMode();

        if (auto existing = FindOurItem(items, name)) {
            if (existing.IsChecked() != wanted) existing.IsChecked(wanted);
            return false;
        }

        muxc::ToggleMenuFlyoutItem item;
        item.Name(name);
        item.Text(kDevModeText);
        item.IsChecked(wanted);

        item.Click([item](const winrt::Windows::Foundation::IInspectable&,
                          const winrt::Windows::Foundation::IInspectable&) {
            ToggleCurrentTabDevMode();
            try {
                item.IsChecked(ActiveTabInDevMode());
            } catch (...) {}
        });

        items.Append(item);
        Log("developer mode: added the toggle to the %ls menu", name);
        return true;
    } catch (...) {
        return false;
    }
}

void UpdateMenus() {
    if (IsNewTabPage()) return;
    Engine_ForEachNode([](const NodeRef& node) {
        if (node.type != L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter") return;

        IInspectable* raw = Engine_Object(node.handle);
        if (!raw) return;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto presenter = object.try_as<muxc::ItemsControl>();
        if (!presenter) return;

        auto items = MenuItems(presenter);
        if (!items) return;

        switch (KindOfMenu(items)) {
            case kMenuSite:
                AddToggleToMenu(items, kSiteItemName, PageOrigin());
                break;
            case kMenuTab:
                AddToggleToMenu(items, kTabItemName, PageOrigin());
                break;
            default:
                break;
        }
    });
}

// ---- the mark on the sidebar tab -------------------------------------------

struct MarkShape {
    winrt::weak_ref<muxsh::Path> path;
    muxm::Brush stroke{nullptr};
    double thickness = 0;
    muxm::DoubleCollection dashes{nullptr};
};

winrt::weak_ref<mux::UIElement> g_markedRow;
std::vector<MarkShape> g_markShapes;

struct RowOnScreen {
    mux::UIElement row{nullptr};
    InstanceHandle handle = 0;
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool selected = false;
};

std::vector<RowOnScreen> SidebarRows() {
    std::vector<RowOnScreen> rows;

    std::vector<InstanceHandle> candidates;
    Engine_ForEachNode([&](const NodeRef& node) {
        if (node.type != L"Microsoft.UI.Xaml.Controls.UserControl") return;

        InstanceHandle parent = Engine_Parent(node.handle);
        for (int i = 0; i < 8 && parent; ++i) {
            if (Engine_Name(parent) == L"RowsPanel") {
                candidates.push_back(node.handle);
                return;
            }
            parent = Engine_Parent(parent);
        }
    });

    for (InstanceHandle handle : candidates) {
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto element = object.try_as<mux::UIElement>();
        if (!element) continue;

        try {
            auto peer = muxa_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
            if (!peer) continue;

            auto rect = peer.GetBoundingRectangle();
            if (rect.Width < 40 || rect.Height < 20) continue;

            RowOnScreen row;
            row.row = element;
            row.handle = handle;
            row.x = rect.X;
            row.y = rect.Y;
            row.width = rect.Width;
            row.height = rect.Height;
            rows.push_back(row);
        } catch (...) {}
    }
    return rows;
}

bool RowShowsText(mux::UIElement row, const std::wstring& text) {
    if (text.empty()) return false;

    std::wstring needle = text;
    for (auto& c : needle) c = static_cast<wchar_t>(::towlower(c));

    std::vector<mux::DependencyObject> stack;
    stack.push_back(row.as<mux::DependencyObject>());
    int guard = 0;
    while (!stack.empty() && guard++ < 120) {
        auto current = stack.back();
        stack.pop_back();

        if (auto block = current.try_as<muxc::TextBlock>()) {
            std::wstring line(block.Text());
            for (auto& c : line) c = static_cast<wchar_t>(::towlower(c));
            if (!line.empty() && (line.find(needle) != std::wstring::npos || needle.find(line) != std::wstring::npos)) {
                return true;
            }
        }

        int count = muxm::VisualTreeHelper::GetChildrenCount(current);
        for (int i = count - 1; i >= 0; --i) {
            auto child = muxm::VisualTreeHelper::GetChild(current, i);
            if (child) stack.push_back(child);
        }
    }
    return false;
}

std::wstring RowTitle(mux::UIElement row) {
    std::vector<mux::DependencyObject> stack;
    stack.push_back(row.as<mux::DependencyObject>());
    int guard = 0;
    while (!stack.empty() && guard++ < 120) {
        auto current = stack.back();
        stack.pop_back();

        if (auto block = current.try_as<muxc::TextBlock>()) {
            std::wstring line(block.Text());
            if (!line.empty() && line != L"\uE711" && line != L"✕" && line != L"x" && line != L"New Tab" && line != L"New tab") {
                return line;
            }
        }

        int count = muxm::VisualTreeHelper::GetChildrenCount(current);
        for (int i = count - 1; i >= 0; --i) {
            auto child = muxm::VisualTreeHelper::GetChild(current, i);
            if (child) stack.push_back(child);
        }
    }
    return L"";
}

const wchar_t kDevMarkYellowName[] = L"DiaTweaksDevMarkYellow";
const wchar_t kDevMarkBlackName[] = L"DiaTweaksDevMarkBlack";

muxc::Panel FindRowPanel(mux::UIElement row) {
    if (auto uc = row.try_as<muxc::UserControl>()) {
        if (auto p = uc.Content().try_as<muxc::Panel>()) return p;
        if (auto b = uc.Content().try_as<muxc::Border>()) {
            if (auto bp = b.Child().try_as<muxc::Panel>()) return bp;
        }
    }
    std::vector<mux::DependencyObject> stack;
    stack.push_back(row.as<mux::DependencyObject>());
    int guard = 0;
    while (!stack.empty() && guard++ < 30) {
        auto cur = stack.back();
        stack.pop_back();
        if (auto p = cur.try_as<muxc::Panel>()) return p;
        int count = muxm::VisualTreeHelper::GetChildrenCount(cur);
        for (int i = 0; i < count; ++i) {
            stack.push_back(muxm::VisualTreeHelper::GetChild(cur, i));
        }
    }
    return nullptr;
}

std::vector<muxsh::Path> RingShapes(mux::UIElement row) {
    std::vector<muxsh::Path> paths;
    std::vector<mux::DependencyObject> stack;
    stack.push_back(row.as<mux::DependencyObject>());
    int guard = 0;
    while (!stack.empty() && guard++ < 120) {
        auto current = stack.back();
        stack.pop_back();
        if (auto path = current.try_as<muxsh::Path>()) {
            paths.push_back(path);
        }
        int count = muxm::VisualTreeHelper::GetChildrenCount(current);
        for (int i = count - 1; i >= 0; --i) {
            auto child = muxm::VisualTreeHelper::GetChild(current, i);
            if (child) stack.push_back(child);
        }
    }
    return paths;
}

void DrawMark(mux::UIElement row) {
    try {
        auto panel = FindRowPanel(row);
        if (panel) {
            muxsh::Rectangle yellowRect{nullptr};
            muxsh::Rectangle blackRect{nullptr};

            for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
                if (auto r = panel.Children().GetAt(i).try_as<muxsh::Rectangle>()) {
                    if (r.Name() == kDevMarkYellowName) yellowRect = r;
                    else if (r.Name() == kDevMarkBlackName) blackRect = r;
                }
            }

            if (!yellowRect) {
                yellowRect = muxsh::Rectangle();
                yellowRect.Name(kDevMarkYellowName);
                yellowRect.RadiusX(8);
                yellowRect.RadiusY(8);
                yellowRect.StrokeThickness(2.5);
                muxm::SolidColorBrush yellow;
                yellow.Color(ColorFrom(kMarkYellow));
                yellowRect.Stroke(yellow);
                yellowRect.IsHitTestVisible(false);
                yellowRect.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                yellowRect.VerticalAlignment(mux::VerticalAlignment::Stretch);
                yellowRect.Margin(mux::Thickness{1, 1, 1, 1});
                panel.Children().Append(yellowRect);
            }
            yellowRect.Visibility(mux::Visibility::Visible);

            if (!blackRect) {
                blackRect = muxsh::Rectangle();
                blackRect.Name(kDevMarkBlackName);
                blackRect.RadiusX(8);
                blackRect.RadiusY(8);
                blackRect.StrokeThickness(2.5);
                muxm::SolidColorBrush black;
                black.Color(ColorFrom(kMarkBlack));
                blackRect.Stroke(black);
                muxm::DoubleCollection pattern;
                pattern.Append(4.0f);
                pattern.Append(4.0f);
                blackRect.StrokeDashArray(pattern);
                blackRect.IsHitTestVisible(false);
                blackRect.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                blackRect.VerticalAlignment(mux::VerticalAlignment::Stretch);
                blackRect.Margin(mux::Thickness{1, 1, 1, 1});
                panel.Children().Append(blackRect);
            }
            blackRect.Visibility(mux::Visibility::Visible);
            return;
        }

        std::vector<muxsh::Path> paths = RingShapes(row);
        if (paths.empty()) return;

        muxm::SolidColorBrush yellow;
        yellow.Color(ColorFrom(kMarkYellow));
        muxm::SolidColorBrush black;
        black.Color(ColorFrom(kMarkBlack));

        muxm::DoubleCollection pattern;
        pattern.Append(4.0f);
        pattern.Append(4.0f);

        for (size_t i = 0; i < paths.size(); ++i) {
            auto path = paths[i];
            MarkShape shape;
            shape.path = winrt::make_weak(path);
            try { shape.stroke = path.Stroke(); } catch (...) {}
            shape.thickness = path.StrokeThickness();
            try { shape.dashes = path.StrokeDashArray(); } catch (...) {}
            g_markShapes.push_back(shape);

            path.IsHitTestVisible(false);
            if (i == 0) {
                path.Stroke(yellow);
                path.StrokeThickness(3.0);
            } else {
                path.Stroke(black);
                path.StrokeThickness(3.0);
                path.StrokeDashArray(pattern);
            }
        }
    } catch (...) {}
}

void ClearMarkOnRow(mux::UIElement row) {
    try {
        auto panel = FindRowPanel(row);
        if (panel) {
            for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
                if (auto r = panel.Children().GetAt(i).try_as<muxsh::Rectangle>()) {
                    if (r.Name() == kDevMarkYellowName || r.Name() == kDevMarkBlackName) {
                        r.Visibility(mux::Visibility::Collapsed);
                    }
                }
            }
        }
    } catch (...) {}
}

void ClearMark() {
    if (g_markShapes.empty() && !g_markedRow.get() && g_markedRowHandle == 0) return;
    try {
        std::vector<RowOnScreen> rows = SidebarRows();
        for (auto& row : rows) {
            ClearMarkOnRow(row.row);
        }
        for (MarkShape& shape : g_markShapes) {
            auto path = shape.path.get();
            if (!path) continue;
            try {
                if (shape.stroke) path.Stroke(shape.stroke);
                path.StrokeThickness(shape.thickness);
                muxm::DoubleCollection emptyDashes;
                path.StrokeDashArray(emptyDashes);
            } catch (...) {}
        }
        g_markShapes.clear();
        g_markedRow = nullptr;
        g_markedRowHandle = 0;
    } catch (...) {}
}

void MarkSidebarTab() {
    static ULONGLONG lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastCheck < 250) return;
    lastCheck = now;

    bool activeInDev = ActiveTabInDevMode();
    if (!activeInDev) {
        ClearMark();
        return;
    }

    std::vector<RowOnScreen> rows = SidebarRows();
    if (rows.empty()) {
        ClearMark();
        return;
    }

    double selectedX = 0, selectedY = 0, selectedWidth = 0, selectedHeight = 0;
    bool haveSelected = false;

    for (auto handle : Engine_FindNodes(L"RowsPanel")) {
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto element = object.try_as<mux::UIElement>();
        if (!element) continue;

        try {
            auto peer = muxa_peers::FrameworkElementAutomationPeer::CreatePeerForElement(element);
            if (!peer) continue;

            auto children = peer.GetChildren();
            for (uint32_t i = 0; i < children.Size(); ++i) {
                auto child = children.GetAt(i);
                if (!child) continue;

                auto pattern = child.GetPattern(muxa_peers::PatternInterface::SelectionItem);
                if (!pattern) continue;
                auto provider =
                    pattern.try_as<winrt::Microsoft::UI::Xaml::Automation::Provider::
                                       ISelectionItemProvider>();
                if (!provider || !provider.IsSelected()) continue;

                auto rect = child.GetBoundingRectangle();
                if (rect.Width < 40 || rect.Height < 20) continue;

                selectedX = rect.X;
                selectedY = rect.Y;
                selectedWidth = rect.Width;
                selectedHeight = rect.Height;
                haveSelected = true;
                break;
            }
        } catch (...) {}
        if (haveSelected) break;
    }

    std::wstring activeTitle = GetSelectedTabTitle();
    std::wstring activeLower = activeTitle;
    for (auto& c : activeLower) c = static_cast<wchar_t>(::towlower(c));
    while (activeLower.ends_with(L"…") || activeLower.ends_with(L".")) activeLower.pop_back();

    std::wstring origin = PageOrigin();
    std::wstring host = origin;
    size_t p = host.find(L"://");
    if (p != std::wstring::npos) host = host.substr(p + 3);
    size_t sl = host.find(L'/');
    if (sl != std::wstring::npos) host = host.substr(0, sl);

    InstanceHandle matchedHandle = 0;
    if (haveSelected) {
        double selectedCenterY = selectedY + selectedHeight / 2.0;
        for (const RowOnScreen& row : rows) {
            if (selectedCenterY >= row.y - 2 && selectedCenterY <= row.y + row.height + 2) {
                matchedHandle = row.handle;
                break;
            }
        }
    }

    // Secondary fallback: match by page title or host if bounding box didn't match
    if (!matchedHandle) {
        if (!activeLower.empty()) {
            for (const RowOnScreen& row : rows) {
                if (RowShowsText(row.row, activeLower)) {
                    matchedHandle = row.handle;
                    break;
                }
            }
        }
        if (!matchedHandle && !host.empty()) {
            for (const RowOnScreen& row : rows) {
                if (RowShowsText(row.row, host)) {
                    matchedHandle = row.handle;
                    break;
                }
            }
        }
    }

    for (const RowOnScreen& row : rows) {
        if (matchedHandle && row.handle == matchedHandle) {
            DrawMark(row.row);
            g_markedRow = winrt::make_weak(row.row);
            g_markedRowHandle = row.handle;
        } else {
            ClearMarkOnRow(row.row);
        }
    }
}

// ---- the developer toolbar ------------------------------------------------

enum Tool {
    kDeviceMode,        // Phone / Device mode (Ctrl+Shift+M)
    kCapturePortrait,   // Capture in portrait (\uEB9F)
    kCaptureArea,       // Camera / screenshot freeform (\uE722)
    kConsole,           // Terminal / console (\uE756)
    kNetwork,           // Globe / network (\uE774)
    kInspect,           // Target / scope / inspect (\uE81D)
    kExtensions,        // Puzzle / extensions (\uEA86)
    kToolCount,
};

void CopyLink() {
    std::wstring url;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        url = g_page.url;
    }
    if (url.empty()) return;

    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();

    size_t bytes = (url.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* text = GlobalLock(memory);
        if (text) {
            memcpy(text, url.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    Log("developer mode: copied the link");
}

std::wstring CurrentTargetSocket() {
    std::wstring socket;
    std::wstring id;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        socket = g_page.socket;
        id = g_page.targetId;
    }
    if (!socket.empty()) return socket;

    int port = 0;
    if (id.empty() || !DevToolsPort(port)) return std::wstring();
    return L"ws://127.0.0.1:" + std::to_wstring(port) + L"/devtools/page/" + id;
}

struct PageTarget {
    int port = 0;
    std::wstring socket;
    std::wstring id;
};

PageTarget CurrentPageTarget() {
    PageTarget target;
    DevToolsPort(target.port);
    std::lock_guard<std::mutex> lock(g_lock);
    target.socket = g_page.socket;
    target.id = g_page.targetId;
    return target;
}

struct ChromiumPair {
    HWND page = nullptr;
    HWND devTools = nullptr;
};

ChromiumPair GetChromiumPair(HWND top) {
    ChromiumPair pair;
    if (!top || !IsWindow(top)) return pair;

    std::vector<HWND> list;
    EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[128]{};
        GetClassNameW(h, cls, 128);
        if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0) {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));

    if (list.empty()) return pair;
    if (list.size() == 1) {
        pair.page = list[0];
        return pair;
    }

    HWND h1 = list[0];
    HWND h2 = list[1];
    RECT r1{}, r2{};
    GetWindowRect(h1, &r1);
    GetWindowRect(h2, &r2);

    int w1 = r1.right - r1.left;
    int h1_h = r1.bottom - r1.top;
    int w2 = r2.right - r2.left;
    int h2_h = r2.bottom - r2.top;

    // Both windows must be laid out with real dimensions before we consider DevTools ready
    if (w1 < 50 || h1_h < 50 || w2 < 50 || h2_h < 50) {
        pair.page = (w1 >= w2) ? h1 : h2;
        pair.devTools = nullptr; // DevTools is still being arranged
        return pair;
    }

    if (r2.left > r1.left || (r2.left == r1.left && r2.top > r1.top)) {
        pair.page = h1;
        pair.devTools = h2;
    } else {
        pair.page = h2;
        pair.devTools = h1;
    }
    return pair;
}

void FocusDevTools(HWND hDevTools) {
    if (!hDevTools || !IsWindow(hDevTools)) return;
    RECT rc{};
    GetWindowRect(hDevTools, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 20 || h <= 20) return;

    POINT ptOriginal;
    GetCursorPos(&ptOriginal);

    // Click on the Console tab area (rc.left + 180, rc.top + 14) to focus DevTools
    // without clicking Inspect Element (at 38) or Device Toolbar (at 76)!
    int clickX = rc.left + min(180, w - 20);
    int clickY = rc.top + 14;

    SetCursorPos(clickX, clickY);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(20);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(30);

    SetCursorPos(ptOriginal.x, ptOriginal.y);
}

void SendCtrlShiftKey(BYTE vk) {
    keybd_event(VK_CONTROL, 0, 0, 0);
    keybd_event(VK_SHIFT, 0, 0, 0);
    Sleep(50);
    keybd_event(vk, 0, 0, 0);
    Sleep(50);
    keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
    Sleep(50);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

void SendCtrlBracket() {
    keybd_event(VK_CONTROL, 0, 0, 0);
    Sleep(15);
    keybd_event(VK_OEM_6, 0, 0, 0);
    Sleep(15);
    keybd_event(VK_OEM_6, 0, KEYEVENTF_KEYUP, 0);
    Sleep(15);
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
}

struct DevToolsTask {
    std::string panel;
};

// Background worker thread: runs DevTools actions asynchronously without blocking the UI thread
DWORD WINAPI DevToolsWorker(LPVOID param) {
    std::unique_ptr<DevToolsTask> task(static_cast<DevToolsTask*>(param));
    if (!task) return 0;

    HWND top = GetForegroundWindow();
    wchar_t topCls[128]{};
    GetClassNameW(top, topCls, 128);
    if (wcscmp(topCls, L"WinUIDesktopWin32WindowClass") != 0) {
        top = nullptr;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t cls[128]{};
            GetClassNameW(h, cls, 128);
            if (wcscmp(cls, L"WinUIDesktopWin32WindowClass") == 0 && IsWindowVisible(h)) {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&top));
    }
    if (top) {
        SetForegroundWindow(top);
        Sleep(20);
    }

    ChromiumPair pair = GetChromiumPair(top);
    bool wasAlreadyOpen = (pair.devTools != nullptr);

    if (task->panel == "console") {
        // Ctrl+Shift+J: always opens/focuses Console — NOT a toggle, always idempotent
        SendCtrlShiftKey('J');
    } else if (task->panel == "network") {
        // Strategy: Use Ctrl+Shift+J (idempotent, always lands on Console),
        // then Ctrl+] twice to step Console -> Sources -> Network
        SendCtrlShiftKey('J');
        if (!wasAlreadyOpen) {
            for (int i = 0; i < 60 && !pair.devTools; ++i) {
                Sleep(50);
                pair = GetChromiumPair(top);
            }
            // Allow Chromium DevTools frontend JS to finish loading its shortcut registry
            Sleep(500);
        } else {
            Sleep(80);
        }

        if (pair.devTools) {
            FocusDevTools(pair.devTools);
            Sleep(40);
        }
        // Console -> Sources -> Network (2 steps)
        for (int s = 0; s < 2; ++s) {
            SendCtrlBracket();
            Sleep(40);
        }
    } else if (task->panel == "elements") {
        if (!wasAlreadyOpen) {
            // F12 opens DevTools (to Elements by default)
            keybd_event(VK_F12, 0, 0, 0);
            Sleep(20);
            keybd_event(VK_F12, 0, KEYEVENTF_KEYUP, 0);
            for (int i = 0; i < 60; ++i) {
                pair = GetChromiumPair(top);
                if (pair.devTools) break;
                Sleep(50);
            }
            Sleep(350);
        } else {
            Sleep(40);
        }

        if (pair.devTools) {
            RECT rc{};
            GetWindowRect(pair.devTools, &rc);
            POINT ptOriginal;
            GetCursorPos(&ptOriginal);

            // Click Button 1 (Inspect Element) at rc.left + 38, rc.top + 14
            SetCursorPos(rc.left + 38, rc.top + 14);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            Sleep(15);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            Sleep(20);
            SetCursorPos(ptOriginal.x, ptOriginal.y);
        }

        // Also enable node inspect mode via CDP
        PageTarget pt = CurrentPageTarget();
        if (!pt.id.empty()) {
            Cdp::SendToPage(pt.port, pt.socket, pt.id, "Overlay.enable", "{}");
            Cdp::SendToPage(pt.port, pt.socket, pt.id, "Overlay.setInspectMode",
                "{\"mode\":\"searchForNode\",\"highlightConfig\":{\"showInfo\":true,\"showStyles\":true,\"showRulers\":true,\"showAccessibilityInfo\":true,\"showExtensionLines\":true,\"contentColor\":{\"r\":111,\"g\":168,\"b\":220,\"a\":0.66},\"paddingColor\":{\"r\":147,\"g\":196,\"b\":125,\"a\":0.55},\"borderColor\":{\"r\":255,\"g\":229,\"b\":153,\"a\":0.66},\"marginColor\":{\"r\":246,\"g\":178,\"b\":107,\"a\":0.66}}}");
        }
    } else if (task->panel == "device") {
        if (!wasAlreadyOpen) {
            keybd_event(VK_F12, 0, 0, 0);
            Sleep(20);
            keybd_event(VK_F12, 0, KEYEVENTF_KEYUP, 0);
            for (int i = 0; i < 60; ++i) {
                pair = GetChromiumPair(top);
                if (pair.devTools) break;
                Sleep(50);
            }
            Sleep(350);
        } else {
            Sleep(40);
        }

        if (pair.devTools) {
            RECT rc{};
            GetWindowRect(pair.devTools, &rc);
            POINT ptOriginal;
            GetCursorPos(&ptOriginal);

            // Click Button 2 (Toggle Device Toolbar) at rc.left + 76, rc.top + 14
            SetCursorPos(rc.left + 76, rc.top + 14);
            mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
            Sleep(15);
            mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            Sleep(20);
            SetCursorPos(ptOriginal.x, ptOriginal.y);
        }
    }

    ClearStuckCopiedText(true);
    Log("developer mode: handled DevTools for %s", task->panel.c_str());
    return 0;
}

void OpenDevTools(const char* panel) {
    auto* task = new DevToolsTask();
    task->panel = panel;

    HANDLE th = CreateThread(nullptr, 0, DevToolsWorker, task, 0, nullptr);
    if (th) CloseHandle(th);

    Log("developer mode: scheduled DevTools for %s", panel);
}

std::wstring CaptureFolder() {
    wchar_t pictures[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, 0, pictures)) || pictures[0] == L'\0') {
        wchar_t profile[MAX_PATH]{};
        DWORD size = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (size == 0 || size > MAX_PATH) return std::wstring();
        wcscpy_s(pictures, profile);
        wcscat_s(pictures, L"\\Pictures");
    }
    std::wstring folder(pictures);
    folder += L"\\DiaTweaks";
    CreateDirectoryW(folder.c_str(), nullptr);
    return folder;
}

bool WritePng(const std::wstring& path, const std::vector<unsigned char>& png) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, png.data(), static_cast<DWORD>(png.size()), &written, nullptr);
    CloseHandle(file);
    return true;
}

std::wstring CaptureName(const wchar_t* kind) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[64];
    swprintf_s(buffer, L"%s-%04d%02d%02d-%02d%02d%02d.png", kind, now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond);
    return std::wstring(buffer);
}

HGLOBAL MakeDib(const std::vector<unsigned char>& pixels, UINT width, UINT height) {
    size_t headerSize = sizeof(BITMAPINFOHEADER);
    size_t total = headerSize + pixels.size();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, total);
    if (!memory) return nullptr;

    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        return nullptr;
    }

    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = static_cast<LONG>(width);
    header.biHeight = -static_cast<LONG>(height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(pixels.size());

    memcpy(data, &header, headerSize);
    memcpy(static_cast<unsigned char*>(data) + headerSize, pixels.data(), pixels.size());
    GlobalUnlock(memory);
    return memory;
}

void SavePicture(const wchar_t* kind, const std::vector<unsigned char>& png) {
    std::wstring folder = CaptureFolder();
    if (!folder.empty()) {
        std::wstring path = folder + L"\\" + CaptureName(kind);
        if (WritePng(path, png)) Log("developer mode: saved %ls", path.c_str());
    }

    if (OpenClipboard(nullptr)) {
        EmptyClipboard();

        IStream* stream = nullptr;
        HGLOBAL memoryBytes = GlobalAlloc(GMEM_MOVEABLE, png.size());
        if (memoryBytes) {
            void* data = GlobalLock(memoryBytes);
            if (data) {
                memcpy(data, png.data(), png.size());
                GlobalUnlock(memoryBytes);
                if (SUCCEEDED(CreateStreamOnHGlobal(memoryBytes, TRUE, &stream))) {
                    IWICImagingFactory* factory = nullptr;
                    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_PPV_ARGS(&factory));
                    if (factory) {
                        IWICBitmapDecoder* decoder = nullptr;
                        if (SUCCEEDED(factory->CreateDecoderFromStream(
                                stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                            decoder) {
                            IWICBitmapFrameDecode* frame = nullptr;
                            decoder->GetFrame(0, &frame);
                            if (frame) {
                                IWICFormatConverter* converter = nullptr;
                                factory->CreateFormatConverter(&converter);
                                if (converter) {
                                    converter->Initialize(
                                        frame, GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom);
                                    UINT w = 0;
                                    UINT h = 0;
                                    converter->GetSize(&w, &h);
                                    if (w > 0 && h > 0) {
                                        std::vector<unsigned char> pixels(w * h * 4);
                                        converter->CopyPixels(nullptr, w * 4,
                                                             static_cast<UINT>(pixels.size()),
                                                             pixels.data());
                                        HGLOBAL dib = MakeDib(pixels, w, h);
                                        if (dib) SetClipboardData(CF_DIB, dib);
                                    }
                                    converter->Release();
                                }
                            }
                            if (frame) frame->Release();
                        }
                        if (decoder) decoder->Release();
                        factory->Release();
                    }
                    stream->Release();
                }
            }
        }
        CloseClipboard();
    }
    MessageBeep(MB_OK);
    Log("developer mode: took a picture of the page");
}

// Fast synchronous GDI capture of the live webpage rendered widget
bool CaptureRenderWidgetToPng(HWND chrom, std::vector<unsigned char>& outPng) {
    if (!chrom || !IsWindow(chrom)) return false;

    RECT rc;
    GetClientRect(chrom, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    POINT pt = {0, 0};
    ClientToScreen(chrom, &pt);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);

    // Flag 0 = visible client area only (NOT PW_RENDERFULLCONTENT which grabs entire scrollable page)
    BOOL printed = PrintWindow(chrom, hdcMem, 0);
    if (!printed) {
        BitBlt(hdcMem, 0, 0, w, h, hdcScreen, pt.x, pt.y, SRCCOPY);
    } else {
        // Chromium's hardware compositor sometimes returns TRUE for PrintWindow(0)
        // while leaving pixels blank black. Check sample points and fall back to BitBlt.
        COLORREF c1 = GetPixel(hdcMem, w / 2, h / 2);
        COLORREF c2 = GetPixel(hdcMem, min(10, w - 1), min(10, h - 1));
        if (c1 == 0 && c2 == 0) {
            BitBlt(hdcMem, 0, 0, w, h, hdcScreen, pt.x, pt.y, SRCCOPY);
        }
    }

    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput gsi;
    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);

    bool ok = false;
    {
        Gdiplus::Bitmap bitmap(hBmp, nullptr);
        CLSID pngClsid;
        CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}", &pngClsid);

        IStream* stream = nullptr;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && stream) {
            if (bitmap.Save(stream, &pngClsid, nullptr) == Gdiplus::Ok) {
                STATSTG stat;
                if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
                    HGLOBAL hMem = nullptr;
                    if (SUCCEEDED(GetHGlobalFromStream(stream, &hMem)) && hMem) {
                        void* data = GlobalLock(hMem);
                        if (data) {
                            size_t sz = static_cast<size_t>(stat.cbSize.QuadPart);
                            outPng.resize(sz);
                            memcpy(outPng.data(), data, sz);
                            GlobalUnlock(hMem);
                            ok = true;
                        }
                    }
                }
            }
            stream->Release();
        }
    }
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(hBmp);
    return ok;
}

uint32_t g_portraitC1 = 0xFF5E7E; // Coral Pink (matches Screenshot 2026-11-11 153216.png)
uint32_t g_portraitC2 = 0xFFA06E; // Warm Peach

muxm::LinearGradientBrush BuildSpectrumBrush() {
    muxm::LinearGradientBrush brush;
    brush.StartPoint(winrt::Windows::Foundation::Point{0.0f, 0.5f});
    brush.EndPoint(winrt::Windows::Foundation::Point{1.0f, 0.5f});

    struct Stop { float off; uint32_t c; };
    Stop stops[] = {
        {0.00f, 0xFF5E7E}, // Coral Pink
        {0.20f, 0xFF7A00}, // Orange
        {0.40f, 0xFFD600}, // Gold
        {0.60f, 0x00E676}, // Green
        {0.80f, 0x00B0FF}, // Sky Blue
        {1.00f, 0x7C4DFF}  // Purple
    };
    for (const auto& s : stops) {
        muxm::GradientStop stop;
        stop.Offset(s.off);
        stop.Color(ColorFrom(s.c));
        brush.GradientStops().Append(stop);
    }
    return brush;
}

void GetSpectrumGradient(double t, uint32_t& outC1, uint32_t& outC2) {
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;

    struct Stop { double pos; uint32_t c1; uint32_t c2; };
    Stop table[] = {
        {0.00, 0xFF5E7E, 0xFFA06E},
        {0.20, 0xFF7A00, 0xFFB74D},
        {0.40, 0xF59E0B, 0xFDE047},
        {0.60, 0x10B981, 0x6EE7B7},
        {0.80, 0x0EA5E9, 0x7DD3FC},
        {1.00, 0x8B5CF6, 0xC4B5FD}
    };

    int seg = 0;
    while (seg < 4 && t > table[seg + 1].pos) seg++;
    double span = table[seg + 1].pos - table[seg].pos;
    double factor = (span > 0.0) ? (t - table[seg].pos) / span : 0.0;

    auto lerp = [](uint32_t a, uint32_t b, double f) -> uint32_t {
        int r = static_cast<int>(((a >> 16) & 0xFF) * (1.0 - f) + ((b >> 16) & 0xFF) * f);
        int g = static_cast<int>(((a >> 8) & 0xFF) * (1.0 - f) + ((b >> 8) & 0xFF) * f);
        int bl = static_cast<int>((a & 0xFF) * (1.0 - f) + (b & 0xFF) * f);
        return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bl & 0xFF);
    };

    outC1 = lerp(table[seg].c1, table[seg + 1].c1, factor);
    outC2 = lerp(table[seg].c2, table[seg + 1].c2, factor);
}

// Interactive Area Snipping Tool (Arc-like overlay with golden selection rect)
struct SnipperState {
    HBITMAP hScreenBmp = nullptr;
    int screenX = 0;
    int screenY = 0;
    int screenW = 0;
    int screenH = 0;
    POINT ptStart = {0, 0};
    POINT ptCurrent = {0, 0};
    bool isDragging = false;
};

static SnipperState g_snipper;

LRESULT CALLBACK SnipperWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            g_snipper.ptStart.x = GET_X_LPARAM(lParam);
            g_snipper.ptStart.y = GET_Y_LPARAM(lParam);
            g_snipper.ptCurrent = g_snipper.ptStart;
            g_snipper.isDragging = true;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            if (g_snipper.isDragging) {
                g_snipper.ptCurrent.x = GET_X_LPARAM(lParam);
                g_snipper.ptCurrent.y = GET_Y_LPARAM(lParam);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (g_snipper.isDragging) {
                g_snipper.isDragging = false;
                ReleaseCapture();
                g_snipper.ptCurrent.x = GET_X_LPARAM(lParam);
                g_snipper.ptCurrent.y = GET_Y_LPARAM(lParam);

                int x1 = min(g_snipper.ptStart.x, g_snipper.ptCurrent.x);
                int y1 = min(g_snipper.ptStart.y, g_snipper.ptCurrent.y);
                int x2 = max(g_snipper.ptStart.x, g_snipper.ptCurrent.x);
                int y2 = max(g_snipper.ptStart.y, g_snipper.ptCurrent.y);
                int w = x2 - x1;
                int h = y2 - y1;

                if (w > 4 && h > 4 && g_snipper.hScreenBmp) {
                    HDC hdcScreen = GetDC(nullptr);
                    HDC hdcSrc = CreateCompatibleDC(hdcScreen);
                    HDC hdcDst = CreateCompatibleDC(hdcScreen);
                    HBITMAP hCropBmp = CreateCompatibleBitmap(hdcScreen, w, h);
                    HGDIOBJ oldSrc = SelectObject(hdcSrc, g_snipper.hScreenBmp);
                    HGDIOBJ oldDst = SelectObject(hdcDst, hCropBmp);

                    BitBlt(hdcDst, 0, 0, w, h, hdcSrc, x1, y1, SRCCOPY);

                    SelectObject(hdcSrc, oldSrc);
                    SelectObject(hdcDst, oldDst);
                    DeleteDC(hdcSrc);
                    DeleteDC(hdcDst);
                    ReleaseDC(nullptr, hdcScreen);

                    std::vector<unsigned char> png;
                    ULONG_PTR token = 0;
                    Gdiplus::GdiplusStartupInput gsi;
                    Gdiplus::GdiplusStartup(&token, &gsi, nullptr);
                    {
                        Gdiplus::Bitmap bmp(hCropBmp, nullptr);
                        CLSID pngClsid;
                        CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}", &pngClsid);
                        IStream* stream = nullptr;
                        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) && stream) {
                            if (bmp.Save(stream, &pngClsid, nullptr) == Gdiplus::Ok) {
                                STATSTG stat;
                                if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
                                    HGLOBAL hMem = nullptr;
                                    if (SUCCEEDED(GetHGlobalFromStream(stream, &hMem)) && hMem) {
                                        void* data = GlobalLock(hMem);
                                        if (data) {
                                            png.resize(static_cast<size_t>(stat.cbSize.QuadPart));
                                            memcpy(png.data(), data, png.size());
                                            GlobalUnlock(hMem);
                                        }
                                    }
                                }
                            }
                            stream->Release();
                        }
                    }
                    Gdiplus::GdiplusShutdown(token);
                    DeleteObject(hCropBmp);

                    if (!png.empty()) {
                        SavePicture(L"screenshot", png);
                    }
                }
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hBuf = CreateCompatibleBitmap(hdc, g_snipper.screenW, g_snipper.screenH);
            HGDIOBJ oldBuf = SelectObject(hdcMem, hBuf);

            HDC hdcSrc = CreateCompatibleDC(hdc);
            HGDIOBJ oldSrc = SelectObject(hdcSrc, g_snipper.hScreenBmp);

            BitBlt(hdcMem, 0, 0, g_snipper.screenW, g_snipper.screenH, hdcSrc, 0, 0, SRCCOPY);

            BLENDFUNCTION bf{};
            bf.BlendOp = AC_SRC_OVER;
            bf.SourceConstantAlpha = 90;
            HDC hdcDim = CreateCompatibleDC(hdc);
            HBITMAP hDimBmp = CreateCompatibleBitmap(hdc, 1, 1);
            HGDIOBJ oldDim = SelectObject(hdcDim, hDimBmp);
            RECT r1 = {0, 0, 1, 1};
            FillRect(hdcDim, &r1, (HBRUSH)GetStockObject(BLACK_BRUSH));
            AlphaBlend(hdcMem, 0, 0, g_snipper.screenW, g_snipper.screenH, hdcDim, 0, 0, 1, 1, bf);
            SelectObject(hdcDim, oldDim);
            DeleteObject(hDimBmp);
            DeleteDC(hdcDim);

            if (g_snipper.isDragging) {
                int x1 = min(g_snipper.ptStart.x, g_snipper.ptCurrent.x);
                int y1 = min(g_snipper.ptStart.y, g_snipper.ptCurrent.y);
                int x2 = max(g_snipper.ptStart.x, g_snipper.ptCurrent.x);
                int y2 = max(g_snipper.ptStart.y, g_snipper.ptCurrent.y);
                int w = x2 - x1;
                int h = y2 - y1;

                if (w > 0 && h > 0) {
                    BitBlt(hdcMem, x1, y1, w, h, hdcSrc, x1, y1, SRCCOPY);

                    HPEN hPen = CreatePen(PS_SOLID, 2, RGB(255, 215, 0));
                    HGDIOBJ oldPen = SelectObject(hdcMem, hPen);
                    HGDIOBJ oldBrush = SelectObject(hdcMem, GetStockObject(NULL_BRUSH));
                    Rectangle(hdcMem, x1, y1, x2, y2);
                    SelectObject(hdcMem, oldPen);
                    SelectObject(hdcMem, oldBrush);
                    DeleteObject(hPen);
                }
            }

            BitBlt(hdc, 0, 0, g_snipper.screenW, g_snipper.screenH, hdcMem, 0, 0, SRCCOPY);

            SelectObject(hdcSrc, oldSrc);
            DeleteDC(hdcSrc);
            SelectObject(hdcMem, oldBuf);
            DeleteObject(hBuf);
            DeleteDC(hdcMem);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void StartAreaCapture() {
    g_snipper.screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_snipper.screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_snipper.screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_snipper.screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    g_snipper.hScreenBmp = CreateCompatibleBitmap(hdcScreen, g_snipper.screenW, g_snipper.screenH);
    HGDIOBJ old = SelectObject(hdcMem, g_snipper.hScreenBmp);
    BitBlt(hdcMem, 0, 0, g_snipper.screenW, g_snipper.screenH, hdcScreen, g_snipper.screenX, g_snipper.screenY, SRCCOPY);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = SnipperWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DiaTweaksAreaSnipper";
    wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
    RegisterClassExW(&wc);

    HWND hwndSnipper = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"DiaTweaksAreaSnipper", L"",
        WS_POPUP | WS_VISIBLE,
        g_snipper.screenX, g_snipper.screenY,
        g_snipper.screenW, g_snipper.screenH,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (hwndSnipper) {
        SetForegroundWindow(hwndSnipper);
        SetCursor(LoadCursor(nullptr, IDC_CROSS));
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (g_snipper.hScreenBmp) {
        DeleteObject(g_snipper.hScreenBmp);
        g_snipper.hScreenBmp = nullptr;
    }
}

std::wstring DiaLogoPath() {
    // 1. Check diatweaks assets folder for user watermark.png (rendered from user SVG)
    HMODULE hDll = GetModuleHandleW(L"diatweaks-xaml.dll");
    if (hDll) {
        wchar_t dllPath[MAX_PATH]{};
        if (GetModuleFileNameW(hDll, dllPath, MAX_PATH) > 0) {
            wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                std::wstring p1 = std::wstring(dllPath) + L"\\assets\\watermark.png";
                if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) return p1;
                wchar_t* parentSlash = wcsrchr(dllPath, L'\\');
                if (parentSlash) {
                    *parentSlash = L'\0';
                    std::wstring p2 = std::wstring(dllPath) + L"\\assets\\watermark.png";
                    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) return p2;
                }
            }
        }
    }

    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        std::wstring p = std::wstring(profile) + L"\\DiaTweaks-Windows\\assets\\watermark.png";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }

    wchar_t modulePath[MAX_PATH]{};
    if (GetModuleFileNameW(GetModuleHandleW(nullptr), modulePath, MAX_PATH) > 0) {
        wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            std::wstring intro = std::wstring(modulePath) + L"\\BoostBrowser_Unboxing.bundle\\xcasset-intro-logo-idiom-universal-lum-any-scale-3x-src-intro-logo-3x-783bf4834d.png";
            DWORD attr = GetFileAttributesW(intro.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return intro;
            }
            std::wstring path = std::wstring(modulePath) + L"\\Assets\\AppList.targetsize-48_altform-lightunplated.png";
            attr = GetFileAttributesW(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                return path;
            }
        }
    }
    return L"";
}

void AddGdiplusRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (r <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    float dia = r * 2.0f;
    path.AddArc(x, y, dia, dia, 180.0f, 90.0f);
    path.AddArc(x + w - dia, y, dia, dia, 270.0f, 90.0f);
    path.AddArc(x + w - dia, y + h - dia, dia, dia, 0.0f, 90.0f);
    path.AddArc(x, y + h - dia, dia, dia, 90.0f, 90.0f);
    path.CloseFigure();
}

// Composites full-screen desktop page into a floating card with custom gradient background
bool CreatePortraitCard(const std::vector<unsigned char>& inputPng,
                        uint32_t c1, uint32_t c2,
                        std::vector<unsigned char>& outputPng) {
    if (inputPng.empty()) return false;

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) return false;

    bool ok = false;
    do {
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, inputPng.size());
        if (!hMem) break;
        void* pMem = GlobalLock(hMem);
        if (!pMem) {
            GlobalFree(hMem);
            break;
        }
        memcpy(pMem, inputPng.data(), inputPng.size());
        GlobalUnlock(hMem);

        IStream* pInStream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hMem, TRUE, &pInStream)) || !pInStream) {
            break;
        }

        std::unique_ptr<Gdiplus::Bitmap> pBmp(Gdiplus::Bitmap::FromStream(pInStream));
        pInStream->Release();
        if (!pBmp || pBmp->GetLastStatus() != Gdiplus::Ok) break;

        UINT pw = pBmp->GetWidth();
        UINT ph = pBmp->GetHeight();
        if (pw == 0 || ph == 0) break;

        // Framing padding around page view
        float padX = static_cast<float>(pw) * 0.12f;
        float padY = static_cast<float>(ph) * 0.12f;
        float totalW = pw + padX * 2.0f;
        float totalH = ph + padY * 2.0f;

        Gdiplus::Bitmap card(static_cast<INT>(totalW), static_cast<INT>(totalH), PixelFormat32bppARGB);
        Gdiplus::Graphics g(&card);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

        // Custom gradient background
        Gdiplus::Color color1(255, (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF);
        Gdiplus::Color color2(255, (c2 >> 16) & 0xFF, (c2 >> 8) & 0xFF, c2 & 0xFF);
        Gdiplus::LinearGradientBrush bgBrush(
            Gdiplus::PointF(0.0f, 0.0f),
            Gdiplus::PointF(totalW, totalH),
            color1, color2);
        g.FillRectangle(&bgBrush, 0.0f, 0.0f, totalW, totalH);

        // Soft drop shadow passes behind the floating card
        for (int s = 0; s < 4; ++s) {
            float sOffset = 10.0f + s * 6.0f;
            float sExpand = 4.0f + s * 4.0f;
            Gdiplus::GraphicsPath shadowPath;
            AddGdiplusRoundRect(shadowPath,
                                padX - sExpand,
                                padY + sOffset,
                                pw + sExpand * 2.0f,
                                ph + sExpand * 2.0f,
                                28.0f);
            Gdiplus::SolidBrush sBrush(Gdiplus::Color(25 - s * 5, 0, 0, 0));
            g.FillPath(&sBrush, &shadowPath);
        }

        // Webpage body with rounded corners
        Gdiplus::GraphicsPath pagePath;
        AddGdiplusRoundRect(pagePath, padX, padY, static_cast<float>(pw), static_cast<float>(ph), 20.0f);

        // Clip and draw image
        g.SetClip(&pagePath, Gdiplus::CombineModeReplace);
        g.DrawImage(pBmp.get(), padX, padY, static_cast<float>(pw), static_cast<float>(ph));
        g.ResetClip();

        // Subtle clean border around page card
        Gdiplus::Pen cardBorder(Gdiplus::Color(60, 0, 0, 0), 1.5f);
        g.DrawPath(&cardBorder, &pagePath);

        // Watermark in bottom left: Dia logo if asset present, otherwise omitted
        std::wstring logoPath = DiaLogoPath();
        if (!logoPath.empty()) {
            std::unique_ptr<Gdiplus::Bitmap> logoBmp(Gdiplus::Bitmap::FromFile(logoPath.c_str()));
            if (logoBmp && logoBmp->GetLastStatus() == Gdiplus::Ok) {
                float srcX = 0, srcY = 0;
                float srcW = (float)logoBmp->GetWidth();
                float srcH = (float)logoBmp->GetHeight();
                // If intro logo, use its cropped content bounding box (147, 0, 810, 925)
                if (logoPath.find(L"xcasset-intro-logo") != std::wstring::npos) {
                    srcX = 147.0f;
                    srcY = 0.0f;
                    srcW = 810.0f;
                    srcH = 925.0f;
                }
                float logoH = padY * 0.42f;
                float logoW = logoH * (srcW / srcH);
                float logoX = padX * 0.35f;
                float logoY = totalH - padY * 0.70f;

                Gdiplus::ImageAttributes imgAttr;
                Gdiplus::ColorMatrix cm = {
                    1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.90f, 0.0f,
                    0.0f, 0.0f, 0.0f, 0.0f, 1.0f
                };
                imgAttr.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                g.DrawImage(logoBmp.get(),
                            Gdiplus::RectF(logoX, logoY, logoW, logoH),
                            srcX, srcY, srcW, srcH,
                            Gdiplus::UnitPixel, &imgAttr);
            }
        }

        CLSID pngClsid;
        CLSIDFromString(L"{557cf406-1a04-11d3-9a73-0000f81ef32e}", &pngClsid);

        IStream* pOutStream = nullptr;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pOutStream)) && pOutStream) {
            if (card.Save(pOutStream, &pngClsid, nullptr) == Gdiplus::Ok) {
                STATSTG stat;
                if (SUCCEEDED(pOutStream->Stat(&stat, STATFLAG_NONAME)) && stat.cbSize.QuadPart > 0) {
                    HGLOBAL hOutMem = nullptr;
                    if (SUCCEEDED(GetHGlobalFromStream(pOutStream, &hOutMem)) && hOutMem) {
                        void* pOut = GlobalLock(hOutMem);
                        if (pOut) {
                            size_t outLen = static_cast<size_t>(stat.cbSize.QuadPart);
                            outputPng.resize(outLen);
                            memcpy(outputPng.data(), pOut, outLen);
                            GlobalUnlock(hOutMem);
                            ok = true;
                        }
                    }
                }
            }
            pOutStream->Release();
        }
    } while (false);

    Gdiplus::GdiplusShutdown(token);
    return ok;
}

DWORD WINAPI PortraitJob(LPVOID) {
    // Wait for the modal/flyout to finish closing so it does not get captured on screen
    Sleep(260);

    std::vector<unsigned char> png;
    // Prefer CDP capture if available: it captures pure web content with zero window overlays
    PageTarget page = CurrentPageTarget();
    if (!page.socket.empty()) {
        Cdp::CaptureViewportOnPage(page.port, page.socket, page.id, png);
    }

    if (png.empty()) {
        HWND top = GetDiaMainWindow();
        HWND chrom = FindChromiumChild(top);
        if (chrom) {
            CaptureRenderWidgetToPng(chrom, png);
        }
    }

    if (png.empty()) {
        Log("developer mode: the page could not be captured");
        return 0;
    }

    std::vector<unsigned char> cardPng;
    if (CreatePortraitCard(png, g_portraitC1, g_portraitC2, cardPng) && !cardPng.empty()) {
        SavePicture(L"portrait", cardPng);
    } else {
        SavePicture(L"portrait", png);
    }
    return 0;
}

DWORD WINAPI FreeformJob(LPVOID) {
    StartAreaCapture();
    return 0;
}

void StartJob(DWORD(WINAPI* job)(LPVOID)) {
    HANDLE thread = CreateThread(nullptr, 0, job, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
}

void ShowExtensions() {
    for (auto handle : Engine_FindNodes(L"OverflowButton")) {
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto button = object.try_as<muxc::Button>();
        if (!button) continue;

        try {
            auto flyout = button.Flyout();
            if (!flyout) continue;

            auto anchor = g_toolbar.get();
            if (!anchor) continue;

            flyout.ShowAt(anchor);
            Log("developer mode: opened the extensions");
            return;
        } catch (...) {}
    }

    int port = 0;
    if (DevToolsPort(port)) {
        std::wstring url = L"chrome://extensions";
        if (Cdp::CreateTab(port, url)) {
            Log("developer mode: opened the extensions");
            return;
        }
    }
    Log("developer mode: the extensions dropdown could not be found");
}

const wchar_t* ToolGlyph(Tool tool) {
    switch (tool) {
        case kDeviceMode:      return L"\uE8EA"; // CellPhone in Segoe Fluent Icons
        case kCapturePortrait: return L"\uEB9F"; // Framed photo / picture
        case kCaptureArea:     return L"\uE722"; // Camera
        case kConsole:         return L"\uE756"; // Terminal >_
        case kNetwork:         return L"\uE774"; // Globe
        case kInspect:         return L"\uE81D"; // Target / Crosshairs
        case kExtensions:      return L"\uEA86"; // Real Puzzle Piece
        default:               return L"\uE713";
    }
}

const wchar_t* ToolTipText(Tool tool) {
    switch (tool) {
        case kDeviceMode:      return L"Toggle device toolbar";
        case kCapturePortrait: return L"Capture in Portrait Mode";
        case kCaptureArea:     return L"Capture a portion of this page (Shift+Ctrl+2)";
        case kConsole:         return L"Console (Ctrl+Shift+J)";
        case kNetwork:         return L"Network";
        case kInspect:         return L"Inspect Element";
        case kExtensions:      return L"Extensions";
        default:               return L"";
    }
}

void RunTool(Tool tool) {
    ClearStuckCopiedText(true);

    switch (tool) {
        case kDeviceMode:
            OpenDevTools("device");
            break;
        case kCapturePortrait:
            // Handled via WinUI flyout attached to button
            break;
        case kCaptureArea:
            StartJob(FreeformJob);
            break;
        case kConsole:
            OpenDevTools("console");
            break;
        case kNetwork:
            OpenDevTools("network");
            break;
        case kInspect:
            OpenDevTools("elements");
            break;
        case kExtensions:
            ShowExtensions();
            break;
        default:
            break;
    }
}

// Builds the floating Portrait Mode capture flyout matching Arc's UI
muxc::Flyout BuildPortraitFlyout() {
    muxc::Flyout flyout;
    try {
        flyout.Placement(muxc::Primitives::FlyoutPlacementMode::Bottom);
    } catch (...) {}

    muxc::StackPanel panel;
    panel.Width(330);
    panel.Spacing(10);
    panel.Padding(mux::Thickness{8, 8, 8, 8});

    // Header pill button
    muxc::Button headerBtn;
    headerBtn.Content(winrt::box_value(winrt::hstring(L"Capture in Portrait Mode")));
    headerBtn.HorizontalAlignment(mux::HorizontalAlignment::Center);
    headerBtn.CornerRadius(mux::CornerRadius{14, 14, 14, 14});
    headerBtn.Padding(mux::Thickness{18, 5, 18, 5});
    headerBtn.FontWeight(winrt::Windows::UI::Text::FontWeight{600});

    // Preview Card Grid
    muxc::Grid cardGrid;
    cardGrid.Width(314);
    cardGrid.Height(170);
    cardGrid.CornerRadius(mux::CornerRadius{12, 12, 12, 12});
    cardGrid.Background(MakeGradientBrush(g_portraitC1, g_portraitC2));

    // Floating webpage box inside preview card
    muxc::Border pageBox;
    pageBox.Width(224);
    pageBox.Height(122);
    pageBox.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    pageBox.HorizontalAlignment(mux::HorizontalAlignment::Center);
    pageBox.VerticalAlignment(mux::VerticalAlignment::Center);
    muxm::SolidColorBrush borderBrush;
    borderBrush.Color(winrt::Windows::UI::ColorHelper::FromArgb(40, 0, 0, 0));
    pageBox.BorderBrush(borderBrush);
    pageBox.BorderThickness(mux::Thickness{1, 1, 1, 1});

    // Live Webpage Preview Image: fills the box entirely with zero white bars
    muxc::Image previewImage;
    previewImage.Stretch(muxm::Stretch::UniformToFill);
    previewImage.HorizontalAlignment(mux::HorizontalAlignment::Center);
    previewImage.VerticalAlignment(mux::VerticalAlignment::Center);
    pageBox.Child(previewImage);
    cardGrid.Children().Append(pageBox);

    // Bottom-left watermark: Dia logo if asset present, otherwise omitted
    std::wstring logoPath = DiaLogoPath();
    if (!logoPath.empty()) {
        muxc::Image logoImg;
        logoImg.Width(16);
        logoImg.Height(16);
        logoImg.Opacity(0.85);
        logoImg.HorizontalAlignment(mux::HorizontalAlignment::Left);
        logoImg.VerticalAlignment(mux::VerticalAlignment::Bottom);
        logoImg.Margin(mux::Thickness{12, 0, 0, 10});
        try {
            winrt::Windows::Foundation::Uri uri(std::wstring(L"file:///") + logoPath);
            muxm_imaging::BitmapImage bmp(uri);
            logoImg.Source(bmp);
            cardGrid.Children().Append(logoImg);
        } catch (...) {}
    }

    panel.Children().Append(headerBtn);
    panel.Children().Append(cardGrid);

    // Bottom Controls Row: [Color Dot] [Gradient Slider] [Preset/Palette Button]
    muxc::StackPanel bottomRow;
    bottomRow.Orientation(muxc::Orientation::Horizontal);
    bottomRow.Spacing(10);
    bottomRow.HorizontalAlignment(mux::HorizontalAlignment::Center);
    bottomRow.VerticalAlignment(mux::VerticalAlignment::Center);
    bottomRow.Margin(mux::Thickness{0, 4, 0, 0});

    // 1. Color Dot
    muxsh::Ellipse colorDot;
    colorDot.Width(20);
    colorDot.Height(20);
    colorDot.VerticalAlignment(mux::VerticalAlignment::Center);
    muxm::SolidColorBrush dotBrush;
    dotBrush.Color(ColorFrom(g_portraitC1));
    colorDot.Fill(dotBrush);
    bottomRow.Children().Append(colorDot);

    // 2. Gradient Slider
    muxc::Slider slider;
    slider.Width(210);
    slider.Minimum(0);
    slider.Maximum(100);
    slider.Value(0);
    slider.VerticalAlignment(mux::VerticalAlignment::Center);
    slider.Background(BuildSpectrumBrush());

    // 3. Preset / Palette Button
    muxc::Button presetBtn;
    presetBtn.Width(24);
    presetBtn.Height(24);
    presetBtn.Padding(mux::Thickness{0, 0, 0, 0});
    presetBtn.CornerRadius(mux::CornerRadius{12, 12, 12, 12});
    presetBtn.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::FontIcon paletteIcon;
    paletteIcon.Glyph(L"\uE790");
    paletteIcon.FontSize(11);
    try {
        muxm::FontFamily fontFam(L"Segoe Fluent Icons, Segoe MDL2 Assets");
        paletteIcon.FontFamily(fontFam);
    } catch (...) {}
    presetBtn.Content(paletteIcon);

    slider.ValueChanged([cardGrid, colorDot](const winrt::Windows::Foundation::IInspectable&,
                                            const muxc_primitives::RangeBaseValueChangedEventArgs& e) {
        uint32_t c1 = 0, c2 = 0;
        GetSpectrumGradient(e.NewValue() / 100.0, c1, c2);
        g_portraitC1 = c1;
        g_portraitC2 = c2;
        cardGrid.Background(MakeGradientBrush(c1, c2));
        muxm::SolidColorBrush dotB;
        dotB.Color(ColorFrom(c1));
        colorDot.Fill(dotB);
    });

    presetBtn.Click([slider](const winrt::Windows::Foundation::IInspectable&,
                             const winrt::Windows::Foundation::IInspectable&) {
        static int presetIdx = 0;
        presetIdx = (presetIdx + 1) % 5;
        double vals[] = {0.0, 25.0, 50.0, 75.0, 100.0};
        slider.Value(vals[presetIdx]);
    });

    bottomRow.Children().Append(slider);
    bottomRow.Children().Append(presetBtn);
    panel.Children().Append(bottomRow);

    struct FlyoutPreviewTask {
        winrt::weak_ref<muxc::Image> img;
        winrt::weak_ref<muxc::Border> box;
    };

    // Refresh live webpage thumbnail when flyout is opened
    flyout.Opening([previewImage, pageBox](const winrt::Windows::Foundation::IInspectable&,
                                           const winrt::Windows::Foundation::IInspectable&) {
        // Run capture on a background thread to avoid blocking the UI.
        // Sets previewImage source and sizes pageBox to match the exact webpage aspect ratio.
        auto task = new FlyoutPreviewTask{winrt::make_weak(previewImage), winrt::make_weak(pageBox)};
        HANDLE th = CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
            std::unique_ptr<FlyoutPreviewTask> t(static_cast<FlyoutPreviewTask*>(param));

            std::vector<unsigned char> png;
            // Prefer CDP capture if available (identical to PortraitJob)
            PageTarget page = CurrentPageTarget();
            if (!page.socket.empty()) {
                Cdp::CaptureViewportOnPage(page.port, page.socket, page.id, png);
            }
            if (png.empty()) {
                HWND top = GetDiaMainWindow();
                HWND chrom = FindChromiumChild(top);
                if (chrom) {
                    CaptureRenderWidgetToPng(chrom, png);
                }
            }
            if (png.empty()) return 0;

            std::wstring previewPath = CaptureFolder() + L"\\portrait_preview.png";
            if (!WritePng(previewPath, png)) return 0;

            // Extract width & height from PNG header (IHDR chunk at bytes 16..23)
            uint32_t imgW = 0, imgH = 0;
            if (png.size() >= 24) {
                imgW = (static_cast<uint32_t>(png[16]) << 24) |
                       (static_cast<uint32_t>(png[17]) << 16) |
                       (static_cast<uint32_t>(png[18]) << 8) |
                       static_cast<uint32_t>(png[19]);
                imgH = (static_cast<uint32_t>(png[20]) << 24) |
                       (static_cast<uint32_t>(png[21]) << 16) |
                       (static_cast<uint32_t>(png[22]) << 8) |
                       static_cast<uint32_t>(png[23]);
            }

            double maxW = 236.0;
            double maxH = 126.0;
            double boxW = maxW;
            double boxH = maxH;
            if (imgW > 0 && imgH > 0) {
                double ar = static_cast<double>(imgW) / static_cast<double>(imgH);
                if (ar >= (maxW / maxH)) {
                    boxW = maxW;
                    boxH = maxW / ar;
                    if (boxH < 70.0) boxH = 70.0;
                } else {
                    boxH = maxH;
                    boxW = maxH * ar;
                    if (boxW < 100.0) boxW = 100.0;
                }
            }

            // Dispatch UI update back to the dispatcher thread
            auto img = t->img.get();
            if (!img) return 0;
            auto weakImg = t->img;
            auto weakBox = t->box;
            try {
                img.DispatcherQueue().TryEnqueue([weakImg, weakBox, previewPath, boxW, boxH]() {
                    auto img2 = weakImg.get();
                    auto box2 = weakBox.get();
                    if (box2) {
                        box2.Width(boxW);
                        box2.Height(boxH);
                    }
                    if (img2) {
                        try {
                            winrt::Windows::Foundation::Uri uri(std::wstring(L"file:///") + previewPath);
                            muxm_imaging::BitmapImage bmp(uri);
                            img2.Source(bmp);
                        } catch (...) {}
                    }
                });
            } catch (...) {}
            return 0;
        }, task, 0, nullptr);
        if (th) CloseHandle(th);
    });

    auto triggerCapture = [flyout](const winrt::Windows::Foundation::IInspectable&,
                                  const winrt::Windows::Foundation::IInspectable&) {
        flyout.Hide();
        StartJob(PortraitJob);
    };
    headerBtn.Click(triggerCapture);

    flyout.Content(panel);
    return flyout;
}

std::wstring CustomToolSvgPath(Tool tool) {
    const wchar_t* name = nullptr;
    switch (tool) {
        case kDeviceMode:      name = L"phone.svg"; break;
        case kCapturePortrait: name = L"portrait.svg"; break;
        case kCaptureArea:     name = L"camera.svg"; break;
        case kConsole:         name = L"console.svg"; break;
        case kNetwork:         name = L"network.svg"; break;
        case kInspect:         name = L"inspect.svg"; break;
        case kExtensions:      name = L"extensions.svg"; break;
        default:               return L"";
    }

    HMODULE hDll = GetModuleHandleW(L"diatweaks-xaml.dll");
    if (hDll) {
        wchar_t dllPath[MAX_PATH]{};
        if (GetModuleFileNameW(hDll, dllPath, MAX_PATH) > 0) {
            wchar_t* lastSlash = wcsrchr(dllPath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                std::wstring p1 = std::wstring(dllPath) + L"\\assets\\icons\\" + name;
                if (GetFileAttributesW(p1.c_str()) != INVALID_FILE_ATTRIBUTES) return p1;
                wchar_t* parentSlash = wcsrchr(dllPath, L'\\');
                if (parentSlash) {
                    *parentSlash = L'\0';
                    std::wstring p2 = std::wstring(dllPath) + L"\\assets\\icons\\" + name;
                    if (GetFileAttributesW(p2.c_str()) != INVALID_FILE_ATTRIBUTES) return p2;
                }
            }
        }
    }

    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        std::wstring p = std::wstring(profile) + L"\\DiaTweaks-Windows\\assets\\icons\\" + name;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"";
}

bool BrushLooksLight(const muxm::Brush& brush) {
    std::vector<winrt::Windows::UI::Color> colors;
    if (auto sb = brush.try_as<muxm::SolidColorBrush>()) {
        colors.push_back(sb.Color());
    } else if (auto gb = brush.try_as<muxm::GradientBrush>()) {
        for (auto const& stop : gb.GradientStops()) colors.push_back(stop.Color());
    }
    double total = 0;
    int count = 0;
    for (auto c : colors) {
        if (c.A < 0x40) continue;
        total += 0.299 * c.R + 0.587 * c.G + 0.114 * c.B;
        ++count;
    }
    return count > 0 && total / count > 160.0;
}

// Reads the toolbar's real colours from the screen. The window material and
// Mica sit behind the XAML tree, so neither Dia's brushes nor the app theme know
// what the toolbar actually looks like once they are translucent.
static bool SampleToolbarLuminance(HWND hwnd, double& luminance) {
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    if (GetForegroundWindow() != hwnd) return false;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin)) return false;
    int width = rc.right - rc.left;
    if (width < 300) return false;

    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    int bandTop = MulDiv(6, dpi, 96);
    int bandHeight = MulDiv(38, dpi, 96);
    int bandLeft = width * 30 / 100;
    int bandWidth = width * 60 / 100;
    if (bandWidth < 100 || bandHeight < 4) return false;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bandWidth;
    info.bmiHeader.biHeight = -bandHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(mem, bitmap);
    BOOL copied = BitBlt(mem, 0, 0, bandWidth, bandHeight, screen,
                         origin.x + bandLeft, origin.y + bandTop, SRCCOPY | CAPTUREBLT);
    double total = 0;
    int count = 0;
    if (copied) {
        auto* pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < bandHeight; y += 4) {
            for (int x = 0; x < bandWidth; x += 8) {
                uint32_t p = pixels[y * bandWidth + x];
                int b = static_cast<int>(p & 0xFF);
                int g = static_cast<int>((p >> 8) & 0xFF);
                int r = static_cast<int>((p >> 16) & 0xFF);
                total += 0.299 * r + 0.587 * g + 0.114 * b;
                ++count;
            }
        }
    }

    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    if (!copied || count == 0) return false;
    luminance = total / count;
    return true;
}

bool IsToolbarLight() {
    static thread_local ULONGLONG s_checkedAt = 0;
    static thread_local bool s_light = false;
    ULONGLONG now = GetTickCount64();
    if (s_checkedAt && now - s_checkedAt < 300) return s_light;

    // What is really painted there (including Mica and our tint) beats every
    // guess made from the XAML brushes or the Windows theme.
    double screenLuminance = 0;
    if (SampleToolbarLuminance(GetDiaMainWindow(), screenLuminance)) {
        s_light = screenLuminance > 150.0;
        s_checkedAt = now;
        return s_light;
    }

    auto onScreen = [](const mux::FrameworkElement& fe) {
        try {
            if (fe.ActualWidth() < 1) return false;
            for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d)) {
                if (auto u = d.try_as<mux::UIElement>(); u && u.Visibility() != mux::Visibility::Visible)
                    return false;
            }
            return true;
        } catch (...) {
            return false;
        }
    };

    int lightVotes = 0, darkVotes = 0;
    auto vote = [&](const winrt::Windows::UI::Color& c) {
        if (c.A < 0x40) return;
        double luminance = 0.299 * c.R + 0.587 * c.G + 0.114 * c.B;
        if (luminance > 160.0) ++lightVotes;
        else if (luminance < 96.0) ++darkVotes;
    };

    auto localColor = [](const mux::DependencyObject& d, const mux::DependencyProperty& dp,
                         winrt::Windows::UI::Color& out) -> bool {
        try {
            auto value = d.ReadLocalValue(dp);
            if (auto sb = value.try_as<muxm::SolidColorBrush>()) {
                out = sb.Color();
                return true;
            }
        } catch (...) {}
        return false;
    };

    // Dia colours the glyph inside each toolbar button, not the button itself:
    // only locally set colours are meaningful (inherited ones carry the app theme).
    auto readIconColors = [&](const mux::FrameworkElement& root) {
        std::vector<mux::DependencyObject> stack;
        stack.push_back(root.as<mux::DependencyObject>());
        int guard = 0;
        while (!stack.empty() && guard++ < 60) {
            auto cur = stack.back();
            stack.pop_back();
            winrt::Windows::UI::Color c{};
            if (auto fi = cur.try_as<muxc::FontIcon>()) {
                if (localColor(fi, muxc::IconElement::ForegroundProperty(), c)) vote(c);
            } else if (auto tb = cur.try_as<muxc::TextBlock>()) {
                if (localColor(tb, muxc::TextBlock::ForegroundProperty(), c)) vote(c);
            } else if (auto shape = cur.try_as<muxsh::Shape>()) {
                if (localColor(shape, muxsh::Shape::FillProperty(), c)) vote(c);
                if (localColor(shape, muxsh::Shape::StrokeProperty(), c)) vote(c);
            }
            int count = muxm::VisualTreeHelper::GetChildrenCount(cur);
            for (int i = 0; i < count && stack.size() < 200; ++i)
                stack.push_back(muxm::VisualTreeHelper::GetChild(cur, i));
        }
    };

    for (const wchar_t* name : {L"BackButton", L"ForwardButton", L"ReloadButton", L"SidebarToggleButton"}) {
        for (auto h : Engine_FindNodes(name)) {
            if (g_markedRowHandle && !SameWindow(h, g_markedRowHandle)) continue;
            IInspectable* raw = Engine_Object(h);
            if (!raw) continue;
            Inspectable obj{nullptr};
            winrt::copy_from_abi(obj, raw);
            raw->Release();
            try {
                auto fe = obj.try_as<mux::FrameworkElement>();
                if (!fe || !onScreen(fe)) continue;
                int before = lightVotes + darkVotes;
                readIconColors(fe);
                if (lightVotes + darkVotes == before) {
                    winrt::Windows::UI::Color c{};
                    if (auto control = obj.try_as<muxc::Control>()) {
                        if (localColor(control, muxc::Control::ForegroundProperty(), c)) vote(c);
                    }
                }
            } catch (...) {}
        }
    }

    if (lightVotes != darkVotes) {
        s_light = lightVotes > darkVotes;
        s_checkedAt = now;
        return s_light;
    }

    // The toolbar's own paint: the widest opaque element in the toolbar band.
    // On sites like GitHub the page theme makes this row dark even though the
    // app theme (and therefore the registry value) says light.
    {
        double bestArea = 0, bestLum = -1;
        Engine_ForEachNode([&](const NodeRef& n) {
            if (g_markedRowHandle && !SameWindow(n.handle, g_markedRowHandle)) return;
            IInspectable* raw = Engine_Object(n.handle);
            if (!raw) return;
            Inspectable obj{nullptr};
            winrt::copy_from_abi(obj, raw);
            raw->Release();
            auto fe = obj.try_as<mux::FrameworkElement>();
            if (!fe || fe.Visibility() != mux::Visibility::Visible) return;
            double w = fe.ActualWidth(), ht = fe.ActualHeight();
            if (w < 200 || ht < 8 || ht > 80) return;
            winrt::Windows::Foundation::Point at{};
            try { at = fe.TransformToVisual(nullptr).TransformPoint({0, 0}); } catch (...) { return; }
            if (at.Y < -12 || at.Y > 60) return;
            muxm::Brush brush{nullptr};
            try {
                if (auto b = obj.try_as<muxc::Border>()) brush = b.Background();
                else if (auto g = obj.try_as<muxc::Grid>()) brush = g.Background();
                else if (auto p = obj.try_as<muxc::Panel>()) brush = p.Background();
                else if (auto cc = obj.try_as<muxc::ContentControl>()) brush = cc.Background();
                else if (auto cp = obj.try_as<muxc::ContentPresenter>()) brush = cp.Background();
                else if (auto sh = obj.try_as<muxsh::Shape>()) brush = sh.Fill();
            } catch (...) {}
            if (!brush) return;
            double lum = -1, total = 0;
            int count = 0;
            if (auto sb = brush.try_as<muxm::SolidColorBrush>()) {
                auto c = sb.Color();
                if (c.A >= 0x80) lum = 0.299 * c.R + 0.587 * c.G + 0.114 * c.B;
            } else if (auto gb = brush.try_as<muxm::GradientBrush>()) {
                for (auto const& st : gb.GradientStops()) {
                    auto c = st.Color();
                    if (c.A < 0x80) continue;
                    total += 0.299 * c.R + 0.587 * c.G + 0.114 * c.B;
                    ++count;
                }
                if (count) lum = total / count;
            }
            if (lum < 0) return;
            double area = w * ht;
            if (area > bestArea) {
                bestArea = area;
                bestLum = lum;
            }
        });
        if (bestLum >= 0) {
            s_light = bestLum > 160.0;
            s_checkedAt = now;
            return s_light;
        }
    }

    // No icon colour available: fall back to the toolbar's own background.
    for (const wchar_t* name : {L"NavigationBarContent", L"AssistantBarHost", L"AssistantBar", L"ContentGroup"}) {
        for (auto h : Engine_FindNodes(name)) {
            if (g_markedRowHandle && !SameWindow(h, g_markedRowHandle)) continue;
            IInspectable* raw = Engine_Object(h);
            if (!raw) continue;
            Inspectable obj{nullptr};
            winrt::copy_from_abi(obj, raw);
            raw->Release();
            try {
                auto fe = obj.try_as<mux::FrameworkElement>();
                if (!fe || !onScreen(fe)) continue;
                if (auto p = obj.try_as<muxc::Panel>()) {
                    if (BrushLooksLight(p.Background())) { s_light = true; s_checkedAt = now; return s_light; }
                } else if (auto cc = obj.try_as<muxc::ContentControl>()) {
                    if (BrushLooksLight(cc.Background())) { s_light = true; s_checkedAt = now; return s_light; }
                } else if (auto b = obj.try_as<muxc::Border>()) {
                    if (BrushLooksLight(b.Background())) { s_light = true; s_checkedAt = now; return s_light; }
                }
            } catch (...) {}
        }
    }

    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS) {
        s_light = v != 0;
    } else {
        s_light = false;
    }
    s_checkedAt = now;
    return s_light;
}

std::wstring GetThemedSvgPath(Tool tool, bool isLight) {
    std::wstring srcPath = CustomToolSvgPath(tool);
    if (srcPath.empty()) return L"";

    if (!isLight) return srcPath;

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring outPath = std::wstring(tempDir) + L"diatweaks_tool_" + std::to_wstring(static_cast<int>(tool)) + L"_light.svg";

    HANDLE hFile = CreateFileW(srcPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hFile, nullptr);
        if (sz > 0 && sz < 65536) {
            std::string content(sz, '\0');
            DWORD read = 0;
            if (ReadFile(hFile, &content[0], sz, &read, nullptr) && read > 0) {
                CloseHandle(hFile);

                std::string target = "#1C1C1E";
                size_t pos = 0;
                while ((pos = content.find("#FFFFFF", pos)) != std::string::npos) {
                    content.replace(pos, 7, target);
                    pos += target.length();
                }
                pos = 0;
                while ((pos = content.find("#ffffff", pos)) != std::string::npos) {
                    content.replace(pos, 7, target);
                    pos += target.length();
                }
                pos = 0;
                while ((pos = content.find("\"white\"", pos)) != std::string::npos) {
                    content.replace(pos, 7, "\"" + target + "\"");
                    pos += target.length() + 2;
                }

                HANDLE hOut = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                if (hOut != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(hOut, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
                    CloseHandle(hOut);
                    return outPath;
                }
            } else {
                CloseHandle(hFile);
            }
        } else {
            CloseHandle(hFile);
        }
    }
    return srcPath;
}

// Builds the strip of buttons that replaces the extension buttons.
mux::FrameworkElement BuildToolbar(double barWidth) {
    bool isLight = IsToolbarLight();

    muxc::StackPanel strip;
    strip.Name(kToolbarName);
    strip.Orientation(muxc::Orientation::Horizontal);
    strip.Spacing(2);
    strip.VerticalAlignment(mux::VerticalAlignment::Center);
    strip.HorizontalAlignment(mux::HorizontalAlignment::Left);

    for (int i = 0; i < kToolCount; ++i) {
        Tool tool = static_cast<Tool>(i);

        // 3 Dividers matching Arc: after CopyLink, after Camera, after Inspect
        if (tool == kCapturePortrait || tool == kConsole || tool == kExtensions) {
            muxsh::Rectangle div;
            div.Width(1);
            div.Height(14);
            div.Margin(mux::Thickness{3, 0, 3, 0});
            div.VerticalAlignment(mux::VerticalAlignment::Center);
            muxm::SolidColorBrush divBrush;
            divBrush.Color(isLight ? winrt::Windows::UI::ColorHelper::FromArgb(40, 0, 0, 0)
                                   : winrt::Windows::UI::ColorHelper::FromArgb(40, 255, 255, 255));
            div.Fill(divBrush);
            strip.Children().Append(div);
        }

        muxc::Button button;
        button.Width(26);
        button.Height(26);
        button.Padding(mux::Thickness{0, 0, 0, 0});
        button.CornerRadius(mux::CornerRadius{4, 4, 4, 4});

        muxm::SolidColorBrush clear;
        clear.Color(winrt::Windows::UI::ColorHelper::FromArgb(1, 255, 255, 255));
        button.Background(clear);
        button.BorderThickness(mux::Thickness{0, 0, 0, 0});
        button.Name(std::wstring(L"DiaTweaksDevModeTool") + std::to_wstring(i));
        button.Foreground(muxm::SolidColorBrush{isLight ? ColorFrom(0x1C1C1E) : winrt::Windows::UI::Colors::White()});
        button.UseSystemFocusVisuals(false);

        // Check if user provided a custom 24x24 SVG icon in assets/icons/
        std::wstring customSvg = GetThemedSvgPath(tool, isLight);
        if (!customSvg.empty()) {
            try {
                muxc::Image img;
                img.Width(18);
                img.Height(18);
                img.HorizontalAlignment(mux::HorizontalAlignment::Center);
                img.VerticalAlignment(mux::VerticalAlignment::Center);
                img.IsHitTestVisible(false);
                winrt::Windows::Foundation::Uri uri(std::wstring(L"file:///") + customSvg);
                muxm_imaging::SvgImageSource svg(uri);
                svg.RasterizePixelWidth(36);
                svg.RasterizePixelHeight(36);
                img.Source(svg);
                button.Content(img);
            } catch (...) {
                muxc::FontIcon icon;
                icon.Glyph(ToolGlyph(tool));
                icon.FontSize(12);
                try {
                    muxm::FontFamily fontFam(L"Segoe Fluent Icons, Segoe MDL2 Assets");
                    icon.FontFamily(fontFam);
                } catch (...) {}
                icon.IsHitTestVisible(false);
                button.Content(icon);
            }
        } else {
            muxc::FontIcon icon;
            icon.Glyph(ToolGlyph(tool));
            icon.FontSize(12);
            try {
                muxm::FontFamily fontFam(L"Segoe Fluent Icons, Segoe MDL2 Assets");
                icon.FontFamily(fontFam);
            } catch (...) {}
            icon.IsHitTestVisible(false);
            button.Content(icon);
        }
        button.IsHitTestVisible(true);

        const wchar_t* tip = ToolTipText(tool);
        if (tip && *tip) {
            muxc::ToolTipService::SetToolTip(button, winrt::box_value(winrt::hstring(tip)));
        }

        if (tool == kCapturePortrait) {
            button.Flyout(BuildPortraitFlyout());
        } else {
            button.Click([tool](const winrt::Windows::Foundation::IInspectable&,
                               const winrt::Windows::Foundation::IInspectable&) {
                RunTool(tool);
            });
        }

        strip.Children().Append(button);
    }

    return strip;
}

// Recolour the existing strip in place. Dia changes the toolbar palette when the
// page or hover state changes; rebuilding the strip there is what made the icons
// flicker, so only the colours and themed icons are swapped.
void ApplyToolbarLight(const mux::FrameworkElement& strip, bool isLight) {
    auto panel = strip.try_as<muxc::Panel>();
    if (!panel) return;

    muxm::SolidColorBrush fg;
    fg.Color(isLight ? ColorFrom(0x1C1C1E) : winrt::Windows::UI::Colors::White());
    const std::wstring prefix = L"DiaTweaksDevModeTool";

    for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
        auto child = panel.Children().GetAt(i);
        if (auto button = child.try_as<muxc::Button>()) {
            try { button.Foreground(fg); } catch (...) {}
            std::wstring name(button.Name());
            if (name.rfind(prefix, 0) == 0) {
                int index = _wtoi(name.c_str() + prefix.size());
                if (index >= 0 && index < kToolCount) {
                    try {
                        if (auto img = button.Content().try_as<muxc::Image>()) {
                            std::wstring path = GetThemedSvgPath(static_cast<Tool>(index), isLight);
                            if (!path.empty()) {
                                muxm_imaging::SvgImageSource svg(
                                    winrt::Windows::Foundation::Uri(std::wstring(L"file:///") + path));
                                svg.RasterizePixelWidth(36);
                                svg.RasterizePixelHeight(36);
                                img.Source(svg);
                            }
                        }
                    } catch (...) {}
                }
            }
        } else if (auto rect = child.try_as<muxsh::Rectangle>()) {
            try {
                muxm::SolidColorBrush div;
                div.Color(isLight ? winrt::Windows::UI::ColorHelper::FromArgb(40, 0, 0, 0)
                                  : winrt::Windows::UI::ColorHelper::FromArgb(40, 255, 255, 255));
                rect.Fill(div);
            } catch (...) {}
        }
    }
}

muxm::Brush BuildStripes(double width, double height) {
    const double band = 16.0;
    const double reach = width + height;

    muxm::LinearGradientBrush stripe;
    stripe.StartPoint(winrt::Windows::Foundation::Point{0.0f, 0.0f});
    stripe.EndPoint(winrt::Windows::Foundation::Point{
        static_cast<float>(reach), static_cast<float>(reach)});
    stripe.MappingMode(muxm::BrushMappingMode::Absolute);

    int bands = static_cast<int>(reach / band);
    if (bands < 2) bands = 2;

    const winrt::Windows::UI::Color pale = ColorFrom(0xFFFFFF, 0x14);
    const winrt::Windows::UI::Color darker = ColorFrom(0x000000, 0x14);

    for (int i = 0; i < bands; ++i) {
        double start = static_cast<double>(i) / bands;
        double edge = (i + 0.5) / bands;
        double next = static_cast<double>(i + 1) / bands;

        muxm::GradientStop paleStop;
        paleStop.Color(pale);
        paleStop.Offset(start);
        stripe.GradientStops().Append(paleStop);

        muxm::GradientStop darkStop;
        darkStop.Color(darker);
        darkStop.Offset(edge);
        stripe.GradientStops().Append(darkStop);

        muxm::GradientStop paleAgain;
        paleAgain.Color(pale);
        paleAgain.Offset(next);
        stripe.GradientStops().Append(paleAgain);
    }

    return stripe;
}

winrt::weak_ref<muxc::ContentControl> g_toolbarHost;
winrt::Windows::Foundation::IInspectable g_toolbarHostContent{nullptr};
winrt::weak_ref<muxc::Panel> g_toolbarPanel;
winrt::weak_ref<mux::FrameworkElement> g_extensionsRepeater;
bool g_extensionsWereVisible = false;
winrt::Windows::Foundation::IInspectable g_toolbarBarBackground{nullptr};
std::vector<winrt::weak_ref<mux::FrameworkElement>> g_hiddenToolbarElements;
// Keep the strip and its stripe brush alive across Dia's hover rebuilds, so a
// hover never recreates (and visibly flickers) the developer toolbar.
mux::FrameworkElement g_toolbarKeep{nullptr};
// Delay tearing the strip down so a momentary "not in dev mode" reading (page
// reload, title rewrite, slow CDP) cannot restore Dia's hover-only toolbar
// opacity and make the icons blink out while the pointer is away.
std::atomic<ULONGLONG> g_toolbarHideAt{0};
constexpr unsigned long long kToolbarHideDelayMs = 1500;
muxm::Brush g_toolbarStripes{nullptr};
winrt::weak_ref<muxc::Border> g_toolbarBar;
bool g_toolbarLight = false;
bool g_toolbarLightKnown = false;
InstanceHandle g_toolbarHostHandle = 0;
double g_stripeW = 0;
double g_stripeH = 0;

bool IsEllipsisElement(mux::FrameworkElement fe) {
    if (!fe) return false;
    std::wstring name(fe.Name());
    for (auto& c : name) c = static_cast<wchar_t>(::towlower(c));
    if (name.find(L"caption") != std::wstring::npos ||
        name.find(L"minimize") != std::wstring::npos ||
        name.find(L"maximize") != std::wstring::npos ||
        name.find(L"close") != std::wstring::npos) {
        return false;
    }
    if (name.find(L"more") != std::wstring::npos ||
        name.find(L"ellipsis") != std::wstring::npos ||
        name.find(L"overflow") != std::wstring::npos ||
        name.find(L"menu") != std::wstring::npos) {
        return true;
    }
    std::vector<mux::DependencyObject> stack;
    stack.push_back(fe.as<mux::DependencyObject>());
    int guard = 0;
    while (!stack.empty() && guard++ < 30) {
        auto cur = stack.back();
        stack.pop_back();
        if (auto fi = cur.try_as<muxc::FontIcon>()) {
            if (fi.Glyph() == L"\uE712" || fi.Glyph() == L"\uE10C") return true;
        }
        if (auto tb = cur.try_as<muxc::TextBlock>()) {
            if (tb.Text() == L"\uE712" || tb.Text() == L"…" || tb.Text() == L"..." || tb.Text() == L"\u2026") return true;
        }
        int count = muxm::VisualTreeHelper::GetChildrenCount(cur);
        for (int i = 0; i < count; ++i) {
            stack.push_back(muxm::VisualTreeHelper::GetChild(cur, i));
        }
    }
    return false;
}

// Dia shows the pinned-extension buttons again on hover. Keep only our strip
// visible while Developer Mode owns that row, without rebuilding anything.
void EnforceToolbarOnly(const muxc::Panel& panel, const mux::FrameworkElement& strip) {
    if (!panel || !strip) return;
    for (uint32_t i = 0; i < panel.Children().Size(); ++i) {
        auto child = panel.Children().GetAt(i).try_as<mux::FrameworkElement>();
        if (!child || child == strip) continue;
        if (child.Name() == kToolbarName) {
            try { child.Visibility(mux::Visibility::Collapsed); } catch (...) {}
            continue;
        }
        try {
            if (child.Visibility() == mux::Visibility::Visible) {
                child.Visibility(mux::Visibility::Collapsed);
                bool known = false;
                for (auto& weak : g_hiddenToolbarElements)
                    if (weak.get() == child) { known = true; break; }
                if (!known) g_hiddenToolbarElements.push_back(winrt::make_weak(child));
            }
        } catch (...) {}
    }
}

void EnsureStripInPanel(const muxc::Panel& panel, const mux::FrameworkElement& strip) {
    if (!panel || !strip) return;
    bool present = false;
    for (uint32_t i = 0; i < panel.Children().Size(); ++i)
        if (panel.Children().GetAt(i) == strip) { present = true; break; }
    if (!present) {
        try {
            if (auto old = muxm::VisualTreeHelper::GetParent(strip).try_as<muxc::Panel>()) {
                for (int i = static_cast<int>(old.Children().Size()) - 1; i >= 0; --i)
                    if (old.Children().GetAt(i) == strip) old.Children().RemoveAt(i);
            }
        } catch (...) {}
        try { panel.Children().InsertAt(0, strip); } catch (...) {}
    }
    try {
        strip.Visibility(mux::Visibility::Visible);
        strip.Opacity(1.0);
    } catch (...) {}
}

void ShowToolbar() {
    if (IsNewTabPage()) return;
    bool placed = false;

    // Ensure UrlBarTrailingStack is visible and opaque when DevMode toolbar is active
    for (auto h : Engine_FindNodes(L"UrlBarTrailingStack")) {
        IInspectable* r = Engine_Object(h);
        if (!r) continue;
        Inspectable obj{nullptr};
        winrt::copy_from_abi(obj, r);
        r->Release();
        if (auto stack = obj.try_as<mux::FrameworkElement>()) {
            try {
                stack.Visibility(mux::Visibility::Visible);
                stack.Opacity(1.0);
            } catch (...) {}
        }
    }

    bool isLight = IsToolbarLight();
    bool lightChanged = !g_toolbarLightKnown || g_toolbarLight != isLight;
    g_toolbarLight = isLight;
    g_toolbarLightKnown = true;
    if (lightChanged) Log("developer mode: toolbar palette %s", isLight ? "light" : "dark");

    muxc::ContentControl targetHost{nullptr};
    muxc::Panel targetPanel{nullptr};
    Inspectable originalContent{nullptr};
    InstanceHandle targetHandle = 0;

    // Reuse the strip across Dia's hover rebuilds: the row's content object may
    // be replaced, but the strip itself stays alive and is simply reinserted.
    if (g_toolbarHostHandle && SameWindow(g_toolbarHostHandle, g_markedRowHandle)) {
        if (auto host = g_toolbarHost.get()) {
            try {
                if (auto p = host.Content().try_as<muxc::Panel>()) {
                    targetHost = host;
                    targetPanel = p;
                    originalContent = host.Content();
                    targetHandle = g_toolbarHostHandle;
                }
            } catch (...) {}
        }
    }

    if (!targetPanel) {
        for (auto handle : Engine_FindNodes(L"PinnedExtensionsHost")) {
            if (!SameWindow(handle, g_markedRowHandle)) continue;
            IInspectable* raw = Engine_Object(handle);
            if (!raw) continue;
            Inspectable object{nullptr};
            winrt::copy_from_abi(object, raw);
            raw->Release();

            auto host = object.try_as<muxc::ContentControl>();
            if (!host) continue;

            try { host.Visibility(mux::Visibility::Visible); } catch (...) {}

            Inspectable original{nullptr};
            try {
                original = host.Content();
            } catch (...) {}

            auto panel = original.try_as<muxc::Panel>();
            if (!panel) {
                muxc::StackPanel sp;
                sp.Orientation(muxc::Orientation::Horizontal);
                host.Content(sp);
                panel = sp;
            }

            targetHost = host;
            targetPanel = panel;
            originalContent = original;
            targetHandle = handle;
            break;
        }
    }

    // Dia collapses PinnedExtensionsHost again when the pointer leaves the
    // toolbar (and shows it on hover). The host is now reused from the cache on
    // later ticks, so the one-time "host.Visibility(Visible)" in the search
    // branch no longer runs. Re-assert visibility AND opacity on every tick so
    // the strip never depends on the pointer being over the toolbar to be seen.
    if (targetHost) {
        try { targetHost.Visibility(mux::Visibility::Visible); } catch (...) {}
        try { targetHost.Opacity(1.0); } catch (...) {}
    }

    if (targetPanel) {
        auto strip = g_toolbarKeep;
        bool builtNew = !strip;
        if (builtNew) strip = BuildToolbar(0);
        if (!strip) return;

        EnsureStripInPanel(targetPanel, strip);
        // Keep the whole chain from the strip up to the host visible/opaque.
        // Dia binds hover opacity on intermediate containers; without this the
        // icons fade out whenever the pointer is not over the toolbar.
        {
            mux::DependencyObject cur = strip.as<mux::DependencyObject>();
            mux::DependencyObject stop = targetHost ? targetHost.as<mux::DependencyObject>() : nullptr;
            for (int i = 0; cur && i < 6; ++i) {
                if (auto fe = cur.try_as<mux::FrameworkElement>()) {
                    try { fe.Visibility(mux::Visibility::Visible); } catch (...) {}
                    try { fe.Opacity(1.0); } catch (...) {}
                }
                if (cur == stop) break;
                cur = muxm::VisualTreeHelper::GetParent(cur);
            }
        }
        if (builtNew || lightChanged) ApplyToolbarLight(strip, isLight);
        EnforceToolbarOnly(targetPanel, strip);

        // Hide any sibling ellipsis/more button in the parent panel.
        try {
            if (auto parentPanel = targetHost.Parent().try_as<muxc::Panel>()) {
                for (uint32_t i = 0; i < parentPanel.Children().Size(); ++i) {
                    if (auto sib = parentPanel.Children().GetAt(i).try_as<mux::FrameworkElement>()) {
                        if (sib == targetHost) continue;
                        if (IsEllipsisElement(sib) && sib.Visibility() == mux::Visibility::Visible) {
                            sib.Visibility(mux::Visibility::Collapsed);
                            g_hiddenToolbarElements.push_back(winrt::make_weak(sib));
                        }
                    }
                }
            }
        } catch (...) {}

        // Also check any known More/Ellipsis nodes.
        for (const wchar_t* name : {L"MoreExtensionsButton", L"ExtensionsMenuButton", L"BrowserActionsMenuButton", L"MoreButton", L"MoreActionsButton"}) {
            for (auto h : Engine_FindNodes(name)) {
                if (!SameWindow(h, g_markedRowHandle)) continue;
                IInspectable* r = Engine_Object(h);
                if (!r) continue;
                Inspectable obj{nullptr};
                winrt::copy_from_abi(obj, r);
                r->Release();
                if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                    if (fe.Visibility() == mux::Visibility::Visible) {
                        fe.Visibility(mux::Visibility::Collapsed);
                        g_hiddenToolbarElements.push_back(winrt::make_weak(fe));
                    }
                }
            }
        }

        g_toolbarKeep = strip;
        g_toolbarHost = winrt::make_weak(targetHost);
        g_toolbarHostContent = originalContent;
        g_toolbarHostHandle = targetHandle;
        g_toolbarPanel = targetPanel;
        g_toolbar = winrt::make_weak(strip);
        placed = builtNew;
    }

    for (auto handle : Engine_FindNodes(L"PinnedExtensionsRepeater")) {
        if (!SameWindow(handle, g_markedRowHandle)) continue;
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto repeater = object.try_as<mux::FrameworkElement>();
        if (!repeater) continue;

        g_extensionsWereVisible = repeater.Visibility() == mux::Visibility::Visible;
        repeater.Visibility(mux::Visibility::Collapsed);
        g_extensionsRepeater = winrt::make_weak(repeater);
    }

    for (auto handle : Engine_FindNodes(L"AssistantBar")) {
        if (!SameWindow(handle, g_markedRowHandle)) continue;
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto border = object.try_as<muxc::Border>();
        if (!border) continue;

        double width = border.ActualWidth();
        double height = border.ActualHeight();
        if (width < 20 || height < 10) continue;

        // Skip New Tab center chatbox (which has Y > 120)
        try {
            auto peer = muxa_peers::FrameworkElementAutomationPeer::CreatePeerForElement(border);
            if (peer) {
                auto rect = peer.GetBoundingRectangle();
                if (rect.Y > 120) continue;
            }
        } catch (...) {}

        if (!g_toolbarBarBackground) {
            g_toolbarBarBackground = border.Background();
        }
        if (!g_toolbarStripes || std::abs(g_stripeW - width) > 1 || std::abs(g_stripeH - height) > 1) {
            g_toolbarStripes = BuildStripes(width, height);
            g_stripeW = width;
            g_stripeH = height;
        }
        // Dia repaints the toolbar during hover; keep the stripes on top every tick.
        if (border.Background() != g_toolbarStripes) border.Background(g_toolbarStripes);
        g_toolbarBar = winrt::make_weak(border);
        break;
    }

    if (placed) Log("developer mode: the developer toolbar is in place");
}

void HideToolbar() {
    if (!g_toolbar.get() && !g_toolbarKeep && !g_toolbarPanel.get() && !g_toolbarBar.get() &&
        g_hiddenToolbarElements.empty() && !g_toolbarHost.get()) return;
    if (auto panel = g_toolbarPanel.get()) {
        for (int i = static_cast<int>(panel.Children().Size()) - 1; i >= 0; --i) {
            if (auto child = panel.Children().GetAt(i).try_as<mux::FrameworkElement>()) {
                if (child.Name() == kToolbarName) {
                    panel.Children().RemoveAt(i);
                }
            }
        }
        g_toolbarPanel = nullptr;
    }

    for (auto handle : Engine_FindNodes(L"PinnedExtensionsHost")) {
        IInspectable* raw = Engine_Object(handle);
        if (!raw) continue;
        Inspectable object{nullptr};
        winrt::copy_from_abi(object, raw);
        raw->Release();

        auto host = object.try_as<muxc::ContentControl>();
        if (!host) continue;
        try {
            if (auto panel = host.Content().try_as<muxc::Panel>()) {
                for (int i = static_cast<int>(panel.Children().Size()) - 1; i >= 0; --i) {
                    if (auto child = panel.Children().GetAt(i).try_as<mux::FrameworkElement>()) {
                        if (child.Name() == kToolbarName) {
                            panel.Children().RemoveAt(i);
                        }
                    }
                }
            }
        } catch (...) {}
    }

    g_toolbarHost = nullptr;
    g_toolbarHostContent = nullptr;
    g_toolbarHostHandle = 0;
    g_toolbar = nullptr;
    g_toolbarKeep = nullptr;
    g_toolbarLightKnown = false;

    if (IsNewTabPage()) {
        g_hiddenToolbarElements.clear();
        g_extensionsRepeater = nullptr;
    } else {
        if (auto repeater = g_extensionsRepeater.get()) {
            repeater.Visibility(mux::Visibility::Visible);
            g_extensionsRepeater = nullptr;
        }

        for (auto& weak : g_hiddenToolbarElements) {
            if (auto el = weak.get()) {
                try {
                    el.Visibility(mux::Visibility::Visible);
                } catch (...) {}
            }
        }
        g_hiddenToolbarElements.clear();
    }

    if (auto bar = g_toolbarBar.get()) {
        if (g_toolbarBarBackground) {
            try {
                bar.Background(g_toolbarBarBackground.as<muxm::Brush>());
            } catch (...) {}
        }
    } else {
        for (auto handle : Engine_FindNodes(L"AssistantBar")) {
            IInspectable* raw = Engine_Object(handle);
            if (!raw) continue;
            Inspectable object{nullptr};
            winrt::copy_from_abi(object, raw);
            raw->Release();

            auto border = object.try_as<muxc::Border>();
            if (!border) continue;

            if (g_toolbarBarBackground) {
                try {
                    border.Background(g_toolbarBarBackground.as<muxm::Brush>());
                } catch (...) {}
            }
        }
    }
    g_toolbarBar = nullptr;
    g_toolbarStripes = nullptr;
    g_stripeW = 0;
    g_stripeH = 0;
    g_toolbarBarBackground = nullptr;
}

void UpdateToolbar() {
    if (ActiveTabInDevMode()) {
        g_toolbarHideAt.store(0);
        ShowToolbar();
        return;
    }
    ULONGLONG deadline = g_toolbarHideAt.load();
    if (!g_toolbarKeep && !deadline) return;  // nothing placed, nothing to hide
    if (!deadline) {
        g_toolbarHideAt.store(GetTickCount64() + kToolbarHideDelayMs);
        return;
    }
    if (GetTickCount64() >= deadline) {
        g_toolbarHideAt.store(0);
        HideToolbar();
    }
}



DWORD WINAPI WorkerThread(LPVOID) {
    while (!g_stopWorker) {
        Page last;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            last = g_page;
        }

        Page page = ReadPage(last);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (page.origin != g_page.origin)
                Log("developer mode: page is now %ls", page.origin.c_str());
            g_page = page;
        }

        for (int i = 0; i < 15 && !g_stopWorker; ++i) {
            if (g_wakeWorker.exchange(false)) break;
            Sleep(100);
        }
    }
    g_workerRunning = false;
    return 0;
}

void StartWorker() {
    bool expected = false;
    if (!g_workerRunning.compare_exchange_strong(expected, true)) return;
    g_stopWorker = false;
    HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (h) {
        CloseHandle(h);
    } else {
        g_workerRunning = false;
    }
}

void StopWorker() {
    g_stopWorker = true;
    g_workerRunning = false;
}

}  // namespace

void SetEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_enabled == enabled) return;
        g_enabled = enabled;
    }
    if (enabled) {
        LoadSites();
        StartWorker();
    } else {
        StopWorker();
        ClearMark();
        HideToolbar();
    }
}

void SetLocalhost(bool enabled) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_localhost = enabled;
}

bool Enabled() {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_enabled;
}

std::wstring CurrentOrigin() {
    return PageOrigin();
}

bool ActiveTabInDevMode() {
    if (IsNewTabPage()) return false;
    // A real tab switch clears this latch, so leaving a developer tab still
    // tears the UI down promptly. Momentary tracker misses must not flip the
    // state to false (see the internal ActiveTabInDevMode used by Tick).
    if (GetTickCount64() < g_pageStaleUntil.load()) return false;
    if (TabInDevMode(CurrentPage())) {
        g_devActiveUntil.store(GetTickCount64() + kDevActiveLatchMs);
        return true;
    }
    return GetTickCount64() < g_devActiveUntil.load();
}

bool ToolbarActive() {
    return static_cast<bool>(g_toolbarKeep);
}

void Tick() {
    if (!Enabled()) return;

    if (IsNewTabPage()) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_page.url.clear();
            g_page.origin.clear();
            g_page.socket.clear();
            g_page.title = L"New Tab";
            g_page.targetId.clear();
            g_page.devTools = false;
        }
        g_devActiveUntil.store(0);
        g_toolbarHideAt.store(0);
        HideToolbar();
        return;
    }

    std::wstring tabTitle = GetSelectedTabTitle();

    // The tab row switches immediately; the page tracker catches up on its next
    // pass. When the selection moved, forget the previous tab's page and hold
    // the developer UI back for a moment so the mark and toolbar cannot appear
    // on a tab that is not in developer mode.
    //
    // Only the letters of the title count: pages that rewrite their title all
    // the time (unread counts, timers) must not be mistaken for a tab switch.
    auto titleKey = [](const std::wstring& title) {
        std::wstring key;
        for (wchar_t c : title) {
            if (iswalpha(static_cast<wint_t>(c)))
                key.push_back(static_cast<wchar_t>(towlower(c)));
        }
        if (key.size() > 24) key.resize(24);
        return key;
    };
    static std::wstring s_lastTabTitleKey;
    std::wstring tabKey = titleKey(tabTitle);
    if (!tabKey.empty() && tabKey != s_lastTabTitleKey) {
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_page.url.clear();
            g_page.origin.clear();
            g_page.socket.clear();
            g_page.targetId.clear();
            g_page.devTools = false;
            // Matches the new tab, so the worker can find it right away.
            g_page.title = tabTitle;
        }
        g_pageStaleUntil.store(GetTickCount64() + kTabSwitchGraceMs);
        g_devActiveUntil.store(0);
        g_toolbarHideAt.store(0);
        g_wakeWorker.store(true);
        ClearMark();
        HideToolbar();
        Log("developer mode: tab switched to %ls, holding the developer UI back until the page tracker catches up",
            tabTitle.c_str());
    }
    if (!tabKey.empty()) s_lastTabTitleKey = tabKey;

    std::wstring addr = NormalizeUrl(GetAddressBarUrl());
    if (GetTickCount64() >= g_pageStaleUntil.load() &&
        !addr.empty() && addr.find(L"newtab") == std::wstring::npos) {
        std::wstring origin = OriginOf(addr);
        if (!origin.empty()) {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_page.origin.empty() || g_page.origin != origin || g_page.url != addr) {
                g_page.url = addr;
                g_page.origin = origin;
                if (g_page.targetId.empty()) g_page.targetId = L"page";
                if (!tabTitle.empty()) g_page.title = tabTitle;
            }
        }
    }

    ClearStuckCopiedText();
    UpdateMenus();
    MarkSidebarTab();
    UpdateToolbar();
}

}  // namespace DevMode
