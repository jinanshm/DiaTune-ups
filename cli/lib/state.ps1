# state.ps1 - the tweak manifest, the saved on/off state, and applying both.

$script:DiaTweaksRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$script:ManifestPath = Join-Path $script:DiaTweaksRoot 'patches.json'
if (-not $env:LOCALAPPDATA) {
    $env:LOCALAPPDATA = [Environment]::GetFolderPath('LocalApplicationData')
}
$script:StateDir = Join-Path $env:LOCALAPPDATA 'DiaTweaks'
$script:StatePath = Join-Path $script:StateDir 'state.json'
$script:AgentPath = Join-Path $script:DiaTweaksRoot 'bin\diatweaks-agent.exe'
$script:AgentScriptPath = Join-Path $script:DiaTweaksRoot 'bin\diatweaks-agent.ps1'
if (-not (Test-Path -LiteralPath $script:AgentScriptPath)) {
    $script:AgentScriptPath = Join-Path $script:DiaTweaksRoot 'cli\lib\agent.ps1'
}
$script:AgentName = 'diatweaks-agent'
$script:XamlDllPath = Join-Path $script:DiaTweaksRoot 'bin\diatweaks-xaml.dll'
$script:InjectorPath = Join-Path $script:DiaTweaksRoot 'bin\diatweaks-inject.exe'
$injectorScript = Join-Path $PSScriptRoot 'injector.ps1'
if (Test-Path -LiteralPath $injectorScript) {
    . $injectorScript
}
$script:XamlConfigPath = Join-Path $script:StateDir 'xaml.cfg'
$script:XamlKeys = @(
    'hideToolbar',
    'revealToolbarOnHover',
    'hideSidebarButton',
    'hideChatButton',
    'showAddressField',
    'hideTrafficLights',
    'hidePersonalizeButton',
    'hideUpgradeBanner',
    'hideTabSearchButton',
    'hideLoadingAnimation',
    'hidePinnedTabBorder',
    'hideTabHoverCard',
    'hideLinkAddress',
    'arcPinnedTabs',
    'hideHistorySuggestions',
    'translucentSidebar',
    'sidebarGrain',
    'peek',
    'toolbarColor',
    'dontPopUpDownloads',
    'profileEmoji',
    'pinnedExtensions',
    'showToolbarWithSidebar',
    'showToolbarOnNewTab',
    'cleanSearchBox',
    'newTabPainting',
    'developerMode',
    'developerModeLocalhost',
    'edgeToEdge',
    'squareSidebarCorners',
    'normalLaunch',
    'segoeUiVariable',
    'notoEmoji'
)
$script:XamlInjected = @{}
$script:Manifest = $null
# The watch loop calls Invoke-Apply every couple of seconds. These caches keep
# the expensive one-time syncs (process scans, disk walks) out of that hot path.
$script:AgentKnownRunning = $false
$script:LastAgentCheck = [datetime]::MinValue
$script:LastWatcherCheck = [datetime]::MinValue

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
    $state['sidebarGrainAmount'] = 50
    $state['sidebarGrainSoftness'] = 0
    $state['newTabPaintingTint'] = 67
    $state['materialTransparency'] = 100
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
        if ($null -ne $saved) { $state[$patch.key] = $saved }
    }
    if ($data.tweaks.sidebarGrainMaterial) {
        $state['sidebarGrainMaterial'] = $data.tweaks.sidebarGrainMaterial
    }
    if ($null -ne $data.tweaks.sidebarGrainAmount) {
        $state['sidebarGrainAmount'] = [int]$data.tweaks.sidebarGrainAmount
    }
    if ($null -ne $data.tweaks.sidebarGrainSoftness) {
        $state['sidebarGrainSoftness'] = [int]$data.tweaks.sidebarGrainSoftness
    }
    if ($null -ne $data.tweaks.newTabPaintingTint) {
        $state['newTabPaintingTint'] = [int]$data.tweaks.newTabPaintingTint
    }
    if ($null -ne $data.tweaks.materialTransparency) {
        $state['materialTransparency'] = [int]$data.tweaks.materialTransparency
    }
    if ($data.tweaks.hideToolbarMode) {
        $state['hideToolbarMode'] = $data.tweaks.hideToolbarMode
    }
    return $state
}

