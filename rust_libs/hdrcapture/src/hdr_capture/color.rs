//! CPU-side color conversion helpers for HDR capture fallbacks.
//!
//! The normal HDR path should do this work on the GPU.  These functions are deliberately free of
//! Windows and Direct3D types so they can also be used for a CPU readback fallback and tested on
//! non-Windows hosts.

use std::fmt;

/// A linear RGB triplet with red, green, and blue components in that order.
pub type LinearRgb = [f32; 3];

const REC2020_TO_SRGB: [[f32; 3]; 3] = [
    [1.660_491, -0.587_641_1, -0.072_849_86],
    [-0.124_550_47, 1.132_899_9, -0.008_349_422],
    [-0.018_150_764, -0.100_578_9, 1.118_729_7],
];

const RGBA16F_BYTES_PER_PIXEL: usize = 8;
const BGRA8_BYTES_PER_PIXEL: usize = 4;
const MAX_FINITE_F16: f32 = 65_504.0;

/// Identifies the RGB primaries used by an `RGBA16F` source buffer.
///
/// Both variants represent linear-light values.  Transfer-function decoding therefore must not be
/// applied before passing values to [`rgba16f_to_bgra8`] or [`rgba16f_to_bgra8_into`].
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum Rgba16fColorSpace {
    /// Windows scRGB: linear sRGB / Rec.709 primaries with extended HDR component values.
    #[default]
    ScRgb,
    /// Linear Rec.2020 primaries with a D65 white point.
    Rec2020,
}

/// Selects quantization dithering for 8-bit output.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum DitherMode {
    /// Quantize each channel by rounding to the nearest 8-bit code value.
    #[default]
    None,
    /// Apply a deterministic 4 by 4 Bayer ordered-dither pattern to RGB channels.
    ///
    /// Alpha is never dithered.  The pattern is anchored at the top-left corner of each conversion;
    /// separate tile conversions restart the pattern at each tile's origin.
    Ordered4x4,
}

/// Errors produced while validating an `RGBA16F` input buffer or BGRA8 output buffer.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Error {
    /// Width, height, stride, or output-length arithmetic overflowed `usize`.
    DimensionOverflow,
    /// A source row pitch cannot hold the requested number of 16-bit-float RGBA pixels.
    SourceRowPitchTooSmall {
        /// The minimum packed source row size in bytes.
        expected: usize,
        /// The supplied source row pitch in bytes.
        actual: usize,
    },
    /// The source buffer ends before the final requested pixel.
    SourceBufferTooSmall {
        /// The minimum source buffer size in bytes, including row padding before the last row.
        expected: usize,
        /// The supplied source buffer length in bytes.
        actual: usize,
    },
    /// The destination buffer cannot hold the tightly packed BGRA8 result.
    DestinationBufferTooSmall {
        /// The minimum destination buffer size in bytes.
        expected: usize,
        /// The supplied destination buffer length in bytes.
        actual: usize,
    },
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::DimensionOverflow => formatter.write_str("image dimensions or row pitch overflow usize"),
            Self::SourceRowPitchTooSmall { expected, actual } => {
                write!(
                    formatter,
                    "RGBA16F source row pitch is too small: expected at least {expected} bytes, got {actual}"
                )
            }
            Self::SourceBufferTooSmall { expected, actual } => {
                write!(
                    formatter,
                    "RGBA16F source buffer is too small: expected at least {expected} bytes, got {actual}"
                )
            }
            Self::DestinationBufferTooSmall { expected, actual } => {
                write!(
                    formatter,
                    "BGRA8 destination buffer is too small: expected at least {expected} bytes, got {actual}"
                )
            }
        }
    }
}

impl std::error::Error for Error {}

/// Converts a linear Rec.2020 RGB value to linear sRGB / Rec.709.
///
/// The matrix assumes a D65 white point for both color spaces.  It intentionally does not clamp
/// the result: a valid Rec.2020 color can lie outside the sRGB gamut and produce negative or
/// greater-than-one components.  [`tone_map_highlights`] clamps those values safely when making
/// an SDR image.
#[inline]
#[must_use]
pub fn linear_rec2020_to_linear_srgb(rgb: LinearRgb) -> LinearRgb {
    [
        REC2020_TO_SRGB[0][0].mul_add(rgb[0], REC2020_TO_SRGB[0][1].mul_add(rgb[1], REC2020_TO_SRGB[0][2] * rgb[2])),
        REC2020_TO_SRGB[1][0].mul_add(rgb[0], REC2020_TO_SRGB[1][1].mul_add(rgb[1], REC2020_TO_SRGB[1][2] * rgb[2])),
        REC2020_TO_SRGB[2][0].mul_add(rgb[0], REC2020_TO_SRGB[2][1].mul_add(rgb[1], REC2020_TO_SRGB[2][2] * rgb[2])),
    ]
}

