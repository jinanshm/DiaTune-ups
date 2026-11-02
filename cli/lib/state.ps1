# state.ps1 - the tweak manifest, the saved on/off state, and applying both.

$script:DiaTweaksRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:ManifestPath = Join-Path $script:DiaTweaksRoot 'patches.json'
if (-not $env:LOCALAPPDATA) {
    $env:LOCALAPPDATA = [Environment]::GetFolderPath('LocalApplicationData')
}
$script:StateDir = Join-Path $env:LOCALAPPDATA 'DiaTweaks'
$script:StatePath = Join-Path $script:StateDir 'state.json'
$script:AgentPath = Join-Path $script:DiaTweaksRoot 'bin\diatweaks-agent.exe'
$script:AgentName = 'diatweaks-agent'
$script:Manifest = $null

# DiaTweaks' own names are keys or titles, matched ignoring case, spaces and
# punctuation. "Square window corners" and squareSidebarCorners both work.
function ConvertTo-TweakName {
    param([string]$Name)
    $clean = $Name -replace '[^a-zA-Z0-9]', ''
    return $clean.ToLowerInvariant()
}

function Get-EditDistance {
    param([string]$Left, [string]$Right)

    $a = $Left
    $b = $Right
    $previous = [int[]]::new($b.Length + 1)
    $current = [int[]]::new($b.Length + 1)
    for ($j = 0; $j -le $b.Length; $j++) { $previous[$j] = $j }

    for ($i = 1; $i -le $a.Length; $i++) {
        $current[0] = $i
        for ($j = 1; $j -le $b.Length; $j++) {
            $cost = if ($a[$i - 1] -eq $b[$j - 1]) { 0 } else { 1 }
            $current[$j] = [Math]::Min(
                [Math]::Min($current[$j - 1] + 1, $previous[$j] + 1),
                $previous[$j - 1] + $cost
            )
        }
        $swap = $previous
        $previous = $current
        $current = $swap
    }

    return $previous[$b.Length]
}

<#
.SYNOPSIS
    Loads and validates patches.json.
#>
function Read-Manifest {
    if ($script:Manifest) { return $script:Manifest }

    if (-not (Test-Path -LiteralPath $script:ManifestPath)) {
        throw "patches.json is missing: $($script:ManifestPath). Re-clone DiaTweaks-Windows."
    }

    try {
        $data = Get-Content -LiteralPath $script:ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        throw "patches.json is damaged ($($_.Exception.Message))."
    }

    if ($data.version -ne 1) {
        throw "patches.json is format $($data.version), which this diatweaks can't read."
    }

    $patches = @($data.patches)
    if ($patches.Count -eq 0) {
        throw "patches.json lists no tweaks."
    }

    $keys = @{}
    foreach ($patch in $patches) {
        if (-not $patch.key -or -not $patch.title -or -not $patch.detail) {
            throw "patches.json has a tweak without a key, title or detail."
        }
        if ($keys.ContainsKey($patch.key)) {
            throw "patches.json lists $($patch.key) twice."
        }
        $keys[$patch.key] = $true
    }

    foreach ($patch in $patches) {
        foreach ($link in @($patch.requires, $patch.disabledBy)) {
            if ($link -and -not $keys.ContainsKey($link)) {
                throw "patches.json says $($patch.key) links to $link, which it doesn't list."
            }
        }
    }

    $script:Manifest = $data
    return $script:Manifest
}

function Get-Tweak {
    param([string]$Key)
    return (Read-Manifest).patches | Where-Object { $_.key -eq $Key } | Select-Object -First 1
}

function Find-Tweak {
    param($Manifest, [string]$Name)

    $wanted = ConvertTo-TweakName $Name
    $match = $Manifest.patches | Where-Object {
        (ConvertTo-TweakName $_.key) -eq $wanted -or (ConvertTo-TweakName $_.title) -eq $wanted
    } | Select-Object -First 1
    if ($match) { return $match }

    $close = @($Manifest.patches | Where-Object {
        $key = ConvertTo-TweakName $_.key
        $title = ConvertTo-TweakName $_.title
        ($wanted.Length -ge 3 -and ($key.Contains($wanted) -or $wanted.Contains($key))) -or
        ($wanted.Length -ge 3 -and ($title.Contains($wanted) -or $wanted.Contains($title))) -or
        (Get-EditDistance $wanted $key) -le 2 -or
        (Get-EditDistance $wanted $title) -le 2
    } | ForEach-Object { $_.key })

    if ($close.Count -eq 0) {
        throw "There's no tweak called ""$Name"". Run ``diatweaks list`` to see them."
    }
    if ($close.Count -eq 1) {
        throw "There's no tweak called ""$Name"". Did you mean $($close[0])?"
    }
    throw "There's no tweak called ""$Name"". Did you mean one of $($close -join ', ')?"
}

function Get-DefaultState {
    $state = @{}
    foreach ($patch in (Read-Manifest).patches) {
        $state[$patch.key] = [bool]$patch.defaultOn
    }
    return $state
}

<#
.SYNOPSIS
    Reads the saved tweak states, filling anything missing with its default.