function Write-State {
    param([Parameter(Mandatory)][hashtable]$State)

    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null

    $tweaks = [ordered]@{}
    foreach ($patch in (Read-Manifest).patches) {
        $val = $State[$patch.key]
        if ($val -is [string] -and $val -notin @('true', 'false', '0', '1')) {
            $tweaks[$patch.key] = $val
        } elseif ($val -is [int]) {
            $tweaks[$patch.key] = $val
        } else {
            $tweaks[$patch.key] = [bool]$val
        }
    }
    if ($State.ContainsKey('sidebarGrainMaterial')) {
        $tweaks['sidebarGrainMaterial'] = $State['sidebarGrainMaterial']
    }
    if ($State.ContainsKey('sidebarGrainAmount')) {
        $tweaks['sidebarGrainAmount'] = [int]$State['sidebarGrainAmount']
    }
    if ($State.ContainsKey('sidebarGrainSoftness')) {
        $tweaks['sidebarGrainSoftness'] = [int]$State['sidebarGrainSoftness']
    }
    if ($State.ContainsKey('newTabPaintingTint')) {
        $tweaks['newTabPaintingTint'] = [int]$State['newTabPaintingTint']
    }
    if ($State.ContainsKey('materialTransparency')) {
        $tweaks['materialTransparency'] = [int]$State['materialTransparency']
    }
    if ($State.ContainsKey('hideToolbarMode')) {
        $tweaks['hideToolbarMode'] = $State['hideToolbarMode']
    }

    $document = [ordered]@{
        version = 1
        tweaks  = $tweaks
    }
    $json = ($document | ConvertTo-Json -Depth 4) + "`n"

    [System.IO.File]::WriteAllText($script:StatePath, $json, [System.Text.UTF8Encoding]::new($false))
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
    return (Test-Path -LiteralPath $script:AgentScriptPath) -or (Test-Path -LiteralPath $script:AgentPath)
}

function Get-AgentProcesses {
    $list = @(Get-Process -Name $script:AgentName -ErrorAction SilentlyContinue)
    $scriptName = 'diatweaks-agent.ps1'
    $psList = @()
    try {
        $psList = @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
            $_.CommandLine -and ($_.CommandLine -like "*$scriptName*" -or $_.CommandLine -like "*agent.ps1*")
        } | ForEach-Object {
            Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        })
    } catch {}
    return @($list + $psList)
}

function Start-Agent {
    if (-not (Test-AgentBuilt)) { return $false }
    if ((Get-AgentProcesses).Count -gt 0) { return $true }

    # The script helper runs without requiring untrusted binary permission approvals
    if (Test-Path -LiteralPath $script:AgentScriptPath) {
        Start-Process -FilePath 'powershell.exe' `
            -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script:AgentScriptPath`"" `
            -WorkingDirectory (Split-Path -Parent $script:AgentScriptPath) `
            -WindowStyle Hidden | Out-Null
        return $true
    } elseif (Test-Path -LiteralPath $script:AgentPath) {
        Start-Process -FilePath $script:AgentPath `
            -WorkingDirectory (Split-Path -Parent $script:AgentPath) `
            -WindowStyle Hidden | Out-Null
        return $true
    }
    return $false
}

function Stop-Agent {
    $processes = Get-AgentProcesses
    foreach ($process in $processes) {
        # Never take down the PowerShell process that is running this apply
        # (a watcher started from a shell could otherwise kill itself).
        if ($process.Id -eq $PID) { continue }
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

<#
.SYNOPSIS
    Small marker files recording work that only needs doing when a setting
    changes, so `watch` does not repeat it every two seconds.
#>
function Read-AppliedMarker {
    param([Parameter(Mandatory)][string]$Name)
    $path = Join-Path $script:StateDir $Name
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    try {
        return ([System.IO.File]::ReadAllText($path)).Trim()
    } catch {
        return $null
    }
}

function Write-AppliedMarker {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Value
    )
    try {
        New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $script:StateDir $Name), $Value)
    } catch {}
}

