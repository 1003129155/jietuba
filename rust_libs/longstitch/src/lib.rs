// 注释掉依赖外部库的模块
// pub mod scroll_screenshot_capture_service;
pub mod image_hash;
pub mod scroll_screenshot_image_service;
pub mod scroll_screenshot_service;
pub mod utils;

use image::ImageFormat;
use pyo3::prelude::*;
use pyo3::types::PyBytes;
use scroll_screenshot_service::{ScrollDirection, ScrollImageList, ScrollScreenshotService};
use std::io::Cursor;

/// Python 包装的滚动截图服务
#[pyclass]
struct PyScrollScreenshotService {
    service: ScrollScreenshotService,
}

#[pymethods]
impl PyScrollScreenshotService {
    #[new]
    fn new() -> Self {
        Self {
            service: ScrollScreenshotService::new(),
        }
    }

    /// 初始化服务
    ///
    /// 参数:
    ///   direction: 0=垂直滚动, 1=水平滚动
    ///   sample_rate: 采样率 (0.0-1.0)
    ///   min_sample_size: 最小采样尺寸
    ///   max_sample_size: 最大采样尺寸
    ///   corner_threshold: 特征点阈值 (默认64)
    ///   descriptor_patch_size: 描述符块大小 (默认9)
    ///   min_size_delta: 最小变化量 (默认64)
    ///   try_rollback: 是否尝试回滚 (默认false)
    ///   distance_threshold: 特征匹配距离阈值 (默认0.1)
    ///   ef_search: HNSW搜索参数 (默认32)
    fn init(
        &mut self,
        direction: u8,
        sample_rate: f32,
        min_sample_size: u32,
        max_sample_size: u32,
        corner_threshold: u8,
        descriptor_patch_size: usize,
        min_size_delta: i32,
        try_rollback: bool,
        distance_threshold: f32,
        ef_search: usize,
    ) {
        let dir = if direction == 0 {
            ScrollDirection::Vertical
        } else {
            ScrollDirection::Horizontal
        };

        self.service.init(
            dir,
            sample_rate,
            min_sample_size,
            max_sample_size,
            corner_threshold,
            descriptor_patch_size,
            min_size_delta,
            try_rollback,
            distance_threshold,
            ef_search,
        );
    }

    /// 添加一张图片
    ///
    /// 参数:
    ///   image_bytes: 图片的字节数据 (支持 PNG, JPEG 等格式)
    ///   direction: 0=上/左图片列表, 1=下/右图片列表
    ///
    /// 返回:
    ///   元组 (overlap_size, is_rollback, result_direction)
    ///   overlap_size: 重叠尺寸 (None 表示未找到重叠)
    ///   is_rollback: 是否需要回滚
    ///   result_direction: 结果方向
    fn add_image(
        &mut self,
        image_bytes: &[u8],
        direction: u8,
    ) -> PyResult<(Option<i32>, bool, u8)> {
        // 从字节数据加载图片
        let img = image::load_from_memory(image_bytes).map_err(|e| {
            PyErr::new::<pyo3::exceptions::PyValueError, _>(format!("无法加载图片: {}", e))
        })?;

        let scroll_list = if direction == 0 {
            ScrollImageList::Top
        } else {
            ScrollImageList::Bottom
        };

        let (overlap_result, is_rollback, result_list) =
            self.service.handle_image(img, scroll_list);

        let overlap_size = overlap_result.map(|(size, _)| size);
        let result_dir = if result_list == ScrollImageList::Top {
            0
        } else {
            1
        };

        Ok((overlap_size, is_rollback, result_dir))
    }

    /// 导出最终合成的长截图
    ///
    /// 返回:
    ///   PNG 格式的图片字节数据
    fn export<'py>(&mut self, py: Python<'py>) -> PyResult<Option<Bound<'py, PyBytes>>> {
        let result = self.service.export();

        match result {
            Some(img) => {
                // 将图片编码为 PNG 字节
                let mut buffer = Cursor::new(Vec::new());
                img.write_to(&mut buffer, ImageFormat::Png).map_err(|e| {
                    PyErr::new::<pyo3::exceptions::PyValueError, _>(format!("无法编码图片: {}", e))
                })?;

                let bytes = buffer.into_inner();
                Ok(Some(PyBytes::new_bound(py, &bytes)))
            }
            None => Ok(None),
        }
    }

    /// 清除所有数据
    fn clear(&mut self) {
        self.service.clear();
    }

    /// 获取当前图片数量
    fn get_image_count(&self) -> (usize, usize) {
        (
            self.service.top_image_list.len(),
            self.service.bottom_image_list.len(),
        )
    }
}

// ========== 图像哈希函数 ==========

/// 计算差值哈希 (dHash)
#[pyfunction]
#[pyo3(signature = (image_bytes, hash_size=None))]
fn compute_dhash(image_bytes: Vec<u8>, hash_size: Option<usize>) -> PyResult<u64> {
    let size = hash_size.unwrap_or(8);
    image_hash::compute_dhash(&image_bytes, size)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(e))
}

