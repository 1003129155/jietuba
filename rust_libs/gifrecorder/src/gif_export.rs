//! GIF 导出 — 流水线: 小批量并行解码 → 缩放 → 光标叠加 → 帧差分编码
//!
//! 使用 `gif` crate 直接编码 + 固定全局调色板 + 帧差分优化。
//! 每次只并行解码一小批帧（BATCH_SIZE），编码后立即释放，
//! 避免一次性解码全部帧导致内存尖峰。
//! 支持鼠标光标 sprite 叠加、进度回调 & 取消。
//!
//! 帧差分：对比当前帧与上一帧的 RGBA 像素，只编码变化区域，
//! 未变化像素标记为透明，大幅减少量化 + LZW 的工作量。
//! 所有帧共享一个全局 palette，用于消除逐帧独立调色板造成的颜色抖动。

use std::fs::File;
use std::io::BufWriter;
use std::sync::Arc;

use color_quant::NeuQuant;
use gif::{DisposalMethod, Encoder as GifLowEncoder, Frame as GifFrame, Repeat as GifRepeat};
use rayon::prelude::*;

use crate::frame_store::FrameStore;
use crate::jpeg;
use crate::resize::resize_rgb;

/// 每批并行解码的帧数。
/// 8 帧 × ~0.7 MB/帧(640×360 RGB) ≈ 6 MB，内存友好且保留并行收益。
const BATCH_SIZE: usize = 8;
const GLOBAL_PALETTE_MAX_SAMPLES: usize = 12;
const GLOBAL_PALETTE_PIXEL_STEP: usize = 4;
const GLOBAL_PALETTE_SPEED: i32 = 10;
const GLOBAL_PALETTE_COLORS: usize = 255;
const TRANSPARENT_INDEX: u8 = 0;

struct GlobalPalette {
    palette_rgb: Vec<u8>,
    quantizer: NeuQuant,
}

/// GIF 导出选项
pub struct GifExportOptions {
    /// 输出路径
    pub path: String,
    /// 输出宽度 (0=原始)
    pub width: u32,
    /// 输出高度 (0=原始)
    pub height: u32,
    /// GIF 重复次数: 0=无限循环
    pub repeat: u16,
    /// 起始帧索引（含，0 表示从头）
    pub frame_start: usize,
    /// 结束帧索引（含，0 表示到最后一帧）
    pub frame_end: usize,
    /// 鼠标光标 sprite 集合（None = 不叠加光标）
    pub cursor_sprites: Option<CursorSprites>,
    /// 每帧的光标参数（None = 不叠加，长度 = 帧数）
    pub cursor_infos: Option<Vec<Option<CursorInfo>>>,
    /// 导出速度倍率（1.0 = 原速，2.0 = 2倍速，0.5 = 半速）
    pub speed_multiplier: f32,
}

/// 单张 sprite（RGBA32，尺寸 w×h）
pub struct Sprite {
    pub data: Vec<u8>,
    pub w: u32,
    pub h: u32,
}

/// 所有光标 sprite 集合
pub struct CursorSprites {
    pub cursor: Sprite,
    pub burst_left: [Sprite; 3],   // 左键烟花 frame 1/2/3
    pub burst_right: [Sprite; 3],  // 右键烟花 frame 1/2/3
    pub scroll_up: Sprite,
    pub scroll_down: Sprite,
}

/// 单帧的光标信息（已缩放到 GIF 坐标系）
pub struct CursorInfo {
    pub x: i32,           // 光标左上角 X
    pub y: i32,           // 光标左上角 Y
    pub press: u8,        // 0=无, 1=左键, 2=右键
    pub scroll: i8,       // 0=无, 1=上, -1=下
    pub burst_frame: u8,  // 0=无, 1/2/3=烟花帧
    pub burst_side: u8,   // 0=无, 1=左, 2=右
}

/// 进度回调: (current_frame, total_frames) → bool
///
/// 返回 false 表示取消
pub type ProgressCallback = Box<dyn Fn(usize, usize) -> bool + Send>;

