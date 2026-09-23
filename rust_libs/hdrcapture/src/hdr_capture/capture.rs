//! Synchronous, HDR-aware screen capture built on DXGI Desktop Duplication.

use std::time::{Duration, Instant};

use windows::Win32::Graphics::Direct3D11::{ID3D11Device, ID3D11DeviceContext, ID3D11Texture2D};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_MODE_ROTATION_IDENTITY, DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE270,
};

use super::display::{self, MonitorDescriptor, Rect};
use super::gpu::{self, GpuCompositor, GpuToneMapper};
use crate::d3d11::create_d3d_device_for_monitor;
use crate::dxgi_duplication_api::{DxgiDuplicationApi, DxgiDuplicationFormat, Error as DuplicationError};
use crate::monitor::Monitor as NativeMonitor;

const CAPTURE_FORMATS: [DxgiDuplicationFormat; 3] =
    [DxgiDuplicationFormat::Rgba16F, DxgiDuplicationFormat::Rgba8, DxgiDuplicationFormat::Bgra8];
const PERFORMANCE_SAMPLE_LIMIT: usize = 240;

/// A monitor exposed by [`Capture::monitors`].
///
/// Index zero describes the complete virtual desktop. Positive indexes correspond to the current
/// Windows `EnumDisplayMonitors` order and are valid inputs to [`Capture::grab`]. All coordinates
/// and dimensions are physical pixels.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Monitor {
    /// Capture selector: zero for the virtual desktop, otherwise a one-based physical monitor.
    pub index: usize,
    /// Physical-pixel monitor bounds in the virtual desktop.
    pub rect: Rect,
    /// Whether this selector represents the combined virtual desktop.
    pub is_virtual_desktop: bool,
    /// GDI display device name, or `"VIRTUAL_DESKTOP"` for selector zero.
    pub device_name: String,
    /// Human-readable target name, when Windows provides one.
    pub friendly_name: String,
    /// Whether Windows currently has Advanced Color enabled on this physical output.
    pub hdr_enabled: bool,
    /// Whether this physical output reports Advanced Color support.
    pub hdr_supported: bool,
}

/// Per-output provenance attached to a completed [`Frame`].
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FrameMonitorInfo {
    /// One-based physical monitor index.
    pub index: usize,
    /// Physical-pixel source rectangle in the virtual desktop.
    pub rect: Rect,
    /// Whether Advanced Color was enabled when this topology was inspected.
    pub hdr_enabled: bool,
    /// DXGI duplication source format, for example `"rgba16f"`.
    pub source_format: String,
    /// Windows DisplayConfig transport color encoding, if reported.
    pub source_color_space: Option<String>,
    /// The capture library's fixed output format, always `"bgra8"`.
    pub output_format: String,
}

/// A tightly packed sRGB `BGRA8` screenshot.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    /// Tightly packed sRGB pixels in blue, green, red, alpha byte order.
    pub bgra: Vec<u8>,
    /// Output width in physical pixels.
    pub width: u32,
    /// Output height in physical pixels.
    pub height: u32,
    /// Diagnostic provenance for every physical monitor that contributed to this frame.
    pub monitor_info: Vec<FrameMonitorInfo>,
}

impl Frame {
    /// Returns the tightly packed sRGB BGRA8 bytes without making a copy.
    #[must_use]
    pub fn bgra(&self) -> &[u8] {
        &self.bgra
    }

    /// Returns a newly allocated, tightly packed sRGB BGR8 image.
    ///
    /// Alpha stripping is intentionally lazy so users who pass BGRA directly to Pillow or OpenCV
    /// do not pay for a second full-image copy.
    #[must_use]
    pub fn bgr(&self) -> Vec<u8> {
        let mut bgr = Vec::with_capacity(self.bgra.len() / 4 * 3);
        for pixel in self.bgra.chunks_exact(4) {
            bgr.extend_from_slice(&pixel[..3]);
        }
        bgr
    }
}

/// Rolling timing information gathered by [`Capture`].
#[derive(Clone, Debug, Default)]
pub struct CaptureStats {
    /// Number of successfully returned frames since construction or the last [`Capture::reset_stats`].
    pub frames: u64,
    /// Total wall-clock time spent inside successful [`Capture::grab`] calls.
    pub total_grab_time: Duration,
    /// Duration of the first successful capture, if one has completed.
    pub first_frame_time: Option<Duration>,
    /// Mean end-to-end `grab` duration over the retained rolling samples.
    pub average_grab_time: Option<Duration>,
    /// P95 end-to-end `grab` duration over the retained rolling samples.
    pub p95_grab_time: Option<Duration>,
    /// Mean final GPU-to-CPU readback duration over the retained rolling samples.
    pub average_readback_time: Option<Duration>,
}

