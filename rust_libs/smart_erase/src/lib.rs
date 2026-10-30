//! Fills a masked region of an image from its surroundings.
//!
//! Modified from Snow Draw Engine's Smart Erase (Apache-2.0); see NOTICE.
//!
//! Cheaper exact models are tried first: an affine colour surface (flat colours and linear
//! gradients), then a verified periodic copy, then a membrane solve when no edge crosses the
//! boundary, and finally multiscale patch synthesis for textures and edges.

mod level;
mod membrane;
mod parallel;
mod patch;
mod periodic;
mod surface;

use level::{Color, Level};

/// Byte order of the four-byte pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelOrder {
    Rgba,
    Bgra,
}

/// Rows of `stride` bytes, four bytes per pixel. Pixels with zero alpha are neither erased
/// nor used as donors.
pub struct Image<'a> {
    pub pixels: &'a [u8],
    pub width: usize,
    pub height: usize,
    pub stride: usize,
    pub order: ChannelOrder,
}

/// The rectangle at (x, y) whose `mask` bytes, row by row, are nonzero where pixels are erased.
pub struct Hole<'a> {
    pub x: usize,
    pub y: usize,
    pub width: usize,
    pub height: usize,
    pub mask: &'a [u8],
}

/// Which reconstruction produced the result.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Solver {
    Unchanged,
    Surface,
    Periodic,
    Membrane,
    Patches,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EraseError {
    InvalidInput,
    /// Nothing around the hole can be sampled.
    NoDonors,
    Cancelled,
}

/// Larger donor regions are solved at an integer reduction; peak memory is about
/// 150 bytes per working pixel.
const MAX_WORKING_PIXELS: usize = 1 << 20;
/// The donor neighbourhood never reaches further than this from the hole.
const MAX_HALO: usize = 512;

pub(crate) struct Cancel<'a>(&'a dyn Fn() -> bool);

impl Cancel<'_> {
    pub(crate) fn check(&self) -> Result<(), EraseError> {
        if (self.0)() {
            Err(EraseError::Cancelled)
        } else {
            Ok(())
        }
    }
}

impl Image<'_> {
    fn pixel(&self, x: usize, y: usize) -> [u8; 4] {
        let at = y * self.stride + x * 4;
        let p = &self.pixels[at..at + 4];
        match self.order {
            ChannelOrder::Rgba => [p[0], p[1], p[2], p[3]],
            ChannelOrder::Bgra => [p[2], p[1], p[0], p[3]],
        }
    }
}

/// Writes the hole's rectangle into `out` (`width * height * 4` bytes, rows packed, the
/// image's channel order): erased pixels reconstructed, every other byte copied.
pub fn erase(image: &Image, hole: &Hole, cancelled: &dyn Fn() -> bool, out: &mut [u8]) -> Result<Solver, EraseError> {
    validate(image, hole, out)?;
    let cancel = Cancel(cancelled);
    for row in 0..hole.height {
        let at = (hole.y + row) * image.stride + hole.x * 4;
        out[row * hole.width * 4..(row + 1) * hole.width * 4].copy_from_slice(&image.pixels[at..at + hole.width * 4]);
    }
    let erased = |x: usize, y: usize| {
        x >= hole.x
            && y >= hole.y
            && x < hole.x + hole.width
            && y < hole.y + hole.height
            && hole.mask[(y - hole.y) * hole.width + x - hole.x] != 0
            && image.pixel(x, y)[3] != 0
    };
    let mut bounds: Option<[usize; 4]> = None;
    for y in hole.y..hole.y + hole.height {
        for x in hole.x..hole.x + hole.width {
            if erased(x, y) {
                let b = bounds.get_or_insert([x, y, x + 1, y + 1]);
                *b = [b[0].min(x), b[1].min(y), b[2].max(x + 1), b[3].max(y + 1)];
            }
        }
    }
    let Some(bounds) = bounds else {
        return Ok(Solver::Unchanged);
    };
    let regions = Regions { mask: bounds, size: [image.width, image.height] };
    let maximum = regions.unaligned(MAX_HALO);
    let mut halo = regions.initial_halo();
    loop {
        cancel.check()?;
        let region = regions.region(halo);
        let scale = reduction(region);
        let level = Level::sample(image, &erased, region, scale, maximum[2].max(maximum[3]), &cancel)?;
        // Sides that can still grow before reaching the image edge or the maximum halo.
        let growable = [
            region[0] > maximum[0],
            region[1] > maximum[1],
            region[0] + region[2] < maximum[0] + maximum[2],
            region[1] + region[3] < maximum[1] + maximum[3],
        ];
        let expandable = halo < MAX_HALO && growable.iter().any(|g| *g);
        match reconstruct(level, expandable, growable, &cancel)? {
            Outcome::Expand => halo = (halo * 2).min(MAX_HALO),
            Outcome::Filled(level, colors, solver) => {
                write(image, hole, &erased, region, scale, &level, &colors, out);
                return Ok(solver);
            }
        }
    }
}

