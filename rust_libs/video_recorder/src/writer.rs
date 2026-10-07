use std::{
    marker::PhantomData, mem::size_of, os::windows::ffi::OsStrExt,
    path::{Path, PathBuf}, ptr, rc::Rc, slice,
};

use windows::{
    core::{GUID, Interface, PCWSTR, PWSTR},
    Win32::{
        Foundation::HMODULE,
        Graphics::{
            Direct3D::D3D_DRIVER_TYPE_HARDWARE,
            Direct3D10::ID3D10Multithread,
            Direct3D11::{D3D11CreateDevice, ID3D11Device, ID3D11DeviceContext,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, D3D11_SDK_VERSION},
        },
        Media::MediaFoundation::*,
        Storage::FileSystem::{DeleteFileW, GetFileAttributesW, INVALID_FILE_ATTRIBUTES},
        System::{
            Com::{CoInitializeEx, CoTaskMemFree, CoUninitialize, IPersist, COINIT_MULTITHREADED},
            LibraryLoader::{GetModuleFileNameW, GetModuleHandleExW,
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT},
            Variant::{VARIANT, VT_BOOL, VT_UI4},
        },
    },
};

use crate::{capture::bgra_to_nv12, diagnostic::diagnostic, error::{Failure, Result}, options::Options};

fn check<T>(result: windows::core::Result<T>, operation: &str) -> Result<T> {
    result.map_err(|error| Failure::new("encoding", format!("{operation}: 0x{:08x}", error.code().0 as u32)))
}

fn wide_path(path: &Path) -> Vec<u16> {
    path.as_os_str().encode_wide().chain(Some(0)).collect()
}

/// COM 和 MF 必须在同一线程退出；声明在输出及编码器之前以最后释放。
pub struct MfRuntime { _thread: PhantomData<Rc<()>> }

impl MfRuntime {
    pub fn new() -> Result<Self> {
        unsafe {
            check(CoInitializeEx(None, COINIT_MULTITHREADED).ok(), "CoInitializeEx")?;
            if let Err(error) = MFStartup(MF_VERSION, MFSTARTUP_FULL) {
                CoUninitialize();
                return check(Err(error), "MFStartup");
            }
        }
        Ok(Self { _thread: PhantomData })
    }
}

impl Drop for MfRuntime {
    fn drop(&mut self) {
        unsafe { let _ = MFShutdown(); CoUninitialize(); }
    }
}

/// 只管理本进程原子创建成功的新文件；初始化失败绝不会删除已有路径。
pub struct OwnedOutput {
    path: PathBuf,
    stream: Option<IMFByteStream>,
    preserve: bool,
}

impl OwnedOutput {
    pub fn new(path: &Path) -> Result<Self> {
        let wide = wide_path(path);
        let stream = unsafe { MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST,
            MF_FILEFLAGS_NONE, PCWSTR(wide.as_ptr())) };
        let stream = stream.map_err(|error| {
            let existing = unsafe { GetFileAttributesW(PCWSTR(wide.as_ptr())) != INVALID_FILE_ATTRIBUTES };
            Failure::new(if existing { "output_exists" } else { "output" },
                format!("Cannot create a new output: 0x{:08x}", error.code().0 as u32))
        })?;
        Ok(Self { path: path.to_owned(), stream: Some(stream), preserve: false })
    }

    pub fn preserve(&mut self) {
        // Finalize 后由调用者释放 Sink Writer；随后释放文件引用再报告 complete。
        self.stream.take();
        self.preserve = true;
    }

    pub fn set_preserve(&mut self, preserve: bool) { self.preserve = preserve; }

    fn stream(&self) -> Result<&IMFByteStream> {
        self.stream.as_ref().ok_or_else(|| Failure::new("output", "Output stream is closed"))
    }
}

impl Drop for OwnedOutput {
    fn drop(&mut self) {
        self.stream.take();
        if !self.preserve {
            let path = wide_path(&self.path);
            if let Err(error) = unsafe { DeleteFileW(PCWSTR(path.as_ptr())) } {
                diagnostic("output_cleanup", &format!("Delete owned output: 0x{:08x}", error.code().0 as u32));
            }
        }
    }
}

