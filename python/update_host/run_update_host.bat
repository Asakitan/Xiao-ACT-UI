@echo off
setlocal

set "ROOT=%~dp0"
pushd "%ROOT%"

if not defined UPDATE_HOST_RELEASE_DIR set "UPDATE_HOST_RELEASE_DIR=%ROOT%releases"
if not defined UPDATE_HOST_DOWNLOADS set "UPDATE_HOST_DOWNLOADS=%UPDATE_HOST_RELEASE_DIR%"
if not defined UPDATE_HOST_PORT set "UPDATE_HOST_PORT=9973"
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

REM app.py 用 "from update_host.workshop_routes import ..." 这种包限定写法，
REM update_host/ 没有 __init__.py(隐式命名空间包)，要让它解析成功，上级目录
REM 得在 sys.path 上——不补的话 workshop 路由会被 app.py 的 try/except 悄悄
REM 吞掉，服务照样能起，但 /api/workshop/* 全部 404。
set "PYTHONPATH=%ROOT%..;%PYTHONPATH%"

python -m uvicorn app:app --host %UPDATE_HOST_HOST% --port %UPDATE_HOST_PORT%

popd
exit /b %ERRORLEVEL%
