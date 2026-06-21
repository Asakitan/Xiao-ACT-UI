@echo off
chcp 65001 >nul 2>&1
setlocal enabledelayedexpansion

:: SAO-UI Nuitka Build -- one-click full pipeline
::
:: Layout:
::   dist\release\XiaoACTUI\
::     XiaoACTUI.exe          C launcher (10KB, no CRT)
::     runtime\XiaoACTUI.exe  Nuitka main EXE
::     web\ assets\ plugins\ locale\ docs\
::
:: Usage:  build_nuitka.bat [--clean]

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST=%ROOT%dist"
set "NUITKA_OUT=%DIST%\nuitka\main.dist"
set "NUITKA_BUILD=%DIST%\nuitka\main.build"
set "RELEASE=%DIST%\release\XiaoACTUI"
set "LAUNCHER_OUT=%DIST%\launcher"

if "%~1"=="--clean" (
    echo [CLEAN] Clearing Nuitka C cache...
    if exist "%DIST%\nuitka" rmdir /s /q "%DIST%\nuitka"
    echo   Cache cleared.
)

:: ---- [1/6] Cython ----
echo [1/6] Building Cython accelerators...
python build_cython_ext.py build_ext --inplace
if errorlevel 1 goto :fail

:: ---- [2/6] C launcher ----
echo [2/6] Compiling C launcher (no CRT, KERNEL32 only)...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found
    goto :fail
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo ERROR: Visual Studio not found
    goto :fail
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if not exist "%LAUNCHER_OUT%" mkdir "%LAUNCHER_OUT%"
rc /nologo launcher.rc
if errorlevel 1 goto :fail
cl /nologo /O2 /GS- launcher.c launcher.res /Fe:"%LAUNCHER_OUT%\XiaoACTUI.exe" /link kernel32.lib
if errorlevel 1 goto :fail
del /q launcher.res launcher.obj >nul 2>&1
echo   OK: %LAUNCHER_OUT%\XiaoACTUI.exe

:: ---- [3/6] Nuitka ----
echo [3/6] Nuitka compilation...
if exist "%NUITKA_OUT%" rmdir /s /q "%NUITKA_OUT%"
python -m nuitka --standalone --assume-yes-for-downloads --windows-console-mode=disable --windows-icon-from-ico=icon.ico --windows-uac-admin --output-dir=%DIST%\nuitka --output-filename=XiaoACTUI.exe --no-prefer-source-code --no-deployment-flag=excluded-module-usage --enable-plugin=tk-inter --include-package=gui_modules --include-package=utils --include-package=render --include-package=updater --include-package=sao_theme --include-package=act_platform --include-package=ui_gpu --include-package=ai_editor --include-package=workshop --include-package=license --include-package=mem_probe --include-module=sao_gui --include-module=sao_webview --include-module=sao_web_panel_common --include-package=google.protobuf --include-package=clr_loader --include-module=clr --include-module=pythonnet --include-module=pygame --include-module=pygame.mixer --include-module=pygame._sdl2 --include-module=cv2 --include-module=PIL.Image --include-module=PIL.ImageDraw --include-module=PIL.ImageFont --include-module=PIL.ImageFilter --include-module=mss --include-module=mss.windows --include-module=windows_capture --include-module=pynput --include-module=pynput.keyboard --include-module=pynput.keyboard._win32 --include-module=pynput.mouse --include-module=pynput.mouse._win32 --include-module=moderngl --include-module=moderngl_window --include-module=moderngl_window.context.glfw --include-module=moderngl_window.context.headless --include-module=glfw --include-module=skia --include-module=zstandard --nofollow-import-to=matplotlib --nofollow-import-to=scipy --nofollow-import-to=pandas --nofollow-import-to=torch --nofollow-import-to=tensorflow --nofollow-import-to=test --nofollow-import-to=unittest --nofollow-import-to=xmlrpc --nofollow-import-to=doctest --nofollow-import-to=pydoc --nofollow-import-to=webview.platforms.android --nofollow-import-to=webview.platforms.gtk --nofollow-import-to=webview.platforms.cocoa --nofollow-import-to=webview.platforms.qt --jobs=8 main.py
if errorlevel 1 (
    echo ERROR: Nuitka compilation failed
    goto :fail
)

:: ---- [4/6] Assemble ----
echo [4/6] Assembling release layout...
if exist "%RELEASE%" rmdir /s /q "%RELEASE%"
mkdir "%RELEASE%"
mkdir "%RELEASE%\runtime"
mkdir "%RELEASE%\temp"
mkdir "%RELEASE%\user_plugins"

copy /y "%LAUNCHER_OUT%\XiaoACTUI.exe" "%RELEASE%\linkstart.exe" >nul
xcopy /e /i /y /q "%NUITKA_OUT%\*" "%RELEASE%\runtime\" >nul

if exist "%RELEASE%\runtime\web" (
    xcopy /e /i /y /q "%RELEASE%\runtime\web" "%RELEASE%\web\" >nul
    rmdir /s /q "%RELEASE%\runtime\web"
)
if exist "%RELEASE%\runtime\assets" (
    xcopy /e /i /y /q "%RELEASE%\runtime\assets" "%RELEASE%\assets\" >nul
    rmdir /s /q "%RELEASE%\runtime\assets"
)

if exist "%ROOT%web" xcopy /e /i /y /q "%ROOT%web" "%RELEASE%\web\" >nul
if exist "%ROOT%assets" xcopy /e /i /y /q "%ROOT%assets" "%RELEASE%\assets\" >nul
if exist "%ROOT%plugins" xcopy /e /i /y /q "%ROOT%plugins" "%RELEASE%\plugins\" >nul
if exist "%ROOT%locale" xcopy /e /i /y /q "%ROOT%locale" "%RELEASE%\locale\" >nul
if exist "%ROOT%docs" xcopy /e /i /y /q "%ROOT%docs" "%RELEASE%\docs\" >nul
if exist "%ROOT%icon.ico" copy /y "%ROOT%icon.ico" "%RELEASE%\" >nul

:: ---- [5/6] Harden ----
echo [5/6] Post-build hardening (runtime/)...
python post_build_harden.py "%RELEASE%\runtime"

:: ---- [6/6] Verify ----
echo [6/6] Verify
echo.
for %%F in ("%RELEASE%\linkstart.exe") do echo   Launcher: %%~zF bytes
for %%F in ("%RELEASE%\runtime\XiaoACTUI.exe") do echo   Runtime:  %%~zF bytes
echo.
echo BUILD OK
popd
exit /b 0

:fail
echo BUILD FAILED
popd
exit /b 1
