# 整机验收 · 第 5 轮：真鼠标项与一键更新（2026-10-15）

第 3、4 轮留下的真鼠标项，和 10.1 里没测的 GitHub 下载一步，由用户在 2026-10-15 亲手操作，结果通过。用户自测，本轮没有记下提交号、环境和逐步现象。

## 逐项结果

| # | 结果 | 备注 |
|---|---|---|
| 1.7 | 通过 | 开机自启开关（用户实测） |
| 4.6、4.7 | 通过 | 全局鼠标手势（用户实测） |
| 5.1 | 通过 | 拖动钉图（用户实测） |
| 6.11 | 通过 | Win+V（用户实测） |
| 10.1 | 通过 | 从 GitHub 下载的一键更新，补上第 3 轮没测的下载一步（用户实测） |
| 10.3 | 通过 | 见下 |
| 10.6 | 通过（单文件版） | master 059d1d2 的 `-Static` 构建：`licenses/` 里有 oneocr、zxing-cpp、libzueci、windows-capture、Lucide、Tabler、各 Rust 库的声明（含 `ppocr_rust`、`updater`）、`libloading-LICENSE.txt`、`qt-THIRD-PARTY-NOTICES.txt` 和 `qt/`；单文件版不带任何 DLL。默认构建的文件夹包没有查 |

### 10.3 从 C++ 版换回 Python 包

被测：C++ 单文件版（master 059d1d2 的 `-Static` 构建，`jietuba_lite.exe`），Python 完整版打包产物（`jietubafabu/dist/jietuba_pp.exe`，10-15 10:02 打的包）。设置用用户真实设置的副本：热键改成冷门组合，清空其余热键槽和全局鼠标快捷键，关掉 Win+V 接管和开机自启，日志、保存目录和剪贴板库指到临时目录。测完整棵 `HKCU\Software\Jietuba` 按备份还原、逐行比对无差异，开机自启项原样，用户真实的剪贴板库没被碰过。

先用 C++ 版：在设置窗口导出、把 49 个开关取反后导入并应用，让 C++ 按自己的写法写进注册表；再往剪贴板放一条文字、一张图片。然后在同一份注册表和剪贴板库上启动 Python 包：

- Python 导出的设置和 C++ 应用后导出的逐键相同（共有的键没有一个值不同）。Python 启动后没有把任何设置改回默认值：两次注册表快照只差剪贴板窗口位置，是测试打开剪贴板窗口留下的。
- 剪贴板历史：C++ 记下的文字和图片都在，Python 的剪贴板窗口里能看到，Python 也能接着往同一个库里记（第一次运行时 Python 新记一条，库里从 2 条变成 3 条）。
- 取反后 C++ 没有生效的 9 个键：`double_click_copy_close` 是派生值，两边点应用时都按「截图时的复制方式」重算，原版也一样；另外 8 个见下面的问题。

**发现的问题（不影响 10.3，已修）**：C++ 导出的设置比原版多 10 个键（`app/screenshot_quality`、`app/screenshot_watermark_last_on`、`gif/fps` 和剪贴板分组的 `auto_cleanup`、`auto_paste`、`close_after_paste`、`move_to_top_on_paste`、`multi_paste_keep_merged`、`preserve_search`、`show_metadata`）。设置窗口里没有它们的控件，所以导出了却导不进来；原版不导出它们。原因是设置窗口回填翻译服务字段时，`ToolSettingsManager::get_provider_field` 为了按名字找一个读取函数，先把所有生成的读取函数都调了一遍，记录读过的键时就把每个键都记了进去；原版按名字只取那一个。改为生成按名字分派的 `generated_accessor_value` 后，离屏空白设置下 C++ 导出的键与原版逐个相同（200 个），`test_settings_window` 的 `exportAndImportRoundTrip` 守着。

## 不再验收

- 10.4 arm64 实机：2026-10-15 决定不做。ARM64 单文件版只靠 CI（`lite.yml`）在 windows-11-arm 上冒烟。
