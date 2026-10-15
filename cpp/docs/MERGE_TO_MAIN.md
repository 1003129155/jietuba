# 合入主仓库清单

目标：主仓库 `1003129155/jietuba` 新增 `cpp/` 目录，同一个标签下完整版继续用 Python 打包，轻量版换成 C++ 静态单 exe。本仓库历史不带过去：导入、路径、工作流、文档的改动作为一个提交放进发版分支。

**进度（10-15）：** 已在主仓库 `release/2.1.7` 上导入（第 2～4 节已做），产物测过后随 2.1.7 的发版 PR 合入 master2。

## 0. 待拍板

| # | 事项 | 建议 |
|---|---|---|
| 0.1 | 目录名 | `cpp/` |
| 0.2 | 版本号 | **已定（10-15）：** 不改版本号，直接在 `release/2.1.7` 上合入、随 2.1.7 发布（C++ 依赖的分层 rust_libs 只在这个分支上）；发布说明写详细 |
| 0.3 | `migration/` 留多少 | **已做（10-15）：** 冻结快照 `migration/reference` 合入时删掉；保留函数对应表 `symbol_inventory.json`、`settings_accessor_map.json`、`parity_report.json`；`source_manifest.json` 只记「C++ 已跟到的 Python 提交」和文件哈希（按 LF 计算），`verify` 直接对比 `../` 并列出之后改过的文件 |
| 0.4 | 桌面快捷方式名 | **已改（10-15）：** 和 Python 一样叫 `Jietuba.lnk`；日志文件名的 `_cpp` 后缀保留，收到反馈时能分清是哪个版本 |
| 0.5 | Python 轻量版打包 | `build_with_ocr_onefile.py --lite` 保留一两个版本作退路，build.yml 不再调用 |
| 0.6 | C++ 的 PR 检查要不要设为必需 | 先不设；不必需时用工作流 `paths` 过滤即可，设为必需后改成作业级 `if`，否则不改 C++ 的 PR 会一直等这个检查 |
| 0.7 | 本仓库合入后 | 设为 archive，只读保留 |

## 1. 合入前在本仓库做完

合入时只搬运、改路径，不夹带功能改动。

- [x] **exe 的图标和版本资源**（10-15）。配置时由 `resources/app.rc.in` 生成，字段照搬 Python 版的 `write_version_info`：ProductName=`jietuba`、ProductVersion=APP_VERSION 等。SignPath 就按这两项校验。图标用 `resources/app.ico`，和原版的 `托盘.ico` 是同一个文件。桌面快捷方式取的是 exe 里的第 0 个图标，以前 exe 里没有图标，快捷方式一直显示默认图标，现在也一起好了。
- [x] **版本号单一来源**（10-15）。配置时从 `${JIETUBA_PYTHON_ROOT}/main/main_app.py` 的 `APP_VERSION` 生成 `app_version.h`；`main_app.py` 一改，CMake 会自动重新配置。合入后只需把 `JIETUBA_PYTHON_ROOT` 改成 `..`，build.yml 的标签校验就同时管住两个版本。
- [x] **发布用的 exe 名**（10-15）。单文件构建（`-Static`，CMake 的 `JIETUBA_LITE_EXE`）产物叫 `jietuba_lite.exe`，版本资源里的 InternalName、OriginalFilename 跟着变；动态链接 Qt 的开发构建仍叫 `jietuba_cpp.exe`。名字记在 CMake 缓存的 `JIETUBA_EXE_NAME` 里，build.ps1 从缓存读。
- [x] **静态轻量版默认带更新器**（10-15）。`-Static` 自动打开 `JIETUBA_UPDATES`，加 `-NoUpdates` 可以去掉；默认构建仍要加 `-Updates` 才带。
- [x] **清掉本机路径**（10-15）：
  - 默认设置里的日志目录：生成脚本改为写占位 `${LOCALAPPDATA}/Jietuba/Logs`（`resources/defaults.json` 和 `tests/fixtures/parity.json` 里共三处）。运行时本来就用自己算出的目录，测试也是和它比。重新生成后只有这三处变化。
  - `migration/source_manifest.json` 的 `source_root` 改为相对路径；`scripts/migration_reference.py` 的 freeze 以后也写相对路径。
  - `docs/MIGRATION.md`、`docs/validation/cpp-canvas-parity-tests.txt`。
  - 再扫一遍：除 `migration/reference` 外，已跟踪文件里没有本机用户目录了。
