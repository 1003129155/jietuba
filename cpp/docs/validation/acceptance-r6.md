# 整机验收 · 第 6 轮：合仓后的 CI 产物与一键更新（2026-10-16）

被测：主仓库 `release/2.1.7` 的 c601d84（9190537 导入 `cpp/`，加版本号 2.1.7），Build Release run 38748285508 的产物：x64、ARM64 两种架构的轻量版（C++ 静态单 exe）和完整版（Python）。分支上 `package` 作业被签名环境拦下（只许 master2 和 release-* 标签，预期如此），发布 zip 照 `package` 的步骤在本地打。

## 自动测试

| 项 | 结果 |
|---|---|
| C++ 全量 ctest（`cpp/` 默认构建，含 10 个 `-platform windows` 桌面测试） | 38/38 通过 |
| 静态构建里的 `offeredFormatsSaveAndImportedFormatsDecode`（存 WebP、读 TIFF） | 通过（静态 Qt 只有 windows 平台插件，用 `-platform windows` 跑） |
| Python 全量 pytest（开 `RUN_REAL_INPUT_TESTS`、`RUN_REAL_CLIPBOARD_TESTS`） | 第 1 遍 4288 过、2 败；第 2 遍 4290 过、0 败。失败的是 `test_input_hub_real_hooks.py` 里两条 Win 组合键用例：前台是靶窗口，但注入的按键一个都没送到；单独重跑 5/5 通过 |
| CI | 完整版两种架构、轻量版两种架构编译通过；轻量版在 x64、ARM64 机器上冒烟通过 |

## 产物核对

- 四个 exe 的产品版本都是 2.1.7；轻量版 PE 架构分别是 x64（0x8664）和 ARM64（0xAA64）。
- 轻量版两种架构各 43 个文件，许可文件清单和本地静态构建一致；exe 里有内嵌的更新器和 WebP、TIFF 插件。
- 发布 zip：轻量版 x64 16,852,067 字节，根目录是 `jietuba_lite.exe`、`LICENSE`、`THIRD-PARTY-NOTICES.md` 和 `licenses/`。

## 一键更新（本机测试源）

做法：
- **测试源**：`server.py` 在 127.0.0.1:18765 模仿 GitHub 的 `releases/latest` 和下载地址，记下每个请求。
- **测试用更新器**：只改两处，`LATEST` 指向测试源、下载地址对 127.0.0.1 放行 HTTP，没有进仓库。
- **Python 旧包**：用 GitHub 上真实发布的 2.1.6 包，启动后把解包目录里的更新器换成测试副本。
- **C++ 旧版**：本地按 2.1.6 编的单文件版，内置测试更新器。
- **操作**：全程走界面，设置 → 关于 → 检查更新 → 一键更新并重启。

| 路径 | 下载 | 点更新到新版就绪 | 新版关于页 | 剪贴板历史 | 设置 | 新版检查更新（真实 GitHub） |
|---|---|---|---|---|---|---|
| Python 轻量版 2.1.6 → C++ 2.1.7 | 16,852,067 B，完整 | 2.1 s | 2.1.7 | 旧版记的那条还在，新版接着记 | 无改动 | 已是最新版本（2.1.7） |
| Python 完整版 2.1.6 → 2.1.7 | 71,760,189 B，完整 | 3.1 s | 2.1.7 | 同上 | 无改动 | 同上 |
| C++ 2.1.6 → C++ 2.1.7 | 16,852,067 B，完整 | 1.1 s | 2.1.7 | 同上 | 无改动 | 同上 |

- 旧进程都已退出；新版日志里没有 ERROR；安装目录留有 `.jietuba-update/<事务>/` 备份。
- 反向测试：x64 包里放 ARM64 的 exe，下载完报 `architecture`（新版 EXE 架构与已安装应用不匹配），已安装的 exe 不变。

## 界面验收（29 个场景，py = 2.1.7 源码，cpp = CI 出的单文件版）

跑了 s1_tray、s1b、s1c、s1d、s2_capture、s2b、s3_tools、s3b、s4b_save、s5_pin、s5b、s5c、s5e、s6_clipboard、s6b、s6c、s7_ocr、s8_long、s8b_record、s8c、s9_settings、s9_lang（英日韩、深色）、s10_misc、s11_settings_actions、s12_resources、s12d_pins、s13_wizard_translate、s13d_pin_translate、s14_update_check，两边都跑完，没有中断。

逐项对比两边的结果，差异只有这些：
- 托盘菜单位置不同：托盘图标两次被系统放在不同位置。
- 保存的文件名带时间戳；保存的 PNG（23,776 B）和 PDF（101,290 B）两边大小相同。
- 剪贴板窗口按钮的 UIA 枚举顺序不同，按钮和位置一样。
- 录制文件大小差几帧：GIF 16,939 / 15,628 B，MP4 56,114 / 56,113 B。

性能和资源：
- 启动到热键就绪：C++ 0.2～0.3 s，Python 1.5～1.9 s。
- 空闲私有内存：C++ 158 MB，Python 211 MB。
- 钉图开关 20 轮，C++ 私有内存第 1/10/20 轮为 168.8 / 176.2 / 180.9 MB（第 4 轮本地构建为 171.4 / 178.7 / 181.4 MB）；句柄、GDI、USER 对象不涨。

## 环境

- `HKCU\Software\Jietuba` 测前导出，每个场景后删掉重导，最后和备份逐行比对 0 差异。
- 自启开关测试会改写或删除 `Run\Jietuba`，每个场景后都写回原值。
- 桌面快捷方式没变；真实剪贴板库仍是 3025 行，当晚新增 0 行；图片目录没有新文件。

## 发现

1. **一键更新只替换 exe，第三方声明不随更新到达。** 从 Python 轻量版自动更新上来的安装目录里，留的是旧 Python 版的 `THIRD-PARTY-NOTICES.txt`，没有 C++ 版的 `LICENSE`、`THIRD-PARTY-NOTICES.md` 和 `licenses/`（含静态链接 Qt 的 LGPL 说明）。关于页的「许可证 → 详情」只显示 MIT 正文。直接下载 zip 的用户不受影响。怎么补（更新器一并解出声明文件，或把声明编进 exe、关于页展示）待定。
2. Python 全量测试第 1 遍 `test_input_hub_real_hooks.py` 有两条偶发失败（见上），复跑没有复现。
