# build.ps1 - builds the keep-awake helper.
#
# Uses the Visual Studio C++ toolchain when it is installed, and falls back to
# MinGW's g++ otherwise. The result is bin\diatweaks-agent.exe.

[CmdletBinding()]
param(
    [switch]$SkipAgent
)

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
Get-Process -Name 'diatweaks-agent', 'Dia' -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 300

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

if (-not $SkipAgent) {
    if ($vcvars) {
        Write-Host "Using $vcvars"

        $batch = Join-Path $env:TEMP 'diatweaks-build.bat'
        $lines = @(
            '@echo off',
            'setlocal',
            "call `"$vcvars`" >nul",
            'if errorlevel 1 exit /b 1',
            "cd /d `"$objDir`"",
            "cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE /Fo`"$objDir/`" /Fe:`"$exe`" `"$source`" user32.lib shell32.lib ole32.lib"
        )
        [System.IO.File]::WriteAllLines($batch, $lines, [System.Text.UTF8Encoding]::new($false))

        & cmd.exe /c $batch
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "diatweaks-agent.exe build failed ($LASTEXITCODE); agent may be running."
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
            Write-Warning "g++ failed with exit code $LASTEXITCODE."
        }
    }

    if (Test-Path -LiteralPath $exe) {
        # Intermediate files are not useful at runtime.
        Get-ChildItem -LiteralPath $objDir -Filter '*.obj' -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue

        $size = [Math]::Round((Get-Item -LiteralPath $exe).Length / 1KB, 1)
        Write-Host "Built $exe ($size KB)" -ForegroundColor Green
    }
}

# --- The XAML tweak DLL and injector ---------------------------------------

function Find-Projection {
    $candidates = @()
    if ($env:DIA_TWEAKS_PROJECTION) { $candidates += $env:DIA_TWEAKS_PROJECTION }
    $candidates += (Join-Path $root 'src\xaml\projection')
    $candidates += 'C:\Users\Jinansh\dia-win\cpx'
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'winrt\Microsoft.UI.Xaml.h'))) {
            return $candidate
        }
    }
    return $null
}

function Find-CppWinrtSdk {
    $base = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits\10\Include'
    if (-not (Test-Path -LiteralPath $base)) { return $null }
    foreach ($version in (Get-ChildItem -LiteralPath $base -Directory | Sort-Object Name -Descending)) {
        $candidate = Join-Path $version.FullName 'cppwinrt'
        if (Test-Path -LiteralPath (Join-Path $candidate 'winrt\base.h')) { return $candidate }
    }
    return $null
}

$xamlSource = Join-Path $root 'src\xaml\diatweaks-xaml.cpp'
$peekSource = Join-Path $root 'src\xaml\peek.cpp'
$devModeSource = Join-Path $root 'src\xaml\devmode.cpp'
$cdpSource = Join-Path $root 'src\xaml\cdp.cpp'
$xamlDef = Join-Path $root 'src\xaml\diatweaks-xaml.def'
$injectSource = Join-Path $root 'src\injector\diatweaks-inject.cpp'
$dll = Join-Path $outDir 'diatweaks-xaml.dll'
$injector = Join-Path $outDir 'diatweaks-inject.exe'

