# 无损行为迁移

目标是将原 Python 项目的功能、界面、默认值、配置键、快捷键、信号顺序、坐标约定、错误回退、撤销粒度和资源生命周期迁移到 C++ Qt6。不能以“功能类似”“界面能运行”或“编译成功”替代行为对齐验收。

原版各模块都已移植并接入 EXE，整机验收（[ACCEPTANCE.md](ACCEPTANCE.md)，两边逐条对照）已完成，2.1.7 起轻量版由 C++ 构建发布，完整版仍是 Python。逐函数对照（下文）只覆盖部分模块。原版里没有 C++ 对应文件的模块，都是无人引用、Python 特有或并入其他文件的代码。

## 原版基线与逐函数对应

迁移期间原版以冻结副本 `migration/reference` 为准；合入主仓库后副本已删除，直接对照上一级目录的 `main/`、`rust_libs/`。`migration/source_manifest.json` 记录 C++ 已跟上的原版提交和当时各文件的 SHA-256（按 LF 换行计算）。

`migration/symbol_inventory.json` 登记了 291 个生产 Python 模块的 5,223 个类/函数符号及行号，测试模块另行登记。每个符号保留待迁移或已有对照测试证据的状态。`migration/parity_report.json` 记录 C++ 对应文件、测试证据、整合状态和剩余项，整体完成标志保持 false，直到真正完成验收。

```powershell
..\venv311\Scripts\python.exe scripts/migration_reference.py verify
python scripts/migration_report.py
```

`verify` 列出之后改过的原版文件；逐个核对、C++ 跟上并重新执行相关对照用例后，用 `sync --ref <提交>` 记下。

## 当前直接移植范围

| 原版模块 | C++ 对应 | 已验证范围 | 整合状态 |
|---|---|---|---|
| `canvas/selection_model.py` | `canvas/selection_model.*` | 最小尺寸、调用者矩形变化、状态、信号顺序、确认/取消、副本语义 | 完整画布整合待完成 |
| `core/global_hotkey.py` | `core/global_hotkey.*` | 键名表、解析与写法、注册方式判定（系统保留组合、不带修饰键的单键）、修饰键名与位掩码 | 已接入快捷键管理、热键录入框、设置页与主程序（每个功能 4 槽，Win 组合与 PrtSc 走钩子、系统注册兜底） |
| `core/last_capture_region.py` | `core/last_capture_region.*` | 进程内最近区域、负坐标、读写副本、不落盘 | 截图快捷键整合待完成 |
| `ui/toolbar_layout.py` | `ui/toolbar_layout.*` | 默认顺序、显示模式、去重、缺项位置、锁定按钮、配置解析与序列化 | 工具栏界面待迁移 |
| `core/ui_scale.py` | `core/ui_scale.*` | Python 偶数舍入、档位吸附、DPI 区间、首次运行、双倍率、标记属性 | Fluent 控件、系统 DPI 回退和界面整合待验收 |
| `settings/tool_settings.py` | `settings/tool_settings.*`、`settings_accessors.inc` | 236 项应用默认值、工具默认值、类型转换、配置键、持久化、信号、123 个简单访问方法 | 核心部分接入；条件访问方法和完整设置界面待迁移 |
| `core/export.py` | `core/export.*` | 分数尺寸、负坐标、像素校验、纯底图、图层可见性恢复 | 使用显式场景适配器；完整 CanvasScene 待迁移 |
| `settings/color_formats.py` | `settings/color_formats.*` | 内置格式、HSL 舍入、默认启用项、旧配置迁移、持久化 | 放大镜与设置页整合待完成 |
| `core/save.py` | `core/save.*` | QImage 保存、原子文件占位、异步回调、重名、清理、PDF 白底、尺寸限制 | Pillow 适配器、保存动作和剪贴板文件引用待移植 |
| `canvas/items/drawing_items.py` | `canvas/items/drawing_items.*` | 矩形、圆角、椭圆、四种序号样式；点击热区、线宽、透明度、绘制像素、悬停/选中规则 | 使用显式编辑控制器适配接口；尚未接入原型截图窗口 |
| `canvas/items/stroke_item.py` | `canvas/items/stroke_item.*` | 画笔、荧光笔：分块局部重绘、画的过程中只抄改动的一截、虚线按块轮廓、外接矩形与点击热区、线宽、透明度 | 已接入画笔/荧光笔工具 |
| `canvas/items/arrow_item.py` | `canvas/items/arrow_item.*` | 9 种箭头、直线/曲线、空心点击剪影、端头缩放、退化切线、热区缓存、控制点跟随/保持、线宽和透明度、端点/控制点/样式撤销恢复 | 完整画布、编辑手柄、箭头工具输入与样式面板整合待迁移 |
| `canvas/undo.py` | `canvas/undo.*`、`canvas/items/item_reference.h` | 添加/删除、序号计数保持与重排、批量删除、样式撤销、0.7 秒合并、场景/命令生命周期；含箭头的部分编辑状态恢复 | 完整画布整合、日志、文字清空和文字/自定义图元专用状态分支待迁移 |
| `tools/number.py` | `canvas/undo.*` 中的 `NumberSequence` | 计数偏移、最大值、下一序号、稳定创建顺序、修改数字后的计数策略 | 工具输入、设置策略、工具栏与光标依赖的完整整合待迁移 |
| `clipboard/**`、`settings/special_paste.py`、`ui/settings_ui/page_clipboard.py`、`special_paste_dialog.py`、`ui/welcome/page3_clipboard.py` | `clipboard/**`、`settings/special_paste.*`、`ui/settings_ui/page_clipboard.cpp`、`special_paste_dialog.*`、`ui/welcome/page3_clipboard.cpp`；后端 `pyclipboard` | 特殊粘贴文本变换、特殊粘贴配置读写、分组图标与命名、文件条目 payload、CSV 读写、分隔符转义、时间范围解析、分组栏容量、管理窗口尺寸、保存格式推断、内容/多选/分组右键菜单；后端增删改、分组、搜索、清空与界面冒烟 | 已接入主程序（监听、热键、钉剪贴板图片；Win+V 是剪贴板热键的第 3 槽）、设置窗口与欢迎向导；交互细节待整应用对照 |

