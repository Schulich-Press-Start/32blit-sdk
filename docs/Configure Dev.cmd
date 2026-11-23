@echo off
setlocal
if "%~2"=="" goto usage
if /i "%~1"=="team-pong" (
  set "build=build-windows"
) else if /i "%~1"=="32blit-platformer" (
  set "build=build-vs"
) else (
  goto usage
)
set "sdk=%~f2"
if not exist "%sdk%\32blit-config.cmake" (
  echo SDK config not found: "%sdk%\32blit-config.cmake"
  exit /b 1
)
where cmake >nul 2>&1 || (echo Install CMake and put it on PATH. & exit /b 1)
where blit >nul 2>&1 || (echo Install the SDK blit CLI and put it on PATH. See docs\WindowsPlaytest.md. & exit /b 1)
pushd "%~dp0%~1" || exit /b 1
cmake --preset windows -B "%build%" "-D32BLIT_DIR=%sdk%" "-DSDL2_DIR=%sdk%/vs/sdl"
set "result=%errorlevel%"
popd
if not "%result%"=="0" exit /b %result%
echo Configuration succeeded. Use Dev in the editor to build and run.
exit /b 0
:usage
echo Usage: "Configure Dev.cmd" team-pong ^| 32blit-platformer "C:\path\to\32blit-sdk"
exit /b 2
