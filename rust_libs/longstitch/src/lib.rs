pub mod error;
pub mod hash;
pub mod lcs;
pub mod session;
pub mod stitch;

use pyo3::create_exception;
use pyo3::prelude::*;
use pyo3::types::PyBytes;

use crate::error::StitchError as CoreError;

create_exception!(
    longstitch,
    StitchError,
    pyo3::exceptions::PyRuntimeError,
    "拼接过程中发生的故障（解码失败、编码失败等）。\n\n\
     注意「两图无重叠」不走异常——那是合法结论，stitch() 对此返回 None。"
);

/// 一次拼接的结果。
///
/// 用具名字段而不是裸元组，调用方不必靠位置记住谁是谁。
#[pyclass(frozen, name = "StitchResult")]
pub struct PyStitchResult {
    /// 拼接结果，PNG 字节。
    #[pyo3(get)]
    png: Py<PyBytes>,
    /// 拼接方向："forward" 或 "reverse"。
    ///
    /// 只有 detect_direction=True 时才可能是 "reverse"，此时返回的是翻转态结果，
    /// 调用方负责最终输出时翻转还原。
    #[pyo3(get)]
    direction: String,
}

#[pymethods]
impl PyStitchResult {
    fn __repr__(&self, py: Python<'_>) -> String {
        format!(
            "StitchResult(png=<{} bytes>, direction='{}')",
            self.png.bind(py).as_bytes().len(),
            self.direction
        )
    }
}

/// 拼接两张竖向连续的截图。
///
/// 参数：
///   img1, img2               PNG/JPEG 等编码后的图片字节
///   detect_direction         自动判断 img2 在 img1 的上方还是下方；
///                            开启时 ignore_img1_*_ratio 不生效（方向未知，无从取舍）
///   ignore_right_pixels      匹配时忽略右侧多少像素（躲开滚动条）
///   ignore_top_pixels        匹配时忽略顶部多少像素（躲开固定表头）
///   min_overlap_ratio        判定重叠成立所需的最小重叠占比
///   ignore_img1_top_ratio    匹配时忽略 img1 顶部的比例
///   ignore_img1_bottom_ratio 匹配时忽略 img1 底部的行数，按 img2 高度的比例计
///   debug                    向标准输出打印匹配过程
///
/// 返回 StitchResult；两图接不上时返回 None。
/// 解码或编码失败抛 StitchError。
#[pyfunction]
#[pyo3(name = "stitch")]
#[pyo3(signature = (
    img1,
    img2,
    *,
    detect_direction = false,
    ignore_right_pixels = 20,
    ignore_top_pixels = 0,
    min_overlap_ratio = 0.01,
    ignore_img1_top_ratio = 0.0,
    ignore_img1_bottom_ratio = 0.0,
    debug = false,
))]
#[allow(clippy::too_many_arguments)]
fn stitch_images(
    py: Python<'_>,
    img1: Vec<u8>,
    img2: Vec<u8>,
    detect_direction: bool,
    ignore_right_pixels: u32,
    ignore_top_pixels: u32,
    min_overlap_ratio: f32,
    ignore_img1_top_ratio: f32,
    ignore_img1_bottom_ratio: f32,
    debug: bool,
) -> PyResult<Option<PyStitchResult>> {
    // 解码 + LCS + 拼接是纯计算，可能耗时数百毫秒；不放开 GIL 会卡住调用方的界面线程
    let outcome = py.allow_threads(|| match (detect_direction, debug) {
        (true, false) => stitch::stitch_two_images_smart_auto(
            &img1,
            &img2,
            ignore_right_pixels,
            ignore_top_pixels,
            min_overlap_ratio,
        ),
        (true, true) => stitch::stitch_two_images_smart_auto_debug(
            &img1,
            &img2,
            ignore_right_pixels,
            ignore_top_pixels,
            min_overlap_ratio,
        ),
        (false, false) => stitch::stitch_two_images_smart(
            &img1,
            &img2,
            ignore_right_pixels,
            ignore_top_pixels,
            min_overlap_ratio,
            ignore_img1_top_ratio,
            ignore_img1_bottom_ratio,
        )
        .map(|png| (png, "forward".to_string())),
        (false, true) => stitch::stitch_two_images_smart_debug(
            &img1,
            &img2,
            ignore_right_pixels,
            ignore_top_pixels,
            min_overlap_ratio,
            ignore_img1_top_ratio,
            ignore_img1_bottom_ratio,
        )
        .map(|png| (png, "forward".to_string())),
    });

    match outcome {
        Ok((png, direction)) => Ok(Some(PyStitchResult {
            png: PyBytes::new_bound(py, &png).unbind(),
            direction,
        })),
        // 无重叠是算法给出的合法结论，不是故障
        Err(CoreError::NoOverlap) => Ok(None),
        Err(e) => Err(StitchError::new_err(e.to_string())),
    }
}

