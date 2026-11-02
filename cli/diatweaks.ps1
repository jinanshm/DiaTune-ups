# diatweaks.ps1 - turn Dia tweaks on and off on Windows.
#
# Run it through diatweaks.cmd in the repository root, or directly:
#   powershell -ExecutionPolicy Bypass -File cli\diatweaks.ps1 list

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'lib\windows.ps1')
. (Join-Path $PSScriptRoot 'lib\dia.ps1')
. (Join-Path $PSScriptRoot 'lib\state.ps1')

$usage = @'
Turn Dia tweaks on and off on Windows.

Usage: diatweaks <command> [tweak...] [-y]

Commands:
  list                    Show every tweak and whether it's on
  enable <tweak...>       Turn tweaks on
  disable <tweak...>      Turn tweaks off
  status                  Show Dia's version and which tweaks are on
  apply                   Apply the tweaks to Dia's open windows now
  watch                   Keep applying them as Dia opens windows
  launch                  Open Dia, then apply the tweaks
  restore [--purge]       Put Dia's windows back to normal
  help                    Show this message

Tweaks can be named by key or title, like edgeToEdge or "Edge-to-edge window".
Add -y or --yes to answer yes to every question.

The window tweaks are DWM hints, so Dia has to be open when they are applied.
Use `diatweaks launch` or leave `diatweaks watch` running to keep them on.
'@

function Write-Info {
    param([string]$Message)
    Write-Host $Message
}

function Write-Success {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Green
}

function Write-Warn {
    param([string]$Message)
    Write-Host $Message -ForegroundColor Yellow
}

function Read-Confirmation {
    param([string]$Question)

    $answer = Read-Host "$Question [Y/n]"
    if ([string]::IsNullOrWhiteSpace($answer)) { return $true }
    return ($answer.Trim() -match '^(y|yes)$')
}

function Write-TweakList {
    $manifest = Read-Manifest
    $state = Read-State
    $width = ($manifest.patches | ForEach-Object { $_.key.Length } |
        Measure-Object -Maximum).Maximum

    foreach ($patch in $manifest.patches) {
        if (-not $patch.supported) {
            $status = 'n/a'
        } elseif ($state[$patch.key]) {
            $status = 'on'
        } else {
            $status = 'off'
        }

        $title = $patch.title
        if (-not $patch.supported) {
            $title += '  (not available on Windows)'
        }
        Write-Host ("{0,-4} {1,-$width}  {2}" -f $status, $patch.key, $title)
    }
}

function Write-ApplySummary {
    param($Result)

    foreach ($warning in $Result.Warnings) { Write-Warn $warning }

    if ($Result.Windows -eq 0) {
        Write-Info 'Dia is not running. Run `diatweaks launch` to open it with these tweaks, or leave `diatweaks watch` running.'
        return
    }

    $what = @()
    if ($Result.EdgeToEdge) { $what += 'an edge-to-edge window' }
    if ($Result.Square) { $what += 'square corners' }
    if ($what.Count -eq 0) {
        Write-Info "All window tweaks are off; Dia's windows are back to the normal frame."
    } else {
        $plural = if ($Result.Windows -eq 1) { '' } else { 's' }
        Write-Success ("Applied {0} to {1} Dia window{2}." -f ($what -join ' and '), $Result.Windows, $plural)
    }

    if ($Result.KeepAwake -and $Result.AgentRunning) {
        Write-Info 'The display will stay awake while Dia is open.'
    }
}

