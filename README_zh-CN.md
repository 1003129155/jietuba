<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="截图吧 logo" />

# 截图吧

**Windows 截图、OCR、钉图、翻译与剪贴板工具**

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[下载 Windows 版](https://github.com/1003129155/jietuba/releases/latest) · [从源码构建](#source-setup) · [开发与测试](#development)

**中文** | [English](README.md) | [日本語](README_JA.md)

</div>

## 项目简介

截图吧是一款免费开源的 Windows 截图工具，截图、标注、OCR、翻译、钉图、录屏和剪贴板历史都在一个程序里，所有功能本地运行。

由 C++（Qt6）实现，图像处理、剪贴板、PP-OCR 引擎等核心由 Rust 实现，支持 Windows x86_64 与 ARM64。早期的 Python（PySide6）实现已归档到 [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) 分支，不再更新。

## 功能一览

<table>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="截图与标注" src="https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827" />
<p><b>截图与标注</b><br>智能识别窗口和控件，画笔、箭头、序号、马赛克、聚光灯、水印等十余种标注，随时撤销重做。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="剪贴板历史" src="https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a" />
<p><b>剪贴板历史</b><br>复制过的文本、图片、文件自动入史，分组永久保存；Win+V 直接打开，多选、拖出、一键钉到屏幕。</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="滚动长截图" src="https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a" />
<p><b>滚动长截图</b><br>上下左右四个方向自动拼接，可自动滚动；往回滚只定位不截短，一键裁掉多余部分。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="OCR 文字识别" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR 文字识别</b><br>Windows 11 截图工具 OCR 与 PP-OCR 双引擎，离线识别，自动合并折行、整理标点。</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="钉图" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />
<p><b>钉图</b><br>截图置顶在屏幕上，可缩放、旋转、继续标注；直接选中图里的文字复制或翻译，也能切成可编辑的文字。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="GIF 与 MP4 录制" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
<p><b>GIF / MP4 录制</b><br>录制中可标注，GIF 可回放剪辑；MP4 可调帧率码率，可录系统声音和鼠标指针，不录麦克风。</p>
</td>
</tr>
</table>

还有：

- **屏幕实时标注**：直接在正在操作的屏幕上画，没选工具时鼠标照常操作下方窗口，适合演示和讲课；带白板和光标高亮（指针发光、点击涟漪）。从托盘菜单「屏幕标注」或 设置 → 快捷键 → 屏幕标注 的热键开始。
- **翻译**：选中文字一键翻译，支持 DeepL、Google、Azure、Amazon、百度、火山引擎、腾讯云、有道、DeepSeek 和 OpenAI 兼容接口，结果可朗读。
- **扫码**：识别选区里的二维码和条形码，一次多个。
- **全局鼠标快捷键**：按住修饰键拖动，直接截图、钉图、识别文字、翻译或扫码（二维码 / 条形码）。
- **其他**：PDF 导出、截图水印、延时截图、五种界面语言（简中、繁中、英、日、韩）。

## 核心亮点

「截图 ⇄ 剪贴板 ⇄ 钉图」三向互通的顺滑体验

- **任意来源自动入史**：不只是自己截的图，复制的任何文本、图片、文件、HTML 都会进入同一份剪贴板历史，可分组永久保存，一键导出到其他设备。
- **历史 → 钉图 → 继续工作**：历史记录里的任何一张图都能一键「钉」回屏幕最前端，随时拿来对照、缩放、继续标注、重新 OCR。
- **功能打磨到细节**：滚动截图支持上下左右全方向毫秒级精准拼接，多屏幕、多 DPI 感知无缝截图，导出多种图片编码。各功能都比肩付费软件甚至更好。
- **内存占用低**：严格控制运行内存，连续复杂使用后常驻内存也能压到很低（往往只有十几 MB 甚至几 MB）。
- **流畅 UI**：优化各种吃配置的场景，降低 CPU 占用，低配电脑也能流畅使用。
- **数据安全**：完全本地运行，不收集数据、不偷偷联网，数据全部保存在本地；翻译是唯一需要联网的功能，只在你主动使用时调用你自己选择的翻译 API。

---

## 下载与使用

1. 打开 [Releases 下载页](https://github.com/1003129155/jietuba/releases/latest)，按设备下载以 `-x64.zip` 或 `-arm64.zip` 结尾的程序包，无需安装运行环境。
2. 完整解压后双击其中的 exe 启动。

|  | 完整版 `jietuba_pp-…zip` | 轻量版 `jietuba_lite-…zip` |
|---|---|---|
| 包内文件 | `jietuba_pp.exe` 和同级的 `models/` 目录（都要保留） | 单个 `jietuba_lite.exe` |
| OCR 引擎 | 截图工具 OCR + 自带 PP-OCR，任何 Windows 都能识别 | 默认只用 Windows 11 截图工具的 OCR，可在设置里下载 [PP-OCR 组件包](#ppocr-pack) |
| 适合 | Windows 10，或想要 OCR 开箱即用 | Windows 11，想要体积小 |

两个版本默认都优先使用截图工具的 OCR（更快、支持的语言更多），可在设置的「OCR 设置」中切换引擎。截图工具低于 11.2308（没有「文本操作」）或没有截图工具的电脑上，轻量版要先更新截图工具或下载 PP-OCR 组件包才能识别文字。

> [!NOTE]
> 通过浏览器下载后，Windows 可能显示运行警告。出现提示时，点击「更多信息」，再选择「仍要运行」即可启动。

> [!WARNING]
> 两个版本共用设置和剪贴板历史，可以互相换用，但不要同时运行，默认快捷键会冲突。

> [!TIP]
> 设置默认保存在 `%APPDATA%\Jietuba\settings.ini`。在 exe 旁放一个 `settings.ini`（空文件也可以），程序就改用它，复制程序文件夹即可带走设置。截图保存目录、剪贴板历史位置、开机自启这类只对本机有意义的项仍留在注册表；剪贴板历史默认在 `%APPDATA%\pyclipboard`，可在设置的「剪贴板」页更换位置。

### 更新

首次安装包含更新器的版本后，可在**关于 → 检查更新 → 一键更新并重启**中升级。更新器下载匹配架构的 GitHub Release ZIP，仅替换主 EXE 后重启；保留设置、剪贴板历史、模型和其他文件。录制、导出或后台保存未结束时保留下载结果，结束任务后重试。最近一次主程序备份保存在 `.jietuba-update/` 中，详见[更新器恢复说明](rust_libs/updater/README.md)。2.1.8 之前的 Python 版一键更新后会换成 C++ 实现，设置、剪贴板历史和模型照常保留。

WinGet 发布接入已准备：发布流程会附带清单，公共源尚需提交审核。发布步骤及上架后的安装命令见 [WinGet 发布说明](docs/winget.md)。

<a id="ppocr-pack"></a>

### PP-OCR 组件包（轻量版）

轻量版带 PP-OCR 的前后处理，但不带 ONNX Runtime 和模型。最简单的做法是在设置的「管理 PP-OCR 模型」里下载：ONNX Runtime 取自 PyPI 上微软发布的 `onnxruntime` wheel，下载后校验哈希；模型有 PP-OCRv6、PP-OCRv5 和多种语种识别模型可选，也可以从本地 ONNX 文件添加。下载、添加、切换都不用重启。

也可以手动放进 exe 旁的 `models/` 目录：`PP-OCRv6_det_small.onnx`、`PP-OCRv6_rec_small.onnx`（仓库的 [models/](models/) 里就有）和 ONNX Runtime 1.24 及以上的纯 CPU 版 `onnxruntime.dll`。

## 代码签名政策

Windows 发布包中的 `jietuba_pp.exe` 和 `jietuba_lite.exe` 都由 GitHub Actions 从本仓库 [cpp/](cpp/) 的 C++ 源码自动构建，并通过 SignPath 签名。

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- 提交者、评审者、批准人：[@1003129155](https://github.com/1003129155)
- 外部贡献通过 Pull Request 提交，经维护者审核后才会合并。

### 隐私说明

除非用户明确要求，本程序不会向其他联网系统传输任何信息。

- 截图、OCR 和剪贴板历史都在本机处理和保存。
- 使用翻译时，待翻译的文字会发送到你在设置中选择的翻译服务（DeepL、Google、Azure、Amazon、百度、火山引擎、腾讯云、有道、DeepSeek 或自行配置的 OpenAI 兼容接口），使用你自己的 API 密钥，并适用该服务的隐私政策。
- 在**关于 → 检查更新**中检查或安装更新时，程序会访问 GitHub 获取版本信息并下载发布包。

---

<a id="source-setup"></a>

## 从源码构建

需要 Visual Studio 的「使用 C++ 的桌面开发」（含 CMake）、Rust MSVC 工具链和 Qt 6.11 的 MSVC SDK。在 `cpp/` 目录下用 PowerShell 执行：

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py   # 下载 Qt SDK 到 .sdk/
.\build.cmd                                         # 编译、测试并部署到 dist/Release
```

已有 Qt6 MSVC x64 SDK 时，用 `.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64`；`-SkipTests` 跳过测试，`-Configuration Debug` 编 Debug，编完运行 `run.cmd` 即可。

发布用的单文件版静态链接 Qt 和 MSVC 运行库，产物只依赖系统 DLL。先用 `.\scripts\build_static_qt.ps1` 从 Qt 官方源码编一份静态 SDK，再构建：

```powershell
.\build.cmd -Static          # 轻量版：dist/Release-static/jietuba_lite.exe
.\build.cmd -Static -Full    # 完整版：dist/Release-static-full/jietuba_pp.exe，内嵌 ONNX Runtime，附默认 PP-OCR 模型
```

ARM64 版在 x64 上交叉编译：先编好 x64 静态 Qt，再运行 `.\scripts\build_static_qt.ps1 -Arch arm64` 和 `.\build.cmd -Static -Arch arm64`，需要 Visual Studio 的 MSVC ARM64 生成工具和 `rustup target add aarch64-pc-windows-msvc`。版本号取自仓库根目录的 `VERSION` 文件。

PP-OCR 模型放在仓库的 [models/](models/) 目录中，包括 `PP-OCRv6_det_small.onnx` 和 `PP-OCRv6_rec_small.onnx`。截图工具 OCR 所需文件取自本机的 Windows 11 截图工具，首次使用时自动复制到 `%LOCALAPPDATA%\Jietuba\oneocr`，不需要另外下载。

---

<a id="development"></a>

## 开发与测试

`.\build.cmd` 会编译并运行全部测试，其中名字以 `_desktop` 结尾的测试会在真实桌面上注入键鼠、注册全局热键、截屏，运行期间不要操作鼠标和键盘。只跑不需要桌面的测试：

```powershell
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
ctest --test-dir build\msvc -C Release --output-on-failure -E "_desktop$|_windows_parity$"
```

测试程序都在临时沙盒里运行，不会读写本机的设置、剪贴板历史和日志。另有分析构建（`scripts\check.ps1`：AddressSanitizer、代码分析、泄漏检测）和覆盖率（`scripts\coverage.ps1`）。

[C++ CI](.github/workflows/cpp.yml) 在每个 PR 上编译、运行测试并检查改动代码的行覆盖率；改到 `rust_libs/` 时，[Rust CI](.github/workflows/rust.yml) 运行 Rust 单元测试并复查第三方许可。

发行由 [build.yml](.github/workflows/build.yml) 完成：标签必须是 `release-<VERSION 文件里的版本>`；[lite.yml](.github/workflows/lite.yml) 构建轻量版和完整版（x64 与 ARM64），签名后分别生成 `jietuba_lite-<tag>-<arch>.zip` 和 `jietuba_pp-<tag>-<arch>.zip`。

---

## 目录结构总览

```text
# 项目根目录
├── README.md / README_zh-CN.md / README_JA.md   # 英文、中文、日文说明文档
├── VERSION                  # 版本号，CMake 与发版流程都读它
├── app.ico                  # 程序与更新器图标
├── licenses/                # 发布包附带的第三方许可
│
├── cpp/                     # C++（Qt6）实现
│   ├── src/                 # 源码，按 <包>/<模块> 组织
│   │   ├── annotation/      # 屏幕实时标注 — 每屏一层、穿透切换、跨屏撤销
│   │   ├── barcode/         # 二维码/条形码识别（zxing-cpp）
│   │   ├── canvas/          # 画布 — 图形编辑核心、撤销、图层
│   │   ├── capture/         # 截图捕获 — 屏幕截图、窗口与元素识别
│   │   ├── clipboard/       # 剪贴板管理 — 历史、分组/快速启动、导入导出、搜索
│   │   ├── core/            # 核心 — 启动引导、日志、资源、主题、国际化、快捷键
│   │   ├── gif/             # GIF/MP4 录制、GIF 编辑、回放、导出
│   │   ├── ocr/             # 截图工具 OCR 与 PP-OCR
│   │   ├── pin/             # 钉图 — 置顶、编辑、OCR、翻译
│   │   ├── settings/        # 统一配置管理
│   │   ├── stitch/          # 长截图拼接
│   │   ├── text_recognition/ # OCR 结果排版
│   │   ├── tools/           # 绘图工具 — 笔、矩形、箭头、文字、马赛克等
│   │   ├── translation/     # 多提供商翻译与朗读
│   │   ├── ui/              # 界面组件、截图窗口、工具栏、设置窗口
│   │   └── video/           # MP4 录制子进程
│   ├── native/              # rust_libs 的 C 接口，静态链接
│   ├── resources/           # 默认设置、界面翻译（XML 与 .qm）、日志文案
│   ├── scripts/             # 构建、Qt SDK 准备、翻译编译
│   └── tests/               # QtTest 测试与对照数据
│
├── rust_libs/               # Rust 库源码，由 C++ 通过 cpp/native 静态链接
│   ├── gifrecorder/         # GIF/视频合成编码器
│   ├── longstitch/          # 长截图拼接算法
│   ├── pyclipboard/         # 剪贴板底层操作
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 识别引擎
│   ├── hdrcapture/          # HDR 桌面截图
│   ├── updater/             # 只替换主 EXE 的独立更新器
│   └── video_recorder/      # Media Foundation 录制与 WASAPI 系统声音采集
│
├── models/                  # PP-OCR ONNX 模型（完整版随包发布）
│   ├── PP-OCRv6_det_small.onnx   # 文本检测模型 (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 文本识别模型 (CRNN/CTC)
│
└── svg/                     # SVG 图标资源
```

## 模块详细说明

### annotation/ — 屏幕标注模块

在正在操作的屏幕上直接画。每块屏幕一层整屏透明窗口，复用 GIF 录制的绘制层（`GifDrawingView`）和截图编辑的工具栏（`Toolbar`，布局配置 `toolbar_layout::ANNOTATION`），工具、设置面板和撤销都是同一套。

<details>
<summary>展开目录结构</summary>

```text
annotation/
├── screen_annotation.h/.cpp   # ScreenAnnotation — 会话：每屏一层、穿透/绘制/隐藏三态、跨屏撤销、焦点归还；ScreenAnnotationShortcutHandler — 绘制时的按键
├── annotation_view.h/.cpp     # AnnotationView — 一块屏幕的绘制层，作为画布宿主把共用工具栏交给 CanvasView
├── annotation_toolbar.h/.cpp  # AnnotationToolbar — 浮动工具栏：穿透、清空、隐藏、退出；不抢焦点，不进截图和录屏
└── cursor_highlight.h/.cpp    # CursorHighlight 与 PointerLight — 指针周围的光和点击涟漪
```

</details>

**核心功能：**
- 穿透态窗口带 `WS_EX_TRANSPARENT`，鼠标直达下方窗口；选了工具才接管鼠标，回到穿透时把焦点还给原来的前台窗口
- 每块屏幕一个画布和撤销栈，会话记下每一步改了哪些屏，撤销和重做按绘制顺序跨屏进行；清空是一步可撤销的操作
- 插拔屏幕、改分辨率时增删或调整对应的图层，工具栏总在可见屏幕内
- 截图前先退回穿透并等这一帧上屏：截图带上画出的内容，不带命中层；工具栏用 `WDA_EXCLUDEFROMCAPTURE` 排除在截图和录屏之外
- 图层里笔迹下面没有截图底图，荧光笔没法和下方内容正片叠底，改为 35% 不透明度覆盖；一笔一次画完，颜色均匀，矩形荧光的边框和内部作为一个形状一次填充
- 刚画完的图形不自动选中，屏幕上和录屏里不会出现控制点；用对应工具点一下照样能选中修改
- 白板是画在笔迹下面的纯白底，点一下开、再点一下关；回到穿透就露出桌面，选下一个工具时白板回来
- 光标高亮从输入钩子的鼠标事件里取指针位置和按键，穿透时也有效，指针不动时什么都不跑，也不会漏掉点击；绘制时圆盘与笔画在同一次重绘里移动，截图时不带它

---

### barcode/ — 扫码模块

识别截图选区里的二维码和条形码。结果窗口左侧在截图上标出每个码的位置，右侧按序号列出内容。

<details>
<summary>展开目录结构</summary>

```text
barcode/
├── reader.h/.cpp         # read_codes / DecodedCode — 基于 zxing-cpp 解码，按阅读顺序给出内容、格式与轮廓
├── result_window.h/.cpp  # BarcodeResultWindow — 扫码结果窗口，截图与结果列表按序号对应
└── barcode.h             # show_barcode_result — 扫码入口：解码选区里的码并打开结果窗口（工具栏扫码按钮）
```

</details>

**核心功能：**
- 支持 QR Code、Data Matrix、Aztec、PDF417 及 Code 128、EAN/UPC 等常见一维码
- 一次识别选区内的多个码，悬停截图或列表中的任一项，两边同时高亮
- 内容一键复制；http(s) 链接可直接在浏览器打开

---

### canvas/ — 画布模块

图形编辑画布系统，提供场景管理、视图渲染、图形项选择和撤销/重做功能。

<details>
<summary>展开目录结构</summary>

```text
canvas/
├── scene.h/.cpp                  # CanvasScene — 画布场景，继承 QGraphicsScene
├── view.h/.cpp                   # CanvasView — 画布视图，继承 QGraphicsView，负责渲染和交互
├── selection_model.h/.cpp        # SelectionModel — 管理被选中的图形项，支持多选
├── undo.h/.cpp                   # CommandUndoStack — 撤销/重做栈，支持添加、删除、批量删除、编辑命令
├── smart_edit_controller.h/.cpp  # SmartEditController — 智能编辑控制器，处理选择与编辑模式切换
├── smart_selection_anim.h/.cpp   # SmartSelectionAnimator — 智能选区换窗口时的矩形补间
├── handle_editor.h/.cpp          # LayerEditor / EditHandle — 图层编辑器，提供控制点拖拽编辑
├── gestures.h/.cpp               # 鼠标手势状态机 — 文字边缘拖动、拉选区、挂起的单击编辑
├── handle_overlay.h/.cpp         # HandleOverlayWidget — 编辑控制点的独立合成层，避免整场景重绘
├── canvas_host.h/.cpp            # CanvasHost — 承载画布视图的窗口向视图提供的接口（工具栏、放大镜等），截图窗口与钉图窗口共用
└── items/                        # 绘制图形项
    ├── drawing_items.h/.cpp      # RectItem / EllipseItem / NumberItem，以及各绘制项共用的 DrawingItemMixin
    ├── stroke_item.h/.cpp        # StrokeItem — 画笔/荧光笔笔迹，只重绘变了的、露出来的那几块
    ├── background_item.h/.cpp    # BackgroundItem — 选区背景底图
    ├── mosaic_item.h/.cpp        # MosaicItem — 马赛克图形项
    ├── spotlight_item.h/.cpp     # SpotlightItem / SpotlightCurtain — 聚光灯的孔与共用幕布
    ├── selection_item.h/.cpp     # SelectionItem — 选中项的边界显示框
    ├── arrow_item.h/.cpp         # ArrowItem — 箭头图元，九种样式的箭杆/端头/描边几何
    ├── text_item.h/.cpp          # TextItem — 文字图元，描边/阴影/背景色块与三态交互框
    ├── watermark_item.h/.cpp     # WatermarkItem / WatermarkStyle — 盖在选区上的水印层：平铺、菱形网格或放在七个位置之一，支持多行和时间
    └── item_reference.h          # ItemReference / ItemLease — 图元所有权与生命周期：场景持有已挂载的图元，撤销命令持有已摘下的图元
```

</details>

**核心功能：**
- 基于 Qt Graphics View Framework 的画布系统
- 支持自由绘制、矩形、椭圆、箭头、文字、编号等图形项
- 完整的撤销/重做机制（基于 QUndoStack）
- 智能编辑控制器实现选择与绘制模式无缝切换
- 控制点编辑器支持图形变换

---

### capture/ — 截图捕获模块

屏幕截图和窗口智能识别的核心服务。

<details>
<summary>展开目录结构</summary>

```text
capture/
├── capture_service.h/.cpp           # CaptureService — 截图服务，屏幕截图核心逻辑
├── display_watcher.h/.cpp           # DisplayChangeWatcher — 插拔显示器、改分辨率、开关 HDR 后在后台重建截图会话
├── system_cursor.h/.cpp             # SystemCursor — 记录截屏时的鼠标指针并画进截图
├── quick_capture_controller.h/.cpp  # QuickCaptureController — 修饰键拖动截图与动作分发
├── window_finder.h/.cpp             # WindowFinder — 窗口查找器，智能选择窗口，识别光标下的窗口
└── msaa_element_finder.h/.cpp       # MSAAElementFinder — 按点识别：后台线程用 MSAA 查询鼠标下的控件及其父级链，供智能选区吸附
```

</details>

**核心功能：**
- 全屏截图和区域截图
- 智能窗口识别与选择（自动排除窗口阴影）
- 光标位置窗口检测
- 鼠标下控件的按点识别（MSAA），智能选区逐层吸附，首帧就给出选区

---

### clipboard/ — 剪贴板管理模块

剪贴板历史管理系统，分为 controllers、core、services、ui 四层结构，支持文本、图片、HTML、文件等类型。
不仅能保存截图历史，还提供独立的三栏管理窗口用于维护分组与内容，并可从历史记录生成钉图。

<details>
<summary>展开目录结构</summary>

```text
clipboard/
├── controllers/                        # 控制层 — 历史加载、粘贴流程、右键菜单、选择状态
│   ├── clipboard_controller.h/.cpp     # ClipboardController — 历史加载、粘贴和菜单逻辑
│   ├── selection_manager.h/.cpp        # SelectionManager — 列表选择状态管理
│   ├── context_menu_controller.h/.cpp  # ClipboardContextMenuController — 右键菜单数据与行为组装
│   ├── foreground_tracker.h/.cpp       # ForegroundWindowTracker — 记住粘贴目标窗口
│   └── paste_keystroke.h/.cpp          # 把焦点还给目标窗口后发送 Ctrl+V
├── core/                               # 数据层 — pyclipboard 封装、数据模型、分组类型
│   ├── manager.h/.cpp                  # ClipboardManager — 数据存储、监听、粘贴 API
│   ├── models.h/.cpp                   # ClipboardItem / Group — 剪贴板数据模型
│   └── text_transform.h/.cpp           # 纯文本加工函数 — 供「特殊粘贴」调用，全部无副作用
├── services/                           # 服务层 — 文件 payload、分组规则、导入导出、保存逻辑
│   ├── file_payload_service.h/.cpp     # file 类型 JSON payload 与旧格式兼容
│   ├── group_service.h/.cpp            # 分组图标、命名与删除确认辅助
│   ├── import_export_service.h/.cpp    # 文本条目 CSV 导入/导出
│   └── manage_dialog_service.h/.cpp    # 管理窗口保存逻辑
└── ui/
    ├── layout_scale.h/.cpp             # 管理窗口共用尺寸常量
    ├── image_item_actions.h/.cpp       # 图片条目的读取与另存为，剪贴板窗口和管理窗口共用
    ├── group_kind_style.h/.cpp         # 分组类型的标识色与线条图标，侧栏行和类型卡片共用
    ├── dialogs/
    │   └── manage_dialog.h/.cpp        # 三栏管理窗口：分组、内容、导入导出
    ├── forms/
    │   ├── group_form.h/.cpp
    │   ├── text_content_form.h/.cpp
    │   ├── file_content_form.h/.cpp
    │   ├── import_export_form.h/.cpp
    │   ├── group_icon_picker.h/.cpp
    │   ├── form_widgets.h/.cpp
    │   └── image_content_form.h/.cpp
    ├── menus/
    │   └── action_menu.h/.cpp
    ├── mixins/
    │   └── frameless_mixin.h/.cpp
    ├── panels/
    │   └── setting_panel.h/.cpp
    ├── resources/
    │   └── emoji_data.h/.cpp
    ├── theme/
    │   ├── themes.h/.cpp
    │   ├── theme_styles.h/.cpp
    │   └── chevron.h/.cpp              # 按颜色和尺寸绘制并缓存的下拉箭头图片，供样式表使用
    ├── widgets/
    │   ├── group_bar.h/.cpp
    │   ├── item_delegate.h/.cpp
    │   ├── item_drag.h/.cpp            # 拖出历史条目：文本、HTML、图片和文件，目标要文件时才写出
    │   ├── selection_bar.h/.cpp        # 多选时列表下方的操作栏：数量、粘贴、保存与其他操作
    │   ├── preview_popup.h/.cpp
    │   ├── quick_edit_popup.h/.cpp
    │   ├── manage_rows.h/.cpp
    │   └── reorder_list.h/.cpp
    └── windows/
        ├── clipboard_window.h/.cpp     # 历史窗口、搜索、预览与快捷粘贴
        └── pin_window.h/.cpp           # 从历史条目创建钉图
```

</details>

**核心功能：**
- 监听系统剪贴板变化，自动保存历史记录
- 支持文本、图片、HTML、文件等多种格式
- 支持通用分组、快速启动分组、筛选分组（固定显示某一类历史）、收藏与搜索
- 独立三栏管理窗口，可编辑分组、文本内容和文件内容
- 文本条目支持 CSV 导入/导出
- 多主题 UI（亮色/暗色等）
- 快捷键快速粘贴历史内容
- 多选后批量粘贴、保存或执行其他操作；条目可直接拖进编辑器、聊天框或资源管理器
- 数据位置可在设置里更换：把当前数据迁移过去，或直接改用目标文件夹里已有的数据
- 默认用 Win+V 打开本软件的剪贴板，代替系统剪贴板历史（剪贴板热键的第 3 个槽）；清空这一槽即交还给系统
- 预览弹窗支持大图/长文查看

---

### core/ — 核心基础模块

提供日志、资源加载、主题管理、国际化、快捷键等基础设施。

<details>
<summary>展开目录结构</summary>

```text
core/
├── bootstrap.h/.cpp            # PreloadManager — 启动引导，环境初始化、DPI感知、单实例控制、链式预加载
├── data_location.h/.cpp        # 设置文件的位置：exe 旁有 settings.ini 就用它，否则用 %APPDATA%\Jietuba
├── game_mode_monitor.h/.cpp    # GameModeMonitor — 跟随前台切换判断游戏是否在前台，供「玩游戏时暂停热键」使用
├── elevation.h/.cpp            # 以管理员身份运行 — 提权状态判断、开机自启用的计划任务
├── logger.h/.cpp               # Logger — 文件+控制台日志系统，支持多级别 (debug/info/warning/error/exception)
├── crash_handler.h/.cpp        # install_crash_hooks() — 全局异常和线程异常捕获
├── resource_manager.h/.cpp     # ResourceManager — SVG/图片等资源加载管理
├── theme.h/.cpp                # ThemeManager — 应用级主题颜色管理
├── ui_scale.h/.cpp             # UIScaleManager — 工具栏/面板/弹层共用的缩放比例
├── i18n.h/.cpp                 # I18nManager / XmlTranslator / tr() — 国际化管理，多语言支持
├── shortcut_manager.h/.cpp     # HotkeySystem / ShortcutManager — 全局热键和应用内快捷键管理
├── global_hotkey.h/.cpp        # 全局热键的键名表与注册方式判定：系统注册、钩子接管或不允许
├── input_hub.h/.cpp            # 进程共用的全局输入：钩子不占界面线程，事件转成 Qt 信号
├── input_engine.h/.cpp         # 输入状态机 — 手势、热键、快捷键录入、旁听订阅
├── input_hooks.h/.cpp          # 独立线程上的低层鼠标键盘钩子与前台窗口钩子
├── quick_capture_input.h/.cpp  # 全局鼠标快捷键的拖动输入，接在共用输入中心上
├── last_capture_region.h/.cpp  # 进程内存的"上次截图区域"，供恢复选区快捷键使用
├── save.h/.cpp                 # SaveService — 文件保存服务（自动命名、路径管理、高质量 PDF 输出）
├── export.h/.cpp               # ExportService — 图像导出服务
├── clipboard_utils.h/.cpp      # copy_image_to_clipboard() — 图像复制到系统剪贴板
├── platform_utils.h/.cpp       # DPI感知设置、AppUserModelID、进程管理等 Windows API 工具
├── qt_utils.h/.cpp             # safe_disconnect() — Qt 信号安全断开工具
├── constants.h/.cpp            # 全局常量定义（字体、路径等）
├── background_tasks.h/.cpp     # 后台保存任务登记
├── update_cache.h/.cpp         # 更新器临时文件与缓存清理
├── updater_process.h/.cpp      # Rust 更新器 JSONL 异步进程接口
├── update_controller.h/.cpp    # 更新下载与安全重启协调
├── update_checker.h/.cpp       # GitHub 最新版本异步查询与版本比较
├── ui_theme.h/.cpp             # UIThemeManager — 应用窗口与原生 Qt 控件的明暗外观（截图配色仍在 theme.h/.cpp）
├── east_asian_width.h/.cpp     # is_east_asian_wide — 码点是否为全角或宽字符（东亚宽度 W/F）
├── east_asian_width.inc        # 东亚宽度表（脚本生成）
├── hooks.h                     # HookList — 挂在对象扩展点上的回调列表，按注册顺序调用，可按 id 单独移除
├── html_entities.inc           # HTML 命名字符引用表（脚本生成）
├── html_unescape.h/.cpp        # html_unescape — 解码 HTML 命名与数字字符引用
├── http_client.h/.cpp          # http_request — 经系统 WinHTTP 的一次阻塞式请求，沿用系统代理，区分 HTTP 错误、超时与网络失败，可取消
├── native.h                    # Rust 库 C 接口（cpp/native）的声明
├── text_utils.h/.cpp           # left_chars — 按字符数截断，不切开代理对
└── serial_executor.h           # SerialExecutor — 在一条专用线程上按提交顺序逐个执行任务
```

</details>

**核心功能：**
- 统一的日志系统，支持文件输出和控制台输出
- 全局崩溃处理，自动捕获未处理异常
- SVG/图片资源统一加载
- 应用级亮色/暗色主题管理
- XML 格式的多语言国际化系统（中/英/日/韩/繁中）
- 全局热键系统（基于 Windows API）和应用内快捷键管理；托盘菜单开启「玩游戏时暂停热键」后，游戏在前台时全局热键暂停
- 文件保存/导出服务（含高质量 PDF 输出）
- 以管理员身份运行：计划任务开机自启免 UAC，普通启动时转交给管理员实例
- 系统 HTTP 请求（WinHTTP，沿用系统代理）和 Rust 库的 C 接口声明

---

### gif/ — GIF 录制模块

屏幕录制、编辑、回放和导出为 GIF/视频。

在录制工具栏选择 **MP4** 即可直接录制 H.264 视频，支持自定义 1–60 fps、0.5–50 Mbps 目标码率、系统声音和鼠标指针开关。可用时优先使用硬件编码，否则回退到 Windows 软件编码。录制由独立的子进程完成（同一个 exe 带 `--video-recorder-worker` 启动），逐帧流式写入视频，界面进程不保存视频帧历史；停止后完成 MP4 封装才报告保存成功。MP4 直接保存，不进入 GIF 编辑和回放流程。画面输出为 SDR，HDR 显示器上经 DXGI 抓取后映射为 SDR，不提供 HDR 视频；系统声音取默认播放设备（最多 8 声道，多声道降混为立体声），不包含麦克风。

> [!NOTE]
> 系统需要可用的 Media Foundation、H.264/AAC 组件；Windows N 版本可能需要 Media Feature Pack。发行包不附加 FFmpeg 或其他编解码器包。

<details>
<summary>展开目录结构</summary>

```text
gif/
├── record_window.h/.cpp        # GifRecordWindow / AppState — 主控制窗口，状态机协调器（管理3层窗口）
├── overlay.h/.cpp              # CaptureOverlay / OverlayMode — 捕获覆盖层，选区调整界面
├── drawing_view.h/.cpp         # GifDrawingView / GifDrawingScene — 录制中绘图编辑视图
├── record_toolbar.h/.cpp       # RecordToolbar — 录制控制工具栏（开始/暂停/停止）
├── frame_recorder.h/.cpp       # FrameRecorder / FrameData / CursorSnapshot — 帧录制器，采样屏幕帧和光标
├── video_recorder.h/.cpp       # VideoRecorder — 对 MP4 录制子进程的有界 JSONL 控制
├── video_settings.h/.cpp       # RecordingOptions — 独立的视频帧率、码率及声音设置
├── playback_engine.h/.cpp      # PlaybackEngine / PlayState — 回放引擎，帧播放和预览
├── playback_controller.h/.cpp  # PlaybackController — 回放控制器，管理回放UI和导出
├── playback_toolbar.h/.cpp     # PlaybackToolbar / RangeSlider — 回放工具栏，进度条、速度控制
├── composer.h/.cpp             # ComposeWorker / ComposerProgressDialog — GIF合成，将帧合成为GIF/视频
├── cursor_overlay.h/.cpp       # CursorOverlay — 光标渲染和点击动画
├── _widgets.h/.cpp             # ClickMenuButton / svg_icon() — 自定义小部件
└── gifrecorder.h/.cpp          # gifrecorder 的 C++ 句柄封装 — 帧存储、帧解码、录制会话、光标精灵
```

</details>

**核心功能：**
- 以状态机模式管理录制流程（选区 → 录制 → 回放 → 导出）
- 三层窗口架构：覆盖层（选区）、绘制层（标注）、工具栏层
- 帧采样录制，支持光标捕获和点击动画
- 回放预览，支持范围裁剪、速度调节
- 导出为 GIF 或视频格式（调用 Rust 库 gifrecorder）

---

### ocr/ — OCR 文字识别模块

文字识别管理，支持 Windows 截图工具自带的 OCR 和 PP-OCR 两个引擎。

<details>
<summary>展开目录结构</summary>

```text
ocr/
├── ocr_manager.h/.cpp        # OCRManager — 引擎选择、加载、释放与统一识别接口
├── ocr_line_geometry.h/.cpp  # 识别行的阅读框与逐字位置，引擎没给出时按字宽估算
├── ppocr_models.h/.cpp       # PP-OCR 模型：models/ 目录、内置清单、添加、删除，经更新器下载
├── snipping_tool_ocr.h/.cpp  # SnippingToolOcr — 查找截图工具、复制 OCR 文件、补边识别
└── bundled_runtime.h/.cpp    # bundled_runtime — 完整版 exe 内嵌的 onnxruntime.dll：启动时 models/ 里缺失或不可用就写入
```

</details>

**核心功能：**
- 自动检测引擎与模型可用性
- 设置里可选「自动 / Windows 截图工具 / PP-OCR」；自动时有截图工具就用它，否则用 PP-OCR
- 截图工具 OCR：直接调用 Windows 11 截图工具自带的识别组件，更快、支持的语言更多（含韩文、俄文、泰文等），需要 Windows 11 截图工具
- PP-OCR：ppocr_rust 引擎（纯 Rust + ONNX Runtime，PP-OCR det + rec），任何 Windows 都能用；完整版自带运行库和模型，轻量版在设置里下载
- PP-OCR 模型：完整版自带 PP-OCRv6 small；设置里的「管理 PP-OCR 模型」可从内置清单下载其他模型（PP-OCRv6 tiny/medium、PP-OCRv5，以及韩文、拉丁字母、西里尔字母、泰文、希腊文识别），文件哈希固定；也可以添加本地 ONNX 模型，试识别通过才列入。模型放在程序旁的 `models/`；选中的模型不在时直接报缺少模型，不换用别的
- 识别与释放互斥；切换引擎时释放旧引擎，新引擎在下次识别时加载
- 单例模式管理，统一的识别接口，返回文字和位置信息

---

### pin/ — 钉图模块

将截图固定在屏幕上，支持编辑、缩放、OCR识别、翻译等。

<details>
<summary>展开目录结构</summary>

```text
pin/
├── pin_window.h/.cpp             # PinWindow — 钉图主窗口，可拖动、缩放、编辑的置顶窗口
├── pin_canvas_view.h/.cpp        # PinCanvasView — 钉图画布视图（唯一内容渲染者）
├── pin_canvas.h/.cpp             # 钉图画布对象
├── pin_manager.h/.cpp            # PinManager — 管理所有钉图窗口（单例）
├── pin_toolbar.h/.cpp            # PinToolbar — 钉图工具栏
├── pin_controls.h/.cpp           # PinControlButtons — 控制按钮（关闭、编辑、复制等）
├── pin_hover.h/.cpp              # PinHoverControls — 悬停按钮和工具栏的显隐决策
├── pin_context_menu.h/.cpp       # PinContextMenu — 右键菜单
├── pin_border_overlay.h/.cpp     # PinBorderOverlay — 边框效果覆盖层
├── pin_click_through_bar.h/.cpp  # PinClickThroughBar — 鼠标穿透时的移动 / 不透明度 / 退出工具条
├── pin_ocr_manager.h/.cpp        # PinOCRManager / PinOcrThread — 钉图OCR管理（异步识别）
├── pin_shortcut.h/.cpp           # PinShortcutController — 快捷键控制（普通模式/编辑模式）
├── pin_thumbnail.h/.cpp          # PinThumbnailMode — 缩略图模式
├── pin_translation.h/.cpp        # PinTranslationHelper — 翻译助手
├── pin_image_transform.h/.cpp    # PinImageTransform — 图像变换（旋转、翻转等）
├── ocr_text_layout.h/.cpp        # OcrTextLayout — 钉图上识别出的文字与选区，以图像像素记录
├── pin_text_selection.h/.cpp     # PinTextSelection — 在钉图上拖选、双击、Ctrl+A 选中文字并高亮
└── pin_text_mode.h/.cpp          # PinTextPanel / PinTextEdit — 文字模式：把钉图换成可编辑的识别文字
```

</details>

**核心功能：**
- 截图结果钉在屏幕最前端，支持拖拽移动和滚轮缩放
- 钉图上可直接进行绘制编辑
- 集成 OCR 识别，可在钉图上直接选中文字并复制，缩放、旋转后照样能选
- 文字模式：把钉图换成可编辑的识别文字，切回图片再切回来，编辑内容和撤销记录都在
- 集成翻译功能，可直接翻译钉图中的文字
- 快捷键支持普通模式和编辑模式

---

### settings/ — 设置模块

统一的配置管理系统。

<details>
<summary>展开目录结构</summary>

```text
settings/
├── app_settings.inc          # 设置表：每项设置一行（键、类型、默认值、作用域），生成类型化访问器、默认值和导入清单
├── tool_settings.h/.cpp      # ToolSettingsManager / ToolSettings — 管理工具颜色、大小、热键等配置
├── data_folder.h/.cpp        # 启动时迁入旧版存在注册表里的设置，并让开机自启项与开关一致
├── settings_transfer.h/.cpp  # 从另一份 settings.ini 或旧版导出的 JSON 导入设置
├── color_formats.h/.cpp      # 放大镜颜色格式模板：渲染、读取、保存
└── special_paste.h/.cpp      # 特殊粘贴菜单的预置选项、顺序与启用状态
```

</details>

**核心功能：**
- 单例模式的配置管理器，所有设置登记在一张表里
- 管理各绘图工具的颜色、线宽、字体大小等参数
- 设置存进 `settings.ini`（exe 旁或 `%APPDATA%\Jietuba`），只对本机有意义的项留在注册表
- 首次启动时把旧版注册表里的设置复制进 `settings.ini`，只复制不删除，退回旧版照常可用

---

### stitch/ — 长截图拼接模块

滚动截图和自动拼接功能。

<details>
<summary>展开目录结构</summary>

```text
stitch/
├── auto_scroll.h/.cpp                  # AutoScroller — 自动滚动：按拼接结果定步长，到底或动鼠标即停
├── change_watch.h/.cpp                 # ChangeWatch — 后台盯着截图区，画面一变就截
├── incremental.h/.cpp                  # IncrementalStitcher — 后台增量拼接与预览缩略图
├── jietuba_long_stitch_unified.h/.cpp  # 长截图拼接接口（调用 Rust longstitch）
├── scroll_window.h/.cpp                # ScrollCaptureWindow — 滚动截图窗口
└── scroll_toolbar.h/.cpp               # 滚动截图工具栏
```

</details>

**核心功能：**
- 滚动页面并截图，支持横向和竖向
- 往回滚时只定位不截短，越过起点就在另一头接上，从页面中间开始截也能两头长
- 自动滚动：光标停到截图区域中间自动滚动截图，到底或鼠标一动就停
- 裁剪：往回滚到想截断的地方，一键去掉当前画面以上或以下的内容
- 文件列表、表格这类只差几个字的行也能对准位置；一下滚过头接不上时提示往回滚，不硬拼
- 基于图像匹配的智能拼接算法（查找重叠区域）
- 统一的长截图接口（调用 Rust 库 longstitch 加速）

---

### text_recognition/ — 文字识别模块

识别截图选区里的文字，只显示、不翻译：后台线程识别，结果窗口先显示“识别中…”，识别完成后在可编辑的文本框里给出文字，可一键复制。

<details>
<summary>展开目录结构</summary>

```text
text_recognition/
├── layout.h/.cpp         # layout — 把识别出的行排成文字：合并折行、去掉中日韩文字间的空格、标点全半角转换
├── placement.h/.cpp      # placement — 结果窗口的摆放位置（只算位置，不操作窗口）
├── recognizer.h/.cpp     # RecognizeThread — 在工作线程里识别一张图并回报结果
├── result_window.h/.cpp  # TextRecognitionWindow — 文字识别结果窗口：可编辑文本框、复制按钮与排版菜单
└── text_recognition.h    # recognize_async / show_text_recognition — 入口：后台识别、打开结果窗口、复制结果
```

</details>

**核心功能：**
- 后台识别，不阻塞界面；识别是一次不可中断的原生调用，取消只是不再汇报结果
- 版面整理：合并折行、去掉中日韩文字间的空格、标点全半角转换，可在窗口里切换
- 结果窗口贴近识别区域摆放，并避开光标
- 文字可编辑，一键复制

---

### tools/ — 绘图工具模块

提供各种绘图工具的实现。

<details>
<summary>展开目录结构</summary>

```text
tools/
├── base.h/.cpp            # Tool / ToolContext — 工具抽象基类和工具上下文
├── controller.h/.cpp      # ToolController — 工具控制器，管理工具切换和状态
├── action.h/.cpp          # ActionTools — 动作工具（复制、保存、取消等）
├── pen.h/.cpp             # PenTool — 自由绘制笔工具
├── rect.h/.cpp            # RectTool — 矩形工具（实心/空心）
├── ellipse.h/.cpp         # EllipseTool — 椭圆工具
├── arrow.h/.cpp           # ArrowTool — 箭头工具
├── text.h/.cpp            # TextTool — 文字工具
├── number.h/.cpp          # NumberTool — 数字编号工具（自动递增）
├── highlighter.h/.cpp     # HighlighterTool — 荧光笔工具
├── mosaic.h/.cpp          # MosaicTool — 马赛克工具
├── spotlight.h/.cpp       # SpotlightTool — 聚光灯工具（压暗框外区域）
├── watermark.h/.cpp       # WatermarkTool — 水印工具（铺上、改文字和样式、记下上次有没有水印）
├── cursor.h               # CursorTool — 光标/选择工具
├── eraser.h/.cpp          # EraserTool — 橡皮擦工具
└── cursor_manager.h/.cpp  # CursorManager — 光标样式管理器
```

</details>

**核心功能：**
- 统一的工具基类架构（Tool → 各具体工具）
- 绘图工具：笔、矩形、椭圆、箭头、文字、数字、荧光笔、马赛克、水印、光标、橡皮擦等
- 工具控制器负责工具切换、鼠标事件分发
- 工具上下文（ToolContext）提供场景、视图、设置等依赖注入

---

### translation/ — 翻译模块

多提供商翻译服务，支持 DeepL、Google、Azure、Amazon、百度、火山引擎、腾讯云、有道、DeepSeek，以及自行配置的 OpenAI 兼容接口。

<details>
<summary>展开目录结构</summary>

```text
translation/
├── provider.h/.cpp                      # TranslationProvider / ProviderMetadata — 提供商抽象契约
├── registry.h/.cpp                      # ProviderRegistry — 提供商注册表与工厂
├── service.h/.cpp                       # TranslationService — 提供商选择与翻译编排
├── models.h/.cpp                        # TranslationRequest / TranslationResult — 与提供商无关的请求/结果模型
├── worker.h/.cpp                        # TranslationWorker — 所有提供商共用的 Qt 工作线程
├── providers/                           # 各翻译提供商实现
│   ├── deepl.h/.cpp                     # DeepL
│   ├── google.h/.cpp                    # Google
│   ├── azure.h/.cpp                     # Azure
│   ├── amazon.h/.cpp                    # Amazon
│   ├── baidu.h/.cpp                     # Baidu
│   ├── tencent.h/.cpp                   # 腾讯云
│   ├── volcengine.h/.cpp                # 火山引擎
│   ├── youdao.h/.cpp                    # 有道
│   ├── custom_llm.h/.cpp                # 自定义 OpenAI 兼容服务（Ollama、LM Studio 等）
│   ├── deepseek.h/.cpp                  # DeepSeek (LLM)
│   ├── openai_compatible.h/.cpp         # OpenAI 兼容接口基类
│   └── common.h                         # 各提供商共用的响应解析与错误体读取
├── smart_translation_controller.h/.cpp  # SmartTranslationController — 一键选中文字探测与翻译弹窗路由
├── translation_popup.h/.cpp             # TranslationPopup — 紧凑翻译弹窗（选中文字/手动输入两种模式）
├── deepl_service.h/.cpp                 # DeepLService / DeepLResult — 旧版 DeepL API 异步翻译
├── languages.h                          # translation_languages / source_languages — 支持的语言列表与语言代码
├── translation_manager.h/.cpp           # TranslationManager — 翻译窗口管理器（单例）
├── translation_dialog.h/.cpp            # TranslationDialog / TranslationLoadingDialog — 翻译结果显示窗口
├── speech_service.h/.cpp                # SpeechService / SapiBackend — 翻译结果朗读：直接调用系统 SAPI，按语言匹配语音，同一时刻只读一处
└── ui/
    └── widgets.h/.cpp                   # 翻译相关小部件
```

</details>

**核心功能：**
- 可插拔的多提供商架构，注册表统一管理
- 一键快捷键：自动探测选中文字并路由到翻译弹窗
- 紧凑翻译弹窗，支持选中文字与手动输入两种模式
- 异步翻译，不阻塞 UI
- 翻译结果支持复制
- 翻译结果可朗读（调用系统语音，按语言匹配，同一时刻只读一处）

---

### translations/ — 语言资源

多语言界面文案，位于 `cpp/resources/translations/`。

<details>
<summary>展开目录结构</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # 中文/英文翻译源文件
├── app_ja.xml / app_ko.xml  # 日文/韩文翻译源文件
├── app_zh_TW.xml            # 繁体中文（台湾）翻译源文件
└── app_*.qm                 # 编译后的 Qt 二进制文件（app_zh.qm 等）
```

</details>

**说明：** `.xml` 为可编辑的翻译源文件，`.qm` 为 Qt 运行时加载的编译文件。新增或修改界面文字时，五个 `.xml` 都要补上同一上下文的条目（多行文案写真换行），再运行 `scripts/compile_translations.py` 重新编译。日志文案直接改 `resources/core/log_translations.json`。

---

### ui/ — 用户界面模块

通用 UI 组件库，为各模块提供统一的界面元素。

<details>
<summary>展开目录结构</summary>

```text
ui/
├── toolbar.h/.cpp                   # Toolbar / ToolbarDragHandle — 可拖动工具栏基类
├── toolbar_layout.h/.cpp            # 截图 / 钉图工具栏按钮排布（顺序 / 显示方式）的归一化与读写
├── toolbar_layout_dialog.h/.cpp     # ToolbarLayoutDialog — 截图 / 钉图工具栏排布对话框
├── reorderable_rows.h/.cpp          # DragGrip / DraggableRow / ReorderableRowList — 可拖动重排的行列表
├── tray_menu.h/.cpp                 # CustomTrayMenu — 系统托盘菜单
├── screenshot_window.h/.cpp         # ScreenshotWindow — 截图主窗口（全屏覆盖、选区绘制）
├── quick_capture_overlay.h/.cpp     # 快速截图透明浮层，复用普通截图的选框、坐标和放大镜
├── update_dialog.h/.cpp             # 更新说明与下载进度
├── dialogs.h/.cpp                   # StandardDialog / 对话框函数集 — 确认、警告、信息、错误对话框
├── toast.h/.cpp                     # Toast — 光标旁不抢焦点的一行轻提示
├── submenu_position.h/.cpp          # 级联子菜单的展开方向与贴边定位
├── magnifier.h/.cpp                 # MagnifierOverlay — 放大镜覆盖层（像素级取色）
├── color_picker_button.h/.cpp       # ColorPickerButton — 颜色选择按钮
├── color_picker_dialog.h/.cpp       # 取色器窗口：饱和度/明度方块、色相与不透明度滑条、吸管
├── save_image_dialog.h/.cpp         # SaveImageDialog — 保存图片对话框：常用文件夹标签、格式与质量
├── screenshot_countdown.h/.cpp      # ScreenshotCountdown — 延时截图的倒计时
├── hotkey_edit.h/.cpp               # HotkeyEdit — 全局快捷键编辑框
├── inapp_key_edit.h/.cpp            # InAppKeyEdit — 应用内快捷键编辑框
├── key_chip.h/.cpp                  # KeyChipLineEdit / StatusIcon — 快捷键按键块与状态图标
├── mask_overlay.h/.cpp              # 遮罩覆盖层
├── selection_overlay.h/.cpp         # SelectionOverlayWidget — 选区装饰浮层，压在遮罩之上
├── base_settings_panel.h/.cpp       # BaseSettingsPanel / StepperWidget — 设置面板基类
├── paint_settings_panel.h/.cpp      # PaintSettingsPanel — 画笔设置面板
├── shape_settings_panel.h/.cpp      # ShapeSettingsPanel — 形状设置面板
├── text_settings_panel.h/.cpp       # TextSettingsPanel — 文字设置面板
├── arrow_settings_panel.h/.cpp      # ArrowSettingsPanel — 箭头设置面板
├── number_settings_panel.h/.cpp     # 数字工具设置面板
├── mosaic_settings_panel.h/.cpp     # 马赛克工具设置面板
├── watermark_settings_panel.h/.cpp  # 水印面板：文字、位置、样式弹层（不透明度/大小/间距/角度/网格线/颜色）、字体
├── watermark_input_dialog.h/.cpp    # WatermarkInputDialog — 输入多行水印，可插入时间并预览
├── initial_selection.h/.cpp         # InitialSelectionGate — 截图窗口显示前等第一次按点识别返回（最多 50 ms），首帧就有选区
│
├── fluent_lite/                     # Fluent 风格轻量组件库
│   ├── buttons.h/.cpp               # 按钮
│   ├── cards.h/.cpp                 # 卡片
│   ├── icons.h/.cpp                 # 图标
│   ├── inputs.h/.cpp                # 输入框
│   ├── labels.h/.cpp                # 标签
│   ├── navigation.h/.cpp            # 导航
│   ├── segmented.h/.cpp             # 分段控件
│   ├── switch.h/.cpp                # 开关
│   ├── theme.h/.cpp                 # 主题
│   ├── titlebar.h/.cpp              # 标题栏
│   ├── frameless.h/.cpp             # 无边框窗口
│   ├── text_context_menu.h/.cpp     # 文本右键菜单
│   └── fluent_lite.h/.cpp           # 汇总头文件，以及勾选按钮
│
├── settings_ui/                     # 应用设置对话框
│   ├── dialog.h/.cpp                # SettingsDialog — 主设置对话框（选项卡式）
│   ├── components.h/.cpp            # SettingCardGroup / ToggleSwitch — 设置组件库
│   ├── page_appearance.cpp          # 外观设置页（主题、语言等）
│   ├── page_capture.cpp             # 截图设置页
│   ├── page_quick_actions.cpp       # 快捷行为设置页（跳过确认或结果窗口）
│   ├── color_format_dialog.h/.cpp   # ColorFormatDialog — 放大镜颜色格式管理窗口
│   ├── ppocr_model_dialog.h/.cpp    # PpocrModelDialog — 下载、添加、删除 PP-OCR 模型
│   ├── page_clipboard.cpp           # 剪贴板设置页
│   ├── special_paste_dialog.h/.cpp  # SpecialPasteDialog — 调整特殊粘贴菜单的顺序与开关
│   ├── page_hotkey.cpp              # 快捷键设置页
│   ├── page_mouse.cpp               # 鼠标快捷键设置页
│   ├── page_pin.cpp                 # 钉图设置页（行为、外观、鼠标穿透）
│   ├── page_screen_annotation.cpp   # 屏幕标注设置页
│   ├── page_translation.cpp         # 翻译设置页
│   ├── provider_fields.cpp          # 服务商字段的读写（按声明）
│   ├── page_log.cpp                 # 日志设置页
│   ├── page_developer.cpp           # 开发者设置页
│   ├── page_misc.cpp                # 杂项设置页
│   ├── page_about.cpp               # 关于页面
│   ├── mouse_binding_editor.h       # MouseBindingEditor — 鼠标快捷键的修饰键与手势选择器
│   └── pages.h                      # 各设置页构建函数的声明
│
├── welcome/                         # 首次启动欢迎向导
│   ├── wizard.h/.cpp                # WelcomeWizard — 欢迎向导主窗口（多页滑动）
│   ├── base_page.h/.cpp             # BasePage — 向导页面基类
│   ├── page1_welcome.cpp            # 欢迎页
│   ├── page2_screenshot.cpp         # 截图快捷键设置页
│   ├── page3_clipboard.cpp          # 剪贴板快捷键设置页
│   ├── page5_translation.cpp        # 翻译功能说明页
│   ├── page6_finish.cpp             # 完成页
│   ├── page_hotkeys.cpp             # 全局快捷键页 — 六个键同属一个冲突域，集中设置并统一判重
│   ├── page_mouse_shortcuts.cpp     # 全局鼠标快捷键页 — 按住修饰键拖动直接截图或执行动作
│   ├── page_screen_annotation.cpp   # 屏幕标注页 — 功能演示与热键
│   └── pages.h                      # 欢迎向导各页的声明
│
└── selection_info/                  # 选区信息UI
    ├── controller.h/.cpp            # 选区信息控制器
    ├── panel.h/.cpp                 # 选区信息面板（尺寸、坐标）
    ├── border_shadow.h/.cpp         # 选区边框阴影效果
    ├── lock_ratio.h/.cpp            # 锁定宽高比功能
    └── rounded_corners.h/.cpp       # 圆角截图功能
```

</details>

**核心功能：**
- 可拖动工具栏基类，所有工具栏继承自此
- 全屏截图窗口，处理选区绘制和交互
- 放大镜、取色器窗口、保存图片对话框等精细UI组件
- Fluent 风格轻量组件库（按钮、卡片、导航、分段、开关等）
- 完整的应用设置对话框（外观/截图/热键/翻译/屏幕标注等多个页面）
- 首次启动欢迎向导（8页引导流程）
- 选区信息面板（尺寸显示、宽高比锁定、圆角等）

---

### video/ — MP4 录制子进程

MP4 录制的工作进程：同一个 exe 带 `--video-recorder-worker` 参数启动，原生录制器只在这个进程里运行，画面帧不进入界面进程。命令以 JSON 行从标准输入进入，事件以 JSON 行从标准输出送出。

<details>
<summary>展开目录结构</summary>

```text
video/
└── worker.h/.cpp  # worker_main — MP4 录制子进程入口：标准输入收 JSON 行命令，标准输出回 JSON 行事件，画面帧只在原生录制器里流转
```

</details>

**核心功能：**
- 录制 H.264 视频，1–60 fps、0.5–50 Mbps 目标码率
- 可选系统声音（默认播放设备）和鼠标指针，画面为 SDR
- 停止后完成 MP4 封装才报告保存成功

---

### tests/ — 测试模块

QtTest 测试。名字以 `_desktop` 结尾的测试在真实桌面上运行，其余可离屏运行；所有测试程序都链接 `test_sandbox.cpp`，设置、数据和日志落在临时目录。

<details>
<summary>展开目录结构</summary>

```text
tests/
├── test_barcode.cpp               # 二维码/条形码识别
├── test_bundled_runtime.cpp       # 完整版内嵌运行库的写入
├── test_canvas_items.cpp          # 画布图元：控制点、边框与外观设置
├── test_canvas_parity.cpp         # 画布图形的几何、像素、撤销与所有权
├── test_canvas_view.cpp           # 画布视图
├── test_capture_finders.cpp       # 真实桌面上的窗口查找与系统光标（桌面测试）
├── test_clipboard.cpp             # 剪贴板纯逻辑与右键菜单
├── test_clipboard_backend.cpp     # 剪贴板后端：数据库与系统剪贴板监听
├── test_clipboard_controller.cpp  # 剪贴板窗口控制器：分页、筛选、分组、粘贴
├── test_clipboard_drag.cpp        # 剪贴板条目拖出：各类目标拿到的格式与按需写出的文件
├── test_clipboard_manage.cpp      # 剪贴板管理窗口
├── test_clipboard_selection.cpp   # 剪贴板列表的键盘与鼠标选择
├── test_clipboard_ui.cpp          # 剪贴板后端、窗口与设置的联动
├── test_clipboard_widgets.cpp     # 剪贴板小部件：行绘制、预览弹窗、齿轮菜单、主题
├── test_clipboard_window.cpp      # 剪贴板窗口的点击、按键与菜单
├── test_core.cpp                  # 核心：全局热键、日志文案、管理员计划任务、单实例交接
├── test_data_folder.cpp           # 设置文件位置、注册表与 INI 的分工、旧版设置迁移、开机自启项
├── test_freehand.cpp              # 画笔笔迹的平滑、吸附与分块重绘
├── test_gif.cpp                   # GIF/MP4 录制、回放与导出
├── test_gif_playback.cpp          # GIF 回放：解码与按显示尺寸播放
├── test_hdr_capture.cpp           # HDR 截图（桌面测试）
├── test_http_providers.cpp        # HTTP 客户端与可配置地址的翻译提供商
├── test_input_engine.cpp          # 输入状态机：手势、热键、快捷键录入
├── test_input_hooks.cpp           # 真实桌面上的输入钩子线程，注入键鼠驱动（桌面测试）
├── test_logger.cpp                # 日志
├── test_main_app.cpp              # 主程序：托盘、快捷键、截图流程与全局输入（桌面测试）
├── test_msaa_element_finder.cpp   # 按点识别与首帧选区
├── test_ocr_manager.cpp           # OCR 引擎与模型管理
├── test_parity.cpp                # 选区状态、最近区域、工具栏排布、缩放取整、设置默认值与持久化
├── test_pin.cpp                   # 钉图：旋转、OCR 坐标映射、窗口行为
├── test_platform.cpp              # 平台函数：进程、DPI、开机自启、崩溃日志
├── test_ppocr_models.cpp          # PP-OCR 模型的下载、添加与清单
├── test_result_windows.cpp        # OCR 与扫码结果窗口
├── test_sandbox.cpp               # 链接进每个测试程序：把设置、数据与日志指到临时目录
├── test_save_image_dialog.cpp     # 保存图片对话框：文件夹标签、格式与质量
├── test_screen_annotation.cpp     # 屏幕标注：每屏图层、穿透与焦点、跨屏撤销、插拔屏幕
├── test_screenshot_window.cpp     # 截图窗口
├── test_scroll_capture.cpp        # 滚动截图
├── test_settings_window.cpp       # 设置窗口
├── test_shortcut_manager.cpp      # 快捷键管理与全局钩子
├── test_smart_selection.cpp       # 真实窗口上的智能选区（桌面测试）
├── test_snipping_tool_ocr.cpp     # 截图工具 OCR 的接入（对着假 oneocr.dll）
├── test_stitch.cpp                # 长截图拼接
├── test_text_recognition.cpp      # 文字识别：版面整理与窗口摆放
├── test_translation.cpp           # 翻译提供商、注册表与服务
├── test_translation_speech.cpp    # 翻译朗读（用假后端，不碰系统语音）
├── test_translation_ui.cpp        # 翻译窗口与弹窗
├── test_ui_widgets.cpp            # 通用界面部件：工具栏、设置面板、快捷键编辑框等
├── test_update.cpp                # 检查更新与一键更新（对着假更新器）
├── clipboard_test_support.h       # 剪贴板测试公用的准备代码
├── caption_check.h                # 检查自绘窗口的标题栏都按同一布局（图标在左、标题随后）
├── save_dialog_answerer.h         # 测试里自动应答保存图片对话框
├── fake_oneocr.cpp                # 测试里代替截图工具 oneocr.dll 的假库（含缺函数的变体）
├── fake_updater.cpp               # 测试里代替更新器的假程序
├── leak_check.h/.cpp              # 泄漏检测模式：同一批测试跑三遍，用调试堆找泄漏
└── fixtures/                      # 预期结果数据（已提交，不再重新生成）
```

</details>

---

## 许可

版权和 MIT 许可见 [LICENSE](LICENSE)；Qt 与其他第三方组件的说明见 [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md)。
