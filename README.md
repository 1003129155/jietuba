**[中文](README.md)** | [English](README_EN.md) | [日本語](README_JA.md)

# 截图 & 剪切板管理软件 — 截图吧
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

## 项目简介

基于 PySide6和Rust 的截图软件和剪切板管理的windows平台软件。

支持区域截图、窗口智能识别、GIF录制、长截图拼接、OCR文字识别、图像钉图、翻译等功能，并内置了完整的剪切板历史管理系统，不限图片来源可以联动截图模块生成钉图或者提取文字。

编译后单文件大小大约42MB.占用内存很低，低配电脑也可以流畅

---

## 安装前置依赖

本项目依赖4个自制 Rust 库，**必须先安装这些包才能运行程序**。

### 1. 创建并激活 Python 3.11 虚拟环境

```bash
python -m venv venv311
# Windows:
venv311\Scripts\activate
```

### 2. 安装 Python 依赖

```bash
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

### 3. 安装自制 Rust 包（必须）

请在项目根目录执行：

```bash
python -m pip install gifrecorder-0.2.1-cp311-cp311-win_amd64.whl longstitch-0.3.11-cp311-cp311-win_amd64.whl pyclipboard-0.3.14-cp311-cp311-win_amd64.whl ppocr_rust-0.1.1-cp311-cp311-win_amd64.whl
```

| 包名 | 版本 | 功能 |
|------|------|------|
| `gifrecorder` | 0.2.1 | GIF/视频合成编码器 |
| `longstitch` | 0.3.11 | 长截图拼接算法 |
| `pyclipboard` | 0.3.14 | 剪切板底层操作 |
| `ppocr_rust` | 0.1.1 | PP-OCR (PaddleOCR) ONNX 文字识别引擎（纯 Rust + ONNX Runtime，需 det/rec 模型） |

> **注意：** 这些 `.whl` 文件仅适用于 Windows x86_64 + Python 3.11 环境。请勿安装到全局 Python 中。

> **OCR 模型：** `ppocr_rust` 需要 `models/` 目录下的 PP-OCR ONNX 模型（`PP-OCRv6_det_small.onnx` + `PP-OCRv6_rec_small.onnx`），仓库已内置。打包发布后请将 `models/` 放在 exe 同级目录。

**开发/构建依赖（可选）：**

```bash
python -m pip install -r requirements-dev.txt
```

### 4. 运行程序

```bash
cd main
python main_app.py
```

---

## 目录结构总览

```
# 项目根目录
├── README.md / README_EN.md / README_JA.md             # 中文、英文、日文说明文档
├── pyproject.toml                                      # Python 项目元数据与依赖声明
├── requirements.txt                                   # 运行依赖
├── requirements-dev.txt                               # 测试与构建依赖
├── build_with_ocr_onefile.py                           # PyInstaller 单文件构建脚本
├── gifrecorder-0.2.1-cp311-cp311-win_amd64.whl       # GIF录制 Rust 预编译包
├── longstitch-0.3.11-cp311-cp311-win_amd64.whl        # 长截图拼接 Rust 预编译包
├── pyclipboard-0.3.14-cp311-cp311-win_amd64.whl      # 剪切板 Rust 预编译包
├── ppocr_rust-0.1.1-cp311-cp311-win_amd64.whl        # OCR Rust 预编译包
│
├── main/                    # Python 主程序
│   ├── main_app.py          # 应用入口，系统托盘、全局快捷键、生命周期管理
│   ├── compile_translations.py  # 翻译文件编译工具（.xml → .qm）
│   │
│   ├── canvas/              # 画布模块 — 图形编辑核心
│   ├── capture/             # 截图捕获模块 — 屏幕截图与窗口识别
│   ├── clipboard/           # 剪切板管理模块 — 历史记录、分组/快速启动、导入导出、搜索
│   ├── core/                # 核心基础模块 — 启动引导、日志、资源、主题、国际化、快捷键
│   ├── gif/                 # GIF录制模块 — 屏幕录制、编辑、回放、导出
│   ├── ocr/                 # OCR模块 — PP-OCR 文字识别
│   ├── pin/                 # 钉图模块 — 截图置顶、编辑、OCR、翻译
│   ├── settings/            # 设置模块 — 统一配置管理
│   ├── stitch/              # 长截图拼接模块 — 滚动截图、自动拼接
│   ├── tools/               # 绘图工具模块 — 笔、矩形、箭头、文字等
│   ├── translation/         # 翻译模块 — DeepL API 翻译服务
│   ├── translations/        # 语言资源 — 中文/英文/日文/韩文
│   ├── ui/                  # 用户界面模块 — 通用UI组件库
│   └── tests/               # 测试模块 — 单元测试与集成测试
│
├── rust_libs/               # Rust 库源码（可自行编译）
│   ├── gifrecorder/         # GIF/视频合成编码器源码
│   ├── longstitch/          # 长截图拼接算法源码
│   ├── pyclipboard/         # 剪切板底层操作源码
│   └── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 识别引擎源码
│
├── models/                  # PP-OCR ONNX 模型文件（OCR 必需）
│   ├── PP-OCRv6_det_small.onnx   # 文本检测模型 (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 文本识别模型 (CRNN/CTC)
│
└── svg/                     # SVG 图标资源
```



## 模块详细说明

### canvas/ — 画布模块

图形编辑画布系统，提供场景管理、视图渲染、图形项选择和撤销/重做功能。

```
canvas/
├── __init__.py
├── scene.py                 # CanvasScene — 画布场景，继承 QGraphicsScene
├── view.py                  # CanvasView — 画布视图，继承 QGraphicsView，负责渲染和交互
├── selection_model.py       # SelectionModel — 管理被选中的图形项，支持多选
├── undo.py                  # CommandUndoStack — 撤销/重做栈，支持添加、删除、批量删除、编辑命令
├── smart_edit_controller.py # SmartEditController — 智能编辑控制器，处理选择与编辑模式切换
├── handle_editor.py         # LayerEditor / EditHandle — 图层编辑器，提供控制点拖拽编辑
└── items/                   # 绘制图形项
    ├── __init__.py
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / ArrowItem / TextItem / NumberItem — 所有绘制项目
    ├── background_item.py   # BackgroundItem — 选区背景底图
    └── selection_item.py    # SelectionItem — 选中项的边界显示框
