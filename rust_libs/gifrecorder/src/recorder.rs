//! 录制会话 — 在独立 Rust 线程中执行截屏循环
//!
//! Rust 线程: FrameSource::grab() → FrameStore::push_bgra()，截屏和 JPEG 压缩都不占 GIL；
//! 截取 DXGI 优先、GDI BitBlt 兜底，见 frame_source。
//! Python 侧只调用 prepare/begin/pause/resume/stop，不碰像素。

use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::frame_source::{Backend, DxgiCapture, FrameSource};
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
    /// 最近一帧的截取路径 (0=尚无, 1=DXGI, 2=GDI)
    backend: AtomicU8,
    /// 录制线程已定好截取来源（DXGI 会话建好或已放弃），此后 begin 即刻开始截取
    ready: AtomicBool,
}

/// 暂停时的兜底轮询间隔：resume()/stop() 会 unpark，正常用不到它。
/// 测试构建里调长，才分得清线程是被叫醒的还是等到了兜底。
const PAUSE_POLL: Duration = Duration::from_millis(if cfg!(test) { 1000 } else { 50 });

/// [`RecordSession::begin`] 交给录制线程的参数
struct Begin {
    store: Arc<FrameStore>,
    left: i32,
    top: i32,
    width: i32,
    height: i32,
    fps: u32,
}

/// 录制会话
pub struct RecordSession {
    control: Arc<SessionControl>,
    handle: Option<JoinHandle<()>>,
    begin: Option<Sender<Begin>>,
    store: Option<Arc<FrameStore>>,
}

impl RecordSession {
    /// 预备录制：立即起录制线程并在其中建好 DXGI 会话，[`Self::begin`] 之后才开始截取。
    ///
    /// 建 DXGI 会话要几十到上百毫秒；录制界面一打开就预备，点录制时画面即刻开始。
    /// `prefer_dxgi` 为 false 时只用 GDI。
    pub fn prepare(prefer_dxgi: bool) -> Self {
        let control = Arc::new(SessionControl {
            state: AtomicU8::new(SESSION_IDLE),
            stop: AtomicBool::new(false),
            paused: AtomicBool::new(false),
            backend: AtomicU8::new(0),
            ready: AtomicBool::new(false),
        });
        let (sender, receiver) = mpsc::channel();
        let ctrl = control.clone();
        let handle = thread::spawn(move || session_thread(receiver, ctrl, prefer_dxgi));

        Self {
            control,
            handle: Some(handle),
            begin: Some(sender),
            store: None,
        }
    }

    /// 开始截取，每个会话只能调用一次
    ///
    /// * `store`  — 帧存储（共享 Arc）
    /// * `left`, `top` — 屏幕截取起点
    /// * `width`, `height` — 截取区域大小
    /// * `fps` — 目标帧率
    pub fn begin(
        &mut self,
        store: Arc<FrameStore>,
        left: i32,
        top: i32,
        width: i32,
        height: i32,
        fps: u32,
    ) -> Result<(), String> {
        let sender = self.begin.take().ok_or("recording has already begun")?;
        self.control.state.store(SESSION_RECORDING, Ordering::Release);
        sender
            .send(Begin { store: store.clone(), left, top, width, height, fps })
            .map_err(|_| "recording thread has exited".to_string())?;
        self.store = Some(store);
        Ok(())
    }

    /// 预备并立即开始录制
    pub fn start(
        store: Arc<FrameStore>,
        left: i32,
        top: i32,
        width: i32,
        height: i32,
        fps: u32,
        prefer_dxgi: bool,
    ) -> Result<Self, String> {
        let mut session = Self::prepare(prefer_dxgi);
        session.begin(store, left, top, width, height, fps)?;
        Ok(session)
    }

    /// 暂停录制
    pub fn pause(&self) {
        self.control.paused.store(true, Ordering::Release);
        self.control.state.store(SESSION_PAUSED, Ordering::Release);
        self.wake();
    }

