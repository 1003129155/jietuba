//! PP-OCR 引擎：det(DBNet) + rec(CRNN/CTC)，纯 Rust + ort。


use ort::session::Session;
use ort::value::Tensor;

use crate::geometry::{self, Pt};

// ====================== 运行库 ======================

#[cfg(feature = "load-dynamic")]
static RUNTIME_LOADED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// 从绝对路径加载 ONNX Runtime 动态库，须在 [`Engine::open`] 之前调用。
///
/// 进程内只加载一份：成功后再调用直接返回成功；失败不留状态，可以重试。
/// 只收绝对路径：未加载时 ort 会按文件名走 DLL 搜索顺序，可能碰上系统自带的旧版。
#[cfg(feature = "load-dynamic")]
pub fn init_runtime(dll_path: &std::path::Path) -> Result<(), String> {
    use std::sync::atomic::Ordering;
    if RUNTIME_LOADED.load(Ordering::Acquire) {
        return Ok(());
    }
    if !dll_path.is_absolute() {
        return Err(format!("ONNX Runtime 路径不是绝对路径: {}", dll_path.display()));
    }
    if !dll_path.is_file() {
        return Err(format!("找不到 ONNX Runtime: {}", dll_path.display()));
    }
    ort::init_from(dll_path)
        .map_err(|e| format!("加载 ONNX Runtime 失败: {e}"))?
        .commit();
    RUNTIME_LOADED.store(true, Ordering::Release);
    Ok(())
}

/// ONNX Runtime 是否已可用（静态链接时恒为真）。
pub fn runtime_loaded() -> bool {
    #[cfg(feature = "load-dynamic")]
    {
        RUNTIME_LOADED.load(std::sync::atomic::Ordering::Acquire)
    }
    #[cfg(not(feature = "load-dynamic"))]
    {
        true
    }
}

// ====================== 打包的 RGB 图像 ======================

struct Img {
    w: usize,
    h: usize,
    data: Vec<u8>, // 紧密排列的 RGB，长度 w*h*3
}

