# Windows 视频录制扩展

通过 PyO3 提供 Python 接口，使用 Windows Media Foundation 编码
H.264 / MP4，通过 WASAPI loopback 捕获系统播放声音并编码为 AAC。区域采集（DXGI 优先、GDI 兜底）、
NV12 转换、编码和音频缓冲均由 Rust 完成，不向 Python 传输原始视频帧或音频块。
不捆绑 FFmpeg、Qt Multimedia 或额外编解码 DLL。

该 crate 属于 `rust_libs` workspace，包名为 `video_recorder`，Python
发行名为 `j-video==0.1.0`，导入名为 `video_recorder`。与 `gifrecorder`
等扩展一致，使用 PyO3 0.22、`cdylib` 和 Maturin 构建 `abi3-py311` wheel，支持
Windows x64 / ARM64。
代码使用随 wheel 分发的 [MIT 许可证](LICENSE)，静态链接的第三方 Rust 依赖另见
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt)。

应用每次录制创建专用 Python 子进程，在其中调用扩展；正常停止完成 MP4 封装后
退出以释放录制资源。未录制时不保持该进程；暂停仍保留当前会话及其缓冲。
GUI 只交换控制命令和状态事件，不加载录制扩展。源码运行使用 `main/video_worker.py`；
onefile 使用自身的 `--video-recorder-worker` 入口，由自定义 runtime hook 在
PyInstaller 的 Qt hook、配置、数据库和单实例初始化前分流，复用当前 onefile
的解压目录。应用退出会等待录制进程完成。

## 安装预编译包

普通源码用户只需要与应用架构匹配的 Python 3.11，无需安装 Rust、Visual Studio 或
Windows SDK。发布到 PyPI 后，`setup.bat` 或以下命令会安装 `requirements.txt` 中
锁定的 `j-video==0.1.0`，pip 自动选择匹配当前 Python 的 x64/ARM64 wheel：

```powershell
python -m pip install -r requirements.txt
```

**当前开发分支尚未发布此版本到 PyPI。** 在本轮开发或验证中，先取得对应架构的预编译
wheel，或由 Rust 开发者按下面的步骤生成 wheel。以下示例使用 `setup.bat` 会复用的
`venv311`；若使用其他应用环境，请改用该环境的 Python 执行 pip：

```powershell
py -3.11 -m venv venv311
.\venv311\Scripts\python.exe -m pip install --force-reinstall --no-deps (Get-ChildItem -Path wheelhouse/j_video-*.whl).FullName
.\venv311\Scripts\python.exe -m pip install -r requirements.txt
```

上述目录只放与当前 Python 架构匹配的录制 wheel。已安装的本地 `0.1.0` 会满足运行
依赖中的同版本要求；构建同版本的新源码后，使用 `--force-reinstall` 更新本地安装。

可通过 `python -c "import video_recorder"` 检查扩展是否能加载。
`build_with_ocr_onefile.py` 收集已安装的录制扩展，并从同一发行包获取 `LICENSE`
和 `THIRD-PARTY-NOTICES.txt`；不重新编译录制扩展，也不查找额外录制 EXE。
应用的独立更新器仍由打包脚本通过 Cargo 编译。
打包脚本拒绝启用 `test-support` 的扩展。先安装与打包 Python 架构匹配的正式 wheel。

## 构建与维护

与其他扩展使用相同的 Maturin 命令，无需单独构建脚本。需要 Rust MSVC 工具链、对应架构的标准库，
以及 Visual Studio MSVC 链接器和 Windows SDK；wheel 构建还需要 Maturin。
Visual Studio 的 C++ desktop tools 工作负载提供链接器与 SDK，此组件没有 C++
源码，也不运行 C++ 编译器。激活应用的 Python 环境，在仓库根目录运行：

```powershell
python -m pip install maturin==1.12.6
# x64 Python；ARM64 Python 将目标改为 aarch64-pc-windows-msvc。
maturin build --locked --release --target x86_64-pc-windows-msvc -i python -m rust_libs/video_recorder/Cargo.toml --out wheelhouse
python -m pip install --force-reinstall --no-deps (Get-ChildItem wheelhouse/j_video-*.whl).FullName
```

正式 wheel 不启用 `test-support`。仅此 package 使用 `opt-level = "s"` 和符号剥离，
其余 workspace 扩展保持原有 release 配置。

API 最低要求为 Windows 10；应用继续沿用其 Windows 10 1809+ 支持范围。
ARM64 需安装匹配目标的 MSVC / SDK 组件；在 x64 主机使用交叉链接工具链，
在 ARM64 主机使用原生工具链。架构构建通过不代表已经进行该架构的真实录制验证。
Windows N/KN 或缺少系统媒体组件的系统需要对应 Media Feature Pack；
初始化失败返回错误，不自动下载第三方编码组件。

从当前源码运行 Rust 单元测试：

