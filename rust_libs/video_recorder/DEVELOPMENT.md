# j-video 开发说明

面向仓库开发者：应用如何调用录制扩展、进程协议和原生验证方法。扩展接口、事件字段与
功能边界见 [README.md](README.md)；构建 wheel、生成许可声明与发布见
[../PUBLISHING.md](../PUBLISHING.md)。

## 进程模型

应用每次录制创建专用 Python 子进程，在其中调用扩展；正常停止完成 MP4 封装后
退出以释放录制资源。未录制时不保持该进程；暂停仍保留当前会话及其缓冲。
GUI 只交换控制命令和状态事件，不加载录制扩展。源码运行使用 `main/video/worker.py`；
onefile 使用自身的 `--video-recorder-worker` 入口，由自定义 runtime hook 在
PyInstaller 的 Qt hook、配置、数据库和单实例初始化前分流，复用当前 onefile
的解压目录。应用退出会等待录制进程完成。

## 进程协议

以下协议由 `main/video/worker.py` 提供。stdout 每行输出一个 JSON 对象，`data` 的字段与
扩展回调事件一致，另有 `error` 事件；stderr 输出少量诊断，不保存画面或音频内容。

```json
{"protocol":1,"event":"ready","data":{"encoder":"NVIDIA H.264 Encoder MFT","hardware":true,"encoder_identified":true,"width":1920,"height":1080,"fps":30,"bitrate":4000000,"audio":"system"}}
{"protocol":1,"event":"error","data":{"code":"output_exists","message":"Cannot create a new output"}}
```

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

## 原生验证

Rust 单元测试：

```powershell
cargo test --locked --release --package video_recorder --manifest-path rust_libs/Cargo.toml
```

原生进程测试需要启用 `test-support` 的扩展：

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

测试完成后重新安装正式 wheel 再打包，打包脚本拒绝启用 `test-support` 的扩展。
真实桌面与声音、长期同步、资源占用和 onefile 生命周期需另做手动验收；
架构构建通过、合成测试或交叉编译结果都不能代替真实录制验证。
