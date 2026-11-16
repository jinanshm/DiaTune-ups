# windows.ps1 - Win32 and DWM interop for DiaTweaks on Windows.
#
# Everything here is read-only or a window-manager hint. Nothing is injected
# into Dia, and nothing touches the signed package.

if (-not ('DiaTweaks.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace DiaTweaks
{
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public static class Native
    {
        // Documented DWM attributes (Windows 11 22000 and later).
        public const int DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        public const int DWMWA_BORDER_COLOR = 34;
        public const int DWMWA_SYSTEMBACKDROP_TYPE = 38;

        public const int DWMWCP_DEFAULT = 0;
        public const int DWMWCP_DONOTROUND = 1;
        public const int DWMWCP_ROUND = 2;
        public const int DWMWCP_ROUNDSMALL = 3;

        public const int DWMSBT_AUTO = 0;
        public const int DWMSBT_NONE = 1;
        public const int DWMSBT_MAINWINDOW = 2; // Mica
        public const int DWMSBT_TRANSIENTWINDOW = 3; // Acrylic
        public const int DWMSBT_TABBEDWINDOW = 4; // Mica Alt

        public const uint DWMWA_COLOR_DEFAULT = 0xFFFFFFFF;
        public const uint DWMWA_COLOR_NONE = 0xFFFFFFFE;

        [DllImport("dwmapi.dll")]
        public static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

        [DllImport("dwmapi.dll")]
        public static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);

        [DllImport("user32.dll")]
        public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

        [DllImport("user32.dll")]
        public static extern bool IsWindow(IntPtr hwnd);

        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hwnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetClassName(IntPtr hwnd, StringBuilder name, int maxCount);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern int GetWindowText(IntPtr hwnd, StringBuilder text, int maxCount);

        [DllImport("user32.dll")]
        public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

        [DllImport("user32.dll")]
        public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    }
}
'@
}

function Format-HResult {
    param([int]$Code)
    return ('0x{0:X8}' -f $Code)
}

function Test-WindowsEleven {
    # 22000 is the first Windows 11 build. The DWM attributes used here need it.
    return ([Environment]::OSVersion.Version.Build -ge 22000)
}

<#
.SYNOPSIS
    Lists visible top-level windows with a non-empty client area.
#>
function Get-TopLevelWindows {
    [CmdletBinding()]
    param()

    $windows = [System.Collections.Generic.List[object]]::new()
    $callback = [DiaTweaks.EnumWindowsProc] {
        param([IntPtr]$hWnd, [IntPtr]$lParam)

        if ([DiaTweaks.Native]::IsWindowVisible($hWnd)) {
            $rect = New-Object DiaTweaks.RECT
            if ([DiaTweaks.Native]::GetWindowRect($hWnd, [ref]$rect)) {
                $width = $rect.Right - $rect.Left
                $height = $rect.Bottom - $rect.Top
                if ($width -gt 0 -and $height -gt 0) {
                    $class = [System.Text.StringBuilder]::new(256)
                    [void][DiaTweaks.Native]::GetClassName($hWnd, $class, $class.Capacity)

                    $title = [System.Text.StringBuilder]::new(512)
                    [void][DiaTweaks.Native]::GetWindowText($hWnd, $title, $title.Capacity)

                    $processId = [uint32]0
                    [void][DiaTweaks.Native]::GetWindowThreadProcessId($hWnd, [ref]$processId)

                    $windows.Add([pscustomobject]@{
                        Handle    = $hWnd
                        ProcessId = [int]$processId
                        ClassName = $class.ToString()
                        Title     = $title.ToString()
                        Width     = $width
                        Height    = $height
                    })
                }
            }
        }

        return $true
    }

    [void][DiaTweaks.Native]::EnumWindows($callback, [IntPtr]::Zero)
    return $windows.ToArray()
}

<#
.SYNOPSIS
    Reads one DWORD DWM window attribute. Returns $null when Windows does not
    support the attribute or the window is gone.
#>
function Get-DwmWindowAttribute {
    param(
        [Parameter(Mandatory)][IntPtr]$Handle,
        [Parameter(Mandatory)][int]$Attribute
    )

    $value = 0
    $hr = [DiaTweaks.Native]::DwmGetWindowAttribute($Handle, $Attribute, [ref]$value, 4)
    if ($hr -ne 0) { return $null }
    return $value
}

