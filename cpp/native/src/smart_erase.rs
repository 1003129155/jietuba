//! smart_erase for C: reconstructs masked pixels of a screenshot from their surroundings.

use std::ffi::c_void;

use smart_erase::{erase, ChannelOrder, EraseError, Hole, Image, Solver};

use crate::panic::guard;
use crate::ppocr::JbCancelled;

/// Erases the masked pixels of the `hole_width` x `hole_height` rectangle at (hole_x, hole_y).
/// The image is `height` rows of `stride` bytes (`len` in all) with four-byte pixels, BGRA
/// when `bgra`, else RGBA. `mask` holds hole_width * hole_height bytes, nonzero to erase;
/// `out` (hole_width * hole_height * 4 bytes, the image's order) receives the rectangle.
/// `cancelled` (may be null) is asked with `context` between steps.
/// Returns the solver used (0 unchanged, 1 surface, 2 periodic, 3 membrane, 4 patches),
/// -1 when cancelled, -2 for invalid arguments, -3 when nothing around the hole can be sampled.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn jb_smart_erase(
    pixels: *const u8,
    len: usize,
    width: u32,
    height: u32,
    stride: usize,
    bgra: bool,
    hole_x: u32,
    hole_y: u32,
    hole_width: u32,
    hole_height: u32,
    mask: *const u8,
    cancelled: JbCancelled,
    context: *const c_void,
    out: *mut u8,
    out_len: usize,
) -> i32 {
    guard(-2, || {
        let mask_len = (hole_width as usize).saturating_mul(hole_height as usize);
        if pixels.is_null() || mask.is_null() || out.is_null() {
            return -2;
        }
        let image = Image {
            pixels: std::slice::from_raw_parts(pixels, len),
            width: width as usize,
            height: height as usize,
            stride,
            order: if bgra { ChannelOrder::Bgra } else { ChannelOrder::Rgba },
        };
        let hole = Hole {
            x: hole_x as usize,
            y: hole_y as usize,
            width: hole_width as usize,
            height: hole_height as usize,
            mask: std::slice::from_raw_parts(mask, mask_len),
        };
        let stop = || cancelled.is_some_and(|asked| asked(context));
        match erase(&image, &hole, &stop, std::slice::from_raw_parts_mut(out, out_len)) {
            Ok(Solver::Unchanged) => 0,
            Ok(Solver::Surface) => 1,
            Ok(Solver::Periodic) => 2,
            Ok(Solver::Membrane) => 3,
            Ok(Solver::Patches) => 4,
            Err(EraseError::Cancelled) => -1,
            Err(EraseError::InvalidInput) => -2,
            Err(EraseError::NoDonors) => -3,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_flat_background_is_restored_through_the_c_interface() {
        // 40 x 30 BGRA, mid grey with a dark square to erase.
        let (w, h) = (40_usize, 30_usize);
        let mut pixels = vec![0_u8; w * h * 4];
        for y in 0..h {
            for x in 0..w {
                let v = if (15..25).contains(&x) && (10..20).contains(&y) { 20 } else { 128 };
                pixels[(y * w + x) * 4..][..4].copy_from_slice(&[v, v, v, 255]);
            }
        }
        let mask = vec![1_u8; 14 * 14];
        let mut out = vec![0_u8; 14 * 14 * 4];
        let solver = unsafe {
            jb_smart_erase(pixels.as_ptr(), pixels.len(), 40, 30, w * 4, true, 13, 8, 14, 14, mask.as_ptr(), None, std::ptr::null(), out.as_mut_ptr(), out.len())
        };
        assert_eq!(solver, 1);
        assert!(out.chunks(4).all(|p| p == [128, 128, 128, 255]));
    }

    #[test]
    fn invalid_arguments_and_cancellation_have_their_codes() {
        let pixels = vec![200_u8; 16 * 16 * 4];
        let mask = vec![1_u8; 4 * 4];
        let mut out = vec![0_u8; 4 * 4 * 4];
        unsafe extern "C" fn always(_: *const c_void) -> bool {
            true
        }
        let call = |x: u32, out_len: usize, cancelled: JbCancelled| unsafe {
            let mut out = out.clone();
            jb_smart_erase(pixels.as_ptr(), pixels.len(), 16, 16, 64, false, x, 4, 4, 4, mask.as_ptr(), cancelled, std::ptr::null(), out.as_mut_ptr(), out_len)
        };
        assert_eq!(call(14, 64, None), -2);
        assert_eq!(call(4, 63, None), -2);
        assert_eq!(call(4, 64, Some(always)), -1);
        assert_eq!(unsafe { jb_smart_erase(std::ptr::null(), 0, 16, 16, 64, false, 4, 4, 4, 4, mask.as_ptr(), None, std::ptr::null(), out.as_mut_ptr(), 64) }, -2);
    }
}