    /// 恢复录制
    pub fn resume(&self) {
        self.control.paused.store(false, Ordering::Release);
        self.control.state.store(SESSION_RECORDING, Ordering::Release);
        self.wake();
    }

    /// 停止录制（阻塞等待线程退出）
    pub fn stop(&mut self) {
        self.control.stop.store(true, Ordering::Release);
        self.control.paused.store(false, Ordering::Release); // 解除暂停
        // 还没 begin 的线程在等参数，关掉通道让它退出
        self.begin = None;
        if let Some(h) = self.handle.take() {
            h.thread().unpark();
            let _ = h.join();
        }
        self.control.state.store(SESSION_STOPPED, Ordering::Release);
    }

    /// 录制线程在帧间和暂停时 park，状态变化后叫醒它，不必等满一个间隔
    fn wake(&self) {
        if let Some(h) = &self.handle {
            h.thread().unpark();
        }
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

    /// 最近一帧的截取路径："dxgi"、"gdi"，尚未截到帧时为 None
    pub fn backend(&self) -> Option<&'static str> {
        match self.control.backend.load(Ordering::Acquire) {
            1 => Some("dxgi"),
            2 => Some("gdi"),
            _ => None,
        }
    }

    /// 获取 FrameStore 引用；begin 之前为 None
    pub fn store(&self) -> Option<&Arc<FrameStore>> {
        self.store.as_ref()
    }

    /// 录制线程是否已定好截取来源；之前调用 begin，第一帧要等 DXGI 会话建完
    pub fn is_ready(&self) -> bool {
        self.control.ready.load(Ordering::Acquire)
    }
}

impl Drop for RecordSession {
    fn drop(&mut self) {
        self.stop();
    }
}

/// 录制线程：先建 DXGI 会话（它绑定在创建它的线程上），再等 begin 的参数
fn session_thread(begin: Receiver<Begin>, ctrl: Arc<SessionControl>, prefer_dxgi: bool) {
    let dxgi = if prefer_dxgi {
        DxgiCapture::new()
            .map_err(|e| eprintln!("[gifrecorder] DXGI 会话建立失败，改用 GDI: {e}"))
            .ok()
    } else {
        None
    };
    ctrl.ready.store(true, Ordering::Release);
    // 通道关闭说明还没开始录制就被停止了
    let Ok(Begin { store, left, top, width, height, fps }) = begin.recv() else {
        return;
    };
    let capturer = match FrameSource::new(dxgi, left, top, width, height) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[gifrecorder] FrameSource 创建失败: {e}");
            ctrl.state.store(SESSION_STOPPED, Ordering::Release);
            return;
        }
    };
    capture_loop(store, ctrl, capturer, fps);
}