- [x] **README.md 第 48 行的退格符**（10-15）：恢复成 `.\scripts\build_static_qt.ps1`、`.\build.cmd`。
- [x] **验收剩余的 P0 项**（10-15，`docs/validation/acceptance-r5.md`）：
  - 真鼠标四项：全局手势、Win+V、拖动钉图、自启开关，用户实测通过。
  - 10.1：走 GitHub 下载的一键更新，用户实测通过。
  - 10.4 arm64 实机不做，ARM64 只靠 CI 冒烟。
  - 10.5 不再适用，完整版继续用 Python。
- [x] **静态版实测一次 WebP 保存和 TIFF 读取**（10-15）。在静态构建目录加编 `jietuba_parity_tests`，`offeredFormatsSaveAndImportedFormatsDecode` 通过；静态 Qt 只带 `windows` 平台插件，要用 `-platform windows` 跑。单文件版在沙盒里冒烟通过，依赖只有系统 DLL。
- [x] **冻结**（10-15）。`verify` 对主仓库 `c5dca37` 无漂移，两边工作区干净、没有在跑的会话；本仓库最后一个提交 7480581，之后的 C++ 改动直接在主仓库 `cpp/` 里做。

## 2. 搬进主仓库时要改的路径

搬运：`git archive <本仓库 HEAD> -- . ':!migration/reference'`，解压到 `cpp/`。压缩后约 4.2 MB，其中 canvas 对照数据（2 个 14 MB 的 JSON）占 2.7 MB。

| 位置 | 现在 | 合入后 |
|---|---|---|
| `CMakeLists.txt` 的 `JIETUBA_PYTHON_ROOT`（版本号来源） | `migration/reference` | `${CMAKE_CURRENT_SOURCE_DIR}/..` |
| `native/Cargo.toml:17-23` 七个 path 依赖 | `../migration/reference/rust_libs/<crate>` | `../../rust_libs/<crate>` |
| `CMakeLists.txt` 的 rust_libs 输入（约 87 行）、更新器 manifest 和 lock（约 173-179 行） | `migration/reference/rust_libs` | `${CMAKE_CURRENT_SOURCE_DIR}/../rust_libs` |
| `scripts/build.ps1:77` 的 `$reference` | `migration\reference` | 仓库根目录；Copy-Notices 从根目录 `licenses/` 按名单拷 |
| `scripts/generate_parity_fixtures.py`、`generate_canvas_fixtures.py`、`generate_setting_accessors.py`、`run_reference_canvas_tests.py` | `ROOT / 'migration/reference/...'` | `ROOT.parent / ...` |
| 各脚本开头注释、`docs/MIGRATION.md`、`docs/ACCEPTANCE.md` 里的解释器 | `..\jietubafabu\venv311\Scripts\python.exe` | `..\venv311\Scripts\python.exe` |
| `scripts/check-parity.ps1:7` 的默认解释器 | `jietubafabu\venv311` | `..\venv311` |
| `AGENTS.md`：新依赖的锁版本来源 | `../jietubafabu/rust_libs/Cargo.lock` | `../rust_libs/Cargo.lock` |
| `.github/workflows/lite.yml` | 放在子目录里，GitHub 不读 | 并入根目录的工作流（见第 3 节），删掉 `cpp/.github` |
| `THIRD-PARTY-NOTICES.md` 开头一句 | 「This project derives from Jietuba…」 | 改成「截图吧的 C++ 轻量版」；`LICENSE` 和根目录的一字不差，删掉后引用根目录那份 |
| `.gitignore` | 已覆盖 build、dist、.sdk、.tools、*.log | 原样作为 `cpp/.gitignore`。主仓库根目录的 .gitignore 不受跟踪，不能依赖它 |

## 3. CI 与发布

- [x] **新增 `cpp.yml`**（10-15，桌面测试 `-platform windows` 不在 CI 跑，对照数据重新生成那步没加）：PR 和 push 都跑，`paths` 设为 `cpp/**`、`rust_libs/**` 和工作流本身。内容是默认（动态 Qt）构建，加离屏 ctest。改 rust_libs 的 PR 也会编 C++ 壳，接口改坏了能当场看到。可选：再加一步「用 `../main` 重新生成对照数据并 diff」，Python 行为变了但 C++ 没跟上时，CI 会变红。
- [x] **build.yml**（10-15：`lite.yml` 改成可复用工作流；完整版 `build`、轻量版 `lite` 并行，`package` 等两者都完再签名打包，签名环境只挂在 `package` 上）：
  - 去掉 Python `--lite` 构建和 `dist_lite`，改为 C++ 静态作业：x64 编译；arm64 在 x64 机器上交叉编译；两种架构各在自己的机器上冒烟。照搬 `lite.yml`。
  - 打包：`jietuba_lite.exe` 加 `licenses/`，包名 `jietuba_lite-<tag>-<arch>.zip`。名字一个字都不能差：旧更新器按包名精确匹配，只取 zip 根目录的 exe。
  - SignPath：签名步骤要等 C++ 作业的产物。「windows-exes」配置要求 zip 根目录下有两个 exe，且产品版本等于 version 参数，所以依赖第 1 节的版本资源。
