"""
截图吧打包脚本
使用 onefile 模式（单文件）

使用方式：
  python build_with_ocr_onefile.py          完整版：截图工具 OCR + PP-OCR，外置 models/ 模型目录
  python build_with_ocr_onefile.py --lite   轻量版：只有截图工具 OCR，不带 PP-OCR 引擎和模型

输出：
  完整版  dist/jietuba_pp.exe、dist/models/PP-OCRv6_det_small.onnx、dist/models/PP-OCRv6_rec_small.onnx
  轻量版  dist_lite/jietuba_lite.exe
"""
import PyInstaller.__main__
from pathlib import Path
import os
import sys

LITE = "--lite" in sys.argv[1:]

from main.video_helper import installed_recorder_licenses

# 路径配置
SCRIPT_DIR = Path(__file__).resolve().parent
REPO_DIR = SCRIPT_DIR
MAIN_APP = "main/main_app.py"
SVG_DIR = "svg"
BUILD_DIR = "build"
DIST_DIR = "dist_lite" if LITE else "dist"
EXE_NAME = "jietuba_lite" if LITE else "jietuba_pp"

# 翻译文件
TRANSLATIONS_DIR = "main/translations"

# 数据文件
datas = [
    f"{SVG_DIR};svg",
    f"{TRANSLATIONS_DIR};translations",
]

# 隐藏导入
hidden_imports = [
    'PySide6.QtCore',
    'PySide6.QtGui',
    'PySide6.QtWidgets',
    'PySide6.QtSvg',
    'PySide6.QtSvgWidgets',
    'PySide6.QtXml',
    'pyclipboard',
    'longstitch',
    'gifrecorder',
    'oneocr',
    'video_recorder',
    'video_worker',
    'hdrcapture',
    'inputhub',
    'zxingcpp',
    'PIL',
    'PIL.Image',
    'mss',
    'mss.windows',
    'mss.base',
    'mss.exception',
    'mss.factory',
    'mss.models',
    'mss.screenshot',
    'mss.tools',
    'win32api',
    'win32con',
    'win32gui',
    'comtypes.client',
    'comtypes.gen.UIAutomationClient',
    'darkdetect',
]

# 排除的模块
excludes = [
    'matplotlib',
    'scipy',
    'numpy',
    'pandas',
    'torch',
    'tensorflow',
    'PyQt5',
    'PyQt6',
    'PySide2',
    'tkinter',
    'pytest',
    'IPython',
    'jupyter',
    'rapidocr',
    'rapidocr_onnxruntime',
    'onnxruntime',
    'keyboard',
    'av',
    'windows_media_ocr',
    # oneocr 的 OpenCV 输入和 HTTP 服务用不到
    'cv2',
    'fastapi',
    'uvicorn',
    'pythoncom',
    'win32com',
    'win32com.client',
    'win32com.server',
    'win32com.gen_py',
    'PySide6.QtQml',
    'PySide6.QtQuick',
    'PySide6.QtQuickControls2',
    'PySide6.QtQuickWidgets',
    'PySide6.QtQuickTest',
    'PySide6.QtNetwork',
    'PySide6.QtSql',
    'PySide6.QtOpenGL',
    'PySide6.QtOpenGLWidgets',
    'PySide6.QtPrintSupport',
    'PySide6.QtHelp',
    'PySide6.QtUiTools',
    'PySide6.QtDesigner',
    'PySide6.QtTest',
    'PySide6.QtConcurrent',
    'PySide6.QtDBus',
    'PySide6.Qt3DAnimation',
    'PySide6.Qt3DCore',
    'PySide6.Qt3DExtras',
    'PySide6.Qt3DInput',
    'PySide6.Qt3DLogic',
    'PySide6.Qt3DRender',
    'PySide6.QtBluetooth',
    'PySide6.QtCharts',
    'PySide6.QtDataVisualization',
    'PySide6.QtGraphs',
    'PySide6.QtHttpServer',
    'PySide6.QtLocation',
    'PySide6.QtMultimedia',
    'PySide6.QtMultimediaWidgets',
    'PySide6.QtNetworkAuth',
    'PySide6.QtNfc',
    'PySide6.QtPdf',
    'PySide6.QtPdfWidgets',
    'PySide6.QtPositioning',
    'PySide6.QtQuick3D',
    'PySide6.QtRemoteObjects',
    'PySide6.QtScxml',
    'PySide6.QtSensors',
    'PySide6.QtSerialBus',
    'PySide6.QtSerialPort',
    'PySide6.QtSpatialAudio',
    'PySide6.QtStateMachine',
    'PySide6.QtTextToSpeech',
    'PySide6.QtVirtualKeyboard',
    'PySide6.QtWebChannel',
    'PySide6.QtWebEngineCore',
    'PySide6.QtWebEngineQuick',
    'PySide6.QtWebEngineWidgets',
    'PySide6.QtWebSockets',
]

if LITE:
    excludes.append('ppocr_rust')
else:
    hidden_imports.append('ppocr_rust')

