@echo off
rem usage: build_vs.cmd <configure-preset> [build-preset]
rem   build_vs.cmd x64-release
rem   build_vs.cmd vs2022 vs2022-release
rem loads the vs 2022 x64 developer environment, then configures + builds.
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set BUILD_PRESET=%2
if "%BUILD_PRESET%"=="" set BUILD_PRESET=%1
cmake --preset %1 || exit /b 1
cmake --build --preset %BUILD_PRESET% || exit /b 1
