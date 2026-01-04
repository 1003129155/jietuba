# pyclipboard

Python 剪贴板管理库（Rust 实现）

基于 PyO3 和 clipboard-rs，提供高性能的剪贴板操作和历史记录管理功能。

## 特性

- ✅ 剪贴板读写（文本、图片、文件、HTML、RTF）
- ✅ 剪贴板变化监听
- ✅ 历史记录管理（SQLite 存储）
- ✅ 图片历史存储（自动保存到文件）
- ✅ 搜索和过滤
- ✅ 置顶功能
- ✅ 分组/标签管理
- ✅ 来源应用获取（Windows）
- ✅ 粘贴次数统计
- ✅ 跨平台（Windows/macOS/Linux）

## 安装

### 从源码编译

需要安装 Rust 和 maturin：

```bash
# 安装 maturin
pip install maturin

# 开发模式安装
maturin develop

# 或构建 wheel 包
maturin build --release
pip install target/wheels/pyclipboard-*.whl
```

## 快速开始

### 基础剪贴板操作

```python
import pyclipboard

# 读取剪贴板文本
text = pyclipboard.get_clipboard_text()
print(f"剪贴板内容: {text}")

# 设置剪贴板文本
pyclipboard.set_clipboard_text("Hello World!")

# 获取剪贴板图片（PNG 字节）
image_bytes = pyclipboard.get_clipboard_image()
if image_bytes:
    with open("clipboard.png", "wb") as f:
        f.write(image_bytes)

# 设置剪贴板图片
with open("image.png", "rb") as f:
    pyclipboard.set_clipboard_image(f.read())

# 获取 HTML 内容
html = pyclipboard.get_clipboard_html()

# 获取 RTF 富文本
rtf = pyclipboard.get_clipboard_rtf()

# 获取/设置剪贴板文件
files = pyclipboard.get_clipboard_files()
pyclipboard.set_clipboard_files(["C:/path/to/file.txt"])

# 获取剪贴板可用格式
formats = pyclipboard.get_available_formats()
print(f"可用格式: {formats}")

# 获取剪贴板内容来源应用（仅 Windows）
owner = pyclipboard.get_clipboard_owner()
print(f"来源应用: {owner}")
```

### 剪贴板历史管理

```python
from pyclipboard import PyClipboardManager

# 创建管理器
manager = PyClipboardManager()  # 默认数据库路径
# 或指定路径
manager = PyClipboardManager("./my_clipboard.db")

# 启动监听（带回调）
def on_clipboard_change(item):
    print(f"新内容: {item.content[:50]}")
    print(f"类型: {item.content_type}")
    print(f"来源: {item.source_app}")
    if item.html_content:
        print(f"HTML: {item.html_content[:100]}")

manager.start_monitor(callback=on_clipboard_change)

# 查询历史
result = manager.get_history(offset=0, limit=20)
print(f"总数: {result.total_count}")
for item in result:
    print(f"  [{item.id}] {item.content_type}: {item.content[:30]}")

# 搜索
items = manager.search("hello", limit=10)

# 置顶
is_pinned = manager.toggle_pin(1)

# 获取图片数据
if item.content_type == "image" and item.image_id:
    image_bytes = manager.get_image_data(item.image_id)

# 粘贴某项（设置到剪贴板并增加粘贴次数）
manager.paste_item(1)

# 删除
manager.delete_item(1)

# 清空历史
manager.clear_history()

# 停止监听
manager.stop_monitor()
```

### 分组管理

```python
from pyclipboard import PyClipboardManager

manager = PyClipboardManager()

# 创建分组
group_id = manager.create_group("工作", color="#FF5722", icon="💼")
print(f"创建分组 ID: {group_id}")

# 获取所有分组
groups = manager.get_groups()
for g in groups:
    print(f"  {g.name} (ID: {g.id})")

# 将项目移动到分组
manager.move_to_group(item_id=1, group_id=group_id)

# 按分组查询
result = manager.get_by_group(group_id=group_id)
for item in result:
    print(f"  {item.content[:30]}")

# 查询未分组的项目
result = manager.get_by_group(group_id=None)

# 重命名分组
manager.rename_group(group_id, "重要工作")

# 删除分组（项目会移到未分组）
manager.delete_group(group_id)
```

## API 参考

### 函数

| 函数 | 说明 |
|------|------|
| `get_clipboard_text()` | 获取剪贴板文本，返回 `Optional[str]` |
| `set_clipboard_text(text)` | 设置剪贴板文本 |
| `get_clipboard_image()` | 获取剪贴板图片（PNG），返回 `Optional[bytes]` |
| `get_clipboard_files()` | 获取剪贴板文件列表，返回 `List[str]` |
| `set_clipboard_files(files)` | 设置剪贴板文件 |

### PyClipboardManager 类

| 方法 | 说明 |
|------|------|
| `start_monitor(callback=None)` | 启动剪贴板监听 |
| `stop_monitor()` | 停止监听 |
| `is_monitoring()` | 检查是否在监听 |
| `get_history(offset, limit, search, content_type)` | 分页查询历史 |
| `get_count()` | 获取总记录数 |
| `get_item(id)` | 根据 ID 获取项 |
| `delete_item(id)` | 删除指定项 |
| `clear_history()` | 清空所有历史 |
| `toggle_pin(id)` | 切换置顶状态 |
| `search(keyword, limit)` | 搜索内容 |
| `add_item(content, content_type)` | 手动添加记录 |

### PyClipboardItem 类

| 属性 | 类型 | 说明 |
|------|------|------|
| `id` | `int` | 唯一标识 |
| `content` | `str` | 主要内容 |
| `html_content` | `Optional[str]` | HTML 内容 |
| `content_type` | `str` | 类型: text/file/image |
| `is_pinned` | `bool` | 是否置顶 |
| `paste_count` | `int` | 粘贴次数 |
| `created_at` | `int` | 创建时间戳 |
| `updated_at` | `int` | 更新时间戳 |

## 构建说明

### 依赖

- Rust 1.70+
- Python 3.8+
- maturin

### 编译命令

```bash
# 开发模式（快速编译，方便调试）
maturin develop

# Release 模式（优化编译）
maturin build --release

# 指定 Python 版本
maturin build --release -i python3.11
```

## License

MIT