/// Failures returned by the synchronous HDR capture API.
#[derive(thiserror::Error, Debug)]
pub enum Error {
    /// Display topology or Advanced Color inspection failed.
    #[error("failed to inspect Windows display topology: {0}")]
    Display(#[from] display::Error),
    /// A monitor could not be resolved from its Windows enumeration index.
    #[error("failed to resolve Windows monitor: {0}")]
    Monitor(#[from] crate::monitor::Error),
    /// DXGI Desktop Duplication failed.
    #[error("DXGI Desktop Duplication failed: {0}")]
    Duplication(#[from] DuplicationError),
    /// D3D11 color conversion or composition failed.
    #[error("GPU HDR conversion failed: {0}")]
    Gpu(String),
    /// A selected monitor does not exist in the current topology.
    #[error("monitor index {index} is not present in the current topology")]
    InvalidMonitorIndex {
        /// The invalid capture selector supplied by the caller.
        index: usize,
    },
    /// No active physical displays were found.
    #[error("Windows reported no active displays")]
    NoDisplays,
    /// A virtual-desktop rectangle could not fit in an addressable D3D11 texture.
    #[error("virtual desktop dimensions are invalid or exceed D3D11 limits")]
    InvalidVirtualDesktop,
    /// The selected monitor has not delivered an initial desktop-duplication frame before timeout.
    #[error("monitor {index} did not deliver an initial frame within {timeout_ms} ms")]
    InitialFrameTimeout {
        /// One-based physical monitor index that did not produce a frame.
        index: usize,
        /// Timeout used for the attempted `AcquireNextFrame` call.
        timeout_ms: u32,
    },
    /// DXGI invalidated a session because display state changed.
    #[error("DXGI duplication access was lost")]
    AccessLost,
    /// A monitor's DXGI frame dimensions no longer match its display-topology rectangle.
    #[error(
        "monitor {index} changed dimensions from {expected_width}x{expected_height} to {actual_width}x{actual_height}"
    )]
    DimensionsChanged {
        /// One-based physical monitor index.
        index: usize,
        /// Width recorded when the session was created.
        expected_width: u32,
        /// Height recorded when the session was created.
        expected_height: u32,
        /// Width returned by the acquired DXGI frame.
        actual_width: u32,
        /// Height returned by the acquired DXGI frame.
        actual_height: u32,
    },
}

/// A persistent, synchronous HDR-aware capture service.
///
/// Construction establishes one DXGI Desktop Duplication session per physical monitor. Sessions
/// are retained across calls to [`Self::grab`]; the implementation polls display topology before
/// each capture and atomically rebuilds the sessions when HDR state, display geometry, or monitor
/// membership changes. On `DXGI_ERROR_ACCESS_LOST`, the failed capture is retried once after a
/// rebuild.
pub struct Capture {
    timeout_ms: u32,
    monitors: Vec<Monitor>,
    displays: Vec<MonitorDescriptor>,
    virtual_rect: Rect,
    shared_context: ID3D11DeviceContext,
    sessions: Vec<MonitorSession>,
    compositor: Option<GpuCompositor>,
    stats: CaptureStats,
    grab_samples: Vec<Duration>,
    readback_samples: Vec<Duration>,
}

struct MonitorSession {
    display: MonitorDescriptor,
    duplication: DxgiDuplicationApi,
    converter: GpuToneMapper,
    shares_primary_device: bool,
    source_format: DxgiDuplicationFormat,
    has_frame: bool,
    /// Frame dimensions Desktop Duplication actually delivers, i.e. the panel's native
    /// pre-rotation orientation. Differs from `display.rect` (post-rotation) whenever the monitor
    /// is rotated a quarter turn; comparing acquired frames against this instead of `display.rect`
    /// is what makes an ordinary rotated frame distinguishable from a real display-mode change.
    native_width: u32,
    native_height: u32,
}

impl MonitorSession {
    fn new(
        display: MonitorDescriptor,
        shared_device: &ID3D11Device,
        shared_context: &ID3D11DeviceContext,
    ) -> Result<Self, Error> {
        let monitor = NativeMonitor::from_index(display.index)?;
        let duplication = match DxgiDuplicationApi::new_with_device(
            monitor,
            shared_device.clone(),
            shared_context.clone(),
            &CAPTURE_FORMATS,
        ) {
            Ok(duplication) => (duplication, true),
            Err(DuplicationError::OutputNotFound) => {
                // This output is on another adapter. The individual session creates a device on
                // that adapter; capture remains correct, with CPU composition as the explicit
                // cross-GPU fallback.
                (DxgiDuplicationApi::new_options(monitor, &CAPTURE_FORMATS)?, false)
            }
            Err(error) => return Err(Error::Duplication(error)),
        };
        let source_format = duplication.0.format();
        // `IDXGIOutputDuplication::GetDesc().ModeDesc` reports the desktop-space (post-rotation)
        // mode, matching `display.rect` exactly. The frame texture `AcquireNextFrame` actually
        // hands back is in the panel's native (pre-rotation) orientation, so on a rotated monitor
        // this is swapped relative to the real per-frame dimensions. A failed rotation query (for
        // example on an indirect display driver) is treated as unrotated.
        let rotation =
            unsafe { duplication.0.output().GetDesc() }.map_or(DXGI_MODE_ROTATION_IDENTITY, |desc| desc.Rotation);
        let quarter_turn = rotation.0 == DXGI_MODE_ROTATION_ROTATE90.0 || rotation.0 == DXGI_MODE_ROTATION_ROTATE270.0;
        let (native_width, native_height) = if quarter_turn {
            (duplication.0.height(), duplication.0.width())
        } else {
            (duplication.0.width(), duplication.0.height())
        };
        let converter =
            GpuToneMapper::new(duplication.0.device(), native_width, native_height, display.sdr_white_level, rotation)?;

        Ok(Self {
            display,
            duplication: duplication.0,
            converter,
            shares_primary_device: duplication.1,
            source_format,
            has_frame: false,
            native_width,
            native_height,
        })
    }

