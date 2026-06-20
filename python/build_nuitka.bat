@echo off
setlocal enabledelayedexpansion

:: ──────────────────────────────────────────────────────────
::  SAO-UI Nuitka Modular Build
::
::  Architecture:
::    XiaoACTUI.exe  ← tiny stub (sys.path + import main)
::    runtime/       ← compiled .pyd modules (individually updatable)
::    web/ assets/ plugins/ locale/ ← data (directly updatable)
::
::  Each package → one .pyd → dev_publish ships changed .pyd only
:: ──────────────────────────────────────────────────────────

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST=%ROOT%dist"
set "RELEASE=%DIST%\release\XiaoACTUI"
set "RT=%RELEASE%\runtime"

:: Parse args: --module <name> to rebuild single module
set "SINGLE_MODULE="
if "%~1"=="--module" (
    set "SINGLE_MODULE=%~2"
    echo Rebuilding single module: %~2
    goto :single_module
)

:: ── Full build ──────────────────────────────────────

:: Step [1/9] Cython
echo [1/9] Building Cython accelerators...
python build_cython_ext.py build_ext --inplace
if errorlevel 1 goto :fail

:: Step [2/9] Encrypt backend
echo [2/9] Encrypting backend data...
if exist "%ROOT%mem_probe\_encrypt_drivers.py" (
    python "%ROOT%mem_probe\_encrypt_drivers.py" 2>nul
)

:: Step [3/9] Stub EXE (tiny, standalone with Python runtime only)
echo [3/9] Compiling stub EXE...
if exist "%DIST%\nuitka_stub" rmdir /s /q "%DIST%\nuitka_stub"
python -m nuitka ^
    --standalone ^
    --assume-yes-for-downloads ^
    --windows-console-mode=disable ^
    --windows-icon-from-ico=icon.ico ^
    --windows-uac-admin ^
    --output-dir=%DIST%\nuitka_stub ^
    --output-filename=XiaoACTUI.exe ^
    --nofollow-import-to=main ^
    --nofollow-import-to=config ^
    --nofollow-import-to=sao_gui ^
    --nofollow-import-to=sao_webview ^
    --nofollow-import-to=gui_modules ^
    --nofollow-import-to=utils ^
    --nofollow-import-to=render ^
    --nofollow-import-to=updater ^
    --nofollow-import-to=sao_theme ^
    --nofollow-import-to=act_platform ^
    --nofollow-import-to=ui_gpu ^
    --nofollow-import-to=ai_editor ^
    --nofollow-import-to=workshop ^
    --nofollow-import-to=license ^
    --nofollow-import-to=mem_probe ^
    --nofollow-import-to=sao_web_panel_common ^
    --no-deployment-flag=excluded-module-usage ^
    --jobs=8 ^
    stub.py
if errorlevel 1 (
    echo ERROR: Stub compilation failed
    goto :fail
)

:: Step [4/9] Compile each package/module → .pyd
echo [4/9] Compiling application modules to .pyd...
if not exist "%DIST%\nuitka_modules" mkdir "%DIST%\nuitka_modules"

:: Packages (--include-package ensures submodules are compiled into the .pyd)
for %%P in (gui_modules utils render updater sao_theme act_platform ui_gpu ai_editor workshop license mem_probe) do (
    echo   Compiling %%P...
    python -m nuitka --module --assume-yes-for-downloads --no-prefer-source-code --include-package=%%P --output-dir="%DIST%\nuitka_modules" --jobs=4 "%%P/" 2>nul
    if errorlevel 1 (
        echo   WARNING: %%P compilation failed, will include as .py fallback
    )
)

:: Standalone modules
for %%M in (config main sao_gui sao_webview sao_web_panel_common) do (
    echo   Compiling %%M.py...
    python -m nuitka --module --assume-yes-for-downloads --no-prefer-source-code --output-dir="%DIST%\nuitka_modules" --jobs=4 "%%M.py" 2>nul
    if errorlevel 1 (
        echo   WARNING: %%M compilation failed, will include as .py fallback
    )
)

:: Step [5/9] Build update.exe
echo [5/9] Building update.exe...
if exist update.spec (
    pyinstaller --clean --noconfirm update.spec 2>nul
)

:: Step [6/9] Assemble release layout
echo [6/9] Assembling release layout...
if exist "%RELEASE%" rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"
mkdir "%RT%"
mkdir "%RELEASE%\temp"