/// 计算平均哈希 (aHash)
#[pyfunction]
#[pyo3(signature = (image_bytes, hash_size=None))]
fn compute_ahash(image_bytes: Vec<u8>, hash_size: Option<usize>) -> PyResult<u64> {
    let size = hash_size.unwrap_or(8);
    image_hash::compute_ahash(&image_bytes, size)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(e))
}

/// 计算感知哈希 (pHash)
#[pyfunction]
#[pyo3(signature = (image_bytes, hash_size=None))]
fn compute_phash(image_bytes: Vec<u8>, hash_size: Option<usize>) -> PyResult<u64> {
    let size = hash_size.unwrap_or(8);
    image_hash::compute_phash(&image_bytes, size)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(e))
}

/// 批量计算哈希（并行处理）
#[pyfunction]
#[pyo3(signature = (image_bytes_list, method, hash_size=None))]
fn batch_compute_hash(
    image_bytes_list: Vec<Vec<u8>>,
    method: String,
    hash_size: Option<usize>,
) -> PyResult<Vec<u64>> {
    let size = hash_size.unwrap_or(8);
    let results = image_hash::batch_compute_hash(&image_bytes_list, &method, size);
    let hashes: Vec<u64> = results.into_iter().map(|r| r.unwrap_or(0)).collect();
    Ok(hashes)
}

/// 计算汉明距离
#[pyfunction]
fn hamming_distance(hash1: u64, hash2: u64) -> u32 {
    image_hash::hamming_distance(hash1, hash2)
}

/// 计算哈希相似度
#[pyfunction]
#[pyo3(signature = (hash1, hash2, hash_size=None))]
fn hash_similarity(hash1: u64, hash2: u64, hash_size: Option<usize>) -> f64 {
    let size = hash_size.unwrap_or(8);
    image_hash::hash_similarity(hash1, hash2, size)
}

/// 计算逐行哈希（用于长截图拼接）
#[pyfunction]
#[pyo3(signature = (image_bytes, ignore_right_pixels=None))]
fn compute_row_hashes(
    image_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
) -> PyResult<Vec<u64>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    image_hash::compute_row_hashes(&image_bytes, ignore)
        .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(e))
}

/// 找到最长公共子串（用于图像拼接）
#[pyfunction]
#[pyo3(signature = (seq1, seq2, min_ratio=None))]
fn find_longest_common_substring(
    seq1: Vec<u64>,
    seq2: Vec<u64>,
    min_ratio: Option<f32>,
) -> (i32, i32, usize) {
    let ratio = min_ratio.unwrap_or(0.1);
    image_hash::find_longest_common_substring(&seq1, &seq2, ratio)
}

/// 完整的双图拼接函数（零拷贝高性能）
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<Bound<'py, PyBytes>>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.1);

    match image_hash::stitch_two_images(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok(result_bytes) => Ok(Some(PyBytes::new_bound(py, &result_bytes))),
        Err(e) => {
            eprintln!("⚠️  Rust 拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// 带调试输出的双图拼接函数
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust_debug<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<Bound<'py, PyBytes>>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.1);

    println!("\n======================================================================");
    println!("🦀 Rust 拼接接口（调试模式）");
    println!("======================================================================");

    match image_hash::stitch_two_images_debug(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok(result_bytes) => {
            println!("✅ Rust 拼接完成");
            Ok(Some(PyBytes::new_bound(py, &result_bytes)))
        }
        Err(e) => {
            eprintln!("⚠️  Rust 拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// 智能双图拼接函数（带多候选纠错机制）
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust_smart<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<Bound<'py, PyBytes>>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.01);

    match image_hash::stitch_two_images_smart(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok(result_bytes) => Ok(Some(PyBytes::new_bound(py, &result_bytes))),
        Err(e) => {
            eprintln!("⚠️  Rust 智能拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// 带调试输出的智能双图拼接函数
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust_smart_debug<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<Bound<'py, PyBytes>>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.01);

    println!("\n======================================================================");
    println!("🧠 Rust 智能拼接接口（多候选纠错 + 调试模式）");
    println!("======================================================================");

    match image_hash::stitch_two_images_smart_debug(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok(result_bytes) => {
            println!("✅ Rust 智能拼接完成");
            Ok(Some(PyBytes::new_bound(py, &result_bytes)))
        }
        Err(e) => {
            eprintln!("⚠️  Rust 智能拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// Python 模块定义
#[pymodule]
fn longstitch(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<PyScrollScreenshotService>()?;
    m.add_function(wrap_pyfunction!(compute_dhash, m)?)?;
    m.add_function(wrap_pyfunction!(compute_ahash, m)?)?;
    m.add_function(wrap_pyfunction!(compute_phash, m)?)?;
    m.add_function(wrap_pyfunction!(batch_compute_hash, m)?)?;
    m.add_function(wrap_pyfunction!(hamming_distance, m)?)?;
    m.add_function(wrap_pyfunction!(hash_similarity, m)?)?;
    m.add_function(wrap_pyfunction!(compute_row_hashes, m)?)?;
    m.add_function(wrap_pyfunction!(find_longest_common_substring, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_debug, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart_debug, m)?)?;
    Ok(())
}