```powershell
cargo test --locked --release --package video_recorder --manifest-path rust_libs/Cargo.toml
```

依赖变化时从仓库根目录重新生成许可声明，并保留 `rust_libs/about.toml` 中的许可闸门：

```powershell
cargo about generate --locked --manifest-path rust_libs/video_recorder/Cargo.toml -o rust_libs/video_recorder/THIRD-PARTY-NOTICES.txt rust_libs/video_recorder/notices.hbs
```

`rust-wheels.yml` 检查许可并统一构建七个扩展；发布流程见
[../PUBLISHING.md](../PUBLISHING.md)。实际 PyPI 发布需明确授权。
onefile 打包使用已安装的正式录制包，内嵌其许可文件，并将声明合并到完整版或
轻量版发行目录的 `THIRD-PARTY-NOTICES.txt`。

## Python 接口

```python
import video_recorder

recorder = video_recorder.Recorder(
    output, left, top, width, height,
    fps=30, bitrate=4_000_000,
    system_audio=True, hardware=True, cursor=True, prefer_dxgi=True,
)
recorder.run(on_event)  # on_event(name: str, data: dict)
```

`run` 在调用线程执行录制并释放 GIL，只有回调时获取 GIL。控制线程可调用
`pause()`、`resume()`、`stop()`、`cancel()`；每个 `Recorder` 只能运行一次。
`video_worker.py` 将有界 JSONL 命令转换成这些方法，并将事件发回 GUI。
扩展不解析命令行或 JSON，也不依赖 Qt；采集、音频和编码数据始终留在 Rust 中。

失败抛出 `RecorderError`（`RuntimeError` 的子类），其 `code` 提供稳定错误码。
回调抛出的 Python 异常在尽力完成 MP4 封装后原样回传，调用方不能将其当作录制成功。

帧率为 1–60 fps，码率为 0.5–50 Mbps。位置允许负坐标，必须处于虚拟桌面范围内。
尺寸至少 2×2、最大 8K 像素量，两个维度均不得大于 8192；控制端将奇数尺寸向内对齐。
输出必须是新文件，使用 `MF_OPENMODE_FAIL_IF_EXIST` 原子创建，不覆盖已有录像。

默认 30 fps、4 Mbps、系统声音、自动编码与光标显示。自动模式请求硬件 H.264，
writer 初始化失败时重新初始化系统软件编码；运行中失败不会静默续录或改为静音。
实际编码器通过 transform 的 CLSID、硬件属性和实现模块识别；`hardware` 报告识别
结果，未能确认硬件时为 `false`，另以 `encoder_identified` 标明识别情况。

