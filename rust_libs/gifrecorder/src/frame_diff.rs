//! GIF 帧间差分 — 以画布上已显示的调色板色为基准，容差内的像素沿用旧值
//!
//! 有损帧源（JPEG）在文字与边缘处的细小色差逐帧不同，按源像素严格相等判断会把它们
//! 全算作变化，帧差分与 LZW 都因此失效。这里改为与画布当前显示的颜色比较：
//! 差异不超过容差时保持透明，显示色始终与源相差不超过容差，误差不会累积。
//! 源像素几乎处处在变（视频、渐变）时，零散的透明像素只会截断 LZW 的重复串，
//! 此时整块区域按最近色不透明写出。
//! 量化与比较按行并行，不依赖 `crate` 内其他模块。

use color_quant::NeuQuant;
use rayon::prelude::*;

/// 调色板第 0 项保留给透明；`NeuQuant` 的下标要整体 +1。
pub const TRANSPARENT_INDEX: u8 = 0;

/// 差分参数
#[derive(Clone, Copy, Debug)]
pub struct DiffParams {
    /// 与显示色的最大通道差不超过该值视为未变化；0 表示严格比较
    pub tolerance: i32,
    /// 左右相邻像素的变化量一致（整片偏移，如悬停高亮）时改用的容差。
    /// JPEG 噪点逐像素起伏，细微但成片的真实变化不能被当作噪点吞掉。
    pub coherent_tolerance: i32,
    /// 源像素发生变化的数量达到变化区域面积的该比例时，整个区域不透明写出
    pub dense_ratio: f32,
}

/// 需要写入的变化区域
pub struct Region {
    pub left: u32,
    pub top: u32,
    pub width: u32,
    pub height: u32,
    /// 区域内逐像素的调色板下标，未变化处为 [`TRANSPARENT_INDEX`]
    pub indices: Vec<u8>,
}

/// 已写出的画面状态
pub struct Canvas {
    width: usize,
    height: usize,
    /// 画布上每个像素当前显示的调色板下标
    shown: Vec<u8>,
    /// 上一帧源像素的最近调色板下标，源像素未变时直接沿用，省去重复量化
    nearest: Vec<u8>,
    /// 当前帧每个像素要写出的下标（0 = 透明）
    scratch: Vec<u8>,
}

/// 一行的比较结果
struct RowDiff {
    /// 需要写出的列范围
    span: Option<(usize, usize)>,
    /// 源像素与上一帧不同的个数
    source_changed: usize,
}

impl Canvas {
    pub fn new(width: usize, height: usize) -> Self {
        let n = width * height;
        Self {
            width,
            height,
            shown: vec![TRANSPARENT_INDEX; n],
            nearest: vec![TRANSPARENT_INDEX; n],
            scratch: vec![TRANSPARENT_INDEX; n],
        }
    }

    /// 写入首帧：整帧量化，返回全帧下标
    pub fn start(&mut self, rgba: &[u8], quantizer: &NeuQuant) -> Vec<u8> {
        debug_assert_eq!(rgba.len(), self.width * self.height * 4);
        self.nearest
            .par_chunks_mut(self.width)
            .zip(rgba.par_chunks(self.width * 4))
            .for_each(|(near, row)| {
                for (n, px) in near.iter_mut().zip(row.chunks_exact(4)) {
                    *n = (quantizer.index_of(px) + 1) as u8;
                }
            });
        self.shown.copy_from_slice(&self.nearest);
        self.nearest.clone()
    }

