"""
截图吧 - 带OCR版本打包脚本
使用 onefile 模式（单文件）
"""
import PyInstaller.__main__
from pathlib import Path

# 路径配置
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR.parent
MAIN_APP = "main/main_app.py"
SVG_DIR = "svg"
BUILD_DIR = "build"
DIST_DIR = "dist"

# 查找 site-packages (需要绝对路径)
SITE_PACKAGES_CANDIDATES = [
    REPO_DIR / "venv" / "Lib" / "site-packages",
    REPO_DIR / "venv" / "lib" / "site-packages",
]
SITE_PACKAGES = next((p for p in SITE_PACKAGES_CANDIDATES if p.exists()), None)

if not SITE_PACKAGES:
    raise RuntimeError("无法定位 venv/site-packages，请确认 venv 在仓库根目录下")

# RapidOCR 数据文件
rapidocr_dir = SITE_PACKAGES / "rapidocr"
rapidocr_datas = [
    f"{rapidocr_dir / 'default_models.yaml'};rapidocr",
    f"{rapidocr_dir / 'config.yaml'};rapidocr",
    f"{rapidocr_dir / 'models'};rapidocr/models",
]

# 数据文件
datas = [
    f"{SVG_DIR};svg",
    *rapidocr_datas,
]

# 隐藏导入
hidden_imports = [
    'PyQt6',
    'PyQt6.QtCore',
    'PyQt6.QtGui',
    'PyQt6.QtWidgets',
    'rapidocr_onnxruntime',
    'onnxruntime',
    'PIL',
    'PIL.Image',
    'numpy',
    'mss',
    'mss.windows',
    'mss.base',
    'mss.exception',
    'mss.factory',
    'mss.models',
    'mss.screenshot',
    'mss.tools',
    'keyboard',
    'win32api',
    'win32con',
    'win32gui',
]

# 排除的模块
excludes = [
    'matplotlib',
    'scipy',
    'pandas',
    'torch',
    'tensorflow',
    'PyQt5',
    'PySide2',
    'PySide6',
    'tkinter',
    'pytest',
    'IPython',
    'jupyter',
]

# PyInstaller 参数
pyinstaller_args = [
    str(MAIN_APP),
    '--name=jietuba_with_ocr',
    '--onefile',  # 使用 onefile 模式
    '--windowed',  # 无控制台窗口 (console=False)
    '--noconfirm',  # 覆盖输出目录
    f'--distpath={DIST_DIR}',
    f'--workpath={BUILD_DIR}',
    f'--specpath=.',  # spec 文件生成在项目根目录
    '--paths=新架构文件',  # hookspath=[str(BASE_DIR)]
    '--noupx',  # 禁用 UPX 压缩
]

# 添加数据文件
for data in datas:
    pyinstaller_args.append(f'--add-data={data}')

# 添加隐藏导入
for imp in hidden_imports:
    pyinstaller_args.append(f'--hidden-import={imp}')

# 添加排除模块
for exc in excludes:
    pyinstaller_args.append(f'--exclude-module={exc}')

if __name__ == '__main__':
    # 切换到项目根目录
    import os
    os.chdir(REPO_DIR)
    
    print("=" * 60)
    print("开始打包 jietuba_with_ocr (onefile 模式)")
    print("=" * 60)
    print(f"源文件: {MAIN_APP}")
    print(f"输出目录: {DIST_DIR}")
    print(f"构建目录: {BUILD_DIR}")
    print("=" * 60)
    
    PyInstaller.__main__.run(pyinstaller_args)
    
    print("=" * 60)
    print("打包完成！")
    print(f"可执行文件位置: {DIST_DIR}/jietuba_with_ocr.exe")
    print("=" * 60)

