# 截图吧轻量版（C++）开发说明

截图吧的轻量版由这里的 C++ Qt6 源码构建，完整版仍由仓库根目录的 Python 源码打包。两个版本功能、界面、配置、快捷键与边界行为一致；整机验收记录见 [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) 和 `docs/validation`。

源码位于 `src/<包>/`，与 Python 版 `main/<包>/` 按模块对应；函数对应表位于 `migration`。对照测试执行 Python 代码生成预期结果，再用 C++ 比较；与 Python 版的差异见 [迁移记录](docs/MIGRATION.md)。

运行程序使用 C++17 和 Qt Widgets，不加载 Python、PySide6 或 Python 扩展。Rust 库直接用仓库的 `rust_libs/` 源码，经 `native/` 的 C 接口静态链接。当前本机配置：Windows x64、MSVC 19.50、Qt 6.11.0（与 Python 版所用 PySide6 相同）。

## 直接运行

双击 `run.cmd`，或运行 `dist/Release/jietuba_cpp.exe`，请保留 EXE 同目录下的 DLL 和插件目录。`.\build.cmd -Static` 出的单文件版在 `dist/Release-static`，只依赖系统 DLL。

功能和操作见仓库根目录的 README。全局快捷键注册失败时仍可使用按钮与托盘。设置、剪贴板历史和日志目录与 Python 版共用（见 `docs/MIGRATION.md`「与 Python 版共用」），两个版本不要同时运行：默认热键会冲突，剪贴板历史会被两个进程同时写。

## 编译

本机的 Qt SDK 放在 `.sdk/6.11.0/msvc2022_64`，下载 SDK 用的 Python 虚拟环境放在 `.tools`。它们不进入版本控制，也不是 EXE 的运行依赖。

安装 Visual Studio 的“使用 C++ 的桌面开发”和 CMake 工具后：

```powershell
.\build.cmd
# 或使用自己的 Qt6 MSVC x64 SDK：
.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64
```

脚本配置 CMake、编译 Release、运行 QtTest，并使用 `windeployqt` 生成 `dist/Release`。Debug 使用 `-Configuration Debug`。SDK 只装 qtbase、qtsvg 和 qtimageformats（存 WebP、读 TIFF 等靠它的插件），因此部署时可能提示缺少 Qt 翻译目录；程序中文文案内置，Qt 标准对话框的按钮可能显示英文。

跨机器恢复开发环境时，用脚本从 Qt 官方下载站取 qtbase、qtsvg 与 qtimageformats，按官方 sha256 校验后解压到 `.sdk/6.11.0/msvc2022_64`；已有的 SDK 再运行一次只补缺的模块，`scripts\build_static_qt.ps1` 对静态 SDK 也是这样。Qt 6.11 起下载站改了目录结构，aqtinstall 3.3.0 装不了：

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py
```

### 单文件轻量版

静态链接 Qt 和 MSVC 运行库，产物只有一个 exe。先从 Qt 官方源码编一份静态 SDK（下载后按官方 MD5 校验，16 线程约 6 分钟），再加 `-Static` 构建：

```powershell
.\scripts\build_static_qt.ps1
.\build.cmd -Static
```

产物在 `dist/Release-static`：`jietuba_lite.exe` 和许可文件，发布的轻量版包就是这些文件。旧版更新器只替换 zip 根目录的 `jietuba_lite.exe`，所以发布包必须是单个 exe、名字不能变。exe 带图标和版本资源，版本号取自 `../main/main_app.py` 的 `APP_VERSION`，和完整版同一个版本号。这种构建不跑测试，同一份代码由默认构建测过。

### PP-OCR 组件包

每个构建都带 PP-OCR 的前后处理，但不带 ONNX Runtime 和模型。在 exe 旁建 `models/`，放进 `PP-OCRv6_det_small.onnx`、`PP-OCRv6_rec_small.onnx` 和 ONNX Runtime 官方纯 CPU 版的 `onnxruntime.dll`（1.24 及以上，与 ppocr_rust 所用 ort 对应的是 1.24.2），重启后 OCR 引擎里就能选 PP-OCR。跑 OCR 测试时把 CMake 的 `JIETUBA_PPOCR_MODELS` 指向放着这三个文件的目录。

ARM64 版在 x64 上交叉编译：先编好 x64 静态 Qt（它的工具供 ARM64 构建使用），再 `.\scripts\build_static_qt.ps1 -Arch arm64` 和 `.\build.cmd -Static -Arch arm64`，需要 Visual Studio 的 MSVC ARM64 生成工具和 `rustup target add aarch64-pc-windows-msvc`。一键更新：单文件版默认把 `rust_libs/updater` 编进 exe，关于页出现「检查更新」；加 `-NoUpdates` 不编，默认构建加 `-Updates` 才编。

### GitHub Actions

- 「C++」（`.github/workflows/cpp.yml`）：改到 `cpp/` 或 `rust_libs/` 的 PR 和 master2 推送时，编默认构建并跑不需要桌面的测试。
- 「Lite single exe」（`.github/workflows/lite.yml`）：编两种架构的单文件版，并分别在 x64 和 ARM64 机器上用 `scripts/smoke_static.ps1` 冒烟。发版的「Build Release」调用它，也可以手动触发出测试包。

## 验证

```powershell
# 打包后的 EXE 无需 Qt SDK 路径或 Python 环境
.\dist\Release\jietuba_cpp.exe --smoke-test --diagnostics startup.json
# 导出主窗口预览
.\dist\Release\jietuba_cpp.exe --smoke-test --preview launcher.png
# 默认构建会运行离屏测试；实际 Windows 桌面测试：
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
.\build\msvc\Release\jietuba_tests.exe -platform windows
# Python 行为对照测试：
.\build\msvc\Release\jietuba_parity_tests.exe -platform offscreen
.\build\msvc\Release\jietuba_canvas_parity_tests.exe -platform offscreen
# 实际 Windows 字体与像素对照：
$env:JIETUBA_CANVAS_FIXTURE = "$PWD\tests\fixtures\canvas-windows.json"
.\build\msvc\Release\jietuba_canvas_parity_tests.exe -platform windows
```

验证记录位于 `docs/validation`：Windows 测试包括实际抓屏、反向拖动、连续两次取消会话、原始像素尺寸导出、标注像素、PNG 保存、撤销重做，以及 SQLite 持久化、去重和容量限制。打包 EXE 已在不含 Qt SDK/Python 的 PATH 下通过启动自检。

启动诊断从 `main()` 开始计时，到主窗口构造完成和首次进入事件循环；不包含操作系统加载 EXE/DLL 的时间。和 Python 版比较启动速度要用整机计时。

## 当前边界

已知缺口记在 MIGRATION.md「暂缺」。混合 DPI 多屏、HDR、Windows N 版等硬件和系统环境需要人工验证，见 ACCEPTANCE.md 第 11 节。

## 许可

版权和 MIT 许可见仓库根目录的 [LICENSE](../LICENSE)。Qt 与随附第三方库的说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

开发参考：[Qt 官方 CMake 入门](https://doc.qt.io/qt-6/cmake-get-started.html)。
