@echo off
setlocal enabledelayedexpansion

:: ──────────────────────────────────────────────────────────
::  SAO-UI Nuitka Build — one-click full pipeline
::
::  Output: dist\release\XiaoACTUI\ (flat layout, hardened)
::  Usage:  build_nuitka.bat
:: ──────────────────────────────────────────────────────────

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST=%ROOT%dist"
set "NUITKA_OUT=%DIST%\nuitka\main.dist"
set "RELEASE=%DIST%\release\XiaoACTUI"

echo [1/5] Building Cython accelerators...
python build_cython_ext.py build_ext --inplace
if errorlevel 1 goto :fail

echo [2/5] Nuitka compilation...
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

echo [3/5] Assembling release...
if exist "%RELEASE%" rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"
mkdir "%RELEASE%\temp"
mkdir "%RELEASE%\user_plugins"
xcopy /e /i /y /q "%NUITKA_OUT%\*" "%RELEASE%\" >nul
if exist "%ROOT%plugins" xcopy /e /i /y /q "%ROOT%plugins" "%RELEASE%\plugins\" >nul
if exist "%ROOT%locale" xcopy /e /i /y /q "%ROOT%locale" "%RELEASE%\locale\" >nul
if exist "%ROOT%docs" xcopy /e /i /y /q "%ROOT%docs" "%RELEASE%\docs\" >nul
if exist "%ROOT%icon.ico" copy /y "%ROOT%icon.ico" "%RELEASE%\" >nul

echo [4/5] Post-build hardening...
python post_build_harden.py "%RELEASE%"

echo [5/5] Done.
echo   Output: %RELEASE%
dir /b "%RELEASE%\XiaoACTUI.exe"

popd
exit /b 0

:fail
echo BUILD FAILED
popd
exit /b 1