fn validate(image: &Image, hole: &Hole, out: &[u8]) -> Result<(), EraseError> {
    let rows_fit = image.height == 0
        || image.stride.checked_mul(image.height - 1).and_then(|n| n.checked_add(image.width.checked_mul(4)?)).is_some_and(|n| n <= image.pixels.len());
    let valid = image.width > 0
        && image.height > 0
        && image.stride >= image.width * 4
        && rows_fit
        && hole.width > 0
        && hole.height > 0
        && hole.x.checked_add(hole.width).is_some_and(|r| r <= image.width)
        && hole.y.checked_add(hole.height).is_some_and(|b| b <= image.height)
        && hole.mask.len() == hole.width * hole.height
        && out.len() == hole.width * hole.height * 4;
    if valid {
        Ok(())
    } else {
        Err(EraseError::InvalidInput)
    }
}

/// The smallest integer reduction that keeps the working grid within the budget.
fn reduction(region: [usize; 4]) -> usize {
    let mut scale = 1;
    while region[2].div_ceil(scale) * region[3].div_ceil(scale) > MAX_WORKING_PIXELS {
        scale += 1;
    }
    scale
}

/// Donor neighbourhood policy: start near the hole and grow only when the solve asks for it.
struct Regions {
    mask: [usize; 4],
    size: [usize; 2],
}

impl Regions {
    fn initial_halo(&self) -> usize {
        (((self.mask[2] - self.mask[0]).min(self.mask[3] - self.mask[1]) / 2).clamp(16, 96) + 32).max(64)
    }

    /// [x, y, width, height] of the hole bounds grown by `halo`, clipped to the image.
    fn unaligned(&self, halo: usize) -> [usize; 4] {
        let left = self.mask[0].saturating_sub(halo);
        let top = self.mask[1].saturating_sub(halo);
        let right = (self.mask[2] + halo).min(self.size[0]);
        let bottom = (self.mask[3] + halo).min(self.size[1]);
        [left, top, right - left, bottom - top]
    }

    /// Aligned to a 32-pixel grid from the maximum region, so nearby holes share regions.
    fn region(&self, halo: usize) -> [usize; 4] {
        let [x, y, w, h] = self.unaligned(halo);
        let [mx, my, mw, mh] = self.unaligned(MAX_HALO);
        let left = mx + (x - mx) / 32 * 32;
        let top = my + (y - my) / 32 * 32;
        let right = (mx + (x + w - mx).div_ceil(32) * 32).min(mx + mw);
        let bottom = (my + (y + h - my).div_ceil(32) * 32).min(my + mh);
        [left, top, right - left, bottom - top]
    }
}

enum Outcome {
    Expand,
    Filled(Level, Vec<Color>, Solver),
}

fn reconstruct(mut level: Level, expandable: bool, growable: [bool; 4], cancel: &Cancel) -> Result<Outcome, EraseError> {
    if !(0..level.hole.len()).any(|i| level.known(i)) {
        return if expandable { Ok(Outcome::Expand) } else { Err(EraseError::NoDonors) };
    }
    // A narrow ring first leaves out content just beside the hole (the next line of text, a
    // neighbouring bubble); the wide one then tolerates antialiased fringes the hole left behind.
    let narrow = level.ring(2, cancel)?;
    if let Some(colors) = surface::fill(&level, &narrow, cancel)? {
        return Ok(Outcome::Filled(level, colors, Solver::Surface));
    }
    let ring = level.ring(4, cancel)?;
    if let Some(colors) = surface::fill(&level, &ring, cancel)? {
        return Ok(Outcome::Filled(level, colors, Solver::Surface));
    }
    let reach = level.make_domain(cancel)?;
    // Guides sample up to sixteen pixels beyond donor centres: grow the region before
    // accepting a domain cut short by its border.
    if expandable && level.near_border(reach + 16, growable) {
        return Ok(Outcome::Expand);
    }
    if let Some(colors) = periodic::fill(&level, cancel)? {
        return Ok(Outcome::Filled(level, colors, Solver::Periodic));
    }
    if membrane::smooth(&level, &ring) {
        let colors = membrane::fill(&level, cancel)?;
        return Ok(Outcome::Filled(level, colors, Solver::Membrane));
    }
    let colors = patch::multiscale(&level, cancel)?;
    Ok(Outcome::Filled(level, colors, Solver::Patches))
}

#[allow(clippy::too_many_arguments)]
fn write(
    image: &Image,
    hole: &Hole,
    erased: &dyn Fn(usize, usize) -> bool,
    region: [usize; 4],
    scale: usize,
    level: &Level,
    colors: &[Color],
    out: &mut [u8],
) {
    for y in hole.y..hole.y + hole.height {
        for x in hole.x..hole.x + hole.width {
            if !erased(x, y) {
                continue;
            }
            let color = if scale == 1 {
                colors[(y - region[1]) * level.width + x - region[0]]
            } else {
                // Pixel centres of the reduced grid sit at (i + 0.5) * scale.
                let fx = ((x - region[0]) as f32 + 0.5) / scale as f32 - 0.5;
                let fy = ((y - region[1]) as f32 + 0.5) / scale as f32 - 0.5;
                level.bilinear(colors, fx, fy)
            };
            let rgb = color.map(|v| v.round().clamp(0.0, 255.0) as u8);
            let at = ((y - hole.y) * hole.width + x - hole.x) * 4;
            let bytes = match image.order {
                ChannelOrder::Rgba => [rgb[0], rgb[1], rgb[2]],
                ChannelOrder::Bgra => [rgb[2], rgb[1], rgb[0]],
            };
            out[at..at + 3].copy_from_slice(&bytes);
        }
    }
}

#[cfg(test)]
mod tests;
