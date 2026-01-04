use serde::{Deserialize, Serialize};
use std::path::Path;
use windows::{
    core::HSTRING,
    Globalization::Language,
    Graphics::Imaging::BitmapDecoder,
    Media::Ocr::{OcrEngine, OcrResult as WinOcrResult},
    Storage::{FileAccessMode, StorageFile},
};

// Python 绑定模块
mod python;
pub use python::*;

/// OCR 识别的文字行
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OcrLine {
    /// 行文本内容
    pub text: String,
    /// 文字区域的边界框
    pub bounds: BoundingBox,
    /// 该行包含的单词列表
    pub words: Vec<OcrWord>,
}

impl OcrLine {
    /// 计算行中单词之间的水平间距
    /// 返回间距数组，gaps[i] 表示第 i 个单词和第 i+1 个单词之间的距离（像素）
    pub fn compute_word_gaps(&self) -> Vec<f32> {
        let mut gaps = Vec::new();

        for i in 0..self.words.len().saturating_sub(1) {
            let w1 = &self.words[i].bounds;
            let w2 = &self.words[i + 1].bounds;

            let gap = w2.x - (w1.x + w1.width);
            gaps.push(gap.max(0.0)); 
        }

        gaps
    }
    
    /// 获取行中单词的平均间距，单词数少于2个时返回 None
    pub fn average_word_gap(&self) -> Option<f32> {
        let gaps = self.compute_word_gaps();
        if gaps.is_empty() {
            None
        } else {
            Some(gaps.iter().sum::<f32>() / gaps.len() as f32)
        }
    }
    
    /// 获取行中最大的单词间距
    pub fn max_word_gap(&self) -> Option<f32> {
        self.compute_word_gaps().into_iter().max_by(|a, b| a.partial_cmp(b).unwrap())
    }
    
    /// 获取行中最小的单词间距
    pub fn min_word_gap(&self) -> Option<f32> {
        self.compute_word_gaps().into_iter().min_by(|a, b| a.partial_cmp(b).unwrap())
    }
}

/// OCR 识别的单词
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OcrWord {
    /// 单词文本内容
    pub text: String,
    /// 文字区域的边界框
    pub bounds: BoundingBox,
}

/// 文字区域的边界框信息
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BoundingBox {
    /// X 坐标（左上角）
    pub x: f32,
    /// Y 坐标（左上角）
    pub y: f32,
    /// 宽度
    pub width: f32,
    /// 高度
    pub height: f32,
}

/// OCR 识别结果
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OcrRecognitionResult {
    /// 识别到的所有文本行
    pub lines: Vec<OcrLine>,
    /// 识别的完整文本内容
    pub text: String,
    /// 文本角度（旋转角度）
    pub text_angle: Option<f64>,
}

/// 从图片文件执行 OCR 识别
/// 
/// # 参数
/// - `image_path` - 图片文件路径
/// - `language` - 语言代码（如 "zh-Hans-CN", "en-US"），None 使用系统默认语言
pub fn recognize_from_file(image_path: &str, language: Option<&str>) -> Result<OcrRecognitionResult, String> {
    let file_path = Path::new(image_path);
    if !file_path.exists() {
        return Err(format!("文件不存在: {}", image_path));
    }
    
    recognize_internal(image_path, language)
        .map_err(|e| format!("OCR 识别失败: {}", e))
}

/// 从字节数组执行 OCR 识别
/// 
/// # 参数
/// - `image_data` - 图片字节数据（支持 PNG、JPG、BMP 等格式）
/// - `language` - 语言代码（如 "zh-Hans-CN", "en-US"），None 使用系统默认语言
pub fn recognize_from_bytes(image_data: &[u8], language: Option<&str>) -> Result<OcrRecognitionResult, String> {
    recognize_from_bytes_internal(image_data, language)
        .map_err(|e| format!("OCR 识别失败: {}", e))
}

