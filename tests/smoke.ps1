# smoke.ps1 - checks the manifest, the CLI files and tweak name matching.
# Run from anywhere: powershell -ExecutionPolicy Bypass -File tests\smoke.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$script:checks = 0

function Assert-True {
    param([bool]$Condition, [string]$Message)
    $script:checks++
    if (-not $Condition) {
        throw "FAIL: $Message"
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Message)
    $script:checks++
    if ($Expected -ne $Actual) {
        throw "FAIL: $Message (expected '$Expected', got '$Actual')"
    }
}

# --- Files and syntax ------------------------------------------------------

$required = @(
    'patches.json',
    'diatweaks.cmd',
    'cli\diatweaks.ps1',
    'cli\lib\windows.ps1',
    'cli\lib\dia.ps1',
    'cli\lib\state.ps1',
    'src\agent\diatweaks-agent.cpp',
    'src\xaml\diatweaks-xaml.cpp',
    'src\xaml\hook.asm',
    'src\xaml\qifuncs.h',
    'src\injector\diatweaks-inject.cpp',
    'scripts\build.ps1'
)

foreach ($relative in $required) {
    Assert-True (Test-Path -LiteralPath (Join-Path $root $relative)) "missing $relative"
}

foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -Filter '*.ps1' -File) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors)
    Assert-Equal 0 $errors.Count "syntax errors in $($file.FullName)"
}

# --- Manifest --------------------------------------------------------------

. (Join-Path $root 'cli\lib\windows.ps1')
. (Join-Path $root 'cli\lib\dia.ps1')
. (Join-Path $root 'cli\lib\state.ps1')

$manifest = Read-Manifest
Assert-Equal 1 $manifest.version 'manifest version'
Assert-True ($manifest.patches.Count -ge 10) 'manifest lists the expected tweaks'

$keys = @($manifest.patches | ForEach-Object { $_.key })
Assert-Equal $keys.Count (@($keys | Select-Object -Unique).Count) 'tweak keys are unique'

$orders = @($manifest.patches | ForEach-Object { $_.order })
Assert-Equal $orders.Count (@($orders | Select-Object -Unique).Count) 'tweak orders are unique'