- [ ] **耗时与缓存**（`save-if` 已加到 ci.yml、build.yml、lite.yml、cpp.yml；剩合入后在 master2 手动跑一次热缓存）：
  - 缓存命中时约 32 分钟，瓶颈是 arm64 交叉编译（约 30 分钟）；没有静态 Qt 缓存时约 2 小时 15 分。
  - 主仓库第一次一定是冷启动，所以合入后先手动跑一次 build.yml，把缓存热起来。缓存 7 天不访问会被清。
  - **主仓库的缓存已经到 10.56 GB，超过了 10 GB 上限**（10-15 查到 13 份，全是 rust-cache，master2、PR #92、release-2.1.6 标签各存了几份）。静态 Qt 两份各约 300 MB，放进去也会很快被挤掉。先给 rust-cache 加 `save-if: ${{ github.ref == 'refs/heads/master2' }}`，PR 和标签不再存缓存。
- [x] CODEOWNERS 不用改，已经是 `* @1003129155`。

## 4. 文档

- [x] **三语 README（中、英、日一起改）**：
  - 「下载与使用」：讲清两个包的区别。完整版是 Python 打包，带 PP-OCR。轻量版是 C++ 原生单 exe，默认用截图工具 OCR，在 exe 旁放上 PP-OCR 组件包（模型和 `onnxruntime.dll`）也能用 PP-OCR；10-14 实测启动 2.16→1.14 秒、内存 199→158 MB。两个版本共用设置和剪贴板历史，不要同时运行。
  - 「代码签名政策」：写明轻量版由 `cpp/` 的源码构建并签名。
  - 「从源码运行」「开发与测试」：加上 C++ 的构建和测试入口，细节链到 `cpp/README.md`。
  - 「Rust 库分层」：写明 C++ 壳的位置 `cpp/native/`。
  - 「目录结构总览」：加上 `cpp/`。
- [x] **根目录 CLAUDE.md**：加一段 C++ 约定。两个版本的行为必须一致；改 Python 行为的 PR 同时改 C++，替代迁移期「先修 Python、再同步快照、再改 C++」的流程；C++ 的细则见 `cpp/AGENTS.md`。
- [x] **`cpp/README.md`**：从「独立迁移项目，验收通过前不替代原版发布」改成「截图吧轻量版（C++）开发说明」。
- [x] **`cpp/AGENTS.md`**：去掉冻结快照和 sync 的流程，改为直接对照 `../main`。
- [x] **`cpp/docs/`**：MIGRATION.md 里的「与原版的差异」「不照搬的 Python 性能绕法」保留；ACCEPTANCE.md 和 validation/ 留作记录（先完成第 1 节的路径清理）。
- [x] **issue 模板** `bug_report.yml`：加一个「版本类型」下拉（完整版 / 轻量版）；环境说明里的「Python 版本」改成只针对源码运行。
- [ ] **发布说明**：写清楚轻量版换成了原生实现，一键更新会自动迁移，设置和剪贴板历史保留；已知差异照 MIGRATION.md「暂缺」写（录屏时滚轮调线宽，录制工具栏面板上的数值不跟着变）。

## 5. 顺序

1. 第 1 节做完，冻结本仓库。（已做）
2. ~~主仓库从发版分支开 `cpp-import` 分支~~ 10-15 改为直接在 `release/2.1.7` 上做：一个导入提交，包含第 2～4 节的改动。（已做）
3. 本地编默认构建跑测试、编静态单文件版实测；推送后手动跑 build.yml 拿两种架构的测试包（`package` 作业会因签名环境只许 master2 和标签而失败，`build`、`lite` 的产物照常可下载）。（已做：run 38748285508，产物全面测试和本机测试源上的一键更新见 `docs/validation/acceptance-r6.md`）
4. 产物测过后开 2.1.7 的发版 PR（release/2.1.7 → master2）。必需检查 test (x64)/(arm64) 通过。diff-cover 只统计 coverage.xml 里有的文件，预计 `cpp/scripts` 下的 Python 不计入，第一次跑时确认一下。
5. 合入后在 master2 手动跑一次 build.yml：热缓存，顺便验签名和打包。
6. 打标签，自动生成草稿 Release。逐项验：旧 Python 轻量版从 GitHub 一键更新到 C++（10.1）、两个 exe 的签名状态、zip 里的内容。
7. 公开 Release 由用户自己点。
8. 本仓库设为 archive。
