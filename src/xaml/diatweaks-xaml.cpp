// diatweaks-xaml.dll - applies DiaTweaks' toolbar tweaks to a running Dia.
//
// Dia's chrome is WinUI 3 XAML living inside a detached content island. Windows
// exposes that UI through the XAML diagnostics service - the same service Visual
// Studio's Live Visual Tree uses. This DLL is loaded into Dia's browser process
// (never into the signed package), asks WinUI for its diagnostics endpoint, and
// from then on receives the live element tree as Dia builds and changes it.
//
// That replaces the earlier heap scan completely: every change is made against
// a real InstanceHandle from WinUI, and every value Dia owned is captured before
// we override it and put back when the tweak is switched off.
//
// Config: %LOCALAPPDATA%\DiaTweaks\xaml.cfg, polled every 250 ms.
// Log:    %LOCALAPPDATA%\DiaTweaks\xaml.log

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ocidl.h>
#include <xamlOM.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Media.Imaging.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Provider.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <robuffer.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#include "devmode.h"
#include "engine.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <ctime>
#include <chrono>
#include "paintings_catalog.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <new>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxd = winrt::Microsoft::UI::Xaml::Data;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxk = winrt::Microsoft::UI::Xaml::Markup;
namespace muxsh = winrt::Microsoft::UI::Xaml::Shapes;
namespace mucomp = winrt::Microsoft::UI::Composition;
using Inspectable = winrt::Windows::Foundation::IInspectable;

namespace SettingsUi {
    void ScanForSettings();
    void SyncToggles();
}

// {6a9f2b71-3c4d-4e8a-9b2f-7d1e5c8a0b63}
static const CLSID CLSID_DiaTweaksXaml = {
    0x6a9f2b71, 0x3c4d, 0x4e8a, {0x9b, 0x2f, 0x7d, 0x1e, 0x5c, 0x8a, 0x0b, 0x63}};

static HINSTANCE g_module{};
static wchar_t g_dir[MAX_PATH]{};
static wchar_t g_logPath[MAX_PATH]{};
static wchar_t g_configPath[MAX_PATH]{};
static wchar_t g_statePath[MAX_PATH]{};
static wchar_t g_dumpRequestPath[MAX_PATH]{};
static wchar_t g_treePath[MAX_PATH]{};
static wchar_t g_ntpProbeRequestPath[MAX_PATH]{};
static wchar_t g_ntpProbePath[MAX_PATH]{};
static wchar_t g_changelogPath[MAX_PATH]{};

// ---------------------------------------------------------------- logging

static std::mutex g_logLock;

static void Log(const wchar_t* fmt, ...) {
    wchar_t body[2048]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(body, _countof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2304]{};
    swprintf_s(line, L"%02d:%02d:%02d.%03d [%lu] %s\r\n", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, GetCurrentThreadId(), body);

    std::lock_guard<std::mutex> lock(g_logLock);
    HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char utf8[4608]{};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD written = 0;
    if (bytes > 1)
        WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    CloseHandle(file);
}

static std::wstring Bstr(BSTR value) {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring();
}

static bool PatchBytes(uint8_t* at, const uint8_t* want, size_t len) {
    DWORD old;
    if (!VirtualProtect(at, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, want, len);
    VirtualProtect(at, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, len);
    return true;
}

static uint8_t* FindOnce(uint8_t* lo, uint8_t* hi, const uint8_t* pat, size_t len) {
    uint8_t* p = std::search(lo, hi, pat, pat + len);
    if (p == hi || std::search(p + 1, hi, pat, pat + len) != hi) return nullptr;
    return p;
}

static void ApplyPinnedExtensionsLimit(int cap) {
    if (cap <= 4 || cap > 100) return;
    auto dia = (uint8_t*)GetModuleHandleW(L"Dia.dll");
    if (!dia) return;
    auto nt = (IMAGE_NT_HEADERS*)(dia + ((IMAGE_DOS_HEADER*)dia)->e_lfanew);
    static const uint8_t a[] = {0x48, 0x83, 0xfe, 0x04, 0x41, 0xbe, 0x04, 0x00, 0x00, 0x00, 0x4c, 0x0f, 0x42, 0xf6};
    static const uint8_t b[] = {0x49, 0x83, 0xff, 0x04, 0xba, 0x04, 0x00, 0x00, 0x00, 0x49, 0x0f, 0x42, 0xd7};
    static const uint8_t c[] = {0x48, 0x83, 0xfe, 0x05};
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
        if (memcmp(sec->Name, ".text", 6)) continue;
        uint8_t *lo = dia + sec->VirtualAddress, *hi = lo + sec->Misc.VirtualSize;
        uint8_t* pa = FindOnce(lo, hi, a, sizeof(a));
        uint8_t* pb = FindOnce(lo, hi, b, sizeof(b));
        uint8_t* pc = pa ? FindOnce(pa, pa + 64, c, sizeof(c)) : nullptr;
        if (!pa || !pb || !pc) return;
        uint8_t na[sizeof(a)], nb[sizeof(b)], nc[sizeof(c)];
        memcpy(na, a, sizeof(a)), memcpy(nb, b, sizeof(b)), memcpy(nc, c, sizeof(c));
        na[3] = na[6] = (uint8_t)cap;
        nb[3] = nb[5] = (uint8_t)cap;
        nc[3] = (uint8_t)(cap + 1);
        PatchBytes(pa, na, sizeof(na));
        PatchBytes(pb, nb, sizeof(nb));
        PatchBytes(pc, nc, sizeof(nc));
        Log(L"pinned extensions: patched cap to %d", cap);
        return;
    }
}

// ---------------------------------------------------------------- sidebar grain
// Procedural tactile textures (sand, finegrain, denim, paper fiber).
namespace Grain {

static inline float Hash32(int x, int y, uint32_t seed) {
    x = (x % 200 + 200) % 200;
    y = (y % 200 + 200) % 200;
    uint32_t n = ((uint32_t)x * 0x1f123bb5u) ^ ((uint32_t)y * 0x5f356495u) ^ (seed * 0x9e3779b9u);
    n ^= (n >> 16);
    n *= 0x7feb352du;
    n ^= (n >> 15);
    n *= 0x846ca68bu;
    n ^= (n >> 16);
    return (n & 0xffffff) / 16777215.0f;
}

static void GenerateSand(std::vector<float>& field) {
    // Continuous crystalline sand/sandstone micro-grit (NO discrete spots/measles!)
    std::fill(field.begin(), field.end(), 0.0f);

    std::vector<float> base1(40000);
    std::vector<float> base2(40000);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            // 1-pixel high-frequency silica micro-grit
            float r1 = Hash32(x, y, 101);
            float r2 = Hash32(x, y, 102);
            base1[y * 200 + x] = (r1 + r2 - 1.0f) * 1.8f;

            // 2-pixel fine mineral grain
            float g1 = Hash32(x / 2, y / 2, 201);
            float g2 = Hash32(x / 2, y / 2, 202);
            base2[y * 200 + x] = (g1 + g2 - 1.0f) * 1.2f;
        }
    }

    // Directional relief: sunlight catching crystalline facet edges
    for (int y = 0; y < 200; y++) {
        int ym1 = (y - 1 + 200) % 200;
        for (int x = 0; x < 200; x++) {
            int xm1 = (x - 1 + 200) % 200;
            float facet = (base1[y * 200 + x] - base1[ym1 * 200 + xm1]) * 0.70f;
            float mineral = base2[y * 200 + x] * 0.45f + base1[y * 200 + x] * 0.55f;
            field[y * 200 + x] = mineral + facet;
        }
    }
}

static void GenerateFineGrain(std::vector<float>& field) {
    // Classic 35mm film emulsion micro-grain (isotropic, 1-pixel scale)
    std::fill(field.begin(), field.end(), 0.0f);
    std::vector<float> base(40000);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            float r1 = Hash32(x, y, 1337);
            float r2 = Hash32(x, y, 2337);
            float r3 = Hash32(x, y, 3337);
            base[y * 200 + x] = (r1 + r2 + r3 - 1.5f) * 2.0f;
        }
    }

    // Organic emulsion clumping (2-pixel micro-clusters)
    std::vector<float> clustered(40000);
    for (int y = 0; y < 200; y++) {
        int ym1 = (y - 1 + 200) % 200, yp1 = (y + 1) % 200;
        for (int x = 0; x < 200; x++) {
            int xm1 = (x - 1 + 200) % 200, xp1 = (x + 1) % 200;
            clustered[y * 200 + x] = base[y * 200 + x] * 0.50f +
                0.125f * (base[y * 200 + xm1] + base[y * 200 + xp1] + base[ym1 * 200 + x] + base[yp1 * 200 + x]);
        }
    }

    // Non-linear crystalline response for sharp photographic grain
    for (size_t i = 0; i < 40000; i++) {
        float val = 0.55f * base[i] + 0.45f * clustered[i];
        if (val > 0.0f) {
            field[i] = powf(val, 0.85f);
        } else {
            field[i] = -powf(-val, 0.85f);
        }
    }
}

static void GenerateDenim(std::vector<float>& field) {
    // Ultra-fine luxury cotton micro-twill (P = 2: 2px per thread, 100 threads across 200px)
    std::fill(field.begin(), field.end(), 0.0f);
    const float kPi = 3.14159265f;
    const int P = 2; // 2-pixel micro-thread pitch (100 threads across 200px)
    for (int y = 0; y < 200; y++) {
        float v = (y % P) / static_cast<float>(P) - 0.5f;
        int j = y / P;
        for (int x = 0; x < 200; x++) {
            float u = (x % P) / static_cast<float>(P) - 0.5f;
            int i = x / P;
            int twill = (i - j + 400) % 4;

            float warpProf = cosf(u * kPi);
            float weftProf = cosf(v * kPi);

            float sinU = sinf(u * kPi);
            float sinV = sinf(v * kPi);
            float groove = -0.30f * (sinU * sinU + sinV * sinV);

            float h = 0.0f;
            if (twill != 0) {
                h = 0.55f + 0.35f * warpProf + groove;
            } else {
                h = -0.15f + 0.45f * weftProf + groove;
            }

            // Fine spun-cotton fiber fuzz (single-pixel scale)
            float fuzz = 0.15f * (Hash32(x, y, 777) - 0.5f);
            field[y * 200 + x] = h + fuzz;
        }
    }
}

static void GeneratePaper(std::vector<float>& field) {
    // Organic cotton / parchment paper tooth with dense interwoven pulp fibers (NO floating worms!)
    std::fill(field.begin(), field.end(), 0.0f);
    const float kPi = 3.14159265f;

    // 1. Continuous felt tooth (smooth multi-octave paper pulp)
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) {
            float c1 = Hash32(x / 8, y / 8, 301);
            float c2 = Hash32(x / 4, y / 4, 302);
            float c3 = Hash32(x / 2, y / 2, 303);
            float c4 = Hash32(x, y, 304);
            float pulp = (c1 * 0.35f + c2 * 0.30f + c3 * 0.22f + c4 * 0.13f - 0.50f) * 1.50f;
            field[y * 200 + x] = pulp;
        }
    }

    // 2. Dense interwoven fine pulp fibers (450 tiny overlapping filaments)
    const int numFibers = 450;
    for (int f = 0; f < numFibers; f++) {
        float cx = Hash32(f, 1, 401) * 200.0f;
        float cy = Hash32(f, 2, 402) * 200.0f;
        float flen = 4.0f + 8.0f * Hash32(f, 3, 403); // Short natural 4-12px fibers
        float angle = Hash32(f, 4, 404) * kPi;
        float intensity = (Hash32(f, 5, 405) - 0.45f) * 1.40f;

        int steps = static_cast<int>(flen * 2.0f);
        for (int s = -steps / 2; s <= steps / 2; s++) {
            float t = (static_cast<float>(s) / steps) * flen;
            float fx = cx + t * cosf(angle);
            float fy = cy + t * sinf(angle);
            int ix = ((static_cast<int>(roundf(fx)) % 200) + 200) % 200;
            int iy = ((static_cast<int>(roundf(fy)) % 200) + 200) % 200;
            field[iy * 200 + ix] += intensity * 0.35f;
        }
    }
}

static void GenerateTexture(const std::wstring& material, uint8_t* pixels, int amount = -1, int softness = -1) {
    if (amount < 0) amount = 50;
    if (softness < 0) softness = 0;

    float amountFactor = std::clamp(amount, 0, 100) / 100.0f;
    float softnessFactor = std::clamp(softness, 0, 100) / 100.0f;

    std::vector<float> field(40000, 0.0f);

    if (material == L"grain" || material == L"fine" || material == L"finegrain") {
        GenerateFineGrain(field);
    } else if (material == L"denim" || material == L"denim weave") {
        GenerateDenim(field);
    } else if (material == L"paper" || material == L"paper fiber" || material == L"fiber") {
        GeneratePaper(field);
    } else { // default: sand
        GenerateSand(field);
    }

    // Apply softness smoothing filter
    if (softnessFactor > 0.001f) {
        int passes = 1 + static_cast<int>(softnessFactor * 3.0f);
        std::vector<float> temp(40000);
        for (int p = 0; p < passes; p++) {
            for (int y = 0; y < 200; y++) {
                for (int x = 0; x < 200; x++) {
                    int xm1 = (x - 1 + 200) % 200, xp1 = (x + 1) % 200;
                    temp[y * 200 + x] = 0.25f * field[y * 200 + xm1] + 0.5f * field[y * 200 + x] + 0.25f * field[y * 200 + xp1];
                }
            }
            for (int y = 0; y < 200; y++) {
                int ym1 = (y - 1 + 200) % 200, yp1 = (y + 1) % 200;
                for (int x = 0; x < 200; x++) {
                    field[y * 200 + x] = 0.25f * temp[ym1 * 200 + x] + 0.5f * temp[y * 200 + x] + 0.25f * temp[yp1 * 200 + x];
                }
            }
        }
    }

    double sum = 0.0, square = 0.0;
    for (size_t i = 0; i < 40000; i++) {
        sum += field[i];
        square += field[i] * field[i];
    }
    double mean = sum / 40000.0;
    double deviation = sqrt(fmax(0.0, square / 40000.0 - mean * mean));
    if (deviation < 0.001) deviation = 0.001;

    // Convert normalized height field to tactile embossed relief:
    // Positive ridges catch light (crisp white highlight);
    // Negative crevices fall in shadow (rich dark shading);
    // Flat baseline has Alpha=0 (transparent, so existing space color / mica shines through).
    for (size_t i = 0; i < 40000; i++) {
        double norm = (field[i] - mean) / deviation;
        if (norm > 0.0) {
            double t = fmin(norm / 2.2, 1.0);
            uint8_t a = static_cast<uint8_t>(fmin(255.0, t * 160.0 * amountFactor));
            pixels[i * 4 + 0] = 255; // B (straight BGRA - white highlights)
            pixels[i * 4 + 1] = 255; // G
            pixels[i * 4 + 2] = 255; // R
            pixels[i * 4 + 3] = a;   // A
        } else if (norm < 0.0) {
            double t = fmin((-norm) / 2.2, 1.0);
            uint8_t a = static_cast<uint8_t>(fmin(255.0, t * 130.0 * amountFactor));
            pixels[i * 4 + 0] = 0;   // B (straight BGRA - shadow crevices)
            pixels[i * 4 + 1] = 0;   // G
            pixels[i * 4 + 2] = 0;   // R
            pixels[i * 4 + 3] = a;   // A
        } else {
            pixels[i * 4 + 0] = pixels[i * 4 + 1] = pixels[i * 4 + 2] = pixels[i * 4 + 3] = 0;
        }
    }
}

static bool WriteBmp(const wchar_t* path, const uint8_t* rgba) {
    BITMAPFILEHEADER bfh{};
    bfh.bfType = 0x4D42; // 'BM'
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + 200 * 200 * 4;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = 200;
    bih.biHeight = -200; // Top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = 200 * 200 * 4;

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    WriteFile(file, &bfh, sizeof(bfh), &written, nullptr);
    WriteFile(file, &bih, sizeof(bih), &written, nullptr);

    std::vector<uint8_t> bgra(200 * 200 * 4);
    for (size_t i = 0; i < 40000; i++) {
        bgra[i * 4 + 0] = rgba[i * 4 + 0]; // B
        bgra[i * 4 + 1] = rgba[i * 4 + 1]; // G
        bgra[i * 4 + 2] = rgba[i * 4 + 2]; // R
        bgra[i * 4 + 3] = rgba[i * 4 + 3]; // A
    }

    WriteFile(file, bgra.data(), static_cast<DWORD>(bgra.size()), &written, nullptr);
    CloseHandle(file);
    return true;
}

} // namespace Grain

// ---------------------------------------------------------------- config

// The CLI writes this INI. The values are copied to atomics so the
// PropertyChanged callbacks (which can fire while the timer is mid-apply)
// always see a consistent flag.
struct TweakCfg {
    bool hideToolbar = false;
    bool hideSidebarButton = false;
    bool hideChatButton = false;
    bool showAddressField = false;
    bool hideTrafficLights = false;
    bool hidePersonalizeButton = false;
    bool hideUpgradeBanner = false;
    bool hideTabSearchButton = false;
    bool hideLoadingAnimation = false;
    bool hidePinnedTabBorder = false;
    bool translucentSidebar = false;
    bool showToolbarWithSidebar = false;
    bool showToolbarOnNewTab = false;
    bool cleanSearchBox = false;
    bool newTabPainting = false;
    bool edgeToEdge = false;
    bool revealToolbarOnHover = false;
    bool toolbarColor = false;
    bool peek = false;
    bool profileEmoji = false;
    bool sidebarGrain = false;
    bool hideTabHoverCard = false;
    bool hideLinkAddress = false;
    bool arcPinnedTabs = false;
    bool hideHistorySuggestions = false;
    bool dontPopUpDownloads = false;
    bool squareSidebarCorners = false;
    bool keepWindowsActive = false;
    bool normalLaunch = true;
    bool segoeUiVariable = false;
    bool notoEmoji = false;
    bool developerMode = false;
    bool developerModeLocalhost = true;
    std::wstring profileEmojiText = L"🦊";
    int pinnedExtensions = 0;
    int sidebarGrainAmount = 50;
    int sidebarGrainSoftness = 0;
    int newTabPaintingTint = 67;
    int materialTransparency = 100;
    std::wstring sidebarGrainMaterial = L"sand";
    std::wstring windowMaterial = L"mica";
    std::wstring hideToolbarMode = L"overlay";
    std::wstring preset = L"custom";
    std::wstring peekTrigger = L"shift";
    std::wstring peekAnimation = L"genie";
    std::wstring notoEmojiFont = L"notoColor";
};

static TweakCfg g_cfg;
static std::atomic<bool> g_hideToolbar{false};
static std::atomic<bool> g_hideSidebarButton{false};
static std::atomic<bool> g_hideChatButton{false};
static std::atomic<bool> g_showAddressField{false};
static std::atomic<bool> g_hideTrafficLights{false};
static std::atomic<bool> g_hidePersonalizeButton{false};
static std::atomic<bool> g_hideUpgradeBanner{false};
static std::atomic<bool> g_hideTabSearchButton{false};
static std::atomic<bool> g_hideLoadingAnimation{false};
static std::atomic<bool> g_hidePinnedTabBorder{false};
static std::atomic<bool> g_translucentSidebar{false};
static std::atomic<int> g_materialTransparency{100};
static std::atomic<bool> g_showToolbarWithSidebar{false};
static std::atomic<bool> g_showToolbarOnNewTab{false};
static std::atomic<bool> g_cleanSearchBox{false};
static std::atomic<bool> g_newTabPainting{false};
static std::atomic<int> g_newTabPaintingTint{67};
static std::atomic<bool> g_edgeToEdge{false};
static std::atomic<bool> g_revealToolbarOnHover{false};
static std::atomic<bool> g_toolbarColor{false};
static std::atomic<bool> g_peek{false};
static std::atomic<bool> g_profileEmoji{false};
static std::atomic<bool> g_sidebarGrain{false};
static std::atomic<bool> g_hideTabHoverCard{false};
static std::atomic<bool> g_hideLinkAddress{false};
static std::atomic<bool> g_arcPinnedTabs{false};
static std::atomic<bool> g_hideHistorySuggestions{false};
static std::atomic<bool> g_dontPopUpDownloads{false};
static std::atomic<bool> g_squareSidebarCorners{false};
static std::atomic<bool> g_keepWindowsActive{false};
static std::atomic<bool> g_normalLaunch{false};
static std::atomic<bool> g_segoeUiVariable{false};
static std::atomic<bool> g_notoEmoji{false};

static bool IsNewTab();

static void ReadConfig() {
    static FILETIME s_lastWriteTime{};
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(g_configPath, GetFileExInfoStandard, &fad)) {
        if (s_lastWriteTime.dwLowDateTime != 0 &&
            fad.ftLastWriteTime.dwLowDateTime == s_lastWriteTime.dwLowDateTime &&
            fad.ftLastWriteTime.dwHighDateTime == s_lastWriteTime.dwHighDateTime) {
            return;
        }
        s_lastWriteTime = fad.ftLastWriteTime;
    }

    g_cfg.hideToolbar = GetPrivateProfileIntW(L"tweaks", L"hideToolbar", 0, g_configPath) != 0;
    g_cfg.hideSidebarButton =
        GetPrivateProfileIntW(L"tweaks", L"hideSidebarButton", 0, g_configPath) != 0;
    g_cfg.hideChatButton =
        GetPrivateProfileIntW(L"tweaks", L"hideChatButton", 0, g_configPath) != 0;
    g_cfg.showAddressField =
        GetPrivateProfileIntW(L"tweaks", L"showAddressField", 0, g_configPath) != 0;
    g_cfg.hideTrafficLights =
        GetPrivateProfileIntW(L"tweaks", L"hideTrafficLights", 0, g_configPath) != 0;
    g_cfg.hidePersonalizeButton =
        GetPrivateProfileIntW(L"tweaks", L"hidePersonalizeButton", 0, g_configPath) != 0;
    g_cfg.hideUpgradeBanner =
        GetPrivateProfileIntW(L"tweaks", L"hideUpgradeBanner", 0, g_configPath) != 0;
    g_cfg.hideTabSearchButton =
        GetPrivateProfileIntW(L"tweaks", L"hideTabSearchButton", 0, g_configPath) != 0;
    g_cfg.hideLoadingAnimation =
        GetPrivateProfileIntW(L"tweaks", L"hideLoadingAnimation", 0, g_configPath) != 0;
    g_cfg.hidePinnedTabBorder =
        GetPrivateProfileIntW(L"tweaks", L"hidePinnedTabBorder", 0, g_configPath) != 0;
    g_cfg.translucentSidebar =
        GetPrivateProfileIntW(L"tweaks", L"translucentSidebar", 0, g_configPath) != 0;
    g_cfg.showToolbarWithSidebar =
        GetPrivateProfileIntW(L"tweaks", L"showToolbarWithSidebar", 0, g_configPath) != 0;
    g_cfg.showToolbarOnNewTab =
        GetPrivateProfileIntW(L"tweaks", L"showToolbarOnNewTab", 0, g_configPath) != 0;
    g_cfg.cleanSearchBox =
        GetPrivateProfileIntW(L"tweaks", L"cleanSearchBox", 0, g_configPath) != 0;
    g_cfg.newTabPainting =
        GetPrivateProfileIntW(L"tweaks", L"newTabPainting", 0, g_configPath) != 0;
    g_cfg.edgeToEdge =
        GetPrivateProfileIntW(L"tweaks", L"edgeToEdge", 0, g_configPath) != 0;
    g_cfg.revealToolbarOnHover =
        GetPrivateProfileIntW(L"tweaks", L"revealToolbarOnHover", 0, g_configPath) != 0;
    g_cfg.toolbarColor =
        GetPrivateProfileIntW(L"tweaks", L"toolbarColor", 0, g_configPath) != 0;
    g_cfg.peek =
        GetPrivateProfileIntW(L"tweaks", L"peek", 0, g_configPath) != 0;
    g_cfg.profileEmoji =
        GetPrivateProfileIntW(L"tweaks", L"profileEmoji", 0, g_configPath) != 0;
    g_cfg.sidebarGrain =
        GetPrivateProfileIntW(L"tweaks", L"sidebarGrain", 0, g_configPath) != 0;
    g_cfg.hideTabHoverCard =
        GetPrivateProfileIntW(L"tweaks", L"hideTabHoverCard", 0, g_configPath) != 0;
    g_cfg.hideLinkAddress =
        GetPrivateProfileIntW(L"tweaks", L"hideLinkAddress", 0, g_configPath) != 0;
    g_cfg.arcPinnedTabs =
        GetPrivateProfileIntW(L"tweaks", L"arcPinnedTabs", 0, g_configPath) != 0;
    g_cfg.hideHistorySuggestions =
        GetPrivateProfileIntW(L"tweaks", L"hideHistorySuggestions", 0, g_configPath) != 0;
    g_cfg.dontPopUpDownloads =
        GetPrivateProfileIntW(L"tweaks", L"dontPopUpDownloads", 0, g_configPath) != 0;
    g_cfg.squareSidebarCorners =
        GetPrivateProfileIntW(L"tweaks", L"squareSidebarCorners", 0, g_configPath) != 0;
    g_cfg.keepWindowsActive =
        GetPrivateProfileIntW(L"tweaks", L"keepWindowsActive", 0, g_configPath) != 0;
    g_cfg.normalLaunch =
        GetPrivateProfileIntW(L"tweaks", L"normalLaunch", 1, g_configPath) != 0;
    g_cfg.segoeUiVariable =
        GetPrivateProfileIntW(L"tweaks", L"segoeUiVariable", 0, g_configPath) != 0;
    g_cfg.notoEmoji =
        GetPrivateProfileIntW(L"tweaks", L"notoEmoji", 0, g_configPath) != 0;
    g_cfg.developerMode =
        GetPrivateProfileIntW(L"tweaks", L"developerMode", 0, g_configPath) != 0;
    g_cfg.developerModeLocalhost =
        GetPrivateProfileIntW(L"tweaks", L"developerModeLocalhost", 1, g_configPath) != 0;

    wchar_t emojiBuf[64]{};
    GetPrivateProfileStringW(L"tweaks", L"profileEmojiText", L"🦊", emojiBuf, _countof(emojiBuf), g_configPath);
    g_cfg.profileEmojiText = emojiBuf;

    wchar_t notoFontBuf[64]{};
    GetPrivateProfileStringW(L"tweaks", L"notoEmojiFont", L"notoColor", notoFontBuf, _countof(notoFontBuf), g_configPath);
    g_cfg.notoEmojiFont = notoFontBuf;

    wchar_t presetBuf[64]{};
    GetPrivateProfileStringW(L"tweaks", L"preset", L"custom", presetBuf, _countof(presetBuf), g_configPath);
    g_cfg.preset = presetBuf;

    wchar_t grainMat[64]{};
    GetPrivateProfileStringW(L"tweaks", L"sidebarGrainMaterial", L"sand", grainMat, _countof(grainMat), g_configPath);
    g_cfg.sidebarGrainMaterial = grainMat;

    wchar_t winMat[64]{};
    GetPrivateProfileStringW(L"tweaks", L"windowMaterial", L"mica", winMat, _countof(winMat), g_configPath);
    g_cfg.windowMaterial = winMat;

    wchar_t tbMode[32]{};
    GetPrivateProfileStringW(L"tweaks", L"hideToolbarMode", L"overlay", tbMode, _countof(tbMode), g_configPath);
    g_cfg.hideToolbarMode = tbMode;

    wchar_t ptBuf[64]{};
    GetPrivateProfileStringW(L"tweaks", L"peekTrigger", L"shift", ptBuf, _countof(ptBuf), g_configPath);
    g_cfg.peekTrigger = ptBuf;

    wchar_t paBuf[64]{};
    GetPrivateProfileStringW(L"tweaks", L"peekAnimation", L"genie", paBuf, _countof(paBuf), g_configPath);
    g_cfg.peekAnimation = paBuf;

    g_cfg.pinnedExtensions =
        GetPrivateProfileIntW(L"tweaks", L"pinnedExtensions", 0, g_configPath);
    if (g_cfg.pinnedExtensions > 4) {
        static bool s_patchedPinned = false;
        if (!s_patchedPinned) {
            s_patchedPinned = true;
            ApplyPinnedExtensionsLimit(g_cfg.pinnedExtensions);
        }
    }

    g_cfg.sidebarGrainAmount =
        GetPrivateProfileIntW(L"tweaks", L"sidebarGrainAmount", 50, g_configPath);
    g_cfg.sidebarGrainSoftness =
        GetPrivateProfileIntW(L"tweaks", L"sidebarGrainSoftness", 0, g_configPath);
    g_cfg.newTabPaintingTint =
        GetPrivateProfileIntW(L"tweaks", L"newTabPaintingTint", 67, g_configPath);
    if (g_cfg.newTabPaintingTint < 0) g_cfg.newTabPaintingTint = 0;
    if (g_cfg.newTabPaintingTint > 100) g_cfg.newTabPaintingTint = 100;
    g_newTabPaintingTint.store(g_cfg.newTabPaintingTint);

    g_cfg.materialTransparency =
        GetPrivateProfileIntW(L"tweaks", L"materialTransparency", 100, g_configPath);
    if (g_cfg.materialTransparency < 0) g_cfg.materialTransparency = 0;
    if (g_cfg.materialTransparency > 100) g_cfg.materialTransparency = 100;

    if (g_cfg.sidebarGrain) {
        static std::wstring s_lastGrainMat;
        static int s_lastGrainAmount = -1;
        static int s_lastGrainSoftness = -1;
        if (s_lastGrainMat != g_cfg.sidebarGrainMaterial || s_lastGrainAmount != g_cfg.sidebarGrainAmount || s_lastGrainSoftness != g_cfg.sidebarGrainSoftness) {
            s_lastGrainMat = g_cfg.sidebarGrainMaterial;
            s_lastGrainAmount = g_cfg.sidebarGrainAmount;
            s_lastGrainSoftness = g_cfg.sidebarGrainSoftness;
            wchar_t bmpPath[MAX_PATH]{};
            swprintf_s(bmpPath, _countof(bmpPath), L"%s\\sidebar-grain.bmp", g_dir);
            std::vector<uint8_t> pixels(200 * 200 * 4);
            Grain::GenerateTexture(g_cfg.sidebarGrainMaterial, pixels.data(), g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness);
            Grain::WriteBmp(bmpPath, pixels.data());
            Log(L"sidebar grain: generated %s pattern (amount=%d, softness=%d) at %s",
                g_cfg.sidebarGrainMaterial.c_str(), g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness, bmpPath);
        }
    }

    g_hideToolbar.store(g_cfg.hideToolbar);
    g_hideSidebarButton.store(g_cfg.hideSidebarButton);
    g_hideChatButton.store(g_cfg.hideChatButton);
    g_showAddressField.store(g_cfg.showAddressField);
    g_hideTrafficLights.store(g_cfg.hideTrafficLights);
    g_hidePersonalizeButton.store(g_cfg.hidePersonalizeButton);
    g_hideUpgradeBanner.store(g_cfg.hideUpgradeBanner);
    g_hideTabSearchButton.store(g_cfg.hideTabSearchButton);
    g_hideLoadingAnimation.store(g_cfg.hideLoadingAnimation);
    g_hidePinnedTabBorder.store(g_cfg.hidePinnedTabBorder);
    g_translucentSidebar.store(g_cfg.translucentSidebar);
    g_materialTransparency.store(g_cfg.materialTransparency);
    g_showToolbarWithSidebar.store(g_cfg.showToolbarWithSidebar);
    g_showToolbarOnNewTab.store(g_cfg.showToolbarOnNewTab);
    g_cleanSearchBox.store(g_cfg.cleanSearchBox);
    g_newTabPainting.store(g_cfg.newTabPainting);
    g_edgeToEdge.store(g_cfg.edgeToEdge);
    g_revealToolbarOnHover.store(g_cfg.revealToolbarOnHover);
    g_toolbarColor.store(g_cfg.toolbarColor);
    g_peek.store(g_cfg.peek);
    g_profileEmoji.store(g_cfg.profileEmoji);
    g_sidebarGrain.store(g_cfg.sidebarGrain);
    g_hideTabHoverCard.store(g_cfg.hideTabHoverCard);
    g_hideLinkAddress.store(g_cfg.hideLinkAddress);
    g_arcPinnedTabs.store(g_cfg.arcPinnedTabs);
    g_hideHistorySuggestions.store(g_cfg.hideHistorySuggestions);
    g_dontPopUpDownloads.store(g_cfg.dontPopUpDownloads);
    g_squareSidebarCorners.store(g_cfg.squareSidebarCorners);
    g_keepWindowsActive.store(g_cfg.keepWindowsActive);
    g_normalLaunch.store(g_cfg.normalLaunch);
    g_segoeUiVariable.store(g_cfg.segoeUiVariable);
    g_notoEmoji.store(g_cfg.notoEmoji);

    // The developer mode tweak needs to know when it is switched on or off.
    DevMode::SetEnabled(g_cfg.developerMode);
    DevMode::SetLocalhost(g_cfg.developerModeLocalhost);
}

static int CfgStamp() {
    return (g_cfg.hideToolbar ? 1 : 0) | (g_cfg.hideSidebarButton ? 2 : 0) |
           (g_cfg.hideChatButton ? 4 : 0) | (g_cfg.showAddressField ? 8 : 0) |
           (g_cfg.hideTrafficLights ? 16 : 0) | (g_cfg.hidePersonalizeButton ? 32 : 0) |
           (g_cfg.hideUpgradeBanner ? 64 : 0) | (g_cfg.hideTabSearchButton ? 128 : 0) |
           (g_cfg.hideLoadingAnimation ? 256 : 0) | (g_cfg.hidePinnedTabBorder ? 512 : 0) |
           (g_cfg.translucentSidebar ? 1024 : 0) | (g_cfg.showToolbarWithSidebar ? 2048 : 0) |
           (g_cfg.showToolbarOnNewTab ? 4096 : 0) | (g_cfg.cleanSearchBox ? 8192 : 0) |
           (g_cfg.edgeToEdge ? 16384 : 0) | (g_cfg.revealToolbarOnHover ? 32768 : 0) |
           (g_cfg.toolbarColor ? 65536 : 0) | (g_cfg.peek ? 131072 : 0) |
           (g_cfg.sidebarGrain ? 262144 : 0) | (g_cfg.hideTabHoverCard ? 524288 : 0) |
           (g_cfg.hideLinkAddress ? 1048576 : 0) | (g_cfg.dontPopUpDownloads ? 2097152 : 0) |
           (g_cfg.squareSidebarCorners ? 4194304 : 0) | (g_cfg.keepWindowsActive ? 8388608 : 0) |
           (g_cfg.newTabPainting ? 16777216 : 0) | (g_cfg.segoeUiVariable ? 33554432 : 0) |
           (g_cfg.notoEmoji ? 67108864 : 0) | (g_cfg.hideHistorySuggestions ? 134217728 : 0) |
            (g_cfg.arcPinnedTabs ? 268435456 : 0);
}

// ---------------------------------------------------------------- tree model

struct Node {
    InstanceHandle parent = 0;
    unsigned childIndex = 0;
    std::wstring type;
    std::wstring name;
    DWORD thread = 0;
    std::vector<InstanceHandle> children;
};

static std::recursive_mutex g_mx;
static std::unordered_map<InstanceHandle, Node> g_nodes;
static std::atomic<bool> g_treeDirty{true};
static std::vector<InstanceHandle> g_roots;
static std::vector<DWORD> g_uiThreads;

static IXamlDiagnostics* g_diag = nullptr;
static IVisualTreeService2* g_vts = nullptr;

// ---------------------------------------------------------------- property overrides

// WinUI hands out handles to real values through IVisualTreeService. A value
// handle belongs to the XAML island it was created in, so one is kept per
// element and re-created if Dia rebuilt the island.
struct AppliedProp {
    unsigned index = 0;
    InstanceHandle value = 0;
    bool hadLocal = false;
    std::wstring origType;
    std::wstring origText;
    InstanceHandle origHandle = 0;
    bool logged = false;
};

// element -> (property index -> applied override)
static std::unordered_map<InstanceHandle, std::map<unsigned, AppliedProp>> g_applied;

static InstanceHandle MakeValue(const wchar_t* type, const wchar_t* value) {
    if (!g_vts) return 0;
    BSTR typeName = SysAllocString(type);
    BSTR text = SysAllocString(value);
    InstanceHandle out = 0;
    HRESULT hr = g_vts->CreateInstance(typeName, text, &out);
    SysFreeString(typeName);
    SysFreeString(text);
    if (FAILED(hr)) {
        Log(L"CreateInstance(%s, %s) failed 0x%08X", type, value, static_cast<unsigned>(hr));
        return 0;
    }
    return out;
}

// Resolves a property by name from the element's property chain. GetPropertyIndex
// rejects plain names on this WinUI build, but the chain carries the index.
static bool LookupProp(InstanceHandle handle, const std::wstring& prop, unsigned& index,
                       std::wstring& valueType, bool& hasLocal,
                       std::wstring* localType = nullptr, std::wstring* localText = nullptr,
                       std::wstring* effectiveText = nullptr) {
    hasLocal = false;
    unsigned sourceCount = 0;
    unsigned propCount = 0;
    PropertyChainSource* sources = nullptr;
    PropertyChainValue* values = nullptr;
    bool found = false;

    HRESULT hr = g_vts ? g_vts->GetPropertyValuesChain(handle, &sourceCount, &sources,
                                                       &propCount, &values)
                       : E_FAIL;
    if (SUCCEEDED(hr)) {
        for (unsigned i = 0; i < propCount; ++i) {
            if (Bstr(values[i].PropertyName) != prop) continue;
            if (!found) {
                index = values[i].Index;
                found = true;
            }
            if (valueType.empty()) valueType = Bstr(values[i].ValueType);
            if (effectiveText && effectiveText->empty() && !values[i].Overridden) {
                *effectiveText = Bstr(values[i].Value);
            }
            if (!values[i].Overridden && values[i].PropertyChainIndex < sourceCount &&
                sources[values[i].PropertyChainIndex].Source == BaseValueSourceLocal) {
                hasLocal = true;
                if (localType) *localType = Bstr(values[i].ValueType);
                if (localText) *localText = Bstr(values[i].Value);
            }
        }
        for (unsigned i = 0; i < sourceCount; ++i) {
            SysFreeString(sources[i].TargetType);
            SysFreeString(sources[i].Name);
            SysFreeString(sources[i].SrcInfo.FileName);
            SysFreeString(sources[i].SrcInfo.Hash);
        }
        for (unsigned i = 0; i < propCount; ++i) {
            SysFreeString(values[i].Type);
            SysFreeString(values[i].DeclaringType);
            SysFreeString(values[i].ValueType);
            SysFreeString(values[i].ItemType);
            SysFreeString(values[i].Value);
            SysFreeString(values[i].PropertyName);
        }
        CoTaskMemFree(sources);
        CoTaskMemFree(values);
    }
    return found;
}

static std::wstring EnumText(const std::wstring& type, const std::wstring& text) {
    if (type.find(L"Visibility") != std::wstring::npos) {
        if (text == L"1") return L"Collapsed";
        if (text == L"2") return L"Hidden";
        return L"Visible";
    }
    return text;
}

static std::wstring PropText(InstanceHandle handle, const wchar_t* property) {
    unsigned index = 0;
    bool hasLocal = false;
    std::wstring valueType;
    std::wstring effective;
    if (!LookupProp(handle, property, index, valueType, hasLocal, nullptr, nullptr, &effective))
        return L"?";
    return EnumText(valueType, effective);
}

static std::wstring NodeName(InstanceHandle handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    auto it = g_nodes.find(handle);
    return it == g_nodes.end() ? std::wstring() : it->second.name;
}

static void RestoreOne(InstanceHandle handle, unsigned index, const AppliedProp& applied) {
    HRESULT hr = E_FAIL;
    if (applied.hadLocal && !applied.origType.empty() && !applied.origText.empty() &&
        applied.origType.find(L"Binding") == std::wstring::npos) {
        InstanceHandle value = MakeValue(applied.origType.c_str(),
                                         EnumText(applied.origType, applied.origText).c_str());
        if (value) hr = g_vts->SetProperty(handle, value, index);
    }
    if (FAILED(hr) && applied.hadLocal && applied.origHandle) {
        hr = g_vts->SetProperty(handle, applied.origHandle, index);
    }
    if (FAILED(hr)) g_vts->ClearProperty(handle, index);
}

// Applies or restores a property on an element. When the tweak is off the
// element's original local value (or its binding/style, via ClearProperty) is
// put back.
static void ApplyProperty(InstanceHandle handle, const wchar_t* propName,
                          const wchar_t* propType, const wchar_t* propVal, bool on) {
    if (!g_vts) return;

    unsigned index = 0;
    bool hasLocal = false;
    std::wstring valueType;
    std::wstring localType;
    std::wstring localText;
    std::wstring currentText;
    if (!LookupProp(handle, propName, index, valueType, hasLocal, &localType, &localText,
                    &currentText))
        return;

    if (!on) {
        AppliedProp saved;
        bool hadSaved = false;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            auto it = g_applied.find(handle);
            if (it != g_applied.end()) {
                auto entry = it->second.find(index);
                if (entry != it->second.end()) {
                    saved = std::move(entry->second);
                    it->second.erase(entry);
                    hadSaved = true;
                }
            }
        }
        if (hadSaved) {
            RestoreOne(handle, index, saved);
            Log(L"      >>> RESTORE '%s' property %s", NodeName(handle).c_str(), propName);
        }
        return;
    }

    if (currentText == propVal) return;
    if (wcscmp(propName, L"Visibility") == 0) {
        if (currentText == L"1" || currentText == L"Collapsed") return;
    }

    InstanceHandle value = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        auto& perElement = g_applied[handle];
        auto existing = perElement.find(index);
        if (existing == perElement.end()) {
            AppliedProp applied;
            applied.index = index;
            applied.hadLocal = hasLocal;
            applied.origType = localType;
            applied.origText = localText;
            if (hasLocal) g_vts->GetProperty(handle, index, &applied.origHandle);
            applied.value = MakeValue(propType, propVal);
            existing = perElement.emplace(index, std::move(applied)).first;
        }
        if (!existing->second.value)
            existing->second.value = MakeValue(propType, propVal);
        value = existing->second.value;
    }
    if (!value) return;

    HRESULT hr = g_vts->SetProperty(handle, value, index);
    if (hr == E_NOTFOUND) {
        InstanceHandle fresh = MakeValue(propType, propVal);
        if (fresh) {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mx);
                auto it = g_applied.find(handle);
                if (it != g_applied.end()) {
                    auto entry = it->second.find(index);
                    if (entry != it->second.end()) entry->second.value = fresh;
                }
            }
            hr = g_vts->SetProperty(handle, fresh, index);
        }
    }
    if (FAILED(hr)) {
        Log(L"SetProperty %s failed 0x%08X on '%s'", propName, static_cast<unsigned>(hr),
            NodeName(handle).c_str());
        return;
    }

    bool logApply = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        auto it = g_applied.find(handle);
        if (it != g_applied.end()) {
            auto entry = it->second.find(index);
            if (entry != it->second.end() && !entry->second.logged) {
                entry->second.logged = true;
                logApply = true;
            }
        }
    }
    if (logApply) Log(L"      >>> APPLY %s=%s on '%s'", propName, propVal, NodeName(handle).c_str());
}

static void ApplyCollapsed(InstanceHandle handle, bool on) {
    ApplyProperty(handle, L"Visibility", L"Microsoft.UI.Xaml.Visibility", L"Collapsed", on);
}

static void ApplyMargin(InstanceHandle handle, const wchar_t* marginStr, bool on) {
    ApplyProperty(handle, L"Margin", L"Microsoft.UI.Xaml.Thickness", marginStr, on);
}

static void ApplyBorderThickness(InstanceHandle handle, const wchar_t* thicknessStr, bool on) {
    ApplyProperty(handle, L"BorderThickness", L"Microsoft.UI.Xaml.Thickness", thicknessStr, on);
}

static void ApplyOpacity(InstanceHandle handle, const wchar_t* opacityStr, bool on) {
    ApplyProperty(handle, L"Opacity", L"Double", opacityStr, on);
}

static InstanceHandle GetParentHandle(InstanceHandle handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    auto it = g_nodes.find(handle);
    return it == g_nodes.end() ? 0 : it->second.parent;
}

static std::wstring GetNodeName(InstanceHandle handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    auto it = g_nodes.find(handle);
    return it == g_nodes.end() ? std::wstring() : it->second.name;
}

static std::wstring GetNodeType(InstanceHandle handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    auto it = g_nodes.find(handle);
    return it == g_nodes.end() ? std::wstring() : it->second.type;
}

static InstanceHandle GetAncestorHandle(InstanceHandle handle, int levelsUp) {
    InstanceHandle cur = handle;
    for (int i = 0; i < levelsUp && cur != 0; i++) {
        cur = GetParentHandle(cur);
    }
    return cur;
}

static bool HasAncestorNamed(InstanceHandle handle, const std::wstring& name) {
    InstanceHandle cur = GetParentHandle(handle);
    while (cur != 0) {
        if (GetNodeName(cur) == name) return true;
        cur = GetParentHandle(cur);
    }
    return false;
}

// ---------------------------------------------------------------- address field

// showAddressField has two parts, exactly as Dia exposes them:
//   AssistantBar          Border that owns the pill's background and radius
//   UrlBarTrailingStack   the panel holding the bookmark/site buttons, whose
//                         Opacity Dia binds to pointer-over
// Both are captured before changing anything so switching the tweak off puts
// Dia's own brush, radius, opacity and opacity binding back.
struct FieldState {
    winrt::weak_ref<muxc::Border> bar{nullptr};
    muxm::Brush savedBackground{nullptr};
    mux::CornerRadius savedRadius{};
    int64_t backgroundToken = 0;
    DWORD thread = 0;
};

struct StackState {
    winrt::weak_ref<mux::FrameworkElement> stack{nullptr};
    muxd::BindingBase savedBinding{nullptr};
    double savedOpacity = 1.0;
    DWORD thread = 0;
};

static std::mutex g_stateLock;
static std::vector<FieldState> g_fields;
static std::vector<StackState> g_stacks;

static muxm::Brush FillBrush() {
    try {
        auto resources = mux::Application::Current().Resources();
        if (auto value = resources.TryLookup(winrt::box_value(L"ControlFillColorDefaultBrush"))) {
            if (auto brush = value.try_as<muxm::Brush>()) return brush;
        }
    } catch (...) {
    }
    // 6% white: Dia's ControlFillColorDefaultBrush in dark themes.
    return muxm::SolidColorBrush(winrt::Windows::UI::Color{0x0f, 0xff, 0xff, 0xff});
}

static bool SameColor(muxm::Brush const& left, muxm::Brush const& right) {
    auto a = left ? left.try_as<muxm::SolidColorBrush>() : nullptr;
    auto b = right ? right.try_as<muxm::SolidColorBrush>() : nullptr;
    if (!a || !b) return left == right;
    return a.Color() == b.Color();
}

static bool GetObject(InstanceHandle handle, Inspectable& out) {
    if (!g_diag) return false;
    IInspectable* raw = nullptr;
    HRESULT hr = g_diag->GetIInspectableFromHandle(handle, &raw);
    if (FAILED(hr) || !raw) return false;
    winrt::copy_from_abi(out, raw);
    raw->Release();
    return true;
}

static void UpdateField(InstanceHandle handle) {
    Inspectable object{nullptr};
    if (!GetObject(handle, object)) return;
    auto bar = object.try_as<muxc::Border>();
    if (!bar) return;

    bool on = g_showAddressField.load() && !g_hideToolbar.load();

    std::lock_guard<std::mutex> lock(g_stateLock);
    size_t found = g_fields.size();
    for (size_t i = 0; i < g_fields.size(); ++i) {
        if (g_fields[i].bar.get() == bar) {
            found = i;
            break;
        }
    }

    if (!on) {
        if (found == g_fields.size()) return;
        FieldState state = std::move(g_fields[found]);
        g_fields.erase(g_fields.begin() + static_cast<std::ptrdiff_t>(found));
        if (state.backgroundToken)
            bar.UnregisterPropertyChangedCallback(muxc::Border::BackgroundProperty(),
                                                  state.backgroundToken);
        bar.Background(state.savedBackground);
        bar.CornerRadius(state.savedRadius);
        Log(L"      >>> RESTORE 'AssistantBar' background");
        return;
    }

    if (found == g_fields.size()) {
        FieldState state;
        state.bar = winrt::make_weak(bar);
        state.savedBackground = bar.Background();
        state.savedRadius = bar.CornerRadius();
        state.thread = GetCurrentThreadId();
        state.backgroundToken = bar.RegisterPropertyChangedCallback(
            muxc::Border::BackgroundProperty(),
            [](mux::DependencyObject const& changed, mux::DependencyProperty const&) {
                if (!g_showAddressField.load() || g_hideToolbar.load()) return;
                auto border = changed.try_as<muxc::Border>();
                if (!border) return;
                auto want = FillBrush();
                if (!SameColor(border.Background(), want)) border.Background(want);
            });
        g_fields.push_back(std::move(state));
        found = g_fields.size() - 1;
    }

    FieldState& state = g_fields[found];
    auto fill = FillBrush();
    if (!SameColor(bar.Background(), fill)) {
        bar.Background(fill);
        Log(L"      >>> PAINT 'AssistantBar' background");
    }
    if (bar.CornerRadius().TopLeft < 1) bar.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
}

static void UpdateTrailingStack(InstanceHandle handle) {
    if (IsNewTab()) return;
    Inspectable object{nullptr};
    if (!GetObject(handle, object)) return;
    auto element = object.try_as<mux::FrameworkElement>();
    if (!element) return;

    // The developer toolbar lives in this trailing stack. Keep it opaque while
    // that toolbar is present, even if the page tracker briefly disagrees about
    // the active tab - otherwise Dia's hover-only opacity binding is restored
    // and the developer icons seem to appear only while hovering the toolbar.
    // While the Developer Mode tweak is on, keep this stack opaque. Dia's
    // default hides these trailing buttons until the toolbar is hovered; that
    // would hide our dev icons and striped border the moment the pointer left.
    // Basing this on the tweak (not the flickering per-tab page detection) makes
    // the toolbar independent of Dia's hover state. UpdateTrailingStack returns
    // early on the New Tab page, so that page keeps Dia's own behaviour.
    bool devModeActive = DevMode::Enabled();
    bool on = (g_showAddressField.load() && !g_hideToolbar.load()) || devModeActive;

    std::lock_guard<std::mutex> lock(g_stateLock);
    size_t found = g_stacks.size();
    for (size_t i = 0; i < g_stacks.size(); ++i) {
        if (g_stacks[i].stack.get() == element) {
            found = i;
            break;
        }
    }

    if (!on) {
        if (found == g_stacks.size()) return;
        StackState state = std::move(g_stacks[found]);
        g_stacks.erase(g_stacks.begin() + static_cast<std::ptrdiff_t>(found));
        if (state.savedBinding) {
            element.SetBinding(mux::UIElement::OpacityProperty(), state.savedBinding);
        } else {
            element.Opacity(state.savedOpacity);
        }
        Log(L"      >>> RESTORE 'UrlBarTrailingStack' opacity");
        return;
    }

    if (found == g_stacks.size()) {
        StackState state;
        state.stack = winrt::make_weak(element);
        state.savedOpacity = element.Opacity();
        if (auto expression = element.GetBindingExpression(mux::UIElement::OpacityProperty()))
            state.savedBinding = expression.ParentBinding();
        state.thread = GetCurrentThreadId();
        g_stacks.push_back(std::move(state));
        found = g_stacks.size() - 1;
    }

    if (element.Visibility() != mux::Visibility::Visible) {
        element.Visibility(mux::Visibility::Visible);
    }
    if (element.Opacity() != 1.0) element.Opacity(1.0);
}

// ---------------------------------------------------------------- applying

static bool IsSidebarOpen() {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    for (const auto& [handle, node] : g_nodes) {
        if (node.name == L"SidebarBorder") {
            Inspectable obj{nullptr};
            if (GetObject(handle, obj)) {
                if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                    if (fe.ActualWidth() >= 1 && fe.Visibility() == mux::Visibility::Visible) {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static bool ShownOnScreen(mux::FrameworkElement fe) {
    for (mux::DependencyObject d = fe; d; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto u = d.try_as<mux::UIElement>(); u && u.Visibility() != mux::Visibility::Visible) return false;
    return true;
}

static thread_local winrt::weak_ref<muxc::Grid> t_activeNewTabPage;
static thread_local bool t_isNewTabCached = false;
static thread_local uint64_t t_lastNewTabCheckTick = 0;

static void InvalidateNewTabCache() {
    t_lastNewTabCheckTick = 0;
}

static bool CheckIsNewTabInternal() {
    // 1. Fast check: is the cached newTabPage still alive and on screen?
    if (auto grid = t_activeNewTabPage.get()) {
        try {
            if (grid.Visibility() == mux::Visibility::Visible &&
                grid.ActualWidth() >= 1 &&
                ShownOnScreen(grid)) {
                return true;
            }
            // Alive but hidden: the user is on a website tab.
            return false;
        } catch (...) {
            t_activeNewTabPage = nullptr;
        }
    }

    // 2. Scan the live tree for the start page (only after it changes).
    static uint64_t s_lastFullScanTick = 0;
    uint64_t now = GetTickCount64();
    if (!g_treeDirty.load() && (now - s_lastFullScanTick < 2000)) return false;
    s_lastFullScanTick = now;

    std::lock_guard<std::recursive_mutex> lock(g_mx);
    for (const auto& [handle, node] : g_nodes) {
        if (node.type != L"Microsoft.UI.Xaml.Controls.Grid") continue;
        Inspectable obj{nullptr};
        if (!GetObject(handle, obj)) continue;
        if (auto fe = obj.try_as<mux::FrameworkElement>()) {
            try {
                if (std::wstring(muxa::AutomationProperties::GetName(fe)) == L"newTabPage") {
                    if (auto g = fe.try_as<muxc::Grid>()) t_activeNewTabPage = winrt::make_weak(g);
                    if (fe.ActualWidth() >= 1 && fe.Visibility() == mux::Visibility::Visible &&
                        ShownOnScreen(fe)) {
                        return true;
                    }
                }
            } catch (...) {}
        }
    }
    return false;
}

static bool IsNewTab() {
    uint64_t now = GetTickCount64();
    if (now - t_lastNewTabCheckTick < 100) {
        return t_isNewTabCached;
    }
    t_lastNewTabCheckTick = now;
    t_isNewTabCached = CheckIsNewTabInternal();
    return t_isNewTabCached;
}

bool IsDiaNewTab() {
    return IsNewTab();
}

static bool PointerAtTop(DWORD uiThread, bool shown) {
    POINT pt;
    if (!GetCursorPos(&pt)) return false;
    HWND w = WindowFromPoint(pt);
    if (!w) return false;
    HWND root = GetAncestor(w, GA_ROOT);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(root, &pid);
    if (pid != GetCurrentProcessId()) return false;
    wchar_t cls[64];
    if (!GetClassNameW(root, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0)
        return shown;
    if (tid != uiThread) return false;
    POINT origin{0, 0};
    ClientToScreen(root, &origin);
    UINT dpi = GetDpiForWindow(root);
    if (!dpi) dpi = 96;
    int y = pt.y - origin.y;
    int showBand = MulDiv(6, dpi, 96), keepBand = MulDiv(52, dpi, 96);
    return y >= 0 && y < (shown ? keepBand : showBand);
}

static std::atomic<bool> g_watchHover{false};
static std::atomic<DWORD> g_hoverThread{0};
static std::atomic<HWND> g_pingWnd{nullptr};
static std::atomic<bool> g_hoverTop{false};
static const UINT WM_DT_HOVER = WM_APP + 0x3a1;

static void UpdateHiddenToolbar();

static DWORD WINAPI HoverWatcher(LPVOID) {
    bool last = false;
    for (;;) {
        if (!g_watchHover.load() || !g_pingWnd.load() || !g_hideToolbar.load() || !g_revealToolbarOnHover.load()) {
            if (last) {
                last = false;
                PostMessageW(g_pingWnd.load(), WM_DT_HOVER, 0, 0);
            }
            Sleep(200);
            continue;
        }
        bool now = PointerAtTop(g_hoverThread.load(), last);
        if (now != last) {
            last = now;
            PostMessageW(g_pingWnd.load(), WM_DT_HOVER, now ? 1 : 0, 0);
        }
        Sleep(25);
    }
}

static LRESULT CALLBACK PingProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DT_HOVER) {
        bool hover = (wp != 0);
        if (hover != g_hoverTop.load()) {
            g_hoverTop.store(hover);
            try {
                UpdateHiddenToolbar();
            } catch (...) {}
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool IsCursorAtTopEdge() {
    return g_hoverTop.load();
}

static std::vector<winrt::weak_ref<muxc::FlyoutPresenter>> g_hiddenHoverCards;

static void UpdateTabHoverCard(InstanceHandle handle) {
    if (!g_hideTabHoverCard.load()) return;
    Inspectable obj{nullptr};
    if (!GetObject(handle, obj)) return;
    auto fe = obj.try_as<mux::FrameworkElement>();
    if (!fe) return;

    for (auto p = muxm::VisualTreeHelper::GetParent(fe); p; p = muxm::VisualTreeHelper::GetParent(p)) {
        if (auto fp = p.try_as<muxc::FlyoutPresenter>()) {
            if (fp.Visibility() != mux::Visibility::Collapsed) {
                fp.Visibility(mux::Visibility::Collapsed);
                std::lock_guard<std::mutex> lock(g_stateLock);
                g_hiddenHoverCards.push_back(winrt::make_weak(fp));
            }
            break;
        }
    }
}

template <class T>
static T Obj(InstanceHandle h) {
    Inspectable obj{nullptr};
    if (!GetObject(h, obj)) return nullptr;
    return obj.try_as<T>();
}

static std::vector<InstanceHandle> FindNodes(const std::wstring& name) {
    std::vector<InstanceHandle> result;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const auto& [handle, node] : g_nodes) {
        if (node.name == name) {
            result.push_back(handle);
        }
    }
    return result;
}

template <class F>
static mux::DependencyObject FindVisual(mux::DependencyObject root, F pred, int depth = 0) {
    if (!root || depth > 60) return nullptr;
    if (pred(root)) return root;
    int n = muxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < n; i++) {
        if (auto r = FindVisual(muxm::VisualTreeHelper::GetChild(root, i), pred, depth + 1)) return r;
    }
    return nullptr;
}

static HWND TopWindowFor(const mux::XamlRoot& root) {
    if (!root) return nullptr;
    try {
        if (auto env = root.ContentIslandEnvironment()) {
            uint64_t val = env.AppWindowId().Value;
            if (val) {
                HWND h = reinterpret_cast<HWND>(static_cast<uintptr_t>(val));
                if (IsWindow(h)) return GetAncestor(h, GA_ROOT);
            }
        }
    } catch (...) {}

    struct Ctx { double w, h; HWND best = nullptr; double bestDiff = 1e9; } ctx;
    auto sz = root.Size();
    double scale = root.RasterizationScale();
    ctx.w = sz.Width * scale;
    ctx.h = sz.Height * scale;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        RECT r;
        if (GetClientRect(h, &r)) {
            double diff = fabs((r.right - r.left) - c.w) + fabs((r.bottom - r.top) - c.h);
            if (diff < c.bestDiff) {
                c.bestDiff = diff;
                c.best = GetAncestor(h, GA_ROOT);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

// ---- engine.h implementations
void Engine_Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[2048]{};
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    wchar_t wbuf[2048]{};
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, _countof(wbuf));
    Log(L"%s", wbuf);
}

std::wstring Engine_Dir() {
    return g_dir;
}

bool Engine_TweakOn(const std::wstring& key) {
    if (key == L"peek") return g_peek.load();
    if (key == L"hideToolbar") return g_hideToolbar.load();
    if (key == L"sidebarGrain") return g_sidebarGrain.load();
    if (key == L"cleanSearchBox") return g_cleanSearchBox.load();
    if (key == L"newTabPainting") return g_newTabPainting.load();
    if (key == L"squareCorners" || key == L"squareSidebarCorners") return g_squareSidebarCorners.load();
    if (key == L"windowMaterial") return g_cfg.windowMaterial != L"none" && !g_cfg.windowMaterial.empty() && g_cfg.windowMaterial != L"default";
    if (key == L"showToolbarWithSidebar") return g_showToolbarWithSidebar.load();
    if (key == L"showToolbarOnNewTab") return g_showToolbarOnNewTab.load();
    if (key == L"revealToolbarOnHover") return g_revealToolbarOnHover.load();
    if (key == L"hidePinnedTabBorder") return g_hidePinnedTabBorder.load();
    if (key == L"dontPopUpDownloads") return g_dontPopUpDownloads.load();
    if (key == L"hideUpgradeBanner") return g_hideUpgradeBanner.load();
    if (key == L"hideLinkAddress") return g_hideLinkAddress.load();
    if (key == L"arcPinnedTabs") return g_arcPinnedTabs.load();
    if (key == L"hideHistorySuggestions") return g_hideHistorySuggestions.load();
    return false;
}

std::wstring Engine_TweakValue(const std::wstring& key) {
    wchar_t val[128]{};
    if (key == L"peekAnimation") {
        GetPrivateProfileStringW(L"tweaks", L"peekAnimation", L"genie", val, _countof(val), g_configPath);
        return val;
    }
    if (key == L"peekTrigger") {
        GetPrivateProfileStringW(L"tweaks", L"peekTrigger", L"shift", val, _countof(val), g_configPath);
        return val;
    }
    if (key == L"peekExpand") {
        GetPrivateProfileStringW(L"tweaks", L"peekExpand", L"blend", val, _countof(val), g_configPath);
        return val;
    }
    if (key == L"sidebarGrainMaterial") {
        return g_cfg.sidebarGrainMaterial;
    }
    if (key == L"windowMaterial") {
        return g_cfg.windowMaterial;
    }
    return L"";
}

bool Engine_NeedsRestart(const std::wstring&) { return false; }
void Engine_SetTweak(const std::wstring&, bool, const std::wstring*) {}
void Engine_SetAll(bool) {}
void Engine_SetMany(const std::vector<std::pair<std::wstring, bool>>&, const std::vector<std::pair<std::wstring, std::wstring>>&) {}
bool Engine_SafeMode() { return false; }
bool Engine_Hover() { return g_hoverTop.load(); }
void Engine_LeaveSafeMode() {}

void Engine_ForEachNode(const std::function<void(const NodeRef&)>& fn) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (const auto& [h, n] : g_nodes) {
        NodeRef ref{h, n.type, n.name};
        fn(ref);
    }
}

InstanceHandle Engine_Parent(InstanceHandle h) {
    return GetAncestorHandle(h, 1);
}

std::wstring Engine_Name(InstanceHandle h) {
    return NodeName(h);
}

std::wstring Engine_Type(InstanceHandle h) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    auto it = g_nodes.find(h);
    return it == g_nodes.end() ? std::wstring() : it->second.type;
}

IInspectable* Engine_Object(InstanceHandle h) {
    if (!g_diag || !h) return nullptr;
    IInspectable* out = nullptr;
    if (FAILED(g_diag->GetIInspectableFromHandle(h, &out))) return nullptr;
    return out;
}

static std::atomic<int> g_menuPresenterCount{0};

bool Engine_HasOpenMenu() {
    return g_menuPresenterCount.load() > 0;
}

std::vector<InstanceHandle> Engine_FindNodes(const std::wstring& name) {
    return FindNodes(name);
}

void Engine_OnDiaChanged(InstanceHandle, const std::wstring&, const std::wstring&) {}
std::vector<TweakInfo> Engine_Tweaks() { return {}; }

bool Ui_Tick() { return false; }
bool Ui_State(const std::wstring&) { return false; }
void Ui_StateApplied() {}
void Ui_WatchProperty(InstanceHandle, const std::wstring&) {}
void Ui_ElementAdded(const std::wstring&) {}

void Ui_OpenInNewTab(const std::wstring& text) {
    ShellExecuteW(nullptr, L"open", text.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Implemented in history.cpp (the Hide history suggestions tweak).
void Media_Tick() {}

static HWND t_peekWindow = nullptr;
void Ui_SetPeekWindow(HWND w) { t_peekWindow = w; }

bool Ui_PeekFixedSize(HWND w) {
    if (!IsWindow(w)) return false;
    LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
    SetWindowLongPtrW(w, GWL_STYLE, style);
    return true;
}

std::wstring Ui_HoveredLink(HWND top) {
    try {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        for (const auto& [handle, node] : g_nodes) {
            if (node.name == L"StatusText" || node.name == L"StatusPillText") {
                Inspectable obj{nullptr};
                if (GetObject(handle, obj)) {
                    if (auto tb = obj.try_as<muxc::TextBlock>()) {
                        HWND w = tb.XamlRoot() ? TopWindowFor(tb.XamlRoot()) : nullptr;
                        if (top && w && w != top && w != GetForegroundWindow()) continue;
                        std::wstring text(tb.Text());
                        if (text.empty()) {
                            for (auto const& in : tb.Inlines()) {
                                if (auto run = in.try_as<winrt::Microsoft::UI::Xaml::Documents::Run>()) {
                                    text += run.Text();
                                }
                            }
                        }
                        if (!text.empty()) return text;
                    }
                }
            }
        }
    } catch (...) {}
    return L"";
}

static mux::FrameworkElement FirstIn(const mux::DependencyObject& root,
                                     const std::function<bool(const mux::FrameworkElement&)>& want) {
    std::vector<mux::DependencyObject> st{root};
    int n = 0;
    while (!st.empty() && n++ < 5000) {
        auto d = st.back();
        st.pop_back();
        if (auto fe = d.try_as<mux::FrameworkElement>(); fe && want(fe)) return fe;
        int c = muxm::VisualTreeHelper::GetChildrenCount(d);
        for (int i = c - 1; i >= 0; i--) st.push_back(muxm::VisualTreeHelper::GetChild(d, i));
    }
    return nullptr;
}

static bool StartsWithNoCase(std::wstring_view s, std::wstring_view prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.data(), prefix.data(), prefix.size()) == 0;
}
static bool EndsWithNoCase(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && _wcsnicmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

static bool TabLabelMatches(const std::wstring& label, const std::wstring& title) {
    if (label.empty() || label == L"New Tab" || title.empty()) return false;
    if (label.starts_with(title) || title.starts_with(label)) return true;
    size_t colon = title.find(L": ");
    if (colon == std::wstring::npos) return false;
    std::wstring page = title.substr(colon + 2);
    return !page.empty() && (label.starts_with(page) || page.starts_with(label));
}

static mux::FrameworkElement PeekTabLabelExact(const mux::FrameworkElement& list, HWND window) {
    wchar_t title[512]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    auto untrimmed = [](std::wstring text) {
        if (text.ends_with(L"…")) text.pop_back();
        else if (text.ends_with(L"...")) text.resize(text.size() - 3);
        return text;
    };
    std::wstring name = untrimmed(title);
    if (name.empty()) return nullptr;
    return FirstIn(list, [&](const mux::FrameworkElement& fe) {
        auto text = fe.try_as<muxc::TextBlock>();
        if (!text || fe.ActualWidth() <= 1 || fe.ActualHeight() <= 1) return false;
        return TabLabelMatches(untrimmed(std::wstring(text.Text())), name);
    });
}

static mux::FrameworkElement PeekTabLabel(const mux::FrameworkElement& list, HWND window) {
    wchar_t title[512]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    auto untrimmed = [](std::wstring text) {
        if (text.ends_with(L"…")) text.pop_back();
        else if (text.ends_with(L"...")) text.resize(text.size() - 3);
        return text;
    };
    std::wstring name = untrimmed(title);
    if (name.empty()) return nullptr;
    auto match = FirstIn(list, [&](const mux::FrameworkElement& fe) {
        auto text = fe.try_as<muxc::TextBlock>();
        if (!text || fe.ActualWidth() <= 1 || fe.ActualHeight() <= 1) return false;
        return TabLabelMatches(untrimmed(std::wstring(text.Text())), name);
    });
    if (match) return match;
    return FirstIn(list, [&](const mux::FrameworkElement& fe) {
        auto text = fe.try_as<muxc::TextBlock>();
        return text && fe.ActualWidth() > 1 && fe.ActualHeight() > 1 && !text.Text().empty();
    });
}

static mux::FrameworkElement PeekTabList(HWND window) {
    std::vector<mux::FrameworkElement> lists;
    for (auto h : FindNodes(L"TabListPresenter")) {
        auto list = Obj<mux::FrameworkElement>(h);
        auto root = list ? list.XamlRoot() : nullptr;
        if (!root) continue;
        if (TopWindowFor(root) == window) lists.push_back(list);
    }
    if (lists.size() > 1) {
        for (auto& l : lists)
            if (PeekTabLabelExact(l, window)) return l;
        for (auto& l : lists)
            if (l.ActualWidth() > 1 && l.ActualHeight() > 1 && l.Visibility() == mux::Visibility::Visible) return l;
    }
    return lists.empty() ? nullptr : lists.front();
}

bool Ui_PeekTabRow(HWND win, RECT& out) {
    try {
        auto list = PeekTabList(win);
        auto label = list ? PeekTabLabel(list, win) : nullptr;
        if (!label) return false;
        if (auto content = list.XamlRoot().Content()) content.UpdateLayout();
        auto row = label;
        for (auto e = label; e && e != list; e = muxm::VisualTreeHelper::GetParent(e).try_as<mux::FrameworkElement>()) {
            if (e.Name() == L"RowsPanel") break;
            if (e.ActualHeight() <= label.ActualHeight() * 3 && e.ActualWidth() > row.ActualWidth()) row = e;
        }
        double scale = row.XamlRoot().RasterizationScale();
        auto o = row.TransformToVisual(nullptr).TransformPoint({0, 0});
        POINT origin{};
        if (!ClientToScreen(win, &origin)) return false;
        out.left = origin.x + (LONG)std::lround(o.X * scale);
        out.top = origin.y + (LONG)std::lround(o.Y * scale);
        out.right = out.left + (LONG)std::lround(row.ActualWidth() * scale);
        out.bottom = out.top + (LONG)std::lround(row.ActualHeight() * scale);
        return !IsRectEmpty(&out);
    } catch (...) {
        return false;
    }
}

std::wstring Ui_PeekTabInfo(HWND win) {
    std::wstring out;
    try {
        wchar_t title[256]{};
        GetWindowTextW(win, title, ARRAYSIZE(title));
        out = L"title '" + std::wstring(title) + L"'";
        auto list = PeekTabList(win);
        if (!list) return out + L", no tab list";
        out += L", labels:";
        int n = 0;
        FirstIn(list, [&](const mux::FrameworkElement& fe) {
            if (auto text = fe.try_as<muxc::TextBlock>(); text && !text.Text().empty()) {
                out += L" '" + std::wstring(text.Text()) + (fe.ActualWidth() <= 1 ? L"' (hidden)" : L"'");
                ++n;
            }
            return n >= 12;
        });
    } catch (...) {}
    return out;
}

std::wstring Ui_PeekMenuInfo(HWND win) {
    std::wstring out;
    try {
        auto list = PeekTabList(win);
        if (!list) return L"no tab list";
        auto pops = muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot());
        out = std::to_wstring(pops.Size()) + L" popups open";
        int n = 0;
        for (auto const& pop : pops) {
            auto child = pop.Child();
            if (!child) continue;
            FirstIn(child, [&](const mux::FrameworkElement& fe) {
                if (auto sub = fe.try_as<muxc::MenuFlyoutSubItem>()) out += L" [" + std::wstring(sub.Text()) + L" >]", ++n;
                else if (auto item = fe.try_as<muxc::MenuFlyoutItem>()) out += L" [" + std::wstring(item.Text()) + L"]", ++n;
                return n >= 20;
            });
        }
    } catch (...) {}
    return out;
}

bool Ui_PeekShowTabMenu(HWND window) {
    try {
        auto list = PeekTabList(window);
        auto label = list ? PeekTabLabel(list, window) : nullptr;
        for (auto element = label; element && element != list;
             element = muxm::VisualTreeHelper::GetParent(element).try_as<mux::FrameworkElement>()) {
            if (auto flyout = element.ContextFlyout().try_as<muxc::MenuFlyout>()) {
                flyout.ShowAt(element);
                return true;
            }
        }
    } catch (...) {}
    return false;
}

bool Ui_PeekMenuOpen(HWND win) {
    try {
        auto list = PeekTabList(win);
        return list && muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot()).Size() > 0;
    } catch (...) {
        return false;
    }
}

void Ui_PeekCloseTabMenu(HWND window) {
    try {
        if (auto list = PeekTabList(window))
            for (auto const& popup : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot()))
                popup.IsOpen(false);
    } catch (...) {}
}

int Ui_PeekSplitWith(HWND win, const std::wstring& title) {
    (void)title;
    try {
        auto list = PeekTabList(win);
        if (!list) return -1;
        for (auto const& pop : muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot())) {
            auto child = pop.Child();
            if (!child) continue;
            auto item = FirstIn(child, [](const mux::FrameworkElement& fe) {
                auto mi = fe.try_as<muxc::MenuFlyoutItem>();
                return mi && StartsWithNoCase(mi.Text(), L"Open as split");
            });
            if (!item) continue;
            auto mi = item.as<muxc::MenuFlyoutItem>();
            if (!mi.IsEnabled()) return -1;
            winrt::Microsoft::UI::Xaml::Automation::Peers::MenuFlyoutItemAutomationPeer(mi).Invoke();
            return 1;
        }
        return 0;
    } catch (...) {
        return -1;
    }
}

bool Ui_PeekTabRowNamed(HWND win, const std::wstring& title, RECT& out) {
    try {
        auto list = PeekTabList(win);
        if (!list) return false;
        auto untrimmed = [](std::wstring text) {
            if (text.ends_with(L"…")) text.pop_back();
            else if (text.ends_with(L"...")) text.resize(text.size() - 3);
            return text;
        };
        std::wstring name = untrimmed(title);
        if (name.empty()) return false;
        auto label = FirstIn(list, [&](const mux::FrameworkElement& fe) {
            auto text = fe.try_as<muxc::TextBlock>();
            if (!text || fe.ActualWidth() <= 1 || fe.ActualHeight() <= 1) return false;
            return TabLabelMatches(untrimmed(std::wstring(text.Text())), name);
        });
        if (!label) return false;
        double scale = label.XamlRoot().RasterizationScale();
        auto o = label.TransformToVisual(nullptr).TransformPoint({0, 0});
        POINT origin{};
        if (!ClientToScreen(win, &origin)) return false;
        out.left = origin.x + (LONG)std::lround(o.X * scale);
        out.top = origin.y + (LONG)std::lround(o.Y * scale);
        out.right = out.left + (LONG)std::lround(label.ActualWidth() * scale);
        out.bottom = out.top + (LONG)std::lround(label.ActualHeight() * scale);
        return !IsRectEmpty(&out);
    } catch (...) {
        return false;
    }
}

int Ui_PeekMoveTab(HWND win, HWND to) {
    try {
        auto list = PeekTabList(win);
        if (!list || !IsWindow(to)) return -1;
        auto pops = muxm::VisualTreeHelper::GetOpenPopupsForXamlRoot(list.XamlRoot());
        for (auto const& pop : pops) {
            auto child = pop.Child();
            if (!child) continue;
            auto element = FirstIn(child, [](const mux::FrameworkElement& fe) {
                auto sub = fe.try_as<muxc::MenuFlyoutSubItem>();
                return sub && StartsWithNoCase(sub.Text(), L"Move to Window");
            });
            if (!element) continue;
            auto sub = element.as<muxc::MenuFlyoutSubItem>();

            std::vector<muxc::MenuFlyoutItem> destinations;
            for (auto const& item : sub.Items()) {
                auto candidate = item.try_as<muxc::MenuFlyoutItem>();
                if (candidate && _wcsicmp(candidate.Text().c_str(), L"New Window") != 0) destinations.push_back(candidate);
            }
            if (destinations.size() == 2 && destinations[0].IsEnabled() != destinations[1].IsEnabled()) {
                destinations.erase(destinations.begin() + (destinations[0].IsEnabled() ? 1 : 0));
            }
            if (destinations.size() == 2) {
                auto singleTab = [](const muxc::MenuFlyoutItem& item) {
                    return EndsWithNoCase(item.Text(), L" & 1 Tab");
                };
                bool firstIsSource = singleTab(destinations[0]), secondIsSource = singleTab(destinations[1]);
                if (firstIsSource == secondIsSource) firstIsSource = true;
                destinations.erase(destinations.begin() + (firstIsSource ? 0 : 1));
            }
            if (destinations.size() != 1 || !destinations.front().IsEnabled()) return -1;
            Peek_BeforeMove();
            winrt::Microsoft::UI::Xaml::Automation::Peers::MenuFlyoutItemAutomationPeer(destinations.front()).Invoke();
            return 1;
        }
    } catch (...) {}
    return -1;
}

static bool WritePng(const wchar_t* path, const uint8_t* bgra, int width, int height) {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return false;
    IWICBitmapEncoder* encoder = nullptr;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr) || !encoder) { factory->Release(); return false; }
    IWICStream* stream = nullptr;
    hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr) && stream) {
        hr = stream->InitializeFromFilename(path, GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    IWICBitmapFrameEncode* frame = nullptr;
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr) && frame) {
        hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr)) hr = frame->WritePixels(height, width * 4, width * height * 4, const_cast<uint8_t*>(bgra));
        if (SUCCEEDED(hr)) hr = frame->Commit();
        frame->Release();
    }
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (stream) stream->Release();
    encoder->Release();
    factory->Release();
    return SUCCEEDED(hr);
}

// ---- Grain overlay helper
static winrt::Microsoft::UI::Xaml::Media::ImageBrush g_grainBrush{nullptr};
static std::wstring g_currentGrainMaterial;
static int g_currentGrainAmount = -1;
static int g_currentGrainSoftness = -1;
static uint32_t g_grainGen = 0;
static wchar_t g_lastGrainPng[MAX_PATH]{};

static winrt::Microsoft::UI::Xaml::Media::ImageBrush GetOrCreateGrainBrush(const std::wstring& material, int amount = -1, int softness = -1) {
    if (amount < 0) amount = g_cfg.sidebarGrainAmount;
    if (softness < 0) softness = g_cfg.sidebarGrainSoftness;

    if (g_grainBrush && g_currentGrainMaterial == material && g_currentGrainAmount == amount && g_currentGrainSoftness == softness) {
        return g_grainBrush;
    }

    // Clean up any stale grain PNG files from previous sessions
    static bool s_cleanedOld = false;
    if (!s_cleanedOld) {
        s_cleanedOld = true;
        WIN32_FIND_DATAW fd{};
        wchar_t searchPattern[MAX_PATH]{};
        swprintf_s(searchPattern, _countof(searchPattern), L"%s\\sidebar-grain-*.png", g_dir);
        HANDLE hFind = FindFirstFileW(searchPattern, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                wchar_t oldPath[MAX_PATH]{};
                swprintf_s(oldPath, _countof(oldPath), L"%s\\%s", g_dir, fd.cFileName);
                DeleteFileW(oldPath);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    wchar_t pngPath[MAX_PATH]{};
    swprintf_s(pngPath, _countof(pngPath), L"%s\\sidebar-grain-%s-%u.png", g_dir, material.c_str(), ++g_grainGen);
    const int tileW = 200, tileH = 200;
    std::vector<uint8_t> rgba(tileW * tileH * 4);
    Grain::GenerateTexture(material, rgba.data(), amount, softness);

    // Pre-tile into a 512 x 2048 seamless buffer so the texture displays at 1:1 scale
    const int w = 512, h = 2048;
    std::vector<uint8_t> tiled(w * h * 4);
    for (int y = 0; y < h; ++y) {
        int ty = y % tileH;
        for (int x = 0; x < w; ++x) {
            int tx = x % tileW;
            size_t srcIdx = (ty * tileW + tx) * 4;
            size_t dstIdx = (y * w + x) * 4;
            tiled[dstIdx + 0] = rgba[srcIdx + 0]; // B
            tiled[dstIdx + 1] = rgba[srcIdx + 1]; // G
            tiled[dstIdx + 2] = rgba[srcIdx + 2]; // R
            tiled[dstIdx + 3] = rgba[srcIdx + 3]; // A
        }
    }
    WritePng(pngPath, tiled.data(), w, h);

    if (g_lastGrainPng[0] != L'\0' && _wcsicmp(g_lastGrainPng, pngPath) != 0) {
        DeleteFileW(g_lastGrainPng);
    }
    wcscpy_s(g_lastGrainPng, pngPath);

    try {
        winrt::Windows::Foundation::Uri uri(std::wstring(L"file:///") + pngPath);
        muxm::Imaging::BitmapImage bmp(uri);
        bmp.CreateOptions(muxm::Imaging::BitmapCreateOptions::IgnoreImageCache);
        muxm::ImageBrush brush;
        brush.ImageSource(bmp);
        brush.Stretch(muxm::Stretch::None);
        brush.AlignmentX(muxm::AlignmentX::Left);
        brush.AlignmentY(muxm::AlignmentY::Top);
        brush.Opacity(1.0);
        g_grainBrush = brush;
        g_currentGrainMaterial = material;
        g_currentGrainAmount = amount;
        g_currentGrainSoftness = softness;
        Log(L"sidebar grain: loaded PNG brush for '%s' amount=%d softness=%d (gen=%u)", material.c_str(), amount, softness, g_grainGen);
        return g_grainBrush;
    } catch (...) {
        return nullptr;
    }
}

static void UpdateSidebarGrain(InstanceHandle handle, bool on) {
    Inspectable obj{nullptr};
    if (!GetObject(handle, obj)) return;
    auto border = obj.try_as<muxc::Border>();
    if (!border) return;

    std::wstring nodeName = NodeName(handle);

    // If SidebarFloatingSurface is visible and has width > 0, let it own the grain overlay
    // and ensure SidebarBorder's overlay is collapsed to avoid double stacking.
    if (nodeName == L"SidebarBorder") {
        for (auto fh : FindNodes(L"SidebarFloatingSurface")) {
            auto fe = Obj<mux::FrameworkElement>(fh);
            if (fe && fe.Visibility() == mux::Visibility::Visible && fe.ActualWidth() > 0) {
                auto child = border.Child();
                if (auto panel = child ? child.try_as<muxc::Panel>() : nullptr) {
                    auto children = panel.Children();
                    for (uint32_t i = 0; i < children.Size(); ++i) {
                        if (auto b = children.GetAt(i).try_as<muxc::Border>()) {
                            if (b.Name() == L"DiaTweaksSidebarGrainOverlay") {
                                b.Visibility(mux::Visibility::Collapsed);
                            }
                        }
                    }
                }
                return;
            }
        }
    } else if (nodeName == L"SidebarFloatingSurface") {
        auto fe = border.try_as<mux::FrameworkElement>();
        if (fe && (fe.Visibility() != mux::Visibility::Visible || fe.ActualWidth() <= 0)) {
            // Floating sidebar is currently hidden; collapse its overlay if present
            auto child = border.Child();
            if (auto panel = child ? child.try_as<muxc::Panel>() : nullptr) {
                auto children = panel.Children();
                for (uint32_t i = 0; i < children.Size(); ++i) {
                    if (auto b = children.GetAt(i).try_as<muxc::Border>()) {
                        if (b.Name() == L"DiaTweaksSidebarGrainOverlay") {
                            b.Visibility(mux::Visibility::Collapsed);
                        }
                    }
                }
            }
            return;
        }
    }

    auto child = border.Child();
    if (!child) return;

    muxc::Border overlay{nullptr};
    auto panel = child.try_as<muxc::Panel>();
    if (panel) {
        auto children = panel.Children();
        std::vector<uint32_t> dupes;
        for (uint32_t i = 0; i < children.Size(); ++i) {
            if (auto b = children.GetAt(i).try_as<muxc::Border>()) {
                if (b.Name() == L"DiaTweaksSidebarGrainOverlay") {
                    if (!overlay) {
                        overlay = b;
                    } else {
                        dupes.push_back(i);
                    }
                }
            }
        }
        for (auto it = dupes.rbegin(); it != dupes.rend(); ++it) {
            children.RemoveAt(*it);
        }

        if (!on) {
            if (overlay) overlay.Visibility(mux::Visibility::Collapsed);
            return;
        }

        auto brush = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial);
        if (!brush) return;

        if (!overlay) {
            overlay = muxc::Border();
            overlay.Name(L"DiaTweaksSidebarGrainOverlay");
            overlay.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            overlay.VerticalAlignment(mux::VerticalAlignment::Stretch);
            overlay.IsHitTestVisible(false);
            if (auto grid = panel.try_as<muxc::Grid>()) {
                muxc::Grid::SetRowSpan(overlay, 99);
                muxc::Grid::SetColumnSpan(overlay, 99);
            }
            overlay.Background(brush);
            overlay.Opacity(1.0);
            overlay.Visibility(mux::Visibility::Visible);
            children.InsertAt(0, overlay);
            Log(L"sidebar grain: overlay added to '%s'", NodeName(handle).c_str());
        } else {
            overlay.Background(brush);
            overlay.Opacity(1.0);
            overlay.Visibility(mux::Visibility::Visible);
        }
    } else {
        muxc::Grid wrapper = child.try_as<muxc::Grid>();
        if (wrapper && wrapper.Name() == L"DiaTweaksSidebarGrainWrapper") {
            auto children = wrapper.Children();
            std::vector<uint32_t> dupes;
            for (uint32_t i = 0; i < children.Size(); ++i) {
                if (auto b = children.GetAt(i).try_as<muxc::Border>()) {
                    if (b.Name() == L"DiaTweaksSidebarGrainOverlay") {
                        if (!overlay) {
                            overlay = b;
                        } else {
                            dupes.push_back(i);
                        }
                    }
                }
            }
            for (auto it = dupes.rbegin(); it != dupes.rend(); ++it) {
                children.RemoveAt(*it);
            }

            if (!on) {
                if (overlay) overlay.Visibility(mux::Visibility::Collapsed);
                return;
            }
            auto brush = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial);
            if (brush && overlay) {
                overlay.Background(brush);
                overlay.Opacity(1.0);
                overlay.Visibility(mux::Visibility::Visible);
            }
        } else if (on) {
            auto brush = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial);
            if (!brush) return;
            wrapper = muxc::Grid();
            wrapper.Name(L"DiaTweaksSidebarGrainWrapper");
            wrapper.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            wrapper.VerticalAlignment(mux::VerticalAlignment::Stretch);

            overlay = muxc::Border();
            overlay.Name(L"DiaTweaksSidebarGrainOverlay");
            overlay.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
            overlay.VerticalAlignment(mux::VerticalAlignment::Stretch);
            overlay.IsHitTestVisible(false);
            overlay.Background(brush);
            overlay.Opacity(1.0);
            overlay.Visibility(mux::Visibility::Visible);

            border.Child(nullptr);
            wrapper.Children().Append(overlay);
            wrapper.Children().Append(child.as<mux::UIElement>());
            border.Child(wrapper);
            Log(L"sidebar grain: wrapped non-panel child of '%s'", NodeName(handle).c_str());
        }
    }
}

// ---------------------------------------------------------------- helpers for advanced tweaks

static mux::FrameworkElement FindByName(const mux::DependencyObject& root, const std::wstring& name) {
    auto found = FindVisual(root, [&](const mux::DependencyObject& d) {
        auto fe = d.try_as<mux::FrameworkElement>();
        return fe && fe.Name() == name;
    });
    return found ? found.try_as<mux::FrameworkElement>() : nullptr;
}

// --- Toolbar Slide & Hover
struct BarAnim {
    winrt::weak_ref<mux::FrameworkElement> group, bar;
    mux::Thickness saved{};
    double offset = 0;  // 0 = shown, -h = hidden
    double from = 0, to = 0;
    double start = 0;
    bool animating = false, managed = false;
    bool overlay = false;  // "Over the page": page stays put, only the toolbar slides
    // Toolbar/containers up to the group, raised above sibling layers while over the page.
    std::vector<std::pair<winrt::weak_ref<mux::UIElement>, int>> raised;
    // Over the page: crop the group to the page card so the window inset stays
    // visible at the top and bottom while the page is moved up behind the toolbar.
    muxm::RectangleGeometry savedClip{nullptr};
    muxm::RectangleGeometry overlayClip{nullptr};
    // Rounded crop (composition), so the visible top corners match the card's bottom.
    mucomp::CompositionClip savedVisualClip{nullptr};
    mucomp::CompositionGeometricClip overlayRoundedClip{nullptr};
    mucomp::CompositionRoundedRectangleGeometry overlayRoundedGeometry{nullptr};
    bool clipSaved = false;
};

static thread_local std::map<InstanceHandle, BarAnim> t_bars;
static thread_local winrt::event_token t_renderToken{};
static thread_local bool t_renderHooked = false;
static thread_local bool t_sliding = false;
static const double kSlideMs = 200;

static double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

static double BarHeight(BarAnim& a) {
    auto b = a.bar.get();
    double h = b ? b.ActualHeight() : 0;
    return h > 1 ? h : 40;
}

static bool OverlayToolbar() { return g_cfg.hideToolbarMode == L"overlay"; }
static void UpdatePageClips();

static void RestoreOverlayClip(BarAnim& a) {
    if (!a.clipSaved) return;
    if (auto g = a.group.get()) {
        try { g.Clip(a.savedClip); } catch (...) {}
        try {
            auto visual = mux::Hosting::ElementCompositionPreview::GetElementVisual(g);
            if (visual) visual.Clip(a.savedVisualClip);
        } catch (...) {}
    }
    a.savedClip = nullptr;
    a.overlayClip = nullptr;
    a.savedVisualClip = nullptr;
    a.overlayRoundedClip = nullptr;
    a.overlayRoundedGeometry = nullptr;
    a.clipSaved = false;
}

static void RaiseBar(BarAnim& a, bool raise) {
    if (!raise) {
        for (auto& [w, z] : a.raised)
            if (auto el = w.get()) muxc::Canvas::SetZIndex(el, z);
        a.raised.clear();
        return;
    }
    if (!a.raised.empty()) return;
    auto g = a.group.get();
    auto b = a.bar.get();
    if (!g || !b) return;
    for (mux::DependencyObject d = b; d && d != g;) {
        auto parent = muxm::VisualTreeHelper::GetParent(d);
        if (parent.try_as<muxc::Panel>())
            if (auto el = d.try_as<mux::UIElement>()) {
                a.raised.push_back({winrt::make_weak(el), muxc::Canvas::GetZIndex(el)});
                muxc::Canvas::SetZIndex(el, 1000);
            }
        d = parent;
    }
}

static void ApplyOffset(BarAnim& a, double h, bool settled) {
    auto g = a.group.get();
    if (!g) return;
    if (a.overlay) {
        // The page container stays a toolbar height higher than Dia laid it out; only
        // the toolbar moves, sliding back down over the top of the page.
        mux::Thickness m{a.saved.Left, a.saved.Top - h, a.saved.Right, a.saved.Bottom};
        auto cur = g.Margin();
        if (cur.Left != m.Left || cur.Top != m.Top || cur.Right != m.Right || cur.Bottom != m.Bottom) g.Margin(m);
        if (auto b = a.bar.get()) {
            float y = (float)(a.offset + h);
            if (b.Translation().y != y) b.Translation({0.0f, y, 0.0f});
        }
        // Keep the window's 4 px page inset: the page may fill the toolbar strip,
        // but it must not paint over the top/bottom border of the window.
        if (!a.clipSaved) {
            try {
                a.savedClip = g.Clip();
                auto visual = mux::Hosting::ElementCompositionPreview::GetElementVisual(g);
                if (visual) a.savedVisualClip = visual.Clip();
                a.clipSaved = true;
            } catch (...) {}
        }
        double hh = BarHeight(a);
        double wh = (std::max)(0.0, g.ActualHeight() - hh);
        double radius = g_squareSidebarCorners.load() ? 0.0 : 8.0;
        if (auto grid = g.try_as<muxc::Grid>()) {
            auto cr = grid.CornerRadius();
            radius = (std::max)(0.0, (std::max)((double)cr.TopLeft, (double)cr.TopRight));
        }
        bool rounded = false;
        try {
            auto visual = mux::Hosting::ElementCompositionPreview::GetElementVisual(g);
            if (visual) {
                auto comp = visual.Compositor();
                if (!a.overlayRoundedGeometry) {
                    a.overlayRoundedGeometry = comp.CreateRoundedRectangleGeometry();
                    a.overlayRoundedClip = comp.CreateGeometricClip(a.overlayRoundedGeometry);
                }
                a.overlayRoundedGeometry.Offset({0.0f, (float)hh});
                a.overlayRoundedGeometry.Size({(float)g.ActualWidth(), (float)wh});
                a.overlayRoundedGeometry.CornerRadius({(float)radius, (float)radius});
                if (g.Clip() == a.overlayClip) g.Clip(a.savedClip);
                if (visual.Clip() != a.overlayRoundedClip) visual.Clip(a.overlayRoundedClip);
                rounded = true;
            }
        } catch (...) {}
        if (!rounded) {
            if (!a.overlayClip) a.overlayClip = muxm::RectangleGeometry();
            a.overlayClip.Rect({0.0f, (float)hh, (float)g.ActualWidth(), (float)wh});
            try {
                if (g.Clip() != a.overlayClip) g.Clip(a.overlayClip);
            } catch (...) {}
        }
        RaiseBar(a, true);
        return;
    }
    double top = a.offset;
    bool constantHeight = !settled || IsNewTab();
    double bottom = constantHeight ? -(h + top) : 0;
    mux::Thickness m{a.saved.Left, a.saved.Top + top, a.saved.Right, a.saved.Bottom + bottom};
    auto cur = g.Margin();
    if (cur.Left != m.Left || cur.Top != m.Top || cur.Right != m.Right || cur.Bottom != m.Bottom) g.Margin(m);
}

static void OnRendering(winrt::Windows::Foundation::IInspectable const&, winrt::Windows::Foundation::IInspectable const&) {
    bool any = false;
    double now = NowMs();
    for (auto& [hnd, a] : t_bars) {
        if (!a.animating) continue;
        double h = BarHeight(a);
        double t = (now - a.start) / kSlideMs;
        if (t >= 1) {
            a.offset = a.to;
            a.animating = false;
            ApplyOffset(a, h, true);
            continue;
        }
        double e = 1 - std::pow(1 - t, 3);
        a.offset = a.from + (a.to - a.from) * e;
        ApplyOffset(a, h, false);
        any = true;
    }
    UpdatePageClips();  // the cut-away top of the page follows the sliding toolbar
    if (!any) {
        t_sliding = false;
        if (t_renderHooked) {
            muxm::CompositionTarget::Rendering(t_renderToken);
            t_renderHooked = false;
        }
    }
}

static void EnsureRendering(bool on) {
    t_sliding = on;
    if (on && !t_renderHooked) {
        t_renderToken = muxm::CompositionTarget::Rendering(OnRendering);
        t_renderHooked = true;
    } else if (!on && t_renderHooked) {
        muxm::CompositionTarget::Rendering(t_renderToken);
        t_renderHooked = false;
    }
}

// ---- Over the page: cut the top of each Chromium page window away under the
// toolbar. Web pages are separate HWNDs above Dia's XAML, so the toolbar can
// only show over them by taking that strip out of the page window's region.
static thread_local std::map<HWND, std::vector<RECT>> t_pageClips;

static void UnclipPage(HWND w) {
    if (IsWindow(w)) SetWindowRgn(w, nullptr, TRUE);
}

static void UpdatePageClips() {
    std::map<HWND, std::vector<RECT>> bars;
    for (auto& [hnd, a] : t_bars) {
        if (!a.managed || !a.overlay) continue;
        auto g = a.group.get();
        auto b = a.bar.get();
        if (!g || !b) continue;
        auto root = g.XamlRoot();
        if (!root) continue;
        HWND top = TopWindowFor(root);
        if (!top) continue;
        double h = BarHeight(a);
        double shown = a.offset + h;  // how much of the toolbar is in the window
        double sc = root.RasterizationScale();
        auto at = g.TransformToVisual(nullptr).TransformPoint({0, 0});
        double barLeft = b.TransformToVisual(g).TransformPoint({0, 0}).X + at.X;
        POINT origin{0, 0};
        ClientToScreen(top, &origin);
        auto cutRect = [&](double left, double right, double topY, double bottomY) {
            RECT r{origin.x + (LONG)std::floor(left * sc), origin.y + (LONG)std::floor(topY * sc),
                   origin.x + (LONG)std::ceil(right * sc), origin.y + (LONG)std::ceil(bottomY * sc)};
            if (r.right > r.left && r.bottom > r.top) bars[top].push_back(r);
        };
        // The page is allowed to fill the toolbar strip, but never the window's
        // inset: the top/bottom border has to stay visible in every state.
        double topInset = at.Y + h;
        double pageBottom = at.Y + g.ActualHeight();
        if (topInset > 0.1) cutRect(at.X, at.X + g.ActualWidth(), 0.0, topInset);
        cutRect(at.X, at.X + g.ActualWidth(), pageBottom, pageBottom + 10000.0);
        if (shown > 0.5) {
            cutRect(barLeft, barLeft + b.ActualWidth(), at.Y + h, at.Y + h + shown);
        }
    }

    std::map<HWND, std::vector<RECT>> want;
    for (auto& [top, rects] : bars) {
        struct Ctx {
            HWND top;
            const std::vector<RECT>* rects;
            std::map<HWND, std::vector<RECT>>* want;
        } ctx{top, &rects, &want};
        EnumChildWindows(top, [](HWND w, LPARAM l) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(l);
            if (GetParent(w) != c.top) return TRUE;
            wchar_t cls[64];
            GetClassNameW(w, cls, 64);
            RECT wr, inter;
            if (wcscmp(cls, L"Chrome_WidgetWin_1") || !IsWindowVisible(w) || !GetWindowRect(w, &wr)) return TRUE;
            std::vector<RECT> cut;
            for (auto& r : *c.rects)
                if (IntersectRect(&inter, &r, &wr))
                    cut.push_back({inter.left - wr.left, inter.top - wr.top, inter.right - wr.left, inter.bottom - wr.top});
            if (!cut.empty()) (*c.want)[w] = cut;
            return TRUE;
        }, (LPARAM)&ctx);
    }

    auto same = [](const std::vector<RECT>& x, const std::vector<RECT>& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++)
            if (!EqualRect(&x[i], &y[i])) return false;
        return true;
    };
    for (auto it = t_pageClips.begin(); it != t_pageClips.end();) {
        if (!want.count(it->first)) {
            UnclipPage(it->first);
            it = t_pageClips.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [w, cut] : want) {
        auto old = t_pageClips.find(w);
        if (old != t_pageClips.end() && same(old->second, cut)) continue;
        if (old == t_pageClips.end()) {
            // Leave windows someone else shaped (Dia, Peek) alone.
            HRGN cur = CreateRectRgn(0, 0, 0, 0);
            bool has = GetWindowRgn(w, cur) != ERROR;
            DeleteObject(cur);
            if (has) continue;
        }
        HRGN rgn = CreateRectRgn(-32768, -32768, 32767, 32767);
        for (auto& r : cut) {
            HRGN hole = CreateRectRgnIndirect(&r);
            CombineRgn(rgn, rgn, hole, RGN_DIFF);
            DeleteObject(hole);
        }
        SetWindowRgn(w, rgn, TRUE);  // the system owns `rgn` now
        t_pageClips[w] = cut;
    }
}

static bool FocusWithin(const mux::FrameworkElement& bar) {
    try {
        auto root = bar.XamlRoot();
        if (!root) return false;
        auto barDo = bar.try_as<mux::DependencyObject>();
        if (!barDo) return false;
        for (auto f = mux::Input::FocusManager::GetFocusedElement(root).try_as<mux::DependencyObject>(); f;
             f = muxm::VisualTreeHelper::GetParent(f))
            if (f == barDo) return true;
    } catch (...) {}
    return false;
}

// Dia lays an invisible caption-input window over the toolbar that turns presses
// there into window drags. With the toolbar hidden the page sits under it, so
// trim that sink down to the sidebar part while the toolbar is out.
struct SinkState {
    HWND sink = nullptr;
    HRGN dia = nullptr;  // Dia's own shape (owned copy), or null
    bool diaHadRegion = false;
    HRGN mine = nullptr;  // the shape we last applied
};
static thread_local std::map<HWND, SinkState> t_sinks;

static HWND FindSink(HWND top) {
    struct Ctx { HWND found = nullptr; } ctx;
    EnumChildWindows(top, [](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, L"BoostBrowser.WindowCaptionInputSink") == 0 ||
            wcscmp(cls, L"BoostBrowser.NativeCaptionInputWindow") == 0) {
            reinterpret_cast<Ctx*>(lp)->found = h;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

static HRGN CopyRgn(HRGN r) {
    if (!r) return nullptr;
    HRGN c = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(c, r, nullptr, RGN_COPY);
    return c;
}

static void RestoreSink(SinkState& st) {
    if (!st.sink || !IsWindow(st.sink)) return;
    SetWindowRgn(st.sink, st.diaHadRegion ? CopyRgn(st.dia) : nullptr, TRUE);
    if (st.mine) DeleteObject(st.mine), st.mine = nullptr;
}

// pageLeftPx: where the page starts, in the top-level window's client pixels.
static void TrimSink(HWND top, bool trim, int pageLeftPx) {
    auto& st = t_sinks[top];
    if (!st.sink || !IsWindow(st.sink)) {
        if (st.mine) DeleteObject(st.mine);
        if (st.dia) DeleteObject(st.dia);
        st = SinkState{};
        st.sink = FindSink(top);
    }
    if (!st.sink) return;
    if (!trim) {
        if (st.mine) RestoreSink(st);
        return;
    }
    HRGN cur = CreateRectRgn(0, 0, 0, 0);
    bool has = GetWindowRgn(st.sink, cur) != ERROR;
    if (!st.mine || !has || !EqualRgn(cur, st.mine)) {
        if (st.dia) DeleteObject(st.dia);
        st.dia = has ? CopyRgn(cur) : nullptr;
        st.diaHadRegion = has;
        RECT wr;
        GetWindowRect(st.sink, &wr);
        POINT origin{0, 0};
        ClientToScreen(top, &origin);
        int keepW = pageLeftPx - (wr.left - origin.x);
        HRGN want = CreateRectRgn(0, 0, (std::max)(0, keepW), wr.bottom - wr.top);
        if (has) CombineRgn(want, want, cur, RGN_AND);
        if (st.mine) DeleteObject(st.mine);
        st.mine = CopyRgn(want);
        SetWindowRgn(st.sink, want, TRUE);
    }
    DeleteObject(cur);
}

static void UpdateSinkTrim() {
    std::map<HWND, int> tops;
    for (auto& [h, a] : t_bars) {
        auto g = a.group.get();
        if (!g || !a.managed || a.to > -0.5) continue;
        auto root = g.XamlRoot();
        if (!root) continue;
        HWND top = TopWindowFor(root);
        if (!top) continue;
        int left = (int)(g.TransformToVisual(nullptr).TransformPoint({0, 0}).X * root.RasterizationScale());
        auto it = tops.find(top);
        tops[top] = it == tops.end() ? left : (std::min)(it->second, left);
    }
    for (auto& [top, st] : t_sinks) {
        auto it = tops.find(top);
        if (it == tops.end()) TrimSink(top, false, 0);
        else TrimSink(top, true, it->second);
    }
}

// Dia 1.7 puts the window buttons in the page's layer group; the shown toolbar
// can paint over them. Raise their branch just above the toolbar's when covered.
static void KeepCaptionAbove(const mux::FrameworkElement& grid, const mux::FrameworkElement& bar) {
    if (!grid || !bar) return;
    std::vector<mux::DependencyObject> barChain;
    for (mux::DependencyObject d = bar; d; d = muxm::VisualTreeHelper::GetParent(d)) barChain.push_back(d);
    mux::DependencyObject mine = grid;
    for (auto parent = muxm::VisualTreeHelper::GetParent(mine); parent;
         mine = parent, parent = muxm::VisualTreeHelper::GetParent(parent)) {
        auto it = std::find(barChain.begin(), barChain.end(), parent);
        if (it == barChain.end()) continue;
        if (it == barChain.begin()) return;
        auto panel = parent.try_as<muxc::Panel>();
        auto myLayer = mine.try_as<mux::UIElement>();
        auto barLayer = (*std::prev(it)).try_as<mux::UIElement>();
        if (!panel || !myLayer || !barLayer) return;
        uint32_t myIndex = 0, barIndex = 0;
        panel.Children().IndexOf(myLayer, myIndex);
        panel.Children().IndexOf(barLayer, barIndex);
        int myZ = muxc::Canvas::GetZIndex(myLayer), barZ = muxc::Canvas::GetZIndex(barLayer);
        bool covered = barZ > myZ || (barZ == myZ && barIndex > myIndex);
        if (!covered) return;
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log(L"window buttons: under the page (z %d at %u, page z %d at %u): raised", myZ, myIndex, barZ, barIndex);
        }
        muxc::Canvas::SetZIndex(myLayer, barZ + 1);
        return;
    }
}

static void UpdateHiddenToolbar() {
    bool hide = g_hideToolbar.load();
    bool show = (g_hideToolbar.load() && (g_revealToolbarOnHover.load() && g_hoverTop.load())) ||
                (g_showToolbarWithSidebar.load() && IsSidebarOpen()) ||
                (g_showToolbarOnNewTab.load() && IsNewTab());
    bool hideAll = hide;
    bool anyFocused = false;
    for (auto gh : FindNodes(L"ContentGroup")) {
        auto& a = t_bars[gh];
        auto group = a.group.get();
        if (!group) {
            group = Obj<mux::FrameworkElement>(gh);
            if (!group) continue;
            a.group = winrt::make_weak(group);
            auto bar = FindVisual(group, [](const mux::DependencyObject& d) {
                auto fe = d.try_as<mux::FrameworkElement>();
                return fe && fe.Name() == L"NavigationBarHost";
            });
            if (auto fe = bar.try_as<mux::FrameworkElement>()) a.bar = winrt::make_weak(fe);
        }
        bool peek = t_peekWindow && group.XamlRoot() && TopWindowFor(group.XamlRoot()) == t_peekWindow;
        hide = hideAll && !peek;
        if (!hide && !a.managed) continue;
        if (!a.managed) {
            a.saved = group.Margin();
            a.managed = true;
            a.offset = 0;
            a.to = 0;
        }
        auto barEl = a.bar.get();
        // Over the page moves the toolbar itself, so it must be the one Dia shows:
        // the start page has another NavigationBarHost that is left empty.
        if (a.overlay && (!barEl || barEl.ActualWidth() < 1)) {
            auto live = FindVisual(group, [](const mux::DependencyObject& d) {
                auto fe = d.try_as<mux::FrameworkElement>();
                return fe && fe.Name() == L"NavigationBarHost" && fe.ActualWidth() >= 1;
            }).try_as<mux::FrameworkElement>();
            if (live && live != barEl) {
                RaiseBar(a, false);
                if (barEl) barEl.Translation({0.0f, 0.0f, 0.0f});
                a.bar = winrt::make_weak(live);
                barEl = live;
                ApplyOffset(a, BarHeight(a), !a.animating);
            }
        }
        if (barEl && barEl.Visibility() != mux::Visibility::Visible) {
            barEl.Visibility(mux::Visibility::Visible);
        }
        bool focused = barEl && FocusWithin(barEl);
        if (focused) anyFocused = true;
        bool want = !hide || show || focused;
        double h = BarHeight(a);
        double target = want ? 0 : -h;
        // Switch between over-the-page and pushing while finishing any slide where
        // it was heading, then carry on in the new mode.
        bool overlay = hide && g_hideToolbar.load() && OverlayToolbar();
        if (a.overlay != overlay) {
            if (a.overlay && barEl) barEl.Translation({0.0f, 0.0f, 0.0f});
            if (a.overlay) RaiseBar(a, false);
            if (a.overlay) RestoreOverlayClip(a);
            a.overlay = overlay;
            a.animating = false;
            a.offset = a.to;
            ApplyOffset(a, h, true);
        }
        if (!hide && !a.animating && a.offset > -0.5) {
            group.Margin(a.saved);
            RestoreOverlayClip(a);
            a.managed = false;
            continue;
        }
        if (a.to == target && (a.animating || std::abs(a.offset - target) < 0.5)) {
            if (!a.animating) ApplyOffset(a, h, true);
            continue;
        }
        a.from = a.offset;
        a.to = target;
        a.start = NowMs();
        a.animating = true;
        EnsureRendering(true);
    }
    for (auto it = t_bars.begin(); it != t_bars.end();)
        it = it->second.group.get() ? std::next(it) : t_bars.erase(it);

    UpdatePageClips();
    UpdateSinkTrim();

    // Hiding toolbar slides top strip out of view and hides window controls in tandem.
    // Revealing toolbar on hover / sidebar open / new tab restores window controls.
    bool wantControls = !g_hideToolbar.load() || show || anyFocused;
    bool hideControls = g_hideTrafficLights.load() || !wantControls;
    for (auto h : FindNodes(L"MinimizeButton")) ApplyCollapsed(h, hideControls);
    for (auto h : FindNodes(L"MaximizeButton")) ApplyCollapsed(h, hideControls);
    for (auto h : FindNodes(L"CloseButton")) ApplyCollapsed(h, hideControls);
    if (!hideControls) {
        // Dia 1.7 can paint the page layer above the window buttons; put them back on top.
        for (auto bh : FindNodes(L"NavigationBarHost")) {
            auto bar = Obj<mux::FrameworkElement>(bh);
            if (!bar || bar.ActualHeight() <= 1) continue;
            auto root = bar.XamlRoot();
            for (auto name : {L"MinimizeButton", L"MaximizeButton", L"CloseButton"})
                for (auto ch : FindNodes(name)) {
                    auto fe = Obj<mux::FrameworkElement>(ch);
                    if (fe && fe.XamlRoot() == root) {
                        try {
                            KeepCaptionAbove(fe, bar);
                        } catch (...) {}
                    }
                }
        }
    }
}

// --- Square corners targeting SharedSurface & ContentHost (not OS desktop window)
struct CornerState {
    winrt::weak_ref<mux::FrameworkElement> el;
    mux::CornerRadius saved;
};
static thread_local std::vector<CornerState> t_corners;

static mux::CornerRadius GetRadius(const mux::FrameworkElement& fe, bool& ok) {
    ok = true;
    if (auto b = fe.try_as<muxc::Border>()) return b.CornerRadius();
    if (auto g = fe.try_as<muxc::Grid>()) return g.CornerRadius();
    if (auto c = fe.try_as<muxc::Control>()) return c.CornerRadius();
    if (auto cp = fe.try_as<muxc::ContentPresenter>()) return cp.CornerRadius();
    ok = false;
    return {};
}

static void SetRadius(const mux::FrameworkElement& fe, mux::CornerRadius r) {
    if (auto b = fe.try_as<muxc::Border>()) b.CornerRadius(r);
    else if (auto g = fe.try_as<muxc::Grid>()) g.CornerRadius(r);
    else if (auto c = fe.try_as<muxc::Control>()) c.CornerRadius(r);
    else if (auto cp = fe.try_as<muxc::ContentPresenter>()) cp.CornerRadius(r);
}

static bool SmallRounded(const mux::CornerRadius& r) {
    double m = (std::max)((std::max)(r.TopLeft, r.TopRight), (std::max)(r.BottomLeft, r.BottomRight));
    return m > 0 && m <= 10;
}

static void SquareUnder(const mux::DependencyObject& d, int depth) {
    if (!d || depth > 9) return;
    if (auto fe = d.try_as<mux::FrameworkElement>()) {
        if (fe.Name() == L"NavigationChromeHost" || fe.Name() == L"FindInPageHost") return;
        bool ok = false;
        auto r = GetRadius(fe, ok);
        if (ok && SmallRounded(r)) {
            t_corners.push_back({winrt::make_weak(fe), r});
            SetRadius(fe, {0, 0, 0, 0});
        }
    }
    int n = muxm::VisualTreeHelper::GetChildrenCount(d);
    for (int i = 0; i < n; i++) SquareUnder(muxm::VisualTreeHelper::GetChild(d, i), depth + 1);
}

struct PageStretch {
    winrt::weak_ref<mux::FrameworkElement> host;
    mux::Thickness saved{};
};
static thread_local std::vector<PageStretch> t_stretch;
static thread_local std::vector<PageStretch> t_statusLift;  // link-hover bubbles raised above the overhang
static const double kCornerOverhang = 9.0;

// The link address bubble sits inside the stretched page, so the part hanging
// past the window's bottom edge would be clipped: raise it by that overhang.
static void LiftStatusBubbles(const std::vector<std::pair<mux::FrameworkElement, double>>& overhangs) {
    for (auto h : FindNodes(L"StatusText")) {
        auto tb = Obj<mux::FrameworkElement>(h);
        if (!tb) continue;
        auto bubble = muxm::VisualTreeHelper::GetParent(tb).try_as<mux::FrameworkElement>();
        if (!bubble) continue;
        double lift = 0;
        for (mux::DependencyObject d = bubble; d; d = muxm::VisualTreeHelper::GetParent(d)) {
            auto it = std::find_if(overhangs.begin(), overhangs.end(), [&](auto const& o) { return o.first == d; });
            if (it != overhangs.end()) {
                lift = it->second;
                break;
            }
        }
        PageStretch* st = nullptr;
        for (auto& p : t_statusLift)
            if (p.host.get() == bubble) st = &p;
        if (!st) {
            if (lift < 0.5) continue;
            t_statusLift.push_back({winrt::make_weak(bubble), bubble.Margin()});
            st = &t_statusLift.back();
        }
        auto m = st->saved;
        m.Bottom += lift;
        auto cur = bubble.Margin();
        if (std::abs(cur.Bottom - m.Bottom) >= 0.5) bubble.Margin(m);
    }
    t_statusLift.erase(std::remove_if(t_statusLift.begin(), t_statusLift.end(),
                                      [](const PageStretch& p) { return !p.host.get(); }),
                       t_statusLift.end());
}

static void StretchPage(bool on) {
    if (!on) {
        for (auto& p : t_stretch)
            if (auto fe = p.host.get()) fe.Margin(p.saved);
        t_stretch.clear();
        for (auto& p : t_statusLift)
            if (auto fe = p.host.get()) fe.Margin(p.saved);
        t_statusLift.clear();
        return;
    }
    if (t_sliding) return;
    std::vector<std::pair<mux::FrameworkElement, double>> overhangs;
    for (auto h : FindNodes(L"ContentHost")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        if (!fe || fe.ActualHeight() < 1) continue;
        PageStretch* st = nullptr;
        for (auto& p : t_stretch)
            if (p.host.get() == fe) st = &p;
        if (!st) {
            t_stretch.push_back({winrt::make_weak(fe), fe.Margin()});
            st = &t_stretch.back();
        }
        auto root = fe.XamlRoot() ? fe.XamlRoot().Content().try_as<mux::FrameworkElement>() : nullptr;
        if (!root) continue;
        double bottom = fe.TransformToVisual(root).TransformPoint({0, 0}).Y + fe.ActualHeight();
        double delta = root.ActualHeight() + kCornerOverhang - bottom;
        if (std::abs(delta) >= 0.5) {
            auto old = fe.Margin();
            auto m = old;
            m.Bottom -= delta;
            if (m.Bottom > st->saved.Bottom) m.Bottom = st->saved.Bottom;
            fe.Margin(m);
            bottom += old.Bottom - m.Bottom;
        }
        overhangs.emplace_back(fe, (std::max)(0.0, bottom - root.ActualHeight()));
    }
    LiftStatusBubbles(overhangs);
}

static void UpdateSquareCorners() {
    // Restore any existing page stretches so bottom strip (profile/status bar) is completely intact
    for (auto& p : t_stretch)
        if (auto fe = p.host.get()) fe.Margin(p.saved);
    t_stretch.clear();

    bool on = g_squareSidebarCorners.load();

    for (auto h : FindNodes(L"ContentGroup")) {
        if (auto cg = Obj<muxc::Grid>(h)) {
            try {
                if (on) {
                    cg.CornerRadius(mux::CornerRadius{0, 0, 0, 0});
                } else {
                    cg.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
                }
            } catch (...) {}
        }
    }

    if (!on) {
        for (auto& c : t_corners)
            if (auto fe = c.el.get()) SetRadius(fe, c.saved);
        t_corners.clear();
        return;
    }
    for (auto& c : t_corners)
        if (auto fe = c.el.get()) {
            bool ok = false;
            if (SmallRounded(GetRadius(fe, ok))) SetRadius(fe, {0, 0, 0, 0});
        }
    for (auto h : FindNodes(L"SharedSurface")) {
        auto fe = Obj<mux::FrameworkElement>(h);
        bool ok = false;
        if (fe && SmallRounded(GetRadius(fe, ok))) {
            t_corners.push_back({winrt::make_weak(fe), GetRadius(fe, ok)});
            SetRadius(fe, {0, 0, 0, 0});
        }
    }
    for (auto h : FindNodes(L"ContentHost")) SquareUnder(Obj<mux::DependencyObject>(h), 0);
    t_corners.erase(std::remove_if(t_corners.begin(), t_corners.end(), [](const CornerState& c) { return !c.el.get(); }),
                    t_corners.end());
}

// --- Window Material Backdrop
static std::vector<InstanceHandle> BrowserRoots() {
    std::vector<InstanceHandle> roots;
    for (auto sb : FindNodes(L"SidebarBorder")) {
        InstanceHandle p = sb, last = sb;
        while (p) {
            last = p;
            p = Engine_Parent(p);
        }
        if (Engine_Type(last) == L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource" &&
            std::find(roots.begin(), roots.end(), last) == roots.end())
            roots.push_back(last);
    }
    return roots;
}

static muxc::Panel WindowBackgroundPanel(InstanceHandle root) {
    InstanceHandle found = 0;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (found || r.type != L"Microsoft.UI.Xaml.Controls.Grid") return;
        InstanceHandle up = Engine_Parent(r.handle);
        if (Engine_Name(up) != L"ClientAreaPresenter") return;
        InstanceHandle p = up, last = up;
        while (p) {
            last = p;
            p = Engine_Parent(p);
        }
        if (last == root) found = r.handle;
    });
    return Obj<muxc::Panel>(found);
}

struct MaterialState {
    winrt::weak_ref<winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource> src;
    winrt::weak_ref<muxc::Panel> panel;
    muxm::Brush savedBrush{nullptr};
    muxm::SolidColorBrush tintBrush{nullptr};
    winrt::Windows::UI::Color tint{};
    std::wstring applied;
    InstanceHandle root = 0;
};
static thread_local std::vector<MaterialState> t_material;

// Dia tints each space/profile with a gradient on SidebarFloatingTint. Reuse
// that colour as the material tint so the backdrop follows the active theme
// instead of staying the neutral grey Dia paints behind the chrome.
static bool ThemeTintColor(winrt::Windows::UI::Color& out) {
    for (auto h : FindNodes(L"SidebarFloatingTint")) {
        auto border = Obj<muxc::Border>(h);
        if (!border) continue;
        try {
            auto bg = border.Background();
            std::vector<winrt::Windows::UI::Color> stops;
            if (auto sb = bg.try_as<muxm::SolidColorBrush>()) {
                stops.push_back(sb.Color());
            } else if (auto gb = bg.try_as<muxm::GradientBrush>()) {
                for (auto const& stop : gb.GradientStops()) stops.push_back(stop.Color());
            }
            bool found = false;
            int bestSat = -1;
            winrt::Windows::UI::Color best{};
            for (auto c : stops) {
                if (c.A < 0x40) continue;
                int sat = (std::max)({c.R, c.G, c.B}) - (std::min)({c.R, c.G, c.B});
                if (sat > bestSat) {
                    bestSat = sat;
                    best = c;
                    found = true;
                }
            }
            if (found) {
                out = best;
                return true;
            }
        } catch (...) {}
    }
    try {
        // No Dia palette yet: use the Windows accent rather than a grey wash.
        static winrt::Windows::UI::ViewManagement::UISettings settings;
        out = settings.GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        return out.A != 0;
    } catch (...) {}
    return false;
}

// The tint has to stay translucent: Mica/Acrylic still provide the backdrop,
// this brush only adds the space/profile colour on top of it.
static constexpr uint8_t kMaterialTintAlpha = 0x66;
// "Translucent sidebar" asks for the Settings window's see-through look while
// the sidebar is open, so the tint thins out there but keeps the same RGB (the
// space colour) and stays stronger when the sidebar is hidden. The Tune-ups
// slider decides how far it thins out (100 % = this floor).
static constexpr uint8_t kTranslucentTintAlpha = 0x00;

static std::wstring DescribeBrush(const muxm::Brush& brush);

// Tune-ups "Translucency" slider: 0 % keeps the space colour, 100 % leaves
// just the backdrop while the sidebar is open. With the sidebar hidden the
// tint stays at its full strength so the chrome does not wash out.
static uint8_t MaterialTintAlpha(bool sidebarOpen) {
    if (!sidebarOpen) return kMaterialTintAlpha;
    int transparency = g_materialTransparency.load();
    if (transparency < 0) transparency = 0;
    if (transparency > 100) transparency = 100;
    int alpha = (static_cast<int>(kMaterialTintAlpha) * (100 - transparency) +
                 static_cast<int>(kTranslucentTintAlpha) * transparency + 50) /
                100;
    if (alpha < 0) alpha = 0;
    if (alpha > 0xFF) alpha = 0xFF;
    return static_cast<uint8_t>(alpha);
}

static void UpdateWindowMaterial() {
    std::wstring kind = g_cfg.windowMaterial;
    bool on = !kind.empty() && kind != L"none" && kind != L"default";

    for (auto root : BrowserRoots()) {
        bool known = false;
        for (auto& m : t_material) {
            if (m.root == root) { known = true; break; }
        }
        if (known) continue;
        MaterialState m;
        m.root = root;
        m.src = winrt::make_weak(Obj<winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource>(root));
        t_material.push_back(m);
    }

    for (auto& m : t_material) {
        auto src = m.src.get();
        if (!src) continue;

        std::wstring want = on ? kind : L"";
        bool sidebarOpen = g_translucentSidebar.load() && IsSidebarOpen();
        // Requested "more transparent" with a visible sidebar: Mica's darkened
        // wallpaper is too subtle there, so use Acrylic for that state while the
        // chosen material still governs the rest.
        std::wstring effective = want;
        if (sidebarOpen && (want == L"mica" || want == L"micaalt" || want == L"micaAlt"))
            effective = L"acrylic";
        auto panel = m.panel.get();
        if (!panel) {
            panel = WindowBackgroundPanel(m.root);
            if (panel) {
                m.panel = winrt::make_weak(panel);
                m.savedBrush = panel.Background();
            }
        }

        if (m.applied != effective) {
            if (effective.empty()) {
                src.SystemBackdrop(nullptr);
                if (panel) panel.Background(m.savedBrush);
            } else {
                if (effective == L"acrylic") {
                    src.SystemBackdrop(muxm::DesktopAcrylicBackdrop());
                } else {
                    muxm::MicaBackdrop mica;
                    if (effective == L"micaalt" || effective == L"micaAlt") {
                        mica.Kind(winrt::Microsoft::UI::Composition::SystemBackdrops::MicaKind::BaseAlt);
                    }
                    src.SystemBackdrop(mica);
                }
            }
            m.applied = effective;
            Log(L"windowMaterial: %s", effective.empty() ? L"off" : effective.c_str());
        }

        if (on && panel) {
            winrt::Windows::UI::Color color{};
            if (ThemeTintColor(color)) {
                color.A = MaterialTintAlpha(sidebarOpen);
                if (!m.tintBrush) m.tintBrush = muxm::SolidColorBrush(color);
                else if (m.tintBrush.Color() != color) m.tintBrush.Color(color);
                m.tint = color;
                if (panel.Background() != m.tintBrush) panel.Background(m.tintBrush);
            } else if (panel.Background() != m.savedBrush) {
                // No colour available: keep the backdrop visible without a tint.
                panel.Background(nullptr);
            }
        }
    }

    if (!on)
        t_material.erase(std::remove_if(t_material.begin(), t_material.end(),
                                        [](const MaterialState& m) { return m.applied.empty(); }),
                         t_material.end());
}

// --- Translucent sidebar
// Dia paints the docked sidebar with an opaque gradient (its space colour).
// Fading that brush in place keeps the colour but lets the window material
// (Mica/Acrylic) show through, matching the translucent Settings window.
static constexpr uint8_t kSidebarPaintAlpha = 0x59;

static muxm::Brush GetElBackground(const mux::DependencyObject& d) {
    try {
        if (auto b = d.try_as<muxc::Border>()) return b.Background();
        if (auto g = d.try_as<muxc::Grid>()) return g.Background();
        if (auto p = d.try_as<muxc::Panel>()) return p.Background();
        if (auto c = d.try_as<muxc::ContentControl>()) return c.Background();
        if (auto cp = d.try_as<muxc::ContentPresenter>()) return cp.Background();
        if (auto r = d.try_as<muxsh::Rectangle>()) return r.Fill();
        if (auto pa = d.try_as<muxsh::Path>()) return pa.Fill();
        if (auto e = d.try_as<muxsh::Ellipse>()) return e.Fill();
    } catch (...) {}
    return nullptr;
}

static void SetElBackground(const mux::DependencyObject& d, const muxm::Brush& brush) {
    if (auto b = d.try_as<muxc::Border>()) { b.Background(brush); return; }
    if (auto g = d.try_as<muxc::Grid>()) { g.Background(brush); return; }
    if (auto p = d.try_as<muxc::Panel>()) { p.Background(brush); return; }
    if (auto c = d.try_as<muxc::ContentControl>()) { c.Background(brush); return; }
    if (auto cp = d.try_as<muxc::ContentPresenter>()) { cp.Background(brush); return; }
    if (auto r = d.try_as<muxsh::Rectangle>()) { r.Fill(brush); return; }
    if (auto pa = d.try_as<muxsh::Path>()) { pa.Fill(brush); return; }
    if (auto e = d.try_as<muxsh::Ellipse>()) { e.Fill(brush); return; }
}

static std::wstring DescribeBrush(const muxm::Brush& brush) {
    if (!brush) return L"null";
    wchar_t buf[96]{};
    try {
        if (auto sb = brush.try_as<muxm::SolidColorBrush>()) {
            auto c = sb.Color();
            swprintf_s(buf, L"solid #%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
            return buf;
        }
        if (auto gb = brush.try_as<muxm::GradientBrush>()) {
            std::wstring out = brush.try_as<muxm::LinearGradientBrush>() ? L"linear" : L"gradient";
            for (auto const& st : gb.GradientStops()) {
                auto c = st.Color();
                swprintf_s(buf, L" [%.2f #%02X%02X%02X%02X]", st.Offset(), c.A, c.R, c.G, c.B);
                out += buf;
            }
            return out;
        }
        return std::wstring(L"other:") + std::wstring(winrt::get_class_name(brush));
    } catch (...) {}
    return L"other";
}

static bool BrushHasOpaqueStop(const muxm::Brush& brush) {
    try {
        if (auto sb = brush.try_as<muxm::SolidColorBrush>()) return sb.Color().A >= 0x80;
        if (auto gb = brush.try_as<muxm::GradientBrush>())
            for (auto const& st : gb.GradientStops())
                if (st.Color().A >= 0x80) return true;
    } catch (...) {}
    return false;
}

static uint32_t BrushSignature(const muxm::Brush& brush) {
    uint32_t h = 2166136261u;
    try {
        auto mix = [&](winrt::Windows::UI::Color c, double offset) {
            uint32_t v = (static_cast<uint32_t>(c.A) << 24) | (static_cast<uint32_t>(c.R) << 16) |
                         (static_cast<uint32_t>(c.G) << 8) | c.B;
            h = (h ^ v) * 16777619u;
            h = (h ^ static_cast<uint32_t>(offset * 1000)) * 16777619u;
        };
        if (auto sb = brush.try_as<muxm::SolidColorBrush>()) {
            mix(sb.Color(), 0);
        } else if (auto gb = brush.try_as<muxm::GradientBrush>()) {
            for (auto const& st : gb.GradientStops()) mix(st.Color(), st.Offset());
        }
    } catch (...) {}
    return h;
}

static muxm::Brush CloneBrushWithAlpha(const muxm::Brush& src, uint8_t alpha) {
    if (!src) return nullptr;
    try {
        if (auto sb = src.try_as<muxm::SolidColorBrush>()) {
            auto c = sb.Color();
            c.A = alpha;
            return muxm::SolidColorBrush(c);
        }
        if (auto lg = src.try_as<muxm::LinearGradientBrush>()) {
            muxm::LinearGradientBrush out;
            out.StartPoint(lg.StartPoint());
            out.EndPoint(lg.EndPoint());
            out.MappingMode(lg.MappingMode());
            out.SpreadMethod(lg.SpreadMethod());
            for (auto const& st : lg.GradientStops()) {
                auto c = st.Color();
                c.A = alpha;
                muxm::GradientStop copy;
                copy.Color(c);
                copy.Offset(st.Offset());
                out.GradientStops().Append(copy);
            }
            return out;
        }
    } catch (...) {}
    return nullptr;
}

struct SidebarPaintState {
    winrt::weak_ref<mux::DependencyObject> el;
    muxm::Brush saved{nullptr};
    muxm::Brush applied{nullptr};
    uint32_t signature = 0;
    bool valid = false;
};
static thread_local SidebarPaintState t_sidebarPaint;

static void UpdateSidebarTranslucency() {
    bool on = g_translucentSidebar.load();

    if (!on) {
        if (t_sidebarPaint.valid) {
            if (auto el = t_sidebarPaint.el.get()) {
                try { SetElBackground(el, t_sidebarPaint.saved); } catch (...) {}
            }
            t_sidebarPaint = {};
        }
        return;
    }

    auto el = t_sidebarPaint.el.get();
    if (!el || !t_sidebarPaint.valid) {
        // Dia paints the docked sidebar with the window backdrop, so there is
        // usually no local brush to fade. Scan occasionally (not every tick) in
        // case a floating sidebar paints one.
        static thread_local ULONGLONG s_nextScan = 0;
        ULONGLONG now = GetTickCount64();
        if (s_nextScan && now < s_nextScan) return;
        s_nextScan = now + 5000;

        mux::DependencyObject best{nullptr};
        double bestArea = 0;
        // The space colour is usually a gradient on an ancestor of the sidebar
        // (SpacePagingRoot) rather than on the sidebar itself: fade the nearest
        // opaque one so the gradient stays but Mica/Acrylic shows through.
        for (auto h : FindNodes(L"SidebarBorder")) {
            for (auto p = GetParentHandle(h); p; p = GetParentHandle(p)) {
                auto fe = Obj<mux::FrameworkElement>(p);
                auto dep = Obj<mux::DependencyObject>(p);
                if (!fe || !dep) continue;
                if (fe.ActualWidth() < 100 || fe.ActualHeight() < 100) continue;
                if (fe.Visibility() != mux::Visibility::Visible) continue;
                auto bg = GetElBackground(dep);
                if (!bg) continue;
                Log(L"sidebar paint: ancestor '%s' %s %.0fx%.0f bg=%s",
                    std::wstring(fe.Name()).c_str(),
                    std::wstring(winrt::get_class_name(dep)).c_str(),
                    fe.ActualWidth(), fe.ActualHeight(), DescribeBrush(bg).c_str());
                if (BrushHasOpaqueStop(bg)) {
                    best = dep;
                    bestArea = fe.ActualWidth() * fe.ActualHeight();
                    break;
                }
            }
            if (best) break;
        }
        for (auto h : FindNodes(L"SidebarBorder")) {
            if (best) break;
            auto root = Obj<mux::DependencyObject>(h);
            if (!root) continue;
            std::vector<mux::DependencyObject> stack{root};
            int guard = 0;
            while (!stack.empty() && guard++ < 400) {
                auto cur = stack.back();
                stack.pop_back();
                auto fe = cur.try_as<mux::FrameworkElement>();
                if (!fe) continue;
                double w = fe.ActualWidth(), hgt = fe.ActualHeight();
                if (w >= 100 && hgt >= 100 && fe.Visibility() == mux::Visibility::Visible) {
                    auto bg = GetElBackground(cur);
                    if (bg && BrushHasOpaqueStop(bg)) {
                        double area = w * hgt;
                        if (area > bestArea) {
                            bestArea = area;
                            best = cur;
                        }
                    }
                }
                int count = muxm::VisualTreeHelper::GetChildrenCount(cur);
                for (int i = 0; i < count; ++i) stack.push_back(muxm::VisualTreeHelper::GetChild(cur, i));
            }
        }
        if (!best) return;
        t_sidebarPaint.el = winrt::make_weak(best);
        t_sidebarPaint.saved = GetElBackground(best);
        t_sidebarPaint.signature = BrushSignature(t_sidebarPaint.saved);
        t_sidebarPaint.applied = CloneBrushWithAlpha(t_sidebarPaint.saved, kSidebarPaintAlpha);
        t_sidebarPaint.valid = t_sidebarPaint.applied != nullptr;
        el = best;
        Log(L"sidebar paint: fading '%s' with %s",
            std::wstring(best.try_as<mux::FrameworkElement>().Name()).c_str(),
            DescribeBrush(t_sidebarPaint.applied).c_str());
    }

    if (!el || !t_sidebarPaint.valid) return;

    // Dia may swap the brush when the profile/space colour changes: follow it.
    auto current = GetElBackground(el);
    if (current != t_sidebarPaint.applied) {
        t_sidebarPaint.saved = current;
        t_sidebarPaint.signature = BrushSignature(current);
        t_sidebarPaint.applied = CloneBrushWithAlpha(current, kSidebarPaintAlpha);
        if (!t_sidebarPaint.applied) { t_sidebarPaint = {}; return; }
    } else {
        uint32_t sig = BrushSignature(t_sidebarPaint.saved);
        if (sig != t_sidebarPaint.signature) {
            t_sidebarPaint.signature = sig;
            t_sidebarPaint.applied = CloneBrushWithAlpha(t_sidebarPaint.saved, kSidebarPaintAlpha);
            if (!t_sidebarPaint.applied) { t_sidebarPaint = {}; return; }
        }
    }
    if (current != t_sidebarPaint.applied) {
        try { SetElBackground(el, t_sidebarPaint.applied); } catch (...) {}
    }
}

// --- Clean Search Box
static bool CleanBoxScope(const mux::DependencyObject& el) {
    mux::DependencyObject p = el;
    for (int i = 0; p && i < 40; i++, p = muxm::VisualTreeHelper::GetParent(p)) {
        if (auto fe = p.try_as<mux::FrameworkElement>()) {
            auto n = fe.Name();
            if (n == L"AssistantPanelHost") return false;
            if (n == L"VariantHost" || n == L"DynamicPopoverRoot" || n == L"ContentHost" || n == L"ContentGroup" || n == L"CommandBarContainerRoot") return true;
        }
    }
    return true;
}

struct CleanBoxHidden {
    winrt::weak_ref<mux::UIElement> el;
    mux::Visibility vis;
};

struct CleanBoxState {
    winrt::weak_ref<muxc::Grid> area, content;
    winrt::weak_ref<muxc::Border> card;
    int row = -1;
    mux::HorizontalAlignment halign{};
    mux::VerticalAlignment valign{};
    mux::Thickness margin{}, cardPadding{};
    mux::GridLength rowHeight{};
    double minHeight = 0;
    double cardMinHeight = 0;
    double cardMaxHeight = 0;
    double cardHeight = 0;
    winrt::weak_ref<mux::FrameworkElement> input;
    mux::Thickness inputMargin{};
    winrt::event_token sizeToken{};
    double lineCenter = 0;
    double bottomPad = 0;
    bool measured = false;
    std::vector<CleanBoxHidden> hidden;
};
static thread_local std::vector<CleanBoxState> t_cleanBoxes;

static void RestoreCleanBox(CleanBoxState& st) {
    auto a = st.area.get();
    auto c = st.content.get();
    for (auto& h : st.hidden) {
        if (auto el = h.el.get()) el.Visibility(h.vis);
    }
    if (a && c) {
        muxc::Grid::SetRow(a, st.row);
        a.HorizontalAlignment(st.halign);
        a.VerticalAlignment(st.valign);
        a.Margin(st.margin);
    }
    if (c) {
        c.MinHeight(st.minHeight);
        c.ClearValue(mux::FrameworkElement::MaxHeightProperty());
        c.ClearValue(mux::FrameworkElement::HeightProperty());
    }
    if (auto b = st.card.get()) {
        b.Padding(st.cardPadding);
        b.MinHeight(st.cardMinHeight);
        b.MaxHeight(st.cardMaxHeight);
        b.Height(st.cardHeight);
    }
    if (auto i = st.input.get()) i.Margin(st.inputMargin);
    if (a && st.sizeToken) a.SizeChanged(st.sizeToken);
    if (c && st.row >= 0 && (uint32_t)st.row < c.RowDefinitions().Size())
        c.RowDefinitions().GetAt(st.row).Height(st.rowHeight);
}

// Middle of the text's first line, from the top of the text row
static double LineCenterInRow(const mux::FrameworkElement& input) {
    for (auto name : {L"QueryIcon", L"QueryIconImage", L"QueryChatIconHost"}) {
        auto e = FindByName(input, name);
        if (!e || e.Visibility() != mux::Visibility::Visible || e.ActualHeight() <= 0) continue;
        return e.TransformToVisual(input).TransformPoint({0, (float)(e.ActualHeight() / 2)}).Y;
    }
    return 0;
}

// Room Dia leaves free at the right end of the text row
static double FreeRightInRow(const mux::FrameworkElement& input) {
    double used = 0;
    for (auto name : {L"QueryTextBoxHost", L"QueryClearButton"}) {
        auto e = FindByName(input, name);
        if (!e || e.Visibility() != mux::Visibility::Visible || e.ActualWidth() <= 0) continue;
        double right = e.TransformToVisual(input).TransformPoint({(float)e.ActualWidth(), 0}).X;
        used = (std::max)(used, right);
    }
    return used > 0 ? (std::max)(0.0, input.ActualWidth() - used) : 0;
}

static thread_local bool t_cleanPending = false;
static thread_local winrt::event_token t_cleanToken{};

static void ScheduleCleanSearchBox();

static void UpdateCleanSearchBox() {
    bool on = g_cleanSearchBox.load();
    if (!on) {
        if (!t_cleanBoxes.empty()) {
            for (auto& st : t_cleanBoxes) RestoreCleanBox(st);
            t_cleanBoxes.clear();
        }
        return;
    }
    for (auto h : FindNodes(L"ToolbarArea")) {
        auto area = Obj<muxc::Grid>(h);
        if (!area) continue;
        auto content = muxm::VisualTreeHelper::GetParent(area).try_as<muxc::Grid>();
        if (!content || content.Name() != L"CommandBarContentGrid" || !CleanBoxScope(content)) continue;
        if (!FindByName(area, L"SubmitButton")) continue;
        CleanBoxState* st = nullptr;
        for (auto& x : t_cleanBoxes)
            if (x.area.get() == area) st = &x;
        if (!st) {
            int row = muxc::Grid::GetRow(area);
            if (row <= 0 || (uint32_t)row >= content.RowDefinitions().Size()) continue;
            CleanBoxState n;
            n.area = winrt::make_weak(area);
            n.content = winrt::make_weak(content);
            n.row = row;
            n.halign = area.HorizontalAlignment();
            n.valign = area.VerticalAlignment();
            n.margin = area.Margin();
            n.minHeight = content.MinHeight();
            if (auto card = muxm::VisualTreeHelper::GetParent(content).try_as<muxc::Border>();
                card && card.Name() == L"CommandBarCard") {
                n.card = winrt::make_weak(card);
                n.cardPadding = card.Padding();
                n.cardMinHeight = card.MinHeight();
                n.cardMaxHeight = card.MaxHeight();
                n.cardHeight = card.Height();
            }
            n.rowHeight = content.RowDefinitions().GetAt(row).Height();
            t_cleanBoxes.push_back(std::move(n));
            st = &t_cleanBoxes.back();
        }
        for (auto const& c : area.Children()) {
            auto fe = c.try_as<mux::FrameworkElement>();
            if (fe && fe.Name() == L"SubmitButton") continue;
            if (c.Visibility() == mux::Visibility::Collapsed) continue;
            bool known = false;
            for (auto& [w, v] : st->hidden)
                if (w.get() == c) known = true;
            if (!known) st->hidden.push_back({winrt::make_weak(c), c.Visibility()});
            c.Visibility(mux::Visibility::Collapsed);
        }
        int textRow = 1;
        for (auto const& c : content.Children())
            if (auto fe = c.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"InputContent")
                textRow = muxc::Grid::GetRow(fe);
        if (muxc::Grid::GetRow(area) != textRow) muxc::Grid::SetRow(area, textRow);
        if (area.HorizontalAlignment() != mux::HorizontalAlignment::Right)
            area.HorizontalAlignment(mux::HorizontalAlignment::Right);
        if (area.VerticalAlignment() != mux::VerticalAlignment::Top)
            area.VerticalAlignment(mux::VerticalAlignment::Top);
        if (content.MinHeight() != 0) content.MinHeight(0);

        if (!st->input.get()) {
            for (auto const& c : content.Children())
                if (auto fe = c.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"InputContent")
                    st->input = winrt::make_weak(fe), st->inputMargin = fe.Margin();
            st->sizeToken = area.SizeChanged([](auto&&, auto&&) { ScheduleCleanSearchBox(); });
        }
        if (auto in = st->input.get(); in && in.ActualHeight() > 0 && area.ActualHeight() > 0) {
            if (double c = LineCenterInRow(in); c > 0) {
                st->lineCenter = c;
                st->bottomPad = (std::clamp)(st->cardPadding.Top + 2 * c - in.ActualHeight(), 0.0, st->cardPadding.Bottom);
                st->measured = true;
            }
        }
        if (auto in = st->input.get()) {
            double extra = (std::max)(0.0, area.ActualWidth() - FreeRightInRow(in));
            auto m = in.Margin();
            if (std::abs(m.Right - (st->inputMargin.Right + extra)) > 0.5)
                in.Margin({m.Left, m.Top, st->inputMargin.Right + extra, m.Bottom});
        }
        double top = st->measured ? (std::max)(-st->cardPadding.Top, st->lineCenter - area.ActualHeight() / 2) : 0;
        if (auto m = area.Margin(); std::abs(m.Top - top) > 0.5 || m.Bottom != 0) area.Margin({m.Left, top, m.Right, 0});
        if (auto card = st->card.get()) {
            double bottom = st->measured ? st->bottomPad : 4.0;
            if (auto pd = card.Padding(); std::abs(pd.Bottom - bottom) > 0.5)
                card.Padding({pd.Left, pd.Top, pd.Right, bottom});
            if (card.MinHeight() != 0) card.MinHeight(0);
            if (auto p = muxm::VisualTreeHelper::GetParent(card).try_as<mux::FrameworkElement>(); p && p.Name() == L"CommandBarContainerRoot") {
                if (p.MinHeight() != 0) p.MinHeight(0);
            }
        }
        if (st->row >= 0 && (uint32_t)st->row < content.RowDefinitions().Size()) {
            auto rd = content.RowDefinitions().GetAt(st->row);
            if (rd.Height().Value != 0 || rd.Height().GridUnitType != mux::GridUnitType::Pixel) {
                rd.Height(mux::GridLength{0, mux::GridUnitType::Pixel});
            }
        }
    }
}

static void ScheduleCleanSearchBox() {
    if (t_cleanPending || !g_cleanSearchBox.load()) return;
    t_cleanPending = true;
    t_cleanToken = muxm::CompositionTarget::Rendering([](auto&&, auto&&) {
        muxm::CompositionTarget::Rendering(t_cleanToken);
        t_cleanPending = false;
        try {
            UpdateCleanSearchBox();
        } catch (...) {
        }
    });
}

// --- Daily painting wallpaper for New Tab page
static int GetTodayPaintingIndex() {
    SYSTEMTIME st;
    GetLocalTime(&st);

    std::tm anchor = {};
    anchor.tm_year = 2026 - 1900;
    anchor.tm_mon = 3 - 1; // March
    anchor.tm_mday = 30;
    anchor.tm_hour = 12;

    std::tm today = {};
    today.tm_year = st.wYear - 1900;
    today.tm_mon = st.wMonth - 1;
    today.tm_mday = st.wDay;
    today.tm_hour = 12;

    std::time_t t_anchor = std::mktime(&anchor);
    std::time_t t_today = std::mktime(&today);

    double diff = std::difftime(t_today, t_anchor);
    int days = static_cast<int>((diff + 1800.0) / 86400.0);
    if (days < 0) days = 0;

    int weeks = days / 7;
    int weekday = (today.tm_wday + 6) % 7; // Mon=0 .. Sun=6
    int dayOffset = (std::min)(weekday, 4);

    return static_cast<int>((weeks * 5 + dayOffset) % kPaintingsCount);
}

static std::wstring GetDiaPackageDir() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    return exePath;
}

static muxm::ImageBrush GetOrCreatePaintingBrush(int index) {
    if (index < 0 || index >= static_cast<int>(kPaintingsCount)) index = 0;
    static int s_cachedIndex = -1;
    static muxm::ImageBrush s_cachedBrush{nullptr};

    if (s_cachedBrush && s_cachedIndex == index) {
        return s_cachedBrush;
    }

    const auto& meta = kPaintings[index];
    std::wstring pkgDir = GetDiaPackageDir();
    std::wstring imgPath = pkgDir + L"\\agent-server-resources\\dist\\agents\\clia-brief\\morning-brief\\" + meta.src;

    if (GetFileAttributesW(imgPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        wchar_t localAppData[MAX_PATH]{};
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
            std::wstring altPath = std::wstring(localAppData) + L"\\DiaTweaks\\" + meta.src;
            if (GetFileAttributesW(altPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                imgPath = altPath;
            }
        }
    }

    try {
        winrt::Windows::Foundation::Uri uri(std::wstring(L"file:///") + imgPath);
        muxm::Imaging::BitmapImage bmp(uri);
        muxm::ImageBrush brush;
        brush.ImageSource(bmp);
        brush.Stretch(muxm::Stretch::UniformToFill);
        brush.AlignmentX(muxm::AlignmentX::Center);
        brush.AlignmentY(muxm::AlignmentY::Center);
        brush.Opacity(1.0);
        s_cachedBrush = brush;
        s_cachedIndex = index;
        return s_cachedBrush;
    } catch (...) {
        return nullptr;
    }
}

struct NewTabState {
    winrt::weak_ref<muxc::Grid> contentGroup;
    winrt::weak_ref<muxc::Grid> newTabPage;
    muxm::Brush savedGroupBackground{nullptr};
    muxm::Brush savedPageBackground{nullptr};
    winrt::weak_ref<muxc::Border> captionPill;
    winrt::weak_ref<muxc::Border> scrimOverlay;
    int appliedPaintingIndex = -1;
    bool hasApplied = false;
};
static thread_local std::vector<NewTabState> t_newTabStates;

// Created lazily on the UI thread: a namespace-scope WinRT object would be
// constructed during DllMain on the loader thread, before XAML exists.
static const muxm::SolidColorBrush& ClearBrush() {
    static muxm::SolidColorBrush brush{winrt::Windows::UI::Color{0, 0, 0, 0}};
    return brush;
}

// Clearing a background hides Dia's page-adaptive toolbar palette for good:
// the site palette lives in local values/brushes, and ClearValue drops it. Keep
// each element's own value (and binding) so turning transparency or the New Tab
// painting off hands the toolbar back to Dia exactly as it was.
struct ClearSaved {
    winrt::weak_ref<mux::DependencyObject> el;
    mux::DependencyProperty dp{nullptr};
    Inspectable value{nullptr};
    muxd::BindingBase binding{nullptr};
    bool thickness = false;
    mux::Thickness savedThickness{};
};
static thread_local std::map<std::pair<void*, void*>, ClearSaved> t_clearSaved;

static void ClearBackground(const mux::DependencyObject& d, const mux::DependencyProperty& dp) {
    if (!d || !dp) return;
    auto key = std::make_pair(winrt::get_abi(d), winrt::get_abi(dp));
    auto it = t_clearSaved.find(key);
    if (it == t_clearSaved.end()) {
        ClearSaved s;
        s.el = winrt::make_weak(d);
        s.dp = dp;
        if (auto fe = d.try_as<mux::FrameworkElement>()) {
            try {
                if (auto be = fe.GetBindingExpression(dp)) s.binding = be.ParentBinding();
            } catch (...) {}
        }
        if (!s.binding) {
            try {
                auto v = d.ReadLocalValue(dp);
                if (v && v != mux::DependencyProperty::UnsetValue()) s.value = v;
            } catch (...) {}
        }
        it = t_clearSaved.emplace(key, s).first;
    } else {
        // Dia may paint a new site palette while we hide the toolbar: keep that
        // newer color for the restore instead of the stale one.
        try {
            auto v = d.ReadLocalValue(dp);
            if (v && v != mux::DependencyProperty::UnsetValue() && v != ClearBrush()) {
                it->second.value = v;
                it->second.binding = nullptr;
            }
        } catch (...) {}
    }
    try { d.SetValue(dp, ClearBrush()); } catch (...) {}
}

static void ClearThickness(const muxc::Border& b) {
    if (!b) return;
    auto key = std::make_pair(winrt::get_abi(b), winrt::get_abi(muxc::Border::BorderThicknessProperty()));
    if (!t_clearSaved.count(key)) {
        ClearSaved s;
        s.el = winrt::make_weak(b.as<mux::DependencyObject>());
        s.dp = muxc::Border::BorderThicknessProperty();
        s.thickness = true;
        s.savedThickness = b.BorderThickness();
        t_clearSaved.emplace(key, s);
    }
    try { b.BorderThickness(mux::Thickness{0, 0, 0, 0}); } catch (...) {}
}

static void RestoreClearSubtree() {
    for (auto& [key, s] : t_clearSaved) {
        auto d = s.el.get();
        if (!d) continue;
        try {
            if (s.thickness) {
                if (auto b = d.try_as<muxc::Border>()) b.BorderThickness(s.savedThickness);
            } else if (s.binding) {
                if (auto fe = d.try_as<mux::FrameworkElement>()) fe.SetBinding(s.dp, s.binding);
            } else if (s.value) {
                d.SetValue(s.dp, s.value);
            } else {
                d.ClearValue(s.dp);
            }
        } catch (...) {}
    }
    t_clearSaved.clear();
}

static void MakeSubtreeClear(const mux::DependencyObject& root, int depth = 0) {
    if (!root || depth > 12) return;
    if (auto p = root.try_as<muxc::Panel>()) {
        ClearBackground(root, muxc::Panel::BackgroundProperty());
    } else if (auto cp = root.try_as<muxc::ContentPresenter>()) {
        ClearBackground(root, muxc::ContentPresenter::BackgroundProperty());
    } else if (root.try_as<muxc::ContentControl>()) {
        ClearBackground(root, muxc::Control::BackgroundProperty());
    } else if (auto b = root.try_as<muxc::Border>()) {
        try {
            std::wstring bName(b.Name());
            if (bName != L"AssistantBar") ClearBackground(root, muxc::Border::BackgroundProperty());
            if (bName == L"BottomSeparator" || b.BorderThickness().Top > 0 || b.BorderThickness().Bottom > 0) {
                ClearThickness(b);
            }
        } catch (...) {}
    } else if (root.try_as<muxc::Control>()) {
        ClearBackground(root, muxc::Control::BackgroundProperty());
    }
    int count = muxm::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        MakeSubtreeClear(muxm::VisualTreeHelper::GetChild(root, i), depth + 1);
    }
}

static void UpdateNewTabPainting() {
    bool on = g_newTabPainting.load();
    bool isNewTab = IsNewTab();

    if (!on || !isNewTab) {
        if (!t_newTabStates.empty()) {
            for (auto& st : t_newTabStates) {
                if (auto cg = st.contentGroup.get()) {
                    try {
                        if (st.savedGroupBackground) {
                            cg.Background(st.savedGroupBackground);
                        } else {
                            cg.ClearValue(muxc::Panel::BackgroundProperty());
                        }
                        if (auto scrim = st.scrimOverlay.get()) {
                            uint32_t idx = 0;
                            if (cg.Children().IndexOf(scrim, idx)) {
                                cg.Children().RemoveAt(idx);
                            }
                        }
                    } catch (...) {}
                }
                if (auto page = st.newTabPage.get()) {
                    try {
                        if (st.savedPageBackground) {
                            page.Background(st.savedPageBackground);
                        } else {
                            page.ClearValue(muxc::Panel::BackgroundProperty());
                        }
                        if (auto pill = st.captionPill.get()) {
                            uint32_t idx = 0;
                            if (page.Children().IndexOf(pill, idx)) {
                                page.Children().RemoveAt(idx);
                            }
                        }
                    } catch (...) {}
                }
            }
            for (auto bsHandle : FindNodes(L"BottomSeparator")) {
                if (auto fe = Obj<mux::FrameworkElement>(bsHandle)) {
                    fe.Visibility(mux::Visibility::Visible);
                    fe.ClearValue(mux::FrameworkElement::HeightProperty());
                    if (auto b = fe.try_as<muxc::Border>()) {
                        b.ClearValue(muxc::Border::BorderThicknessProperty());
                    }
                }
            }
            // Hand the toolbar back to Dia's own site palette (not a solid grey).
            RestoreClearSubtree();
            t_newTabStates.clear();
        }
        return;
    }

    std::vector<muxc::Grid> livePages;
    if (auto p = t_activeNewTabPage.get()) {
        try {
            if (p.Visibility() == mux::Visibility::Visible && p.ActualWidth() >= 1 && ShownOnScreen(p)) {
                livePages.push_back(p);
            }
        } catch (...) {}
    }
    if (livePages.empty()) {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        for (const auto& [handle, node] : g_nodes) {
            if (node.type == L"Microsoft.UI.Xaml.Controls.Grid") {
                Inspectable obj{nullptr};
                if (GetObject(handle, obj)) {
                    if (auto grid = obj.try_as<muxc::Grid>()) {
                        try {
                            std::wstring autoName(muxa::AutomationProperties::GetName(grid));
                            if (autoName == L"newTabPage" && grid.Visibility() == mux::Visibility::Visible && ShownOnScreen(grid)) {
                                livePages.push_back(grid);
                                t_activeNewTabPage = winrt::make_weak(grid);
                                break;
                            }
                        } catch (...) {}
                    }
                }
            }
        }
    }
    if (livePages.empty()) return;

    int todayIndex = GetTodayPaintingIndex();
    auto brush = GetOrCreatePaintingBrush(todayIndex);
    if (!brush) return;

    const auto& meta = kPaintings[todayIndex];

    muxm::SolidColorBrush clearBrush{winrt::Windows::UI::Color{0, 0, 0, 0}};

    int tintPct = g_newTabPaintingTint.load();
    if (tintPct < 0) tintPct = 0;
    if (tintPct > 100) tintPct = 100;
    uint8_t alpha = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(tintPct * 255.0 / 100.0)), 0, 255));
    muxm::SolidColorBrush scrimBrush{winrt::Windows::UI::Color{alpha, 14, 16, 20}};

    for (auto activePage : livePages) {
        muxc::Grid contentGroup{nullptr};
        for (mux::DependencyObject p = activePage; p; p = muxm::VisualTreeHelper::GetParent(p)) {
            if (auto fe = p.try_as<mux::FrameworkElement>()) {
                if (fe.Name() == L"ContentGroup") {
                    contentGroup = fe.try_as<muxc::Grid>();
                    break;
                }
            }
        }
        if (!contentGroup) {
            for (auto h : FindNodes(L"ContentGroup")) {
                if (auto cg = Obj<muxc::Grid>(h)) {
                    if (cg.Visibility() == mux::Visibility::Visible) {
                        contentGroup = cg;
                        break;
                    }
                }
            }
        }

        NewTabState* st = nullptr;
        for (auto& s : t_newTabStates) {
            if (s.newTabPage.get() == activePage) {
                st = &s;
                break;
            }
        }

        bool alreadyApplied = (st && st->hasApplied && st->appliedPaintingIndex == todayIndex &&
                              contentGroup && contentGroup.Background() == brush);

        // Ensure tab bar and content hosts are transparent so ContentGroup's wallpaper flows through seamlessly
        for (auto ch : FindNodes(L"NavigationChromeHost")) {
            if (auto g = Obj<muxc::Grid>(ch)) {
                MakeSubtreeClear(g);
                try {
                    mux::DependencyObject parent = muxm::VisualTreeHelper::GetParent(g);
                    if (parent) {
                        if (auto pg = parent.try_as<muxc::Panel>()) {
                            ClearBackground(pg, muxc::Panel::BackgroundProperty());
                        }
                    }
                } catch (...) {}
            }
        }
        for (auto ch : FindNodes(L"NavigationBarHost")) {
            if (auto cc = Obj<muxc::ContentControl>(ch)) MakeSubtreeClear(cc);
        }
        for (auto ch : FindNodes(L"NavigationBarContent")) {
            if (auto g = Obj<muxc::Grid>(ch)) MakeSubtreeClear(g);
        }
        for (auto ch : FindNodes(L"ContentHost")) {
            if (auto cc = Obj<muxc::ContentControl>(ch)) {
                ClearBackground(cc, muxc::Control::BackgroundProperty());
            }
        }
        for (auto bsHandle : FindNodes(L"BottomSeparator")) {
            if (auto fe = Obj<mux::FrameworkElement>(bsHandle)) {
                if (fe.Visibility() != mux::Visibility::Collapsed) fe.Visibility(mux::Visibility::Collapsed);
                if (fe.Height() != 0) fe.Height(0);
            }
        }

        if (!st) {
            NewTabState newState;
            newState.newTabPage = winrt::make_weak(activePage);
            newState.savedPageBackground = activePage.Background();
            if (contentGroup) {
                newState.contentGroup = winrt::make_weak(contentGroup);
                newState.savedGroupBackground = contentGroup.Background();
            }
            t_newTabStates.push_back(newState);
            st = &t_newTabStates.back();
        }

        // Apply painting wallpaper directly to ContentGroup so it covers the FULL page behind the tab bar
        if (contentGroup) {
            if (!g_squareSidebarCorners.load()) {
                contentGroup.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
            }
            if (contentGroup.Background() != brush) {
                contentGroup.Background(brush);
            }

            // Neutral subtle greyish dark scrim over the painting across the FULL page
            muxc::Border scrim = st->scrimOverlay.get();
            if (!scrim) {
                for (uint32_t i = 0; i < contentGroup.Children().Size(); ++i) {
                    if (auto b = contentGroup.Children().GetAt(i).try_as<muxc::Border>()) {
                        try {
                            if (muxa::AutomationProperties::GetName(b) == L"DiaTweaksScrim") {
                                scrim = b;
                                break;
                            }
                        } catch (...) {}
                    }
                }
            }
            if (!scrim) {
                scrim = muxc::Border();
                muxa::AutomationProperties::SetName(scrim, L"DiaTweaksScrim");
                scrim.Background(scrimBrush);
                scrim.CornerRadius(contentGroup.CornerRadius());
                scrim.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
                scrim.VerticalAlignment(mux::VerticalAlignment::Stretch);
                scrim.IsHitTestVisible(false);
                muxc::Grid::SetRow(scrim, 0);
                muxc::Grid::SetColumn(scrim, 0);
                muxc::Grid::SetRowSpan(scrim, 99);
                muxc::Grid::SetColumnSpan(scrim, 99);
                contentGroup.Children().InsertAt(0, scrim);
                st->scrimOverlay = winrt::make_weak(scrim);
            } else {
                scrim.Background(scrimBrush);
                scrim.CornerRadius(contentGroup.CornerRadius());
            }
        }

        // Keep activePage transparent so ContentGroup's wallpaper & scrim shine through
        if (activePage.Background() != clearBrush && activePage.Background() != nullptr) {
            activePage.Background(clearBrush);
        }

        if (st->hasApplied && st->appliedPaintingIndex == todayIndex) {
            continue;
        }

        try {
            st->appliedPaintingIndex = todayIndex;
            st->hasApplied = true;

            if (auto oldPill = st->captionPill.get()) {
                uint32_t idx = 0;
                if (activePage.Children().IndexOf(oldPill, idx)) {
                    activePage.Children().RemoveAt(idx);
                }
            }

            std::wstring caption;
            if (meta.title && meta.title[0]) caption += meta.title;
            if (meta.artist && meta.artist[0]) {
                if (!caption.empty()) caption += L"  —  ";
                caption += meta.artist;
            }
            if (meta.date && meta.date[0]) {
                if (!caption.empty()) caption += L", ";
                caption += meta.date;
            }

            muxc::TextBlock tb;
            tb.Text(caption);
            tb.FontSize(11.5);
            tb.FontFamily(muxm::FontFamily(L"Segoe UI Variable"));
            tb.Foreground(muxm::SolidColorBrush(winrt::Windows::UI::Color{230, 255, 255, 255}));

            muxc::Border pill;
            pill.CornerRadius(mux::CornerRadiusHelper::FromUniformRadius(6));
            pill.Background(muxm::SolidColorBrush(winrt::Windows::UI::Color{130, 0, 0, 0}));
            pill.Padding(mux::Thickness{12, 5, 12, 5});
            pill.Child(tb);
            pill.HorizontalAlignment(mux::HorizontalAlignment::Right);
            pill.VerticalAlignment(mux::VerticalAlignment::Bottom);
            pill.Margin(mux::Thickness{0, 0, 24, 20});
            pill.IsHitTestVisible(false);
            muxa::AutomationProperties::SetName(pill, L"newTabPaintingCaption");
            muxc::Grid::SetRow(pill, 0);
            muxc::Grid::SetColumn(pill, 0);
            muxc::Grid::SetRowSpan(pill, 99);
            muxc::Grid::SetColumnSpan(pill, 99);

            activePage.Children().Append(pill);
            st->captionPill = winrt::make_weak(pill);

            Log(L"newTabPainting: applied painting #%d (%s) on full ContentGroup", todayIndex, meta.title);
        } catch (...) {
            Log(L"newTabPainting: exception applying painting #%d", todayIndex);
        }
    }
}

// --- Don't Pop Up Downloads
static thread_local std::set<InstanceHandle> t_seenPopups;

static void UpdateDownloadsPopup() {
    if (!g_dontPopUpDownloads.load()) {
        t_seenPopups.clear();
        return;
    }
    LASTINPUTINFO li{sizeof(li)};
    GetLastInputInfo(&li);
    bool userJustActed = GetTickCount() - li.dwTime < 600;
    Engine_ForEachNode([&](const NodeRef& r) {
        if (r.type != L"Microsoft.UI.Xaml.Controls.Primitives.Popup" || t_seenPopups.count(r.handle)) return;
        auto popup = Obj<muxc::Primitives::Popup>(r.handle);
        if (!popup || !popup.IsOpen()) return;
        t_seenPopups.insert(r.handle);
        auto child = popup.Child();
        if (!child) return;
        auto dl = FindVisual(child, [](const mux::DependencyObject& d) {
            auto fe = d.try_as<mux::FrameworkElement>();
            if (!fe) return false;
            std::wstring n(fe.Name());
            std::wstring t(winrt::get_class_name(fe));
            return n.find(L"Download") != std::wstring::npos || t.find(L"Download") != std::wstring::npos;
        });
        if (dl && !userJustActed) {
            popup.IsOpen(false);
            Log(L"closed downloads pop-up");
        }
    });
}

// --- Hide Pinned Tab Border
struct PinnedBorderSaved {
    winrt::weak_ref<muxc::Border> border;
    mux::Thickness savedThickness;
};
static thread_local std::vector<PinnedBorderSaved> t_pinnedBorders;

static void UpdatePinnedTabBorder() {
    bool on = g_hidePinnedTabBorder.load();
    if (!on) {
        for (auto& s : t_pinnedBorders) {
            if (auto b = s.border.get()) b.BorderThickness(s.savedThickness);
        }
        t_pinnedBorders.clear();
        return;
    }
    for (auto dh : FindNodes(L"DockHost")) {
        auto host = Obj<mux::DependencyObject>(dh);
        if (!host) continue;
        std::function<void(const mux::DependencyObject&, int)> walk = [&](const mux::DependencyObject& d, int depth) {
            if (!d || depth > 8) return;
            if (auto b = d.try_as<muxc::Border>()) {
                auto th = b.BorderThickness();
                if (th.Left > 0 || th.Top > 0 || th.Right > 0 || th.Bottom > 0) {
                    bool known = false;
                    for (auto& s : t_pinnedBorders) {
                        if (s.border.get() == b) { known = true; break; }
                    }
                    if (!known) {
                        t_pinnedBorders.push_back({winrt::make_weak(b), th});
                    }
                    b.BorderThickness({0, 0, 0, 0});
                }
            }
            int n = muxm::VisualTreeHelper::GetChildrenCount(d);
            for (int i = 0; i < n; i++) walk(muxm::VisualTreeHelper::GetChild(d, i), depth + 1);
        };
        walk(host, 0);
    }
}

// ---------------------------------------------------------------- Arc pinned tabs
// Ported from zioder/WinDiaTweaks (after JaceThings/DiaTweaks' Arc pinned tabs).
// On hover, a pinned tab that has left its pinned page shows a small button at its right end:
// "-" takes it back (Dia's own "Back to Pinned URL"); one still on its page shows "x" (Dia's
// own Close tab). We never add a button into Dia's row (that broke Back to Pinned URL); the
// button lives in a Popup overlaid on the row's right end and presses Dia's own button on click.

// Presses a button even when collapsed: its host is shown (invisibly) for one layout pass.
static bool PressPinnedButton(const muxc::Button& b, const char* what) {
    if (!b) return false;
    mux::UIElement hidden{nullptr};
    for (mux::DependencyObject d = b; d; d = muxm::VisualTreeHelper::GetParent(d))
        if (auto fe = d.try_as<mux::FrameworkElement>(); fe && fe.Visibility() == mux::Visibility::Collapsed) {
            hidden = fe;
            break;
        }
    double op = hidden ? hidden.Opacity() : 1;
    bool ok = false;
    try {
        if (hidden) hidden.Opacity(0), hidden.Visibility(mux::Visibility::Visible), hidden.UpdateLayout();
        mux::Automation::Peers::ButtonAutomationPeer(b).Invoke();
        ok = true;
    } catch (...) {
        Log(L"arc pinned tabs: could not press %hs button", what);
    }
    if (hidden) hidden.Visibility(mux::Visibility::Collapsed), hidden.Opacity(op);
    return ok;
}

// Every tab row in the list (including those in groups): Dia's row is a UserControl whose grid
// has five columns (icon, ..., title, close button).
static std::vector<mux::FrameworkElement> ArcPinnedAllTabRows() {
    std::vector<mux::FrameworkElement> out;
    for (auto h : FindNodes(L"RowsPanel"))
        if (auto rows = Obj<mux::FrameworkElement>(h); rows && rows.ActualHeight() > 0)
            FirstIn(rows, [&](const mux::FrameworkElement& fe) {
                if (auto uc = fe.try_as<muxc::UserControl>())
                    if (auto g = uc.Content().try_as<muxc::Grid>(); g && g.ColumnDefinitions().Size() == 5)
                        if (std::find(out.begin(), out.end(), fe) == out.end()) out.push_back(fe);
                return false;
            });
    return out;
}

struct ArcPinnedRow {
    winrt::weak_ref<muxc::UserControl> row;
    winrt::weak_ref<muxc::Grid> grid;
    winrt::event_token entered{}, exited{}, canceled{};
    bool pinned = false, inFolder = false;
};
static thread_local std::vector<ArcPinnedRow> t_arcPinnedRows;

static thread_local struct {
    muxc::Primitives::Popup popup{nullptr};
    muxc::Button button{nullptr};
    muxc::FontIcon icon{nullptr};
    winrt::weak_ref<muxc::Grid> grid;
    winrt::weak_ref<muxc::Button> held;
    mux::Visibility heldVisibility = mux::Visibility::Collapsed;
    int64_t heldToken = 0;
    bool away = false;
    bool overRow = false, overButton = false;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
} t_arcPin;

static muxc::Button ArcPinnedRowButton(const muxc::Grid& g, const wchar_t* name, bool visibleOnly) {
    for (auto const& c : g.Children())
        if (auto b = c.try_as<muxc::Button>()) {
            if (visibleOnly && b.Visibility() != mux::Visibility::Visible) continue;
            if (muxa::AutomationProperties::GetName(b) == name) return b;
        }
    return nullptr;
}

static ArcPinnedRow* ArcPinnedRowFor(const muxc::Grid& g) {
    for (auto& r : t_arcPinnedRows)
        if (g && r.grid.get() == g) return &r;
    return nullptr;
}

static void ArcReleaseDiaClose() {
    if (auto c = t_arcPin.held.get()) {
        c.UnregisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), t_arcPin.heldToken);
        c.Opacity(1), c.IsHitTestVisible(true);
        c.Visibility(t_arcPin.heldVisibility);
    }
    t_arcPin.held = nullptr;
    t_arcPin.heldToken = 0;
}

static void ArcHoldDiaClose(const muxc::Button& close) {
    if (t_arcPin.held.get() == close) return;
    ArcReleaseDiaClose();
    t_arcPin.heldVisibility = close.Visibility();
    close.Opacity(0), close.IsHitTestVisible(false);
    close.Visibility(mux::Visibility::Visible);
    t_arcPin.held = winrt::make_weak(close);
    t_arcPin.heldToken = close.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), [](auto const& sender, auto&&) {
        if (auto c = sender.template try_as<muxc::Button>(); c && t_arcPin.held.get() == c && c.Visibility() != mux::Visibility::Visible)
            c.Visibility(mux::Visibility::Visible);
    });
}

static void ArcHidePinButton() {
    ArcReleaseDiaClose();
    if (t_arcPin.popup && t_arcPin.popup.IsOpen()) t_arcPin.popup.IsOpen(false);
    t_arcPin.grid = nullptr;
    t_arcPin.overRow = t_arcPin.overButton = false;
}

static void ArcPressPinButton() {
    auto g = t_arcPin.grid.get();
    bool away = t_arcPin.away;
    ArcHidePinButton();
    if (!g) return;
    if (away) {
        if (auto back = ArcPinnedRowButton(g, L"Back to Pinned URL", false))
            PressPinnedButton(back, "back to pinned");
    } else {
        PressPinnedButton(ArcPinnedRowButton(g, L"Close tab", false), "close pinned");
    }
}

static void ArcMakePinButton() {
    if (t_arcPin.popup) return;
    t_arcPin.icon = muxc::FontIcon();
    t_arcPin.icon.FontSize(10);
    t_arcPin.button = muxc::Button();
    t_arcPin.button.Name(L"DiaTweaksPinnedButton");
    t_arcPin.button.Content(t_arcPin.icon);
    t_arcPin.button.Click([](auto&&, auto&&) {
        try { ArcPressPinButton(); } catch (...) {}
    });
    t_arcPin.button.PointerEntered([](auto&&, auto&&) { t_arcPin.overButton = true; });
    t_arcPin.button.PointerExited([](auto&&, auto&&) {
        t_arcPin.overButton = false;
        if (t_arcPin.timer) t_arcPin.timer.Start();
    });
    t_arcPin.popup = muxc::Primitives::Popup();
    t_arcPin.popup.ShouldConstrainToRootBounds(false);
    t_arcPin.popup.Child(t_arcPin.button);
    t_arcPin.timer = t_arcPin.button.DispatcherQueue().CreateTimer();
    t_arcPin.timer.Interval(std::chrono::milliseconds(80));
    t_arcPin.timer.IsRepeating(false);
    t_arcPin.timer.Tick([](auto&&, auto&&) {
        try { if (!t_arcPin.overRow && !t_arcPin.overButton) ArcHidePinButton(); } catch (...) {}
    });
}

static void ArcStylePinButton(bool dark) {
    static thread_local int styled = -1;
    if (styled == (int)dark) return;
    styled = dark;
    auto& b = t_arcPin.button;
    auto brush = [](uint8_t a, uint8_t v) { return muxm::SolidColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(a, v, v, v)); };
    uint8_t ink = dark ? 255 : 0;
    auto res = b.Resources();
    auto put = [&](const wchar_t* key, const muxm::SolidColorBrush& v) { res.Insert(winrt::box_value(key), v); };
    put(L"ButtonBackground", brush(0, ink));
    put(L"ButtonBackgroundPointerOver", brush(dark ? 0x26 : 0x14, ink));
    put(L"ButtonBackgroundPressed", brush(dark ? 0x1a : 0x0e, ink));
    put(L"ButtonBorderBrush", brush(0, ink));
    put(L"ButtonBorderBrushPointerOver", brush(0, ink));
    put(L"ButtonBorderBrushPressed", brush(0, ink));
    put(L"ButtonForeground", brush(dark ? 0xcc : 0xb3, ink));
    put(L"ButtonForegroundPointerOver", brush(0xff, ink));
    put(L"ButtonForegroundPressed", brush(dark ? 0xb3 : 0x99, ink));
    b.Background(brush(0, ink));
    b.BorderThickness({0, 0, 0, 0});
    b.Padding({0, 0, 0, 0});
    b.CornerRadius({4, 4, 4, 4});
    b.MinWidth(0), b.MinHeight(0);
    b.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    b.VerticalContentAlignment(mux::VerticalAlignment::Center);
    b.RequestedTheme(dark ? mux::ElementTheme::Dark : mux::ElementTheme::Light);
    t_arcPin.icon.FontFamily(muxm::FontFamily(L"Segoe Fluent Icons, Segoe MDL2 Assets"));
}

static bool ArcPlacePinButton(const ArcPinnedRow& r) {
    auto g = r.grid.get();
    auto row = r.row.get();
    if (!g || !row || !row.IsLoaded() || !r.pinned || g.ActualWidth() <= 0 || !g.XamlRoot()) return false;
    auto close = ArcPinnedRowButton(g, L"Close tab", false);
    if (!close) return false;
    bool away = ArcPinnedRowButton(g, L"Back to Pinned URL", true) != nullptr;
    if (!away && r.inFolder) return false;
    ArcMakePinButton();
    t_arcPin.grid = winrt::make_weak(g);
    bool wasHeld = t_arcPin.held.get() == close;
    ArcHoldDiaClose(close);
    if (!wasHeld) g.UpdateLayout();
    auto& b = t_arcPin.button;
    double w = close.ActualWidth() > 0 ? close.ActualWidth() : 22, h = close.ActualHeight() > 0 ? close.ActualHeight() : 22;
    if (b.Width() != w) b.Width(w), b.Height(h);
    ArcStylePinButton(g.ActualTheme() != mux::ElementTheme::Light);
    winrt::hstring glyph = away ? L"\uE173" : L"\uE711";  // Remove (-) / Cancel (x), as Dia's
    if (t_arcPin.icon.Glyph() != glyph) {
        t_arcPin.icon.Glyph(glyph);
        const wchar_t* label = away ? L"Back to Pinned URL" : L"Close tab";
        muxa::AutomationProperties::SetName(b, label);
        muxc::ToolTipService::SetToolTip(b, winrt::box_value(label));
    }
    t_arcPin.away = away;
    double x, y;
    if (close.ActualWidth() > 0) {
        auto c = close.TransformToVisual(nullptr).TransformPoint({0, 0});
        x = c.X, y = c.Y;
    } else {
        auto o = g.TransformToVisual(nullptr).TransformPoint({0, 0});
        x = o.X + g.ActualWidth() - g.Padding().Right - close.Margin().Right - w;
        y = o.Y + (g.ActualHeight() - h) / 2;
    }
    auto& p = t_arcPin.popup;
    if (p.XamlRoot() != g.XamlRoot()) p.IsOpen(false), p.XamlRoot(g.XamlRoot());
    if (std::abs(p.HorizontalOffset() - x) > 0.5) p.HorizontalOffset(x);
    if (std::abs(p.VerticalOffset() - y) > 0.5) p.VerticalOffset(y);
    if (!p.IsOpen()) p.IsOpen(true);
    return true;
}

static void ArcPinnedRowHover(const winrt::weak_ref<muxc::Grid>& wg, bool on) {
    auto g = wg.get();
    auto r = ArcPinnedRowFor(g);
    if (!r) return;
    if (on) {
        t_arcPin.overRow = ArcPlacePinButton(*r);
        if (!t_arcPin.overRow && t_arcPin.grid.get() == g) ArcHidePinButton();
    } else if (t_arcPin.grid.get() == g) {
        t_arcPin.overRow = false;
        if (t_arcPin.timer) t_arcPin.timer.Start();
    }
}

static void ArcForgetPinnedRow(ArcPinnedRow& r) {
    if (auto row = r.row.get()) {
        row.PointerEntered(r.entered);
        row.PointerExited(r.exited);
        row.PointerCanceled(r.canceled);
    }
}

static void ArcAdoptPinnedRow(const muxc::UserControl& row, const muxc::Grid& g) {
    ArcPinnedRow r;
    r.row = winrt::make_weak(row);
    r.grid = winrt::make_weak(g);
    auto wg = r.grid;
    r.entered = row.PointerEntered([wg](auto&&, auto&&) { try { ArcPinnedRowHover(wg, true); } catch (...) {} });
    r.exited = row.PointerExited([wg](auto&&, auto&&) { try { ArcPinnedRowHover(wg, false); } catch (...) {} });
    r.canceled = row.PointerCanceled([wg](auto&&, auto&&) { try { ArcPinnedRowHover(wg, false); } catch (...) {} });
    t_arcPinnedRows.push_back(std::move(r));
}

// Dia's "away from pinned page" look goes: no "/" after the icon and no "Back to Pinned URL" line.
struct ArcPinnedMark {
    winrt::weak_ref<muxc::Button> back;
    double opacity = 1, width = 0;
    winrt::weak_ref<mux::FrameworkElement> subtitle;
    mux::Visibility subtitleVisibility = mux::Visibility::Collapsed;
    int64_t subtitleToken = 0;
};
static thread_local std::vector<ArcPinnedMark> t_arcPinnedMarks;

static void ArcRestorePinnedMark(ArcPinnedMark& m) {
    if (auto b = m.back.get()) b.Opacity(m.opacity), b.IsHitTestVisible(true), b.Width(m.width);
    if (auto s = m.subtitle.get()) {
        s.UnregisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), m.subtitleToken);
        s.Visibility(m.subtitleVisibility);
    }
}

static void ArcUpdatePinnedMarks() {
    if (!g_arcPinnedTabs.load()) {
        for (auto& m : t_arcPinnedMarks) ArcRestorePinnedMark(m);
        t_arcPinnedMarks.clear();
        return;
    }
    std::erase_if(t_arcPinnedMarks, [](ArcPinnedMark& m) {
        auto b = m.back.get();
        if (b && b.IsLoaded()) return false;
        ArcRestorePinnedMark(m);
        return true;
    });
    for (auto& row : ArcPinnedAllTabRows()) {
        auto g = row.try_as<muxc::UserControl>().Content().try_as<muxc::Grid>();
        auto back = g ? ArcPinnedRowButton(g, L"Back to Pinned URL", false) : nullptr;
        if (!back) continue;
        if (std::any_of(t_arcPinnedMarks.begin(), t_arcPinnedMarks.end(), [&](ArcPinnedMark& m) { return m.back.get() == back; })) {
            if (back.Opacity() != 0) back.Opacity(0);
            if (back.Width() != 0) back.Width(0);
            continue;
        }
        ArcPinnedMark m;
        m.back = winrt::make_weak(back);
        m.opacity = back.Opacity();
        m.width = back.Width();
        back.Opacity(0), back.IsHitTestVisible(false), back.Width(0);
        for (auto const& c : g.Children()) {
            auto stack = c.try_as<muxc::StackPanel>();
            if (!stack) continue;
            for (auto const& line : stack.Children()) {
                auto fe = line.try_as<mux::FrameworkElement>();
                if (!fe || !FirstIn(fe, [](const mux::FrameworkElement& e) {
                        auto t = e.try_as<muxc::TextBlock>();
                        return t && t.Text() == L"Back to Pinned URL";
                    }))
                    continue;
                m.subtitle = winrt::make_weak(fe);
                m.subtitleVisibility = fe.Visibility();
                fe.Visibility(mux::Visibility::Collapsed);
                m.subtitleToken = fe.RegisterPropertyChangedCallback(mux::UIElement::VisibilityProperty(), [](auto const& sender, auto&&) {
                    if (auto e = sender.template try_as<mux::FrameworkElement>(); e && e.Visibility() != mux::Visibility::Collapsed)
                        e.Visibility(mux::Visibility::Collapsed);
                });
            }
        }
        t_arcPinnedMarks.push_back(std::move(m));
    }
}

static void ArcUpdatePinnedTabs() {
    if (!g_arcPinnedTabs.load()) {
        if (t_arcPin.popup && t_arcPin.popup.IsOpen()) ArcHidePinButton();
        for (auto& r : t_arcPinnedRows) ArcForgetPinnedRow(r);
        t_arcPinnedRows.clear();
        for (auto& m : t_arcPinnedMarks) ArcRestorePinnedMark(m);
        t_arcPinnedMarks.clear();
        return;
    }
    std::erase_if(t_arcPinnedRows, [](ArcPinnedRow& r) {
        auto row = r.row.get();
        if (row && row.IsLoaded()) return false;
        ArcForgetPinnedRow(r);
        return true;
    });
    for (auto& r : t_arcPinnedRows) r.pinned = false;
    for (auto h : FindNodes(L"TabSeparatorRow")) {
        auto sep = Obj<mux::FrameworkElement>(h);
        auto list = sep ? muxm::VisualTreeHelper::GetParent(sep).try_as<muxc::StackPanel>() : nullptr;
        if (!list || list.ActualHeight() <= 0) continue;
        for (auto const& child : list.Children()) {
            if (child == sep) break;
            auto top = child.try_as<mux::FrameworkElement>();
            if (!top) continue;
            FirstIn(top, [&](const mux::FrameworkElement& fe) {
                auto uc = fe.try_as<muxc::UserControl>();
                auto g = uc ? uc.Content().try_as<muxc::Grid>() : nullptr;
                if (!g || g.ColumnDefinitions().Size() != 5) return false;
                bool inFolder = false;
                for (auto p = muxm::VisualTreeHelper::GetParent(fe); p && p != list; p = muxm::VisualTreeHelper::GetParent(p))
                    if (auto pe = p.try_as<mux::FrameworkElement>(); pe && pe.Name() == L"RowsPanel") inFolder = true;
                auto r = ArcPinnedRowFor(g);
                if (!r) ArcAdoptPinnedRow(uc, g), r = ArcPinnedRowFor(g);
                if (r) r->pinned = true, r->inFolder = inFolder;
                return false;
            });
        }
    }
    if (t_arcPin.popup && t_arcPin.popup.IsOpen()) {
        auto r = ArcPinnedRowFor(t_arcPin.grid.get());
        if (!r || !ArcPlacePinButton(*r)) ArcHidePinButton();
    }
}

static void UpdateLinkAddressNode(InstanceHandle handle) {
    bool hide = g_hideLinkAddress.load();
    Inspectable obj{nullptr};
    if (GetObject(handle, obj)) {
        if (auto fe = obj.try_as<mux::FrameworkElement>()) {
            auto name = fe.Name();
            if (name == L"StatusText") {
                // StatusText must remain Visible for Chromium link hover binding to update
                fe.Visibility(mux::Visibility::Visible);
                fe.Opacity(hide ? 0.0 : 1.0);
                fe.IsHitTestVisible(!hide);
            } else {
                fe.Visibility(hide ? mux::Visibility::Collapsed : mux::Visibility::Visible);
                fe.Opacity(hide ? 0.0 : 1.0);
            }
        }
    }
}

static thread_local bool t_lastHideLinkAddress = false;

static void UpdateHideLinkAddress() {
    bool hide = g_hideLinkAddress.load();
    if (!hide && !t_lastHideLinkAddress) return;
    t_lastHideLinkAddress = hide;
    for (const wchar_t* name : { L"StatusPill", L"StatusPillText", L"StatusPillIcon" }) {
        for (auto h : FindNodes(name)) {
            if (auto fe = Obj<mux::FrameworkElement>(h)) {
                fe.Visibility(hide ? mux::Visibility::Collapsed : mux::Visibility::Visible);
                fe.Opacity(hide ? 0.0 : 1.0);
            }
        }
    }
    for (auto h : FindNodes(L"StatusText")) {
        if (auto fe = Obj<mux::FrameworkElement>(h)) {
            fe.Opacity(hide ? 0.0 : 1.0);
            if (hide) {
                fe.MaxHeight(0);
                fe.MaxWidth(0);
            }
        }
    }
}

static void UpdateStateJson(const std::wstring& key, const std::wstring& value);

static std::mutex g_emojiLock;
static std::map<std::wstring, std::wstring> g_profileEmojis;

static std::wstring FindLocalStatePath() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    std::wstring searchPattern = std::wstring(local) + L"\\Packages\\TheBrowserCompany.Dia_*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring path = std::wstring(local) + L"\\Packages\\" + fd.cFileName +
                                    L"\\LocalCache\\Local\\Dia\\User Data\\Local State";
                if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    FindClose(hFind);
                    return path;
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return L"";
}

static std::vector<std::wstring> GetProfileNames() {
    static std::vector<std::wstring> s_cached;
    static ULONGLONG s_lastCheck = 0;
    ULONGLONG now = GetTickCount64();
    if (!s_cached.empty() && now - s_lastCheck < 4000) {
        return s_cached;
    }
    s_lastCheck = now;

    std::wstring path = FindLocalStatePath();
    if (!path.empty()) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(file, nullptr);
            if (size > 0 && size < 2000000) {
                std::string json(size, '\0');
                DWORD read = 0;
                if (ReadFile(file, json.data(), size, &read, nullptr)) {
                    size_t orderPos = json.find("\"profiles_order\":");
                    if (orderPos != std::string::npos) {
                        size_t startBracket = json.find('[', orderPos);
                        size_t endBracket = json.find(']', startBracket);
                        if (startBracket != std::string::npos && endBracket != std::string::npos) {
                            std::string orderStr = json.substr(startBracket + 1, endBracket - startBracket - 1);
                            std::vector<std::string> profileIds;
                            size_t p = 0;
                            while ((p = orderStr.find('"', p)) != std::string::npos) {
                                size_t p2 = orderStr.find('"', p + 1);
                                if (p2 == std::string::npos) break;
                                profileIds.push_back(orderStr.substr(p + 1, p2 - p - 1));
                                p = p2 + 1;
                            }
                            std::vector<std::wstring> names;
                            for (const auto& id : profileIds) {
                                std::string search = "\"" + id + "\":{";
                                size_t idPos = json.find(search);
                                if (idPos != std::string::npos) {
                                    size_t namePos = json.find("\"name\":\"", idPos);
                                    if (namePos != std::string::npos) {
                                        size_t nStart = namePos + 8;
                                        size_t nEnd = json.find('"', nStart);
                                        if (nEnd != std::string::npos) {
                                            std::string nameUtf8 = json.substr(nStart, nEnd - nStart);
                                            int wlen = MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, nullptr, 0);
                                            if (wlen > 1) {
                                                std::wstring wname(wlen - 1, L'\0');
                                                MultiByteToWideChar(CP_UTF8, 0, nameUtf8.c_str(), -1, wname.data(), wlen);
                                                names.push_back(wname);
                                            }
                                        }
                                    }
                                }
                            }
                            if (!names.empty()) {
                                s_cached = names;
                                CloseHandle(file);
                                return s_cached;
                            }
                        }
                    }
                }
            }
            CloseHandle(file);
        }
    }

    if (s_cached.empty()) {
        s_cached = { L"Personal", L"College", L"Mummy" };
    }
    return s_cached;
}

static std::wstring GetProfileEmoji(const std::wstring& profileName) {
    if (profileName.empty()) return L"";
    {
        std::lock_guard<std::mutex> lk(g_emojiLock);
        auto it = g_profileEmojis.find(profileName);
        if (it != g_profileEmojis.end()) return it->second;
    }
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"profileEmojis", profileName.c_str(), L"__DT_NOT_SET__", buf, _countof(buf), g_configPath);
    if (wcscmp(buf, L"__DT_NOT_SET__") != 0) {
        std::lock_guard<std::mutex> lk(g_emojiLock);
        g_profileEmojis[profileName] = buf;
        return buf;
    }
    // Default assignments if never set before
    std::wstring def;
    if (profileName == L"Personal") def = L"🦊";
    else if (profileName == L"College") def = L"💻";
    else if (profileName == L"Mummy") def = L"🌸";
    else def = L"";

    {
        std::lock_guard<std::mutex> lk(g_emojiLock);
        g_profileEmojis[profileName] = def;
    }
    return def;
}

static void SaveProfileEmoji(const std::wstring& profileName, const std::wstring& emoji) {
    if (profileName.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_emojiLock);
        g_profileEmojis[profileName] = emoji;
    }
    WritePrivateProfileStringW(L"profileEmojis", profileName.c_str(), emoji.c_str(), g_configPath);
    UpdateStateJson(L"profileEmojiText", emoji);
}

static thread_local std::vector<winrt::weak_ref<muxc::StackPanel>> t_spaceEmojiBars;

static std::vector<muxc::Button> GetNativeSpaceButtons() {
    std::vector<muxc::Button> btns;
    for (auto sh : FindNodes(L"SpaceSwitcherHost")) {
        auto host = Obj<mux::FrameworkElement>(sh);
        if (!host || !host.XamlRoot()) continue;
        std::function<void(const mux::DependencyObject&)> findBtns = [&](const mux::DependencyObject& d) {
            if (!d) return;
            if (auto b = d.try_as<muxc::Button>()) {
                btns.push_back(b);
                return;
            }
            int nc = muxm::VisualTreeHelper::GetChildrenCount(d);
            for (int i = 0; i < nc; i++) {
                findBtns(muxm::VisualTreeHelper::GetChild(d, i));
            }
        };
        findBtns(host);
        if (!btns.empty()) break;
    }
    return btns;
}

static void InvokeNativeSpaceButton(size_t index) {
    auto nativeBtns = GetNativeSpaceButtons();
    Log(L"[ProfileEmoji] InvokeNativeSpaceButton(%zu) - nativeBtns count = %zu", index, nativeBtns.size());
    if (index >= nativeBtns.size()) return;

    auto nb = nativeBtns[index];
    try {
        if (auto cmd = nb.Command()) {
            auto param = nb.CommandParameter();
            if (cmd.CanExecute(param)) {
                Log(L"[ProfileEmoji] Executing Command on native button %zu", index);
                cmd.Execute(param);
            }
        }
    } catch (...) {}

    try {
        auto peer = mux::Automation::Peers::FrameworkElementAutomationPeer::CreatePeerForElement(nb);
        if (!peer) {
            peer = mux::Automation::Peers::ButtonAutomationPeer(nb);
        }
        if (peer) {
            if (auto prov = peer.try_as<winrt::Microsoft::UI::Xaml::Automation::Provider::IInvokeProvider>()) {
                Log(L"[ProfileEmoji] Invoking IInvokeProvider on native button %zu", index);
                prov.Invoke();
            }
        }
    } catch (...) {}
}

static void UpdateProfileEmoji() {
    // The emoji row replaces the space switcher inside Dia's sidebar. In the
    // top-tab layout the same switcher is placed in the title bar, where an
    // emoji row would sit on top of Dia's own compact space buttons; there the
    // native switcher is left exactly as Dia draws it.
    bool on = g_profileEmoji.load() && IsSidebarOpen();
    static bool s_emojiApplied = false;
    if (!on) {
        if (!s_emojiApplied && t_spaceEmojiBars.empty()) return;
        s_emojiApplied = false;
    } else {
        s_emojiApplied = true;
    }

    // 1. Remove any top-bar emojis (WindowDragHeader / HeaderContentHost)
    for (auto h : FindNodes(L"HeaderContentHost")) {
        auto host = Obj<mux::FrameworkElement>(h);
        auto header = host ? muxm::VisualTreeHelper::GetParent(host).try_as<muxc::Grid>() : nullptr;
        if (!header) continue;
        for (int i = (int)header.Children().Size() - 1; i >= 0; i--) {
            if (auto fe = header.Children().GetAt(i).try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksProfileEmoji") {
                header.Children().RemoveAt(i);
            }
        }
    }
    for (auto h : FindNodes(L"WindowDragHeader")) {
        auto wdh = Obj<muxc::Grid>(h);
        if (!wdh) continue;
        for (int i = (int)wdh.Children().Size() - 1; i >= 0; i--) {
            if (auto fe = wdh.Children().GetAt(i).try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksProfileEmoji") {
                wdh.Children().RemoveAt(i);
            }
        }
    }
    // Remove old single-emoji TextBlocks from SpacePagingRoot
    for (auto h : FindNodes(L"SpacePagingRoot")) {
        auto paging = Obj<muxc::Grid>(h);
        if (!paging) continue;
        for (int i = (int)paging.Children().Size() - 1; i >= 0; i--) {
            if (auto fe = paging.Children().GetAt(i).try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksSpaceEmoji") {
                paging.Children().RemoveAt(i);
            }
        }
    }

    if (!on) {
        // Feature is OFF: remove custom emoji bars and restore default dots
        for (auto& w : t_spaceEmojiBars) {
            if (auto sp = w.get()) {
                if (auto p = muxm::VisualTreeHelper::GetParent(sp).try_as<muxc::Panel>()) {
                    uint32_t idx = 0;
                    if (p.Children().IndexOf(sp, idx)) p.Children().RemoveAt(idx);
                }
            }
        }
        t_spaceEmojiBars.clear();

        for (auto sh : FindNodes(L"SpaceIndicatorsViewport")) {
            if (auto siv = Obj<mux::FrameworkElement>(sh)) { siv.Opacity(1.0); siv.IsHitTestVisible(true); }
        }
        for (auto sh : FindNodes(L"SpaceIndicatorsPanel")) {
            if (auto sip = Obj<mux::FrameworkElement>(sh)) { sip.Opacity(1.0); sip.IsHitTestVisible(true); }
        }
        for (auto sh : FindNodes(L"SpaceSwitcherButton")) {
            if (auto ssb = Obj<mux::FrameworkElement>(sh)) { ssb.Opacity(1.0); ssb.IsHitTestVisible(true); }
        }
        for (auto sh : FindNodes(L"SpaceSwitcherHost")) {
            if (auto ssh = Obj<mux::FrameworkElement>(sh)) { ssh.Opacity(1.0); ssh.IsHitTestVisible(true); }
        }
        return;
    }

    // Feature is ON: hide native dots so they don't overlap with custom emojis/dots
    for (auto sh : FindNodes(L"SpaceIndicatorsViewport")) {
        if (auto siv = Obj<mux::FrameworkElement>(sh)) { siv.Opacity(0.0); siv.IsHitTestVisible(false); }
    }
    for (auto sh : FindNodes(L"SpaceIndicatorsPanel")) {
        if (auto sip = Obj<mux::FrameworkElement>(sh)) { sip.Opacity(0.0); sip.IsHitTestVisible(false); }
    }
    for (auto sh : FindNodes(L"SpaceSwitcherButton")) {
        if (auto ssb = Obj<mux::FrameworkElement>(sh)) { ssb.Opacity(0.0); ssb.IsHitTestVisible(false); }
    }
    for (auto sh : FindNodes(L"SpaceSwitcherHost")) {
        if (auto ssh = Obj<mux::FrameworkElement>(sh)) { ssh.Opacity(0.0); ssh.IsHitTestVisible(false); }
    }

    double spaceWidth = 190.0;
    for (auto sh : FindNodes(L"ResizingSidebarHost")) {
        if (auto fe = Obj<mux::FrameworkElement>(sh); fe && fe.ActualWidth() > 50.0) {
            spaceWidth = fe.ActualWidth();
            break;
        }
    }

    auto profileNames = GetProfileNames();
    size_t numSpaces = profileNames.size();
    if (numSpaces < 1) numSpaces = 1;

    // Detect active space index
    int activeIdx = -1;

    // 1. Check SpacePagesPanel RenderTransform (live sliding offset)
    static thread_local int64_t t_ttXToken = 0;
    static thread_local winrt::weak_ref<muxm::TranslateTransform> t_hookedTt;
    for (auto sh : FindNodes(L"SpacePagesPanel")) {
        if (auto spp = Obj<muxc::StackPanel>(sh)) {
            if (auto rt = spp.RenderTransform()) {
                if (auto tt = rt.try_as<muxm::TranslateTransform>()) {
                    activeIdx = static_cast<int>(round(-tt.X() / spaceWidth));
                    if (t_hookedTt.get() != tt) {
                        if (auto oldTt = t_hookedTt.get(); oldTt && t_ttXToken != 0) {
                            try { oldTt.UnregisterPropertyChangedCallback(muxm::TranslateTransform::XProperty(), t_ttXToken); } catch (...) {}
                            t_ttXToken = 0;
                        }
                        t_hookedTt = winrt::make_weak(tt);
                        try {
                            t_ttXToken = tt.RegisterPropertyChangedCallback(muxm::TranslateTransform::XProperty(), [](auto&&, auto&&) {
                                try { UpdateProfileEmoji(); } catch (...) {}
                            });
                        } catch (...) {}
                    }
                } else if (auto ct = rt.try_as<muxm::CompositeTransform>()) {
                    activeIdx = static_cast<int>(round(-ct.TranslateX() / spaceWidth));
                }
            }
            break;
        }
    }

    // 2. Fallback: check dot size inside native buttons in SpaceSwitcherHost (active dot is 6px, inactive is 4px)
    if (activeIdx < 0) {
        auto nativeBtns = GetNativeSpaceButtons();
        for (size_t b = 0; b < nativeBtns.size(); b++) {
            bool dotActive = false;
            std::function<void(const mux::DependencyObject&)> checkDot = [&](const mux::DependencyObject& d) {
                if (!d || dotActive) return;
                if (auto tb = d.try_as<muxc::TextBlock>()) {
                    if (tb.ActualWidth() > 5.0 || tb.Width() > 5.0) {
                        dotActive = true;
                        return;
                    }
                }
                if (auto fi = d.try_as<muxc::FontIcon>()) {
                    if (fi.FontSize() > 5.0 || fi.ActualWidth() > 5.0) {
                        dotActive = true;
                        return;
                    }
                }
                int nc = muxm::VisualTreeHelper::GetChildrenCount(d);
                for (int c = 0; c < nc; c++) {
                    checkDot(muxm::VisualTreeHelper::GetChild(d, c));
                }
            };
            checkDot(nativeBtns[b]);
            if (dotActive) {
                activeIdx = static_cast<int>(b);
                break;
            }
        }
    }

    if (activeIdx < 0) activeIdx = 0;
    if (activeIdx >= (int)numSpaces) activeIdx = (int)numSpaces - 1;

    // 4. Place each space's respective emoji or normal dot into diatweaksSpaceEmojiBar
    for (auto h : FindNodes(L"SpacePagingRoot")) {
        auto paging = Obj<muxc::Grid>(h);
        if (!paging) continue;

        int switcherRow = 2;
        for (auto const& c : paging.Children()) {
            if (auto fe = c.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"SpaceSwitcherHost") {
                switcherRow = muxc::Grid::GetRow(fe);
                break;
            }
        }

        muxc::StackPanel emojiBar{nullptr};
        for (auto const& c : paging.Children()) {
            if (auto sp = c.try_as<muxc::StackPanel>(); sp && sp.Name() == L"diatweaksSpaceEmojiBar") {
                emojiBar = sp;
                break;
            }
        }

        if (!emojiBar) {
            emojiBar = muxc::StackPanel();
            emojiBar.Name(L"diatweaksSpaceEmojiBar");
            emojiBar.Orientation(muxc::Orientation::Horizontal);
            emojiBar.HorizontalAlignment(mux::HorizontalAlignment::Center);
            emojiBar.VerticalAlignment(mux::VerticalAlignment::Center);
            emojiBar.MinHeight(38);
            emojiBar.Spacing(10); // space out accordingly so it's not cluttered
            muxc::Grid::SetRow(emojiBar, switcherRow);
            paging.Children().Append(emojiBar);
            t_spaceEmojiBars.push_back(winrt::make_weak(emojiBar));
        }

        while (emojiBar.Children().Size() > numSpaces) {
            emojiBar.Children().RemoveAt(emojiBar.Children().Size() - 1);
        }

        for (size_t i = 0; i < numSpaces; i++) {
            std::wstring pName = profileNames[i];
            std::wstring em = GetProfileEmoji(pName);

            muxc::Button btn{nullptr};
            if (i < emojiBar.Children().Size()) {
                btn = emojiBar.Children().GetAt(static_cast<uint32_t>(i)).try_as<muxc::Button>();
            }

            if (!btn) {
                btn = muxc::Button();
                btn.MinWidth(28);
                btn.MinHeight(28);
                btn.Padding(mux::Thickness{4, 2, 4, 2});
                btn.Background(muxm::SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0}));
                btn.BorderThickness(mux::Thickness{0, 0, 0, 0});
                btn.CornerRadius(mux::CornerRadius{6, 6, 6, 6});

                btn.Click([i, spaceWidth](auto&&, auto&&) {
                    // 1. Optimistic UI update
                    for (auto& w : t_spaceEmojiBars) {
                        if (auto bar = w.get()) {
                            for (size_t k = 0; k < bar.Children().Size(); k++) {
                                if (auto b = bar.Children().GetAt(static_cast<uint32_t>(k)).try_as<muxc::Button>()) {
                                    b.Opacity((k == i) ? 1.0 : 0.45);
                                }
                            }
                        }
                    }

                    // 2. Invoke the native space button in Dia
                    InvokeNativeSpaceButton(i);

                    // 3. Slide SpacePagesPanel if needed
                    for (auto sh : FindNodes(L"SpacePagesPanel")) {
                        if (auto spp = Obj<muxc::StackPanel>(sh)) {
                            if (auto rt = spp.RenderTransform()) {
                                if (auto tt = rt.try_as<muxm::TranslateTransform>()) {
                                    tt.X(-static_cast<double>(i) * spaceWidth);
                                } else if (auto ct = rt.try_as<muxm::CompositeTransform>()) {
                                    ct.TranslateX(-static_cast<double>(i) * spaceWidth);
                                }
                            }
                            break;
                        }
                    }

                    // 4. Trigger update
                    UpdateProfileEmoji();
                });

                emojiBar.Children().Append(btn);
            }

            bool isActive = (static_cast<int>(i) == activeIdx);
            double targetOpacity = isActive ? 1.0 : 0.45;

            if (em.empty()) {
                // Profile has no emoji: show normal circular dot
                auto dot = btn.Content().try_as<muxc::Border>();
                if (!dot) {
                    dot = muxc::Border();
                    dot.Background(muxm::SolidColorBrush(winrt::Windows::UI::Color{255, 255, 255, 255}));
                    dot.HorizontalAlignment(mux::HorizontalAlignment::Center);
                    dot.VerticalAlignment(mux::VerticalAlignment::Center);
                    dot.Margin(mux::Thickness{6, 6, 6, 6});
                    btn.Content(dot);
                }
                double dotSize = isActive ? 6.0 : 4.0;
                dot.Width(dotSize);
                dot.Height(dotSize);
                dot.CornerRadius(mux::CornerRadius{dotSize / 2, dotSize / 2, dotSize / 2, dotSize / 2});
            } else {
                auto tb = btn.Content().try_as<muxc::TextBlock>();
                if (!tb) {
                    tb = muxc::TextBlock();
                    tb.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
                    tb.FontSize(15);
                    tb.HorizontalAlignment(mux::HorizontalAlignment::Center);
                    tb.VerticalAlignment(mux::VerticalAlignment::Center);
                    btn.Content(tb);
                }
                if (tb.Text() != em) tb.Text(em);
            }

            btn.Opacity(targetOpacity);
        }
    }
}

// ---------------------------------------------------------------- Segoe UI Variable Font
struct SavedFontInfo {
    muxm::FontFamily originalFamily{nullptr};
    bool hadLocal = false;
};
static thread_local std::map<InstanceHandle, SavedFontInfo> t_savedFonts;

static bool IsIconOrSymbol(const mux::DependencyObject& obj) {
    if (auto tb = obj.try_as<muxc::TextBlock>()) {
        try {
            auto ff = tb.FontFamily();
            if (ff) {
                std::wstring src = std::wstring(ff.Source());
                if (src.find(L"Icon") != std::wstring::npos ||
                    src.find(L"Emoji") != std::wstring::npos ||
                    src.find(L"MDL2") != std::wstring::npos ||
                    src.find(L"Symbol") != std::wstring::npos) {
                    return true;
                }
            }
            winrt::hstring text = tb.Text();
            if (!text.empty()) {
                if (text.size() == 1) {
                    wchar_t ch = text[0];
                    if ((ch >= 0xE000 && ch <= 0xF8FF) || ch >= 0xFF00) return true;
                } else if (text.size() == 2) {
                    wchar_t h = text[0], l = text[1];
                    if (h >= 0xD800 && h <= 0xDBFF) {
                        uint32_t cp = 0x10000 + (((h - 0xD800) << 10) | (l - 0xDC00));
                        if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;
                    }
                }
            }
        } catch (...) {}
        return false;
    }
    if (auto cp = obj.try_as<muxc::ContentPresenter>()) {
        try {
            auto ff = cp.FontFamily();
            if (ff) {
                std::wstring src = std::wstring(ff.Source());
                if (src.find(L"Icon") != std::wstring::npos ||
                    src.find(L"Emoji") != std::wstring::npos ||
                    src.find(L"MDL2") != std::wstring::npos ||
                    src.find(L"Symbol") != std::wstring::npos) {
                    return true;
                }
            }
            if (auto content = cp.Content()) {
                if (auto str = content.try_as<winrt::hstring>()) {
                    if (!str->empty()) {
                        wchar_t ch = (*str)[0];
                        if ((ch >= 0xE000 && ch <= 0xF8FF) || ch >= 0xFF00) return true;
                    }
                }
            }
        } catch (...) {}
        return false;
    }
    if (auto rtb = obj.try_as<muxc::RichTextBlock>()) {
        try {
            auto ff = rtb.FontFamily();
            if (ff) {
                std::wstring src = std::wstring(ff.Source());
                if (src.find(L"Icon") != std::wstring::npos ||
                    src.find(L"Emoji") != std::wstring::npos ||
                    src.find(L"MDL2") != std::wstring::npos ||
                    src.find(L"Symbol") != std::wstring::npos) {
                    return true;
                }
            }
        } catch (...) {}
        return false;
    }
    if (auto ctrl = obj.try_as<muxc::Control>()) {
        try {
            auto ff = ctrl.FontFamily();
            if (ff) {
                std::wstring src = std::wstring(ff.Source());
                if (src.find(L"Icon") != std::wstring::npos ||
                    src.find(L"Emoji") != std::wstring::npos ||
                    src.find(L"MDL2") != std::wstring::npos ||
                    src.find(L"Symbol") != std::wstring::npos) {
                    return true;
                }
            }
        } catch (...) {}
        return false;
    }
    return false;
}

static std::wstring GetDiaUserDataDir() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L"";
    std::wstring packagesPattern = std::wstring(local) + L"\\Packages\\TheBrowserCompany.Dia*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(packagesPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return L"";
    std::wstring pkgName = fd.cFileName;
    FindClose(hFind);
    return std::wstring(local) + L"\\Packages\\" + pkgName + L"\\LocalCache\\Local\\Dia\\User Data";
}

static void SyncWebSegoeUiVariable(bool on) {
    std::wstring userData = GetDiaUserDataDir();
    if (userData.empty()) return;

    std::vector<std::wstring> profileDirs;
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW((userData + L"\\*").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
                profileDirs.push_back(userData + L"\\" + fd.cFileName);
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    for (const auto& pDir : profileDirs) {
        // 1. Update style-guide.css in HomeSurface\Shared so the New Tab page uses Segoe UI Variable
        std::wstring cssPath = pDir + L"\\HomeSurface\\Shared\\style-guide.css";
        HANDLE hFile = CreateFileW(cssPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, nullptr);
            if (size != INVALID_FILE_SIZE && size > 0 && size < 100000) {
                std::string content(size, '\0');
                DWORD read = 0;
                if (ReadFile(hFile, content.data(), size, &read, nullptr) && read == size) {
                    const std::string kMarker = "\"Segoe UI Variable\", ";
                    size_t pos = content.find("--home-font-body:");
                    bool modified = false;
                    if (pos != std::string::npos) {
                        if (on) {
                            if (content.find(kMarker) == std::string::npos) {
                                size_t insertPos = content.find_first_not_of(" \t", pos + 17);
                                if (insertPos != std::string::npos) {
                                    content.insert(insertPos, kMarker);
                                    modified = true;
                                }
                            }
                        } else {
                            size_t markerPos = content.find(kMarker);
                            if (markerPos != std::string::npos) {
                                content.erase(markerPos, kMarker.length());
                                modified = true;
                            }
                        }
                    }
                    if (modified) {
                        SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
                        DWORD written = 0;
                        WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
                        SetEndOfFile(hFile);
                        Log(L"segoeUiVariable: updated %s (on=%d)", cssPath.c_str(), on ? 1 : 0);
                    }
                }
            }
            CloseHandle(hFile);
        }
    }
}

static bool s_lastSegoeUiVarState = false;
static bool s_segoeUiVarStateInit = false;

static void UpdateSegoeUiVariable() {
    bool on = g_segoeUiVariable.load();
    const DWORD thread = GetCurrentThreadId();

    if (!s_segoeUiVarStateInit || on != s_lastSegoeUiVarState) {
        s_segoeUiVarStateInit = true;
        s_lastSegoeUiVarState = on;
        Log(L"segoeUiVariable: state changed to %d on thread %lu", on ? 1 : 0, thread);
        SyncWebSegoeUiVariable(on);
    } else if (!on || !g_treeDirty.load()) {
        return;
    }

    static const wchar_t* kVarFontName = L"Segoe UI Variable";
    muxm::FontFamily varFont(kVarFontName);
    muxm::FontFamily defaultFont(L"Segoe UI");

    // 1. Application-level Theme Resources
    try {
        auto res = mux::Application::Current().Resources();
        auto targetFont = on ? varFont : defaultFont;
        res.Insert(winrt::box_value(L"ContentControlThemeFontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"TextControlThemeFontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"PivotHeaderItemThemeFontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"NormalThemeFontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"StandardFontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"FontFamily"), winrt::box_value(targetFont));
        res.Insert(winrt::box_value(L"ControlContentThemeFontFamily"), winrt::box_value(targetFont));
    } catch (...) {}

    // 2. Window Roots (propagate inheritable FontFamily down the tree)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        for (auto rootHandle : g_roots) {
            Inspectable obj{nullptr};
            if (!GetObject(rootHandle, obj)) continue;
            if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                try {
                    if (on) {
                        fe.SetValue(muxc::TextBlock::FontFamilyProperty(), winrt::box_value(varFont));
                    } else {
                        fe.ClearValue(muxc::TextBlock::FontFamilyProperty());
                    }
                } catch (...) {}
            }
            if (auto ctrl = obj.try_as<muxc::Control>()) {
                try {
                    if (on) {
                        ctrl.FontFamily(varFont);
                    } else {
                        ctrl.ClearValue(muxc::Control::FontFamilyProperty());
                    }
                } catch (...) {}
            }
        }
    }

    // 3. Elements currently in the visual tree (TextBlock, ContentPresenter, RichTextBlock, Control)
    if (on) {
        if (t_savedFonts.size() > 4000) {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            for (auto it = t_savedFonts.begin(); it != t_savedFonts.end(); ) {
                if (g_nodes.find(it->first) == g_nodes.end()) {
                    it = t_savedFonts.erase(it);
                } else {
                    ++it;
                }
            }
        }

        std::vector<InstanceHandle> handles;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            for (const auto& [handle, node] : g_nodes) {
                if (node.thread == thread) handles.push_back(handle);
            }
        }

        size_t countTb = 0, countCp = 0, countRtb = 0, countCtrl = 0, countSkipped = 0;
        for (auto handle : handles) {
            Inspectable obj{nullptr};
            if (!GetObject(handle, obj)) continue;

            if (auto tb = obj.try_as<muxc::TextBlock>()) {
                if (IsIconOrSymbol(tb)) { countSkipped++; continue; }
                try {
                    auto curFf = tb.FontFamily();
                    if (!curFf || curFf.Source() != kVarFontName) {
                        if (t_savedFonts.find(handle) == t_savedFonts.end()) {
                            auto localVal = tb.ReadLocalValue(muxc::TextBlock::FontFamilyProperty());
                            bool hadLocal = (localVal != mux::DependencyProperty::UnsetValue());
                            t_savedFonts[handle] = { hadLocal ? curFf : nullptr, hadLocal };
                        }
                        tb.FontFamily(varFont);
                        countTb++;
                    }
                } catch (...) {}
            } else if (auto cp = obj.try_as<muxc::ContentPresenter>()) {
                if (IsIconOrSymbol(cp)) { countSkipped++; continue; }
                try {
                    auto curFf = cp.FontFamily();
                    if (!curFf || curFf.Source() != kVarFontName) {
                        if (t_savedFonts.find(handle) == t_savedFonts.end()) {
                            auto localVal = cp.ReadLocalValue(muxc::ContentPresenter::FontFamilyProperty());
                            bool hadLocal = (localVal != mux::DependencyProperty::UnsetValue());
                            t_savedFonts[handle] = { hadLocal ? curFf : nullptr, hadLocal };
                        }
                        cp.FontFamily(varFont);
                        countCp++;
                    }
                } catch (...) {}
            } else if (auto rtb = obj.try_as<muxc::RichTextBlock>()) {
                if (IsIconOrSymbol(rtb)) { countSkipped++; continue; }
                try {
                    auto curFf = rtb.FontFamily();
                    if (!curFf || curFf.Source() != kVarFontName) {
                        if (t_savedFonts.find(handle) == t_savedFonts.end()) {
                            auto localVal = rtb.ReadLocalValue(muxc::RichTextBlock::FontFamilyProperty());
                            bool hadLocal = (localVal != mux::DependencyProperty::UnsetValue());
                            t_savedFonts[handle] = { hadLocal ? curFf : nullptr, hadLocal };
                        }
                        rtb.FontFamily(varFont);
                        countRtb++;
                    }
                } catch (...) {}
            } else if (auto ctrl = obj.try_as<muxc::Control>()) {
                if (IsIconOrSymbol(ctrl)) { countSkipped++; continue; }
                try {
                    auto curFf = ctrl.FontFamily();
                    if (!curFf || curFf.Source() != kVarFontName) {
                        if (t_savedFonts.find(handle) == t_savedFonts.end()) {
                            auto localVal = ctrl.ReadLocalValue(muxc::Control::FontFamilyProperty());
                            bool hadLocal = (localVal != mux::DependencyProperty::UnsetValue());
                            t_savedFonts[handle] = { hadLocal ? curFf : nullptr, hadLocal };
                        }
                        ctrl.FontFamily(varFont);
                        countCtrl++;
                    }
                } catch (...) {}
            }
        }
        if (countTb > 0 || countCp > 0 || countRtb > 0 || countCtrl > 0) {
            Log(L"segoeUiVariable: applied var font to %zu tb, %zu cp, %zu rtb, %zu ctrl (skipped %zu symbols)",
                countTb, countCp, countRtb, countCtrl, countSkipped);
        }
    } else {
        if (!t_savedFonts.empty()) {
            size_t restored = 0;
            for (const auto& [handle, saved] : t_savedFonts) {
                Inspectable obj{nullptr};
                if (!GetObject(handle, obj)) continue;

                try {
                    if (auto tb = obj.try_as<muxc::TextBlock>()) {
                        if (saved.hadLocal && saved.originalFamily) {
                            tb.FontFamily(saved.originalFamily);
                        } else {
                            tb.ClearValue(muxc::TextBlock::FontFamilyProperty());
                        }
                        restored++;
                    } else if (auto cp = obj.try_as<muxc::ContentPresenter>()) {
                        if (saved.hadLocal && saved.originalFamily) {
                            cp.FontFamily(saved.originalFamily);
                        } else {
                            cp.ClearValue(muxc::ContentPresenter::FontFamilyProperty());
                        }
                        restored++;
                    } else if (auto rtb = obj.try_as<muxc::RichTextBlock>()) {
                        if (saved.hadLocal && saved.originalFamily) {
                            rtb.FontFamily(saved.originalFamily);
                        } else {
                            rtb.ClearValue(muxc::RichTextBlock::FontFamilyProperty());
                        }
                        restored++;
                    } else if (auto ctrl = obj.try_as<muxc::Control>()) {
                        if (saved.hadLocal && saved.originalFamily) {
                            ctrl.FontFamily(saved.originalFamily);
                        } else {
                            ctrl.ClearValue(muxc::Control::FontFamilyProperty());
                        }
                        restored++;
                    }
                } catch (...) {}
            }
            Log(L"segoeUiVariable: restored %zu elements", restored);
            t_savedFonts.clear();
        }
    }
}

// ---------------------------------------------------------------- Noto emoji font
// Emoji are not drawn because an element names a font family. The text stack
// asks for the element's family (Segoe UI Variable, say) and DirectWrite's
// fallback swaps Segoe UI Emoji in for the emoji code points, so there is no
// "Segoe UI Emoji" property to replace. This tweak puts the Noto family in
// front of the family an element already uses, but only on elements whose text
// actually carries an emoji. Noto has no Latin glyphs, so ordinary text in the
// same run falls through to the font it would have used anyway, while the
// emoji resolve to Noto before Segoe UI Emoji is ever considered.

struct SavedEmojiFamily {
    muxm::FontFamily originalFamily{nullptr};
    bool hadLocal = false;
};
static thread_local std::map<InstanceHandle, SavedEmojiFamily> t_savedEmojiFamilies;

// Segoe Fluent Icons and Segoe MDL2 keep their glyph font: their code points
// sit in the private use area and must never be pointed at an emoji family.
static bool UsesIconFont(const muxm::FontFamily& family) {
    if (!family) return false;
    const std::wstring src(family.Source());
    return src.find(L"Fluent Icons") != std::wstring::npos ||
           src.find(L"MDL2") != std::wstring::npos ||
           src.find(L"Segoe UI Symbol") != std::wstring::npos;
}

// U+FE0F asks for emoji presentation on its own, so it counts by itself. The
// rest are the BMP and astral code points Unicode calls emoji; check marks and
// arrows that Windows normally draws as text are deliberately left out.
static bool IsEmojiCodePoint(unsigned cp) {
    if (cp == 0xFE0F) return true;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true;  // emoji, flags, skin tones
    if (cp >= 0x2600 && cp <= 0x26FF) return true;    // misc symbols
    if (cp == 0x2705 || cp == 0x2728 || cp == 0x274C || cp == 0x274E ||
        (cp >= 0x2753 && cp <= 0x2755) || cp == 0x2757 || cp == 0x2764 ||
        (cp >= 0x2795 && cp <= 0x2797) || cp == 0x27A1 || cp == 0x27B0 || cp == 0x27BF)
        return true;                                  // coloured dingbats
    if ((cp >= 0x2B05 && cp <= 0x2B07) || cp == 0x2B1B || cp == 0x2B1C ||
        cp == 0x2B50 || cp == 0x2B55)
        return true;
    if ((cp >= 0x231A && cp <= 0x231B) || cp == 0x2328 ||
        (cp >= 0x23E9 && cp <= 0x23EC) || cp == 0x23F0 || cp == 0x23F3 ||
        (cp >= 0x25FD && cp <= 0x25FE) || (cp >= 0x2614 && cp <= 0x2615) ||
        (cp >= 0x2648 && cp <= 0x2653) || cp == 0x267F || cp == 0x2693 ||
        cp == 0x26A1 || (cp >= 0x26AA && cp <= 0x26AB) ||
        (cp >= 0x26BD && cp <= 0x26BE) || (cp >= 0x26C4 && cp <= 0x26C5) ||
        cp == 0x26CE || cp == 0x26D4 || cp == 0x26EA ||
        (cp >= 0x26F2 && cp <= 0x26F3) || cp == 0x26F5 || cp == 0x26FA ||
        cp == 0x26FD || (cp >= 0x270A && cp <= 0x270B))
        return true;
    return false;
}

static bool TextHasEmoji(const std::wstring& text) {
    for (size_t i = 0; i < text.size(); i++) {
        wchar_t c = text[i];
        unsigned cp = static_cast<unsigned>(c);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
            const wchar_t low = text[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((static_cast<unsigned>(c) - 0xD800) << 10) +
                     (static_cast<unsigned>(low) - 0xDC00);
                i++;
            }
        }
        if (IsEmojiCodePoint(cp)) return true;
    }
    return false;
}

static std::wstring BoxedText(const Inspectable& value) {
    if (!value) return L"";
    try {
        return std::wstring(winrt::unbox_value<winrt::hstring>(value));
    } catch (...) {
        return L"";
    }
}

static std::wstring RichTextText(const muxc::RichTextBlock& rtb) {
    std::wstring text;
    try {
        for (auto const& block : rtb.Blocks()) {
            auto paragraph = block.try_as<winrt::Microsoft::UI::Xaml::Documents::Paragraph>();
            if (!paragraph) continue;
            for (auto const& item : paragraph.Inlines()) {
                if (auto run = item.try_as<winrt::Microsoft::UI::Xaml::Documents::Run>()) {
                    text += std::wstring(run.Text());
                    if (text.size() > 400) return text;  // a glimpse is enough
                }
            }
        }
    } catch (...) {}
    return text;
}

static int CALLBACK CountFontFamilyProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM data) {
    ++*reinterpret_cast<int*>(data);
    return 1;  // one match is enough
}

static bool FontFamilyInstalled(const std::wstring& family) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, family.c_str(), LF_FACESIZE);
    int count = 0;
    EnumFontFamiliesExW(dc, &lf, CountFontFamilyProc, reinterpret_cast<LPARAM>(&count), 0);
    DeleteDC(dc);
    return count > 0;
}

// Puts family in front of base, dropping any Noto family an earlier tick (or the
// other Noto family, after a switch) already stacked there.
static std::wstring EmojiChain(const std::wstring& family, std::wstring base) {
    static const wchar_t* kEmojiFamilies[] = {L"Noto Color Emoji", L"Noto Emoji"};
    for (auto name : kEmojiFamilies) {
        const std::wstring prefix = std::wstring(name) + L", ";
        if (base.compare(0, prefix.size(), prefix) == 0) {
            base.erase(0, prefix.size());
            break;
        }
    }
    if (base.empty()) return family;
    return family + L", " + base;
}

static void UpdateNotoEmoji() {
    const bool on = g_notoEmoji.load();
    const DWORD thread = GetCurrentThreadId();
    const std::wstring family =
        g_cfg.notoEmojiFont == L"notoMono" ? L"Noto Emoji" : L"Noto Color Emoji";

    static thread_local bool s_lastOn = false;
    static thread_local std::wstring s_lastFamily;

    if (!on) {
        s_lastOn = false;
        s_lastFamily = family;
        if (t_savedEmojiFamilies.empty()) return;

        size_t restored = 0;
        for (const auto& [handle, saved] : t_savedEmojiFamilies) {
            Inspectable obj{nullptr};
            if (!GetObject(handle, obj)) continue;
            try {
                if (auto tb = obj.try_as<muxc::TextBlock>()) {
                    if (saved.hadLocal && saved.originalFamily) tb.FontFamily(saved.originalFamily);
                    else tb.ClearValue(muxc::TextBlock::FontFamilyProperty());
                    restored++;
                } else if (auto cp = obj.try_as<muxc::ContentPresenter>()) {
                    if (saved.hadLocal && saved.originalFamily) cp.FontFamily(saved.originalFamily);
                    else cp.ClearValue(muxc::ContentPresenter::FontFamilyProperty());
                    restored++;
                } else if (auto rtb = obj.try_as<muxc::RichTextBlock>()) {
                    if (saved.hadLocal && saved.originalFamily) rtb.FontFamily(saved.originalFamily);
                    else rtb.ClearValue(muxc::RichTextBlock::FontFamilyProperty());
                    restored++;
                } else if (auto ctrl = obj.try_as<muxc::Control>()) {
                    if (saved.hadLocal && saved.originalFamily) ctrl.FontFamily(saved.originalFamily);
                    else ctrl.ClearValue(muxc::Control::FontFamilyProperty());
                    restored++;
                }
            } catch (...) {}
        }
        if (restored > 0) Log(L"notoEmoji: restored %zu elements", restored);
        t_savedEmojiFamilies.clear();
        return;
    }

    const bool first = !s_lastOn || s_lastFamily != family;
    s_lastOn = true;
    s_lastFamily = family;

    if (first) {
        Log(L"notoEmoji: using %s (installed: %d)", family.c_str(),
            FontFamilyInstalled(family) ? 1 : 0);
    }

    if (t_savedEmojiFamilies.size() > 2000) {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        for (auto it = t_savedEmojiFamilies.begin(); it != t_savedEmojiFamilies.end();) {
            if (g_nodes.find(it->first) == g_nodes.end()) it = t_savedEmojiFamilies.erase(it);
            else ++it;
        }
    }

    std::vector<InstanceHandle> handles;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        for (const auto& [handle, node] : g_nodes) {
            if (node.thread == thread) handles.push_back(handle);
        }
    }

    size_t touched = 0;
    for (auto handle : handles) {
        Inspectable obj{nullptr};
        if (!GetObject(handle, obj)) continue;

        auto apply = [&](auto element, mux::DependencyProperty prop, const std::wstring& text) {
            if (text.empty() || !TextHasEmoji(text)) return;
            auto ff = element.FontFamily();
            if (UsesIconFont(ff)) return;
            const std::wstring src = ff ? std::wstring(ff.Source()) : L"";
            const std::wstring wanted = EmojiChain(family, src);
            if (src == wanted) return;  // this tick's family is already stacked
            if (t_savedEmojiFamilies.find(handle) == t_savedEmojiFamilies.end()) {
                const bool hadLocal =
                    element.ReadLocalValue(prop) != mux::DependencyProperty::UnsetValue();
                t_savedEmojiFamilies[handle] = {hadLocal ? ff : muxm::FontFamily{nullptr}, hadLocal};
            }
            element.FontFamily(muxm::FontFamily(wanted.c_str()));
            touched++;
        };

        try {
            if (auto tb = obj.try_as<muxc::TextBlock>())
                apply(tb, muxc::TextBlock::FontFamilyProperty(), std::wstring(tb.Text()));
            else if (auto cp = obj.try_as<muxc::ContentPresenter>())
                apply(cp, muxc::ContentPresenter::FontFamilyProperty(), BoxedText(cp.Content()));
            else if (auto rtb = obj.try_as<muxc::RichTextBlock>())
                apply(rtb, muxc::RichTextBlock::FontFamilyProperty(), RichTextText(rtb));
            else if (auto ctrl = obj.try_as<muxc::ContentControl>())
                apply(ctrl, muxc::Control::FontFamilyProperty(), BoxedText(ctrl.Content()));
        } catch (...) {}
    }

    if (touched > 0) Log(L"notoEmoji: stacked %s on %zu elements", family.c_str(), touched);
}

struct Target {
    InstanceHandle handle = 0;
    std::wstring name;
    std::wstring parent;
    std::wstring type;
    unsigned childIndex = 0;
};

static void EvaluateNode(const Target& target, DWORD thread) {
    if (thread != GetCurrentThreadId()) return;
    const InstanceHandle handle = target.handle;
    const std::wstring& name = target.name;
    const std::wstring& parent = target.parent;
    const std::wstring& type = target.type;
    const unsigned childIndex = target.childIndex;

    bool transparentToolbar = g_toolbarColor.load() || (g_newTabPainting.load() && IsNewTab());
    if (name == L"NavigationBarHost" || name == L"NavigationChromeHost" || name == L"NavigationBarContent") {
        Inspectable obj{nullptr};
        if (GetObject(handle, obj)) {
            if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                if (!IsNewTab() && g_hideToolbar.load() && name == L"NavigationBarHost" && fe.Visibility() != mux::Visibility::Visible) {
                    fe.Visibility(mux::Visibility::Visible);
                }
                if (transparentToolbar) {
                    MakeSubtreeClear(fe);
                } else {
                    RestoreClearSubtree();
                }
            }
        }
    } else if (name == L"BottomSeparator") {
        Inspectable obj{nullptr};
        if (GetObject(handle, obj)) {
            if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                if (transparentToolbar) {
                    fe.Visibility(mux::Visibility::Collapsed);
                    fe.Height(0);
                } else {
                    fe.Visibility(mux::Visibility::Visible);
                    fe.ClearValue(mux::FrameworkElement::HeightProperty());
                    if (auto b = fe.try_as<muxc::Border>()) {
                        b.ClearValue(muxc::Border::BorderThicknessProperty());
                    }
                }
            }
        }
        ApplyCollapsed(handle, transparentToolbar);
    } else if (name == L"SidebarToggleButton") {
        ApplyCollapsed(handle, g_hideSidebarButton.load());
    } else if (name == L"ChatButtonHost") {
        ApplyCollapsed(handle, g_hideChatButton.load());
    } else if (name == L"MinimizeButton" || name == L"MaximizeButton" || name == L"CloseButton") {
        bool show = (g_hideToolbar.load() && (g_revealToolbarOnHover.load() && g_hoverTop.load())) ||
                    (g_showToolbarWithSidebar.load() && IsSidebarOpen()) ||
                    (g_showToolbarOnNewTab.load() && IsNewTab());
        bool wantControls = !g_hideToolbar.load() || show;
        bool hideControls = g_hideTrafficLights.load() || !wantControls;
        ApplyCollapsed(handle, hideControls);
    } else if (name == L"PersonalizeButton") {
        ApplyCollapsed(handle, g_hidePersonalizeButton.load());
    } else if (name == L"AdvancedChatUpsellHost" || name == L"ChatUsageCard" ||
               name == L"ChatUsageButton" || name == L"ChatUsageMessage" ||
               name.find(L"ChatUsage") != std::wstring::npos ||
               name.find(L"Upsell") != std::wstring::npos) {
        ApplyCollapsed(handle, g_hideUpgradeBanner.load());
    } else if (name == L"AssistantBar") {
        UpdateField(handle);
    } else if (name == L"UrlBarTrailingStack") {
        UpdateTrailingStack(handle);
    } else if (name == L"OverflowButton" && HasAncestorNamed(handle, L"WindowDragHeader")) {
        ApplyCollapsed(handle, g_hideTabSearchButton.load());
    } else if (name == L"LoadingIndicatorLayer") {
        ApplyCollapsed(handle, g_hideLoadingAnimation.load());
    } else if (name == L"SidebarBorder") {
        UpdateSidebarGrain(handle, g_sidebarGrain.load());
        // Never fade the border: it contains the sidebar's text. Clear any value
        // applied by older builds and let the floating surface own translucency.
        ApplyOpacity(handle, L"0.7", false);
    } else if (name == L"SidebarFloatingSurface") {
        UpdateSidebarGrain(handle, g_sidebarGrain.load());
        ApplyOpacity(handle, L"0.7", g_translucentSidebar.load());
    } else if (name == L"SidebarFloatingTint") {
        // The surface's opacity already fades its tint; fading both doubles up.
        ApplyOpacity(handle, L"0.7", false);
    } else if (name == L"StatusText" || name == L"StatusPill" ||
               name == L"StatusPillIcon" || name == L"StatusPillText") {
        UpdateLinkAddressNode(handle);
    } else if (name == L"ActionsRow") {
        UpdateTabHoverCard(handle);
    } else if (name == L"ToolbarArea") {
        // Updated in ApplyAllOnThisThread()
    } else if (name == L"advanced") {
        SettingsUi::ScanForSettings();
    } else if (childIndex == 0 && (type.find(L"Grid") != std::wstring::npos)) {
        InstanceHandle up2 = GetAncestorHandle(handle, 2);
        if (GetNodeName(up2) == L"ClientAreaPresenter") {
            ApplyMargin(handle, L"0,0,0,0", g_edgeToEdge.load());
        }
    }
}

static bool IsTargetNode(const Node& node) {
    const std::wstring& name = node.name;
    if (!name.empty()) {
        if (name == L"NavigationBarHost" || name == L"NavigationBarContent" ||
            name == L"NavigationChromeHost" || name == L"BottomSeparator" ||
            name == L"SidebarToggleButton" || name == L"ChatButtonHost" ||
            name == L"AssistantBar" || name == L"UrlBarTrailingStack" ||
            name == L"MinimizeButton" || name == L"MaximizeButton" ||
            name == L"CloseButton" || name == L"PersonalizeButton" ||
            name == L"AdvancedChatUpsellHost" || name == L"ChatUsageCard" ||
            name == L"ChatUsageButton" || name == L"ChatUsageMessage" ||
            name == L"OverflowButton" || name == L"LoadingIndicatorLayer" ||
            name == L"SidebarBorder" ||
            name == L"SidebarFloatingSurface" || name == L"SidebarFloatingTint" ||
            name == L"StatusText" || name == L"StatusPill" ||
            name == L"ActionsRow" || name == L"DownloadsFlyout" ||
            name == L"ToolbarArea" || name == L"advanced" ||
            name == L"SpaceSwitcherHost" || name == L"SpaceSwitcherButton" ||
            name == L"SpaceIndicatorsViewport" || name == L"SpaceIndicatorsPanel" ||
            name == L"ContentGroup")
            return true;
    }
    if (node.type.find(L"Grid") != std::wstring::npos && node.childIndex == 0) return true;
    if (node.type.find(L"Border") != std::wstring::npos && (node.childIndex == 1 || node.childIndex == 2)) return true;
    return false;
}

namespace SettingsUi {
    void CheckActiveSettingsPage();
}

// Applies everything on the UI thread that owns the elements. A snapshot is
// taken first because SetProperty can synchronously add/remove nodes.
static void ApplyAllOnThisThread() {
    IsNewTab();
    std::vector<Target> targets;
    const DWORD thread = GetCurrentThreadId();
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        targets.reserve(g_nodes.size());
        for (const auto& [handle, node] : g_nodes) {
            if (node.thread != thread) continue;
            if (!IsTargetNode(node)) continue;
            Target target;
            target.handle = handle;
            target.name = node.name;
            target.type = node.type;
            target.childIndex = node.childIndex;
            auto parent = g_nodes.find(node.parent);
            if (parent != g_nodes.end()) target.parent = parent->second.name;
            targets.push_back(std::move(target));
        }
    }
    for (const auto& target : targets)
        EvaluateNode(target, thread);

    UpdateHiddenToolbar();
    UpdateSquareCorners();
    UpdateWindowMaterial();
    UpdateSidebarTranslucency();
    UpdateCleanSearchBox();
    UpdateNewTabPainting();
    UpdateDownloadsPopup();
    UpdatePinnedTabBorder();
    ArcUpdatePinnedTabs();
    ArcUpdatePinnedMarks();
    UpdateProfileEmoji();
    UpdateHideLinkAddress();
    SettingsUi::CheckActiveSettingsPage();
    UpdateSegoeUiVariable();
    UpdateNotoEmoji();
}

// ---------------------------------------------------------------- DWM & state sync

static bool IsMainDiaWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return false;

    // Filter by style: child controls have WS_CHILD
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0) return false;

    // Filter out toolwindows / floating tooltips
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) return false;

    // Main window and Settings window class in WASDK / WinUI 3
    wchar_t cls[128]{};
    if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) || wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0) {
        return false;
    }

    // Size check: main and settings windows have dimensions (>= 320x240)
    RECT rc{};
    if (GetClientRect(hwnd, &rc)) {
        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;
        if (width < 320 || height < 240) return false;
    }

    return true;
}

static void ApplyDwmToProcessWindows() {
    DWORD currentPid = GetCurrentProcessId();
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        if (!IsMainDiaWindow(hwnd)) return TRUE;

        // Border: 0xFFFFFFFE (DWMWA_COLOR_NONE) disables the window border for edgeToEdge,
        // 0xFFFFFFFF (DWMWA_COLOR_DEFAULT) restores Windows 11 default border.
        COLORREF border = g_edgeToEdge.load() ? 0xFFFFFFFE : 0xFFFFFFFF;
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));

        // Window corners: 1 (DWMWCP_DONOTROUND) if squareSidebarCorners is enabled,
        // 2 (DWMWCP_ROUND) to restore Windows 11 rounded corners when disabled.
        int corners = g_squareSidebarCorners.load() ? 1 /* DWMWCP_DONOTROUND */ : 2 /* DWMWCP_ROUND */;
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));

        // Material: DWMSBT_AUTO (0), DWMSBT_NONE (1), DWMSBT_MAINWINDOW (2), DWMSBT_TRANSIENTWINDOW (3), DWMSBT_TABBEDWINDOW (4)
        int material = 0; // DWMSBT_AUTO (0 restores default window rendering)
        if (g_cfg.windowMaterial == L"mica") material = 2;
        else if (g_cfg.windowMaterial == L"acrylic") material = 3;
        else if (g_cfg.windowMaterial == L"micaalt") material = 4;
        else material = 0; // default or none -> DWMSBT_AUTO so DWM does not force black!
        DwmSetWindowAttribute(hwnd, 38 /* DWMWA_SYSTEMBACKDROP_TYPE */, &material, sizeof(material));
        return TRUE;
    }, (LPARAM)currentPid);
}

static void UpdateStateJson(const std::wstring& key, const std::wstring& value) {
    if (!g_statePath[0]) return;
    HANDLE file = CreateFileW(g_statePath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(file, nullptr);
    if (size == 0 || size > 1024 * 1024) { CloseHandle(file); return; }
    std::string content(size, '\0');
    DWORD read = 0;
    ReadFile(file, &content[0], size, &read, nullptr);
    CloseHandle(file);

    char k8[256]{};
    WideCharToMultiByte(CP_UTF8, 0, key.c_str(), -1, k8, sizeof(k8), nullptr, nullptr);

    std::string newVal;
    if (value == L"1" || value == L"true") newVal = "true";
    else if (value == L"0" || value == L"false") newVal = "false";
    else if (!value.empty() && iswdigit(value[0])) {
        char num[64]{};
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, num, sizeof(num), nullptr, nullptr);
        newVal = num;
    } else {
        char str[256]{};
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, str, sizeof(str), nullptr, nullptr);
        newVal = std::string("\"") + str + "\"";
    }

    std::string search = std::string("\"") + k8 + "\"";
    size_t pos = content.find(search);
    if (pos == std::string::npos) {
        size_t tweaksPos = content.find("\"tweaks\"");
        if (tweaksPos != std::string::npos) {
            size_t openBrace = content.find('{', tweaksPos);
            if (openBrace != std::string::npos) {
                std::string insertStr = "\n                   \"" + std::string(k8) + "\":  " + newVal + ",";
                content.insert(openBrace + 1, insertStr);
                file = CreateFileW(g_statePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
                    CloseHandle(file);
                }
            }
        }
        return;
    }

    size_t colon = content.find(':', pos + search.size());
    if (colon == std::string::npos) return;

    size_t valStart = content.find_first_not_of(" \t\r\n", colon + 1);
    if (valStart == std::string::npos) return;

    size_t valEnd = content.find_first_of(",\r\n}", valStart);
    if (valEnd == std::string::npos) valEnd = content.size();

    content.replace(valStart, valEnd - valStart, newVal);

    file = CreateFileW(g_statePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
        CloseHandle(file);
    }
}

// ---------------------------------------------------------------- Settings -> Patches Page

namespace SettingsUi {

struct CardLook {
    muxm::Brush background{nullptr};
    muxm::Brush border{nullptr};
    mux::CornerRadius radius{8, 8, 8, 8};
    bool fromDia = false;
};
static thread_local CardLook t_card;

static winrt::Windows::Foundation::IInspectable Resource(const wchar_t* key) {
    try {
        auto res = mux::Application::Current().Resources();
        auto k = winrt::box_value(key);
        if (auto v = res.TryLookup(k)) return v;
    } catch (...) {}
    return nullptr;
}

static muxm::Brush BrushRes(const wchar_t* key, winrt::Windows::UI::Color fallback) {
    if (auto b = Resource(key).try_as<muxm::Brush>()) return b;
    return muxm::SolidColorBrush(fallback);
}

static void CaptureCardLook(const mux::DependencyObject& root) {
    auto found = FindVisual(root, [](const mux::DependencyObject& d) {
        auto b = d.try_as<muxc::Border>();
        return b && b.BorderThickness().Left == 1 && b.MinHeight() == 40 && b.Background() != nullptr;
    });
    if (auto b = found.try_as<muxc::Border>()) {
        t_card.background = b.Background();
        t_card.border = b.BorderBrush();
        t_card.radius = b.CornerRadius();
        t_card.fromDia = true;
    }
}

static void EnsureCardLook() {
    if (t_card.background) return;
    t_card.background = BrushRes(L"CardBackgroundFillColorDefaultBrush", winrt::Windows::UI::Color{255, 0x2b, 0x2b, 0x2b});
    t_card.border = BrushRes(L"CardStrokeColorDefaultBrush", winrt::Windows::UI::Color{255, 0x1c, 0x1c, 0x1c});
}

struct NavHook {
    winrt::weak_ref<muxc::NavigationView> nav;
    muxc::NavigationViewItem item{nullptr};
    mux::FrameworkElement page{nullptr};
    winrt::Windows::Foundation::IInspectable saved{nullptr};
    InstanceHandle handle = 0;
};
static thread_local std::vector<NavHook> t_navs;

struct BoundToggle {
    std::wstring key;
    winrt::weak_ref<muxc::ToggleSwitch> sw;
};
static thread_local std::vector<BoundToggle> t_toggles;

struct BoundChoice {
    std::wstring key;
    winrt::weak_ref<muxc::ComboBox> box;
};
static thread_local std::vector<BoundChoice> t_choices;

static thread_local winrt::weak_ref<muxc::ComboBox> t_presetBox;
static thread_local bool t_syncing = false;

struct TweakOption {
    std::wstring value;
    std::wstring label;
};

struct TweakDef {
    std::wstring key;
    std::wstring title;
    std::wstring desc;
    const wchar_t* glyph = nullptr;
    std::vector<TweakOption> choices;
};

struct GrainControlState {
    winrt::weak_ref<mux::FrameworkElement> card;
    winrt::weak_ref<muxc::Border> previewBar;
    winrt::weak_ref<muxc::Slider> amountSlider;
    winrt::weak_ref<muxc::TextBlock> amountLabel;
    winrt::weak_ref<muxc::Slider> softnessSlider;
    winrt::weak_ref<muxc::TextBlock> softnessLabel;
};
static thread_local GrainControlState t_grainControls;

static bool GetTweakBool(const std::wstring& key) {
    if (key == L"hideToolbar") return g_cfg.hideToolbar;
    if (key == L"hideSidebarButton") return g_cfg.hideSidebarButton;
    if (key == L"hideChatButton") return g_cfg.hideChatButton;
    if (key == L"showAddressField") return g_cfg.showAddressField;
    if (key == L"hideTrafficLights") return g_cfg.hideTrafficLights;
    if (key == L"hidePersonalizeButton") return g_cfg.hidePersonalizeButton;
    if (key == L"hideUpgradeBanner") return g_cfg.hideUpgradeBanner;
    if (key == L"hideTabSearchButton") return g_cfg.hideTabSearchButton;
    if (key == L"hideLoadingAnimation") return g_cfg.hideLoadingAnimation;
    if (key == L"hidePinnedTabBorder") return g_cfg.hidePinnedTabBorder;
    if (key == L"translucentSidebar") return g_cfg.translucentSidebar;
    if (key == L"showToolbarWithSidebar") return g_cfg.showToolbarWithSidebar;
    if (key == L"showToolbarOnNewTab") return g_cfg.showToolbarOnNewTab;
    if (key == L"cleanSearchBox") return g_cfg.cleanSearchBox;
    if (key == L"newTabPainting") return g_cfg.newTabPainting;
    if (key == L"edgeToEdge") return g_cfg.edgeToEdge;
    if (key == L"revealToolbarOnHover") return g_cfg.revealToolbarOnHover;
    if (key == L"toolbarColor") return g_cfg.toolbarColor;
    if (key == L"peek") return g_cfg.peek;
    if (key == L"profileEmoji") return g_cfg.profileEmoji;
    if (key == L"sidebarGrain") return g_cfg.sidebarGrain;
    if (key == L"hideTabHoverCard") return g_cfg.hideTabHoverCard;
    if (key == L"hideLinkAddress") return g_cfg.hideLinkAddress;
    if (key == L"arcPinnedTabs") return g_cfg.arcPinnedTabs;
    if (key == L"hideHistorySuggestions") return g_cfg.hideHistorySuggestions;
    if (key == L"dontPopUpDownloads") return g_cfg.dontPopUpDownloads;
    if (key == L"squareSidebarCorners") return g_cfg.squareSidebarCorners;
    if (key == L"keepWindowsActive") return g_cfg.keepWindowsActive;
    if (key == L"normalLaunch") return g_cfg.normalLaunch;
    if (key == L"segoeUiVariable") return g_cfg.segoeUiVariable;
    if (key == L"notoEmoji") return g_cfg.notoEmoji;
    if (key == L"developerMode") return g_cfg.developerMode;
    if (key == L"developerModeLocalhost") return g_cfg.developerModeLocalhost;
    return false;
}

static std::wstring GetTweakString(const std::wstring& key) {
    if (key == L"windowMaterial") return g_cfg.windowMaterial;
    if (key == L"hideToolbarMode") return g_cfg.hideToolbarMode;
    if (key == L"sidebarGrainMaterial") return g_cfg.sidebarGrainMaterial;
    if (key == L"sidebarGrainAmount") return std::to_wstring(g_cfg.sidebarGrainAmount);
    if (key == L"sidebarGrainSoftness") return std::to_wstring(g_cfg.sidebarGrainSoftness);
    if (key == L"pinnedExtensions") return std::to_wstring(g_cfg.pinnedExtensions);
    if (key == L"preset") return g_cfg.preset;
    if (key == L"peekTrigger") return g_cfg.peekTrigger;
    if (key == L"peekAnimation") return g_cfg.peekAnimation;
    if (key == L"profileEmojiText") return g_cfg.profileEmojiText;
    if (key == L"notoEmojiFont") return g_cfg.notoEmojiFont;
    return L"";
}

struct EmojiControlState {
    winrt::weak_ref<mux::FrameworkElement> card;
    winrt::weak_ref<muxc::TextBox> textBox;
};
static thread_local EmojiControlState t_emojiControls;

struct PaintingControlState {
    winrt::weak_ref<mux::FrameworkElement> card;
    winrt::weak_ref<muxc::Slider> tintSlider;
    winrt::weak_ref<muxc::TextBlock> tintLabel;
};
static thread_local PaintingControlState t_paintingControls;

struct MaterialControlState {
    winrt::weak_ref<mux::FrameworkElement> card;
    winrt::weak_ref<muxc::Slider> slider;
    winrt::weak_ref<muxc::TextBlock> label;
};
static thread_local MaterialControlState t_materialControls;

static void SyncToggles() {
    t_syncing = true;
    for (auto& b : t_toggles) {
        if (auto sw = b.sw.get()) {
            bool want = GetTweakBool(b.key);
            if (sw.IsOn() != want) sw.IsOn(want);
        }
    }
    for (auto& c : t_choices) {
        if (auto box = c.box.get()) {
            std::wstring want = GetTweakString(c.key);
            for (uint32_t i = 0; i < box.Items().Size(); i++) {
                if (auto ci = box.Items().GetAt(i).try_as<muxc::ComboBoxItem>()) {
                    std::wstring tag(winrt::unbox_value_or<winrt::hstring>(ci.Tag(), L""));
                    if (tag == want) {
                        if (box.SelectedIndex() != (int)i) box.SelectedIndex((int)i);
                        break;
                    }
                }
            }
        }
    }
    if (auto pb = t_presetBox.get()) {
        std::wstring want = g_cfg.preset;
        for (uint32_t i = 0; i < pb.Items().Size(); i++) {
            if (auto ci = pb.Items().GetAt(i).try_as<muxc::ComboBoxItem>()) {
                std::wstring tag(winrt::unbox_value_or<winrt::hstring>(ci.Tag(), L""));
                if (tag == want) {
                    if (pb.SelectedIndex() != (int)i) pb.SelectedIndex((int)i);
                    break;
                }
            }
        }
    }
    if (auto c = t_grainControls.card.get()) {
        c.Visibility(g_cfg.sidebarGrain ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    if (auto as = t_grainControls.amountSlider.get()) {
        if (static_cast<int>(as.Value()) != g_cfg.sidebarGrainAmount) {
            as.Value(g_cfg.sidebarGrainAmount);
        }
    }
    if (auto al = t_grainControls.amountLabel.get()) {
        al.Text(std::to_wstring(g_cfg.sidebarGrainAmount) + L"%");
    }
    if (auto ss = t_grainControls.softnessSlider.get()) {
        if (static_cast<int>(ss.Value()) != g_cfg.sidebarGrainSoftness) {
            ss.Value(g_cfg.sidebarGrainSoftness);
        }
    }
    if (auto sl = t_grainControls.softnessLabel.get()) {
        sl.Text(std::to_wstring(g_cfg.sidebarGrainSoftness) + L"%");
    }
    if (auto pbar = t_grainControls.previewBar.get()) {
        auto brush = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial, g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness);
        if (brush) pbar.Background(brush);
    }
    if (auto c = t_emojiControls.card.get()) {
        c.Visibility(g_cfg.profileEmoji ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    if (auto tb = t_emojiControls.textBox.get()) {
        tb.Text(g_cfg.profileEmojiText.empty() ? L"🦊" : g_cfg.profileEmojiText);
    }
    if (auto c = t_paintingControls.card.get()) {
        c.Visibility(g_cfg.newTabPainting ? mux::Visibility::Visible : mux::Visibility::Collapsed);
    }
    if (auto ts = t_paintingControls.tintSlider.get()) {
        if (static_cast<int>(ts.Value()) != g_cfg.newTabPaintingTint) {
            ts.Value(g_cfg.newTabPaintingTint);
        }
    }
    if (auto tl = t_paintingControls.tintLabel.get()) {
        tl.Text(std::to_wstring(g_cfg.newTabPaintingTint) + L"%");
    }
    if (auto c = t_materialControls.card.get()) {
        bool materialOn = !g_cfg.windowMaterial.empty() && g_cfg.windowMaterial != L"default" &&
                          g_cfg.windowMaterial != L"none";
        c.Visibility(g_cfg.translucentSidebar && materialOn ? mux::Visibility::Visible
                                                           : mux::Visibility::Collapsed);
    }
    if (auto s = t_materialControls.slider.get()) {
        if (static_cast<int>(s.Value()) != g_cfg.materialTransparency) {
            s.Value(g_cfg.materialTransparency);
        }
    }
    if (auto l = t_materialControls.label.get()) {
        l.Text(std::to_wstring(g_cfg.materialTransparency) + L"%");
    }
    t_syncing = false;
}

static void SetTweak(const std::wstring& key, const std::wstring& value) {
    WritePrivateProfileStringW(L"tweaks", key.c_str(), value.c_str(), g_configPath);
    UpdateStateJson(key, value);

    if (key != L"preset") {
        WritePrivateProfileStringW(L"tweaks", L"preset", L"custom", g_configPath);
        UpdateStateJson(L"preset", L"custom");
        g_cfg.preset = L"custom";
    }

    if (key == L"sidebarGrainMaterial" || key == L"sidebarGrainAmount" || key == L"sidebarGrainSoftness") {
        g_grainBrush = nullptr;
    }

    ReadConfig();

    if (key == L"edgeToEdge" || key == L"squareSidebarCorners" || key == L"windowMaterial") {
        ApplyDwmToProcessWindows();
    }

    ApplyAllOnThisThread();
    SyncToggles();
}

static void ApplyPreset(const std::wstring& preset) {
    if (preset == L"custom") {
        WritePrivateProfileStringW(L"tweaks", L"preset", L"custom", g_configPath);
        UpdateStateJson(L"preset", L"custom");
        g_cfg.preset = L"custom";
        SyncToggles();
        return;
    }

    if (preset == L"none") {
        const wchar_t* allKeys[] = {
            L"edgeToEdge", L"squareSidebarCorners", L"hideToolbar", L"toolbarColor",
            L"revealToolbarOnHover", L"showToolbarWithSidebar", L"showToolbarOnNewTab",
            L"showAddressField", L"hideSidebarButton", L"peek", L"hideTrafficLights",
            L"hideChatButton", L"hidePersonalizeButton", L"hideUpgradeBanner",
            L"hideTabSearchButton", L"profileEmoji", L"sidebarGrain", L"pinnedExtensions",
            L"hideLoadingAnimation", L"hidePinnedTabBorder", L"hideTabHoverCard",
            L"translucentSidebar", L"dontPopUpDownloads", L"cleanSearchBox", L"newTabPainting", L"hideLinkAddress",
            L"keepWindowsActive"
        };
        for (const auto* k : allKeys) {
            WritePrivateProfileStringW(L"tweaks", k, L"0", g_configPath);
            UpdateStateJson(k, L"0");
        }
        WritePrivateProfileStringW(L"tweaks", L"windowMaterial", L"default", g_configPath);
        UpdateStateJson(L"windowMaterial", L"default");
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainAmount", L"50", g_configPath);
        UpdateStateJson(L"sidebarGrainAmount", L"50");
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainSoftness", L"0", g_configPath);
        UpdateStateJson(L"sidebarGrainSoftness", L"0");
        WritePrivateProfileStringW(L"tweaks", L"newTabPaintingTint", L"67", g_configPath);
        UpdateStateJson(L"newTabPaintingTint", L"67");
        WritePrivateProfileStringW(L"tweaks", L"preset", L"none", g_configPath);
        UpdateStateJson(L"preset", L"none");
    } else if (preset == L"arc") {
        const wchar_t* arcOn[] = {
            L"edgeToEdge", L"squareSidebarCorners", L"hideToolbar", L"peek",
            L"hideTabHoverCard", L"cleanSearchBox", L"hideLinkAddress", L"sidebarGrain"
        };
        for (const auto* k : arcOn) {
            WritePrivateProfileStringW(L"tweaks", k, L"1", g_configPath);
            UpdateStateJson(k, L"1");
        }
        WritePrivateProfileStringW(L"tweaks", L"windowMaterial", L"acrylic", g_configPath);
        UpdateStateJson(L"windowMaterial", L"acrylic");
        WritePrivateProfileStringW(L"tweaks", L"preset", L"arc", g_configPath);
        UpdateStateJson(L"preset", L"arc");
    } else if (preset == L"arcWindows") {
        const wchar_t* arcWinOn[] = {
            L"edgeToEdge", L"squareSidebarCorners", L"toolbarColor", L"peek",
            L"hideTabHoverCard", L"cleanSearchBox", L"hideLinkAddress", L"sidebarGrain"
        };
        for (const auto* k : arcWinOn) {
            WritePrivateProfileStringW(L"tweaks", k, L"1", g_configPath);
            UpdateStateJson(k, L"1");
        }
        WritePrivateProfileStringW(L"tweaks", L"windowMaterial", L"acrylic", g_configPath);
        UpdateStateJson(L"windowMaterial", L"acrylic");
        WritePrivateProfileStringW(L"tweaks", L"preset", L"arcWindows", g_configPath);
        UpdateStateJson(L"preset", L"arcWindows");
    }

    ReadConfig();
    ApplyDwmToProcessWindows();
    ApplyAllOnThisThread();
    SyncToggles();
}

static mux::FrameworkElement BuildRow(const TweakDef& t) {
    muxc::Grid row;
    row.Padding(mux::Thickness{12, 8, 12, 8});
    row.MinHeight(40);
    row.ColumnSpacing(12);

    muxc::ColumnDefinition c0, c1, c2;
    c0.Width(mux::GridLengthHelper::Auto());
    c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
    c2.Width(mux::GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnDefinitions().Append(c2);

    muxc::FontIcon icon;
    icon.Glyph(t.glyph ? t.glyph : L"\uE713");
    icon.FontSize(16);
    icon.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(icon, 0);
    row.Children().Append(icon);

    muxc::StackPanel text;
    text.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::TextBlock title;
    title.Text(t.title);
    title.TextWrapping(mux::TextWrapping::Wrap);
    text.Children().Append(title);

    if (!t.desc.empty()) {
        muxc::TextBlock d;
        d.Text(t.desc);
        d.FontSize(12);
        d.TextWrapping(mux::TextWrapping::Wrap);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
    }
    muxc::Grid::SetColumn(text, 1);
    row.Children().Append(text);

    muxc::StackPanel controls;
    controls.Orientation(muxc::Orientation::Horizontal);
    controls.Spacing(12);
    controls.VerticalAlignment(mux::VerticalAlignment::Center);

    std::wstring key = t.key;

    if (!t.choices.empty()) {
        muxc::ComboBox box;
        box.MinWidth(130);
        std::wstring currentVal = GetTweakString(key);
        int sel = 0, i = 0;
        for (const auto& opt : t.choices) {
            muxc::ComboBoxItem ci;
            ci.Content(winrt::box_value(opt.label));
            ci.Tag(winrt::box_value(opt.value));
            box.Items().Append(ci);
            if (opt.value == currentVal) sel = i;
            i++;
        }
        box.SelectedIndex(sel);
        muxa::AutomationProperties::SetAutomationId(box, L"patch_" + key);
        muxa::AutomationProperties::SetName(box, t.title);
        box.SelectionChanged([key](winrt::Windows::Foundation::IInspectable const& s, auto&&) {
            if (t_syncing) return;
            auto ci = s.as<muxc::ComboBox>().SelectedItem().try_as<muxc::ComboBoxItem>();
            if (!ci) return;
            std::wstring val(winrt::unbox_value_or<winrt::hstring>(ci.Tag(), L""));
            SetTweak(key, val);
        });
        t_choices.push_back({key, winrt::make_weak(box)});
        controls.Children().Append(box);
    } else {
        muxc::ToggleSwitch sw;
        sw.MinWidth(0);
        sw.OnContent(winrt::box_value(L"On"));
        sw.OffContent(winrt::box_value(L"Off"));
        sw.IsOn(GetTweakBool(key));
        muxa::AutomationProperties::SetAutomationId(sw, L"patch_" + key);
        muxa::AutomationProperties::SetName(sw, t.title);
        sw.Toggled([key](winrt::Windows::Foundation::IInspectable const& s, auto&&) {
            if (t_syncing) return;
            bool on = s.as<muxc::ToggleSwitch>().IsOn();
            SetTweak(key, on ? L"1" : L"0");
        });
        t_toggles.push_back({key, winrt::make_weak(sw)});
        controls.Children().Append(sw);
    }

    muxc::Grid::SetColumn(controls, 2);
    row.Children().Append(controls);
    return row;
}

static mux::FrameworkElement BuildMaterialOptionsRow() {
    muxc::StackPanel card;
    card.Padding(mux::Thickness{16, 12, 16, 14});
    card.Spacing(10);
    card.Background(BrushRes(L"CardBackgroundFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0x24, 0x24, 0x24}));
    card.BorderBrush(t_card.border);
    card.BorderThickness(mux::Thickness{1, 1, 1, 1});
    card.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    card.Margin(mux::Thickness{12, 6, 12, 6});

    muxc::Grid headerGrid;
    muxc::ColumnDefinition hc0, hc1;
    hc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    hc1.Width(mux::GridLengthHelper::Auto());
    headerGrid.ColumnDefinitions().Append(hc0);
    headerGrid.ColumnDefinitions().Append(hc1);

    muxc::TextBlock header;
    header.Text(L"Translucency");
    header.FontSize(13);
    header.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    muxc::Grid::SetColumn(header, 0);
    headerGrid.Children().Append(header);

    muxc::TextBlock percent;
    percent.Text(std::to_wstring(g_cfg.materialTransparency) + L"%");
    percent.FontSize(13);
    percent.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    muxc::Grid::SetColumn(percent, 1);
    headerGrid.Children().Append(percent);
    card.Children().Append(headerGrid);

    muxc::TextBlock hint;
    hint.Text(L"How much the space colour fades while the sidebar is open. 100% leaves only the window material.");
    hint.FontSize(12);
    hint.TextWrapping(mux::TextWrapping::Wrap);
    hint.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    card.Children().Append(hint);

    muxc::Slider slider;
    slider.Minimum(0);
    slider.Maximum(100);
    slider.StepFrequency(1);
    slider.Value(g_cfg.materialTransparency);
    slider.Margin(mux::Thickness{0, -4, 0, 4});
    card.Children().Append(slider);

    slider.ValueChanged([percent](auto&&, muxc::Primitives::RangeBaseValueChangedEventArgs const& e) {
        if (t_syncing) return;
        int val = static_cast<int>(e.NewValue());
        percent.Text(std::to_wstring(val) + L"%");
        g_cfg.materialTransparency = val;
        g_materialTransparency.store(val);
        ApplyAllOnThisThread();
        WritePrivateProfileStringW(L"tweaks", L"materialTransparency", std::to_wstring(val).c_str(), g_configPath);
        UpdateStateJson(L"materialTransparency", std::to_wstring(val));
    });

    t_materialControls.card = winrt::make_weak(card.as<mux::FrameworkElement>());
    t_materialControls.slider = winrt::make_weak(slider);
    t_materialControls.label = winrt::make_weak(percent);

    bool materialOn = !g_cfg.windowMaterial.empty() && g_cfg.windowMaterial != L"default" &&
                      g_cfg.windowMaterial != L"none";
    card.Visibility(g_cfg.translucentSidebar && materialOn ? mux::Visibility::Visible
                                                           : mux::Visibility::Collapsed);
    return card;
}

static mux::FrameworkElement BuildGrainOptionsRow() {
    muxc::StackPanel card;
    card.Padding(mux::Thickness{16, 12, 16, 14});
    card.Spacing(10);
    card.Background(BrushRes(L"CardBackgroundFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0x24, 0x24, 0x24}));
    card.BorderBrush(t_card.border);
    card.BorderThickness(mux::Thickness{1, 1, 1, 1});
    card.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    card.Margin(mux::Thickness{12, 6, 12, 6});

    // Header: "Options"
    muxc::TextBlock header;
    header.Text(L"Options");
    header.FontSize(13);
    header.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    header.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    card.Children().Append(header);

    // 1. Grain row: "Grain" on left, "XX%" on right
    muxc::Grid grainHeaderGrid;
    muxc::ColumnDefinition ghc0, ghc1;
    ghc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    ghc1.Width(mux::GridLengthHelper::Auto());
    grainHeaderGrid.ColumnDefinitions().Append(ghc0);
    grainHeaderGrid.ColumnDefinitions().Append(ghc1);

    muxc::TextBlock grainLabel;
    grainLabel.Text(L"Grain");
    grainLabel.FontSize(13);
    grainLabel.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    muxc::Grid::SetColumn(grainLabel, 0);
    grainHeaderGrid.Children().Append(grainLabel);

    muxc::TextBlock grainPercent;
    grainPercent.Text(std::to_wstring(g_cfg.sidebarGrainAmount) + L"%");
    grainPercent.FontSize(13);
    grainPercent.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    muxc::Grid::SetColumn(grainPercent, 1);
    grainHeaderGrid.Children().Append(grainPercent);
    card.Children().Append(grainHeaderGrid);

    // Textured preview card
    muxc::Border previewContainer;
    previewContainer.Height(36);
    previewContainer.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
    previewContainer.Background(BrushRes(L"ControlFillColorDefaultBrush", winrt::Windows::UI::Color{255, 0x30, 0x30, 0x30}));
    previewContainer.BorderBrush(BrushRes(L"CardStrokeColorDefaultBrush", winrt::Windows::UI::Color{255, 0x48, 0x48, 0x48}));
    previewContainer.BorderThickness(mux::Thickness{1, 1, 1, 1});

    muxc::Border previewBar;
    previewBar.Height(36);
    previewBar.CornerRadius(mux::CornerRadius{8, 8, 8, 8});
    auto brush = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial, g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness);
    if (brush) previewBar.Background(brush);
    previewContainer.Child(previewBar);
    card.Children().Append(previewContainer);

    // Grain slider
    muxc::Slider grainSlider;
    grainSlider.Minimum(0);
    grainSlider.Maximum(100);
    grainSlider.StepFrequency(1);
    grainSlider.Value(g_cfg.sidebarGrainAmount);
    grainSlider.Margin(mux::Thickness{0, -4, 0, 4});
    card.Children().Append(grainSlider);

    // 2. Softness row
    muxc::Grid softHeaderGrid;
    muxc::ColumnDefinition shc0, shc1;
    shc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    shc1.Width(mux::GridLengthHelper::Auto());
    softHeaderGrid.ColumnDefinitions().Append(shc0);
    softHeaderGrid.ColumnDefinitions().Append(shc1);

    muxc::TextBlock softLabel;
    softLabel.Text(L"Softness");
    softLabel.FontSize(13);
    softLabel.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    muxc::Grid::SetColumn(softLabel, 0);
    softHeaderGrid.Children().Append(softLabel);

    muxc::TextBlock softPercent;
    softPercent.Text(std::to_wstring(g_cfg.sidebarGrainSoftness) + L"%");
    softPercent.FontSize(13);
    softPercent.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    muxc::Grid::SetColumn(softPercent, 1);
    softHeaderGrid.Children().Append(softPercent);
    card.Children().Append(softHeaderGrid);

    // Softness slider
    muxc::Slider softSlider;
    softSlider.Minimum(0);
    softSlider.Maximum(100);
    softSlider.StepFrequency(1);
    softSlider.Value(g_cfg.sidebarGrainSoftness);
    softSlider.Margin(mux::Thickness{0, -4, 0, 4});
    card.Children().Append(softSlider);

    // 3. Reset button
    muxc::Grid btnGrid;
    muxc::ColumnDefinition bgc0, bgc1;
    bgc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    bgc1.Width(mux::GridLengthHelper::Auto());
    btnGrid.ColumnDefinitions().Append(bgc0);
    btnGrid.ColumnDefinitions().Append(bgc1);

    muxc::Button resetBtn;
    resetBtn.Content(winrt::box_value(L"Reset"));
    resetBtn.Padding(mux::Thickness{16, 4, 16, 4});
    muxc::Grid::SetColumn(resetBtn, 1);
    btnGrid.Children().Append(resetBtn);
    card.Children().Append(btnGrid);

    // Event handlers
    grainSlider.ValueChanged([grainPercent, previewBar](auto&&, muxc::Primitives::RangeBaseValueChangedEventArgs const& e) {
        if (t_syncing) return;
        int val = static_cast<int>(e.NewValue());
        grainPercent.Text(std::to_wstring(val) + L"%");
        g_cfg.sidebarGrainAmount = val;
        g_grainBrush = nullptr;
        auto b = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial, g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness);
        if (b) previewBar.Background(b);
        ApplyAllOnThisThread();
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainAmount", std::to_wstring(val).c_str(), g_configPath);
        UpdateStateJson(L"sidebarGrainAmount", std::to_wstring(val));
    });

    softSlider.ValueChanged([softPercent, previewBar](auto&&, muxc::Primitives::RangeBaseValueChangedEventArgs const& e) {
        if (t_syncing) return;
        int val = static_cast<int>(e.NewValue());
        softPercent.Text(std::to_wstring(val) + L"%");
        g_cfg.sidebarGrainSoftness = val;
        g_grainBrush = nullptr;
        auto b = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial, g_cfg.sidebarGrainAmount, g_cfg.sidebarGrainSoftness);
        if (b) previewBar.Background(b);
        ApplyAllOnThisThread();
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainSoftness", std::to_wstring(val).c_str(), g_configPath);
        UpdateStateJson(L"sidebarGrainSoftness", std::to_wstring(val));
    });

    resetBtn.Click([grainSlider, softSlider, grainPercent, softPercent, previewBar](auto&&, auto&&) {
        g_cfg.sidebarGrainAmount = 50;
        g_cfg.sidebarGrainSoftness = 0;
        grainSlider.Value(50);
        softSlider.Value(0);
        grainPercent.Text(L"50%");
        softPercent.Text(L"0%");
        g_grainBrush = nullptr;
        auto b = GetOrCreateGrainBrush(g_cfg.sidebarGrainMaterial, 50, 0);
        if (b) previewBar.Background(b);
        ApplyAllOnThisThread();
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainAmount", L"50", g_configPath);
        WritePrivateProfileStringW(L"tweaks", L"sidebarGrainSoftness", L"0", g_configPath);
        UpdateStateJson(L"sidebarGrainAmount", L"50");
        UpdateStateJson(L"sidebarGrainSoftness", L"0");
    });

    t_grainControls.card = winrt::make_weak(card.as<mux::FrameworkElement>());
    t_grainControls.previewBar = winrt::make_weak(previewBar);
    t_grainControls.amountSlider = winrt::make_weak(grainSlider);
    t_grainControls.amountLabel = winrt::make_weak(grainPercent);
    t_grainControls.softnessSlider = winrt::make_weak(softSlider);
    t_grainControls.softnessLabel = winrt::make_weak(softPercent);
    card.Visibility(g_cfg.sidebarGrain ? mux::Visibility::Visible : mux::Visibility::Collapsed);

    return card;
}

static mux::FrameworkElement BuildProfileEmojiOptionsRow() {
    muxc::StackPanel card;
    card.Padding(mux::Thickness{16, 12, 16, 14});
    card.Spacing(10);
    card.Background(BrushRes(L"CardBackgroundFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0x24, 0x24, 0x24}));
    card.BorderBrush(t_card.border);
    card.BorderThickness(mux::Thickness{1, 1, 1, 1});
    card.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    card.Margin(mux::Thickness{12, 6, 12, 6});

    muxc::TextBlock header;
    header.Text(L"Choose Profile Emoji");
    header.FontSize(13);
    header.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    header.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    card.Children().Append(header);

    muxc::StackPanel pills;
    pills.Orientation(muxc::Orientation::Horizontal);
    pills.Spacing(6);

    const wchar_t* presets[] = { L"🦊", L"🚀", L"💻", L"☕", L"🎨", L"🌟", L"🐱", L"🌸", L"💜", L"🔥" };
    muxc::TextBox tb;
    tb.Text(g_cfg.profileEmojiText.empty() ? L"🦊" : g_cfg.profileEmojiText);
    tb.Width(64);
    tb.FontSize(14);
    tb.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
    tb.HorizontalContentAlignment(mux::HorizontalAlignment::Center);

    for (const wchar_t* em : presets) {
        muxc::Button btn;
        btn.Content(winrt::box_value(em));
        btn.FontSize(14);
        btn.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
        btn.Padding(mux::Thickness{8, 4, 8, 4});
        btn.Click([em, tb](auto&&, auto&&) {
            tb.Text(em);
            g_cfg.profileEmojiText = em;
            WritePrivateProfileStringW(L"tweaks", L"profileEmojiText", em, g_configPath);
            UpdateStateJson(L"profileEmojiText", em);
            UpdateProfileEmoji();
        });
        pills.Children().Append(btn);
    }
    card.Children().Append(pills);

    muxc::StackPanel customRow;
    customRow.Orientation(muxc::Orientation::Horizontal);
    customRow.Spacing(10);
    muxc::TextBlock customLabel;
    customLabel.Text(L"Custom emoji:");
    customLabel.FontSize(12);
    customLabel.VerticalAlignment(mux::VerticalAlignment::Center);
    customLabel.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    customRow.Children().Append(customLabel);
    customRow.Children().Append(tb);

    tb.TextChanged([](auto&& sender, auto&&) {
        if (t_syncing) return;
        auto box = sender.try_as<muxc::TextBox>();
        if (!box) return;
        std::wstring text(box.Text());
        if (!text.empty()) {
            g_cfg.profileEmojiText = text;
            WritePrivateProfileStringW(L"tweaks", L"profileEmojiText", text.c_str(), g_configPath);
            UpdateStateJson(L"profileEmojiText", text);
            UpdateProfileEmoji();
        }
    });

    card.Children().Append(customRow);

    t_emojiControls.card = winrt::make_weak(card.as<mux::FrameworkElement>());
    t_emojiControls.textBox = winrt::make_weak(tb);
    card.Visibility(g_cfg.profileEmoji ? mux::Visibility::Visible : mux::Visibility::Collapsed);

    return card;
}

static mux::FrameworkElement BuildPaintingTintOptionsRow() {
    muxc::StackPanel card;
    card.Padding(mux::Thickness{16, 12, 16, 14});
    card.Spacing(10);
    card.Background(BrushRes(L"CardBackgroundFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0x24, 0x24, 0x24}));
    card.BorderBrush(t_card.border);
    card.BorderThickness(mux::Thickness{1, 1, 1, 1});
    card.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    card.Margin(mux::Thickness{12, 4, 12, 6});

    // Header: "Wallpaper Tint"
    muxc::TextBlock header;
    header.Text(L"Wallpaper Tint");
    header.FontSize(13);
    header.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    header.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    card.Children().Append(header);

    // Row: "Tint Darkness" on left, "XX%" on right
    muxc::Grid tintHeaderGrid;
    muxc::ColumnDefinition thc0, thc1;
    thc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    thc1.Width(mux::GridLengthHelper::Auto());
    tintHeaderGrid.ColumnDefinitions().Append(thc0);
    tintHeaderGrid.ColumnDefinitions().Append(thc1);

    muxc::TextBlock tintLabel;
    tintLabel.Text(L"Tint Darkness");
    tintLabel.FontSize(13);
    tintLabel.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    muxc::Grid::SetColumn(tintLabel, 0);
    tintHeaderGrid.Children().Append(tintLabel);

    muxc::TextBlock tintPercent;
    tintPercent.Text(std::to_wstring(g_cfg.newTabPaintingTint) + L"%");
    tintPercent.FontSize(13);
    tintPercent.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    muxc::Grid::SetColumn(tintPercent, 1);
    tintHeaderGrid.Children().Append(tintPercent);
    card.Children().Append(tintHeaderGrid);

    // Slider: 0% to 100%
    muxc::Slider tintSlider;
    tintSlider.Minimum(0);
    tintSlider.Maximum(100);
    tintSlider.StepFrequency(1);
    tintSlider.Value(g_cfg.newTabPaintingTint);
    tintSlider.Margin(mux::Thickness{0, -4, 0, 4});
    card.Children().Append(tintSlider);

    // Reset button
    muxc::Grid btnGrid;
    muxc::ColumnDefinition bgc0, bgc1;
    bgc0.Width(mux::GridLength{1, mux::GridUnitType::Star});
    bgc1.Width(mux::GridLengthHelper::Auto());
    btnGrid.ColumnDefinitions().Append(bgc0);
    btnGrid.ColumnDefinitions().Append(bgc1);

    muxc::Button resetBtn;
    resetBtn.Content(winrt::box_value(L"Reset"));
    resetBtn.Padding(mux::Thickness{16, 4, 16, 4});
    muxc::Grid::SetColumn(resetBtn, 1);
    btnGrid.Children().Append(resetBtn);
    card.Children().Append(btnGrid);

    // Event handlers
    tintSlider.ValueChanged([tintPercent](auto&&, muxc::Primitives::RangeBaseValueChangedEventArgs const& e) {
        if (t_syncing) return;
        int val = static_cast<int>(e.NewValue());
        tintPercent.Text(std::to_wstring(val) + L"%");
        g_cfg.newTabPaintingTint = val;
        g_newTabPaintingTint.store(val);
        UpdateNewTabPainting();
        WritePrivateProfileStringW(L"tweaks", L"newTabPaintingTint", std::to_wstring(val).c_str(), g_configPath);
        UpdateStateJson(L"newTabPaintingTint", std::to_wstring(val));
    });

    resetBtn.Click([tintSlider, tintPercent](auto&&, auto&&) {
        g_cfg.newTabPaintingTint = 67;
        g_newTabPaintingTint.store(67);
        tintSlider.Value(67);
        tintPercent.Text(L"67%");
        UpdateNewTabPainting();
        WritePrivateProfileStringW(L"tweaks", L"newTabPaintingTint", L"67", g_configPath);
        UpdateStateJson(L"newTabPaintingTint", L"67");
    });

    t_paintingControls.card = winrt::make_weak(card.as<mux::FrameworkElement>());
    t_paintingControls.tintSlider = winrt::make_weak(tintSlider);
    t_paintingControls.tintLabel = winrt::make_weak(tintPercent);
    card.Visibility(g_cfg.newTabPainting ? mux::Visibility::Visible : mux::Visibility::Collapsed);

    return card;
}

static muxc::StackPanel BuildProfileEmojiPicker(const std::wstring& profileName, muxc::TextBlock const& colorLabel) {
    muxc::StackPanel root;
    root.Name(L"diatweaksProfileEmojiRow");
    root.Padding(mux::Thickness{12, 10, 12, 10});
    root.Spacing(8);

    double labelWidth = (colorLabel && colorLabel.ActualWidth() > 0) ? colorLabel.ActualWidth() : 60.0;

    muxc::StackPanel labelRow;
    labelRow.Orientation(muxc::Orientation::Horizontal);
    labelRow.Spacing(12);

    muxc::TextBlock lbl;
    lbl.Text(L"Emoji");
    lbl.FontSize(colorLabel ? colorLabel.FontSize() : 14);
    lbl.FontWeight(colorLabel ? colorLabel.FontWeight() : winrt::Windows::UI::Text::FontWeight{500});
    lbl.VerticalAlignment(mux::VerticalAlignment::Center);
    lbl.Width(labelWidth);
    if (colorLabel && colorLabel.Foreground()) lbl.Foreground(colorLabel.Foreground());
    labelRow.Children().Append(lbl);

    std::wstring currentEmoji = GetProfileEmoji(profileName);

    muxc::TextBox tb;
    tb.Text(currentEmoji);
    tb.Width(52);
    tb.FontSize(15);
    tb.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
    tb.HorizontalContentAlignment(mux::HorizontalAlignment::Center);
    tb.CornerRadius(mux::CornerRadius{6, 6, 6, 6});

    muxc::StackPanel pills;
    pills.Orientation(muxc::Orientation::Horizontal);
    pills.Spacing(6);
    pills.VerticalAlignment(mux::VerticalAlignment::Center);

    // "None" button
    muxc::Button noneBtn;
    noneBtn.Content(winrt::box_value(L"None"));
    noneBtn.FontSize(12);
    noneBtn.Padding(mux::Thickness{8, 4, 8, 4});
    noneBtn.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
    noneBtn.Click([tb, rootWeak = winrt::make_weak(root)](auto&&, auto&&) {
        tb.Text(L"");
        std::wstring pName = L"Personal";
        if (auto r = rootWeak.get()) {
            pName = winrt::unbox_value_or<winrt::hstring>(r.Tag(), L"Personal").c_str();
        }
        SaveProfileEmoji(pName, L"");
        UpdateProfileEmoji();
    });
    pills.Children().Append(noneBtn);

    const wchar_t* presets[] = { L"🦊", L"🚀", L"💻", L"☕", L"🎨", L"🌟", L"🐱", L"🌸", L"💜", L"🔥" };
    for (const wchar_t* em : presets) {
        muxc::Button btn;
        btn.Content(winrt::box_value(em));
        btn.FontSize(14);
        btn.FontFamily(muxm::FontFamily(L"Segoe UI Emoji"));
        btn.Padding(mux::Thickness{7, 4, 7, 4});
        btn.CornerRadius(mux::CornerRadius{6, 6, 6, 6});
        btn.Click([em, tb, rootWeak = winrt::make_weak(root)](auto&&, auto&&) {
            tb.Text(em);
            std::wstring pName = L"Personal";
            if (auto r = rootWeak.get()) {
                pName = winrt::unbox_value_or<winrt::hstring>(r.Tag(), L"Personal").c_str();
            }
            SaveProfileEmoji(pName, em);
            UpdateProfileEmoji();
        });
        pills.Children().Append(btn);
    }
    labelRow.Children().Append(pills);
    root.Children().Append(labelRow);

    muxc::StackPanel customRow;
    customRow.Orientation(muxc::Orientation::Horizontal);
    customRow.Spacing(10);
    double indent = labelWidth + 12.0;
    customRow.Margin(mux::Thickness{indent, 0, 0, 0});

    muxc::TextBlock customLbl;
    customLbl.Text(L"Custom emoji:");
    customLbl.FontSize(12);
    customLbl.VerticalAlignment(mux::VerticalAlignment::Center);
    customLbl.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    customRow.Children().Append(customLbl);
    customRow.Children().Append(tb);

    tb.TextChanged([rootWeak = winrt::make_weak(root)](auto&& sender, auto&&) {
        if (auto box = sender.try_as<muxc::TextBox>()) {
            std::wstring pName = L"Personal";
            if (auto r = rootWeak.get()) {
                pName = winrt::unbox_value_or<winrt::hstring>(r.Tag(), L"Personal").c_str();
            }
            std::wstring txt(box.Text());
            SaveProfileEmoji(pName, txt);
            UpdateProfileEmoji();
        }
    });

    root.Children().Append(customRow);
    return root;
}

static void CheckProfileSettingsPage(muxc::NavigationView const& nav) {
    if (!nav) return;
    auto content = nav.Content().try_as<mux::FrameworkElement>();
    if (!content) return;

    std::wstring profileName;
    FindVisual(content, [&profileName](mux::DependencyObject const& d) {
        if (auto tb = d.try_as<muxc::TextBlock>()) {
            std::wstring txt(tb.Text());
            if (txt.rfind(L"Profiles > ", 0) == 0) {
                profileName = txt.substr(11);
                while (!profileName.empty() && iswspace(profileName.front())) profileName.erase(profileName.begin());
                while (!profileName.empty() && iswspace(profileName.back())) profileName.pop_back();
                return true;
            }
        }
        return false;
    });

    if (profileName.empty()) return;

    muxc::TextBlock colorLabel{nullptr};
    auto colorObj = FindVisual(content, [](mux::DependencyObject const& d) {
        if (auto tb = d.try_as<muxc::TextBlock>()) {
            if (tb.Text() == L"Color") return true;
        }
        return false;
    });
    if (colorObj) colorLabel = colorObj.try_as<muxc::TextBlock>();
    if (!colorLabel) return;

    mux::FrameworkElement colorRowChild{nullptr};
    muxc::Panel cardPanel{nullptr};

    for (mux::DependencyObject curr = colorLabel; curr; ) {
        auto parent = muxm::VisualTreeHelper::GetParent(curr);
        if (!parent) break;

        muxc::Panel candidate{nullptr};
        if (auto pnl = parent.try_as<muxc::Panel>()) {
            candidate = pnl;
        } else if (auto b = parent.try_as<muxc::Border>()) {
            candidate = b.Child().try_as<muxc::Panel>();
        }

        if (candidate) {
            bool hasName = false;
            FindVisual(candidate, [&hasName](mux::DependencyObject const& d) {
                if (auto tb = d.try_as<muxc::TextBlock>()) {
                    if (tb.Text() == L"Name") {
                        hasName = true;
                        return true;
                    }
                }
                return false;
            });

            if (hasName) {
                cardPanel = candidate;
                for (auto const& c : candidate.Children()) {
                    if (auto fe = c.try_as<mux::FrameworkElement>()) {
                        bool found = false;
                        FindVisual(fe, [&found, &colorLabel](mux::DependencyObject const& d) {
                            if (d == colorLabel) { found = true; return true; }
                            return false;
                        });
                        if (found) {
                            colorRowChild = fe;
                            break;
                        }
                    }
                }
                break;
            }
        }
        curr = parent;
    }

    if (!cardPanel || !colorRowChild) return;

    // Clean up any stray/misplaced emoji rows in content outside cardPanel
    FindVisual(content, [&cardPanel](mux::DependencyObject const& d) {
        if (auto pnl = d.try_as<muxc::Panel>(); pnl && pnl != cardPanel) {
            for (int i = (int)pnl.Children().Size() - 1; i >= 0; i--) {
                if (auto fe = pnl.Children().GetAt(i).try_as<mux::FrameworkElement>()) {
                    if (fe.Name() == L"diatweaksProfileEmojiRow" || fe.Name() == L"diatweaksProfileEmojiSep") {
                        pnl.Children().RemoveAt(i);
                    }
                }
            }
        }
        return false;
    });

    mux::FrameworkElement existingRow{nullptr};
    for (auto const& c : cardPanel.Children()) {
        if (auto fe = c.try_as<mux::FrameworkElement>(); fe && fe.Name() == L"diatweaksProfileEmojiRow") {
            existingRow = fe;
            break;
        }
    }

    if (existingRow) {
        std::wstring tag(winrt::unbox_value_or<winrt::hstring>(existingRow.Tag(), L""));
        if (tag != profileName) {
            existingRow.Tag(winrt::box_value(winrt::hstring(profileName)));
            std::wstring curEmoji = GetProfileEmoji(profileName);
            auto tbObj = FindVisual(existingRow, [](const mux::DependencyObject& d) { return d.try_as<muxc::TextBox>() != nullptr; });
            if (auto tb = tbObj.try_as<muxc::TextBox>()) {
                tb.Text(curEmoji);
            }
        }
        return;
    }

    auto emojiRow = BuildProfileEmojiPicker(profileName, colorLabel);
    emojiRow.Tag(winrt::box_value(winrt::hstring(profileName)));

    muxm::Brush sepBrush{nullptr};
    for (auto const& c : cardPanel.Children()) {
        if (auto b = c.try_as<muxc::Border>(); b && b.Height() == 1.0) {
            sepBrush = b.Background();
            break;
        }
    }
    if (!sepBrush) {
        sepBrush = BrushRes(L"CardStrokeColorDefaultBrush", winrt::Windows::UI::Color{255, 60, 60, 60});
    }

    muxc::Border sepLine;
    sepLine.Name(L"diatweaksProfileEmojiSep");
    sepLine.Height(1);
    sepLine.Background(sepBrush);

    if (auto grid = cardPanel.try_as<muxc::Grid>()) {
        if (grid.RowDefinitions().Size() == 0) {
            // Assign sequential row definitions to any existing children so they do not overlap at (0,0)
            for (uint32_t i = 0; i < grid.Children().Size(); i++) {
                muxc::RowDefinition rd;
                rd.Height(mux::GridLength{1, mux::GridUnitType::Auto});
                grid.RowDefinitions().Append(rd);
                if (auto fe = grid.Children().GetAt(i).try_as<mux::FrameworkElement>()) {
                    muxc::Grid::SetRow(fe, (int)i);
                }
            }
        }

        int colorRowIdx = muxc::Grid::GetRow(colorRowChild);
        muxc::RowDefinition rdSep;
        rdSep.Height(mux::GridLength{1, mux::GridUnitType::Pixel});
        grid.RowDefinitions().InsertAt(colorRowIdx + 1, rdSep);

        muxc::RowDefinition rdEmoji;
        rdEmoji.Height(mux::GridLength{1, mux::GridUnitType::Auto});
        grid.RowDefinitions().InsertAt(colorRowIdx + 2, rdEmoji);

        for (auto const& c : grid.Children()) {
            if (auto fe = c.try_as<mux::FrameworkElement>()) {
                if (fe != sepLine && fe != emojiRow) {
                    int r = muxc::Grid::GetRow(fe);
                    if (r > colorRowIdx) muxc::Grid::SetRow(fe, r + 2);
                }
            }
        }
        muxc::Grid::SetRow(sepLine, colorRowIdx + 1);
        muxc::Grid::SetColumn(sepLine, 0);
        muxc::Grid::SetColumnSpan(sepLine, 99);

        muxc::Grid::SetRow(emojiRow, colorRowIdx + 2);
        muxc::Grid::SetColumn(emojiRow, 0);
        muxc::Grid::SetColumnSpan(emojiRow, 99);

        grid.Children().Append(sepLine);
        grid.Children().Append(emojiRow);
    } else {
        uint32_t colorIdx = 0;
        if (cardPanel.Children().IndexOf(colorRowChild, colorIdx)) {
            cardPanel.Children().InsertAt(colorIdx + 1, sepLine);
            cardPanel.Children().InsertAt(colorIdx + 2, emojiRow);
        } else {
            cardPanel.Children().Append(sepLine);
            cardPanel.Children().Append(emojiRow);
        }
    }
}

static mux::FrameworkElement BuildSection(const std::wstring& name, const std::vector<mux::FrameworkElement>& rows) {
    muxc::StackPanel sec;

    muxc::Border head;
    head.Height(38);
    head.Padding(mux::Thickness{12, 0, 12, 0});
    muxc::TextBlock ht;
    ht.Text(name);
    ht.FontSize(16);
    ht.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    ht.VerticalAlignment(mux::VerticalAlignment::Center);
    head.Child(ht);
    sec.Children().Append(head);

    muxc::Border card;
    card.MinHeight(40);
    card.Background(t_card.background);
    card.BorderBrush(t_card.border);
    card.BorderThickness(mux::Thickness{1, 1, 1, 1});
    card.CornerRadius(t_card.radius);

    muxc::StackPanel list;
    for (size_t i = 0; i < rows.size(); i++) {
        if (i > 0) {
            muxc::Border line;
            line.Height(1);
            line.Background(t_card.border);
            list.Children().Append(line);
        }
        list.Children().Append(rows[i]);
    }
    card.Child(list);
    sec.Children().Append(card);
    return sec;
}

static mux::FrameworkElement BuildPresetSection() {
    muxc::Grid row;
    row.Padding(mux::Thickness{12, 8, 12, 8});
    row.MinHeight(40);
    row.ColumnSpacing(12);

    muxc::ColumnDefinition c0, c1, c2;
    c0.Width(mux::GridLengthHelper::Auto());
    c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
    c2.Width(mux::GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnDefinitions().Append(c2);

    muxc::FontIcon icon;
    icon.Glyph(L"\uE771");
    icon.FontSize(16);
    icon.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::Grid::SetColumn(icon, 0);
    row.Children().Append(icon);

    muxc::StackPanel text;
    text.VerticalAlignment(mux::VerticalAlignment::Center);
    muxc::TextBlock title;
    title.Text(L"Preset");
    text.Children().Append(title);
    muxc::TextBlock d;
    d.Text(L"A ready-made set of tune-ups (Arc, Arc for Windows, or None).");
    d.FontSize(12);
    d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
    text.Children().Append(d);
    muxc::Grid::SetColumn(text, 1);
    row.Children().Append(text);

    muxc::ComboBox box;
    box.MinWidth(130);
    box.VerticalAlignment(mux::VerticalAlignment::Center);

    const std::pair<const wchar_t*, const wchar_t*> presets[] = {
        {L"custom", L"Custom"},
        {L"none", L"None (Default)"},
        {L"arc", L"Arc"},
        {L"arcWindows", L"Arc (Windows)"}
    };
    int sel = 0, i = 0;
    for (const auto& [val, label] : presets) {
        muxc::ComboBoxItem ci;
        ci.Content(winrt::box_value(label));
        ci.Tag(winrt::box_value(val));
        box.Items().Append(ci);
        if (g_cfg.preset == val) sel = i;
        i++;
    }
    box.SelectedIndex(sel);
    muxa::AutomationProperties::SetAutomationId(box, L"patch_preset");
    box.SelectionChanged([](winrt::Windows::Foundation::IInspectable const& s, auto&&) {
        if (t_syncing) return;
        auto ci = s.as<muxc::ComboBox>().SelectedItem().try_as<muxc::ComboBoxItem>();
        if (!ci) return;
        std::wstring v(winrt::unbox_value_or<winrt::hstring>(ci.Tag(), L""));
        ApplyPreset(v);
    });
    t_presetBox = winrt::make_weak(box);
    muxc::Grid::SetColumn(box, 2);
    row.Children().Append(box);

    return BuildSection(L"Preset", {row});
}

struct ChangelogVersion {
    std::wstring version;
    std::wstring date;
    std::wstring title;
    std::vector<std::wstring> changes;
};

static std::vector<ChangelogVersion> LoadChangelog() {
    std::vector<ChangelogVersion> result;

    auto GetDefaultChangelog = []() -> std::vector<ChangelogVersion> {
        ChangelogVersion v;
        v.version = L"1.1.5";
        v.date = L"2026-11-19";
        v.title = L"Dia Tune-ups v1.1.5";
        v.changes = {
            L"Patching at sign-in works again: the background watcher is now diatweaks-watch.exe and the loader is diatweaks-loader.exe. Security software on some PCs silently blocked the old file names, so the watcher never started and Dia stayed unpatched.",
            L"No console window: the watcher is a windowless helper, so nothing pops up and stays open after sign-in.",
            L"Developer-mode tab mark in the top tab layout: the mark now finds tab rows in both the sidebar and the top tab strip, follows the active tab by its centre, and spans the row so the dashed outline surrounds the whole tab."
        };
        return {v};
    };

    if (g_changelogPath[0] == 0) {
        wchar_t local[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) && local[0]) {
            swprintf_s(g_changelogPath, _countof(g_changelogPath), L"%s\\DiaTweaks\\changelog.json", local);
        }
    }

    std::vector<std::wstring> candidates;
    if (g_changelogPath[0]) candidates.push_back(g_changelogPath);

    wchar_t modPath[MAX_PATH]{};
    if (GetModuleFileNameW(g_module, modPath, MAX_PATH)) {
        if (wchar_t* s = wcsrchr(modPath, L'\\')) {
            *s = 0;
            candidates.push_back(std::wstring(modPath) + L"\\changelog.json");
            candidates.push_back(std::wstring(modPath) + L"\\..\\changelog.json");
        }
    }

    std::wstring jsonStr;
    for (const auto& path : candidates) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(file, nullptr);
            if (size > 0 && size != INVALID_FILE_SIZE && size < 1024 * 1024) {
                std::string buf(size, '\0');
                DWORD read = 0;
                if (ReadFile(file, buf.data(), size, &read, nullptr) && read > 0) {
                    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, nullptr, 0);
                    if (wlen > 0) {
                        jsonStr.resize(wlen);
                        MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)read, jsonStr.data(), wlen);
                    }
                }
            }
            CloseHandle(file);
            if (!jsonStr.empty()) break;
        }
    }

    if (!jsonStr.empty()) {
        try {
            winrt::Windows::Data::Json::JsonObject root;
            if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring(jsonStr), root)) {
                if (root.HasKey(L"versions")) {
                    auto arr = root.GetNamedArray(L"versions");
                    for (uint32_t i = 0; i < arr.Size(); i++) {
                        auto vObj = arr.GetObjectAt(i);
                        ChangelogVersion ver;
                        ver.version = std::wstring(vObj.GetNamedString(L"version", L""));
                        ver.date = std::wstring(vObj.GetNamedString(L"date", L""));
                        ver.title = std::wstring(vObj.GetNamedString(L"title", L""));
                        if (vObj.HasKey(L"changes")) {
                            auto cArr = vObj.GetNamedArray(L"changes");
                            for (uint32_t j = 0; j < cArr.Size(); j++) {
                                ver.changes.push_back(std::wstring(cArr.GetStringAt(j)));
                            }
                        }
                        result.push_back(ver);
                    }
                }
            }
        } catch (...) {}
    }

    if (result.empty()) {
        result = GetDefaultChangelog();
        if (g_changelogPath[0] && GetFileAttributesW(g_changelogPath) == INVALID_FILE_ATTRIBUTES) {
            const char defaultJson[] = "{\n"
                "  \"versions\": [\n"
                "    {\n"
                "      \"version\": \"1.1.5\",\n"
                "      \"date\": \"2026-11-19\",\n"
                "      \"title\": \"Dia Tune-ups v1.1.5\",\n"
                "      \"changes\": [\n"
                "        \"Patching at sign-in works again: the background watcher is now diatweaks-watch.exe and the loader is diatweaks-loader.exe. Security software on some PCs silently blocked the old file names, so the watcher never started and Dia stayed unpatched.\",\n"
                "        \"No console window: the watcher is a windowless helper, so nothing pops up and stays open after sign-in.\",\n"
                "        \"Developer-mode tab mark in the top tab layout: the mark now finds tab rows in both the sidebar and the top tab strip, follows the active tab by its centre, and spans the row so the dashed outline surrounds the whole tab.\"\n"
                "      ]\n"
                "    }\n"
                "  ]\n"
                "}\n";
            HANDLE hOut = CreateFileW(g_changelogPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hOut != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(hOut, defaultJson, (DWORD)strlen(defaultJson), &written, nullptr);
                CloseHandle(hOut);
            }
        }
    }

    return result;
}

struct RemoteVersionInfo {
    bool success{false};
    bool isNewer{false};
    std::wstring version;
    std::wstring date;
    std::wstring title;
};

// Does one HTTPS GET and returns the response body on HTTP 200.
static bool HttpGetBody(const wchar_t* host, const wchar_t* path, std::string& body) {
    HINTERNET hSession = WinHttpOpen(L"DiaTweaks-Updater/1.2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hConnect) {
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (hReq) {
            WinHttpSetTimeouts(hReq, 4000, 4000, 6000, 6000);
            const wchar_t* headers =
                L"User-Agent: DiaTweaks-Updater\r\nAccept: application/vnd.github+json\r\n";
            if (WinHttpSendRequest(hReq, headers, static_cast<DWORD>(wcslen(headers)),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr)) {
                DWORD status = 0, statusSize = sizeof(status);
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    nullptr, &status, &statusSize, nullptr);
                if (status == 200) {
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (WinHttpReadData(hReq, chunk.data(), avail, &read) && read > 0)
                            body.append(chunk.data(), read);
                        else
                            break;
                    }
                    ok = !body.empty();
                }
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

// The DLL lives in <repo>\bin; the repo root is the parent of that folder.
static std::wstring RepoRoot() {
    wchar_t modulePath[MAX_PATH]{};
    if (!GetModuleFileNameW(g_module, modulePath, MAX_PATH)) return {};
    std::wstring p = modulePath;
    size_t binSlash = p.find_last_of(L'\\');
    if (binSlash == std::wstring::npos) return {};
    std::wstring bin = p.substr(0, binSlash);
    size_t repoSlash = bin.find_last_of(L'\\');
    if (repoSlash == std::wstring::npos) return {};
    return bin.substr(0, repoSlash);
}

// Reads the checked-out commit from .git so we can compare it with GitHub. The
// updater installs by `git pull`, so every push is a new build - there is no
// need for a release tag or a version bump to detect it.
static std::wstring LocalHeadSha() {
    auto readTrimmed = [](const std::wstring& path) -> std::string {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        DWORD size = GetFileSize(f, nullptr);
        if (size == INVALID_FILE_SIZE || size > 65536) { CloseHandle(f); return {}; }
        std::string buf(size, '\0');
        DWORD read = 0;
        bool ok = ReadFile(f, buf.data(), size, &read, nullptr) && read > 0;
        CloseHandle(f);
        if (!ok) return {};
        std::string s = buf.substr(0, read);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        return s;
    };
    auto widen = [](const std::string& s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring out(n > 0 ? n - 1 : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
        return out;
    };

    std::wstring root = RepoRoot();
    if (root.empty()) return {};
    std::string head = readTrimmed(root + L"\\.git\\HEAD");
    if (head.empty()) return {};
    if (head.rfind("ref:", 0) == 0) {
        std::string ref = head.substr(4);
        while (!ref.empty() && ref.front() == ' ') ref.erase(ref.begin());
        std::string sha = readTrimmed(root + L"\\.git\\" + widen(ref));
        if (!sha.empty()) return widen(sha);
        // Loose ref missing (packed): look it up in packed-refs.
        std::string packed = readTrimmed(root + L"\\.git\\packed-refs");
        size_t pos = 0;
        while ((pos = packed.find('\n', pos)) != std::string::npos) {
            std::string line = packed.substr(0, pos);
            ++pos;
            if (line.empty() || line[0] == '#' || line[0] == '^') continue;
            size_t sp = line.find(' ');
            if (sp == std::string::npos) continue;
            if (line.substr(sp + 1) == ref) return widen(line.substr(0, sp));
        }
        return {};
    }
    return widen(head);
}

static RemoteVersionInfo CheckChangelogVersion() {
    RemoteVersionInfo info;
    std::string body;
    if (!HttpGetBody(L"raw.githubusercontent.com",
                     L"/jinanshm/DiaTweaks-Win/master/changelog.json", body))
        return info;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
    if (wlen <= 0) return info;
    std::wstring jsonStr(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), jsonStr.data(), wlen);
    try {
        winrt::Windows::Data::Json::JsonObject root;
        if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring(jsonStr), root) &&
            root.HasKey(L"versions")) {
            auto arr = root.GetNamedArray(L"versions");
            if (arr.Size() > 0) {
                auto first = arr.GetObjectAt(0);
                info.version = std::wstring(first.GetNamedString(L"version", L""));
                info.date = std::wstring(first.GetNamedString(L"date", L""));
                info.title = std::wstring(first.GetNamedString(L"title", L""));
                info.success = true;
                int curA = 1, curB = 1, curC = 2, remA = 0, remB = 0, remC = 0;
                swscanf_s(info.version.c_str(), L"%d.%d.%d", &remA, &remB, &remC);
                info.isNewer = remA > curA || (remA == curA && remB > curB) ||
                               (remA == curA && remB == curB && remC > curC);
            }
        }
    } catch (...) {}
    return info;
}

static RemoteVersionInfo CheckForUpdatesOnline() {
    // Prefer the git commit: GitHub does not publish a release for every build,
    // but every build is a push to master. Comparing SHAs "catches up" to all of
    // them, including ones where the hard-coded version was never bumped.
    std::string body;
    if (HttpGetBody(L"api.github.com", L"/repos/jinanshm/DiaTweaks-Win/commits/master", body)) {
        try {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), nullptr, 0);
            if (wlen > 0) {
                std::wstring jsonStr(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, body.data(), (int)body.size(), jsonStr.data(), wlen);
                winrt::Windows::Data::Json::JsonObject root;
                if (winrt::Windows::Data::Json::JsonObject::TryParse(winrt::hstring(jsonStr), root)) {
                    RemoteVersionInfo info;
                    std::wstring sha = std::wstring(root.GetNamedString(L"sha", L""));
                    if (!sha.empty()) {
                        info.version = sha.substr(0, 7);
                        info.success = true;
                        if (root.HasKey(L"commit")) {
                            auto commit = root.GetNamedObject(L"commit");
                            info.date = std::wstring(commit.GetNamedString(L"date", L""));
                            if (commit.HasKey(L"message")) {
                                std::wstring msg = std::wstring(commit.GetNamedString(L"message", L""));
                                size_t nl = msg.find(L'\n');
                                info.title = nl == std::wstring::npos ? msg : msg.substr(0, nl);
                            }
                            if (commit.HasKey(L"author")) {
                                auto author = commit.GetNamedObject(L"author");
                                if (info.date.empty())
                                    info.date = std::wstring(author.GetNamedString(L"date", L""));
                            }
                        }
                        std::wstring local = LocalHeadSha();
                        info.isNewer = !local.empty() && local != sha;
                        if (local.empty()) {
                            // No local git metadata (e.g. installed from a zip):
                            // fall back to the changelog version comparison.
                            RemoteVersionInfo fallback = CheckChangelogVersion();
                            if (fallback.success) {
                                info.version = fallback.version;
                                info.date = fallback.date;
                                info.title = fallback.title;
                                info.isNewer = fallback.isNewer;
                            }
                        }
                        return info;
                    }
                }
            }
        } catch (...) {}
    }
    // GitHub API unavailable (offline / rate limited): fall back to changelog.
    return CheckChangelogVersion();
}

static mux::FrameworkElement BuildDiaTuneupsSection() {
    std::vector<mux::FrameworkElement> rows;

    // 1. Update Dia Tune-ups
    {
        muxc::Grid row;
        row.Padding(mux::Thickness{12, 8, 12, 8});
        row.MinHeight(40);
        row.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1, c2;
        c0.Width(mux::GridLengthHelper::Auto());
        c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::FontIcon icon;
        icon.Glyph(L"\uE895"); // Sync / Download / Update icon
        icon.FontSize(16);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(icon, 0);
        row.Children().Append(icon);

        muxc::StackPanel text;
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock title;
        title.Text(L"Update Dia Tune-ups");
        text.Children().Append(title);
        muxc::TextBlock d;
        d.Text(L"Version 1.1.5 • One click checks GitHub and installs the newest build.");
        d.FontSize(12);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);

        muxc::Button btn;
        btn.Content(winrt::box_value(L"Update Tune-ups"));
        btn.VerticalAlignment(mux::VerticalAlignment::Center);
        btn.Click([btnWeak = winrt::make_weak(btn), descWeak = winrt::make_weak(d)](auto&&, auto&&) {
            auto b = btnWeak.get();
            auto desc = descWeak.get();
            if (!b || !desc) return;

            b.IsEnabled(false);
            b.Content(winrt::box_value(L"Checking..."));
            desc.Text(L"Checking GitHub for a new build...");
            auto queue = b.DispatcherQueue();

            // One click: check, report what was found, then pull + rebuild.
            // All child processes are launched hidden, so no CMD window pops up.
            std::thread([btnWeak, descWeak, queue]() {
                RemoteVersionInfo info = CheckForUpdatesOnline();
                if (queue) {
                    queue.TryEnqueue([btnWeak, descWeak, info]() {
                        auto bb = btnWeak.get();
                        auto dd = descWeak.get();
                        if (bb) bb.Content(winrt::box_value(L"Updating..."));
                        if (!dd) return;
                        if (info.success && info.isNewer) {
                            std::wstring msg = L"New build " + info.version + L" found";
                            if (!info.date.empty()) msg += L" (" + info.date + L")";
                            if (!info.title.empty()) msg += L" - " + info.title;
                            dd.Text(msg + L" - installing now...");
                        } else if (info.success) {
                            dd.Text(L"You're up to date - refreshing the current build anyway...");
                        } else {
                            dd.Text(L"Couldn't reach GitHub - trying a direct pull...");
                        }
                    });
                }

                // Launch PowerShell hidden via ShellExecuteEx (no cmd.exe /c
                // console window), wait for it, then report back on the UI thread.
                wchar_t params[1400]{};
                swprintf_s(params, _countof(params),
                    L"-NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -Command "
                    L"\"Set-Location -LiteralPath '%s'; git pull origin master; "
                    L"& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
                    L"-File '.\\scripts\\build.ps1' -SkipAgent\"", RepoRoot().c_str());
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = L"open";
                sei.lpFile = L"powershell.exe";
                sei.lpParameters = params;
                sei.nShow = SW_HIDE;
                bool launched = ShellExecuteExW(&sei) != FALSE && sei.hProcess;
                if (launched) {
                    WaitForSingleObject(sei.hProcess, 10 * 60 * 1000);
                    CloseHandle(sei.hProcess);
                }

                if (queue) {
                    queue.TryEnqueue([btnWeak, descWeak, launched]() {
                        auto bb = btnWeak.get();
                        auto dd = descWeak.get();
                        if (bb) {
                            bb.Content(winrt::box_value(launched ? L"Updated \u2713" : L"Try again"));
                            bb.IsEnabled(true);
                        }
                        if (dd) {
                            dd.Text(launched
                                ? L"Dia Tune-ups updated. Restart Dia to load the new build."
                                : L"Couldn't start the updater. Check that PowerShell is available.");
                        }
                    });
                }
            }).detach();
        });
        muxc::Grid::SetColumn(btn, 2);
        row.Children().Append(btn);
        rows.push_back(row);
    }

    // 2. Changelog card (with expandable in-panel changelog viewer)
    {
        muxc::StackPanel cardStack;

        muxc::Grid row;
        row.Padding(mux::Thickness{12, 8, 12, 8});
        row.MinHeight(40);
        row.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1, c2;
        c0.Width(mux::GridLengthHelper::Auto());
        c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::FontIcon icon;
        icon.Glyph(L"\uE81C"); // Document / Note / History icon
        icon.FontSize(16);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(icon, 0);
        row.Children().Append(icon);

        muxc::StackPanel text;
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock title;
        title.Text(L"Changelog");
        text.Children().Append(title);
        muxc::TextBlock d;
        d.Text(L"View what's new in version 1.1.5 and recent changes.");
        d.FontSize(12);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);

        muxc::Button btn;
        btn.Content(winrt::box_value(L"Show changelog"));
        btn.VerticalAlignment(mux::VerticalAlignment::Center);

        muxc::StackPanel changelogPanel;
        changelogPanel.Padding(mux::Thickness{40, 4, 16, 12});
        changelogPanel.Spacing(10);
        changelogPanel.Visibility(mux::Visibility::Collapsed);

        auto versions = LoadChangelog();
        for (const auto& ver : versions) {
            muxc::StackPanel vBlock;
            vBlock.Spacing(4);

            muxc::TextBlock vTitle;
            std::wstring headerText = ver.title.empty() ? (L"Version " + ver.version) : ver.title;
            if (!ver.date.empty()) {
                headerText += L"  (" + ver.date + L")";
            }
            vTitle.Text(headerText);
            vTitle.FontSize(13);
            vTitle.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
            vBlock.Children().Append(vTitle);

            for (const auto& change : ver.changes) {
                muxc::TextBlock cText;
                cText.Text(L"•  " + change);
                cText.FontSize(12);
                cText.TextWrapping(mux::TextWrapping::Wrap);
                cText.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
                cText.Margin(mux::Thickness{4, 1, 0, 1});
                vBlock.Children().Append(cText);
            }
            changelogPanel.Children().Append(vBlock);
        }

        btn.Click([btnWeak = winrt::make_weak(btn), panelWeak = winrt::make_weak(changelogPanel)](auto&&, auto&&) {
            auto b = btnWeak.get();
            auto p = panelWeak.get();
            if (!b || !p) return;
            if (p.Visibility() == mux::Visibility::Visible) {
                p.Visibility(mux::Visibility::Collapsed);
                b.Content(winrt::box_value(L"Show changelog"));
            } else {
                p.Visibility(mux::Visibility::Visible);
                b.Content(winrt::box_value(L"Hide changelog"));
            }
        });

        muxc::Grid::SetColumn(btn, 2);
        row.Children().Append(btn);

        cardStack.Children().Append(row);
        cardStack.Children().Append(changelogPanel);
        rows.push_back(cardStack);
    }

    // 3. Support the developer
    {
        muxc::Grid row;
        row.Padding(mux::Thickness{12, 8, 12, 8});
        row.MinHeight(40);
        row.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1, c2;
        c0.Width(mux::GridLengthHelper::Auto());
        c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::FontIcon icon;
        icon.Glyph(L"\uEB52"); // HeartFill icon
        icon.FontSize(16);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(icon, 0);
        row.Children().Append(icon);

        muxc::StackPanel text;
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock title;
        title.Text(L"Support the developer");
        text.Children().Append(title);
        muxc::TextBlock d;
        d.Text(L"Enjoying Dia Tune-ups? Buy me a coffee to support continued development!");
        d.FontSize(12);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);

        muxc::Button btn;
        btn.Content(winrt::box_value(L"Buy me a coffee"));
        btn.VerticalAlignment(mux::VerticalAlignment::Center);
        btn.Click([](auto&&, auto&&) {
            ShellExecuteW(nullptr, L"open", L"https://buymeacoffee.com/jinansh_", nullptr, nullptr, SW_SHOWNORMAL);
        });
        muxc::Grid::SetColumn(btn, 2);
        row.Children().Append(btn);
        rows.push_back(row);
    }

    return BuildSection(L"Dia Tune-ups", rows);
}

static mux::FrameworkElement BuildActionsSection() {
    std::vector<mux::FrameworkElement> rows;

    // 1. Turn off all tune-ups
    {
        muxc::Grid row;
        row.Padding(mux::Thickness{12, 8, 12, 8});
        row.MinHeight(40);
        row.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1, c2;
        c0.Width(mux::GridLengthHelper::Auto());
        c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::FontIcon icon;
        icon.Glyph(L"\uE8DA");
        icon.FontSize(16);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(icon, 0);
        row.Children().Append(icon);

        muxc::StackPanel text;
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock title;
        title.Text(L"Turn off all tune-ups");
        text.Children().Append(title);
        muxc::TextBlock d;
        d.Text(L"Reset Dia to its original appearance.");
        d.FontSize(12);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);

        muxc::Button btn;
        btn.Content(winrt::box_value(L"Turn all off"));
        btn.VerticalAlignment(mux::VerticalAlignment::Center);
        btn.Click([](auto&&, auto&&) {
            ApplyPreset(L"none");
        });
        muxc::Grid::SetColumn(btn, 2);
        row.Children().Append(btn);
        rows.push_back(row);
    }

    // 2. Open data folder
    {
        muxc::Grid row;
        row.Padding(mux::Thickness{12, 8, 12, 8});
        row.MinHeight(40);
        row.ColumnSpacing(12);
        muxc::ColumnDefinition c0, c1, c2;
        c0.Width(mux::GridLengthHelper::Auto());
        c1.Width(mux::GridLength{1, mux::GridUnitType::Star});
        c2.Width(mux::GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        row.ColumnDefinitions().Append(c2);

        muxc::FontIcon icon;
        icon.Glyph(L"\uED25");
        icon.FontSize(16);
        icon.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::Grid::SetColumn(icon, 0);
        row.Children().Append(icon);

        muxc::StackPanel text;
        text.VerticalAlignment(mux::VerticalAlignment::Center);
        muxc::TextBlock title;
        title.Text(L"Tune-ups Data Folder");
        text.Children().Append(title);
        muxc::TextBlock d;
        d.Text(L"Open the Dia Tune-ups data folder in File Explorer.");
        d.FontSize(12);
        d.Foreground(BrushRes(L"TextFillColorSecondaryBrush", winrt::Windows::UI::Color{255, 0xc5, 0xc5, 0xc5}));
        text.Children().Append(d);
        muxc::Grid::SetColumn(text, 1);
        row.Children().Append(text);

        muxc::Button btn;
        btn.Content(winrt::box_value(L"Open folder"));
        btn.VerticalAlignment(mux::VerticalAlignment::Center);
        btn.Click([](auto&&, auto&&) {
            ShellExecuteW(nullptr, L"open", g_dir, nullptr, nullptr, SW_SHOWNORMAL);
        });
        muxc::Grid::SetColumn(btn, 2);
        row.Children().Append(btn);
        rows.push_back(row);
    }

    return BuildSection(L"Actions", rows);
}

static mux::FrameworkElement BuildPage(const NavHook& hook) {
    t_syncing = true;
    t_toggles.clear();
    t_choices.clear();

    EnsureCardLook();

    muxc::Grid root;
    root.Padding(mux::Thickness{24, 44, 24, 24});
    muxc::RowDefinition r0, r1;
    r0.Height(mux::GridLengthHelper::Auto());
    r1.Height(mux::GridLength{1, mux::GridUnitType::Star});
    root.RowDefinitions().Append(r0);
    root.RowDefinitions().Append(r1);

    muxc::TextBlock title;
    title.Text(L"Tune-ups");
    title.FontSize(20);
    title.FontWeight(winrt::Windows::UI::Text::FontWeight{600});
    title.Margin(mux::Thickness{16, 0, 0, 24});
    root.Children().Append(title);

    muxc::ScrollViewer scroll;
    scroll.Margin(mux::Thickness{0, 0, -24, 0});
    scroll.Padding(mux::Thickness{0, 0, 24, 0});
    scroll.HorizontalScrollBarVisibility(muxc::ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(muxc::ScrollBarVisibility::Auto);
    muxc::Grid::SetRow(scroll, 1);

    muxc::StackPanel stack;
    stack.Spacing(16);
    stack.Padding(mux::Thickness{0, 0, 0, 24});

    // 0. Dia Tune-ups (Update, Changelog, Support Developer)
    stack.Children().Append(BuildDiaTuneupsSection());

    // 1. Presets
    stack.Children().Append(BuildPresetSection());

    // 2. Window
    std::vector<mux::FrameworkElement> winRows;
    winRows.push_back(BuildRow({L"edgeToEdge", L"Edge-to-edge window", L"Remove the window frame border so the page meets the window edges.", L"\uE7F4"}));
    winRows.push_back(BuildRow({L"squareSidebarCorners", L"Square window corners", L"Square the window corners instead of the rounded corners Windows 11 draws.", L"\uE7F4"}));
    winRows.push_back(BuildRow({L"windowMaterial", L"Window material", L"Apply Mica, Mica Alt, or Acrylic backdrop material behind the tabs and sidebar.", L"\uE790",
        {{L"default", L"Default"}, {L"mica", L"Mica"}, {L"acrylic", L"Acrylic"}, {L"micaalt", L"Mica Alt"}, {L"none", L"None"}}}));
    winRows.push_back(BuildMaterialOptionsRow());
    winRows.push_back(BuildRow({L"keepWindowsActive", L"Keep display awake", L"Stop the screen from sleeping while Dia is open.", L"\uEC46"}));
    winRows.push_back(BuildRow({L"normalLaunch", L"Patch Dia however it's opened", L"Keep tweaks active whenever Dia opens from any shortcut, pinned icon, or link.", L"\uE7E8"}));
    stack.Children().Append(BuildSection(L"Window", winRows));

    // 3. Toolbar
    std::vector<mux::FrameworkElement> tbRows;
    tbRows.push_back(BuildRow({L"hideToolbar", L"Hide toolbar", L"Hide the toolbar row: the address field, navigation buttons, and extensions.", L"\uE838"}));
    tbRows.push_back(BuildRow({L"hideToolbarMode", L"Hidden toolbar behavior", L"Over the page keeps the page in place and slides the toolbar over its top; pushes page down moves the page with it.", L"\uE838",
        {{L"overlay", L"Over the page"}, {L"push", L"Pushes page down"}}}));
    tbRows.push_back(BuildRow({L"toolbarColor", L"Toolbar color", L"Make the toolbar transparent so the sidebar color or window backdrop shines through.", L"\uE790"}));
    tbRows.push_back(BuildRow({L"revealToolbarOnHover", L"Reveal toolbar on hover", L"Reveals the toolbar when moving the pointer to the top edge of the window.", L"\uE838"}));
    tbRows.push_back(BuildRow({L"showToolbarWithSidebar", L"Show toolbar with sidebar", L"Keeps the toolbar showing when the sidebar is open.", L"\uE838"}));
    tbRows.push_back(BuildRow({L"showToolbarOnNewTab", L"Show toolbar on new tab", L"Keeps the toolbar showing when viewing a new tab or start page.", L"\uE838"}));
    tbRows.push_back(BuildRow({L"showAddressField", L"Always show address field", L"Keep the address field bookmark and site buttons showing without hovering.", L"\uE774"}));
    stack.Children().Append(BuildSection(L"Toolbar", tbRows));

    // 4. Toolbar Items
    std::vector<mux::FrameworkElement> itemRows;
    itemRows.push_back(BuildRow({L"hideSidebarButton", L"Hide sidebar button", L"Hide the sidebar toggle at the top-left of the toolbar.", L"\uE700"}));
    itemRows.push_back(BuildRow({L"hideChatButton", L"Hide Chat button", L"Hide the Chat button at the top-right of the toolbar.", L"\uE8BD"}));
    itemRows.push_back(BuildRow({L"hidePersonalizeButton", L"Hide Personalize button", L"Hide the Personalize button when Dia shows it in the toolbar.", L"\uE77F"}));
    itemRows.push_back(BuildRow({L"hideTabSearchButton", L"Hide tab search button", L"Hide the overflow / search tabs button in the window drag header.", L"\uE721"}));
    itemRows.push_back(BuildRow({L"hideTrafficLights", L"Hide window controls", L"Hide the minimize, maximize, and close buttons in the title bar.", L"\uE711"}));
    itemRows.push_back(BuildRow({L"hideLoadingAnimation", L"Hide loading animation", L"Hide the loading indicator line in the address bar.", L"\uE895"}));
    itemRows.push_back(BuildRow({L"pinnedExtensions", L"Pinned extensions shown", L"Show up to 6, 8, 10, 12 or 16 pinned extensions by the address bar instead of 4.", L"\uE8B7",
        {{L"0", L"Default (4)"}, {L"6", L"6"}, {L"8", L"8"}, {L"10", L"10"}, {L"12", L"12"}, {L"16", L"16"}}}));
    itemRows.push_back(BuildRow({L"developerMode", L"Developer Mode Toolbar", L"Adds a Developer Mode toggle to the site and tab menus, with the Arc-style developer toolbar.", L"\uE756"}));
    itemRows.push_back(BuildRow({L"developerModeLocalhost", L"Developer mode for localhost", L"Treat localhost and 127.0.0.1 as being in developer mode automatically.", L"\uE774"}));
    stack.Children().Append(BuildSection(L"Toolbar Items", itemRows));

    // 5. Sidebar & Tab Bar
    std::vector<mux::FrameworkElement> sbRows;
    sbRows.push_back(BuildRow({L"translucentSidebar", L"Translucent sidebar", L"Make the floating sidebar translucent.", L"\uE760"}));
    sbRows.push_back(BuildRow({L"sidebarGrain", L"Sidebar grain", L"Add procedural tactile texture behind the sidebar and tabs.", L"\uE760"}));
    sbRows.push_back(BuildRow({L"sidebarGrainMaterial", L"Grain texture material", L"Choose between sand, grain, denim, or paper tactile patterns.", L"\uE790",
        {{L"sand", L"Sand"}, {L"grain", L"Fine grain"}, {L"denim", L"Denim weave"}, {L"paper", L"Paper fiber"}}}));
    sbRows.push_back(BuildGrainOptionsRow());
    sbRows.push_back(BuildRow({L"hidePinnedTabBorder", L"Hide pinned tab border", L"Remove the border outline around pinned tabs.", L"\uE7C3"}));
    sbRows.push_back(BuildRow({L"arcPinnedTabs", L"Arc pinned tabs", L"On hover, show a minus to return a pinned tab to its page, or a close button when it is already there.", L"\uE718"}));
    sbRows.push_back(BuildRow({L"hideTabHoverCard", L"Hide tab hover card", L"Stops the preview card from popping up when hovering over open or pinned tabs.", L"\uE7C3"}));
    sbRows.push_back(BuildRow({L"profileEmoji", L"Profile emoji", L"Show an emoji in place of each space's dot in the space switcher. Choose them in Settings › Profiles.", L"\uE77F"}));
    stack.Children().Append(BuildSection(L"Sidebar & Tab Bar", sbRows));

    // 6. Page & Search
    std::vector<mux::FrameworkElement> pageRows;
    pageRows.push_back(BuildRow({L"peek", L"Peek link preview", L"Hold on a link or click with modifier to preview it in a card without leaving the tab.", L"\uE7A7"}));
    pageRows.push_back(BuildRow({L"peekTrigger", L"Peek trigger", L"How a Peek opens: Shift+click, hold on a link, Ctrl+Shift+click, or Alt+click.", L"\uE8B0",
        {{L"shift", L"Shift+click"}, {L"hold", L"Hold on a link"}, {L"ctrlShift", L"Ctrl+Shift+click"}, {L"alt", L"Alt+click"}}}));
    pageRows.push_back(BuildRow({L"peekAnimation", L"Peek animation", L"How the card flies in and out: Genie, Ease In/Out, or Zoom.", L"\uE7F4",
        {{L"genie", L"Genie"}, {L"easeInOut", L"Ease In/Out"}, {L"zoom", L"Zoom"}}}));
    pageRows.push_back(BuildRow({L"hideLinkAddress", L"Hide link address", L"Hide the hover link URL preview bubble at the bottom corner of the window.", L"\uE71B"}));
    pageRows.push_back(BuildRow({L"hideHistorySuggestions", L"Hide history suggestions", L"Keep visited pages and past searches out of address and command bar suggestions. Searching, Chat and open tabs still show.", L"\uE81C"}));
    pageRows.push_back(BuildRow({L"cleanSearchBox", L"Clean search box", L"Streamline the New Tab and Command Bar into a clean single-line input.", L"\uE721"}));
    pageRows.push_back(BuildRow({L"newTabPainting", L"Daily painting wallpaper", L"Display the Daily Brief's painting of the day as the New Tab page wallpaper.", L"\uEB9F"}));
    pageRows.push_back(BuildPaintingTintOptionsRow());
    pageRows.push_back(BuildRow({L"dontPopUpDownloads", L"Don't pop up downloads", L"Stops the downloads list from opening by itself for each download.", L"\uE896"}));
    pageRows.push_back(BuildRow({L"hideUpgradeBanner", L"Hide upgrade banner", L"Hide the Chat usage and upgrade card under the command bar.", L"\uE8BD"}));
    pageRows.push_back(BuildRow({L"segoeUiVariable", L"Force Segoe UI Variable font", L"Forces browser default sans-serif font to Segoe UI Variable (VF) instead of system-ui.", L"\uE8D2"}));
    pageRows.push_back(BuildRow({L"notoEmoji", L"Noto emoji font", L"Draw emoji in Dia's interface with Noto instead of Segoe UI Emoji. Install Noto Color Emoji (or Noto Emoji) first; only elements that show an emoji are changed.", L"\uE8D2"}));
    pageRows.push_back(BuildRow({L"notoEmojiFont", L"Emoji font family", L"Which Noto family to stack in front of Dia's fonts: Noto Color Emoji for colour, Noto Emoji for monochrome outlines.", L"\uE790",
        {{L"notoColor", L"Noto Color Emoji"}, {L"notoMono", L"Noto Emoji"}}}));
    stack.Children().Append(BuildSection(L"Page & Search", pageRows));

    // 7. Actions
    stack.Children().Append(BuildActionsSection());

    scroll.Content(stack);
    root.Children().Append(scroll);
    SyncToggles();
    return root;
}

static void HookNav(muxc::NavigationView nav, InstanceHandle advHandle) {
    if (!nav) return;

    for (auto& h : t_navs) {
        if (auto n = h.nav.get()) {
            if (n == nav) return;
        }
    }

    try {
        for (auto const& it : nav.MenuItems()) {
            if (auto fe = it.try_as<mux::FrameworkElement>()) {
                if (fe.Name() == L"diatweaksPatches") return;
            }
        }
    } catch (...) {}

    muxc::NavigationViewItem item;
    item.Name(L"diatweaksPatches");
    item.Content(winrt::box_value(L"Tune-ups"));

    muxc::IconElement iconElem{nullptr};
    try {
        const wchar_t* pathXaml =
            L"<PathIcon xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
            L"Data=\"F1 M 7.98,13.10 L 7.28,13.00 L 6.85,13.20 L 2.18,17.03 L 1.42,17.92 L 1.00,19.20 L 1.10,20.52 L 1.75,21.78 L 2.68,22.55 L 3.65,22.92 L 4.75,22.98 L 5.58,22.78 L 6.32,22.38 L 6.95,21.80 L 10.88,16.98 L 10.98,16.28 L 10.78,15.85 L 10.30,15.52 L 9.60,15.55 L 9.20,15.85 L 5.18,20.72 L 4.60,20.98 L 4.12,20.98 L 3.62,20.78 L 3.25,20.42 L 3.00,19.85 L 3.00,19.38 L 3.25,18.80 L 8.38,14.48 L 8.48,14.20 L 8.45,13.68 L 8.28,13.35 Z M 4.35,1.20 L 4.03,1.68 L 4.05,2.38 L 6.65,5.90 L 5.88,6.65 L 2.38,4.05 L 1.68,4.03 L 1.20,4.35 L 1.00,4.78 L 1.05,8.52 L 1.38,10.10 L 1.92,11.08 L 2.75,11.80 L 3.98,12.28 L 8.50,12.48 L 16.55,21.78 L 17.58,22.58 L 18.70,22.95 L 19.73,22.98 L 21.60,22.20 L 22.55,21.08 L 22.95,19.90 L 22.95,18.70 L 22.48,17.40 L 21.78,16.55 L 14.85,10.55 L 18.18,7.25 L 19.40,7.38 L 20.60,6.80 L 22.88,4.48 L 22.98,3.78 L 22.78,3.35 L 20.62,1.20 L 20.20,1.00 L 19.50,1.10 L 16.90,3.75 L 16.58,4.75 L 16.73,5.82 L 13.30,9.22 L 12.48,8.50 L 12.30,4.08 L 11.90,2.92 L 11.23,2.05 L 10.10,1.38 L 8.75,1.08 L 4.78,1.00 Z M 20.00,3.42 L 20.55,4.00 L 19.23,5.32 L 19.15,5.38 L 19.08,5.40 L 18.90,5.40 L 18.82,5.38 L 18.75,5.32 L 18.65,5.22 L 18.60,5.15 L 18.58,5.08 L 18.58,4.90 L 18.60,4.82 L 18.65,4.75 Z M 7.02,3.03 L 9.05,3.15 L 9.98,3.65 L 10.30,4.32 L 10.42,5.08 L 10.60,9.48 L 20.60,18.23 L 20.92,18.82 L 20.98,19.58 L 20.75,20.18 L 20.35,20.62 L 19.78,20.92 L 19.03,20.98 L 18.23,20.60 L 9.48,10.60 L 4.85,10.40 L 4.03,10.20 L 3.50,9.83 L 3.12,8.93 L 3.03,7.02 L 5.15,8.60 L 6.12,8.83 L 7.00,8.40 L 8.65,6.65 L 8.83,6.12 L 8.72,5.40 Z\" />";
        iconElem = muxk::XamlReader::Load(pathXaml).as<muxc::PathIcon>();
    } catch (...) {}

    if (!iconElem) {
        muxc::FontIcon icon;
        icon.Glyph(L"\uEC7A"); // Segoe Fluent Icons: Wrench & Screwdriver (matches garage.svg)
        iconElem = icon;
    }
    item.Icon(iconElem);
    item.Tag(winrt::box_value(L"patches"));

    try {
        auto items = nav.MenuItems();
        uint32_t at = items.Size();
        for (uint32_t i = 0; i < items.Size(); i++) {
            if (auto fe = items.GetAt(i).try_as<mux::FrameworkElement>()) {
                if (fe.Name() == L"advanced") {
                    at = i + 1;
                    break;
                }
            }
        }
        items.InsertAt(at, item);
    } catch (winrt::hresult_error const& e) {
        Log(L"Patches: can't add to Settings menu (0x%08lx)", (unsigned long)e.code());
        return;
    }

    NavHook hook;
    hook.nav = winrt::make_weak(nav);
    hook.item = item;
    hook.handle = advHandle;
    t_navs.push_back(hook);

    if (auto c = nav.Content().try_as<mux::DependencyObject>()) {
        CaptureCardLook(c);
    }

    nav.SelectionChanged([navWeak = winrt::make_weak(nav), itemWeak = winrt::make_weak(item)](
        muxc::NavigationView const& sender,
        muxc::NavigationViewSelectionChangedEventArgs const& a) {
        auto nav = navWeak.get();
        auto item = itemWeak.get();
        if (!nav || !item) return;

        bool isOurs = false;
        try {
            if (auto c = a.SelectedItemContainer()) {
                if (c == item) isOurs = true;
            }
        } catch (...) {}
        try {
            if (a.SelectedItem() == item) isOurs = true;
        } catch (...) {}

        for (auto& h : t_navs) {
            auto hNav = h.nav.get();
            if (hNav != nav) continue;

            if (isOurs) {
                auto cur = sender.Content();
                if (!h.page || cur != h.page) h.saved = cur;
                if (auto c = cur.try_as<mux::DependencyObject>()) CaptureCardLook(c);
                h.page = BuildPage(h);
                sender.Content(h.page);
            } else if (h.page && sender.Content() == h.page) {
                if (h.saved) sender.Content(h.saved);
            }
        }
        CheckProfileSettingsPage(sender);
    });

    Log(L"Patches: added to Settings menu");
}

static void ScanForSettings() {
    std::vector<InstanceHandle> advList;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);
        for (const auto& [handle, node] : g_nodes) {
            if (node.name == L"advanced") {
                advList.push_back(handle);
            }
        }
    }

    for (InstanceHandle adv : advList) {
        Inspectable advObj{nullptr};
        if (GetObject(adv, advObj)) {
            if (auto advFe = advObj.try_as<mux::FrameworkElement>()) {
                for (auto p = muxm::VisualTreeHelper::GetParent(advFe); p; p = muxm::VisualTreeHelper::GetParent(p)) {
                    if (auto nav = p.try_as<muxc::NavigationView>()) {
                        HookNav(nav, adv);
                        break;
                    }
                }
            }
        }

        InstanceHandle p = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            auto it = g_nodes.find(adv);
            if (it != g_nodes.end()) p = it->second.parent;
        }
        for (int i = 0; p && i < 40; i++) {
            std::wstring type;
            InstanceHandle nextP = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mx);
                auto it = g_nodes.find(p);
                if (it != g_nodes.end()) {
                    type = it->second.type;
                    nextP = it->second.parent;
                }
            }
            if (type == L"Microsoft.UI.Xaml.Controls.NavigationView") {
                Inspectable navObj{nullptr};
                if (GetObject(p, navObj)) {
                    if (auto nav = navObj.try_as<muxc::NavigationView>()) {
                        HookNav(nav, adv);
                    }
                }
                break;
            }
            p = nextP;
        }
    }

    CheckActiveSettingsPage();
}

void CheckActiveSettingsPage() {
    t_navs.erase(std::remove_if(t_navs.begin(), t_navs.end(), [](const NavHook& h) { return !h.nav.get(); }),
                 t_navs.end());
    for (auto& h : t_navs) {
        if (auto n = h.nav.get()) {
            CheckProfileSettingsPage(n);
        }
    }
}

} // namespace SettingsUi

// ---------------------------------------------------------------- tree dump

static bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    std::string utf8;
    utf8.push_back('\xEF');
    utf8.push_back('\xBB');
    utf8.push_back('\xBF');
    if (!text.empty()) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            size_t offset = utf8.size();
            utf8.resize(offset + static_cast<size_t>(bytes));
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                utf8.data() + offset, bytes, nullptr, nullptr);
        }
    }

    std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Log(L"dump: cannot create %s (error %lu)", temporary.c_str(), GetLastError());
        return false;
    }
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
    MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    return true;
}

// The CLI writes xaml.dump.request; the UI-thread timer answers with a TSV
// list of every named element it has seen. GetUiLayer is not usable here (it
// returns an empty root Grid on Dia's build), so the advice tree is the source.
static void WriteTreeDump() {
    std::wstring text;
    size_t rowsWritten = 0;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);

        struct Row {
            InstanceHandle handle = 0;
            DWORD thread = 0;
            std::wstring type;
            std::wstring name;
            std::wstring parent;
            std::wstring visibility;
            std::wstring opacity;
            std::wstring width;
            std::wstring height;
        };
        std::vector<Row> rows;
        rows.reserve(g_nodes.size());
        for (const auto& [handle, node] : g_nodes) {
            if (node.name.empty()) continue;
            Row row;
            row.handle = handle;
            row.thread = node.thread;
            row.type = node.type;
            row.name = node.name;
            auto parent = g_nodes.find(node.parent);
            if (parent != g_nodes.end()) row.parent = parent->second.name;
            rows.push_back(std::move(row));
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.name != b.name) return a.name < b.name;
            return a.handle < b.handle;
        });

        // Read the effective values outside the tree lock; these calls go back
        // into WinUI and must not run while the node table is held.
        for (Row& row : rows) {
            row.visibility = PropText(row.handle, L"Visibility");
            row.opacity = PropText(row.handle, L"Opacity");
            row.width = PropText(row.handle, L"ActualWidth");
            row.height = PropText(row.handle, L"ActualHeight");
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t header[256]{};
        swprintf_s(header, L"# DiaTweaks XAML tree %04u-%02u-%02u %02u:%02u:%02u pid=%lu nodes=%zu\r\n",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   GetCurrentProcessId(), rows.size());
        text = header;
        text += L"# handle\tthread\ttype\tname\tparent\tvis\topacity\twidth\theight\r\n";
        for (const Row& row : rows) {
            wchar_t line[1536]{};
            swprintf_s(line, L"%llu\t%lu\t%s\t%s\t%s\t%s\t%s\t%s\t%s\r\n",
                       static_cast<unsigned long long>(row.handle), row.thread,
                       row.type.c_str(), row.name.c_str(), row.parent.c_str(),
                       row.visibility.c_str(), row.opacity.c_str(),
                       row.width.c_str(), row.height.c_str());
            text += line;
        }
        rowsWritten = rows.size();
    }

    if (!WriteTextFileUtf8(g_treePath, text)) {
        DeleteFileW(g_dumpRequestPath);
        return;
    }

    DeleteFileW(g_dumpRequestPath);
    Log(L"dump: wrote %zu named node(s) to %s", rowsWritten, g_treePath);
}

// Probe for the New Tab page's own XAML island. Normal dumps only list named
// elements; the New Tab background is unnamed, so this walks up from the
// "Ask anything" RichEditBox and from the Dia logo image and prints the
// ancestor chain plus direct children. Triggered by xaml.ntp.request.
static void WriteNtpProbe() {
    std::wstring text;
    {
        std::lock_guard<std::recursive_mutex> lock(g_mx);

        auto appendNode = [&](const std::wstring& prefix, InstanceHandle h) {
            auto it = g_nodes.find(h);
            if (it == g_nodes.end()) {
                text += prefix + L"<gone h=" + std::to_wstring(h) + L">\r\n";
                return;
            }
            const Node& n = it->second;
            text += prefix + n.type + L" name='" + n.name + L"' h=" + std::to_wstring(h);
            Inspectable obj{nullptr};
            if (GetObject(h, obj)) {
                if (auto fe = obj.try_as<mux::FrameworkElement>()) {
                    text += L" size=" + std::to_wstring((int)std::lround(fe.ActualWidth())) + L"x" +
                            std::to_wstring((int)std::lround(fe.ActualHeight()));
                    try {
                        auto at = fe.TransformToVisual(nullptr).TransformPoint({0, 0});
                        text += L" at=" + std::to_wstring((int)std::lround(at.X)) + L"," +
                                std::to_wstring((int)std::lround(at.Y));
                    } catch (...) {}
                    auto m = fe.Margin();
                    wchar_t mg[96]{};
                    swprintf_s(mg, L" margin=%.0f,%.0f,%.0f,%.0f", m.Left, m.Top, m.Right, m.Bottom);
                    text += mg;
                    if (auto b = fe.try_as<muxc::Border>()) {
                        auto bt = b.BorderThickness();
                        wchar_t bb[96]{};
                        swprintf_s(bb, L" border=%.0f,%.0f,%.0f,%.0f brush=%d", bt.Left, bt.Top, bt.Right,
                                   bt.Bottom, b.BorderBrush() ? 1 : 0);
                        text += bb;
                    } else if (auto g = fe.try_as<muxc::Grid>()) {
                        auto bt = g.BorderThickness();
                        wchar_t bb[96]{};
                        swprintf_s(bb, L" border=%.0f,%.0f,%.0f,%.0f brush=%d", bt.Left, bt.Top, bt.Right,
                                   bt.Bottom, g.BorderBrush() ? 1 : 0);
                        text += bb;
                    }
                    text += L" vis=" + std::to_wstring((int)fe.Visibility());
                    wchar_t op[32]{};
                    swprintf_s(op, L"%.2f", fe.Opacity());
                    text += L" op=";
                    text += op;
                    try {
                        std::wstring autoName(muxa::AutomationProperties::GetName(fe));
                        if (!autoName.empty()) text += L" autoName='" + autoName + L"'";
                    } catch (...) {}
                }
            }
            text += L" kids=" + std::to_wstring(n.children.size()) + L"\r\n";
        };

        text += L"# total nodes=" + std::to_wstring(g_nodes.size()) + L"\r\n";
        text += L"# roots=" + std::to_wstring(g_roots.size()) + L"\r\n";
        for (auto root : g_roots) appendNode(L"root ", root);

        std::vector<InstanceHandle> boxes;
        std::vector<InstanceHandle> logos;
        for (const auto& [h, n] : g_nodes) {
            if (n.type == L"Microsoft.UI.Xaml.Controls.RichEditBox") boxes.push_back(h);
            if (n.type == L"Microsoft.UI.Xaml.Controls.Image") logos.push_back(h);
        }
        text += L"# RichEditBox=" + std::to_wstring(boxes.size()) + L" Image=" +
                std::to_wstring(logos.size()) + L"\r\n";

        auto dumpCandidate = [&](InstanceHandle h, const wchar_t* label) {
            text += L"\r\n--- ";
            text += label;
            text += L"\r\n";
            appendNode(L"", h);
            if (auto it = g_nodes.find(h); it != g_nodes.end() &&
                it->second.type == L"Microsoft.UI.Xaml.Controls.RichEditBox") {
                Inspectable obj{nullptr};
                if (GetObject(h, obj)) {
                    if (auto reb = obj.try_as<muxc::RichEditBox>()) {
                        text += L"    placeholder='" + std::wstring(reb.PlaceholderText()) + L"'\r\n";
                    }
                }
            }
            std::vector<InstanceHandle> chain;
            for (auto p = GetParentHandle(h); p && chain.size() < 60; p = GetParentHandle(p))
                chain.push_back(p);
            int depth = static_cast<int>(chain.size());
            for (auto p : chain) {
                std::wstring indent(static_cast<size_t>(depth--) * 2, L' ');
                appendNode(indent + L"up ", p);
                auto it = g_nodes.find(p);
                if (it == g_nodes.end()) continue;
                std::wstring childIndent(static_cast<size_t>(depth + 1) * 2 + 2, L' ');
                for (auto c : it->second.children) appendNode(childIndent + L"kid ", c);
            }
        };

        for (auto h : boxes) dumpCandidate(h, L"RichEditBox");
        for (auto h : logos) {
            auto it = g_nodes.find(h);
            if (it == g_nodes.end()) continue;
            Inspectable obj{nullptr};
            if (!GetObject(h, obj)) continue;
            auto fe = obj.try_as<mux::FrameworkElement>();
            if (!fe) continue;
            if (fe.ActualWidth() < 100 || fe.ActualWidth() > 200) continue;
            if (fe.ActualHeight() < 100 || fe.ActualHeight() > 200) continue;
            dumpCandidate(h, L"Image");
        }
    }

    if (WriteTextFileUtf8(g_ntpProbePath, text)) Log(L"ntp probe: wrote %s", g_ntpProbePath);
    DeleteFileW(g_ntpProbeRequestPath);
}

// ---------------------------------------------------------------- diagnostics callback

class Tap : public IObjectWithSite, public IVisualTreeServiceCallback2 {
    std::atomic<ULONG> m_ref{1};
    IUnknown* m_site = nullptr;

public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else if (riid == __uuidof(IVisualTreeServiceCallback) ||
                   riid == __uuidof(IVisualTreeServiceCallback2)) {
            *ppv = static_cast<IVisualTreeServiceCallback2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = --m_ref;
        if (!refs) delete this;
        return refs;
    }

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override {
        if (m_site) m_site->Release();
        m_site = site;
        if (site) site->AddRef();
        if (!site) return S_OK;

        HRESULT hr = site->QueryInterface(__uuidof(IXamlDiagnostics), (void**)&g_diag);
        if (FAILED(hr)) {
            Log(L"SetSite: no IXamlDiagnostics 0x%08X", static_cast<unsigned>(hr));
            return hr;
        }
        hr = site->QueryInterface(__uuidof(IVisualTreeService2), (void**)&g_vts);
        if (FAILED(hr)) {
            Log(L"SetSite: no IVisualTreeService2 0x%08X", static_cast<unsigned>(hr));
            return hr;
        }

        ReadConfig();
        Log(L"attached to Dia pid=%lu (xaml diagnostics)", GetCurrentProcessId());

        // Advise must not run on the thread that called SetSite.
        AddRef();
        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID parameter) -> DWORD {
                auto self = static_cast<Tap*>(parameter);
                HRESULT advise = g_vts->AdviseVisualTreeChange(self);
                Log(L"AdviseVisualTreeChange 0x%08X", static_cast<unsigned>(advise));
                self->Release();
                return 0;
            },
            this, 0, nullptr);
        if (thread) CloseHandle(thread);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppv) override {
        if (!m_site) {
            *ppv = nullptr;
            return E_FAIL;
        }
        return m_site->QueryInterface(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                 VisualMutationType mutation) override {
        std::wstring type = Bstr(element.Type);
        std::wstring name = Bstr(element.Name);
        SysFreeString(element.Type);
        SysFreeString(element.Name);
        SysFreeString(element.SrcInfo.FileName);
        SysFreeString(element.SrcInfo.Hash);

        {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            if (mutation == Add) {
                if (type == L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter") {
                    g_menuPresenterCount.fetch_add(1);
                }
                Node& node = g_nodes[element.Handle];
                node.parent = relation.Parent;
                node.childIndex = relation.ChildIndex;
                node.type = type;
                node.name = name;
                node.thread = GetCurrentThreadId();
                if (relation.Parent) {
                    auto& children = g_nodes[relation.Parent].children;
                    if (std::find(children.begin(), children.end(), element.Handle) == children.end())
                        children.push_back(element.Handle);
                } else if (std::find(g_roots.begin(), g_roots.end(), element.Handle) == g_roots.end()) {
                    g_roots.push_back(element.Handle);
                }
            } else {
                auto it = g_nodes.find(element.Handle);
                if (it != g_nodes.end()) {
                    if (it->second.type == L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter") {
                        g_menuPresenterCount.fetch_sub(1);
                    }
                    auto parent = g_nodes.find(it->second.parent);
                    if (parent != g_nodes.end()) {
                        auto& children = parent->second.children;
                        children.erase(std::remove(children.begin(), children.end(), element.Handle),
                                       children.end());
                    }
                    g_nodes.erase(it);
                }
                g_applied.erase(element.Handle);
            }
        }

        if (mutation == Add) {
            // Only record the node here. Applying inside this callback can
            // re-enter the tree service; the timer does the work next tick.
            g_treeDirty.store(true);
            EnsureTimerOnThisThread();
        } else if (mutation == Remove) {
            g_treeDirty.store(true);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) override {
        return S_OK;
    }

    static void EnsureTimerOnThisThread() {
        const DWORD thread = GetCurrentThreadId();
        {
            std::lock_guard<std::mutex> lock(g_stateLock);
            if (std::find(g_uiThreads.begin(), g_uiThreads.end(), thread) != g_uiThreads.end())
                return;
            g_uiThreads.push_back(thread);
        }
        if (!g_pingWnd.load()) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = PingProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"DiaTweaksHoverPing";
            RegisterClassExW(&wc);
            HWND ping = CreateWindowExW(0, L"DiaTweaksHoverPing", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            g_pingWnd.store(ping);
            g_hoverThread.store(thread);
            g_watchHover.store(true);
            HANDLE hw = CreateThread(nullptr, 0, HoverWatcher, nullptr, 0, nullptr);
            if (hw) CloseHandle(hw);
        }
        SetTimer(nullptr, 0, 250, TimerProc);
        Log(L"timer on ui thread %lu", thread);
    }

private:
    static std::atomic<int> g_lastStamp;

    static void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD) {
        ReadConfig();
        const int stamp = CfgStamp();
        const bool configChanged = stamp != g_lastStamp.exchange(stamp);
        if (configChanged) Log(L"config changed -> 0x%X", stamp);

        // The CLI can ask for a snapshot of every named element; the answer is
        // written from this UI thread so the node table is not raced.
        if (GetFileAttributesW(g_dumpRequestPath) != INVALID_FILE_ATTRIBUTES) WriteTreeDump();
        if (GetFileAttributesW(g_ntpProbeRequestPath) != INVALID_FILE_ATTRIBUTES) WriteNtpProbe();

        bool treeDirty = g_treeDirty.exchange(false);
        bool currentIsNewTab = IsNewTab();
        static bool s_lastIsNewTab = false;
        bool tabChanged = (currentIsNewTab != s_lastIsNewTab);
        s_lastIsNewTab = currentIsNewTab;

        // Window-frame tweaks belong to the window, so they must be applied to
        // every window Dia opens, not only when the config changed. Keeping
        // this on a slow tick means a window opened long after startup is
        // patched within a second even if the CLI watcher is not running. Stay
        // out of DWM entirely while every frame tweak is off.
        static ULONGLONG s_lastDwmSync = 0;
        static bool s_dwmTweaksWereApplied = false;
        ULONGLONG nowTicks = GetTickCount64();
        if (configChanged || nowTicks - s_lastDwmSync >= 1000) {
            s_lastDwmSync = nowTicks;
            bool dwmTweaksOn = g_edgeToEdge.load() || g_squareSidebarCorners.load() ||
                               (g_cfg.windowMaterial != L"default" &&
                                g_cfg.windowMaterial != L"none");
            if (dwmTweaksOn || s_dwmTweaksWereApplied) {
                ApplyDwmToProcessWindows();
                s_dwmTweaksWereApplied = dwmTweaksOn;
            }
        }

        if (configChanged || treeDirty || tabChanged) {
            ApplyAllOnThisThread();
            SettingsUi::ScanForSettings();
            SettingsUi::SyncToggles();

            if (!g_hideTabHoverCard.load()) {
                std::lock_guard<std::mutex> lock(g_stateLock);
                for (auto& w : g_hiddenHoverCards) {
                    if (auto fp = w.get()) fp.Visibility(mux::Visibility::Visible);
                }
                g_hiddenHoverCards.clear();
            }

            // Drop state for elements Dia has already thrown away.
            std::lock_guard<std::mutex> lock(g_stateLock);
            g_fields.erase(std::remove_if(g_fields.begin(), g_fields.end(),
                                          [](const FieldState& state) { return !state.bar.get(); }),
                           g_fields.end());
            g_stacks.erase(std::remove_if(g_stacks.begin(), g_stacks.end(),
                                          [](const StackState& state) { return !state.stack.get(); }),
                           g_stacks.end());
            g_hiddenHoverCards.erase(std::remove_if(g_hiddenHoverCards.begin(), g_hiddenHoverCards.end(),
                                                   [](const auto& w) { return !w.get(); }),
                                    g_hiddenHoverCards.end());
        }

        Peek_Tick();
        History_Tick();
        // Arc pinned tabs need to re-adopt rows Dia rebuilds on hover; run
        // periodically (not just on config/tree changes) like zioder's port.
        if (g_arcPinnedTabs.load()) {
            static int s_arcPinTick = 0;
            if (++s_arcPinTick % 3 == 0) {
                ArcUpdatePinnedTabs();
                ArcUpdatePinnedMarks();
            }
        }
        DevMode::Tick();
    }
};

std::atomic<int> Tap::g_lastStamp{-1};

// ---------------------------------------------------------------- attach

class Factory : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto tap = new (std::nothrow) Tap();
        if (!tap) return E_OUTOFMEMORY;
        HRESULT hr = tap->QueryInterface(riid, ppv);
        tap->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

static Factory g_factory;

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (clsid != CLSID_DiaTweaksXaml) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return S_FALSE; }

typedef HRESULT(WINAPI* PFN_InitializeXamlDiagnosticsEx)(LPCWSTR endpoint, DWORD pid,
                                                          LPCWSTR xamlDll, LPCWSTR tapDll,
                                                          CLSID clsid, LPCWSTR data);

static DWORD WINAPI Bootstrap(LPVOID) {
    HMODULE udk = nullptr;
    HMODULE xaml = nullptr;
    const ULONGLONG deadline = GetTickCount64() + 40000;
    while (GetTickCount64() < deadline) {
        if (!udk) udk = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
        if (!xaml) xaml = GetModuleHandleW(L"Microsoft.UI.Xaml.dll");
        if (udk && xaml) break;
        Sleep(10);
    }
    if (!udk || !xaml) {
        Log(L"attach: FrameworkUdk=%p Microsoft.UI.Xaml=%p", udk, xaml);
        return 0;
    }

    auto initialize =
        (PFN_InitializeXamlDiagnosticsEx)GetProcAddress(udk, "InitializeXamlDiagnosticsEx");
    if (!initialize) {
        Log(L"attach: InitializeXamlDiagnosticsEx not exported");
        return 0;
    }

    wchar_t xamlPath[MAX_PATH]{};
    wchar_t tapPath[MAX_PATH]{};
    GetModuleFileNameW(xaml, xamlPath, MAX_PATH);
    GetModuleFileNameW(g_module, tapPath, MAX_PATH);
    Log(L"attach: xaml=%s", xamlPath);
    Log(L"attach: tap=%s", tapPath);

    // WinUI's endpoint usually ends in 0. Try the bare name and 0..7 for a
    // while: early in Dia's start the endpoint may not be listening yet.
    for (int attempt = 0; attempt < 120 && !g_diag; ++attempt) {
        for (int suffix = -1; suffix < 8 && !g_diag; ++suffix) {
            std::wstring endpoint = L"WinUIVisualDiagConnection";
            if (suffix >= 0) endpoint += std::to_wstring(suffix);
            const wchar_t* dlls[] = {xamlPath, L"Microsoft.UI.Xaml.dll"};
            for (const wchar_t* dll : dlls) {
                HRESULT hr = initialize(endpoint.c_str(), GetCurrentProcessId(), dll, tapPath,
                                        CLSID_DiaTweaksXaml, L"");
                if (attempt == 0)
                    Log(L"attach %s (%s) -> 0x%08X", endpoint.c_str(), dll,
                        static_cast<unsigned>(hr));
                if (SUCCEEDED(hr)) {
                    Log(L"attach accepted via %s", endpoint.c_str());
                    // WinUI hands over the site asynchronously; stop probing.
                    for (int wait = 0; wait < 500 && !g_diag; ++wait) Sleep(10);
                    return 0;
                }
            }
        }
        Sleep(250);
    }
    Log(L"attach: no diagnostics endpoint answered");
    return 0;
}

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_module = instance;
    DisableThreadLibraryCalls(instance);

    wchar_t local[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) && local[0]) {
        swprintf_s(g_dir, _countof(g_dir), L"%s\\DiaTweaks", local);
        CreateDirectoryW(g_dir, nullptr);
    } else {
        GetModuleFileNameW(instance, g_dir, MAX_PATH);
        if (wchar_t* slash = wcsrchr(g_dir, L'\\')) *slash = 0;
    }
    swprintf_s(g_logPath, _countof(g_logPath), L"%s\\xaml.log", g_dir);
    swprintf_s(g_configPath, _countof(g_configPath), L"%s\\xaml.cfg", g_dir);
    swprintf_s(g_statePath, _countof(g_statePath), L"%s\\state.json", g_dir);
    swprintf_s(g_dumpRequestPath, _countof(g_dumpRequestPath), L"%s\\xaml.dump.request", g_dir);
    swprintf_s(g_treePath, _countof(g_treePath), L"%s\\xaml.tree.txt", g_dir);
    swprintf_s(g_ntpProbeRequestPath, _countof(g_ntpProbeRequestPath), L"%s\\xaml.ntp.request", g_dir);
    swprintf_s(g_ntpProbePath, _countof(g_ntpProbePath), L"%s\\xaml.ntp.txt", g_dir);
    swprintf_s(g_changelogPath, _countof(g_changelogPath), L"%s\\changelog.json", g_dir);
    DeleteFileW(g_logPath);

    HANDLE thread = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    return TRUE;
}
