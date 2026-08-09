@echo off
:: SAO Auto - one-click debug configure + build
::
:: Runs the windows-debug CMake preset, builds SaoAuto.exe and every DLL,
:: then runs the fast tests.  Full symbols, no PGO, no LTO -- this is what
:: you want during iterative development.
::
:: Requirements:
::   * Visual Studio 2022 with the "Desktop development with C++" workload
::   * VCPKG_ROOT env var pointing at a vcpkg checkout
::
:: Usage:
::   build_debug.bat [--clean]

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    echo        See vendor_note.md for setup instructions.
    exit /b 1
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
echo [tests] Running Catch2 unit + smoke tests...
ctest --preset windows-debug --output-on-failure -LE integration -j 16
if errorlevel 1 (
    echo WARNING: some tests failed.
    goto :done
)

echo.
echo [OK] Debug build complete.
echo      Artefacts: build\windows-debug\bin\SaoAuto.exe
goto :done

:fail
echo.
echo [FAIL] Build aborted.
popd
exit /b 1

:done
popd
endlocal
exit /b 0
