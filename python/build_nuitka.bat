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

:: ---- [0/7] Roslyn ----
echo [0/7] Ensuring Roslyn C# compiler DLLs...
if not exist "%ROOT%act_platform\scripting\roslyn\Microsoft.CodeAnalysis.CSharp.dll" (
    python -m act_platform.scripting.fetch_roslyn
    if errorlevel 1 (
        echo WARNING: Roslyn fetch failed, .cs plugin compilation will need .NET SDK
    )
) else (
    echo   OK: Roslyn DLLs already present
)

:: ---- [1/7] Cython ----
echo [1/7] Building Cython accelerators...
python build_cython_ext.py build_ext --inplace
if errorlevel 1 goto :fail

:: ---- [2/7] C launcher ----
echo [2/7] Compiling C launcher (no CRT, KERNEL32 only)...
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

:: ---- [3/7] Nuitka ----
echo [3/7] Nuitka compilation...
if exist "%NUITKA_OUT%" rmdir /s /q "%NUITKA_OUT%"

:: AI Editor real terminal PTY (pywinpty, optional dependency). Only the
:: Python modules need explicit inclusion (imported conditionally, so the
:: import follower misses them). The four native binaries (conpty.dll,
:: winpty.dll, OpenConsole.exe, winpty-agent.exe) are auto-bundled by
:: Nuitka's dll-files plugin (built-in winpty package config); listing them
:: again via --include-data-files makes the build fail with a conflict.
:: cv2's opencv_videoio ffmpeg DLL is covered the same way.
set "WINPTY_NUITKA_FLAGS="
set "WINPTY_DIR="
for /f "usebackq tokens=*" %%W in (`python -c "import importlib.util as u,os; s=u.find_spec('winpty'); print(os.path.dirname(s.origin)) if s and s.origin else print('')" 2^>nul`) do set "WINPTY_DIR=%%W"
if defined WINPTY_DIR if not "%WINPTY_DIR%"=="" (
    echo   pywinpty found, including winpty modules
    set "WINPTY_NUITKA_FLAGS=--include-module=winpty --include-module=winpty.ptyprocess --include-module=winpty.enums"
) else (
    echo   pywinpty not installed - this build will fall back to pipe-mode terminal only
)

:: Localized MSVC (Chinese-only clui resources) emits diagnostics in the
:: console codepage; under chcp 65001 that is UTF-8, which Nuitka's scons
:: decodes as mbcs (cp936) and crashes with UnicodeDecodeError. Match the
:: console CP to the ANSI CP for the compile, restore afterwards.
chcp 936 >nul 2>&1
python -m nuitka --standalone --assume-yes-for-downloads --windows-console-mode=disable --windows-icon-from-ico=icon.ico --windows-uac-admin --output-dir=%DIST%\nuitka --output-filename=XiaoACTUI.exe --no-prefer-source-code --no-deployment-flag=excluded-module-usage --enable-plugin=tk-inter --include-package=gui_modules --include-package=utils --include-package=render --include-package=updater --include-package=sao_theme --include-package=act_platform --include-package=ui_gpu --include-package=ai_editor --include-package=workshop --include-package=license --include-package=mem_probe --include-module=sao_gui --include-module=sao_webview --include-module=sao_web_panel_common --include-package=google.protobuf --include-package=clr_loader --include-module=clr --include-module=pythonnet --include-module=pygame --include-module=pygame.mixer --include-module=pygame._sdl2 --include-module=cv2 --include-module=PIL.Image --include-module=PIL.ImageDraw --include-module=PIL.ImageFont --include-module=PIL.ImageFilter --include-module=mss --include-module=mss.windows --include-module=windows_capture --include-module=pynput --include-module=pynput.keyboard --include-module=pynput.keyboard._win32 --include-module=pynput.mouse --include-module=pynput.mouse._win32 --include-module=moderngl --include-module=moderngl_window --include-module=moderngl_window.context.headless --include-module=skia --include-module=zstandard --include-package=lupa --include-data-files=act_platform/scripting/roslyn/*.dll=act_platform/scripting/roslyn/ %WINPTY_NUITKA_FLAGS% --nofollow-import-to=httpx._main --nofollow-import-to=matplotlib --nofollow-import-to=scipy --nofollow-import-to=pandas --nofollow-import-to=torch --nofollow-import-to=tensorflow --nofollow-import-to=test --nofollow-import-to=unittest --nofollow-import-to=xmlrpc --nofollow-import-to=doctest --nofollow-import-to=pydoc --nofollow-import-to=webview.platforms.android --nofollow-import-to=webview.platforms.gtk --nofollow-import-to=webview.platforms.cocoa --nofollow-import-to=webview.platforms.qt --nofollow-import-to=ai_editor.selftest --jobs=8 main.py
set "NUITKA_RC=%errorlevel%"
chcp 65001 >nul 2>&1
if not "%NUITKA_RC%"=="0" (
    echo ERROR: Nuitka compilation failed
    goto :fail
)

:: ---- [4/7] Assemble ----
echo [4/7] Assembling release layout...
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

:: ---- [5/7] Harden ----
echo [5/7] Post-build hardening (runtime/)...
python post_build_harden.py "%RELEASE%\runtime"

:: ---- [6/7] Verify ----
echo [6/7] Roslyn in distribution check...
if exist "%RELEASE%\runtime\act_platform\scripting\roslyn\Microsoft.CodeAnalysis.CSharp.dll" (
    echo   OK: Roslyn compiler bundled
) else (
    echo   WARNING: Roslyn DLLs not found in distribution
)
if defined WINPTY_DIR if not "%WINPTY_DIR%"=="" (
    if exist "%RELEASE%\runtime\winpty\conpty.dll" (
        echo   OK: ConPTY binaries bundled
    ) else (
        echo   WARNING: winpty native binaries missing - PTY terminal will fall back to pipe mode
    )
)

:: ---- [7/7] Verify ----
echo [7/7] Verify
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
