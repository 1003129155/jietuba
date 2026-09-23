//! HDR 正确的 Windows 桌面截图，供 Python 调用。
//!
//! 底层模块 vendor 自 NiiightmareXD/windows-capture（MIT），只保留 DXGI Desktop Duplication
//! 相关部分；出处与裁剪范围见 README.md。

pub mod d3d11;
pub mod dxgi_duplication_api;
pub mod hdr_capture;
pub mod monitor;

use pyo3::create_exception;
use pyo3::exceptions::PyRuntimeError;
use pyo3::prelude::*;
use pyo3::types::{PyBytes, PyDict, PyList};

use crate::hdr_capture::{
    Capture as HdrCapture, Error as HdrError, Frame as HdrFrame, FrameMonitorInfo, Monitor, ToneMapping,
};

create_exception!(hdrcapture, CaptureError, PyRuntimeError);
create_exception!(hdrcapture, InitialFrameTimeout, CaptureError);
create_exception!(hdrcapture, AccessLost, CaptureError);
create_exception!(hdrcapture, DimensionsChanged, CaptureError);
create_exception!(hdrcapture, InvalidMonitorIndex, CaptureError);

/// 把 Rust 侧已分好类的错误映射到对应的 Python 异常。
///
/// 调用方需要区分「等不到首帧」（应加大预算重试）和「显示器索引非法」（应直接报错），
/// 把两者压成同一个 RuntimeError 会迫使调用方去匹配错误字符串。
fn to_py_error(error: &HdrError) -> PyErr {
    let message = error.to_string();
    match error {
        HdrError::InitialFrameTimeout { .. } => InitialFrameTimeout::new_err(message),
        HdrError::AccessLost => AccessLost::new_err(message),
        HdrError::DimensionsChanged { .. } => DimensionsChanged::new_err(message),
        HdrError::InvalidMonitorIndex { .. } => InvalidMonitorIndex::new_err(message),
        _ => CaptureError::new_err(message),
    }
}

/// D3D11 immediate context 不是线程安全的，`#[pyclass(unsendable)]` 已把对象钉在创建它的
/// 线程上，`&mut self` 又排除了同线程重入，因此释放 GIL 期间不会有第二个线程碰到它。
/// `allow_threads` 要求闭包 `Send` 只是为了拦截 Python 对象跨 GIL 边界，此处不涉及。
struct GilReleased<T>(T);

// SAFETY: 见上。闭包由 `allow_threads` 在原线程执行，不会发生跨线程移动。
unsafe impl<T> Send for GilReleased<T> {}

#[pyclass(module = "hdrcapture", name = "Monitor", frozen, get_all)]
#[derive(Clone)]
pub struct PyMonitor {
    /// 0 为完整虚拟桌面，正数为 EnumDisplayMonitors 顺序中的物理显示器。
    pub index: usize,
    /// (x, y, width, height)，Windows 物理像素；左侧/上方的显示器 x/y 可为负。
    pub rect: (i32, i32, u32, u32),
    pub is_virtual_desktop: bool,
    pub device_name: String,
    pub friendly_name: String,
    pub hdr_enabled: bool,
    pub hdr_supported: bool,
}

impl From<&Monitor> for PyMonitor {
    fn from(monitor: &Monitor) -> Self {
        Self {
            index: monitor.index,
            rect: (monitor.rect.x, monitor.rect.y, monitor.rect.width, monitor.rect.height),
            is_virtual_desktop: monitor.is_virtual_desktop,
            device_name: monitor.device_name.clone(),
            friendly_name: monitor.friendly_name.clone(),
            hdr_enabled: monitor.hdr_enabled,
            hdr_supported: monitor.hdr_supported,
        }
    }
}

#[pymethods]
impl PyMonitor {
    fn __repr__(&self) -> String {
        format!(
            "Monitor(index={}, rect={:?}, hdr_enabled={}, friendly_name={:?})",
            self.index,
            self.rect,
            if self.hdr_enabled { "True" } else { "False" },
            self.friendly_name
        )
    }
}

fn monitor_info_to_py<'py>(py: Python<'py>, info: &FrameMonitorInfo) -> PyResult<Bound<'py, PyDict>> {
    let dict = PyDict::new_bound(py);
    dict.set_item("index", info.index)?;
    dict.set_item("rect", (info.rect.x, info.rect.y, info.rect.width, info.rect.height))?;
    dict.set_item("hdr_enabled", info.hdr_enabled)?;
    dict.set_item("source_format", &info.source_format)?;
    dict.set_item("source_color_space", info.source_color_space.clone())?;
    dict.set_item("output_format", &info.output_format)?;
    dict.set_item("tone_map_peak", info.tone_map_peak)?;
    Ok(dict)
}

