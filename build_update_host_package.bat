@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST_DIR=%ROOT%dist"
set "OUT_DIR=%DIST_DIR%\release\UpdateHost"
set "PKG_DIR=%OUT_DIR%\update_host"
set "ZIP_PATH=%OUT_DIR%\update_host_deploy.zip"
set "TEMP_ZIP=%DIST_DIR%\update_host_deploy.zip"

echo [1/5] Building UpdateHost.exe...
pyinstaller --clean --noconfirm update_host\UpdateHost.spec
if errorlevel 1 goto :fail

echo [2/5] Preparing output folders...
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%"
mkdir "%PKG_DIR%"

echo [3/5] Copying update host files...
copy /y "%DIST_DIR%\UpdateHost.exe" "%PKG_DIR%\UpdateHost.exe" >nul
copy /y "%ROOT%update_host\README.md" "%PKG_DIR%\README.md" >nul
copy /y "%ROOT%update_host\run_update_host.bat" "%PKG_DIR%\run_update_host.bat" >nul

if exist "%ROOT%update_host\releases" (
    xcopy /e /i /y "%ROOT%update_host\releases" "%PKG_DIR%\releases" >nul
) else (
    mkdir "%PKG_DIR%\releases" >nul
)

if exist "%PKG_DIR%\__pycache__" rmdir /s /q "%PKG_DIR%\__pycache__"
if exist "%PKG_DIR%\update_host_config.json" del /q "%PKG_DIR%\update_host_config.json"
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"
if exist "%TEMP_ZIP%" del /q "%TEMP_ZIP%"

echo [4/5] Creating deploy zip...
tar.exe -a -cf "%TEMP_ZIP%" -C "%OUT_DIR%" "update_host"
if errorlevel 1 goto :fail
if not exist "%TEMP_ZIP%" goto :fail
move /y "%TEMP_ZIP%" "%ZIP_PATH%" >nul

echo [5/5] Done.
echo Update host package:
echo   %ZIP_PATH%

popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
echo Packaging failed with exit code %ERR%.
popd
exit /b %ERR%
