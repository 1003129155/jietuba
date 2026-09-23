//! D3D11 resources used by the synchronous HDR capture path.
//!
//! This module deliberately keeps images on the adapter until the caller asks for a completed
//! screenshot.  An HDR monitor is rendered into a normalized `BGRA8` texture with a pixel
//! shader, and same-adapter monitor textures are copied into a virtual-desktop texture before a
//! single staging-texture readback.

use std::mem::size_of_val;
use std::slice;

use windows::Win32::Graphics::Direct3D::{
    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
    Fxc::{D3DCOMPILE_ENABLE_STRICTNESS, D3DCOMPILE_OPTIMIZATION_LEVEL3, D3DCompile},
    ID3DBlob, ID3DInclude,
};
use windows::Win32::Graphics::Direct3D11::{
    D3D11_BIND_CONSTANT_BUFFER, D3D11_BIND_RENDER_TARGET, D3D11_BIND_SHADER_RESOURCE, D3D11_BUFFER_DESC,
    D3D11_COMPARISON_NEVER, D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_FLOAT32_MAX, D3D11_SAMPLER_DESC,
    D3D11_SUBRESOURCE_DATA, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE2D_DESC, D3D11_USAGE_DEFAULT,
    D3D11_USAGE_IMMUTABLE, D3D11_VIEWPORT, ID3D11Buffer, ID3D11DepthStencilView, ID3D11Device, ID3D11DeviceContext,
    ID3D11InputLayout, ID3D11PixelShader, ID3D11RenderTargetView, ID3D11SamplerState, ID3D11ShaderResourceView,
    ID3D11Texture2D, ID3D11VertexShader,
};
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R32G32_FLOAT, DXGI_MODE_ROTATION,
    DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270, DXGI_SAMPLE_DESC,
};
use windows::core::s;

use super::capture::ToneMapping;
use super::color::{ReferenceWhiteCurve, TileStats, adaptive_peak};
use crate::d3d11::{MappedStagingTexture, StagingTexture};
use crate::dxgi_duplication_api::DxgiDuplicationFormat;

/// Side of the square each statistics texel summarizes. Must stay identical to `STATS_TILE` in
/// the shader.
const STATS_TILE: u32 = 32;
/// `AdaptiveToneMap` contents that select the static mapping.
const STATIC_TONE_MAP: [f32; 8] = [0.0; 8];

