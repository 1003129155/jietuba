//! Display-topology and Advanced Color diagnostics.
//!
//! This module deliberately joins the two Windows views of a display: GDI supplies the
//! physical-pixel desktop rectangle, while the Display Configuration API supplies the target
//! used to query Advanced Color state.  A GDI source can drive more than one target in a cloned
//! topology, so paths are matched by their source GDI device name rather than by enumeration
//! order.

use std::mem::size_of;

use windows::Win32::Devices::Display::{
    DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
    DISPLAYCONFIG_DEVICE_INFO_HEADER, DISPLAYCONFIG_DEVICE_INFO_TYPE, DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO,
    DISPLAYCONFIG_MODE_INFO, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED,
    DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL, DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED, DISPLAYCONFIG_PATH_INFO,
    DISPLAYCONFIG_SDR_WHITE_LEVEL, DISPLAYCONFIG_SOURCE_DEVICE_NAME, DISPLAYCONFIG_TARGET_DEVICE_NAME,
    DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes, QDC_ONLY_ACTIVE_PATHS, QueryDisplayConfig,
};
use windows::Win32::Foundation::{
    E_ACCESSDENIED, ERROR_INSUFFICIENT_BUFFER, ERROR_SUCCESS, LPARAM, LUID, RECT as Win32Rect, TRUE,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020,
    DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709, DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020,
    DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020, DXGI_COLOR_SPACE_TYPE, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
    DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020, DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
};
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, DXGI_ERROR_NOT_FOUND, IDXGIFactory1, IDXGIOutput6};
use windows::Win32::Graphics::Gdi::{
    DISPLAYCONFIG_COLOR_ENCODING, DISPLAYCONFIG_COLOR_ENCODING_INTENSITY, DISPLAYCONFIG_COLOR_ENCODING_RGB,
    DISPLAYCONFIG_COLOR_ENCODING_YCBCR420, DISPLAYCONFIG_COLOR_ENCODING_YCBCR422,
    DISPLAYCONFIG_COLOR_ENCODING_YCBCR444, EnumDisplayMonitors, GetMonitorInfoW, HDC, HMONITOR, MONITORINFOEXW,
};
use windows::Win32::UI::HiDpi::{
    DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, DPI_AWARENESS_PER_MONITOR_AWARE, GetAwarenessFromDpiAwarenessContext,
    GetThreadDpiAwarenessContext, SetProcessDpiAwarenessContext,
};
use windows::core::{BOOL, Interface};

/// A physical-pixel rectangle in the Windows virtual desktop.
///
/// `x` and `y` may be negative when a display is left of or above the primary display.  Width
/// and height are non-negative physical-pixel dimensions, unaffected by DPI virtualization.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
pub struct Rect {
    /// The physical-pixel x coordinate of the rectangle's left edge.
    pub x: i32,
    /// The physical-pixel y coordinate of the rectangle's top edge.
    pub y: i32,
    /// The rectangle width in physical pixels.
    pub width: u32,
    /// The rectangle height in physical pixels.
    pub height: u32,
}

impl Rect {
    /// Creates a physical-pixel rectangle.
    #[must_use]
    pub const fn new(x: i32, y: i32, width: u32, height: u32) -> Self {
        Self { x, y, width, height }
    }

    /// Returns the physical-pixel coordinate immediately after the rectangle's right edge.
    ///
    /// The return type is wider than `i32` so this method cannot overflow for a valid [`Rect`].
    #[must_use]
    pub const fn right(self) -> i64 {
        self.x as i64 + self.width as i64
    }

    /// Returns the physical-pixel coordinate immediately after the rectangle's bottom edge.
    ///
    /// The return type is wider than `i32` so this method cannot overflow for a valid [`Rect`].
    #[must_use]
    pub const fn bottom(self) -> i64 {
        self.y as i64 + self.height as i64
    }
}