#[derive(Default)]
pub struct EncoderMetadata {
    pub encoder: String,
    pub module: String,
    pub hardware: bool,
    pub encoder_identified: bool,
}

pub struct Writer {
    sink: Option<IMFSinkWriter>,
    video_stream: u32,
    audio_stream: Option<u32>,
    // 仅保留一帧 NV12，等下一帧或停止时确定持续时间，避免低帧率录像多出尾画面。
    pending_video: Option<(IMFSample, i64)>,
    // 保持设备及 manager 活到 Sink Writer 释放之后。
    _device: Option<ID3D11Device>,
    _context: Option<ID3D11DeviceContext>,
    _manager: Option<IMFDXGIDeviceManager>,
    metadata: EncoderMetadata,
}

impl Writer {
    pub fn new(options: &Options, output_file: &mut OwnedOutput, hardware: bool) -> Result<Self> {
        if cfg!(feature = "test-support") && hardware && options.fail_hardware {
            return Err(Failure::new("encoding", "Injected hardware initialization failure (test build)"));
        }
        unsafe {
            let attributes = create_attributes(6)?;
            check(attributes.SetUINT32(&MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, u32::from(hardware)), "Set hardware preference")?;
            check(attributes.SetUINT32(&MF_LOW_LATENCY, 1), "Set low latency")?;
            check(attributes.SetGUID(&MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_MPEG4), "Set MP4 container")?;
            let mut device: Option<ID3D11Device> = None;
            let mut context: Option<ID3D11DeviceContext> = None;
            let mut manager: Option<IMFDXGIDeviceManager> = None;
            if hardware {
                match D3D11CreateDevice(None, D3D_DRIVER_TYPE_HARDWARE, HMODULE::default(),
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                    None, D3D11_SDK_VERSION, Some(&mut device), None, Some(&mut context)) {
                    Ok(()) => {
                        if let Some(device) = &device {
                            if let Ok(multi) = device.cast::<ID3D10Multithread>() { let _ = multi.SetMultithreadProtected(true); }
                            let mut token = 0;
                            check(MFCreateDXGIDeviceManager(&mut token, &mut manager), "MFCreateDXGIDeviceManager")?;
                            let manager = manager.as_ref().ok_or_else(|| Failure::new("encoding", "Missing DXGI manager"))?;
                            check(manager.ResetDevice(device, token), "ResetDevice")?;
                            check(attributes.SetUnknown(&MF_SINK_WRITER_D3D_MANAGER, manager), "Set D3D manager")?;
                        }
                    }
                    Err(error) => diagnostic("d3d_device", &format!("0x{:08x}", error.code().0 as u32)),
                }
            }
            let stream = output_file.stream()?;
            // 硬件初始化回退复用同一新文件，原子文件归属不变。
            check(stream.SetLength(0), "Reset output length")?;
            check(stream.SetCurrentPosition(0), "Reset output position")?;
            let sink = check(MFCreateSinkWriterFromURL(PCWSTR::null(), stream, &attributes), "MFCreateSinkWriterFromURL")?;
            let encoded = video_type(options, MFVideoFormat_H264)?;
            check(encoded.SetUINT32(&MF_MT_AVG_BITRATE, options.bitrate), "Set bitrate")?;
            check(encoded.SetUINT32(&MF_MT_MPEG2_PROFILE, 77), "Set H264 main profile")?;
            let video_stream = check(sink.AddStream(&encoded), "Add video stream")?;
            let raw = video_type(options, MFVideoFormat_NV12)?;
            check(raw.SetUINT32(&MF_MT_DEFAULT_STRIDE, options.width), "Set stride")?;
            // 同时写入编码器初始化参数，避免系统按默认画质模式忽略目标码率。
            let encoding = create_attributes(5)?;
            check(encoding.SetUINT32(&CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR.0 as u32), "Set CBR parameters")?;
            check(encoding.SetUINT32(&CODECAPI_AVEncCommonMeanBitRate, options.bitrate), "Set bitrate parameters")?;
            if !hardware {
                check(encoding.SetUINT32(&CODECAPI_AVEncNumWorkerThreads, 1), "Set software encoder worker limit")?;
                check(encoding.SetUINT32(&CODECAPI_AVLowLatencyMode, 1), "Set software frame encoding mode")?;
                check(encoding.SetUINT32(&CODECAPI_AVEncMPVGOPSize, options.fps * 2), "Set software GOP size")?;
            }
            check(sink.SetInputMediaType(video_stream, &raw, &encoding), "Set video input")?;
            let audio_stream = if options.audio { Some(add_audio_stream(&sink)?) } else { None };
            check(sink.BeginWriting(), "BeginWriting")?;
            let mut writer = Self {
                sink: Some(sink), video_stream, audio_stream, pending_video: None,
                _device: device, _context: context,
                _manager: manager, metadata: EncoderMetadata::default(),
            };
            writer.verify_rate_control(options, hardware)?;
            writer.identify_encoder();
            if hardware && is_unusable_hardware_encoder(&writer.metadata.encoder, &writer.metadata.module) {
                diagnostic("unusable_hardware_encoder", &writer.metadata.encoder);
                return Err(Failure::new("encoding", format!("Hardware encoder cannot be used: {}", writer.metadata.encoder)));
            }
            writer.identify_audio_encoder();
            Ok(writer)
        }
    }