#>
function Read-State {
    $state = Get-DefaultState
    if (-not (Test-Path -LiteralPath $script:StatePath)) { return $state }

    try {
        $data = Get-Content -LiteralPath $script:StatePath -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        Write-Warning "Ignoring damaged state file ($($_.Exception.Message)); using defaults."
        return $state
    }

    if ($data.version -ne 1 -or -not $data.tweaks) {
        Write-Warning "Ignoring state file with an unknown format; using defaults."
        return $state
    }

    foreach ($patch in (Read-Manifest).patches) {
        $saved = $data.tweaks.($patch.key)
        if ($saved -is [bool]) { $state[$patch.key] = $saved }
    }
    return $state
}

function Write-State {
    param([Parameter(Mandatory)][hashtable]$State)

    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null

    $tweaks = [ordered]@{}
    foreach ($patch in (Read-Manifest).patches) {
        $tweaks[$patch.key] = [bool]$State[$patch.key]
    }

    $document = [ordered]@{
        version = 1
        tweaks  = $tweaks
    }
    $json = ($document | ConvertTo-Json -Depth 4) + "`n"

    $temporary = "$($script:StatePath).tmp"
    [System.IO.File]::WriteAllText($temporary, $json, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $script:StatePath -Force
}

function Reset-State {
    # Restore means "stop changing Dia's windows", not "go back to the
    # defaults that were on for a fresh install".
    $off = @{}
    foreach ($patch in (Read-Manifest).patches) {
        $off[$patch.key] = $false
    }
    Write-State -State $off
}

# --- The keep-awake helper -------------------------------------------------

function Test-AgentBuilt {
    return (Test-Path -LiteralPath $script:AgentPath)
}

function Get-AgentProcesses {
    return @(Get-Process -Name $script:AgentName -ErrorAction SilentlyContinue)
}

function Start-Agent {
    if (-not (Test-AgentBuilt)) { return $false }
    if ((Get-AgentProcesses).Count -gt 0) { return $true }

    # The helper exits by itself when the last Dia process goes away.
    Start-Process -FilePath $script:AgentPath `
        -WorkingDirectory (Split-Path -Parent $script:AgentPath) `
        -WindowStyle Hidden | Out-Null
    return $true
}

function Stop-Agent {
    $processes = Get-AgentProcesses
    foreach ($process in $processes) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

# --- Applying --------------------------------------------------------------

<#
.SYNOPSIS
    Applies the saved state to every open Dia window and reconciles the
    keep-awake helper with it.

.DESCRIPTION
    DWM window attributes belong to the window, not to Dia's profile, so they
    have to be applied again whenever Dia opens a window. `apply` does it once,
    `watch` keeps doing it, and `launch` does it after starting Dia.
#>
function Invoke-Apply {
    $state = Read-State
    $windows = @(Get-DiaWindows)
    $edge = [bool]$state['edgeToEdge']
    $square = ([bool]$state['squareSidebarCorners']) -and $edge

    $warnings = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-WindowsEleven)) {
        $warnings.Add('These window tweaks need Windows 11; this PC is on an older Windows.')
    }

    $applied = 0
    $borderFailures = 0
    $cornerFailures = 0
    $borderCode = 0
    $cornerCode = 0
    foreach ($window in $windows) {
        $result = Set-TweakedWindow -Handle $window.Handle -EdgeToEdge $edge -SquareCorners $square
        if ($edge -and $result.BorderHr -ne 0) {
            $borderFailures++
            $borderCode = $result.BorderHr
        }
        if ($square -and $result.CornersHr -ne 0) {
            $cornerFailures++
            $cornerCode = $result.CornersHr
        }
        $applied++
    }

    if ($borderFailures -gt 0) {
        $warnings.Add("Windows refused the border change on $borderFailures window(s) ($(Format-HResult $borderCode)).")
    }
    if ($cornerFailures -gt 0) {
        $warnings.Add("Windows refused the corner change on $cornerFailures window(s) ($(Format-HResult $cornerCode)).")
    }

    $keepAwake = [bool]$state['keepWindowsActive']
    $agentBuilt = Test-AgentBuilt
    $agentRunning = $false

    if ($keepAwake -and $windows.Count -gt 0) {
        if ($agentBuilt) {
            [void](Start-Agent)
            $agentRunning = $true
        } else {
            $warnings.Add("Keep the display awake is on, but the helper isn't built. Run scripts\build.ps1.")
        }
    } else {
        Stop-Agent
    }

    return [pscustomobject]@{
        Windows      = $applied
        EdgeToEdge   = $edge
        Square       = $square
        KeepAwake    = $keepAwake
        AgentBuilt   = $agentBuilt
        AgentRunning = $agentRunning
        Warnings     = @($warnings)
    }
}

<#
.SYNOPSIS
    Puts every Dia window back to the default Windows frame.
#>
function Invoke-RestoreWindows {
    $restored = 0
    foreach ($window in @(Get-DiaWindows)) {
        [void](Reset-TweakedWindow -Handle $window.Handle)
        $restored++
    }
    return $restored
}