/// Errors emitted while normalizing or composing GPU frames.
#[derive(thiserror::Error, Debug)]
pub(super) enum Error {
    /// A Windows graphics API call failed.
    #[error("Direct3D operation failed: {0}")]
    Windows(#[from] windows::core::Error),
    /// Creating a reusable staging texture failed.
    #[error("failed to create a D3D11 staging texture: {0}")]
    Staging(#[from] crate::d3d11::Error),
    /// A D3D compiler error was returned for the embedded shader.
    #[error("failed to compile the HDR conversion shader: {0}")]
    ShaderCompilation(String),
    /// A successful API call failed to populate a requested COM object.
    #[error("Direct3D did not return {0}")]
    MissingObject(&'static str),
}

const HDR_SHADER_SOURCE: &str = r#"
struct VsOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint vertex_id : SV_VertexID) {
    // A full-screen triangle avoids a vertex buffer and covers all pixels exactly once.
    const float2 positions[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };
    const float2 uvs[3] = { float2(0.0, 1.0), float2(0.0, -1.0), float2(2.0, 1.0) };
    VsOut output;
    output.position = float4(positions[vertex_id], 0.0, 1.0);
    output.uv = uvs[vertex_id];
    return output;
}

Texture2D<float4> source_texture : register(t0);
SamplerState source_sampler : register(s0);

float3 linear_to_srgb(float3 value) {
    float3 low = value * 12.92;
    float3 high = 1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055;
    return saturate(value <= 0.0031308 ? low : high);
}

cbuffer ToneMapParams : register(b0) {
    // Where this desktop's white sits on the scRGB scale, from Windows' SDR white level.
    float sdr_white_level;
    // Clockwise quarter-turns (0-3) needed to bring a DXGI Desktop Duplication frame - always
    // delivered in the panel's native, pre-rotation orientation - upright to match the rotated
    // desktop rectangle Windows reports for this monitor.
    float rotation_steps;
    float2 tone_map_padding;
};

// Maps a sample coordinate in the (possibly rotated) output back to the source texture, which is
// always in the panel's native orientation.
float2 unrotate_uv(float2 uv, float steps) {
    if (steps < 0.5) return uv;
    if (steps < 1.5) return float2(uv.y, 1.0 - uv.x);
    if (steps < 2.5) return float2(1.0 - uv.x, 1.0 - uv.y);
    return float2(1.0 - uv.y, uv.x);
}

// Rewritten before each adaptive render; `adaptive_enabled` is zero for static tone mapping. Field
// order must stay identical to ReferenceWhiteCurve::shader_constants in color.rs.
cbuffer AdaptiveToneMap : register(b1) {
    float adaptive_enabled;
    float input_maximum;
    float output_white;
    float curve_xa;
    float curve_xb;
    float curve_ya;
    float curve_yb;
    float adaptive_padding;
};

float3 normalize_to_white(float3 value, float white_level) {
    float scale = (isfinite(white_level) && white_level > 0.0) ? white_level : 1.0;
    return max(value, 0.0) / scale;
}

float max_component(float3 value) {
    return max(value.r, max(value.g, value.b));
}

// Must stay identical to ReferenceWhiteCurve::gain in color.rs.
float reference_white_gain(float x) {
    if (x <= 1.0) return output_white;
    if (x >= input_maximum) return 1.0 / x;
    float t = abs(curve_xa) < 0.00001
        ? (x - 1.0) / curve_xb
        : (-curve_xb + sqrt(max(curve_xb * curve_xb - 4.0 * curve_xa * (1.0 - x), 0.0))) / (2.0 * curve_xa);
    t = saturate(t);
    return (output_white + t * (curve_yb + t * curve_ya)) / x;
}

// Must stay identical to tone_map_highlights / tone_map_adaptive in color.rs: that module is the
// readable reference implementation of this shader, and a silent divergence between them is
// invisible in tests. Statically, SDR content passes through untouched and a brighter pixel is
// divided by its brightest component so that component lands on white with the hue kept.
float3 tone_map_highlights(float3 value, float white_level) {
    float3 normalized = normalize_to_white(value, white_level);
    float peak = max_component(normalized);
    if (adaptive_enabled > 0.5) return saturate(normalized * reference_white_gain(peak));
    return saturate(peak > 1.0 ? normalized / peak : normalized);
}

// Must stay identical to STATS_TILE and color::HDR_CONTENT_THRESHOLD on the Rust side.
static const uint STATS_TILE = 32;
static const float HDR_CONTENT_THRESHOLD = 1.02;

// One output texel per STATS_TILE square of the native-orientation source: its brightest
// component relative to desktop white, and how many of its pixels count as HDR content.
float2 ps_tile_stats(VsOut input) : SV_TARGET {
    uint width, height;
    source_texture.GetDimensions(width, height);
    uint2 origin = uint2(input.position.xy) * STATS_TILE;
    float peak = 0.0;
    float hdr_pixels = 0.0;
    [loop] for (uint y = 0; y < STATS_TILE; ++y) {
        [loop] for (uint x = 0; x < STATS_TILE; ++x) {
            uint2 texel = origin + uint2(x, y);
            if (texel.x < width && texel.y < height) {
                float value = max_component(normalize_to_white(source_texture.Load(int3(texel, 0)).rgb, sdr_white_level));
                peak = max(peak, value);
                hdr_pixels += value > HDR_CONTENT_THRESHOLD ? 1.0 : 0.0;
            }
        }
    }
    return float2(peak, hdr_pixels);
}

float4 ps_hdr(VsOut input) : SV_TARGET {
    float4 sampled = source_texture.SampleLevel(source_sampler, unrotate_uv(input.uv, rotation_steps), 0.0);
    // Desktop Duplication's FP16 HDR representation is scRGB: linear values with sRGB primaries.
    float3 display = linear_to_srgb(tone_map_highlights(sampled.rgb, sdr_white_level));
    return float4(display, saturate(sampled.a));
}

float4 ps_rgba8(VsOut input) : SV_TARGET {
    // Sampling exposes logical RGBA channels.  The BGRA render-target format performs the
    // physical channel layout conversion, preserving ordinary SDR pixels without a gamma pass.
    return source_texture.SampleLevel(source_sampler, unrotate_uv(input.uv, rotation_steps), 0.0);
}
"#;

fn shader_error(blob: Option<ID3DBlob>, fallback: windows::core::Error) -> Error {
    let detail = blob
        .map(|blob| unsafe {
            let bytes = slice::from_raw_parts(blob.GetBufferPointer().cast::<u8>(), blob.GetBufferSize());
            String::from_utf8_lossy(bytes).trim_end_matches('\0').to_owned()
        })
        .filter(|detail| !detail.is_empty())
        .unwrap_or_else(|| fallback.to_string());
    Error::ShaderCompilation(detail)
}

fn compile_shader(entry_point: windows::core::PCSTR, target: windows::core::PCSTR) -> Result<Vec<u8>, Error> {
    let mut byte_code = None;
    let mut errors = None;
    let result = unsafe {
        D3DCompile(
            HDR_SHADER_SOURCE.as_ptr().cast(),
            HDR_SHADER_SOURCE.len(),
            s!("hdr_capture.hlsl"),
            None,
            None::<&ID3DInclude>,
            entry_point,
            target,
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0,
            &mut byte_code,
            Some(&mut errors),
        )
    };

    if let Err(error) = result {
        return Err(shader_error(errors, error));
    }

    let byte_code = byte_code.ok_or(Error::MissingObject("compiled shader bytecode"))?;
    let bytes = unsafe { slice::from_raw_parts(byte_code.GetBufferPointer().cast::<u8>(), byte_code.GetBufferSize()) };
    Ok(bytes.to_vec())
}

fn create_default_texture(
    device: &ID3D11Device,
    width: u32,
    height: u32,
    format: DXGI_FORMAT,
    bind_flags: u32,
) -> Result<ID3D11Texture2D, Error> {
    let desc = D3D11_TEXTURE2D_DESC {
        Width: width,
        Height: height,
        MipLevels: 1,
        ArraySize: 1,
        Format: format,
        SampleDesc: DXGI_SAMPLE_DESC { Count: 1, Quality: 0 },
        Usage: D3D11_USAGE_DEFAULT,
        BindFlags: bind_flags,
        CPUAccessFlags: 0,
        MiscFlags: 0,
    };
    let mut texture = None;
    unsafe { device.CreateTexture2D(&desc, None, Some(&mut texture))? };
    texture.ok_or(Error::MissingObject("a D3D11 texture"))
}

/// GPU converter with reusable textures and shaders for one monitor.
pub(super) struct GpuToneMapper {
    width: u32,
    height: u32,
    native_width: u32,
    native_height: u32,
    rotated: bool,
    output: ID3D11Texture2D,
    output_rtv: ID3D11RenderTargetView,
    staging: StagingTexture,
    source_rgba16f: ID3D11Texture2D,
    source_rgba16f_srv: ID3D11ShaderResourceView,
    source_rgba8: ID3D11Texture2D,
    source_rgba8_srv: ID3D11ShaderResourceView,
    source_bgra8: ID3D11Texture2D,
    source_bgra8_srv: ID3D11ShaderResourceView,
    vertex_shader: ID3D11VertexShader,
    hdr_pixel_shader: ID3D11PixelShader,
    rgba8_pixel_shader: ID3D11PixelShader,
    tile_stats_shader: ID3D11PixelShader,
    sampler: ID3D11SamplerState,
    tone_map_params: ID3D11Buffer,
    adaptive_params: ID3D11Buffer,
    stats: ID3D11Texture2D,
    stats_rtv: ID3D11RenderTargetView,
    stats_staging: StagingTexture,
    stats_width: u32,
    stats_height: u32,
    /// Format of the frame last loaded by [`Self::convert`]; `None` before the first frame.
    format: Option<DxgiDuplicationFormat>,
    /// Tone mapping `output` currently holds; `None` when it has to be rendered again.
    rendered: Option<ToneMapping>,
    /// Peak the last FP16 render fitted into the output; `None` when it used the static mapping.
    tone_map_peak: Option<f32>,
}

/// Clockwise quarter-turns needed to undo a `DXGI_MODE_ROTATION` and bring a Desktop Duplication
/// frame - always delivered in the panel's native, pre-rotation orientation - upright.
const fn rotation_steps(rotation: DXGI_MODE_ROTATION) -> u32 {
    if rotation.0 == DXGI_MODE_ROTATION_ROTATE90.0 {
        1
    } else if rotation.0 == DXGI_MODE_ROTATION_ROTATE180.0 {
        2
    } else if rotation.0 == DXGI_MODE_ROTATION_ROTATE270.0 {
        3
    } else {
        0
    }
}

impl GpuToneMapper {
    /// Creates reusable conversion and readback resources for a monitor.
    ///
    /// `native_width`/`native_height` are the dimensions Desktop Duplication actually delivers
    /// frames in, i.e. the panel's pre-rotation orientation. `rotation` is the monitor's current
    /// `DXGI_MODE_ROTATION`; when it is a quarter turn, the output texture (and therefore every
    /// [`Self::readback`]) is sized `native_height x native_width` and sampling is rotated in the
    /// pixel shader to match. `sdr_white_level` is the monitor's current Windows SDR white level on
    /// the scRGB scale. Both are baked into an immutable constant buffer because a session is
    /// rebuilt whenever display state changes, so neither value can go stale while these resources
    /// live.
    pub(super) fn new(
        device: &ID3D11Device,
        native_width: u32,
        native_height: u32,
        sdr_white_level: f32,
        rotation: DXGI_MODE_ROTATION,
    ) -> Result<Self, Error> {
        let steps = rotation_steps(rotation);
        let rotated = steps == 1 || steps == 3;
        let (width, height) = if rotated { (native_height, native_width) } else { (native_width, native_height) };

        let output = create_default_texture(
            device,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            (D3D11_BIND_RENDER_TARGET.0 | D3D11_BIND_SHADER_RESOURCE.0) as u32,
        )?;
        let mut output_rtv = None;
        unsafe { device.CreateRenderTargetView(&output, None, Some(&mut output_rtv))? };
        let output_rtv = output_rtv.ok_or(Error::MissingObject("a render-target view"))?;

        let source_rgba16f = create_default_texture(
            device,
            native_width,
            native_height,
            windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R16G16B16A16_FLOAT,
            D3D11_BIND_SHADER_RESOURCE.0 as u32,
        )?;
        let mut source_rgba16f_srv = None;
        unsafe { device.CreateShaderResourceView(&source_rgba16f, None, Some(&mut source_rgba16f_srv))? };
        let source_rgba16f_srv = source_rgba16f_srv.ok_or(Error::MissingObject("an HDR shader-resource view"))?;

        let source_rgba8 = create_default_texture(
            device,
            native_width,
            native_height,
            windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE.0 as u32,
        )?;
        let mut source_rgba8_srv = None;
        unsafe { device.CreateShaderResourceView(&source_rgba8, None, Some(&mut source_rgba8_srv))? };
        let source_rgba8_srv = source_rgba8_srv.ok_or(Error::MissingObject("an SDR shader-resource view"))?;

        // Only needed to rotate a Bgra8-format frame (the common SDR case): CopyResource can't
        // rotate, so a rotated Bgra8 source must be routed through the shader like the other two
        // formats, which requires its own same-format shader-resource view.
        let source_bgra8 = create_default_texture(
            device,
            native_width,
            native_height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE.0 as u32,
        )?;
        let mut source_bgra8_srv = None;
        unsafe { device.CreateShaderResourceView(&source_bgra8, None, Some(&mut source_bgra8_srv))? };
        let source_bgra8_srv = source_bgra8_srv.ok_or(Error::MissingObject("a BGRA shader-resource view"))?;

        let stats_width = native_width.div_ceil(STATS_TILE);
        let stats_height = native_height.div_ceil(STATS_TILE);
        let stats = create_default_texture(
            device,
            stats_width,
            stats_height,
            DXGI_FORMAT_R32G32_FLOAT,
            D3D11_BIND_RENDER_TARGET.0 as u32,
        )?;
        let mut stats_rtv = None;
        unsafe { device.CreateRenderTargetView(&stats, None, Some(&mut stats_rtv))? };
        let stats_rtv = stats_rtv.ok_or(Error::MissingObject("a statistics render-target view"))?;

        let vertex_byte_code = compile_shader(s!("vs_main"), s!("vs_4_0"))?;
        let hdr_pixel_byte_code = compile_shader(s!("ps_hdr"), s!("ps_4_0"))?;
        let rgba8_pixel_byte_code = compile_shader(s!("ps_rgba8"), s!("ps_4_0"))?;
        let tile_stats_byte_code = compile_shader(s!("ps_tile_stats"), s!("ps_4_0"))?;
        let mut vertex_shader = None;
        let mut hdr_pixel_shader = None;
        let mut rgba8_pixel_shader = None;
        let mut tile_stats_shader = None;
        unsafe {
            device.CreateVertexShader(&vertex_byte_code, None, Some(&mut vertex_shader))?;
            device.CreatePixelShader(&hdr_pixel_byte_code, None, Some(&mut hdr_pixel_shader))?;
            device.CreatePixelShader(&rgba8_pixel_byte_code, None, Some(&mut rgba8_pixel_shader))?;
            device.CreatePixelShader(&tile_stats_byte_code, None, Some(&mut tile_stats_shader))?;
        }

        let sampler_desc = D3D11_SAMPLER_DESC {
            Filter: D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            AddressU: D3D11_TEXTURE_ADDRESS_CLAMP,
            AddressV: D3D11_TEXTURE_ADDRESS_CLAMP,
            AddressW: D3D11_TEXTURE_ADDRESS_CLAMP,
            MipLODBias: 0.0,
            MaxAnisotropy: 1,
            ComparisonFunc: D3D11_COMPARISON_NEVER,
            BorderColor: [0.0; 4],
            MinLOD: 0.0,
            MaxLOD: D3D11_FLOAT32_MAX,
        };
        let mut sampler = None;
        unsafe { device.CreateSamplerState(&sampler_desc, Some(&mut sampler))? };

        // A constant buffer is 16-byte aligned, so the two scalars are padded out to one float4.
        let tone_map_constants: [f32; 4] = [sdr_white_level, steps as f32, 0.0, 0.0];
        let buffer_desc = D3D11_BUFFER_DESC {
            ByteWidth: size_of_val(&tone_map_constants) as u32,
            Usage: D3D11_USAGE_IMMUTABLE,
            BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
            ..Default::default()
        };
        let initial_data = D3D11_SUBRESOURCE_DATA { pSysMem: tone_map_constants.as_ptr().cast(), ..Default::default() };
        let mut tone_map_params = None;
        // SAFETY: The descriptor matches `tone_map_constants`, which outlives this call.
        unsafe { device.CreateBuffer(&buffer_desc, Some(&initial_data), Some(&mut tone_map_params))? };

        let adaptive_desc = D3D11_BUFFER_DESC {
            ByteWidth: size_of_val(&STATIC_TONE_MAP) as u32,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
            ..Default::default()
        };
        let adaptive_data = D3D11_SUBRESOURCE_DATA { pSysMem: STATIC_TONE_MAP.as_ptr().cast(), ..Default::default() };
        let mut adaptive_params = None;
        // SAFETY: The descriptor matches `STATIC_TONE_MAP`, a constant that outlives this call.
        unsafe { device.CreateBuffer(&adaptive_desc, Some(&adaptive_data), Some(&mut adaptive_params))? };

        Ok(Self {
            width,
            height,
            native_width,
            native_height,
            rotated,
            output,
            output_rtv,
            staging: StagingTexture::new(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM)?,
            source_rgba16f,
            source_rgba16f_srv,
            source_rgba8,
            source_rgba8_srv,
            source_bgra8,
            source_bgra8_srv,
            vertex_shader: vertex_shader.ok_or(Error::MissingObject("a vertex shader"))?,
            hdr_pixel_shader: hdr_pixel_shader.ok_or(Error::MissingObject("an HDR pixel shader"))?,
            rgba8_pixel_shader: rgba8_pixel_shader.ok_or(Error::MissingObject("an SDR pixel shader"))?,
            tile_stats_shader: tile_stats_shader.ok_or(Error::MissingObject("a statistics pixel shader"))?,
            sampler: sampler.ok_or(Error::MissingObject("a sampler state"))?,
            tone_map_params: tone_map_params.ok_or(Error::MissingObject("a tone-map constant buffer"))?,
            adaptive_params: adaptive_params.ok_or(Error::MissingObject("an adaptive tone-map constant buffer"))?,
            stats_staging: StagingTexture::new(device, stats_width, stats_height, DXGI_FORMAT_R32G32_FLOAT)?,
            stats,
            stats_rtv,
            stats_width,
            stats_height,
            format: None,
            rendered: None,
            tone_map_peak: None,
        })
    }

    /// Returns the normalized `BGRA8` texture for GPU composition.
    pub(super) const fn output(&self) -> &ID3D11Texture2D {
        &self.output
    }

    /// Keeps a copy of `source` and renders it into the reusable normalized `BGRA8` output texture.
    pub(super) fn convert(
        &mut self,
        context: &ID3D11DeviceContext,
        source: &ID3D11Texture2D,
        format: DxgiDuplicationFormat,
        tone_mapping: ToneMapping,
    ) -> Result<(), Error> {
        let retained = match format {
            // A straight copy can't rotate, so a rotated monitor must go through the shader even
            // for the format that would otherwise need no conversion at all.
            DxgiDuplicationFormat::Bgra8 if !self.rotated => &self.output,
            DxgiDuplicationFormat::Bgra8 => &self.source_bgra8,
            DxgiDuplicationFormat::Rgba16F => &self.source_rgba16f,
            DxgiDuplicationFormat::Rgba8 => &self.source_rgba8,
        };
        unsafe { context.CopyResource(retained, source) };
        self.format = Some(format);
        self.rendered = None;
        self.render(context, tone_mapping)
    }

    /// Renders the retained frame into `output` with `tone_mapping`, unless `output` already holds
    /// exactly that. Only an FP16 HDR frame renders differently per tone mapping, so a cached frame
    /// can serve static and adaptive callers alternately without a new desktop present.
    pub(super) fn render(&mut self, context: &ID3D11DeviceContext, tone_mapping: ToneMapping) -> Result<(), Error> {
        let Some(format) = self.format else {
            return Ok(());
        };
        let is_hdr = format == DxgiDuplicationFormat::Rgba16F;
        if self.rendered == Some(tone_mapping) || (self.rendered.is_some() && !is_hdr) {
            return Ok(());
        }

        match format {
            DxgiDuplicationFormat::Bgra8 if !self.rotated => {}
            DxgiDuplicationFormat::Bgra8 => self.draw_output(context, &self.source_bgra8_srv, &self.rgba8_pixel_shader),
            DxgiDuplicationFormat::Rgba8 => self.draw_output(context, &self.source_rgba8_srv, &self.rgba8_pixel_shader),
            DxgiDuplicationFormat::Rgba16F => {
                self.tone_map_peak = match tone_mapping {
                    ToneMapping::Static => None,
                    ToneMapping::Adaptive => self.measure_peak(context)?,
                };
                let constants = self
                    .tone_map_peak
                    .map_or(STATIC_TONE_MAP, |peak| ReferenceWhiteCurve::new(peak).shader_constants());
                // SAFETY: `constants` has exactly the buffer's byte width and outlives this call.
                unsafe { context.UpdateSubresource(&self.adaptive_params, 0, None, constants.as_ptr().cast(), 0, 0) };
                self.draw_output(context, &self.source_rgba16f_srv, &self.hdr_pixel_shader);
            }
        }
        self.rendered = Some(tone_mapping);
        Ok(())
    }

    /// Peak, relative to desktop white, that the last render fitted into the output; `None` when
    /// the static mapping was used.
    pub(super) const fn tone_map_peak(&self) -> Option<f32> {
        self.tone_map_peak
    }

    /// Summarizes the retained FP16 frame per tile on the GPU and picks the peak adaptive tone
    /// mapping should fit, or `None` when the frame holds too little HDR content for it.
    fn measure_peak(&mut self, context: &ID3D11DeviceContext) -> Result<Option<f32>, Error> {
        self.draw(
            context,
            &self.stats_rtv,
            self.stats_width,
            self.stats_height,
            &self.source_rgba16f_srv,
            &self.tile_stats_shader,
        );
        unsafe { context.CopyResource(self.stats_staging.texture(), &self.stats) };
        let mapped = MappedStagingTexture::map_borrowed(context, &mut self.stats_staging)?;
        let row_pitch = mapped.row_pitch() as usize;
        let row_bytes = self.stats_width as usize * 8;
        let tiles: Vec<TileStats> = mapped
            .as_slice(self.stats_height)
            .chunks_exact(row_pitch)
            .flat_map(|row| row[..row_bytes].chunks_exact(8))
            .map(|texel| {
                [
                    f32::from_le_bytes([texel[0], texel[1], texel[2], texel[3]]),
                    f32::from_le_bytes([texel[4], texel[5], texel[6], texel[7]]),
                ]
            })
            .collect();
        Ok(adaptive_peak(&tiles, u64::from(self.native_width) * u64::from(self.native_height)))
    }

    fn draw_output(
        &self,
        context: &ID3D11DeviceContext,
        source: &ID3D11ShaderResourceView,
        pixel_shader: &ID3D11PixelShader,
    ) {
        self.draw(context, &self.output_rtv, self.width, self.height, source, pixel_shader);
    }

    fn draw(
        &self,
        context: &ID3D11DeviceContext,
        target: &ID3D11RenderTargetView,
        width: u32,
        height: u32,
        source: &ID3D11ShaderResourceView,
        pixel_shader: &ID3D11PixelShader,
    ) {
        let render_targets = [Some(target.clone())];
        let sources = [Some(source.clone())];
        let samplers = [Some(self.sampler.clone())];
        let constant_buffers = [Some(self.tone_map_params.clone()), Some(self.adaptive_params.clone())];
        let viewport = D3D11_VIEWPORT {
            TopLeftX: 0.0,
            TopLeftY: 0.0,
            Width: width as f32,
            Height: height as f32,
            MinDepth: 0.0,
            MaxDepth: 1.0,
        };
        unsafe {
            context.OMSetRenderTargets(Some(&render_targets), None::<&ID3D11DepthStencilView>);
            context.RSSetViewports(Some(&[viewport]));
            context.IASetInputLayout(None::<&ID3D11InputLayout>);
            context.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context.VSSetShader(&self.vertex_shader, None);
            context.PSSetShader(pixel_shader, None);
            context.PSSetShaderResources(0, Some(&sources));
            context.PSSetSamplers(0, Some(&samplers));
            context.PSSetConstantBuffers(0, Some(&constant_buffers));
            context.Draw(3, 0);
            // Explicitly unbind the source before its texture can be used by another operation.
            context.PSSetShaderResources(0, Some(&[None]));
            context.OMSetRenderTargets(None, None::<&ID3D11DepthStencilView>);
        }
    }

    /// Copies the normalized texture to its persistent staging texture and returns packed BGRA.
    pub(super) fn readback(&mut self, context: &ID3D11DeviceContext) -> Result<Vec<u8>, Error> {
        unsafe { context.CopyResource(self.staging.texture(), &self.output) };
        let mapped = MappedStagingTexture::map_borrowed(context, &mut self.staging)?;
        let row_pitch = mapped.row_pitch() as usize;
        let source = mapped.as_slice(self.height);
        let packed_row = self.width as usize * 4;
        let mut result = Vec::with_capacity(packed_row * self.height as usize);
        for row in source.chunks_exact(row_pitch).take(self.height as usize) {
            result.extend_from_slice(&row[..packed_row]);
        }
        Ok(result)
    }
}

/// A reusable BGRA8 virtual-desktop texture and its one-shot CPU readback buffer.
pub(super) struct GpuCompositor {
    width: u32,
    height: u32,
    texture: ID3D11Texture2D,
    render_target: ID3D11RenderTargetView,
    staging: StagingTexture,
}

impl GpuCompositor {
    /// Allocates a virtual-desktop composition surface.
    pub(super) fn new(device: &ID3D11Device, width: u32, height: u32) -> Result<Self, Error> {
        let texture = create_default_texture(
            device,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_RENDER_TARGET.0 as u32,
        )?;
        let mut render_target = None;
        unsafe { device.CreateRenderTargetView(&texture, None, Some(&mut render_target))? };
        Ok(Self {
            width,
            height,
            texture,
            render_target: render_target.ok_or(Error::MissingObject("a virtual-desktop render target"))?,
            staging: StagingTexture::new(device, width, height, DXGI_FORMAT_B8G8R8A8_UNORM)?,
        })
    }

    /// Clears the virtual desktop then copies a normalized monitor texture at every supplied offset.
    pub(super) fn compose(&self, context: &ID3D11DeviceContext, sources: &[(&ID3D11Texture2D, u32, u32)]) {
        unsafe {
            context.ClearRenderTargetView(&self.render_target, &[0.0, 0.0, 0.0, 1.0]);
            for (texture, left, top) in sources {
                context.CopySubresourceRegion(&self.texture, 0, *left, *top, 0, *texture, 0, None);
            }
        }
    }

    /// Reads the completed virtual desktop once into a tightly packed BGRA buffer.
    pub(super) fn readback(&mut self, context: &ID3D11DeviceContext) -> Result<Vec<u8>, Error> {
        unsafe { context.CopyResource(self.staging.texture(), &self.texture) };
        let mapped = MappedStagingTexture::map_borrowed(context, &mut self.staging)?;
        let row_pitch = mapped.row_pitch() as usize;
        let source = mapped.as_slice(self.height);
        let packed_row = self.width as usize * 4;
        let mut result = Vec::with_capacity(packed_row * self.height as usize);
        for row in source.chunks_exact(row_pitch).take(self.height as usize) {
            result.extend_from_slice(&row[..packed_row]);
        }
        Ok(result)
    }
}
