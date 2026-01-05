# -*- coding: utf-8 -*-
"""
剪贴板模块测试脚本

运行方式:
    python -m clipboard.test_clipboard
    
或:
    python test_clipboard.py
"""

import sys
import os

# 添加 main 目录到路径
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from PyQt6.QtWidgets import QApplication
from clipboard import ClipboardManager, ClipboardWindow


def test_manager():
    """测试管理器基础功能"""
    print("=" * 50)
    print("📋 测试 ClipboardManager")
    print("=" * 50)
    
    manager = ClipboardManager()
    
    print(f"[OK] 管理器可用: {manager.is_available}")
    
    if not manager.is_available:
        print("[ERROR] 管理器不可用，请检查 pyclipboard 是否安装")
        return False
    
    # 测试获取当前剪贴板
    print("\n📝 当前剪贴板内容:")
    text = manager.get_clipboard_text()
    print(f"   文本: {text[:50] if text else '(空)'}...")
    
    owner = manager.get_clipboard_owner()
    print(f"   来源: {owner}")
    
    # 测试获取历史
    print("\n📚 历史记录:")
    items = manager.get_history(limit=5)
    print(f"   共 {manager.get_total_count()} 条记录")
    for i, item in enumerate(items, 1):
        print(f"   {i}. {item.icon} {item.display_text[:40]}...")
    
    # 测试搜索
    if items:
        print("\n🔍 测试搜索...")
        search_results = manager.search("a", limit=3)
        print(f"   找到 {len(search_results)} 条匹配")
    
    # 测试分组
    print("\n📁 测试分组功能:")
    groups = manager.get_groups()
    print(f"   现有分组: {len(groups)}")
    for g in groups:
        print(f"   - {g.name} (ID: {g.id})")
    
    print("\n[OK] 管理器测试完成!")
    return True


def test_window():
    """测试窗口界面"""
    print("\n" + "=" * 50)
    print("🖼️ 测试 ClipboardWindow")
    print("=" * 50)
    
    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    
    window = ClipboardWindow()
    
    def on_paste(item_id):
        print(f"📋 粘贴了项目 ID: {item_id}")
    
    def on_close():
        print("🚪 窗口已关闭")
    
    window.item_pasted.connect(on_paste)
    window.closed.connect(on_close)
    
    print("[OK] 窗口创建成功，正在显示...")
    print("   - 双击项目可粘贴")
    print("   - 按 ESC 关闭窗口")
    print("   - 按数字键 1-9 快速粘贴")
    print("   - 右键可查看更多操作")
    
    window.show()
    
    sys.exit(app.exec())


def test_monitor():
    """测试剪贴板监听"""
    print("\n" + "=" * 50)
    print("👀 测试剪贴板监听")
    print("=" * 50)
    
    manager = ClipboardManager()
    
    if not manager.is_available:
        print("[ERROR] 管理器不可用")
        return
    
    def on_change(item):
        print(f"\n📋 新内容!")
        print(f"   类型: {item.content_type}")
        print(f"   内容: {item.display_text[:50]}")
        print(f"   来源: {item.source_app}")
    
    print("开始监听剪贴板变化...")
    print("复制一些内容试试，按 Ctrl+C 退出")
    
    manager.start_monitoring(callback=on_change)
    
    try:
        import time
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n停止监听")
        manager.stop_monitoring()


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(description="剪贴板模块测试")
    parser.add_argument("--mode", choices=["manager", "window", "monitor", "all"],
                        default="all", help="测试模式")
    
    args = parser.parse_args()
    
    if args.mode == "manager":
        test_manager()
    elif args.mode == "window":
        test_window()
    elif args.mode == "monitor":
        test_monitor()
    else:
        # 默认测试管理器，然后打开窗口
        if test_manager():
            print("\n按 Enter 打开窗口测试...")
            input()
            test_window()
