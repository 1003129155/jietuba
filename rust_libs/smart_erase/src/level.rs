// Modified from Snow Draw Engine's Smart Erase (Apache-2.0); see NOTICE.

use std::collections::VecDeque;

use crate::{Cancel, EraseError, Image};

/// RGB, 0 to 255.
pub(crate) type Color = [f32; 3];

pub(crate) fn distance(a: Color, b: Color) -> f32 {
    (0..3).map(|c| (a[c] - b[c]) * (a[c] - b[c])).sum()
}

pub(crate) fn mix(a: Color, b: Color, t: f32) -> Color {
    std::array::from_fn(|c| a[c] * (1.0 - t) + b[c] * t)
}

/// One working grid. Hole pixels carry no colour; uncovered pixels (transparent or reduced
/// away) are neither holes nor donors.
#[derive(Clone)]
pub(crate) struct Level {
    pub width: usize,
    pub height: usize,
    /// Extent of the largest donor neighbourhood, which scales random search windows and
    /// distance penalties independently of how far the region has grown.
    pub search_span: usize,
    pub source: Vec<Color>,
    pub hole: Vec<bool>,
    pub covered: Vec<bool>,
    /// Known pixels close enough to the hole to donate patches.
    pub domain: Vec<bool>,
}

impl Level {
    pub fn new(width: usize, height: usize) -> Self {
        let n = width * height;
        Self {
            width,
            height,
            search_span: width.max(height),
            source: vec![[0.0; 3]; n],
            hole: vec![false; n],
            covered: vec![false; n],
            domain: vec![false; n],
        }
    }

    /// The region [x, y, width, height] of `image`, each cell averaging `scale` x `scale`
    /// pixels; a cell with any erased pixel is a hole.
    pub fn sample(
        image: &Image,
        erased: &dyn Fn(usize, usize) -> bool,
        region: [usize; 4],
        scale: usize,
        search_span: usize,
        cancel: &Cancel,
    ) -> Result<Self, EraseError> {
        let mut level = Level::new(region[2].div_ceil(scale), region[3].div_ceil(scale));
        level.search_span = search_span.div_ceil(scale).max(1);
        for ly in 0..level.height {
            cancel.check()?;
            let y0 = region[1] + ly * scale;
            let y1 = (y0 + scale).min(region[1] + region[3]);
            for lx in 0..level.width {
                let x0 = region[0] + lx * scale;
                let x1 = (x0 + scale).min(region[0] + region[2]);
                let mut sum = [0.0_f32; 3];
                let mut count = 0.0_f32;
                let (mut covered, mut hole) = (false, false);
                for y in y0..y1 {
                    for x in x0..x1 {
                        let p = image.pixel(x, y);
                        if p[3] == 0 {
                            continue;
                        }
                        covered = true;
                        if erased(x, y) {
                            hole = true;
                        } else {
                            for c in 0..3 {
                                sum[c] += f32::from(p[c]);
                            }
                            count += 1.0;
                        }
                    }
                }
                let i = ly * level.width + lx;
                level.covered[i] = covered;
                level.hole[i] = hole;
                if covered && !hole {
                    level.source[i] = sum.map(|v| v / count);
                }
            }
        }
        Ok(level)
    }

    pub fn at(&self, x: isize, y: isize) -> Option<usize> {
        (x >= 0 && y >= 0 && (x as usize) < self.width && (y as usize) < self.height).then(|| y as usize * self.width + x as usize)
    }

    pub fn known(&self, i: usize) -> bool {
        self.covered[i] && !self.hole[i]
    }

    pub fn holes(&self) -> Vec<usize> {
        self.hole.iter().enumerate().filter_map(|(i, h)| h.then_some(i)).collect()
    }

    /// [x0, y0, x1, y1) of the hole pixels.
    pub fn hole_bounds(&self) -> [usize; 4] {
        let mut b = [usize::MAX, usize::MAX, 0, 0];
        for (i, h) in self.hole.iter().enumerate() {
            if *h {
                let (x, y) = (i % self.width, i / self.width);
                b = [b[0].min(x), b[1].min(y), b[2].max(x + 1), b[3].max(y + 1)];
            }
        }
        b
    }

    /// Known pixels within `radius` pixels of the hole, in index order.
    pub fn ring(&self, radius: isize, cancel: &Cancel) -> Result<Vec<usize>, EraseError> {
        let mut ring = vec![false; self.source.len()];
        for (n, i) in self.holes().into_iter().enumerate() {
            if n % 64 == 0 {
                cancel.check()?;
            }
            let (x, y) = ((i % self.width) as isize, (i / self.width) as isize);
            for dy in -radius..=radius {
                for dx in -radius..=radius {
                    if dx * dx + dy * dy <= radius * radius + radius {
                        if let Some(j) = self.at(x + dx, y + dy) {
                            ring[j] |= self.known(j);
                        }
                    }
                }
            }
        }
        Ok(ring.iter().enumerate().filter_map(|(i, r)| r.then_some(i)).collect())
    }

