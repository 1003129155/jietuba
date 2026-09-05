@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

echo ============================================
echo   Jietuba 一键部署脚本
echo ============================================
echo.

rem ---------- 1. 定位 Python 3.11 ----------
set "PYTHON_CMD="
py -3.11 --version >nul 2>nul
if %errorlevel%==0 (
    set "PYTHON_CMD=py -3.11"
) else (
    python --version 2>nul | findstr /c:"3.11" >nul
    if !errorlevel!==0 set "PYTHON_CMD=python"
)

if not defined PYTHON_CMD (
    echo [错误] 未检测到 Python 3.11，请先安装 Python 3.11 x64 并勾选 "Add to PATH"。
    echo        下载地址: https://www.python.org/downloads/release/python-3119/
    pause
    exit /b 1
)

echo [1/5] 使用 Python: %PYTHON_CMD%

rem ---------- 2. 创建虚拟环境 ----------
if not exist venv311\Scripts\activate.bat (
    echo [2/5] 创建虚拟环境 venv311 ...
    %PYTHON_CMD% -m venv venv311
) else (
    echo [2/5] 虚拟环境已存在，跳过创建。
)

call venv311\Scripts\activate.bat

echo [3/5] 升级 pip 并安装 Python 依赖 ...
python -m pip install --upgrade pip -q
pip install -r requirements.txt -q
if not %errorlevel%==0 (
    echo [错误] 安装 requirements.txt 失败。
    pause
    exit /b 1
)

rem ---------- 4. 选择 Rust 扩展包来源 ----------
set "WHEELS=gifrecorder-0.2.1-cp311-cp311-win_amd64.whl longstitch-0.3.11-cp311-cp311-win_amd64.whl pyclipboard-0.3.14-cp311-cp311-win_amd64.whl ppocr_rust-0.1.1-cp311-cp311-win_amd64.whl"
set "RELEASE_URL=https://github.com/1003129155/jietuba/releases/download/rust-libs-v1"

echo.
echo [4/5] 请选择自制 Rust 扩展包(gifrecorder/longstitch/pyclipboard/ppocr_rust)来源:
echo   [1] 使用仓库根目录自带的 .whl 文件 (默认)
echo   [2] 从 GitHub Release 重新下载最新版
set /p WHEEL_SRC="请输入选项 (1/2，直接回车默认 1): "
if not defined WHEEL_SRC set "WHEEL_SRC=1"

if "%WHEEL_SRC%"=="2" (
    echo   将从 Release 下载: %RELEASE_URL%
    for %%W in (%WHEELS%) do (
        echo   下载 %%W ...
        curl -L -f -o "%%W" "%RELEASE_URL%/%%W"
        if not !errorlevel!==0 (
            echo [错误] 下载 %%W 失败，请检查网络，或手动从下方页面下载后重新运行本脚本:
            echo   https://github.com/1003129155/jietuba/releases/tag/rust-libs-v1
            pause
            exit /b 1
        )
    )
) else (
    set "MISSING="
    for %%W in (%WHEELS%) do (
        if not exist "%%W" set "MISSING=1"
    )
    if defined MISSING (
        echo   本地 wheel 文件不全，自动改为从 Release 下载 ...
        for %%W in (%WHEELS%) do (
            if not exist "%%W" (
                echo   下载 %%W ...
                curl -L -f -o "%%W" "%RELEASE_URL%/%%W"
                if not !errorlevel!==0 (
                    echo [错误] 下载 %%W 失败，请检查网络，或手动从下方页面下载后重新运行本脚本:
                    echo   https://github.com/1003129155/jietuba/releases/tag/rust-libs-v1
                    pause
                    exit /b 1
                )
            )
        )
    )
)

echo   安装 Rust 扩展包 ...
pip install %WHEELS% -q
if not %errorlevel%==0 (
    echo [错误] Rust 扩展包安装失败。
    pause
    exit /b 1
)

echo.
echo [5/5] 部署完成！
echo.
set /p RUN_NOW="是否立即启动程序？(Y/N): "
if /i "%RUN_NOW%"=="Y" (
    cd main
    python main_app.py
    cd ..
) else (
    echo 之后可用以下命令启动:
    echo   venv311\Scripts\activate
    echo   cd main ^&^& python main_app.py
)

pause
