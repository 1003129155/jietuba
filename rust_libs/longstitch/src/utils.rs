// 简化版的辅助函数，用于替代 snow_shot_app_utils
use image::DynamicImage;

/// 将图片覆盖到目标缓冲区
pub fn overlay_image(
    target: &mut Vec<u8>,
    target_width: usize,
    source: &DynamicImage,
    offset_x: usize,
    offset_y: usize,
    channel_count: usize,
) {
    let source_rgba = source.to_rgba8();
    let (source_width, source_height) = source_rgba.dimensions();

    for y in 0..source_height as usize {
        for x in 0..source_width as usize {
            let target_x = offset_x + x;
            let target_y = offset_y + y;

            if target_x < target_width {
                let source_pixel = source_rgba.get_pixel(x as u32, y as u32);
                let target_index = (target_y * target_width + target_x) * channel_count;

                if target_index + channel_count <= target.len() {
                    target[target_index] = source_pixel[0];
                    target[target_index + 1] = source_pixel[1];
                    target[target_index + 2] = source_pixel[2];
                    if channel_count == 4 {
                        target[target_index + 3] = source_pixel[3];
                    }
                }
            }
        }
    }
}
