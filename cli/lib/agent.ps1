# agent.ps1 - DiaTweaks keep-awake and launch watcher helper.
[CmdletBinding()]
param(
    [switch]$Watch
)

$scriptDir = Split-Path -Parent $PSCommandPath
$root = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$binDir = Join-Path $root 'bin'
if (-not (Test-Path -LiteralPath $binDir)) {
    $binDir = $scriptDir
}

$injectorScript = Join-Path $scriptDir 'injector.ps1'
if (-not (Test-Path -LiteralPath $injectorScript)) {
    $injectorScript = Join-Path (Join-Path $root 'cli\lib') 'injector.ps1'
}
if (Test-Path -LiteralPath $injectorScript) {
    . $injectorScript
}

if ($Watch) {
    # Keep DiaTweaks active whenever Dia opens. Only the browser process owns
    # Dia's XAML window; injecting into its renderer helpers just leaves timers
    # that can never attach. Retry until the DLL is actually loaded, because
    # Dia's XAML tree is not ready the instant the process appears.
    $windowsHelper = Join-Path (Join-Path $root 'cli\lib') 'windows.ps1'
    if (Test-Path -LiteralPath $windowsHelper) {
        . $windowsHelper
    }

    $lastAttempt = @{}
    while ($true) {
        $diaProcesses = @(Get-Process -Name 'Dia' -ErrorAction SilentlyContinue)
        $target = $null
        if ($diaProcesses.Count -gt 0) {
            $ids = @($diaProcesses | Select-Object -ExpandProperty Id)
            if ('DiaTweaks.Native' -as [type]) {
                $main = @(Get-TopLevelWindows | Where-Object {
                    $ids -contains $_.ProcessId -and
                    $_.ClassName -eq 'WinUIDesktopWin32WindowClass' -and
                    $_.Width -ge 400 -and $_.Height -ge 300
                } | Select-Object -First 1)
                if ($main) {
                    $target = $diaProcesses | Where-Object { $_.Id -eq $main.ProcessId } | Select-Object -First 1
                }
            }
            if (-not $target) {
                # Before the window exists the browser is the oldest Dia process.
                try {
                    $target = $diaProcesses | Sort-Object StartTime | Select-Object -First 1
                } catch {
                    $target = $diaProcesses | Select-Object -First 1
                }
            }
        }

        if ($target) {
            $key = "$($target.Id)"
            $loaded = $false
            try {
                $loaded = @($target.Modules | Where-Object { $_.ModuleName -eq 'diatweaks-xaml.dll' }).Count -gt 0
            } catch {
                $loaded = $false
            }

            if (-not $loaded) {
                $last = $lastAttempt[$key]
                if (-not $last -or ((Get-Date) - $last).TotalSeconds -ge 3) {
                    $lastAttempt[$key] = Get-Date
                    $dllPath = Join-Path $binDir 'diatweaks-xaml.dll'
                    if (Test-Path -LiteralPath $dllPath) {
                        Start-Sleep -Milliseconds 250
                        if (Get-Command 'Invoke-DiaTweaksInjection' -ErrorAction SilentlyContinue) {
                            Invoke-DiaTweaksInjection -ProcessId $target.Id -DllPath $dllPath | Out-Null
                        } elseif (Test-Path -LiteralPath (Join-Path $binDir 'diatweaks-inject.exe')) {
                            $env:SEE_MASK_NOZONECHECKS = '1'
                            & (Join-Path $binDir 'diatweaks-inject.exe') $target.Id $dllPath 2>&1 | Out-Null
                        }
                    }
                }
            }
        }

        if ($lastAttempt.Count -gt 200) { $lastAttempt.Clear() }
        Start-Sleep -Milliseconds 1500
    }
} else {
    # Keeps the display awake while Dia is open
    Add-Type -TypeDefinition @"
    using System;
    using System.Runtime.InteropServices;
    public static class WinPower {
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint SetThreadExecutionState(uint esFlags);
    }
"@ -ErrorAction SilentlyContinue

    $ES_CONTINUOUS = 0x80000000
    $ES_SYSTEM_REQUIRED = 0x00000001
    $ES_DISPLAY_REQUIRED = 0x00000002

    $idleChecks = 0
    while ($true) {
        $running = ((Get-Process -Name 'Dia' -ErrorAction SilentlyContinue).Count -gt 0)
        if ($running) {
            $idleChecks = 0
            [WinPower]::SetThreadExecutionState($ES_CONTINUOUS -bor $ES_SYSTEM_REQUIRED -bor $ES_DISPLAY_REQUIRED)
        } else {
            $idleChecks++
            if ($idleChecks -ge 3) { break }
        }
        Start-Sleep -Seconds 2
    }
    [WinPower]::SetThreadExecutionState($ES_CONTINUOUS)
}