/// A display monitor and its current Windows Advanced Color diagnostics.
// `sdr_white_level` is a measured float, so this type is deliberately only `PartialEq`.
#[derive(Clone, Debug, PartialEq)]
pub struct MonitorDescriptor {
    /// One-based position of this monitor in the current `EnumDisplayMonitors` enumeration.
    pub index: usize,
    /// GDI source device name, normally of the form `\\.\DISPLAY1`.
    pub device_name: String,
    /// EDID-derived target name, or [`Self::device_name`] when Windows does not provide one.
    pub friendly_name: String,
    /// Physical-pixel monitor bounds in the virtual desktop.
    pub rect: Rect,
    /// Whether Windows currently has Advanced Color enabled for the selected target.
    pub hdr_enabled: bool,
    /// Whether the selected target reports Advanced Color support.
    pub hdr_supported: bool,
    /// Bits per color channel currently reported by the target.
    pub bits_per_color: u32,
    /// The target's DisplayConfig color encoding, such as `"RGB"` or `"YCbCr444"`.
    ///
    /// This is the transport encoding reported by Windows, rather than an ICC profile or a full
    /// transfer-function/color-gamut description. It is `None` for an unrecognized encoding.
    pub color_space: Option<String>,
    /// DXGI's actual output color-space token, when `IDXGIOutput6::GetDesc1` is available.
    ///
    /// Values use DXGI names such as `"RGB_FULL_G22_NONE_P709"` or
    /// `"RGB_FULL_G2084_NONE_P2020"`; unfamiliar values retain their numeric DXGI token.
    pub dxgi_output_color_space: Option<String>,
    /// Per-channel bit depth reported by DXGI output metadata, when available.
    pub dxgi_bits_per_color: Option<u32>,
    /// Where this target places SDR reference white on the scRGB scale used by HDR capture.
    ///
    /// Desktop Duplication hands HDR desktops out as scRGB, where `1.0` is the fixed 80-nit
    /// reference white rather than the white the user actually sees. Windows scales SDR content
    /// by the "SDR content brightness" slider, so ordinary desktop white commonly lands at
    /// several times `1.0`. Dividing captured samples by this value restores a machine-independent
    /// signal in which `1.0` means "the white of this desktop".
    ///
    /// It is `1.0` when Windows reports no scaling, and for SDR targets, so it is always a safe
    /// divisor.
    pub sdr_white_level: f32,
}

