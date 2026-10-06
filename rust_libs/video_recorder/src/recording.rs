use std::{marker::PhantomData, rc::Rc, thread, time::{Duration, Instant}};
use windows::Win32::UI::HiDpi::{SetThreadDpiAwarenessContext, DPI_AWARENESS_CONTEXT,
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2};
use crate::{audio::{AudioCapture, qpc_ticks_100ns}, capture::Capture,
    diagnostic::diagnostic, error::Result, options::Options,
    session::{ControlState, Event, Statistics}, writer::{MfRuntime, OwnedOutput, Writer}};

/// 扩展只改变录制线程的 DPI 上下文，退出时恢复，不影响宿主的 Qt 窗口。
struct PhysicalPixels { previous: DPI_AWARENESS_CONTEXT, _thread: PhantomData<Rc<()>> }

impl PhysicalPixels {
    fn new() -> Self {
        Self { previous: unsafe { SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) },
            _thread: PhantomData }
    }
}

impl Drop for PhysicalPixels {
    fn drop(&mut self) {
        if !self.previous.0.is_null() { unsafe { SetThreadDpiAwarenessContext(self.previous); } }
    }
}

fn stats(frames: u64, active: f64, dropped: u64) -> Statistics {
    Statistics { frames, elapsed_ms: (active.max(0.0) * 1000.0) as u64, dropped }
}