function Set-DwmWindowAttribute {
    param(
        [Parameter(Mandatory)][IntPtr]$Handle,
        [Parameter(Mandatory)][int]$Attribute,
        [Parameter(Mandatory)][int]$Value
    )

    $value = $Value
    return [DiaTweaks.Native]::DwmSetWindowAttribute($Handle, $Attribute, [ref]$value, 4)
}

<#
.SYNOPSIS
    Applies the native DWM tweaks to one Dia window.

.DESCRIPTION
    edgeToEdge removes the window's 1px border by asking DWM for
    DWMWA_COLOR_NONE. squareSidebarCorners asks DWM not to round the corners.
    windowMaterial configures DWMWA_SYSTEMBACKDROP_TYPE (Mica, Acrylic, Mica Alt).
    All are compositor hints, so Dia itself does not have to change.

    Returns a small object describing what DWM said, so callers can stay quiet
    when nothing changed.
#>
function Set-TweakedWindow {
    param(
        [Parameter(Mandatory)][IntPtr]$Handle,
        [bool]$EdgeToEdge,
        [bool]$SquareCorners = $false,
        [string]$WindowMaterial = 'default'
    )

    $result = [pscustomobject]@{
        Handle         = $Handle
        BorderHr       = 0
        CornersHr      = 0
        MaterialHr     = 0
        BorderBefore   = $null
        CornersBefore  = $null
        MaterialBefore = $null
    }

    if (-not [DiaTweaks.Native]::IsWindow($Handle)) {
        $result.BorderHr = -1
        $result.CornersHr = -1
        $result.MaterialHr = -1
        return $result
    }

    # DWM colour sentinels are DWORDs. As signed Int32 they are -2
    # (DWMWA_COLOR_NONE, 0xFFFFFFFE) and -1 (DWMWA_COLOR_DEFAULT, 0xFFFFFFFF).
    $targetBorder = if ($EdgeToEdge) { -2 } else { -1 }

    # DWMWCP_DONOTROUND (1) when SquareCorners is requested,
    # DWMWCP_ROUND (2) to explicitly preserve/restore Windows 11 rounded corners.
    $targetCorners = if ($SquareCorners) {
        [DiaTweaks.Native]::DWMWCP_DONOTROUND
    } else {
        [DiaTweaks.Native]::DWMWCP_ROUND
    }

    $targetMaterial = switch ($WindowMaterial.ToLowerInvariant()) {
        'mica'      { [DiaTweaks.Native]::DWMSBT_MAINWINDOW }
        'acrylic'   { [DiaTweaks.Native]::DWMSBT_TRANSIENTWINDOW }
        'micaalt'   { [DiaTweaks.Native]::DWMSBT_TABBEDWINDOW }
        'none'      { [DiaTweaks.Native]::DWMSBT_NONE }
        default     { [DiaTweaks.Native]::DWMSBT_AUTO }
    }

    $result.BorderBefore = Get-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_BORDER_COLOR)
    $result.CornersBefore = Get-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_WINDOW_CORNER_PREFERENCE)
    $result.MaterialBefore = Get-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_SYSTEMBACKDROP_TYPE)

    # Only call DWM when the value differs, so a watching loop does not churn
    # the compositor every couple of seconds.
    if ($null -eq $result.BorderBefore -or $result.BorderBefore -ne $targetBorder) {
        $result.BorderHr = Set-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_BORDER_COLOR) -Value $targetBorder
    }
    if ($null -eq $result.CornersBefore -or $result.CornersBefore -ne $targetCorners) {
        $result.CornersHr = Set-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_WINDOW_CORNER_PREFERENCE) -Value $targetCorners
    }
    if ($null -eq $result.MaterialBefore -or $result.MaterialBefore -ne $targetMaterial) {
        $result.MaterialHr = Set-DwmWindowAttribute -Handle $Handle -Attribute ([DiaTweaks.Native]::DWMWA_SYSTEMBACKDROP_TYPE) -Value $targetMaterial
    }

    return $result
}

<#
.SYNOPSIS
    Returns a Dia window to Windows' default frame and corners.
#>
function Reset-TweakedWindow {
    param([Parameter(Mandatory)][IntPtr]$Handle)

    return Set-TweakedWindow -Handle $Handle -EdgeToEdge $false -SquareCorners $false -WindowMaterial 'default'
}