已经建立映射不代表覆盖了全部输入，也不代表整个模块或用户流程已经无损。表中「整合状态」是建对照测试时的记录，之后各模块已全部接入 EXE；窗口和用户流程按 ACCEPTANCE.md 验收。

## 对照测试

`scripts/generate_parity_fixtures.py` 只运行 Python 版的实现来生成预期结果，C++ 不能生成自己的正确答案。当前涵盖：

- 1,548 步选区状态/信号轨迹。
- 267 组工具栏排布，包括畸形配置与 Python 字典解包顺序。
- 346 组缩放输入、736 组像素换算、16 组 DPI 输入。
- 236 项应用默认值、18 组工具类型转换、123 个简单设置访问方法。
- 导出像素哈希、图层状态、颜色格式与旧配置、保存尺寸限制、PDF 合成、重名与异步保存。
- `scripts/generate_core_fixtures.py` 另生成全局热键的判定表：各键名带不同修饰键的解析结果与注册方式、全部虚拟键在几组修饰键下的写法和判定。
- `scripts/generate_clipboard_fixtures.py` 生成剪贴板纯逻辑与右键菜单的预期：菜单部分在临时库上跑原版 `ClipboardController`，C++ 按相同顺序建同样的分组和条目后逐项比较；另含 Python 字符串与路径语义（`core/py_text`）和各编码的 CSV 导入导出。剪贴板窗口、管理窗口、控制器、后端监听和设置页另有离屏行为测试（临时库、临时设置、粘贴按键由测试接管），剪贴板源文件行覆盖率约 92%。
- `scripts/generate_freehand_fixtures.py` 生成自由笔迹的预期：按住 Shift 拉直线在各角度档下的吸附终点，以及平滑开关、直线中途按下松开时每一步是否改动、从第几个元素起改写、末端位置和画完的整条路径。笔迹图元 `StrokeItem` 另有离屏像素测试：边画边延伸与整条新建的逐像素一致，实线整条露出时与 Qt 直接画的逐像素一致，局部重绘与整幅一致（虚线也是），每步标脏范围盖住所有变化的像素，外接矩形包住画出的像素和点击热区。
- `scripts/generate_canvas_fixtures.py` 另外生成 6,042 步基础图元及箭头的几何/像素/悬停轨迹、1,477 步序号撤销轨迹和 496 步编辑状态恢复轨迹。序号轨迹还比较工具栏刷新、光标刷新和撤销栈索引的调用顺序。重复几何存为无损索引，C++ 比较前还原；数值容差 1e-9，像素哈希精确比较。

