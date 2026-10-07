use std::{ffi::c_void, mem::size_of, ptr, slice};

use hdrcapture::hdr_capture::display::Rect;
use hdrcapture::hdr_capture::{Capture as DxgiSession, Error as HdrError, ToneMapping};
use windows::Win32::{
    Graphics::Gdi::{
        BitBlt, CreateCompatibleDC, CreateDIBSection, DeleteDC, DeleteObject, GdiFlush, GetDC,
        ReleaseDC, SelectObject, BITMAPINFO, BITMAPINFOHEADER, BI_RGB, CAPTUREBLT, DIB_RGB_COLORS,
        HBITMAP, HDC, HGDIOBJ, SRCCOPY,
    },
    UI::WindowsAndMessaging::{
        CopyIcon, DestroyIcon, DrawIconEx, GetCursorInfo, GetIconInfo, CURSORINFO, CURSOR_SHOWING,
        DI_NORMAL, HICON, ICONINFO,
    },
};

use crate::{diagnostic::diagnostic, error::{Failure, Result}, options::Options};

/// 一张可复用的顶向下 BGRA DIB，采集过程中不保存帧历史。
///
/// 开启 HDR 的显示器上 GDI 会把超出桌面白的内容逐通道截断，彩色高光偏色；DXGI 路径经
/// hdrcapture 在 GPU 上按固定映射转成 sRGB，只读回录制区域，再写入同一张 DIB 叠加光标。
/// SDR 内容两条路径逐像素一致，同一段录制里混用两者看不出接缝。
pub struct Capture {
    screen: HDC,
    memory: HDC,
    bitmap: HBITMAP,
    previous: HGDIOBJ,
    pixels: *mut c_void,
    width: u32,
    height: u32,
    /// DXGI 会话绑定在创建它的线程上，只能在录制线程里建、用、释放。
    dxgi: Option<DxgiSession>,
    region: Rect,
}

impl Capture {
    pub fn new(options: &Options) -> Result<Self> {
        let mut capture = Self {
            screen: HDC::default(), memory: HDC::default(), bitmap: HBITMAP::default(),
            previous: HGDIOBJ::default(), pixels: ptr::null_mut(),
            width: options.width, height: options.height,
            dxgi: if options.prefer_dxgi && !options.synthetic { open_dxgi() } else { None },
            region: Rect::new(options.x, options.y, options.width, options.height),
        };
        unsafe {
            capture.screen = GetDC(None);
            if capture.screen.0.is_null() {
                return Err(Failure::new("capture", "GetDC failed"));
            }
            capture.memory = CreateCompatibleDC(Some(capture.screen));
            if capture.memory.0.is_null() {
                return Err(Failure::new("capture", "CreateCompatibleDC failed"));
            }
            let info = BITMAPINFO {
                bmiHeader: BITMAPINFOHEADER {
                    biSize: size_of::<BITMAPINFOHEADER>() as u32,
                    biWidth: options.width as i32, biHeight: -(options.height as i32),
                    biPlanes: 1, biBitCount: 32, biCompression: BI_RGB.0,
                    ..Default::default()
                },
                ..Default::default()
            };
            capture.bitmap = CreateDIBSection(Some(capture.screen), &info, DIB_RGB_COLORS,
                &mut capture.pixels, None, 0)
                .map_err(|error| Failure::new("capture", format!("CreateDIBSection: {error}")))?;
            if capture.pixels.is_null() {
                return Err(Failure::new("capture", "GDI allocation failed"));
            }
            let previous = SelectObject(capture.memory, HGDIOBJ(capture.bitmap.0));
            if previous.0.is_null() || previous.0 as isize == -1 {
                return Err(Failure::new("capture", "GDI SelectObject failed"));
            }
            capture.previous = previous;
        }
        Ok(capture)
    }

    pub fn grab(&mut self, options: &Options, frame: u64) -> Result<&[u8]> {
        if self.width != options.width || self.height != options.height {
            return Err(Failure::new("capture", "Capture dimensions changed"));
        }
        let length = self.width as usize * self.height as usize * 4;
        unsafe {
            if options.synthetic {
                let pixels = slice::from_raw_parts_mut(self.pixels.cast::<u8>(), length);
                fill_synthetic(pixels, self.width, self.height, frame, options.synthetic_medium);
            } else {
                if !self.grab_dxgi(length) {
                    BitBlt(self.memory, 0, 0, self.width as i32, self.height as i32,
                        Some(self.screen), options.x, options.y, SRCCOPY | CAPTUREBLT)
                        .map_err(|error| Failure::new("capture", format!("BitBlt: {error}")))?;
                }
                if options.cursor { self.draw_cursor(options); }
                if !GdiFlush().as_bool() {
                    return Err(Failure::new("capture", "GdiFlush failed"));
                }
            }
            Ok(slice::from_raw_parts(self.pixels.cast::<u8>(), length))
        }
    }

