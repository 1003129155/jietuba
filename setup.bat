@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

rem Read Windows user language settings: redirected PowerShell may report en-US.
rem Prefer the display language override, then the preferred language, then region.
set "SETUP_LANG=en"
for /f "delims=" %%L in ('powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$ui = Get-WinUILanguageOverride; if ($ui) { $ui.TwoLetterISOLanguageName } else { $tag = (Get-WinUserLanguageList)[0].LanguageTag; if ($tag) { ($tag -split '-')[0] } else { (Get-Culture).TwoLetterISOLanguageName } }" 2^>nul') do (
    for %%S in (zh en ja ko) do if /i "%%L"=="%%S" set "SETUP_LANG=%%S"
)
call :lang_!SETUP_LANG!

echo ============================================
echo   !MSG_TITLE!
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
    echo !MSG_PYTHON_MISSING!
    echo        !MSG_DOWNLOAD! https://www.python.org/downloads/release/python-3119/
    call :pause_localized
    exit /b 1
)

echo [1/4] !MSG_PYTHON! %PYTHON_CMD%

rem ---------- 2. 创建虚拟环境 ----------
if not exist venv311\Scripts\activate.bat (
    echo [2/4] !MSG_VENV_CREATE!
    %PYTHON_CMD% -m venv venv311
) else (
    echo [2/4] !MSG_VENV_EXISTS!
)

call venv311\Scripts\activate.bat

echo [3/4] !MSG_INSTALL!
python -m pip install --upgrade pip -q
pip install -r requirements.txt -q
if not %errorlevel%==0 (
    echo !MSG_INSTALL_FAILED!
    call :pause_localized
    exit /b 1
)

echo.
echo [4/4] !MSG_COMPLETE!
echo.
set "RUN_NOW="
set /p "RUN_NOW=!MSG_RUN_NOW!"
if /i "%RUN_NOW%"=="Y" (
    cd main
    python main_app.py
    cd ..
) else (
    echo !MSG_RUN_LATER!
    echo   venv311\Scripts\activate
    echo   cd main ^&^& python main_app.py
)

call :pause_localized
exit /b 0

:pause_localized
echo !MSG_PAUSE!
pause >nul
exit /b

:lang_zh
set "MSG_TITLE=Jietuba 一键部署脚本"
set "MSG_PYTHON_MISSING=[错误] 未检测到 Python 3.11，请安装与系统架构一致的 Python 3.11（x64 或 ARM64）并勾选 "Add to PATH"。"
set "MSG_DOWNLOAD=下载地址:"
set "MSG_PYTHON=使用 Python:"
set "MSG_VENV_CREATE=创建虚拟环境 venv311 ..."
set "MSG_VENV_EXISTS=虚拟环境已存在，跳过创建。"
set "MSG_INSTALL=升级 pip 并安装 Python 依赖（含四个自制 Rust 扩展包，来自 PyPI）..."
set "MSG_INSTALL_FAILED=[错误] 安装 requirements.txt 失败。"
set "MSG_COMPLETE=部署完成！"
set "MSG_RUN_NOW=是否立即启动程序？(Y/N): "
set "MSG_RUN_LATER=之后可用以下命令启动:"
set "MSG_PAUSE=请按任意键继续 . . ."
exit /b

:lang_en
set "MSG_TITLE=Jietuba One-click Setup"
set "MSG_PYTHON_MISSING=[Error] Python 3.11 was not found. Install Python 3.11 for your Windows architecture (x64 or ARM64) and select "Add to PATH"."
set "MSG_DOWNLOAD=Download:"
set "MSG_PYTHON=Using Python:"
set "MSG_VENV_CREATE=Creating the venv311 virtual environment ..."
set "MSG_VENV_EXISTS=The virtual environment already exists. Skipping creation."
set "MSG_INSTALL=Upgrading pip and installing Python dependencies (including four custom Rust extensions from PyPI) ..."
set "MSG_INSTALL_FAILED=[Error] Failed to install requirements.txt."
set "MSG_COMPLETE=Setup complete."
set "MSG_RUN_NOW=Start the application now? (Y/N): "
set "MSG_RUN_LATER=You can start the application later with these commands:"
set "MSG_PAUSE=Press any key to continue . . ."
exit /b

:lang_ja
set "MSG_TITLE=Jietuba ワンクリックセットアップ"
set "MSG_PYTHON_MISSING=[エラー] Python 3.11 が見つかりません。Windows のシステム構成に合った Python 3.11（x64 または ARM64）をインストールし、"Add to PATH" にチェックを入れてください。"
set "MSG_DOWNLOAD=ダウンロード:"
set "MSG_PYTHON=使用する Python:"
set "MSG_VENV_CREATE=仮想環境 venv311 を作成しています ..."
set "MSG_VENV_EXISTS=仮想環境は既に存在するため、作成をスキップします。"
set "MSG_INSTALL=pip を更新し、Python の依存パッケージ（PyPI の自作 Rust 拡張パッケージ 4 個を含む）をインストールしています ..."
set "MSG_INSTALL_FAILED=[エラー] requirements.txt のインストールに失敗しました。"
set "MSG_COMPLETE=セットアップが完了しました！"
set "MSG_RUN_NOW=今すぐアプリを起動しますか？(Y/N): "
set "MSG_RUN_LATER=後で起動するには、次のコマンドを実行してください:"
set "MSG_PAUSE=続行するには何かキーを押してください . . ."
exit /b

:lang_ko
set "MSG_TITLE=Jietuba 원클릭 설치"
set "MSG_PYTHON_MISSING=[오류] Python 3.11을 찾을 수 없습니다. Windows 아키텍처에 맞는 Python 3.11(x64 또는 ARM64)을 설치하고 "Add to PATH"를 선택하세요."
set "MSG_DOWNLOAD=다운로드:"
set "MSG_PYTHON=사용할 Python:"
set "MSG_VENV_CREATE=가상 환경 venv311을 생성하는 중 ..."
set "MSG_VENV_EXISTS=가상 환경이 이미 존재하므로 생성을 건너뜁니다."
set "MSG_INSTALL=pip을 업그레이드하고 Python 의존성 패키지(PyPI의 자체 제작 Rust 확장 패키지 4개 포함)를 설치하는 중 ..."
set "MSG_INSTALL_FAILED=[오류] requirements.txt 설치에 실패했습니다."
set "MSG_COMPLETE=설치가 완료되었습니다."
set "MSG_RUN_NOW=지금 프로그램을 실행하시겠습니까? (Y/N): "
set "MSG_RUN_LATER=나중에 다음 명령으로 프로그램을 실행할 수 있습니다:"
set "MSG_PAUSE=계속하려면 아무 키나 누르세요 . . ."
exit /b
