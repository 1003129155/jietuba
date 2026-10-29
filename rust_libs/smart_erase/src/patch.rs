// Modified from Snow Draw Engine's Smart Erase (Apache-2.0); see NOTICE.

//! Textures and edges: multiscale patch synthesis guided by a continuation of the
//! surrounding background.

use std::ops::Range;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering::Relaxed};

use crate::level::{distance, erode, mix, nearest, Color, Level};
use crate::parallel;
use crate::{Cancel, EraseError};

/// Rows per band of the match search. Bands of one parity run in parallel; the fixed size keeps
/// the result independent of the number of threads.
const BAND_ROWS: usize = 16;

struct Guide {
    color: Vec<Color>,
    variation: Vec<f32>,
}

struct Filled {
    image: Vec<Color>,
    matches: Vec<usize>,
    width: usize,
    height: usize,
}

/// CIE Lab, for perceptual guide and patch costs.
fn lab(rgb: Color) -> Color {
    let v = rgb.map(|c| {
        let c = c / 255.0;
        if c <= 0.04045 {
            c / 12.92
        } else {
            ((c + 0.055) / 1.055).powf(2.4)
        }
    });
    let xyz = [
        (v[0] * 0.4124564 + v[1] * 0.3575761 + v[2] * 0.1804375) / 0.95047,
        v[0] * 0.2126729 + v[1] * 0.7151522 + v[2] * 0.072175,
        (v[0] * 0.0193339 + v[1] * 0.119192 + v[2] * 0.9503041) / 1.08883,
    ];
    let f = xyz.map(|t| if t > 0.008856 { t.cbrt() } else { 7.787 * t + 16.0 / 116.0 });
    [116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2])]
}

/// One or three channels for the Gaussian.
trait Channels: Copy + Send + Sync {
    const ZERO: Self;
    fn add_scaled(&mut self, value: Self, weight: f32);
}

impl Channels for f32 {
    const ZERO: f32 = 0.0;
    fn add_scaled(&mut self, value: f32, weight: f32) {
        *self += value * weight;
    }
}

impl Channels for Color {
    const ZERO: Color = [0.0; 3];
    fn add_scaled(&mut self, value: Color, weight: f32) {
        for c in 0..3 {
            self[c] += value[c] * weight;
        }
    }
}

/// Separable Gaussian with clamped edges.
fn gaussian<T: Channels>(values: &[T], level: &Level, radius: usize, cancel: &Cancel) -> Result<Vec<T>, EraseError> {
    cancel.check()?;
    let sigma = radius as f32 / 2.0;
    let mut kernel: Vec<_> = (-(radius as isize)..=radius as isize).map(|i| (-(i * i) as f32 / (2.0 * sigma * sigma)).exp()).collect();
    let norm: f32 = kernel.iter().sum();
    kernel.iter_mut().for_each(|k| *k /= norm);
    let (w, h) = (level.width, level.height);
    let mut temp = vec![T::ZERO; values.len()];
    parallel::for_each_chunk(&mut temp, 16 * w, |start, chunk| {
        for (n, out) in chunk.iter_mut().enumerate() {
            let (x, y) = ((start + n) % w, (start + n) / w);
            for (k, weight) in kernel.iter().enumerate() {
                let xx = (x + k).saturating_sub(radius).min(w - 1);
                out.add_scaled(values[y * w + xx], *weight);
            }
        }
    });
    cancel.check()?;
    let mut out = vec![T::ZERO; values.len()];
    parallel::for_each_chunk(&mut out, 16 * w, |start, chunk| {
        for (n, value) in chunk.iter_mut().enumerate() {
            let (x, y) = ((start + n) % w, (start + n) / w);
            for (k, weight) in kernel.iter().enumerate() {
                let yy = (y + k).saturating_sub(radius).min(h - 1);
                value.add_scaled(temp[yy * w + x], *weight);
            }
        }
    });
    Ok(out)
}

