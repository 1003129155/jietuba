pub mod scroll_screenshot_capture_service;
pub mod scroll_screenshot_image_service;
pub mod scroll_screenshot_service;

use pyo3::prelude::*;
use pyo3::types::PyBytes;
use scroll_screenshot_service::{ScrollDirection, ScrollImageList, ScrollScreenshotService};
use image::{DynamicImage, ImageFormat};
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
    fn add_image(&mut self, image_bytes: &[u8], direction: u8) -> PyResult<(Option<i32>, bool, u8)> {
        // 从字节数据加载图片
        let img = image::load_from_memory(image_bytes)
            .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(format!("无法加载图片: {}", e)))?;

        let scroll_list = if direction == 0 {
            ScrollImageList::Top
        } else {
            ScrollImageList::Bottom
        };

        let (overlap_result, is_rollback, result_list) = 
            self.service.handle_image(img, scroll_list);

        let overlap_size = overlap_result.map(|(size, _)| size);
        let result_dir = if result_list == ScrollImageList::Top { 0 } else { 1 };

        Ok((overlap_size, is_rollback, result_dir))
    }

    /// 导出最终合成的长截图
    /// 
    /// 返回:
    ///   PNG 格式的图片字节数据
    fn export(&mut self, py: Python) -> PyResult<Option<PyObject>> {
        let result = self.service.export();

        match result {
            Some(img) => {
                // 将图片编码为 PNG 字节
                let mut buffer = Cursor::new(Vec::new());
                img.write_to(&mut buffer, ImageFormat::Png)
                    .map_err(|e| PyErr::new::<pyo3::exceptions::PyValueError, _>(format!("无法编码图片: {}", e)))?;

                let bytes = buffer.into_inner();
                Ok(Some(PyBytes::new(py, &bytes).into()))
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

/// Python 模块定义
#[pymodule]
fn jietuba_rust(_py: Python, m: &PyModule) -> PyResult<()> {
    m.add_class::<PyScrollScreenshotService>()?;
    Ok(())
}
