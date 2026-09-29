//! 录制帧来源：优先 DXGI Desktop Duplication，拿不到帧时用 GDI BitBlt 顶上。
//!
//! 开启 HDR 的显示器上 BitBlt 会把超出桌面白的内容逐通道截断，彩色高光偏色；DXGI 路径经
//! hdrcapture 在 GPU 上按固定映射转成 sRGB，只读回录制区域。SDR 内容两条路径逐像素一致，
//! 所以同一段录制里混用两者看不出接缝。

use hdrcapture::hdr_capture::display::Rect;
use hdrcapture::hdr_capture::{Capture, Error as HdrError, ToneMapping};

use crate::capture::ScreenCapture;

/// DXGI 录制区域采集。录制线程自建会话：同一进程每块屏只能有一个 duplication，
/// 主线程的截图会话须在开录前释放，否则这里建会话失败、整段回落 GDI。
struct DxgiRegion {
    capture: Capture,
    region: Rect,
    pixels: Vec<u8>,
}

impl DxgiRegion {
    fn new(region: Rect) -> Result<Self, HdrError> {
        // 等待预算为 0：桌面没有新的 present 就说明画面没变，缓存帧正是当前画面；
        // 录制节拍由调用方控制，不能在这里等。
        Ok(Self { capture: Capture::with_timeout(0)?, region, pixels: Vec::new() })
    }

    fn refresh(&mut self) -> Result<(), HdrError> {
        // 固定映射只看单个像素，相邻帧的同一内容结果一致。
        let frame = self.capture.grab_region(self.region, 0, ToneMapping::Static)?;
        self.pixels = frame.bgra;
        Ok(())
    }
}

enum Attempt {
    Dxgi,
    /// 新会话在第一次真实桌面 present 之前没有帧；这一帧用 GDI，之后继续试 DXGI。
    NotYet,
    /// DXGI 已不可用（会话建不起来、显示器配置变了等），本次录制剩下的帧都用 GDI。
    GiveUp,
}

/// 最近一帧由哪条路径截取，供调用方记日志排查。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Backend {
    Dxgi,
    Gdi,
}

pub(crate) struct FrameSource {
    dxgi: Option<DxgiRegion>,
    gdi: ScreenCapture,
    last_backend: Option<Backend>,
}

impl FrameSource {
    /// `prefer_dxgi` 为 false 时只用 GDI，对应截图引擎设为 mss。
    pub fn new(left: i32, top: i32, width: i32, height: i32, prefer_dxgi: bool) -> Result<Self, String> {
        let gdi = ScreenCapture::new(left, top, width, height)?;
        let dxgi = if prefer_dxgi {
            DxgiRegion::new(Rect::new(left, top, gdi.width(), gdi.height()))
                .map_err(|e| eprintln!("[gifrecorder] DXGI 会话建立失败，改用 GDI: {e}"))
                .ok()
        } else {
            None
        };
        Ok(Self { dxgi, gdi, last_backend: None })
    }

    pub fn last_backend(&self) -> Option<Backend> {
        self.last_backend
    }

    /// 截取一帧，返回紧凑的 BGRA 像素。
    pub fn grab(&mut self) -> Result<&[u8], String> {
        let attempt = match self.dxgi.as_mut().map(DxgiRegion::refresh) {
            None => Attempt::GiveUp,
            Some(Ok(())) => Attempt::Dxgi,
            Some(Err(HdrError::InitialFrameTimeout { .. })) => Attempt::NotYet,
            Some(Err(e)) => {
                eprintln!("[gifrecorder] DXGI 截取失败，本次录制改用 GDI: {e}");
                Attempt::GiveUp
            }
        };
        if matches!(attempt, Attempt::GiveUp) {
            self.dxgi = None;
        }
        match (attempt, &self.dxgi) {
            (Attempt::Dxgi, Some(dxgi)) => {
                self.last_backend = Some(Backend::Dxgi);
                Ok(&dxgi.pixels)
            }
            _ => {
                self.last_backend = Some(Backend::Gdi);
                self.gdi.grab()
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gdi_only_source_captures_the_requested_size() {
        let mut source = FrameSource::new(0, 0, 64, 48, false).unwrap();
        assert!(source.dxgi.is_none());
        assert_eq!(source.grab().unwrap().len(), 64 * 48 * 4);
        assert_eq!(source.last_backend(), Some(Backend::Gdi));
    }

    #[test]
    fn dxgi_source_matches_gdi_size_whichever_path_serves_the_frame() {
        let mut source = FrameSource::new(0, 0, 64, 48, true).unwrap();
        for _ in 0..3 {
            assert_eq!(source.grab().unwrap().len(), 64 * 48 * 4);
        }
    }
}