/// Tone-maps linear HDR RGB to the normalized linear SDR range, relative to this desktop's white.
///
/// `sdr_white_level` is where the capture's scRGB samples place the white the user actually sees,
/// as reported by [`crate::hdr_capture::display::MonitorDescriptor::sdr_white_level`]. Dividing by
/// it first is what makes the result independent of the machine: Windows scales SDR desktop
/// content by its "SDR content brightness" slider, so without this step the same screen yields a
/// different screenshot on every display. A non-positive or non-finite value is treated as `1.0`.
///
/// After normalization, a pixel whose brightest component is at most desktop white is returned
/// unchanged, so SDR desktop content matches a GDI capture bit for bit. 8-bit sRGB has no code
/// values above white, so keeping SDR exact leaves no room to grade highlights: a brighter pixel is
/// divided by its brightest component, which lands that component on white and keeps the ratios
/// between components, and therefore the hue, instead of clamping each component separately.
/// Negative values and NaN clamp to black, and positive infinity maps to white.
#[inline]
#[must_use]
pub fn tone_map_highlights(rgb: LinearRgb, sdr_white_level: f32) -> LinearRgb {
    let normalized = normalize_to_white(rgb, sdr_white_level);
    let peak = max_component(normalized);
    if peak <= 1.0 {
        return normalized;
    }
    // Divide rather than multiply by `1 / peak`: the reciprocal of a huge peak is subnormal and
    // would leave the brightest component just short of 1.0.
    normalized.map(|value| normalize_unit(value / peak))
}

/// Linear level, relative to desktop white, above which a pixel counts as HDR content.
///
/// FP16 rounding already puts SDR white a little above 1.0, so the threshold sits 2 % higher.
/// Must stay identical to `HDR_CONTENT_THRESHOLD` in the GPU shader.
pub const HDR_CONTENT_THRESHOLD: f32 = 1.02;

/// Minimum share of a frame's pixels that must be HDR content before adaptive tone mapping
/// darkens the frame, so a handful of stray pixels cannot dim a whole screenshot.
const MIN_HDR_PIXEL_FRACTION: f64 = 0.000_1;
/// A statistics tile contributes its peak only when it holds at least this many HDR pixels, so a
/// lone hot pixel cannot set the curve.
const MIN_TILE_HDR_PIXELS: f32 = 4.0;
/// Quantile of the qualifying tile peaks taken as the frame peak. The few brightest tiles, such as
/// a sun disc, are left to clip instead of darkening everything else to make room for them.
const PEAK_TILE_QUANTILE: f32 = 0.95;

/// GPU statistics for one tile: its brightest component relative to desktop white, and how many
/// of its pixels exceed [`HDR_CONTENT_THRESHOLD`].
pub type TileStats = [f32; 2];

/// Picks the peak, relative to desktop white, that adaptive tone mapping should fit into the
/// output, or `None` when the frame holds too little HDR content to justify darkening it.
#[must_use]
pub fn adaptive_peak(tiles: &[TileStats], pixel_count: u64) -> Option<f32> {
    let hdr_pixels: f64 = tiles.iter().map(|tile| f64::from(tile[1])).sum();
    if hdr_pixels == 0.0 || hdr_pixels < pixel_count as f64 * MIN_HDR_PIXEL_FRACTION {
        return None;
    }

    let mut peaks: Vec<f32> = tiles
        .iter()
        .filter(|tile| tile[1] >= MIN_TILE_HDR_PIXELS)
        .map(|tile| sanitize_linear(tile[0]))
        .collect();
    if peaks.is_empty() {
        return None;
    }
    peaks.sort_by(f32::total_cmp);
    let rank = (peaks.len() as f32 * PEAK_TILE_QUANTILE).ceil() as usize;
    Some(peaks[rank.clamp(1, peaks.len()) - 1])
}