foreach ($patch in $manifest.patches) {
    Assert-True ($patch.title.Length -gt 0) "$($patch.key) has a title"
    Assert-True ($patch.detail.Length -gt 0) "$($patch.key) has a detail"
    foreach ($link in @($patch.requires, $patch.disabledBy)) {
        if ($link) {
            Assert-True ($keys -contains $link) "$($patch.key) links to a known tweak: $link"
        }
    }

    if ($patch.supported) {
        Assert-True ($patch.mechanism -in @('border', 'corners', 'awake', 'xaml')) `
            "$($patch.key) names a Windows mechanism"
    } else {
        Assert-True (-not $patch.mechanism) "$($patch.key) has no mechanism because it is unavailable"
    }
}

$supported = @($manifest.patches | Where-Object { $_.supported } | ForEach-Object { $_.key })
Assert-True ($supported.Count -ge 20) 'expanded tweaks are available on Windows'
foreach ($key in @('edgeToEdge', 'squareSidebarCorners', 'keepWindowsActive',
        'hideToolbar', 'toolbarColor', 'revealToolbarOnHover', 'hideSidebarButton', 'hideChatButton', 'showAddressField',
        'showToolbarWithSidebar', 'showToolbarOnNewTab', 'hideTrafficLights',
        'hidePersonalizeButton', 'hideUpgradeBanner', 'hideTabSearchButton',
        'profileEmoji', 'sidebarGrain', 'pinnedExtensions', 'hideLoadingAnimation',
        'hidePinnedTabBorder', 'hideTabHoverCard', 'translucentSidebar', 'windowMaterial',
        'dontPopUpDownloads', 'cleanSearchBox', 'hideLinkAddress', 'notoEmoji')) {
    Assert-True ($supported -contains $key) "$key is supported"
}

# --- Name matching ---------------------------------------------------------

$found = Find-Tweak $manifest 'squareSidebarCorners'
Assert-Equal 'squareSidebarCorners' $found.key 'keys match'

$found = Find-Tweak $manifest 'Square window corners'
Assert-Equal 'squareSidebarCorners' $found.key 'titles match'

$found = Find-Tweak $manifest 'edge-to-edge window'
Assert-Equal 'edgeToEdge' $found.key 'punctuation and case are ignored'

$found = Find-Tweak $manifest 'Keep the display awake'
Assert-Equal 'keepWindowsActive' $found.key 'adapted title matches'

$threw = $false
try { Find-Tweak $manifest 'hideToolbarz' | Out-Null } catch { $threw = $true }
Assert-True $threw 'unknown names are rejected'

# --- State defaults --------------------------------------------------------

$savedStatePath = $script:StatePath
$savedStateDir = $script:StateDir
$tempStateDir = Join-Path $env:TEMP "diatweaks-smoke-$PID"
$script:StateDir = $tempStateDir
$script:StatePath = Join-Path $tempStateDir 'state.json'
try {
    $state = Read-State
    Assert-True (-not [bool]$state['edgeToEdge']) 'edgeToEdge defaults off'
    Assert-True (-not [bool]$state['squareSidebarCorners']) 'squareSidebarCorners defaults off'
    Assert-True (-not [bool]$state['keepWindowsActive']) 'keepWindowsActive defaults off'

    $state['keepWindowsActive'] = $true
    Write-State -State $state
    $reloaded = Read-State
    Assert-True ([bool]$reloaded['keepWindowsActive']) 'state round-trips through state.json'

    Assert-True (-not [bool]$state['hideToolbar']) 'hideToolbar defaults off'
    $state['hideToolbar'] = $true
    Write-XamlConfig -State $state
    $ini = Get-Content -LiteralPath (Join-Path $script:StateDir 'xaml.cfg') -Raw
    Assert-True ($ini.Contains('hideToolbar=1')) 'xaml.cfg carries enabled XAML tweaks'
    Assert-True ($ini.Contains('hideChatButton=0')) 'xaml.cfg carries disabled XAML tweaks'
} finally {
    Remove-Item -LiteralPath $tempStateDir -Recurse -Force -ErrorAction SilentlyContinue
    $script:StatePath = $savedStatePath
    $script:StateDir = $savedStateDir
}

# --- The CLI itself --------------------------------------------------------

$powershell = if (Get-Command pwsh.exe -ErrorAction SilentlyContinue) { 'pwsh.exe' } else { 'powershell.exe' }
$cli = Join-Path $root 'cli\diatweaks.ps1'
$output = & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli 'list' 2>&1
Assert-Equal 0 $LASTEXITCODE 'diatweaks list exits successfully'
$text = $output -join "`n"
foreach ($key in $keys) {
    Assert-True ($text.Contains($key)) "list shows $key"
}
Assert-True ($text.Contains('n/a')) 'list marks unavailable tweaks'

$output = & $powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File $cli 'nonsense' 2>&1
Assert-Equal 2 $LASTEXITCODE 'unknown commands exit with 2'

# --- Docs ------------------------------------------------------------------

$readme = Get-Content -LiteralPath (Join-Path $root 'README.md') -Raw -Encoding UTF8
foreach ($key in $keys) {
    Assert-True ($readme.Contains($key)) "README documents $key"
}

if (-not (Test-Path -LiteralPath (Join-Path $root 'bin\diatweaks-agent.ps1')) -and
    -not (Test-Path -LiteralPath (Join-Path $root 'bin\diatweaks-watch.exe'))) {
    Write-Warning 'bin\diatweaks-watch.exe is not built yet; run scripts\build.ps1.'
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'bin\diatweaks-xaml.dll'))) {
    Write-Warning 'bin\diatweaks-xaml.dll is not built yet; run scripts\build.ps1.'
}

Write-Host "smoke: $script:checks checks passed" -ForegroundColor Green
