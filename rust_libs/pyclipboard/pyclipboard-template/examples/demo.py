"""
pyclipboard 使用示例

运行前请先安装:
    maturin develop
    
或:
    maturin build --release
    pip install target/wheels/pyclipboard-*.whl
"""

import pyclipboard
from pyclipboard import PyClipboardManager
import time


def example_basic():
    """基础剪贴板操作示例"""
    print("=" * 50)
    print("基础剪贴板操作示例")
    print("=" * 50)
    
    # 设置文本
    pyclipboard.set_clipboard_text("Hello from Python + Rust!")
    
    # 读取文本
    text = pyclipboard.get_clipboard_text()
    print(f"剪贴板文本: {text}")
    
    # 获取图片
    image = pyclipboard.get_clipboard_image()
    if image:
        print(f"剪贴板图片: {len(image)} 字节")
        with open("clipboard_image.png", "wb") as f:
            f.write(image)
        print("已保存到 clipboard_image.png")
    else:
        print("剪贴板中没有图片")
    
    # 获取文件
    files = pyclipboard.get_clipboard_files()
    if files:
        print(f"剪贴板文件: {files}")
    else:
        print("剪贴板中没有文件")
    
    print()


def example_manager():
    """剪贴板历史管理示例"""
    print("=" * 50)
    print("剪贴板历史管理示例")
    print("=" * 50)
    
    # 创建管理器（使用当前目录的数据库）
    manager = PyClipboardManager("./example_clipboard.db")
    
    # 添加一些测试数据
    print("添加测试数据...")
    manager.add_item("第一条记录")
    manager.add_item("第二条记录 - Hello World")
    manager.add_item("第三条记录 - Python + Rust")
    manager.add_item('{"files": ["C:/test.txt"]}', "file")
    
    # 获取总数
    count = manager.get_count()
    print(f"总记录数: {count}")
    
    # 查询历史
    print("\n历史记录:")
    result = manager.get_history(offset=0, limit=10)
    for item in result:
        pin = "📌" if item.is_pinned else "  "
        print(f"  {pin} [{item.id}] {item.content_type}: {item.content[:40]}")
    
    # 搜索
    print("\n搜索 'Hello':")
    items = manager.search("Hello")
    for item in items:
        print(f"  找到: {item.content}")
    
    # 置顶第一条
    if result.items:
        first_id = result.items[0].id
        is_pinned = manager.toggle_pin(first_id)
        print(f"\n切换 ID={first_id} 的置顶状态: {is_pinned}")
    
    # 获取单个项
    item = manager.get_item(1)
    if item:
        print(f"\n获取 ID=1: {item.to_dict()}")
    
    print()


def example_monitor():
    """剪贴板监听示例"""
    print("=" * 50)
    print("剪贴板监听示例")
    print("=" * 50)
    
    manager = PyClipboardManager("./monitor_clipboard.db")
    
    # 定义回调
    def on_clipboard_change(item):
        print(f"[新内容] {item.content_type}: {item.content[:50]}")
    
    # 启动监听
    print("开始监听剪贴板...")
    print("请复制一些内容，5秒后自动停止")
    manager.start_monitor(callback=on_clipboard_change)
    
    # 检查状态
    print(f"监听状态: {manager.is_monitoring()}")
    
    # 等待 5 秒
    time.sleep(5)
    
    # 停止监听
    manager.stop_monitor()
    print("已停止监听")
    
    # 显示记录
    print("\n捕获的记录:")
    for item in manager.get_history():
        print(f"  [{item.id}] {item.content[:40]}")
    
    print()


def main():
    """主函数"""
    print("\n" + "=" * 50)
    print("pyclipboard 示例程序")
    print("=" * 50 + "\n")
    
    # 基础操作
    example_basic()
    
    # 历史管理
    example_manager()
    
    # 监听（可选，取消注释以运行）
    # example_monitor()
    
    print("示例完成!")


if __name__ == "__main__":
    main()