/// SMPTE ST 2094-50 reference-white tone curve, the one Skia uses to render HDR gain-map images on
/// SDR displays.
///
/// It fits `[0, peak]` (relative to desktop white) into `[0, 1]`: everything up to desktop white is
/// scaled by `output_white`, and the range above it follows a quadratic Bézier that reaches 1.0 at
/// the peak. The bigger the peak, the more desktop white is darkened, down to half at a 1000-nit
/// peak over the BT.2408 203-nit reference white.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReferenceWhiteCurve {
    input_maximum: f32,
    output_white: f32,
    xa: f32,
    xb: f32,
    ya: f32,
    yb: f32,
}

impl ReferenceWhiteCurve {
    const KAPPA: f32 = 0.65;

    /// Builds the curve for a frame whose content peaks at `peak` times desktop white.
    #[must_use]
    pub fn new(peak: f32) -> Self {
        let input_maximum = if peak.is_nan() { 1.000_1 } else { peak.clamp(1.000_1, 64.0) };
        let reference_headroom = (1000.0_f32 / 203.0).log2();
        let compression = (input_maximum.log2() / reference_headroom).min(1.0);
        let output_white = 0.5_f32.mul_add(-compression, 1.0);
        let x_middle = (1.0 - Self::KAPPA) + Self::KAPPA / output_white;
        let y_middle = (1.0 - Self::KAPPA).mul_add(output_white, Self::KAPPA);
        Self {
            input_maximum,
            output_white,
            xa: 2.0_f32.mul_add(-x_middle, 1.0) + input_maximum,
            xb: 2.0_f32.mul_add(x_middle, -2.0),
            ya: 2.0_f32.mul_add(-y_middle, output_white) + 1.0,
            yb: 2.0_f32.mul_add(y_middle, -2.0 * output_white),
        }
    }

    /// Where desktop white lands, as a linear fraction of output white.
    #[must_use]
    pub const fn output_white(&self) -> f32 {
        self.output_white
    }

    /// Gain to apply to all three components of a pixel whose brightest component is
    /// `max_component`. Must stay identical to `reference_white_gain` in the GPU shader.
    #[must_use]
    pub fn gain(&self, max_component: f32) -> f32 {
        if max_component <= 1.0 {
            return self.output_white;
        }
        if max_component >= self.input_maximum {
            return 1.0 / max_component;
        }
        // Solve x(t) = 1 + xb t + xa t^2 for t, then evaluate y(t) = output_white + yb t + ya t^2.
        let t = if self.xa.abs() < 0.000_01 {
            (max_component - 1.0) / self.xb
        } else {
            let discriminant = self.xb.mul_add(self.xb, -4.0 * self.xa * (1.0 - max_component));
            (-self.xb + discriminant.max(0.0).sqrt()) / (2.0 * self.xa)
        };
        let t = t.clamp(0.0, 1.0);
        t.mul_add(t.mul_add(self.ya, self.yb), self.output_white) / max_component
    }

    /// Values for the shader's `AdaptiveToneMap` constant buffer, in declaration order.
    #[must_use]
    pub const fn shader_constants(&self) -> [f32; 8] {
        [1.0, self.input_maximum, self.output_white, self.xa, self.xb, self.ya, self.yb, 0.0]
    }
}

/// Adaptive counterpart of [`tone_map_highlights`]: the readable reference for the shader path
/// used when a frame carries HDR content. The same gain scales all three components, so hue is
/// kept.
#[inline]
#[must_use]
pub fn tone_map_adaptive(rgb: LinearRgb, sdr_white_level: f32, curve: &ReferenceWhiteCurve) -> LinearRgb {
    let normalized = normalize_to_white(rgb, sdr_white_level);
    let gain = curve.gain(max_component(normalized));
    normalized.map(|value| normalize_unit(value * gain))
}

#[inline]
fn normalize_to_white(rgb: LinearRgb, sdr_white_level: f32) -> LinearRgb {
    let scale = if sdr_white_level.is_finite() && sdr_white_level > 0.0 { sdr_white_level } else { 1.0 };
    rgb.map(|value| sanitize_linear(value / scale))
}

#[inline]
fn max_component(rgb: LinearRgb) -> f32 {
    rgb[0].max(rgb[1]).max(rgb[2])
}

/// Encodes a normalized linear-sRGB component with the sRGB opto-electronic transfer function.
///
/// Values below zero and NaN become zero; values above one and positive infinity become one.
/// Clamp only after tone mapping when converting HDR pixels, since clipping HDR values before
/// [`tone_map_highlights`] would discard highlight detail.
#[inline]
#[must_use]
pub fn srgb_encode(linear: f32) -> f32 {
    let linear = normalize_unit(linear);

    if linear >= 1.0 {
        return 1.0;
    }

    if linear <= 0.003_130_8 {
        linear * 12.92
    } else {
        normalize_unit(1.055_f32.mul_add(linear.powf(1.0 / 2.4), -0.055))
    }
}