自定义码率同时写入媒体类型与编码器初始化参数；开始编码后读回配置，配置失败返回
错误。系统软件编码采用单个工作线程、低延迟模式与两秒 GOP。CBR 是编码目标，
不是文件大小上限，高复杂度画面仍可能超过目标码率。低帧率主要减少采集、转换与
编码工作，不保证相同目标码率下文件大小按比例减少。编码属性参考
[Windows H.264 编码器文档](https://learn.microsoft.com/en-us/windows/win32/medfound/h-264-video-encoder)。

## 应用内部进程协议

以下协议由 `main/video_worker.py` 提供。stdout 每行输出一个 JSON 对象；stderr
输出少量诊断，不保存画面或音频内容。
所有对象使用 `protocol: 1`，状态字段放在 `data` 中：

```json
{"protocol":1,"event":"ready","data":{"encoder":"NVIDIA H.264 Encoder MFT","hardware":true,"encoder_identified":true,"width":1920,"height":1080,"fps":30,"bitrate":4000000,"audio":"system"}}
{"protocol":1,"event":"progress","data":{"frames":31,"elapsed_ms":1001,"dropped":0,"queued_bytes":3110400,"max_queued_bytes":3110400}}
{"protocol":1,"event":"paused","data":{"frames":60,"elapsed_ms":2000,"dropped":0}}
{"protocol":1,"event":"resumed","data":{"frames":60,"elapsed_ms":2000,"dropped":0}}
{"protocol":1,"event":"complete","data":{"output":"C:\\Videos\\clip.mp4","frames":90,"elapsed_ms":3000,"dropped":0,"encoder":"NVIDIA H.264 Encoder MFT","hardware":true,"finalized":true,"max_queued_bytes":3110400}}
{"protocol":1,"event":"error","data":{"code":"output_exists","message":"Cannot create a new output"}}
```

`elapsed_ms` 排除暂停时间。`dropped` 为过期采集机会；`queued_bytes` 是 Sink Writer
的输入队列观测，不代表编码器和驱动的全部内存。成功 `Finalize` 并释放输出文件
引用后才发送 `complete`。

stdin 接受以下对象，字段顺序不限，每行最长 2048 bytes，仅接受这两个字段，
重复字段及其他字段均返回协议错误：

```json
{"protocol":1,"command":"pause"}
{"protocol":1,"command":"resume"}
{"protocol":1,"command":"stop"}
{"protocol":1,"command":"cancel"}
```

控制端收到 `paused` / `resumed` 后更新状态。后端保留最新控制状态，不积累命令
历史；控制端应等待确认再发送相反命令。`stop` 正常保存；`cancel` 结束并删除
本进程创建的文件，发送 `cancelled`。初始化阶段停止或没有视频帧也发送 `cancelled`。
stdin EOF 按 `stop` 处理。命令读取与录制分别执行；停止或取消后退出不依赖控制端关闭 stdin。
退出码 0 表示完成或取消，1 表示失败；控制端还必须检查终止事件。

初始化失败清理本进程创建的输出，已有文件保持原样。录制中采集、编码、声音或
封装失败返回 `error`，尽力完成尾部封装并保留可能可读的部分录像；失败文件不能
当作完整录像。控制端应先发停止并等待退出，强杀可能使 MP4 缺少封装信息。

## 内存与功能边界

- 复用 BGRA 采集表面；直接转换到 MF sample 的 NV12 缓冲，同步提交，不保留帧历史。
  Sink Writer 使用背压，过期采集机会丢弃；系统和驱动有另外的编码缓冲。
- 系统声音使用固定长度的 48 kHz、双声道 PCM16 环形缓冲与时间标签，按 20 ms
  块提交；静音模式不分配音频采集缓冲。低帧率仍持续处理声音与控制命令。
- 默认播放设备支持 1–8 声道 float32 或 PCM16/24/32，通过插值转为 48 kHz。
  多声道按 Windows speaker mask 降混为立体声，保留中置、环绕与低频并预留混音余量。
  缺少有效声道布局时返回可操作的格式提示；无设备、格式异常或无效时间戳不会静默丢弃声音。
  缺失声音补零并计数。
- 暂停停止并重置 WASAPI，恢复时清除未提交的时间标签，暂停声音不进入录像。
  麦克风、混音与录制期间切换播放设备尚未提供。
- 光标使用 `GetCursorInfo` / `CopyIcon` / `DrawIconEx` 按热点合成并释放原生资源。
  受保护窗口、桌面切换或安全桌面不保证可录。
- 画面输出为 SDR sRGB，再转 BT.601 limited-range NV12；不提供 10-bit 或 HDR 视频。
  开启 HDR 的显示器上 GDI 会截断超出桌面白的内容，因此默认经 `hdrcapture` 用 DXGI 截取并在
  GPU 上按固定映射转成 sRGB，只读回录制区域；新会话尚无帧、DXGI 不可用或 `prefer_dxgi=False`
  时用 GDI。SDR 内容两条路径逐像素一致。DXGI 会话绑定在录制线程上，随录制结束释放。

## 自动化诊断构建

```powershell
# 单独存放测试 wheel，不与正式发行包混用。
maturin build --locked --release --target x86_64-pc-windows-msvc -i python -m rust_libs/video_recorder/Cargo.toml --features test-support --out wheelhouse-test
python -m pip install --force-reinstall --no-deps (Get-ChildItem wheelhouse-test/j_video-*.whl).FullName
$env:JIETUBA_VIDEO_TEST_NATIVE = "1"
python -m pytest main/tests/test_video_native_process.py -c main/tests/pytest.ini
```

本机系统声音验收（会短暂播放测试音，需要可用的播放设备）：

```powershell
$env:JIETUBA_VIDEO_TEST_AUDIO = "1"
python -m pytest main/tests/test_video_native_process.py -k real_loopback -c main/tests/pytest.ini
```

验收检查暂停前后都有可解码的 AAC 声音，音视频结束点一致，并从时间轴扣除暂停。

`test-support` 默认关闭，workspace 单元测试不打开该 feature。测试扩展提供
`Recorder._configure_test(*, synthetic=True, medium=False, duration=None, fail_hardware=False)`，
用于合成画面、限时录制和硬件初始化故障注入；正式扩展没有这个方法。测试 worker
仅在此方法存在时接受 `--synthetic`（高频运动压力画面）、`--synthetic-medium`
（静态面板与局部运动）、`--duration <0..86400 秒，必须大于 0>` 和 `--fail-hardware-init`。
合成模式不采集屏幕，仍使用真实系统 H.264 / AAC；只有 `--audio system` 才采集
系统声音。限时诊断须保持 stdin 打开，以免 EOF 提前停止。
CI 先验证正式 wheel 不含测试入口，再安装测试 wheel 运行原生进程用例。测试完成
后应重新安装正式 wheel 再打包；测试 wheel 不进入 PyPI 发布目录。
真实桌面与声音、长期同步、资源占用和 onefile 生命周期
需另做手动验收，不能用合成测试或交叉编译结果代替。
