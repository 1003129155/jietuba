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

echo [1/4] 使用 Python: %PYTHON_CMD%

rem ---------- 2. 创建虚拟环境 ----------
if not exist venv311\Scripts\activate.bat (
    echo [2/4] 创建虚拟环境 venv311 ...
    %PYTHON_CMD% -m venv venv311
) else (
    echo [2/4] 虚拟环境已存在，跳过创建。
)

call venv311\Scripts\activate.bat

echo [3/4] 升级 pip 并安装 Python 依赖（含四个自制 Rust 扩展包，来自 PyPI）...
python -m pip install --upgrade pip -q
pip install -r requirements.txt -q
if not %errorlevel%==0 (
    echo [错误] 安装 requirements.txt 失败。
    pause
    exit /b 1
)

echo.
echo [4/4] 部署完成！
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
