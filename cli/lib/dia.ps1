# dia.ps1 - finding the installed Dia package, its processes and its windows.

$script:DiaPackageName = 'TheBrowserCompany.Dia'
$script:DiaWindowClass = 'WinUIDesktopWin32WindowClass'
$script:DiaPackage = $null
$script:DiaPackageLoaded = $false
$script:DiaAppId = $null

<#
.SYNOPSIS
    The current user's Dia package, or $null when Dia is not installed.
#>
function Get-DiaPackage {
    if (-not $script:DiaPackageLoaded) {
        $script:DiaPackageLoaded = $true
        try {
            $script:DiaPackage = Get-AppxPackage -Name $script:DiaPackageName -ErrorAction Stop |
                Select-Object -First 1
        } catch {
            $script:DiaPackage = $null
        }
    }
    return $script:DiaPackage
}

function Get-DiaInfo {
    $package = Get-DiaPackage
    if (-not $package) { return $null }

    $exe = Join-Path $package.InstallLocation 'Dia.exe'
    $version = $package.Version.ToString()
    if (-not $version -and (Test-Path -LiteralPath $exe)) {
        $version = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
    }

    return [pscustomobject]@{
        Name             = $package.Name
        Version          = $version
        PackageFullName  = $package.PackageFullName
        PackageFamilyName = $package.PackageFamilyName
        InstallLocation  = $package.InstallLocation
        ExePath          = $exe
    }
}

<#
.SYNOPSIS
    The Application User Model ID used to start the packaged Dia.
#>
function Get-DiaAppId {
    if ($script:DiaAppId) { return $script:DiaAppId }

    $entry = Get-StartApps -ErrorAction SilentlyContinue |
        Where-Object { $_.AppID -like "$($script:DiaPackageName)_*!*" } |
        Select-Object -First 1
    if ($entry) {
        $script:DiaAppId = $entry.AppID
        return $script:DiaAppId
    }

    $package = Get-DiaPackage
    if ($package) {
        $script:DiaAppId = "$($package.PackageFamilyName)!Dia"
        return $script:DiaAppId
    }

    return $null
}

function Get-DiaProcesses {
    return @(Get-Process -Name 'Dia' -ErrorAction SilentlyContinue)
}

function Test-DiaRunning {
    return ((Get-DiaProcesses).Count -gt 0)
}

<#
.SYNOPSIS
    Dia's visible top-level windows.

.DESCRIPTION
    Dia's browser window uses WinUIDesktopWin32WindowClass. Chromium's helper
    processes share the Dia.exe name but only own small input-sink windows, so
    matching the window class is the reliable filter. If a future Dia renames
    the class, a title-and-size fallback still finds the main window.
#>
function Get-DiaWindows {
    $processIds = @(Get-DiaProcesses | Select-Object -ExpandProperty Id)
    if ($processIds.Count -eq 0) { return @() }

    $owned = @(Get-TopLevelWindows | Where-Object { $processIds -contains $_.ProcessId })
    $main = @($owned | Where-Object { $_.ClassName -eq $script:DiaWindowClass })
    if ($main.Count -gt 0) { return $main }

    return @($owned | Where-Object {
        $_.Title -and $_.Width -ge 400 -and $_.Height -ge 300
    })
}

<#
.SYNOPSIS
    Starts Dia through its package identity.
#>
function Start-Dia {
    $appId = Get-DiaAppId
    if ($appId) {
        # The shell resolves shell:AppsFolder, which keeps Dia's package
        # identity (and therefore its profile) intact.
        Start-Process -FilePath 'explorer.exe' -ArgumentList @("shell:AppsFolder\$appId") | Out-Null
        return
    }

    $info = Get-DiaInfo
    if (-not $info) {
        throw "Dia isn't installed for this account. Install it from https://www.diabrowser.com first."
    }
    if (-not (Test-Path -LiteralPath $info.ExePath)) {
        throw "Dia's executable is missing: $($info.ExePath)"
    }
    Start-Process -FilePath $info.ExePath | Out-Null
}

<#
.SYNOPSIS
    Waits for Dia's first main window. Returns an empty array on timeout.
#>
function Wait-DiaWindow {
    param([int]$TimeoutSeconds = 20)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $windows = @(Get-DiaWindows)
        if ($windows.Count -gt 0) { return $windows }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)

    return @()
}