/// 导出 GIF
///
/// 流水线步骤 (按 BATCH_SIZE 分批):
/// 1. 并行: clone_jpeg → decode JPEG → resize → RGB→RGBA → cursor blend
/// 2. 顺序: GIF encode (逐帧写入文件)
///
/// 内存峰值 ≈ BATCH_SIZE × 单帧 RGBA，远小于全量解码。
pub fn export_gif(
    store: &Arc<FrameStore>,
    opts: &GifExportOptions,
    progress: Option<ProgressCallback>,
) -> Result<(), String> {
    let total_n = store.frame_count();
    if total_n == 0 {
        return Err("no frames to export".into());
    }

    // 计算实际导出的帧范围 [start, end]（均含）
    let start = opts.frame_start.min(total_n - 1);
    let end = if opts.frame_end == 0 || opts.frame_end >= total_n {
        total_n - 1
    } else {
        opts.frame_end.min(total_n - 1)
    };
    let end = end.max(start);
    let n = end - start + 1; // 实际要导出的帧数

    let src_w = store.width();
    let src_h = store.height();
    let dst_w = if opts.width > 0 { opts.width } else { src_w };
    let dst_h = if opts.height > 0 { opts.height } else { src_h };
    let need_resize = dst_w != src_w || dst_h != src_h;

    // 重置取消标志
    store.set_cancel(false);

    // ── 预计算每帧延迟 (需要全部时间戳，但只是 u32 很小) ──
    let timestamps = store.frame_timestamps();
    if timestamps.len() <= end {
        return Err(format!(
            "timestamps count ({}) < required ({})",
            timestamps.len(),
            end + 1
        ));
    }

    let global_palette = build_global_palette(
        store,
        opts,
        start,
        end,
        dst_w,
        dst_h,
        need_resize,
    )?;

    // ── gif crate 编码器（固定全局调色板 + 帧差分） ──
    let file = File::create(&opts.path).map_err(|e| format!("create file: {e}"))?;
    let writer = BufWriter::with_capacity(256 * 1024, file); // 256KB 写缓冲
    let mut encoder = GifLowEncoder::new(
        writer,
        dst_w as u16,
        dst_h as u16,
        &global_palette.palette_rgb,
    )
        .map_err(|e| format!("create encoder: {e}"))?;
    let repeat = if opts.repeat == 0 {
        GifRepeat::Infinite
    } else {
        GifRepeat::Finite(opts.repeat)
    };
    encoder.set_repeat(repeat).map_err(|e| format!("set repeat: {e}"))?;

    // ── 分批处理（仅处理 start..=end 范围内的帧） ──
    let mut prev_rgba: Option<Vec<u8>> = None;
    let mut encoded = 0usize;

    for batch_start in (0..n).step_by(BATCH_SIZE) {
        if store.is_cancelled() {
            return Err("cancelled".into());
        }

        let batch_end = (batch_start + BATCH_SIZE).min(n);
        // 将批次内偏移量转换为真实帧索引
        let indices: Vec<usize> = (batch_start..batch_end).map(|i| start + i).collect();

        // 并行: clone_jpeg → decode → resize → RGBA → cursor blend
        // sprites/infos 通过 Arc 共享，满足 par_iter 的 Send 要求
        let batch_rgba: Vec<Result<(Vec<u8>, usize), String>> = indices
            .par_iter()
            .map(|&idx| {
                render_frame_rgba(
                    store,
                    idx,
                    dst_w,
                    dst_h,
                    need_resize,
                    opts.cursor_sprites.as_ref(),
                    opts.cursor_infos.as_deref(),
                )
                .map(|rgba| (rgba, idx))
            })
            .collect();

        // 顺序: 帧差分 + GIF 编码（按原始顺序）
        for r in batch_rgba {
            let (rgba, idx) = match r {
                Ok(f) => f,
                Err(e) if e == "cancelled" => return Err("cancelled".into()),
                Err(e) => return Err(e),
            };

            if store.is_cancelled() {
                return Err("cancelled".into());
            }

            // 计算帧延迟（使用原始帧时间戳，idx 是真实帧索引）
            let delay_ms = if idx < end {
                timestamps[idx + 1].saturating_sub(timestamps[idx])
            } else if n > 1 {
                let total_dur = timestamps[end].saturating_sub(timestamps[start]);
                total_dur / (n as u32 - 1)
            } else {
                100
            };
            // 应用速度倍率（2x → 延迟减半；0.5x → 延迟翻倍）
            let speed = opts.speed_multiplier.max(0.1);
            let delay_ms = ((delay_ms as f32 / speed).round() as u32).max(1);
            // GIF delay 单位是厘秒 (1/100s)，最小 2 (20ms)
            let delay_cs = ((delay_ms + 5) / 10).max(2) as u16;

            if let Some(ref prev) = prev_rgba {
                // ── 帧差分编码 ──
                if let Some((dx, dy, dw, dh)) = find_dirty_rect(prev, &rgba, dst_w, dst_h) {
                    // 提取脏区域，未变化像素标记为透明
                    let dirty = extract_dirty_rgba(prev, &rgba, dst_w, dx, dy, dw, dh);
                    let indexed = rgba_to_palette_indices(&dirty, &global_palette.quantizer);
                    let mut frame = GifFrame::from_indexed_pixels(
                        dw as u16, dh as u16, indexed, Some(TRANSPARENT_INDEX),
                    );
                    frame.left = dx as u16;
                    frame.top = dy as u16;
                    frame.delay = delay_cs;
                    frame.dispose = DisposalMethod::Keep;

                    encoder.write_frame(&frame)
                        .map_err(|e| format!("encode frame {idx}: {e}"))?;
                } else {
                    // 完全相同帧：写 1×1 透明帧仅保留延迟
                    let mut frame = GifFrame::from_indexed_pixels(
                        1,
                        1,
                        vec![TRANSPARENT_INDEX],
                        Some(TRANSPARENT_INDEX),
                    );
                    frame.delay = delay_cs;
                    frame.dispose = DisposalMethod::Keep;

                    encoder.write_frame(&frame)
                        .map_err(|e| format!("encode frame {idx}: {e}"))?;
                }
            } else {
                // ── 首帧: 全帧编码 ──
                let indexed = rgba_to_palette_indices(&rgba, &global_palette.quantizer);
                let mut frame = GifFrame::from_indexed_pixels(
                    dst_w as u16, dst_h as u16, indexed, Some(TRANSPARENT_INDEX),
                );
                frame.delay = delay_cs;
                frame.dispose = DisposalMethod::Keep;

                encoder.write_frame(&frame)
                    .map_err(|e| format!("encode frame {idx}: {e}"))?;
            }

            prev_rgba = Some(rgba);
            encoded += 1;

            if let Some(ref cb) = progress {
                if !cb(encoded, n) {
                    store.set_cancel(true);
                    return Err("cancelled".into());
                }
            }
        }
        // 本批次内存在此作用域结束后释放
    }

    Ok(())
}