/// 一张 sRGB `BGRA8` 截图。
///
/// 像素在构造时一次性搬进 `PyBytes`，Rust 侧的 `Vec` 随即释放，因此一帧全程只占一份
/// 缓冲；重复读取 `bgra` 只是增加引用计数，不会像每次现转那样反复分配整幅图。
#[pyclass(module = "hdrcapture", name = "Frame")]
pub struct PyFrame {
    bgra: Py<PyBytes>,
    width: u32,
    height: u32,
    monitor_info: Vec<FrameMonitorInfo>,
}

impl PyFrame {
    fn from_frame(py: Python<'_>, frame: HdrFrame) -> Self {
        Self {
            bgra: PyBytes::new_bound(py, frame.bgra()).unbind(),
            width: frame.width,
            height: frame.height,
            monitor_info: frame.monitor_info,
        }
    }
}

#[pymethods]
impl PyFrame {
    #[getter]
    const fn width(&self) -> u32 {
        self.width
    }

    #[getter]
    const fn height(&self) -> u32 {
        self.height
    }

    /// 紧凑的 sRGB BGRA8，无行填充，可直接交给 QImage / Pillow。
    #[getter]
    fn bgra(&self, py: Python<'_>) -> Py<PyBytes> {
        self.bgra.clone_ref(py)
    }

    /// 去掉 Alpha 的 BGR8；按需分配，不用就不付这份拷贝。
    #[getter]
    fn bgr<'py>(&self, py: Python<'py>) -> Bound<'py, PyBytes> {
        let bgra = self.bgra.bind(py).as_bytes();
        let mut bgr = Vec::with_capacity(bgra.len() / 4 * 3);
        for pixel in bgra.chunks_exact(4) {
            bgr.extend_from_slice(&pixel[..3]);
        }
        PyBytes::new_bound(py, &bgr)
    }

    /// 本帧每个参与物理输出的来源诊断（HDR 状态、DXGI 源格式等）。
    #[getter]
    fn monitor_info<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyList>> {
        let items = self
            .monitor_info
            .iter()
            .map(|info| monitor_info_to_py(py, info))
            .collect::<PyResult<Vec<_>>>()?;
        PyList::new_bound(py, items).extract()
    }

    fn __repr__(&self) -> String {
        format!(
            "Frame(width={}, height={}, monitors={})",
            self.width,
            self.height,
            self.monitor_info.len()
        )
    }
}

/// 常驻的 HDR 捕获会话。
///
/// 每个物理显示器保留一个 DXGI Desktop Duplication 会话，连续 `grab` 复用。不要为每帧新建：
/// 新会话必须先等到一次真实的桌面 present 才会返回首帧，显示器休眠时会等不到。
#[pyclass(module = "hdrcapture", name = "Capture", unsendable)]
pub struct PyCapture {
    capture: Option<HdrCapture>,
}

impl PyCapture {
    fn inner(&mut self) -> PyResult<&mut HdrCapture> {
        self.capture.as_mut().ok_or_else(|| CaptureError::new_err("capture is closed"))
    }

    fn inner_ref(&self) -> PyResult<&HdrCapture> {
        self.capture.as_ref().ok_or_else(|| CaptureError::new_err("capture is closed"))
    }
}

#[pymethods]
impl PyCapture {
    #[new]
    #[pyo3(signature = (timeout_ms = 100))]
    fn new(timeout_ms: u32) -> PyResult<Self> {
        let capture = HdrCapture::with_timeout(timeout_ms).map_err(|error| to_py_error(&error))?;
        Ok(Self { capture: Some(capture) })
    }

    /// 索引 0 为虚拟桌面，其后是当前物理显示器。热插拔后需重新读取，不要跨帧缓存索引。
    #[getter]
    fn monitors(&self) -> PyResult<Vec<PyMonitor>> {
        Ok(self.inner_ref()?.monitors().iter().map(PyMonitor::from).collect())
    }

