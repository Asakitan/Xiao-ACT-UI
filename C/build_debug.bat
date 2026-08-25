@echo off
:: SAO Auto - one-click debug configure + build
::
:: Runs the windows-debug CMake preset and builds SaoAuto.exe and every DLL.
:: No tests exist in this repository; verification is brain simulation only.
::
:: Usage:
::   build_debug.bat [--clean]

setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    echo        See vendor_note.md for setup instructions.
    goto :fail
)

if /I "%~1"=="--clean" (
    echo [CLEAN] Removing build\windows-debug ...
    if exist build\windows-debug rmdir /s /q build\windows-debug
)

echo [1/2] Configuring windows-debug preset...
cmake --preset windows-debug
if errorlevel 1 goto :fail

echo [2/2] Building windows-debug preset...
cmake --build --preset windows-debug --parallel 16
if errorlevel 1 goto :fail

echo.
echo [OK] Debug build complete.
echo      Artefacts: build\windows-debug\bin\SaoAuto.exe
goto :done

:fail
echo.
echo [FAIL] Debug build aborted.
popd
exit /b 1

:done
popd
endlocal
exit /b 0
