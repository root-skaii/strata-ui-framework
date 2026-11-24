@echo off
rem usage: build_vs.cmd <configure-preset> [build-preset] [extra cmake args...]
rem   build_vs.cmd x64-release
rem   build_vs.cmd x64-dev -DSTRATA_BUILD_VULKAN=OFF
rem   build_vs.cmd vs2022 vs2022-release
rem loads the Visual Studio x64 developer environment (unless already active), then configures and builds.
setlocal EnableExtensions

if "%~1"=="" goto :usage
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
if "%~1"=="/?" goto :usage

if defined VSCMD_VER goto :build

rem vswhere lives in the installer folder; vcvars itself looks for it on PATH
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if exist "%VSINSTALLER%\vswhere.exe" set "PATH=%VSINSTALLER%;%PATH%"

set "VSROOT="
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo build_vs: no Visual Studio with the C++ x64 tools found ^(install the "Desktop development with C++" workload^) 1>&2
    exit /b 1
)
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
    echo build_vs: %VSROOT%\VC\Auxiliary\Build\vcvars64.bat is missing 1>&2
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
cd /d "%~dp0"
set "CFG_PRESET=%~1"
set "BUILD_PRESET="
set "RAW_ARGS=%*"
call set "EXTRA_ARGS=%%RAW_ARGS:*%1=%%"

set "ARG2=%~2"
if defined ARG2 (
    if not "%ARG2:~0,1%"=="-" (
        set "BUILD_PRESET=%ARG2%"
        call set "EXTRA_ARGS=%%EXTRA_ARGS:*%2=%%"
    )
)
if not defined BUILD_PRESET set "BUILD_PRESET=%CFG_PRESET%"

cmake --preset "%CFG_PRESET%" %EXTRA_ARGS% || exit /b 1
cmake --build --preset "%BUILD_PRESET%" || exit /b 1
exit /b 0

:usage
echo Usage: build_vs.cmd ^<configure-preset^> [build-preset] [extra cmake args...]
echo.
echo Examples:
echo   build_vs.cmd x64-release
echo   build_vs.cmd x64-dev -DSTRATA_BUILD_VULKAN=OFF
echo   build_vs.cmd x64-clang-release
echo   build_vs.cmd vs2022 vs2022-release
echo.
echo Run "cmake --list-presets" to see all available presets.
exit /b 0
