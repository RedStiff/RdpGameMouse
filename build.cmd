@echo off
rem Builds the RDP client plugin (DLL) and the remote agents (EXE) into out\ with a static CRT,
rem so neither machine needs the Visual C++ redistributable.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio with C++ tools not found.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
if not exist obj mkdir obj

set "CFLAGS=/nologo /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /Foobj\"

cl %CFLAGS% client\rgm_client.cpp /LD ^
   /link /DEF:client\rgm_client.def /IMPLIB:obj\rgm_client.lib user32.lib /OUT:out\RdpGameMouseClient.dll || exit /b 1

rem Console agent: for running by hand or from a per-game launcher.
cl %CFLAGS% server\rgm_agent.cpp ^
   /link /OUT:out\RdpGameMouseAgent.exe || exit /b 1

rem Same agent without a console window, for the system-wide autostart.
cl %CFLAGS% server\rgm_agent.cpp ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /OUT:out\RdpGameMouseSystem.exe || exit /b 1

echo Build OK
