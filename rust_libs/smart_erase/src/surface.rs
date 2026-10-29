// Modified from Snow Draw Engine's Smart Erase (Apache-2.0); see NOTICE.

//! Flat colours and linear gradients: an affine colour model fitted to the ring.

use crate::level::{Color, Level};
use crate::{Cancel, EraseError};

/// The hole filled by `c = a + b x + d y` per channel, or None when the ring does not
/// follow such a surface. The fit is robust to a few foreground or antialiasing pixels.
pub(crate) fn fill(level: &Level, ring: &[usize], cancel: &Cancel) -> Result<Option<Vec<Color>>, EraseError> {
    if ring.len() < 12 {
        return Ok(None);
    }
    let w = level.width;
    let [x0, y0, x1, y1] = level.hole_bounds();
    let basis = |i: usize| [1.0, ((i % w) as f64 - x0 as f64) / (x1 - x0) as f64, ((i / w) as f64 - y0 as f64) / (y1 - y0) as f64];
    let predict = |b: [f64; 3], c: [[f64; 3]; 3]| -> [f64; 3] { std::array::from_fn(|channel| (0..3).map(|j| b[j] * c[j][channel]).sum()) };
    let observed = |i: usize| level.source[i].map(|v| f64::from(v) / 255.0);
    let stride = (ring.len() / 4096).max(1);
    let mut coefficients = [[0.0_f64; 3]; 3];
    // Iteratively reweighted least squares: residuals above about 2/255 lose weight.
    for iteration in 0..5 {
        cancel.check()?;
        let mut normal = [[0.0; 3]; 3];
        let mut rhs = [[0.0; 3]; 3];
        for &i in ring.iter().step_by(stride) {
            let b = basis(i);
            let value = observed(i);
            let p = predict(b, coefficients);
            let residual = (0..3).map(|c| (p[c] - value[c]).powi(2)).sum::<f64>().sqrt();
            let weight = if iteration == 0 { 1.0 } else { (0.008 / residual.max(1e-8)).min(1.0) };
            for a in 0..3 {
                for c in 0..3 {
                    normal[a][c] += weight * b[a] * b[c];
                    rhs[a][c] += weight * b[a] * value[c];
                }
            }
        }
        let Some(solution) = solve(normal, rhs) else {
            return Ok(None);
        };
        coefficients = solution;
    }
    // Accept only when 97% of the ring is within 4/255 and the inliers fit tightly.
    let (mut inliers, mut count, mut error) = (0_usize, 0_usize, 0.0);
    for &i in ring.iter().step_by(stride) {
        count += 1;
        let p = predict(basis(i), coefficients);
        let value = observed(i);
        let e = (0..3).map(|c| (p[c] - value[c]).powi(2)).sum::<f64>();
        if e < (4.0_f64 / 255.0).powi(2) {
            inliers += 1;
            error += e;
        }
    }
    if inliers * 100 < count * 97 || error > inliers as f64 * 6.0 / (255.0 * 255.0) {
        return Ok(None);
    }
    let mut out = level.source.clone();
    for i in level.holes() {
        out[i] = predict(basis(i), coefficients).map(|v| (v * 255.0) as f32);
    }
    Ok(Some(out))
}

/// Gauss-Jordan elimination with partial pivoting for three right-hand sides.
fn solve(mut a: [[f64; 3]; 3], mut b: [[f64; 3]; 3]) -> Option<[[f64; 3]; 3]> {
    for k in 0..3 {
        let p = (k..3).max_by(|i, j| a[*i][k].abs().total_cmp(&a[*j][k].abs()))?;
        if a[p][k].abs() < 1e-12 {
            return None;
        }
        a.swap(k, p);
        b.swap(k, p);
        let pivot = a[k][k];
        for c in 0..3 {
            a[k][c] /= pivot;
            b[k][c] /= pivot;
        }
        for r in 0..3 {
            if r != k {
                let f = a[r][k];
                for c in 0..3 {
                    a[r][c] -= f * a[k][c];
                    b[r][c] -= f * b[k][c];
                }
            }
        }
    }
    Some(b)
}