```


**核心功能：**
- 基于 Qt Graphics View Framework 的画布系统
- 支持自由绘制、矩形、椭圆、箭头、文字、编号等图形项
- 完整的撤销/重做机制（基于 QUndoStack）
- 智能编辑控制器实现选择与绘制模式无缝切换
- 控制点编辑器支持图形变换

---

### capture/ — 截图捕获模块

屏幕截图和窗口智能识别的核心服务。

```
capture/
├── __init__.py
├── capture_service.py       # CaptureService — 截图服务，屏幕截图核心逻辑
└── window_finder.py         # WindowFinder — 窗口查找器，智能选择窗口，识别光标下的窗口
```

**核心功能：**
- 全屏截图和区域截图
- 智能窗口识别与选择（自动排除窗口阴影）
- 光标位置窗口检测

---

### clipboard/ — 剪切板管理模块

类似 Ditto 的剪切板历史管理系统，现已拆分为 controllers、core、services、ui 四层结构，支持文本、图片、HTML、文件等类型。
不仅能保存截图历史，还提供独立的三栏管理窗口用于维护分组与内容，并可从历史记录生成钉图。
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

```
clipboard/
├── __init__.py
├── controllers/             # 控制层 — 历史加载、粘贴流程、右键菜单、选择状态
│   ├── clipboard_controller.py   # ClipboardController — 历史加载、粘贴和菜单逻辑
│   ├── selection_manager.py      # SelectionManager — 列表选择状态管理
│   └── __init__.py
├── core/                    # 数据层 — pyclipboard 封装、数据模型、分组类型
│   ├── manager.py           # ClipboardManager — 数据存储、监听、粘贴 API
│   ├── models.py            # ClipboardItem / Group — 剪切板数据模型
│   ├── enums.py             # GroupType — 分组类型定义
│   └── __init__.py
├── services/                # 服务层 — 文件 payload、分组规则、导入导出、保存逻辑
│   ├── file_payload_service.py   # file 类型 JSON payload 与旧格式兼容
│   ├── group_service.py          # 分组图标、命名与删除确认辅助
│   ├── import_export_service.py  # 文本条目 CSV 导入/导出
│   └── manage_dialog_service.py  # 管理窗口保存逻辑
├── ui/
│   ├── dialogs/
│   │   └── manage_dialog.py      # 三栏管理窗口：分组、内容、导入导出
│   ├── forms/
│   │   ├── group_form.py
│   │   ├── text_content_form.py
│   │   ├── file_content_form.py
│   │   ├── import_export_form.py
│   │   └── group_icon_picker.py
│   ├── mixins/
│   │   └── frameless_mixin.py
│   ├── panels/
│   │   └── setting_panel.py
│   ├── resources/
│   │   └── emoji_data.py
│   ├── theme/
│   │   ├── themes.py
│   │   └── theme_styles.py
│   ├── widgets/
│   │   ├── draggable_list_widget.py
│   │   ├── group_bar.py
│   │   ├── item_delegate.py
│   │   ├── item_widget.py
│   │   └── preview_popup.py
│   └── windows/
│       ├── clipboard_window.py   # 历史窗口、搜索、预览与快捷粘贴
│       └── pin_window.py         # 从历史条目创建钉图
```

**核心功能：**
- 监听系统剪切板变化，自动保存历史记录
- 支持文本、图片、HTML、文件等多种格式
- 支持通用分组、快速启动分组、收藏与搜索
- 独立三栏管理窗口，可编辑分组、文本内容和文件内容
- 文本条目支持 CSV 导入/导出
- 多主题 UI（亮色/暗色等）
- 快捷键快速粘贴历史内容
- 预览弹窗支持大图/长文查看

---

### core/ — 核心基础模块

提供日志、资源加载、主题管理、国际化、快捷键等基础设施。

```
core/
├── __init__.py
├── bootstrap.py             # PreloadManager — 启动引导，环境初始化、DPI感知、单实例控制、链式预加载
├── logger.py                # Logger — 文件+控制台日志系统，支持多级别 (debug/info/warning/error/exception)
├── crash_handler.py         # install_crash_hooks() — 全局异常和线程异常捕获
├── resource_manager.py      # ResourceManager — SVG/图片等资源加载管理
├── theme.py                 # ThemeManager — 应用级主题颜色管理
├── i18n.py                  # I18nManager / XmlTranslator / tr() — 国际化管理，多语言支持
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — 全局热键和应用内快捷键管理
├── save.py                  # SaveService — 文件保存服务（自动命名、路径管理）
├── export.py                # ExportService — 图像导出服务
├── clipboard_utils.py       # copy_image_to_clipboard() — 图像复制到系统剪切板
├── platform_utils.py        # DPI感知设置、AppUserModelID、进程管理等 Windows API 工具
├── qt_utils.py              # safe_disconnect() — Qt 信号安全断开工具
└── constants.py             # 全局常量定义（字体、路径等）
```

**核心功能：**
- 统一的日志系统，支持文件输出和控制台输出
- 全局崩溃处理，自动捕获未处理异常
- SVG/图片资源统一加载
- 应用级亮色/暗色主题管理
- XML 格式的多语言国际化系统（中/英/日）
- 全局热键系统（基于 Windows API）和应用内快捷键管理
- 文件保存/导出服务

---

### gif/ — GIF 录制模块

屏幕录制、编辑、回放和导出为 GIF/视频。
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />

```
gif/
├── __init__.py
├── record_window.py         # GifRecordWindow / AppState — 主控制窗口，状态机协调器（管理3层窗口）
├── overlay.py               # CaptureOverlay / OverlayMode — 捕获覆盖层，选区调整界面
├── drawing_view.py          # GifDrawingView / GifDrawingScene — 录制中绘图编辑视图
├── drawing_toolbar.py       # GifDrawingToolbar — 绘制工具栏
├── record_toolbar.py        # RecordToolbar — 录制控制工具栏（开始/暂停/停止）
├── frame_recorder.py        # FrameRecorder / FrameData / CursorSnapshot — 帧录制器，采样屏幕帧和光标
├── playback_engine.py       # PlaybackEngine / PlayState — 回放引擎，帧播放和预览
├── playback_controller.py   # PlaybackController — 回放控制器，管理回放UI和导出
├── playback_toolbar.py      # PlaybackToolbar / RangeSlider — 回放工具栏，进度条、速度控制
├── composer.py              # _ComposeWorker / ComposerProgressDialog — GIF合成，将帧合成为GIF/视频
├── cursor_overlay.py        # CursorOverlay — 光标渲染和点击动画
└── _widgets.py              # ClickMenuButton / svg_icon() — 自定义小部件
```

**核心功能：**
- 以状态机模式管理录制流程（选区 → 录制 → 回放 → 导出）
- 三层窗口架构：覆盖层（选区）、绘制层（标注）、工具栏层
- 帧采样录制，支持光标捕获和点击动画
- 回放预览，支持范围裁剪、速度调节
- 导出为 GIF 或视频格式（调用 Rust 库 gifrecorder）

---

### ocr/ — OCR 文字识别模块

支持 PP-OCR 引擎的文字识别管理。
<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

```
ocr/
├── __init__.py
└── ocr_manager.py           # OCRManager — 基于 ppocr_rust (PP-OCR) 的文字识别
```

**核心功能：**
- 自动检测引擎与模型可用性
- 基于 ppocr_rust 引擎（纯 Rust + ONNX Runtime，PP-OCR det + rec），推理在原生线程运行不阻塞 UI
- 支持中/英/日文识别
- 单例模式管理，统一的识别接口，返回文字和位置信息

---

### pin/ — 钉图模块

将截图固定在屏幕上，支持编辑、缩放、OCR识别、翻译等。
<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />
```

