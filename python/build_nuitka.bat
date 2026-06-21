@echo off
setlocal enabledelayedexpansion

:: ──────────────────────────────────────────────────────────
::  SAO-UI Nuitka Build Pipeline
::
::  Compiles the entire application into a single native EXE
::  then applies post-build hardening (DLL rename + Tk class).
::  Output: dist\release\XiaoACTUI\ (same path as PyInstaller)
:: ──────────────────────────────────────────────────────────

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST=%ROOT%dist"
set "NUITKA_OUT=%DIST%\nuitka\main.dist"
set "RELEASE=%DIST%\release\XiaoACTUI"

:: ── Step [1/6] Cython accelerators ──
echo [1/6] Building Cython accelerators...
python build_cython_ext.py build_ext --inplace
if errorlevel 1 goto :fail

:: ── Step [2/6] Encrypt backend data ──
echo [2/6] Encrypting backend data...
if exist "%ROOT%mem_probe\_encrypt_drivers.py" (
    python "%ROOT%mem_probe\_encrypt_drivers.py" 2>nul
)

:: ── Step [3/6] Nuitka full compilation ──
echo [3/6] Nuitka compilation (uses C cache if available)...
if exist "%NUITKA_OUT%" rmdir /s /q "%NUITKA_OUT%"

python -m nuitka ^
    --standalone ^
    --assume-yes-for-downloads ^
    --windows-console-mode=disable ^
    --windows-icon-from-ico=icon.ico ^
    --windows-uac-admin ^
    --output-dir=%DIST%\nuitka ^
    --output-filename=XiaoACTUI.exe ^
    --no-prefer-source-code ^
    --enable-plugin=tk-inter ^
    --include-package=gui_modules ^
    --include-package=utils ^
    --include-package=render ^
    --include-package=updater ^
    --include-package=sao_theme ^
    --include-package=act_platform ^
    --include-package=ui_gpu ^
    --include-package=ai_editor ^
    --include-package=workshop ^
    --include-package=license ^
    --include-package=mem_probe ^
    --include-module=sao_gui ^
    --include-module=sao_webview ^
    --include-module=sao_web_panel_common ^
    --include-package=google.protobuf ^
    --include-package=clr_loader ^
    --include-module=clr ^
    --include-module=pythonnet ^
    --include-module=pygame ^
    --include-module=pygame.mixer ^
    --include-module=pygame._sdl2 ^
    --include-module=cv2 ^
    --include-module=PIL.Image ^
    --include-module=PIL.ImageDraw ^
    --include-module=PIL.ImageFont ^
    --include-module=PIL.ImageFilter ^
    --include-module=mss ^
    --include-module=mss.windows ^
    --include-module=windows_capture ^
    --include-module=pynput ^
    --include-module=pynput.keyboard ^
    --include-module=pynput.keyboard._win32 ^
    --include-module=pynput.mouse ^
    --include-module=pynput.mouse._win32 ^
    --include-module=moderngl ^
    --include-module=moderngl_window ^
    --include-module=moderngl_window.context.glfw ^
    --include-module=moderngl_window.context.headless ^
    --include-module=glfw ^
    --include-module=skia ^
    --include-module=zstandard ^
    --nofollow-import-to=matplotlib ^
    --nofollow-import-to=scipy ^
    --nofollow-import-to=pandas ^
    --nofollow-import-to=torch ^
    --nofollow-import-to=tensorflow ^
    --nofollow-import-to=test ^
    --nofollow-import-to=unittest ^
    --nofollow-import-to=xmlrpc ^
    --nofollow-import-to=doctest ^
    --nofollow-import-to=pydoc ^
    --nofollow-import-to=webview.platforms.android ^
    --nofollow-import-to=webview.platforms.gtk ^
    --nofollow-import-to=webview.platforms.cocoa ^
    --nofollow-import-to=webview.platforms.qt ^
    --include-data-dir=web=web ^
    --include-data-dir=assets=assets ^
    --jobs=8 ^
    main.py
if errorlevel 1 (
    echo ERROR: Nuitka compilation failed
    goto :fail
)

:: ── Step [4/6] Assemble release layout ──
echo [4/6] Assembling release layout...
if exist "%RELEASE%" rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"

:: Copy Nuitka output to release
xcopy /e /i /y /q "%NUITKA_OUT%\*" "%RELEASE%\" >nul

:: Copy plugins (source .py, loaded dynamically)
if exist "%ROOT%plugins" (
    xcopy /e /i /y /q "%ROOT%plugins" "%RELEASE%\plugins\" >nul
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

:: Extras
if exist "%ROOT%locale" xcopy /e /i /y /q "%ROOT%locale" "%RELEASE%\locale\" >nul
if exist "%ROOT%docs" xcopy /e /i /y /q "%ROOT%docs" "%RELEASE%\docs\" >nul
if not exist "%RELEASE%\user_plugins" mkdir "%RELEASE%\user_plugins"
if not exist "%RELEASE%\temp" mkdir "%RELEASE%\temp"

:: ── Step [5/6] Post-build hardening ──
echo [5/6] Post-build hardening (DLL rename + Tk class)...
python post_build_harden.py "%RELEASE%"
if errorlevel 1 (
    echo WARNING: Hardening had issues, continuing
)

:: ── Step [6/6] Verify ──
echo [6/6] Verifying...
echo   Checking module names...
python -c "import os; [print(f'  FOUND: {f}') for f in os.listdir(r'%RELEASE%') if 'python' in f.lower()]"
echo   Layout:
dir /b "%RELEASE%"

echo.
echo ══════════════════════════════════════
echo   BUILD COMPLETE
echo   Output: %RELEASE%
echo ══════════════════════════════════════

popd
exit /b 0

:fail
echo.
echo BUILD FAILED
popd
exit /b 1
