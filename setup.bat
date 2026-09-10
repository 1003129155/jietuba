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
set "PKGS=j-gif j-stitch j-clipboard j-ppocr"
set "RELEASE_TAG=rust-libs-v1"

echo.
echo [4/5] 请选择自制 Rust 扩展包(%PKGS%)来源:
echo   [1] 使用 wheels\ 目录自带的 .whl 文件 (默认)
echo   [2] 从 GitHub Release 重新下载
set /p WHEEL_SRC="请输入选项 (1/2，直接回车默认 1): "
if not defined WHEEL_SRC set "WHEEL_SRC=1"

rem 本地不全就自动改为下载，不必让用户自己发现
set "WHEEL_COUNT=0"
for %%W in (wheels\*.whl) do set /a WHEEL_COUNT+=1
if !WHEEL_COUNT! LSS 4 (
    echo   wheels\ 里只找到 !WHEEL_COUNT! 个 .whl，自动改为从 Release 下载。
    set "WHEEL_SRC=2"
)

if "!WHEEL_SRC!"=="2" (
    echo   从 Release !RELEASE_TAG! 下载 ...
    rem 按 Release 里实际存在的 .whl 下载，不写死文件名，升版本无需改本脚本
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; New-Item -ItemType Directory -Force wheels | Out-Null; (Invoke-RestMethod 'https://api.github.com/repos/1003129155/jietuba/releases/tags/!RELEASE_TAG!').assets | Where-Object name -like '*.whl' | ForEach-Object { Write-Host ('   ' + $_.name); Invoke-WebRequest $_.browser_download_url -OutFile ('wheels' + $_.name) }"
    if not !errorlevel!==0 (
        echo [错误] 下载失败，请检查网络，或手动从下方页面下载到 wheels\ 后重新运行：
        echo   https://github.com/1003129155/jietuba/releases/tag/!RELEASE_TAG!
        pause
        exit /b 1
    )
)

echo   安装 Rust 扩展包 ...
rem 按包名装，版本由 wheels\ 里的文件决定
pip install --no-index --find-links=wheels %PKGS% -q
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