pin/
├── __init__.py
├── pin_window.py            # PinWindow — 钉图主窗口，可拖动、缩放、编辑的置顶窗口
├── pin_canvas_view.py       # PinCanvasView — 钉图画布视图（唯一内容渲染者）
├── pin_canvas.py            # 钉图画布对象
├── pin_manager.py           # PinManager — 管理所有钉图窗口（单例）
├── pin_toolbar.py           # PinToolbar — 钉图工具栏
├── pin_controls.py          # PinControlButtons — 控制按钮（关闭、编辑、复制等）
├── pin_context_menu.py      # PinContextMenu — 右键菜单
├── pin_border_overlay.py    # PinBorderOverlay — 边框效果覆盖层
├── pin_ocr_manager.py       # PinOCRManager / _OCRThread — 钉图OCR管理（异步识别）
├── pin_shortcut.py          # PinShortcutController — 快捷键控制（普通模式/编辑模式）
├── pin_thumbnail.py         # PinThumbnailMode — 缩略图模式
├── pin_translation.py       # PinTranslationHelper — 翻译助手
├── pin_image_transform.py   # PinImageTransform — 图像变换（旋转、翻转等）
└── ocr_text_layer.py        # OCRTextLayer / OCRTextItem — OCR文字层显示
```

**核心功能：**
- 截图结果钉在屏幕最前端，支持拖拽移动和滚轮缩放
- 钉图上可直接进行绘制编辑
- 集成 OCR 识别，显示可选中文字层
- 集成翻译功能，可直接翻译钉图中的文字
- 快捷键支持普通模式和编辑模式

---

### settings/ — 设置模块

统一的配置管理系统。

```
settings/
├── __init__.py
└── tool_settings.py         # ToolSettingsManager / ToolSettings — 管理工具颜色、大小、热键等配置
```

**核心功能：**
- 单例模式的配置管理器
- 管理各绘图工具的颜色、线宽、字体大小等参数
- 持久化存储到 QSettings

---

### stitch/ — 长截图拼接模块

滚动截图和自动拼接功能。
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)
```

