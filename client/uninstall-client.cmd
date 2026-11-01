@echo off
rem Removes the plugin from Remote Desktop Connection. Close all Remote Desktop windows first.
reg delete "HKCU\Software\Microsoft\Terminal Server Client\Default\AddIns\RdpGameMouse" /f >nul 2>&1
del /q "%LOCALAPPDATA%\RdpGameMouse\RdpGameMouseClient.dll" 2>nul
rmdir "%LOCALAPPDATA%\RdpGameMouse" 2>nul
echo Removed.
pause
