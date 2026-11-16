@echo off
setlocal
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
set OVR=..\QuestLHSync\third_party\openvr
cl /nologo /c /O1 /Oi /GS- /Gy- /Zl /fp:fast src\cave.c /Foout\cave.obj || exit /b 1
python src\gen_cave.py out\cave.obj out\cave_bytes.h || exit /b 1
cl /nologo /LD /O2 /EHsc /std:c++17 /MT /W3 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  /I%OVR%\headers /Iout src\driver.cpp /Foout\ /Fe:driver\framedragfix\bin\win64\driver_framedragfix.dll ^
  /link /NOLOGO psapi.lib || exit /b 1
echo built driver\framedragfix\bin\win64\driver_framedragfix.dll
