//! 增量拼接会话：逐帧推入截图，会话内只保留拼接结果的像素和逐行哈希。
//!
//! 匹配与 `stitch` 模块的双图拼接共用 `search_range` / `overlap_candidates`，每帧只处理新帧本身：
//! 已拼好的长图不再编解码，行哈希也只算新帧。
//! 会话记着最新一帧在结果里的位置，下一帧先在它附近找，回滚到哪都能定位。
//! 帧落在已拼内容之内只更新位置；伸出结果开头或末尾，就在那一头接上，两个方向都能长。
//! 每行算两种哈希，先按逐像素指纹找位置，找不到再按平均色哈希找（见 `RowHashes`）；
//! 位置要有能单独定位的行支撑，只零星对上一小段时视为不重叠，不硬接。
//! 输入输出用 BGRA（Windows 截屏的原生布局），内部按 RGBA 存放以复用行哈希。

use std::collections::HashMap;

use rayon::prelude::*;

use crate::error::StitchError;
use crate::hash::{compute_row_fingerprints_raw, compute_row_hashes_raw};
use crate::stitch::{overlap_candidates, search_range};

/// 这一帧之前的滚动方向（往下即结果行号增大），用来在几个候选之间取舍。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Heading {
    Down,
    Up,
}

/// 一帧推入后的结论。
///
/// 伸出开头：结果先去掉顶部 `head_cut` 行，再把新帧前 `head_rows` 行接在最前面；
/// 伸出末尾：结果截到 `keep` 行，再接上新帧从 `skip` 行开始的部分；
/// 落在已拼内容之内：结果不变，`keep` 为原高度、`skip` 为帧高。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PushOutcome {
    pub head_cut: usize,
    pub head_rows: usize,
    pub keep: usize,
    pub skip: usize,
    /// 这一帧第 0 行在更新后的结果里的行号。
    pub top: usize,
}

/// 一次推入要找的位置：新帧的行哈希和取舍条件。
#[derive(Clone, Copy)]
struct Query<'a> {
    frame_rows: &'a RowHashes,
    /// 这一帧顶部、底部固定栏的行数，不参与定位。
    bars: (usize, usize),
    heading: Option<Heading>,
    ignore_img1_top_ratio: f32,
    ignore_img1_bottom_ratio: f32,
    debug: bool,
}

/// 一个重叠候选：结果第 `start_i` 行起与帧第 `start_j` 行起连续 `len` 行相同。
#[derive(Clone, Copy, Debug)]
struct Placement {
    start_i: usize,
    start_j: usize,
    len: usize,
}

impl Placement {
    /// 帧第 0 行对应的结果行号；帧伸出结果开头时为负。
    fn top(&self) -> isize {
        self.start_i as isize - self.start_j as isize
    }
}

/// 重叠里至少要有这么多行能单独定位（在搜索范围和新帧里都只出现一次）才算可信。
/// 空白行、一列条目里相同的部分到处都对得上，不能当作位置的证据。
const CREDIBLE_UNIQUE_ROWS: usize = 8;

/// 每次搜索最多看这么多个候选。几乎一样的条目、段落间的空白都能凑出许多段假匹配，候选少了真正的重叠会被挤掉。
const CANDIDATES: usize = 32;

/// 不够可信的位置至少要有这么多能单独定位的行才接受。没有这样的行时，只有占这一帧四分之一以上的长段
/// （条目完全重复、大片空白）才按方向取：几乎一样的条目里各条目相同的部分、段落之间的空白，
/// 贴着结果边缘都能凑出一小段，这一帧和已拼内容根本不重叠时也对得上。
const WEAK_UNIQUE_ROWS: usize = 2;

/// 重叠区里一小块对不上（光标悬停的高亮、闪烁的光标）会把匹配段截断。长图这一头至少这么多行与新帧对得上、
/// 且不是一片纯色时，说明这一头本来就对，原有的行不换；纯色的固定栏错开几行也对得上，所以要看不同的行数。
const EDGE_MATCH_ROWS: usize = 16;
const EDGE_DISTINCT_ROWS: usize = 4;

/// 行哈希忽略右侧的宽度至少为帧宽的这几分之一：截图区比滚动条宽出一截时，滚动条落在常规的右侧忽略区之外，
/// 滑块一动指纹就对不上，平均色也会跟着变，在几乎一样的条目里凑出假的「只出现一次」的行。
const IGNORE_RIGHT_DIVISOR: u32 = 10;

fn distinct_rows(hashes: &[u64]) -> usize {
    let mut sorted = hashes.to_vec();
    sorted.sort_unstable();
    sorted.dedup();
    sorted.len()
}

