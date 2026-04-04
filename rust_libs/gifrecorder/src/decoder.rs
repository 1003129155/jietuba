//! 后台帧解码器 — 用 crossbeam channel + 专用线程预解码 JPEG → RGB24
//!
//! Python 侧 playback_engine 通过 `next_frame()` 拉取，
//! 后台线程提前解码 N 帧放入缓冲区。

use crossbeam_channel::{bounded, Receiver};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::thread::{self, JoinHandle};

use crate::frame_store::FrameStore;
use crate::jpeg;
use crate::resize::resize_rgb;

/// 解码后的一帧
pub struct DecodedFrame {
    /// RGB24 像素数据，长度 = display_w * display_h * 3
    pub rgb: Vec<u8>,
    /// 原始帧在录制时间轴上的偏移 (ms)
    pub elapsed_ms: u32,
}

/// 后台帧解码器
pub struct FrameDecoder {
    rx: Receiver<DecodedFrame>,
    stop_flag: Arc<AtomicBool>,
    handle: Option<JoinHandle<()>>,
    /// 本次解码的总帧数 (用于循环播放判断)
    total_frames: usize,
    /// 已经取出的帧数
    fetched: usize,
}

impl FrameDecoder {
    /// 创建一个后台解码器
    ///
    /// * `store`      - 共享的 FrameStore (Arc)
    /// * `display_w`  - 输出宽度 (0 表示保持原始尺寸)
    /// * `display_h`  - 输出高度 (0 表示保持原始尺寸)
    /// * `prefetch`   - 预解码缓冲区大小 (推荐 4~8)
    pub fn start(
        store: Arc<FrameStore>,
        display_w: u32,
        display_h: u32,
        prefetch: usize,
    ) -> Self {
        let total_frames = store.frame_count();
        let (tx, rx) = bounded::<DecodedFrame>(prefetch.max(1));
        let stop_flag = Arc::new(AtomicBool::new(false));
        let flag = stop_flag.clone();

        let handle = thread::spawn(move || {
            let src_w = store.width();
            let src_h = store.height();
            let need_resize =
                display_w > 0 && display_h > 0 && (display_w != src_w || display_h != src_h);

            for idx in 0..total_frames {
                if flag.load(Ordering::Relaxed) {
                    break;
                }

                // 从 FrameStore 拿到 JPEG bytes + elapsed_ms
                let (jpeg_data, elapsed_ms) = match store.clone_jpeg(idx) {
                    Some(v) => v,
                    None => break,
                };

                // 解码 JPEG → RGB24
                let decoded = match jpeg::decode_to_rgb(&jpeg_data) {
                    Ok(d) => d,
                    Err(_) => continue,
                };

                let rgb = if need_resize {
                    match resize_rgb(&decoded.rgb, decoded.width, decoded.height, display_w, display_h) {
                        Ok(r) => r,
                        Err(_) => decoded.rgb,
                    }
                } else {
                    decoded.rgb
                };

                let frame = DecodedFrame { rgb, elapsed_ms };

                // 阻塞式发送 — 如果缓冲区满就等待消费者取走
                if tx.send(frame).is_err() {
                    break; // receiver 被 drop，退出
                }
            }
            // 线程自然结束，channel close
        });

        Self {
            rx,
            stop_flag,
            handle: Some(handle),
            total_frames,
            fetched: 0,
        }
    }

    /// 取下一帧 (阻塞直到有帧可用)
    ///
    /// 返回 None 表示所有帧已解码完毕 / 已停止
    pub fn next_frame(&mut self) -> Option<DecodedFrame> {
        match self.rx.recv() {
            Ok(f) => {
                self.fetched += 1;
                Some(f)
            }
            Err(_) => None, // channel closed
        }
    }

    /// 非阻塞尝试取帧
    pub fn try_next_frame(&mut self) -> Option<DecodedFrame> {
        match self.rx.try_recv() {
            Ok(f) => {
                self.fetched += 1;
                Some(f)
            }
            Err(_) => None,
        }
    }

    /// 当前缓冲区中等待消费的帧数
    pub fn buffered_count(&self) -> usize {
        self.rx.len()
    }

    /// 是否已经取完所有帧
    pub fn is_finished(&self) -> bool {
        self.fetched >= self.total_frames && self.rx.is_empty()
    }

    /// 总帧数
    pub fn total_frames(&self) -> usize {
        self.total_frames
    }

    /// 已取帧数
    pub fn fetched_count(&self) -> usize {
        self.fetched
    }

    /// 停止后台解码线程
    pub fn stop(&mut self) {
        self.stop_flag.store(true, Ordering::Relaxed);
        // 排空 channel 让发送端尽快退出
        while self.rx.try_recv().is_ok() {}
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

impl Drop for FrameDecoder {
    fn drop(&mut self) {
        self.stop();
    }
}
