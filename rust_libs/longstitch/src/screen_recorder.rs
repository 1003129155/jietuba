/// 屏幕录制模块
/// 
/// 实现基于 wcap 的屏幕录制功能
/// 支持区域录制、视频和 GIF 输出

use image::{ImageBuffer, Rgba, RgbaImage};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use scrap::{Capturer, Display};

/// 录制格式
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum RecordFormat {
    Mp4,  // MP4 视频格式
    Gif,  // GIF 动图格式
}

/// 录制区域
#[derive(Debug, Clone, Copy)]
pub struct RecordRegion {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

/// 录制状态
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum RecordState {
    Idle,       // 空闲
    Recording,  // 录制中
    Stopped,    // 已停止
}

/// 录制配置
#[derive(Debug, Clone)]
pub struct RecordConfig {
    pub format: RecordFormat,
    pub fps: u32,              // 帧率
    pub region: RecordRegion,   // 录制区域
    pub output_path: String,    // 输出路径
}

/// 屏幕录制器
pub struct ScreenRecorder {
    config: Option<RecordConfig>,
    state: Arc<Mutex<RecordState>>,
    frames: Arc<Mutex<Vec<RgbaImage>>>,
    recording_thread: Option<thread::JoinHandle<()>>,
}

impl ScreenRecorder {
    /// 创建新的录制器
    pub fn new() -> Self {
        Self {
            config: None,
            state: Arc::new(Mutex::new(RecordState::Idle)),
            frames: Arc::new(Mutex::new(Vec::new())),
            recording_thread: None,
        }
    }

    /// 设置录制配置
    pub fn set_config(&mut self, config: RecordConfig) {
        self.config = Some(config);
    }

    /// 开始录制
    pub fn start_recording(&mut self) -> Result<(), String> {
        if self.config.is_none() {
            return Err("录制配置未设置".to_string());
        }

        let mut state = self.state.lock().unwrap();
        if *state != RecordState::Idle {
            return Err("录制器已在运行".to_string());
        }
        *state = RecordState::Recording;
        drop(state);

        let config = self.config.as_ref().unwrap().clone();
        let state_clone = Arc::clone(&self.state);
        let frames_clone = Arc::clone(&self.frames);

        // 启动录制线程
        self.recording_thread = Some(thread::spawn(move || {
            Self::recording_loop(config, state_clone, frames_clone);
        }));

        Ok(())
    }

    /// 停止录制
    pub fn stop_recording(&mut self) -> Result<(), String> {
        let mut state = self.state.lock().unwrap();
        if *state != RecordState::Recording {
            return Err("录制器未在运行".to_string());
        }
        *state = RecordState::Stopped;
        drop(state);

        // 等待录制线程结束
        if let Some(handle) = self.recording_thread.take() {
            handle.join().map_err(|_| "录制线程终止失败".to_string())?;
        }

        // 保存录制结果
        self.save_recording()?;

        // 重置状态
        let mut state = self.state.lock().unwrap();
        *state = RecordState::Idle;
        self.frames.lock().unwrap().clear();

        Ok(())
    }

    /// 录制循环
    fn recording_loop(
        config: RecordConfig,
        state: Arc<Mutex<RecordState>>,
        frames: Arc<Mutex<Vec<RgbaImage>>>,
    ) {
        // 获取显示器
        let display = match Display::primary() {
            Ok(d) => d,
            Err(e) => {
                eprintln!("无法获取主显示器: {}", e);
                return;
            }
        };

        // 创建捕获器
        let mut capturer = match Capturer::new(display) {
            Ok(c) => c,
            Err(e) => {
                eprintln!("无法创建捕获器: {}", e);
                return;
            }
        };

        let frame_duration = Duration::from_millis(1000 / config.fps as u64);
        let mut last_frame_time = Instant::now();

        loop {
            // 检查是否需要停止
            {
                let current_state = state.lock().unwrap();
                if *current_state != RecordState::Recording {
                    break;
                }
            }

            // 控制帧率
            let now = Instant::now();
            let elapsed = now.duration_since(last_frame_time);
            if elapsed < frame_duration {
                thread::sleep(frame_duration - elapsed);
                continue;
            }
            last_frame_time = now;

            // 捕获屏幕
            match capturer.frame() {
                Ok(frame) => {
                    let width = capturer.width();
                    let height = capturer.height();

                    // 裁剪到指定区域
                    if let Some(cropped) = Self::crop_frame(
                        frame,
                        width,
                        height,
                        &config.region,
                    ) {
                        frames.lock().unwrap().push(cropped);
                    }
                }
                Err(e) => {
                    // 某些情况下捕获可能暂时失败，继续尝试
                    if e.to_string().contains("WouldBlock") {
                        thread::sleep(Duration::from_millis(10));
                    } else {
                        eprintln!("捕获失败: {}", e);
                    }
                }
            }
        }
    }

    /// 裁剪帧到指定区域
    fn crop_frame(
        frame: &scrap::Frame,
        width: usize,
        height: usize,
        region: &RecordRegion,
    ) -> Option<RgbaImage> {
        // 验证区域有效性
        if region.x < 0 || region.y < 0 {
            return None;
        }
        if region.x as usize + region.width as usize > width {
            return None;
        }
        if region.y as usize + region.height as usize > height {
            return None;
        }

        // 创建裁剪后的图像
        let mut cropped = RgbaImage::new(region.width, region.height);

        for y in 0..region.height {
            for x in 0..region.width {
                let src_x = region.x as usize + x as usize;
                let src_y = region.y as usize + y as usize;
                let src_index = (src_y * width + src_x) * 4;

                if src_index + 3 < frame.len() {
                    let pixel = Rgba([
                        frame[src_index + 2], // B -> R
                        frame[src_index + 1], // G
                        frame[src_index],     // R -> B
                        frame[src_index + 3], // A
                    ]);
                    cropped.put_pixel(x, y, pixel);
                }
            }
        }

        Some(cropped)
    }

