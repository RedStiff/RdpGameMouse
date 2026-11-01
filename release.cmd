@echo off
rem Builds everything and assembles the installers in release\:
rem   release\client\  - for the PC you connect FROM (plugin for mstsc)
rem   release\server\  - for the REMOTE machine (agent)
rem plus a zip of each folder.
setlocal
cd /d "%~dp0"
call "%~dp0build.cmd" || exit /b 1
cd /d "%~dp0"

if exist release rmdir /s /q release
mkdir release\client release\server

copy /y out\RdpGameMouseClient.dll release\client\ >nul
copy /y client\install-client.cmd release\client\ >nul
copy /y client\uninstall-client.cmd release\client\ >nul

copy /y out\RdpGameMouseSystem.exe release\server\ >nul
copy /y out\RdpGameMouseAgent.exe release\server\ >nul
copy /y server\install-system.cmd release\server\ >nul
copy /y server\uninstall-system.cmd release\server\ >nul

powershell -NoProfile -Command ^
  "Compress-Archive -Force release\client\* release\RdpGameMouse-client.zip;" ^
  "Compress-Archive -Force release\server\* release\RdpGameMouse-server.zip" || exit /b 1

echo Release ready in %~dp0release