原版选区/区域/缩放/排布测试 64 项、颜色和保存测试 58 项已在冻结副本上通过。这是原版参考验证，不能替代 C++ 的行为对照测试。结果位于 `docs/validation/python-*.txt`；C++ 对照结果位于 `cpp-parity-tests.txt`。

原版基础图元、撤销栈与序号撤销测试另有 99 项通过，记录为 `python-canvas-tests.txt`。新的 C++ 对照与生命周期测试为 `cpp-canvas-parity-tests.txt`。图元命令使用共享引用保留被移出场景的对象，场景销毁或清空时使引用失效，避免原生指针悬空；调用者需要以 `ItemReference` 保留独立于撤销栈的引用。不能将栈上分配的图元交给拥有生命周期的命令。

加入箭头样式测试后，原版相关测试共 161 项通过，记录为 `python-arrow-canvas-tests.txt`。`.qm` 是构建产物、不受版本控制；`scripts/run_reference_canvas_tests.py` 把清单里的原版文件复制到 `build` 下的临时目录，用 PySide6 的 lrelease 从 XML 生成 `.qm` 后执行原版测试，再清理临时副本，不改动原版目录。

另在实际 Windows Qt 平台重新执行原版 oracle，生成 `canvas-windows.json`，并通过同一组 C++ 对照测试，记录为 `cpp-canvas-windows-tests.txt`。序号确认使用 Arial 实际字形，其像素哈希与原版一致；离屏平台的占位字体结果单独保留，不能替代桌面字体验证。Windows 字体/像素基线属于当前机器环境，跨机器字体版本和 DPI 仍须在相同参考环境下重新对照。

`EditItemCommand` 当前恢复已移植图元的 Qt 值状态、序号、矩形、画笔与圆角，以及箭头端点、控制点修改标记和样式。字体/文字、自定义图元和 Python 复杂对象引用语义尚未完成，因此 `_apply_state` 没有记为已迁移。箭头恢复保留原版规则：非空字符串 `"false"` 仍为真；未知样式恢复后保留旧轮廓并捕获几何更新异常；设置 control_pos 不会自动标记弯曲或立即更新轮廓；极短退化端点会保留旧轮廓。编辑控制器与序号刷新是显式依赖接口，其场景和视图实现尚待迁移。所有图元测试仍是有限的原生类型输入覆盖，不代表 Python 所有动态类型与异常输入均已对齐。

```powershell
..\venv311\Scripts\python.exe scripts/generate_setting_accessors.py
..\venv311\Scripts\python.exe scripts/generate_parity_fixtures.py
..\venv311\Scripts\python.exe scripts/generate_canvas_fixtures.py --platform offscreen
..\venv311\Scripts\python.exe scripts/generate_canvas_fixtures.py --platform windows --output tests/fixtures/canvas-windows.json
..\venv311\Scripts\python.exe scripts/run_reference_canvas_tests.py
..\venv311\Scripts\python.exe -I scripts/generate_freehand_fixtures.py
..\venv311\Scripts\python.exe -I scripts/generate_clipboard_fixtures.py
..\venv311\Scripts\python.exe -I scripts/generate_python_case_tables.py
.\build.cmd
```

简单访问方法只转译可以精确识别的 AST 形式，条件、校验和复杂流程全部保留待人工迁移，不生成占位实现。

C++ 与参考 PySide6 同为 Qt 6.11.0。这些有限用例已经通过，但不能据此保证所有渲染和平台行为一致。

## 剩余迁移与验收顺序

各模块的移植已完成，Rust 库经 C ABI 复用原版核心，ARM64 已能交叉编译。剩下的是：

1. 整机验收：按 ACCEPTANCE.md 两边逐条对照，P0 全过后才能替代原版发布。合入清单列出的剩余 P0 项 10-15 已做完（validation/acceptance-r5.md），arm64 实机不做。
2. 升级路径：已发出的原版更新器把轻量版升级到 C++ 单文件版，10-15 已连同 GitHub 下载实测通过；换回原版时设置和剪贴板历史不丢（ACCEPTANCE.md 10.3）10-15 实测通过。完整版继续用 Python 打包。
3. 逐函数对照继续扩大到画布整合、钉图、长截图等模块。