if (-not $vcvars) {
    Write-Warning 'Visual Studio C++ tools were not found; skipping diatweaks-xaml.dll and the injector.'
} else {
    $projection = Find-Projection
    $cppwinrt = Find-CppWinrtSdk
    if (-not $projection) {
        throw @'
Couldn't find the WinUI projection headers.
Set DIA_TWEAKS_PROJECTION to a folder containing winrt\Microsoft.UI.Xaml.h,
or put them in src\xaml\projection.
'@
    }
    if (-not $cppwinrt) {
        throw 'Couldn''t find the Windows SDK cppwinrt headers (Windows Kits\10\Include\*\cppwinrt).'
    }

    Write-Host "XAML projection: $projection"
    Write-Host "Windows SDK: $cppwinrt"

    $agentScript = Join-Path $root 'cli\lib\agent.ps1'
    $binAgentScript = Join-Path $outDir 'diatweaks-agent.ps1'
    if (Test-Path -LiteralPath $agentScript) {
        Copy-Item -LiteralPath $agentScript -Destination $binAgentScript -Force
    }

    $targetDll = $dll
    $isLocked = $false
    if (Test-Path -LiteralPath $dll) {
        try {
            $stream = [System.IO.File]::Open($dll, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            $stream.Close()
        } catch {
            $isLocked = $true
            $targetDll = Join-Path $outDir 'diatweaks-xaml-test.dll'
        }
    }

    $batch = Join-Path $env:TEMP 'diatweaks-xaml-build.bat'
    $lines = @(
        '@echo off',
        'setlocal',
        "call `"$vcvars`" >nul",
        'if errorlevel 1 exit /b 1',
        "cd /d `"$objDir`"",
        "cl /nologo /LD /std:c++20 /EHsc /MT /O2 /W3 /bigobj /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /I`"$projection`" /I`"$cppwinrt`" `"$xamlSource`" `"$peekSource`" `"$devModeSource`" `"$cdpSource`" /Fe:`"$targetDll`" /link /DEF:`"$xamlDef`" /SUBSYSTEM:WINDOWS ole32.lib oleaut32.lib runtimeobject.lib windowsapp.lib user32.lib dwmapi.lib shell32.lib gdi32.lib gdiplus.lib d3d11.lib dcomp.lib d2d1.lib comctl32.lib msimg32.lib dxgi.lib windowscodecs.lib winhttp.lib ws2_32.lib",
        'if errorlevel 1 exit /b 3',
        "cl /nologo /std:c++17 /EHsc /MT /O2 /W3 /DUNICODE /D_UNICODE `"$injectSource`" /Fe:`"$injector`"",
        'exit /b %ERRORLEVEL%'
    )
    [System.IO.File]::WriteAllLines($batch, $lines, [System.Text.UTF8Encoding]::new($false))

    Get-Process -Name 'Dia' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) {
        throw @"
The XAML build failed (exit code $LASTEXITCODE). If Dia is running, close it so
Windows releases bin\diatweaks-xaml.dll, then run scripts\build.ps1 again.
"@
    }

    if ($isLocked) {
        try {
            Copy-Item -LiteralPath $targetDll -Destination $dll -Force
            Remove-Item -LiteralPath $targetDll -Force -ErrorAction SilentlyContinue
            $isLocked = $false
        } catch {
            Write-Warning "Dia is currently running and locking bin\diatweaks-xaml.dll. Built bin\diatweaks-xaml-test.dll. Close Dia and run scripts\update-dll.ps1 to update diatweaks-xaml.dll."
        }
    }

    $finalDll = if ($isLocked) { $targetDll } else { $dll }
    foreach ($artifact in @($finalDll, $injector)) {
        if (-not (Test-Path -LiteralPath $artifact)) {
            throw "The build finished but $artifact is missing."
        }
    }

    Get-ChildItem -LiteralPath $objDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.obj', '.exp', '.lib') } |
        Remove-Item -Force -ErrorAction SilentlyContinue

    $changelogSource = Join-Path $root 'changelog.json'
    if (Test-Path -LiteralPath $changelogSource) {
        Copy-Item -LiteralPath $changelogSource -Destination (Join-Path $outDir 'changelog.json') -Force
        $stateDir = Join-Path $env:LOCALAPPDATA 'DiaTweaks'
        if (-not (Test-Path -LiteralPath $stateDir)) {
            New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
        }
        Copy-Item -LiteralPath $changelogSource -Destination (Join-Path $stateDir 'changelog.json') -Force
    }

    foreach ($artifact in @($finalDll, $injector)) {
        $size = [Math]::Round((Get-Item -LiteralPath $artifact).Length / 1KB, 1)
        Write-Host "Built $artifact ($size KB)" -ForegroundColor Green
    }
}
