<div align="center">

[English](README.en.md) | [日本語](README.ja.md) | [简体中文](README.md)

</div>

---

# Jietuba（截图吧） 截图工具

一款闲着无聊写的 **Windows 截图工具**，支持  
**图片编辑、钉图、OCR文字识别、长截图（横向竖向都行）、吸附窗口、自定义保存** 等主流功能。
虽然不及商业软件那么功能繁多,但是主流功能都有，且运行内存占用低(100mb-150mb)，文件构成简单，可以自己打包运行在本地环境

---

## 📸 功能

### 核心功能

- **🎯 截图**
  - 快捷键截图（默认 `Ctrl + 1`）
  - 多显示器支持 多屏合成，多dpi支持

- **🎨 丰富的编辑工具**
  - 画笔：自由绘制（按住 `Shift` 可画直线）
  - 直线 / 箭头
  - 矩形 / 椭圆：支持描边与填充
  - 文本：支持字体、颜色、大小调整
  - 荧光笔：重点标记（按住 `Shift` 可画直线）
  - 橡皮擦：擦除图层
  - 编号标注：自动递增（`Shift + 滚轮` 可修改编号）

- **📌 钉图（Pin）功能**
  - 将截图固定在桌面
  - 支持拖动、缩放（滚轮）
  - 支持截图时候编辑的内容基础上再次编辑

- **🔤 钉图后自动 OCR 文字识别**
  - 基于 **RapidOCR**（无需联网）
  - 支持中文 / 英文 / 日文（可以自己替换模型文件支持更多语言）
  - OCR 文本直接从钉图上复制

- **📜 长截图功能**
  - 竖向:识别鼠标滚轮，进行滚动截图
  - 横向：支持横向移动的网页等等可以shift按钮进行横向移动，不行的地方可以手动移动截图拼接
  - 用Rust写了自研算法，打包成了依赖包.whl文件，实现快速自动去重与拼接

---

## 🛠 环境配置

① 在虚拟环境中安装所需依赖

```bash
pip install -r requirements_with_ocr.txt

② 安装长截图拼接算法
pip install jietuba_rust-0.3.0-cp39-cp39-win_amd64.whl --文件里有

③ 替换 RapidOCR 默认模型

将以下目录中的 models 文件夹整体替换为项目提供的模型文件：

venv/Lib/site-packages/rapidocr/
├── default_models.yaml
├── config.yaml
└── models/        # 🔥 整个目录替换

④ 启动程序
python main_app.py


###🚀 初次使用说明

启动程序后，系统托盘中会出现图标
右键托盘图标可执行：
开始截图（或按 Ctrl + 1）
按 Enter 保存，按 ESC 取消

### 文件構造

```text
jietu/
├── main/                    # 主程序源码
│   ├── main_app.py          # 程序入口
│   ├── requirements_no_ocr.txt
│   ├── requirements_with_ocr.txt
│   ├── OCR_SETUP_GUIDE.md
│   │
│   ├── canvas/              # 画布系统（核心绘制引擎）
│   │   ├── model.py
│   │   ├── scene.py
│   │   ├── view.py
│   │   ├── toolbar_adapter.py
│   │   ├── layer_editor.py
│   │   ├── export.py
│   │   ├── undo.py
│   │   ├── snap_system.py
│   │   ├── cursor_decision.py
│   │   ├── smart_edit_controller.py
│   │   └── items/
│   │
│   ├── capture/             # 截图模块
│   │   ├── capture_service.py
│   │   ├── window_finder.py
│   │   └── SMART_SELECTION.md
│   │
│   ├── tools/               # 绘图工具
│   │   ├── controller.py
│   │   ├── pen.py
│   │   ├── arrow.py
│   │   ├── rect.py
│   │   ├── ellipse.py
│   │   ├── text.py
│   │   ├── highlighter.py
│   │   ├── eraser.py
│   │   ├── number.py
│   │   ├── cursor.py
│   │   └── action.py
│   │
│   ├── pin/                 # 置顶窗口模块
│   │   ├── pin_window.py
│   │   ├── pin_manager.py
│   │   ├── pin_canvas.py
│   │   ├── pin_toolbar.py
│   │   └── ocr_text_layer.py
│   │
│   ├── ocr/                 # OCR 模块
│   │   ├── ocr_manager.py
│   │   └── OCR_INTEGRATION.md
│   │
│   ├── stitch/              # 长截图拼接
│   │   ├── jietuba_long_stitch.py
│   │   ├── jietuba_long_stitch_rust.py
│   │   └── jietuba_long_stitch_unified.py
│   │
│   ├── ui/                  # UI 模块
│   │   ├── screenshot_window.py
│   │   ├── settings_window.py
│   │   ├── toolbar.py
│   │   ├── color_board.py
│   │   └── size_slider.py
│   │
│   ├── core/                # 核心模块
│   │   ├── hotkey_system.py
│   │   ├── resource_manager.py
│   │   ├── logger.py
│   │   └── save.py
│   │
│   └── settings/
│       └── tool_settings.py
│
├── packaging/               # 打包脚本
├── svg/                     # SVG 图标资源
├── models/                  # OCR 模型文件

<div align="center">

如果这个项目对你有帮助，请点一个 ⭐ Star！

Made with ❤️ by RiJyaaru

</div> ```

## License
This project is licensed under the MIT License.

