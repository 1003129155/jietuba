# -*- coding: utf-8 -*-
"""
compile_translations.py - 编译翻译文件

将 .ts 翻译源文件编译成 .qm 二进制文件。

使用方法:
    python compile_translations.py

需要安装 PyQt6-tools:
    pip install PyQt6-tools
"""
import subprocess
import sys
from pathlib import Path


def find_lrelease():
    """查找 lrelease 工具路径"""
    # 尝试常见路径
    candidates = [
        "lrelease",  # PATH 中
        "lrelease6",  # PyQt6 版本
        "pyside6-lrelease",  # PySide6 版本
    ]
    
    # 尝试在 venv/Scripts 中查找
    venv_scripts = Path(__file__).parent.parent / "venv" / "Scripts"
    if venv_scripts.exists():
        candidates.extend([
            str(venv_scripts / "lrelease.exe"),
            str(venv_scripts / "lrelease6.exe"),
        ])
    
    # 尝试在 site-packages 中查找
    try:
        import PyQt6
        pyqt6_path = Path(PyQt6.__file__).parent
        qt_bin = pyqt6_path / "Qt6" / "bin"
        if qt_bin.exists():
            candidates.append(str(qt_bin / "lrelease.exe"))
    except ImportError:
        pass
    
    for cmd in candidates:
        try:
            result = subprocess.run([cmd, "-version"], capture_output=True, text=True)
            if result.returncode == 0 or "lrelease" in result.stdout.lower():
                return cmd
        except FileNotFoundError:
            continue
    
    return None


def compile_ts_files():
    """编译所有 .ts/.xml 翻译文件"""
    translations_dir = Path(__file__).parent / "translations"
    
    if not translations_dir.exists():
        print(f"❌ 翻译目录不存在: {translations_dir}")
        return False
    
    # 支持 .ts 和 .xml 后缀的翻译文件
    ts_files = list(translations_dir.glob("*.ts")) + list(translations_dir.glob("*.xml"))
    
    if not ts_files:
        print(f"❌ 没有找到 .ts 或 .xml 文件: {translations_dir}")
        return False
    
    print(f"📁 翻译目录: {translations_dir}")
    print(f"📄 找到 {len(ts_files)} 个 .ts 文件")
    
    # 查找 lrelease
    lrelease = find_lrelease()
    
    if lrelease:
        print(f"🔧 使用 lrelease: {lrelease}")
        
        for ts_file in ts_files:
            qm_file = ts_file.with_suffix(".qm")
            print(f"\n📝 编译: {ts_file.name} -> {qm_file.name}")
            
            try:
                result = subprocess.run(
                    [lrelease, str(ts_file), "-qm", str(qm_file)],
                    capture_output=True,
                    text=True
                )
                if result.returncode == 0:
                    print(f"   ✅ 成功")
                else:
                    print(f"   ❌ 失败: {result.stderr}")
            except Exception as e:
                print(f"   ❌ 错误: {e}")
    else:
        print("\n⚠️ 未找到 lrelease 工具，使用 Python 简易编译...")
        print("   (安装 PyQt6-tools 可获得完整功能: pip install PyQt6-tools)")
        
        # 使用 Python 简易方式创建空的 .qm 文件（Qt 会回退到源文本）
        for ts_file in ts_files:
            qm_file = ts_file.with_suffix(".qm")
            # 创建一个最小的有效 .qm 文件头
            # Qt .qm 文件是二进制格式，这里只创建占位文件
            # 实际翻译需要使用 lrelease 编译
            with open(qm_file, 'wb') as f:
                # Qt .qm 文件魔数
                f.write(b'\x3c\xb8\x64\x18\xff\xff\xff\xff\x08\x00\x00\x00\x00')
            print(f"   📄 创建占位文件: {qm_file.name}")
        
        print("\n💡 提示: 占位文件只能让程序启动，实际翻译需要:")
        print("   1. pip install PyQt6-tools")
        print("   2. 重新运行此脚本")
    
    print("\n✅ 完成!")
    return True


if __name__ == "__main__":
    compile_ts_files()