/// Failures that can occur while reading display topology or Advanced Color state.
#[derive(Debug, thiserror::Error)]
pub enum Error {
    /// The process could not opt into physical-pixel, per-monitor DPI awareness.
    #[error("SetProcessDpiAwarenessContext failed: {0}")]
    DpiAwareness(#[source] windows::core::Error),
    /// The embedding application fixed the process to a non-per-monitor DPI context.
    #[error("the hosting process is not per-monitor DPI aware, so physical-pixel capture coordinates are unsafe")]
    IncompatibleDpiAwareness,
    /// DXGI output diagnostics could not be enumerated.
    #[error("DXGI output diagnostics failed: {0}")]
    Dxgi(#[from] windows::core::Error),
    /// `EnumDisplayMonitors` failed.
    #[error("EnumDisplayMonitors failed: {0}")]
    EnumDisplayMonitors(#[source] windows::core::Error),
    /// `GetMonitorInfoW` failed for an enumerated monitor.
    #[error("GetMonitorInfoW failed: {0}")]
    GetMonitorInfo(#[source] windows::core::Error),
    /// A GDI monitor rectangle did not describe a non-negative `u32` size.
    #[error("invalid monitor rectangle: ({left}, {top})..({right}, {bottom})")]
    InvalidMonitorRectangle {
        /// Left coordinate returned by GDI.
        left: i32,
        /// Top coordinate returned by GDI.
        top: i32,
        /// Right coordinate returned by GDI.
        right: i32,
        /// Bottom coordinate returned by GDI.
        bottom: i32,
    },
    /// No active Display Configuration path matched an enumerated GDI device name.
    ///
    /// This usually means that the topology changed while it was being read; retrying the
    /// enumeration obtains a fresh snapshot.
    #[error("no active DisplayConfig path matches {device_name}")]
    ActivePathNotFound {
        /// GDI source device name that could not be mapped to a target path.
        device_name: String,
    },
    /// A Display Configuration topology query failed.
    #[error("{operation} failed with Win32 error {code}")]
    DisplayConfig {
        /// Name of the failed topology API call.
        operation: &'static str,
        /// Win32 error code returned by the API.
        code: u32,
    },
    /// A `DisplayConfigGetDeviceInfo` query failed.
    #[error("{operation} failed with Win32 error {code}")]
    DisplayConfigDeviceInfo {
        /// Kind of device-information request that failed.
        operation: &'static str,
        /// Win32 error code returned by the API.
        code: i32,
    },
}

/// Enumerates active displays with physical-pixel bounds and Advanced Color diagnostics.
///
/// GDI supplies the monitor list and rectangles. Each monitor's GDI source device name is then
/// matched to a `QDC_ONLY_ACTIVE_PATHS` path before target-specific data is queried. For a cloned
/// topology, an `HMONITOR` can represent several targets. In that case this function follows the
/// Windows path-selection guidance: it prefers an internal target when present, otherwise it uses
/// the first matching path, which is the highest-priority clone path.
///
/// # Errors
///
/// Returns an [`Error`] when Windows cannot enumerate monitors, cannot obtain a coherent active
/// Display Configuration path, or rejects an Advanced Color query.
pub fn enumerate_displays() -> Result<Vec<MonitorDescriptor>, Error> {
    enable_per_monitor_dpi_awareness()?;
    let monitors = enumerate_monitor_handles()?;
    let paths = resolve_active_paths(query_active_paths()?)?;
    let mut displays = Vec::with_capacity(monitors.len());

    for (zero_based_index, monitor) in monitors.into_iter().enumerate() {
        let monitor_info = get_monitor_info(monitor)?;
        let device_name = wide_string(&monitor_info.szDevice);
        let rect = rect_from_win32(&monitor_info.monitorInfo.rcMonitor)?;
        let path = select_path_for_device(&paths, &device_name)
            .ok_or_else(|| Error::ActivePathNotFound { device_name: device_name.clone() })?;

        let mut friendly_name = target_friendly_name(path)?;
        if friendly_name.is_empty() {
            friendly_name.clone_from(&device_name);
        }

        let advanced_color = advanced_color_info(path)?;
        let dxgi = dxgi_output_info(monitor)?;
        displays.push(MonitorDescriptor {
            index: zero_based_index + 1,
            device_name,
            friendly_name,
            rect,
            hdr_enabled: advanced_color.enabled,
            hdr_supported: advanced_color.supported,
            bits_per_color: advanced_color.bits_per_color,
            color_space: color_encoding_name(advanced_color.encoding).map(str::to_owned),
            dxgi_output_color_space: dxgi.as_ref().map(|value| value.color_space.clone()),
            dxgi_bits_per_color: dxgi.map(|value| value.bits_per_color),
            sdr_white_level: sdr_white_level(path)?,
        });
    }

    Ok(displays)
}

fn enable_per_monitor_dpi_awareness() -> Result<(), Error> {
    match unsafe { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) } {
        Ok(()) => Ok(()),
        // A host application may have chosen its process-wide DPI context before loading this
        // library. It is safe to continue only when its current thread is already per-monitor
        // aware; accepting a system/unaware context would silently virtualize GDI coordinates.
        Err(error) if error.code() == E_ACCESSDENIED => {
            let awareness = unsafe { GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()) };
            if awareness == DPI_AWARENESS_PER_MONITOR_AWARE { Ok(()) } else { Err(Error::IncompatibleDpiAwareness) }
        }
        Err(error) => Err(Error::DpiAwareness(error)),
    }
}

#[derive(Clone)]
struct ResolvedPath {
    path: DISPLAYCONFIG_PATH_INFO,
    source_device_name: String,
}

#[derive(Clone, Copy)]
struct AdvancedColorInfo {
    supported: bool,
    enabled: bool,
    encoding: DISPLAYCONFIG_COLOR_ENCODING,
    bits_per_color: u32,
}

struct DxgiOutputInfo {
    color_space: String,
    bits_per_color: u32,
}

fn enumerate_monitor_handles() -> Result<Vec<HMONITOR>, Error> {
    let mut monitors = Vec::new();
    // SAFETY: The callback receives the pointer to `monitors` only for the duration of this
    // synchronous call. It casts the same pointer back to its original type before using it.
    unsafe {
        EnumDisplayMonitors(None, None, Some(collect_monitor), LPARAM((&raw mut monitors).cast::<()>() as isize))
    }
    .ok()
    .map_err(Error::EnumDisplayMonitors)?;

    Ok(monitors)
}

// `EnumDisplayMonitors` invokes this callback synchronously while `monitors` is alive.
unsafe extern "system" fn collect_monitor(monitor: HMONITOR, _: HDC, _: *mut Win32Rect, data: LPARAM) -> BOOL {
    // SAFETY: `data` is created from a valid `*mut Vec<HMONITOR>` in `enumerate_monitor_handles`.
    let monitors = unsafe { &mut *(data.0 as *mut Vec<HMONITOR>) };
    monitors.push(monitor);
    TRUE
}

fn get_monitor_info(monitor: HMONITOR) -> Result<MONITORINFOEXW, Error> {
    let mut monitor_info = MONITORINFOEXW::default();
    monitor_info.monitorInfo.cbSize = size_of::<MONITORINFOEXW>() as u32;

    // SAFETY: `monitor` came from `EnumDisplayMonitors` and `monitor_info` is a correctly sized,
    // writable `MONITORINFOEXW` whose prefix is the `MONITORINFO` expected by this API.
    unsafe { GetMonitorInfoW(monitor, (&raw mut monitor_info).cast()) }.ok().map_err(Error::GetMonitorInfo)?;

    Ok(monitor_info)
}

fn query_active_paths() -> Result<Vec<DISPLAYCONFIG_PATH_INFO>, Error> {
    const MAX_TOPOLOGY_READ_ATTEMPTS: usize = 3;

    for _ in 0..MAX_TOPOLOGY_READ_ATTEMPTS {
        let mut number_of_paths = 0;
        let mut number_of_modes = 0;
        // SAFETY: Both count pointers are valid for the duration of the call.
        let status =
            unsafe { GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut number_of_paths, &mut number_of_modes) };
        if status != ERROR_SUCCESS {
            return Err(Error::DisplayConfig { operation: "GetDisplayConfigBufferSizes", code: status.0 });
        }

        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); number_of_paths as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); number_of_modes as usize];
        // SAFETY: The vectors have the capacities returned by `GetDisplayConfigBufferSizes`; the
        // mutable count variables and optional topology argument meet the API's requirements.
        let status = unsafe {
            QueryDisplayConfig(
                QDC_ONLY_ACTIVE_PATHS,
                &mut number_of_paths,
                paths.as_mut_ptr(),
                &mut number_of_modes,
                modes.as_mut_ptr(),
                None,
            )
        };

        if status == ERROR_SUCCESS {
            paths.truncate(number_of_paths as usize);
            return Ok(paths);
        }
        if status != ERROR_INSUFFICIENT_BUFFER {
            return Err(Error::DisplayConfig { operation: "QueryDisplayConfig", code: status.0 });
        }
    }

    Err(Error::DisplayConfig { operation: "QueryDisplayConfig", code: ERROR_INSUFFICIENT_BUFFER.0 })
}