/// 会话推入一帧后的结论：长图先截到 keep 行，再接上新帧从 skip 行开始的部分。
#[pyclass(frozen, name = "PushResult")]
pub struct PyPushResult {
    /// 拼接结果保留的行数（第一帧为 0）。
    #[pyo3(get)]
    keep: usize,
    /// 新帧跳过的行数（第一帧为 0，即整帧追加）。
    #[pyo3(get)]
    skip: usize,
    /// 推入后拼接结果的总行数。
    #[pyo3(get)]
    height: usize,
    /// "forward" 或 "reverse"。
    ///
    /// 只有 detect_direction=True 时才可能是 "reverse"：会话里的长图已整体翻转，
    /// 调用方此后要先翻转帧再推入，最终输出时再翻转还原。
    #[pyo3(get)]
    direction: String,
}

#[pymethods]
impl PyPushResult {
    fn __repr__(&self) -> String {
        format!(
            "PushResult(keep={}, skip={}, height={}, direction='{}')",
            self.keep, self.skip, self.height, self.direction
        )
    }
}

/// 增量拼接会话。
///
/// 逐帧推入 BGRA 截图，会话只回传拼接参数；已拼好的长图留在会话里，
/// 需要时用 export() 一次取出。匹配规则与 stitch() 相同。
///
/// 参数：
///   width, height        每一帧的宽高
///   ignore_right_pixels  匹配时忽略右侧多少像素（躲开滚动条）
///   ignore_top_pixels    匹配时忽略新帧顶部多少像素（躲开固定表头）
///   min_overlap_ratio    判定重叠成立所需的最小重叠占比
#[pyclass(name = "StitchSession")]
pub struct PyStitchSession {
    inner: session::StitchSession,
}

#[pymethods]
impl PyStitchSession {
    #[new]
    #[pyo3(signature = (width, height, *, ignore_right_pixels = 20, ignore_top_pixels = 0, min_overlap_ratio = 0.01))]
    fn new(
        width: u32,
        height: u32,
        ignore_right_pixels: u32,
        ignore_top_pixels: u32,
        min_overlap_ratio: f32,
    ) -> PyResult<Self> {
        session::StitchSession::new(width, height, ignore_right_pixels, ignore_top_pixels, min_overlap_ratio)
            .map(|inner| Self { inner })
            .map_err(|e| StitchError::new_err(e.to_string()))
    }

    /// 每一帧的宽度。
    #[getter]
    fn width(&self) -> u32 {
        self.inner.width()
    }

    /// 当前拼接结果的行数，尚未推入任何帧时为 0。
    #[getter]
    fn height(&self) -> usize {
        self.inner.height()
    }

    /// 推入一帧 BGRA 截图（bytes，长度须为 width*height*4）。
    ///
    /// 第一帧只作为起点。接不上时返回 None，会话保持不变；
    /// 帧尺寸不符抛 StitchError。
    ///   detect_direction         自动判断新帧在上方还是下方，参数含义同 stitch()
    ///   ignore_img1_top_ratio    匹配时忽略拼接结果顶部的比例
    ///   ignore_img1_bottom_ratio 匹配时忽略拼接结果底部的行数，按帧高的比例计
    #[pyo3(signature = (bgra, *, detect_direction = false, ignore_img1_top_ratio = 0.0, ignore_img1_bottom_ratio = 0.0, debug = false))]
    fn push(
        &mut self,
        py: Python<'_>,
        bgra: &[u8],
        detect_direction: bool,
        ignore_img1_top_ratio: f32,
        ignore_img1_bottom_ratio: f32,
        debug: bool,
    ) -> PyResult<Option<PyPushResult>> {
        let inner = &mut self.inner;
        let outcome = py.allow_threads(|| {
            inner.push_bgra(bgra, detect_direction, ignore_img1_top_ratio, ignore_img1_bottom_ratio, debug)
        });
        match outcome {
            Ok(o) => Ok(Some(PyPushResult {
                keep: o.keep,
                skip: o.skip,
                height: self.inner.height(),
                direction: if o.reversed { "reverse" } else { "forward" }.to_string(),
            })),
            Err(CoreError::NoOverlap) => Ok(None),
            Err(e) => Err(StitchError::new_err(e.to_string())),
        }
    }

    /// 当前拼接结果，BGRA 字节，尺寸为 width x height。
    fn export<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyBytes>> {
        let inner = &self.inner;
        // 直接写进新建的 bytes，不经中间缓冲；该对象此时还没交给 Python，释放 GIL 写入是安全的
        PyBytes::new_bound_with(py, inner.byte_len(), |buf| {
            py.allow_threads(|| inner.export_into(buf));
            Ok(())
        })
    }

    /// 立即释放拼接结果占用的内存。
    fn close(&mut self) {
        self.inner.clear();
    }

    fn __repr__(&self) -> String {
        format!("StitchSession(width={}, height={})", self.inner.width(), self.inner.height())
    }
}

#[pymodule]
fn longstitch(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("StitchError", m.py().get_type_bound::<StitchError>())?;
    m.add_class::<PyStitchResult>()?;
    m.add_class::<PyPushResult>()?;
    m.add_class::<PyStitchSession>()?;
    m.add_function(wrap_pyfunction!(stitch_images, m)?)?;
    Ok(())
}