/// Converts a little-endian `RGBA16F` image into a tightly packed BGRA8 image.
///
/// `source_row_pitch` is the number of bytes between successive source rows and may include
/// Direct3D readback padding.  The returned buffer is always tightly packed (`width * height *
/// 4` bytes), with channels in blue, green, red, alpha order.  `ScRgb` sources use linear sRGB
/// primaries directly; `Rec2020` sources are first converted by
/// [`linear_rec2020_to_linear_srgb`].  RGB is tone-mapped, sRGB-encoded, and then quantized;
/// alpha is simply clamped to `[0, 1]` and quantized.
///
/// The input parser explicitly reads little-endian half-float words, independent of host
/// endianness.  It accepts trailing bytes after the final pixel and ignores source row padding.
///
/// # Errors
///
/// Returns [`Error`] if dimensions overflow, a row pitch cannot contain a packed row, or the
/// source buffer is too short.
pub fn rgba16f_to_bgra8(
    source: &[u8],
    width: u32,
    height: u32,
    source_row_pitch: usize,
    color_space: Rgba16fColorSpace,
    sdr_white_level: f32,
    dither: DitherMode,
) -> Result<Vec<u8>, Error> {
    let layout = validate_layout(source, width, height, source_row_pitch)?;
    let mut destination = vec![0; layout.destination_len];

    convert_into(source, &mut destination, source_row_pitch, color_space, sdr_white_level, dither, layout);

    Ok(destination)
}

/// Converts a little-endian `RGBA16F` image into a caller-owned BGRA8 buffer.
///
/// This is the allocation-free counterpart of [`rgba16f_to_bgra8`].  The first `width * height *
/// 4` bytes of `destination` receive a tightly packed BGRA8 image; any remaining bytes are left
/// untouched.  See [`rgba16f_to_bgra8`] for the color conversion and byte-order rules.
///
/// # Errors
///
/// Returns [`Error`] for malformed source dimensions or buffers, or when `destination` is too
/// small for the tightly packed result.
#[allow(clippy::too_many_arguments)]
pub fn rgba16f_to_bgra8_into(
    source: &[u8],
    width: u32,
    height: u32,
    source_row_pitch: usize,
    destination: &mut [u8],
    color_space: Rgba16fColorSpace,
    sdr_white_level: f32,
    dither: DitherMode,
) -> Result<(), Error> {
    let layout = validate_layout(source, width, height, source_row_pitch)?;

    if destination.len() < layout.destination_len {
        return Err(Error::DestinationBufferTooSmall { expected: layout.destination_len, actual: destination.len() });
    }

    convert_into(source, destination, source_row_pitch, color_space, sdr_white_level, dither, layout);

    Ok(())
}

#[derive(Clone, Copy)]
struct ImageLayout {
    width: usize,
    height: usize,
    destination_len: usize,
}

fn validate_layout(source: &[u8], width: u32, height: u32, source_row_pitch: usize) -> Result<ImageLayout, Error> {
    let width = usize::try_from(width).map_err(|_| Error::DimensionOverflow)?;
    let height = usize::try_from(height).map_err(|_| Error::DimensionOverflow)?;
    let destination_len = width
        .checked_mul(height)
        .and_then(|pixel_count| pixel_count.checked_mul(BGRA8_BYTES_PER_PIXEL))
        .ok_or(Error::DimensionOverflow)?;

    if width == 0 || height == 0 {
        return Ok(ImageLayout { width, height, destination_len });
    }

    let packed_source_row_len = width.checked_mul(RGBA16F_BYTES_PER_PIXEL).ok_or(Error::DimensionOverflow)?;
    if source_row_pitch < packed_source_row_len {
        return Err(Error::SourceRowPitchTooSmall { expected: packed_source_row_len, actual: source_row_pitch });
    }

    let required_source_len = source_row_pitch
        .checked_mul(height - 1)
        .and_then(|before_last_row| before_last_row.checked_add(packed_source_row_len))
        .ok_or(Error::DimensionOverflow)?;
    if source.len() < required_source_len {
        return Err(Error::SourceBufferTooSmall { expected: required_source_len, actual: source.len() });
    }

    Ok(ImageLayout { width, height, destination_len })
}

