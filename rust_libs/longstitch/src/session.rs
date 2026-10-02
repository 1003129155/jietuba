//! 增量拼接会话：逐帧推入截图，会话内只保留拼接结果的像素和逐行哈希。
//!
//! 匹配规则与 `stitch` 模块的双图拼接完全相同（共用 `search_range` / `match_overlap`），
//! 区别是每帧只处理新帧本身：已拼好的长图不再编解码，行哈希也只算新帧。
//! 输入输出用 BGRA（Windows 截屏的原生布局），内部按 RGBA 存放以复用行哈希。

use rayon::prelude::*;

use crate::error::StitchError;
use crate::hash::compute_row_hashes_raw;
use crate::stitch::{match_overlap, search_range};

/// 一帧推入后的结论：长图先截到 `keep` 行，再接上新帧从 `skip` 行开始的部分。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PushOutcome {
    pub keep: usize,
    pub skip: usize,
    /// 自动判向判为反向：会话里的长图已整体翻转，之后的帧须由调用方翻转后推入。
    pub reversed: bool,
}

pub struct StitchSession {
    width: u32,
    frame_height: u32,
    ignore_right_pixels: u32,
    ignore_top_pixels: u32,
    min_overlap_ratio: f32,
    /// 拼接结果按推入顺序分段存放的整行像素（RGBA）。不用一整块缓冲：
    /// 整块追加会按倍数预留容量、扩容时新旧两块并存，长截图的内存峰值会成倍放大。
    segments: Vec<Vec<u8>>,
    hashes: Vec<u64>,
}