fn resolve_active_paths(paths: Vec<DISPLAYCONFIG_PATH_INFO>) -> Result<Vec<ResolvedPath>, Error> {
    paths.into_iter().map(|path| Ok(ResolvedPath { source_device_name: source_device_name(&path)?, path })).collect()
}

fn source_device_name(path: &DISPLAYCONFIG_PATH_INFO) -> Result<String, Error> {
    let mut source = DISPLAYCONFIG_SOURCE_DEVICE_NAME {
        header: device_info_header(
            DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
            size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32,
            path.sourceInfo.adapterId,
            path.sourceInfo.id,
        ),
        ..Default::default()
    };
    display_config_get_device_info(&mut source.header, "DisplayConfigGetDeviceInfo(GET_SOURCE_NAME)")?;
    Ok(wide_string(&source.viewGdiDeviceName))
}

fn target_friendly_name(path: &DISPLAYCONFIG_PATH_INFO) -> Result<String, Error> {
    let mut target = DISPLAYCONFIG_TARGET_DEVICE_NAME {
        header: device_info_header(
            DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
            size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32,
            path.targetInfo.adapterId,
            path.targetInfo.id,
        ),
        ..Default::default()
    };
    display_config_get_device_info(&mut target.header, "DisplayConfigGetDeviceInfo(GET_TARGET_NAME)")?;
    Ok(wide_string(&target.monitorFriendlyDeviceName))
}

fn advanced_color_info(path: &DISPLAYCONFIG_PATH_INFO) -> Result<AdvancedColorInfo, Error> {
    let mut advanced_color = DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO {
        header: device_info_header(
            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
            size_of::<DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO>() as u32,
            path.targetInfo.adapterId,
            path.targetInfo.id,
        ),
        ..Default::default()
    };
    display_config_get_device_info(&mut advanced_color.header, "DisplayConfigGetDeviceInfo(GET_ADVANCED_COLOR_INFO)")?;

    // The low two bits are `advancedColorSupported` and `advancedColorEnabled`, respectively.
    // SAFETY: The API initialized the returned union value after a successful call.
    let flags = unsafe { advanced_color.Anonymous.value };
    Ok(AdvancedColorInfo {
        supported: flags & 0b01 != 0,
        enabled: flags & 0b10 != 0,
        encoding: advanced_color.colorEncoding,
        bits_per_color: advanced_color.bitsPerColorChannel,
    })
}