function Sync-NormalLaunch {
    param([bool]$Enabled)
    $want = if ($Enabled) { 'on' } else { 'off' }
    $applied = Read-AppliedMarker 'normalLaunch.applied'
    if ($applied -eq $want) {
        if (-not $Enabled) { return }
        # The shortcut and watcher are already in place. Only look for a dead
        # watcher occasionally: the process scan is far too costly per tick.
        if (((Get-Date) - $script:LastWatcherCheck).TotalSeconds -lt 60) { return }
    }

    $startupDir = [Environment]::GetFolderPath('Startup')
    $lnkPath = Join-Path $startupDir 'DiaTweaks.lnk'
    if ($Enabled) {
        try {
            if (-not (Test-Path -LiteralPath $lnkPath)) {
                $wsh = New-Object -ComObject WScript.Shell
                $sc = $wsh.CreateShortcut($lnkPath)
                if (Test-Path -LiteralPath $script:AgentScriptPath) {
                    $sc.TargetPath = 'powershell.exe'
                    $sc.Arguments = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script:AgentScriptPath`" -Watch"
                    $sc.WorkingDirectory = Split-Path -Parent $script:AgentScriptPath
                } else {
                    $sc.TargetPath = $script:AgentPath
                    $sc.Arguments = '--watch'
                    $sc.WorkingDirectory = Split-Path -Parent $script:AgentPath
                }
                $sc.Description = 'Keep DiaTweaks active whenever Dia opens'
                $sc.Save()
            }
        } catch {}

        # One pass over the process list, not one CIM query per candidate.
        $watcherRunning = @(Get-Process -Name 'diatweaks-agent' -ErrorAction SilentlyContinue).Count -gt 0
        if (-not $watcherRunning) {
            try {
                $watcherRunning = @(Get-CimInstance Win32_Process -Filter "Name = 'powershell.exe'" -ErrorAction Stop | Where-Object {
                    $_.CommandLine -and $_.CommandLine -like '*diatweaks-agent.ps1*' -and
                    $_.CommandLine -match '(-Watch|--watch)'
                }).Count -gt 0
            } catch {
                $watcherRunning = $false
            }
        }

        if (-not $watcherRunning) {
            if (Test-Path -LiteralPath $script:AgentScriptPath) {
                Start-Process -FilePath 'powershell.exe' `
                    -ArgumentList "-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File `"$script:AgentScriptPath`" -Watch" `
                    -WorkingDirectory (Split-Path -Parent $script:AgentScriptPath) `
                    -WindowStyle Hidden | Out-Null
            } elseif (Test-Path -LiteralPath $script:AgentPath) {
                Start-Process -FilePath $script:AgentPath -ArgumentList '--watch' `
                    -WorkingDirectory (Split-Path -Parent $script:AgentPath) `
                    -WindowStyle Hidden | Out-Null
            }
        }
        $script:LastWatcherCheck = Get-Date
        Write-AppliedMarker 'normalLaunch.applied' $want
    } else {
        if (Test-Path -LiteralPath $lnkPath) {
            Remove-Item -LiteralPath $lnkPath -Force -ErrorAction SilentlyContinue
        }
        Write-AppliedMarker 'normalLaunch.applied' $want
    }
}

function Sync-SegoeUiVariable {
    param([bool]$Enabled)
    $want = if ($Enabled) { 'on' } else { 'off' }
    # This walks Dia's whole profile and rewrites files. Only do it when the
    # setting actually changed; running it from the watch loop churned every
    # Preference file twice a second.
    if ((Read-AppliedMarker 'segoeUiVariable.applied') -eq $want) { return }

    $base = Join-Path $env:LOCALAPPDATA 'Packages'
    $diaPkg = Get-ChildItem -LiteralPath $base -Directory -Filter '*TheBrowserCompany.Dia*' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $diaPkg) { return }
    $userData = Join-Path $diaPkg.FullName 'LocalCache\Local\Dia\User Data'
    if (-not (Test-Path -LiteralPath $userData)) { return }

    $desired = "Segoe UI Variable"
    $prefFiles = @(Get-ChildItem -LiteralPath $userData -Filter "Preferences" -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($file in $prefFiles) {
        if ($file.FullName -like "*Extensions*") { continue }
        try {
            $json = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            if (-not $json.webkit) { $json | Add-Member -MemberType NoteProperty -Name 'webkit' -Value ([pscustomobject]@{}) }
            if (-not $json.webkit.webprefs) { $json.webkit | Add-Member -MemberType NoteProperty -Name 'webprefs' -Value ([pscustomobject]@{}) }
            if (-not $json.webkit.webprefs.fonts) { $json.webkit.webprefs | Add-Member -MemberType NoteProperty -Name 'fonts' -Value ([pscustomobject]@{}) }
            if (-not $json.webkit.webprefs.fonts.sansserif) { $json.webkit.webprefs.fonts | Add-Member -MemberType NoteProperty -Name 'sansserif' -Value ([pscustomobject]@{}) }
            if (-not $json.webkit.webprefs.fonts.standard) { $json.webkit.webprefs.fonts | Add-Member -MemberType NoteProperty -Name 'standard' -Value ([pscustomobject]@{}) }

            $changed = $false
            if ($Enabled) {
                if ($json.webkit.webprefs.fonts.sansserif.Zyyy -ne $desired) {
                    $json.webkit.webprefs.fonts.sansserif.Zyyy = $desired
                    $changed = $true
                }
                if ($json.webkit.webprefs.fonts.standard.Zyyy -ne $desired) {
                    $json.webkit.webprefs.fonts.standard.Zyyy = $desired
                    $changed = $true
                }
            } else {
                if ($json.webkit.webprefs.fonts.sansserif.Zyyy -in "Segoe UI Variable", "Segoe UI Variable Text") {
                    $json.webkit.webprefs.fonts.sansserif.Zyyy = ""
                    $changed = $true
                }
                if ($json.webkit.webprefs.fonts.standard.Zyyy -in "Segoe UI Variable", "Segoe UI Variable Text") {
                    $json.webkit.webprefs.fonts.standard.Zyyy = ""
                    $changed = $true
                }
            }
            if ($changed) {
                $out = ($json | ConvertTo-Json -Depth 32)
                [System.IO.File]::WriteAllText($file.FullName, $out, [System.Text.UTF8Encoding]::new($false))
            }
        } catch {}
    }

    # Also patch style-guide.css in HomeSurface\Shared so the New Tab page uses Segoe UI Variable
    $cssFiles = @(Get-ChildItem -LiteralPath $userData -Filter "style-guide.css" -Recurse -File -ErrorAction SilentlyContinue)
    foreach ($cssFile in $cssFiles) {
        try {
            $cssContent = [System.IO.File]::ReadAllText($cssFile.FullName, [System.Text.Encoding]::UTF8)
            if ($Enabled) {
                if ($cssContent -notmatch '"Segoe UI Variable"') {
                    $cssContent = $cssContent -replace '(--home-font-body:\s*)', '$1"Segoe UI Variable", '
                    [System.IO.File]::WriteAllText($cssFile.FullName, $cssContent, [System.Text.Encoding]::UTF8)
                }
            } else {
                if ($cssContent -match '"Segoe UI Variable",\s*') {
                    $cssContent = $cssContent -replace '"Segoe UI Variable",\s*', ''
                    [System.IO.File]::WriteAllText($cssFile.FullName, $cssContent, [System.Text.Encoding]::UTF8)
                }
            }
        } catch {}
    }

    Write-AppliedMarker 'segoeUiVariable.applied' $want
}

# --- The XAML tweaks -------------------------------------------------------

function Test-XamlNeeded {
    param([hashtable]$State)
    foreach ($key in $script:XamlKeys) {
        if ($State[$key]) { return $true }
    }
    return $false
}

<#
.SYNOPSIS
    Writes the small INI that diatweaks-xaml.dll polls.
#>
function Write-XamlConfig {
    param([hashtable]$State)

    New-Item -ItemType Directory -Force -Path $script:StateDir | Out-Null

    $repoChangelog = Join-Path $script:DiaTweaksRoot 'changelog.json'
    $stateChangelog = Join-Path $script:StateDir 'changelog.json'
    if (Test-Path -LiteralPath $repoChangelog) {
        $needsCopy = -not (Test-Path -LiteralPath $stateChangelog)
        if (-not $needsCopy) {
            try {
                $needsCopy = (Get-Item -LiteralPath $repoChangelog).LastWriteTimeUtc -gt
                    (Get-Item -LiteralPath $stateChangelog).LastWriteTimeUtc
            } catch {
                $needsCopy = $true
            }
        }
        if ($needsCopy) {
            Copy-Item -LiteralPath $repoChangelog -Destination $stateChangelog -Force -ErrorAction SilentlyContinue
        }
    }

    $lines = @('[tweaks]')
    foreach ($key in $script:XamlKeys) {
        $value = if ($State[$key]) {
            if ($State[$key] -is [int]) { $State[$key] }
            elseif ($State[$key] -is [string] -and $State[$key] -match '^\d+$') { [int]$State[$key] }
            else { 1 }
        } else { 0 }
        $lines += "$key=$value"
    }

    $grainMat = if ($State['sidebarGrainMaterial']) { $State['sidebarGrainMaterial'] } else { 'sand' }
    $lines += "sidebarGrainMaterial=$grainMat"

    $grainAmount = if ($null -ne $State['sidebarGrainAmount']) { $State['sidebarGrainAmount'] } else { 50 }
    $lines += "sidebarGrainAmount=$grainAmount"

    $grainSoftness = if ($null -ne $State['sidebarGrainSoftness']) { $State['sidebarGrainSoftness'] } else { 0 }
    $lines += "sidebarGrainSoftness=$grainSoftness"

    $winMat = if ($State['windowMaterial']) {
        if ($State['windowMaterial'] -is [string]) { $State['windowMaterial'] } else { 'mica' }
    } else { 'default' }
    $lines += "windowMaterial=$winMat"

    $tbMode = if ($State['hideToolbarMode']) { $State['hideToolbarMode'] } else { 'overlay' }
    $lines += "hideToolbarMode=$tbMode"

    $peekTrig = if ($State['peekTrigger']) { $State['peekTrigger'] } else { 'shift' }
    $lines += "peekTrigger=$peekTrig"

    $peekAnim = if ($State['peekAnimation']) { $State['peekAnimation'] } else { 'genie' }
    $lines += "peekAnimation=$peekAnim"

    $profileEmojiText = if ($State['profileEmojiText']) { $State['profileEmojiText'] } else { '🦊' }
    $lines += "profileEmojiText=$profileEmojiText"

    $notoFont = if ($State['notoEmojiFont']) { $State['notoEmojiFont'] } else { 'notoColor' }
    $lines += "notoEmojiFont=$notoFont"

    $paintingTint = if ($null -ne $State['newTabPaintingTint']) { $State['newTabPaintingTint'] } else { 67 }
    $lines += "newTabPaintingTint=$paintingTint"

    $materialTransparency = if ($null -ne $State['materialTransparency']) { [int]$State['materialTransparency'] } else { 100 }
    $lines += "materialTransparency=$materialTransparency"

    $configPath = Join-Path $script:StateDir 'xaml.cfg'
    $existingEmojis = ""
    if (Test-Path -LiteralPath $configPath) {
        try {
            $content = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::Unicode)
            if ($content -match '(?ms)(\[profileEmojis\].*?)(?:\[|\z)') {
                $existingEmojis = "`n" + $matches[1].Trim() + "`n"
            }
        } catch {}
    }
    if ($State['profileEmojis'] -is [hashtable]) {
        $emojiLines = @("[profileEmojis]")
        foreach ($k in $State['profileEmojis'].Keys) {
            $emojiLines += "$k=$($State['profileEmojis'][$k])"
        }
        $existingEmojis = "`n" + ($emojiLines -join "`n") + "`n"
    }

    $text = ($lines -join "`n") + "`n" + $existingEmojis
    # The helper polls this file. Rewriting it on every watch tick made Dia see
    # a half-written INI for a moment; only replace it when the text changed,
    # and swap it into place atomically.
    $current = $null
    if (Test-Path -LiteralPath $configPath) {
        try { $current = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::Unicode) } catch {}
    }
    if ($current -ne $text) {
        $tempPath = "$configPath.tmp"
        [System.IO.File]::WriteAllText($tempPath, $text, [System.Text.Encoding]::Unicode)
        Move-Item -LiteralPath $tempPath -Destination $configPath -Force
    }
}

<#
.SYNOPSIS
    Makes sure the XAML helper is loaded in Dia and its config is current.
#>
function Test-XamlModuleLoaded {
    param([Parameter(Mandatory)][int]$ProcessId)
    try {
        $process = Get-Process -Id $ProcessId -ErrorAction Stop
        return @($process.Modules | Where-Object { $_.ModuleName -eq 'diatweaks-xaml.dll' }).Count -gt 0
    } catch {
        return $false
    }
}

<#
.SYNOPSIS
    Makes sure the XAML helper is loaded in Dia and its config is current.
#>
function Invoke-XamlApply {
    param([hashtable]$State, [switch]$Force)

    $xamlActive = Test-XamlNeeded -State $State
    Write-XamlConfig -State $State

    $windows = @(Get-DiaWindows)
    if ($windows.Count -eq 0) {
        return [pscustomobject]@{ Active = $xamlActive; Loaded = $false; Message = $null }
    }

    $processId = $windows[0].ProcessId
    $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
    $cacheKey = if ($process) { "${processId}:$($process.StartTime.Ticks)" } else { "$processId" }
    $alreadyLoaded = $script:XamlInjected.ContainsKey($cacheKey)

    if (-not (Test-Path -LiteralPath $script:XamlDllPath)) {
        return [pscustomobject]@{
            Active  = $xamlActive
            Loaded  = $false
            Message = "The XAML tweaks need bin\diatweaks-xaml.dll. Run scripts\build.ps1."
        }
    }

    # Another invocation (or the launch watcher) may already have loaded the
    # helper. Checking the module list costs far less than re-injecting, and it
    # keeps `apply` snappy in a fresh PowerShell process.
    if (-not $alreadyLoaded -and (Test-XamlModuleLoaded -ProcessId $processId)) {
        $alreadyLoaded = $true
        $script:XamlInjected[$cacheKey] = $true
    }
    if ($alreadyLoaded) {
        return [pscustomobject]@{ Active = $xamlActive; Loaded = $true; Message = $null }
    }

    $injected = $false
    # First: In-process injection directly in PowerShell (no external .exe, eliminates Security Warning dialogs)
    if (Get-Command 'Invoke-DiaTweaksInjection' -ErrorAction SilentlyContinue) {
        $injected = Invoke-DiaTweaksInjection -ProcessId $processId -DllPath $script:XamlDllPath
    }

    # Second: Fallback to diatweaks-inject.exe if in-process injection was not available
    if (-not $injected -and (Test-Path -LiteralPath $script:InjectorPath)) {
        $env:SEE_MASK_NOZONECHECKS = '1'
        try {
            Unblock-File -LiteralPath $script:InjectorPath -ErrorAction SilentlyContinue
            Unblock-File -LiteralPath $script:XamlDllPath -ErrorAction SilentlyContinue
        } catch {}

        $output = & $script:InjectorPath $processId $script:XamlDllPath 2>&1
        $code = $LASTEXITCODE
        $injected = ($code -eq 0)
    }

    if (-not $injected) {
        return [pscustomobject]@{
            Active  = $xamlActive
            Loaded  = $false
            Message = "Couldn't load the XAML helper into Dia."
        }
    }

    $script:XamlInjected[$cacheKey] = $true
    # Give the first timer round time to find Dia's XAML tree and apply.
    Start-Sleep -Milliseconds 450
    return [pscustomobject]@{ Active = $xamlActive; Loaded = $true; Message = $null }
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
    $square = [bool]$state['squareSidebarCorners']
    $winMat = if ($state['windowMaterial']) {
        if ($state['windowMaterial'] -is [string]) { $state['windowMaterial'] } else { 'mica' }
    } else { 'default' }

    $warnings = [System.Collections.Generic.List[string]]::new()
    if (-not (Test-WindowsEleven)) {
        $warnings.Add('These window tweaks need Windows 11; this PC is on an older Windows.')
    }

    $applied = 0
    $borderFailures = 0
    $materialFailures = 0
    $borderCode = 0
    $materialCode = 0
    foreach ($window in $windows) {
        $result = Set-TweakedWindow -Handle $window.Handle -EdgeToEdge $edge -SquareCorners $square -WindowMaterial $winMat
        if ($edge -and $result.BorderHr -ne 0) {
            $borderFailures++
            $borderCode = $result.BorderHr
        }
        if ($winMat -ne 'default' -and $result.MaterialHr -ne 0) {
            $materialFailures++
            $materialCode = $result.MaterialHr
        }
        $applied++
    }

    if ($borderFailures -gt 0) {
        $warnings.Add("Windows refused the border change on $borderFailures window(s) ($(Format-HResult $borderCode)).")
    }
    if ($materialFailures -gt 0) {
        $warnings.Add("Windows refused the window material change on $materialFailures window(s) ($(Format-HResult $materialCode)).")
    }

    $keepAwake = [bool]$state['keepWindowsActive']
    $agentBuilt = Test-AgentBuilt
    $agentRunning = $false

    # Looking for the helper means a WMI process scan. In a watching loop that
    # used to happen twice a second; check only when the answer can have
    # changed, or every so often in case the helper died.
    $now = Get-Date
    if ($keepAwake -and $windows.Count -gt 0) {
        if ($agentBuilt) {
            $agentRunning = $true
            $due = -not $script:AgentKnownRunning -or
                ($now - $script:LastAgentCheck).TotalSeconds -ge 30
            if ($due) {
                $script:LastAgentCheck = $now
                [void](Start-Agent)
                $script:AgentKnownRunning = $true
            }
        } else {
            $warnings.Add("Keep the display awake is on, but the helper isn't built. Run scripts\build.ps1.")
        }
    } else {
        $due = $script:AgentKnownRunning -or ($now - $script:LastAgentCheck).TotalSeconds -ge 15
        if ($due) {
            $script:LastAgentCheck = $now
            Stop-Agent
            $script:AgentKnownRunning = $false
        }
    }

    $xaml = Invoke-XamlApply -State $state
    if ($xaml.Message) { $warnings.Add($xaml.Message) }

    Sync-NormalLaunch -Enabled ([bool]$state['normalLaunch'])
    Sync-SegoeUiVariable -Enabled ([bool]$state['segoeUiVariable'])

    return [pscustomobject]@{
        Windows      = $applied
        EdgeToEdge   = $edge
        Square       = $square
        KeepAwake    = $keepAwake
        AgentBuilt   = $agentBuilt
        AgentRunning = $agentRunning
        XamlActive   = $xaml.Active
        XamlLoaded   = $xaml.Loaded
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

    # A loaded XAML helper watches this file and restores its own changes.
    $off = @{}
    foreach ($patch in (Read-Manifest).patches) { $off[$patch.key] = $false }
    [void](Invoke-XamlApply -State $off)

    return $restored
}