/// The background expected inside the hole: smoothed observations continued across it
/// along whichever of four directions has the most compatible endpoints, which keeps bands
/// and edges without assuming a layout. Also carries the local texture strength.
fn guides(level: &Level, colors: &[Color], cancel: &Cancel) -> Result<Guide, EraseError> {
    let [x0, y0, x1, y1] = level.hole_bounds();
    let radius = ((x1 - x0).min(y1 - y0) / 12).clamp(4, 16);
    let weights: Vec<f32> = (0..colors.len()).map(|i| if level.known(i) { 1.0 } else { 0.0 }).collect();
    let weights = gaussian(&weights, level, radius, cancel)?;
    let observed: Vec<_> = colors.iter().enumerate().map(|(i, c)| if level.known(i) { *c } else { [0.0; 3] }).collect();
    let mut mean = gaussian(&observed, level, radius, cancel)?;
    let squared: Vec<f32> = observed.iter().map(|c| c.iter().map(|v| v * v).sum::<f32>()).collect();
    let squared = gaussian(&squared, level, radius, cancel)?;
    let mut variation = vec![0.0; colors.len()];
    for i in 0..mean.len() {
        for value in &mut mean[i] {
            *value /= weights[i].max(1e-6);
        }
        variation[i] = (squared[i] / weights[i].max(1e-6) - mean[i].iter().map(|c| c * c).sum::<f32>()).max(0.0).sqrt();
    }
    let mut guide = Guide { color: mean.clone(), variation: variation.clone() };
    let mut costs = vec![f32::MAX; colors.len()];
    let flush = |before: Option<usize>, after: Option<usize>, pending: &[usize], guide: &mut Guide, costs: &mut [f32]| {
        let (Some(a), Some(b)) = (before.or(after), after.or(before)) else {
            return;
        };
        let difference = distance(mean[a], mean[b]);
        let span = (((a % level.width).abs_diff(b % level.width).pow(2) + (a / level.width).abs_diff(b / level.width).pow(2)) as f32).sqrt();
        let cost = difference + 0.02 * span + if before.is_none() || after.is_none() { 1000.0 } else { 0.0 };
        for &i in pending {
            if cost >= costs[i] {
                continue;
            }
            let d = (((i % level.width).abs_diff(a % level.width).pow(2) + (i / level.width).abs_diff(a / level.width).pow(2)) as f32).sqrt();
            let t = if span > 0.0 { d / span } else { 0.0 };
            guide.color[i] = if difference < 900.0 {
                mix(mean[a], mean[b], t)
            } else if t < 0.5 {
                mean[a]
            } else {
                mean[b]
            };
            guide.variation[i] = variation[a] * (1.0 - t) + variation[b] * t;
            costs[i] = cost;
        }
    };
    for (dx, dy) in [(1_isize, 0_isize), (0, 1), (1, 1), (-1, 1)] {
        for y in 0..level.height {
            cancel.check()?;
            for x in 0..level.width {
                // Each line starts where stepping back leaves the grid.
                if level.at(x as isize - dx, y as isize - dy).is_some() {
                    continue;
                }
                let mut before = None;
                let mut pending = Vec::new();
                let (mut xx, mut yy) = (x as isize, y as isize);
                while let Some(i) = level.at(xx, yy) {
                    if level.known(i) {
                        flush(before, Some(i), &pending, &mut guide, &mut costs);
                        pending.clear();
                        before = Some(i);
                    } else if level.hole[i] {
                        pending.push(i);
                    } else {
                        flush(before, None, &pending, &mut guide, &mut costs);
                        pending.clear();
                        before = None;
                    }
                    xx += dx;
                    yy += dy;
                }
                flush(before, None, &pending, &mut guide, &mut costs);
            }
        }
    }
    Ok(guide)
}

/// The level's hole filled coarse to fine; colours outside the hole are the level's own.
pub(crate) fn multiscale(level: &Level, cancel: &Cancel) -> Result<Vec<Color>, EraseError> {
    // Conservative reduction: no coarse donor may include a hole or a coverage gap.
    let mut coarse: Vec<Level> = Vec::new();
    while coarse.len() < 5 {
        let fine = coarse.last().unwrap_or(level);
        if fine.width.min(fine.height) < 40 {
            break;
        }
        let next = downsample(fine, cancel)?;
        if !next.hole.iter().any(|h| *h) || erode(&next.domain, next.width, next.height, 3).iter().filter(|d| **d).count() < 16 {
            break;
        }
        coarse.push(next);
    }
    let levels: Vec<&Level> = std::iter::once(level).chain(coarse.iter()).collect();
    let count = levels.len();
    let mut previous = None;
    for (n, level) in levels.iter().rev().enumerate() {
        cancel.check()?;
        let passes = if n == 0 {
            5
        } else if n + 1 == count {
            2
        } else {
            3
        };
        previous = Some(fill(level, previous.as_ref(), passes, cancel)?);
    }
    let filled = previous.ok_or(EraseError::NoDonors)?;
    let mut out = level.source.clone();
    for i in level.holes() {
        out[i] = filled.image[i];
    }
    Ok(out)
}

