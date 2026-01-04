// 从字节数组识别示例

use windows_media_ocr::recognize_from_bytes;
use std::fs;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    
    if args.len() < 2 {
        eprintln!("用法: cargo run --example recognize_from_bytes <图片路径> [语言代码]");
        eprintln!("示例: cargo run --example recognize_from_bytes image.png");
        eprintln!("示例: cargo run --example recognize_from_bytes image.png en-US");
        return;
    }
    
    let image_path = &args[1];
    let language = if args.len() >= 3 {
        Some(args[2].as_str())
    } else {
        None
    };
    
    // 读取图片到内存
    let image_data = match fs::read(image_path) {
        Ok(data) => data,
        Err(e) => {
            eprintln!("读取文件失败: {}", e);
            return;
        }
    };
    
    println!("图片大小: {} 字节", image_data.len());
    
    // 从字节数组识别
    match recognize_from_bytes(&image_data, language) {
        Ok(result) => {
            // 输出完整文本
            println!("\n识别文本:");
            println!("{}\n", result.text);
            
            // 输出统计信息
            println!("统计信息:");
            println!("  总行数: {}", result.lines.len());
            
            let total_words: usize = result.lines.iter()
                .map(|line| line.words.len())
                .sum();
            println!("  总单词数: {}", total_words);
            
            // 显示前3行的详细信息
            println!("\n详细信息（前3行）:");
            for (i, line) in result.lines.iter().take(3).enumerate() {
                println!("\n行 {}:", i + 1);
                println!("  文本: {}", line.text);
                println!("  位置: x={:.0}, y={:.0}, w={:.0}, h={:.0}", 
                    line.bounds.x, line.bounds.y,
                    line.bounds.width, line.bounds.height);
                
                if let Some(avg_gap) = line.average_word_gap() {
                    println!("  平均间距: {:.1} px", avg_gap);
                }
            }
            
            // 输出JSON示例（前2行）
            if result.lines.len() > 0 {
                println!("\nJSON格式示例（第1行）:");
                if let Ok(json) = serde_json::to_string_pretty(&result.lines[0]) {
                    println!("{}", json);
                }
            }
        }
        Err(e) => {
            eprintln!("错误: {}", e);
        }
    }
}
