//! 文本子类型：存在 clipboard.sub_kind 里的字符串，取值是库的存储格式，不能改。
//!
//! 只认整条内容就是一个链接、颜色或路径的文本，宁漏不误：一段话里夹着链接仍是普通文本。
//! 判定不访问文件系统，监听线程上查网络路径是否存在可能卡住几秒。

/// 判过、是普通文本；图片和文件也写这个。NULL 才表示还没判
pub const PLAIN: &str = "";
pub const URL: &str = "url";
pub const COLOR: &str = "color";
pub const PATH: &str = "path";

/// 本程序取色时随文字一起写进剪贴板的格式。取色的输出格式可自定义（如 `255, 0, 0`），
/// 不能靠文字认出来，带这个格式的文本直接记成颜色
pub const COLOR_MARKER_FORMAT: &str = "Jietuba.Color";

/// 判定规则的版本，存在 PRAGMA user_version；规则改了就加一，打开库时重判全部文本
pub const RULES_VERSION: i64 = 1;

/// 超过这个长度的文本不可能是链接、颜色或单个路径，不再逐字检查
const MAX_LEN: usize = 4096;

pub fn detect(text: &str, color_marked: bool) -> &'static str {
    if color_marked {
        return COLOR;
    }
    let value = text.trim();
    if value.is_empty() || value.len() > MAX_LEN {
        return PLAIN;
    }
    if is_url(value) {
        URL
    } else if is_color_literal(value) {
        COLOR
    } else if is_path_list(value) {
        PATH
    } else {
        PLAIN
    }
}

fn is_url(value: &str) -> bool {
    if value.chars().any(char::is_whitespace) {
        return false;
    }
    let lower = value.to_ascii_lowercase();
    for scheme in ["http://", "https://", "ftp://", "file://"] {
        if let Some(rest) = lower.strip_prefix(scheme) {
            return !rest.is_empty();
        }
    }
    // www.example.com：www. 后面还要有一段带点的域名
    lower.strip_prefix("www.").is_some_and(|rest| {
        rest.split_once('.').is_some_and(|(host, tail)| !host.is_empty() && !tail.is_empty())
    })
}

/// 不带标记时只认 CSS 写法：# 开头的十六进制和 rgb()/hsl() 系列
fn is_color_literal(value: &str) -> bool {
    match value.strip_prefix('#') {
        Some(hex) => {
            // #123、#1024 多半是编号，3/4 位的要带 a-f 才算颜色
            let short = hex.len() <= 4;
            parse_hex(hex).is_some() && (!short || hex.bytes().any(|b| b.is_ascii_alphabetic()))
        }
        None => parse_color_function(value).is_some(),
    }
}

/// 整条是一个或多个绝对路径，每行一个，允许两端带双引号（资源管理器的「复制文件地址」）
fn is_path_list(value: &str) -> bool {
    let mut lines = value.lines().map(str::trim).filter(|line| !line.is_empty()).peekable();
    lines.peek().is_some() && lines.all(is_absolute_path)
}

/// 路径文本里的各个路径，去掉两端引号；不是路径文本时为空
pub fn paths(text: &str) -> Vec<String> {
    let value = text.trim();
    if value.len() > MAX_LEN || !is_path_list(value) {
        return Vec::new();
    }
    value.lines().map(str::trim).filter(|line| !line.is_empty()).filter_map(unquoted).map(str::to_string).collect()
}

/// 两端都有引号时去掉；只有一边有引号的不是路径
fn unquoted(line: &str) -> Option<&str> {
    match line.strip_prefix('"') {
        Some(inner) => inner.strip_suffix('"'),
        None => Some(line),
    }
}

fn is_absolute_path(line: &str) -> bool {
    let Some(path) = unquoted(line) else {
        return false;
    };
    if path.chars().any(|c| c.is_control() || matches!(c, '"' | '<' | '>' | '|' | '?' | '*')) {
        return false;
    }
    let bytes = path.as_bytes();
    let is_drive = bytes.len() >= 3
        && bytes[0].is_ascii_alphabetic()
        && bytes[1] == b':'
        && matches!(bytes[2], b'\\' | b'/')
        && !path[2..].contains(':');
    if is_drive {
        return true;
    }
    // \\server\share 起头的网络路径
    path.strip_prefix("\\\\").is_some_and(|rest| {
        !rest.contains(':')
            && rest.split_once('\\').is_some_and(|(host, tail)| {
                let share = tail.split('\\').next().unwrap_or("");
                !host.is_empty() && !share.is_empty()
            })
    })
}

