@echo off
rem Stops the system-wide game-mouse agent and removes its autostart.
taskkill /im RdpGameMouseSystem.exe /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v RdpGameMouse /f >nul 2>&1
del /q "%LOCALAPPDATA%\RdpGameMouse\RdpGameMouseSystem.exe" "%LOCALAPPDATA%\RdpGameMouse\agent.log" 2>nul
rmdir "%LOCALAPPDATA%\RdpGameMouse" 2>nul
echo Removed.
pause