    /// 以 `cur` 更新画布，返回需要写出的变化区域；整帧无变化返回 `None`。
    /// `prev` 必须是上一次传入 `start`/`advance` 的源帧。
    pub fn advance(
        &mut self,
        prev: &[u8],
        cur: &[u8],
        palette_rgb: &[u8],
        quantizer: &NeuQuant,
        params: DiffParams,
    ) -> Option<Region> {
        let w = self.width;
        debug_assert_eq!(prev.len(), w * self.height * 4);
        debug_assert_eq!(cur.len(), prev.len());

        let ctx = RowContext {
            cur,
            width: w,
            height: self.height,
            palette_rgb,
            quantizer,
            params,
        };
        let rows: Vec<RowDiff> = self
            .scratch
            .par_chunks_mut(w)
            .zip(self.nearest.par_chunks_mut(w))
            .zip(self.shown.par_chunks(w))
            .zip(prev.par_chunks(w * 4))
            .enumerate()
            .map(|(y, (((out, near), shown), prev_row))| diff_row(&ctx, y, out, near, shown, prev_row))
            .collect();

        let top = rows.iter().position(|r| r.span.is_some())?;
        let bottom = rows.iter().rposition(|r| r.span.is_some())?;
        let (left, right) = rows
            .iter()
            .filter_map(|r| r.span)
            .fold((usize::MAX, 0), |(l, r), (a, b)| (l.min(a), r.max(b)));
        let (rw, rh) = (right - left + 1, bottom - top + 1);

        let source_changed: usize = rows.iter().map(|r| r.source_changed).sum();
        let dense = source_changed as f32 >= params.dense_ratio * (rw * rh) as f32;
        let source = if dense { &self.nearest } else { &self.scratch };

        let mut indices = Vec::with_capacity(rw * rh);
        for y in top..=bottom {
            let start = y * w + left;
            indices.extend_from_slice(&source[start..start + rw]);
        }

        self.shown
            .par_chunks_mut(w)
            .zip(source.par_chunks(w))
            .for_each(|(shown, written)| {
                for (s, &o) in shown.iter_mut().zip(written) {
                    if o != TRANSPARENT_INDEX {
                        *s = o;
                    }
                }
            });

        Some(Region {
            left: left as u32,
            top: top as u32,
            width: rw as u32,
            height: rh as u32,
            indices,
        })
    }
}

/// 逐行处理共用的当前帧与量化信息
struct RowContext<'a> {
    cur: &'a [u8],
    width: usize,
    height: usize,
    palette_rgb: &'a [u8],
    quantizer: &'a NeuQuant,
    params: DiffParams,
}

/// 处理第 `y` 行
fn diff_row(
    ctx: &RowContext,
    y: usize,
    out: &mut [u8],
    near: &mut [u8],
    shown: &[u8],
    prev_row: &[u8],
) -> RowDiff {
    let cur_row = &ctx.cur[y * ctx.width * 4..(y + 1) * ctx.width * 4];
    let mut first = usize::MAX;
    let mut last = 0;
    let mut source_changed = 0;

    for x in 0..out.len() {
        let o = x * 4;
        let cur = &cur_row[o..o + 4];
        out[x] = TRANSPARENT_INDEX;
        if prev_row[o..o + 3] == cur[..3] {
            continue;
        }
        source_changed += 1;
        let nearest = (ctx.quantizer.index_of(cur) + 1) as u8;
        near[x] = nearest;
        if nearest == shown[x] {
            continue;
        }
        let tolerance = if is_flat(ctx, x, y) || is_coherent_shift(prev_row, cur_row, x) {
            ctx.params.coherent_tolerance
        } else {
            ctx.params.tolerance
        };
        if within_tolerance(cur, ctx.palette_rgb, shown[x], tolerance) {
            continue;
        }
        out[x] = nearest;
        first = first.min(x);
        last = x;
    }

    RowDiff {
        span: (first != usize::MAX).then_some((first, last)),
        source_changed,
    }
}

/// 邻近像素的通道差不超过该值即视为平坦
const FLAT_SPREAD: i32 = 2;

/// 相邻像素变化量之差不超过该值即视为一致
const COHERENT_SPREAD: i32 = 2;