/// Reads DXGI's own view of the output that drives `monitor`.
///
/// This complements the DisplayConfig data: Advanced Color reports what Windows has enabled for
/// the target, while `IDXGIOutput6::GetDesc1` reports the color space a capture actually receives.
/// A monitor served by an indirect display driver has no `IDXGIOutput` at all, and a pre-`Output6`
/// driver stack exposes no such metadata, so both cases report `Ok(None)` instead of failing the
/// whole enumeration.
fn dxgi_output_info(monitor: HMONITOR) -> Result<Option<DxgiOutputInfo>, Error> {
    // SAFETY: `CreateDXGIFactory1` only writes the requested interface pointer.
    let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1() }?;

    for adapter_index in 0u32.. {
        // SAFETY: Enumeration indexes are in range until DXGI reports `DXGI_ERROR_NOT_FOUND`.
        let adapter = match unsafe { factory.EnumAdapters1(adapter_index) } {
            Ok(adapter) => adapter,
            Err(error) if error.code() == DXGI_ERROR_NOT_FOUND => break,
            Err(error) => return Err(Error::Dxgi(error)),
        };

        for output_index in 0u32.. {
            // SAFETY: As above; the returned interface is owned by this scope.
            let output = match unsafe { adapter.EnumOutputs(output_index) } {
                Ok(output) => output,
                Err(error) if error.code() == DXGI_ERROR_NOT_FOUND => break,
                Err(error) => return Err(Error::Dxgi(error)),
            };

            let Ok(output) = output.cast::<IDXGIOutput6>() else { continue };
            // SAFETY: The interface was obtained above and is alive for this call.
            let descriptor = unsafe { output.GetDesc1() }?;
            if descriptor.Monitor != monitor {
                continue;
            }

            return Ok(Some(DxgiOutputInfo {
                color_space: dxgi_color_space_name(descriptor.ColorSpace),
                bits_per_color: descriptor.BitsPerColor,
            }));
        }
    }

    Ok(None)
}

/// Reads where Windows places SDR reference white for a target, as an scRGB multiplier.
///
/// `DISPLAYCONFIG_SDR_WHITE_LEVEL` reports the level in thousandths of the 80-nit scRGB reference,
/// so `3800` means desktop white sits at `3.8` in captured scRGB samples. Windows only defines
/// this for Advanced Color targets; anything else, and any non-positive reading, yields `1.0` so
/// callers can divide unconditionally.
fn sdr_white_level(path: &DISPLAYCONFIG_PATH_INFO) -> Result<f32, Error> {
    let mut white_level = DISPLAYCONFIG_SDR_WHITE_LEVEL {
        header: device_info_header(
            DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
            size_of::<DISPLAYCONFIG_SDR_WHITE_LEVEL>() as u32,
            path.targetInfo.adapterId,
            path.targetInfo.id,
        ),
        ..Default::default()
    };

    // An SDR target legitimately rejects this request, which is not a capture failure.
    if display_config_get_device_info(&mut white_level.header, "DisplayConfigGetDeviceInfo(GET_SDR_WHITE_LEVEL)")
        .is_err()
    {
        return Ok(1.0);
    }

    let scale = white_level.SDRWhiteLevel as f32 / 1_000.0;
    Ok(if scale.is_finite() && scale > 0.0 { scale } else { 1.0 })
}

fn display_config_get_device_info(
    header: &mut DISPLAYCONFIG_DEVICE_INFO_HEADER,
    operation: &'static str,
) -> Result<(), Error> {
    // SAFETY: All callers construct a complete request structure whose header refers to the
    // enclosing writable object, as required by `DisplayConfigGetDeviceInfo`.
    let status = unsafe { DisplayConfigGetDeviceInfo(header) };
    if status == 0 { Ok(()) } else { Err(Error::DisplayConfigDeviceInfo { operation, code: status }) }
}

const fn device_info_header(
    request_type: DISPLAYCONFIG_DEVICE_INFO_TYPE,
    request_size: u32,
    adapter_id: LUID,
    id: u32,
) -> DISPLAYCONFIG_DEVICE_INFO_HEADER {
    DISPLAYCONFIG_DEVICE_INFO_HEADER { r#type: request_type, size: request_size, adapterId: adapter_id, id }
}

fn select_path_for_device<'a>(paths: &'a [ResolvedPath], device_name: &str) -> Option<&'a DISPLAYCONFIG_PATH_INFO> {
    let mut selected: Option<&DISPLAYCONFIG_PATH_INFO> = None;

    for resolved in paths {
        if !resolved.source_device_name.eq_ignore_ascii_case(device_name) {
            continue;
        }

        let current_is_internal = is_internal_target(&resolved.path);
        let selected_is_internal = selected.is_some_and(is_internal_target);
        if selected.is_none() || (current_is_internal && !selected_is_internal) {
            selected = Some(&resolved.path);
        }
    }

    selected
}