    /// 捕获虚拟桌面或单块显示器，返回始终为 sRGB BGRA8。
    ///
    /// `timeout_ms` 是**每个物理输出**的 DXGI 等待预算，不是整次调用的总预算。
    ///
    /// `adaptive=True` 时，画面里有足够的 HDR 内容就整屏压暗、给高光留出层次，结果随内容变化；
    /// 默认的固定映射让 SDR 内容逐像素不变，同一内容多次抓取结果一致，拼接和录制要用它。
    #[pyo3(signature = (monitor, timeout_ms = None, adaptive = false))]
    fn grab(
        &mut self,
        py: Python<'_>,
        monitor: &Bound<'_, PyAny>,
        timeout_ms: Option<u32>,
        adaptive: bool,
    ) -> PyResult<PyFrame> {
        let index = resolve_monitor_index(monitor)?;
        let capture = GilReleased(self.inner()?);
        let tone_mapping = if adaptive { ToneMapping::Adaptive } else { ToneMapping::Static };

        // 等待 present 期间持有 GIL 会冻结整个解释器，实测单次可达预算上限。
        let result = py.allow_threads(move || {
            let capture = capture;
            let timeout_ms = timeout_ms.unwrap_or_else(|| capture.0.timeout_ms());
            capture.0.grab_with_tone_mapping(index, timeout_ms, tone_mapping)
        });

        result.map(|frame| PyFrame::from_frame(py, frame)).map_err(|error| to_py_error(&error))
    }

    /// 端到端与最终读回阶段的滚动耗时（毫秒）。
    #[getter]
    fn stats<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyDict>> {
        let stats = self.inner_ref()?.stats();
        let dict = PyDict::new_bound(py);
        dict.set_item("frames", stats.frames)?;
        dict.set_item("total_grab_ms", stats.total_grab_time.as_secs_f64() * 1_000.0)?;
        dict.set_item("first_frame_ms", stats.first_frame_time.map(|v| v.as_secs_f64() * 1_000.0))?;
        dict.set_item("average_grab_ms", stats.average_grab_time.map(|v| v.as_secs_f64() * 1_000.0))?;
        dict.set_item("p95_grab_ms", stats.p95_grab_time.map(|v| v.as_secs_f64() * 1_000.0))?;
        dict.set_item("average_readback_ms", stats.average_readback_time.map(|v| v.as_secs_f64() * 1_000.0))?;
        Ok(dict)
    }

    fn reset_stats(&mut self) -> PyResult<()> {
        self.inner()?.reset_stats();
        Ok(())
    }

    #[getter]
    const fn closed(&self) -> bool {
        self.capture.is_none()
    }

    /// 释放 DXGI 与 D3D11 资源。可重复调用。
    fn close(&mut self) {
        self.capture = None;
    }

    fn __enter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }

    fn __exit__(
        &mut self,
        _exc_type: &Bound<'_, PyAny>,
        _exc_value: &Bound<'_, PyAny>,
        _traceback: &Bound<'_, PyAny>,
    ) -> bool {
        self.close();
        false
    }
}

/// 接受 `Monitor`、整数索引，或带 `index` 键/属性的对象，便于从 mss 的 `monitors[0]` 字典迁移。
fn resolve_monitor_index(monitor: &Bound<'_, PyAny>) -> PyResult<usize> {
    if let Ok(value) = monitor.extract::<PyRef<'_, PyMonitor>>() {
        return Ok(value.index);
    }
    if let Ok(index) = monitor.extract::<usize>() {
        return Ok(index);
    }

    // 候选值必须能取出整数才算数：str 之类自带 `index` 方法的对象会在这里落空并走统一报错，
    // 否则调用方只会看到一句与显示器无关的 TypeError。
    monitor
        .get_item("index")
        .ok()
        .or_else(|| monitor.getattr("index").ok())
        .and_then(|value| value.extract::<usize>().ok())
        .ok_or_else(|| {
            InvalidMonitorIndex::new_err(
                "monitor must be a Monitor, a non-negative integer index, or expose an integer 'index'",
            )
        })
}

#[pymodule]
fn hdrcapture(module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyCapture>()?;
    module.add_class::<PyFrame>()?;
    module.add_class::<PyMonitor>()?;
    module.add("CaptureError", module.py().get_type_bound::<CaptureError>())?;
    module.add("InitialFrameTimeout", module.py().get_type_bound::<InitialFrameTimeout>())?;
    module.add("AccessLost", module.py().get_type_bound::<AccessLost>())?;
    module.add("DimensionsChanged", module.py().get_type_bound::<DimensionsChanged>())?;
    module.add("InvalidMonitorIndex", module.py().get_type_bound::<InvalidMonitorIndex>())?;
    Ok(())
}
