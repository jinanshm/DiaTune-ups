@echo off
setlocal
set SEE_MASK_NOZONECHECKS=1
set "CLI=%~dp0cli\diatweaks.ps1"

if not exist "%CLI%" (
    echo Can't find "%CLI%".
    exit /b 1
)

where pwsh.exe >nul 2>nul
if not errorlevel 1 (
    pwsh.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CLI%" %*
    exit /b %ERRORLEVEL%
)

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%CLI%" %*
exit /b %ERRORLEVEL%
