//! PP-OCR 引擎：det(DBNet) + rec(CRNN/CTC)，纯 Rust + ort。

use std::sync::{Mutex, OnceLock};

use ort::session::Session;
use ort::value::Tensor;

use crate::geometry::{self, Pt};

// ====================== 打包的 RGB 图像 ======================

struct Img {
    w: usize,
    h: usize,
    data: Vec<u8>, // 紧密排列的 RGB，长度 w*h*3
}

impl Img {
    /// 从带 stride 的 RGB 源构造紧密 RGB。
    fn from_raw_rgb(src: &[u8], w: usize, h: usize, stride: usize) -> Img {
        let mut data = vec![0u8; w * h * 3];
        for y in 0..h {
            let srow = y * stride;
            let drow = y * w * 3;
            data[drow..drow + w * 3].copy_from_slice(&src[srow..srow + w * 3]);
        }
        Img { w, h, data }
    }

    #[inline]
    fn px(&self, x: usize, y: usize) -> [f32; 3] {
        let i = (y * self.w + x) * 3;
        [self.data[i] as f32, self.data[i + 1] as f32, self.data[i + 2] as f32]
    }

    /// 双线性采样（坐标可越界，自动 clamp）。返回 RGB。
    fn sample(&self, fx: f32, fy: f32) -> [f32; 3] {
        let x = fx.max(0.0).min((self.w - 1) as f32);
        let y = fy.max(0.0).min((self.h - 1) as f32);
        let x0 = x.floor() as usize;
        let y0 = y.floor() as usize;
        let x1 = (x0 + 1).min(self.w - 1);
        let y1 = (y0 + 1).min(self.h - 1);
        let dx = x - x0 as f32;
        let dy = y - y0 as f32;
        let p00 = self.px(x0, y0);
        let p10 = self.px(x1, y0);
        let p01 = self.px(x0, y1);
        let p11 = self.px(x1, y1);
        let mut out = [0.0f32; 3];
        for c in 0..3 {
            let top = p00[c] * (1.0 - dx) + p10[c] * dx;
            let bot = p01[c] * (1.0 - dx) + p11[c] * dx;
            out[c] = top * (1.0 - dy) + bot * dy;
        }
        out
    }
}

// ====================== 预处理 ======================

/// 把整图按 DBNet 规则缩放，归一化为 CHW(BGR, (x/255-0.5)/0.5)。
/// 返回 (chw_data, dst_w, dst_h, ratio_w, ratio_h)。
fn preprocess_det(img: &Img) -> (Vec<f32>, usize, usize, f32, f32) {
    let limit_side: f32 = 736.0;
    let max_side_cap: f32 = 1536.0; // 防止超大图导致推理过慢
    let (h, w) = (img.h as f32, img.w as f32);

    // limit_type = min：短边不足 limit 则放大
    let mut ratio = if h.min(w) < limit_side {
        limit_side / h.min(w)
    } else {
        1.0
    };
    // 上限保护：长边不超过 max_side_cap
    if h.max(w) * ratio > max_side_cap {
        ratio = max_side_cap / h.max(w);
    }

    let round32 = |v: f32| -> usize {
        let r = (v / 32.0).round() as i64 * 32;
        r.max(32) as usize
    };
    let dst_h = round32(h * ratio);
    let dst_w = round32(w * ratio);
    let ratio_h = dst_h as f32 / h;
    let ratio_w = dst_w as f32 / w;

    let plane = dst_h * dst_w;
    let mut data = vec![0f32; 3 * plane];
    for y in 0..dst_h {
        let sy = y as f32 / ratio_h;
        for x in 0..dst_w {
            let sx = x as f32 / ratio_w;
            let rgb = img.sample(sx, sy);
            let off = y * dst_w + x;
            // BGR 顺序 + 归一化
            data[off] = (rgb[2] / 255.0 - 0.5) / 0.5; // B
            data[plane + off] = (rgb[1] / 255.0 - 0.5) / 0.5; // G
            data[2 * plane + off] = (rgb[0] / 255.0 - 0.5) / 0.5; // R
        }
    }
    (data, dst_w, dst_h, ratio_w, ratio_h)
}

// ====================== 连通域 (BFS, 4-邻接) ======================