impl StitchSession {
    pub fn new(
        width: u32,
        frame_height: u32,
        ignore_right_pixels: u32,
        ignore_top_pixels: u32,
        min_overlap_ratio: f32,
    ) -> Result<Self, StitchError> {
        if width == 0 || frame_height == 0 {
            return Err(StitchError::InvalidFrame(format!(
                "frame size must be positive, got {width}x{frame_height}"
            )));
        }
        Ok(Self {
            width,
            frame_height,
            ignore_right_pixels,
            ignore_top_pixels,
            min_overlap_ratio,
            segments: Vec::new(),
            hashes: Vec::new(),
        })
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    /// 当前拼接结果的行数，尚未推入任何帧时为 0。
    pub fn height(&self) -> usize {
        self.hashes.len()
    }

    fn stride(&self) -> usize {
        self.width as usize * 4
    }

    /// 推入一帧 BGRA 截图。第一帧只作为起点；无重叠时返回 `NoOverlap`，会话保持不变。
    ///
    /// `detect_direction` 对应 `stitch_two_images_smart_auto`：先试正向，正向失败或缩短
    /// 再试整体翻转后的反向，取舍规则与之相同。
    pub fn push_bgra(
        &mut self,
        bgra: &[u8],
        detect_direction: bool,
        ignore_img1_top_ratio: f32,
        ignore_img1_bottom_ratio: f32,
        debug: bool,
    ) -> Result<PushOutcome, StitchError> {
        let frame = self.bgra_to_rgba(bgra)?;
        let frame_hashes =
            compute_row_hashes_raw(&frame, self.width, 0, self.frame_height, self.ignore_right_pixels);

        if self.hashes.is_empty() {
            self.segments = vec![frame];
            self.hashes = frame_hashes;
            return Ok(PushOutcome { keep: 0, skip: 0, reversed: false });
        }

        if !detect_direction {
            let (keep, skip) = self.locate(
                &self.hashes,
                &frame_hashes,
                ignore_img1_top_ratio,
                ignore_img1_bottom_ratio,
                debug,
            )?;
            self.apply(keep, skip, &frame, &frame_hashes);
            return Ok(PushOutcome { keep, skip, reversed: false });
        }

        // 方向未知，不做 img1 顶/底忽略
        let img1_h = self.height();
        let h2 = self.frame_height as usize;
        let forward = self.locate(&self.hashes, &frame_hashes, 0.0, 0.0, debug);
        let forward_h = forward.as_ref().ok().map(|&(keep, skip)| keep + h2 - skip);
        if let (Ok((keep, skip)), Some(h)) = (&forward, forward_h) {
            if h >= img1_h {
                let (keep, skip) = (*keep, *skip);
                self.apply(keep, skip, &frame, &frame_hashes);
                return Ok(PushOutcome { keep, skip, reversed: false });
            }
        }

        let reversed_hashes: Vec<u64> = self.hashes.iter().rev().copied().collect();
        let reversed_frame_hashes: Vec<u64> = frame_hashes.iter().rev().copied().collect();
        let reverse = self.locate(&reversed_hashes, &reversed_frame_hashes, 0.0, 0.0, debug);

        let use_reverse = match (&forward, &reverse) {
            (_, Ok((keep, skip))) => {
                let reverse_h = keep + h2 - skip;
                reverse_h >= img1_h || forward_h.map_or(true, |f| reverse_h > f)
            }
            (Ok(_), Err(_)) => false,
            (Err(e1), Err(e2)) => {
                return Err(if matches!(e1, StitchError::NoOverlap) { e2.clone() } else { e1.clone() });
            }
        };

        if use_reverse {
            let (keep, skip) = reverse.expect("reverse is Ok when chosen");
            let stride = self.stride();
            self.segments.reverse();
            for segment in &mut self.segments {
                flip_rows(segment, stride);
            }
            self.hashes = reversed_hashes;
            let mut flipped = frame;
            flip_rows(&mut flipped, stride);
            self.apply(keep, skip, &flipped, &reversed_frame_hashes);
            Ok(PushOutcome { keep, skip, reversed: true })
        } else {
            let (keep, skip) = forward.expect("forward is Ok when reverse is not chosen");
            self.apply(keep, skip, &frame, &frame_hashes);
            Ok(PushOutcome { keep, skip, reversed: false })
        }
    }

    /// 当前拼接结果的 BGRA 字节数。
    pub fn byte_len(&self) -> usize {
        self.hashes.len() * self.stride()
    }

    /// 把当前拼接结果按 BGRA 写进 `out`，其长度须为 `byte_len()`。
    pub fn export_into(&self, out: &mut [u8]) {
        assert_eq!(out.len(), self.byte_len(), "export buffer size mismatch");
        let mut offset = 0;
        for segment in &self.segments {
            out[offset..offset + segment.len()]
                .par_chunks_exact_mut(4)
                .zip(segment.par_chunks_exact(4))
                .for_each(|(dst, src)| {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = src[3];
                });
            offset += segment.len();
        }
    }

    /// 当前拼接结果，BGRA。
    pub fn export_bgra(&self) -> Vec<u8> {
        let mut out = vec![0u8; self.byte_len()];
        self.export_into(&mut out);
        out
    }

    /// 立即释放拼接结果占用的内存，会话回到未推入任何帧的状态。
    pub fn clear(&mut self) {
        self.segments = Vec::new();
        self.hashes = Vec::new();
    }

    /// 截图的 alpha 无意义，统一置为不透明，与经 PIL 转 RGB 再编码的旧路径一致。
    fn bgra_to_rgba(&self, bgra: &[u8]) -> Result<Vec<u8>, StitchError> {
        let expected = self.stride() * self.frame_height as usize;
        if bgra.len() != expected {
            return Err(StitchError::InvalidFrame(format!(
                "expected {expected} bytes for a {}x{} BGRA frame, got {}",
                self.width,
                self.frame_height,
                bgra.len()
            )));
        }
        let mut out = vec![0u8; expected];
        out.par_chunks_exact_mut(4)
            .zip(bgra.par_chunks_exact(4))
            .for_each(|(dst, src)| {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = 255;
            });
        Ok(out)
    }

    /// 返回 (长图保留行数, 新帧跳过行数)。
    fn locate(
        &self,
        img1_hashes: &[u64],
        frame_hashes: &[u64],
        ignore_img1_top_ratio: f32,
        ignore_img1_bottom_ratio: f32,
        debug: bool,
    ) -> Result<(usize, usize), StitchError> {
        let hash_start = self.ignore_top_pixels.min(self.frame_height) as usize;
        let img2_hashes = &frame_hashes[hash_start..];
        let img1_len = img1_hashes.len();
        let (start, end) = search_range(
            img1_len,
            img2_hashes.len(),
            self.frame_height as usize,
            ignore_img1_top_ratio,
            ignore_img1_bottom_ratio,
        );
        let (start_i, start_j, len) = match_overlap(
            &img1_hashes[start..end],
            start,
            img1_len,
            img2_hashes,
            hash_start,
            self.min_overlap_ratio,
            ignore_img1_top_ratio,
            ignore_img1_bottom_ratio,
            debug,
        )?;
        Ok((start_i as usize + len, start_j as usize + len))
    }

    fn apply(&mut self, keep: usize, skip: usize, frame: &[u8], frame_hashes: &[u64]) {
        let stride = self.stride();
        let mut rows = self.hashes.len();
        while let Some(last) = self.segments.last_mut() {
            let last_rows = last.len() / stride;
            if rows - last_rows >= keep {
                rows -= last_rows;
                self.segments.pop();
            } else {
                last.truncate((keep - (rows - last_rows)) * stride);
                break;
            }
        }
        self.hashes.truncate(keep);
        if skip < frame_hashes.len() {
            self.segments.push(frame[skip * stride..].to_vec());
            self.hashes.extend_from_slice(&frame_hashes[skip..]);
        }
    }
}

fn flip_rows(pixels: &mut [u8], stride: usize) {
    let rows = pixels.len() / stride;
    for y in 0..rows / 2 {
        let (top, bottom) = pixels.split_at_mut((rows - 1 - y) * stride);
        top[y * stride..(y + 1) * stride].swap_with_slice(&mut bottom[..stride]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stitch::{stitch_two_images_smart, stitch_two_images_smart_auto};
    use image::{DynamicImage, Rgba, RgbaImage};
    use std::io::Cursor;

    const W: u32 = 120;
    const H: u32 = 90;

    fn mix(a: u32, b: u32) -> u32 {
        let mut h = a.wrapping_mul(0x9E37_79B1) ^ b.wrapping_mul(0x85EB_CA77);
        h ^= h >> 15;
        h = h.wrapping_mul(0xC2B2_AE3D);
        h ^ (h >> 13)
    }

    /// 白底上分布着文字块的长页面，段落之间留白，便于产生重复的行哈希。
    fn page(height: u32) -> RgbaImage {
        RgbaImage::from_fn(W, height, |x, y| {
            let line = y / 6;
            let ink = line % 7 < 5 && mix(line, 1) % 4 != 0 && y % 6 < 4 && mix(line * 8 + y % 6, x / 3) % 3 == 0;
            if ink { Rgba([20, 40, 60, 255]) } else { Rgba([250, 250, 250, 255]) }
        })
    }

    /// 每行颜色都不同的页面，方向判定不会被重复图案干扰。
    fn distinct_rows_page(height: u32) -> RgbaImage {
        RgbaImage::from_fn(W, height, |x, y| {
            let shade = |k: u32| ((y * k + x / 40) % 256) as u8;
            Rgba([shade(7), shade(13), shade(29), 255])
        })
    }

    fn frame(page: &RgbaImage, top: u32) -> RgbaImage {
        image::imageops::crop_imm(page, 0, top, W, H).to_image()
    }

    fn to_bgra(img: &RgbaImage) -> Vec<u8> {
        img.as_raw().chunks_exact(4).flat_map(|p| [p[2], p[1], p[0], p[3]]).collect()
    }

    fn png(img: &RgbaImage) -> Vec<u8> {
        let mut out = Vec::new();
        DynamicImage::ImageRgba8(img.clone())
            .write_to(&mut Cursor::new(&mut out), image::ImageOutputFormat::Png)
            .unwrap();
        out
    }

    fn decode(bytes: &[u8]) -> RgbaImage {
        image::load_from_memory(bytes).unwrap().to_rgba8()
    }

    fn session() -> StitchSession {
        StitchSession::new(W, H, 20, 0, 0.01).unwrap()
    }

    fn exported(s: &StitchSession) -> RgbaImage {
        let rgba: Vec<u8> = s.export_bgra().chunks_exact(4).flat_map(|p| [p[2], p[1], p[0], p[3]]).collect();
        RgbaImage::from_raw(W, s.height() as u32, rgba).unwrap()
    }

    /// 旧路径：每帧都把已拼长图整张交给双图拼接。
    fn stitch_by_pairs(frames: &[RgbaImage], top_ratio: f32) -> RgbaImage {
        let mut result = frames[0].clone();
        for f in &frames[1..] {
            if let Ok(bytes) = stitch_two_images_smart(&png(&result), &png(f), 20, 0, 0.01, top_ratio, 0.0) {
                result = decode(&bytes);
            }
        }
        result
    }

    #[test]
    fn matches_pairwise_stitching_frame_by_frame() {
        let p = page(900);
        let tops = [0, 37, 81, 81, 150, 204, 260, 311, 390, 455, 520, 600, 690, 760, 810];
        let frames: Vec<_> = tops.iter().map(|&t| frame(&p, t)).collect();

        let mut s = session();
        for f in &frames {
            let _ = s.push_bgra(&to_bgra(f), false, 0.15, 0.0, false);
        }
        assert_eq!(exported(&s), stitch_by_pairs(&frames, 0.15));
    }

    #[test]
    fn matches_pairwise_stitching_across_a_rollback() {
        let p = distinct_rows_page(900);
        let tops = [0, 60, 120, 180, 240, 300, 240, 180, 240, 300, 360, 420, 480, 540, 600];
        let frames: Vec<_> = tops.iter().map(|&t| frame(&p, t)).collect();

        let mut s = session();
        let mut shrank = false;
        for f in &frames {
            let before = s.height();
            if s.push_bgra(&to_bgra(f), false, 0.15, 0.0, false).is_ok() && s.height() < before {
                shrank = true;
            }
        }
        assert!(shrank);
        assert_eq!(exported(&s), stitch_by_pairs(&frames, 0.15));
    }

    #[test]
    fn matches_pairwise_stitching_with_a_fixed_footer() {
        // 每帧底部同一条底栏：长图末尾的旧底栏要被截掉，走分段截断
        let p = page(900);
        let frames: Vec<_> = (0..12)
            .map(|k| {
                let mut f = frame(&p, k * 60);
                for y in H - 12..H {
                    for x in 0..W {
                        f.put_pixel(x, y, if x % 17 < 9 { Rgba([30, 60, 140, 255]) } else { Rgba([220, 220, 220, 255]) });
                    }
                }
                f
            })
            .collect();

        let mut s = session();
        for f in &frames {
            let _ = s.push_bgra(&to_bgra(f), false, 0.15, 0.0, false);
        }
        let expected = stitch_by_pairs(&frames, 0.15);
        assert!(expected.height() > H * 3);
        assert_eq!(exported(&s), expected);
    }

    #[test]
    fn upward_capture_keeps_every_row_however_long() {
        // 帧由调用方翻转后推入；重叠 60 行，长图一旦超过 1200 行，按总长算的 5% 就会盖住它
        let p = distinct_rows_page(2400);
        let flip = |img: &RgbaImage| image::imageops::flip_vertical(img);
        let tops: Vec<u32> = (0..=77).rev().map(|k| k * 30).collect();

        let mut s = session();
        for &top in &tops {
            s.push_bgra(&to_bgra(&flip(&frame(&p, top))), false, 0.0, 0.05, false).unwrap();
        }
        assert_eq!(flip(&exported(&s)), image::imageops::crop_imm(&p, 0, 0, W, 2400).to_image());
    }

    #[test]
    fn first_frame_is_kept_as_is() {
        let p = page(300);
        let f = frame(&p, 0);
        let mut s = session();
        let outcome = s.push_bgra(&to_bgra(&f), false, 0.15, 0.0, false).unwrap();
        assert_eq!(outcome, PushOutcome { keep: 0, skip: 0, reversed: false });
        assert_eq!(exported(&s), f);
    }

    #[test]
    fn no_overlap_leaves_session_unchanged() {
        let p = page(900);
        let mut s = session();
        s.push_bgra(&to_bgra(&frame(&p, 0)), false, 0.15, 0.0, false).unwrap();
        let before = exported(&s);
        let noise = RgbaImage::from_fn(W, H, |x, y| {
            let v = (mix(x, y) & 0xFF) as u8;
            Rgba([v, v ^ 0x5A, v ^ 0xA5, 255])
        });
        assert_eq!(s.push_bgra(&to_bgra(&noise), false, 0.15, 0.0, false), Err(StitchError::NoOverlap));
        assert_eq!(exported(&s), before);
    }

    #[test]
    fn detects_reverse_scrolling_like_the_auto_pair() {
        let p = distinct_rows_page(600);
        let (lower, upper) = (frame(&p, 300), frame(&p, 250));

        let mut s = session();
        s.push_bgra(&to_bgra(&lower), false, 0.0, 0.0, false).unwrap();
        let outcome = s.push_bgra(&to_bgra(&upper), true, 0.0, 0.0, false).unwrap();

        let (bytes, direction) = stitch_two_images_smart_auto(&png(&lower), &png(&upper), 20, 0, 0.01).unwrap();
        assert!(outcome.reversed);
        assert_eq!(direction, "reverse");
        assert_eq!(exported(&s), decode(&bytes));
    }

    #[test]
    fn detects_forward_scrolling_like_the_auto_pair() {
        let p = distinct_rows_page(600);
        let (a, b) = (frame(&p, 100), frame(&p, 160));

        let mut s = session();
        s.push_bgra(&to_bgra(&a), false, 0.0, 0.0, false).unwrap();
        let outcome = s.push_bgra(&to_bgra(&b), true, 0.0, 0.0, false).unwrap();

        let (bytes, direction) = stitch_two_images_smart_auto(&png(&a), &png(&b), 20, 0, 0.01).unwrap();
        assert!(!outcome.reversed);
        assert_eq!(direction, "forward");
        assert_eq!(exported(&s), decode(&bytes));
    }

    #[test]
    fn rejects_frames_of_the_wrong_size() {
        let mut s = session();
        let err = s.push_bgra(&[0u8; 16], false, 0.0, 0.0, false).unwrap_err();
        assert!(matches!(err, StitchError::InvalidFrame(_)));
        assert_eq!(s.height(), 0);
    }

    #[test]
    fn alpha_is_forced_opaque() {
        let p = page(300);
        let mut bgra = to_bgra(&frame(&p, 0));
        bgra.chunks_exact_mut(4).for_each(|px| px[3] = 0);
        let mut s = session();
        s.push_bgra(&bgra, false, 0.0, 0.0, false).unwrap();
        assert!(s.export_bgra().chunks_exact(4).all(|px| px[3] == 255));
    }

    #[test]
    fn clear_releases_the_result() {
        let p = page(300);
        let mut s = session();
        s.push_bgra(&to_bgra(&frame(&p, 0)), false, 0.0, 0.0, false).unwrap();
        s.clear();
        assert_eq!(s.height(), 0);
        assert!(s.export_bgra().is_empty());
    }
}
