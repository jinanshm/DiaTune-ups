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
    if ($package) {
        $exe = Join-Path $package.InstallLocation 'Dia.exe'
        $version = $package.Version.ToString()
        if (-not $version -and (Test-Path -LiteralPath $exe)) {
            $version = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
        }

        return [pscustomobject]@{
            Name              = $package.Name
            Version           = $version
            PackageFullName   = $package.PackageFullName
            PackageFamilyName = $package.PackageFamilyName
            InstallLocation   = $package.InstallLocation
            ExePath           = $exe
        }
    }

    # Fallback to HKCU AppModel repository when CIM/Get-AppxPackage is restricted
    try {
        $regBase = "HKCU:\Software\Classes\Local Settings\Software\Microsoft\Windows\CurrentVersion\AppModel\Repository\Packages"
        $pkgKey = Get-ChildItem $regBase -ErrorAction SilentlyContinue |
            Where-Object { $_.PSChildName -like "*$($script:DiaPackageName)*" } |
            Select-Object -First 1
        if ($pkgKey) {
            $props = Get-ItemProperty -Path $pkgKey.PSPath -ErrorAction SilentlyContinue
            if ($props -and $props.PackageRootFolder) {
                $exe = Join-Path $props.PackageRootFolder 'Dia.exe'
                $pkgFullName = $props.PackageID
                $parts = $pkgFullName -split '_'
                $version = if ($parts.Count -ge 2) { $parts[1] } else { '' }
                $family = if ($parts.Count -ge 5) { "$($parts[0])_$($parts[4])" } else { "$($script:DiaPackageName)_ttt1ap7aakyb4" }
                return [pscustomobject]@{
                    Name              = $script:DiaPackageName
                    Version           = $version
                    PackageFullName   = $pkgFullName
                    PackageFamilyName = $family
                    InstallLocation   = $props.PackageRootFolder
                    ExePath           = $exe
                }
            }
        }
    } catch {}

    return $null
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
    $main = @($owned | Where-Object { $_.ClassName -eq $script:DiaWindowClass -and $_.Width -ge 400 -and $_.Height -ge 300 })
    if ($main.Count -gt 0) { return $main }

    return @($owned | Where-Object {
        $_.Title -and $_.Width -ge 400 -and $_.Height -ge 300
    })
}

<#
.SYNOPSIS
    Starts Dia through its package identity.

.DESCRIPTION
    The Developer mode tweak needs Chromium's DevTools, and Chromium only
    offers them when the browser is started with a debugging port. When that
    tweak is on, Dia is started with the port instead of through the shell.
#>
function Start-Dia {
    # The debugging port Chromium's DevTools listen on.
    $debugPort = 9222

    $needsDebugPort = $false
    try {
        $state = Read-State
        $needsDebugPort = [bool]$state['developerMode']
    } catch {
        $needsDebugPort = $false
    }

    $info = Get-DiaInfo
    if (-not $info) {
        throw "Dia isn't installed for this account. Install it from https://www.diabrowser.com first."
    }
    if (-not (Test-Path -LiteralPath $info.ExePath)) {
        throw "Dia's executable is missing: $($info.ExePath)"
    }

    if ($needsDebugPort) {
        $appId = Get-DiaAppId
        if ($appId) {
            $actSrc = @'
using System;
using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IAppActivationMgr {
    [PreserveSig]
    int ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string appUserModelId, [MarshalAs(UnmanagedType.LPWStr)] string arguments, uint options, out uint processId);
}
[ComImport, Guid("45ba127d-10a8-46ea-8ab7-56ea9078943c")]
class AppActivationMgrClass { }
public static class AppActivator {
    public static uint Activate(string appId, string args) {
        var mgr = (IAppActivationMgr)new AppActivationMgrClass();
        uint pid;
        int hr = mgr.ActivateApplication(appId, args, 0, out pid);
        if (hr != 0) Marshal.ThrowExceptionForHR(hr);
        return pid;
    }
}
'@
            try {
                Add-Type -TypeDefinition $actSrc -Language CSharp -ErrorAction SilentlyContinue
                [AppActivator]::Activate($appId, "--remote-debugging-port=$debugPort --remote-allow-origins=*") | Out-Null
                return
            } catch {
                # Fall back to direct launch if COM activation fails
            }
        }
        Start-Process -FilePath $info.ExePath -ArgumentList @(
            "--remote-debugging-port=$debugPort",
            '--remote-allow-origins=*'
        ) | Out-Null
        return
    }

    $appId = Get-DiaAppId
    if ($appId) {
        # The shell resolves shell:AppsFolder, which keeps Dia's package
        # identity (and therefore its profile) intact.
        Start-Process -FilePath 'explorer.exe' -ArgumentList @("shell:AppsFolder\$appId") | Out-Null
        return
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