fn render_frame_rgba(
    store: &Arc<FrameStore>,
    idx: usize,
    dst_w: u32,
    dst_h: u32,
    need_resize: bool,
    cursor_sprites: Option<&CursorSprites>,
    cursor_infos: Option<&[Option<CursorInfo>]>,
) -> Result<Vec<u8>, String> {
    if store.is_cancelled() {
        return Err("cancelled".into());
    }

    let (jpeg_data, _elapsed) = store
        .clone_jpeg(idx)
        .ok_or_else(|| format!("frame {idx} missing"))?;

    let d = jpeg::decode_to_rgb(&jpeg_data)?;
    let rgb = if need_resize {
        resize_rgb(&d.rgb, d.width, d.height, dst_w, dst_h)?
    } else {
        d.rgb
    };

    let mut rgba = rgb_to_rgba(&rgb, dst_w, dst_h);

    if let (Some(sprites), Some(infos)) = (cursor_sprites, cursor_infos) {
        if let Some(Some(ci)) = infos.get(idx) {
            if ci.burst_frame >= 1 && ci.burst_frame <= 3 {
                let f = (ci.burst_frame - 1) as usize;
                let burst = if ci.burst_side == 1 {
                    &sprites.burst_left[f]
                } else {
                    &sprites.burst_right[f]
                };
                let bx = ci.x - (burst.w as i32) / 2 + 6;
                let by = ci.y - (burst.h as i32) / 2 + 6;
                alpha_blend_sprite(&mut rgba, dst_w, dst_h, &burst.data, burst.w, burst.h, bx, by);
            }
            if ci.scroll != 0 {
                let scroll_sp = if ci.scroll > 0 {
                    &sprites.scroll_up
                } else {
                    &sprites.scroll_down
                };
                let sx = ci.x + sprites.cursor.w as i32 + 10;
                let sy = ci.y + sprites.cursor.h as i32 / 2 - scroll_sp.h as i32 / 2;
                alpha_blend_sprite(&mut rgba, dst_w, dst_h, &scroll_sp.data, scroll_sp.w, scroll_sp.h, sx, sy);
            }
            alpha_blend_sprite(
                &mut rgba,
                dst_w,
                dst_h,
                &sprites.cursor.data,
                sprites.cursor.w,
                sprites.cursor.h,
                ci.x,
                ci.y,
            );
        }
    }

    Ok(rgba)
}

