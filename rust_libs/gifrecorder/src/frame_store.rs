//! 帧存储 — 全部帧数据的唯一所有者
//!
//! 录制阶段：接收 BGRA bytes → JPEG 压缩 → 存入 Vec
//! 回放阶段：按索引 JPEG 解码 → resize → RGB24 返回
//! 导出阶段：批量解码 → 量化 → GIF 编码
//!
//! 内存管理：支持帧数上限和内存上限，超限时丢弃最旧帧。

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;

use crate::jpeg;
use crate::resize::resize_rgb;

// ── 单帧数据 ──

/// 一帧压缩后的数据
pub(crate) struct JpegFrame {
    /// JPEG 压缩后的字节
    data: Vec<u8>,
    /// 距录制开始的毫秒数
    elapsed_ms: u32,
}

// ── 录制配置 ──

/// 录制配置
pub struct RecordConfig {
    /// JPEG 压缩质量 (1-100)
    pub jpeg_quality: i32,
    /// 最大帧数 (0 = 不限)
    pub max_frames: usize,
    /// 最大内存字节数 (0 = 不限)
    pub max_memory_bytes: usize,
}

impl Default for RecordConfig {
    fn default() -> Self {
        Self {
            jpeg_quality: 95,
            max_frames: 0,
            max_memory_bytes: 0,
        }
    }
}

// ── 录制状态 ──

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecordState {
    Idle,
    Recording,
    Paused,
    Stopped,
}

// ── FrameStore ──

pub struct FrameStore {
    /// 录制区域宽度
    width: u32,
    /// 录制区域高度
    height: u32,
    /// 帧率
    fps: u32,
    /// 配置
    config: RecordConfig,

    /// 帧数据（JPEG 压缩）
    frames: Mutex<Vec<JpegFrame>>,
    /// 当前 JPEG 数据总字节数（原子，快速查询）
    total_bytes: AtomicU64,
    /// 丢弃的帧数
    dropped_frames: AtomicU64,

    /// 录制状态
    state: Mutex<RecordState>,
    /// 取消标志（用于 export_gif 中断）
    cancel_flag: AtomicBool,
}

impl FrameStore {
    /// 创建新的帧存储
    pub fn new(width: u32, height: u32, fps: u32, config: RecordConfig) -> Self {
        Self {
            width,
            height,
            fps,
            config,
            frames: Mutex::new(Vec::with_capacity(256)),
            total_bytes: AtomicU64::new(0),
            dropped_frames: AtomicU64::new(0),
            state: Mutex::new(RecordState::Idle),
            cancel_flag: AtomicBool::new(false),
        }
    }

    // ══════════════════════════════════════════════
    //  元信息接口
    // ══════════════════════════════════════════════

    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    pub fn fps(&self) -> u32 {
        self.fps
    }

    pub fn frame_count(&self) -> usize {
        self.frames.lock().unwrap().len()
    }

    /// 当前 JPEG 数据占用的总字节数
    pub fn memory_usage_bytes(&self) -> u64 {
        self.total_bytes.load(Ordering::Relaxed)
    }

    /// 累计丢弃的帧数（因超限策略丢弃）
    pub fn dropped_frames(&self) -> u64 {
        self.dropped_frames.load(Ordering::Relaxed)
    }

    /// 获取指定帧的时间戳（毫秒）
    pub fn get_elapsed_ms(&self, index: usize) -> Option<u32> {
        let frames = self.frames.lock().unwrap();
        frames.get(index).map(|f| f.elapsed_ms)
    }

    /// 总录制时长（毫秒），基于最后一帧的时间戳
    pub fn total_duration_ms(&self) -> u32 {
        let frames = self.frames.lock().unwrap();
        frames.last().map(|f| f.elapsed_ms).unwrap_or(0)
    }

    /// 获取所有帧的时间戳列表
    pub fn frame_timestamps(&self) -> Vec<u32> {
        let frames = self.frames.lock().unwrap();
        frames.iter().map(|f| f.elapsed_ms).collect()
    }

    // ══════════════════════════════════════════════
    //  录制状态接口
    // ══════════════════════════════════════════════

    pub fn state(&self) -> RecordState {
        *self.state.lock().unwrap()
    }

    pub fn is_recording(&self) -> bool {
        *self.state.lock().unwrap() == RecordState::Recording
    }

    pub fn is_paused(&self) -> bool {
        *self.state.lock().unwrap() == RecordState::Paused
    }

    pub fn set_state(&self, state: RecordState) {
        *self.state.lock().unwrap() = state;
    }

    // ══════════════════════════════════════════════
    //  录制阶段：接收帧
    // ══════════════════════════════════════════════

