//! gifrecorder — Rust 实现的 GIF 录制器
//!
//! 替代 PyAV (67 MB) 的轻量级方案。
//! 提供帧存储、JPEG 压缩、后台解码、GIF 导出、Win32 屏幕截取。

pub mod capture;
pub mod decoder;
pub mod frame_store;
pub mod gif_export;
pub mod jpeg;
pub mod recorder;
pub mod resize;

use std::sync::Arc;

use pyo3::prelude::*;
use pyo3::buffer::PyBuffer;
use pyo3::types::{PyBytes, PyDict};

use decoder::FrameDecoder;
use frame_store::{FrameStore, RecordConfig, RecordState};
use recorder::RecordSession;

// ═══════════════════════════════════════════════
//  PyFrameStore — Python 包装
// ═══════════════════════════════════════════════

/// GIF 帧存储器
///
/// 录制阶段接收 BGRA/RGB 帧并压缩为 JPEG；
/// 回放阶段按索引解码为 RGB24；
/// 导出阶段批量编码为 GIF 文件。
#[pyclass(name = "FrameStore")]
struct PyFrameStore {
    inner: Arc<FrameStore>,
}

#[pymethods]
impl PyFrameStore {
    /// 创建 FrameStore
    ///
    /// Args:
    ///     width: 录制区域宽度 (px)
    ///     height: 录制区域高度 (px)
    ///     fps: 目标帧率
    ///     jpeg_quality: JPEG 压缩质量 (1-100, 默认 95)
    ///     max_frames: 最大帧数 (0=不限, 默认 0)
    ///     max_memory_bytes: 最大内存字节数 (0=不限, 默认 0)
    #[new]
    #[pyo3(signature = (width, height, fps, jpeg_quality=95, max_frames=0, max_memory_bytes=0))]
    fn new(
        width: u32,
        height: u32,
        fps: u32,
        jpeg_quality: i32,
        max_frames: usize,
        max_memory_bytes: usize,
    ) -> Self {
        let config = RecordConfig {
            jpeg_quality,
            max_frames,
            max_memory_bytes,
        };
        Self {
            inner: Arc::new(FrameStore::new(width, height, fps, config)),
        }
    }

    // ── 元信息 ──

    /// 录制区域宽度
    #[getter]
    fn width(&self) -> u32 {
        self.inner.width()
    }

    /// 录制区域高度
    #[getter]
    fn height(&self) -> u32 {
        self.inner.height()
    }

    /// 帧率
    #[getter]
    fn fps(&self) -> u32 {
        self.inner.fps()
    }

    /// 当前帧数
    #[getter]
    fn frame_count(&self) -> usize {
        self.inner.frame_count()
    }

    /// JPEG 数据占用的总字节数
    #[getter]
    fn memory_usage_bytes(&self) -> u64 {
        self.inner.memory_usage_bytes()
    }

    /// 累计丢弃的帧数
    #[getter]
    fn dropped_frames(&self) -> u64 {
        self.inner.dropped_frames()
    }

    /// 总录制时长 (毫秒)
    #[getter]
    fn total_duration_ms(&self) -> u32 {
        self.inner.total_duration_ms()
    }

    /// 所有帧的时间戳列表 (毫秒)
    #[getter]
    fn frame_timestamps(&self) -> Vec<u32> {
        self.inner.frame_timestamps()
    }

    /// 获取指定帧的时间戳 (毫秒)
    fn get_elapsed_ms(&self, index: usize) -> PyResult<u32> {
        self.inner.get_elapsed_ms(index).ok_or_else(|| {
            pyo3::exceptions::PyIndexError::new_err(format!("frame index {index} out of range"))
        })
    }

    // ── 录制状态 ──

    /// 是否正在录制
    #[getter]
    fn is_recording(&self) -> bool {
        self.inner.is_recording()
    }

    /// 是否暂停
    #[getter]
    fn is_paused(&self) -> bool {
        self.inner.is_paused()
    }

    /// 设置录制状态
    ///
    /// Args:
    ///     state: 0=Idle, 1=Recording, 2=Paused, 3=Stopped
    fn set_state(&self, state: u8) -> PyResult<()> {
        let s = match state {
            0 => RecordState::Idle,
            1 => RecordState::Recording,
            2 => RecordState::Paused,
            3 => RecordState::Stopped,
            _ => {
                return Err(pyo3::exceptions::PyValueError::new_err(
                    "state must be 0-3 (Idle/Recording/Paused/Stopped)",
                ))
            }
        };
        self.inner.set_state(s);
        Ok(())
    }