fn convert_into(
    source: &[u8],
    destination: &mut [u8],
    source_row_pitch: usize,
    color_space: Rgba16fColorSpace,
    sdr_white_level: f32,
    dither: DitherMode,
    layout: ImageLayout,
) {
    for y in 0..layout.height {
        let source_row_offset = y * source_row_pitch;
        let destination_row_offset = y * layout.width * BGRA8_BYTES_PER_PIXEL;

        for x in 0..layout.width {
            let source_pixel_offset = source_row_offset + x * RGBA16F_BYTES_PER_PIXEL;
            let destination_pixel_offset = destination_row_offset + x * BGRA8_BYTES_PER_PIXEL;

            let rgba = [
                sanitize_hdr_sample(read_f16_le(source, source_pixel_offset)),
                sanitize_hdr_sample(read_f16_le(source, source_pixel_offset + 2)),
                sanitize_hdr_sample(read_f16_le(source, source_pixel_offset + 4)),
                read_f16_le(source, source_pixel_offset + 6),
            ];
            let linear_srgb = match color_space {
                Rgba16fColorSpace::ScRgb => [rgba[0], rgba[1], rgba[2]],
                Rgba16fColorSpace::Rec2020 => linear_rec2020_to_linear_srgb([rgba[0], rgba[1], rgba[2]]),
            };
            let encoded = tone_map_highlights(linear_srgb, sdr_white_level).map(srgb_encode);

            destination[destination_pixel_offset] = quantize_to_u8(encoded[2], x, y, dither);
            destination[destination_pixel_offset + 1] = quantize_to_u8(encoded[1], x, y, dither);
            destination[destination_pixel_offset + 2] = quantize_to_u8(encoded[0], x, y, dither);
            destination[destination_pixel_offset + 3] = quantize_to_u8(rgba[3], x, y, DitherMode::None);
        }
    }
}

#[inline]
fn sanitize_linear(value: f32) -> f32 {
    if value.is_nan() || value <= 0.0 {
        0.0
    } else {
        value.min(f32::MAX)
    }
}

#[inline]
fn normalize_unit(value: f32) -> f32 {
    if value.is_nan() || value <= 0.0 {
        0.0
    } else if value >= 1.0 {
        1.0
    } else {
        value
    }
}

#[inline]
fn sanitize_hdr_sample(value: f32) -> f32 {
    if value.is_nan() {
        0.0
    } else if value == f32::INFINITY {
        MAX_FINITE_F16
    } else if value == f32::NEG_INFINITY {
        -MAX_FINITE_F16
    } else {
        value
    }
}

#[inline]
fn quantize_to_u8(value: f32, x: usize, y: usize, dither: DitherMode) -> u8 {
    let code_value = normalize_unit(value) * 255.0;
    let quantized = match dither {
        DitherMode::None => code_value.round(),
        DitherMode::Ordered4x4 => {
            let threshold = (f32::from(BAYER_4X4[y & 3][x & 3]) + 0.5) / 16.0;
            (code_value + threshold).floor()
        }
    };

    quantized.clamp(0.0, 255.0) as u8
}

const BAYER_4X4: [[u8; 4]; 4] = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]];

#[inline]
fn read_f16_le(source: &[u8], offset: usize) -> f32 {
    let bits = u16::from_le_bytes([source[offset], source[offset + 1]]);
    f16_bits_to_f32(bits)
}

/// Decodes IEEE 754 binary16 bits without relying on the unstable primitive `f16` type.
#[inline]
fn f16_bits_to_f32(bits: u16) -> f32 {
    let sign = u32::from(bits & 0x8000) << 16;
    let exponent = (bits >> 10) & 0x1f;
    let fraction = u32::from(bits & 0x03ff);

    let f32_bits = match exponent {
        0 if fraction == 0 => sign,
        0 => {
            let mut normalized_fraction = fraction;
            let mut unbiased_exponent = -14_i32;
            while normalized_fraction & 0x0400 == 0 {
                normalized_fraction <<= 1;
                unbiased_exponent -= 1;
            }

            let f32_exponent = u32::try_from(unbiased_exponent + 127)
                .expect("a normalized binary16 subnormal always has a representable binary32 exponent");
            sign | (f32_exponent << 23) | ((normalized_fraction & 0x03ff) << 13)
        }
        0x1f => sign | 0x7f80_0000 | (fraction << 13),
        _ => sign | ((u32::from(exponent) + 112) << 23) | (fraction << 13),
    };

    f32::from_bits(f32_bits)
}

