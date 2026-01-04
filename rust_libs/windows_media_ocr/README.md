# qcocr

基于 Windows OCR API 的 Rust 图像文字识别库。

## 功能特性

- 使用 Windows 自带的 OCR API (Windows.Media.Ocr)
- 支持多语言识别（中文、英文等）
- 按行和按单词获取识别结果
- 返回文本内容和位置信息（边界框坐标和尺寸）
- 字符间距计算
- 支持从文件路径或字节数组识别
- JSON 序列化支持

## 系统要求

- Windows 10 (1809) 及以上
- 已安装对应语言的 OCR 支持

## 安装

在 `Cargo.toml` 中添加：

```toml
[dependencies]
qcocr = { git = "https://github.com/mosheng1/qcocr" }
```

或从本地路径：

```toml
[dependencies]
qcocr = { path = "path/to/qcocr" }
```

## 使用示例

### 从文件识别

```rust
use qcocr::recognize_from_file;

fn main() {
    match recognize_from_file("image.png", None) {
        Ok(result) => {
            println!("{}", result.text);
        }
        Err(e) => {
            eprintln!("错误: {}", e);
        }
    }
}
```

### 从字节数组识别

```rust
use qcocr::recognize_from_bytes;
use std::fs;

fn main() {
    let image_data = fs::read("image.png").unwrap();
    match recognize_from_bytes(&image_data, None) {
        Ok(result) => println!("{}", result.text),
        Err(e) => eprintln!("错误: {}", e),
    }
}
```

### 指定语言

```rust
use qcocr::recognize_from_file;

fn main() {
    let result = recognize_from_file("image.png", Some("en-US")).unwrap();
    println!("{}", result.text);
}
```

### 获取位置信息

```rust
use qcocr::recognize_from_file;

fn main() {
    let result = recognize_from_file("image.png", None).unwrap();
    
    for line in &result.lines {
        println!("文本: {}", line.text);
        println!("位置: x={:.0}, y={:.0}, w={:.0}, h={:.0}",
            line.bounds.x, line.bounds.y,
            line.bounds.width, line.bounds.height);
    }
}
```

### 查询可用语言

```rust
use qcocr::get_available_languages;

fn main() {
    let languages = get_available_languages().unwrap();
    for lang in languages {
        println!("{}", lang);
    }
}
```

### 计算字符间距

```rust
use qcocr::recognize_from_file;

fn main() {
    let result = recognize_from_file("image.png", None).unwrap();
    
    for line in &result.lines {
        let gaps = line.compute_word_gaps();
        if let Some(avg) = line.average_word_gap() {
            println!("平均间距: {:.1}px", avg);
        }
    }
}
```

## API 文档

### 数据结构

#### `OcrRecognitionResult`
OCR 识别结果

- `lines: Vec<OcrLine>` - 识别到的所有文本行
- `text: String` - 完整的识别文本
- `text_angle: Option<f64>` - 文本旋转角度（如果有）

#### `OcrLine`
文本行

**字段：**
- `text: String` - 行文本内容
- `bounds: BoundingBox` - 边界框
- `words: Vec<OcrWord>` - 该行包含的单词列表

**方法：**
- `compute_word_gaps() -> Vec<f32>` - 计算单词之间的间距数组
- `average_word_gap() -> Option<f32>` - 获取平均单词间距
- `max_word_gap() -> Option<f32>` - 获取最大单词间距
- `min_word_gap() -> Option<f32>` - 获取最小单词间距

#### `OcrWord`
单词

- `text: String` - 单词文本
- `bounds: BoundingBox` - 边界框

#### `BoundingBox`
边界框（位置和大小）

- `x: f32` - X 坐标（左上角）
- `y: f32` - Y 坐标（左上角）
- `width: f32` - 宽度
- `height: f32` - 高度

### 函数

#### `recognize_from_file(image_path: &str, language: Option<&str>) -> Result<OcrRecognitionResult, String>`

从图片文件识别文本。

**参数:**
- `image_path` - 图片文件路径（支持 PNG、JPG、BMP 等格式）
- `language` - 可选的语言代码（如 "zh-Hans-CN"、"en-US"），None 使用系统默认语言

**返回:**
- `Ok(OcrRecognitionResult)` - 识别成功
- `Err(String)` - 识别失败，包含错误信息

#### `recognize_from_bytes(image_data: &[u8], language: Option<&str>) -> Result<OcrRecognitionResult, String>`

从字节数组识别文本。

**参数:**
- `image_data` - 图片文件的字节数据（支持 PNG、JPG、BMP 等格式）
- `language` - 可选的语言代码（如 "zh-Hans-CN"、"en-US"），None 使用系统默认语言

**返回:**
- `Ok(OcrRecognitionResult)` - 识别成功
- `Err(String)` - 识别失败，包含错误信息

**使用场景:**
- 从网络下载的图片
- 从数据库读取的图片
- 内存中的图片数据
- 截图等二进制数据

#### `get_available_languages() -> Result<Vec<String>, String>`

获取系统支持的 OCR 语言列表。

**返回:**
- `Ok(Vec<String>)` - 语言代码列表
- `Err(String)` - 获取失败

## 运行示例

```bash
# 从文件识别
cargo run --example recognize_from_file image.png

# 从字节数组识别
cargo run --example recognize_from_bytes image.png

# 指定语言
cargo run --example recognize_from_file image.png en-US
```

## 语言代码

- `zh-Hans-CN` - 中文简体
- `zh-Hant-TW` - 中文繁体
- `en-US` - 英文
- `ja-JP` - 日语
- `ko-KR` - 韩语