/// 行哈希模块 - 长截图拼接专用
use rayon::prelude::*;

// ========== 行哈希（长截图拼接专用）==========

/// 从 PNG/JPEG 字节计算逐行哈希
pub fn compute_row_hashes(
    image_bytes: &[u8],
    ignore_right_pixels: u32,
) -> Result<Vec<u64>, String> {
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;
    let rgba_img = img.to_rgba8();
    Ok(compute_row_hashes_from_rgba(&rgba_img, ignore_right_pixels, false))
}

/// 带调试输出的版本
pub fn compute_row_hashes_debug(
    image_bytes: &[u8],
    ignore_right_pixels: u32,
) -> Result<Vec<u64>, String> {
    let img =
        image::load_from_memory(image_bytes).map_err(|e| format!("Failed to load image: {}", e))?;
    let rgba_img = img.to_rgba8();
    Ok(compute_row_hashes_from_rgba(&rgba_img, ignore_right_pixels, true))
}

/// 直接从 RgbaImage 计算行哈希（零拷贝）
pub fn compute_row_hashes_from_rgba(
    rgba_img: &image::RgbaImage,
    ignore_right_pixels: u32,
    debug: bool,
) -> Vec<u64> {
    let width = rgba_img.width();
    let height = rgba_img.height();

    let effective_width = if ignore_right_pixels > 0 && width > ignore_right_pixels {
        width - ignore_right_pixels
    } else {
        width
    };

    let raw = rgba_img.as_raw();
    let stride = (width * 4) as usize;

    let row_hashes: Vec<u64> = (0..height)
        .into_par_iter()
        .map(|y| {
            let mut r_sum: u64 = 0;
            let mut g_sum: u64 = 0;
            let mut b_sum: u64 = 0;
            let pixel_count = effective_width as u64;

            let row_start = y as usize * stride;
            let row_data = &raw[row_start..row_start + (effective_width as usize) * 4];
            for chunk in row_data.chunks_exact(4) {
                r_sum += chunk[0] as u64;
                g_sum += chunk[1] as u64;
                b_sum += chunk[2] as u64;
            }

            if pixel_count > 0 {
                let r_mean = ((r_sum / pixel_count) / 8) * 8;
                let g_mean = ((g_sum / pixel_count) / 8) * 8;
                let b_mean = ((b_sum / pixel_count) / 8) * 8;

                r_mean
                    .wrapping_mul(73856093)
                    .wrapping_add(g_mean.wrapping_mul(19349663))
                    .wrapping_add(b_mean.wrapping_mul(83492791))
            } else {
                0
            }
        })
        .collect();

    if debug {
        println!("  📊 样本哈希值（每100行）:");
        for y in (0..height).step_by(100).take(3) {
            let mut r_sum: u64 = 0;
            let mut g_sum: u64 = 0;
            let mut b_sum: u64 = 0;

            let row_start = y as usize * stride;
            let row_data = &raw[row_start..row_start + (effective_width as usize) * 4];
            for chunk in row_data.chunks_exact(4) {
                r_sum += chunk[0] as u64;
                g_sum += chunk[1] as u64;
                b_sum += chunk[2] as u64;
            }

            let pixel_count = effective_width as u64;
            if pixel_count > 0 {
                let r_mean = ((r_sum / pixel_count) / 8) * 8;
                let g_mean = ((g_sum / pixel_count) / 8) * 8;
                let b_mean = ((b_sum / pixel_count) / 8) * 8;
                let hash = row_hashes[y as usize];

                println!(
                    "     行{}: RGB({},{},{}) -> hash={}",
                    y, r_mean, g_mean, b_mean, hash as i64
                );
            }
        }
    }

    row_hashes
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgba, RgbaImage};

    #[test]
    fn test_row_hashes() {
        let img = RgbaImage::from_fn(100, 50, |_x, y| {
            Rgba([(y * 5) as u8, (y * 5) as u8, (y * 5) as u8, 255])
        });

        let mut bytes = Vec::new();
        img.write_to(
            &mut std::io::Cursor::new(&mut bytes),
            image::ImageFormat::Png,
        )
        .unwrap();

        let hashes = compute_row_hashes(&bytes, 0).unwrap();
        assert_eq!(hashes.len(), 50);
    }
}