    fn update(&mut self, timeout_ms: u32) -> Result<(), Error> {
        let deadline = Instant::now() + Duration::from_millis(u64::from(timeout_ms));

        loop {
            // A duplication session's very first `AcquireNextFrame` reliably returns a frame that
            // Windows has not yet filled with desktop content, and an idle desktop can return
            // further such frames afterwards. `LastPresentTime == 0` is how DXGI reports that the
            // desktop image carries no new content: the texture is blank before the first real
            // present and merely stale after it, so it must never be converted as if it were a
            // capture. Accepting it is what produced intermittently black first screenshots.
            match self.duplication.acquire_next_frame(remaining_ms(deadline)) {
                Ok(frame) if frame.frame_info().LastPresentTime == 0 => {
                    if self.has_frame {
                        // The retained converted frame is still the most recent desktop content.
                        return Ok(());
                    }
                }
                Ok(frame) => {
                    // Compared against the panel's native (pre-rotation) dimensions, not
                    // `display.rect`: a rotated monitor's frames never match `display.rect`
                    // directly, and that mismatch is expected on every frame, not a mode change.
                    if frame.width() != self.native_width || frame.height() != self.native_height {
                        return Err(Error::DimensionsChanged {
                            index: self.display.index,
                            expected_width: self.native_width,
                            expected_height: self.native_height,
                            actual_width: frame.width(),
                            actual_height: frame.height(),
                        });
                    }
                    self.source_format = frame.format();
                    self.converter.convert(frame.device_context(), frame.texture(), self.source_format)?;
                    self.has_frame = true;
                    return Ok(());
                }
                Err(DuplicationError::Timeout) if self.has_frame => return Ok(()),
                Err(DuplicationError::Timeout) => {
                    return Err(Error::InitialFrameTimeout { index: self.display.index, timeout_ms });
                }
                Err(DuplicationError::AccessLost) => return Err(Error::AccessLost),
                Err(error) => return Err(Error::Duplication(error)),
            }

            // Only a content-free frame reaches this point, and only before the first real one.
            if Instant::now() >= deadline {
                return Err(Error::InitialFrameTimeout { index: self.display.index, timeout_ms });
            }
        }
    }

    fn frame_info(&self) -> FrameMonitorInfo {
        FrameMonitorInfo {
            index: self.display.index,
            rect: self.display.rect,
            hdr_enabled: self.display.hdr_enabled,
            source_format: source_format_name(self.source_format).to_owned(),
            source_color_space: self.display.color_space.clone(),
            output_format: "bgra8".to_owned(),
        }
    }
}

impl Capture {
    /// Creates a capture service using a 100 ms wait budget for each physical output.
    ///
    /// A frame may be returned from the persistent cache when the desktop did not change during
    /// that interval. The first frame from every requested monitor still requires a present.
    pub fn new() -> Result<Self, Error> {
        Self::with_timeout(100)
    }

    /// Creates a capture service with a per-monitor DXGI wait budget in milliseconds.
    pub fn with_timeout(timeout_ms: u32) -> Result<Self, Error> {
        Self::from_displays(timeout_ms, display::enumerate_displays()?)
    }

    fn from_displays(timeout_ms: u32, displays: Vec<MonitorDescriptor>) -> Result<Self, Error> {
        let virtual_rect = virtual_rect(&displays)?;
        // Anchor the shared device on a real active output instead of relying on D3D11's default
        // adapter selection. This maximizes the number of outputs that can share the GPU-only
        // composition path on multi-adapter machines.
        let primary_monitor = NativeMonitor::from_index(displays.first().ok_or(Error::NoDisplays)?.index)?;
        let (shared_device, shared_context) =
            create_d3d_device_for_monitor(windows::Win32::Graphics::Gdi::HMONITOR(primary_monitor.as_raw_hmonitor()))
                .map_err(DuplicationError::DirectXError)?;
        let mut sessions = Vec::with_capacity(displays.len());
        for display in &displays {
            sessions.push(MonitorSession::new(display.clone(), &shared_device, &shared_context)?);
        }

        let all_share_primary_device = sessions.iter().all(|session| session.shares_primary_device);
        let compositor = all_share_primary_device
            .then(|| GpuCompositor::new(&shared_device, virtual_rect.width, virtual_rect.height))
            .transpose()?;
        let monitors = public_monitors(&displays, virtual_rect);

        Ok(Self {
            timeout_ms,
            monitors,
            displays,
            virtual_rect,
            shared_context,
            sessions,
            compositor,
            stats: CaptureStats::default(),
            grab_samples: Vec::new(),
            readback_samples: Vec::new(),
        })
    }

    /// Returns virtual-desktop selector zero followed by all physical monitor selectors.
    #[must_use]
    pub fn monitors(&self) -> &[Monitor] {
        &self.monitors
    }

    /// Returns a snapshot of the latest rolling capture timings.
    #[must_use]
    pub const fn stats(&self) -> &CaptureStats {
        &self.stats
    }

    /// Clears all rolling timing samples without disturbing DXGI sessions or cached frames.
    pub fn reset_stats(&mut self) {
        self.stats = CaptureStats::default();
        self.grab_samples.clear();
        self.readback_samples.clear();
    }

    /// Captures selector zero (the full virtual desktop) or a one-based physical monitor index.
    ///
    /// HDR FP16 source pixels are tone-mapped to sRGB BGRA8 on the GPU before readback. Same-GPU
    /// virtual desktops are composed in a single GPU texture and read back once. Mixed-adapter
    /// desktops keep tone mapping on each GPU and use a correct CPU composition fallback because
    /// D3D11 resources cannot be copied directly between independent adapters.
    pub fn grab(&mut self, monitor_index: usize) -> Result<Frame, Error> {
        self.grab_with_timeout(monitor_index, self.timeout_ms)
    }

    /// Captures a monitor with a one-call DXGI wait budget that overrides the configured default.
    ///
    /// This is useful for polling applications that normally keep a longer default timeout but
    /// occasionally need a non-blocking or low-latency capture attempt. A cached frame is still
    /// returned when no new desktop present arrives within the supplied interval.
    pub fn grab_with_timeout(&mut self, monitor_index: usize, timeout_ms: u32) -> Result<Frame, Error> {
        let start = Instant::now();
        self.refresh_topology()?;

        let result = self.grab_once(monitor_index, timeout_ms).or_else(|error| {
            if !matches!(error, Error::AccessLost | Error::DimensionsChanged { .. }) {
                return Err(error);
            }
            // A display mode change invalidates both the duplication object and often the HDR
            // format. Re-enumerate first, then retry exactly once so persistent failures surface.
            self.rebuild(display::enumerate_displays()?)?;
            self.grab_once(monitor_index, timeout_ms)
        });

        if let Ok(frame) = result {
            self.record_timing(start.elapsed(), Duration::ZERO);
            Ok(frame)
        } else {
            result
        }
    }

