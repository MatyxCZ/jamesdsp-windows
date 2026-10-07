@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Register-Application.ps1" -InstallDirectory "%~dp0."
if errorlevel 1 (
    echo.
    echo Registration failed. Read the error above. Extract the complete ZIP to a permanent folder and try again.
    pause
    exit /b 1
)
echo.
echo You can now launch JamesDSP from Windows Search or the Start Menu.
echo Startup at sign-in is unchanged. Enable it in JamesDSP settings if desired.
pause
exit /b 0
