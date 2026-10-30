//! Smooth backgrounds (wallpaper gradients, vignettes): a membrane stretched over the ring.
//! Patch synthesis tiles such surfaces into visible facets; the membrane cannot, but it
//! blurs any edge crossing the boundary, hence the edge test.

use crate::level::{Color, Level};
use crate::{Cancel, EraseError};

/// A ring pixel stepping more than this from its right or lower neighbour is an edge.
const EDGE_STEP: f32 = 12.0;
/// Fewer edge pixels than this on the ring count as no edge at all.
const MAX_EDGE_PIXELS: usize = 3;

pub(crate) fn smooth(level: &Level, ring: &[usize]) -> bool {
    let mut edges = 0;
    for &i in ring {
        let (x, y) = (i % level.width, i / level.width);
        for j in [(x + 1 < level.width).then_some(i + 1), (y + 1 < level.height).then_some(i + level.width)].into_iter().flatten() {
            if level.known(j) && (0..3).any(|c| (level.source[i][c] - level.source[j][c]).abs() > EDGE_STEP) {
                edges += 1;
                break;
            }
        }
        if edges >= MAX_EDGE_PIXELS {
            return false;
        }
    }
    true
}

const OUTSIDE: u8 = 0;
const FIXED: u8 = 1;
const FREE: u8 = 2;

struct Grid {
    width: usize,
    height: usize,
    value: Vec<Color>,
    state: Vec<u8>,
}

impl Grid {
    fn count(&self, state: u8) -> usize {
        self.state.iter().filter(|s| **s == state).count()
    }

    /// Half size: a cell is free if any child is, else fixed at its fixed children's mean.
    fn coarsen(&self) -> Grid {
        let (width, height) = (self.width.div_ceil(2), self.height.div_ceil(2));
        let mut coarse = Grid { width, height, value: vec![[0.0; 3]; width * height], state: vec![OUTSIDE; width * height] };
        for y in 0..height {
            for x in 0..width {
                let mut sum = [0.0; 3];
                let mut fixed = 0.0;
                let mut free = false;
                for (cx, cy) in [(2 * x, 2 * y), (2 * x + 1, 2 * y), (2 * x, 2 * y + 1), (2 * x + 1, 2 * y + 1)] {
                    if cx >= self.width || cy >= self.height {
                        continue;
                    }
                    let j = cy * self.width + cx;
                    match self.state[j] {
                        FREE => free = true,
                        FIXED => {
                            sum.iter_mut().zip(self.value[j]).for_each(|(s, v)| *s += v);
                            fixed += 1.0;
                        }
                        _ => {}
                    }
                }
                let i = y * width + x;
                if free {
                    coarse.state[i] = FREE;
                } else if fixed > 0.0 {
                    coarse.state[i] = FIXED;
                    coarse.value[i] = sum.map(|v| v / fixed);
                }
            }
        }
        coarse
    }

    /// Successive over-relaxation of the free cells towards their neighbours' mean.
    fn relax(&mut self, max_sweeps: usize, tolerance: f32, cancel: &Cancel) -> Result<(), EraseError> {
        let free: Vec<_> = (0..self.state.len()).filter(|i| self.state[*i] == FREE).collect();
        for sweep in 0..max_sweeps {
            if sweep % 8 == 0 {
                cancel.check()?;
            }
            let mut change = 0.0_f32;
            for &i in &free {
                let (x, y) = (i % self.width, i / self.width);
                let mut sum = [0.0; 3];
                let mut n = 0.0;
                for j in [
                    x.checked_sub(1).map(|_| i - 1),
                    (x + 1 < self.width).then_some(i + 1),
                    y.checked_sub(1).map(|_| i - self.width),
                    (y + 1 < self.height).then_some(i + self.width),
                ]
                .into_iter()
                .flatten()
                {
                    if self.state[j] != OUTSIDE {
                        sum.iter_mut().zip(self.value[j]).for_each(|(s, v)| *s += v);
                        n += 1.0;
                    }
                }
                if n == 0.0 {
                    continue;
                }
                for (value, total) in self.value[i].iter_mut().zip(sum) {
                    let step = 1.7 * (total / n - *value);
                    *value += step;
                    change = change.max(step.abs());
                }
            }
            if change < tolerance {
                break;
            }
        }
        Ok(())
    }
}

/// The hole filled with the harmonic interpolation of its surroundings, solved coarse to fine.
pub(crate) fn fill(level: &Level, cancel: &Cancel) -> Result<Vec<Color>, EraseError> {
    let state = (0..level.source.len()).map(|i| if level.hole[i] { FREE } else if level.covered[i] { FIXED } else { OUTSIDE }).collect();
    let mut grids = vec![Grid { width: level.width, height: level.height, value: level.source.clone(), state }];
    loop {
        let last = grids.last().unwrap();
        if last.width.min(last.height) <= 8 {
            break;
        }
        let coarse = last.coarsen();
        if coarse.count(FIXED) < 8 || coarse.count(FREE) == 0 {
            break;
        }
        grids.push(coarse);
    }
    let coarsest = grids.last_mut().unwrap();
    let fixed = coarsest.count(FIXED).max(1) as f32;
    let mut mean = [0.0; 3];
    for i in 0..coarsest.state.len() {
        if coarsest.state[i] == FIXED {
            mean.iter_mut().zip(coarsest.value[i]).for_each(|(m, v)| *m += v / fixed);
        }
    }
    for i in 0..coarsest.state.len() {
        if coarsest.state[i] == FREE {
            coarsest.value[i] = mean;
        }
    }
    coarsest.relax(4000, 0.01, cancel)?;
    for k in (0..grids.len() - 1).rev() {
        let (fine, coarse) = grids.split_at_mut(k + 1);
        let (fine, coarse) = (&mut fine[k], &coarse[0]);
        for y in 0..fine.height {
            for x in 0..fine.width {
                let i = y * fine.width + x;
                if fine.state[i] == FREE {
                    fine.value[i] = coarse.value[(y / 2) * coarse.width + x / 2];
                }
            }
        }
        fine.relax(400, 0.02, cancel)?;
    }
    Ok(grids.swap_remove(0).value)
}
