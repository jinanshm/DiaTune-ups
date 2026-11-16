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
    # Keep DiaTweaks active whenever Dia opens
    $seenPids = @{}
    while ($true) {
        $diaProcesses = @(Get-Process -Name 'Dia' -ErrorAction SilentlyContinue)
        foreach ($proc in $diaProcesses) {
            if (-not $seenPids.ContainsKey($proc.Id)) {
                $seenPids[$proc.Id] = $true
                Start-Sleep -Milliseconds 600
                $dll = Join-Path $binDir 'diatweaks-xaml.dll'
                if (Test-Path -LiteralPath $dll) {
                    if (Get-Command 'Invoke-DiaTweaksInjection' -ErrorAction SilentlyContinue) {
                        Invoke-DiaTweaksInjection -ProcessId $proc.Id -DllPath $dll | Out-Null
                    } elseif (Test-Path -LiteralPath (Join-Path $binDir 'diatweaks-inject.exe')) {
                        $env:SEE_MASK_NOZONECHECKS = '1'
                        & (Join-Path $binDir 'diatweaks-inject.exe') $proc.Id $dll 2>&1 | Out-Null
                    }
                }
            }
        }
        if ($seenPids.Count -gt 500) { $seenPids.Clear() }
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
