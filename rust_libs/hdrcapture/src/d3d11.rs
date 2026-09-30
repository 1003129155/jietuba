use std::slice;

use windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
use windows::Win32::Foundation::HMODULE;
use windows::Win32::Graphics::Direct3D::{
    D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_UNKNOWN, D3D_FEATURE_LEVEL, D3D_FEATURE_LEVEL_9_1, D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_11_1,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_CPU_ACCESS_READ, D3D11_CPU_ACCESS_WRITE, D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_MAP_READ_WRITE,
    D3D11_MAPPED_SUBRESOURCE, D3D11_SDK_VERSION, D3D11_TEXTURE2D_DESC, D3D11_USAGE_STAGING, D3D11CreateDevice,
    ID3D11Device, ID3D11DeviceContext, ID3D11Texture2D,
};
use windows::Win32::Graphics::Dxgi::Common::{DXGI_FORMAT, DXGI_SAMPLE_DESC};
use windows::Win32::Graphics::Dxgi::{
    CreateDXGIFactory1, DXGI_ERROR_NOT_FOUND, IDXGIAdapter, IDXGIDevice, IDXGIFactory1,
};
use windows::Win32::Graphics::Gdi::HMONITOR;
use windows::Win32::System::WinRT::Direct3D11::CreateDirect3D11DeviceFromDXGIDevice;
use windows::core::Interface;

#[derive(thiserror::Error, Eq, PartialEq, Clone, Debug)]
/// Errors that can occur when creating or working with Direct3D devices and textures.
pub enum Error {
    /// The created device does not support at least feature level 11.0.
    #[error("Failed to create DirectX device with the recommended feature levels")]
    FeatureLevelNotSatisfied,
    /// A Win32 API reported success but did not populate the requested output value.
    #[error("Windows API succeeded but did not return {0}")]
    UnexpectedNullResult(&'static str),
    /// A Windows Runtime/Win32 API call failed.
    ///
    /// Wraps [`windows::core::Error`].
    #[error("Windows API Error: {0}")]
    WindowsError(#[from] windows::core::Error),
    /// No DXGI adapter exposed the requested monitor.
    #[error("Failed to find a DXGI adapter for the requested monitor")]
    MonitorAdapterNotFound,
}

/// A wrapper to send a DirectX device across threads.
pub struct SendDirectX<T>(pub T);

impl<T> SendDirectX<T> {
    /// Constructs a new `SendDirectX` instance.
    #[inline]
    #[must_use]
    pub const fn new(device: T) -> Self {
        Self(device)
    }
}

#[allow(clippy::non_send_fields_in_send_ty)]
unsafe impl<T> Send for SendDirectX<T> {}

enum StagingTextureHandle<'a> {
    Owned(StagingTexture),
    Borrowed(&'a mut StagingTexture),
}

impl StagingTextureHandle<'_> {
    const fn texture(&self) -> &ID3D11Texture2D {
        match self {
            Self::Owned(texture) => texture.texture(),
            Self::Borrowed(texture) => texture.texture(),
        }
    }

    const fn set_mapped(&mut self, mapped: bool) {
        match self {
            Self::Owned(texture) => texture.set_mapped(mapped),
            Self::Borrowed(texture) => texture.set_mapped(mapped),
        }
    }
}

/// A mapped staging texture that automatically unmaps itself when dropped.
pub(crate) struct MappedStagingTexture<'a> {
    context: &'a ID3D11DeviceContext,
    texture: StagingTextureHandle<'a>,
    mapped: D3D11_MAPPED_SUBRESOURCE,
}

impl<'a> MappedStagingTexture<'a> {
    /// Maps an owned staging texture.
    pub fn map_owned(context: &'a ID3D11DeviceContext, texture: StagingTexture) -> Result<Self, windows::core::Error> {
        Self::map(context, StagingTextureHandle::Owned(texture))
    }

    /// Maps a caller-provided staging texture after making sure it is currently unmapped.
    pub fn map_borrowed(
        context: &'a ID3D11DeviceContext,
        texture: &'a mut StagingTexture,
    ) -> Result<Self, windows::core::Error> {
        unmap_staging_texture(context, texture);
        Self::map(context, StagingTextureHandle::Borrowed(texture))
    }

    fn map(
        context: &'a ID3D11DeviceContext,
        mut texture: StagingTextureHandle<'a>,
    ) -> Result<Self, windows::core::Error> {
        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        unsafe {
            context.Map(texture.texture(), 0, D3D11_MAP_READ_WRITE, 0, Some(&mut mapped))?;
        }
        texture.set_mapped(true);

        Ok(Self { context, texture, mapped })
    }