    /// 保存录制结果
    fn save_recording(&self) -> Result<(), String> {
        let config = self.config.as_ref().ok_or("配置未设置")?;
        let frames = self.frames.lock().unwrap();

        if frames.is_empty() {
            return Err("没有录制任何帧".to_string());
        }

        match config.format {
            RecordFormat::Gif => self.save_as_gif(&frames, &config.output_path)?,
            RecordFormat::Mp4 => self.save_as_mp4(&frames, &config.output_path, config.fps)?,
        }

        Ok(())
    }

    /// 保存为 GIF
    fn save_as_gif(&self, frames: &[RgbaImage], output_path: &str) -> Result<(), String> {
        use std::fs::File;
        use gif::{Encoder, Frame, Repeat};

        let file = File::create(output_path)
            .map_err(|e| format!("无法创建文件: {}", e))?;

        if frames.is_empty() {
            return Err("没有帧可以保存".to_string());
        }

        let width = frames[0].width() as u16;
        let height = frames[0].height() as u16;

        let mut encoder = Encoder::new(file, width, height, &[])
            .map_err(|e| format!("无法创建 GIF 编码器: {}", e))?;

        encoder.set_repeat(Repeat::Infinite)
            .map_err(|e| format!("无法设置循环: {}", e))?;

        for frame in frames {
            // 转换为索引颜色
            let mut rgba_data = Vec::new();
            for pixel in frame.pixels() {
                rgba_data.extend_from_slice(&pixel.0);
            }

            // 简单的调色板量化（实际应用中应该使用更好的算法）
            let palette = Self::generate_palette(&rgba_data);
            let indexed = Self::quantize_image(&rgba_data, &palette);

            let mut gif_frame = Frame::from_indexed_pixels(
                width,
                height,
                &indexed,
                Some(&palette[..]),
            );

            // 设置帧延迟（10ms 单位）
            let config = self.config.as_ref().unwrap();
            gif_frame.delay = (100 / config.fps) as u16; // 100 = 1秒

            encoder.write_frame(&gif_frame)
                .map_err(|e| format!("无法写入帧: {}", e))?;
        }

        Ok(())
    }

    /// 生成简单的调色板
    fn generate_palette(rgba_data: &[u8]) -> Vec<u8> {
        // 简化版：使用216色网络安全色板
        let mut palette = Vec::with_capacity(256 * 3);
        
        // 216色立方体
        for r in 0..6 {
            for g in 0..6 {
                for b in 0..6 {
                    palette.push((r * 51) as u8);
                    palette.push((g * 51) as u8);
                    palette.push((b * 51) as u8);
                }
            }
        }
        
        // 填充剩余颜色为灰度
        for i in 216..256 {
            let gray = ((i - 216) * 255 / 39) as u8;
            palette.push(gray);
            palette.push(gray);
            palette.push(gray);
        }
        
        palette
    }

    /// 量化图像到调色板
    fn quantize_image(rgba_data: &[u8], palette: &[u8]) -> Vec<u8> {
        let pixel_count = rgba_data.len() / 4;
        let mut indexed = Vec::with_capacity(pixel_count);

        for i in 0..pixel_count {
            let r = rgba_data[i * 4];
            let g = rgba_data[i * 4 + 1];
            let b = rgba_data[i * 4 + 2];

            // 找到最接近的颜色
            let mut min_dist = u32::MAX;
            let mut best_index = 0;

            for (j, chunk) in palette.chunks(3).enumerate() {
                let pr = chunk[0] as i32;
                let pg = chunk[1] as i32;
                let pb = chunk[2] as i32;

                let dist = ((r as i32 - pr).pow(2) + 
                           (g as i32 - pg).pow(2) + 
                           (b as i32 - pb).pow(2)) as u32;

                if dist < min_dist {
                    min_dist = dist;
                    best_index = j;
                }
            }

            indexed.push(best_index as u8);
        }

        indexed
    }

    /// 保存为 MP4（简化版，实际需要更复杂的编码）
    fn save_as_mp4(&self, frames: &[RgbaImage], output_path: &str, fps: u32) -> Result<(), String> {
        // 注意：这是一个占位实现
        // 实际的 MP4 编码需要使用 ffmpeg 或 Media Foundation
        // 这里我们暂时将帧保存为图像序列
        
        use std::path::Path;
        let output_dir = Path::new(output_path).parent()
            .ok_or("无效的输出路径")?;
        
        let base_name = Path::new(output_path)
            .file_stem()
            .and_then(|s| s.to_str())
            .ok_or("无效的文件名")?;

        // 保存为图像序列
        for (i, frame) in frames.iter().enumerate() {
            let frame_path = output_dir.join(format!("{}_{:06}.png", base_name, i));
            frame.save(&frame_path)
                .map_err(|e| format!("无法保存帧 {}: {}", i, e))?;
        }

        // TODO: 使用 ffmpeg 或其他工具将图像序列编码为 MP4
        println!("注意：MP4 编码尚未完全实现，帧已保存为图像序列");
        println!("帧数: {}, FPS: {}", frames.len(), fps);

        Ok(())
    }

    /// 获取当前状态
    pub fn get_state(&self) -> RecordState {
        *self.state.lock().unwrap()
    }

    /// 获取已录制的帧数
    pub fn get_frame_count(&self) -> usize {
        self.frames.lock().unwrap().len()
    }
}

impl Default for ScreenRecorder {
    fn default() -> Self {
        Self::new()
    }
}
