# OCR 集成指南

## 📖 概述

OCR 模块为钉图窗口提供了完整的文字识别和交互功能。主要包括：

1. **OCRManager**: 识别引擎（单例模式，使用 RapidOCR）
2. **OCRTextLayer**: 透明文字选择层（Word 风格交互）

## 🚀 在钉图窗口中集成 OCR

### 1. 异步初始化 OCR 文字层

在钉图窗口创建后，异步触发 OCR 识别：

```python
from PyQt6.QtCore import QThread
from ocr import is_ocr_available, initialize_ocr, recognize_text
from pin import OCRTextLayer  # OCR文字层已移至pin模块

def _init_ocr_text_layer_async(self):
    """异步初始化 OCR 文字选择层（不阻塞主线程）"""
    try:
        # 1. 检查 OCR 是否可用
        if not is_ocr_available():
            print("⚠️ [OCR] OCR 模块不可用，跳过初始化")
            return
        
        # 2. 初始化 OCR 引擎
        if not initialize_ocr():
            print("⚠️ [OCR] OCR 引擎初始化失败")
            return
        
        print("✅ [OCR] OCR 引擎已就绪（支持中日韩英混合识别）")
        
        # 3. 创建透明文字层
        self.ocr_text_layer = OCRTextLayer(self)
        self.ocr_text_layer.setGeometry(0, 0, self.width(), self.height())
        
        # 4. 设置绘图状态检查回调（可选）
        self.ocr_text_layer.is_drawing_callback = self._check_drawing_status
        
        # 5. 启用文字层
        self.ocr_text_layer.set_enabled(True)
        
        # 6. 创建异步 OCR 识别线程
        class OCRThread(QThread):
            def __init__(self, pixmap, parent=None):
                super().__init__(parent)
                self.pixmap = pixmap
                self.result = None
            
            def run(self):
                try:
                    # 调用 OCR 识别
                    self.result = recognize_text(
                        self.pixmap, 
                        return_format="dict",
                        enable_grayscale=True,  # 启用灰度转换
                        enable_upscale=False,   # 是否放大图像
                        upscale_factor=1.5      # 放大倍数
                    )
                except Exception as e:
                    print(f"❌ [OCR Thread] 识别失败: {e}")
                    self.result = None
        
        # 7. 获取钉图图像
        pixmap = self.get_pixmap()  # 获取钉图的 QPixmap
        original_width = pixmap.width()
        original_height = pixmap.height()
        
        # 8. 启动异步识别
        self.ocr_thread = OCRThread(pixmap, self)
        
        def on_ocr_finished():
            try:
                # 检查结果是否有效
                if self.ocr_thread.result and isinstance(self.ocr_thread.result, dict):
                    if self.ocr_thread.result.get('code') == 100:
                        # 加载 OCR 结果到文字层
                        self.ocr_text_layer.load_ocr_result(
                            self.ocr_thread.result, 
                            original_width, 
                            original_height
                        )
                        print(f"✅ [OCR] 识别完成，找到 {len(self.ocr_thread.result.get('data', []))} 个文字块")
            except Exception as e:
                print(f"❌ [OCR] 加载结果失败: {e}")
            finally:
                # 清理线程
                if hasattr(self, 'ocr_thread') and self.ocr_thread:
                    self.ocr_thread.deleteLater()
                    self.ocr_thread = None
        
        self.ocr_thread.finished.connect(on_ocr_finished)
        self.ocr_thread.start()
        
    except Exception as e:
        print(f"⚠️ [OCR] 初始化失败: {e}")
```

### 2. 处理窗口关闭

```python
def closeEvent(self, event):
    # 停止 OCR 线程
    if hasattr(self, 'ocr_thread') and self.ocr_thread is not None:
        if self.ocr_thread.isRunning():
            print("⚠️ [OCR] 窗口关闭，断开OCR线程信号...")
            try:
                self.ocr_thread.finished.disconnect()
            except:
                pass
            # 让线程在后台完成，不阻塞关闭
            self.ocr_thread = None
    
    # 清理文字层
    if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
        self.ocr_text_layer.set_enabled(False)
        self.ocr_text_layer.deleteLater()
        self.ocr_text_layer = None
    
    super().closeEvent(event)
```

### 3. 处理窗口缩放

```python
def resizeEvent(self, event):
    super().resizeEvent(event)
    
    # 同步调整文字层大小
    if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
        self.ocr_text_layer.setGeometry(0, 0, self.width(), self.height())
```

### 4. 与绘图工具集成

```python
def _check_drawing_status(self) -> bool:
    """检查是否正在绘图（供 OCR 文字层回调）"""
    if hasattr(self, 'tool_controller'):
        current_tool = self.tool_controller.current_tool
        # cursor 工具表示未在绘图
        return current_tool and current_tool.id != "cursor"
    return False

def on_tool_changed(self, tool_id: str):
    """工具切换时通知 OCR 文字层"""
    if hasattr(self, 'ocr_text_layer') and self.ocr_text_layer:
        is_drawing = (tool_id != "cursor")
        self.ocr_text_layer.set_drawing_mode(is_drawing)
```

## 🎨 OCR 文字层功能

### 用户交互

1. **鼠标悬停**: 在文字上显示 I-beam 光标
2. **单击拖拽**: Word 风格文字选择
3. **双击**: 选择整个文字块
4. **Ctrl+C**: 复制选中文字
5. **Ctrl+A**: 全选所有文字
6. **Esc**: 取消选择

### 自动行为

1. **绘图模式**: 自动禁用文字选择
2. **窗口缩放**: 自动调整坐标
3. **透明背景**: 不遮挡钉图内容

## 📊 OCR 识别结果格式

### 字典格式（推荐）

```python
{
    "code": 100,
    "msg": "成功",
    "data": [
        {
            "box": [[x1,y1], [x2,y2], [x3,y3], [x4,y4]],
            "text": "识别的文字",
            "score": 0.95
        },
        ...
    ],
    "elapse": 0.123  # 识别耗时（秒）
}
```

### 其他格式

```python
# 纯文本格式
result = recognize_text(pixmap, return_format="text")
# "第一行文字\n第二行文字\n..."

# 列表格式
result = recognize_text(pixmap, return_format="list")
# ["第一行文字", "第二行文字", ...]
```

## ⚙️ 配置选项

### 图像预处理

```python
result = recognize_text(
    pixmap,
    enable_grayscale=True,   # 灰度转换（提升速度）
    enable_upscale=False,    # 图像放大（提升小字识别率）
    upscale_factor=1.5       # 放大倍数（1.0-3.0）
)
```

### 性能优化建议

- **灰度转换**: 建议启用（~5ms），提升识别速度
- **图像放大**: 仅在小字模糊时启用（~30-50ms）
- **异步识别**: 必须使用 QThread，避免 UI 卡顿

## 🔧 依赖安装

```bash
pip install rapidocr-onnxruntime
```

## 📝 注意事项

1. **单例模式**: OCRManager 是全局单例，多次调用 `initialize_ocr()` 不会重复初始化
2. **线程安全**: OCR 识别在后台线程执行，结果加载在主线程
3. **内存管理**: 窗口关闭时必须清理 OCR 线程和文字层
4. **坐标系统**: 使用归一化坐标（0-1），支持窗口缩放

## 🎯 集成检查清单

- [ ] 异步初始化 OCR 文字层
- [ ] 处理窗口关闭事件
- [ ] 处理窗口缩放事件
- [ ] 与绘图工具集成
- [ ] 安装 RapidOCR 依赖
- [ ] 测试文字选择功能
- [ ] 测试复制粘贴功能
