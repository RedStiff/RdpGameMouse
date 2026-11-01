@echo off
rem Run on the PC you connect FROM. Registers the plugin for Remote Desktop Connection (mstsc), current user only.
setlocal
set "DEST=%LOCALAPPDATA%\RdpGameMouse"
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0RdpGameMouseClient.dll" "%DEST%\" >nul || (
    echo Copy failed. Close all Remote Desktop windows and run this again.
    pause
    exit /b 1
)
reg add "HKCU\Software\Microsoft\Terminal Server Client\Default\AddIns\RdpGameMouse" /v Name /t REG_SZ /d "%DEST%\RdpGameMouseClient.dll" /f >nul || (
    echo Registry update failed.
    pause
    exit /b 1
)
echo Installed to %DEST%
echo Reconnect Remote Desktop for the plugin to load.
pause