impl Img {
    /// 从带 stride 的 RGB 源构造紧密 RGB；源比给的尺寸短时报错。
    fn from_raw_rgb(src: &[u8], w: usize, h: usize, stride: usize) -> Result<Img, String> {
        if h > 0 && (stride < w * 3 || src.len() < (h - 1) * stride + w * 3) {
            return Err(format!("图像数据不足: {w}x{h}, stride {stride}, {} 字节", src.len()));
        }
        let mut data = vec![0u8; w * h * 3];
        for y in 0..h {
            let srow = y * stride;
            let drow = y * w * 3;
            data[drow..drow + w * 3].copy_from_slice(&src[srow..srow + w * 3]);
        }
        Ok(Img { w, h, data })
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

// ====================== 检测预处理：补边与分块 ======================
//
// 检测按原尺寸做、不缩放：缩小会把小字缩没，长图压到一千多像素后整张都认不出。
// 大图分块推理，各块的概率图拼回整图再统一出框，框坐标就是原图坐标。

/// 任一边（补边后）达到这个长度就分块：整张大图一次推理，模型的中间张量随面积涨，
/// 4K 屏要 1.7 GB，分块后不到 500 MB。
const DET_TILE_FROM: usize = 1440;
const DET_TILE: usize = 960;
/// 相邻块的重叠。字在块边上被截断，模型在离块边几十像素内给的概率或缺或歪，大字跨过块边时
/// 会断成两段、重复认出半个字；重叠里按离各自块边的远近交叉淡化（见 paste_blend）。
/// 重叠只有 96 时，蓝底上的小字跨过块边仍会断。
const DET_TILE_OVERLAP: usize = 192;

/// 检测的归一化照 PaddleOCR 训练时的做法：图按 BGR 排，ImageNet 的均值方差依次用在 B、G、R 上。
const DET_MEAN: [f32; 3] = [0.485, 0.456, 0.406];
const DET_STD: [f32; 3] = [0.229, 0.224, 0.225];

/// 补边后的检测图：w×h，原图左上角在 (left, top)。
#[derive(Clone, Copy, Debug, PartialEq)]
struct Canvas {
    w: usize,
    h: usize,
    left: usize,
    top: usize,
}

/// 宽高各补到 32 的倍数且不小于 128，两侧均分。
fn det_canvas(w: usize, h: usize) -> Canvas {
    let side = |v: usize| v.max(128).div_ceil(32) * 32;
    let (cw, ch) = (side(w), side(h));
    Canvas { w: cw, h: ch, left: (cw - w) / 2, top: (ch - h) / 2 }
}

/// 补边的颜色：四个角像素的平均，接近底色，补出的边不会被当成一道框线。
fn border_color(img: &Img) -> [u8; 3] {
    let corners = [img.px(0, 0), img.px(img.w - 1, 0), img.px(0, img.h - 1), img.px(img.w - 1, img.h - 1)];
    let mut fill = [0u8; 3];
    for (c, value) in fill.iter_mut().enumerate() {
        *value = (corners.iter().map(|p| p[c]).sum::<f32>() / 4.0).round() as u8;
    }
    fill
}

/// 一个检测块在补边后整图上的位置和大小。
#[derive(Clone, Copy, Debug, PartialEq)]
struct Tile {
    x: usize,
    y: usize,
    w: usize,
    h: usize,
}

/// 一条边上的分段 (起点, 长度)：每段 DET_TILE、相邻重叠 DET_TILE_OVERLAP，剩下不足 1.3 段时
/// 整个并进最后一段，免得末尾切出一条窄边。length 是 32 的倍数，各段的起点和长度因此也是。
fn det_spans(length: usize) -> Vec<(usize, usize)> {
    let mut spans = Vec::new();
    let mut start = 0;
    loop {
        let rest = length - start;
        if rest * 10 < DET_TILE * 13 {
            spans.push((start, rest));
            return spans;
        }
        spans.push((start, DET_TILE));
        start += DET_TILE - DET_TILE_OVERLAP;
    }
}

/// 补边后的图切成的检测块，先行后列。
fn det_tiles(canvas: Canvas) -> Vec<Tile> {
    if canvas.w.max(canvas.h) < DET_TILE_FROM {
        return vec![Tile { x: 0, y: 0, w: canvas.w, h: canvas.h }];
    }
    let columns = det_spans(canvas.w);
    det_spans(canvas.h)
        .into_iter()
        .flat_map(|(y, h)| columns.iter().map(move |&(x, w)| Tile { x, y, w, h }))
        .collect()
}

/// 一块的检测输入，CHW(BGR)，按 DET_MEAN、DET_STD 归一化；落在补边里的像素取 fill。
fn det_input(img: &Img, fill: [u8; 3], canvas: Canvas, tile: Tile) -> Vec<f32> {
    let plane = tile.w * tile.h;
    let mut data = vec![0f32; 3 * plane];
    let border = fill.map(|v| v as f32);
    for ty in 0..tile.h {
        let sy = (tile.y + ty).checked_sub(canvas.top).filter(|&y| y < img.h);
        for tx in 0..tile.w {
            let sx = (tile.x + tx).checked_sub(canvas.left).filter(|&x| x < img.w);
            let rgb = match (sx, sy) {
                (Some(x), Some(y)) => img.px(x, y),
                _ => border,
            };
            let off = ty * tile.w + tx;
            for (c, value) in [rgb[2], rgb[1], rgb[0]].into_iter().enumerate() {
                data[c * plane + off] = (value / 255.0 - DET_MEAN[c]) / DET_STD[c];
            }
        }
    }
    data
}

/// 块内第 i 个像素（共 len 个）的权重：挨着别的块的一侧，在重叠宽度内从 0 线性升到 1。
/// 相邻两块的重叠正好是 DET_TILE_OVERLAP，两边的权重在重叠里相加处处为 1。
fn blend_weight(i: usize, len: usize, inner_before: bool, inner_after: bool) -> f32 {
    let overlap = DET_TILE_OVERLAP as f32;
    let mut weight = 1.0f32;
    if inner_before {
        weight = weight.min((i as f32 + 0.5) / overlap);
    }
    if inner_after {
        weight = weight.min(((len - i) as f32 - 0.5) / overlap);
    }
    weight
}

/// 把一块的概率图按权重加进原图大小 (w×h) 的概率图，补边部分丢掉。
fn paste_blend(prob: &mut [f32], w: usize, h: usize, canvas: Canvas, tile: Tile, tile_prob: &[f32]) {
    let x0 = tile.x.max(canvas.left);
    let x1 = (tile.x + tile.w).min(canvas.left + w);
    if x0 >= x1 {
        return;
    }
    let (has_left, has_right) = (tile.x > 0, tile.x + tile.w < canvas.w);
    let column_weights: Vec<f32> = (x0..x1).map(|cx| blend_weight(cx - tile.x, tile.w, has_left, has_right)).collect();
    for ty in 0..tile.h {
        let Some(y) = (tile.y + ty).checked_sub(canvas.top).filter(|&y| y < h) else { continue };
        let row_weight = blend_weight(ty, tile.h, tile.y > 0, tile.y + tile.h < canvas.h);
        let src = &tile_prob[ty * tile.w + (x0 - tile.x)..ty * tile.w + (x1 - tile.x)];
        let dst = &mut prob[y * w + (x0 - canvas.left)..y * w + (x1 - canvas.left)];
        for ((d, &s), &column_weight) in dst.iter_mut().zip(src).zip(&column_weights) {
            *d += row_weight * column_weight * s;
        }
    }
}

/// 分格清零的格子边长、算作明暗变化的相邻像素亮度差，和够得着的格数：离任何有变化的格子
/// 超过 FLAT_REACH 格（48 像素）就不会是字。
const FLAT_CELL: usize = 16;
const FLAT_STEP: i32 = 8;
const FLAT_REACH: usize = 3;

/// 大片纯色上模型也会给出整块的高概率（蓝底、米黄底尤其明显），出框后识别成空串或一个乱字，
/// 还可能和旁边的字连成一块。字离笔画边缘总不远，离明暗变化太远的地方把概率清零。
fn clear_flat(prob: &mut [f32], img: &Img) {
    let (w, h) = (img.w, img.h);
    let (gw, gh) = (w.div_ceil(FLAT_CELL), h.div_ceil(FLAT_CELL));
    let luma = |x: usize, y: usize| {
        let i = (y * w + x) * 3;
        (299 * img.data[i] as i32 + 587 * img.data[i + 1] as i32 + 114 * img.data[i + 2] as i32) / 1000
    };
    let mut changed = vec![false; gw * gh];
    for y in 0..h {
        for x in 0..w {
            let v = luma(x, y);
            if (x + 1 < w && (luma(x + 1, y) - v).abs() >= FLAT_STEP) || (y + 1 < h && (luma(x, y + 1) - v).abs() >= FLAT_STEP) {
                changed[(y / FLAT_CELL) * gw + x / FLAT_CELL] = true;
            }
        }
    }
    let mut near = vec![false; gw * gh];
    for gy in 0..gh {
        for gx in 0..gw {
            let (x0, x1) = (gx.saturating_sub(FLAT_REACH), (gx + FLAT_REACH).min(gw - 1));
            near[gy * gw + gx] = (gy.saturating_sub(FLAT_REACH)..=(gy + FLAT_REACH).min(gh - 1)).any(|row| changed[row * gw + x0..=row * gw + x1].contains(&true));
        }
    }
    for y in 0..h {
        for x in 0..w {
            if !near[(y / FLAT_CELL) * gw + x / FLAT_CELL] {
                prob[y * w + x] = 0.0;
            }
        }
    }
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

/// 用四边形双线性映射把文本框抠出来并校正为正立矩形；第二项为真表示竖排、已转成横向。
fn crop_quad(img: &Img, quad: &[Pt]) -> (Img, bool) {
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
    let out = Img { w: crop_w, h: crop_h, data };
    // 竖排文本（高远大于宽）：顺时针转 90°，让文字变横向
    if out.h as f32 >= out.w as f32 * 1.5 {
        return (rotate90(&out), true);
    }
    (out, false)
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

/// 把一行文本图缩放到识别模型的输入高度，归一化为 CHW(BGR)。返回 (chw_data, dst_w)。
/// 宽度按比例走、不封顶：整屏宽的一行压窄后字被挤扁，基本认不对。
fn preprocess_rec(img: &Img, target_h: usize) -> (Vec<f32>, usize) {
    let ratio = img.w as f32 / img.h as f32;
    let dst_w = ((target_h as f32 * ratio).ceil() as usize).max(16);

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

/// 一个字符在行内的位置，沿阅读方向，以整行长度为 1。
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CharSpan {
    pub start: f32,
    pub center: f32,
    pub end: f32,
}

pub struct OcrLine {
    pub box_pts: Vec<Pt>, // 原图坐标的 4 点，左上起顺时针
    pub text: String,
    pub score: f32,
    /// 竖排：识别前整行顺时针转了 90°，阅读方向是右上角到右下角，否则是左上角到右上角。
    pub vertical: bool,
    /// text 里每个字符（Unicode 标量）一项，按出现顺序。
    pub chars: Vec<CharSpan>,
}

pub struct Engine {
    det: Session,
    rec: Session,
    vocab: Vec<String>, // index 0 = blank
    rec_height: usize,
}

/// 输入高度不固定的识别模型按 48 送入：PP-OCR v3 起的识别模型都按这个高度训练。
const DEFAULT_REC_HEIGHT: usize = 48;

impl Engine {
    /// 加载 det/rec 模型，构造一个独立的引擎实例；字典取自 rec 模型的 `character` 元数据。
    ///
    /// 每个实例自带模型、互不干扰，生命周期由调用方持有；原先的进程级全局
    /// 单例无法同时跑两套模型，也无法在 release 与 recognize 并发时自保。
    pub fn open(det_path: &str, rec_path: &str) -> Result<Self, String> {
        Self::open_with_dict(det_path, rec_path, None)
    }

    /// 同 [`Engine::open`]，给了 `dict_path` 时字典以该文件为准（每行一个字符）。
    ///
    /// 字典行数必须和识别模型的输出类别数对得上（类别 = 空白 + 字典 + 可选的末尾空格），
    /// 对不上就报错：错一位的字典不会失败，只会把每个字都识别成别的字。
    pub fn open_with_dict(det_path: &str, rec_path: &str, dict_path: Option<&str>) -> Result<Self, String> {
        if !runtime_loaded() {
            return Err("ONNX Runtime 尚未加载".to_string());
        }
        let det = Session::builder()
            .map_err(|e| e.to_string())?
            .commit_from_file(det_path)
            .map_err(|e| format!("加载 det 模型失败: {e}"))?;
        let mut rec = Session::builder()
            .map_err(|e| e.to_string())?
            .commit_from_file(rec_path)
            .map_err(|e| format!("加载 rec 模型失败: {e}"))?;

        let charset = match dict_path {
            Some(path) => std::fs::read_to_string(path).map_err(|e| format!("读取字典失败: {e}"))?,
            None => rec
                .metadata()
                .map_err(|e| e.to_string())?
                .custom("character")
                .unwrap_or_default(),
        };
        let chars: Vec<&str> = charset.trim_start_matches('\u{feff}').lines().collect();
        if chars.is_empty() {
            return Err(if dict_path.is_some() {
                "字典文件是空的".to_string()
            } else {
                "识别模型没有字典：模型元数据里没有 character，也没有提供字典文件".to_string()
            });
        }
        let rec_height = rec.inputs()[0]
            .dtype()
            .tensor_shape()
            .and_then(|shape| shape.get(2).copied())
            .filter(|&h| h > 0)
            .map_or(DEFAULT_REC_HEIGHT, |h| h as usize);
        let classes = rec_classes(&mut rec, rec_height)?;
        let mut vocab: Vec<String> = Vec::with_capacity(chars.len() + 2);
        vocab.push(String::new()); // index 0 = blank
        vocab.extend(chars.iter().map(|c| c.to_string()));
        if classes == vocab.len() + 1 {
            vocab.push(" ".to_string());
        } else if classes != vocab.len() {
            return Err(format!(
                "字典有 {} 个字符，识别模型输出 {classes} 类，二者对不上",
                chars.len()
            ));
        }
        Ok(Engine { det, rec, vocab, rec_height })
    }

    /// 用空白图各跑一次检测和识别。部分算子缺失或形状不兼容要到推理时才报错，
    /// 添加模型时先跑一遍，免得等到用户识别时才失败。
    pub fn self_test(&mut self) -> Result<(), String> {
        let det = Tensor::from_array(([1_i64, 3, 64, 64], vec![0f32; 3 * 64 * 64])).map_err(|e| e.to_string())?;
        self.det
            .run(ort::inputs![det])
            .map_err(|e| format!("det 推理失败: {e}"))?;
        rec_classes(&mut self.rec, self.rec_height).map(|_| ())
    }

    /// 识别整张图，返回所有文本行（原图坐标）。长图要几秒，cancelled 在每个检测块和
    /// 每一行之前问一次，返回真就停下，结果为 None。
    pub fn recognize(
        &mut self,
        src_rgb: &[u8],
        w: usize,
        h: usize,
        stride: usize,
        cancelled: &dyn Fn() -> bool,
    ) -> Result<Option<Vec<OcrLine>>, String> {
    let img = Img::from_raw_rgb(src_rgb, w, h, stride)?;
    if w == 0 || h == 0 {
        return Ok(Some(Vec::new()));
    }

    // ---- det ----
    let canvas = det_canvas(w, h);
    let fill = border_color(&img);
    let mut prob = vec![0f32; w * h];
    for tile in det_tiles(canvas) {
        if cancelled() {
            return Ok(None);
        }
        let input = Tensor::from_array(([1_i64, 3, tile.h as i64, tile.w as i64], det_input(&img, fill, canvas, tile)))
            .map_err(|e| e.to_string())?;
        let out = self.det.run(ort::inputs![input]).map_err(|e| format!("det 推理失败: {e}"))?;
        let (shape, tile_prob) = out[0].try_extract_tensor::<f32>().map_err(|e| e.to_string())?;
        if tile_prob.len() != tile.w * tile.h {
            return Err(format!("det 输出形状 {shape:?} 与输入 {}x{} 不符", tile.w, tile.h));
        }
        paste_blend(&mut prob, w, h, canvas, tile, tile_prob);
    }
    clear_flat(&mut prob, &img);
    let boxes = det_boxes(&prob, w, h);

    // ---- rec ----
    let mut results = Vec::new();
    for quad in boxes {
        if cancelled() {
            return Ok(None);
        }
        let (crop, vertical) = crop_quad(&img, &quad);
        if crop.w < 3 || crop.h < 3 {
            continue;
        }
        let (rec_data, dw) = preprocess_rec(&crop, self.rec_height);
        let rec_tensor = Tensor::from_array((
            [1_i64, 3, self.rec_height as i64, dw as i64],
            rec_data,
        ))
        .map_err(|e| e.to_string())?;
        let rec_out = self
            .rec
            .run(ort::inputs![rec_tensor])
            .map_err(|e| format!("rec 推理失败: {e}"))?;
        let (shape, data) = rec_out[0]
            .try_extract_tensor::<f32>()
            .map_err(|e| e.to_string())?;
        // shape = [1, T, C]
        let t = shape[1] as usize;
        let c = shape[2] as usize;
        let decoded = ctc_decode(data, t, c, &self.vocab);
        if !decoded.text.is_empty() {
            // 识别输入按比例缩放、不补边，时间步与裁出的行图线性对应。
            let chars = char_spans(&decoded.tokens, crop.w as f32, crop.h as f32);
            results.push(OcrLine { box_pts: quad, text: decoded.text, score: decoded.score, vertical, chars });
        }
    }

    Ok(Some(results))
    }
}

/// 识别模型的输出类别数：输出形状固定时直接读，否则拿一张空白行图跑一次看输出。
fn rec_classes(rec: &mut Session, height: usize) -> Result<usize, String> {
    let declared = rec.outputs()[0]
        .dtype()
        .tensor_shape()
        .and_then(|shape| shape.last().copied())
        .filter(|&c| c > 0);
    let width = 160usize;
    let input = Tensor::from_array(([1_i64, 3, height as i64, width as i64], vec![0f32; 3 * height * width]))
        .map_err(|e| e.to_string())?;
    let out = rec.run(ort::inputs![input]).map_err(|e| format!("rec 推理失败: {e}"))?;
    let (shape, _) = out[0].try_extract_tensor::<f32>().map_err(|e| e.to_string())?;
    let actual = *shape.last().ok_or("rec 输出形状无效")?;
    if shape.len() != 3 || actual <= 0 || declared.is_some_and(|c| c != actual) {
        return Err(format!("rec 输出形状无效: {shape:?}"));
    }
    Ok(actual as usize)
}

fn dist(a: Pt, b: Pt) -> f32 {
    ((a[0] - b[0]).powi(2) + (a[1] - b[1]).powi(2)).sqrt()
}

/// DB 后处理：w×h 概率图上的文本框（4 点，左上起顺时针），从上到下排。
fn det_boxes(prob: &[f32], w: usize, h: usize) -> Vec<Vec<Pt>> {
    let thresh = 0.3f32;
    let box_thresh = 0.5f32;
    let unclip_ratio = 1.6f32;
    let bitmap: Vec<bool> = prob.iter().map(|&v| v > thresh).collect();

    let mut boxes: Vec<Vec<Pt>> = Vec::new();
    for comp in connected_components(&bitmap, w, h) {
        let pts: Vec<Pt> = comp.iter().map(|&(x, y)| [x as f32, y as f32]).collect();
        let rect = geometry::min_area_rect(&geometry::convex_hull(pts));
        if box_score(prob, w, h, &rect) < box_thresh {
            continue;
        }
        let expanded = geometry::unclip(&rect, unclip_ratio);
        let final_box = geometry::min_area_rect(&geometry::convex_hull(expanded));
        let short_side = dist(final_box[0], final_box[1]).min(dist(final_box[1], final_box[2]));
        if short_side < 3.0 {
            continue;
        }
        let clamped: Vec<Pt> = final_box
            .iter()
            .map(|p| [p[0].max(0.0).min((w - 1) as f32), p[1].max(0.0).min((h - 1) as f32)])
            .collect();
        boxes.push(geometry::order_quad(clamped));
    }
    boxes.sort_by(|a, b| {
        let ay = a.iter().map(|p| p[1]).sum::<f32>();
        let by = b.iter().map(|p| p[1]).sum::<f32>();
        ay.total_cmp(&by)
    });
    boxes
}

/// 框内（含边界）的概率均值。斜着的框只占外接矩形的一小部分，按整个外接矩形平均会被空白拉低。
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
            if geometry::convex_contains(quad, [x as f32, y as f32]) {
                sum += prob[y * w + x];
                cnt += 1;
            }
        }
    }
    if cnt == 0 {
        0.0
    } else {
        sum / cnt as f32
    }
}

/// 解码出的一个字典项：含几个字符，以及它在行内的中心（以帧数为 1）。
#[derive(Clone, Copy, Debug, PartialEq)]
struct Token {
    chars: usize,
    center: f32,
}

struct Decoded {
    text: String,
    score: f32,
    tokens: Vec<Token>,
}

/// CTC 贪心解码。data 为 [T*C] 行优先；分数是各字首帧概率的均值。
/// 一个字常连续占几帧，中心取这几帧的中点。
fn ctc_decode(data: &[f32], t: usize, c: usize, vocab: &[String]) -> Decoded {
    let run_center = |first: usize, last: usize| ((first + last) as f32 / 2.0 + 0.5) / t as f32;
    let mut text = String::new();
    let mut tokens: Vec<Token> = Vec::new();
    let mut score_sum = 0.0f32;
    let mut score_cnt = 0u32;
    let mut prev_idx = 0usize;
    // 还在延续的已输出项：(tokens 下标, 起始帧)
    let mut run: Option<(usize, usize)> = None;
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
        if best != prev_idx {
            if let Some((index, first)) = run.take() {
                tokens[index].center = run_center(first, ti - 1);
            }
            if best != 0 && best < vocab.len() {
                text.push_str(&vocab[best]);
                score_sum += best_v;
                score_cnt += 1;
                tokens.push(Token { chars: vocab[best].chars().count(), center: 0.0 });
                run = Some((tokens.len() - 1, ti));
            }
        }
        prev_idx = best;
    }
    if let Some((index, first)) = run {
        tokens[index].center = run_center(first, t - 1);
    }
    let score = if score_cnt > 0 { score_sum / score_cnt as f32 } else { 0.0 };
    Decoded { text, score, tokens }
}

/// 由各项中心推出逐字区间，length、thickness 是行图的宽高（像素），结果以 length 为 1。
/// 字宽取与前后中心间距的平均、不超过行高（首字以行首、末字以行尾为邻），夹在行内；
/// 相邻重叠时在两者之间对半分，分界不越过两边的中心。一项含多个字符时平分它的区间。
fn char_spans(tokens: &[Token], length: f32, thickness: f32) -> Vec<CharSpan> {
    if tokens.is_empty() || !(length > 0.0) || !(thickness > 0.0) {
        return Vec::new();
    }
    let n = tokens.len();
    let centers: Vec<f32> = tokens.iter().map(|token| token.center * length).collect();
    let mut bounds: Vec<(f32, f32)> = (0..n)
        .map(|i| {
            let prev = if i == 0 { 0.0 } else { centers[i - 1] };
            let next = if i + 1 == n { length } else { centers[i + 1] };
            let half = ((next - prev) / 2.0).min(thickness) / 2.0;
            ((centers[i] - half).max(0.0), (centers[i] + half).min(length))
        })
        .collect();
    for i in 1..n {
        if bounds[i].0 < bounds[i - 1].1 {
            let split = ((bounds[i - 1].1 + bounds[i].0) / 2.0).max(centers[i - 1]).min(centers[i]);
            bounds[i - 1].1 = split;
            bounds[i].0 = split;
        }
    }
    let mut spans = Vec::new();
    for (i, token) in tokens.iter().enumerate() {
        let (start, end) = bounds[i];
        let width = (end - start) / token.chars.max(1) as f32;
        for j in 0..token.chars {
            let (s, e) = (start + width * j as f32, start + width * (j + 1) as f32);
            let center = if token.chars == 1 { centers[i] } else { (s + e) / 2.0 };
            spans.push(CharSpan { start: s / length, center: center / length, end: e / length });
        }
    }
    spans
}

#[cfg(test)]
mod tests {
    use super::*;