    pub fn metadata(&self) -> &EncoderMetadata { &self.metadata }

    fn sink(&self) -> Result<&IMFSinkWriter> {
        self.sink.as_ref().ok_or_else(|| Failure::new("encoding", "Sink Writer is finalized"))
    }

    pub fn write_video(&mut self, bgra: &[u8], width: u32, height: u32, timestamp: i64) -> Result<()> {
        self.flush_video(timestamp)?;
        let length = width as usize * height as usize * 3 / 2;
        let length = u32::try_from(length).map_err(|_| Failure::new("encoding", "Video frame exceeds buffer limit"))?;
        unsafe {
            let buffer = check(MFCreateMemoryBuffer(length), "Video buffer allocation")?;
            let mut bytes = ptr::null_mut();
            check(buffer.Lock(&mut bytes, None, None), "Video buffer Lock")?;
            let lock = BufferLock(&buffer);
            if bytes.is_null() { return Err(Failure::new("encoding", "Video buffer Lock returned null")); }
            bgra_to_nv12(bgra, slice::from_raw_parts_mut(bytes, length as usize), width, height)?;
            lock.unlock()?;
            check(buffer.SetCurrentLength(length), "Video buffer length")?;
            let sample = check(MFCreateSample(), "Video sample")?;
            check(sample.AddBuffer(&buffer), "Video sample buffer")?;
            check(sample.SetSampleTime(timestamp), "Video timestamp")?;
            self.pending_video = Some((sample, timestamp));
            Ok(())
        }
    }

    fn flush_video(&mut self, end_timestamp: i64) -> Result<()> {
        if let Some((sample, timestamp)) = self.pending_video.take() {
            unsafe {
                check(sample.SetSampleDuration(end_timestamp.saturating_sub(timestamp).max(1)), "Video duration")?;
                check(self.sink()?.WriteSample(self.video_stream, &sample), "Write video sample")?;
            }
        }
        Ok(())
    }

    pub fn write_audio(&self, sample: &IMFSample) -> Result<u64> {
        let stream = self.audio_stream.ok_or_else(|| Failure::new("audio", "Audio stream is disabled"))?;
        unsafe { check(self.sink()?.WriteSample(stream, sample), "Write audio sample")?; }
        Ok(self.stream_queued_bytes(stream))
    }

    pub fn queued_bytes(&self) -> u64 { self.stream_queued_bytes(self.video_stream) }

    fn stream_queued_bytes(&self, stream: u32) -> u64 {
        let Some(sink) = &self.sink else { return 0; };
        let mut statistics = MF_SINK_WRITER_STATISTICS {
            cb: size_of::<MF_SINK_WRITER_STATISTICS>() as u32, ..Default::default()
        };
        if unsafe { sink.GetStatistics(stream, &mut statistics) }.is_ok() {
            u64::from(statistics.dwByteCountQueued)
        } else { 0 }
    }

    pub fn finalize(&mut self, end_timestamp: i64) -> Result<()> {
        // 暂停期间仍保留这一帧；传入扣除暂停后的时间，停止时裁齐音视频结束点。
        self.flush_video(end_timestamp)?;
        if let Some(sink) = &self.sink { unsafe { check(sink.Finalize(), "Finalize")?; } }
        self.sink.take();
        Ok(())
    }