#[cfg(test)]
mod tests {
    use super::{
        DitherMode, Error, ReferenceWhiteCurve, Rgba16fColorSpace, adaptive_peak, f16_bits_to_f32,
        linear_rec2020_to_linear_srgb, quantize_to_u8, rgba16f_to_bgra8, rgba16f_to_bgra8_into, srgb_encode,
        tone_map_adaptive, tone_map_highlights,
    };

    fn assert_close(actual: f32, expected: f32, tolerance: f32) {
        assert!((actual - expected).abs() <= tolerance, "expected {expected} +/- {tolerance}, got {actual}");
    }

    fn append_f16_bits(buffer: &mut Vec<u8>, bits: u16) {
        buffer.extend_from_slice(&bits.to_le_bytes());
    }

    fn append_pixel(buffer: &mut Vec<u8>, red: u16, green: u16, blue: u16, alpha: u16) {
        append_f16_bits(buffer, red);
        append_f16_bits(buffer, green);
        append_f16_bits(buffer, blue);
        append_f16_bits(buffer, alpha);
    }

    #[test]
    fn decodes_binary16_values_and_special_values() {
        assert_close(f16_bits_to_f32(0x0001), 2_f32.powi(-24), 0.0);
        assert_close(f16_bits_to_f32(0x03ff), 0.000_060_975_55, 0.000_000_01);
        assert_close(f16_bits_to_f32(0x3c00), 1.0, 0.0);
        assert_close(f16_bits_to_f32(0xc000), -2.0, 0.0);
        assert!(f16_bits_to_f32(0x7c00).is_infinite());
        assert!(f16_bits_to_f32(0x7e00).is_nan());
    }

    #[test]
    fn rec2020_matrix_preserves_d65_neutral_and_maps_red() {
        let neutral = linear_rec2020_to_linear_srgb([1.0, 1.0, 1.0]);
        assert_close(neutral[0], 1.0, 0.000_001);
        assert_close(neutral[1], 1.0, 0.000_001);
        assert_close(neutral[2], 1.0, 0.000_001);

        let red = linear_rec2020_to_linear_srgb([1.0, 0.0, 0.0]);
        assert_close(red[0], 1.660_491, 0.000_001);
        assert_close(red[1], -0.124_550_47, 0.000_001);
        assert_close(red[2], -0.018_150_764, 0.000_001);
    }

    #[test]
    fn tone_mapper_preserves_desktop_content_and_keeps_highlight_hue() {
        let preserved = tone_map_highlights([0.0, 0.18, 0.9], 1.0);
        assert_eq!(preserved, [0.0, 0.18, 0.9]);
        assert_eq!(tone_map_highlights([1.0, 1.0, 1.0], 1.0), [1.0, 1.0, 1.0]);

        // 8-bit output has nothing above white, so a neutral highlight is white...
        assert_eq!(tone_map_highlights([3.0, 3.0, 3.0], 1.0), [1.0, 1.0, 1.0]);
        // ...and a colored one keeps its component ratios rather than clamping to (1, 1, 0).
        let orange = tone_map_highlights([4.0, 2.0, 0.0], 1.0);
        assert_close(orange[0], 1.0, 0.0);
        assert_close(orange[1], 0.5, 0.000_001);
        assert_close(orange[2], 0.0, 0.0);

        let sanitized = tone_map_highlights([-1.0, f32::NAN, f32::INFINITY], 1.0);
        assert_eq!(sanitized, [0.0, 0.0, 1.0]);
    }

    #[test]
    fn desktop_content_survives_the_scrgb_round_trip_bit_exact() {
        // Windows composes an SDR pixel into FP16 scRGB as its linear value times the SDR white
        // level. Every 8-bit code must come back unchanged, or captures stop matching GDI and
        // picked colors are off; desktop white landing a hair above 1.0 is the fragile case.
        fn srgb_decode(encoded: f32) -> f32 {
            if encoded <= 0.040_45 { encoded / 12.92 } else { ((encoded + 0.055) / 1.055).powf(2.4) }
        }
        fn round_to_f16(value: f32) -> f32 {
            f32::from_bits((value.to_bits() + 0x1000) & !0x1fff)
        }

        for white_level in [1.0_f32, 2.5, 4.1, 6.0] {
            for code in 0_u8..=255 {
                let linear = round_to_f16(srgb_decode(f32::from(code) / 255.0) * white_level);
                let mapped = tone_map_highlights([linear, linear, linear], white_level)[0];
                let output = quantize_to_u8(srgb_encode(mapped), 0, 0, DitherMode::None);
                assert_eq!(output, code, "white level {white_level}");
            }
        }
    }