    fn refresh_topology(&mut self) -> Result<(), Error> {
        let current = display::enumerate_displays()?;
        if current != self.displays {
            self.rebuild(current)?;
        }
        Ok(())
    }

    fn rebuild(&mut self, displays: Vec<MonitorDescriptor>) -> Result<(), Error> {
        let stats = std::mem::take(&mut self.stats);
        let grab_samples = std::mem::take(&mut self.grab_samples);
        let readback_samples = std::mem::take(&mut self.readback_samples);
        // DXGI allows only one live IDXGIOutputDuplication per output, so the replacement sessions
        // must be built after the current ones are gone, not before: building `rebuilt` while
        // `self.sessions` is still alive fails DuplicateOutput1 with E_INVALIDARG for every output
        // that the old and new session sets have in common. If `from_displays` fails here, the next
        // `refresh_topology` retries from a clean slate rather than this half-torn-down one.
        self.sessions.clear();
        self.compositor = None;
        let mut rebuilt = Self::from_displays(self.timeout_ms, displays)?;
        rebuilt.stats = stats;
        rebuilt.grab_samples = grab_samples;
        rebuilt.readback_samples = readback_samples;
        *self = rebuilt;
        Ok(())
    }

    fn grab_once(&mut self, monitor_index: usize, timeout_ms: u32) -> Result<Frame, Error> {
        if monitor_index == 0 {
            self.grab_virtual_desktop(timeout_ms)
        } else {
            self.grab_monitor(monitor_index, timeout_ms)
        }
    }

    fn grab_monitor(&mut self, monitor_index: usize, timeout_ms: u32) -> Result<Frame, Error> {
        let readback_start = Instant::now();
        let (bgra, width, height, monitor_info) = {
            let session = self
                .sessions
                .iter_mut()
                .find(|session| session.display.index == monitor_index)
                .ok_or(Error::InvalidMonitorIndex { index: monitor_index })?;
            session.update(timeout_ms)?;
            (
                session.converter.readback(session.duplication.device_context())?,
                session.display.rect.width,
                session.display.rect.height,
                session.frame_info(),
            )
        };
        self.record_readback_timing(readback_start.elapsed());
        Ok(Frame { bgra, width, height, monitor_info: vec![monitor_info] })
    }

    fn grab_virtual_desktop(&mut self, timeout_ms: u32) -> Result<Frame, Error> {
        for session in &mut self.sessions {
            session.update(timeout_ms)?;
        }

        let readback_start = Instant::now();
        let bgra = if let Some(compositor) = &mut self.compositor {
            let mut sources: Vec<(&ID3D11Texture2D, u32, u32)> = Vec::with_capacity(self.sessions.len());
            for session in &self.sessions {
                sources.push((
                    session.converter.output(),
                    offset_from_virtual(session.display.rect.x, self.virtual_rect.x)?,
                    offset_from_virtual(session.display.rect.y, self.virtual_rect.y)?,
                ));
            }
            compositor.compose(&self.shared_context, &sources);
            compositor.readback(&self.shared_context)?
        } else {
            self.compose_cross_adapter_cpu()?
        };
        self.record_readback_timing(readback_start.elapsed());

        Ok(Frame {
            bgra,
            width: self.virtual_rect.width,
            height: self.virtual_rect.height,
            monitor_info: self.sessions.iter().map(MonitorSession::frame_info).collect(),
        })
    }

    fn compose_cross_adapter_cpu(&mut self) -> Result<Vec<u8>, Error> {
        let total_bytes = self
            .virtual_rect
            .width
            .checked_mul(self.virtual_rect.height)
            .and_then(|pixels| pixels.checked_mul(4))
            .and_then(|bytes| usize::try_from(bytes).ok())
            .ok_or(Error::InvalidVirtualDesktop)?;
        let mut destination = vec![0_u8; total_bytes];
        for pixel in destination.chunks_exact_mut(4) {
            pixel[3] = 255;
        }

        let virtual_width = self.virtual_rect.width as usize;
        for session in &mut self.sessions {
            let source = session.converter.readback(session.duplication.device_context())?;
            let source_width = session.display.rect.width as usize;
            let source_height = session.display.rect.height as usize;
            let destination_x = offset_from_virtual(session.display.rect.x, self.virtual_rect.x)? as usize;
            let destination_y = offset_from_virtual(session.display.rect.y, self.virtual_rect.y)? as usize;
            for row in 0..source_height {
                let source_start = row * source_width * 4;
                let destination_start = ((destination_y + row) * virtual_width + destination_x) * 4;
                destination[destination_start..destination_start + source_width * 4]
                    .copy_from_slice(&source[source_start..source_start + source_width * 4]);
            }
        }
        Ok(destination)
    }

    fn record_timing(&mut self, grab_time: Duration, readback_time: Duration) {
        self.stats.frames += 1;
        self.stats.total_grab_time += grab_time;
        if self.stats.first_frame_time.is_none() {
            self.stats.first_frame_time = Some(grab_time);
        }
        push_sample(&mut self.grab_samples, grab_time);
        if !readback_time.is_zero() {
            push_sample(&mut self.readback_samples, readback_time);
        }
        self.update_timing_stats();
    }