stitch/
├── __init__.py
├── jietuba_long_stitch.py           # 长截图拼接算法核心
├── jietuba_long_stitch_unified.py   # 统一长截图接口
├── scroll_window.py                 # ScrollCaptureWindow — 滚动截图窗口
└── scroll_toolbar.py                # 滚动截图工具栏
```

**核心功能：**
- 滚动页面并截图，支持横向和竖向
- 基于图像匹配的智能拼接算法（查找重叠区域）
- 统一的长截图接口（调用 Rust 库 longstitch 加速）

---

### tools/ — 绘图工具模块

提供各种绘图工具的实现。

```
tools/
├── __init__.py
├── base.py                  # Tool / ToolContext — 工具抽象基类和工具上下文
├── controller.py            # ToolController — 工具控制器，管理工具切换和状态
├── action.py                # ActionTools — 动作工具（复制、保存、取消等）
├── pen.py                   # PenTool — 自由绘制笔工具
├── rect.py                  # RectTool — 矩形工具（实心/空心）
├── ellipse.py               # EllipseTool — 椭圆工具
├── arrow.py                 # ArrowTool — 箭头工具
├── text.py                  # TextTool — 文字工具
├── number.py                # NumberTool — 数字编号工具（自动递增）
├── highlighter.py           # HighlighterTool — 荧光笔/马赛克工具
├── cursor.py                # CursorTool — 光标/选择工具
├── eraser.py                # EraserTool — 橡皮擦工具
└── cursor_manager.py        # CursorManager — 光标样式管理器
```

**核心功能：**
- 统一的工具基类架构（Tool → 各具体工具）
- 11种绘图工具：笔、矩形、椭圆、箭头、文字、数字、荧光笔、光标、橡皮擦等
- 工具控制器负责工具切换、鼠标事件分发
- 工具上下文（ToolContext）提供场景、视图、设置等依赖注入

---

### translation/ — 翻译模块

基于 DeepL API 的文字翻译服务。

```
translation/
├── __init__.py
├── deepl_service.py         # DeepLService / TranslationThread — DeepL API 异步翻译
├── languages.py             # SupportedLanguages — DeepL 支持的语言列表与语言代码
├── translation_manager.py   # TranslationManager — 翻译窗口管理器（单例）
├── translation_dialog.py    # TranslationDialog / TranslationLoadingDialog — 翻译结果显示窗口
└── ui/
    ├── __init__.py
    ├── dialog.py            # 翻译对话框UI组件
    └── widgets.py           # 翻译相关小部件