    /// 获取当前录制状态 (0=Idle, 1=Recording, 2=Paused, 3=Stopped)
    #[getter]
    fn state(&self) -> u8 {
        match self.inner.state() {
            RecordState::Idle => 0,
            RecordState::Recording => 1,
            RecordState::Paused => 2,
            RecordState::Stopped => 3,
        }
    }

    // ── 录制阶段 ──

    /// 接收一帧 BGRA 数据 (来自 mss 截屏)
    ///
    /// Args:
    ///     bgra: 任意支持 buffer protocol 的对象 (bytes/bytearray/memoryview/ctypes.Array)
    ///           长度 = width * height * 4
    ///     elapsed_ms: 距录制开始的毫秒数
    ///
    /// Returns:
    ///     bool — True 表示正常存储, False 表示有旧帧被丢弃
    fn push_bgra(&self, py: Python<'_>, bgra: PyBuffer<u8>, elapsed_ms: u32) -> PyResult<bool> {
        let slice = buffer_as_bytes(py, &bgra)?;
        self.inner
            .push_bgra(slice, elapsed_ms)
            .map_err(|e| pyo3::exceptions::PyValueError::new_err(e))
    }

    /// 接收一帧 RGB24 数据
    ///
    /// Args:
    ///     rgb: 任意支持 buffer protocol 的对象 (bytes/bytearray/memoryview)
    ///          长度 = width * height * 3
    ///     elapsed_ms: 距录制开始的毫秒数
    ///
    /// Returns:
    ///     bool — True 表示正常存储, False 表示有旧帧被丢弃
    fn push_rgb(&self, py: Python<'_>, rgb: PyBuffer<u8>, elapsed_ms: u32) -> PyResult<bool> {
        let slice = buffer_as_bytes(py, &rgb)?;
        self.inner
            .push_rgb(slice, elapsed_ms)
            .map_err(|e| pyo3::exceptions::PyValueError::new_err(e))
    }

    // ── 回放阶段 ──

    /// 解码单帧为 RGB24 bytes
    ///
    /// Args:
    ///     index: 帧索引
    ///     display_w: 输出宽度 (0=原始尺寸)
    ///     display_h: 输出高度 (0=原始尺寸)
    ///
    /// Returns:
    ///     bytes — RGB24 像素数据
    #[pyo3(signature = (index, display_w=0, display_h=0))]
    fn get_frame_rgb<'py>(
        &self,
        py: Python<'py>,
        index: usize,
        display_w: u32,
        display_h: u32,
    ) -> PyResult<Bound<'py, PyBytes>> {
        let rgb = self
            .inner
            .get_frame_rgb(index, display_w, display_h)
            .map_err(|e| pyo3::exceptions::PyValueError::new_err(e))?;
        Ok(PyBytes::new_bound(py, &rgb))
    }

    /// 创建后台解码器 (用于流式回放)
    ///
    /// Args:
    ///     display_w: 输出宽度 (0=原始尺寸)
    ///     display_h: 输出高度 (0=原始尺寸)
    ///     prefetch: 预解码缓冲区大小 (默认 4)
    ///
    /// Returns:
    ///     FrameDecoder 对象
    #[pyo3(signature = (display_w=0, display_h=0, prefetch=4))]
    fn start_decoder(
        &self,
        display_w: u32,
        display_h: u32,
        prefetch: usize,
    ) -> PyFrameDecoder {
        let decoder = FrameDecoder::start(
            self.inner.clone(),
            display_w,
            display_h,
            prefetch,
        );
        PyFrameDecoder { inner: Some(decoder) }
    }

    // ── GIF 导出 ──

    /// 导出为 GIF 文件
    ///
    /// 在后台线程并行解码 + 缩放，然后顺序编码 GIF。
    /// 支持通过 progress_callback 报告进度，回调返回 False 可取消导出。
    ///
    /// Args:
    ///     path: 输出文件路径
    ///     width: GIF 宽度 (0=原始尺寸)
    ///     height: GIF 高度 (0=原始尺寸)
    ///     repeat: 循环次数 (0=无限循环)
    ///     progress_callback: 可选进度回调 fn(current: int, total: int) -> bool
    ///     cursor_sprites: 可选 dict，鼠标 sprite 集合
    ///         {"cursor": (bytes, w, h), "burst_left_1": ..., "scroll_up": ..., ...}
    ///     cursor_infos: 可选 list[tuple|None]，每帧光标参数
    ///         每个 tuple = (x, y, press, scroll, burst_frame, burst_side)
    ///
    /// Raises:
    ///     ValueError: 导出失败或被取消
    #[pyo3(signature = (path, width=0, height=0, repeat=0, frame_start=0, frame_end=0, progress_callback=None, cursor_sprites=None, cursor_infos=None, speed=None))]
    fn export_gif(
        &self,
        py: Python<'_>,
        path: String,
        width: u32,
        height: u32,
        repeat: u16,
        frame_start: usize,
        frame_end: usize,
        progress_callback: Option<PyObject>,
        cursor_sprites: Option<Bound<'_, PyDict>>,
        cursor_infos: Option<Vec<Option<(i32, i32, u8, i8, u8, u8)>>>,
        speed: Option<f32>,
    ) -> PyResult<()> {
        // 解析 cursor_sprites dict → CursorSprites
        let parsed_sprites = match cursor_sprites {
            Some(ref dict) => Some(parse_cursor_sprites(dict)?),
            None => None,
        };

        // 转换 cursor_infos
        let parsed_infos: Option<Vec<Option<gif_export::CursorInfo>>> = cursor_infos.map(|v| {
            v.into_iter()
                .map(|opt| {
                    opt.map(|(x, y, press, scroll, burst_frame, burst_side)| {
                        gif_export::CursorInfo {
                            x, y, press, scroll, burst_frame, burst_side,
                        }
                    })
                })
                .collect()
        });

        let opts = gif_export::GifExportOptions {
            path,
            width,
            height,
            repeat,
            frame_start,
            frame_end,
            cursor_sprites: parsed_sprites,
            cursor_infos: parsed_infos,
            speed_multiplier: speed.unwrap_or(1.0),
        };

        let store = self.inner.clone();

        // 如果有 Python 回调，需要在 GIL 内调用
        let progress: Option<gif_export::ProgressCallback> = match progress_callback {
            Some(cb) => {
                Some(Box::new(move |current, total| {
                    Python::with_gil(|py| {
                        match cb.call1(py, (current, total)) {
                            Ok(result) => result.is_truthy(py).unwrap_or(true),
                            Err(_) => false, // 回调异常 → 取消
                        }
                    })
                }))
            }
            None => None,
        };

        // 释放 GIL 执行耗时操作
        py.allow_threads(|| {
            gif_export::export_gif(&store, &opts, progress)
                .map_err(|e| pyo3::exceptions::PyValueError::new_err(e))
        })
    }

    /// 取消正在进行的 GIF 导出
    fn cancel_export(&self) {
        self.inner.set_cancel(true);
    }

    /// 清空所有帧数据
    fn clear(&self) {
        self.inner.clear();
    }
}