    unsafe fn verify_rate_control(&self, options: &Options, requested_hardware: bool) -> Result<()> {
        let extended = check(self.sink()?.cast::<IMFSinkWriterEx>(), "Sink Writer encoder inspection")?;
        let mut verified = false;
        for index in 0..16 {
            let mut category = GUID::zeroed();
            let mut transform = None;
            if extended.GetTransformForStream(self.video_stream, index, Some(&mut category), &mut transform).is_err() { break; }
            if category != MFT_CATEGORY_VIDEO_ENCODER { continue; }
            let transform = transform.ok_or_else(|| Failure::new("encoding", "Missing encoder transform"))?;
            let codec = check(transform.cast::<ICodecAPI>(), "Encoder codec properties")?;
            let module = transform_module(&transform);
            let software = !requested_hardware || is_software_module(&module);
            // IsSupported 的 S_FALSE 不能经 Result<()> 被误当成已支持。
            let workers_supported = (Interface::vtable(&codec).IsSupported)(Interface::as_raw(&codec), &CODECAPI_AVEncNumWorkerThreads).0 == 0;
            if software && workers_supported {
                check(codec.SetValue(&CODECAPI_AVEncNumWorkerThreads, &VARIANT::from(1_u32)), "Software encoder worker limit")?;
                check(codec.SetValue(&CODECAPI_AVLowLatencyMode, &VARIANT::from(true)), "Software frame encoding mode")?;
                check(codec.SetValue(&CODECAPI_AVEncMPVGOPSize, &VARIANT::from(options.fps * 2)), "Software GOP size")?;
                let low_latency = codec.GetValue(&CODECAPI_AVLowLatencyMode).ok()
                    .is_some_and(|actual| actual.vt() == VT_BOOL && bool::try_from(&actual).unwrap_or(false));
                diagnostic("software_encoding", &format!("workers=1, low_latency={low_latency}"));
            }
            let mode = eAVEncCommonRateControlMode_CBR.0 as u32;
            if codec_uint(&codec, &CODECAPI_AVEncCommonRateControlMode) != Some(mode) {
                check(codec.SetValue(&CODECAPI_AVEncCommonRateControlMode, &VARIANT::from(mode)), "Encoder CBR mode")?;
            }
            if codec_uint(&codec, &CODECAPI_AVEncCommonMeanBitRate) != Some(options.bitrate) {
                check(codec.SetValue(&CODECAPI_AVEncCommonMeanBitRate, &VARIANT::from(options.bitrate)), "Encoder mean bitrate")?;
            }
            let actual_mode = check(codec.GetValue(&CODECAPI_AVEncCommonRateControlMode), "Read encoder rate control")?;
            let actual_bitrate = check(codec.GetValue(&CODECAPI_AVEncCommonMeanBitRate), "Read encoder bitrate")?;
            let actual_mode = variant_uint(&actual_mode);
            let actual_bitrate = variant_uint(&actual_bitrate);
            verified = actual_mode == Some(mode) && actual_bitrate == Some(options.bitrate);
            diagnostic("codec_rate_control", &format!("mode={}, mean_bitrate={}, verified={verified}",
                actual_mode.unwrap_or(u32::MAX), actual_bitrate.unwrap_or(0)));
        }
        if !verified { return Err(Failure::new("encoding", "Encoder cannot verify constant bitrate control")); }
        Ok(())
    }

