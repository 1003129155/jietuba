//! Windows 剪贴板读写。
//!
//! 一律用本模块建的隐藏窗口打开剪贴板，一次打开里读完或写完。两个程序都用空窗口
//! 句柄打开剪贴板时互不排斥：后打开的一方一关剪贴板，先打开的一方接下来的读写就
//! 全部失败（写剪贴板的程序只清空了、内容没写进去）。用自己的窗口打开则正常互斥。

use std::ffi::c_void;
use std::io::Cursor;
use std::time::{Duration, Instant};

use image::RgbaImage;

pub const CF_TEXT: u32 = 1;
pub const CF_DIB: u32 = 8;
pub const CF_UNICODETEXT: u32 = 13;
pub const CF_HDROP: u32 = 15;
pub const CF_LOCALE: u32 = 16;
pub const CF_DIBV5: u32 = 17;
/// 0xC000 起是按名字注册的格式，编号每次开机重新分配，只有名字可靠
pub const FIRST_REGISTERED_FORMAT: u32 = 0xC000;
/// 单个格式超过这个大小就不读
const MAX_FORMAT_BYTES: usize = 64 * 1024 * 1024;

#[link(name = "user32")]
extern "system" {
    fn OpenClipboard(hwnd: *mut c_void) -> i32;
    fn CloseClipboard() -> i32;
    fn EmptyClipboard() -> i32;
    fn GetClipboardData(format: u32) -> *mut c_void;
    fn SetClipboardData(format: u32, hmem: *mut c_void) -> *mut c_void;
    fn IsClipboardFormatAvailable(format: u32) -> i32;
    fn EnumClipboardFormats(format: u32) -> u32;
    fn GetClipboardFormatNameW(format: u32, name: *mut u16, max: i32) -> i32;
    fn RegisterClipboardFormatW(name: *const u16) -> u32;
    fn GetClipboardSequenceNumber() -> u32;
    fn CountClipboardFormats() -> i32;
    fn CreateWindowExW(
        ex_style: u32, class_name: *const u16, window_name: *const u16, style: u32,
        x: i32, y: i32, width: i32, height: i32,
        parent: *mut c_void, menu: *mut c_void, instance: *mut c_void, param: *mut c_void,
    ) -> *mut c_void;
    fn DestroyWindow(hwnd: *mut c_void) -> i32;
}

