//! 录制会话 — 在独立 Rust 线程中执行截屏循环
//!
//! 架构：
//!   Rust 线程: ScreenCapture::grab() → FrameStore::push_bgra()
//!   Python 侧: 仅调用 start/pause/resume/stop，完全不碰像素
//!
//! 优势：
//!   - 零 GIL 争用（截屏 + JPEG 压缩全在 Rust 线程）
//!   - 无 mss 依赖（直接 Win32 BitBlt）
//!   - 精确 fps 节拍控制

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::capture::ScreenCapture;
use crate::frame_store::FrameStore;

/// 录制会话状态
const SESSION_IDLE: u8 = 0;
const SESSION_RECORDING: u8 = 1;
const SESSION_PAUSED: u8 = 2;
const SESSION_STOPPED: u8 = 3;

/// 共享控制标志
struct SessionControl {
    /// 当前状态 (0=idle, 1=recording, 2=paused, 3=stopped)
    state: AtomicU8,
    /// 停止标志
    stop: AtomicBool,
    /// 暂停标志
    paused: AtomicBool,
}

/// 录制会话
pub struct RecordSession {
    control: Arc<SessionControl>,
    handle: Option<JoinHandle<()>>,
    store: Arc<FrameStore>,
}

impl RecordSession {
    /// 启动录制会话
    ///
    /// * `store`  — 帧存储（共享 Arc）
    /// * `left`, `top` — 屏幕截取起点
    /// * `width`, `height` — 截取区域大小
    /// * `fps` — 目标帧率
    pub fn start(
        store: Arc<FrameStore>,
        left: i32,
        top: i32,
        width: i32,
        height: i32,
        fps: u32,
    ) -> Result<Self, String> {
        let control = Arc::new(SessionControl {
            state: AtomicU8::new(SESSION_RECORDING),
            stop: AtomicBool::new(false),
            paused: AtomicBool::new(false),
        });

        let ctrl = control.clone();
        let store_clone = store.clone();

        let handle = thread::spawn(move || {
            capture_loop(store_clone, ctrl, left, top, width, height, fps);
        });

        Ok(Self {
            control,
            handle: Some(handle),
            store,
        })
    }

    /// 暂停录制
    pub fn pause(&self) {
        self.control.paused.store(true, Ordering::Release);
        self.control.state.store(SESSION_PAUSED, Ordering::Release);
    }

    /// 恢复录制
    pub fn resume(&self) {
        self.control.paused.store(false, Ordering::Release);
        self.control.state.store(SESSION_RECORDING, Ordering::Release);
    }

    /// 停止录制（阻塞等待线程退出）
    pub fn stop(&mut self) {
        self.control.stop.store(true, Ordering::Release);
        self.control.paused.store(false, Ordering::Release); // 解除暂停
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
        self.control.state.store(SESSION_STOPPED, Ordering::Release);
    }

    /// 当前状态
    pub fn state(&self) -> u8 {
        self.control.state.load(Ordering::Acquire)
    }

    /// 是否正在录制
    pub fn is_recording(&self) -> bool {
        self.state() == SESSION_RECORDING
    }

    /// 是否暂停
    pub fn is_paused(&self) -> bool {
        self.state() == SESSION_PAUSED
    }

    /// 是否已停止
    pub fn is_stopped(&self) -> bool {
        let s = self.state();
        s == SESSION_STOPPED || s == SESSION_IDLE
    }

    /// 获取 FrameStore 引用
    pub fn store(&self) -> &Arc<FrameStore> {
        &self.store
    }
}

impl Drop for RecordSession {
    fn drop(&mut self) {
        self.stop();
    }
}

