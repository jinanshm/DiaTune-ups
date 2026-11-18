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

bool IsXamlDllLoaded(DWORD pid) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool loaded = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, L"diatweaks-xaml.dll") == 0) {
                loaded = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return loaded;
}

bool IsDiaProcess(DWORD pid) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return false;
    wchar_t exePath[MAX_PATH]{};
    DWORD size = MAX_PATH;
    bool isDia = false;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
        const wchar_t* file = wcsrchr(exePath, L'\\');
        isDia = file && _wcsicmp(file + 1, kProcessName) == 0;
    }
    CloseHandle(hProcess);
    return isDia;
}

struct Candidate {
    DWORD pid = 0;
    bool ownsMainWindow = false;
};

static BOOL CALLBACK FindMainWindow(HWND hwnd, LPARAM param) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[128]{};
    if (!GetClassNameW(hwnd, cls, 128) || wcscmp(cls, L"WinUIDesktopWin32WindowClass") != 0) return TRUE;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return TRUE;
    if (rc.right - rc.left < 400 || rc.bottom - rc.top < 300) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid || !IsDiaProcess(pid)) return TRUE;
    *reinterpret_cast<Candidate*>(param) = Candidate{pid, true};
    return FALSE;
}

// Dia's browser process: the one that owns the XAML window once it exists.
// Before the window appears the browser is the oldest Dia process.
DWORD FindDiaBrowserProcess() {
    Candidate windowCandidate;
    EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&windowCandidate));
    if (windowCandidate.pid) return windowCandidate.pid;

    DWORD oldestPid = 0;
    ULONGLONG oldestStart = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kProcessName) != 0) continue;
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!hProcess) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(hProcess, &created, &exited, &kernel, &user)) {
                ULONGLONG start = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
                if (!oldestPid || start < oldestStart) {
                    oldestPid = entry.th32ProcessID;
                    oldestStart = start;
                }
            }
            CloseHandle(hProcess);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return oldestPid;
}

int RunWatch() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kWatchMutexName);
    if (mutex == nullptr || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0; // Already running
    }

    for (;;) {
        DWORD pid = FindDiaBrowserProcess();
        if (pid && !IsXamlDllLoaded(pid)) {
            // Retrying is safe: LoadLibraryW on a loaded DLL does nothing, and
            // Dia's XAML endpoint may not be listening the first time.
            Sleep(250);
            InjectDia(pid);
        }
        Sleep(1500);
    }

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