fn recognize_from_bytes_internal(image_data: &[u8], language: Option<&str>) -> windows::core::Result<OcrRecognitionResult> {
    use windows::Storage::Streams::{DataWriter, InMemoryRandomAccessStream};
    
    let stream = InMemoryRandomAccessStream::new()?;
    let writer = DataWriter::CreateDataWriter(&stream)?;
    
    writer.WriteBytes(image_data)?;
    writer.StoreAsync()?.get()?;
    writer.FlushAsync()?.get()?;
    
    stream.Seek(0)?;
    
    let decoder = BitmapDecoder::CreateAsync(&stream)?.get()?;
    let bitmap = decoder.GetSoftwareBitmapAsync()?.get()?;
    
    let engine = if let Some(lang) = language {
        let language_obj = Language::CreateLanguage(&HSTRING::from(lang))?;
        OcrEngine::TryCreateFromLanguage(&language_obj)?
    } else {
        OcrEngine::TryCreateFromUserProfileLanguages()?
    };
    
    let result = engine.RecognizeAsync(&bitmap)?.get()?;
    
    convert_ocr_result(&result)
}

fn recognize_internal(image_path: &str, language: Option<&str>) -> windows::core::Result<OcrRecognitionResult> {
    let file = StorageFile::GetFileFromPathAsync(&HSTRING::from(image_path))?.get()?;
    let stream = file.OpenAsync(FileAccessMode::Read)?.get()?;
    
    let decoder = BitmapDecoder::CreateAsync(&stream)?.get()?;
    let bitmap = decoder.GetSoftwareBitmapAsync()?.get()?;
    
    let engine = if let Some(lang) = language {
        let language_obj = Language::CreateLanguage(&HSTRING::from(lang))?;
        OcrEngine::TryCreateFromLanguage(&language_obj)?
    } else {
        OcrEngine::TryCreateFromUserProfileLanguages()?
    };
    
    let result = engine.RecognizeAsync(&bitmap)?.get()?;
    
    convert_ocr_result(&result)
}

fn convert_ocr_result(win_result: &WinOcrResult) -> windows::core::Result<OcrRecognitionResult> {
    let mut lines = Vec::new();
    let mut full_text = String::new();
    
    let win_lines = win_result.Lines()?;
    let line_count = win_lines.Size()?;

    for i in 0..line_count {
        let win_line = win_lines.GetAt(i)?;

        let mut words = Vec::new();
        let win_words = win_line.Words()?;
        let word_count = win_words.Size()?;

        let mut min_x = f32::MAX;
        let mut min_y = f32::MAX;
        let mut max_x = f32::MIN;
        let mut max_y = f32::MIN;

        for j in 0..word_count {
            let win_word = win_words.GetAt(j)?;
            let word_text = win_word.Text()?.to_string();

            let rect = win_word.BoundingRect()?;

            let word_bounds = BoundingBox {
                x: rect.X,
                y: rect.Y,
                width: rect.Width,
                height: rect.Height,
            };

            min_x = min_x.min(rect.X);
            min_y = min_y.min(rect.Y);
            max_x = max_x.max(rect.X + rect.Width);
            max_y = max_y.max(rect.Y + rect.Height);

            words.push(OcrWord {
                text: word_text,
                bounds: word_bounds,
            });
        }

        let line_text: String = words.iter().map(|w| w.text.as_str()).collect();
        full_text.push_str(&line_text);
        full_text.push('\n');

        let line_bounds = if word_count > 0 {
            BoundingBox {
                x: min_x,
                y: min_y,
                width: max_x - min_x,
                height: max_y - min_y,
            }
        } else {
            BoundingBox {
                x: 0.0,
                y: 0.0,
                width: 0.0,
                height: 0.0,
            }
        };

        lines.push(OcrLine {
            text: line_text,
            bounds: line_bounds,
            words,
        });
    }

    let text_angle = win_result.TextAngle()
        .ok()
        .and_then(|a| a.Value().ok());

    Ok(OcrRecognitionResult {
        lines,
        text: full_text.trim().to_string(),
        text_angle,
    })
}

/// 获取系统支持的 OCR 语言列表
pub fn get_available_languages() -> Result<Vec<String>, String> {
    get_available_languages_internal()
        .map_err(|e| format!("获取可用语言失败: {}", e))
}

fn get_available_languages_internal() -> windows::core::Result<Vec<String>> {
    let languages = OcrEngine::AvailableRecognizerLanguages()?;
    let count = languages.Size()?;
    
    let mut result = Vec::new();
    for i in 0..count {
        let lang = languages.GetAt(i)?;
        let lang_tag = lang.LanguageTag()?.to_string();
        result.push(lang_tag);
    }
    
    Ok(result)
}
