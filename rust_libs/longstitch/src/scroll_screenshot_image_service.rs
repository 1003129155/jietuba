use image::DynamicImage;
use std::collections::VecDeque;

use crate::scroll_screenshot_service::ScrollImageList;

pub struct ScrollScreenshotImage {
    pub image: DynamicImage,
    pub direction: ScrollImageList,
}

/**
 * 将截图和处理截图分开处理
 * 通过短时间内多次截图来提高滚动截图的响应速度和可靠性
 */
pub struct ScrollScreenshotImageService {
    image_queue: VecDeque<ScrollScreenshotImage>,
}

impl ScrollScreenshotImageService {
    pub fn new() -> Self {
        Self {
            image_queue: VecDeque::new(),
        }
    }

    /**
     * 将截图添加到待处理队列尾部
     */
    pub fn push_image(&mut self, image: DynamicImage, direction: ScrollImageList) {
        self.image_queue
            .push_back(ScrollScreenshotImage { image, direction });
    }

    /**
     * 从队列头开始获取连续相同direction的图片，遇到不同direction则终止
     */
    pub fn pop_image(&mut self) -> Option<ScrollScreenshotImage> {
        return self.image_queue.pop_front();
    }

    pub fn has_image(&self) -> bool {
        !self.image_queue.is_empty()
    }

    pub fn image_count(&self) -> usize {
        self.image_queue.len()
    }

    pub fn clear(&mut self) {
        self.image_queue.clear();
    }
}
