//! oneocr.dll 的 Python 绑定
//!
//! 暴露给 Python 的接口：
//! - `oneocr_available()` → bool
//! - `oneocr_initialize()` → None
//! - `oneocr_recognize(image_data: bytes)` → dict
//! - `oneocr_release()` → None

use pyo3::prelude::*;
use pyo3::types::{PyDict, PyList};

use crate::oneocr::engine;

/// 检查 oneocr 高精度引擎是否可用
#[pyfunction]
pub fn oneocr_available() -> bool {
    engine::is_available()
}

/// 初始化 oneocr 引擎（可选，首次识别时自动初始化）
#[pyfunction]
pub fn oneocr_initialize() -> PyResult<()> {
    engine::initialize()
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
}

/// 识别图像中的文字（线程安全，多线程调用会自动排队）
///
/// Args:
///     image_data (bytes): PNG/JPG/BMP 格式的图像字节数据
///
/// Returns:
///     dict: {
///         "text": str,
///         "text_angle": float | None,
///         "lines": [{ "text": str, "bounding_rect": {...}, "words": [...] }]
///     }
#[pyfunction]
pub fn oneocr_recognize(py: Python<'_>, image_data: &[u8]) -> PyResult<PyObject> {
    // 释放 GIL，让其他 Python 线程可以继续执行
    let result = py
        .allow_threads(|| engine::recognize(image_data))
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))?;

    // 转换为 Python dict
    result_to_py(py, &result)
}

/// 零拷贝识别：从 BGRA 像素指针直接识别文字
///
/// 专为 Qt QImage(Format_ARGB32) 设计：ARGB32 在 little-endian 上
/// 的内存布局恰好是 BGRA，可以直接传 bits() 指针，无需任何格式转换。
///
/// Args:
///     ptr (int): QImage.bits() 的内存地址（整数）
///     width (int): 图像宽度（像素）
///     height (int): 图像高度（像素）
///     stride (int): 每行字节数（QImage.bytesPerLine()）
///
/// Safety:
///     调用方必须保证 ptr 指向的内存在函数返回前有效。
///     由于此函数是阻塞调用（同步），Python 侧只需确保
///     QImage 变量在调用期间不被销毁即可。
#[pyfunction]
pub fn oneocr_recognize_raw(
    py: Python<'_>,
    ptr: usize,
    width: i32,
    height: i32,
    stride: i32,
) -> PyResult<PyObject> {
    // 释放 GIL，让其他 Python 线程可以继续执行
    let result = py
        .allow_threads(|| engine::recognize_raw(ptr, width, height, stride))
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))?;

    // 转换为 Python dict
    result_to_py(py, &result)
}

/// 释放 oneocr 引擎资源
#[pyfunction]
pub fn oneocr_release() {
    engine::release();
}

// ============================================================
// 内部转换：Rust 结构体 → Python dict
// ============================================================

fn result_to_py(py: Python<'_>, result: &engine::OneOcrResult) -> PyResult<PyObject> {
    let dict = PyDict::new_bound(py);

    dict.set_item("text", &result.text)?;
    dict.set_item("text_angle", result.text_angle)?;

    let lines = PyList::empty_bound(py);
    for line in &result.lines {
        lines.append(line_to_py(py, line)?)?;
    }
    dict.set_item("lines", lines)?;

    Ok(dict.into())
}

fn line_to_py(py: Python<'_>, line: &engine::OneOcrLine) -> PyResult<PyObject> {
    let dict = PyDict::new_bound(py);

    dict.set_item("text", &line.text)?;
    dict.set_item("bounding_rect", bbox_to_py(py, &line.bounding_rect)?)?;

    let words = PyList::empty_bound(py);
    for word in &line.words {
        words.append(word_to_py(py, word)?)?;
    }
    dict.set_item("words", words)?;

    Ok(dict.into())
}

fn word_to_py(py: Python<'_>, word: &engine::OneOcrWord) -> PyResult<PyObject> {
    let dict = PyDict::new_bound(py);

    dict.set_item("text", &word.text)?;
    dict.set_item("bounding_rect", bbox_to_py(py, &word.bounding_rect)?)?;
    dict.set_item("confidence", word.confidence)?;

    Ok(dict.into())
}

fn bbox_to_py(py: Python<'_>, bbox: &Option<engine::BoundingRect>) -> PyResult<PyObject> {
    match bbox {
        Some(b) => {
            let dict = PyDict::new_bound(py);
            dict.set_item("x1", b.x1)?;
            dict.set_item("y1", b.y1)?;
            dict.set_item("x2", b.x2)?;
            dict.set_item("y2", b.y2)?;
            dict.set_item("x3", b.x3)?;
            dict.set_item("y3", b.y3)?;
            dict.set_item("x4", b.x4)?;
            dict.set_item("y4", b.y4)?;
            Ok(dict.into())
        }
        None => Ok(py.None()),
    }
}

/// 将 oneocr 的函数注册到 Python 模块（由 python.rs 调用）
pub fn register_oneocr_functions(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(oneocr_available, m)?)?;
    m.add_function(wrap_pyfunction!(oneocr_initialize, m)?)?;
    m.add_function(wrap_pyfunction!(oneocr_recognize, m)?)?;
    m.add_function(wrap_pyfunction!(oneocr_recognize_raw, m)?)?;
    m.add_function(wrap_pyfunction!(oneocr_release, m)?)?;
    Ok(())
}
