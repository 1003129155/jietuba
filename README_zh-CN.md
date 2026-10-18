**[中文](README_zh-CN.md)** | [English](README.md) | [日本語](README_JA.md)

# 截图吧 — Windows 截图、OCR、钉图、翻译与剪贴板工具

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/ci.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[下载 Windows 版](https://github.com/1003129155/jietuba/releases/latest) · [源码运行](#source-setup) · [开发与测试](#development)

<img width="1391" height="844" alt="21" src="https://github.com/user-attachments/assets/3f25de58-2780-4a28-931b-8430f764ee51" />


## 项目简介

截图吧是一款免费开源的 Windows 截图工具：区域与窗口截图、滚动长截图拼接、标注、OCR 文字识别、翻译、钉图、GIF/MP4 录制、二维码/条形码识别、PDF 导出，以及完整的剪贴板历史管理。所有功能本地运行。

MP4 区域录制支持自定义帧率和码率、暂停/继续、鼠标指针及系统声音；常见 5.1/7.1 播放设备的声音自动转成立体声，不包含麦克风。

截图吧由 C++（Qt6）实现，图像处理、剪贴板操作、PP-OCR 引擎等核心由 Rust 实现。支持 Windows x86_64 与 ARM64。早期的 Python（PySide6）实现已归档到 [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) 分支，不再更新。

提供可直接运行的 Windows 发行包，也可以从源码构建。

---

## 核心亮点

"截图 ⇄ 剪贴板 ⇄ 钉图"三向互通的顺滑体验

- **任意来源自动入史**：不只是自己截的图，复制的任何文本/图片/文件/html，都会自动进入同一份剪贴板历史，可分组永久保存一键导出多设备共享

- **历史 → 钉图 → 继续工作**：历史记录里的任何一张图，都能一键“钉”回屏幕最前端。随时拿来对照、缩放、继续标注、重新 OCR。

- **功能打磨到细节**：滚动截图支持上下左右全方向毫秒级精准拼接，多屏幕、多 DPI感知无缝截图，导出各种编码图片。各功能都比肩付费软件甚至更好。


- **内存占用**：有严格的运行内存设计，连续复杂使用场景下常驻内存也占用可以被压到非常低水平（往往仅在十几 MB 甚至几 MB 级别）

- **流畅 UI**：优化各种吃配置的场景，减少cpu占用让低配电脑也可以流畅使用

- **数据安全**：功能完全本地运行，无数据收集、无偷偷联网，数据全部保存在本地；翻译是唯一需要联网的功能，仅在你主动使用时调用你自己选择的第三方翻译 API

---

## 下载与使用

普通用户可以直接使用对应架构的 Windows x86_64 或 ARM64 发行包，无需安装运行环境或配置开发环境。

1. 打开 [Releases 下载页](https://github.com/1003129155/jietuba/releases/latest)，根据设备下载以 `-x64.zip` 或 `-arm64.zip` 结尾的程序包。
2. 程序包分两种，完整解压后双击其中的 exe 启动：
   - **完整版** `jietuba_pp-…zip`：带 PP-OCR 引擎和模型，任何 Windows 都能使用 OCR。保留 `jietuba_pp.exe` 和同级的 `models/` 目录。
   - **轻量版** `jietuba_lite-…zip`：单个 exe，不带 PP-OCR，体积更小。默认只使用 Windows 11 截图工具自带的 OCR；没有截图工具、或截图工具低于 11.2308（没有「文本操作」）的电脑上 OCR 不可用，请更新截图工具或改用完整版。在 exe 旁放上 PP-OCR 组件包也能使用 PP-OCR，见 [cpp/README.md](cpp/README.md#pp-ocr-组件包)。
3. 两个版本默认都优先使用截图工具的 OCR（更快、支持的语言更多），可在设置的「OCR 设置」中切换引擎。
4. 程序尚未进行数字签名，通过浏览器下载后，Windows 可能显示运行警告。出现提示时，点击“更多信息”，再选择“仍要运行”即可启动。

两个版本共用设置和剪贴板历史，可以互相换用，但不要同时运行（默认快捷键会冲突）。

首次安装包含更新器的版本后，可在**关于 → 检查更新 → 一键更新并重启**中升级。更新器下载匹配架构的 GitHub Release ZIP，仅替换主 EXE 后重启；保留设置、剪贴板历史、模型和其他文件。录制、导出或后台保存未结束时保留下载结果，结束任务后重试。最近一次主程序备份保存在 `.jietuba-update/` 中，详见[更新器恢复说明](rust_libs/updater/README.md)。2.1.8 之前的 Python 版一键更新后会换成 C++ 实现，设置、剪贴板历史和模型照常保留。

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

`.\build.cmd -Static` 构建发布用的轻量版单文件 exe，`-Static -Full` 构建完整版；静态 Qt 的准备、PP-OCR 组件包等完整说明见 [cpp/README.md](cpp/README.md)。

PP-OCR 模型放在仓库的 [models/](models/) 目录中，包括 `PP-OCRv6_det_small.onnx` 和 `PP-OCRv6_rec_small.onnx`。截图工具 OCR 所需文件取自本机的 Windows 11 截图工具，首次使用时自动复制到 `%LOCALAPPDATA%\Jietuba\oneocr`，不需要另外下载。

### Rust 扩展包

[rust_libs/](rust_libs/) 里的七个库也作为独立的 Python 扩展包发布到 PyPI，可以单独使用；截图吧本身由 C++ 直接链接它们的 Rust 核心，不依赖这些包。PyPI 发行名与 Python import 名对应如下：

| pip 包名 | import 名 | 版本 | 功能 |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.4.1 | GIF/视频合成编码器 |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.5.1 | 长截图拼接算法 |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.7 | 剪贴板底层操作 |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.3.0 | PP-OCR (PaddleOCR) ONNX 文字识别引擎（纯 Rust + ONNX Runtime，需 det/rec 模型） |
| [`j-hdrcapture`](https://pypi.org/project/j-hdrcapture/) | `hdrcapture` | 0.1.0 | 支持 HDR 的桌面截图（DXGI Desktop Duplication + GPU 色调映射） |
| [`j-input`](https://pypi.org/project/j-input/) | `inputhub` | 0.1.2 | 不占界面线程的全局鼠标键盘钩子（拖动手势、侧键、热键、滚轮、前台窗口） |
| [`j-video`](https://pypi.org/project/j-video/) | `video_recorder` | 0.1.0 | Media Foundation H.264/MP4 录制与 WASAPI 系统声音采集 |

七个扩展面向 Windows x86_64 和 ARM64，声明 Python `>=3.11` 并启用 `abi3-py311`，配置详见各包的 `pyproject.toml` 和 `Cargo.toml`。

---

<a id="development"></a>

## 开发与测试

构建、测试和开发约定见 [cpp/README.md](cpp/README.md)。[C++ CI](.github/workflows/cpp.yml) 在每个 PR 上编译并运行不需要桌面的测试；改到 `rust_libs/` 时，[Rust CI](.github/workflows/rust-wheels.yml) 运行 Rust 单元测试、复查第三方许可并构建 wheel。会注入真实键鼠、注册热键或截屏的桌面测试只在开发机上运行。

自动发行流程见 [build.yml](.github/workflows/build.yml)：标签必须是 `release-<VERSION 文件里的版本>`；由 [lite.yml](.github/workflows/lite.yml) 构建轻量版和完整版（x64 与 ARM64），签名后分别生成 `jietuba_lite-<tag>-<arch>.zip` 和 `jietuba_pp-<tag>-<arch>.zip`。

### C++ 实现

自 2.1.8 起只有 C++ 实现（[cpp/](cpp/)），发布的两个包都由它构建，都是单个静态 exe：轻量版 `jietuba_lite.exe`（`build.ps1 -Static`）；完整版 `jietuba_pp.exe`（`-Static -Full`）另内嵌 ONNX Runtime，包里带默认 PP-OCR 模型 `models/`。旧版更新器按包名精确匹配，只替换 zip 根目录的同名 exe、保留 `models/`，所以包名和 exe 名不能改；完整版的 `onnxruntime.dll` 由 exe 在启动时补进 `models/`。版本号只写在仓库根目录的 `VERSION` 文件里。

Python（PySide6）版归档在 [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) 分支，不再更新。C++ 测试里已有的纯逻辑夹具是当年由它生成的，已提交、不再重新生成；新功能直接在 C++ 里实现，预期值写在 C++ 测试里。

### Rust 库分层

`rust_libs/` 下的逻辑只写一份，C++ 版和发布到 PyPI 的 Python 扩展共用：

- 每个库分两层：核心是纯 Rust；pyo3 绑定只放在 `python.rs`（录制库是 `bindings.rs`），由默认开启的 `python` 特性控制。C++ 版以 `default-features = false` 依赖，在自己的 C 接口层（`cpp/native/`）调用同一份核心；新增能力只需接到 `cpp/native/`，不必再写 pyo3 绑定。
- 绑定层只做类型转换、GIL 的拿放、错误映射和对象生命周期；判断、默认值、重试、线程这类逻辑写进核心。
- 核心里可恢复的错误返回 `Result`，不要 `panic`。
- 不依赖 Qt 的非界面逻辑优先写进 Rust 核心；用到 Qt 类型的界面代码留在 C++ 里。

### 代码注释

注释只说明代码本身表达不了的约束和设计原因，要短、要准：

- 不写排查过程、修改历史、事故复盘和开发时的推演；
- 不提其他软件或项目，也不拿它们当依据；
- 不写成段的说明文，代码、函数名和测试名已经说清楚的不再重复。

---

## 目录结构总览

```text
# 项目根目录
├── README.md / README_zh-CN.md / README_JA.md   # 英文、中文、日文说明文档
├── VERSION                  # 版本号，CMake 与发版流程都读它
├── 托盘.ico                 # 程序与更新器图标
├── licenses/                # 发布包附带的第三方许可
│
├── cpp/                     # C++（Qt6）实现
│   ├── src/                 # 源码，按 <包>/<模块> 组织
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
├── rust_libs/               # Rust 库源码，C++ 实现与 PyPI 扩展共用
│   ├── gifrecorder/         # GIF/视频合成编码器
│   ├── longstitch/          # 长截图拼接算法
│   ├── pyclipboard/         # 剪贴板底层操作
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 识别引擎
│   ├── hdrcapture/          # HDR 桌面截图
│   ├── inputhub/            # 全局鼠标与键盘钩子
│   ├── updater/             # 只替换主 EXE 的独立更新器
│   └── video_recorder/      # Media Foundation 录制与 WASAPI 系统声音采集
│
├── models/                  # PP-OCR ONNX 模型（完整版随包发布）
│   ├── PP-OCRv6_det_small.onnx   # 文本检测模型 (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 文本识别模型 (CRNN/CTC)
│
└── svg/                     # SVG 图标资源
```
