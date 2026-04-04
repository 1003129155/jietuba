//! Win32 GDI 屏幕截取 — BitBlt 方案
//!
//! 优势：
//!   - 纯 Rust，零 Python 依赖（替代 mss）
//!   - 无 GIL 争用，截屏 + JPEG 压缩全在 Rust 线程
//!   - 内存零拷贝：BitBlt → DIB → 直接传给 JPEG 编码器

// 链接 Win32 GDI 库
#[link(name = "user32")]
extern "system" {}
#[link(name = "gdi32")]
extern "system" {}

// ── Win32 FFI 类型 ──

#[allow(non_camel_case_types)]
type HWND = isize;
#[allow(non_camel_case_types)]
type HDC = isize;
#[allow(non_camel_case_types)]
type HBITMAP = isize;
#[allow(non_camel_case_types)]
type HGDIOBJ = isize;
#[allow(non_camel_case_types)]
type BOOL = i32;
#[allow(non_camel_case_types)]
type LONG = i32;
#[allow(non_camel_case_types)]
type WORD = u16;
#[allow(non_camel_case_types)]
type DWORD = u32;

const SRCCOPY: DWORD = 0x00CC0020;
const DIB_RGB_COLORS: u32 = 0;
const BI_RGB: DWORD = 0;

#[repr(C)]
#[allow(non_snake_case)]
struct BITMAPINFOHEADER {
    biSize: DWORD,
    biWidth: LONG,
    biHeight: LONG,
    biPlanes: WORD,
    biBitCount: WORD,
    biCompression: DWORD,
    biSizeImage: DWORD,
    biXPelsPerMeter: LONG,
    biYPelsPerMeter: LONG,
    biClrUsed: DWORD,
    biClrImportant: DWORD,
}

#[repr(C)]
#[allow(non_snake_case)]
struct BITMAPINFO {
    bmiHeader: BITMAPINFOHEADER,
    bmiColors: [u32; 1], // 不使用调色板
}

extern "system" {
    fn GetDC(hWnd: HWND) -> HDC;
    fn ReleaseDC(hWnd: HWND, hDC: HDC) -> i32;
    fn CreateCompatibleDC(hdc: HDC) -> HDC;
    fn DeleteDC(hdc: HDC) -> BOOL;
    fn CreateCompatibleBitmap(hdc: HDC, cx: i32, cy: i32) -> HBITMAP;
    fn SelectObject(hdc: HDC, h: HGDIOBJ) -> HGDIOBJ;
    fn DeleteObject(h: HGDIOBJ) -> BOOL;
    fn BitBlt(
        hdc: HDC, x: i32, y: i32, cx: i32, cy: i32,
        hdcSrc: HDC, x1: i32, y1: i32, rop: DWORD,
    ) -> BOOL;
    fn GetDIBits(
        hdc: HDC, hbm: HBITMAP, start: u32, cLines: u32,
        lpvBits: *mut u8, lpbmi: *mut BITMAPINFO, usage: u32,
    ) -> i32;
}

// ── 截屏上下文（可复用，避免每帧重新创建 GDI 对象）──

/// 屏幕截取器 — 持有 GDI 资源，可重复截屏同一区域
pub(crate) struct ScreenCapture {
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    hdc_screen: HDC,
    hdc_mem: HDC,
    hbitmap: HBITMAP,
    hbitmap_old: HGDIOBJ,
    buffer: Vec<u8>,     // BGRA 像素缓冲区（复用）
}

// GDI 句柄可跨线程使用（在同一线程创建和操作）
unsafe impl Send for ScreenCapture {}

impl ScreenCapture {
    /// 创建截屏上下文
    ///
    /// * `left`, `top` — 屏幕坐标 (虚拟桌面)
    /// * `width`, `height` — 截取区域大小
    pub fn new(left: i32, top: i32, width: i32, height: i32) -> Result<Self, String> {
        if width <= 0 || height <= 0 {
            return Err(format!("invalid capture size: {width}x{height}"));
        }

        unsafe {
            let hdc_screen = GetDC(0); // 整个虚拟桌面
            if hdc_screen == 0 {
                return Err("GetDC(NULL) failed".into());
            }

            let hdc_mem = CreateCompatibleDC(hdc_screen);
            if hdc_mem == 0 {
                ReleaseDC(0, hdc_screen);
                return Err("CreateCompatibleDC failed".into());
            }

            let hbitmap = CreateCompatibleBitmap(hdc_screen, width, height);
            if hbitmap == 0 {
                DeleteDC(hdc_mem);
                ReleaseDC(0, hdc_screen);
                return Err("CreateCompatibleBitmap failed".into());
            }

            let hbitmap_old = SelectObject(hdc_mem, hbitmap);

            let buf_size = (width * height * 4) as usize; // BGRA
            let buffer = vec![0u8; buf_size];

            Ok(Self {
                left,
                top,
                width,
                height,
                hdc_screen,
                hdc_mem,
                hbitmap,
                hbitmap_old,
                buffer,
            })
        }
    }

    /// 截取一帧，返回 BGRA 像素切片（底部在前 → 需翻转行）
    ///
    /// 返回的切片指向内部缓冲区，生命周期与 `&mut self` 相同。
    pub fn grab(&mut self) -> Result<&[u8], String> {
        unsafe {
            // BitBlt: 屏幕 → 内存 DC
            let ok = BitBlt(
                self.hdc_mem, 0, 0, self.width, self.height,
                self.hdc_screen, self.left, self.top,
                SRCCOPY,
            );
            if ok == 0 {
                return Err("BitBlt failed".into());
            }

            // 从 HBITMAP 读取像素到缓冲区
            let mut bmi = BITMAPINFO {
                bmiHeader: BITMAPINFOHEADER {
                    biSize: std::mem::size_of::<BITMAPINFOHEADER>() as DWORD,
                    biWidth: self.width,
                    biHeight: -self.height, // 负值 = top-down (第一行是顶部)
                    biPlanes: 1,
                    biBitCount: 32,
                    biCompression: BI_RGB,
                    biSizeImage: 0,
                    biXPelsPerMeter: 0,
                    biYPelsPerMeter: 0,
                    biClrUsed: 0,
                    biClrImportant: 0,
                },
                bmiColors: [0],
            };

            let lines = GetDIBits(
                self.hdc_mem,
                self.hbitmap,
                0,
                self.height as u32,
                self.buffer.as_mut_ptr(),
                &mut bmi,
                DIB_RGB_COLORS,
            );
            if lines == 0 {
                return Err("GetDIBits failed".into());
            }

            Ok(&self.buffer)
        }
    }

    /// 截取区域宽度
    pub fn width(&self) -> u32 {
        self.width as u32
    }

    /// 截取区域高度
    pub fn height(&self) -> u32 {
        self.height as u32
    }
}

impl Drop for ScreenCapture {
    fn drop(&mut self) {
        unsafe {
            SelectObject(self.hdc_mem, self.hbitmap_old);
            DeleteObject(self.hbitmap);
            DeleteDC(self.hdc_mem);
            ReleaseDC(0, self.hdc_screen);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn capture_small_region() {
        let mut cap = ScreenCapture::new(0, 0, 64, 48).unwrap();
        let bgra = cap.grab().unwrap();
        assert_eq!(bgra.len(), 64 * 48 * 4);
        // 像素不全是 0（屏幕上总有点东西）
        assert!(bgra.iter().any(|&b| b != 0));
    }
}
