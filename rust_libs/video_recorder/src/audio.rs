use crate::error::{Failure, Result};
use crate::diagnostic::diagnostic;
use crate::writer::Writer;
use std::marker::PhantomData;
use std::ptr::{self, NonNull};
use std::rc::Rc;
use windows::core::GUID;
use windows::Win32::Media::Audio::{
    eConsole, eRender, IAudioCaptureClient, IAudioClient, IMMDeviceEnumerator, MMDeviceEnumerator,
    AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY, AUDCLNT_BUFFERFLAGS_SILENT,
    AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
    WAVEFORMATEX, WAVEFORMATEXTENSIBLE, WAVE_FORMAT_PCM,
};
use windows::Win32::Media::KernelStreaming::{KSDATAFORMAT_SUBTYPE_PCM, WAVE_FORMAT_EXTENSIBLE};
use windows::Win32::Media::MediaFoundation::{
    IMFMediaBuffer, IMFSample, MFCreateMemoryBuffer, MFCreateSample,
};
use windows::Win32::Media::Multimedia::{KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, WAVE_FORMAT_IEEE_FLOAT};
use windows::Win32::System::Com::{CoCreateInstance, CoTaskMemFree, CLSCTX_ALL};
use windows::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};

pub const AUDIO_RATE: u32 = 48_000;
const CAPACITY: usize = AUDIO_RATE as usize;
const CHUNK_FRAMES: usize = 960;
const TICKS_PER_SECOND: i64 = 10_000_000;
const EMPTY_TAG: u64 = u64::MAX;

fn checked<T>(value: windows::core::Result<T>, context: &str) -> Result<T> {
    value.map_err(|error| Failure::new("audio", format!("{context}: {error}")))
}

pub fn qpc_ticks_100ns() -> Result<i64> {
    let mut counter = 0;
    let mut frequency = 0;
    unsafe {
        checked(
            QueryPerformanceCounter(&mut counter),
            "QueryPerformanceCounter",
        )?;
        checked(
            QueryPerformanceFrequency(&mut frequency),
            "QueryPerformanceFrequency",
        )?;
    }
    if frequency <= 0 {
        return Err(Failure::new("audio_timestamp", "Invalid QPC frequency"));
    }
    i64::try_from(i128::from(counter) * i128::from(TICKS_PER_SECOND) / i128::from(frequency))
        .map_err(|_| Failure::new("audio_timestamp", "QPC timestamp overflow"))
}

// GetMixFormat 返回的内存属于 COM 分配器；失败和正常退出均在同一线程释放。
struct MixFormat(NonNull<WAVEFORMATEX>);

