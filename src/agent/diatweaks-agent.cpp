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

namespace {

constexpr wchar_t kMutexName[] = L"Local\\DiaTweaks.KeepAwake";
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

}  // namespace

int wmain(int argc, wchar_t **argv) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0) {
            wprintf(L"Keeps the display awake while Dia is open.\n"
                    L"Started by diatweaks apply/watch/launch; exits when Dia closes.\n");
            return 0;
        }
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
