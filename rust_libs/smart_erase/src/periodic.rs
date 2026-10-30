// Modified from Snow Draw Engine's Smart Erase (Apache-2.0); see NOTICE.

//! Exactly repeating backgrounds: copy along a short horizontal or vertical period.

use crate::level::{distance, Color, Level};
use crate::{Cancel, EraseError};

/// Two donors one or more periods apart on either side of a hole pixel must agree to within
/// this squared RGB distance. A flat background matches every period, so without this a
/// period found there would copy an object touching the hole across it.
const SIDES_AGREE: f32 = 3.0 * 12.0 * 12.0;

pub(crate) fn fill(level: &Level, cancel: &Cancel) -> Result<Option<Vec<Color>>, EraseError> {
    let points: Vec<_> = level.domain.iter().enumerate().filter_map(|(i, d)| d.then_some(i)).collect();
    let holes = level.holes();
    for (dx, dy) in [(1_isize, 0_isize), (0, 1)] {
        for period in 2..=64 {
            if !matches(level, &points, dx, dy, period, cancel)? {
                continue;
            }
            if let Some(output) = copy(level, &holes, dx, dy, period, cancel)? {
                return Ok(Some(output));
            }
        }
    }
    Ok(None)
}

/// Whether the domain repeats with this period: the mean squared step must stay within 0.5.
fn matches(level: &Level, points: &[usize], dx: isize, dy: isize, period: isize, cancel: &Cancel) -> Result<bool, EraseError> {
    cancel.check()?;
    let mut count = 0;
    let mut error = 0.0;
    for (n, i) in points.iter().enumerate() {
        if n % 4096 == 0 {
            cancel.check()?;
        }
        let x = (i % level.width) as isize + dx * period;
        let y = (i / level.width) as isize + dy * period;
        if let Some(j) = level.at(x, y).filter(|j| level.domain[*j]) {
            count += 1;
            error += distance(level.source[*i], level.source[j]);
            // Early rejection against the largest possible final count.
            if error > points.len() as f32 * 0.5 {
                break;
            }
        }
    }
    Ok(count >= 64 && error <= count as f32 * 0.5)
}

fn copy(level: &Level, holes: &[usize], dx: isize, dy: isize, period: isize, cancel: &Cancel) -> Result<Option<Vec<Color>>, EraseError> {
    let steps = level.width.max(level.height) as isize / period + 1;
    let side = |i: usize, sign: isize| {
        (1..steps).find_map(|n| {
            let x = (i % level.width) as isize + dx * period * n * sign;
            let y = (i / level.width) as isize + dy * period * n * sign;
            level.at(x, y).filter(|j| level.domain[*j]).map(|j| (n, j))
        })
    };
    let mut output = level.source.clone();
    for (n, &i) in holes.iter().enumerate() {
        if n % 64 == 0 {
            cancel.check()?;
        }
        let donor = match (side(i, -1), side(i, 1)) {
            (Some((na, a)), Some((nb, b))) => {
                if distance(level.source[a], level.source[b]) > SIDES_AGREE {
                    return Ok(None);
                }
                if na <= nb {
                    a
                } else {
                    b
                }
            }
            (Some((_, a)), None) => a,
            (None, Some((_, b))) => b,
            (None, None) => return Ok(None),
        };
        output[i] = level.source[donor];
    }
    Ok(Some(output))
}