    /// 用 DXGI 截一帧写入 DIB；本帧没取到时返回 false，由调用方用 GDI 顶上。
    unsafe fn grab_dxgi(&mut self, length: usize) -> bool {
        let Some(dxgi) = self.dxgi.as_mut() else { return false };
        // 等待预算为 0：桌面没有新的 present 就说明画面没变，缓存帧正是当前画面；
        // 录制节拍由调用方控制，不能在这里等。
        match dxgi.grab_region(self.region, 0, ToneMapping::Static) {
            Ok(frame) if frame.bgra().len() == length => {
                ptr::copy_nonoverlapping(frame.bgra().as_ptr(), self.pixels.cast::<u8>(), length);
                true
            }
            // 新会话在第一次真实桌面 present 之前没有帧；这一帧用 GDI，之后继续试 DXGI。
            Err(HdrError::InitialFrameTimeout { .. }) => false,
            // 显示器配置变了等原因使 DXGI 不可用，本次录制剩下的帧都用 GDI。
            other => {
                let reason = match other {
                    Ok(frame) => format!("frame is {} bytes, expected {length}", frame.bgra().len()),
                    Err(error) => error.to_string(),
                };
                diagnostic("dxgi_failed", &reason);
                self.dxgi = None;
                false
            }
        }
    }

    unsafe fn draw_cursor(&self, options: &Options) {
        let mut current = CURSORINFO { cbSize: size_of::<CURSORINFO>() as u32, ..Default::default() };
        if GetCursorInfo(&mut current).is_err() || current.flags.0 & CURSOR_SHOWING.0 == 0 { return; }
        // 系统光标句柄不归本进程所有，复制后只释放副本及 GetIconInfo 返回的位图。
        let Ok(copy) = CopyIcon(HICON(current.hCursor.0)) else { return; };
        let mut info = ICONINFO::default();
        if GetIconInfo(copy, &mut info).is_ok() {
            let _ = DrawIconEx(self.memory,
                current.ptScreenPos.x - options.x - info.xHotspot as i32,
                current.ptScreenPos.y - options.y - info.yHotspot as i32,
                copy, 0, 0, 0, None, DI_NORMAL);
            if !info.hbmMask.0.is_null() { let _ = DeleteObject(HGDIOBJ(info.hbmMask.0)); }
            if !info.hbmColor.0.is_null() { let _ = DeleteObject(HGDIOBJ(info.hbmColor.0)); }
        }
        let _ = DestroyIcon(copy);
    }
}

fn open_dxgi() -> Option<DxgiSession> {
    DxgiSession::with_timeout(0)
        .map_err(|error| diagnostic("dxgi_unavailable", &error.to_string()))
        .ok()
}

impl Drop for Capture {
    fn drop(&mut self) {
        unsafe {
            // 必须先恢复原 GDI 对象，再删除仍可能被 DC 引用的 DIB。
            if !self.previous.0.is_null() { SelectObject(self.memory, self.previous); }
            if !self.bitmap.0.is_null() { let _ = DeleteObject(HGDIOBJ(self.bitmap.0)); }
            if !self.memory.0.is_null() { let _ = DeleteDC(self.memory); }
            if !self.screen.0.is_null() { ReleaseDC(None, self.screen); }
        }
    }
}

fn fill_synthetic(destination: &mut [u8], width: u32, height: u32, frame: u64, medium: bool) {
    let box_width = width / 2;
    let box_height = height / 2;
    let box_x = (frame.wrapping_mul(3) % u64::from(width - box_width + 1)) as u32;
    let box_y = (frame.wrapping_mul(2) % u64::from(height - box_height + 1)) as u32;
    for y in 0..height {
        for x in 0..width {
            let index = (y as usize * width as usize + x as usize) * 4;
            destination[index] = if (u64::from(x / 64 + y / 64) + frame / 4) & 1 != 0 { 200 } else { 40 };
            destination[index + 1] = u64::from(y).wrapping_add(frame.wrapping_mul(2)) as u8;
            destination[index + 2] = u64::from(x).wrapping_add(frame.wrapping_mul(3)) as u8;
            destination[index + 3] = 255;
            if medium {
                // 静态面板叠加移动细节，像素与原测试后端保持一致。
                let inside = x >= box_x && x < box_x + box_width && y >= box_y && y < box_y + box_height;
                if inside {
                    destination[index] = u64::from((x - box_x) / 4).wrapping_add(frame) as u8;
                    destination[index + 1] = u64::from((y - box_y) / 4).wrapping_add(frame) as u8;
                    destination[index + 2] = (40 + (((x - box_x) / 32 + (y - box_y) / 32) & 1) * 100) as u8;
                } else {
                    destination[index] = (220 + ((x / 80 + y / 80) & 1) * 12) as u8;
                    destination[index + 1] = 232;
                    destination[index + 2] = 230;
                }
            }
        }
    }
}