fn build_global_palette(
    store: &Arc<FrameStore>,
    opts: &GifExportOptions,
    start: usize,
    end: usize,
    dst_w: u32,
    dst_h: u32,
    need_resize: bool,
) -> Result<GlobalPalette, String> {
    let sample_indices = choose_palette_sample_indices(start, end, GLOBAL_PALETTE_MAX_SAMPLES);
    let mut sampled_rgba = Vec::new();

    for idx in sample_indices {
        let rgba = render_frame_rgba(
            store,
            idx,
            dst_w,
            dst_h,
            need_resize,
            opts.cursor_sprites.as_ref(),
            opts.cursor_infos.as_deref(),
        )?;
        append_sampled_pixels(&mut sampled_rgba, &rgba, GLOBAL_PALETTE_PIXEL_STEP);
    }

    if sampled_rgba.is_empty() {
        return Err("failed to build global palette sample".into());
    }

    let quantizer = NeuQuant::new(
        GLOBAL_PALETTE_SPEED,
        GLOBAL_PALETTE_COLORS,
        &sampled_rgba,
    );
    let mut palette_rgb = Vec::with_capacity(256 * 3);
    palette_rgb.extend_from_slice(&[0, 0, 0]);
    palette_rgb.extend_from_slice(&quantizer.color_map_rgb());

    Ok(GlobalPalette {
        palette_rgb,
        quantizer,
    })
}

fn choose_palette_sample_indices(start: usize, end: usize, max_samples: usize) -> Vec<usize> {
    let total = end - start + 1;
    if total <= max_samples {
        return (start..=end).collect();
    }

    let mut indices = Vec::with_capacity(max_samples);
    for i in 0..max_samples {
        let offset = i * (total - 1) / (max_samples - 1);
        let idx = start + offset;
        if indices.last().copied() != Some(idx) {
            indices.push(idx);
        }
    }
    indices
}

fn append_sampled_pixels(dst: &mut Vec<u8>, rgba: &[u8], pixel_step: usize) {
    for pixel in rgba.chunks_exact(4).step_by(pixel_step) {
        dst.extend_from_slice(pixel);
    }
}

fn rgba_to_palette_indices(rgba: &[u8], quantizer: &NeuQuant) -> Vec<u8> {
    let mut indexed = Vec::with_capacity(rgba.len() / 4);
    for pixel in rgba.chunks_exact(4) {
        if pixel[3] == 0 {
            indexed.push(TRANSPARENT_INDEX);
        } else {
            indexed.push((quantizer.index_of(pixel) + 1) as u8);
        }
    }
    indexed
}

// ═══════════════════════════════════════════════
//  帧差分辅助函数
// ═══════════════════════════════════════════════

