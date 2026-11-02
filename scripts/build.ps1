# build.ps1 - builds the keep-awake helper.
#
# Uses the Visual Studio C++ toolchain when it is installed, and falls back to
# MinGW's g++ otherwise. The result is bin\diatweaks-agent.exe.

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'src\agent\diatweaks-agent.cpp'
$outDir = Join-Path $root 'bin'
$objDir = Join-Path $outDir 'obj'
$exe = Join-Path $outDir 'diatweaks-agent.exe'

if (-not (Test-Path -LiteralPath $source)) {
    throw "Missing source: $source"
}

New-Item -ItemType Directory -Force -Path $outDir, $objDir | Out-Null

# A running helper locks the old exe on Windows.
Get-Process -Name 'diatweaks-agent' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 150

function Find-VcVars {
    $programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
    $programFiles = [Environment]::GetFolderPath('ProgramFiles')

    $vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $install = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($install) {
            $candidate = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    $editions = @('Community', 'Professional', 'Enterprise', 'BuildTools')
    $versions = @('2022', '2019', '2017')
    foreach ($version in $versions) {
        foreach ($edition in $editions) {
            $candidate = Join-Path $programFiles "Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path -LiteralPath $candidate) { return $candidate }
            $candidate = Join-Path $programFilesX86 "Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }

    # Preview / Insider installs use a version-numbered folder.
    $known = @(
        (Join-Path $programFilesX86 'Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'),
        (Join-Path $programFilesX86 'Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat')
    )
    foreach ($candidate in $known) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }

    return $null
}

$vcvars = Find-VcVars

if ($vcvars) {
    Write-Host "Using $vcvars"

    $batch = Join-Path $env:TEMP 'diatweaks-build.bat'
    $lines = @(
        '@echo off',
        'setlocal',
        "call `"$vcvars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$objDir`"",
        "cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /Fe:`"$exe`" `"$source`""
    )
    [System.IO.File]::WriteAllLines($batch, $lines, [System.Text.UTF8Encoding]::new($false))

    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) {
        throw "cl.exe failed with exit code $LASTEXITCODE."
    }
} else {
    $gpp = Get-Command 'g++' -ErrorAction SilentlyContinue
    if (-not $gpp) {
        throw @'
Couldn't find a C++ compiler.
Install "Desktop development with C++" from the Visual Studio Build Tools:
https://visualstudio.microsoft.com/visual-cpp-build-tools/
Then run scripts\build.ps1 again.
'@
    }

    Write-Host "Using $($gpp.Source)"
    & $gpp.Source -std=c++17 -O2 -static -municode -o $exe $source
    if ($LASTEXITCODE -ne 0) {
        throw "g++ failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $exe)) {
    throw "The build finished but $exe is missing."
}

# Intermediate files are not useful at runtime.
Get-ChildItem -LiteralPath $objDir -Filter '*.obj' -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

$size = [Math]::Round((Get-Item -LiteralPath $exe).Length / 1KB, 1)
Write-Host "Built $exe ($size KB)" -ForegroundColor Green