```

**核心功能：**
- 调用 DeepL API 进行文字翻译
- 异步翻译，不阻塞 UI
- 翻译结果弹窗显示，支持复制

---

### translations/ — 语言资源

多语言翻译文件存放目录。

```
translations/
├── app_zh.xml               # 中文翻译源文件
├── app_en.xml               # 英文翻译源文件
├── app_ja.xml               # 日文翻译源文件
├── app_zh.qm                # 中文编译后二进制文件
├── app_en.qm                # 英文编译后二进制文件
└── app_ja.qm                # 日文编译后二进制文件
```

**说明：** `.xml` 为可编辑的翻译源文件，`.qm` 为 Qt 运行时加载的编译文件。修改翻译后需运行 `compile_translations.py` 重新编译。

---

### ui/ — 用户界面模块

通用 UI 组件库，为各模块提供统一的界面元素。

```
ui/
├── __init__.py
├── toolbar.py               # Toolbar / _DragHandle — 可拖动工具栏基类
├── screenshot_window.py     # ScreenshotWindow — 截图主窗口（全屏覆盖、选区绘制）
├── dialogs.py               # StandardDialog / 对话框函数集 — 确认、警告、信息、错误对话框
├── magnifier.py             # MagnifierOverlay — 放大镜覆盖层（像素级取色）
├── color_picker_dialog.py   # ColorPickerDialog — 自定义HSV颜色选择器
├── color_picker_button.py   # ColorPickerButton — 颜色选择按钮
├── hotkey_edit.py           # HotkeyEdit — 全局快捷键编辑框
├── inapp_key_edit.py        # InAppKeyEdit — 应用内快捷键编辑框
├── mask_overlay.py          # 遮罩覆盖层
├── base_settings_panel.py   # BaseSettingsPanel / StepperWidget — 设置面板基类
├── paint_settings_panel.py  # PaintSettingsPanel — 画笔设置面板
├── shape_settings_panel.py  # ShapeSettingsPanel — 形状设置面板
├── text_settings_panel.py   # TextSettingsPanel — 文字设置面板
├── arrow_settings_panel.py  # ArrowSettingsPanel — 箭头设置面板
├── number_settings_panel.py # 数字工具设置面板
│
├── settings_ui/             # 应用设置对话框
│   ├── __init__.py
│   ├── dialog.py            # SettingsDialog — 主设置对话框（选项卡式）
│   ├── components.py        # SettingCardGroup / ToggleSwitch — 设置组件库
│   ├── page_appearance.py   # 外观设置页（主题、语言等）
│   ├── page_capture.py      # 截图设置页
│   ├── page_clipboard.py    # 剪切板设置页
│   ├── page_hotkey.py       # 快捷键设置页
│   ├── page_translation.py  # 翻译设置页
│   ├── page_log.py          # 日志设置页
│   ├── page_developer.py    # 开发者设置页
│   ├── page_misc.py         # 杂项设置页
│   ├── page_about.py        # 关于页面
│   └── mock_config.py       # MockConfig — 测试用模拟配置
│
├── welcome/                 # 首次启动欢迎向导
│   ├── __init__.py
│   ├── wizard.py            # WelcomeWizard — 欢迎向导主窗口（多页滑动）
│   ├── base_page.py         # BasePage — 向导页面基类
│   ├── page1_welcome.py     # 欢迎页
│   ├── page2_screenshot.py  # 截图快捷键设置页
│   ├── page3_clipboard.py   # 剪切板快捷键设置页
│   ├── page4_smart_select.py # 智能选择说明页
│   ├── page5_translation.py # 翻译功能说明页
│   └── page6_finish.py      # 完成页
│
└── selection_info/          # 选区信息UI
    ├── __init__.py
    ├── controller.py        # 选区信息控制器
    ├── panel.py             # 选区信息面板（尺寸、坐标）
    ├── hook_manager.py      # 钩子管理
    ├── border_shadow.py     # 选区边框阴影效果
    ├── lock_ratio.py        # 锁定宽高比功能
    └── rounded_corners.py   # 圆角截图功能