#[link(name = "kernel32")]
extern "system" {
    fn GlobalAlloc(flags: u32, bytes: usize) -> *mut c_void;
    fn GlobalFree(hmem: *mut c_void) -> *mut c_void;
    fn GlobalLock(hmem: *mut c_void) -> *mut c_void;
    fn GlobalUnlock(hmem: *mut c_void) -> i32;
    fn GlobalSize(hmem: *mut c_void) -> usize;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn MultiByteToWideChar(code_page: u32, flags: u32, src: *const u8, src_len: i32, dst: *mut u16, dst_len: i32) -> i32;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn register_format(name: &str) -> u32 {
    unsafe { RegisterClipboardFormatW(wide(name).as_ptr()) }
}

pub fn sequence_number() -> u32 {
    unsafe { GetClipboardSequenceNumber() }
}

pub fn format_count() -> i32 {
    unsafe { CountClipboardFormats() }
}

pub fn format_name(format: u32) -> String {
    let standard = match format {
        1 => Some("CF_TEXT"),
        7 => Some("CF_OEMTEXT"),
        8 => Some("CF_DIB"),
        13 => Some("CF_UNICODETEXT"),
        15 => Some("CF_HDROP"),
        16 => Some("CF_LOCALE"),
        17 => Some("CF_DIBV5"),
        _ => None,
    };
    if let Some(name) = standard {
        return name.to_string();
    }
    let mut buf = [0u16; 256];
    let len = unsafe { GetClipboardFormatNameW(format, buf.as_mut_ptr(), buf.len() as i32) };
    if len > 0 {
        String::from_utf16_lossy(&buf[..len as usize])
    } else {
        format!("UNKNOWN_{}", format)
    }
}

/// 只用来打开剪贴板的隐藏窗口；在哪个线程建就在哪个线程用
pub struct ClipboardWindow(*mut c_void);

impl ClipboardWindow {
    pub fn new() -> Option<Self> {
        const HWND_MESSAGE: isize = -3;
        let class = wide("STATIC");
        let hwnd = unsafe {
            CreateWindowExW(
                0, class.as_ptr(), std::ptr::null(), 0, 0, 0, 0, 0,
                HWND_MESSAGE as *mut c_void, std::ptr::null_mut(),
                GetModuleHandleW(std::ptr::null()), std::ptr::null_mut(),
            )
        };
        (!hwnd.is_null()).then_some(Self(hwnd))
    }

    /// 剪贴板正被别的程序占着时，每 10ms 重试一次，最多等 timeout
    pub fn open(&self, timeout: Duration) -> Option<OpenClipboardGuard<'_>> {
        let started = Instant::now();
        loop {
            if unsafe { OpenClipboard(self.0) } != 0 {
                return Some(OpenClipboardGuard { _window: self });
            }
            if started.elapsed() >= timeout {
                return None;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
}

impl Drop for ClipboardWindow {
    fn drop(&mut self) {
        unsafe { DestroyWindow(self.0) };
    }
}

/// 打开着的剪贴板，离开作用域时关闭
pub struct OpenClipboardGuard<'a> {
    _window: &'a ClipboardWindow,
}

impl OpenClipboardGuard<'_> {
    pub fn is_available(&self, format: u32) -> bool {
        unsafe { IsClipboardFormatAvailable(format) != 0 }
    }

    pub fn formats(&self) -> Vec<(u32, String)> {
        let mut found = Vec::new();
        let mut format = 0;
        loop {
            format = unsafe { EnumClipboardFormats(format) };
            if format == 0 {
                return found;
            }
            found.push((format, format_name(format)));
        }
    }

    /// 读取一个按内存块存放的格式；没有、读不出或超过上限时返回 None
    pub fn read(&self, format: u32) -> Option<Vec<u8>> {
        unsafe {
            let hmem = GetClipboardData(format);
            if hmem.is_null() {
                return None;
            }
            let ptr = GlobalLock(hmem);
            if ptr.is_null() {
                return None;
            }
            let size = GlobalSize(hmem);
            let data = (size > 0 && size <= MAX_FORMAT_BYTES)
                .then(|| std::slice::from_raw_parts(ptr as *const u8, size).to_vec());
            GlobalUnlock(hmem);
            data
        }
    }

    pub fn clear(&self) -> bool {
        unsafe { EmptyClipboard() != 0 }
    }

    pub fn put(&self, format: u32, data: &[u8]) -> bool {
        const GMEM_MOVEABLE: u32 = 0x0002;
        if data.is_empty() {
            return false;
        }
        unsafe {
            let hmem = GlobalAlloc(GMEM_MOVEABLE, data.len());
            if hmem.is_null() {
                return false;
            }
            let ptr = GlobalLock(hmem);
            if ptr.is_null() {
                GlobalFree(hmem);
                return false;
            }
            std::ptr::copy_nonoverlapping(data.as_ptr(), ptr as *mut u8, data.len());
            GlobalUnlock(hmem);
            // 成功后内存归剪贴板所有，失败时还在自己手里
            if SetClipboardData(format, hmem).is_null() {
                GlobalFree(hmem);
                return false;
            }
        }
        true
    }
}

impl Drop for OpenClipboardGuard<'_> {
    fn drop(&mut self) {
        unsafe { CloseClipboard() };
    }
}

/// 清空剪贴板并写入这些格式；剪贴板打不开时返回 false。
/// 在界面线程上调用，等别的程序让出剪贴板不宜太久
pub fn write(formats: &[(u32, Vec<u8>)]) -> bool {
    let Some(window) = ClipboardWindow::new() else {
        return false;
    };
    let Some(clipboard) = window.open(Duration::from_millis(300)) else {
        return false;
    };
    clipboard.clear();
    for (format, data) in formats {
        clipboard.put(*format, data);
    }
    true
}

// ── 格式编解码 ────────────────────────────────────────────

/// CF_UNICODETEXT：UTF-16LE，到第一个 0 为止
pub fn decode_unicode(data: &[u8]) -> String {
    let units: Vec<u16> = data.chunks_exact(2)
        .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
        .take_while(|unit| *unit != 0)
        .collect();
    String::from_utf16_lossy(&units)
}

pub fn encode_unicode(text: &str) -> Vec<u8> {
    text.encode_utf16().chain(std::iter::once(0)).flat_map(u16::to_le_bytes).collect()
}