/// 截屏循环（在独立线程运行）
fn capture_loop(
    store: Arc<FrameStore>,
    ctrl: Arc<SessionControl>,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    fps: u32,
) {
    // 创建 GDI 截屏上下文
    let mut capturer = match ScreenCapture::new(left, top, width, height) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[gifrecorder] ScreenCapture 创建失败: {e}");
            ctrl.state.store(SESSION_STOPPED, Ordering::Release);
            return;
        }
    };

    let frame_interval = Duration::from_secs_f64(1.0 / fps as f64);
    let record_start = Instant::now();
    let mut frame_count: u64 = 0;
    let mut pause_offset = Duration::ZERO;
    let mut pause_start: Option<Instant> = None;

    loop {
        // ── 检查停止 ──
        if ctrl.stop.load(Ordering::Acquire) {
            break;
        }

        // ── 暂停处理 ──
        if ctrl.paused.load(Ordering::Acquire) {
            if pause_start.is_none() {
                pause_start = Some(Instant::now());
            }
            // 暂停期间 spin-sleep 50ms
            thread::sleep(Duration::from_millis(50));
            continue;
        } else if let Some(ps) = pause_start.take() {
            // 刚从暂停恢复：累加暂停时长
            pause_offset += ps.elapsed();
        }

        // ── fps 节拍控制 ──
        let target_time = frame_interval * frame_count as u32;
        let wall_elapsed = record_start.elapsed() - pause_offset;
        if wall_elapsed < target_time {
            let sleep = target_time - wall_elapsed;
            if sleep > Duration::from_micros(500) {
                thread::sleep(sleep);
            }
            // 再次检查停止
            if ctrl.stop.load(Ordering::Acquire) {
                break;
            }
        }

        // ── 截屏 ──
        let bgra = match capturer.grab() {
            Ok(data) => data,
            Err(_) => {
                frame_count += 1;
                continue; // 偶尔截屏失败（例如切换桌面）跳过
            }
        };

        // ── 计算 elapsed_ms（排除暂停时间）──
        let elapsed = record_start.elapsed() - pause_offset;
        let elapsed_ms = elapsed.as_millis() as u32;

        // ── 存入 FrameStore（JPEG 压缩在此发生）──
        let _ = store.push_bgra(bgra, elapsed_ms);

        frame_count += 1;
    }

    // 线程结束，capturer 在 Drop 中释放 GDI 资源
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame_store::RecordConfig;

    #[test]
    fn record_session_basic() {
        let store = Arc::new(FrameStore::new(64, 48, 10, RecordConfig {
            jpeg_quality: 80,
            ..Default::default()
        }));

        let mut session = RecordSession::start(
            store.clone(), 0, 0, 64, 48, 10,
        ).unwrap();

        assert!(session.is_recording());

        // 录制约 300ms
        thread::sleep(Duration::from_millis(300));

        session.stop();
        assert!(session.is_stopped());

        let count = store.frame_count();
        // 10fps × 0.3s ≈ 3 帧（允许 1~5）
        assert!(count >= 1, "frame_count = {count}");
        assert!(count <= 6, "frame_count = {count}");
    }

    #[test]
    fn record_session_pause_resume() {
        let store = Arc::new(FrameStore::new(64, 48, 10, RecordConfig {
            jpeg_quality: 80,
            ..Default::default()
        }));

        let mut session = RecordSession::start(
            store.clone(), 0, 0, 64, 48, 10,
        ).unwrap();

        // 录制 200ms
        thread::sleep(Duration::from_millis(200));
        let count1 = store.frame_count();

        // 暂停 300ms
        session.pause();
        assert!(session.is_paused());
        thread::sleep(Duration::from_millis(300));
        let count_during_pause = store.frame_count();
        // 暂停信号有极短的竞争窗口，可能多抓 1 帧
        assert!(count_during_pause <= count1 + 1, "暂停期间帧数异常增长");

        // 恢复 200ms
        session.resume();
        assert!(session.is_recording());
        thread::sleep(Duration::from_millis(200));

        session.stop();
        let count_final = store.frame_count();
        assert!(count_final > count1, "恢复后应有新帧");
    }
}