fn is_internal_target(path: &DISPLAYCONFIG_PATH_INFO) -> bool {
    let output_technology = path.targetInfo.outputTechnology;
    output_technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL
        || output_technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED
        || output_technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED
}

fn color_encoding_name(encoding: DISPLAYCONFIG_COLOR_ENCODING) -> Option<&'static str> {
    if encoding == DISPLAYCONFIG_COLOR_ENCODING_RGB {
        Some("RGB")
    } else if encoding == DISPLAYCONFIG_COLOR_ENCODING_YCBCR444 {
        Some("YCbCr444")
    } else if encoding == DISPLAYCONFIG_COLOR_ENCODING_YCBCR422 {
        Some("YCbCr422")
    } else if encoding == DISPLAYCONFIG_COLOR_ENCODING_YCBCR420 {
        Some("YCbCr420")
    } else if encoding == DISPLAYCONFIG_COLOR_ENCODING_INTENSITY {
        Some("Intensity")
    } else {
        None
    }
}

/// Names the DXGI color space of an output, dropping the shared `DXGI_COLOR_SPACE_` prefix.
///
/// Only the spaces a display output can report are named. Anything else keeps its numeric DXGI
/// token so a diagnostic still identifies it exactly, which matters because this value is meant
/// to be compared against a capture's source format on unfamiliar hardware.
fn dxgi_color_space_name(color_space: DXGI_COLOR_SPACE_TYPE) -> String {
    let name = if color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 {
        "RGB_FULL_G22_NONE_P709"
    } else if color_space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 {
        "RGB_FULL_G10_NONE_P709"
    } else if color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709 {
        "RGB_STUDIO_G22_NONE_P709"
    } else if color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020 {
        "RGB_STUDIO_G22_NONE_P2020"
    } else if color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 {
        "RGB_FULL_G2084_NONE_P2020"
    } else if color_space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 {
        "RGB_STUDIO_G2084_NONE_P2020"
    } else if color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020 {
        "RGB_FULL_G22_NONE_P2020"
    } else if color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 {
        "YCBCR_STUDIO_G22_LEFT_P709"
    } else if color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020 {
        "YCBCR_STUDIO_G22_LEFT_P2020"
    } else if color_space == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 {
        "YCBCR_STUDIO_G2084_LEFT_P2020"
    } else {
        return format!("DXGI_COLOR_SPACE_TYPE({})", color_space.0);
    };

    name.to_owned()
}

fn wide_string(value: &[u16]) -> String {
    let end = value.iter().position(|character| *character == 0).unwrap_or(value.len());
    String::from_utf16_lossy(&value[..end])
}

fn rect_from_win32(rect: &Win32Rect) -> Result<Rect, Error> {
    rect_from_edges(rect.left, rect.top, rect.right, rect.bottom)
}

fn rect_from_edges(left: i32, top: i32, right: i32, bottom: i32) -> Result<Rect, Error> {
    let width = i64::from(right) - i64::from(left);
    let height = i64::from(bottom) - i64::from(top);
    let maximum_dimension = i64::from(u32::MAX);

    if !(0..=maximum_dimension).contains(&width) || !(0..=maximum_dimension).contains(&height) {
        return Err(Error::InvalidMonitorRectangle { left, top, right, bottom });
    }

    Ok(Rect::new(left, top, width as u32, height as u32))
}

#[cfg(test)]
mod tests {
    use super::{Rect, rect_from_edges};

    #[test]
    fn rectangle_preserves_negative_virtual_desktop_coordinates() {
        let rect = rect_from_edges(-1_920, -1_080, 0, 0).expect("valid virtual-desktop rectangle");

        assert_eq!(rect, Rect::new(-1_920, -1_080, 1_920, 1_080));
        assert_eq!(rect.right(), 0);
        assert_eq!(rect.bottom(), 0);
    }

    #[test]
    fn rectangle_rejects_reversed_edges() {
        assert!(rect_from_edges(100, 0, 99, 10).is_err());
        assert!(rect_from_edges(0, 100, 10, 99).is_err());
    }
}
