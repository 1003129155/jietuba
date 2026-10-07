use std::{path::PathBuf, sync::Mutex};
use pyo3::{exceptions::PyTypeError, prelude::*, types::PyDict};
use crate::{error::Failure, options::Options, recording, session::{ControlState, Event, Statistics}};

// PyO3 0.22 的异常宏检查已废弃的 gil-refs feature；只在宏所在模块兼容它。
#[allow(unexpected_cfgs)]
mod exceptions {
    pyo3::create_exception!(video_recorder, RecorderError, pyo3::exceptions::PyRuntimeError);
}
use exceptions::RecorderError;

fn python_error(py: Python<'_>, error: Failure) -> PyErr {
    let result = RecorderError::new_err(error.message);
    // 稳定错误码供界面翻译，原始消息仅用于诊断。
    if let Err(attribute_error) = result.value_bound(py).setattr("code", error.code) {
        return attribute_error;
    }
    result
}

#[pyclass(name = "Recorder", module = "video_recorder", frozen)]
struct Recorder {
    // run 一次性取走配置，录制过程不持有互斥锁；控制调用只写入原子状态。
    options: Mutex<Option<Options>>,
    controls: ControlState,
}

#[pymethods]
impl Recorder {
    #[new]
    #[pyo3(signature = (output, left, top, width, height, *, fps=30, bitrate=4_000_000,
        system_audio=true, hardware=true, cursor=true, prefer_dxgi=true))]
    fn new(output: PathBuf, left: i32, top: i32, width: u32, height: u32,
        fps: u32, bitrate: u32, system_audio: bool, hardware: bool, cursor: bool,
        prefer_dxgi: bool) -> Self
    {
        Self {
            options: Mutex::new(Some(Options { output, x: left, y: top, width, height, fps,
                bitrate, audio: system_audio, hardware, cursor, prefer_dxgi,
                ..Options::default() })),
            controls: ControlState::default(),
        }
    }

    fn run(&self, py: Python<'_>, callback: Py<PyAny>) -> PyResult<()> {
        if !callback.bind(py).is_callable() { return Err(PyTypeError::new_err("callback must be callable")); }
        let options = self.options.lock()
            .map_err(|_| python_error(py, Failure::new("state", "Recorder state is unavailable")))?
            .take().ok_or_else(|| python_error(py, Failure::new("state", "Recorder.run can only be called once")))?;
        let mut callback_error = None;
        // 采集和编码期间释放 GIL，其他 Python 线程可随时暂停、停止或取消。
        let result = py.allow_threads(|| recording::record(&options, &self.controls, &mut |event| {
            let result = Python::with_gil(|py| -> PyResult<()> {
                let (name, data) = event_data(py, event)?;
                callback.call1(py, (name, data))?;
                Ok(())
            });
            result.map_err(|error| {
                callback_error = Some(error);
                Failure::new("callback", "Recording callback failed")
            })
        }));
        // 核心已完成资源释放；保留回调原异常及 traceback，不用收尾错误覆盖它。
        if let Some(error) = callback_error { return Err(error); }
        result.map_err(|error| python_error(py, error))
    }

    fn pause(&self) { self.controls.pause(); }
    fn resume(&self) { self.controls.resume(); }
    fn stop(&self) { self.controls.stop(); }
    fn cancel(&self) { self.controls.cancel(); }

    #[cfg(feature = "test-support")]
    #[pyo3(signature = (*, synthetic=true, medium=false, duration=None, fail_hardware=false))]
    fn _configure_test(&self, py: Python<'_>, synthetic: bool, medium: bool,
        duration: Option<f64>, fail_hardware: bool) -> PyResult<()>
    {
        if duration.is_some_and(|value| !value.is_finite() || value <= 0.0 || value > 86400.0) {
            return Err(python_error(py, Failure::new("arguments", "Invalid test duration")));
        }
        let mut pending = self.options.lock()
            .map_err(|_| python_error(py, Failure::new("state", "Recorder state is unavailable")))?;
        let options = pending.as_mut()
            .ok_or_else(|| python_error(py, Failure::new("state", "Cannot configure a started Recorder")))?;
        options.synthetic = synthetic || medium;
        options.synthetic_medium = medium;
        options.duration = duration;
        options.fail_hardware = fail_hardware;
        Ok(())
    }
}

fn add_stats(data: &Bound<'_, PyDict>, stats: Statistics) -> PyResult<()> {
    data.set_item("frames", stats.frames)?;
    data.set_item("elapsed_ms", stats.elapsed_ms)?;
    data.set_item("dropped", stats.dropped)
}

/// Python 字典只在接口层创建；不把原始画面或音频跨越 Python 边界。
fn event_data(py: Python<'_>, event: Event) -> PyResult<(&'static str, Bound<'_, PyDict>)> {
    let data = PyDict::new_bound(py);
    let name = match event {
        Event::Ready { encoder, hardware, encoder_identified, width, height, fps, bitrate, audio } => {
            data.set_item("encoder", encoder)?;
            data.set_item("hardware", hardware)?;
            data.set_item("encoder_identified", encoder_identified)?;
            data.set_item("width", width)?;
            data.set_item("height", height)?;
            data.set_item("fps", fps)?;
            data.set_item("bitrate", bitrate)?;
            data.set_item("audio", if audio { "system" } else { "none" })?;
            "ready"
        }
        Event::Progress { statistics, queued_bytes, max_queued_bytes } => {
            add_stats(&data, statistics)?;
            data.set_item("queued_bytes", queued_bytes)?;
            data.set_item("max_queued_bytes", max_queued_bytes)?;
            "progress"
        }
        Event::Paused(stats) => { add_stats(&data, stats)?; "paused" }
        Event::Resumed(stats) => { add_stats(&data, stats)?; "resumed" }
        Event::Cancelled(stats) => { add_stats(&data, stats)?; "cancelled" }
        Event::Complete { statistics, output, encoder, hardware, max_queued_bytes } => {
            add_stats(&data, statistics)?;
            data.set_item("output", &output)?;
            data.set_item("path", output)?;
            data.set_item("encoder", encoder)?;
            data.set_item("hardware", hardware)?;
            data.set_item("max_queued_bytes", max_queued_bytes)?;
            data.set_item("finalized", true)?;
            "complete"
        }
    };
    Ok((name, data))
}

#[pymodule]
fn video_recorder(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add("__version__", env!("CARGO_PKG_VERSION"))?;
    m.add("RecorderError", m.py().get_type_bound::<RecorderError>())?;
    m.add_class::<Recorder>()
}
