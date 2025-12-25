# -*- mode: python ; coding: utf-8 -*-


a = Analysis(
    ['..\\新架构文件\\main_app.py'],
    pathex=[],
    binaries=[],
    datas=[('svg', 'svg')],
    hiddenimports=['PyQt6', 'PyQt6.QtCore', 'PyQt6.QtGui', 'PyQt6.QtWidgets', 'PIL', 'PIL.Image', 'keyboard', 'win32api', 'win32con', 'win32gui'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=['ocr', 'rapidocr', 'rapidocr_onnxruntime', 'onnxruntime', 'onnx', 'matplotlib', 'scipy', 'pandas', 'torch', 'tensorflow', 'PyQt5', 'PySide2', 'PySide6', 'tkinter', 'pytest', 'IPython', 'jupyter', 'cv2', 'opencv'],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='jietuba_no_ocr',
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
