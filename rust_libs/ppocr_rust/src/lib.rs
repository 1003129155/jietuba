//! ppocr_rust - PP-OCR (PaddleOCR) ONNX 推理的 Python 扩展
//!
//! 用 Rust + ort(ONNX Runtime)在原生线程跑 det+rec，避免 Python 侧
//! opencv/numpy 依赖与 GIL 争用（推理时通过 allow_threads 释放 GIL）。

mod engine;
mod geometry;

use std::sync::Mutex;

use pyo3::create_exception;
use pyo3::prelude::*;

create_exception!(
    ppocr_rust,
    OcrError,
    pyo3::exceptions::PyRuntimeError,
    "模型加载或推理过程中的故障。"
);

fn to_py(e: impl std::fmt::Display) -> PyErr {
    OcrError::new_err(e.to_string())
}

/// 一行识别结果。
///
/// 用具名字段取代原先的 `(box, text, score)` 三元组：调用方不必靠位置
/// 记住谁是谁，IDE 也能给出补全。
#[pyclass(frozen, name = "TextLine")]
pub struct TextLine {
    /// 文本框的四个角点，原图坐标，顺序为左上→右上→右下→左下。
    #[pyo3(get)]
    points: Vec<(f64, f64)>,
    /// 识别出的文本。
    #[pyo3(get)]
    text: String,
    /// 置信度，0.0 ~ 1.0。
    #[pyo3(get)]
    score: f64,
}

#[pymethods]
impl TextLine {
    fn __repr__(&self) -> String {
        format!("TextLine(text={:?}, score={:.3})", self.text, self.score)
    }
}

/// 一个 PP-OCR 引擎实例，自带 det + rec 两个模型。
///
/// 每个实例互相独立，可以同时开多个跑不同模型；实例被回收或 close() 后
/// 模型随之释放。原先是进程级全局单例，开不出第二个引擎，也无法在释放与
/// 推理并发时自保。
#[pyclass(name = "Engine")]
pub struct PyEngine {
    // Option 用于 close()/__exit__ 之后把模型放掉——ONNX 模型有几十 MB，
    // 不能只靠对象回收
    inner: Mutex<Option<engine::Engine>>,
}

#[pymethods]
impl PyEngine {
    /// 加载 det 与 rec 两个 ONNX 模型。识别用的字典从 rec 模型的
    /// `character` metadata 读取，无需额外的字典文件。
    #[new]
    fn new(py: Python<'_>, det_path: &str, rec_path: &str) -> PyResult<Self> {
        // 加载模型是纯 IO + 计算，几百毫秒起步，放开 GIL
        let inner = py
            .allow_threads(|| engine::Engine::open(det_path, rec_path))
            .map_err(to_py)?;
        Ok(Self {
            inner: Mutex::new(Some(inner)),
        })
    }

    /// 识别整张 RGB 图，返回 list[TextLine]。
    ///
    /// 参数：data = 紧密或带 stride 的 RGB 字节；w/h 像素尺寸；stride 每行字节数。
    fn recognize(
        &self,
        py: Python<'_>,
        data: Vec<u8>,
        w: usize,
        h: usize,
        stride: usize,
    ) -> PyResult<Vec<TextLine>> {
        // 推理在原生线程跑，主线程 UI 不受影响
        let lines = py.allow_threads(|| {
            let mut guard = self.inner.lock().map_err(|e| e.to_string())?;
            let engine = guard.as_mut().ok_or("引擎已关闭")?;
            engine.recognize(&data, w, h, stride)
        })
        .map_err(to_py)?;

        Ok(lines
            .into_iter()
            .map(|l| TextLine {
                points: l
                    .box_pts
                    .iter()
                    .map(|p| (p[0] as f64, p[1] as f64))
                    .collect(),
                text: l.text,
                score: l.score as f64,
            })
            .collect())
    }

    /// 释放模型。之后再调用 recognize 会抛 OcrError。重复调用无副作用。
    fn close(&self) {
        if let Ok(mut guard) = self.inner.lock() {
            *guard = None;
        }
    }

    #[getter]
    fn closed(&self) -> bool {
        self.inner.lock().map(|g| g.is_none()).unwrap_or(true)
    }

    fn __enter__(slf: PyRef<'_, Self>) -> PyRef<'_, Self> {
        slf
    }

    #[pyo3(signature = (*_args))]
    fn __exit__(&self, _args: &Bound<'_, pyo3::types::PyTuple>) -> bool {
        self.close();
        false
    }

    fn __repr__(&self) -> String {
        format!(
            "Engine({})",
            if self.closed() { "closed" } else { "open" }
        )
    }
}

#[pymodule]
fn ppocr_rust(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("OcrError", m.py().get_type_bound::<OcrError>())?;
    m.add_class::<PyEngine>()?;
    m.add_class::<TextLine>()?;
    Ok(())
}
