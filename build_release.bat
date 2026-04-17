@echo off
setlocal

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST_DIR=%ROOT%dist"
set "RELEASE_DIR=%DIST_DIR%\release"
set "CLIENT_DIR=%RELEASE_DIR%\XiaoACTUI"
set "SERVER_DIR=%RELEASE_DIR%\AutoKeyServer"

echo [1/4] Building XiaoACTUI.exe...
pyinstaller --clean --noconfirm XiaoACTUI.spec
if errorlevel 1 goto :fail

echo [2/4] Building AutoKeyServer.exe...
pyinstaller --clean --noconfirm server\AutoKeyServer.spec
if errorlevel 1 goto :fail

echo [3/4] Preparing release folders...
if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%CLIENT_DIR%"
mkdir "%CLIENT_DIR%\exports\auto_keys"
mkdir "%CLIENT_DIR%\exports\boss_raids"
mkdir "%CLIENT_DIR%\temp"
mkdir "%SERVER_DIR%"
mkdir "%SERVER_DIR%\data"

echo [4/4] Copying binaries and docs...
copy /y "%DIST_DIR%\XiaoACTUI.exe" "%CLIENT_DIR%\XiaoACTUI.exe" >nul
copy /y "%DIST_DIR%\AutoKeyServer.exe" "%SERVER_DIR%\AutoKeyServer.exe" >nul
copy /y "%ROOT%README.md" "%CLIENT_DIR%\README.md" >nul
copy /y "%ROOT%LICENSE" "%CLIENT_DIR%\LICENSE" >nul
copy /y "%ROOT%Start.bat" "%CLIENT_DIR%\Start.bat" >nul
copy /y "%ROOT%LICENSE" "%SERVER_DIR%\LICENSE" >nul

echo Release created at:
echo   %RELEASE_DIR%
popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
echo Build failed with exit code %ERR%.
popd
exit /b %ERR%