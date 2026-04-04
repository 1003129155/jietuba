//! JPEG 编解码封装 — 纯 Rust 实现
//!
//! 编码: jpeg-encoder (纯 Rust)
//! 解码: image crate (内部使用 zune-jpeg, 带 SIMD 优化)
//!
//! 无需 NASM / libjpeg-turbo 等外部依赖。

use jpeg_encoder::{ColorType as EncColorType, Encoder};
use std::io::Cursor;

/// JPEG 编码：RGB24 → JPEG bytes
pub fn encode_rgb(
    rgb: &[u8],
    width: u32,
    height: u32,
    quality: i32,
) -> Result<Vec<u8>, String> {
    let q = quality.clamp(1, 100) as u8;
    let mut buf = Vec::with_capacity(rgb.len() / 4); // 预估

    let encoder = Encoder::new(&mut buf, q);
    encoder
        .encode(rgb, width as u16, height as u16, EncColorType::Rgb)
        .map_err(|e| format!("jpeg encode: {e}"))?;

    Ok(buf)
}

/// JPEG 编码：BGRA32 → JPEG bytes (直接从 mss 截屏格式压缩)
///
/// 先将 BGRA 转换为 RGB，再编码。
/// 转换开销在 640×480 约 0.1ms，可忽略。
pub fn encode_bgra(
    bgra: &[u8],
    width: u32,
    height: u32,
    quality: i32,
) -> Result<Vec<u8>, String> {
    let pixel_count = (width * height) as usize;
    let expected = pixel_count * 4;
    if bgra.len() != expected {
        return Err(format!(
            "BGRA size mismatch: got {} expected {}",
            bgra.len(),
            expected
        ));
    }

    // BGRA → RGB
    let mut rgb = Vec::with_capacity(pixel_count * 3);
    for i in 0..pixel_count {
        let offset = i * 4;
        rgb.push(bgra[offset + 2]); // R (from BGRA[2])
        rgb.push(bgra[offset + 1]); // G (from BGRA[1])
        rgb.push(bgra[offset]);     // B (from BGRA[0])
    }

    encode_rgb(&rgb, width, height, quality)
}

/// JPEG 解码结果
pub struct DecodedFrame {
    pub rgb: Vec<u8>,
    pub width: u32,
    pub height: u32,
}

/// JPEG 解码：JPEG bytes → RGB24
pub fn decode_to_rgb(jpeg: &[u8]) -> Result<DecodedFrame, String> {
    let cursor = Cursor::new(jpeg);
    let img = image::ImageReader::new(cursor)
        .with_guessed_format()
        .map_err(|e| format!("jpeg format: {e}"))?
        .decode()
        .map_err(|e| format!("jpeg decode: {e}"))?;

    let rgb_img = img.to_rgb8();
    let width = rgb_img.width();
    let height = rgb_img.height();
    let rgb = rgb_img.into_raw();

    Ok(DecodedFrame { rgb, width, height })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_rgb() {
        let w = 64u32;
        let h = 48u32;
        // 生成渐变测试图
        let mut rgb = vec![0u8; (w * h * 3) as usize];
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 3) as usize;
                rgb[i] = (x * 4) as u8;
                rgb[i + 1] = (y * 5) as u8;
                rgb[i + 2] = 128;
            }
        }

        let jpeg = encode_rgb(&rgb, w, h, 95).unwrap();
        assert!(!jpeg.is_empty());
        assert!(jpeg.len() < rgb.len()); // JPEG 应该更小

        let decoded = decode_to_rgb(&jpeg).unwrap();
        assert_eq!(decoded.width, w);
        assert_eq!(decoded.height, h);
        assert_eq!(decoded.rgb.len(), rgb.len());
    }

    #[test]
    fn roundtrip_bgra() {
        let w = 64u32;
        let h = 48u32;
        let mut bgra = vec![0u8; (w * h * 4) as usize];
        for y in 0..h {
            for x in 0..w {
                let i = ((y * w + x) * 4) as usize;
                bgra[i] = 200;     // B
                bgra[i + 1] = 100; // G
                bgra[i + 2] = 50;  // R
                bgra[i + 3] = 255; // A
            }
        }

        let jpeg = encode_bgra(&bgra, w, h, 90).unwrap();
        assert!(!jpeg.is_empty());

        let decoded = decode_to_rgb(&jpeg).unwrap();
        assert_eq!(decoded.width, w);
        assert_eq!(decoded.height, h);
    }
}
