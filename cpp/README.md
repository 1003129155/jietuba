# 截图吧 C++ 实现

截图吧有 Python（仓库根目录 `main/`）和 C++（这里）两套实现，功能、界面、设置、快捷键与边界行为一致，目前发布的轻量版由这里构建。C++ 版使用 C++17 和 Qt 6 Widgets，运行时不加载 Python、PySide6 或 Python 扩展；截图、拼接、剪贴板、OCR、录制等核心直接用仓库的 `rust_libs/`。

## 目录

| 目录 | 内容 |
|---|---|
| `src/` | 源码，与 `main/` 按模块对应：`main/<包>/<模块>.py` ↔ `src/<包>/<模块>.h/.cpp`，类名、方法名、设置键沿用 Python 版 |
| `native/` | `rust_libs` 的 C 接口层，由 cargo 编成静态库链进 exe |
| `tests/` | QtTest 测试；`tests/fixtures/` 是由 Python 版生成的预期结果 |
| `resources/` | 默认设置，以及由 Python 版生成的界面翻译和日志文案；图标直接用仓库根目录的 `svg/` 和 `托盘.ico` |
| `parity/` | 与 Python 版对照用的数据：上次核对的 Python 提交、设置访问方法对应表 |
| `scripts/` | 构建、Qt SDK 准备、对照数据生成等脚本 |
| `licenses/` | 随发布包附带的第三方许可 |

## 编译

