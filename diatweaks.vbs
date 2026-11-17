' diatweaks.vbs - silent (no console window) launcher for Dia Tune-ups.
' Double-click this file, or point a shortcut at it, to apply/launch/update the
' tweaks without a CMD window flashing on screen. For example, a shortcut with
' the target:  wscript.exe "C:\path\to\DiaTweaks-Windows\diatweaks.vbs" launch

Option Explicit
Dim sh, fso, root, cli, ps, cmd, args, i

Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
root = fso.GetParentFolderName(WScript.ScriptFullName)
cli = root & "\cli\diatweaks.ps1"

' Prefer PowerShell 7, then fall back to the built-in Windows PowerShell 5.1.
ps = "pwsh.exe"
If sh.ExpandEnvironmentStrings("%ProgramFiles%\PowerShell\7\pwsh.exe") <> "" Then
    If fso.FileExists(sh.ExpandEnvironmentStrings("%ProgramFiles%\PowerShell\7\pwsh.exe")) Then
        ps = """" & sh.ExpandEnvironmentStrings("%ProgramFiles%\PowerShell\7\pwsh.exe") & """"
    End If
End If

cmd = ps & " -NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File """ & cli & """"
For i = 0 To WScript.Arguments.Count - 1
    args = Replace(WScript.Arguments(i), """", """""")
    cmd = cmd & " """ & args & """"
Next

' Window style 0 = hidden, False = do not wait for completion.
sh.Run cmd, 0, False