/// CF_HDROP：DROPFILES 头（20 字节）后接以 0 分隔、双 0 结尾的路径列表
pub fn parse_hdrop(data: &[u8]) -> Vec<String> {
    let field = |offset: usize| data.get(offset..offset + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
    let (Some(files_offset), Some(is_wide)) = (field(0), field(16)) else {
        return Vec::new();
    };
    let Some(list) = data.get(files_offset as usize..) else {
        return Vec::new();
    };
    if is_wide != 0 {
        let units: Vec<u16> = list.chunks_exact(2).map(|p| u16::from_le_bytes([p[0], p[1]])).collect();
        units.split(|unit| *unit == 0)
            .take_while(|path| !path.is_empty())
            .map(String::from_utf16_lossy)
            .collect()
    } else {
        list.split(|byte| *byte == 0)
            .take_while(|path| !path.is_empty())
            .map(ansi_to_string)
            .collect()
    }
}

fn ansi_to_string(bytes: &[u8]) -> String {
    const CP_ACP: u32 = 0;
    unsafe {
        let len = MultiByteToWideChar(CP_ACP, 0, bytes.as_ptr(), bytes.len() as i32, std::ptr::null_mut(), 0);
        if len <= 0 {
            return String::from_utf8_lossy(bytes).into_owned();
        }
        let mut buf = vec![0u16; len as usize];
        MultiByteToWideChar(CP_ACP, 0, bytes.as_ptr(), bytes.len() as i32, buf.as_mut_ptr(), len);
        String::from_utf16_lossy(&buf)
    }
}

pub fn build_hdrop(files: &[String]) -> Vec<u8> {
    const HEADER_LEN: u32 = 20;
    let mut data = Vec::new();
    data.extend_from_slice(&HEADER_LEN.to_le_bytes()); // pFiles
    data.extend_from_slice(&[0u8; 12]);                // pt、fNC
    data.extend_from_slice(&1u32.to_le_bytes());       // fWide
    for file in files {
        data.extend(file.encode_utf16().chain(std::iter::once(0)).flat_map(u16::to_le_bytes));
    }
    data.extend_from_slice(&[0, 0]);
    data
}

/// 按 PNG、CF_DIBV5、CF_DIB 的顺序取第一个能解码的
pub fn decode_image(png: Option<&[u8]>, dibv5: Option<&[u8]>, dib: Option<&[u8]>) -> Option<RgbaImage> {
    use image::codecs::bmp::BmpDecoder;
    use image::DynamicImage;

    if let Some(png) = png {
        if let Ok(img) = image::load_from_memory_with_format(png, image::ImageFormat::Png) {
            return Some(img.to_rgba8());
        }
    }
    [dibv5, dib].into_iter().flatten().find_map(|dib| {
        let dib = with_masks_after_header(dib);
        let decoder = BmpDecoder::new_without_file_header(Cursor::new(dib.as_ref())).ok()?;
        DynamicImage::from_decoder(decoder).ok().map(|img| img.to_rgba8())
    })
}

/// V4/V5 头 + BI_BITFIELDS 时，掩码在头里；有的程序会在头后再放一份掩码，有的不放。
/// 解码器认定头后有这 12 字节，没有的就补上，否则像素整体错位。
fn with_masks_after_header(dib: &[u8]) -> std::borrow::Cow<'_, [u8]> {
    use std::borrow::Cow;

    const BI_BITFIELDS: u32 = 3;
    let u32_at = |offset: usize| dib.get(offset..offset + 4)
        .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
    let (Some(header_len), Some(compression)) = (u32_at(0), u32_at(16)) else {
        return Cow::Borrowed(dib);
    };
    let header_len = header_len as usize;
    if !matches!(header_len, 108 | 124) || compression != BI_BITFIELDS || dib.len() < header_len {
        return Cow::Borrowed(dib);
    }
    let masks = &dib[40..52];
    if dib.get(header_len..header_len + 12) == Some(masks) {
        return Cow::Borrowed(dib);
    }
    let mut fixed = Vec::with_capacity(dib.len() + 12);
    fixed.extend_from_slice(&dib[..header_len]);
    fixed.extend_from_slice(masks);
    fixed.extend_from_slice(&dib[header_len..]);
    Cow::Owned(fixed)
}

