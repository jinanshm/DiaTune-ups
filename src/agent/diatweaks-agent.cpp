// diatweaks-agent.cpp - keeps the display awake while Dia is open.
//
// The macOS tweak kept an unfocused window looking active. Windows has no
// equivalent window state, so the adaptation keeps the machine awake instead.
// SetThreadExecutionState only affects the calling thread's process, so this
// small helper does the asking and exits when Dia does. Nothing is injected
// into Dia.
//
// Build with scripts\build.ps1. Run with no arguments.

#include <windows.h>
#include <tlhelp32.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <set>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr wchar_t kMutexName[] = L"Local\\DiaTweaks.KeepAwake";
constexpr wchar_t kWatchMutexName[] = L"Local\\DiaTweaks.Watch";
constexpr wchar_t kProcessName[] = L"Dia.exe";
constexpr DWORD kPollMilliseconds = 2000;
constexpr int kIdleChecksBeforeExit = 3;

bool IsDiaRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

std::wstring GetAgentDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wchar_t* last = wcsrchr(buf, L'\\');
    if (last) *last = L'\0';
    return buf;
}

static bool g_newDiaEvent = false;
static std::set<DWORD> g_seenPids;

static void CALLBACK OnWindowCreated(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || g_seenPids.count(pid)) return;

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        wchar_t exePath[MAX_PATH]{};
        DWORD sz = MAX_PATH;
        if (QueryFullProcessImageNameW(hProcess, 0, exePath, &sz)) {
            const wchar_t* fn = wcsrchr(exePath, L'\\');
            if (fn && _wcsicmp(fn + 1, kProcessName) == 0) {
                g_newDiaEvent = true;
            }
        }
        CloseHandle(hProcess);
    }
}

void InjectDia(DWORD pid) {
    std::wstring dir = GetAgentDir();
    std::wstring injector = dir + L"\\diatweaks-inject.exe";
    std::wstring dll = dir + L"\\diatweaks-xaml.dll";

    if (GetFileAttributesW(injector.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }

    std::wstring cmd = L"\"" + injector + L"\" " + std::to_wstring(pid) + L" \"" + dll + L"\"";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

int RunWatch() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kWatchMutexName);
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // Already running
    }

    HWINEVENTHOOK hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, OnWindowCreated, 0, 0,
                                         WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    DWORD lastHandledPid = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Check if Dia is running
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (_wcsicmp(entry.szExeFile, kProcessName) == 0) {
                        if (entry.th32ProcessID != lastHandledPid && !g_seenPids.count(entry.th32ProcessID)) {
                            g_seenPids.insert(entry.th32ProcessID);
                            lastHandledPid = entry.th32ProcessID;
                            Sleep(600); // Give Dia's window a moment to construct its XAML tree
                            InjectDia(entry.th32ProcessID);
                        }
                    }
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }

        if (g_seenPids.size() > 500) g_seenPids.clear();
        Sleep(1500);
    }

    if (hook) UnhookWinEvent(hook);
    CloseHandle(mutex);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t **argv) {
    bool watch = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--watch") == 0) {
            watch = true;
        } else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            wprintf(L"DiaTweaks helper.\n"
                    L"  --watch  Keeps Dia patched whenever Dia opens.\n"
                    L"  (none)   Keeps display awake while Dia is open.\n");
            return 0;
        }
    }

    if (watch) {
        return RunWatch();
    }

    // One helper per user session. A second copy leaves the first alone.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex == nullptr) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    // The CLI only starts this once Dia is running. If Dia has already gone,
    // there is nothing to keep awake.
    if (!IsDiaRunning()) {
        CloseHandle(mutex);
        return 0;
    }

    int exitCode = 0;
    int idleChecks = 0;

    for (;;) {
        if (IsDiaRunning()) {
            idleChecks = 0;
            EXECUTION_STATE result = SetThreadExecutionState(
                ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
            if (result == 0) {
                exitCode = 2;
                break;
            }
        } else if (++idleChecks >= kIdleChecksBeforeExit) {
            break;
        }
        Sleep(kPollMilliseconds);
    }

    // Hand the display back to Windows' normal power policy.
    SetThreadExecutionState(ES_CONTINUOUS);

    CloseHandle(mutex);
    return exitCode;
}