    /// Marks the donor domain: known pixels within a reach of the hole that grows until at
    /// least 64 full 7x7 patches fit. Returns the reach.
    pub fn make_domain(&mut self, cancel: &Cancel) -> Result<usize, EraseError> {
        let nearest = nearest(self.width, self.height, &self.hole, cancel)?;
        let [x0, y0, x1, y1] = self.hole_bounds();
        let mut reach = ((x1 - x0).min(y1 - y0) / 2).clamp(16, 96);
        loop {
            cancel.check()?;
            for (i, p) in nearest.iter().enumerate() {
                let near = *p != usize::MAX && {
                    let dx = (i % self.width).abs_diff(p % self.width);
                    let dy = (i / self.width).abs_diff(p / self.width);
                    dx * dx + dy * dy <= reach * reach
                };
                self.domain[i] = self.known(i) && near;
            }
            if erode(&self.domain, self.width, self.height, 3).iter().filter(|v| **v).count() >= 64 || reach >= 512 {
                return Ok(reach);
            }
            reach = (reach * 2).min(512);
        }
    }

    /// Whether the hole comes within `margin` of a side of the grid that could still grow.
    pub fn near_border(&self, margin: usize, growable: [bool; 4]) -> bool {
        let [x0, y0, x1, y1] = self.hole_bounds();
        (growable[0] && x0 < margin)
            || (growable[1] && y0 < margin)
            || (growable[2] && x1 + margin > self.width)
            || (growable[3] && y1 + margin > self.height)
    }

    /// `colors` sampled at a fractional cell position, from covered cells only.
    pub fn bilinear(&self, colors: &[Color], fx: f32, fy: f32) -> Color {
        let fx = fx.clamp(0.0, (self.width - 1) as f32);
        let fy = fy.clamp(0.0, (self.height - 1) as f32);
        let (x0, y0) = (fx.floor() as usize, fy.floor() as usize);
        let (x1, y1) = ((x0 + 1).min(self.width - 1), (y0 + 1).min(self.height - 1));
        let (tx, ty) = (fx - x0 as f32, fy - y0 as f32);
        let mut sum = [0.0_f32; 3];
        let mut total = 0.0;
        for (x, y, w) in [(x0, y0, (1.0 - tx) * (1.0 - ty)), (x1, y0, tx * (1.0 - ty)), (x0, y1, (1.0 - tx) * ty), (x1, y1, tx * ty)] {
            let i = y * self.width + x;
            if self.covered[i] && w > 0.0 {
                for c in 0..3 {
                    sum[c] += colors[i][c] * w;
                }
                total += w;
            }
        }
        if total > 0.0 {
            sum.map(|v| v / total)
        } else {
            colors[fy.round() as usize * self.width + fx.round() as usize]
        }
    }
}

/// For every pixel, the index of a nearest seed by breadth-first propagation (usize::MAX
/// when there are no seeds).
pub(crate) fn nearest(width: usize, height: usize, seeds: &[bool], cancel: &Cancel) -> Result<Vec<usize>, EraseError> {
    let mut result = vec![usize::MAX; seeds.len()];
    let mut queue = VecDeque::new();
    for (i, seed) in seeds.iter().enumerate() {
        if *seed {
            result[i] = i;
            queue.push_back(i);
        }
    }
    while let Some(i) = queue.pop_front() {
        if i % 4096 == 0 {
            cancel.check()?;
        }
        let (x, y) = (i % width, i / width);
        for j in [
            x.checked_sub(1).map(|_| i - 1),
            (x + 1 < width).then_some(i + 1),
            y.checked_sub(1).map(|_| i - width),
            (y + 1 < height).then_some(i + width),
        ]
        .into_iter()
        .flatten()
        {
            if result[j] == usize::MAX {
                result[j] = result[i];
                queue.push_back(j);
            }
        }
    }
    Ok(result)
}

/// Pixels whose whole (2 * radius + 1) square lies inside `mask` and the grid.
pub(crate) fn erode(mask: &[bool], width: usize, height: usize, radius: usize) -> Vec<bool> {
    let stride = width + 1;
    let mut sums = vec![0_u32; (width + 1) * (height + 1)];
    for y in 0..height {
        let mut row = 0;
        for x in 0..width {
            row += u32::from(mask[y * width + x]);
            sums[(y + 1) * stride + x + 1] = sums[y * stride + x + 1] + row;
        }
    }
    let mut out = vec![false; mask.len()];
    let side = (radius * 2 + 1) as u32;
    for y in radius..height.saturating_sub(radius) {
        for x in radius..width.saturating_sub(radius) {
            let a = (y - radius) * stride + x - radius;
            let b = (y - radius) * stride + x + radius + 1;
            let c = (y + radius + 1) * stride + x - radius;
            let d = (y + radius + 1) * stride + x + radius + 1;
            out[y * width + x] = sums[d] + sums[a] - sums[b] - sums[c] == side * side;
        }
    }
    out
}