fn connected_components(bitmap: &[bool], w: usize, h: usize) -> Vec<Vec<(usize, usize)>> {
    let mut visited = vec![false; w * h];
    let mut comps: Vec<Vec<(usize, usize)>> = Vec::new();
    let mut stack: Vec<(usize, usize)> = Vec::new();
    for sy in 0..h {
        for sx in 0..w {
            let idx = sy * w + sx;
            if !bitmap[idx] || visited[idx] {
                continue;
            }
            stack.clear();
            stack.push((sx, sy));
            visited[idx] = true;
            let mut comp = Vec::new();
            while let Some((x, y)) = stack.pop() {
                comp.push((x, y));
                let neigh = [
                    (x.wrapping_sub(1), y),
                    (x + 1, y),
                    (x, y.wrapping_sub(1)),
                    (x, y + 1),
                ];
                for &(nx, ny) in neigh.iter() {
                    if nx < w && ny < h {
                        let nidx = ny * w + nx;
                        if bitmap[nidx] && !visited[nidx] {
                            visited[nidx] = true;
                            stack.push((nx, ny));
                        }
                    }
                }
            }
            if comp.len() >= 4 {
                comps.push(comp);
            }
        }
    }
    comps
}

// ====================== 透视裁剪（四边形 → 矩形） ======================

/// 用四边形双线性映射把文本框抠出来并校正为正立矩形。
fn crop_quad(img: &Img, quad: &[Pt]) -> Img {
    let tl = quad[0];
    let tr = quad[1];
    let br = quad[2];
    let bl = quad[3];
    let dist = |a: Pt, b: Pt| ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt();
    let crop_w = dist(tl, tr).max(dist(bl, br)).round().max(1.0) as usize;
    let crop_h = dist(tl, bl).max(dist(tr, br)).round().max(1.0) as usize;

    let mut data = vec![0u8; crop_w * crop_h * 3];
    for y in 0..crop_h {
        let v = if crop_h > 1 { y as f32 / (crop_h - 1) as f32 } else { 0.0 };
        for x in 0..crop_w {
            let u = if crop_w > 1 { x as f32 / (crop_w - 1) as f32 } else { 0.0 };
            // 单位方块 → 四边形的双线性插值
            let top_x = tl[0] * (1.0 - u) + tr[0] * u;
            let top_y = tl[1] * (1.0 - u) + tr[1] * u;
            let bot_x = bl[0] * (1.0 - u) + br[0] * u;
            let bot_y = bl[1] * (1.0 - u) + br[1] * u;
            let sx = top_x * (1.0 - v) + bot_x * v;
            let sy = top_y * (1.0 - v) + bot_y * v;
            let rgb = img.sample(sx, sy);
            let i = (y * crop_w + x) * 3;
            data[i] = rgb[0] as u8;
            data[i + 1] = rgb[1] as u8;
            data[i + 2] = rgb[2] as u8;
        }
    }
    let mut out = Img { w: crop_w, h: crop_h, data };
    // 竖排文本（高远大于宽）：顺时针转 90°，让文字变横向
    if out.h as f32 >= out.w as f32 * 1.5 {
        out = rotate90(&out);
    }
    out
}

fn rotate90(img: &Img) -> Img {
    // 顺时针 90°：new(w,h) = (img.h, img.w); new[x,y] = old[y, h-1-x]
    let nw = img.h;
    let nh = img.w;
    let mut data = vec![0u8; nw * nh * 3];
    for y in 0..nh {
        for x in 0..nw {
            let oy = x;
            let ox = img.w - 1 - y;
            let src = (oy * img.w + ox) * 3;
            let dst = (y * nw + x) * 3;
            data[dst..dst + 3].copy_from_slice(&img.data[src..src + 3]);
        }
    }
    Img { w: nw, h: nh, data }
}

// ====================== rec 预处理 ======================

/// 把一行文本图缩放到高 48，归一化为 CHW(BGR)。返回 (chw_data, dst_w)。
fn preprocess_rec(img: &Img) -> (Vec<f32>, usize) {
    let target_h = 48usize;
    let ratio = img.w as f32 / img.h as f32;
    let mut dst_w = (target_h as f32 * ratio).ceil() as usize;
    dst_w = dst_w.max(16).min(2000);

    let plane = target_h * dst_w;
    let mut data = vec![0f32; 3 * plane];
    for y in 0..target_h {
        let sy = y as f32 / target_h as f32 * img.h as f32;
        for x in 0..dst_w {
            let sx = x as f32 / dst_w as f32 * img.w as f32;
            let rgb = img.sample(sx, sy);
            let off = y * dst_w + x;
            data[off] = (rgb[2] / 255.0 - 0.5) / 0.5;
            data[plane + off] = (rgb[1] / 255.0 - 0.5) / 0.5;
            data[2 * plane + off] = (rgb[0] / 255.0 - 0.5) / 0.5;
        }
    }
    (data, dst_w)
}

