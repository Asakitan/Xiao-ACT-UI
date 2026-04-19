@echo off
setlocal enabledelayedexpansion

set "ROOT=%~dp0"
pushd "%ROOT%"

set "DIST_DIR=%ROOT%dist"
set "RELEASE_DIR=%DIST_DIR%\release"
set "CLIENT_DIR=%RELEASE_DIR%\XiaoACTUI"
set "SERVER_DIR=%RELEASE_DIR%\AutoKeyServer"

echo [1/5] Building XiaoACTUI.exe (onedir + runtime/ contents_directory)...
pyinstaller --clean --noconfirm XiaoACTUI.spec
if errorlevel 1 goto :fail

echo [2/5] Building update.exe (standalone helper)...
pyinstaller --clean --noconfirm update.spec
if errorlevel 1 goto :fail

echo [3/5] Building AutoKeyServer.exe...
pyinstaller --clean --noconfirm server\AutoKeyServer.spec
if errorlevel 1 goto :fail

echo [4/5] Preparing release folders...
if exist "%RELEASE_DIR%" rmdir /s /q "%RELEASE_DIR%"
mkdir "%RELEASE_DIR%"
mkdir "%CLIENT_DIR%"
mkdir "%CLIENT_DIR%\exports\auto_keys"
mkdir "%CLIENT_DIR%\exports\boss_raids"
mkdir "%CLIENT_DIR%\temp"
mkdir "%SERVER_DIR%"
mkdir "%SERVER_DIR%\data"

echo [5/5] Copying binaries + lifting modular folders out of runtime/...
rem onedir build: dist\XiaoACTUI\ contains XiaoACTUI.exe + runtime\ (renamed _internal)
xcopy /e /i /y "%DIST_DIR%\XiaoACTUI" "%CLIENT_DIR%" >nul

rem update.exe: copy next to main exe
copy /y "%DIST_DIR%\update.exe" "%CLIENT_DIR%\update.exe" >nul

rem Lift modular data folders OUT of runtime\ to top level so they can be updated independently
for %%D in (web assets proto) do (
    if exist "%CLIENT_DIR%\runtime\%%D" (
        echo   moving runtime\%%D -^> %%D
        move /y "%CLIENT_DIR%\runtime\%%D" "%CLIENT_DIR%\%%D" >nul
    )
)
if exist "%CLIENT_DIR%\runtime\icon.ico" (
    move /y "%CLIENT_DIR%\runtime\icon.ico" "%CLIENT_DIR%\icon.ico" >nul
)

copy /y "%DIST_DIR%\AutoKeyServer.exe" "%SERVER_DIR%\AutoKeyServer.exe" >nul

echo.
echo Release created at:
echo   %RELEASE_DIR%
echo Client layout:
echo   XiaoACTUI.exe   update.exe   web\   assets\   proto\   runtime\
popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
echo Build failed with exit code %ERR%.
popd
exit /b %ERR%
