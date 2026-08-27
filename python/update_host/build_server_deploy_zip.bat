@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

REM 打包 update_host 的"纯源码部署包"(不是 exe)。原因: build_service.py 需要
REM 拉起一个真正的 python.exe 去跑 setup.py/Cython 编译插件，冻结成 exe 之后
REM sys.executable 会变成 exe 自己，那条子进程编译链路就废了。所以服务器侧
REM 老实跑源码 + venv，用 start_server.bat 一键装依赖+启动。

set "ROOT=%~dp0"
pushd "%ROOT%"
set "UPDATE_HOST_TEST_ENV="
set "UPDATE_HOST_REQUIRE_TLS=1"

set "OUT_DIR=%ROOT%..\dist\update_host_deploy"
set "ZIP_PATH=%ROOT%..\dist\update_host_server_deploy.zip"

set "CERT_SOURCE=%UPDATE_HOST_SSL_CERTFILE%"
if not defined CERT_SOURCE set "CERT_SOURCE=%ROOT%..\license_server\server.crt"
set "KEY_SOURCE=%UPDATE_HOST_SSL_KEYFILE%"
if not defined KEY_SOURCE set "KEY_SOURCE=%ROOT%..\license_server\server.key"
set "CERT_SOURCE=%CERT_SOURCE:"=%"
set "KEY_SOURCE=%KEY_SOURCE:"=%"
for %%I in ("%CERT_SOURCE%") do set "CERT_SOURCE=%%~fI"
for %%I in ("%KEY_SOURCE%") do set "KEY_SOURCE=%%~fI"
if not exist "%CERT_SOURCE%" (
    echo [错误] 缺少 TLS 自签证书: %CERT_SOURCE%
    popd
    exit /b 1
)
if not exist "%KEY_SOURCE%" (
    echo [错误] 缺少 TLS 自签私钥: %KEY_SOURCE%
    popd
    exit /b 1
)

if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"
echo [1/4] 校验 TLS 证书 SPKI pin (d317)...
python -c "import sys; sys.path.insert(0, r'%ROOT%..'); from tls_pinning import validate_certificate_file; validate_certificate_file(r'%CERT_SOURCE%')"
if errorlevel 1 (
    echo [错误] TLS 证书 SPKI pin 校验失败，停止打包且不生成 zip。
    popd
    exit /b 1
)

echo [2/4] 清理旧的打包目录 ...
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%\update_host"
mkdir "%OUT_DIR%\license_server"

echo [3/4] 拷贝服务端文件 ...
set "MISSING=0"
call :copyrootfile server_service.py
call :copyrootfile tls_pinning.py
call :copyfile app.py

call :copyfile build_service.py
call :copyfile toolchain_bootstrap.py
call :copyfile native_crypto.py
call :copyfile update_host_main.py
call :copyfile publish_release.py
call :copyfile requirements.txt
call :copyfile start_server.bat
call :copyfile run_update_host.bat
call :copyfile README.md

if "%MISSING%"=="1" (
    echo 打包中止：上面列出的文件缺失。
    popd
    exit /b 1
)

copy /y "%CERT_SOURCE%" "%OUT_DIR%\license_server\server.crt" >nul
if errorlevel 1 goto :fail
copy /y "%KEY_SOURCE%" "%OUT_DIR%\license_server\server.key" >nul
if errorlevel 1 goto :fail

REM 故意不打包本机的 releases/ 目录：那是发布数据(可能好几百 MB)，而且如果
REM 你部署的目标是已经在跑的生产服务器，本机这份多半是过时快照，解压覆盖
REM 上去会把服务器上真实的发布数据冲掉。只建空目录，发布走 publish_release.py。
mkdir "%OUT_DIR%\update_host\releases" >nul

if exist "%OUT_DIR%\update_host\update_host_config.json" del /q "%OUT_DIR%\update_host\update_host_config.json"

echo [4/4] 打 zip ...
tar.exe -a -cf "%ZIP_PATH%" -C "%OUT_DIR%" "server_service.py" "update_host" "license_server"
if errorlevel 1 goto :fail
if not exist "%ZIP_PATH%" goto :fail

echo.
echo 完成: %ZIP_PATH%
echo.
echo 部署方法:
echo   1. 把这个 zip 传到服务器，解压
echo   2. 服务器上运行 update_host\start_server.bat (双击或命令行)，或用根目录 server_service.py 托管 SCM
echo      第一次运行会自动装 Python venv 依赖 (fastapi/uvicorn)
echo      第一次有人上传"非开源"插件时，会自动把 Cython/MSVC BuildTools
echo      静默装到 D:\Xiaoworkshop (可能耗时较久，属于一次性成本)
echo   3. 确认服务端监听 9973，且端口映射已将 x2.sjcmc.cn:15018 转发到 9973

popd
exit /b 0

:copyfile
if not exist "%ROOT%%~1" (
    echo [错误] 缺文件: %~1
    set "MISSING=1"
    goto :eof
)
copy /y "%ROOT%%~1" "%OUT_DIR%\update_host\%~1" >nul
goto :eof

:copyrootfile
if not exist "%ROOT%..\%~1" (
    echo [错误] 缺文件: %~1
    set "MISSING=1"
    goto :eof
)
copy /y "%ROOT%..\%~1" "%OUT_DIR%\%~1" >nul
goto :eof

:fail
set "ERR=%ERRORLEVEL%"
echo 打包失败, exit code %ERR%.
popd
exit /b %ERR%