    fn record_readback_timing(&mut self, readback_time: Duration) {
        push_sample(&mut self.readback_samples, readback_time);
        self.update_timing_stats();
    }

    fn update_timing_stats(&mut self) {
        self.stats.average_grab_time = average_duration(&self.grab_samples);
        self.stats.p95_grab_time = percentile_95(&self.grab_samples);
        self.stats.average_readback_time = average_duration(&self.readback_samples);
    }
}

impl From<gpu::Error> for Error {
    fn from(error: gpu::Error) -> Self {
        Self::Gpu(error.to_string())
    }
}

fn public_monitors(displays: &[MonitorDescriptor], virtual_rect: Rect) -> Vec<Monitor> {
    let mut monitors = Vec::with_capacity(displays.len() + 1);
    monitors.push(Monitor {
        index: 0,
        rect: virtual_rect,
        is_virtual_desktop: true,
        device_name: "VIRTUAL_DESKTOP".to_owned(),
        friendly_name: "Virtual desktop".to_owned(),
        hdr_enabled: displays.iter().any(|display| display.hdr_enabled),
        hdr_supported: displays.iter().any(|display| display.hdr_supported),
    });
    monitors.extend(displays.iter().map(|display| Monitor {
        index: display.index,
        rect: display.rect,
        is_virtual_desktop: false,
        device_name: display.device_name.clone(),
        friendly_name: display.friendly_name.clone(),
        hdr_enabled: display.hdr_enabled,
        hdr_supported: display.hdr_supported,
    }));
    monitors
}

/// Milliseconds left in a wait budget, saturating at zero.
///
/// A zero result still lets `AcquireNextFrame` report whether a frame is already queued, so an
/// exhausted budget polls once rather than blocking.
fn remaining_ms(deadline: Instant) -> u32 {
    u32::try_from(deadline.saturating_duration_since(Instant::now()).as_millis()).unwrap_or(u32::MAX)
}

fn virtual_rect(displays: &[MonitorDescriptor]) -> Result<Rect, Error> {
    let first = displays.first().ok_or(Error::NoDisplays)?;
    let mut left = i64::from(first.rect.x);
    let mut top = i64::from(first.rect.y);
    let mut right = first.rect.right();
    let mut bottom = first.rect.bottom();
    for display in &displays[1..] {
        left = left.min(i64::from(display.rect.x));
        top = top.min(i64::from(display.rect.y));
        right = right.max(display.rect.right());
        bottom = bottom.max(display.rect.bottom());
    }
    let width = right.checked_sub(left).ok_or(Error::InvalidVirtualDesktop)?;
    let height = bottom.checked_sub(top).ok_or(Error::InvalidVirtualDesktop)?;
    if width <= 0
        || height <= 0
        || width > i64::from(u32::MAX)
        || height > i64::from(u32::MAX)
        || left < i64::from(i32::MIN)
        || left > i64::from(i32::MAX)
        || top < i64::from(i32::MIN)
        || top > i64::from(i32::MAX)
    {
        return Err(Error::InvalidVirtualDesktop);
    }
    Ok(Rect::new(left as i32, top as i32, width as u32, height as u32))
}

fn offset_from_virtual(position: i32, virtual_origin: i32) -> Result<u32, Error> {
    let offset = i64::from(position) - i64::from(virtual_origin);
    u32::try_from(offset).map_err(|_| Error::InvalidVirtualDesktop)
}

const fn source_format_name(format: DxgiDuplicationFormat) -> &'static str {
    match format {
        DxgiDuplicationFormat::Rgba16F => "rgba16f",
        DxgiDuplicationFormat::Rgba8 => "rgba8",
        DxgiDuplicationFormat::Bgra8 => "bgra8",
    }
}

fn push_sample(samples: &mut Vec<Duration>, value: Duration) {
    if samples.len() == PERFORMANCE_SAMPLE_LIMIT {
        samples.remove(0);
    }
    samples.push(value);
}

fn average_duration(samples: &[Duration]) -> Option<Duration> {
    let total = samples.iter().map(Duration::as_nanos).sum::<u128>();
    (!samples.is_empty())
        .then(|| Duration::from_nanos((total / samples.len() as u128).min(u128::from(u64::MAX)) as u64))
}

fn percentile_95(samples: &[Duration]) -> Option<Duration> {
    if samples.is_empty() {
        return None;
    }
    let mut sorted = samples.to_vec();
    sorted.sort_unstable();
    let index = (sorted.len() * 95).div_ceil(100).saturating_sub(1);
    sorted.get(index).copied()
}

#[cfg(test)]
mod tests {
    use super::{average_duration, offset_from_virtual, percentile_95, virtual_rect};
    use crate::hdr_capture::display::{MonitorDescriptor, Rect};
    use std::time::Duration;

    fn display(index: usize, rect: Rect) -> MonitorDescriptor {
        MonitorDescriptor {
            index,
            device_name: format!("\\\\.\\DISPLAY{index}"),
            friendly_name: format!("Display {index}"),
            rect,
            hdr_enabled: false,
            hdr_supported: false,
            bits_per_color: 8,
            color_space: Some("RGB".to_owned()),
            dxgi_output_color_space: Some("RGB_FULL_G22_NONE_P709".to_owned()),
            dxgi_bits_per_color: Some(8),
            sdr_white_level: 1.0,
        }
    }

    #[test]
    fn virtual_desktop_preserves_negative_coordinates_and_gaps() {
        let rect = virtual_rect(&[
            display(1, Rect::new(0, 0, 1_920, 1_080)),
            display(2, Rect::new(-1_280, -720, 1_280, 720)),
            display(3, Rect::new(1_920, 300, 1_024, 768)),
        ])
        .expect("valid layout");
        assert_eq!(rect, Rect::new(-1_280, -720, 4_224, 1_800));
        assert_eq!(offset_from_virtual(0, rect.x).unwrap(), 1_280);
        assert_eq!(offset_from_virtual(-720, rect.y).unwrap(), 0);
    }

