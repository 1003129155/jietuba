# C++ 版约定

截图吧轻量版的原生 C++ Qt6 实现，与仓库根目录的 Python 版（完整版）行为一致，换取更快的启动、更流畅的界面和更小的体积。运行时不嵌入 Python、不依赖 PySide6。下文的「原版」指 Python 版。

## 目录

- `src/` 镜像 Python 包结构：`main/<包>/<模块>.py` 对应 `src/<包>/<模块>.h/.cpp`，类名、方法名、设置键沿用原版，方便对照。
- 原版就在上一级目录（`../main`、`../rust_libs`），翻译和对照以它为准。`scripts/migration_reference.py verify` 列出 C++ 上次跟上之后原版改过的文件；C++ 跟上以后用 `sync --ref <提交>` 记下这个提交。

## 翻译规则

- 按模块翻译，先读完整个原函数再动手；保留设置键、默认值、数据格式、快捷键、信号顺序、坐标约定、错误回退、撤销粒度和资源所有权。
- 不自行另写新实现。只有 Python 特有写法在 C++ 中无对应时才偏离，并在代码注释和 `docs/MIGRATION.md` 的「与原版的差异」中写明。
- 改行为时两边一起改：同一个 PR 里先改原版，再照着改 C++，两边行为始终一致。原版的 bug 不在 C++ 里单独修正；只在 C++ 里出现的问题直接修。
- 依赖尚未翻译的模块时，用最窄的接口或前置声明隔开，并在 `docs/MIGRATION.md` 记录待补项；不要用简化实现顶替。
- 优先打通日常主流程：托盘 → 快捷键 → 截图窗口与选区 → 标注 → 复制/保存 → 钉图。OCR、长截图、录制、翻译等随后。
- 纯逻辑模块用原版 Python 生成预期结果做对照测试（`scripts/generate_*_fixtures.py`），不能由 C++ 自己生成答案；界面模块至少要编译通过并跑冒烟测试。
- 设置和数据与 Python 版共用（见 `docs/MIGRATION.md`「与 Python 版共用」），测试不得改动原项目和用户数据。测试程序都链接 `tests/test_sandbox.cpp`，默认位置会落到临时目录；新写的测试仍要自己注入临时设置，不能靠沙盒兜底。

## 照搬会出错的 Python 写法

- 释放时机：Python 置空或删除对象后，调用栈上的引用仍让它活着。C++ 里若该对象正处在调用链上（例如由它分发的按键、发出的信号导致自身被释放），用 `deleteLater` 或排到事件循环之后再释放。
- 异常：原版槽函数和事件处理里未捕获的异常由崩溃钩子记录后照常运行，子线程里的只结束该线程；C++ 异常穿出槽函数或线程入口会终止进程，要在这一层接住并照原版记日志。
- 共享状态：原版多线程共用、未加锁的 list/dict 等靠 GIL 不出错，C++ 要显式加锁或排回主线程处理。
- 规避写法：原版只为绕开 Python/PySide 自身问题的代码（如 `core/main_thread_gc`、`layout_widgets` 里作废条目包装）不照搬，按上面的偏离规则记明。只为遮住 Python 性能成本的写法（窗口复用、跨线程轮询、预加载期间推迟热键等）同样算，记在 `docs/MIGRATION.md`「不照搬的 Python 性能绕法」，写明原版为什么这样做、C++ 凭什么不需要。同步原版改动时先分清是行为修复还是这类规避。

## 工程实现

- 构建：`.\build.cmd`（全量含测试）、`-SkipTests`、`-Static`（单文件轻量版，静态 Qt 先用 `scripts\build_static_qt.ps1` 编出）、`-CheckPending`（只编译 CMake 里 `JIETUBA_PENDING_SOURCES` 待定文件）。依赖未就绪的翻译放进待定列表，就绪后移出。
- 分析构建：`.\scripts\check.ps1 -Mode Asan`（AddressSanitizer 跑测试，可加 `-TestFilter`；MSVC 的 ASan 查不了泄漏）、`-Mode Analyze`（MSVC 代码分析加 clang-tidy，规则在 `.clang-tidy`）、`-Mode Leak`（Debug 构建，`QTEST_MAIN` 的测试各跑三遍，用调试堆报第二遍分配、第三遍跑完仍未释放的块和分配处的调用栈，见 `tests/leak_check.cpp`；Rust 库走系统堆，看不到；自己写 `main` 的测试只跑一遍）；各用 `build\asan`、`build\analyze`、`build\leak`，不影响正常构建。崩溃时 `faulthandler.log` 记录带函数和行号的原生调用栈。
- 覆盖率：`.\scripts\coverage.ps1`（单独做一份关掉优化的构建 `build\coverage-build`，用 VS 自带的 Microsoft.CodeCoverage.Console 包住 ctest；`-TestFilter`、`-SkipBuild`）。结果按 src 下文件汇总在 `build\coverage\summary.txt`。
- Rust 库经 `native/` 静态库的 C ABI 调用：以 `default-features = false` 直接依赖 `../rust_libs` 的 crate（不带 pyo3 绑定），`native/src` 只写 C 接口这层壳：参数与返回值转换、错误码、句柄生命周期，要改行为就改 rust_libs 的核心，两个版本一起生效。每个导出函数的函数体都经 `native/src/panic.rs` 的 guard，panic 时返回该函数的失败值（单元测试逐个检查）。新增依赖后用 `../rust_libs/Cargo.lock` 的版本钉住。
- 对照数据用 `..\venv311\Scripts\python.exe -I scripts\generate_*_fixtures.py` 生成，原版行为改了要重新生成。
- 第三方 C++ 库用 CMake FetchContent 固定到原版所用版本（zxing-cpp 3.1.1，仅它和调用它的源文件开 C++20）。截图工具 OCR 的模型密钥同样在配置时从 oneocr 1.0.12 的 `oneocr.py` 读出，不写进本仓库。
- MP4 录制沿用原版的子进程结构：同一 exe 带 `--video-recorder-worker` 运行录制进程，JSON 行协议通信。
- 提交信息一行中文、不超过 30 字；代码注释按仓库根目录 README「开发与测试 · 代码注释」。
