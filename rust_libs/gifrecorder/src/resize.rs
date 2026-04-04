//! 图像 resize — 基于 fast_image_resize
//!
//! 提供 RGB24 → RGB24 的高质量缩放。

use fast_image_resize as fir;

/// RGB24 resize
///
/// 输入: rgb 原始数据, src_w×src_h
/// 输出: dst_w×dst_h 的 RGB24 数据
pub fn resize_rgb(
    rgb: &[u8],
    src_w: u32,
    src_h: u32,
    dst_w: u32,
    dst_h: u32,
) -> Result<Vec<u8>, String> {
    if src_w == dst_w && src_h == dst_h {
        return Ok(rgb.to_vec());
    }

    let src_image = fir::images::Image::from_vec_u8(
        src_w,
        src_h,
        rgb.to_vec(),
        fir::PixelType::U8x3,
    )
    .map_err(|e| format!("src image: {e}"))?;

    let mut dst_image = fir::images::Image::new(dst_w, dst_h, fir::PixelType::U8x3);

    let mut resizer = fir::Resizer::new();
    resizer
        .resize(
            &src_image,
            &mut dst_image,
            Some(&fir::ResizeOptions::new().resize_alg(fir::ResizeAlg::Convolution(
                fir::FilterType::Lanczos3,
            ))),
        )
        .map_err(|e| format!("resize: {e}"))?;

    Ok(dst_image.into_vec())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resize_basic() {
        let w = 64u32;
        let h = 48u32;
        let rgb = vec![128u8; (w * h * 3) as usize];

        let result = resize_rgb(&rgb, w, h, 32, 24).unwrap();
        assert_eq!(result.len(), (32 * 24 * 3) as usize);
    }

    #[test]
    fn resize_noop() {
        let rgb = vec![42u8; 64 * 48 * 3];
        let result = resize_rgb(&rgb, 64, 48, 64, 48).unwrap();
        assert_eq!(result, rgb);
    }
}