fn occurrences(hashes: &[u64]) -> HashMap<u64, u32> {
    let mut counts = HashMap::with_capacity(hashes.len());
    for &h in hashes {
        *counts.entry(h).or_insert(0) += 1;
    }
    counts
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum HashKind {
    Exact,
    Coarse,
}

/// 每行的两种哈希。`exact` 是逐像素指纹，只差几个字的行（文件列表、表格）也分得开，但渲染上的细微差别也会让它对不上；
/// `coarse` 是整行平均色量化，容得下这些差别，几乎一样的行却会算成同一行。
#[derive(Default)]
struct RowHashes {
    exact: Vec<u64>,
    coarse: Vec<u64>,
}

impl RowHashes {
    fn len(&self) -> usize {
        self.coarse.len()
    }

    fn of(&self, kind: HashKind) -> &[u64] {
        match kind {
            HashKind::Exact => &self.exact,
            HashKind::Coarse => &self.coarse,
        }
    }

    fn drop_head(&mut self, cut: usize) {
        self.exact.drain(..cut);
        self.coarse.drain(..cut);
    }

    fn truncate(&mut self, keep: usize) {
        self.exact.truncate(keep);
        self.coarse.truncate(keep);
    }

    /// 把 `frame` 的前 `rows` 行接在最前面。
    fn prepend(&mut self, frame: &RowHashes, rows: usize) {
        self.exact.splice(0..0, frame.exact[..rows].iter().copied());
        self.coarse.splice(0..0, frame.coarse[..rows].iter().copied());
    }

    /// 接上 `frame` 从 `skip` 行开始的部分。
    fn append(&mut self, frame: &RowHashes, skip: usize) {
        self.exact.extend_from_slice(&frame.exact[skip..]);
        self.coarse.extend_from_slice(&frame.coarse[skip..]);
    }
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
    rows: RowHashes,
    /// 最新一帧第 0 行在结果里的行号。
    position: usize,
    /// 最近一次位置变化的方向。调用方不知道这一帧的滚动方向时（手动截图、键盘翻页）按它取舍。
    last_move: Option<Heading>,
    /// 上一次推入的帧的逐像素指纹（不论接没接上），用来认出固定栏。
    last_frame: Vec<u64>,
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
            rows: RowHashes::default(),
            position: 0,
            last_move: None,
            last_frame: Vec::new(),
        })
    }

    pub fn width(&self) -> u32 {
        self.width
    }

    /// 当前拼接结果的行数，尚未推入任何帧时为 0。
    pub fn height(&self) -> usize {
        self.rows.len()
    }

    /// 最近一次位置变化的方向；还没移动过时为 `None`。
    pub fn last_move(&self) -> Option<Heading> {
        self.last_move
    }

    fn stride(&self) -> usize {
        self.width as usize * 4
    }

    /// 推入一帧 BGRA 截图。第一帧只作为起点；无重叠时返回 `NoOverlap`，会话保持不变。
    ///
    /// `heading` 是这一帧之前的滚动方向，不知道时传 `None`，按最近一次位置变化的方向取舍。
    pub fn push_bgra(
        &mut self,
        bgra: &[u8],
        heading: Option<Heading>,
        ignore_img1_top_ratio: f32,
        ignore_img1_bottom_ratio: f32,
        debug: bool,
    ) -> Result<PushOutcome, StitchError> {
        let frame = self.bgra_to_rgba(bgra)?;
        let frame_rows = self.row_hashes(&frame);

        if self.rows.len() == 0 {
            self.segments = vec![frame];
            self.last_frame = frame_rows.exact.clone();
            self.rows = frame_rows;
            self.position = 0;
            self.last_move = None;
            return Ok(PushOutcome { head_cut: 0, head_rows: 0, keep: 0, skip: 0, top: 0 });
        }

        let located = self.locate(&Query {
            frame_rows: &frame_rows,
            bars: self.fixed_bars(&frame_rows.exact),
            heading: heading.or(self.last_move),
            ignore_img1_top_ratio,
            ignore_img1_bottom_ratio,
            debug,
        });
        self.last_frame = frame_rows.exact.clone();
        let (placement, kind) = located?;
        Ok(self.commit(placement, kind, &frame, &frame_rows))
    }

    /// 与上一帧同一位置完全相同的顶部、底部连续行数：内容滚动了而这些行没动，是固定的标题栏、底栏。
    /// 它们和结果两头的固定栏总对得上，拿来定位会把一帧摆到结果的开头或末尾。
    /// 两帧完全一样（没滚动），或两头加起来占了一半以上（剩下的太少）时不算。
    fn fixed_bars(&self, frame: &[u64]) -> (usize, usize) {
        let last = &self.last_frame;
        if last.len() != frame.len() || last.as_slice() == frame {
            return (0, 0);
        }
        let top = frame.iter().zip(last).take_while(|(a, b)| a == b).count();
        let bottom = frame.iter().rev().zip(last.iter().rev()).take_while(|(a, b)| a == b).count();
        if (top + bottom) * 2 >= frame.len() {
            return (0, 0);
        }
        (top, bottom)
    }

    /// 当前拼接结果的 BGRA 字节数。
    pub fn byte_len(&self) -> usize {
        self.rows.len() * self.stride()
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

    /// 去掉最新一帧顶边以上的内容，结果从这一帧开始；返回去掉的行数。
    pub fn crop_top(&mut self) -> usize {
        let cut = self.position.min(self.rows.len());
        self.drop_head(cut);
        self.position = 0;
        cut
    }

    /// 去掉最新一帧底边以下的内容，结果到这一帧为止；返回去掉的行数。
    pub fn crop_bottom(&mut self) -> usize {
        let len = self.rows.len();
        let keep = (self.position + self.frame_height as usize).min(len);
        self.truncate(keep);
        len - keep
    }

    /// 立即释放拼接结果占用的内存，会话回到未推入任何帧的状态。
    pub fn clear(&mut self) {
        self.segments = Vec::new();
        self.rows = RowHashes::default();
        self.position = 0;
        self.last_move = None;
        self.last_frame = Vec::new();
    }

    fn row_hashes(&self, frame: &[u8]) -> RowHashes {
        let ignore = self.ignore_right_pixels.max(self.width / IGNORE_RIGHT_DIVISOR);
        RowHashes {
            exact: compute_row_fingerprints_raw(frame, self.width, 0, self.frame_height, ignore),
            coarse: compute_row_hashes_raw(frame, self.width, 0, self.frame_height, ignore),
        }
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

    /// 找出这一帧在结果里的位置：先按逐像素指纹找；渲染上有细微差别时指纹对不上，再按平均色哈希找，
    /// 但平均色哈希分不清几乎一样的行，只接受有能单独定位的行支撑的位置。都找不到就视为不重叠。
    fn locate(&self, query: &Query) -> Result<(Placement, HashKind), StitchError> {
        if let Some(p) = self.locate_by(HashKind::Exact, query) {
            return Ok((p, HashKind::Exact));
        }
        self.locate_by(HashKind::Coarse, query)
            .map(|p| (p, HashKind::Coarse))
            .ok_or(StitchError::NoOverlap)
    }

    /// 按一种行哈希找位置：先在上一帧附近找；附近没有站得住的位置（比如一下跳得很远，如 Home/End），再全图找。
    /// 按取舍顺序依次看候选，接受第一个有足够多能单独定位的行支撑的。逐像素指纹还接受不够可信、但匹配段占了这一帧
    /// 与结果重合部分一半以上的（见 `WEAK_UNIQUE_ROWS`）：重叠本来就短、条目完全重复、大片空白时只能这样定；
    /// 只零星对上一小段，多半是这一帧和已拼内容根本不重叠。平均色哈希分不清几乎一样的行，不这样凑合。
    fn locate_by(&self, kind: HashKind, query: &Query) -> Option<Placement> {
        let exact = kind == HashKind::Exact;
        let result = self.rows.of(kind);
        let hash_start = (self.ignore_top_pixels as usize).max(query.bars.0).min(self.frame_height as usize);
        let hash_end = (self.frame_height as usize - query.bars.1).max(hash_start);
        let img2_hashes = &query.frame_rows.of(kind)[hash_start..hash_end];
        let img1_len = result.len();
        let frame_counts = occurrences(img2_hashes);
        // 帧本身能定位的行就少（大片空白）、或重叠本来就短时，门槛随之降低
        let unique_in_frame = frame_counts.values().filter(|&&c| c == 1).count();
        let needed = CREDIBLE_UNIQUE_ROWS.min((unique_in_frame / 2).max(1));
        let near = window_around(self.position, self.frame_height as usize, img2_hashes.len(), img1_len);
        let search = |window| {
            let (start, end) = search_range(
                window,
                img1_len,
                self.frame_height as usize,
                query.ignore_img1_top_ratio,
                query.ignore_img1_bottom_ratio,
            );
            let candidates: Vec<Placement> = overlap_candidates(
                &result[start..end],
                start,
                img2_hashes,
                hash_start,
                self.min_overlap_ratio,
                CANDIDATES,
            )
            .into_iter()
            .map(|(start_i, start_j, len)| Placement { start_i, start_j, len })
            .collect();
            let range_counts = occurrences(&result[start..end]);
            let acceptable = |p: &Placement| {
                let unique = (0..p.len)
                    .filter(|&k| {
                        range_counts[&result[p.start_i + k]] == 1
                            && frame_counts[&img2_hashes[p.start_j - hash_start + k]] == 1
                    })
                    .count();
                // 平均色哈希撞得多，碰巧只出现一次的几行说明不了位置，门槛不随帧和段长放宽
                let enough = if exact { needed.min((p.len / 2).max(1)) } else { CREDIBLE_UNIQUE_ROWS };
                unique >= enough || exact && {
                    // 这一帧参与定位的行里落在结果之内的那部分
                    let top = p.top();
                    let first = (hash_start as isize).max(-top);
                    let last = (hash_end as isize).min(img1_len as isize - top);
                    let guess = p.len * 4 >= img2_hashes.len();
                    p.len * 2 >= (last - first).max(0) as usize && (unique >= WEAK_UNIQUE_ROWS || guess)
                }
            };
            let chosen = self.preference(&candidates, query.heading).into_iter().find(|p| acceptable(p));
            if query.debug {
                println!("  🔍 {kind:?}: 在结果 [{start}:{end}) 里找，候选 {candidates:?}，接受 {chosen:?}");
            }
            chosen
        };
        search(near).or_else(|| if near == (0, img1_len) { None } else { search((0, img1_len)) })
    }

    /// 候选的取舍顺序：先按长度排位置挪向滚动方向那一边、且不比最长候选短五倍以上的，再按长度排其余的；
    /// 依次看，接受第一个站得住的。往下滚且上一帧贴着结果末尾时，这与双图拼接"挑能让结果变长的"是同一回事。
    /// 完全不知道方向（第一对帧又没有提示）时两个方向一样可能，只按长度排。
    fn preference(&self, candidates: &[Placement], heading: Option<Heading>) -> Vec<Placement> {
        let Some(longest) = candidates.first().map(|p| p.len) else {
            return Vec::new();
        };
        let position = self.position as isize;
        let (mut preferred, rest): (Vec<Placement>, Vec<Placement>) = candidates.iter().partition(|p| {
            let along = match heading {
                Some(Heading::Down) => p.top() >= position,
                Some(Heading::Up) => p.top() <= position,
                None => true,
            };
            along && longest <= p.len * 5
        });
        preferred.extend(rest);
        preferred
    }

    /// 帧伸出结果开头或末尾，就在那一头接上，顺带替换掉新帧也覆盖到的旧标题栏、旧底栏。
    /// 落在已拼内容之内（回滚、没滚动）只记位置，结果不动。`kind` 是找到这个位置所用的行哈希。
    fn commit(&mut self, placement: Placement, kind: HashKind, frame: &[u8], frame_rows: &RowHashes) -> PushOutcome {
        let len = self.height();
        let frame_height = self.frame_height as usize;
        let top = placement.top();
        let previous = self.position as isize;
        if top != previous {
            self.last_move = Some(if top > previous { Heading::Down } else { Heading::Up });
        }
        if top < 0 {
            // 开头对得上时只接上新露出的部分；对不上的多半是旧的标题栏，换成新帧的
            let (cut, rows) = if self.edge_holds(kind, frame_rows, top, false) {
                (0, (-top) as usize)
            } else if let Some(from) = self.agreement_from_edge(kind, frame_rows, top, false) {
                (from, (from as isize - top) as usize)
            } else {
                (placement.start_i, placement.start_j)
            };
            self.prepend(cut, rows, frame, frame_rows);
            self.position = 0;
            PushOutcome { head_cut: cut, head_rows: rows, keep: self.height(), skip: frame_height, top: 0 }
        } else if top as usize + frame_height > len {
            // 末尾对得上时只接上新露出的部分；对不上的多半是旧的底栏，换成新帧的
            let (keep, skip) = if self.edge_holds(kind, frame_rows, top, true) {
                (len, len - top as usize)
            } else if let Some(to) = self.agreement_from_edge(kind, frame_rows, top, true) {
                (to, (to as isize - top) as usize)
            } else {
                (placement.start_i + placement.len, placement.start_j + placement.len)
            };
            self.apply(keep, skip, frame, frame_rows);
            self.position = top as usize;
            PushOutcome { head_cut: 0, head_rows: 0, keep, skip, top: self.position }
        } else {
            self.position = top as usize;
            PushOutcome { head_cut: 0, head_rows: 0, keep: len, skip: frame_height, top: self.position }
        }
    }

    /// 帧放在 `top` 处时，结果的末尾（`at_end`）或开头是否连续一段与帧对得上、且不是一片纯色。
    fn edge_holds(&self, kind: HashKind, frame_rows: &RowHashes, top: isize, at_end: bool) -> bool {
        let hashes = self.rows.of(kind);
        let frame_hashes = frame_rows.of(kind);
        let len = hashes.len() as isize;
        let height = frame_hashes.len() as isize;
        let mut count = 0isize;
        loop {
            let row = if at_end { len - 1 - count } else { count };
            let frame_row = row - top;
            if row < 0 || row >= len || frame_row < 0 || frame_row >= height
                || hashes[row as usize] != frame_hashes[frame_row as usize]
            {
                break;
            }
            count += 1;
        }
        let count = count as usize;
        if count < EDGE_MATCH_ROWS {
            return false;
        }
        let len = len as usize;
        let rows = if at_end { &hashes[len - count..] } else { &hashes[..count] };
        distinct_rows(rows) >= EDGE_DISTINCT_ROWS
    }

    /// 结果这一头与帧对不上时，从这一头往里找第一段按 `edge_holds` 的标准对得上的行，在那里接：
    /// 跳过的是旧固定栏，而悬停高亮这类局部差别若把匹配段截成两截，接缝不会落到它的另一侧。
    /// 返回接缝在结果里的行号（开头为那段的起点，末尾为终点），找不到返回 None。
    fn agreement_from_edge(&self, kind: HashKind, frame_rows: &RowHashes, top: isize, at_end: bool) -> Option<usize> {
        let hashes = self.rows.of(kind);
        let frame_hashes = frame_rows.of(kind);
        let window = EDGE_MATCH_ROWS as isize;
        let lo = top.max(0);
        let hi = (top + frame_hashes.len() as isize).min(hashes.len() as isize);
        let holds = |from: isize| {
            (from..from + window).all(|row| hashes[row as usize] == frame_hashes[(row - top) as usize])
                && distinct_rows(&hashes[from as usize..(from + window) as usize]) >= EDGE_DISTINCT_ROWS
        };
        if at_end {
            (lo..=hi - window).rev().find(|&from| holds(from)).map(|from| (from + window) as usize)
        } else {
            (lo..=hi - window).find(|&from| holds(from)).map(|from| from as usize)
        }
    }

    /// 去掉结果顶部 `cut` 行，再把帧的前 `rows` 行接在最前面。
    fn prepend(&mut self, cut: usize, rows: usize, frame: &[u8], frame_rows: &RowHashes) {
        self.drop_head(cut);
        if rows > 0 {
            self.segments.insert(0, frame[..rows * self.stride()].to_vec());
        }
        self.rows.prepend(frame_rows, rows);
    }

    /// 结果截到 `keep` 行，再接上帧从 `skip` 行开始的部分。
    fn apply(&mut self, keep: usize, skip: usize, frame: &[u8], frame_rows: &RowHashes) {
        self.truncate(keep);
        if skip < frame_rows.len() {
            self.segments.push(frame[skip * self.stride()..].to_vec());
            self.rows.append(frame_rows, skip);
        }
    }

    /// 去掉结果顶部 `cut` 行。
    fn drop_head(&mut self, cut: usize) {
        let stride = self.stride();
        let mut remaining = cut;
        while remaining > 0 {
            let first_rows = self.segments[0].len() / stride;
            if first_rows <= remaining {
                self.segments.remove(0);
                remaining -= first_rows;
            } else {
                self.segments[0].drain(..remaining * stride);
                remaining = 0;
            }
        }
        self.rows.drop_head(cut);
    }

    /// 结果截到 `keep` 行。
    fn truncate(&mut self, keep: usize) {
        let stride = self.stride();
        let mut rows = self.rows.len();
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
        self.rows.truncate(keep);
    }
}

/// 上一帧附近的搜索窗口：上一帧下沿往上两个 img2 高度、往下一个 img2 高度。
/// 正常往下滚时上一帧贴着结果末尾，窗口与 `stitch::tail_window` 相同。
fn window_around(position: usize, frame_height: usize, img2_len: usize, img1_len: usize) -> (usize, usize) {
    let bottom = (position + frame_height).min(img1_len);
    (bottom.saturating_sub(img2_len * 2), (bottom + img2_len).min(img1_len))
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{Rgba, RgbaImage};

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

    /// 每行颜色都不同且不成周期的页面，远距离定位不会被重复图案干扰。
    fn distinct_rows_page(height: u32) -> RgbaImage {
        RgbaImage::from_fn(W, height, |x, y| {
            let shade = |k: u32| (mix(y, k) & 0xFF) as u8 ^ (x / 40) as u8;
            Rgba([shade(7), shade(13), shade(29), 255])
        })
    }

    /// 每 `period` 行重复一次的页面（如一列相同的条目），好几个位置都对得上，只能靠方向取舍。
    fn repeating_page(height: u32, period: u32) -> RgbaImage {
        let block = distinct_rows_page(period);
        RgbaImage::from_fn(W, height, |x, y| *block.get_pixel(x, y % period))
    }

    fn frame(page: &RgbaImage, top: u32) -> RgbaImage {
        image::imageops::crop_imm(page, 0, top, W, H).to_image()
    }

    fn region(page: &RgbaImage, from: u32, to: u32) -> RgbaImage {
        image::imageops::crop_imm(page, 0, from, W, to - from).to_image()
    }

    fn to_bgra(img: &RgbaImage) -> Vec<u8> {
        img.as_raw().chunks_exact(4).flat_map(|p| [p[2], p[1], p[0], p[3]]).collect()
    }

    fn session() -> StitchSession {
        StitchSession::new(W, H, 20, 0, 0.01).unwrap()
    }

    fn exported(s: &StitchSession) -> RgbaImage {
        let rgba: Vec<u8> = s.export_bgra().chunks_exact(4).flat_map(|p| [p[2], p[1], p[0], p[3]]).collect();
        RgbaImage::from_raw(s.width(), s.height() as u32, rgba).unwrap()
    }

    const BAR: u32 = 12;

    fn paint_bar(img: &mut RgbaImage, from: u32) {
        for y in from..from + BAR {
            for x in 0..W {
                img.put_pixel(x, y, if x % 17 < 9 { Rgba([30, 60, 140, 255]) } else { Rgba([220, 220, 220, 255]) });
            }
        }
    }

    /// 盖住顶部的固定标题栏。
    fn header(img: &mut RgbaImage) {
        paint_bar(img, 0);
    }

    /// 盖住底部的固定底栏。
    fn footer(img: &mut RgbaImage) {
        let height = img.height();
        paint_bar(img, height - BAR);
    }

    /// 按真实滚动推入：方向取自相邻两帧的先后，检查每帧的位置和结果覆盖的范围。
    fn push_and_track(p: &RgbaImage, tops: &[u32]) -> StitchSession {
        push_and_track_with(p, tops, |_| {})
    }

    /// 同 `push_and_track`，每帧都由 `bars` 盖上固定栏；结果里固定栏只该在最外侧留一份。
    fn push_and_track_with(p: &RgbaImage, tops: &[u32], bars: impl Fn(&mut RgbaImage)) -> StitchSession {
        let mut s = session();
        let (mut lo, mut hi) = (tops[0], tops[0]);
        for (k, &t) in tops.iter().enumerate() {
            let heading = match k.checked_sub(1).map(|i| tops[i]) {
                Some(prev) if t > prev => Some(Heading::Down),
                Some(prev) if t < prev => Some(Heading::Up),
                _ => None,
            };
            // 往上长的那一头就是顶部，忽略得少一些，免得切掉本来就不长的重叠（与调用方的取法一致）
            let top_ratio = if heading == Some(Heading::Up) { 0.05 } else { 0.15 };
            let mut f = frame(p, t);
            bars(&mut f);
            let outcome = s.push_bgra(&to_bgra(&f), heading, top_ratio, 0.0, false).unwrap();
            lo = lo.min(t);
            hi = hi.max(t);
            assert_eq!(outcome.top, (t - lo) as usize, "frame at {t}");
            assert_eq!(s.height(), (hi - lo + H) as usize, "after frame at {t}");
        }
        let mut expected = region(p, lo, hi + H);
        bars(&mut expected);
        assert_eq!(exported(&s), expected);
        s
    }

    #[test]
    fn small_overlaps_are_found_and_a_gap_is_reported() {
        // 重叠只有十来行、大半是空白也要接对；600 之后一下滚过了一整屏，之后的帧都接不上，结果停在那里
        let p = page(900);
        let tops = [0, 37, 81, 81, 150, 204, 260, 311, 390, 455, 520, 600, 690, 760, 810];
        let mut s = session();
        for &t in &tops {
            let outcome = s.push_bgra(&to_bgra(&frame(&p, t)), None, 0.15, 0.0, false);
            if t <= 600 {
                assert_eq!(outcome.map(|o| o.top), Ok(t as usize), "frame at {t}");
            } else {
                assert_eq!(outcome, Err(StitchError::NoOverlap), "frame at {t}");
            }
        }
        assert_eq!(exported(&s), region(&p, 0, 690));
    }

    #[test]
    fn a_fixed_footer_stays_at_the_end() {
        // 每帧底部同一条底栏：长图末尾的旧底栏要被截掉，走分段截断
        let p = page(900);
        let tops: Vec<u32> = (0..18).map(|k| k * 40).collect();
        let s = push_and_track_with(&p, &tops, footer);
        assert!(s.height() > (H * 3) as usize);
    }

    const LW: u32 = 400;

    /// 条目只差几个数字的列表（文件列表、表格）：每 20 行一个条目。中间 6 行是各条目不同的数字，
    /// 墨点一样多、只是位置不同，整行平均色都相同；上下各 4 行是各条目都一样的部分（文件名前缀的笔画）。
    fn listing(height: u32) -> RgbaImage {
        RgbaImage::from_fn(LW, height, |x, y| {
            let row = y % 20;
            let ink = match row {
                2..=5 | 12..=15 => mix(row, x / 5) % 4 == 0,
                6..=11 => (20..260).contains(&x) && {
                    let value = y / 20 * 6 + row - 6;
                    let offset = (x - 20) % 20;
                    if value >> ((x - 20) / 20) & 1 == 1 { offset < 3 } else { (10..13).contains(&offset) }
                },
                _ => false,
            };
            if ink { Rgba([30, 30, 30, 255]) } else { Rgba([250, 250, 250, 255]) }
        })
    }

    /// 截一帧列表。截图区比滚动条宽出一截：滚动条离右边缘 24~30 像素，滑块随位置移动。
    fn listing_frame(page: &RgbaImage, top: u32) -> RgbaImage {
        let mut f = image::imageops::crop_imm(page, 0, top, LW, H).to_image();
        scrollbar(&mut f, top * (H - 30) / (page.height() - H));
        f
    }

    fn scrollbar(f: &mut RgbaImage, thumb: u32) {
        for y in 0..H {
            for x in LW - 30..LW - 24 {
                let on_thumb = (thumb..thumb + 30).contains(&y);
                f.put_pixel(x, y, if on_thumb { Rgba([120, 120, 120, 255]) } else { Rgba([235, 235, 235, 255]) });
            }
        }
    }

    #[test]
    fn a_scrollbar_near_the_right_edge_is_ignored() {
        // 内容相同、只有滑块位置不同的两帧，两种行哈希都一样
        let p = listing(1400);
        let s = StitchSession::new(LW, H, 20, 0, 0.01).unwrap();
        let shot = |thumb| {
            let mut f = image::imageops::crop_imm(&p, 0, 200, LW, H).to_image();
            scrollbar(&mut f, thumb);
            s.row_hashes(f.as_raw())
        };
        let (a, b) = (shot(0), shot(50));
        assert_eq!(a.exact, b.exact);
        assert_eq!(a.coarse, b.coarse);
    }

    /// 底部的固定横幅，每行图案不同（带文字的横幅）。
    fn banner(img: &mut RgbaImage) {
        let (width, height) = img.dimensions();
        for y in height - BAR..height {
            for x in 0..width {
                let ink = mix(y + BAR - height, x / 4) % 3 == 0;
                img.put_pixel(x, y, if ink { Rgba([90, 60, 10, 255]) } else { Rgba([255, 243, 205, 255]) });
            }
        }
    }

    fn heading_between(previous: Option<u32>, t: u32) -> Option<Heading> {
        match previous {
            Some(prev) if t > prev => Some(Heading::Down),
            Some(prev) if t < prev => Some(Heading::Up),
            _ => None,
        }
    }

    fn top_ratio(heading: Option<Heading>) -> f32 {
        if heading == Some(Heading::Up) { 0.05 } else { 0.15 }
    }

    #[test]
    fn near_identical_rows_are_told_apart() {
        let p = listing(1400);
        let tops = [0u32, 40, 80, 120, 80, 40, 80, 120, 160, 200, 240, 200, 280];
        let mut s = StitchSession::new(LW, H, 20, 0, 0.01).unwrap();
        for (k, &t) in tops.iter().enumerate() {
            let heading = heading_between(k.checked_sub(1).map(|i| tops[i]), t);
            let outcome = s.push_bgra(&to_bgra(&listing_frame(&p, t)), heading, top_ratio(heading), 0.0, false);
            assert_eq!(outcome.map(|o| o.top), Ok(t as usize), "frame at {t}");
        }
        // 滚动条那一条每帧都不一样，比较时去掉
        let without_scrollbar = |img: &RgbaImage| image::imageops::crop_imm(img, 0, 0, LW - 40, img.height()).to_image();
        let expected = image::imageops::crop_imm(&p, 0, 0, LW, 280 + H).to_image();
        assert_eq!(without_scrollbar(&exported(&s)), without_scrollbar(&expected));
    }

    #[test]
    fn a_frame_that_overlaps_nothing_is_reported_not_forced_in() {
        // 一下滚过了一整屏：几乎一样的条目贴着结果末尾能对上一小段，底部有固定横幅时横幅也对得上，
        // 但这一帧不该接，结果保持不变
        let p = listing(1400);
        for with_banner in [false, true] {
            let shot = |t| {
                let mut f = listing_frame(&p, t);
                if with_banner {
                    banner(&mut f);
                }
                f
            };
            for jump in [190, 220, 260, 300, 340] {
                let mut s = StitchSession::new(LW, H, 20, 0, 0.01).unwrap();
                for t in [0, 40, 80] {
                    let heading = if t == 0 { None } else { Some(Heading::Down) };
                    s.push_bgra(&to_bgra(&shot(t)), heading, 0.15, 0.0, false).unwrap();
                }
                let before = exported(&s);
                let outcome = s.push_bgra(&to_bgra(&shot(jump)), Some(Heading::Down), 0.15, 0.0, false);
                assert_eq!(outcome, Err(StitchError::NoOverlap), "jump to {jump}, banner {with_banner}");
                assert_eq!(exported(&s), before);
            }
        }
    }

    #[test]
    fn scrolling_back_after_a_gap_joins_again() {
        // 接不上之后往回滚：这一帧之前滚轮是往上的，但相对已拼内容它还在下面。回到还不重叠的地方继续报接不上，
        // 回到和结果末尾重叠的地方就接回去，之后照常往下接
        let p = listing(1400);
        for with_banner in [false, true] {
            let shot = |t| {
                let mut f = listing_frame(&p, t);
                if with_banner {
                    banner(&mut f);
                }
                f
            };
            for back in [140, 120] {
                let mut s = StitchSession::new(LW, H, 20, 0, 0.01).unwrap();
                for t in [0, 40, 80] {
                    let heading = if t == 0 { None } else { Some(Heading::Down) };
                    s.push_bgra(&to_bgra(&shot(t)), heading, 0.15, 0.0, false).unwrap();
                }
                for (t, heading) in [(300, Heading::Down), (250, Heading::Up), (200, Heading::Up)] {
                    let outcome = s.push_bgra(&to_bgra(&shot(t)), Some(heading), top_ratio(Some(heading)), 0.0, false);
                    assert_eq!(outcome, Err(StitchError::NoOverlap), "frame at {t}, banner {with_banner}");
                }
                let outcome = s.push_bgra(&to_bgra(&shot(back)), Some(Heading::Up), 0.05, 0.0, false);
                assert_eq!(outcome.map(|o| o.top), Ok(back as usize), "back to {back}, banner {with_banner}");
                for t in (1..=5).map(|k| back + k * 40) {
                    let outcome = s.push_bgra(&to_bgra(&shot(t)), Some(Heading::Down), 0.15, 0.0, false);
                    assert_eq!(outcome.map(|o| o.top), Ok(t as usize), "frame at {t}, banner {with_banner}");
                }
                assert_eq!(s.height(), (back + 200 + H) as usize);
            }
        }
    }

    #[test]
    fn blank_lines_do_not_bridge_a_gap() {
        // 段落之间有空白的页面：一下滚过头之后，新帧开头的空白和结果末尾的空白也对得上，但不能当成重叠；
        // 往回滚的帧要么接对，要么接着报接不上，重叠够长时一定接回去
        for (height, start) in [(1700, 300), (2000, 420), (2000, 600), (2000, 777), (1700, 151)] {
            let p = page(height);
            for flick in [150u32, 200, 260] {
                for back in [20u32, 50, 80, 110, 130] {
                    let tops = [start, start + 40, start + 80];
                    let mut s = session();
                    for (k, &t) in tops.iter().enumerate() {
                        let heading = heading_between(k.checked_sub(1).map(|i| tops[i]), t);
                        s.push_bgra(&to_bgra(&frame(&p, t)), heading, top_ratio(heading), 0.0, false).unwrap();
                    }
                    let gap = start + 80 + flick;
                    let outcome = s.push_bgra(&to_bgra(&frame(&p, gap)), Some(Heading::Down), 0.15, 0.0, false);
                    assert_eq!(outcome, Err(StitchError::NoOverlap), "start {start}, frame at {gap}");
                    let t = gap - back;
                    let top = s.push_bgra(&to_bgra(&frame(&p, t)), Some(Heading::Up), 0.05, 0.0, false).map(|o| o.top);
                    let overlap = (start + 80 + H) as i64 - t as i64;
                    let case = format!("start {start}, back to {t}, overlap {overlap}: {top:?}");
                    if overlap <= 0 {
                        assert_eq!(top, Err(StitchError::NoOverlap), "{case}");
                    } else if overlap >= 40 {
                        assert_eq!(top, Ok((t - start) as usize), "{case}");
                    } else {
                        assert!(top.is_err() || top == Ok((t - start) as usize), "{case}");
                    }
                }
            }
        }
    }

    #[test]
    fn slight_rendering_differences_fall_back_to_the_average_colour() {
        // 同一行在不同帧里像素值差一点（比如按小数像素位移渲染的抗锯齿），逐像素指纹处处对不上
        let p = distinct_rows_page(900);
        let tops = [0u32, 50, 100, 150, 100, 150, 200, 250];
        let mut s = session();
        for (k, &t) in tops.iter().enumerate() {
            let mut f = frame(&p, t);
            for (x, y, px) in f.enumerate_pixels_mut() {
                px.0[1] ^= (mix(x + y * W, k as u32) & 1) as u8;
            }
            let heading = heading_between(k.checked_sub(1).map(|i| tops[i]), t);
            let outcome = s.push_bgra(&to_bgra(&f), heading, top_ratio(heading), 0.0, false);
            assert_eq!(outcome.map(|o| o.top), Ok(t as usize), "frame at {t}");
        }
        assert_eq!(s.height(), (250 + H) as usize);
    }

    #[test]
    fn rollback_keeps_the_result_and_tracks_the_position() {
        let p = distinct_rows_page(900);
        push_and_track(&p, &[0, 60, 120, 180, 240, 300, 240, 180, 120, 180, 240, 300, 360, 420, 480, 540, 600]);
    }

    #[test]
    fn finds_a_frame_far_from_the_previous_one() {
        // 从末尾一下跳回开头，超出上一帧附近的搜索窗口，靠全图搜索定位
        let p = distinct_rows_page(900);
        push_and_track(&p, &[0, 60, 120, 180, 240, 300, 360, 420, 480, 0, 60, 540, 600]);
    }

    #[test]
    fn a_far_jump_back_does_not_stick_to_the_other_end() {
        // 一下跳回开头附近，上一帧附近找不到；结果末尾的空白和新帧开头的空白也对得上，但那在滚动方向的反面，
        // 不能凑合接上，要再全图找到真正的位置
        let p = page(1600);
        for last in [440, 480, 540] {
            let mut tops = vec![600, 640, 680, 640, 600, 560, 520, 560, 600, 640, 680, 720];
            tops.push(last);
            push_and_track(&p, &tops);
        }
    }

    #[test]
    fn jump_back_over_blank_rows_finds_the_real_position() {
        // 文字页段落之间是空白行，附近窗口里处处能"对上"空白，不能当作位置
        let p = page(1300);
        let mut tops: Vec<u32> = (0..=10).map(|k| k * 60).collect();
        tops.extend([0, 60, 660, 720]);
        push_and_track(&p, &tops);
    }

    #[test]
    fn upward_capture_keeps_every_row_however_long() {
        // 重叠 60 行；长图远超一屏后，往上长的那一头仍然接得上
        let p = distinct_rows_page(2400);
        let tops: Vec<u32> = (0..=77).rev().map(|k| k * 30).collect();
        push_and_track(&p, &tops);
    }

    #[test]
    fn scrolling_past_the_start_grows_the_other_end() {
        // 从中间开始：先往下再往上越过起点，或先往上再往下越过起点
        let p = page(1600);
        push_and_track(&p, &[600, 660, 720, 660, 600, 540, 480, 420, 360]);
        push_and_track(&p, &[600, 540, 480, 540, 600, 660, 720, 780, 840]);
    }

    #[test]
    fn fixed_bars_stay_at_the_outer_edges_both_ways() {
        // 往下长时新帧的标题栏不接进来，往上长时结果顶上的旧标题栏换成新帧的；底栏同理
        let p = distinct_rows_page(1200);
        let tops = [600, 630, 660, 630, 600, 570, 540, 510, 540, 600, 690];
        push_and_track_with(&p, &tops, header);
        push_and_track_with(&p, &tops, footer);
    }

    #[test]
    fn unknown_heading_follows_the_last_movement() {
        // 条目重复：伸出开头和落在结果之内都对得上，后者还更长。只有第二帧带方向（之后是键盘翻页、手动截图），
        // 靠最近一次位置变化的方向接着往上长
        let p = repeating_page(900, 100);
        let tops: Vec<u32> = (0..=10).rev().map(|k| 300 + k * 30).collect();
        let mut s = session();
        for (k, &t) in tops.iter().enumerate() {
            let heading = if k == 1 { Some(Heading::Up) } else { None };
            let top_ratio = if heading == Some(Heading::Up) { 0.05 } else { 0.15 };
            let outcome = s.push_bgra(&to_bgra(&frame(&p, t)), heading, top_ratio, 0.0, false).unwrap();
            assert_eq!(outcome.top, 0, "frame at {t}");
        }
        assert_eq!(s.last_move(), Some(Heading::Up));
        assert_eq!(exported(&s), region(&p, 300, 600 + H));
    }

    #[test]
    fn local_differences_inside_the_overlap_stay_out_of_the_result() {
        // 光标停在截图区中间时，每帧中间那一行都带着悬停高亮（第一帧截下时光标还不在那里）
        fn hovered(p: &RgbaImage, top: u32) -> RgbaImage {
            let mut f = frame(p, top);
            for y in 40..46 {
                for x in 0..W {
                    f.put_pixel(x, y, Rgba([200, 225, 250, 255]));
                }
            }
            f
        }
        let p = distinct_rows_page(900);
        for tops in [(0..=30).map(|k| k * 20).collect::<Vec<u32>>(), (0..=30).rev().map(|k| k * 20).collect()] {
            let mut s = session();
            s.push_bgra(&to_bgra(&frame(&p, tops[0])), None, 0.15, 0.0, false).unwrap();
            for pair in tops.windows(2) {
                let heading = if pair[1] > pair[0] { Heading::Down } else { Heading::Up };
                let top_ratio = if heading == Heading::Up { 0.05 } else { 0.15 };
                s.push_bgra(&to_bgra(&hovered(&p, pair[1])), Some(heading), top_ratio, 0.0, false).unwrap();
            }
            assert_eq!(exported(&s), region(&p, 0, 600 + H));
        }
    }

    #[test]
    fn hover_next_to_a_fixed_bar_stays_out_of_the_result() {
        // 往上长时结果顶上是旧标题栏、往下长时末尾是旧底栏，那一头对不上；悬停高亮又把匹配段截成两截，
        // 较长的一截在高亮另一侧。接缝要落在旧固定栏旁边，不能把高亮带进来
        fn shot(p: &RgbaImage, top: u32, bar: fn(&mut RgbaImage), hover: std::ops::Range<u32>) -> RgbaImage {
            let mut f = frame(p, top);
            bar(&mut f);
            for y in hover {
                for x in 0..W {
                    f.put_pixel(x, y, Rgba([200, 225, 250, 255]));
                }
            }
            f
        }
        let p = distinct_rows_page(900);
        let cases: [(Vec<u32>, fn(&mut RgbaImage), std::ops::Range<u32>); 2] = [
            ((0..=15).map(|k| 600 - k * 20).collect(), header, 50..56),
            ((0..=15).map(|k| 300 + k * 20).collect(), footer, 34..40),
        ];
        for (tops, bar, hover) in cases {
            let mut s = session();
            s.push_bgra(&to_bgra(&shot(&p, tops[0], bar, 0..0)), None, 0.15, 0.0, false).unwrap();
            for pair in tops.windows(2) {
                let heading = heading_between(Some(pair[0]), pair[1]);
                let outcome = s.push_bgra(&to_bgra(&shot(&p, pair[1], bar, hover.clone())), heading, top_ratio(heading), 0.0, false);
                assert!(outcome.is_ok(), "frame at {}", pair[1]);
            }
            let (lo, hi) = (*tops.iter().min().unwrap(), *tops.iter().max().unwrap());
            let mut expected = region(&p, lo, hi + H);
            bar(&mut expected);
            assert_eq!(exported(&s), expected);
        }
    }

    #[test]
    fn crop_cuts_at_the_latest_frame_and_growth_goes_on() {
        let p = distinct_rows_page(900);
        let mut s = push_and_track(&p, &[0, 60, 120, 180, 240, 300, 240, 180]);
        assert_eq!(s.crop_bottom(), 300 - 180);
        assert_eq!(exported(&s), region(&p, 0, 180 + H));
        assert_eq!(s.crop_top(), 180);
        assert_eq!(exported(&s), region(&p, 180, 180 + H));
        assert_eq!((s.crop_top(), s.crop_bottom()), (0, 0));
        // 裁完照常往两头长
        for (t, heading) in [(120, Heading::Up), (60, Heading::Up), (120, Heading::Down), (240, Heading::Down), (300, Heading::Down)] {
            let top_ratio = if heading == Heading::Up { 0.05 } else { 0.15 };
            s.push_bgra(&to_bgra(&frame(&p, t)), Some(heading), top_ratio, 0.0, false).unwrap();
        }
        assert_eq!(exported(&s), region(&p, 60, 300 + H));
    }

    #[test]
    fn first_frame_is_kept_as_is() {
        let p = page(300);
        let f = frame(&p, 0);
        let mut s = session();
        let outcome = s.push_bgra(&to_bgra(&f), None, 0.15, 0.0, false).unwrap();
        assert_eq!(outcome, PushOutcome { head_cut: 0, head_rows: 0, keep: 0, skip: 0, top: 0 });
        assert_eq!(exported(&s), f);
    }

    #[test]
    fn growing_the_top_reports_what_changed() {
        let p = distinct_rows_page(900);
        let mut s = session();
        s.push_bgra(&to_bgra(&frame(&p, 300)), None, 0.15, 0.0, false).unwrap();
        let outcome = s.push_bgra(&to_bgra(&frame(&p, 250)), Some(Heading::Up), 0.05, 0.0, false).unwrap();
        assert_eq!(outcome.top, 0);
        assert_eq!(outcome.head_rows - outcome.head_cut, 50);
        assert_eq!(exported(&s), region(&p, 250, 300 + H));
    }

    #[test]
    fn no_overlap_leaves_session_unchanged() {
        let p = page(900);
        let mut s = session();
        s.push_bgra(&to_bgra(&frame(&p, 0)), None, 0.15, 0.0, false).unwrap();
        let before = exported(&s);
        let noise = RgbaImage::from_fn(W, H, |x, y| {
            let v = (mix(x, y) & 0xFF) as u8;
            Rgba([v, v ^ 0x5A, v ^ 0xA5, 255])
        });
        assert_eq!(s.push_bgra(&to_bgra(&noise), None, 0.15, 0.0, false), Err(StitchError::NoOverlap));
        assert_eq!(exported(&s), before);
    }

    #[test]
    fn rejects_frames_of_the_wrong_size() {
        let mut s = session();
        let err = s.push_bgra(&[0u8; 16], None, 0.0, 0.0, false).unwrap_err();
        assert!(matches!(err, StitchError::InvalidFrame(_)));
        assert_eq!(s.height(), 0);
    }

    #[test]
    fn alpha_is_forced_opaque() {
        let p = page(300);
        let mut bgra = to_bgra(&frame(&p, 0));
        bgra.chunks_exact_mut(4).for_each(|px| px[3] = 0);
        let mut s = session();
        s.push_bgra(&bgra, None, 0.0, 0.0, false).unwrap();
        assert!(s.export_bgra().chunks_exact(4).all(|px| px[3] == 255));
    }

    #[test]
    fn clear_releases_the_result() {
        let p = page(300);
        let mut s = session();
        s.push_bgra(&to_bgra(&frame(&p, 0)), None, 0.0, 0.0, false).unwrap();
        s.clear();
        assert_eq!(s.height(), 0);
        assert!(s.export_bgra().is_empty());
    }

}
