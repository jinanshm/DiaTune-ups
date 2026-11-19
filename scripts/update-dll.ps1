# update-dll.ps1 - Closes Dia, replaces diatweaks-xaml.dll with diatweaks-xaml-test.dll, and relaunches Dia
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'bin\diatweaks-xaml-test.dll'
$dest = Join-Path $root 'bin\diatweaks-xaml.dll'

Write-Host "Stopping Dia to release file lock..."
Get-Process -Name 'diatweaks-watch', 'diatweaks-agent', 'Dia' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Milliseconds 600

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$updated = $false
while ($sw.Elapsed.TotalSeconds -lt 30) {
    try {
        $stream = [System.IO.File]::Open($dest, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        $stream.Close()
        Copy-Item -LiteralPath $source -Destination $dest -Force
        Write-Host "SUCCESS: diatweaks-xaml.dll has been updated with the latest build!"
        $updated = $true
        break
    } catch {
        Start-Sleep -Milliseconds 500
    }
}

if (-not $updated) {
    Write-Warning "Could not copy to diatweaks-xaml.dll within 30 seconds."
    exit 1
}

Write-Host "Launching Dia with updated tweaks (no console window)..."
$psExe = if (Get-Command 'pwsh' -ErrorAction SilentlyContinue) { 'pwsh' } else { 'powershell' }
& $psExe -NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "$root\cli\diatweaks.ps1" launch
