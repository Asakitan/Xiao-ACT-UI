@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

set "ROOT=%~dp0"
pushd "%ROOT%"

if not defined UPDATE_HOST_RELEASE_DIR set "UPDATE_HOST_RELEASE_DIR=%ROOT%releases"
if not defined UPDATE_HOST_DOWNLOADS set "UPDATE_HOST_DOWNLOADS=%UPDATE_HOST_RELEASE_DIR%"
if not defined UPDATE_HOST_PORT set "UPDATE_HOST_PORT=9973"
if not defined UPDATE_HOST_HOST set "UPDATE_HOST_HOST=0.0.0.0"
set "UPDATE_HOST_TEST_ENV="
set "UPDATE_HOST_REQUIRE_TLS=1"

echo ============================================================
echo  SAO Auto Update Host - 一键启动, 自动装依赖
echo ============================================================

REM ── [1/4] 找系统 Python（只用来创建 venv，不直接跑服务） ──
set "PY_LAUNCHER="
where py >nul 2>nul
if !ERRORLEVEL!==0 (
    set "PY_LAUNCHER=py -3"
) else (
    where python >nul 2>nul
    if !ERRORLEVEL!==0 (
        set "PY_LAUNCHER=python"
    )
)

if not defined PY_LAUNCHER (
    echo [1/4] 本机没有 Python，尝试用 winget 静默安装 Python 3.11 ...
    where winget >nul 2>nul
    if !ERRORLEVEL! NEQ 0 (
        echo.
        echo [错误] 本机既没有 Python 也没有 winget，无法自动安装。
        echo 请手动安装 64 位版本的 Python 3.11 或更高版本，安装时记得勾选 Add to PATH，然后重新运行本脚本。
        echo 下载地址: https://www.python.org/downloads/
        popd
        exit /b 1
    )
    winget install --id Python.Python.3.11 --silent --accept-package-agreements --accept-source-agreements
    if !ERRORLEVEL! NEQ 0 (
        echo [错误] winget 静默安装 Python 失败，请手动安装后重试。
        popd
        exit /b 1
    )
    REM winget 装完当前 shell 的 PATH 不会自动刷新，重新探测一次常见路径
    where py >nul 2>nul
    if !ERRORLEVEL!==0 (
        set "PY_LAUNCHER=py -3"
    ) else (
        echo [错误] Python 装好了但当前终端还没刷新 PATH，请关闭这个窗口重新打开一个再运行本脚本。
        popd
        exit /b 1
    )
)
echo [1/4] 系统 Python: %PY_LAUNCHER%

REM ── [2/4] 建 venv（幂等，已存在就跳过） ──
if not exist "%ROOT%.venv\Scripts\python.exe" (
    echo [2/4] 首次运行：创建独立虚拟环境 .venv ...
    %PY_LAUNCHER% -m venv "%ROOT%.venv"
    if !ERRORLEVEL! NEQ 0 (
        echo [错误] venv 创建失败。
        popd
        exit /b 1
    )
) else (
    echo [2/4] .venv 已存在，跳过创建
)
set "VENV_PY=%ROOT%.venv\Scripts\python.exe"

REM ── [3/4] 装依赖（pip 对已满足的包是几乎零成本的 no-op，可放心每次都跑） ──
echo [3/4] 检查/安装依赖 (fastapi/uvicorn) ...
"%VENV_PY%" -m pip install --no-input --disable-pip-version-check --quiet --upgrade pip
"%VENV_PY%" -m pip install --no-input --disable-pip-version-check -r "%ROOT%requirements.txt"
if !ERRORLEVEL! NEQ 0 (
    echo [错误] 依赖安装失败，看看上面的 pip 报错。
    popd
    exit /b 1
)

REM ── [4/4] 启动 ──
REM canonical Workshop routes live in app.py. workshop_routes.py is a deprecated
REM compatibility module and is intentionally not mounted by the service.
set "PYTHONPATH=%ROOT%..;%PYTHONPATH%"

echo [4/4] 启动服务
echo   release_dir = %UPDATE_HOST_RELEASE_DIR%
echo   downloads   = %UPDATE_HOST_DOWNLOADS%
echo   bind        = %UPDATE_HOST_HOST%:%UPDATE_HOST_PORT%
echo   闭源插件自动构建工具链 Cython/MSVC 首次触发时会另外静默装到 D:\Xiaoworkshop
echo     可用环境变量 XIAOWORKSHOP_ROOT 覆盖这个路径
echo ============================================================

"%VENV_PY%" update_host_main.py

popd
exit /b %ERRORLEVEL%