/// 颜色文本解析成 RGBA。比判定宽：还认不带 # 的十六进制和 `r, g, b` 三个数，
/// 这两种只在带取色标记时才记成颜色，单凭文字太容易误判
pub fn parse_color(text: &str) -> Option<[u8; 4]> {
    let value = text.trim();
    if let Some(hex) = value.strip_prefix('#') {
        return parse_hex(hex);
    }
    parse_color_function(value)
        .or_else(|| parse_hex(value))
        .or_else(|| parse_rgb_args(value, false))
}

fn parse_hex(hex: &str) -> Option<[u8; 4]> {
    if !matches!(hex.len(), 3 | 4 | 6 | 8) || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let digits: Vec<u8> = hex.bytes().map(|b| (b as char).to_digit(16).unwrap_or(0) as u8).collect();
    let channels: Vec<u8> = if digits.len() <= 4 {
        digits.iter().map(|d| d * 17).collect()
    } else {
        digits.chunks(2).map(|pair| pair[0] * 16 + pair[1]).collect()
    };
    Some([channels[0], channels[1], channels[2], channels.get(3).copied().unwrap_or(255)])
}

/// rgb()/rgba()/hsl()/hsla()，参数用逗号或空格分隔，透明度可写在 / 后面
fn parse_color_function(value: &str) -> Option<[u8; 4]> {
    let open = value.find('(')?;
    let args = value[open + 1..].strip_suffix(')')?;
    match value[..open].trim().to_ascii_lowercase().as_str() {
        "rgb" | "rgba" => parse_rgb_args(args, true),
        "hsl" | "hsla" => parse_hsl_args(args),
        _ => None,
    }
}

/// 拆出三个分量和可选的透明度；`allow_alpha` 为假时只收三个数
fn split_args(args: &str, allow_alpha: bool) -> Option<(Vec<&str>, Option<&str>)> {
    let (main, slash_alpha) = match args.split_once('/') {
        Some((main, alpha)) => (main, Some(alpha.trim())),
        None => (args, None),
    };
    let parts: Vec<&str> = main.split(|c: char| c == ',' || c.is_whitespace()).filter(|part| !part.is_empty()).collect();
    match (parts.len(), slash_alpha) {
        (3, alpha) if alpha.is_none() || allow_alpha => Some((parts, alpha)),
        (4, None) if allow_alpha => Some((parts[..3].to_vec(), Some(parts[3]))),
        _ => None,
    }
}

fn parse_rgb_args(args: &str, allow_alpha: bool) -> Option<[u8; 4]> {
    let (parts, alpha) = split_args(args, allow_alpha)?;
    let mut rgb = [0u8; 3];
    for (slot, part) in rgb.iter_mut().zip(&parts) {
        *slot = match part.strip_suffix('%') {
            Some(percent) => unit_to_byte(bounded(percent, 100.0)? / 100.0),
            None => {
                let number = bounded(part, 255.0)?;
                number.round() as u8
            }
        };
    }
    Some([rgb[0], rgb[1], rgb[2], parse_alpha(alpha)?])
}

fn parse_hsl_args(args: &str) -> Option<[u8; 4]> {
    let (parts, alpha) = split_args(args, true)?;
    let hue: f64 = parts[0].strip_suffix("deg").unwrap_or(parts[0]).parse().ok()?;
    if !hue.is_finite() {
        return None;
    }
    let saturation = bounded(parts[1].strip_suffix('%').unwrap_or(parts[1]), 100.0)? / 100.0;
    let lightness = bounded(parts[2].strip_suffix('%').unwrap_or(parts[2]), 100.0)? / 100.0;
    let [r, g, b] = hsl_to_rgb(hue.rem_euclid(360.0), saturation, lightness);
    Some([unit_to_byte(r), unit_to_byte(g), unit_to_byte(b), parse_alpha(alpha)?])
}

fn parse_alpha(alpha: Option<&str>) -> Option<u8> {
    match alpha {
        None => Some(255),
        Some(text) => match text.strip_suffix('%') {
            Some(percent) => Some(unit_to_byte(bounded(percent, 100.0)? / 100.0)),
            None => Some(unit_to_byte(bounded(text, 1.0)?)),
        },
    }
}

/// 0..=max 之间的数，超出范围的不当颜色
fn bounded(text: &str, max: f64) -> Option<f64> {
    let number: f64 = text.trim().parse().ok()?;
    (0.0..=max).contains(&number).then_some(number)
}

fn unit_to_byte(unit: f64) -> u8 {
    (unit * 255.0).round() as u8
}