    #[test]
    fn tone_mapper_is_independent_of_the_desktop_white_level() {
        // The same desktop white must produce the same screenshot pixel whatever the Windows
        // "SDR content brightness" slider placed it at. This is the whole point of normalizing.
        let reference = tone_map_highlights([1.0, 0.5, 0.18], 1.0);
        for scale in [1.0_f32, 2.5, 3.8, 12.0] {
            let scaled = tone_map_highlights([scale, 0.5 * scale, 0.18 * scale], scale);
            assert_close(scaled[0], reference[0], 0.000_001);
            assert_close(scaled[1], reference[1], 0.000_001);
            assert_close(scaled[2], reference[2], 0.000_001);
        }

        // A nonsense reading must not silently blank or blow out the capture.
        for bogus in [0.0_f32, -1.0, f32::NAN, f32::INFINITY] {
            let fallback = tone_map_highlights([0.18, 0.18, 0.18], bogus);
            assert_close(fallback[0], 0.18, 0.000_001);
        }
    }

    #[test]
    fn reference_white_curve_fits_the_peak_into_the_output() {
        for peak in [1.2_f32, 2.0, 4.0, 10.0, 64.0] {
            let curve = ReferenceWhiteCurve::new(peak);
            let output = |x: f32| x * curve.gain(x);

            // Desktop white is darkened to make room, never brightened, and at most halved.
            assert!(curve.output_white() < 1.0 && curve.output_white() >= 0.5, "peak {peak}");
            assert_close(output(1.0), curve.output_white(), 0.000_001);
            // The peak lands on white, and the curve rises monotonically on the way there.
            assert_close(output(peak), 1.0, 0.000_1);
            let samples: Vec<f32> = (0..=200).map(|i| output(peak * i as f32 / 200.0)).collect();
            assert!(samples.windows(2).all(|pair| pair[1] >= pair[0] - 0.000_001), "peak {peak}");
        }

        // A barely-HDR frame is barely darkened; a 1000-nit-class peak halves desktop white.
        assert!(ReferenceWhiteCurve::new(1.2).output_white() > 0.9);
        assert_close(ReferenceWhiteCurve::new(10.0).output_white(), 0.5, 0.0);
    }

    #[test]
    fn adaptive_mapping_scales_all_components_by_one_gain() {
        let curve = ReferenceWhiteCurve::new(4.0);
        let grey = tone_map_adaptive([0.18, 0.18, 0.18], 1.0, &curve);
        assert_close(grey[0], 0.18 * curve.output_white(), 0.000_001);

        let orange = tone_map_adaptive([8.0, 4.0, 0.0], 2.0, &curve);
        assert_close(orange[0], 1.0, 0.000_1);
        assert_close(orange[1], 0.5, 0.000_1);
        assert_close(orange[2], 0.0, 0.0);
    }

    #[test]
    fn adaptive_peak_needs_real_hdr_content() {
        let sdr_tile = [0.9, 0.0];
        assert_eq!(adaptive_peak(&[sdr_tile; 100], 102_400), None);

        // A few stray bright pixels are not enough to darken a whole screenshot.
        let mut tiles = vec![sdr_tile; 100];
        tiles[0] = [30.0, 3.0];
        assert_eq!(adaptive_peak(&tiles, 1_000_000), None);

        // Real content qualifies; the tile that holds a lone hot pixel does not set the peak.
        let mut tiles = vec![[2.0, 500.0]; 40];
        tiles.push([50.0, 1.0]);
        assert_eq!(adaptive_peak(&tiles, 1_000_000), Some(2.0));
    }

    #[test]
    fn adaptive_peak_leaves_the_brightest_few_tiles_to_clip() {
        let mut tiles = vec![[2.0, 500.0]; 99];
        tiles.push([40.0, 500.0]);
        assert_eq!(adaptive_peak(&tiles, 1_000_000), Some(2.0));
    }

    #[test]
    fn srgb_encoder_matches_reference_points() {
        assert_close(srgb_encode(0.0), 0.0, 0.0);
        assert_close(srgb_encode(0.003_130_8), 0.040_449_936, 0.000_001);
        assert_close(srgb_encode(0.18), 0.461_356_13, 0.000_001);
        assert_close(srgb_encode(1.0), 1.0, 0.0);
        assert_close(srgb_encode(-0.1), 0.0, 0.0);
        assert_close(srgb_encode(f32::INFINITY), 1.0, 0.0);
    }

