use std::path::PathBuf;

use crate::error::{Failure, Result};
use windows::Win32::UI::WindowsAndMessaging::{
    GetSystemMetrics, SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN, SM_XVIRTUALSCREEN,
    SM_YVIRTUALSCREEN,
};

#[derive(Clone, Debug)]
pub struct Options {
    pub output: PathBuf,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate: u32,
    pub audio: bool,
    pub hardware: bool,
    pub cursor: bool,
    pub synthetic: bool,
    pub synthetic_medium: bool,
    pub fail_hardware: bool,
    pub duration: Option<f64>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            output: PathBuf::new(), x: 0, y: 0, width: 0, height: 0, fps: 30,
            bitrate: 4_000_000, audio: true, hardware: true, cursor: true,
            synthetic: false, synthetic_medium: false, fail_hardware: false, duration: None,
        }
    }
}

impl Options {
    pub fn validate(&self) -> Result<()> {
        if self.width < 2 || self.height < 2 || self.width > 8192 || self.height > 8192
            || self.width % 2 != 0 || self.height % 2 != 0
            || u64::from(self.width) * u64::from(self.height) > 33_177_600
        {
            return Err(Failure::new("arguments", "Dimensions must be positive even values, up to 8K"));
        }
        if !(1..=60).contains(&self.fps) || !(500_000..=50_000_000).contains(&self.bitrate) {
            return Err(Failure::new("arguments", "FPS must be 1..60 and bitrate 500000..50000000"));
        }
        if self.duration.is_some_and(|value| !value.is_finite() || value <= 0.0 || value > 86400.0) {
            return Err(Failure::new("arguments", "Invalid test duration"));
        }
        if !self.output.is_absolute()
            || !self.output.extension().is_some_and(|ext| ext.eq_ignore_ascii_case("mp4"))
            || self.output.to_str().is_none()
            || self.output.to_string_lossy().contains('\0')
        {
            return Err(Failure::new("arguments", "Output must be an absolute Unicode MP4 path"));
        }
        if !self.synthetic {
            // 物理像素边界使用宽整数，防止负坐标或极大坐标发生整数溢出。
            let (left, top, width, height) = unsafe { (
                GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
            ) };
            if self.x < left || self.y < top
                || i64::from(self.x) + i64::from(self.width) > i64::from(left) + i64::from(width)
                || i64::from(self.y) + i64::from(self.height) > i64::from(top) + i64::from(height)
            {
                return Err(Failure::new("arguments", "Capture rectangle is outside the virtual desktop"));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn valid() -> Options {
        Options { output: PathBuf::from(r"C:\测试\录制.mp4"), width: 320, height: 240,
            synthetic: true, ..Options::default() }
    }

    #[test]
    fn dimensions_and_rates_reject_invalid_bounds() {
        let mut value = valid();
        assert!(value.validate().is_ok());
        value.width = 321;
        assert!(value.validate().is_err());
        value.width = 320;
        value.fps = 0;
        assert!(value.validate().is_err());
        value.fps = 60;
        value.bitrate = 499_999;
        assert!(value.validate().is_err());
        value.bitrate = 50_000_000;
        assert!(value.validate().is_ok());
    }

    #[test]
    fn only_absolute_mp4_output_is_accepted() {
        let mut value = valid();
        for path in ["relative.mp4", r"C:\clip.gif", r"C:clip.mp4", "C:\\clip\0hidden.mp4"] {
            value.output = PathBuf::from(path);
            assert!(value.validate().is_err());
        }
        value.output = PathBuf::from(r"\\server\share\中文.MP4");
        assert!(value.validate().is_ok());
    }

    #[test]
    fn test_duration_must_be_finite_and_bounded() {
        let mut value = valid();
        for duration in [0.0, -1.0, f64::NAN, f64::INFINITY, 86400.1] {
            value.duration = Some(duration);
            assert!(value.validate().is_err());
        }
        value.duration = Some(1.0);
        assert!(value.validate().is_ok());
    }
}