// ====================== 引擎 ======================

pub struct OcrLine {
    pub box_pts: Vec<Pt>, // 原图坐标的 4 点
    pub text: String,
    pub score: f32,
}

struct Engine {
    det: Session,
    rec: Session,
    vocab: Vec<String>, // index 0 = blank
}

static ENGINE: OnceLock<Mutex<Option<Engine>>> = OnceLock::new();

fn cell() -> &'static Mutex<Option<Engine>> {
    ENGINE.get_or_init(|| Mutex::new(None))
}

pub fn is_initialized() -> bool {
    cell().lock().map(|g| g.is_some()).unwrap_or(false)
}

pub fn initialize(det_path: &str, rec_path: &str) -> Result<(), String> {
    let mut guard = cell().lock().map_err(|e| e.to_string())?;
    if guard.is_some() {
        return Ok(());
    }
    let det = Session::builder()
        .map_err(|e| e.to_string())?
        .commit_from_file(det_path)
        .map_err(|e| format!("加载 det 模型失败: {e}"))?;
    let rec = Session::builder()
        .map_err(|e| e.to_string())?
        .commit_from_file(rec_path)
        .map_err(|e| format!("加载 rec 模型失败: {e}"))?;

    // 从 rec 模型 metadata 读取字典，构造 CTC vocab（用块作用域确保 meta 借用结束后再移动 rec）
    let charset = {
        let meta = rec.metadata().map_err(|e| e.to_string())?;
        meta.custom("character").unwrap_or_default()
    };
    let mut vocab: Vec<String> = Vec::with_capacity(charset.lines().count() + 2);
    vocab.push(String::new()); // index 0 = blank
    for line in charset.lines() {
        vocab.push(line.to_string());
    }
    vocab.push(" ".to_string()); // 末尾 space

    *guard = Some(Engine { det, rec, vocab });
    Ok(())
}

pub fn release() {
    if let Ok(mut g) = cell().lock() {
        *g = None;
    }
}

