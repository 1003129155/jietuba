use std::{path::PathBuf, sync::atomic::{AtomicBool, Ordering}};

/// 只保存最新控制意图，不积累控制命令或录制帧。
#[derive(Default)]
pub struct ControlState {
    stop_requested: AtomicBool,
    cancel_requested: AtomicBool,
    pause_requested: AtomicBool,
}

impl ControlState {
    pub fn pause(&self) { self.pause_requested.store(true, Ordering::SeqCst); }
    pub fn resume(&self) { self.pause_requested.store(false, Ordering::SeqCst); }
    pub fn stop(&self) { self.stop_requested.store(true, Ordering::SeqCst); }
    pub fn cancel(&self) {
        self.cancel_requested.store(true, Ordering::SeqCst);
        self.stop();
    }
    pub fn is_stopped(&self) -> bool { self.stop_requested.load(Ordering::SeqCst) }
    pub fn is_cancelled(&self) -> bool { self.cancel_requested.load(Ordering::SeqCst) }
    pub fn is_paused(&self) -> bool { self.pause_requested.load(Ordering::SeqCst) }
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Statistics {
    pub frames: u64,
    pub elapsed_ms: u64,
    pub dropped: u64,
}

/// 核心仅发出类型化的状态与统计，绑定层决定如何向调用者表示。
#[derive(Debug)]
pub enum Event {
    Ready {
        encoder: String, hardware: bool, encoder_identified: bool,
        width: u32, height: u32, fps: u32, bitrate: u32, audio: bool,
    },
    Progress { statistics: Statistics, queued_bytes: u64, max_queued_bytes: u64 },
    Paused(Statistics),
    Resumed(Statistics),
    Cancelled(Statistics),
    Complete {
        statistics: Statistics, output: PathBuf, encoder: String,
        hardware: bool, max_queued_bytes: u64,
    },
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn latest_pause_intent_and_terminal_cancel_are_kept() {
        let controls = ControlState::default();
        controls.pause();
        assert!(controls.is_paused());
        controls.resume();
        assert!(!controls.is_paused());
        controls.cancel();
        controls.resume();
        assert!(controls.is_stopped());
        assert!(controls.is_cancelled());
    }

    #[test]
    fn stop_preserves_recording_while_cancel_discards_it() {
        let controls = ControlState::default();
        controls.stop();
        assert!(controls.is_stopped());
        assert!(!controls.is_cancelled());
        controls.cancel();
        assert!(controls.is_cancelled());
    }
}