/// RGBA → BITMAPV5HEADER + 32 位 BGRA（自下而上、非预乘 alpha）
pub fn rgba_to_dibv5(img: &RgbaImage) -> Vec<u8> {
    let (width, height) = img.dimensions();
    let pixel_bytes = width * height * 4;
    let mut data = Vec::with_capacity(124 + pixel_bytes as usize);
    data.extend_from_slice(&124u32.to_le_bytes());          // bV5Size
    data.extend_from_slice(&(width as i32).to_le_bytes());  // bV5Width
    data.extend_from_slice(&(height as i32).to_le_bytes()); // bV5Height：正数为自下而上
    data.extend_from_slice(&1u16.to_le_bytes());            // bV5Planes
    data.extend_from_slice(&32u16.to_le_bytes());           // bV5BitCount
    data.extend_from_slice(&3u32.to_le_bytes());            // bV5Compression = BI_BITFIELDS
    data.extend_from_slice(&pixel_bytes.to_le_bytes());     // bV5SizeImage
    data.extend_from_slice(&[0u8; 16]);                     // 分辨率、调色板
    for mask in [0x00FF_0000u32, 0x0000_FF00, 0x0000_00FF, 0xFF00_0000] {
        data.extend_from_slice(&mask.to_le_bytes());
    }
    data.extend_from_slice(&0x7352_4742u32.to_le_bytes());  // bV5CSType = LCS_sRGB
    data.extend_from_slice(&[0u8; 48]);                     // Endpoints、Gamma
    data.extend_from_slice(&4u32.to_le_bytes());            // bV5Intent = LCS_GM_IMAGES
    data.extend_from_slice(&[0u8; 12]);                     // ProfileData、ProfileSize、Reserved
    for row in img.rows().rev() {
        for pixel in row {
            let [r, g, b, a] = pixel.0;
            data.extend_from_slice(&[b, g, r, a]);
        }
    }
    data
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unicode_text_round_trips_and_stops_at_the_terminator() {
        let mut data = encode_unicode("截图吧 123");
        data.extend_from_slice(&[0x41, 0x00, 0x42, 0x00]); // 分配粒度带出来的尾巴
        assert_eq!(decode_unicode(&data), "截图吧 123");
    }

    #[test]
    fn file_list_round_trips() {
        let files = vec![r"C:\图片\截图 1.png".to_string(), r"D:\a.txt".to_string()];
        assert_eq!(parse_hdrop(&build_hdrop(&files)), files);
    }

    #[test]
    fn truncated_file_list_is_empty_not_a_panic() {
        assert!(parse_hdrop(&[1, 2, 3]).is_empty());
        let mut data = build_hdrop(&["C:\\x".to_string()]);
        data[0] = 0xFF; // pFiles 指到数据外面
        assert!(parse_hdrop(&data).is_empty());
    }

    #[test]
    fn dibv5_round_trips_with_alpha() {
        let mut img = RgbaImage::new(3, 2);
        img.put_pixel(0, 0, image::Rgba([255, 0, 0, 255]));
        img.put_pixel(2, 1, image::Rgba([10, 20, 30, 128]));
        let dib = rgba_to_dibv5(&img);
        assert_eq!(&dib[..4], &124u32.to_le_bytes());
        assert_eq!(decode_image(None, Some(&dib), None), Some(img));
    }

    #[test]
    fn dibv5_with_masks_repeated_after_the_header_decodes_too() {
        let img = RgbaImage::from_pixel(2, 2, image::Rgba([40, 50, 60, 200]));
        let plain = rgba_to_dibv5(&img);
        let mut with_masks = plain[..124].to_vec();
        with_masks.extend_from_slice(&plain[40..52]);
        with_masks.extend_from_slice(&plain[124..]);
        assert_eq!(decode_image(None, Some(&with_masks), None), Some(img));
    }

    #[test]
    fn png_is_preferred_over_bitmaps() {
        let mut png = Vec::new();
        let img = RgbaImage::from_pixel(1, 1, image::Rgba([1, 2, 3, 4]));
        img.write_to(&mut Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        let other = rgba_to_dibv5(&RgbaImage::from_pixel(1, 1, image::Rgba([9, 9, 9, 9])));
        assert_eq!(decode_image(Some(&png), Some(&other), None), Some(img));
    }
}