/// xorshift32 with a fixed seed, so the same input always gives the same result.
struct Random(u32);

impl Random {
    fn next(&mut self) -> u32 {
        let mut x = self.0;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.0 = x;
        x
    }

    fn pick(&mut self, n: usize) -> usize {
        self.next() as usize % n
    }
}

fn fill(level: &Level, previous: Option<&Filled>, passes: usize, cancel: &Cancel) -> Result<Filled, EraseError> {
    let mut colors = vec![[0.0; 3]; level.source.len()];
    parallel::for_each_chunk(&mut colors, 4096, |start, chunk| {
        for (n, color) in chunk.iter_mut().enumerate() {
            *color = lab(level.source[start + n]);
        }
    });
    let guide = guides(level, &colors, cancel)?;
    let holes = level.holes();
    let mut radius = 3;
    let mut donors = erode(&level.domain, level.width, level.height, radius);
    while !donors.iter().any(|d| *d) && radius > 0 {
        radius -= 1;
        donors = erode(&level.domain, level.width, level.height, radius);
    }
    let candidates: Vec<_> = donors.iter().enumerate().filter_map(|(i, d)| d.then_some(i)).collect();
    if candidates.is_empty() {
        return Err(EraseError::NoDonors);
    }
    let nearest = nearest(level.width, level.height, &donors, cancel)?;
    let r = radius as isize;
    let w = level.width;
    let side = (2 * r + 1) as usize;
    let mut random = Random(0x534e4f57);
    let mut output = colors.clone();
    // Atomic so that bands searched in parallel can read their neighbours' rows.
    let matches: Vec<AtomicUsize> = (0..colors.len()).map(|_| AtomicUsize::new(usize::MAX)).collect();
    let cost = |i: usize, p: usize| distance(guide.color[i], guide.color[p]) + 4.0 * (guide.variation[i] - guide.variation[p]).powi(2);
    for (n, &i) in holes.iter().enumerate() {
        if n % 64 == 0 {
            cancel.check()?;
        }
        let mut best = nearest[i];
        let mut error = cost(i, best);
        for _ in 0..32 {
            let p = candidates[random.pick(candidates.len())];
            let e = cost(i, p);
            if e < error {
                best = p;
                error = e;
            }
        }
        matches[i].store(best, Relaxed);
        output[i] = colors[best];
        if let Some(prev) = previous {
            let px = i % w * prev.width / level.width;
            let py = i / w * prev.height / level.height;
            let p = prev.matches[py * prev.width + px];
            if p != usize::MAX {
                let sx = p % prev.width * level.width / prev.width;
                let sy = p / prev.width * level.height / prev.height;
                if let Some(j) = level.at(sx as isize, sy as isize).filter(|j| donors[*j]) {
                    matches[i].store(j, Relaxed);
                }
                output[i] = lab(prev.image[py * prev.width + px]);
            }
        }
    }
    // Per hole: covered pixels of its patch window that are known and missing, and whether the
    // window lies wholly inside the grid and coverage (the common case, read without checks).
    let mut coverage = vec![(0.0_f32, 0.0_f32, false); holes.len()];
    for (n, &i) in holes.iter().enumerate() {
        let (x, y) = ((i % w) as isize, (i / w) as isize);
        let (mut known, mut missing, mut full) = (0.0, 0.0, true);
        for dy in -r..=r {
            for dx in -r..=r {
                match level.at(x + dx, y + dy) {
                    Some(t) if level.covered[t] => {
                        if level.hole[t] {
                            missing += 1.0;
                        } else {
                            known += 1.0;
                        }
                    }
                    _ => full = false,
                }
            }
        }
        coverage[n] = (known, missing, full);
    }
    // Holes are in index order, so each band is a run of them.
    let band_of = |k: usize| holes[k] / w / BAND_ROWS;
    let mut bands: [Vec<Range<usize>>; 2] = [Vec::new(), Vec::new()];
    let mut start = 0;
    for k in 1..=holes.len() {
        if k == holes.len() || band_of(k) != band_of(start) {
            bands[band_of(start) % 2].push(start..k);
            start = k;
        }
    }
    let costs: Vec<AtomicU32> = (0..colors.len()).map(|_| AtomicU32::new(f32::MAX.to_bits())).collect();
    let mut reuse = vec![0.0; colors.len()];
    let expected = (holes.len() as f32 / candidates.len() as f32).max(1.0);
    let scale = level.search_span as f32;
    let mut next = output.clone();
    let mut rgb = level.source.clone();
    let mut votes = vec![([0.0; 3], [0.0; 3]); holes.len()];
    for pass in 0..passes {
        cancel.check()?;
        let synthesized = 0.15 + 0.20 * pass as f32 / (passes - 1).max(1) as f32;
        reuse.fill(0.0);
        for &i in &holes {
            reuse[matches[i].load(Relaxed)] += 1.0;
        }
        for &i in &candidates {
            reuse[i] = 25.0 * (reuse[i] / expected).ln_1p();
        }
        let step = if pass % 2 == 0 { 1_isize } else { -1 };
        let search = |band: &Range<usize>| {
            let mut random = Random(0x534e4f57 ^ (band_of(band.start) as u32).wrapping_mul(0x9e37_79b1) ^ (pass as u32).wrapping_mul(0x85eb_ca77) | 1);
            for n in 0..band.len() {
                let k = if step > 0 { band.start + n } else { band.end - 1 - n };
                let i = holes[k];
                let x = (i % w) as isize;
                let y = (i / w) as isize;
                let (known, missing, full) = coverage[k];
                let norm = (known + missing * synthesized).max(1.0);
                let mut best = matches[i].load(Relaxed);
                let mut best_cost = f32::MAX;
                let mut try_candidate = |p: usize| {
                    if p == usize::MAX || !donors[p] {
                        return;
                    }
                    let sx = (p % w) as isize;
                    let sy = (p / w) as isize;
                    let fixed = 8.0 / (4.0 + guide.variation[i]) * distance(guide.color[i], guide.color[p])
                        + 4.0 * (guide.variation[i] - guide.variation[p]).powi(2)
                        + reuse[p]
                        + 8.0 * ((sx - x).pow(2) + (sy - y).pow(2)) as f32 / (scale * scale);
                    if fixed >= best_cost {
                        return;
                    }
                    // Every term is nonnegative: once the partial sum reaches this the candidate cannot win.
                    let limit = (best_cost - fixed) * norm;
                    let mut sum = 0.0;
                    if full {
                        // Donors are eroded by the radius, so the source window is always inside too.
                        for dy in -r..=r {
                            let t0 = (y + dy) as usize * w + (x - r) as usize;
                            let s0 = (sy + dy) as usize * w + (sx - r) as usize;
                            for c in 0..side {
                                let d = distance(output[t0 + c], colors[s0 + c]);
                                sum += if level.hole[t0 + c] { d * synthesized } else { d };
                            }
                            if sum >= limit {
                                return;
                            }
                        }
                    } else {
                        for dy in -r..=r {
                            for dx in -r..=r {
                                let (Some(t), Some(s)) = (level.at(x + dx, y + dy), level.at(sx + dx, sy + dy)) else {
                                    continue;
                                };
                                if !level.covered[t] {
                                    continue;
                                }
                                let weight = if level.hole[t] { synthesized } else { 1.0 };
                                sum += distance(output[t], colors[s]) * weight;
                            }
                            if sum >= limit {
                                return;
                            }
                        }
                    }
                    let score = sum / norm + fixed;
                    if score < best_cost {
                        best = p;
                        best_cost = score;
                    }
                };
                try_candidate(matches[i].load(Relaxed));
                try_candidate(nearest[i]);
                try_candidate(candidates[random.pick(candidates.len())]);
                for (nx, ny, ox, oy) in [(x - step, y, step, 0), (x, y - step, 0, step)] {
                    if let Some(n) = level.at(nx, ny) {
                        let p = matches[n].load(Relaxed);
                        if p != usize::MAX {
                            if let Some(j) = level.at((p % w) as isize + ox, (p / w) as isize + oy) {
                                try_candidate(j);
                            }
                        }
                    }
                }
                // Random windows centred on the current match, halving each time.
                let center = matches[i].load(Relaxed);
                let mut window = level.search_span as isize;
                while window > 0 {
                    let sx = (center % w) as isize + random.pick((window * 2 + 1) as usize) as isize - window;
                    let sy = (center / w) as isize + random.pick((window * 2 + 1) as usize) as isize - window;
                    if let Some(p) = level.at(sx, sy) {
                        try_candidate(p);
                    }
                    window /= 2;
                }
                matches[i].store(best, Relaxed);
                costs[i].store(best_cost.to_bits(), Relaxed);
            }
        };
        // A band reads only the rows beside it, which the other parity owns.
        parallel::for_each(&bands[0], search);
        cancel.check()?;
        parallel::for_each(&bands[1], search);
        cancel.check()?;
        // Voting: overlapping patches average, favouring good matches that agree with the guide.
        parallel::for_each_chunk(&mut votes, 1024, |start, chunk| {
            for (n, vote) in chunk.iter_mut().enumerate() {
                let i = holes[start + n];
                let x = (i % w) as isize;
                let y = (i / w) as isize;
                let own = matches[i].load(Relaxed);
                let anchor = colors[own];
                let tolerance = (guide.variation[i] * 0.25).clamp(1.5, 10.0);
                let mut sum = [0.0; 3];
                let mut sum_rgb = [0.0; 3];
                let mut total = 0.0;
                for dy in -r..=r {
                    for dx in -r..=r {
                        let Some(t) = level.at(x + dx, y + dy) else {
                            continue;
                        };
                        let p = matches[t].load(Relaxed);
                        if !level.hole[t] || p == usize::MAX {
                            continue;
                        }
                        let Some(j) = level.at((p % w) as isize - dx, (p / w) as isize - dy) else {
                            continue;
                        };
                        let weight = 1.0
                            / ((1.0 + f32::from_bits(costs[t].load(Relaxed)))
                                * (1.0 + distance(guide.color[j], guide.color[i]) / 100.0)
                                * (1.0 + distance(colors[j], anchor) / (tolerance * tolerance)));
                        for c in 0..3 {
                            sum[c] += colors[j][c] * weight;
                            sum_rgb[c] += level.source[j][c] * weight;
                        }
                        total += weight;
                    }
                }
                *vote = if total > 0.0 {
                    (sum.map(|v| v / total), sum_rgb.map(|v| v / total))
                } else {
                    (output[i], level.source[own])
                };
            }
        });
        for (k, &i) in holes.iter().enumerate() {
            next[i] = votes[k].0;
            rgb[i] = votes[k].1;
        }
        std::mem::swap(&mut output, &mut next);
    }
    Ok(Filled { image: rgb, matches: matches.into_iter().map(AtomicUsize::into_inner).collect(), width: level.width, height: level.height })
}

