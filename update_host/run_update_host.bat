@echo off
setlocal

set "ROOT=%~dp0"
pushd "%ROOT%"

if not defined UPDATE_HOST_RELEASE_DIR set "UPDATE_HOST_RELEASE_DIR=%ROOT%releases"
if not defined UPDATE_HOST_DOWNLOADS set "UPDATE_HOST_DOWNLOADS=%UPDATE_HOST_RELEASE_DIR%"
if not defined UPDATE_HOST_PORT set "UPDATE_HOST_PORT=9330"
if not defined UPDATE_HOST_HOST set "UPDATE_HOST_HOST=0.0.0.0"

if exist "%ROOT%UpdateHost.exe" (
    echo Starting SAO Auto Update Host (EXE)...
    echo   release_dir=%UPDATE_HOST_RELEASE_DIR%
    echo   downloads=%UPDATE_HOST_DOWNLOADS%
    echo   bind=%UPDATE_HOST_HOST%:%UPDATE_HOST_PORT%
    "%ROOT%UpdateHost.exe"
    popd
    exit /b %ERRORLEVEL%
)

echo Starting SAO Auto Update Host...
echo   release_dir=%UPDATE_HOST_RELEASE_DIR%
echo   downloads=%UPDATE_HOST_DOWNLOADS%
echo   bind=%UPDATE_HOST_HOST%:%UPDATE_HOST_PORT%

python -m uvicorn app:app --host %UPDATE_HOST_HOST% --port %UPDATE_HOST_PORT%

popd
exit /b %ERRORLEVEL%
