// 从文件路径识别示例
// 演示基本的 OCR 识别功能，包括文本、位置、间距等信息

use windows_media_ocr::recognize_from_file;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    
    if args.len() < 2 {
        eprintln!("用法: cargo run --example recognize_from_file <图片路径> [语言代码]");
        eprintln!("示例: cargo run --example recognize_from_file image.png");
        eprintln!("示例: cargo run --example recognize_from_file image.png en-US");
        return;
    }
    
    let image_path = &args[1];
    let language = if args.len() >= 3 {
        Some(args[2].as_str())
    } else {
        None
    };
    
    // 执行识别
    match recognize_from_file(image_path, language) {
        Ok(result) => {
            // 输出完整文本
            println!("识别文本:");
            println!("{}\n", result.text);
            
            // 输出行级详细信息
            println!("详细信息 (共 {} 行):", result.lines.len());
            for (i, line) in result.lines.iter().enumerate() {
                println!("\n行 {}:", i + 1);
                println!("  文本: {}", line.text);
                println!("  位置: x={:.0}, y={:.0}, w={:.0}, h={:.0}",
                    line.bounds.x, line.bounds.y,
                    line.bounds.width, line.bounds.height);
                println!("  单词数: {}", line.words.len());
                
                // 计算单词间距
                if let Some(avg_gap) = line.average_word_gap() {
                    println!("  平均间距: {:.1} px", avg_gap);
                }
            }
            
            // 输出文本角度
            if let Some(angle) = result.text_angle {
                println!("\n文本角度: {:.2}°", angle);
            }
        }
        Err(e) => {
            eprintln!("错误: {}", e);
        }
    }
}