fn downsample(fine: &Level, cancel: &Cancel) -> Result<Level, EraseError> {
    let known: Vec<_> = (0..fine.source.len()).map(|i| fine.known(i)).collect();
    let safe = erode(&known, fine.width, fine.height, 2);
    let domain = erode(&fine.domain, fine.width, fine.height, 2);
    let mut coarse = Level::new(fine.width.div_ceil(2), fine.height.div_ceil(2));
    coarse.search_span = fine.search_span.div_ceil(2);
    const WEIGHTS: [f32; 5] = [1.0, 4.0, 6.0, 4.0, 1.0];
    for y in 0..coarse.height {
        cancel.check()?;
        for x in 0..coarse.width {
            let i = y * coarse.width + x;
            let (fx, fy) = (x * 2, y * 2);
            let mut all_covered = true;
            let mut any_hole = false;
            let mut all_domain = true;
            let mut sum = [0.0; 3];
            let mut total = 0.0;
            for dy in -2_isize..=2 {
                for dx in -2_isize..=2 {
                    if let Some(j) = fine.at(fx as isize + dx, fy as isize + dy) {
                        all_covered &= fine.covered[j];
                        any_hole |= fine.hole[j];
                        all_domain &= domain[j];
                        if fine.known(j) {
                            let w = WEIGHTS[(dx + 2) as usize] * WEIGHTS[(dy + 2) as usize];
                            for (c, value) in sum.iter_mut().enumerate() {
                                *value += fine.source[j][c] * w;
                            }
                            total += w;
                        }
                    } else {
                        all_covered = false;
                        all_domain = false;
                    }
                }
            }
            coarse.hole[i] = any_hole && all_covered;
            coarse.covered[i] = all_covered;
            coarse.domain[i] = all_domain && safe[fy * fine.width + fx] && !any_hole;
            if total > 0.0 && !any_hole {
                coarse.source[i] = sum.map(|v| v / total);
            }
        }
    }
    Ok(coarse)
}
