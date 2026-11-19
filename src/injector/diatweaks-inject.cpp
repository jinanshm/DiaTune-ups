// diatweaks-loader.exe <pid> <dll-path>
//
// Minimal LoadLibraryW injector for diatweaks-xaml.dll.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cwchar>
#pragma comment(lib, "advapi32.lib")

static void EnableDebugPrivilege() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(token);
    }
}

static bool ModuleLoaded(DWORD pid, const wchar_t* dllPath) {
    const wchar_t* slash = wcsrchr(dllPath, L'\\');
    const wchar_t* base = slash ? slash + 1 : dllPath;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, base) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: diatweaks-loader <pid> <dll-path>\n");
        return 1;
    }

    DWORD pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    if (pid == 0) {
        wprintf(L"invalid pid\n");
        return 1;
    }

    EnableDebugPrivilege();

    if (ModuleLoaded(pid, argv[2])) {
        wprintf(L"already loaded\n");
        return 0;
    }

    // AppContainer processes (like packaged Dia) reject PROCESS_ALL_ACCESS with error 5.
    // Request only the exact permissions needed for remote allocation and thread execution.
    DWORD access = PROCESS_CREATE_THREAD |
                   PROCESS_QUERY_INFORMATION |
                   PROCESS_VM_OPERATION |
                   PROCESS_VM_WRITE |
                   PROCESS_VM_READ |
                   SYNCHRONIZE;
    HANDLE process = OpenProcess(access, FALSE, pid);
    if (!process) {
        DWORD err = GetLastError();
        // Fallback with PROCESS_QUERY_LIMITED_INFORMATION
        access = PROCESS_CREATE_THREAD |
                 PROCESS_QUERY_LIMITED_INFORMATION |
                 PROCESS_VM_OPERATION |
                 PROCESS_VM_WRITE |
                 PROCESS_VM_READ |
                 SYNCHRONIZE;
        process = OpenProcess(access, FALSE, pid);
        if (!process) {
            wprintf(L"OpenProcess failed: %lu (secondary: %lu)\n", err, GetLastError());
            return 2;
        }
    }

    SIZE_T bytes = (wcslen(argv[2]) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (!remote) {
        wprintf(L"VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(process);
        return 3;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, argv[2], bytes, &written)) {
        wprintf(L"WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 4;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    DWORD tid = 0;
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, &tid);
    if (!thread) {
        wprintf(L"CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return 5;
    }

    WaitForSingleObject(thread, 15000);
    DWORD result = 0;
    GetExitCodeThread(thread, &result);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);

    wprintf(L"LoadLibraryW returned 0x%08lX (tid=%lu)\n", result, tid);
    return result ? 0 : 6;
}
