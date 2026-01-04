use pyo3::prelude::*;
use pyo3::types::PyDict;

/// Python 版本的边界框
#[pyclass]
#[derive(Clone)]
pub struct PyBoundingBox {
    #[pyo3(get)]
    pub x: f32,
    #[pyo3(get)]
    pub y: f32,
    #[pyo3(get)]
    pub width: f32,
    #[pyo3(get)]
    pub height: f32,
}

#[pymethods]
impl PyBoundingBox {
    fn __repr__(&self) -> String {
        format!("BoundingBox(x={}, y={}, width={}, height={})", 
            self.x, self.y, self.width, self.height)
    }
    
    fn to_dict<'py>(&self, py: Python<'py>) -> PyResult<Py<PyDict>> {
        let dict = PyDict::new_bound(py);
        dict.set_item("x", self.x)?;
        dict.set_item("y", self.y)?;
        dict.set_item("width", self.width)?;
        dict.set_item("height", self.height)?;
        Ok(dict.into())
    }
}

/// Python 版本的单词
#[pyclass]
#[derive(Clone)]
pub struct PyOcrWord {
    #[pyo3(get)]
    pub text: String,
    #[pyo3(get)]
    pub bounds: PyBoundingBox,
}

#[pymethods]
impl PyOcrWord {
    fn __repr__(&self) -> String {
        format!("OcrWord(text='{}', bounds={})", self.text, self.bounds.__repr__())
    }
}

/// Python 版本的行
#[pyclass]
#[derive(Clone)]
pub struct PyOcrLine {
    #[pyo3(get)]
    pub text: String,
    #[pyo3(get)]
    pub bounds: PyBoundingBox,
    #[pyo3(get)]
    pub words: Vec<PyOcrWord>,
}

#[pymethods]
impl PyOcrLine {
    fn __repr__(&self) -> String {
        format!("OcrLine(text='{}', words={})", self.text, self.words.len())
    }
}

/// Python 版本的 OCR 结果
#[pyclass]
#[derive(Clone)]
pub struct PyOcrResult {
    #[pyo3(get)]
    pub text: String,
    #[pyo3(get)]
    pub lines: Vec<PyOcrLine>,
    #[pyo3(get)]
    pub text_angle: Option<f64>,
}

#[pymethods]
impl PyOcrResult {
    fn __repr__(&self) -> String {
        format!("OcrResult(lines={}, text_angle={:?})", self.lines.len(), self.text_angle)
    }
    
    /// 转换为字典格式
    fn to_dict<'py>(&self, py: Python<'py>) -> PyResult<Py<PyDict>> {
        let dict = PyDict::new_bound(py);
        dict.set_item("text", &self.text)?;
        dict.set_item("text_angle", self.text_angle)?;
        
        let lines_list: Vec<_> = self.lines.iter().map(|line| {
            let line_dict = PyDict::new_bound(py);
            line_dict.set_item("text", &line.text).unwrap();
            line_dict.set_item("bounds", line.bounds.to_dict(py).unwrap()).unwrap();
            
            let words_list: Vec<_> = line.words.iter().map(|word| {
                let word_dict = PyDict::new_bound(py);
                word_dict.set_item("text", &word.text).unwrap();
                word_dict.set_item("bounds", word.bounds.to_dict(py).unwrap()).unwrap();
                word_dict
            }).collect();
            line_dict.set_item("words", words_list).unwrap();
            line_dict
        }).collect();
        
        dict.set_item("lines", lines_list)?;
        Ok(dict.into())
    }
}

/// 将内部结果转换为 Python 结果
fn convert_result(result: crate::OcrRecognitionResult) -> PyOcrResult {
    let lines = result.lines.into_iter().map(|line| {
        let words = line.words.into_iter().map(|word| {
            PyOcrWord {
                text: word.text,
                bounds: PyBoundingBox {
                    x: word.bounds.x,
                    y: word.bounds.y,
                    width: word.bounds.width,
                    height: word.bounds.height,
                },
            }
        }).collect();
        
        PyOcrLine {
            text: line.text,
            bounds: PyBoundingBox {
                x: line.bounds.x,
                y: line.bounds.y,
                width: line.bounds.width,
                height: line.bounds.height,
            },
            words,
        }
    }).collect();
    
    PyOcrResult {
        text: result.text,
        lines,
        text_angle: result.text_angle,
    }
}

/// 从图片文件路径识别文字
/// 
/// Args:
///     image_path: 图片文件路径
///     language: 语言代码，如 "zh-Hans-CN", "en-US"，默认使用系统语言
/// 
/// Returns:
///     OcrResult 对象，包含识别结果
#[pyfunction]
#[pyo3(signature = (image_path, language=None))]
pub fn recognize_from_file(image_path: &str, language: Option<&str>) -> PyResult<PyOcrResult> {
    crate::recognize_from_file(image_path, language)
        .map(convert_result)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
}

/// 从字节数据识别文字
/// 
/// Args:
///     image_data: 图片字节数据 (bytes)
///     language: 语言代码，如 "zh-Hans-CN", "en-US"，默认使用系统语言
/// 
/// Returns:
///     OcrResult 对象，包含识别结果
#[pyfunction]
#[pyo3(signature = (image_data, language=None))]
pub fn recognize_from_bytes(image_data: &[u8], language: Option<&str>) -> PyResult<PyOcrResult> {
    crate::recognize_from_bytes(image_data, language)
        .map(convert_result)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
}

/// 获取系统支持的 OCR 语言列表
#[pyfunction]
pub fn get_available_languages() -> PyResult<Vec<String>> {
    crate::get_available_languages()
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyRuntimeError, _>(e))
}

/// windows_media_ocr - Windows OCR Python 库
/// 
/// 使用 Windows.Media.Ocr API 进行文字识别
#[pymodule]
pub fn windows_media_ocr(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyBoundingBox>()?;
    m.add_class::<PyOcrWord>()?;
    m.add_class::<PyOcrLine>()?;
    m.add_class::<PyOcrResult>()?;
    m.add_function(wrap_pyfunction!(recognize_from_file, m)?)?;
    m.add_function(wrap_pyfunction!(recognize_from_bytes, m)?)?;
    m.add_function(wrap_pyfunction!(get_available_languages, m)?)?;
    Ok(())
}
