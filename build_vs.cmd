@echo off
rem usage: build_vs.cmd <configure-preset> [build-preset]
rem   build_vs.cmd x64-release
rem   build_vs.cmd vs2022 vs2022-release
rem loads the visual studio x64 developer environment (unless this already is a developer prompt), then configures + builds.
setlocal

if defined VSCMD_VER goto :build

rem vswhere lives in the installer folder; vcvars itself looks for it on PATH
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if exist "%VSINSTALLER%\vswhere.exe" set "PATH=%VSINSTALLER%;%PATH%"

set "VSROOT="
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo build_vs: no visual studio with the C++ x64 tools found ^(install the "Desktop development with C++" workload^) 1>&2
    exit /b 1
)
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo build_vs: %VSROOT%\VC\Auxiliary\Build\vcvars64.bat is missing 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
cd /d "%~dp0"
set BUILD_PRESET=%2
if "%BUILD_PRESET%"=="" set BUILD_PRESET=%1
cmake --preset %1 || exit /b 1
cmake --build --preset %BUILD_PRESET% || exit /b 1