def build_updater():
    import platform
    import subprocess
    machine = platform.machine().lower()
    targets = {"amd64": "x86_64-pc-windows-msvc", "x86_64": "x86_64-pc-windows-msvc",
               "arm64": "aarch64-pc-windows-msvc", "aarch64": "aarch64-pc-windows-msvc"}
    if machine not in targets:
        raise RuntimeError(f"Unsupported Windows package architecture: {machine}")
    target = targets[machine]
    # Build the standalone helper separately with a static CRT so it can outlive the application.
    command = ["cargo", "rustc", "--locked", "--release", "--target", target,
               "--manifest-path", str(REPO_DIR / "rust_libs" / "Cargo.toml"),
               "-p", "jietuba-updater", "--bin", "jietuba_updater", "--",
               "-C", "target-feature=+crt-static"]
    subprocess.run(command, check=True, cwd=REPO_DIR)
    return REPO_DIR / "rust_libs" / "target" / target / "release" / "jietuba_updater.exe"

def video_recorder_assets():
    """验证正式扩展并收集同一安装包的许可声明，不在打包时编译 Rust。"""
    import video_recorder

    if hasattr(video_recorder.Recorder, "_configure_test"):
        raise RuntimeError("发行包不得包含启用 test-support 的录制扩展，请先安装正式 wheel。")
    return installed_recorder_licenses()


def write_version_info():
    """生成 exe 版本资源。SignPath 按产品名和产品版本校验待签名文件，版本号取自 APP_VERSION。"""
    import re
    source = (REPO_DIR / MAIN_APP).read_text(encoding="utf-8")
    version = re.search(r'^APP_VERSION = "([^"]+)"', source, re.M).group(1)
    numbers = [int(n) for n in re.match(r"\d+(?:\.\d+)*", version).group().split(".")[:4]]
    numbers = tuple(numbers + [0] * (4 - len(numbers)))
    build_dir = REPO_DIR / BUILD_DIR
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "app_version.txt").write_text(version, encoding="utf-8")
    path = build_dir / f"version_{EXE_NAME}.txt"
    path.write_text(f"""VSVersionInfo(
  ffi=FixedFileInfo(filevers={numbers}, prodvers={numbers}, mask=0x3f, flags=0x0,
                    OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040904B0', [
      StringStruct('ProductName', 'jietuba'),
      StringStruct('ProductVersion', {version!r}),
      StringStruct('FileVersion', {version!r}),
      StringStruct('FileDescription', 'jietuba'),
      StringStruct('InternalName', {EXE_NAME!r}),
      StringStruct('OriginalFilename', '{EXE_NAME}.exe'),
      StringStruct('LegalCopyright', 'Copyright (c) 2025 JYAARU'),
    ])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])]),
  ]
)
""", encoding="utf-8")
    return path