impl Drop for MixFormat {
    fn drop(&mut self) {
        unsafe { CoTaskMemFree(Some(self.0.as_ptr().cast())) };
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SampleKind {
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
}

#[derive(Clone, Copy, Debug)]
struct SampleFormat {
    rate: u32,
    channels: usize,
    block_align: usize,
    bits: u16,
    kind: SampleKind,
    stereo_weights: [[f64; 2]; 8],
}

impl SampleFormat {
    fn new(format: WAVEFORMATEX, subtype: GUID, channel_mask: u32) -> Result<Self> {
        let channels = format.nChannels;
        if !(1..=8).contains(&channels) || format.nSamplesPerSec == 0 {
            return Err(Failure::new(
                "audio_format",
                "Playback formats must have 1..8 channels and a valid sample rate",
            ));
        }
        let floating = u32::from(format.wFormatTag) == WAVE_FORMAT_IEEE_FLOAT
            || subtype == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        let pcm =
            u32::from(format.wFormatTag) == WAVE_FORMAT_PCM || subtype == KSDATAFORMAT_SUBTYPE_PCM;
        let kind = match (floating, pcm, format.wBitsPerSample) {
            (true, _, 32) => SampleKind::Float32,
            (false, true, 16) => SampleKind::Pcm16,
            (false, true, 24) => SampleKind::Pcm24,
            (false, true, 32) => SampleKind::Pcm32,
            _ => {
                return Err(Failure::new(
                    "audio_format",
                    "Unsupported WASAPI mix sample format",
                ))
            }
        };
        if format.nBlockAlign != format.nChannels * (format.wBitsPerSample / 8) {
            return Err(Failure::new(
                "audio_format",
                "Invalid playback block alignment",
            ));
        }
        Ok(Self {
            rate: format.nSamplesPerSec,
            channels: usize::from(format.nChannels),
            block_align: usize::from(format.nBlockAlign),
            bits: format.wBitsPerSample,
            kind,
            stereo_weights: stereo_weights(usize::from(channels), channel_mask)?,
        })
    }

    fn read_value(self, data: &[u8], frame: usize, channel: usize) -> f64 {
        // 所有整数都按 Windows PCM 的小端有符号格式读取。
        let channel = channel.min(self.channels - 1);
        let offset = frame * self.block_align + channel * usize::from(self.bits / 8);
        let bytes = &data[offset..];
        match self.kind {
            SampleKind::Float32 => {
                let value = f32::from_le_bytes(bytes[..4].try_into().unwrap());
                if value.is_finite() {
                    f64::from(value)
                } else {
                    0.0
                }
            }
            SampleKind::Pcm16 => {
                f64::from(i16::from_le_bytes(bytes[..2].try_into().unwrap())) / 32_768.0
            }
            SampleKind::Pcm24 => {
                let value = i32::from_le_bytes([bytes[0], bytes[1], bytes[2], 0]) << 8 >> 8;
                f64::from(value) / 8_388_608.0
            }
            SampleKind::Pcm32 => {
                f64::from(i32::from_le_bytes(bytes[..4].try_into().unwrap())) / 2_147_483_648.0
            }
        }
    }

    fn read_stereo(self, data: &[u8], frame: usize) -> [f64; 2] {
        if self.channels == 1 { return [self.read_value(data, frame, 0); 2]; }
        if self.channels == 2 && self.stereo_weights[0] == [1.0, 0.0]
            && self.stereo_weights[1] == [0.0, 1.0] {
            return [self.read_value(data, frame, 0), self.read_value(data, frame, 1)];
        }
        let mut output = [0.0; 2];
        for channel in 0..self.channels {
            let sample = self.read_value(data, frame, channel);
            output[0] += sample * self.stereo_weights[channel][0];
            output[1] += sample * self.stereo_weights[channel][1];
        }
        output
    }
}

fn stereo_weights(channels: usize, mask: u32) -> Result<[[f64; 2]; 8]> {
    // WAVEFORMATEXTENSIBLE 按 mask 从低位到高位排列，不能假定前两路就是左右声道。
    // https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask
    let mask = if mask == 0 {
        match channels { 1 => 0x4, 2 => 0x3, _ => 0 }
    } else { mask };
    if mask.count_ones() as usize != channels || mask & !0x3ffff != 0 {
        return Err(Failure::new("audio_format", "Missing or invalid playback speaker layout"));
    }
    let mut weights = [[0.0; 2]; 8];
    let mut channel = 0;
    for bit in 0..18 {
        if mask & (1 << bit) == 0 { continue; }
        weights[channel] = match bit {
            0 => [1.0, 0.0], // front left
            1 => [0.0, 1.0], // front right
            3 => [0.5, 0.5], // LFE 保留低频，降低权重
            4 | 6 | 9 | 12 | 15 => [std::f64::consts::FRAC_1_SQRT_2, 0.0],
            5 | 7 | 10 | 14 | 17 => [0.0, std::f64::consts::FRAC_1_SQRT_2],
            _ => [std::f64::consts::FRAC_1_SQRT_2; 2], // center / back center / top center
        };
        channel += 1;
    }
    // 固定增益预留叠加空间，避免多声道同时响时削波；单声道与立体声保持原音量。
    for side in 0..2 {
        let gain = weights[..channels].iter().map(|weight| weight[side]).sum::<f64>().max(1.0);
        for weight in &mut weights[..channels] { weight[side] /= gain; }
    }
    if channels == 1 { weights[0] = [1.0, 1.0]; }
    Ok(weights)
}

fn pcm16(value: f64) -> i16 {
    (value.clamp(-1.0, 32_767.0 / 32_768.0) * 32_768.0).round() as i16
}

#[derive(Default)]
struct Counters {
    real_frames: u64,
    silence_frames: u64,
    captured_frames: u64,
    late_frames: u64,
    overflow_frames: u64,
    discontinuities: u64,
    packets: u64,
    nonzero_frames: u64,
    timestamp_errors: u64,
    pause_tail_frames_discarded: u64,
    max_queued_bytes: u64,
}

struct PcmRing {
    samples: Vec<i16>,
    tags: Vec<u64>,
    chunk: Vec<i16>,
    written_frames: u64,
    counters: Counters,
}

impl PcmRing {
    fn new(enabled: bool) -> Self {
        Self {
            samples: vec![0; if enabled { CAPACITY * 2 } else { 0 }],
            tags: vec![EMPTY_TAG; if enabled { CAPACITY } else { 0 }],
            chunk: vec![0; if enabled { CHUNK_FRAMES * 2 } else { 0 }],
            written_frames: 0,
            counters: Counters::default(),
        }
    }

    fn put(&mut self, frame: u64, left: i16, right: i16) {
        if frame < self.written_frames {
            self.counters.late_frames += 1;
        } else if frame - self.written_frames >= CAPACITY as u64 {
            self.counters.overflow_frames += 1;
        } else {
            let index = frame as usize % CAPACITY;
            self.tags[index] = frame;
            self.samples[index * 2] = left;
            self.samples[index * 2 + 1] = right;
        }
    }

    fn clear_pending(&mut self) {
        self.counters.pause_tail_frames_discarded += self
            .tags
            .iter()
            .filter(|&&tag| tag != EMPTY_TAG && tag >= self.written_frames)
            .count() as u64;
        self.tags.fill(EMPTY_TAG);
    }

    fn make_chunk(&mut self, count: usize) {
        for index in 0..count {
            let frame = self.written_frames + index as u64;
            let at = frame as usize % CAPACITY;
            let (left, right) = if self.tags[at] == frame {
                self.tags[at] = EMPTY_TAG;
                self.counters.real_frames += 1;
                (self.samples[at * 2], self.samples[at * 2 + 1])
            } else {
                self.counters.silence_frames += 1;
                (0, 0)
            };
            if left != 0 || right != 0 {
                self.counters.nonzero_frames += 1;
            }
            self.chunk[index * 2] = left;
            self.chunk[index * 2 + 1] = right;
        }
    }

    fn resample(
        &mut self,
        format: SampleFormat,
        data: &[u8],
        count: u32,
        first: i64,
        silent: bool,
    ) {
        if count == 0 {
            return;
        }
        let out_count = (u64::from(count) * u64::from(AUDIO_RATE) + u64::from(format.rate) / 2)
            / u64::from(format.rate);
        for index in 0..out_count {
            let frame = i128::from(first) + i128::from(index);
            if frame < 0 {
                continue;
            }
            let (left, right) = if silent {
                (0, 0)
            } else {
                let source = index as f64 * f64::from(format.rate) / f64::from(AUDIO_RATE);
                let at = (source as usize).min(count as usize - 1);
                let next = (at + 1).min(count as usize - 1);
                let fraction = source - at as f64;
                let a = format.read_stereo(data, at);
                let b = format.read_stereo(data, next);
                (pcm16(a[0] + (b[0] - a[0]) * fraction),
                 pcm16(a[1] + (b[1] - a[1]) * fraction))
            };
            if let Ok(frame) = u64::try_from(frame) {
                self.put(frame, left, right);
            }
        }
    }
}

// 即使时间戳校验或重采样失败，已取得的 WASAPI 包也必须成对释放。
struct CapturePacket<'a> {
    capture: &'a IAudioCaptureClient,
    data: *mut u8,
    frames: u32,
    flags: u32,
    qpc: u64,
    held: bool,
}

impl<'a> CapturePacket<'a> {
    fn get(capture: &'a IAudioCaptureClient) -> Result<Self> {
        let mut packet = Self {
            capture,
            data: ptr::null_mut(),
            frames: 0,
            flags: 0,
            qpc: 0,
            held: false,
        };
        unsafe {
            checked(
                capture.GetBuffer(
                    &mut packet.data,
                    &mut packet.frames,
                    &mut packet.flags,
                    None,
                    Some(&mut packet.qpc),
                ),
                "WASAPI GetBuffer",
            )?;
        }
        packet.held = packet.frames > 0;
        Ok(packet)
    }