    /// Returns the mapped bytes as an immutable slice for the requested number of rows.
    #[must_use]
    pub const fn as_slice(&self, rows: u32) -> &[u8] {
        let len = rows as usize * self.mapped.RowPitch as usize;
        unsafe { slice::from_raw_parts(self.mapped.pData.cast(), len) }
    }

    /// Returns the mapped bytes as a mutable slice for the requested number of rows.
    #[must_use]
    pub const fn as_mut_slice(&mut self, rows: u32) -> &mut [u8] {
        let len = rows as usize * self.mapped.RowPitch as usize;
        unsafe { slice::from_raw_parts_mut(self.mapped.pData.cast(), len) }
    }

    /// Returns the row pitch reported by D3D11 for the mapped texture.
    #[must_use]
    pub const fn row_pitch(&self) -> u32 {
        self.mapped.RowPitch
    }

    /// Returns the depth pitch reported by D3D11 for the mapped texture.
    #[must_use]
    pub const fn depth_pitch(&self) -> u32 {
        self.mapped.DepthPitch
    }
}

impl Drop for MappedStagingTexture<'_> {
    fn drop(&mut self) {
        unsafe {
            self.context.Unmap(self.texture.texture(), 0);
        }
        self.texture.set_mapped(false);
    }
}

/// Unmaps a staging texture if it is currently mapped.
pub(crate) fn unmap_staging_texture(context: &ID3D11DeviceContext, texture: &mut StagingTexture) {
    if texture.is_mapped() {
        unsafe {
            context.Unmap(texture.texture(), 0);
        }
        texture.set_mapped(false);
    }
}

/// Creates an [`windows::Win32::Graphics::Direct3D11::ID3D11Device`] and an
/// [`windows::Win32::Graphics::Direct3D11::ID3D11DeviceContext`].
///
/// # Errors
///
/// - [`Error::WindowsError`] when the underlying `D3D11CreateDevice` call fails
/// - [`Error::FeatureLevelNotSatisfied`] when the created device does not support at least feature
///   level 11.0
#[inline]
pub fn create_d3d_device() -> Result<(ID3D11Device, ID3D11DeviceContext), Error> {
    create_d3d_device_inner(None, D3D_DRIVER_TYPE_HARDWARE)
}

fn create_d3d_device_inner(
    adapter: Option<&IDXGIAdapter>,
    driver_type: windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE,
) -> Result<(ID3D11Device, ID3D11DeviceContext), Error> {
    // Array of Direct3D feature levels.
    // The feature levels are listed in descending order of capability.
    // The highest feature level supported by the system is at index 0.
    // The lowest feature level supported by the system is at the last index.
    let feature_flags = [
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
        D3D_FEATURE_LEVEL_9_3,
        D3D_FEATURE_LEVEL_9_2,
        D3D_FEATURE_LEVEL_9_1,
    ];

    let mut d3d_device = None;
    let mut feature_level = D3D_FEATURE_LEVEL::default();
    let mut d3d_device_context = None;
    unsafe {
        D3D11CreateDevice(
            adapter,
            driver_type,
            HMODULE::default(),
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            Some(&feature_flags),
            D3D11_SDK_VERSION,
            Some(&mut d3d_device),
            Some(&mut feature_level),
            Some(&mut d3d_device_context),
        )?;
    };

    if feature_level.0 < D3D_FEATURE_LEVEL_11_0.0 {
        return Err(Error::FeatureLevelNotSatisfied);
    }

    let d3d_device = d3d_device.ok_or(Error::UnexpectedNullResult("an `ID3D11Device`"))?;
    let d3d_device_context = d3d_device_context.ok_or(Error::UnexpectedNullResult("an `ID3D11DeviceContext`"))?;

    Ok((d3d_device, d3d_device_context))
}