用相同功能与相同 Qt 版本做整应用对照，全部通过后才能称为无损迁移完成。

2026-10-14 起两个版本共用设置和数据（见下文「与 Python 版共用」），一段时间内同时使用、一起测试。测试一律在沙盒里运行，不会碰到用户的设置和数据。

本轮开始前检查了源代码漂移：原项目的 `main/ui/settings_ui/page_capture.py`、对应测试及四个翻译 XML 与冻结基线有差异；本轮移植的图元、撤销与序号源码没有漂移。原始改动未覆盖，后续设置页/翻译迁移须单独合并这些变化。

箭头迁移期间再次检测到原项目的快捷截图测试、长截图文件名测试、设置对话框及 mock_config 有新改动；箭头源码和撤销源码仍与基线一致。完整漂移清单记录在 `docs/validation/source-drift.json`，这些新改动未覆盖、未丢弃。

## 与原版的差异

按决定或修复刻意偏离原版的地方；每条在代码注释里也有说明。

**与 Python 版共用（2026-10-14 起，之前是各用各的）**

迁移基本完成后，两个版本要一起用一段时间，所以设置和数据改回原版的位置：

- 设置存在 `HKCU\Software\Jietuba\ToolSettings`，剪贴板窗口位置存在 `Jietuba\ClipboardWindow`，数据与日志目录是 `%LOCALAPPDATA%\Jietuba`。剪贴板库没设路径时用后端默认的 `%APPDATA%\pyclipboard\clipboard.db`，和原版一样。
- 开机自启用同一个值名 `Jietuba`，和共用的 `autostart_enabled` 设置保持一致：最后开启它的那个版本会在登录时启动。
- 桌面快捷方式和原版同名（`Jietuba.lnk`）：C++ 版作为轻量版发布后是同一个产品，向导指向最后运行它的那个 exe。
- 单文件构建（`build.ps1 -Static`）叫 `jietuba_lite.exe`，和原版轻量版同名：更新器替换的就是这个名字，exe 里的版本资源也按它填。实例记录文件按 exe 名命名，所以 C++ 版和原版轻量版互相结束对方的旧实例，和原版完整版（`jietuba_pp.exe`）可以同时运行。动态链接 Qt 的开发构建仍叫 `jietuba_cpp.exe`。

仍然分开的只有日志：文件名带 `_cpp`（`runtime_cpp_YYYYMMDD.log`、`crash_cpp.log`、`faulthandler_cpp.log`）。两个版本同时运行时不会写进同一个文件，各自也只清理自己的旧日志；收到反馈时也能分清是哪个版本。

两个版本同时运行时默认热键会冲突，后启动的那个会提示注册失败；两边都开着剪贴板监听时，同一份历史会被两个进程同时写。

测试在沙盒里运行：`tests/test_sandbox.cpp` 链进每个测试程序，在 `main()` 之前把 `%LOCALAPPDATA%`、`%APPDATA%` 指向临时目录，并设置 `JIETUBA_TEST_SANDBOX`。库里的默认设置存储、剪贴板窗口位置和桌面路径据此落到临时目录，开机自启在测试里不写入。`test_core` 的 `testDefaultsStayInTheSandbox` 负责守住这一点。识别用的截图工具文件副本在测试里放在临时目录下固定的 `jietuba-oneocr-test-cache`，各次测试共用：已加载的 DLL 在退出时删不掉，不能跟着沙盒走。退出时没删掉的沙盒，满一天后由下一次测试清掉。以前留在 `ToolSettingsCpp` 和 `%LOCALAPPDATA%\JietubaCpp` 里的开发数据不再读取，也没有删除。

**暂缺（依赖未迁移，用窄接口隔开）**

- 录屏绘制层滚轮调线宽/透明度时，录制工具栏面板上的数值不跟着变（画布找工具栏只认截图工具栏类型）；笔迹本身照常变化。
- 原版主线程 GC（`core/main_thread_gc`）是 Python 特有机制，C++ 无对应。

**修复或无对应写法**

