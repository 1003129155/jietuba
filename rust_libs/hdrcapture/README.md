# hdrcapture

HDR 正确的 Windows 桌面截图。DXGI Desktop Duplication 取 FP16 源帧，GPU 上按该屏的
Windows SDR 白点归一化并做高光 roll-off，输出紧凑的 sRGB `BGRA8`。

`mss` 用的是 GDI `BitBlt`，在开启 HDR 的显示器上会把超出桌面白的内容硬截断为纯白。
本机实测（2560×1440，HDR 开启，SDR 白点 4.1）：

| | 纯白像素占比 | median |
|---|---|---|
| hdrcapture | 0.00 % | 5.0 ms |
| mss | 28.5 % | 15.5 ms |

```python
import hdrcapture

cap = hdrcapture.Capture(timeout_ms=100)   # 会话常驻，不要为每帧新建
monitor = cap.monitors[0]                  # 0 = 完整虚拟桌面
frame = cap.grab(monitor)
frame.bgra                                 # bytes，紧凑 BGRA8，无行填充
monitor.rect                               # (x, y, width, height)，物理像素，可为负
```

`grab()` 也接受整数索引或带 `index` 键的字典，便于从 `mss` 迁移。

异常均继承自 `hdrcapture.CaptureError`（其自身继承 `RuntimeError`）：
`InitialFrameTimeout`、`AccessLost`、`DimensionsChanged`、`InvalidMonitorIndex`。

## 会话生命周期

新建的会话必须先等到一次**真实的桌面 present** 才能返回首帧，等不到则抛
`InitialFrameTimeout`。显示器醒着时 present 每 5~10 ms 就有一次，任何预算都能拿到；
显示器休眠时则任何预算都等不到（实测 5000 ms 仍失败）。

`grab()` 每次都会重新枚举拓扑，`MonitorDescriptor` 全字段参与比较，因此插拔显示器、
开关 HDR、改分辨率或位置、拖动「SDR 内容亮度」滑块都会触发会话重建。**重建会丢弃
全部缓存帧**，所以拓扑变化之后的第一次 `grab()` 回到冷启动状态。调用方应对
`InitialFrameTimeout` 留一条退路。

## 上游出处

底层模块 vendor 自 [NiiightmareXD/windows-capture](https://github.com/NiiightmareXD/windows-capture)，
MIT，版权声明见 `LICENSE-UPSTREAM`（分发 `.pyd` 时必须随附）。

- 基准提交：`c7d106448eb9d9b251345c39047711e1cd408ae2`
- `src/hdr_capture/` 在上游属于该提交之上**尚未提交的工作区内容**，无法用提交号定位。

### vendor 范围

只搬 `hdr_capture` 实际依赖的部分：`d3d11.rs`、`dxgi_duplication_api.rs`、`monitor.rs`、
`hdr_capture/`。未搬入 `capture`、`encoder`、`frame`、`graphics_capture_api`、
`graphics_capture_picker`、`settings`、`window` —— 这些属于 Windows.Graphics.Capture
那套 API，本 crate 不用。`windows` crate 的 feature 也相应从上游的 30 项减到 11 项。

### 相对上游的改动

为切断对未搬入模块的引用，只做了删除，没有改逻辑：

- `dxgi_duplication_api.rs`：删掉两个 `save_as_image()`、`ImageEncoderError` 错误变体
  及对 `crate::encoder` 的引用（`hdr_capture` 自己做 GPU tone-map 和读回，不走编码器）。
- `monitor.rs`：删掉 `impl TryInto<GraphicsCaptureItemType> for Monitor`
  及对 `crate::settings` 的引用（那是 WGC 的桥接）。
- 上述删除连带失效的 rustdoc 链接一并修正。

`src/lib.rs` 是本项目自己写的 pyo3 绑定，不来自上游。相对上游的
`windows-capture-python` 有两处实质差异：

- 异常按 Rust 侧的错误分类映射到各自的 Python 类型，而不是全部压成一个 `RuntimeError`；
- `grab()` 期间通过 `allow_threads` 释放 GIL。上游绑定全程持有 GIL，会让等待
  present 的时间冻结整个解释器。

### 同步上游

因为做了裁剪，无法直接 `git merge`。同步时对照上述四个文件手工取差异，并检查上游是否
在 `hdr_capture` 里新增了对未搬入模块的引用。