function Invoke-Status {
    $info = Get-DiaInfo
    if ($info) {
        Write-Info "Dia $($info.Version)  ($($info.PackageFullName))"
    } else {
        Write-Info "Dia isn't installed for this account."
    }

    if (Test-DiaRunning) {
        $windows = @(Get-DiaWindows)
        $plural = if ($windows.Count -eq 1) { '' } else { 's' }
        Write-Info "Dia is running ($($windows.Count) window$plural)."
    } else {
        Write-Info 'Dia is not running.'
    }

    $state = Read-State
    if ($state['keepWindowsActive']) {
        if ((Get-AgentProcesses).Count -gt 0) {
            Write-Info 'Keep-awake helper: running.'
        } elseif (-not (Test-AgentBuilt)) {
            Write-Info 'Keep-awake helper: not built (run scripts\build.ps1).'
        } else {
            Write-Info 'Keep-awake helper: not running.'
        }
    } else {
        Write-Info 'Keep-awake: off.'
    }

    $windowsEleven = if (Test-WindowsEleven) { 'yes' } else { 'no' }
    Write-Info "Windows 11: $windowsEleven."
    Write-Info ''
    Write-TweakList
}

function Invoke-SetTweaks {
    param(
        [string[]]$Names,
        [bool]$On,
        [bool]$AssumeYes
    )

    if (-not $Names -or $Names.Count -eq 0) {
        throw 'Name at least one tweak. Run `diatweaks list` to see them.'
    }

    $manifest = Read-Manifest
    $current = Read-State
    $wanted = @{}
    foreach ($key in $current.Keys) { $wanted[$key] = $current[$key] }

    $targets = @()
    foreach ($name in $Names) {
        $patch = Find-Tweak $manifest $name
        if (-not $patch.supported) {
            $reason = $patch.detail
            $prefix = 'Not available on Windows. '
            if ($reason.StartsWith($prefix)) { $reason = $reason.Substring($prefix.Length) }
            throw "$($patch.title) isn't available on Windows. $reason"
        }
        if (-not ($targets | Where-Object { $_.key -eq $patch.key })) {
            $targets += $patch
        }
    }

    $onOff = if ($On) { 'on' } else { 'off' }
    foreach ($patch in $targets) {
        if ([bool]$current[$patch.key] -eq $On) {
            Write-Info "$($patch.title) is already $onOff."
        }
        $wanted[$patch.key] = $On
    }

    # Turning something on also offers whatever it needs, one link at a time.
    if ($On) {
        foreach ($patch in $targets) {
            $needer = $patch
            while ($needer.requires) {
                $required = Get-Tweak $needer.requires
                if ($wanted[$required.key]) { break }

                $question = "$($needer.title) needs $($required.title), which is off. Turn that on too?"
                if ($AssumeYes -or (Read-Confirmation $question)) {
                    $wanted[$required.key] = $true
                    $needer = $required
                } else {
                    break
                }
            }
        }
    }

    $changes = @($manifest.patches | Where-Object {
        [bool]$current[$_.key] -ne [bool]$wanted[$_.key]
    })

    if ($changes.Count -eq 0) {
        Write-Info 'Nothing to change.'
        return
    }

    Write-State -State $wanted

    foreach ($patch in $changes) {
        $word = if ($wanted[$patch.key]) { 'on' } else { 'off' }
        Write-Success "Turned $word $($patch.title)."
    }

    foreach ($patch in $manifest.patches) {
        if ($patch.requires -and $wanted[$patch.key] -and -not $wanted[$patch.requires]) {
            $required = Get-Tweak $patch.requires
            Write-Warn "$($patch.title) does nothing while $($required.title) is off."
        }
    }

    $result = Invoke-Apply
    Write-ApplySummary -Result $result
}

function Invoke-ApplyCommand {
    $result = Invoke-Apply
    Write-ApplySummary -Result $result
}

function Invoke-Watch {
    Write-Info 'Watching Dia every 2 seconds. Press Ctrl+C to stop.'
    $lastSignature = $null
    $seenWarnings = @{}

    while ($true) {
        try {
            $result = Invoke-Apply
            $signature = '{0}|{1}|{2}|{3}' -f $result.Windows, $result.EdgeToEdge, $result.Square, $result.AgentRunning
            if ($signature -ne $lastSignature) {
                Write-ApplySummary -Result $result
                $lastSignature = $signature
            } else {
                foreach ($warning in $result.Warnings) {
                    if (-not $seenWarnings.ContainsKey($warning)) {
                        Write-Warn $warning
                        $seenWarnings[$warning] = $true
                    }
                }
            }
        } catch {
            Write-Warn $_.Exception.Message
        }
        Start-Sleep -Seconds 2
    }
}