    fn release(mut self) -> Result<()> {
        if self.held {
            self.held = false;
            unsafe {
                checked(
                    self.capture.ReleaseBuffer(self.frames),
                    "WASAPI ReleaseBuffer",
                )?
            };
        }
        Ok(())
    }
}

impl Drop for CapturePacket<'_> {
    fn drop(&mut self) {
        if self.held {
            unsafe {
                let _ = self.capture.ReleaseBuffer(self.frames);
            }
        }
    }
}

struct LockedBuffer<'a>(&'a IMFMediaBuffer);

impl Drop for LockedBuffer<'_> {
    fn drop(&mut self) {
        unsafe {
            let _ = self.0.Unlock();
        }
    }
}

fn audio_sample(pcm: &[i16], timestamp: i64, duration: i64) -> Result<IMFSample> {
    unsafe {
        let bytes = (pcm.len() * std::mem::size_of::<i16>()) as u32;
        let buffer = checked(MFCreateMemoryBuffer(bytes), "Audio buffer allocation")?;
        let mut destination = ptr::null_mut();
        checked(
            buffer.Lock(&mut destination, None, None),
            "Audio buffer Lock",
        )?;
        let locked = LockedBuffer(&buffer);
        if destination.is_null() {
            return Err(Failure::new(
                "audio",
                "Audio buffer Lock returned a null pointer",
            ));
        }
        ptr::copy_nonoverlapping(pcm.as_ptr().cast::<u8>(), destination, bytes as usize);
        checked(buffer.Unlock(), "Audio buffer Unlock")?;
        std::mem::forget(locked);
        checked(buffer.SetCurrentLength(bytes), "Audio buffer length")?;
        let sample = checked(MFCreateSample(), "Audio sample")?;
        checked(sample.AddBuffer(&buffer), "Audio sample buffer")?;
        checked(sample.SetSampleTime(timestamp), "Audio time")?;
        checked(sample.SetSampleDuration(duration), "Audio duration")?;
        Ok(sample)
    }
}