:: Copy stub standalone output → release root (EXE + Python DLLs + stdlib)
xcopy /e /i /y /q "%DIST%\nuitka_stub\stub.dist\*" "%RELEASE%\" >nul
:: Rename stub exe
if exist "%RELEASE%\stub.exe" ren "%RELEASE%\stub.exe" XiaoACTUI.exe

:: Move DLLs and stdlib .pyd into runtime/
:: Python runtime DLLs (python311/xactrt11) must be in BOTH root AND runtime/
:: because .pyd modules in runtime/ need them in their DLL search path
for %%F in ("%RELEASE%\*.dll") do (
    set "FN=%%~nxF"
    if /i "!FN!"=="python311.dll" (
        copy /y "%%~F" "%RT%\" >nul
    ) else if /i "!FN!"=="python3.dll" (
        copy /y "%%~F" "%RT%\" >nul
    ) else if /i "!FN!"=="xactrt11.dll" (
        copy /y "%%~F" "%RT%\" >nul
    ) else if /i "!FN!"=="xactrt3.dll" (
        copy /y "%%~F" "%RT%\" >nul
    ) else (
        move /y "%%~F" "%RT%\" >nul 2>nul
    )
)
for %%F in ("%RELEASE%\*.pyd") do (
    move /y "%%~F" "%RT%\" >nul 2>nul
)
:: Move supporting directories into runtime/
for /d %%D in ("%RELEASE%\tcl" "%RELEASE%\tcl8" "%RELEASE%\tk" "%RELEASE%\certifi" "%RELEASE%\numpy*" "%RELEASE%\PIL" "%RELEASE%\cv2" "%RELEASE%\pygame" "%RELEASE%\google" "%RELEASE%\webview" "%RELEASE%\moderngl" "%RELEASE%\glfw" "%RELEASE%\zstandard" "%RELEASE%\bcrypt" "%RELEASE%\cryptography" "%RELEASE%\clr_loader" "%RELEASE%\pythonnet" "%RELEASE%\pyglm" "%RELEASE%\tornado" "%RELEASE%\regex" "%RELEASE%\markupsafe" "%RELEASE%\jaraco" "%RELEASE%\glcontext" "%RELEASE%\skia*" "%RELEASE%\windows_capture") do (
    if exist "%%~D" move /y "%%~D" "%RT%\" >nul 2>nul
)

:: Copy our compiled .pyd modules into runtime/
echo   Copying compiled modules...
for %%F in ("%DIST%\nuitka_modules\*.pyd") do (
    copy /y "%%~F" "%RT%\" >nul
)
:: Also copy any package .pyd build artifacts
for /d %%D in ("%DIST%\nuitka_modules\*.build") do (
    :: Nuitka sometimes outputs into build dirs
)

:: Copy Cython .pyd accelerators
for %%F in (_sao_cy_uihelpers _sao_cy_memscan _sao_cy_pixels _sao_cy_wnd _sao_cy_packet _sao_cy_sr_uihelpers) do (
    if exist "%ROOT%%%F.cp311-win_amd64.pyd" copy /y "%ROOT%%%F.cp311-win_amd64.pyd" "%RT%\" >nul
)
:: mem_probe Cython
if not exist "%RT%\mem_probe" mkdir "%RT%\mem_probe"
if exist "%ROOT%mem_probe\rt_io.cp311-win_amd64.pyd" copy /y "%ROOT%mem_probe\rt_io.cp311-win_amd64.pyd" "%RT%\mem_probe\" >nul
if exist "%ROOT%mem_probe\_sao_cy_memscan.cp311-win_amd64.pyd" copy /y "%ROOT%mem_probe\_sao_cy_memscan.cp311-win_amd64.pyd" "%RT%\" >nul

:: Copy update.exe
if exist "%DIST%\update\update.exe" copy /y "%DIST%\update\update.exe" "%RELEASE%\update.exe" >nul

:: Step [7/9] Copy data + plugins
echo [7/9] Copying data and plugins...

:: web/assets (may have been included by stub, move to top level)
if exist "%RT%\web" move /y "%RT%\web" "%RELEASE%\web" >nul
if exist "%RT%\assets" move /y "%RT%\assets" "%RELEASE%\assets" >nul
:: If not included, copy from source
if not exist "%RELEASE%\web" xcopy /e /i /y /q "%ROOT%web" "%RELEASE%\web\" >nul
if not exist "%RELEASE%\assets" xcopy /e /i /y /q "%ROOT%assets" "%RELEASE%\assets\" >nul