pub fn record(options: &Options, controls: &ControlState,
    emit: &mut impl FnMut(Event) -> Result<()>) -> Result<()>
{
    let _pixels = PhysicalPixels::new();
    options.validate()?;
    if controls.is_stopped() { return emit(Event::Cancelled(Statistics::default())); }
    // COM、MF、GDI、WASAPI 及编码器均在当前线程创建和释放。
    let _runtime = MfRuntime::new()?;
    let mut output = OwnedOutput::new(&options.output)?;
    let mut capture = Capture::new(options)?;
    let mut audio = AudioCapture::new(options.audio)?;
    let mut writer = match Writer::new(options, &mut output, options.hardware) {
        Ok(writer) => writer,
        Err(error) if options.hardware => {
            diagnostic("software_fallback", &error.message);
            if controls.is_stopped() { return emit(Event::Cancelled(Statistics::default())); }
            Writer::new(options, &mut output, false)?
        }
        Err(error) => return Err(error),
    };
    if controls.is_stopped() {
        // 尚未写入任何帧，不封装将要删除的空流。
        return emit(Event::Cancelled(Statistics::default()));
    }
    let metadata = writer.metadata();
    let encoder = metadata.encoder.clone();
    let hardware = metadata.hardware;
    let identified = metadata.encoder_identified;
    let mut frames = 0u64;
    let mut dropped = 0u64;
    let mut max_queued = 0u64;
    let mut active = 0.0;
    let start = Instant::now();
    let mut deadline = start;
    let mut paused = Duration::ZERO;
    let mut pause_start = None;
    let interval = Duration::from_nanos(1_000_000_000 / u64::from(options.fps));
    let mut next_progress = 0.0;
    audio.start(qpc_ticks_100ns()?)?;
    let recording = (|| -> Result<()> {
        emit(Event::Ready { encoder: encoder.clone(), hardware, encoder_identified: identified,
            width: options.width, height: options.height, fps: options.fps,
            bitrate: options.bitrate, audio: options.audio })?;
        while !controls.is_stopped() {
            let now = Instant::now();
            active = pause_start.unwrap_or(now).duration_since(start).saturating_sub(paused).as_secs_f64();
            if options.duration.is_some_and(|limit| active >= limit) { break; }
            if controls.is_paused() && pause_start.is_none() {
                audio.pause()?;
                audio.write_until(&writer, active)?;
                pause_start = Some(now);
                emit(Event::Paused(stats(frames, active, dropped)))?; continue;
            }
            if !controls.is_paused() {
                if let Some(since) = pause_start.take() {
                    let duration = now.duration_since(since);
                    paused += duration;
                    deadline += duration;
                    audio.resume((paused.as_secs_f64()*10_000_000.0) as i64)?;
                    emit(Event::Resumed(stats(frames, active, dropped)))?; continue;
                }
            }
            if pause_start.is_some() { thread::sleep(Duration::from_millis(10)); continue; }
            audio.poll()?;
            audio.write_until(&writer, active - 0.10)?;
            if now < deadline {
                thread::sleep((deadline.duration_since(now)+Duration::from_millis(1)).min(Duration::from_millis(10)));
                continue;
            }
            let bgra = capture.grab(options, frames)?;
            writer.write_video(bgra, options.width, options.height,
                if frames == 0 { 0 } else { (active*10_000_000.0) as i64 })?;
            frames += 1;
            let queued = writer.queued_bytes();
            max_queued = max_queued.max(queued);
            let written = Instant::now();
            deadline += interval;
            // 系统编码器保持背压，过期机会丢弃，绝不积累整个录像的帧历史。
            while deadline + interval < written { deadline += interval; dropped += 1; }
            if active >= next_progress {
                emit(Event::Progress { statistics: stats(frames, active, dropped),
                    queued_bytes: queued, max_queued_bytes: max_queued })?;
                next_progress = active + 1.0;
            }
        }
        active = pause_start.unwrap_or_else(Instant::now).duration_since(start).saturating_sub(paused).as_secs_f64();
        if let Some(limit) = options.duration { active = active.min(limit); }
        if frames == 0 { return emit(Event::Cancelled(stats(frames, active, dropped))); }
        if pause_start.is_none() {
            let drain = Instant::now();
            while drain.elapsed() < Duration::from_millis(100) {
                audio.poll()?;
                thread::sleep(Duration::from_millis(5));
            }
            audio.pause()?;
        }
        audio.write_until(&writer, active)?;
        audio.finish_log();
        writer.finalize((active*10_000_000.0) as i64)?;
        if controls.is_cancelled() { return emit(Event::Cancelled(stats(frames, active, dropped))); }
        // 先关闭所有文件引用，后通知 Python，界面不把尚未封装的 MP4 当作成功。
        output.preserve();
        emit(Event::Complete { statistics: stats(frames, active, dropped), output: options.output.clone(),
            encoder, hardware, max_queued_bytes: max_queued })?;
        Ok(())
    })();
    if recording.is_err() {
        // Python 回调失败也经过同一收尾路径，不再调用已失败的观察者。
        active = pause_start.unwrap_or_else(Instant::now).duration_since(start).saturating_sub(paused).as_secs_f64();
        if let Some(limit) = options.duration { active = active.min(limit); }
        let result = writer.finalize((active*10_000_000.0) as i64);
        diagnostic("failure_finalize", &format!("{result:?}"));
        let preserve = frames > 0 && !controls.is_cancelled();
        output.set_preserve(preserve);
        if preserve { diagnostic("partial_output", &options.output.to_string_lossy()); }
    }
    recording
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::Failure;

    #[test]
    fn cancelled_before_run_does_not_open_output_or_encoder() {
        let options = Options { output: std::env::temp_dir().join("unused-video-recorder-cancel.mp4"),
            width: 320, height: 240, synthetic: true, ..Options::default() };
        let controls = ControlState::default();
        controls.cancel();
        let mut count = 0;
        record(&options, &controls, &mut |event| {
            let Event::Cancelled(stats) = event else { panic!("Unexpected session event") };
            assert_eq!(stats.frames, 0);
            count += 1;
            Ok(())
        }).unwrap();
        assert_eq!(count, 1);
    }

    #[test]
    fn cancelled_observer_failure_is_returned_without_followup_events() {
        let options = Options { output: std::env::temp_dir().join("unused-video-recorder-observer.mp4"),
            width: 320, height: 240, synthetic: true, ..Options::default() };
        let controls = ControlState::default();
        controls.stop();
        let mut calls = 0;
        let error = record(&options, &controls, &mut |_| {
            calls += 1;
            Err(Failure::new("observer", "Observer rejected cancellation"))
        }).unwrap_err();
        assert_eq!(error.code, "observer");
        assert_eq!(calls, 1);
    }
}