    fn logits(frames: &[usize], classes: usize) -> Vec<f32> {
        let mut data = vec![0.0f32; frames.len() * classes];
        for (t, &best) in frames.iter().enumerate() {
            data[t * classes + best] = 0.9;
        }
        data
    }

    fn vocab(items: &[&str]) -> Vec<String> {
        items.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn decode_centers_each_item_on_its_frames() {
        let words = vocab(&["", "a", "b", "cd"]);
        let decoded = ctc_decode(&logits(&[0, 1, 1, 0, 2, 0, 0, 3, 3, 3], 4), 10, 4, &words);
        assert_eq!(decoded.text, "abcd");
        assert!((decoded.score - 0.9).abs() < 1e-6);
        assert_eq!(
            decoded.tokens,
            vec![Token { chars: 1, center: 0.2 }, Token { chars: 1, center: 0.45 }, Token { chars: 2, center: 0.85 }]
        );
    }

    #[test]
    fn decode_repeats_only_across_a_blank() {
        let words = vocab(&["", "a"]);
        let decoded = ctc_decode(&logits(&[1, 1, 0, 1], 2), 4, 2, &words);
        assert_eq!(decoded.text, "aa");
        assert_eq!(decoded.tokens.len(), 2);
        assert!((decoded.tokens[0].center - 0.25).abs() < 1e-6);
        assert!((decoded.tokens[1].center - 0.875).abs() < 1e-6);
    }

    #[test]
    fn decode_skips_classes_outside_the_vocabulary() {
        let decoded = ctc_decode(&logits(&[3, 1], 4), 2, 4, &vocab(&["", "a"]));
        assert_eq!(decoded.text, "a");
        assert_eq!(decoded.tokens, vec![Token { chars: 1, center: 0.75 }]);
    }

    #[test]
    fn spans_follow_the_neighbouring_centers() {
        let tokens = [Token { chars: 1, center: 0.2 }, Token { chars: 1, center: 0.45 }];
        let spans = char_spans(&tokens, 100.0, 30.0);
        // 字宽 22.5 与 40（封顶为行高 30），重叠的 [30, 31.25] 在 30.625 处分开。
        let expected = [(0.0875, 0.2, 0.30625), (0.30625, 0.45, 0.6)];
        assert_eq!(spans.len(), 2);
        for (span, (start, center, end)) in spans.iter().zip(expected) {
            assert!((span.start - start).abs() < 1e-5 && (span.center - center).abs() < 1e-5 && (span.end - end).abs() < 1e-5, "{span:?}");
        }
    }

    #[test]
    fn spans_stay_around_their_centers_when_spacing_is_uneven() {
        // 第三项后面间隔很大，它的半宽会伸过前一项的中心；分界仍要落在两个中心之间。
        let tokens = [Token { chars: 1, center: 0.09 }, Token { chars: 1, center: 0.10 }, Token { chars: 1, center: 0.11 }, Token { chars: 1, center: 1.0 - 0.005 }];
        let spans = char_spans(&tokens, 1000.0, 200.0);
        for (i, span) in spans.iter().enumerate() {
            assert!(span.start <= span.center && span.center <= span.end, "{i}: {span:?}");
            assert!(span.start >= 0.0 && span.end <= 1.0, "{i}: {span:?}");
        }
        for pair in spans.windows(2) {
            assert!(pair[0].end <= pair[1].start + 1e-6, "{pair:?}");
        }
    }

    #[test]
    fn spans_split_items_with_several_characters() {
        let spans = char_spans(&[Token { chars: 2, center: 0.5 }], 100.0, 100.0);
        // 字宽 min(100/2, 100) = 50，区间 [25, 75]，两个字符各占一半。
        assert_eq!(spans.len(), 2);
        assert!((spans[0].start - 0.25).abs() < 1e-6 && (spans[0].end - 0.5).abs() < 1e-6 && (spans[0].center - 0.375).abs() < 1e-6);
        assert!((spans[1].start - 0.5).abs() < 1e-6 && (spans[1].end - 0.75).abs() < 1e-6);
    }

    #[test]
    fn spans_need_a_real_line() {
        let token = [Token { chars: 1, center: 0.5 }];
        assert!(char_spans(&token, 0.0, 10.0).is_empty());
        assert!(char_spans(&token, 10.0, 0.0).is_empty());
        assert!(char_spans(&[], 10.0, 10.0).is_empty());
    }

    #[test]
    fn tilted_boxes_are_scored_by_the_pixels_inside() {
        // 37 ≤ x+y ≤ 43、|x−y| ≤ 30 的斜带，框正好是它；外接矩形里大半在带外。
        let (w, h) = (40usize, 40usize);
        let in_band = |i: usize| {
            let (x, y) = ((i % w) as i32, (i / w) as i32);
            (37..=43).contains(&(x + y)) && (x - y).abs() <= 30
        };
        let band: Vec<f32> = (0..w * h).map(|i| if in_band(i) { 1.0 } else { 0.0 }).collect();
        let outside: Vec<f32> = band.iter().map(|v| 1.0 - v).collect();
        let quad = [[3.5, 33.5], [33.5, 3.5], [36.5, 6.5], [6.5, 36.5]];
        assert_eq!(box_score(&band, w, h, &quad), 1.0);
        assert_eq!(box_score(&outside, w, h, &quad), 0.0);
    }

    fn canvas(w: usize, h: usize) -> Canvas {
        Canvas { w, h, left: 0, top: 0 }
    }

    #[test]
    fn the_canvas_pads_both_sides_to_multiples_of_32() {
        // 1080 → 1088、9000 → 9024，多出的 8 和 24 两侧各半。
        assert_eq!(det_canvas(1080, 9000), Canvas { w: 1088, h: 9024, left: 4, top: 12 });
        assert_eq!(det_canvas(1920, 1080), Canvas { w: 1920, h: 1088, left: 0, top: 4 });
        // 不足 128 的补到 128；奇数的差额多的一半在右、下。
        assert_eq!(det_canvas(100, 50), Canvas { w: 128, h: 128, left: 14, top: 39 });
        assert_eq!(det_canvas(129, 161), Canvas { w: 160, h: 192, left: 15, top: 15 });
        assert_eq!(det_canvas(2560, 1440), canvas(2560, 1440));
    }

    #[test]
    fn the_border_takes_the_mean_of_the_corners() {
        // 3×2：四角是黑、红、绿、蓝，中间两个白像素不参与。
        let data = [[0, 0, 0], [255, 255, 255], [255, 0, 0], [0, 255, 0], [255, 255, 255], [0, 0, 255]].concat();
        // 每个通道 255/4 = 63.75，取 64。
        assert_eq!(border_color(&Img { w: 3, h: 2, data }), [64, 64, 64]);
        assert_eq!(border_color(&Img { w: 1, h: 1, data: vec![10, 20, 30] }), [10, 20, 30]);
    }

    #[test]
    fn spans_merge_a_short_rest_into_the_last_one() {
        // 段长 960、起点每次进 768；剩余不足 1.3×960 = 1248 时整个作为最后一段。
        assert_eq!(det_spans(1216), vec![(0, 1216)]);
        assert_eq!(det_spans(1248), vec![(0, 960), (768, 480)]);
        assert_eq!(det_spans(1920), vec![(0, 960), (768, 1152)]);
        // 2016：起点 768 处正好剩 1248，仍切 960，剩下的 480 成最后一段。
        assert_eq!(det_spans(2016), vec![(0, 960), (768, 960), (1536, 480)]);
        assert_eq!(det_spans(3840), vec![(0, 960), (768, 960), (1536, 960), (2304, 960), (3072, 768)]);
        // 9024：起点 7680 处剩 1344，切 960；8448 处剩 576，成最后一段。
        let tall = det_spans(9024);
        assert_eq!(tall.len(), 12);
        assert_eq!(tall[10], (7680, 960));
        assert_eq!(tall[11], (8448, 576));
    }

    #[test]
    fn only_a_side_of_1440_or_more_splits_the_image() {
        assert_eq!(det_tiles(canvas(1408, 1408)), vec![Tile { x: 0, y: 0, w: 1408, h: 1408 }]);
        assert_eq!(det_tiles(canvas(1920, 1088)), vec![Tile { x: 0, y: 0, w: 960, h: 1088 }, Tile { x: 768, y: 0, w: 1152, h: 1088 }]);
        // 有一边分块时另一边也照同样的规则切：1280 ≥ 1248，切成两段。
        assert_eq!(det_tiles(canvas(1280, 1440)), vec![
            Tile { x: 0, y: 0, w: 960, h: 960 },
            Tile { x: 768, y: 0, w: 512, h: 960 },
            Tile { x: 0, y: 768, w: 960, h: 672 },
            Tile { x: 768, y: 768, w: 512, h: 672 },
        ]);
        let long = det_tiles(canvas(1088, 9024));
        assert_eq!(long.len(), 12);
        assert!(long.iter().all(|tile| tile.x == 0 && tile.w == 1088));
    }

    #[test]
    fn tiles_cover_the_canvas_and_overlap_by_192() {
        for length in (128..=12000).step_by(32) {
            let spans = det_spans(length);
            assert_eq!(spans[0].0, 0, "{length}");
            let (last_start, last_len) = *spans.last().unwrap();
            assert_eq!(last_start + last_len, length, "{length}");
            assert!(spans.iter().all(|&(start, len)| start % 32 == 0 && len % 32 == 0 && len > 0), "{length}: {spans:?}");
            for pair in spans.windows(2) {
                assert_eq!(pair[0].1, 960, "{length}: {spans:?}");
                assert_eq!(pair[0].0 + 960 - pair[1].0, 192, "{length}: {spans:?}");
            }
            if spans.len() > 1 {
                assert!((480..1248).contains(&last_len), "{length}: {spans:?}");
            } else {
                assert!(length < 1248, "{length}");
            }
        }
    }

    fn close(got: [f32; 3], expected: [f32; 3]) -> bool {
        got.iter().zip(expected).all(|(a, b)| (a - b).abs() < 1e-4)
    }

    #[test]
    fn the_input_places_the_image_inside_the_border() {
        // 2×1 的图补成 128×128，原图左上角在 (63, 63)。
        let img = Img { w: 2, h: 1, data: vec![255, 0, 0, 0, 0, 255] };
        let padded = det_canvas(2, 1);
        assert_eq!(padded, Canvas { w: 128, h: 128, left: 63, top: 63 });
        let input = det_input(&img, [255, 255, 255], padded, Tile { x: 0, y: 0, w: 128, h: 128 });
        let plane = 128 * 128;
        let bgr = |x: usize, y: usize| [input[y * 128 + x], input[plane + y * 128 + x], input[2 * plane + y * 128 + x]];
        // B、G、R 依次减 0.485、0.456、0.406，再除以 0.229、0.224、0.225：0 和 255 分别得到
        // B −2.1179 / 2.2489，G −2.0357 / 2.4286，R −1.8044 / 2.6400。
        let (red, blue, white) = ([-2.1179, -2.0357, 2.64], [2.2489, -2.0357, -1.8044], [2.2489, 2.4286, 2.64]);
        assert!(close(bgr(63, 63), red), "{:?}", bgr(63, 63));
        assert!(close(bgr(64, 63), blue), "{:?}", bgr(64, 63));
        for (x, y) in [(0, 0), (65, 63), (63, 64), (62, 63)] {
            assert!(close(bgr(x, y), white), "({x}, {y}): {:?}", bgr(x, y));
        }
        // 不从原点起的块：块内 (0, 0) 是整图 (62, 63)，落在补边里；(1, 0) 是原图的红像素。
        let part = det_input(&img, [0, 0, 128], padded, Tile { x: 62, y: 63, w: 32, h: 32 });
        let plane = 32 * 32;
        // 补边的 B = 128：(128/255 − 0.485) / 0.229 = 0.0741。
        assert!(close([part[0], part[plane], part[2 * plane]], [0.0741, -2.0357, -1.8044]));
        assert!(close([part[1], part[plane + 1], part[2 * plane + 1]], red));
    }

    #[test]
    fn overlapping_tiles_cross_fade_and_their_weights_add_up_to_one() {
        // 每块都全是 1 时，拼出来处处是 1：权重在重叠里相加为 1，补边部分不落进原图。
        for (w, h) in [(1920, 1080), (3830, 2170), (1080, 9000), (1300, 1440), (900, 700)] {
            let padded = det_canvas(w, h);
            let mut prob = vec![0f32; w * h];
            for tile in det_tiles(padded) {
                paste_blend(&mut prob, w, h, padded, tile, &vec![1.0; tile.w * tile.h]);
            }
            let worst = prob.iter().map(|v| (v - 1.0).abs()).fold(0.0f32, f32::max);
            assert!(worst < 1e-4, "{w}x{h}: {worst}");
        }
        // 1920×1088 分成 x 0..960（全是 0.2）和 768..1920（全是 0.6），重叠 768..960。
        // 前一块的权重 (960 − x − 0.5)/192，后一块 (x − 768 + 0.5)/192。
        let padded = canvas(1920, 1088);
        let tiles = det_tiles(padded);
        let mut prob = vec![0f32; 1920 * 1088];
        for (tile, value) in tiles.iter().zip([0.2f32, 0.6]) {
            paste_blend(&mut prob, 1920, 1088, padded, *tile, &vec![value; tile.w * tile.h]);
        }
        // x = 864：0.2 × 95.5/192 + 0.6 × 96.5/192 = 0.401042；768：0.201042；959：0.598958。
        for (x, expected) in [(100, 0.2), (767, 0.2), (768, 0.201042), (864, 0.401042), (959, 0.598958), (960, 0.6), (1500, 0.6)] {
            for y in [0, 500, 1087] {
                assert!((prob[y * 1920 + x] - expected).abs() < 1e-5, "({x}, {y}): {}", prob[y * 1920 + x]);
            }
        }
    }

    #[test]
    fn probability_far_from_any_contrast_is_cleared() {
        // 200×100 的白图（13×7 格），只有一个黑点；离它所在的格子 3 格（48 像素）以内保留。
        let (w, h) = (200, 100);
        let dotted = |x: usize, y: usize| {
            let mut data = vec![255u8; w * h * 3];
            data[(y * w + x) * 3..(y * w + x) * 3 + 3].fill(0);
            Img { w, h, data }
        };
        let cleared = |img: &Img| {
            let mut prob = vec![1.0f32; w * h];
            clear_flat(&mut prob, img);
            prob
        };
        // 点在 (10, 10)，格 (0, 0)：x、y < 64 的保留。
        let prob = cleared(&dotted(10, 10));
        for (x, y, kept) in [(63, 63, true), (0, 0, true), (64, 10, false), (10, 64, false), (199, 99, false)] {
            assert_eq!(prob[y * w + x] == 1.0, kept, "({x}, {y})");
        }
        // 点在 (150, 90)，格 (9, 5)：x ≥ 96、y ≥ 32 的保留。
        let prob = cleared(&dotted(150, 90));
        for (x, y, kept) in [(96, 50, true), (95, 50, false), (150, 32, true), (150, 31, false), (199, 99, true)] {
            assert_eq!(prob[y * w + x] == 1.0, kept, "({x}, {y})");
        }
        // 上白下黑、只有上下之间有变化：分界在 y = 99|100，格行 6，y 在 48..160 的保留。
        let (w2, h2) = (40, 200);
        let mut halves = vec![255u8; w2 * h2 * 3];
        halves[w2 * 100 * 3..].fill(0);
        let mut prob = vec![1.0f32; w2 * h2];
        clear_flat(&mut prob, &Img { w: w2, h: h2, data: halves });
        for (y, kept) in [(47, false), (48, true), (159, true), (160, false)] {
            assert_eq!(prob[y * w2 + 10] == 1.0, kept, "y {y}");
        }
        // 一点变化都没有就全清掉；亮度差不到 8 的不算变化。
        assert!(cleared(&Img { w, h, data: vec![200; w * h * 3] }).iter().all(|&v| v == 0.0));
        let mut faint = vec![200u8; w * h * 3];
        faint[(10 * w + 10) * 3..(10 * w + 10) * 3 + 3].fill(193);
        assert!(cleared(&Img { w, h, data: faint }).iter().all(|&v| v == 0.0));
    }

    #[test]
    fn short_image_data_is_an_error() {
        assert!(Img::from_raw_rgb(&[0; 11], 2, 2, 6).is_err());
        assert!(Img::from_raw_rgb(&[0; 12], 2, 2, 5).is_err());
        // 末行不必有 stride 的余量。
        assert!(Img::from_raw_rgb(&[0; 14], 2, 2, 8).is_ok());
    }

    #[test]
    fn vertical_crops_are_turned() {
        let img = Img { w: 20, h: 100, data: vec![255; 20 * 100 * 3] };
        let (tall, vertical) = crop_quad(&img, &[[0.0, 0.0], [10.0, 0.0], [10.0, 60.0], [0.0, 60.0]]);
        assert!(vertical);
        assert!(tall.w > tall.h);
        let (_, horizontal) = crop_quad(&img, &[[0.0, 0.0], [19.0, 0.0], [19.0, 10.0], [0.0, 10.0]]);
        assert!(!horizontal);
    }
}