    /// 接收一帧 BGRA 数据（来自 mss 截屏），压缩为 JPEG 存储
    ///
    /// 返回 Ok(true) 表示正常存储，Ok(false) 表示被丢弃（超限），Err 表示编码失败
    pub fn push_bgra(&self, bgra: &[u8], elapsed_ms: u32) -> Result<bool, String> {
        let expected = (self.width * self.height * 4) as usize;
        if bgra.len() != expected {
            return Err(format!(
                "BGRA size mismatch: got {} expected {} ({}x{})",
                bgra.len(),
                expected,
                self.width,
                self.height
            ));
        }

        let jpeg_data = jpeg::encode_bgra(bgra, self.width, self.height, self.config.jpeg_quality)?;
        let jpeg_size = jpeg_data.len() as u64;

        let mut frames = self.frames.lock().unwrap();

        // ── 超限策略：丢弃最旧帧 ──
        let mut dropped = false;

        // 检查帧数上限
        if self.config.max_frames > 0 && frames.len() >= self.config.max_frames {
            if let Some(old) = frames.first() {
                let old_size = old.data.len() as u64;
                self.total_bytes.fetch_sub(old_size, Ordering::Relaxed);
            }
            frames.remove(0);
            self.dropped_frames.fetch_add(1, Ordering::Relaxed);
            dropped = true;
        }

        // 检查内存上限
        if self.config.max_memory_bytes > 0 {
            while self.total_bytes.load(Ordering::Relaxed) + jpeg_size
                > self.config.max_memory_bytes as u64
                && !frames.is_empty()
            {
                let old_size = frames[0].data.len() as u64;
                self.total_bytes.fetch_sub(old_size, Ordering::Relaxed);
                frames.remove(0);
                self.dropped_frames.fetch_add(1, Ordering::Relaxed);
                dropped = true;
            }
        }

        self.total_bytes.fetch_add(jpeg_size, Ordering::Relaxed);
        frames.push(JpegFrame {
            data: jpeg_data,
            elapsed_ms,
        });

        Ok(!dropped)
    }

    /// 接收一帧 RGB24 数据，压缩为 JPEG 存储
    pub fn push_rgb(&self, rgb: &[u8], elapsed_ms: u32) -> Result<bool, String> {
        let expected = (self.width * self.height * 3) as usize;
        if rgb.len() != expected {
            return Err(format!(
                "RGB size mismatch: got {} expected {} ({}x{})",
                rgb.len(),
                expected,
                self.width,
                self.height
            ));
        }

        let jpeg_data = jpeg::encode_rgb(rgb, self.width, self.height, self.config.jpeg_quality)?;
        let jpeg_size = jpeg_data.len() as u64;

        let mut frames = self.frames.lock().unwrap();

        // 同 push_bgra 的超限策略
        if self.config.max_frames > 0 && frames.len() >= self.config.max_frames {
            if let Some(old) = frames.first() {
                let old_size = old.data.len() as u64;
                self.total_bytes.fetch_sub(old_size, Ordering::Relaxed);
            }
            frames.remove(0);
            self.dropped_frames.fetch_add(1, Ordering::Relaxed);
        }

        if self.config.max_memory_bytes > 0 {
            while self.total_bytes.load(Ordering::Relaxed) + jpeg_size
                > self.config.max_memory_bytes as u64
                && !frames.is_empty()
            {
                let old_size = frames[0].data.len() as u64;
                self.total_bytes.fetch_sub(old_size, Ordering::Relaxed);
                frames.remove(0);
                self.dropped_frames.fetch_add(1, Ordering::Relaxed);
            }
        }

        self.total_bytes.fetch_add(jpeg_size, Ordering::Relaxed);
        frames.push(JpegFrame {
            data: jpeg_data,
            elapsed_ms,
        });

        Ok(true)
    }

    // ══════════════════════════════════════════════
    //  回放阶段：单帧解码
    // ══════════════════════════════════════════════

    /// 解码单帧为 RGB24（可选 resize 到 display 尺寸）
    ///
    /// 用于 seek 预览 / 首帧展示
    pub fn get_frame_rgb(
        &self,
        index: usize,
        display_w: u32,
        display_h: u32,
    ) -> Result<Vec<u8>, String> {
        let frames = self.frames.lock().unwrap();
        let frame = frames
            .get(index)
            .ok_or_else(|| format!("frame index {index} out of range (count={})", frames.len()))?;

        let decoded = jpeg::decode_to_rgb(&frame.data)?;

        // 如果需要 resize
        if display_w > 0 && display_h > 0
            && (decoded.width != display_w || decoded.height != display_h)
        {
            resize_rgb(&decoded.rgb, decoded.width, decoded.height, display_w, display_h)
        } else {
            Ok(decoded.rgb)
        }
    }

    // ══════════════════════════════════════════════
    //  清理
    // ══════════════════════════════════════════════