/// 识别整张图，返回所有文本行（原图坐标）。
pub fn recognize(src_rgb: &[u8], w: usize, h: usize, stride: usize) -> Result<Vec<OcrLine>, String> {
    let mut guard = cell().lock().map_err(|e| e.to_string())?;
    let engine = guard.as_mut().ok_or("引擎未初始化")?;

    let img = Img::from_raw_rgb(src_rgb, w, h, stride);

    // ---- det ----
    let (det_data, dst_w, dst_h, ratio_w, ratio_h) = preprocess_det(&img);
    let det_tensor = Tensor::from_array((
        [1_i64, 3, dst_h as i64, dst_w as i64],
        det_data,
    ))
    .map_err(|e| e.to_string())?;
    let det_out = engine
        .det
        .run(ort::inputs!["x" => det_tensor])
        .map_err(|e| format!("det 推理失败: {e}"))?;
    let prob = det_out[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| e.to_string())?;
    let prob_slice = prob.1; // (&Shape, &[f32])

    // 二值化
    let thresh = 0.3f32;
    let box_thresh = 0.5f32;
    let unclip_ratio = 1.6f32;
    let n = dst_w * dst_h;
    let prob_map: &[f32] = &prob_slice[..n.min(prob_slice.len())];
    let bitmap: Vec<bool> = prob_map.iter().map(|&v| v > thresh).collect();

    let comps = connected_components(&bitmap, dst_w, dst_h);

    let mut boxes: Vec<Vec<Pt>> = Vec::new();
    for comp in comps {
        let pts: Vec<Pt> = comp.iter().map(|&(x, y)| [x as f32, y as f32]).collect();
        let hull = geometry::convex_hull(pts.clone());
        let rect = geometry::min_area_rect(&hull);
        // box_score_fast：AABB 内 prob 均值
        let score = box_score(prob_map, dst_w, dst_h, &rect);
        if score < box_thresh {
            continue;
        }
        // unclip
        let expanded = geometry::unclip(&rect, unclip_ratio);
        let final_box = geometry::min_area_rect(&geometry::convex_hull(expanded));
        // 最小边过滤
        let sw = dist(final_box[0], final_box[1]).min(dist(final_box[1], final_box[2]));
        if sw < 3.0 {
            continue;
        }
        // 映射回原图坐标
        let mapped: Vec<Pt> = final_box
            .iter()
            .map(|p| {
                [
                    (p[0] / ratio_w).max(0.0).min((w - 1) as f32),
                    (p[1] / ratio_h).max(0.0).min((h - 1) as f32),
                ]
            })
            .collect();
        boxes.push(geometry::order_quad(mapped));
    }

    // 按从上到下、从左到右排序
    boxes.sort_by(|a, b| {
        let ay = a.iter().map(|p| p[1]).sum::<f32>();
        let by = b.iter().map(|p| p[1]).sum::<f32>();
        ay.partial_cmp(&by).unwrap()
    });

    // ---- rec ----
    let mut results = Vec::new();
    for quad in boxes {
        let crop = crop_quad(&img, &quad);
        if crop.w < 3 || crop.h < 3 {
            continue;
        }
        let (rec_data, dw) = preprocess_rec(&crop);
        let rec_tensor = Tensor::from_array((
            [1_i64, 3, 48_i64, dw as i64],
            rec_data,
        ))
        .map_err(|e| e.to_string())?;
        let rec_out = engine
            .rec
            .run(ort::inputs!["x" => rec_tensor])
            .map_err(|e| format!("rec 推理失败: {e}"))?;
        let (shape, data) = rec_out[0]
            .try_extract_tensor::<f32>()
            .map_err(|e| e.to_string())?;
        // shape = [1, T, C]
        let t = shape[1] as usize;
        let c = shape[2] as usize;
        let (text, score) = ctc_decode(data, t, c, &engine.vocab);
        if !text.is_empty() {
            results.push(OcrLine { box_pts: quad, text, score });
        }
    }

    Ok(results)
}

fn dist(a: Pt, b: Pt) -> f32 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt()
}

/// AABB 内概率均值。
fn box_score(prob: &[f32], w: usize, h: usize, quad: &[Pt]) -> f32 {
    let (mut minx, mut miny, mut maxx, mut maxy) = (f32::MAX, f32::MAX, f32::MIN, f32::MIN);
    for p in quad {
        minx = minx.min(p[0]);
        miny = miny.min(p[1]);
        maxx = maxx.max(p[0]);
        maxy = maxy.max(p[1]);
    }
    let x0 = (minx.floor().max(0.0) as usize).min(w - 1);
    let x1 = (maxx.ceil().max(0.0) as usize).min(w - 1);
    let y0 = (miny.floor().max(0.0) as usize).min(h - 1);
    let y1 = (maxy.ceil().max(0.0) as usize).min(h - 1);
    let mut sum = 0.0;
    let mut cnt = 0;
    for y in y0..=y1 {
        for x in x0..=x1 {
            sum += prob[y * w + x];
            cnt += 1;
        }
    }
    if cnt == 0 {
        0.0
    } else {
        sum / cnt as f32
    }
}

/// CTC 贪心解码。data 为 [T*C] 行优先。返回 (text, mean_score)。
fn ctc_decode(data: &[f32], t: usize, c: usize, vocab: &[String]) -> (String, f32) {
    let mut text = String::new();
    let mut score_sum = 0.0f32;
    let mut score_cnt = 0u32;
    let mut prev_idx = 0usize;
    for ti in 0..t {
        let row = &data[ti * c..ti * c + c];
        let mut best = 0usize;
        let mut best_v = row[0];
        for (i, &v) in row.iter().enumerate() {
            if v > best_v {
                best_v = v;
                best = i;
            }
        }
        if best != 0 && best != prev_idx {
            if best < vocab.len() {
                text.push_str(&vocab[best]);
                score_sum += best_v;
                score_cnt += 1;
            }
        }
        prev_idx = best;
    }
    let score = if score_cnt > 0 { score_sum / score_cnt as f32 } else { 0.0 };
    (text, score)
}