```

**核心功能：**
- 可拖动工具栏基类，所有工具栏继承自此
- 全屏截图窗口，处理选区绘制和交互
- 放大镜、颜色选择器等精细UI组件
- 完整的应用设置对话框（外观/截图/热键/翻译等多个页面）
- 首次启动欢迎向导（6页引导流程）
- 选区信息面板（尺寸显示、宽高比锁定、圆角等）

---

### tests/ — 测试模块

单元测试和集成测试。

```
tests/
├── conftest.py              # pytest 配置和公共 fixture
├── pytest.ini               # pytest 运行配置
├── run_tests.py             # 测试运行脚本
├── test_undo_stack.py       # 撤销栈测试
├── test_selection_model.py  # 选择模型测试
├── test_clipboard_api.py    # 剪切板公共 API 测试
├── test_clipboard_data.py   # 剪切板数据层测试
├── test_clipboard_manage_dialog.py # 剪切板管理窗口测试
├── test_clipboard_services.py # 剪切板服务层测试
├── test_clipboard_themes.py # 主题系统测试
├── test_clipboard_utils.py  # 剪切板工具函数测试
├── test_core_utils.py       # 核心工具类测试
├── test_crash_handler.py    # 崩溃处理测试
├── test_emoji_data.py       # Emoji数据测试
├── test_gif_data.py         # GIF数据结构测试
├── test_i18n.py             # 国际化系统测试
├── test_resource_manager.py # 资源管理器测试
├── test_save_service.py     # 保存服务测试
├── test_stitch_algorithm.py # 拼接算法测试
├── test_theme_manager.py    # 主题管理器测试
├── test_tool_settings.py    # 工具设置测试
└── test_tools_base.py       # 工具基类测试
```

---

## 外部 Rust 库依赖

主程序调用了以下自制 Rust 库（位于 `rust_libs/` 目录）：

| 库名 | 功能 |
|------|------|
| `gifrecorder` | GIF/视频合成编码器 |
| `longstitch` | 长截图拼接加速 |
| `pyclipboard` | 剪切板底层操作 |
| `ppocr_rust` | PP-OCR (PaddleOCR) ONNX 文字识别引擎 |

---