    /// 清空所有帧数据，并释放底层堆内存
    pub fn clear(&self) {
        let mut frames = self.frames.lock().unwrap();
        // drop 旧 Vec（释放堆内存），替换为小容量新 Vec
        // Vec::clear() 只设 len=0 但保留 capacity，不释放内存
        *frames = Vec::with_capacity(16);
        self.total_bytes.store(0, Ordering::Relaxed);
        self.dropped_frames.store(0, Ordering::Relaxed);
        *self.state.lock().unwrap() = RecordState::Idle;
        self.cancel_flag.store(false, Ordering::Relaxed);
    }

    // ══════════════════════════════════════════════
    //  取消标志 (用于 GIF 导出中断)
    // ══════════════════════════════════════════════

    pub fn set_cancel(&self, cancel: bool) {
        self.cancel_flag.store(cancel, Ordering::Relaxed);
    }

    pub fn is_cancelled(&self) -> bool {
        self.cancel_flag.load(Ordering::Relaxed)
    }

    // ══════════════════════════════════════════════
    //  内部：访问原始 JPEG 数据 (供 decoder / gif_export 使用)
    // ══════════════════════════════════════════════

    /// 获取指定范围帧的 JPEG 数据引用（在锁内执行闭包）
    #[allow(dead_code)]
    pub(crate) fn with_frames<F, R>(&self, f: F) -> R
    where
        F: FnOnce(&[JpegFrame]) -> R,
    {
        let frames = self.frames.lock().unwrap();
        f(&frames)
    }

    /// 获取单帧 JPEG 数据的克隆（用于跨线程传递）
    pub(crate) fn clone_jpeg(&self, index: usize) -> Option<(Vec<u8>, u32)> {
        let frames = self.frames.lock().unwrap();
        frames
            .get(index)
            .map(|f| (f.data.clone(), f.elapsed_ms))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn make_bgra(w: u32, h: u32) -> Vec<u8> {
        vec![128u8; (w * h * 4) as usize]
    }

    #[test]
    fn basic_push_and_get() {
        let store = FrameStore::new(64, 48, 15, RecordConfig::default());
        let bgra = make_bgra(64, 48);

        assert!(store.push_bgra(&bgra, 0).unwrap());
        assert!(store.push_bgra(&bgra, 67).unwrap());
        assert!(store.push_bgra(&bgra, 133).unwrap());

        assert_eq!(store.frame_count(), 3);
        assert_eq!(store.get_elapsed_ms(0), Some(0));
        assert_eq!(store.get_elapsed_ms(1), Some(67));
        assert_eq!(store.get_elapsed_ms(2), Some(133));
        assert_eq!(store.total_duration_ms(), 133);
        assert_eq!(store.frame_timestamps(), vec![0, 67, 133]);

        // 解码
        let rgb = store.get_frame_rgb(0, 0, 0).unwrap();
        assert_eq!(rgb.len(), (64 * 48 * 3) as usize);

        // resize 解码
        let rgb_small = store.get_frame_rgb(0, 32, 24).unwrap();
        assert_eq!(rgb_small.len(), (32 * 24 * 3) as usize);
    }

    #[test]
    fn max_frames_eviction() {
        let config = RecordConfig {
            max_frames: 3,
            ..Default::default()
        };
        let store = FrameStore::new(64, 48, 15, config);
        let bgra = make_bgra(64, 48);

        store.push_bgra(&bgra, 0).unwrap();
        store.push_bgra(&bgra, 100).unwrap();
        store.push_bgra(&bgra, 200).unwrap();
        assert_eq!(store.frame_count(), 3);
        assert_eq!(store.dropped_frames(), 0);

        // 第 4 帧会淘汰第 0 帧
        let kept = store.push_bgra(&bgra, 300).unwrap();
        assert!(!kept); // 返回 false 表示发生了淘汰
        assert_eq!(store.frame_count(), 3);
        assert_eq!(store.dropped_frames(), 1);
        assert_eq!(store.get_elapsed_ms(0), Some(100)); // 最旧的是第 1 帧
    }

    #[test]
    fn state_transitions() {
        let store = FrameStore::new(64, 48, 15, RecordConfig::default());
        assert_eq!(store.state(), RecordState::Idle);
        assert!(!store.is_recording());
        assert!(!store.is_paused());

        store.set_state(RecordState::Recording);
        assert!(store.is_recording());

        store.set_state(RecordState::Paused);
        assert!(store.is_paused());
        assert!(!store.is_recording());

        store.clear();
        assert_eq!(store.state(), RecordState::Idle);
    }

    #[test]
    fn bgra_size_mismatch() {
        let store = FrameStore::new(64, 48, 15, RecordConfig::default());
        let bad = vec![0u8; 100];
        assert!(store.push_bgra(&bad, 0).is_err());
    }
}