    #[test]
    fn rolling_metrics_compute_mean_and_p95() {
        let samples = [1_u64, 2, 3, 4, 100].map(Duration::from_millis);
        assert_eq!(average_duration(&samples), Some(Duration::from_millis(22)));
        assert_eq!(percentile_95(&samples), Some(Duration::from_millis(100)));
    }
}

#[cfg(test)]
mod virtual_desktop_simulation {
    //! Exercises multi-monitor composition on a machine that has only one display.
    //!
    //! Windows reports only physically attached monitors, so the virtual-desktop path cannot be
    //! covered by capturing this machine. These tests substitute synthetic per-monitor textures
    //! for the DXGI frames and then run the real code: the same `GpuToneMapper` the capture path
    //! uses per output, the same `virtual_rect` / `offset_from_virtual` geometry, and the same
    //! `GpuCompositor`. Only the duplication source is simulated.

    use std::time::{Duration, Instant};

    use windows::Win32::Graphics::Direct3D11::{
        D3D11_BIND_SHADER_RESOURCE, D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT, ID3D11Device,
        ID3D11Texture2D,
    };
    use windows::Win32::Graphics::Dxgi::Common::{
        DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_MODE_ROTATION_IDENTITY,
        DXGI_SAMPLE_DESC,
    };

    use super::gpu::{GpuCompositor, GpuToneMapper};
    use super::{offset_from_virtual, virtual_rect};
    use crate::d3d11::create_d3d_device;
    use crate::dxgi_duplication_api::DxgiDuplicationFormat;
    use crate::hdr_capture::display::{MonitorDescriptor, Rect};

    /// A simulated monitor: where it sits on the virtual desktop and what it is showing.
    struct SimulatedMonitor {
        rect: Rect,
        hdr: bool,
        /// Where this monitor's desktop white sits on the scRGB scale, as Windows would report it.
        sdr_white_level: f32,
        /// The color the whole monitor displays: scRGB when HDR, otherwise 0-255 RGB.
        fill: [f32; 3],
    }

    fn descriptor(index: usize, monitor: &SimulatedMonitor) -> MonitorDescriptor {
        MonitorDescriptor {
            index,
            device_name: format!("DISPLAY{index}"),
            friendly_name: format!("Simulated {index}"),
            rect: monitor.rect,
            hdr_enabled: monitor.hdr,
            hdr_supported: monitor.hdr,
            bits_per_color: if monitor.hdr { 10 } else { 8 },
            color_space: Some("RGB".to_owned()),
            dxgi_output_color_space: None,
            dxgi_bits_per_color: None,
            sdr_white_level: monitor.sdr_white_level,
        }
    }

    /// Truncating `f32` to binary16, which is exact for the powers of two these tests use.
    fn f16_bits(value: f32) -> u16 {
        let bits = value.to_bits();
        let sign = ((bits >> 16) & 0x8000) as u16;
        let exponent = ((bits >> 23) & 0xff) as i32 - 127 + 15;
        if exponent <= 0 {
            return sign;
        }
        if exponent >= 0x1f {
            return sign | 0x7c00;
        }
        sign | ((exponent as u16) << 10) | ((bits >> 13) & 0x03ff) as u16
    }

