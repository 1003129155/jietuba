//! ppocr_rust - PP-OCR (PaddleOCR) ONNX 推理的 Python 扩展
//!
//! 用 Rust + ort(ONNX Runtime)在原生线程跑 det+rec，避免 Python 侧
//! opencv/numpy 依赖与 GIL 争用（推理时通过 allow_threads 释放 GIL）。

mod engine;
mod geometry;

use pyo3::prelude::*;
use pyo3::types::{PyList, PyTuple};

fn to_py<E: std::fmt::Display>(e: E) -> PyErr {
    PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e.to_string())
}

/// 引擎是否已初始化。
#[pyfunction]
fn ppocr_initialized() -> bool {
    engine::is_initialized()
}

/// 初始化引擎（加载 det + rec 模型，字典从 rec 模型 metadata 读取）。
#[pyfunction]
fn ppocr_initialize(py: Python<'_>, det_path: String, rec_path: String) -> PyResult<()> {
    py.allow_threads(|| engine::initialize(&det_path, &rec_path))
        .map_err(to_py)
}

/// 释放引擎。
#[pyfunction]
fn ppocr_release() {
    engine::release();
}

/// 识别整张 RGB 图。
///
/// 参数：data = 紧密或带 stride 的 RGB 字节；w/h 像素尺寸；stride 每行字节数。
/// 返回：list[ (box, text, score) ]，box = [[x,y],[x,y],[x,y],[x,y]]。
#[pyfunction]
fn ppocr_recognize(
    py: Python<'_>,
    data: Vec<u8>,
    w: usize,
    h: usize,
    stride: usize,
) -> PyResult<PyObject> {
    // 释放 GIL 跑推理 —— 主线程 UI 不受影响
    let lines = py
        .allow_threads(|| engine::recognize(&data, w, h, stride))
        .map_err(to_py)?;

    let out = PyList::empty_bound(py);
    for line in lines {
        let box_list = PyList::empty_bound(py);
        for p in &line.box_pts {
            box_list.append(PyList::new_bound(py, [p[0] as f64, p[1] as f64]))?;
        }
        let items: Vec<PyObject> = vec![
            box_list.into_py(py).into_any(),
            line.text.into_py(py),
            (line.score as f64).into_py(py),
        ];
        let tup = PyTuple::new_bound(py, items);
        out.append(tup)?;
    }
    Ok(out.into())
}

#[pymodule]
fn ppocr_rust(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(ppocr_initialized, m)?)?;
    m.add_function(wrap_pyfunction!(ppocr_initialize, m)?)?;
    m.add_function(wrap_pyfunction!(ppocr_release, m)?)?;
    m.add_function(wrap_pyfunction!(ppocr_recognize, m)?)?;
    Ok(())
}