/// 找出两帧之间变化像素的最小包围矩形。
///
/// 返回 `(x, y, w, h)`；如果两帧完全相同返回 `None`。
/// 优化：先按行比较整行字节切片，快速跳过不变行。
fn find_dirty_rect(prev: &[u8], curr: &[u8], w: u32, h: u32) -> Option<(u32, u32, u32, u32)> {
    let mut min_x = w;
    let mut min_y = h;
    let mut max_x = 0u32;
    let mut max_y = 0u32;
    let stride = (w * 4) as usize;

    for y in 0..h {
        let row_start = (y as usize) * stride;
        let row_end = row_start + stride;
        // 快速跳过完全相同的行
        if prev[row_start..row_end] == curr[row_start..row_end] {
            continue;
        }
        min_y = min_y.min(y);
        max_y = y;
        let mut left = 0u32;
        while left < w {
            let off = row_start + (left as usize) * 4;
            if prev[off] != curr[off]
                || prev[off + 1] != curr[off + 1]
                || prev[off + 2] != curr[off + 2]
            {
                break;
            }
            left += 1;
        }

        let mut right = w - 1;
        while right > left {
            let off = row_start + (right as usize) * 4;
            if prev[off] != curr[off]
                || prev[off + 1] != curr[off + 1]
                || prev[off + 2] != curr[off + 2]
            {
                break;
            }
            right -= 1;
        }

        min_x = min_x.min(left);
        max_x = max_x.max(right);
    }

    if max_x < min_x {
        return None;
    }
    Some((min_x, min_y, max_x - min_x + 1, max_y - min_y + 1))
}

/// 提取脏区域的 RGBA 像素。
///
/// 未变化像素设 alpha=0（透明），`GifFrame::from_rgba_speed` 会自动
/// 将其映射到 transparent index，使 LZW 压缩更高效。
fn extract_dirty_rgba(
    prev: &[u8],
    curr: &[u8],
    full_w: u32,
    rx: u32, ry: u32, rw: u32, rh: u32,
) -> Vec<u8> {
    let mut result = vec![0u8; (rw * rh * 4) as usize];
    let stride = (full_w * 4) as usize;

    for dy in 0..rh {
        let y = ry + dy;
        let row_base = (y as usize) * stride;
        for dx in 0..rw {
            let x = rx + dx;
            let off = row_base + (x as usize) * 4;
            if prev[off] != curr[off]
                || prev[off + 1] != curr[off + 1]
                || prev[off + 2] != curr[off + 2]
            {
                let out_off = ((dy * rw + dx) * 4) as usize;
                result[out_off] = curr[off];
                result[out_off + 1] = curr[off + 1];
                result[out_off + 2] = curr[off + 2];
                result[out_off + 3] = curr[off + 3];
            }
        }
    }
    result
}

// ═══════════════════════════════════════════════
//  像素格式转换 & 混合
// ═══════════════════════════════════════════════

/// RGB24 → RGBA32 (alpha = 255)
fn rgb_to_rgba(rgb: &[u8], w: u32, h: u32) -> Vec<u8> {
    let pixel_count = (w * h) as usize;
    let mut rgba = vec![0u8; pixel_count * 4];
    for i in 0..pixel_count {
        let rgb_off = i * 3;
        let rgba_off = i * 4;
        rgba[rgba_off] = rgb[rgb_off];
        rgba[rgba_off + 1] = rgb[rgb_off + 1];
        rgba[rgba_off + 2] = rgb[rgb_off + 2];
        rgba[rgba_off + 3] = 255;
    }
    rgba
}