// ═══════════════════════════════════════════════
//  PyFrameDecoder — Python 包装
// ═══════════════════════════════════════════════

/// 后台帧解码器
///
/// 在独立线程预解码 JPEG → RGB24，Python 侧通过 next_frame() 拉取。
#[pyclass(name = "FrameDecoder")]
struct PyFrameDecoder {
    inner: Option<FrameDecoder>,
}

#[pymethods]
impl PyFrameDecoder {
    /// 取下一帧 (阻塞直到有帧)
    ///
    /// Returns:
    ///     tuple(bytes, int) — (RGB24 数据, elapsed_ms)，或 None 表示所有帧已取完
    fn next_frame<'py>(&mut self, py: Python<'py>) -> PyResult<Option<(Bound<'py, PyBytes>, u32)>> {
        let decoder = self.inner.as_mut().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;

        // 释放 GIL 等待帧
        let frame = py.allow_threads(|| decoder.next_frame());

        match frame {
            Some(f) => {
                let bytes = PyBytes::new_bound(py, &f.rgb);
                Ok(Some((bytes, f.elapsed_ms)))
            }
            None => Ok(None),
        }
    }

    /// 非阻塞尝试取帧
    ///
    /// Returns:
    ///     tuple(bytes, int) 或 None
    fn try_next_frame<'py>(&mut self, py: Python<'py>) -> PyResult<Option<(Bound<'py, PyBytes>, u32)>> {
        let decoder = self.inner.as_mut().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;

        match decoder.try_next_frame() {
            Some(f) => {
                let bytes = PyBytes::new_bound(py, &f.rgb);
                Ok(Some((bytes, f.elapsed_ms)))
            }
            None => Ok(None),
        }
    }

    /// 缓冲区中等待消费的帧数
    #[getter]
    fn buffered_count(&self) -> PyResult<usize> {
        let decoder = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;
        Ok(decoder.buffered_count())
    }

    /// 是否已取完所有帧
    #[getter]
    fn is_finished(&self) -> PyResult<bool> {
        let decoder = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;
        Ok(decoder.is_finished())
    }

    /// 总帧数
    #[getter]
    fn total_frames(&self) -> PyResult<usize> {
        let decoder = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;
        Ok(decoder.total_frames())
    }

    /// 已取帧数
    #[getter]
    fn fetched_count(&self) -> PyResult<usize> {
        let decoder = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;
        Ok(decoder.fetched_count())
    }

    /// 跳过 n 帧（不分配 PyBytes，仅丢弃解码数据，在 Rust 侧释放 GIL）。
    ///
    /// 用于大帧号 seek 时替代 Python 层逐帧调用 next_frame()，
    /// 避免每帧 ~MB 级的 RGB 数据复制到 Python 堆。
    ///
    /// Returns:
    ///     实际跳过帧数（解码器提前结束时小于 n）
    fn skip(&mut self, py: Python<'_>, n: usize) -> PyResult<usize> {
        let decoder = self.inner.as_mut().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("decoder already stopped")
        })?;
        // 释放 GIL：整个 skip 过程在 Rust 侧完成，不分配 PyBytes
        let skipped = py.allow_threads(|| {
            let mut count = 0usize;
            for _ in 0..n {
                if decoder.next_frame().is_none() {
                    break;
                }
                count += 1;
            }
            count
        });
        Ok(skipped)
    }

    /// 停止后台解码线程
    fn stop(&mut self) {
        if let Some(mut d) = self.inner.take() {
            d.stop();
        }
    }

    fn __enter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }

    fn __exit__(
        &mut self,
        _exc_type: &Bound<'_, PyAny>,
        _exc_val: &Bound<'_, PyAny>,
        _exc_tb: &Bound<'_, PyAny>,
    ) {
        self.stop();
    }
}