- 长截图整图拼接接口 `stitch_images` / `stitch_images_auto` 未迁移：全程序无调用，长截图走增量拼接。
- 翻译厂商的少数异常分支（响应根节点类型不对时原版抛 Python 异常落进「Translation failed: ...」），C++ 按「Invalid ... response」处理；火山引擎响应里的 `Error` 不是对象时按「Volcengine MT API error」处理。
- 火山、腾讯的月度额度上限按 `int()` 的规则解析（`core/py_text` 的 `py_int`），超出 qint64 的数按最大值算；原版整数没有上限，两边都等于不设限。
- `gif/drawing_toolbar.py` 未迁移：全程序无引用，绘制工具已并入录制工具栏。
- 录屏绘制场景直接用 `CanvasScene`（隐藏背景与选区框、预置已确认的空选区），原版是另写一个鸭子类型的场景；清空绘制只移除标注，画笔光标这类工具辅助项留在场景里。
- MP4 子进程的命令行按 UTF-16 取再转 UTF-8，中文保存路径不受 ANSI 代码页影响。
- 更新（`core/update_*`、`core/updater_process`、`ui/update_dialog`）：更新器助手由 CMake 从 `rust_libs/updater` 编出，以 Qt 资源嵌进 exe，每次运行拷到缓存再启动（原版从 PyInstaller 解包目录拷出）。`JIETUBA_UPDATES` 在单文件版（`-Static`，即发布的轻量版）默认开，默认构建加 `-Updates` 才开；关着时不嵌助手、关于页没有「检查更新」。版本类型总是 lite：PP-OCR 是 exe 旁的组件包，更新只换 exe，组件包留在原处（原版读打包时写的 `app_variant.txt`，full 是它的 Python 完整版）。检查更新用异步 `QProcess`，不开线程（原版在 `QThread` 里阻塞调用）；设置窗口关掉时检查进程随之结束。测试见 `tests/test_update.cpp`（原版用 Python 脚本冒充助手处，这里是 `tests/fake_updater.cpp`）和 `test_main_app` 的 `updatePreparationWaitsForWorkAndRestores`；原版直接测 Rust 助手本身的用例（`production_updater`）没有照搬，同一个助手由原版 CI 测；关于页的检查流程没有自动测试。
- PP-OCR 的 ONNX Runtime 不静态链接（原版完整版静态链接，轻量版不带 PP-OCR）：`ppocr_rust` 开 `load-dynamic`，第一次用 PP-OCR 时从 exe 旁 `models/` 里的 `onnxruntime.dll` 加载（官方纯 CPU 版，需要 1.24 及以上）。exe 因此只多出 PP-OCR 的前后处理，约 0.2 MB，也不再链接 DirectML；每个构建都能用 PP-OCR，放上组件包（`models/` 里的两个模型和 `onnxruntime.dll`）即可。缺模型报「缺少模型」，缺 DLL 或版本不对报「缺少引擎」，沿用原版的两种状态和提示。加载前先读 DLL 的文件版本号，不对就不去加载：版本不对的 DLL 加载时可能卡住而不报错。升级 ort 时同步改 `ocr/ocr_manager.cpp` 的 `ORT_MIN_MINOR` 和组件包里的 DLL；ONNX Runtime 的许可声明随组件包分发。
- Media Foundation（mfplat、mfreadwrite）延迟加载：原版录制扩展只在 MP4 子进程里才加载，缺媒体功能包的 Windows N 版照样能启动；C++ 是同一个 exe，所以改为延迟加载，并在录制子进程开头先探测，缺失时报 `helper_missing`，与原版 `import video_recorder` 失败时相同。
- 许可声明：原版由 `build_notices.py` 合成一个 `THIRD-PARTY-NOTICES.txt`；C++ 由 `scripts/build.ps1` 的 `Copy-Notices` 把其中仓库自带的来源放进发布包 `licenses/`，原版新增这类来源时要在这里跟上。原版另按 PyInstaller 的打包清单附上 PyPI 依赖、Python 运行时和 PySide6 所带 Qt 的声明（`licenses/pyside6-qt-NOTICES.txt`、`licenses/zxing-cpp-bundled-NOTICES.txt`），C++ 不带这些包，不拷它们。C++ 另外附 zxing-cpp、libzueci（静态链入，不含 zint）和按 Qt SBOM 生成的 `qt-THIRD-PARTY-NOTICES.txt`（`scripts/qt_notices.ps1`）。windeployqt 从 System32 拷来的 `icuuc.dll` 部署后删掉，与原版一样用系统自带的。
- 录制窗口的全局引用（原版挂在 `app._gif_window`）改为 `active_gif_window()`。
- `ui/color_picker_dialog.py`、`ui/settings_ui/mock_config.py` 未迁移：前者全程序无引用，后者只供原版单独预览设置窗口。
- 设置窗口里页面把控件按原版属性名登记到对话框（`set_attr` / `has_attr`），对应原版的 `setattr` / `hasattr`，保存、快照、重置的清单与原版逐项对应。
- 设置导出记录「读过哪些配置键」：原版临时替换 `qsettings` 对象，C++ 的 `ToolSettingsManager::qsettings` 是一层可挂记录器的包装（`SettingsStore`）。
- 翻译「测试连接 / 获取模型」用表单里未保存的值：原版借真实配置类的方法拼参数，C++ 先取真实配置的参数表，再按「配置键以参数名结尾」把表单值覆盖进去。
- 剪贴板后端 `pyclipboard` 以 `default-features = false` 直接依赖（不带 pyo3），经 C ABI 以 JSON 传条目和分组；监听回调在监听线程上，主程序排回界面线程后发 `clipboard_item_received`。
- Rust 库里的 panic：原版经 pyo3 变成 Python 异常，由崩溃钩子记下后程序照常运行；C++ 在每个导出函数里用 `catch_unwind` 接住，返回该函数的失败值（与句柄无效时相同），报告写进 `crash_cpp.log`（`native/src/panic.rs`、`core/crash_handler`）。为此 native 用 `panic = "unwind"`：完整版 exe 大 1.0 MB、zip 大 0.29 MB。
- 剪贴板的齿轮菜单、筛选与右键菜单关闭后即释放；原版把每个菜单挂在窗口下，关掉后仍留着。
- 设置页「更改剪贴板数据位置」的迁移线程不挂在设置窗口下（销毁运行中的线程会终止进程）；迁移结束时设置窗口已不在，也照样按新路径（失败时按旧路径）重新打开数据库。
- `StrokeItem` 继承 `QAbstractGraphicsShapeItem`，画笔存在基类里，几处按基类读画笔的代码（工具栏面板、线型切换）照常读得到；原版是自定义图元，那几处按有没有 `pen()` 判断。改画笔要走 `StrokeItem::setPen`：它会丢掉旧画笔定下的分块和虚线轮廓，按基类指针改会跳过这一步。
- 画笔、荧光笔按住 Shift 拉直线的默认角度：工具没有设置管理器时用 `DEFAULT_SHIFT_LINE_ANGLE`（原版读类属性 `APP_DEFAULT_SETTINGS`，C++ 的默认值表随管理器实例加载），`test_freehand` 核对它与默认值表一致。
- 大小写转换（`str.upper/lower/title/swapcase/casefold`，含希腊文词尾 Σ）按原版解释器的 Unicode 14 表做（`py_case_tables.inc` 由脚本生成），不用 Qt 自带的较新 Unicode 表。
- 文件条目内容按 `json.loads` 的规则解析：顶层是字符串、数字等非对象时视为无路径，而不是当成原始路径。
- 欢迎向导的桌面快捷方式直接用 `IShellLink` 创建，原版通过 PowerShell 调 WScript.Shell，结果相同。原版 `wizard.py` 里的 `_DotIndicator` 无引用，未迁移。
- 对象寿命不再靠引用计数（2026-10-15）。原版里还被调用栈或线程引用着的对象不会被回收，C++ 要显式处理：
  - 剪贴板后端句柄是共享引用（`clipboard/core/manager`）：拼接粘贴、占用统计等后台调用各持一份，「更改数据位置」释放存储时先停监听，数据库在最后一个调用返回后才关闭。
  - GIF 合成的 worker 随自己的线程结束而删除（`gif/composer`）；线程结束后再 `deleteLater` 永远不会执行。
  - 后台停止录制的线程只持有录制会话和帧存储（`gif/frame_recorder`），帧数据回到界面线程再同步：等待期间录制窗口可能已关闭并删掉录制器。
  - 退出时等完剪贴板队列后，最多再等 3 秒让后台保存线程结束（`core/background_tasks` 的 `wait_idle`，`main_app`）：静态对象析构时仍在运行的线程可能用到已销毁的对象；原版的守护线程随解释器一起停下，没有这个问题。
