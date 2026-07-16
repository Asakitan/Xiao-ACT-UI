@echo off
:: SAO Auto - one-click release configure + build + test
::
:: RelWithDebInfo build, optimizations on, symbols preserved.  Runs the
:: full unit + smoke test suite.  Integration tests are OFF by default —
:: pass --integration to include them.
::
:: Usage:
::   build_release.bat [--clean] [--integration]

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    exit /b 1
)

set "RUN_INTEGRATION=0"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--clean" (
    echo [CLEAN] Removing build\windows-release ...
    if exist build\windows-release rmdir /s /q build\windows-release
    shift
    goto :parse_args
)
if /I "%~1"=="--integration" (
    set "RUN_INTEGRATION=1"
    shift
    goto :parse_args
)
echo ERROR: unknown argument: %~1
exit /b 1
:args_done

echo [1/3] Configuring windows-release preset...
cmake --preset windows-release
if errorlevel 1 goto :fail

echo [2/3] Building windows-release preset...
cmake --build --preset windows-release --parallel
if errorlevel 1 goto :fail

echo [3/3] Running tests...
if "%RUN_INTEGRATION%"=="1" (
    ctest --preset windows-release --output-on-failure
) else (
    ctest --preset windows-release --output-on-failure -LE integration
)
if errorlevel 1 (
    echo WARNING: some tests failed.
    goto :done
)

echo.
echo [OK] Release build complete.
echo      Artefacts: build\windows-release\bin\SaoAuto.exe
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
