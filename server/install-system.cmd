@echo off
rem Run on the REMOTE machine. Installs the system-wide game-mouse agent for the current user:
rem it starts at logon, runs in the background and turns game mode on for any fullscreen window
rem that hides the cursor. Ctrl+Alt+F12 cycles Auto / Always / Off.
rem It also forwards this machine's sound to the plugin with low latency (--audio).
setlocal
set "DEST=%LOCALAPPDATA%\RdpGameMouse"
set "ARGS=--fullscreen --audio --key Ctrl+Alt+F12 --log \"%DEST%\agent.log\""
taskkill /im RdpGameMouseSystem.exe /f >nul 2>&1
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0RdpGameMouseSystem.exe" "%DEST%\" >nul || (
    echo Copy failed.
    pause
    exit /b 1
)
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v RdpGameMouse /t REG_SZ /d "\"%DEST%\RdpGameMouseSystem.exe\" %ARGS%" /f >nul || (
    echo Registry update failed.
    pause
    exit /b 1
)
start "" "%DEST%\RdpGameMouseSystem.exe" --fullscreen --audio --key Ctrl+Alt+F12 --log "%DEST%\agent.log"
echo Installed to %DEST% and started. It will also start automatically at logon.
pause