需要 Visual Studio 的「使用 C++ 的桌面开发」（含 CMake）、Rust MSVC 工具链和 Qt 6.11 的 MSVC x64 SDK（qtbase、qtsvg、qtimageformats）。SDK 默认放在 `.sdk/6.11.0/msvc2022_64`，可以用脚本从 Qt 官方下载站取，按官方 sha256 校验后解压；已有的 SDK 再运行一次只补缺的模块：

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py
```

构建：

```powershell
.\build.cmd
# 或使用自己的 Qt6 MSVC x64 SDK：
.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64
```

脚本配置 CMake、编译 Release、运行全部测试，并用 `windeployqt` 部署到 `dist/Release`，运行 `run.cmd` 或其中的 `jietuba_cpp.exe` 即可。`-SkipTests` 跳过测试，`-Configuration Debug` 编 Debug。程序的中文文案内置；SDK 不带 Qt 自身的翻译，Qt 标准对话框的按钮可能显示英文。

### 单文件版

静态链接 Qt 和 MSVC 运行库，产物只有一个只依赖系统 DLL 的 exe，发布的轻量版就是它。先从 Qt 官方源码编一份静态 SDK（下载后按官方 MD5 校验），再加 `-Static` 构建：

```powershell
.\scripts\build_static_qt.ps1
.\build.cmd -Static
```

产物在 `dist/Release-static`：`jietuba_lite.exe` 和许可文件。exe 带图标和版本资源，版本号取自 `../main/main_app.py` 的 `APP_VERSION`，与 Python 版同一个版本号。单文件版默认内置 `rust_libs/updater` 并启用一键更新，`-NoUpdates` 去掉；默认构建加 `-Updates` 才内置。

ARM64 版在 x64 上交叉编译：先编好 x64 静态 Qt（ARM64 构建要用它的工具），再运行 `.\scripts\build_static_qt.ps1 -Arch arm64` 和 `.\build.cmd -Static -Arch arm64`；需要 Visual Studio 的 MSVC ARM64 生成工具和 `rustup target add aarch64-pc-windows-msvc`。

### PP-OCR 组件包

每个构建都带 PP-OCR 的前后处理，但不带 ONNX Runtime 和模型，两者都放在 exe 旁的 `models/`。最简单的做法是在设置的「管理 PP-OCR 模型」里下载：

- ONNX Runtime：取自 PyPI 上微软发布的 `onnxruntime` wheel（当前 1.24.4，x64 和 ARM64 各一份），下载后校验整个 wheel 的 SHA-256，再从中取出带微软签名的 `onnxruntime.dll` 并校验它；地址依次是清华镜像、PyPI 官方、腾讯云镜像和阿里云镜像，给的是同一个文件。
- 模型：内置清单列出 PP-OCRv6 tiny/small/medium、PP-OCRv5 mobile/server 和几种语种识别模型，地址和 SHA-256 固定在 `rust_libs/ppocr_rust/catalog.json`；也可以从本地 ONNX 文件添加，添加前会试识别一遍。

下载由更新器的 `download-model` 命令完成，模型的发现、校验和就位都在 `ppocr_rust::models`，和 Python 版共用；下载、添加、切换都不用重启。模型下载成功后，如果 `models/` 里还没有可用的 ONNX Runtime，下载管理器会接着把它下载下来；取消或失败时不下载。关闭管理窗口后下载继续，再次打开可查看进度和结果；点击取消或退出程序才停止下载，不保留未完成的文件。也可以手动放：`PP-OCRv6_det_small.onnx`、`PP-OCRv6_rec_small.onnx`（仓库根目录 `models/` 里就有）和 ONNX Runtime 1.24 及以上的纯 CPU 版 `onnxruntime.dll`。

跑 OCR 测试时，把 CMake 的 `JIETUBA_PPOCR_MODELS` 指向放着默认模型和 `onnxruntime.dll` 的目录；不设时，需要真实模型的用例跳过。

## 测试

`.\build.cmd` 会运行全部测试，其中名字以 `_desktop` 结尾的测试在真实桌面上注入键鼠、注册全局热键、截屏，运行期间不要操作鼠标和键盘。只跑不需要桌面的测试：

```powershell
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
ctest --test-dir build\msvc -C Release --output-on-failure -E "_desktop$|_windows_parity$"
```

测试程序都链接 `tests/test_sandbox.cpp`：在 `main()` 之前把 `%LOCALAPPDATA%`、`%APPDATA%` 指向临时目录，不会读写本机的设置、剪贴板历史和日志。

- 单文件版冒烟：`.\scripts\smoke_static.ps1 -Exe dist\Release-static\jietuba_lite.exe`，在临时沙盒里启动 exe，检查它能运行、写出日志且没有崩溃记录。
- 分析构建：`.\scripts\check.ps1 -Mode Asan`（AddressSanitizer）、`-Mode Analyze`（MSVC 代码分析加 clang-tidy，规则在 `.clang-tidy`）、`-Mode Leak`（Debug 构建，用调试堆找测试里泄漏的块），各用独立的构建目录。
- 覆盖率：`.\scripts\coverage.ps1`，结果按源文件汇总在 `build\coverage\summary.txt`。

## 与 Python 版保持一致

- 改了其中一套的行为，同一个 PR 里另一套也要改。
- 纯逻辑模块的预期结果由 Python 版生成，C++ 逐项比较：`scripts/generate_*_fixtures.py` 写出 `tests/fixtures/`，Python 版的行为改了要重新生成（`.\scripts\check-parity.ps1 -Regenerate` 重新生成后构建并测试）。界面另有离屏行为测试和桌面测试。
- 界面翻译（`.qm`）和日志文案由 `scripts/generate_resources.py` 从 `main/` 生成；仓库的 CI 会核对它们是否与 Python 版一致。
- `scripts/python_baseline.py verify` 列出上次核对之后改过的 Python 文件，C++ 跟上以后用 `sync` 记下这次核对的提交；PR 的 CI 摘要里也会列出这份清单。
- 两套实现共用设置和数据：设置在 `HKCU\Software\Jietuba\ToolSettings`，数据与日志在 `%LOCALAPPDATA%\Jietuba`，剪贴板历史库也是同一个。只有日志文件名带 `_cpp`，便于区分。两个版本不要同时运行：默认热键会冲突，剪贴板历史会被两个进程同时写。

以上命令里的 Python 都用仓库根目录的虚拟环境（`..\venv311\Scripts\python.exe`），生成预期结果要用到 PySide6 和 Python 版自己的依赖。

## Rust 库与第三方库

- `native/` 以 `default-features = false` 依赖 `rust_libs` 的各个 crate（不带 pyo3 绑定），只做参数转换、错误码和句柄生命周期；逻辑都在 `rust_libs` 的核心里，两套实现共用。每个导出函数都接住 panic，返回该函数的失败值。
- `native/Cargo.lock` 也记着 `rust_libs` 各 crate 的版本号，crate 升版本时要一起更新，见 [rust_libs/PUBLISHING.md](../rust_libs/PUBLISHING.md)。
- zxing-cpp 3.1.1 由 CMake FetchContent 在配置时下载；截图工具 OCR 的模型密钥同样在配置时从 oneocr 1.0.12 的源码读取，不写进仓库。
- MP4 录制和 Python 版一样放在子进程里：同一个 exe 带 `--video-recorder-worker` 运行录制进程。

## CI

- [C++](../.github/workflows/cpp.yml)：改到 `cpp/` 或 `rust_libs/` 的 PR 和 master2 推送时，编默认构建并跑不需要桌面的测试。
- [Lite single exe](../.github/workflows/lite.yml)：编 x64 和 ARM64 的单文件版，并分别在两种架构的机器上冒烟。发版的 Build Release 调用它，也可以手动运行出测试包。

## 许可

版权和 MIT 许可见仓库根目录的 [LICENSE](../LICENSE)；Qt 与其他第三方组件的说明见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。