    fn texture(device: &ID3D11Device, width: u32, height: u32, format: DXGI_FORMAT, row: &[u8]) -> ID3D11Texture2D {
        let pixels: Vec<u8> = row.repeat(width as usize * height as usize);
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_SHADER_RESOURCE.0 as u32,
            ..Default::default()
        };
        let data = D3D11_SUBRESOURCE_DATA {
            pSysMem: pixels.as_ptr().cast(),
            SysMemPitch: width * row.len() as u32,
            ..Default::default()
        };
        let mut created = None;
        // SAFETY: `pixels` covers the whole surface described above and outlives this call.
        unsafe { device.CreateTexture2D(&desc, Some(&data), Some(&mut created)) }.expect("create source texture");
        created.expect("D3D11 returned no texture")
    }

    fn source_texture(device: &ID3D11Device, monitor: &SimulatedMonitor) -> (ID3D11Texture2D, DxgiDuplicationFormat) {
        let width = monitor.rect.width;
        let height = monitor.rect.height;
        if monitor.hdr {
            let mut row = Vec::new();
            for channel in monitor.fill {
                row.extend_from_slice(&f16_bits(channel).to_le_bytes());
            }
            row.extend_from_slice(&f16_bits(1.0).to_le_bytes());
            (texture(device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, &row), DxgiDuplicationFormat::Rgba16F)
        } else {
            let row = [monitor.fill[2] as u8, monitor.fill[1] as u8, monitor.fill[0] as u8, 255];
            (texture(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, &row), DxgiDuplicationFormat::Bgra8)
        }
    }

    /// Runs the real per-monitor conversion and virtual-desktop composition.
    fn compose(monitors: &[SimulatedMonitor]) -> (Vec<u8>, Rect) {
        let (device, context) = create_d3d_device().expect("this crate requires a D3D11 device");
        let displays: Vec<_> = monitors.iter().enumerate().map(|(i, m)| descriptor(i + 1, m)).collect();
        let desktop = virtual_rect(&displays).expect("a valid simulated layout");

        let mut converters = Vec::new();
        for monitor in monitors {
            let (source, format) = source_texture(&device, monitor);
            let converter =
                GpuToneMapper::new(
                    &device,
                    monitor.rect.width,
                    monitor.rect.height,
                    monitor.sdr_white_level,
                    DXGI_MODE_ROTATION_IDENTITY,
                )
                    .expect("tone mapper");
            converter.convert(&context, &source, format).expect("per-monitor conversion");
            converters.push(converter);
        }

        let mut compositor = GpuCompositor::new(&device, desktop.width, desktop.height).expect("compositor");
        let placements: Vec<_> = converters
            .iter()
            .zip(monitors)
            .map(|(converter, monitor)| {
                let left = offset_from_virtual(monitor.rect.x, desktop.x).expect("in-range x offset");
                let top = offset_from_virtual(monitor.rect.y, desktop.y).expect("in-range y offset");
                (converter.output(), left, top)
            })
            .collect();
        compositor.compose(&context, &placements);
        (compositor.readback(&context).expect("virtual desktop readback"), desktop)
    }

    fn pixel(frame: &[u8], desktop: Rect, x: u32, y: u32) -> [u8; 4] {
        let offset = (y as usize * desktop.width as usize + x as usize) * 4;
        frame[offset..offset + 4].try_into().expect("four channels")
    }

    /// Prints a coarse map of the composed desktop so `cargo test -- --nocapture` shows the layout.
    ///
    /// Each cell samples one pixel: `.` is cleared background, and every distinct color present
    /// gets its own digit, so a misplaced or dropped monitor is visible at a glance.
    fn print_map(frame: &[u8], desktop: Rect, label: &str) {
        const COLUMNS: u32 = 48;
        const ROWS: u32 = 18;

        let mut legend: Vec<[u8; 4]> = Vec::new();
        let mut rendered = String::new();
        for row in 0..ROWS {
            for column in 0..COLUMNS {
                let x = column * desktop.width / COLUMNS;
                let y = row * desktop.height / ROWS;
                let sample = pixel(frame, desktop, x, y);
                if sample == [0, 0, 0, 255] {
                    rendered.push('.');
                    continue;
                }
                let index = legend.iter().position(|known| *known == sample).unwrap_or_else(|| {
                    legend.push(sample);
                    legend.len() - 1
                });
                rendered.push(char::from_digit(index as u32 + 1, 10).unwrap_or('?'));
            }
            rendered.push('\n');
        }

        println!();
        println!("{label}: virtual desktop {}x{} at ({}, {})", desktop.width, desktop.height, desktop.x, desktop.y);
        print!("{rendered}");
        for (index, color) in legend.iter().enumerate() {
            println!("  {} = BGRA {:?}", index + 1, color);
        }
        println!("  . = cleared background");
    }

    fn assert_near(actual: [u8; 4], expected: [u8; 3], label: &str) {
        for (channel, (got, want)) in actual.iter().zip(expected).enumerate() {
            let difference = i32::from(*got) - i32::from(want);
            assert!(
                difference.abs() <= 1,
                "{label}: channel {channel} was {got}, expected {want} (GPU and CPU encoders may differ by one)"
            );
        }
    }

    #[test]
    fn mixed_hdr_and_sdr_monitors_compose_side_by_side() {
        // An HDR monitor whose desktop white sits at scRGB 4.0, beside a smaller SDR monitor
        // pushed downwards so the bounding box also contains a gap.
        let monitors = [
            SimulatedMonitor {
                rect: Rect::new(0, 0, 640, 480),
                hdr: true,
                sdr_white_level: 4.0,
                fill: [4.0, 4.0, 4.0],
            },
            SimulatedMonitor {
                rect: Rect::new(640, 120, 320, 240),
                hdr: false,
                sdr_white_level: 1.0,
                fill: [255.0, 255.0, 255.0],
            },
        ];
        let (frame, desktop) = compose(&monitors);
        print_map(&frame, desktop, "HDR left, SDR right with a gap");

        assert_eq!((desktop.x, desktop.y), (0, 0));
        assert_eq!((desktop.width, desktop.height), (960, 480));
        assert_eq!(frame.len(), 960 * 480 * 4);

        // The HDR monitor's desktop white must land on the same 251 the CPU reference produces.
        // Raw scRGB 4.0 would clip to 255, so this only passes if the white level reached the
        // shader and was applied per monitor.
        assert_near(pixel(&frame, desktop, 320, 240), [251, 251, 251], "HDR monitor white");

        // The SDR monitor beside it is copied through untouched, so its white stays exactly 255.
        assert_eq!(pixel(&frame, desktop, 800, 240), [255, 255, 255, 255], "SDR monitor white");

        // The gap above the shorter, lower monitor is cleared rather than left undefined.
        assert_eq!(pixel(&frame, desktop, 800, 40), [0, 0, 0, 255], "gap above the SDR monitor");
    }

    #[test]
    fn negative_coordinates_place_monitors_at_the_correct_offsets() {
        // A monitor above and to the left of the primary: the layout that makes naive
        // implementations write outside the target or drop a display.
        let monitors = [
            SimulatedMonitor {
                rect: Rect::new(0, 0, 640, 480),
                hdr: false,
                sdr_white_level: 1.0,
                fill: [255.0, 0.0, 0.0],
            },
            SimulatedMonitor {
                rect: Rect::new(-320, -240, 320, 240),
                hdr: false,
                sdr_white_level: 1.0,
                fill: [0.0, 255.0, 0.0],
            },
        ];
        let (frame, desktop) = compose(&monitors);
        print_map(&frame, desktop, "secondary above-left of the primary");

        assert_eq!((desktop.x, desktop.y), (-320, -240));
        assert_eq!((desktop.width, desktop.height), (960, 720));

        // The secondary lands at the origin of the virtual desktop, the primary below-right of it.
        assert_eq!(pixel(&frame, desktop, 160, 120), [0, 255, 0, 255], "monitor at negative coordinates");
        assert_eq!(pixel(&frame, desktop, 640, 480), [0, 0, 255, 255], "primary monitor");

        // The two rectangles do not tile the bounding box, and the remainder must be cleared.
        assert_eq!(pixel(&frame, desktop, 160, 480), [0, 0, 0, 255], "region below the secondary");
    }

    #[test]
    fn virtual_desktop_composition_bench_at_realistic_sizes() {
        // Not a correctness test: measures the steady-state cost of per-monitor tone mapping plus
        // virtual-desktop composition and single readback, using synthetic source textures so it
        // runs on a machine with only one physical display. Run with `--release --nocapture` to
        // see timings; debug-mode numbers are not representative of the real capture path.
        bench_layout(
            "dual 4K HDR side by side",
            &[
                SimulatedMonitor { rect: Rect::new(0, 0, 3_840, 2_160), hdr: true, sdr_white_level: 3.8, fill: [3.8; 3] },
                SimulatedMonitor {
                    rect: Rect::new(3_840, 0, 3_840, 2_160),
                    hdr: true,
                    sdr_white_level: 3.8,
                    fill: [3.8; 3],
                },
            ],
        );

        bench_layout(
            "triple mixed HDR/SDR, negative offset and gap",
            &[
                SimulatedMonitor { rect: Rect::new(0, 0, 3_840, 2_160), hdr: true, sdr_white_level: 3.8, fill: [3.8; 3] },
                SimulatedMonitor {
                    rect: Rect::new(-1_920, 300, 1_920, 1_080),
                    hdr: false,
                    sdr_white_level: 1.0,
                    fill: [255.0; 3],
                },
                SimulatedMonitor {
                    rect: Rect::new(3_840, -440, 2_560, 1_440),
                    hdr: true,
                    sdr_white_level: 4.5,
                    fill: [4.5; 3],
                },
            ],
        );

        bench_layout(
            "quad 1080p 2x2 grid, alternating HDR/SDR",
            &[
                SimulatedMonitor { rect: Rect::new(0, 0, 1_920, 1_080), hdr: true, sdr_white_level: 3.8, fill: [3.8; 3] },
                SimulatedMonitor {
                    rect: Rect::new(1_920, 0, 1_920, 1_080),
                    hdr: false,
                    sdr_white_level: 1.0,
                    fill: [200.0; 3],
                },
                SimulatedMonitor {
                    rect: Rect::new(0, 1_080, 1_920, 1_080),
                    hdr: false,
                    sdr_white_level: 1.0,
                    fill: [200.0; 3],
                },
                SimulatedMonitor {
                    rect: Rect::new(1_920, 1_080, 1_920, 1_080),
                    hdr: true,
                    sdr_white_level: 3.8,
                    fill: [3.8; 3],
                },
            ],
        );
    }

    /// Times steady-state `convert()` + `compose()` + `readback()` for a fixed synthetic layout.
    ///
    /// Source textures and converters are created once, the way the real capture path reuses its
    /// session across frames; only the per-frame GPU work is timed.
    fn bench_layout(label: &str, monitors: &[SimulatedMonitor]) {
        let (device, context) = create_d3d_device().expect("this crate requires a D3D11 device");
        let displays: Vec<_> = monitors.iter().enumerate().map(|(i, m)| descriptor(i + 1, m)).collect();
        let desktop = virtual_rect(&displays).expect("a valid simulated layout");

        let mut converters = Vec::new();
        let mut sources = Vec::new();
        for monitor in monitors {
            let (source, format) = source_texture(&device, monitor);
            let converter =
                GpuToneMapper::new(
                    &device,
                    monitor.rect.width,
                    monitor.rect.height,
                    monitor.sdr_white_level,
                    DXGI_MODE_ROTATION_IDENTITY,
                )
                    .expect("tone mapper");
            sources.push((source, format));
            converters.push(converter);
        }
        let mut compositor = GpuCompositor::new(&device, desktop.width, desktop.height).expect("compositor");
        let placements: Vec<_> = converters
            .iter()
            .zip(monitors)
            .map(|(converter, monitor)| {
                let left = offset_from_virtual(monitor.rect.x, desktop.x).expect("in-range x offset");
                let top = offset_from_virtual(monitor.rect.y, desktop.y).expect("in-range y offset");
                (converter.output(), left, top)
            })
            .collect();

        // Warm up: the first GPU dispatch on a device pays driver/shader compilation costs that a
        // long-lived capture session would already have absorbed before this loop matters.
        for (converter, (source, format)) in converters.iter().zip(&sources) {
            converter.convert(&context, source, *format).expect("warmup conversion");
        }
        compositor.compose(&context, &placements);
        compositor.readback(&context).expect("warmup readback");

        const ITERATIONS: usize = 200;
        let mut samples = Vec::with_capacity(ITERATIONS);
        for _ in 0..ITERATIONS {
            let start = Instant::now();
            for (converter, (source, format)) in converters.iter().zip(&sources) {
                converter.convert(&context, source, *format).expect("conversion");
            }
            compositor.compose(&context, &placements);
            compositor.readback(&context).expect("readback");
            samples.push(start.elapsed());
        }

        samples.sort();
        let total: Duration = samples.iter().sum();
        let average = total / ITERATIONS as u32;
        let p95 = samples[(ITERATIONS * 95 / 100).min(samples.len() - 1)];
        let min = samples[0];
        let max = samples[samples.len() - 1];
        println!(
            "{label}: {} monitor(s), desktop {}x{} at ({}, {}) -- avg={average:?} p95={p95:?} min={min:?} max={max:?}",
            monitors.len(),
            desktop.width,
            desktop.height,
            desktop.x,
            desktop.y
        );
    }
}
