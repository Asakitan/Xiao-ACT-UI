@echo off
setlocal

set "ROOT=%~dp0"
pushd "%ROOT%"

if not defined UPDATE_HOST_RELEASE_DIR set "UPDATE_HOST_RELEASE_DIR=%ROOT%releases"
if not defined UPDATE_HOST_DOWNLOADS set "UPDATE_HOST_DOWNLOADS=%UPDATE_HOST_RELEASE_DIR%"
if not defined UPDATE_HOST_PORT set "UPDATE_HOST_PORT=9973"
if not defined UPDATE_HOST_HOST set "UPDATE_HOST_HOST=0.0.0.0"
set "UPDATE_HOST_TEST_ENV="
set "UPDATE_HOST_REQUIRE_TLS=1"

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

REM canonical Workshop routes live in app.py. workshop_routes.py is a deprecated
REM compatibility module and is intentionally not mounted by the service.
set "PYTHONPATH=%ROOT%..;%PYTHONPATH%"

python update_host_main.py

popd
exit /b %ERRORLEVEL%
