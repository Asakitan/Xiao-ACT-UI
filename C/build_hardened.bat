@echo off
:: SAO Auto - strict hardened release acceptance
::
:: windows-hardened is the ship tree. No tests exist in this repository;
:: verification is brain simulation only. --crypter is an explicit
:: diagnostic that writes only outside the audited ship tree.
::
::   build_hardened.bat [--clean] [--pack] [--crypter]

setlocal enabledelayedexpansion
set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    goto :fail
)
set "DO_PACK=0"
set "DO_CRYPTER=0"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--clean" (
    echo [CLEAN] Removing hardened trees ...
    if exist build\windows-hardened rmdir /s /q build\windows-hardened
    shift
    goto :parse_args
)
if /I "%~1"=="--pack" (
    set "DO_PACK=1"
    shift
    goto :parse_args
)
if /I "%~1"=="--crypter" (
    set "DO_CRYPTER=1"
    shift
    goto :parse_args
)
echo ERROR: unknown argument: %~1
goto :fail
:args_done

if defined SAO_NATIVE_LOADER_PAYLOAD_TARGETS (
    echo ERROR: SAO_NATIVE_LOADER_PAYLOAD_TARGETS has no production consumer mapping.
    goto :fail
)

echo [1/5] Configuring windows-hardened preset...
cmake --preset windows-hardened
if errorlevel 1 goto :fail
if not exist build\windows-hardened\sao_version.txt (
    echo ERROR: CMake version file was not produced.
    goto :fail
)
set "SAO_VERSION="
set /p SAO_VERSION=<build\windows-hardened\sao_version.txt
if not defined SAO_VERSION (
    echo ERROR: CMake version file is empty.
    goto :fail
)
echo(!SAO_VERSION!| findstr.exe /r /x /c:"[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" >nul
if errorlevel 1 (
    echo ERROR: CMake version must be strict semver x.y.z.
    goto :fail
)

echo [2/5] Building windows-hardened ship tree...
cmake --build --preset windows-hardened --parallel
if errorlevel 1 goto :fail

echo [3/5] Staging and auditing windows-hardened ship tree...
cmake --build --preset windows-hardened --target sao_release_acceptance --parallel
if errorlevel 1 goto :fail

echo [4/5] Processing optional diagnostics and packaging...

if "%DO_CRYPTER%"=="1" (
    echo   [crypter diagnostic] Building flat stub blob ...
    cmake --build --preset windows-hardened --target sao_shell_stub_bin --parallel
    if errorlevel 1 goto :fail
    set "STUB_BIN=build\windows-hardened\bin\Release\stub.bin"
    if not exist "!STUB_BIN!" (
        echo ERROR: stub.bin not produced at !STUB_BIN!
        goto :fail
    )
    if exist build\windows-hardened\bin\Release\sao_shell_crypter_cli.exe (
        set "CRYPTER_TOOL=build\windows-hardened\bin\Release\sao_shell_crypter_cli.exe"
    ) else if exist build\windows-hardened\bin\sao_shell_crypter_cli.exe (
        set "CRYPTER_TOOL=build\windows-hardened\bin\sao_shell_crypter_cli.exe"
    ) else if exist build\windows-hardened\bin\Release\tools\sao_shell_crypter_cli.exe (
        set "CRYPTER_TOOL=build\windows-hardened\bin\Release\tools\sao_shell_crypter_cli.exe"
    ) else if exist build\windows-hardened\bin\tools\sao_shell_crypter_cli.exe (
        set "CRYPTER_TOOL=build\windows-hardened\bin\tools\sao_shell_crypter_cli.exe"
    ) else (
        echo ERROR: sao_shell_crypter_cli.exe not built -- requested diagnostic is fatal.
        goto :fail
    )
    if not exist build\windows-hardened\diagnostics\crypter mkdir build\windows-hardened\diagnostics\crypter
    "!CRYPTER_TOOL!" --in build\windows-hardened\ship\bin\SaoAuto.exe --out build\windows-hardened\diagnostics\crypter\SaoAuto.wrapped.exe --stub "!STUB_BIN!"
    if errorlevel 1 goto :fail
    for %%D in (build\windows-hardened\ship\bin\sao_platform_*.dll) do (
        "!CRYPTER_TOOL!" --in "%%D" --out "build\windows-hardened\diagnostics\crypter\%%~nD.wrapped.dll" --stub "!STUB_BIN!"
        if errorlevel 1 goto :fail
    )
)

if "%DO_PACK%"=="1" (
    if exist build\windows-hardened\bin\Release\sao_pack.exe (
        set "PACK_TOOL=build\windows-hardened\bin\Release\sao_pack.exe"
    ) else if exist build\windows-hardened\bin\sao_pack.exe (
        set "PACK_TOOL=build\windows-hardened\bin\sao_pack.exe"
    ) else if exist build\windows-hardened\bin\Release\tools\sao_pack.exe (
        set "PACK_TOOL=build\windows-hardened\bin\Release\tools\sao_pack.exe"
    ) else if exist build\windows-hardened\bin\tools\sao_pack.exe (
        set "PACK_TOOL=build\windows-hardened\bin\tools\sao_pack.exe"
    ) else (
        echo ERROR: sao_pack.exe not built -- requested packaging is fatal.
        goto :fail
    )
    "!PACK_TOOL!" --input build\windows-hardened\ship\bin --output dist\release\SaoAuto --version !SAO_VERSION! --zip --force
    if errorlevel 1 goto :fail
    if not exist dist\release\SaoAuto\manifest.json goto :fail
    if not exist dist\release\SaoAuto.zip goto :fail
    if not exist dist\release\SaoAuto\plugins\ (
        echo ERROR: onedir package is missing the same-root plugins directory.
        goto :fail
    )
)

echo [5/5] Hardened release acceptance complete.
echo      Ship tree: build\windows-hardened\ship\bin\SaoAuto.exe
if "%DO_PACK%"=="1" echo      Onedir: dist\release\SaoAuto\SaoAuto.exe
goto :done

:fail
echo.
echo [FAIL] Hardened release acceptance aborted.
popd
exit /b 1

:done
popd
endlocal
exit /b 0