    unsafe fn identify_encoder(&mut self) {
        let Some(sink) = &self.sink else { return; };
        let Ok(extended) = sink.cast::<IMFSinkWriterEx>() else { return; };
        let known = enumerate_encoders();
        for index in 0..16 {
            let mut category = GUID::zeroed();
            let mut transform = None;
            if extended.GetTransformForStream(self.video_stream, index, Some(&mut category), &mut transform).is_err() { break; }
            if category != MFT_CATEGORY_VIDEO_ENCODER { continue; }
            let Some(transform) = transform else { continue; };
            let attributes = transform.GetAttributes().ok();
            let clsid = attributes.as_ref().and_then(|attrs| attrs.GetGUID(&MFT_TRANSFORM_CLSID_Attribute).ok())
                .or_else(|| transform.cast::<IPersist>().ok().and_then(|persist| persist.GetClassID().ok()))
                .unwrap_or_else(GUID::zeroed);
            let module = transform_module(&transform);
            let mut name = attr_text(attributes.as_ref(), &MFT_FRIENDLY_NAME_Attribute);
            let mut hardware = !attr_text(attributes.as_ref(), &MFT_ENUM_HARDWARE_URL_Attribute).is_empty();
            let mut identified = !name.is_empty() || clsid != GUID::zeroed();
            if let Some(record) = known.iter().find(|record| record.clsid == clsid) {
                name = record.name.clone(); hardware = record.hardware; identified = true;
            }
            if name.is_empty() {
                name = if module.is_empty() { "Windows H.264 (unidentified)".to_owned() } else { module.clone() };
            }
            if is_software_module(&module) { hardware = false; identified = true; }
            diagnostic("actual_encoder", &format!("{name}, module={module}, hardware={hardware}"));
            self.metadata = EncoderMetadata { encoder: name, module,
                hardware, encoder_identified: identified };
        }
    }

    unsafe fn identify_audio_encoder(&self) {
        let Some(audio_stream) = self.audio_stream else { return; };
        let Some(sink) = &self.sink else { return; };
        let Ok(extended) = sink.cast::<IMFSinkWriterEx>() else { return; };
        for index in 0..16 {
            let mut category = GUID::zeroed();
            let mut transform = None;
            if extended.GetTransformForStream(audio_stream, index, Some(&mut category), &mut transform).is_err() { break; }
            let Some(transform) = transform else { continue; };
            let attrs = transform.GetAttributes().ok();
            diagnostic(if category == MFT_CATEGORY_AUDIO_ENCODER { "actual_audio_encoder" } else { "actual_audio_transform" },
                &format!("index={index}, name={}, implementation_module={}",
                    attr_text(attrs.as_ref(), &MFT_FRIENDLY_NAME_Attribute), transform_module(&transform)));
        }
    }
}

struct BufferLock<'a>(&'a IMFMediaBuffer);
impl BufferLock<'_> {
    unsafe fn unlock(self) -> Result<()> {
        let result = self.0.Unlock();
        std::mem::forget(self);
        check(result, "Video buffer Unlock")
    }
}
impl Drop for BufferLock<'_> { fn drop(&mut self) { unsafe { let _ = self.0.Unlock(); } } }

unsafe fn create_attributes(capacity: u32) -> Result<IMFAttributes> {
    let mut attributes = None;
    check(MFCreateAttributes(&mut attributes, capacity), "MFCreateAttributes")?;
    attributes.ok_or_else(|| Failure::new("encoding", "Missing MF attributes"))
}

unsafe fn video_type(options: &Options, subtype: GUID) -> Result<IMFMediaType> {
    let media_type = check(MFCreateMediaType(), "Video media type")?;
    check(media_type.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Video), "Set video major type")?;
    check(media_type.SetGUID(&MF_MT_SUBTYPE, &subtype), "Set video subtype")?;
    check(media_type.SetUINT32(&MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive.0 as u32), "Set progressive")?;
    // MFSetAttributeSize/Ratio 是 SDK 的内联函数，其 ABI 就是两个 UINT32 打包成 UINT64。
    check(media_type.SetUINT64(&MF_MT_FRAME_SIZE, pack_pair(options.width, options.height)), "Set frame size")?;
    check(media_type.SetUINT64(&MF_MT_FRAME_RATE, pack_pair(options.fps, 1)), "Set frame rate")?;
    check(media_type.SetUINT64(&MF_MT_PIXEL_ASPECT_RATIO, pack_pair(1, 1)), "Set aspect ratio")?;
    check(media_type.SetUINT32(&MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601.0 as u32), "Set BT.601 matrix")?;
    check(media_type.SetUINT32(&MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235.0 as u32), "Set limited range")?;
    check(media_type.SetUINT32(&MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_SMPTE170M.0 as u32), "Set primaries")?;
    check(media_type.SetUINT32(&MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709.0 as u32), "Set transfer function")?;
    Ok(media_type)
}

fn pack_pair(high: u32, low: u32) -> u64 { u64::from(high) << 32 | u64::from(low) }