pub struct AudioCapture {
    client: Option<IAudioClient>,
    capture: Option<IAudioCaptureClient>,
    format: Option<SampleFormat>,
    started: bool,
    start_qpc: i64,
    paused_ticks: i64,
    ring: PcmRing,
    // WASAPI 接口只在录制线程创建、轮询和释放，不允许移动到另一线程。
    _same_thread: PhantomData<Rc<()>>,
}

impl AudioCapture {
    pub fn new(enabled: bool) -> Result<Self> {
        let mut result = Self {
            client: None,
            capture: None,
            format: None,
            started: false,
            start_qpc: 0,
            paused_ticks: 0,
            ring: PcmRing::new(enabled),
            _same_thread: PhantomData,
        };
        if !enabled {
            return Ok(result);
        }
        unsafe {
            let enumerator: IMMDeviceEnumerator = checked(
                CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_ALL),
                "Audio device enumerator",
            )?;
            let endpoint = enumerator.GetDefaultAudioEndpoint(eRender, eConsole)
                .map_err(|error| Failure::new("audio_device", format!("Default playback endpoint: {error}")))?;
            let client: IAudioClient =
                checked(endpoint.Activate(CLSCTX_ALL, None), "Activate WASAPI")?;
            let raw = checked(client.GetMixFormat(), "WASAPI mix format")?;
            let mix =
                MixFormat(NonNull::new(raw).ok_or_else(|| {
                    Failure::new("audio_format", "WASAPI returned no mix format")
                })?);
            let base = ptr::read_unaligned(mix.0.as_ptr());
            let (subtype, channel_mask) = if u32::from(base.wFormatTag) == WAVE_FORMAT_EXTENSIBLE {
                if base.cbSize < 22 {
                    return Err(Failure::new(
                        "audio_format",
                        "Invalid extensible playback format",
                    ));
                }
                let extended = ptr::read_unaligned(mix.0.as_ptr().cast::<WAVEFORMATEXTENSIBLE>());
                (extended.SubFormat, extended.dwChannelMask)
            } else {
                (GUID::zeroed(), 0)
            };
            let format = SampleFormat::new(base, subtype, channel_mask)?;
            checked(
                client.Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                    TICKS_PER_SECOND,
                    0,
                    mix.0.as_ptr(),
                    None,
                ),
                "Initialize loopback",
            )?;
            let capture: IAudioCaptureClient =
                checked(client.GetService(), "WASAPI capture service")?;
            let buffer_frames = checked(client.GetBufferSize(), "WASAPI buffer size")?;
            diagnostic("audio_capture_format", &format!(
                "\"source\":\"default_playback_loopback\",\"mix_rate\":{},\"mix_channels\":{},\"mix_bits\":{},\"float\":{},\"device_buffer_frames\":{},\"ring_capacity_frames\":{}",
                format.rate, format.channels, format.bits, format.kind == SampleKind::Float32,
                buffer_frames, CAPACITY));
            result.client = Some(client);
            result.capture = Some(capture);
            result.format = Some(format);
        }
        Ok(result)
    }

    pub fn start(&mut self, qpc: i64) -> Result<()> {
        self.start_qpc = qpc;
        if let Some(client) = &self.client {
            unsafe { checked(client.Start(), "WASAPI Start")? };
            self.started = true;
        }
        Ok(())
    }

    pub fn pause(&mut self) -> Result<()> {
        self.poll()?;
        self.stop()
    }

    pub fn stop(&mut self) -> Result<()> {
        if let Some(client) = &self.client {
            if self.started {
                unsafe { checked(client.Stop(), "WASAPI Stop")? };
                self.started = false;
                unsafe { checked(client.Reset(), "WASAPI Reset")? };
            }
        }
        Ok(())
    }

    pub fn resume(&mut self, paused_ticks: i64) -> Result<()> {
        // 恢复前丢弃尚未提交的旧标签，暂停前的尾音不会进入恢复后的时间线。
        self.ring.clear_pending();
        self.paused_ticks = paused_ticks;
        if let Some(client) = &self.client {
            unsafe { checked(client.Start(), "WASAPI resume")? };
            self.started = true;
        }
        Ok(())
    }

    pub fn poll(&mut self) -> Result<()> {
        if !self.started {
            return Ok(());
        }
        let Some(capture) = &self.capture else {
            return Ok(());
        };
        let format = self
            .format
            .ok_or_else(|| Failure::new("audio_format", "Missing mix format"))?;
        let mut packets = 0;
        while unsafe { checked(capture.GetNextPacketSize(), "WASAPI next packet")? } != 0 {
            packets += 1;
            if packets > 256 {
                return Err(Failure::new(
                    "audio",
                    "WASAPI capture cannot keep up with playback",
                ));
            }
            let packet = CapturePacket::get(capture)?;
            if packet.frames == 0 {
                break;
            }
            self.ring.counters.packets += 1;
            self.ring.counters.captured_frames += u64::from(packet.frames);
            if packet.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY.0 as u32 != 0 {
                self.ring.counters.discontinuities += 1;
            }
            if packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR.0 as u32 != 0 {
                self.ring.counters.timestamp_errors += 1;
                return Err(Failure::new(
                    "audio_timestamp",
                    "WASAPI reported an invalid packet timestamp",
                ));
            }
            // 包时间戳与录制起点均为 QPC 的 100 ns 单位，暂停时间从共同时间线扣除。
            let begin =
                i128::from(packet.qpc) - i128::from(self.start_qpc) - i128::from(self.paused_ticks);
            let first = (begin as f64 * f64::from(AUDIO_RATE) / TICKS_PER_SECOND as f64).round();
            if first < i64::MIN as f64 || first > i64::MAX as f64 {
                return Err(Failure::new(
                    "audio_timestamp",
                    "WASAPI packet timestamp overflow",
                ));
            }
            let silent = packet.flags & AUDCLNT_BUFFERFLAGS_SILENT.0 as u32 != 0;
            let data = if silent {
                &[][..]
            } else {
                if packet.data.is_null() {
                    return Err(Failure::new(
                        "audio",
                        "WASAPI returned a null non-silent packet",
                    ));
                }
                let size = (packet.frames as usize)
                    .checked_mul(format.block_align)
                    .filter(|&size| size <= isize::MAX as usize)
                    .ok_or_else(|| Failure::new("audio_format", "WASAPI packet length overflow"))?;
                unsafe { std::slice::from_raw_parts(packet.data, size) }
            };
            self.ring
                .resample(format, data, packet.frames, first as i64, silent);
            packet.release()?;
        }
        Ok(())
    }

    pub fn write_until(&mut self, writer: &Writer, active_seconds: f64) -> Result<()> {
        if self.client.is_none() {
            return Ok(());
        }
        if !active_seconds.is_finite() || active_seconds > i64::MAX as f64 / TICKS_PER_SECOND as f64
        {
            return Err(Failure::new(
                "audio_timestamp",
                "Invalid active recording duration",
            ));
        }
        let target = (active_seconds.max(0.0) * f64::from(AUDIO_RATE)) as u64;
        while self.ring.written_frames < target {
            let count = ((target - self.ring.written_frames) as usize).min(CHUNK_FRAMES);
            self.ring.make_chunk(count);
            let timestamp = (i128::from(self.ring.written_frames) * i128::from(TICKS_PER_SECOND)
                / i128::from(AUDIO_RATE)) as i64;
            let end = (i128::from(self.ring.written_frames + count as u64)
                * i128::from(TICKS_PER_SECOND)
                / i128::from(AUDIO_RATE)) as i64;
            let sample = audio_sample(&self.ring.chunk[..count * 2], timestamp, end - timestamp)?;
            let queued = writer.write_audio(&sample)?;
            self.ring.counters.max_queued_bytes = self.ring.counters.max_queued_bytes.max(queued);
            self.ring.written_frames += count as u64;
        }
        Ok(())
    }

    pub fn finish_log(&self) {
        let c = &self.ring.counters;
        let mode = if self.client.is_some() {
            "system"
        } else {
            "none"
        };
        let capacity_bytes = self.ring.samples.len() * std::mem::size_of::<i16>()
            + self.ring.tags.len() * std::mem::size_of::<u64>()
            + self.ring.chunk.len() * std::mem::size_of::<i16>();
        diagnostic("audio_stopped", &format!(
            "\"source\":\"{}\",\"rate\":{},\"submitted_frames\":{},\"submitted_seconds\":{},\"captured_input_frames\":{},\"packets\":{},\"capture_backed_frames\":{},\"missing_frames_filled_silence\":{},\"nonzero_frames\":{},\"late_frames_discarded\":{},\"overflow_frames_discarded\":{},\"discontinuities\":{},\"pause_tail_frames_discarded\":{},\"timestamp_errors\":{},\"max_audio_writer_queued_bytes\":{},\"ring_capacity_bytes\":{}",
            mode, AUDIO_RATE, self.ring.written_frames, self.ring.written_frames as f64 / f64::from(AUDIO_RATE),
            c.captured_frames, c.packets, c.real_frames, c.silence_frames, c.nonzero_frames,
            c.late_frames, c.overflow_frames, c.discontinuities, c.pause_tail_frames_discarded,
            c.timestamp_errors, c.max_queued_bytes, capacity_bytes));
    }
}

