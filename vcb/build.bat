@echo off
setlocal enabledelayedexpansion
where cl >nul 2>nul && goto :build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo Visual Studio C++ Build Tools not found. Install them with:
  echo.
  echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  echo.
  pause & exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo MSVC toolset missing. & pause & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
:build
if not exist build mkdir build
cl /nologo /std:c++20 /O2 /EHsc /MT /DNDEBUG main.cpp /Fe:build\vcb.exe /Fo:build\ /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib gdiplus.lib winmm.lib
if errorlevel 1 ( echo. & echo BUILD FAILED & pause & exit /b 1 )
if exist config.ini copy /y config.ini build\config.ini >nul
echo. & echo Built: build\vcb.exe
pause