unsafe fn add_audio_stream(sink: &IMFSinkWriter) -> Result<u32> {
    let encoded = check(MFCreateMediaType(), "Audio output type")?;
    check(encoded.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Audio), "Audio major")?;
    check(encoded.SetGUID(&MF_MT_SUBTYPE, &MFAudioFormat_AAC), "AAC subtype")?;
    for (key, value) in [
        (MF_MT_AUDIO_NUM_CHANNELS, 2), (MF_MT_AUDIO_SAMPLES_PER_SECOND, 48_000),
        (MF_MT_AUDIO_BITS_PER_SAMPLE, 16), (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 128_000 / 8),
        (MF_MT_AAC_PAYLOAD_TYPE, 0), (MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29),
    ] { check(encoded.SetUINT32(&key, value), "Set AAC format")?; }
    let stream = check(sink.AddStream(&encoded), "Add AAC stream")?;
    let pcm = check(MFCreateMediaType(), "Audio PCM type")?;
    check(pcm.SetGUID(&MF_MT_MAJOR_TYPE, &MFMediaType_Audio), "PCM major")?;
    check(pcm.SetGUID(&MF_MT_SUBTYPE, &MFAudioFormat_PCM), "PCM subtype")?;
    for (key, value) in [
        (MF_MT_AUDIO_NUM_CHANNELS, 2), (MF_MT_AUDIO_SAMPLES_PER_SECOND, 48_000),
        (MF_MT_AUDIO_BITS_PER_SAMPLE, 16), (MF_MT_AUDIO_BLOCK_ALIGNMENT, 4),
        (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48_000 * 4),
    ] { check(pcm.SetUINT32(&key, value), "Set PCM format")?; }
    check(sink.SetInputMediaType(stream, &pcm, None), "Set PCM audio input")?;
    Ok(stream)
}

unsafe fn attr_text(attributes: Option<&IMFAttributes>, key: &GUID) -> String {
    let Some(attributes) = attributes else { return String::new(); };
    let mut value = PWSTR::null();
    let mut length = 0;
    if attributes.GetAllocatedString(key, &mut value, &mut length).is_err() { return String::new(); }
    if value.is_null() { return String::new(); }
    let text = String::from_utf16_lossy(slice::from_raw_parts(value.0, length as usize));
    CoTaskMemFree(Some(value.0.cast()));
    text
}

unsafe fn transform_module(transform: &IMFTransform) -> String {
    // 查询实际 transform 的方法地址所属模块，避免把硬编请求等同于识别结果。
    let address = Interface::vtable(transform).GetStreamLimits as *const ();
    let mut module = HMODULE::default();
    if GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        PCWSTR(address.cast()), &mut module).is_err() { return String::new(); }
    let mut path = [0_u16; 1024];
    let length = GetModuleFileNameW(Some(module), &mut path) as usize;
    if length == 0 || length >= path.len() { return String::new(); }
    let path = String::from_utf16_lossy(&path[..length]);
    path.rsplit(['\\', '/']).next().unwrap_or_default().to_owned()
}

fn is_software_module(module: &str) -> bool {
    module.eq_ignore_ascii_case("mfh264enc.dll") || module.eq_ignore_ascii_case("msmpeg2venc.dll")
}

/// Microsoft DX12 硬件编码器能完成初始化，写入第一批样本时却返回 MF_E_UNEXPECTED；
/// 厂商编码器拒绝当前画面尺寸时系统会改选它。按初始化失败处理，改用软件编码。
fn is_unusable_hardware_encoder(name: &str, module: &str) -> bool {
    module.to_ascii_lowercase().starts_with("msh264enchmft") || name.to_ascii_lowercase().contains("dx12 encoder")
}

fn variant_uint(value: &VARIANT) -> Option<u32> {
    if value.vt() == VT_UI4 { u32::try_from(value).ok() } else { None }
}

unsafe fn codec_uint(codec: &ICodecAPI, key: &GUID) -> Option<u32> {
    codec.GetValue(key).ok().and_then(|value| variant_uint(&value))
}

struct EncoderInfo { clsid: GUID, name: String, hardware: bool }
struct ActivationArray { pointer: *mut Option<IMFActivate>, count: u32 }
impl Drop for ActivationArray {
    fn drop(&mut self) {
        if self.pointer.is_null() { return; }
        unsafe {
            for activation in slice::from_raw_parts_mut(self.pointer, self.count as usize) { activation.take(); }
            CoTaskMemFree(Some(self.pointer.cast()));
        }
    }
}