impl Drop for AudioCapture {
    fn drop(&mut self) {
        if self.started {
            if let Some(client) = &self.client {
                unsafe {
                    let _ = client.Stop();
                    let _ = client.Reset();
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn format(tag: u16, bits: u16, channels: u16, rate: u32) -> SampleFormat {
        SampleFormat::new(
            WAVEFORMATEX {
                wFormatTag: tag,
                nChannels: channels,
                nSamplesPerSec: rate,
                nAvgBytesPerSec: rate * u32::from(channels) * u32::from(bits / 8),
                nBlockAlign: channels * (bits / 8),
                wBitsPerSample: bits,
                cbSize: 0,
            },
            GUID::zeroed(),
            0,
        )
        .unwrap()
    }

    #[test]
    fn pcm_integer_formats_keep_sign_and_channel_order() {
        for (bits, data) in [
            (16, vec![0x00, 0x80, 0xff, 0x7f]),
            (24, vec![0x00, 0x00, 0x80, 0xff, 0xff, 0x7f]),
            (32, vec![0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0x7f]),
        ] {
            let fmt = format(WAVE_FORMAT_PCM as u16, bits, 2, AUDIO_RATE);
            assert_eq!(pcm16(fmt.read_value(&data, 0, 0)), i16::MIN);
            assert_eq!(pcm16(fmt.read_value(&data, 0, 1)), i16::MAX);
        }
    }

    #[test]
    fn float_format_rejects_nonfinite_samples_and_clips() {
        let fmt = format(WAVE_FORMAT_IEEE_FLOAT as u16, 32, 1, AUDIO_RATE);
        for value in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(fmt.read_value(&value.to_le_bytes(), 0, 1), 0.0);
        }
        assert_eq!(
            pcm16(fmt.read_value(&2.0_f32.to_le_bytes(), 0, 0)),
            i16::MAX
        );
        assert_eq!(
            pcm16(fmt.read_value(&(-2.0_f32).to_le_bytes(), 0, 0)),
            i16::MIN
        );
    }

    #[test]
    fn mono_24khz_is_interpolated_to_stereo_48khz() {
        let fmt = format(WAVE_FORMAT_PCM as u16, 16, 1, 24_000);
        let data: Vec<u8> = [0_i16, 16_384_i16]
            .into_iter()
            .flat_map(i16::to_le_bytes)
            .collect();
        let mut ring = PcmRing::new(true);
        ring.resample(fmt, &data, 2, 0, false);
        ring.make_chunk(4);
        assert_eq!(
            &ring.chunk[..8],
            &[0, 0, 8192, 8192, 16384, 16384, 16384, 16384]
        );
    }

    #[test]
    fn float_96khz_is_downsampled_and_negative_timestamps_are_skipped() {
        let fmt = format(WAVE_FORMAT_IEEE_FLOAT as u16, 32, 1, 96_000);
        let data: Vec<u8> = [0.25_f32, 0.5, -0.25, -0.5]
            .into_iter()
            .flat_map(f32::to_le_bytes)
            .collect();
        let mut ring = PcmRing::new(true);
        ring.resample(fmt, &data, 4, -1, false);
        ring.make_chunk(2);
        assert_eq!(&ring.chunk[..4], &[-8192, -8192, 0, 0]);
        assert_eq!(ring.counters.real_frames, 1);
        assert_eq!(ring.counters.silence_frames, 1);
    }

    #[test]
    fn ring_is_bounded_and_stale_wraparound_tags_never_reappear() {
        let mut ring = PcmRing::new(true);
        let sizes = (ring.samples.len(), ring.tags.len(), ring.chunk.len());
        ring.put(0, 10, 20);
        ring.put(CAPACITY as u64, 30, 40);
        assert_eq!(ring.counters.overflow_frames, 1);
        ring.make_chunk(1);
        ring.written_frames = CAPACITY as u64;
        ring.put(0, 99, 99);
        ring.make_chunk(1);
        assert_eq!(&ring.chunk[..2], &[0, 0]);
        assert_eq!(ring.counters.late_frames, 1);
        assert_eq!(
            (ring.samples.len(), ring.tags.len(), ring.chunk.len()),
            sizes
        );
        assert_eq!(sizes, (96_000, 48_000, 1920));
    }

    #[test]
    fn resume_discards_pending_tail_without_rewinding_the_timeline() {
        let mut ring = PcmRing::new(true);
        ring.written_frames = 200;
        ring.put(200, 123, 456);
        ring.put(201, 789, 321);
        ring.clear_pending();
        ring.make_chunk(2);
        assert_eq!(&ring.chunk[..4], &[0, 0, 0, 0]);
        assert_eq!(ring.written_frames, 200);
        assert_eq!(ring.counters.pause_tail_frames_discarded, 2);
    }

    #[test]
    fn invalid_audio_formats_fail_instead_of_generating_silent_audio() {
        let base = WAVEFORMATEX {
            wFormatTag: WAVE_FORMAT_PCM as u16,
            nChannels: 2,
            nSamplesPerSec: AUDIO_RATE,
            nBlockAlign: 4,
            wBitsPerSample: 16,
            ..Default::default()
        };
        for bad in [
            WAVEFORMATEX {
                nChannels: 6,
                ..base
            },
            WAVEFORMATEX {
                nSamplesPerSec: 0,
                ..base
            },
            WAVEFORMATEX {
                nBlockAlign: 8,
                ..base
            },
            WAVEFORMATEX {
                wBitsPerSample: 8,
                ..base
            },
            WAVEFORMATEX {
                wFormatTag: 99,
                ..base
            },
        ] {
            assert!(SampleFormat::new(bad, GUID::zeroed(), 0).is_err());
        }
        let extensible = WAVEFORMATEX {
            wFormatTag: WAVE_FORMAT_EXTENSIBLE as u16,
            wBitsPerSample: 32,
            nBlockAlign: 8,
            ..base
        };
        assert_eq!(
            SampleFormat::new(extensible, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 3)
                .unwrap()
                .kind,
            SampleKind::Float32
        );
    }

    #[test]
    fn surround_center_lfe_and_rear_channels_are_kept_in_stereo() {
        for mask in [0x3f_u32, 0x60f, 0x63f, 0xff] {
            let channels = mask.count_ones() as u16;
            let format = SampleFormat::new(WAVEFORMATEX {
                wFormatTag: WAVE_FORMAT_EXTENSIBLE as u16,
                nChannels: channels, nSamplesPerSec: AUDIO_RATE,
                wBitsPerSample: 32, nBlockAlign: channels * 4,
                cbSize: 22, ..Default::default()
            }, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, mask).unwrap();
            // 每一路单独发声：中置/LFE 两边都有，左右环绕不得串到另一边。
            for channel in 0..channels as usize {
                let mut samples = vec![0.0_f32; channels as usize];
                samples[channel] = 0.5;
                let data: Vec<u8> = samples.iter().flat_map(|value| value.to_le_bytes()).collect();
                let stereo = format.read_stereo(&data, 0);
                assert!(stereo[0] > 0.0 || stereo[1] > 0.0);
                match channel {
                    0 | 4 | 6 => assert_eq!(stereo[1], 0.0),
                    1 | 5 | 7 => assert_eq!(stereo[0], 0.0),
                    _ => assert_eq!(stereo[0], stereo[1]),
                }
                let mut ring = PcmRing::new(true);
                ring.resample(format, &data, 1, 0, false);
                ring.make_chunk(1);
                assert_eq!(ring.chunk[0], pcm16(stereo[0]));
                assert_eq!(ring.chunk[1], pcm16(stereo[1]));
            }
            let data: Vec<u8> = vec![1.0_f32; channels as usize].iter()
                .flat_map(|value| value.to_le_bytes()).collect();
            for sample in format.read_stereo(&data, 0) { assert!((sample - 1.0).abs() < 1e-12); }
        }
    }

    #[test]
    fn speaker_mask_controls_channel_order_and_rejects_ambiguous_layouts() {
        let weights = stereo_weights(2, 0x5).unwrap(); // FL + FC，第二路不是右前方
        assert_eq!(weights[0][1], 0.0);
        assert!(weights[1][0] > 0.0 && weights[1][1] > 0.0);
        assert!(stereo_weights(6, 0).is_err());
        assert!(stereo_weights(6, 3).is_err());
        assert!(stereo_weights(2, 0x80000001).is_err());
        assert_eq!(stereo_weights(1, 0).unwrap()[0], [1.0, 1.0]);
        assert_eq!(&stereo_weights(2, 0).unwrap()[..2], &[[1.0, 0.0], [0.0, 1.0]]);
    }

    #[test]
    fn silent_packets_need_no_data_and_disabled_audio_allocates_no_ring() {
        let mut ring = PcmRing::new(true);
        ring.resample(
            format(WAVE_FORMAT_PCM as u16, 16, 2, AUDIO_RATE),
            &[],
            2,
            0,
            true,
        );
        ring.make_chunk(2);
        assert_eq!(&ring.chunk[..4], &[0, 0, 0, 0]);
        assert_eq!(ring.counters.real_frames, 2);
        let disabled = PcmRing::new(false);
        assert!(
            disabled.samples.is_empty() && disabled.tags.is_empty() && disabled.chunk.is_empty()
        );
    }
}