/// Creates a D3D11 device on the adapter that owns `monitor`.
///
/// This is required for DXGI Desktop Duplication on multi-GPU systems: a device created for one
/// adapter cannot duplicate an output that belongs to another adapter. Callers that capture
/// several outputs on the same adapter should reuse the returned device.
pub fn create_d3d_device_for_monitor(monitor: HMONITOR) -> Result<(ID3D11Device, ID3D11DeviceContext), Error> {
    let factory: IDXGIFactory1 = unsafe { CreateDXGIFactory1()? };
    let mut adapter_index = 0;

    loop {
        let adapter = match unsafe { factory.EnumAdapters1(adapter_index) } {
            Ok(adapter) => adapter,
            Err(error) if error.code() == DXGI_ERROR_NOT_FOUND => return Err(Error::MonitorAdapterNotFound),
            Err(error) => return Err(Error::WindowsError(error)),
        };
        let adapter_base = adapter.cast::<IDXGIAdapter>()?;
        let mut output_index = 0;

        loop {
            match unsafe { adapter_base.EnumOutputs(output_index) } {
                Ok(output) => {
                    if unsafe { output.GetDesc()? }.Monitor.0 == monitor.0 {
                        return create_d3d_device_inner(Some(&adapter_base), D3D_DRIVER_TYPE_UNKNOWN);
                    }
                    output_index += 1;
                }
                Err(error) if error.code() == DXGI_ERROR_NOT_FOUND => break,
                Err(error) => return Err(Error::WindowsError(error)),
            }
        }

        adapter_index += 1;
    }
}

/// Creates an [`windows::Graphics::DirectX::Direct3D11::IDirect3DDevice`] from an
/// [`windows::Win32::Graphics::Direct3D11::ID3D11Device`].
///
/// # Errors
///
/// - [`Error::WindowsError`] when creating the Direct3D11 device wrapper fails
#[inline]
pub fn create_direct3d_device(d3d_device: &ID3D11Device) -> Result<IDirect3DDevice, Error> {
    let dxgi_device: IDXGIDevice = d3d_device.cast()?;
    let inspectable = unsafe { CreateDirect3D11DeviceFromDXGIDevice(&dxgi_device)? };
    let device: IDirect3DDevice = inspectable.cast()?;

    Ok(device)
}

/// Reusable CPU-read/write staging texture wrapper.
pub struct StagingTexture {
    inner: ID3D11Texture2D,
    desc: D3D11_TEXTURE2D_DESC,
    is_mapped: bool,
}

impl StagingTexture {
    /// Create a staging texture suitable for CPU read/write with the given geometry/format.
    pub fn new(device: &ID3D11Device, width: u32, height: u32, format: DXGI_FORMAT) -> Result<Self, Error> {
        let desc = D3D11_TEXTURE2D_DESC {
            Width: width,
            Height: height,
            MipLevels: 1,
            ArraySize: 1,
            Format: format,
            SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
            Usage: D3D11_USAGE_STAGING,
            BindFlags: 0,
            CPUAccessFlags: (D3D11_CPU_ACCESS_READ.0 | D3D11_CPU_ACCESS_WRITE.0) as u32,
            MiscFlags: 0,
        };

        let mut tex = None;
        unsafe {
            device.CreateTexture2D(&desc, None, Some(&mut tex))?;
        }
        let inner = tex.ok_or(Error::UnexpectedNullResult("an `ID3D11Texture2D`"))?;

        Ok(Self { inner, desc, is_mapped: false })
    }

    /// Gets the underlying [`windows::Win32::Graphics::Direct3D11::ID3D11Texture2D`].
    #[inline]
    #[must_use]
    pub const fn texture(&self) -> &ID3D11Texture2D {
        &self.inner
    }

    /// Gets the description of the texture.
    #[inline]
    #[must_use]
    pub const fn desc(&self) -> D3D11_TEXTURE2D_DESC {
        self.desc
    }

    /// Checks if the texture is currently mapped.
    #[inline]
    #[must_use]
    pub const fn is_mapped(&self) -> bool {
        self.is_mapped
    }

    /// Marks the texture as mapped or unmapped.
    #[inline]
    pub const fn set_mapped(&mut self, mapped: bool) {
        self.is_mapped = mapped;
    }

    /// Validate an externally constructed texture as a CPU staging texture.
    /// The texture must have been created with `D3D11_USAGE_STAGING` usage and
    /// `D3D11_CPU_ACCESS_READ` and `D3D11_CPU_ACCESS_WRITE` CPU access flags.
    pub fn from_raw_checked(tex: ID3D11Texture2D) -> Option<Self> {
        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { tex.GetDesc(&mut desc) };
        let is_staging = desc.Usage == D3D11_USAGE_STAGING;
        let cpu_rw_mask = (D3D11_CPU_ACCESS_READ.0 | D3D11_CPU_ACCESS_WRITE.0) as u32;
        let has_cpu_rw = (desc.CPUAccessFlags & cpu_rw_mask) == cpu_rw_mask;

        if !is_staging || !has_cpu_rw {
            return None;
        }

        Some(Self { inner: tex, desc, is_mapped: false })
    }
}
