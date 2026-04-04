pub mod hash;
pub mod lcs;
pub mod stitch;

use pyo3::prelude::*;
use pyo3::types::PyBytes;

// ========== 拼接函数 ==========

/// 智能双图拼接（多候选纠错）
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

    match stitch::stitch_two_images_smart(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok(result_bytes) => Ok(Some(PyBytes::new_bound(py, &result_bytes))),
        Err(e) => {
            eprintln!("⚠️  Rust 智能拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// 智能双图拼接（调试模式）
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

    match stitch::stitch_two_images_smart_debug(&img1_bytes, &img2_bytes, ignore, ratio) {
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

/// 智能拼接 + 自动方向检测
/// 返回 (png_bytes, direction_str)，direction_str: "forward" 或 "reverse"
/// "reverse" 时返回翻转态结果，调用方负责最终输出时翻转还原
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust_smart_auto<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<(Bound<'py, PyBytes>, String)>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.01);

    match stitch::stitch_two_images_smart_auto(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok((result_bytes, direction)) => {
            Ok(Some((PyBytes::new_bound(py, &result_bytes), direction)))
        }
        Err(e) => {
            eprintln!("⚠️  Rust 自动方向拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// 自动方向检测（调试模式）
#[pyfunction]
#[pyo3(signature = (img1_bytes, img2_bytes, ignore_right_pixels=None, min_overlap_ratio=None))]
fn stitch_two_images_rust_smart_auto_debug<'py>(
    py: Python<'py>,
    img1_bytes: Vec<u8>,
    img2_bytes: Vec<u8>,
    ignore_right_pixels: Option<u32>,
    min_overlap_ratio: Option<f32>,
) -> PyResult<Option<(Bound<'py, PyBytes>, String)>> {
    let ignore = ignore_right_pixels.unwrap_or(20);
    let ratio = min_overlap_ratio.unwrap_or(0.01);

    println!("\n======================================================================");
    println!("🧭 Rust 自动方向检测拼接（调试模式）");
    println!("======================================================================");

    match stitch::stitch_two_images_smart_auto_debug(&img1_bytes, &img2_bytes, ignore, ratio) {
        Ok((result_bytes, direction)) => {
            println!("✅ 自动方向拼接完成，方向: {}", direction);
            Ok(Some((PyBytes::new_bound(py, &result_bytes), direction)))
        }
        Err(e) => {
            eprintln!("⚠️  Rust 自动方向拼接失败: {}", e);
            Ok(None)
        }
    }
}

/// Python 模块定义
#[pymodule]
fn longstitch(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart_debug, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart_auto, m)?)?;
    m.add_function(wrap_pyfunction!(stitch_two_images_rust_smart_auto_debug, m)?)?;
    Ok(())
}
