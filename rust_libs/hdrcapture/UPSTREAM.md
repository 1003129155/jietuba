# 与 windows-capture 的关系（维护说明）

`src/d3d11.rs`、`src/dxgi_duplication_api.rs`、`src/monitor.rs` 取自
[NiiightmareXD/windows-capture](https://github.com/NiiightmareXD/windows-capture)，
MIT，版权声明见 `LICENSE-UPSTREAM`（分发 `.pyd` 或编进它的程序时必须随附）。

- 基准提交：`c7d106448eb9d9b251345c39047711e1cd408ae2`
- `src/hdr_capture/` 和 Python 绑定（`src/python.rs`）是本项目写的，上游没有对应代码。

## 搬入范围

只搬 `hdr_capture` 依赖的三个文件。`capture`、`encoder`、`frame`、`graphics_capture_api`、
`graphics_capture_picker`、`settings`、`window` 属于 Windows.Graphics.Capture 那套 API，
没有搬入；`windows` crate 的 feature 也相应从上游的 30 项减到 11 项。

## 相对上游的改动

搬入时为切断对未搬入模块的引用做的删除：

- `dxgi_duplication_api.rs`：删掉两个 `save_as_image()`、`ImageEncoderError` 错误变体及对
  `crate::encoder` 的引用。
- `monitor.rs`：删掉 `impl TryInto<GraphicsCaptureItemType> for Monitor` 及对
  `crate::settings` 的引用。
- 连带失效的 rustdoc 链接一并修正；文档示例里的 crate 路径由 `windows_capture` 改为 `hdrcapture`。

之后本项目在 `dxgi_duplication_api.rs` 上还有功能改动（旧帧判定、区域裁剪读回），以 git 历史为准。

## 同步上游

裁剪过，无法直接 `git merge`。同步时对照这三个文件手工取差异，并检查上游改动是否引用了
未搬入的模块。