/// 将 sprite (RGBA32, sw×sh) 叠加到 dst (RGBA32, dw×dh) 的 (ox, oy) 位置。
///
/// - 自动裁剪超出 dst 边界的部分
/// - 跳过 alpha=0 像素
/// - 就地修改 dst，零额外分配
fn alpha_blend_sprite(
    dst: &mut [u8], dw: u32, dh: u32,
    src: &[u8], sw: u32, sh: u32,
    ox: i32, oy: i32,
) {
    let dw = dw as i32;
    let dh = dh as i32;
    let sw = sw as i32;
    let sh = sh as i32;

    // 裁剪到 dst 有效区域
    let x0 = ox.max(0);
    let y0 = oy.max(0);
    let x1 = (ox + sw).min(dw);
    let y1 = (oy + sh).min(dh);
    if x0 >= x1 || y0 >= y1 {
        return;
    }

    for dy in y0..y1 {
        let sy = dy - oy;
        for dx in x0..x1 {
            let sx = dx - ox;
            let s_off = ((sy * sw + sx) * 4) as usize;
            let d_off = ((dy * dw + dx) * 4) as usize;

            let sa = src[s_off + 3] as u32;
            if sa == 0 {
                continue;
            }
            if sa == 255 {
                dst[d_off]     = src[s_off];
                dst[d_off + 1] = src[s_off + 1];
                dst[d_off + 2] = src[s_off + 2];
                dst[d_off + 3] = 255;
            } else {
                let da = 255 - sa;
                dst[d_off]     = ((src[s_off]     as u32 * sa + dst[d_off]     as u32 * da) / 255) as u8;
                dst[d_off + 1] = ((src[s_off + 1] as u32 * sa + dst[d_off + 1] as u32 * da) / 255) as u8;
                dst[d_off + 2] = ((src[s_off + 2] as u32 * sa + dst[d_off + 2] as u32 * da) / 255) as u8;
                dst[d_off + 3] = 255;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_rgb_to_rgba() {
        let rgb = vec![10, 20, 30, 40, 50, 60];
        let rgba = rgb_to_rgba(&rgb, 2, 1);
        assert_eq!(rgba, vec![10, 20, 30, 255, 40, 50, 60, 255]);
    }

    #[test]
    fn test_blend_sprite_opaque() {
        // 2×2 dst (黑色), 1×1 sprite (红色不透明) 放在 (1,0)
        let mut dst = vec![0,0,0,255, 0,0,0,255, 0,0,0,255, 0,0,0,255];
        let src = vec![255,0,0,255];
        alpha_blend_sprite(&mut dst, 2, 2, &src, 1, 1, 1, 0);
        // 只有 (1,0) 变红
        assert_eq!(&dst[4..8], &[255,0,0,255]);
        assert_eq!(&dst[0..4], &[0,0,0,255]);
    }

    #[test]
    fn test_blend_sprite_clip() {
        // sprite 超出 dst 右边界，应裁剪
        let mut dst = vec![0,0,0,255, 0,0,0,255]; // 2×1
        let src = vec![255,0,0,255, 0,255,0,255];  // 2×1
        alpha_blend_sprite(&mut dst, 2, 1, &src, 2, 1, 1, 0);
        // 只有 (1,0) 被绘制 (红色)，(0,255,0) 被裁掉
        assert_eq!(&dst[0..4], &[0,0,0,255]);
        assert_eq!(&dst[4..8], &[255,0,0,255]);
    }

    #[test]
    fn test_blend_sprite_half_alpha() {
        let mut dst = vec![0,0,0,255]; // 1×1 黑色
        let src = vec![255,255,255,128]; // 白色半透明
        alpha_blend_sprite(&mut dst, 1, 1, &src, 1, 1, 0, 0);
        assert_eq!(dst[0], 128); // (255*128)/255 ≈ 128
        assert_eq!(dst[3], 255);
    }

    #[test]
    fn test_blend_sprite_negative_offset() {
        // sprite 部分超出 dst 左边界
        let mut dst = vec![0,0,0,255, 0,0,0,255]; // 2×1
        let src = vec![255,0,0,255, 0,255,0,255];  // 2×1
        alpha_blend_sprite(&mut dst, 2, 1, &src, 2, 1, -1, 0);
        // sprite[0] 在 dst(-1,0) 被裁掉，sprite[1] 在 dst(0,0)
        assert_eq!(&dst[0..4], &[0,255,0,255]);
        assert_eq!(&dst[4..8], &[0,0,0,255]);
    }
}