- 线程与回调的异常（2026-10-15）：Rust 监听线程回调进来的 `on_native_item` 接住异常并记日志；拼接粘贴、欢迎页收尾、工作集裁剪这几条分离线程和 `background_tasks` 一样经 `run_thread_guarded`（`core/crash_handler`），异常写进崩溃日志并只结束该线程，对应原版线程异常由崩溃钩子记录。
- OCR 管理器的引擎选择、偏好和上次错误由界面线程改、识别线程读，单独加锁；引擎是否已加载用原子标志，界面线程判断时不必等正在进行的识别（`ocr/ocr_manager`）。原版靠 GIL。

**不照搬的 Python 性能绕法**

原版有些写法只为遮住 Python 自己的成本（解释器开销、GIL 争用、工作线程不碰 Qt 对象）。C++ 没有这些成本，照搬只剩复杂度和延迟，按 AGENTS.md「规避写法」不照搬（2026-10-14 定）。同步原版时，这几处的原版改动先分清是绕法本身还是行为修复，只跟后者。

- 截图窗口每次截图新建，截图结束时销毁（`ui/screenshot_window`、`main_app`）。原版只留一个窗口，每次换上新场景、重绑浮层和工具栏，为的是省下 Python 里约 250 ms 的建窗时间。C++ 建一个约 27–32 ms，复用只能省十来毫秒（2026-10-13 运行日志的计时），却要维护 `prepare_new_session`、三个浮层的 `rebind`、信息面板的 `set_view`、工具栏的 `reset_session_state` 以及会话信号的登记与断开。原版 6abc86d「普通截图结束后快速截图失效」就出在这套复用上。设置在建窗时读取，和原版每次重读的效果相同。
- UIA 控件扫描的结果由工作线程排队通知界面线程（`capture/uia_element_finder`）。原版的工作线程不碰 Qt 对象，界面线程每 33 ms 轮询一次结果队列。C++ 投递事件是线程安全的，结果一到就处理，智能选区下钻到控件最多快 33 ms。
- 翻译请求能真正中止（`core/http_client`、`translation/worker`）。原版的 urllib 中断不了：取消后线程要等到超时才结束，退出程序时最多等「超时 + 余量」。C++ 让 WinHTTP 走异步模式，由调用线程等待，取消只需发一个事件，取消和退出都立即结束。被取消的请求按网络错误 `[WinError 12017]` 结束，本来也不再发出结果。
- 启动时先注册热键和托盘，再做预加载（`core/bootstrap`）。原版把热键排在整条预加载链的最后，怕预加载期间截图和预加载争解释器而卡顿。C++ 没有这种争用，托盘和热键不必等 OCR 引擎加载完。OCR 引擎的选择和剪贴板监听也提前到注册热键之前：前者保证启动阶段的识别用对引擎，后者让剪贴板历史更早开始记录。首次运行照旧在欢迎向导之后注册。原版只为导入模块做的两项预热（1×1 GDI 截屏、PNG 编码器）在原生程序里没有可预热的东西，已去掉。