fn hsl_to_rgb(hue: f64, saturation: f64, lightness: f64) -> [f64; 3] {
    let chroma = (1.0 - (2.0 * lightness - 1.0).abs()) * saturation;
    let sector = hue / 60.0;
    let x = chroma * (1.0 - (sector % 2.0 - 1.0).abs());
    let (r, g, b) = match sector as u32 {
        0 => (chroma, x, 0.0),
        1 => (x, chroma, 0.0),
        2 => (0.0, chroma, x),
        3 => (0.0, x, chroma),
        4 => (x, 0.0, chroma),
        _ => (chroma, 0.0, x),
    };
    let m = lightness - chroma / 2.0;
    [r + m, g + m, b + m]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn whole_links_are_urls() {
        for text in ["https://example.com", "  http://a.b/c?d=1  ", "FTP://host/file", "file:///C:/a.txt", "www.example.com"] {
            assert_eq!(detect(text, false), URL, "{text}");
        }
        for text in ["see https://example.com", "https://", "www.example", "https://a.com\nhttps://b.com"] {
            assert_ne!(detect(text, false), URL, "{text}");
        }
    }

    #[test]
    fn css_colors_are_colors_but_numbered_tags_are_not() {
        for text in ["#fff", "#FFFA", "#1a2b3c", "#11223344", "#123456", "rgb(255, 0, 0)", "rgba(0 0 0 / 50%)", "hsl(120, 50%, 50%)", "HSLA(120deg 50% 50% / 0.5)"] {
            assert_eq!(detect(text, false), COLOR, "{text}");
        }
        for text in ["#123", "#1024", "#12345", "#ggg", "rgb(256, 0, 0)", "rgb(1, 2)", "hsl(0, 120%, 50%)", "255, 0, 0", "FF0000"] {
            assert_eq!(detect(text, false), PLAIN, "{text}");
        }
    }

    #[test]
    fn marked_text_is_a_color_whatever_it_says() {
        assert_eq!(detect("255, 0, 0", true), COLOR);
        assert_eq!(detect("FF0000", true), COLOR);
        assert_eq!(detect("自定义 颜色", true), COLOR);
    }

    #[test]
    fn absolute_paths_one_per_line_are_paths() {
        for text in [
            r"C:\Users\a\b.txt",
            r#""D:\My Files\x.png""#,
            "C:/work/repo",
            r"\\server\share\dir",
            "\"C:\\a.txt\"\r\n\"C:\\b.txt\"\r\n",
        ] {
            assert_eq!(detect(text, false), PATH, "{text}");
        }
        for text in [
            r"Users\a",
            r#""C:\a.txt"#,
            r"C:\a?.txt",
            r"\\server",
            r"\\server\",
            "C:\\a.txt\nnot a path",
            r"C:\a:b",
        ] {
            assert_ne!(detect(text, false), PATH, "{text}");
        }
    }

    #[test]
    fn path_text_splits_into_unquoted_paths() {
        assert_eq!(paths("\"C:\\a.txt\"\r\n  D:\\b c\\  \n"), vec![r"C:\a.txt", r"D:\b c\"]);
        assert!(paths("C:\\a.txt\nnot a path").is_empty());
        assert!(paths("https://a.com").is_empty());
    }

    #[test]
    fn links_win_over_paths_and_long_text_is_plain() {
        assert_eq!(detect("file://C:/a", false), URL);
        assert_eq!(detect(&format!("C:\\{}", "a".repeat(MAX_LEN)), false), PLAIN);
        assert_eq!(detect("   ", false), PLAIN);
    }

    #[test]
    fn colors_parse_to_rgba() {
        assert_eq!(parse_color("#f0a"), Some([255, 0, 170, 255]));
        assert_eq!(parse_color("#11223380"), Some([0x11, 0x22, 0x33, 0x80]));
        assert_eq!(parse_color("FF8000"), Some([255, 128, 0, 255]));
        assert_eq!(parse_color("12, 34, 56"), Some([12, 34, 56, 255]));
        assert_eq!(parse_color("rgb(100%, 0%, 50%)"), Some([255, 0, 128, 255]));
        assert_eq!(parse_color("rgba(1, 2, 3, 0.5)"), Some([1, 2, 3, 128]));
        assert_eq!(parse_color("hsl(0, 100%, 50%)"), Some([255, 0, 0, 255]));
        assert_eq!(parse_color("hsl(240, 100%, 25%)"), Some([0, 0, 128, 255]));
        assert_eq!(parse_color("hsl(360, 0%, 100%)"), Some([255, 255, 255, 255]));
    }

    #[test]
    fn unparsable_colors_are_none() {
        for text in ["", "red", "12, 34", "12, 34, 56, 0.5", "rgb(a, b, c)", "rgb(1, 2, 3", "hsl(nan, 1%, 1%)", "#12345", "rgba(1, 2, 3, 2)"] {
            assert_eq!(parse_color(text), None, "{text}");
        }
    }
}
