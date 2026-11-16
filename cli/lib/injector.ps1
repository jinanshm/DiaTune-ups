# injector.ps1 - In-process Win32 DLL injector for DiaTweaks.
# Performs LoadLibraryW injection directly inside PowerShell without calling any external .exe,
# avoiding Windows ShellExecute "Open File - Security Warning" prompts.

function Invoke-DiaTweaksInjection {
    param(
        [Parameter(Mandatory=$true)]
        [int]$ProcessId,
        [Parameter(Mandatory=$true)]
        [string]$DllPath
    )

    if (-not (Test-Path -LiteralPath $DllPath)) {
        return $false
    }

    try {
        if (-not ([System.Management.Automation.PSTypeName]'DiaTweaks.NativeInjector').Type) {
            $src = @"
using System;
using System.Runtime.InteropServices;
namespace DiaTweaks {
    public static class NativeInjector {
        [DllImport("advapi32.dll", SetLastError = true)]
        public static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

        [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        public static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out long lpLuid);

        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        public struct TOKEN_PRIVILEGES {
            public int PrivilegeCount;
            public long Luid;
            public int Attributes;
        }

        [DllImport("advapi32.dll", SetLastError = true)]
        public static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, int BufferLength, IntPtr PreviousState, IntPtr ReturnLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, int dwProcessId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out IntPtr lpNumberOfBytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr GetModuleHandle(string lpModuleName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out uint lpThreadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool VirtualFreeEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint dwFreeType);

        public static void EnablePrivilege() {
            try {
                IntPtr hToken;
                if (OpenProcessToken(System.Diagnostics.Process.GetCurrentProcess().Handle, 0x0020 | 0x0008, out hToken)) {
                    long luid;
                    if (LookupPrivilegeValue(null, "SeDebugPrivilege", out luid)) {
                        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
                        tp.PrivilegeCount = 1;
                        tp.Luid = luid;
                        tp.Attributes = 0x00000002;
                        AdjustTokenPrivileges(hToken, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
                    }
                    CloseHandle(hToken);
                }
            } catch {}
        }

        public static int Inject(int processId, string dllPath) {
            EnablePrivilege();
            uint access = 0x0002 | 0x0400 | 0x0008 | 0x0020 | 0x0010 | 0x00100000;
            IntPtr hProcess = OpenProcess(access, false, processId);
            if (hProcess == IntPtr.Zero) {
                access = 0x0002 | 0x1000 | 0x0008 | 0x0020 | 0x0010 | 0x00100000;
                hProcess = OpenProcess(access, false, processId);
                if (hProcess == IntPtr.Zero) return 2;
            }

            byte[] bytes = System.Text.Encoding.Unicode.GetBytes(dllPath + "\0");
            IntPtr pRemote = VirtualAllocEx(hProcess, IntPtr.Zero, (uint)bytes.Length, 0x1000 | 0x2000, 0x04);
            if (pRemote == IntPtr.Zero) {
                CloseHandle(hProcess);
                return 3;
            }

            IntPtr written;
            if (!WriteProcessMemory(hProcess, pRemote, bytes, (uint)bytes.Length, out written)) {
                VirtualFreeEx(hProcess, pRemote, 0, 0x8000);
                CloseHandle(hProcess);
                return 4;
            }

            IntPtr hKernel = GetModuleHandle("kernel32.dll");
            IntPtr pLoadLibrary = GetProcAddress(hKernel, "LoadLibraryW");
            uint threadId;
            IntPtr hThread = CreateRemoteThread(hProcess, IntPtr.Zero, 0, pLoadLibrary, pRemote, 0, out threadId);
            if (hThread == IntPtr.Zero) {
                VirtualFreeEx(hProcess, pRemote, 0, 0x8000);
                CloseHandle(hProcess);
                return 5;
            }

            WaitForSingleObject(hThread, 15000);
            CloseHandle(hThread);
            VirtualFreeEx(hProcess, pRemote, 0, 0x8000);
            CloseHandle(hProcess);
            return 0;
        }
    }
}
"@
            Add-Type -TypeDefinition $src -ErrorAction Stop
        }

        $res = [DiaTweaks.NativeInjector]::Inject($ProcessId, $DllPath)
        return ($res -eq 0)
    } catch {
        return $false
    }
}
