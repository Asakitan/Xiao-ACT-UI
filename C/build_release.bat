@echo off
:: SAO Auto - strict RelWithDebInfo release acceptance
::
:: No tests exist in this repository; verification is brain simulation only.
::
:: Usage:
::   build_release.bat [--clean]

setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    goto :fail
)

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--clean" (
    echo [CLEAN] Removing release trees ...
    if exist build\windows-release rmdir /s /q build\windows-release
    shift
    goto :parse_args
)
echo ERROR: unknown argument: %~1
goto :fail
:args_done

echo [1/4] Configuring windows-release preset...
cmake --preset windows-release
if errorlevel 1 goto :fail

echo [2/4] Building windows-release ship tree...
cmake --build --preset windows-release --parallel
if errorlevel 1 goto :fail

echo [3/4] Staging and auditing windows-release ship tree...
cmake --build --preset windows-release --target sao_release_acceptance --parallel
if errorlevel 1 goto :fail

echo [4/4] Packaging audited ship tree and verifying ZIP...
if exist build\windows-release\bin\RelWithDebInfo\sao_pack.exe (
    set "PACK_TOOL=build\windows-release\bin\RelWithDebInfo\sao_pack.exe"
) else if exist build\windows-release\bin\sao_pack.exe (
    set "PACK_TOOL=build\windows-release\bin\sao_pack.exe"
) else (
    echo ERROR: sao_pack.exe not built -- release packaging is mandatory.
    goto :fail
)
"!PACK_TOOL!" --input build\windows-release\ship\bin --output dist\release\SaoAuto --version 0.2.0 --zip --force
if errorlevel 1 goto :fail
if not exist dist\release\SaoAuto\manifest.json (
    echo ERROR: package manifest was not produced.
    goto :fail
)
if not exist dist\release\SaoAuto.zip (
    echo ERROR: release ZIP was not produced.
    goto :fail
)

echo.
echo [OK] Release acceptance complete.
echo      Ship tree: build\windows-release\ship\bin\SaoAuto.exe
echo      Package: dist\release\SaoAuto.zip
goto :done

:fail
echo.
echo [FAIL] Release acceptance aborted.
popd
exit /b 1

:done
popd
endlocal
exit /b 0
