use std::sync::Mutex;

use pyo3::create_exception;
use pyo3::prelude::*;

use std::path::Path;

use crate::{engine, models};

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
    /// 加载 det 与 rec 两个 ONNX 模型。不给 dict_path 时，识别用的字典从 rec 模型的
    /// `character` metadata 读取。
    #[new]
    #[pyo3(signature = (det_path, rec_path, dict_path=None))]
    fn new(py: Python<'_>, det_path: &str, rec_path: &str, dict_path: Option<&str>) -> PyResult<Self> {
        // 加载模型是纯 IO + 计算，几百毫秒起步，放开 GIL
        let inner = py
            .allow_threads(|| engine::Engine::open_with_dict(det_path, rec_path, dict_path))
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

fn json(value: &impl serde::Serialize) -> PyResult<String> {
    serde_json::to_string(value).map_err(to_py)
}

/// 内置的可下载模型清单（JSON）。
#[pyfunction]
fn models_catalog() -> PyResult<String> {
    json(models::catalog())
}

/// `root` 下已安装的模型列表（JSON），默认模型的 id 为空串。
#[pyfunction]
fn models_discover(root: &str) -> PyResult<String> {
    json(&models::discover(Path::new(root)))
}

/// 按 id 找已安装的模型（JSON），没有时返回 None。
#[pyfunction]
fn models_resolve(root: &str, model_id: &str) -> PyResult<Option<String>> {
    models::resolve(Path::new(root), model_id).map(|m| json(&m)).transpose()
}

/// 「自动」用的已装模型（JSON），一个都没有时返回 None。
#[pyfunction]
fn models_auto_choice(root: &str) -> PyResult<Option<String>> {
    models::auto_choice(Path::new(root)).map(|m| json(&m)).transpose()
}

/// 复制并校验用户选的模型文件，成功后返回新模型（JSON）。
#[pyfunction]
#[pyo3(signature = (root, name, det_path, rec_path, dict_path=None))]
fn models_import(py: Python<'_>, root: &str, name: &str, det_path: &str, rec_path: &str, dict_path: Option<&str>) -> PyResult<String> {
    let model = py
        .allow_threads(|| {
            models::import(Path::new(root), name, Path::new(det_path), Path::new(rec_path), dict_path.map(Path::new))
        })
        .map_err(to_py)?;
    json(&model)
}

/// 更新器下载完清单模型后调用，返回就位的模型（JSON）。
#[pyfunction]
fn models_finish_download(py: Python<'_>, root: &str, catalog_id: &str) -> PyResult<String> {
    let model = py.allow_threads(|| models::finish_download(Path::new(root), catalog_id)).map_err(to_py)?;
    json(&model)
}

#[pyfunction]
fn models_remove(py: Python<'_>, root: &str, model_id: &str) -> PyResult<()> {
    py.allow_threads(|| models::remove(Path::new(root), model_id)).map_err(to_py)
}

#[pyfunction]
fn models_clean_staging(py: Python<'_>, root: &str) {
    py.allow_threads(|| models::clean_staging(Path::new(root)));
}

#[pymodule]
fn ppocr_rust(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("OcrError", m.py().get_type_bound::<OcrError>())?;
    m.add_class::<PyEngine>()?;
    m.add_class::<TextLine>()?;
    for function in [
        wrap_pyfunction!(models_catalog, m)?,
        wrap_pyfunction!(models_discover, m)?,
        wrap_pyfunction!(models_resolve, m)?,
        wrap_pyfunction!(models_auto_choice, m)?,
        wrap_pyfunction!(models_import, m)?,
        wrap_pyfunction!(models_finish_download, m)?,
        wrap_pyfunction!(models_remove, m)?,
        wrap_pyfunction!(models_clean_staging, m)?,
    ] {
        m.add_function(function)?;
    }
    Ok(())
}
