@echo off
:: SAO Auto - ship-ready hardened release build
::
:: Release build with LTO, CFG/CET when supported, high-entropy ASLR, DEP,
:: deterministic linking, and a release-artifact audit. Runs unit + smoke +
:: integration tests, then builds tools. Optionally post-signs every PE via
:: sign_cli.
::
::   build_hardened.bat [--clean] [--pack] [--sign-thumbprint=<sha1>] [--crypter]
::
:: --crypter: opt-in. After the normal build + audit, extract the flat stub
:: blob (sao_shell_stub_bin) and run sao_shell_crypter_cli over SaoAuto.exe and
:: the sao_platform_*.dll set, writing wrapped copies alongside the originals,
:: then continues to sign/pack. Default off so normal hardened builds are
:: unaffected. The crypter step is a no-op when the host/stub targets are not
:: built (e.g. a sliver tree without the shell subsystem).

setlocal enabledelayedexpansion

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%"

if not defined VCPKG_ROOT (
    echo ERROR: VCPKG_ROOT environment variable is not set.
    exit /b 1
)

set "DO_PACK=0"
set "SIGN_THUMB="
set "DO_CRYPTER=0"

:parse_args
if "%~1"=="" goto :args_done
if /I "%~1"=="--clean" (
    echo [CLEAN] Removing build\windows-hardened ...
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
set "ARG=%~1"
if /I "!ARG:~0,18!"=="--sign-thumbprint=" (
    set "SIGN_THUMB=!ARG:~18!"
    shift
    goto :parse_args
)
echo ERROR: unknown argument: %~1
exit /b 1
:args_done

if defined SAO_NATIVE_LOADER_PAYLOAD_TARGETS if not defined SAO_CRYPTER_KEY (
    echo ERROR: SAO_CRYPTER_KEY is required when SAO_NATIVE_LOADER_PAYLOAD_TARGETS is set.
    exit /b 1
)

echo [1/5] Configuring windows-hardened preset...
if defined SAO_NATIVE_LOADER_PAYLOAD_TARGETS (
    cmake --preset windows-hardened "-DSAO_NATIVE_LOADER_PAYLOAD_TARGETS=!SAO_NATIVE_LOADER_PAYLOAD_TARGETS!"
) else (
    cmake --preset windows-hardened "-DSAO_NATIVE_LOADER_PAYLOAD_TARGETS="
)
if errorlevel 1 goto :fail

echo [2/5] Building windows-hardened preset...
cmake --build --preset windows-hardened --parallel
if errorlevel 1 goto :fail

echo [3/5] Staging and auditing hardened release artifacts...
cmake --build --preset windows-hardened --target sao_release_artifact_audit --parallel
if errorlevel 1 goto :fail

echo [4/5] Running full test suite (unit + smoke + integration)...
ctest --preset windows-hardened --output-on-failure
if errorlevel 1 (
    echo WARNING: some tests failed.  Continuing anyway; investigate before shipping.
)

echo [5/5] Post-processing...

if "%DO_CRYPTER%"=="1" (
    echo   [crypter] Building flat stub blob ^(sao_shell_stub_bin^)...
    cmake --build --preset windows-hardened --target sao_shell_stub_bin --parallel
    if errorlevel 1 (
        echo ERROR: sao_shell_stub_bin target failed.
        goto :fail
    )
    set "STUB_BIN=build\windows-hardened\bin\Release\stub.bin"
    if not exist "!STUB_BIN!" (
        echo ERROR: stub.bin not produced at !STUB_BIN!
        goto :fail
    )
    if not exist build\windows-hardened\bin\tools\sao_shell_crypter_cli.exe (
        echo ERROR: sao_shell_crypter_cli.exe not built -- crypter step requires SAO_BUILD_TOOLS=ON.
        goto :fail
    )
    echo   [crypter] Wrapping SaoAuto.exe with stub !STUB_BIN! ...
    build\windows-hardened\bin\tools\sao_shell_crypter_cli.exe ^
        --in build\windows-hardened\bin\SaoAuto.exe ^
        --out build\windows-hardened\bin\SaoAuto.wrapped.exe ^
        --stub "!STUB_BIN!"
    if errorlevel 1 (
        echo ERROR: sao_shell_crypter_cli failed for SaoAuto.exe
        goto :fail
    )
    for %%D in (build\windows-hardened\bin\sao_platform_*.dll) do (
        echo   [crypter] Wrapping %%~nxD ...
        build\windows-hardened\bin\tools\sao_shell_crypter_cli.exe ^
            --in "%%D" ^
            --out "build\windows-hardened\bin\%%~nD.wrapped.dll" ^
            --stub "!STUB_BIN!"
        if errorlevel 1 (
            echo ERROR: sao_shell_crypter_cli failed for %%D
            goto :fail
        )
    )
)

if defined SIGN_THUMB (
    if exist build\windows-hardened\bin\tools\sao_sign.exe (
        echo   Signing PE artefacts with thumbprint !SIGN_THUMB! ...
        build\windows-hardened\bin\tools\sao_sign.exe ^
            --input build\windows-hardened\bin\SaoAuto.exe ^
            --cert !SIGN_THUMB!
        if errorlevel 1 (
            echo ERROR: sign_cli failed.
            goto :fail
        )
    ) else (
        echo   sao_sign.exe not built, skipping post-signing.
    )
)

if "%DO_PACK%"=="1" (
    if exist build\windows-hardened\bin\tools\sao_pack.exe (
        echo   Assembling onedir release via sao_pack ...
        build\windows-hardened\bin\tools\sao_pack.exe ^
            --input build\windows-hardened ^
            --output dist\release\SaoAuto ^
            --version %CMAKE_PROJECT_VERSION% ^
            --force
        if errorlevel 1 (
            echo ERROR: sao_pack failed.
            goto :fail
        )
    ) else (
        echo   sao_pack.exe not built, skipping onedir assembly.
    )
)

echo.
echo [OK] Hardened build complete.
echo      Artefacts: build\windows-hardened\bin\SaoAuto.exe
if "%DO_PACK%"=="1" (
    echo      Onedir  : dist\release\SaoAuto\SaoAuto.exe
)
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