if __name__ == '__main__':
    video_licenses = video_recorder_assets()
    datas.extend(f"{path};licenses/video_recorder" for path in video_licenses.values())
    os.chdir(REPO_DIR)

    updater_binary = build_updater()

    # Generate the UIA typelib wrapper before Analysis so the onefile build
    # includes its dynamic imports without requiring a writable runtime cache.
    import comtypes.client
    comtypes.client.GetModule("UIAutomationCore.dll")

    print("=" * 60)
    print(f"开始打包 {EXE_NAME} (onefile 模式，{'轻量版' if LITE else '完整版'})")
    print("=" * 60)
    print(f"源文件: {MAIN_APP}")
    print(f"输出目录: {DIST_DIR}")
    print(f"构建目录: {BUILD_DIR}")
    print("=" * 60)

    datas.append("rust_libs/updater/THIRD-PARTY-NOTICES.txt;updater")
    # 更新器按程序自报的版本类型选下载包，exe 改了名也能更新
    variant_file = REPO_DIR / BUILD_DIR / f"variant_{EXE_NAME}" / "app_variant.txt"
    variant_file.parent.mkdir(parents=True, exist_ok=True)
    variant_file.write_text("lite" if LITE else "full", encoding="utf-8")
    datas.append(f"{variant_file.relative_to(REPO_DIR).as_posix()};updater")
    version_repr = repr(write_version_info().relative_to(REPO_DIR).as_posix())
    binaries_repr = repr([(str(updater_binary), "updater")])
    datas_repr  = repr([(d.split(';')[0], d.split(';')[1]) for d in datas])
    hidden_repr = repr(hidden_imports)
    excl_repr   = repr(excludes)

    spec_content = f"""\
# -*- mode: python ; coding: utf-8 -*-
# 此 spec 文件由 build_with_ocr_onefile.py 自动生成，请勿手动修改后直接运行
import os as _os

a = Analysis(
    ['{MAIN_APP}'],
    pathex=['main'],
    binaries={binaries_repr},
    datas={datas_repr},
    hiddenimports={hidden_repr},
    hookspath=[],
    hooksconfig={{}},
    runtime_hooks=['main/video_worker_hook.py'],
    excludes={excl_repr},
    noarchive=False,
    optimize=0,
)

# ── binary 层过滤：使用白名单模式，只保留实际需要的 PySide6 DLL/pyd ──

# 白名单：只保留这些 PySide6 相关的 DLL 和 pyd
_PYSIDE6_WHITELIST = {{
    # 核心 DLL
    'qt6core.dll', 'qt6gui.dll', 'qt6widgets.dll',
    'qt6svg.dll', 'qt6svgwidgets.dll', 'qt6xml.dll',
    # 核心 pyd
    'qtcore.pyd', 'qtgui.pyd', 'qtwidgets.pyd',
    'qtsvg.pyd', 'qtsvgwidgets.pyd', 'qtxml.pyd',
    # shiboken / PySide6 绑定
    'pyside6.abi3.dll', 'shiboken6.abi3.dll', 'shiboken.pyd',
    # VC++ 运行时（从 shiboken6 目录带入）
    'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll',
    'concrt140.dll', 'vcamp140.dll', 'vccorlib140.dll', 'vcomp140.dll',
}}

# 额外要删除的文件
_EXTRA_STRIP = {{
    'opengl32sw.dll',
    # Qt 6.11 在 Windows 上使用系统 ICU。Codex/文档工具可能把 Poppler 的
    # ABI 不兼容副本注入 PATH；若 PyInstaller 收集它，QtWidgets 会在启动时
    # 报 DLL load failed。不要把这些外来 ICU DLL 冻结进应用。
    'icuuc.dll',
    # FFmpeg（Qt Multimedia 拉入，截图不需要）
    'avcodec-61.dll', 'avformat-61.dll', 'avutil-59.dll',
    'swscale-8.dll', 'swresample-5.dll',
}}
_EXTRA_STRIP_PATTERNS = ('libscipy_openblas', 'libopenblas', 'icudt')
_STRIP_BINARY_PATTERNS = ('_avif.',)  # Pillow AVIF

def _keep_binary(entry):
    name = _os.path.basename(entry[1]).lower()
    # 1) 模式排除
    if any(name.startswith(p) for p in _STRIP_BINARY_PATTERNS):
        return False
    if any(name.startswith(p) for p in _EXTRA_STRIP_PATTERNS):
        return False
    # 2) 额外指定排除
    if name in _EXTRA_STRIP:
        return False
    # 3) 排除 windows_media_ocr 的 pyd/dll
    if name.startswith('windows_media_ocr'):
        return False
    # 4) PySide6 相关文件：白名单模式
    src_lower = entry[1].lower()
    if 'pyside6' in src_lower or 'shiboken6' in src_lower:
        if 'plugins' in src_lower or 'translations' in src_lower:
            return True
        return name in _PYSIDE6_WHITELIST
    # 5) 非 PySide6 文件：保留
    return True

a.binaries = TOC([entry for entry in a.binaries if _keep_binary(entry)])

# ── QM 翻译文件过滤：只保留 zh/ja，去掉其他 Qt 语言包 ──
_KEEP_QM_LOCALES = {{'zh', 'zh_cn', 'zh_tw', 'ja'}}
def _keep_qm(entry):
    src = entry[1]
    name = _os.path.basename(src).lower()
    if not name.endswith('.qm'):
        return True
    if 'pyside6' not in src.lower():
        return True
    stem = name[:-3]
    locale = stem.split('_', 1)[1] if '_' in stem else stem
    return locale in _KEEP_QM_LOCALES or locale.startswith('zh')
a.datas = TOC([e for e in a.datas if _keep_qm(e)])

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='{EXE_NAME}',
    icon='托盘.ico',
    version={version_repr},
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
"""

    spec_path = REPO_DIR / f"{EXE_NAME}.spec"
    spec_path.write_text(spec_content, encoding="utf-8")
    print(f"已生成 spec 文件: {spec_path}")

    PyInstaller.__main__.run([
        str(spec_path),
        '--noconfirm',
        f'--distpath={DIST_DIR}',
        f'--workpath={BUILD_DIR}',
    ])

    # ── 完整版把 PP-OCR 模型外置到 exe 同级 models/ 目录 ──
    import shutil
    src_models = REPO_DIR / "models"
    dst_models = REPO_DIR / DIST_DIR / "models"
    model_files = ["PP-OCRv6_det_small.onnx", "PP-OCRv6_rec_small.onnx"]
    if not LITE and src_models.exists():
        dst_models.mkdir(parents=True, exist_ok=True)
        for fn in model_files:
            sp = src_models / fn
            if sp.exists():
                shutil.copy2(sp, dst_models / fn)
                print(f"已外置模型: {fn}")
            else:
                print(f"警告: 缺少模型 {fn}")
    elif not LITE:
        print(f"警告: 未找到 models 目录 {src_models}")

    from build_notices import write_notices
    print(f"许可声明: {write_notices(REPO_DIR / DIST_DIR, lite=LITE)}")

    print("=" * 60)
    print("打包完成！")
    print(f"可执行文件位置: {DIST_DIR}/{EXE_NAME}.exe")
    if not LITE:
        print(f"模型目录(需与 exe 同级): {DIST_DIR}/models/")
    print("=" * 60)
