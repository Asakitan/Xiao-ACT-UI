@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

REM 打包 update_host 的"纯源码部署包"(不是 exe)。原因: build_service.py 需要
REM 拉起一个真正的 python.exe 去跑 setup.py/Cython 编译插件，冻结成 exe 之后
REM sys.executable 会变成 exe 自己，那条子进程编译链路就废了。所以服务器侧
REM 老实跑源码 + venv，用 start_server.bat 一键装依赖+启动。

set "ROOT=%~dp0"
pushd "%ROOT%"

set "OUT_DIR=%ROOT%..\dist\update_host_deploy"
set "ZIP_PATH=%ROOT%..\dist\update_host_server_deploy.zip"

echo [1/3] 清理旧的打包目录 ...
if exist "%OUT_DIR%" rmdir /s /q "%OUT_DIR%"
mkdir "%OUT_DIR%\update_host"

echo [2/3] 拷贝服务端文件 ...
set "MISSING=0"
call :copyfile app.py
call :copyfile workshop_routes.py
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

REM 故意不打包本机的 releases/ 目录：那是发布数据(可能好几百 MB)，而且如果
REM 你部署的目标是已经在跑的生产服务器，本机这份多半是过时快照，解压覆盖
REM 上去会把服务器上真实的发布数据冲掉。只建空目录，发布走 publish_release.py。
mkdir "%OUT_DIR%\update_host\releases" >nul

if exist "%OUT_DIR%\update_host\update_host_config.json" del /q "%OUT_DIR%\update_host\update_host_config.json"

echo [3/3] 打 zip ...
if exist "%ZIP_PATH%" del /q "%ZIP_PATH%"
tar.exe -a -cf "%ZIP_PATH%" -C "%OUT_DIR%" "update_host"
if errorlevel 1 goto :fail
if not exist "%ZIP_PATH%" goto :fail

echo.
echo 完成: %ZIP_PATH%
echo.
echo 部署方法:
echo   1. 把这个 zip 传到服务器，解压
echo   2. 服务器上运行 update_host\start_server.bat (双击或命令行)
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

:fail
set "ERR=%ERRORLEVEL%"
echo 打包失败, exit code %ERR%.
popd
exit /b %ERR%