/// BT.601、limited range；每个 2×2 像素块平均色度后交错写入 NV12。
pub fn bgra_to_nv12(source: &[u8], destination: &mut [u8], width: u32, height: u32) -> Result<()> {
    let pixels = (width as usize).checked_mul(height as usize);
    let Some(pixels) = pixels.filter(|pixels| pixels.checked_mul(4).is_some()) else {
        return Err(Failure::new("encoding", "Video dimensions exceed addressable memory"));
    };
    if width == 0 || height == 0 || width & 1 != 0 || height & 1 != 0
        || source.len() != pixels * 4 || destination.len() != pixels * 3 / 2 {
        return Err(Failure::new("encoding", "Invalid BGRA or NV12 frame dimensions"));
    }
    let width = width as usize;
    let height = height as usize;
    let (luma, chroma) = destination.split_at_mut(pixels);
    for y in (0..height).step_by(2) {
        for x in (0..width).step_by(2) {
            let mut sum_u = 0;
            let mut sum_v = 0;
            for dy in 0..2 {
                for dx in 0..2 {
                    let at = (y + dy) * width + x + dx;
                    let index = at * 4;
                    let b = i32::from(source[index]);
                    let g = i32::from(source[index + 1]);
                    let r = i32::from(source[index + 2]);
                    luma[at] = (((66 * r + 129 * g + 25 * b + 128) >> 8) + 16).clamp(0, 255) as u8;
                    sum_u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    sum_v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                }
            }
            chroma[y / 2 * width + x] = ((sum_u + 2) / 4).clamp(0, 255) as u8;
            chroma[y / 2 * width + x + 1] = ((sum_v + 2) / 4).clamp(0, 255) as u8;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn desktop_options(prefer_dxgi: bool) -> Options {
        Options { width: 64, height: 48, prefer_dxgi, ..Options::default() }
    }

    #[test]
    fn gdi_only_capture_returns_the_requested_size() {
        let options = desktop_options(false);
        let mut capture = Capture::new(&options).unwrap();
        assert!(capture.dxgi.is_none());
        assert_eq!(capture.grab(&options, 0).unwrap().len(), 64 * 48 * 4);
    }

    #[test]
    fn dxgi_capture_returns_the_requested_size_whichever_path_serves_the_frame() {
        let options = desktop_options(true);
        let mut capture = Capture::new(&options).unwrap();
        for frame in 0..3 {
            assert_eq!(capture.grab(&options, frame).unwrap().len(), 64 * 48 * 4);
        }
    }

    #[test]
    fn synthetic_capture_never_opens_a_dxgi_session() {
        let options = Options { synthetic: true, ..desktop_options(true) };
        assert!(Capture::new(&options).unwrap().dxgi.is_none());
    }

    #[test]
    fn nv12_uses_limited_range_and_averaged_chroma() {
        let mut output = [0; 6];
        for (pixel, expected) in [
            ([0, 0, 0, 255], [16, 16, 16, 16, 128, 128]),
            ([255, 255, 255, 255], [235, 235, 235, 235, 128, 128]),
            ([0, 0, 255, 255], [82, 82, 82, 82, 90, 240]),
        ] {
            bgra_to_nv12(&pixel.repeat(4), &mut output, 2, 2).unwrap();
            assert_eq!(output, expected);
        }
        let pixels = [0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255];
        bgra_to_nv12(&pixels, &mut output, 2, 2).unwrap();
        assert_eq!(output, [82, 144, 41, 235, 128, 128]);
    }

    #[test]
    fn conversion_rejects_odd_dimensions_and_invalid_buffers() {
        assert!(bgra_to_nv12(&[0; 24], &mut [0; 9], 3, 2).is_err());
        assert!(bgra_to_nv12(&[0; 15], &mut [0; 6], 2, 2).is_err());
        assert!(bgra_to_nv12(&[0; 16], &mut [0; 5], 2, 2).is_err());
        assert!(bgra_to_nv12(&[], &mut [], u32::MAX - 1, u32::MAX - 1).is_err());
    }

    #[test]
    fn synthetic_modes_match_reference_pixels() {
        let mut pixels = [0; 64];
        fill_synthetic(&mut pixels, 4, 4, 1, false);
        assert_eq!(&pixels[..4], &[40, 2, 3, 255]);
        assert_eq!(&pixels[60..], &[40, 5, 6, 255]);
        fill_synthetic(&mut pixels, 4, 4, 1, true);
        assert_eq!(&pixels[..4], &[220, 232, 230, 255]);
        assert_eq!(&pixels[32..36], &[1, 1, 40, 255]);
    }
}