:: Plugins (source .py, not compiled)
if exist "%ROOT%plugins" (
    xcopy /e /i /y /q "%ROOT%plugins" "%RELEASE%\plugins\" >nul
    :: Remove dev artifacts from plugins
    for /r "%RELEASE%\plugins" %%F in (*.pyc *.pdb *.lib *.h *.c *.pyx) do del /f "%%F" 2>nul
    for /d /r "%RELEASE%\plugins" %%D in (__pycache__ out bin _cache) do (
        if exist "%%D" rmdir /s /q "%%D" 2>nul
    )
)

:: Plugin Cython .pyd
for /r "%ROOT%plugins" %%F in (_sao_cy*.pyd) do (
    set "REL=%%~dpF"
    set "REL=!REL:%ROOT%=!"
    if not exist "%RELEASE%\!REL!" mkdir "%RELEASE%\!REL!"
    copy /y "%%F" "%RELEASE%\!REL!" >nul
)

:: locale, proto, docs
if exist "%ROOT%locale" xcopy /e /i /y /q "%ROOT%locale" "%RELEASE%\locale\" >nul
if exist "%ROOT%proto" xcopy /e /i /y /q "%ROOT%proto" "%RELEASE%\proto\" >nul
if exist "%ROOT%docs" xcopy /e /i /y /q "%ROOT%docs" "%RELEASE%\docs\" >nul
if not exist "%RELEASE%\user_plugins" mkdir "%RELEASE%\user_plugins"
if exist "%ROOT%icon.ico" copy /y "%ROOT%icon.ico" "%RELEASE%\icon.ico" >nul

:: Step [8/9] Post-build hardening
echo [8/9] Post-build hardening...
:: Harden entire release tree (DLLs in root + runtime/ .pyd + strings + Tk)
python post_build_harden.py "%RELEASE%"
if errorlevel 1 (
    echo WARNING: Hardening failed, continuing without
)
:: Also scrub the main EXE
python -c "from post_build_harden import _scrub_exe; from pathlib import Path; _scrub_exe(Path(r'%RELEASE%\XiaoACTUI.exe'))"

:: Step [9/9] Verify
echo [9/9] Verifying...
python -c "import os; data=open(r'%RELEASE%\XiaoACTUI.exe','rb').read(); print(f'  EXE: {len(data)//1024}KB, python311={data.count(b\"python311\")}x')"
python -c "import os; d=r'%RT%'; data=open(os.path.join(d,'xactrt11.dll'),'rb').read() if os.path.exists(os.path.join(d,'xactrt11.dll')) else b''; print(f'  DLL: python311={data.count(b\"python311\")}x Py_Init={data.count(b\"Py_Init\")}x')"

echo.
echo   Layout:
dir /b "%RELEASE%"
echo.
echo   runtime/ modules:
dir /b "%RT%\*.pyd" 2>nul | find /c ".pyd"
echo   .pyd files in runtime/

echo.
echo ══════════════════════════════════════════
echo   MODULAR BUILD COMPLETE
echo   Output: %RELEASE%
echo ══════════════════════════════════════════
popd
exit /b 0

:: ── Single module rebuild (for dev_publish) ──
:single_module
echo Rebuilding module: %SINGLE_MODULE%

:: Check if it's a package or standalone module
if exist "%ROOT%%SINGLE_MODULE%\__init__.py" (
    echo   Package: %SINGLE_MODULE%/
    python -m nuitka --module --assume-yes-for-downloads --no-prefer-source-code --output-dir="%DIST%\nuitka_modules" --jobs=4 "%SINGLE_MODULE%/"
) else if exist "%ROOT%%SINGLE_MODULE%.py" (
    echo   Module: %SINGLE_MODULE%.py
    python -m nuitka --module --assume-yes-for-downloads --no-prefer-source-code --output-dir="%DIST%\nuitka_modules" --jobs=4 "%SINGLE_MODULE%.py"
) else (
    echo   ERROR: %SINGLE_MODULE% not found
    goto :fail
)

:: Copy updated .pyd to release
for %%F in ("%DIST%\nuitka_modules\%SINGLE_MODULE%*.pyd") do (
    copy /y "%%~F" "%RT%\" >nul
    echo   Updated: %%~nxF
)

:: Re-harden the new .pyd (rename imports to match xactrt11.dll)
python -c "from post_build_harden import patch_imports, patch_import_names; from pathlib import Path; import glob; [patch_imports(Path(f), {b'python311.dll': b'xactrt11.dll', b'python3.dll': b'xactrt3.dll'}) for f in glob.glob(str(Path(r'%RT%') / '%SINGLE_MODULE%*.pyd'))]"

echo   Module rebuild complete.
popd
exit /b 0

:fail
echo.
echo BUILD FAILED
popd
exit /b 1
