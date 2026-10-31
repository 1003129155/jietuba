<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="截图吧 logo" />

# 截图吧

**Windows 截图、屏幕绘制、OCR、钉图、翻译与剪贴板工具**

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[下载 Windows 版](https://github.com/1003129155/jietuba/releases/latest) · [从源码构建](#source-setup) · [开发与测试](#development)

**简体中文** | [繁體中文](README_zh-TW.md) | [English](README.md) | [日本語](README_JA.md) | [한국어](README_KO.md)

</div>

## 项目简介

截图吧是一款免费开源的 Windows 截图与效率工具，集截图标注、屏幕绘制、OCR、翻译、万物钉图、录屏与剪贴板历史于一体。打通截图、钉图与剪贴板，截下来的内容随时找回、随手复用。

## 截图 ⇄ 钉图 ⇄ 剪贴板，三向互通

截完存入历史，从历史一键钉回屏幕；钉图继续标注、取字，再复制回剪贴板，三者联动，一气呵成。

- **丰富截图工具，丝滑操作**：智能吸附窗口与控件，十余种标注工具随手可用；四向长截图、自动滚动、多屏与混合 DPI、HDR 截图。
- **万物钉图**：一键钉住你需要的内容，文本、图片、文件，边看边用；缩放、标注、取字、翻译，随手完成。
- **剪贴板记录一切**：不仅仅是自己的截图，任何来源复制即记录，分组收藏、搜索筛选、多选拖出，重要内容随时找回；任何内容都可以直接拖拽到想要的窗口，或者钉在桌面。
- **流畅又省内存**：性能深度优化，响应迅速，低配电脑的复杂场景也能流畅运行；实测空闲时常驻内存（工作集）可低至约 3 MB，高强度使用时内存占用同样很低。免安装，解压即用。

由 C++（Qt6）实现，图像处理、剪贴板、PP-OCR 引擎等核心由 Rust 实现，支持 Windows x86_64 与 ARM64。早期的 Python（PySide6）实现已归档到 [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) 分支，不再更新。

## 功能一览

<table>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="截图与标注" src="https://github.com/user-attachments/assets/397d6a3a-51a6-4860-9d05-0aed2c7c0429" />
<p><b>截图与标注</b><br>智能识别窗口和控件，画笔、箭头、序号、马赛克、聚光灯、水印等十余种标注，随时撤销重做。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="剪贴板历史" src="https://github.com/user-attachments/assets/d34ff233-fe7f-49dd-89ae-c5978fa25b5a" />
<p><b>剪贴板历史</b><br>记录一切，万物钉图。分组收藏、搜索筛选、多选拖出，复制过的内容随时找回。</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="滚动长截图" src="https://github.com/user-attachments/assets/a179b3e7-d201-4208-b518-81cecc4e9661" />
<p><b>滚动长截图</b><br>上下左右四个方向自动拼接，可自动滚动；往回滚只定位不截短，一键裁掉多余部分。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="OCR 文字识别" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR 文字识别</b><br>Windows 11 截图工具 OCR 与 PP-OCR 双引擎，离线识别，自动合并折行、整理标点。</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="钉图" src="https://github.com/user-attachments/assets/d03e74e7-576b-46eb-8aea-21cc96d6a020" />
<p><b>万物钉图</b><br>一键钉在屏幕上，边看边用。缩放、标注、取字、翻译，随手完成。</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="GIF 与 MP4 录制" src="https://github.com/user-attachments/assets/284f09b1-24b2-4569-9bc7-278c3bcd34da" />
<p><b>GIF / MP4 录制</b><br>录制中可标注，GIF 可回放剪辑；MP4 可调帧率码率，可录系统声音和鼠标指针，不录麦克风。</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<p><b>翻译</b><br>选中文字，一键翻译。支持 DeepL、Google、Azure、Amazon、百度、火山引擎、腾讯云、有道、DeepSeek 与 OpenAI 兼容接口，结果可朗读。</p>
</td>
<td width="50%" valign="top">
<p><b>屏幕绘制</b><br>直接在屏幕上书写、标注，边操作边讲解；白板和光标高亮，让演示、教学更直观。</p>
</td>
</tr>
</table>

还有：

- **扫码**：识别选区里的二维码和条形码，一次多个。
- **全局鼠标快捷键**：按住修饰键拖动，直接截图、钉图、识别文字、翻译或扫码（二维码 / 条形码）。
- **其他**：PDF 导出、截图水印、延时截图、五种界面语言（简中、繁中、英、日、韩）。


---

## 下载与使用

1. 打开 [Releases 下载页](https://github.com/1003129155/jietuba/releases/latest)，按设备下载以 `-x64.zip` 或 `-arm64.zip` 结尾的程序包，无需安装运行环境。
2. 完整解压后双击其中的 exe 启动。

|  | 完整版 `jietuba_pp-…zip` | 轻量版 `jietuba_lite-…zip` |
|---|---|---|
| 包内文件 | `jietuba_pp.exe` 和同级的 `models/` 目录（都要保留） | 单个 `jietuba_lite.exe` |
| OCR 引擎 | 截图工具 OCR + 自带 PP-OCR，无需安装系统截图工具也能识别 | 默认使用 Windows 11 截图工具的 OCR，可在设置里下载 [PP-OCR 组件包](#ppocr-pack) |
| 适合 | Windows 10，或想要 OCR 开箱即用 | Windows 11，想要体积小 |

两个版本默认自动选择可用引擎：优先使用 Windows 11 截图工具 OCR，否则尝试 PP-OCR；可在设置的「OCR 设置」中手动切换。识别语种取决于引擎和模型，完整版自带的 PP-OCRv6 small 支持中、英、日。截图工具低于 11.2308（没有「文本操作」）或未安装截图工具时，轻量版需要可用的截图工具 OCR 或 PP-OCR 组件包才能识别文字。

> [!NOTE]
> 通过浏览器下载后，Windows 可能显示运行警告。出现提示时，点击「更多信息」，再选择「仍要运行」即可启动。

> [!WARNING]
> 两个版本共用设置和剪贴板历史，可以互相换用，但不要同时运行，默认快捷键会冲突。

> [!TIP]
> 设置默认保存在 `%APPDATA%\Jietuba\settings.ini`。在 exe 旁放一个 `settings.ini`（空文件也可以），程序就改用它，复制程序文件夹即可带走设置。截图保存目录、剪贴板历史位置、开机自启这类只对本机有意义的项仍留在注册表；剪贴板历史默认在 `%APPDATA%\pyclipboard`，可在设置的「剪贴板」页更换位置。

### 更新

首次安装包含更新器的版本后，可在**关于 → 检查更新 → 一键更新并重启**中升级。更新器下载匹配架构的 GitHub Release ZIP，仅替换主 EXE 后重启；保留设置、剪贴板历史、模型和其他文件。录制、导出或后台保存未结束时保留下载结果，结束任务后重试。最近一次主程序备份保存在 `.jietuba-update/` 中，详见[更新器恢复说明](rust_libs/updater/README.md)。2.1.8 之前的 Python 版一键更新后会换成 C++ 实现，设置、剪贴板历史和模型照常保留。

<a id="ppocr-pack"></a>

### PP-OCR 组件包（轻量版）

轻量版带 PP-OCR 的前后处理，但不带 ONNX Runtime 和模型。可在设置的「管理 PP-OCR 模型」中下载：模型从 ModelScope 获取，ONNX Runtime 从微软发布的 `onnxruntime` wheel 中提取，下载源包括 PyPI 和镜像站；下载后按内置清单校验文件大小与哈希。支持 PP-OCRv6、PP-OCRv5 和多种语种识别模型，也可以添加本地 ONNX 文件。下载、添加、切换都不用重启。

也可以手动放进 exe 旁的 `models/` 目录：`PP-OCRv6_det_small.onnx`、`PP-OCRv6_rec_small.onnx`（仓库的 [models/](models/) 里就有）和 ONNX Runtime 1.24 及以上的纯 CPU 版 `onnxruntime.dll`。

## 代码签名政策

Windows 发布包中的 `jietuba_pp.exe` 和 `jietuba_lite.exe` 均由 GitHub Actions 从本仓库 [cpp/](cpp/) 的 C++ 源码构建。发布流程支持通过 SignPath 进行代码签名；是否签名取决于该次构建配置，请以具体发布包的 EXE 数字签名和发布说明为准。

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- 提交者、评审者、批准人：[@1003129155](https://github.com/1003129155)
- 外部贡献通过 Pull Request 提交，经维护者审核后才会合并。

<a id="privacy"></a>

### 隐私说明

截图、OCR、标注、录屏和剪贴板历史在本机处理和保存。所有联网功能均需你手动触发，程序不会自动联网。具体包括：

- 日常截图、OCR 识别和剪贴板记录不需要上传内容；使用 PP-OCR 前需准备好本地模型和运行库。
- 翻译时，待翻译文字会发送到设置中选定的服务，并使用你配置的凭据；该服务的隐私政策适用。自定义 OpenAI 兼容接口也可以指向本机服务，此时文字发送到该本地端点。
- 在**关于 → 检查更新**中检查或安装更新时，程序会访问 GitHub 获取版本信息并下载发布包。
- 在「管理 PP-OCR 模型」中下载模型或运行库时，会访问 ModelScope、PyPI 或内置清单中的镜像源；这些请求用于下载组件，不上传待识别图片。
- 在翻译设置中测试连接或获取模型列表时，会请求你配置的服务端点。连接测试发送固定示例文字；获取模型列表不发送剪贴板内容。

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

发布用的单文件版静态链接 Qt 和 MSVC 运行库，无需另行安装这两项；OCR 所需模型与 ONNX Runtime 见上方组件包说明。先用 `.\scripts\build_static_qt.ps1` 从 Qt 官方源码编一份静态 SDK，再构建：

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

[C++ CI](.github/workflows/cpp.yml) 在每个 PR 上编译、运行离屏测试并检查行覆盖率；桌面测试另行运行，其失败不阻断该检查。改到 `rust_libs/` 时，[Rust CI](.github/workflows/rust.yml) 运行 Rust 单元测试并复查第三方许可。

发行由 [build.yml](.github/workflows/build.yml) 完成：标签必须是 `release-<VERSION 文件里的版本>`；[lite.yml](.github/workflows/lite.yml) 构建轻量版和完整版（x64 与 ARM64），再由发布流程按配置签名、打包，分别生成 `jietuba_lite-<tag>-<arch>.zip` 和 `jietuba_pp-<tag>-<arch>.zip`。

---

## 目录结构总览

```text
# 项目根目录
├── README.md / README_zh-CN.md / README_zh-TW.md / README_JA.md / README_KO.md   # 英文、简中、繁中、日文、韩文说明文档
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
│   ├── gifrecorder/         # GIF 帧存储、回放与合成编码
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

各模块的详细说明见 [docs/MODULES.md](docs/MODULES.md)。

---

## 许可

版权和 MIT 许可见 [LICENSE](LICENSE)；Qt 与其他第三方组件的说明见 [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md)。
