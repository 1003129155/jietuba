pub mod error;
pub mod hash;
pub mod lcs;
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
///   ignore_img1_bottom_ratio 匹配时忽略 img1 底部的比例
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

#[pymodule]
fn longstitch(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("StitchError", m.py().get_type_bound::<StitchError>())?;
    m.add_class::<PyStitchResult>()?;
    m.add_function(wrap_pyfunction!(stitch_images, m)?)?;
    Ok(())
}
