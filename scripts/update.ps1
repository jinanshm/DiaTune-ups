# update.ps1 - Updates DiaTweaks (Dia Tune-ups) from GitHub repository
# Runs with no visible console window: any child PowerShell it spawns is hidden.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Write-Host "Checking for DiaTweaks updates..." -ForegroundColor Cyan

# Check if git is available
$git = Get-Command 'git' -ErrorAction SilentlyContinue
if (-not $git) {
    Write-Warning "git is not installed on this system. Please update files manually or install git."
    exit 1
}

# Pull latest changes
Push-Location $root
try {
    Write-Host "Pulling latest changes from origin..."
    & git pull origin master
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "git pull returned exit code $LASTEXITCODE"
    } else {
        Write-Host "Repository updated successfully." -ForegroundColor Green
    }
} finally {
    Pop-Location
}

# Synchronize changelog.json
$changelog = Join-Path $root 'changelog.json'
$stateDir = Join-Path $env:LOCALAPPDATA 'DiaTweaks'
if (Test-Path -LiteralPath $changelog) {
    if (-not (Test-Path -LiteralPath $stateDir)) {
        New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
    }
    Copy-Item -LiteralPath $changelog -Destination (Join-Path $stateDir 'changelog.json') -Force
}

# Rebuild binaries (hidden, so no extra console window opens)
Write-Host "Rebuilding binaries..."
$psExe = if (Get-Command 'pwsh' -ErrorAction SilentlyContinue) { 'pwsh' } else { 'powershell' }
& $psExe -NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File (Join-Path $root 'scripts\build.ps1') -SkipAgent

Write-Host "`nDia Tune-ups update complete! Restart Dia to load updated tweaks." -ForegroundColor Green