    #[test]
    fn converts_little_endian_scrgb_rgba16f_to_bgra8() {
        let mut source = Vec::new();
        append_pixel(&mut source, 0x3c00, 0x3c00, 0x3c00, 0x3c00);
        append_pixel(&mut source, 0x0000, 0x3c00, 0x3800, 0x3800);

        let output = rgba16f_to_bgra8(&source, 2, 1, 16, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None)
            .expect("valid packed source should convert");

        let white = quantize_to_u8(srgb_encode(tone_map_highlights([1.0, 1.0, 1.0], 1.0)[0]), 0, 0, DitherMode::None);
        let green = quantize_to_u8(srgb_encode(tone_map_highlights([0.0, 1.0, 0.5], 1.0)[1]), 1, 0, DitherMode::None);
        let blue = quantize_to_u8(srgb_encode(tone_map_highlights([0.0, 1.0, 0.5], 1.0)[2]), 1, 0, DitherMode::None);

        assert_eq!(output, vec![white, white, white, 255, blue, green, 0, 128]);
    }

    #[test]
    fn conversion_respects_source_padding_and_reuses_destination() {
        let mut source = vec![0xa5; 32];
        let mut first = Vec::new();
        append_pixel(&mut first, 0x0000, 0x0000, 0x0000, 0x3c00);
        let mut second = Vec::new();
        append_pixel(&mut second, 0x3c00, 0x0000, 0x0000, 0x3c00);
        source[..8].copy_from_slice(&first);
        source[16..24].copy_from_slice(&second);

        let mut destination = vec![0xcc; 12];
        rgba16f_to_bgra8_into(&source, 1, 2, 16, &mut destination, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None)
            .expect("a padded source and oversized destination should convert");

        assert_eq!(&destination[..4], &[0, 0, 0, 255]);
        assert!(destination[6] > 0);
        assert_eq!(destination[5], 0);
        assert_eq!(destination[4], 0);
        assert_eq!(destination[7], 255);
        assert_eq!(&destination[8..], &[0xcc; 4]);
    }

    #[test]
    fn rec2020_conversion_happens_before_tone_mapping() {
        let mut source = Vec::new();
        append_pixel(&mut source, 0x3c00, 0x0000, 0x0000, 0x3c00);

        let output = rgba16f_to_bgra8(&source, 1, 1, 8, Rgba16fColorSpace::Rec2020, 1.0, DitherMode::None)
            .expect("a valid Rec.2020 source should convert");

        assert_eq!(output[0], 0);
        assert_eq!(output[1], 0);
        assert!(output[2] > 0);
        assert_eq!(output[3], 255);
    }

    #[test]
    fn ordered_dither_uses_the_bayer_position_and_leaves_alpha_undithered() {
        assert_eq!(quantize_to_u8(0.5, 0, 0, DitherMode::None), 128);
        assert_eq!(quantize_to_u8(0.5, 0, 0, DitherMode::Ordered4x4), 127);
        assert_eq!(quantize_to_u8(0.5, 0, 3, DitherMode::Ordered4x4), 128);
    }

    #[test]
    fn rejects_malformed_buffers_before_reading_them() {
        let source = vec![0; 15];
        assert_eq!(
            rgba16f_to_bgra8(&source, 2, 1, 15, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None,),
            Err(Error::SourceRowPitchTooSmall { expected: 16, actual: 15 })
        );

        assert_eq!(
            rgba16f_to_bgra8(&source, 2, 1, 16, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None,),
            Err(Error::SourceBufferTooSmall { expected: 16, actual: 15 })
        );

        let mut destination = [0_u8; 3];
        assert_eq!(
            rgba16f_to_bgra8_into(&[0; 8], 1, 1, 8, &mut destination, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None,),
            Err(Error::DestinationBufferTooSmall { expected: 4, actual: 3 })
        );
    }

    #[test]
    fn accepts_empty_images_and_prevents_dimension_overflow() {
        let empty = rgba16f_to_bgra8(&[], 0, 42, 0, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None)
            .expect("empty images require no source bytes");
        assert!(empty.is_empty());

        assert_eq!(
            rgba16f_to_bgra8(&[], u32::MAX, u32::MAX, 0, Rgba16fColorSpace::ScRgb, 1.0, DitherMode::None,),
            Err(Error::DimensionOverflow)
        );
    }
}