/// 当前帧里 `(x, y)` 与上下左右邻居几乎同色。
/// JPEG 噪点只出现在边缘与纹理附近，平坦处与显示色的差异必然是真实变化，
/// 文字移走后留在背景上的残影也靠它擦掉。
fn is_flat(ctx: &RowContext, x: usize, y: usize) -> bool {
    let px = |x: usize, y: usize| &ctx.cur[(y * ctx.width + x) * 4..][..3];
    let center = px(x, y);
    let mut neighbours = 0;
    for (nx, ny) in [(x.wrapping_sub(1), y), (x + 1, y), (x, y.wrapping_sub(1)), (x, y + 1)] {
        if nx >= ctx.width || ny >= ctx.height {
            continue;
        }
        let n = px(nx, ny);
        if (0..3).any(|c| (n[c] as i32 - center[c] as i32).abs() > FLAT_SPREAD) {
            return false;
        }
        neighbours += 1;
    }
    neighbours >= 2
}

/// `x` 及其左右邻居都发生了变化，且三者的变化量在各通道上一致
fn is_coherent_shift(prev_row: &[u8], cur_row: &[u8], x: usize) -> bool {
    let n = cur_row.len() / 4;
    if x == 0 || x + 1 >= n {
        return false;
    }
    let delta = |x: usize, c: usize| cur_row[x * 4 + c] as i32 - prev_row[x * 4 + c] as i32;
    (0..3).all(|c| {
        let d = delta(x, c);
        (delta(x - 1, c) - d).abs() <= COHERENT_SPREAD && (delta(x + 1, c) - d).abs() <= COHERENT_SPREAD
    }) && [x - 1, x + 1]
        .iter()
        .all(|&nx| prev_row[nx * 4..nx * 4 + 3] != cur_row[nx * 4..nx * 4 + 3])
}

fn within_tolerance(px: &[u8], palette_rgb: &[u8], index: u8, tolerance: i32) -> bool {
    if tolerance <= 0 {
        return false;
    }
    let p = &palette_rgb[index as usize * 3..index as usize * 3 + 3];
    (0..3).all(|c| (px[c] as i32 - p[c] as i32).abs() <= tolerance)
}

#[cfg(test)]
mod tests {
    use super::*;

    const TOL: i32 = 8;
    const SPARSE: DiffParams = DiffParams { tolerance: TOL, coherent_tolerance: 2, dense_ratio: 2.0 };
    const DENSE: DiffParams = DiffParams { tolerance: TOL, coherent_tolerance: 2, dense_ratio: 0.9 };

    /// 灰阶调色板：每个灰度基本都有对应项，量化误差很小。
    fn gray_palette() -> (NeuQuant, Vec<u8>) {
        let sample: Vec<u8> = (0..=255u8).flat_map(|v| [v, v, v, 255]).collect();
        let q = NeuQuant::new(1, 255, &sample);
        let mut rgb = vec![0, 0, 0];
        rgb.extend_from_slice(&q.color_map_rgb());
        (q, rgb)
    }

    fn gray_frame(w: usize, h: usize, v: u8) -> Vec<u8> {
        (0..w * h).flat_map(|_| [v, v, v, 255]).collect()
    }

    fn set_px(frame: &mut [u8], w: usize, x: usize, y: usize, v: u8) {
        let o = (y * w + x) * 4;
        frame[o..o + 3].copy_from_slice(&[v, v, v]);
    }

    fn started(w: usize, h: usize, v: u8, q: &NeuQuant) -> (Canvas, Vec<u8>) {
        let mut canvas = Canvas::new(w, h);
        let frame = gray_frame(w, h, v);
        canvas.start(&frame, q);
        (canvas, frame)
    }

    #[test]
    fn start_quantizes_every_pixel() {
        let (q, _) = gray_palette();
        let mut canvas = Canvas::new(4, 3);
        let indices = canvas.start(&gray_frame(4, 3, 120), &q);
        assert_eq!(indices.len(), 12);
        assert!(indices.iter().all(|&i| i != TRANSPARENT_INDEX && i == indices[0]));
    }