// ═══════════════════════════════════════════════
//  PyRecordSession — Python 包装
// ═══════════════════════════════════════════════

/// 屏幕录制会话
///
/// 在独立 Rust 线程中执行截屏循环（Win32 BitBlt），
/// Python 侧仅调用 start/pause/resume/stop，完全不碰像素。
///
/// 使用方法:
///     store = gifrecorder.FrameStore(w, h, fps)
///     session = gifrecorder.RecordSession(store, left, top, w, h, fps)
///     # ... 录制中 ...
///     session.pause()
///     session.resume()
///     session.stop()  # 阻塞等待线程退出
#[pyclass(name = "RecordSession")]
struct PyRecordSession {
    inner: Option<RecordSession>,
}

#[pymethods]
impl PyRecordSession {
    /// 创建并启动录制会话
    ///
    /// Args:
    ///     store: FrameStore 实例
    ///     left: 截取区域左上角 X (屏幕坐标)
    ///     top: 截取区域左上角 Y (屏幕坐标)
    ///     width: 截取区域宽度
    ///     height: 截取区域高度
    ///     fps: 目标帧率
    #[new]
    fn new(
        store: &PyFrameStore,
        left: i32,
        top: i32,
        width: i32,
        height: i32,
        fps: u32,
    ) -> PyResult<Self> {
        let session = RecordSession::start(
            store.inner.clone(),
            left, top, width, height, fps,
        )
        .map_err(|e| pyo3::exceptions::PyRuntimeError::new_err(e))?;

        Ok(Self { inner: Some(session) })
    }

    /// 暂停录制
    fn pause(&self) -> PyResult<()> {
        let session = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("session already stopped")
        })?;
        session.pause();
        Ok(())
    }

    /// 恢复录制
    fn resume(&self) -> PyResult<()> {
        let session = self.inner.as_ref().ok_or_else(|| {
            pyo3::exceptions::PyRuntimeError::new_err("session already stopped")
        })?;
        session.resume();
        Ok(())
    }

    /// 停止录制 (阻塞等待截屏线程退出)
    fn stop(&mut self, py: Python<'_>) -> PyResult<()> {
        if let Some(mut session) = self.inner.take() {
            // 释放 GIL，让截屏线程能完成最后工作
            py.allow_threads(|| session.stop());
        }
        Ok(())
    }

    /// 当前状态 (0=idle, 1=recording, 2=paused, 3=stopped)
    #[getter]
    fn state(&self) -> u8 {
        self.inner.as_ref().map_or(3, |s| s.state())
    }

    /// 是否正在录制
    #[getter]
    fn is_recording(&self) -> bool {
        self.inner.as_ref().map_or(false, |s| s.is_recording())
    }

    /// 是否暂停
    #[getter]
    fn is_paused(&self) -> bool {
        self.inner.as_ref().map_or(false, |s| s.is_paused())
    }

    /// 是否已停止
    #[getter]
    fn is_stopped(&self) -> bool {
        self.inner.as_ref().map_or(true, |s| s.is_stopped())
    }
}