以下几处看着像绕法，核对后照原版保留（2026-10-14），除非实现变了，不必再重查：

- 工作集裁剪（`core/platform_utils`）。原版裁剪前的 `gc.collect` 在 C++ 里没有对应，剩下的只让任务管理器里的数字变小。实测不裁剪时截图结束后私有工作集也会从 58 MB 落回 13 MB，裁剪后下次截图多约 3400 次软缺页，耗时差在噪声内（2560×1440，无 DXGI 会话）。成本低，保留。
- 设置窗口按需建页（`ui/settings_ui/dialog`）。C++ 一次建完其余页仍要约 330 ms，慢在 Qt 的样式和控件构建。
- 剪贴板窗口离屏预热（`clipboard/ui/windows/clipboard_window`）。不预热时首次打开要 240–320 ms，预热后 8–10 ms。
- 放大镜按当前主题建好画笔（`ui/magnifier`）。原版的主题色同步是因为窗口复用；现在截图窗口每次新建，只在构造时建一次，没有同步负担。
- 钉图显示 300 ms 后才开始自动识别（`pin/pin_window`）。引擎初始化在界面线程上，引擎未常驻时要等锁或加载模型，延后让钉图先画出来；很快关掉的钉图也不会启动识别。复制文字、翻译等主动请求立即开始，不受这个延迟影响。