/// 截屏循环（在录制线程运行）
fn capture_loop(store: Arc<FrameStore>, ctrl: Arc<SessionControl>, mut capturer: FrameSource, fps: u32) {
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
            thread::park_timeout(PAUSE_POLL);
            continue;
        } else if let Some(ps) = pause_start.take() {
            // 刚从暂停恢复：累加暂停时长
            pause_offset += ps.elapsed();
        }

        // ── fps 节拍控制 ──
        // park 而不是 sleep：stop()/pause() 会 unpark，停止不必等满一个帧间隔。
        // park 可能提前返回，所以醒来后回到循环顶部重新检查。
        let target_time = frame_interval * frame_count as u32;
        let wall_elapsed = record_start.elapsed() - pause_offset;
        if wall_elapsed < target_time {
            let remaining = target_time - wall_elapsed;
            if remaining > Duration::from_micros(500) {
                thread::park_timeout(remaining);
                continue;
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
        let backend = match capturer.last_backend() {
            Some(Backend::Dxgi) => 1,
            Some(Backend::Gdi) => 2,
            None => 0,
        };
        ctrl.backend.store(backend, Ordering::Release);

        frame_count += 1;
    }

    // 线程结束，capturer 在 Drop 中释放 DXGI 会话与 GDI 资源
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frame_store::RecordConfig;

    fn small_store(fps: u32) -> Arc<FrameStore> {
        Arc::new(FrameStore::new(64, 48, fps, RecordConfig {
            jpeg_quality: 80,
            ..Default::default()
        }))
    }

    fn wait_for_frames(store: &FrameStore, count: usize, limit: Duration) -> Duration {
        let start = Instant::now();
        while store.frame_count() < count && start.elapsed() < limit {
            thread::sleep(Duration::from_millis(1));
        }
        start.elapsed()
    }

    #[test]
    fn prepared_session_captures_as_soon_as_it_begins() {
        let mut session = RecordSession::prepare(true);
        // 建 DXGI 会话的耗时看机器：没有显卡的 CI 上要一秒多，没有 DXGI 的环境会放弃改用 GDI
        let setup = Instant::now();
        while !session.is_ready() && setup.elapsed() < Duration::from_secs(10) {
            thread::sleep(Duration::from_millis(5));
        }
        assert!(session.is_ready());
        assert!(!session.is_recording());

        // 1fps：开始后若没有立即截取，第一帧要等将近一整个帧间隔
        let store = small_store(1);
        session.begin(store.clone(), 0, 0, 64, 48, 1).unwrap();
        let first_frame = wait_for_frames(&store, 1, Duration::from_secs(3));
        session.stop();

        assert!(store.frame_count() >= 1);
        assert!(first_frame < Duration::from_millis(500), "first frame after {first_frame:?}");
    }

    #[test]
    fn stop_does_not_wait_for_the_next_frame() {
        let store = small_store(1);
        let mut session = RecordSession::start(store.clone(), 0, 0, 64, 48, 1, false).unwrap();
        wait_for_frames(&store, 1, Duration::from_secs(2));

        // 1fps 时下一帧要等将近 1 秒，stop() 应当叫醒线程而不是等它
        let stop_start = Instant::now();
        session.stop();
        assert!(stop_start.elapsed() < Duration::from_millis(300), "stop took {:?}", stop_start.elapsed());
    }

    #[test]
    fn resume_does_not_wait_for_the_pause_poll() {
        // 恢复后下一帧仍按原节拍排期；帧间隔取 10ms，测试构建的兜底轮询是 PAUSE_POLL（1s），
        // 截一帧再慢也用不了它的一半，这样测到的就是 resume() 有没有叫醒线程。
        let store = small_store(100);
        let mut session = RecordSession::start(store.clone(), 0, 0, 64, 48, 100, false).unwrap();
        wait_for_frames(&store, 1, Duration::from_secs(2));
        session.pause();
        thread::sleep(Duration::from_millis(120));

        let paused_count = store.frame_count();
        session.resume();
        let next_frame = wait_for_frames(&store, paused_count + 1, Duration::from_secs(3));
        session.stop();
        assert!(next_frame < PAUSE_POLL / 2, "first frame after resume took {next_frame:?}");
    }

    #[test]
    fn stopping_a_prepared_session_before_it_begins_records_nothing() {
        let mut session = RecordSession::prepare(false);
        session.stop();
        assert!(session.is_stopped());
        assert!(session.store().is_none());
        assert!(session.backend().is_none());
    }

    #[test]
    fn a_session_begins_only_once() {
        let mut session = RecordSession::prepare(false);
        session.begin(small_store(10), 0, 0, 64, 48, 10).unwrap();
        assert!(session.begin(small_store(10), 0, 0, 64, 48, 10).is_err());
        session.stop();
    }

    #[test]
    fn record_session_basic() {
        let store = Arc::new(FrameStore::new(64, 48, 10, RecordConfig {
            jpeg_quality: 80,
            ..Default::default()
        }));

        let mut session = RecordSession::start(
            store.clone(), 0, 0, 64, 48, 10, true,
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
            store.clone(), 0, 0, 64, 48, 10, true,
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