// ═══════════════════════════════════════════════
//  辅助: 解析 Python dict → CursorSprites
// ═══════════════════════════════════════════════

/// 从 Python dict 解析一张 sprite: dict[key] = (bytes, w, h)
fn extract_sprite(dict: &Bound<'_, PyDict>, key: &str) -> PyResult<gif_export::Sprite> {
    let item = dict.get_item(key)?.ok_or_else(|| {
        pyo3::exceptions::PyKeyError::new_err(format!("cursor_sprites 缺少 key: {key}"))
    })?;
    let tuple: (Vec<u8>, u32, u32) = item.extract()?;
    Ok(gif_export::Sprite {
        data: tuple.0,
        w: tuple.1,
        h: tuple.2,
    })
}

/// 从 Python cursor_sprites dict 构建 CursorSprites
fn parse_cursor_sprites(dict: &Bound<'_, PyDict>) -> PyResult<gif_export::CursorSprites> {
    let cursor = extract_sprite(dict, "cursor")?;
    let burst_left = [
        extract_sprite(dict, "burst_left_1")?,
        extract_sprite(dict, "burst_left_2")?,
        extract_sprite(dict, "burst_left_3")?,
    ];
    let burst_right = [
        extract_sprite(dict, "burst_right_1")?,
        extract_sprite(dict, "burst_right_2")?,
        extract_sprite(dict, "burst_right_3")?,
    ];
    let scroll_up   = extract_sprite(dict, "scroll_up")?;
    let scroll_down = extract_sprite(dict, "scroll_down")?;

    Ok(gif_export::CursorSprites {
        cursor,
        burst_left,
        burst_right,
        scroll_up,
        scroll_down,
    })
}

// ═══════════════════════════════════════════════
//  辅助: PyBuffer<u8> → &[u8] 零拷贝
// ═══════════════════════════════════════════════

/// 将 PyBuffer<u8> 零拷贝转为 `&[u8]`。
///
/// `PyBuffer::as_slice` 返回 `&[ReadOnlyCell<u8>]`，
/// `ReadOnlyCell<u8>` 与 `u8` 内存布局相同（`#[repr(transparent)]`），
/// 可安全 transmute。
fn buffer_as_bytes<'a>(py: Python<'a>, buf: &'a PyBuffer<u8>) -> PyResult<&'a [u8]> {
    let cells = buf.as_slice(py).ok_or_else(|| {
        pyo3::exceptions::PyBufferError::new_err(
            "buffer 必须是 C-contiguous 连续内存",
        )
    })?;
    // ReadOnlyCell<u8> 是 #[repr(transparent)] 包装的 UnsafeCell<u8>，
    // 与 u8 内存布局完全一致，可安全 cast。
    let ptr = cells.as_ptr() as *const u8;
    let len = cells.len();
    Ok(unsafe { std::slice::from_raw_parts(ptr, len) })
}

// ═══════════════════════════════════════════════
//  常量 — 录制状态值 (方便 Python 引用)
// ═══════════════════════════════════════════════

/// 录制状态常量
///
/// 用法:
///     gifrecorder.STATE_IDLE        # 0
///     gifrecorder.STATE_RECORDING   # 1
///     gifrecorder.STATE_PAUSED      # 2
///     gifrecorder.STATE_STOPPED     # 3
const STATE_IDLE: u8 = 0;
const STATE_RECORDING: u8 = 1;
const STATE_PAUSED: u8 = 2;
const STATE_STOPPED: u8 = 3;

// ═══════════════════════════════════════════════
//  模块定义
// ═══════════════════════════════════════════════

/// gifrecorder — Rust 实现的 GIF 录制器
///
/// 替代 PyAV/FFmpeg，用于屏幕录制和 GIF 导出。
/// 核心功能:
///   - FrameStore: 帧存储管理（JPEG 压缩、内存控制）
///   - RecordSession: Win32 截屏录制（独立 Rust 线程）
///   - FrameDecoder: 后台流式解码（回放用）
///   - export_gif: 高性能 GIF 导出
#[pymodule]
fn gifrecorder(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyFrameStore>()?;
    m.add_class::<PyRecordSession>()?;
    m.add_class::<PyFrameDecoder>()?;

    // 状态常量
    m.add("STATE_IDLE", STATE_IDLE)?;
    m.add("STATE_RECORDING", STATE_RECORDING)?;
    m.add("STATE_PAUSED", STATE_PAUSED)?;
    m.add("STATE_STOPPED", STATE_STOPPED)?;

    // 版本
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;

    Ok(())
}