unsafe fn enumerate_encoders() -> Vec<EncoderInfo> {
    let mut found = Vec::new();
    let input = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_NV12 };
    let output = MFT_REGISTER_TYPE_INFO { guidMajorType: MFMediaType_Video, guidSubtype: MFVideoFormat_H264 };
    for hardware in [false, true] {
        let flags = if hardware { MFT_ENUM_FLAG_HARDWARE } else { MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_LOCALMFT };
        let mut activations = ActivationArray { pointer: ptr::null_mut(), count: 0 };
        if let Err(error) = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags | MFT_ENUM_FLAG_SORTANDFILTER,
            Some(&input), Some(&output), &mut activations.pointer, &mut activations.count) {
            diagnostic("encoder_enumeration", &format!("0x{:08x}", error.code().0 as u32));
            continue;
        }
        if activations.pointer.is_null() { continue; }
        for activation in slice::from_raw_parts(activations.pointer, activations.count as usize).iter().flatten() {
            found.push(EncoderInfo {
                clsid: activation.GetGUID(&MFT_TRANSFORM_CLSID_Attribute).unwrap_or_else(|_| GUID::zeroed()),
                name: attr_text(Some(activation), &MFT_FRIENDLY_NAME_Attribute), hardware,
            });
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, time::{SystemTime, UNIX_EPOCH}};

    #[test]
    fn encoder_inspection_requires_exact_variant_type() {
        assert_eq!(variant_uint(&VARIANT::from(4_000_000_u32)), Some(4_000_000));
        assert_eq!(variant_uint(&VARIANT::from(4_000_000_i32)), None);
        assert_eq!(variant_uint(&VARIANT::from(true)), None);
        assert!(is_software_module("MFH264ENC.DLL"));
        assert!(!is_software_module("nvEncodeAPI64.dll"));
    }

    #[test]
    fn dx12_hardware_encoder_is_never_used() {
        assert!(is_unusable_hardware_encoder("Microsoft AVC DX12 Encoder HMFT", "msh264enchmft_store.dll"));
        assert!(is_unusable_hardware_encoder("", "MSH264ENCHMFT_STORE.DLL"));
        assert!(is_unusable_hardware_encoder("Microsoft AVC DX12 Encoder HMFT", ""));
        assert!(!is_unusable_hardware_encoder("Vendor H.264 Encoder MFT", "vendorenc64.dll"));
        assert!(!is_unusable_hardware_encoder("H264 Encoder MFT", "mfh264enc.dll"));
    }

    #[test]
    fn media_size_and_ratio_pack_as_sdk_inline_helpers() {
        assert_eq!(pack_pair(1920, 1080), 0x00000780_00000438);
        assert_eq!(pack_pair(30, 1), 0x0000001e_00000001);
    }

    #[test]
    fn output_ownership_never_overwrites_or_removes_existing_files() {
        let _runtime = MfRuntime::new().unwrap();
        let unique = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
        let directory = std::env::temp_dir().join(format!("jietuba-video-output-{}-{unique}", std::process::id()));
        fs::create_dir(&directory).unwrap();
        let existing = directory.join("已有文件.mp4");
        fs::write(&existing, b"existing file bytes").unwrap();
        let error = OwnedOutput::new(&existing).err().unwrap();
        assert_eq!(error.code, "output_exists");
        assert_eq!(fs::read(&existing).unwrap(), b"existing file bytes");
        let owned = directory.join("cancelled.mp4");
        let output = OwnedOutput::new(&owned).unwrap();
        assert!(owned.exists());
        drop(output);
        assert!(!owned.exists());
        let preserved = directory.join("preserved.mp4");
        let mut output = OwnedOutput::new(&preserved).unwrap();
        output.preserve();
        // 文件句柄已关闭，保留结果可以立即被调用者打开。
        fs::write(&preserved, b"released owned file").unwrap();
        drop(output);
        assert_eq!(fs::read(&preserved).unwrap(), b"released owned file");
        fs::remove_file(existing).unwrap();
        fs::remove_file(preserved).unwrap();
        fs::remove_dir(directory).unwrap();
    }
}