function Invoke-Launch {
    if (Test-DiaRunning) {
        Write-Info 'Dia is already running.'
    } else {
        Start-Dia
        Write-Info 'Opening Dia...'
    }

    $windows = @(Wait-DiaWindow -TimeoutSeconds 25)
    if ($windows.Count -eq 0) {
        throw "Dia didn't open a window within 25 seconds."
    }

    # Give the first frame a moment to settle before changing the frame.
    Start-Sleep -Milliseconds 750

    # Dia sometimes replaces its window while it finishes starting. Apply for
    # a few seconds so the final window gets the tweaks too.
    $deadline = (Get-Date).AddSeconds(3)
    do {
        $result = Invoke-Apply
        Start-Sleep -Milliseconds 500
    } while ((Get-Date) -lt $deadline)

    Write-ApplySummary -Result $result
}

function Invoke-Restore {
    param([bool]$Purge)

    Stop-Agent
    $restored = Invoke-RestoreWindows

    if ($Purge) {
        if (Test-Path -LiteralPath $script:StateDir) {
            Remove-Item -LiteralPath $script:StateDir -Recurse -Force
        }
        Write-Info "Deleted DiaTweaks' saved settings ($($script:StateDir))."
        Write-Info 'Dia itself was never modified.'
    } else {
        Reset-State
        Write-Info 'Turned every DiaTweaks tweak off.'
    }

    if ($restored -gt 0) {
        $plural = if ($restored -eq 1) { '' } else { 's' }
        Write-Success "Put $restored Dia window$plural back to the normal frame."
    } else {
        Write-Info 'Dia is not running, so there were no windows to put back.'
    }
}

# --- Parsing ---------------------------------------------------------------

$raw = @($args)
$assumeYes = $false
$wantsHelp = $false
$words = @()

foreach ($argument in $raw) {
    $lower = $argument.ToLowerInvariant()
    if ($lower -in @('-y', '--yes', '/y')) {
        $assumeYes = $true
    } elseif ($lower -in @('-h', '--help', '-?', 'help')) {
        $wantsHelp = $true
    } else {
        $words += $argument
    }
}

if ($wantsHelp) {
    Write-Host $usage
    exit 0
}

if ($words.Count -eq 0) {
    Write-Host $usage
    exit 2
}

$command = $words[0].ToLowerInvariant()
$names = @()
if ($words.Count -gt 1) {
    $names = @($words[1..($words.Count - 1)])
}

$purge = $false
if ($command -eq 'restore') {
    $purge = @($names | Where-Object { $_.ToLowerInvariant() -eq '--purge' }).Count -gt 0
    $names = @($names | Where-Object { $_.ToLowerInvariant() -ne '--purge' })
}

try {
    switch ($command) {
        'list' {
            Write-TweakList
        }
        'enable' {
            Invoke-SetTweaks -Names $names -On $true -AssumeYes $assumeYes
        }
        'disable' {
            Invoke-SetTweaks -Names $names -On $false -AssumeYes $assumeYes
        }
        'status' {
            Invoke-Status
        }
        'apply' {
            Invoke-ApplyCommand
        }
        'watch' {
            Invoke-Watch
        }
        'launch' {
            Invoke-Launch
        }
        'restore' {
            Invoke-Restore -Purge $purge
        }
        default {
            Write-Host "There's no command called ""$command""."
            Write-Host ''
            Write-Host $usage
            exit 2
        }
    }
} catch {
    Write-Host ''
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