    #[test]
    fn identical_frame_has_no_region() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(8, 8, 100, &q);
        assert!(canvas.advance(&prev, &prev.clone(), &pal, &q, SPARSE).is_none());
    }

    #[test]
    fn change_within_tolerance_is_ignored() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(8, 8, 100, &q);
        let mut cur = prev.clone();
        set_px(&mut cur, 8, 3, 3, 100 + (TOL as u8 - 4));
        assert!(canvas.advance(&prev, &cur, &pal, &q, SPARSE).is_none());
    }

    #[test]
    fn uniform_small_shift_is_not_mistaken_for_noise() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(16, 2, 100, &q);
        let cur = gray_frame(16, 2, 105);
        let r = canvas.advance(&prev, &cur, &pal, &q, SPARSE).unwrap();
        assert_eq!((r.left, r.top, r.width, r.height), (0, 0, 16, 2));
    }

    #[test]
    fn coherent_shift_on_textured_pixels_is_kept() {
        // 每个像素与邻居相差 10（不平坦），但整行统一偏移 5：只能靠相干判断保留，两端缺邻居的除外
        let (q, pal) = gray_palette();
        let ramp = |offset: u8| -> Vec<u8> {
            (0..16u8).flat_map(|x| [100 + x * 10 + offset, 100 + x * 10 + offset, 100 + x * 10 + offset, 255]).collect()
        };
        let mut canvas = Canvas::new(16, 1);
        let prev = ramp(0);
        canvas.start(&prev, &q);
        let r = canvas.advance(&prev, &ramp(5), &pal, &q, SPARSE).unwrap();
        assert_eq!((r.left, r.width), (1, 14));
    }

    #[test]
    fn jittering_textured_pixels_are_still_ignored() {
        let (q, pal) = gray_palette();
        let ramp = |sign: i32| -> Vec<u8> {
            (0..16i32)
                .flat_map(|x| {
                    let v = (100 + x * 10 + if x % 2 == 0 { sign * 2 } else { -sign * 2 }) as u8;
                    [v, v, v, 255]
                })
                .collect()
        };
        let mut canvas = Canvas::new(16, 1);
        let prev = ramp(1);
        canvas.start(&prev, &q);
        assert!(canvas.advance(&prev, &ramp(-1), &pal, &q, SPARSE).is_none());
    }

    #[test]
    fn flat_background_clears_stale_residue() {
        let (q, pal) = gray_palette();
        let mut canvas = Canvas::new(16, 4);
        let mut noisy = gray_frame(16, 4, 100);
        set_px(&mut noisy, 16, 8, 1, 106);
        canvas.start(&noisy, &q);
        let clean = gray_frame(16, 4, 100);
        let r = canvas.advance(&noisy, &clean, &pal, &q, SPARSE).unwrap();
        assert_eq!((r.left, r.top, r.width, r.height), (8, 1, 1, 1));
    }

    #[test]
    fn residue_beside_busy_pixels_stays_within_tolerance() {
        let (q, pal) = gray_palette();
        let mut canvas = Canvas::new(16, 4);
        let mut noisy = gray_frame(16, 4, 100);
        set_px(&mut noisy, 16, 8, 1, 106);
        canvas.start(&noisy, &q);
        let mut cur = gray_frame(16, 4, 100);
        set_px(&mut cur, 16, 9, 1, 160);
        let r = canvas.advance(&noisy, &cur, &pal, &q, SPARSE).unwrap();
        // 只有 (9,1) 的真实变化被写出，(8,1) 邻着高反差像素、不平坦，残留 6 级仍在容差内
        assert_eq!((r.left, r.top, r.width, r.height), (9, 1, 1, 1));
    }

    #[test]
    fn jittering_changes_within_tolerance_are_ignored() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(16, 1, 100, &q);
        let mut cur = prev.clone();
        for x in 0..16 {
            set_px(&mut cur, 16, x, 0, if x % 2 == 0 { 105 } else { 95 });
        }
        assert!(canvas.advance(&prev, &cur, &pal, &q, SPARSE).is_none());
    }

    #[test]
    fn region_is_the_tight_bounding_box() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(16, 8, 50, &q);
        let mut cur = prev.clone();
        for (x, y) in [(5, 3), (6, 3), (7, 3), (5, 4), (6, 4), (7, 4)] {
            set_px(&mut cur, 16, x, y, 250);
        }
        let r = canvas.advance(&prev, &cur, &pal, &q, SPARSE).unwrap();
        assert_eq!((r.left, r.top, r.width, r.height), (5, 3, 3, 2));
        assert_eq!(r.indices.len(), 6);
        assert!(r.indices.iter().all(|&i| i != TRANSPARENT_INDEX));
    }

    #[test]
    fn unchanged_pixels_inside_region_stay_transparent() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(16, 8, 50, &q);
        let mut cur = prev.clone();
        set_px(&mut cur, 16, 2, 1, 250);
        set_px(&mut cur, 16, 12, 6, 250);
        let r = canvas.advance(&prev, &cur, &pal, &q, DENSE).unwrap();
        assert_eq!((r.left, r.top, r.width, r.height), (2, 1, 11, 6));
        assert_eq!(r.indices.iter().filter(|&&i| i != TRANSPARENT_INDEX).count(), 2);
    }

    /// 两端大变化、中间只是容差内的细小变化：稀疏时中间保持透明，密集时整块写出。
    fn edges_changed_everything_shifted() -> (NeuQuant, Vec<u8>, Vec<u8>, Vec<u8>) {
        let (q, pal) = gray_palette();
        let prev = gray_frame(8, 1, 100);
        let mut cur = prev.clone();
        for x in 1..7 {
            set_px(&mut cur, 8, x, 0, 101);
        }
        set_px(&mut cur, 8, 0, 0, 250);
        set_px(&mut cur, 8, 7, 0, 250);
        (q, pal, prev, cur)
    }

    #[test]
    fn sparse_changes_keep_the_middle_transparent() {
        let (q, pal, prev, cur) = edges_changed_everything_shifted();
        let mut canvas = Canvas::new(8, 1);
        canvas.start(&prev, &q);
        let r = canvas.advance(&prev, &cur, &pal, &q, SPARSE).unwrap();
        assert_eq!(r.indices.iter().filter(|&&i| i == TRANSPARENT_INDEX).count(), 6);
    }

    #[test]
    fn dense_source_changes_write_the_whole_region() {
        let (q, pal, prev, cur) = edges_changed_everything_shifted();
        let mut canvas = Canvas::new(8, 1);
        canvas.start(&prev, &q);
        let r = canvas.advance(&prev, &cur, &pal, &q, DENSE).unwrap();
        assert!(r.indices.iter().all(|&i| i != TRANSPARENT_INDEX));
        // 写出的是最近色，画布随之更新，下一帧不再重复这些像素
        assert!(canvas.advance(&cur, &cur.clone(), &pal, &q, DENSE).is_none());
    }

    #[test]
    fn slow_drift_never_exceeds_tolerance() {
        let (q, pal) = gray_palette();
        let (mut canvas, mut prev) = started(4, 4, 100, &q);
        for step in 1..=40u8 {
            let cur = gray_frame(4, 4, 100 + step * 2);
            canvas.advance(&prev, &cur, &pal, &q, SPARSE);
            let src = cur[0] as i32;
            let shown = canvas.shown[0] as usize;
            let quant_err = (pal[(q.index_of(&cur[..4]) + 1) * 3] as i32 - src).abs();
            let shown_err = (pal[shown * 3] as i32 - src).abs();
            assert!(shown_err <= TOL.max(quant_err), "step {step}: shown error {shown_err}");
            prev = cur;
        }
    }

    #[test]
    fn strict_tolerance_reports_any_palette_change() {
        let (q, pal) = gray_palette();
        let (mut canvas, prev) = started(8, 8, 100, &q);
        let mut cur = prev.clone();
        set_px(&mut cur, 8, 3, 3, 140);
        let strict = DiffParams { tolerance: 0, coherent_tolerance: 0, dense_ratio: 2.0 };
        assert!(canvas.advance(&prev, &cur, &pal, &q, strict).is_some());
    }
}
